#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

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
    JS_SetMaxStackSize(runtime, 256u * 1024u);
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
    JS_SetMaxStackSize(runtime, 256u * 1024u);
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

int main(int argc, char **argv)
{
    if (argc == 2) {
        char *end = NULL;
        unsigned long requested = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') return 2;
        return run_failure_boundary((size_t) requested);
    }
    if (argc != 1) return 2;

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
    return 0;
}
