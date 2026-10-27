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

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--bigint-only") == 0)
        return bigint_truncation();
    if (argc == 2 && strcmp(argv[1], "--unicode-only") == 0)
        return unicode_refusal();
    return unicode_refusal() || bigint_truncation();
}
