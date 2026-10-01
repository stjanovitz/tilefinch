/* Shared admission for the isolated Allegrex emitter and count-only lab.
   No allocation, mutation, ownership changes, calls or retained page pointers. */
static bool tf_primitive_source_valid(JSFunctionBytecode *body,
                                       TFRegionHelper helper, uint32_t index)
{
    if (helper == tf_region_integer) return true;
    if (index > 255) return false;
    if (helper == tf_region_local || helper == tf_region_local_check)
        return index < body->var_count;
    if (helper == tf_region_arg) return index < body->arg_count;
    if (helper == tf_region_ref || helper == tf_region_ref_check)
        return index < body->closure_var_count;
    if (helper == tf_region_constant) return index < body->cpool_count;
    return false;
}

#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
#define TF_PRIMITIVE_BLOCK_LIMIT 16u
#define TF_PRIMITIVE_STACK_LIMIT 3u
struct TFValueBlock {
    unsigned offsets[TF_PRIMITIVE_BLOCK_LIMIT], arguments[TF_PRIMITIVE_BLOCK_LIMIT];
    uint8_t opcodes[TF_PRIMITIVE_BLOCK_LIMIT];
    TFRegionHelper helpers[TF_PRIMITIVE_BLOCK_LIMIT];
    unsigned count, incoming, peak, end, target;
    bool if_true, conditional;
};

static bool tf_value_scan(JSFunctionBytecode *body, const uint8_t *boundaries,
                         unsigned start, TFPrimitiveBlock *b, bool fields)
{
    unsigned length = body->byte_code_len, cursor = start;
    int depth = 0, minimum = 0, maximum = 0;
    memset(b, 0, sizeof(*b));
    for (unsigned i = 0; i < TF_PRIMITIVE_BLOCK_LIMIT && cursor < length; i++) {
        unsigned op = body->byte_code_buf[cursor];
        unsigned size = tf_region_opcodes[op].size;
        if (!size || size > length - cursor) return false;
        uint32_t argument;
        TFRegionHelper helper = tf_region_decode(body->byte_code_buf + cursor, &argument);
        unsigned pop, push;
        if (tf_primitive_source_valid(body, helper, argument) || helper == tf_region_immediate) {
            pop = 0; push = 1;
        } else if (helper == tf_region_drop) { pop = 1; push = 0;
        } else if (helper == tf_region_dup) { pop = 1; push = 2;
        } else if (fields && helper == tf_region_field) { pop = 1; push = 1;
        } else if (helper == tf_region_less || (helper == tf_region_semantic &&
                    (op == OP_strict_eq || op == OP_strict_neq))) { pop = 2; push = 1;
        } else if (helper == tf_region_semantic && (op == OP_lnot || op == OP_is_null ||
                    op == OP_is_undefined || op == OP_is_undefined_or_null)) { pop = 1; push = 1;
        } else if (helper == tf_region_branch && (op == OP_if_true || op == OP_if_false ||
                    op == OP_if_true8 || op == OP_if_false8)) { pop = 1; push = 0;
        } else return false;
        b->offsets[i] = cursor;
        b->opcodes[i] = op;
        b->helpers[i] = helper;
        b->arguments[i] = argument;
        b->count++;
        depth -= (int)pop;
        if (depth < minimum) minimum = depth;
        depth += (int)push;
        if (depth > maximum) maximum = depth;
        if (maximum - minimum > TF_PRIMITIVE_STACK_LIMIT) return false;
        if (helper == tf_region_branch) {
            if (b->count < 2) return false;
            int32_t delta = op == OP_if_true || op == OP_if_false ?
                (int32_t)get_u32(body->byte_code_buf + cursor + 1) :
                (int8_t)body->byte_code_buf[cursor + 1];
            int64_t target = (int64_t)cursor + 1 + delta;
            /* Emission requires the complete boundary map. Count-only replay
               scans compiler-owned bytecode and does not authorize execution. */
            if (target < 0 || target >= length || (boundaries && !boundaries[target])) return false;
            b->target = (unsigned)target;
            b->if_true = op == OP_if_true || op == OP_if_true8;
            b->conditional = true;
            b->incoming = (unsigned)-minimum;
            b->peak = (unsigned)maximum;
            b->end = cursor + size;
            return true;
        }
        cursor += size;
    }
    return false;
}

static bool tf_primitive_scan(JSFunctionBytecode *body, const uint8_t *boundaries,
                             unsigned start, TFPrimitiveBlock *b)
{
    return tf_value_scan(body, boundaries, start, b, false);
}

enum { TF_PRIMITIVE_ACCEPT, TF_PRIMITIVE_STACK, TF_PRIMITIVE_TYPE,
       TF_PRIMITIVE_INTEGER, TF_PRIMITIVE_POLL, TF_PRIMITIVE_INPUT_TYPE,
       TF_PRIMITIVE_GUARD_COUNT };
static bool tf_primitive_value(JSValue value)
{
    int tag = JS_VALUE_GET_NORM_TAG(value);
    return tag == JS_TAG_INT || tag == JS_TAG_BOOL ||
           tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED;
}
static unsigned tf_primitive_type_refusal(JSValue value, int *refusal_tag)
{
    if (refusal_tag) *refusal_tag = JS_VALUE_GET_NORM_TAG(value);
    return TF_PRIMITIVE_TYPE;
}

/* A read-only guard oracle, not an evaluator: registers are borrowed copies.
   It predicts the primitive fast path before the real interpreter executes
   the same instructions. Quantum is reported separately by live execution. */
static unsigned tf_primitive_guards(JSContext *ctx, JSFunctionBytecode *body,
    const TFPrimitiveBlock *b, JSValue *base, JSValue *limit, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs, int *refusal_tag)
{
    if (refusal_tag) *refusal_tag = JS_TAG_UNDEFINED;
    if (sp - base < (ptrdiff_t)b->incoming || limit - sp < (ptrdiff_t)b->peak)
        return TF_PRIMITIVE_STACK;
    JSValue values[TF_PRIMITIVE_STACK_LIMIT];
    unsigned depth = b->incoming;
    for (unsigned i = 0; i < depth; i++) {
        values[i] = sp[(int)i - (int)b->incoming];
        if (!tf_primitive_value(values[i])) {
            tf_primitive_type_refusal(values[i], refusal_tag);
            return TF_PRIMITIVE_INPUT_TYPE;
        }
    }
    for (unsigned i = 0; i < b->count - 1; i++) {
        TFRegionHelper helper = b->helpers[i];
        unsigned op = body->byte_code_buf[b->offsets[i]], index = b->arguments[i];
        if (tf_primitive_source_valid(body, helper, index) || helper == tf_region_immediate) {
            JSValue value;
            if (helper == tf_region_integer) value = JS_NewInt32(ctx, (int32_t)index);
            else if (helper == tf_region_immediate)
                value = index == OP_null ? JS_NULL : index == OP_undefined ? JS_UNDEFINED :
                    JS_NewBool(ctx, index == OP_push_true);
            else if (helper == tf_region_arg) value = args[index];
            else if (helper == tf_region_local || helper == tf_region_local_check) value = locals[index];
            else if (helper == tf_region_constant) value = body->cpool[index];
            else {
                if (!refs || !refs[index] || !refs[index]->pvalue) return TF_PRIMITIVE_TYPE;
                value = *refs[index]->pvalue;
            }
            if (!tf_primitive_value(value)) return tf_primitive_type_refusal(value, refusal_tag);
            values[depth++] = value;
        } else if (helper == tf_region_drop) depth--;
        else if (helper == tf_region_dup) { values[depth] = values[depth-1]; depth++; }
        else if (helper == tf_region_less || op == OP_strict_eq || op == OP_strict_neq) {
            JSValue left = values[depth-2], right = values[depth-1];
            int lt = JS_VALUE_GET_NORM_TAG(left), rt = JS_VALUE_GET_NORM_TAG(right);
            bool result;
            if (helper == tf_region_less) {
                if (lt != JS_TAG_INT || rt != JS_TAG_INT) return TF_PRIMITIVE_INTEGER;
                result = JS_VALUE_GET_INT(left) < JS_VALUE_GET_INT(right);
            } else {
                result = lt == rt && (lt == JS_TAG_NULL || lt == JS_TAG_UNDEFINED ||
                    JS_VALUE_GET_INT(left) == JS_VALUE_GET_INT(right));
                result ^= op == OP_strict_neq;
            }
            values[depth-2] = JS_NewBool(ctx, result);
            depth--;
        } else {
            JSValue value = values[depth-1];
            bool nullish = JS_IsNull(value) || JS_IsUndefined(value);
            bool result = op == OP_lnot ? nullish || !JS_VALUE_GET_INT(value) :
                op == OP_is_null ? JS_IsNull(value) :
                op == OP_is_undefined ? JS_IsUndefined(value) : nullish;
            values[depth-1] = JS_NewBool(ctx, result);
        }
    }
    return ctx->interrupt_counter < 2 ? TF_PRIMITIVE_POLL : TF_PRIMITIVE_ACCEPT;
}
#endif
