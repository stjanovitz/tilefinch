/* The node-handle table: slots, generations and the pointer index, wrapper
   leases and their sweep, slot retirement and exhaustion-time reclamation.
   Included by js_dom_bindings.c, in its translation unit, so the hot
   static helpers (bridge_node_index_find, bridge_node_handle, ...) stay
   inlinable into the DOM natives that call them. */

static int64_t bridge_node_handle(const DomBridge *bridge, size_t slot)
{
    if (bridge == NULL || slot >= bridge->node_count
        || bridge->nodes[slot] == NULL
        || bridge->node_generations[slot] == 0
        || bridge->node_generations[slot]
               >= DOM_BRIDGE_NODE_GENERATION_MAX) return 0;
    return (int64_t) ((uint32_t) (bridge->node_generations[slot]
                                 << DOM_BRIDGE_NODE_INDEX_BITS)
                     | (uint32_t) (slot + 1));
}

static size_t bridge_node_index_home(const DomBridge *bridge,
                                     const lxb_dom_node_t *node)
{
    uintptr_t key = (uintptr_t) node;
    uint32_t mixed = (uint32_t) (key >> 3);
#if UINTPTR_MAX > UINT32_MAX
    mixed ^= (uint32_t) (key >> 35);
#endif
    mixed *= UINT32_C(2654435761);
    return (size_t) mixed & (bridge->node_index_capacity - 1u);
}

/* The slot holding `node`, or SIZE_MAX. Compares pointers only: a slot may
   hold a pointer whose node has since been destroyed. */
static size_t bridge_node_index_find(const DomBridge *bridge,
                                     const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL || bridge->node_index == NULL)
        return SIZE_MAX;
    size_t at = bridge_node_index_home(bridge, node);
    for (size_t probes = 0; probes < bridge->node_index_capacity;
         probes++) {
        uint16_t entry = bridge->node_index[at];
        if (entry == 0u) return SIZE_MAX;
        if (bridge->nodes[entry - 1u] == node) return entry - 1u;
        at = (at + 1u) & (bridge->node_index_capacity - 1u);
    }
    return SIZE_MAX;
}

static void bridge_node_index_insert(DomBridge *bridge, size_t slot)
{
    size_t at = bridge_node_index_home(bridge, bridge->nodes[slot]);
    while (bridge->node_index[at] != 0u)
        at = (at + 1u) & (bridge->node_index_capacity - 1u);
    bridge->node_index[at] = (uint16_t) (slot + 1u);
}

/* Remove `slot` while bridge->nodes[slot] still holds its pointer. Linear
   probing with backward-shift deletion keeps every run unbroken. */
static void bridge_node_index_remove(DomBridge *bridge, size_t slot)
{
    const size_t mask = bridge->node_index_capacity - 1u;
    size_t hole = bridge_node_index_home(bridge, bridge->nodes[slot]);
    for (size_t probes = 0;; probes++) {
        if (probes == bridge->node_index_capacity
            || bridge->node_index[hole] == 0u) return;
        if (bridge->node_index[hole] == (uint16_t) (slot + 1u)) break;
        hole = (hole + 1u) & mask;
    }
    for (size_t at = (hole + 1u) & mask; bridge->node_index[at] != 0u;
         at = (at + 1u) & mask) {
        size_t home = bridge_node_index_home(
            bridge, bridge->nodes[bridge->node_index[at] - 1u]);
        /* The entry may fill the hole unless its home lies cyclically in
           (hole, at]. */
        bool stays = hole <= at ? home > hole && home <= at
                                : home > hole || home <= at;
        if (stays) continue;
        bridge->node_index[hole] = bridge->node_index[at];
        hole = at;
    }
    bridge->node_index[hole] = 0u;
}

static size_t bridge_node_index_capacity_for(size_t capacity)
{
    size_t index = 64u;
    while (index < 2u * capacity) index *= 2u;
    return index;
}

/* Moves the slot table to `capacity` slots (a multiple of 32, at least the
   current one). Every array, including the lazily allocated wrapper
   references and owner tags, is resized before anything is published: a
   refusal leaves the table exactly as it was (an array already enlarged
   only carries unused, initialized tail entries). Slot numbers, and so
   handles, are unchanged; only the pointer index is rebuilt. */
static bool bridge_node_table_resize(DomBridge *bridge, size_t capacity)
{
    if (bridge == NULL || bridge->budget == NULL || capacity == 0
        || capacity % 32u != 0 || capacity > DOM_BRIDGE_NODE_LIMIT_MAX
        || capacity < bridge->node_capacity) return false;
    if (capacity == bridge->node_capacity) return true;
    size_t index_capacity = bridge_node_index_capacity_for(capacity);
    size_t pointer_bytes = capacity
        * (sizeof(lxb_dom_node_t *) + sizeof(uintptr_t));
    size_t word_bytes = capacity * 2u * sizeof(uint32_t)
        + capacity / 32u * sizeof(uint32_t);
    unsigned char *block = budget_calloc(
        bridge->budget, 1, pointer_bytes + word_bytes + capacity);
    if (block == NULL) return false;
    uint16_t *index = budget_calloc(
        bridge->budget, index_capacity, sizeof(uint16_t));
    if (index == NULL) {
        budget_free(bridge->budget, block);
        return false;
    }
    if (bridge->wrapper_refs != NULL) {
        JSValue *refs = budget_realloc(
            bridge->budget, bridge->wrapper_refs,
            capacity * sizeof(JSValue));
        if (refs == NULL) {
            budget_free(bridge->budget, index);
            budget_free(bridge->budget, block);
            return false;
        }
        for (size_t at = bridge->node_capacity; at < capacity; at++)
            refs[at] = JS_UNDEFINED;
        bridge->wrapper_refs = refs;
    }
    if (bridge->node_owner_tags != NULL) {
        unsigned char *tags = budget_realloc(
            bridge->budget, bridge->node_owner_tags, capacity);
        if (tags == NULL) {
            budget_free(bridge->budget, index);
            budget_free(bridge->budget, block);
            return false;
        }
        memset(tags + bridge->node_capacity, 0,
               capacity - bridge->node_capacity);
        bridge->node_owner_tags = tags;
    }
    lxb_dom_node_t **nodes = (lxb_dom_node_t **) (void *) block;
    uintptr_t *owners = (uintptr_t *) (void *) (nodes + capacity);
    uint32_t *generations = (uint32_t *) (void *) (owners + capacity);
    uint32_t *leases = generations + capacity;
    uint32_t *reusable = leases + capacity;
    unsigned char *flags = (unsigned char *) (reusable + capacity / 32u);
    size_t old = bridge->node_capacity;
    if (old != 0) {
        memcpy(nodes, bridge->nodes, old * sizeof(*nodes));
        memcpy(owners, bridge->node_owner_document_identities,
               old * sizeof(*owners));
        memcpy(generations, bridge->node_generations,
               old * sizeof(*generations));
        memcpy(leases, bridge->node_wrapper_leases, old * sizeof(*leases));
        memcpy(reusable, bridge->node_reusable_bits,
               old / 32u * sizeof(*reusable));
        memcpy(flags, bridge->node_retention_flags, old);
    }
    /* The arrays share one block whose base is `nodes`. */
    budget_free(bridge->budget, bridge->nodes);
    budget_free(bridge->budget, bridge->node_index);
    bridge->nodes = nodes;
    bridge->node_owner_document_identities = owners;
    bridge->node_generations = generations;
    bridge->node_wrapper_leases = leases;
    bridge->node_reusable_bits = reusable;
    bridge->node_retention_flags = flags;
    bridge->node_index = index;
    bridge->node_index_capacity = index_capacity;
    bridge->node_capacity = capacity;
    for (size_t slot = 0; slot < bridge->node_count; slot++)
        if (bridge->nodes[slot] != NULL)
            bridge_node_index_insert(bridge, slot);
    return true;
}

bool js_rt_bridge_node_table_init(DomBridge *bridge, size_t limit)
{
    if (bridge == NULL || bridge->nodes != NULL) return false;
    if (limit == 0) limit = DOM_BRIDGE_NODE_LIMIT;
    if (limit > DOM_BRIDGE_NODE_LIMIT_MAX) limit = DOM_BRIDGE_NODE_LIMIT_MAX;
    limit -= limit % 32u;
    if (limit == 0) return false;
    bridge->node_capacity_limit = limit;
    return bridge_node_table_resize(
        bridge, limit < DOM_BRIDGE_NODE_LIMIT ? limit : DOM_BRIDGE_NODE_LIMIT);
}

void js_rt_bridge_node_table_free(DomBridge *bridge)
{
    if (bridge == NULL) return;
    budget_free(bridge->budget, bridge->nodes);
    budget_free(bridge->budget, bridge->node_index);
    bridge->nodes = NULL;
    bridge->node_owner_document_identities = NULL;
    bridge->node_generations = NULL;
    bridge->node_wrapper_leases = NULL;
    bridge->node_reusable_bits = NULL;
    bridge->node_retention_flags = NULL;
    bridge->node_index = NULL;
    bridge->node_index_capacity = 0;
    bridge->node_capacity = 0;
    bridge->node_count = 0;
}

/* One growth step when every slot is live: double, up to the policy's
   ceiling. Growth is admitted like any page allocation; a refusal leaves the
   table full and registration reports exhaustion as before. */
static bool bridge_node_table_grow(DomBridge *bridge)
{
    if (bridge == NULL || bridge->node_capacity >= bridge->node_capacity_limit)
        return false;
    size_t capacity = bridge->node_capacity * 2u;
    if (capacity > bridge->node_capacity_limit)
        capacity = bridge->node_capacity_limit;
    if (!bridge_node_table_resize(bridge, capacity)) {
        if (bridge->result != NULL
            && bridge->result->dom_handle_growth_refusals != SIZE_MAX)
            bridge->result->dom_handle_growth_refusals++;
        return false;
    }
    if (bridge->result != NULL) {
        bridge->result->dom_handle_slot_capacity = bridge->node_capacity;
        if (bridge->result->dom_handle_growths != SIZE_MAX)
            bridge->result->dom_handle_growths++;
    }
    return true;
}

static void bridge_node_set_reusable(DomBridge *bridge, size_t slot,
                                     bool reusable)
{
    uint32_t bit = UINT32_C(1) << (slot % 32u);
    if (reusable) bridge->node_reusable_bits[slot / 32u] |= bit;
    else bridge->node_reusable_bits[slot / 32u] &= ~bit;
}

/* The lowest NULL slot whose generation can still advance, or SIZE_MAX. */
static size_t bridge_first_reusable_slot(const DomBridge *bridge)
{
    size_t words = (bridge->node_count + 31u) / 32u;
    for (size_t word = 0; word < words; word++) {
        uint32_t bits = bridge->node_reusable_bits[word];
        if (bits != 0u) return word * 32u + (size_t) __builtin_ctz(bits);
    }
    return SIZE_MAX;
}

static void bridge_notify_node_state_retired(DomBridge *bridge,
                                             int64_t handle, bool wrapper_only)
{
    if (bridge == NULL || bridge->host == NULL
        || bridge->host->context == NULL || handle <= 0) return;
    JSContext *context = bridge->host->context;
    if (JS_IsFunction(
            context, js_rt_bridge_trusted_hook(
                bridge, SCRIPT_HOST_RETIRE_NATIVE_NODE_STATE))) {
        JSValue arguments[2] = {JS_NewInt64(context, handle),
                               JS_NewBool(context, wrapper_only)};
        JSValue result = JS_Call(
            context,
            js_rt_bridge_trusted_hook(
                bridge, SCRIPT_HOST_RETIRE_NATIVE_NODE_STATE),
            JS_UNDEFINED, 2, arguments);
        JS_FreeValue(context, arguments[0]);
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(context);
            JS_FreeValue(context, exception);
        }
        JS_FreeValue(context, result);
    }
}

bool js_rt_bridge_node_slot_for_handle(const DomBridge *bridge,
                                 int64_t handle, size_t *slot)
{
    if (bridge == NULL || handle <= 0) return false;
    uint64_t encoded = (uint64_t) handle;
    uint64_t generation = encoded >> DOM_BRIDGE_NODE_INDEX_BITS;
    size_t index = (size_t) (encoded & DOM_BRIDGE_NODE_INDEX_MASK);
    if (index == 0 || index > bridge->node_count
        || generation != bridge->node_generations[index - 1]
        || bridge->nodes[index - 1] == NULL) return false;
    if (slot != NULL) *slot = index - 1;
    return true;
}

/* Every registered root carries the native-only carrier mark (set in
   js_dom_register_shadow_root), so a query walk tests the node itself
   instead of scanning the registry once per visited element. */
static bool bridge_node_is_shadow_root(const DomBridge *bridge,
                                       const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL || bridge->shadow_root_count == 0)
        return false;
    return document_node_is_shadow_carrier(node);
}

/* The carrier itself is not observable as part of the light tree, nor are
   its descendants.  A ShadowRoot-scoped query begins at the carrier's first
   child, so it remains able to inspect its own tree while nested shadow roots
   are still isolated. */
static bool bridge_query_node_hidden_by_shadow(
    const DomBridge *bridge, const lxb_dom_node_t *node,
    const lxb_dom_node_t *boundary,
    const DomDocumentOrderTraversal *traversal, size_t *shadow_depth)
{
    if (shadow_depth == NULL || traversal == NULL) return false;
    if (*shadow_depth != SIZE_MAX) {
        if (traversal->current_depth > *shadow_depth) return true;
        *shadow_depth = SIZE_MAX;
    }
    if (node != boundary && bridge_node_is_shadow_root(bridge, node)) {
        *shadow_depth = traversal->current_depth;
        return true;
    }
    return false;
}

static void bridge_reclaim_node_slots(DomBridge *bridge,
                                      const lxb_dom_node_t *keep,
                                      bool shadow_pressure);

JSValue js_dom_register_shadow_root(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    size_t slot = 0;
    if (bridge == NULL || argc < 1
        || JS_ToInt64(context, &handle, argv[0]) < 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        || bridge->nodes[slot]->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return JS_FALSE;
    }
    bool reclaim = argc > 1 && JS_ToBool(context, argv[1]) > 0;
    size_t write = 0;
retry:
    write = 0;
    for (size_t read = 0; read < bridge->shadow_root_count; read++) {
        size_t existing_slot = 0;
        uint32_t existing = bridge->shadow_root_handles[read];
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, (int64_t) existing, &existing_slot)) continue;
        if (existing == (uint32_t) handle) return JS_TRUE;
        bridge->shadow_root_handles[write++] = existing;
    }
    bridge->shadow_root_count = write;
    size_t admitted = DOM_BRIDGE_SHADOW_ROOT_BASE;
    if (bridge->budget != NULL) {
        size_t by_budget = bridge->budget->limit
            / (argc > 2 && JS_ToBool(context, argv[2]) > 0
               ? DOM_BRIDGE_SHARED_SHADOW_ROOT_BUDGET_BYTES
               : DOM_BRIDGE_SHADOW_ROOT_BUDGET_BYTES);
        if (by_budget > admitted) admitted = by_budget;
    }
    if (admitted > DOM_BRIDGE_SHADOW_ROOT_LIMIT)
        admitted = DOM_BRIDGE_SHADOW_ROOT_LIMIT;
    if (write >= admitted) {
        if (reclaim) {
            reclaim = false;
            /* The bootstrap has released only disconnected cache ownership.
               Reuse the handle-table sweep and its unproductive-GC backoff;
               author-held roots and connected wrappers remain live. */
            bridge_reclaim_node_slots(bridge, bridge->nodes[slot], true);
            goto retry;
        }
        return JS_FALSE;
    }
    /* Style resolution recognises the carrier by this native-only mark
       (shadow composition, document.h). */
    if (!document_mark_shadow_carrier(bridge->nodes[slot])) return JS_FALSE;
    bridge->shadow_root_handles[write] = (uint32_t) handle;
    bridge->shadow_root_count = write + 1u;
    bridge->shadow_reclaim_gc_skip = 0;
    bridge->shadow_reclaim_gc_backoff = 0;
    return JS_TRUE;
}

static void bridge_wrapper_ref_clear(DomBridge *bridge, size_t slot);
static void bridge_set_event_state(DomBridge *bridge, size_t slot,
                                   unsigned char event, bool stuck);

/* Restarts the owner-turn wrapper sweep over the whole table. */
static void bridge_arm_wrapper_sweep(DomBridge *bridge)
{
    if (bridge->wrapper_refs != NULL)
        bridge->node_wrapper_sweep_remaining = bridge->node_count;
}

/* Event state, defined in js_event_state.inc (included further down). */
static void bridge_hold_detached_event_state(DomBridge *bridge, size_t slot);
static void bridge_reopen_stuck_tree(DomBridge *bridge, lxb_dom_node_t *root);
static bool bridge_event_retry_open(const DomBridge *bridge);
static TILEFINCH_COLD_PATH void bridge_retry_event_releases(DomBridge *bridge);

static int64_t bridge_invalidate_node_slot_impl(
    DomBridge *bridge, size_t slot, bool notify)
{
    if (bridge == NULL || slot >= bridge->node_count
        || bridge->nodes[slot] == NULL) return 0;
    int64_t retired_handle = bridge_node_handle(bridge, slot);
    if (retired_handle != 0
        && bridge->fullscreen_node_handle == retired_handle) {
        if (bridge->stylesheet != NULL) {
            ((Stylesheet *) bridge->stylesheet)->fullscreen_node = NULL;
        }
        bridge->fullscreen_node_handle = 0;
        if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;
    }
    bridge_node_index_remove(bridge, slot);
    bridge->nodes[slot] = NULL;
    bridge_wrapper_ref_clear(bridge, slot);
    bridge->node_owner_document_identities[slot] = 0;
    if (bridge->node_owner_tags != NULL) bridge->node_owner_tags[slot] = 0;
    /* Never wrap an incarnation: a wrapped handle could make an arbitrarily
       old JavaScript wrapper refer to an unrelated replacement node.  A slot
       that exhausts its hundreds of thousands of incarnations is retired. */
    if (bridge->node_generations[slot]
        < DOM_BRIDGE_NODE_GENERATION_MAX) {
        bridge->node_generations[slot]++;
    }
    bridge_node_set_reusable(
        bridge, slot,
        bridge->node_generations[slot] < DOM_BRIDGE_NODE_GENERATION_MAX);
    bridge_set_event_state(bridge, slot, 0, false);
    bridge->events.holder_epoch++;
    bridge->node_retention_flags[slot] =
        notify ? 0 : BRIDGE_NODE_PENDING_RETIRE_NOTIFY;
    if (bridge->result != NULL
        && bridge->result->dom_handle_slots_live != 0) {
        bridge->result->dom_handle_slots_live--;
    }
    if (notify) bridge_notify_node_state_retired(bridge, retired_handle, false);
    return retired_handle;
}

void bridge_invalidate_node_slot(DomBridge *bridge, size_t slot)
{
    (void) bridge_invalidate_node_slot_impl(bridge, slot, true);
}

/* Deliver the deferred retirement callbacks for the slots one discard
   retired. Only that discard sets the pending bit, and it always drains its
   own slots here, so the retire bitmap names every pending slot. */
static void bridge_notify_pending_node_retirements(
    DomBridge *bridge, const unsigned char *retire_slots,
    size_t first_slot, size_t last_slot)
{
    if (bridge == NULL || retire_slots == NULL || first_slot > last_slot)
        return;
    size_t bytes = (bridge->node_count + 7u) / 8u;
    if (bytes > last_slot / 8u + 1u) bytes = last_slot / 8u + 1u;
    for (size_t byte = first_slot / 8u; byte < bytes; byte++) {
        if (retire_slots[byte] == 0) continue;
        for (size_t bit = 0; bit < 8u; bit++) {
            if ((retire_slots[byte] & (1u << bit)) == 0) continue;
            size_t slot = byte * 8u + bit;
            /* A callback below may reenter and grow the table. */
            if (slot >= bridge->node_count
                || (bridge->node_retention_flags[slot]
                    & BRIDGE_NODE_PENDING_RETIRE_NOTIFY) == 0) continue;
            bridge->node_retention_flags[slot] &=
                (unsigned char) ~BRIDGE_NODE_PENDING_RETIRE_NOTIFY;
            uint32_t generation = bridge->node_generations[slot];
            if (generation <= 1) continue;
            int64_t handle = (int64_t) (
                ((generation - 1u) << DOM_BRIDGE_NODE_INDEX_BITS)
                | (uint32_t) (slot + 1u));
            bridge_notify_node_state_retired(bridge, handle, false);
        }
    }
}

/* The adopted owner tag (see DomBridge.node_owner_tags) of a node no
   adoption has tagged: the document's for a connected node, the template
   contents owner's inside template contents, else that of the nearest
   tagged ancestor of its detached tree. Nodes parsed into an adopted tree
   thereby take its owner, as the DOM's insert steps adopt them. */
static unsigned char bridge_node_owner_tag_inherited(
    const DomBridge *bridge, const lxb_dom_node_t *node)
{
    const lxb_dom_node_t *root = node;
    for (size_t steps = 0; root->parent != NULL; steps++) {
        if (steps >= DOM_TRAVERSAL_VISIT_LIMIT) return 1;
        root = root->parent;
    }
    if (root->type == LXB_DOM_NODE_TYPE_DOCUMENT) return 1;
    if (root->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT
        && lxb_dom_interface_document_fragment(root)->host != NULL)
        return 2;
    for (const lxb_dom_node_t *at = node->parent;
         at != NULL && bridge->node_owner_tags != NULL; at = at->parent) {
        size_t slot = bridge_node_index_find(bridge, at);
        if (slot != SIZE_MAX && bridge->node_owner_tags[slot] != 0)
            return bridge->node_owner_tags[slot];
    }
    return 1;
}

int64_t js_rt_bridge_find_node_handle(const DomBridge *bridge,
                                      const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL || bridge->nodes == NULL) return 0;
    size_t slot = bridge_node_index_find(bridge, node);
    /* A slot captured for another document's node at this address is not
       this node's handle; registration replaces it. */
    if (slot == SIZE_MAX || bridge->node_owner_document_identities[slot]
            != js_rt_node_owner_identity(node)) return 0;
    return bridge_node_handle(bridge, slot);
}

int64_t js_rt_bridge_register_node(DomBridge *bridge, lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return 0;
    if (bridge->nodes == NULL) return 0;
    uintptr_t owner = js_rt_node_owner_identity(node);
    const size_t none = SIZE_MAX;
    size_t reusable = none;
    size_t existing = bridge_node_index_find(bridge, node);
    if (existing != SIZE_MAX) {
        if (bridge->node_owner_document_identities[existing] == owner)
            return bridge_node_handle(bridge, existing);
        /* An allocator may reuse a just-destroyed address for a node in a
           different document.  The captured owner prevents an old handle
           from silently acquiring that new identity. */
        bridge_invalidate_node_slot(bridge, existing);
        /* The retirement callback is JavaScript; it may have registered
           this node or claimed the freed slot meanwhile. */
        size_t again = bridge_node_index_find(bridge, node);
        if (again != SIZE_MAX
            && bridge->node_owner_document_identities[again] == owner)
            return bridge_node_handle(bridge, again);
        if ((bridge->node_reusable_bits[existing / 32u]
             & (UINT32_C(1) << (existing % 32u))) != 0u)
            reusable = existing;
    }
    if (reusable == none) {
        size_t first = bridge_first_reusable_slot(bridge);
        if (first != SIZE_MAX) reusable = first;
    }
    if (reusable == none
        && bridge->node_count == bridge->node_capacity) {
        /* Dead wrappers are reclaimed first, exactly as at a fixed-size
           table; only a table that is genuinely live grows. */
        bridge_reclaim_node_slots(bridge, node, false);
        /* Reclamation runs retirement callbacks, which may have registered
           this node meanwhile. */
        size_t again = bridge_node_index_find(bridge, node);
        if (again != SIZE_MAX
            && bridge->node_owner_document_identities[again] == owner)
            return bridge_node_handle(bridge, again);
        size_t first = bridge_first_reusable_slot(bridge);
        if (first != SIZE_MAX) reusable = first;
        else if (bridge->node_count == bridge->node_capacity)
            (void) bridge_node_table_grow(bridge);
    }
    if (reusable == none) {
        if (bridge->node_count == bridge->node_capacity) {
            if (bridge->result != NULL
                && bridge->result->dom_handle_exhaustions != SIZE_MAX) {
                bridge->result->dom_handle_exhaustions++;
            }
            return 0;
        }
        reusable = bridge->node_count++;
        bridge->node_generations[reusable] = 1;
        if (bridge->result != NULL
            && bridge->result->dom_handle_slots_high_water
                   < bridge->node_count) {
            bridge->result->dom_handle_slots_high_water = bridge->node_count;
        }
    } else if (bridge->result != NULL
               && bridge->result->dom_handle_slot_reuses != SIZE_MAX) {
        bridge->result->dom_handle_slot_reuses++;
    }
    bridge->nodes[reusable] = node;
    bridge->node_owner_document_identities[reusable] = owner;
    bridge_node_set_reusable(bridge, reusable, false);
    bridge_node_index_insert(bridge, reusable);
    /* Tag at registration: once script holds the node, removal from its
       tree must not change its owner. */
    if (bridge->node_owner_tags != NULL)
        bridge->node_owner_tags[reusable] =
            bridge_node_owner_tag_inherited(bridge, node);
    if (bridge->result != NULL) {
        if (bridge->result->dom_handle_slots_live != SIZE_MAX) {
            bridge->result->dom_handle_slots_live++;
        }
        if (bridge->result->dom_handle_slots_peak
            < bridge->result->dom_handle_slots_live) {
            bridge->result->dom_handle_slots_peak =
                bridge->result->dom_handle_slots_live;
        }
    }
    return bridge_node_handle(bridge, reusable);
}

/* A handle for `node` as a DOM binding returns it: 0 for no node, and a
   RangeError, rather than a null that bootstrap code would dereference,
   when the handle table cannot admit the node even after reclamation. */
static JSValue bridge_throw_handles_exhausted(JSContext *context)
{
    return JS_ThrowRangeError(context, "DOM node handle table exhausted");
}

static JSValue bridge_node_handle_value(JSContext *context,
                                        DomBridge *bridge,
                                        lxb_dom_node_t *node)
{
    int64_t handle = js_rt_bridge_register_node(bridge, node);
    if (handle == 0 && bridge != NULL && node != NULL)
        return bridge_throw_handles_exhausted(context);
    return JS_NewInt64(context, handle);
}

lxb_dom_node_t *js_rt_bridge_node_arg(JSContext *context,
                                DomBridge *bridge, JSValueConst value)
{
    int64_t handle = 0;
    if (JS_ToInt64(context, &handle, value) < 0 || handle <= 0) return NULL;
    size_t slot = 0;
    return js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        ? bridge->nodes[slot] : NULL;
}

static void bridge_wrapper_ref_clear(DomBridge *bridge, size_t slot)
{
    if (bridge == NULL || bridge->wrapper_refs == NULL
        || bridge->host == NULL || bridge->host->runtime == NULL) return;
    /* A kept wrapper's reference ends with its slot's WeakRef (only a
       rebind replaces that while the wrapper lives). */
    if ((bridge->node_retention_flags[slot] & BRIDGE_NODE_EVENT_KEPT) != 0)
        bridge_set_event_state(bridge, slot, 0, false);
    JSValue *ref = &bridge->wrapper_refs[slot];
    if (JS_IsObject(*ref)) JS_FreeValueRT(bridge->host->runtime, *ref);
    *ref = JS_UNDEFINED;
}

/* Before the realm is freed: drop every stored reference and the table. */
void js_rt_bridge_wrapper_refs_free(DomBridge *bridge)
{
    if (bridge == NULL || bridge->wrapper_refs == NULL) return;
    for (size_t slot = 0; slot < bridge->node_capacity; slot++)
        bridge_wrapper_ref_clear(bridge, slot);
    budget_free(bridge->budget, bridge->wrapper_refs);
    bridge->wrapper_refs = NULL;
}

JSValue js_dom_retain_node_wrapper(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    size_t slot = 0;
    if (argc < 1 || JS_ToInt64(context, &handle, argv[0]) < 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)) {
        return JS_NewInt32(context, 0);
    }
    uint32_t lease = bridge->node_wrapper_leases[slot];
    if (lease == UINT32_MAX) {
        /* At this practically unreachable boundary, retain the native handle
           permanently rather than wrap a lease and let an ancient finalizer
           invalidate a current wrapper. */
        bridge_pin_node_slot(bridge, slot);
        return JS_NewInt32(context, 0);
    }
    /* The wrapper cache's WeakRef for this lease, for native getters. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (bridge->wrapper_refs == NULL) {
            bridge->wrapper_refs = budget_calloc(
                bridge->budget, bridge->node_capacity, sizeof(JSValue));
            if (bridge->wrapper_refs != NULL)
                for (size_t at = 0; at < bridge->node_capacity; at++)
                    bridge->wrapper_refs[at] = JS_UNDEFINED;
        }
        if (bridge->wrapper_refs == NULL) {
            /* Rebinding can be materializing an author-held secondary node.
               Keep its native lifetime safe even when tracking refuses, but
               do not publish an untrackable lease as a successful cache. */
            bridge_pin_node_slot(bridge, slot);
            return JS_ThrowOutOfMemory(context);
        }
        bridge_wrapper_ref_clear(bridge, slot);
        bridge->wrapper_refs[slot] = JS_DupValue(context, argv[1]);
    } else {
        bridge_wrapper_ref_clear(bridge, slot);
    }
    lease++;
    if (lease == 0) lease = 1;
    bridge->node_wrapper_leases[slot] = lease;
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_LIVE_WRAPPER;
    bridge->node_wrapper_sweep_remaining = bridge->node_count;
    return JS_NewInt64(context, lease);
}

bool bridge_node_is_connected(const lxb_dom_node_t *node)
{
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
    }
    return false;
}

static size_t bridge_discard_unretained_detached_subtree(
    DomBridge *bridge, lxb_dom_node_t *root);
static lxb_dom_node_t *bridge_owned_lifetime_root(lxb_dom_node_t *node);
static bool bridge_subtree_may_contain(lxb_dom_node_t *root,
                                       const lxb_dom_node_t *node);

/* Wrapper-only retirements a sweep hands to the bootstrap in one call,
   instead of one JavaScript call per released wrapper. */
typedef struct {
    int64_t handles[64];
    size_t count;
} BridgeRetiredWrappers;

static void bridge_retired_wrappers_flush(DomBridge *bridge,
                                          BridgeRetiredWrappers *batch)
{
    if (batch == NULL || batch->count == 0) return;
    size_t count = batch->count;
    batch->count = 0;
    JSContext *context = bridge->host == NULL ? NULL : bridge->host->context;
    JSValue list = JS_UNDEFINED;
    if (count > 1 && context != NULL
        && JS_IsFunction(context,
                         js_rt_bridge_trusted_hook(
                             bridge, SCRIPT_HOST_RETIRE_NATIVE_NODE_STATE))) {
        /* Handles are below 2^53, so a Float64Array carries them exactly. */
        double values[sizeof(batch->handles) / sizeof(batch->handles[0])];
        for (size_t i = 0; i < count; i++) values[i] = (double) batch->handles[i];
        JSValue buffer = JS_NewArrayBufferCopy(
            context, (const uint8_t *) values, count * sizeof(values[0]));
        /* The constructor reads (buffer, byteOffset, length) unpadded. */
        JSValueConst view[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};
        if (!JS_IsException(buffer))
            list = JS_NewTypedArray(context, 3, view, JS_TYPED_ARRAY_FLOAT64);
        JS_FreeValue(context, buffer);
    }
    bool listed = JS_IsObject(list);
    if (!listed) {
        /* A refused list falls back to the per-handle notice. */
        JS_FreeValue(context, list);
        if (context != NULL && JS_HasException(context))
            JS_FreeValue(context, JS_GetException(context));
        for (size_t i = 0; i < count; i++)
            bridge_notify_node_state_retired(bridge, batch->handles[i], true);
        return;
    }
    JSValue arguments[2] = {list, JS_TRUE};
    JSValue result = JS_Call(
        context,
        js_rt_bridge_trusted_hook(bridge,
                                  SCRIPT_HOST_RETIRE_NATIVE_NODE_STATE),
        JS_UNDEFINED, 2, arguments);
    JS_FreeValue(context, list);
    if (JS_IsException(result)) JS_FreeValue(context, JS_GetException(context));
    JS_FreeValue(context, result);
}

/* Ends the live wrapper identity of `slot` and reclaims its detached
   lifetime tree unless something else retains it. A tree that may contain
   `keep` (a node native code is still using) is left for the wrapper's own
   finalizer. With `batch`, the wrapper-only notice joins it instead of
   calling into JavaScript now. Returns the handle slots released. */
static size_t bridge_release_wrapper_slot(DomBridge *bridge, size_t slot,
                                          const lxb_dom_node_t *keep,
                                          BridgeRetiredWrappers *batch)
{
    lxb_dom_node_t *node = bridge->nodes[slot];
    int64_t handle = bridge_node_handle(bridge, slot);
    if (batch == NULL) {
        bridge_notify_node_state_retired(bridge, handle, true);
    } else if (handle > 0) {
        if (batch->count == sizeof(batch->handles) / sizeof(batch->handles[0]))
            bridge_retired_wrappers_flush(bridge, batch);
        batch->handles[batch->count++] = handle;
    }
    bridge_wrapper_ref_clear(bridge, slot);
    bridge->node_retention_flags[slot] &=
        (unsigned char) ~BRIDGE_NODE_LIVE_WRAPPER;
    if ((bridge->node_retention_flags[slot] & BRIDGE_NODE_NATIVE_PIN) != 0
        || bridge_node_is_connected(node)) {
        if (bridge->result != NULL
            && bridge->result->dom_handle_connected_preserves != SIZE_MAX) {
            bridge->result->dom_handle_connected_preserves++;
        }
        return 0;
    }
    /* A discard below sends full retirements; this wrapper's notice (and
       the batch before it) goes first, as without batching. That is
       JavaScript, so read the slot's node again afterwards. */
    if (batch != NULL && batch->count != 0) {
        bridge_retired_wrappers_flush(bridge, batch);
        node = bridge->nodes[slot];
        if (node == NULL) return 0;
    }
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
    /* Template contents have no DOM parent but do have an explicit native
       host. Follow that ownership before deciding: a connected or retained
       template must survive, while an independent discarded fragment must
       not wait forever for another descendant wrapper to die. */
    if (root == NULL || bridge_node_is_connected(root)) return 0;
    /* A detached tree lost a wrapper: perhaps the frozen one that kept it
       from being held. Let the sweep try that tree again. */
    if (bridge->events.stuck_slots != 0) bridge_reopen_stuck_tree(bridge, root);
    if (keep != NULL && bridge_subtree_may_contain(root, keep)) return 0;
    size_t released = bridge_discard_unretained_detached_subtree(
        bridge, root);
    if (released != 0 && bridge->result != NULL
        && bridge->result->dom_handle_wrapper_releases != SIZE_MAX) {
        js_rt_saturating_add_size(
            &bridge->result->dom_handle_wrapper_releases, released);
    }
    return released;
}

#ifdef JS_RT_TEST_INSTRUMENTS
JSValue js_dom_release_node_wrapper(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0, lease_value = 0;
    size_t slot = 0;
    if (argc < 2 || JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt64(context, &lease_value, argv[1]) < 0
        || lease_value <= 0 || (uint64_t) lease_value > UINT32_MAX
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        || bridge->node_wrapper_leases[slot]
               != (uint32_t) lease_value) {
        if (bridge != NULL && bridge->result != NULL
            && bridge->result->dom_handle_stale_releases != SIZE_MAX) {
            bridge->result->dom_handle_stale_releases++;
        }
        return JS_FALSE;
    }
    return bridge_release_wrapper_slot(bridge, slot, NULL, NULL) != 0
        ? JS_TRUE : JS_FALSE;
}
#endif

/* Owner turns release cleared wrapper WeakRefs in bounded slices. A long
   synchronous task can still fill the table before its next owner turn.
   Registration then sweeps the whole table, collecting first when that alone
   frees too little (a wrapper cycle clears only at a collection). The sweep
   is linear in the table; a collection that frees little backs off within
   one entry into JavaScript, so a genuinely full table cannot turn every
   refused registration into a collection. */
#define BRIDGE_RECLAIM_ENOUGH(bridge) ((bridge)->node_capacity / 64u)
#define BRIDGE_RECLAIM_BACKOFF_MAX 1024u

static size_t bridge_release_dead_wrappers(DomBridge *bridge,
                                           const lxb_dom_node_t *keep)
{
    JSContext *context = bridge->host->context;
    size_t released = 0;
    BridgeRetiredWrappers batch = {.count = 0};
    /* Release callbacks are JavaScript and may grow the table. */
    for (size_t slot = 0; slot < bridge->node_count; slot++) {
        if (bridge->wrapper_refs == NULL) break;
        if ((bridge->node_retention_flags[slot]
             & BRIDGE_NODE_LIVE_WRAPPER) == 0
            || bridge->nodes[slot] == NULL
            || !JS_IsObject(bridge->wrapper_refs[slot])) continue;
        JSValue wrapper = JS_WeakRefDeref(context, bridge->wrapper_refs[slot]);
        bool live = JS_IsObject(wrapper);
        JS_FreeValue(context, wrapper);
        if (!live)
            released += bridge_release_wrapper_slot(bridge, slot, keep, &batch);
    }
    bridge_retired_wrappers_flush(bridge, &batch);
    return released;
}

void js_rt_bridge_sweep_node_wrappers(DomBridge *bridge)
{
    if (bridge == NULL || bridge->host == NULL || bridge->host->context == NULL
        || bridge->node_reclaim_active) return;
    if (bridge->wrapper_refs == NULL || bridge->node_count == 0) {
        bridge->node_wrapper_sweep_remaining = 0;
        return;
    }
    bridge->node_reclaim_active = true;
    bool holds_open = bridge_event_retry_open(bridge);
    if (bridge->events.release_pending && holds_open) {
        bridge_retry_event_releases(bridge);
        holds_open = bridge_event_retry_open(bridge);
    }
    BridgeRetiredWrappers batch = {.count = 0};
    /* Detached nodes whose event state is still rooted. */
    size_t holds[64], hold_count = 0;
    for (size_t work = 0; work < 64 && bridge->node_wrapper_sweep_remaining != 0;
         work++) {
        if (bridge->node_wrapper_sweep_cursor >= bridge->node_count)
            bridge->node_wrapper_sweep_cursor = 0;
        size_t slot = bridge->node_wrapper_sweep_cursor++;
        bridge->node_wrapper_sweep_remaining--;
        if ((bridge->node_retention_flags[slot]
             & (BRIDGE_NODE_EVENT_ROOTED | BRIDGE_NODE_EVENT_KEPT)) != 0
            && (bridge->node_retention_flags[slot]
                & BRIDGE_NODE_EVENT_STUCK) == 0
            && holds_open
            && hold_count < sizeof(holds) / sizeof(holds[0])
            && bridge->nodes[slot] != NULL
            && !bridge_node_is_connected(bridge->nodes[slot]))
            holds[hold_count++] = slot;
        if ((bridge->node_retention_flags[slot] & BRIDGE_NODE_LIVE_WRAPPER) == 0
            || bridge->nodes[slot] == NULL
            || !JS_IsObject(bridge->wrapper_refs[slot])) continue;
        JSValue wrapper = JS_WeakRefDeref(bridge->host->context,
                                         bridge->wrapper_refs[slot]);
        bool live = JS_IsObject(wrapper);
        JS_FreeValue(bridge->host->context, wrapper);
        if (!live) (void) bridge_release_wrapper_slot(bridge, slot, NULL, &batch);
    }
    bridge_retired_wrappers_flush(bridge, &batch);
    /* After the releases: a tree they discarded needs no holder. */
    for (size_t i = 0; i < hold_count; i++)
        bridge_hold_detached_event_state(bridge, holds[i]);
    bridge->node_reclaim_active = false;
}

static bool bridge_shadow_slot_reclaimed(const DomBridge *bridge)
{
    for (size_t i = 0; i < bridge->shadow_root_count; i++)
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, bridge->shadow_root_handles[i], NULL)) return true;
    return false;
}

static void bridge_reclaim_node_slots(DomBridge *bridge,
                                      const lxb_dom_node_t *keep,
                                      bool shadow_pressure)
{
    if (bridge->host == NULL || bridge->host->runtime == NULL
        || bridge->host->context == NULL || bridge->wrapper_refs == NULL
        || bridge->node_reclaim_active) return;
    bridge->node_reclaim_active = true;
    /* A full shadow registry needs one root, not a fraction of the much
       larger handle table. Its fruitless-collection backoff is independent
       so ordinary handle pressure cannot veto its first useful collection. */
    uint16_t *skip = shadow_pressure ? &bridge->shadow_reclaim_gc_skip
                                    : &bridge->node_reclaim_gc_skip;
    uint16_t *backoff_state = shadow_pressure ? &bridge->shadow_reclaim_gc_backoff
                                            : &bridge->node_reclaim_gc_backoff;
    size_t released = bridge_release_dead_wrappers(bridge, keep);
    bool enough = shadow_pressure ? bridge_shadow_slot_reclaimed(bridge)
        : released >= BRIDGE_RECLAIM_ENOUGH(bridge);
    if (!enough) {
        if (*skip != 0) {
            (*skip)--;
        } else {
            /* As at every explicit full collection: one pass breaks a
               wrapper self-cycle and the next clears its WeakRef. */
            JS_RunGC(bridge->host->runtime);
            JS_RunGC(bridge->host->runtime);
            released += bridge_release_dead_wrappers(bridge, keep);
            enough = shadow_pressure ? bridge_shadow_slot_reclaimed(bridge)
                : released >= BRIDGE_RECLAIM_ENOUGH(bridge);
            if (!enough) {
                uint32_t backoff = *backoff_state == 0
                    ? 8u : 2u * (uint32_t) *backoff_state;
                if (backoff > BRIDGE_RECLAIM_BACKOFF_MAX)
                    backoff = BRIDGE_RECLAIM_BACKOFF_MAX;
                *backoff_state = (uint16_t) backoff;
                *skip = (uint16_t) backoff;
            }
        }
    }
    if (enough) {
        *skip = 0;
        *backoff_state = 0;
    }
    bridge->node_reclaim_active = false;
}

typedef enum {
    BRIDGE_SUBTREE_NOT_FOUND = 0,
    BRIDGE_SUBTREE_FOUND,
    BRIDGE_SUBTREE_INDETERMINATE
} BridgeSubtreeSearchResult;

/* Visits every node of `root`'s live subtree, descending into template
   contents. Returns FOUND when `visit` stops the walk, INDETERMINATE when a
   bound cuts it short, NOT_FOUND after a complete walk. */
typedef bool (*BridgeSubtreeVisitor)(void *opaque, lxb_dom_node_t *node);

static BridgeSubtreeSearchResult bridge_live_subtree_visit(
    lxb_dom_node_t *root, size_t ownership_depth,
    BridgeSubtreeVisitor visit, void *opaque)
{
    if (root == NULL) return BRIDGE_SUBTREE_NOT_FOUND;
    if (ownership_depth >= 8) return BRIDGE_SUBTREE_INDETERMINATE;
    DomDocumentOrderTraversal traversal = {
        .next = root, .boundary = root
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (visit(opaque, at)) return BRIDGE_SUBTREE_FOUND;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && at->ns == LXB_NS_HTML) {
            size_t name_length = 0;
            const char *name = document_element_name(at, &name_length);
            if (name != NULL && name_length == 8
                && strncasecmp(name, "template", 8) == 0) {
                lxb_html_template_element_t *element =
                    lxb_html_interface_template(at);
                lxb_dom_node_t *content = element->content == NULL ? NULL
                    : lxb_dom_interface_node(element->content);
                BridgeSubtreeSearchResult nested =
                    bridge_live_subtree_visit(
                        content, ownership_depth + 1, visit, opaque);
                if (nested != BRIDGE_SUBTREE_NOT_FOUND) return nested;
            }
        }
    }
    return traversal.next == NULL
        ? BRIDGE_SUBTREE_NOT_FOUND : BRIDGE_SUBTREE_INDETERMINATE;
}

static bool bridge_subtree_visit_is_candidate(void *opaque,
                                              lxb_dom_node_t *node)
{
    return node == (const lxb_dom_node_t *) opaque;
}

static BridgeSubtreeSearchResult bridge_live_subtree_contains_node(
    lxb_dom_node_t *root, const lxb_dom_node_t *candidate)
{
    if (root == NULL || candidate == NULL) return BRIDGE_SUBTREE_NOT_FOUND;
    return bridge_live_subtree_visit(
        root, 0, bridge_subtree_visit_is_candidate, (void *) candidate);
}

/* Whether `node` may lie in `root`'s lifetime tree; a walk a bound cut
   short counts as a possible match. */
static bool bridge_subtree_may_contain(lxb_dom_node_t *root,
                                       const lxb_dom_node_t *node)
{
    return root == NULL || root == node
        || bridge_live_subtree_contains_node(root, node)
               != BRIDGE_SUBTREE_NOT_FOUND;
}

static lxb_dom_node_t *bridge_owned_lifetime_root(lxb_dom_node_t *node)
{
    if (node == NULL) return NULL;
    lxb_dom_node_t *root = node;
    size_t ownership_hops = 0;
    for (;;) {
        lxb_dom_node_t *parent = NULL;
        while (root != NULL) {
            parent = root->parent;
            if (parent == NULL) break;
            root = parent;
        }
        if (root == NULL
            || root->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) break;
        lxb_dom_document_fragment_t *fragment =
            lxb_dom_interface_document_fragment(root);
        lxb_dom_element_t *host = fragment->host;
        if (host == NULL) break;
        /* A fragment owned by a template is not an independent lifetime
           root. Refuse reclamation when the bounded ownership walk cannot
           prove that it reached the outermost host. */
        if (ownership_hops++ >= 8) return NULL;
        root = lxb_dom_interface_node(host);
    }
    return root;
}
