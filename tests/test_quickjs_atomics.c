/* Atomics in the vendored QuickJS (atomics-no-block layer): no agent may
   suspend, so Atomics.wait throws a TypeError once its arguments are
   checked, and Atomics.notify, with nobody waiting, returns 0. Both still
   validate and convert their arguments in the specification's order, and
   the other operations work on shared and ordinary integer arrays. */
#include <stdio.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"

static const char atomics_source[] =
    "(function(){\n"
    "  const log = [];\n"
    "  function check(name, value, expected) {\n"
    "    if (value !== expected) log.push(name + '=' + String(value));\n"
    "  }\n"
    "  function thrown(f) {\n"
    "    try { f(); return 'none'; } catch (e) { return e.name + ':' + e.message; }\n"
    "  }\n"
    "  const shared = new Int32Array(new SharedArrayBuffer(16));\n"
    "  const shared64 = new BigInt64Array(new SharedArrayBuffer(16));\n"
    "  const plain = new Int32Array(4);\n"
    "  /* wait: arguments first, then the agent that cannot suspend. */\n"
    "  check('wait', thrown(() => Atomics.wait(shared, 0, 0, 0)),\n"
    "        'TypeError:cannot block in this thread');\n"
    "  check('wait-not-equal', thrown(() => Atomics.wait(shared, 0, 1)),\n"
    "        'TypeError:cannot block in this thread');\n"
    "  check('wait-64', thrown(() => Atomics.wait(shared64, 1, 0n, 5)),\n"
    "        'TypeError:cannot block in this thread');\n"
    "  check('wait-index', thrown(() => Atomics.wait(shared, 4, 0)).split(':')[0],\n"
    "        'RangeError');\n"
    "  check('wait-type', thrown(() => Atomics.wait(new Int16Array(new SharedArrayBuffer(8)), 0, 0)).split(':')[0],\n"
    "        'TypeError');\n"
    "  const order = [];\n"
    "  check('wait-order', thrown(() => Atomics.wait(shared, 0,\n"
    "      { valueOf() { order.push('value'); return 0; } },\n"
    "      { valueOf() { order.push('timeout'); return 1; } })),\n"
    "      'TypeError:cannot block in this thread');\n"
    "  check('wait-order-log', order.join(','), 'value,timeout');\n"
    "  check('wait-timeout-error', thrown(() => Atomics.wait(shared, 0, 0,\n"
    "      { valueOf() { throw new SyntaxError('t'); } })), 'SyntaxError:t');\n"
    "  /* notify: nobody waits. */\n"
    "  check('notify', Atomics.notify(shared, 0), 0);\n"
    "  check('notify-count', Atomics.notify(shared, 1, 5), 0);\n"
    "  check('notify-plain', Atomics.notify(plain, 0, 1), 0);\n"
    "  check('notify-64', Atomics.notify(shared64, 0), 0);\n"
    "  check('notify-index', thrown(() => Atomics.notify(shared, 9)).split(':')[0],\n"
    "        'RangeError');\n"
    "  check('notify-count-error', thrown(() => Atomics.notify(shared, 0,\n"
    "      { valueOf() { throw new SyntaxError('c'); } })), 'SyntaxError:c');\n"
    "  /* The other operations. */\n"
    "  check('add', Atomics.add(shared, 0, 5), 0);\n"
    "  check('sub', Atomics.sub(shared, 0, 2), 5);\n"
    "  check('exchange', Atomics.exchange(shared, 0, 9), 3);\n"
    "  check('cmpxchg', Atomics.compareExchange(shared, 0, 9, 4), 9);\n"
    "  check('load', Atomics.load(shared, 0), 4);\n"
    "  check('store', Atomics.store(plain, 2, 7), 7);\n"
    "  check('or', Atomics.or(plain, 2, 8), 7);\n"
    "  check('and', Atomics.and(plain, 2, 12), 15);\n"
    "  check('xor', Atomics.xor(plain, 2, 1), 12);\n"
    "  check('load-plain', Atomics.load(plain, 2), 13);\n"
    "  check('add-64', Atomics.add(shared64, 1, 3n), 0n);\n"
    "  check('load-64', Atomics.load(shared64, 1), 3n);\n"
    "  check('lock-free', Atomics.isLockFree(4), true);\n"
    "  check('pause', Atomics.pause(), undefined);\n"
    "  return log.length ? log.join(' ') : 'ok';\n"
    "})()";

int main(void)
{
    Budget budget;
    budget_init(&budget, 8u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (pool == NULL) return 1;
    JSRuntime *runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    JSContext *context = runtime ? JS_NewContext(runtime) : NULL;
    int failed = 1;
    if (context != NULL) {
        JSValue value = JS_Eval(context, atomics_source,
                                sizeof(atomics_source) - 1, "atomics",
                                JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value)) {
            JSValue exception = JS_GetException(context);
            const char *text = JS_ToCString(context, exception);
            fprintf(stderr, "atomics: exception %s\n", text ? text : "?");
            JS_FreeCString(context, text);
            JS_FreeValue(context, exception);
        } else {
            const char *text = JS_ToCString(context, value);
            if (text != NULL && strcmp(text, "ok") == 0)
                failed = 0;
            else
                fprintf(stderr, "atomics: %s\n", text ? text : "?");
            JS_FreeCString(context, text);
        }
        JS_FreeValue(context, value);
        JS_FreeContext(context);
    }
    if (runtime) JS_FreeRuntime(runtime);
    budget_quickjs_pool_destroy(pool);
    if (budget.current != 0) {
        fprintf(stderr, "atomics: %zu bytes left after teardown\n",
                budget.current);
        failed = 1;
    }
    if (!failed) puts("QuickJS Atomics: PASS");
    return failed;
}
