/* Allegrex-only experiment: binding -> ordinary own field(s) -> truth branch.
   The binding roots the complete read-only property chain. Every temporary
   acquire is followed by a release inside this region, so the net ownership
   and stack change are zero. No helper, allocation, callback or cached shape.
   Refusal publishes the unchanged source PC to the original interpreter. */
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
static bool mips_object_bit(bool html, unsigned *offset, unsigned *mask)
{
    JSObject object = {0};
    if (html) object.is_HTMLDDA = 1;
    else object.is_exotic = 1;
    const uint8_t *bytes = (const uint8_t *)&object;
    bool found = false;
    for (unsigned i = 0; i < sizeof(object); i++) if (bytes[i]) {
        if (found || (bytes[i] & (bytes[i]-1))) return false;
        *offset = i; *mask = bytes[i]; found = true;
    }
    return found;
}

static void mips_borrowed_rc(Emitter *e, TFChainFixups *fix, unsigned payload)
{
    /* Original temporary DupValue must not overflow. No last-reference
       destruction is possible while its binding/property owner remains live. */
    mips_i(e,35,M_T4,payload,-4);
    mips_i(e,10,M_T5,M_T4,1);
    mips_exit_if(e,fix,false,M_T5,M_ZERO);
    immediate32(e,M_T5,INT32_MAX);
    mips_exit_if(e,fix,true,M_T4,M_T5);
}

static void mips_borrowed_field(Emitter *e, TFChainFixups *fix, uint32_t atom,
                                unsigned exotic_offset, unsigned exotic_mask,
                                uint32_t next_mask, uint32_t type_mask)
{
    _Static_assert(sizeof(JSProperty) == 8 && sizeof(JSShapeProperty) == 8,
                   "Allegrex property slot ABI");
    _Static_assert(offsetof(JSShapeProperty,atom) == 4, "shape header ABI");
    /* t0/t1: rooted borrowed receiver/result. All scratch is caller-saved. */
    immediate32(e,M_T4,JS_TAG_OBJECT);
    mips_exit_if(e,fix,false,M_T1,M_T4);
    mips_borrowed_rc(e,fix,M_T0);
    mips_i(e,36,M_T4,M_T0,exotic_offset); /* lbu, compiler-derived bit location */
    mips_i(e,12,M_T4,M_T4,exotic_mask);
    mips_exit_if(e,fix,false,M_T4,M_ZERO);
    mips_load(e,M_T2,M_T0,offsetof(JSObject,shape));
    mips_load(e,M_T4,M_T2,offsetof(JSShape,prop_hash_mask));
    immediate32(e,M_T3,atom);
    mips_r(e,36,M_T5,M_T3,M_T4);
    emit(e,(M_T5 << 16) | (M_T5 << 11) | (2u << 6)); /* sll index,2 */
    mips_r(e,33,M_T5,M_T2,M_T5);
    mips_load(e,M_T5,M_T5,offsetof(JSShape,hash_table));
    mips_i(e,9,M_T4,M_T4,1);
    emit(e,(M_T4 << 16) | (M_T4 << 11) | (2u << 6));
    mips_r(e,33,M_T6,M_T2,M_T4);
    mips_i(e,9,M_T6,M_T6,sizeof(JSShape)); /* get_shape_prop, not prop_size */
    immediate32(e,M_A0,32); /* maximum collision-chain visits */
    unsigned loop = e->count;
    mips_exit_if(e,fix,true,M_T5,M_ZERO); /* absent: ordinary prototype semantics */
    mips_load(e,M_T4,M_T2,offsetof(JSShape,prop_count));
    mips_r(e,43,M_T4,M_T4,M_T5);
    mips_exit_if(e,fix,false,M_T4,M_ZERO);
    mips_i(e,9,M_T4,M_T5,-1);
    emit(e,(M_T4 << 16) | (M_T4 << 11) | (3u << 6));
    mips_r(e,33,M_T4,M_T6,M_T4);
    mips_load(e,M_V0,M_T4,offsetof(JSShapeProperty,atom));
    unsigned found = mips_branch(e,true,M_V0,M_T3);
    mips_load(e,M_V0,M_T4,0);
    immediate32(e,M_V1,next_mask);
    mips_r(e,36,M_T5,M_V0,M_V1);
    mips_i(e,9,M_A0,M_A0,-1);
    mips_exit_if(e,fix,true,M_A0,M_ZERO);
    unsigned again = mips_branch(e,true,M_ZERO,M_ZERO);
    if (!mips_patch(e,again,loop) || !mips_patch(e,found,e->count)) e->refused = true;
    mips_load(e,M_V0,M_T4,0);
    immediate32(e,M_V1,type_mask);
    mips_r(e,36,M_V0,M_V0,M_V1);
    mips_exit_if(e,fix,false,M_V0,M_ZERO); /* getter, var-ref and autoinit refuse */
    mips_load(e,M_T4,M_T0,offsetof(JSObject,prop));
    mips_i(e,9,M_T5,M_T5,-1);
    emit(e,(M_T5 << 16) | (M_T5 << 11) | (3u << 6));
    mips_r(e,33,M_T4,M_T4,M_T5);
    mips_load(e,M_T0,M_T4,0); mips_load(e,M_T1,M_T4,4);
}

static void mips_borrowed_truth(Emitter *e, TFChainFixups *fix,
                                unsigned html_offset, unsigned html_mask)
{
    unsigned immediate_paths[4], refs[4], done[6], ndone = 0;
    const int immediate_tags[] = {JS_TAG_INT,JS_TAG_BOOL,JS_TAG_SHORT_BIG_INT,JS_TAG_NULL};
    for (unsigned i = 0; i < 4; i++) {
        immediate32(e,M_T4,immediate_tags[i]);
        immediate_paths[i] = mips_branch(e,true,M_T1,M_T4);
    }
    immediate32(e,M_T4,JS_TAG_UNDEFINED);
    unsigned undefined = mips_branch(e,true,M_T1,M_T4);
    const int ref_tags[] = {JS_TAG_OBJECT,JS_TAG_STRING,JS_TAG_STRING_ROPE,JS_TAG_SYMBOL};
    for (unsigned i = 0; i < 4; i++) {
        immediate32(e,M_T4,ref_tags[i]);
        refs[i] = mips_branch(e,true,M_T1,M_T4);
    }
    mips_jump(e,fix,TF_CHAIN_EXIT); /* floats, large BigInts and internal values */
    for (unsigned i = 0; i < 3; i++)
        if (!mips_patch(e,immediate_paths[i],e->count)) e->refused = true;
    mips_r(e,43,M_T0,M_ZERO,M_T0);
    done[ndone++] = mips_branch(e,true,M_ZERO,M_ZERO);
    if (!mips_patch(e,immediate_paths[3],e->count) || !mips_patch(e,undefined,e->count)) e->refused = true;
    immediate32(e,M_T0,0);
    done[ndone++] = mips_branch(e,true,M_ZERO,M_ZERO);
    for (unsigned i = 0; i < 4; i++) {
        if (!mips_patch(e,refs[i],e->count)) e->refused = true;
        mips_borrowed_rc(e,fix,M_T0);
        if (i == 0) {
            mips_i(e,36,M_T4,M_T0,html_offset);
            mips_i(e,12,M_T4,M_T4,html_mask);
            mips_i(e,11,M_T0,M_T4,1);
        } else if (i == 1) {
            JSString sample = {.len=0x7fffffffu}; uint32_t mask;
            memcpy(&mask,&sample,sizeof(mask));
            if (mask != 0x7fffffffu) { e->refused = true; return; }
            mips_load(e,M_T4,M_T0,0); immediate32(e,M_T5,mask);
            mips_r(e,36,M_T4,M_T4,M_T5); mips_r(e,43,M_T0,M_ZERO,M_T4);
        } else if (i == 2) {
            mips_load(e,M_T4,M_T0,offsetof(JSStringRope,len));
            mips_r(e,43,M_T0,M_ZERO,M_T4);
        } else immediate32(e,M_T0,1);
        done[ndone++] = mips_branch(e,true,M_ZERO,M_ZERO);
    }
    for (unsigned i = 0; i < ndone; i++)
        if (!mips_patch(e,done[i],e->count)) e->refused = true;
}

static bool mips_borrowed_block(Emitter *e, TFChainFixups *fix,
                                TFRegionPlan *plan, unsigned at, unsigned *end)
{
    if (!borrowed_fields || !direct_branches || !continuation_values) return false;
    TFPrimitiveBlock b;
    if (!tf_borrowed_field_scan(plan->body,plan->steps,at,&b)) return false;
    unsigned exotic_offset, exotic_mask, html_offset, html_mask;
    if (!mips_object_bit(false,&exotic_offset,&exotic_mask) ||
        !mips_object_bit(true,&html_offset,&html_mask)) return false;
    JSShapeProperty sample = {0}; uint32_t next_mask, type_mask;
    sample.hash_next = (1u << 26)-1; memcpy(&next_mask,&sample,sizeof(next_mask));
    sample.hash_next = 0; sample.flags = JS_PROP_TMASK;
    memcpy(&type_mask,&sample,sizeof(type_mask));
    if (next_mask != (1u << 26)-1 || !type_mask) return false;
    mips_stack_guard(e,fix,true); /* preserve original temporary peak */
    mips_i(e,11,M_T4,M_S3,TF_REGION_QUANTUM-b.count+1);
    mips_exit_if(e,fix,true,M_T4,M_ZERO);
    mips_block_source(e,b.helpers[0],b.arguments[0],M_T0,M_T1);
    for (unsigned i = 1; i < b.count-1; i++)
        mips_borrowed_field(e,fix,b.arguments[i],exotic_offset,exotic_mask,next_mask,type_mask);
    mips_borrowed_truth(e,fix,html_offset,html_mask);
    mips_load(e,M_A0,M_S0,offsetof(TFRegionFrame,ctx));
    mips_load(e,M_A1,M_A0,offsetof(JSContext,interrupt_counter));
    mips_i(e,10,M_T4,M_A1,2); mips_exit_if(e,fix,false,M_T4,M_ZERO);
    /* All guards passed: zero net stack/RC change; publish exactly one poll. */
    immediate32(e,M_T4,(uintptr_t)&borrowed_field_probes);
    mips_i(e,36,M_T4,M_T4,0);
    unsigned no_probe = mips_branch(e,true,M_T4,M_ZERO);
    immediate32(e,M_T4,(uintptr_t)&borrowed_field_hits);
    mips_load(e,M_T5,M_T4,0); mips_i(e,9,M_T5,M_T5,1); mips_store(e,M_T5,M_T4,0);
    if (!mips_patch(e,no_probe,e->count)) e->refused = true;
    mips_i(e,9,M_A1,M_A1,-1);
    mips_store(e,M_A1,M_A0,offsetof(JSContext,interrupt_counter));
    mips_i(e,9,M_S3,M_S3,b.count);
    immediate32(e,M_S4,b.target);
    mips_fix(e,fix,mips_branch(e,!b.if_true,M_T0,M_ZERO),borrowed_probe_exit ? TF_CHAIN_EXIT : b.target);
    immediate32(e,M_S4,b.end); mips_jump(e,fix,borrowed_probe_exit ? TF_CHAIN_EXIT : b.end);
    plan->borrowed_field_blocks++;
    *end = b.end;
    return !e->refused;
}
#else
static bool mips_borrowed_block(Emitter *e, TFChainFixups *fix,
                                TFRegionPlan *plan, unsigned at, unsigned *end)
{
    (void)e; (void)fix; (void)plan; (void)at; (void)end;
    return false;
}
#endif
