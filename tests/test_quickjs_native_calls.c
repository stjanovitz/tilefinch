/* Native-call shortcuts in the vendored QuickJS (README change 28): the
   interpreter's call opcodes enter a native function without
   JS_CallInternal()'s bytecode frame (after the same interrupt poll), and
   String.prototype.charCodeAt, String.fromCharCode and Math.imul take
   their integer arguments without the general conversions. The results
   must be the general path's, every other argument shape must still be
   converted, and every native call must still consume interrupt budget. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

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
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *rt;
    JSContext *ctx;
} Realm;

static bool realm_open(Realm *realm)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 16u * MIB);
    realm->pool = budget_quickjs_pool_create(&realm->budget);
    if (realm->pool == NULL) return false;
    realm->rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), realm->pool);
    if (realm->rt == NULL) return false;
    JS_SetMaxStackSize(realm->rt, test_stack_limit());
    realm->ctx = JS_NewContext(realm->rt);
    return realm->ctx != NULL;
}

static bool realm_close(Realm *realm)
{
    if (realm->ctx) JS_FreeContext(realm->ctx);
    if (realm->rt) JS_FreeRuntime(realm->rt);
    if (realm->pool) {
        (void) budget_quickjs_pool_trim(realm->pool, 0);
        if (!budget_quickjs_pool_destroy(realm->pool)) return false;
    }
    return realm->budget.current == 0;
}

static int expect_ok(JSContext *ctx, const char *name, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), name,
                            JS_EVAL_TYPE_GLOBAL);
    int failed = 1;
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, exception);
        fprintf(stderr, "%s: exception %s\n", name, text ? text : "?");
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, exception);
    } else {
        const char *text = JS_ToCString(ctx, value);
        if (text && strcmp(text, "ok") == 0) {
            failed = 0;
        } else {
            fprintf(stderr, "%s: %s\n", name, text ? text : "?");
        }
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, value);
    return failed;
}

static const char results_source[] =
    "(() => {"
    "const errs = [];"
    "const eq = (a, b, m) => { if (!Object.is(a, b)) errs.push(m + ': ' + String(a) + ' !== ' + String(b)); };"
    /* charCodeAt */
    "const s = 'aé€\\u{1F600}z';"
    "eq(s.charCodeAt(0), 97, 'latin');"
    "eq(s.charCodeAt(1), 0xe9, 'latin-1');"
    "eq(s.charCodeAt(2), 0x20ac, 'wide');"
    "eq(s.charCodeAt(3), 0xd83d, 'surrogate');"
    "eq(s.charCodeAt(5), 122, 'after pair');"
    "eq(s.charCodeAt(6), NaN, 'past end');"
    "eq(s.charCodeAt(-1), NaN, 'negative');"
    "eq(s.charCodeAt(), 97, 'no index');"
    "eq(s.charCodeAt(1.9), 0xe9, 'fractional index');"
    "eq(s.charCodeAt('2'), 0x20ac, 'string index');"
    "eq(s.charCodeAt(NaN), 97, 'NaN index');"
    "eq(''.charCodeAt(0), NaN, 'empty');"
    "const rope = 'x'.repeat(9000) + 'y'.repeat(9000);"
    "eq(rope.charCodeAt(8999), 120, 'rope left');"
    "eq(rope.charCodeAt(9000), 121, 'rope right');"
    "eq(String.prototype.charCodeAt.call(123, 1), 50, 'number receiver');"
    "eq(String.prototype.charCodeAt.call(new String('q'), 0), 113, 'wrapper receiver');"
    "let threw = false; try { String.prototype.charCodeAt.call(null, 0); } catch (e) { threw = e instanceof TypeError; }"
    "eq(threw, true, 'null receiver');"
    /* fromCharCode */
    "eq(String.fromCharCode(65), 'A', 'one unit');"
    "eq(String.fromCharCode(0), '\\0', 'zero');"
    "eq(String.fromCharCode(255), '\\xff', 'latin-1 unit');"
    "eq(String.fromCharCode(0x20ac), '€', 'wide unit');"
    "eq(String.fromCharCode(-1), '\\uffff', 'negative wraps');"
    "eq(String.fromCharCode(0x10041), 'A', 'above 16 bits wraps');"
    "eq(String.fromCharCode(65.9), 'A', 'fraction');"
    "eq(String.fromCharCode('66'), 'B', 'string unit');"
    "eq(String.fromCharCode(0xd83d, 0xde00), '\\u{1F600}', 'pair');"
    "eq(String.fromCharCode(), '', 'no units');"
    "eq(String.fromCharCode(0x20ac).length, 1, 'wide length');"
    "eq(String.fromCharCode(97) + String.fromCharCode(0x20ac), 'a€', 'concat');"
    /* Math.imul */
    "eq(Math.imul(3, 4), 12, 'small');"
    "eq(Math.imul(-5, 12), -60, 'negative');"
    "eq(Math.imul(0x40000000, 4), 0, 'wraps to zero');"
    "eq(Math.imul(0x7fffffff, 2), -2, 'wraps negative');"
    "eq(Math.imul(-2147483648, -1), -2147483648, 'min times minus one');"
    "eq(Math.imul(0xffffffff, 5), -5, 'uint32 argument');"
    "eq(Math.imul(2.7, 3), 6, 'fraction');"
    "eq(Math.imul('4', 5), 20, 'string argument');"
    "eq(Math.imul(3), 0, 'missing argument');"
    "eq(Object.is(Math.imul(-1, 0), 0), true, 'no negative zero');"
    /* native calls through every call opcode */
    "const o = { f: Math.max };"
    "eq(o.f(1, 7, 3), 7, 'method call');"
    "eq(Math.max.call(null, 2, 9), 9, 'call()');"
    "eq(((f) => f(4, 5))(Math.min), 4, 'plain call');"
    "eq([1, 2, 3].map(Math.abs).join(), '1,2,3', 'native callback');"
    "eq((() => Math.abs(-6))(), 6, 'tail call');"
    "eq(new Array(3).length, 3, 'native constructor');"
    "eq(typeof Symbol(), 'symbol', 'constructor without new');"
    "threw = false; try { Symbol.call(); new Symbol(); } catch (e) { threw = e instanceof TypeError; }"
    "eq(threw, true, 'new Symbol');"
    "return errs.join('; ') || 'ok';"
    "})()";

static uint64_t work_units(JSRuntime *rt)
{
    uint64_t counters[JS_WORK_COUNT];
    JS_GetWorkCounters(rt, counters);
    return counters[JS_WORK_UNITS];
}

/* A native call consumes one unit of interrupt budget, as it did through
   JS_CallInternal(): straight-line code (no loop, no bytecode call) with
   2,000 native calls must consume at least 2,000 units more than the same
   code without them. */
static int run_interrupt_budget(void)
{
    Realm realm;
    if (!realm_open(&realm)) return 1;
    enum { CALLS = 2000 };
    static const char call[] = "Math.abs(-1);String.fromCharCode(66);";
    static const char plain[] = "void 0;void 0;";
    size_t size = sizeof(call) * CALLS / 2 + 64;
    char *with_calls = malloc(size);
    char *without = malloc(size);
    int failed = with_calls == NULL || without == NULL;
    if (!failed) {
        with_calls[0] = without[0] = '\0';
        for (int i = 0; i < CALLS / 2; i++) {
            strcat(with_calls, call);
            strcat(without, plain);
        }
        uint64_t units[2];
        const char *sources[2] = { without, with_calls };
        for (int i = 0; i < 2 && !failed; i++) {
            uint64_t before = work_units(realm.rt);
            JSValue value = JS_Eval(realm.ctx, sources[i], strlen(sources[i]),
                                    "<budget>", JS_EVAL_TYPE_GLOBAL);
            units[i] = work_units(realm.rt) - before;
            failed |= JS_IsException(value);
            JS_FreeValue(realm.ctx, value);
        }
        if (!failed && units[1] - units[0] < CALLS) {
            fprintf(stderr, "interrupt budget: %llu units for %d native calls "
                    "(%llu without them)\n",
                    (unsigned long long) units[1], CALLS,
                    (unsigned long long) units[0]);
            failed = 1;
        }
    }
    free(with_calls);
    free(without);
    if (!realm_close(&realm)) {
        fprintf(stderr, "interrupt budget: memory not returned\n");
        failed = 1;
    }
    return failed;
}

/* An interrupt handler that stops the script must stop a loop of native
   calls at a poll, as it stops any other code. */
static int stop_after(JSRuntime *rt, void *opaque)
{
    (void) rt;
    unsigned *polls = opaque;
    return ++*polls >= 3;
}

static int run_interrupt_stop(void)
{
    Realm realm;
    if (!realm_open(&realm)) return 1;
    unsigned polls = 0;
    JS_SetInterruptHandler(realm.rt, stop_after, &polls);
    static const char source[] =
        "let n = 0; while (true) { n = Math.imul(n + 1, 3) & 0xffff; }";
    JSValue value = JS_Eval(realm.ctx, source, sizeof(source) - 1,
                            "<stop>", JS_EVAL_TYPE_GLOBAL);
    int failed = !JS_IsException(value) || polls < 3;
    if (failed)
        fprintf(stderr, "interrupt stop: polls=%u\n", polls);
    JS_FreeValue(realm.ctx, value);
    JS_FreeValue(realm.ctx, JS_GetException(realm.ctx));
    JS_SetInterruptHandler(realm.rt, NULL, NULL);
    if (!realm_close(&realm)) {
        fprintf(stderr, "interrupt stop: memory not returned\n");
        failed = 1;
    }
    return failed;
}

int main(void)
{
    int failed = 0;
    Realm realm;
    if (!realm_open(&realm)) return 1;
    failed |= expect_ok(realm.ctx, "<results>", results_source);
    if (!realm_close(&realm)) {
        fprintf(stderr, "results: memory not returned\n");
        failed = 1;
    }
    failed |= run_interrupt_budget();
    failed |= run_interrupt_stop();
    puts(failed ? "QuickJS native calls: FAIL" : "QuickJS native calls: PASS");
    return failed;
}
