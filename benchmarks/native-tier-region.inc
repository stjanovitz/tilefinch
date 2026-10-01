/* Included inside an isolated QuickJS translation unit, before CallInternal.
   All generic guards leave the unsupported operation wholly to the interpreter.
   No shipping hooks, retained raw bytecode pointers, or author-facing API. */
#include "native-tier-code-pool.h"

#if !DIRECT_DISPATCH || !SHORT_OPCODES || (!defined(__aarch64__) && !defined(__PSP__))
#error The region handoff requires ARM64 or PSP and direct dispatch
#endif
#if defined(TF_REGION_TABLE_DISPATCH) && !defined(TF_REGION_FRAME_LEASE)
#error Frame-selected dispatch requires the retained frame handoff
#endif

#define TF_REGION_BYTECODE_LIMIT 4096u
#define TF_REGION_CODE_LIMIT (64u * 1024u)
#define TF_REGION_STEP_LIMIT 32u
#define TF_REGION_QUANTUM 4096u
#include "native-tier-block-census.inc"
#ifdef TF_REGION_LIVE_ENGINE
static bool chained_emission = true;
static bool value_abi = true;
static bool direct_branches = true;
static bool continuation_values = true;
#else
static bool chained_emission;
static bool value_abi;
static bool direct_branches;
static bool continuation_values;
#endif
/* Research toggle: retain the interpreter's ordinary frame cleanup while
   transferring a terminal result out of the emitted activation exactly once. */
static bool native_returns;
static bool method_calls; /* isolated whole-function continuation research */
typedef struct TFRegionPlan TFRegionPlan;
typedef struct TFValueBlock TFPrimitiveBlock;

typedef struct {
    JSFunctionBytecode *body;
    JSValue *locals, *args, *base, *limit, *sp;
    JSVarRef **refs;
    const uint8_t *pc;
    unsigned completed;
    JSContext *ctx;
    JSStackFrame *frame;
    bool threw, quota_exit;
    bool returned;
    JSValue result;
#ifdef __PSP__
    unsigned value_block_hits; /* emitted only in the explicit semantic probe */
#endif
    TFRegionPlan *plan; /* leased activation owns the predecoded metadata */
    unsigned reference_block_hits; /* enabled only by focused semantics */
} TFRegionFrame;
typedef int (*TFRegionHelper)(TFRegionFrame *, uint32_t, uint32_t);
typedef void (*TFRegionEntry)(TFRegionFrame *, const void *);
#ifdef TF_REGION_COMPACT_PLAN_U32
typedef uint32_t TFRegionOffset;
#else
typedef uint16_t TFRegionOffset;
#endif

struct TFRegionPlan {
    JSValue root;
    JSFunctionBytecode *body; /* protected by root, never key by address alone */
    NativeCodePool *pool;
    NativeCodeHandle image;
#ifndef TF_REGION_COMPACT_PLAN
    uint32_t entry[TF_REGION_BYTECODE_LIMIT]; /* offset + 1, zero is refusal */
    uint8_t steps[TF_REGION_BYTECODE_LIMIT];
#else
    uint8_t *steps; /* chained boundaries are compile scratch, never retained */
#endif
    unsigned entries, code_bytes, instructions;
    uint64_t calls, completed, guards, quota_exits, exceptions, lease_refusals;
    uint64_t lease_acquisitions;
    uint64_t returns;
    uint64_t call_continuations;
    unsigned active_frames;
    bool enabled, chained, retired, admission_only, pointer_leases;
    TFPrimitiveBlock *reference_blocks;
    size_t reference_bytes;
    unsigned reference_count;
#ifdef __PSP__
    unsigned binding_kind, binding_index, fused_fields, basic_blocks, cold_blocks, cold_start;
    unsigned semantic_direct_sites;
    unsigned value_blocks;
    unsigned borrowed_field_blocks;
    unsigned cfg_regions;
    unsigned cfg_owned_stores;
#endif
#ifdef TF_REGION_COMPACT_PLAN
    TFRegionOffset entry[]; /* exact bytecode length; native byte offset + 1 */
#endif
};

static size_t tf_region_plan_bytes(unsigned length, bool chained)
{
    if (!length || length > TF_REGION_BYTECODE_LIMIT) return 0;
#ifdef TF_REGION_COMPACT_PLAN
    return sizeof(TFRegionPlan) + length * (sizeof(TFRegionOffset) + (chained ? 0u : 1u));
#else
    (void)chained;
    return sizeof(TFRegionPlan);
#endif
}

static bool tf_region_set_entry(TFRegionPlan *plan, unsigned at, unsigned byte_offset)
{
    /* Refuse before narrowing. A full emission buffer is not an entry point;
       every entry must leave room for at least one native instruction. */
    _Static_assert(TF_REGION_CODE_LIMIT == UINT16_MAX + 1u, "compact entry range");
    if (at >= (unsigned)plan->body->byte_code_len || byte_offset % 4 ||
        byte_offset >= TF_REGION_CODE_LIMIT) return false;
    plan->entry[at] = byte_offset + 1;
    return true;
}

/* Probe installs one rooted plan for one runtime. Re-entrant child calls have
   their own handoff and only enter if they execute that exact rooted body. */
#ifdef __PSP__
/* Isolated single-owner PSP fixture. No author-visible global code cache. */
#define TF_REGION_THREAD_LOCAL
#else
#define TF_REGION_THREAD_LOCAL _Thread_local
#endif
static TF_REGION_THREAD_LOCAL TFRegionPlan *tf_region_plan;
/* Proof runs collect handoff counts. Timed probe samples can omit the
   per-handoff 64-bit writes without changing admission or execution. */
static TF_REGION_THREAD_LOCAL bool tf_region_collect_metrics = true;
#ifndef TF_REGION_LIVE_ENGINE
/* Two rooted fixture plans, not a page-controlled or address-only cache. */
static TF_REGION_THREAD_LOCAL TFRegionPlan *tf_region_callee_plan;
static TFRegionPlan *tf_region_fixture_select(JSFunctionBytecode *body)
{
    if (tf_region_plan && tf_region_plan->body == body) return tf_region_plan;
    if (tf_region_callee_plan && tf_region_callee_plan->body == body) return tf_region_callee_plan;
    return NULL;
}
#endif
static bool tf_region_census_active(JSContext *ctx)
{
#ifdef CONFIG_TILEFINCH_EXECUTION_CENSUS
    /* This prototype has no per-emitted-op census yet. Keep an active census
       wholly interpreted rather than silently undercounting its work. */
    return ctx->rt->execution_census;
#else
    (void)ctx;
    return false;
#endif
}
#ifdef TF_REGION_LIVE_ENGINE
static TFRegionPlan *tf_region_live_select(JSContext *ctx, JSFunctionBytecode *body,
                                            JSValueConst function);
/* Count-only attribution is opt-in and separate from elapsed-time A/B runs. */
static uint32_t tf_region_job_epoch;
static bool tf_region_job_census_enabled;
static bool tf_region_call_probe_enabled;
static uint64_t tf_region_call_probe_counter;
static uint64_t tf_region_call_probe_samples;
static uint64_t tf_region_call_probe_ns;
static uint64_t tf_region_call_probe_clock_ns;
#define TF_REGION_CALL_PROBE_START \
    uint64_t tf_region_call_started_ns = 0; \
    if (tf_region_call_probe_enabled && \
        ((++tf_region_call_probe_counter & 63u) == 0u)) { \
        uint64_t tf_clock_started_ns = tf_region_clock_ns(); \
        tf_region_call_started_ns = tf_region_clock_ns(); \
        tf_region_call_probe_clock_ns += \
            tf_region_call_started_ns - tf_clock_started_ns; \
    }
#define TF_REGION_CALL_PROBE_RESTART do { \
    if (tf_region_call_started_ns != 0) { \
        tf_region_call_probe_ns += tf_region_clock_ns() - \
                                   tf_region_call_started_ns; \
        tf_region_call_probe_samples++; \
        tf_region_call_started_ns = 0; \
    } \
} while (0)
static uint64_t tf_region_job_opcode_counts[256];
static uint32_t tf_region_job_pair_counts[256][256];
static uint32_t tf_region_job_adjacent_counts[256][256];
static TFValueBlockCensus tf_region_job_value_blocks[4];
static uint64_t tf_region_job_branch_tags[8];
static uint64_t tf_region_job_body_identity;
static unsigned tf_region_value_block_kind(JSFunctionBytecode *body, const uint8_t *pc);
static void tf_region_primitive_record(JSContext *ctx, JSFunctionBytecode *body,
    bool call, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs);
static void tf_region_reference_record(JSContext *ctx, JSFunctionBytecode *body,
    bool call, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs);
static JSFunctionBytecode *tf_region_job_previous_body;
static int tf_region_job_previous_opcode = -1;
static uint32_t tf_region_job_previous_offset;
static const uint8_t tf_region_opcode_sizes[256] = {
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = size,
#define def(id, size, n_pop, n_push, f) [OP_##id] = size,
#include "quickjs-opcode.h"
#undef DEF
#undef def
};
static const char *const tf_region_opcode_names[256] = {
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = #id,
#define def(id, size, n_pop, n_push, f) [OP_##id] = #id,
#include "quickjs-opcode.h"
#undef DEF
#undef def
};
static const uint8_t tf_region_opcode_pops[256] = {
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = n_pop,
#define def(id, size, n_pop, n_push, f) [OP_##id] = n_pop,
#include "quickjs-opcode.h"
#undef DEF
#undef def
};
static const uint8_t tf_region_opcode_pushes[256] = {
#define DEF(id, size, n_pop, n_push, f) [OP_##id] = n_push,
#define def(id, size, n_pop, n_push, f) [OP_##id] = n_push,
#include "quickjs-opcode.h"
#undef DEF
#undef def
};
static inline void tf_region_job_record(JSContext *ctx, JSFunctionBytecode *body, bool call,
    int opcode, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs)
{
    if (!tf_region_job_census_enabled || tf_region_job_epoch == 0) return;
    if (body->tf_job_epoch != tf_region_job_epoch) {
        body->tf_job_epoch = tf_region_job_epoch;
        body->tf_job_calls = 0;
        body->tf_job_interpreter_ops = 0;
        body->tf_job_primitive_ops = 0;
        body->tf_job_reference_ops = 0;
        body->tf_job_identity = ++tf_region_job_body_identity;
        memset(body->tf_job_value_ops, 0, sizeof(body->tf_job_value_ops));
    }
    tf_region_primitive_record(ctx, body, call, pc, base, sp, locals, args, refs);
    tf_region_reference_record(ctx, body, call, pc, base, sp, locals, args, refs);
    if (call) {
        body->tf_job_calls++;
        for (unsigned model = 0; model < 4; model++)
            tf_value_block_finish(&tf_region_job_value_blocks[model]);
    }
    else {
        body->tf_job_interpreter_ops++;
        tf_region_job_opcode_counts[opcode]++;
        uint32_t offset = (uint32_t)(pc - body->byte_code_buf);
        unsigned kind = tf_region_value_block_kind(body, pc);
        unsigned pops = tf_region_opcode_pops[opcode];
        if (kind == 16) pops += opcode >= OP_call0 && opcode <= OP_call3
            ? (unsigned)opcode - OP_call0 : get_u16(pc + 1);
        for (unsigned model = 0; model < 4; model++) {
            unsigned admitted = kind <= (4u << model) ? kind : 0;
            if (admitted) body->tf_job_value_ops[model]++;
            tf_value_block_record(&tf_region_job_value_blocks[model], body, offset,
                tf_region_opcode_sizes[opcode], admitted,
                pops, tf_region_opcode_pushes[opcode]);
        }
        if (opcode == OP_if_true || opcode == OP_if_false ||
            opcode == OP_if_true8 || opcode == OP_if_false8) {
            int tag = JS_VALUE_GET_NORM_TAG(sp[-1]);
            unsigned bucket = tag == JS_TAG_INT ? 0 : tag == JS_TAG_BOOL ? 1 :
                tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED ? 2 :
                tag_is_string(tag) ? 3 : tag == JS_TAG_OBJECT ? 4 :
                tag == JS_TAG_BIG_INT ? 5 : tag == JS_TAG_FLOAT64 ? 6 : 7;
            tf_region_job_branch_tags[bucket]++;
        }
        if (body == tf_region_job_previous_body &&
            tf_region_job_previous_opcode >= 0 &&
            tf_region_job_pair_counts[tf_region_job_previous_opcode][opcode]
                != UINT32_MAX) {
            tf_region_job_pair_counts[tf_region_job_previous_opcode][opcode]++;
            if (offset == tf_region_job_previous_offset +
                          tf_region_opcode_sizes[tf_region_job_previous_opcode]
                && tf_region_job_adjacent_counts[tf_region_job_previous_opcode][opcode]
                       != UINT32_MAX)
                tf_region_job_adjacent_counts[tf_region_job_previous_opcode][opcode]++;
        }
        tf_region_job_previous_body = body;
        tf_region_job_previous_opcode = opcode;
        tf_region_job_previous_offset = offset;
    }
}
#endif

static bool tf_region_can_release(JSValue value)
{
    return !JS_VALUE_HAS_REF_COUNT(value) ||
        __js_rc(JS_VALUE_GET_PTR(value))->ref_count > 1;
}
static void tf_region_release(JSValue value)
{
    if (JS_VALUE_HAS_REF_COUNT(value)) __js_rc(JS_VALUE_GET_PTR(value))->ref_count--;
}
static int tf_region_done(TFRegionFrame *frame, uint32_t next)
{
    frame->pc = frame->body->byte_code_buf + next;
    frame->completed++;
    return 1;
}
static int tf_region_push(TFRegionFrame *frame, JSValue value, uint32_t next)
{
    if (frame->sp == frame->limit) return 0;
    *frame->sp++ = JS_DupValueRT(NULL, value); /* Dup has no runtime access */
    return tf_region_done(frame, next);
}
static int tf_region_local(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count) return 0;
    return tf_region_push(frame, frame->locals[index], next);
}
static int tf_region_local_check(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count || JS_IsUninitialized(frame->locals[index])) return 0;
    return tf_region_push(frame, frame->locals[index], next);
}
static int tf_region_arg(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->arg_count) return 0;
    return tf_region_push(frame, frame->args[index], next);
}
static int tf_region_ref(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->closure_var_count) return 0;
    return tf_region_push(frame, *frame->refs[index]->pvalue, next);
}
static int tf_region_ref_check(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->closure_var_count || JS_IsUninitialized(*frame->refs[index]->pvalue)) return 0;
    return tf_region_push(frame, *frame->refs[index]->pvalue, next);
}
static int tf_region_put_local(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count || frame->sp == frame->base ||
        !tf_region_can_release(frame->locals[index])) return 0;
    tf_region_release(frame->locals[index]);
    frame->locals[index] = *--frame->sp;
    return tf_region_done(frame, next);
}
static int tf_region_set_local(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count || frame->sp == frame->base ||
        !tf_region_can_release(frame->locals[index])) return 0;
    JSValue value = JS_DupValueRT(NULL, frame->sp[-1]);
    tf_region_release(frame->locals[index]);
    frame->locals[index] = value;
    return tf_region_done(frame, next);
}
static int tf_region_put_local_check(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count || JS_IsUninitialized(frame->locals[index])) return 0;
    return tf_region_put_local(frame, index, next);
}
static int tf_region_set_local_check(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->var_count || JS_IsUninitialized(frame->locals[index])) return 0;
    return tf_region_set_local(frame, index, next);
}
static int tf_region_constant(TFRegionFrame *frame, uint32_t index, uint32_t next)
{
    if (index >= frame->body->cpool_count) return 0;
    return tf_region_push(frame, frame->body->cpool[index], next);
}
static int tf_region_integer(TFRegionFrame *frame, uint32_t value, uint32_t next)
{
    return tf_region_push(frame, JS_MKVAL(JS_TAG_INT, (int32_t)value), next);
}
static int tf_region_immediate(TFRegionFrame *frame, uint32_t opcode, uint32_t next)
{
    JSValue value = JS_UNDEFINED;
    if (opcode == OP_null) value = JS_NULL;
    else if (opcode == OP_push_false) value = JS_FALSE;
    else if (opcode == OP_push_true) value = JS_TRUE;
    return tf_region_push(frame, value, next);
}
static int tf_region_drop(TFRegionFrame *frame, uint32_t unused, uint32_t next)
{
    (void)unused;
    if (frame->sp == frame->base || !tf_region_can_release(frame->sp[-1])) return 0;
    tf_region_release(*--frame->sp);
    return tf_region_done(frame, next);
}
static int tf_region_dup(TFRegionFrame *frame, uint32_t unused, uint32_t next)
{
    (void)unused;
    if (frame->sp == frame->base) return 0;
    return tf_region_push(frame, frame->sp[-1], next);
}
static int tf_region_field(TFRegionFrame *frame, uint32_t atom, uint32_t next)
{
    if (frame->sp == frame->base) return 0;
    JSValue object = frame->sp[-1];
    if (JS_VALUE_GET_TAG(object) != JS_TAG_OBJECT || !tf_region_can_release(object)) return 0;
    JSObject *p = JS_VALUE_GET_OBJ(object);
    if (p->is_exotic) return 0;
    JSProperty *property;
    JSShapeProperty *shape = find_own_property(&property, p, atom);
    if (!shape || (shape->flags & JS_PROP_TMASK)) return 0;
    JSValue result = JS_DupValueRT(NULL, property->u.value);
    tf_region_release(object);
    frame->sp[-1] = result;
    return tf_region_done(frame, next);
}

/* Deliberately keep the frame-based comparator above unchanged for A/B.
   This ABI never allocates, re-enters, destroys a last reference or throws.
   JS_EXCEPTION is a refusal sentinel, without modifying the runtime exception.
   On refusal, the caller still owns its original operand. */
static JSValue tf_region_own_value(JSValue object, uint32_t atom)
{
    if (JS_VALUE_GET_TAG(object) != JS_TAG_OBJECT || !tf_region_can_release(object)) return JS_EXCEPTION;
    JSObject *p = JS_VALUE_GET_OBJ(object);
    if (p->is_exotic) return JS_EXCEPTION;
    JSProperty *property;
    JSShapeProperty *shape = find_own_property(&property, p, atom);
    if (!shape || (shape->flags & JS_PROP_TMASK)) return JS_EXCEPTION;
    JSValue result = JS_DupValueRT(NULL, property->u.value);
    tf_region_release(object);
    return result;
}

static int tf_region_increment(TFRegionFrame *f, uint32_t index, uint32_t next)
{
    if (index >= f->body->var_count || JS_VALUE_GET_TAG(f->locals[index]) != JS_TAG_INT ||
        JS_VALUE_GET_INT(f->locals[index]) == INT32_MAX) return 0;
    f->locals[index] = JS_NewInt32(f->ctx, JS_VALUE_GET_INT(f->locals[index]) + 1);
    return tf_region_done(f, next);
}
static int tf_region_post_increment(TFRegionFrame *f, uint32_t unused, uint32_t next)
{
    (void)unused;
    if (f->sp == f->base || f->sp == f->limit ||
        JS_VALUE_GET_TAG(f->sp[-1]) != JS_TAG_INT ||
        JS_VALUE_GET_INT(f->sp[-1]) == INT32_MAX) return 0;
    JSValue result = JS_NewInt32(f->ctx, JS_VALUE_GET_INT(f->sp[-1]) + 1);
    *f->sp++ = result; /* post-increment preserves the original below the result */
    return tf_region_done(f, next);
}
static int tf_region_less(TFRegionFrame *f, uint32_t unused, uint32_t next)
{
    (void)unused;
    if (f->sp - f->base < 2 || JS_VALUE_GET_TAG(f->sp[-2]) != JS_TAG_INT ||
        JS_VALUE_GET_TAG(f->sp[-1]) != JS_TAG_INT) return 0;
    int result = JS_VALUE_GET_INT(f->sp[-2]) < JS_VALUE_GET_INT(f->sp[-1]);
    f->sp[-2] = JS_NewBool(f->ctx, result);
    f->sp--;
    return tf_region_done(f, next);
}
static bool tf_region_quota(TFRegionFrame *f)
{
    if (f->completed < TF_REGION_QUANTUM) return false;
    f->quota_exit = true;
    return true;
}
static int tf_region_call_common(TFRegionFrame *f, unsigned argc, bool method,
                                uint32_t next)
{
    unsigned prefix = method ? 2u : 1u;
    if (tf_region_quota(f) || argc > 64 ||
        f->sp - f->base < (ptrdiff_t)argc + prefix) return 0;
    JSValue *args = f->sp - argc;
    f->pc = f->body->byte_code_buf + next;
    f->frame->cur_pc = f->pc;
    if (tf_region_collect_metrics && f->plan) f->plan->call_continuations++;
    /* Use the same current call path as OP_call*, including its native
       function fast entry and interrupt poll. A research helper must not
       silently compare yesterday's slow dispatcher with today's interpreter. */
    JSValue result = js_call_from_bytecode(f->ctx, args[-1],
        method ? args[-2] : JS_UNDEFINED, argc, args);
    if (JS_IsException(result)) { f->threw = true; return 0; }
    /* Match the interpreter exactly: a throwing call leaves its argument
       stack owned for the exception unwinder; success consumes the arguments. */
    for (int i = -(int)prefix; i < (int)argc; i++) JS_FreeValue(f->ctx, args[i]);
    f->sp = args - prefix;
    *f->sp++ = result;
    tf_region_done(f, next);
    /* Callbacks/final releases can retire the owner or disable its plan.
       The active-frame lease pins metadata until normal interpreter cleanup. */
    return !f->ctx->host_retired &&
        (!f->plan || (!f->plan->retired && f->plan->enabled));
}
static int tf_region_call(TFRegionFrame *f, uint32_t argc, uint32_t next)
{ return tf_region_call_common(f, argc, false, next); }
static int tf_region_call_method(TFRegionFrame *f, uint32_t argc, uint32_t next)
{ return tf_region_call_common(f, argc, true, next); }
static int tf_region_return(TFRegionFrame *f, uint32_t opcode, uint32_t next)
{
    if (tf_region_quota(f) || (opcode == OP_return && f->sp == f->base)) return 0;
    /* Move, do not duplicate: ordinary done: frees only the remaining stack.
       The handoff macro moves this value to ret_val before closing the lease. */
    f->result = opcode == OP_return ? *--f->sp : JS_UNDEFINED;
    f->returned = true;
    tf_region_done(f, next);
    return 0; /* terminal operation, never decode beyond the bytecode buffer */
}
static int tf_region_branch(TFRegionFrame *f, uint32_t opcode, uint32_t next)
{
    if (tf_region_quota(f)) return 0;
    const uint8_t *operand = f->pc + 1;
    bool conditional = opcode == OP_if_false || opcode == OP_if_true ||
        opcode == OP_if_false8 || opcode == OP_if_true8;
    bool taken = true;
    if (conditional) {
        if (f->sp == f->base) return 0;
        JSValue value = f->sp[-1];
        int truth = (uint32_t)JS_VALUE_GET_TAG(value) <= JS_TAG_UNDEFINED ?
            JS_VALUE_GET_INT(value) : JS_ToBoolFree(f->ctx, value);
        f->sp--;
        taken = (opcode == OP_if_true || opcode == OP_if_true8) ? !!truth : !truth;
    }
    int32_t delta = opcode == OP_goto16 ? (int16_t)get_u16(operand) :
        (opcode == OP_goto || opcode == OP_if_true || opcode == OP_if_false) ?
        (int32_t)get_u32(operand) : (int8_t)*operand;
    f->pc = taken ? operand + delta : f->body->byte_code_buf + next;
    f->completed++;
    if (js_poll_interrupts(f->ctx)) { f->threw = true; return 0; }
    return taken ? 2 : 1;
}

#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
/* Common run-breaking operations share a bounded semantic continuation rather
   than re-entering CallInternal. No coercion, allocation, author callback or
   final-reference release is admitted. The generic interpreter still owns
   complex equality, TDZ errors and every refused operation, before effects. */
static int tf_region_semantic(TFRegionFrame *f, uint32_t opcode, uint32_t next)
{
    if (tf_region_quota(f)) return 0;
    if (opcode == OP_set_loc_uninitialized) {
        unsigned index = get_u16(f->pc + 1);
        if (index >= f->body->var_count || !tf_region_can_release(f->locals[index])) return 0;
        tf_region_release(f->locals[index]);
        f->locals[index] = JS_UNINITIALIZED;
        return tf_region_done(f, next);
    }
    if (f->sp == f->base || !tf_region_can_release(f->sp[-1])) return 0;
    JSValue right = f->sp[-1];
    int result;
    if (opcode == OP_strict_eq || opcode == OP_strict_neq) {
        if (f->sp - f->base < 2 || !tf_region_can_release(f->sp[-2])) return 0;
        JSValue left = f->sp[-2];
        int lt = JS_VALUE_GET_TAG(left), rt = JS_VALUE_GET_TAG(right);
        /* String/large-BigInt comparison can walk variable-size storage. Keep
           it and its existing work accounting in the interpreter. */
        if (tag_is_string(lt) || tag_is_string(rt) || lt == JS_TAG_BIG_INT || rt == JS_TAG_BIG_INT) return 0;
        if (JS_VALUE_HAS_REF_COUNT(left) && JS_VALUE_HAS_REF_COUNT(right) &&
            JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right) &&
            __js_rc(JS_VALUE_GET_PTR(left))->ref_count <= 2) return 0;
        result = js_strict_eq2(f->ctx, left, right, JS_EQ_STRICT) ^ (opcode == OP_strict_neq);
        tf_region_release(left);
        tf_region_release(right);
        f->sp--;
    } else {
        switch (opcode) {
        case OP_lnot:
            if (JS_VALUE_GET_TAG(right) == JS_TAG_BIG_INT) return 0;
            result = !JS_ToBool(f->ctx, right); break;
        case OP_is_null: result = JS_IsNull(right); break;
        case OP_is_undefined: result = JS_IsUndefined(right); break;
        case OP_is_undefined_or_null: result = JS_IsNull(right) || JS_IsUndefined(right); break;
        default: return 0;
        }
        tf_region_release(right);
    }
    f->sp[-1] = JS_NewBool(f->ctx, result);
    return tf_region_done(f, next);
}
#endif

typedef struct { uint8_t size, pop, push; const char *name; } TFRegionOpcode;
static const TFRegionOpcode tf_region_opcodes[256] = {
#define DEF(id, size, pop, push, format) [OP_##id] = {size, pop, push, #id},
#define def(id, size, pop, push, format)
#include "quickjs-opcode.h"
};

static TFRegionHelper tf_region_decode(const uint8_t *pc, uint32_t *argument)
{
    unsigned op = pc[0];
    *argument = 0;
    if (op >= OP_get_loc0 && op <= OP_get_loc3) { *argument = op - OP_get_loc0; return tf_region_local; }
    if (op >= OP_get_arg0 && op <= OP_get_arg3) { *argument = op - OP_get_arg0; return tf_region_arg; }
    if (op >= OP_get_var_ref0 && op <= OP_get_var_ref3) { *argument = op - OP_get_var_ref0; return tf_region_ref; }
    if (op >= OP_put_loc0 && op <= OP_put_loc3) { *argument = op - OP_put_loc0; return tf_region_put_local; }
    if (op >= OP_set_loc0 && op <= OP_set_loc3) { *argument = op - OP_set_loc0; return tf_region_set_local; }
    if (op >= OP_push_minus1 && op <= OP_push_7) { *argument = (uint32_t)(op - OP_push_0); return tf_region_integer; }
    if (chained_emission) {
#if defined(TF_REGION_FRAME_LEASE) && !defined(TF_REGION_INTERPRETER_ONLY)
        if (native_returns && (op == OP_return || op == OP_return_undef)) {
            *argument = op; return tf_region_return;
        }
#endif
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
        switch (op) {
        case OP_set_loc_uninitialized: case OP_lnot:
        case OP_strict_eq: case OP_strict_neq:
        case OP_is_null: case OP_is_undefined: case OP_is_undefined_or_null:
            *argument = op; return tf_region_semantic;
        }
#endif
        if (op >= OP_call0 && op <= OP_call3) { *argument = op - OP_call0; return tf_region_call; }
        switch (op) {
        case OP_call: *argument = get_u16(pc + 1); return tf_region_call;
        case OP_inc_loc: *argument = pc[1]; return tf_region_increment;
        case OP_post_inc: return tf_region_post_increment;
        case OP_lt: return tf_region_less;
        case OP_if_false: case OP_if_true: case OP_if_false8: case OP_if_true8:
        case OP_goto: case OP_goto8: case OP_goto16: *argument = op; return tf_region_branch;
        }
    }
    switch (op) {
    case OP_get_loc: *argument = get_u16(pc + 1); return tf_region_local;
    case OP_get_loc8: *argument = pc[1]; return tf_region_local;
    case OP_get_loc_check: *argument = get_u16(pc + 1); return tf_region_local_check;
    case OP_get_arg: *argument = get_u16(pc + 1); return tf_region_arg;
    case OP_get_var_ref: *argument = get_u16(pc + 1); return tf_region_ref;
    case OP_get_var_ref_check: *argument = get_u16(pc + 1); return tf_region_ref_check;
    case OP_put_loc: *argument = get_u16(pc + 1); return tf_region_put_local;
    case OP_put_loc8: *argument = pc[1]; return tf_region_put_local;
    case OP_set_loc: *argument = get_u16(pc + 1); return tf_region_set_local;
    case OP_set_loc8: *argument = pc[1]; return tf_region_set_local;
    case OP_put_loc_check: *argument = get_u16(pc + 1); return tf_region_put_local_check;
    case OP_set_loc_check: *argument = get_u16(pc + 1); return tf_region_set_local_check;
    case OP_push_const: *argument = get_u32(pc + 1); return tf_region_constant;
    case OP_push_const8: *argument = pc[1]; return tf_region_constant;
    case OP_push_i8: *argument = (int8_t)pc[1]; return tf_region_integer;
    case OP_push_i16: *argument = (int16_t)get_u16(pc + 1); return tf_region_integer;
    case OP_push_i32: *argument = get_u32(pc + 1); return tf_region_integer;
    case OP_undefined: case OP_null: case OP_push_false: case OP_push_true:
        *argument = op; return tf_region_immediate;
    case OP_drop: return tf_region_drop;
    case OP_dup: return tf_region_dup;
    case OP_get_field: *argument = get_u32(pc + 1); return tf_region_field;
    case OP_get_length: *argument = JS_ATOM_length; return tf_region_field;
    case OP_call_method:
        if (!chained_emission || !method_calls) return NULL;
        *argument = get_u16(pc + 1); return tf_region_call_method;
    default: return NULL;
    }
}

#include "native-tier-primitive-blocks.inc"
#include "native-tier-reference-blocks.inc"
#include "native-tier-borrowed-cfg.inc"

#ifdef TF_REGION_LIVE_ENGINE
/* One bounded, diagnostic-only scan cache. The per-job body serial prevents
   allocator-address reuse from reusing another body's map. No body is rooted,
   no executable admission is performed, and measured runs keep this off. */
static struct {
    uint8_t boundaries[TF_REGION_BYTECODE_LIMIT], starts[TF_REGION_BYTECODE_LIMIT];
    uint8_t reference_starts[TF_REGION_BYTECODE_LIMIT];
    JSFunctionBytecode *body;
    const uint8_t *code;
    uint64_t identity;
    unsigned length;
} tf_primitive_scan_cache;
static struct {
    uint64_t total, candidates, completed, accepted, accepted_ops, refused_ops, abandoned, scans;
    uint64_t guards[TF_PRIMITIVE_GUARD_COUNT], histogram[TF_PRIMITIVE_BLOCK_LIMIT + 1];
    uint64_t types[7]; /* object, string, symbol, BigInt, float, TDZ, other */
    TFPrimitiveBlock pending;
    JSFunctionBytecode *body;
    unsigned next, guard;
} tf_primitive_job;
typedef struct {
    uint64_t candidates, completed, accepted, accepted_ops, refused_ops, abandoned;
    uint64_t verified, mismatches, fields, inherited_fields;
    uint64_t guards[TF_REFERENCE_GUARD_COUNT], histogram[TF_PRIMITIVE_BLOCK_LIMIT + 1];
    struct {
        uint64_t regions, instructions, acquires, releases, net_headers;
        uint64_t balanced_reference_regions, incoming, outgoing, reference_outgoing;
    } frontiers[TF_REFERENCE_FRONTIER_COUNT];
    uint64_t frontier_opcodes[256]; /* fixed diagnostic histogram, no page authority */
    TFPrimitiveBlock pending;
    TFReferenceResult result;
    JSFunctionBytecode *body;
    unsigned next, guard, prefix;
} TFReferenceJob;
static TFReferenceJob tf_reference_job, tf_borrowed_job;

static bool tf_primitive_map(JSFunctionBytecode *body)
{
    if (body->func_kind != JS_FUNC_NORMAL || body->byte_code_len <= 0 ||
        body->byte_code_len > TF_REGION_BYTECODE_LIMIT) return false;
    if (tf_primitive_scan_cache.body == body &&
        tf_primitive_scan_cache.identity == body->tf_job_identity &&
        tf_primitive_scan_cache.code == body->byte_code_buf &&
        tf_primitive_scan_cache.length == (unsigned)body->byte_code_len) return true;
    unsigned length = body->byte_code_len;
    memset(tf_primitive_scan_cache.boundaries, 0, length);
    memset(tf_primitive_scan_cache.starts, 0, length);
    memset(tf_primitive_scan_cache.reference_starts, 0, length);
    tf_primitive_scan_cache.body = NULL;
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        if (!size || size > length - at) return false;
        tf_primitive_scan_cache.boundaries[at] = 1;
        at += size;
    }
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        TFPrimitiveBlock b;
        if (tf_primitive_scan(body, tf_primitive_scan_cache.boundaries, at, &b)) {
            tf_primitive_scan_cache.starts[at] = b.count;
            at = b.end;
        } else at += size;
    }
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        TFPrimitiveBlock b;
        if (tf_reference_scan(body, tf_primitive_scan_cache.boundaries, at, &b)) {
            tf_primitive_scan_cache.reference_starts[at] = b.count;
            at = b.end;
        } else at += size;
    }
    tf_primitive_scan_cache.body = body;
    tf_primitive_scan_cache.code = body->byte_code_buf;
    tf_primitive_scan_cache.identity = body->tf_job_identity;
    tf_primitive_scan_cache.length = length;
    tf_primitive_job.scans++;
    return true;
}

static void tf_region_primitive_record(JSContext *ctx, JSFunctionBytecode *body,
    bool call, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs)
{
    if (call) {
        if (tf_primitive_job.next) tf_primitive_job.abandoned++;
        tf_primitive_job.next = 0;
        return;
    }
    tf_primitive_job.total++;
    unsigned offset = (unsigned)(pc - body->byte_code_buf);
    if (tf_primitive_job.next) {
        TFPrimitiveBlock *b = &tf_primitive_job.pending;
        if (tf_primitive_job.body == body && offset == b->offsets[tf_primitive_job.next]) {
            if (++tf_primitive_job.next == b->count) {
                tf_primitive_job.completed++;
                if (tf_primitive_job.guard == TF_PRIMITIVE_ACCEPT) {
                    tf_primitive_job.accepted++;
                    tf_primitive_job.accepted_ops += b->count;
                    body->tf_job_primitive_ops += b->count;
                    tf_primitive_job.histogram[b->count]++;
                } else tf_primitive_job.refused_ops += b->count;
                tf_primitive_job.next = 0;
            }
            return;
        }
        tf_primitive_job.abandoned++;
        tf_primitive_job.next = 0;
    }
    if (!tf_primitive_map(body) || !tf_primitive_scan_cache.starts[offset]) return;
    TFPrimitiveBlock *b = &tf_primitive_job.pending;
    if (!tf_primitive_scan(body, tf_primitive_scan_cache.boundaries, offset, b)) return;
    tf_primitive_job.candidates++;
    int refusal_tag;
    tf_primitive_job.guard = tf_primitive_guards(ctx, body, b, base, base + body->stack_size,
                                                 sp, locals, args, refs, &refusal_tag);
    if (tf_primitive_job.guard == TF_PRIMITIVE_TYPE ||
        tf_primitive_job.guard == TF_PRIMITIVE_INPUT_TYPE) {
        unsigned bucket = refusal_tag == JS_TAG_OBJECT ? 0 : tag_is_string(refusal_tag) ? 1 :
            refusal_tag == JS_TAG_SYMBOL ? 2 :
            refusal_tag == JS_TAG_BIG_INT || refusal_tag == JS_TAG_SHORT_BIG_INT ? 3 :
            refusal_tag == JS_TAG_FLOAT64 ? 4 : refusal_tag == JS_TAG_UNINITIALIZED ? 5 : 6;
        tf_primitive_job.types[bucket]++;
    }
    tf_primitive_job.guards[tf_primitive_job.guard]++;
    tf_primitive_job.body = body;
    tf_primitive_job.next = 1;
}

/* Check the prediction against the interpreter at its branch or handoff.
   This validates live values and exact RC deltas, not just opcode eligibility. */
static bool tf_reference_verify(const TFReferenceResult *r, unsigned prefix, bool conditional,
                                JSValue *base, JSValue *sp)
{
    if (sp - base != (ptrdiff_t)(prefix + r->depth)) return false;
    for (unsigned i = 0; i < r->depth; i++)
        if (!tf_reference_same(base[prefix+i], r->values[i])) return false;
    JSValue condition = conditional ? r->values[r->depth - 1] : JS_UNDEFINED;
    JSRefCountHeader *condition_ref = JS_VALUE_HAS_REF_COUNT(condition) ?
        __js_rc(JS_VALUE_GET_PTR(condition)) : NULL;
    for (unsigned i = 0; i < r->ref_count; i++) {
        const TFReferenceCount *ref = &r->refs[i];
        if (ref->header->ref_count != ref->initial + ref->delta +
                                      (ref->header == condition_ref)) return false;
    }
    return true;
}

static void tf_region_reference_record_model(TFReferenceJob *job, bool borrowed,
    JSContext *ctx, JSFunctionBytecode *body,
    bool call, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs)
{
    if (call) {
        if (job->next) job->abandoned++;
        job->next = 0;
        return;
    }
    unsigned offset = (unsigned)(pc - body->byte_code_buf);
    if (job->next) {
        TFPrimitiveBlock *b = &job->pending;
        unsigned expected = job->next < b->count ? b->offsets[job->next] : b->end;
        if (job->body == body && offset == expected) {
            unsigned terminal = b->count + !b->conditional;
            if (++job->next == terminal) {
                job->completed++;
                if (job->guard == TF_REFERENCE_ACCEPT) {
                    if (tf_reference_verify(&job->result, job->prefix,
                                            b->conditional, base, sp)) {
                        job->verified++; job->accepted++; job->accepted_ops += b->count;
                        if (!borrowed) body->tf_job_reference_ops += b->count;
                        job->fields += job->result.fields;
                        job->inherited_fields += job->result.inherited_fields;
                        job->histogram[b->count]++;
                        unsigned frontier = tf_reference_frontier(body,b);
                        job->frontiers[frontier].regions++;
                        unsigned terminal = b->conditional ? b->opcodes[b->count-1] :
                            body->byte_code_buf[b->end];
                        job->frontier_opcodes[terminal]++;
                        job->frontiers[frontier].instructions += b->count;
                        job->frontiers[frontier].acquires += job->result.acquires;
                        job->frontiers[frontier].releases += job->result.releases;
                        unsigned changed = 0;
                        for (unsigned i = 0; i < job->result.ref_count; i++)
                            changed += job->result.refs[i].delta != 0;
                        job->frontiers[frontier].net_headers += changed;
                        job->frontiers[frontier].balanced_reference_regions +=
                            !changed && (job->result.acquires || job->result.releases);
                        job->frontiers[frontier].incoming += b->incoming;
                        unsigned outgoing = job->result.depth-b->conditional;
                        job->frontiers[frontier].outgoing += outgoing;
                        for (unsigned i = 0; i < outgoing; i++)
                            job->frontiers[frontier].reference_outgoing +=
                                JS_VALUE_HAS_REF_COUNT(job->result.values[i]);
                    } else job->mismatches++;
                } else job->refused_ops += b->count;
                job->next = 0;
            }
            if (b->conditional || job->next) return;
            /* A fallthrough boundary can start the next disjoint region. */
        } else {
            job->abandoned++;
            job->next = 0;
        }
    }
    if (!tf_primitive_map(body) || !tf_primitive_scan_cache.reference_starts[offset]) return;
    TFPrimitiveBlock *b = &job->pending;
    if (!(borrowed ? tf_borrowed_field_scan(body,tf_primitive_scan_cache.boundaries,offset,b) :
                    tf_reference_scan(body,tf_primitive_scan_cache.boundaries,offset,b))) return;
    job->candidates++;
    job->guard = tf_reference_guards(ctx, body, b, base, base + body->stack_size,
        sp, locals, args, refs, &job->result);
    if (borrowed && job->guard == TF_REFERENCE_ACCEPT) {
        if (job->result.own_fields != job->result.fields) job->guard = TF_REFERENCE_FIELD;
        else if (JS_VALUE_GET_NORM_TAG(job->result.values[0]) == JS_TAG_FLOAT64) job->guard = TF_REFERENCE_TRUTH;
        else for (unsigned i = 0; i < job->result.ref_count; i++)
            if (job->result.refs[i].delta) { job->guard = TF_REFERENCE_VALUE; break; }
    }
    job->guards[job->guard]++;
    job->prefix = (unsigned)(sp - base) - b->incoming;
    job->body = body; job->next = 1;
}

static void tf_region_reference_record(JSContext *ctx, JSFunctionBytecode *body,
    bool call, const uint8_t *pc, JSValue *base, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs)
{
    tf_region_reference_record_model(&tf_reference_job,false,ctx,body,call,pc,base,sp,locals,args,refs);
    tf_region_reference_record_model(&tf_borrowed_job,true,ctx,body,call,pc,base,sp,locals,args,refs);
}

static unsigned tf_region_value_block_kind(JSFunctionBytecode *body, const uint8_t *pc)
{
    if (body->func_kind != JS_FUNC_NORMAL || body->byte_code_len <= 0 ||
        (unsigned)body->byte_code_len > TF_REGION_BYTECODE_LIMIT) return 0;
    uint32_t index;
    TFRegionHelper helper = tf_region_decode(pc, &index);
    if (helper == tf_region_local || helper == tf_region_local_check)
        return index < body->var_count && index <= 255 ? 1 : 0;
    if (helper == tf_region_arg) return index < body->arg_count && index <= 255 ? 1 : 0;
    if (helper == tf_region_ref || helper == tf_region_ref_check)
        return index < body->closure_var_count && index <= 255 ? 1 : 0;
    if (helper == tf_region_constant) return index < body->cpool_count && index <= 255 ? 1 : 0;
    if (helper == tf_region_integer || helper == tf_region_immediate ||
        helper == tf_region_drop || helper == tf_region_dup) return 1;
    if (helper == tf_region_put_local || helper == tf_region_put_local_check ||
        helper == tf_region_set_local || helper == tf_region_set_local_check ||
        helper == tf_region_increment)
        return index < body->var_count && index <= 255 ? 2 : 0;
    if (helper == tf_region_less || helper == tf_region_post_increment) return 4;
    if (helper == tf_region_branch) return 32;
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    if (helper == tf_region_semantic) {
        if (pc[0] == OP_set_loc_uninitialized)
            return get_u16(pc + 1) < body->var_count ? 2 : 0;
        return 4;
    }
#endif
    switch (pc[0]) {
    case OP_get_field: case OP_get_field2: case OP_get_length:
    case OP_get_array_el: case OP_get_array_el2: case OP_get_array_el3:
        return 8;
    case OP_call: case OP_call_method:
        return get_u16(pc + 1) <= 64 ? 16 : 0;
    default:
        return pc[0] >= OP_call0 && pc[0] <= OP_call3 ? 16 : 0;
    }
}
#endif

static bool tf_region_enter(JSContext *ctx, JSFunctionBytecode *body, int flags,
                            JSValue *locals, JSValue *args, JSVarRef **refs,
                            JSValue *base, JSStackFrame *sf, JSValue **stack, const uint8_t **pc)
{
    TFRegionPlan *plan = tf_region_plan;
    if (!plan || !plan->enabled || plan->body != body || ctx->host_retired || tf_region_census_active(ctx) ||
        (flags & ~JS_CALL_FLAG_COPY_ARGV) || body->func_kind != JS_FUNC_NORMAL) return false;
    size_t offset = (size_t)(*pc - body->byte_code_buf);
    if (offset >= (size_t)body->byte_code_len || !plan->entry[offset]) return false;
    NativeCodeLease lease;
    if (!native_code_pool_enter(plan->pool, plan->image, plan->chained ? 16 : plan->entry[offset] - 1, &lease)) {
        plan->lease_refusals++;
        return false;
    }
    plan->lease_acquisitions++;
    TFRegionFrame frame = {body, locals, args, base, base + body->stack_size,
                           *stack, refs, *pc, 0, ctx, sf, false, false};
    frame.plan = plan;
    TFRegionEntry entry;
    _Static_assert(sizeof(entry) == sizeof(lease.entry), "ARM64 function pointer ABI");
    memcpy(&entry, &lease.entry, sizeof(entry));
    entry(&frame, plan->chained ? (const char *)lease.entry + plan->entry[offset] - 1 - 16 : NULL);
    *stack = frame.sp;
    *pc = frame.pc;
    if (tf_region_collect_metrics) {
        plan->calls++;
        plan->completed += frame.completed;
        if (!plan->chained && frame.completed < plan->steps[offset]) plan->guards++;
        if (frame.quota_exit) plan->quota_exits++;
        if (frame.threw) plan->exceptions++;
    }
    if (!native_code_pool_leave(plan->pool, lease)) abort(); /* fixture invariant */
    return frame.threw;
}

#if defined(TF_REGION_FRAME_LEASE) && !defined(TF_REGION_INTERPRETER_ONLY)
typedef struct {
    TFRegionPlan *plan;
    TFRegionFrame frame;
    NativeCodeLease lease;
    TFRegionEntry entry;
    bool admission_only;
    bool pointer_leases;
} TFRegionActivation;

static bool tf_region_activation_init(TFRegionActivation *a, TFRegionPlan *plan,
    JSContext *ctx, JSFunctionBytecode *body, JSValue *locals, JSValue *args,
    JSVarRef **refs, JSValue *base, JSStackFrame *sf)
{
    a->pointer_leases = plan->pointer_leases;
    bool entered = a->pointer_leases ?
        native_code_pool_enter_ref(plan->pool, &plan->image, 16, &a->lease) :
        native_code_pool_enter(plan->pool, plan->image, 16, &a->lease);
    if (!entered) {
        plan->lease_refusals++;
        return false;
    }
    plan->lease_acquisitions++;
    plan->active_frames++;
    a->plan = plan;
    a->admission_only = plan->admission_only;
    a->frame = (TFRegionFrame){body, locals, args, base, base + body->stack_size,
        base, refs, body->byte_code_buf, 0, ctx, sf, false, false};
    a->frame.plan = plan;
    memcpy(&a->entry, &a->lease.entry, sizeof(a->entry));
    return true;
}
static void tf_region_activation_close(TFRegionActivation *a)
{
    if (!a) return;
    TFRegionPlan *plan = a->plan;
    if (!plan->active_frames) abort();
    bool left = a->pointer_leases ? native_code_pool_leave_ref(plan->pool, &a->lease) :
        native_code_pool_leave(plan->pool, a->lease);
    if (!left) abort();
    plan->active_frames--;
}
static bool tf_region_activation_resume(TFRegionActivation *a, unsigned offset,
#ifdef TF_REGION_TABLE_DISPATCH
                                        JSValue *stack, const uint8_t *pc)
#else
                                        JSValue **stack, const uint8_t **pc)
#endif
{
    TFRegionPlan *plan = a->plan;
    TFRegionFrame *f = &a->frame;
#ifdef TF_REGION_TABLE_DISPATCH
    f->sp = stack;
    f->pc = pc;
#else
    f->sp = *stack;
    f->pc = *pc;
#endif
    f->completed = 0;
    f->threw = f->quota_exit = false;
    f->returned = false;
    f->result = JS_UNDEFINED;
    a->entry(f, (const char *)a->lease.entry + plan->entry[offset] - 1 - 16);
#ifndef TF_REGION_TABLE_DISPATCH
    *stack = f->sp;
    *pc = f->pc;
#endif
    if (tf_region_collect_metrics) {
        plan->calls++;
        plan->completed += f->completed;
        if (f->quota_exit) plan->quota_exits++;
        if (f->threw) plan->exceptions++;
        if (f->returned) plan->returns++;
    }
    return f->threw;
}
/* The experimental plan owner may retire/disable an image while a frame is
   live, but must not free plan metadata until active_frames reaches zero.
   This is not yet a browser-owned cache or teardown integration. */
#define TF_REGION_DECLARE TFRegionActivation *tf_activation = NULL
#define TF_REGION_RETURN do { \
    if (tf_activation->frame.returned) { \
        ret_val = tf_activation->frame.result; \
        tf_activation->frame.result = JS_UNDEFINED; \
        goto done; \
    } \
} while (0)
#ifdef TF_REGION_TABLE_DISPATCH
/* Do not take the addresses of interpreter pc/sp at the shared dispatch label.
   That forced both locals into memory throughout the disabled interpreter in
   the measured pointer-argument version. Only copy values at this boundary. */
#define TF_REGION_SELECT_TABLE do { \
    if (tf_activation && !tf_activation->admission_only) tf_dispatch_table = tf_native_dispatch_table; \
} while (0)
#define TF_REGION_RESUME(offset) do { \
    bool tf_threw = tf_region_activation_resume(tf_activation, offset, sp, pc); \
    sp = tf_activation->frame.sp; pc = tf_activation->frame.pc; \
    if (tf_threw) goto exception; \
    TF_REGION_RETURN; \
} while (0)
#else
#define TF_REGION_SELECT_TABLE ((void)0)
#define TF_REGION_RESUME(offset) do { \
    if (tf_region_activation_resume(tf_activation, offset, &sp, &pc)) goto exception; \
    TF_REGION_RETURN; \
} while (0)
#endif
#ifdef TF_REGION_LIVE_ENGINE
#define TF_REGION_CANDIDATE tf_region_live_select(ctx, b, sf->cur_func)
#define TF_REGION_REMEMBER(candidate) (tf_region_plan = (candidate))
#else
#define TF_REGION_CANDIDATE tf_region_fixture_select(b)
#define TF_REGION_REMEMBER(candidate) ((void)0)
#endif
#define TF_REGION_INIT do { \
    TFRegionPlan *tf_candidate = TF_REGION_CANDIDATE; \
    TF_REGION_REMEMBER(tf_candidate); \
    if (tf_candidate && tf_candidate->enabled && tf_candidate->chained && \
        tf_candidate->body == b && b->func_kind == JS_FUNC_NORMAL && \
        !(flags & ~JS_CALL_FLAG_COPY_ARGV) && !ctx->host_retired && !tf_region_census_active(ctx) && \
        !js_check_stack_overflow(rt, sizeof(TFRegionActivation))) { \
        TFRegionActivation *tf_scratch = alloca(sizeof(*tf_scratch)); \
        if (tf_region_activation_init(tf_scratch, tf_candidate, ctx, b, var_buf, \
                arg_buf, var_refs, stack_buf, sf)) tf_activation = tf_scratch; \
    } \
    TF_REGION_SELECT_TABLE; \
} while (0)
#define TF_REGION_CLOSE tf_region_activation_close(tf_activation)
#define TF_REGION_DISPATCH(context, bytecode, cursor, ...) do { \
    if (unlikely(tf_activation && !tf_activation->admission_only && \
        tf_activation->plan->enabled && !(context)->host_retired)) { \
        size_t tf_offset = (size_t)((cursor) - (bytecode)->byte_code_buf); \
        if (tf_offset < (size_t)(bytecode)->byte_code_len && tf_activation->plan->entry[tf_offset]) \
            TF_REGION_RESUME(tf_offset); \
    } \
} while (0)
#elif defined(TF_REGION_INTERPRETER_ONLY)
/* Compile a genuinely unhooked control, not merely a disabled runtime flag. */
#define TF_REGION_DECLARE
#define TF_REGION_INIT ((void)0)
#define TF_REGION_CLOSE ((void)0)
#define TF_REGION_DISPATCH(...) ((void)0)
#else
#define TF_REGION_DECLARE
#define TF_REGION_INIT ((void)0)
#define TF_REGION_CLOSE ((void)0)
/* Reject before materializing call arguments and spilling the interpreter PC
   and stack. The first measured hook called enter even with no active plan. */
#define TF_REGION_DISPATCH(context, bytecode, cursor, ...) do { \
    if (unlikely(tf_region_plan && tf_region_plan->enabled && tf_region_plan->body == (bytecode))) { \
        size_t tf_offset = (size_t)((cursor) - (bytecode)->byte_code_buf); \
        if (tf_offset < (size_t)(bytecode)->byte_code_len && tf_region_plan->entry[tf_offset]) \
            if (tf_region_enter(context, bytecode, __VA_ARGS__)) goto exception; \
    } \
} while (0)
#endif
