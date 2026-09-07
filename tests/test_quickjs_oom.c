#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int run_large_repeat_eval(void)
{
    static const char memory_source[] =
        "let local=40;eval(' '.repeat(1337331)+'local+2')";
    static const char semantic_source[] =
        "(()=>{let local=40;"
        "const direct=eval(' '.repeat(1337331)+'local+2');"
        "let column=0;try{eval(' '.repeat(1337331)+'!')}"
        "catch(e){column=e.columnNumber}"
        "const repeated='ab'.repeat(70000);"
        "const newline=eval(' '.repeat(1337331)+'\\n7');"
        "const longLeaf='/*'.padEnd(9002,'x')+'*/21*2';"
        "const pollLeaf='/*'.padEnd(5002,'y')+'*/6*7';"
        "const longResult=eval(' '.repeat(1337331)+longLeaf);"
        "const pollResult=eval(' '.repeat(1337331)+pollLeaf);"
        "return [direct,repeated.length,repeated.charAt(139999),"
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
        && strcmp(text, "42,140000,b,7,1337333,42,42") == 0;
    if (!okay) fprintf(stderr, "repeat semantics result: %s\n", text ? text : "null");
    JS_FreeCString(context, text);
    JS_FreeValue(context, value);
    if (!okay) return 1;

    /* A hostile virtual prefix must not make native compaction scan toward
       the one-billion-code-unit string limit. The rope stays virtual, the
       bounded refusal is catchable, and the realm remains usable. */
    static const char bounded_source[] =
        "(()=>{let bounded=false;try{eval(' '.repeat(4194305))}"
        "catch(error){bounded=error instanceof RangeError;}"
        "return bounded&&eval('40+2')===42;})()";
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
    okay = text != NULL && strcmp(text, "1:1:1:1:1:1") == 0;
    if (!okay)
        fprintf(stderr, "compact character-array ownership result: %s\n",
                text != NULL ? text : "<unprintable>");
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

int main(int argc, char **argv)
{
    if (argc == 2) {
        char *end = NULL;
        unsigned long requested = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') return 2;
        return run_failure_boundary((size_t) requested);
    }
    if (argc != 1) return 2;
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
    for (size_t boundary = 0; boundary <= 96; boundary++) {
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
