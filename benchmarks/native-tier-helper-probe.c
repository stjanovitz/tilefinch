/* Ahead-of-time helper ABI fixture, deliberately excluded from normal builds.
 * This hand-lowered synthetic program is NOT a JIT or a browser fast path.
 * See docs/engineering/NATIVE_TIER_INVESTIGATION.md for measurement limits. */
#ifdef TILEFINCH_HELPER_QUICKJS_SOURCE
#include TILEFINCH_HELPER_QUICKJS_SOURCE
#else
#include "../third_party/quickjs/quickjs.c"
#endif
#include "tilefinch/budget_quickjs.h"
#include <time.h>

#ifdef __PSP__
#include <pspkernel.h>
#include <psppower.h>
PSP_MODULE_INFO("Tilefinch Helper Probe", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
PSP_HEAP_SIZE_KB(16384);
/* Same workload and alternating lanes, shorter repetitions on hardware.
   These constants change duration, not semantics or poll frequency. */
#define PROBE_ITERATIONS 20000
#define PROBE_SAMPLES 5
#else
#define PROBE_ITERATIONS 200000
#define PROBE_SAMPLES 8
#endif

static JSAtom enabled_atom, value_atom;

static JSValue force_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}

static int interrupt_now(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (*(unsigned *)opaque)++;
    return 1;
}

static force_inline JSValue probe_field_inline(JSContext *ctx, JSValueConst receiver, JSAtom atom)
{
    if (JS_IsObject(receiver)) {
        JSObject *object = JS_VALUE_GET_OBJ(receiver);
        for (unsigned depth = 0; depth < 64; depth++) {
            JSProperty *property;
            JSShapeProperty *shape = find_own_property(&property, object, atom);
            if (shape) {
                if (!(shape->flags & JS_PROP_TMASK)) return JS_DupValue(ctx, property->u.value);
                if ((shape->flags & JS_PROP_TMASK) == JS_PROP_GETSET)
                    return js_call_property_getter(ctx, property, receiver);
                break;
            }
            if (object->is_exotic) break;
            object = object->shape->proto;
            if (!object) return JS_UNDEFINED;
        }
    }
    return JS_GetPropertyInternal(ctx, receiver, atom, receiver, 0);
}

static no_inline JSValue probe_field_helper(JSContext *ctx, JSValueConst receiver, JSAtom atom)
{
    return probe_field_inline(ctx, receiver, atom);
}

static no_inline JSValue probe_call_helper(JSContext *ctx, JSValueConst callback, JSValue argument)
{
    JSValue result = JS_CallInternal(ctx, callback, JS_UNDEFINED, JS_UNDEFINED, 1, &argument, 0);
    JS_FreeValue(ctx, argument);
    return result;
}

/* The loop is the lowering of the synthetic JS function below, not a shortcut
   selected by a page name. Owned result/arguments are materialized across every
   semantic helper. Existing QuickJS helpers handle getters, proxies and throws.
   This fixture does NOT implement JS stack traces or a production native frame. */
#define PROBE_LOOP(name, field) \
static no_inline JSValue name(JSContext *ctx, JSValueConst object, JSValueConst callback, int count) \
{ \
    JSValue result = JS_UNDEFINED; \
    if (count < 0 || count > 1000000) return JS_ThrowRangeError(ctx, "probe count"); \
    if (js_poll_interrupts(ctx) || js_poll_interrupts(ctx)) goto fail; \
    for (int i = 0; i < count; i++) { \
        if (js_poll_interrupts(ctx)) goto fail; \
        JSValue enabled = field(ctx, object, enabled_atom); \
        if (JS_IsException(enabled)) goto fail; \
        int truth = JS_ToBoolFree(ctx, enabled); \
        if (js_poll_interrupts(ctx)) goto fail; \
        if (truth) { \
            JSValue argument = field(ctx, object, value_atom); \
            if (JS_IsException(argument)) goto fail; \
            JSValue next = probe_call_helper(ctx, callback, argument); \
            if (JS_IsException(next)) goto fail; \
            JS_FreeValue(ctx, result); \
            result = next; \
        } \
    } \
    if (js_poll_interrupts(ctx)) goto fail; \
    return result; \
fail: \
    JS_FreeValue(ctx, result); \
    return JS_EXCEPTION; \
}

PROBE_LOOP(probe_inline, probe_field_inline)
PROBE_LOOP(probe_helpers, probe_field_helper)

static JSValue eval(JSContext *ctx, const char *source)
{
    return JS_Eval(ctx, source, strlen(source), "<native-abi-fixture>", 0);
}

static uint64_t clock_ns(void)
{
#ifdef __PSP__
    return (uint64_t)sceKernelGetSystemTimeWide() * 1000u;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
#endif
}

static JSValue execute(JSContext *ctx, unsigned kind, JSValue fn, JSValue object, JSValue callback, int count)
{
    if (kind == 1) return probe_inline(ctx, object, callback, count);
    if (kind == 2) return probe_helpers(ctx, object, callback, count);
    JSValue args[] = {object, callback, JS_NewInt32(ctx, count)};
    return JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
}

static int check_result(JSContext *ctx, JSValue result, const char *expected)
{
    JSValue text = result;
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(ctx);
        text = JS_GetPropertyStr(ctx, exception, "message");
        JS_FreeValue(ctx, exception);
    }
    const char *actual = JS_ToCString(ctx, text);
    int okay = actual && !strcmp(actual, expected);
    if (!okay) fprintf(stderr, "mismatch expected=%s got=%s\n", expected, actual ? actual : "<exception>");
    JS_FreeCString(ctx, actual);
    JS_FreeValue(ctx, text);
    return okay;
}

static int helper_probe_main(void)
{
    uint64_t elapsed_ns[4][3][PROBE_SAMPLES];
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *pool = budget_quickjs_pool_create(&budget);
    if (!pool) return 1;
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), pool);
    if (!rt) return 2;
    JS_SetMemoryLimit(rt, 8u * 1024u * 1024u);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) return 3;
    JSValue global = JS_GetGlobalObject(ctx);
    if (JS_SetPropertyStr(ctx, global, "probeGC", JS_NewCFunction(ctx, force_gc, "probeGC", 0)) < 0) return 12;
    JS_FreeValue(ctx, global);
    enabled_atom = JS_NewAtom(ctx, "enabled");
    value_atom = JS_NewAtom(ctx, "value");
    JSValue fn = eval(ctx, "(function(object,callback,count){let result;for(let i=0;i<count;i++){if(object.enabled)result=callback(object.value);}return result;})");
    if (JS_IsException(fn)) return 4;
    static const uint8_t sizes[256] = {
#define DEF(id, size, pop, push, fmt) size,
#define def(id, size, pop, push, fmt)
#include "../third_party/quickjs/quickjs-opcode.h"
    };
    JSFunctionBytecode *bytecode = JS_VALUE_GET_OBJ(fn)->u.func.function_bytecode;
    unsigned instruction_count = 0;
    for (unsigned offset = 0; offset < (unsigned)bytecode->byte_code_len;) {
        unsigned size = sizes[bytecode->byte_code_buf[offset]];
        if (!size || size > (unsigned)bytecode->byte_code_len-offset || instruction_count>=1024) return 13;
        offset += size;
        instruction_count++;
    }
    printf("fixture-bytecode bytes=%d instructions=%u\n", bytecode->byte_code_len, instruction_count);
    const char *objects[] = {
        "({enabled:true,value:7})",
        "Object.create({enabled:true,value:7})",
        "({get enabled(){return true},get value(){return 7}})",
        "new Proxy({enabled:true,value:7},{get(t,k,r){return Reflect.get(t,k,r)}})",
        "({enabled:false,get value(){throw Error('must not read')}})",
        "({get enabled(){throw Error('enabled-error')}})",
        "({enabled:true,get value(){throw Error('value-error')}})",
        "({enabled:true,value:7})"
    };
    const char *expected[] = {"8", "8", "8", "8", "undefined", "enabled-error", "value-error", "call-error"};
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned scene = 0; scene < 8; scene++) {
            JSValue object = eval(ctx, objects[scene]);
            JSValue callback = eval(ctx, scene == 7 ? "(x=>{throw Error('call-error')})" : "(x=>x+1)");
            if (JS_IsException(object) || JS_IsException(callback)) return 5;
            if (!check_result(ctx, execute(ctx, kind, fn, object, callback, 7), expected[scene])) return 6;
            JS_FreeValue(ctx, object);
            JS_FreeValue(ctx, callback);
        }
    }
    puts("semantics=pass cases=24");
    const char *factories[] = {
        "(()=>{let o={enabled:true,value:0};return {object:o,callback(x){o.value=x+1;return o.value},describe(){return ''+o.value}}})()",
        "(()=>{let o={enabled:true,value:7},calls=0;return {object:o,callback(x){calls++;o.enabled=false;return x},describe(){return ''+calls}}})()",
        "(()=>{let log='',o={get enabled(){log+='e';return true},get value(){log+='v';return 7}};return {object:o,callback(x){log+='c';return x+1},describe(){return log}}})()",
        "(()=>{let o={enabled:true,value:7},calls=0;return {object:o,callback(x){calls++;if(calls===3)throw Error('third-call');return x},describe(){return ''+calls}}})()",
        "(()=>{let o={enabled:true,value:{n:7}},calls=0;return {object:o,callback(x){calls++;return {n:x.n+calls}},describe(){return ''+calls}}})()"
    };
    const char *results[] = {"7", "7", "8", "third-call", "[object Object]"};
    const char *states[] = {"7", "1", "evcevcevcevcevcevcevc", "3", "7"};
    for (unsigned kind = 0; kind < 3; kind++) {
        for (unsigned scene = 0; scene < 5; scene++) {
            JSValue pack = eval(ctx, factories[scene]);
            if (JS_IsException(pack)) return 9;
            JSValue object = JS_GetPropertyStr(ctx, pack, "object");
            JSValue callback = JS_GetPropertyStr(ctx, pack, "callback");
            JSValue describe = JS_GetPropertyStr(ctx, pack, "describe");
            if (!check_result(ctx, execute(ctx, kind, fn, object, callback, 7), results[scene])) return 10;
            if (!check_result(ctx, JS_Call(ctx, describe, pack, 0, NULL), states[scene])) return 11;
            JS_FreeValue(ctx, object);
            JS_FreeValue(ctx, callback);
            JS_FreeValue(ctx, describe);
            JS_FreeValue(ctx, pack);
            JS_RunGC(rt);
        }
    }
    puts("reentrancy=pass cases=15");
    for (unsigned kind = 0; kind < 3; kind++) {
        JSValue object = eval(ctx, "({enabled:true,value:{n:7}})");
        JSValue callback = eval(ctx, "(x=>{probeGC();return x.n+1})");
        if (!check_result(ctx, execute(ctx, kind, fn, object, callback, 7), "8")) return 14;
        unsigned polls = 0;
        ctx->interrupt_counter = 1;
        JS_SetInterruptHandler(rt, interrupt_now, &polls);
        JSValue result = execute(ctx, kind, fn, object, callback, 7);
        JS_SetInterruptHandler(rt, NULL, NULL);
        if (polls != 1 || !check_result(ctx, result, "interrupted")) return 15;
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, callback);
    }
    puts("gc-and-interrupt=pass cases=6");
    for (unsigned scene = 0; scene < 4; scene++) {
        JSValue object = eval(ctx, objects[scene]);
        JSValue callback = eval(ctx, "(x=>x+1)");
        for (unsigned repetition = 0; repetition <= PROBE_SAMPLES; repetition++) {
            for (unsigned position = 0; position < 3; position++) {
                unsigned kind = (position + repetition) % 3;
                uint64_t begin = clock_ns();
                JSValue result = execute(ctx, kind, fn, object, callback, PROBE_ITERATIONS);
                uint64_t elapsed = clock_ns() - begin;
                if (!check_result(ctx, result, "8")) return 7;
                if (repetition) elapsed_ns[scene][kind][repetition - 1] = elapsed;
                printf("timing scene=%u kind=%u rep=%u ns=%llu\n", scene, kind, repetition, (unsigned long long)elapsed);
            }
        }
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, callback);
    }
    for (unsigned scene = 0; scene < 4; scene++) {
        uint64_t medians[3];
        for (unsigned kind = 0; kind < 3; kind++) {
            uint64_t *samples = elapsed_ns[scene][kind];
            for (unsigned i = 1; i < PROBE_SAMPLES; i++) {
                uint64_t value = samples[i];
                unsigned j = i;
                while (j && value < samples[j - 1]) {
                    samples[j] = samples[j - 1];
                    j--;
                }
                samples[j] = value;
            }
            medians[kind] = samples[(PROBE_SAMPLES - 1) / 2] +
                (samples[PROBE_SAMPLES / 2] - samples[(PROBE_SAMPLES - 1) / 2]) / 2;
        }
        printf("summary scene=%u interpreter-ns=%llu inline-ns=%llu helpers-ns=%llu\n",
            scene, (unsigned long long)medians[0], (unsigned long long)medians[1],
            (unsigned long long)medians[2]);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeAtom(ctx, enabled_atom);
    JS_FreeAtom(ctx, value_atom);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (!budget_quickjs_pool_destroy(pool) || budget.current != 0) return 8;
    puts("budget=0");
    return 0;
}

int main(void)
{
#ifdef __PSP__
    /* Private host0 evidence only: no network, audio or Memory Stick writes.
       Return to PSPLink with the module inert; the host unloads it afterward. */
    if (!freopen("host0:/native-helper-probe.txt", "w", stdout)) return 16;
    setvbuf(stdout, NULL, _IONBF, 0); /* do not lose the last completed sample */
    if (scePowerSetClockFrequency(333, 333, 166) < 0) {
        puts("helper-probe outcome=fail clock-refused");
        fflush(stdout);
        return 17;
    }
    printf("helper-probe platform=psp cpu-mhz=%d iterations=%u samples=%u\n",
        scePowerGetCpuClockFrequencyInt(), PROBE_ITERATIONS, PROBE_SAMPLES);
#endif
    int result = helper_probe_main();
    printf("helper-probe outcome=%s status=%d\n", result ? "fail" : "pass", result);
    fflush(stdout);
    return result;
}
