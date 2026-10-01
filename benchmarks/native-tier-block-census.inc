/* Count-only whole-block candidate model. No execution, emitted code or
   retained page pointers. Every call boundary terminates the current run.
   Instruction coverage is not elapsed-time coverage or guard admission. */
#define TF_VALUE_BLOCK_LIMIT 16u
typedef struct {
    uint64_t total, eligible, runs, excluded;
    uint64_t histogram[TF_VALUE_BLOCK_LIMIT + 1];
    uint64_t kinds[64]; /* source=1, local-write=2, predicate=4, property=8, call=16, branch=32 */
    unsigned length, flags, previous_offset, previous_size;
    int depth, minimum, maximum;
    unsigned maximum_inputs, maximum_capacity;
    const void *previous_body;
} TFValueBlockCensus;

static void tf_value_block_finish(TFValueBlockCensus *c)
{
    if (!c->length) return;
    c->runs++;
    c->histogram[c->length]++;
    c->kinds[c->flags] += c->length;
    unsigned inputs = (unsigned)-c->minimum;
    if (inputs > c->maximum_inputs) c->maximum_inputs = inputs;
    if ((unsigned)c->maximum > c->maximum_capacity)
        c->maximum_capacity = (unsigned)c->maximum;
    c->length = c->flags = 0;
    c->depth = c->minimum = c->maximum = 0;
    c->previous_body = NULL;
}

static void tf_value_block_record(TFValueBlockCensus *c, const void *body,
    unsigned offset, unsigned size, unsigned kind, unsigned pops, unsigned pushes)
{
    c->total++;
    if (!kind) {
        tf_value_block_finish(c);
        c->excluded++;
        return;
    }
    if (body != c->previous_body || offset != c->previous_offset + c->previous_size)
        tf_value_block_finish(c);
    c->eligible++;
    c->length++;
    c->flags |= kind;
    c->depth -= (int)pops;
    if (c->depth < c->minimum) c->minimum = c->depth;
    c->depth += (int)pushes;
    if (c->depth > c->maximum) c->maximum = c->depth;
    c->previous_body = body;
    c->previous_offset = offset;
    c->previous_size = size;
    if (c->length == TF_VALUE_BLOCK_LIMIT) tf_value_block_finish(c);
}
