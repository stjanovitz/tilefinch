/* Actual-frame differential probe. Generated interpreter hooks exist only in
   this target, and the emitter consumes engine-produced bytecode, not source
   names or captured application identifiers. ARM64/Allegrex, not shipping. */
#ifdef __PSP__
#include <pspkernel.h>
#include <psppower.h>
#include <malloc.h>
#include <stdlib.h>
/* Define the backend before QuickJS intentionally forbids raw malloc/free in
   its own code. The code-pool reservation owns this aligned allocation. */
static void psp_region_unmap(void *memory) { free(memory); }
#ifndef TF_REGION_LIVE_ENGINE
PSP_MODULE_INFO("Tilefinch Region Probe", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
PSP_HEAP_SIZE_KB(16384);
#endif
#else
#include <sys/mman.h>
#endif
#include "native-tier-region-quickjs.c"
#include "tilefinch/budget_quickjs.h"
#include <unistd.h>
#include <time.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stdout, "region check failed at %d: %s\n", __LINE__, #value); \
    fflush(stdout); exit(1); \
} } while (0)

#ifdef __PSP__
#define REGION_ITERATIONS 20000
#define REGION_SAMPLES 5
#else
#define REGION_ITERATIONS 200000
#define REGION_SAMPLES 9
#endif

#ifdef TF_REGION_INTERPRETER_ONLY
#define PROBE_LANES 1u
#else
#define PROBE_LANES 2u
#endif
#ifdef TF_REGION_FRAME_LEASE
#define PROBE_FRAME_LEASE 1u
#else
#define PROBE_FRAME_LEASE 0u
#endif
#ifdef TF_REGION_TABLE_DISPATCH
#define PROBE_TABLE_DISPATCH 1u
#else
#define PROBE_TABLE_DISPATCH 0u
#endif
#ifdef TF_REGION_LIVE_ENGINE
static bool direct_emission = true;
#else
static bool direct_emission;
#endif
static bool profile_native;
static unsigned profile_property;
static bool predicates_only;
static bool map_emission;
static bool mixed_workload;
static bool nested_only;
static bool reference_blocks;
static bool reference_only;
#ifdef __PSP__
static bool cached_bindings;
static bool fused_fields;
static bool basic_blocks;
static bool cold_blocks;
static bool value_blocks;
static bool value_block_probes;
static bool borrowed_fields;
static bool register_cfg;
static bool cfg_owned_stores;
static bool cfg_owned_probe_exit; /* inspect exactly one materialized store */
static bool cfg_placement; /* cross-swap byte-identical images, not lowering */
static bool borrowed_field_probes;
static bool borrowed_probe_exit; /* raw emitted-prefix tests only */
static unsigned borrowed_field_hits;
#endif

static void block_census_model_tests(void)
{
    TFValueBlockCensus c = {0};
    unsigned body_a, body_b;
    tf_value_block_record(&c, &body_a, 0, 1, 1, 0, 1);
    tf_value_block_record(&c, &body_a, 1, 2, 1, 0, 1);
    tf_value_block_record(&c, &body_a, 3, 1, 4, 2, 1);
    tf_value_block_record(&c, &body_a, 4, 1, 2, 1, 0);
    tf_value_block_finish(&c);
    CHECK(c.total == 4 && c.eligible == 4 && c.runs == 1 && c.histogram[4] == 1);
    CHECK(c.kinds[7] == 4 && c.maximum_inputs == 0 && c.maximum_capacity == 2);
    /* Explicit call boundary: even recursive calls with the same body cannot
       stitch two frames into a fictitious longer block. */
    tf_value_block_record(&c, &body_a, 5, 1, 1, 0, 1);
    tf_value_block_finish(&c);
    tf_value_block_record(&c, &body_a, 6, 1, 2, 1, 0);
    tf_value_block_record(&c, &body_b, 7, 1, 1, 0, 1);
    tf_value_block_record(&c, &body_b, 12, 1, 1, 0, 1); /* branch/discontinuity */
    tf_value_block_record(&c, &body_b, 13, 1, 0, 0, 0);
    CHECK(c.total == 9 && c.eligible == 8 && c.excluded == 1 && c.runs == 5);
    CHECK(c.histogram[1] == 4 && c.maximum_inputs == 1);
    memset(&c, 0, sizeof(c));
    for (unsigned i = 0; i < 35; i++)
        tf_value_block_record(&c, &body_a, i, 1, 1, 0, 1);
    tf_value_block_finish(&c);
    CHECK(c.total == 35 && c.eligible == 35 && c.runs == 3);
    CHECK(c.histogram[16] == 2 && c.histogram[3] == 1 && c.maximum_capacity == 16);
    memset(&c, 0, sizeof(c));
    tf_value_block_record(&c, &body_a, 0, 1, 8, 2, 1);
    tf_value_block_record(&c, &body_a, 1, 3, 16, 66, 1);
    tf_value_block_finish(&c);
    CHECK(c.histogram[2] == 1 && c.kinds[24] == 2 && c.maximum_inputs == 67);
    puts("region bounded value-block census, call boundaries and stack effects=pass");
}

typedef struct { JSRuntime *rt; size_t ceiling, reserved; bool refuse; } ProbeHeap;
static bool reserve(void *opaque, size_t bytes)
{
    ProbeHeap *heap = opaque;
    JSMallocState *state = &heap->rt->malloc_ctx.malloc_state;
    if (heap->refuse || heap->reserved > heap->ceiling) return false;
    size_t limit = heap->ceiling - heap->reserved;
    if (state->malloc_limit != limit || state->malloc_size > limit || bytes > limit - state->malloc_size) return false;
    heap->reserved += bytes;
    state->malloc_limit = heap->ceiling - heap->reserved;
    return true;
}
static size_t reserved_limit(const ProbeHeap *heap)
{
    return heap->reserved > heap->ceiling ? 0 : heap->ceiling - heap->reserved;
}
static void release(void *opaque, size_t bytes)
{
    ProbeHeap *heap = opaque;
    CHECK(bytes <= heap->reserved);
    CHECK(heap->rt->malloc_ctx.malloc_state.malloc_limit == reserved_limit(heap));
    heap->reserved -= bytes;
    heap->rt->malloc_ctx.malloc_state.malloc_limit = reserved_limit(heap);
}
static void *map_code(void *opaque, size_t bytes)
{
    (void)opaque;
#ifdef __PSP__
    /* Charged by the code pool's Budget reservation before this allocation.
       PSP RAM has no W^X protection; pages are never exposed to author JS. */
    return memalign(64, bytes);
#else
    void *result = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return result == MAP_FAILED ? NULL : result;
#endif
}
static bool publish_code(void *opaque, void *memory, size_t bytes)
{
    (void)opaque;
#ifdef __PSP__
    sceKernelDcacheWritebackInvalidateRange(memory, bytes);
    sceKernelIcacheInvalidateRange(memory, bytes);
    return true; /* SDK cache APIs are void; execution is the publication proof */
#else
    if (mprotect(memory, bytes, PROT_READ | PROT_EXEC)) return false;
    __builtin___clear_cache(memory, (char *)memory + bytes);
    return true;
#endif
}
static bool unmap_code(void *opaque, void *memory, size_t bytes)
{
    (void)opaque;
#ifdef __PSP__
    (void)bytes;
    psp_region_unmap(memory);
    return true;
#else
    return munmap(memory, bytes) == 0;
#endif
}

static size_t code_page_size(void)
{
#ifdef __PSP__
    return 64; /* data/instruction-cache aligned, not fictitious MMU pages */
#else
    long bytes = sysconf(_SC_PAGESIZE);
    return bytes > 0 ? (size_t)bytes : 0;
#endif
}

typedef struct {
    uint32_t *words;
    unsigned count;
    bool refused;
#ifdef __PSP__
    unsigned binding_kind, binding_index; /* 0=none, 1=local, 2=argument */
    bool full_save;
#endif
} Emitter;
static void map_range(Emitter *e, unsigned *begin, const char *opcode, const char *phase)
{
    if (map_emission && e->count > *begin)
        printf("region code-map begin=%u end=%u opcode=%s phase=%s\n",
            *begin * 4, e->count * 4, opcode, phase);
    *begin = e->count;
}
static void emit(Emitter *emitter, uint32_t word)
{
    if (emitter->count == TF_REGION_CODE_LIMIT / sizeof(uint32_t)) { emitter->refused = true; return; }
    emitter->words[emitter->count++] = word;
}
#ifdef __PSP__
#include "native-tier-mips.inc"
#else
static void immediate32(Emitter *emitter, unsigned reg, uint32_t value)
{
    emit(emitter, 0x52800000u | ((value & 0xffff) << 5) | reg); /* movz wN */
    if (value >> 16) emit(emitter, 0x72a00000u | ((value >> 16) << 5) | reg); /* movk wN,lsl16 */
}
static void emit_address(Emitter *emitter, uintptr_t address)
{
    emit(emitter, 0xd2800010u | ((address & 0xffff) << 5));
    for (unsigned half = 1; half < 4; half++)
        emit(emitter, 0xf2800010u | (half << 21) | (((address >> (16 * half)) & 0xffff) << 5));
}
static void helper_address(Emitter *emitter, TFRegionHelper helper)
{
    uintptr_t address;
    _Static_assert(sizeof(address) == sizeof(helper), "ARM64 helper-pointer ABI");
    memcpy(&address, &helper, sizeof(address));
    emit_address(emitter, address);
}
static void own_value_address(Emitter *emitter)
{
    JSValue (*helper)(JSValue, uint32_t) = tf_region_own_value;
    uintptr_t address;
    _Static_assert(sizeof(address) == sizeof(helper), "ARM64 value helper-pointer ABI");
    memcpy(&address, &helper, sizeof(address));
    emit_address(emitter, address);
}

#include "native-tier-arm64.inc"
#include "native-tier-chain.inc"
#endif

static size_t compile_scratch_bytes(unsigned length, bool chained)
{
    size_t bytes = TF_REGION_CODE_LIMIT + (chained ? sizeof(TFChainFixups) : 0);
#ifdef TF_REGION_COMPACT_PLAN
    if (chained) bytes += length;
#else
    (void)length;
#endif
    return bytes;
}

static bool prepare_reference_blocks(Budget *budget, ProbeHeap *heap, TFRegionPlan *plan)
{
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    if (!reference_blocks || !plan->chained) return true;
    unsigned length = plan->body->byte_code_len, count = 0;
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[plan->body->byte_code_buf[at]].size;
        if (!size || size > length - at) return false;
        plan->steps[at] = 1;
        at += size;
    }
    for (unsigned at = 0; at < length && count < TF_REFERENCE_BLOCK_LIMIT;) {
        TFPrimitiveBlock b;
        if (tf_reference_scan(plan->body,plan->steps,at,&b)) { count++; at = b.end; }
        else at += tf_region_opcodes[plan->body->byte_code_buf[at]].size;
    }
    if (!count) return true;
    size_t bytes = count * sizeof(TFPrimitiveBlock); /* count <= 128 */
    if (!reserve(heap,bytes)) return false;
    plan->reference_bytes = bytes;
    plan->reference_blocks = budget_calloc_category(budget,BUDGET_CATEGORY_JAVASCRIPT,1,bytes);
    if (!plan->reference_blocks) return false;
    for (unsigned at = 0; at < length && plan->reference_count < count;) {
        TFPrimitiveBlock *b = &plan->reference_blocks[plan->reference_count];
        if (tf_reference_scan(plan->body,plan->steps,at,b)) { plan->reference_count++; at = b->end; }
        else at += tf_region_opcodes[plan->body->byte_code_buf[at]].size;
    }
    return plan->reference_count == count;
#else
    (void)budget; (void)heap; (void)plan;
    return true;
#endif
}

/* Bound source walking, emission, scratch and ownership independently. A plan
   only roots one function; closures/atoms/bytecode follow that existing root. */
static TFRegionPlan *compile_plan(JSContext *ctx, Budget *budget, ProbeHeap *heap,
                                  NativeCodePool *pool, JSValueConst function)
{
    if (!JS_IsObject(function)) return NULL;
    JSObject *object = JS_VALUE_GET_OBJ(function);
    if (object->class_id != JS_CLASS_BYTECODE_FUNCTION) return NULL;
    JSFunctionBytecode *body = object->u.func.function_bytecode;
    if (body->is_lazy || body->func_kind != JS_FUNC_NORMAL ||
        !body->byte_code_len || body->byte_code_len > TF_REGION_BYTECODE_LIMIT) return NULL;
    const size_t plan_bytes = tf_region_plan_bytes(body->byte_code_len, chained_emission);
    const size_t scratch_bytes = compile_scratch_bytes(body->byte_code_len, chained_emission);
    const size_t charge = plan_bytes + scratch_bytes; /* both independently capped above */
    if (!reserve(heap, charge)) return NULL;
    TFRegionPlan *plan = budget_calloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT, 1, plan_bytes);
    uint32_t *scratch = budget_calloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT, 1, scratch_bytes);
    if (!plan || !scratch) goto fail;
    plan->root = JS_UNDEFINED;
    plan->body = body;
    plan->pool = pool;
    plan->chained = chained_emission;
#ifdef TF_REGION_COMPACT_PLAN
    plan->steps = chained_emission ? (uint8_t *)scratch + TF_REGION_CODE_LIMIT + sizeof(TFChainFixups) :
        (uint8_t *)(plan->entry + body->byte_code_len);
#endif
    if (!prepare_reference_blocks(budget,heap,plan)) goto fail;
    Emitter emitter = {.words = scratch, .count = 4}; /* readable zero prefix for UBSan */
    if (chained_emission) {
        TFChainFixups *fix = (TFChainFixups *)(scratch + TF_REGION_CODE_LIMIT / 4);
        if (!emit_chain(&emitter, plan, fix)) goto fail;
    } else {
#ifdef __PSP__
        goto fail; /* this backend only admits the retained-frame ABI */
#else
    unsigned offset = 0;
    while (offset < (unsigned)body->byte_code_len) {
        unsigned start = offset, steps = 0;
        uint32_t argument;
        while (offset < (unsigned)body->byte_code_len && steps < TF_REGION_STEP_LIMIT) {
            unsigned size = tf_region_opcodes[body->byte_code_buf[offset]].size;
            if (!size || size > (unsigned)body->byte_code_len - offset) goto fail;
            if (!tf_region_decode(body->byte_code_buf + offset, &argument)) break;
            offset += size;
            steps++;
        }
        if (steps >= 2) {
            if (!tf_region_set_entry(plan, start, emitter.count * 4)) goto fail;
            plan->steps[start] = steps;
            plan->entries++;
            plan->instructions += steps;
            if (direct_emission) {
                emit_direct_region(&emitter, body, start, offset);
                if (emitter.refused) goto fail;
            } else {
            emit(&emitter, 0xd503245fu); /* bti c */
            emit(&emitter, 0xa9bf7bf3u); /* stp x19,x30,[sp,#-16]! */
            emit(&emitter, 0xaa0003f3u); /* mov x19,x0: persistent frame */
            unsigned branches[TF_REGION_STEP_LIMIT], count = 0;
            for (unsigned at = start; at < offset;) {
                unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
                TFRegionHelper helper = tf_region_decode(body->byte_code_buf + at, &argument);
                emit(&emitter, 0xaa1303e0u); /* mov x0,x19 */
                immediate32(&emitter, 1, argument);
                immediate32(&emitter, 2, at + size);
                helper_address(&emitter, helper);
                emit(&emitter, 0xd63f0200u); /* blr x16 */
                branches[count++] = emitter.count;
                emit(&emitter, 0); /* cbz w0, shared exit (guard before effects) */
                at += size;
            }
            unsigned exit = emitter.count;
            emit(&emitter, 0xa8c17bf3u); /* ldp x19,x30,[sp],#16 */
            emit(&emitter, 0xd65f03c0u); /* ret */
            if (emitter.refused) goto fail;
            for (unsigned i = 0; i < count; i++)
                scratch[branches[i]] = 0x34000000u | ((exit - branches[i]) << 5);
            }
        }
        if (offset == start || steps < TF_REGION_STEP_LIMIT) {
            if (offset == (unsigned)body->byte_code_len) break;
            unsigned size = tf_region_opcodes[body->byte_code_buf[offset]].size;
            if (!size || size > (unsigned)body->byte_code_len - offset) goto fail;
            offset += size;
        }
    }
#endif
    }
    if (!plan->entries) goto fail;
    plan->code_bytes = emitter.count * 4;
    size_t image_bytes = plan->code_bytes;
#if defined(__PSP__) && !defined(TF_REGION_LIVE_ENGINE)
    /* Match heap placement across emitter variants in this isolated fixture.
       Different mapping sizes otherwise move later JS objects and can change
       even the interpreted control. Charge all padding; never a browser policy. */
    if (image_bytes < 16u * 1024u) image_bytes = 16u * 1024u;
#endif
    if (!native_code_pool_allocate(pool, image_bytes, &plan->image)) goto fail;
    if (!native_code_pool_write(pool, plan->image, 0, scratch, plan->code_bytes) ||
        !native_code_pool_publish(pool, plan->image)) {
        (void)native_code_pool_retire(pool, plan->image);
        goto fail;
    }
    plan->root = JS_DupValue(ctx, function);
    plan->enabled = true;
#ifdef TF_REGION_COMPACT_PLAN
    if (plan->chained) plan->steps = NULL;
#endif
    budget_free(budget, scratch);
    release(heap, scratch_bytes);
    return plan;
fail:
    if (plan && plan->reference_bytes) {
        budget_free(budget,plan->reference_blocks);
        release(heap,plan->reference_bytes);
    }
    budget_free(budget, scratch);
    budget_free(budget, plan);
    release(heap, charge);
    return NULL;
}
static void destroy_plan_runtime(JSRuntime *rt, Budget *budget, ProbeHeap *heap, TFRegionPlan *plan)
{
    if (!plan) return;
    CHECK(tf_region_plan != plan);
#ifndef TF_REGION_LIVE_ENGINE
    CHECK(tf_region_callee_plan != plan);
#endif
    CHECK(plan->active_frames == 0);
    if (!plan->retired) CHECK(native_code_pool_retire(plan->pool, plan->image));
    size_t bytes = tf_region_plan_bytes(plan->body->byte_code_len, plan->chained);
    if (plan->reference_bytes) {
        budget_free(budget,plan->reference_blocks);
        release(heap,plan->reference_bytes);
    }
    JS_FreeValueRT(rt, plan->root);
    budget_free(budget, plan);
    release(heap, bytes);
}

static void destroy_plan(JSContext *ctx, Budget *budget, ProbeHeap *heap, TFRegionPlan *plan)
{
    destroy_plan_runtime(JS_GetRuntime(ctx), budget, heap, plan);
}

#ifndef TF_REGION_LIVE_ENGINE
static void plan_storage_tests(void)
{
    Budget budget;
    budget_init(&budget, 128u * 1024u);
    CHECK(tf_region_plan_bytes(0, true) == 0);
    CHECK(tf_region_plan_bytes(TF_REGION_BYTECODE_LIMIT + 1, true) == 0);
    const unsigned lengths[] = {1, 17, TF_REGION_BYTECODE_LIMIT};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        for (unsigned chained = 0; chained < 2; chained++) {
            unsigned length = lengths[i];
            size_t bytes = tf_region_plan_bytes(length, chained);
            TFRegionPlan *plan = budget_calloc_category(&budget, BUDGET_CATEGORY_JAVASCRIPT, 1, bytes);
            CHECK(plan != NULL);
            JSFunctionBytecode body = {0};
            body.byte_code_len = length;
            plan->body = &body;
#ifdef TF_REGION_COMPACT_PLAN
            CHECK(bytes == sizeof(*plan) + length * (sizeof(TFRegionOffset) + (chained ? 0u : 1u)));
            if (!chained) {
                plan->steps = (uint8_t *)(plan->entry + length);
                memset(plan->steps, TF_REGION_STEP_LIMIT, length);
            }
#endif
            CHECK(tf_region_set_entry(plan, 0, 16));
            CHECK(plan->entry[0] == 17);
            CHECK(tf_region_set_entry(plan, length - 1, TF_REGION_CODE_LIMIT - 4));
            CHECK(plan->entry[length - 1] == TF_REGION_CODE_LIMIT - 3);
            CHECK(!tf_region_set_entry(plan, length, 16));
            CHECK(!tf_region_set_entry(plan, length - 1, TF_REGION_CODE_LIMIT));
            CHECK(!tf_region_set_entry(plan, length - 1, UINT_MAX));
            CHECK(!tf_region_set_entry(plan, length - 1, 19));
            CHECK(plan->entry[length - 1] == TF_REGION_CODE_LIMIT - 3);
#ifdef TF_REGION_COMPACT_PLAN
            if (!chained) for (unsigned at = 0; at < length; at++)
                CHECK(plan->steps[at] == TF_REGION_STEP_LIMIT);
#endif
            budget_free(&budget, plan);
            CHECK(budget.current == 0);
        }
    }
    puts("region exact-plan bounds and offset narrowing=pass");
}

static void plan_refusal_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap,
                               NativeCodePool *pool, JSValueConst function)
{
    size_t owned = budget->current, reserved = heap->reserved;
    size_t pool_bytes = native_code_pool_charged_bytes(pool);
    /* Refuse the plan, compile scratch, then executable-page reservation.
       Every path must return both the Budget and JS admission to baseline. */
    for (unsigned allocation = 0; allocation < 3 + reference_blocks; allocation++) {
        budget_inject_failure_after(budget, allocation);
        TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
        budget_clear_failure_injection(budget);
        CHECK(plan == NULL);
        CHECK(budget->current == owned && heap->reserved == reserved);
        CHECK(native_code_pool_charged_bytes(pool) == pool_bytes);
    }
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan != NULL);
    CHECK(heap->reserved - reserved == tf_region_plan_bytes(plan->body->byte_code_len, plan->chained) +
        plan->reference_bytes + native_code_pool_charged_bytes(pool) - pool_bytes);
#ifdef TF_REGION_COMPACT_PLAN
    CHECK(!plan->chained || plan->steps == NULL);
#endif
    destroy_plan(ctx, budget, heap, plan);
    CHECK(budget->current == owned && heap->reserved == reserved);
    puts("region plan allocation refusal and exact ownership=pass");
}

static JSValue eval(JSContext *ctx, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "<region-fixture>", 0);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, exception);
        fprintf(stderr, "fixture setup: %s\n", message ? message : "exception");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, exception);
        exit(1);
    }
    return value;
}
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
static void primitive_guard_tests(JSContext *ctx)
{
    bool selected = chained_emission;
    chained_emission = true;
    JSValue function = eval(ctx, "(function(a){return a??globalThis.blockFallback})");
    JSFunctionBytecode *body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    TFPrimitiveBlock block;
    CHECK(tf_primitive_scan(body, NULL, 0, &block));
    CHECK(block.count == 4 && block.incoming == 0 && block.peak == 2);
    unsigned target = block.target;
    uint8_t boundaries[TF_REGION_BYTECODE_LIMIT] = {0};
    CHECK(!tf_primitive_scan(body, boundaries, 0, &block));
    boundaries[target] = 1;
    CHECK(tf_primitive_scan(body, boundaries, 0, &block));
    static const char *inputs[] = {"undefined", "null", "false", "true", "0", "-7", "1.5", "'x'", "({})",
        "-2147483648", "2147483647", "-0", "NaN", "Infinity", "''", "'\\u{1f600}'",
        "0n", "1n", "1n<<80n", "Symbol('x')", "Object.create(null)"};
    unsigned cases = 0;
    int counter = ctx->interrupt_counter;
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); i++) {
        JSValue args[1] = {eval(ctx, inputs[i])};
        /* Literal negation may preserve FLOAT64 at INT32_MIN; explicitly
           construct immediate integer boundary values for this guard case. */
        if (i == 9 || i == 10) args[0] = JS_NewInt32(ctx, i == 9 ? INT32_MIN : INT32_MAX);
        bool primitive = i <= 5 || i == 9 || i == 10;
        for (unsigned capacity = 0; capacity <= 2; capacity++) {
            for (unsigned slow = 0; slow < 2; slow++) {
                JSValue stack[2] = {JS_UNDEFINED, JS_UNDEFINED};
                JSValue original = args[0];
                ctx->interrupt_counter = slow ? 1 : 19;
                JSMallocState *heap = &ctx->rt->malloc_ctx.malloc_state;
                size_t bytes = heap->malloc_size, allocations = heap->malloc_count;
                int refs = JS_VALUE_HAS_REF_COUNT(original) ? __js_rc(JS_VALUE_GET_PTR(original))->ref_count : 0;
                unsigned result = tf_primitive_guards(ctx, body, &block, stack, stack+capacity,
                                                        stack, NULL, args, NULL, NULL);
                unsigned expected = capacity < 2 ? TF_PRIMITIVE_STACK : !primitive ? TF_PRIMITIVE_TYPE :
                    slow ? TF_PRIMITIVE_POLL : TF_PRIMITIVE_ACCEPT;
                if (result != expected)
                    printf("region primitive-oracle input=%u capacity=%u slow=%u tag=%d result=%u expected=%u\n",
                        i, capacity, slow, JS_VALUE_GET_NORM_TAG(args[0]), result, expected);
                CHECK(result == expected);
                CHECK(heap->malloc_size == bytes && heap->malloc_count == allocations);
                CHECK(!memcmp(&original, args, sizeof(original)));
                CHECK(JS_IsUndefined(stack[0]) && JS_IsUndefined(stack[1]));
                if (refs) CHECK(__js_rc(JS_VALUE_GET_PTR(original))->ref_count == refs);
                cases++;
            }
        }
        JS_FreeValue(ctx, args[0]);
    }
    CHECK(tf_primitive_scan(body, NULL, 1, &block) && block.count == 3 && block.incoming == 1);
    JSValue incoming[2] = {eval(ctx, "({})"), JS_UNDEFINED};
    int refusal_tag;
    CHECK(tf_primitive_guards(ctx, body, &block, incoming, incoming+2, incoming+1,
        NULL, NULL, NULL, &refusal_tag) == TF_PRIMITIVE_INPUT_TYPE);
    CHECK(refusal_tag == JS_TAG_OBJECT);
    JS_FreeValue(ctx, incoming[0]);
    JS_FreeValue(ctx, function);
    function = eval(ctx, "(function(a,b){if(a<b)return 1;return 0})");
    body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    CHECK(tf_primitive_scan(body, NULL, 0, &block) && block.count == 4);
    JSValue args[2] = {JS_NewBool(ctx, true), JS_NewInt32(ctx, 2)};
    JSValue stack[2];
    ctx->interrupt_counter = 19;
    CHECK(tf_primitive_guards(ctx, body, &block, stack, stack+2, stack, NULL, args, NULL, NULL) == TF_PRIMITIVE_INTEGER);
    args[0] = JS_NewInt32(ctx, 1);
    CHECK(tf_primitive_guards(ctx, body, &block, stack, stack+2, stack, NULL, args, NULL, NULL) == TF_PRIMITIVE_ACCEPT);
    ctx->interrupt_counter = counter;
    JS_FreeValue(ctx, function);
    chained_emission = selected;
    printf("region read-only primitive block guard oracle=pass cases=%u\n", cases+3);
}

static void reference_guard_tests(JSContext *ctx)
{
    /* Frontier reporting must distinguish the unexecuted next instruction
       from the accepted prefix, including its artificial capacity limits. */
    static const struct { uint8_t op; unsigned expected; } frontiers[] = {
        {OP_put_loc8,TF_REFERENCE_BINDING_WRITE},
        {OP_set_loc_uninitialized,TF_REFERENCE_BINDING_WRITE},
        {OP_put_arg,TF_REFERENCE_BINDING_WRITE},
        {OP_put_var_ref,TF_REFERENCE_BINDING_WRITE},
        {OP_call0,TF_REFERENCE_CALL},{OP_call_method,TF_REFERENCE_CALL},
        {OP_tail_call,TF_REFERENCE_CALL},{OP_call_constructor,TF_REFERENCE_CALL},
        {OP_put_field,TF_REFERENCE_PROPERTY_WRITE},
        {OP_put_array_el,TF_REFERENCE_PROPERTY_WRITE},
        {OP_return,TF_REFERENCE_RETURN},{OP_return_undef,TF_REFERENCE_RETURN},
        {OP_push_i8,TF_REFERENCE_STACK_LIMIT},{OP_dup,TF_REFERENCE_STACK_LIMIT},
        {OP_nop,TF_REFERENCE_OTHER}
    };
    for (unsigned i = 0; i < sizeof(frontiers)/sizeof(frontiers[0]); i++) {
        uint8_t code[8] = {OP_nop,frontiers[i].op};
        JSFunctionBytecode body = {.byte_code_buf=code,.byte_code_len=sizeof(code),
                                  .var_count=1,.arg_count=1,.closure_var_count=1};
        TFPrimitiveBlock block = {.count=2,.end=1};
        CHECK(tf_reference_frontier(&body,&block) == frontiers[i].expected);
        block.count = TF_PRIMITIVE_BLOCK_LIMIT;
        CHECK(tf_reference_frontier(&body,&block) == TF_REFERENCE_COUNT_LIMIT);
        block.conditional = true;
        CHECK(tf_reference_frontier(&body,&block) == TF_REFERENCE_CONDITIONAL);
    }
    puts("region reference frontier conditional, write, call, return and capacity accounting=pass");
    bool selected = chained_emission;
    chained_emission = true;
    int counter = ctx->interrupt_counter;
    static const char *sources[] = {
        "(function(a){if(a)return 1;return 0})",
        "(function(a){if(!a)return 1;return 0})",
        "(function(a){if(a.value)return 1;return 0})",
        "(function(a){if(a.value===a.value)return 1;return 0})",
        "(function(a){if(a.length)return 1;return 0})",
        "(function(a){if(a===a)return 1;return 0})",
        "(function(a){if(a??false)return 1;return 0})"
    };
    static const char *inputs[] = {
        "undefined", "null", "false", "true", "0", "-7", "1.5", "-0", "NaN", "Infinity",
        "''", "'x'", "'\\u{1f600}'", "Symbol('x')", "0n", "1n", "1n<<80n",
        "({value:1})", "({value:null})", "({value:{}})", "Object.create({value:3})",
        "({get value(){throw Error('getter must not run')}})",
        "new Proxy({value:3},{get(){throw Error('proxy must not run')}})",
        "Object.create(null)", "({value:'x',length:0})", "({value:1.5,length:2})"
    };
    unsigned cases = 0, accepted = 0;
    for (unsigned s = 0; s < sizeof(sources)/sizeof(sources[0]); s++) {
        JSValue function = eval(ctx, sources[s]);
        JSFunctionBytecode *body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
        TFPrimitiveBlock block;
        CHECK(tf_value_scan(body, NULL, 0, &block, true));
        for (unsigned a = 0; a < sizeof(inputs)/sizeof(inputs[0]); a++) {
            JSValue args[1] = {eval(ctx, inputs[a])};
            for (unsigned capacity = 0; capacity <= 3; capacity++) {
                for (unsigned slow = 0; slow < 2; slow++) {
                    JSValue stack[3] = {JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                    TFReferenceResult result;
                    ctx->interrupt_counter = slow ? 1 : 19;
                    JSMallocState *heap = &ctx->rt->malloc_ctx.malloc_state;
                    size_t bytes = heap->malloc_size, allocations = heap->malloc_count;
                    int rc = JS_VALUE_HAS_REF_COUNT(args[0]) ? __js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count : 0;
                    unsigned status = tf_reference_guards(ctx, body, &block, stack, stack+capacity,
                        stack, NULL, args, NULL, &result);
                    CHECK(heap->malloc_size == bytes && heap->malloc_count == allocations);
                    CHECK(JS_IsUndefined(stack[0]) && JS_IsUndefined(stack[1]) && JS_IsUndefined(stack[2]));
                    if (rc) CHECK(__js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count == rc);
                    if (capacity < block.peak) CHECK(status == TF_REFERENCE_STACK);
                    else if (slow) CHECK(status == TF_REFERENCE_POLL);
                    else if (status == TF_REFERENCE_ACCEPT) {
                        for (unsigned i = 0; i < result.ref_count; i++)
                            CHECK(result.refs[i].header->ref_count == result.refs[i].initial);
                        /* The nullish short-circuit's first branch is internal;
                           all other fixtures return the predicted condition. */
                        if (s != 6) {
                            JSValue actual = JS_Call(ctx, function, JS_UNDEFINED, 1, args);
                            CHECK(!JS_IsException(actual));
                            CHECK(JS_VALUE_GET_INT(actual) == result.condition);
                            JS_FreeValue(ctx, actual);
                        }
                        accepted++;
                    }
                    cases++;
                }
            }
            JS_FreeValue(ctx, args[0]);
        }
        JS_FreeValue(ctx, function);
    }
    /* Incoming owned values differ from rooted binding reads: consuming a
       sole reference must fall back, even if a child property survives. */
    JSValue function = eval(ctx, sources[2]);
    JSFunctionBytecode *body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    TFPrimitiveBlock block;
    CHECK(tf_value_scan(body, NULL, 1, &block, true) && block.incoming == 1);
    JSValue stack[3] = {eval(ctx, "({value:1})"),JS_UNDEFINED,JS_UNDEFINED};
    TFReferenceResult result;
    ctx->interrupt_counter = 19;
    CHECK(__js_rc(JS_VALUE_GET_PTR(stack[0]))->ref_count == 1);
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack+1,NULL,NULL,NULL,&result)
          == TF_REFERENCE_DESTRUCTOR);
    JSValue root = JS_DupValue(ctx, stack[0]);
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack+1,NULL,NULL,NULL,&result)
          == TF_REFERENCE_ACCEPT);
    CHECK(result.depth == 1 && JS_VALUE_GET_INT(result.values[0]) == 1);
    CHECK(__js_rc(JS_VALUE_GET_PTR(root))->ref_count == 2);
    JS_FreeValue(ctx, root); JS_FreeValue(ctx, stack[0]); JS_FreeValue(ctx, function);
    /* Refuse overflow, TDZ and bounded prototype exhaustion without changing
       the heap or acquiring even a temporary reference. */
    function = eval(ctx, sources[0]);
    body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    CHECK(tf_value_scan(body,NULL,0,&block,true));
    JSValue args[1] = {eval(ctx,"({})")};
    JSRefCountHeader *header = __js_rc(JS_VALUE_GET_PTR(args[0]));
    int saved = header->ref_count; header->ref_count = INT32_MAX;
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack,NULL,args,NULL,&result)
          == TF_REFERENCE_OVERFLOW);
    CHECK(header->ref_count == INT32_MAX); header->ref_count = saved;
    JS_FreeValue(ctx,args[0]); args[0] = JS_UNINITIALIZED;
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack,NULL,args,NULL,&result)
          == TF_REFERENCE_VALUE);
    JS_FreeValue(ctx,function);
    function = eval(ctx,sources[2]);
    body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    CHECK(tf_value_scan(body,NULL,0,&block,true));
    args[0] = eval(ctx,"(()=>{let p={value:1};for(let i=0;i<9;i++)p=Object.create(p);return p})()");
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack,NULL,args,NULL,&result)
          == TF_REFERENCE_FIELD);
    JS_FreeValue(ctx,args[0]); JS_FreeValue(ctx,function);
    /* The prefix leaves the two owned arguments intact and stops BEFORE the
       call. It must not claim to execute either that call or the return. */
    function = eval(ctx,"(function(a,f){return f(a.value)})");
    body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
    CHECK(tf_reference_scan(body,NULL,0,&block));
    CHECK(!block.conditional && block.count == 3 && block.incoming == 0 && block.peak == 2);
    CHECK(body->byte_code_buf[block.end] == OP_tail_call || body->byte_code_buf[block.end] == OP_call1);
    JSValue pair[2] = {eval(ctx,"({value:{answer:1}})"),eval(ctx,"(v=>v)")};
    CHECK(tf_reference_guards(ctx,body,&block,stack,stack+3,stack,NULL,pair,NULL,&result)
          == TF_REFERENCE_ACCEPT);
    CHECK(result.depth == 2 && JS_VALUE_GET_PTR(result.values[0]) == JS_VALUE_GET_PTR(pair[1]));
    CHECK(JS_IsObject(result.values[1]) && result.fields == 1 && result.next == block.end);
    CHECK(result.ref_count == 3);
    CHECK(result.acquires == 3 && result.releases == 1);
    CHECK(tf_reference_frontier(body,&block) == TF_REFERENCE_CALL);
    for (unsigned i = 0; i < result.ref_count; i++)
        CHECK(result.refs[i].header->ref_count == result.refs[i].initial);
    JS_FreeValue(ctx,pair[0]); JS_FreeValue(ctx,pair[1]); JS_FreeValue(ctx,function);
    ctx->interrupt_counter = counter;
    chained_emission = selected;
    printf("region read-only reference/property oracle=pass cases=%u accepted=%u\n",cases+6,accepted);
}

/* Independent owned-stack execution of the original operations, using engine
   property/equality/truth semantics, not the speculative guard's result. */
static void reference_original_prefix(TFRegionFrame *f, const TFPrimitiveBlock *b)
{
    for (unsigned i = 0; i < b->count; i++) {
        unsigned op = b->opcodes[i], index = b->arguments[i];
        TFRegionHelper helper = b->helpers[i];
        unsigned next = b->offsets[i] + tf_region_opcodes[op].size;
        f->pc = f->body->byte_code_buf + b->offsets[i];
        if (tf_primitive_source_valid(f->body,helper,index) || helper == tf_region_immediate ||
            helper == tf_region_dup) CHECK(helper(f,index,next));
        else if (helper == tf_region_drop) { JS_FreeValue(f->ctx,*--f->sp); tf_region_done(f,next); }
        else if (helper == tf_region_field) {
            JSValue receiver = f->sp[-1];
            JSValue value = JS_GetProperty(f->ctx,receiver,index);
            CHECK(!JS_IsException(value));
            JS_FreeValue(f->ctx,receiver); f->sp[-1] = value; tf_region_done(f,next);
        } else if (helper == tf_region_branch) CHECK(tf_region_branch(f,index,next));
        else if (helper == tf_region_less || op == OP_strict_eq || op == OP_strict_neq) {
            JSValue left = f->sp[-2], right = f->sp[-1];
            bool value = helper == tf_region_less ? JS_VALUE_GET_INT(left) < JS_VALUE_GET_INT(right) :
                JS_StrictEq(f->ctx,left,right) ^ (op == OP_strict_neq);
            JS_FreeValue(f->ctx,left); JS_FreeValue(f->ctx,right);
            f->sp--; f->sp[-1] = JS_NewBool(f->ctx,value); tf_region_done(f,next);
        } else {
            JSValue value = f->sp[-1];
            bool result = op == OP_lnot ? !JS_ToBool(f->ctx,value) : op == OP_is_null ? JS_IsNull(value) :
                op == OP_is_undefined ? JS_IsUndefined(value) : JS_IsNull(value) || JS_IsUndefined(value);
            JS_FreeValue(f->ctx,value); f->sp[-1] = JS_NewBool(f->ctx,result); tf_region_done(f,next);
        }
    }
}

static void reference_commit_tests(JSContext *ctx)
{
    bool selected = chained_emission, probes = tf_reference_probes;
    chained_emission = tf_reference_probes = true;
    int counter = ctx->interrupt_counter;
    static const char *sources[] = {
        "(function(a){if(a.value)return 1;return 0})",
        "(function(a){if(a.value===a.value)return 1;return 0})",
        "(function(a){if(a??false)return 1;return 0})",
        "(function(a,f){return f(a.value)})"
    };
    static const char *inputs[] = {"({value:1})","({value:null})","({value:'x'})",
        "({value:{}})","Object.create({value:3})","({value:NaN})",
        "({get value(){throw Error('getter must not run')}})",
        "new Proxy({value:3},{get(){throw Error('proxy must not run')}})",
        "null","undefined","''","'\\u{1f600}'","Symbol('x')","1n<<80n",
        "(()=>{let a={};a.value=a;return a})()"};
    unsigned cases = 0, accepted = 0;
    for (unsigned source = 0; source < sizeof(sources)/sizeof(sources[0]); source++) {
        JSValue function = eval(ctx,sources[source]);
        JSFunctionBytecode *body = JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
        TFPrimitiveBlock block;
        CHECK(tf_reference_scan(body,NULL,0,&block));
        TFRegionPlan plan = {.body=body,.reference_blocks=&block,.reference_count=1};
        for (unsigned input = 0; input < sizeof(inputs)/sizeof(inputs[0]); input++) {
            JSValue args[2] = {eval(ctx,inputs[input]),eval(ctx,"(v=>v)")};
            for (unsigned capacity = 0; capacity <= 4; capacity++) {
                for (unsigned mode = 0; mode < 3; mode++) {
                    JSValue stack[4] = {JS_NewInt32(ctx,37),JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                    JSValue snapshot[4]; memcpy(snapshot,stack,sizeof(stack));
                    TFRegionFrame f = {.body=body,.args=args,.base=stack,.limit=stack+capacity,
                        .sp=stack+1,.pc=body->byte_code_buf,.ctx=ctx,.plan=&plan,
                        .completed=mode == 2 ? TF_REGION_QUANTUM-block.count+1 : 0};
                    TFRegionFrame before = f;
                    ctx->interrupt_counter = mode == 1 ? 1 : 19;
                    TFReferenceResult prediction;
                    unsigned status = tf_reference_guards(ctx,body,&block,stack,stack+capacity,stack+1,
                                                           NULL,args,NULL,&prediction);
                    JSMallocState *heap = &ctx->rt->malloc_ctx.malloc_state;
                    size_t bytes = heap->malloc_size, allocations = heap->malloc_count;
                    bool admit = status == TF_REFERENCE_ACCEPT && mode != 2;
                    int result = tf_reference_execute(&f,0,0);
                    CHECK(heap->malloc_size == bytes && heap->malloc_count == allocations);
                    CHECK((result != 0) == admit);
                    if (!admit) {
                        CHECK(!memcmp(&before,&f,sizeof(f)) && !memcmp(snapshot,stack,sizeof(stack)));
                        CHECK(ctx->interrupt_counter == (mode == 1 ? 1 : 19));
                        for (unsigned i = 0; i < prediction.ref_count; i++)
                            CHECK(prediction.refs[i].header->ref_count == prediction.refs[i].initial);
                    } else {
                        CHECK(f.completed == block.count && f.reference_block_hits == 1);
                        CHECK(f.pc == body->byte_code_buf+prediction.next);
                        CHECK(f.sp == stack+1+prediction.depth-block.conditional);
                        CHECK(ctx->interrupt_counter == 19-block.conditional);
                        for (unsigned i = 0; i < prediction.ref_count; i++)
                            CHECK(prediction.refs[i].header->ref_count == prediction.refs[i].initial+prediction.refs[i].delta);
                        JSValue values[4]; unsigned count = f.sp-stack;
                        memcpy(values,stack,count*sizeof(JSValue));
                        while (f.sp > stack+1) JS_FreeValue(ctx,*--f.sp);
                        JSValue expected[4] = {JS_NewInt32(ctx,37),JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                        TFRegionFrame original = {.body=body,.args=args,.base=expected,.limit=expected+4,
                            .sp=expected+1,.pc=body->byte_code_buf,.ctx=ctx};
                        ctx->interrupt_counter = 19;
                        reference_original_prefix(&original,&block);
                        CHECK(original.completed == block.count && original.pc == f.pc);
                        CHECK(original.sp-expected == count && ctx->interrupt_counter == 19-block.conditional);
                        for (unsigned i = 0; i < count; i++) CHECK(tf_reference_same(values[i],expected[i]));
                        while (original.sp > expected+1) JS_FreeValue(ctx,*--original.sp);
                        accepted++;
                    }
                    cases++;
                }
            }
            JS_FreeValue(ctx,args[0]); JS_FreeValue(ctx,args[1]);
        }
        JS_FreeValue(ctx,function);
    }
    ctx->interrupt_counter = counter;
    chained_emission = selected; tf_reference_probes = probes;
    printf("region reference batch atomic ownership, original-prefix differential and refusal=pass cases=%u accepted=%u\n",cases,accepted);
}
#endif
static JSValue collect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_UNDEFINED;
}
static JSValue retire_current_plan(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    TFRegionPlan *plan = tf_region_plan;
    if (plan && !plan->retired) {
        if (PROBE_FRAME_LEASE) CHECK(plan->active_frames > 0);
        plan->enabled = false;
        plan->retired = true;
        CHECK(native_code_pool_retire(plan->pool, plan->image));
    }
    return JS_UNDEFINED;
}

typedef struct {
    const char *name, *source, *expected;
    bool requires_lease_refusal;
    unsigned expected_binding_kind, expected_binding_index;
    bool invoke_result;
} Case;
static const Case cases[] = {
    {"CFG branch method receiver", "(()=>{let o={a:true,b:false,x:19,y:23,f(v){gc();return this.y+v}};return function(){if(o.a)return o.f(o.x);if(o.b)return o.x;return o.y}})()", "42"},
    {"CFG shared result ownership", "(()=>{let o={a:false,b:true,c:true,x:{v:17},y:{v:23},z:{v:29}};return function(){if(o.a){if(o.b)return ()=>o.x.v;return ()=>o.y.v}if(o.c)return ()=>o.z.v;return ()=>o.x.v}})()", "29", false, 0, 0, true},
    {"CFG getter precise fallback", "(()=>{let n=0,o={a:false,b:true,get c(){n++;gc();return true},x:19,y:23,z:29};return function(){let r;if(o.a){if(o.b)r=o.x;else r=o.y}else if(o.c)r=o.z;else r=o.x;return r+':'+n}})()", "29:1"},
    {"CFG shape changes after owned call", "(()=>{let o={a:true,b:true,c:false,x:19,y:23,z:29};return function(){let r;if(o.a){if(o.b)r=o.x;else r=o.y}else if(o.c)r=o.z;else r=o.x;delete o.a;Object.defineProperty(o,'a',{get(){gc();return false}});if(o.a){if(o.b)return r+':'+o.x;return r+':'+o.y}if(o.c)return r+':'+o.z;return r+':'+o.x}})()", "19:19"},
    {"own value and locals", "(()=>{let o={x:19};return function(){let a=o;let b=a.x;return b+2}})()", "21"},
    {"inherited fallback", "(()=>{let o=Object.create({x:19});return function(){let a=o;let b=a.x;return b+2}})()", "21"},
    {"getter once", "(()=>{let n=0,o={get x(){n++;return 19}};return function(){let a=o;let b=a.x;return [b,n].join(':')}})()", "19:1"},
    {"proxy once", "(()=>{let n=0,o=new Proxy({x:19},{get(t,k){n++;return t[k]}});return function(){let a=o;let b=a.x;return [b,n].join(':')}})()", "19:1"},
    {"throw after committed read", "(()=>{let n=0,o={x:19,get y(){n++;throw Error('sentinel')}};return function(){let a=o;let b=a.x;try{return a.y}catch(e){return b+':'+n+':'+e.message}}})()", "19:1:sentinel"},
    {"shape mutation", "(()=>{let o={x:19};return function(){let a=o;let b=a.x;delete a.x;Object.defineProperty(a,'x',{get(){return 23}});return b+':'+a.x}})()", "19:23"},
    {"GC and reentry", "(()=>{let o={x:19,get y(){gc();return this.x+1}};return function(){let a=o;let b=a.x;return b+':'+a.y}})()", "19:20"},
    {"last reference drop", "(function(){let o={x:{q:7}};let a=o.x;o=null;a=null;return 23})", "23"},
    {"last reference overwrite", "(function(){let a={q:7};a=9;return a+1})", "10"},
    {"TDZ exception", "(function(){let a={x:1};let b=a.x;try{b+=value;let value=7}catch(e){return b+':'+e.name}})", "1:ReferenceError"},
    {"closure TDZ exception", "(()=>{return function(){let a={x:1};let b=a.x;try{return b+value}catch(e){return b+':'+e.name}};let value=7})()", "1:ReferenceError"},
    {"branch target inside region", "(function(){let n=0,o={x:3};for(let i=0;i<6;i++){let a=o;let b=a.x;if(i===3)continue;n+=b}return n})", "15"},
    {"method receiver", "(()=>{let o={x:19,f(){return this.x}};return function(){let a=o;let b=a.x;return b+':'+a.f()}})()", "19:19"},
    {"object identity", "(()=>{let child={},o={x:child};return function(){let a=o;let b=a.x;return b===child}})()", "true"},
    {"undefined null booleans", "(function(){let a={x:null};let b=a.x;return [b===null,undefined,false,true,-3,300].join(':')})", "true::false:true:-3:300"},
    {"constant representations", "(function(){let a=1.25;let b=-0;let c='\\uD834\\uDD1E';let d=72057594037927937n;return [a,Object.is(b,-0),c,d.toString()].join(':')})", "1.25:true:𝄞:72057594037927937"},
    {"recursive same body", "(function f(n){let a={x:n||0};let b=a.x;if(b<4)return f(b+1)+b;return b})", "10"},
    {"throwing call identity and frame", "(()=>{let mark={},n=0;function fail(x){n++;throw mark}return function nativeCaller(){let a={x:19};let b=a.x;try{let c=fail(b);return c}catch(e){return [e===mark,b,n].join(':')}}})()", "true:19:1"},
    {"call exception backtrace", "(()=>{function fail(){throw Error('sentinel')}return function nativeCaller(){try{let a=fail();return a}catch(e){return e.stack.includes('nativeCaller')&&e.message==='sentinel'}}})()", "true"},
    {"noncallable call", "(function(){let f=19;try{let a=f();return a}catch(e){return e.name}})", "TypeError"},
    {"call argument mutation and closure", "(()=>{function f(a,b,c,d,e){a=b;gc();return ()=>[a.x,c,d,e].join(':')}return function(){let a={x:1},b={x:7};let out=f(a,b,3,4,5);gc();return out()+':'+a.x}})()", "7:3:4:5:1"},
    {"callback reentry and lease exhaustion", "(function f(n){if(!n)n=1;let next=n+1;if(n<10){let out=f(next);return out+n}return n})", "55", true},
    {"branch truth values", "(function(){let values=[0,-0,NaN,'',null,undefined,false,1,-2,{},[],true];let n=0;for(let i=0;i<values.length;i++){if(values[i])n++}return n})", "5"},
    {"integer increment overflow", "(function(){let n=2147483647;n++;return n})", "2147483648"},
    {"increment coercion once", "(function(){let calls=0,n={valueOf(){calls++;return 3}};n++;return n+':'+calls})", "4:1"},
    {"comparison coercion once", "(function(){let calls=0,n={valueOf(){calls++;return 3}};if(n<4)return calls;return 99})", "1"},
    {"comparison block signed endpoints", "(function(){let a=-2147483648,b=2147483647;if(a<b)return 'signed';return 'wrong'})", "signed"},
    {"comparison block borrowed closure", "(()=>{let a=-1,b=0;return function(){if(a<b)return 7;return 9}})()", "7"},
    {"comparison block throwing coercion", "(()=>{let mark={},calls=0,n={valueOf(){calls++;gc();throw mark}};return function(){let bound=4;try{if(n<bound)return 'wrong'}catch(e){return (e===mark)+':'+calls}}})()", "true:1"},
    {"last property receiver reference", "(function(){let a=1;let n=({x:19}).x;return n+a})", "20"},
    {"self-referential property value", "(()=>{let o={};o.self=o;return function(){let a=o;let b=a.self;return b===o}})()", "true"},
    {"signed comparisons", "(function(){let a=-2147483648,b=2147483647;return [a<b,b<a,a<a].join(':')})", "true:false:false"},
    {"retirement during callback", "(function(){let n=1;retirePlan();for(let i=0;i<10;i++)n++;gc();return n})", "11"},
    {"predicate immediate semantics", "(function(){let a=NaN,b=-0,c=0;return [a===a,a!==a,b===c,undefined===null,!a,!b,!undefined,!null].join(':')})", "false:true:true:false:true:true:true:true"},
    {"predicate coercion is absent", "(function(){let n=0,o={valueOf(){n++;return 1}},p=new Proxy(o,{get(){n++;throw 1}});return [!p,p===o,p===p,p!==p,n].join(':')})", "false:false:true:false:0"},
    {"predicate strings and bigints fall back", "(function(){let s='ab'.repeat(5000),t=('a'+'b').repeat(5000),b=1n<<80n;return [s===t,s!==t,b===b,b===0n,!s,!b,!0n].join(':')})", "true:false:true:false:false:false:true"},
    {"predicate shared last references", "(function(){let o={};let equal=((o=undefined)||{});return (equal===equal)+':'+(!{})})", "true:false"},
    {"predicate nullish semantics", "(function(){let a=null,b=undefined,c=0;return [a===null,a===undefined,b===null,b===undefined,c===null,c===undefined,a==null,b==null,c==null].join(':')})", "true:false:false:true:false:false:true:true:false"},
    {"nullish continuation semantics", "(function(){let a=null,b=undefined,c=0;return [a??8,b??8,c??8,a?.x,c?.x].join(':')})", "8:8:0::"},
    {"reinitialized local owns object", "(function(){let total=0;for(let i=0;i<20;i++){let o={n:i};total+=o.n;gc()}return total})", "190"},
    {"binding reload after getter and GC", "(function(){let n=0,sum=0;let o={get x(){gc();let old=n;n=old+1;return old}};for(;n<4;n++){sum+=o.x}return n+':'+sum})", "4:2", false, 1, 0},
    {"binding publication before throwing getter", "(function(){let n=0;let o={get x(){gc();throw n}};try{for(;n<4;n++){if(n<2)continue;let value=o.x}}catch(e){return e+':'+n}return 'miss'})", "2:2", false, 1, 0},
    {"argument reload after callback", "(function(a){a=3;let o={get x(){a=7;gc();return 1}};let first=a;let x=o.x;return [first,a,a,a,a,x].join(':')})", "3:7:7:7:7:1", false, 2, 0},
    {"field fusion shape transitions", "(function(){let o={x:3},sum=0;for(let i=0;i<5;i++){if(i===1)Object.defineProperty(o,'x',{get(){gc();return 5},configurable:true});if(i===3){delete o.x;Object.setPrototypeOf(o,{x:7})}sum+=o.x}return sum})", "27"},
    {"field getter replaces borrowed source", "(function(){let o,n=0;o={y:5,get x(){n++;o={x:9};gc();return this.y}};let first=o.x;return first+':'+o.x+':'+n})", "5:9:1"},
    {"return closes captured local", "(function(){let o={x:17};gc();return ()=>o.x})", "17", false, 0, 0, true},
    {"finally overrides return", "(function(){let o={x:17};try{return o}finally{gc();return 23}})", "23"},
    {"bare undefined return", "(function(){let o={x:17};gc();return})", "undefined"},
};

typedef struct { uint64_t quotas, exceptions; } CaseEvents;
static bool describe_instructions;
static CaseEvents run_case(const Case *test, unsigned native)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator != NULL);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt != NULL);
    /* ASan enlarges the interpreter's C frame; keep the deliberate 10-deep
       lease-exhaustion fixture below a fixed test-only native-stack ceiling. */
#ifdef __PSP__
    JS_SetMaxStackSize(rt, 96u * 1024u); /* below the real 128 KiB owner stack */
#else
    JS_SetMaxStackSize(rt, 4u * 1024u * 1024u);
#endif
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    ProbeHeap heap = {rt, 8u * 1024u * 1024u, 0, false};
    JS_SetMemoryLimit(rt, heap.ceiling);
    JSValue global = JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx, global, "gc", JS_NewCFunction(ctx, collect, "gc", 0)) >= 0);
    CHECK(JS_SetPropertyStr(ctx, global, "retirePlan", JS_NewCFunction(ctx, retire_current_plan, "retirePlan", 0)) >= 0);
    JS_FreeValue(ctx, global);
    JSValue function = eval(ctx, test->source);
    size_t page_size = code_page_size();
    CHECK(page_size > 0);
    NativeCodePool *pool = native_code_pool_create(&budget,
        (NativeCodeHeap){&heap, reserve, release},
        (NativeCodeBackend){NULL, (size_t)page_size, map_code, publish_code, unmap_code}, NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    heap.refuse = native == 2; /* actual admission refusal, not simulated result */
    TFRegionPlan *plan = compile_plan(ctx, &budget, &heap, pool, function);
    heap.refuse = false;
    CHECK((plan != NULL) == (native != 2));
#ifdef __PSP__
    if (cached_bindings && plan && test->expected_binding_kind) {
        CHECK(plan->binding_kind == test->expected_binding_kind);
        CHECK(plan->binding_index == test->expected_binding_index);
    }
#endif
    if (describe_instructions && plan && native == 1) {
        for (unsigned at = 0; at < (unsigned)plan->body->byte_code_len;) {
            unsigned op = plan->body->byte_code_buf[at];
            uint32_t unused;
            printf("region instruction at=%u opcode=%s admitted=%u\n", at,
                tf_region_opcodes[op].name,
                tf_region_decode(plan->body->byte_code_buf + at, &unused) != NULL);
            at += tf_region_opcodes[op].size;
        }
    }
    if (native) tf_region_plan = plan;
    if (native == 1) {
        JS_FreeValue(ctx, function);
        function = JS_UNDEFINED;
        JS_RunGC(rt); /* the plan is now the only retained function root */
    }
    JSValue result = JS_Call(ctx, native == 1 ? plan->root : function, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "%s mode=%u exception=%s\n", test->name, native, message ? message : "unknown");
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    CHECK(!JS_IsException(result));
    if (test->invoke_result) {
        JS_RunGC(rt);
        JSValue called = JS_Call(ctx, result, JS_UNDEFINED, 0, NULL);
        JS_FreeValue(ctx, result);
        result = called;
        CHECK(!JS_IsException(result));
    }
    const char *actual = JS_ToCString(ctx, result);
    if (!actual || strcmp(actual, test->expected))
        printf("%s mode=%u actual=%s expected=%s\n", test->name, native, actual ? actual : "null", test->expected);
    CHECK(actual && !strcmp(actual, test->expected));
    if (native == 1) CHECK(plan->calls > 0 && plan->completed > 0);
    else CHECK(!plan || (plan->calls == 0 && plan->completed == 0));
    if (native == 1 && chained_emission && test->requires_lease_refusal)
        CHECK(plan->lease_refusals > 0);
    CaseEvents events = {plan ? plan->quota_exits : 0, plan ? plan->exceptions : 0};
    printf("region case=%s mode=%u regions=%u native-bytes=%u admitted-ops=%u calls=%llu completed=%llu guard-exits=%llu quotas=%llu exceptions=%llu\n",
        test->name, native, plan ? plan->entries : 0, plan ? plan->code_bytes : 0, plan ? plan->instructions : 0,
        (unsigned long long)(plan ? plan->calls : 0), (unsigned long long)(plan ? plan->completed : 0),
        (unsigned long long)(plan ? plan->guards : 0),
        (unsigned long long)events.quotas, (unsigned long long)events.exceptions);
    tf_region_plan = NULL;
    JS_FreeCString(ctx, actual);
    JS_FreeValue(ctx, result);
    destroy_plan(ctx, &budget, &heap, plan);
    CHECK(native_code_pool_destroy(&pool));
    CHECK(heap.reserved == 0);
    JS_FreeValue(ctx, function);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator));
    CHECK(budget.current == 0);
    return events;
}

static uint64_t now_ns(void)
{
#ifdef __PSP__
    return (uint64_t)sceKernelGetSystemTimeWide() * 1000u;
#else
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
#endif
}
static uint64_t median(uint64_t *samples, unsigned count)
{
    for (unsigned i = 1; i < count; i++) {
        uint64_t sample = samples[i];
        unsigned j = i;
        while (j && sample < samples[j - 1]) { samples[j] = samples[j - 1]; j--; }
        samples[j] = sample;
    }
    return samples[count / 2];
}

static int interrupt_probe(JSRuntime *rt, void *opaque)
{
    (void)rt;
    unsigned *count = opaque;
    return ++*count >= 3;
}

#define TRANSCRIPT_LIMIT 48u
typedef struct {
    JSContext *ctx;
    JSValue global;
    unsigned count, interval;
    int32_t positions[TRANSCRIPT_LIMIT];
} InterruptTranscript;
static int record_interrupt(JSRuntime *rt, void *opaque)
{
    (void)rt;
    InterruptTranscript *record = opaque;
    CHECK(record->count < TRANSCRIPT_LIMIT);
    JSValue value = JS_GetPropertyStr(record->ctx, record->global, "pollMarker");
    CHECK(JS_ToInt32(record->ctx, &record->positions[record->count++], value) == 0);
    JS_FreeValue(record->ctx, value);
    record->ctx->interrupt_counter = record->interval;
    return record->count == TRANSCRIPT_LIMIT;
}
static void branch_transcripts(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    JSValue function = eval(ctx, "(function(condition){for(let i=0;i<10000;i++){globalThis.pollMarker=i;if(condition)globalThis.pollMarker=i+100;if(i<7)globalThis.pollMarker=i+200;else globalThis.pollMarker=i+300}return 0})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan != NULL);
#ifdef __PSP__
    if (basic_blocks) CHECK(plan->basic_blocks > 0);
#endif
    static const char *conditions[] = {
        "true", "false", "-19", "0", "null", "undefined", "NaN", "-0",
        "1.25", "({})", "'text'", "''", "0n", "1n", "Symbol('truth')"
    };
    static const unsigned intervals[] = {1, 2, 17};
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    for (unsigned c = 0; c < sizeof(conditions) / sizeof(conditions[0]); c++) {
        JSValue condition = eval(ctx, conditions[c]);
        for (unsigned i = 0; i < sizeof(intervals) / sizeof(intervals[0]); i++) {
            InterruptTranscript records[2] = {0};
            for (unsigned lane = 0; lane < PROBE_LANES; lane++) {
                CHECK(JS_SetPropertyStr(ctx, global, "pollMarker", JS_NewInt32(ctx, -1)) >= 0);
                records[lane].ctx = ctx;
                records[lane].global = global; /* borrowed while this function roots it */
                records[lane].interval = intervals[i];
                tf_region_plan = lane ? plan : NULL;
                JS_SetInterruptHandler(rt, record_interrupt, &records[lane]);
                ctx->interrupt_counter = 1;
                JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 1, &condition);
                CHECK(JS_IsException(result) && records[lane].count == TRANSCRIPT_LIMIT);
                JSValue error = JS_GetException(ctx);
                CHECK(!JS_IsNull(error));
                JS_FreeValue(ctx, error);
                JS_SetInterruptHandler(rt, NULL, NULL);
                JS_SetUncatchableException(ctx, false);
            }
            if (PROBE_LANES == 2) CHECK(!memcmp(records[0].positions, records[1].positions, sizeof(records[0].positions)));
        }
        JS_FreeValue(ctx, condition);
    }
    tf_region_plan = NULL;
    JS_FreeValue(ctx, global);
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    printf("region branch-interrupt-transcripts=pass conditions=%zu intervals=%zu checkpoints=%u\n",
        sizeof(conditions) / sizeof(conditions[0]), sizeof(intervals) / sizeof(intervals[0]), TRANSCRIPT_LIMIT);
}

static void guard_tests(JSContext *ctx)
{
    uint8_t code[2] = {OP_nop, OP_return};
    JSFunctionBytecode body = {.var_count = 1, .byte_code_buf = code};
    JSValue locals[1] = {JS_NewObject(ctx)};
    JSValue stack[2] = {JS_NewObject(ctx), JS_UNDEFINED};
    CHECK(!JS_IsException(locals[0]) && !JS_IsException(stack[0]));
    JSAtom atom = JS_NewAtom(ctx, "value");
    CHECK(atom != JS_ATOM_NULL);
    CHECK(JS_SetProperty(ctx, stack[0], atom, JS_NewInt32(ctx, 19)) >= 0);
    TFRegionFrame frame = {&body, locals, NULL, stack, stack + 2, stack + 1, NULL, code, 0};
    CHECK(!tf_region_drop(&frame, 0, 1));
    CHECK(!tf_region_field(&frame, atom, 1));
    CHECK(!tf_region_put_local(&frame, 0, 1));
    CHECK(!tf_region_set_local(&frame, 0, 1));
    CHECK(frame.pc == code && frame.sp == stack + 1 && frame.completed == 0);
    CHECK(__js_rc(JS_VALUE_GET_PTR(stack[0]))->ref_count == 1);
    CHECK(__js_rc(JS_VALUE_GET_PTR(locals[0]))->ref_count == 1);
    JSValue owner = JS_DupValue(ctx, stack[0]);
    CHECK(tf_region_field(&frame, atom, 1));
    CHECK(JS_VALUE_GET_TAG(stack[0]) == JS_TAG_INT && JS_VALUE_GET_INT(stack[0]) == 19);
    CHECK(__js_rc(JS_VALUE_GET_PTR(owner))->ref_count == 1);
    JS_FreeValue(ctx, owner);
    owner = JS_DupValue(ctx, locals[0]);
    CHECK(tf_region_put_local(&frame, 0, 1));
    CHECK(frame.sp == stack && frame.completed == 2);
    CHECK(__js_rc(JS_VALUE_GET_PTR(owner))->ref_count == 1);
    CHECK(JS_VALUE_GET_TAG(locals[0]) == JS_TAG_INT && JS_VALUE_GET_INT(locals[0]) == 19);
    JS_FreeValue(ctx, owner);
    JS_FreeValue(ctx, locals[0]);
    JS_FreeAtom(ctx, atom);
    puts("region last-reference guards preserve PC, stack and ownership=pass");
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    {
        uint8_t code[] = {OP_set_loc_uninitialized, 0, 0, OP_return};
        JSFunctionBytecode body = {.var_count = 1, .byte_code_buf = code};
        JSValue local = JS_NewObject(ctx);
        JSValue stack[2];
        stack[0] = JS_NewObject(ctx);
        stack[1] = JS_DupValue(ctx, stack[0]);
        TFRegionFrame f = {.body = &body, .locals = &local, .base = stack,
            .limit = stack + 2, .sp = stack + 2, .pc = code, .ctx = ctx};
        CHECK(!tf_region_semantic(&f, OP_strict_eq, 3));
        CHECK(!tf_region_semantic(&f, OP_set_loc_uninitialized, 3));
        CHECK(f.pc == code && f.sp == stack + 2 && f.completed == 0);
        CHECK(__js_rc(JS_VALUE_GET_PTR(stack[0]))->ref_count == 2);
        CHECK(__js_rc(JS_VALUE_GET_PTR(local))->ref_count == 1);
        JSValue owner = JS_DupValue(ctx, stack[0]);
        CHECK(tf_region_semantic(&f, OP_strict_eq, 3));
        CHECK(f.sp == stack + 1 && JS_VALUE_GET_TAG(stack[0]) == JS_TAG_BOOL && JS_VALUE_GET_INT(stack[0]) == 1);
        CHECK(__js_rc(JS_VALUE_GET_PTR(owner))->ref_count == 1);
        JS_FreeValue(ctx, owner);
        owner = JS_DupValue(ctx, local);
        f.pc = code;
        CHECK(tf_region_semantic(&f, OP_set_loc_uninitialized, 3));
        CHECK(JS_IsUninitialized(local) && __js_rc(JS_VALUE_GET_PTR(owner))->ref_count == 1);
        JS_FreeValue(ctx, owner);
        f.pc = code;
        f.completed = TF_REGION_QUANTUM;
        CHECK(!tf_region_semantic(&f, OP_lnot, 3));
        CHECK(f.quota_exit && f.pc == code && JS_VALUE_GET_INT(stack[0]) == 1);
        f.completed = 0;
        f.sp = stack;
        CHECK(!tf_region_semantic(&f, OP_lnot, 3));
        f.sp = stack + 1;
        CHECK(!tf_region_semantic(&f, OP_strict_eq, 3));
        code[1] = 1; /* invalid local index refuses before touching storage */
        CHECK(!tf_region_semantic(&f, OP_set_loc_uninitialized, 3));
        puts("region semantic-continuation alias, quota and bounds guards=pass");
    }
#endif
}

#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
/* Compare the borrowed executor with the existing owned helpers instruction
   by instruction. The latter acquire/release every intermediate value and do
   not use the CFG's predicted edge or stack map. */
static void cfg_original_prefix(TFRegionFrame *f, unsigned instructions)
{
    while (instructions--) {
        unsigned at=f->pc-f->body->byte_code_buf, op=*f->pc;
        unsigned next=at+tf_region_opcodes[op].size;
        uint32_t argument;
        TFRegionHelper helper=tf_region_decode(f->pc,&argument);
        if (op == OP_get_field2) {
            CHECK(f->sp > f->base && f->sp < f->limit);
            JSValue field; bool inherited,own;
            CHECK(!tf_reference_field(f->sp[-1],get_u32(f->pc+1),&field,&inherited,&own) && own);
            *f->sp++=JS_DupValue(f->ctx,field);
            f->completed++; f->pc=f->body->byte_code_buf+next;
        } else {
            CHECK(helper && helper(f,argument,next));
        }
    }
}
typedef struct { JSRefCountHeader *headers[64]; int counts[64]; unsigned count; } CFGReferences;
static void cfg_collect_references(CFGReferences *r, JSValue value)
{
    if (!JS_VALUE_HAS_REF_COUNT(value)) return;
    JSRefCountHeader *header=__js_rc(JS_VALUE_GET_PTR(value));
    for(unsigned i=0;i<r->count;i++) if(r->headers[i] == header) return;
    CHECK(r->count < 64);
    r->headers[r->count]=header; r->counts[r->count++]=header->ref_count;
    if (!JS_IsObject(value)) return;
    JSObject *object=JS_VALUE_GET_OBJ(value);
    if (object->is_exotic) return;
    CHECK(object->shape->prop_count <= 64);
    for(unsigned i=0;i<object->shape->prop_count;i++)
        if (get_shape_prop(object->shape)[i].atom && !(get_shape_prop(object->shape)[i].flags & JS_PROP_TMASK))
            cfg_collect_references(r,object->prop[i].u.value);
}
static void cfg_check_references(const CFGReferences *r)
{
    for(unsigned i=0;i<r->count;i++) CHECK(r->headers[i]->ref_count == r->counts[i]);
}
static void cfg_guard_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
#ifdef __PSP__
    if (!register_cfg) return;
    bool selected_owned=cfg_owned_stores;
    cfg_owned_stores=false; /* this oracle deliberately ends before writes */
#else
    (void)heap; (void)pool;
#endif
    TFBorrowedCFG *g=budget_calloc_category(budget,BUDGET_CATEGORY_JAVASCRIPT,1,sizeof(*g));
    CHECK(g);
    static const char *sources[] = {
        "(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})",
        "(function(o){return o.a&&o.b ? o.x : o.y})",
        "(function(o){if(o.a)return o.f(o.x);if(o.b)return o.x;return o.z})",
        "(function(o){while(o){if(o.a)continue;if(o.b)break;return o.x}return o.z})",
        "(function(o){if(o.a??o.b)return o.x;if(!o.c)return o.y;return o.z})"
    };
    static const char *inputs[] = {
        "({a:true,b:true,c:true,x:19,y:23,z:29,f(v){return this.y+v}})",
        "({a:false,b:true,c:true,x:{},y:'wide \\u{1f600}',z:Symbol('z'),f(v){return v}})",
        "({a:false,b:false,c:false,x:null,y:1n,z:1n<<80n})",
        "({a:3.5,b:true,c:true,x:'x'})", "({a:NaN,b:false,c:true})",
        "({a:'',b:'a'.repeat(8192)+'b',c:0,x:{},y:{},z:{}})",
        "({a:1n,b:true,c:false,x:{},y:{}})", "({a:1n<<80n,b:false})",
        "Object.create({a:true,b:true,x:19})",
        "({get a(){throw Error('getter must not run')},b:true})",
        "new Proxy({a:true},{get(){throw Error('proxy must not run')}})",
        "null", "undefined", "Symbol('x')", "JS_UNINITIALIZED",
        "(()=>{let o={a:true,b:false};o.x=o.y=o.z=o;return o})()"
    };
    unsigned tested=0,progress=0,loops=0;
    int saved_counter=ctx->interrupt_counter;
    for (unsigned s=0;s<sizeof(sources)/sizeof(sources[0]);s++) {
        JSValue function=eval(ctx,sources[s]);
        JSFunctionBytecode *body=JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
        CHECK(tf_cfg_scan(body,NULL,0,g));
#ifdef __PSP__
        TFRegionPlan *plan=compile_plan(ctx,budget,heap,pool,function);
        if(!plan || !plan->cfg_regions || !plan->entry[0])
            printf("region cfg admission source=%u plan=%p cfg=%u entry=%u nodes=%u branches=%u supported=%u peak=%u\n",
                s,(void *)plan,plan ? plan->cfg_regions : 0,plan ? plan->entry[0] : 0,
                g->count,g->branches,g->supported,g->peak);
        CHECK(plan && plan->cfg_regions && plan->entry[0]);
#endif
        for (unsigned a=0;a<sizeof(inputs)/sizeof(inputs[0]);a++) {
            JSValue argument=a == 14 ? JS_UNINITIALIZED : eval(ctx,inputs[a]);
            for (unsigned capacity=0;capacity<=3;capacity++) for (unsigned mode=0;mode<6;mode++) {
                JSValue overflowing=argument;
                if(mode == 5 && JS_IsObject(argument) && !JS_VALUE_GET_OBJ(argument)->is_exotic) {
                    JSProperty *property;
                    JSAtom atom=JS_NewAtom(ctx,"x");
                    JSShapeProperty *shape=find_own_property(&property,JS_VALUE_GET_OBJ(argument),atom);
                    if(shape && !(shape->flags & JS_PROP_TMASK)) overflowing=property->u.value;
                    JS_FreeAtom(ctx,atom);
                }
                JSRefCountHeader *overflow=mode>=4 && JS_VALUE_HAS_REF_COUNT(overflowing) ?
                    __js_rc(JS_VALUE_GET_PTR(overflowing)) : NULL;
                int old_count=overflow ? overflow->ref_count : 0;
                if(overflow) overflow->ref_count=INT32_MAX-3;
                CFGReferences refs={0}; cfg_collect_references(&refs,argument);
                JSValue expected[3]={JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                unsigned before=mode == 2 ? TF_REGION_QUANTUM-1 : mode == 3 ? TF_REGION_QUANTUM : 0;
                int initial_counter=mode == 1 ? 1 : INT32_MAX;
                TFRegionFrame f={.body=body,.args=&argument,.base=expected,.sp=expected,
                    .limit=expected+capacity,.pc=body->byte_code_buf,.ctx=ctx,.completed=before};
                JSMallocState *h=&ctx->rt->malloc_ctx.malloc_state;
                size_t bytes=h->malloc_size,allocations=h->malloc_count;
                ctx->interrupt_counter=initial_counter;
                tf_cfg_predict(&f,g);
                CHECK(h->malloc_size == bytes && h->malloc_count == allocations);
                unsigned count=f.sp-expected,done=f.completed-before,pc=f.pc-body->byte_code_buf;
                int final_counter=ctx->interrupt_counter;
                bool quota=f.quota_exit;
                JSValue values[3]; memcpy(values,expected,sizeof(values));
                while(f.sp>expected) JS_FreeValue(ctx,*--f.sp);
                cfg_check_references(&refs);
                JSValue stack[3]={JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                TFRegionFrame original={.body=body,.args=&argument,.base=stack,.sp=stack,
                    .limit=stack+capacity,.pc=body->byte_code_buf,.ctx=ctx,.completed=before};
                ctx->interrupt_counter=initial_counter;
                cfg_original_prefix(&original,done);
                CHECK(original.pc == body->byte_code_buf+pc && original.sp-stack == count);
                CHECK(original.completed == before+done && ctx->interrupt_counter == final_counter);
                for(unsigned i=0;i<count;i++) CHECK(tf_reference_same(stack[i],values[i]));
                while(original.sp>stack) JS_FreeValue(ctx,*--original.sp);
                cfg_check_references(&refs);
#ifdef __PSP__
                original.pc=body->byte_code_buf; original.completed=before;
                original.quota_exit=false; original.plan=plan;
                ctx->interrupt_counter=initial_counter;
                NativeCodeLease lease; CHECK(native_code_pool_enter(pool,plan->image,16,&lease));
                TFRegionEntry entry; memcpy(&entry,&lease.entry,sizeof(entry));
                entry(&original,(const char *)lease.entry+plan->entry[0]-1-16);
                CHECK(native_code_pool_leave(pool,lease));
                if(original.pc != body->byte_code_buf+pc || original.completed != before+done ||
                    original.sp-stack != count || ctx->interrupt_counter != final_counter || original.quota_exit != quota)
                    printf("region cfg mismatch source=%u input=%u capacity=%u mode=%u pc=%u/%u ops=%u/%u depth=%u/%u counter=%d/%d quota=%u/%u\n",
                        s,a,capacity,mode,(unsigned)(original.pc-body->byte_code_buf),pc,original.completed,before+done,
                        (unsigned)(original.sp-stack),count,ctx->interrupt_counter,final_counter,original.quota_exit,quota);
                CHECK(original.pc == body->byte_code_buf+pc && original.completed == before+done);
                CHECK(original.sp-stack == count && ctx->interrupt_counter == final_counter && original.quota_exit == quota);
                for(unsigned i=0;i<count;i++) CHECK(tf_reference_same(stack[i],values[i]));
                while(original.sp>stack) JS_FreeValue(ctx,*--original.sp);
                cfg_check_references(&refs);
#else
                (void)quota;
#endif
                CHECK(h->malloc_size == bytes && h->malloc_count == allocations);
                progress+=done>0; loops+=done == TF_REGION_QUANTUM; tested++;
                if(overflow) overflow->ref_count=old_count;
            }
            JS_FreeValue(ctx,argument);
        }
#ifdef __PSP__
        destroy_plan(ctx,budget,heap,plan);
#endif
        JS_FreeValue(ctx,function);
    }
    ctx->interrupt_counter=saved_counter; budget_free(budget,g);
#ifdef __PSP__
    cfg_owned_stores=selected_owned;
#endif
    CHECK(progress && loops);
    printf("region borrowed CFG owned-prefix differential, branches, loops and exact handoff=pass cases=%u progress=%u quantum-loops=%u\n",tested,progress,loops);
}
/* Same emitted property/branch guards as the timed path, with only the final
   branch routed to the epilogue so exact pre-handoff state can be inspected. */
static void cfg_owned_guard_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
#ifdef __PSP__
    if(!register_cfg) return;
    bool selected=cfg_owned_stores;
    cfg_owned_stores=cfg_owned_probe_exit=true;
#else
    (void)heap; (void)pool;
#endif
    TFBorrowedCFG *g=budget_calloc_category(budget,BUDGET_CATEGORY_JAVASCRIPT,1,sizeof(*g)); CHECK(g);
    static const char *sources[] = {
        "(function(o){var r;if(o.a){if(o.b)r=o.x;else r=o.y}else if(o.c)r=o.z;else r=o.x;return r})",
        "(function(o){let r;if(o.a){if(o.b)r=o.x;else r=o.y}else if(o.c)r=o.z;else r=o.x;return r})",
        "(function(o){var r;if(o.a&&o.b)r=o.x;else if(o.c)r=o.y;else r=o.z;return r})"
    };
    static const char *inputs[] = {
        "({a:true,b:true,c:true,x:{},y:'y',z:1n})",
        "({a:true,b:false,c:true,x:null,y:'a'.repeat(8192)+'b',z:1n})",
        "({a:false,b:true,c:true,x:19,y:{},z:1n<<80n})",
        "({a:false,b:false,c:false,x:Symbol('x'),y:23,z:{}})",
        "({a:NaN,b:true,x:19})", "({get a(){throw Error('do not invoke')},x:19})",
        "Object.create({a:true,b:true,x:19})", "null"
    };
    unsigned cases=0,stores=0,quotas=0,refusals=0;
    int saved_counter=ctx->interrupt_counter;
    for(unsigned s=0;s<sizeof(sources)/sizeof(sources[0]);s++) {
        JSValue function=eval(ctx,sources[s]);
        JSFunctionBytecode *body=JS_VALUE_GET_OBJ(function)->u.func.function_bytecode;
        unsigned start=0;
        while(start < body->byte_code_len && body->byte_code_buf[start] != OP_get_arg0)
            start+=tf_region_opcodes[body->byte_code_buf[start]].size;
        CHECK(body->var_count == 1 && start < body->byte_code_len && tf_cfg_scan(body,NULL,start,g));
#ifdef __PSP__
        TFRegionPlan *plan=compile_plan(ctx,budget,heap,pool,function);
        CHECK(plan && plan->cfg_owned_stores && plan->entry[start]);
#endif
        for(unsigned a=0;a<sizeof(inputs)/sizeof(inputs[0]);a++) {
            JSValue argument=eval(ctx,inputs[a]);
            JSValue old=eval(ctx,"({old:true})");
            for(unsigned mode=0;mode<7;mode++) {
                JSValue local=mode == 4 ? JS_UNINITIALIZED : JS_DupValue(ctx,old);
                JSValue initial_local=local;
                if(mode == 6 && JS_IsObject(argument) && !JS_VALUE_GET_OBJ(argument)->is_exotic)
                    CHECK(JS_SetPropertyStr(ctx,argument,"x",JS_DupValue(ctx,old)) >= 0);
                JSRefCountHeader *old_header=__js_rc(JS_VALUE_GET_PTR(old));
                int old_count=old_header->ref_count;
                if(mode == 5) old_header->ref_count=1; /* force final-release guard */
                CFGReferences refs={0}; cfg_collect_references(&refs,argument); cfg_collect_references(&refs,old);
                JSValue stack[3]={JS_UNDEFINED,JS_UNDEFINED,JS_UNDEFINED};
                TFRegionFrame f={.body=body,.args=&argument,.locals=&local,.base=stack,.sp=stack,
                    .limit=stack+3,.pc=body->byte_code_buf+start,.ctx=ctx};
                ctx->interrupt_counter=INT32_MAX;
                tf_cfg_predict(&f,g);
                unsigned prefix=f.completed;
                while(f.sp>stack) JS_FreeValue(ctx,*--f.sp);
                cfg_check_references(&refs);
                unsigned before=mode == 1 ? TF_REGION_QUANTUM-prefix : mode == 2 ? TF_REGION_QUANTUM : 0;
                int counter=mode == 3 ? 1 : INT32_MAX;
                f.pc=body->byte_code_buf+start; f.completed=before; f.quota_exit=false;
                ctx->interrupt_counter=counter;
                JSMallocState *h=&ctx->rt->malloc_ctx.malloc_state;
                size_t bytes=h->malloc_size,allocations=h->malloc_count;
                tf_cfg_predict(&f,g);
                uint32_t index;
                TFRegionHelper helper=tf_region_decode(f.pc,&index);
                bool eligible=helper == tf_region_put_local || helper == tf_region_put_local_check;
                bool committed=false;
                if(eligible) {
                    if(f.completed == TF_REGION_QUANTUM) f.quota_exit=true;
                    else committed=helper(&f,index,(f.pc-body->byte_code_buf)+tf_region_opcodes[*f.pc].size);
                }
                JSValue expected_local=local,expected[3]; memcpy(expected,stack,sizeof(expected));
                unsigned pc=f.pc-body->byte_code_buf,completed=f.completed,count=f.sp-stack;
                int final_counter=ctx->interrupt_counter; bool quota=f.quota_exit;
                while(f.sp>stack) JS_FreeValue(ctx,*--f.sp);
                if(committed) { JS_FreeValue(ctx,local); local=JS_DupValue(ctx,initial_local); }
                cfg_check_references(&refs);
#ifdef __PSP__
                f.pc=body->byte_code_buf+start; f.completed=before; f.quota_exit=false; f.plan=plan;
                ctx->interrupt_counter=counter;
                NativeCodeLease lease; CHECK(native_code_pool_enter(pool,plan->image,16,&lease));
                TFRegionEntry entry; memcpy(&entry,&lease.entry,sizeof(entry));
                entry(&f,(const char *)lease.entry+plan->entry[start]-1-16);
                CHECK(native_code_pool_leave(pool,lease));
                bool local_matches=(JS_IsUninitialized(local) && JS_IsUninitialized(expected_local)) ||
                    tf_reference_same(local,expected_local);
                if(f.pc != body->byte_code_buf+pc || f.completed != completed || f.sp-stack != count ||
                    f.quota_exit != quota || !local_matches)
                    printf("region owned-cfg mismatch source=%u input=%u mode=%u pc=%u/%u ops=%u/%u depth=%u/%u quota=%u/%u\n",
                        s,a,mode,(unsigned)(f.pc-body->byte_code_buf),pc,f.completed,completed,
                        (unsigned)(f.sp-stack),count,f.quota_exit,quota);
                CHECK(f.pc == body->byte_code_buf+pc && f.completed == completed && f.sp-stack == count);
                CHECK(f.quota_exit == quota && ctx->interrupt_counter == final_counter && local_matches);
                for(unsigned i=0;i<count;i++) CHECK(tf_reference_same(stack[i],expected[i]));
                while(f.sp>stack) JS_FreeValue(ctx,*--f.sp);
                if(committed) { JS_FreeValue(ctx,local); local=JS_DupValue(ctx,initial_local); }
                cfg_check_references(&refs);
#else
                (void)pc; (void)completed; (void)count; (void)final_counter;
                (void)expected_local; (void)expected;
#endif
                CHECK(h->malloc_size == bytes && h->malloc_count == allocations);
                cases++; stores+=committed; quotas+=quota; refusals+=eligible && !committed && !quota;
                if(mode == 5) old_header->ref_count=old_count;
                JS_FreeValue(ctx,local);
            }
            JS_FreeValue(ctx,argument); JS_FreeValue(ctx,old);
        }
#ifdef __PSP__
        destroy_plan(ctx,budget,heap,plan);
#endif
        JS_FreeValue(ctx,function);
    }
    CHECK(stores && quotas && refusals);
    budget_free(budget,g);
#ifdef __PSP__
    cfg_owned_stores=selected; cfg_owned_probe_exit=false;
#endif
    ctx->interrupt_counter=saved_counter;
    printf("region materialized owned-store differential, alias, final-release, TDZ and quota=pass cases=%u stores=%u quotas=%u refusals=%u\n",
        cases,stores,quotas,refusals);
}
#ifdef __PSP__
static void borrowed_emitted_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap,
                                    NativeCodePool *pool)
{
    if (!borrowed_fields) return;
    bool selected_probes = borrowed_field_probes;
    borrowed_probe_exit = borrowed_field_probes = true;
    int counter = ctx->interrupt_counter;
    JSValue function = eval(ctx,"(function(a){if(a.value)return 1;return 0})");
    TFRegionPlan *plan = compile_plan(ctx,budget,heap,pool,function);
    CHECK(plan && plan->borrowed_field_blocks == 1 && plan->entry[0]);
    TFPrimitiveBlock block;
    CHECK(tf_value_scan(plan->body,NULL,0,&block,true) && block.count == 3);
    static const char *inputs[] = {"({value:1})","({value:0})","({value:true})",
        "({value:false})","({value:null})","({value:void 0})","({value:3.5})",
        "({value:NaN})","({value:-0})","({value:''})","({value:'wide \\u{1f600}'})",
        "({value:'a'.repeat(8192)+'b'})","({value:{}})","({value:Symbol('x')})",
        "({value:1n})","({value:1n<<80n})","Object.create({value:1})",
        "({get value(){throw Error('getter must not run')}})",
        "new Proxy({value:1},{get(){throw Error('proxy must not run')}})",
        "(()=>{let o={};o.value=o;return o})()","null","undefined",
        "Object.freeze({value:true})","({value:{}})","({value:[]})",
        "({value:new Proxy({},{get(){throw Error('truth must not trap')}})})"};
    unsigned tested = 0, accepted = 0;
    for (unsigned input = 0; input < sizeof(inputs)/sizeof(inputs[0]); input++) {
        JSValue argument = eval(ctx,inputs[input]);
        if (input == 23) {
            JSValue child = JS_GetPropertyStr(ctx,argument,"value");
            JS_VALUE_GET_OBJ(child)->is_HTMLDDA = 1;
            JS_FreeValue(ctx,child);
        }
        bool own = false;
        JSValue field = JS_UNDEFINED;
        if (JS_IsObject(argument) && !JS_VALUE_GET_OBJ(argument)->is_exotic) {
            JSProperty *property;
            JSShapeProperty *shape = find_own_property(&property,JS_VALUE_GET_OBJ(argument),block.arguments[1]);
            own = shape && !(shape->flags & JS_PROP_TMASK);
            if (own) field = property->u.value;
        }
        for (unsigned capacity = 0; capacity < 2; capacity++) {
            for (unsigned mode = 0; mode < 5; mode++) {
                JSRefCountHeader *overflow = mode == 3 && JS_VALUE_HAS_REF_COUNT(argument) ?
                    __js_rc(JS_VALUE_GET_PTR(argument)) : mode == 4 && own && JS_VALUE_HAS_REF_COUNT(field) ?
                    __js_rc(JS_VALUE_GET_PTR(field)) : NULL;
                int original_count = overflow ? overflow->ref_count : 0;
                if (overflow) overflow->ref_count = INT32_MAX;
                JSValue stack[1] = {JS_NewInt32(ctx,37)};
                TFRegionFrame f = {.body=plan->body,.args=&argument,.base=stack,.limit=stack+capacity,
                    .sp=stack,.pc=plan->body->byte_code_buf,.ctx=ctx,.plan=plan,
                    .completed=mode == 2 ? TF_REGION_QUANTUM-block.count+1 : 0};
                unsigned before = f.completed;
                ctx->interrupt_counter = mode == 1 ? 1 : 19;
                TFReferenceResult prediction;
                unsigned status = tf_reference_guards(ctx,plan->body,&block,stack,stack+capacity,stack,
                                                       NULL,&argument,NULL,&prediction);
                bool admit = own && status == TF_REFERENCE_ACCEPT && mode != 2 &&
                    JS_VALUE_GET_NORM_TAG(prediction.values[0]) != JS_TAG_FLOAT64;
                JSMallocState *state = &ctx->rt->malloc_ctx.malloc_state;
                size_t bytes = state->malloc_size, allocations = state->malloc_count;
                borrowed_field_hits = 0;
                NativeCodeLease lease;
                CHECK(native_code_pool_enter(pool,plan->image,16,&lease));
                TFRegionEntry entry; memcpy(&entry,&lease.entry,sizeof(entry));
                entry(&f,(const char *)lease.entry+plan->entry[0]-1-16);
                CHECK(native_code_pool_leave(pool,lease));
                CHECK(state->malloc_size == bytes && state->malloc_count == allocations);
                CHECK(f.sp == stack && JS_VALUE_GET_INT(stack[0]) == 37 && !f.threw && !f.quota_exit);
                if (f.completed != before+(admit ? block.count : 0))
                    printf("region borrowed-mismatch input=%u capacity=%u mode=%u own=%u status=%u admit=%u completed=%u before=%u pc=%u hits=%u counter=%d\n",
                        input,capacity,mode,own,status,admit,f.completed,before,
                        (unsigned)(f.pc-plan->body->byte_code_buf),borrowed_field_hits,ctx->interrupt_counter);
                CHECK(f.completed == before+(admit ? block.count : 0));
                CHECK(f.pc == plan->body->byte_code_buf+(admit ? prediction.next : 0));
                CHECK(ctx->interrupt_counter == (mode == 1 ? 1 : 19)-(int)admit);
                CHECK(borrowed_field_hits == admit);
                for (unsigned i = 0; i < prediction.ref_count; i++)
                    CHECK(prediction.refs[i].header->ref_count == prediction.refs[i].initial);
                if (overflow) overflow->ref_count = original_count;
                accepted += admit; tested++;
            }
        }
        JS_FreeValue(ctx,argument);
    }
    destroy_plan(ctx,budget,heap,plan); JS_FreeValue(ctx,function);
    ctx->interrupt_counter = counter;
    borrowed_probe_exit = false; borrowed_field_probes = selected_probes;
    printf("region emitted borrowed-field stack, ownership, truth, poll and refusal=pass cases=%u accepted=%u\n",tested,accepted);
}
#endif
static void emitted_continuation_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    static const struct { const char *source; unsigned opcode; bool binary, zero_result; } checks[] = {
        {"(function(x){return !x})", OP_lnot, false, true},
        {"(function(x){return x===null})", OP_is_null, false, false},
        {"(function(x){return x===void 0})", OP_is_undefined, false, false},
        {"(function(x,y){return x===y})", OP_strict_eq, true, true},
        {"(function(x,y){return x!==y})", OP_strict_neq, true, false},
    };
    for (unsigned c = 0; c < sizeof(checks) / sizeof(checks[0]); c++) {
        JSValue function = eval(ctx, checks[c].source);
        TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
        CHECK(plan != NULL);
#ifdef __PSP__
        CHECK(!continuation_values || plan->semantic_direct_sites > 0);
#endif
        unsigned at = 0;
        while (at < (unsigned)plan->body->byte_code_len && plan->body->byte_code_buf[at] != checks[c].opcode)
            at += tf_region_opcodes[plan->body->byte_code_buf[at]].size;
        if (at >= (unsigned)plan->body->byte_code_len || !plan->entry[at])
            fprintf(stderr, "continuation fixture opcode=%s source=%s at=%u bytes=%u\n",
                tf_region_opcodes[checks[c].opcode].name, checks[c].source, at, plan->body->byte_code_len);
        CHECK(at < (unsigned)plan->body->byte_code_len && plan->entry[at]);
        for (unsigned scenario = 0; scenario < 4; scenario++) {
            JSValue stack[2] = {JS_UNDEFINED, JS_UNDEFINED};
            unsigned count = checks[c].binary ? 2 : 1;
            if (scenario == 0) count--; /* underflow before the operation */
            else if (scenario == 1) {
                stack[0] = JS_NewObject(ctx);
                if (count == 2) stack[1] = JS_DupValue(ctx, stack[0]);
            } else {
                stack[0] = stack[1] = JS_NewInt32(ctx, 0);
            }
            TFRegionFrame f = {.body = plan->body, .base = stack, .limit = stack + 2,
                .sp = stack + count, .pc = plan->body->byte_code_buf + at, .ctx = ctx};
            if (scenario == 3) f.completed = TF_REGION_QUANTUM;
            NativeCodeLease lease;
            CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
            TFRegionEntry entry;
            memcpy(&entry, &lease.entry, sizeof(entry));
            entry(&f, (const char *)lease.entry + plan->entry[at] - 1 - 16);
            CHECK(native_code_pool_leave(pool, lease));
            if (scenario == 3) {
                CHECK(f.completed == TF_REGION_QUANTUM && f.quota_exit &&
                    f.pc == plan->body->byte_code_buf + at && f.sp == stack + count);
                CHECK(JS_VALUE_GET_TAG(stack[0]) == JS_TAG_INT && JS_VALUE_GET_INT(stack[0]) == 0);
            } else if (scenario < 2) {
                CHECK(f.completed == 0 && f.pc == plan->body->byte_code_buf + at && f.sp == stack + count);
                if (scenario == 1) CHECK(__js_rc(JS_VALUE_GET_PTR(stack[0]))->ref_count == (int)count);
            } else {
                CHECK(f.completed == 1 && f.pc == plan->body->byte_code_buf + at + 1 && f.sp == stack + 1);
                CHECK(JS_VALUE_GET_TAG(stack[0]) == JS_TAG_BOOL && JS_VALUE_GET_INT(stack[0]) == checks[c].zero_result);
            }
            while (f.sp > stack) JS_FreeValue(ctx, *--f.sp);
        }
        destroy_plan(ctx, budget, heap, plan);
        JS_FreeValue(ctx, function);
    }
    puts("region emitted continuation underflow, final-reference and result guards=pass");
    static const char *values[] = {"undefined", "null", "false", "true", "0", "-1", "1",
        "-2147483648", "2147483647", "-0", "NaN", "Infinity", "1.5", "''", "'x'",
        "'\\u{1f600}'", "0n", "1n", "1n<<80n", "Symbol('x')", "({})", "Object.create(null)"};
    JSValue inputs[sizeof(values) / sizeof(values[0])];
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) inputs[i] = eval(ctx, values[i]);
    JSValue function = eval(ctx, "(function(x,y){return [x===y,x!==y,!x,x===null,x===void 0,(x??9)===9].join(':')})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan != NULL);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        for (unsigned j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            JSValue args[2] = {inputs[i], inputs[j]};
            tf_region_plan = NULL;
            JSValue expected = JS_Call(ctx, function, JS_UNDEFINED, 2, args);
            tf_region_plan = plan;
            JSValue actual = JS_Call(ctx, function, JS_UNDEFINED, 2, args);
            CHECK(!JS_IsException(expected) && !JS_IsException(actual) && JS_StrictEq(ctx, expected, actual));
            JS_FreeValue(ctx, expected);
            JS_FreeValue(ctx, actual);
        }
    }
    tf_region_plan = NULL;
    CHECK(plan->completed > 0 && plan->active_frames == 0);
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) JS_FreeValue(ctx, inputs[i]);
    puts("region continuation 484-pair predicate differential=pass");
#ifdef __PSP__
    /* A TDZ initialization followed by an unsupported global read stops at
       precisely one operation. Exercise final/non-final local ownership and
       the quantum before any release; no actual-frame cleanup is bypassed. */
    function = eval(ctx, "(function(){let x=globalThis.seed;return x?x:x})");
    plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan != NULL);
    CHECK(!continuation_values || plan->semantic_direct_sites > 0);
    unsigned at = 0;
    while (at < (unsigned)plan->body->byte_code_len &&
           plan->body->byte_code_buf[at] != OP_set_loc_uninitialized)
        at += tf_region_opcodes[plan->body->byte_code_buf[at]].size;
    CHECK(at < (unsigned)plan->body->byte_code_len && plan->entry[at]);
    unsigned index = get_u16(plan->body->byte_code_buf + at + 1);
    CHECK(plan->body->var_count == 1 && index == 0);
    CHECK(!cached_bindings || (plan->binding_kind == 1 && plan->binding_index == index));
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        JSValue local = JS_NewObject(ctx), stack[1] = {JS_UNDEFINED};
        JSValue retained = scenario == 1 ? JS_DupValue(ctx, local) : JS_UNDEFINED;
        TFRegionFrame f = {.body = plan->body, .locals = &local, .base = stack,
            .limit = stack + 1, .sp = stack, .pc = plan->body->byte_code_buf + at, .ctx = ctx};
        if (scenario == 2) f.completed = TF_REGION_QUANTUM;
        NativeCodeLease lease;
        CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
        TFRegionEntry entry;
        memcpy(&entry, &lease.entry, sizeof(entry));
        entry(&f, (const char *)lease.entry + plan->entry[at] - 1 - 16);
        CHECK(native_code_pool_leave(pool, lease));
        CHECK(f.sp == stack);
        if (scenario == 1) {
            CHECK(f.completed == 1 && f.pc == plan->body->byte_code_buf + at + 3);
            CHECK(JS_VALUE_GET_TAG(local) == JS_TAG_UNINITIALIZED);
            CHECK(__js_rc(JS_VALUE_GET_PTR(retained))->ref_count == 1);
        } else {
            CHECK(JS_IsObject(local) && __js_rc(JS_VALUE_GET_PTR(local))->ref_count == 1);
            CHECK(f.pc == plan->body->byte_code_buf + at);
            CHECK(f.completed == (scenario == 2 ? TF_REGION_QUANTUM : 0));
            CHECK(f.quota_exit == (scenario == 2));
        }
        JS_FreeValue(ctx, local);
        JS_FreeValue(ctx, retained);
    }
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    puts("region Allegrex TDZ direct final-release and quota guards=pass");
#endif
}
#endif

static void emitted_guard_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    JSValue function = eval(ctx, "(function(x){var y=x;var z=0;return y})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan && plan->entry[0] && (plan->chained || plan->steps[0] == 5) && plan->body->var_count == 2);
    uint32_t argument;
    CHECK(tf_region_decode(plan->body->byte_code_buf, &argument) == tf_region_arg && argument == 0);
    CHECK(tf_region_decode(plan->body->byte_code_buf + 1, &argument) == tf_region_put_local && argument == 0);
    for (unsigned scenario = 0; scenario < 3; scenario++) {
        JSValue args[1] = {JS_NewObject(ctx)}, locals[2] = {JS_NewObject(ctx), JS_UNDEFINED};
        JSValue stack[2] = {JS_UNDEFINED, JS_UNDEFINED};
        CHECK(!JS_IsException(args[0]) && !JS_IsException(locals[0]));
        JSValue retained = scenario == 2 ? JS_DupValue(ctx, locals[0]) : JS_UNDEFINED;
        TFRegionFrame frame = {plan->body, locals, args, stack,
            scenario == 0 ? stack : stack + 2, stack, NULL, plan->body->byte_code_buf, 0};
        NativeCodeLease lease;
        CHECK(native_code_pool_enter(pool, plan->image, plan->chained ? 16 : plan->entry[0] - 1, &lease));
        TFRegionEntry entry;
        memcpy(&entry, &lease.entry, sizeof(entry));
        entry(&frame, plan->chained ? (const char *)lease.entry + plan->entry[0] - 1 - 16 : NULL);
        CHECK(native_code_pool_leave(pool, lease));
        if (scenario == 0) {
            CHECK(frame.completed == 0 && frame.pc == plan->body->byte_code_buf && frame.sp == stack);
            CHECK(__js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count == 1);
        } else if (scenario == 1) {
            CHECK(frame.completed == 1 && frame.pc == plan->body->byte_code_buf + 1 && frame.sp == stack + 1);
            CHECK(__js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count == 2);
            CHECK(__js_rc(JS_VALUE_GET_PTR(locals[0]))->ref_count == 1);
        } else {
            CHECK(frame.completed == 5 && frame.pc == plan->body->byte_code_buf + 5 && frame.sp == stack + 1);
            CHECK(__js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count == 3);
            CHECK(__js_rc(JS_VALUE_GET_PTR(retained))->ref_count == 1);
        }
        while (frame.sp > stack) JS_FreeValue(ctx, *--frame.sp);
        JS_FreeValue(ctx, retained);
        JS_FreeValue(ctx, locals[0]);
        JS_FreeValue(ctx, locals[1]);
        JS_FreeValue(ctx, args[0]);
    }
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    puts("region emitted stack-bound and final-reference exits=pass");
}

#ifdef __PSP__
static void emitted_basic_block_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    /* Identical setup in both timed lanes: candidate-only qualification can
       perturb later allocator placement even with equal code mapping sizes. */
    bool requested = basic_blocks;
    bool requested_cold = cold_blocks;
    bool requested_values = value_blocks;
    value_blocks = false;
    basic_blocks = true;
    cold_blocks = true;
    JSValue function = eval(ctx, "(function(a,b){if(a<b)return globalThis.blockLeft;return globalThis.blockRight})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan && plan->basic_blocks == 1 && plan->cold_blocks == 1 && plan->entry[0]);
    unsigned positions[5] = {0};
    for (unsigned i = 0; i < 4; i++)
        positions[i+1] = positions[i] + tf_region_opcodes[plan->body->byte_code_buf[positions[i]]].size;
    const uint8_t *code = plan->body->byte_code_buf;
    CHECK(code[0] == OP_get_arg0 && code[positions[1]] == OP_get_arg1 && code[positions[2]] == OP_lt);
    CHECK(code[positions[3]] == OP_if_false8 || code[positions[3]] == OP_if_false);
    int32_t delta = code[positions[3]] == OP_if_false ?
        (int32_t)get_u32(code + positions[3] + 1) : (int8_t)code[positions[3] + 1];
    unsigned target = positions[3] + 1 + delta;
    /* Test the lowered entry itself, including exact partial original state.
       Integer/float/string/object tag refusals must leave comparison semantics
       and owned values to the interpreter, without calling valueOf early. */
    static const char *inputs[] = {"-2147483648", "2147483647", "1.25", "'3'", "({valueOf(){throw Error('must not run')}})"};
    for (unsigned input = 0; input < sizeof(inputs)/sizeof(inputs[0]); input++)
        for (unsigned capacity = 0; capacity <= 2; capacity++)
            for (unsigned slow = 0; slow < 3; slow++) {
                JSValue args[2] = {input < 2 ? JS_NewInt32(ctx, input == 0 ? INT32_MIN : INT32_MAX) :
                    eval(ctx, inputs[input]), JS_NewInt32(ctx, 4)};
                JSValue stack[2] = {JS_UNDEFINED, JS_UNDEFINED};
                unsigned initial = slow == 2 ? TF_REGION_QUANTUM - 3 : 0;
                ctx->interrupt_counter = slow == 1 ? 1 : 19;
                TFRegionFrame f = {.body = plan->body, .args = args, .base = stack,
                    .limit = stack + capacity, .sp = stack, .pc = code, .ctx = ctx,
                    .completed = initial};
                NativeCodeLease lease;
                CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
                TFRegionEntry entry;
                memcpy(&entry, &lease.entry, sizeof(entry));
                entry(&f, (const char *)lease.entry + plan->entry[0] - 1 - 16);
                CHECK(native_code_pool_leave(pool, lease));
                unsigned done, depth, pc;
                if (capacity < 2) { done = depth = capacity; pc = positions[capacity]; }
                else if (input >= 2) { done = depth = 2; pc = positions[2]; }
                else if (slow) { done = 3; depth = 1; pc = positions[3]; }
                else { done = 4; depth = 0; pc = input == 0 ? positions[4] : target; }
                if (f.completed != initial + done || f.sp != stack + depth || f.pc != code + pc)
                    printf("region block-guard input=%u capacity=%u slow=%u completed=%u/%u depth=%u/%u pc=%u/%u\n",
                        input, capacity, slow, f.completed, initial + done,
                        (unsigned)(f.sp-stack), depth, (unsigned)(f.pc-code), pc);
                CHECK(f.completed == initial + done && f.sp == stack + depth && f.pc == code + pc);
                CHECK(!f.threw && f.quota_exit == (capacity == 2 && input < 2 && slow == 2));
                CHECK(ctx->interrupt_counter == (capacity == 2 && input < 2 && !slow ? 18 : slow == 1 ? 1 : 19));
                if (capacity == 2 && input < 2 && slow) {
                    CHECK(JS_IsBool(stack[0]) && JS_VALUE_GET_INT(stack[0]) == (input == 0));
                }
                for (unsigned i = 0; i < depth; i++) JS_FreeValue(ctx, stack[i]);
                JS_FreeValue(ctx, args[0]);
            }
    /* Interpreter continuations or branch targets may enter any interior
       operation after the preceding operations already published their stack.
       Out-of-line layout must not erase or redirect these entry points. */
    for (unsigned interior = 1; interior < 4; interior++) {
        CHECK(plan->entry[positions[interior]] != 0);
        CHECK(plan->entry[positions[interior]] > plan->entry[0]);
        for (unsigned truth = 0; truth < 2; truth++) {
            JSValue args[2] = {JS_NewInt32(ctx, truth ? -7 : 8), JS_NewInt32(ctx, 4)};
            JSValue stack[2] = {args[0], args[1]};
            unsigned depth = interior == 2 ? 2 : 1;
            if (interior == 3) stack[0] = JS_NewBool(ctx, truth);
            ctx->interrupt_counter = 19;
            TFRegionFrame f = {.body = plan->body, .args = args, .base = stack,
                .limit = stack + 2, .sp = stack + depth, .pc = code + positions[interior], .ctx = ctx};
            NativeCodeLease lease;
            CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
            TFRegionEntry entry;
            memcpy(&entry, &lease.entry, sizeof(entry));
            entry(&f, (const char *)lease.entry + plan->entry[positions[interior]] - 1 - 16);
            CHECK(native_code_pool_leave(pool, lease));
            CHECK(f.completed == 4 - interior && f.sp == stack);
            CHECK(f.pc == code + (truth ? positions[4] : target));
            CHECK(ctx->interrupt_counter == 18 && !f.threw && !f.quota_exit);
        }
    }
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    puts("region basic-block exact guards, quota, cold entries and stack publication=pass cases=51");
    basic_blocks = requested;
    cold_blocks = requested_cold;
    value_blocks = requested_values;
}

static void emitted_value_block_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    bool selected = value_blocks, selected_values = continuation_values;
    bool selected_probes = value_block_probes;
    value_blocks = continuation_values = true;
    value_block_probes = true;
    JSValue function = eval(ctx, "(function(a){return a??globalThis.blockFallback})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan && plan->value_blocks > 0 && plan->entry[0]);
    const uint8_t *code = plan->body->byte_code_buf;
    unsigned positions[5] = {0};
    for (unsigned i = 0; i < 4; i++)
        positions[i+1] = positions[i] + tf_region_opcodes[code[positions[i]]].size;
    CHECK(code[0] == OP_get_arg0 && code[positions[1]] == OP_dup &&
          code[positions[2]] == OP_is_undefined_or_null &&
          (code[positions[3]] == OP_if_false8 || code[positions[3]] == OP_if_false));
    int32_t delta = code[positions[3]] == OP_if_false ?
        (int32_t)get_u32(code + positions[3] + 1) : (int8_t)code[positions[3] + 1];
    unsigned target = positions[3] + 1 + delta;
    static const char *inputs[] = {"undefined", "null", "false", "true", "0", "-7", "1.5", "'x'", "({})",
        "-2147483648", "2147483647", "-0", "NaN", "Infinity", "''", "'\\u{1f600}'",
        "0n", "1n", "1n<<80n", "Symbol('x')", "Object.create(null)"};
    unsigned cases = 0;
    for (unsigned input = 0; input < sizeof(inputs)/sizeof(inputs[0]); input++) {
        for (unsigned capacity = 0; capacity <= 2; capacity++) {
            for (unsigned slow = 0; slow < 3; slow++) {
                JSValue args[1] = {eval(ctx, inputs[input])};
                int initial_refs = JS_VALUE_HAS_REF_COUNT(args[0]) ? __js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count : 0;
                bool primitive = (unsigned)JS_VALUE_GET_TAG(args[0]) <= JS_TAG_UNDEFINED;
                JSValue stack[2] = {JS_UNDEFINED, JS_UNDEFINED};
                unsigned initial = slow == 2 ? TF_REGION_QUANTUM - 3 : 0;
                ctx->interrupt_counter = slow == 1 ? 1 : 19;
                TFRegionFrame f = {.body=plan->body, .args=args, .base=stack, .limit=stack+capacity,
                    .sp=stack, .pc=code, .ctx=ctx, .completed=initial};
                NativeCodeLease lease;
                CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
                TFRegionEntry entry;
                memcpy(&entry, &lease.entry, sizeof(entry));
                entry(&f, (const char *)lease.entry + plan->entry[0] - 1 - 16);
                CHECK(native_code_pool_leave(pool, lease));
                /* A nullish operand falls through drop then an unsupported
                   global read. Non-nullish operands stop at return. */
                unsigned done = capacity < 2 ? capacity : slow ? 3 : input < 2 ? 5 : 4;
                unsigned depth = capacity < 2 ? capacity : slow ? 2 : input < 2 ? 0 : 1;
                unsigned pc = capacity < 2 ? positions[capacity] : slow ? positions[3] :
                    (input < 2 ? positions[4]+1 : target);
                if (f.completed != initial+done || f.sp != stack+depth || f.pc != code+pc)
                    printf("region value-block guard input=%u capacity=%u slow=%u completed=%u/%u depth=%u/%u pc=%u/%u\n",
                        input,capacity,slow,f.completed,initial+done,(unsigned)(f.sp-stack),depth,(unsigned)(f.pc-code),pc);
                CHECK(f.completed == initial+done && f.sp == stack+depth && f.pc == code+pc);
                CHECK(f.value_block_hits == (primitive && capacity == 2 && !slow));
                CHECK(ctx->interrupt_counter == (capacity == 2 && !slow ? 18 : slow == 1 ? 1 : 19));
                CHECK(f.quota_exit == (capacity == 2 && slow == 2));
                if (depth) CHECK(!memcmp(&stack[0],&args[0],sizeof(JSValue)));
                if (depth == 2) CHECK(JS_IsBool(stack[1]) && JS_VALUE_GET_INT(stack[1]) == (input < 2));
                if (JS_VALUE_HAS_REF_COUNT(args[0]))
                    CHECK(__js_rc(JS_VALUE_GET_PTR(args[0]))->ref_count == initial_refs + (depth != 0));
                while (f.sp > stack) JS_FreeValue(ctx,*--f.sp);
                JS_FreeValue(ctx,args[0]);
                cases++;
            }
        }
    }
    /* Interior entries remain ordinary canonical operations: a resumed
       interpreter must not execute the hot block as though its prefix ran. */
    for (unsigned interior = 1; interior < 4; interior++) {
        CHECK(plan->entry[positions[interior]] != 0);
        for (unsigned input = 0; input < 4; input++) {
            JSValue args[1] = {eval(ctx, inputs[input])};
            JSValue stack[2] = {JS_DupValue(ctx,args[0]), JS_UNDEFINED};
            unsigned depth = interior == 1 ? 1 : 2;
            if (interior == 2) stack[1] = JS_DupValue(ctx,args[0]);
            else if (interior == 3) stack[1] = JS_NewBool(ctx,input < 2);
            ctx->interrupt_counter = 19;
            TFRegionFrame f = {.body=plan->body, .args=args, .base=stack, .limit=stack+2,
                .sp=stack+depth, .pc=code+positions[interior], .ctx=ctx};
            NativeCodeLease lease;
            CHECK(native_code_pool_enter(pool,plan->image,16,&lease));
            TFRegionEntry entry;
            memcpy(&entry,&lease.entry,sizeof(entry));
            entry(&f,(const char *)lease.entry+plan->entry[positions[interior]]-1-16);
            CHECK(native_code_pool_leave(pool,lease));
            CHECK(f.completed == (input < 2 ? 5u : 4u)-interior);
            CHECK(f.sp == stack+(input < 2 ? 0 : 1));
            CHECK(f.pc == code+(input < 2 ? positions[4]+1 : target));
            CHECK(ctx->interrupt_counter == 18 && f.value_block_hits == 0 && !f.quota_exit);
            if (f.sp > stack) CHECK(JS_StrictEq(ctx,stack[0],args[0]));
            while (f.sp > stack) JS_FreeValue(ctx,*--f.sp);
            JS_FreeValue(ctx,args[0]);
            cases++;
        }
    }
    destroy_plan(ctx,budget,heap,plan);
    JS_FreeValue(ctx,function);
    value_blocks = selected;
    continuation_values = selected_values;
    value_block_probes = selected_probes;
    printf("region primitive value-block canonical fallback, polls and ownership=pass cases=%u\n",cases);
}
static void emitted_fused_field_tests(JSContext *ctx, Budget *budget, ProbeHeap *heap, NativeCodePool *pool)
{
    if (!fused_fields) return;
    JSValue function = eval(ctx, "(function(object){return object.x})");
    TFRegionPlan *plan = compile_plan(ctx, budget, heap, pool, function);
    CHECK(plan && plan->fused_fields == 1 && plan->entry[0]);
    CHECK(plan->body->byte_code_buf[0] == OP_get_arg0);
    unsigned field_size = tf_region_opcodes[plan->body->byte_code_buf[1]].size;
    for (unsigned capacity = 0; capacity < 2; capacity++) {
        JSValue receiver = eval(ctx, "({x:{value:7}})");
        JSValue child = JS_GetPropertyStr(ctx, receiver, "x");
        int receiver_refs = __js_rc(JS_VALUE_GET_PTR(receiver))->ref_count;
        int child_refs = __js_rc(JS_VALUE_GET_PTR(child))->ref_count;
        JSValue stack[1] = {JS_UNDEFINED};
        TFRegionFrame f = {.body = plan->body, .args = &receiver, .base = stack,
            .limit = stack + capacity, .sp = stack, .pc = plan->body->byte_code_buf, .ctx = ctx};
        NativeCodeLease lease;
        CHECK(native_code_pool_enter(pool, plan->image, 16, &lease));
        TFRegionEntry entry;
        memcpy(&entry, &lease.entry, sizeof(entry));
        entry(&f, (const char *)lease.entry + plan->entry[0] - 1 - 16);
        CHECK(native_code_pool_leave(pool, lease));
        CHECK(__js_rc(JS_VALUE_GET_PTR(receiver))->ref_count == receiver_refs);
        CHECK(__js_rc(JS_VALUE_GET_PTR(child))->ref_count == child_refs + (int)capacity);
        CHECK(f.completed == capacity * 2 && f.sp == stack + capacity);
        CHECK(f.pc == plan->body->byte_code_buf + (capacity ? 1 + field_size : 0));
        if (capacity) {
            CHECK(JS_VALUE_GET_PTR(stack[0]) == JS_VALUE_GET_PTR(child));
            JS_FreeValue(ctx, stack[0]);
        }
        JS_FreeValue(ctx, child);
        JS_FreeValue(ctx, receiver);
    }
    destroy_plan(ctx, budget, heap, plan);
    JS_FreeValue(ctx, function);
    puts("region fused-field capacity and ownership guards=pass");
}
#endif

/* Same JavaScript and values in both lanes; includes interpreter fallback,
   pool leases, frame traffic and diagnostic counting. Short-helper, direct,
   and chained variants share these exact workloads and watchdog checks. */
static void throughput(bool measure)
{
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator != NULL);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    guard_tests(ctx);
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    primitive_guard_tests(ctx);
    reference_guard_tests(ctx);
    reference_commit_tests(ctx);
#endif
    ProbeHeap heap = {rt, 8u * 1024u * 1024u, 0, false};
    JS_SetMemoryLimit(rt, heap.ceiling);
    JSValue function = eval(ctx, mixed_workload ?
        "(function(object,callback,count){let result;for(let i=0;i<count;i++){let value=object.value;let valid=value===19;let blocked=!object.enabled;if(valid&&!blocked)result=callback(value)}return result})" :
        "(function(object,callback,count){let result;for(let i=0;i<count;i++){if(object.enabled){result=callback(object.value)}}return result})");
    NativeCodePool *pool = native_code_pool_create(&budget,
        (NativeCodeHeap){&heap, reserve, release},
        (NativeCodeBackend){NULL, code_page_size(), map_code, publish_code, unmap_code}, NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    cfg_guard_tests(ctx,&budget,&heap,pool);
    cfg_owned_guard_tests(ctx,&budget,&heap,pool);
#endif
#if defined(__PSP__) && defined(TF_REGION_SEMANTIC_CONTINUATIONS)
    borrowed_emitted_tests(ctx,&budget,&heap,pool);
#endif
    bool selected_returns = native_returns;
    /* These raw emitted-entry guards deliberately stop before the terminal
       opcode; actual-frame return ownership is qualified separately below. */
    native_returns = false;
    plan_refusal_tests(ctx, &budget, &heap, pool, function);
    bool selected_references = reference_blocks;
    reference_blocks = false;
    emitted_guard_tests(ctx, &budget, &heap, pool);
#ifdef __PSP__
    emitted_basic_block_tests(ctx, &budget, &heap, pool);
    emitted_value_block_tests(ctx, &budget, &heap, pool);
    emitted_fused_field_tests(ctx, &budget, &heap, pool);
#endif
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    emitted_continuation_tests(ctx, &budget, &heap, pool);
#endif
    reference_blocks = selected_references;
    native_returns = selected_returns;
    map_emission = profile_native;
    uint64_t begin = now_ns();
    TFRegionPlan *plan = compile_plan(ctx, &budget, &heap, pool, function);
    uint64_t compile_ns = now_ns() - begin;
    map_emission = false;
    CHECK(plan != NULL);
#ifdef __PSP__
    printf("region binding-cache enabled=%u kind=%u index=%u\n",
        cached_bindings, plan->binding_kind, plan->binding_index);
    if (cached_bindings) CHECK(plan->binding_kind != 0);
    printf("region fused-fields enabled=%u sites=%u\n", fused_fields, plan->fused_fields);
    if (fused_fields) CHECK(plan->fused_fields == 2);
    printf("region basic-blocks enabled=%u sites=%u\n", basic_blocks, plan->basic_blocks);
    if (basic_blocks) CHECK(plan->basic_blocks > 0);
    printf("region cold-blocks enabled=%u sites=%u hot-bytes=%u\n", cold_blocks, plan->cold_blocks,
        plan->cold_blocks ? plan->cold_start : plan->code_bytes);
    if (cold_blocks) CHECK(plan->cold_blocks == plan->basic_blocks);
#endif
    printf("region compile-ns=%llu regions=%u static-ops=%u code-bytes=%u pool-charge=%zu plan-bytes=%zu scratch-peak=%zu reference-count=%u reference-bytes=%zu\n",
        (unsigned long long)compile_ns, plan->entries, plan->instructions, plan->code_bytes,
        native_code_pool_charged_bytes(pool), tf_region_plan_bytes(plan->body->byte_code_len, plan->chained),
        compile_scratch_bytes(plan->body->byte_code_len, plan->chained), plan->reference_count, plan->reference_bytes);
    JSValue args[3] = { JS_UNDEFINED, eval(ctx, "(x=>x+1)"), JS_NewInt32(ctx, REGION_ITERATIONS) };
    static const char *objects[] = {
        "({enabled:true,value:19})",
        "Object.create({enabled:true,value:19})",
        "({get enabled(){return true},get value(){return 19}})",
        "new Proxy({enabled:true,value:19},{get(t,k){return t[k]}})",
    };
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
    args[0] = eval(ctx, objects[0]);
    args[2] = JS_NewInt32(ctx, 3);
    plan->calls = 0;
    tf_region_plan = plan;
    JS_SetExecutionCensus(rt, 1);
    JSValue census_result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
    CHECK(!JS_IsException(census_result) && JS_VALUE_GET_INT(census_result) == 20);
    CHECK(plan->calls == 0); /* no unreported native operations during census */
    JS_FreeValue(ctx, census_result);
    JS_SetExecutionCensus(rt, 0);
    tf_region_plan = NULL;
    JS_FreeValue(ctx, args[0]);
    args[0] = JS_UNDEFINED;
    args[2] = JS_NewInt32(ctx, REGION_ITERATIONS);
    puts("region active execution census stays interpreted=pass");
#endif
    if (profile_native) {
        CHECK(PROBE_LANES == 2 && chained_emission);
        NativeCodeLease map;
        CHECK(native_code_pool_enter(pool, plan->image, 0, &map));
        printf("region code-base=%p bytes=%u\n", map.entry, plan->code_bytes);
        CHECK(native_code_pool_leave(pool, map));
        args[0] = eval(ctx, objects[profile_property]);
        args[2] = JS_NewInt32(ctx, 2000000);
        tf_region_plan = plan;
        printf("region sample-window=begin native=1 property=%s iterations=200000000\n",
            profile_property == 2 ? "getter" : "own");
        fflush(stdout);
        for (unsigned i = 0; i < 100; i++) {
            JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
            CHECK(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 20);
            JS_FreeValue(ctx, result);
        }
        puts("region sample-window=end");
        JS_FreeValue(ctx, args[0]);
        args[2] = JS_NewInt32(ctx, REGION_ITERATIONS);
    }
    for (unsigned kind = 0; measure && kind < 4; kind++) {
        args[0] = eval(ctx, objects[kind]);
        uint64_t samples[2][REGION_SAMPLES];
        for (unsigned repeat = 0; repeat <= REGION_SAMPLES; repeat++) {
            for (unsigned lane = 0; lane < PROBE_LANES; lane++) {
                unsigned native = (lane + repeat) % PROBE_LANES;
                tf_region_plan = native ? plan : NULL;
                plan->calls = plan->completed = plan->guards = plan->quota_exits = plan->exceptions = plan->lease_acquisitions = 0;
                begin = now_ns();
                JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
                uint64_t elapsed = now_ns() - begin;
                CHECK(plan->active_frames == 0);
                if (PROBE_FRAME_LEASE && native) CHECK(plan->lease_acquisitions == 1);
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
                /* This is the continuity experiment's causal assertion: the
                   old emitter re-entered about four times per iteration. */
                if (mixed_workload && kind == 0 && native) CHECK(plan->calls < 2000);
#endif
                CHECK(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 20);
                JS_FreeValue(ctx, result);
                if (repeat) samples[native][repeat - 1] = elapsed;
                if (repeat == REGION_SAMPLES) printf("region work kind=%u mode=%u calls=%llu completed=%llu short-guard-exits=%llu quota=%llu exceptions=%llu leases=%llu\n", kind, native,
                    (unsigned long long)plan->calls, (unsigned long long)plan->completed, (unsigned long long)plan->guards,
                    (unsigned long long)plan->quota_exits, (unsigned long long)plan->exceptions,
                    (unsigned long long)plan->lease_acquisitions);
            }
        }
        printf("region timing kind=%u hook-compiled=%u direct=%u chain=%u value-abi=%u branch-abi=%u mixed=%u frame-lease=%u table-dispatch=%u continuation-values=%u interpreter-ns=%llu native-ns=%llu\n", kind, PROBE_LANES - 1, direct_emission, chained_emission, value_abi, direct_branches, mixed_workload, PROBE_FRAME_LEASE, PROBE_TABLE_DISPATCH, continuation_values,
            (unsigned long long)median(samples[0], REGION_SAMPLES),
            (unsigned long long)(PROBE_LANES == 2 ? median(samples[1], REGION_SAMPLES) : 0));
        JS_FreeValue(ctx, args[0]);
    }
    /* Identical branch polls: a native run must stop at the same iteration as
       the interpreter, not bypass the watchdog with an unmetered region. */
    args[0] = eval(ctx, "({enabled:true,value:19})");
    JS_FreeValue(ctx, args[1]);
    args[1] = eval(ctx, "(x=>{globalThis.pollWork++;return x+1})");
    int32_t work[2];
    for (unsigned native = 0; native < PROBE_LANES; native++) {
        JSValue global = JS_GetGlobalObject(ctx);
        CHECK(JS_SetPropertyStr(ctx, global, "pollWork", JS_NewInt32(ctx, 0)) >= 0);
        unsigned polls = 0;
        tf_region_plan = native ? plan : NULL;
        JS_SetInterruptHandler(rt, interrupt_probe, &polls);
        ctx->interrupt_counter = 1;
        JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
        CHECK(JS_IsException(result) && polls == 3);
        JSValue exception = JS_GetException(ctx);
        CHECK(!JS_IsNull(exception));
        JS_FreeValue(ctx, exception);
        JS_SetInterruptHandler(rt, NULL, NULL);
        JS_SetUncatchableException(ctx, false);
        JSValue done = JS_GetPropertyStr(ctx, global, "pollWork");
        CHECK(JS_ToInt32(ctx, &work[native], done) == 0);
        JS_FreeValue(ctx, done);
        JS_FreeValue(ctx, global);
        puts("region branch-watchdog=pass");
    }
    CHECK(work[0] > 0 && (PROBE_LANES == 1 || work[0] == work[1]));
    tf_region_plan = NULL;
    for (unsigned i = 0; i < 3; i++) JS_FreeValue(ctx, args[i]);
    destroy_plan(ctx, &budget, &heap, plan);
    branch_transcripts(ctx, &budget, &heap, pool);
    CHECK(native_code_pool_destroy(&pool));
    CHECK(heap.reserved == 0);
    JS_FreeValue(ctx, function);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator));
    CHECK(budget.current == 0);
}

#if defined(TF_REGION_FRAME_LEASE) && !defined(TF_REGION_INTERPRETER_ONLY)
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
#ifdef __PSP__
/* Four matched lanes share one rooted function/object and one interpreted
   driver. Warm-up proves execution with counters; timed samples omit their
   per-handoff writes. Identical-code controls expose placement sensitivity. */
static void cfg_throughput(bool measure)
{
    if(!register_cfg || !measure) return;
    bool selected_owned=cfg_owned_stores;
    bool selected_metrics=tf_region_collect_metrics;
    Budget budget; budget_init(&budget,16u*1024u*1024u);
    BudgetQuickJSPool *allocator=budget_quickjs_pool_create(&budget); CHECK(allocator);
    JSRuntime *rt=JS_NewRuntime2(budget_quickjs_pool_allocator(),allocator); CHECK(rt);
    JSContext *ctx=JS_NewContext(rt); CHECK(ctx);
    ProbeHeap heap={rt,8u*1024u*1024u,0,false}; JS_SetMemoryLimit(rt,heap.ceiling);
    NativeCodePool *pool=native_code_pool_create(&budget,(NativeCodeHeap){&heap,reserve,release},
        (NativeCodeBackend){NULL,code_page_size(),map_code,publish_code,unmap_code},NATIVE_POOL_BYTE_LIMIT); CHECK(pool);
    static const struct { const char *source,*object; } families[] = {
        {"(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})","({a:true,b:true,c:true,x:19,y:19,z:19})"},
        {"(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})","({a:{},b:{},c:{},x:19,y:19,z:19})"},
        {"(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})","({a:'shared text',b:'nested',c:'',x:19,y:19,z:19})"},
        {"(function(o){if(o.a??o.b)return o.x;if(!o.c)return o.y;return o.z})","({a:null,b:true,c:false,x:19,y:19,z:19})"},
        {"(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})","({a:false,b:true,c:true,x:19,y:19,z:19})"},
        {"(function(o){if(o.a){if(o.b)return o.x;return o.y}if(o.c)return o.z;return o.x})","({get a(){return true},b:true,c:true,x:19,y:19,z:19})"}
    };
    JSValue driver=eval(ctx,"(function(f,o,n){var r;for(var i=0;i<n;i++)r=f(o);return r})");
    unsigned family_count=sizeof(families)/sizeof(families[0]);
    unsigned measured_families=cfg_placement ? family_count : 2*family_count;
    unsigned lanes=cfg_placement ? 5 : 4;
    for(unsigned family=0;family<measured_families;family++) {
        unsigned kind=family%family_count;
        bool one_frame=family>=family_count;
        JSValue function=eval(ctx,one_frame ?
            "(function(o,n){var r;while(n--){if(o.a){if(o.b)r=o.x;else r=o.y}else if(o.c)r=o.z;else r=o.x}return r})" :
            families[kind].source),object=eval(ctx,families[kind].object);
        register_cfg=false;
        cfg_owned_stores=false;
        TFRegionPlan *direct=compile_plan(ctx,&budget,&heap,pool,function); CHECK(direct);
        register_cfg=true;
        TFRegionPlan *candidate=compile_plan(ctx,&budget,&heap,pool,function);
        CHECK(candidate && candidate->cfg_regions);
        cfg_owned_stores=true;
        TFRegionPlan *owned=compile_plan(ctx,&budget,&heap,pool,function);
        CHECK(owned && owned->cfg_regions && (!one_frame || owned->cfg_owned_stores));
        if(!one_frame) {
            NativeCodeLease read_map,owned_map;
            CHECK(native_code_pool_enter(pool,candidate->image,0,&read_map));
            CHECK(native_code_pool_enter(pool,owned->image,0,&owned_map));
            CHECK(!owned->cfg_owned_stores && owned->code_bytes == candidate->code_bytes &&
                !memcmp(read_map.entry,owned_map.entry,owned->code_bytes));
            CHECK(owned->body == candidate->body &&
                !memcmp(owned->entry,candidate->entry,
                    owned->body->byte_code_len*sizeof(*owned->entry)));
            if(cfg_placement)
                printf("region cfg-placement-address family=%u read-plan=%p owned-plan=%p read-image=%p owned-image=%p code=%u\n",
                    family,(void *)candidate,(void *)owned,read_map.entry,owned_map.entry,owned->code_bytes);
            CHECK(native_code_pool_leave(pool,read_map) && native_code_pool_leave(pool,owned_map));
            printf("region cfg-identical-code family=%u read-only-vs-owned=pass\n",family);
        }
        JSValue args[3]={one_frame ? object : function,
            one_frame ? JS_NewInt32(ctx,REGION_ITERATIONS) : object,JS_NewInt32(ctx,REGION_ITERATIONS)};
        NativeCodeHandle read_image=candidate->image,owned_image=owned->image;
        uint64_t samples[5][REGION_SAMPLES];
        for(unsigned repeat=0;repeat<=REGION_SAMPLES;repeat++) for(unsigned order=0;order<lanes;order++) {
            unsigned lane=(order+repeat)%lanes;
            TFRegionPlan *plan=cfg_placement ? (lane == 1 || lane == 2 ? candidate : owned) :
                lane == 1 ? direct : lane == 2 ? candidate : owned;
            /* Both plans and images remain rooted and published. Swap only
               while no frame lease is live, then restore before teardown.
               This is safe only after code AND exact entry maps compare equal. */
            CHECK(!candidate->active_frames && !owned->active_frames);
            NativeCodeHandle original_image=plan->image;
            if(cfg_placement && lane == 2) plan->image=owned_image;
            if(cfg_placement && lane == 3) plan->image=read_image;
            tf_region_plan=lane ? plan : NULL;
            tf_region_collect_metrics=repeat == 0;
            plan->calls=plan->completed=plan->guards=plan->quota_exits=plan->lease_acquisitions=0;
            ctx->interrupt_counter=JS_INTERRUPT_COUNTER_INIT;
            uint64_t begin=now_ns();
            JSValue result=JS_Call(ctx,one_frame ? function : driver,JS_UNDEFINED,one_frame ? 2 : 3,args);
            uint64_t elapsed=now_ns()-begin;
            plan->image=original_image;
            CHECK(JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 19);
            CHECK(!plan->active_frames && !plan->quota_exits);
            if(lane) {
                CHECK(plan->lease_acquisitions == (one_frame ? 1 : REGION_ITERATIONS));
                CHECK(repeat ? !plan->calls && !plan->completed : plan->completed > 0);
            }
            JS_FreeValue(ctx,result);
            if(repeat) samples[lane][repeat-1]=elapsed;
            if(repeat == 0)
                printf("region cfg-work family=%u lane=%u calls=%llu ops=%llu guards=%llu leases=%llu code=%u regions=%u owned-stores=%u\n",
                    family,lane,(unsigned long long)plan->calls,(unsigned long long)plan->completed,
                    (unsigned long long)plan->guards,(unsigned long long)plan->lease_acquisitions,
                    plan->code_bytes,plan->cfg_regions,plan->cfg_owned_stores);
        }
        if(cfg_placement)
            printf("region cfg-placement-timing family=%u iterations=%u metrics=off interpreter-ns=%llu read-plan-read-image-ns=%llu read-plan-owned-image-ns=%llu owned-plan-read-image-ns=%llu owned-plan-owned-image-ns=%llu\n",
                family,REGION_ITERATIONS,(unsigned long long)median(samples[0],REGION_SAMPLES),
                (unsigned long long)median(samples[1],REGION_SAMPLES),(unsigned long long)median(samples[2],REGION_SAMPLES),
                (unsigned long long)median(samples[3],REGION_SAMPLES),(unsigned long long)median(samples[4],REGION_SAMPLES));
        else
            printf("region cfg-timing family=%u iterations=%u metrics=off interpreter-ns=%llu direct-ns=%llu candidate-ns=%llu owned-ns=%llu\n",
                family,REGION_ITERATIONS,(unsigned long long)median(samples[0],REGION_SAMPLES),
                (unsigned long long)median(samples[1],REGION_SAMPLES),(unsigned long long)median(samples[2],REGION_SAMPLES),
                (unsigned long long)median(samples[3],REGION_SAMPLES));
        for(unsigned lane=0;lane<lanes;lane++)
            printf("region cfg-range family=%u lane=%u min-ns=%llu max-ns=%llu\n",family,lane,
                (unsigned long long)samples[lane][0],(unsigned long long)samples[lane][REGION_SAMPLES-1]);
        tf_region_plan=NULL;
        destroy_plan(ctx,&budget,&heap,direct); destroy_plan(ctx,&budget,&heap,candidate); destroy_plan(ctx,&budget,&heap,owned);
        JS_FreeValue(ctx,object); JS_FreeValue(ctx,function);
    }
    CHECK(native_code_pool_destroy(&pool) && !heap.reserved);
    JS_FreeValue(ctx,driver); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator) && !budget.current);
    cfg_owned_stores=selected_owned;
    tf_region_collect_metrics=selected_metrics;
}
#endif
/* Price batching against the existing direct tier and the interpreter in one
   linked image. Identical rooted functions, objects, callbacks and branch polls
   are reused; counters proving execution are disabled in measured calls. */
static void reference_throughput(bool measure)
{
    bool selected = reference_blocks, selected_values = continuation_values;
    bool selected_returns = native_returns, selected_probes = tf_reference_probes;
#ifdef __PSP__
    bool selected_borrowed = borrowed_fields, selected_borrowed_probes = borrowed_field_probes;
#endif
    continuation_values = true; native_returns = false;
    Budget budget; budget_init(&budget,16u*1024u*1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget); CHECK(allocator);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(),allocator); CHECK(rt);
    JSContext *ctx = JS_NewContext(rt); CHECK(ctx);
    ProbeHeap heap = {rt,8u*1024u*1024u,0,false}; JS_SetMemoryLimit(rt,heap.ceiling);
    NativeCodePool *pool = native_code_pool_create(&budget,(NativeCodeHeap){&heap,reserve,release},
        (NativeCodeBackend){NULL,code_page_size(),map_code,publish_code,unmap_code},NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool);
    static const struct { const char *source, *object; } families[] = {
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){if(o.enabled)r=f(o.value)}return r})","({enabled:true,value:19})"},
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){let v=o.child;if(v)r=f(v.value)}return r})","({child:{value:19}})"},
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){let v=o.label;if(v&&v===o.label)r=f(o.value)}return r})","({label:'shared text',value:19})"},
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){let v=o.value;let c=v??o.fallback;if(c)r=f(c)}return r})","({value:null,fallback:19})"},
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){let v=o.child;if(v.enabled)r=f(v.value)}return r})","({child:{enabled:true,value:19}})"},
        {"(function(o,f,n){let r;for(let i=0;i<n;i++){if(o.enabled)r=f(o.value)}return r})","({get enabled(){return true},get value(){return 19}})"}
    };
#ifdef __PSP__
    static const char *borrowed_sources[] = {
        NULL,
        "(function(o,f,n){let r;for(let i=0;i<n;i++){if(o.child)r=f(o.child.value)}return r})",
        "(function(o,f,n){let r;for(let i=0;i<n;i++){if(o.label)r=f(o.value)}return r})",
        NULL,
        "(function(o,f,n){let r;for(let i=0;i<n;i++){if(o.child.enabled)r=f(o.child.value)}return r})",
        NULL
    };
#endif
    unsigned iterations = measure ? REGION_ITERATIONS : 4;
    JSValue args[3] = {JS_UNDEFINED,eval(ctx,"(v=>v+1)"),JS_NewInt32(ctx,iterations)};
    for (unsigned family = 0; family < sizeof(families)/sizeof(families[0]); family++) {
        const char *source = families[family].source;
#ifdef __PSP__
        if (selected_borrowed && borrowed_sources[family]) source = borrowed_sources[family];
#endif
        JSValue function = eval(ctx,source);
        reference_blocks = false;
#ifdef __PSP__
        borrowed_fields = false;
#endif
        TFRegionPlan *direct = compile_plan(ctx,&budget,&heap,pool,function); CHECK(direct);
        reference_blocks = true;
#ifdef __PSP__
        if (selected_borrowed) { reference_blocks = false; borrowed_fields = true; }
#endif
        TFRegionPlan *batch = compile_plan(ctx,&budget,&heap,pool,function);
        CHECK(batch);
#ifdef __PSP__
        CHECK(selected_borrowed ? batch->borrowed_field_blocks || family == 3 : batch->reference_count);
#else
        CHECK(batch->reference_count);
#endif
        args[0] = eval(ctx,families[family].object);
        /* Prove an actual leased generated entry calls the batch helper. */
        tf_region_plan = batch; tf_reference_probes = true; tf_reference_hits = 0;
#ifdef __PSP__
        borrowed_field_probes = true; borrowed_field_hits = 0;
#endif
        JSValue result = JS_Call(ctx,function,JS_UNDEFINED,3,args);
        CHECK(!JS_IsException(result) && JS_VALUE_GET_INT(result) == 20);
        unsigned hits = tf_reference_hits;
#ifdef __PSP__
        if (selected_borrowed) hits = borrowed_field_hits;
        CHECK(hits > 0 || (selected_borrowed && (family == 3 || family == 5)));
        borrowed_field_probes = false;
#else
        CHECK(hits > 0);
#endif
        JS_FreeValue(ctx,result); tf_reference_probes = false;
        uint64_t samples[3][REGION_SAMPLES];
        for (unsigned repeat = 0; measure && repeat <= REGION_SAMPLES; repeat++) {
            for (unsigned order = 0; order < 3; order++) {
                unsigned lane = (order+repeat)%3;
                TFRegionPlan *plan = lane == 2 ? batch : lane == 1 ? direct : NULL;
                tf_region_plan = plan;
                if (plan) plan->lease_acquisitions = plan->completed = 0;
                uint64_t begin = now_ns();
                result = JS_Call(ctx,function,JS_UNDEFINED,3,args);
                uint64_t elapsed = now_ns()-begin;
                CHECK(!JS_IsException(result) && JS_VALUE_GET_INT(result) == 20);
#ifdef __PSP__
                CHECK((selected_borrowed ? borrowed_field_hits : tf_reference_hits) == hits);
#else
                CHECK(tf_reference_hits == hits); /* no counter in timed calls */
#endif
                if (plan) CHECK(plan->active_frames == 0 && plan->lease_acquisitions == 1);
                JS_FreeValue(ctx,result);
                if (repeat) samples[lane][repeat-1] = elapsed;
            }
        }
        unsigned blocks = batch->reference_count;
        const char *kind = "reference";
#ifdef __PSP__
        if (selected_borrowed) { blocks = batch->borrowed_field_blocks; kind = "borrowed"; }
#endif
        if (measure) printf("region reference-timing kind=%s family=%u iterations=%u blocks=%u proof-hits=%u direct-bytes=%u batch-bytes=%u metadata=%zu interpreter-ns=%llu direct-ns=%llu batch-ns=%llu\n",
            kind,family,REGION_ITERATIONS,blocks,hits,direct->code_bytes,batch->code_bytes,batch->reference_bytes,
            (unsigned long long)median(samples[0],REGION_SAMPLES),
            (unsigned long long)median(samples[1],REGION_SAMPLES),
            (unsigned long long)median(samples[2],REGION_SAMPLES));
        for (unsigned lane = 0; measure && lane < 3; lane++)
            printf("region reference-range family=%u lane=%u min-ns=%llu max-ns=%llu\n",family,lane,
                (unsigned long long)samples[lane][0],(unsigned long long)samples[lane][REGION_SAMPLES-1]);
        if (!measure) printf("region emitted reference batch=pass family=%u proof-hits=%u\n",family,hits);
        tf_region_plan = NULL;
        destroy_plan(ctx,&budget,&heap,direct); destroy_plan(ctx,&budget,&heap,batch);
        JS_FreeValue(ctx,args[0]); args[0] = JS_UNDEFINED; JS_FreeValue(ctx,function);
    }
    JS_FreeValue(ctx,args[1]);
    CHECK(native_code_pool_destroy(&pool) && heap.reserved == 0);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator) && budget.current == 0);
    reference_blocks = selected; continuation_values = selected_values;
    native_returns = selected_returns; tf_reference_probes = selected_probes;
#ifdef __PSP__
    borrowed_fields = selected_borrowed; borrowed_field_probes = selected_borrowed_probes;
#endif
}
#endif

/* Same linked image, function, inputs and polls; only predicate code generation
   differs between the two native plans. No per-op clocks or sampled profiler.
   This tests measured opcode patterns, not a substitute assistant response. */
static void predicate_throughput(bool measure)
{
    if (!measure) return;
    bool selected_values = continuation_values, selected_returns = native_returns;
#ifdef __PSP__
    bool selected_blocks = value_blocks;
    value_blocks = false;
    const unsigned lanes = 4;
#else
    const unsigned lanes = 3;
#endif
    native_returns = false;
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator != NULL);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    ProbeHeap heap = {rt, 8u * 1024u * 1024u, 0, false};
    JS_SetMemoryLimit(rt, heap.ceiling);
    NativeCodePool *pool = native_code_pool_create(&budget,
        (NativeCodeHeap){&heap, reserve, release},
        (NativeCodeBackend){NULL, code_page_size(), map_code, publish_code, unmap_code}, NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    static const struct { const char *source, *object; } families[] = {
        {"(function(object,callback,count){let result;for(let i=0;i<count;i++){let value=object.value;let valid=value===19;let blocked=!object.enabled;if(valid&&!blocked)result=callback(value)}return result})",
         "({enabled:true,value:19})"},
        {"(function(object,callback,count){let result;for(let i=0;i<count;i++){let value=object.value;let selected=value??19;let valid=selected===19;if(valid&&!!selected)result=callback(selected)}return result})",
         "({value:null})"},
        {"(function(object,callback,count){let result;for(let i=0;i<count;i++){let value=object.value;let fallback=void 0;let chosen=(value===null||value===fallback)?19:value;if(chosen!==0&&!object.blocked)result=callback(chosen)}return result})",
         "({value:undefined,blocked:false})"},
    };
    JSValue args[3] = {JS_UNDEFINED, eval(ctx, "(x=>x+1)"), JS_NewInt32(ctx, REGION_ITERATIONS)};
    for (unsigned family = 0; family < sizeof(families) / sizeof(families[0]); family++) {
        JSValue function = eval(ctx, families[family].source);
        continuation_values = false;
        TFRegionPlan *helper = compile_plan(ctx, &budget, &heap, pool, function);
        continuation_values = true;
        TFRegionPlan *direct = compile_plan(ctx, &budget, &heap, pool, function);
        CHECK(helper != NULL && direct != NULL);
#ifdef __PSP__
        CHECK(helper->semantic_direct_sites == 0 && direct->semantic_direct_sites > 0);
        value_blocks = true;
        TFRegionPlan *block = compile_plan(ctx, &budget, &heap, pool, function);
        value_blocks = false;
        CHECK(block != NULL && block->value_blocks > 0 && !value_block_probes);
#endif
        args[0] = eval(ctx, families[family].object);
        uint64_t samples[4][REGION_SAMPLES];
        for (unsigned repeat = 0; repeat <= REGION_SAMPLES; repeat++) {
            for (unsigned order = 0; order < lanes; order++) {
                unsigned lane = (order + repeat) % lanes;
                TFRegionPlan *plan = lane == 1 ? helper : direct;
#ifdef __PSP__
                if (lane == 3) plan = block;
#endif
                tf_region_plan = lane == 0 ? NULL : plan;
                plan->calls = plan->completed = plan->guards = plan->quota_exits = plan->lease_acquisitions = 0;
                uint64_t begin = now_ns();
                JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 3, args);
                uint64_t elapsed = now_ns() - begin;
                CHECK(!JS_IsException(result) && JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 20);
                CHECK(helper->active_frames == 0 && direct->active_frames == 0);
                if (lane) CHECK(plan->lease_acquisitions == 1);
                JS_FreeValue(ctx, result);
                if (repeat) samples[lane][repeat - 1] = elapsed;
                if (repeat == REGION_SAMPLES && lane)
                    printf("region predicate-work family=%u lane=%u calls=%llu completed=%llu guards=%llu quota=%llu\n",
                        family, lane, (unsigned long long)plan->calls, (unsigned long long)plan->completed,
                        (unsigned long long)plan->guards, (unsigned long long)plan->quota_exits);
            }
        }
        uint64_t medians[4];
        for (unsigned lane = 0; lane < lanes; lane++) medians[lane] = median(samples[lane], REGION_SAMPLES);
        printf("region predicate-timing family=%u iterations=%u helper-bytes=%u direct-bytes=%u interpreter-ns=%llu helper-ns=%llu direct-ns=%llu\n",
            family, REGION_ITERATIONS, helper->code_bytes, direct->code_bytes,
            (unsigned long long)medians[0], (unsigned long long)medians[1], (unsigned long long)medians[2]);
#ifdef __PSP__
        printf("region value-block-timing family=%u sites=%u block-bytes=%u interpreter-ns=%llu direct-ns=%llu block-ns=%llu\n",
            family,block->value_blocks,block->code_bytes,(unsigned long long)medians[0],
            (unsigned long long)medians[2],(unsigned long long)medians[3]);
#endif
        for (unsigned lane = 0; lane < lanes; lane++)
            printf("region predicate-range family=%u lane=%u min-ns=%llu max-ns=%llu\n",
                family, lane, (unsigned long long)samples[lane][0], (unsigned long long)samples[lane][REGION_SAMPLES - 1]);
        tf_region_plan = NULL;
        destroy_plan(ctx, &budget, &heap, helper);
        destroy_plan(ctx, &budget, &heap, direct);
#ifdef __PSP__
        destroy_plan(ctx, &budget, &heap, block);
#endif
        JS_FreeValue(ctx, function);
        JS_FreeValue(ctx, args[0]);
    }
    JS_FreeValue(ctx, args[1]);
    CHECK(native_code_pool_destroy(&pool) && heap.reserved == 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator) && budget.current == 0);
    continuation_values = selected_values;
    native_returns = selected_returns;
#ifdef __PSP__
    value_blocks = selected_blocks;
#endif
}

static void nested_return_probe(bool measure)
{
    bool selected_returns = native_returns;
    native_returns = true; /* identical semantic setup in every timed lane */
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator != NULL);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt != NULL);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx != NULL);
    ProbeHeap heap = {rt, 8u * 1024u * 1024u, 0, false};
    JS_SetMemoryLimit(rt, heap.ceiling);
    NativeCodePool *pool = native_code_pool_create(&budget,
        (NativeCodeHeap){&heap, reserve, release},
        (NativeCodeBackend){NULL, code_page_size(), map_code, publish_code, unmap_code}, NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool != NULL);
    JSValue parent = eval(ctx, "(function(callback,value){return callback(value)})");
    JSValue child = eval(ctx, "(function(value){return value})");
    TFRegionPlan *caller = compile_plan(ctx, &budget, &heap, pool, parent);
    TFRegionPlan *callee = compile_plan(ctx, &budget, &heap, pool, child);
    CHECK(caller != NULL && callee != NULL);
    tf_region_plan = caller;
    tf_region_callee_plan = callee;
    static const char *values[] = {"19", "undefined", "'owned-string'", "({x:17})", "(()=>23)"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        JSValue args[2] = {child, eval(ctx, values[i])};
        JS_RunGC(rt);
        for (unsigned lane = 0; lane < 6; lane++) {
            caller->enabled = lane != 0;
            callee->enabled = lane >= 2;
            callee->admission_only = lane == 2 || lane == 4;
            callee->pointer_leases = lane >= 4;
            uint64_t before = callee->returns;
            uint64_t leases = callee->lease_acquisitions, calls = callee->calls;
            JSValue result = JS_Call(ctx, parent, JS_UNDEFINED, 2, args);
            CHECK(!JS_IsException(result) && js_strict_eq(ctx, result, args[1]));
            CHECK(caller->active_frames == 0 && callee->active_frames == 0);
            CHECK(callee->returns - before == (lane == 3 || lane == 5));
            CHECK(callee->lease_acquisitions - leases == (lane >= 2));
            if (callee->admission_only) CHECK(callee->calls == calls);
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, args[1]);
    }
    /* Guard refusal leaves the result stack untouched for ordinary dispatch. */
    JSValue stack[1] = {JS_NewInt32(ctx, 7)};
    TFRegionFrame frame = {.body = callee->body, .base = stack, .limit = stack + 1,
        .sp = stack + 1, .completed = TF_REGION_QUANTUM, .ctx = ctx};
    CHECK(tf_region_return(&frame, OP_return, callee->body->byte_code_len) == 0);
    CHECK(!frame.returned && frame.sp == stack + 1 && frame.quota_exit);
    frame.completed = 0;
    frame.sp = stack;
    CHECK(tf_region_return(&frame, OP_return, callee->body->byte_code_len) == 0);
    CHECK(!frame.returned && frame.sp == stack);
    CHECK(tf_region_return(&frame, OP_return_undef, callee->body->byte_code_len) == 0);
    CHECK(frame.returned && JS_IsUndefined(frame.result));
    puts("region nested-return identity, ownership, terminal entry, admission-only and quota=pass");

    tf_region_plan = tf_region_callee_plan = NULL;
    destroy_plan(ctx, &budget, &heap, caller);
    JS_FreeValue(ctx, parent);
    parent = eval(ctx, "(function(callback,value,count){let result;for(let i=0;i<count;i++)result=callback(value);return result})");
    caller = compile_plan(ctx, &budget, &heap, pool, parent);
    CHECK(caller != NULL);
    tf_region_plan = caller;
    tf_region_callee_plan = callee;
    JSValue args[3] = {child, JS_NewInt32(ctx, 19), JS_NewInt32(ctx, REGION_ITERATIONS)};
    uint64_t samples[6][REGION_SAMPLES];
    /* Same result, but separate lease overhead (0), repeated helper-backed
       assignment (1), and direct integer work without that helper (2). */
    for (unsigned family = 0; measure && family < 3; family++) {
        if (family) {
            tf_region_callee_plan = NULL;
            destroy_plan(ctx, &budget, &heap, callee);
            JS_FreeValue(ctx, child);
            child = eval(ctx, family == 1 ?
                "(function(value){let result=value;for(let i=0;i<16;i++){if(value)result=value}return result})" :
                "(function(value){for(var i=0;i<16;i++){}return value})");
            callee = compile_plan(ctx, &budget, &heap, pool, child);
            CHECK(callee != NULL);
            tf_region_callee_plan = callee;
            args[0] = child;
        }
        for (unsigned repeat = 0; repeat <= REGION_SAMPLES; repeat++) {
            for (unsigned order = 0; order < 6; order++) {
                unsigned lane = (order + repeat) % 6;
                caller->enabled = lane != 0;
                callee->enabled = lane >= 2;
                callee->admission_only = lane == 2 || lane == 4;
                callee->pointer_leases = lane >= 4;
                caller->returns = callee->returns = 0;
                callee->calls = callee->completed = callee->lease_acquisitions = 0;
                uint64_t begin = now_ns();
                JSValue result = JS_Call(ctx, parent, JS_UNDEFINED, 3, args);
                uint64_t elapsed = now_ns() - begin;
                CHECK(!JS_IsException(result) && JS_VALUE_GET_TAG(result) == JS_TAG_INT && JS_VALUE_GET_INT(result) == 19);
                CHECK(caller->returns == (lane != 0));
                CHECK(callee->returns == (lane == 3 || lane == 5 ? REGION_ITERATIONS : 0));
                CHECK(callee->lease_acquisitions == (lane >= 2 ? REGION_ITERATIONS : 0));
                if (callee->admission_only) CHECK(callee->calls == 0 && callee->completed == 0);
                CHECK(caller->active_frames == 0 && callee->active_frames == 0);
                JS_FreeValue(ctx, result);
                if (repeat) samples[lane][repeat - 1] = elapsed;
            }
        }
        uint64_t medians[6];
        for (unsigned lane = 0; lane < 6; lane++) medians[lane] = median(samples[lane], REGION_SAMPLES);
        printf("region nested-call family=%u iterations=%u caller-bytes=%u callee-bytes=%u interpreter-ns=%llu caller-only-ns=%llu admission-only-ns=%llu caller-callee-ns=%llu pointer-admission-ns=%llu pointer-native-ns=%llu\n",
            family, REGION_ITERATIONS, caller->code_bytes, callee->code_bytes,
            (unsigned long long)medians[0], (unsigned long long)medians[1],
            (unsigned long long)medians[2], (unsigned long long)medians[3],
            (unsigned long long)medians[4], (unsigned long long)medians[5]);
        for (unsigned lane = 0; lane < 6; lane++)
            printf("region nested-range family=%u lane=%u min-ns=%llu max-ns=%llu\n", family, lane,
                (unsigned long long)samples[lane][0], (unsigned long long)samples[lane][REGION_SAMPLES - 1]);
    }
    tf_region_plan = tf_region_callee_plan = NULL;
    destroy_plan(ctx, &budget, &heap, caller);
    destroy_plan(ctx, &budget, &heap, callee);
    CHECK(native_code_pool_destroy(&pool) && heap.reserved == 0);
    JS_FreeValue(ctx, parent);
    JS_FreeValue(ctx, child);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator) && budget.current == 0);
    native_returns = selected_returns;
}
#endif
static JSValue retire_call_plan(JSContext *ctx, JSValueConst receiver,
                               int argc, JSValueConst *argv)
{
    (void)receiver; (void)argc; (void)argv;
    CHECK(tf_region_plan && tf_region_plan->active_frames == PROBE_FRAME_LEASE);
    tf_region_plan->retired = true;
    tf_region_plan->enabled = false;
    CHECK(native_code_pool_retire(tf_region_plan->pool, tf_region_plan->image));
    return JS_NewInt32(ctx, 19);
}

static void call_continuation_probe(bool measure)
{
    if (!method_calls) return;
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    BudgetQuickJSPool *allocator = budget_quickjs_pool_create(&budget);
    CHECK(allocator);
    JSRuntime *rt = JS_NewRuntime2(budget_quickjs_pool_allocator(), allocator);
    CHECK(rt);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx);
    ProbeHeap heap = {rt, 8u * 1024u * 1024u, 0, false};
    JS_SetMemoryLimit(rt, heap.ceiling);
    NativeCodePool *pool = native_code_pool_create(&budget,
        (NativeCodeHeap){&heap, reserve, release},
        (NativeCodeBackend){NULL, code_page_size(), map_code, publish_code, unmap_code}, NATIVE_POOL_BYTE_LIMIT);
    CHECK(pool);
    static const struct { const char *source, *argument; bool throws; } checks[] = {
        {"(function(o){let value=o.method(1,2,3,4);return value})",
         "({base:9,method:function(a,b,c,d){return this.base+a+b+c+d}})",false},
        {"(function(f){let value=f(1,2,3,4);return value})", "(function(){return 19})",false},
        {"(function(o){let value=o.method(-19);return value})", "({method:Math.abs})",false},
        {"(function(o){let value=o.method();return value})",
         "({method:(function(){return this.value}).bind({value:19})})",false},
        {"(function(o){let value=o.method();return value})",
         "({value:19,method:new Proxy(function(){},{apply:function(f,receiver){return receiver.value}})})",false},
        {"(function(o){let value=o.method();return value})", "({method:function(){throw new RangeError('call-probe')}})",true},
        {"(function(){let value=0;let callback=function(){value=19};callback();return value})", "undefined",false},
    };
    unsigned admitted = 0;
    for (unsigned c=0;c<sizeof(checks)/sizeof(checks[0]);c++) {
        JSValue function = eval(ctx, checks[c].source);
        JSValue argument = eval(ctx, checks[c].argument);
        TFRegionPlan *plan = compile_plan(ctx,&budget,&heap,pool,function);
        CHECK(plan);
        tf_region_plan = plan;
        for (unsigned lane=0;lane<2;lane++) {
            plan->enabled = lane != 0;
            plan->call_continuations = 0;
            JSValue result = JS_Call(ctx,function,JS_UNDEFINED,1,&argument);
            if (checks[c].throws) {
                CHECK(JS_IsException(result));
                JSValue exception = JS_GetException(ctx);
                JSValue message = JS_GetPropertyStr(ctx,exception,"message");
                const char *text = JS_ToCString(ctx,message);
                CHECK(text && !strcmp(text,"call-probe"));
                JS_FreeCString(ctx,text); JS_FreeValue(ctx,message); JS_FreeValue(ctx,exception);
            } else CHECK(JS_VALUE_GET_TAG(result)==JS_TAG_INT && JS_VALUE_GET_INT(result)==19);
            CHECK(plan->active_frames == 0);
            printf("region method-call-case=%u lane=%u continuations=%llu\n",c,lane,
                (unsigned long long)plan->call_continuations);
            CHECK(plan->call_continuations == (lane ? 1u : 0u));
            admitted += lane;
            JS_FreeValue(ctx,result);
            JSValue unrelated = eval(ctx,"21+21");
            CHECK(JS_VALUE_GET_INT(unrelated)==42); JS_FreeValue(ctx,unrelated);
        }
        tf_region_plan = NULL;
        destroy_plan(ctx,&budget,&heap,plan);
        JS_FreeValue(ctx,argument); JS_FreeValue(ctx,function);
    }
    JSValue function = eval(ctx,"(function(o){let value=o.method();return value})");
    JSValue object = JS_NewObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,object,"method",JS_NewCFunction(ctx,retire_call_plan,"retire",0)) >= 0);
    TFRegionPlan *plan = compile_plan(ctx,&budget,&heap,pool,function);
    CHECK(plan); tf_region_plan = plan;
    JSValue result = JS_Call(ctx,function,JS_UNDEFINED,1,&object);
    CHECK(JS_VALUE_GET_TAG(result)==JS_TAG_INT && JS_VALUE_GET_INT(result)==19);
    CHECK(plan->retired && !plan->enabled && !plan->active_frames && plan->call_continuations==1);
    JS_FreeValue(ctx,result);
    tf_region_plan = NULL;
    destroy_plan(ctx,&budget,&heap,plan);
    JS_FreeValue(ctx,object); JS_FreeValue(ctx,function);

    /* A saturated quantum or malformed operand depth must not poll/call. */
    JSValue stack[2] = {JS_UNDEFINED,JS_UNDEFINED};
    TFRegionFrame frame = {.ctx=ctx,.base=stack,.limit=stack+2,.sp=stack+2,.completed=TF_REGION_QUANTUM};
    CHECK(!tf_region_call_method(&frame,0,0) && frame.quota_exit && frame.sp==stack+2);
    frame.completed=0; frame.quota_exit=false;
    CHECK(!tf_region_call_method(&frame,1,0) && !frame.quota_exit && frame.sp==stack+2);
    CHECK(!tf_region_call(&frame,65,0) && frame.sp==stack+2);
    printf("region method-call receiver, native, bound/proxy, throw, re-entry, retirement and quota=pass admitted=%u\n",admitted);

    static const char *arguments[] = {"({value:19,method:function(v){return v}})","({value:-19,method:Math.abs})"};
    function = eval(ctx,"(function(o,n){var result;for(var i=0;i<n;i++)result=o.method(o.value);return result})");
    method_calls=false;
    TFRegionPlan *control=compile_plan(ctx,&budget,&heap,pool,function);
    method_calls=true;
    TFRegionPlan *candidate=compile_plan(ctx,&budget,&heap,pool,function);
    CHECK(control && candidate);
    for (unsigned family=0;measure && family<2;family++) {
        JSValue args[2]={eval(ctx,arguments[family]),JS_NewInt32(ctx,REGION_ITERATIONS)};
        uint64_t samples[3][REGION_SAMPLES];
        for(unsigned repeat=0;repeat<=REGION_SAMPLES;repeat++) for(unsigned order=0;order<3;order++) {
            unsigned lane=(repeat+order)%3;
            plan=lane==1 ? control : candidate;
            plan->enabled=lane!=0; plan->call_continuations=0; plan->calls=0;
            tf_region_plan=plan; tf_region_collect_metrics=repeat==0;
            uint64_t begin=now_ns();
            result=JS_Call(ctx,function,JS_UNDEFINED,2,args);
            uint64_t elapsed=now_ns()-begin;
            CHECK(JS_VALUE_GET_TAG(result)==JS_TAG_INT && JS_VALUE_GET_INT(result)==19 && !plan->active_frames);
            if(!repeat) CHECK(plan->call_continuations==(lane==2 ? REGION_ITERATIONS : 0u));
            JS_FreeValue(ctx,result);
            if(repeat) samples[lane][repeat-1]=elapsed;
        }
        printf("region method-call-timing family=%u metrics=off iterations=%u interpreter-ns=%llu fallback-ns=%llu continuation-ns=%llu control-bytes=%u candidate-bytes=%u\n",
            family,REGION_ITERATIONS,(unsigned long long)median(samples[0],REGION_SAMPLES),
            (unsigned long long)median(samples[1],REGION_SAMPLES),(unsigned long long)median(samples[2],REGION_SAMPLES),control->code_bytes,candidate->code_bytes);
        JS_FreeValue(ctx,args[0]);
    }
    tf_region_collect_metrics=true; tf_region_plan=NULL;
    destroy_plan(ctx,&budget,&heap,control); destroy_plan(ctx,&budget,&heap,candidate);
    JS_FreeValue(ctx,function);
    CHECK(native_code_pool_destroy(&pool) && heap.reserved == 0);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    CHECK(budget_quickjs_pool_destroy(allocator) && budget.current == 0);
}

int main(int argc, char **argv)
{
    block_census_model_tests();
#ifdef __PSP__
    if (!freopen("host0:/native-region-probe.txt", "w", stdout)) return 16;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (scePowerSetClockFrequency(333, 333, 166) < 0) return 17;
    printf("region platform=psp cpu-mhz=%d\n", scePowerGetCpuClockFrequencyInt());
    /* The first device pass is semantic qualification. Timings are enabled
       only after every actual-frame and emitted-guard check completes. */
#endif
    bool measure = true, cfg_selected = false;
    bool describe_admission = false;
#ifdef TF_REGION_FRAME_LEASE
    direct_emission = chained_emission = value_abi = direct_branches = true;
    printf("region retained-frame-bytes=%zu\n", sizeof(TFRegionActivation));
#endif
    CHECK(argc <= 4);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--semantics-only")) measure = false;
        else if (!strcmp(argv[i], "--direct")) direct_emission = true;
        else if (!strcmp(argv[i], "--chain")) direct_emission = chained_emission = true;
        else if (!strcmp(argv[i], "--value-abi")) direct_emission = chained_emission = value_abi = true;
        else if (!strcmp(argv[i], "--frame-abi")) value_abi = false;
#ifdef __PSP__
        else if (!strcmp(argv[i], "--cached-bindings")) cached_bindings = true;
        else if (!strcmp(argv[i], "--fused-fields")) fused_fields = true;
        else if (!strcmp(argv[i], "--basic-blocks")) basic_blocks = true;
        else if (!strcmp(argv[i], "--cold-blocks")) basic_blocks = cold_blocks = true;
        else if (!strcmp(argv[i], "--value-blocks")) value_blocks = continuation_values = true;
        else if (!strcmp(argv[i], "--borrowed-fields")) borrowed_fields = continuation_values = true;
        else if (!strcmp(argv[i], "--register-cfg")) cfg_selected = register_cfg = continuation_values = true;
        else if (!strcmp(argv[i], "--owned-cfg")) cfg_selected = cfg_owned_stores = register_cfg = continuation_values = true;
        else if (!strcmp(argv[i], "--cfg-placement")) cfg_placement = cfg_selected = cfg_owned_stores = register_cfg = continuation_values = true;
#endif
        else if (!strcmp(argv[i], "--branch-abi")) direct_emission = chained_emission = value_abi = direct_branches = true;
        else if (!strcmp(argv[i], "--sample-own")) { profile_native = true; measure = false; }
        else if (!strcmp(argv[i], "--sample-getter")) { profile_native = true; profile_property = 2; measure = false; }
        else if (!strcmp(argv[i], "--describe-admission")) describe_admission = true;
        else if (!strcmp(argv[i], "--mixed")) mixed_workload = true;
        else if (!strcmp(argv[i], "--nested-only")) nested_only = true;
        else if (!strcmp(argv[i], "--method-calls")) method_calls = direct_emission = chained_emission = true;
        else if (!strcmp(argv[i], "--predicates-only")) predicates_only = true;
#if defined(TF_REGION_FRAME_LEASE) && !defined(TF_REGION_INTERPRETER_ONLY)
        else if (!strcmp(argv[i], "--native-returns")) native_returns = true;
#endif
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
        else if (!strcmp(argv[i], "--reference-only")) reference_only = true;
        else if (!strcmp(argv[i], "--continuation-values")) continuation_values = true;
        else if (!strcmp(argv[i], "--reference-blocks")) reference_blocks = continuation_values = true;
#endif
        else CHECK(false);
    }
    if (describe_admission) {
        /* Export this compiled decoder's actual opcode surface for offline
           census accounting. Zero operands test support, not runtime guards,
           function eligibility or successful code-size admission. */
        printf("region admission-limit bytecode=%u plan-bytes=%zu code-limit=%u\n",
            TF_REGION_BYTECODE_LIMIT, tf_region_plan_bytes(TF_REGION_BYTECODE_LIMIT, chained_emission), TF_REGION_CODE_LIMIT);
        for (unsigned opcode = 0; opcode < 256; opcode++) {
            if (!tf_region_opcodes[opcode].size) continue;
            uint8_t instruction[16] = {0};
            uint32_t argument;
            CHECK(tf_region_opcodes[opcode].size <= sizeof(instruction));
            instruction[0] = opcode;
            TFRegionHelper helper = tf_region_decode(instruction, &argument);
            const char *kind = !helper ? "unsupported" :
                helper == tf_region_field ? "property" :
                helper == tf_region_branch ? "branch" :
                (helper == tf_region_call || helper == tf_region_call_method) ? "call" : "value";
            printf("region admission opcode=%u name=%s kind=%s\n",
                opcode, tf_region_opcodes[opcode].name, kind);
        }
        return 0;
    }
    plan_storage_tests();
    for (unsigned test = 0; test < sizeof(cases) / sizeof(cases[0]); test++)
        for (unsigned mode = 0; mode < 3; mode++)
            if (mode != 1 || PROBE_LANES == 2) run_case(&cases[test], mode);
    const Case quota = {"long native loop yields and resumes", "(function(){let i=0;while(i<20000)i++;return i})", "20000"};
    describe_instructions = true;
    for (unsigned mode = 0; mode < 3; mode++) {
        if (mode == 1 && PROBE_LANES != 2) continue;
        CaseEvents events = run_case(&quota, mode);
        CHECK(!events.exceptions);
        if (mode == 1 && chained_emission) CHECK(events.quotas > 0);
        else CHECK(!events.quotas);
    }
    puts("region actual-frame semantics and Budget baseline=pass");
    call_continuation_probe(measure);
    throughput(measure && !method_calls && !nested_only && !predicates_only && !reference_only && !cfg_selected);
#if defined(TF_REGION_FRAME_LEASE) && !defined(TF_REGION_INTERPRETER_ONLY)
    nested_return_probe(measure && !method_calls && !predicates_only && !reference_only && !cfg_selected);
    predicate_throughput(measure && predicates_only);
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
#ifdef __PSP__
    cfg_throughput(measure);
#endif
    if (reference_blocks || reference_only
#ifdef __PSP__
        || borrowed_fields
#endif
    ) reference_throughput(measure && reference_only);
#endif
#endif
    puts("region outcome=pass");
    return 0;
}
#endif /* TF_REGION_LIVE_ENGINE */

#ifdef TF_REGION_LIVE_ENGINE
#ifndef CONFIG_TILEFINCH_CALL_COUNTS
#error The live selector needs saturating per-bytecode heat accounting
#endif

/* A bounded multi-body integration gate, not a production cache. The pointers
   are thread-local only to find the Budget-owned per-runtime state; the
   registry and every executable page belong to one runtime and are released
   by the generated runtime destructor hook. */
#define TF_LIVE_RUNTIME_LIMIT 8u
#define TF_LIVE_PLAN_LIMIT 16u
#define TF_LIVE_ATTEMPT_LIMIT 32u
#define TF_LIVE_WARM_CALLS 256u
typedef struct {
    JSRuntime *runtime;
    Budget *budget;
    ProbeHeap heap;
    NativeCodePool *pool;
    TFRegionPlan *plans[TF_LIVE_PLAN_LIMIT];
    unsigned plan_count;
    unsigned attempts;
} TFRegionLiveState;
static TF_REGION_THREAD_LOCAL TFRegionLiveState *tf_live_states[TF_LIVE_RUNTIME_LIMIT];

static TFRegionLiveState *tf_region_live_state(JSRuntime *rt)
{
    for (unsigned i = 0; i < TF_LIVE_RUNTIME_LIMIT; i++)
        if (tf_live_states[i] && tf_live_states[i]->runtime == rt)
            return tf_live_states[i];
    return NULL;
}

void tilefinch_native_tier_snapshot(JSRuntime *runtime,
                                    uint64_t *epoch, uint64_t *calls,
                                    uint64_t *completed)
{
    TFRegionLiveState *state = tf_region_live_state(runtime);
    *epoch = state ? (uint64_t)(uintptr_t)state : 0;
    *calls = *completed = 0;
    if (state) for (unsigned i = 0; i < state->plan_count; i++) {
        *calls += state->plans[i]->calls;
        *completed += state->plans[i]->completed;
    }
}

uint64_t tilefinch_native_tier_lazy_compile_ns(void)
{
    return tf_region_lazy_compile_ns;
}

void tilefinch_native_tier_job_begin(void)
{
    static bool checked;
    if (!checked) {
        tf_region_job_census_enabled = getenv("TILEFINCH_NATIVE_TIER_JOB_CENSUS") != NULL;
        tf_region_call_probe_enabled = getenv("TILEFINCH_NATIVE_TIER_CALL_PROBE") != NULL;
        checked = true;
    }
    if (tf_region_call_probe_enabled) {
        tf_region_call_probe_counter = 0;
        tf_region_call_probe_samples = 0;
        tf_region_call_probe_ns = 0;
        tf_region_call_probe_clock_ns = 0;
    }
    if (!tf_region_job_census_enabled) return;
    tf_region_job_epoch++;
    if (tf_region_job_epoch == 0) tf_region_job_epoch = 1;
    memset(tf_region_job_opcode_counts, 0, sizeof(tf_region_job_opcode_counts));
    memset(tf_region_job_pair_counts, 0, sizeof(tf_region_job_pair_counts));
    memset(tf_region_job_adjacent_counts, 0,
           sizeof(tf_region_job_adjacent_counts));
    tf_region_job_previous_body = NULL;
    tf_region_job_previous_opcode = -1;
    memset(tf_region_job_value_blocks, 0, sizeof(tf_region_job_value_blocks));
    memset(tf_region_job_branch_tags, 0, sizeof(tf_region_job_branch_tags));
    memset(&tf_primitive_job, 0, sizeof(tf_primitive_job));
    memset(&tf_reference_job, 0, sizeof(tf_reference_job));
    memset(&tf_borrowed_job, 0, sizeof(tf_borrowed_job));
    memset(&tf_primitive_scan_cache, 0, sizeof(tf_primitive_scan_cache));
}

void tilefinch_native_tier_job_report(JSRuntime *runtime, uint64_t wall_us)
{
    if (tf_region_call_probe_enabled && wall_us >= 50000u)
        fprintf(stderr,
            "tf-job-call-entry wall-us=%llu samples=%llu sampled-ns=%llu clock-ns=%llu estimated-net-us=%llu\n",
            (unsigned long long)wall_us,
            (unsigned long long)tf_region_call_probe_samples,
            (unsigned long long)tf_region_call_probe_ns,
            (unsigned long long)tf_region_call_probe_clock_ns,
            (unsigned long long)((tf_region_call_probe_ns >
                tf_region_call_probe_clock_ns
                    ? tf_region_call_probe_ns - tf_region_call_probe_clock_ns
                    : 0u) * 64u / 1000u));
    if (!tf_region_job_census_enabled || wall_us < 50000u) return;
    fprintf(stderr, "tf-job-primitive-block total=%llu candidates=%llu completed=%llu accepted=%llu accepted-ops=%llu refused-ops=%llu abandoned=%llu scans=%llu guards=",
        (unsigned long long)tf_primitive_job.total,
        (unsigned long long)tf_primitive_job.candidates,
        (unsigned long long)tf_primitive_job.completed,
        (unsigned long long)tf_primitive_job.accepted,
        (unsigned long long)tf_primitive_job.accepted_ops,
        (unsigned long long)tf_primitive_job.refused_ops,
        (unsigned long long)(tf_primitive_job.abandoned + (tf_primitive_job.next != 0)),
        (unsigned long long)tf_primitive_job.scans);
    for (unsigned i = 0; i < TF_PRIMITIVE_GUARD_COUNT; i++)
        fprintf(stderr, "%s%llu", i ? "," : "", (unsigned long long)tf_primitive_job.guards[i]);
    fprintf(stderr, " refusal-types=");
    for (unsigned i = 0; i < 7; i++)
        fprintf(stderr, "%s%llu", i ? "," : "", (unsigned long long)tf_primitive_job.types[i]);
    fprintf(stderr, " histogram=");
    for (unsigned i = 2; i <= TF_PRIMITIVE_BLOCK_LIMIT; i++)
        if (tf_primitive_job.histogram[i]) fprintf(stderr, "%u:%llu,", i,
            (unsigned long long)tf_primitive_job.histogram[i]);
    fprintf(stderr, "\n");
    fprintf(stderr,"tf-job-borrowed-field total=%llu candidates=%llu completed=%llu accepted=%llu accepted-ops=%llu refused-ops=%llu abandoned=%llu verified=%llu mismatches=%llu fields=%llu\n",
        (unsigned long long)tf_primitive_job.total,(unsigned long long)tf_borrowed_job.candidates,
        (unsigned long long)tf_borrowed_job.completed,(unsigned long long)tf_borrowed_job.accepted,
        (unsigned long long)tf_borrowed_job.accepted_ops,(unsigned long long)tf_borrowed_job.refused_ops,
        (unsigned long long)(tf_borrowed_job.abandoned+(tf_borrowed_job.next != 0)),
        (unsigned long long)tf_borrowed_job.verified,(unsigned long long)tf_borrowed_job.mismatches,
        (unsigned long long)tf_borrowed_job.fields);
    fprintf(stderr, "tf-job-reference-block total=%llu candidates=%llu completed=%llu accepted=%llu accepted-ops=%llu refused-ops=%llu abandoned=%llu verified=%llu mismatches=%llu fields=%llu inherited=%llu guards=",
        (unsigned long long)tf_primitive_job.total,
        (unsigned long long)tf_reference_job.candidates,
        (unsigned long long)tf_reference_job.completed,
        (unsigned long long)tf_reference_job.accepted,
        (unsigned long long)tf_reference_job.accepted_ops,
        (unsigned long long)tf_reference_job.refused_ops,
        (unsigned long long)(tf_reference_job.abandoned + (tf_reference_job.next != 0)),
        (unsigned long long)tf_reference_job.verified,
        (unsigned long long)tf_reference_job.mismatches,
        (unsigned long long)tf_reference_job.fields,
        (unsigned long long)tf_reference_job.inherited_fields);
    for (unsigned i = 0; i < TF_REFERENCE_GUARD_COUNT; i++)
        fprintf(stderr, "%s%llu", i ? "," : "", (unsigned long long)tf_reference_job.guards[i]);
    fprintf(stderr, " histogram=");
    for (unsigned i = 2; i <= TF_PRIMITIVE_BLOCK_LIMIT; i++)
        if (tf_reference_job.histogram[i]) fprintf(stderr, "%u:%llu,", i,
            (unsigned long long)tf_reference_job.histogram[i]);
    fprintf(stderr, "\n");
    static const char *frontier_names[TF_REFERENCE_FRONTIER_COUNT] = {
        "conditional","binding-write","call","property-write","return",
        "count-limit","stack-limit","other"};
    uint64_t frontier_regions = 0, frontier_instructions = 0;
    for (unsigned i = 0; i < TF_REFERENCE_FRONTIER_COUNT; i++) {
        frontier_regions += tf_reference_job.frontiers[i].regions;
        frontier_instructions += tf_reference_job.frontiers[i].instructions;
        fprintf(stderr,"tf-job-reference-frontier kind=%s regions=%llu instructions=%llu acquires=%llu releases=%llu net-headers=%llu balanced-reference-regions=%llu incoming=%llu outgoing=%llu reference-outgoing=%llu\n",
            frontier_names[i],
            (unsigned long long)tf_reference_job.frontiers[i].regions,
            (unsigned long long)tf_reference_job.frontiers[i].instructions,
            (unsigned long long)tf_reference_job.frontiers[i].acquires,
            (unsigned long long)tf_reference_job.frontiers[i].releases,
            (unsigned long long)tf_reference_job.frontiers[i].net_headers,
            (unsigned long long)tf_reference_job.frontiers[i].balanced_reference_regions,
            (unsigned long long)tf_reference_job.frontiers[i].incoming,
            (unsigned long long)tf_reference_job.frontiers[i].outgoing,
            (unsigned long long)tf_reference_job.frontiers[i].reference_outgoing);
    }
    fprintf(stderr,"tf-job-reference-accounting consistent=%u regions=%llu instructions=%llu\n",
        frontier_regions == tf_reference_job.accepted &&
            frontier_instructions == tf_reference_job.accepted_ops,
        (unsigned long long)frontier_regions,(unsigned long long)frontier_instructions);
    for (unsigned op = 0; op < 256; op++) if (tf_reference_job.frontier_opcodes[op])
        fprintf(stderr,"tf-job-reference-frontier-op opcode=%s regions=%llu\n",
            tf_region_opcodes[op].name,
            (unsigned long long)tf_reference_job.frontier_opcodes[op]);
    for (unsigned model = 0; model < 4; model++) {
    TFValueBlockCensus *blocks = &tf_region_job_value_blocks[model];
    tf_value_block_finish(blocks);
    fprintf(stderr, "tf-job-value-block model=%u total=%llu eligible=%llu excluded=%llu runs=%llu max-inputs=%u max-capacity=%u histogram=", model,
        (unsigned long long)blocks->total, (unsigned long long)blocks->eligible,
        (unsigned long long)blocks->excluded, (unsigned long long)blocks->runs,
        blocks->maximum_inputs, blocks->maximum_capacity);
    for (unsigned i = 1; i <= TF_VALUE_BLOCK_LIMIT; i++)
        if (blocks->histogram[i]) fprintf(stderr, "%u:%llu,", i,
            (unsigned long long)blocks->histogram[i]);
    fprintf(stderr, " kinds=");
    for (unsigned i = 1; i < 64; i++)
        if (blocks->kinds[i]) fprintf(stderr, "%u:%llu,", i,
            (unsigned long long)blocks->kinds[i]);
    fprintf(stderr, "\n");
    }
    fprintf(stderr, "tf-job-branch-tags int=%llu bool=%llu nullish=%llu string=%llu object=%llu bigint=%llu float=%llu other=%llu\n",
        (unsigned long long)tf_region_job_branch_tags[0],
        (unsigned long long)tf_region_job_branch_tags[1],
        (unsigned long long)tf_region_job_branch_tags[2],
        (unsigned long long)tf_region_job_branch_tags[3],
        (unsigned long long)tf_region_job_branch_tags[4],
        (unsigned long long)tf_region_job_branch_tags[5],
        (unsigned long long)tf_region_job_branch_tags[6],
        (unsigned long long)tf_region_job_branch_tags[7]);
    JSFunctionBytecode *top[16] = {0};
    JSFunctionBytecode *top_calls[12] = {0};
    uint64_t total_ops = 0, total_calls = 0;
    unsigned bodies = 0;
    struct list_head *el;
    list_for_each(el, &runtime->gc_obj_list) {
        JSGCObjectHeader *header = list_entry(el, JSGCObjectHeader, link);
        if (js_rc(header)->gc_obj_type != JS_GC_OBJ_TYPE_FUNCTION_BYTECODE) continue;
        JSFunctionBytecode *body = (JSFunctionBytecode *)header;
        if (body->tf_job_epoch != tf_region_job_epoch) continue;
        total_ops += body->tf_job_interpreter_ops;
        total_calls += body->tf_job_calls;
        bodies++;
        for (unsigned i = 0; i < sizeof(top) / sizeof(top[0]); i++) {
            if (top[i] != NULL && top[i]->tf_job_interpreter_ops >= body->tf_job_interpreter_ops)
                continue;
            for (unsigned j = sizeof(top) / sizeof(top[0]) - 1; j > i; j--)
                top[j] = top[j - 1];
            top[i] = body;
            break;
        }
        for (unsigned i = 0; i < sizeof(top_calls) / sizeof(top_calls[0]); i++) {
            if (top_calls[i] != NULL && top_calls[i]->tf_job_calls >= body->tf_job_calls)
                continue;
            for (unsigned j = sizeof(top_calls) / sizeof(top_calls[0]) - 1; j > i; j--)
                top_calls[j] = top_calls[j - 1];
            top_calls[i] = body;
            break;
        }
    }
    fprintf(stderr, "tf-job-census wall-us=%llu live-bodies=%u interpreter-ops=%llu calls=%llu\n",
        (unsigned long long)wall_us, bodies,
        (unsigned long long)total_ops, (unsigned long long)total_calls);
    for (unsigned i = 0; i < sizeof(top) / sizeof(top[0]) && top[i] != NULL; i++) {
        JSFunctionBytecode *body = top[i];
        char name[96];
        JSAtom atom = body->func_name;
        if (__JS_AtomIsTaggedInt(atom)) snprintf(name, sizeof(name), "%u", __JS_AtomToUInt32(atom));
        else if (atom == JS_ATOM_NULL) snprintf(name, sizeof(name), "anonymous");
        else {
            JSString *str = runtime->atom_array[atom];
            unsigned n = str->len < sizeof(name) - 1 ? str->len : sizeof(name) - 1;
            for (unsigned j = 0; j < n; j++) {
                unsigned ch = string_get(str, j);
                name[j] = ch >= 32 && ch < 127 && ch != '"' ? (char)ch : '?';
            }
            name[n] = 0;
        }
        fprintf(stderr, "tf-job-body rank=%u body=%p bytes=%d interpreter-ops=%llu calls=%u primitive-ops=%llu reference-ops=%llu value-ops=%llu,%llu,%llu,%llu name=\"%s\"\n",
            i + 1, (void *)body, body->byte_code_len,
            (unsigned long long)body->tf_job_interpreter_ops, body->tf_job_calls,
            (unsigned long long)body->tf_job_primitive_ops,
            (unsigned long long)body->tf_job_reference_ops,
            (unsigned long long)body->tf_job_value_ops[0],
            (unsigned long long)body->tf_job_value_ops[1],
            (unsigned long long)body->tf_job_value_ops[2],
            (unsigned long long)body->tf_job_value_ops[3], name);
    }
    for (unsigned i = 0; i < sizeof(top_calls) / sizeof(top_calls[0]) && top_calls[i] != NULL; i++) {
        JSFunctionBytecode *body = top_calls[i];
        fprintf(stderr, "tf-job-call-body rank=%u body=%p bytes=%d interpreter-ops=%llu calls=%u\n",
            i + 1, (void *)body, body->byte_code_len,
            (unsigned long long)body->tf_job_interpreter_ops, body->tf_job_calls);
        if (body->byte_code_len <= 128) {
            fprintf(stderr, "tf-job-call-code rank=%u seq=", i + 1);
            unsigned at = 0, decoded = 0;
            while (at < (unsigned)body->byte_code_len && decoded++ < 32) {
                unsigned opcode = body->byte_code_buf[at];
                fprintf(stderr, "%s%s", at ? "/" : "",
                    tf_region_opcode_names[opcode]
                        ? tf_region_opcode_names[opcode] : "?");
                unsigned size = tf_region_opcode_sizes[opcode];
                if (size == 0) break;
                at += size;
            }
            fprintf(stderr, "\n");
        }
    }
    for (unsigned rank = 0; rank < 16; rank++) {
        unsigned best = 0;
        for (unsigned i = 1; i < 256; i++)
            if (tf_region_job_opcode_counts[i] > tf_region_job_opcode_counts[best]) best = i;
        if (tf_region_job_opcode_counts[best] == 0) break;
        fprintf(stderr, "tf-job-opcode rank=%u opcode=%u name=%s count=%llu\n",
            rank + 1, best, tf_region_opcode_names[best]
                ? tf_region_opcode_names[best] : "?",
            (unsigned long long)tf_region_job_opcode_counts[best]);
        tf_region_job_opcode_counts[best] = 0;
    }
    for (unsigned rank = 0; rank < 16; rank++) {
        unsigned first = 0, second = 0;
        for (unsigned i = 0; i < 256; i++) for (unsigned j = 0; j < 256; j++)
            if (tf_region_job_pair_counts[i][j] >
                tf_region_job_pair_counts[first][second]) {
                first = i;
                second = j;
            }
        unsigned count = tf_region_job_pair_counts[first][second];
        if (count == 0) break;
        fprintf(stderr, "tf-job-pair rank=%u first=%s second=%s count=%u\n",
            rank + 1, tf_region_opcode_names[first]
                ? tf_region_opcode_names[first] : "?",
            tf_region_opcode_names[second]
                ? tf_region_opcode_names[second] : "?", count);
        tf_region_job_pair_counts[first][second] = 0;
    }
    for (unsigned rank = 0; rank < 16; rank++) {
        unsigned first = 0, second = 0;
        for (unsigned i = 0; i < 256; i++) for (unsigned j = 0; j < 256; j++)
            if (tf_region_job_adjacent_counts[i][j] >
                tf_region_job_adjacent_counts[first][second]) {
                first = i;
                second = j;
            }
        unsigned count = tf_region_job_adjacent_counts[first][second];
        if (count == 0) break;
        fprintf(stderr, "tf-job-adjacent rank=%u first=%s second=%s count=%u\n",
            rank + 1, tf_region_opcode_names[first]
                ? tf_region_opcode_names[first] : "?",
            tf_region_opcode_names[second]
                ? tf_region_opcode_names[second] : "?", count);
        tf_region_job_adjacent_counts[first][second] = 0;
    }
}

/* Called only by the isolated engine's JS_SetMemoryLimit. Browser boot and
   page-pressure raises/lowering remain authoritative; native reservations
   reduce the effective QuickJS room without double-charging physical Budget. */
static size_t tf_region_live_limit(JSRuntime *rt, size_t limit)
{
    TFRegionLiveState *state = tf_region_live_state(rt);
    if (!state) return limit;
    state->heap.ceiling = limit;
    return state->heap.reserved > limit ? 0 : limit - state->heap.reserved;
}

static TFRegionLiveState *tf_region_live_start(JSRuntime *rt)
{
    unsigned slot = 0;
    while (slot < TF_LIVE_RUNTIME_LIMIT && tf_live_states[slot]) slot++;
    if (slot == TF_LIVE_RUNTIME_LIMIT ||
        rt->malloc_ctx.mf.js_malloc != budget_quickjs_pool_allocator()->js_malloc ||
        !rt->malloc_ctx.malloc_state.opaque) return NULL;
    Budget *budget = budget_quickjs_pool_owner(rt->malloc_ctx.malloc_state.opaque);
    if (!budget) return NULL;
    ProbeHeap heap = {rt, rt->malloc_ctx.malloc_state.malloc_limit, 0, false};
    if (!reserve(&heap, sizeof(TFRegionLiveState))) return NULL;
    TFRegionLiveState *state = budget_calloc_category(budget, BUDGET_CATEGORY_JAVASCRIPT,
                                                      1, sizeof(*state));
    if (!state) { release(&heap, sizeof(TFRegionLiveState)); return NULL; }
    state->runtime = rt;
    state->budget = budget;
    state->heap = heap;
    tf_live_states[slot] = state;
    state->pool = native_code_pool_create(budget,
        (NativeCodeHeap){&state->heap, reserve, release},
        (NativeCodeBackend){NULL, code_page_size(), map_code, publish_code, unmap_code},
        NATIVE_POOL_BYTE_LIMIT);
    if (!state->pool) {
        tf_live_states[slot] = NULL;
        release(&state->heap, sizeof(*state));
        budget_free(budget, state);
        return NULL;
    }
    return state;
}

static TFRegionPlan *tf_region_live_select(JSContext *ctx, JSFunctionBytecode *body,
                                            JSValueConst function)
{
#ifdef TF_REGION_LIVE_DISABLED
    (void)ctx; (void)body; (void)function;
    return NULL;
#else
    TFRegionLiveState *state = tf_region_live_state(ctx->rt);
    if (state) for (unsigned i = 0; i < state->plan_count; i++)
        if (state->plans[i]->body == body) return state->plans[i];
    if (body->tf_calls != TF_LIVE_WARM_CALLS || body->is_lazy ||
        body->byte_code_len < 64 || body->byte_code_len > TF_REGION_BYTECODE_LIMIT ||
        body->func_kind != JS_FUNC_NORMAL || ctx->host_retired)
        return NULL;
    if (!state) state = tf_region_live_start(ctx->rt);
    if (!state || state->attempts >= TF_LIVE_ATTEMPT_LIMIT ||
        state->plan_count >= TF_LIVE_PLAN_LIMIT) return NULL;
    state->attempts++;
    TFRegionPlan *plan = compile_plan(ctx, state->budget, &state->heap,
                                      state->pool, function);
    if (plan) {
        state->plans[state->plan_count++] = plan;
        fprintf(stderr, "tf-live-tier admitted=1 slot=%u body-bytes=%d code-bytes=%u plan-bytes=%zu\n",
            state->plan_count, body->byte_code_len, plan->code_bytes,
            tf_region_plan_bytes(body->byte_code_len, true));
    } else {
        fprintf(stderr, "tf-live-tier admitted=0 body-bytes=%d\n", body->byte_code_len);
    }
    return plan;
#endif
}

static void tf_region_live_reset(JSRuntime *rt)
{
    for (unsigned i = 0; i < TF_LIVE_RUNTIME_LIMIT; i++) {
        TFRegionLiveState *state = tf_live_states[i];
        if (!state || state->runtime != rt) continue;
        for (unsigned j = 0; j < state->plan_count; j++) {
            TFRegionPlan *plan = state->plans[j];
            fprintf(stderr, "tf-live-tier slot=%u calls=%llu completed=%llu quota=%llu guards=%llu\n",
                j + 1, (unsigned long long)plan->calls,
                (unsigned long long)plan->completed,
                (unsigned long long)plan->quota_exits,
                (unsigned long long)plan->guards);
            if (tf_region_plan == plan) tf_region_plan = NULL;
            destroy_plan_runtime(rt, state->budget, &state->heap, plan);
        }
        CHECK(native_code_pool_destroy(&state->pool));
        Budget *budget = state->budget;
        CHECK(state->heap.reserved == sizeof(*state));
        release(&state->heap, sizeof(*state));
        tf_live_states[i] = NULL;
        budget_free(budget, state);
        return;
    }
}
#endif
