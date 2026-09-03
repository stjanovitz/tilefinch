#include "js_runtime_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/wasm_runtime.h"

#if defined(TILEFINCH_HAVE_WAMR) || defined(TILEFINCH_HAVE_WAMR_COMPONENT)
#include <wasm_export.h>
uint32_t tilefinch_wasm_export_function_index(
    wasm_module_t module, const char *name);
uint32_t tilefinch_wasm_imported_start_function_index(wasm_module_t module);
#endif

#if !defined(TILEFINCH_HAVE_WAMR_COMPONENT)
void js_wasm_component_configure(const char *program_directory)
{
    (void)program_directory;
}
#endif

#if defined(TILEFINCH_HAVE_WAMR_COMPONENT)
#include "tilefinch/wasm_component.h"
#include <pspkernel.h>

static TilefinchWasmComponentApi wasm_component_api;
static char wasm_component_path[256];
static SceUID wasm_component_module = -1;

void js_wasm_component_configure(const char *program_directory)
{
    wasm_component_path[0] = '\0';
    if (program_directory == NULL || program_directory[0] == '\0') return;
    int length = snprintf(wasm_component_path, sizeof(wasm_component_path),
                          "%s/tilefinch-wasm.prx", program_directory);
    if (length <= 0 || (size_t)length >= sizeof(wasm_component_path))
        wasm_component_path[0] = '\0';
}

static bool wasm_component_load(void)
{
    if (wasm_component_api.magic == TILEFINCH_WASM_COMPONENT_MAGIC
        && wasm_component_api.abi_version == TILEFINCH_WASM_COMPONENT_ABI_VERSION
        && wasm_component_api.struct_size >= sizeof(wasm_component_api)) return true;
    if (wasm_component_path[0] == '\0') return false;
    SceUID module = sceKernelLoadModule(wasm_component_path, 0, NULL);
    if (module < 0) return false;
    memset(&wasm_component_api, 0, sizeof(wasm_component_api));
    TilefinchWasmComponentStart start = {
        .magic = TILEFINCH_WASM_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_WASM_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(start),
        .api = &wasm_component_api
    };
    int status = -1;
    int started = sceKernelStartModule(
        module, sizeof(start), &start, &status, NULL);
    if (started < 0 || status != 0
        || wasm_component_api.magic != TILEFINCH_WASM_COMPONENT_MAGIC
        || wasm_component_api.abi_version != TILEFINCH_WASM_COMPONENT_ABI_VERSION
        || wasm_component_api.struct_size < sizeof(wasm_component_api)) {
        (void)sceKernelUnloadModule(module);
        memset(&wasm_component_api, 0, sizeof(wasm_component_api));
        return false;
    }
    wasm_component_module = module;
    return true;
}

#define wasm_runtime_full_init(...) wasm_component_api.runtime_full_init(__VA_ARGS__)
#define wasm_runtime_destroy(...) wasm_component_api.runtime_destroy(__VA_ARGS__)
#define wasm_runtime_set_log_level(...) wasm_component_api.runtime_set_log_level(__VA_ARGS__)
#define wasm_runtime_load(...) wasm_component_api.runtime_load(__VA_ARGS__)
#define wasm_runtime_load_ex(...) wasm_component_api.runtime_load_ex(__VA_ARGS__)
#define wasm_runtime_unload(...) wasm_component_api.runtime_unload(__VA_ARGS__)
#define wasm_runtime_instantiate_ex(...) wasm_component_api.runtime_instantiate_ex(__VA_ARGS__)
#define wasm_runtime_deinstantiate(...) wasm_component_api.runtime_deinstantiate(__VA_ARGS__)
#define wasm_runtime_create_exec_env(...) wasm_component_api.runtime_create_exec_env(__VA_ARGS__)
#define wasm_runtime_destroy_exec_env(...) wasm_component_api.runtime_destroy_exec_env(__VA_ARGS__)
#define wasm_runtime_set_instruction_count_limit(...) wasm_component_api.runtime_set_instruction_count_limit(__VA_ARGS__)
#define wasm_runtime_get_import_count(...) wasm_component_api.runtime_get_import_count(__VA_ARGS__)
#define wasm_runtime_get_import_type(...) wasm_component_api.runtime_get_import_type(__VA_ARGS__)
#define wasm_runtime_get_export_count(...) wasm_component_api.runtime_get_export_count(__VA_ARGS__)
#define wasm_runtime_get_export_type(...) wasm_component_api.runtime_get_export_type(__VA_ARGS__)
#define tilefinch_wasm_export_function_index(...) \
    wasm_component_api.module_get_export_function_index(__VA_ARGS__)
#define tilefinch_wasm_imported_start_function_index(...) \
    wasm_component_api.module_get_imported_start_function_index(__VA_ARGS__)
#define wasm_runtime_register_natives_raw(...) wasm_component_api.runtime_register_natives_raw(__VA_ARGS__)
#define wasm_runtime_unregister_natives(...) wasm_component_api.runtime_unregister_natives(__VA_ARGS__)
#define wasm_runtime_lookup_function(...) wasm_component_api.runtime_lookup_function(__VA_ARGS__)
#define wasm_runtime_lookup_memory(...) wasm_component_api.runtime_lookup_memory(__VA_ARGS__)
#define wasm_runtime_get_export_global_inst(...) \
    wasm_component_api.runtime_get_export_global_inst(__VA_ARGS__)
#define wasm_func_get_param_count(...) wasm_component_api.func_get_param_count(__VA_ARGS__)
#define wasm_func_get_result_count(...) wasm_component_api.func_get_result_count(__VA_ARGS__)
#define wasm_func_get_param_types(...) wasm_component_api.func_get_param_types(__VA_ARGS__)
#define wasm_func_get_result_types(...) wasm_component_api.func_get_result_types(__VA_ARGS__)
#define wasm_runtime_call_wasm_a(...) wasm_component_api.runtime_call_wasm_a(__VA_ARGS__)
#define wasm_runtime_get_exception(...) wasm_component_api.runtime_get_exception(__VA_ARGS__)
#define wasm_runtime_set_exception(...) wasm_component_api.runtime_set_exception(__VA_ARGS__)
#define wasm_runtime_get_function_attachment(...) wasm_component_api.runtime_get_function_attachment(__VA_ARGS__)
#define wasm_func_type_get_param_count(...) wasm_component_api.func_type_get_param_count(__VA_ARGS__)
#define wasm_func_type_get_result_count(...) wasm_component_api.func_type_get_result_count(__VA_ARGS__)
#define wasm_func_type_get_param_valkind(...) wasm_component_api.func_type_get_param_valkind(__VA_ARGS__)
#define wasm_func_type_get_result_valkind(...) wasm_component_api.func_type_get_result_valkind(__VA_ARGS__)
#define wasm_memory_get_base_address(...) wasm_component_api.memory_get_base_address(__VA_ARGS__)
#define wasm_memory_get_bytes_per_page(...) wasm_component_api.memory_get_bytes_per_page(__VA_ARGS__)
#define wasm_memory_get_cur_page_count(...) wasm_component_api.memory_get_cur_page_count(__VA_ARGS__)
#define wasm_memory_get_max_page_count(...) wasm_component_api.memory_get_max_page_count(__VA_ARGS__)
#define wasm_memory_enlarge(...) wasm_component_api.memory_enlarge(__VA_ARGS__)
#endif

JSValue js_wasm_detach_buffer(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "ArrayBuffer expected");
    size_t length = 0;
    if (JS_GetArrayBuffer(context, &length, argv[0]) == NULL) {
        JSValue exception = JS_GetException(context);
        JS_FreeValue(context, exception);
        return JS_ThrowTypeError(context, "ArrayBuffer expected");
    }
    JS_DetachArrayBuffer(context, argv[0]);
    return JS_UNDEFINED;
}

#define TILEFINCH_WASM_MODULE_BYTES (512u * 1024u)
#define TILEFINCH_WASM_POOL_BYTES (4u * 1024u * 1024u)
#define TILEFINCH_WASM_STACK_BYTES (64u * 1024u)
#define TILEFINCH_WASM_MAX_MEMORY_PAGES 16u
#define TILEFINCH_WASM_MAX_EXPORTS 64u
#define TILEFINCH_WASM_MAX_ARGUMENTS 16u
#define TILEFINCH_WASM_MAX_RESULTS 4u
#define TILEFINCH_WASM_MAX_IMPORTS 32u
#define TILEFINCH_WASM_MAX_IMPORT_NAME_BYTES 4096u
#define TILEFINCH_WASM_MAX_CUSTOM_SECTIONS 32u
/* Native WebAssembly executes outside QuickJS's interrupt handler.  This is
   therefore the browser-thread responsiveness boundary, not merely a memory
   quota.  Fifty million interpreter dispatches is still an explicit per-call
   ceiling, while admitting fixed-width-SIMD feature/throughput probes whose
   vector loops legitimately exceed the original ten-million boundary. */
#define TILEFINCH_WASM_INSTRUCTION_LIMIT 50000000

#if defined(TILEFINCH_HAVE_WAMR) || defined(TILEFINCH_HAVE_WAMR_COMPONENT)

typedef struct TilefinchWasmInstance TilefinchWasmInstance;

typedef struct {
    TilefinchWasmInstance *owner;
    JSContext *context;
    JSValue function;
    NativeSymbol symbol;
    const char *module_name;
    wasm_valkind_t parameter_types[TILEFINCH_WASM_MAX_ARGUMENTS];
    wasm_valkind_t result_type;
    uint8_t parameter_count;
    uint8_t result_count;
    bool registered;
    char signature[TILEFINCH_WASM_MAX_ARGUMENTS + 4u];
} TilefinchWasmImport;

struct TilefinchWasmInstance {
    Budget *budget;
    uint8_t *module_bytes;
    size_t module_length;
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t execution;
    wasm_memory_inst_t exported_memory;
    JSValue memory_buffer;
    JSValue memory_object;
    JSValue pending_import_exception;
    TilefinchWasmImport *imports;
    char *import_names;
    uint32_t import_count;
    size_t memory_bytes;
    bool counted_live;
    bool owns_runtime_operation;
};

typedef struct {
    Budget *budget;
    uint8_t *bytes;
    size_t length;
} TilefinchWasmModule;

typedef struct {
    Budget *budget;
    void *pool;
    size_t live_instances;
    size_t active_operations;
    bool initialized;
} TilefinchWasmRuntime;

/* QuickJS's ordinary ArrayBuffer allocator is constrained by the realm heap.
   Linear memory is already charged to the page by WAMR, and a coherent script
   view can be as large as one MiB. Charging the mirror to the QuickJS heap as
   well makes a perfectly valid bounded module fail merely because the realm
   has compiled a large application. Keep the mirror Budget-owned instead;
   the ArrayBuffer finalizer returns its combined header/data allocation to
   the same page budget.
   The mirror remains intentional for now because WAMR may relocate its memory
   during grow. */
typedef struct {
    Budget *budget;
    uint8_t bytes[];
} TilefinchWasmArrayBufferBacking;

static TilefinchWasmRuntime wasm_runtime_state;
static JSClassID wasm_instance_class_id;
static JSClassID wasm_module_class_id;

static void wasm_array_buffer_free(
    JSRuntime *runtime, void *opaque, void *pointer)
{
    (void) runtime;
    (void) opaque;
    /* Bellard QuickJS invokes an external ArrayBuffer's free callback once
       with its data while detaching and again with NULL when the detached
       object is finalized. Derive ownership only from a live data pointer;
       retaining an opaque allocation across the first call would either leak
       on ordinary finalization or become stale on the second call. */
    if (pointer == NULL) return;
    TilefinchWasmArrayBufferBacking *backing =
        (TilefinchWasmArrayBufferBacking *)
            ((uint8_t *) pointer
             - offsetof(TilefinchWasmArrayBufferBacking, bytes));
    budget_free(backing->budget, backing);
}

static JSValue wasm_new_array_buffer_copy(
    JSContext *context, Budget *budget, const uint8_t *source, size_t length)
{
    if (budget == NULL || (length != 0 && source == NULL))
        return JS_ThrowInternalError(context, "invalid WebAssembly memory");
    if (length > SIZE_MAX - sizeof(TilefinchWasmArrayBufferBacking))
        return JS_ThrowOutOfMemory(context);
    TilefinchWasmArrayBufferBacking *backing = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, sizeof(*backing) + length);
    if (backing == NULL) return JS_ThrowOutOfMemory(context);
    backing->budget = budget;
    if (length != 0)
        memcpy(backing->bytes, source, length);
    JSValue value = JS_NewArrayBuffer(
        context, backing->bytes, length, wasm_array_buffer_free,
        NULL, false);
    if (JS_IsException(value))
        budget_free(budget, backing);
    return value;
}

static bool wasm_read_u32_leb(
    const uint8_t *bytes, size_t length, size_t *cursor, uint32_t *value)
{
    if (bytes == NULL || cursor == NULL || value == NULL) return false;
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (*cursor >= length) return false;
        uint8_t byte = bytes[(*cursor)++];
        if (shift == 28 && (byte & 0xf0u) != 0) return false;
        result |= (uint32_t) (byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *value = result;
            return true;
        }
    }
    return false;
}

static const char *wasm_external_kind_name(wasm_import_export_kind_t kind)
{
    switch (kind) {
    case WASM_IMPORT_EXPORT_KIND_FUNC: return "function";
    case WASM_IMPORT_EXPORT_KIND_TABLE: return "table";
    case WASM_IMPORT_EXPORT_KIND_MEMORY: return "memory";
    case WASM_IMPORT_EXPORT_KIND_GLOBAL: return "global";
    default: return NULL;
    }
}

/* Compilation and reflection validate module structure without linking its
   imports. Linking belongs to Instance construction; otherwise a valid
   `new WebAssembly.Module(bytes)` would incorrectly require an import object. */
static wasm_module_t wasm_load_for_inspection(
    uint8_t *bytes, size_t length, char *error, size_t error_capacity)
{
    LoadArgs arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.no_resolve = true;
    return wasm_runtime_load_ex(
        bytes, (uint32_t) length, &arguments,
        error, (uint32_t) error_capacity);
}

static char wasm_signature_kind(wasm_valkind_t kind)
{
    switch (kind) {
    case WASM_I32: return 'i';
    case WASM_I64: return 'I';
    case WASM_F32: return 'f';
    case WASM_F64: return 'F';
    default: return '\0';
    }
}

static void wasm_import_fail(
    TilefinchWasmImport *imported, JSValue exception, const char *message)
{
    if (imported == NULL || imported->owner == NULL) return;
    TilefinchWasmInstance *owner = imported->owner;
    if (!JS_IsUndefined(owner->pending_import_exception))
        JS_FreeValue(imported->context, owner->pending_import_exception);
    owner->pending_import_exception = exception;
    if (owner->instance != NULL)
        wasm_runtime_set_exception(owner->instance, message);
}

static void wasm_import_raw_callback(
    wasm_exec_env_t execution, uint64_t *raw_arguments)
{
    TilefinchWasmImport *imported = wasm_runtime_get_function_attachment(
        execution);
    if (imported == NULL || imported->context == NULL
        || imported->owner == NULL || raw_arguments == NULL) return;
    JSContext *context = imported->context;
    JSValue arguments[TILEFINCH_WASM_MAX_ARGUMENTS];
    memset(arguments, 0, sizeof(arguments));
    uint64_t *raw_at = raw_arguments;
    for (uint32_t i = 0; i < imported->parameter_count; i++, raw_at++) {
        switch (imported->parameter_types[i]) {
        case WASM_I32: {
            int32_t value = 0;
            memcpy(&value, raw_at, sizeof(value));
            arguments[i] = JS_NewInt32(context, value);
            break;
        }
        case WASM_I64: {
            int64_t value = 0;
            memcpy(&value, raw_at, sizeof(value));
            arguments[i] = JS_NewBigInt64(context, value);
            break;
        }
        case WASM_F32: {
            float value = 0;
            memcpy(&value, raw_at, sizeof(value));
            arguments[i] = JS_NewFloat64(context, value);
            break;
        }
        case WASM_F64: {
            double value = 0;
            memcpy(&value, raw_at, sizeof(value));
            arguments[i] = JS_NewFloat64(context, value);
            break;
        }
        default:
            JS_ThrowTypeError(context, "unsupported import type");
            wasm_import_fail(
                imported, JS_GetException(context),
                "unsupported JavaScript import type");
            return;
        }
    }
    JSValue result = JS_Call(
        context, imported->function, JS_UNDEFINED,
        imported->parameter_count, arguments);
    for (uint32_t i = 0; i < imported->parameter_count; i++)
        JS_FreeValue(context, arguments[i]);
    if (JS_IsException(result)) {
        wasm_import_fail(
            imported, JS_GetException(context),
            "JavaScript WebAssembly import threw");
        return;
    }
    if (imported->result_count != 0) {
        int failed = 0;
        switch (imported->result_type) {
        case WASM_I32: {
            int32_t value = 0;
            failed = JS_ToInt32(context, &value, result);
            if (failed >= 0)
                memcpy(raw_arguments, &value, sizeof(value));
            break;
        }
        case WASM_I64: {
            int64_t value = 0;
            failed = JS_ToBigInt64(context, &value, result);
            if (failed >= 0)
                memcpy(raw_arguments, &value, sizeof(value));
            break;
        }
        case WASM_F32: {
            double value = 0;
            failed = JS_ToFloat64(context, &value, result);
            if (failed >= 0) {
                float narrowed = (float) value;
                memcpy(raw_arguments, &narrowed, sizeof(narrowed));
            }
            break;
        }
        case WASM_F64: {
            double value = 0;
            failed = JS_ToFloat64(context, &value, result);
            if (failed >= 0)
                memcpy(raw_arguments, &value, sizeof(value));
            break;
        }
        default:
            failed = -1;
            JS_ThrowTypeError(context, "unsupported WebAssembly import result");
            break;
        }
        if (failed < 0) {
            JS_FreeValue(context, result);
            wasm_import_fail(
                imported, JS_GetException(context),
                "invalid JavaScript WebAssembly import result");
            return;
        }
    }
    JS_FreeValue(context, result);
}

static bool wasm_trace_enabled(void)
{
    return getenv("TILEFINCH_TRACE_WASM") != NULL;
}

static void wasm_runtime_release_if_idle(void)
{
    if (!wasm_runtime_state.initialized
        || wasm_runtime_state.live_instances != 0
        || wasm_runtime_state.active_operations != 0) return;
    wasm_runtime_destroy();
    budget_free(wasm_runtime_state.budget, wasm_runtime_state.pool);
    memset(&wasm_runtime_state, 0, sizeof(wasm_runtime_state));
}

static bool wasm_runtime_enter(Budget *budget)
{
    if (budget == NULL) return false;
    if (wasm_runtime_state.initialized) {
        if (wasm_runtime_state.budget != budget) return false;
    } else {
        void *pool = budget_malloc_category(
            budget, BUDGET_CATEGORY_JAVASCRIPT, TILEFINCH_WASM_POOL_BYTES);
        if (pool == NULL) return false;
        RuntimeInitArgs arguments;
        memset(&arguments, 0, sizeof(arguments));
        arguments.mem_alloc_type = Alloc_With_Pool;
        arguments.mem_alloc_option.pool.heap_buf = pool;
        arguments.mem_alloc_option.pool.heap_size = TILEFINCH_WASM_POOL_BYTES;
        arguments.running_mode = Mode_Interp;
        if (!wasm_runtime_full_init(&arguments)) {
            budget_free(budget, pool);
            return false;
        }
        wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);
        wasm_runtime_state.budget = budget;
        wasm_runtime_state.pool = pool;
        wasm_runtime_state.initialized = true;
    }
    if (wasm_runtime_state.active_operations == SIZE_MAX) {
        wasm_runtime_release_if_idle();
        return false;
    }
    wasm_runtime_state.active_operations++;
    return true;
}

static void wasm_runtime_leave(void)
{
    if (wasm_runtime_state.active_operations != 0)
        wasm_runtime_state.active_operations--;
    wasm_runtime_release_if_idle();
}

static void wasm_instance_destroy(
    JSRuntime *runtime, TilefinchWasmInstance *instance)
{
    if (instance == NULL) return;
    if (runtime != NULL)
        JS_FreeValueRT(runtime, instance->memory_buffer);
    if (runtime != NULL)
        JS_FreeValueRT(runtime, instance->memory_object);
    if (runtime != NULL)
        JS_FreeValueRT(runtime, instance->pending_import_exception);
    if (instance->execution != NULL)
        wasm_runtime_destroy_exec_env(instance->execution);
    if (instance->instance != NULL)
        wasm_runtime_deinstantiate(instance->instance);
    if (instance->module != NULL)
        wasm_runtime_unload(instance->module);
    for (uint32_t i = 0; i < instance->import_count; i++) {
        TilefinchWasmImport *imported = &instance->imports[i];
        if (imported->registered) {
            (void) wasm_runtime_unregister_natives(
                imported->module_name,
                &imported->symbol);
        }
        if (runtime != NULL)
            JS_FreeValueRT(runtime, imported->function);
    }
    budget_free(instance->budget, instance->imports);
    budget_free(instance->budget, instance->import_names);
    budget_free(instance->budget, instance->module_bytes);
    Budget *budget = instance->budget;
    bool counted_live = instance->counted_live;
    bool owns_runtime_operation = instance->owns_runtime_operation;
    budget_free(budget, instance);
    if (counted_live && wasm_runtime_state.live_instances != 0)
        wasm_runtime_state.live_instances--;
    if (owns_runtime_operation
        && wasm_runtime_state.active_operations != 0) {
        wasm_runtime_state.active_operations--;
    }
    wasm_runtime_release_if_idle();
}

static void wasm_instance_finalizer(JSRuntime *runtime, JSValue value)
{
    TilefinchWasmInstance *instance =
        JS_GetOpaque(value, wasm_instance_class_id);
    wasm_instance_destroy(runtime, instance);
}

static void wasm_instance_mark(
    JSRuntime *runtime, JSValueConst value, JS_MarkFunc *mark)
{
    TilefinchWasmInstance *instance =
        JS_GetOpaque(value, wasm_instance_class_id);
    if (instance != NULL)
        JS_MarkValue(runtime, instance->memory_buffer, mark);
    if (instance != NULL)
        JS_MarkValue(runtime, instance->memory_object, mark);
    if (instance != NULL)
        JS_MarkValue(runtime, instance->pending_import_exception, mark);
    if (instance != NULL) {
        for (uint32_t i = 0; i < instance->import_count; i++)
            JS_MarkValue(runtime, instance->imports[i].function, mark);
    }
}

static const JSClassDef wasm_instance_class = {
    .class_name = "WebAssembly.Instance",
    .finalizer = wasm_instance_finalizer,
    .gc_mark = wasm_instance_mark
};

static void wasm_module_finalizer(JSRuntime *runtime, JSValue value)
{
    (void) runtime;
    TilefinchWasmModule *module = JS_GetOpaque(value, wasm_module_class_id);
    if (module == NULL) return;
    budget_free(module->budget, module->bytes);
    budget_free(module->budget, module);
}

static const JSClassDef wasm_module_class = {
    .class_name = "WebAssembly.Module",
    .finalizer = wasm_module_finalizer
};

bool js_wasm_runtime_init(JSRuntime *runtime)
{
    if (runtime == NULL) return false;
    if (wasm_instance_class_id == 0) {
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        JS_NewClassID(&wasm_instance_class_id);
#else
        JS_NewClassID(runtime, &wasm_instance_class_id);
#endif
    }
    if (wasm_module_class_id == 0) {
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
        JS_NewClassID(&wasm_module_class_id);
#else
        JS_NewClassID(runtime, &wasm_module_class_id);
#endif
    }
    return wasm_instance_class_id != 0 && wasm_module_class_id != 0
        && JS_NewClass(runtime, wasm_instance_class_id,
                       &wasm_instance_class) >= 0
        && JS_NewClass(runtime, wasm_module_class_id,
                       &wasm_module_class) >= 0;
}

static bool wasm_buffer_source(
    JSContext *context, JSValueConst value,
    const uint8_t **bytes, size_t *length, JSValue *owner)
{
    *bytes = JS_GetArrayBuffer(context, length, value);
    *owner = JS_UNDEFINED;
    if (*bytes != NULL) return true;
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
    size_t offset = 0;
    size_t view_length = 0;
    size_t element_bytes = 0;
    JSValue buffer = JS_GetTypedArrayBuffer(
        context, value, &offset, &view_length, &element_bytes);
    (void) element_bytes;
    if (JS_IsException(buffer)) return false;
    size_t buffer_length = 0;
    uint8_t *buffer_bytes = JS_GetArrayBuffer(
        context, &buffer_length, buffer);
    if (buffer_bytes == NULL || offset > buffer_length
        || view_length > buffer_length - offset) {
        JS_FreeValue(context, buffer);
        return false;
    }
    *bytes = buffer_bytes + offset;
    *length = view_length;
    *owner = buffer;
    return true;
}

JSValue js_wasm_snapshot_source(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "WebAssembly source is required");
    const uint8_t *source = NULL;
    size_t source_length = 0;
    JSValue source_owner = JS_UNDEFINED;
    if (!wasm_buffer_source(
            context, argv[0], &source, &source_length, &source_owner)) {
        return JS_ThrowTypeError(
            context, "WebAssembly source must be an ArrayBuffer or view");
    }
    if (source_length > TILEFINCH_WASM_MODULE_BYTES) {
        JS_FreeValue(context, source_owner);
        return JS_ThrowRangeError(
            context, "WebAssembly module exceeds Tilefinch limits");
    }
    JSValue snapshot = JS_NewArrayBufferCopy(
        context, source, source_length);
    JS_FreeValue(context, source_owner);
    return snapshot;
}

static JSValue wasm_throw_runtime(
    JSContext *context, const char *prefix, const char *detail)
{
    return JS_ThrowInternalError(
        context, "%s%s%s", prefix,
        detail != NULL && detail[0] != '\0' ? ": " : "",
        detail == NULL ? "" : detail);
}

static JSValue wasm_throw_tagged(
    JSContext *context, const char *prefix, const char *detail,
    const char *marker)
{
    char message[256];
    snprintf(
        message, sizeof(message), "%s%s%s", prefix,
        detail != NULL && detail[0] != '\0' ? ": " : "",
        detail == NULL ? "" : detail);
    JSValue error = JS_NewError(context);
    if (JS_IsException(error)) return JS_EXCEPTION;
    if (JS_SetPropertyStr(
            context, error, "message", JS_NewString(context, message)) < 0
        || JS_DefinePropertyValueStr(
               context, error, marker,
               JS_NewBool(context, true), 0) < 0) {
        JS_FreeValue(context, error);
        return JS_EXCEPTION;
    }
    return JS_Throw(context, error);
}

static JSValue wasm_throw_execution_runtime(
    JSContext *context, const char *prefix, const char *detail)
{
    return wasm_throw_tagged(
        context, prefix, detail, "__tilefinchWasmRuntimeTrap");
}

static JSValue wasm_throw_link(
    JSContext *context, const char *prefix, const char *detail)
{
    return wasm_throw_tagged(
        context, prefix, detail, "__tilefinchWasmLinkError");
}

static bool wasm_memory_extent(
    wasm_memory_inst_t memory, size_t *bytes)
{
    if (memory == NULL || bytes == NULL) return false;
    uint64_t pages = wasm_memory_get_cur_page_count(memory);
    uint64_t page_bytes = wasm_memory_get_bytes_per_page(memory);
    if (page_bytes == 0 || pages > TILEFINCH_WASM_MAX_MEMORY_PAGES
        || pages > SIZE_MAX / page_bytes
        || pages * page_bytes
               > (uint64_t) TILEFINCH_WASM_MAX_MEMORY_PAGES * 65536u) {
        return false;
    }
    *bytes = (size_t) (pages * page_bytes);
    return true;
}

/* WAMR owns its bounded linear memory while QuickJS owns the page-visible
   ArrayBuffer.  Synchronize at the WebAssembly call boundary so JavaScript
   stores made before a call and WebAssembly stores made during it remain
   coherent without exposing a WAMR pointer whose address may change on grow. */
static bool wasm_memory_sync_to_runtime(
    JSContext *context, TilefinchWasmInstance *instance)
{
    if (instance->exported_memory == NULL) return true;
    size_t native_bytes = 0;
    size_t script_bytes = 0;
    uint8_t *script = JS_GetArrayBuffer(
        context, &script_bytes, instance->memory_buffer);
    if (!wasm_memory_extent(instance->exported_memory, &native_bytes)
        || script == NULL || native_bytes != instance->memory_bytes
        || script_bytes != native_bytes) {
        JS_ThrowRangeError(
            context, "WebAssembly memory growth exceeds Tilefinch limits");
        return false;
    }
    if (native_bytes != 0) {
        memcpy(wasm_memory_get_base_address(instance->exported_memory),
               script, native_bytes);
    }
    return true;
}

static bool wasm_memory_sync_to_script(
    JSContext *context, TilefinchWasmInstance *instance,
    bool force_replacement)
{
    if (instance->exported_memory == NULL) return true;
    size_t native_bytes = 0;
    size_t script_bytes = 0;
    uint8_t *script = JS_GetArrayBuffer(
        context, &script_bytes, instance->memory_buffer);
    if (!wasm_memory_extent(instance->exported_memory, &native_bytes)
        || script == NULL || script_bytes != instance->memory_bytes) {
        JS_ThrowRangeError(
            context, "WebAssembly memory growth exceeds Tilefinch limits");
        return false;
    }
    if (force_replacement || native_bytes != instance->memory_bytes) {
        JSValue replacement = wasm_new_array_buffer_copy(
            context, instance->budget,
            wasm_memory_get_base_address(instance->exported_memory),
            native_bytes);
        if (JS_IsException(replacement)) return false;
        JS_DetachArrayBuffer(context, instance->memory_buffer);
        JS_FreeValue(context, instance->memory_buffer);
        instance->memory_buffer = JS_DupValue(context, replacement);
        instance->memory_bytes = native_bytes;
        if (!JS_IsUndefined(instance->memory_object)
            && JS_SetPropertyStr(
                   context, instance->memory_object, "buffer",
                   JS_DupValue(context, replacement)) < 0) {
            JS_FreeValue(context, replacement);
            return false;
        }
        JS_FreeValue(context, replacement);
        return true;
    }
    if (native_bytes != 0) {
        memcpy(script,
               wasm_memory_get_base_address(instance->exported_memory),
               native_bytes);
    }
    return true;
}

static JSValue js_wasm_call_export(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv, int magic, JSValue *function_data)
{
    (void) this_value;
    (void) magic;
    TilefinchWasmInstance *instance = JS_GetOpaque2(
        context, function_data[0], wasm_instance_class_id);
    if (instance == NULL) return JS_EXCEPTION;
    const char *name = JS_ToCString(context, function_data[1]);
    if (name == NULL) return JS_EXCEPTION;
    wasm_function_inst_t function = wasm_runtime_lookup_function(
        instance->instance, name);
    if (wasm_trace_enabled()) {
        fprintf(stderr, "tilefinch-wasm: call export=%s argc=%d\n",
                name, argc);
    }
    JS_FreeCString(context, name);
    if (function == NULL)
        return wasm_throw_execution_runtime(
            context, "missing WebAssembly export", NULL);
    if (!wasm_memory_sync_to_runtime(context, instance))
        return JS_EXCEPTION;

    uint32_t parameter_count = wasm_func_get_param_count(
        function, instance->instance);
    uint32_t result_count = wasm_func_get_result_count(
        function, instance->instance);
    if (parameter_count > TILEFINCH_WASM_MAX_ARGUMENTS
        || result_count > TILEFINCH_WASM_MAX_RESULTS) {
        return JS_ThrowRangeError(
            context, "WebAssembly function signature exceeds Tilefinch limits");
    }
    wasm_valkind_t parameter_types[TILEFINCH_WASM_MAX_ARGUMENTS];
    wasm_valkind_t result_types[TILEFINCH_WASM_MAX_RESULTS];
    wasm_val_t arguments[TILEFINCH_WASM_MAX_ARGUMENTS];
    wasm_val_t results[TILEFINCH_WASM_MAX_RESULTS];
    memset(arguments, 0, sizeof(arguments));
    memset(results, 0, sizeof(results));
    wasm_func_get_param_types(function, instance->instance, parameter_types);
    wasm_func_get_result_types(function, instance->instance, result_types);
    for (uint32_t i = 0; i < parameter_count; i++) {
        JSValueConst input = i < (uint32_t) argc ? argv[i] : JS_UNDEFINED;
        arguments[i].kind = parameter_types[i];
        if (parameter_types[i] == WASM_I32) {
            if (JS_ToInt32(context, &arguments[i].of.i32, input) < 0)
                return JS_EXCEPTION;
        } else if (parameter_types[i] == WASM_I64) {
            if (JS_ToBigInt64(context, &arguments[i].of.i64, input) < 0)
                return JS_EXCEPTION;
        } else if (parameter_types[i] == WASM_F32) {
            double number = 0;
            if (JS_ToFloat64(context, &number, input) < 0)
                return JS_EXCEPTION;
            arguments[i].of.f32 = (float) number;
        } else if (parameter_types[i] == WASM_F64) {
            if (JS_ToFloat64(context, &arguments[i].of.f64, input) < 0)
                return JS_EXCEPTION;
        } else {
            return JS_ThrowTypeError(
                context, "unsupported WebAssembly argument type");
        }
    }
    for (uint32_t i = 0; i < result_count; i++)
        results[i].kind = result_types[i];
    /* WAMR stores a trap on the instance. It is state for the just-finished
       invocation, not a permanent poison bit for every later export call. */
    wasm_runtime_set_exception(instance->instance, NULL);
    wasm_runtime_set_instruction_count_limit(
        instance->execution, TILEFINCH_WASM_INSTRUCTION_LIMIT);
    bool called = wasm_runtime_call_wasm_a(
            instance->execution, function, result_count, results,
            parameter_count, arguments);
    if (!wasm_memory_sync_to_script(context, instance, false)) {
        wasm_runtime_set_exception(instance->instance, NULL);
        return JS_EXCEPTION;
    }
    if (!called) {
        if (!JS_IsUndefined(instance->pending_import_exception)) {
            JSValue exception = instance->pending_import_exception;
            instance->pending_import_exception = JS_UNDEFINED;
            wasm_runtime_set_exception(instance->instance, NULL);
            return JS_Throw(context, exception);
        }
        const char *exception = wasm_runtime_get_exception(instance->instance);
        JSValue error = wasm_throw_execution_runtime(
            context, "WebAssembly execution failed", exception);
        wasm_runtime_set_exception(instance->instance, NULL);
        return error;
    }
    wasm_runtime_set_exception(instance->instance, NULL);
    if (result_count == 0) return JS_UNDEFINED;
    JSValue converted[TILEFINCH_WASM_MAX_RESULTS];
    for (uint32_t i = 0; i < result_count; i++) {
        if (results[i].kind == WASM_I32)
            converted[i] = JS_NewInt32(context, results[i].of.i32);
        else if (results[i].kind == WASM_I64)
            converted[i] = JS_NewBigInt64(context, results[i].of.i64);
        else if (results[i].kind == WASM_F32)
            converted[i] = JS_NewFloat64(context, results[i].of.f32);
        else if (results[i].kind == WASM_F64)
            converted[i] = JS_NewFloat64(context, results[i].of.f64);
        else {
            for (uint32_t j = 0; j < i; j++)
                JS_FreeValue(context, converted[j]);
            return JS_ThrowTypeError(
                context, "unsupported WebAssembly result type");
        }
        if (JS_IsException(converted[i])) {
            for (uint32_t j = 0; j < i; j++)
                JS_FreeValue(context, converted[j]);
            return JS_EXCEPTION;
        }
    }
    if (result_count == 1) return converted[0];
    JSValue array = JS_NewArray(context);
    if (JS_IsException(array)) {
        for (uint32_t i = 0; i < result_count; i++)
            JS_FreeValue(context, converted[i]);
        return JS_EXCEPTION;
    }
    for (uint32_t i = 0; i < result_count; i++) {
        if (JS_SetPropertyUint32(context, array, i, converted[i]) < 0) {
            for (uint32_t j = i + 1; j < result_count; j++)
                JS_FreeValue(context, converted[j]);
            JS_FreeValue(context, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

static JSValue wasm_memory_export(
    JSContext *context, TilefinchWasmInstance *instance,
    JSValueConst instance_object, const char *name)
{
    wasm_memory_inst_t memory = wasm_runtime_lookup_memory(
        instance->instance, name);
    if (memory == NULL) return JS_UNDEFINED;
    if (instance->exported_memory == memory
        && !JS_IsUndefined(instance->memory_object)) {
        return JS_DupValue(context, instance->memory_object);
    }
    size_t bytes = 0;
    if (!wasm_memory_extent(memory, &bytes))
        return JS_ThrowRangeError(context, "WebAssembly memory is too large");
    if (instance->exported_memory != NULL
        && instance->exported_memory != memory) {
        return JS_ThrowRangeError(
            context, "multiple WebAssembly memories are not supported");
    }
    JSValue memory_object = JS_NewObject(context);
    JSValue buffer = JS_IsUndefined(instance->memory_buffer)
        ? wasm_new_array_buffer_copy(
              context, instance->budget,
              wasm_memory_get_base_address(memory), bytes)
        : JS_DupValue(context, instance->memory_buffer);
    JSValue retained = instance->exported_memory == NULL
        ? JS_DupValue(context, buffer) : JS_UNDEFINED;
    if (JS_IsException(memory_object) || JS_IsException(buffer)) {
        JS_FreeValue(context, memory_object);
        JS_FreeValue(context, buffer);
        JS_FreeValue(context, retained);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(context, memory_object, "buffer", buffer) < 0) {
        JS_FreeValue(context, retained);
        JS_FreeValue(context, memory_object);
        return JS_EXCEPTION;
    }
    if (instance->exported_memory == NULL) {
        instance->exported_memory = memory;
        instance->memory_bytes = bytes;
        instance->memory_buffer = retained;
        instance->memory_object = JS_DupValue(context, memory_object);
    }
    if (JS_DefinePropertyValueStr(
            context, memory_object, "__tilefinchWasmMemory",
            JS_NewBool(context, true), 0) < 0) {
        JS_FreeValue(context, memory_object);
        return JS_EXCEPTION;
    }
    if (JS_DefinePropertyValueStr(
            context, memory_object, "__tilefinchWasmInstance",
            JS_DupValue(context, instance_object), 0) < 0) {
        JS_FreeValue(context, memory_object);
        return JS_EXCEPTION;
    }
    return memory_object;
}

static const char *wasm_global_type_name(wasm_valkind_t kind)
{
    switch (kind) {
    case WASM_I32: return "i32";
    case WASM_I64: return "i64";
    case WASM_F32: return "f32";
    case WASM_F64: return "f64";
    default: return NULL;
    }
}

static JSValue wasm_global_export(
    JSContext *context, TilefinchWasmInstance *instance,
    JSValueConst instance_object, const char *name)
{
    wasm_global_inst_t global;
    memset(&global, 0, sizeof(global));
    if (!wasm_runtime_get_export_global_inst(
            instance->instance, name, &global)) return JS_UNDEFINED;
    const char *type = wasm_global_type_name(global.kind);
    if (type == NULL || global.global_data == NULL) {
        return JS_ThrowTypeError(
            context, "unsupported WebAssembly global type");
    }
    JSValue object = JS_NewObject(context);
    if (JS_IsException(object)) return JS_EXCEPTION;
    bool okay = JS_DefinePropertyValueStr(
            context, object, "__tilefinchWasmGlobal",
            JS_NewBool(context, true), 0) >= 0
        && JS_DefinePropertyValueStr(
            context, object, "__tilefinchWasmInstance",
            JS_DupValue(context, instance_object), 0) >= 0
        && JS_DefinePropertyValueStr(
            context, object, "__tilefinchWasmName",
            JS_NewString(context, name), 0) >= 0
        && JS_DefinePropertyValueStr(
            context, object, "__tilefinchWasmType",
            JS_NewString(context, type), 0) >= 0
        && JS_DefinePropertyValueStr(
            context, object, "__tilefinchWasmMutable",
            JS_NewBool(context, global.is_mutable), 0) >= 0;
    if (!okay) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    return object;
}

JSValue js_wasm_global_get(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2)
        return JS_ThrowTypeError(context, "WebAssembly.Global expected");
    const char *name = JS_ToCString(context, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    TilefinchWasmInstance *instance = JS_GetOpaque2(
        context, argv[0], wasm_instance_class_id);
    if (instance == NULL) {
        JS_FreeCString(context, name);
        return JS_EXCEPTION;
    }
    wasm_global_inst_t global;
    memset(&global, 0, sizeof(global));
    bool found = wasm_runtime_get_export_global_inst(
        instance->instance, name, &global);
    JS_FreeCString(context, name);
    if (!found || global.global_data == NULL)
        return JS_ThrowTypeError(context, "WebAssembly.Global expected");
    if (global.kind == WASM_I32) {
        int32_t value = 0;
        memcpy(&value, global.global_data, sizeof(value));
        return JS_NewInt32(context, value);
    }
    if (global.kind == WASM_I64) {
        int64_t value = 0;
        memcpy(&value, global.global_data, sizeof(value));
        return JS_NewBigInt64(context, value);
    }
    if (global.kind == WASM_F32) {
        float value = 0;
        memcpy(&value, global.global_data, sizeof(value));
        return JS_NewFloat64(context, value);
    }
    if (global.kind == WASM_F64) {
        double value = 0;
        memcpy(&value, global.global_data, sizeof(value));
        return JS_NewFloat64(context, value);
    }
    return JS_ThrowTypeError(context, "unsupported WebAssembly global type");
}

JSValue js_wasm_global_set(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 3)
        return JS_ThrowTypeError(context, "WebAssembly.Global expected");
    const char *name = JS_ToCString(context, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    TilefinchWasmInstance *instance = JS_GetOpaque2(
        context, argv[0], wasm_instance_class_id);
    if (instance == NULL) {
        JS_FreeCString(context, name);
        return JS_EXCEPTION;
    }
    wasm_global_inst_t before;
    memset(&before, 0, sizeof(before));
    if (!wasm_runtime_get_export_global_inst(
            instance->instance, name, &before)
        || before.global_data == NULL) {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "WebAssembly.Global expected");
    }
    if (!before.is_mutable) {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(context, "WebAssembly.Global is immutable");
    }
    wasm_val_t value;
    memset(&value, 0, sizeof(value));
    value.kind = before.kind;
    int converted = -1;
    if (before.kind == WASM_I32)
        converted = JS_ToInt32(context, &value.of.i32, argv[2]);
    else if (before.kind == WASM_I64)
        converted = JS_ToBigInt64(context, &value.of.i64, argv[2]);
    else if (before.kind == WASM_F32) {
        double number = 0;
        converted = JS_ToFloat64(context, &number, argv[2]);
        value.of.f32 = (float) number;
    } else if (before.kind == WASM_F64)
        converted = JS_ToFloat64(context, &value.of.f64, argv[2]);
    else {
        JS_FreeCString(context, name);
        return JS_ThrowTypeError(
            context, "unsupported WebAssembly global type");
    }
    if (converted < 0) {
        JS_FreeCString(context, name);
        return JS_EXCEPTION;
    }

    /* Numeric coercion above can run author code. Re-resolve the opaque
       instance and its exported global only after that code has returned. */
    instance = JS_GetOpaque2(context, argv[0], wasm_instance_class_id);
    wasm_global_inst_t after;
    memset(&after, 0, sizeof(after));
    if (instance == NULL
        || !wasm_runtime_get_export_global_inst(
               instance->instance, name, &after)
        || after.global_data == NULL || !after.is_mutable
        || after.kind != value.kind) {
        JS_FreeCString(context, name);
        if (instance == NULL) return JS_EXCEPTION;
        return JS_ThrowTypeError(context, "WebAssembly.Global is unavailable");
    }
    JS_FreeCString(context, name);
    switch (value.kind) {
    case WASM_I32:
        memcpy(after.global_data, &value.of.i32, sizeof(value.of.i32));
        break;
    case WASM_I64:
        memcpy(after.global_data, &value.of.i64, sizeof(value.of.i64));
        break;
    case WASM_F32:
        memcpy(after.global_data, &value.of.f32, sizeof(value.of.f32));
        break;
    case WASM_F64:
        memcpy(after.global_data, &value.of.f64, sizeof(value.of.f64));
        break;
    default:
        return JS_ThrowTypeError(
            context, "unsupported WebAssembly global type");
    }
    return JS_UNDEFINED;
}

JSValue js_wasm_memory_grow(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2)
        return JS_ThrowTypeError(context, "WebAssembly.Memory expected");
    TilefinchWasmInstance *instance = JS_GetOpaque2(
        context, argv[0], wasm_instance_class_id);
    if (instance == NULL) return JS_EXCEPTION;
    if (instance->exported_memory == NULL)
        return JS_ThrowTypeError(context, "WebAssembly.Memory expected");
    uint32_t delta = 0;
    if (JS_ToUint32(context, &delta, argv[1]) < 0) return JS_EXCEPTION;
    size_t old_bytes = 0;
    if (!wasm_memory_extent(instance->exported_memory, &old_bytes))
        return JS_ThrowRangeError(context, "WebAssembly memory is too large");
    uint32_t old_pages = (uint32_t) (old_bytes / 65536u);
    if (wasm_trace_enabled()) {
        fprintf(stderr,
                "tilefinch-wasm: memory grow current=%lu max=%llu delta=%lu\n",
                (unsigned long) old_pages,
                (unsigned long long) wasm_memory_get_max_page_count(
                    instance->exported_memory),
                (unsigned long) delta);
    }
    if (delta > TILEFINCH_WASM_MAX_MEMORY_PAGES - old_pages)
        return JS_ThrowRangeError(context, "WebAssembly memory limit exceeded");
    if (!wasm_memory_sync_to_runtime(context, instance))
        return JS_EXCEPTION;
    if (delta != 0
        && !wasm_memory_enlarge(instance->exported_memory, delta)) {
        const char *detail = wasm_runtime_get_exception(instance->instance);
        return JS_ThrowRangeError(
            context, "WebAssembly memory grow failed%s%s",
            detail != NULL && detail[0] != '\0' ? ": " : "",
            detail == NULL ? "" : detail);
    }
    if (!wasm_memory_sync_to_script(context, instance, true))
        return JS_EXCEPTION;
    return JS_NewUint32(context, old_pages);
}

JSValue js_wasm_available(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    if (wasm_trace_enabled())
        fprintf(stderr, "tilefinch-wasm: namespace requested\n");
#if defined(TILEFINCH_HAVE_WAMR_COMPONENT)
    if (!wasm_component_load()) return JS_NewBool(context, false);
#endif
    return JS_NewBool(context, true);
}

/* WAMR may retain or rewrite portions of the input while loading it.  Always
   validate a private Budget-owned copy, then discard it before returning to
   author code.  This also makes ArrayBuffer mutation during later module use
   irrelevant and keeps the validation allocation explicitly bounded. */
static int wasm_validate_source(
    JSContext *context, JSValueConst source_value, bool oversize_is_error,
    const uint8_t **source_out, size_t *source_length_out,
    JSValue *source_owner_out, char *error, size_t error_capacity)
{
    const uint8_t *source = NULL;
    size_t source_length = 0;
    JSValue source_owner = JS_UNDEFINED;
    if (!wasm_buffer_source(
            context, source_value, &source, &source_length, &source_owner)) {
        JS_ThrowTypeError(
            context, "WebAssembly source must be an ArrayBuffer or view");
        return -1;
    }
    if (source_length > TILEFINCH_WASM_MODULE_BYTES) {
        JS_FreeValue(context, source_owner);
        if (oversize_is_error) {
            JS_ThrowRangeError(
                context, "WebAssembly module exceeds Tilefinch limits");
            return -1;
        }
        return 0;
    }

    /* No author-code coercion occurs after this point, so resolving the realm
       bridge here cannot be invalidated by a hostile getter. */
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->budget == NULL) {
        JS_FreeValue(context, source_owner);
        JS_ThrowInternalError(context, "WebAssembly runtime is detached");
        return -1;
    }
    if (!wasm_runtime_enter(bridge->budget)) {
        JS_FreeValue(context, source_owner);
        JS_ThrowOutOfMemory(context);
        return -1;
    }
    uint8_t *validation_copy = budget_malloc_category(
        bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, source_length);
    if (validation_copy == NULL) {
        JS_FreeValue(context, source_owner);
        wasm_runtime_leave();
        JS_ThrowOutOfMemory(context);
        return -1;
    }
    memcpy(validation_copy, source, source_length);
    wasm_module_t candidate = wasm_load_for_inspection(
        validation_copy, source_length, error, error_capacity);
    bool valid = candidate != NULL;
    if (candidate != NULL) wasm_runtime_unload(candidate);
    budget_free(bridge->budget, validation_copy);
    wasm_runtime_leave();
    if (!valid) {
        JS_FreeValue(context, source_owner);
        return 0;
    }
    *source_out = source;
    *source_length_out = source_length;
    *source_owner_out = source_owner;
    return 1;
}

JSValue js_wasm_compile(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "WebAssembly source is required");
    const uint8_t *source = NULL;
    size_t source_length = 0;
    JSValue source_owner = JS_UNDEFINED;
    char error[192] = {0};
    int valid = wasm_validate_source(
        context, argv[0], true, &source, &source_length, &source_owner,
        error, sizeof(error));
    if (valid < 0) return JS_EXCEPTION;
    if (valid == 0)
        return wasm_throw_runtime(context, "WebAssembly compile failed", error);

    DomBridge *bridge = JS_GetContextOpaque(context);
    TilefinchWasmModule *module = budget_calloc_category(
        bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, 1u, sizeof(*module));
    if (module != NULL) {
        module->bytes = budget_malloc_category(
            bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, source_length);
    }
    if (module == NULL || module->bytes == NULL) {
        if (module != NULL) budget_free(bridge->budget, module);
        JS_FreeValue(context, source_owner);
        return JS_ThrowOutOfMemory(context);
    }
    module->budget = bridge->budget;
    module->length = source_length;
    memcpy(module->bytes, source, source_length);
    JS_FreeValue(context, source_owner);

    JSValue handle = JS_NewObjectClass(context, wasm_module_class_id);
    if (JS_IsException(handle)) {
        budget_free(module->budget, module->bytes);
        budget_free(module->budget, module);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(handle, module);
    return handle;
}

JSValue js_wasm_validate(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "WebAssembly source is required");
    const uint8_t *source = NULL;
    size_t source_length = 0;
    JSValue source_owner = JS_UNDEFINED;
    char error[192] = {0};
    int valid = wasm_validate_source(
        context, argv[0], false, &source, &source_length, &source_owner,
        error, sizeof(error));
    if (valid < 0) return JS_EXCEPTION;
    if (valid > 0) JS_FreeValue(context, source_owner);
    return JS_NewBool(context, valid > 0);
}

static JSValue wasm_module_custom_sections(
    JSContext *context, const TilefinchWasmModule *module,
    const char *requested, size_t requested_length)
{
    JSValue result = JS_NewArray(context);
    if (JS_IsException(result)) return JS_EXCEPTION;
    const uint8_t *bytes = module->bytes;
    size_t length = module->length;
    size_t cursor = 8u;
    uint32_t match_count = 0;
    unsigned section_count = 0;
    while (cursor < length && section_count++ < 128u) {
        uint8_t id = bytes[cursor++];
        uint32_t section_length = 0;
        if (!wasm_read_u32_leb(bytes, length, &cursor, &section_length)
            || section_length > length - cursor) {
            JS_FreeValue(context, result);
            return JS_ThrowInternalError(
                context, "invalid compiled WebAssembly module");
        }
        size_t section_end = cursor + section_length;
        if (id == 0) {
            uint32_t name_length = 0;
            if (!wasm_read_u32_leb(
                    bytes, section_end, &cursor, &name_length)
                || name_length > section_end - cursor) {
                JS_FreeValue(context, result);
                return JS_ThrowInternalError(
                    context, "invalid WebAssembly custom section");
            }
            bool matches = name_length == requested_length
                && memcmp(bytes + cursor, requested, requested_length) == 0;
            cursor += name_length;
            if (matches) {
                if (match_count >= TILEFINCH_WASM_MAX_CUSTOM_SECTIONS) {
                    JS_FreeValue(context, result);
                    return JS_ThrowRangeError(
                        context, "too many WebAssembly custom sections");
                }
                JSValue copy = JS_NewArrayBufferCopy(
                    context, bytes + cursor, section_end - cursor);
                if (JS_IsException(copy)) {
                    JS_FreeValue(context, result);
                    return JS_EXCEPTION;
                }
                if (JS_SetPropertyUint32(
                        context, result, match_count++, copy) < 0) {
                    /* JS_SetPropertyUint32 consumes copy on failure. */
                    JS_FreeValue(context, result);
                    return JS_EXCEPTION;
                }
            }
        }
        cursor = section_end;
    }
    if (cursor != length) {
        JS_FreeValue(context, result);
        return JS_ThrowRangeError(
            context, "WebAssembly section count exceeds Tilefinch limits");
    }
    return result;
}

JSValue js_wasm_module_info(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 2)
        return JS_ThrowTypeError(context, "WebAssembly.Module expected");
    int32_t mode = -1;
    if (JS_ToInt32(context, &mode, argv[1]) < 0) return JS_EXCEPTION;
    const char *custom_name = NULL;
    size_t custom_name_length = 0;
    if (mode == 2) {
        if (argc < 3)
            return JS_ThrowTypeError(context, "custom section name required");
        custom_name = JS_ToCStringLen(
            context, &custom_name_length, argv[2]);
        if (custom_name == NULL) return JS_EXCEPTION;
    }
    TilefinchWasmModule *module = JS_GetOpaque2(
        context, argv[0], wasm_module_class_id);
    if (module == NULL) {
        JS_FreeCString(context, custom_name);
        return JS_EXCEPTION;
    }
    if (mode == 2) {
        JSValue result = wasm_module_custom_sections(
            context, module, custom_name, custom_name_length);
        JS_FreeCString(context, custom_name);
        return result;
    }
    if (mode != 0 && mode != 1)
        return JS_ThrowRangeError(context, "invalid WebAssembly module query");
    if (!wasm_runtime_enter(module->budget))
        return JS_ThrowOutOfMemory(context);
    uint8_t *copy = budget_malloc_category(
        module->budget, BUDGET_CATEGORY_JAVASCRIPT, module->length);
    if (copy == NULL) {
        wasm_runtime_leave();
        return JS_ThrowOutOfMemory(context);
    }
    memcpy(copy, module->bytes, module->length);
    char error[192] = {0};
    wasm_module_t inspected = wasm_load_for_inspection(
        copy, module->length, error, sizeof(error));
    if (inspected == NULL) {
        budget_free(module->budget, copy);
        wasm_runtime_leave();
        return wasm_throw_runtime(
            context, "WebAssembly module inspection failed", error);
    }
    int32_t count = mode == 0
        ? wasm_runtime_get_import_count(inspected)
        : wasm_runtime_get_export_count(inspected);
    if (count < 0 || count > (int32_t) TILEFINCH_WASM_MAX_EXPORTS) {
        wasm_runtime_unload(inspected);
        budget_free(module->budget, copy);
        wasm_runtime_leave();
        return JS_ThrowRangeError(
            context, "WebAssembly descriptor count exceeds Tilefinch limits");
    }
    JSValue result = JS_NewArray(context);
    for (int32_t i = 0; i < count && !JS_IsException(result); i++) {
        const char *module_name = NULL;
        const char *name = NULL;
        wasm_import_export_kind_t kind;
        if (mode == 0) {
            wasm_import_t imported;
            memset(&imported, 0, sizeof(imported));
            wasm_runtime_get_import_type(inspected, i, &imported);
            module_name = imported.module_name;
            name = imported.name;
            kind = imported.kind;
        } else {
            wasm_export_t exported;
            memset(&exported, 0, sizeof(exported));
            wasm_runtime_get_export_type(inspected, i, &exported);
            name = exported.name;
            kind = exported.kind;
        }
        const char *kind_name = wasm_external_kind_name(kind);
        if (name == NULL || kind_name == NULL
            || (mode == 0 && module_name == NULL)) {
            JS_FreeValue(context, result);
            result = JS_ThrowInternalError(
                context, "invalid WebAssembly descriptor");
            break;
        }
        JSValue descriptor = JS_NewObject(context);
        if (JS_IsException(descriptor)) {
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
            break;
        }
        bool descriptor_ready = (mode != 0
                || JS_SetPropertyStr(
                       context, descriptor, "module",
                       JS_NewString(context, module_name)) >= 0)
            && JS_SetPropertyStr(
                   context, descriptor, "name",
                   JS_NewString(context, name)) >= 0
            && JS_SetPropertyStr(
                   context, descriptor, "kind",
                   JS_NewString(context, kind_name)) >= 0;
        if (!descriptor_ready) {
            JS_FreeValue(context, descriptor);
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
            break;
        }
        /* JS_SetPropertyUint32 consumes descriptor on both success and
           failure, so the error path must not free it again. */
        if (JS_SetPropertyUint32(
                context, result, (uint32_t) i, descriptor) < 0) {
            JS_FreeValue(context, result);
            result = JS_EXCEPTION;
            break;
        }
    }
    wasm_runtime_unload(inspected);
    budget_free(module->budget, copy);
    wasm_runtime_leave();
    return result;
}

static bool wasm_prepare_function_imports(
    JSContext *context, TilefinchWasmInstance *instance,
    wasm_module_t inspected, JSValueConst import_object)
{
    int32_t import_count = wasm_runtime_get_import_count(inspected);
    if (import_count < 0) {
        wasm_throw_link(context, "could not inspect WebAssembly imports", NULL);
        return false;
    }
    if (import_count == 0) return true;
    if (import_count > (int32_t) TILEFINCH_WASM_MAX_IMPORTS) {
        JS_ThrowRangeError(context, "WebAssembly import count exceeds Tilefinch limits");
        return false;
    }
    size_t name_bytes = 0;
    for (int32_t i = 0; i < import_count; i++) {
        wasm_import_t imported;
        memset(&imported, 0, sizeof(imported));
        wasm_runtime_get_import_type(inspected, i, &imported);
        if (imported.kind != WASM_IMPORT_EXPORT_KIND_FUNC
            || imported.module_name == NULL || imported.name == NULL) {
            wasm_throw_link(
                context,
                "only JavaScript function imports are supported by Tilefinch",
                NULL);
            return false;
        }
        size_t module_length = strlen(imported.module_name) + 1u;
        size_t name_length = strlen(imported.name) + 1u;
        if (module_length > TILEFINCH_WASM_MAX_IMPORT_NAME_BYTES - name_bytes
            || name_length > TILEFINCH_WASM_MAX_IMPORT_NAME_BYTES
                               - name_bytes - module_length) {
            JS_ThrowRangeError(
                context, "WebAssembly import names exceed Tilefinch limits");
            return false;
        }
        name_bytes += module_length + name_length;
    }
    instance->imports = budget_calloc_category(
        instance->budget, BUDGET_CATEGORY_JAVASCRIPT,
        (size_t) import_count, sizeof(*instance->imports));
    instance->import_names = budget_malloc_category(
        instance->budget, BUDGET_CATEGORY_JAVASCRIPT, name_bytes);
    if (instance->imports == NULL || instance->import_names == NULL) {
        JS_ThrowOutOfMemory(context);
        return false;
    }
    instance->import_count = (uint32_t) import_count;
    char *name_at = instance->import_names;
    for (int32_t i = 0; i < import_count; i++) {
        TilefinchWasmImport *target = &instance->imports[i];
        target->function = JS_UNDEFINED;
        wasm_import_t imported;
        memset(&imported, 0, sizeof(imported));
        wasm_runtime_get_import_type(inspected, i, &imported);
        size_t module_length = strlen(imported.module_name) + 1u;
        size_t name_length = strlen(imported.name) + 1u;
        memcpy(name_at, imported.module_name, module_length);
        target->module_name = name_at;
        name_at += module_length;
        memcpy(name_at, imported.name, name_length);
        target->symbol.symbol = name_at;
        name_at += name_length;
        target->owner = instance;
        target->context = context;
        target->symbol.func_ptr = (void *) wasm_import_raw_callback;
        target->symbol.attachment = target;

        uint32_t parameter_count = wasm_func_type_get_param_count(
            imported.u.func_type);
        uint32_t result_count = wasm_func_type_get_result_count(
            imported.u.func_type);
        if (parameter_count > TILEFINCH_WASM_MAX_ARGUMENTS
            || result_count > 1u) {
            JS_ThrowRangeError(
                context, "WebAssembly import signature exceeds Tilefinch limits");
            return false;
        }
        target->parameter_count = (uint8_t) parameter_count;
        target->result_count = (uint8_t) result_count;
        size_t signature_at = 0;
        target->signature[signature_at++] = '(';
        for (uint32_t p = 0; p < parameter_count; p++) {
            wasm_valkind_t kind = wasm_func_type_get_param_valkind(
                imported.u.func_type, p);
            char encoded = wasm_signature_kind(kind);
            if (encoded == '\0') {
                JS_ThrowTypeError(
                    context, "unsupported WebAssembly import parameter type");
                return false;
            }
            target->parameter_types[p] = kind;
            target->signature[signature_at++] = encoded;
        }
        target->signature[signature_at++] = ')';
        if (result_count != 0) {
            target->result_type = wasm_func_type_get_result_valkind(
                imported.u.func_type, 0);
            char encoded = wasm_signature_kind(target->result_type);
            if (encoded == '\0') {
                JS_ThrowTypeError(
                    context, "unsupported WebAssembly import result type");
                return false;
            }
            target->signature[signature_at++] = encoded;
        }
        target->signature[signature_at] = '\0';
        target->symbol.signature = target->signature;

        JSValue namespace_object = JS_GetPropertyStr(
            context, import_object, target->module_name);
        if (JS_IsException(namespace_object)) return false;
        if (!JS_IsObject(namespace_object)) {
            JS_FreeValue(context, namespace_object);
            wasm_throw_link(
                context, "missing WebAssembly import namespace",
                target->module_name);
            return false;
        }
        target->function = JS_GetPropertyStr(
            context, namespace_object, target->symbol.symbol);
        JS_FreeValue(context, namespace_object);
        if (JS_IsException(target->function)) return false;
        if (!JS_IsFunction(context, target->function)) {
            wasm_throw_link(
                context, "WebAssembly function import is not callable",
                target->symbol.symbol);
            return false;
        }
    }
    for (uint32_t i = 0; i < instance->import_count; i++) {
        TilefinchWasmImport *imported = &instance->imports[i];
        if (!wasm_runtime_register_natives_raw(
                imported->module_name, &imported->symbol, 1u)) {
            wasm_throw_link(
                context, "could not bind WebAssembly function import",
                imported->symbol.symbol);
            return false;
        }
        imported->registered = true;
    }
    return true;
}

JSValue js_wasm_instantiate(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1)
        return JS_ThrowTypeError(context, "WebAssembly source is required");
    const uint8_t *source = NULL;
    size_t source_length = 0;
    JSValue source_owner = JS_UNDEFINED;
    TilefinchWasmModule *compiled = JS_GetOpaque(
        argv[0], wasm_module_class_id);
    if (compiled != NULL) {
        source = compiled->bytes;
        source_length = compiled->length;
    } else if (!wasm_buffer_source(
                   context, argv[0], &source, &source_length, &source_owner)) {
        return JS_ThrowTypeError(
            context, "WebAssembly source must be a Module or BufferSource");
    }
    if (source_length == 0 || source_length > TILEFINCH_WASM_MODULE_BYTES) {
        JS_FreeValue(context, source_owner);
        return JS_ThrowRangeError(
            context, "WebAssembly module exceeds Tilefinch limits");
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->budget == NULL) {
        JS_FreeValue(context, source_owner);
        return JS_ThrowInternalError(context, "WebAssembly runtime is detached");
    }
    if (!wasm_runtime_enter(bridge->budget)) {
        JS_FreeValue(context, source_owner);
        return JS_ThrowOutOfMemory(context);
    }

    TilefinchWasmInstance *instance = budget_calloc_category(
        bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, 1u, sizeof(*instance));
    if (instance != NULL) {
        instance->module_bytes = budget_malloc_category(
            bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, source_length);
    }
    if (instance == NULL || instance->module_bytes == NULL) {
        if (instance != NULL) budget_free(bridge->budget, instance);
        JS_FreeValue(context, source_owner);
        wasm_runtime_leave();
        return JS_ThrowOutOfMemory(context);
    }
    instance->budget = bridge->budget;
    instance->memory_buffer = JS_UNDEFINED;
    instance->memory_object = JS_UNDEFINED;
    instance->pending_import_exception = JS_UNDEFINED;
    instance->owns_runtime_operation = true;
    instance->module_length = source_length;
    memcpy(instance->module_bytes, source, source_length);
    JS_FreeValue(context, source_owner);

    uint8_t *inspection_bytes = budget_malloc_category(
        bridge->budget, BUDGET_CATEGORY_JAVASCRIPT, source_length);
    if (inspection_bytes == NULL) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return JS_ThrowOutOfMemory(context);
    }
    memcpy(inspection_bytes, instance->module_bytes, source_length);
    char error[192] = {0};
    wasm_module_t inspected = wasm_load_for_inspection(
        inspection_bytes, source_length, error, sizeof(error));
    if (inspected == NULL) {
        budget_free(bridge->budget, inspection_bytes);
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return wasm_throw_link(context, "WebAssembly link failed", error);
    }
    JSValueConst import_object = argc >= 2 ? argv[1] : JS_UNDEFINED;
    bool imports_ready = wasm_prepare_function_imports(
        context, instance, inspected, import_object);
    wasm_runtime_unload(inspected);
    budget_free(bridge->budget, inspection_bytes);
    if (!imports_ready) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return JS_EXCEPTION;
    }
    memset(error, 0, sizeof(error));
    instance->module = wasm_runtime_load(
        instance->module_bytes, (uint32_t) source_length,
        error, sizeof(error));
    if (instance->module == NULL) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return wasm_throw_link(context, "WebAssembly link failed", error);
    }
    uint32_t imported_start =
        tilefinch_wasm_imported_start_function_index(instance->module);
    /* WAMR copies the resolved function pointer, signature, and attachment
       into the loaded module. Remove the process-global registration now so
       another standards-compliant instance may bind the same import name to
       a different JavaScript function. The instance-owned symbol storage
       remains live until its module is unloaded. */
    for (uint32_t i = 0; i < instance->import_count; i++) {
        TilefinchWasmImport *imported = &instance->imports[i];
        if (imported->registered
            && !wasm_runtime_unregister_natives(
                   imported->module_name, &imported->symbol)) {
            wasm_instance_destroy(JS_GetRuntime(context), instance);
            return wasm_throw_link(
                context, "could not finalize WebAssembly import binding",
                imported->symbol.symbol);
        }
        imported->registered = false;
    }
    InstantiationArgs instantiate = {
        .default_stack_size = TILEFINCH_WASM_STACK_BYTES,
        .host_managed_heap_size = 0,
        .max_memory_pages = TILEFINCH_WASM_MAX_MEMORY_PAGES
    };
    instance->instance = wasm_runtime_instantiate_ex(
        instance->module, &instantiate, error, sizeof(error));
    if (instance->instance == NULL) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return wasm_throw_link(
            context, "WebAssembly instantiation failed", error);
    }
    if (imported_start != UINT32_MAX) {
        JSValue started = JS_Call(
            context, instance->imports[imported_start].function,
            JS_UNDEFINED, 0, NULL);
        if (JS_IsException(started)) {
            wasm_instance_destroy(JS_GetRuntime(context), instance);
            return JS_EXCEPTION;
        }
        JS_FreeValue(context, started);
    }
    instance->execution = wasm_runtime_create_exec_env(
        instance->instance, TILEFINCH_WASM_STACK_BYTES);
    if (instance->execution == NULL) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return JS_ThrowOutOfMemory(context);
    }

    int32_t export_count = wasm_runtime_get_export_count(instance->module);
    if (export_count < 0
        || export_count > (int32_t) TILEFINCH_WASM_MAX_EXPORTS) {
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return JS_ThrowRangeError(
            context, "WebAssembly export count exceeds Tilefinch limits");
    }
    JSValue instance_object = JS_NewObjectClass(
        context, wasm_instance_class_id);
    JSValue exports = JS_NewObject(context);
    if (JS_IsException(instance_object) || JS_IsException(exports)) {
        JS_FreeValue(context, instance_object);
        JS_FreeValue(context, exports);
        wasm_instance_destroy(JS_GetRuntime(context), instance);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(instance_object, instance);
    wasm_runtime_state.live_instances++;
    instance->counted_live = true;

    for (int32_t i = 0; i < export_count; i++) {
        wasm_export_t exported;
        memset(&exported, 0, sizeof(exported));
        wasm_runtime_get_export_type(instance->module, i, &exported);
        if (exported.name == NULL || exported.name[0] == '\0') continue;
        JSValue value = JS_UNDEFINED;
        if (exported.kind == WASM_IMPORT_EXPORT_KIND_FUNC) {
            wasm_function_inst_t function = wasm_runtime_lookup_function(
                instance->instance, exported.name);
            if (function == NULL) {
                value = wasm_throw_link(
                    context, "missing WebAssembly function export",
                    exported.name);
            } else {
                uint32_t parameter_count = wasm_func_get_param_count(
                    function, instance->instance);
                if (parameter_count > TILEFINCH_WASM_MAX_ARGUMENTS) {
                    value = JS_ThrowRangeError(
                        context,
                        "WebAssembly function signature exceeds Tilefinch limits");
                }
                JSValue data[2] = {
                    JS_DupValue(context, instance_object),
                    JS_NewString(context, exported.name)
                };
                if (JS_IsUndefined(value) && JS_IsException(data[1]))
                    value = JS_EXCEPTION;
                if (JS_IsUndefined(value) && !JS_IsException(data[1])) {
                    value = JS_NewCFunctionData(
                        context, js_wasm_call_export,
                        (int) parameter_count, 0, 2, data);
                    uint32_t function_index =
                        tilefinch_wasm_export_function_index(
                            instance->module, exported.name);
                    if (!JS_IsException(value)
                        && function_index != UINT32_MAX) {
                        if (JS_DefinePropertyValueStr(
                                context, value, "__tilefinchWasmIndex",
                                JS_NewUint32(context, function_index), 0) < 0) {
                            JS_FreeValue(context, value);
                            value = JS_EXCEPTION;
                        }
                    }
                }
                JS_FreeValue(context, data[0]);
                JS_FreeValue(context, data[1]);
            }
        } else if (exported.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            value = wasm_memory_export(
                context, instance, instance_object, exported.name);
        } else if (exported.kind == WASM_IMPORT_EXPORT_KIND_GLOBAL) {
            value = wasm_global_export(
                context, instance, instance_object, exported.name);
        } else if (exported.kind == WASM_IMPORT_EXPORT_KIND_TABLE) {
            value = wasm_throw_link(
                context,
                "native WebAssembly table exports are unsupported by Tilefinch",
                exported.name);
        }
        if (JS_IsException(value)) {
            JS_FreeValue(context, exports);
            JS_FreeValue(context, instance_object);
            return JS_EXCEPTION;
        }
        if (!JS_IsUndefined(value)
            && JS_SetPropertyStr(
                   context, exports, exported.name, value) < 0) {
            /* JS_SetPropertyStr consumes value even when it fails. */
            JS_FreeValue(context, exports);
            JS_FreeValue(context, instance_object);
            return JS_EXCEPTION;
        }
    }
    if (JS_SetPropertyStr(context, instance_object, "exports", exports) < 0) {
        JS_FreeValue(context, instance_object);
        return JS_EXCEPTION;
    }
    instance->owns_runtime_operation = false;
    wasm_runtime_leave();
    return instance_object;
}

#else

bool js_wasm_runtime_init(JSRuntime *runtime)
{
    return runtime != NULL;
}

JSValue js_wasm_available(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_NewBool(context, false);
}

JSValue js_wasm_compile(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_validate(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_NewBool(context, false);
}

JSValue js_wasm_instantiate(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_module_info(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_memory_grow(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_global_get(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_global_set(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

JSValue js_wasm_snapshot_source(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    return JS_ThrowInternalError(
        context, "WebAssembly component is unavailable");
}

#endif
