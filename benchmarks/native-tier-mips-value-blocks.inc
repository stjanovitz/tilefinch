/* Fixed, compile-only primitive stack analysis. A fast block has no calls,
   allocation, reference changes or local writes. Borrow at most three immediate
   values; guard every input, the original stack peak, quantum and branch poll
   before publishing effects. A miss executes the original cold instructions,
   including every partial-state/final-reference/TDZ fallback. */
typedef struct { unsigned count, words[TF_CHAIN_MISS_LIMIT]; } TFPrimitiveMisses;
static void mips_primitive_miss(Emitter *e, TFPrimitiveMisses *m, bool equal,
                                 unsigned a, unsigned b)
{
    if (m->count == TF_CHAIN_MISS_LIMIT) { e->refused = true; return; }
    m->words[m->count++] = mips_branch(e, equal, a, b);
}
static void mips_primitive_tag(Emitter *e, TFPrimitiveMisses *m, unsigned tag)
{
    mips_i(e, 11, M_T4, tag, JS_TAG_UNDEFINED + 1);
    mips_primitive_miss(e, m, true, M_T4, M_ZERO);
}

static bool mips_value_block(Emitter *e, TFChainFixups *fix,
                              TFRegionPlan *plan, unsigned at, unsigned *end)
{
    if (!value_blocks || !direct_branches || !continuation_values) return false;
    if (js_check_stack_overflow(plan->body->realm->rt, 1024)) return false;
    TFPrimitiveBlock b;
    if (!tf_primitive_scan(plan->body, plan->steps, at, &b)) return false;
    if (fix->cold_count == TF_CHAIN_COLD_LIMIT) { e->refused = true; return false; }
    TFPrimitiveMisses misses = {0};
    /* Original peak capacity matters even when no temporary is materialized. */
    if (b.incoming) {
        mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, base));
        mips_i(e, 9, M_T4, M_T4, b.incoming * 8);
        mips_r(e, 43, M_T4, M_S1, M_T4);
        mips_primitive_miss(e, &misses, false, M_T4, M_ZERO);
    }
    if (b.peak) {
        mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, limit));
        mips_i(e, 9, M_A2, M_S1, b.peak * 8);
        mips_r(e, 43, M_T4, M_T4, M_A2);
        mips_primitive_miss(e, &misses, false, M_T4, M_ZERO);
    }
    mips_i(e, 11, M_T4, M_S3, TF_REGION_QUANTUM - b.count + 1);
    mips_primitive_miss(e, &misses, true, M_T4, M_ZERO);
    static const unsigned payloads[] = {M_T0, M_T2, M_T5};
    static const unsigned tags[] = {M_T1, M_T3, M_T6};
    unsigned depth = b.incoming;
    for (unsigned i = 0; i < b.incoming; i++) {
        mips_i(e, 35, payloads[i], M_S1, ((int)i - (int)b.incoming) * 8);
        mips_i(e, 35, tags[i], M_S1, ((int)i - (int)b.incoming) * 8 + 4);
        mips_primitive_tag(e, &misses, tags[i]);
    }
    for (unsigned i = 0; i < b.count - 1; i++) {
        TFRegionHelper helper = b.helpers[i];
        unsigned op = plan->body->byte_code_buf[b.offsets[i]];
        if (tf_primitive_source_valid(plan->body, helper, b.arguments[i]) || helper == tf_region_immediate) {
            if (depth == TF_PRIMITIVE_STACK_LIMIT) { e->refused = true; return false; }
            if (helper == tf_region_immediate) {
                immediate32(e, payloads[depth], b.arguments[i] == OP_push_true);
                immediate32(e, tags[depth], b.arguments[i] == OP_null ? JS_TAG_NULL :
                    b.arguments[i] == OP_undefined ? JS_TAG_UNDEFINED : JS_TAG_BOOL);
            } else {
                mips_block_source(e, helper, b.arguments[i], payloads[depth], tags[depth]);
                mips_primitive_tag(e, &misses, tags[depth]);
            }
            depth++;
        } else if (helper == tf_region_drop) {
            if (!depth) { e->refused = true; return false; }
            depth--;
        } else if (helper == tf_region_dup) {
            if (!depth || depth == TF_PRIMITIVE_STACK_LIMIT) { e->refused = true; return false; }
            mips_r(e, 33, payloads[depth], payloads[depth-1], M_ZERO);
            mips_r(e, 33, tags[depth], tags[depth-1], M_ZERO);
            depth++;
        } else if (helper == tf_region_less || op == OP_strict_eq || op == OP_strict_neq) {
            if (depth < 2) { e->refused = true; return false; }
            unsigned l = depth - 2, r = depth - 1;
            if (helper == tf_region_less) {
                mips_r(e, 37, M_T4, tags[l], tags[r]);
                mips_primitive_miss(e, &misses, false, M_T4, M_ZERO);
                mips_r(e, 42, payloads[l], payloads[l], payloads[r]);
            } else {
                mips_r(e, 38, M_T4, tags[l], tags[r]);
                mips_i(e, 11, M_T4, M_T4, 1);
                mips_r(e, 38, payloads[l], payloads[l], payloads[r]);
                mips_i(e, 11, payloads[l], payloads[l], 1);
                mips_i(e, 11, M_A2, tags[l], JS_TAG_NULL);
                mips_i(e, 14, M_A2, M_A2, 1);
                mips_r(e, 37, payloads[l], payloads[l], M_A2);
                mips_r(e, 36, payloads[l], payloads[l], M_T4);
                if (op == OP_strict_neq) mips_i(e, 14, payloads[l], payloads[l], 1);
            }
            immediate32(e, tags[l], JS_TAG_BOOL);
            depth--;
        } else {
            if (!depth) { e->refused = true; return false; }
            unsigned p = payloads[depth-1], tag = tags[depth-1];
            if (op == OP_lnot) {
                mips_i(e, 11, p, p, 1);
                mips_i(e, 11, M_T4, tag, JS_TAG_NULL);
                mips_i(e, 14, M_T4, M_T4, 1);
                mips_r(e, 37, p, p, M_T4);
            } else if (op == OP_is_undefined_or_null) {
                mips_i(e, 9, p, tag, -JS_TAG_NULL);
                mips_i(e, 11, p, p, 2);
            } else {
                mips_i(e, 14, p, tag, op == OP_is_null ? JS_TAG_NULL : JS_TAG_UNDEFINED);
                mips_i(e, 11, p, p, 1);
            }
            immediate32(e, tag, JS_TAG_BOOL);
        }
    }
    if (!depth) { e->refused = true; return false; }
    unsigned condition = payloads[depth-1];
    /* Null/undefined are false independent of their internal payload bits. */
    mips_i(e, 11, M_T4, tags[depth-1], JS_TAG_NULL);
    mips_r(e, 43, condition, M_ZERO, condition);
    mips_r(e, 36, condition, condition, M_T4);
    mips_load(e, M_A0, M_S0, offsetof(TFRegionFrame, ctx));
    mips_load(e, M_A1, M_A0, offsetof(JSContext, interrupt_counter));
    mips_i(e, 10, M_T4, M_A1, 2);
    mips_primitive_miss(e, &misses, false, M_T4, M_ZERO);

    /* All refusal guards passed. Publish only surviving values and one poll. */
    if (value_block_probes) {
        mips_load(e, M_T4, M_S0, offsetof(TFRegionFrame, value_block_hits));
        mips_i(e, 9, M_T4, M_T4, 1);
        mips_store(e, M_T4, M_S0, offsetof(TFRegionFrame, value_block_hits));
    }
    depth--;
    for (unsigned i = 0; i < depth; i++) {
        mips_i(e, 43, payloads[i], M_S1, ((int)i - (int)b.incoming) * 8);
        mips_i(e, 43, tags[i], M_S1, ((int)i - (int)b.incoming) * 8 + 4);
    }
    mips_i(e, 9, M_S1, M_S1, ((int)depth - (int)b.incoming) * 8);
    mips_i(e, 9, M_A1, M_A1, -1);
    mips_store(e, M_A1, M_A0, offsetof(JSContext, interrupt_counter));
    mips_i(e, 9, M_S3, M_S3, b.count);
    immediate32(e, M_S4, b.target);
    mips_fix(e, fix, mips_branch(e, !b.if_true, condition, M_ZERO), b.target);
    immediate32(e, M_S4, b.end);
    mips_jump(e, fix, b.end);
    unsigned record = fix->cold_count++;
    fix->cold[record].start = (uint16_t)at;
    fix->cold[record].end = (uint16_t)b.end;
    fix->cold[record].miss_count = (uint16_t)misses.count;
    for (unsigned i = 0; i < misses.count; i++) {
        if (misses.words[i] >= TF_REGION_CODE_LIMIT / 4) e->refused = true;
        fix->cold[record].misses[i] = (uint16_t)misses.words[i];
    }
    plan->value_blocks++;
    *end = b.end;
    return !e->refused;
}
