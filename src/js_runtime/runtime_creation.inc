#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
#include "../validation_phase_internal.h"

static JSValue js_validation_render_history(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (!bridge || argc != 1 || !JS_IsBool(argv[0])) return JS_FALSE;
    return JS_NewBool(context, script_runtime_webgl_validation_history(
        bridge->host, JS_ToBool(context, argv[0]) != 0));
}

static ValidationPhaseClock js_validation_phase_clock(void)
{
    ValidationPhaseClock clock = {0};
    SceKernelThreadRunStatus status = {.size = sizeof(status)};
    clock.wall_begin_us = (uint64_t) sceKernelGetSystemTimeWide();
    int result = sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status);
    clock.wall_end_us = (uint64_t) sceKernelGetSystemTimeWide();
    clock.valid = result >= 0;
    if (clock.valid)
        clock.cpu_us = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
    return clock;
}

void js_rt_validation_cpu_enter(ScriptRuntime *runtime, unsigned phase)
{
    if (runtime != NULL && runtime->validation_cpu.enabled)
        validation_cpu_enter(&runtime->validation_cpu, phase, js_validation_phase_clock());
}

void js_rt_validation_cpu_leave(ScriptRuntime *runtime, unsigned phase)
{
    if (runtime != NULL && runtime->validation_cpu.enabled)
        validation_cpu_leave(&runtime->validation_cpu, phase, js_validation_phase_clock());
}

/* Numeric validation protocol: configure, begin-frame, enter, leave,
   end-frame. It is deliberately not a shipping browser API. Only configure
   allocates; per-frame counts arrive in one fixed typed-array copy. */
static JSValue js_validation_phase(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    uint32_t op, id;
    if (runtime == NULL || argc < 2 || !JS_IsNumber(argv[0])
        || !JS_IsNumber(argv[1])) return JS_FALSE;
    if (JS_ToUint32(context, &op, argv[0]) < 0
        || JS_ToUint32(context, &id, argv[1]) < 0) return JS_EXCEPTION;
    if (op == 0u) {
        if (id < 1u || id > 3u
            || (runtime->validation_phases && runtime->validation_phases->active))
            return JS_FALSE;
        if (!runtime->validation_phases)
            runtime->validation_phases = budget_calloc_category(runtime->budget,
                BUDGET_CATEGORY_JAVASCRIPT, 1u, sizeof(ValidationPhaseCapture));
        if (!runtime->validation_phases) return JS_FALSE;
        memset(runtime->validation_phases, 0, sizeof(*runtime->validation_phases));
        runtime->validation_phases->mode = id;
        return JS_TRUE;
    }
    ValidationPhaseCapture *capture = runtime->validation_phases;
    if (!capture) return JS_FALSE;
    ValidationPhaseClock clock = {0};
    if (capture->mode != 2u && (op == 1u || op == 4u
            || (capture->mode == 3u && (op == 2u || op == 3u))))
        clock = js_validation_phase_clock();
    bool okay = false;
    if (op == 1u) okay = validation_phase_begin(capture, id, clock);
    else if (op == 2u) okay = validation_phase_enter(capture, id, clock);
    else if (op == 3u) okay = validation_phase_leave(capture, id, clock);
    else if (op == 4u && argc == 4 && JS_IsNumber(argv[2])) {
        uint32_t checksum_b;
        if (JS_ToUint32(context, &checksum_b, argv[2]) < 0) return JS_EXCEPTION;
        size_t offset = 0, length = 0, element = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(context, argv[3], &offset, &length, &element);
        if (JS_IsException(buffer)) return buffer;
        size_t capacity = 0;
        uint8_t *bytes = JS_GetArrayBuffer(context, &capacity, buffer);
        if (bytes && element == sizeof(uint32_t) && offset % sizeof(uint32_t) == 0u
            && offset <= capacity && length <= capacity - offset
            && length % sizeof(uint32_t) == 0u
            && length / sizeof(uint32_t) <= VALIDATION_PHASE_COUNTER_LIMIT)
            okay = validation_phase_end(capture, clock, id, checksum_b,
                (const uint32_t *) (bytes + offset), length / sizeof(uint32_t));
        else validation_phase_fail(capture);
        JS_FreeValue(context, buffer);
    } else validation_phase_fail(capture);
    return JS_NewBool(context, okay);
}

#endif

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
/* Whole-buffer validation hashes avoid thousands of JS/native Math.imul
   crossings in the intrusive parity proof. Views and writes remain bounded;
   no strings, temporary objects, source coercion or allocation occur here. */
static JSValue js_validation_hash_bytes(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value;
    uint32_t count;
    if (argc != 3 || !JS_IsNumber(argv[1])) return JS_FALSE;
    if (JS_ToUint32(context, &count, argv[1]) < 0) return JS_EXCEPTION;
    if (count > 512u * 1024u) return JS_FALSE;
    size_t in_offset = 0, in_length = 0, in_element = 0;
    size_t out_offset = 0, out_length = 0, out_element = 0;
    JSValue input = JS_GetTypedArrayBuffer(context, argv[0],
        &in_offset, &in_length, &in_element);
    if (JS_IsException(input)) return input;
    JSValue output = JS_GetTypedArrayBuffer(context, argv[2],
        &out_offset, &out_length, &out_element);
    if (JS_IsException(output)) {JS_FreeValue(context, input); return output;}
    size_t in_capacity = 0, out_capacity = 0;
    uint8_t *in = JS_GetArrayBuffer(context, &in_capacity, input);
    uint8_t *out = JS_GetArrayBuffer(context, &out_capacity, output);
    bool okay = in && out && in_offset <= in_capacity
        && in_length <= in_capacity - in_offset && count <= in_length
        && out_offset <= out_capacity && out_length <= out_capacity - out_offset
        && out_element == sizeof(uint32_t) && out_length >= 2u * sizeof(uint32_t);
    if (okay) {
        uint32_t hashes[2] = {UINT32_C(2166136261), UINT32_C(0x9e3779b9)};
        for (uint32_t at = 0; at < count; at++) {
            hashes[0] = (hashes[0] ^ in[in_offset + at]) * UINT32_C(16777619);
            hashes[1] = (hashes[1] ^ in[in_offset + at]) * UINT32_C(2246822519);
        }
        memcpy(out + out_offset, hashes, sizeof(hashes));
    }
    JS_FreeValue(context, output); JS_FreeValue(context, input);
    return JS_NewBool(context, okay);
}
#endif

#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
/* Two samples around a bounded validation kernel separate CPU work from
   mixer/supervisor preemption. Never polled inside ordinary gameplay. */
static JSValue js_validation_thread_cpu_us(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    SceKernelThreadRunStatus status = {.size = sizeof(status)};
    if (sceKernelReferThreadRunStatus(sceKernelGetThreadId(), &status) < 0)
        return JS_ThrowInternalError(context, "CPU clock unavailable");
    uint64_t us = ((uint64_t) status.runClocks.hi << 32) | status.runClocks.low;
    return JS_NewFloat64(context, (double) us);
}
#endif

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
    size_t policy_maximum = policy->maximum_host_compile_source_bytes;
    /* Trusted bootstrap sources are pinned at build time to the bootstrap
       ceiling by _Static_assert; the smaller authored-script strict unit
       must not reject them on the source-fallback path. The realistic
       ceiling and the unbounded lab profile are unchanged, and the policy
       value stays the reported limit. */
    size_t maximum = policy_maximum;
    if (maximum == SCRIPT_PSP_STRICT_MAXIMUM_HOST_COMPILE_BYTES)
        maximum = SCRIPT_BOOTSTRAP_STRICT_MAXIMUM_HOST_COMPILE_BYTES;
    js_rt_saturating_add_size(&result->host_compile_attempts, 1);
    result->host_compile_source_limit_bytes = policy_maximum;
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
    /* Heap census per module: which QuickJS categories the module's
       evaluation retained. Walks the whole heap, so trace mode only. The
       final "read" column is the serialized bytecode consumed from ROM by
       JS_ReadObject (QuickJS reports it as binary_object_size); it is not
       heap, and with ROM data the code itself is not either. */
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    JSMemoryUsage census_before;
    if (trace) js_rt_heap_census(runtime, &census_before);
#else
    (void) trace;
#endif
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
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
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
        JSMemoryUsage census_after;
        js_rt_heap_census(runtime, &census_after);
#define TILEFINCH_CENSUS_DELTA(field) \
        ((long long) (census_after.field - census_before.field))
        fprintf(stderr,
                "javascript-startup-heap-census name=\"%s\" malloc=%lld "
                "atoms=%lld/%lld strings=%lld/%lld objects=%lld/%lld "
                "props=%lld/%lld shapes=%lld/%lld funcs=%lld/%lld "
                "code=%lld pc2line=%lld/%lld cfuncs=%lld arrays=%lld "
                "fast-elements=%lld read=%lld/%lld\n",
                source_url,
                TILEFINCH_CENSUS_DELTA(malloc_size),
                TILEFINCH_CENSUS_DELTA(atom_count),
                TILEFINCH_CENSUS_DELTA(atom_size),
                TILEFINCH_CENSUS_DELTA(str_count),
                TILEFINCH_CENSUS_DELTA(str_size),
                TILEFINCH_CENSUS_DELTA(obj_count),
                TILEFINCH_CENSUS_DELTA(obj_size),
                TILEFINCH_CENSUS_DELTA(prop_count),
                TILEFINCH_CENSUS_DELTA(prop_size),
                TILEFINCH_CENSUS_DELTA(shape_count),
                TILEFINCH_CENSUS_DELTA(shape_size),
                TILEFINCH_CENSUS_DELTA(js_func_count),
                TILEFINCH_CENSUS_DELTA(js_func_size),
                TILEFINCH_CENSUS_DELTA(js_func_code_size),
                TILEFINCH_CENSUS_DELTA(js_func_pc2line_count),
                TILEFINCH_CENSUS_DELTA(js_func_pc2line_size),
                TILEFINCH_CENSUS_DELTA(c_func_count),
                TILEFINCH_CENSUS_DELTA(array_count),
                TILEFINCH_CENSUS_DELTA(fast_array_elements),
                TILEFINCH_CENSUS_DELTA(binary_object_count),
                TILEFINCH_CENSUS_DELTA(binary_object_size));
#undef TILEFINCH_CENSUS_DELTA
    }
#endif
    return evaluated;
}

typedef enum {
    LAZY_BOOTSTRAP_CANVAS = 0,
    LAZY_BOOTSTRAP_INDEXED_DB,
    LAZY_BOOTSTRAP_MOTION,
    LAZY_BOOTSTRAP_STREAMS,
    LAZY_BOOTSTRAP_GAME_AUDIO,
    LAZY_BOOTSTRAP_WEBGL,
    LAZY_BOOTSTRAP_WORKER,
    LAZY_BOOTSTRAP_WASM,
    LAZY_BOOTSTRAP_CAPABILITIES,
    LAZY_BOOTSTRAP_INTL,
    LAZY_BOOTSTRAP_OPFS,
    LAZY_BOOTSTRAP_TRAVERSAL,
    LAZY_BOOTSTRAP_SHADOW,
    LAZY_BOOTSTRAP_COUNT
} LazyBootstrapFeature;

_Static_assert(LAZY_BOOTSTRAP_COUNT == SCRIPT_LAZY_BOOTSTRAP_FEATURE_COUNT,
               "lazy bootstrap state must cover every feature");

typedef enum {
    LAZY_BOOTSTRAP_GLOBAL = 0,
    LAZY_BOOTSTRAP_CANVAS_PROTOTYPE,
    LAZY_BOOTSTRAP_STORAGE_MANAGER_PROTOTYPE,
    LAZY_BOOTSTRAP_DOCUMENT_PROTOTYPE
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
    {"ImageBitmap", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
    {"createImageBitmap", LAZY_BOOTSTRAP_CANVAS, LAZY_BOOTSTRAP_GLOBAL},
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
    {"TextDecoderStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"CompressionStream", LAZY_BOOTSTRAP_STREAMS, LAZY_BOOTSTRAP_GLOBAL},
    {"DecompressionStream", LAZY_BOOTSTRAP_STREAMS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"AudioContext", LAZY_BOOTSTRAP_GAME_AUDIO, LAZY_BOOTSTRAP_GLOBAL},
    {"webkitAudioContext", LAZY_BOOTSTRAP_GAME_AUDIO,
     LAZY_BOOTSTRAP_GLOBAL},
    {"BaseAudioContext", LAZY_BOOTSTRAP_GAME_AUDIO,
     LAZY_BOOTSTRAP_GLOBAL},
    {"AudioBuffer", LAZY_BOOTSTRAP_GAME_AUDIO, LAZY_BOOTSTRAP_GLOBAL},
    {"AudioBufferSourceNode", LAZY_BOOTSTRAP_GAME_AUDIO,
     LAZY_BOOTSTRAP_GLOBAL},
    {"GainNode", LAZY_BOOTSTRAP_GAME_AUDIO, LAZY_BOOTSTRAP_GLOBAL},
    {"Worker", LAZY_BOOTSTRAP_WORKER, LAZY_BOOTSTRAP_GLOBAL},
    {"WebAssembly", LAZY_BOOTSTRAP_WASM, LAZY_BOOTSTRAP_GLOBAL},
    {"speechSynthesis", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"SpeechSynthesis", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"SpeechSynthesisVoice", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"SpeechSynthesisEvent", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"SpeechSynthesisErrorEvent", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"SpeechSynthesisUtterance", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"MediaSource", LAZY_BOOTSTRAP_CAPABILITIES, LAZY_BOOTSTRAP_GLOBAL},
    {"SourceBuffer", LAZY_BOOTSTRAP_CAPABILITIES, LAZY_BOOTSTRAP_GLOBAL},
    {"SourceBufferList", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"FontFaceSet", LAZY_BOOTSTRAP_CAPABILITIES, LAZY_BOOTSTRAP_GLOBAL},
    {"fonts", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_DOCUMENT_PROTOTYPE},
    {"PerformanceEventTiming", LAZY_BOOTSTRAP_CAPABILITIES,
     LAZY_BOOTSTRAP_GLOBAL},
    {"Intl", LAZY_BOOTSTRAP_INTL, LAZY_BOOTSTRAP_GLOBAL},
    {"FileSystemHandle", LAZY_BOOTSTRAP_OPFS, LAZY_BOOTSTRAP_GLOBAL},
    {"FileSystemFileHandle", LAZY_BOOTSTRAP_OPFS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"FileSystemDirectoryHandle", LAZY_BOOTSTRAP_OPFS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"FileSystemWritableFileStream", LAZY_BOOTSTRAP_OPFS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"FileSystemSyncAccessHandle", LAZY_BOOTSTRAP_OPFS,
     LAZY_BOOTSTRAP_GLOBAL},
    {"getDirectory", LAZY_BOOTSTRAP_OPFS,
     LAZY_BOOTSTRAP_STORAGE_MANAGER_PROTOTYPE},
    {"NodeFilter", LAZY_BOOTSTRAP_TRAVERSAL, LAZY_BOOTSTRAP_GLOBAL},
    {"TreeWalker", LAZY_BOOTSTRAP_TRAVERSAL, LAZY_BOOTSTRAP_GLOBAL},
    {"NodeIterator", LAZY_BOOTSTRAP_TRAVERSAL, LAZY_BOOTSTRAP_GLOBAL},
    {"createTreeWalker", LAZY_BOOTSTRAP_TRAVERSAL,
     LAZY_BOOTSTRAP_DOCUMENT_PROTOTYPE},
    {"createNodeIterator", LAZY_BOOTSTRAP_TRAVERSAL,
     LAZY_BOOTSTRAP_DOCUMENT_PROTOTYPE}
};

static const char *runtime_lazy_bootstrap_name(LazyBootstrapFeature feature)
{
    switch (feature) {
    case LAZY_BOOTSTRAP_CANVAS: return "Canvas";
    case LAZY_BOOTSTRAP_INDEXED_DB: return "IndexedDB";
    case LAZY_BOOTSTRAP_MOTION: return "Motion";
    case LAZY_BOOTSTRAP_STREAMS: return "Streams";
    case LAZY_BOOTSTRAP_GAME_AUDIO: return "Game audio";
    case LAZY_BOOTSTRAP_WEBGL: return "WebGL";
    case LAZY_BOOTSTRAP_WORKER: return "Worker";
    case LAZY_BOOTSTRAP_WASM: return "WebAssembly";
    case LAZY_BOOTSTRAP_CAPABILITIES: return "Browser capabilities";
    case LAZY_BOOTSTRAP_INTL: return "Intl";
    case LAZY_BOOTSTRAP_OPFS: return "Origin-private file system";
    case LAZY_BOOTSTRAP_TRAVERSAL: return "DOM traversal";
    case LAZY_BOOTSTRAP_SHADOW: return "Shadow DOM ownership";
    default: return "Platform feature";
    }
}

static JSValue js_runtime_run_worker(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv);
static JSValue js_runtime_trace_worker_message(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv);
static JSValue js_runtime_create_worker_realm(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv);
static JSValue js_runtime_destroy_worker_realm(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv);

#ifndef TILEFINCH_NO_TRACE
static JSValue js_runtime_trace_eval_source(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    static unsigned sequence;
    const char *directory = getenv("TILEFINCH_TRACE_EVAL_SOURCE_DIR");
    if (directory == NULL || directory[0] == '\0' || argc < 1
        || sequence >= 16u) {
        return JS_UNDEFINED;
    }
    size_t length = 0;
    const char *source = JS_ToCStringLen(context, &length, argv[0]);
    if (source == NULL) return JS_EXCEPTION;
    const size_t limit = 512u * 1024u;
    size_t written_length = length < limit ? length : limit;
    char path[512];
    int path_length = snprintf(
        path, sizeof(path), "%s/eval-%04u.js", directory, sequence++);
    FILE *file = path_length > 0 && (size_t) path_length < sizeof(path)
        ? fopen(path, "wb") : NULL;
    bool okay = false;
    if (file != NULL) {
        okay = fwrite(source, 1u, written_length, file) == written_length;
        okay = fclose(file) == 0 && okay;
    }
    fprintf(stderr,
            "tilefinch-eval-source: path=\"%s\" bytes=%zu shown=%zu "
            "written=%s\n",
            path_length > 0 && (size_t) path_length < sizeof(path)
                ? path : "<invalid>",
            length, written_length, okay ? "yes" : "no");
    JS_FreeCString(context, source);
    return JS_UNDEFINED;
}

/* TILEFINCH_LAB_INIT_SCRIPT: read a bounded lab file and evaluate it as a
   diagnostic in the page realm. Failures are printed, never fatal. */
static void runtime_evaluate_lab_init_script(ScriptRuntime *host)
{
    const char *path = tilefinch_lab_init_script_path();
    FILE *file = path == NULL ? NULL : fopen(path, "rb");
    if (file == NULL) {
        printf("census-init-script status=unreadable\n");
        return;
    }
    const long limit = 1024L * 1024L;
    long size = fseek(file, 0L, SEEK_END) == 0 ? ftell(file) : -1L;
    char *source = size >= 0L && size <= limit
                   && fseek(file, 0L, SEEK_SET) == 0
        ? budget_malloc(host->budget, (size_t) size + 1u) : NULL;
    size_t length = source == NULL ? 0
        : fread(source, 1u, (size_t) size, file);
    bool complete = source != NULL && length == (size_t) size
                    && !ferror(file);
    fclose(file);
    if (!complete) {
        budget_free(host->budget, source);
        printf("census-init-script status=too-large\n");
        return;
    }
    source[length] = '\0';
    ScriptResult *scratch = budget_calloc(host->budget, 1, sizeof(*scratch));
    bool ok = scratch != NULL && js_rt_evaluate_source_type_at(
        host->context, source, length, "<lab-init-script>", NULL, NULL,
        TILEFINCH_CREDENTIALS_SAME_ORIGIN, JS_EVAL_TYPE_GLOBAL,
        SCRIPT_COMPILE_SOURCE_DIAGNOSTIC, scratch);
    budget_free(host->budget, source);
    printf("census-init-script status=%s bytes=%zu error=\"%.200s\"\n",
           ok ? "ok" : "failed", length,
           ok || scratch == NULL ? "" : scratch->error);
    budget_free(host->budget, scratch);
}
#endif

static JSValue runtime_lazy_bootstrap_target(
    JSContext *context, uint8_t target)
{
    JSValue global = JS_GetGlobalObject(context);
    if (target == LAZY_BOOTSTRAP_GLOBAL || JS_IsException(global)) {
        return global;
    }
    const char *constructor_name = target == LAZY_BOOTSTRAP_CANVAS_PROTOTYPE
        ? "HTMLCanvasElement"
        : target == LAZY_BOOTSTRAP_DOCUMENT_PROTOTYPE ? "Document"
        : "StorageManager";
    JSValue constructor = JS_GetPropertyStr(
        context, global, constructor_name);
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

/* Only the initial trusted bootstrap receives this temporary channel entry.
   Keep descriptor hardening out of JS reflection: a keys array and its
   enumeration table overlap at exactly the smallest realm's memory peak. */
static JSValue runtime_harden_global_hooks(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    /* These names are bootstrap metadata, not page heap state. Keep their
       single source here so even the smallest realm retains DOM headroom.
       Only bootstrap-defined hooks are listed: natives are installed frozen
       unless their JsRtNative row says otherwise, so they need no entry. */
    static const char *const protected_names[] = {
        "__tilefinchBoundedAncestorPath",
        "__tilefinchBlobBytes",
        "__tilefinchBlobForURL",
        "__tilefinchCloneWorkerValue",
        "__tilefinchAbortFileReaderForWorker",
        "__tilefinchAbortIndexedDBForWorker",
        "__tilefinchAbortSignalBrand",
        "__tilefinchBlobTextForWorker",
        "__tilefinchAbortXHRForWorker",
        "__tilefinchCloseWebSocketForWorker",
        "__tilefinchPrepareMessagePortTransfer",
        "__tilefinchCreatePrivateWeakMap",
        "__tilefinchCreateMessageEvent",
        "__tilefinchCreateWorkerCrypto",
        "__tilefinchSetCryptoBufferNormalizer",
        "__tilefinchCreateWorkerPerformance",
        "__tilefinchWorkerCloneIntrinsics",
        "__tilefinchDispatchActivationHandle",
        "__tilefinchDispatchAt",
        "__tilefinchDispatchDOMContentLoaded",
        "__tilefinchDispatchHandle",
        "__tilefinchDispatchInputHandle",
        "__tilefinchDispatchSubmitHandle",
        "__tilefinchDispatchWindowEventCheckpointed",
        "__tilefinchDispatchEventTargetCheckpointed",
        "__tilefinchEventObserverDelta",
        "__tilefinchFetchForWorker",
        "__tilefinchFetchWorkerScript",
        "__tilefinchFetchWorkerScriptSync",
        "__tilefinchFocusEventsObserved",
        "__tilefinchInvokeEventTargetCheckpointed",
        "__tilefinchMarkNativeFunction",
        "__tilefinchNormalizeTimerDelay",
        "__tilefinchScheduleTimeout",
        "__tilefinchScheduleInterval",
        "__tilefinchScheduleTask",
        "__tilefinchCancelTimer",
        "__tilefinchSchedulerTime",
        "__tilefinchScheduleReservedTask",
        "__tilefinchScheduleReservedFrame",
        "__tilefinchPointerHoverEventsObserved",
        "__tilefinchPointerMarkupChanged",
        "__tilefinchPointerMoveEventsObserved",
        "__tilefinchRegisterNativeNodeStateCleanup",
        "__tilefinchReportUncaught",
        "__tilefinchRetireNativeNodeState",
        "__tilefinchHoldEventState",
        "__tilefinchReleaseEventHolders",
        "__tilefinchRunTask",
        "__tilefinchSetFocusHandle",
        "__tilefinchWrap",
        /* Invoked by native code or later bootstrap modules by name.
           ReceiveMessage deliberately remains replaceable for failure tests. */
        "__tilefinchCommitSameDocument",
        "__tilefinchDeliverEventSource",
        "__tilefinchDeliverWebSocket",
        "__tilefinchDeliverMultiplayer",
        "__tilefinchDeliverNetwork",
        "__tilefinchDocumentURLRevision",
        "__tilefinchIntersectionRecheck",
        "__tilefinchMediaRecheck",
        "__tilefinchMaybeStartMotion",
        "__tilefinchMotionRecheck",
        "__tilefinchBeginMotionObservation",
        "__tilefinchParserMutationCheckpoint",
        "__tilefinchPumpTimers",
        "__tilefinchSchedulerSnapshot",
        "__tilefinchRecordNavigationTiming",
        "__tilefinchRecordResourceTiming",
        "__tilefinchRefreshNamedProperties",
        "__tilefinchResizeRecheck",
        "__tilefinchRestoreSameDocument",
        "__tilefinchSetFrameWindowState",
        "__tilefinchTrustedEvent",
        "__tilefinchTrustedString",
        "__tilefinchUpdateGamepad",
    };
    JSAtom protected_atoms[128];
    const uint32_t count = sizeof(protected_names) / sizeof(protected_names[0]);
    _Static_assert(sizeof(protected_names) / sizeof(protected_names[0]) <= 128,
                   "bootstrap hook atom bound");
    uint32_t retained = 0, key_count = 0;
    JSPropertyEnum *keys = NULL;
    JSValue global = JS_UNDEFINED;
    JSValue result = JS_EXCEPTION;
    for (; retained < count; retained++) {
        JSAtom atom = JS_NewAtom(context, protected_names[retained]);
        if (atom == JS_ATOM_NULL) {
            if (!JS_HasException(context))
                JS_ThrowTypeError(context, "Invalid bootstrap hook name");
            goto done;
        }
        protected_atoms[retained] = atom;
    }
    global = JS_GetGlobalObject(context);
    if (JS_GetOwnPropertyNames(context, &keys, &key_count, global,
                              JS_GPN_STRING_MASK) < 0) goto done;
    if (key_count > 8192u) {
        JS_ThrowRangeError(context, "Bootstrap global table exceeds bound");
        goto done;
    }
    for (uint32_t at = 0; at < key_count; at++) {
        JSAtom atom = keys[at].atom;
        bool protect = false;
        for (uint32_t i = 0; i < count; i++)
            if (protected_atoms[i] == atom) { protect = true; break; }
        const char *name = JS_AtomToCString(context, atom);
        if (name == NULL) goto done;
        bool hook = strncmp(name, "__tilefinch", sizeof("__tilefinch") - 1u) == 0;
        JS_FreeCString(context, name);
        if (!hook) continue;
        JSPropertyDescriptor descriptor;
        int found = JS_GetOwnProperty(context, &descriptor, global, atom);
        if (found < 0) goto done;
        if (!found) continue;
        protect = protect && JS_IsFunction(context, descriptor.value);
        bool enumerable = (descriptor.flags & JS_PROP_ENUMERABLE) != 0;
        JS_FreeValue(context, descriptor.value);
        JS_FreeValue(context, descriptor.getter);
        JS_FreeValue(context, descriptor.setter);
        if (!enumerable && !protect) continue;
        int flags = JS_PROP_HAS_ENUMERABLE;
        if (protect) flags |= JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_WRITABLE;
        if (JS_DefineProperty(context, global, atom, JS_UNDEFINED,
                              JS_UNDEFINED, JS_UNDEFINED, flags) < 0) goto done;
    }
    result = JS_UNDEFINED;
done:
    JS_FreePropertyEnum(context, keys, key_count);
    JS_FreeValue(context, global);
    for (uint32_t i = 0; i < retained; i++) JS_FreeAtom(context, protected_atoms[i]);
    return result;
}

/* Trusted bootstrap attaches its native-readable state (see
   ScriptRuntime.host_state) to this null-prototype object, which exists only
   while that bootstrap evaluates. With `with_state`, `state` is a
   Float64Array over host_state itself. */
static bool runtime_define_host_channel(ScriptRuntime *runtime,
                                        bool with_state)
{
    JSContext *context = runtime->context;
    JSValue channel = JS_NewObjectProto(context, JS_NULL);
    if (JS_IsException(channel)) return false;
    if (with_state) {
        static const JsRtNative harden_global_hooks = {
            "hardenGlobalHooks", runtime_harden_global_hooks, 0, 0 };
        if (!js_rt_install_native(context, channel, &harden_global_hooks)) {
            JS_FreeValue(context, channel);
            return false;
        }
        JSValue buffer = JS_NewArrayBuffer(
            context, (uint8_t *) runtime->host_state,
            sizeof(runtime->host_state), NULL, NULL, false);
        JSValueConst arguments[3] = { buffer, JS_UNDEFINED, JS_UNDEFINED };
        JSValue state = JS_IsException(buffer) ? JS_EXCEPTION
            : JS_NewTypedArray(context, 3, arguments,
                               JS_TYPED_ARRAY_FLOAT64);
        JS_FreeValue(context, buffer);
        if (JS_IsException(state)
            || JS_DefinePropertyValueStr(context, channel, "state", state,
                                         0) < 0) {
            JS_FreeValue(context, channel);
            return false;
        }
    }
    JSValue global = JS_GetGlobalObject(context);
    bool defined = JS_DefinePropertyValueStr(
        context, global, "__tilefinchHostChannel", channel,
        JS_PROP_CONFIGURABLE) >= 0;
    JS_FreeValue(context, global);
    return defined;
}

/* Keep the record the bootstrap attached as `property` (when it did) and
   remove the channel before author code can reach it. */
static bool runtime_take_host_channel(ScriptRuntime *runtime,
                                      const char *property, JSValue *slot)
{
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context);
    JSAtom atom = JS_NewAtom(context, "__tilefinchHostChannel");
    JSValue channel = atom == JS_ATOM_NULL ? JS_UNDEFINED
        : JS_GetProperty(context, global, atom);
    JSValue value = JS_IsObject(channel)
        ? JS_GetPropertyStr(context, channel, property) : JS_UNDEFINED;
    if (JS_IsObject(value)) {
        JS_FreeValue(context, *slot);
        *slot = value;
    } else {
        JS_FreeValue(context, value);
    }
    JS_FreeValue(context, channel);
    bool removed = atom != JS_ATOM_NULL
        && JS_DeleteProperty(context, global, atom, 0) >= 0;
    if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
    JS_FreeValue(context, global);
    return removed;
}

/* Lazy modules run after hardening.js deleted the eager test-probe flag.
   Define it for the module's synchronous initialization only, over any
   page-defined value, and remove it before author code resumes. Shipping
   realms get `false`, so a page cannot switch the probes on.

   Only TRUE counts as success. Without JS_PROP_THROW, QuickJS answers FALSE
   (not -1) when a page owns a non-configurable flag (a top-level `var`, or
   Object.defineProperty), and the page's value would then reach the module.
   The caller fails the load instead: no module, so no probes. */
static bool runtime_lazy_test_probes(ScriptRuntime *runtime, bool open)
{
    JSContext *context = runtime->context;
    JSValue global = JS_GetGlobalObject(context);
    JSAtom atom = JS_NewAtom(context, "__tilefinchTestProbes");
    bool probes = false;
#ifdef JS_RT_TEST_INSTRUMENTS
    probes = runtime->test_probes;
#endif
    bool ok = !JS_IsException(global) && atom != JS_ATOM_NULL
        && (open
            ? JS_DefinePropertyValue(context, global, atom,
                                     JS_NewBool(context, probes),
                                     JS_PROP_CONFIGURABLE) > 0
            : JS_DeleteProperty(context, global, atom, 0) > 0);
    if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
    JS_FreeValue(context, global);
    return ok;
}

/* Bridges a lazy bootstrap group needs only while it initializes. */
static const JsRtNative runtime_checkpoint_continuation = {
    "__tilefinchQueueCheckpointContinuation",
    js_queue_checkpoint_continuation, 1, JS_RT_NATIVE_TRANSIENT };
static const JsRtNative runtime_motion_source_bridge = {
    "__tilefinchMotionSourceBridge", js_stylesheet_motion_source, 0,
    JS_RT_NATIVE_TRANSIENT };
static const JsRtNative runtime_opfs_command = {
    "__tilefinchOpfsCommand", js_opfs_command, 4, JS_RT_NATIVE_TRANSIENT };
#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
static const JsRtNative runtime_game_audio_reference_natives[] = {
    { "__tilefinchGameAudioDecode", js_game_audio_decode, 1, 0 },
    { "__tilefinchGameAudioCommand", js_game_audio_command, 11, 0 },
};
#endif

static bool runtime_load_lazy_bootstrap(
    ScriptRuntime *runtime, LazyBootstrapFeature feature)
{
    if (runtime == NULL || feature >= LAZY_BOOTSTRAP_COUNT) return false;
    uint8_t *state = &runtime->lazy_bootstrap_state[feature];
    if (*state == 2) return true;
    if (*state == 1 || *state == 3) return false;
    *state = 1;
    runtime_remove_lazy_bootstrap_properties(runtime->context, feature);
    bool trace = tilefinch_trace_js_startup();
    uint64_t total_us = 0;
    uint64_t compile_us = 0;
    bool evaluated = false;
    if (!runtime_lazy_test_probes(runtime, true)) {
        /* A page-defined, non-configurable flag: fail the load. */
    } else if (feature == LAZY_BOOTSTRAP_CANVAS) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_canvas,
                         browser_bootstrap_canvas_bytecode),
            "<browser-canvas-lazy>", &browser_bootstrap_canvas_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_INDEXED_DB) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSAtom bridge_atom = JS_NewAtom(
            runtime->context,
            "__tilefinchQueueCheckpointContinuation");
        bool bridge_ready = !JS_IsException(global)
            && bridge_atom != JS_ATOM_NULL
            && js_rt_install_native(
                runtime->context, global, &runtime_checkpoint_continuation)
            && runtime_define_host_channel(runtime, false);
        if (bridge_ready) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                             browser_bootstrap_indexeddb,
                             browser_bootstrap_indexeddb_bytecode),
                "<browser-indexeddb-lazy>",
                &browser_bootstrap_indexeddb_bytecode, false, trace,
                &total_us, &compile_us);
            /* The lazy module captures this capability synchronously. Keep it
               unavailable to author code even when module evaluation fails
               before its own cleanup runs. */
        }
        if (bridge_atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(
                runtime->context, global, bridge_atom, 0);
            JS_FreeAtom(runtime->context, bridge_atom);
        }
        if (bridge_ready
            && !runtime_take_host_channel(
                   runtime, "indexedDBStats",
                   &runtime->host_indexed_db_stats))
            evaluated = false;
        JS_FreeValue(runtime->context, global);
    } else if (feature == LAZY_BOOTSTRAP_MOTION) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSAtom bridge_atom = JS_NewAtom(
            runtime->context, "__tilefinchMotionSourceBridge");
        bool bridge_ready = !JS_IsException(global)
            && bridge_atom != JS_ATOM_NULL
            && js_rt_install_native(
                runtime->context, global, &runtime_motion_source_bridge)
            && runtime_define_host_channel(runtime, false);
        if (bridge_ready) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                             browser_bootstrap_motion,
                             browser_bootstrap_motion_bytecode),
                "<browser-motion-lazy>",
                &browser_bootstrap_motion_bytecode, false, trace,
                &total_us, &compile_us);
        }
        /* Capture only inside trusted initialization: exposing raw external
           stylesheet text to authors would bypass CSSOM origin checks. Also
           remove the capability when initialization fails before its cleanup. */
        if (bridge_atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(runtime->context, global, bridge_atom, 0);
            JS_FreeAtom(runtime->context, bridge_atom);
        }
        if (bridge_ready
            && !runtime_take_host_channel(runtime, "styleFlushed",
                                           &runtime->motion_style_flushed))
            evaluated = false;
        JS_FreeValue(runtime->context, global);
    } else if (feature == LAZY_BOOTSTRAP_STREAMS) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_streams,
                         browser_bootstrap_streams_bytecode),
            "<browser-streams-lazy>",
            &browser_bootstrap_streams_bytecode, false, trace,
            &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_GAME_AUDIO) {
        if (runtime->game_audio_internal_slots) {
            evaluated = audio_slots_initialize(runtime);
        }
#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
        else {
            JSValue global = JS_GetGlobalObject(runtime->context);
            bool ready = !JS_IsException(global)
                && JS_RT_INSTALL_NATIVES(runtime->context, global,
                                         runtime_game_audio_reference_natives);
            if (ready) evaluated = runtime_evaluate_bootstrap_profiled(
                runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                             browser_bootstrap_game_audio,
                             browser_bootstrap_game_audio_bytecode),
                "<browser-game-audio-lazy>",
                &browser_bootstrap_game_audio_bytecode, false, trace,
                &total_us, &compile_us);
            JS_FreeValue(runtime->context, global);
        }
#endif
    } else if (feature == LAZY_BOOTSTRAP_WEBGL) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_webgl,
                         browser_bootstrap_webgl_bytecode),
            "<browser-webgl-lazy>", &browser_bootstrap_webgl_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_WORKER) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        bool slot_ready = !JS_IsException(global)
            && JS_DefinePropertyValueStr(
                   runtime->context, global,
                   "__tilefinchInstallWorker", JS_UNDEFINED,
                   JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE) >= 0;
        if (slot_ready) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                             browser_bootstrap_worker,
                             browser_bootstrap_worker_bytecode),
                "<browser-worker-lazy>",
                &browser_bootstrap_worker_bytecode, false, trace,
                &total_us, &compile_us);
        }
        JSValue installer = evaluated
            ? JS_GetPropertyStr(
                  runtime->context, global,
                  "__tilefinchInstallWorker")
            : JS_UNDEFINED;
        bool hidden = false;
        if (slot_ready) {
            hidden = JS_SetPropertyStr(
                runtime->context, global,
                "__tilefinchInstallWorker", JS_UNDEFINED) >= 0;
        }
        JSValue run_worker = JS_NewCFunction(
            runtime->context, js_runtime_run_worker,
            "__tilefinchRunWorker", 3);
        JSValue trace_worker_message = JS_NewCFunction(
            runtime->context, js_runtime_trace_worker_message,
            "__tilefinchTraceWorkerMessage", 2);
        JSValue create_realm = JS_NewCFunction(
            runtime->context, js_runtime_create_worker_realm,
            "__tilefinchCreateWorkerRealm", 2);
        JSValue destroy_realm = JS_NewCFunction(
            runtime->context, js_runtime_destroy_worker_realm,
            "__tilefinchDestroyWorkerRealm", 1);
        char creator_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = "null";
        if (runtime->bridge.opaque_origin
            || runtime->bridge.document_url == NULL
            || !tilefinch_url_origin(
                runtime->bridge.document_url, creator_origin,
                sizeof(creator_origin))) {
            snprintf(creator_origin, sizeof(creator_origin), "null");
        }
        JSValue creator_origin_value = JS_NewString(
            runtime->context, creator_origin);
        JSValue creator_secure_context = JS_NewBool(
            runtime->context,
            runtime->bridge.document_url != NULL
                && tilefinch_url_potentially_trustworthy(
                    runtime->bridge.document_url));
        JSValue creator_url_value = JS_NewString(
            runtime->context, runtime->bridge.document_url == NULL
                ? "about:blank" : runtime->bridge.document_url);
        bool installed = hidden
            && JS_IsFunction(runtime->context, installer)
            && !JS_IsException(run_worker)
            && !JS_IsException(trace_worker_message)
            && !JS_IsException(create_realm)
            && !JS_IsException(destroy_realm)
            && !JS_IsException(creator_origin_value)
            && !JS_IsException(creator_secure_context)
            && !JS_IsException(creator_url_value);
        if (installed) {
            JSValue arguments[7] = {
                run_worker, trace_worker_message, create_realm,
                destroy_realm, creator_origin_value,
                creator_secure_context, creator_url_value};
            JSValue result = JS_Call(
                runtime->context, installer, JS_UNDEFINED,
                7, arguments);
            installed = !JS_IsException(result);
            JS_FreeValue(runtime->context, result);
        }
        JS_FreeValue(runtime->context, creator_url_value);
        JS_FreeValue(runtime->context, creator_secure_context);
        JS_FreeValue(runtime->context, creator_origin_value);
        JS_FreeValue(runtime->context, destroy_realm);
        JS_FreeValue(runtime->context, create_realm);
        JS_FreeValue(runtime->context, trace_worker_message);
        JS_FreeValue(runtime->context, run_worker);
        JS_FreeValue(runtime->context, installer);
        JS_FreeValue(runtime->context, global);
        evaluated = evaluated && installed;
    } else if (feature == LAZY_BOOTSTRAP_WASM) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_wasm,
                         browser_bootstrap_wasm_bytecode),
            "<browser-wasm-lazy>", &browser_bootstrap_wasm_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_CAPABILITIES) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_capabilities,
                         browser_bootstrap_capabilities_bytecode),
            "<browser-capabilities-lazy>",
            &browser_bootstrap_capabilities_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_INTL) {
        /* Hand intl.js the time zone Date's local time uses through a
           temporary global, deleted as soon as the module has captured it. */
        char time_zone[64];
        js_rt_default_time_zone(&runtime->bridge, time_zone,
                                sizeof(time_zone));
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSAtom zone_atom = JS_NewAtom(
            runtime->context, "__tilefinchDefaultTimeZone");
        if (!JS_IsException(global) && zone_atom != JS_ATOM_NULL) {
            (void) JS_DefinePropertyValue(
                runtime->context, global, zone_atom,
                JS_NewString(runtime->context, time_zone),
                JS_PROP_CONFIGURABLE);
        }
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_intl,
                         browser_bootstrap_intl_bytecode),
            "<browser-intl-lazy>", &browser_bootstrap_intl_bytecode,
            false, trace, &total_us, &compile_us);
        if (zone_atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(
                runtime->context, global, zone_atom, 0);
            JS_FreeAtom(runtime->context, zone_atom);
        }
        JS_FreeValue(runtime->context, global);
    } else if (feature == LAZY_BOOTSTRAP_TRAVERSAL) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_traversal,
                         browser_bootstrap_traversal_bytecode),
            "<browser-traversal-lazy>",
            &browser_bootstrap_traversal_bytecode, false, trace,
            &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_SHADOW) {
        evaluated = runtime_evaluate_bootstrap_profiled(
            runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                         browser_bootstrap_shadow,
                         browser_bootstrap_shadow_bytecode),
            "<browser-shadow-lazy>", &browser_bootstrap_shadow_bytecode,
            false, trace, &total_us, &compile_us);
    } else if (feature == LAZY_BOOTSTRAP_OPFS) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        JSAtom bridge_atom = JS_NewAtom(
            runtime->context, "__tilefinchOpfsCommand");
        JSAtom storage_atom = JS_NewAtom(
            runtime->context, "__tilefinchOpfsStorageManager");
        bool bridge_ready = !JS_IsException(global)
            && bridge_atom != JS_ATOM_NULL
            && storage_atom != JS_ATOM_NULL
            && js_rt_install_native(
                runtime->context, global, &runtime_opfs_command)
            && JS_DefinePropertyValue(
                   runtime->context, global, storage_atom,
                   JS_DupValue(runtime->context,
                               runtime->opfs_storage_manager),
                   JS_PROP_CONFIGURABLE) >= 0;
        if (bridge_ready) {
            evaluated = runtime_evaluate_bootstrap_profiled(
                runtime, TILEFINCH_BOOTSTRAP_SOURCE_ARGS(
                             browser_bootstrap_opfs,
                             browser_bootstrap_opfs_bytecode),
                "<browser-opfs-lazy>", &browser_bootstrap_opfs_bytecode,
                false, trace, &total_us, &compile_us);
        }
        if (bridge_atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(
                runtime->context, global, bridge_atom, 0);
            JS_FreeAtom(runtime->context, bridge_atom);
        }
        if (storage_atom != JS_ATOM_NULL) {
            (void) JS_DeleteProperty(
                runtime->context, global, storage_atom, 0);
            JS_FreeAtom(runtime->context, storage_atom);
        }
        JS_FreeValue(runtime->context, global);
    }
    if (!runtime_lazy_test_probes(runtime, false)) evaluated = false;
#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
    if (evaluated && feature == LAZY_BOOTSTRAP_GAME_AUDIO
        && !runtime->game_audio_internal_slots) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        runtime->game_audio_host_suspend = JS_GetPropertyStr(
            runtime->context, global,
            "__tilefinchSuspendGameAudioFromHost");
        runtime->game_audio_host_complete = JS_GetPropertyStr(
            runtime->context, global,
            "__tilefinchCompleteGameAudioVoice");
        JSAtom suspend_atom = JS_NewAtom(
            runtime->context, "__tilefinchSuspendGameAudioFromHost");
        JSAtom complete_atom = JS_NewAtom(
            runtime->context, "__tilefinchCompleteGameAudioVoice");
        bool functions_ok = JS_IsFunction(
                runtime->context, runtime->game_audio_host_suspend)
            && JS_IsFunction(
                runtime->context, runtime->game_audio_host_complete);
        bool suspend_deleted = suspend_atom != JS_ATOM_NULL
            && JS_DeleteProperty(
                   runtime->context, global, suspend_atom, 0) > 0;
        bool complete_deleted = complete_atom != JS_ATOM_NULL
            && JS_DeleteProperty(
                   runtime->context, global, complete_atom, 0) > 0;
        bool captured = functions_ok && suspend_deleted && complete_deleted;
        if (suspend_atom != JS_ATOM_NULL)
            JS_FreeAtom(runtime->context, suspend_atom);
        if (complete_atom != JS_ATOM_NULL)
            JS_FreeAtom(runtime->context, complete_atom);
        JS_FreeValue(runtime->context, global);
        if (!captured) {
            JS_FreeValue(runtime->context,
                         runtime->game_audio_host_suspend);
            runtime->game_audio_host_suspend = JS_UNDEFINED;
            JS_FreeValue(runtime->context,
                         runtime->game_audio_host_complete);
            runtime->game_audio_host_complete = JS_UNDEFINED;
            evaluated = false;
        }
    }
#endif
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

bool script_runtime_game_audio_status(
    const ScriptRuntime *runtime, ScriptGameAudioStatus *status)
{
    if (status == NULL) return false;
    *status = (ScriptGameAudioStatus) {0};
    if (runtime == NULL) return false;
    uint8_t state = runtime->lazy_bootstrap_state[LAZY_BOOTSTRAP_GAME_AUDIO];
    status->selected_native = runtime->game_audio_internal_slots;
    status->lazy_installed = state == 2;
    status->native_present = runtime->game_audio_slots != NULL;
    status->failed = state == 3;
    return true;
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
    if (tilefinch_trace_js_startup()
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
    /* A prototype accessor (document.fonts) reads with the original
       receiver; the receiver does not matter for prototype methods. */
    JSAtom atom = JS_NewAtom(context, property->name);
    JSValue result = atom == JS_ATOM_NULL ? JS_EXCEPTION
        : JS_GetPropertyInternal(
              context, target, atom,
              property->target == LAZY_BOOTSTRAP_GLOBAL ? target : this_value,
              0);
    if (atom != JS_ATOM_NULL) JS_FreeAtom(context, atom);
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

static JSValue js_runtime_ensure_shadow_bootstrap(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !runtime_load_lazy_bootstrap(
            bridge->host, LAZY_BOOTSTRAP_SHADOW))
        return JS_ThrowInternalError(context,
            "Shadow DOM ownership could not be initialized");
    return JS_TRUE;
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

static JSValue js_runtime_ensure_webgl_bootstrap(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || !runtime_load_lazy_bootstrap(
            bridge->host, LAZY_BOOTSTRAP_WEBGL)) {
        return JS_ThrowInternalError(
            context, "WebGL support could not be initialized");
    }
    return JS_TRUE;
}

static JSContext *js_runtime_worker_realm_context(
    ScriptRuntime *host, JSValueConst global)
{
    if (host == NULL || !JS_IsObject(global)) return NULL;
    for (size_t slot = 0; slot < SCRIPT_WORKER_REALM_LIMIT; slot++) {
        if (host->worker_realms[slot].context != NULL
            && JS_VALUE_GET_PTR(host->worker_realms[slot].global)
                   == JS_VALUE_GET_PTR(global))
            return host->worker_realms[slot].context;
    }
    return NULL;
}

/* Copy every own property of the page-side scope object onto the realm's
   global object: data properties by value, accessors by getter/setter, all
   with their original attributes. The page-realm functions keep working when
   called from the worker realm because QuickJS invokes C and script
   functions in the realm that created them. */
static bool js_runtime_copy_scope_properties(
    JSContext *context, JSContext *realm, JSValueConst global,
    JSValueConst scope)
{
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(
            context, &properties, &count, scope,
            JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return false;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        JSPropertyDescriptor descriptor;
        int has = JS_GetOwnProperty(
            context, &descriptor, scope, properties[i].atom);
        if (has < 0) { ok = false; break; }
        if (has == 0) continue;
        int flags = descriptor.flags
            & (JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        if (descriptor.flags & JS_PROP_GETSET) {
            ok = JS_DefineProperty(
                     realm, global, properties[i].atom, JS_UNDEFINED,
                     descriptor.getter, descriptor.setter,
                     flags | JS_PROP_HAS_GET | JS_PROP_HAS_SET
                         | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE)
                 >= 0;
        } else {
            ok = JS_DefineProperty(
                     realm, global, properties[i].atom, descriptor.value,
                     JS_UNDEFINED, JS_UNDEFINED,
                     flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE
                         | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE)
                 >= 0;
        }
        JS_FreeValue(context, descriptor.value);
        JS_FreeValue(context, descriptor.getter);
        JS_FreeValue(context, descriptor.setter);
    }
    JS_FreePropertyEnum(context, properties, count);
    return ok;
}

/* createWorkerRealm(scope, prototype, internal_init): a fresh context whose global object
   inherits from `prototype` (DedicatedWorkerGlobalScope.prototype) and owns a
   copy of every scope property plus `self`. The optional bounded initializer
   is authored browser bootstrap, evaluated before the realm is published; it
   is not a second author-script evaluator and therefore does not bypass the
   worker-src check in js_runtime_run_worker. Returns that global. */
static JSValue js_runtime_create_worker_realm(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL || argc < 2
        || !JS_IsObject(argv[0]) || !JS_IsObject(argv[1]))
        return JS_ThrowTypeError(context, "invalid worker realm request");
    size_t initializer_length = 0;
    const char *initializer = NULL;
    if (argc > 2 && !JS_IsUndefined(argv[2])) {
        initializer = JS_ToCStringLen(
            context, &initializer_length, argv[2]);
        if (initializer == NULL) return JS_EXCEPTION;
        if (initializer_length >
            SCRIPT_WORKER_REALM_INITIALIZER_MAX_BYTES) {
            JS_FreeCString(context, initializer);
            return JS_ThrowRangeError(
                context, "worker realm initializer exceeds bound");
        }
    }
    ScriptRuntime *host = bridge->host;
#ifndef __PSP__
    if (tilefinch_test_faults()->refuse_next_worker_realm) {
        tilefinch_test_faults()->refuse_next_worker_realm = false;
        return JS_ThrowOutOfMemory(context);
    }
#endif
    size_t slot = SCRIPT_WORKER_REALM_LIMIT;
    for (size_t i = 0; i < SCRIPT_WORKER_REALM_LIMIT; i++) {
        if (host->worker_realms[i].context == NULL) { slot = i; break; }
    }
    if (slot == SCRIPT_WORKER_REALM_LIMIT)
        return JS_ThrowRangeError(context, "worker realm quota exceeded");
    JSContext *realm = JS_NewContext(host->runtime);
    if (realm == NULL) {
        if (initializer != NULL) JS_FreeCString(context, initializer);
        return JS_ThrowOutOfMemory(context);
    }
    JS_SetContextOpaque(realm, bridge);
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    JS_SetDynamicCodeEnabled(realm, bridge->dynamic_code_allowed);
    JS_SetHostGetCodeForEval(
        realm, js_runtime_host_get_code_for_eval, NULL);
#endif
    JSValue global = JS_GetGlobalObject(realm);
    bool ok = !JS_IsException(global)
        && JS_SetPrototype(realm, global, argv[1]) >= 0
        && js_runtime_copy_scope_properties(context, realm, global, argv[0])
        && JS_DefinePropertyValueStr(
               realm, global, "self", JS_DupValue(realm, global),
               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) >= 0;
    JSValue initializer_result = JS_UNDEFINED;
    if (ok && initializer != NULL) {
        initializer_result = JS_Eval(
            realm, initializer, initializer_length, "<worker-platform>",
            JS_EVAL_TYPE_GLOBAL);
        ok = !JS_IsException(initializer_result);
    }
    if (initializer != NULL) JS_FreeCString(context, initializer);
    if (!ok) {
        JSValue exception = JS_IsException(initializer_result)
            ? JS_GetException(realm) : JS_UNDEFINED;
        JS_FreeValue(realm, initializer_result);
        JS_FreeValue(realm, global);
        JS_FreeContext(realm);
        return JS_IsUndefined(exception)
            ? JS_EXCEPTION : JS_Throw(context, exception);
    }
    JS_FreeValue(realm, initializer_result);
    host->worker_realms[slot].context = realm;
    host->worker_realms[slot].global = JS_DupValue(realm, global);
    return global;
}

static JSValue js_runtime_destroy_worker_realm(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->host == NULL || argc < 1)
        return JS_UNDEFINED;
    ScriptRuntime *host = bridge->host;
    for (size_t slot = 0; slot < SCRIPT_WORKER_REALM_LIMIT; slot++) {
        if (host->worker_realms[slot].context == NULL
            || JS_VALUE_GET_PTR(host->worker_realms[slot].global)
                   != JS_VALUE_GET_PTR(argv[0]))
            continue;
        JSContext *realm = host->worker_realms[slot].context;
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        JS_RetireContext(realm);
#endif
        JS_FreeValue(realm, host->worker_realms[slot].global);
        host->worker_realms[slot].global = JS_UNDEFINED;
        host->worker_realms[slot].context = NULL;
        /* Objects created in the realm stay valid: functions hold their own
           realm references, so the context is released when the last one
           goes away. */
        JS_FreeContext(realm);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
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
    int32_t operation = 0;
    if (argc > 3 && JS_ToInt32(context, &operation, argv[3]) < 0) {
        if (source != NULL) JS_FreeCString(context, source);
        if (url != NULL) JS_FreeCString(context, url);
        return JS_EXCEPTION;
    }
    TilefinchCredentialsMode module_credentials =
        TILEFINCH_CREDENTIALS_SAME_ORIGIN;
    if (operation == 1 && argc > 4 && !JS_IsUndefined(argv[4])) {
        const char *credentials = JS_ToCString(context, argv[4]);
        if (credentials == NULL) {
            if (source != NULL) JS_FreeCString(context, source);
            if (url != NULL) JS_FreeCString(context, url);
            return JS_EXCEPTION;
        }
        if (strcmp(credentials, "omit") == 0) {
            module_credentials = TILEFINCH_CREDENTIALS_OMIT;
        } else if (strcmp(credentials, "include") == 0) {
            module_credentials = TILEFINCH_CREDENTIALS_INCLUDE;
        } else if (strcmp(credentials, "same-origin") != 0) {
            JS_FreeCString(context, credentials);
            if (source != NULL) JS_FreeCString(context, source);
            if (url != NULL) JS_FreeCString(context, url);
            return JS_ThrowTypeError(
                context, "invalid module Worker credentials mode");
        }
        JS_FreeCString(context, credentials);
    }
    if (source == NULL || url == NULL) {
        if (source != NULL) JS_FreeCString(context, source);
        if (url != NULL) JS_FreeCString(context, url);
        return JS_EXCEPTION;
    }
    const TilefinchContentSecurityPolicy *csp =
        &bridge->document->content_security_policy;
    bool allowed = operation == 3
        || (operation == 4 && bridge->dynamic_code_allowed)
        || (operation == 2
            ? tilefinch_csp_allows_request(
                  csp, TILEFINCH_DESTINATION_SCRIPT, url)
            : tilefinch_csp_allows_worker(csp, url));
    if (!allowed || source_length > bridge->maximum_script_file_bytes) {
        JS_FreeCString(context, source);
        JS_FreeCString(context, url);
        return allowed
            ? JS_ThrowRangeError(context, "worker source quota exceeded")
            : JS_ThrowTypeError(
                  context, operation == 2
                      ? "worker import blocked by Content Security Policy"
                      : operation == 4
                          ? "worker string timer blocked by Content Security Policy"
                          : "worker blocked by Content Security Policy");
    }
#ifndef TILEFINCH_NO_TRACE
    if (tilefinch_trace_worker_source()) {
        const size_t trace_limit = 16u * 1024u;
        size_t trace_length = source_length < trace_limit
            ? source_length : trace_limit;
        fprintf(stderr, "tilefinch-worker-source: url=%s bytes=%zu shown=%zu\n",
                url, source_length, trace_length);
        if (trace_length != 0)
            (void) fwrite(source, 1u, trace_length, stderr);
        fprintf(stderr, "\ntilefinch-worker-source-end: truncated=%d\n",
                source_length > trace_length ? 1 : 0);
    }
#endif
    /* Classic scripts use the worker global directly. Module workers compile
       as actual modules: tokens in strings/comments are never rewritten, the
       top-level this value is undefined, and import.meta comes from QuickJS. */
    JSContext *realm = js_runtime_worker_realm_context(bridge->host, argv[0]);
    if (realm == NULL) {
        JS_FreeCString(context, source);
        JS_FreeCString(context, url);
        return JS_ThrowTypeError(context, "worker realm is not active");
    }
#if defined(CONFIG_PROPERTY_FAULT_TRACE)
    runtime_configure_property_fault_trace(JS_GetRuntime(realm));
#endif
    JSValue result;
    if (operation == 1) {
        bool registered = js_rt_runtime_module_root_register(
            bridge->host, url, url, NULL,
            module_credentials);
        result = registered
            ? JS_Eval(realm, source, source_length, url,
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)
            : JS_ThrowRangeError(realm, "worker module metadata limit exceeded");
        if (!JS_IsException(result)
            && !js_rt_module_set_import_meta(realm, result, url, true)) {
            JS_FreeValue(realm, result);
            result = JS_EXCEPTION;
        }
        if (!JS_IsException(result)) result = JS_EvalFunction(realm, result);
    } else {
        result = JS_Eval(
            realm, source, source_length, url, JS_EVAL_TYPE_GLOBAL);
    }
    JS_FreeCString(context, source);
    JS_FreeCString(context, url);
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(realm);
        return JS_Throw(context, exception);
    }
    return result;
}

static JSValue js_runtime_trace_worker_message(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
#ifndef TILEFINCH_NO_TRACE
    const char *trace_filter = getenv("TILEFINCH_TRACE_WORKER_MESSAGES");
    if (trace_filter != NULL && argc == 1) {
        const char *direction = JS_ToCString(context, argv[0]);
        if (direction == NULL) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
            return JS_FALSE;
        }
        bool selected = trace_filter[0] == '\0'
            || strcmp(trace_filter, "1") == 0
            || strcmp(trace_filter, direction) == 0;
        JS_FreeCString(context, direction);
        return JS_NewBool(context, selected);
    }
    if (trace_filter != NULL && argc >= 2) {
        const char *direction = JS_ToCString(context, argv[0]);
        if (direction != NULL) {
            /* A busy worker can exchange hundreds of structured messages.
               Validation often needs only one boundary (for example the
               first owner-handler failure), and serializing every payload
               perturbs both heap pressure and timer cadence.  Preserve the
               historical "1"/empty all-directions mode while accepting an
               exact direction name as a low-overhead filter. */
            bool selected = trace_filter[0] == '\0'
                || strcmp(trace_filter, "1") == 0
                || strcmp(trace_filter, direction) == 0;
            if (!selected) {
                JS_FreeCString(context, direction);
                return JS_UNDEFINED;
            }
            if (JS_IsString(argv[1])) {
                size_t length = 0;
                const char *message = JS_ToCStringLen(
                    context, &length, argv[1]);
                if (message != NULL) {
                    const size_t trace_limit = 16u * 1024u;
                    size_t shown = length < trace_limit
                        ? length : trace_limit;
                    fprintf(stderr,
                            "tilefinch-worker-message: direction=%s "
                            "type=string bytes=%zu shown=%zu\n",
                            direction, length, shown);
                    if (shown != 0)
                        (void) fwrite(message, 1u, shown, stderr);
                    fprintf(stderr,
                            "\ntilefinch-worker-message-end: truncated=%d\n",
                            length > shown ? 1 : 0);
                    JS_FreeCString(context, message);
                } else {
                    JSValue exception = JS_GetException(context);
                    JS_FreeValue(context, exception);
                }
            } else {
                /* Validation-only tracing must expose the structured-clone
                   payload, not merely its type: challenge workers commonly
                   return small records whose field values identify the first
                   standards mismatch.  Keep this bounded and fail-soft; a
                   hostile toJSON/getter must never perturb ordinary builds. */
                JSValue json = JS_JSONStringify(
                    context, argv[1], JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(json) && JS_IsString(json)) {
                    size_t length = 0;
                    const char *message = JS_ToCStringLen(
                        context, &length, json);
                    if (message != NULL) {
                        const size_t trace_limit = 16u * 1024u;
                        size_t shown = length < trace_limit
                            ? length : trace_limit;
                        fprintf(stderr,
                                "tilefinch-worker-message: direction=%s "
                                "type=json bytes=%zu shown=%zu\n",
                                direction, length, shown);
                        if (shown != 0)
                            (void) fwrite(message, 1u, shown, stderr);
                        fprintf(stderr,
                                "\ntilefinch-worker-message-end: "
                                "truncated=%d\n",
                                length > shown ? 1 : 0);
                        JS_FreeCString(context, message);
                    } else {
                        JSValue exception = JS_GetException(context);
                        JS_FreeValue(context, exception);
                    }
                } else {
                    if (JS_IsException(json)) {
                        JSValue exception = JS_GetException(context);
                        JS_FreeValue(context, exception);
                    }
                    fprintf(stderr,
                            "tilefinch-worker-message: direction=%s "
                            "type=non-string\n",
                            direction);
                }
                JS_FreeValue(context, json);
            }
            JS_FreeCString(context, direction);
        } else {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
        }
    }
#else
    (void) context;
    (void) argc;
    (void) argv;
#endif
    return JS_UNDEFINED;
}

#if defined(CONFIG_PROPERTY_FAULT_TRACE)
static void runtime_configure_property_fault_trace(JSRuntime *runtime)
{
    const char *trace_value = tilefinch_trace_js_property_faults_value();
    uint32_t limit = 0u;
    if (trace_value != NULL && trace_value[0] != '\0'
        && strcmp(trace_value, "0") != 0) {
        char *end = NULL;
        unsigned long requested = strtoul(trace_value, &end, 10);
        limit = 32u;
        if (end != trace_value && *end == '\0') {
            if (requested < 1u)
                requested = 1u;
            if (requested > 128u)
                requested = 128u;
            limit = (uint32_t) requested;
        }
    }
    JS_SetPropertyFaultTraceLimit(runtime, limit);
}
#endif

/* The realm's native entry points, in install order. Install order is the
   global's own-property order, so rows are appended where the native
   belongs in that order, not grouped by owner. Flags carry each native's
   descriptor policy (js_runtime_internal.h); a native without flags is
   frozen (neither writable nor configurable), which is also why the
   bootstrap's protected-hook list leaves natives out. */
static const JsRtNative runtime_entry_natives[] = {
    { "__tilefinchEnsureShadowBootstrap",
      js_runtime_ensure_shadow_bootstrap, 0, 0 },
    { "__tilefinchEnsureCanvasBootstrap",
      js_runtime_ensure_canvas_bootstrap, 0, JS_RT_NATIVE_UNPROFILED },
    { "__tilefinchEnsureMotionBootstrap",
      js_runtime_ensure_motion_bootstrap, 0, JS_RT_NATIVE_UNPROFILED },
    { "__tilefinchEnsureWebGLBootstrap",
      js_runtime_ensure_webgl_bootstrap, 0, JS_RT_NATIVE_UNPROFILED },
};

static const JsRtNative runtime_natives_first[] = {
    { "__tilefinchCssSupports", js_css_supports, 2, 0 },
    { "__tilefinchFontShorthandValid", js_font_shorthand_valid, 1, 0 },
    { "__tilefinchCompileInlineEventHandler",
      js_compile_inline_event_handler, 1, 0 },
    { "__tilefinchStylesheetHasMotionKeyframes",
      js_stylesheet_has_motion_keyframes, 0, 0 },
    { "__tilefinchWasmAvailable", js_wasm_available, 0, 0 },
    { "__tilefinchWasmInstantiate", js_wasm_instantiate, 2, 0 },
    { "__tilefinchWasmCompile", js_wasm_compile, 1, 0 },
    { "__tilefinchWasmValidate", js_wasm_validate, 1, 0 },
    { "__tilefinchWasmModuleInfo", js_wasm_module_info, 3, 0 },
    { "__tilefinchWasmMemoryGrow", js_wasm_memory_grow, 2, 0 },
    { "__tilefinchWasmGlobalGet", js_wasm_global_get, 2, 0 },
    { "__tilefinchWasmGlobalSet", js_wasm_global_set, 3, 0 },
    { "__tilefinchWasmDetachBuffer", js_wasm_detach_buffer, 1, 0 },
    { "__tilefinchWasmSnapshotSource", js_wasm_snapshot_source, 1, 0 },
    { "__tilefinchCompressionRun",
      js_compression_run, 3, JS_RT_NATIVE_COMPRESSION_STREAMS },
    { "__tilefinchBody", js_dom_body, 0, 0 },
    { "__tilefinchDocumentElement", js_dom_document_element, 0, 0 },
    { "__tilefinchQuery", js_dom_query, 1, 0 },
    { "__tilefinchQueryAll", js_dom_query_all, 2, 0 },
    { "__tilefinchRegisterShadowRoot", js_dom_register_shadow_root, 1, 0 },
    { "__tilefinchConstructedSheetText",
      js_dom_constructed_sheet_text, 2, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchSheetRevision",
      js_dom_sheet_revision, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchCssStatementEnds",
      js_css_statement_ends, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchSetAdoptedSheets",
      js_dom_set_adopted_sheets, 2, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchMatches", js_dom_matches, 2, 0 },
    { "__tilefinchClosest", js_dom_closest, 2, 0 },
    { "__tilefinchMakeFastGetter", js_dom_make_fast_getter, 3, 0 },
    { "__tilefinchMakeFastMethod", js_dom_make_fast_method, 4, 0 },
    { "__tilefinchRelation", js_dom_relation, 2, 0 },
    { "__tilefinchComparePosition", js_dom_compare_position, 2, 0 },
    { "__tilefinchIsAncestor", js_dom_is_ancestor, 2, 0 },
    { "__tilefinchSelectorsTestAttribute",
      js_dom_selectors_test_attribute, 1, 0 },
    { "__tilefinchNextAncestorEntry", js_dom_next_ancestor_entry, 3, 0 },
    { "__tilefinchIsConnected", js_dom_is_connected, 1, 0 },
    { "__tilefinchRootNode", js_dom_root_node, 2, 0 },
    { "__tilefinchNodeOwner", js_dom_node_owner, 2, 0 },
    { "__tilefinchChildren", js_dom_children, 1, 0 },
    { "__tilefinchChildNodes", js_dom_child_nodes, 1, 0 },
    { "__tilefinchDocumentChildNodes", js_dom_document_child_nodes, 0, 0 },
    { "__tilefinchTagName", js_dom_tag_name, 1, 0 },
    { "__tilefinchNodeIdentity", js_dom_node_identity, 3, 0 },
    { "__tilefinchParseColor", js_dom_parse_color, 1, 0 },
    { "__tilefinchCommitCanvasSurface", js_canvas_commit_surface, 8, 0 },
    { "__tilefinchCanvasImageSource", js_canvas_image_source, 1, 0 },
    { "__tilefinchWebGLRender", js_webgl_render, 8, 0 },
    { "__tilefinchWebGLIndexMaximum", js_webgl_index_maximum, 4, 0 },
    { "__tilefinchWebGLFiniteFloat32", js_webgl_finite_float32, 2, 0 },
    { "__tilefinchWebGLCombineMatrix4", js_webgl_combine_matrix4, 3, 0 },
    { "__tilefinchWebGLReadPixels", js_webgl_read_pixels, 7, 0 },
    { "__tilefinchWebGLSnapshot", js_webgl_snapshot, 1, 0 },
    { "__tilefinchWebGLReleaseSurface", js_webgl_release_surface, 1, 0 },
    { "__tilefinchSetFullscreen", js_dom_set_fullscreen, 2, 0 },
    { "__tilefinchPageControlsNotice", js_dom_page_controls_notice, 1, 0 },
    { "__tilefinchCanvasRasterRect", js_canvas_raster_rect, 13, 0 },
    { "__tilefinchCanvasRasterRectBatch", js_canvas_raster_rect_batch, 4, 0 },
    { "__tilefinchCanvasMeasureText", js_canvas_measure_text, 5, 0 },
    { "__tilefinchCanvasRasterPath", js_canvas_raster_path, 21, 0 },
    { "__tilefinchCanvasRasterPaintBatch",
      js_canvas_raster_paint_batch, 4, 0 },
    { "__tilefinchCanvasRasterImage", js_canvas_raster_image, 20, 0 },
    { "__tilefinchCanvasRasterImageBatch",
      js_canvas_raster_image_batch, 5, 0 },
    { "__tilefinchNodeType", js_dom_node_type, 1, 0 },
    { "__tilefinchDescendants", js_dom_descendants, 2, 0 },
    { "__tilefinchTraverse", js_dom_traverse, 4, 0 },
    { "__tilefinchObserveParserInsertions",
      js_dom_observe_parser_insertions, 1, 0 },
    { "__tilefinchNamedElementIds", js_dom_named_element_ids, 0, 0 },
    { "__tilefinchFrameHandles", js_dom_frame_handles, 1, 0 },
    { "__tilefinchContent", js_dom_content, 1, 0 },
    { "__tilefinchClone", js_dom_clone, 2, 0 },
    { "__tilefinchAttributes", js_dom_attributes, 1, 0 },
    { "__tilefinchCreate", js_dom_create, 2, 0 },
    { "__tilefinchCreateFragment", js_dom_create_fragment, 0, 0 },
    { "__tilefinchCreateText", js_dom_create_text, 1, 0 },
    { "__tilefinchCreateComment", js_dom_create_comment, 1, 0 },
    { "__tilefinchSetCustomState", js_dom_set_custom_state, 2, 0 },
    { "__tilefinchGetCustomState", js_dom_get_custom_state, 1, 0 },
    { "__tilefinchRetainNodeWrapper", js_dom_retain_node_wrapper, 1, 0 },
    { "__tilefinchNoteEventState", js_dom_note_event_state, 3, 0 },
    { "__tilefinchGrowEventTargets", js_dom_grow_event_targets, 1, 0 },
    { "__tilefinchGetText", js_dom_get_text, 1, 0 },
    { "__tilefinchGetTextPrefix", js_dom_get_text_prefix, 3, 0 },
    { "__tilefinchGetStyleAttributePrefix",
      js_dom_get_style_attribute_prefix, 2, 0 },
    { "__tilefinchSetText", js_dom_set_text, 2, 0 },
    { "__tilefinchGetInnerHTML", js_dom_get_inner_html, 1, 0 },
    { "__tilefinchSetInnerHTML", js_dom_set_inner_html, 2, 0 },
    { "__tilefinchGetAttribute", js_dom_get_attribute, 2, 0 },
    { "__tilefinchImageProperty", js_dom_image_property, 2, 0 },
};

/* js_fetch_cors_install() runs between the last two tables. */
static const JsRtNative runtime_natives_second[] = {
    { "__tilefinchSetAttribute", js_dom_set_attribute, 3, 0 },
    { "__tilefinchSetControlValue", js_dom_set_control_value, 2, 0 },
    { "__tilefinchParserFormOwner", js_dom_parser_form_owner, 1, 0 },
    { "__tilefinchHasParserFormOwners", js_dom_has_parser_form_owners, 0, 0 },
    { "__tilefinchDomVersion", js_dom_version, 0, 0 },
    { "__tilefinchRemoveAttribute", js_dom_remove_attribute, 2, 0 },
    { "__tilefinchStyleGet", js_style_get, 2, 0 },
    { "__tilefinchComputedStyleGet", js_computed_style_get, 2, 0 },
    { "__tilefinchComputedStyleRead", js_computed_style_read, 3, 0 },
    { "__tilefinchRenderedText", js_dom_rendered_text, 1, 0 },
    { "__tilefinchComputedStyleSupport", js_computed_style_support, 1, 0 },
    { "__tilefinchTransitionSnapshot", js_transition_snapshot, 2, 0 },
    { "__tilefinchTimingFunction", js_timing_function, 1, 0 },
    { "__tilefinchAttributeChangeMayAffectHas",
      js_attribute_change_may_affect_has, 3, 0 },
    { "__tilefinchStyleReach", js_style_reach, 2, 0 },
    { "__tilefinchStyleSet", js_style_set, 3, 0 },
    { "__tilefinchAppend", js_dom_append, 2, 0 },
    { "__tilefinchPrepareDynamicSubtree",
      js_dom_prepare_dynamic_subtree, 1, 0 },
    { "__tilefinchAppendMany", js_dom_append_many, 2, 0 },
    { "__tilefinchInsertBefore", js_dom_insert_before, 3, 0 },
    { "__tilefinchRemove", js_dom_remove, 1, 0 },
    { "__tilefinchRecordEvent", js_dom_record_event, 0, 0 },
    { "__tilefinchRecordEventHandler", js_dom_record_event_handler, 0, 0 },
    { "__tilefinchStorageGet", js_storage_get, 2, 0 },
    { "__tilefinchStorageAvailable", js_storage_available, 0, 0 },
    { "__tilefinchStorageEstimate", js_storage_estimate, 0, 0 },
    { "__tilefinchHeapMemorySnapshot", js_heap_memory_snapshot, 0, 0 },
    { "__tilefinchStorageSet", js_storage_set, 3, 0 },
    { "__tilefinchStorageLength", js_storage_length, 1, 0 },
    { "__tilefinchStorageKey", js_storage_key, 2, 0 },
    { "__tilefinchStorageRemove", js_storage_remove, 2, 0 },
    { "__tilefinchStorageClear", js_storage_clear, 1, 0 },
    { "__tilefinchCookieGet", js_cookie_get, 0, 0 },
    { "__tilefinchCookieSet", js_cookie_set, 1, 0 },
    { "__tilefinchConsoleLog", js_console_log, 1, 0 },
    { "__tilefinchResolveURL", js_resolve_url, 2, 0 },
    { "__tilefinchDocumentBaseURI", js_document_base_uri, 0, 0 },
    { "__tilefinchCurrentDocumentURL", js_current_document_url, 0, 0 },
    { "__tilefinchSecureContext", js_secure_context, 0, 0 },
    { "__tilefinchDocumentDomainValid", js_document_domain_valid, 2, 0 },
    { "__tilefinchDocumentURLRevision", js_document_url_revision, 0, 0 },
    { "__tilefinchSetDocumentURL", js_set_document_url, 1, 0 },
};

static const JsRtNative runtime_natives_third[] = {
    { "__tilefinchScriptForceAsync", js_script_force_async, 1, 0 },
    { "__tilefinchScriptAsyncAssigned", js_script_async_assigned, 1, 0 },
    { "__tilefinchElementNonce", js_element_nonce, 2, 0 },
    { "__tilefinchCryptoRandomFill", js_crypto_random_fill, 1, 0 },
    { "__tilefinchPrefersDark", js_prefers_dark_color_scheme, 0, 0 },
    { "__tilefinchMediaMatches", js_media_matches, 4, 0 },
    { "__tilefinchDecodeUtf8Valid",
      js_utf8_decode_valid, 2, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchEncodingName", js_encoding_name, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchDecodeSingleByte",
      js_decode_single_byte, 3, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchEncodeUtf8", js_utf8_encode, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchDetachArrayBuffer", js_detach_array_buffer, 1, 0 },
    { "__tilefinchIsProxy", js_is_proxy, 1, 0 },
    { "__tilefinchCryptoDigestSHA256", js_crypto_digest_sha256, 1, 0 },
    { "__tilefinchVerifyResourceIntegrity",
      js_resource_integrity_verify, 2, 0 },
    { "__tilefinchBase64EncodeBytes", js_base64_encode_bytes, 1, 0 },
    { "__tilefinchBase64EncodeLatin1", js_base64_encode_latin1, 1, 0 },
    { "__tilefinchBase64DecodeString", js_base64_decode_string, 1, 0 },
    { "__tilefinchPerformanceNow", js_performance_now, 1, 0 },
    { "__tilefinchPerformanceSample", js_performance_sample, 1, 0 },
    { "__tilefinchSchedulerNow", js_scheduler_now, 0, 0 },
    { "__tilefinchCallbackCheckpoint", js_callback_checkpoint, 0, 0 },
    { "__tilefinchQueueCheckpointContinuation",
      js_queue_checkpoint_continuation, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchEnqueueMicrotask",
      js_enqueue_microtask, 1, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchDateNow", js_date_now, 1, 0 },
    { "__tilefinchPostMessage", js_post_message, 3, 0 },
    { "__tilefinchRequestNavigation", js_request_navigation, 2, 0 },
    { "__tilefinchUserActivationState",
      js_user_activation_state, 0, JS_RT_NATIVE_TRANSIENT },
    { "__tilefinchRequestMedia", js_request_media, 4, 0 },
    { "__tilefinchRequestScroll", js_request_scroll, 1, 0 },
    { "__tilefinchGeometry", js_element_geometry, 1, 0 },
    { "__tilefinchSetElementScroll", js_set_element_scroll, 3, 0 },
    { "__tilefinchCreateFrameEval", js_create_frame_eval, 2, 0 },
    { "__tilefinchFrameScope", js_frame_scope, 1, 0 },
};

typedef struct {
    const char *property;
    const char *function_name;
    JSCFunction *function;
    uint8_t arguments;
} RuntimeNamedMethod;

/* Element and document query methods the bootstrap moves onto their
   prototypes; they keep their Web name, unlike the bridge natives. */
static const RuntimeNamedMethod runtime_named_methods[] = {
    { "__tilefinchDocumentQuerySelector",
      "querySelector", js_dom_query_selector_method, 1 },
    { "__tilefinchDocumentQuerySelectorAll",
      "querySelectorAll", js_dom_query_selector_all_method, 1 },
    { "__tilefinchDocumentGetElementById",
      "getElementById", js_dom_get_element_by_id_method, 1 },
    { "__tilefinchElementQuerySelector",
      "querySelector", js_dom_query_selector_method, 1 },
    { "__tilefinchElementQuerySelectorAll",
      "querySelectorAll", js_dom_query_selector_all_method, 1 },
    { "__tilefinchDocumentFragmentQuerySelector",
      "querySelector", js_dom_query_selector_method, 1 },
    { "__tilefinchDocumentFragmentQuerySelectorAll",
      "querySelectorAll", js_dom_query_selector_all_method, 1 },
};

static bool runtime_install_named_methods(JSContext *context, JSValue global)
{
    for (size_t i = 0; i < sizeof(runtime_named_methods)
                                / sizeof(runtime_named_methods[0]); i++) {
        const RuntimeNamedMethod *method = &runtime_named_methods[i];
        if (!install_named_function(context, global, method->property,
                                    method->function_name, method->function,
                                    method->arguments))
            return false;
    }
    return true;
}

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
static const JsRtNative runtime_validation_natives[] = {
#if defined(__PSP__) && defined(TILEFINCH_PSP_VALIDATION_LOG)
    { "__tilefinchValidationThreadCpuUs", js_validation_thread_cpu_us, 0, 0 },
    { "__tilefinchValidationPhase", js_validation_phase, 4, 0 },
    { "__tilefinchValidationRenderHistory",
      js_validation_render_history, 1, 0 },
#endif
    { "__tilefinchValidationHashBytes", js_validation_hash_bytes, 3, 0 },
    { "__tilefinchReleaseNodeWrapper", js_dom_release_node_wrapper, 2, 0 },
};
#endif

/* What the deterministic replay derivation fixed for one realm before
   anything was allocated (runtime_create_derive_replay). */
typedef struct {
    bool deterministic;
    uint64_t seed;
    uint64_t clock_origin_ms;
} RuntimeReplayDerivation;

/* Startup phase stamps for the TILEFINCH_TRACE_JS_STARTUP summary. */
typedef struct {
    uint64_t started_ns;
    uint64_t runtime_ready_ns;
    uint64_t context_ready_ns;
    uint64_t native_ready_ns;
    uint64_t bootstrap_total_us;
    uint64_t bootstrap_compile_us;
} RuntimeStartupClock;

/* Rejects options no realm can be created from; clears `result` once the
   arguments can be trusted with it. */
static bool runtime_create_check_options(
    const PocDocument *document, const Budget *budget,
    const ScriptRuntimeOptions *options, ScriptResult *result)
{
    if (options == NULL
        || (options->document_scope != SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
            && options->document_scope
                   != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME)) {
        return false;
    }
    const ViewportContext *viewport = &options->viewport;
    if (document == NULL || budget == NULL || result == NULL
        || viewport->css_width <= 0 || viewport->css_height <= 0
        || viewport->device_width <= 0 || viewport->device_height <= 0)
        return false;
    memset(result, 0, sizeof(*result));
#if !defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    if (options->dynamic_code_disabled) {
        snprintf(result->error, sizeof(result->error), "%s",
                 "QuickJS build cannot enforce CSP dynamic-code policy");
        return false;
    }
#endif
    return true;
}

static bool runtime_create_derive_replay(const char *document_url,
                                         ScriptResult *result,
                                         RuntimeReplayDerivation *replay)
{
    /* Freeze and validate every input to the replay derivation before any
       realm allocation or entropy state is created.  In particular, do not
       let the bounded host buffer silently turn two distinct overlong URLs
       into the same replay identity. */
    const uint64_t replay_seed = deterministic_replay.seed;
    uint64_t replay_clock_origin_ms = 0;
    replay->deterministic = deterministic_replay.enabled;
    replay->seed = replay_seed;
    replay->clock_origin_ms = 0;
    if (replay->deterministic) {
        size_t document_url_length = 0;
        if (!replay_document_url_exact(document_url, &document_url_length)
            || document_url_length
                   >= sizeof(((ScriptRuntime *) 0)->document_url)) {
            snprintf(result->error, sizeof(result->error), "%s",
                     "deterministic replay requires an exact canonical "
                     "HTTP(S) document URL");
            return false;
        }
        const uint64_t maximum_date_ms = UINT64_C(8640000000000000);
        if (fetch_trace_clock_origin_ms(&replay_clock_origin_ms)) {
            if (replay_clock_origin_ms > maximum_date_ms) {
                snprintf(result->error, sizeof(result->error), "%s",
                         "deterministic replay clock origin exceeds the "
                         "ECMAScript Date range");
                return false;
            }
        } else {
            /* Keep a useful contemporary epoch while making every seed safe
               without unsigned addition wrap.  The bounded seed component is
               part of the fallback policy, not an entropy-stream input. */
            replay_clock_origin_ms = UINT64_C(1700000000000)
                + replay_seed % UINT64_C(31557600000);
        }
        replay->clock_origin_ms = replay_clock_origin_ms;
    }
    return true;
}

/* The one unwind for a creation that fails before
   runtime_create_bind_bridge(): frees the QuickJS context, runtime and
   allocator pool that exist, then the host. From the bridge binding on,
   script_runtime_destroy() owns the host instead. */
static ScriptRuntime *runtime_create_abandon(ScriptRuntime *host)
{
    /* No script has run, so the profile holds no atoms to free. */
    budget_free(host->budget, host->profile);
    if (host->context != NULL) JS_FreeContext(host->context);
    if (host->runtime != NULL) JS_FreeRuntime(host->runtime);
    if (host->quickjs_pool != NULL)
        (void) budget_quickjs_pool_destroy(host->quickjs_pool);
    budget_free(host->budget, host);
    return NULL;
}

/* Allocates the host and sets every field creation reads before the
   engine exists: undefined callbacks, the realm's options and clamped
   script quotas, and its document and top-level URLs. */
static ScriptRuntime *runtime_create_host(
    PocDocument *document, Budget *budget, unsigned timeout_ms,
    const char *document_url, const ScriptRuntimeOptions *options,
    ScriptResult *result)
{
    ScriptRuntime *host = budget_calloc(budget, 1, sizeof(*host));
    if (host == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not allocate script runtime host");
        return NULL;
    }
    host->budget = budget;
    host->host_network_queue_stats = JS_UNDEFINED;
    host->host_indexed_db_stats = JS_UNDEFINED;
    host->motion_style_flushed = JS_UNDEFINED;
    host->media_update = JS_UNDEFINED;
    host->document_all_cache = JS_UNDEFINED;
    host->opfs_storage_manager = JS_UNDEFINED;
    host->game_audio_host_suspend = JS_UNDEFINED;
    host->game_audio_host_complete = JS_UNDEFINED;
#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
    host->game_audio_internal_slots = options->game_audio_internal_slots;
#else
    host->game_audio_internal_slots = true;
#endif
    host->function_to_string = JS_UNDEFINED;
    host->host_global = JS_UNDEFINED;
#ifdef JS_RT_XHR_DIAGNOSTICS
    host->private_outcome_classifier = JS_UNDEFINED;
    host->xhr_diagnostics_install = JS_UNDEFINED;
#endif
    for (size_t callback = 0; callback < SCRIPT_HOST_CALLBACK_COUNT;
         callback++) {
        host->host_callbacks[callback] = JS_UNDEFINED;
    }
    host->bridge.current_script = JS_NULL;
    host->page_visibility_desired = true;
    host->document = document;
    host->document_scope = options->document_scope;
    host->timeout_ms = timeout_ms;
    host->session = options->session;
    host->watchdog.result = &host->result;
    host->bridge.execution_policy = options->execution_policy;
    host->bridge.dynamic_code_allowed = !options->dynamic_code_disabled;
    host->bridge.allow_test_network_primitive_overrides =
        options->allow_test_network_primitive_overrides;
#ifdef JS_RT_TEST_INSTRUMENTS
    host->test_probes = !options->omit_test_probes;
#endif
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
        return runtime_create_abandon(host);
    }
    snprintf(host->top_level_url, sizeof(host->top_level_url), "%s",
             top_level_url);
    return host;
}

/* The QuickJS allocator pool, runtime (memory and stack limits, watchdog,
   hooks) and context, and the WebAssembly bindings. On failure the caller
   abandons the host (runtime_create_abandon). */
static bool runtime_create_engine(ScriptRuntime *host, Budget *budget,
                                  size_t js_memory_limit,
                                  ScriptResult *result,
                                  RuntimeStartupClock *clock)
{
    host->quickjs_pool = budget_quickjs_pool_create(budget);

    if (host->quickjs_pool == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not create QuickJS allocator pool");
        return false;
    }
    host->runtime = JS_NewRuntime2(budget_quickjs_pool_allocator(),
                                  host->quickjs_pool);
    if (host->runtime == NULL) {
        snprintf(result->error, sizeof(result->error),
                 "could not create QuickJS runtime");
        return false;
    }
    clock->runtime_ready_ns = js_rt_monotonic_time_ns();
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
    const char *stack_kib_value = tilefinch_lab_getenv("TILEFINCH_JS_STACK_KB");
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
    if (script_profile_enabled()) {
        /* Diagnostic only: a refused allocation leaves profiling off. */
        host->profile = budget_calloc(budget, 1, sizeof(*host->profile));
    }
    JS_SetGCHook(host->runtime, script_runtime_gc_hook, host);
    JS_SetGCPacingHook(host->runtime, js_rt_gc_pacing, host);
    JS_SetLazyCompileHook(host->runtime, script_runtime_lazy_compile_hook,
                          host);
    /* The page runtime's thread owns the script split (script_split.h).
       TILEFINCH_SCRIPT_SPLIT=0 leaves it off and =1 keeps the timed kinds
       without the poll sample, to measure its own cost in one binary. */
    {
        const char *split_mode = tilefinch_lab_getenv("TILEFINCH_SCRIPT_SPLIT");
        bool split_off = split_mode != NULL && strcmp(split_mode, "0") == 0;
        script_split_set_sampling(split_mode == NULL
                                  || strcmp(split_mode, "1") != 0);
        if (!split_off) script_split_set_enabled(true);
    }
    host->promise_rejection_state.result = &host->result;
    JS_SetHostPromiseRejectionTracker(host->runtime,
                                      promise_rejection_tracker,
                                      &host->promise_rejection_state);

    host->context = JS_NewContext(host->runtime);
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    if (host->context != NULL
        && getenv("TILEFINCH_JS_TRAP_ALLOC_SIZE") != NULL) {
        budget_quickjs_pool_set_stack_dump_hook(
            js_rt_dump_interpreter_stack, host);
    }
#endif
    if (host->context == NULL) {
        snprintf(result->error, sizeof(result->error), "could not create QuickJS context");
        return false;
    }
    if (!js_wasm_runtime_init(host->runtime)) {
        snprintf(result->error, sizeof(result->error),
                 "could not initialize WebAssembly bindings");
        return false;
    }
    clock->context_ready_ns = js_rt_monotonic_time_ns();
    return true;
}

/* Binds the bridge to the document, page Budget, session and options;
   allocates the DOM handle table; fixes the realm's clocks (performance
   origin, replay) and entropy, and makes the bridge the context opaque.
   Called once the engine exists: on failure the caller destroys the host
   with script_runtime_destroy(). */
static bool runtime_create_bind_bridge(
    ScriptRuntime *host, PocDocument *document, Budget *budget,
    unsigned timeout_ms, const ScriptRuntimeOptions *options,
    const RuntimeReplayDerivation *replay, ScriptResult *result,
    uint64_t *time_origin_ms)
{
    const bool deterministic = replay->deterministic;
    const uint64_t replay_seed = replay->seed;
    const uint64_t replay_clock_origin_ms = replay->clock_origin_ms;
    host->bridge.document = document;
    host->bridge.budget = budget;
    host->bridge.result = &host->result;
    host->bridge.relayout_dirty = &host->relayout_dirty;
    host->bridge.session = options->session;
    host->bridge.fonts = options->fonts;
    host->bridge.document_url = host->document_url;
    host->bridge.top_level_url = host->top_level_url;
    host->bridge.document_base_dirty = true;
    host->bridge.document_url_revision = 1u;
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
        return false;
    }
    if (!js_rt_bridge_node_table_init(
            &host->bridge,
            host->bridge.execution_policy.dom_handle_slot_capacity)) {
        snprintf(result->error, sizeof(result->error),
                 "could not allocate the DOM node handle table");
        return false;
    }
    host->result.dom_handle_slot_capacity = host->bridge.node_capacity;
    snprintf(host->bridge.referrer_policy,
             sizeof(host->bridge.referrer_policy), "%s",
             options->referrer_policy == NULL
               ? "" : options->referrer_policy);
    host->bridge.fetch_timeout_ms = timeout_ms;
    host->bridge.viewport_width = options->viewport.css_width;
    host->bridge.viewport_height = options->viewport.css_height;
    host->bridge.host = host;
    if (!js_webgl_realm_epoch_advance(&host->bridge)) {
        snprintf(result->error, sizeof(result->error),
                 "could not assign WebGL realm identity");
        return false;
    }
    uint64_t performance_created_ns = js_rt_monotonic_time_ns();
    host->bridge.performance_origin_ns = performance_created_ns;
    uint64_t performance_time_origin_ms = deterministic
        ? replay_clock_origin_ms : wall_time_ms();
    if (!deterministic && options->navigation_started_us != 0
        && options->navigation_started_us <= performance_created_ns / 1000u) {
        uint64_t elapsed_us = performance_created_ns / 1000u
            - options->navigation_started_us;
        /* Reject nonsensical or stale embedding anchors rather than exposing
           a clock whose epoch and monotonic halves no longer correspond. */
        if (elapsed_us <= UINT64_C(3600000000)) {
            host->bridge.performance_origin_ns =
                options->navigation_started_us * UINT64_C(1000);
            uint64_t elapsed_ms = elapsed_us / UINT64_C(1000);
            performance_time_origin_ms = performance_time_origin_ms >= elapsed_ms
                ? performance_time_origin_ms - elapsed_ms : 0;
        }
    }
#if !defined(__PSP__)
    unsigned performance_origin_backdate_ms =
        tilefinch_test_faults()
            ->runtime_performance_origin_backdate_ms;
    tilefinch_test_faults()->runtime_performance_origin_backdate_ms = 0;
    uint64_t performance_origin_backdate_ns =
        (uint64_t) performance_origin_backdate_ms * UINT64_C(1000000);
    if (!deterministic && performance_origin_backdate_ms != 0
        && host->bridge.performance_origin_ns
               >= performance_origin_backdate_ns
        && performance_time_origin_ms >= performance_origin_backdate_ms) {
        host->bridge.performance_origin_ns -= performance_origin_backdate_ns;
        performance_time_origin_ms -= performance_origin_backdate_ms;
    }
#endif
    host->bridge.deterministic_entropy = deterministic;
    host->bridge.deterministic_clock = deterministic;
    host->bridge.fixed_wall_clock = deterministic && fetch_trace_fixed_wall_clock();
    host->bridge.wall_clock_timers =
        !deterministic && wall_clock_timers_enabled;
    host->bridge.replay_seed = replay_seed;
    host->bridge.clock_origin_ms = replay_clock_origin_ms;
    host->bridge.entropy_state = host->bridge.deterministic_entropy
        ? script_runtime_deterministic_entropy_state_v1(
              replay_seed, host->document_url,
              options->document_scope)
        : 0;
    JS_SetContextOpaque(host->context, &host->bridge);
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    JS_SetHostGetCodeForEval(
        host->context, js_runtime_host_get_code_for_eval, NULL);
#endif
    *time_origin_ms = performance_time_origin_ms;
    return true;
}

/* The realm's globals before any bootstrap runs: the native tables, the
   host facts the bootstrap reads (__tilefinch* values), the deterministic
   Math.random, and `document` with its title and currentScript. The
   document object is consumed when it becomes the global. */
static bool runtime_install_globals(ScriptRuntime *host, JSValue global,
                                    JSValue *js_document_slot,
                                    const ScriptRuntimeOptions *options,
                                    uint64_t performance_time_origin_ms)
{
    PocDocument *document = host->document;
    JSValue js_document = *js_document_slot;
    const int css_viewport_width = options->viewport.css_width;
    const int css_viewport_height = options->viewport.css_height;
    const int device_width = options->viewport.device_width;
    const int device_height = options->viewport.device_height;
    bool setup_ok = true;
    if (setup_ok)
        setup_ok = JS_RT_INSTALL_NATIVES(
            host->context, global, runtime_entry_natives);
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
        const char *document_referrer = options->document_referrer == NULL
            ? "" : options->document_referrer;
        if (strlen(document_referrer) >= TILEFINCH_URL_SERIALIZED_LIMIT) {
            setup_ok = false;
        } else {
            setup_ok = JS_SetPropertyStr(
                host->context, global, "__tilefinchDocumentReferrer",
                JS_NewString(host->context, document_referrer)) >= 0;
        }
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchPerformanceTimeOrigin",
            JS_NewFloat64(
                host->context, (double) performance_time_origin_ms)) >= 0;
    }
    if (setup_ok) {
        /* The realm reports its top-level document's identity, the same one
           fetch_prepare_page_request() puts on that document's requests.
           Worker navigators mirror this value. */
        const char *site_user_agent =
            site_identity_user_agent_override(host->top_level_url);
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchBrowserUserAgent",
            JS_NewString(host->context, site_user_agent != NULL
                ? site_user_agent : TILEFINCH_BROWSER_USER_AGENT)) >= 0;
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchBrowserBrand",
            JS_NewString(host->context, TILEFINCH_BROWSER_BRAND)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchBrowserBrandVersion",
                JS_NewString(host->context,
                             TILEFINCH_BROWSER_BRAND_VERSION)) >= 0
            && JS_SetPropertyStr(
                host->context, global, "__tilefinchBrowserFullVersion",
                JS_NewString(host->context,
                             TILEFINCH_BROWSER_FULL_VERSION)) >= 0;
    }
#if TILEFINCH_LAB_ENVIRONMENT
    /* Lab knob; the bootstrap reads an absent flag as false. */
    if (setup_ok
        && tilefinch_lab_getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") != NULL) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchDiagnosticMobileSafari",
            JS_TRUE) >= 0;
    }
#endif
    if (setup_ok) {
        setup_ok = js_lazy_webpack_install(host, global);
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
#ifdef JS_RT_TEST_INSTRUMENTS
    /* Host and validation realms only: the bootstrap then publishes its
       test-only probes. hardening.js deletes this flag; lazy modules get it
       back from runtime_load_lazy_bootstrap(). */
    if (setup_ok && host->test_probes) {
        setup_ok = JS_SetPropertyStr(
            host->context, global, "__tilefinchTestProbes", JS_TRUE) >= 0;
    }
#endif
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
        /* Each table installs in row order; the module tables sit at
           their historical places, so the global's own-property order
           is unchanged. */
        setup_ok = JS_RT_INSTALL_NATIVES(
                       host->context, global, runtime_natives_first)
                   && JS_RT_INSTALL_NATIVES(
                       host->context, global, runtime_natives_second)
                   && js_fetch_cors_install(host->context, global)
                   && JS_RT_INSTALL_NATIVES(
                       host->context, global, runtime_natives_third)
                   && runtime_install_named_methods(host->context, global);
    }
    if (setup_ok) {
        setup_ok = JS_SetPropertyStr(host->context, js_document, "title",
            JS_NewString(host->context, document->title == NULL ? "" : document->title)) >= 0;
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
                           JS_UNDEFINED,
                           JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE) < 0) {
                /* No setter: author code cannot write the native execution
                   scope. Configurable so the bootstrap can move it to
                   Document.prototype, where WebIDL puts it (hardening.js). */
                setup_ok = false;
            }
            JS_FreeAtom(host->context, current_script_atom);
        }
        if (setup_ok) {
            int property_result = JS_SetPropertyStr(host->context, global, "document",
                                                    js_document);
            js_document = JS_UNDEFINED;
            setup_ok = property_result >= 0;
        }
    }

#if !defined(__PSP__) || defined(TILEFINCH_PSP_VALIDATION_LOG)
    if (setup_ok)
        setup_ok = JS_RT_INSTALL_NATIVES(
            host->context, global, runtime_validation_natives);
#endif
    *js_document_slot = js_document;
    return setup_ok;
}

/* One step of the bootstrap sequence: evaluate a bootstrap module,
   install a lazy module's placeholders, or define the host channel. */
typedef enum {
    RUNTIME_BOOTSTRAP_EVALUATE,
    RUNTIME_BOOTSTRAP_LAZY,
    RUNTIME_BOOTSTRAP_HOST_CHANNEL
} RuntimeBootstrapStepKind;

/* When a step runs: always, only in deterministic-replay realms, only for
   a potentially trustworthy non-opaque origin (OPFS), or only under
   TILEFINCH_TRACE_DPU. */
typedef enum {
    RUNTIME_BOOTSTRAP_ALWAYS,
    RUNTIME_BOOTSTRAP_IF_DETERMINISTIC_CLOCK,
    RUNTIME_BOOTSTRAP_IF_SECURE_ORIGIN,
    RUNTIME_BOOTSTRAP_IF_TRACE_DPU
} RuntimeBootstrapStepCondition;

typedef struct {
    uint8_t kind;
    uint8_t condition;
    /* Evaluate with the authored source rather than the bytecode when the
       heap is tight (runtime_evaluate_bootstrap_profiled). */
    bool prefer_source_on_tight_heap;
    uint8_t lazy;
    /* NULL in bytecode-only builds, which keep no authored copy; the
       length then comes from the bytecode. */
    const char *source;
    const size_t *source_length;
    const char *source_url;
    const BrowserBootstrapBytecode *bytecode;
} RuntimeBootstrapStep;

#if defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
#define RUNTIME_BOOTSTRAP_SOURCE(symbol) NULL, NULL
#else
#define RUNTIME_BOOTSTRAP_SOURCE(symbol) (symbol), &(symbol##_length)
#endif
#define RUNTIME_BOOTSTRAP_MODULE(condition, symbol, url, prefer_source)   \
    { RUNTIME_BOOTSTRAP_EVALUATE, (condition), (prefer_source), 0,       \
      RUNTIME_BOOTSTRAP_SOURCE(symbol), (url), &(symbol##_bytecode) }
#define RUNTIME_BOOTSTRAP_LAZY_MODULE(condition, feature)                 \
    { RUNTIME_BOOTSTRAP_LAZY, (condition), false, (feature),             \
      NULL, NULL, NULL, NULL }

/* The order is the realm's: each module may use what the earlier ones
   published, and the global's own-property order follows it. */
static const RuntimeBootstrapStep runtime_bootstrap_sequence[] = {
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_cssom, "<browser-cssom>", false),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_IF_DETERMINISTIC_CLOCK,
        browser_deterministic_clock_bootstrap,
        "<browser-deterministic-clock>", false),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap, "<browser-bootstrap>", true),
    /* frames.js only defines its installer; platform.js invokes it with
       private helpers, so it must be evaluated in between. */
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_frames, "<browser-frames>", false),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_platform, "<browser-platform>", false),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_CANVAS),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_custom_elements, "<browser-custom-elements>",
        false),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_STREAMS),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_GAME_AUDIO),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_WORKER),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_WASM),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_CAPABILITIES),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_INTL),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_TRAVERSAL),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_IF_SECURE_ORIGIN,
                                  LAZY_BOOTSTRAP_OPFS),
    { RUNTIME_BOOTSTRAP_HOST_CHANNEL, RUNTIME_BOOTSTRAP_ALWAYS, false, 0,
      NULL, NULL, NULL, NULL },
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_compat, "<browser-compat>", false),
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
    /* Diagnostic source only: no bytecode is generated for it. */
    { RUNTIME_BOOTSTRAP_EVALUATE, RUNTIME_BOOTSTRAP_IF_TRACE_DPU, false, 0,
      browser_dpu_diagnostic, &browser_dpu_diagnostic_length,
      "<dpu-diagnostic>", NULL },
#endif
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_scheduler, "<browser-scheduler>", false),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_IF_DETERMINISTIC_CLOCK,
        browser_deterministic_scheduler_clock_bootstrap,
        "<browser-deterministic-scheduler-clock>", false),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_MOTION),
    RUNTIME_BOOTSTRAP_LAZY_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
                                  LAZY_BOOTSTRAP_INDEXED_DB),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_IF_DETERMINISTIC_CLOCK,
        browser_deterministic_replay_surface_bootstrap,
        "<browser-deterministic-replay-surface>", false),
    RUNTIME_BOOTSTRAP_MODULE(RUNTIME_BOOTSTRAP_ALWAYS,
        browser_bootstrap_hardening, "<browser-hardening>", false),
};

#undef RUNTIME_BOOTSTRAP_LAZY_MODULE
#undef RUNTIME_BOOTSTRAP_MODULE
#undef RUNTIME_BOOTSTRAP_SOURCE

static bool runtime_bootstrap_step_applies(
    const ScriptRuntime *host, const RuntimeBootstrapStep *step)
{
    switch ((RuntimeBootstrapStepCondition) step->condition) {
    case RUNTIME_BOOTSTRAP_IF_DETERMINISTIC_CLOCK:
        return host->bridge.deterministic_clock;
    case RUNTIME_BOOTSTRAP_IF_SECURE_ORIGIN:
        return !host->bridge.opaque_origin
            && host->bridge.document_url != NULL
            && tilefinch_url_potentially_trustworthy(
                   host->bridge.document_url);
    case RUNTIME_BOOTSTRAP_IF_TRACE_DPU:
        return tilefinch_trace_dpu();
    case RUNTIME_BOOTSTRAP_ALWAYS:
    default:
        return true;
    }
}

/* Runs runtime_bootstrap_sequence in order, stopping at the first step
   that fails. */
static bool runtime_evaluate_bootstrap_sequence(
    ScriptRuntime *host, bool trace_startup, RuntimeStartupClock *clock)
{
    for (size_t i = 0; i < sizeof(runtime_bootstrap_sequence)
                               / sizeof(runtime_bootstrap_sequence[0]);
         i++) {
        const RuntimeBootstrapStep *step = &runtime_bootstrap_sequence[i];
        if (!runtime_bootstrap_step_applies(host, step)) continue;
        bool ok;
        switch ((RuntimeBootstrapStepKind) step->kind) {
        case RUNTIME_BOOTSTRAP_LAZY:
            ok = runtime_install_lazy_bootstrap_feature(
                host, (LazyBootstrapFeature) step->lazy);
            break;
        case RUNTIME_BOOTSTRAP_HOST_CHANNEL:
            ok = runtime_define_host_channel(host, true);
            break;
        case RUNTIME_BOOTSTRAP_EVALUATE:
        default:
            ok = runtime_evaluate_bootstrap_profiled(
                host, step->source,
                step->source_length != NULL ? *step->source_length
                                            : step->bytecode->source_length,
                step->source_url, step->bytecode,
                step->prefer_source_on_tight_heap, trace_startup,
                &clock->bootstrap_total_us, &clock->bootstrap_compile_us);
            break;
        }
        if (!ok) return false;
    }
    return true;
}

/* Natives published only after the bootstrap: the legacy surface
   (document.all and friends) and CSS.registerProperty, whose lazy function
   entry then stays cold on pages that never register a property. */
static bool runtime_install_late_surface(ScriptRuntime *host,
                                         JSValue global)
{
    bool evaluated = true;
    if (evaluated)
        evaluated = runtime_install_legacy_surface(host, global);
    if (evaluated) {
        /* Publish after bootstrap reflection so QuickJS's lazy function
           entry remains cold on pages that never register a property. */
        static const JSCFunctionListEntry css_functions[] = {
            { "registerProperty", JS_PROP_C_W_E, JS_DEF_CFUNC, 0,
              .u = { .func = { 1, JS_CFUNC_generic,
                  { .generic = js_css_register_property } } } }
        };
        JSValue css = JS_GetPropertyStr(host->context, global, "CSS");
        if (JS_IsException(css)) evaluated = false;
        else evaluated = JS_SetPropertyFunctionList(host->context, css,
            css_functions, sizeof(css_functions) / sizeof(css_functions[0])) == 0;
        JS_FreeValue(host->context, css);
    }
    return evaluated;
}

/* Captures the bootstrap hooks native code calls (host_callbacks[]) and
   navigator.storage before author code can replace them. */
static bool runtime_capture_trusted_hooks(ScriptRuntime *host,
                                          JSValue global)
{
    bool evaluated = true;
    if (evaluated) {
        JSValue navigator = JS_GetPropertyStr(
            host->context, global, "navigator");
        if (!JS_IsException(navigator)) {
            host->opfs_storage_manager = JS_GetPropertyStr(
                host->context, navigator, "storage");
            if (JS_IsException(host->opfs_storage_manager)) {
                JS_FreeValue(host->context,
                             host->opfs_storage_manager);
                host->opfs_storage_manager = JS_UNDEFINED;
            }
        }
        JS_FreeValue(host->context, navigator);
        evaluated = runtime_capture_host_callbacks(host, global);
    }
    return evaluated;
}

/* Takes what the bootstrap handed over through its private host
   channel: the XHR recorder installer (test-probe realms) and the network
   queue statistics. */
static bool runtime_take_host_handoffs(ScriptRuntime *host, JSValue global)
{
    bool evaluated = true;
#ifdef JS_RT_XHR_DIAGNOSTICS
    /* Test-probe realms hand over the XHR recorder installer; it never
       becomes a page-reachable global. */
    if (evaluated) {
        JSValue channel = JS_GetPropertyStr(
            host->context, global, "__tilefinchHostChannel");
        JSValue install = JS_IsObject(channel)
            ? JS_GetPropertyStr(host->context, channel, "xhrDiagnostics")
            : JS_UNDEFINED;
        if (JS_IsFunction(host->context, install)) {
            host->xhr_diagnostics_install = install;
        } else {
            JS_FreeValue(host->context, install);
        }
        JS_FreeValue(host->context, channel);
    }
#else
    (void) global;
#endif
    if (evaluated) {
        evaluated = runtime_take_host_channel(
            host, "networkQueueStats", &host->host_network_queue_stats);
    }
    return evaluated;
}

static bool runtime_remove_transient_globals(ScriptRuntime *host,
                                            JSValue global)
{
    bool evaluated = true;
    /* Remove temporary bootstrap bridges before author code; native code
       has already captured their callable values. Worker installs and
       removes its compiler bridge inside the first-use loader. */
    if (evaluated) {
        JSAtom media_state_atom = JS_NewAtom(
            host->context, "__tilefinchMediaStateFor");
        JSAtom fullscreen_exit_atom = JS_NewAtom(
            host->context, "__tilefinchExitFullscreenFromHost");
        JSAtom visibility_apply_atom = JS_NewAtom(
            host->context, "__tilefinchApplyPageVisibility");
        JSAtom visibility_query_atom = JS_NewAtom(
            host->context, "__tilefinchPageVisible");
        JSAtom user_activation_atom = JS_NewAtom(
            host->context, "__tilefinchUserActivationState");
        if (media_state_atom == JS_ATOM_NULL
            || fullscreen_exit_atom == JS_ATOM_NULL
            || visibility_apply_atom == JS_ATOM_NULL
            || visibility_query_atom == JS_ATOM_NULL
            || user_activation_atom == JS_ATOM_NULL
            || JS_DeleteProperty(
                   host->context, global, media_state_atom, 0) < 0
            || JS_DeleteProperty(
                   host->context, global, fullscreen_exit_atom, 0) < 0
            || JS_DeleteProperty(
                   host->context, global, visibility_apply_atom, 0) <= 0
            || JS_DeleteProperty(
                   host->context, global, visibility_query_atom, 0) <= 0
            || JS_DeleteProperty(
                   host->context, global, user_activation_atom, 0) <= 0) {
            evaluated = false;
        }
        JS_FreeAtom(host->context, media_state_atom);
        JS_FreeAtom(host->context, fullscreen_exit_atom);
        JS_FreeAtom(host->context, visibility_apply_atom);
        JS_FreeAtom(host->context, visibility_query_atom);
        JS_FreeAtom(host->context, user_activation_atom);
    }
    return evaluated;
}

/* The last steps before author code: the lab's census probe, the
   document's dynamic-code policy, and a fresh deterministic clock. */
static bool runtime_prepare_author_code(
    ScriptRuntime *host, const ScriptRuntimeOptions *options)
{
    bool evaluated = true;
#if !defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    (void) options;
#endif
#ifndef TILEFINCH_NO_TRACE
    /* Lab-only site-census instrumentation (tools/site-census): run a
       file in the top-level page realm before author code. A failing
       probe is reported and never fails the page. */
    if (evaluated && tilefinch_lab_init_script_path() != NULL
        && host->document_scope == SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) {
        runtime_evaluate_lab_init_script(host);
    }
#endif
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
    return evaluated;
}

/* Runs the document's parser-inserted scripts, then DOMContentLoaded and
   the microtasks it queued; deferred runtimes skip the scripts and the
   event (script_runtime_finish_loading() runs them later). */
static bool runtime_start_document(ScriptRuntime *host,
                                   PocDocument *document,
                                   bool run_document_scripts)
{
    bool evaluated = true;
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
    return evaluated;
}

static ScriptRuntime *script_runtime_create_internal(
    PocDocument *document, Budget *budget, size_t js_memory_limit,
    unsigned timeout_ms, const char *document_url,
    const ScriptRuntimeOptions *options,
    ScriptResult *result)
{
    if (!runtime_create_check_options(document, budget, options, result))
        return NULL;
    const bool run_document_scripts = !options->defer_document_scripts;
    const bool trace_startup =
        tilefinch_trace_js_startup();
    RuntimeStartupClock clock = { .started_ns = js_rt_monotonic_time_ns() };
    RuntimeReplayDerivation replay;
    if (!runtime_create_derive_replay(document_url, result, &replay))
        return NULL;
    ScriptRuntime *host = runtime_create_host(
        document, budget, timeout_ms, document_url, options, result);
    if (host == NULL) return NULL;
    if (!runtime_create_engine(host, budget, js_memory_limit, result, &clock))
        return runtime_create_abandon(host);
    uint64_t performance_time_origin_ms = 0;
    if (!runtime_create_bind_bridge(host, document, budget, timeout_ms,
                                    options, &replay, result,
                                    &performance_time_origin_ms)) {
        script_runtime_destroy(host);
        return NULL;
    }

    JSValue global = JS_GetGlobalObject(host->context);
    JSValue js_document = JS_NewObject(host->context);
    bool setup_ok = !JS_IsException(global) && !JS_IsException(js_document);
    if (setup_ok)
        setup_ok = runtime_install_globals(host, global, &js_document,
                                           options,
                                           performance_time_origin_ms);
    clock.native_ready_ns = js_rt_monotonic_time_ns();
    if (setup_ok) setup_ok = js_rt_runtime_script_checkpoint(host, 1);
    bool evaluated = false;
    if (!setup_ok) {
        js_rt_record_exception(host->context, &host->result);
    } else {
        evaluated = runtime_evaluate_bootstrap_sequence(
            host, trace_startup, &clock);
        if (evaluated)
            evaluated = runtime_install_late_surface(host, global);
        if (evaluated)
            evaluated = runtime_capture_trusted_hooks(host, global);
#if !defined(TILEFINCH_BOOTSTRAP_BYTECODE_ONLY)
        if (evaluated && tilefinch_trace_eval_source()) {
#ifndef TILEFINCH_NO_TRACE
            JSValue trace_eval_source = JS_NewCFunction(
                host->context, js_runtime_trace_eval_source,
                "__tilefinchTraceEvalSource", 1);
            if (JS_IsException(trace_eval_source)
                || JS_DefinePropertyValueStr(
                       host->context, global,
                       "__tilefinchTraceEvalSource", trace_eval_source,
                       JS_PROP_CONFIGURABLE) < 0) {
                if (JS_IsException(trace_eval_source)) {
                    JS_FreeValue(host->context, trace_eval_source);
                }
                evaluated = false;
            }
#endif
            if (evaluated) evaluated = runtime_evaluate_checkpointed(host,
                browser_eval_source_diagnostic,
                browser_eval_source_diagnostic_length,
                "<eval-source-diagnostic>");
        }
#endif
        if (evaluated)
            evaluated = runtime_take_host_handoffs(host, global);
        if (evaluated)
            evaluated = runtime_remove_transient_globals(host, global);
        if (evaluated)
            evaluated = runtime_prepare_author_code(host, options);
        if (evaluated)
            evaluated = runtime_start_document(host, document,
                                               run_document_scripts);
    }
    if (evaluated) evaluated = js_rt_runtime_refresh(host);

    if (evaluated) {
        js_rt_runtime_read_lab_summary(host, global);
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
        uint64_t total_us = finished_ns >= clock.started_ns
            ? (finished_ns - clock.started_ns) / UINT64_C(1000) : 0;
        uint64_t runtime_us = clock.runtime_ready_ns >= clock.started_ns
            ? (clock.runtime_ready_ns - clock.started_ns) / UINT64_C(1000) : 0;
        uint64_t context_us = clock.context_ready_ns >= clock.runtime_ready_ns
            ? (clock.context_ready_ns - clock.runtime_ready_ns) / UINT64_C(1000) : 0;
        uint64_t native_us = clock.native_ready_ns >= clock.context_ready_ns
            ? (clock.native_ready_ns - clock.context_ready_ns) / UINT64_C(1000) : 0;
        uint64_t accounted_us = runtime_us + context_us + native_us;
        accounted_us = accounted_us > UINT64_MAX - clock.bootstrap_total_us
            ? UINT64_MAX : accounted_us + clock.bootstrap_total_us;
        fprintf(stderr,
                "javascript-startup-summary total-us=%llu runtime-us=%llu "
                "context-us=%llu native-install-us=%llu bootstraps-us=%llu "
                "bootstrap-compile-us=%llu bootstrap-execute-us=%llu "
                "post-us=%llu success=%s\n",
                (unsigned long long) total_us,
                (unsigned long long) runtime_us,
                (unsigned long long) context_us,
                (unsigned long long) native_us,
                (unsigned long long) clock.bootstrap_total_us,
                (unsigned long long) clock.bootstrap_compile_us,
                (unsigned long long) (clock.bootstrap_total_us
                    >= clock.bootstrap_compile_us
                    ? clock.bootstrap_total_us - clock.bootstrap_compile_us : 0),
                (unsigned long long) (total_us >= accounted_us
                    ? total_us - accounted_us : 0),
                host->result.success ? "yes" : "no");
    }
    if (!host->result.success) {
        script_runtime_destroy(host);
        return NULL;
    }
    script_census_attach(host->runtime, budget);
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
