/* Isolated reference-aware admission and bounded ownership commit.
   Simulate the original refcount operations, refusing before any decrement
   could run a destructor. All values remain borrowed from the unchanged
   interpreter stack/bindings/object graph until all guards pass. */
#ifdef TF_REGION_SEMANTIC_CONTINUATIONS
enum { TF_REFERENCE_ACCEPT, TF_REFERENCE_STACK, TF_REFERENCE_VALUE,
       TF_REFERENCE_TRUTH, TF_REFERENCE_EQUALITY, TF_REFERENCE_INTEGER,
       TF_REFERENCE_FIELD, TF_REFERENCE_DESTRUCTOR, TF_REFERENCE_OVERFLOW,
       TF_REFERENCE_POLL, TF_REFERENCE_GUARD_COUNT };
#define TF_REFERENCE_RC_LIMIT (TF_PRIMITIVE_BLOCK_LIMIT * 2u + TF_PRIMITIVE_STACK_LIMIT)
#define TF_REFERENCE_BLOCK_LIMIT 128u
static bool tf_reference_probes;
static unsigned tf_reference_hits;
typedef struct {
    JSRefCountHeader *header;
    int initial, delta;
} TFReferenceCount;
typedef struct {
    JSValue values[TF_PRIMITIVE_STACK_LIMIT]; /* at the branch or handoff */
    TFReferenceCount refs[TF_REFERENCE_RC_LIMIT];
    unsigned depth, ref_count, fields, inherited_fields, own_fields, next;
    unsigned acquires, releases;
    bool condition;
} TFReferenceResult;

/* Diagnostic frontier categories, not execution admission. Classify the
   original handoff instruction; never credit it to the preceding region. */
enum { TF_REFERENCE_CONDITIONAL, TF_REFERENCE_BINDING_WRITE,
       TF_REFERENCE_CALL, TF_REFERENCE_PROPERTY_WRITE, TF_REFERENCE_RETURN,
       TF_REFERENCE_COUNT_LIMIT, TF_REFERENCE_STACK_LIMIT, TF_REFERENCE_OTHER,
       TF_REFERENCE_FRONTIER_COUNT };
static unsigned tf_reference_frontier(JSFunctionBytecode *body,
                                      const TFPrimitiveBlock *b)
{
    if (b->conditional) return TF_REFERENCE_CONDITIONAL;
    if (b->count == TF_PRIMITIVE_BLOCK_LIMIT) return TF_REFERENCE_COUNT_LIMIT;
    if (b->end >= (unsigned)body->byte_code_len) return TF_REFERENCE_OTHER;
    const char *name = tf_region_opcodes[body->byte_code_buf[b->end]].name;
    if (!strncmp(name,"put_loc",7) || !strncmp(name,"set_loc",7) ||
        !strncmp(name,"put_arg",7) || !strncmp(name,"set_arg",7) ||
        !strncmp(name,"put_var_ref",11) || !strncmp(name,"set_var_ref",11))
        return TF_REFERENCE_BINDING_WRITE;
    if (!strncmp(name,"call",4) || !strncmp(name,"tail_call",9))
        return TF_REFERENCE_CALL;
    if (!strncmp(name,"put_field",9) || !strncmp(name,"put_array_el",12) ||
        !strncmp(name,"define_field",12)) return TF_REFERENCE_PROPERTY_WRITE;
    if (!strncmp(name,"return",6)) return TF_REFERENCE_RETURN;
    uint32_t argument;
    TFRegionHelper helper = tf_region_decode(body->byte_code_buf+b->end,&argument);
    if (tf_primitive_source_valid(body,helper,argument) || helper == tf_region_immediate ||
        helper == tf_region_dup) return TF_REFERENCE_STACK_LIMIT;
    return TF_REFERENCE_OTHER;
}

/* A reference region may hand off BEFORE a call, write, getter-capable op or
   return, rather than requiring a conditional at its tail. Never credit that
   unsupported operation. Interior entries remain interpreter-owned. */
static bool tf_reference_scan(JSFunctionBytecode *body, const uint8_t *boundaries,
                              unsigned start, TFPrimitiveBlock *b)
{
    if (tf_value_scan(body, boundaries, start, b, true)) return true;
    unsigned cursor = start, length = body->byte_code_len;
    int depth = 0, minimum = 0, maximum = 0;
    memset(b, 0, sizeof(*b));
    for (unsigned i = 0; i < TF_PRIMITIVE_BLOCK_LIMIT && cursor < length; i++) {
        unsigned op = body->byte_code_buf[cursor], size = tf_region_opcodes[op].size;
        if (!size || size > length - cursor) return false;
        uint32_t argument;
        TFRegionHelper helper = tf_region_decode(body->byte_code_buf + cursor, &argument);
        if (!(tf_primitive_source_valid(body,helper,argument) || helper == tf_region_immediate ||
              helper == tf_region_drop || helper == tf_region_dup || helper == tf_region_field ||
              helper == tf_region_less || (helper == tf_region_semantic &&
              (op == OP_lnot || op == OP_is_null || op == OP_is_undefined ||
               op == OP_is_undefined_or_null || op == OP_strict_eq || op == OP_strict_neq)))) break;
        int new_depth = depth - tf_region_opcodes[op].pop;
        int new_minimum = new_depth < minimum ? new_depth : minimum;
        new_depth += tf_region_opcodes[op].push;
        int new_maximum = new_depth > maximum ? new_depth : maximum;
        if (new_maximum - new_minimum > TF_PRIMITIVE_STACK_LIMIT) break;
        b->offsets[b->count] = cursor;
        b->opcodes[b->count] = op;
        b->helpers[b->count] = helper;
        b->arguments[b->count++] = argument;
        depth = new_depth; minimum = new_minimum; maximum = new_maximum;
        cursor += size;
    }
    if (b->count < 2 || cursor == length || (boundaries && !boundaries[cursor])) return false;
    b->incoming = (unsigned)-minimum;
    b->peak = (unsigned)maximum;
    b->target = b->end = cursor;
    return true;
}

static bool tf_borrowed_field_scan(JSFunctionBytecode *body, const uint8_t *boundaries,
                                   unsigned start, TFPrimitiveBlock *b)
{
    if (!tf_value_scan(body,boundaries,start,b,true) || b->count < 3 || b->count > 6 ||
        b->incoming || b->peak != 1 || !b->conditional ||
        !tf_primitive_source_valid(body,b->helpers[0],b->arguments[0]) ||
        b->helpers[0] == tf_region_integer) return false;
    for (unsigned i = 1; i < b->count-1; i++) if (b->helpers[i] != tf_region_field) return false;
    return true;
}

static bool tf_reference_value(JSValue value)
{
    int tag = JS_VALUE_GET_NORM_TAG(value);
    return tf_primitive_value(value) || tag_is_string(tag) || tag == JS_TAG_OBJECT ||
           tag == JS_TAG_SYMBOL || tag == JS_TAG_FLOAT64 ||
           tag == JS_TAG_BIG_INT || tag == JS_TAG_SHORT_BIG_INT;
}

static unsigned tf_reference_count(TFReferenceResult *r, JSValue value, int change)
{
    if (!JS_VALUE_HAS_REF_COUNT(value)) return TF_REFERENCE_ACCEPT;
    if (change > 0) r->acquires++;
    else r->releases++;
    JSRefCountHeader *header = __js_rc(JS_VALUE_GET_PTR(value));
    unsigned at;
    for (at = 0; at < r->ref_count; at++) if (r->refs[at].header == header) break;
    if (at == r->ref_count) {
        if (at == TF_REFERENCE_RC_LIMIT) return TF_REFERENCE_OVERFLOW;
        r->refs[at] = (TFReferenceCount){header, header->ref_count, 0};
        r->ref_count++;
    }
    TFReferenceCount *ref = &r->refs[at];
    int64_t count = (int64_t)ref->initial + ref->delta + change;
    if (count <= 0) return TF_REFERENCE_DESTRUCTOR;
    if (count > INT32_MAX) return TF_REFERENCE_OVERFLOW;
    ref->delta += change;
    return TF_REFERENCE_ACCEPT;
}

static unsigned tf_reference_truth(JSValue value, bool *truth)
{
    int tag = JS_VALUE_GET_NORM_TAG(value);
    if (tag == JS_TAG_INT || tag == JS_TAG_BOOL) *truth = JS_VALUE_GET_INT(value) != 0;
    else if (tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED) *truth = false;
    else if (tag == JS_TAG_STRING) *truth = JS_VALUE_GET_STRING(value)->len != 0;
    else if (tag == JS_TAG_STRING_ROPE) *truth = JS_VALUE_GET_STRING_ROPE(value)->len != 0;
    else if (tag == JS_TAG_OBJECT) *truth = !JS_VALUE_GET_OBJ(value)->is_HTMLDDA;
    else if (tag == JS_TAG_SYMBOL) *truth = true;
    else if (tag == JS_TAG_SHORT_BIG_INT) *truth = JS_VALUE_GET_SHORT_BIG_INT(value) != 0;
    else if (tag == JS_TAG_FLOAT64) {
        double number = JS_VALUE_GET_FLOAT64(value);
        *truth = number != 0 && !isnan(number);
    } else return TF_REFERENCE_TRUTH; /* no limb scan or author conversion */
    return TF_REFERENCE_ACCEPT;
}

static unsigned tf_reference_equal(JSValue left, JSValue right, bool *equal)
{
    int lt = JS_VALUE_GET_NORM_TAG(left), rt = JS_VALUE_GET_NORM_TAG(right);
    if ((lt == JS_TAG_INT || lt == JS_TAG_FLOAT64) &&
        (rt == JS_TAG_INT || rt == JS_TAG_FLOAT64)) {
        if (lt != JS_TAG_INT || rt != JS_TAG_INT) return TF_REFERENCE_EQUALITY;
        *equal = JS_VALUE_GET_INT(left) == JS_VALUE_GET_INT(right);
    } else if (tag_is_string(lt) && tag_is_string(rt)) {
        if (JS_VALUE_GET_PTR(left) != JS_VALUE_GET_PTR(right)) return TF_REFERENCE_EQUALITY;
        *equal = true; /* never flatten ropes or compare unbounded text */
    } else if (lt == JS_TAG_BIG_INT || lt == JS_TAG_SHORT_BIG_INT ||
               rt == JS_TAG_BIG_INT || rt == JS_TAG_SHORT_BIG_INT) {
        return TF_REFERENCE_EQUALITY;
    } else if (lt != rt) *equal = false;
    else if (lt == JS_TAG_OBJECT || lt == JS_TAG_SYMBOL)
        *equal = JS_VALUE_GET_PTR(left) == JS_VALUE_GET_PTR(right);
    else if (lt == JS_TAG_NULL || lt == JS_TAG_UNDEFINED) *equal = true;
    else if (lt == JS_TAG_BOOL) *equal = JS_VALUE_GET_INT(left) == JS_VALUE_GET_INT(right);
    else return TF_REFERENCE_EQUALITY;
    return TF_REFERENCE_ACCEPT;
}

/* Ordinary data properties only; both prototype and collision walks have
   fixed ceilings. Never touch a getter, proxy, autoinit or live var-ref. */
static unsigned tf_reference_field(JSValue receiver, JSAtom atom, JSValue *value,
                                   bool *inherited, bool *own)
{
    int tag = JS_VALUE_GET_NORM_TAG(receiver);
    *inherited = false;
    *own = false;
    if (atom == JS_ATOM_length && tag_is_string(tag)) {
        uint32_t length = tag == JS_TAG_STRING ? JS_VALUE_GET_STRING(receiver)->len :
                                               JS_VALUE_GET_STRING_ROPE(receiver)->len;
        if (length > INT32_MAX) return TF_REFERENCE_FIELD;
        *value = JS_NewInt32(NULL, length);
        return TF_REFERENCE_ACCEPT;
    }
    if (tag != JS_TAG_OBJECT) return TF_REFERENCE_FIELD;
    JSObject *object = JS_VALUE_GET_OBJ(receiver);
    for (unsigned level = 0; level < 8; level++) {
        if (object->is_exotic) return TF_REFERENCE_FIELD;
        JSShape *shape = object->shape;
        JSShapeProperty *properties = get_shape_prop(shape);
        uint32_t hash = shape->hash_table[(uintptr_t)atom & shape->prop_hash_mask];
        for (unsigned chain = 0; hash && chain < 32; chain++) {
            if (hash > (unsigned)shape->prop_count) return TF_REFERENCE_FIELD;
            JSShapeProperty *property = &properties[hash - 1];
            if (property->atom == atom) {
                if (property->flags & JS_PROP_TMASK) return TF_REFERENCE_FIELD;
                *value = object->prop[hash - 1].u.value;
                *inherited = level != 0;
                *own = level == 0;
                return TF_REFERENCE_ACCEPT;
            }
            hash = property->hash_next;
        }
        if (hash) return TF_REFERENCE_FIELD;
        if (!shape->proto) { *value = JS_UNDEFINED; return TF_REFERENCE_ACCEPT; }
        object = shape->proto;
    }
    return TF_REFERENCE_FIELD;
}

static unsigned tf_reference_guards(JSContext *ctx, JSFunctionBytecode *body,
    const TFPrimitiveBlock *b, JSValue *base, JSValue *limit, JSValue *sp,
    JSValue *locals, JSValue *args, JSVarRef **refs, TFReferenceResult *r)
{
    memset(r, 0, sizeof(*r));
    if (sp - base < (ptrdiff_t)b->incoming || limit - sp < (ptrdiff_t)b->peak)
        return TF_REFERENCE_STACK;
    if (ctx->interrupt_counter < 2) return TF_REFERENCE_POLL;
    unsigned depth = b->incoming;
    for (unsigned i = 0; i < depth; i++) {
        r->values[i] = sp[(int)i - (int)b->incoming];
        if (!tf_reference_value(r->values[i])) return TF_REFERENCE_VALUE;
    }
    for (unsigned i = 0; i < b->count; i++) {
        TFRegionHelper helper = b->helpers[i];
        unsigned op = b->opcodes[i], index = b->arguments[i];
        unsigned status = TF_REFERENCE_ACCEPT;
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
                if (!refs || !refs[index] || !refs[index]->pvalue) return TF_REFERENCE_VALUE;
                value = *refs[index]->pvalue;
            }
            if (!tf_reference_value(value)) return TF_REFERENCE_VALUE;
            status = tf_reference_count(r, value, 1);
            r->values[depth++] = value;
        } else if (helper == tf_region_drop) {
            status = tf_reference_count(r, r->values[--depth], -1);
        } else if (helper == tf_region_dup) {
            status = tf_reference_count(r, r->values[depth - 1], 1);
            r->values[depth] = r->values[depth - 1]; depth++;
        } else if (helper == tf_region_field) {
            JSValue receiver = r->values[depth - 1], value;
            bool inherited, own;
            status = tf_reference_field(receiver, index, &value, &inherited, &own);
            if (status) return status;
            if (!tf_reference_value(value)) return TF_REFERENCE_VALUE;
            status = tf_reference_count(r, value, 1);
            if (status) return status;
            status = tf_reference_count(r, receiver, -1);
            r->values[depth - 1] = value;
            r->fields++;
            r->inherited_fields += inherited;
            r->own_fields += own;
        } else if (helper == tf_region_branch) {
            r->depth = depth; /* preserve the pre-branch interpreter snapshot */
            status = tf_reference_truth(r->values[depth - 1], &r->condition);
            if (status) return status;
            r->next = r->condition == b->if_true ? b->target : b->end;
            status = tf_reference_count(r, r->values[depth - 1], -1);
        } else if (helper == tf_region_less || op == OP_strict_eq || op == OP_strict_neq) {
            JSValue left = r->values[depth - 2], right = r->values[depth - 1];
            bool result;
            if (helper == tf_region_less) {
                if (JS_VALUE_GET_NORM_TAG(left) != JS_TAG_INT ||
                    JS_VALUE_GET_NORM_TAG(right) != JS_TAG_INT) return TF_REFERENCE_INTEGER;
                result = JS_VALUE_GET_INT(left) < JS_VALUE_GET_INT(right);
            } else {
                status = tf_reference_equal(left, right, &result);
                if (status) return status;
                result ^= op == OP_strict_neq;
            }
            status = tf_reference_count(r, left, -1);
            if (status) return status;
            status = tf_reference_count(r, right, -1);
            r->values[depth - 2] = JS_NewBool(ctx, result); depth--;
        } else {
            JSValue value = r->values[depth - 1];
            bool result;
            if (op == OP_lnot) {
                status = tf_reference_truth(value, &result);
                if (status) return status;
                result = !result;
            } else result = op == OP_is_null ? JS_IsNull(value) :
                op == OP_is_undefined ? JS_IsUndefined(value) : JS_IsNull(value) || JS_IsUndefined(value);
            status = tf_reference_count(r, value, -1);
            r->values[depth - 1] = JS_NewBool(ctx, result);
        }
        if (status) return status;
    }
    if (!b->conditional) { r->depth = depth; r->next = b->end; }
    return TF_REFERENCE_ACCEPT;
}

/* All speculative work is read-only. Commit only after every original stack,
   type, ownership and poll guard passes. Net RC deltas cannot destroy anything,
   and no allocation/callback/GC is possible between validation and publication. */
static int tf_reference_execute(TFRegionFrame *f, uint32_t index, uint32_t unused)
{
    (void)unused;
    TFRegionPlan *plan = f->plan;
    if (!plan || plan->body != f->body || index >= plan->reference_count) return 0;
    const TFPrimitiveBlock *b = &plan->reference_blocks[index];
    if (!f->ctx || f->pc != f->body->byte_code_buf + b->offsets[0] ||
        f->completed > TF_REGION_QUANTUM - b->count) return 0;
    TFReferenceResult r;
    if (tf_reference_guards(f->ctx,f->body,b,f->base,f->limit,f->sp,
                            f->locals,f->args,f->refs,&r) != TF_REFERENCE_ACCEPT) return 0;
    unsigned depth = r.depth - b->conditional;
    JSValue *destination = f->sp - b->incoming;
    for (unsigned i = 0; i < r.ref_count; i++)
        if (r.refs[i].delta) r.refs[i].header->ref_count += r.refs[i].delta;
    for (unsigned i = 0; i < depth; i++) destination[i] = r.values[i];
    f->sp = destination + depth;
    f->pc = f->body->byte_code_buf + r.next;
    f->completed += b->count;
    if (b->conditional) f->ctx->interrupt_counter--;
    if (tf_reference_probes) { f->reference_block_hits++; tf_reference_hits++; }
    return b->conditional && r.next == b->target ? 2 : 1;
}

static bool tf_reference_same(JSValue a, JSValue b)
{
    int tag = JS_VALUE_GET_NORM_TAG(a);
    if (tag != JS_VALUE_GET_NORM_TAG(b)) return false;
    if (JS_VALUE_HAS_REF_COUNT(a)) return JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
    if (tag == JS_TAG_INT || tag == JS_TAG_BOOL) return JS_VALUE_GET_INT(a) == JS_VALUE_GET_INT(b);
    if (tag == JS_TAG_FLOAT64) {
        double av = JS_VALUE_GET_FLOAT64(a), bv = JS_VALUE_GET_FLOAT64(b);
        return memcmp(&av,&bv,sizeof(av)) == 0;
    }
    if (tag == JS_TAG_SHORT_BIG_INT)
        return JS_VALUE_GET_SHORT_BIG_INT(a) == JS_VALUE_GET_SHORT_BIG_INT(b);
    return tag == JS_TAG_NULL || tag == JS_TAG_UNDEFINED;
}
#endif
