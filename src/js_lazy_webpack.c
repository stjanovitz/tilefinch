/* Lazy-webpack bundle tier: bounded LZ retention of factory sources and
   on-demand per-factory compilation for webpack-shaped bundles.  Split from
   js_runtime.c; shares the runtime internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/build_version.h"
#include "tilefinch/platform.h"
#include "fnv1a.h"
#include "tilefinch/script_lazy.h"
#include "tilefinch/sha256.h"
#include "tilefinch/url.h"
#include "js_source_store.h"
#include "script_text_lz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef TILEFINCH_QUICKJS_ENGINE_ID
#define TILEFINCH_QUICKJS_ENGINE_ID "unknown-engine"
#endif

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
        || !tilefinch_trace_lazy_scripts()) return;
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

#ifndef TILEFINCH_NO_TRACE
/* Factory-cache measurement (TILEFINCH_TRACE_FACTORY_CACHE, see
   diagnostic_trace.h and tools/site-census/factory_cache.py). Lab builds
   only: what caching compiled factories would hold and save. */
static uint64_t census_fnv64(const unsigned char *data, size_t length)
{
    return fnv1a64(FNV1A64_SEED, data, length);
}

static void census_lazy_function_stats(JSRuntime *js_runtime,
                                       uint64_t *compiled, uint64_t *bytes,
                                       uint64_t *deferred)
{
    *compiled = *bytes = *deferred = 0;
    JSLazyFunctionStats stats;
    JS_GetLazyFunctionStats(js_runtime, &stats);
    *compiled = stats.compiled;
    *bytes = stats.compiled_source_bytes;
    *deferred = stats.deferred;
}

/* The classic table's serialization (JS_WRITE_OBJ_BYTECODE); 0 when the
   program does not serialize. */
static size_t census_serialized_bytes(JSContext *context,
                                      JSValueConst program, uint64_t *us)
{
    uint64_t started_ns = js_rt_monotonic_time_ns();
    size_t length = 0;
    uint8_t *bytes = JS_WriteObject(context, &length, program,
                                    JS_WRITE_OBJ_BYTECODE);
    if (bytes == NULL) {
        lazy_discard_pending_exception(context);
        length = 0;
    } else {
        js_free(context, bytes);
    }
    *us = (js_rt_monotonic_time_ns() - started_ns) / UINT64_C(1000);
    return length;
}

static uint64_t census_elapsed_us(uint64_t started_ns)
{
    return (js_rt_monotonic_time_ns() - started_ns) / UINT64_C(1000);
}

static uint64_t census_saturating_sub(uint64_t a, uint64_t b)
{
    return a > b ? a - b : 0;
}

/* First-call compiles of lazily compiled function bodies are charged to
   the innermost factory call; a factory a call requires reports its own. */
static uint64_t census_nested_lazy_count, census_nested_lazy_bytes;

typedef struct {
    bool active;
    uint64_t compiled, bytes, outer_compiled, outer_bytes, started_ns;
} CensusFactoryCall;

static void census_factory_call_begin(ScriptRuntime *runtime,
                                      CensusFactoryCall *call)
{
    memset(call, 0, sizeof(*call));
    if (!tilefinch_trace_factory_cache()) return;
    uint64_t deferred = 0;
    census_lazy_function_stats(runtime->runtime, &call->compiled,
                               &call->bytes, &deferred);
    call->outer_compiled = census_nested_lazy_count;
    call->outer_bytes = census_nested_lazy_bytes;
    census_nested_lazy_count = census_nested_lazy_bytes = 0;
    call->started_ns = js_rt_monotonic_time_ns();
    call->active = true;
}

static void census_factory_call_end(ScriptRuntime *runtime,
                                    const ScriptLazyRuntimeBundle *bundle,
                                    size_t factory_index,
                                    ScriptLazyRuntimeFactory *factory,
                                    const CensusFactoryCall *call,
                                    bool threw)
{
    if (!call->active) return;
    uint64_t call_us = census_elapsed_us(call->started_ns);
    uint64_t compiled = 0, bytes = 0, deferred = 0;
    census_lazy_function_stats(runtime->runtime, &compiled, &bytes,
                               &deferred);
    uint64_t total_compiled = census_saturating_sub(compiled, call->compiled);
    uint64_t total_bytes = census_saturating_sub(bytes, call->bytes);
    uint64_t own_compiled =
        census_saturating_sub(total_compiled, census_nested_lazy_count);
    uint64_t own_bytes =
        census_saturating_sub(total_bytes, census_nested_lazy_bytes);
    census_nested_lazy_count = call->outer_compiled + total_compiled;
    census_nested_lazy_bytes = call->outer_bytes + total_bytes;
    size_t run = 0;
    uint64_t write_us = 0;
    if (!threw && !JS_IsUndefined(factory->census_program)) {
        run = census_serialized_bytes(runtime->context,
                                      factory->census_program, &write_us);
    }
    JS_FreeValue(runtime->context, factory->census_program);
    factory->census_program = JS_UNDEFINED;
    printf("census-lazy-factory-run digest=%016llx index=%zu run=%zu "
           "write-us=%llu lazy-count=%llu lazy-bytes=%llu call-us=%llu "
           "threw=%s\n",
           (unsigned long long) bundle->census_digest, factory_index, run,
           (unsigned long long) write_us, (unsigned long long) own_compiled,
           (unsigned long long) own_bytes, (unsigned long long) call_us,
           threw ? "yes" : "no");
}
#endif

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
#ifndef TILEFINCH_NO_TRACE
            JS_FreeValue(context, bundle->factories[i].census_program);
#endif
        }
    }
    lazy_bundle_release_source(bundle);
    for (size_t i = 0; i < bundle->factory_count; i++) {
        budget_free(budget, bundle->factories[i].compressed_source);
        budget_free(budget, bundle->factories[i].plain);
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
    /* functions freed later find no factory (js_lazy_webpack_text_*) */
    runtime->lazy_webpack_external_text_bytes = 0;
    runtime->lazy_webpack_plain_text_bytes = 0;
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
    size_t written = script_text_lz_compress(
        input, input_length, compressed, capacity, table);
    bool verified = written != 0
        && script_text_lz_matches(
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

/* A factory compiles as a program of its own: this prefix, the factory's
   text, and ")". */
static const char lazy_strict_prefix[] = "\"use strict\";(\n";
static const char lazy_loose_prefix[] = "(\n";

static size_t lazy_program_prefix(const ScriptLazyRuntimeBundle *bundle,
                                  const char **prefix)
{
    if (prefix != NULL)
        *prefix = bundle->strict_mode ? lazy_strict_prefix
                                      : lazy_loose_prefix;
    return bundle->strict_mode ? sizeof(lazy_strict_prefix) - 1
                               : sizeof(lazy_loose_prefix) - 1;
}

static bool lazy_factory_program(Budget *budget,
                                 const ScriptLazyRuntimeBundle *bundle,
                                 const ScriptLazyRuntimeFactory *factory,
                                 char **program, size_t *program_length)
{
    const char *prefix = NULL;
    size_t prefix_length = lazy_program_prefix(bundle, &prefix);
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
    char *copy = budget_malloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT,
                                        length + 1);
    if (copy == NULL) return false;
    memcpy(copy, prefix, prefix_length);
    bool restored = raw_available;
    if (raw_available) {
        memcpy(copy + prefix_length,
               bundle->source + factory->source_offset,
               factory->source_length);
    } else {
        restored = script_text_lz_decompress(
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

/* ---- Factory text outside the heap ----

   QuickJS keeps the text of each function it compiles for
   Function.prototype.toString(), and a lazily compiled inner function
   compiles its body from that text on its first call. For a factory that
   is a plain copy, in the realm heap and for as long as any of its
   functions lives, of text compressed_source already holds (on
   login.live.com, 1.7 MiB for 403 factories). A factory whose program
   (lazy_factory_program()) is at least LAZY_EXTERNAL_TEXT_MINIMUM bytes
   therefore compiles with JS_STRIP_SOURCE and names its program by an
   external id (JS_SetExternalSourceHooks(); evaluation.inc routes ids at or
   above SCRIPT_SOURCE_STORE_ID_LIMIT here), so its functions record only
   where their text is. A read is served from the factory's plain copy,
   or decodes compressed_source from its start up to the end of the read
   into a new one. The compile's program buffer becomes the first plain
   copy.

   Plain copies follow the function-source store's rule, plain first and
   given up under pressure: at each safe point they are trimmed, least
   recently read first, to what the page's room allows plain (a quarter
   of it, less the store's own plain text: js_rt_source_store_safe_point()),
   all of them once the realm was refused growth, and when the
   realm's growth would be refused they go before the store's text
   (js_rt_source_store_relieve()). Dropping one loses nothing:
   compressed_source lives as long as the bundle, so a read never fails
   while it does, and the text is never "required" the way the store's
   is. A copy is also dropped when no function refers to its text any
   more. TILEFINCH_LAZY_WEBPACK_FACTORY_TEXT=heap (lab builds) keeps the
   text in the heap, for A/B measurement. */
#define LAZY_EXTERNAL_TEXT_MINIMUM 1024u
#define LAZY_EXTERNAL_INDEX_BITS 13u
#define LAZY_EXTERNAL_BUNDLE_LIMIT \
    ((uint32_t) (SCRIPT_SOURCE_STORE_ID_LIMIT >> LAZY_EXTERNAL_INDEX_BITS))
_Static_assert(SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT
                   <= (1u << LAZY_EXTERNAL_INDEX_BITS),
               "a factory index fits in an external text id");

#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define LAZY_TEXT_TIMING 1
#else
#define LAZY_TEXT_TIMING 0
#endif

enum { LAZY_TEXT_ASCII_UNKNOWN = 0, LAZY_TEXT_ASCII_YES, LAZY_TEXT_ASCII_NO };

static bool lazy_external_text_enabled(void)
{
    const char *mode =
        tilefinch_lab_getenv("TILEFINCH_LAZY_WEBPACK_FACTORY_TEXT");
    return mode == NULL || strcmp(mode, "heap") != 0;
}

static size_t lazy_program_length(const ScriptLazyRuntimeBundle *bundle,
                                  const ScriptLazyRuntimeFactory *factory)
{
    return lazy_program_prefix(bundle, NULL) + factory->source_length + 1u;
}

/* The external id of factory `index`'s program text, or 0 if the bundle's
   identifier does not fit in one. */
static uint32_t lazy_external_id(const ScriptLazyRuntimeBundle *bundle,
                                 size_t index)
{
    if (bundle->identifier == 0
        || bundle->identifier >= LAZY_EXTERNAL_BUNDLE_LIMIT
        || index >= SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT) return 0;
    return SCRIPT_SOURCE_STORE_ID_LIMIT
        | (bundle->identifier << LAZY_EXTERNAL_INDEX_BITS)
        | (uint32_t) index;
}

/* The factory external id `id` names, or NULL once its bundle is gone
   (the realm is being destroyed). */
static ScriptLazyRuntimeFactory *lazy_external_factory(
    ScriptRuntime *runtime, uint32_t id,
    const ScriptLazyRuntimeBundle **bundle_out)
{
    if (runtime == NULL || id < SCRIPT_SOURCE_STORE_ID_LIMIT) return NULL;
    uint32_t local = id - SCRIPT_SOURCE_STORE_ID_LIMIT;
    ScriptLazyRuntimeBundle *bundle = lazy_bundle_find(
        runtime, local >> LAZY_EXTERNAL_INDEX_BITS);
    size_t index = local & ((1u << LAZY_EXTERNAL_INDEX_BITS) - 1u);
    if (bundle == NULL || index >= bundle->factory_count) return NULL;
    if (bundle_out != NULL) *bundle_out = bundle;
    return &bundle->factories[index];
}

static void lazy_plain_drop(ScriptRuntime *runtime,
                            const ScriptLazyRuntimeBundle *bundle,
                            ScriptLazyRuntimeFactory *factory)
{
    if (factory->plain == NULL) return;
    size_t held = lazy_program_length(bundle, factory) + 1u;
    budget_free(runtime->budget, factory->plain);
    factory->plain = NULL;
    factory->plain_length = 0;
    factory->plain_input = 0;
    runtime->lazy_webpack_plain_text_bytes -=
        held < runtime->lazy_webpack_plain_text_bytes
            ? held : runtime->lazy_webpack_plain_text_bytes;
}

size_t js_lazy_webpack_text_trim(ScriptRuntime *runtime, size_t allowance)
{
    if (runtime == NULL) return 0;
    size_t before = runtime->lazy_webpack_plain_text_bytes;
    while (runtime->lazy_webpack_plain_text_bytes > allowance) {
        ScriptLazyRuntimeBundle *victim_bundle = NULL;
        ScriptLazyRuntimeFactory *victim = NULL;
        for (ScriptLazyRuntimeBundle *bundle = runtime->lazy_webpack_bundles;
             bundle != NULL; bundle = bundle->next) {
            for (size_t i = 0; i < bundle->factory_count; i++) {
                ScriptLazyRuntimeFactory *factory = &bundle->factories[i];
                if (factory->plain == NULL) continue;
                if (allowance == 0) {
                    /* everything goes: no need to order */
                    lazy_plain_drop(runtime, bundle, factory);
                    js_rt_saturating_add_size(
                        &runtime->result
                             .lazy_webpack_external_text_plain_drops, 1);
                    continue;
                }
                if (victim == NULL
                    || factory->plain_used < victim->plain_used) {
                    victim = factory;
                    victim_bundle = bundle;
                }
            }
        }
        if (victim == NULL) break;
        lazy_plain_drop(runtime, victim_bundle, victim);
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_external_text_plain_drops, 1);
    }
    size_t after = runtime->lazy_webpack_plain_text_bytes;
    return before > after ? before - after : 0;
}

/* Makes `factory`'s plain copy hold at least `end` bytes of its program;
   false if the Budget refused the copy or the text does not decode. */
static bool lazy_plain_ensure(ScriptRuntime *runtime,
                              const ScriptLazyRuntimeBundle *bundle,
                              ScriptLazyRuntimeFactory *factory, size_t end)
{
    const char *prefix = NULL;
    size_t prefix_length = lazy_program_prefix(bundle, &prefix);
    size_t program_length = prefix_length + factory->source_length + 1u;
    if (factory->plain != NULL && factory->plain_length >= end) return true;
    if (factory->plain == NULL) {
        unsigned char *plain = budget_malloc_category(
            runtime->budget, BUDGET_CATEGORY_JAVASCRIPT, program_length + 1u);
        if (plain == NULL) {
            /* other factories' copies give way to this one */
            (void) js_lazy_webpack_text_trim(runtime, 0);
            plain = budget_malloc_category(
                runtime->budget, BUDGET_CATEGORY_JAVASCRIPT,
                program_length + 1u);
            if (plain == NULL) return false;
        }
        memcpy(plain, prefix, prefix_length);
        factory->plain = plain;
        factory->plain_length = prefix_length;
        factory->plain_input = 0;
        runtime->lazy_webpack_plain_text_bytes += program_length + 1u;
    }
    size_t text_at = factory->plain_length - prefix_length;
    if (text_at > factory->source_length) text_at = factory->source_length;
    size_t text_end = end > prefix_length ? end - prefix_length : 0;
    if (text_end > factory->source_length) text_end = factory->source_length;
    if (text_at < text_end) {
        size_t decoded_from = text_at;
        unsigned char *text = factory->plain + prefix_length;
        bool raw = bundle->source != NULL
            && factory->source_offset <= bundle->source_length
            && factory->source_length
                   <= bundle->source_length - factory->source_offset;
        if (raw) {
            /* still registering: the bundle's own bytes are at hand */
            memcpy(text, bundle->source + factory->source_offset,
                   factory->source_length);
            factory->plain_input = factory->compressed_source_length;
            text_at = factory->source_length;
        } else {
#if LAZY_TEXT_TIMING
            uint64_t started_ns = js_rt_monotonic_time_ns();
#endif
            if (factory->compressed_source == NULL
                || !script_text_lz_decompress_until(
                       factory->compressed_source,
                       factory->compressed_source_length, text,
                       factory->source_length, &factory->plain_input,
                       &text_at, text_end)
                || text_at < text_end) {
                lazy_plain_drop(runtime, bundle, factory);
                return false;
            }
#if LAZY_TEXT_TIMING
            runtime->result.lazy_webpack_external_text_decode_us +=
                (js_rt_monotonic_time_ns() - started_ns) / 1000u;
#endif
            js_rt_saturating_add_size(
                &runtime->result.lazy_webpack_external_text_decodes, 1);
            js_rt_saturating_add_size(
                &runtime->result.lazy_webpack_external_text_decoded_bytes,
                text_at - decoded_from);
        }
        factory->plain_length = prefix_length + text_at;
    }
    if (end == program_length && text_at == factory->source_length
        && factory->plain_length < program_length) {
        factory->plain[program_length - 1u] = ')';
        factory->plain[program_length] = '\0';
        factory->plain_length = program_length;
    }
    return factory->plain_length >= end;
}

int js_lazy_webpack_text_read(ScriptRuntime *runtime, uint32_t id,
                              uint32_t offset, uint32_t length, char *dest)
{
    const ScriptLazyRuntimeBundle *bundle = NULL;
    ScriptLazyRuntimeFactory *factory =
        lazy_external_factory(runtime, id, &bundle);
    if (runtime != NULL)
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_external_text_reads, 1);
    size_t program_length = factory == NULL
        ? 0 : lazy_program_length(bundle, factory);
    if (factory == NULL || offset > program_length
        || length > program_length - offset
        || (length != 0 && dest == NULL)
        || !lazy_plain_ensure(runtime, bundle, factory,
                              (size_t) offset + length)) {
        if (runtime != NULL)
            js_rt_saturating_add_size(
                &runtime->result.lazy_webpack_external_text_read_failures, 1);
        return -1;
    }
    if (length != 0) memcpy(dest, factory->plain + offset, length);
    factory->plain_used = ++runtime->lazy_webpack_plain_clock;
    js_rt_saturating_add_size(
        &runtime->result.lazy_webpack_external_text_read_bytes, length);
    return 0;
}

int js_lazy_webpack_text_query(ScriptRuntime *runtime, uint32_t id,
                               uint32_t offset, uint32_t length)
{
    const ScriptLazyRuntimeBundle *bundle = NULL;
    ScriptLazyRuntimeFactory *factory =
        lazy_external_factory(runtime, id, &bundle);
    if (factory == NULL || offset > lazy_program_length(bundle, factory)
        || length > lazy_program_length(bundle, factory) - offset) {
        if (runtime != NULL)
            js_rt_saturating_add_size(
                &runtime->result.lazy_webpack_external_text_read_failures, 1);
        return -1;
    }
    if (factory->plain_ascii == LAZY_TEXT_ASCII_UNKNOWN
        && factory->plain != NULL
        && factory->plain_length == lazy_program_length(bundle, factory)) {
        unsigned char bits = 0;
        for (size_t i = 0; i < factory->plain_length; i++)
            bits |= factory->plain[i];
        factory->plain_ascii = (bits & 0x80u) == 0
            ? LAZY_TEXT_ASCII_YES : LAZY_TEXT_ASCII_NO;
    }
    /* 0: possibly not ASCII; the engine then reads a window at a time */
    return factory->plain_ascii == LAZY_TEXT_ASCII_YES ? 1 : 0;
}

void js_lazy_webpack_text_release(ScriptRuntime *runtime, uint32_t id)
{
    const ScriptLazyRuntimeBundle *bundle = NULL;
    ScriptLazyRuntimeFactory *factory =
        lazy_external_factory(runtime, id, &bundle);
    if (factory == NULL || factory->external_owners == 0) return;
    if (--factory->external_owners != 0) return;
    size_t program_length = lazy_program_length(bundle, factory);
    runtime->lazy_webpack_external_text_bytes -=
        program_length < runtime->lazy_webpack_external_text_bytes
            ? program_length : runtime->lazy_webpack_external_text_bytes;
    lazy_plain_drop(runtime, bundle, factory);
}

/* Compile setup for factory `index`: strips the program's text from the
   heap and names it by external id. Returns the id (0: the text stays in
   the heap) and the strip flags to restore. The owner count goes up before
   the compile, since a compile that fails releases the id inside it. */
static uint32_t lazy_external_compile_begin(
    ScriptRuntime *runtime, ScriptLazyRuntimeBundle *bundle, size_t index,
    size_t program_length, int *previous_strip)
{
    ScriptLazyRuntimeFactory *factory = &bundle->factories[index];
    uint32_t id = program_length >= LAZY_EXTERNAL_TEXT_MINIMUM
            && program_length <= UINT32_MAX / 2u
            && factory->external_owners < UINT32_MAX
            && lazy_external_text_enabled()
        ? lazy_external_id(bundle, index) : 0;
    if (id == 0 || !js_rt_external_source_hooks(runtime)) return 0;
    if (factory->external_owners++ == 0)
        runtime->lazy_webpack_external_text_bytes += program_length;
    *previous_strip = JS_GetStripInfo(runtime->runtime);
    JS_SetStripInfo(runtime->runtime, *previous_strip | JS_STRIP_SOURCE);
    (void) JS_SetExternalSourceId(runtime->runtime, id,
                                  (uint32_t) program_length);
    return id;
}

/* Undoes lazy_external_compile_begin(); true if the compile took the id
   (its functions, if it made any, now refer to the text). */
static bool lazy_external_compile_end(ScriptRuntime *runtime, uint32_t id,
                                      int previous_strip)
{
    if (id == 0) return false;
    JS_SetStripInfo(runtime->runtime, previous_strip);
    uint32_t pending = JS_SetExternalSourceId(runtime->runtime, 0, 0);
    if (pending == 0) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_external_text_compiles, 1);
        return true;
    }
    /* never reached JS_Eval (admission refused it) */
    js_lazy_webpack_text_release(runtime, pending);
    return false;
}

/* Every factory retains exact compressed source, so a cold toString query
   need not instantiate compiler state just to recover author text. */
static JSValue lazy_factory_source_string(
    JSContext *context, ScriptRuntime *runtime,
    const ScriptLazyRuntimeBundle *bundle,
    ScriptLazyRuntimeFactory *factory)
{
    size_t prefix_length = lazy_program_prefix(bundle, NULL);
    if (factory->plain != NULL
        && factory->plain_length == lazy_program_length(bundle, factory)) {
        /* The output allocation can reclaim plain copies through the heap
           growth hook. Keep this buffer owned but unavailable to trimming
           until the copy is complete, without allocating another copy. */
        unsigned char *plain = factory->plain;
        factory->plain = NULL;
        JSValue text = JS_NewStringLen(
            context, (const char *) plain + prefix_length,
            factory->source_length);
        factory->plain = plain;
        /* A collection during the allocation may have freed the last
           external source owner while the buffer was borrowed. */
        if (factory->external_owners == 0)
            lazy_plain_drop(runtime, bundle, factory);
        return text;
    }
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
        || !script_text_lz_decompress(
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

/* Admits a factory's first compile on the model script admission uses
   (script_admission.h): the measured median working set, twice the source
   plus parser state, against the heap the realm can still reach, while the
   page Budget keeps the factory's program text, that working set and the
   execution reserve. The bundle itself was admitted at load on its largest
   factory with the same model. A factory that needs more than planned is
   stopped by the heap limit and refused at compile time with the realm
   intact (js_rt_compile_source_type). */
static bool lazy_factory_prepare_compile_working_set(
    ScriptRuntime *runtime, const ScriptLazyRuntimeBundle *bundle,
    size_t factory_index, const ScriptLazyRuntimeFactory *factory)
{
    if (runtime == NULL || factory == NULL
        || factory->source_length > SIZE_MAX - 32u) return false;
    size_t program_bytes = factory->source_length + 32u;
    size_t heap_required =
        script_admission_compile_peak(factory->source_length);
    size_t page_required = program_bytes;
    js_rt_saturating_add_size(&page_required, heap_required);
    js_rt_saturating_add_size(&page_required,
                              SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES);
    bool page_pressure = budget_remaining(runtime->budget) < page_required;
    bool heap_pressure = script_runtime_heap_available(runtime) < heap_required;
    size_t reclaimed = 0;
    /* Near its limit a realm compiling many factories would otherwise run
       a full collection before every one, though the heap is mostly the
       live code just compiled (xe.com: hundreds of collections that freed
       nothing). Collect up front only once enough has been allocated since
       the last collection to pay for it; otherwise return allocator caches
       and optional caches first, and collect only if the compile would
       still be refused. */
    bool collected = false;
    if ((page_pressure || heap_pressure) && js_rt_gc_due(runtime)) {
        size_t before = budget_remaining(runtime->budget);
        (void) script_runtime_collect_and_trim(runtime);
        size_t after = budget_remaining(runtime->budget);
        reclaimed = after > before ? after - before : 0;
        collected = true;
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_compile_pressure_collections, 1);
    } else if (page_pressure) {
        size_t before = budget_remaining(runtime->budget);
        (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
        size_t after = budget_remaining(runtime->budget);
        reclaimed = after > before ? after - before : 0;
    }
    for (;;) {
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
        heap_pressure =
            script_runtime_heap_available(runtime) < heap_required;
        if (collected || (!page_pressure && !heap_pressure)) break;
        /* The last resort before refusing the compile. */
        size_t before = budget_remaining(runtime->budget);
        (void) script_runtime_collect_and_trim(runtime);
        size_t after = budget_remaining(runtime->budget);
        js_rt_saturating_add_size(&reclaimed,
                                  after > before ? after - before : 0);
        collected = true;
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_compile_pressure_collections, 1);
    }
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
    size_t working_bytes = program_bytes;
    js_rt_saturating_add_size(&working_bytes, heap_required);
    budget_record_pressure(runtime->budget, BUDGET_PRESSURE_JAVASCRIPT,
                           working_bytes, reclaimed);
    if (tilefinch_trace_lazy_scripts()) {
        fprintf(stderr,
                "lazy-webpack-factory-failed bundle=%u factory=%zu "
                "bytes=%zu page-required=%zu page-remaining=%zu "
                "heap-required=%zu heap-available=%zu stage=admission\n",
                (unsigned) bundle->identifier, factory_index,
                factory->source_length,
                page_required, budget_remaining(runtime->budget),
                heap_required, script_runtime_heap_available(runtime));
    }
    return false;
}

/* Replaces a pending exception's own property `name` (ignoring a refusal:
   the exception itself must survive). */
static void lazy_error_set(JSContext *context, JSValueConst error,
                           const char *name, JSValue value)
{
    if (JS_IsException(value)
        || JS_DefinePropertyValueStr(
               context, error, name, value,
               JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
        lazy_discard_pending_exception(context);
    }
}

/* A factory that does not compile throws the parser's SyntaxError, which
   QuickJS locates in the program compiled for the factory: file
   "<url>#factory-<n>", the factory starting on its line 2. Report it where
   it is in the bundle instead (fileName, lineNumber, columnNumber and the
   stack's first line), as compiling the whole bundle would have. Any other
   exception, or one whose position is not in that program, is left alone.
   The exception stays pending. */
static void lazy_factory_relocate_syntax_error(
    JSContext *context, const ScriptLazyRuntimeBundle *bundle,
    const ScriptLazyRuntimeFactory *factory, const char *program_name)
{
    if (bundle->source_url == NULL || bundle->source_url[0] == '\0'
        || factory->source_line == 0 || factory->source_column == 0) return;
    JSValue error = JS_GetException(context);
    if (!JS_IsObject(error)) {
        JS_Throw(context, error);
        return;
    }
    JSValue file = JS_GetPropertyStr(context, error, "fileName");
    JSValue line_value = JS_GetPropertyStr(context, error, "lineNumber");
    JSValue column_value = JS_GetPropertyStr(context, error, "columnNumber");
    JSValue stack_value = JS_GetPropertyStr(context, error, "stack");
    const char *file_name = JS_IsString(file)
        ? JS_ToCString(context, file) : NULL;
    const char *stack = JS_IsString(stack_value)
        ? JS_ToCString(context, stack_value) : NULL;
    int32_t line = 0, column = 0;
    bool located = file_name != NULL
        && strcmp(file_name, program_name) == 0
        && JS_IsNumber(line_value) && JS_IsNumber(column_value)
        && JS_ToInt32(context, &line, line_value) == 0
        && JS_ToInt32(context, &column, column_value) == 0
        && line >= 2 && column >= 1;
    if (located) {
        uint64_t bundle_line = (uint64_t) factory->source_line
            + (uint64_t) (line - 2);
        uint64_t bundle_column = line == 2
            ? (uint64_t) factory->source_column + (uint64_t) column - 1u
            : (uint64_t) column;
        if (bundle_line > INT32_MAX) bundle_line = INT32_MAX;
        if (bundle_column > INT32_MAX) bundle_column = INT32_MAX;
        lazy_error_set(context, error, "fileName",
                       JS_NewString(context, bundle->source_url));
        lazy_error_set(context, error, "lineNumber",
                       JS_NewInt32(context, (int32_t) bundle_line));
        lazy_error_set(context, error, "columnNumber",
                       JS_NewInt32(context, (int32_t) bundle_column));
        /* The stack's first line is the parser's position. */
        static const char frame[] = "    at ";
        size_t name_length = strlen(program_name);
        if (stack != NULL
            && strncmp(stack, frame, sizeof(frame) - 1u) == 0
            && strncmp(stack + sizeof(frame) - 1u, program_name,
                       name_length) == 0) {
            const char *rest = strchr(stack, '\n');
            size_t rest_length = rest == NULL ? 0 : strlen(rest);
            size_t capacity = strlen(bundle->source_url) + rest_length + 48u;
            char *text = js_malloc(context, capacity);
            if (text != NULL) {
                int written = snprintf(
                    text, capacity, "%s%s:%lu:%lu%s", frame,
                    bundle->source_url, (unsigned long) bundle_line,
                    (unsigned long) bundle_column,
                    rest == NULL ? "" : rest);
                if (written > 0 && (size_t) written < capacity) {
                    lazy_error_set(context, error, "stack",
                                   JS_NewStringLen(context, text,
                                                   (size_t) written));
                }
                js_free(context, text);
            } else {
                lazy_discard_pending_exception(context);
            }
        }
    }
    JS_FreeCString(context, stack);
    JS_FreeCString(context, file_name);
    JS_FreeValue(context, stack_value);
    JS_FreeValue(context, column_value);
    JS_FreeValue(context, line_value);
    JS_FreeValue(context, file);
    JS_Throw(context, error);
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
        script_runtime_note_memory_refusal(runtime);
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
#ifndef TILEFINCH_NO_TRACE
    bool census = tilefinch_trace_factory_cache();
    uint64_t census_fnv = census
        ? census_fnv64((const unsigned char *) program, program_length) : 0;
    uint64_t census_lazy_before = 0, census_lazy_after = 0, census_unused = 0;
    if (census)
        census_lazy_function_stats(runtime->runtime, &census_unused,
                                   &census_unused, &census_lazy_before);
    uint64_t census_started_ns = js_rt_monotonic_time_ns();
#endif
    int previous_strip = 0;
    uint32_t external_id = lazy_external_compile_begin(
        runtime, bundle, factory_index, program_length, &previous_strip);
    JSValue compiled = js_rt_compile_source_type(
        context, program, program_length, name, JS_EVAL_TYPE_GLOBAL,
        SCRIPT_COMPILE_SOURCE_LAZY_FACTORY, &runtime->result, &admitted);
    bool external = lazy_external_compile_end(runtime, external_id,
                                              previous_strip);
#ifndef TILEFINCH_NO_TRACE
    uint64_t census_compile_us = census_elapsed_us(census_started_ns);
    if (census)
        census_lazy_function_stats(runtime->runtime, &census_unused,
                                   &census_unused, &census_lazy_after);
#endif
    if (external && factory->external_owners != 0 && factory->plain == NULL) {
        /* the program buffer is the text's first plain copy */
        factory->plain = (unsigned char *) program;
        factory->plain_length = program_length;
        factory->plain_input = factory->compressed_source_length;
        factory->plain_used = ++runtime->lazy_webpack_plain_clock;
        runtime->lazy_webpack_plain_text_bytes += program_length + 1u;
        program = NULL;
    }
    budget_free(runtime->budget, program);
    if (!admitted || JS_IsException(compiled)) {
        if (tilefinch_trace_lazy_scripts()) {
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
        lazy_factory_relocate_syntax_error(context, bundle, factory, name);
        return compiled;
    }
    js_rt_saturating_add_size(&runtime->result.lazy_webpack_source_compiles, 1);
    if (!js_rt_runtime_script_checkpoint(runtime, 1)) {
        if (tilefinch_trace_lazy_scripts()) {
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
#ifndef TILEFINCH_NO_TRACE
    if (census) {
        uint64_t write_us = 0;
        size_t fresh = census_serialized_bytes(context, compiled, &write_us);
        factory->census_compiles++;
        JS_FreeValue(context, factory->census_program);
        factory->census_program = JS_DupValue(context, compiled);
        printf("census-lazy-factory digest=%016llx index=%zu bytes=%zu "
               "compile-us=%llu fresh=%zu write-us=%llu deferred=%llu "
               "nth=%u fnv=%016llx\n",
               (unsigned long long) bundle->census_digest, factory_index,
               factory->source_length,
               (unsigned long long) census_compile_us, fresh,
               (unsigned long long) write_us,
               (unsigned long long) census_saturating_sub(
                   census_lazy_after, census_lazy_before),
               (unsigned) factory->census_compiles,
               (unsigned long long) census_fnv);
    }
#endif
    JSValue value = JS_EvalFunction(context, compiled);
    if (JS_IsException(value) || !JS_IsFunction(context, value)) {
        if (tilefinch_trace_lazy_scripts()) {
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
    if (tilefinch_trace_lazy_scripts()
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
    if ((factory->source_length >= 64u * 1024u
         || (runtime->result.lazy_webpack_factories_compiled & 31u) == 0)
        && js_rt_gc_due(runtime)) {
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
#ifndef TILEFINCH_NO_TRACE
    JS_FreeValue(runtime->context, factory->census_program);
    factory->census_program = JS_UNDEFINED;
#endif
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
#ifndef TILEFINCH_NO_TRACE
    CensusFactoryCall census_call;
    census_factory_call_begin(runtime, &census_call);
#endif
    JSValue returned = JS_Call(context, callable, JS_UNDEFINED, argc, argv);
#ifndef TILEFINCH_NO_TRACE
    census_factory_call_end(runtime, bundle, factory_index, factory,
                            &census_call, JS_IsException(returned));
#endif
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
#ifndef TILEFINCH_NO_TRACE
    CensusFactoryCall census_call;
    census_factory_call_begin(runtime, &census_call);
#endif
    JSValue returned = JS_Call(context, callable, argv[2], argument_count,
                               (JSValueConst *) arguments);
#ifndef TILEFINCH_NO_TRACE
    census_factory_call_end(runtime, bundle, factory_index, factory,
                            &census_call, JS_IsException(returned));
#endif
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
    /* Wrappers are function objects, and strict equality of objects is
       identity: a non-object receiver matches none, and the scan (every
       factory of every bundle, per call) compares pointers. */
    void *receiver = JS_VALUE_GET_TAG(this_value) == JS_TAG_OBJECT
        ? JS_VALUE_GET_PTR(this_value) : NULL;
    for (ScriptLazyRuntimeBundle *bundle = receiver == NULL
             ? NULL : runtime->lazy_webpack_bundles;
         bundle != NULL; bundle = bundle->next) {
        for (size_t i = 0; i < bundle->factory_count; i++) {
            ScriptLazyRuntimeFactory *factory = &bundle->factories[i];
            if (JS_VALUE_GET_TAG(factory->wrapper) == JS_TAG_OBJECT
                && JS_VALUE_GET_PTR(factory->wrapper) == receiver) {
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
    JSValue prototype = JS_IsException(constructor) ? JS_UNDEFINED
        : JS_GetPropertyStr(context, constructor, "prototype");
    JSValue original = JS_IsUndefined(prototype) || JS_IsException(prototype)
        ? JS_UNDEFINED : JS_GetPropertyStr(context, prototype, "toString");
    /* Materializing the original intrinsic can fail on allocation. Only
       create the wrapper when the property setter can take ownership of it;
       an unconsumed native function retains a reference to this realm. */
    bool ok = false;
    if (JS_IsFunction(context, original)) {
        function = JS_NewCFunction(
            context, js_lazy_function_to_string, "toString", 0);
        ok = !JS_IsException(function)
            && JS_SetPropertyStr(context, prototype, "toString", function) >= 0;
    }
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

/* Lab A/B only (lazy_preflight_requested): QuickJS does not expose a
   grammar-only parser, so this compiles one isolated factory at a time and
   immediately releases its compiler object. It used to run for every
   factory of every load, to keep a syntax error's timing exact; most of
   those factories never ran. */
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
        if (tilefinch_trace_lazy_scripts()) {
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

/* Advances a 1-based line/column position (QuickJS's counting: lines end
   at '\n', columns count UTF-8 lead bytes) over source[from, to). */
static void lazy_advance_position(const char *source, size_t from,
                                  size_t to, uint32_t *line,
                                  uint32_t *column)
{
    for (size_t at = from; at < to; at++) {
        unsigned char byte = (unsigned char) source[at];
        if (byte == '\n') {
            if (*line != UINT32_MAX) (*line)++;
            *column = 1;
        } else if ((byte < 0x80u || byte >= 0xc0u) && *column != UINT32_MAX) {
            (*column)++;
        }
    }
}

/* Lab A/B only (TILEFINCH_LAZY_WEBPACK_PREFLIGHT): compile every factory
   once as a syntax check before registration, as before the first-visit
   preflight was retired. A shipping build never reads it. */
static bool lazy_preflight_requested(void)
{
    return tilefinch_lab_getenv("TILEFINCH_LAZY_WEBPACK_PREFLIGHT") != NULL;
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
        SCRIPT_TEXT_LZ_HASH_SIZE * sizeof(*table));
    if (table == NULL) return false;
    /* No factory is compiled here: each one is compiled, and so checked,
       when it first runs (lazy_factory_compile). A factory that never runs
       is never checked; see docs/engineering/SCRIPT_MEMORY.md, "Syntax errors:
       a deliberate deviation". */
    bool preflight = lazy_preflight_requested();
    uint32_t line = 1, column = 1;
    size_t position = 0;
    for (size_t i = 0; i < bundle->factory_count; i++) {
        ScriptLazyRuntimeFactory *factory = &bundle->factories[i];
        if (factory->source_offset >= position
            && factory->source_offset <= bundle->source_length) {
            lazy_advance_position(bundle->source, position,
                                  factory->source_offset, &line, &column);
            position = factory->source_offset;
        }
        factory->source_line = line;
        factory->source_column = column;
        bool cached = (!preflight
                       || lazy_preflight_factory_syntax(
                              runtime, bundle, i, factory))
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
        if (tilefinch_trace_lazy_scripts()) {
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

/* ---- Lazy webpack bundle records ----

   A record is the plan of one exact byte sequence, so a later load of the
   same bytes skips the planner: the factory table only. It vouches for no
   factory's syntax (format 1 did, when every factory was preflighted on a
   first visit): factories are checked when they first run, on every load.
   The session stores it as an opaque payload in its LAZY_BUNDLE table
   (session.h), keyed like a classic script by a fixed name, the request
   URL, the top-level site and the bytes' length and SHA-256.

   Payload (little-endian), LAZY_RECORD_HEADER bytes then one
   LAZY_RECORD_FACTORY entry per factory:
     magic "TFLB", format, bytecode ABI, engine fingerprint (FNV-1a of
     TILEFINCH_QUICKJS_ENGINE_ID), release sequence, flags (strict mode),
     factory count, source length;
     per factory: source offset, source length (u32 each), the gap from the
     end of its key to its source (u16), the key length (u8), and the kind
     and arity (u8).
   A record whose identity differs from this build (ABI, engine, release or
   format), or whose factory table does not describe the bytes, is a miss:
   the bundle is planned as on a first visit and the record is dropped. The release sequence ties records to the planner build too
   (script_lazy.c is not part of the engine fingerprint). */
#define LAZY_RECORD_MAGIC UINT32_C(0x424c4654)
#define LAZY_RECORD_FORMAT 2u
#define LAZY_RECORD_HEADER 32u
#define LAZY_RECORD_FACTORY 12u
#define LAZY_RECORD_FLAG_STRICT 1u
/* The table's compile flags for a record: a format change misses before
   any payload is read. */
#define LAZY_RECORD_COMPILE_FLAGS (UINT32_C(0x4c420000) | LAZY_RECORD_FORMAT)

static const char lazy_record_name[] = "#lazy-webpack-bundle";

static void lazy_record_put_u32(unsigned char *at, uint32_t value)
{
    at[0] = (unsigned char) value;
    at[1] = (unsigned char) (value >> 8);
    at[2] = (unsigned char) (value >> 16);
    at[3] = (unsigned char) (value >> 24);
}

static uint32_t lazy_record_get_u32(const unsigned char *at)
{
    return (uint32_t) at[0] | (uint32_t) at[1] << 8
        | (uint32_t) at[2] << 16 | (uint32_t) at[3] << 24;
}

static uint32_t lazy_record_engine_fingerprint(void)
{
    static const char engine[] = TILEFINCH_QUICKJS_ENGINE_ID;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i + 1u < sizeof(engine); i++) {
        hash ^= (unsigned char) engine[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

/* The header words that must equal this build's: magic, format, ABI,
   engine, release. */
static void lazy_record_identity(uint32_t identity[5])
{
    identity[0] = LAZY_RECORD_MAGIC;
    identity[1] = LAZY_RECORD_FORMAT;
    identity[2] = TILEFINCH_QUICKJS_BYTECODE_ABI;
    identity[3] = lazy_record_engine_fingerprint();
    identity[4] = (uint32_t) TILEFINCH_RELEASE_SEQUENCE;
}

static bool lazy_record_key_init(
    ScriptRuntime *runtime, const char *request_url, const char *source,
    size_t source_length, char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT],
    BrowserScriptBytecodeKey *key)
{
    memset(key, 0, sizeof(*key));
    if (runtime == NULL || runtime->session == NULL || request_url == NULL
        || request_url[0] == '\0' || source == NULL || source_length == 0
        || source_length > UINT32_MAX
        || runtime->session->maximum_lazy_bundle_record_bytes == 0
        /* As the classic table: a data: URL is its own source and a blob:
           URL is minted afresh by every page. */
        || strncasecmp(request_url, "data:", 5) == 0
        || strncasecmp(request_url, "blob:", 5) == 0
        || script_runtime_origin_is_opaque(runtime)
        || runtime->bridge.top_level_url == NULL
        || !tilefinch_url_site_key(runtime->bridge.top_level_url, partition,
                                   TILEFINCH_ORIGIN_SERIALIZED_LIMIT)
        || tilefinch_lab_getenv("TILEFINCH_DISABLE_LAZY_BUNDLE_RECORDS")
               != NULL) {
        return false;
    }
    if (runtime->module_bytecode_generation == 0) {
        runtime->module_bytecode_generation =
            browser_session_module_bytecode_generation(runtime->session);
    }
    *key = (BrowserScriptBytecodeKey) {
        .module_name = lazy_record_name,
        .response_url = request_url,
        .partition_key = partition,
        .source = (const unsigned char *) source,
        .source_length = source_length,
        .compile_flags = LAZY_RECORD_COMPILE_FLAGS,
        .ordinal = 0
    };
    return true;
}

/* The RAM table, then the persistent tier (whose index names the record's
   group, or the card is not touched). */
static BrowserSharedBody *lazy_record_acquire(ScriptRuntime *runtime,
                                              BrowserScriptBytecodeKey *key)
{
    BrowserSession *session = runtime->session;
    uint32_t generation = runtime->module_bytecode_generation;
    BrowserSharedBody *record = browser_session_script_bytecode_acquire(
        session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, key, generation);
    if (record != NULL || key->digest_ready
        || !browser_session_script_disk_enabled(session)) return record;
    if (browser_session_script_disk_promote(
            session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, key, generation)) {
        record = browser_session_script_bytecode_acquire(
            session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE, key, generation);
    }
    return record;
}

static bool lazy_record_key_byte(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
        || (byte >= '0' && byte <= '9') || byte == '_' || byte == '$'
        || byte >= 0x80;
}

/* Rebuilds the plan a record describes, checking that it fits the bytes:
   ordered, non-overlapping factories inside the source, each starting the
   way its kind starts and ending with its closing brace, each key a
   plausible property name before it. */
static bool lazy_record_restore(Budget *budget, const unsigned char *data,
                                size_t length, const char *source,
                                size_t source_length,
                                ScriptLazyWebpackPlan *plan)
{
    memset(plan, 0, sizeof(*plan));
    if (length < LAZY_RECORD_HEADER) return false;
    uint32_t identity[5];
    lazy_record_identity(identity);
    for (size_t i = 0; i < 5; i++) {
        if (lazy_record_get_u32(data + 4u * i) != identity[i]) return false;
    }
    uint32_t flags = lazy_record_get_u32(data + 20);
    uint32_t count = lazy_record_get_u32(data + 24);
    if ((flags & ~LAZY_RECORD_FLAG_STRICT) != 0
        || lazy_record_get_u32(data + 28) != source_length
        || count == 0 || count > SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT
        || (length - LAZY_RECORD_HEADER) / LAZY_RECORD_FACTORY != count
        || (length - LAZY_RECORD_HEADER) % LAZY_RECORD_FACTORY != 0)
        return false;
    ScriptLazyFactory *factories = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, count * sizeof(*factories));
    if (factories == NULL) return false;
    const unsigned char *bytes = (const unsigned char *) source;
    size_t previous_end = 0, total = 0, largest = 0;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *at =
            data + LAZY_RECORD_HEADER + (size_t) i * LAZY_RECORD_FACTORY;
        size_t offset = lazy_record_get_u32(at);
        size_t factory_length = lazy_record_get_u32(at + 4);
        size_t gap = (size_t) at[8] | (size_t) at[9] << 8;
        size_t key_length = at[10];
        unsigned kind = at[11] >> 2;
        unsigned arity = at[11] & 3u;
        bool ok = factory_length != 0 && key_length != 0
            && offset <= source_length
            && factory_length <= source_length - offset
            && offset >= previous_end
            && gap + key_length <= offset - previous_end
            && (kind == SCRIPT_LAZY_FACTORY_ARROW
                || kind == SCRIPT_LAZY_FACTORY_FUNCTION)
            && bytes[offset + factory_length - 1u] == '}'
            && lazy_record_key_byte(bytes[offset - gap - key_length])
            && lazy_record_key_byte(bytes[offset - gap - 1u]);
        if (ok && kind == SCRIPT_LAZY_FACTORY_FUNCTION) {
            ok = factory_length > 8u && memcmp(bytes + offset, "function", 8)
                == 0;
        } else if (ok) {
            ok = bytes[offset] == '(' || lazy_record_key_byte(bytes[offset]);
        }
        if (!ok) {
            budget_free(budget, factories);
            return false;
        }
        factories[i] = (ScriptLazyFactory) {
            .key_offset = offset - gap - key_length,
            .key_length = key_length,
            .source_offset = offset,
            .source_length = factory_length,
            .arity = (uint8_t) arity,
            .kind = (ScriptLazyFactoryKind) kind
        };
        previous_end = offset + factory_length;
        total += factory_length;
        if (factory_length > largest) largest = factory_length;
    }
    *plan = (ScriptLazyWebpackPlan) {
        .budget = budget,
        .factories = factories,
        .factory_count = count,
        .factory_capacity = count,
        .factory_source_bytes = total,
        .largest_factory_bytes = largest,
        .strict_mode = (flags & LAZY_RECORD_FLAG_STRICT) != 0,
        .from_record = true
    };
    return true;
}

/* The record of a plan, or false
   when a factory does not fit the compact form (the bundle then simply
   has no record). */
static bool lazy_record_encode(Budget *budget,
                               const ScriptLazyWebpackPlan *plan,
                               size_t source_length, unsigned char **record,
                               size_t *record_length)
{
    *record = NULL;
    *record_length = 0;
    if (plan->factory_count == 0
        || plan->factory_count > SCRIPT_LAZY_RESIDENT_FACTORY_LIMIT
        || source_length > UINT32_MAX) return false;
    size_t length =
        LAZY_RECORD_HEADER + plan->factory_count * LAZY_RECORD_FACTORY;
    unsigned char *data = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, length);
    if (data == NULL) return false;
    uint32_t identity[5];
    lazy_record_identity(identity);
    for (size_t i = 0; i < 5; i++)
        lazy_record_put_u32(data + 4u * i, identity[i]);
    lazy_record_put_u32(data + 20,
                        plan->strict_mode ? LAZY_RECORD_FLAG_STRICT : 0u);
    lazy_record_put_u32(data + 24, (uint32_t) plan->factory_count);
    lazy_record_put_u32(data + 28, (uint32_t) source_length);
    for (size_t i = 0; i < plan->factory_count; i++) {
        const ScriptLazyFactory *factory = &plan->factories[i];
        unsigned char *at =
            data + LAZY_RECORD_HEADER + i * LAZY_RECORD_FACTORY;
        size_t key_end = factory->key_offset + factory->key_length;
        if (factory->key_length == 0 || factory->key_length > UINT8_MAX
            || key_end < factory->key_offset
            || key_end > factory->source_offset
            || factory->source_offset - key_end > UINT16_MAX
            || factory->arity > 3u) {
            budget_free(budget, data);
            return false;
        }
        size_t gap = factory->source_offset - key_end;
        lazy_record_put_u32(at, (uint32_t) factory->source_offset);
        lazy_record_put_u32(at + 4, (uint32_t) factory->source_length);
        at[8] = (unsigned char) gap;
        at[9] = (unsigned char) (gap >> 8);
        at[10] = (unsigned char) factory->key_length;
        at[11] = (unsigned char) (((unsigned) factory->kind << 2)
                                  | factory->arity);
    }
    *record = data;
    *record_length = length;
    return true;
}

bool script_runtime_lazy_webpack_plan(
    ScriptRuntime *runtime, Budget *budget, const char *request_url,
    const char *source, size_t source_length, ScriptLazyWebpackPlan *plan)
{
    if (plan == NULL) return false;
    memset(plan, 0, sizeof(*plan));
    if (budget == NULL || source == NULL || source_length == 0) return false;
    uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    BrowserScriptBytecodeKey key;
    bool keyed = lazy_record_key_init(runtime, request_url, source,
                                      source_length, partition, &key);
    bool hit = false;
    if (keyed) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_record_lookups, 1);
        BrowserSharedBody *record = lazy_record_acquire(runtime, &key);
        if (record != NULL) {
            hit = lazy_record_restore(budget, record->data, record->length,
                                      source, source_length, plan);
            browser_shared_body_release(record);
            if (!hit) {
                /* Another build's record, or one that does not describe
                   these bytes: neither tier keeps it. */
                browser_session_script_bytecode_invalidate(
                    runtime->session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE,
                    &key);
                browser_session_script_disk_discard(
                    runtime->session, BROWSER_SCRIPT_BYTECODE_LAZY_BUNDLE,
                    &key);
            }
        }
    }
    bool planned = hit || script_lazy_webpack_plan_create(
        budget, source, source_length, plan);
    if (planned && keyed && key.digest_ready) {
        plan->digest_ready = true;
        memcpy(plan->source_digest, key.source_digest,
               sizeof(plan->source_digest));
    }
    uint64_t elapsed_us =
        (tilefinch_platform_monotonic_time_ns() - started_ns) / UINT64_C(1000);
    if (runtime != NULL) {
        js_rt_saturating_add_u64(
            &runtime->result.lazy_webpack_plan_total_us, elapsed_us);
        if (hit)
            js_rt_saturating_add_size(
                &runtime->result.lazy_webpack_record_hits, 1);
    }
#ifndef TILEFINCH_NO_TRACE
    /* Factory-cache measurement (TILEFINCH_TRACE_FACTORY_CACHE). */
    if (tilefinch_trace_factory_cache()) {
        printf("census-lazy-plan bytes=%zu planned=%s factories=%zu "
               "factory-bytes=%zu us=%llu record=%s\n", source_length,
               planned ? "yes" : "no", planned ? plan->factory_count : 0,
               planned ? plan->factory_source_bytes : 0,
               (unsigned long long) elapsed_us,
               hit ? "hit" : !keyed ? "none"
                   : key.digest_ready ? "miss-changed" : "miss");
    }
#endif
    return planned;
}

bool script_runtime_lazy_webpack_plan_eligible(
    ScriptRuntime *runtime, Budget *budget, const char *request_url,
    const char *source, size_t source_length, bool module,
    ScriptLazyWebpackPlan *plan)
{
    if (plan == NULL) return false;
    memset(plan, 0, sizeof(*plan));
    if (module || source == NULL
        || source_length < SCRIPT_LAZY_WEBPACK_MINIMUM_BYTES
        || tilefinch_lab_getenv("TILEFINCH_DISABLE_LAZY_WEBPACK") != NULL)
        return false;
    if (!script_runtime_lazy_webpack_plan(runtime, budget, request_url,
                                          source, source_length, plan))
        return false;
    /* A content-shape decision: no URL, host, chunk identifier, or module
       identifier participates. */
    if (plan->factory_source_bytes < source_length / 2) {
        script_lazy_webpack_plan_destroy(plan);
        return false;
    }
    return true;
}

/* Queues the record of a planner-made plan whose registration just
   compiled; the session digests the bytes at idle and stores it. */
static void lazy_record_queue(ScriptRuntime *runtime,
                              const ScriptLazyWebpackPlan *plan,
                              const char *source, size_t source_length,
                              const ScriptLazyBundleRecordTarget *target)
{
    if (target == NULL || target->no_store || target->body == NULL
        || plan->from_record
        || target->body->data != (const unsigned char *) source
        || target->body->length != source_length) return;
    char partition[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
    BrowserScriptBytecodeKey key;
    unsigned char *record = NULL;
    size_t record_length = 0;
    if (!lazy_record_key_init(runtime, target->request_url, source,
                              source_length, partition, &key)
        || !lazy_record_encode(runtime->budget, plan, source_length,
                               &record, &record_length)) return;
    if (plan->digest_ready) {
        key.digest_ready = true;
        memcpy(key.source_digest, plan->source_digest,
               sizeof(key.source_digest));
    }
    key.source = NULL;
    if (browser_session_lazy_bundle_record_queue(
            runtime->session, &key, target->body,
            runtime->module_bytecode_generation, record, record_length)) {
        js_rt_saturating_add_size(
            &runtime->result.lazy_webpack_record_queued, 1);
    }
    budget_free(runtime->budget, record);
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
#ifndef TILEFINCH_NO_TRACE
        bundle->factories[i].census_program = JS_UNDEFINED;
#endif
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
    return script_runtime_evaluate_external_lazy_webpack_recorded(
        runtime, script_node, source, source_length, source_url, plan, NULL,
        source_lease, release_source, result);
}

ScriptLazyEvaluation script_runtime_evaluate_external_lazy_webpack_recorded(
    ScriptRuntime *runtime, lxb_dom_node_t *script_node,
    const char *source, size_t source_length, const char *source_url,
    const ScriptLazyWebpackPlan *plan,
    const ScriptLazyBundleRecordTarget *record,
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
    /* Kept across evaluation for its load or error event. */
    js_rt_bridge_keep_node_handle(&runtime->bridge, script_handle);
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
    if (tilefinch_trace_lazy_scripts()) {
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
#ifndef TILEFINCH_NO_TRACE
    bool census = bundle != NULL && tilefinch_trace_factory_cache();
    uint64_t census_sha_us = 0, census_fnv_us = 0, census_store_us = 0,
             census_register_us = 0;
    if (census) {
        uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
        uint64_t started_ns = js_rt_monotonic_time_ns();
        if (tilefinch_sha256_digest((const uint8_t *) source, source_length,
                                    digest)) {
            for (size_t i = 0; i < 8; i++)
                bundle->census_digest = (bundle->census_digest << 8)
                    | digest[i];
        }
        census_sha_us = census_elapsed_us(started_ns);
        started_ns = js_rt_monotonic_time_ns();
        volatile uint64_t fnv = census_fnv64(
            (const unsigned char *) source, source_length);
        (void) fnv;
        census_fnv_us = census_elapsed_us(started_ns);
    }
    uint64_t census_store_started_ns = js_rt_monotonic_time_ns();
#endif
    size_t preflight_attempts_before =
        runtime->result.lazy_webpack_syntax_preflight_attempts;
    size_t preflight_bytes_before =
        runtime->result.lazy_webpack_syntax_preflight_source_bytes;
    uint64_t preflight_us_before =
        runtime->result.lazy_webpack_syntax_preflight_total_us;
    bool ready = bundle != NULL
        && lazy_store_factory_sources(runtime, bundle);
#ifndef TILEFINCH_NO_TRACE
    census_store_us = census_elapsed_us(census_store_started_ns);
#endif
    if (ready) {
        /* Registration retains only exact, independently-compressed factory
           source; a callable is compiled (and its syntax checked) only when
           its stable wrapper is used. */
        if (tilefinch_trace_lazy_scripts()) {
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
#ifndef TILEFINCH_NO_TRACE
    uint64_t census_register_started_ns = js_rt_monotonic_time_ns();
#endif
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
#ifndef TILEFINCH_NO_TRACE
    census_register_us = census_elapsed_us(census_register_started_ns);
    if (census) {
        printf("census-lazy-bundle digest=%016llx ready=%s bytes=%zu "
               "factories=%zu factory-bytes=%zu rendered=%zu "
               "preflight-count=%zu preflight-bytes=%zu preflight-us=%llu "
               "store-us=%llu register-us=%llu sha-us=%llu fnv-us=%llu "
               "url=%.300s\n",
               (unsigned long long) bundle->census_digest,
               ready ? "yes" : "no", source_length, plan->factory_count,
               plan->factory_source_bytes, rendered_length,
               runtime->result.lazy_webpack_syntax_preflight_attempts
                   - preflight_attempts_before,
               runtime->result.lazy_webpack_syntax_preflight_source_bytes
                   - preflight_bytes_before,
               (unsigned long long)
                   (runtime->result.lazy_webpack_syntax_preflight_total_us
                    - preflight_us_before),
               (unsigned long long) census_store_us,
               (unsigned long long) census_register_us,
               (unsigned long long) census_sha_us,
               (unsigned long long) census_fnv_us,
               source_url == NULL ? "" : source_url);
    }
#endif
    budget_free(runtime->budget, rendered);
    if (!ready) {
        JS_FreeValue(runtime->context, compiled_registration);
        lazy_bundle_destroy(runtime->context, runtime->budget, bundle);
        js_rt_saturating_add_size(&runtime->result.lazy_webpack_fallbacks, 1);
        JS_RunGC(runtime->runtime);
        (void) budget_quickjs_pool_trim(runtime->quickjs_pool, 0);
        if (tilefinch_trace_lazy_scripts()) {
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
    /* The registration compiled: the plan is what a later load of these
       exact bytes may skip. */
    lazy_record_queue(runtime, plan, source, source_length, record);
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
    if (tilefinch_trace_lazy_scripts()) {
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
    bool threw = !evaluated;
    /* A registration run that threw, reported to the window error event,
       still ran: the element gets `load` (HTML "execute the script
       element"). */
    bool reported = false;
    JS_FreeValue(runtime->context, evaluated_value);
    if (threw) {
        bool previous_muted = runtime->script_errors_muted;
        bool outer_reported = runtime->script_error_reported;
        bool outer_author_error = runtime->script_author_error;
        runtime->script_errors_muted = js_rt_script_errors_muted(
            runtime, script_node, source_url);
        reported = js_rt_record_script_exception(
            runtime->context,
            source_url == NULL ? "<external-script>" : source_url,
            SCRIPT_COMPILE_SOURCE_EXTERNAL, &runtime->result);
        runtime->script_errors_muted = previous_muted;
        runtime->script_error_reported = outer_reported;
        runtime->script_author_error = outer_author_error;
    }
    if (evaluated) evaluated = js_rt_runtime_run_jobs(runtime);
    else if (reported) reported = js_rt_runtime_run_jobs(runtime);
    if (!js_rt_current_script_scope_end(
            runtime->context, &previous_current_script,
            &runtime->result)) evaluated = false;
    if (!evaluated) {
        if (!threw) js_rt_record_exception(runtime->context, &runtime->result);
        js_rt_capture_error_source_context(source, source_length, source_url,
                                     &runtime->result);
    }
    js_lazy_webpack_recover_failure(runtime);
    lazy_bundle_release_source(bundle);
    lazy_trace_memory(runtime, "source-released", source_url,
                      bundle->compressed_source_length,
                      plan->factory_count);
    if (evaluated) evaluated = js_rt_runtime_refresh(runtime);
    lazy_trace_memory(runtime, "executed", source_url, source_length,
                      plan->factory_count);
    if (!evaluated) {
        (void) script_runtime_collect_and_trim(runtime);
        runtime->lazy_factory_recovery_pending = false;
        runtime->result.external_scripts_failed++;
        runtime->result.success = true;
        size_t slot = 0;
        if (!script_runtime_document_refresh_failed(runtime)
            && js_rt_bridge_node_slot_for_handle(
                &runtime->bridge, script_handle, &slot)) {
            (void) script_runtime_dispatch_node(
                runtime, runtime->bridge.nodes[slot],
                reported ? "load" : "error", NULL);
        }
        js_rt_runtime_update_result(runtime, result);
        runtime->last_script_author_error = reported;
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
    runtime->last_script_author_error = false;
    return dispatched ? SCRIPT_LAZY_EVALUATION_SUCCEEDED
                      : SCRIPT_LAZY_EVALUATION_FAILED;
}
