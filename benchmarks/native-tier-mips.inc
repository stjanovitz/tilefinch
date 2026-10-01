/* Experimental Allegrex/o32 emitter. Included after QuickJS internals and the
   shared region semantics. It lowers bytecode, never source/site identities.
   s0=handoff, s1=owned stack, s2=bytecode, s3=completed, s4=current offset.
   Optional s5=owned binding slot, s6/s7=its tagged value.
   Every branch has an explicit inert delay slot; no partial operation is
   committed before a refusal guard. Unsupported cases resume the interpreter. */
#if !defined(__PSP__) || !defined(__MIPSEL__) || !defined(JS_NAN_BOXING)
#error This backend requires the PSP little-endian o32 value ABI
#endif
_Static_assert(sizeof(JSValue) == 8 && sizeof(void *) == 4, "o32 value ABI");
_Static_assert(sizeof(JSRefCountHeader) == 4, "reference prefix ABI");

#define TF_CHAIN_FIXUP_LIMIT 2048u
#define TF_CHAIN_EXIT UINT16_MAX
#define TF_CHAIN_COLD_LIMIT 128u
#define TF_CHAIN_MISS_LIMIT 24u
typedef struct {
    unsigned count;
    struct { uint16_t word, target; } items[TF_CHAIN_FIXUP_LIMIT];
    /* Compile-only scratch, charged with the existing fixed emission buffer. */
    unsigned cold_count;
    struct { uint16_t start, end, miss_count, misses[TF_CHAIN_MISS_LIMIT]; } cold[TF_CHAIN_COLD_LIMIT];
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    TFBorrowedCFG cfg; /* charged compile scratch, never retained by a page */
#endif
} TFChainFixups;

enum { M_ZERO=0, M_V0=2, M_V1=3, M_A0=4, M_A1=5, M_A2=6, M_T0=8, M_T1=9,
       M_T2=10, M_T3=11, M_T4=12, M_T5=13, M_T6=14, M_T7=15, M_T8=24, M_T9=25, M_S0=16, M_S1=17,
       M_S2=18, M_S3=19, M_S4=20, M_S5=21, M_S6=22, M_S7=23,
       M_SP=29, M_RA=31 };

/* The source binding retains its receiver throughout this no-GC/no-callback
   operation. Refusal changes no ownership; success duplicates only the result.
   Do not extend this borrowing contract to getters, proxies or finalizers. */
static JSValue mips_borrow_field(JSValueConst receiver, uint32_t atom)
{
    if (!JS_IsObject(receiver)) return JS_EXCEPTION;
    JSObject *object = JS_VALUE_GET_OBJ(receiver);
    if (object->is_exotic) return JS_EXCEPTION;
    JSProperty *property;
    JSShapeProperty *shape = find_own_property(&property, object, atom);
    if (!shape || (shape->flags & JS_PROP_TMASK)) return JS_EXCEPTION;
    return JS_DupValueRT(NULL, property->u.value);
}

/* Keep complex property semantics inside the activation, rather than bouncing
   out to the opcode dispatcher. The real QuickJS frame still owns all values
   and supplies the exception location. No property/getter is cached. */
static int mips_field_semantic(TFRegionFrame *f, uint32_t atom, uint32_t next)
{
    if (f->sp == f->base || !f->ctx || !f->frame) return 0;
    JSValue receiver = f->sp[-1], result;
    f->frame->cur_pc = f->body->byte_code_buf + next;
    if (JS_IsObject(receiver)) {
        JSObject *object = JS_VALUE_GET_OBJ(receiver);
        for (unsigned depth = 0; depth < 64; depth++) {
            JSProperty *property;
            JSShapeProperty *shape = find_own_property(&property, object, atom);
            if (shape) {
                if (!(shape->flags & JS_PROP_TMASK)) {
                    result = JS_DupValue(f->ctx, property->u.value);
                    goto found;
                }
                if ((shape->flags & JS_PROP_TMASK) == JS_PROP_GETSET) {
                    result = js_call_property_getter(f->ctx, property, receiver);
                    goto found;
                }
                break;
            }
            if (object->is_exotic) break;
            object = object->shape->proto;
            if (!object) { result = JS_UNDEFINED; goto found; }
        }
    }
    result = JS_GetPropertyInternal(f->ctx, receiver, atom, receiver, 0);
found:
    if (JS_IsException(result)) {
        f->pc = f->body->byte_code_buf + next;
        f->threw = true;
        return 0; /* throwing access leaves its receiver to the unwinder */
    }
    JS_FreeValue(f->ctx, receiver);
    f->sp[-1] = result;
    return tf_region_done(f, next);
}
static void mips_i(Emitter *e, unsigned opcode, unsigned to, unsigned from, int immediate)
{
    if (to > 31 || from > 31 || immediate < INT16_MIN || immediate > INT16_MAX) {
        e->refused = true; return;
    }
    emit(e, (opcode << 26) | (from << 21) | (to << 16) | (uint16_t)immediate);
}
static void mips_r(Emitter *e, unsigned function, unsigned to, unsigned a, unsigned b)
{
    if (to > 31 || a > 31 || b > 31) { e->refused = true; return; }
    emit(e, (a << 21) | (b << 16) | (to << 11) | function);
}
static void immediate32(Emitter *e, unsigned reg, uint32_t value)
{
    if (reg > 31) { e->refused = true; return; }
    if ((int32_t)value >= INT16_MIN && (int32_t)value <= INT16_MAX) {
        mips_i(e, 9, reg, M_ZERO, (int32_t)value); /* addiu */
    } else {
        emit(e, 0x3c000000u | (reg << 16) | (value >> 16)); /* lui */
        if (value & 0xffffu)
            emit(e, 0x34000000u | (reg << 21) | (reg << 16) | (value & 0xffffu));
    }
}
static void mips_load(Emitter *e, unsigned to, unsigned base, size_t offset)
{
    if (offset > INT16_MAX || offset % 4) { e->refused = true; return; }
    mips_i(e, 35, to, base, (int)offset);
}
static void mips_store(Emitter *e, unsigned value, unsigned base, size_t offset)
{
    if (offset > INT16_MAX || offset % 4) { e->refused = true; return; }
    mips_i(e, 43, value, base, (int)offset);
}
static unsigned mips_branch(Emitter *e, bool equal, unsigned a, unsigned b)
{
    unsigned at = e->count;
    emit(e, ((equal ? 4u : 5u) << 26) | (a << 21) | (b << 16));
    emit(e, 0); /* delay slot */
    return at;
}
static bool mips_patch(Emitter *e, unsigned at, unsigned destination)
{
    int64_t delta = (int64_t)destination - at - 1;
    if (e->refused || at >= e->count || delta < INT16_MIN || delta > INT16_MAX)
        return false;
    e->words[at] |= (uint16_t)delta;
    return true;
}
static void mips_fix(Emitter *e, TFChainFixups *fix, unsigned at, unsigned target)
{
    if (e->refused || fix->count == TF_CHAIN_FIXUP_LIMIT || at >= TF_REGION_CODE_LIMIT / 4 ||
        (target >= TF_REGION_BYTECODE_LIMIT && target != TF_CHAIN_EXIT)) {
        e->refused = true; return;
    }
    fix->items[fix->count].word = (uint16_t)at;
    fix->items[fix->count++].target = (uint16_t)target;
}
static void mips_exit_if(Emitter *e, TFChainFixups *fix, bool equal, unsigned a, unsigned b)
{
    mips_fix(e, fix, mips_branch(e, equal, a, b), TF_CHAIN_EXIT);
}
static void mips_jump(Emitter *e, TFChainFixups *fix, unsigned target)
{
    mips_fix(e, fix, mips_branch(e, true, M_ZERO, M_ZERO), target);
}
static void mips_stack_guard(Emitter *e, TFChainFixups *fix, bool push)
{
    mips_load(e, M_T2, M_S0, push ? offsetof(TFRegionFrame, limit) : offsetof(TFRegionFrame, base));
    mips_exit_if(e, fix, true, M_S1, M_T2);
}
/* The supplied registers hold payload/tag. Final release remains interpreter-owned;
   it can invoke finalizers, allocation and arbitrary author code. */
static void mips_reference(Emitter *e, TFChainFixups *fix, bool duplicate,
                           unsigned payload, unsigned tag)
{
    immediate32(e, M_T3, (uint32_t)JS_TAG_FIRST);
    mips_r(e, 43, M_T2, tag, M_T3); /* sltu: immediate tag */
    unsigned immediate = mips_branch(e, false, M_T2, M_ZERO);
    mips_i(e, 35, M_T2, payload, -4);
    if (!duplicate) {
        mips_i(e, 10, M_T3, M_T2, 2); /* slti count,2 */
        mips_exit_if(e, fix, false, M_T3, M_ZERO);
    }
    mips_i(e, 9, M_T2, M_T2, duplicate ? 1 : -1);
    mips_i(e, 43, M_T2, payload, -4);
    if (!mips_patch(e, immediate, e->count)) e->refused = true;
}
static void mips_flush(Emitter *e)
{
    /* The register pair owns the slot's value without an extra reference.
       Publish before every re-entrant/GC-capable helper and every exit. */
    if (e->binding_kind) {
        mips_store(e, M_S6, M_S5, 0);
        mips_store(e, M_S7, M_S5, 4);
    }
    mips_store(e, M_S1, M_S0, offsetof(TFRegionFrame, sp));
    mips_r(e, 33, M_T2, M_S2, M_S4);
    mips_store(e, M_T2, M_S0, offsetof(TFRegionFrame, pc));
    mips_store(e, M_S3, M_S0, offsetof(TFRegionFrame, completed));
}
static void mips_prologue(Emitter *e)
{
    unsigned saved = e->binding_kind || e->full_save ? 8 : 5;
    unsigned bytes = e->binding_kind || e->full_save ? 56 : 40;
    mips_i(e, 9, M_SP, M_SP, -(int)bytes);
    for (unsigned i = 0; i < saved; i++) mips_store(e, M_S0+i, M_SP, 16 + i*4);
    mips_store(e, M_RA, M_SP, 16 + saved*4);
    mips_r(e, 33, M_S0, M_A0, M_ZERO);
    mips_load(e, M_S1, M_S0, offsetof(TFRegionFrame, sp));
    mips_load(e, M_T2, M_S0, offsetof(TFRegionFrame, body));
    mips_load(e, M_S2, M_T2, offsetof(JSFunctionBytecode, byte_code_buf));
    mips_load(e, M_S3, M_S0, offsetof(TFRegionFrame, completed));
    if (e->binding_kind) {
        mips_load(e, M_S5, M_S0, e->binding_kind == 1 ?
            offsetof(TFRegionFrame, locals) : offsetof(TFRegionFrame, args));
        mips_i(e, 9, M_S5, M_S5, e->binding_index * 8);
        mips_load(e, M_S6, M_S5, 0);
        mips_load(e, M_S7, M_S5, 4);
    }
    emit(e, (M_A1 << 21) | 8u); /* jr already-validated block address */
    emit(e, 0);
}
static void mips_epilogue(Emitter *e)
{
    mips_flush(e);
    unsigned saved = e->binding_kind || e->full_save ? 8 : 5;
    for (unsigned i = 0; i < saved; i++) mips_load(e, M_S0+i, M_SP, 16 + i*4);
    mips_load(e, M_RA, M_SP, 16 + saved*4);
    mips_i(e, 9, M_SP, M_SP, e->binding_kind || e->full_save ? 56 : 40);
    emit(e, (M_RA << 21) | 8u);
    emit(e, 0);
}
static void mips_helper(Emitter *e, TFChainFixups *fix, TFRegionHelper helper,
                        uint32_t argument, unsigned next)
{
    /* Full owned state is visible before a semantic call can re-enter or GC. */
    mips_flush(e);
    mips_r(e, 33, M_A0, M_S0, M_ZERO);
    immediate32(e, M_A1, argument);
    immediate32(e, M_A2, next);
    uintptr_t address;
    _Static_assert(sizeof(address) == sizeof(helper), "o32 helper pointer ABI");
    memcpy(&address, &helper, sizeof(address));
    immediate32(e, M_T9, (uint32_t)address);
    emit(e, (M_T9 << 21) | (M_RA << 11) | 9u); /* jalr */
    emit(e, 0);
    mips_load(e, M_S1, M_S0, offsetof(TFRegionFrame, sp));
    mips_load(e, M_S3, M_S0, offsetof(TFRegionFrame, completed));
    mips_load(e, M_T2, M_S0, offsetof(TFRegionFrame, pc));
    mips_r(e, 35, M_S4, M_T2, M_S2); /* subu */
    /* A getter/callback may mutate a captured local or mapped argument. */
    if (e->binding_kind) {
        mips_load(e, M_S6, M_S5, 0);
        mips_load(e, M_S7, M_S5, 4);
    }
    mips_exit_if(e, fix, true, M_V0, M_ZERO);
}

static void mips_field_value(Emitter *e, TFChainFixups *fix, uint32_t atom, unsigned next)
{
    mips_stack_guard(e, fix, false);
    /* o32 returns an eight-byte JSValue in v0/v1. The own-data helper cannot
       allocate, throw, poll, re-enter or destroy a final reference, so the
       owned stack may stay in registers until its result is committed. */
    mips_i(e, 35, M_A0, M_S1, -8);
    mips_i(e, 35, M_A1, M_S1, -4);
    immediate32(e, M_A2, atom);
    JSValue (*helper)(JSValue, uint32_t) = tf_region_own_value;
    uintptr_t address;
    _Static_assert(sizeof(address) == sizeof(helper), "o32 value-return helper ABI");
    memcpy(&address, &helper, sizeof(address));
    immediate32(e, M_T9, (uint32_t)address);
    emit(e, (M_T9 << 21) | (M_RA << 11) | 9u);
    emit(e, 0);
    immediate32(e, M_T0, JS_TAG_EXCEPTION);
    unsigned refused = mips_branch(e, true, M_V1, M_T0);
    mips_i(e, 43, M_V0, M_S1, -8);
    mips_i(e, 43, M_V1, M_S1, -4);
    mips_i(e, 9, M_S3, M_S3, 1);
    unsigned succeeded = mips_branch(e, true, M_ZERO, M_ZERO);
    if (!mips_patch(e, refused, e->count)) e->refused = true;
    mips_helper(e, fix, mips_field_semantic, atom, next);
    if (!mips_patch(e, succeeded, e->count)) e->refused = true;
}
static bool mips_fused_field(Emitter *e, TFChainFixups *fix, TFRegionPlan *plan,
                              TFRegionHelper source, uint32_t index,
                              uint32_t atom, unsigned next)
{
    JSFunctionBytecode *body = plan->body;
    bool local = source == tf_region_local || source == tf_region_local_check;
    bool arg = source == tf_region_arg;
    bool ref = source == tf_region_ref || source == tf_region_ref_check;
    bool constant = source == tf_region_constant;
    unsigned limit = local ? body->var_count : arg ? body->arg_count :
        ref ? body->closure_var_count : constant ? body->cpool_count : 0;
    if (!fused_fields || index >= limit || index > 255) return false;
    mips_stack_guard(e, fix, true); /* before duplicating the field result */
    bool cached = index == e->binding_index &&
        ((local && e->binding_kind == 1) || (arg && e->binding_kind == 2));
    if (cached) {
        mips_r(e, 33, M_A0, M_S6, M_ZERO);
        mips_r(e, 33, M_A1, M_S7, M_ZERO);
    } else {
        if (local || arg) {
            mips_load(e, M_T4, M_S0, local ? offsetof(TFRegionFrame, locals) : offsetof(TFRegionFrame, args));
            mips_i(e, 9, M_T4, M_T4, index * 8);
        } else if (ref) {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, refs));
            mips_load(e, M_T4, M_T4, index * 4);
            mips_load(e, M_T4, M_T4, offsetof(JSVarRef, pvalue));
        } else {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, body));
            mips_load(e, M_T4, M_T4, offsetof(JSFunctionBytecode, cpool));
            mips_i(e, 9, M_T4, M_T4, index * 8);
        }
        mips_load(e, M_A0, M_T4, 0);
        mips_load(e, M_A1, M_T4, 4);
    }
    immediate32(e, M_A2, atom);
    JSValue (*helper)(JSValueConst, uint32_t) = mips_borrow_field;
    uintptr_t address;
    _Static_assert(sizeof(address) == sizeof(helper), "o32 borrowed helper ABI");
    memcpy(&address, &helper, sizeof(address));
    immediate32(e, M_T9, (uint32_t)address);
    emit(e, (M_T9 << 21) | (M_RA << 11) | 9u);
    emit(e, 0);
    immediate32(e, M_T0, JS_TAG_EXCEPTION);
    unsigned refused = mips_branch(e, true, M_V1, M_T0);
    mips_store(e, M_V0, M_S1, 0);
    mips_store(e, M_V1, M_S1, 4);
    mips_i(e, 9, M_S1, M_S1, 8);
    mips_i(e, 9, M_S3, M_S3, 2);
    immediate32(e, M_S4, next);
    mips_jump(e, fix, next);
    if (!mips_patch(e, refused, e->count)) e->refused = true;
    plan->fused_fields++;
    return true; /* failure falls through the original two operations */
}
static bool mips_direct(Emitter *e, TFChainFixups *fix, JSFunctionBytecode *body,
                        TFRegionHelper helper, uint32_t argument, unsigned at)
{
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    if (continuation_values && helper == tf_region_semantic) {
        unsigned op = body->byte_code_buf[at];
        unsigned index = op == OP_set_loc_uninitialized ?
            get_u16(body->byte_code_buf + at + 1) : 0;
        if (op == OP_set_loc_uninitialized && (index >= body->var_count || index > 255))
            return false;
        /* The semantic helper checks the quantum at every operation. Keep
           that exact refusal before any ownership or stack change. */
        mips_i(e, 11, M_T0, M_S3, TF_REGION_QUANTUM);
        unsigned admitted = mips_branch(e, false, M_T0, M_ZERO);
        immediate32(e, M_T0, 1);
        mips_i(e, 40, M_T0, M_S0, offsetof(TFRegionFrame, quota_exit));
        mips_jump(e, fix, TF_CHAIN_EXIT);
        if (!mips_patch(e, admitted, e->count)) e->refused = true;
        if (op == OP_set_loc_uninitialized) {
            bool cached = e->binding_kind == 1 && index == e->binding_index;
            unsigned payload = cached ? M_S6 : M_T0;
            unsigned tag = cached ? M_S7 : M_T1;
            if (!cached) {
                mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, locals));
                mips_i(e, 9, M_T4, M_T4, index * 8);
                mips_load(e, M_T0, M_T4, 0);
                mips_load(e, M_T1, M_T4, 4);
            }
            mips_reference(e, fix, false, payload, tag);
            immediate32(e, payload, 0);
            immediate32(e, tag, JS_TAG_UNINITIALIZED);
            if (!cached) {
                mips_store(e, M_T0, M_T4, 0);
                mips_store(e, M_T1, M_T4, 4);
            }
        } else if (op == OP_strict_eq || op == OP_strict_neq) {
            mips_load(e, M_T2, M_S0, offsetof(TFRegionFrame, base));
            mips_i(e, 9, M_T2, M_T2, 16);
            mips_r(e, 43, M_T3, M_S1, M_T2);
            mips_exit_if(e, fix, false, M_T3, M_ZERO);
            mips_i(e, 35, M_T0, M_S1, -16);
            mips_i(e, 35, M_T1, M_S1, -12);
            mips_i(e, 35, M_T2, M_S1, -8);
            mips_i(e, 35, M_T3, M_S1, -4);
            /* Only int/bool/null/undefined: floats, strings, objects, symbols
               and BigInts stay with the interpreter before any effects. */
            mips_i(e, 11, M_T4, M_T1, JS_TAG_UNDEFINED + 1);
            mips_exit_if(e, fix, true, M_T4, M_ZERO);
            mips_i(e, 11, M_T4, M_T3, JS_TAG_UNDEFINED + 1);
            mips_exit_if(e, fix, true, M_T4, M_ZERO);
            mips_r(e, 38, M_T4, M_T1, M_T3); /* xor tags */
            mips_i(e, 11, M_T4, M_T4, 1); /* same kind */
            mips_r(e, 38, M_T0, M_T0, M_T2);
            mips_i(e, 11, M_T0, M_T0, 1); /* same payload */
            mips_i(e, 11, M_T2, M_T1, JS_TAG_NULL);
            mips_i(e, 14, M_T2, M_T2, 1); /* null/undefined ignore payload */
            mips_r(e, 37, M_T0, M_T0, M_T2);
            mips_r(e, 36, M_T0, M_T0, M_T4);
            if (op == OP_strict_neq) mips_i(e, 14, M_T0, M_T0, 1);
            immediate32(e, M_T1, JS_TAG_BOOL);
            mips_i(e, 43, M_T0, M_S1, -16);
            mips_i(e, 43, M_T1, M_S1, -12);
            mips_i(e, 9, M_S1, M_S1, -8);
        } else {
            mips_stack_guard(e, fix, false);
            mips_i(e, 35, M_T0, M_S1, -8);
            mips_i(e, 35, M_T1, M_S1, -4);
            if (op == OP_lnot) {
                mips_i(e, 11, M_T2, M_T1, JS_TAG_UNDEFINED + 1);
                mips_exit_if(e, fix, true, M_T2, M_ZERO);
                mips_i(e, 11, M_T0, M_T0, 1); /* int/bool == 0 */
                mips_i(e, 11, M_T2, M_T1, JS_TAG_NULL);
                mips_i(e, 14, M_T2, M_T2, 1);
                mips_r(e, 37, M_T0, M_T0, M_T2);
            } else {
                /* The tag check does not coerce; decrement only a non-final
                   reference, with all refusal guards before the decrement. */
                mips_reference(e, fix, false, M_T0, M_T1);
                if (op == OP_is_undefined_or_null) {
                    mips_i(e, 9, M_T0, M_T1, -JS_TAG_NULL);
                    mips_i(e, 11, M_T0, M_T0, 2);
                } else {
                    unsigned tag = op == OP_is_null ? JS_TAG_NULL : JS_TAG_UNDEFINED;
                    mips_i(e, 14, M_T0, M_T1, tag);
                    mips_i(e, 11, M_T0, M_T0, 1);
                }
            }
            immediate32(e, M_T1, JS_TAG_BOOL);
            mips_i(e, 43, M_T0, M_S1, -8);
            mips_i(e, 43, M_T1, M_S1, -4);
        }
        mips_i(e, 9, M_S3, M_S3, 1);
        return true;
    }
#else
    (void)at;
#endif
    bool checked = helper == tf_region_local_check || helper == tf_region_ref_check;
    bool local = helper == tf_region_local || helper == tf_region_local_check;
    bool arg = helper == tf_region_arg;
    bool ref = helper == tf_region_ref || helper == tf_region_ref_check;
    bool constant = helper == tf_region_constant;
    if (local || arg || ref || constant) {
        unsigned limit = local ? body->var_count : arg ? body->arg_count :
            ref ? body->closure_var_count : body->cpool_count;
        if (argument >= limit || argument > 255) return false;
        mips_stack_guard(e, fix, true);
        bool cached = argument == e->binding_index &&
            ((local && e->binding_kind == 1) || (arg && e->binding_kind == 2));
        unsigned payload = cached ? M_S6 : M_T0;
        unsigned tag = cached ? M_S7 : M_T1;
        if (cached) {
            /* No frame read; the common helper/exit barrier publishes it. */
        } else if (local || arg) {
            mips_load(e, M_T4, M_S0, local ? offsetof(TFRegionFrame, locals) : offsetof(TFRegionFrame, args));
            mips_i(e, 9, M_T4, M_T4, argument * 8);
        } else if (ref) {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, refs));
            mips_load(e, M_T4, M_T4, argument * 4);
            mips_load(e, M_T4, M_T4, offsetof(JSVarRef, pvalue));
        } else {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, body));
            mips_load(e, M_T4, M_T4, offsetof(JSFunctionBytecode, cpool));
            mips_i(e, 9, M_T4, M_T4, argument * 8);
        }
        if (!cached) {
            mips_load(e, M_T0, M_T4, 0);
            mips_load(e, M_T1, M_T4, 4);
        }
        if (checked) {
            immediate32(e, M_T3, JS_TAG_UNINITIALIZED);
            mips_exit_if(e, fix, true, tag, M_T3);
        }
        mips_reference(e, fix, true, payload, tag);
        mips_store(e, payload, M_S1, 0);
        mips_store(e, tag, M_S1, 4);
        mips_i(e, 9, M_S1, M_S1, 8);
    } else if (helper == tf_region_integer || helper == tf_region_immediate) {
        mips_stack_guard(e, fix, true);
        unsigned tag = JS_TAG_INT, value = argument;
        if (helper == tf_region_immediate) {
            tag = argument == OP_null ? JS_TAG_NULL : argument == OP_undefined ? JS_TAG_UNDEFINED : JS_TAG_BOOL;
            value = argument == OP_push_true;
        }
        immediate32(e, M_T0, value);
        immediate32(e, M_T1, tag);
        mips_store(e, M_T0, M_S1, 0);
        mips_store(e, M_T1, M_S1, 4);
        mips_i(e, 9, M_S1, M_S1, 8);
    } else if (helper == tf_region_drop || helper == tf_region_dup) {
        mips_stack_guard(e, fix, false);
        if (helper == tf_region_dup) mips_stack_guard(e, fix, true);
        mips_i(e, 35, M_T0, M_S1, -8);
        mips_i(e, 35, M_T1, M_S1, -4);
        mips_reference(e, fix, helper == tf_region_dup, M_T0, M_T1);
        if (helper == tf_region_dup) {
            mips_store(e, M_T0, M_S1, 0);
            mips_store(e, M_T1, M_S1, 4);
        }
        mips_i(e, 9, M_S1, M_S1, helper == tf_region_dup ? 8 : -8);
    } else if (helper == tf_region_put_local || helper == tf_region_put_local_check) {
        if (argument >= body->var_count || argument > 255) return false;
        mips_stack_guard(e, fix, false);
        bool cached = e->binding_kind == 1 && argument == e->binding_index;
        unsigned payload = cached ? M_S6 : M_T0;
        unsigned tag = cached ? M_S7 : M_T1;
        if (!cached) {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, locals));
            mips_i(e, 9, M_T4, M_T4, argument * 8);
            mips_load(e, M_T0, M_T4, 0);
            mips_load(e, M_T1, M_T4, 4);
        }
        if (helper == tf_region_put_local_check) {
            immediate32(e, M_T3, JS_TAG_UNINITIALIZED);
            mips_exit_if(e, fix, true, tag, M_T3);
        }
        mips_reference(e, fix, false, payload, tag);
        mips_i(e, 35, payload, M_S1, -8);
        mips_i(e, 35, tag, M_S1, -4);
        if (!cached) {
            mips_store(e, M_T0, M_T4, 0);
            mips_store(e, M_T1, M_T4, 4);
        }
        mips_i(e, 9, M_S1, M_S1, -8);
    } else if (helper == tf_region_increment) {
        if (argument >= body->var_count || argument > 255) return false;
        bool cached = e->binding_kind == 1 && argument == e->binding_index;
        unsigned payload = cached ? M_S6 : M_T0;
        unsigned tag = cached ? M_S7 : M_T1;
        if (!cached) {
            mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, locals));
            mips_i(e, 9, M_T4, M_T4, argument * 8);
            mips_load(e, M_T0, M_T4, 0);
            mips_load(e, M_T1, M_T4, 4);
        }
        mips_exit_if(e, fix, false, tag, M_ZERO); /* int tag is zero */
        immediate32(e, M_T2, INT32_MAX);
        mips_exit_if(e, fix, true, payload, M_T2);
        mips_i(e, 9, payload, payload, 1);
        if (!cached) mips_store(e, M_T0, M_T4, 0);
    } else if (helper == tf_region_less) {
        mips_load(e, M_T2, M_S0, offsetof(TFRegionFrame, base));
        mips_i(e, 9, M_T2, M_T2, 16);
        mips_r(e, 43, M_T3, M_S1, M_T2);
        mips_exit_if(e, fix, false, M_T3, M_ZERO);
        mips_i(e, 35, M_T0, M_S1, -12);
        mips_i(e, 35, M_T1, M_S1, -4);
        mips_r(e, 37, M_T2, M_T0, M_T1); /* or tags */
        mips_exit_if(e, fix, false, M_T2, M_ZERO);
        mips_i(e, 35, M_T0, M_S1, -16);
        mips_i(e, 35, M_T1, M_S1, -8);
        mips_r(e, 42, M_T2, M_T0, M_T1); /* signed slt */
        mips_i(e, 43, M_T2, M_S1, -16);
        immediate32(e, M_T1, JS_TAG_BOOL);
        mips_i(e, 43, M_T1, M_S1, -12);
        mips_i(e, 9, M_S1, M_S1, -8);
    } else return false;
    mips_i(e, 9, M_S3, M_S3, 1);
    return true;
}

static void mips_fast_branch(Emitter *e, TFChainFixups *fix, unsigned opcode,
                              unsigned target, unsigned next, bool conditional)
{
    /* Same poll and quota points as the interpreter. Refuse BEFORE consuming
       the condition or counter when the slow interrupt path is required. */
    mips_i(e, 11, M_T0, M_S3, TF_REGION_QUANTUM); /* sltiu */
    unsigned admitted = mips_branch(e, false, M_T0, M_ZERO);
    immediate32(e, M_T0, 1);
    mips_i(e, 40, M_T0, M_S0, offsetof(TFRegionFrame, quota_exit));
    mips_jump(e, fix, TF_CHAIN_EXIT);
    if (!mips_patch(e, admitted, e->count)) e->refused = true;
    if (conditional) {
        mips_stack_guard(e, fix, false);
        mips_i(e, 35, M_T0, M_S1, -8);
        mips_i(e, 35, M_T1, M_S1, -4);
        immediate32(e, M_T2, JS_TAG_UNDEFINED + 1);
        mips_r(e, 43, M_T3, M_T1, M_T2);
        mips_exit_if(e, fix, true, M_T3, M_ZERO);
    }
    mips_load(e, M_T2, M_S0, offsetof(TFRegionFrame, ctx));
    mips_load(e, M_T3, M_T2, offsetof(JSContext, interrupt_counter));
    mips_i(e, 10, M_T4, M_T3, 2);
    mips_exit_if(e, fix, false, M_T4, M_ZERO);
    mips_i(e, 9, M_T3, M_T3, -1);
    mips_store(e, M_T3, M_T2, offsetof(JSContext, interrupt_counter));
    if (conditional) mips_i(e, 9, M_S1, M_S1, -8);
    mips_i(e, 9, M_S3, M_S3, 1);
    immediate32(e, M_S4, target);
    if (conditional) {
        bool if_true = opcode == OP_if_true || opcode == OP_if_true8;
        mips_fix(e, fix, mips_branch(e, !if_true, M_T0, M_ZERO), target);
        immediate32(e, M_S4, next);
        mips_jump(e, fix, next);
    } else mips_jump(e, fix, target);
}
static bool mips_select_binding(Emitter *e, TFRegionPlan *plan)
{
    if (!cached_bindings) return true;
    /* Compile-only, fixed scratch. Scores describe eliminated slot traffic,
       not application identities. Refuse under stack pressure. */
    if (js_check_stack_overflow(plan->body->realm->rt, 2048)) return false;
    uint16_t scores[2][256] = {{0}};
    JSFunctionBytecode *body = plan->body;
    for (unsigned at = 0; at < (unsigned)body->byte_code_len;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        if (!size || size > (unsigned)body->byte_code_len - at) return false;
        uint32_t argument = 0;
        TFRegionHelper helper = tf_region_decode(body->byte_code_buf + at, &argument);
        if (argument < 256) {
            if ((helper == tf_region_local || helper == tf_region_local_check) && argument < body->var_count)
                scores[0][argument] += 4;
            else if ((helper == tf_region_put_local || helper == tf_region_put_local_check ||
                      helper == tf_region_increment) && argument < body->var_count)
                scores[0][argument] += 6;
            else if (helper == tf_region_arg && argument < body->arg_count)
                scores[1][argument] += 4;
        }
        at += size;
    }
    unsigned best = 7;
    for (unsigned kind = 0; kind < 2; kind++)
        for (unsigned index = 0; index < 256; index++)
            if (scores[kind][index] > best) {
                best = scores[kind][index];
                e->binding_kind = kind + 1;
                e->binding_index = index;
            }
    plan->binding_kind = e->binding_kind;
    plan->binding_index = e->binding_index;
    return true;
}

/* First basic-block lowering slice: two borrowed integer inputs, comparison,
   conditional branch. No temporary tagged values visit the operand stack and
   no ownership changes until every guard passes. A miss falls through the
   original operations, including their partial-capacity and slow-poll exits.
   Intermediate bytecode entries remain independently callable. */
static void mips_block_source(Emitter *e, TFRegionHelper helper, uint32_t index,
                               unsigned payload, unsigned tag)
{
    if (helper == tf_region_integer) {
        immediate32(e, payload, index);
        immediate32(e, tag, JS_TAG_INT);
        return;
    }
    bool local = helper == tf_region_local || helper == tf_region_local_check;
    bool arg = helper == tf_region_arg;
    bool ref = helper == tf_region_ref || helper == tf_region_ref_check;
    if (index == e->binding_index &&
        ((local && e->binding_kind == 1) || (arg && e->binding_kind == 2))) {
        mips_r(e, 33, payload, M_S6, M_ZERO);
        mips_r(e, 33, tag, M_S7, M_ZERO);
        return;
    }
    if (local || arg) {
        mips_load(e, M_T4, M_S0, local ? offsetof(TFRegionFrame, locals) : offsetof(TFRegionFrame, args));
        mips_i(e, 9, M_T4, M_T4, index * 8);
    } else if (ref) {
        mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, refs));
        mips_load(e, M_T4, M_T4, index * 4);
        mips_load(e, M_T4, M_T4, offsetof(JSVarRef, pvalue));
    } else {
        mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, body));
        mips_load(e, M_T4, M_T4, offsetof(JSFunctionBytecode, cpool));
        mips_i(e, 9, M_T4, M_T4, index * 8);
    }
    mips_load(e, payload, M_T4, 0);
    mips_load(e, tag, M_T4, 4);
}
static bool mips_compare_block(Emitter *e, TFChainFixups *fix,
                                TFRegionPlan *plan, unsigned at, unsigned *end)
{
    if (!basic_blocks || !direct_branches) return false;
    JSFunctionBytecode *body = plan->body;
    unsigned positions[4], cursor = at;
    uint32_t arguments[4]; /* match the decoder's 32-bit operand ABI */
    TFRegionHelper helpers[4];
    unsigned length = body->byte_code_len;
    for (unsigned i = 0; i < 4; i++) {
        if (cursor >= length) return false;
        positions[i] = cursor;
        const uint8_t *pc = body->byte_code_buf + cursor;
        unsigned size = tf_region_opcodes[pc[0]].size;
        if (!size || size > length - cursor) return false;
        helpers[i] = tf_region_decode(pc, &arguments[i]);
        cursor += size;
    }
    if (cursor >= length || !tf_primitive_source_valid(body, helpers[0], arguments[0]) ||
        !tf_primitive_source_valid(body, helpers[1], arguments[1]) ||
        helpers[2] != tf_region_less || helpers[3] != tf_region_branch) return false;
    const uint8_t *branch = body->byte_code_buf + positions[3];
    unsigned opcode = branch[0];
    bool if_true = opcode == OP_if_true || opcode == OP_if_true8;
    if (!if_true && opcode != OP_if_false && opcode != OP_if_false8) return false;
    int32_t delta = opcode == OP_if_true || opcode == OP_if_false ?
        (int32_t)get_u32(branch + 1) : (int8_t)branch[1];
    int64_t target = (int64_t)positions[3] + 1 + delta;
    if (target < 0 || target >= length || !plan->steps[target]) return false;

    if (cold_blocks && fix->cold_count == TF_CHAIN_COLD_LIMIT) {
        e->refused = true;
        return false;
    }
    unsigned misses[4];
    /* Original evaluation temporarily needs two slots. A one-slot frame must
       take the original path and publish its first successful push. */
    mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, limit));
    mips_r(e, 35, M_T4, M_T4, M_S1);
    mips_i(e, 11, M_T4, M_T4, 16);
    misses[0] = mips_branch(e, false, M_T4, M_ZERO);
    mips_block_source(e, helpers[0], arguments[0], M_T0, M_T1);
    mips_block_source(e, helpers[1], arguments[1], M_T2, M_T3);
    mips_r(e, 37, M_T4, M_T1, M_T3);
    misses[1] = mips_branch(e, false, M_T4, M_ZERO); /* integer tags are zero */
    /* At the original branch, three operations have already completed. */
    mips_i(e, 11, M_A2, M_S3, TF_REGION_QUANTUM - 3);
    misses[2] = mips_branch(e, true, M_A2, M_ZERO);
    mips_load(e, M_A0, M_S0, offsetof(TFRegionFrame, ctx));
    mips_load(e, M_A1, M_A0, offsetof(JSContext, interrupt_counter));
    mips_i(e, 10, M_A2, M_A1, 2);
    misses[3] = mips_branch(e, false, M_A2, M_ZERO);

    mips_r(e, 42, M_T0, M_T0, M_T2); /* signed comparison */
    mips_i(e, 9, M_A1, M_A1, -1);
    mips_store(e, M_A1, M_A0, offsetof(JSContext, interrupt_counter));
    mips_i(e, 9, M_S3, M_S3, 4);
    immediate32(e, M_S4, (unsigned)target);
    mips_fix(e, fix, mips_branch(e, !if_true, M_T0, M_ZERO), (unsigned)target);
    immediate32(e, M_S4, cursor);
    mips_jump(e, fix, cursor);
    if (cold_blocks) {
        unsigned record = fix->cold_count++;
        fix->cold[record].start = (uint16_t)at;
        fix->cold[record].end = (uint16_t)cursor;
        fix->cold[record].miss_count = 4;
        for (unsigned i = 0; i < 4; i++) {
            if (misses[i] >= TF_REGION_CODE_LIMIT / 4) e->refused = true;
            fix->cold[record].misses[i] = (uint16_t)misses[i];
        }
        plan->cold_blocks++;
    } else {
        for (unsigned i = 0; i < 4; i++)
            if (!mips_patch(e, misses[i], e->count)) e->refused = true;
    }
    *end = cursor;
    plan->basic_blocks++;
    return true;
}

#include "native-tier-mips-value-blocks.inc"
#include "native-tier-mips-borrowed-fields.inc"
#include "native-tier-mips-borrowed-cfg.inc"

static bool mips_reference_block(Emitter *e, TFChainFixups *fix,
                                  TFRegionPlan *plan, unsigned at, unsigned *end)
{
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    unsigned index;
    for (index = 0; index < plan->reference_count; index++)
        if (plan->reference_blocks[index].offsets[0] == at) break;
    if (index == plan->reference_count) return false;
    const TFPrimitiveBlock *b = &plan->reference_blocks[index];
    mips_flush(e); /* includes a cached local/argument before borrowed reads */
    mips_r(e,33,M_A0,M_S0,M_ZERO);
    immediate32(e,M_A1,index); immediate32(e,M_A2,0);
    TFRegionHelper helper = tf_reference_execute;
    uintptr_t address;
    memcpy(&address,&helper,sizeof(address));
    immediate32(e,M_T9,address);
    emit(e,(M_T9 << 21) | (M_RA << 11) | 9u); emit(e,0);
    mips_load(e,M_S1,M_S0,offsetof(TFRegionFrame,sp));
    mips_load(e,M_S3,M_S0,offsetof(TFRegionFrame,completed));
    mips_load(e,M_T2,M_S0,offsetof(TFRegionFrame,pc));
    mips_r(e,35,M_S4,M_T2,M_S2);
    if (e->binding_kind) {
        mips_load(e,M_S6,M_S5,0); mips_load(e,M_S7,M_S5,4);
    }
    mips_fix(e,fix,mips_branch(e,true,M_V0,M_ZERO),TF_CHAIN_EXIT);
    if (b->conditional) {
        immediate32(e,M_T2,2);
        mips_fix(e,fix,mips_branch(e,true,M_V0,M_T2),b->target);
    }
    mips_jump(e,fix,b->end);
    /* Misses and interior entries use the original interpreter. Avoid a full
       emitted duplicate of every region: code competes with the 16 KiB I-cache
       and the fixed 64 KiB/image bound. Refusal has committed no effects. */
    *end = b->end;
    return !e->refused;
#else
    (void)e; (void)fix; (void)plan; (void)at; (void)end;
    return false;
#endif
}

/* One original operation, shared by the hot stream and out-of-line fallback.
   Entry registration is separate so a cold copy cannot replace the hot entry
   for its block start. Every interior bytecode entry stays independently valid. */
static bool mips_operation(Emitter *e, TFRegionPlan *plan, TFChainFixups *fix, unsigned at)
{
    JSFunctionBytecode *body = plan->body;
    unsigned length = body->byte_code_len;
    const uint8_t *pc = body->byte_code_buf + at;
    unsigned size = tf_region_opcodes[pc[0]].size, next = at + size;
    uint32_t argument;
    TFRegionHelper helper = tf_region_decode(pc, &argument);
    if (!helper || next > length || (next == length && helper != tf_region_return)) return false;
    unsigned following_size = next < length ? tf_region_opcodes[body->byte_code_buf[next]].size : 0;
    if (following_size && following_size < length - next) {
        uint32_t atom;
        if (tf_region_decode(body->byte_code_buf + next, &atom) == tf_region_field)
            mips_fused_field(e, fix, plan, helper, argument, atom, next + following_size);
    }
    if (helper == tf_region_branch) {
        int32_t delta = pc[0] == OP_goto16 ? (int16_t)get_u16(pc + 1) :
            (pc[0] == OP_goto || pc[0] == OP_if_true || pc[0] == OP_if_false) ?
            (int32_t)get_u32(pc + 1) : (int8_t)pc[1];
        int64_t target = (int64_t)at + 1 + delta;
        if (target < 0 || target >= length || !plan->steps[target]) return false;
        bool conditional = pc[0] == OP_if_false || pc[0] == OP_if_true ||
            pc[0] == OP_if_false8 || pc[0] == OP_if_true8;
        if (direct_branches) {
            mips_fast_branch(e, fix, pc[0], (unsigned)target, next, conditional);
            return !e->refused;
        }
        mips_helper(e, fix, helper, argument, next);
        if (conditional) {
            immediate32(e, M_T0, 2);
            mips_fix(e, fix, mips_branch(e, true, M_V0, M_T0), (unsigned)target);
        } else {
            mips_jump(e, fix, (unsigned)target);
            return !e->refused;
        }
    } else if (value_abi && helper == tf_region_field)
        mips_field_value(e, fix, argument, next);
    else {
        bool direct = mips_direct(e, fix, body, helper, argument, at);
        if (!direct)
            mips_helper(e, fix, helper == tf_region_field ? mips_field_semantic : helper, argument, next);
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
        else if (helper == tf_region_semantic) plan->semantic_direct_sites++;
#endif
    }
    immediate32(e, M_S4, next);
    uint32_t unused;
    if (next == length || !tf_region_decode(body->byte_code_buf + next, &unused))
        mips_jump(e, fix, TF_CHAIN_EXIT);
    return !e->refused;
}
static bool emit_chain(Emitter *e, TFRegionPlan *plan, TFChainFixups *fix)
{
    JSFunctionBytecode *body = plan->body;
    unsigned length = body->byte_code_len;
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        if (!size || size > length - at) return false;
        plan->steps[at] = 1;
        at += size;
    }
    if (!register_cfg && !mips_select_binding(e, plan)) return false;
    e->full_save = register_cfg;
    mips_prologue(e);
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size, next = at + size;
        uint32_t argument;
        if (!tf_region_decode(body->byte_code_buf + at, &argument)) { at = next; continue; }
        if (!tf_region_set_entry(plan, at, e->count * 4)) return false;
        plan->entries++;
        plan->instructions++;
        unsigned end = next;
        /* Publish the source PC before either speculative or ordinary guards. */
        immediate32(e, M_S4, at);
        Emitter before_cfg=*e;
        unsigned before_fixups=fix->count;
        if (mips_cfg_block(e,fix,plan,at)) {
            if (!e->refused) {
                at = next;
                continue;
            }
            /* Optional CFG lowering must not turn a valid bounded direct
               plan into a refusal when its code/fixup allowance is exhausted.
               Only newly emitted words were patched; published entries and
               older fixups are unchanged. */
            *e=before_cfg;
            fix->count=before_fixups;
        }
        if (mips_borrowed_block(e,fix,plan,at,&end) ||
            mips_reference_block(e,fix,plan,at,&end) ||
            mips_value_block(e, fix, plan, at, &end) ||
            (mips_compare_block(e, fix, plan, at, &end) && cold_blocks)) {
            at = end;
            continue;
        }
        if (!mips_operation(e, plan, fix, at)) return false;
        at = next;
    }
    if (fix->cold_count) mips_jump(e, fix, TF_CHAIN_EXIT);
    if (fix->cold_count) plan->cold_start = e->count * 4;
    for (unsigned record = 0; record < fix->cold_count; record++) {
        unsigned at = fix->cold[record].start, end = fix->cold[record].end;
        for (unsigned i = 0; i < fix->cold[record].miss_count; i++)
            if (!mips_patch(e, fix->cold[record].misses[i], e->count)) return false;
        for (unsigned i = 0; at < end && i < 16; i++) {
            unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
            if (i) {
                if (!tf_region_set_entry(plan, at, e->count * 4)) return false;
                plan->entries++;
                plan->instructions++;
            }
            immediate32(e, M_S4, at);
            if (!mips_operation(e, plan, fix, at)) return false;
            at += size;
        }
        if (at != end) return false;
        /* The branch has already chosen a mapped destination or exit; there
           is never a fall-through from one cold block into another. */
    }
    unsigned exit = e->count;
    mips_epilogue(e);
    if (e->refused || !plan->entries) return false;
    for (unsigned i = 0; i < fix->count; i++) {
        unsigned target = fix->items[i].target;
        unsigned destination = target == TF_CHAIN_EXIT || !plan->entry[target] ?
            exit : (plan->entry[target] - 1) / 4;
        if (!mips_patch(e, fix->items[i].word, destination)) return false;
    }
    return true;
}
