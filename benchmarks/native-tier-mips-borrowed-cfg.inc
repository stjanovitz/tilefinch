/* Isolated Allegrex CFG lowering. Borrowed stack pairs survive branch edges.
   A miss materializes the precise pre-operation owned stack, never replays
   earlier branches/polls and never invokes a callback with unrooted values. */
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
static const unsigned cfg_payloads[3] = {M_S5,M_S7,M_T8};
static const unsigned cfg_tags[3] = {M_S6,M_T7,M_T9};
static void mips_cfg_copy(Emitter *e, unsigned p, unsigned t, unsigned from_p, unsigned from_t)
{
    mips_r(e,33,p,from_p,M_ZERO); mips_r(e,33,t,from_t,M_ZERO);
}
static void mips_cfg_value(Emitter *e, TFChainFixups *fix)
{
    immediate32(e,M_T4,JS_TAG_UNINITIALIZED);
    mips_exit_if(e,fix,true,M_T1,M_T4);
    immediate32(e,M_T4,JS_TAG_EXCEPTION); mips_exit_if(e,fix,true,M_T1,M_T4);
    immediate32(e,M_T4,JS_TAG_CATCH_OFFSET); mips_exit_if(e,fix,true,M_T1,M_T4);
    immediate32(e,M_T4,(uint32_t)JS_TAG_FIRST);
    mips_r(e,43,M_T4,M_T1,M_T4);
    unsigned immediate = mips_branch(e,false,M_T4,M_ZERO);
    mips_i(e,35,M_T4,M_T0,-4); /* signed reference-prefix displacement */
    mips_i(e,10,M_T5,M_T4,1); mips_exit_if(e,fix,false,M_T5,M_ZERO);
    immediate32(e,M_T5,INT32_MAX-4);
    mips_r(e,42,M_T5,M_T5,M_T4); mips_exit_if(e,fix,false,M_T5,M_ZERO);
    if (!mips_patch(e,immediate,e->count)) e->refused = true;
}
static void mips_cfg_own_values(Emitter *e, TFChainFixups *fix, unsigned depth)
{
    /* Creation guards reserve headroom for all three live values plus the
       original get_field's acquire-before-release temporary. No GC/callback. */
    for (unsigned i = 0; i < depth; i++) {
        mips_reference(e,fix,true,cfg_payloads[i],cfg_tags[i]);
        mips_store(e,cfg_payloads[i],M_S1,i*8);
        mips_store(e,cfg_tags[i],M_S1,i*8+4);
    }
    mips_i(e,9,M_S1,M_S1,depth*8);
}
static void mips_cfg_materialize(Emitter *e, TFChainFixups *fix, unsigned depth)
{
    mips_cfg_own_values(e,fix,depth);
    mips_jump(e,fix,TF_CHAIN_EXIT);
}
static bool mips_cfg_block(Emitter *e, TFChainFixups *fix, TFRegionPlan *plan, unsigned at)
{
    if (!register_cfg || plan->cfg_regions >= 8 || (plan->steps[at] & 2) || e->binding_kind ||
        js_check_stack_overflow(plan->body->realm->rt,2048) ||
        !tf_cfg_scan(plan->body,plan->steps,at,&fix->cfg)) return false;
    TFBorrowedCFG *g = &fix->cfg;
    mips_load(e,M_T4,M_S0,offsetof(TFRegionFrame,base));
    mips_exit_if(e,fix,false,M_S1,M_T4); /* owned incoming values stay interpreted */
    mips_load(e,M_T4,M_S0,offsetof(TFRegionFrame,limit));
    mips_i(e,9,M_T5,M_S1,g->peak*8); mips_r(e,43,M_T4,M_T4,M_T5);
    mips_exit_if(e,fix,false,M_T4,M_ZERO);
    unsigned exotic_offset,exotic_mask,html_offset,html_mask;
    if (!mips_object_bit(false,&exotic_offset,&exotic_mask) ||
        !mips_object_bit(true,&html_offset,&html_mask)) { e->refused=true; return true; }
    JSShapeProperty sample = {0}; uint32_t next_mask,type_mask;
    sample.hash_next=(1u<<26)-1; memcpy(&next_mask,&sample,sizeof(next_mask));
    sample.hash_next=0; sample.flags=JS_PROP_TMASK; memcpy(&type_mask,&sample,sizeof(type_mask));
    if (next_mask != (1u<<26)-1 || !type_mask) { e->refused=true; return true; }
    unsigned edge_words[TF_BORROWED_CFG_LIMIT][2];
    /* Compile scratch is bounded and the compiler's native stack check must
       account for this fixed fixup table (no recursive CFG traversal). */
    unsigned owned_stores=0;
    for (unsigned i = 0; i < g->count; i++) {
        TFBorrowedCFGNode *n = &g->nodes[i]; n->emitted=e->count;
        immediate32(e,M_S4,n->pc);
        if (!n->kind) {
            bool store=n->helper == tf_region_put_local || n->helper == tf_region_put_local_check;
            if (cfg_owned_stores && store && n->depth && n->argument < plan->body->var_count && n->argument <= 255) {
                /* End the borrowing lifetime FIRST. The direct store consumes
                   an owned value, guards TDZ/final release before changes, and
                   cannot invoke a callback. No borrowed register survives it.
                   Subsequent regions reload the now-current bindings. */
                mips_cfg_own_values(e,fix,n->depth);
                mips_i(e,11,M_T0,M_S3,TF_REGION_QUANTUM);
                unsigned quota_ok=mips_branch(e,false,M_T0,M_ZERO);
                immediate32(e,M_T0,1); mips_i(e,40,M_T0,M_S0,offsetof(TFRegionFrame,quota_exit));
                mips_jump(e,fix,TF_CHAIN_EXIT);
                if(!mips_patch(e,quota_ok,e->count)) e->refused=true;
                if(!mips_direct(e,fix,plan->body,n->helper,n->argument,n->pc)) e->refused=true;
                immediate32(e,M_S4,n->next);
                uint32_t unused;
                unsigned target=!cfg_owned_probe_exit && n->next < plan->body->byte_code_len &&
                    tf_region_decode(plan->body->byte_code_buf+n->next,&unused) ? n->next : TF_CHAIN_EXIT;
                mips_jump(e,fix,target);
                owned_stores++;
            } else mips_cfg_materialize(e,fix,n->depth);
            continue;
        }
        unsigned guards = fix->count;
        mips_i(e,11,M_T4,M_S3,TF_REGION_QUANTUM);
        unsigned quota_ok=mips_branch(e,false,M_T4,M_ZERO);
        immediate32(e,M_T4,1); mips_i(e,40,M_T4,M_S0,offsetof(TFRegionFrame,quota_exit));
        mips_jump(e,fix,TF_CHAIN_EXIT);
        if (!mips_patch(e,quota_ok,e->count)) e->refused=true;
        unsigned out = n->depth;
        if (n->kind == TF_CFG_SOURCE) {
            bool ref=n->helper == tf_region_ref || n->helper == tf_region_ref_check;
            bool local=n->helper == tf_region_local || n->helper == tf_region_local_check;
            if (ref) {
                mips_load(e,M_T4,M_S0,offsetof(TFRegionFrame,refs)); mips_exit_if(e,fix,true,M_T4,M_ZERO);
                mips_load(e,M_T4,M_T4,n->argument*4); mips_exit_if(e,fix,true,M_T4,M_ZERO);
                mips_load(e,M_T4,M_T4,offsetof(JSVarRef,pvalue)); mips_exit_if(e,fix,true,M_T4,M_ZERO);
            } else if (local || n->helper == tf_region_arg) {
                mips_load(e,M_T4,M_S0,local ? offsetof(TFRegionFrame,locals) : offsetof(TFRegionFrame,args));
                mips_exit_if(e,fix,true,M_T4,M_ZERO);
            }
            if (n->helper == tf_region_immediate) {
                immediate32(e,M_T0,n->argument == OP_push_true);
                immediate32(e,M_T1,n->argument == OP_null ? JS_TAG_NULL :
                    n->argument == OP_undefined ? JS_TAG_UNDEFINED : JS_TAG_BOOL);
            } else mips_block_source(e,n->helper,n->argument,M_T0,M_T1);
            mips_cfg_value(e,fix);
            mips_cfg_copy(e,cfg_payloads[out],cfg_tags[out],M_T0,M_T1); out++;
        } else if (n->kind == TF_CFG_FIELD || n->kind == TF_CFG_FIELD_KEEP) {
            mips_cfg_copy(e,M_T0,M_T1,cfg_payloads[out-1],cfg_tags[out-1]);
            mips_borrowed_field(e,fix,n->argument,exotic_offset,exotic_mask,next_mask,type_mask);
            mips_cfg_value(e,fix);
            unsigned destination = n->kind == TF_CFG_FIELD_KEEP ? out++ : out-1;
            mips_cfg_copy(e,cfg_payloads[destination],cfg_tags[destination],M_T0,M_T1);
        } else if (n->kind == TF_CFG_DUP) {
            mips_cfg_copy(e,cfg_payloads[out],cfg_tags[out],cfg_payloads[out-1],cfg_tags[out-1]); out++;
        } else if (n->kind == TF_CFG_DROP) out--;
        else if (n->kind == TF_CFG_TEST) {
            mips_cfg_copy(e,M_T0,M_T1,cfg_payloads[out-1],cfg_tags[out-1]);
            if (n->opcode == OP_lnot) {
                mips_borrowed_truth(e,fix,html_offset,html_mask); mips_i(e,14,M_T0,M_T0,1);
            } else if (n->opcode == OP_is_undefined_or_null) {
                mips_i(e,9,M_T0,M_T1,-JS_TAG_NULL); mips_i(e,11,M_T0,M_T0,2);
            } else {
                mips_i(e,14,M_T0,M_T1,n->opcode == OP_is_null ? JS_TAG_NULL : JS_TAG_UNDEFINED);
                mips_i(e,11,M_T0,M_T0,1);
            }
            immediate32(e,M_T1,JS_TAG_BOOL);
            mips_cfg_copy(e,cfg_payloads[out-1],cfg_tags[out-1],M_T0,M_T1);
        } else if (n->kind == TF_CFG_BRANCH) {
            if (n->conditional) {
                mips_cfg_copy(e,M_T0,M_T1,cfg_payloads[out-1],cfg_tags[out-1]);
                mips_borrowed_truth(e,fix,html_offset,html_mask); out--;
            }
            mips_load(e,M_A0,M_S0,offsetof(TFRegionFrame,ctx));
            mips_load(e,M_A1,M_A0,offsetof(JSContext,interrupt_counter));
            mips_i(e,10,M_T4,M_A1,2); mips_exit_if(e,fix,false,M_T4,M_ZERO);
            mips_i(e,9,M_A1,M_A1,-1); mips_store(e,M_A1,M_A0,offsetof(JSContext,interrupt_counter));
        }
        mips_i(e,9,M_S3,M_S3,1);
        if (n->kind == TF_CFG_BRANCH) {
            bool truth=n->opcode == OP_if_true || n->opcode == OP_if_true8;
            edge_words[i][0]=mips_branch(e,n->conditional ? !truth : true,
                n->conditional ? M_T0 : M_ZERO,M_ZERO);
            if (n->conditional) edge_words[i][1]=mips_branch(e,true,M_ZERO,M_ZERO);
        } else edge_words[i][0]=mips_branch(e,true,M_ZERO,M_ZERO);
        unsigned materialize=e->count, end_guards=fix->count;
        for (unsigned f=guards; f<end_guards; f++)
            if (fix->items[f].target != TF_CHAIN_EXIT || !mips_patch(e,fix->items[f].word,materialize)) e->refused=true;
        fix->count=guards;
        mips_cfg_materialize(e,fix,n->depth);
    }
    for (unsigned i=0; i<g->count; i++) {
        TFBorrowedCFGNode *n=&g->nodes[i];
        if (!n->kind) continue;
        unsigned destination=n->kind == TF_CFG_BRANCH ? n->target : n->next;
        if (!mips_patch(e,edge_words[i][0],g->nodes[g->map[destination]].emitted)) e->refused=true;
        if (n->kind == TF_CFG_BRANCH && n->conditional &&
            !mips_patch(e,edge_words[i][1],g->nodes[g->map[n->next]].emitted)) e->refused=true;
    }
    if (!e->refused) {
        for (unsigned i=0;i<g->count;i++) if (g->nodes[i].kind)
            plan->steps[g->nodes[i].pc] |= 2;
        plan->cfg_regions++;
        plan->cfg_owned_stores+=owned_stores;
    }
    return true;
}
#else
static bool mips_cfg_block(Emitter *e, TFChainFixups *fix, TFRegionPlan *plan, unsigned at)
{ (void)e; (void)fix; (void)plan; (void)at; return false; }
#endif
