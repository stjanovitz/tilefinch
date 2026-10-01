/* Direct value operations for the isolated ARM64 region experiment.
   x19=handoff, x20=owned operand stack, x21=bytecode base,
   w22=completed operations, w23=current bytecode offset. */
_Static_assert(sizeof(JSValue) == 16 && offsetof(JSValue, tag) == 8,
               "direct emitter requires the host struct-value representation");
_Static_assert(sizeof(JSRefCountHeader) == 4 && offsetof(JSRefCountHeader, ref_count) == 0,
               "direct emitter mirrors __js_rc's four-byte prefix");
_Static_assert(JS_TAG_FIRST < 0 && JS_TAG_FIRST > -4096, "reference tag guard immediate");

typedef struct { unsigned at[TF_REGION_STEP_LIMIT * 4], count; } RegionExits;

static void load64(Emitter *e, unsigned to, unsigned from, size_t offset)
{
    if (offset % 8 || offset / 8 > 4095) { e->refused = true; return; }
    emit(e, 0xf9400000u | ((offset / 8) << 10) | (from << 5) | to);
}
static void store64(Emitter *e, unsigned value, unsigned to, size_t offset)
{
    if (offset % 8 || offset / 8 > 4095) { e->refused = true; return; }
    emit(e, 0xf9000000u | ((offset / 8) << 10) | (to << 5) | value);
}
static void pair(Emitter *e, bool load, unsigned a, unsigned b, unsigned base, int offset)
{
    if (offset % 8 || offset < -512 || offset > 504) { e->refused = true; return; }
    emit(e, (load ? 0xa9400000u : 0xa9000000u) |
        (((unsigned)(offset / 8) & 127) << 15) | (b << 10) | (base << 5) | a);
}
static void add_offset(Emitter *e, unsigned reg, unsigned offset)
{
    if (offset > 4095) { e->refused = true; return; }
    if (offset) emit(e, 0x91000000u | (offset << 10) | (reg << 5) | reg);
}
static void exit_branch(Emitter *e, RegionExits *exits, uint32_t instruction)
{
    if (e->refused || exits->count == TF_REGION_STEP_LIMIT * 4) { e->refused = true; return; }
    exits->at[exits->count++] = e->count;
    emit(e, instruction);
}
static void patch_local_branch(Emitter *e, unsigned at)
{
    if (e->refused || at >= e->count || e->count - at >= (1u << 18)) { e->refused = true; return; }
    e->words[at] |= (e->count - at) << 5;
}
static void stack_guard(Emitter *e, RegionExits *exits, bool push)
{
    load64(e, 9, 19, push ? offsetof(TFRegionFrame, limit) : offsetof(TFRegionFrame, base));
    emit(e, 0xeb09029fu); /* cmp x20,x9 */
    exit_branch(e, exits, 0x54000000u | (push ? 2 : 9)); /* b.hs / b.ls */
}
/* x10/x11 hold a JSValue. Reference updates cannot invoke page code or free an
   object: the release path guards before decrementing a final reference. */
static void reference_update(Emitter *e, RegionExits *exits, bool duplicate)
{
    emit(e, 0x3100001fu | ((unsigned)-JS_TAG_FIRST << 10) | (11 << 5)); /* cmn w11,#-FIRST */
    unsigned immediate = e->count;
    emit(e, 0x54000003u); /* b.lo immediate value */
    emit(e, 0xb85fc14cu); /* ldur w12,[x10,#-4] */
    if (!duplicate) {
        emit(e, 0x7100059fu); /* cmp w12,#1 */
        exit_branch(e, exits, 0x5400000du); /* b.le exit */
    }
    emit(e, (duplicate ? 0x11000000u : 0x51000000u) | (1 << 10) | (12 << 5) | 12);
    emit(e, 0xb81fc14cu); /* stur w12,[x10,#-4] */
    patch_local_branch(e, immediate);
}
static void direct_frame_flush(Emitter *e)
{
    store64(e, 20, 19, offsetof(TFRegionFrame, sp));
    emit(e, 0x8b1702a9u); /* add x9,x21,x23 */
    store64(e, 9, 19, offsetof(TFRegionFrame, pc));
    emit(e, 0xb9000000u | ((offsetof(TFRegionFrame, completed) / 4) << 10) | (19 << 5) | 22);
}
static void direct_prologue(Emitter *e)
{
    emit(e, 0xd503245fu); /* bti c */
    emit(e, 0xa9bd53f3u); /* stp x19,x20,[sp,#-48]! */
    pair(e, false, 21, 22, 31, 16);
    pair(e, false, 23, 30, 31, 32);
    emit(e, 0xaa0003f3u); /* mov x19,x0 */
    load64(e, 20, 19, offsetof(TFRegionFrame, sp));
    load64(e, 21, 19, offsetof(TFRegionFrame, body));
    load64(e, 21, 21, offsetof(JSFunctionBytecode, byte_code_buf));
    emit(e, 0xb9400000u | ((offsetof(TFRegionFrame, completed) / 4) << 10) | (19 << 5) | 22);
}
static void direct_epilogue(Emitter *e)
{
    direct_frame_flush(e);
    pair(e, true, 21, 22, 31, 16);
    pair(e, true, 23, 30, 31, 32);
    emit(e, 0xa8c353f3u); /* ldp x19,x20,[sp],#48 */
    emit(e, 0xd65f03c0u);
}

/* Returns false without emission when this instruction stays a semantic C
   helper. Indices are checked against the pinned body before emitting an
   address; no speculative class/shape offsets or cached JS values are used. */
static bool direct_operation(Emitter *e, RegionExits *exits, JSFunctionBytecode *body,
                             TFRegionHelper helper, uint32_t argument, unsigned at)
{
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
    if (continuation_values && helper == tf_region_semantic) {
        unsigned op = body->byte_code_buf[at];
        /* Match the semantic helper's per-operation quantum check, including
           raw entry at a predicate rather than only the next branch poll. */
        immediate32(e, 9, TF_REGION_QUANTUM);
        emit(e, 0x6b0902dfu); /* cmp w22,w9 */
        unsigned admitted = e->count;
        emit(e, 0x54000003u); /* b.lo */
        immediate32(e, 9, 1);
        _Static_assert(offsetof(TFRegionFrame, quota_exit) < 4096, "quota flag immediate");
        emit(e, 0x39000000u | (offsetof(TFRegionFrame, quota_exit) << 10) | (19 << 5) | 9);
        emit(e, 0x6b09013fu); /* cmp w9,w9: unconditional conditional-format exit */
        exit_branch(e, exits, 0x54000000u);
        patch_local_branch(e, admitted);
        if (op == OP_set_loc_uninitialized) {
            unsigned index = get_u16(body->byte_code_buf + at + 1);
            if (index >= body->var_count || index > 255) return false;
            load64(e, 9, 19, offsetof(TFRegionFrame, locals));
            add_offset(e, 9, index * 16);
            pair(e, true, 10, 11, 9, 0);
            reference_update(e, exits, false);
            immediate32(e, 10, 0);
            immediate32(e, 11, JS_TAG_UNINITIALIZED);
            pair(e, false, 10, 11, 9, 0);
        } else if (op == OP_strict_eq || op == OP_strict_neq) {
            load64(e, 9, 19, offsetof(TFRegionFrame, base));
            add_offset(e, 9, 16);
            emit(e, 0xeb09029fu);
            exit_branch(e, exits, 0x54000009u); /* two operands required */
            pair(e, true, 10, 11, 20, -32);
            pair(e, true, 12, 13, 20, -16);
            emit(e, 0x7100017fu | (JS_TAG_UNDEFINED << 10));
            exit_branch(e, exits, 0x54000008u);
            emit(e, 0x710001bfu | (JS_TAG_UNDEFINED << 10));
            exit_branch(e, exits, 0x54000008u);
            emit(e, 0x6b0d017fu); /* cmp w11,w13: same immediate kind */
            emit(e, 0x1a9f17e9u); /* cset w9,eq */
            emit(e, 0x6b0c015fu); /* cmp w10,w12: same payload */
            emit(e, 0x1a9f17eau);
            emit(e, 0x0a09014au); /* and w10,w10,w9 */
            if (op == OP_strict_neq) {
                immediate32(e, 12, 1);
                emit(e, 0x4a0c014au); /* eor w10,w10,w12 */
            }
            immediate32(e, 11, JS_TAG_BOOL);
            pair(e, false, 10, 11, 20, -32);
            emit(e, 0xd1004294u);
        } else {
            stack_guard(e, exits, false);
            pair(e, true, 10, 11, 20, -16);
            if (op == OP_lnot) {
                emit(e, 0x7100017fu | (JS_TAG_UNDEFINED << 10));
                exit_branch(e, exits, 0x54000008u);
                emit(e, 0x7100015fu); /* cmp w10,#0 */
                emit(e, 0x1a9f17eau);
            } else {
                /* A nullish test never coerces. Guard final release first,
                   then compare the original tag (reference_update keeps it). */
                reference_update(e, exits, false);
                if (op == OP_is_undefined_or_null) {
                    emit(e, 0x7100017fu | (JS_TAG_NULL << 10));
                    emit(e, 0x1a9f17e9u);
                    emit(e, 0x7100017fu | (JS_TAG_UNDEFINED << 10));
                    emit(e, 0x1a9f17eau);
                    emit(e, 0x2a09014au); /* orr w10,w10,w9 */
                } else {
                    unsigned tag = op == OP_is_null ? JS_TAG_NULL : JS_TAG_UNDEFINED;
                    emit(e, 0x7100017fu | (tag << 10));
                    emit(e, 0x1a9f17eau);
                }
            }
            immediate32(e, 11, JS_TAG_BOOL);
            pair(e, false, 10, 11, 20, -16);
        }
        emit(e, 0x110006d6u);
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
        stack_guard(e, exits, true);
        if (local || arg) {
            load64(e, 9, 19, local ? offsetof(TFRegionFrame, locals) : offsetof(TFRegionFrame, args));
            add_offset(e, 9, argument * 16);
        } else if (ref) {
            load64(e, 9, 19, offsetof(TFRegionFrame, refs));
            load64(e, 9, 9, argument * 8);
            load64(e, 9, 9, offsetof(JSVarRef, pvalue));
        } else {
            load64(e, 9, 19, offsetof(TFRegionFrame, body));
            load64(e, 9, 9, offsetof(JSFunctionBytecode, cpool));
            add_offset(e, 9, argument * 16);
        }
        pair(e, true, 10, 11, 9, 0);
        if (checked) {
            emit(e, 0x7100001fu | (JS_TAG_UNINITIALIZED << 10) | (11 << 5));
            exit_branch(e, exits, 0x54000000u); /* b.eq exit before duplication */
        }
        reference_update(e, exits, true);
        pair(e, false, 10, 11, 20, 0);
        add_offset(e, 20, 16);
    } else if (helper == tf_region_integer || helper == tf_region_immediate) {
        stack_guard(e, exits, true);
        unsigned tag = JS_TAG_INT, value = argument;
        if (helper == tf_region_immediate) {
            tag = argument == OP_null ? JS_TAG_NULL : argument == OP_undefined ? JS_TAG_UNDEFINED : JS_TAG_BOOL;
            value = argument == OP_push_true;
        }
        immediate32(e, 10, value);
        immediate32(e, 11, tag);
        pair(e, false, 10, 11, 20, 0);
        add_offset(e, 20, 16);
    } else if (helper == tf_region_dup) {
        stack_guard(e, exits, false);
        stack_guard(e, exits, true);
        pair(e, true, 10, 11, 20, -16);
        reference_update(e, exits, true);
        pair(e, false, 10, 11, 20, 0);
        add_offset(e, 20, 16);
    } else if (helper == tf_region_drop) {
        stack_guard(e, exits, false);
        pair(e, true, 10, 11, 20, -16);
        reference_update(e, exits, false);
        emit(e, 0xd1004294u); /* sub x20,x20,#16 */
    } else if (helper == tf_region_put_local || helper == tf_region_put_local_check) {
        if (argument >= body->var_count || argument > 255) return false;
        stack_guard(e, exits, false);
        load64(e, 9, 19, offsetof(TFRegionFrame, locals));
        add_offset(e, 9, argument * 16);
        pair(e, true, 10, 11, 9, 0);
        if (helper == tf_region_put_local_check) {
            emit(e, 0x7100001fu | (JS_TAG_UNINITIALIZED << 10) | (11 << 5));
            exit_branch(e, exits, 0x54000000u);
        }
        reference_update(e, exits, false); /* x9 remains the local slot */
        pair(e, true, 10, 11, 20, -16);
        pair(e, false, 10, 11, 9, 0);
        emit(e, 0xd1004294u);
    } else if (value_abi && helper == tf_region_less) {
        load64(e, 9, 19, offsetof(TFRegionFrame, base));
        add_offset(e, 9, 16);
        emit(e, 0xeb09029fu); /* cmp x20,base+one operand */
        exit_branch(e, exits, 0x54000009u); /* b.ls: need two operands */
        pair(e, true, 10, 11, 20, -32);
        pair(e, true, 12, 13, 20, -16);
        emit(e, 0x7100017fu | (JS_TAG_INT << 10)); /* cmp w11,#INT */
        exit_branch(e, exits, 0x54000001u);
        emit(e, 0x710001bfu | (JS_TAG_INT << 10)); /* cmp w13,#INT */
        exit_branch(e, exits, 0x54000001u);
        emit(e, 0x6b0c015fu); /* cmp w10,w12 */
        emit(e, 0x1a9fa7eau); /* cset w10,lt (signed) */
        immediate32(e, 11, JS_TAG_BOOL);
        pair(e, false, 10, 11, 20, -32);
        emit(e, 0xd1004294u); /* sub x20,x20,#16 */
    } else if (value_abi && (helper == tf_region_post_increment || helper == tf_region_increment)) {
        if (helper == tf_region_increment) {
            if (argument >= body->var_count || argument > 255) return false;
            load64(e, 9, 19, offsetof(TFRegionFrame, locals));
            add_offset(e, 9, argument * 16);
            pair(e, true, 10, 11, 9, 0);
        } else {
            stack_guard(e, exits, false);
            stack_guard(e, exits, true);
            pair(e, true, 10, 11, 20, -16);
        }
        emit(e, 0x7100017fu | (JS_TAG_INT << 10));
        exit_branch(e, exits, 0x54000001u);
        immediate32(e, 12, INT32_MAX);
        emit(e, 0x6b0c015fu); /* cmp w10,w12 */
        exit_branch(e, exits, 0x54000000u); /* overflow stays in interpreter */
        emit(e, 0x1100054au); /* add w10,w10,#1 */
        if (helper == tf_region_increment) pair(e, false, 10, 11, 9, 0);
        else {
            pair(e, false, 10, 11, 20, 0);
            add_offset(e, 20, 16);
        }
    } else return false;
    emit(e, 0x110006d6u); /* add w22,w22,#1 */
    return true;
}

static void emit_direct_region(Emitter *e, JSFunctionBytecode *body, unsigned start, unsigned end)
{
    RegionExits exits = {0};
    direct_prologue(e);
    for (unsigned at = start; at < end;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        uint32_t argument;
        TFRegionHelper helper = tf_region_decode(body->byte_code_buf + at, &argument);
        immediate32(e, 23, at);
        if (!direct_operation(e, &exits, body, helper, argument, at)) {
            direct_frame_flush(e);
            emit(e, 0xaa1303e0u);
            immediate32(e, 1, argument);
            immediate32(e, 2, at + size);
            helper_address(e, helper);
            emit(e, 0xd63f0200u);
            load64(e, 20, 19, offsetof(TFRegionFrame, sp));
            emit(e, 0xb9400000u | ((offsetof(TFRegionFrame, completed) / 4) << 10) | (19 << 5) | 22);
            exit_branch(e, &exits, 0x34000000u); /* cbz w0 */
        }
        at += size;
    }
    immediate32(e, 23, end);
    unsigned exit = e->count;
    direct_epilogue(e);
    if (e->refused) return;
    for (unsigned i = 0; i < exits.count; i++) {
        unsigned at = exits.at[i];
        if (exit - at >= (1u << 18)) { e->refused = true; return; }
        e->words[at] |= (exit - at) << 5;
    }
}
