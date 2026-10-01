static bool install_named_function(JSContext *context, JSValue global,
                                   const char *property_name,
                                   const char *function_name,
                                   JSCFunction *function, int arguments)
{
#ifdef TILEFINCH_SCRIPT_SPLIT
    function = script_split_native(function);
#endif
    JSValue value = JS_NewCFunction(context, function, function_name,
                                    arguments);
    return !JS_IsException(value)
           && JS_SetPropertyStr(context, global, property_name, value) >= 0;
}

static uint64_t wall_time_ms(void)
{
    return tilefinch_platform_wall_time_ns() / UINT64_C(1000000);
}

uint64_t js_rt_monotonic_time_ns(void)
{
    return tilefinch_platform_monotonic_time_ns();
}

static uint64_t monotonic_time_ms(void)
{
    return js_rt_monotonic_time_ns() / UINT64_C(1000000);
}

static void watchdog_add_work(Watchdog *watchdog, size_t units)
{
    if (watchdog->result != NULL) {
        if (units > SIZE_MAX - watchdog->result->script_work_units) {
            watchdog->result->script_work_units = SIZE_MAX;
        } else {
            watchdog->result->script_work_units += units;
        }
    }
    if (units > SIZE_MAX - watchdog->slice_work_units) {
        watchdog->slice_work_units = SIZE_MAX;
    } else {
        watchdog->slice_work_units += units;
    }
}

static void watchdog_finish_slice(Watchdog *watchdog)
{
    if (watchdog->slice_started_ns == 0) return;
    uint64_t finished = js_rt_monotonic_time_ns();
    uint64_t elapsed_us = finished >= watchdog->slice_started_ns
        ? (finished - watchdog->slice_started_ns) / UINT64_C(1000) : 0;
    if (watchdog->result != NULL
        && elapsed_us > watchdog->result->script_max_slice_us) {
        watchdog->result->script_max_slice_us = elapsed_us;
        watchdog->result->script_max_slice_work_units =
            watchdog->slice_work_units;
    }
    /* A finished slice is idle until its caller explicitly resumes script
       work.  Leaving this timestamp armed would charge layout, paint, or
       event-loop idle time to the next JavaScript checkpoint. */
    watchdog->slice_started_ns = 0;
    watchdog->slice_work_units = 0;
}

static bool watchdog_cooperate(Watchdog *watchdog)
{
    watchdog_finish_slice(watchdog);
    if (watchdog->result != NULL) {
        if (watchdog->result->script_cooperative_yields != SIZE_MAX) {
            watchdog->result->script_cooperative_yields++;
        }
    }
    bool keep_going = tilefinch_platform_cooperate(
        "script", watchdog->result == NULL
                    ? watchdog->polls
                    : watchdog->result->script_work_units);
    if (keep_going) watchdog->slice_started_ns = js_rt_monotonic_time_ns();
    if (!keep_going) watchdog->interrupted = true;
    return keep_going;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* An interrupt is uncatchable: it unwinds page script without running its
   finally blocks, so page invariants kept by try/finally stay broken. */
static void watchdog_log_interrupt(const Watchdog *watchdog,
                                   const char *reason, uint64_t now_ms)
{
    static unsigned logged;
    if (logged >= 8u) return;
    logged++;
    char message[480];
    int length = snprintf(message, sizeof(message),
             "tilefinch-js-interrupt: reason=%s elapsed-ms=%llu polls=%zu "
             "stack=", reason,
             (unsigned long long) (now_ms >= watchdog->started_ms
                 ? now_ms - watchdog->started_ms : 0),
             watchdog->polls);
    const ScriptRuntime *owner = (const ScriptRuntime *) (const void *)
        ((const char *) watchdog - offsetof(ScriptRuntime, watchdog));
    for (int level = 0; level < 20 && length > 0
         && (size_t) length < sizeof(message) - 2u; level++) {
        JSAtom atom = JS_GetScriptOrModuleName(owner->context, level);
        if (atom == JS_ATOM_NULL) continue;
        const char *name = JS_AtomToCString(owner->context, atom);
        JS_FreeAtom(owner->context, atom);
        if (name == NULL) break;
        const char *base = strrchr(name, '/');
        length += snprintf(message + length, sizeof(message) - (size_t) length,
                           "%s<", base == NULL ? name : base + 1);
        JS_FreeCString(owner->context, name);
    }
    if (length > 0 && (size_t) length < sizeof(message) - 2u)
        snprintf(message + length, sizeof(message) - (size_t) length,
                 " sync-layouts=%zu/%llums max=%llums",
                 owner->bridge.synchronous_layout_calls,
                 (unsigned long long)
                     (owner->bridge.synchronous_layout_us / 1000u),
                 (unsigned long long)
                     (owner->bridge.synchronous_layout_max_us / 1000u));
    tilefinch_platform_log_message(message);
}

/* Names the scripts on the stack of a JavaScript entry that has run for a
   second: on the device there is no profiler, and a page that keeps the
   runtime busy otherwise shows up only as long advances. */
static void watchdog_log_long_task(Watchdog *watchdog, uint64_t elapsed_ms)
{
    static unsigned logged;
    if (logged >= 80u) return;
    logged++;
    ScriptRuntime *owner = (ScriptRuntime *) (void *)
        ((char *) watchdog - offsetof(ScriptRuntime, watchdog));
    JSContext *context = owner->context;
    char message[512];
    int length = snprintf(message, sizeof(message),
                          "tilefinch-js-sample: entry=%llums stack=",
                          (unsigned long long) elapsed_ms);
    char previous[64] = {0};
    for (int level = 0; level < 24 && length > 0
         && (size_t) length < sizeof(message) - 2u; level++) {
        /* Native frames (JSON.stringify, a host callback) have no script
           name; keep walking past them. */
        JSAtom atom = JS_GetScriptOrModuleName(context, level);
        if (atom == JS_ATOM_NULL) continue;
        const char *name = JS_AtomToCString(context, atom);
        JS_FreeAtom(context, atom);
        if (name == NULL) break;
        const char *base = strrchr(name, '/');
        base = base == NULL ? name : base + 1;
        if (strncmp(previous, base, sizeof(previous) - 1u) != 0) {
            length += snprintf(message + length,
                               sizeof(message) - (size_t) length,
                               "%s%.60s", previous[0] == 0 ? "" : "<",
                               base);
            snprintf(previous, sizeof(previous), "%s", base);
        }
        JS_FreeCString(context, name);
    }
    tilefinch_platform_log_message(message);
}
#endif

/* Rows per table. Keying by definition line keeps anonymous functions
   apart, so a page needs many; the profile is allocated only while
   profiling, from the page Budget (smaller on the PSP). */
#if defined(__PSP__)
#define SCRIPT_PROFILE_SLOTS 256u
#else
#define SCRIPT_PROFILE_SLOTS 1024u
#endif
#define SCRIPT_PROFILE_NATIVES 256u
#define SCRIPT_PROFILE_DEPTH 12
typedef struct {
    JSAtom file;
    JSAtom function;
    int line;
    int column;
    int defline;  /* definition line/column of the frame's function */
    int defcol;
    uint64_t us;
    uint32_t samples;
} ScriptProfileSlot;
struct ScriptProfile {
    ScriptProfileSlot self[SCRIPT_PROFILE_SLOTS];
    ScriptProfileSlot inclusive[SCRIPT_PROFILE_SLOTS];
    /* Microtask-phase attribution (phase 1 = promise job, 2 = checkpoint
       continuation): inclusive and self per function, and the outermost
       script frame of the job (its "caller"). */
    ScriptProfileSlot micro_inclusive[SCRIPT_PROFILE_SLOTS];
    ScriptProfileSlot micro_outer[SCRIPT_PROFILE_SLOTS];
    ScriptProfileSlot micro_self[SCRIPT_PROFILE_SLOTS];
    int phase;
    int micro_site; /* runtime_loop.inc call site: 1 entry, 2 network, 3 timer */
    uint64_t micro_us[3];
    uint32_t micro_samples[3];
    uint64_t micro_site_us[4];
    /* The last timed native, for TILEFINCH_JS_PROFILE_OUTLIER_US. */
    int last_native;
    uint64_t last_native_end_ns;
    uint64_t last_native_us;
    uint64_t last_native_inside_us;
    uint64_t last_ns;
    uint64_t total_us;
    uint64_t unframed_us;
    uint64_t unframed_pending_us;
    uint64_t unframed_compile_us;
    uint64_t unframed_checkpoint_us;
    uint64_t unframed_entry_us;
    uint64_t unframed_tail_us;
    uint64_t profiler_us;
    unsigned compile_depth;
    unsigned checkpoint_depth;
    bool returning_from_script;
    uint64_t dropped_us;
    /* Intervals too long to attribute to one stack frame. Keep these
       separate from sampled script time instead of silently losing them. */
    uint64_t long_gap_us;
    uint32_t long_gap_count;
    uint32_t samples;
    /* Collections, timed through JS_SetGCHook and kept out of the frame
       tables: otherwise a collection is charged to whichever frame's
       allocation happened to trigger it. */
    uint64_t gc_begin_ns;
    uint64_t gc_unsampled_us;
    uint64_t gc_us;
    uint64_t gc_max_us;
    uint32_t gc_count;
    /* Host natives, timed at the call (the sampler polls only in bytecode,
       so a long native was billed to whichever frame polled next). Self
       time excludes script the native called back into. */
    uint64_t native_us[SCRIPT_PROFILE_NATIVES];
    uint64_t native_self_us;
    uint64_t job_begin_ns; /* joins opt-in samples to one promise execution */
    uint32_t native_calls[SCRIPT_PROFILE_NATIVES];
    /* Script time before a native began, not yet charged to a frame; the
       next poll adds it (the poll clock restarts when a native returns). */
    uint64_t pending_script_us;
    uint32_t native_depth;
    /* Innermost frame when the running timed native began: a poll that
       still sees it comes from the native's own checkpoint. */
    const void *native_stack_top;
    /* Time charged to slots later replaced when the table was full: the
       error bound of the (space-saving) self and inclusive tables. */
    uint64_t evicted_us;
    /* Host work done inside a poll after sampling (UI cooperation, input,
       validation logging): not script, so kept out of the next sample. */
    uint64_t host_us;
    /* Calls per profiled native for the tilefinch-work record: cumulative,
       so a report keeps them (these stay last; see script_profile_release). */
    uint64_t work_native_total;
    uint32_t work_native_calls[SCRIPT_PROFILE_NATIVES];
};

typedef struct {
    JSCFunction *function;
    const char *name;
} ScriptProfileNative;
static ScriptProfileNative script_profile_natives[SCRIPT_PROFILE_NATIVES];
static size_t script_profile_native_count;

/* Installed on every realm: records the heap left after each collection
   (the live graph that collection pacing grows from) and times collections
   for the profiler. Allocation-free. */
static void script_runtime_gc_hook(void *opaque, int begin)
{
    ScriptRuntime *owner = opaque;
    if (owner == NULL) return;
    /* Collections nest in nothing that collects, so one token serves. */
    static unsigned gc_split = SCRIPT_SPLIT_KIND_COUNT + 1u;
    if (begin) gc_split = script_split_enter(SCRIPT_SPLIT_GC);
    else script_split_leave(gc_split);
    if (!begin && owner->quickjs_pool != NULL) {
        owner->heap_live_after_gc =
            budget_quickjs_pool_js_malloc_current(owner->quickjs_pool);
        owner->heap_collections++;
    }
    struct ScriptProfile *profile = owner->profile;
    if (profile == NULL) return;
    uint64_t now_ns = js_rt_monotonic_time_ns();
    if (begin) {
        profile->gc_begin_ns = now_ns;
        return;
    }
    if (profile->gc_begin_ns == 0 || now_ns < profile->gc_begin_ns) return;
    uint64_t us = (now_ns - profile->gc_begin_ns) / UINT64_C(1000);
    profile->gc_begin_ns = 0;
    profile->gc_count++;
    profile->gc_us += us;
    profile->gc_unsampled_us += us;
    if (us > profile->gc_max_us) profile->gc_max_us = us;
}

/* First-call compiles of lazy bodies are compile time in the script split,
   exclusive of the script around them. A compile runs no script, so pairs
   do not nest in practice; a small token stack keeps a nested pair (a
   collection inside a compile is its own kind) correct anyway. */
static void script_runtime_lazy_compile_hook(void *opaque, int begin)
{
    (void) opaque;
    static unsigned tokens[8];
    static unsigned depth;
    if (begin) {
        unsigned token = script_split_enter(SCRIPT_SPLIT_COMPILE);
        if (depth < 8u) tokens[depth] = token;
        depth++;
    } else if (depth != 0) {
        depth--;
        if (depth < 8u) script_split_leave(tokens[depth]);
    }
}

static bool script_profile_enabled(void);
static void script_profile_emit(const char *message);

/* The host is about to call into script from an empty JavaScript stack:
   restart the sampler's clock, so host work since the previous entry
   (layout, rendering, network) is not billed to the first frame that
   polls in this one. */
static void script_profile_enter(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->profile == NULL
        || runtime->runtime == NULL || JS_IsStackActive(runtime->runtime))
        return;
    runtime->profile->last_ns = js_rt_monotonic_time_ns();
}

static JSValue script_profile_native_call(JSContext *context,
                                          JSValueConst this_value, int argc,
                                          JSValueConst *argv, int magic,
                                          JSValue *data)
{
    (void) data;
    const ScriptProfileNative *native = &script_profile_natives[magic];
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *owner = bridge == NULL ? NULL : bridge->host;
    struct ScriptProfile *profile = owner == NULL ? NULL : owner->profile;
    if (profile != NULL) {
        profile->work_native_total++;
        profile->work_native_calls[magic]++;
    }
    if (profile == NULL || profile->native_depth != 0)
        return native->function(context, this_value, argc, argv);
    uint64_t begin_ns = js_rt_monotonic_time_ns();
    /* Close the caller's script interval at the call, so neither this
       native's time nor script it calls back into inherits it. */
    uint64_t before_us = profile->last_ns != 0 && begin_ns > profile->last_ns
        ? (begin_ns - profile->last_ns) / UINT64_C(1000) : 0;
    /* A longer gap is time outside this entry, as in the sampler. */
    if (before_us <= UINT64_C(250000))
        profile->pending_script_us += before_us;
    else {
        js_rt_saturating_add_u64(&profile->long_gap_us, before_us);
        if (profile->long_gap_count != UINT32_MAX) profile->long_gap_count++;
    }
    profile->last_ns = begin_ns;
    uint64_t inside_before =
        profile->total_us + profile->gc_us + profile->host_us + profile->profiler_us;
    profile->native_stack_top = JS_GetStackTop(owner->runtime);
    profile->native_depth++;
    unsigned ledger = work_ledger_enter(WORK_LEDGER_NATIVE);
    JSValue value = native->function(context, this_value, argc, argv);
    work_ledger_leave(ledger);
    /* The runtime may have released the profile (a report) meanwhile. */
    profile = owner->profile;
    if (profile == NULL) return value;
    profile->native_depth = 0;
    uint64_t now_ns = js_rt_monotonic_time_ns();
    uint64_t us = now_ns > begin_ns ? (now_ns - begin_ns) / UINT64_C(1000) : 0;
    uint64_t inside_after =
        profile->total_us + profile->gc_us + profile->host_us + profile->profiler_us;
    uint64_t inside = inside_after > inside_before
        ? inside_after - inside_before : 0;
    us = inside < us ? us - inside : 0;
    profile->native_us[magic] += us;
    profile->native_self_us += us;
    profile->native_calls[magic]++;
    profile->last_ns = now_ns;
    profile->last_native = magic;
    profile->last_native_end_ns = now_ns;
    profile->last_native_us = us;
    profile->last_native_inside_us = inside;
    return value;
}

/* The value js_rt_install_function installs: the native itself, or, while
   profiling, a timed trampoline carrying the same name and length. */
static JSValue script_profile_native_value(JSContext *context,
                                           JSCFunction *function,
                                           const char *name, int arguments)
{
    if (!script_profile_enabled())
        return JS_NewCFunction(context, function, name, arguments);
    size_t index = 0;
    while (index < script_profile_native_count
           && script_profile_natives[index].function != function)
        index++;
    if (index == script_profile_native_count) {
        if (index == SCRIPT_PROFILE_NATIVES)
            return JS_NewCFunction(context, function, name, arguments);
        script_profile_natives[index].function = function;
        script_profile_natives[index].name = name;
        script_profile_native_count++;
    }
    JSValue value = JS_NewCFunctionData(context, script_profile_native_call,
                                        arguments, (int) index, 0, NULL);
    if (JS_IsException(value)) return value;
    if (JS_DefinePropertyValueStr(context, value, "name",
                                  JS_NewString(context, name),
                                  JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, value);
        return JS_EXCEPTION;
    }
    return value;
}

static bool script_profile_enabled(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    const char *setting = getenv("TILEFINCH_VALIDATION_JS_PROFILE");
    return setting == NULL || strcmp(setting, "0") != 0;
#elif defined(TILEFINCH_NO_TRACE)
    return false;
#else
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("TILEFINCH_TRACE_JS_PROFILE") != NULL;
    return enabled != 0;
#endif
}

static uint64_t script_profile_outlier_threshold(void)
{
#ifdef TILEFINCH_NO_TRACE
    return 0;
#else
    static long threshold = -1;
    if (threshold < 0) {
        const char *value = getenv("TILEFINCH_JS_PROFILE_OUTLIER_US");
        threshold = value == NULL ? 0 : atol(value);
        if (threshold < 0) threshold = 0;
    }
    return (uint64_t) threshold;
#endif
}

typedef struct {
    uint64_t sampled, native, gc, host, profiler, unframed, long_gap;
    uint64_t lazy_compiles, lazy_bytes;
} ScriptProfileJobSnapshot;

static ScriptProfileJobSnapshot script_profile_job_snapshot(ScriptRuntime *runtime)
{
    ScriptProfileJobSnapshot snapshot = {0};
    const struct ScriptProfile *p = runtime->profile;
    if (p != NULL && script_profile_outlier_threshold() != 0) {
        JSLazyFunctionStats lazy;
        JS_GetLazyFunctionStats(runtime->runtime, &lazy);
        snapshot = (ScriptProfileJobSnapshot) {
            p->total_us, p->native_self_us, p->gc_us, p->host_us,
            p->profiler_us, p->unframed_us, p->long_gap_us,
            lazy.compiled, lazy.compiled_source_bytes};
    }
    return snapshot;
}

static void script_profile_job_finish(ScriptRuntime *runtime,
    ScriptProfileJobSnapshot before, uint64_t begin_ns)
{
    struct ScriptProfile *p = runtime->profile;
    if (p == NULL) return;
    p->job_begin_ns = 0;
    uint64_t threshold = script_profile_outlier_threshold();
    if (threshold == 0) return;
    uint64_t wall = (js_rt_monotonic_time_ns() - begin_ns) / 1000u;
    if (wall < threshold) return;
    ScriptProfileJobSnapshot after = script_profile_job_snapshot(runtime);
    char message[448];
    snprintf(message, sizeof(message),
        "tilefinch-promise-job: job=%llu wall-us=%llu sampled-us=%llu "
        "native-us=%llu gc-us=%llu cooperate-us=%llu profiler-us=%llu unframed-us=%llu long-gap-us=%llu "
        "lazy-compiles=%llu lazy-bytes=%llu",
        (unsigned long long) begin_ns, (unsigned long long) wall,
        (unsigned long long) (after.sampled - before.sampled),
        (unsigned long long) (after.native - before.native),
        (unsigned long long) (after.gc - before.gc),
        (unsigned long long) (after.host - before.host),
        (unsigned long long) (after.profiler - before.profiler),
        (unsigned long long) (after.unframed - before.unframed),
        (unsigned long long) (after.long_gap - before.long_gap),
        (unsigned long long) (after.lazy_compiles - before.lazy_compiles),
        (unsigned long long) (after.lazy_bytes - before.lazy_bytes));
    script_profile_emit(message);
}

/* Ordinary trace-free builds cannot allocate ScriptProfile (see
   script_profile_enabled). Do not retain a dormant stack sampler, or put
   its pointer checks on the interrupt path. Validation keeps the sampler
   even though its general trace switch is off. */
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
static bool script_profile_charge(JSContext *context, ScriptProfileSlot *slots,
                                  JSAtom file, JSAtom function, int line,
                                  int column, int defline, int defcol,
                                  uint64_t us, uint64_t *evicted_us)
{
    ScriptProfileSlot *empty = NULL;
    for (size_t i = 0; i < SCRIPT_PROFILE_SLOTS; i++) {
        ScriptProfileSlot *slot = &slots[i];
        if (slot->samples == 0) {
            if (empty == NULL) empty = slot;
            continue;
        }
        if (slot->file == file && slot->function == function
            && slot->line == line && slot->column == column
            && slot->defline == defline && slot->defcol == defcol) {
            slot->us += us;
            slot->samples++;
            return true;
        }
    }
    if (empty == NULL) {
        /* Full (minified bundles spread one function over many columns):
           replace the lightest slot and keep its time, so heavy hitters
           survive and the table never silently drops time. */
        ScriptProfileSlot *lightest = &slots[0];
        for (size_t i = 1; i < SCRIPT_PROFILE_SLOTS; i++)
            if (slots[i].us < lightest->us) lightest = &slots[i];
        if (evicted_us != NULL) *evicted_us += lightest->us;
        if (lightest->file != JS_ATOM_NULL)
            JS_FreeAtom(context, lightest->file);
        if (lightest->function != JS_ATOM_NULL)
            JS_FreeAtom(context, lightest->function);
        uint64_t inherited = lightest->us;
        uint32_t samples = lightest->samples;
        lightest->file = file == JS_ATOM_NULL
            ? file : JS_DupAtom(context, file);
        lightest->function = function == JS_ATOM_NULL
            ? function : JS_DupAtom(context, function);
        lightest->line = line;
        lightest->column = column;
        lightest->defline = defline;
        lightest->defcol = defcol;
        lightest->us = inherited + us;
        lightest->samples = samples + 1u;
        return true;
    }
    empty->file = file == JS_ATOM_NULL ? file : JS_DupAtom(context, file);
    empty->function = function == JS_ATOM_NULL
        ? function : JS_DupAtom(context, function);
    empty->line = line;
    empty->column = column;
    empty->defline = defline;
    empty->defcol = defcol;
    empty->us = us;
    empty->samples = 1;
    return true;
}

/* Allocation-free apart from atom reference counts: runs in the QuickJS
   interrupt handler. */
static void script_profile_sample(ScriptRuntime *owner, uint64_t now_ns)
{
    struct ScriptProfile *profile = owner->profile;
    if (profile == NULL) return;
    uint64_t us = profile->last_ns != 0 && now_ns > profile->last_ns
        ? (now_ns - profile->last_ns) / UINT64_C(1000) : 0;
    profile->last_ns = now_ns;
    /* Collections since the last poll are reported on their own; script
       time that ran before a timed native is charged here. */
    uint64_t gc_us = profile->gc_unsampled_us;
    profile->gc_unsampled_us = 0;
    us = gc_us < us ? us - gc_us : 0;
    /* A native that checkpoints (style resolution, task and import
       boundaries) polls from inside itself: the time since it began is its
       own, left to its timer; only the script before the call is charged,
       to the calling frame. */
    if (profile->native_depth != 0
        && JS_GetStackTop(owner->runtime) == profile->native_stack_top)
        us = 0;
    uint64_t pending_us = profile->pending_script_us;
    us += pending_us;
    profile->pending_script_us = 0;
    /* A gap longer than a slice is time outside this entry. */
    if (us == 0) return;
    if (us > UINT64_C(250000)) {
        js_rt_saturating_add_u64(&profile->long_gap_us, us);
        if (profile->long_gap_count != UINT32_MAX) profile->long_gap_count++;
        return;
    }
    profile->samples++;
    profile->total_us += us;
    int phase = profile->phase;
    /* TILEFINCH_JS_PROFILE_OUTLIER_US: print the stack and the last native
       of every sample interval at least that long. */
    {
        uint64_t threshold = script_profile_outlier_threshold();
        if (threshold > 0 && us >= threshold) {
            char line_buf[1800];
            int n = snprintf(line_buf, sizeof(line_buf),
                             "tilefinch-js-outlier us=%llu phase=%d site=%d job=%llu "
                             "last-native=%s ago-us=%llu native-us=%llu inside-us=%llu stack=",
                             (unsigned long long) us, phase, profile->micro_site,
                             (unsigned long long) profile->job_begin_ns,
                             profile->last_native >= 0
                                 && (size_t) profile->last_native < script_profile_native_count
                                 ? script_profile_natives[profile->last_native].name : "-",
                             (unsigned long long) (now_ns > profile->last_native_end_ns
                                 ? (now_ns - profile->last_native_end_ns) / 1000u : 0),
                             (unsigned long long) profile->last_native_us,
                             (unsigned long long) profile->last_native_inside_us);
            for (int level = 0; level < 12 && n < (int) sizeof(line_buf) - 140; level++) {
                JSAtom function = JS_ATOM_NULL, file = JS_ATOM_NULL;
                int line = 0, column = 0, defline = 0, defcol = 0;
                if (!JS_GetStackFrameInfo(owner->context, level, &function, &file,
                                          &line, &column)) break;
                const char *fs = file == JS_ATOM_NULL ? "?" : JS_AtomToCString(owner->context, file);
                const char *fn = function == JS_ATOM_NULL ? "" : JS_AtomToCString(owner->context, function);
                const char *base = fs == NULL ? NULL : strrchr(fs, '/');
                (void) JS_GetStackFrameDefinition(owner->context, level, &defline, &defcol);
                n += snprintf(line_buf + n, sizeof(line_buf) - (size_t) n, "%s%.40s:%.24s:%d:%d@%d:%d",
                              level ? "<" : "", base ? base + 1 : (fs ? fs : "?"),
                              fn == NULL || fn[0] == 0 ? "anon" : fn, line, column, defline, defcol);
                if (fs != NULL && file != JS_ATOM_NULL) JS_FreeCString(owner->context, fs);
                if (fn != NULL && function != JS_ATOM_NULL) JS_FreeCString(owner->context, fn);
            }
            script_profile_emit(line_buf);
        }
    }
    if (phase > 0 && phase <= 2) {
        profile->micro_us[phase] += us;
        profile->micro_samples[phase]++;
        if (profile->micro_site >= 0 && profile->micro_site < 4)
            profile->micro_site_us[profile->micro_site] += us;
    }
    /* Inclusive slots are keyed by the function's definition line/column
       too, so anonymous functions do not merge; recursion is detected by
       definition, not name. */
    struct { JSAtom file, function; int defline, defcol; } seen[SCRIPT_PROFILE_DEPTH * 2];
    size_t seen_count = 0;
    bool self_done = false;
    JSAtom outer_function = JS_ATOM_NULL, outer_file = JS_ATOM_NULL;
    int outer_defline = 0, outer_defcol = 0;
    bool outer_seen = false;
    bool outer_complete = false;
    for (int level = 0; level < 64; level++) {
        JSAtom function = JS_ATOM_NULL, file = JS_ATOM_NULL;
        int line = 0, column = 0, defline = 0, defcol = 0;
        if (!JS_GetStackFrameInfo(owner->context, level, &function, &file,
                                  &line, &column)) {
            outer_complete = true;
            break;
        }
        /* A native frame is innermost only when a native polls (task and
           import checkpoints): the time belongs to the script that called
           it. */
        if (line < 0) continue;
        (void) JS_GetStackFrameDefinition(owner->context, level, &defline,
                                          &defcol);
        outer_function = function; outer_file = file;
        outer_defline = defline; outer_defcol = defcol; outer_seen = true;
        /* Bootstrap code is stripped of file names and lines: an anonymous
           bootstrap function is charged as such rather than to its named
           parent, so bootstrap overhead stays visible. */
        if (!self_done) {
            self_done = true;
            if (!script_profile_charge(owner->context, profile->self, file,
                                       function, line, column, defline,
                                       defcol, us, &profile->evicted_us))
                profile->dropped_us += us;
            if (phase > 0)
                (void) script_profile_charge(owner->context,
                                             profile->micro_self, file,
                                             function, line, column, defline,
                                             defcol, us, NULL);
        }
        if (seen_count >= SCRIPT_PROFILE_DEPTH * 2) continue;
        bool repeated = false;
        for (size_t i = 0; i < seen_count; i++)
            if (seen[i].file == file && seen[i].function == function
                && seen[i].defline == defline
                && seen[i].defcol == defcol) repeated = true;
        if (repeated) continue;
        seen[seen_count].function = function;
        seen[seen_count].file = file;
        seen[seen_count].defline = defline;
        seen[seen_count].defcol = defcol;
        seen_count++;
        (void) script_profile_charge(owner->context, profile->inclusive,
                                     file, function, 0, 0, defline, defcol,
                                     us, NULL);
        if (phase > 0)
            (void) script_profile_charge(owner->context,
                                         profile->micro_inclusive, file,
                                         function, 0, 0, defline, defcol,
                                         us, NULL);
    }
    if (!self_done) {
        profile->unframed_us += us;
        profile->unframed_pending_us += pending_us;
        if (profile->compile_depth) profile->unframed_compile_us += us - pending_us;
        else if (profile->returning_from_script) profile->unframed_tail_us += us - pending_us;
        else if (profile->checkpoint_depth) profile->unframed_checkpoint_us += us - pending_us;
        else profile->unframed_entry_us += us - pending_us;
    }
    if (phase > 0 && outer_seen)
        (void) script_profile_charge(owner->context, profile->micro_outer,
                                     outer_file, outer_function, 0, 0,
                                     outer_defline, outer_defcol, us, NULL);
    /* The printed stack can end inside a deep framework. The existing
       walk reaches farther, but also has a cap: explicitly distinguish a
       complete root from a depth-limited ancestor, never infer a callback
       identity from the latter. */
    uint64_t threshold = script_profile_outlier_threshold();
    if (profile->job_begin_ns != 0 && outer_seen && threshold != 0
        && us >= threshold) {
        const char *file = outer_file == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(owner->context, outer_file);
        const char *function = outer_function == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(owner->context, outer_function);
        const char *base = file == NULL ? NULL : strrchr(file, '/');
        char message[320];
        snprintf(message, sizeof(message),
            "tilefinch-promise-root: job=%llu us=%llu complete=%d at=%.100s:%.60s@%d:%d",
            (unsigned long long) profile->job_begin_ns,
            (unsigned long long) us,
            outer_complete ? 1 : 0,
            base != NULL ? base + 1 : file != NULL ? file : "<bootstrap>",
            function != NULL && function[0] ? function : "<anon>",
            outer_defline, outer_defcol);
        script_profile_emit(message);
        if (file != NULL) JS_FreeCString(owner->context, file);
        if (function != NULL) JS_FreeCString(owner->context, function);
    }
}

/* A short callback may return without another VM poll. Close its final
   interval before host bookkeeping; otherwise the next empty-stack
   checkpoint inherits that work, or the next entry discards it. */
static void script_profile_leave(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->profile == NULL
        || JS_IsStackActive(runtime->runtime)) return;
    struct ScriptProfile *profile = runtime->profile;
    uint64_t before = js_rt_monotonic_time_ns();
    profile->returning_from_script = true;
    script_profile_sample(runtime, before);
    profile->returning_from_script = false;
    uint64_t after = js_rt_monotonic_time_ns();
    if (after > before) profile->profiler_us += (after - before) / 1000u;
    profile->last_ns = after;
}
#else
static void script_profile_leave(ScriptRuntime *runtime)
{
    (void) runtime;
}
#endif

static void script_profile_release(JSContext *context,
                                   struct ScriptProfile *profile)
{
    ScriptProfileSlot *tables[5] = {profile->self, profile->inclusive,
                                    profile->micro_inclusive,
                                    profile->micro_outer, profile->micro_self};
    for (size_t t = 0; t < 5; t++) {
        for (size_t i = 0; i < SCRIPT_PROFILE_SLOTS; i++) {
            ScriptProfileSlot *slot = &tables[t][i];
            if (slot->samples == 0) continue;
            if (slot->file != JS_ATOM_NULL) JS_FreeAtom(context, slot->file);
            if (slot->function != JS_ATOM_NULL)
                JS_FreeAtom(context, slot->function);
        }
    }
    uint64_t last_ns = profile->last_ns;
    unsigned compile_depth = profile->compile_depth;
    unsigned checkpoint_depth = profile->checkpoint_depth;
    memset(profile, 0, offsetof(struct ScriptProfile, work_native_total));
    profile->last_ns = last_ns;
    profile->compile_depth = compile_depth;
    profile->checkpoint_depth = checkpoint_depth;
}

/* The device log captures engine lines through the platform logger; the
   host lab prints them. */
static void script_profile_emit(const char *message)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    tilefinch_platform_log_message(message);
#elif defined(TILEFINCH_NATIVE_TIER_LAB)
    /* Whole-page lab diagnostics share one stream with its job trace. The
       replay runner merges stdout/stderr; splitting a line across buffered
       stdout and unbuffered stderr corrupts the attribution record. */
    fprintf(stderr, "%s\n", message);
#else
    printf("%s\n", message);
#endif
}

/* Rows per table: 12, or TILEFINCH_JS_PROFILE_RANKS (up to SCRIPT_PROFILE_SLOTS) on the
   host for a deeper look. */
static int script_profile_ranks(void)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG) || defined(TILEFINCH_NO_TRACE)
    return 12;
#else
    static int ranks = -1;
    if (ranks < 0) {
        const char *value = getenv("TILEFINCH_JS_PROFILE_RANKS");
        ranks = value == NULL ? 12 : atoi(value);
        if (ranks < 1 || ranks > (int) SCRIPT_PROFILE_SLOTS) ranks = 12;
    }
    return ranks;
#endif
}

static void script_profile_print_table(JSContext *context, const char *label,
                                       const char *kind,
                                       ScriptProfileSlot *slots)
{
    for (int rank = 0; rank < script_profile_ranks(); rank++) {
        ScriptProfileSlot *best = NULL;
        for (size_t i = 0; i < SCRIPT_PROFILE_SLOTS; i++) {
            ScriptProfileSlot *slot = &slots[i];
            if (slot->samples == 0 || slot->line < 0) continue;
            if (best == NULL || slot->us > best->us) best = slot;
        }
        if (best == NULL) break;
        const char *file = best->file == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(context, best->file);
        const char *function = best->function == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(context, best->function);
        const char *base = file == NULL ? NULL : strrchr(file, '/');
        char message[320];
        snprintf(message, sizeof(message),
               "tilefinch-js-profile: label=%s %s rank=%d ms=%llu "
               "samples=%u at=%s:%s:%d:%d fn=%d:%d us=%llu", label, kind, rank,
               (unsigned long long) (best->us / 1000u), best->samples,
               base == NULL ? (file == NULL ? "<bootstrap>" : file) : base + 1,
               function == NULL || function[0] == '\0' ? "<anon>" : function,
               best->line, best->column, best->defline, best->defcol,
               (unsigned long long) best->us);
        script_profile_emit(message);
        if (file != NULL) JS_FreeCString(context, file);
        if (function != NULL) JS_FreeCString(context, function);
        best->line = -1 - best->line; /* mark printed */
    }
}

/* Per-function call counts (PSP_BROWSER_JS_CALL_COUNTS builds): the
   rows are allocated only while a report prints. */
typedef struct {
    JSContext *context;
    JSAtom file;
    JSAtom function;
    int line;
    int column;
    uint32_t calls;
} ScriptProfileCallRow;
#define SCRIPT_PROFILE_CALL_ROWS 16384u
static ScriptProfileCallRow *script_profile_call_rows;
static size_t script_profile_call_row_count;
static uint64_t script_profile_call_total;

static void script_profile_collect_calls(void *opaque, JSContext *ctx,
                                         JSAtom file, JSAtom function,
                                         int line, int column, uint32_t calls)
{
    (void) opaque;
    script_profile_call_total += calls;
    if (script_profile_call_rows == NULL
        || script_profile_call_row_count == SCRIPT_PROFILE_CALL_ROWS) return;
    ScriptProfileCallRow *row =
        &script_profile_call_rows[script_profile_call_row_count++];
    row->context = ctx;
    row->file = file;
    row->function = function;
    row->line = line;
    row->column = column;
    row->calls = calls;
}

static int script_profile_call_row_compare(const void *a, const void *b)
{
    const ScriptProfileCallRow *x = a, *y = b;
    return x->calls < y->calls ? 1 : x->calls > y->calls ? -1 : 0;
}

static void script_profile_print_calls(ScriptRuntime *runtime,
                                       const char *label)
{
    script_profile_call_row_count = 0;
    script_profile_call_total = 0;
    script_profile_call_rows =
        malloc(SCRIPT_PROFILE_CALL_ROWS * sizeof(*script_profile_call_rows));
    if (script_profile_call_rows == NULL) return;
    if (!JS_ProfileForEachBytecode(runtime->runtime,
                                   script_profile_collect_calls, NULL, 1)) {
        free(script_profile_call_rows);
        script_profile_call_rows = NULL;
        return;
    }
    qsort(script_profile_call_rows, script_profile_call_row_count,
          sizeof(script_profile_call_rows[0]),
          script_profile_call_row_compare);
    char message[320];
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s calls total=%llu functions=%zu",
             label == NULL ? "" : label,
             (unsigned long long) script_profile_call_total,
             script_profile_call_row_count);
    script_profile_emit(message);
    int ranks = script_profile_ranks();
    for (size_t i = 0; i < script_profile_call_row_count && (int) i < ranks;
         i++) {
        ScriptProfileCallRow *row = &script_profile_call_rows[i];
        const char *file = row->file == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(row->context, row->file);
        const char *function = row->function == JS_ATOM_NULL ? NULL
            : JS_AtomToCString(row->context, row->function);
        const char *base = file == NULL ? NULL : strrchr(file, '/');
        snprintf(message, sizeof(message),
                 "tilefinch-js-profile: label=%s calls rank=%zu calls=%u "
                 "at=%s:%s fn=%d:%d", label == NULL ? "" : label, i,
                 row->calls,
                 base == NULL ? (file == NULL ? "<bootstrap>" : file) : base + 1,
                 function == NULL || function[0] == '\0' ? "<anon>" : function,
                 row->line, row->column);
        script_profile_emit(message);
        if (file != NULL) JS_FreeCString(row->context, file);
        if (function != NULL) JS_FreeCString(row->context, function);
    }
    free(script_profile_call_rows);
    script_profile_call_rows = NULL;
}

void script_runtime_profile_report(ScriptRuntime *runtime, const char *label)
{
    if (runtime == NULL || runtime->profile == NULL
        || runtime->context == NULL) return;
    struct ScriptProfile *profile = runtime->profile;
    char message[384];
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s samples=%u total-ms=%llu "
             "evicted-ms=%llu host-ms=%llu gc=%u gc-ms=%llu gc-max-ms=%llu "
             "total-us=%llu unframed-us=%llu long-gap-us=%llu long-gap-count=%u",
             label == NULL ? "" : label, profile->samples,
             (unsigned long long) (profile->total_us / 1000u),
             (unsigned long long)
                 ((profile->dropped_us + profile->evicted_us) / 1000u),
             (unsigned long long) (profile->host_us / 1000u),
             profile->gc_count,
             (unsigned long long) (profile->gc_us / 1000u),
             (unsigned long long) (profile->gc_max_us / 1000u),
             (unsigned long long) profile->total_us,
             (unsigned long long) profile->unframed_us,
             (unsigned long long) profile->long_gap_us,
             (unsigned) profile->long_gap_count);
    script_profile_emit(message);
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s unframed pending-us=%llu compile-us=%llu "
             "checkpoint-us=%llu entry-us=%llu tail-us=%llu profiler-us=%llu",
             label == NULL ? "" : label,
             (unsigned long long) profile->unframed_pending_us,
             (unsigned long long) profile->unframed_compile_us,
             (unsigned long long) profile->unframed_checkpoint_us,
             (unsigned long long) profile->unframed_entry_us,
             (unsigned long long) profile->unframed_tail_us,
             (unsigned long long) profile->profiler_us);
    script_profile_emit(message);
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s attribute-writes=%zu "
             "unchanged=%zu native-ms=%llu", label == NULL ? "" : label,
             runtime->bridge.attribute_writes,
             runtime->bridge.attribute_writes_unchanged,
             (unsigned long long)
                 (runtime->bridge.attribute_write_ns / UINT64_C(1000000)));
    script_profile_emit(message);
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s heap live=%zu limit=%zu "
             "window=%zu growth=%d raises=%zu pregrows=%zu "
             "refusals=%zu returns=%zu", label == NULL ? "" : label,
             budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool),
             runtime->base_memory_limit,
             runtime->boot_window_active ? runtime->boot_window_bytes : 0,
             runtime->heap_growth_enabled ? 1 : 0,
             runtime->heap_growth_raises, runtime->heap_growth_pregrows,
             runtime->heap_growth_refusals, runtime->heap_growth_returns);
    script_profile_emit(message);
    runtime->work_attribute_writes_reported += runtime->bridge.attribute_writes;
    runtime->bridge.attribute_writes = 0;
    runtime->bridge.attribute_writes_unchanged = 0;
    runtime->bridge.attribute_write_ns = 0;
    uint64_t native_total_us = 0;
    for (size_t i = 0; i < script_profile_native_count; i++)
        native_total_us += profile->native_us[i];
    __typeof__(runtime->bridge.computed_style_cache) *style_cache =
        &runtime->bridge.computed_style_cache;
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s native-ms=%llu "
             "style-cache=%zu/%zu clears=%zu/%zu host-clears=%zu bytes=%zu "
             "var-cache=%zu/%zu var-clears=%zu style-probes=%zu",
             label == NULL ? "" : label,
             (unsigned long long) (native_total_us / 1000u),
             style_cache->hits, style_cache->misses,
             style_cache->scoped_clears, style_cache->full_clears,
             style_cache->host_clears,
             js_rt_bridge_computed_style_cache_bytes(&runtime->bridge),
             style_cache->variables.hits, style_cache->variables.misses,
             style_cache->variables.clears, style_cache->lookup_probes);
    style_cache->hits = 0;
    style_cache->misses = 0;
    style_cache->lookup_probes = 0;
    style_cache->scoped_clears = 0;
    style_cache->full_clears = 0;
    style_cache->host_clears = 0;
    style_cache->variables.hits = 0;
    style_cache->variables.misses = 0;
    style_cache->variables.clears = 0;
    script_profile_emit(message);
    for (int rank = 0; rank < script_profile_ranks(); rank++) {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < script_profile_native_count; i++)
            if (profile->native_calls[i] != 0
                && (best == SIZE_MAX
                    || profile->native_us[i] > profile->native_us[best]))
                best = i;
        if (best == SIZE_MAX || profile->native_us[best] < 1000u) break;
        snprintf(message, sizeof(message),
                 "tilefinch-js-profile: label=%s native rank=%d ms=%llu "
                 "calls=%u at=%s", label == NULL ? "" : label, rank,
                 (unsigned long long) (profile->native_us[best] / 1000u),
                 profile->native_calls[best],
                 script_profile_natives[best].name);
        script_profile_emit(message);
        profile->native_calls[best] = 0;
    }
    script_profile_print_table(runtime->context, label, "self",
                               profile->self);
    script_profile_print_table(runtime->context, label, "inclusive",
                               profile->inclusive);
    /* Microtask phase: promise jobs by run_jobs call site, their outermost
       frames, and inclusive/self time inside them. */
    snprintf(message, sizeof(message),
             "tilefinch-js-profile: label=%s microtask promise-job-ms=%llu "
             "samples=%u continuation-ms=%llu samples=%u "
             "site-ms other=%llu entry=%llu network=%llu timer=%llu",
             label == NULL ? "" : label,
             (unsigned long long) (profile->micro_us[1] / 1000u),
             profile->micro_samples[1],
             (unsigned long long) (profile->micro_us[2] / 1000u),
             profile->micro_samples[2],
             (unsigned long long) (profile->micro_site_us[0] / 1000u),
             (unsigned long long) (profile->micro_site_us[1] / 1000u),
             (unsigned long long) (profile->micro_site_us[2] / 1000u),
             (unsigned long long) (profile->micro_site_us[3] / 1000u));
    script_profile_emit(message);
    script_profile_print_table(runtime->context, label, "micro-outer",
                               profile->micro_outer);
    script_profile_print_table(runtime->context, label, "micro-inclusive",
                               profile->micro_inclusive);
    script_profile_print_table(runtime->context, label, "micro-self",
                               profile->micro_self);
    script_profile_print_calls(runtime, label);
    script_profile_release(runtime->context, profile);
}

uint32_t script_runtime_work_counters(const ScriptRuntime *runtime,
                                      uint64_t work[SCRIPT_WORK_COUNT])
{
    uint32_t missing = (1u << SCRIPT_WORK_CALLS)
        | (1u << SCRIPT_WORK_BYTECODE_OPS) | (1u << SCRIPT_WORK_FLOAT64_BOXES)
        | (1u << SCRIPT_WORK_NATIVE_CALLS);
    memset(work, 0, SCRIPT_WORK_COUNT * sizeof(work[0]));
    if (runtime == NULL) return missing;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    _Static_assert(SCRIPT_WORK_LAZY_BYTES == JS_WORK_LAZY_BYTES
                   && SCRIPT_WORK_CALLS == JS_WORK_CALLS,
                   "script work leads with the engine's counters");
    if (runtime->runtime != NULL)
        missing = (uint32_t) JS_GetWorkCounters(runtime->runtime, work)
            | (1u << SCRIPT_WORK_NATIVE_CALLS);
#endif
    if (runtime->profile != NULL) {
        missing &= ~(1u << SCRIPT_WORK_NATIVE_CALLS);
        work[SCRIPT_WORK_NATIVE_CALLS] = runtime->profile->work_native_total;
    }
    work[SCRIPT_WORK_ATTRIBUTE_WRITES] = runtime->work_attribute_writes_reported
        + runtime->bridge.attribute_writes;
    BudgetQuickJSActivity activity;
    budget_quickjs_pool_activity(runtime->quickjs_pool, &activity);
    work[SCRIPT_WORK_ALLOCS] =
        activity.allocation_calls + activity.reallocation_calls;
    work[SCRIPT_WORK_ALLOC_BYTES] =
        activity.allocated_bytes + activity.reallocated_bytes;
    work[SCRIPT_WORK_SOURCE_BYTES] = runtime->bridge.script_quota_bytes;
    work[SCRIPT_WORK_MODULE_COMPILES] = runtime->result.module_compile_count;
    work[SCRIPT_WORK_MODULE_RESTORES] =
        runtime->result.module_bytecode_cache_hits;
    work[SCRIPT_WORK_DOM_MUTATIONS] = runtime->result.dom_mutations;
    /* MutationObserver routing lives in the bootstrap (dom.js): read its
       counters as plain numbers, running no script and allocating nothing
       (the property atoms already exist). */
    if (runtime->context == NULL || runtime->runtime == NULL
        || JS_IsStackActive(runtime->runtime))
        return missing;
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue stats = JS_GetPropertyStr(runtime->context, global,
                                      "__tilefinchRetentionStats");
    JS_FreeValue(runtime->context, global);
    static const char *const names[2] = {"mutationRecords", "observerVisits"};
    for (size_t i = 0; i < 2 && JS_IsObject(stats); i++) {
        JSValue value = JS_GetPropertyStr(runtime->context, stats, names[i]);
        double number = 0;
        if (JS_IsNumber(value)
            && JS_ToFloat64(runtime->context, &number, value) == 0
            && number >= 0)
            work[SCRIPT_WORK_MUTATION_RECORDS + i] = (uint64_t) number;
        JS_FreeValue(runtime->context, value);
    }
    JS_FreeValue(runtime->context, stats);
    return missing;
}

size_t script_runtime_opcode_counts(const ScriptRuntime *runtime,
                                    const char **names, uint64_t *counts,
                                    size_t capacity)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    if (runtime != NULL && runtime->runtime != NULL)
        return JS_GetOpcodeCounts(runtime->runtime, names, counts, capacity);
#else
    (void) runtime; (void) names; (void) counts; (void) capacity;
#endif
    return 0;
}

void script_runtime_report_pool_traffic(const ScriptRuntime *runtime,
                                        const char *label, FILE *output)
{
    if (runtime == NULL) return;
    budget_quickjs_pool_report_traffic(runtime->quickjs_pool, label, output);
}

size_t script_runtime_work_top_natives(const ScriptRuntime *runtime,
                                       size_t limit, const char **names,
                                       uint64_t *calls)
{
    const struct ScriptProfile *profile =
        runtime == NULL ? NULL : runtime->profile;
    size_t count = 0;
    /* Insertion into the short ranked list: by calls, then registration
       order, so equal counts list deterministically. Marks only. */
    for (size_t i = 0; profile != NULL && i < script_profile_native_count;
         i++) {
        uint32_t value = profile->work_native_calls[i];
        size_t at = count;
        while (at > 0 && calls[at - 1] < value) at--;
        if (value == 0 || at >= limit) continue;
        if (count < limit) count++;
        for (size_t j = count - 1; j > at; j--) {
            calls[j] = calls[j - 1];
            names[j] = names[j - 1];
        }
        calls[at] = value;
        names[at] = script_profile_natives[i].name;
    }
    return count;
}

static int interrupt_handler_service(JSRuntime *runtime, Watchdog *watchdog,
                                     uint64_t now_ns, uint64_t now);

/* Which code the JS time since the previous poll most likely belongs to
   (script_split.h): the innermost bytecode frame's origin, or a built-in
   that polls from its own bounded loop. Stripped bootstrap functions carry
   no file name. */
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
static void interrupt_split_sample(ScriptRuntime *owner)
{
    if (!script_split_sampling() || owner == NULL || owner->context == NULL)
        return;
    bool in_js = script_split_current() == SCRIPT_SPLIT_JS;
    ScriptSplitSample sample = SCRIPT_SPLIT_SAMPLE_PAGE;
    /* Origin only: the line (JS_GetStackFrameInfo) would decode the
       function's pc-to-line table on every poll. */
    for (int level = 0; level < 4; level++) {
        int origin = JS_GetStackFrameOrigin(owner->context, level);
        if (origin == JS_FRAME_ORIGIN_NONE) break;
        if (origin == JS_FRAME_ORIGIN_NATIVE) {
            if (level == 0 && in_js) {
                sample = SCRIPT_SPLIT_SAMPLE_NATIVE;
                break;
            }
            continue;
        }
        sample = origin == JS_FRAME_ORIGIN_ANONYMOUS
            ? SCRIPT_SPLIT_SAMPLE_BOOTSTRAP : SCRIPT_SPLIT_SAMPLE_PAGE;
        break;
    }
    script_split_poll(sample);
}
#endif

static int interrupt_handler(JSRuntime *runtime, void *opaque)
{
    Watchdog *watchdog = opaque;
    uint64_t now_ns = js_rt_monotonic_time_ns();
    uint64_t now = now_ns / UINT64_C(1000000);
    watchdog->polls++;
    watchdog_add_work(watchdog, 1);
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    ScriptRuntime *profiled = (ScriptRuntime *) (void *)
        ((char *) watchdog - offsetof(ScriptRuntime, watchdog));
    interrupt_split_sample(profiled);
    if (profiled->profile != NULL) script_profile_sample(profiled, now_ns);
    else profiled = NULL;
    uint64_t sampled_ns = profiled == NULL ? now_ns : js_rt_monotonic_time_ns();
    if (profiled != NULL && sampled_ns > now_ns)
        profiled->profile->profiler_us += (sampled_ns - now_ns) / 1000u;
#endif
    unsigned split = script_split_enter(SCRIPT_SPLIT_HOST);
    int verdict = interrupt_handler_service(runtime, watchdog, now_ns, now);
    script_split_leave(split);
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    /* The rest of this poll was host work: restart the sampler after it. */
    if (profiled != NULL && profiled->profile != NULL) {
        uint64_t after_ns = js_rt_monotonic_time_ns();
        if (after_ns > sampled_ns)
            profiled->profile->host_us +=
                (after_ns - sampled_ns) / UINT64_C(1000);
        profiled->profile->last_ns = after_ns;
    }
#endif
    return verdict;
}

static int interrupt_handler_service(JSRuntime *runtime, Watchdog *watchdog,
                                     uint64_t now_ns, uint64_t now)
{
#if !defined(CONFIG_EXECUTION_PROFILE)
    (void) runtime;
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Sample at most once a second while JavaScript keeps running: one long
       entry, or a stream of short microtasks that together hold an advance
       (a 64-job checkpoint of 30 ms jobs is two seconds on Allegrex). */
    static uint64_t next_sample_ms;
    if (now >= watchdog->started_ms && now - watchdog->started_ms >= 20u
        && now >= next_sample_ms) {
        if (next_sample_ms != 0)
            watchdog_log_long_task(watchdog, now - watchdog->started_ms);
        next_sample_ms = now + 1000u;
    }
#endif
    /* Four polls amortize cheap calls, but are not a time bound: on Allegrex
       a single VM/native unit can take tens of milliseconds. Service input
       at the next safe poll once the slice has used eight milliseconds. */
    bool slice_expired = watchdog->slice_started_ns != 0
        && now_ns >= watchdog->slice_started_ns
        && now_ns - watchdog->slice_started_ns >= UINT64_C(8000000);
    if ((watchdog->slice_work_units >= 4 || slice_expired)
        && !watchdog_cooperate(watchdog)) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        watchdog_log_interrupt(watchdog, "cooperate", now);
#endif
#ifndef TILEFINCH_NO_TRACE
        if (tilefinch_trace_js_interrupts()) {
            fprintf(stderr,
                    "tilefinch-js-interrupt: reason=cooperate "
                    "now-ms=%llu started-ms=%llu deadline-ms=%llu "
                    "polls=%zu\n",
                    (unsigned long long) now,
                    (unsigned long long) watchdog->started_ms,
                    (unsigned long long) watchdog->deadline_ms,
                    watchdog->polls);
        }
#endif
        return 1;
    }
    /* Watchdog cadence is intentionally not page-visible. Deterministic
       replay time advances only at explicit Date/performance observations
       and event-loop elapsed_ms boundaries, which other engines can mirror. */
#if defined(CONFIG_EXECUTION_PROFILE)
    JS_RecordExecutionProfilePoll(runtime);
    if (now >= watchdog->next_profile_ms && now < watchdog->deadline_ms) {
        JS_DumpExecutionProfileSnapshot(runtime, now - watchdog->started_ms);
        do {
            watchdog->next_profile_ms += 1000u;
        } while (watchdog->next_profile_ms <= now);
    }
#endif
    if (now >= watchdog->deadline_ms) {
        watchdog->interrupted = true;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        watchdog_log_interrupt(watchdog, "deadline", now);
#endif
#ifndef TILEFINCH_NO_TRACE
        if (tilefinch_trace_js_interrupts()) {
            fprintf(stderr,
                    "tilefinch-js-interrupt: reason=deadline "
                    "now-ms=%llu started-ms=%llu deadline-ms=%llu "
                    "polls=%zu\n",
                    (unsigned long long) now,
                    (unsigned long long) watchdog->started_ms,
                    (unsigned long long) watchdog->deadline_ms,
                    watchdog->polls);
        }
#endif
        return 1;
    }
    return 0;
}

bool js_rt_runtime_native_checkpoint(ScriptRuntime *runtime)
{
    /* A native import/export is work inside the current task, not a new
       asynchronous callback. Share the VM's work/time cooperative cadence;
       interrupt_handler still checks the wall deadline on every boundary. */
    if (runtime == NULL || runtime->watchdog.interrupted) return false;
    if (runtime->profile != NULL) runtime->profile->checkpoint_depth++;
    bool okay = interrupt_handler(runtime->runtime, &runtime->watchdog) == 0;
    if (runtime->profile != NULL) runtime->profile->checkpoint_depth--;
    return okay;
}

bool js_rt_runtime_callback_checkpoint(ScriptRuntime *runtime)
{
    if (runtime == NULL) return false;
    Watchdog *watchdog = &runtime->watchdog;
    watchdog->polls++;
    watchdog_add_work(watchdog, 1);
    /* Unlike the VM's amortised interrupt counter, an asynchronous callback
       is already a browser task boundary.  Cooperating on every such entry is
       bounded by the host's callback budget and gives Circle/input a chance
       before any author callback starts. */
    if (!watchdog_cooperate(watchdog)) return false;
    if (monotonic_time_ms() >= watchdog->deadline_ms) {
        watchdog->interrupted = true;
        return false;
    }
    return true;
}

static bool node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

/* Error formatting is best-effort. A hostile thrown value can itself throw
   from ToPrimitive/toString; drain that secondary exception so it cannot
   replace the original browser-task failure or poison the next QuickJS API. */
static void js_rt_clear_formatting_exception(JSContext *context)
{
    JSValue secondary = JS_GetException(context);
    JS_FreeValue(context, secondary);
}

static const char *js_rt_exception_to_cstring(
    JSContext *context, JSValueConst value)
{
    const char *text = JS_ToCString(context, value);
    if (text == NULL) js_rt_clear_formatting_exception(context);
    return text;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static void runtime_report_heap_failure(ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->heap_failure_census_reported
        || script_runtime_heap_rejections(runtime) == 0) return;
    /* One diagnostic heap walk after refusal, never a forced collection.
       Include caught timer failures as well as exceptions escaping eval. */
    runtime->heap_failure_census_reported = true;
    JSMemoryUsage usage;
    uint64_t started_ns = js_rt_monotonic_time_ns();
    js_rt_heap_census(runtime, &usage);
    BudgetQuickJSActivity activity;
    budget_quickjs_pool_activity(runtime->quickjs_pool, &activity);
    char diagnostic[768];
    snprintf(diagnostic, sizeof(diagnostic),
        "tilefinch-heap-failure: malloc=%lld retained=%lld "
        "objects=%lld/%lld properties=%lld shapes=%lld/%lld "
        "functions=%lld/%lld code=%lld strings=%lld/%lld atoms=%lld/%lld "
        "arrays=%lld elements=%lld binaries=%lld/%lld handles=%zu census-us=%llu "
        "reject-old=%zu reject-new=%zu reject-live=%zu arenas=%lld/%lld/%lld",
        (long long) usage.malloc_size, (long long) usage.memory_used_size,
        (long long) usage.obj_count, (long long) usage.obj_size,
        (long long) usage.prop_size,
        (long long) usage.shape_count, (long long) usage.shape_size,
        (long long) usage.js_func_count, (long long) usage.js_func_size,
        (long long) usage.js_func_code_size,
        (long long) usage.str_count, (long long) usage.str_size,
        (long long) usage.atom_count, (long long) usage.atom_size,
        (long long) usage.fast_array_count,
        (long long) usage.fast_array_elements,
        (long long) usage.binary_object_count,
        (long long) usage.binary_object_size, runtime->bridge.node_count,
        (unsigned long long) ((js_rt_monotonic_time_ns() - started_ns)
                              / UINT64_C(1000)),
        activity.rejected_old_bytes, activity.rejected_new_bytes,
        activity.rejected_live_bytes,
        (long long)usage.malloc_arena_count,
        (long long)usage.malloc_arena_capacity,
        (long long)usage.malloc_arena_used);
    tilefinch_platform_log_message(diagnostic);
}
#endif

void js_rt_record_exception(JSContext *context, ScriptResult *result)
{
    JSValue exception = JS_GetException(context);
    const char *message = js_rt_exception_to_cstring(context, exception);
    if (message != NULL) {
        snprintf(result->error, sizeof(result->error), "%s", message);
        JS_FreeCString(context, message);
    } else {
        snprintf(result->error, sizeof(result->error), "JavaScript exception");
    }
    JSValue stack_value = JS_GetPropertyStr(context, exception, "stack");
    if (JS_IsException(stack_value)) {
        js_rt_clear_formatting_exception(context);
        stack_value = JS_UNDEFINED;
    }
    const char *stack = JS_IsUndefined(stack_value)
        ? NULL : js_rt_exception_to_cstring(context, stack_value);
    if (stack != NULL && stack[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 2 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\n%s", stack);
        }
        JS_FreeCString(context, stack);
    }
    JS_FreeValue(context, stack_value);
    JSValue global = JS_GetGlobalObject(context);
    JSValue selector_value = JS_GetPropertyStr(
        context, global, "__tilefinchLastMissingSelector");
    const char *selector = JS_IsString(selector_value)
                           ? JS_ToCString(context, selector_value) : NULL;
    if (selector != NULL && selector[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nlast empty selector: %s", selector);
        }
        JS_FreeCString(context, selector);
    }
    JS_FreeValue(context, selector_value);
    JSValue base64_value = JS_GetPropertyStr(
        context, global, "__tilefinchBase64Error");
    const char *base64 = JS_IsString(base64_value)
                         ? JS_ToCString(context, base64_value) : NULL;
    if (base64 != NULL && base64[0] != '\0') {
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nbase64 diagnostic: %s", base64);
        }
        JS_FreeCString(context, base64);
    }
    JS_FreeValue(context, base64_value);
    JSValue frame_eval_value = JS_GetPropertyStr(
        context, global, "__tilefinchFrameEvalTelemetry");
    const char *frame_eval = JS_IsString(frame_eval_value)
                             ? JS_ToCString(context, frame_eval_value) : NULL;
    if (frame_eval != NULL && frame_eval[0] != '\0') {
        snprintf(result->frame_eval_telemetry,
                 sizeof(result->frame_eval_telemetry), "%s", frame_eval);
        size_t used = strlen(result->error);
        if (used + 20 < sizeof(result->error)) {
            snprintf(result->error + used, sizeof(result->error) - used,
                     "\nframe eval: %s", frame_eval);
        }
        JS_FreeCString(context, frame_eval);
    }
    JS_FreeValue(context, frame_eval_value);
    JS_FreeValue(context, global);
    JS_FreeValue(context, exception);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    DomBridge *bridge = JS_GetContextOpaque(context);
    char diagnostic[768];
    snprintf(diagnostic, sizeof(diagnostic),
        "tilefinch-script-exception: interrupted=%d heap-remaining=%zu "
        "heap-rejections=%zu error=\"%.600s\"",
        bridge != NULL && bridge->host != NULL
            && bridge->host->watchdog.interrupted,
        bridge == NULL || bridge->host == NULL ? 0u
            : script_runtime_heap_remaining(bridge->host),
        bridge == NULL || bridge->host == NULL ? 0u
            : script_runtime_heap_rejections(bridge->host), result->error);
    tilefinch_platform_log_message(diagnostic);
    runtime_report_heap_failure(bridge == NULL ? NULL : bridge->host);
#endif
}

static void promise_rejection_tracker(JSContext *context,
                                      JSValueConst promise,
                                      JSValueConst reason,
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
                                      int is_handled,
#else
                                      bool is_handled,
#endif
                                      void *opaque)
{
    if (opaque == NULL) return;
    /* Worker promise rejection events belong to the WorkerGlobalScope, not
       the owner Window.  The immutable realm hook only queues a bounded task;
       it never invokes author handlers from inside QuickJS's rejection
       tracker.  A failure in this browser-owned notification path must not
       replace the promise's own rejection. */
    JSValue worker_global = JS_GetGlobalObject(context);
    JSValue worker_notify = JS_GetPropertyStr(
        context, worker_global, "__tilefinchQueuePromiseRejection");
    if (JS_IsFunction(context, worker_notify)) {
        JSValue args[3] = {
            JS_DupValue(context, promise),
            JS_DupValue(context, reason),
            JS_NewBool(context, is_handled),
        };
        JSValue notified = JS_Call(
            context, worker_notify, worker_global, 3, args);
        for (size_t i = 0; i < 3; i++) JS_FreeValue(context, args[i]);
        if (JS_IsException(notified)) js_rt_clear_formatting_exception(context);
        JS_FreeValue(context, notified);
    } else if (JS_IsException(worker_notify)) {
        js_rt_clear_formatting_exception(context);
    }
    JS_FreeValue(context, worker_notify);
    JS_FreeValue(context, worker_global);
    PromiseRejectionState *state = opaque;
    ScriptResult *result = state->result;
    if (result == NULL) return;
    void *identity = JS_VALUE_GET_PTR(promise);
    if (is_handled) {
        js_rt_saturating_add_size(&result->promise_rejections_handled, 1);
        for (size_t i = 0; i < state->count; i++) {
            if (state->promises[i] != identity) continue;
            state->promises[i] = state->promises[state->count - 1];
            state->count--;
            break;
        }
        result->promise_rejections = state->count;
        if (state->count == 0) {
            result->last_promise_rejection[0] = '\0';
            state->last_reported = NULL;
        } else if (state->last_reported == identity) {
            snprintf(result->last_promise_rejection,
                     sizeof(result->last_promise_rejection),
                     "unhandled promise rejection remains");
            state->last_reported = NULL;
        }
        return;
    }
    for (size_t i = 0; i < state->count; i++) {
        if (state->promises[i] == identity) return;
    }
    js_rt_saturating_add_size(&result->promise_rejections_created, 1);
    if (JS_IsUndefined(reason)) {
        js_rt_saturating_add_size(&result->promise_rejections_undefined, 1);
    }
    if (state->count < sizeof(state->promises) / sizeof(state->promises[0])) {
        state->promises[state->count++] = identity;
    }
    result->promise_rejections = state->count;
    state->last_reported = identity;
    const char *message = js_rt_exception_to_cstring(context, reason);
    snprintf(result->last_promise_rejection,
             sizeof(result->last_promise_rejection), "%s",
             message == NULL ? "unhandled promise rejection" : message);
    if (message != NULL) JS_FreeCString(context, message);
    JSValue stack_value = JS_GetPropertyStr(context, reason, "stack");
    if (JS_IsException(stack_value)) {
        js_rt_clear_formatting_exception(context);
        stack_value = JS_UNDEFINED;
    }
    const char *stack = JS_IsUndefined(stack_value)
        ? NULL : js_rt_exception_to_cstring(context, stack_value);
    if (stack != NULL && stack[0] != '\0') {
        size_t used = strlen(result->last_promise_rejection);
        if (used + 2 < sizeof(result->last_promise_rejection)) {
            snprintf(result->last_promise_rejection + used,
                     sizeof(result->last_promise_rejection) - used,
                     "\n%s", stack);
        }
        JS_FreeCString(context, stack);
    }
    /* A SyntaxError from a deferred compile (a lazy function's first call,
       a dynamic import) names its source only through these properties. */
    JSValue file_value = JS_IsObject(reason)
        ? JS_GetPropertyStr(context, reason, "fileName") : JS_UNDEFINED;
    if (JS_IsException(file_value)) {
        js_rt_clear_formatting_exception(context);
        file_value = JS_UNDEFINED;
    }
    if (JS_IsString(file_value)) {
        const char *file = JS_ToCString(context, file_value);
        int32_t line = 0, column = 0;
        JSValue line_value = JS_GetPropertyStr(context, reason, "lineNumber");
        JSValue column_value =
            JS_GetPropertyStr(context, reason, "columnNumber");
        if (JS_ToInt32(context, &line, line_value) < 0
            || JS_ToInt32(context, &column, column_value) < 0) {
            js_rt_clear_formatting_exception(context);
        }
        JS_FreeValue(context, line_value);
        JS_FreeValue(context, column_value);
        size_t used = strlen(result->last_promise_rejection);
        if (file != NULL && used + 2 < sizeof(result->last_promise_rejection)) {
            snprintf(result->last_promise_rejection + used,
                     sizeof(result->last_promise_rejection) - used,
                     "\n@ %s:%d:%d", file, (int) line, (int) column);
        }
        if (file != NULL) JS_FreeCString(context, file);
    }
    JS_FreeValue(context, file_value);
    if (tilefinch_trace_promise_rejections()) {
        fprintf(stderr, "promise-rejection source=\"%s\" reason=\"%s\" "
                "created=%zu handled=%zu live=%zu undefined=%zu\n",
                state->active_source == NULL ? "<event-loop>"
                                             : state->active_source,
                result->last_promise_rejection,
                result->promise_rejections_created,
                result->promise_rejections_handled,
                result->promise_rejections,
                result->promise_rejections_undefined);
    }
    JS_FreeValue(context, stack_value);
}

void js_rt_saturating_add_size(size_t *value, size_t amount)
{
    if (value == NULL) return;
    *value = amount > SIZE_MAX - *value ? SIZE_MAX : *value + amount;
}

void js_rt_saturating_add_u64(uint64_t *value, uint64_t amount)
{
    if (value == NULL) return;
    *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount;
}

static uint64_t compile_observation_threshold(const DomBridge *bridge)
{
    return bridge == NULL
           || bridge->execution_policy.slow_compile_threshold_us == 0
        ? SCRIPT_DEFAULT_SLOW_DEPENDENCY_US
        : bridge->execution_policy.slow_compile_threshold_us;
}

typedef struct {
    int64_t malloc_size;
    int64_t retained_size;
    int64_t function_count;
    int64_t code_size;
    int64_t object_count;
} ScriptResidencySample;

static const char *script_residency_trace_mode(void)
{
    const char *mode = tilefinch_trace_script_residency_value();
    return mode == NULL || mode[0] == '\0' ? NULL : mode;
}

static ScriptResidencySample script_residency_sample(JSRuntime *runtime)
{
    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    if (runtime != NULL) JS_ComputeMemoryUsage(runtime, &usage);
    return (ScriptResidencySample) {
        .malloc_size = usage.malloc_size,
        .retained_size = usage.memory_used_size,
        .function_count = usage.js_func_count,
        .code_size = usage.js_func_code_size,
        .object_count = usage.obj_count
    };
}

static int64_t script_residency_delta(int64_t after, int64_t before)
{
    if (after >= before) return after - before;
    return -(before - after);
}

static void script_residency_label(char output[256], const char *name)
{
    const char *source = name == NULL ? "<script>" : name;
    size_t written = 0;
    for (; *source != '\0' && written + 1 < 256; source++) {
        unsigned char character = (unsigned char) *source;
        output[written++] = character < 32 || character == 127
            || character == '"' || character == '\\' ? '_' : (char) character;
    }
    output[written] = '\0';
}

static void script_residency_trace_compile(
    JSRuntime *runtime, const char *name, size_t source_bytes,
    ScriptCompileSourceKind source_kind, ScriptResidencySample before,
    bool admitted, bool compiled)
{
    if (script_residency_trace_mode() == NULL) return;
    ScriptResidencySample after = script_residency_sample(runtime);
    char label[256];
    script_residency_label(label, name);
    fprintf(stderr,
            "script-residency phase=compile name=\"%s\" kind=%u "
            "source-bytes=%zu admitted=%s compiled=%s malloc=%lld "
            "malloc-delta=%lld retained=%lld retained-delta=%lld "
            "functions=%lld function-delta=%lld code=%lld code-delta=%lld "
            "objects=%lld object-delta=%lld\n",
            label, (unsigned) source_kind, source_bytes,
            admitted ? "yes" : "no", compiled ? "yes" : "no",
            (long long) after.malloc_size,
            (long long) script_residency_delta(
                after.malloc_size, before.malloc_size),
            (long long) after.retained_size,
            (long long) script_residency_delta(
                after.retained_size, before.retained_size),
            (long long) after.function_count,
            (long long) script_residency_delta(
                after.function_count, before.function_count),
            (long long) after.code_size,
            (long long) script_residency_delta(after.code_size,
                                                before.code_size),
            (long long) after.object_count,
            (long long) script_residency_delta(
                after.object_count, before.object_count));
}

static void script_residency_trace_settled(
    JSRuntime *runtime, const char *name, size_t source_bytes, bool success)
{
    const char *mode = script_residency_trace_mode();
    if (mode == NULL || runtime == NULL) return;
    ScriptResidencySample before = script_residency_sample(runtime);
    bool collected = strcmp(mode, "gc") == 0;
    if (collected) {
        JS_RunGC(runtime);
        JS_RunGC(runtime);
    }
    ScriptResidencySample after = collected
        ? script_residency_sample(runtime) : before;
    char label[256];
    script_residency_label(label, name);
    fprintf(stderr,
            "script-residency phase=settled name=\"%s\" source-bytes=%zu "
            "success=%s collected=%s malloc=%lld reclaimed-malloc=%lld "
            "retained=%lld reclaimed-retained=%lld functions=%lld "
            "reclaimed-functions=%lld code=%lld reclaimed-code=%lld "
            "objects=%lld reclaimed-objects=%lld\n",
            label, source_bytes, success ? "yes" : "no",
            collected ? "yes" : "no", (long long) after.malloc_size,
            (long long) script_residency_delta(before.malloc_size,
                                                after.malloc_size),
            (long long) after.retained_size,
            (long long) script_residency_delta(before.retained_size,
                                                after.retained_size),
            (long long) after.function_count,
            (long long) script_residency_delta(before.function_count,
                                                after.function_count),
            (long long) after.code_size,
            (long long) script_residency_delta(before.code_size,
                                                after.code_size),
            (long long) after.object_count,
            (long long) script_residency_delta(before.object_count,
                                                after.object_count));
}

void js_rt_trace_script_quota_rejection(DomBridge *bridge,
                                        const char *reason)
{
    if (script_residency_trace_mode() == NULL || bridge == NULL
        || bridge->host == NULL || bridge->host->runtime == NULL) return;
    ScriptResidencySample sample = script_residency_sample(
        bridge->host->runtime);
    fprintf(stderr,
            "script-residency phase=quota-reject reason=%s count=%zu/%zu "
            "source-bytes=%zu/%zu reserved-bytes=%zu malloc=%lld "
            "retained=%lld functions=%lld code=%lld objects=%lld\n",
            reason == NULL ? "unknown" : reason,
            bridge->script_quota_count, bridge->maximum_scripts,
            bridge->script_quota_bytes, bridge->maximum_script_bytes,
            bridge->script_quota_reserved_bytes,
            (long long) sample.malloc_size,
            (long long) sample.retained_size,
            (long long) sample.function_count,
            (long long) sample.code_size,
            (long long) sample.object_count);
}

static bool js_rt_compile_source_admitted(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result,
    bool count_compile_attempt)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t policy_maximum = bridge == NULL ? 0
        : bridge->execution_policy.maximum_host_compile_source_bytes;
    /* Internal (trusted bootstrap) sources are pinned at build time to the
       bootstrap ceiling; the authored-script strict unit governs only
       page code. The policy value stays the reported limit. */
    size_t maximum = policy_maximum;
    if (source_kind == SCRIPT_COMPILE_SOURCE_INTERNAL
        && maximum == SCRIPT_PSP_STRICT_MAXIMUM_HOST_COMPILE_BYTES)
        maximum = SCRIPT_BOOTSTRAP_STRICT_MAXIMUM_HOST_COMPILE_BYTES;
    if (result != NULL) {
        if (count_compile_attempt) {
            js_rt_saturating_add_size(&result->host_compile_attempts, 1);
        }
        result->host_compile_source_limit_bytes = policy_maximum;
        result->last_compile_source_kind = source_kind;
        result->last_compile_source_bytes = length;
        snprintf(result->last_compile_source_name,
                 sizeof(result->last_compile_source_name), "%s",
                 name == NULL ? "<script>" : name);
    }
    if (maximum != 0 && length > maximum) {
        if (result != NULL) {
            result->last_compile_admission =
                SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT;
            js_rt_saturating_add_size(&result->host_compile_rejections, 1);
            js_rt_saturating_add_size(
                &result->host_compile_rejected_source_bytes, length);
            snprintf(result->error, sizeof(result->error),
                     "JavaScript source rejected before compile: "
                     "%s has %zu bytes; host compile limit is %zu bytes",
                     name == NULL ? "<script>" : name, length, maximum);
        }
        return false;
    }
    uint64_t projected_limit_us = bridge == NULL ? 0
        : bridge->execution_policy.maximum_host_compile_projected_us;
    size_t modeled_rate = bridge == NULL ? 0
        : bridge->execution_policy.modeled_compile_bytes_per_ms;
    if (projected_limit_us != 0 && modeled_rate != 0) {
        uint64_t projected_us =
            (uint64_t) length > UINT64_MAX / UINT64_C(1000)
                ? UINT64_MAX
                : (uint64_t) length * UINT64_C(1000) / (uint64_t) modeled_rate;
        if (projected_us > projected_limit_us) {
            if (result != NULL) {
                result->last_compile_admission =
                    SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME;
                js_rt_saturating_add_size(&result->host_compile_rejections, 1);
                js_rt_saturating_add_size(
                    &result->host_compile_projected_rejections, 1);
                js_rt_saturating_add_size(
                    &result->host_compile_projected_rejected_bytes, length);
                snprintf(result->error, sizeof(result->error),
                         "JavaScript source rejected before compile: "
                         "%s has %zu bytes; modeled compile time %llu us "
                         "exceeds the %llu us budget at %zu bytes/ms",
                         name == NULL ? "<script>" : name, length,
                         (unsigned long long) projected_us,
                         (unsigned long long) projected_limit_us,
                         modeled_rate);
            }
            return false;
        }
    }
    if (result != NULL) {
        result->last_compile_admission = SCRIPT_COMPILE_ADMISSION_ACCEPTED;
        if (count_compile_attempt) {
            js_rt_saturating_add_size(
                &result->host_compile_source_bytes, length);
        }
    }
    return true;
}

bool js_rt_admit_cached_compile_source(
    JSContext *context, size_t length, const char *name,
    ScriptCompileSourceKind source_kind, ScriptResult *result)
{
    if (js_rt_compile_source_admitted(
            context, length, name, source_kind, result, false)) return true;
    JSRuntime *runtime = JS_GetRuntime(context);
    ScriptResidencySample before = {0};
    if (script_residency_trace_mode() != NULL) {
        before = script_residency_sample(runtime);
    }
    script_residency_trace_compile(
        runtime, name, length, source_kind, before, false, false);
    return false;
}

bool js_rt_evaluate_compiled_source(
    JSContext *context, JSValue compiled, const char *name,
    size_t source_length, ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (JS_IsException(compiled)) return false;
    if (bridge != NULL && bridge->host != NULL
        && !js_rt_runtime_script_checkpoint(bridge->host, 1)) {
        JS_FreeValue(context, compiled);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, source_length, false);
        return false;
    }
    BudgetAllocationOwner previous_owner =
        bridge_document_owner_enter(bridge);
    unsigned ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
    unsigned split = script_split_enter(SCRIPT_SPLIT_JS);
    script_profile_enter(bridge == NULL ? NULL : bridge->host);
    compiled = JS_EvalFunction(context, compiled);
    script_profile_leave(bridge == NULL ? NULL : bridge->host);
    script_split_leave(split);
    work_ledger_leave(ledger);
    bridge_document_owner_leave(bridge, previous_owner);
    if (JS_IsException(compiled)) {
        JS_FreeValue(context, compiled);
        js_rt_record_exception(context, result);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, source_length, false);
        return false;
    }
    JS_FreeValue(context, compiled);
    script_residency_trace_settled(
        JS_GetRuntime(context), name, source_length, true);
    return true;
}

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
/*
 * Lazy function compilation (JS_SetLazyFunctionThreshold). An inner function
 * of a page script or module whose source is at least this long keeps no
 * bytecode until its first call (and its body is only validated, see
 * script_lazy_function_preparse() below); most of a
 * large site's functions never run during a page's life. The functions that
 * do run are compiled a second time, alone, when first called. See "lazy
 * function compilation" in docs/engineering/MEMORY_EXPERIMENTS.md for the
 * measurements behind the threshold. TILEFINCH_JS_LAZY_FUNCTIONS overrides
 * it (0 compiles everything eagerly). eval() and Function bodies are always
 * compiled eagerly.
 */
#define SCRIPT_LAZY_FUNCTION_MINIMUM_BYTES 128u

static uint32_t script_lazy_function_threshold(void)
{
    static bool resolved = false;
    static uint32_t threshold = SCRIPT_LAZY_FUNCTION_MINIMUM_BYTES;
    if (!resolved) {
        const char *value = getenv("TILEFINCH_JS_LAZY_FUNCTIONS");
        if (value != NULL && value[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (end != value && *end == '\0' && parsed <= UINT32_MAX) {
                threshold = (uint32_t) parsed;
            }
        }
        resolved = true;
    }
    return threshold;
}

/*
 * Preparsing (JS_SetLazyFunctionPreparse): the body of a function that will
 * be kept lazy is validated rather than parsed when its script compiles
 * (checked for early errors, its end found and the names it may take from
 * enclosing functions recorded), which cuts module compile time by about a
 * third. Early errors are still reported when the script compiles.
 * TILEFINCH_JS_PREPARSE=0 parses every body in full, as before, for A/B
 * comparisons.
 */
static bool script_lazy_function_preparse(void)
{
    static bool resolved = false;
    static bool enabled = true;
    if (!resolved) {
        const char *value = getenv("TILEFINCH_JS_PREPARSE");
        if (value != NULL && strcmp(value, "0") == 0) enabled = false;
        resolved = true;
    }
    return enabled;
}
#endif

JSValue js_rt_compile_source_type(JSContext *context, const char *source,
                                   size_t length, const char *name,
                                   int evaluation_type,
                                   ScriptCompileSourceKind source_kind,
                                   ScriptResult *result, bool *admitted)
{
    if (admitted != NULL) *admitted = false;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSRuntime *js_runtime = JS_GetRuntime(context);
    ScriptResidencySample residency_before = {0};
    if (script_residency_trace_mode() != NULL) {
        residency_before = script_residency_sample(js_runtime);
    }
    if (!js_rt_compile_source_admitted(
            context, length, name, source_kind, result, true)) {
        script_residency_trace_compile(
            js_runtime, name, length, source_kind, residency_before,
            false, false);
        return JS_EXCEPTION;
    }
    if (admitted != NULL) *admitted = true;
    uint64_t compile_started = js_rt_monotonic_time_ns();
    /* QuickJS's automatic cycle collection is triggered by object creation,
       not every compiler allocation. External loader segments can therefore
       hit the hard heap limit with dead cycles still occupying their compile
       reserve. Scale the pressure watermark with the upcoming source instead
       of collecting before tiny loader segments with hundreds of KiB free.
       This is a collection heuristic, not an admission guarantee: the hard
       heap limit still governs every allocation. Keep a 64 KiB minimum for
       parser overhead and the existing 1 MiB maximum; saturate before size
       multiplication. Do not retry an already executed script. */
    const size_t gc_minimum = 64u * 1024u;
    const size_t gc_maximum = 1024u * 1024u;
    size_t gc_reserve = length >= (gc_maximum - gc_minimum) / 16u
        ? gc_maximum : gc_minimum + length * 16u;
    if (source_kind == SCRIPT_COMPILE_SOURCE_EXTERNAL && bridge != NULL
        && bridge->host != NULL
        && script_runtime_heap_remaining(bridge->host) < gc_reserve) {
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t gc_started = js_rt_monotonic_time_ns();
        size_t remaining_before = script_runtime_heap_remaining(bridge->host);
#endif
        JS_RunGC(js_runtime);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t gc_finished = js_rt_monotonic_time_ns();
        char gc_message[192];
        snprintf(gc_message, sizeof(gc_message),
            "tilefinch-compile-pressure-gc: elapsed=%lluus remaining=%zu->%zu "
            "source-bytes=%zu",
            (unsigned long long) ((gc_finished - gc_started) / UINT64_C(1000)),
            remaining_before, script_runtime_heap_remaining(bridge->host), length);
        tilefinch_platform_log_message(gc_message);
#endif
    }
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    /* Read when the parse starts; imports compiled from inside this JS_Eval
       come back through here and restore it on their way out. */
    uint32_t previous_lazy_threshold = JS_GetLazyFunctionThreshold(js_runtime);
    JS_SetLazyFunctionThreshold(js_runtime, script_lazy_function_threshold());
    JS_SetLazyFunctionPreparse(js_runtime, script_lazy_function_preparse());
#endif
    ScriptRuntime *profile_owner = bridge == NULL ? NULL : bridge->host;
    unsigned compile_ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
    /* A compile entered from outside script (a parser-inserted script or
       module) is not inside any window this ledger splits; one from inside
       (dynamic import) is. */
    unsigned compile_split = script_split_enter(SCRIPT_SPLIT_COMPILE);
    script_profile_enter(profile_owner);
    if (profile_owner != NULL && profile_owner->profile != NULL)
        profile_owner->profile->compile_depth++;
    JSValue value = JS_Eval(
        context, source, length, name == NULL ? "<script>" : name,
        evaluation_type | JS_EVAL_FLAG_COMPILE_ONLY);
    script_profile_leave(profile_owner);
    script_split_leave(compile_split);
    work_ledger_leave(compile_ledger);
    if (profile_owner != NULL && profile_owner->profile != NULL)
        profile_owner->profile->compile_depth--;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    JS_SetLazyFunctionThreshold(js_runtime, previous_lazy_threshold);
#endif
    uint64_t compile_finished = js_rt_monotonic_time_ns();
    uint64_t compile_us = compile_finished >= compile_started
        ? (compile_finished - compile_started) / UINT64_C(1000) : 0;
    /* The parser polls the watchdog's interrupt handler, so an admitted
       compile can now end early through the normal parse-error unwind.
       Record the truthful classification; the elapsed slice still counts
       below because the aborted parse genuinely ran. */
    if (JS_IsException(value) && result != NULL && bridge != NULL
        && bridge->host != NULL && bridge->host->watchdog.interrupted) {
        result->last_compile_admission =
            SCRIPT_COMPILE_ADMISSION_ABORTED_WATCHDOG;
        js_rt_saturating_add_size(&result->host_compile_watchdog_aborts, 1);
        js_rt_saturating_add_size(
            &result->host_compile_watchdog_aborted_bytes, length);
    }
    if (result != NULL) {
        js_rt_saturating_add_u64(&result->host_compile_total_us, compile_us);
        if (compile_us > result->max_nonpreemptible_compile_us) {
            result->max_nonpreemptible_compile_us = compile_us;
            result->max_nonpreemptible_compile_bytes = length;
            result->max_nonpreemptible_compile_source_kind = source_kind;
            snprintf(result->max_nonpreemptible_compile_source_name,
                     sizeof(result->max_nonpreemptible_compile_source_name),
                     "%s", name == NULL ? "<script>" : name);
        }
        if (compile_us > compile_observation_threshold(bridge)) {
            js_rt_saturating_add_size(&result->nonpreemptible_compile_count, 1);
        }
    }
    script_residency_trace_compile(
        js_runtime, name, length, source_kind, residency_before,
        true, !JS_IsException(value));
    return value;
}

static bool evaluate_source_type_at(JSContext *context, const char *source,
                                    size_t length, const char *name,
                                    const char *module_base_url,
                                    const char *module_referrer_policy,
                                    TilefinchCredentialsMode module_credentials,
                                    int evaluation_type,
                                    ScriptCompileSourceKind source_kind,
                                    bool external_module,
                                    ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    bool module = (evaluation_type & JS_EVAL_TYPE_MASK)
        == JS_EVAL_TYPE_MODULE;
    const char *base = module_base_url == NULL ? name : module_base_url;
    if (module) {
        if (runtime == NULL
            || !js_rt_runtime_module_root_register(
                runtime, name, base, module_referrer_policy,
                module_credentials)) {
            return false;
        }
        /* `module_base_url` may be borrowed HTTP-cache metadata. Import
           normalization during compile can evict that cache entry, so use
           the runtime-owned copy registered above for import.meta as well. */
        base = js_rt_runtime_module_base_lookup(runtime, name);
        if (base == NULL) return false;
        if (!js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_LOADING)) return false;
    }
    bool admitted = false;
    uint64_t compile_started_us = tilefinch_platform_monotonic_time_us();
    JSValue value = module && external_module
        ? js_rt_module_compile_external(
            runtime, context, source, length, name, base, result, &admitted)
        : js_rt_compile_source_type(
            context, source, length, name, evaluation_type, source_kind,
            result, &admitted);
    if (result != NULL && (unsigned) source_kind < 6u) {
        result->compile_us[source_kind] +=
            tilefinch_platform_monotonic_time_us() - compile_started_us;
    }
    if (!admitted) {
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    if (!JS_IsException(value) && bridge != NULL && bridge->host != NULL
        && !js_rt_runtime_script_checkpoint(bridge->host, 1)) {
        JS_FreeValue(context, value);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, length, false);
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    if (module) {
        if (!JS_IsException(value)
            && !js_rt_module_set_import_meta(context, value, base, true)) {
            JS_FreeValue(context, value);
            value = JS_EXCEPTION;
        }
    }
    BudgetAllocationOwner previous_owner =
        bridge_document_owner_enter(bridge);
    uint64_t execute_started_us = tilefinch_platform_monotonic_time_us();
#if defined(CONFIG_PROPERTY_FAULT_TRACE)
    /* Reset the bounded diagnostic budget after compilation and browser-side
       currentScript setup, immediately before author bytecode executes. */
    if (!JS_IsException(value)
        && source_kind != SCRIPT_COMPILE_SOURCE_INTERNAL) {
        runtime_configure_property_fault_trace(JS_GetRuntime(context));
    }
#endif
    if (!JS_IsException(value)) {
        unsigned ledger = work_ledger_enter(WORK_LEDGER_SCRIPT);
        unsigned split = script_split_enter(SCRIPT_SPLIT_JS);
        script_profile_enter(runtime);
        value = JS_EvalFunction(context, value);
        script_profile_leave(runtime);
        script_split_leave(split);
        work_ledger_leave(ledger);
    }
    if (result != NULL && (unsigned) source_kind < 6u) {
        result->execute_us[source_kind] +=
            tilefinch_platform_monotonic_time_us() - execute_started_us;
    }
    bridge_document_owner_leave(bridge, previous_owner);
    if (JS_IsException(value)) {
        JS_FreeValue(context, value);
        js_rt_record_exception(context, result);
        script_residency_trace_settled(
            JS_GetRuntime(context), name, length, false);
        if (module) {
            (void) js_rt_runtime_module_root_state_set(
                runtime, name, SCRIPT_MODULE_MAP_FAILED);
        }
        return false;
    }
    JS_FreeValue(context, value);
    script_residency_trace_settled(
        JS_GetRuntime(context), name, length, true);
    if (module) {
        (void) js_rt_runtime_module_root_state_set(
            runtime, name, SCRIPT_MODULE_MAP_EVALUATED);
    }
    return true;
}

bool js_rt_evaluate_source_type_at(JSContext *context, const char *source,
                             size_t length, const char *name,
                             const char *module_base_url,
                             const char *module_referrer_policy,
                             TilefinchCredentialsMode module_credentials,
                             int evaluation_type,
                             ScriptCompileSourceKind source_kind,
                             ScriptResult *result)
{
    return evaluate_source_type_at(
        context, source, length, name, module_base_url,
        module_referrer_policy, module_credentials, evaluation_type,
        source_kind, false, result);
}

bool js_rt_evaluate_external_module_at(
    JSContext *context, const char *source, size_t length, const char *name,
    const char *response_url, const char *module_referrer_policy,
    TilefinchCredentialsMode module_credentials, ScriptResult *result)
{
    return evaluate_source_type_at(
        context, source, length, name, response_url, module_referrer_policy,
        module_credentials, JS_EVAL_TYPE_MODULE, SCRIPT_COMPILE_SOURCE_MODULE,
        true, result);
}

static bool evaluate_source_type(JSContext *context, const char *source,
                                 size_t length, const char *name,
                                 int evaluation_type,
                                 ScriptCompileSourceKind source_kind,
                                 ScriptResult *result)
{
    return js_rt_evaluate_source_type_at(context, source, length, name, name,
                                   NULL, TILEFINCH_CREDENTIALS_SAME_ORIGIN,
                                   evaluation_type, source_kind, result);
}

bool js_rt_evaluate_source(JSContext *context, const char *source,
                     size_t length, const char *name,
                     ScriptResult *result)
{
    return evaluate_source_type(context, source, length, name,
                                JS_EVAL_TYPE_GLOBAL,
                                SCRIPT_COMPILE_SOURCE_INTERNAL, result);
}

void js_rt_capture_error_source_context(const char *source, size_t length,
                                  const char *source_url,
                                  ScriptResult *result)
{
    if (source == NULL || source_url == NULL || source_url[0] == '\0'
        || result == NULL || result->error[0] == '\0') return;
    snprintf(result->error_source_context,
             sizeof(result->error_source_context),
             "capture-pending url=%s length=%zu", source_url, length);
    const char *position = strstr(result->error, source_url);
    if (position != NULL) {
        position += strlen(source_url);
        if (*position++ != ':') position = NULL;
    }
    if (position == NULL) {
        const char *open = strchr(result->error, '(');
        const char *close = open == NULL ? NULL : strchr(open, ')');
        if (close == NULL) return;
        const char *column_colon = close;
        while (column_colon > open && column_colon[-1] != ':') {
            column_colon--;
        }
        if (column_colon == open) return;
        const char *line_colon = column_colon - 1;
        while (line_colon > open && line_colon[-1] != ':') line_colon--;
        if (line_colon == open) return;
        position = line_colon;
    }
    char *end = NULL;
    unsigned long line = strtoul(position, &end, 10);
    if (end == position || *end++ != ':' || line == 0) return;
    unsigned long column = strtoul(end, &end, 10);
    if (column == 0) return;
    size_t offset = 0;
    for (unsigned long current = 1; current < line && offset < length;
         current++) {
        const char *newline = memchr(source + offset, '\n', length - offset);
        if (newline == NULL) return;
        offset = (size_t) (newline - source) + 1;
    }
    if (column - 1 > length - offset) return;
    offset += (size_t) column - 1;
    size_t start = offset > 900 ? offset - 900 : 0;
    size_t finish = offset + 900 < length ? offset + 900 : length;
    size_t written = 0;
    for (size_t at = start;
         at < finish && written + 1 < sizeof(result->error_source_context);
         at++) {
        unsigned char character = (unsigned char) source[at];
        result->error_source_context[written++] =
            character < 32 || character == 127 ? ' ' : (char) character;
    }
    result->error_source_context[written] = '\0';
}


static bool document_current_script_get(JSContext *context,
                                        ScriptCurrentScriptScope *scope,
                                        ScriptResult *result)
{
    (void) result;
    if (scope == NULL) return false;
    scope->value = JS_UNDEFINED;
    scope->owner_document_identity = 0;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    scope->value = JS_DupValue(context, bridge->current_script);
    scope->owner_document_identity =
        bridge->current_script_owner_document_identity;
    return true;
}

/* Consumes value, matching JS_SetPropertyStr's ownership contract. */
static bool document_current_script_set(JSContext *context, JSValue value,
                                        ScriptResult *result)
{
    (void) result;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) {
        JS_FreeValue(context, value);
        return false;
    }
    JS_FreeValue(context, bridge->current_script);
    bridge->current_script = value;
    bridge->current_script_owner_document_identity = 0;
    return true;
}

static bool set_current_script(JSContext *context, lxb_dom_node_t *node,
                               ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    bool source_override = bridge != NULL
        && bridge->current_script_source_active;
    const char *source_key = source_override
        ? bridge->current_script_source_key : NULL;
    size_t source_section = source_override
        ? bridge->current_script_source_section : 0;
    if (!source_override && bridge != NULL) {
        for (size_t i = 0; i < bridge->script_source_node_count; i++) {
            ScriptSourceNode *source = &bridge->script_source_nodes[i];
            if (source->node == node) {
                source_override = true;
                source_key = source->key;
                source_section = source->section;
                break;
            }
        }
    }
    int64_t handle = source_override ? 0 : js_rt_bridge_register_node(bridge, node);
    JSValue wrap = source_override
        ? bridge->trusted_stable_script_wrap : bridge->trusted_node_wrap;
    if (!JS_IsFunction(context, wrap)) return false;
    JSValue arguments[2];
    int argument_count = 1;
    if (source_override) {
        arguments[0] = JS_NewString(context, source_key);
        arguments[1] = JS_NewInt64(
            context, (int64_t) source_section);
        argument_count = 2;
    } else arguments[0] = JS_NewInt64(context, handle);
    JSValue current = JS_Call(
        context, wrap, JS_UNDEFINED, argument_count, arguments);
    for (int index = 0; index < argument_count; index++) {
        JS_FreeValue(context, arguments[index]);
    }
    bool current_exception = JS_IsException(current);
    bool current_set = !current_exception
        && document_current_script_set(context, current, result);
    if (current_exception || !current_set) {
        if (current_exception) JS_FreeValue(context, current);
        if (current_exception) js_rt_record_exception(context, result);
        return false;
    }
    bridge->current_script_owner_document_identity = js_rt_node_owner_identity(node);
    return true;
}

bool js_rt_current_script_scope_begin(JSContext *context,
                                lxb_dom_node_t *node, bool module,
                                ScriptCurrentScriptScope *previous,
                                ScriptResult *result)
{
    if (!document_current_script_get(context, previous, result)) return false;
    bool set = module
        ? document_current_script_set(context, JS_NULL, result)
        : set_current_script(context, node, result);
    if (!set) {
        (void) document_current_script_set(
            context, JS_DupValue(context, previous->value), result);
        DomBridge *bridge = JS_GetContextOpaque(context);
        if (bridge != NULL) {
            bridge->current_script_owner_document_identity =
                previous->owner_document_identity;
        }
        JS_FreeValue(context, previous->value);
        previous->value = JS_UNDEFINED;
        previous->owner_document_identity = 0;
    }
    return set;
}

bool js_rt_current_script_scope_end(JSContext *context,
                              ScriptCurrentScriptScope *previous,
                              ScriptResult *result)
{
    if (previous == NULL) return false;
    bool restored = document_current_script_set(
        context, JS_DupValue(context, previous->value), result);
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (restored && bridge != NULL) {
        bridge->current_script_owner_document_identity =
            previous->owner_document_identity;
    }
    JS_FreeValue(context, previous->value);
    previous->value = JS_UNDEFINED;
    previous->owner_document_identity = 0;
    return restored;
}

static bool evaluate_script_node(JSContext *context, lxb_dom_node_t *node,
                                 ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    size_t source_length = 0;
    (void) document_attribute(node, "src", &source_length);
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "src", 3)) {
        result->external_scripts_skipped++;
        return true;
    }
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    bool module = false;
    bool executable = script_type_attribute_classify(
        type, type_length, &module);
    if (module) {
        /* Module loading is deliberately outside this bounded host. */
        result->external_scripts_skipped++;
        return true;
    }
    if (!executable) return true;
    source_length = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        if (length > SIZE_MAX - source_length) return false;
        source_length += length;
    }
    if (source_length != 0) {
        size_t execution_reserve =
            script_runtime_inline_execution_reserve(
                bridge->host, source_length);
        size_t required = source_length > SIZE_MAX
                          - execution_reserve
            ? SIZE_MAX
            : source_length + execution_reserve;
        size_t remaining = script_runtime_heap_remaining(bridge->host);
        if (remaining < required) {
            size_t before = budget_remaining(bridge->budget);
            (void) script_runtime_collect_and_trim(bridge->host);
            size_t after = budget_remaining(bridge->budget);
            remaining = script_runtime_heap_available(bridge->host);
            size_t reclaimed = after > before ? after - before : 0;
            if (remaining < required) {
                budget_record_pressure(
                    bridge->budget, BUDGET_PRESSURE_JAVASCRIPT,
                    source_length, reclaimed);
                if (tilefinch_trace_script_failures()) {
                    fprintf(stderr,
                            "inline-script-admission bytes=%zu "
                            "heap-remaining=%zu reserve=%zu admitted=no\n",
                            source_length, remaining, execution_reserve);
                }
                return true;
            }
            budget_record_pressure(
                bridge->budget, BUDGET_PRESSURE_JAVASCRIPT, 0, reclaimed);
        }
    }
    ScriptQuotaReservation quota = {0};
    ScriptQuotaReserveResult quota_status = js_rt_bridge_script_quota_reserve(
        bridge, SCRIPT_QUOTA_NEW_EXECUTABLE, source_length, &quota);
    if (quota_status != SCRIPT_QUOTA_RESERVE_GRANTED
        || !js_rt_bridge_script_quota_commit(bridge, &quota, source_length)) {
        js_rt_bridge_script_quota_abort(bridge, &quota);
        snprintf(result->error, sizeof(result->error),
                 "initial inline script exceeded executable quota");
        return false;
    }
    char *source_copy = budget_malloc(bridge->budget, source_length + 1);
    if (source_copy == NULL) return false;
    size_t copied = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        size_t length = 0;
        const char *source = document_text_data(child, &length);
        if (source == NULL || length == 0) continue;
        memcpy(source_copy + copied, source, length);
        copied += length;
    }
    source_copy[copied] = '\0';
    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    if (!js_rt_current_script_scope_begin(
            context, node, false, &previous_current_script, result)) {
        budget_free(bridge->budget, source_copy);
        return false;
    }
    bool evaluated = source_length == 0 || evaluate_source_type(
        context, source_copy, source_length, "<inline-script>",
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_INLINE, result);
    if (!js_rt_current_script_scope_end(
            context, &previous_current_script, result)) evaluated = false;
    budget_free(bridge->budget, source_copy);
    if (evaluated) result->scripts_evaluated++;
    return evaluated;
}

static bool evaluate_scripts(JSContext *context, lxb_dom_node_t *node,
                             ScriptResult *result)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return false;
    ScriptElementSnapshot *scripts = budget_calloc(
        bridge->budget, SCRIPT_DYNAMIC_NODE_LIMIT, sizeof(*scripts));
    if (scripts == NULL) return false;
    size_t script_count = 0;
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (!node_name_is(at, "script")) continue;
        if (script_count >= SCRIPT_DYNAMIC_NODE_LIMIT) break;
        ScriptElementState *state = js_rt_script_element_state_register(
            bridge, at, true);
        if (state == NULL) continue;
        state->programmatic = false;
        state->force_async = false;
        state->already_started = true;
        int64_t handle = js_rt_bridge_register_node(bridge, at);
        if (handle != 0) {
            scripts[script_count++] = (ScriptElementSnapshot) {
                .node_handle = handle, .identity = state->identity
            };
        }
    }
    bool complete = true;
    for (size_t index = 0; index < script_count; index++) {
        size_t slot = 0;
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, scripts[index].node_handle, &slot)) continue;
        lxb_dom_node_t *node = bridge->nodes[slot];
        ScriptElementState *state = js_rt_script_element_state_find(
            bridge, node);
        if (state == NULL || state->identity != scripts[index].identity) {
            continue;
        }
        const ScriptRuntime *runtime = bridge->host;
        size_t failures_before = bridge->budget->failure_count;
        size_t heap_rejections_before = runtime == NULL
            ? 0 : script_runtime_heap_rejections(runtime);
        if (!evaluate_script_node(context, node, result)) {
            result->scripts_failed++;
            /* An uncaught author exception terminates only that script. Keep
               loading the document, but retain genuinely fatal engine and
               budget failures as pipeline failures. Decide from what this
               script actually hit, never from its (author-controlled) error
               text: `throw Error("allocation failed")` is an author error. */
            if (runtime == NULL || runtime->watchdog.interrupted
                || bridge->budget->failure_count != failures_before
                || script_runtime_heap_rejections(runtime)
                       != heap_rejections_before) {
                complete = false;
                break;
            }
        }
    }
    budget_free(bridge->budget, scripts);
    return complete;
}

FetchScheduler *script_runtime_fetch_scheduler(ScriptRuntime *runtime)
{
    if (runtime == NULL) return NULL;
    DomBridge *bridge = &runtime->bridge;
    if (bridge->fetch_scheduler == NULL) {
        bridge->fetch_scheduler = bridge_fetch_scheduler_create(
            bridge, bridge->maximum_script_bytes);
    }
    return bridge->fetch_scheduler;
}
