/* Lazy-webpack bundle tier: bounded LZ retention of factory sources and
   on-demand per-factory compilation for webpack-shaped bundles.  Split from
   js_runtime.c; shares the runtime internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void js_lazy_webpack_recover_failure(ScriptRuntime *runtime)
{
    if (runtime == NULL || !runtime->lazy_factory_recovery_pending) return;
    runtime->lazy_factory_recovery_pending = false;
    (void) script_runtime_collect_and_trim(runtime);
}

static void lazy_discard_pending_exception(JSContext *context)
{
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
}

static void lazy_trace_memory(ScriptRuntime *runtime, const char *phase,
                              const char *source_url, size_t source_bytes,
                              size_t factory_count)
{
    if (runtime == NULL
        || getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") == NULL) return;
    fprintf(stderr,
            "lazy-webpack-memory phase=%s url=\"%s\" source=%zu "
            "factories=%zu current=%zu remaining=%zu javascript=%zu "
            "resource=%zu session=%zu\n",
            phase == NULL ? "" : phase,
            source_url == NULL ? "" : source_url, source_bytes,
            factory_count, runtime->budget->current,
            budget_remaining(runtime->budget),
            runtime->budget->categories[BUDGET_CATEGORY_JAVASCRIPT].current,
            runtime->budget->categories[BUDGET_CATEGORY_RESOURCE].current,
            runtime->budget->categories[BUDGET_CATEGORY_SESSION].current);
}

static void lazy_bundle_release_source(ScriptLazyRuntimeBundle *bundle)
{
    if (bundle == NULL || bundle->source_lease == NULL) return;
    if (bundle->release_source != NULL) {
        bundle->release_source(bundle->source_lease);
    }
    bundle->source_lease = NULL;
    bundle->release_source = NULL;
    bundle->source = NULL;
    bundle->source_length = 0;
}

static void lazy_bundle_destroy(JSContext *context, Budget *budget,
                                ScriptLazyRuntimeBundle *bundle)
{
    if (bundle == NULL) return;
    if (context != NULL) {
        for (size_t i = 0; i < bundle->factory_count; i++) {
            JS_FreeValue(context, bundle->factories[i].compiled);
            JS_FreeValue(context, bundle->factories[i].wrapper);
        }
    }
    lazy_bundle_release_source(bundle);
    for (size_t i = 0; i < bundle->factory_count; i++) {
        budget_free(budget, bundle->factories[i].compressed_source);
    }
    budget_free(budget, bundle->factories);
    budget_free(budget, bundle->source_url);
    budget_free(budget, bundle);
}

void js_lazy_webpack_bundles_destroy(ScriptRuntime *runtime)
{
    if (runtime == NULL) return;
    ScriptLazyRuntimeBundle *bundle = runtime->lazy_webpack_bundles;
    runtime->lazy_webpack_bundles = NULL;
    while (bundle != NULL) {
        ScriptLazyRuntimeBundle *next = bundle->next;
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        bundle = next;
    }
}

static ScriptLazyRuntimeBundle *lazy_bundle_find(ScriptRuntime *runtime,
                                                  uint32_t identifier)
{
    for (ScriptLazyRuntimeBundle *bundle = runtime->lazy_webpack_bundles;
         bundle != NULL; bundle = bundle->next) {
        if (bundle->identifier == identifier) return bundle;
    }
    return NULL;
}

#define SCRIPT_LAZY_LZ_HASH_BITS 14u
#define SCRIPT_LAZY_LZ_HASH_SIZE (1u << SCRIPT_LAZY_LZ_HASH_BITS)

static uint32_t lazy_lz_word(const unsigned char *source)
{
    uint32_t word;
    memcpy(&word, source, sizeof(word));
    return word;
}

static size_t lazy_lz_hash(uint32_t word)
{
    return (size_t) ((word * UINT32_C(2654435761))
                     >> (32u - SCRIPT_LAZY_LZ_HASH_BITS));
}

static bool lazy_lz_emit_length(unsigned char *output, size_t capacity,
                                size_t *at, size_t length)
{
    while (length >= 255) {
        if (*at >= capacity) return false;
        output[(*at)++] = 255;
        length -= 255;
    }
    if (*at >= capacity) return false;
    output[(*at)++] = (unsigned char) length;
    return true;
}

/* A small independent LZ4-compatible block encoder keeps cold factory source
   compact without adding a PSP runtime dependency. Blocks are per factory,
   so first-call compilation and Function#toString never inflate a complete
   multi-megabyte bundle. */
static size_t lazy_lz_compress_block(
    const unsigned char *source, size_t length,
    unsigned char *output, size_t capacity, uint32_t *table)
{
    memset(table, 0, SCRIPT_LAZY_LZ_HASH_SIZE * sizeof(*table));
    size_t anchor = 0, input = 0, out = 0;
    while (input + 4 <= length) {
        size_t hash = lazy_lz_hash(lazy_lz_word(source + input));
        uint32_t encoded = table[hash];
        table[hash] = input < UINT32_MAX ? (uint32_t) input + 1u : 0;
        if (encoded == 0) {
            input++;
            continue;
        }
        size_t match = (size_t) encoded - 1u;
        if (match >= input || input - match > UINT16_MAX
            || match + 4 > length
            || memcmp(source + match, source + input, 4) != 0) {
            input++;
            continue;
        }
        size_t matched = 4;
        while (input + matched < length
               && source[match + matched] == source[input + matched]) {
            matched++;
        }
        if (out >= capacity) return 0;
        size_t token_at = out++;
        size_t literals = input - anchor;
        unsigned token = (unsigned) (literals < 15 ? literals : 15) << 4;
        if (literals >= 15
            && !lazy_lz_emit_length(output, capacity, &out,
                                    literals - 15)) return 0;
        if (literals > capacity - out) return 0;
        memcpy(output + out, source + anchor, literals);
        out += literals;
        if (capacity - out < 2) return 0;
        size_t distance = input - match;
        output[out++] = (unsigned char) distance;
        output[out++] = (unsigned char) (distance >> 8);
        size_t match_code = matched - 4;
        token |= (unsigned) (match_code < 15 ? match_code : 15);
        if (match_code >= 15
            && !lazy_lz_emit_length(output, capacity, &out,
                                    match_code - 15)) return 0;
        output[token_at] = (unsigned char) token;
        input += matched;
        anchor = input;
    }
    size_t literals = length - anchor;
    if (out >= capacity) return 0;
    size_t token_at = out++;
    output[token_at] = (unsigned char) ((literals < 15 ? literals : 15) << 4);
    if (literals >= 15
        && !lazy_lz_emit_length(output, capacity, &out,
                                literals - 15)) return 0;
    if (literals > capacity - out) return 0;
    memcpy(output + out, source + anchor, literals);
    return out + literals;
}

static bool lazy_lz_read_length(const unsigned char *input,
                                size_t input_length, size_t *input_at,
                                size_t *length)
{
    for (;;) {
        if (*input_at >= input_length) return false;
        unsigned value = input[(*input_at)++];
        if (*length > SIZE_MAX - value) return false;
        *length += value;
        if (value != 255) return true;
    }
}

static bool lazy_lz_decompress_block(
    const unsigned char *input, size_t input_length,
    unsigned char *output, size_t output_length)
{
    size_t in = 0, out = 0;
    while (in < input_length) {
        unsigned token = input[in++];
        size_t literals = token >> 4;
        if (literals == 15
            && !lazy_lz_read_length(
                input, input_length, &in, &literals)) return false;
        if (literals > input_length - in
            || literals > output_length - out) return false;
        memcpy(output + out, input + in, literals);
        in += literals;
        out += literals;
        if (in == input_length) return out == output_length;
        if (input_length - in < 2) return false;
        size_t distance = (size_t) input[in]
                        | (size_t) input[in + 1] << 8;
        in += 2;
        if (distance == 0 || distance > out) return false;
        size_t matched = token & 15u;
        if (matched == 15
            && !lazy_lz_read_length(
                input, input_length, &in, &matched)) return false;
        if (matched > SIZE_MAX - 4) return false;
        matched += 4;
        if (matched > output_length - out) return false;
        for (size_t i = 0; i < matched; i++) {
            output[out + i] = output[out + i - distance];
        }
        out += matched;
    }
    return out == output_length;
}

/* Verify a compressed image against its original without materializing a
   second full-size output. Match references point backward into bytes already
   checked in original, which is equivalent to comparing a decoded block. */
static bool lazy_lz_decompress_matches(
    const unsigned char *input, size_t input_length,
    const unsigned char *original, size_t original_length)
{
    size_t in = 0, out = 0;
    while (in < input_length) {
        unsigned token = input[in++];
        size_t literals = token >> 4;
        if (literals == 15
            && !lazy_lz_read_length(
                input, input_length, &in, &literals)) return false;
        if (literals > input_length - in
            || literals > original_length - out
            || memcmp(input + in, original + out, literals) != 0) {
            return false;
        }
        in += literals;
        out += literals;
        if (in == input_length) return out == original_length;
        if (input_length - in < 2) return false;
        size_t distance = (size_t) input[in]
                        | (size_t) input[in + 1] << 8;
        in += 2;
        if (distance == 0 || distance > out) return false;
        size_t matched = token & 15u;
        if (matched == 15
            && !lazy_lz_read_length(
                input, input_length, &in, &matched)) return false;
        if (matched > SIZE_MAX - 4) return false;
        matched += 4;
        if (matched > original_length - out) return false;
        for (size_t i = 0; i < matched; i++) {
            if (original[out + i] != original[out + i - distance]) {
                return false;
            }
        }
        out += matched;
    }
    return out == original_length;
}

static bool lazy_compress_exact_block(
    ScriptRuntime *runtime, const unsigned char *input, size_t input_length,
    uint32_t *table, unsigned char **stored, size_t *stored_length,
    size_t *stored_capacity)
{
    if (runtime == NULL || input == NULL || input_length == 0 || table == NULL
        || stored == NULL || *stored != NULL || stored_length == NULL
        || stored_capacity == NULL) return false;
    size_t overhead = input_length / 255u + 64u;
    if (input_length > SIZE_MAX - overhead) return false;
    size_t capacity = input_length + overhead;
    unsigned char *compressed = budget_malloc_category(
        runtime->budget, BUDGET_CATEGORY_RESOURCE, capacity);
    if (compressed == NULL) {
        budget_free(runtime->budget, compressed);
        return false;
    }
    size_t written = lazy_lz_compress_block(
        input, input_length, compressed, capacity, table);
    bool verified = written != 0
        && lazy_lz_decompress_matches(
            compressed, written, input, input_length);
    if (!verified) {
        budget_free(runtime->budget, compressed);
        return false;
    }
    unsigned char *trimmed = budget_realloc_category(
        runtime->budget, BUDGET_CATEGORY_RESOURCE, compressed, written);
    *stored = trimmed == NULL ? compressed : trimmed;
    *stored_length = written;
    *stored_capacity = trimmed == NULL ? capacity : written;
    return true;
}

static bool lazy_factory_store_source(
    ScriptRuntime *runtime, ScriptLazyRuntimeFactory *factory,
    const unsigned char *source, uint32_t *table)
{
    return factory != NULL && factory->source_length != 0
        && lazy_compress_exact_block(
            runtime, source, factory->source_length, table,
            &factory->compressed_source,
            &factory->compressed_source_length,
            &factory->compressed_source_capacity);
}

static bool lazy_factory_program(Budget *budget,
                                 const ScriptLazyRuntimeBundle *bundle,
                                 const ScriptLazyRuntimeFactory *factory,
                                 char **program, size_t *program_length)
{
    static const char strict_prefix[] = "\"use strict\";(\n";
    static const char loose_prefix[] = "(\n";
    const char *prefix = bundle->strict_mode ? strict_prefix : loose_prefix;
    size_t prefix_length = bundle->strict_mode
        ? sizeof(strict_prefix) - 1 : sizeof(loose_prefix) - 1;
    bool raw_available = bundle->source != NULL
        && factory->source_offset <= bundle->source_length
        && factory->source_length
               <= bundle->source_length - factory->source_offset;
    bool compressed_available = factory->compressed_source != NULL
        && factory->compressed_source_length != 0;
    if ((!raw_available && !compressed_available)
        || factory->source_length > SIZE_MAX - prefix_length - 2) {
        return false;
    }
    size_t length = prefix_length + factory->source_length + 1;
    char *copy = budget_malloc(budget, length + 1);
    if (copy == NULL) return false;
    memcpy(copy, prefix, prefix_length);
    bool restored = raw_available;
    if (raw_available) {
        memcpy(copy + prefix_length,
               bundle->source + factory->source_offset,
               factory->source_length);
    } else {
        restored = lazy_lz_decompress_block(
            factory->compressed_source,
            factory->compressed_source_length,
            (unsigned char *) copy + prefix_length,
            factory->source_length);
    }
    if (!restored) {
        budget_free(budget, copy);
        return false;
    }
    copy[length - 1] = ')';
    copy[length] = '\0';
    *program = copy;
    *program_length = length;
    return true;
}

/* Every factory retains exact compressed source, so a cold toString query
   need not instantiate compiler state just to recover author text. */
static JSValue lazy_factory_source_string(
    JSContext *context, ScriptRuntime *runtime,
    const ScriptLazyRuntimeBundle *bundle,
    const ScriptLazyRuntimeFactory *factory)
{
    if (bundle->source != NULL
        && factory->source_offset <= bundle->source_length
        && factory->source_length
               <= bundle->source_length - factory->source_offset) {
        return JS_NewStringLen(
            context, bundle->source + factory->source_offset,
            factory->source_length);
    }
    if (factory->compressed_source == NULL
        || factory->compressed_source_length == 0) return JS_UNDEFINED;
    unsigned char *source = budget_malloc(
        runtime->budget, factory->source_length);
    if (source == NULL
        || !lazy_lz_decompress_block(
               factory->compressed_source,
               factory->compressed_source_length,
               source, factory->source_length)) {
        budget_free(runtime->budget, source);
        return JS_ThrowOutOfMemory(context);
    }
    JSValue text = JS_NewStringLen(
        context, (const char *) source, factory->source_length);
    budget_free(runtime->budget, source);
    return text;
}

static bool lazy_factory_prepare_compile_working_set(
    ScriptRuntime *runtime, const ScriptLazyRuntimeBundle *bundle,
    size_t factory_index, const ScriptLazyRuntimeFactory *factory)
{
    if (runtime == NULL || factory == NULL
        || factory->source_length
               > (SIZE_MAX - SCRIPT_LAZY_COMPILE_FIXED_HEAP_BYTES)
                    / SCRIPT_LAZY_COMPILE_HEAP_MULTIPLIER
        || factory->source_length > SIZE_MAX - 32u) return false;
    size_t program_bytes = factory->source_length + 32u;
    size_t heap_working_bytes =
        factory->source_length * SCRIPT_LAZY_COMPILE_HEAP_MULTIPLIER
        + SCRIPT_LAZY_COMPILE_FIXED_HEAP_BYTES;
    if (heap_working_bytes
            > SIZE_MAX - SCRIPT_LAZY_COMPILE_EXECUTION_RESERVE_BYTES
        || program_bytes > SIZE_MAX - heap_working_bytes
        || program_bytes + heap_working_bytes
               > SIZE_MAX - SCRIPT_LAZY_COMPILE_PAGE_RESERVE_BYTES) {
        return false;
    }
    size_t heap_required = heap_working_bytes
        + SCRIPT_LAZY_COMPILE_EXECUTION_RESERVE_BYTES;
    size_t page_required = program_bytes + heap_working_bytes
        + SCRIPT_LAZY_COMPILE_PAGE_RESERVE_BYTES;
    bool page_pressure = budget_remaining(runtime->budget) < page_required;
    bool heap_pressure = script_runtime_heap_remaining(runtime) < heap_required;
    size_t reclaimed = 0;
    if (page_pressure || heap_pressure) {
        size_t before = budget_remaining(runtime->budget);
        (void) script_runtime_collect_and_trim(runtime);
        size_t after = budget_remaining(runtime->budget);
        reclaimed = after > before ? after - before : 0;
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_compile_pressure_collections, 1);
    }
    if (budget_remaining(runtime->budget) < page_required
        && runtime->session != NULL
        && runtime->session->budget == runtime->budget) {
        size_t needed = page_required - budget_remaining(runtime->budget);
        size_t cache_reclaimed = browser_session_cache_reclaim(
            runtime->session, needed);
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_compile_cache_reclaimed_bytes,
            cache_reclaimed);
        js_rt_saturating_add_size(&reclaimed, cache_reclaimed);
    }
    page_pressure = budget_remaining(runtime->budget) < page_required;
    heap_pressure = script_runtime_heap_remaining(runtime) < heap_required;
    if (!page_pressure && !heap_pressure) {
        if (reclaimed != 0) {
            budget_record_pressure(runtime->budget,
                                   BUDGET_PRESSURE_JAVASCRIPT, 0,
                                   reclaimed);
        }
        return true;
    }
    js_rt_saturating_add_size(
        &runtime->result.lazy_webpack_compile_admission_rejections, 1);
    budget_record_pressure(runtime->budget, BUDGET_PRESSURE_JAVASCRIPT,
                           program_bytes + heap_working_bytes, reclaimed);
    if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
        fprintf(stderr,
                "lazy-webpack-factory-failed bundle=%u factory=%zu "
                "bytes=%zu page-required=%zu page-remaining=%zu "
                "heap-required=%zu heap-remaining=%zu stage=admission\n",
                (unsigned) bundle->identifier, factory_index,
                factory->source_length,
                page_required, budget_remaining(runtime->budget),
                heap_required, script_runtime_heap_remaining(runtime));
    }
    return false;
}

static JSValue lazy_factory_compile(JSContext *context,
                                    ScriptRuntime *runtime,
                                    ScriptLazyRuntimeBundle *bundle,
                                    size_t factory_index)
{
    ScriptLazyRuntimeFactory *factory = &bundle->factories[factory_index];
    if (!JS_IsUndefined(factory->compiled)) {
        return JS_DupValue(context, factory->compiled);
    }
    if (factory->compressed_source == NULL
        || factory->compressed_source_length == 0) {
        return JS_ThrowInternalError(context,
                                     "lazy factory image unavailable");
    }
    if (!lazy_factory_prepare_compile_working_set(
            runtime, bundle, factory_index, factory)) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_factory_compile_failures, 1);
        return JS_ThrowOutOfMemory(context);
    }
    char *program = NULL;
    size_t program_length = 0;
    if (!lazy_factory_program(runtime->budget, bundle, factory,
                              &program, &program_length)) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_factory_compile_failures, 1);
        return JS_ThrowOutOfMemory(context);
    }
    char name[256];
    snprintf(name, sizeof(name), "%s#factory-%zu",
             bundle->source_url == NULL ? "<external-script>"
                                        : bundle->source_url,
             factory_index);
    bool admitted = false;
    JSValue compiled = js_rt_compile_source_type(
        context, program, program_length, name, JS_EVAL_TYPE_GLOBAL,
        SCRIPT_COMPILE_SOURCE_LAZY_FACTORY, &runtime->result, &admitted);
    budget_free(runtime->budget, program);
    if (!admitted || JS_IsException(compiled)) {
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-factory-failed bundle=%u factory=%zu "
                    "bytes=%zu admitted=%s stage=compile\n",
                    (unsigned) bundle->identifier, factory_index,
                    factory->source_length, admitted ? "yes" : "no");
        }
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_factory_compile_failures, 1);
        if (!admitted) {
            return JS_ThrowRangeError(context, "%s", runtime->result.error);
        }
        return compiled;
    }
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_source_compiles, 1);
    if (!js_rt_runtime_script_checkpoint(runtime, 1)) {
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-factory-failed bundle=%u factory=%zu "
                    "bytes=%zu stage=checkpoint\n",
                    (unsigned) bundle->identifier, factory_index,
                    factory->source_length);
        }
        JS_FreeValue(context, compiled);
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_factory_compile_failures, 1);
        return JS_ThrowInternalError(context,
                                     "lazy factory compilation interrupted");
    }
    JSValue value = JS_EvalFunction(context, compiled);
    if (JS_IsException(value) || !JS_IsFunction(context, value)) {
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-factory-failed bundle=%u factory=%zu "
                    "bytes=%zu stage=evaluate\n",
                    (unsigned) bundle->identifier, factory_index,
                    factory->source_length);
        }
        if (!JS_IsException(value)) {
            JS_FreeValue(context, value);
            value = JS_ThrowTypeError(context,
                                      "lazy factory is not callable");
        }
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_factory_compile_failures, 1);
        return value;
    }
    factory->compiled = JS_DupValue(context, value);
    bundle->compiled_count++;
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_factories_compiled, 1);
    if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL
        && (bundle->compiled_count <= 8
            || (bundle->compiled_count & 63u) == 0
            || factory->source_length >= 64u * 1024u)) {
        fprintf(stderr,
                "lazy-webpack-factory-compiled bundle=%u factory=%zu "
                "bytes=%zu representation=%s compile-events=%zu "
                "factories=%zu\n",
                (unsigned) bundle->identifier, factory_index,
                factory->source_length,
                "source", bundle->compiled_count,
                bundle->factory_count);
    }
    /* Keep compact source after compilation: it preserves exact
       Function.prototype.toString behavior and permits a cold factory to be
       recompiled if author code invokes its stable wrapper again. */
    if (factory->source_length >= 64u * 1024u
        || (runtime->result.lazy_webpack_factories_compiled & 31u) == 0) {
        JS_RunGC(runtime->runtime);
    }
    /* A factory is intentionally a separate compile unit. Returning compiler
       scratch to the shared page budget here is what makes that separation a
       working-set reduction instead of an allocator-cache copy of eagerness. */
    (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
    return value;
}

static void lazy_factory_evict_compiled(ScriptRuntime *runtime,
                                        ScriptLazyRuntimeFactory *factory)
{
    if (runtime == NULL || factory == NULL
        || JS_IsUndefined(factory->compiled)) return;
    JS_FreeValue(runtime->context, factory->compiled);
    factory->compiled = JS_UNDEFINED;
    js_rt_saturating_add_size(
        &runtime->result.lazy_webpack_compiled_factory_evictions, 1);
}

static JSValue js_lazy_webpack_native_arrow(
    JSContext *context, JSValueConst this_value, int argc,
    JSValueConst *argv, int magic, JSValue *function_data)
{
    (void) this_value;
    (void) magic;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    uint32_t bundle_id = 0, factory_index = 0;
    if (runtime == NULL
        || JS_ToUint32(context, &bundle_id, function_data[0]) < 0
        || JS_ToUint32(context, &factory_index, function_data[1]) < 0) {
        return JS_ThrowTypeError(context, "invalid lazy factory reference");
    }
    ScriptLazyRuntimeBundle *bundle = lazy_bundle_find(runtime, bundle_id);
    if (bundle == NULL || factory_index >= bundle->factory_count
        || bundle->factories[factory_index].kind
               != SCRIPT_LAZY_FACTORY_ARROW) {
        return JS_ThrowTypeError(context, "unknown lazy arrow factory");
    }
    ScriptLazyRuntimeFactory *factory = &bundle->factories[factory_index];
    JSValue callable = lazy_factory_compile(
        context, runtime, bundle, factory_index);
    if (JS_IsException(callable)) {
        runtime->lazy_factory_recovery_pending = true;
        return callable;
    }
    uint64_t started_ns = js_rt_monotonic_time_ns();
    size_t polls_before = runtime->watchdog.polls;
    JSValue returned = JS_Call(context, callable, JS_UNDEFINED, argc, argv);
    js_rt_runtime_record_host_callback(runtime, "<lazy-webpack-factory>",
                                 started_ns, polls_before);
    lazy_factory_evict_compiled(runtime, factory);
    JS_FreeValue(context, callable);
    if (JS_IsException(returned)) {
        runtime->lazy_factory_recovery_pending = true;
    }
    return returned;
}

static JSValue js_lazy_webpack_invoke(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    uint32_t bundle_id = 0, factory_index = 0;
    if (runtime == NULL || argc < 3
        || JS_ToUint32(context, &bundle_id, argv[0]) < 0
        || JS_ToUint32(context, &factory_index, argv[1]) < 0) {
        return JS_ThrowTypeError(context, "invalid lazy factory reference");
    }
    ScriptLazyRuntimeBundle *bundle = lazy_bundle_find(runtime, bundle_id);
    if (bundle == NULL || factory_index >= bundle->factory_count) {
        return JS_ThrowTypeError(context, "unknown lazy factory reference");
    }
    ScriptLazyRuntimeFactory *factory = &bundle->factories[factory_index];
    JSValue callable = lazy_factory_compile(
        context, runtime, bundle, factory_index);
    if (JS_IsException(callable)) {
        runtime->lazy_factory_recovery_pending = true;
        return callable;
    }

    JSValue arguments[16];
    int argument_count = 0;
    int arguments_initialized = 0;
    if (factory->kind == SCRIPT_LAZY_FACTORY_FUNCTION) {
        if (argc != 4 || !JS_IsObject(argv[3])) {
            (void) JS_ThrowTypeError(context,
                                     "invalid lazy function arguments");
            goto invocation_failed;
        }
        JSValue length_value = JS_GetPropertyStr(context, argv[3], "length");
        uint32_t length = 0;
        if (JS_IsException(length_value)) goto invocation_failed;
        if (JS_ToUint32(context, &length, length_value) < 0) {
            JS_FreeValue(context, length_value);
            goto invocation_failed;
        }
        JS_FreeValue(context, length_value);
        if (length > sizeof(arguments) / sizeof(arguments[0])) {
            (void) JS_ThrowRangeError(
                context, "lazy function argument limit exceeded");
            goto invocation_failed;
        }
        argument_count = (int) length;
        for (int i = 0; i < argument_count; i++) {
            arguments[i] = JS_GetPropertyUint32(context, argv[3],
                                                (uint32_t) i);
            if (JS_IsException(arguments[i])) goto invocation_failed;
            arguments_initialized++;
        }
    } else {
        argument_count = argc - 3;
        if (argument_count < 0 || (size_t) argument_count
                > sizeof(arguments) / sizeof(arguments[0])) {
            (void) JS_ThrowRangeError(
                context, "lazy arrow argument limit exceeded");
            goto invocation_failed;
        }
        for (int i = 0; i < argument_count; i++) {
            arguments[i] = JS_DupValue(context, argv[i + 3]);
            arguments_initialized++;
        }
    }
    uint64_t started_ns = js_rt_monotonic_time_ns();
    size_t polls_before = runtime->watchdog.polls;
    JSValue returned = JS_Call(context, callable, argv[2], argument_count,
                               (JSValueConst *) arguments);
    js_rt_runtime_record_host_callback(runtime, "<lazy-webpack-factory>",
                                 started_ns, polls_before);
    /* Webpack itself caches module exports, not factory executions. Keeping
       every internal compiled factory after its call needlessly turns a
       lazy multi-megabyte chunk back into an eager bytecode working set.
       The author-visible wrapper remains stable and the compact exact source
       above makes an unusual repeat direct invocation semantically valid. */
    while (arguments_initialized > 0)
        JS_FreeValue(context, arguments[--arguments_initialized]);
    lazy_factory_evict_compiled(runtime, factory);
    JS_FreeValue(context, callable);
    if (JS_IsException(returned)) {
        runtime->lazy_factory_recovery_pending = true;
    }
    return returned;

invocation_failed:
    while (arguments_initialized > 0)
        JS_FreeValue(context, arguments[--arguments_initialized]);
    lazy_factory_evict_compiled(runtime, factory);
    JS_FreeValue(context, callable);
    return JS_EXCEPTION;
}

static JSValue js_lazy_webpack_wrap(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    uint32_t bundle_id = 0, factory_index = 0;
    if (runtime == NULL || (argc != 2 && argc != 3)
        || JS_ToUint32(context, &bundle_id, argv[0]) < 0
        || JS_ToUint32(context, &factory_index, argv[1]) < 0
        || (argc == 3 && !JS_IsFunction(context, argv[2]))) {
        return JS_ThrowTypeError(context, "invalid lazy factory wrapper");
    }
    ScriptLazyRuntimeBundle *bundle = lazy_bundle_find(runtime, bundle_id);
    if (bundle == NULL || factory_index >= bundle->factory_count) {
        return JS_ThrowTypeError(context, "unknown lazy factory wrapper");
    }
    ScriptLazyRuntimeFactory *factory = &bundle->factories[factory_index];
    if (!JS_IsUndefined(factory->wrapper) || bundle->source == NULL
        || (argc == 2 && factory->kind != SCRIPT_LAZY_FACTORY_ARROW)
        || (argc == 3 && factory->kind != SCRIPT_LAZY_FACTORY_FUNCTION)
        || factory->key_offset > bundle->source_length
        || factory->key_length
               > bundle->source_length - factory->key_offset) {
        return JS_ThrowTypeError(context, "duplicate lazy factory wrapper");
    }
    JSValue wrapper;
    if (argc == 2) {
        JSValueConst data[2] = {argv[0], argv[1]};
        wrapper = JS_NewCFunctionData(
            context, js_lazy_webpack_native_arrow,
            (int) factory->arity, 0, 2, data);
    } else {
        wrapper = JS_DupValue(context, argv[2]);
    }
    if (JS_IsException(wrapper)) return wrapper;
    JSValue name = JS_NewStringLen(
        context, bundle->source + factory->key_offset,
        factory->key_length);
    if (JS_IsException(name)
        || JS_DefinePropertyValueStr(
               context, wrapper, "name", name,
               JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, wrapper);
        return JS_EXCEPTION;
    }
    factory->wrapper = JS_DupValue(context, wrapper);
    return wrapper;
}

static JSValue js_lazy_function_to_string(JSContext *context,
                                          JSValueConst this_value,
                                          int argc, JSValueConst *argv)
{
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge == NULL ? NULL : bridge->host;
    if (runtime == NULL) {
        return JS_ThrowInternalError(context, "script runtime unavailable");
    }
    for (ScriptLazyRuntimeBundle *bundle = runtime->lazy_webpack_bundles;
         bundle != NULL; bundle = bundle->next) {
        for (size_t i = 0; i < bundle->factory_count; i++) {
            ScriptLazyRuntimeFactory *factory = &bundle->factories[i];
            if (!JS_IsUndefined(factory->wrapper)
                && JS_StrictEq(context, this_value, factory->wrapper) == 1) {
                if (!JS_IsUndefined(factory->compiled)) {
                    return JS_Call(context, runtime->function_to_string,
                                   factory->compiled, 0, NULL);
                }
                JSValue source_text = lazy_factory_source_string(
                    context, runtime, bundle, factory);
                if (!JS_IsUndefined(source_text)) return source_text;
                JSValue callable = lazy_factory_compile(
                    context, runtime, bundle, i);
                if (JS_IsException(callable)) {
                    runtime->lazy_factory_recovery_pending = true;
                    return callable;
                }
                JSValue text = JS_Call(
                    context, runtime->function_to_string,
                    callable, 0, NULL);
                lazy_factory_evict_compiled(runtime, factory);
                JS_FreeValue(context, callable);
                if (JS_IsException(text)) {
                    runtime->lazy_factory_recovery_pending = true;
                }
                return text;
            }
        }
    }
    if (!JS_IsFunction(context, runtime->function_to_string)) {
        return JS_ThrowInternalError(context,
                                     "Function#toString unavailable");
    }
    return JS_Call(context, runtime->function_to_string, this_value, 0, NULL);
}

bool js_lazy_webpack_install(ScriptRuntime *runtime,
                                        JSValue global)
{
    JSContext *context = runtime->context;
    JSValue function = JS_NewCFunction(
        context, js_lazy_webpack_invoke,
        "__tilefinchLazyWebpackInvoke", 6);
    if (JS_IsException(function)
        || JS_DefinePropertyValueStr(
               context, global, "__tilefinchLazyWebpackInvoke", function, 0)
               < 0) return false;
    function = JS_NewCFunction(
        context, js_lazy_webpack_wrap,
        "__tilefinchLazyWebpackWrap", 3);
    if (JS_IsException(function)
        || JS_DefinePropertyValueStr(
               context, global, "__tilefinchLazyWebpackWrap", function, 0)
               < 0) return false;
    JSValue constructor = JS_GetPropertyStr(context, global, "Function");
    JSValue prototype = JS_GetPropertyStr(context, constructor, "prototype");
    JSValue original = JS_GetPropertyStr(context, prototype, "toString");
    function = JS_NewCFunction(
        context, js_lazy_function_to_string, "toString", 0);
    bool ok = !JS_IsException(constructor) && !JS_IsException(prototype)
        && JS_IsFunction(context, original) && !JS_IsException(function)
        && JS_SetPropertyStr(context, prototype, "toString", function) >= 0;
    if (ok) runtime->function_to_string = original;
    else JS_FreeValue(context, original);
    JS_FreeValue(context, prototype);
    JS_FreeValue(context, constructor);
    return ok;
}

static size_t lazy_resident_source_limit(const ScriptRuntime *runtime)
{
    if (runtime == NULL || runtime->budget == NULL) return 0;
    size_t proportional = runtime->budget->limit / 4u;
    return proportional < SCRIPT_LAZY_MAX_RETAINED_SOURCE_BYTES
        ? proportional : SCRIPT_LAZY_MAX_RETAINED_SOURCE_BYTES;
}

/* QuickJS does not expose a grammar-only parser. Compile one isolated factory
   at a time and immediately release its compiler object, preserving external
   script syntax-error timing without retaining bytecode for cold factories. */
static bool lazy_preflight_factory_syntax(
    ScriptRuntime *runtime, ScriptLazyRuntimeBundle *bundle,
    size_t factory_index, ScriptLazyRuntimeFactory *factory)
{
    if (!lazy_factory_prepare_compile_working_set(
            runtime, bundle, factory_index, factory)) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_syntax_preflight_failures, 1);
        return false;
    }
    char *program = NULL;
    size_t program_length = 0;
    if (!lazy_factory_program(runtime->budget, bundle, factory,
                              &program, &program_length)) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_syntax_preflight_failures, 1);
        return false;
    }
    char name[256];
    snprintf(name, sizeof(name), "%s#preflight-factory-%zu",
             bundle->source_url == NULL ? "<external-script>"
                                        : bundle->source_url,
             factory_index);
    js_rt_saturating_add_size(
        &runtime->result.lazy_webpack_syntax_preflight_attempts, 1);
    js_rt_saturating_add_size(
        &runtime->result.lazy_webpack_syntax_preflight_source_bytes,
        factory->source_length);
    uint64_t started_ns = js_rt_monotonic_time_ns();
    bool admitted = false;
    JSValue compiled = js_rt_compile_source_type(
        runtime->context, program, program_length, name,
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_LAZY_FACTORY,
        &runtime->result, &admitted);
    uint64_t finished_ns = js_rt_monotonic_time_ns();
    budget_free(runtime->budget, program);
    uint64_t elapsed_us = finished_ns >= started_ns
        ? (finished_ns - started_ns) / UINT64_C(1000) : 0;
    js_rt_saturating_add_u64(
        &runtime->result.lazy_webpack_syntax_preflight_total_us,
        elapsed_us);
    if (!admitted || JS_IsException(compiled)) {
        if (JS_IsException(compiled)) {
            lazy_discard_pending_exception(runtime->context);
        } else {
            JS_FreeValue(runtime->context, compiled);
        }
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_syntax_preflight_failures, 1);
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-preflight-failed bundle=%u factory=%zu "
                    "bytes=%zu admitted=%s\n",
                    (unsigned) bundle->identifier, factory_index,
                    factory->source_length, admitted ? "yes" : "no");
        }
        return false;
    }
    JS_FreeValue(runtime->context, compiled);
    if (factory->source_length >= 64u * 1024u) {
        (void) script_runtime_collect_and_trim(runtime);
    } else if ((factory_index & 15u) == 15u) {
        (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
    }
    return true;
}

static bool lazy_store_factory_sources(
    ScriptRuntime *runtime, ScriptLazyRuntimeBundle *bundle)
{
    if (runtime == NULL || bundle == NULL || bundle->source == NULL)
        return false;
    size_t source_limit = lazy_resident_source_limit(runtime);
    runtime->result.lazy_webpack_resident_source_limit_bytes = source_limit;
    size_t resident_source = 0, resident_factories = 0, resident_bundles = 0;
    bool bounds_rejected = false;
    for (ScriptLazyRuntimeBundle *stored = runtime->lazy_webpack_bundles;
         stored != NULL; stored = stored->next) {
        if (resident_source > SIZE_MAX - stored->compressed_source_length
            || resident_factories > SIZE_MAX - stored->factory_count
            || resident_bundles == SIZE_MAX) {
            bounds_rejected = true;
            goto failed_without_table;
        }
        resident_source += stored->compressed_source_length;
        resident_factories += stored->factory_count;
        resident_bundles++;
    }
    if (resident_source > source_limit
        || resident_factories > SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT
        || bundle->factory_count
               > SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT - resident_factories
        || resident_bundles >= SCRIPT_LAZY_RESIDENT_BUNDLE_LIMIT) {
        bounds_rejected = true;
        goto failed_without_table;
    }
    uint32_t *table = budget_malloc(
        runtime->budget,
        SCRIPT_LAZY_LZ_HASH_SIZE * sizeof(*table));
    if (table == NULL) return false;
    for (size_t i = 0; i < bundle->factory_count; i++) {
        ScriptLazyRuntimeFactory *factory = &bundle->factories[i];
        bool cached = lazy_preflight_factory_syntax(
                runtime, bundle, i, factory)
            && factory->source_offset <= bundle->source_length
            && factory->source_length
                   <= bundle->source_length - factory->source_offset
            && lazy_factory_store_source(
                runtime, factory,
                (const unsigned char *) bundle->source
                    + factory->source_offset,
                table);
        if (!cached
            || bundle->compressed_source_length
                   > SIZE_MAX - factory->compressed_source_capacity) {
            budget_free(runtime->budget, table);
            return false;
        }
        bundle->compressed_source_length +=
            factory->compressed_source_capacity;
        if (bundle->compressed_source_length
                > source_limit - resident_source) {
            bounds_rejected = true;
            budget_free(runtime->budget, table);
            goto failed_without_table;
        }
        if ((i & 31u) == 31u
            && !js_rt_runtime_script_checkpoint(runtime, 1)) {
            budget_free(runtime->budget, table);
            return false;
        }
    }
    budget_free(runtime->budget, table);
    return true;

failed_without_table:
    if (bounds_rejected) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_residency_rejections, 1);
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-residency-rejected bundle=%u "
                    "resident-source=%zu candidate-source=%zu limit=%zu "
                    "resident-factories=%zu candidate-factories=%zu "
                    "resident-bundles=%zu\n",
                    (unsigned) bundle->identifier, resident_source,
                    bundle->compressed_source_length, source_limit,
                    resident_factories, bundle->factory_count,
                    resident_bundles);
        }
    }
    return false;
}

static uint32_t lazy_next_bundle_identifier(ScriptRuntime *runtime)
{
    do {
        runtime->next_lazy_webpack_bundle_id++;
        if (runtime->next_lazy_webpack_bundle_id == 0) {
            runtime->next_lazy_webpack_bundle_id++;
        }
    } while (lazy_bundle_find(
                 runtime, runtime->next_lazy_webpack_bundle_id) != NULL);
    return runtime->next_lazy_webpack_bundle_id;
}

static ScriptLazyRuntimeBundle *lazy_bundle_create(
    ScriptRuntime *runtime, uint32_t identifier,
    const char *source, size_t source_length, const char *source_url,
    const ScriptLazyWebpackPlan *plan)
{
    if (plan->factory_count == 0
        || plan->factory_count > SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT
        || plan->factory_count > SIZE_MAX / sizeof(ScriptLazyRuntimeFactory)) {
        return NULL;
    }
    ScriptLazyRuntimeBundle *bundle = budget_calloc(
        runtime->budget, 1, sizeof(*bundle));
    if (bundle == NULL) return NULL;
    bundle->factories = budget_calloc(
        runtime->budget, plan->factory_count,
        sizeof(*bundle->factories));
    size_t url_length = source_url == NULL ? 0 : strlen(source_url);
    if (url_length > 2047) url_length = 2047;
    bundle->source_url = budget_malloc(runtime->budget, url_length + 1);
    if (bundle->factories == NULL || bundle->source_url == NULL) {
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        return NULL;
    }
    if (url_length != 0) memcpy(bundle->source_url, source_url, url_length);
    bundle->source_url[url_length] = '\0';
    bundle->identifier = identifier;
    bundle->source = source;
    bundle->source_length = source_length;
    bundle->strict_mode = plan->strict_mode;
    bundle->factory_count = plan->factory_count;
    for (size_t i = 0; i < plan->factory_count; i++) {
        bundle->factories[i].source_offset =
            plan->factories[i].source_offset;
        bundle->factories[i].source_length =
            plan->factories[i].source_length;
        bundle->factories[i].arity = plan->factories[i].arity;
        bundle->factories[i].kind = plan->factories[i].kind;
        bundle->factories[i].key_offset = plan->factories[i].key_offset;
        bundle->factories[i].key_length = plan->factories[i].key_length;
        bundle->factories[i].compiled = JS_UNDEFINED;
        bundle->factories[i].wrapper = JS_UNDEFINED;
    }
    return bundle;
}

ScriptLazyEvaluation script_runtime_evaluate_external_lazy_webpack(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    const ScriptLazyWebpackPlan *plan,
    void *source_lease, ScriptSourceLeaseReleaseCallback release_source,
    ScriptResult *result)
{
    if (runtime == NULL || script_node == NULL || source == NULL
        || plan == NULL || plan->factory_count == 0
        || source_lease == NULL || release_source == NULL) {
        return SCRIPT_LAZY_EVALUATION_FALLBACK;
    }
    int64_t script_handle = js_rt_bridge_register_node(
        &runtime->bridge, script_node);
    if (script_handle == 0) return SCRIPT_LAZY_EVALUATION_FALLBACK;
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_candidates, 1);
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue page_trace = JS_GetPropertyStr(runtime->context, global,
                                           "__tilefinchPageTrace");
    bool page_diagnostic = JS_ToBool(runtime->context, page_trace) == 1;
    JS_FreeValue(runtime->context, page_trace);
    JS_FreeValue(runtime->context, global);
    if (page_diagnostic) {
        js_rt_saturating_add_size(&runtime->result.lazy_webpack_fallbacks, 1);
        js_rt_runtime_update_result(runtime, result);
        return SCRIPT_LAZY_EVALUATION_FALLBACK;
    }
    js_rt_runtime_arm_watchdog(runtime);
    if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
        fprintf(stderr,
                "lazy-webpack-candidate url=\"%s\" bytes=%zu factories=%zu "
                "factory-bytes=%zu largest=%zu strict=%s\n",
                source_url == NULL ? "" : source_url, source_length,
                plan->factory_count, plan->factory_source_bytes,
                plan->largest_factory_bytes,
                plan->strict_mode ? "yes" : "no");
    }
    lazy_trace_memory(runtime, "candidate", source_url, source_length,
                      plan->factory_count);
    uint32_t identifier = lazy_next_bundle_identifier(runtime);
    ScriptLazyRuntimeBundle *bundle = lazy_bundle_create(
        runtime, identifier, source, source_length, source_url, plan);
    size_t preflight_attempts_before =
        runtime->result.lazy_webpack_syntax_preflight_attempts;
    size_t preflight_bytes_before =
        runtime->result.lazy_webpack_syntax_preflight_source_bytes;
    uint64_t preflight_us_before =
        runtime->result.lazy_webpack_syntax_preflight_total_us;
    bool ready = bundle != NULL
        && lazy_store_factory_sources(runtime, bundle);
    if (ready) {
        /* Registration retains only exact, independently-compressed factory
           source. Grammar preflight compiler objects have already been freed;
           a callable is compiled again only when its stable wrapper is used. */
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-preflight bundle=%u attempts=%zu "
                    "source-bytes=%zu total-us=%llu\n",
                    (unsigned) identifier,
                    runtime->result.lazy_webpack_syntax_preflight_attempts
                        - preflight_attempts_before,
                    runtime->result.lazy_webpack_syntax_preflight_source_bytes
                        - preflight_bytes_before,
                    (unsigned long long)
                        (runtime->result
                             .lazy_webpack_syntax_preflight_total_us
                         - preflight_us_before));
        }
        lazy_trace_memory(runtime, "stored", source_url, source_length,
                          plan->factory_count);
    }
    char *rendered = NULL;
    size_t rendered_length = 0;
    JSValue compiled_registration = JS_UNDEFINED;
    if (ready) {
        ready = script_lazy_webpack_render(
            runtime->budget, source, source_length, plan, identifier,
            &rendered, &rendered_length);
    }
    bool admitted = false;
    if (ready) {
        compiled_registration = js_rt_compile_source_type(
            runtime->context, rendered, rendered_length,
            source_url == NULL ? "<external-script>" : source_url,
            JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_EXTERNAL,
            &runtime->result, &admitted);
        ready = admitted && !JS_IsException(compiled_registration);
        if (!ready && JS_IsException(compiled_registration)) {
            lazy_discard_pending_exception(runtime->context);
            compiled_registration = JS_UNDEFINED;
        }
    }
    budget_free(runtime->budget, rendered);
    if (!ready) {
        JS_FreeValue(runtime->context, compiled_registration);
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        js_rt_saturating_add_size(&runtime->result.lazy_webpack_fallbacks, 1);
        JS_RunGC(runtime->runtime);
        (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
        if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
            fprintf(stderr,
                    "lazy-webpack-fallback url=\"%s\" factories=%zu\n",
                    source_url == NULL ? "" : source_url,
                    plan->factory_count);
        }
        js_rt_runtime_update_result(runtime, result);
        return SCRIPT_LAZY_EVALUATION_FALLBACK;
    }
    if (!js_rt_runtime_script_checkpoint(runtime, 1)) {
        JS_FreeValue(runtime->context, compiled_registration);
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        js_rt_saturating_add_size(&runtime->result.lazy_webpack_fallbacks, 1);
        js_rt_runtime_update_result(runtime, result);
        return SCRIPT_LAZY_EVALUATION_FALLBACK;
    }
    ScriptCurrentScriptScope previous_current_script = {
        .value = JS_UNDEFINED
    };
    if (!js_rt_current_script_scope_begin(
            runtime->context, script_node, false,
            &previous_current_script, &runtime->result)) {
        JS_FreeValue(runtime->context, compiled_registration);
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        js_rt_saturating_add_size(&runtime->result.lazy_webpack_fallbacks, 1);
        js_rt_runtime_update_result(runtime, result);
        return SCRIPT_LAZY_EVALUATION_FALLBACK;
    }
    bundle->source_lease = source_lease;
    bundle->release_source = release_source;
    bundle->next = runtime->lazy_webpack_bundles;
    runtime->lazy_webpack_bundles = bundle;
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_applied, 1);
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_factories_deferred,
                        plan->factory_count);
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_factory_source_bytes,
                        plan->factory_source_bytes);
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_compressed_source_bytes,
                        bundle->compressed_source_length);
    if (getenv("TILEFINCH_TRACE_LAZY_SCRIPTS") != NULL) {
        fprintf(stderr,
                "lazy-webpack-applied url=\"%s\" bundle=%u "
                "source-bytes=%zu transformed-bytes=%zu factories=%zu "
                "compressed-source=%zu\n",
                source_url == NULL ? "" : source_url,
                (unsigned) identifier,
                source_length, rendered_length, plan->factory_count,
                bundle->compressed_source_length);
    }
    lazy_trace_memory(runtime, "registered", source_url, source_length,
                      plan->factory_count);

    BudgetAllocationOwner previous_owner =
        document_allocation_owner_enter(runtime->document);
    JSValue evaluated_value = JS_EvalFunction(
        runtime->context, compiled_registration);
    document_allocation_owner_leave(runtime->document, previous_owner);
    bool evaluated = !JS_IsException(evaluated_value);
    JS_FreeValue(runtime->context, evaluated_value);
    if (!js_rt_current_script_scope_end(
            runtime->context, &previous_current_script,
            &runtime->result)) evaluated = false;
    if (!evaluated) {
        js_rt_record_exception(runtime->context, &runtime->result);
        js_rt_capture_error_source_context(source, source_length, source_url,
                                     &runtime->result);
    }
    js_lazy_webpack_recover_failure(runtime);
    lazy_bundle_release_source(bundle);
    lazy_trace_memory(runtime, "source-released", source_url,
                      bundle->compressed_source_length,
                      plan->factory_count);
    if (evaluated) evaluated = js_rt_runtime_run_jobs(runtime);
    if (evaluated) evaluated = js_rt_runtime_refresh(runtime);
    lazy_trace_memory(runtime, "executed", source_url, source_length,
                      plan->factory_count);
    if (!evaluated) {
        (void) script_runtime_collect_and_trim(runtime);
        runtime->lazy_factory_recovery_pending = false;
        runtime->result.external_scripts_failed++;
        runtime->result.success = true;
        size_t slot = 0;
        if (js_rt_bridge_node_slot_for_handle(
                &runtime->bridge, script_handle, &slot)) {
            (void) script_runtime_dispatch_node(
                runtime, runtime->bridge.nodes[slot], "error", NULL);
        }
        js_rt_runtime_update_result(runtime, result);
        return SCRIPT_LAZY_EVALUATION_FAILED;
    }
    runtime->result.external_scripts_loaded++;
    runtime->result.external_script_bytes += source_length;
    size_t slot = 0;
    bool live = js_rt_bridge_node_slot_for_handle(
        &runtime->bridge, script_handle, &slot);
    bool dispatched = !live || script_runtime_dispatch_node(
        runtime, runtime->bridge.nodes[slot], "load", result);
    runtime->result.success = dispatched;
    js_rt_runtime_update_result(runtime, result);
    return dispatched ? SCRIPT_LAZY_EVALUATION_SUCCEEDED
                      : SCRIPT_LAZY_EVALUATION_FAILED;
}
