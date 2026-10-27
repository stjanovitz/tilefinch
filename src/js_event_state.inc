/* Event-state retention for the DOM bridge: where each node's listener and
   handler maps live (rooted, held by a detached tree's holder, or kept on
   its wrapper), and the native side of the bootstrap's hold and release
   hooks (dom.js, the event-state store). Included by js_dom_bindings.c at
   this point of that translation unit, so these functions stay static and
   keep using the handle table's static helpers defined above the include
   (bridge_node_index_find, bridge_live_subtree_visit,
   bridge_owned_lifetime_root, bridge_arm_wrapper_sweep, ...) of
   js_dom_handles.inc; the handle table, wrapper sweep and DOM natives call
   back in through the forward declarations near the top of
   js_dom_handles.inc. Its state is DomBridge.events (DomEventState, in
   js_runtime_internal.h); its hooks are SCRIPT_HOST_HOLD_EVENT_STATE and
   SCRIPT_HOST_RELEASE_EVENT_HOLDERS. */

static bool bridge_unstick_visit(void *opaque, lxb_dom_node_t *node)
{
    DomBridge *bridge = opaque;
    size_t slot = bridge_node_index_find(bridge, node);
    if (slot != SIZE_MAX
        && (bridge->node_retention_flags[slot] & BRIDGE_NODE_EVENT_STUCK)
               != 0) {
        bridge->node_retention_flags[slot] &=
            (unsigned char) ~BRIDGE_NODE_EVENT_STUCK;
        if (bridge->events.stuck_slots != 0) bridge->events.stuck_slots--;
    }
    return bridge->events.stuck_slots == 0;
}

/* Clears STUCK in `root`'s lifetime tree and arms the sweep when it was
   there. Other stuck trees wait for a change of their own: retrying them
   all here cost a table scan, a full sweep and a refused hold on every
   detached wrapper release. A walk a bound cut short reopens them all. */
static void bridge_reopen_stuck_tree(DomBridge *bridge, lxb_dom_node_t *root)
{
    size_t stuck = bridge->events.stuck_slots;
    if (bridge_live_subtree_visit(root, 0, bridge_unstick_visit, bridge)
        == BRIDGE_SUBTREE_INDETERMINATE) {
        for (size_t at = 0; at < bridge->node_count; at++)
            bridge->node_retention_flags[at] &=
                (unsigned char) ~BRIDGE_NODE_EVENT_STUCK;
        bridge->events.stuck_slots = 0;
    }
    if (bridge->events.stuck_slots == stuck) return;
#ifdef JS_RT_TEST_INSTRUMENTS
    bridge->events.stuck_reopens++;
#endif
    bridge_arm_wrapper_sweep(bridge);
}

/* Event state (see BRIDGE_NODE_EVENT_ROOTED and dom.js). A detached tree
   whose state stays rooted can never be collected when a handler closes
   over its element, so the wrapper sweep hands each detached, unpinned
   tree's state to a holder only its live wrappers reference. Whatever could
   let the tree outlive those wrappers roots it again first: an insertion of
   or into it (bridge_event_state_joining) or a pin (bridge_pin_node_slot).
   A wrapper made later in the tree joins its holder
   (bridge_wrapper_event_holder). */
typedef struct {
    double *items;
    size_t count;
    size_t capacity;
} BridgeHandleList;

static bool bridge_handle_list_push(DomBridge *bridge, BridgeHandleList *list,
                                    int64_t handle)
{
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 16u : list->capacity * 2u;
        double *items = budget_realloc(bridge->budget, list->items,
                                       capacity * sizeof(*items));
        if (items == NULL) return false;
        list->items = items;
        list->capacity = capacity;
    }
    /* Handles are below 2^53, so a Float64Array carries them exactly. */
    list->items[list->count++] = (double) handle;
    return true;
}

static JSValue bridge_handle_list_value(JSContext *context,
                                        const BridgeHandleList *list)
{
    JSValue buffer = JS_NewArrayBufferCopy(
        context, (const uint8_t *) list->items,
        list->count * sizeof(*list->items));
    if (JS_IsException(buffer)) return buffer;
    JSValueConst view[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};
    JSValue array = JS_NewTypedArray(context, 3, view, JS_TYPED_ARRAY_FLOAT64);
    JS_FreeValue(context, buffer);
    return array;
}

/* What a trusted event-state hook answered when it threw: the realm's
   heap refused an allocation. The bootstrap then left every node's state
   where it was (dom.js), except that an unfinished release keeps its
   holders for a later one. */
#define BRIDGE_EVENT_HOOK_THREW (-1)

/* Whether holds, and retries of unfinished releases, may run: nothing
   failed, or the heap has collected enough times since. */
static bool bridge_event_retry_open(const DomBridge *bridge)
{
    return bridge->events.retry_backoff == 0
        || bridge->host->heap_collections - bridge->events.retry_collection
               >= bridge->events.retry_backoff;
}

/* Records a hook's answer: a throw closes holds and release retries until
   more collections ran (doubling the wait while they keep failing); a
   finished release finished every unfinished one before it. */
static void bridge_event_hook_answered(DomBridge *bridge, int answer,
                                       bool release)
{
    if (answer != BRIDGE_EVENT_HOOK_THREW) {
        if (release) bridge->events.release_pending = false;
        if (!bridge->events.release_pending) bridge->events.retry_backoff = 0;
        return;
    }
#ifdef JS_RT_TEST_INSTRUMENTS
    bridge->events.hook_failures++;
#endif
    if (release) bridge->events.release_pending = true;
    bridge->events.retry_backoff = bridge->events.retry_backoff == 0 ? 1u
        : bridge->events.retry_backoff >= 32u ? 64u
        : (uint8_t) (bridge->events.retry_backoff * 2u);
    bridge->events.retry_collection = bridge->host->heap_collections;
}

/* Calls a trusted event-state hook with one Float64Array per list: 1 when
   it returned true, 0 otherwise, BRIDGE_EVENT_HOOK_THREW when it threw.
   A release whose lists could not be made hands the bootstrap each
   handle as a number instead, which needs no allocation to reach it, so
   it can at least keep their holders for a later release. */
static int bridge_event_state_call(DomBridge *bridge, JSValueConst hook,
                                   const BridgeHandleList *lists,
                                   int count)
{
    JSContext *context = bridge->host == NULL ? NULL : bridge->host->context;
    if (context == NULL || !JS_IsFunction(context, hook)) return 0;
    bool release = JS_VALUE_GET_PTR(hook)
        == JS_VALUE_GET_PTR(js_rt_bridge_trusted_hook(
               bridge, SCRIPT_HOST_RELEASE_EVENT_HOLDERS));
    JSValue arguments[2] = {JS_UNDEFINED, JS_UNDEFINED};
    bool ready = true;
    for (int i = 0; i < count && ready; i++) {
        arguments[i] = bridge_handle_list_value(context, &lists[i]);
        ready = !JS_IsException(arguments[i]);
    }
#ifdef JS_RT_TEST_INSTRUMENTS
    bridge->events.hook_calls++;
#endif
#if !defined(__PSP__)
    TilefinchTestFaults *faults = tilefinch_test_faults();
    unsigned *refuse = release ? &faults->event_release_refuse_allocation
                               : &faults->event_hold_refuse_allocation;
    bool armed = ready && *refuse != 0;
    if (armed) {
        JS_TestRefuseAllocation(*refuse);
        *refuse = 0;
    }
#endif
    JSValue result = ready
        ? JS_Call(context, hook, JS_UNDEFINED, count, arguments)
        : JS_EXCEPTION;
#if !defined(__PSP__)
    if (armed) {
        faults->event_hook_refused = !JS_TestRefusalPending();
        JS_TestRefuseAllocation(0);
    }
#endif
    int answer = JS_IsException(result) ? BRIDGE_EVENT_HOOK_THREW
        : JS_IsBool(result) && JS_ToBool(context, result) > 0;
    if (JS_IsException(result)) JS_FreeValue(context, JS_GetException(context));
    JS_FreeValue(context, result);
    for (int i = 0; i < count; i++) JS_FreeValue(context, arguments[i]);
    if (!ready && release) {
        for (size_t i = 0; i < lists[0].count; i++) {
            JSValue handle = JS_NewFloat64(context, lists[0].items[i]);
            result = JS_Call(context, hook, JS_UNDEFINED, 1, &handle);
            if (JS_IsException(result))
                JS_FreeValue(context, JS_GetException(context));
            JS_FreeValue(context, result);
        }
    }
    return answer;
}

static bool bridge_wrapper_slot_live(DomBridge *bridge, size_t slot)
{
    if ((bridge->node_retention_flags[slot] & BRIDGE_NODE_LIVE_WRAPPER) == 0
        || bridge->wrapper_refs == NULL
        || !JS_IsObject(bridge->wrapper_refs[slot])) return false;
    JSValue wrapper = JS_WeakRefDeref(bridge->host->context,
                                      bridge->wrapper_refs[slot]);
    bool live = JS_IsObject(wrapper);
    JS_FreeValue(bridge->host->context, wrapper);
    return live;
}

typedef struct {
    DomBridge *bridge;
    /* Holding: handles with event state, then those of live wrappers.
       Otherwise: handles with held state only. */
    BridgeHandleList lists[2];
    bool holding;
    bool refused;
    /* Joining (bridge_event_state_joining): whether the moving subtree can
       join the holder of the tree it moves into. */
    bool joinable;
    /* bridge_tree_held_handle: the first HELD handle found. */
    int64_t found;
} BridgeEventStateScan;

static bool bridge_event_state_visit(void *opaque, lxb_dom_node_t *node)
{
    BridgeEventStateScan *scan = opaque;
    DomBridge *bridge = scan->bridge;
    size_t slot = bridge_node_index_find(bridge, node);
    if (slot == SIZE_MAX) return false;
    unsigned char flags = bridge->node_retention_flags[slot];
    int64_t handle = bridge_node_handle(bridge, slot);
    if (!scan->holding) {
        if ((flags & BRIDGE_NODE_EVENT_HELD) != 0
            && !bridge_handle_list_push(bridge, &scan->lists[0], handle))
            scan->refused = true;
        return scan->refused;
    }
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_EVENT_IN_HELD_TREE;
    /* A pinned node outlives every wrapper: its tree stays rooted. */
    if ((flags & BRIDGE_NODE_NATIVE_PIN) != 0
        || ((flags & BRIDGE_NODE_EVENT_STATE) != 0
            && !bridge_handle_list_push(bridge, &scan->lists[0], handle))
        || (bridge_wrapper_slot_live(bridge, slot)
            && !bridge_handle_list_push(bridge, &scan->lists[1], handle)))
        scan->refused = true;
    return scan->refused;
}

/* Walks `root`'s lifetime tree; false when a bound or a refused list cut
   it short. */
static bool bridge_event_state_scan(DomBridge *bridge, lxb_dom_node_t *root,
                                    bool holding, BridgeEventStateScan *scan)
{
    *scan = (BridgeEventStateScan) {.bridge = bridge, .holding = holding};
    return bridge_live_subtree_visit(root, 0, bridge_event_state_visit, scan)
               == BRIDGE_SUBTREE_NOT_FOUND
        && !scan->refused;
}

static void bridge_event_state_scan_free(BridgeEventStateScan *scan)
{
    for (size_t i = 0; i < 2; i++)
        budget_free(scan->bridge->budget, scan->lists[i].items);
}

/* Roots again the held event state found in `root`'s subtree. */
static void bridge_release_event_state(DomBridge *bridge,
                                       lxb_dom_node_t *root)
{
    if (bridge->events.held_slots == 0) return;
    BridgeEventStateScan scan;
    (void) bridge_event_state_scan(bridge, root, false, &scan);
    if (scan.lists[0].count != 0)
        bridge_event_hook_answered(
            bridge,
            bridge_event_state_call(
                bridge,
                js_rt_bridge_trusted_hook(
                    bridge, SCRIPT_HOST_RELEASE_EVENT_HOLDERS),
                scan.lists, 1),
            true);
    bridge_event_state_scan_free(&scan);
}

static bool bridge_first_held_visit(void *opaque, lxb_dom_node_t *node)
{
    BridgeEventStateScan *scan = opaque;
    DomBridge *bridge = scan->bridge;
    size_t slot = bridge_node_index_find(bridge, node);
    if (slot == SIZE_MAX
        || (bridge->node_retention_flags[slot] & BRIDGE_NODE_EVENT_HELD) == 0)
        return false;
    scan->found = bridge_node_handle(bridge, slot);
    return true;
}

/* A handle HELD in detached `root`'s lifetime tree, or 0; a repeat query
   on an unchanged tree reuses the last answer without the walk. */
static int64_t bridge_tree_held_handle(DomBridge *bridge, lxb_dom_node_t *root)
{
    if (root != bridge->events.holder_probe_root
        || bridge->events.holder_epoch != bridge->events.holder_probe_epoch) {
        BridgeEventStateScan scan = {.bridge = bridge};
        bool found = bridge_live_subtree_visit(
            root, 0, bridge_first_held_visit, &scan)
            == BRIDGE_SUBTREE_FOUND;
        bridge->events.holder_probe_root = root;
        bridge->events.holder_probe_epoch = bridge->events.holder_epoch;
        bridge->events.holder_probe_handle = found ? scan.found : 0;
    }
    return bridge->events.holder_probe_handle;
}

/* Joining: the held handles moving and, when it may join a holder, the
   moving subtree's live wrappers (marking its nodes as in a held tree); a
   pinned node refuses the join, as it refuses a hold. */
static bool bridge_event_state_join_visit(void *opaque, lxb_dom_node_t *node)
{
    BridgeEventStateScan *scan = opaque;
    DomBridge *bridge = scan->bridge;
    size_t slot = bridge_node_index_find(bridge, node);
    if (slot == SIZE_MAX) return false;
    unsigned char flags = bridge->node_retention_flags[slot];
    int64_t handle = bridge_node_handle(bridge, slot);
    if ((flags & BRIDGE_NODE_EVENT_HELD) != 0
        && !bridge_handle_list_push(bridge, &scan->lists[0], handle))
        scan->refused = true;
    if (scan->holding) {
        bridge->node_retention_flags[slot] |= BRIDGE_NODE_EVENT_IN_HELD_TREE;
        if ((flags & BRIDGE_NODE_NATIVE_PIN) != 0
            || (bridge_wrapper_slot_live(bridge, slot)
                && !bridge_handle_list_push(bridge, &scan->lists[1],
                                            handle)))
            scan->joinable = false;
    }
    return scan->refused;
}

/* Before `node` moves under `parent`: the held state moving with it, and
   that of the tree it joins, must not depend on the wrappers of only one
   side. Nothing to do unless one side may hold state: the moving subtree
   has held nodes, or `parent` is marked BRIDGE_NODE_EVENT_IN_HELD_TREE (a
   connected tree's state is never held). Into a held tree, the moving
   subtree's wrappers join its holder, so a tree built step by step is not
   rooted again on every insertion only for the next sweep to hold it
   again. Otherwise, or when they cannot join, both sides' holders are
   released (__tilefinchReleaseEventHolders, which gets the parent's
   handle, then the moving held handles, and the wrappers to join). */
static void bridge_event_state_joining(DomBridge *bridge,
                                       lxb_dom_node_t *parent,
                                       lxb_dom_node_t *node)
{
    if (bridge->events.held_slots == 0) return;
    size_t slot = bridge_node_index_find(bridge, parent);
    bool held_tree = slot != SIZE_MAX
        && (bridge->node_retention_flags[slot]
            & BRIDGE_NODE_EVENT_IN_HELD_TREE) != 0;
    if (held_tree && bridge_node_is_connected(parent)) {
        bridge->node_retention_flags[slot] &=
            (unsigned char) ~BRIDGE_NODE_EVENT_IN_HELD_TREE;
        held_tree = false;
    }
    bool moving = !bridge_node_is_connected(node);
    BridgeEventStateScan scan = {.bridge = bridge};
    scan.holding = held_tree && moving;
    scan.joinable = scan.holding;
    if (moving
        && bridge_live_subtree_visit(
               node, 0, bridge_event_state_join_visit, &scan)
               != BRIDGE_SUBTREE_NOT_FOUND)
        scan.joinable = false;
    size_t moved = scan.lists[0].count;
    if (held_tree || moved != 0) {
        /* Held state changes trees: no probe may answer from before. */
        if (moved != 0) bridge->events.holder_epoch++;
        BridgeHandleList call[2] = {{0}, scan.lists[1]};
        bool listed = bridge_handle_list_push(
            bridge, &call[0],
            slot == SIZE_MAX ? 0 : bridge_node_handle(bridge, slot));
        for (size_t i = 0; listed && i < moved; i++)
            listed = bridge_handle_list_push(
                bridge, &call[0], (int64_t) scan.lists[0].items[i]);
        /* A list cut short releases what it has, as before. A release
           that threw may have left the parent's holder: keep the mark. */
        int answer = bridge_event_state_call(
            bridge,
            js_rt_bridge_trusted_hook(
                bridge, SCRIPT_HOST_RELEASE_EVENT_HOLDERS),
            call, listed && scan.joinable ? 2 : 1);
        bridge_event_hook_answered(bridge, answer, true);
        bool parent_held = answer != 0;
        budget_free(bridge->budget, call[0].items);
        /* The mark outlived the parent's holder. */
        slot = bridge_node_index_find(bridge, parent);
        if (!parent_held && slot != SIZE_MAX)
            bridge->node_retention_flags[slot] &=
                (unsigned char) ~BRIDGE_NODE_EVENT_IN_HELD_TREE;
    }
    bridge_event_state_scan_free(&scan);
}

void bridge_pin_node_slot(DomBridge *bridge, size_t slot)
{
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
    if (bridge->events.held_slots == 0) return;
    lxb_dom_node_t *root = bridge_owned_lifetime_root(bridge->nodes[slot]);
    if (root != NULL && !bridge_node_is_connected(root))
        bridge_release_event_state(bridge, root);
}

/* A hold refused by a wrapper the page made non-extensible: the tree's
   rooted and kept nodes, whose state stayed where it was, are STUCK until
   the tree changes. */
static TILEFINCH_COLD_PATH void bridge_mark_stuck(DomBridge *bridge,
                                                const BridgeHandleList *list)
{
    for (size_t i = 0; i < list->count; i++) {
        size_t slot = 0;
        if (!js_rt_bridge_node_slot_for_handle(
                bridge, (int64_t) list->items[i], &slot)) continue;
        unsigned char event = bridge->node_retention_flags[slot]
            & (BRIDGE_NODE_EVENT_ROOTED | BRIDGE_NODE_EVENT_KEPT);
        if (event != 0) bridge_set_event_state(bridge, slot, event, true);
    }
}

/* Retries the releases the bootstrap could not finish (a number as the
   handle list: no allocation reaches it). */
static TILEFINCH_COLD_PATH void bridge_retry_event_releases(DomBridge *bridge)
{
    JSContext *context = bridge->host->context;
    JSValueConst hook = js_rt_bridge_trusted_hook(
        bridge, SCRIPT_HOST_RELEASE_EVENT_HOLDERS);
    if (context == NULL || !JS_IsFunction(context, hook)) return;
#ifdef JS_RT_TEST_INSTRUMENTS
    bridge->events.release_retries++;
#endif
    JSValue none = JS_NewInt32(context, 0);
    JSValue result = JS_Call(context, hook, JS_UNDEFINED, 1, &none);
    bool threw = JS_IsException(result);
    if (threw) JS_FreeValue(context, JS_GetException(context));
    JS_FreeValue(context, result);
    bridge_event_hook_answered(bridge, threw ? BRIDGE_EVENT_HOOK_THREW : 0,
                               true);
}

static void bridge_hold_detached_event_state(DomBridge *bridge, size_t slot)
{
    lxb_dom_node_t *node = bridge->nodes[slot];
    if (!bridge_event_retry_open(bridge)) return;
    if (node == NULL
        || (bridge->node_retention_flags[slot]
            & (BRIDGE_NODE_EVENT_ROOTED | BRIDGE_NODE_EVENT_KEPT)) == 0
        || (bridge->node_retention_flags[slot] & BRIDGE_NODE_EVENT_STUCK) != 0)
        return;
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
    if (root == NULL || bridge_node_is_connected(root)) return;
    BridgeEventStateScan scan;
    /* A tree without a live wrapper is about to be discarded, or is kept
       by native work that may still dispatch to it: leave it rooted. */
    if (bridge_event_state_scan(bridge, root, true, &scan)
        && scan.lists[1].count != 0) {
        int answer = bridge_event_state_call(
            bridge,
            js_rt_bridge_trusted_hook(bridge, SCRIPT_HOST_HOLD_EVENT_STATE),
            scan.lists, 2);
        if (answer == 0) bridge_mark_stuck(bridge, &scan.lists[0]);
        bridge_event_hook_answered(bridge, answer, false);
    }
    bridge_event_state_scan_free(&scan);
}

/* Moves `slot`'s event state bits to `event` (one of the
   BRIDGE_NODE_EVENT_STATE bits, or 0) and STUCK to `stuck`, keeping the
   held and stuck counts, and releasing a kept wrapper's reference when the
   state leaves it. Taking that reference is js_dom_note_event_state's. */
static void bridge_set_event_state(DomBridge *bridge, size_t slot,
                                   unsigned char event, bool stuck)
{
    unsigned char flags = bridge->node_retention_flags[slot];
    stuck = stuck && (event == BRIDGE_NODE_EVENT_ROOTED
                      || event == BRIDGE_NODE_EVENT_KEPT);
    bool was_held = (flags & BRIDGE_NODE_EVENT_HELD) != 0;
    if (was_held != (event == BRIDGE_NODE_EVENT_HELD)) {
        if (!was_held) bridge->events.held_slots++;
        else if (bridge->events.held_slots != 0) bridge->events.held_slots--;
#ifdef JS_RT_TEST_INSTRUMENTS
        if (was_held) bridge->events.held_releases++;
#endif
        bridge->events.holder_epoch++;
    }
    bool was_stuck = (flags & BRIDGE_NODE_EVENT_STUCK) != 0;
    if (was_stuck != stuck) {
        if (!was_stuck) bridge->events.stuck_slots++;
        else if (bridge->events.stuck_slots != 0) bridge->events.stuck_slots--;
    }
    bridge->node_retention_flags[slot] = (unsigned char) (
        (flags & ~(BRIDGE_NODE_EVENT_STATE | BRIDGE_NODE_EVENT_STUCK))
        | event | (stuck ? BRIDGE_NODE_EVENT_STUCK : 0u));
    if ((flags & BRIDGE_NODE_EVENT_KEPT) == 0
        || event == BRIDGE_NODE_EVENT_KEPT) return;
    /* The reference js_dom_note_event_state took: the wrapper is alive
       while it is held, so its WeakRef still answers. */
    JSContext *context = bridge->host == NULL ? NULL : bridge->host->context;
    if (context == NULL || bridge->wrapper_refs == NULL
        || !JS_IsObject(bridge->wrapper_refs[slot])) return;
    JSValue wrapper = JS_WeakRefDeref(context, bridge->wrapper_refs[slot]);
    if (!JS_IsObject(wrapper)) return;
    JS_FreeValue(context, wrapper);
    JS_FreeValue(context, wrapper);
}

/* Native: where the node of handle argv[0] keeps its event state, argv[1]:
   0 nowhere, 1 rooted, 2 held, 3 kept on its wrapper argv[2]. A note ends
   STUCK (bridge_mark_stuck). False when the wrapper cannot be kept (it is
   not the node's live wrapper). */
JSValue js_dom_note_event_state(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    int32_t state = 0;
    size_t slot = 0;
    if (argc < 2 || JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &state, argv[1]) < 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        return JS_FALSE;
    int32_t where = state & 3;
    unsigned char event = where == 1 ? BRIDGE_NODE_EVENT_ROOTED
        : where == 2 ? BRIDGE_NODE_EVENT_HELD
        : where == 3 ? BRIDGE_NODE_EVENT_KEPT : 0;
    if (event == BRIDGE_NODE_EVENT_KEPT
        && (bridge->node_retention_flags[slot] & BRIDGE_NODE_EVENT_KEPT) == 0) {
        /* Hold one reference to the node's live wrapper, the one carrying
           the state, until the state leaves it. */
        if (argc < 3 || !JS_IsObject(argv[2])
            || (bridge->node_retention_flags[slot]
                & BRIDGE_NODE_LIVE_WRAPPER) == 0
            || bridge->wrapper_refs == NULL
            || !JS_IsObject(bridge->wrapper_refs[slot]))
            return JS_FALSE;
        JSValue wrapper = JS_WeakRefDeref(context, bridge->wrapper_refs[slot]);
        if (!JS_IsObject(wrapper)
            || JS_VALUE_GET_PTR(wrapper) != JS_VALUE_GET_PTR(argv[2])) {
            JS_FreeValue(context, wrapper);
            return JS_FALSE;
        }
        /* `wrapper` is the kept reference. */
    }
    bridge_set_event_state(bridge, slot, event, false);
    if ((event == BRIDGE_NODE_EVENT_ROOTED || event == BRIDGE_NODE_EVENT_KEPT)
        && !bridge_node_is_connected(bridge->nodes[slot]))
        bridge_arm_wrapper_sweep(bridge);
    return JS_TRUE;
}

/* Event targets: the bootstrap's listener and handler maps (dom.js), a
   pair keyed by node handle, start at 128 targets per map and double, up
   to the limit, while the realm's heap can take both maps' growth and
   still keep the execution reserve every script admission keeps free
   (script_runtime_heap_available, script_admission.h). A target costs one
   Map record per map (key and value JSValues, list links, hash chain), its
   allocation header and its share of the hash table: 80.6 bytes measured
   on the 64-bit host (4,096 entries), about 52 on the PSP (a 40-byte record
   with NaN-boxed values and 4-byte pointers). A full pair costs about
   416 KiB on the PSP; every doubling is admitted against the heap as it is
   then, so it never passes the reserve. 4,096 targets is half the base
   handle table. */
#define BRIDGE_EVENT_TARGET_LIMIT 4096u
#define BRIDGE_EVENT_TARGET_ENTRY_BYTES (sizeof(void *) == 4u ? 52u : 81u)

/* Native: the capacity a pair of maps holding argv[0] targets each may
   grow to; argv[0] itself when growth is refused. */
JSValue js_dom_grow_event_targets(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t current = 0;
    if (argc < 1 || JS_ToInt64(context, &current, argv[0]) < 0
        || current <= 0 || bridge == NULL || bridge->host == NULL)
        return JS_NewInt64(context, current);
    if ((uint64_t) current >= BRIDGE_EVENT_TARGET_LIMIT)
        return JS_NewInt64(context, current);
    size_t next = (size_t) current * 2u;
    if (next > BRIDGE_EVENT_TARGET_LIMIT) next = BRIDGE_EVENT_TARGET_LIMIT;
#ifndef __PSP__
    if (tilefinch_test_faults()->refuse_event_target_growth)
        return JS_NewInt64(context, current);
#endif
    size_t per_target = 2u * BRIDGE_EVENT_TARGET_ENTRY_BYTES;
    size_t step = (next - (size_t) current) * per_target;
    size_t available = script_runtime_heap_available(bridge->host);
    if (step > available
        || SCRIPT_ADMISSION_EXECUTION_RESERVE_BYTES > available - step)
        return JS_NewInt64(context, current);
    return JS_NewInt64(context, (int64_t) next);
}

/* The handle whose held state a new wrapper for slot `slot` joins: one
   HELD in the node's detached tree, or 0. */
static int64_t bridge_wrapper_event_holder(DomBridge *bridge, size_t slot)
{
    if (bridge->events.held_slots == 0) return 0;
    int64_t held = (bridge->node_retention_flags[slot]
                    & BRIDGE_NODE_EVENT_HELD) != 0
        ? bridge_node_handle(bridge, slot) : 0;
    if (held == 0) {
        lxb_dom_node_t *root = bridge_owned_lifetime_root(bridge->nodes[slot]);
        if (root == NULL || bridge_node_is_connected(root)) return 0;
        held = bridge_tree_held_handle(bridge, root);
    }
    /* An insertion under the node must now consider that holder. */
    if (held != 0)
        bridge->node_retention_flags[slot] |= BRIDGE_NODE_EVENT_IN_HELD_TREE;
    return held;
}
