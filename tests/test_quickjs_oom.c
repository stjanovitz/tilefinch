#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

/* Match the browser's instrumented-host C-stack guard. ASan inflates native
   parser frames; this does not change any heap or virtual-string ceiling. */
static size_t test_stack_limit(void)
{
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    return 4u * MIB;
#endif
#elif defined(__SANITIZE_ADDRESS__)
    return 4u * MIB;
#endif
    return 256u * 1024u;
}

typedef struct {
    unsigned polls;
    unsigned stop_after;
} RepeatEvalInterrupt;

static int interrupt_repeat_eval(JSRuntime *runtime, void *opaque)
{
    (void) runtime;
    RepeatEvalInterrupt *interrupt = opaque;
    interrupt->polls++;
    return interrupt->polls >= interrupt->stop_after;
}

static int run_reallocation_peak_census(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    const JSMallocFunctions *allocator = budget_quickjs_pool_allocator();
    JSMallocState state = {
        .malloc_limit = MIB,
        .opaque = pool
    };
    void *allocation = allocator->js_malloc(&state, 1024u);
    if (allocation == NULL) return 1;
    allocation = allocator->js_realloc(&state, allocation, 256u * 1024u);
    int okay = allocation != NULL
        && budget_quickjs_pool_js_malloc_current(pool) == state.malloc_size
        && budget_quickjs_pool_js_malloc_peak(pool) == state.malloc_size;
    allocator->js_free(&state, allocation);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0)
        return 1;
    return okay ? 0 : 1;
}

static int run_shared_function_source(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMaxStackSize(rt, test_stack_limit());
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    const char prefix[] = "function outer(){function middle(){function inner(){/*";
    const char suffix[] = "*/return 42}return inner}return middle}"
        "globalThis.saved=outer()();outer=null;";
    size_t length = sizeof(prefix) - 1 + 65536 + sizeof(suffix) - 1;
    char *source = malloc(length + 1);
    if (!source) return 1;
    memcpy(source, prefix, sizeof(prefix) - 1);
    memset(source + sizeof(prefix) - 1, 'x', 65536);
    memcpy(source + sizeof(prefix) - 1 + 65536, suffix, sizeof(suffix));
    JSMemoryUsage before, after;
    JS_ComputeMemoryUsage(rt, &before);
    JSValue compiled = JS_Eval(ctx, source, length, "<shared-source>",
                              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_ComputeMemoryUsage(rt, &after);
    int okay = !JS_IsException(compiled)
        && after.malloc_size - before.malloc_size < (int64_t)(length * 2)
        && before.malloc_arena_used <= before.malloc_arena_capacity
        && after.malloc_arena_used <= after.malloc_arena_capacity
        && after.malloc_arena_capacity <= after.malloc_size;
    fprintf(stderr, "shared source: input=%zu retained-delta=%lld\n", length,
            (long long)(after.malloc_size - before.malloc_size));
    /* Cached bytecode must preserve sharing, not serialize each nested copy.
       The retained child and its toString() must outlive the parent/read buffer. */
    size_t byte_count = 0;
    uint8_t *bytes = JS_IsException(compiled) ? NULL
        : JS_WriteObject(ctx, &byte_count, compiled, JS_WRITE_OBJ_BYTECODE);
    JSValue result = JS_IsException(compiled) ? JS_UNDEFINED
        : JS_EvalFunction(ctx, compiled);
    okay = okay && !JS_IsException(result) && bytes != NULL;
    JS_FreeValue(ctx, result);
    JS_RunGC(rt);
    const char check[] = "saved()===42&&saved.toString()==="
        "'function inner(){/*'+'x'.repeat(65536)+'*/return 42}'";
    result = JS_Eval(ctx, check, sizeof(check)-1, "<source-lifetime>", 0);
    okay = okay && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    const char kinds[] =
        "(()=>{function factory(){const a=x=>x+1;"
        "class C {m(){return 7}} async function f(){return 8}"
        "function* g(){yield 9}return [a,C,f,g]}"
        "const v=factory();factory=null;"
        "return v[0].toString()==='x=>x+1'&&"
        "v[1].toString()==='class C {m(){return 7}}'&&"
        "v[1].prototype.m.toString()==='m(){return 7}'&&"
        "v[2].toString()==='async function f(){return 8}'&&"
        "v[3].toString()==='function* g(){yield 9}'})()";
    result = JS_Eval(ctx, kinds, sizeof(kinds)-1, "<source-kinds>", 0);
    okay = okay && !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    const char unicode_source[] =
        "(()=>{for(const text of ['\\u00e9','\\u20ac','\\ud83d\\ude80',"
        "'a\\u00e9b\\u20acc\\ud83d\\ude80']){"
        "const source='function exact(){/*'+text+'*/return 42}';"
        "const f=eval('('+source+')');"
        "if(f.toString()!==source||f()!==42)return false;}return true})()";
    result = JS_Eval(ctx, unicode_source, sizeof(unicode_source)-1,
                     "<unicode-function-source>", 0);
    okay = okay && !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
    JS_FreeValue(ctx, result);
    const char *malformed[] = {
        "function bad(){/*\x80*/}", "function bad(){/*\xe2\x82*/}",
        "function bad(){/*\xf5\x80\x80\x80*/}",
    };
    for (size_t i = 0; i < sizeof(malformed)/sizeof(malformed[0]); ++i) {
        result = JS_Eval(ctx, malformed[i], strlen(malformed[i]),
                         "<source-comment-bytes>", 0);
        okay = okay && !JS_IsException(result);
        JS_FreeValue(ctx, result);
        result = JS_Eval(ctx, "bad.toString()", 14, "<source-replacement>", 0);
        JSValue expected = JS_NewStringLen(ctx, malformed[i], strlen(malformed[i]));
        const char *actual_text = JS_ToCString(ctx, result);
        const char *expected_text = JS_ToCString(ctx, expected);
        okay = okay && actual_text && expected_text
            && strcmp(actual_text, expected_text) == 0;
        JS_FreeCString(ctx, actual_text);
        JS_FreeCString(ctx, expected_text);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, expected);
    }
    if (bytes) {
        clock_t read_started = clock();
        for (unsigned iteration = 0; iteration < 128; ++iteration) {
            JSValue cached = JS_ReadObject(ctx, bytes, byte_count,
                                           JS_READ_OBJ_BYTECODE);
            okay = okay && !JS_IsException(cached);
            JS_FreeValue(ctx, cached);
        }
        fprintf(stderr, "cached shared source: 128 reads cpu-us=%.0f\n",
                (double)(clock() - read_started) * 1000000.0 / CLOCKS_PER_SEC);
        JSValue roundtrip = JS_ReadObject(ctx, bytes, byte_count,
                                          JS_READ_OBJ_BYTECODE);
        size_t rewritten_length = 0;
        uint8_t *rewritten = JS_IsException(roundtrip) ? NULL
            : JS_WriteObject(ctx, &rewritten_length, roundtrip, JS_WRITE_OBJ_BYTECODE);
        okay = okay && rewritten && rewritten_length == byte_count
            && memcmp(rewritten, bytes, byte_count) == 0;
        js_free(ctx, rewritten);
        JS_FreeValue(ctx, roundtrip);
        /* Old serialized caches are declined, not interpreted with new spans. */
        uint8_t version = bytes[0];
        bytes[0] = (uint8_t)(version - 1);
        roundtrip = JS_ReadObject(ctx, bytes, byte_count, JS_READ_OBJ_BYTECODE);
        okay = okay && JS_IsException(roundtrip);
        JS_FreeValue(ctx, roundtrip);
        JS_FreeValue(ctx, JS_GetException(ctx));
        bytes[0] = version;
        static const size_t headroom[] = {0, 32768, 65536, 69632, 73728};
        unsigned refused_reads = 0;
        for (size_t limit = 0; limit < sizeof(headroom)/sizeof(headroom[0]); ++limit) {
            JS_ComputeMemoryUsage(rt, &before);
            JS_SetMemoryLimit(rt, (size_t)before.malloc_size + headroom[limit]);
            roundtrip = JS_ReadObject(ctx, bytes, byte_count, JS_READ_OBJ_BYTECODE);
            if (JS_IsException(roundtrip)) {
                ++refused_reads;
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, roundtrip);
            JS_SetMemoryLimit(rt, 4u * MIB);
        }
        okay = okay && refused_reads != 0;
        /* Truncation after parent adoption must unwind all child-owner refs.
           The next complete read still succeeds in this same runtime. */
        for (size_t missing = 1; missing <= 24 && missing < byte_count; ++missing) {
            JSValue partial = JS_ReadObject(ctx, bytes, byte_count - missing,
                                            JS_READ_OBJ_BYTECODE);
            okay = okay && JS_IsException(partial);
            JS_FreeValue(ctx, partial);
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_ComputeMemoryUsage(rt, &before);
        compiled = JS_ReadObject(ctx, bytes, byte_count, JS_READ_OBJ_BYTECODE);
        JS_ComputeMemoryUsage(rt, &after);
        fprintf(stderr, "cached shared source: input=%zu serialized=%zu retained-delta=%lld\n",
                length, byte_count, (long long)(after.malloc_size - before.malloc_size));
        okay = okay && byte_count < length * 2
            && after.malloc_size - before.malloc_size < (int64_t)(length * 2);
        js_free(ctx, bytes);
        result = JS_IsException(compiled) ? compiled : JS_EvalFunction(ctx, compiled);
        okay = okay && !JS_IsException(result);
        JS_FreeValue(ctx, result);
        result = JS_Eval(ctx, check, sizeof(check)-1, "<source-roundtrip>", 0);
        okay = okay && JS_ToBool(ctx, result) == 1;
        JS_FreeValue(ctx, result);
    }
    /* Siblings must restore the enclosing source scope; Unicode keeps exact
       UTF-8 byte offsets rather than decoded character indexes. */
    static const char sibling_source[] =
        "(()=>{function parent(){function a(){/*\xe2\x82\xac*/return 1}"
        "function b(){function c(){/*\xf0\x9f\x9a\x80*/return 2}return c}"
        "return [a,b()]}const pair=parent();parent=null;"
        "const a=pair[0].toString(),c=pair[1].toString();"
        "return pair[0]()===1&&pair[1]()===2&&"
        "a==='function a(){/*\xe2\x82\xac*/return 1}'&&"
        "c==='function c(){/*\xf0\x9f\x9a\x80*/return 2}'})()";
    compiled = JS_Eval(ctx, sibling_source, sizeof(sibling_source) - 1,
        "<cached-source-siblings>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    bytes = JS_IsException(compiled) ? NULL
        : JS_WriteObject(ctx, &byte_count, compiled, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, compiled);
    okay = okay && bytes != NULL;
    if (bytes) {
        compiled = JS_ReadObject(ctx, bytes, byte_count, JS_READ_OBJ_BYTECODE);
        js_free(ctx, bytes);
        result = JS_IsException(compiled) ? compiled : JS_EvalFunction(ctx, compiled);
        okay = okay && !JS_IsException(result) && JS_ToBool(ctx, result) == 1;
        JS_FreeValue(ctx, result);
    }
    free(source);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    return okay ? 0 : 1;
}

static int run_automatic_gc_near_limit(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMemoryLimit(rt, 512u * 1024u);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    /* A live graph above two thirds of the hard limit used to make the
       automatic post-GC threshold exceed that limit. One long callback then
       failed on collectible cycles before the embedder's next safe point. */
    const char source[] = "globalThis.retained=new Uint8Array(350000);"
        "(()=>{for(let i=0;i<10000;i++){const c={value:i};c.self=c;}return 42})()";
    JSValue result = JS_Eval(ctx, source, sizeof(source)-1, "<gc-headroom>", 0);
    int32_t number = 0;
    int okay = !JS_IsException(result) && !JS_ToInt32(ctx, &number, result)
        && number == 42;
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        fprintf(stderr, "automatic GC headroom: %s\n", message ? message : "exception");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
    }
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    return okay ? 0 : 1;
}

static int run_dense_join_pressure(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    JSContext *ctx = rt ? JS_NewContext(rt) : NULL;
    if (!ctx) return 1;
    char *text = malloc(90000);
    if (!text) return 1;
    memset(text, 'x', 90000);
    JSValue parts = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, parts, 0, JS_NewString(ctx, "prefix"));
    JS_SetPropertyUint32(ctx, parts, 1, JS_NewStringLen(ctx, text, 90000));
    JS_SetPropertyUint32(ctx, parts, 2, JS_NewString(ctx, "suffix"));
    free(text);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "parts", parts);
    JS_FreeValue(ctx, global);
    const char contracts[] =
        "(()=>{const big=parts[1];"
        "if([big,'\\u20ac'].join('|')!==big+'|\\u20ac')return false;"
        "let order='';const a=[big,'tail'];"
        "Object.defineProperty(a,1,{get(){order+='g';return 'tail'}});"
        "const sep={toString(){order+='s';return '|'}};"
        "if(a.join(sep)!==big+'|tail'||order!=='sg')return false;"
        "const b=[big,'old'];const changed=b.join({toString(){b[1]='new';return ''}});"
        "if(changed!==big+'new')return false;"
        "const many=Array(65).fill(big.slice(0,512));"
        "many[32]='\\u20ac'+many[32].slice(1);"
        "const joined=many.join('|');"
        "if(joined.length!==33344||joined[16416]!=='\\u20ac')return false;"
        "let rope=big+'\\ud83d\\ude00';let source=['head',rope,'tail'];"
        "globalThis.joinRetained=source.join('');source[1]='changed';"
        "rope=null;source=null;"
        "return b[1]==='new'&&Array(4097).fill('').join('')===''})()";
    JSValue check = JS_Eval(ctx, contracts, sizeof(contracts)-1,
                            "<join-contracts>", 0);
    bool contract_ok = !JS_IsException(check) && JS_ToBool(ctx, check) == 1;
    JS_FreeValue(ctx, check);
    JS_RunGC(rt);
    const char lifetime[] =
        "joinRetained.length===90010&&joinRetained.slice(90004)==='\\ud83d\\ude00tail'"
        "&&joinRetained.slice(0,4)==='head'";
    check = JS_Eval(ctx, lifetime, sizeof(lifetime)-1, "<join-lifetime>", 0);
    contract_ok = contract_ok && !JS_IsException(check) && JS_ToBool(ctx, check) == 1;
    JS_FreeValue(ctx, check);
    global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "joinRetained", JS_UNDEFINED);
    JS_FreeValue(ctx, global);
    JS_RunGC(rt);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(rt, &usage);
    JS_SetMemoryLimit(rt, usage.malloc_size + 200000);
    JS_SetGCThreshold(rt, usage.malloc_size + 190000);
    const char source[] =
        "for(let i=0;i<600;i++){const cycle={};cycle.self=cycle;}parts.join('').length";
    JSValue result = JS_Eval(ctx, source, sizeof(source)-1, "<join-pressure>", 0);
    int32_t length = 0;
    int okay = contract_ok && !JS_IsException(result) && !JS_ToInt32(ctx, &length, result)
        && length == 90012;
    if (!okay) fprintf(stderr, "dense join pressure: exception=%d length=%d\n",
                       JS_IsException(result), length);
    if (JS_IsException(result)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, result);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    return okay ? 0 : 1;
}

static int run_function_source_allocation_pressure(bool wide)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMemoryLimit(rt, 1024u * 1024u);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    const char prefix[] = "function sourceCopy(){/*";
    const char suffix[] = "*/return 42}";
    size_t length = sizeof(prefix)-1 + 180000 + sizeof(suffix)-1;
    char *source = malloc(length+1);
    if (!source) return 1;
    memcpy(source, prefix, sizeof(prefix)-1);
    memset(source+sizeof(prefix)-1, 'x', 180000);
    if (wide) memcpy(source+sizeof(prefix)-1, "\xe2\x82\xac", 3);
    memcpy(source+sizeof(prefix)-1+180000, suffix, sizeof(suffix));
    JSValue value = JS_Eval(ctx, source, length, "<source-copy-pressure>", 0);
    if (JS_IsException(value)) return 1;
    JS_FreeValue(ctx, value);
    JS_RunGC(rt);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(rt, &usage);
    JS_SetMemoryLimit(rt, usage.malloc_size + 240000);
    JS_SetGCThreshold(rt, usage.malloc_size + 230000);
    const char copy[] =
        "for(let i=0;i<600;i++){const cycle={};cycle.self=cycle;}"
        "String(sourceCopy).length";
    value = JS_Eval(ctx, copy, sizeof(copy)-1, "<source-copy-call>", 0);
    int32_t number = 0;
    int okay = !JS_IsException(value) && !JS_ToInt32(ctx, &number, value)
        && number == (int32_t)(sizeof(prefix)-1+180000+strlen("*/return 42}")
                              - (wide ? 2 : 0));
    if (!okay) fprintf(stderr,"function source pressure: exception=%d length=%d\n",
                       JS_IsException(value), number);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
    }
    JS_FreeValue(ctx, value);
    free(source);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    return okay ? 0 : 1;
}

static int run_source_snapshot_sharing(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMaxStackSize(rt, test_stack_limit());
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    const char prefix[] = "function snapshotOwner(){function child(){return 42}/*";
    const char suffix[] = "*/return child}";
    size_t length = sizeof(prefix)-1 + 65536 + sizeof(suffix)-1;
    char *source = malloc(length + 1);
    if (!source) return 1;
    memcpy(source, prefix, sizeof(prefix)-1);
    memset(source + sizeof(prefix)-1, 'x', 65536);
    memcpy(source + sizeof(prefix)-1 + 65536, suffix, sizeof(suffix));
    int okay = 1;
    for (unsigned order = 0; order < 2 && okay; ++order) {
        JS_SetMemoryLimit(rt, 2u * MIB);
        JSValue value = JS_Eval(ctx, source, length, "<snapshot-owner>", 0);
        okay = !JS_IsException(value);
        JS_FreeValue(ctx, value);
        JS_RunGC(rt);
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, usage.malloc_size + 16u * 1024u);
        const char snapshot[] = "String(snapshotOwner)";
        value = JS_Eval(ctx, snapshot, sizeof(snapshot)-1, "<shared-snapshot>", 0);
        okay = okay && !JS_IsException(value);
        JS_SetMemoryLimit(rt, 2u * MIB);
        /* Atomization must not allow concatenation or source teardown to
           modify/free the backing registered in the runtime's atom table. */
        JSAtom atom = okay ? JS_ValueToAtom(ctx, value) : JS_ATOM_NULL;
        JSValue global = JS_GetGlobalObject(ctx);
        if (order == 0)
            JS_SetPropertyStr(ctx, global, "snapshotOwner", JS_UNDEFINED);
        JS_RunGC(rt);
        const char *text = okay ? JS_ToCString(ctx, value) : NULL;
        okay = okay && atom != JS_ATOM_NULL && text && strcmp(text, source) == 0;
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, value);
        JS_FreeAtom(ctx, atom);
        if (order == 1) {
            const char call[] = "snapshotOwner()()===42";
            value = JS_Eval(ctx, call, sizeof(call)-1, "<source-after-snapshot>", 0);
            okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
            JS_FreeValue(ctx, value);
            JS_SetPropertyStr(ctx, global, "snapshotOwner", JS_UNDEFINED);
        }
        JS_FreeValue(ctx, global);
        JS_RunGC(rt);
    }
    const char semantics[] =
        "(()=>{let calls=0;const o={[Symbol.toPrimitive](hint){"
        "if(hint!=='string')throw Error('hint');calls++;return 'x'.repeat(12000)}};"
        "if(String(o).length!==12000||calls!==1||new String(o).valueOf().length!==12000"
        "||calls!==2||String(Symbol('x'))!=='Symbol(x)')return false;"
        "try{String({[Symbol.toPrimitive](){return Symbol()}});return false}catch(e){"
        "return e instanceof TypeError}})()";
    JSValue value = JS_Eval(ctx, semantics, sizeof(semantics)-1, "<string-coercion>", 0);
    okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
    JS_FreeValue(ctx, value);
    const char unicode_chunks[] =
        "(()=>{const text='function chunked(){/*'+'x'.repeat(4070)+'\\u20ac'"
        "+'y'.repeat(4094)+'\\ud83d\\ude80'+'z'.repeat(8192)+'*/return 7}';"
        "const f=eval('('+text+')');return String(f)===text&&f()===7})()";
    value = JS_Eval(ctx, unicode_chunks, sizeof(unicode_chunks)-1, "<source-chunks>", 0);
    okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
    JS_FreeValue(ctx, value);
    free(source);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    if (!okay) fprintf(stderr, "shared source snapshot failed\n");
    return okay ? 0 : 1;
}

static int run_source_span_snapshot(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMaxStackSize(rt, test_stack_limit());
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    int okay = 1;
    for (unsigned order = 0; order < 4 && okay; ++order) {
        const char setup[] =
            "globalThis.expected='function child(){/*'+'x'.repeat(32768)"
            "+'\\u20ac\\ud83d\\ude80'+'y'.repeat(32768)+'*/return 42}';"
            "globalThis.factory=eval('(function outer(){/*\\u20ac*/'+expected+';return child})');"
            "globalThis.child=factory();globalThis.snap=null;";
        JSValue value = JS_Eval(ctx, setup, sizeof(setup)-1, "<source-span-setup>", 0);
        okay = !JS_IsException(value);
        JS_FreeValue(ctx, value);
        if (order >= 2) {
            /* No nested functions: the source has no shared owner until its
               first toString call, which must adopt rather than duplicate it. */
            const char standalone[] =
                "child=eval('('+expected+')');factory=()=>child";
            value = JS_Eval(ctx, standalone, sizeof(standalone)-1,
                "<standalone-source>", 0);
            okay = okay && !JS_IsException(value);
            JS_FreeValue(ctx, value);
        }
        JS_RunGC(rt);
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, usage.malloc_size + 16u * 1024u);
        const char capture[] = "snap=String(child);snap.length===expected.length";
        value = JS_Eval(ctx, capture, sizeof(capture)-1, "<source-span-snapshot>", 0);
        okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
        JS_FreeValue(ctx, value);
        JS_SetMemoryLimit(rt, 6u * MIB);
        const char release_first[] = "factory=null;child=null";
        if ((order & 1u) == 0) {
            value = JS_Eval(ctx, release_first, sizeof(release_first)-1, "<source-span-release>", 0);
            JS_FreeValue(ctx, value);
            JS_RunGC(rt);
        }
        const char check[] =
            "(()=>{if(snap!==expected||snap[9]!=='c'||snap.charCodeAt(9)!==99)return false;"
            "if(new Map([[snap,42]]).get(expected)!==42)return false;"
            "let joined=['(',snap,')'].join('');if(eval(joined)()!==42)return false;"
            "let rope=snap;const tail='z'.repeat(1024);for(let i=0;i<100;i++)rope=rope+tail;"
            "if(rope.length!==snap.length+102400)return false;"
            "if(rope.slice(0,snap.length)!==expected)return false;"
            "const o={[snap]:42};if(o[expected]!==42)return false;"
            "if(JSON.parse(JSON.stringify([snap]))[0]!==expected)return false;"
            "if(!/return 42/.test(snap)||String(snap).indexOf('child')!==9)return false;"
            "return eval(' '.repeat(140000)+'('+snap+')')()===42})()";
        value = JS_Eval(ctx, check, sizeof(check)-1, "<source-span-semantics>", 0);
        okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
        JS_FreeValue(ctx, value);
        const char release[] = "snap=null;expected=null";
        value = JS_Eval(ctx, release, sizeof(release)-1, "<source-span-drop>", 0);
        JS_FreeValue(ctx, value);
        JS_RunGC(rt);
        if ((order & 1u) != 0) {
            const char call[] = "child()===42&&factory()()===42";
            value = JS_Eval(ctx, call, sizeof(call)-1, "<source-span-survives>", 0);
            okay = okay && !JS_IsException(value) && JS_ToBool(ctx, value) == 1;
            JS_FreeValue(ctx, value);
        }
        value = JS_Eval(ctx, release_first, sizeof(release_first)-1, "<source-span-final>", 0);
        JS_FreeValue(ctx, value);
        JS_RunGC(rt);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay = budget_quickjs_pool_destroy(pool) && budget.current == 0 && okay;
    if (!okay) fprintf(stderr, "source span snapshot failed\n");
    return okay ? 0 : 1;
}

static int run_large_repeat_eval(void)
{
    static const char memory_source[] =
        "let local=40;eval(' '.repeat(1337331)+'local+2')";
    static const char semantic_source[] =
        "(()=>{let local=40;"
        "const direct=eval(' '.repeat(1337331)+'local+2');"
        "const tabs=eval(' \\t'.repeat(668666)+'40+2');"
        "const astral=eval(' '.repeat(1337331)+"
        "\"'\\ud83d\\ude00'.length\");"
        "let column=0;try{eval(' '.repeat(1337331)+'!')}"
        "catch(e){column=e.columnNumber}"
        "const repeated='ab'.repeat(70000);"
        "const newline=eval(' '.repeat(1337331)+'\\n7');"
        "const longLeaf='/*'.padEnd(9002,'x')+'*/21*2';"
        "const pollLeaf='/*'.padEnd(5002,'y')+'*/6*7';"
        "const longResult=eval(' '.repeat(1337331)+longLeaf);"
        "const pollResult=eval(' '.repeat(1337331)+pollLeaf);"
        "return [direct,tabs,astral,repeated.length,repeated.charAt(139999),"
        "newline,column,longResult,pollResult].join(',')})()";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) return 1;

    /* The pre-fix implementation constructs and then linearizes a 1.3 MiB
       flat repeat result and cannot run this under 2.5 MiB. A balanced repeat
       rope plus prefix-aware eval stays well inside the same hard ceiling. */
    JS_SetMemoryLimit(runtime, 2560u * 1024u);
    JS_SetMaxStackSize(runtime, test_stack_limit());
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) return 1;
    JSValue value = JS_Eval(context, memory_source,
                            sizeof(memory_source) - 1u,
                            "<large-repeat-eval-memory>",
                            JS_EVAL_TYPE_GLOBAL);
    int32_t result = 0;
    if (JS_IsException(value) || JS_ToInt32(context, &result, value)
        || result != 42) {
        if (JS_IsException(value)) {
            JSValue exception = JS_GetException(context);
            const char *message = JS_ToCString(context, exception);
            fprintf(stderr, "repeat memory check: %s\n", message ? message : "exception");
            JS_FreeCString(context, message);
            JS_FreeValue(context, exception);
        } else {
            JS_FreeValue(context, value);
        }
        return 1;
    }
    JS_FreeValue(context, value);
#ifdef CONFIG_TILEFINCH_EVAL_ROPE_TEST
    {
        uint64_t physical_work = JS_GetEvalRopePhysicalWork(runtime);
        if (physical_work == 0 || physical_work >= 10000u) {
            fprintf(stderr, "repeat eval physical work: %llu\n",
                    (unsigned long long) physical_work);
            return 1;
        }
    }
#endif

    /* Large repeat remains an ordinary ECMAScript string. The compact eval
       path preserves direct-eval scope, does not discard a newline, and keeps
       the same one-based syntax-error column as the flat-string path. */
    JS_SetMemoryLimit(runtime, 6u * MIB);
    value = JS_Eval(context, semantic_source,
                    sizeof(semantic_source) - 1u,
                    "<large-repeat-eval-semantics>",
                    JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "repeat semantics check: %s\n", message ? message : "exception");
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
        return 1;
    }
    const char *text = JS_ToCString(context, value);
    int okay = text != NULL
        && strcmp(text, "42,42,2,140000,b,7,1337333,42,42") == 0;
    if (!okay) fprintf(stderr, "repeat semantics result: %s\n", text ? text : "null");
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);
    if (!okay) return 1;

    /* A hostile virtual prefix must not make native compaction scan toward
       the one-billion-code-unit string limit. The rope stays virtual, the
       bounded refusal is catchable, and the realm remains usable. */
    static const char bounded_source[] =
        "(()=>{const exact=eval(' '.repeat(4194304));"
        "let bounded=false;try{eval(' '.repeat(4194304)+'0')}"
        "catch(error){bounded=error instanceof RangeError;}"
        "return exact===undefined&&bounded&&eval('40+2')===42;})()";
    value = JS_Eval(context, bounded_source,
                    sizeof(bounded_source) - 1u,
                    "<bounded-repeat-eval-scan>",
                    JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value) || JS_ToBool(context, value) != 1) {
        if (JS_IsException(value)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
        } else {
            JS_FreeValue(context, value);
        }
        return 1;
    }
    JS_FreeValue(context, value);

    /* Cached subtree accounting remains a logical scan: it must continue to
       poll at the ordinary interval even though it no longer revisits every
       virtual character. An interrupted eval must not poison its runtime. */
    static const char prepare_interrupt_source[] =
        "globalThis.__repeatInterrupt="
        "' '.repeat(1337331)+'40+2'";
    value = JS_Eval(context, prepare_interrupt_source,
                    sizeof(prepare_interrupt_source) - 1u,
                    "<repeat-eval-interrupt-prepare>",
                    JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) return 1;
    JS_FreeValue(context, value);
    RepeatEvalInterrupt interrupt = { .stop_after = 1u };
    JS_SetInterruptHandler(runtime, interrupt_repeat_eval, &interrupt);
    static const char interrupt_source[] =
        "eval(globalThis.__repeatInterrupt)";
    value = JS_Eval(context, interrupt_source,
                    sizeof(interrupt_source) - 1u,
                    "<repeat-eval-interrupt>", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(value) || interrupt.polls == 0) {
        JS_FreeValue(context, value);
        return 1;
    }
    JS_FreeValue(context, JS_GetException(context));
    JS_SetInterruptHandler(runtime, NULL, NULL);
    static const char after_interrupt_source[] = "40+2";
    value = JS_Eval(context, after_interrupt_source,
                    sizeof(after_interrupt_source) - 1u,
                    "<repeat-eval-after-interrupt>",
                    JS_EVAL_TYPE_GLOBAL);
    result = 0;
    if (JS_IsException(value) || JS_ToInt32(context, &result, value)
        || result != 42) {
        JS_FreeValue(context, value);
        return 1;
    }
    JS_FreeValue(context, value);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0)
        return 1;
    return okay ? 0 : 1;
}

static int run_compact_character_array(void)
{
    static const char memory_source[] =
        "(()=>{const n=617000;const chars=[];"
        "for(let i=0;i<n;i++)chars.push(String.fromCharCode((i*29+7)&255));"
        "const joined=chars.join('');"
        "return [joined.length,joined.charCodeAt(0),"
        "joined.charCodeAt(12345),joined.charCodeAt(n-1)].join(':')})()";
    static const char semantic_source[] =
        "(()=>{const n=128;const chars=[];"
        "for(let i=0;i<n;i++)chars.push(String.fromCharCode((i*29+7)&255));"
        "const joined=chars.join('');"
        "chars[3]='Z';if(chars[3]!=='Z')return 'set';"
        "delete chars[n-1];chars[n-1]=String.fromCharCode(202);"
        "if(chars.length!==n||chars[n-1].charCodeAt(0)!==202)return 'append';"
        "chars.length=64;chars.copyWithin(8,0,8);"
        "const copied=chars[8].charCodeAt(0)===7&&"
        "chars[15].charCodeAt(0)===210;"
        "const removed=chars.splice(4,2,'X','Y');"
        "chars.reverse();"
        "const applied=String.prototype.concat.apply('',chars.slice(0,4));"
        "Object.defineProperty(chars,'1',{value:'K',writable:true,"
        "enumerable:true,configurable:true});"
        "chars.push('wide');"
        "let sparse=[];sparse.unshift('9');sparse.length=70;"
        "sparse.fill('q',1,45);sparse.splice(23,4,'N');"
        "sparse.reverse();sparse.reverse();sparse.length=1;"
        "if(sparse.length!==1||Object.keys(sparse).join(',')!=='0'"
        "||sparse.join('')!=='9')return 'length-shrink';"
        "const preserved=joined.length===n&&joined.charCodeAt(3)===94;"
        "return [preserved?1:0,copied?1:0,removed[0].charCodeAt(0),"
        "removed[1].charCodeAt(0),applied.length,chars[1],"
        "chars[64],chars.length].join(':')})()";
    static const char ownership_source[] =
        "(()=>{let atomChars=[];for(let i=0;i<96;i++)"
        "atomChars.push(String.fromCharCode(65+i%26));"
        "let atomString=atomChars.join('');const atomLength=atomString.length,"
        "atomFirst=atomString.charCodeAt(0),holder={};"
        "holder[atomString]=17;atomString=null;"
        "for(let i=0;i<96;i++)atomChars.push(String.fromCharCode(97+i%26));"
        "atomChars.length=40;atomChars.push('q');"
        "const retainedKey=Object.keys(holder)[0],atomSafe="
        "holder[retainedKey]===17&&retainedKey.length===atomLength"
        "&&retainedKey.charCodeAt(0)===atomFirst;"
        "let wide=[];for(let i=0;i<48;i++)wide.push('a');"
        "wide[7]='Ā';const wideSafe=wide[7]==='Ā'&&wide.length===48;"
        "let astral=[];for(let i=0;i<48;i++)astral.push('b');"
        "astral[9]='😀';const astralSafe=astral[9]==='😀'&&astral.length===48;"
        "let shared=[];for(let i=0;i<128;i++)shared.push('c');"
        "const beforeGrowth=shared.join('');for(let i=0;i<160;i++)shared.push('d');"
        "const afterGrowth=shared.join('');shared.length=8;"
        "const cowSafe=beforeGrowth.length===128&&beforeGrowth[127]==='c'"
        "&&afterGrowth.length===288&&afterGrowth[287]==='d'"
        "&&shared.length===8&&shared.join('')==='cccccccc';"
        "let arrayFirst=[];for(let i=0;i<32;i++)arrayFirst.push('e');"
        "const survivesArray=arrayFirst.join('');arrayFirst=null;"
        "const arrayFirstSafe=survivesArray==='e'.repeat(32);"
        "let stringFirst=[];for(let i=0;i<32;i++)stringFirst.push('f');"
        "let released=stringFirst.join('');released=null;stringFirst.push('g');"
        "const stringFirstSafe=stringFirst.length===33"
        "&&stringFirst[32]==='g';"
        "return [atomSafe,wideSafe,astralSafe,cowSafe,arrayFirstSafe,"
        "stringFirstSafe].map(Number).join(':')})()";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    if (setenv("TILEFINCH_JS_ARRAY_CAP_KB", "640", 1) != 0) return 1;
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) {
        unsetenv("TILEFINCH_JS_ARRAY_CAP_KB");
        return 1;
    }
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) {
        unsetenv("TILEFINCH_JS_ARRAY_CAP_KB");
        return 1;
    }

    /* The response decoder used by large challenge scripts builds a dense
       character array before joining it. Normal JSValue storage exceeds this
       ceiling, while the bounded compact representation retains ordinary
       Array semantics and deoptimizes before unsupported mutations. */
    JS_SetMemoryLimit(runtime, 2u * MIB);
    JS_SetMaxStackSize(runtime, test_stack_limit());
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) {
        unsetenv("TILEFINCH_JS_ARRAY_CAP_KB");
        return 1;
    }
    int okay = 0;
    JSValue value = JS_Eval(context, memory_source,
                            sizeof(memory_source) - 1u,
                            "<compact-character-array-memory>",
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "compact character-array memory exception: %s\n",
                message != NULL ? message : "<unprintable>");
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
        goto cleanup;
    }
    const char *text = JS_ToCString(context, value);
    if (text == NULL || strcmp(text, "617000:7:124:114") != 0) {
        fprintf(stderr, "compact character-array memory result: %s\n",
                text != NULL ? text : "<unprintable>");
        JS_FreeCString(context, text);
        JS_FreeValue(context, value);
        goto cleanup;
    }
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);

    value = JS_Eval(context, semantic_source,
                    sizeof(semantic_source) - 1u,
                    "<compact-character-array-semantics>",
                    JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "compact character-array semantic exception: %s\n",
                message != NULL ? message : "<unprintable>");
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
        goto cleanup;
    }
    text = JS_ToCString(context, value);
    okay = text != NULL
        && strcmp(text, "1:1:123:152:4:K:wide:65") == 0;
    if (!okay)
        fprintf(stderr, "compact character-array semantic result: %s\n",
                text != NULL ? text : "<unprintable>");
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);

    value = JS_Eval(context, ownership_source,
                    sizeof(ownership_source) - 1u,
                    "<compact-character-array-ownership>",
                    JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "compact character-array ownership exception: %s\n",
                message != NULL ? message : "<unprintable>");
        JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
        goto cleanup;
    }
    text = JS_ToCString(context, value);
    bool ownership_ok = text != NULL
        && strcmp(text, "1:1:1:1:1:1") == 0;
    if (!ownership_ok)
        fprintf(stderr, "compact character-array ownership result: %s\n",
                text != NULL ? text : "<unprintable>");
    okay = okay && ownership_ok;
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);

cleanup:
    unsetenv("TILEFINCH_JS_ARRAY_CAP_KB");
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0)
        return 1;
    return okay ? 0 : 1;
}

static int run_failure_boundary(size_t successful_allocations)
{
    static const char source[] =
        "(()=>{function descend(depth){if(depth!==0)"
        "return descend(depth-1);const value=null;return value.missing}"
        "return descend(64)})()";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) return 1;
    JS_SetMemoryLimit(runtime, 4u * MIB);
    JS_SetMaxStackSize(runtime, test_stack_limit());
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) return 1;

    JSValue compiled = JS_Eval(
        context, source, sizeof(source) - 1u, "<oom-backtrace>",
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return 1;
    budget_inject_failure_after(&budget, successful_allocations);
    JSValue value = JS_EvalFunction(context, compiled);
    budget_clear_failure_injection(&budget);
    if (!JS_IsException(value)) {
        JS_FreeValue(context, value);
        return 1;
    }
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0) return 1;
    return 0;
}

static int run_scope_resolution_memory_limit(size_t allowance)
{
    char source[8192];
    size_t used = (size_t) snprintf(source, sizeof(source),
                                  "function labels(){return function(){return function(){return ");
    for (unsigned i = 0; i < 256; i++) {
        used += (size_t) snprintf(source + used, sizeof(source) - used,
                                 "%slabel%u", i ? "+" : "", i);
    }
    used += (size_t) snprintf(source + used, sizeof(source) - used, ";};};}labels;");
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) return 1;
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) { JS_FreeRuntime(runtime); return 1; }
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(runtime, &usage);
    JS_SetMemoryLimit(runtime, usage.malloc_size + allowance);
    JSValue compiled = JS_Eval(context, source, used, "<scope-memory-limit>",
                              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_SetMemoryLimit(runtime, (size_t) -1);
    if (JS_IsException(compiled)) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
    } else JS_FreeValue(context, compiled);
    JSValue next = JS_Eval(context, "6*7", 3, "<after-refusal>", JS_EVAL_TYPE_GLOBAL);
    int32_t value = 0;
    int failed = JS_IsException(next) || JS_ToInt32(context, &value, next) || value != 42;
    JS_FreeValue(context, next);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0) failed = 1;
    return failed;
}

static int run_parse_failure_boundary(size_t successful_allocations)
{
    /* Enough functions, literals, and containers that parsing itself
       performs many budgeted allocations before any bytecode runs. */
    static const char source[] =
        "(()=>{function a(x){return {p:x,q:'text-'+x,r:[x,x+1,x+2]};}"
        "function b(y){return a(y).r.map(v=>v*2).join(',');}"
        "class C{constructor(v){this.v=v;}method(){return b(this.v);}}"
        "return new C(3).method();})()";
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) return 1;
    JS_SetMemoryLimit(runtime, 4u * MIB);
    JS_SetMaxStackSize(runtime, test_stack_limit());
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) return 1;

    budget_inject_failure_after(&budget, successful_allocations);
    JSValue compiled = JS_Eval(
        context, source, sizeof(source) - 1u, "<parse-oom>",
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    budget_clear_failure_injection(&budget);
    if (JS_IsException(compiled)) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
    } else {
        /* Boundaries past the parse's allocation count compile normally;
           the invariant under test is clean unwind and zero owned bytes. */
        JS_FreeValue(context, compiled);
    }
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void) budget_quickjs_pool_trim(pool, 0);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0) return 1;
    return 0;
}

static int run_retired_callable_contract(void)
{
    static const char *sources[] = {
        "(async function task(value){return value})",
        "(function* task(value){yield value})",
        "(async function* task(value){yield value})"
    };
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JS_SetMaxStackSize(rt, test_stack_limit());
    JSContext *caller = JS_NewContext(rt);
    int failed = caller == NULL;
    for (unsigned i = 0; !failed && i < 3; ++i) {
        JSContext *realm = JS_NewContext(rt);
        if (!realm) { failed = 1; break; }
        JSValue fn = JS_Eval(realm, sources[i], strlen(sources[i]),
                             "<callable-lifecycle>", JS_EVAL_TYPE_GLOBAL);
        JSValue arg = JS_NewInt32(caller, 42);
        JSValue live = JS_Call(caller, fn, JS_UNDEFINED, 1, &arg);
        failed |= JS_IsException(live);
        JSValue next = JS_UNDEFINED;
        if (i == 1) {
            const char *get_next = "Object.getPrototypeOf((function*(){})()).next";
            next = JS_Eval(caller, get_next, strlen(get_next),
                           "<live-generator-method>", JS_EVAL_TYPE_GLOBAL);
            JSValue yielded = JS_Call(caller, next, live, 0, NULL);
            JSValue value = JS_GetPropertyStr(caller, yielded, "value");
            int32_t number = 0;
            failed |= JS_IsException(yielded)
                || JS_ToInt32(caller, &number, value) != 0 || number != 42;
            JS_FreeValue(caller, value);
            JS_FreeValue(caller, yielded);
        }
        JS_RetireContext(realm);
        if (i == 1) {
            JSValue stopped = JS_Call(caller, next, live, 0, NULL);
            failed |= !JS_IsException(stopped);
            JS_FreeValue(caller, stopped);
            JS_FreeValue(caller, JS_GetException(caller));
        }
        JS_FreeValue(caller, next);
        JS_FreeValue(caller, live);
        JSValue retired = JS_Call(caller, fn, JS_UNDEFINED, 1, &arg);
        failed |= !JS_IsException(retired);
        JS_FreeValue(caller, retired);
        JSValue exception = JS_GetException(caller);
        failed |= JS_IsNull(exception);
        JS_FreeValue(caller, exception);
        JS_FreeValue(caller, fn);
        JS_FreeContext(realm);
    }
    if (caller) {
        JSValue check = JS_Eval(caller, "6*7", 3, "<live-control>", JS_EVAL_TYPE_GLOBAL);
        int32_t value = 0;
        failed |= JS_ToInt32(caller, &value, check) != 0 || value != 42;
        JS_FreeValue(caller, check);
        JS_FreeContext(caller);
    }
    JS_FreeRuntime(rt);
    failed |= !budget_quickjs_pool_destroy(pool) || budget.current != 0;
    return failed;
}

static int run_cstring_refusal_lifetime(void)
{
    Budget budget;
    budget_init(&budget, 2u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    int okay = 1;
    /* Both Latin-1 expansion and UTF-16 conversion duplicate the input
       before allocating output. Refusal must release that duplicate. */
    for (unsigned wide = 0; wide < 2; ++wide) {
        char bytes[16384];
        for (size_t i = 0; i < sizeof(bytes); i += 2) {
            bytes[i] = wide ? (char)0xc4 : (char)0xc3;
            bytes[i + 1] = (char)0xa9;
        }
        JSValue input = JS_NewStringLen(ctx, bytes, sizeof(bytes));
        okay &= !JS_IsException(input);
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, (size_t)usage.malloc_size);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            size_t length = 1;
            const char *text = JS_ToCStringLen(ctx, &length, input);
            okay &= text == NULL && length == 0;
            JS_FreeCString(ctx, text);
            JSValue exception = JS_GetException(ctx);
            okay &= !JS_IsNull(exception);
            JS_FreeValue(ctx, exception);
        }
        JS_SetMemoryLimit(rt, 2u * MIB);
        const char *text = JS_ToCString(ctx, input);
        okay &= text != NULL && memcmp(text, bytes, sizeof(bytes)) == 0;
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, input);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay &= budget_quickjs_pool_destroy(pool) && budget.current == 0;
    if (!okay) fprintf(stderr, "CString refusal leaked its source\n");
    return okay ? 0 : 1;
}

static int run_bytecode_refusal_atomicity(void)
{
    Budget budget;
    budget_init(&budget, 4u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    char source[10000];
    const char prefix[] = "function retained(){return '";
    const char suffix[] = "'};retained()";
    memcpy(source, prefix, sizeof(prefix)-1);
    memset(source + sizeof(prefix)-1, 'x', 9000);
    memcpy(source + sizeof(prefix)-1 + 9000, suffix, sizeof(suffix));
    size_t source_length = sizeof(prefix)-1 + 9000 + sizeof(suffix)-1;
    JSValue compiled = JS_Eval(ctx, source, source_length, "<writer-refusal>",
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    int okay = !JS_IsException(compiled);
    if (!okay) {
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        fprintf(stderr, "writer compile: %s\n", message ? message : "no message");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
    }
    size_t expected_length = 0;
    uint8_t *expected = okay ? JS_WriteObject(ctx, &expected_length, compiled,
        JS_WRITE_OBJ_BYTECODE) : NULL;
    okay &= expected != NULL;
    unsigned refused = 0, accepted = 0;
    for (size_t allowance = 0; okay && allowance <= 65536; allowance += 512) {
        JSMemoryUsage usage;
        JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, (size_t)usage.malloc_size + allowance);
        size_t length = 1;
        uint8_t *bytes = JS_WriteObject(ctx, &length, compiled, JS_WRITE_OBJ_BYTECODE);
        JS_SetMemoryLimit(rt, 4u * MIB);
        if (bytes) {
            accepted++;
            if (length != expected_length || memcmp(bytes, expected, length) != 0)
                fprintf(stderr, "writer allowance=%zu got=%zu expected=%zu\n",
                    allowance, length, expected_length);
            okay &= length == expected_length && memcmp(bytes, expected, length) == 0;
        } else {
            refused++;
            okay &= length == 0;
            JSValue exception = JS_GetException(ctx);
            JS_FreeValue(ctx, exception);
        }
        js_free(ctx, bytes);
    }
    okay &= refused != 0 && accepted != 0;
    js_free(ctx, expected);
    JS_FreeValue(ctx, compiled);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay &= budget_quickjs_pool_destroy(pool) && budget.current == 0;
    if (!okay) fprintf(stderr, "Bytecode writer published a partial artifact (accepted=%u refused=%u expected=%zu)\n", accepted, refused, expected_length);
    return okay ? 0 : 1;
}

static int run_sparse_bytecode_atoms(void)
{
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 1;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 1;
    enum { COUNT = 20000 };
    JSAtom *atoms = malloc(COUNT * sizeof(*atoms));
    if (!atoms) return 1;
    int okay = 1;
    for (unsigned i = 0; i < COUNT; ++i) {
        char name[40];
        snprintf(name, sizeof(name), "earlier_page_property_%u", i);
        atoms[i] = JS_NewAtom(ctx, name);
        okay &= atoms[i] != JS_ATOM_NULL;
    }
    const char source[] = "(()=>{const lateValue=40;return {lateResult:lateValue+2}})()";
    JSValue compiled = JS_Eval(ctx, source, sizeof(source)-1,
        "<late-small-script>", JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    okay &= !JS_IsException(compiled);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(rt, &usage);
    JS_SetMemoryLimit(rt, (size_t)usage.malloc_size + 16u * 1024u);
    size_t rejects = budget_quickjs_pool_rejection_count(pool), length = 0;
    uint8_t *bytes = JS_IsException(compiled) ? NULL
        : JS_WriteObject(ctx, &length, compiled, JS_WRITE_OBJ_BYTECODE);
    okay &= bytes != NULL && length != 0
        && budget_quickjs_pool_rejection_count(pool) == rejects;
    JS_SetMemoryLimit(rt, 8u * MIB);
    if (bytes) {
        JSValue restored = JS_ReadObject(ctx, bytes, length, JS_READ_OBJ_BYTECODE);
        JSValue result = JS_IsException(restored) ? restored : JS_EvalFunction(ctx, restored);
        JSValue field = JS_IsException(result) ? JS_UNDEFINED
            : JS_GetPropertyStr(ctx, result, "lateResult");
        int32_t answer = 0;
        okay &= !JS_IsException(result) && JS_ToInt32(ctx, &answer, field) == 0 && answer == 42;
        JS_FreeValue(ctx, field);
        JS_FreeValue(ctx, result);
    }
    js_free(ctx, bytes);
    JS_FreeValue(ctx, compiled);
    for (unsigned i = 0; i < COUNT; ++i) JS_FreeAtom(ctx, atoms[i]);
    free(atoms);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay &= budget_quickjs_pool_destroy(pool) && budget.current == 0;
    if (!okay) fprintf(stderr, "Bytecode cache scaled with unrelated page atoms\n");
    return okay ? 0 : 1;
}

static int run_near_limit_array_growth(void)
{
#ifndef CONFIG_TILEFINCH_EVAL_ROPE_TEST
    return 0;
#else
    Budget budget;
    budget_init(&budget, 8u * MIB);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(
        budget_quickjs_pool_allocator(), pool);
    if (runtime == NULL) return 1;
    JS_SetMemoryLimit(runtime, 4u * MIB);
    JS_SetMaxStackSize(runtime, test_stack_limit());
    JSContext *context = JS_NewContext(runtime);
    if (context == NULL) return 1;

    static const char prepare[] =
        /* 94215 is an exact fast-array capacity reached after the host's
           large-array growth cap takes over. */
        "globalThis.nearLimitArray=Array(94215).fill(0);";
    JSValue value = JS_Eval(context, prepare, sizeof(prepare) - 1u,
                            "<near-limit-array-prepare>",
                            JS_EVAL_TYPE_GLOBAL);
    int okay = !JS_IsException(value);
    JS_FreeValue(context, value);
    JSValue global = JS_GetGlobalObject(context);
    JSValue array = JS_GetPropertyStr(context, global, "nearLimitArray");
    uint32_t capacity = JS_GetFastArrayCapacityForTest(array);
    okay &= capacity >= 94215u
        && JS_SetPropertyStr(context, global, "nearLimitCapacity",
                            JS_NewUint32(context, capacity)) >= 0;
    JS_FreeValue(context, array);
    JS_FreeValue(context, global);
    static const char fill_capacity[] =
        "while(nearLimitArray.length<nearLimitCapacity)"
        "nearLimitArray.push(0);";
    value = JS_Eval(context, fill_capacity, sizeof(fill_capacity) - 1u,
                    "<near-limit-array-fill-capacity>",
                    JS_EVAL_TYPE_GLOBAL);
    okay &= !JS_IsException(value);
    JS_FreeValue(context, value);
    JS_RunGC(runtime);
    /* Isolate the explicit array-growth collection from the ordinary
       allocator threshold. */
    JS_SetGCThreshold(runtime, SIZE_MAX);

    size_t live = budget_quickjs_pool_js_malloc_current(pool);
    JS_SetMemoryLimit(runtime, live + 96u * 1024u);
    uint64_t gc_before = JS_GetGCRunCount(runtime);
    static const char grow[] =
        "for(let i=0;i<4096;i++)nearLimitArray.push(i);"
        "nearLimitArray.length";
    value = JS_Eval(context, grow, sizeof(grow) - 1u,
                    "<near-limit-array-grow>", JS_EVAL_TYPE_GLOBAL);
    int32_t length = 0;
    okay &= !JS_IsException(value)
        && JS_ToInt32(context, &length, value) == 0
        && length == (int32_t)(capacity + 4096u);
    uint64_t gc_delta = JS_GetGCRunCount(runtime) - gc_before;
    okay &= gc_delta == 0u;
    JS_FreeValue(context, value);

    JS_SetMemoryLimit(runtime, 4u * MIB);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    (void)budget_quickjs_pool_trim(pool, 0);
    okay &= budget_quickjs_pool_destroy(pool) && budget.current == 0;
    if (!okay) {
        fprintf(stderr,
                "near-limit array growth failed length=%d gc-runs=%llu\n",
                length, (unsigned long long)gc_delta);
    }
    return okay ? 0 : 1;
#endif
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        char *end = NULL;
        unsigned long requested = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') return 2;
        return run_failure_boundary((size_t) requested);
    }
    if (argc != 1) return 2;
    if (run_bytecode_refusal_atomicity() != 0) return 1;
    if (run_sparse_bytecode_atoms() != 0) return 1;
    if (run_near_limit_array_growth() != 0) return 1;
    if (run_cstring_refusal_lifetime() != 0) return 1;
    if (run_source_snapshot_sharing() != 0) return 1;
    if (run_source_span_snapshot() != 0) return 1;
    if (run_dense_join_pressure() != 0) return 1;
    if (run_function_source_allocation_pressure(false) != 0
        || run_function_source_allocation_pressure(true) != 0) return 1;
    if (run_automatic_gc_near_limit() != 0) {
        fprintf(stderr, "QuickJS automatic GC headroom failed\n");
        return 1;
    }
    if (run_shared_function_source() != 0) {
        fprintf(stderr, "QuickJS shared function source failed\n");
        return 1;
    }
    if (run_retired_callable_contract() != 0) {
        fprintf(stderr, "QuickJS retired callable lifecycle failed\n");
        return 1;
    }

    if (run_reallocation_peak_census() != 0) {
        fprintf(stderr, "QuickJS realloc peak census failed\n");
        return 1;
    }
    puts("QuickJS realloc peak census: PASS");

    /* Exercise every host-allocation boundary around error-object creation,
       message attachment, backtrace construction, and stack-property growth.
       The pinned Bellard revision used to release current_exception during
       one of these failures and then dereference its cleared object shape. */
    for (size_t boundary = 0; boundary <= 64; boundary++) {
        if (run_failure_boundary(boundary) != 0) {
            fprintf(stderr,
                    "QuickJS OOM backtrace failed after %zu allocations\n",
                    boundary);
            return 1;
        }
    }
    puts("QuickJS OOM backtrace boundaries: PASS");

    /* Allocation failure inside JS_Eval's parse/bytecode phase must unwind
       as cleanly as the execution-time boundaries above. */
    for (size_t boundary = 0; boundary <= 512; boundary++) {
        if (run_parse_failure_boundary(boundary) != 0) {
            fprintf(stderr,
                    "QuickJS parse OOM failed after %zu allocations\n",
                    boundary);
            return 1;
        }
    }
    puts("QuickJS parse OOM boundaries: PASS");

    for (size_t allowance = 0; allowance <= 65536; allowance += 64) {
        if (run_scope_resolution_memory_limit(allowance) != 0) {
            fprintf(stderr, "QuickJS scope OOM failed at %zu bytes\n", allowance);
            return 1;
        }
    }
    puts("QuickJS scope-resolution OOM boundaries: PASS");

    if (run_large_repeat_eval() != 0) {
        fprintf(stderr, "QuickJS large repeat/eval bound failed\n");
        return 1;
    }
    puts("QuickJS large repeat/eval bound: PASS");

    if (run_compact_character_array() != 0) {
        fprintf(stderr, "QuickJS compact character-array bound failed\n");
        return 1;
    }
    puts("QuickJS compact character-array bound: PASS");
    return 0;
}
