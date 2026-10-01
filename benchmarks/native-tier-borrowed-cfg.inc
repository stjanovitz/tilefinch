/* Compile-only read-only CFG. Every value originates in an unchanged rooted
   binding/constant or a property graph reachable from one. Calls, writes and
   unsupported operations are explicit owned-state materialization points.
   This is not executable admission by pointer identity or page-controlled IR. */
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
#define TF_BORROWED_CFG_LIMIT 128u
enum { TF_CFG_EXIT, TF_CFG_SOURCE, TF_CFG_FIELD, TF_CFG_FIELD_KEEP,
       TF_CFG_DUP, TF_CFG_DROP, TF_CFG_TEST, TF_CFG_BRANCH };
typedef struct {
    uint16_t pc, next, target;
    uint32_t argument;
    TFRegionHelper helper;
    uint8_t kind, depth, opcode, conditional;
    unsigned emitted;
} TFBorrowedCFGNode;
typedef struct {
    int16_t map[TF_REGION_BYTECODE_LIMIT];
    TFBorrowedCFGNode nodes[TF_BORROWED_CFG_LIMIT];
    unsigned count, branches, peak, supported;
} TFBorrowedCFG;

static bool tf_cfg_add(TFBorrowedCFG *g, unsigned pc, unsigned depth)
{
    if (pc >= TF_REGION_BYTECODE_LIMIT || depth > TF_PRIMITIVE_STACK_LIMIT) return false;
    if (g->map[pc] >= 0) return g->nodes[g->map[pc]].depth == depth;
    if (g->count == TF_BORROWED_CFG_LIMIT) return false;
    g->map[pc] = (int16_t)g->count;
    g->nodes[g->count++] = (TFBorrowedCFGNode){.pc=pc,.depth=depth};
    return true;
}

static bool tf_cfg_scan(JSFunctionBytecode *body, const uint8_t *boundaries,
                        unsigned start, TFBorrowedCFG *g)
{
    unsigned length = body->byte_code_len;
    if (!length || length > TF_REGION_BYTECODE_LIMIT || start >= length ||
        body->func_kind != JS_FUNC_NORMAL || (boundaries && !boundaries[start])) return false;
    memset(g,0,sizeof(*g));
    memset(g->map,0xff,sizeof(g->map));
    if (!tf_cfg_add(g,start,0)) return false;
    for (unsigned i = 0; i < g->count; i++) {
        TFBorrowedCFGNode *n = &g->nodes[i];
        const uint8_t *pc = body->byte_code_buf+n->pc;
        unsigned size = tf_region_opcodes[*pc].size;
        if (!size || size > length-n->pc) return false;
        n->opcode = *pc; n->next = n->pc+size;
        n->helper = tf_region_decode(pc,&n->argument);
        if (tf_primitive_source_valid(body,n->helper,n->argument) || n->helper == tf_region_immediate)
            n->kind = TF_CFG_SOURCE;
        else if (n->helper == tf_region_field) n->kind = TF_CFG_FIELD;
        else if (*pc == OP_get_field2) { n->kind=TF_CFG_FIELD_KEEP; n->argument=get_u32(pc+1); }
        else if (n->helper == tf_region_dup) n->kind = TF_CFG_DUP;
        else if (n->helper == tf_region_drop) n->kind = TF_CFG_DROP;
        else if (*pc == OP_lnot || *pc == OP_is_null || *pc == OP_is_undefined ||
                 *pc == OP_is_undefined_or_null) n->kind = TF_CFG_TEST;
        else if (n->helper == tf_region_branch) {
            int32_t delta = *pc == OP_goto16 ? (int16_t)get_u16(pc+1) :
                (*pc == OP_goto || *pc == OP_if_false || *pc == OP_if_true) ?
                (int32_t)get_u32(pc+1) : (int8_t)pc[1];
            int64_t target = (int64_t)n->pc+1+delta;
            if (target < 0 || target >= length || (boundaries && !boundaries[target])) return false;
            n->target = (unsigned)target;
            n->conditional = *pc == OP_if_false || *pc == OP_if_true ||
                *pc == OP_if_false8 || *pc == OP_if_true8;
            n->kind = TF_CFG_BRANCH;
        }
        unsigned pop = n->kind == TF_CFG_SOURCE ? 0 :
            n->kind == TF_CFG_BRANCH ? n->conditional : n->kind == TF_CFG_EXIT ? 0 : 1;
        unsigned push = n->kind == TF_CFG_SOURCE || n->kind == TF_CFG_FIELD ||
            n->kind == TF_CFG_TEST ? 1 : n->kind == TF_CFG_DUP || n->kind == TF_CFG_FIELD_KEEP ? 2 : 0;
        if (n->depth < pop || n->depth-pop+push > TF_PRIMITIVE_STACK_LIMIT ||
            (n->next == length && n->kind != TF_CFG_EXIT)) n->kind = TF_CFG_EXIT;
        if (!n->kind) continue;
        unsigned outgoing = n->depth-pop+push;
        if (outgoing > g->peak) g->peak = outgoing;
        if (n->kind == TF_CFG_BRANCH) {
            g->branches++;
            if (!tf_cfg_add(g,n->target,outgoing)) return false;
            if (!n->conditional) { g->supported++; continue; }
        }
        if (!tf_cfg_add(g,n->next,outgoing)) return false;
        g->supported++;
    }
    return g->branches >= 2 && g->supported >= 8;
}

/* Portable read-only oracle; export owned values only at the exact handoff.
   It has no authority to invoke a getter, callback, allocation or finalizer. */
static bool tf_cfg_value(JSValue value)
{
    int tag=JS_VALUE_GET_NORM_TAG(value);
    if (tag == JS_TAG_UNINITIALIZED || tag == JS_TAG_EXCEPTION || tag == JS_TAG_CATCH_OFFSET) return false;
    if (!JS_VALUE_HAS_REF_COUNT(value)) return true;
    int count=__js_rc(JS_VALUE_GET_PTR(value))->ref_count;
    return count > 0 && count <= INT32_MAX-4;
}
static void tf_cfg_predict(TFRegionFrame *f, const TFBorrowedCFG *g)
{
    if (f->sp != f->base || f->limit-f->sp < (ptrdiff_t)g->peak) return;
    JSValue values[TF_PRIMITIVE_STACK_LIMIT]; unsigned depth=0;
    unsigned pc=g->nodes[0].pc;
    for (unsigned tick=0; tick<=TF_REGION_QUANTUM; tick++) {
        if (pc >= TF_REGION_BYTECODE_LIMIT || g->map[pc] < 0) break;
        const TFBorrowedCFGNode *n=&g->nodes[g->map[pc]];
        if (n->depth != depth || !n->kind) break;
        if (f->completed == TF_REGION_QUANTUM) { f->quota_exit=true; break; }
        JSValue value=JS_UNDEFINED;
        if (n->kind == TF_CFG_SOURCE) {
            TFRegionHelper h=n->helper;
            if (h == tf_region_integer) value=JS_NewInt32(f->ctx,(int32_t)n->argument);
            else if (h == tf_region_immediate) value=n->argument == OP_null ? JS_NULL :
                n->argument == OP_undefined ? JS_UNDEFINED : JS_NewBool(f->ctx,n->argument == OP_push_true);
            else if (h == tf_region_arg) { if (!f->args) break; value=f->args[n->argument]; }
            else if (h == tf_region_local || h == tf_region_local_check) {
                if (!f->locals) break; value=f->locals[n->argument];
            } else if (h == tf_region_constant) value=f->body->cpool[n->argument];
            else {
                if (!f->refs || !f->refs[n->argument] || !f->refs[n->argument]->pvalue) break;
                value=*f->refs[n->argument]->pvalue;
            }
            if (!tf_cfg_value(value)) break;
            values[depth++]=value;
        } else if (n->kind == TF_CFG_FIELD || n->kind == TF_CFG_FIELD_KEEP) {
            bool inherited,own;
            if (tf_reference_field(values[depth-1],n->argument,&value,&inherited,&own) ||
                !own || !tf_cfg_value(value)) break;
            if (n->kind == TF_CFG_FIELD_KEEP) values[depth++]=value;
            else values[depth-1]=value;
        } else if (n->kind == TF_CFG_DUP) { values[depth]=values[depth-1]; depth++; }
        else if (n->kind == TF_CFG_DROP) depth--;
        else if (n->kind == TF_CFG_TEST) {
            bool result;
            value=values[depth-1];
            if (n->opcode == OP_lnot) {
                if (tf_reference_truth(value,&result) || JS_VALUE_GET_NORM_TAG(value) == JS_TAG_FLOAT64) break;
                result=!result;
            } else result=n->opcode == OP_is_null ? JS_IsNull(value) :
                n->opcode == OP_is_undefined ? JS_IsUndefined(value) : JS_IsNull(value) || JS_IsUndefined(value);
            values[depth-1]=JS_NewBool(f->ctx,result);
        } else if (n->kind == TF_CFG_BRANCH) {
            bool truth=false;
            if (n->conditional && (tf_reference_truth(values[depth-1],&truth) ||
                JS_VALUE_GET_NORM_TAG(values[depth-1]) == JS_TAG_FLOAT64)) break;
            if (f->ctx->interrupt_counter < 2) break;
            if (n->conditional) depth--;
            f->ctx->interrupt_counter--; f->completed++;
            bool if_true=n->opcode == OP_if_true || n->opcode == OP_if_true8;
            pc=!n->conditional || truth == if_true ? n->target : n->next;
            continue;
        }
        f->completed++; pc=n->next;
    }
    f->pc=f->body->byte_code_buf+pc;
    for (unsigned i=0; i<depth; i++) f->base[i]=JS_DupValue(f->ctx,values[i]);
    f->sp=f->base+depth;
}
#endif
