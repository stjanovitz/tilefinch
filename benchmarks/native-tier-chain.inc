/* One leased native activation can traverse multiple bytecode blocks. The
   common prologue receives a trusted body entry in x1, never author input. */
#define TF_CHAIN_FIXUP_LIMIT 2048u
#define TF_CHAIN_EXIT UINT16_MAX
typedef struct {
    unsigned count;
    struct { uint16_t word, target; } items[TF_CHAIN_FIXUP_LIMIT];
} TFChainFixups;

static void chain_fixup(Emitter *e, TFChainFixups *fix, unsigned word, unsigned target)
{
    if (e->refused || fix->count == TF_CHAIN_FIXUP_LIMIT || word >= TF_REGION_CODE_LIMIT / 4 ||
        (target >= TF_REGION_BYTECODE_LIMIT && target != TF_CHAIN_EXIT)) { e->refused = true; return; }
    fix->items[fix->count].word = word;
    fix->items[fix->count++].target = target;
}
static void chain_jump(Emitter *e, TFChainFixups *fix, unsigned target, uint32_t opcode)
{
    chain_fixup(e, fix, e->count, target);
    emit(e, opcode);
}
static bool chain_branch_target(TFRegionPlan *plan, unsigned at, unsigned *target, bool *conditional)
{
    const uint8_t *pc = plan->body->byte_code_buf + at;
    int32_t delta = pc[0] == OP_goto16 ? (int16_t)get_u16(pc + 1) :
        (pc[0] == OP_goto || pc[0] == OP_if_true || pc[0] == OP_if_false) ?
        (int32_t)get_u32(pc + 1) : (int8_t)pc[1];
    int64_t destination = (int64_t)at + 1 + delta;
    if (destination < 0 || destination >= plan->body->byte_code_len || !plan->steps[destination]) return false;
    *target = (unsigned)destination;
    *conditional = pc[0] == OP_if_false || pc[0] == OP_if_true ||
        pc[0] == OP_if_false8 || pc[0] == OP_if_true8;
    return true;
}
static void emit_fast_branch(Emitter *e, TFChainFixups *fix, unsigned opcode,
                             unsigned target, unsigned next, bool conditional,
                             unsigned *mapped)
{
    const char *name = tf_region_opcodes[opcode].name;
    /* Refuse before consuming the condition or charging the interrupt poll.
       The interpreter executes the branch exactly once after a refusal. */
    immediate32(e, 9, TF_REGION_QUANTUM);
    emit(e, 0x6b0902dfu); /* cmp w22,w9 */
    unsigned admitted = e->count;
    emit(e, 0x54000003u); /* b.lo below quota */
    immediate32(e, 9, 1);
    _Static_assert(offsetof(TFRegionFrame, quota_exit) < 4096, "quota flag immediate");
    emit(e, 0x39000000u | (offsetof(TFRegionFrame, quota_exit) << 10) | (19 << 5) | 9);
    chain_jump(e, fix, TF_CHAIN_EXIT, 0x14000000u);
    patch_local_branch(e, admitted);
    RegionExits guards = {0};
    if (conditional) {
        stack_guard(e, &guards, false);
        pair(e, true, 10, 11, 20, -16);
        emit(e, 0x7100017fu | (JS_TAG_UNDEFINED << 10)); /* cmp w11,#UNDEFINED */
        exit_branch(e, &guards, 0x54000008u); /* b.hi: other tags need ToBoolFree */
    }
    map_range(e, mapped, name, "branch-guards");
    load64(e, 9, 19, offsetof(TFRegionFrame, ctx));
    _Static_assert(sizeof(((JSContext *)0)->interrupt_counter) == 4 &&
                   offsetof(JSContext, interrupt_counter) % 4 == 0 &&
                   offsetof(JSContext, interrupt_counter) / 4 < 4096, "interrupt counter ABI");
    unsigned counter_offset = offsetof(JSContext, interrupt_counter) / 4;
    emit(e, 0xb9400000u | (counter_offset << 10) | (9 << 5) | 12);
    emit(e, 0x7100059fu); /* cmp w12,#1 */
    exit_branch(e, &guards, 0x5400000du); /* b.le: interpreter owns slow poll */
    emit(e, 0x5100058cu); /* sub w12,w12,#1 */
    emit(e, 0xb9000000u | (counter_offset << 10) | (9 << 5) | 12);
    for (unsigned i = 0; i < guards.count; i++) chain_fixup(e, fix, guards.at[i], TF_CHAIN_EXIT);
    if (conditional) emit(e, 0xd1004294u); /* consume the immediate condition */
    emit(e, 0x110006d6u); /* charge the completed branch */
    map_range(e, mapped, name, "branch-poll");
    immediate32(e, 23, target);
    if (conditional) {
        emit(e, 0x7100015fu); /* cmp w10,#0 */
        bool if_true = opcode == OP_if_true || opcode == OP_if_true8;
        chain_jump(e, fix, target, 0x54000000u | (if_true ? 1 : 0));
        immediate32(e, 23, next);
        chain_jump(e, fix, next, 0x14000000u);
    } else chain_jump(e, fix, target, 0x14000000u);
    map_range(e, mapped, name, "branch-route");
}
static bool emit_chain(Emitter *e, TFRegionPlan *plan, TFChainFixups *fix)
{
    JSFunctionBytecode *body = plan->body;
    unsigned length = body->byte_code_len;
    /* Mark exact instruction boundaries before accepting branch relocations. */
    for (unsigned at = 0; at < length;) {
        unsigned size = tf_region_opcodes[body->byte_code_buf[at]].size;
        if (!size || size > length - at) return false;
        plan->steps[at] = 1;
        at += size;
    }
    unsigned mapped = e->count;
    direct_prologue(e);
    emit(e, 0xd61f0020u); /* br x1: already-validated plan entry */
    map_range(e, &mapped, "entry", "entry");
    for (unsigned at = 0; at < length;) {
        const uint8_t *pc = body->byte_code_buf + at;
        unsigned size = tf_region_opcodes[pc[0]].size, next = at + size;
        uint32_t argument;
        TFRegionHelper helper = tf_region_decode(pc, &argument);
        if (!helper) { at = next; continue; }
        if (next == length && helper != tf_region_return) return false;
        if (!tf_region_set_entry(plan, at, e->count * 4)) return false;
        plan->entries++;
        plan->instructions++;
        immediate32(e, 23, at);
        map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "pc");
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
        bool reference_emitted = false;
        for (unsigned index = 0; index < plan->reference_count; index++) {
            TFPrimitiveBlock *b = &plan->reference_blocks[index];
            if (b->offsets[0] != at) continue;
            direct_frame_flush(e);
            emit(e,0xaa1303e0u); /* x0 = canonical frame */
            immediate32(e,1,index); immediate32(e,2,0);
            helper_address(e,tf_reference_execute); emit(e,0xd63f0200u);
            load64(e,20,19,offsetof(TFRegionFrame,sp));
            emit(e,0xb9400000u | ((offsetof(TFRegionFrame,completed)/4) << 10) | (19 << 5) | 22);
            load64(e,9,19,offsetof(TFRegionFrame,pc));
            emit(e,0xcb150137u); /* current bytecode offset from published PC */
            chain_jump(e,fix,TF_CHAIN_EXIT,0x34000000u); /* cbz w0: original interpreter */
            if (b->conditional) {
                emit(e,0x7100081fu); /* cmp w0,#2 */
                chain_jump(e,fix,b->target,0x54000000u);
            }
            chain_jump(e,fix,b->end,0x14000000u);
            at = b->end;
            reference_emitted = true;
            break; /* no emitted duplicate; interior entries are interpreted */
        }
        if (reference_emitted) continue;
#endif
        if (direct_branches && helper == tf_region_branch) {
            unsigned target;
            bool conditional;
            if (!chain_branch_target(plan, at, &target, &conditional)) return false;
            emit_fast_branch(e, fix, pc[0], target, next, conditional, &mapped);
            at = next;
            continue;
        }
        RegionExits exits = {0};
        if (value_abi && helper == tf_region_field) {
            /* The pure helper consumes/replaces one owned value on success.
               No frame publication is required: it cannot call JS or GC. */
            stack_guard(e, &exits, false);
            pair(e, true, 0, 1, 20, -16);
            immediate32(e, 2, argument);
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "value-helper-args");
            own_value_address(e);
            emit(e, 0xd63f0200u);
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "value-helper-call");
            emit(e, 0x7100003fu | (JS_TAG_EXCEPTION << 10)); /* cmp w1,#EXCEPTION */
            exit_branch(e, &exits, 0x54000000u);
            pair(e, false, 0, 1, 20, -16);
            emit(e, 0x110006d6u);
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "value-helper-result");
        } else if (!direct_operation(e, &exits, body, helper, argument, at)) {
            direct_frame_flush(e);
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "helper-save");
            emit(e, 0xaa1303e0u);
            immediate32(e, 1, argument);
            immediate32(e, 2, next);
            helper_address(e, helper);
            emit(e, 0xd63f0200u);
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "helper-call");
            load64(e, 20, 19, offsetof(TFRegionFrame, sp));
            emit(e, 0xb9400000u | ((offsetof(TFRegionFrame, completed) / 4) << 10) | (19 << 5) | 22);
            load64(e, 9, 19, offsetof(TFRegionFrame, pc));
            emit(e, 0xcb150137u); /* sub x23,x9,x21: helper may branch or throw */
            exit_branch(e, &exits, 0x34000000u); /* cbz w0 */
            map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "helper-restore");
        } else map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "value-and-guards");
        for (unsigned i = 0; i < exits.count; i++)
            chain_fixup(e, fix, exits.at[i], TF_CHAIN_EXIT);
        if (helper == tf_region_branch) {
            unsigned target;
            bool conditional;
            if (!chain_branch_target(plan, at, &target, &conditional)) return false;
            if (!conditional) {
                chain_jump(e, fix, (unsigned)target, 0x14000000u);
                map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "control");
                at = next;
                continue;
            }
            emit(e, 0x7100081fu); /* cmp w0,#2: helper says branch taken */
            chain_jump(e, fix, (unsigned)target, 0x54000000u);
        }
        uint32_t unused;
        if (next == length || !tf_region_decode(body->byte_code_buf + next, &unused)) {
            immediate32(e, 23, next);
            chain_jump(e, fix, TF_CHAIN_EXIT, 0x14000000u);
        }
        map_range(e, &mapped, tf_region_opcodes[pc[0]].name, "control");
        at = next;
    }
    unsigned exit = e->count;
    direct_epilogue(e);
    map_range(e, &mapped, "exit", "exit");
    if (e->refused || !plan->entries) return false;
    for (unsigned i = 0; i < fix->count; i++) {
        unsigned at = fix->items[i].word, target = fix->items[i].target;
        unsigned destination = target == TF_CHAIN_EXIT || !plan->entry[target] ? exit : (plan->entry[target] - 1) / 4;
        int32_t distance = (int32_t)destination - (int32_t)at;
        bool unconditional = e->words[at] == 0x14000000u;
        if (distance <= -(1 << 18) || distance >= (1 << 18)) return false;
        e->words[at] |= unconditional ? ((uint32_t)distance & 0x3ffffffu) :
            (((uint32_t)distance & 0x7ffffu) << 5);
    }
    return true;
}
