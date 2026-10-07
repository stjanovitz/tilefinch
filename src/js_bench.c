/* JavaScript engine micro-benchmark. See include/tilefinch/js_bench.h.

   Every kernel is a JavaScript function of one argument (the iteration
   count) compiled once and called `repeat` times; the best wall time on the
   platform monotonic clock is reported with the engine's own work counters
   (interrupt-budget units, and float64 boxes in an op-count engine). The
   kernels isolate one engine cost each so that a per-kernel device/host
   ratio says which costs the device pays disproportionately: a kernel
   whose ratio matches the plain integer loop costs what the clock ratio
   and cache misses explain; one far above it names a device-specific
   expense (software double arithmetic, allocator accounting, syscalls).

   Allocation-heavy kernels run twice, on the production allocator (the
   Budget-charged QuickJS pool) and on the engine's default malloc, so the
   pool's per-allocation cost is measured rather than argued. */
#include "tilefinch/js_bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/platform.h"
#include "tilefinch/script_lazy.h"

#define JS_BENCH_LINE_MAX 320
#define JS_BENCH_TRACE_RECORDS_MAX 4096
#define JS_BENCH_TRACE_BODY_MAX (4u * 1024u * 1024u)
#define JS_BENCH_SELECTION_MAX 512u

typedef enum {
    JS_BENCH_ALLOC_POOL,
    JS_BENCH_ALLOC_MALLOC
} JsBenchAllocator;

typedef struct {
    const char *name;
    /* Iterations at scale 1. */
    uint64_t iterations;
    /* Also run on the default malloc allocator. */
    int both_allocators;
    const char *source;
} JsBenchKernel;

/* Each source evaluates to a function of n. `|0` keeps accumulators
   integer where the kernel is not about doubles, so an int kernel never
   silently becomes a float64 kernel through overflow. */
static const JsBenchKernel js_bench_kernels[] = {
    {"int_loop", 300000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ s=(s+i)|0; } return s; })"},
    {"double_arith", 300000, 0,
     "(function(n){ let x=0.5; for(let i=0;i<n;i++){ x=x*1.0000001+0.25; }"
     " return x; })"},
    {"double_compare", 300000, 0,
     "(function(n){ let x=0.5,c=0; for(let i=0;i<n;i++){ x+=0.5;"
     " if(x>1000.5){x=0.5;c++;} } return c; })"},
    {"int_divide", 300000, 0,
     "(function(n){ let s=0; for(let i=1;i<=n;i++){ s=(s+((n/i)|0))|0; }"
     " return s; })"},
    {"mixed_int_double", 300000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ s+=i*0.5; } return s; })"},
    {"prop_get", 200000, 0,
     "(function(n){ const o={a:1,b:2,c:3,d:4}; let s=0;"
     " for(let i=0;i<n;i++){ s=(s+o.a+o.b+o.c+o.d)|0; } return s; })"},
    {"prop_get_proto", 200000, 0,
     "(function(n){ const p={z:7}; const o=Object.create(p); o.x=1; let s=0;"
     " for(let i=0;i<n;i++){ s=(s+o.x+o.z)|0; } return s; })"},
    {"prop_get_poly", 200000, 0,
     "(function(n){ const v=[{a:1,b:2},{b:2,a:3},{c:0,a:5,b:1},{a:7,d:1,b:0}];"
     " let s=0; for(let i=0;i<n;i++){ const o=v[i&3]; s=(s+o.a+o.b)|0; }"
     " return s; })"},
    {"prop_set", 200000, 0,
     "(function(n){ const o={a:1,b:2}; for(let i=0;i<n;i++){ o.a=i; o.b=i; }"
     " return o.a; })"},
    /* Matched ownership/working-set probes. Setup is outside the timed
       function, and each read/store is executed even when its value is
       overwritten. The ring indices and object shapes are identical;
       only receiver/value working sets differ. These are synthetic costs,
       not a measurement of refcount or cache misses in a page callback. */
#define OWN_READ_LOOP \
     "return function(n){let q;for(let i=0;i<n;i++){const o=v[i&1023];" \
     "q=o.a;q=o.b;q=o.c;q=o.d;}" \
     "if(q!==v[(n-1)&1023].d)throw Error('own-read result');return 1;};})()"
    {"own_hot_int", 200000, 0,
     "(function(){const o={a:1,b:2,c:3,d:4},v=new Array(1024).fill(o);"
     OWN_READ_LOOP},
    {"own_ring_int", 200000, 0,
     "(function(){const v=new Array(1024);for(let i=0;i<1024;i++)"
     "v[i]={a:1,b:2,c:3,d:4};" OWN_READ_LOOP},
    {"own_hot_ref", 200000, 0,
     "(function(){const a={},b={},c={},d={},o={a,b,c,d},"
     "v=new Array(1024).fill(o);" OWN_READ_LOOP},
    {"own_ring_ref", 200000, 0,
     "(function(){const a={},b={},c={},d={},v=new Array(1024);"
     "for(let i=0;i<1024;i++)v[i]={a,b,c,d};" OWN_READ_LOOP},
    {"own_ring_ref_scattered", 200000, 0,
     "(function(){const v=new Array(1024);for(let i=0;i<1024;i++)"
     "v[i]={a:{},b:{},c:{},d:{}};" OWN_READ_LOOP},
#undef OWN_READ_LOOP
#define BINDING_MOVE_LOOP \
     "return function(n){let a=seed,b=seed,c=seed,d=seed;" \
     "for(let i=0;i<n;i++){a=b;b=c;c=d;d=a;}" \
     "if(a!==seed||b!==seed||c!==seed||d!==seed)" \
     "throw Error('binding-move result');return 1;};})()"
    {"binding_move_int", 300000, 0,
     "(function(){const seed=7;" BINDING_MOVE_LOOP},
    {"binding_move_ref", 300000, 0,
     "(function(){const seed={};" BINDING_MOVE_LOOP},
#undef BINDING_MOVE_LOOP
    {"global_get", 200000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ s=(s+Math.PI|0)|0; }"
     " return s; })"},
    {"call", 200000, 0,
     "(function(n){ function f(a,b){ return (a+b)|0; } let s=0;"
     " for(let i=0;i<n;i++){ s=f(s,i); } return s; })"},
    {"method_call", 200000, 0,
     "(function(n){ const o={v:0, add(x){ this.v=(this.v+x)|0; }};"
     " for(let i=0;i<n;i++) o.add(i); return o.v; })"},
    {"closure_call", 200000, 0,
     "(function(n){ let k=3; const f=(x)=>(x+k)|0; let s=0;"
     " for(let i=0;i<n;i++){ s=f(s); } return s; })"},
    {"native_call", 200000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ s=(s+Math.abs(-i))|0; }"
     " return s; })"},
    {"alloc_objects", 100000, 1,
     "(function(n){ let last=null; for(let i=0;i<n;i++){"
     " last={a:i,b:i+1,c:last}; if((i&1023)===0) last=null; }"
     " return last===null?0:last.a; })"},
    /* alloc_objects without its 1,024-object chain: each object dies at
       once, so allocation reuses the same few hot lines. Against
       alloc_objects it separates the allocation path's own cost from the
       data-cache misses of a retained working set. */
    {"alloc_objects_hot", 100000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ const o={a:i,b:i+1,c:null};"
     " s=(s+o.a)|0; } return s; })"},
    {"alloc_arrays", 100000, 1,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ const a=[i,i+1,i+2];"
     " s=(s+a.length)|0; } return s; })"},
    {"closure_create", 100000, 1,
     "(function(n){ let f=null; for(let i=0;i<n;i++){ const k=i; f=()=>k; }"
     " return f(); })"},
    /* closure_create keeping its last 4,096 closures alive: the same code
       path plus one element store, over a working set far past the 16 KiB
       D-cache (each new closure reuses the block of one created 4,096
       iterations earlier). Device CPI here against closure_create's says
       whether data misses or the code path make closure creation slow. */
    {"closure_ring", 100000, 0,
     "(function(n){ const r=new Array(4096).fill(null); let f=null;"
     " for(let i=0;i<n;i++){ const k=i; f=()=>k; r[i&4095]=f; }"
     " return f(); })"},
    /* A function expression (a constructor, so it also gets the lazily
       created 'prototype') and an async arrow: closure creation with the
       other two property layouts of a new function object. */
    {"function_create", 100000, 0,
     "(function(n){ let f=null; for(let i=0;i<n;i++){ const k=i;"
     " f=function(){ return k; }; } return f(); })"},
    {"async_closure_create", 100000, 0,
     "(function(n){ let f=null; for(let i=0;i<n;i++){ const k=i;"
     " f=async()=>k; } return typeof f; })"},
    /* Array spread of a 16-element array: each element goes through the
       OP_append path into the new literal. */
    {"array_spread", 50000, 0,
     "(function(n){ const a=[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]; let s=0;"
     " for(let i=0;i<n;i++){ const b=[...a]; s=(s+b.length)|0; } return s; })"},
    /* An eight-property object literal that dies at once: every property
       is a shape transition (and, at sizes 2, 3, 4 and 6, a property-array
       reallocation). */
    {"object_literal8", 100000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ const o={a:i,b:1,c:2,"
     "d:3,e:4,f:5,g:6,h:7}; s=(s+o.h)|0; } return s; })"},
    /* The shape of chatgpt.com's string decoders: charCodeAt, Math.imul
       and String.fromCharCode, all with integer arguments (native calls
       with no float in or out). */
    {"xor_decode", 100000, 0,
     "(function(n){ const s='abcdefghijklmnop'; let h=0, out='';"
     " for(let i=0;i<n;i++){ const c=s.charCodeAt(i&15)^(i&31);"
     " h=Math.imul(h^c,16777619); if((i&63)===0) out='';"
     " out+=String.fromCharCode(c); } return (h^out.length)|0; })"},
    /* Object.defineProperty with descriptor literals, the way bundlers
       define exports (enumerable getters) and fields (data). */
    {"define_property", 50000, 0,
     "(function(n){ const g=function(){ return 1; }; let s=0;"
     " for(let i=0;i<n;i++){ const o={};"
     " Object.defineProperty(o,'a',{enumerable:true,get:g});"
     " Object.defineProperty(o,'b',{value:i,writable:true});"
     " s=(s+o.b+o.a)|0; } return s; })"},
    {"array_push_index", 100000, 1,
     "(function(n){ const a=[]; let s=0; for(let i=0;i<n;i++){ a.push(i);"
     " s=(s+a[a.length>>1])|0; if(a.length>=4096) a.length=0; } return s; })"},
    {"string_append", 100000, 1,
     "(function(n){ let s=''; for(let i=0;i<n;i++){ s+='ab';"
     " if(s.length>=4096) s=''; } return s.length; })"},
    {"string_concat_int", 50000, 1,
     "(function(n){ let c=0; for(let i=0;i<n;i++){ const s='k'+i;"
     " c=(c+s.length)|0; } return c; })"},
    {"string_ops", 50000, 0,
     "(function(n){ const s='the quick brown fox jumps over the lazy dog';"
     " let c=0; for(let i=0;i<n;i++){ c=(c+s.charCodeAt(i%40)"
     "+s.indexOf('lazy')+s.slice(4,9).length)|0; } return c; })"},
    {"number_to_string", 20000, 0,
     "(function(n){ let c=0; for(let i=0;i<n;i++){"
     " c=(c+String(i*1.5+0.25).length)|0; } return c; })"},
    {"string_to_number", 20000, 0,
     "(function(n){ let c=0; const t=['1.5','123.456','7e3','42'];"
     " for(let i=0;i<n;i++){ c+=Number(t[i&3]); } return c; })"},
    {"math", 50000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){"
     " s+=Math.floor(Math.sqrt(i)*1.5)+Math.max(i,3); } return s; })"},
    {"map_set_get", 50000, 1,
     "(function(n){ const m=new Map(); let s=0; for(let i=0;i<n;i++){"
     " m.set(i&1023,i); s=(s+m.get(i&511))|0; } return s; })"},
    {"json", 200, 1,
     "(function(n){ const o={a:[1,2,3,{b:'x',c:1.5}],d:'hello world',"
     "e:{f:{g:[true,false,null]}}}; let src=JSON.stringify(o);"
     " for(let i=0;i<6;i++) src='['+src+','+src+']'; let c=0;"
     " for(let i=0;i<n;i++){ c+=JSON.stringify(JSON.parse(src)).length; }"
     " return c; })"},
    {"regexp", 20000, 0,
     "(function(n){ const re=/([a-z]+)-(\\d+)/; let c=0;"
     " for(let i=0;i<n;i++){ const m=re.exec('item-'+(i&255)+' abc-42 tail');"
     " c=(c+m[2].length)|0; } return c; })"},
    {"date_now", 20000, 0,
     "(function(n){ let s=0; for(let i=0;i<n;i++){ s+=Date.now()&1; }"
     " return s; })"},
    {"typed_array_u8", 100000, 0,
     "(function(n){ const a=new Uint8Array(4096); let s=0;"
     " for(let i=0;i<n;i++){ a[i&4095]=i; s=(s+a[(i*7)&4095])|0; }"
     " return s; })"},
    {"typed_array_f64", 100000, 0,
     "(function(n){ const a=new Float64Array(1024); let s=0;"
     " for(let i=0;i<n;i++){ a[i&1023]=i*0.5; s+=a[(i*7)&1023]; }"
     " return s; })"},
    {"try_catch", 20000, 1,
     "(function(n){ let c=0; for(let i=0;i<n;i++){ try{ if(i&1) throw i;"
     " c=(c+1)|0; }catch(e){ c=(c+2)|0; } } return c; })"},
    {"spread_args", 50000, 1,
     "(function(n){ function f(a,b,c){ return (a+b+c)|0; } const v=[1,2,3];"
     " let s=0; for(let i=0;i<n;i++){ s=(s+f(...v))|0; } return s; })"},
};

#define JS_BENCH_KERNEL_COUNT \
    (sizeof(js_bench_kernels) / sizeof(js_bench_kernels[0]))

typedef struct {
    JsBenchEmit emit;
    void *opaque;
    const JsBenchOptions *options;
    int failed;
    uint64_t total_ns;
} JsBenchSession;

typedef struct {
    JsBenchAllocator allocator;
    Budget budget;
    BudgetQuickJSPool *pool;
    JSRuntime *runtime;
    JSContext *context;
} JsBenchRuntime;

/* A stand-in for the browser's interrupt handler: two monotonic clock
   reads and the millisecond division it performs on every poll, without the
   host's watchdog state. Counting polls gives the per-poll cost. */
typedef struct {
    uint64_t polls;
    uint64_t sink;
} JsBenchPollProbe;

static int js_bench_poll_handler(JSRuntime *runtime, void *opaque)
{
    JsBenchPollProbe *probe = opaque;
    (void) runtime;
    uint64_t now_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t now_ms = now_ns / UINT64_C(1000000);
    uint64_t after_ns = tilefinch_platform_monotonic_time_ns();
    probe->polls++;
    probe->sink += now_ms + (after_ns - now_ns);
    return 0;
}

static uint64_t js_bench_now_ns(void);

static void js_bench_emitf(JsBenchSession *session, const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void js_bench_emitf(JsBenchSession *session, const char *format, ...)
{
    char line[JS_BENCH_LINE_MAX];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    /* The monotonic clock at the report: a kernel ran between the previous
       line's at-ms and its own, which splits program-counter samples
       (tools/pc_profile_report.py --bench-log) into kernels. */
    if (length > 0 && (size_t) length < sizeof line) {
        (void) snprintf(line + length, sizeof line - (size_t) length,
                        " at-ms=%llu",
                        (unsigned long long) (js_bench_now_ns()
                                              / UINT64_C(1000000)));
    }
    session->emit(session->opaque, line);
}

static const char *js_bench_allocator_name(JsBenchAllocator allocator)
{
    return allocator == JS_BENCH_ALLOC_POOL ? "pool" : "malloc";
}

/* QuickJS resolves a module's static imports inside the JS_Eval that
   compiles it, even compile-only. Every import resolves to an empty
   module, so a record compiles whatever it imports and only its own
   source is timed (an empty module costs a few microseconds). */
static JSModuleDef *js_bench_stub_module_loader(JSContext *context,
                                                const char *module_name,
                                                void *opaque)
{
    (void) opaque;
    JSValue value = JS_Eval(context, "", 0, module_name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) return NULL;
    JSModuleDef *module = JS_VALUE_GET_PTR(value);
    JS_FreeValue(context, value);
    return module;
}

static int js_bench_runtime_open(JsBenchRuntime *bench,
                                 JsBenchAllocator allocator,
                                 size_t memory_limit)
{
    memset(bench, 0, sizeof *bench);
    bench->allocator = allocator;
    if (allocator == JS_BENCH_ALLOC_POOL) {
        budget_init(&bench->budget, memory_limit + memory_limit / 2u);
        bench->pool = budget_quickjs_pool_create(&bench->budget);
        if (bench->pool == NULL) return -1;
        bench->runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(),
                                        bench->pool);
    } else {
        bench->runtime = JS_NewRuntime();
    }
    if (bench->runtime == NULL) return -1;
    JS_SetMemoryLimit(bench->runtime, memory_limit);
    /* The browser's device guard (runtime_creation.inc). */
    JS_SetMaxStackSize(bench->runtime, 1024u * 1024u);
    JS_SetModuleLoaderFunc(bench->runtime, NULL, js_bench_stub_module_loader,
                           NULL);
    bench->context = JS_NewContext(bench->runtime);
    if (bench->context == NULL) return -1;
    return 0;
}

static void js_bench_runtime_close(JsBenchRuntime *bench)
{
    if (bench->context != NULL) JS_FreeContext(bench->context);
    if (bench->runtime != NULL) JS_FreeRuntime(bench->runtime);
    if (bench->pool != NULL) (void) budget_quickjs_pool_destroy(bench->pool);
    memset(bench, 0, sizeof *bench);
}

static uint64_t js_bench_now_ns(void)
{
    return tilefinch_platform_monotonic_time_ns();
}


static int js_bench_selected(const JsBenchOptions *options, const char *name)
{
    const char *only = options->only;
    size_t length = strlen(name);
    if (only == NULL || only[0] == '\0') return 1;
    while (*only != '\0') {
        const char *end = strchr(only, ',');
        size_t span = end == NULL ? strlen(only) : (size_t) (end - only);
        if (span == length && memcmp(only, name, length) == 0) return 1;
        if (end == NULL) break;
        only = end + 1;
    }
    return 0;
}

static int js_bench_selection_name(const char *text, size_t length,
                                    const char *name)
{
    return strlen(name) == length && memcmp(text, name, length) == 0;
}

/* Refuse the whole filter before running anything. Otherwise a typo can
   produce a successful empty timing, or silently omit part of an A/B. */
static int js_bench_selection_valid(const JsBenchOptions *options)
{
    const char *text = options->only;
    if (text == NULL || text[0] == '\0') return 1;
    size_t length = 0;
    while (length <= JS_BENCH_SELECTION_MAX && text[length] != '\0')
        length++;
    if (length > JS_BENCH_SELECTION_MAX) return 0;
    size_t start = 0;
    for (size_t end = 0; end <= length; end++) {
        if (end != length && text[end] != ',') continue;
        size_t span = end - start;
        if (span == 0) return 0;
        int known = 0;
        for (size_t at = 0; at < JS_BENCH_KERNEL_COUNT; at++) {
            if (js_bench_selection_name(text + start, span,
                                        js_bench_kernels[at].name)) {
                known = 1;
                break;
            }
        }
        if (!known)
            known = js_bench_selection_name(text + start, span, "poll")
                || js_bench_selection_name(text + start, span, "gc")
                || js_bench_selection_name(text + start, span,
                                           "compile_synthetic");
        if (!known && (js_bench_selection_name(text + start, span,
                                                "compile_trace")
                       || js_bench_selection_name(text + start, span,
                                                  "compile_peak")))
            known = options->trace_dir != NULL
                && options->trace_dir[0] != '\0';
        if (!known) return 0;
        start = end + 1u;
    }
    return 1;
}

static uint64_t js_bench_iterations(const JsBenchOptions *options,
                                    uint64_t base)
{
    if (options->scale == 0) return 1;
    return base * options->scale;
}

static void js_bench_report_exception(JsBenchSession *session,
                                      JSContext *context, const char *name)
{
    JSValue exception = JS_GetException(context);
    const char *text = JS_ToCString(context, exception);
    js_bench_emitf(session, "tilefinch-js-bench: kernel=%s error=\"%s\"",
                   name, text == NULL ? "?" : text);
    if (text != NULL) JS_FreeCString(context, text);
    JS_FreeValue(context, exception);
    session->failed++;
}

/* Runs one kernel function `repeat` times on `bench`; reports the best. */
static void js_bench_run_kernel(JsBenchSession *session, JsBenchRuntime *bench,
                                const JsBenchKernel *kernel,
                                JSInterruptHandler *handler, void *opaque,
                                const char *suffix)
{
    JSContext *context = bench->context;
    uint64_t iterations = js_bench_iterations(session->options,
                                              kernel->iterations);
    JSValue function = JS_Eval(context, kernel->source, strlen(kernel->source),
                               kernel->name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(function)) {
        js_bench_report_exception(session, context, kernel->name);
        return;
    }
    JS_SetInterruptHandler(bench->runtime, handler, opaque);
    uint64_t best_ns = UINT64_MAX;
    uint64_t work_before[JS_WORK_COUNT], work_after[JS_WORK_COUNT];
    uint64_t best_work = 0, best_f64 = 0, best_polls = 0, best_ops = 0;
    uint64_t best_blocks = 0, best_fresh = 0;
    BudgetQuickJSActivity activity_before, activity_after;
    int okay = 1;
    for (unsigned attempt = 0; okay && attempt < session->options->repeat;
         attempt++) {
        JSValue argument = JS_NewInt64(context, (int64_t) iterations);
        (void) JS_GetWorkCounters(bench->runtime, work_before);
        budget_quickjs_pool_activity(bench->pool, &activity_before);
        uint64_t started_ns = js_bench_now_ns();
        JSValue result = JS_Call(context, function, JS_UNDEFINED, 1,
                                 &argument);
        uint64_t elapsed_ns = js_bench_now_ns() - started_ns;
        (void) JS_GetWorkCounters(bench->runtime, work_after);
        budget_quickjs_pool_activity(bench->pool, &activity_after);
        JS_FreeValue(context, argument);
        if (JS_IsException(result)) {
            js_bench_report_exception(session, context, kernel->name);
            okay = 0;
        }
        JS_FreeValue(context, result);
        if (okay && elapsed_ns < best_ns) {
            best_ns = elapsed_ns;
            best_work = work_after[JS_WORK_UNITS] - work_before[JS_WORK_UNITS];
            best_f64 = work_after[JS_WORK_FLOAT64_BOXES]
                       - work_before[JS_WORK_FLOAT64_BOXES];
            best_polls = work_after[JS_WORK_POLLS] - work_before[JS_WORK_POLLS];
            best_ops = work_after[JS_WORK_BYTECODE_OPS]
                       - work_before[JS_WORK_BYTECODE_OPS];
            best_blocks = activity_after.pool_blocks
                          - activity_before.pool_blocks;
            best_fresh = activity_after.pool_fresh_blocks
                         - activity_before.pool_fresh_blocks;
        }
    }
    JS_SetInterruptHandler(bench->runtime, NULL, NULL);
    JS_FreeValue(context, function);
    if (!okay) return;
    session->total_ns += best_ns;
    js_bench_emitf(session,
                   "tilefinch-js-bench: kernel=%s%s alloc=%s iters=%llu "
                   "us=%llu ns-per-iter=%llu work=%llu polls=%llu f64=%llu "
                   "ops=%llu pool-blocks=%llu pool-fresh=%llu",
                   kernel->name, suffix,
                   js_bench_allocator_name(bench->allocator),
                   (unsigned long long) iterations,
                   (unsigned long long) (best_ns / 1000u),
                   (unsigned long long) (best_ns / iterations),
                   (unsigned long long) best_work,
                   (unsigned long long) best_polls,
                   (unsigned long long) best_f64,
                   (unsigned long long) best_ops,
                   (unsigned long long) best_blocks,
                   (unsigned long long) best_fresh);
}

/* Builds a retained graph of `objects` small objects, then times a full
   collection over it; the per-object cost is the mark/sweep walk. */
static void js_bench_run_gc(JsBenchSession *session, JsBenchRuntime *bench)
{
    static const char source[] =
        "(function(n){ const root=new Array(n); let prev=null;"
        " for(let i=0;i<n;i++){ prev={a:i,b:'v'+(i&255),c:prev,d:[i]};"
        " root[i]=prev; } globalThis.__benchRoot=root; return n; })";
    JSContext *context = bench->context;
    uint64_t objects = js_bench_iterations(session->options, 40000);
    JSValue function = JS_Eval(context, source, sizeof source - 1, "gc_build",
                               JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(function)) {
        js_bench_report_exception(session, context, "gc");
        return;
    }
    JSValue argument = JS_NewInt64(context, (int64_t) objects);
    uint64_t build_started_ns = js_bench_now_ns();
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 1, &argument);
    uint64_t build_ns = js_bench_now_ns() - build_started_ns;
    JS_FreeValue(context, argument);
    JS_FreeValue(context, function);
    if (JS_IsException(result)) {
        js_bench_report_exception(session, context, "gc");
        JS_FreeValue(context, result);
        return;
    }
    JS_FreeValue(context, result);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(bench->runtime, &usage);
    uint64_t best_ns = UINT64_MAX;
    for (unsigned attempt = 0; attempt < session->options->repeat + 1u;
         attempt++) {
        uint64_t started_ns = js_bench_now_ns();
        JS_RunGC(bench->runtime);
        uint64_t elapsed_ns = js_bench_now_ns() - started_ns;
        if (elapsed_ns < best_ns) best_ns = elapsed_ns;
    }
    session->total_ns += best_ns + build_ns;
    js_bench_emitf(session,
                   "tilefinch-js-bench: kernel=gc alloc=%s objects=%llu "
                   "build-us=%llu us=%llu ns-per-object=%llu "
                   "heap-bytes=%lld heap-objects=%lld",
                   js_bench_allocator_name(bench->allocator),
                   (unsigned long long) objects,
                   (unsigned long long) (build_ns / 1000u),
                   (unsigned long long) (best_ns / 1000u),
                   (unsigned long long) (best_ns / objects),
                   (long long) usage.malloc_size, (long long) usage.obj_count);
    /* Release the graph so later kernels start from an empty heap. */
    JSValue global = JS_GetGlobalObject(context);
    (void) JS_SetPropertyStr(context, global, "__benchRoot", JS_UNDEFINED);
    JS_FreeValue(context, global);
    JS_RunGC(bench->runtime);
}

/* Compiles `source` as a module, compile only, on a fresh context so
   repeated compiles do not hit the module cache. */
static int js_bench_compile_once(JsBenchSession *session, JsBenchRuntime *bench,
                                 const char *name, const char *source,
                                 size_t length, uint64_t *elapsed_ns)
{
    JSContext *context = JS_NewContext(bench->runtime);
    if (context == NULL) return -1;
    uint64_t started_ns = js_bench_now_ns();
    JSValue value = JS_Eval(context, source, length, name,
                            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) {
        /* A classic script (a syntax error as a module, such as a
           top-level `return` or HTML comment): compile it as one. The
           failed module parse is part of the timed region, as the browser
           would also have to classify it. */
        JSValue exception = JS_GetException(context);
        JSValue name_value = JS_GetPropertyStr(context, exception, "name");
        const char *kind = JS_ToCString(context, name_value);
        int syntax = kind != NULL && strcmp(kind, "SyntaxError") == 0;
        if (kind != NULL) JS_FreeCString(context, kind);
        JS_FreeValue(context, name_value);
        if (syntax) {
            JS_FreeValue(context, exception);
            value = JS_Eval(context, source, length, name,
                            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
        } else {
            JS_Throw(context, exception);
        }
    }
    *elapsed_ns = js_bench_now_ns() - started_ns;
    int okay = !JS_IsException(value);
    if (!okay) {
        JSValue exception = JS_GetException(context);
        const char *text = JS_ToCString(context, exception);
        js_bench_emitf(session, "tilefinch-js-bench: compile=%s error=\"%.80s\"",
                       name, text == NULL ? "?" : text);
        if (text != NULL) JS_FreeCString(context, text);
        JS_FreeValue(context, exception);
    }
    JS_FreeValue(context, value);
    JS_FreeContext(context);
    return okay ? 0 : -1;
}

/* A synthetic bundle in the shape of minified application code: many
   small functions, property chains, object literals, numeric literals,
   string literals and a class, with distinct identifiers per copy so atom
   interning sees a realistic unique/repeat mix. About 120 KB at scale 1. */
static char *js_bench_synthetic_source(unsigned scale, size_t *length)
{
    static const char unit[] =
        "function f%u(a,b){const c=a.x+b.y*0.75;if(c>12.5){return {x:c,y:b.y,"
        "z:'v%u',w:[1,2,3.25]};}return a.k%u(c|0,\"s%u\")}"
        "class C%u{constructor(v){this.v%u=v;this.n=0;}get m(){return this.v%u"
        "*2}set m(x){this.v%u=x/2}run(q){let t=0;for(let i=0;i<q;i++){t+=this."
        "m+i%%7;}return t}}"
        "const o%u={a:1,b:2.5,c:'c%u',d:null,e:[f%u,C%u],f(){return this.a+this."
        "b},g:{h:{i:{j:%u}}}};export const e%u=o%u.g.h.i.j+o%u.f();\n";
    unsigned copies = scale == 0 ? 4u : 300u * scale;
    size_t capacity = (size_t) copies * (sizeof unit + 96u) + 1u;
    char *source = malloc(capacity);
    size_t at = 0;
    if (source == NULL) return NULL;
    for (unsigned copy = 0; copy < copies && at < capacity; copy++) {
        unsigned k = copy + 1u;
        int wrote = snprintf(source + at, capacity - at, unit, k, k, k, k, k,
                             k, k, k, k, k, k, k, k, k, k, k);
        if (wrote < 0 || (size_t) wrote >= capacity - at) break;
        at += (size_t) wrote;
    }
    source[at] = '\0';
    *length = at;
    return source;
}

static void js_bench_run_compile_synthetic(JsBenchSession *session,
                                           JsBenchRuntime *bench,
                                           uint32_t lazy_threshold,
                                           const char *label)
{
    size_t length = 0;
    char *source = js_bench_synthetic_source(session->options->scale, &length);
    if (source == NULL || length == 0) {
        free(source);
        session->failed++;
        return;
    }
    JS_SetLazyFunctionThreshold(bench->runtime, (unsigned) lazy_threshold);
    JS_SetStripInfo(bench->runtime, JS_STRIP_SOURCE);
    uint64_t best_ns = UINT64_MAX;
    for (unsigned attempt = 0; attempt < session->options->repeat; attempt++) {
        uint64_t elapsed_ns = 0;
        if (js_bench_compile_once(session, bench, "synthetic.js", source,
                                  length, &elapsed_ns) != 0) {
            session->failed++;
            free(source);
            return;
        }
        if (elapsed_ns < best_ns) best_ns = elapsed_ns;
    }
    session->total_ns += best_ns;
    js_bench_emitf(session,
                   "tilefinch-js-bench: kernel=compile_%s alloc=%s bytes=%zu "
                   "us=%llu ns-per-byte=%llu lazy=%u",
                   label, js_bench_allocator_name(bench->allocator), length,
                   (unsigned long long) (best_ns / 1000u),
                   (unsigned long long) (best_ns / length), (unsigned) lazy_threshold);
    free(source);
}

static int js_bench_meta_is_javascript(const char *path)
{
    FILE *file = fopen(path, "rb");
    char line[512];
    int javascript = 0;
    if (file == NULL) return -1;
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, "content-type=", 13) == 0) {
            javascript = strstr(line, "javascript") != NULL
                         || strstr(line, "ecmascript") != NULL;
            break;
        }
    }
    fclose(file);
    return javascript;
}

static char *js_bench_read_file(const char *path, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || (unsigned long) size > JS_BENCH_TRACE_BODY_MAX) {
        fclose(file);
        return NULL;
    }
    if (fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    char *buffer = malloc((size_t) size + 1u);
    if (buffer == NULL) { fclose(file); return NULL; }
    if (fread(buffer, 1, (size_t) size, file) != (size_t) size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(file);
    *length = (size_t) size;
    return buffer;
}

/* Compiles every JavaScript record of an HTTP trace directory. Reads are
   outside the timed region; each record compiles on a fresh context so the
   module registry does not grow across records. Records are compiled in
   index order, one runtime, like the page's own module loads. */
static void js_bench_run_compile_trace(JsBenchSession *session,
                                       JsBenchRuntime *bench,
                                       uint32_t lazy_threshold)
{
    const char *dir = session->options->trace_dir;
    char path[512];
    uint64_t total_ns = 0, total_bytes = 0, max_ns = 0;
    unsigned files = 0, failures = 0, unreadable = 0;
    JS_SetLazyFunctionThreshold(bench->runtime, (unsigned) lazy_threshold);
    JS_SetStripInfo(bench->runtime, JS_STRIP_SOURCE);
    for (unsigned index = 0; index < JS_BENCH_TRACE_RECORDS_MAX; index++) {
        (void) snprintf(path, sizeof path, "%s/%04u.meta", dir, index);
        int javascript = js_bench_meta_is_javascript(path);
        if (javascript < 0) break;
        if (!javascript) continue;
        (void) snprintf(path, sizeof path, "%s/%04u.body", dir, index);
        size_t length = 0;
        char *source = js_bench_read_file(path, &length);
        if (source == NULL || length == 0) {
            free(source);
            unreadable++;
            continue;
        }
        char name[32];
        (void) snprintf(name, sizeof name, "%04u.js", index);
        uint64_t elapsed_ns = 0;
        if (js_bench_compile_once(session, bench, name, source, length,
                                  &elapsed_ns) != 0) {
            failures++;
        } else {
            files++;
            total_ns += elapsed_ns;
            total_bytes += length;
            if (elapsed_ns > max_ns) max_ns = elapsed_ns;
            if (session->options->scale == 0 && files >= 3) {
                free(source);
                break;
            }
        }
        free(source);
    }
    session->total_ns += total_ns;
    JSLazyFunctionStats lazy;
    JS_GetLazyFunctionStats(bench->runtime, &lazy);
    js_bench_emitf(session,
                   "tilefinch-js-bench: kernel=compile_trace alloc=%s files=%u "
                   "failed=%u unreadable=%u bytes=%llu us=%llu ns-per-byte=%llu "
                   "max-us=%llu lazy=%u deferred=%llu deferred-bytes=%llu",
                   js_bench_allocator_name(bench->allocator), files, failures,
                   unreadable, (unsigned long long) total_bytes,
                   (unsigned long long) (total_ns / 1000u),
                   (unsigned long long)
                       (total_bytes == 0 ? 0 : total_ns / total_bytes),
                   (unsigned long long) (max_ns / 1000u), (unsigned) lazy_threshold,
                   (unsigned long long) lazy.deferred,
                   (unsigned long long) lazy.deferred_source_bytes);
    if (files == 0) session->failed++;
}

/* 1 when `source` must compile as a module (it is not a valid classic
   script: import/export or top-level await), 0 for a classic script, as the
   browser loads nearly every bundle. Classified on a throwaway runtime so
   the failed attempt does not raise the measured runtime's high-water
   marks. */
static int js_bench_source_is_module(const char *name, const char *source,
                                     size_t length, size_t memory_limit)
{
    JsBenchRuntime probe;
    int module = 0;
    if (js_bench_runtime_open(&probe, JS_BENCH_ALLOC_POOL, memory_limit)
        != 0) {
        js_bench_runtime_close(&probe);
        return -1;
    }
    JSValue value = JS_Eval(probe.context, source, length, name,
                            JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(probe.context);
        JSValue name_value = JS_GetPropertyStr(probe.context, exception,
                                               "name");
        const char *kind = JS_ToCString(probe.context, name_value);
        module = kind != NULL && strcmp(kind, "SyntaxError") == 0;
        if (kind != NULL) JS_FreeCString(probe.context, kind);
        JS_FreeValue(probe.context, name_value);
        JS_FreeValue(probe.context, exception);
    }
    JS_FreeValue(probe.context, value);
    js_bench_runtime_close(&probe);
    return module;
}

/* Compiles `source` once on a fresh pool runtime configured like a page
   realm and reports its high-water marks over the empty context. */
typedef struct {
    int ok;
    uint64_t us;
    size_t js_peak, js_retained, budget_peak, budget_retained;
    unsigned long long deferred, deferred_bytes;
} JsBenchCompilePeak;

static int js_bench_measure_compile(const char *name, const char *source,
                                    size_t length, int module,
                                    uint32_t lazy_threshold,
                                    size_t memory_limit,
                                    JsBenchCompilePeak *out)
{
    JsBenchRuntime bench;
    memset(out, 0, sizeof *out);
    if (js_bench_runtime_open(&bench, JS_BENCH_ALLOC_POOL, memory_limit)
        != 0) {
        js_bench_runtime_close(&bench);
        return -1;
    }
    JS_SetLazyFunctionThreshold(bench.runtime, (unsigned) lazy_threshold);
    JS_SetLazyFunctionPreparse(bench.runtime, 1);
    JS_SetStripInfo(bench.runtime,
                    module && length >= 8u * 1024u ? JS_STRIP_SOURCE : 0);
    JS_RunGC(bench.runtime);
    size_t budget_base = bench.budget.current;
    size_t js_base = budget_quickjs_pool_js_malloc_current(bench.pool);
    size_t budget_peak_before = bench.budget.peak;
    size_t js_peak_before = budget_quickjs_pool_js_malloc_peak(bench.pool);
    uint64_t started_ns = js_bench_now_ns();
    JSValue value = JS_Eval(bench.context, source, length, name,
                            (module ? JS_EVAL_TYPE_MODULE
                                    : JS_EVAL_TYPE_GLOBAL)
                                | JS_EVAL_FLAG_COMPILE_ONLY);
    out->us = (js_bench_now_ns() - started_ns) / 1000u;
    out->ok = !JS_IsException(value);
    size_t budget_peak = bench.budget.peak > budget_peak_before
        ? bench.budget.peak : budget_peak_before;
    size_t js_peak = budget_quickjs_pool_js_malloc_peak(bench.pool);
    if (js_peak < js_peak_before) js_peak = js_peak_before;
    out->js_peak = js_peak - js_base;
    out->budget_peak = budget_peak - budget_base;
    out->js_retained =
        budget_quickjs_pool_js_malloc_current(bench.pool) - js_base;
    out->budget_retained = bench.budget.current - budget_base;
    JSLazyFunctionStats lazy;
    JS_GetLazyFunctionStats(bench.runtime, &lazy);
    out->deferred = (unsigned long long) lazy.deferred;
    out->deferred_bytes = (unsigned long long) lazy.deferred_source_bytes;
    JS_FreeValue(bench.context, value);
    js_bench_runtime_close(&bench);
    return 0;
}

/* The compile working set of every JavaScript record of a trace, for the
   script admission model (docs/engineering/MEMORY_EXPERIMENTS.md, "Script
   admission by compile working set"). Each record compiles once on its own
   fresh pool runtime, the way the browser compiles a page script (lazy
   function threshold and preparsing on, modules of 8 KiB or more with
   their source stripped), so the pool's and the Budget's high-water marks
   belong to that one compile. Reported per record: the transient peak over
   the empty context (js-peak, budget-peak) and what stays after the compile
   with the compiled function still held (js-retained, budget-retained). */
static void js_bench_run_compile_peak(JsBenchSession *session,
                                      uint32_t lazy_threshold)
{
    const char *dir = session->options->trace_dir;
    char path[512];
    unsigned files = 0, failures = 0;
    for (unsigned index = 0; index < JS_BENCH_TRACE_RECORDS_MAX; index++) {
        (void) snprintf(path, sizeof path, "%s/%04u.meta", dir, index);
        int javascript = js_bench_meta_is_javascript(path);
        if (javascript < 0) break;
        if (!javascript) continue;
        (void) snprintf(path, sizeof path, "%s/%04u.body", dir, index);
        size_t length = 0;
        char *source = js_bench_read_file(path, &length);
        if (source == NULL || length == 0) {
            free(source);
            continue;
        }
        char name[32];
        (void) snprintf(name, sizeof name, "%04u.js", index);
        int module = js_bench_source_is_module(
            name, source, length, session->options->memory_limit);
        JsBenchCompilePeak whole;
        if (module < 0
            || js_bench_measure_compile(name, source, length, module,
                                        lazy_threshold,
                                        session->options->memory_limit,
                                        &whole) != 0) {
            free(source);
            failures++;
            continue;
        }
        if (whole.ok) files++; else failures++;
        js_bench_emitf(session,
                       "tilefinch-js-bench: kernel=compile_peak file=%s "
                       "kind=%s ok=%d bytes=%zu us=%llu js-peak=%zu "
                       "js-retained=%zu budget-peak=%zu budget-retained=%zu "
                       "deferred=%llu deferred-bytes=%llu",
                       name, module ? "module" : "classic", whole.ok, length,
                       (unsigned long long) whole.us, whole.js_peak,
                       whole.js_retained, whole.budget_peak,
                       whole.budget_retained, whole.deferred,
                       whole.deferred_bytes);
        /* A bundle the browser splits (a Webpack factory table or a
           resource-loader statement sequence) compiles one unit at a time;
           its working set is the largest unit's. */
        Budget plan_budget;
        budget_init(&plan_budget, 64u * 1024u * 1024u);
        ScriptLazyWebpackPlan webpack;
        ScriptResourceLoaderPlan loader;
        memset(&webpack, 0, sizeof webpack);
        memset(&loader, 0, sizeof loader);
        const char *unit_kind = NULL;
        size_t units = 0, unit_offset = 0, unit_length = 0, unit_bytes = 0;
        if (!module && script_lazy_webpack_plan_create(
                &plan_budget, source, length, &webpack)) {
            unit_kind = "webpack";
            units = webpack.factory_count;
            unit_bytes = webpack.factory_source_bytes;
            for (size_t at = 0; at < webpack.factory_count; at++) {
                if (webpack.factories[at].source_length > unit_length) {
                    unit_length = webpack.factories[at].source_length;
                    unit_offset = webpack.factories[at].source_offset;
                }
            }
        } else if (!module && script_resource_loader_plan_create(
                       &plan_budget, source, length, &loader)) {
            unit_kind = "loader";
            units = loader.statement_count;
            unit_bytes = loader.statement_source_bytes;
            for (size_t at = 0; at < loader.statement_count; at++) {
                if (loader.statements[at].source_length > unit_length) {
                    unit_length = loader.statements[at].source_length;
                    unit_offset = loader.statements[at].source_offset;
                }
            }
        }
        if (unit_kind != NULL && unit_length != 0) {
            /* A factory compiles as a parenthesized function expression;
               a loader statement as itself. */
            int wrap = strcmp(unit_kind, "webpack") == 0;
            char *unit = malloc(unit_length + 3u);
            if (unit != NULL) {
                size_t at = 0;
                if (wrap) unit[at++] = '(';
                memcpy(unit + at, source + unit_offset, unit_length);
                at += unit_length;
                if (wrap) unit[at++] = ')';
                unit[at] = '\0';
                JsBenchCompilePeak largest;
                if (js_bench_measure_compile(name, unit, at, 0,
                                             lazy_threshold,
                                             session->options->memory_limit,
                                             &largest) == 0) {
                    js_bench_emitf(session,
                                   "tilefinch-js-bench: kernel=compile_unit "
                                   "file=%s unit=%s units=%zu unit-bytes=%zu "
                                   "largest=%zu ok=%d us=%llu js-peak=%zu "
                                   "budget-peak=%zu",
                                   name, unit_kind, units, unit_bytes, at,
                                   largest.ok,
                                   (unsigned long long) largest.us,
                                   largest.js_peak, largest.budget_peak);
                }
                free(unit);
            }
        }
        script_lazy_webpack_plan_destroy(&webpack);
        script_resource_loader_plan_destroy(&loader);
        free(source);
    }
    js_bench_emitf(session,
                   "tilefinch-js-bench: kernel=compile_peak_summary files=%u "
                   "failed=%u", files, failures);
    if (files == 0) session->failed++;
}

void js_bench_default_options(JsBenchOptions *options)
{
    memset(options, 0, sizeof *options);
    options->scale = 1;
    options->repeat = 1;
    options->lazy_threshold = 128;
    options->memory_limit = 16u * 1024u * 1024u;
}

int js_bench_run(const JsBenchOptions *options, JsBenchEmit emit,
                 void *opaque)
{
    JsBenchSession session = {emit, opaque, options, 0, 0};
    JsBenchRuntime bench;
    unsigned repeat = options->repeat == 0 ? 1u : options->repeat;
    JsBenchOptions effective = *options;
    effective.repeat = repeat;
    session.options = &effective;
    js_bench_emitf(&session,
                   "tilefinch-js-bench: begin scale=%u repeat=%u "
                   "heap-limit=%zu lazy=%u trace=%s",
                   effective.scale, repeat, effective.memory_limit,
                   (unsigned) effective.lazy_threshold,
                   effective.trace_dir == NULL ? "-" : effective.trace_dir);
    if (effective.only != NULL)
        js_bench_emitf(&session, "tilefinch-js-bench: selected=%s",
                       effective.only);
    if (!js_bench_selection_valid(&effective)) {
        js_bench_emitf(&session, "tilefinch-js-bench: error=\"selection\"");
        return 1;
    }

    /* Pass 1: every kernel on the production allocator. */
    if (js_bench_runtime_open(&bench, JS_BENCH_ALLOC_POOL,
                              effective.memory_limit) != 0) {
        js_bench_emitf(&session, "tilefinch-js-bench: error=\"runtime\"");
        js_bench_runtime_close(&bench);
        return 1;
    }
    for (size_t at = 0; at < JS_BENCH_KERNEL_COUNT; at++) {
        const JsBenchKernel *kernel = &js_bench_kernels[at];
        if (!js_bench_selected(&effective, kernel->name)) continue;
        js_bench_run_kernel(&session, &bench, kernel, NULL, NULL, "");
    }
    if (js_bench_selected(&effective, "poll")) {
        /* The int loop again under a browser-shaped interrupt handler: the
           difference against the plain int_loop, per poll, is the cost of
           the clock reads and division the browser pays every 10,000
           units of work. */
        JsBenchPollProbe probe = {0, 0};
        js_bench_run_kernel(&session, &bench, &js_bench_kernels[0],
                            js_bench_poll_handler, &probe, "_polled");
        js_bench_emitf(&session,
                       "tilefinch-js-bench: kernel=poll handler-polls=%llu",
                       (unsigned long long) probe.polls);
    }
    if (js_bench_selected(&effective, "gc"))
        js_bench_run_gc(&session, &bench);
    if (js_bench_selected(&effective, "compile_synthetic")) {
        js_bench_run_compile_synthetic(&session, &bench,
                                       effective.lazy_threshold, "synthetic");
        js_bench_run_compile_synthetic(&session, &bench, 0, "synthetic_eager");
    }
    if (effective.trace_dir != NULL && effective.trace_dir[0] != '\0'
        && js_bench_selected(&effective, "compile_trace"))
        js_bench_run_compile_trace(&session, &bench, effective.lazy_threshold);
    js_bench_runtime_close(&bench);
    /* Opens its own runtime per record, so only on explicit request. */
    if (effective.trace_dir != NULL && effective.trace_dir[0] != '\0'
        && effective.only != NULL
        && js_bench_selected(&effective, "compile_peak"))
        js_bench_run_compile_peak(&session, effective.lazy_threshold);

    /* Pass 2: the allocation-bound kernels on the default allocator. */
    if (js_bench_runtime_open(&bench, JS_BENCH_ALLOC_MALLOC,
                              effective.memory_limit) != 0) {
        js_bench_emitf(&session, "tilefinch-js-bench: error=\"runtime-malloc\"");
        js_bench_runtime_close(&bench);
        return session.failed + 1;
    }
    for (size_t at = 0; at < JS_BENCH_KERNEL_COUNT; at++) {
        const JsBenchKernel *kernel = &js_bench_kernels[at];
        if (!kernel->both_allocators
            || !js_bench_selected(&effective, kernel->name))
            continue;
        js_bench_run_kernel(&session, &bench, kernel, NULL, NULL, "");
    }
    if (js_bench_selected(&effective, "gc"))
        js_bench_run_gc(&session, &bench);
    if (js_bench_selected(&effective, "compile_synthetic"))
        js_bench_run_compile_synthetic(&session, &bench,
                                       effective.lazy_threshold, "synthetic");
    js_bench_runtime_close(&bench);

    js_bench_emitf(&session, "tilefinch-js-bench: end failed=%d total-us=%llu",
                   session.failed,
                   (unsigned long long) (session.total_ns / 1000u));
    return session.failed;
}
