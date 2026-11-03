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
    return unicode_refusal() || bigint_truncation()
        || symbol_key_ownership() || captured_reference_limit();
}
