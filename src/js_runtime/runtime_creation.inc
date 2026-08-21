typedef enum {
    BOOTSTRAP_BYTECODE_UNAVAILABLE = 0,
    BOOTSTRAP_BYTECODE_EVALUATED,
    BOOTSTRAP_BYTECODE_FAILED
} BootstrapBytecodeResult;

static uint64_t bootstrap_source_hash(const char *source, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) source[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Trusted bytecode avoids QuickJS parsing, but the embedder's explicit
   source-admission contract still applies to the source it represents.
   Record the same bounded admission attempt as the source compiler without
   charging compile time or claiming that JS_Eval(COMPILE_ONLY) ran. */
static bool runtime_bootstrap_source_admitted(
    ScriptRuntime *runtime, size_t length, const char *source_url)
{
    ScriptResult *result = &runtime->result;
    const ScriptExecutionPolicy *policy =
        &runtime->bridge.execution_policy;
    size_t maximum = policy->maximum_host_compile_source_bytes;
    js_rt_saturating_add_size(&result->host_compile_attempts, 1);
    result->host_compile_source_limit_bytes = maximum;
    result->last_compile_source_kind = SCRIPT_COMPILE_SOURCE_INTERNAL;
    result->last_compile_source_bytes = length;
    snprintf(result->last_compile_source_name,
             sizeof(result->last_compile_source_name), "%s",
             source_url == NULL ? "<bootstrap>" : source_url);
    if (maximum != 0 && length > maximum) {
        result->last_compile_admission =
            SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT;
        js_rt_saturating_add_size(&result->host_compile_rejections, 1);
        js_rt_saturating_add_size(
            &result->host_compile_rejected_source_bytes, length);
        snprintf(result->error, sizeof(result->error),
                 "JavaScript source rejected before compile: "
                 "%s has %zu bytes; host compile limit is %zu bytes",
                 source_url == NULL ? "<bootstrap>" : source_url,
                 length, maximum);
        return false;
    }
    uint64_t projected_limit_us =
        policy->maximum_host_compile_projected_us;
    size_t modeled_rate = policy->modeled_compile_bytes_per_ms;
    if (projected_limit_us != 0 && modeled_rate != 0) {
        uint64_t projected_us =
            (uint64_t) length > UINT64_MAX / UINT64_C(1000)
                ? UINT64_MAX
                : (uint64_t) length * UINT64_C(1000)
                    / (uint64_t) modeled_rate;
        if (projected_us > projected_limit_us) {
            result->last_compile_admission =
                SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME;
            js_rt_saturating_add_size(
                &result->host_compile_rejections, 1);
            js_rt_saturating_add_size(
                &result->host_compile_projected_rejections, 1);
            js_rt_saturating_add_size(
                &result->host_compile_projected_rejected_bytes, length);
            snprintf(result->error, sizeof(result->error),
                     "JavaScript source rejected before compile: "
                     "%s has %zu bytes; modeled compile time %llu us "
                     "exceeds the %llu us budget at %zu bytes/ms",
                     source_url == NULL ? "<bootstrap>" : source_url,
                     length, (unsigned long long) projected_us,
                     (unsigned long long) projected_limit_us,
                     modeled_rate);
            return false;
        }
    }
    result->last_compile_admission = SCRIPT_COMPILE_ADMISSION_ACCEPTED;
    js_rt_saturating_add_size(&result->host_compile_source_bytes, length);
    return true;
}

static BootstrapBytecodeResult runtime_evaluate_bootstrap_bytecode(
    ScriptRuntime *runtime, const char *source, size_t length,
    const char *source_url, const BrowserBootstrapBytecode *bytecode,
    bool prefer_source_on_tight_heap)
{
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
    if (getenv("TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE") != NULL) {
        return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    }
#else
    (void) prefer_source_on_tight_heap;
#endif
    if (bytecode == NULL) return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    if (bytecode->data == NULL || bytecode->bytecode_length == 0
        || bytecode->source_length != length
        || (source != NULL
            && bytecode->source_hash
                   != bootstrap_source_hash(source, length))) {
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_bytecode_restore_failures, 1);
        return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    }
    /* Prefer source only when the serialized representation is still more
       than twice its authored source. This is deliberately representation-
       based: source formatting and generator changes must not leave a stale
       hard-coded size heuristic selecting the slower path. */
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
    if (prefer_source_on_tight_heap
        && runtime->base_memory_limit <= 6u * 1024u * 1024u
        && bytecode->bytecode_length > length
        && bytecode->bytecode_length - length > length) {
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_bytecode_source_preferences, 1);
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_bytecode_preferred_source_bytes,
            length);
        return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    }
#endif
    uint64_t started = js_rt_monotonic_time_ns();
    JSValue function = JS_ReadObject(
        runtime->context, bytecode->data, bytecode->bytecode_length,
        JS_READ_OBJ_BYTECODE | JS_READ_OBJ_ROM_DATA);
    uint64_t deserialized = js_rt_monotonic_time_ns();
    uint64_t restore_us =
        deserialized >= started
            ? (deserialized - started) / UINT64_C(1000) : 0;
    if (JS_IsException(function)) {
        JSValue exception = JS_GetException(runtime->context);
        JS_FreeValue(runtime->context, exception);
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_bytecode_restore_failures, 1);
        return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    }
#ifndef TILEFINCH_NO_TRACE
    /* Host-only deterministic fault injection keeps the source-fallback
       accounting regression test independent of allocator timing. */
    if (getenv("TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE") != NULL) {
        JS_FreeValue(runtime->context, function);
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_bytecode_restore_failures, 1);
        return BOOTSTRAP_BYTECODE_UNAVAILABLE;
    }
#endif
    /* Commit source-admission telemetry only once the bytecode is usable.
       Allocation, decompression, or deserialization failure falls back to
       source compilation, whose admission path must remain the sole attempt
       recorded for that bootstrap. */
    if (!runtime_bootstrap_source_admitted(
            runtime, length, source_url)) {
        JS_FreeValue(runtime->context, function);
        return BOOTSTRAP_BYTECODE_FAILED;
    }
    if (!js_rt_runtime_script_checkpoint(runtime, 1)) {
        JS_FreeValue(runtime->context, function);
        return BOOTSTRAP_BYTECODE_FAILED;
    }
    BudgetAllocationOwner previous_owner =
        bridge_document_owner_enter(&runtime->bridge);
    JSValue evaluated = JS_EvalFunction(runtime->context, function);
    bridge_document_owner_leave(&runtime->bridge, previous_owner);
    if (JS_IsException(evaluated)) {
        JS_FreeValue(runtime->context, evaluated);
        js_rt_record_exception(runtime->context, &runtime->result);
        return BOOTSTRAP_BYTECODE_FAILED;
    }
    JS_FreeValue(runtime->context, evaluated);
    js_rt_saturating_add_size(
        &runtime->result.bootstrap_bytecode_restores, 1);
    js_rt_saturating_add_size(
        &runtime->result.bootstrap_bytecode_stored_bytes,
        bytecode->bytecode_length);
    js_rt_saturating_add_size(
        &runtime->result.bootstrap_bytecode_bytes,
        bytecode->bytecode_length);
    js_rt_saturating_add_u64(
        &runtime->result.bootstrap_bytecode_restore_us,
        restore_us);
    return BOOTSTRAP_BYTECODE_EVALUATED;
}

static bool runtime_evaluate_bootstrap_profiled(
    ScriptRuntime *runtime, const char *source, size_t length,
    const char *source_url, const BrowserBootstrapBytecode *bytecode,
    bool prefer_source_on_tight_heap,
    bool trace, uint64_t *bootstrap_total_us,
    uint64_t *bootstrap_compile_us)
{
    uint64_t compile_before = runtime->result.host_compile_total_us;
    uint64_t started = js_rt_monotonic_time_ns();
    BootstrapBytecodeResult restored = runtime_evaluate_bootstrap_bytecode(
        runtime, source, length, source_url, bytecode,
        prefer_source_on_tight_heap);
    bool evaluated = restored == BOOTSTRAP_BYTECODE_EVALUATED;
    if (restored == BOOTSTRAP_BYTECODE_UNAVAILABLE) {
        if (source == NULL) {
            snprintf(runtime->result.error, sizeof(runtime->result.error),
                     "embedded JavaScript bytecode could not be restored: %s",
                     source_url == NULL ? "<bootstrap>" : source_url);
        } else {
            evaluated = runtime_evaluate_checkpointed(
                runtime, source, length, source_url);
        }
    }
    uint64_t finished = js_rt_monotonic_time_ns();
    uint64_t total_us = finished >= started
        ? (finished - started) / UINT64_C(1000) : 0;
    uint64_t compile_us =
        runtime->result.host_compile_total_us >= compile_before
        ? runtime->result.host_compile_total_us - compile_before : 0;
    js_rt_saturating_add_u64(bootstrap_total_us, total_us);
    js_rt_saturating_add_u64(bootstrap_compile_us, compile_us);
    if (trace) {
        fprintf(stderr,
                "javascript-startup-bootstrap name=\"%s\" bytes=%zu "
                "total-us=%llu compile-us=%llu execute-us=%llu "
                "heap-bytes=%zu bytecode=%s ok=%s\n",
                source_url, length, (unsigned long long) total_us,
                (unsigned long long) compile_us,
                (unsigned long long) (total_us >= compile_us
                    ? total_us - compile_us : 0),
                budget_quickjs_pool_js_malloc_current(
                    runtime->quickjs_pool),
                restored == BOOTSTRAP_BYTECODE_EVALUATED ? "yes" : "no",
                evaluated ? "yes" : "no");
    }
    return evaluated;
}

typedef enum {
    LAZY_BOOTSTRAP_CANVAS = 0,
    LAZY_BOOTSTRAP_INDEXED_DB,
    LAZY_BOOTSTRAP_MOTION,
    LAZY_BOOTSTRAP_STREAMS,
    LAZY_BOOTSTRAP_COUNT
} LazyBootstrapFeature;

_Static_assert(LAZY_BOOTSTRAP_COUNT == SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT,
               "lazy bootstrap state must cover every feature");

typedef enum {
    LAZY_BOOTSTRAP_GLOBAL = 0,
    LAZY_BOOTSTRAP_CANVAS_PROTOTYPE
} LazyBootstrapTarget;

typedef struct {
    const char *name;
    uint8_t feature;
    uint8_t target;
} LazyBootstrapProperty;

static const LazyBootstrapProperty lazy_bootstrap_properties[] = {
    {"CanvasGradient", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"CanvasPattern", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"CanvasRenderingContext2D", LAZY_BOOTSTRAP_CANVAS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"DOMMatrix", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"DOMMatrixReadOnly", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"ImageData", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"Path2D", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"getContext", LAZY_BOOTSTRAP_CANVAS,
     LAZY_BOOTSTRAP_CANVAS_PROTOTYPE},
    {"toDataURL", LAZY_BOOTSTRAP_CANVAS,
     LAZY_BOOTSTRAP_CANVAS_PROTOTYPE},
    {"toBlob", LAZY_BOOTSTRAP_CANVAS,
     LAZY_BOOTSTRAP_CANVAS_PROTOTYPE},
    {"indexedDB", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"DOMStringList", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBRequest", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBOpenDBRequest", LAZY_BOOTSTRAP_INDEXED_DB,
     LAZY_BOOTSTRAP_GLOBAL},
    {"IDBKeyRange", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBTransaction", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBObjectStore", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBIndex", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBCursor", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBCursorWithValue", LAZY_BOOTSTRAP_INDEXED_DB,
     LAZY_BOOTSTRAP_GLOBAL},
    {"IDBDatabase", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"IDBFactory", LAZY_BOOTSTRAP_INDEXED_DB, LAZY_BOOTSTRAP_GLOBAL},
    {"AnimationEvent", LAZY_BOOTSTRAP_MOTION, LAZY_BOOTSTRAP_GLOBAL},
    {"ReadableStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"ReadableStreamDefaultReader", LAZY_BOOTSTRAP_STREAMS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"WritableStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"WritableStreamDefaultWriter", LAZY_BOOTSTRAP_STREAMS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"TransformStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"TextEncoderStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"TextDecoderStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL}
};

static const char *runtime_lazy_bootstrap_name(LazyBootstrapFeature feature)
{
    switch (feature) {
    case LAZY_BOOTSTRAP_CANVAS: return "Canvas";
    case LAZY_BOOTSTRAP_INDEXED_DB: return "IndexedDB";
    case LAZY_BOOTSTRAP_MOTION: return "Motion";
    case LAZY_BOOTSTRAP_STREAMS: return "Streams";
    default: return "Platform feature";
    }
}

static JSValue runtime_lazy_bootstrap_target(
    JSContext *context, uint8_t target)
{
    JSValue global = JS_GetGlobalObject(context);
    if (target == LAZY_BOOTSTRAP_GLOBAL || JS_IsException(global)) {
        return global;
    }
    JSValue constructor = JS_GetPropertyStr(
        context, global, "HTMLCanvasElement");
    JS_FreeValue(context, global);
    if (JS_IsException(constructor)) return constructor;
    JSValue prototype = JS_GetPropertyStr(context, constructor, "prototype");
    JS_FreeValue(context, constructor);
    return prototype;
}

static void runtime_remove_lazy_bootstrap_properties(
    JSContext *context, LazyBootstrapFeature feature)
{
    for (size_t i = 0;
         i < sizeof(lazy_bootstrap_properties)
                 / sizeof(lazy_bootstrap_properties[0]); i++) {
        const LazyBootstrapProperty *property =
            &lazy_bootstrap_properties[i];
        if (property->feature != feature) continue;
        JSValue target = runtime_lazy_bootstrap_target(
            context, property->target);
        if (JS_IsException(target)) {
            JS_FreeValue(context, target);
            continue;
        }
        JSAtom atom = JS_NewAtom(context, property->name);
        if (atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(context, target, atom, 0);
            JS_FreeAtom(context, atom);
        }
        JS_FreeValue(context, target);
    }
}

static bool runtime_install_lazy_bootstrap_feature(
    ScriptRuntime *runtime, LazyBootstrapFeature feature);

static bool runtime_load_lazy_bootstrap(
    ScriptRuntime *runtime, LazyBootstrapFeature feature)
{
    if (runtime == NULL || feature >= LAZY_BOOTSTRAP_COUNT) return false;
    uint8_t *state = &runtime->lazy_bootstrap_state[feature];
    if (*state == 2) return true;
    if (*state == 1 || *state == 3) return false;
    *state = 1;
    runtime_remove_lazy_bootstrap_properties(runtime->context, feature);
    bool trace = getenv("TILEFINCH_TRACE_JS_STARTUP") != NULL;
    uint64_t total_us = 0;
    uint64_t compile_us = 0;
    bool evaluated = false;
    if (feature == LAZY_BOOTSTRAP_CANVAS) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_canvas,
                         browser_bootstrap_canvas_bytecode),
            "<browser-canvas-lazy>", &browser_bootstrap_canvas_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_INDEXED_DB) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_indexeddb,
                         browser_bootstrap_indexeddb_bytecode),
            "<browser-indexeddb-lazy>",
            &browser_bootstrap_indexeddb_bytecode, false, trace,
            &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_MOTION) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_motion,
                         browser_bootstrap_motion_bytecode),
            "<browser-motion-lazy>",
            &browser_bootstrap_motion_bytecode, false, trace,
            &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_STREAMS) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_streams,
                         browser_bootstrap_streams_bytecode),
            "<browser-streams-lazy>",
            &browser_bootstrap_streams_bytecode, false, trace,
            &total_us, &compile_us);
    }
    *state = evaluated ? 2 : 3;
    if (evaluated) {
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_lazy_module_loads, 1);
    } else {
        js_rt_saturating_add_size(
            &runtime->result.bootstrap_lazy_module_failures, 1);
        /* Keep standards-visible names present after a bounded load failure.
           Subsequent access fails deterministically instead of observing an
           engine-dependent half-installed surface. */
        (void) runtime_install_lazy_bootstrap_feature(runtime, feature);
    }
    return evaluated;
}

static JSValue js_runtime_lazy_bootstrap_property(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv, int magic)
{
    (void) this_value;
    if (magic < 0
        || (size_t) magic
               >= sizeof(lazy_bootstrap_properties)
                      / sizeof(lazy_bootstrap_properties[0])) {
        return JS_ThrowInternalError(context,
                                     "invalid lazy bootstrap property");
    }
    const LazyBootstrapProperty *property =
        &lazy_bootstrap_properties[magic];
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_JS_STARTUP") != NULL
        && runtime != NULL
        && runtime->lazy_bootstrap_state[property->feature] == 0) {
        fprintf(stderr, "javascript-lazy-bootstrap trigger=\"%s\"\n",
                property->name);
    }
#endif
    if (!runtime_load_lazy_bootstrap(
            runtime, (LazyBootstrapFeature) property->feature)) {
        return JS_ThrowInternalError(
            context, "%s support could not be initialized",
            runtime_lazy_bootstrap_name(
                (LazyBootstrapFeature) property->feature));
    }
    JSValue target = runtime_lazy_bootstrap_target(context, property->target);
    if (JS_IsException(target)) return target;
    if (argc != 0) {
        int result = JS_SetPropertyStr(
            context, target, property->name, JS_DupValue(context, argv[0]));
        JS_FreeValue(context, target);
        return result < 0 ? JS_EXCEPTION : JS_UNDEFINED;
    }
    JSValue result = JS_GetPropertyStr(context, target, property->name);
    JS_FreeValue(context, target);
    return result;
}

static bool runtime_install_lazy_bootstrap_feature(
    ScriptRuntime *runtime, LazyBootstrapFeature feature)
{
    if (runtime == NULL || runtime->context == NULL) return false;
    for (size_t i = 0;
         i < sizeof(lazy_bootstrap_properties)
                 / sizeof(lazy_bootstrap_properties[0]); i++) {
        const LazyBootstrapProperty *property =
            &lazy_bootstrap_properties[i];
        if (property->feature != feature) continue;
        JSValue target = runtime_lazy_bootstrap_target(
            runtime->context, property->target);
        if (JS_IsException(target)) {
            JS_FreeValue(runtime->context, target);
            return false;
        }
        JSAtom atom = JS_NewAtom(runtime->context, property->name);
        JSValue getter = JS_NewCFunctionMagic(
            runtime->context, js_runtime_lazy_bootstrap_property,
            property->name, 0, JS_CFUNC_generic_magic, (int) i);
        JSValue setter = JS_NewCFunctionMagic(
            runtime->context, js_runtime_lazy_bootstrap_property,
            property->name, 1, JS_CFUNC_generic_magic, (int) i);
        bool ok = atom != JS_ATOM_NULL
            && !JS_IsException(getter) && !JS_IsException(setter);
        if (ok) {
            ok = JS_DefinePropertyGetSet(
                runtime->context, target, atom, getter, setter,
                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) >= 0;
            /* DefinePropertyGetSet consumes both functions. */
            getter = JS_UNDEFINED;
            setter = JS_UNDEFINED;
        }
        JS_FreeValue(runtime->context, getter);
        JS_FreeValue(runtime->context, setter);
        JS_FreeAtom(runtime->context, atom);
        JS_FreeValue(runtime->context, target);
        if (!ok) return false;
    }
    return true;
}

static JSValue js_runtime_ensure_canvas_bootstrap(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !runtime_load_lazy_bootstrap(
            bridge->host, LAZY_BOOTSTRAP_CANVAS)) {
        return JS_ThrowInternalError(
            context, "Canvas support could not be initialized");
    }
    return JS_TRUE;
}

static JSValue js_runtime_ensure_motion_bootstrap(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !runtime_load_lazy_bootstrap(
            bridge->host, LAZY_BOOTSTRAP_MOTION)) {
        return JS_ThrowInternalError(
            context, "Motion support could not be initialized");
    }
    return JS_TRUE;
}

static JSValue js_runtime_run_worker(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL || argc < 3)
        return JS_ThrowTypeError(context, "invalid worker invocation");
    size_t source_length = 0;
    const char *source = JS_ToCStringLen(context, &source_length, argv[1]);
    const char *url = JS_ToCString(context, argv[2]);
    if (source == NULL || url == NULL) {
        if (source != NULL) JS_FreeCString(context, source);
        if (url != NULL) JS_FreeCString(context, url);
        return JS_EXCEPTION;
    }
    const TilefinchContentSecurityPolicy *csp =
        &bridge->document->content_security_policy;
    bool allowed = tilefinch_csp_allows_worker(csp, url);
    if (!allowed || source_length > bridge->maximum_script_file_bytes) {
        JS_FreeCString(context, source);
        JS_FreeCString(context, url);
        return allowed
            ? JS_ThrowRangeError(context, "worker source quota exceeded")
            : JS_ThrowTypeError(context, "worker blocked by Content Security Policy");
    }
    static const char prefix[] = "(function(scope){const self=scope;const globalThis=scope;const postMessage=scope.postMessage;const addEventListener=scope.addEventListener;const removeEventListener=scope.removeEventListener;const importScripts=()=>{};const __tilefinchWorkerMeta=scope.__tilefinchWorkerMeta;const __tilefinchWorkerImport=u=>new Promise((resolve,reject)=>{let tries=0;const attempt=()=>{import(String(u)).then(resolve,err=>{if(++tries>=40)reject(err);else setTimeout(attempt,100);});};setTimeout(attempt,400);});\n";
    static const char suffix[] = "\n})";
    if (source_length > SIZE_MAX - sizeof(prefix) - sizeof(suffix)) {
        JS_FreeCString(context, source);
        JS_FreeCString(context, url);
        return JS_ThrowRangeError(context, "worker source is too large");
    }
    size_t length = sizeof(prefix) - 1u + source_length
        + sizeof(suffix) - 1u;
    char *program = budget_malloc(bridge->host->budget, length + 1u);
    if (program == NULL) {
        JS_FreeCString(context, source);
        JS_FreeCString(context, url);
        return JS_ThrowOutOfMemory(context);
    }
    size_t used = 0;
    memcpy(program + used, prefix, sizeof(prefix) - 1u);
    used += sizeof(prefix) - 1u;
    memcpy(program + used, source, source_length); used += source_length;
    memcpy(program + used, suffix, sizeof(suffix));
    JS_FreeCString(context, source);
    JS_FreeCString(context, url);
    JSValue function = JS_Eval(
        context, program, length, "<worker>", JS_EVAL_TYPE_GLOBAL);
    budget_free(bridge->host->budget, program);
    if (JS_IsException(function)) return function;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 1, argv);
    JS_FreeValue(context, function);
    return result;
}

static ScriptRuntime *script_runtime_create_internal(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ScriptRuntimeOptions *options,
    ScriptResult *result)
{
    if (options == NULL
        || (options->document_scope != SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
            && options->document_scope
                   != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME)) {
        return NULL;
    }
    const ViewportContext *viewport = &options->viewport;
    BrowserSession *session = options->session;
    bool run_document_scripts = !options->defer_document_scripts;
    int css_viewport_width = viewport->css_width;
    int css_viewport_height = viewport->css_height;
    int device_width = viewport->device_width;
    int device_height = viewport->device_height;
    if (document == NULL || budget == NULL || result == NULL
        || css_viewport_width <= 0 || css_viewport_height <= 0
        || device_width <= 0 || device_height <= 0) return NULL;
    memset(result, 0, sizeof(*result));
#if !defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    if (options->dynamic_code_disabled) {
        snprintf(result->error, sizeof(result->error), "%s",
                 "QuickJS build cannot enforce CSP dynamic-code policy");
        return NULL;
    }
#endif
    const bool trace_startup =
        getenv("TILEFINCH_TRACE_JS_STARTUP") != NULL;
    const uint64_t startup_started_ns = js_rt_monotonic_time_ns();
    uint64_t runtime_ready_ns;
    uint64_t context_ready_ns;
    uint64_t native_ready_ns;
    uint64_t bootstrap_total_us = 0;
    uint64_t bootstrap_compile_us = 0;

    /* Freeze and validate every input to the replay derivation before any
       realm allocation or entropy state is created.  In particular, do not
       let the bounded host buffer silently turn two distinct overlong URLs
       into the same replay identity. */
    const bool deterministic = deterministic_replay.enabled;
    const uint64_t replay_seed = deterministic_replay.seed;
    uint64_t replay_clock_origin_ms = 0;
    if (deterministic) {
        size_t document_url_length = 0;
        if (!replay_document_url_exact(document_url, &document_url_length)
            || document_url_length
                   >= sizeof(((ScriptRuntime *) 0)->document_url)) {
            snprintf(result->error, sizeof(result->error), "%s",
                     "deterministic replay requires an exact canonical "
                     "HTTP(S) document URL");
            return NULL;
        }
        const uint64_t maximum_date_ms = UINT64_C(8640000000000000);
        if (fetch_trace_clock_origin_ms(&replay_clock_origin_ms)) {
            if (replay_clock_origin_ms > maximum_date_ms) {
                snprintf(result->error, sizeof(result->error), "%s",
                         "deterministic replay clock origin exceeds the "
                         "ECMAScript Date range");
                return NULL;
            }
        } else {
            /* Keep a useful contemporary epoch while making every seed safe
               without unsigned addition wrap.  The bounded seed component is
               part of the fallback policy, not an entropy-stream input. */
            replay_clock_origin_ms = UINT64_C(1700000000000)
                + replay_seed % UINT64_C(31557600000);
        }
    }
    ScriptRuntime *host = budget_calloc(budget, 1, sizeof(*host));
    if (host == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not allocate script runtime host");
        return NULL;
    }
    host->budget = budget;
    host->page_scroll_apply = JS_UNDEFINED;
    host->page_scroll_flush = JS_UNDEFINED;
    host->dispatch_at = JS_UNDEFINED;
    host->dispatch_handle = JS_UNDEFINED;
    host->dispatch_activation_handle = JS_UNDEFINED;
    host->dispatch_input_handle = JS_UNDEFINED;
    host->dispatch_submit_handle = JS_UNDEFINED;
    host->dom_content_loaded_dispatch = JS_UNDEFINED;
    host->media_state_for = JS_UNDEFINED;
    host->media_update = JS_UNDEFINED;
    host->function_to_string = JS_UNDEFINED;
    host->host_global = JS_UNDEFINED;
    for (size_t callback = 0; callback < SCRIPT_HOST_CALLBACK_COUNT;
         callback++) {
        host->host_callbacks[callback] = JS_UNDEFINED;
    }
    host->bridge.current_script = JS_NULL;
    host->bridge.trusted_node_wrap = JS_UNDEFINED;
    host->bridge.trusted_stable_script_wrap = JS_UNDEFINED;
    host->bridge.trusted_retire_native_node_state = JS_UNDEFINED;
    host->document = document;
    host->document_scope = options->document_scope;
    host->timeout_ms = timeout_ms;
    host->session = session;
    host->watchdog.result = &host->result;
    host->bridge.execution_policy = options->execution_policy;
    host->bridge.dynamic_code_allowed = !options->dynamic_code_disabled;
    host->bridge.allow_test_network_primitive_overrides =
        options->allow_test_network_primitive_overrides;
    host->bridge.stylesheet = options->stylesheet;
    host->bridge.maximum_scripts = options->maximum_scripts == 0
        ? SCRIPT_DYNAMIC_TASK_LIMIT : options->maximum_scripts;
    if (host->bridge.maximum_scripts > SCRIPT_REALM_MAXIMUM_SCRIPTS) {
        host->bridge.maximum_scripts = SCRIPT_REALM_MAXIMUM_SCRIPTS;
    }
    host->bridge.maximum_script_bytes =
        options->maximum_script_bytes == 0
            ? SCRIPT_DYNAMIC_DEFAULT_TOTAL_BYTES
            : options->maximum_script_bytes;
    if (host->bridge.maximum_script_bytes
        > SCRIPT_REALM_MAXIMUM_TOTAL_BYTES) {
        host->bridge.maximum_script_bytes =
            SCRIPT_REALM_MAXIMUM_TOTAL_BYTES;
    }
    host->bridge.maximum_script_file_bytes =
        options->maximum_script_file_bytes == 0
            ? SCRIPT_DYNAMIC_DEFAULT_FILE_BYTES
            : options->maximum_script_file_bytes;
    if (host->bridge.maximum_script_file_bytes
        > SCRIPT_REALM_MAXIMUM_FILE_BYTES) {
        host->bridge.maximum_script_file_bytes =
            SCRIPT_REALM_MAXIMUM_FILE_BYTES;
    }
    if (host->bridge.maximum_script_file_bytes
        > host->bridge.maximum_script_bytes) {
        host->bridge.maximum_script_file_bytes =
            host->bridge.maximum_script_bytes;
    }
    if (host->bridge.execution_policy.slow_compile_threshold_us == 0) {
        host->bridge.execution_policy.slow_compile_threshold_us =
            SCRIPT_DEFAULT_SLOW_DEPENDENCY_US;
    }
    if (host->bridge.execution_policy.slow_callback_threshold_us == 0) {
        host->bridge.execution_policy.slow_callback_threshold_us =
            SCRIPT_DEFAULT_SLOW_DEPENDENCY_US;
    }
    host->result.host_compile_source_limit_bytes =
        host->bridge.execution_policy.maximum_host_compile_source_bytes;
    snprintf(host->document_url, sizeof(host->document_url), "%s",
             document_url == NULL ? "" : document_url);
    const char *top_level_url = options->top_level_url == NULL
        ? document_url : options->top_level_url;
    if (top_level_url == NULL
        || strlen(top_level_url) >= sizeof(host->top_level_url)) {
        snprintf(result->error, sizeof(result->error),
                 "invalid top-level document URL");
        budget_free(budget, host);
        return NULL;
    }
    snprintf(host->top_level_url, sizeof(host->top_level_url), "%s",
             top_level_url);
    host->quickjs_pool = budget_quickjs_pool_create(budget);

    if (host->quickjs_pool == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not create QuickJS allocator pool");
        budget_free(budget, host);
        return NULL;
    }
    host->runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(),
                                  host->quickjs_pool);
    if (host->runtime == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not create QuickJS runtime");
        (void) budget_quickjs_pool_destroy(host->quickjs_pool);
        budget_free(budget, host);
        return NULL;
    }
    runtime_ready_ns = js_rt_monotonic_time_ns();
#if defined(CONFIG_PROPERTY_FAULT_TRACE)
    {
        const char *trace_value = getenv("TILEFINCH_TRACE_JS_PROPERTY_FAULTS");
        if (trace_value != NULL && trace_value[0] != '\0'
            && strcmp(trace_value, "0") != 0) {
            char *end = NULL;
            unsigned long requested = strtoul(trace_value, &end, 10);
            uint32_t limit = 32u;
            if (end != trace_value && *end == '\0') {
                if (requested < 1u)
                    requested = 1u;
                if (requested > 128u)
                    requested = 128u;
                limit = (uint32_t) requested;
            }
            JS_SetPropertyFaultTraceLimit(host->runtime, limit);
        }
    }
#endif
    {
        const char *window_env = getenv("TILEFINCH_JS_BOOT_WINDOW_KB");
        long window_kb = window_env == NULL ? 0 : atol(window_env);
        host->base_memory_limit = js_memory_limit;
        if (window_kb > 0
            && js_memory_limit <= SIZE_MAX - (size_t) window_kb * 1024u) {
            host->boot_window_bytes = (size_t) window_kb * 1024u;
            host->boot_window_active = true;
            js_memory_limit += host->boot_window_bytes;
        }
    }
    JS_SetMemoryLimit(host->runtime, js_memory_limit);
    /* Modern transformed bundles can have finite, standards-shaped call paths
       above 2 MiB in an unoptimized host build.  Use one host bound in Debug
       and Release so replay behaviour does not depend on compiler frame
       packing.  This QuickJS limit is a C-stack guard, not an up-front heap
       reservation; TILEFINCH_JS_STACK_KB remains an escape hatch for constrained
       or diagnostic builds. */
    size_t stack_limit = 4u * 1024u * 1024u;
#if defined(__PSP__)
    /* The PSP has no guard pages: overrunning the physical thread stack
       silently tramples whatever the heap placed below it (observed as a
       corrupt JSString after OOM-exception recursion).  The script EBOOT
       provides a 2560KB stack; a 1536KB guard leaves a full megabyte for
       the engine frames between the guard check and the physical edge. */
    stack_limit = 1536u * 1024u;
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    stack_limit = 4u * 1024u * 1024u;
#endif
#elif defined(__SANITIZE_ADDRESS__)
    stack_limit = 4u * 1024u * 1024u;
#endif
    const char *stack_kib_value = getenv("TILEFINCH_JS_STACK_KB");
    if (stack_kib_value != NULL && stack_kib_value[0] != '\0') {
        char *end = NULL;
        unsigned long stack_kib = strtoul(stack_kib_value, &end, 10);
        if (end != stack_kib_value && *end == '\0'
            && stack_kib >= 64 && stack_kib <= 8192) {
            stack_limit = (size_t) stack_kib * 1024u;
        }
    }
    JS_SetMaxStackSize(host->runtime, stack_limit);
    js_rt_runtime_arm_watchdog(host);
    JS_SetInterruptHandler(host->runtime, interrupt_handler, &host->watchdog);
    host->promise_rejection_state.result = &host->result;
    JS_SetHostPromiseRejectionTracker(host->runtime,
                                      promise_rejection_tracker,
                                      &host->promise_rejection_state);

    host->context = JS_NewContext(host->runtime);
    if (host->context != NULL
        && getenv("TILEFINCH_JS_TRAP_ALLOC_SIZE") != NULL) {
        budget_quickjs_pool_set_stack_dump_hook(
            js_rt_dump_interpreter_stack, host);
    }
    if (host->context == NULL) {
        snprintf(result->error, sizeof(result->error), "could not create QuickJS context");
        JS_FreeRuntime(host->runtime);
        (void) budget_quickjs_pool_destroy(host->quickjs_pool);
        budget_free(budget, host);
        return NULL;
    }
    context_ready_ns = js_rt_monotonic_time_ns();

    host->bridge.document = document;
    host->bridge.budget = budget;
    host->bridge.result = &host->result;
    host->bridge.relayout_dirty = &host->relayout_dirty;
    host->bridge.session = session;
    host->bridge.document_url = host->document_url;
    host->bridge.top_level_url = host->top_level_url;
    host->bridge.document_base_dirty = true;
    ScriptRemoteDocumentBinding remote_binding = {
        .element_lookup = options->remote_element_lookup,
        .element_opaque = options->remote_element_opaque,
        .selector_lookup = options->remote_selector_lookup,
        .selector_opaque = options->remote_selector_opaque,
        .selector_collect = options->remote_selector_collect,
        .selector_collect_opaque = options->remote_selector_collect_opaque,
        .descendant_collect = options->remote_descendant_collect,
        .descendant_collect_opaque =
            options->remote_descendant_collect_opaque,
        .node_read = options->remote_node_read,
        .node_read_opaque = options->remote_node_read_opaque,
        .node_write = options->remote_node_write,
        .node_write_opaque = options->remote_node_write_opaque,
        .node_visibility = options->node_visibility,
        .node_visibility_opaque = options->node_visibility_opaque,
        .section_identity = options->section_identity
    };
    if (!script_runtime_rebind_remote_document(
            host, &remote_binding)
        && options->document_scope
               != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME) {
        JS_FreeContext(host->context);
        JS_FreeRuntime(host->runtime);
        (void) budget_quickjs_pool_destroy(host->quickjs_pool);
        budget_free(budget, host);
        return NULL;
    }
    /* Acquire a borrowed page domain only after the last manual early-failure
       cleanup path. From here onward script_runtime_destroy() owns it.
       Standalone runtimes defer their private domain until network is used. */
    if (options->fetch_scheduler_domain != NULL) {
        host->bridge.fetch_scheduler_domain =
            fetch_scheduler_domain_retain(
                options->fetch_scheduler_domain);
    }
    if (options->fetch_scheduler_domain != NULL
        && host->bridge.fetch_scheduler_domain == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not acquire script fetch scheduler domain");
        script_runtime_destroy(host);
        return NULL;
    }
    snprintf(host->bridge.referrer_policy,
             sizeof(host->bridge.referrer_policy), "%s",
             options->referrer_policy == NULL
               ? "" : options->referrer_policy);
    host->bridge.fetch_timeout_ms = timeout_ms;
    host->bridge.viewport_width = css_viewport_width;
    host->bridge.viewport_height = css_viewport_height;
    host->bridge.host = host;
    host->bridge.performance_origin_ns = js_rt_monotonic_time_ns();
    host->bridge.deterministic_entropy = deterministic;
    host->bridge.deterministic_clock = deterministic;
    host->bridge.replay_seed = replay_seed;
    host->bridge.clock_origin_ms = replay_clock_origin_ms;
    host->bridge.entropy_state = host->bridge.deterministic_entropy
        ? script_runtime_deterministic_entropy_state_v1(
              replay_seed, host->document_url,
              options->document_scope)
        : 0;
    JS_SetContextOpaque(host->context, &host->bridge);

    JSValue global = JS_GetGlobalObject(host->context);
    JSValue js_document = JS_NewObject(host->context);
    bool setup_ok = !JS_IsException(global) && !JS_IsException(js_document);
    if (setup_ok) {
        JSValue ensure_canvas = JS_NewCFunction(
            host->context, js_runtime_ensure_canvas_bootstrap,
            "__tilefinchEnsureCanvasBootstrap", 0);
        if (JS_IsException(ensure_canvas)
            || JS_DefinePropertyValueStr(
                   host->context, global,
                   "__tilefinchEnsureCanvasBootstrap", ensure_canvas,
                   0) < 0) {
            if (JS_IsException(ensure_canvas)) {
                JS_FreeValue(host->context, ensure_canvas);
            }
            setup_ok = false;
        }
    }
    if (setup_ok) {
        JSValue ensure_motion = JS_NewCFunction(
            host->context, js_runtime_ensure_motion_bootstrap,
            "__tilefinchEnsureMotionBootstrap", 0);
        if (JS_IsException(ensure_motion)
            || JS_DefinePropertyValueStr(
                   host->context, global,
                   "__tilefinchEnsureMotionBootstrap", ensure_motion,
                   0) < 0) {
            if (JS_IsException(ensure_motion)) {
                JS_FreeValue(host->context, ensure_motion);
            }
            setup_ok = false;
        }
    }
    if (setup_ok) {
        JSValue run_worker = JS_NewCFunction(
            host->context, js_runtime_run_worker,
            "__tilefinchRunWorker", 3);
        if (JS_IsException(run_worker)
            || JS_DefinePropertyValueStr(
                   host->context, global, "__tilefinchRunWorker",
                   run_worker, JS_PROP_CONFIGURABLE) < 0) {
            if (JS_IsException(run_worker))
                JS_FreeValue(host->context, run_worker);
            setup_ok = false;
        }
    }
    if (setup_ok) {
        setup_ok = JS_DefinePropertyValueStr(
            host->context, global,
            "__tilefinchCspAllowsInlineEventHandlers",
            JS_NewBool(
                host->context,
                tilefinch_csp_allows_script_attribute(
                    &document->content_security_policy)),
            0) >= 0;
    }
    if (setup_ok && host->bridge.deterministic_entropy) {
        JSValue math = JS_GetPropertyStr(host->context, global, "Math");
        JSValue random = JS_NewCFunction(host->context,
                                         js_replay_math_random,
                                         "random", 0);
        if (JS_IsException(math) || JS_IsException(random)) {
            setup_ok = false;
            if (!JS_IsException(random))
                JS_FreeValue(host->context, random);
        } else {
            /* JS_SetPropertyStr consumes random on success or failure. */
            setup_ok = JS_SetPropertyStr(host->context, math,
                                         "random", random) >= 0;
        }
        JS_FreeValue(host->context, math);
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchLocationHref",
            JS_NewString(host->context, host->document_url)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchBrowserUserAgent",
            JS_NewString(host->context, TILEFINCH_BROWSER_USER_AGENT)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchDiagnosticMobileSafari",
            JS_NewBool(host->context,
                getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") != NULL)) >= 0;
    }
    if (setup_ok) {
        setup_ok = js_lazy_webpack_install(host, global);
    }
    if (setup_ok) {
        JSAtom writer_atom = JS_NewAtom(
            host->context, "__tilefinchHasRemoteNodeWriter");
        JSValue writer_getter = JS_NewCFunction(
            host->context, js_remote_node_writer_active,
            "get __tilefinchHasRemoteNodeWriter", 0);
        if (writer_atom == JS_ATOM_NULL
            || JS_IsException(writer_getter)) {
            if (!JS_IsException(writer_getter)) {
                JS_FreeValue(host->context, writer_getter);
            }
            setup_ok = false;
        } else if (JS_DefinePropertyGetSet(
                       host->context, global, writer_atom,
                       writer_getter, JS_UNDEFINED,
                       0) < 0) {
            /* QuickJS consumes getter/setter values on this API path. */
            setup_ok = false;
        }
        JS_FreeAtom(host->context, writer_atom);
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchViewportWidth",
            JS_NewInt32(host->context, css_viewport_width)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchViewportHeight",
                JS_NewInt32(host->context, css_viewport_height)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchDeviceWidth",
                JS_NewInt32(host->context, device_width)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchDeviceHeight",
                JS_NewInt32(host->context, device_height)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchViewportScale",
                JS_NewFloat64(host->context,
                              (double) device_width
                              / (double) css_viewport_width)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchTraceBase64",
            JS_NewBool(host->context,
                getenv("TILEFINCH_TRACE_BASE64") != NULL)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchTraceTasksEnabled",
            JS_NewBool(host->context,
                getenv("TILEFINCH_TRACE_TASKS") != NULL)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global,
            "__tilefinchPointerMarkupInitiallyPresent",
            JS_NewBool(
                host->context,
                host->document != NULL
                    && host->document->pointer_event_attributes_present)) >= 0;
    }
    if (setup_ok) {
        setup_ok = js_rt_install_function(
                       host->context, global, "__tilefinchCssSupports",
                       js_css_supports, 2)
                   && js_rt_install_function(
                       host->context, global,
                       "__tilefinchCompileInlineEventHandler",
                       js_compile_inline_event_handler, 1)
                   && js_rt_install_function(
                       host->context, global,
                       "__tilefinchStylesheetHasMotionKeyframes",
                       js_stylesheet_has_motion_keyframes, 0)
                   && js_rt_install_function(host->context, global, "__tilefinchBody",
                                    js_dom_body, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchDocumentElement",
                                       js_dom_document_element, 0)
                   && js_rt_install_function(host->context, global, "__tilefinchQuery",
                                       js_dom_query, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchQueryAll",
                                       js_dom_query_all, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchMatches",
                                       js_dom_matches, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRelation",
                                       js_dom_relation, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchIsConnected",
                                       js_dom_is_connected, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchChildren",
                                       js_dom_children, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchChildNodes",
                                       js_dom_child_nodes, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchDocumentChildNodes",
                                       js_dom_document_child_nodes, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchTagName",
                                       js_dom_tag_name, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchNamespaceURI",
                                       js_dom_namespace_uri, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchParseColor",
                                       js_dom_parse_color, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchNodeType",
                                       js_dom_node_type, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchDescendants",
                                       js_dom_descendants, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchNamedElementIds",
                                       js_dom_named_element_ids, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchContent",
                                       js_dom_content, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchClone",
                                       js_dom_clone, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchAttributes",
                                       js_dom_attributes, 1)
                   && js_rt_install_function(host->context, global, "__tilefinchCreate",
                                       js_dom_create, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCreateFragment",
                                       js_dom_create_fragment, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCreateText",
                                       js_dom_create_text, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCreateComment",
                                       js_dom_create_comment, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetCustomState",
                                       js_dom_set_custom_state, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchGetCustomState",
                                       js_dom_get_custom_state, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRetainNodeWrapper",
                                       js_dom_retain_node_wrapper, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchReleaseNodeWrapper",
                                       js_dom_release_node_wrapper, 2)
                   && js_rt_install_function(host->context, global, "__tilefinchGetText",
                                       js_dom_get_text, 1)
                   && js_rt_install_function(host->context, global, "__tilefinchSetText",
                                       js_dom_set_text, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchGetInnerHTML",
                                       js_dom_get_inner_html, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetInnerHTML",
                                       js_dom_set_inner_html, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchGetAttribute",
                                       js_dom_get_attribute, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchImageProperty",
                                       js_dom_image_property, 2)
                   && js_remote_bindings_install(host->context, global)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetAttribute",
                                       js_dom_set_attribute, 3)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetControlValue",
                                       js_dom_set_control_value, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchParserFormOwner",
                                       js_dom_parser_form_owner, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchHasParserFormOwners",
                                       js_dom_has_parser_form_owners, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchHeapRemaining",
                                       js_runtime_heap_remaining, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRemoveAttribute",
                                       js_dom_remove_attribute, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStyleGet",
                                       js_style_get, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchComputedStyleGet",
                                       js_computed_style_get, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStyleSet",
                                       js_style_set, 3)
                   && js_rt_install_function(host->context, global, "__tilefinchAppend",
                                       js_dom_append, 2)
                   && js_rt_install_function(
                                       host->context, global,
                                       "__tilefinchPrepareDynamicSubtree",
                                       js_dom_prepare_dynamic_subtree, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchAppendMany",
                                       js_dom_append_many, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchInsertBefore",
                                       js_dom_insert_before, 3)
                   && js_rt_install_function(host->context, global, "__tilefinchRemove",
                                       js_dom_remove, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRecordEvent",
                                       js_dom_record_event, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageGet",
                                       js_storage_get, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageAvailable",
                                       js_storage_available, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageSet",
                                       js_storage_set, 3)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageLength",
                                       js_storage_length, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageKey",
                                       js_storage_key, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageRemove",
                                       js_storage_remove, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStorageClear",
                                       js_storage_clear, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCookieGet",
                                       js_cookie_get, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCookieSet",
                                       js_cookie_set, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchConsoleLog",
                                       js_console_log, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchResolveURL",
                                       js_resolve_url, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchDocumentBaseURI",
                                       js_document_base_uri, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetDocumentURL",
                                       js_set_document_url, 1)
                   && js_fetch_cors_install(host->context, global)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchScriptForceAsync",
                                       js_script_force_async, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchScriptAsyncAssigned",
                                       js_script_async_assigned, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCryptoRandomFill",
                                       js_crypto_random_fill, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCryptoDigestSHA256",
                                       js_crypto_digest_sha256, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchBase64EncodeBytes",
                                       js_base64_encode_bytes, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchBase64DecodeString",
                                       js_base64_decode_string, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchPerformanceNow",
                                       js_performance_now, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchPerformanceSample",
                                       js_performance_sample, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCallbackCheckpoint",
                                       js_callback_checkpoint, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchDateNow",
                                       js_date_now, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchPostMessage",
                                       js_post_message, 3)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRequestNavigation",
                                       js_request_navigation, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRequestMedia",
                                       js_request_media, 4)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchRequestScroll",
                                       js_request_scroll, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSuppressRemoteLookup",
                                       js_remote_lookup_suppress, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchStableNodeKey",
                                       js_stable_node_key, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSectionIdentity",
                                       js_section_identity, 0)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchFindStableNode",
                                       js_find_stable_node, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchGeometry",
                                       js_element_geometry, 1)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchSetElementScroll",
                                       js_set_element_scroll, 3)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchQueryCount",
                                       js_dom_query_count, 2)
                   && js_rt_install_function(host->context, global,
                                       "__tilefinchCreateFrameEval",
                                       js_create_frame_eval, 2)
                   && install_named_function(host->context, global,
                                       "__tilefinchDocumentQuerySelector",
                                       "querySelector",
                                       js_dom_query_selector_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchDocumentQuerySelectorAll",
                                       "querySelectorAll",
                                       js_dom_query_selector_all_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchDocumentGetElementById",
                                       "getElementById",
                                       js_dom_get_element_by_id_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchElementQuerySelector",
                                       "querySelector",
                                       js_dom_query_selector_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchElementQuerySelectorAll",
                                       "querySelectorAll",
                                       js_dom_query_selector_all_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchDocumentFragmentQuerySelector",
                                       "querySelector",
                                       js_dom_query_selector_method, 1)
                   && install_named_function(host->context, global,
                                       "__tilefinchDocumentFragmentQuerySelectorAll",
                                       "querySelectorAll",
                                       js_dom_query_selector_all_method, 1);
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(host->context, js_document, "title",
            JS_NewString(host->context, document->title == NULL ? "" : document->title)) >= 0;
        JSAtom body_text_atom = JS_NewAtom(host->context, "bodyText");
        JSValue body_text_getter = JS_NewCFunction(
            host->context, js_document_body_text, "get bodyText", 0);
        if (body_text_atom == JS_ATOM_NULL
            || JS_IsException(body_text_getter)) {
            if (!JS_IsException(body_text_getter)) {
                JS_FreeValue(host->context, body_text_getter);
            }
            setup_ok = false;
        } else if (JS_DefinePropertyGetSet(
                       host->context, js_document, body_text_atom,
                       body_text_getter, JS_UNDEFINED,
                       JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
            /* QuickJS consumes getter/setter values on this API path. */
            setup_ok = false;
        }
        JS_FreeAtom(host->context, body_text_atom);
        if (setup_ok) {
            JSAtom current_script_atom = JS_NewAtom(
                host->context, "currentScript");
            JSValue current_script_getter = JS_NewCFunction(
                host->context, js_document_current_script,
                "get currentScript", 0);
            if (current_script_atom == JS_ATOM_NULL
                || JS_IsException(current_script_getter)) {
                if (!JS_IsException(current_script_getter)) {
                    JS_FreeValue(host->context, current_script_getter);
                }
                setup_ok = false;
            } else if (JS_DefinePropertyGetSet(
                           host->context, js_document,
                           current_script_atom, current_script_getter,
                           JS_UNDEFINED, JS_PROP_ENUMERABLE) < 0) {
                /* No configurable bit and no setter: author code can
                   neither replace nor write the native execution scope. */
                setup_ok = false;
            }
            JS_FreeAtom(host->context, current_script_atom);
        }
        setup_ok = setup_ok && JS_SetPropertyStr(host->context, js_document, "nodeCount",
            JS_NewInt64(host->context, (int64_t) document->node_count)) >= 0;
        if (setup_ok) {
            int property_result = JS_SetPropertyStr(host->context, global, "document",
                                                    js_document);
            js_document = JS_UNDEFINED;
            setup_ok = property_result >= 0;
        }
    }

    native_ready_ns = js_rt_monotonic_time_ns();
    if (setup_ok) setup_ok = js_rt_runtime_script_checkpoint(host, 1);
    bool evaluated = false;
    if (!setup_ok) {
        js_rt_record_exception(host->context, &host->result);
    } else {
        evaluated = runtime_evaluate_bootstrap_profiled(
            host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                      browser_bootstrap_cssom,
                      browser_bootstrap_cssom_bytecode),
            "<browser-cssom>",
            &browser_bootstrap_cssom_bytecode, false, trace_startup,
            &bootstrap_total_us, &bootstrap_compile_us);
        if (evaluated && host->bridge.deterministic_clock) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_deterministic_clock_bootstrap,
                          browser_deterministic_clock_bootstrap_bytecode),
                "<browser-deterministic-clock>",
                &browser_deterministic_clock_bootstrap_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) evaluated = runtime_evaluate_bootstrap_profiled(
            host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                      browser_bootstrap, browser_bootstrap_bytecode),
            "<browser-bootstrap>", &browser_bootstrap_bytecode,
            true, trace_startup,
            &bootstrap_total_us, &bootstrap_compile_us);
        if (evaluated) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_bootstrap_platform,
                          browser_bootstrap_platform_bytecode),
                "<browser-platform>", &browser_bootstrap_platform_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) {
            evaluated = runtime_install_lazy_bootstrap_feature(
                host, LAZY_BOOTSTRAP_CANVAS);
        }
        if (evaluated) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_bootstrap_custom_elements,
                          browser_bootstrap_custom_elements_bytecode),
                "<browser-custom-elements>",
                &browser_bootstrap_custom_elements_bytecode, false,
                trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) {
            evaluated = runtime_install_lazy_bootstrap_feature(
                host, LAZY_BOOTSTRAP_STREAMS);
        }
        if (evaluated) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_bootstrap_compat,
                          browser_bootstrap_compat_bytecode),
                "<browser-compat>", &browser_bootstrap_compat_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
        if (evaluated && getenv("TILEFINCH_TRACE_DPU") != NULL) {
            evaluated = runtime_evaluate_bootstrap_profiled(host,
                browser_dpu_diagnostic,
                browser_dpu_diagnostic_length,
                "<dpu-diagnostic>", NULL, false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
#endif
        if (evaluated) {
            evaluated = runtime_evaluate_bootstrap_profiled(host,
                TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                    browser_bootstrap_scheduler,
                    browser_bootstrap_scheduler_bytecode),
                "<browser-scheduler>", &browser_bootstrap_scheduler_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated && host->bridge.deterministic_clock) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_deterministic_scheduler_clock_bootstrap,
                          browser_deterministic_scheduler_clock_bootstrap_bytecode),
                "<browser-deterministic-scheduler-clock>",
                &browser_deterministic_scheduler_clock_bootstrap_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) {
            evaluated = runtime_install_lazy_bootstrap_feature(
                host, LAZY_BOOTSTRAP_MOTION);
        }
        if (evaluated) {
            evaluated = runtime_install_lazy_bootstrap_feature(
                host, LAZY_BOOTSTRAP_INDEXED_DB);
        }
        if (evaluated && host->bridge.deterministic_clock) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_deterministic_replay_surface_bootstrap,
                          browser_deterministic_replay_surface_bootstrap_bytecode),
                "<browser-deterministic-replay-surface>",
                &browser_deterministic_replay_surface_bootstrap_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                host, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                          browser_bootstrap_hardening,
                          browser_bootstrap_hardening_bytecode),
                "<browser-hardening>",
                &browser_bootstrap_hardening_bytecode,
                false, trace_startup,
                &bootstrap_total_us, &bootstrap_compile_us);
        }
        if (evaluated) {
            host->page_scroll_apply = JS_GetPropertyStr(
                host->context, global, "__tilefinchApplyPageScroll");
            host->page_scroll_flush = JS_GetPropertyStr(
                host->context, global, "__tilefinchFlushPageScroll");
            host->dispatch_at = JS_GetPropertyStr(
                host->context, global, "__tilefinchDispatchAt");
            host->dispatch_handle = JS_GetPropertyStr(
                host->context, global, "__tilefinchDispatchHandle");
            host->dispatch_activation_handle = JS_GetPropertyStr(
                host->context, global,
                "__tilefinchDispatchActivationHandle");
            host->dispatch_input_handle = JS_GetPropertyStr(
                host->context, global, "__tilefinchDispatchInputHandle");
            host->dispatch_submit_handle = JS_GetPropertyStr(
                host->context, global, "__tilefinchDispatchSubmitHandle");
            host->dom_content_loaded_dispatch = JS_GetPropertyStr(
                host->context, global,
                "__tilefinchDispatchDOMContentLoaded");
            host->media_state_for = JS_GetPropertyStr(
                host->context, global, "__tilefinchMediaStateFor");
            host->bridge.trusted_node_wrap = JS_GetPropertyStr(
                host->context, global, "__tilefinchWrap");
            host->bridge.trusted_stable_script_wrap = JS_GetPropertyStr(
                host->context, global,
                "__tilefinchCurrentScriptForStable");
            host->bridge.trusted_retire_native_node_state =
                JS_GetPropertyStr(
                    host->context, global,
                    "__tilefinchRetireNativeNodeState");
            evaluated = runtime_capture_host_callbacks(host, global)
                     && JS_IsFunction(host->context,
                                      host->page_scroll_apply)
                     && JS_IsFunction(host->context,
                                      host->page_scroll_flush)
                     && JS_IsFunction(host->context, host->dispatch_at)
                     && JS_IsFunction(host->context, host->dispatch_handle)
                     && JS_IsFunction(
                            host->context,
                            host->dispatch_activation_handle)
                     && JS_IsFunction(
                            host->context, host->dispatch_input_handle)
                     && JS_IsFunction(
                            host->context, host->dispatch_submit_handle)
                     && JS_IsFunction(
                            host->context,
                            host->dom_content_loaded_dispatch)
                     && JS_IsFunction(host->context,
                                      host->media_state_for)
                     && JS_IsFunction(host->context,
                                      host->bridge.trusted_node_wrap)
                     && JS_IsFunction(
                            host->context,
                            host->bridge.trusted_stable_script_wrap)
                     && JS_IsFunction(
                            host->context,
                            host->bridge.trusted_retire_native_node_state);
        }
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
        if (evaluated && getenv("TILEFINCH_TRACE_EVAL_SOURCE") != NULL) {
            evaluated = runtime_evaluate_checkpointed(host,
                browser_eval_source_diagnostic,
                browser_eval_source_diagnostic_length,
                "<eval-source-diagnostic>");
        }
#endif
        /* platform.js retained private worker and media-state helpers in
           bootstrap closures. Remove the temporary bridges before author
           code; native code has already captured their callable values. */
        if (evaluated) {
            JSAtom run_worker_atom = JS_NewAtom(
                host->context, "__tilefinchRunWorker");
            JSAtom media_state_atom = JS_NewAtom(
                host->context, "__tilefinchMediaStateFor");
            if (run_worker_atom == JS_ATOM_NULL
                || media_state_atom == JS_ATOM_NULL
                || JS_DeleteProperty(
                       host->context, global, run_worker_atom, 0) < 0
                || JS_DeleteProperty(
                       host->context, global, media_state_atom, 0) < 0) {
                evaluated = false;
            }
            JS_FreeAtom(host->context, run_worker_atom);
            JS_FreeAtom(host->context, media_state_atom);
        }
        /* Trusted bootstrap may use dynamic compilation to construct bounded
           browser shims. Author code begins only after the native QuickJS
           gate adopts the document's CSP decision. Wrapping eval in page JS
           would be bypassable through aliases and Function prototypes. */
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
        if (evaluated) {
            JS_SetDynamicCodeEnabled(
                host->context, options->dynamic_code_disabled ? 0 : 1);
        }
#endif
        /* Host bootstrap activity is not part of the page-visible contract.
           timeOrigin was captured from zero; author code and future event-loop
           ticks now start from the same independent wall/monotonic origin. */
        if (evaluated && host->bridge.deterministic_clock) {
            host->bridge.clock_host_elapsed_ms = 0;
            host->bridge.clock_wall_elapsed_ms = 0;
            host->bridge.clock_monotonic_elapsed_ms = 0;
            memset(host->bridge.clock_source_counts, 0,
                   sizeof(host->bridge.clock_source_counts));
        }
        if (evaluated && run_document_scripts) {
            evaluated = evaluate_scripts(host->context,
                lxb_dom_interface_node(document->html), &host->result);
            if (evaluated) evaluated = js_rt_runtime_script_checkpoint(host, 1);
        }
        if (evaluated && run_document_scripts) {
            /*
             * Bootstrap and parser-script garbage is dead before the loading
             * lifecycle begins.  On deliberately small realms, collect it
             * before allocating the DOMContentLoaded/load event graph rather
             * than turning recoverable pressure into a failed navigation.
             */
            if (script_runtime_heap_remaining(host) < 256u * 1024u)
                (void) script_runtime_collect_and_trim(host);
            static const char dispatch[] = "__tilefinchDispatchDOMContentLoaded()";
            evaluated = runtime_evaluate_checkpointed(
                host, dispatch, sizeof(dispatch) - 1,
                "<dom-content-loaded>");
            host->result.dom_content_loaded_dispatched = evaluated;
        }
        if (evaluated) {
            evaluated = js_rt_runtime_run_jobs(host)
                        && js_rt_runtime_script_checkpoint(host, 1);
        }
    }
    if (evaluated) evaluated = js_rt_runtime_refresh(host);

    if (evaluated) {
        JSValue summary = JS_GetPropertyStr(host->context, global, "pocSummary");
        if (!JS_IsUndefined(summary)) {
            const char *text = JS_ToCString(host->context, summary);
            if (text != NULL) {
                snprintf(host->result.summary, sizeof(host->result.summary),
                         "%s", text);
                JS_FreeCString(host->context, text);
            }
        }
        JS_FreeValue(host->context, summary);
        JSValue submitted = JS_GetPropertyStr(host->context, global,
                                              "__tilefinchSubmitted");
        host->result.form_submission_requested =
            JS_ToBool(host->context, submitted) > 0;
        JS_FreeValue(host->context, submitted);
        host->result.success = true;
    }

    JS_FreeValue(host->context, js_document);
    JS_FreeValue(host->context, global);
    js_rt_runtime_update_result(host, result);
    if (trace_startup) {
        uint64_t finished_ns = js_rt_monotonic_time_ns();
        uint64_t total_us = finished_ns >= startup_started_ns
            ? (finished_ns - startup_started_ns) / UINT64_C(1000) : 0;
        uint64_t runtime_us = runtime_ready_ns >= startup_started_ns
            ? (runtime_ready_ns - startup_started_ns) / UINT64_C(1000) : 0;
        uint64_t context_us = context_ready_ns >= runtime_ready_ns
            ? (context_ready_ns - runtime_ready_ns) / UINT64_C(1000) : 0;
        uint64_t native_us = native_ready_ns >= context_ready_ns
            ? (native_ready_ns - context_ready_ns) / UINT64_C(1000) : 0;
        uint64_t accounted_us = runtime_us + context_us + native_us;
        accounted_us = accounted_us > UINT64_MAX - bootstrap_total_us
            ? UINT64_MAX : accounted_us + bootstrap_total_us;
        fprintf(stderr,
                "javascript-startup-summary total-us=%llu runtime-us=%llu "
                "context-us=%llu native-install-us=%llu bootstraps-us=%llu "
                "bootstrap-compile-us=%llu bootstrap-execute-us=%llu "
                "post-us=%llu success=%s\n",
                (unsigned long long) total_us,
                (unsigned long long) runtime_us,
                (unsigned long long) context_us,
                (unsigned long long) native_us,
                (unsigned long long) bootstrap_total_us,
                (unsigned long long) bootstrap_compile_us,
                (unsigned long long) (bootstrap_total_us
                    >= bootstrap_compile_us
                    ? bootstrap_total_us - bootstrap_compile_us : 0),
                (unsigned long long) (total_us >= accounted_us
                    ? total_us - accounted_us : 0),
                host->result.success ? "yes" : "no");
    }
    if (!host->result.success) {
        script_runtime_destroy(host);
        return NULL;
    }
    return host;
}

ScriptRuntime *script_runtime_create_with_session(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, ScriptResult *result)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)) return NULL;
    ScriptRuntimeOptions options = {
        .session = session, .viewport = viewport
    };
    return script_runtime_create_configured(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &options, result);
}

ScriptRuntime *script_runtime_create_configured(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ScriptRuntimeOptions *options, ScriptResult *result)
{
    return script_runtime_create_internal(
        document, budget, js_memory_limit, timeout_ms, document_url,
        options, result);
}

ScriptRuntime *script_runtime_create_with_session_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, int css_viewport_width,
    int css_viewport_height, int device_width, int device_height,
    ScriptResult *result)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, css_viewport_width,
                               css_viewport_height, device_width,
                               device_height)) return NULL;
    ScriptRuntimeOptions options = {
        .session = session, .viewport = viewport
    };
    return script_runtime_create_configured(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &options, result);
}

ScriptRuntime *script_runtime_create_deferred(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, ScriptResult *result)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)) return NULL;
    ScriptRuntimeOptions options = {
        .session = session, .viewport = viewport,
        .defer_document_scripts = true
    };
    return script_runtime_create_configured(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &options, result);
}

ScriptRuntime *script_runtime_create_deferred_viewport(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    BrowserSession *session, int css_viewport_width,
    int css_viewport_height, int device_width, int device_height,
    ScriptResult *result)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, css_viewport_width,
                               css_viewport_height, device_width,
                               device_height)) return NULL;
    ScriptRuntimeOptions options = {
        .session = session, .viewport = viewport,
        .defer_document_scripts = true
    };
    return script_runtime_create_configured(
        document, budget, js_memory_limit, timeout_ms, document_url,
        &options, result);
}

ScriptRuntime *script_runtime_create(PocDocument *document, Budget *budget,
                                     size_t js_memory_limit,
                                     unsigned timeout_ms,
                                     const char *document_url,
                                     ScriptResult *result)
{
    return script_runtime_create_with_session(
        document, budget, js_memory_limit, timeout_ms, document_url, NULL,
        result);
}
