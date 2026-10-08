/* DOM bindings: node handle registry, stable-node identity, selector
   queries, traversal, creation, content/attribute/style access, mutation
   bindings, and mutation-journal classification.  Split from js_runtime.c;
   shares the runtime internals through js_runtime_internal.h.  Exported
   binding symbols keep their js_* names so the orchestrator's install chain
   preserves the script-observable property order. */
#include "style_internal.h"
#include "style_cache_internal.h"
#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#include "js_runtime_internal.h"
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/dom/interfaces/attr.h>

#include "tilefinch/platform.h"
#include "tilefinch/resources.h"
#include "tilefinch_compiler.h"
#include "tilefinch/media_discovery.h"

#include <lexbor/dom/interfaces/element.h>
#include <lexbor/ns/ns.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

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
                                             int64_t handle)
{
    if (bridge == NULL || bridge->host == NULL
        || bridge->host->context == NULL || handle <= 0) return;
    JSContext *context = bridge->host->context;
    if (JS_IsFunction(
            context, bridge->trusted_retire_native_node_state)) {
        JSValue argument = JS_NewInt64(context, handle);
        JSValue result = JS_Call(
            context, bridge->trusted_retire_native_node_state,
            JS_UNDEFINED, 1, &argument);
        JS_FreeValue(context, argument);
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
    size_t write = 0;
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
                           / DOM_BRIDGE_SHADOW_ROOT_BUDGET_BYTES;
        if (by_budget > admitted) admitted = by_budget;
    }
    if (admitted > DOM_BRIDGE_SHADOW_ROOT_LIMIT)
        admitted = DOM_BRIDGE_SHADOW_ROOT_LIMIT;
    if (write >= admitted) return JS_FALSE;
    /* Style resolution recognises the carrier by this native-only mark
       (shadow composition, document.h). */
    if (!document_mark_shadow_carrier(bridge->nodes[slot])) return JS_FALSE;
    bridge->shadow_root_handles[write] = (uint32_t) handle;
    bridge->shadow_root_count = write + 1u;
    return JS_TRUE;
}

static void bridge_wrapper_ref_clear(DomBridge *bridge, size_t slot);
static void bridge_reclaim_node_slots(DomBridge *bridge,
                                      const lxb_dom_node_t *keep);

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
    bridge->node_retention_flags[slot] =
        notify ? 0 : BRIDGE_NODE_PENDING_RETIRE_NOTIFY;
    if (bridge->result != NULL
        && bridge->result->dom_handle_slots_live != 0) {
        bridge->result->dom_handle_slots_live--;
    }
    if (notify) bridge_notify_node_state_retired(bridge, retired_handle);
    return retired_handle;
}

/* Records whether the next Page controls claim asked to skip a repeated
   entry notice. Only script_runtime_take_page_controls_notice() reads it,
   and it can only quiet a notice this document has already shown. */
JSValue js_dom_page_controls_notice(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL)
        bridge->page_controls_notice_once =
            argc > 0 && JS_ToBool(context, argv[0]) > 0;
    return JS_UNDEFINED;
}

JSValue js_dom_set_fullscreen(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t enabled = 0;
    int64_t handle = 0;
    if (bridge == NULL || bridge->host == NULL || argc < 2
        || JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToInt32(context, &enabled, argv[1]) < 0) return JS_FALSE;
    if (enabled != 0) {
        size_t slot = 0;
        if (!js_rt_bridge_user_activation_is_active(bridge)
            || bridge->host->document_scope
                   != SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
            || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
            || bridge->nodes[slot]->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !bridge_node_is_connected(bridge->nodes[slot])) {
            return JS_FALSE;
        }
        /* requestFullscreen() is an activation-consuming API. The sticky
           hasBeenActive bit remains set, but this gesture cannot authorize a
           later privileged operation. */
        js_rt_bridge_consume_user_activation(bridge);
        bridge->fullscreen_node_handle = handle;
        if (bridge->stylesheet != NULL) {
            ((Stylesheet *) bridge->stylesheet)->fullscreen_node =
                bridge->nodes[slot];
        }
    } else {
        bridge->fullscreen_node_handle = 0;
        if (bridge->stylesheet != NULL) {
            ((Stylesheet *) bridge->stylesheet)->fullscreen_node = NULL;
        }
    }
    if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;
    return JS_TRUE;
}

TilefinchGameAudio *js_rt_game_audio_engine(ScriptRuntime *runtime)
{
    if (runtime == NULL) return NULL;
    if (runtime->game_audio == NULL)
        runtime->game_audio = tilefinch_game_audio_create(runtime->budget);
    return runtime->game_audio;
}

bool js_rt_game_audio_lifecycle(ScriptRuntime *runtime, bool destroy)
{
    if (runtime == NULL) return false;
    if (destroy) {
        tilefinch_game_audio_destroy(runtime->game_audio);
        runtime->game_audio = NULL;
    } else tilefinch_game_audio_suspend(runtime->game_audio);
    return true;
}

#ifdef TILEFINCH_GAME_AUDIO_REFERENCE
static TilefinchGameAudio *runtime_game_audio(DomBridge *bridge)
{
    return bridge == NULL ? NULL : js_rt_game_audio_engine(bridge->host);
}

JSValue js_game_audio_decode(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t length = 0;
    uint8_t *bytes = argc > 0
        ? JS_GetArrayBuffer(context, &length, argv[0]) : NULL;
    if (bytes == NULL || length == 0) return JS_NULL;
    TilefinchGameAudioBufferInfo info = {0};
    if (!tilefinch_game_audio_decode_wav(
            runtime_game_audio(bridge), bytes, length, &info)) return JS_NULL;
    JSValue object = JS_NewObject(context);
    if (JS_IsException(object)) return object;
    if (JS_SetPropertyStr(context, object, "handle",
                          JS_NewUint32(context, info.handle)) < 0
        || JS_SetPropertyStr(context, object, "length",
                             JS_NewUint32(context, info.frames)) < 0
        || JS_SetPropertyStr(context, object, "sampleRate",
                             JS_NewUint32(context, info.sample_rate)) < 0
        || JS_SetPropertyStr(context, object, "numberOfChannels",
                             JS_NewInt32(context, info.channels)) < 0) {
        JS_FreeValue(context, object);
        return JS_EXCEPTION;
    }
    return object;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
static bool game_audio_native_measurement;
static unsigned game_audio_native_depth;
static ScriptGameAudioNativeMetrics game_audio_native_metrics;

void script_runtime_game_audio_native_measure(bool enabled)
{
    game_audio_native_measurement = enabled;
    memset(&game_audio_native_metrics, 0, sizeof(game_audio_native_metrics));
}

void script_runtime_game_audio_native_take(ScriptGameAudioNativeMetrics *metrics)
{
    if (metrics != NULL) *metrics = game_audio_native_metrics;
    memset(&game_audio_native_metrics, 0, sizeof(game_audio_native_metrics));
}

static JSValue js_game_audio_command_impl(JSContext *context,
#else
JSValue js_game_audio_command(JSContext *context,
#endif
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t command = -1;
    if (bridge == NULL || bridge->host == NULL || argc < 1) return JS_FALSE;
    if (JS_ToInt32(context, &command, argv[0]) < 0) return JS_EXCEPTION;
    switch ((TilefinchGameAudioCommand) command) {
    case TILEFINCH_GAME_AUDIO_COMMAND_RESUME:
        if (!js_rt_bridge_user_activation_is_active(bridge)) return JS_FALSE;
        return tilefinch_game_audio_resume(runtime_game_audio(bridge))
            ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_SUSPEND:
        return JS_NewBool(context, js_rt_game_audio_lifecycle(bridge->host, false));
    case TILEFINCH_GAME_AUDIO_COMMAND_CLOSE:
        return JS_NewBool(context, js_rt_game_audio_lifecycle(bridge->host, true));
    case TILEFINCH_GAME_AUDIO_COMMAND_START_OSCILLATOR: {
        int32_t type = 0;
        double frequency = 0, gain_left = 1, gain_right = 1, delay = 0;
        if (argc < 6 || JS_ToInt32(context, &type, argv[1]) < 0
            || JS_ToFloat64(context, &frequency, argv[2]) < 0
            || JS_ToFloat64(context, &gain_left, argv[3]) < 0
            || JS_ToFloat64(context, &gain_right, argv[4]) < 0
            || JS_ToFloat64(context, &delay, argv[5]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        uint32_t voice = 0;
        if (!tilefinch_game_audio_start_oscillator(
                bridge->host->game_audio,
                (TilefinchGameAudioOscillatorType) type,
                frequency, gain_left, gain_right, delay, &voice))
            return JS_FALSE;
        return JS_NewUint32(context, voice);
    }
    default:
        break;
    }
    uint32_t handle = 0;
    if (argc < 2) return JS_FALSE;
    if (JS_ToUint32(context, &handle, argv[1]) < 0) return JS_EXCEPTION;
    /* Every coercion can run author code, which may close the context.
       Resolve the runtime-owned engine only after the last one, so a nested
       close() cannot leave a stale native pointer in this command. */
    switch ((TilefinchGameAudioCommand) command) {
    case TILEFINCH_GAME_AUDIO_COMMAND_STOP: {
        double delay = 0;
        if (argc > 2 && JS_ToFloat64(context, &delay, argv[2]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        tilefinch_game_audio_stop(bridge->host->game_audio, handle, delay);
        return JS_TRUE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_SET_GAIN: {
        double gain_left = 1, gain_right = 1;
        if (argc < 4
            || JS_ToFloat64(context, &gain_left, argv[2]) < 0
            || JS_ToFloat64(context, &gain_right, argv[3]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_update_voice(
                bridge->host->game_audio, handle, gain_left, gain_right)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_SET_FREQUENCY: {
        double frequency = 0;
        if (argc < 3
            || JS_ToFloat64(context, &frequency, argv[2]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_update_oscillator(
                bridge->host->game_audio, handle, frequency)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_GAIN_TARGET: {
        double gain_left = 0, gain_right = 0;
        double delay = 0, time_constant = 0;
        if (argc < 6
            || JS_ToFloat64(context, &gain_left, argv[2]) < 0
            || JS_ToFloat64(context, &gain_right, argv[3]) < 0
            || JS_ToFloat64(context, &delay, argv[4]) < 0
            || JS_ToFloat64(context, &time_constant, argv[5]) < 0)
            return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_schedule_envelope_target(
                bridge->host->game_audio, handle, gain_left, gain_right,
                delay, time_constant)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_GAIN:
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_cancel_envelope(
                bridge->host->game_audio, handle) ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_CANCEL_PITCH:
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_cancel_pitch_curve(
                bridge->host->game_audio, handle) ? JS_TRUE : JS_FALSE;
    case TILEFINCH_GAME_AUDIO_COMMAND_CURVE: {
        int32_t pitch = 0;
        double left = 1, right = 1, delay = 0, duration = 0;
        if (argc < 8) return JS_FALSE;
        if (JS_ToInt32(context, &pitch, argv[2]) < 0
            || JS_ToFloat64(context, &left, argv[4]) < 0
            || JS_ToFloat64(context, &right, argv[5]) < 0
            || JS_ToFloat64(context, &delay, argv[6]) < 0
            || JS_ToFloat64(context, &duration, argv[7]) < 0)
            return JS_EXCEPTION;
        if (pitch != 0 && pitch != 1) return JS_FALSE;
        /* Coercion may close the context or detach the view. Collect bytes
           only afterward, copy them before releasing the backing buffer,
           and resolve the runtime-owned engine last. */
        size_t offset = 0, length = 0, element_size = 0, storage = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(
            context, argv[3], &offset, &length, &element_size);
        if (JS_IsException(buffer)) return JS_EXCEPTION;
        uint8_t *bytes = JS_GetArrayBuffer(context, &storage, buffer);
        float values[TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT];
        bool valid = bytes != NULL && element_size == sizeof(float)
            && offset <= storage && length <= storage - offset
            && length % sizeof(float) == 0u
            && length / sizeof(float) >= 2u
            && length / sizeof(float) <= TILEFINCH_GAME_AUDIO_CURVE_POINT_LIMIT;
        if (valid) memcpy(values, bytes + offset, length);
        JS_FreeValue(context, buffer);
        if (!valid) return JS_FALSE;
        for (size_t at = 0; at < length / sizeof(float); at++) {
            uint32_t bits = 0u;
            memcpy(&bits, &values[at], sizeof(bits));
            if ((bits & UINT32_C(0x7fffffff)) >= UINT32_C(0x7f800000))
                return JS_ThrowTypeError(context,
                    "Audio curve samples must be finite");
        }
        bridge = JS_GetContextOpaque(context);
        return bridge != NULL && bridge->host != NULL
            && tilefinch_game_audio_schedule_curve(
                bridge->host->game_audio, handle, pitch != 0, values,
                length / sizeof(float), left, right, delay, duration)
            ? JS_TRUE : JS_FALSE;
    }
    case TILEFINCH_GAME_AUDIO_COMMAND_START_BUFFER: {
        double offset = 0, duration = 0, rate = 1;
        double gain_left = 1, gain_right = 1;
        double loop_start = 0, loop_end = 0, delay = 0;
        int loop = 0;
        if (argc < 11
            || JS_ToFloat64(context, &offset, argv[2]) < 0
            || JS_ToFloat64(context, &duration, argv[3]) < 0
            || JS_ToFloat64(context, &rate, argv[4]) < 0
            || JS_ToFloat64(context, &gain_left, argv[5]) < 0
            || JS_ToFloat64(context, &gain_right, argv[6]) < 0
            || (loop = JS_ToBool(context, argv[7])) < 0
            || JS_ToFloat64(context, &loop_start, argv[8]) < 0
            || JS_ToFloat64(context, &loop_end, argv[9]) < 0
            || JS_ToFloat64(context, &delay, argv[10]) < 0) return JS_FALSE;
        bridge = JS_GetContextOpaque(context);
        if (bridge == NULL || bridge->host == NULL) return JS_FALSE;
        uint32_t voice = 0;
        if (!tilefinch_game_audio_start(
                bridge->host->game_audio, handle, offset, duration, rate,
                gain_left, gain_right, loop > 0, loop_start, loop_end, delay,
                &voice)) return JS_FALSE;
        return JS_NewUint32(context, voice);
    }
    default:
        return JS_FALSE;
    }
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
JSValue js_game_audio_command(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    if (!game_audio_native_measurement)
        return js_game_audio_command_impl(context, this_value, argc, argv);
    /* Observe an already-integer opcode without repeating author coercion.
       Slot 12 is unknown/non-integer input. Reentrant calls are counted but
       not timed twice; their time belongs to the outer native boundary. */
    _Static_assert(SCRIPT_GAME_AUDIO_NATIVE_COMMANDS
                       == TILEFINCH_GAME_AUDIO_COMMAND_COUNT + 1u,
                   "one metrics slot per command plus the unknown slot");
    unsigned slot = SCRIPT_GAME_AUDIO_NATIVE_COMMANDS - 1u;
    if (argc > 0 && JS_VALUE_GET_TAG(argv[0]) == JS_TAG_INT) {
        int command = JS_VALUE_GET_INT(argv[0]);
        if (command >= 0 && command < (int) slot) slot = (unsigned) command;
    }
    bool outer = game_audio_native_depth++ == 0;
    uint64_t before = outer ? tilefinch_platform_monotonic_time_us() : 0;
    JSValue result = js_game_audio_command_impl(context, this_value, argc, argv);
    uint64_t after = outer ? tilefinch_platform_monotonic_time_us() : 0;
    game_audio_native_depth--;
    if (outer) {
        game_audio_native_metrics.command_calls[slot]++;
        if (after >= before)
            game_audio_native_metrics.command_us[slot] += after - before;
    } else {
        game_audio_native_metrics.nested_calls++;
    }
    return result;
}
#endif
#endif /* TILEFINCH_GAME_AUDIO_REFERENCE */

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
            bridge_notify_node_state_retired(bridge, handle);
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
        bridge_reclaim_node_slots(bridge, node);
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
        bridge->node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
        return JS_NewInt32(context, 0);
    }
    lease++;
    if (lease == 0) lease = 1;
    bridge->node_wrapper_leases[slot] = lease;
    bridge->node_retention_flags[slot] |= BRIDGE_NODE_LIVE_WRAPPER;
    /* The wrapper cache's WeakRef for this lease, for native getters. */
    if (argc > 1 && JS_IsObject(argv[1])) {
        if (bridge->wrapper_refs == NULL) {
            bridge->wrapper_refs = budget_calloc(
                bridge->budget, bridge->node_capacity, sizeof(JSValue));
            if (bridge->wrapper_refs != NULL)
                for (size_t at = 0; at < bridge->node_capacity; at++)
                    bridge->wrapper_refs[at] = JS_UNDEFINED;
        }
        if (bridge->wrapper_refs != NULL) {
            bridge_wrapper_ref_clear(bridge, slot);
            bridge->wrapper_refs[slot] = JS_DupValue(context, argv[1]);
        }
    } else {
        bridge_wrapper_ref_clear(bridge, slot);
    }
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

/* Ends the live wrapper identity of `slot` and reclaims its detached
   lifetime tree unless something else retains it. A tree that may contain
   `keep` (a node native code is still using) is left for the wrapper's own
   finalizer. Returns the handle slots released. */
static size_t bridge_release_wrapper_slot(DomBridge *bridge, size_t slot,
                                          const lxb_dom_node_t *keep)
{
    lxb_dom_node_t *node = bridge->nodes[slot];
    bridge_wrapper_ref_clear(bridge, slot);
    bridge->node_retention_flags[slot] &=
        (unsigned char) ~BRIDGE_NODE_LIVE_WRAPPER;
    if ((bridge->node_retention_flags[slot] & BRIDGE_NODE_NATIVE_PIN) != 0
        /* Parentless fragments can still be owned by a template element,
           and Lexbor retains ordinary detached fragments in the document's
           arena. Keep both until document retirement instead of letting a
           JavaScript wrapper finalizer guess at native ownership. */
        || (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT
            && node->parent == NULL)
        || bridge_node_is_connected(node)) {
        if (bridge->result != NULL
            && bridge->result->dom_handle_connected_preserves != SIZE_MAX) {
            bridge->result->dom_handle_connected_preserves++;
        }
        return 0;
    }
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
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
    return bridge_release_wrapper_slot(bridge, slot, NULL) != 0
        ? JS_TRUE : JS_FALSE;
}

/* Handle slots are released by the wrapper cache's FinalizationRegistry,
   whose cleanup runs only as a job between tasks. One long synchronous
   script can therefore fill the table with wrappers that have died but not
   been finalized. When registration finds the table full, do the release
   those cleanup jobs would do for every wrapper whose WeakRef has cleared,
   collecting first when that alone frees too little (a wrapper in a cycle
   clears only at a collection). The queued cleanup job later finds a
   retired handle or an ended lease and does nothing. The sweep is linear in
   the table; a collection that frees little backs off exponentially within
   one entry into JavaScript, so a genuinely full table cannot turn every
   refused registration into a collection. */
#define BRIDGE_RECLAIM_ENOUGH(bridge) ((bridge)->node_capacity / 64u)
#define BRIDGE_RECLAIM_BACKOFF_MAX 1024u

static size_t bridge_release_dead_wrappers(DomBridge *bridge,
                                           const lxb_dom_node_t *keep)
{
    JSContext *context = bridge->host->context;
    size_t released = 0;
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
        if (!live) released += bridge_release_wrapper_slot(bridge, slot, keep);
    }
    return released;
}

static void bridge_reclaim_node_slots(DomBridge *bridge,
                                      const lxb_dom_node_t *keep)
{
    if (bridge->host == NULL || bridge->host->runtime == NULL
        || bridge->host->context == NULL || bridge->wrapper_refs == NULL
        || bridge->node_reclaim_active) return;
    bridge->node_reclaim_active = true;
    size_t released = bridge_release_dead_wrappers(bridge, keep);
    if (released < BRIDGE_RECLAIM_ENOUGH(bridge)) {
        if (bridge->node_reclaim_gc_skip != 0) {
            bridge->node_reclaim_gc_skip--;
        } else {
            /* As at every explicit full collection: one pass breaks a
               wrapper self-cycle and the next clears its WeakRef. */
            JS_RunGC(bridge->host->runtime);
            JS_RunGC(bridge->host->runtime);
            released += bridge_release_dead_wrappers(bridge, keep);
            if (released < BRIDGE_RECLAIM_ENOUGH(bridge)) {
                uint32_t backoff = bridge->node_reclaim_gc_backoff == 0
                    ? 8u : 2u * (uint32_t) bridge->node_reclaim_gc_backoff;
                if (backoff > BRIDGE_RECLAIM_BACKOFF_MAX)
                    backoff = BRIDGE_RECLAIM_BACKOFF_MAX;
                bridge->node_reclaim_gc_backoff = (uint16_t) backoff;
                bridge->node_reclaim_gc_skip = (uint16_t) backoff;
            }
        }
    }
    if (released >= BRIDGE_RECLAIM_ENOUGH(bridge)) {
        bridge->node_reclaim_gc_skip = 0;
        bridge->node_reclaim_gc_backoff = 0;
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

typedef struct {
    const DomBridge *bridge;
    unsigned char *retire_slots;
    /* The marked slot range, so retirement need not scan the table. */
    size_t first_slot;
    size_t last_slot;
} BridgeSubtreeHandleScan;

/* Marks each handle slot inside the subtree for retirement; stops at the
   first slot whose identity is still retained. */
static bool bridge_subtree_visit_handles(void *opaque, lxb_dom_node_t *node)
{
    BridgeSubtreeHandleScan *scan = opaque;
    size_t slot = bridge_node_index_find(scan->bridge, node);
    if (slot == SIZE_MAX) return false;
    if (scan->bridge->node_retention_flags[slot] != 0) return true;
    scan->retire_slots[slot / 8u] |= (unsigned char) (1u << (slot % 8u));
    if (slot < scan->first_slot) scan->first_slot = slot;
    if (slot > scan->last_slot) scan->last_slot = slot;
    return false;
}

static void bridge_adopted_sheet_unpin(DomBridge *bridge,
                                       lxb_dom_node_t *sheet);
static void bridge_mutation_journal_append(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    const uint32_t *changed_tokens, size_t changed_token_count,
    bool relational, uint64_t has_entries, uint32_t has_serial);

static size_t bridge_discard_unretained_detached_subtree(
    DomBridge *bridge, lxb_dom_node_t *root)
{
    if (bridge == NULL || root == NULL || root->parent != NULL) return 0;
    /* One walk finds every handle inside the subtree through the pointer
       index; any still-retained identity keeps the whole subtree. */
    unsigned char retire_slots[(DOM_BRIDGE_NODE_LIMIT_MAX + 7u) / 8u] = {0};
    BridgeSubtreeHandleScan handle_scan = {
        bridge, retire_slots, SIZE_MAX, 0
    };
    if (bridge_live_subtree_visit(
            root, 0, bridge_subtree_visit_handles, &handle_scan)
        != BRIDGE_SUBTREE_NOT_FOUND) return 0;
    unsigned char script_states[SCRIPT_DYNAMIC_NODE_LIMIT] = {0};
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(
                root, bridge->script_elements[i].node);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        script_states[i] = result == BRIDGE_SUBTREE_FOUND;
    }
    size_t mutation_count =
        bridge->mutations.count < SCRIPT_MUTATION_JOURNAL_LIMIT
            ? bridge->mutations.count : SCRIPT_MUTATION_JOURNAL_LIMIT;
    unsigned char mutation_slots[
        (SCRIPT_MUTATION_JOURNAL_LIMIT + 7u) / 8u] = {0};
    bool mutations_retired = false;
    unsigned char scope_slots[
        (SCRIPT_MUTATION_JOURNAL_LIMIT + 7u) / 8u] = {0};
    for (size_t i = 0; i < mutation_count; i++) {
        lxb_dom_node_t *scope = bridge->mutations.records[i].scope;
        if (scope == NULL) continue;
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(root, scope);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        if (result == BRIDGE_SUBTREE_FOUND) {
            scope_slots[i / 8u] |= (unsigned char) (1u << (i % 8u));
        }
    }
    for (size_t i = 0; i < mutation_count; i++) {
        lxb_dom_node_t *target = bridge->mutations.records[i].node;
        if (target == NULL) continue;
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(root, target);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        if (result == BRIDGE_SUBTREE_FOUND) {
            mutation_slots[i / 8u] |= (unsigned char) (1u << (i % 8u));
            mutations_retired = true;
        }
    }
    bool overflow_root_retired = false;
    for (size_t i = 0; i < bridge->mutations.overflow_root_count; i++) {
        BridgeSubtreeSearchResult result = bridge_live_subtree_contains_node(
            root, bridge->mutations.overflow_roots[i]);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        if (result == BRIDGE_SUBTREE_FOUND) overflow_root_retired = true;
    }
    BridgeSubtreeSearchResult declared_marker =
        bridge->document->declared_video_card_node == NULL
            ? BRIDGE_SUBTREE_NOT_FOUND
            : bridge_live_subtree_contains_node(
                  root, bridge->document->declared_video_card_node);
    BridgeSubtreeSearchResult reader_marker =
        bridge->document->reader_declared_video_card_node == NULL
            ? BRIDGE_SUBTREE_NOT_FOUND
            : bridge_live_subtree_contains_node(
                  root, bridge->document->reader_declared_video_card_node);
    if (declared_marker == BRIDGE_SUBTREE_INDETERMINATE
        || reader_marker == BRIDGE_SUBTREE_INDETERMINATE
        || !document_control_state_discard_subtree(
               bridge->document, root)) {
        return 0;
    }
    if (declared_marker == BRIDGE_SUBTREE_FOUND)
        bridge->document->declared_video_card_node = NULL;
    if (reader_marker == BRIDGE_SUBTREE_FOUND)
        bridge->document->reader_declared_video_card_node = NULL;
    /* A detached-node reclamation can retire a mutation target before the
       host consumes the journal. Preserve the conservative rebuild signal,
       but never let layout reuse dereference a retired raw pointer.

       Only the records this subtree actually owns may be cleared. Nulling
       the whole journal also raised `overflowed`, which the history runtime
       reads as a destructive tick and answers with a layout-reuse cache
       reset plus a full-document resource scan; since assigning to
       textContent discards one detached child at a time, every such
       assignment paid for both. */
    for (size_t i = 0; i < mutation_count; i++) {
        if ((scope_slots[i / 8u]
             & (unsigned char) (1u << (i % 8u))) == 0) continue;
        bridge->mutations.records[i].scope = NULL;
    }
    if (overflow_root_retired) {
        /* The pointer dies; so does the scoped overflow fallback. */
        bridge->mutations.overflow_root_count = 0;
        bridge->mutations.overflow_roots_lost = true;
    }
    if (mutations_retired) {
        for (size_t i = 0; i < mutation_count; i++) {
            if ((mutation_slots[i / 8u]
                 & (unsigned char) (1u << (i % 8u))) == 0) continue;
            bridge->mutations.records[i].node = NULL;
        }
        bridge->mutations.conservative_resource_scan = true;
        /* Without a retirement listener the host cannot know which cached
           node pointers die here; with one, the retired records are simply
           skipped and the surviving removal scopes carry the structural
           change. */
        if (bridge->node_retirement == NULL) {
            bridge->mutations.overflowed = true;
            bridge->mutations.retirement_unobserved = true;
        }
    }
    js_rt_script_element_states_purge_marked(bridge, script_states);
    size_t released = 0;
    for (size_t i = handle_scan.first_slot;
         i <= handle_scan.last_slot && i < bridge->node_count; i++) {
        if ((retire_slots[i / 8u]
             & (unsigned char) (1u << (i % 8u))) != 0) {
            /* A detached canvas context can be collected without an explicit
               context-loss call. Retire its bounded color/depth ownership
               while the native node is still valid; otherwise a stale node
               can occupy one of two WebGL depth slots until page teardown. */
            if (bridge->nodes[i] != NULL
                && bridge->nodes[i]->local_name == LXB_TAG_CANVAS
                && bridge->images != NULL && bridge->budget != NULL) {
                (void) images_release_canvas(
                    bridge->images, bridge->budget, bridge->nodes[i]);
            }
            (void) bridge_invalidate_node_slot_impl(bridge, i, false);
            released++;
        }
    }
    if (bridge->node_retirement != NULL) {
        bridge->node_retirement(bridge->node_retirement_opaque, root);
    }
    document_parser_insertions_discard_subtree(bridge->document, root);
    /* Adoption lists of shadow roots inside the subtree go with it; the
       sheets they alone adopted lose their pin below. */
    lxb_dom_node_t *unadopted[DOCUMENT_CONSTRUCTED_SHEET_LIMIT];
    size_t unadopted_count = document_adoptions_discard_subtree(
        bridge->document, root, unadopted, DOCUMENT_CONSTRUCTED_SHEET_LIMIT);
    /* Detached: no style can change, and the removal walk is wasted. */
    document_style_quiet_begin();
    lxb_dom_node_destroy_deep(root);
    document_style_quiet_end();
    for (size_t i = 0; i < unadopted_count; i++)
        bridge_adopted_sheet_unpin(bridge, unadopted[i]);
    /* Cleanup callbacks are JavaScript. Run them only after native teardown
       and handle retirement are complete, so author reentrancy cannot destroy
       the subtree a second time. */
    bridge_notify_pending_node_retirements(
        bridge, retire_slots, handle_scan.first_slot, handle_scan.last_slot);
    return released;
}

void bridge_release_native_node_pin(DomBridge *bridge, int64_t handle)
{
    size_t slot = 0;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)) return;
    lxb_dom_node_t *node = bridge->nodes[slot];
    bridge->node_retention_flags[slot] &=
        (unsigned char) ~BRIDGE_NODE_NATIVE_PIN;
    if (node == NULL || bridge_node_is_connected(node)) return;
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
    (void) bridge_discard_unretained_detached_subtree(bridge, root);
}

/* Constructed stylesheets (document.h, document_adopted_sheets_active).
   A sheet's text lives in a detached <style> element held by its
   CSSStyleSheet; while any adoptedStyleSheets list names it the bridge pins
   it, so a collected wrapper cannot free an element the cascade parses. */
static void bridge_adopted_sheet_unpin(DomBridge *bridge,
                                       lxb_dom_node_t *sheet)
{
    if (bridge == NULL || sheet == NULL
        || document_constructed_sheet_adopted(bridge->document, sheet))
        return;
    size_t slot = bridge_node_index_find(bridge, sheet);
    if (slot == SIZE_MAX) return;
    bridge_release_native_node_pin(bridge, bridge_node_handle(bridge, slot));
}

/* What adoption contributes to the cascade changed. Journaled as an
   author data attribute on `scope` (a shadow root's carrier, or the root
   element for the document's list and sheet text), which restyles nothing
   by itself, with the stylesheet flags set: navigation then inserts newly
   adopted sheets after the document's sources, restyles the roots whose
   scope moved, or rebuilds (navigation_try_append_style_sources). */
static void bridge_note_adopted_styles_changed(DomBridge *bridge,
                                               lxb_dom_node_t *scope)
{
    static const char attribute[] = "data-adopted-stylesheets";
    if (bridge == NULL || bridge->document == NULL
        || bridge->document->html == NULL) return;
    lxb_dom_element_t *root_element = lxb_dom_document_element(
        &bridge->document->html->dom_document);
    if (root_element == NULL) return;
    if (scope == NULL || !bridge_node_is_connected(scope))
        scope = lxb_dom_interface_node(root_element);
    bridge->dom_version++;
    document_note_connected_mutation(bridge->document);
    document_style_changed();
    bridge->computed_style_cache.dirty_all = true;
    if (bridge->result != NULL) {
        bridge->result->dom_mutations++;
        bridge->result->relayout_required = true;
    }
    if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;
    bridge_mutation_journal_append(
        bridge, SCRIPT_MUTATION_ATTRIBUTE, scope, attribute,
        sizeof(attribute) - 1u, NULL, 0, false, UINT64_MAX, 0);
    bridge->mutations.resource_rebuild_required = true;
    bridge->mutations.stylesheet_rebuild_required = true;
}

/* __tilefinchConstructedSheetText(handle, text): makes `text` the sheet
   element's only child and registers the sheet. False (nothing applied)
   when the element is not a detached <style> or a bound refuses it. */
JSValue js_dom_constructed_sheet_text(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 1
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || bridge->document == NULL
        || bridge->document->html == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_STYLE || node->ns != LXB_NS_HTML
        || node->parent != NULL) return JS_FALSE;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[1]);
    if (text == NULL) return JS_EXCEPTION;
    bool ok = document_constructed_sheet_text_fits(
        bridge->document, node, length);
    lxb_dom_text_t *replacement = !ok || length == 0 ? NULL
        : lxb_dom_document_create_text_node(
            &bridge->document->html->dom_document,
            (const lxb_char_t *) text, length);
    JS_FreeCString(context, text);
    if (length != 0 && replacement == NULL) ok = false;
    bool active = false;
    if (ok) ok = document_constructed_sheet_note_text(
        bridge->document, node, length, &active);
    if (!ok) {
        if (replacement != NULL)
            lxb_dom_node_destroy_deep(lxb_dom_interface_node(replacement));
        return JS_FALSE;
    }
    while (node->first_child != NULL) {
        lxb_dom_node_t *removed = node->first_child;
        document_style_quiet_begin();
        lxb_dom_node_remove(removed);
        document_style_quiet_end();
        (void) bridge_discard_unretained_detached_subtree(bridge, removed);
    }
    if (replacement != NULL) {
        document_style_quiet_begin();
        (void) lxb_dom_node_append_child(
            node, lxb_dom_interface_node(replacement));
        document_style_quiet_end();
    }
    if (active) bridge_note_adopted_styles_changed(bridge, NULL);
    return JS_TRUE;
}

/* __tilefinchCssStatementEnds(text): the UTF-16 end offsets of the complete
   top-level statements of `text`, split by the native stylesheet scanner
   (stylesheet_next_statement), then the number of blocks its unfinished
   tail leaves open. -1 instead of that tail count means a bounded batch of
   complete statements was returned; the caller resumes at its last end. */
JSValue js_css_statement_ends(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    size_t length = 0;
    const char *text = argc > 0
        ? JS_ToCStringLen(context, &length, argv[0])
        : NULL;
    if (text == NULL) return JS_EXCEPTION;
#ifndef __PSP__
    if (length > tilefinch_test_faults()->css_statement_max_source_bytes)
        tilefinch_test_faults()->css_statement_max_source_bytes = length;
#endif
    JSValue ends = JS_NewArray(context);
    size_t offset = 0, open_blocks = 0, mapped = 0;
    int64_t units = 0;
    uint32_t count = 0;
    bool more = true;
    /* This is a batch bound, not a stylesheet rule cap. Dense sheets must
       not allocate their whole offset table before the joined-text quota
       can stop them. Every batch is continued at a complete statement. */
    const uint32_t statement_batch = 4096u;
    while (!JS_IsException(ends) && more && count < statement_batch) {
        more = stylesheet_next_statement(text, length, &offset, &open_blocks);
        /* UTF-8 lead bytes start one UTF-16 unit, four-byte ones two. */
        for (; more && mapped < offset; mapped++) {
            unsigned char byte = (unsigned char) text[mapped];
            if ((byte & 0xc0u) != 0x80u) units += byte >= 0xf0u ? 2 : 1;
        }
        if (JS_SetPropertyUint32(
                context, ends, count++,
                JS_NewInt64(context,
                            more ? units : (int64_t) open_blocks)) < 0) {
            JS_FreeValue(context, ends);
            ends = JS_EXCEPTION;
        }
    }
    if (!JS_IsException(ends) && more && count == statement_batch
        && JS_SetPropertyUint32(context, ends, count, JS_NewInt32(context, -1)) < 0) {
        JS_FreeValue(context, ends);
        ends = JS_EXCEPTION;
    }
    JS_FreeCString(context, text);
    return ends;
}

/* __tilefinchFontShorthandValid(text): whether `text` parses as a CSS font
   shorthand value by the cascade's own parser (document.fonts.check and
   load use it), with the realm's stylesheet for viewport units when there
   is one. */
JSValue js_font_shorthand_valid(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t length = 0;
    const char *text = argc > 0
        ? JS_ToCStringLen(context, &length, argv[0])
        : NULL;
    if (text == NULL) return JS_EXCEPTION;
    ComputedStyle font = {0};
    bool valid = style_parse_font_shorthand(
        bridge == NULL ? NULL : (const Stylesheet *) bridge->stylesheet,
        text, length, &font);
    JS_FreeCString(context, text);
    return JS_NewBool(context, valid);
}

/* __tilefinchSetAdoptedSheets(rootHandle, [sheetHandles]): rootHandle 0
   is the document, otherwise a shadow root's carrier. Every sheet must have
   been given text first. False (nothing changes) when a bound refuses. */
JSValue js_dom_set_adopted_sheets(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL || argc < 2)
        return JS_FALSE;
    int64_t root_handle = 0;
    if (JS_ToInt64(context, &root_handle, argv[0]) < 0) return JS_EXCEPTION;
    lxb_dom_node_t *root = NULL;
    if (root_handle != 0) {
        root = js_rt_bridge_node_arg(context, bridge, argv[0]);
        if (root == NULL) return JS_FALSE;
    }
    int64_t length = 0;
    JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
    if (JS_IsException(length_value)) return JS_EXCEPTION;
    int status = JS_ToInt64(context, &length, length_value);
    JS_FreeValue(context, length_value);
    if (status < 0) return JS_EXCEPTION;
    if (length < 0 || length > (int64_t) DOCUMENT_ADOPTED_SHEETS_PER_ROOT)
        return JS_FALSE;
    lxb_dom_node_t *sheets[DOCUMENT_ADOPTED_SHEETS_PER_ROOT];
    for (int64_t i = 0; i < length; i++) {
        JSValue item = JS_GetPropertyUint32(context, argv[1], (uint32_t) i);
        if (JS_IsException(item)) return JS_EXCEPTION;
        sheets[i] = js_rt_bridge_node_arg(context, bridge, item);
        JS_FreeValue(context, item);
        if (sheets[i] == NULL
            || !document_constructed_sheet_known(bridge->document, sheets[i]))
            return JS_FALSE;
    }
    uint64_t before = document_adopted_sheets_signature(bridge->document);
    lxb_dom_node_t *released[DOCUMENT_ADOPTED_SHEETS_PER_ROOT];
    size_t released_count = 0;
    if (!document_adoption_set(bridge->document, root, sheets,
                               (size_t) length, released, &released_count))
        return JS_FALSE;
    for (int64_t i = 0; i < length; i++) {
        size_t slot = bridge_node_index_find(bridge, sheets[i]);
        if (slot != SIZE_MAX)
            bridge->node_retention_flags[slot] |= BRIDGE_NODE_NATIVE_PIN;
    }
    if (document_adopted_sheets_signature(bridge->document) != before)
        bridge_note_adopted_styles_changed(bridge, root);
    for (size_t i = 0; i < released_count; i++)
        bridge_adopted_sheet_unpin(bridge, released[i]);
    return JS_TRUE;
}

static void bridge_detach_and_discard_children(
    DomBridge *bridge, lxb_dom_node_t *parent)
{
    if (bridge == NULL || parent == NULL) return;
    bool trace = tilefinch_trace_script_failures();
    while (parent->first_child != NULL) {
        lxb_dom_node_t *child = parent->first_child;
        unsigned child_type = (unsigned) child->type;
        char child_name[32] = {0};
        if (trace && child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t name_length = 0;
            const char *name = document_element_name(child, &name_length);
            if (name != NULL) {
                if (name_length >= sizeof(child_name)) {
                    name_length = sizeof(child_name) - 1;
                }
                memcpy(child_name, name, name_length);
            }
        }
        /* Published by the caller's INNER_HTML note. */
        document_style_quiet_begin();
        lxb_dom_node_remove(child);
        document_style_quiet_end();
        /* A JavaScript-held descendant keeps the complete detached subtree
           alive, including otherwise-unwrapped ancestors.  That preserves
           parent/sibling relationships and lets the subtree be reinserted.
           Unobserved trees are reclaimed immediately instead of waiting for
           a later QuickJS collection cycle. */
        size_t released =
            bridge_discard_unretained_detached_subtree(bridge, child);
        if (trace) {
            fprintf(stderr,
                    "dom-detach-child type=%u name=\"%s\" retained=%s "
                    "released-handles=%zu\n",
                    child_type, child_name,
                    released == 0 ? "yes" : "no", released);
        }
    }
}

enum { BRIDGE_ORDINAL_WALK_DEPTH_LIMIT = 48 };

/* Script can nest the DOM arbitrarily deep (js_dom_append imposes no cap),
   so these ordinal walks bound their own recursion the way the subtree walks
   below already do.  Exceeding the bound reports "not found", which callers
   already treat as an absent stable key. */
static lxb_dom_node_t *bridge_element_at_ordinal(lxb_dom_node_t *node,
                                                 size_t wanted,
                                                 size_t *ordinal,
                                                 size_t depth)
{
    if (depth >= BRIDGE_ORDINAL_WALK_DEPTH_LIMIT) return NULL;
    for (; node != NULL; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (*ordinal == wanted) return node;
            (*ordinal)++;
        }
        lxb_dom_node_t *found = bridge_element_at_ordinal(
            node->first_child, wanted, ordinal, depth + 1);
        if (found != NULL) return found;
    }
    return NULL;
}

static lxb_dom_node_t *bridge_node_at_ordinal(lxb_dom_node_t *node,
                                              size_t wanted,
                                              size_t *ordinal,
                                              size_t depth)
{
    if (depth >= BRIDGE_ORDINAL_WALK_DEPTH_LIMIT) return NULL;
    for (; node != NULL; node = node->next) {
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            if (*ordinal == wanted) return node;
            (*ordinal)++;
        }
        lxb_dom_node_t *found = bridge_node_at_ordinal(
            node->first_child, wanted, ordinal, depth + 1);
        if (found != NULL) return found;
    }
    return NULL;
}

/* Resolve both ordinal domains in one bounded walk. The compressed section
   store indexes immutable source, so it cannot answer this for a live DOM
   after script insertions/removals. Query traversals use these cursors once
   at their starting node and then advance them without further root walks. */
static bool bridge_ordinals_walk(lxb_dom_node_t *node,
                                 lxb_dom_node_t *target,
                                 size_t *node_ordinal,
                                 size_t *element_ordinal,
                                 size_t *target_node_ordinal,
                                 size_t *target_element_ordinal,
                                 size_t *target_depth,
                                 size_t depth)
{
    if (depth >= BRIDGE_ORDINAL_WALK_DEPTH_LIMIT) return false;
    for (; node != NULL; node = node->next) {
        size_t current_node_ordinal = SIZE_MAX;
        size_t current_element_ordinal = SIZE_MAX;
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            current_node_ordinal = (*node_ordinal)++;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            current_element_ordinal = (*element_ordinal)++;
        }
        if (node == target) {
            *target_node_ordinal = current_node_ordinal;
            *target_element_ordinal = current_element_ordinal;
            if (target_depth != NULL) *target_depth = depth;
            return true;
        }
        if (bridge_ordinals_walk(
                node->first_child, target, node_ordinal, element_ordinal,
                target_node_ordinal, target_element_ordinal,
                target_depth, depth + 1)) return true;
    }
    return false;
}

static bool bridge_document_order_traversal_init(
    DomBridge *bridge, DomDocumentOrderTraversal *traversal,
    lxb_dom_node_t *next, const lxb_dom_node_t *boundary)
{
    if (traversal == NULL) return false;
    *traversal = (DomDocumentOrderTraversal) {
        .next = next,
        .boundary = boundary,
        .current_node_ordinal = SIZE_MAX,
        .current_element_ordinal = SIZE_MAX
    };
    if (bridge == NULL || bridge->node_visibility == NULL || next == NULL) {
        return true;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    size_t node_ordinal = 0, element_ordinal = 0;
    size_t target_node_ordinal = SIZE_MAX;
    size_t target_element_ordinal = SIZE_MAX;
    size_t target_depth = 0;
    if (!bridge_ordinals_walk(
            root, next, &node_ordinal, &element_ordinal,
            &target_node_ordinal, &target_element_ordinal,
            &target_depth, 0)) return false;
    traversal->next_node_ordinal = target_node_ordinal != SIZE_MAX
        ? target_node_ordinal : node_ordinal;
    traversal->next_element_ordinal = target_element_ordinal != SIZE_MAX
        ? target_element_ordinal : element_ordinal;
    traversal->next_depth = target_depth;
    traversal->track_ordinals = true;
    return true;
}

static bool bridge_node_visible(DomBridge *bridge, lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL
        || bridge->node_visibility == NULL) return true;
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    size_t node_ordinal = 0, element_ordinal = 0;
    size_t target_node_ordinal = SIZE_MAX;
    size_t target_element_ordinal = SIZE_MAX;
    if (!bridge_ordinals_walk(
            root, node, &node_ordinal, &element_ordinal,
            &target_node_ordinal, &target_element_ordinal, NULL, 0)) {
        return false;
    }
    return bridge->node_visibility(
        bridge->node_visibility_opaque, bridge->section_identity,
        target_element_ordinal, target_node_ordinal, (unsigned) node->type);
}

static bool bridge_traversal_node_visible(
    DomBridge *bridge, lxb_dom_node_t *node,
    const DomDocumentOrderTraversal *traversal)
{
    if (bridge == NULL || node == NULL
        || bridge->node_visibility == NULL) return true;
    if (traversal == NULL || !traversal->track_ordinals) {
        return bridge_node_visible(bridge, node);
    }
    if (traversal->current_depth >= BRIDGE_ORDINAL_WALK_DEPTH_LIMIT) {
        return false;
    }
    return bridge->node_visibility(
        bridge->node_visibility_opaque, bridge->section_identity,
        traversal->current_element_ordinal,
        traversal->current_node_ordinal, (unsigned) node->type);
}

JSValue js_stable_node_key(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || node == NULL) return JS_NULL;
    size_t node_ordinal = 0, element_ordinal = 0, name_length = 0;
    size_t target_node_ordinal = SIZE_MAX;
    size_t target_element_ordinal = SIZE_MAX;
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    if (!bridge_ordinals_walk(
            root, node, &node_ordinal, &element_ordinal,
            &target_node_ordinal, &target_element_ordinal, NULL, 0)) {
        return JS_NULL;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        if (bridge->remote_node_read == NULL
            || target_node_ordinal == SIZE_MAX) return JS_NULL;
        char key[96];
        int written = snprintf(key, sizeof(key), "n:%zu:%zu:%u",
                               bridge->section_identity,
                               target_node_ordinal,
                               (unsigned) node->type);
        return written > 0 && (size_t) written < sizeof(key)
            ? JS_NewStringLen(context, key, (size_t) written) : JS_NULL;
    }
    const char *name = document_element_name(node, &name_length);
    if (name == NULL || name_length == 0 || name_length >= 32
        || target_element_ordinal == SIZE_MAX) return JS_NULL;
    if (bridge->remote_node_read != NULL && name_length == 4) {
        if (strncasecmp(name, "html", 4) == 0) {
            return JS_NewString(context, "d:html");
        }
        if (strncasecmp(name, "head", 4) == 0) {
            return JS_NewString(context, "d:head");
        }
        if (strncasecmp(name, "body", 4) == 0) {
            return JS_NewString(context, "d:body");
        }
    }
    char key[96];
    int written = snprintf(key, sizeof(key), "s:%zu:%zu:%.*s",
                           bridge->section_identity,
                           target_element_ordinal,
                           (int) name_length, name);
    return written > 0 && (size_t) written < sizeof(key)
        ? JS_NewStringLen(context, key, (size_t) written) : JS_NULL;
}

JSValue js_section_identity(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewInt64(context, bridge == NULL
                       ? 0 : (int64_t) bridge->section_identity);
}

JSValue js_find_stable_node(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *key = JS_ToCStringLen(context, &length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    size_t section = 0, wanted = 0;
    unsigned wanted_type = 0;
    char tag_name[32] = {0};
    int consumed = 0;
    bool element_key = sscanf(key, "s:%zu:%zu:%31s%n", &section, &wanted,
                              tag_name, &consumed) == 3
        && consumed >= 0 && (size_t) consumed == length
        && section == bridge->section_identity;
    consumed = 0;
    bool node_key = !element_key
        && sscanf(key, "n:%zu:%zu:%u%n", &section, &wanted,
                  &wanted_type, &consumed) == 3
        && consumed >= 0 && (size_t) consumed == length
        && section == bridge->section_identity;
    JS_FreeCString(context, key);
    if (!element_key && !node_key) return JS_NewInt64(context, 0);
    if (node_key) {
        size_t ordinal = 0;
        lxb_dom_node_t *node = bridge_node_at_ordinal(
            lxb_dom_interface_node(bridge->document->html), wanted,
            &ordinal, 0);
        if (node == NULL || (unsigned) node->type != wanted_type) node = NULL;
        return bridge_node_handle_value(context, bridge, node);
    }
    size_t ordinal = 0, name_length = 0;
    lxb_dom_node_t *node = bridge_element_at_ordinal(
        lxb_dom_interface_node(bridge->document->html), wanted, &ordinal, 0);
    const char *name = document_element_name(node, &name_length);
    if (name == NULL || strlen(tag_name) != name_length
        || strncasecmp(name, tag_name, name_length) != 0) node = NULL;
    return bridge_node_handle_value(context, bridge, node);
}

static bool selector_list_matches(lxb_dom_node_t *node,
                                  const char *selector, size_t length,
                                  const lxb_dom_node_t *scope)
{
    size_t start = 0;
    int square = 0, round = 0;
    char quote = 0;
    bool escaped = false;
    for (size_t i = 0; i <= length; i++) {
        char value = i < length ? selector[i] : ',';
        if (quote != 0) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == quote) quote = 0;
            continue;
        }
        if (i < length && escaped) {
            escaped = false;
            continue;
        }
        if (i < length && value == '\\') {
            escaped = true;
            continue;
        }
        if (value == '\'' || value == '"') {
            quote = value;
            continue;
        }
        if (value == '[') square++;
        else if (value == ']' && square > 0) square--;
        else if (value == '(') round++;
        else if (value == ')' && round > 0) round--;
        if (value == ',' && square == 0 && round == 0) {
            size_t first = start, last = i;
            while (first < last
                   && isspace((unsigned char) selector[first])) first++;
            while (last > first
                   && isspace((unsigned char) selector[last - 1])) last--;
            if (last > first
                && style_selector_matches_scoped(
                    node, selector + first, last - first, scope)) return true;
            start = i + 1;
        }
    }
    return false;
}

/* A query's selector list, prepared once for the whole walk. */
typedef struct {
    StyleQuerySelectorList list;
    bool prepared;
    const char *text;
    size_t length;
} BridgeQuerySelector;

static void bridge_query_selector_prepare(BridgeQuerySelector *query,
                                          const char *text, size_t length)
{
    query->text = text;
    query->length = length;
    query->prepared =
        style_query_selector_list_prepare(&query->list, text, length);
}

static bool bridge_query_selector_matches(const BridgeQuerySelector *query,
                                          lxb_dom_node_t *node,
                                          const lxb_dom_node_t *scope)
{
    return query->prepared
        ? style_query_selector_list_matches(&query->list, node, scope)
        : selector_list_matches(node, query->text, query->length, scope);
}

/* Pre-order traversal using DOM parent/sibling links instead of a PSP stack
   frame per nesting level. `boundary` is included when it is also `next`,
   but traversal never escapes through that node's following sibling. */
lxb_dom_node_t *dom_document_order_next(
    DomDocumentOrderTraversal *traversal)
{
    if (traversal == NULL || traversal->next == NULL
        || traversal->visited >= DOM_TRAVERSAL_VISIT_LIMIT) return NULL;
    lxb_dom_node_t *node = traversal->next;
    traversal->visited++;
    traversal->current_depth = traversal->next_depth;
    traversal->current_node_ordinal = SIZE_MAX;
    traversal->current_element_ordinal = SIZE_MAX;
    if (traversal->track_ordinals) {
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            traversal->current_node_ordinal =
                traversal->next_node_ordinal++;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            traversal->current_element_ordinal =
                traversal->next_element_ordinal++;
        }
    }
    if (node->first_child != NULL) {
        traversal->next = node->first_child;
        traversal->next_depth = traversal->current_depth + 1u;
        return node;
    }
    lxb_dom_node_t *at = node;
    size_t climbed = 0;
    while (at != NULL && at != traversal->boundary
           && at->next == NULL
           && climbed++ < DOM_TRAVERSAL_VISIT_LIMIT) {
        at = at->parent;
    }
    traversal->next = at == NULL || at == traversal->boundary
        || climbed >= DOM_TRAVERSAL_VISIT_LIMIT ? NULL : at->next;
    traversal->next_depth = climbed > traversal->current_depth
        ? 0 : traversal->current_depth - climbed;
    return node;
}

/* innerHTML marks every parsed script "already started", so it never runs.
   Range.createContextualFragment instead clears that flag and the parser
   document: its scripts are script-inserted (programmatic, not force-async)
   and run through the ordinary insertion path, CSP included, when the
   fragment is connected. Those must all get native state, or one would be
   silently inert, so a capacity failure there rolls back and reports. */
static bool script_element_states_register_parsed_subtree(
    DomBridge *bridge, lxb_dom_node_t *container, bool unstarted)
{
    if (bridge == NULL || container == NULL) return false;
    size_t initial_count = bridge->script_element_count;
    DomDocumentOrderTraversal traversal = {
        .next = container->first_child, .boundary = container
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        size_t name_length = 0;
        const char *name = document_element_name(at, &name_length);
        if (name == NULL || name_length != 6
            || strncasecmp(name, "script", 6) != 0) continue;
        ScriptElementState *state = js_rt_script_element_state_register(
            bridge, at, !unstarted || at->ns == LXB_NS_HTML);
        if (state != NULL) {
            state->programmatic = unstarted;
            state->force_async = false;
            state->already_started = !unstarted;
        } else if (unstarted) {
            memset(bridge->script_elements + initial_count, 0,
                   (bridge->script_element_count - initial_count)
                       * sizeof(bridge->script_elements[0]));
            bridge->script_element_count = initial_count;
            return false;
        }
    }
    return true;
}

/* HTML's script cloning steps copy only "already started": a clone of a
   script that ran (or failed, or was refused) never runs, and a clone of
   one that never started (template contents, a non-script type, no
   source yet) runs once when inserted. Everything else is a new element's:
   no parser document (script-inserted) and force-async set.

   Every cloned script element gets native state, including clones of the
   host parser's scripts, which have none: a script without state is what
   the document pipeline's discovery waves execute as newly inserted, which
   ran parser-script clones a second time. */
static bool script_element_states_clone_pass(
    DomBridge *bridge, lxb_dom_node_t *source, lxb_dom_node_t *clone,
    bool deep, size_t *started, size_t *unstarted)
{
    size_t initial_count = bridge->script_element_count;
    DomDocumentOrderTraversal source_traversal = {
        .next = source, .boundary = source
    };
    DomDocumentOrderTraversal clone_traversal = {
        .next = clone, .boundary = clone
    };
    for (;;) {
        lxb_dom_node_t *source_node = dom_document_order_next(
            &source_traversal);
        lxb_dom_node_t *clone_node = dom_document_order_next(
            &clone_traversal);
        if (source_node == NULL || clone_node == NULL) break;
        size_t name_length = 0;
        const char *name = document_element_name(source_node, &name_length);
        ScriptElementState *source_state = js_rt_script_element_state_find(
            bridge, source_node);
        if (source_state != NULL
            || (name != NULL && name_length == 6
                && strncasecmp(name, "script", 6) == 0)) {
            bool already_started = source_state != NULL
                ? source_state->already_started
                : js_rt_script_element_parser_started(source_node);
            bool html = source_state != NULL ? source_state->html
                                             : source_node->ns == LXB_NS_HTML;
            ScriptElementState *clone_state =
                js_rt_script_element_state_register(bridge, clone_node, html);
            if (clone_state == NULL) {
                memset(bridge->script_elements + initial_count, 0,
                       (bridge->script_element_count - initial_count)
                           * sizeof(bridge->script_elements[0]));
                bridge->script_element_count = initial_count;
                return false;
            }
            clone_state->programmatic = true;
            clone_state->force_async = html;
            clone_state->already_started = already_started;
#if TILEFINCH_SCRIPT_CLONE_CENSUS
            clone_state->census_started_clone = already_started;
#endif
            if (already_started) (*started)++;
            else (*unstarted)++;
        }
        if (!deep) break;
    }
    return true;
}

/* The table is bounded and its entries are released only when a dropped
   clone's wrapper is reclaimed, which a long synchronous script (a slider
   cloning slides in a loop) never reaches on its own. A full table first
   reclaims dead wrappers, as handle registration does; when the subtree
   still cannot be recorded, the clone is refused and reported rather than
   produced with a script that could run again. */
static bool script_element_states_clone_subtree(
    DomBridge *bridge, lxb_dom_node_t *source, lxb_dom_node_t *clone,
    bool deep)
{
    if (bridge == NULL || source == NULL || clone == NULL) return false;
    size_t started = 0, unstarted = 0;
    bool recorded = script_element_states_clone_pass(
        bridge, source, clone, deep, &started, &unstarted);
    if (!recorded) {
        bridge_reclaim_node_slots(bridge, source);
        started = unstarted = 0;
        recorded = script_element_states_clone_pass(
            bridge, source, clone, deep, &started, &unstarted);
    }
#if TILEFINCH_SCRIPT_CLONE_CENSUS
    if (bridge->result != NULL) {
        js_rt_saturating_add_size(
            recorded ? &bridge->result->script_clones_started
                     : &bridge->result->script_clones_refused,
            recorded ? started : 1);
        if (recorded) js_rt_saturating_add_size(
            &bridge->result->script_clones_unstarted, unstarted);
    }
#else
    (void) started;
    (void) unstarted;
#endif
    return recorded;
}

static lxb_dom_node_t *selector_query(
    DomBridge *bridge, lxb_dom_node_t *node,
    const lxb_dom_node_t *boundary, const char *selector, size_t length)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, boundary)) return NULL;
    size_t shadow_depth = SIZE_MAX;
    BridgeQuerySelector query;
    bridge_query_selector_prepare(&query, selector, length);
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, boundary, &traversal, &shadow_depth)) continue;
        if (bridge_query_selector_matches(&query, at, boundary)
            && bridge_traversal_node_visible(
                bridge, at, &traversal)) return at;
    }
    return NULL;
}

static bool bridge_mutation_name_equal(const char *name, size_t length,
                                       const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (name == NULL || length != wanted_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) name[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static bool bridge_mutation_node_name_is(lxb_dom_node_t *node,
                                         const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return bridge_mutation_name_equal(name, length, wanted);
}

static bool bridge_mutation_inside_svg(lxb_dom_node_t *node)
{
    size_t depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL && depth++ < 64;
         at = at->parent) {
        if (bridge_mutation_node_name_is(at, "svg")) return true;
    }
    return false;
}

static bool bridge_mutation_subtree_contains_image(
    const DomBridge *bridge, lxb_dom_node_t *root)
{
    enum { MAXIMUM_WORK = 256, MAXIMUM_DEPTH = 48 };
    if (root == NULL) return false;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    for (;;) {
        /* Bounded-out is resource-sensitive: a conservative refresh is
           preferable to retaining a stale raster below a large subtree. */
        if (++work > MAXIMUM_WORK) return true;
        if (bridge_mutation_node_name_is(node, "svg")
            || (bridge_mutation_node_name_is(node, "img")
                && bridge->images != NULL
                && images_find_node(bridge->images, node) == NULL
                /* A formerly visible missing image is not a reveal. Its
                   explicit src mutation/retry owns discovery. */
                && (bridge->layout == NULL
                    || layout_box_for_node(bridge->layout, node) == NULL))) return true;
        if (node->first_child != NULL && depth < MAXIMUM_DEPTH) {
            node = node->first_child;
            depth++;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            if (depth != 0) depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    return false;
}

/* Only a document holding inline SVG rasters has a presentation-only raster
   to go stale; without one, colour and custom-property churn refreshes
   nothing and must not pay for an image refresh. */
static bool bridge_mutation_has_inline_svg_rasters(const DomBridge *bridge)
{
    return bridge->images != NULL
        && (bridge->images->stats.inline_svg_rasterized != 0
            || bridge->images->stats.inline_svg_refresh_reused != 0);
}

/* Inline-style properties that can change what an SVG beneath resolves for
   its raster (currentColor, em sizes, inherited presentation values, a
   custom property one of those reads). */
static bool bridge_mutation_style_property_affects_svg_raster(
    const char *name, size_t length)
{
    if (name == NULL) return false;
    if (length > 2 && name[0] == '-' && name[1] == '-') return true;
    return bridge_mutation_name_equal(name, length, "color")
        || bridge_mutation_name_equal(name, length, "font")
        || bridge_mutation_name_equal(name, length, "font-size")
        || (length >= 4 && strncasecmp(name, "fill", 4) == 0)
        || (length >= 6 && strncasecmp(name, "stroke", 6) == 0);
}

static bool bridge_mutation_inside_style(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (bridge_mutation_node_name_is(at, "style")) return true;
    }
    return false;
}

typedef enum {
    BRIDGE_MUTATION_RESOURCE_NONE = 0,
    BRIDGE_MUTATION_RESOURCE_IMAGE = 1u << 0,
    BRIDGE_MUTATION_RESOURCE_STYLESHEET = 1u << 1,
    BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT = 1u << 2,
    /* Not a fetched resource: a <meta http-equiv=refresh>, which navigation
       must consider for the document's declarative refresh. */
    BRIDGE_MUTATION_RESOURCE_REFRESH_META = 1u << 3
} BridgeMutationResourceFlags;

/* An HTML <meta> whose http-equiv is "refresh" (ASCII case-insensitive,
   untrimmed, as the enumerated attribute is matched). */
static bool bridge_mutation_link_is_style_resource(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *rel = document_attribute(node, "rel", &length);
    if (rel == NULL) return false;
    bool preload = false;
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) rel[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) rel[at])) at++;
        if (at - start == sizeof("stylesheet") - 1
            && strncasecmp(rel + start, "stylesheet",
                           sizeof("stylesheet") - 1) == 0) return true;
        if (at - start == sizeof("preload") - 1
            && strncasecmp(rel + start, "preload",
                           sizeof("preload") - 1) == 0) preload = true;
    }
    const char *as = document_attribute(node, "as", &length);
    return preload && as != NULL && length == 5u
        && strncasecmp(as, "style", 5u) == 0;
}

static BridgeMutationResourceFlags bridge_mutation_resource_attribute(
    lxb_dom_node_t *node, const char *name, size_t length)
{
    if (node == NULL || name == NULL) return BRIDGE_MUTATION_RESOURCE_NONE;
    if (bridge_mutation_node_name_is(node, "img")) {
        return (bridge_mutation_name_equal(name, length, "src")
            || bridge_mutation_name_equal(name, length, "srcset")
            || bridge_mutation_name_equal(name, length, "sizes")
            || bridge_mutation_name_equal(name, length, "data-src")
            || bridge_mutation_name_equal(name, length, "data-srcset"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "source")) {
        return (bridge_mutation_name_equal(name, length, "srcset")
            || bridge_mutation_name_equal(name, length, "data-srcset")
            || bridge_mutation_name_equal(name, length, "media")
            || bridge_mutation_name_equal(name, length, "type"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "video")) {
        return bridge_mutation_name_equal(name, length, "poster")
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "link")) {
        /* A rel transition may remove the sheet which was active before the
           mutation, so it cannot be classified from the new value alone. */
        if (bridge_mutation_name_equal(name, length, "rel")
            || bridge_mutation_name_equal(name, length, "as")) {
            return BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        return bridge_mutation_link_is_style_resource(node)
               && (bridge_mutation_name_equal(name, length, "href")
                   || bridge_mutation_name_equal(name, length, "media")
                   || bridge_mutation_name_equal(name, length, "crossorigin")
                   || bridge_mutation_name_equal(name, length, "integrity")
                   || bridge_mutation_name_equal(name, length, "disabled"))
            ? BRIDGE_MUTATION_RESOURCE_STYLESHEET
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "base")) {
        return bridge_mutation_name_equal(name, length, "href")
            ? BRIDGE_MUTATION_RESOURCE_STYLESHEET
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    if (bridge_mutation_node_name_is(node, "use")) {
        return (bridge_mutation_name_equal(name, length, "href")
                || bridge_mutation_name_equal(name, length, "xlink:href"))
            ? BRIDGE_MUTATION_RESOURCE_IMAGE
            : BRIDGE_MUTATION_RESOURCE_NONE;
    }
    return BRIDGE_MUTATION_RESOURCE_NONE;
}

static bool bridge_mutation_inline_style_has_resource(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *style = document_attribute(node, "style", &length);
    if (style == NULL || length < 4) return false;
    /* The renderer's only externally fetched inline-style value is a CSS
       image URL.  A var() can resolve to one through either the declaration
       itself or a custom property consumed by an image declaration, so it is
       conservatively resource-sensitive too.  Match function names folded
       and tolerate whitespace before the opening parenthesis.  Removing the
       last resource value does not need a rebuild: the next layout reads the
       live declaration and any now-unused resource remains safely owned until
       teardown. */
    for (size_t at = 0; at + 3 < length; at++) {
        bool url = tolower((unsigned char) style[at]) == 'u'
            && tolower((unsigned char) style[at + 1]) == 'r'
            && tolower((unsigned char) style[at + 2]) == 'l';
        bool variable = tolower((unsigned char) style[at]) == 'v'
            && tolower((unsigned char) style[at + 1]) == 'a'
            && tolower((unsigned char) style[at + 2]) == 'r';
        if (!url && !variable) continue;
        size_t after = at + 3;
        while (after < length
               && isspace((unsigned char) style[after])) after++;
        if (after < length && style[after] == '(') return true;
    }
    return false;
}

static bool bridge_mutation_style_property_is_resource(
    const char *name, size_t length)
{
    if (name == NULL) return true;
    return (length >= 2 && name[0] == '-' && name[1] == '-')
        || bridge_mutation_name_equal(name, length, "background")
        || bridge_mutation_name_equal(name, length, "background-image")
        || bridge_mutation_name_equal(name, length, "mask")
        || bridge_mutation_name_equal(name, length, "mask-image");
}

static BridgeMutationResourceFlags bridge_mutation_resource_subtree(
    const PocDocument *document, lxb_dom_node_t *root)
{
    /* DOM parent links let this be a stackless depth-first walk.  The caps
       prevent hostile author trees from turning one bridge call into an
       unbounded PSP stack/work boundary; bounded-out means "unknown", never
       "clean".  The root's siblings are intentionally outside the subtree. */
    enum { MAXIMUM_SUBTREE_WORK = 256, MAXIMUM_SUBTREE_DEPTH = 48 };
    if (root == NULL) return BRIDGE_MUTATION_RESOURCE_NONE;
    bool adopters = document_adoption_shadow_roots_present(document);
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    BridgeMutationResourceFlags result = BRIDGE_MUTATION_RESOURCE_NONE;
    for (;;) {
        if (++work > MAXIMUM_SUBTREE_WORK) {
            return result | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT;
        }
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (bridge_mutation_name_equal(tag, tag_length, "style")) {
            result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        if (document_is_refresh_meta(node)) {
            result |= BRIDGE_MUTATION_RESOURCE_REFRESH_META;
        }
        /* A shadow root's adopted sheets enter or leave the cascade with
           its carrier. */
        if (adopters && node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_adoption_root_has_sheets(document, node)) {
            result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            bool link = bridge_mutation_name_equal(tag, tag_length, "link");
            bool stylesheet_link =
                link && bridge_mutation_link_is_style_resource(node);
            if (stylesheet_link) {
                result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
            }
            /* bridge_mutation_resource_attribute() classifies attributes of
               these elements only; on any other element every lookup below
               would be answered "none". */
            bool resource_element = link
                || bridge_mutation_name_equal(tag, tag_length, "img")
                || bridge_mutation_name_equal(tag, tag_length, "source")
                || bridge_mutation_name_equal(tag, tag_length, "video")
                || bridge_mutation_name_equal(tag, tag_length, "base")
                || bridge_mutation_name_equal(tag, tag_length, "use");
            static const char *const attributes[] = {
                "src", "srcset", "data-src", "data-srcset", "href",
                "xlink:href", "rel", "media", "type", "sizes", "poster"
            };
            for (size_t i = 0; resource_element
                 && i < sizeof(attributes) / sizeof(attributes[0]); i++) {
                /* A connected rel mutation is destructive because its old
                   value may have removed a stylesheet. For an inserted
                   subtree the final rel is fully known; only stylesheets
                   and style preloads belong to this resource lane. */
                if (!stylesheet_link && link
                    && bridge_mutation_name_equal(
                        attributes[i], strlen(attributes[i]), "rel")) {
                    continue;
                }
                size_t length = 0;
                (void) document_attribute(node, attributes[i], &length);
                if (length != 0) {
                    result |= bridge_mutation_resource_attribute(
                        node, attributes[i], strlen(attributes[i]));
                }
            }
            if (bridge_mutation_inline_style_has_resource(node)) {
                result |= BRIDGE_MUTATION_RESOURCE_IMAGE;
            }
        }
        if (node->first_child != NULL) {
            if (++depth > MAXIMUM_SUBTREE_DEPTH) {
                return result | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT;
            }
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    return result;
}

static bool bridge_mutation_record_equal(
    const ScriptMutationRecord *record, ScriptMutationKind kind,
    const lxb_dom_node_t *node, const char *attribute,
    size_t attribute_length)
{
    if (record == NULL || record->kind != kind || record->node != node) {
        return false;
    }
    size_t stored_length = strlen(record->attribute);
    if (attribute == NULL) return stored_length == 0;
    /* Renderer records retain a conservative dependency prefix, not the
       author-visible attribute name. Compare the same bounded prefix we
       store: otherwise every write to a long name occupies another slot.
       Resource flags are still accumulated from every full-name mutation. */
    if (attribute_length >= sizeof(record->attribute))
        attribute_length = sizeof(record->attribute) - 1u;
    if (stored_length != attribute_length) return false;
    for (size_t i = 0; i < attribute_length; i++) {
        if (record->attribute[i]
            != (char) tolower((unsigned char) attribute[i])) return false;
    }
    return true;
}

static bool bridge_node_within(const lxb_dom_node_t *node,
                               const lxb_dom_node_t *scope);

/* Adds `root` to the overflow roots, merged past the limit into the
   deepest ancestor it shares with one of them. */
static void bridge_mutation_overflow_add(ScriptMutationJournal *journal,
                                         lxb_dom_node_t *root)
{
    for (unsigned merges = 0; merges <= SCRIPT_MUTATION_OVERFLOW_ROOT_LIMIT;
         merges++) {
        for (size_t i = 0; i < journal->overflow_root_count; i++)
            if (bridge_node_within(root, journal->overflow_roots[i])) return;
        /* Roots the new one contains go. */
        size_t kept = 0;
        for (size_t i = 0; i < journal->overflow_root_count; i++)
            if (!bridge_node_within(journal->overflow_roots[i], root))
                journal->overflow_roots[kept++] = journal->overflow_roots[i];
        journal->overflow_root_count = (uint8_t) kept;
        if (kept < SCRIPT_MUTATION_OVERFLOW_ROOT_LIMIT) {
            journal->overflow_roots[journal->overflow_root_count++] = root;
            return;
        }
        size_t best = 0, best_depth = 0;
        lxb_dom_node_t *best_ancestor = NULL;
        for (size_t i = 0; i < kept; i++) {
            lxb_dom_node_t *common = journal->overflow_roots[i];
            while (common != NULL && !bridge_node_within(root, common))
                common = common->parent;
            size_t depth = 0;
            for (lxb_dom_node_t *at = common; at != NULL; at = at->parent)
                depth++;
            if (common != NULL && common->type == LXB_DOM_NODE_TYPE_ELEMENT
                && (best_ancestor == NULL || depth > best_depth)) {
                best = i;
                best_depth = depth;
                best_ancestor = common;
            }
        }
        if (best_ancestor == NULL) break;
        journal->overflow_roots[best] =
            journal->overflow_roots[--journal->overflow_root_count];
        root = best_ancestor;
    }
    journal->overflow_roots_lost = true;
}

/* Where a change the full journal cannot record is contained: the
   subtree of the element's parent (sibling and :has() reach included),
   or for a tree or text change of its parent's parent (the parent's own
   emptiness and position tests too). */
static void bridge_mutation_overflow_root(ScriptMutationJournal *journal,
                                          ScriptMutationKind kind,
                                          lxb_dom_node_t *node)
{
    if (journal->overflow_roots_lost) return;
    lxb_dom_node_t *root = node == NULL ? NULL : node->parent;
    if (root != NULL && (kind == SCRIPT_MUTATION_CHILD_LIST
                         || kind == SCRIPT_MUTATION_HEAD_SCRIPT
                         || kind == SCRIPT_MUTATION_TEXT))
        root = root->parent;
    if (root != NULL && root->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        /* The change is at the top: the document element's subtree. */
        root = NULL;
        for (lxb_dom_node_t *at = node; at != NULL; at = at->parent)
            if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                && at->parent != NULL
                && at->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) root = at;
    }
    if (root == NULL || !bridge_node_is_connected(root)) {
        journal->overflow_roots_lost = true;
        return;
    }
    bridge_mutation_overflow_add(journal, root);
}

static void bridge_mutation_journal_append(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    const uint32_t *changed_tokens, size_t changed_token_count,
    bool relational, uint64_t has_entries, uint32_t has_serial)
{
    ScriptMutationJournal *journal = &bridge->mutations;
    if (has_serial != 0) {
        if (journal->has_serial == 0) journal->has_serial = has_serial;
        else if (journal->has_serial != has_serial)
            journal->has_serial_mixed = true;
    }
    for (size_t reverse = journal->count; reverse != 0; reverse--) {
        ScriptMutationRecord *record = &journal->records[reverse - 1];
        if (bridge_mutation_record_equal(
                record, kind, node, attribute, attribute_length)) {
            record->relational |= relational;
            record->has_entries |= has_serial == 0 ? UINT64_MAX : has_entries;
            /* Coalescing the node/attribute must retain every dependency
               changed during this turn, not just the first write's tokens. */
            if (changed_tokens == NULL
                || changed_token_count > SCRIPT_MUTATION_TOKEN_LIMIT) {
                record->changed_tokens_exact = false;
            } else if (record->changed_tokens_exact) {
                size_t count = record->changed_token_count;
                const uint32_t *run = journal->tokens
                    + record->changed_token_offset;
                size_t added = 0;
                uint32_t fresh[SCRIPT_MUTATION_TOKEN_LIMIT];
                for (size_t i = 0; i < changed_token_count; i++) {
                    bool present = false;
                    for (size_t at = 0; at < count && !present; at++)
                        present = run[at] == changed_tokens[i];
                    for (size_t at = 0; at < added && !present; at++)
                        present = fresh[at] == changed_tokens[i];
                    if (!present) fresh[added++] = changed_tokens[i];
                }
                if (added != 0) {
                    bool at_end = (size_t) record->changed_token_offset
                        + count == journal->token_count;
                    size_t start = at_end ? record->changed_token_offset
                                          : journal->token_count;
                    if (count + added > SCRIPT_MUTATION_TOKEN_LIMIT
                        || start + count + added
                               > SCRIPT_MUTATION_TOKEN_POOL) {
                        record->changed_tokens_exact = false;
                    } else {
                        /* A run left behind by relocation stays unused
                           until the journal is consumed. */
                        if (!at_end)
                            memmove(journal->tokens + start, run,
                                    count * sizeof(*run));
                        memcpy(journal->tokens + start + count, fresh,
                               added * sizeof(*fresh));
                        record->changed_token_offset = (uint16_t) start;
                        record->changed_token_count =
                            (uint8_t) (count + added);
                        journal->token_count = start + count + added;
                    }
                }
            }
            if (!record->changed_tokens_exact) record->changed_token_count = 0;
            return;
        }
    }
    if (journal->count >= SCRIPT_MUTATION_JOURNAL_LIMIT) {
        journal->overflowed = true;
        bridge_mutation_overflow_root(journal, kind, node);
        /* Records beyond the fixed journal cannot be independently audited,
           so retain the old whole-document fingerprint oracle. */
        journal->conservative_resource_scan = true;
        return;
    }
    ScriptMutationRecord *record = &journal->records[journal->count++];
    record->kind = kind;
    record->node = node;
    record->relational = relational;
    record->has_entries = has_serial == 0 ? UINT64_MAX : has_entries;
    record->inserted_from_detached = false;
    record->scope = NULL;
    record->removed_last = false;
    record->owner_document_identity = js_rt_node_owner_identity(node);
    size_t copy_length = attribute == NULL ? 0 : attribute_length;
    if (copy_length >= sizeof(record->attribute)) {
        copy_length = sizeof(record->attribute) - 1;
    }
    for (size_t i = 0; i < copy_length; i++) {
        record->attribute[i] = (char) tolower(
            (unsigned char) attribute[i]);
    }
    record->attribute[copy_length] = '\0';
    record->changed_tokens_exact = changed_tokens != NULL
        && changed_token_count <= SCRIPT_MUTATION_TOKEN_LIMIT
        && journal->token_count + changed_token_count
               <= SCRIPT_MUTATION_TOKEN_POOL;
    record->changed_token_count = 0;
    record->changed_token_offset = (uint16_t) journal->token_count;
    if (record->changed_tokens_exact) {
        record->changed_token_count = (uint8_t) changed_token_count;
        memcpy(journal->tokens + journal->token_count, changed_tokens,
               changed_token_count * sizeof(*changed_tokens));
        journal->token_count += changed_token_count;
    }
}

static void bridge_computed_style_note_mutation(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length, bool relational,
    const uint32_t *changed_tokens, size_t changed_token_count);

/* Whether a mutation can change which form owns a control, or whether an
   element is a listed control at all (form.elements): tree changes, and the
   id, form and type attributes; an unnamed or internal (custom-element
   state) attribute stays conservative. Class, style and the rest cannot. */
static bool bridge_mutation_moves_form_controls(ScriptMutationKind kind,
                                                const char *attribute,
                                                size_t length)
{
    if (kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_CANVAS) return false;
    if (kind != SCRIPT_MUTATION_ATTRIBUTE || attribute == NULL) return true;
    return (length == 2 && strncasecmp(attribute, "id", 2) == 0)
        || (length == 4 && (strncasecmp(attribute, "form", 4) == 0
                            || strncasecmp(attribute, "type", 4) == 0))
        || (length > 14 && strncasecmp(attribute, "data-tilefinch", 14) == 0);
}

static void bridge_mutated_summarized(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count, uint64_t has_entries, uint32_t has_serial);

static void bridge_mutated_with_relational(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count)
{
    bridge_mutated_summarized(bridge, kind, node, attribute,
                              attribute_length, relational_selector_sensitive,
                              changed_tokens, changed_token_count,
                              UINT64_MAX, 0);
}

/* has_entries/has_serial: the :has() entries a tree or text change can
   move (stylesheet_tree_change_has_entries), kept on its record. */
static void bridge_mutated_summarized(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive, const uint32_t *changed_tokens,
    size_t changed_token_count, uint64_t has_entries, uint32_t has_serial)
{
    if (bridge == NULL) return;
    /* Every script mutation, connected or not, invalidates DOM-derived
       caches kept in the realm (attribute lists). */
    bridge->dom_version++;
    if (bridge_mutation_moves_form_controls(kind, attribute,
                                            attribute_length))
        bridge->dom_structure_version++;
    /* Mutating a detached construction tree cannot affect layout or the
       connected resource graph. Its eventual insertion is represented by
       one child-list record over the complete subtree. Besides avoiding
       wasted work, this keeps a framework assembling one card from
       overflowing the PSP's fixed mutation journal before it is appended. */
    if (node != NULL && !bridge_node_is_connected(node)) return;
    /* A declaration miss is valid only for the DOM generation that was
       scanned. Author mutation may hydrate metadata or a playable element;
       native card insertion does not pass through this bridge and therefore
       keeps its already-bound selection. */
    media_declared_video_cache_destroy(bridge->document);
    document_note_connected_mutation(bridge->document);
    bridge_computed_style_note_mutation(bridge, kind, node, attribute,
                                        attribute_length,
                                        relational_selector_sensitive,
                                        changed_tokens, changed_token_count);
    /* Attribute and tree mutations can change which connected <base href> is
       first. Invalidating conservatively keeps the mutation fast and avoids a
       second subtree walk on the PSP hot path. */
    bridge->document_base_dirty = true;
    if (bridge->result != NULL) {
        bridge->result->dom_mutations++;
        bridge->result->relayout_required = true;
    }
    /* An attribute, inline-style or canvas change leaves every document
       statistic but the attribute totals intact, so it need not cost a
       whole-document refresh. While the parser is still inserting nodes,
       though, that refresh is also what brings the parser's growth into the
       statistics the streaming checkpoints compare, so keep it until the
       tree is complete. */
    bool attribute_only = kind == SCRIPT_MUTATION_ATTRIBUTE
        || kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_CANVAS;
    if (attribute_only)
        document_note_attribute_mutation(
            bridge->document, attribute, attribute_length);
    if (!attribute_only || bridge->document == NULL
        || bridge->document->html == NULL
        || bridge->document->html->ready_state
               != LXB_HTML_DOCUMENT_READY_STATE_COMPLETE)
        bridge->stats_mutations++;
    if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;

    ScriptMutationJournal *journal = &bridge->mutations;
    bridge_mutation_journal_append(
        bridge, kind, node, attribute, attribute_length, changed_tokens,
        changed_token_count, relational_selector_sensitive, has_entries,
        has_serial);

    bool resource_rebuild = false;
    /* resource_rebuild's causes: a stylesheet source and/or an image. */
    bool stylesheet_cause = false;
    bool image_rebuild = false;
    bool image_resource_scan = false;
    bool image_resource_refresh = false;
    bool conservative_scan = false;
    /* A connected <meta http-equiv=refresh> may have arrived (or a subtree
       too large to inspect did). */
    bool refresh_meta = false;
    switch (kind) {
    case SCRIPT_MUTATION_TEXT:
        if (node == NULL) conservative_scan = true;
        else resource_rebuild = bridge_mutation_inside_style(node);
        stylesheet_cause = resource_rebuild;
        break;
    case SCRIPT_MUTATION_INLINE_STYLE:
        /* The prior declaration may have owned an image even when the new
           one does not. Keep only image-bearing properties destructive;
           ordinary geometry/color declarations are layout-only. */
        resource_rebuild = bridge_mutation_style_property_is_resource(
            attribute, attribute_length);
        image_rebuild = resource_rebuild;
        /* A custom property never changes the stylesheet: at most it lets
           a var() image declaration resolve to a new URL. Discover that
           additively, and only when such a declaration can exist (the
           sheet uses var() in one, or this element's own inline style may);
           a no-longer-used image stays owned until teardown. */
        if (resource_rebuild && attribute != NULL && attribute_length >= 2u
            && attribute[0] == '-' && attribute[1] == '-') {
            resource_rebuild = false;
            image_resource_scan = (bridge->stylesheet != NULL
                    && bridge->stylesheet->image_declarations_use_variables)
                || bridge_mutation_inline_style_has_resource(node);
            break;
        }
        /* Visibility changes can expose a background URL which the initial
           image walk correctly skipped under display:none/hidden.  Re-scan
           only when this element's final inline declaration actually owns a
           resource, keeping ordinary animation/style churn off the resource
           path while ensuring a script-revealed hero is discovered. */
        image_resource_scan = !resource_rebuild
            && bridge_mutation_inline_style_has_resource(node)
            && (bridge_mutation_name_equal(
                    attribute, attribute_length, "display")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "visibility")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "opacity"));
        break;
    case SCRIPT_MUTATION_ATTRIBUTE:
    {
        BridgeMutationResourceFlags flags =
            bridge_mutation_resource_attribute(
                node, attribute, attribute_length);
        /* Setting content or http-equiv on a connected meta processes it
           again, as browsers do for an element already in the document. */
        refresh_meta = node != NULL && node->local_name == LXB_TAG_META
            && node->ns == LXB_NS_HTML
            && (bridge_mutation_name_equal(
                    attribute, attribute_length, "content")
                || bridge_mutation_name_equal(
                    attribute, attribute_length, "http-equiv"));
        /* A style attribute is an inline declaration block, not a sheet:
           a URL (or var() that may resolve to one) in the new value needs
           additive image discovery, never a stylesheet rebuild. */
        bool style_attribute = bridge_mutation_name_equal(
            attribute, attribute_length, "style")
            && !bridge_mutation_node_name_is(node, "style");
        if (style_attribute)
            image_resource_scan =
                bridge_mutation_inline_style_has_resource(node);
        resource_rebuild = bridge_mutation_node_name_is(node, "style")
            || (flags & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
        stylesheet_cause = resource_rebuild;
        if ((flags & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0) {
            /* An icon swap (<use href> inside an inline <svg>) changes only
               that SVG's raster, which the refresh lane re-resolves from its
               root, sprite fetch included; outside an <svg> a <use> renders
               nothing. Neither needs the sheet or other images rebuilt. */
            if (bridge_mutation_node_name_is(node, "img")
                || bridge_mutation_node_name_is(node, "use")) {
                image_resource_refresh = true;
            } else {
                resource_rebuild = true;
                image_rebuild = true;
            }
        }
        break;
    }
    case SCRIPT_MUTATION_INNER_HTML:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(bridge->document, node);
        resource_rebuild = node != NULL
            && (bridge_mutation_inside_style(node)
                || (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                               | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0);
        image_rebuild = node != NULL
            && (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
        stylesheet_cause = node != NULL
            && (bridge_mutation_inside_style(node)
                || (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0);
        /* innerHTML may have removed the last resource, which cannot be
           inferred by walking only the replacement subtree. */
        conservative_scan = !resource_rebuild
            || (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        refresh_meta = (subtree & (BRIDGE_MUTATION_RESOURCE_REFRESH_META
                                   | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT))
            != 0;
        break;
    }
    case SCRIPT_MUTATION_CHILD_LIST:
    case SCRIPT_MUTATION_HEAD_SCRIPT:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(bridge->document, node);
        bool connected = node != NULL && bridge_node_is_connected(node);
        if (!connected) {
            /* A removal can invalidate retained decoded surfaces and source
               order, so it remains destructive when resources are present. */
            resource_rebuild =
                (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                            | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0;
            image_rebuild = (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
            stylesheet_cause =
                (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
        } else {
            resource_rebuild =
                (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
            stylesheet_cause = resource_rebuild;
            image_resource_scan =
                (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
        }
        conservative_scan =
            (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        refresh_meta = connected
            && (subtree & (BRIDGE_MUTATION_RESOURCE_REFRESH_META
                           | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT)) != 0;
        break;
    }
    case SCRIPT_MUTATION_CANVAS:
        /* The resource already exists by the time this record is emitted.
           Creation/resizing still takes the ordinary fast-layout path;
           same-geometry publication is eligible for paint-only damage. */
        break;
    case SCRIPT_MUTATION_UNKNOWN:
    default:
        conservative_scan = true;
        break;
    }
    /* A class/id change whose tokens no rule able to affect display,
       visibility or an image depends on cannot reveal or hide an image.
       With inline SVG rasters present it must also leave what those SVGs
       resolve alone: a change inside one always refreshes it, one above
       one only when a colour, font-size, size, presentation or
       custom-property rule depends on its tokens (checked last, after the
       bounded subtree walk has found an SVG to refresh). */
    bool svg_rasters = node != NULL
        && bridge_mutation_has_inline_svg_rasters(bridge);
    bool identity_bounded = kind == SCRIPT_MUTATION_ATTRIBUTE
        && changed_tokens != NULL && bridge->stylesheet != NULL
#ifndef TILEFINCH_NO_TRACE
        && getenv("TILEFINCH_DISABLE_DISCOVERY_GATE") == NULL
#endif
        && (bridge_mutation_name_equal(attribute, attribute_length, "class")
            || bridge_mutation_name_equal(attribute, attribute_length, "id"))
        && !stylesheet_tokens_may_affect_discovery(
               bridge->stylesheet, changed_tokens, changed_token_count)
        && !(svg_rasters
             && (bridge_mutation_inside_svg(node)
                 || (bridge_mutation_subtree_contains_image(bridge, node)
                     && stylesheet_tokens_may_affect_svg_raster(
                            bridge->stylesheet, changed_tokens,
                            changed_token_count))));
    bool descendant_image_sensitive =
        node != NULL && !identity_bounded
        && (bridge_mutation_inside_svg(node)
            || ((kind == SCRIPT_MUTATION_ATTRIBUTE
                 || kind == SCRIPT_MUTATION_INLINE_STYLE)
                && (bridge_mutation_name_equal(
                        attribute, attribute_length, "class")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "id")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "style")
                    || (kind == SCRIPT_MUTATION_ATTRIBUTE
                        && bridge_mutation_name_equal(
                               attribute, attribute_length, "color"))
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "hidden")
                    || (kind == SCRIPT_MUTATION_INLINE_STYLE
                        && (bridge_mutation_name_equal(attribute, attribute_length, "display")
                            || bridge_mutation_name_equal(attribute, attribute_length, "visibility")
                            || bridge_mutation_name_equal(attribute, attribute_length, "opacity")
                            || (svg_rasters
                                && bridge_mutation_style_property_affects_svg_raster(
                                       attribute, attribute_length)))))
               && bridge_mutation_subtree_contains_image(bridge, node)));
    if (refresh_meta) bridge->refresh_meta_mutated = true;
    journal->resource_rebuild_required |= resource_rebuild;
    journal->image_rebuild_required |= resource_rebuild && image_rebuild;
    journal->stylesheet_rebuild_required |= resource_rebuild && stylesheet_cause;
    journal->image_resource_scan_required |= image_resource_scan;
    journal->image_resource_refresh_required |=
        image_resource_refresh || descendant_image_sensitive;
    journal->conservative_resource_scan |= conservative_scan;
    bool focus_marker = kind == SCRIPT_MUTATION_ATTRIBUTE
        && attribute != NULL
        && attribute_length == sizeof("data-tilefinch-focus") - 1
        && strncasecmp(
            attribute, "data-tilefinch-focus",
            sizeof("data-tilefinch-focus") - 1) == 0;
    /* Focus has a dedicated ancestor-aware invalidator. Keep this aggregate
       for unrelated records in the same author turn. */
    if (!focus_marker) {
        journal->relational_selector_sensitive |=
            relational_selector_sensitive;
    }
    if ((resource_rebuild || image_resource_scan || image_resource_refresh
         || conservative_scan)
        && tilefinch_trace_mutation_policy()) {
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        fprintf(stderr,
                "tilefinch: mutation-policy resource kind=%d class=%s "
                "node=%.*s "
                "attribute=%.*s rebuild=%d image-scan=%d "
                "image-refresh=%d conservative=%d\n",
                (int) kind,
                resource_rebuild ? "rebuild"
                : (image_resource_scan ? "image-add" : "image-refresh"),
                (int) name_length, name == NULL ? "" : name,
                (int) attribute_length,
                attribute == NULL ? "" : attribute,
                resource_rebuild, image_resource_scan,
                image_resource_refresh, conservative_scan);
    }
}

void js_rt_bridge_note_canvas_mutation(DomBridge *bridge,
                                       lxb_dom_node_t *node,
                                       bool paint_only)
{
    static const char paint[] = "paint";
    static const char structure[] = "surface";
    if (paint_only) {
        /* Replacing pixels in an already-sized canvas changes neither the
           connected DOM nor document-derived title/body/statistics. Publish
           bounded paint damage without making js_rt_runtime_refresh() walk
           and re-summarize the complete document on every animation frame. */
        if (bridge == NULL || node == NULL
            || !bridge_node_is_connected(node)) return;
        if (bridge->result != NULL) bridge->result->relayout_required = true;
        if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;
        bridge_mutation_journal_append(
            bridge, SCRIPT_MUTATION_CANVAS, node,
            paint, sizeof(paint) - 1u, NULL, 0, false, UINT64_MAX, 0);
        return;
    }
    bridge_mutated_with_relational(
        bridge, SCRIPT_MUTATION_CANVAS, node,
        structure, sizeof(structure) - 1u,
        false, NULL, 0);
}

static void bridge_mutated(DomBridge *bridge, ScriptMutationKind kind,
                           lxb_dom_node_t *node,
                           const char *attribute,
                           size_t attribute_length)
{
    /* Callers without exact pre-mutation state remain conservative. A child
       inserted (after the fact) or about to be removed, and a text node's
       data, are classified against the sheet's :has() rules instead; a
       text write that replaced an element's children cannot be. */
    bool relational = true;
    uint64_t has_entries = UINT64_MAX;
    uint32_t has_serial = 0;
    if (bridge != NULL && node != NULL && node->parent != NULL
        && (kind == SCRIPT_MUTATION_CHILD_LIST
            || kind == SCRIPT_MUTATION_HEAD_SCRIPT
            || (kind == SCRIPT_MUTATION_TEXT
                && node->type != LXB_DOM_NODE_TYPE_ELEMENT)))
        relational = stylesheet_tree_change_has_entries(
            bridge->stylesheet, node, node->parent, &has_entries,
            &has_serial);
    bridge_mutated_summarized(
        bridge, kind, node, attribute, attribute_length, relational, NULL, 0,
        has_entries, has_serial);
}

JSValue js_dom_body(JSContext *context, JSValueConst this_value,
                    int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *body = document_body_node(bridge->document);
    if (body != NULL && bridge->node_visibility != NULL
        && !bridge->node_visibility(
               bridge->node_visibility_opaque,
               SCRIPT_NODE_VISIBILITY_DOCUMENT_BODY_SECTION,
               0, 0, LXB_DOM_NODE_TYPE_ELEMENT)) body = NULL;
    return bridge_node_handle_value(context, bridge, body);
}

JSValue js_dom_document_element(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    lxb_dom_node_t *html = selector_query(
        bridge, root, root, "html", 4);
    return bridge_node_handle_value(context, bridge, html);
}

JSValue js_dom_query(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *selector = JS_ToCStringLen(context, &length, argv[0]);
    if (selector == NULL) return JS_EXCEPTION;
    lxb_dom_node_t *scope = argc > 1
        ? js_rt_bridge_node_arg(context, bridge, argv[1]) : NULL;
    lxb_dom_node_t *root = scope == NULL
        ? lxb_dom_interface_node(bridge->document->html)
        : scope->first_child;
    const lxb_dom_node_t *boundary = scope == NULL ? root : scope;
    lxb_dom_node_t *found = length <= 512
                            ? selector_query(bridge, root, boundary,
                                             selector, length)
                            : NULL;
    JS_FreeCString(context, selector);
    return bridge_node_handle_value(context, bridge, found);
}

/* Results one querySelectorAll or descendant walk may return. Every result
   holds its own node handle, so the handle table is the real bound: a walk
   needing more handles than the table can give throws the handle-exhausted
   error instead of returning a short list. One past the largest table keeps
   the walk going until that failing registration rather than truncating
   silently (a fixed cap of 128, later 4,096, cut whole-document walks
   short on large pages). */
#define DOM_QUERY_RESULT_LIMIT (SCRIPT_DOM_HANDLE_SLOT_CAPACITY_MAX + 1u)

/* False when a match could not be given a handle. */
static bool query_all_nodes(DomBridge *bridge, lxb_dom_node_t *node,
                            const lxb_dom_node_t *boundary,
                            const char *selector, size_t length,
                            JSContext *context, JSValue array,
                            uint32_t *count, uint32_t result_limit)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, boundary)) return true;
    size_t shadow_depth = SIZE_MAX;
    BridgeQuerySelector query;
    bridge_query_selector_prepare(&query, selector, length);
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < result_limit;
         at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, boundary, &traversal, &shadow_depth)) continue;
        if (bridge_query_selector_matches(&query, at, boundary)
            && bridge_traversal_node_visible(
                bridge, at, &traversal)) {
            int64_t handle = js_rt_bridge_register_node(bridge, at);
            if (handle == 0) return false;
            (void) JS_SetPropertyUint32(
                context, array, (*count)++, JS_NewInt64(context, handle));
        }
    }
    return true;
}

JSValue js_dom_query_all(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSValue array = JS_NewArray(context);
    if (argc < 1 || JS_IsException(array)) return array;
    size_t length = 0;
    const char *selector = JS_ToCStringLen(context, &length, argv[0]);
    if (selector == NULL) { JS_FreeValue(context, array); return JS_EXCEPTION; }
    lxb_dom_node_t *scope = argc > 1
        ? js_rt_bridge_node_arg(context, bridge, argv[1]) : NULL;
    lxb_dom_node_t *root = scope == NULL
        ? lxb_dom_interface_node(bridge->document->html)
        : scope->first_child;
    const lxb_dom_node_t *boundary = scope == NULL ? root : scope;
    int64_t requested = DOM_QUERY_RESULT_LIMIT;
    if (argc > 2 && JS_ToInt64(context, &requested, argv[2]) < 0) {
        JS_FreeCString(context, selector);
        JS_FreeValue(context, array);
        return JS_EXCEPTION;
    }
    uint32_t result_limit = requested <= 0 ? 0
        : (requested > DOM_QUERY_RESULT_LIMIT
               ? DOM_QUERY_RESULT_LIMIT : (uint32_t) requested);
    uint32_t count = 0;
    bool complete = length > 512 || result_limit == 0
        || query_all_nodes(bridge, root, boundary, selector, length,
                           context, array, &count, result_limit);
    JS_FreeCString(context, selector);
    if (!complete) {
        JS_FreeValue(context, array);
        return bridge_throw_handles_exhausted(context);
    }
    return array;
}

static void query_count_nodes(DomBridge *bridge, lxb_dom_node_t *node,
                              const lxb_dom_node_t *boundary,
                              const char *selector,
                              size_t length, size_t limit, size_t *count)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, boundary)) return;
    size_t shadow_depth = SIZE_MAX;
    BridgeQuerySelector query;
    bridge_query_selector_prepare(&query, selector, length);
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < limit;
         at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, boundary, &traversal, &shadow_depth)) continue;
        if (bridge_query_selector_matches(&query, at, boundary)
            && bridge_traversal_node_visible(
                bridge, at, &traversal)) (*count)++;
    }
}

JSValue js_dom_query_count(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NewInt32(context, 0);
    size_t length = 0;
    const char *selector = JS_ToCStringLen(context, &length, argv[0]);
    if (selector == NULL) return JS_EXCEPTION;
    int64_t requested = 129;
    if (argc > 1 && JS_ToInt64(context, &requested, argv[1]) < 0) {
        JS_FreeCString(context, selector);
        return JS_EXCEPTION;
    }
    size_t limit = requested <= 0 ? 0
        : (requested > 4096 ? 4096 : (size_t) requested);
    size_t count = 0;
    if (length <= 512 && limit != 0) {
        lxb_dom_node_t *root = lxb_dom_interface_node(
            bridge->document->html);
        query_count_nodes(bridge, root, root, selector, length,
                          limit, &count);
    }
    JS_FreeCString(context, selector);
    return JS_NewInt64(context, (int64_t) count);
}

JSValue js_dom_matches(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || argc < 2) return JS_FALSE;
    size_t length = 0;
    const char *selector = JS_ToCStringLen(context, &length, argv[1]);
    if (selector == NULL) return JS_EXCEPTION;
    bool matches = length <= 512
        && selector_list_matches(node, selector, length, node);
    JS_FreeCString(context, selector);
    return JS_NewBool(context, matches);
}

/* Element.closest() without a wrapper per ancestor: the element, then its
   element ancestors as parentElement reaches them (stopping at the document,
   a fragment, a shadow-root carrier, or a hidden parent), with the selector
   prepared once and each candidate as its own :scope, as matches() does.
   Returns the first match's handle; 0 for none; or minus the handle of a
   parentless subtree root, where script continues through a JavaScript-side
   detached parent if that root has one. */
#define DOM_CLOSEST_ANCESTOR_LIMIT 256u
JSValue js_dom_closest(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || argc < 2 || node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *selector = JS_ToCStringLen(context, &length, argv[1]);
    if (selector == NULL) return JS_EXCEPTION;
    int64_t found = 0;
    bool registered = false;
    if (length <= 512) {
        BridgeQuerySelector query;
        bridge_query_selector_prepare(&query, selector, length);
        lxb_dom_node_t *at = node;
        for (unsigned steps = 0; steps < DOM_CLOSEST_ANCESTOR_LIMIT;
             steps++) {
            if (bridge_query_selector_matches(&query, at, at)) {
                found = js_rt_bridge_register_node(bridge, at);
                registered = true;
                break;
            }
            lxb_dom_node_t *parent = at->parent;
            if (parent == NULL) {
                found = -js_rt_bridge_register_node(bridge, at);
                registered = true;
                break;
            }
            if (parent->type != LXB_DOM_NODE_TYPE_ELEMENT
                || bridge_node_is_shadow_root(bridge, parent)
                || !bridge_node_visible(bridge, parent)) break;
            at = parent;
        }
    }
    JS_FreeCString(context, selector);
    if (registered && found == 0)
        return bridge_throw_handles_exhausted(context);
    return JS_NewInt64(context, found);
}

static bool js_values_strict_equal(JSContext *context,
                                   JSValueConst left, JSValueConst right)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    return JS_StrictEq(context, left, right);
#else
    return JS_IsStrictEqual(context, left, right);
#endif
}

static int64_t dom_method_scope_handle(JSContext *context,
                                       JSValueConst this_value)
{
    if (!JS_IsObject(this_value)) {
        JS_ThrowTypeError(context, "DOM query called on incompatible receiver");
        return -1;
    }
    JSValue handle_value = JS_GetPropertyStr(context, this_value, "__handle");
    if (JS_IsException(handle_value)) return -1;
    if (!JS_IsUndefined(handle_value)) {
        int64_t handle = 0;
        int converted = JS_ToInt64(context, &handle, handle_value);
        JS_FreeValue(context, handle_value);
        if (converted < 0 || handle <= 0) {
            if (converted >= 0)
                JS_ThrowTypeError(context,
                                  "DOM query called on incompatible receiver");
            return -1;
        }
        return handle;
    }
    JS_FreeValue(context, handle_value);

    JSValue global = JS_GetGlobalObject(context);
    JSValue document = JS_IsException(global) ? JS_EXCEPTION :
                       JS_GetPropertyStr(context, global, "document");
    bool is_document = !JS_IsException(global) &&
                       !JS_IsException(document) &&
                       js_values_strict_equal(context, this_value, document);
    JS_FreeValue(context, document);
    JS_FreeValue(context, global);
    if (!is_document) {
        JS_ThrowTypeError(context, "DOM query called on incompatible receiver");
        return -1;
    }
    return 0;
}

JSValue js_rt_wrap_dom_handle(JSContext *context, JSValueConst handle)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL
        || !JS_IsFunction(context, bridge->trusted_node_wrap)) {
        return JS_ThrowInternalError(
            context, "trusted DOM wrapper is unavailable");
    }
    return JS_Call(
        context, bridge->trusted_node_wrap, JS_UNDEFINED, 1, &handle);
}

JSValue js_rt_bridge_wrap_node(JSContext *context, DomBridge *bridge,
                               lxb_dom_node_t *node)
{
    JSValue handle = bridge_node_handle_value(context, bridge, node);
    if (JS_IsException(handle)) return handle;
    JSValue wrapped = js_rt_wrap_dom_handle(context, handle);
    JS_FreeValue(context, handle);
    return wrapped;
}

int64_t js_rt_document_scope(JSContext *context, JSValueConst value)
{
    JSValue handle_value = JS_GetPropertyStr(context, value, "__handle");
    if (JS_IsException(handle_value)) return -2;
    if (!JS_IsUndefined(handle_value)) {
        int64_t handle = 0;
        int converted = JS_ToInt64(context, &handle, handle_value);
        JS_FreeValue(context, handle_value);
        if (converted < 0) return -2;
        DomBridge *bridge = JS_GetContextOpaque(context);
        size_t slot = 0;
        return handle > 0
            && js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
            && bridge->nodes[slot]->type == LXB_DOM_NODE_TYPE_DOCUMENT
            ? handle : -1;
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue document = JS_IsException(global) ? JS_EXCEPTION
        : JS_GetPropertyStr(context, global, "document");
    JS_FreeValue(context, global);
    if (JS_IsException(document)) return -2;
    bool realm_document = js_values_strict_equal(context, value, document);
    JS_FreeValue(context, document);
    return realm_document ? 0 : -1;
}

bool js_rt_element_walk_init(DomElementWalk *walk, DomBridge *bridge,
                             int64_t scope)
{
    *walk = (DomElementWalk) { .bridge = bridge, .shadow_depth = SIZE_MAX };
    if (bridge == NULL || bridge->document == NULL
        || bridge->document->html == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    walk->boundary = root;
    if (scope != 0) {
        size_t slot = 0;
        if (!js_rt_bridge_node_slot_for_handle(bridge, scope, &slot)
            || bridge->nodes[slot]->type != LXB_DOM_NODE_TYPE_DOCUMENT)
            return false;
        walk->boundary = bridge->nodes[slot];
        root = bridge->nodes[slot]->first_child;
    }
    return bridge_document_order_traversal_init(
        bridge, &walk->traversal, root, walk->boundary);
}

lxb_dom_node_t *js_rt_element_walk_next(DomElementWalk *walk)
{
    for (lxb_dom_node_t *at = dom_document_order_next(&walk->traversal);
         at != NULL; at = dom_document_order_next(&walk->traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                walk->bridge, at, walk->boundary, &walk->traversal,
                &walk->shadow_depth)) continue;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && bridge_traversal_node_visible(
                   walk->bridge, at, &walk->traversal)) return at;
    }
    return NULL;
}

static lxb_dom_node_t *find_element_id_exact(DomBridge *bridge,
                                             lxb_dom_node_t *node,
                                             const char *identifier,
                                             size_t length)
{
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    size_t shadow_depth = SIZE_MAX;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, node, &traversal, &shadow_depth)) continue;
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        /* Lexbor keeps each element's id attribute at hand; reading it
           avoids a by-name attribute search per element. */
        lxb_dom_attr_t *id =
            lxb_dom_element_id_attribute(lxb_dom_interface_element(at));
        if (id == NULL) continue;
        size_t value_length = 0;
        const lxb_char_t *value = lxb_dom_attr_value(id, &value_length);
        if (value != NULL && value_length == length
            && memcmp(value, identifier, length) == 0) return at;
    }
    return NULL;
}

JSValue js_dom_get_element_by_id_method(
JSContext *context, JSValueConst this_value,
int argc, JSValueConst *argv)
{
    int64_t scope = dom_method_scope_handle(context, this_value);
    if (scope != 0) {
        if (tilefinch_trace_script_failures()) {
            fprintf(stderr, "dom-get-element-by-id incompatible scope=%lld\n",
                    (long long) scope);
        }
        return JS_EXCEPTION;
    }
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NULL;
    size_t length = 0;
    const char *identifier = JS_ToCStringLen(context, &length, argv[0]);
    if (identifier == NULL) return JS_EXCEPTION;
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    /* getElementById("") is null even when an element has id="". */
    lxb_dom_node_t *found = length != 0 && length <= 128
        ? find_element_id_exact(bridge, root, identifier, length) : NULL;
    if (found != NULL && !bridge_node_visible(bridge, found)) found = NULL;
    size_t section = 0;
    char tag_name[32] = "div";
    char stable_key[96] = {0};
    bool indexed = !bridge->remote_lookup_suppressed
        && length != 0 && length <= 128
        && bridge->remote_element_lookup != NULL
        && bridge->remote_element_lookup(
               bridge->remote_element_opaque, identifier, length,
               &section, tag_name, stable_key);
    /* A materialized section is only one source interval.  A duplicate ID in
       an earlier, unmaterialized interval still wins in document order. */
    if (found != NULL && (!indexed || section >= bridge->section_identity)) {
        JS_FreeCString(context, identifier);
        JSValue handle = bridge_node_handle_value(context, bridge, found);
        if (JS_IsException(handle)) return handle;
        JSValue wrapped = js_rt_wrap_dom_handle(context, handle);
        JS_FreeValue(context, handle);
        return wrapped;
    }
    bool remote = indexed && section != bridge->section_identity;
    if (!remote) {
        JS_FreeCString(context, identifier);
        return JS_NULL;
    }
    const char *queue_key = stable_key[0] == '\0'
                            ? identifier : stable_key;
    size_t queue_length = stable_key[0] == '\0'
                          ? length : strlen(stable_key);
    if (bridge->remote_node_read == NULL) {
        js_rt_bridge_queue_remote_element(bridge, queue_key, queue_length, section);
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue wrapper = JS_IsException(global) ? JS_EXCEPTION
        : JS_GetPropertyStr(context, global,
                            stable_key[0] == '\0'
                              ? "__tilefinchWrapRemote"
                              : "__tilefinchWrapRemoteStable");
    if (JS_IsException(global) || JS_IsException(wrapper)) {
        JS_FreeCString(context, identifier);
        JS_FreeValue(context, wrapper);
        JS_FreeValue(context, global);
        return JS_EXCEPTION;
    }
    JSValue arguments[3] = {
        stable_key[0] == '\0'
          ? JS_NewStringLen(context, identifier, length)
          : JS_NewString(context, stable_key),
        JS_NewString(context, tag_name),
        JS_NewInt64(context, (int64_t) section)
    };
    JS_FreeCString(context, identifier);
    JSValue result = JS_Call(context, wrapper, global, 3, arguments);
    for (size_t i = 0; i < 3; i++) JS_FreeValue(context, arguments[i]);
    JS_FreeValue(context, wrapper);
    JS_FreeValue(context, global);
    return result;
}

static JSValue simple_id_selector_value(JSContext *context,
                                        JSValueConst selector)
{
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, selector);
    if (text == NULL) return JS_EXCEPTION;
    bool simple = length > 1 && length <= 129 && text[0] == '#';
    for (size_t i = 1; simple && i < length; i++) {
        unsigned char byte = (unsigned char) text[i];
        simple = isalnum(byte) || byte == '-' || byte == '_';
    }
    JSValue value = simple
        ? JS_NewStringLen(context, text + 1, length - 1) : JS_UNDEFINED;
    JS_FreeCString(context, text);
    return value;
}

JSValue js_dom_query_selector_method(JSContext *context,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    int64_t scope = dom_method_scope_handle(context, this_value);
    if (scope < 0) return JS_EXCEPTION;
    JSValue selector = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue handle = JS_NewInt64(context, scope);
    JSValue arguments[2] = { selector, handle };
    JSValue found = js_dom_query(context, JS_UNDEFINED, 2, arguments);
    JS_FreeValue(context, handle);
    if (JS_IsException(found)) return found;
    int64_t found_handle = 0;
    if (scope == 0 && JS_ToInt64(context, &found_handle, found) == 0) {
        bool local_found = found_handle != 0;
        bool local_precedes_sections = false;
        if (local_found) {
            DomBridge *bridge = JS_GetContextOpaque(context);
            lxb_dom_node_t *node = js_rt_bridge_node_arg(context, bridge, found);
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            /* Section stores virtualize body descendants, never the document
               element, head, or body roots.  Those roots precede every body
               descendant in tree order, so a local match is already the
               canonical querySelector result and cannot be displaced by a
               remote section. */
            local_precedes_sections = node != NULL
                && (node == document_body_node(bridge->document)
                    || (name != NULL && name_length == 4
                        && (strncasecmp(name, "html", 4) == 0
                            || strncasecmp(name, "head", 4) == 0)));
        }
        JSValue identifier = simple_id_selector_value(context, selector);
        if (JS_IsException(identifier)) {
            JS_FreeValue(context, found);
            return identifier;
        }
        if (!local_found && !JS_IsUndefined(identifier)) {
            JS_FreeValue(context, found);
            JSValue remote = js_dom_get_element_by_id_method(
                context, this_value, 1, &identifier);
            JS_FreeValue(context, identifier);
            return remote;
        }
        JS_FreeValue(context, identifier);
        JSValue remote = local_precedes_sections ? JS_NULL
            : js_remote_selector_result(context, selector, local_found);
        if (JS_IsException(remote) || !JS_IsNull(remote)) {
            JS_FreeValue(context, found);
            return remote;
        }
        JS_FreeValue(context, remote);
    }
    JSValue wrapped = js_rt_wrap_dom_handle(context, found);
    JS_FreeValue(context, found);
    return wrapped;
}

static JSValue js_nodelist_item(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    int64_t index = -1;
    if (argc < 1 || JS_ToInt64(context, &index, argv[0]) < 0) {
        return JS_NULL;
    }
    if (index < 0 || index > UINT32_MAX) return JS_NULL;
    JSValue value = JS_GetPropertyUint32(context, this_value,
                                         (uint32_t) index);
    if (JS_IsUndefined(value)) {
        JS_FreeValue(context, value);
        return JS_NULL;
    }
    return value;
}

JSValue js_dom_query_selector_all_method(JSContext *context,
                                         JSValueConst this_value,
                                         int argc,
                                         JSValueConst *argv)
{
    int64_t scope = dom_method_scope_handle(context, this_value);
    if (scope < 0) return JS_EXCEPTION;
    JSValue selector = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue handle = JS_NewInt64(context, scope);
    JSValue arguments[2] = { selector, handle };
    JSValue values = js_dom_query_all(context, JS_UNDEFINED, 2, arguments);
    JS_FreeValue(context, handle);
    if (JS_IsException(values)) return values;

    JSValue length_value = JS_GetPropertyStr(context, values, "length");
    uint32_t length = 0;
    if (JS_IsException(length_value)
        || JS_ToUint32(context, &length, length_value) < 0) {
        JS_FreeValue(context, length_value);
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, length_value);
    bool remotely_wrapped = false;
    if (scope == 0) {
        JSValue remote = js_remote_selector_collection(
            context, selector, values, length, &length);
        if (JS_IsException(remote)) {
            JS_FreeValue(context, values);
            return remote;
        }
        if (!JS_IsNull(remote)) {
            JS_FreeValue(context, values);
            values = remote;
            remotely_wrapped = true;
        } else JS_FreeValue(context, remote);
    }
    if (scope == 0 && length == 0 && !remotely_wrapped) {
        JSValue identifier = simple_id_selector_value(context, selector);
        if (JS_IsException(identifier)) {
            JS_FreeValue(context, values);
            return identifier;
        }
        if (!JS_IsUndefined(identifier)) {
            JSValue remote = js_dom_get_element_by_id_method(
                context, this_value, 1, &identifier);
            if (JS_IsException(remote)
                || (!JS_IsNull(remote)
                    && JS_SetPropertyUint32(context, values, 0, remote) < 0)) {
                if (JS_IsNull(remote)) JS_FreeValue(context, remote);
                JS_FreeValue(context, identifier);
                JS_FreeValue(context, values);
                return JS_EXCEPTION;
            }
            if (JS_IsNull(remote)) JS_FreeValue(context, remote);
        } else {
            JSValue first = js_remote_selector_result(
                context, selector, false);
            if (JS_IsException(first)
                || (!JS_IsNull(first)
                    && JS_SetPropertyUint32(context, values, 0, first) < 0)) {
                if (JS_IsNull(first)) JS_FreeValue(context, first);
                JS_FreeValue(context, identifier);
                JS_FreeValue(context, values);
                return JS_EXCEPTION;
            }
            if (JS_IsNull(first)) JS_FreeValue(context, first);
        }
        JS_FreeValue(context, identifier);
    }
    for (uint32_t index = 0; !remotely_wrapped && index < length; index++) {
        JSValue found = JS_GetPropertyUint32(context, values, index);
        if (JS_IsException(found)) {
            JS_FreeValue(context, values);
            return found;
        }
        JSValue wrapped = js_rt_wrap_dom_handle(context, found);
        JS_FreeValue(context, found);
        if (JS_IsException(wrapped)
            || JS_SetPropertyUint32(context, values, index, wrapped) < 0) {
            if (!JS_IsException(wrapped)) {
                /* JS_SetPropertyUint32 consumes wrapped. */
            }
            JS_FreeValue(context, values);
            return JS_EXCEPTION;
        }
    }
    JSValue item = JS_NewCFunction(context, js_nodelist_item, "item", 1);
    if (JS_IsException(item)
        || JS_SetPropertyStr(context, values, "item", item) < 0) {
        JS_FreeValue(context, values);
        return JS_EXCEPTION;
    }
    return values;
}

JSValue js_dom_relation(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    int32_t relation = 0;
    if (node == NULL || argc < 2
        || JS_ToInt32(context, &relation, argv[1]) < 0) {
        return JS_NewInt64(context, 0);
    }
    bool elements_only = relation >= 0 && relation <= 3;
    /* Relation 8 is the raw parent: unlike relation 0 it may be a
       document fragment or the document itself (parentNode semantics). */
    lxb_dom_node_t *related = relation == 0 || relation == 8
                              ? node->parent
                              : (relation == 1 || relation == 4
                                 ? node->first_child
                              : (relation == 3 || relation == 6
                                 ? node->prev
                              : (relation == 7 ? node->last_child
                                               : node->next)));
    while (elements_only && related != NULL
           && related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        related = relation == 1 || relation == 2 ? related->next
                  : (relation == 3 ? related->prev : related->parent);
    }
    if (related != NULL && !bridge_node_visible(bridge, related)) related = NULL;
    return bridge_node_handle_value(context, bridge, related);
}

/* DOM order needs no wrapper for ancestors or siblings. Keep this constant
   stack and bounded even for detached trees; JS handles virtual/shadow trees. */
JSValue js_dom_compare_position(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handles[2];
    size_t slots[2];
    if (argc < 2 || bridge == NULL) return JS_UNDEFINED;
    /* Coercions can run author code: resolve neither pointer until both end. */
    if (JS_ToInt64(context, &handles[0], argv[0]) < 0
        || JS_ToInt64(context, &handles[1], argv[1]) < 0)
        return JS_EXCEPTION;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handles[0], &slots[0])
        || !js_rt_bridge_node_slot_for_handle(bridge, handles[1], &slots[1]))
        return JS_UNDEFINED;
    lxb_dom_node_t *left = bridge->nodes[slots[0]];
    lxb_dom_node_t *right = bridge->nodes[slots[1]];
    if (left == right) return JS_NewInt32(context, 0);
    lxb_dom_node_t *roots[2] = {left, right};
    size_t depths[2] = {0, 0};
    for (unsigned i = 0; i < 2; ++i) {
        while (roots[i]->parent != NULL && depths[i] < 512u) {
            roots[i] = roots[i]->parent;
            depths[i]++;
        }
        if (roots[i]->parent != NULL) return JS_UNDEFINED;
    }
    /* Disconnected ordering stays in the JS WeakMap, never pointer order. */
    if (roots[0] != roots[1]) return JS_UNDEFINED;
    while (depths[0] > depths[1]) { left = left->parent; depths[0]--; }
    if (left == right) return JS_NewInt32(context, 2 | 8);
    while (depths[1] > depths[0]) { right = right->parent; depths[1]--; }
    if (left == right) return JS_NewInt32(context, 4 | 16);
    while (left->parent != right->parent) {
        left = left->parent;
        right = right->parent;
    }
    for (size_t i = 0; right->prev != NULL && i < 16384u; ++i) {
        right = right->prev;
        if (right == left) return JS_NewInt32(context, 4);
    }
    return right->prev == NULL ? JS_NewInt32(context, 2) : JS_UNDEFINED;
}

/* __tilefinchIsAncestor(ancestor, node): whether ancestor lies on node's
   parentNode chain within the script walks' 256-step bound, without a
   wrapper per step (mutation observer subtree checks). undefined when a
   handle is stale or section-remote nodes make the script chain differ. */
JSValue js_dom_is_ancestor(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handles[2];
    size_t slots[2];
    if (argc < 2 || bridge == NULL || bridge->remote_mode_seen
        || bridge->remote_node_write != NULL
        || bridge->node_visibility != NULL) return JS_UNDEFINED;
    if (JS_ToInt64(context, &handles[0], argv[0]) < 0
        || JS_ToInt64(context, &handles[1], argv[1]) < 0)
        return JS_EXCEPTION;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handles[0], &slots[0])
        || !js_rt_bridge_node_slot_for_handle(bridge, handles[1], &slots[1]))
        return JS_UNDEFINED;
    const lxb_dom_node_t *ancestor = bridge->nodes[slots[0]];
    const lxb_dom_node_t *node = bridge->nodes[slots[1]];
    if (bridge_node_is_shadow_root(bridge, node)) return JS_FALSE;
    const lxb_dom_node_t *at = node->parent;
    for (unsigned steps = 0; at != NULL && steps < 256u; steps++) {
        if (at == ancestor) return JS_TRUE;
        /* The internal host link is not a DOM parentNode edge. */
        if (bridge_node_is_shadow_root(bridge, at)) break;
        at = at->parent;
    }
    return JS_FALSE;
}

/* __tilefinchSelectorsTestAttribute(name): whether some selector of the
   attached sheet tests an attribute whose name begins with `name`
   (stylesheet_selectors_reference_attribute_prefix); true without a sheet.
   A data-* attribute no selector tests is author bookkeeping that cannot
   restyle anything. */
JSValue js_dom_selectors_test_attribute(JSContext *context,
                                        JSValueConst this_value,
                                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL || argc < 1)
        return JS_TRUE;
    size_t length = 0;
    const char *name = JS_ToCStringLen(context, &length, argv[0]);
    if (name == NULL) return JS_EXCEPTION;
    bool tested = stylesheet_selectors_reference_attribute_prefix(
        bridge->stylesheet, name, length);
    JS_FreeCString(context, name);
    return JS_NewBool(context, tested);
}

/* __tilefinchNextAncestorEntry(node, entries, start): the first index
   i >= start whose entries[i].handle is node itself, lies on node's chain
   as above, or is not a live native handle (script decides those);
   entries.length when none. One call skips an observer's unrelated
   transient registrations. undefined as for __tilefinchIsAncestor. The
   entries are the bootstrap's own plain objects: reading them runs no
   page code, so the resolved nodes stay valid throughout. */
JSValue js_dom_next_ancestor_entry(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    uint32_t index = 0, length = 0;
    size_t slot = 0;
    if (argc < 3 || bridge == NULL || bridge->remote_mode_seen
        || bridge->remote_node_write != NULL
        || bridge->node_visibility != NULL || !JS_IsArray(context, argv[1]))
        return JS_UNDEFINED;
    JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
    int converted = JS_IsException(length_value) ? -1
        : JS_ToUint32(context, &length, length_value);
    JS_FreeValue(context, length_value);
    if (converted < 0 || JS_ToInt64(context, &handle, argv[0]) < 0
        || JS_ToUint32(context, &index, argv[2]) < 0) return JS_EXCEPTION;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        return JS_UNDEFINED;
    const lxb_dom_node_t *node = bridge->nodes[slot];
    JSAtom handle_atom = JS_NewAtom(context, "handle");
    if (handle_atom == JS_ATOM_NULL) return JS_EXCEPTION;
    for (; index < length; index++) {
        JSValue entry = JS_GetPropertyUint32(context, argv[1], index);
        JSValue value = JS_IsObject(entry)
            ? JS_GetProperty(context, entry, handle_atom) : JS_UNDEFINED;
        JS_FreeValue(context, entry);
        int64_t candidate = 0;
        size_t candidate_slot = 0;
        bool native = JS_IsNumber(value)
            && JS_ToInt64(context, &candidate, value) == 0
            && js_rt_bridge_node_slot_for_handle(bridge, candidate,
                                                 &candidate_slot);
        JS_FreeValue(context, value);
        if (!native) break;
        const lxb_dom_node_t *root = bridge->nodes[candidate_slot];
        const lxb_dom_node_t *at = node;
        for (unsigned steps = 0; at != NULL && at != root && steps < 256u;
             steps++) {
            if (bridge_node_is_shadow_root(bridge, at)) {
                at = NULL;
                break;
            }
            at = at->parent;
        }
        if (at == root) break;
    }
    JS_FreeAtom(context, handle_atom);
    return JS_NewUint32(context, index);
}

/* __tilefinchNearestId(node): the first non-empty id on node's inclusive
   parentElement chain within 256 steps, "" when none (section state's
   dirty-node key, without a wrapper per step). undefined where wrapper
   stable keys may take precedence (section-remote nodes). */
JSValue js_dom_nearest_id(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    size_t slot = 0;
    if (argc < 1 || bridge == NULL || bridge->remote_mode_seen
        || bridge->remote_node_write != NULL
        || bridge->node_visibility != NULL) return JS_UNDEFINED;
    if (JS_ToInt64(context, &handle, argv[0]) < 0) return JS_EXCEPTION;
    if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        return JS_UNDEFINED;
    lxb_dom_node_t *at = bridge->nodes[slot];
    for (unsigned steps = 0; at != NULL && steps < 256u; steps++) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const lxb_char_t *id = lxb_dom_element_get_attribute(
                lxb_dom_interface_element(at), (const lxb_char_t *) "id", 2,
                &length);
            if (id != NULL && length > 0)
                return JS_NewStringLen(context, (const char *) id, length);
        }
        /* parentElement stops at a non-element or a shadow-root carrier. */
        at = at->parent;
        if (at != NULL && (at->type != LXB_DOM_NODE_TYPE_ELEMENT
                           || bridge_node_is_shadow_root(bridge, at)))
            break;
    }
    return JS_NewString(context, "");
}

JSValue js_dom_is_connected(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    return bridge_node_is_connected(node) ? JS_TRUE : JS_FALSE;
}

/* Node.getRootNode() and the pre-insert "inclusive ancestor" check without
   a wrapper per ancestor. Walks native parents from argv[0]: returns
   argv[1]'s handle if the walk meets it, -1 on reaching a document, else
   the handle of the parentless root (script continues through a
   JavaScript-side detached parent, as closest() does). 0 leaves the walk
   to script: shadow trees, remote or section-filtered documents, and
   ancestries past the script walks' 256-step bound. */
#define DOM_ROOT_NODE_ANCESTOR_LIMIT 256u
JSValue js_dom_root_node(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handles[2] = {0, 0};
    if (bridge == NULL || argc < 1 || bridge->remote_mode_seen
        || bridge->remote_node_write != NULL
        || bridge->node_visibility != NULL || bridge->shadow_root_count > 0)
        return JS_NewInt32(context, 0);
    if (JS_ToInt64(context, &handles[0], argv[0]) < 0
        || (argc > 1 && !JS_IsUndefined(argv[1])
            && JS_ToInt64(context, &handles[1], argv[1]) < 0))
        return JS_EXCEPTION;
    size_t slot = 0;
    if (handles[0] <= 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handles[0], &slot))
        return JS_NewInt32(context, 0);
    lxb_dom_node_t *at = bridge->nodes[slot];
    lxb_dom_node_t *stop = NULL;
    if (handles[1] > 0
        && js_rt_bridge_node_slot_for_handle(bridge, handles[1], &slot))
        stop = bridge->nodes[slot];
    for (unsigned steps = 0; at != stop && at->parent != NULL; steps++) {
        if (steps >= DOM_ROOT_NODE_ANCESTOR_LIMIT)
            return JS_NewInt32(context, 0);
        at = at->parent;
    }
    if (at == stop) return JS_NewInt64(context, handles[1]);
    if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT)
        return JS_NewInt32(context, -1);
    return bridge_node_handle_value(context, bridge, at);
}

/* __tilefinchNodeOwner(handle): the node's adopted owner tag, 0 for an
   unknown handle. (handle, tag) records the tag for that node alone (the
   bootstrap adopts node by node) and answers whether it could; the first
   such call allocates the table and tags every registered node.
   (0) lists the tags 3-255 some node still holds, so the bootstrap can
   reuse the rest. */
JSValue js_dom_node_owner(JSContext *context, JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    int32_t tag = 0;
    size_t slot = 0;
    if (bridge == NULL || argc < 1
        || JS_ToInt64(context, &handle, argv[0]) < 0)
        return JS_NewInt32(context, 0);
    unsigned char *tags = bridge->node_owner_tags;
    if (handle == 0) {
        JSValue list = JS_NewArray(context);
        uint32_t seen[8] = {0}, count = 0;
        for (size_t at = 0; tags != NULL && !JS_IsException(list)
             && at < bridge->node_count; at++) {
            unsigned value = tags[at];
            if (value < 3 || (seen[value / 32u] & (1u << (value % 32u))))
                continue;
            seen[value / 32u] |= 1u << (value % 32u);
            if (JS_SetPropertyUint32(context, list, count++,
                                     JS_NewInt32(context, (int32_t) value))
                < 0) {
                JS_FreeValue(context, list);
                return JS_EXCEPTION;
            }
        }
        return list;
    }
    if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        return argc > 1 ? JS_FALSE : JS_NewInt32(context, 0);
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        unsigned char value = tags != NULL ? tags[slot] : 0;
        if (value == 0) {
            value = bridge_node_owner_tag_inherited(
                bridge, bridge->nodes[slot]);
            if (tags != NULL) tags[slot] = value;
        }
        return JS_NewInt32(context, value);
    }
    if (JS_ToInt32(context, &tag, argv[1]) < 0) return JS_EXCEPTION;
    if (tag < 1 || tag > 255) return JS_FALSE;
    if (tags == NULL) {
        tags = budget_calloc(bridge->budget, bridge->node_capacity, 1);
        if (tags == NULL) return JS_FALSE;
        /* Registered nodes are the document's or template contents'. */
        for (size_t at = 0; at < bridge->node_count; at++)
            if (bridge->nodes[at] != NULL)
                tags[at] = bridge_node_owner_tag_inherited(
                    bridge, bridge->nodes[at]);
        bridge->node_owner_tags = tags;
    }
    tags[slot] = (unsigned char) tag;
    return JS_TRUE;
}

JSValue js_dom_children(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    JSValue array = JS_NewArray(context);
    if (node == NULL || JS_IsException(array)) return array;
    uint32_t count = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL
         && count < DOM_QUERY_RESULT_LIMIT; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!bridge_node_visible(bridge, child)) continue;
        int64_t handle = js_rt_bridge_register_node(bridge, child);
        if (handle == 0) {
            JS_FreeValue(context, array);
            return bridge_throw_handles_exhausted(context);
        }
        (void) JS_SetPropertyUint32(context, array, count++,
                                    JS_NewInt64(context, handle));
    }
    return array;
}

JSValue js_dom_child_nodes(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    JSValue array = JS_NewArray(context);
    if (node == NULL || JS_IsException(array)) return array;
    uint32_t count = 0;
    for (lxb_dom_node_t *child = node->first_child; child != NULL
         && count < DOM_QUERY_RESULT_LIMIT; child = child->next) {
        if (!bridge_node_visible(bridge, child)) continue;
        int64_t handle = js_rt_bridge_register_node(bridge, child);
        if (handle == 0) {
            JS_FreeValue(context, array);
            return bridge_throw_handles_exhausted(context);
        }
        (void) JS_SetPropertyUint32(context, array, count++,
                                    JS_NewInt64(context, handle));
    }
    return array;
}

JSValue js_dom_document_child_nodes(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSValue array = JS_NewArray(context);
    if (bridge == NULL || bridge->document == NULL
        || bridge->document->html == NULL || JS_IsException(array)) {
        return array;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    uint32_t count = 0;
    for (lxb_dom_node_t *child = root->first_child; child != NULL
         && count < DOM_QUERY_RESULT_LIMIT; child = child->next) {
        if (!bridge_node_visible(bridge, child)) continue;
        int64_t handle = js_rt_bridge_register_node(bridge, child);
        if (handle == 0) {
            JS_FreeValue(context, array);
            return bridge_throw_handles_exhausted(context);
        }
        (void) JS_SetPropertyUint32(context, array, count++,
                                    JS_NewInt64(context, handle));
    }
    return array;
}

static JSValue bridge_tag_name_value(JSContext *context,
                                    lxb_dom_node_t *node);

JSValue js_dom_tag_name(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    return bridge_tag_name_value(context, node);
}

static JSValue bridge_tag_name_value(JSContext *context,
                                    lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *name = node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        ? NULL
        : (const char *) lxb_dom_element_qualified_name(
              lxb_dom_interface_element(node), &length);
    if (name == NULL) return JS_NULL;
    if (node->ns != LXB_NS_HTML)
        return JS_NewStringLen(context, name, length);
    char upper[64];
    if (length >= sizeof(upper)) length = sizeof(upper) - 1;
    for (size_t i = 0; i < length; i++)
        upper[i] = (char) toupper((unsigned char) name[i]);
    upper[length] = '\0';
    return JS_NewStringLen(context, upper, length);
}

JSValue js_dom_namespace_uri(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->owner_document == NULL) return JS_NULL;
    size_t length = 0;
    const lxb_char_t *uri = lxb_ns_by_id(
        node->owner_document->ns, node->ns, &length);
    return uri == NULL || length == 0
        ? JS_NULL
        : JS_NewStringLen(context, (const char *) uri, length);
}

static int bridge_dom_node_type(const lxb_dom_node_t *node);

/* An interned (atom) string: every wrapper of a tag or namespace then
   holds the same string instead of its own copy. */
static JSValue bridge_interned_string(JSContext *context, const char *chars,
                                      size_t length)
{
    JSAtom atom = JS_NewAtomLen(context, chars, length);
    if (atom == JS_ATOM_NULL) return JS_EXCEPTION;
    JSValue value = JS_AtomToString(context, atom);
    JS_FreeAtom(context, atom);
    return value;
}

/* __tilefinchNodeIdentity(handle[, array]): a new wrapper's identity in
   one call, [tagName, nodeType, namespaceURI, section identity], each as
   __tilefinchTagName (|| ""), __tilefinchNodeType, __tilefinchNamespaceURI
   and __tilefinchSectionIdentity report it. */
JSValue js_dom_node_identity(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    bool element = node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT;
    size_t length = 0;
    const char *name = element
        ? (const char *) lxb_dom_element_qualified_name(
              lxb_dom_interface_element(node), &length)
        : NULL;
    char upper[64];
    if (name != NULL && node->ns == LXB_NS_HTML) {
        /* As bridge_tag_name_value reports it. */
        if (length >= sizeof(upper)) length = sizeof(upper) - 1;
        for (size_t i = 0; i < length; i++)
            upper[i] = (char) toupper((unsigned char) name[i]);
        name = upper;
    }
    JSValue tag = bridge_interned_string(context, name == NULL ? "" : name,
                                         name == NULL ? 0 : length);
    if (JS_IsException(tag)) return tag;
    JSValue namespace_uri = JS_NULL;
    const lxb_char_t *uri = !element || node->owner_document == NULL
        ? NULL : lxb_ns_by_id(node->owner_document->ns, node->ns, &length);
    if (uri != NULL && length > 0)
        namespace_uri = bridge_interned_string(
            context, (const char *) uri, length);
    /* The caller may pass its own array to refill, sparing an allocation
       per wrapper. */
    JSValue identity = JS_IsException(namespace_uri) ? JS_EXCEPTION
        : argc > 1 && JS_IsArray(context, argv[1]) > 0
        ? JS_DupValue(context, argv[1]) : JS_NewArray(context);
    if (JS_IsException(identity)) {
        JS_FreeValue(context, tag);
        JS_FreeValue(context, namespace_uri);
        return JS_EXCEPTION;
    }
    /* Each set consumes its value, even when it fails; in index order, so
       a new array stays a fast array. */
    if (JS_SetPropertyUint32(context, identity, 0, tag) < 0
        || JS_SetPropertyUint32(
               context, identity, 1,
               JS_NewInt32(context, bridge_dom_node_type(node))) < 0) {
        JS_FreeValue(context, namespace_uri);
        JS_FreeValue(context, identity);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyUint32(context, identity, 2, namespace_uri) < 0
        || JS_SetPropertyUint32(
               context, identity, 3,
               JS_NewInt64(context, bridge == NULL
                           ? 0 : (int64_t) bridge->section_identity)) < 0) {
        JS_FreeValue(context, identity);
        return JS_EXCEPTION;
    }
    return identity;
}

JSValue js_dom_parse_color(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_NULL;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[0]);
    if (text == NULL) return JS_EXCEPTION;
    uint32_t color = 0;
    uint8_t alpha = 0;
    bool parsed = length <= 127
        && style_color_parse(text, length, &color, &alpha);
    JS_FreeCString(context, text);
    return parsed
        ? JS_NewUint32(context, ((uint32_t) alpha << 24) | color)
        : JS_NULL;
}

/* The DOM nodeType a native node reports to script. */
static int bridge_dom_node_type(const lxb_dom_node_t *node)
{
    if (node == NULL) return 0;
    switch (node->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT: return 1;
    case LXB_DOM_NODE_TYPE_TEXT: return 3;
    case LXB_DOM_NODE_TYPE_CDATA_SECTION: return 4;
    case LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION: return 7;
    case LXB_DOM_NODE_TYPE_COMMENT: return 8;
    case LXB_DOM_NODE_TYPE_DOCUMENT: return 9;
    case LXB_DOM_NODE_TYPE_DOCUMENT_TYPE: return 10;
    default: return 11;
    }
}

/*
 * Native receiver getters for the hottest node properties. Ancestor walks
 * read parentNode once per step, and the script getters paid a global
 * lookup, a virtual-node check and a wrapper-cache lookup in bytecode on
 * every read. These answer the ordinary case -- a native node of this
 * realm, no script-side detached parent, no section-remote nodes -- and
 * call the script getter (data[0]) for everything else, so the two cannot
 * disagree on anything unusual.
 */
enum {
    FAST_GETTER_PARENT_NODE = 0,
    FAST_GETTER_PARENT_ELEMENT = 1,
    FAST_GETTER_TAG_NAME = 2,
    FAST_GETTER_NODE_TYPE = 3,
    FAST_GETTER_FIRST_CHILD = 4,
    FAST_GETTER_LAST_CHILD = 5,
    FAST_GETTER_NEXT_SIBLING = 6,
    FAST_GETTER_PREVIOUS_SIBLING = 7,
    FAST_GETTER_COUNT
};

/* 1: *node is the receiver's native node; 0: use the script getter;
   -1: exception. */
static int fast_getter_receiver(JSContext *context, DomBridge *bridge,
                                JSValueConst receiver, lxb_dom_node_t **node,
                                int64_t *resolved_handle)
{
    if (bridge == NULL || bridge->remote_mode_seen
        || bridge->remote_node_write != NULL || !JS_IsObject(receiver))
        return 0;
    JSValue handle_value = JS_GetPropertyStr(context, receiver, "__handle");
    if (JS_IsException(handle_value)) return -1;
    int64_t handle = 0;
    bool numeric = JS_IsNumber(handle_value)
        && JS_ToInt64(context, &handle, handle_value) == 0;
    JS_FreeValue(context, handle_value);
    size_t slot = 0;
    if (!numeric || handle <= 0
        || !js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
        return 0;
    *node = bridge->nodes[slot];
    if (resolved_handle != NULL) *resolved_handle = handle;
    return 1;
}

/* A script-side detached parent (JavaScript-only trees) takes the script
   path. 1: none; 0: present; -1: exception. */
static int fast_getter_no_detached_parent(JSContext *context,
                                          JSValueConst receiver)
{
    JSValue parent = JS_GetPropertyStr(context, receiver,
                                       "__tilefinchDetachedParent");
    if (JS_IsException(parent)) return -1;
    int present = JS_ToBool(context, parent);
    JS_FreeValue(context, parent);
    return present < 0 ? -1 : present ? 0 : 1;
}

/* The live wrapper the script cache holds for `node`, else a new one from
   the trusted wrapper; JS_NULL when script would produce none. */
static JSValue fast_getter_wrapper(JSContext *context, DomBridge *bridge,
                                   lxb_dom_node_t *node)
{
    int64_t handle = js_rt_bridge_register_node(bridge, node);
    if (handle == 0 && node != NULL)
        return bridge_throw_handles_exhausted(context);
    if (handle <= 0) return JS_NULL;
    size_t slot = 0;
    if (bridge->wrapper_refs != NULL
        && js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        && JS_IsObject(bridge->wrapper_refs[slot])) {
        JSValue cached = JS_WeakRefDeref(context,
                                         bridge->wrapper_refs[slot]);
        if (JS_IsObject(cached)) return cached;
        JS_FreeValue(context, cached);
    }
    JSValue handle_value = JS_NewInt64(context, handle);
    JSValue wrapped = js_rt_wrap_dom_handle(context, handle_value);
    JS_FreeValue(context, handle_value);
    return wrapped;
}

static bool fast_getter_canonical_name(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL
        && ((length == 4 && (strncasecmp(name, "html", 4) == 0
                             || strncasecmp(name, "head", 4) == 0
                             || strncasecmp(name, "body", 4) == 0)));
}

static JSValue js_dom_fast_getter(JSContext *context, JSValueConst receiver,
                                  int argc, JSValueConst *argv, int kind,
                                  JSValue *data)
{
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = NULL;
    int64_t handle = 0;
    int ready = fast_getter_receiver(context, bridge, receiver, &node, &handle);
    if (ready < 0) return JS_EXCEPTION;
    if (ready > 0 && (kind == FAST_GETTER_PARENT_NODE
                      || kind == FAST_GETTER_PARENT_ELEMENT)) {
        ready = fast_getter_no_detached_parent(context, receiver);
        if (ready < 0) return JS_EXCEPTION;
        /* The property lookup can run author code and retire the node. Use
           the captured generation-bearing handle, never a second observable
           receiver lookup or the raw pointer from before the callback. */
        size_t slot = 0;
        node = NULL;
        if (ready > 0 && !bridge->remote_mode_seen
            && bridge->remote_node_write == NULL) {
            /* A retired handle has no parent. Falling back would re-enter
               author code and can return undefined for a forged receiver. */
            if (!js_rt_bridge_node_slot_for_handle(bridge, handle, &slot))
                return JS_NULL;
            node = bridge->nodes[slot];
        } else {
            ready = 0;
        }
    }
    if (ready > 0) {
        if (kind == FAST_GETTER_NODE_TYPE)
            return JS_NewInt32(context, bridge_dom_node_type(node));
        if (kind == FAST_GETTER_TAG_NAME) {
            if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
                return JS_NewString(context, "");
            return bridge_tag_name_value(context, node);
        }
        if (kind >= FAST_GETTER_FIRST_CHILD) {
            /* Script: wrap(the relation's handle), which the relation
               already nulls when hidden. A shadow-root carrier is skipped
               over by script (shadowAdjustedSibling), so it goes there. */
            lxb_dom_node_t *related =
                kind == FAST_GETTER_FIRST_CHILD ? node->first_child
                : kind == FAST_GETTER_LAST_CHILD ? node->last_child
                : kind == FAST_GETTER_NEXT_SIBLING ? node->next
                : node->prev;
            if (related == NULL || !bridge_node_visible(bridge, related))
                return JS_NULL;
            if (!bridge_node_is_shadow_root(bridge, related)) {
                JSValue wrapper = fast_getter_wrapper(context, bridge,
                                                      related);
                if (!JS_IsNull(wrapper) && !JS_IsUndefined(wrapper))
                    return wrapper;
            }
            return JS_Call(context, data[0], receiver, 0, NULL);
        }
        lxb_dom_node_t *parent = node->parent;
        bool visible = parent != NULL && bridge_node_visible(bridge, parent);
        if (kind == FAST_GETTER_PARENT_ELEMENT) {
            /* Script: wrap(raw parent) instanceof Element, canonicalized;
               the shadow-root carrier is a DocumentFragment to script. */
            if (!visible || parent->type != LXB_DOM_NODE_TYPE_ELEMENT
                || bridge_node_is_shadow_root(bridge, parent))
                return JS_NULL;
            if (!fast_getter_canonical_name(parent)) {
                JSValue wrapper = fast_getter_wrapper(context, bridge,
                                                      parent);
                if (!JS_IsNull(wrapper) && !JS_IsUndefined(wrapper))
                    return wrapper;
                if (JS_IsException(wrapper)) return wrapper;
            }
        } else if (visible) {
            if (parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) {
                JSValue global = JS_GetGlobalObject(context);
                JSValue document = JS_GetPropertyStr(context, global,
                                                     "document");
                JS_FreeValue(context, global);
                return document;
            }
            JSValue wrapper = fast_getter_wrapper(context, bridge, parent);
            if (!JS_IsNull(wrapper) && !JS_IsUndefined(wrapper))
                return wrapper;
            if (JS_IsException(wrapper)) return wrapper;
        }
    }
    return JS_Call(context, data[0], receiver, 0, NULL);
}

/* Host A/B switch: TILEFINCH_DISABLE_FAST_DOM keeps the script getters
   and methods (lab comparisons); device builds always install. */
static bool fast_dom_disabled(void)
{
#if defined(TILEFINCH_PSP_VALIDATION_LOG) || defined(TILEFINCH_NO_TRACE)
    return false;
#else
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("TILEFINCH_DISABLE_FAST_DOM") != NULL;
    return disabled != 0;
#endif
}

/* __tilefinchMakeFastGetter(kind, scriptGetter, name): a native getter
   for one of the kinds above that falls back to scriptGetter. */
JSValue js_dom_make_fast_getter(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    if (fast_dom_disabled()) return JS_UNDEFINED;
    int32_t kind = -1;
    if (argc < 3 || JS_ToInt32(context, &kind, argv[0]) < 0
        || kind < 0 || kind >= FAST_GETTER_COUNT
        || !JS_IsFunction(context, argv[1])) return JS_UNDEFINED;
    JSValue getter = JS_NewCFunctionData(context, js_dom_fast_getter, 0,
                                         kind, 1, &argv[1]);
    if (JS_IsException(getter)) return getter;
    JSValue name = JS_ToString(context, argv[2]);
    if (JS_IsException(name)
        || JS_DefinePropertyValueStr(context, getter, "name", name,
                                     JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, getter);
        return JS_EXCEPTION;
    }
    return getter;
}

/* Native receiver methods, same contract as the getters above. */
enum {
    FAST_METHOD_GET_ATTRIBUTE = 0,
    FAST_METHOD_COUNT
};

static JSValue js_dom_fast_method(JSContext *context, JSValueConst receiver,
                                  int argc, JSValueConst *argv, int kind,
                                  JSValue *data)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = NULL;
    if (kind == FAST_METHOD_GET_ATTRIBUTE && argc > 0
        && JS_IsString(argv[0])) {
        int ready = fast_getter_receiver(context, bridge, receiver, &node, NULL);
        if (ready < 0) return JS_EXCEPTION;
        if (ready > 0 && node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node->ns == LXB_NS_HTML) {
            size_t length = 0;
            const char *name = JS_ToCStringLen(context, &length, argv[0]);
            if (name == NULL) return JS_EXCEPTION;
            /* HTML names are ASCII-lowercased; anything non-ASCII keeps
               script's Unicode toLowerCase. */
            char lowered[128];
            bool ascii = length < sizeof(lowered);
            for (size_t i = 0; ascii && i < length; i++) {
                unsigned char c = (unsigned char) name[i];
                if (c >= 0x80u) ascii = false;
                else lowered[i] = (char) tolower(c);
            }
            JS_FreeCString(context, name);
            if (ascii) {
                size_t value_length = 0;
                const lxb_char_t *value = lxb_dom_element_get_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) lowered, length, &value_length);
                bool present = value != NULL
                    || lxb_dom_element_has_attribute(
                        lxb_dom_interface_element(node),
                        (const lxb_char_t *) lowered, length);
                return !present ? JS_NULL
                    : JS_NewStringLen(context,
                                      value == NULL ? "" : (const char *) value,
                                      value == NULL ? 0 : value_length);
            }
        }
    }
    return JS_Call(context, data[0], receiver, argc, argv);
}

/* __tilefinchMakeFastMethod(kind, scriptMethod, name, length). */
JSValue js_dom_make_fast_method(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    if (fast_dom_disabled()) return JS_UNDEFINED;
    int32_t kind = -1, arity = 0;
    if (argc < 4 || JS_ToInt32(context, &kind, argv[0]) < 0
        || kind < 0 || kind >= FAST_METHOD_COUNT
        || !JS_IsFunction(context, argv[1])
        || JS_ToInt32(context, &arity, argv[3]) < 0) return JS_UNDEFINED;
    JSValue method = JS_NewCFunctionData(context, js_dom_fast_method, arity,
                                         kind, 1, &argv[1]);
    if (JS_IsException(method)) return method;
    JSValue name = JS_ToString(context, argv[2]);
    if (JS_IsException(name)
        || JS_DefinePropertyValueStr(context, method, "name", name,
                                     JS_PROP_CONFIGURABLE) < 0) {
        JS_FreeValue(context, method);
        return JS_EXCEPTION;
    }
    return method;
}

/* Section-remote wrappers exist: native getters defer to script. */
JSValue js_dom_note_remote_wrapper(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL) bridge->remote_mode_seen = true;
    return JS_UNDEFINED;
}

JSValue js_dom_node_type(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    return JS_NewInt32(context, bridge_dom_node_type(node));
}

/* Live TreeWalker/NodeIterator stepping. Each call walks native links from
   one node to the next in tree order whose type whatToShow admits, so nodes
   the walk only passes over never become wrappers and nothing is
   materialized up front. Shadow-root carriers other than the root are
   skipped with their subtrees, as in the light tree script sees. A handle
   is registered only for the node returned; when the realm's handle table
   is full the step throws rather than return a wrong node. */
#define DOM_TRAVERSE_MOVE_LIMIT 16384u
#define DOM_TRAVERSE_LINK_LIMIT DOM_TRAVERSAL_VISIT_LIMIT
enum {
    DOM_TRAVERSE_FOLLOWING = 0,
    DOM_TRAVERSE_FOLLOWING_SKIP_CHILDREN = 1,
    DOM_TRAVERSE_PRECEDING = 2
};
/* Results other than a positive handle. */
#define DOM_TRAVERSE_NONE 0
#define DOM_TRAVERSE_ROOT (-1)
#define DOM_TRAVERSE_UNSUPPORTED (-2)
#define DOM_TRAVERSE_DOCUMENT (-3)
/* -(handle + 4): the move ceiling stopped at this unmatched node. */

static bool bridge_traverse_hidden(DomBridge *bridge,
                                   const lxb_dom_node_t *node,
                                   const lxb_dom_node_t *root)
{
    return node != root && node->type == LXB_DOM_NODE_TYPE_ELEMENT
        && bridge->shadow_root_count != 0
        && bridge_node_is_shadow_root(bridge, node);
}

static lxb_dom_node_t *bridge_traverse_visible_sibling(
    DomBridge *bridge, lxb_dom_node_t *node, const lxb_dom_node_t *root,
    bool forward)
{
    for (size_t links = 0; node != NULL && links < DOM_TRAVERSE_LINK_LIMIT;
         links++) {
        if (!bridge_traverse_hidden(bridge, node, root)) return node;
        node = forward ? node->next : node->prev;
    }
    return NULL;
}

/* The node after `at` in tree order, not descending into `at` when asked;
   climbing stops at `root` exactly as TreeWalker's nextNode does. */
static lxb_dom_node_t *bridge_traverse_following(
    DomBridge *bridge, const lxb_dom_node_t *root, lxb_dom_node_t *at,
    bool skip_children)
{
    lxb_dom_node_t *next = skip_children ? NULL
        : bridge_traverse_visible_sibling(bridge, at->first_child, root, true);
    if (next != NULL) return next;
    for (size_t links = 0; at != NULL && links < DOM_TRAVERSE_LINK_LIMIT;
         links++, at = at->parent) {
        if (at == root) return NULL;
        next = bridge_traverse_visible_sibling(bridge, at->next, root, true);
        if (next != NULL) return next;
    }
    return NULL;
}

/* The node before `at` in tree order, never leaving `root`. */
static lxb_dom_node_t *bridge_traverse_preceding(
    DomBridge *bridge, const lxb_dom_node_t *root, lxb_dom_node_t *at)
{
    if (at == root) return NULL;
    lxb_dom_node_t *previous =
        bridge_traverse_visible_sibling(bridge, at->prev, root, false);
    if (previous == NULL) return at->parent;
    for (size_t links = 0; links < DOM_TRAVERSE_LINK_LIMIT; links++) {
        lxb_dom_node_t *last = bridge_traverse_visible_sibling(
            bridge, previous->last_child, root, false);
        if (last == NULL) return previous;
        previous = last;
    }
    return NULL;
}

JSValue js_dom_traverse(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t root_handle = 0, from_handle = 0;
    uint32_t what_to_show = 0;
    int32_t mode = 0;
    if (argc < 4 || JS_ToInt64(context, &root_handle, argv[0]) < 0
        || JS_ToInt64(context, &from_handle, argv[1]) < 0
        || JS_ToUint32(context, &what_to_show, argv[2]) < 0
        || JS_ToInt32(context, &mode, argv[3]) < 0) return JS_EXCEPTION;
    /* Section and remote documents expose a virtual tree that only the
       script-level DOM accessors model; they walk that instead. */
    if (bridge == NULL || bridge->document == NULL
        || bridge->document->html == NULL || bridge->node_visibility != NULL
        || bridge->remote_descendant_collect != NULL
        || mode < DOM_TRAVERSE_FOLLOWING || mode > DOM_TRAVERSE_PRECEDING)
        return JS_NewInt64(context, DOM_TRAVERSE_UNSUPPORTED);
    lxb_dom_node_t *document_node =
        lxb_dom_interface_node(bridge->document->html);
    size_t slot = 0;
    lxb_dom_node_t *root = root_handle == 0 ? document_node
        : js_rt_bridge_node_slot_for_handle(bridge, root_handle, &slot)
            ? bridge->nodes[slot] : NULL;
    lxb_dom_node_t *at = from_handle == 0 ? root
        : js_rt_bridge_node_slot_for_handle(bridge, from_handle, &slot)
            ? bridge->nodes[slot] : NULL;
    if (root == NULL || at == NULL)
        return JS_NewInt64(context, DOM_TRAVERSE_UNSUPPORTED);
    for (size_t moves = 0; moves < DOM_TRAVERSE_MOVE_LIMIT; moves++) {
        at = mode == DOM_TRAVERSE_PRECEDING
            ? bridge_traverse_preceding(bridge, root, at)
            : bridge_traverse_following(
                  bridge, root, at,
                  moves == 0 && mode == DOM_TRAVERSE_FOLLOWING_SKIP_CHILDREN);
        if (at == NULL) return JS_NewInt64(context, DOM_TRAVERSE_NONE);
        int type = bridge_dom_node_type(at);
        if (((what_to_show >> (type - 1)) & 1u) != 0u) {
            if (at == root) return JS_NewInt64(context, DOM_TRAVERSE_ROOT);
            if (at == document_node)
                return JS_NewInt64(context, DOM_TRAVERSE_DOCUMENT);
            return bridge_node_handle_value(context, bridge, at);
        }
        if (at == root && mode == DOM_TRAVERSE_PRECEDING)
            return JS_NewInt64(context, DOM_TRAVERSE_NONE);
    }
    /* Neither can be a resumption point: every walk ends at the root, and
       the document is only ever reached as its own root. */
    if (at == root || at == document_node)
        return JS_NewInt64(context, DOM_TRAVERSE_NONE);
    int64_t handle = js_rt_bridge_register_node(bridge, at);
    if (handle == 0) return bridge_throw_handles_exhausted(context);
    return JS_NewInt64(context, -(handle + 4));
}

/* False when a descendant could not be given a handle. */
static bool append_descendants(DomBridge *bridge, lxb_dom_node_t *node,
                               int32_t what_to_show, JSContext *context,
                               JSValue array, uint32_t *count)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, node)) return true;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < DOM_QUERY_RESULT_LIMIT;
         at = dom_document_order_next(&traversal)) {
        bool include = (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                        && (what_to_show & 1) != 0)
                       || (at->type == LXB_DOM_NODE_TYPE_TEXT
                           && (what_to_show & 4) != 0)
                       || (at->type == LXB_DOM_NODE_TYPE_COMMENT
                           && (what_to_show & 128) != 0);
        if (include && bridge_traversal_node_visible(
                bridge, at, &traversal)) {
            int64_t handle = js_rt_bridge_register_node(bridge, at);
            if (handle == 0) return false;
            (void) JS_SetPropertyUint32(context, array, (*count)++,
                                        JS_NewInt64(context, handle));
        }
    }
    return true;
}

JSValue js_dom_descendants(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSValue array = JS_NewArray(context);
    if (JS_IsException(array) || argc < 1) return array;
    lxb_dom_node_t *root = js_rt_bridge_node_arg(context, bridge, argv[0]);
    int32_t what_to_show = -1;
    if (argc > 1 && JS_ToInt32(context, &what_to_show, argv[1]) < 0) {
        JS_FreeValue(context, array);
        return JS_EXCEPTION;
    }
    unsigned root_special = 0;
    if (bridge != NULL && root != NULL) {
        if (root == lxb_dom_interface_node(bridge->document->html)) {
            root_special = 1;
        } else if (root == document_body_node(bridge->document)) {
            root_special = 3;
        }
    }
    if (root_special != 0 && bridge->remote_descendant_collect != NULL) {
        JSValue remote = js_remote_descendant_collection(
            context, bridge, root_special, what_to_show);
        if (JS_IsException(remote) || !JS_IsNull(remote)) {
            JS_FreeValue(context, array);
            return remote;
        }
        JS_FreeValue(context, remote);
    }
    uint32_t count = 0;
    if (root != NULL && !append_descendants(bridge, root, what_to_show,
                                            context, array, &count)) {
        JS_FreeValue(context, array);
        return bridge_throw_handles_exhausted(context);
    }
    return array;
}

/* MutationObserver arms the parser-insertion journal while any observer
   watches childList changes; see DocumentParserInsertionJournal. */
JSValue js_dom_observe_parser_insertions(JSContext *context,
                                         JSValueConst this_value,
                                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL) return JS_FALSE;
    int armed = argc > 0 ? JS_ToBool(context, argv[0]) : 0;
    if (armed < 0) return JS_EXCEPTION;
    document_parser_insertions_arm(bridge->document, armed != 0);
    return JS_TRUE;
}

JSValue js_dom_named_element_ids(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSValue array = JS_NewArray(context);
    if (JS_IsException(array) || bridge == NULL
        || bridge->document == NULL) return array;

    lxb_dom_node_t *root = lxb_dom_interface_node(bridge->document->html);
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, root, root)) return array;
    uint32_t count = 0;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && count < 128;
         at = dom_document_order_next(&traversal)) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !bridge_traversal_node_visible(
                bridge, at, &traversal)) continue;
        size_t id_length = 0;
        const char *id = document_attribute(at, "id", &id_length);
        if (id == NULL || id_length == 0 || id_length > 128) continue;
        JSValue value = JS_NewStringLen(context, id, id_length);
        if (JS_IsException(value)
            || JS_SetPropertyUint32(context, array, count++, value) < 0) {
            JS_FreeValue(context, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

/* Handles of the <iframe> and <frame> elements in the document's tree (or
   in the subtree at the optional root handle), in tree order: the
   containers of the document-tree child navigables window.length, window[i]
   and window[name] answer for. Template contents and shadow trees (their
   carriers' subtrees) are not in that tree. Bounded like a query result. */
JSValue js_dom_frame_handles(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    JSValue array = JS_NewArray(context);
    if (JS_IsException(array) || bridge == NULL
        || bridge->document == NULL || bridge->document->html == NULL)
        return array;
    /* Most pages have no frames at all: skip the walk (this runs before
       every script and on every element insertion or removal). */
    if (document_frames_impossible(bridge->document)) return array;
    lxb_dom_node_t *root = argc > 0 && !JS_IsUndefined(argv[0])
        ? js_rt_bridge_node_arg(context, bridge, argv[0])
        : lxb_dom_interface_node(bridge->document->html);
    DomDocumentOrderTraversal traversal;
    if (root == NULL || !bridge_document_order_traversal_init(
            bridge, &traversal, root, root)) return array;
    uint32_t count = 0;
    size_t shadow_depth = SIZE_MAX;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && count < DOM_QUERY_RESULT_LIMIT;
         at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, root, &traversal, &shadow_depth)
            || at->type != LXB_DOM_NODE_TYPE_ELEMENT || at->ns != LXB_NS_HTML
            || (at->local_name != LXB_TAG_IFRAME
                && at->local_name != LXB_TAG_FRAME)
            || !bridge_traversal_node_visible(bridge, at, &traversal))
            continue;
        int64_t handle = js_rt_bridge_register_node(bridge, at);
        if (handle == 0) {
            JS_FreeValue(context, array);
            return bridge_throw_handles_exhausted(context);
        }
        if (JS_SetPropertyUint32(context, array, count++,
                                 JS_NewInt64(context, handle)) < 0) {
            JS_FreeValue(context, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

JSValue js_dom_content(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->ns != LXB_NS_HTML || name == NULL || length != 8
        || strncasecmp(name, "template", 8) != 0) {
        return JS_NewInt64(context, 0);
    }
    lxb_html_template_element_t *element = lxb_html_interface_template(node);
    return bridge_node_handle_value(
        context, bridge, lxb_dom_interface_node(element->content));
}

static bool bridge_clear_cloned_node_user_walk(
    lxb_dom_node_t *clone, size_t ownership_depth, size_t *visited)
{
    if (clone == NULL || visited == NULL || ownership_depth >= 8) return false;
    DomDocumentOrderTraversal traversal = {
        .next = clone, .boundary = clone
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if ((*visited)++ >= DOM_TRAVERSAL_VISIT_LIMIT) return false;
        at->user = NULL;
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
                if (content != NULL
                    && !bridge_clear_cloned_node_user_walk(
                           content, ownership_depth + 1, visited)) {
                    return false;
                }
            }
        }
    }
    return traversal.next == NULL;
}

static bool bridge_clear_cloned_node_user(lxb_dom_node_t *clone, bool deep)
{
    if (clone == NULL) return false;
    if (!deep) {
        clone->user = NULL;
        return true;
    }
    size_t visited = 0;
    return bridge_clear_cloned_node_user_walk(clone, 0, &visited);
}

JSValue js_dom_clone(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    bool deep = argc > 1 && JS_ToBool(context, argv[1]) > 0;
    if (deep && node != NULL) {
        lxb_dom_node_t *at = node;
        size_t depth = 0, visited = 0;
        for (;;) {
            if (visited++ >= DOM_TRAVERSAL_VISIT_LIMIT) {
                return JS_ThrowInternalError(
                    context, "DOM clone exceeds the bounded node limit");
            }
            if (at->first_child != NULL) {
                if (depth >= 64) {
                    return JS_ThrowInternalError(
                        context, "DOM clone exceeds the bounded depth limit");
                }
                at = at->first_child;
                depth++;
                continue;
            }
            while (at != node && at->next == NULL) {
                at = at->parent;
                if (depth != 0) depth--;
            }
            if (at == node) break;
            at = at->next;
        }
    }
    document_style_quiet_begin();
    lxb_dom_node_t *clone = node == NULL ? NULL
                                         : lxb_dom_node_clone(node, deep);
    if (clone != NULL
        && (!bridge_clear_cloned_node_user(clone, deep)
            || !document_nonce_clone_subtree(node, clone, deep)
            || !script_element_states_clone_subtree(
                   bridge, node, clone, deep))) {
        lxb_dom_node_destroy_deep(clone);
        clone = NULL;
    }
    document_style_quiet_end();
    return bridge_node_handle_value(context, bridge, clone);
}

JSValue js_dom_attributes(JSContext *context,
                          JSValueConst this_value,
                          int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    JSValue array = JS_NewArray(context);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || JS_IsException(array)) return array;
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    uint32_t index = 0;
    for (lxb_dom_attr_t *attr = element->first_attr; attr != NULL
         && index < 64; attr = attr->next) {
        size_t name_length = 0, value_length = 0;
        const lxb_char_t *name = lxb_dom_attr_qualified_name(attr,
                                                            &name_length);
        const lxb_char_t *value = lxb_dom_attr_value(attr, &value_length);
        JSValue item = JS_NewObject(context);
        if (JS_IsException(item)) break;
        (void) JS_SetPropertyStr(context, item, "name",
            JS_NewStringLen(context, (const char *) name, name_length));
        (void) JS_SetPropertyStr(context, item, "value",
            JS_NewStringLen(context, (const char *) value, value_length));
        (void) JS_SetPropertyUint32(context, array, index++, item);
    }
    return array;
}

JSValue js_dom_create(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *tag = JS_ToCStringLen(context, &length, argv[0]);
    if (tag == NULL) return JS_EXCEPTION;
    bool script = length == 6 && strncasecmp(tag, "script", 6) == 0;
    bool namespace_argument = argc > 1;
    bool has_namespace = namespace_argument
        && !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]);
    size_t namespace_length = 0;
    const char *namespace_uri = has_namespace
        ? JS_ToCStringLen(context, &namespace_length, argv[1]) : NULL;
    if (has_namespace && namespace_uri == NULL) {
        JS_FreeCString(context, tag);
        return JS_EXCEPTION;
    }
    if (namespace_length == 0) {
        if (namespace_uri != NULL) JS_FreeCString(context, namespace_uri);
        namespace_uri = NULL;
        has_namespace = false;
    }
    bool html = has_namespace
        && strcmp(namespace_uri, "http://www.w3.org/1999/xhtml") == 0;
    const char *local_name = tag;
    size_t local_length = length;
    const char *prefix = NULL;
    size_t prefix_length = 0;
    const char *colon = memchr(tag, ':', length);
    if (colon != NULL) {
        prefix = tag;
        prefix_length = (size_t) (colon - tag);
        local_name = colon + 1;
        local_length = length - prefix_length - 1u;
    }
    lxb_dom_document_t *document =
        &bridge->document->html->dom_document;
    lxb_dom_element_t *element = NULL;
    if (length != 0 && length <= 64) {
        element = !namespace_argument
            ? lxb_dom_document_create_element(
                  document, (const lxb_char_t *) tag, length, NULL)
            : lxb_dom_element_create(
                  document,
                  (const lxb_char_t *) local_name, local_length,
                  (const lxb_char_t *) namespace_uri, namespace_length,
                  (const lxb_char_t *) prefix, prefix_length,
                  NULL, 0, true);
    }
    if (namespace_uri != NULL) JS_FreeCString(context, namespace_uri);
    JS_FreeCString(context, tag);
    lxb_dom_node_t *node = element == NULL
        ? NULL : lxb_dom_interface_node(element);
    if (script && argc > 1 && node != NULL
        && js_rt_script_element_state_register(bridge, node, html) == NULL) {
        /* A script without native state would look usable but remain
           permanently inert.  Make the bounded-capacity failure explicit
           to the DOM shim instead. */
        lxb_dom_node_destroy_deep(node);
        node = NULL;
    }
    return bridge_node_handle_value(context, bridge, node);
}

JSValue js_dom_create_fragment(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_document_fragment_t *fragment =
        lxb_dom_document_create_document_fragment(
            &bridge->document->html->dom_document);
    return bridge_node_handle_value(
        context, bridge,
        fragment == NULL ? NULL : lxb_dom_interface_node(fragment));
}

JSValue js_dom_create_text(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *data = JS_ToCStringLen(context, &length, argv[0]);
    if (data == NULL) return JS_EXCEPTION;
    lxb_dom_text_t *node = length <= DOM_TEXT_NODE_LIMIT
        ? lxb_dom_document_create_text_node(
              &bridge->document->html->dom_document,
              (const lxb_char_t *) data, length)
        : NULL;
    JS_FreeCString(context, data);
    return bridge_node_handle_value(
        context, bridge, node == NULL ? NULL : lxb_dom_interface_node(node));
}

JSValue js_dom_create_comment(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_NewInt64(context, 0);
    size_t length = 0;
    const char *data = JS_ToCStringLen(context, &length, argv[0]);
    if (data == NULL) return JS_EXCEPTION;
    lxb_dom_comment_t *node = length <= DOM_INNER_HTML_LIMIT
        ? lxb_dom_document_create_comment(
              &bridge->document->html->dom_document,
              (const lxb_char_t *) data, length)
        : NULL;
    JS_FreeCString(context, data);
    return bridge_node_handle_value(
        context, bridge, node == NULL ? NULL : lxb_dom_interface_node(node));
}

JSValue js_dom_set_custom_state(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 2) return JS_FALSE;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    int32_t state = 0;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || JS_ToInt32(context, &state, argv[1]) < 0) return JS_FALSE;
    lxb_dom_element_t *element = lxb_dom_interface_element(node);
    element->custom_state = state > 0
        ? LXB_DOM_ELEMENT_CUSTOM_STATE_CUSTOM
        : (state < 0 ? LXB_DOM_ELEMENT_CUSTOM_STATE_FAILED
                     : LXB_DOM_ELEMENT_CUSTOM_STATE_UNDEFINED);
    bridge_mutated(bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
                   "data-tilefinch-custom-state",
                   sizeof("data-tilefinch-custom-state") - 1);
    return JS_TRUE;
}

JSValue js_dom_get_custom_state(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || argc < 1) return JS_NewInt32(context, 0);
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return JS_NewInt32(context, 0);
    }
    return JS_NewInt32(
        context, (int) lxb_dom_interface_element(node)->custom_state);
}

#define DOM_TEXT_CONTENT_NODE_LIMIT 200000u
#define DOM_TEXT_CONTENT_BYTE_LIMIT (8u * 1024u * 1024u)

/* The selector traversal has a smaller, silently terminating quota. Text
   reads need their own complete walk: the caller checks the visited count
   and throws rather than publishing a prefix. Parent links are native-owned. */
static lxb_dom_node_t *bridge_text_content_next(lxb_dom_node_t *at,
                                               const lxb_dom_node_t *root)
{
    if (at->first_child != NULL) return at->first_child;
    for (size_t climbed = 0; at != root && at->next == NULL
         && climbed < DOM_TEXT_CONTENT_NODE_LIMIT; climbed++) {
        at = at->parent;
        if (at == NULL) return NULL;
    }
    return at == root ? NULL : at->next;
}

JSValue js_dom_get_text(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL) return JS_NULL;
    const char *root_key = NULL;
    if (bridge->remote_node_read != NULL) {
        if (node == document_body_node(bridge->document)) {
            root_key = "d:body";
        } else {
            size_t tag_length = 0;
            const char *tag = document_element_name(node, &tag_length);
            if (tag != NULL && tag_length == 4
                && strncasecmp(tag, "html", 4) == 0) root_key = "d:html";
        }
    }
    if (root_key != NULL) {
        ScriptRemoteNodeReadResult read = {0};
        bool success = bridge->remote_node_read(
            bridge->remote_node_read_opaque, root_key, strlen(root_key),
            bridge->section_identity, SCRIPT_REMOTE_NODE_TEXT,
            NULL, 0, &read);
        if (!success || read.is_null) {
            js_rt_remote_node_read_result_destroy(bridge, &read);
            return JS_ThrowInternalError(
                context, "remote document text read failed");
        }
        JSValue value = JS_NewStringLen(
            context, read.value == NULL ? "" : read.value, read.length);
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return value;
    }
    /* Do not put read-only scratch in Lexbor's retained text arena. In
       particular, a large style.textContent read used to leave another arena
       chunk resident after its temporary string was freed. No author code is
       invoked while these borrowed bytes are copied into the JS string. */
    lxb_dom_node_t *text_node = node;
    if ((node->type == LXB_DOM_NODE_TYPE_ELEMENT
         || node->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)
        && node->first_child != NULL && node->first_child == node->last_child
        && node->first_child->type == LXB_DOM_NODE_TYPE_TEXT)
        text_node = node->first_child;
    if (text_node->type == LXB_DOM_NODE_TYPE_TEXT
        || text_node->type == LXB_DOM_NODE_TYPE_COMMENT
        || text_node->type == LXB_DOM_NODE_TYPE_PROCESSING_INSTRUCTION) {
        const lexbor_str_t *data =
            &lxb_dom_interface_character_data(text_node)->data;
        return JS_NewStringLen(context,
            data->data == NULL ? "" : (const char *) data->data, data->length);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)
    {
        if (node->type == LXB_DOM_NODE_TYPE_ATTRIBUTE) {
            size_t length = 0;
            const lxb_char_t *text = lxb_dom_attr_value(lxb_dom_interface_attr(node), &length);
            return JS_NewStringLen(context, text == NULL ? "" : (const char *) text, length);
        }
        return JS_NewString(context, "");
    }

    /* Complete results or an explicit quota error, never a silent prefix.
       These bounds cover the admitted document and the existing rendered-text
       traversal ceiling while keeping hostile script-built trees bounded. */
    size_t length = 0, visited = 0;
    for (lxb_dom_node_t *at = node; at != NULL;
         at = bridge_text_content_next(at, node)) {
        if (++visited > DOM_TEXT_CONTENT_NODE_LIMIT)
            return JS_ThrowRangeError(context, "textContent node quota exceeded");
        if (at->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        size_t bytes = lxb_dom_interface_text(at)->char_data.data.length;
        if (bytes > DOM_TEXT_CONTENT_BYTE_LIMIT - length)
            return JS_ThrowRangeError(context, "textContent byte quota exceeded");
        length += bytes;
    }
    char local[512];
    char *text = length <= sizeof(local) ? local
        : budget_malloc(bridge->budget, length);
    if (text == NULL) return JS_ThrowOutOfMemory(context);
    size_t used = 0;
    for (lxb_dom_node_t *at = node; at != NULL;
         at = bridge_text_content_next(at, node)) {
        if (at->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        const lexbor_str_t *data = &lxb_dom_interface_text(at)->char_data.data;
        if (data->length != 0) memcpy(text + used, data->data, data->length);
        used += data->length;
    }
    JSValue value = JS_NewStringLen(context, text, length);
    if (text != local) budget_free(bridge->budget, text);
    return value;
}

#define DOM_TEXT_PREFIX_BYTE_LIMIT (256u * 1024u)
#define DOM_TEXT_PREFIX_NODE_LIMIT 4096u

static size_t bridge_utf8_prefix_length(
    const unsigned char *text, size_t length, size_t maximum_bytes)
{
    if (text == NULL || length == 0 || maximum_bytes == 0) return 0;
    size_t take = length < maximum_bytes ? length : maximum_bytes;
    if (take < length) {
        while (take > 0 && (text[take] & 0xc0u) == 0x80u) take--;
    }
    return take;
}

static JSValue bridge_text_prefix_result(
    JSContext *context, JSValue text, uint32_t visited_nodes,
    uint32_t copied_bytes)
{
    if (JS_IsException(text)) return text;
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)
        || JS_SetPropertyStr(context, result, "text", text) < 0
        || JS_SetPropertyStr(context, result, "nodes",
                             JS_NewUint32(context, visited_nodes)) < 0
        || JS_SetPropertyStr(context, result, "bytes",
                             JS_NewUint32(context, copied_bytes)) < 0) {
        if (JS_IsException(result)) JS_FreeValue(context, text);
        else JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* Bootstrap consumers sometimes need only a bounded prefix of a large style
   node.  Keep Node.textContent standards-observable and complete; this private
   primitive walks the native tree and never materializes the omitted suffix in
   either the Budget or QuickJS heap. Report the bytes copied as well as nodes
   visited so consumers can enforce their aggregate native-input quota without
   another JavaScript pass over every UTF-16 code unit. */
JSValue js_dom_get_text_prefix(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    uint32_t maximum_bytes = 0;
    uint32_t maximum_nodes = 0;
    if (bridge == NULL || argc < 3) return JS_NULL;
    /* Coercion may run author code. Resolve the native node only after every
       coercion so a hostile valueOf() cannot detach a previously resolved
       pointer underneath this bootstrap helper. */
    if (JS_ToUint32(context, &maximum_bytes, argv[1]) < 0
        || JS_ToUint32(context, &maximum_nodes, argv[2]) < 0) {
        return JS_EXCEPTION;
    }
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    if (node == NULL) return JS_NULL;
    if (maximum_bytes > DOM_TEXT_PREFIX_BYTE_LIMIT) {
        maximum_bytes = DOM_TEXT_PREFIX_BYTE_LIMIT;
    }
    if (maximum_nodes > DOM_TEXT_PREFIX_NODE_LIMIT) {
        maximum_nodes = DOM_TEXT_PREFIX_NODE_LIMIT;
    }
    if (maximum_bytes == 0 || maximum_nodes == 0) {
        return bridge_text_prefix_result(
            context, JS_NewString(context, ""), 0, 0);
    }

    size_t required = 0;
    uint32_t visited = 0;
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && visited < maximum_nodes && required < maximum_bytes;
         at = dom_document_order_next(&traversal)) {
        visited++;
        size_t text_length = 0;
        const unsigned char *text = (const unsigned char *)
            document_text_data(at, &text_length);
        if (text == NULL || text_length == 0) continue;
        size_t take = bridge_utf8_prefix_length(
            text, text_length, maximum_bytes - required);
        if (take == 0) break;
        required += take;
        if (take < text_length) break;
    }
    if (required == 0) {
        return bridge_text_prefix_result(
            context, JS_NewString(context, ""), visited, 0);
    }
    const uint32_t consumed_nodes = visited;

    unsigned char *prefix = budget_malloc(bridge->budget, required);
    if (prefix == NULL) return JS_NULL;
    size_t used = 0;
    visited = 0;
    traversal = (DomDocumentOrderTraversal) {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && visited < maximum_nodes && used < required;
         at = dom_document_order_next(&traversal)) {
        visited++;
        size_t text_length = 0;
        const unsigned char *text = (const unsigned char *)
            document_text_data(at, &text_length);
        if (text == NULL || text_length == 0) continue;
        size_t take = text_length;
        if (take > required - used) take = required - used;
        memcpy(prefix + used, text, take);
        used += take;
    }
    JSValue value = JS_NewStringLen(
        context, (const char *) prefix, used);
    budget_free(bridge->budget, prefix);
    return bridge_text_prefix_result(context, value, consumed_nodes,
                                     (uint32_t) used);
}

JSValue js_dom_get_style_attribute_prefix(
    JSContext *context, JSValueConst this_value,
    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    uint32_t maximum_bytes = 0;
    if (bridge == NULL || argc < 2) return JS_NULL;
    /* This coercion can execute author code, so resolve the handle last. */
    if (JS_ToUint32(context, &maximum_bytes, argv[1]) < 0) {
        return JS_EXCEPTION;
    }
    lxb_dom_node_t *node = js_rt_bridge_node_arg(
        context, bridge, argv[0]);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return JS_NULL;
    }
    if (maximum_bytes > DOM_TEXT_PREFIX_BYTE_LIMIT) {
        maximum_bytes = DOM_TEXT_PREFIX_BYTE_LIMIT;
    }
    size_t value_length = 0;
    const unsigned char *value = (const unsigned char *)
        document_attribute(node, "style", &value_length);
    if (value == NULL) return JS_NULL;
    size_t take = bridge_utf8_prefix_length(
        value, value_length, maximum_bytes);
    return JS_NewStringLen(context, (const char *) value, take);
}

JSValue js_dom_set_text(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || argc < 2) return JS_FALSE;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[1]);
    if (text == NULL) return JS_EXCEPTION;
    lxb_status_t status = LXB_STATUS_OK;
    /* The TEXT note below covers every write here. Brackets stay around
       Lexbor calls only: discarding a subtree can run JavaScript cleanup,
       whose native side effects must still count. */
    if (node->type == LXB_DOM_NODE_TYPE_TEXT
        || node->type == LXB_DOM_NODE_TYPE_COMMENT) {
        document_style_quiet_begin();
        status = lxb_dom_node_text_content_set(
            node, (const lxb_char_t *) text, length);
        document_style_quiet_end();
    } else {
        /* Element.textContent detaches the previous children; it must not
           destroy Node objects still referenced by author JavaScript.
           Allocate the replacement first so an OOM leaves the old tree
           untouched, then reclaim only detached subtrees with no live JS or
           native identity. */
        lxb_dom_text_t *replacement = length == 0 ? NULL
            : lxb_dom_document_create_text_node(
                &bridge->document->html->dom_document,
                (const lxb_char_t *) text, length);
        if (length != 0 && replacement == NULL) {
            status = LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        } else {
            while (node->first_child != NULL) {
                lxb_dom_node_t *removed = node->first_child;
                document_style_quiet_begin();
                lxb_dom_node_remove(removed);
                document_style_quiet_end();
                (void) bridge_discard_unretained_detached_subtree(
                    bridge, removed);
            }
            document_style_quiet_begin();
            if (replacement != NULL
                && lxb_dom_node_append_child(
                       node, lxb_dom_interface_node(replacement))
                       != LXB_DOM_EXCEPTION_OK) {
                lxb_dom_node_destroy_deep(
                    lxb_dom_interface_node(replacement));
                status = LXB_STATUS_ERROR;
            }
            document_style_quiet_end();
        }
    }
    JS_FreeCString(context, text);
    if (status == LXB_STATUS_OK) {
        bridge_mutated(bridge, SCRIPT_MUTATION_TEXT, node, NULL, 0);
        lxb_dom_node_t *preparation_root = node;
        for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
            if (js_rt_script_element_state_find(bridge, at) != NULL) {
                preparation_root = at;
                break;
            }
        }
        (void) js_rt_dynamic_prepare_subtree(context, preparation_root);
    }
    return JS_NewBool(context, status == LXB_STATUS_OK);
}

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    bool failed;
} InnerHTMLBuffer;

static lxb_status_t inner_html_receive(const lxb_char_t *data, size_t length,
                                       void *opaque)
{
    InnerHTMLBuffer *buffer = opaque;
    if (buffer->failed || buffer->length + length > DOM_INNER_HTML_LIMIT) {
        buffer->failed = true;
        return LXB_STATUS_ERROR;
    }
    char *resized = budget_realloc(buffer->budget, buffer->data,
                                   buffer->length + length + 1);
    if (resized == NULL) {
        buffer->failed = true;
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }
    buffer->data = resized;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return LXB_STATUS_OK;
}

JSValue js_dom_get_inner_html(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)) {
        return JS_NULL;
    }
    InnerHTMLBuffer buffer = {.budget = bridge->document->budget};
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        if (lxb_html_serialize_tree_cb(child, inner_html_receive, &buffer)
            != LXB_STATUS_OK) {
            buffer.failed = true;
            break;
        }
    }
    JSValue value = buffer.failed
                    ? JS_ThrowInternalError(context,
                                            "innerHTML serialization limit")
                    : JS_NewStringLen(context,
                                      buffer.data == NULL ? "" : buffer.data,
                                      buffer.length);
    budget_free(buffer.budget, buffer.data);
    return value;
}

/* `context_name`, when given, replaces the target's own name as the
   fragment parser's context element (Range.createContextualFragment parses
   into a new DocumentFragment, which has none) and leaves parsed scripts
   unstarted instead of inert. */
static bool bridge_replace_inner_html(
    DomBridge *bridge, lxb_dom_node_t *node,
    const char *html, size_t length, bool prepare_dynamic_scripts,
    const char *context_override, size_t context_override_length)
{
    if (bridge == NULL || bridge->document == NULL || node == NULL
        || (node->type != LXB_DOM_NODE_TYPE_ELEMENT
            && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)
        || (html == NULL && length != 0)
        || length > DOM_INNER_HTML_LIMIT) return false;
    static const char div_name[] = "div";
    size_t context_name_length = sizeof(div_name) - 1u;
    const char *context_name = div_name;
    if (context_override != NULL) {
        context_name = context_override;
        context_name_length = context_override_length;
    } else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
        context_name = document_element_name(node, &context_name_length);
    /* Every connected change below is published as one INNER_HTML note.
       Brackets stay around Lexbor calls only (see js_dom_set_text). */
    document_style_quiet_begin();
    lxb_dom_element_t *container = context_name == NULL ? NULL
        : lxb_dom_document_create_element(
              &bridge->document->html->dom_document,
              (const lxb_char_t *) context_name, context_name_length, NULL);
    lxb_dom_node_t *container_node = container == NULL ? NULL
        : lxb_dom_interface_node(container);
    bool valid = container_node != NULL
        && document_set_element_inner_html(
               bridge->document, container_node,
               html == NULL ? "" : html, length);
    document_style_quiet_end();
    bool changed = false;
    if (valid) {
        bridge_detach_and_discard_children(bridge, node);
        changed = true;
        document_style_quiet_begin();
        while (container_node->first_child != NULL) {
            lxb_dom_node_t *child = container_node->first_child;
            lxb_dom_node_remove(child);
            if (lxb_dom_node_append_child(node, child)
                != LXB_DOM_EXCEPTION_OK) {
                valid = false;
                break;
            }
        }
        document_style_quiet_end();
    }
    document_style_quiet_begin();
    if (container_node != NULL) lxb_dom_node_destroy_deep(container_node);
    document_style_quiet_end();
    if (changed) {
        /* innerHTML's fragment parsing does not execute scripts. Register
           every restored source node before publishing the mutation. */
        if (!script_element_states_register_parsed_subtree(
                bridge, node, context_override != NULL)) valid = false;
        bridge_mutated(
            bridge, SCRIPT_MUTATION_INNER_HTML, node, NULL, 0);
        if (prepare_dynamic_scripts)
            (void) js_rt_dynamic_prepare_subtree(
                bridge->host->context, node);
    }
    return valid;
}

bool script_runtime_replace_document_body(
    ScriptRuntime *runtime, PocDocument *document,
    const char *markup, size_t length)
{
    if (runtime == NULL || document == NULL
        || runtime->document != document
        || runtime->bridge.document != document) return false;
    lxb_dom_node_t *body = document_body_node(document);
    return bridge_replace_inner_html(
        &runtime->bridge, body, markup, length, false, NULL, 0);
}

JSValue js_dom_set_inner_html(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) || argc < 2) {
        return JS_FALSE;
    }
    /* (fragment, html, contextName): Range.createContextualFragment's
       parse into an empty DocumentFragment it just created. */
    const char *context_name = NULL;
    size_t context_length = 0;
    if (argc > 2) {
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT
            || node->first_child != NULL) return JS_FALSE;
        context_name = JS_ToCStringLen(context, &context_length, argv[2]);
        if (context_name == NULL) return JS_EXCEPTION;
        if (context_length == 0 || context_length > 64) {
            JS_FreeCString(context, context_name);
            return JS_FALSE;
        }
    }
    size_t length = 0;
    const char *html = JS_ToCStringLen(context, &length, argv[1]);
    bool valid = html != NULL && bridge_replace_inner_html(
        bridge, node, html, length, context_name == NULL,
        context_name, context_length);
    if (context_name != NULL) JS_FreeCString(context, context_name);
    if (html == NULL) return JS_EXCEPTION;
    JS_FreeCString(context, html);
    return JS_NewBool(context, valid);
}

JSValue js_dom_get_attribute(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT || argc < 2) {
        return JS_NULL;
    }
    size_t name_length = 0;
    const char *name = JS_ToCStringLen(context, &name_length, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    size_t value_length = 0;
    const lxb_char_t *value = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, &value_length);
    bool present = value != NULL || lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    JS_FreeCString(context, name);
    return !present ? JS_NULL
                    : JS_NewStringLen(context,
                                      value == NULL ? ""
                                                    : (const char *) value,
                                      value == NULL ? 0 : value_length);
}

JSValue js_dom_image_property(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = bridge != NULL && argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    int32_t property = -1;
    if (node == NULL || argc < 2
        || JS_ToInt32(context, &property, argv[1]) < 0
        || !bridge_mutation_node_name_is(node, "img")) {
        return property == 0 ? JS_NewString(context, "")
                             : JS_NewInt32(context, 0);
    }
    if (property == 0) {
        size_t length = 0;
        const char *source = image_select_source(
            bridge->stylesheet, node, &length);
        return source == NULL ? JS_NewString(context, "")
                              : JS_NewStringLen(context, source, length);
    }
    const ImageResource *image = images_find_node(bridge->images, node);
    bool available = image_resource_available(image);
    if (property == 1) {
        return JS_NewInt32(context, available ? image->source_width : 0);
    }
    if (property == 2) {
        return JS_NewInt32(context, available ? image->source_height : 0);
    }
    if (property == 3) {
        size_t length = 0;
        const char *source = image_select_source(
            bridge->stylesheet, node, &length);
        return JS_NewBool(context,
                          source == NULL || length == 0 || available);
    }
    return JS_NewInt32(context, 0);
}

static bool js_dom_latch_checked_default(DomBridge *bridge,
                                         lxb_dom_node_t *node,
                                         const char *name,
                                         size_t name_length)
{
    if (bridge == NULL || bridge->document == NULL || node == NULL
        || name == NULL || name_length != sizeof("checked") - 1u
        || strncasecmp(name, "checked", name_length) != 0
        || !bridge_mutation_node_name_is(node, "input")) return true;
    size_t type_length = 0;
    const char *type = document_attribute(node, "type", &type_length);
    bool checkable = type != NULL
        && ((type_length == sizeof("checkbox") - 1u
             && strncasecmp(type, "checkbox", type_length) == 0)
            || (type_length == sizeof("radio") - 1u
                && strncasecmp(type, "radio", type_length) == 0));
    if (!checkable) return true;
    bool ignored_default = false;
    return document_control_checked_default(
        bridge->document, node,
        lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "checked", sizeof("checked") - 1u),
        &ignored_default);
}

/* The answer js_rt_script_element_parser_started infers for a host-parser
   script reads its type, which the author can change without starting or
   un-starting the element (consent managers retype text/plain scripts and
   clone them). Record the flag the parser left before the type moves. The
   state stays parser-inserted, as a stateless script is to CSP. */
static void script_element_state_pin_parser_started(
    DomBridge *bridge, lxb_dom_node_t *node,
    const char *name, size_t name_length)
{
    if (bridge == NULL || node == NULL || name_length != 4
        || strncasecmp(name, "type", 4) != 0 || node->ns != LXB_NS_HTML
        || node->local_name != LXB_TAG_SCRIPT
        || js_rt_script_element_state_find(bridge, node) != NULL) return;
    bool started = js_rt_script_element_parser_started(node);
    ScriptElementState *state =
        js_rt_script_element_state_register(bridge, node, true);
    if (state == NULL) return;
    state->programmatic = false;
    state->force_async = false;
    state->already_started = started;
}

JSValue js_dom_set_attribute(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT || argc < 3) {
        return JS_FALSE;
    }
    size_t name_length = 0, value_length = 0;
    const char *name = JS_ToCStringLen(context, &name_length, argv[1]);
    const char *value = JS_ToCStringLen(context, &value_length, argv[2]);
    if (name == NULL || value == NULL) {
        if (name != NULL) JS_FreeCString(context, name);
        if (value != NULL) JS_FreeCString(context, value);
        return JS_EXCEPTION;
    }
    if (!js_dom_latch_checked_default(
            bridge, node, name, name_length)) {
        JS_FreeCString(context, value);
        JS_FreeCString(context, name);
        return JS_FALSE;
    }
    size_t old_value_length = 0;
    const lxb_char_t *old_value = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, &old_value_length);
    bool old_present = old_value != NULL || lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    /* Timed only for the profiler; the counts are free. */
    bool timed = bridge != NULL && bridge->host != NULL
        && bridge->host->profile != NULL;
    uint64_t write_started_ns = timed ? js_rt_monotonic_time_ns() : 0;
    if (bridge != NULL) {
        bridge->attribute_writes++;
        if (old_present && old_value_length == value_length
            && (value_length == 0
                || memcmp(old_value, value, value_length) == 0))
            bridge->attribute_writes_unchanged++;
    }
    /* Writing the same id, class, inline style, data-* or aria-* value
       again changes nothing a selector, declaration or layout reads (pages
       re-apply identical root style blocks on every render). Keep the DOM
       setter and JavaScript observer/custom-element delivery, but do not
       invalidate the style/layout tree for that no-op. A style write also
       revokes CSSOM authorization, so it is a no-op only when that was
       already revoked. Resource and form attributes retain their existing
       setter effects. */
    bool unchanged_selector_identity = old_present
        && old_value_length == value_length
        && (value_length == 0
            || memcmp(old_value, value, value_length) == 0)
        && ((name_length == 2 && strncasecmp(name, "id", 2) == 0)
            || (name_length == 5 && strncasecmp(name, "class", 5) == 0)
            || (name_length == 5 && strncasecmp(name, "style", 5) == 0
                && !document_style_attribute_cssom_authorized(node))
            || (name_length > 5 && strncasecmp(name, "data-", 5) == 0)
            || (name_length > 5 && strncasecmp(name, "aria-", 5) == 0));
    bool relational_selector_sensitive =
        !unchanged_selector_identity && stylesheet_attribute_change_may_affect_has(
            bridge == NULL ? NULL : bridge->stylesheet,
            name, name_length,
            old_present ? (old_value == NULL ? "" : (const char *) old_value)
                        : NULL,
            old_present ? old_value_length : 0,
            value, value_length);
    uint32_t changed_tokens[SCRIPT_MUTATION_TOKEN_LIMIT];
    size_t changed_token_count = 0;
    bool changed_tokens_exact = !unchanged_selector_identity
        && stylesheet_attribute_change_tokens(
            name, name_length,
            old_present ? (old_value == NULL ? "" : (const char *) old_value)
                        : NULL,
            old_present ? old_value_length : 0,
            value, value_length, changed_tokens,
            SCRIPT_MUTATION_TOKEN_LIMIT, &changed_token_count);
    script_element_state_pin_parser_started(bridge, node, name, name_length);
    document_style_quiet_begin();
    lxb_dom_attr_t *attribute = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, (const lxb_char_t *) value, value_length);
    document_style_quiet_end();
    bool async_attribute = name_length == 5
        && strncasecmp(name, "async", 5) == 0;
    bool source_attribute = name_length == 3
        && strncasecmp(name, "src", 3) == 0;
    if (attribute != NULL) {
        if (name_length == 5 && strncasecmp(name, "style", 5) == 0) {
            document_style_attribute_set_cssom_authorized(node, false);
        }
        if (!unchanged_selector_identity) {
            bridge_mutated_with_relational(
                bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
                name, name_length, relational_selector_sensitive,
                changed_tokens_exact ? changed_tokens : NULL,
                changed_token_count);
        }
        ScriptElementState *state = js_rt_script_element_state_find(bridge, node);
        if (async_attribute && state != NULL && state->programmatic
            && state->html) {
            state->force_async = false;
        }
        if (source_attribute) {
            (void) js_rt_dynamic_prepare_subtree(context, node);
        }
    }
    JS_FreeCString(context, value);
    JS_FreeCString(context, name);
    if (timed) {
        uint64_t finished_ns = js_rt_monotonic_time_ns();
        if (finished_ns > write_started_ns)
            bridge->attribute_write_ns += finished_ns - write_started_ns;
    }
    return JS_NewBool(context, attribute != NULL);
}

JSValue js_dom_set_control_value(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL || argc < 2) {
        return JS_FALSE;
    }
    size_t length = 0;
    const char *value = JS_ToCStringLen(context, &length, argv[1]);
    if (value == NULL) return JS_EXCEPTION;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (bridge->document == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        JS_FreeCString(context, value);
        return JS_FALSE;
    }
    size_t old_length = 0;
    const char *old = document_control_value(node, &old_length);
    if (old != NULL && old_length == length
        && (length == 0 || memcmp(old, value, length) == 0)) {
        /* Native text entry already published these exact live bytes before
           dispatching input. Synchronizing the JS wrapper must not invalidate
           the entire page again. A first write still establishes dirty/default
           state even when it matches the authored value attribute. */
        JS_FreeCString(context, value);
        return JS_TRUE;
    }
    bool set = document_control_value_set(
        bridge->document, node, value, length);
    if (set) {
        /* A live value can change both intrinsic text width and pixels while
           leaving the authored value attribute untouched. Reuse the normal
           bounded mutation path to invalidate layout and paint. */
        bridge_mutated(
            bridge, SCRIPT_MUTATION_ATTRIBUTE, node, "value", 5);
    }
    JS_FreeCString(context, value);
    return JS_NewBool(context, set);
}

JSValue js_dom_parser_form_owner(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    lxb_dom_node_t *owner =
        document_control_parser_form_owner(node);
    if (bridge == NULL || owner == NULL) return JS_NULL;
    return bridge_node_handle_value(context, bridge, owner);
}

/* A value that changes whenever the DOM may have: every script mutation
   plus the parser's growth (node count). Realm caches compare it. */
JSValue js_dom_version(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL) return JS_NewFloat64(context, -1.0);
    double nodes = bridge->document == NULL
        ? 0.0 : (double) bridge->document->node_count;
    /* A truthy argument asks for the structural version (form.elements). */
    uint64_t version = argc > 0 && JS_ToBool(context, argv[0]) > 0
        ? bridge->dom_structure_version : bridge->dom_version;
    return JS_NewFloat64(context, (double) version * 16777216.0 + nodes);
}

JSValue js_dom_has_parser_form_owners(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewBool(
        context, bridge != NULL
            && document_has_parser_form_owners(bridge->document));
}

JSValue js_runtime_heap_remaining(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    (void) argc;
    (void) argv;
    /* O(1) from the allocator-maintained count; no heap census. */
    DomBridge *bridge = JS_GetContextOpaque(context);
    size_t remaining = script_runtime_heap_remaining(
        bridge == NULL ? NULL : bridge->host);
    return JS_NewInt64(context, (int64_t) remaining);
}

JSValue js_dom_remove_attribute(JSContext *context,
                                JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || argc < 2) return JS_FALSE;
    size_t name_length = 0;
    const char *name = JS_ToCStringLen(context, &name_length, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    if (!js_dom_latch_checked_default(
            bridge, node, name, name_length)) {
        JS_FreeCString(context, name);
        return JS_FALSE;
    }
    bool existed = lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    size_t old_value_length = 0;
    const lxb_char_t *old_value = existed
        ? lxb_dom_element_get_attribute(
              lxb_dom_interface_element(node), (const lxb_char_t *) name,
              name_length, &old_value_length)
        : NULL;
    bool relational_selector_sensitive =
        stylesheet_attribute_change_may_affect_has(
            bridge == NULL ? NULL : bridge->stylesheet,
            name, name_length,
            existed ? (old_value == NULL ? "" : (const char *) old_value)
                    : NULL,
            existed ? old_value_length : 0, NULL, 0);
    uint32_t changed_tokens[SCRIPT_MUTATION_TOKEN_LIMIT];
    size_t changed_token_count = 0;
    bool changed_tokens_exact = stylesheet_attribute_change_tokens(
        name, name_length,
        existed ? (old_value == NULL ? "" : (const char *) old_value) : NULL,
        existed ? old_value_length : 0, NULL, 0, changed_tokens,
        SCRIPT_MUTATION_TOKEN_LIMIT, &changed_token_count);
    if (existed) {
        script_element_state_pin_parser_started(
            bridge, node, name, name_length);
    }
    document_style_quiet_begin();
    lxb_status_t status = lxb_dom_element_remove_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    document_style_quiet_end();
    if (existed && status == LXB_STATUS_OK) {
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
            name, name_length, relational_selector_sensitive,
            changed_tokens_exact ? changed_tokens : NULL,
            changed_token_count);
    }
    JS_FreeCString(context, name);
    return JS_NewBool(context, status == LXB_STATUS_OK);
}

static bool property_equal(const char *first, size_t first_length,
                           const char *second, size_t second_length)
{
    if (first_length != second_length) return false;
    for (size_t i = 0; i < first_length; i++) {
        if (tolower((unsigned char) first[i])
            != tolower((unsigned char) second[i])) return false;
    }
    return true;
}

static uint32_t sparse_modern_property_mask(const char *name, size_t length)
{
#define MODERN_PROPERTY(wanted, bit) \
    if (property_equal(name, length, wanted, sizeof(wanted) - 1u)) return bit
    MODERN_PROPERTY("user-select", STYLE_MODERN_USER_SELECT);
    MODERN_PROPERTY("-webkit-user-select", STYLE_MODERN_USER_SELECT);
    MODERN_PROPERTY("touch-action", STYLE_MODERN_TOUCH_ACTION);
    MODERN_PROPERTY("text-size-adjust", STYLE_MODERN_TEXT_SIZE_ADJUST);
    MODERN_PROPERTY("-webkit-text-size-adjust",
                    STYLE_MODERN_TEXT_SIZE_ADJUST);
    MODERN_PROPERTY("resize", STYLE_MODERN_RESIZE);
    MODERN_PROPERTY("text-wrap", STYLE_MODERN_TEXT_WRAP);
    MODERN_PROPERTY("text-wrap-style", STYLE_MODERN_TEXT_WRAP);
    MODERN_PROPERTY("translate", STYLE_MODERN_TRANSLATE);
    MODERN_PROPERTY("rotate", STYLE_MODERN_ROTATE);
    MODERN_PROPERTY("scale", STYLE_MODERN_SCALE);
    MODERN_PROPERTY("isolation", STYLE_MODERN_ISOLATION);
    MODERN_PROPERTY("hyphens", STYLE_MODERN_TYPOGRAPHY);
    MODERN_PROPERTY("tab-size", STYLE_MODERN_TYPOGRAPHY);
    MODERN_PROPERTY("font-kerning", STYLE_MODERN_TYPOGRAPHY);
    MODERN_PROPERTY("text-rendering", STYLE_MODERN_TYPOGRAPHY);
    MODERN_PROPERTY("mix-blend-mode", STYLE_MODERN_MIX_BLEND);
    MODERN_PROPERTY("backdrop-filter", STYLE_MODERN_BACKDROP_FILTER);
    MODERN_PROPERTY("-webkit-backdrop-filter", STYLE_MODERN_BACKDROP_FILTER);
    MODERN_PROPERTY("backface-visibility", STYLE_MODERN_BACKFACE_VISIBILITY);
    MODERN_PROPERTY("transform-style", STYLE_MODERN_TRANSFORM_STYLE);
    MODERN_PROPERTY("color-scheme", STYLE_MODERN_COLOR_SCHEME);
    if (length >= 12u && strncasecmp(name, "border-image", 12u) == 0)
        return STYLE_MODERN_BORDER_IMAGE;
    if (length >= 18u
        && strncasecmp(name, "border-", 7u) == 0
        && strncasecmp(name + length - 7u, "-radius", 7u) == 0) {
        return STYLE_MODERN_LOGICAL_RADIUS;
    }
#undef MODERN_PROPERTY
    return 0;
}

static uint8_t sparse_modern_typography_mask(
    const char *name, size_t length)
{
#define TYPOGRAPHY_PROPERTY(wanted, bit) \
    if (length == sizeof(wanted) - 1u \
        && strncasecmp(name, wanted, sizeof(wanted) - 1u) == 0) return bit
    TYPOGRAPHY_PROPERTY("hyphens", STYLE_MODERN_TYPOGRAPHY_HYPHENS);
    TYPOGRAPHY_PROPERTY("tab-size", STYLE_MODERN_TYPOGRAPHY_TAB_SIZE);
    TYPOGRAPHY_PROPERTY("font-kerning", STYLE_MODERN_TYPOGRAPHY_KERNING);
    TYPOGRAPHY_PROPERTY(
        "text-rendering", STYLE_MODERN_TYPOGRAPHY_TEXT_RENDERING);
#undef TYPOGRAPHY_PROPERTY
    return 0;
}

static bool retained_scroll_box_shorthand(
    const Stylesheet *stylesheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t capacity)
{
    if (!property_equal(name, name_length, "scroll-margin", 13)
        && !property_equal(name, name_length, "scroll-padding", 14)) {
        return false;
    }
    char shorthand[15];
    if (name_length >= sizeof(shorthand)) return false;
    memcpy(shorthand, name, name_length);
    shorthand[name_length] = '\0';
    StyleRetainedBoxValues values;
    if (!style_retained_box_values(
            stylesheet, node, shorthand, &values)
        || values.present_mask != 0x0fu) return false;
    const char *top = values.values[0], *right = values.values[1];
    const char *bottom = values.values[2], *left = values.values[3];
    int written;
    if (strcmp(top, right) == 0 && strcmp(top, bottom) == 0
        && strcmp(top, left) == 0) {
        written = snprintf(output, capacity, "%s", top);
    } else if (strcmp(top, bottom) == 0 && strcmp(right, left) == 0) {
        written = snprintf(output, capacity, "%s %s", top, right);
    } else if (strcmp(right, left) == 0) {
        written = snprintf(
            output, capacity, "%s %s %s", top, right, bottom);
    } else {
        written = snprintf(
            output, capacity, "%s %s %s %s",
            top, right, bottom, left);
    }
    return written >= 0 && (size_t) written < capacity;
}

static int serialize_computed_color(char *output, size_t capacity,
                                    uint32_t color, unsigned alpha)
{
    unsigned red = (unsigned) ((color >> 16) & 255u);
    unsigned green = (unsigned) ((color >> 8) & 255u);
    unsigned blue = (unsigned) (color & 255u);
    return alpha == 255
        ? snprintf(output, capacity, "rgb(%u, %u, %u)",
                   red, green, blue)
        : snprintf(output, capacity, "rgba(%u, %u, %u, %.3g)",
                   red, green, blue, (double) alpha / 255.0);
}

/* One declaration of an inline style attribute. A `;` inside quotes or
   parentheses (`url(data:image/png;base64,...)`) does not end it. */
typedef struct {
    size_t start, end;             /* the whole declaration, without `;` */
    size_t name_start, name_end;   /* trimmed; name_end == start if none */
    size_t value_start, value_end; /* trimmed */
    bool has_colon;
} InlineDeclaration;

static bool inline_declaration_next(const char *style, size_t length,
                                    size_t *at, InlineDeclaration *out)
{
    if (style == NULL || *at >= length) return false;
    size_t start = *at, end = start;
    int parentheses = 0;
    char quote = '\0';
    while (end < length) {
        char character = style[end];
        if (quote != '\0') {
            if (character == quote && style[end - 1] != '\\') quote = '\0';
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '(') {
            parentheses++;
        } else if (character == ')' && parentheses > 0) {
            parentheses--;
        } else if (character == ';' && parentheses == 0) {
            break;
        }
        end++;
    }
    size_t colon = start;
    while (colon < end && style[colon] != ':') colon++;
    InlineDeclaration declaration = {
        .start = start, .end = end,
        .name_start = start, .name_end = colon,
        .value_start = colon < end ? colon + 1 : end, .value_end = end,
        .has_colon = colon < end
    };
    while (declaration.name_start < declaration.name_end
           && isspace((unsigned char) style[declaration.name_start]))
        declaration.name_start++;
    while (declaration.name_end > declaration.name_start
           && isspace((unsigned char) style[declaration.name_end - 1]))
        declaration.name_end--;
    while (declaration.value_start < declaration.value_end
           && isspace((unsigned char) style[declaration.value_start]))
        declaration.value_start++;
    while (declaration.value_end > declaration.value_start
           && isspace((unsigned char) style[declaration.value_end - 1]))
        declaration.value_end--;
    *out = declaration;
    *at = end + (end < length);
    return true;
}

static bool inline_declaration_named(const char *style,
                                     const InlineDeclaration *declaration,
                                     const char *wanted, size_t wanted_length)
{
    return declaration->has_colon
        && property_equal(style + declaration->name_start,
                          declaration->name_end - declaration->name_start,
                          wanted, wanted_length);
}

JSValue js_style_get(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || argc < 2) return JS_NewString(context, "");
    size_t wanted_length = 0;
    const char *wanted = JS_ToCStringLen(context, &wanted_length, argv[1]);
    if (wanted == NULL) return JS_EXCEPTION;
    size_t style_length = 0;
    const char *style = document_attribute(node, "style", &style_length);
    /* The last declaration of a name wins, as in the cascade. The raw
       value keeps any !important; script strips the priority itself. */
    size_t value_start = 0, value_end = 0;
    bool found = false;
    InlineDeclaration declaration;
    for (size_t at = 0;
         inline_declaration_next(style, style_length, &at, &declaration);) {
        if (!inline_declaration_named(style, &declaration,
                                      wanted, wanted_length)) continue;
        value_start = declaration.value_start;
        value_end = declaration.value_end;
        found = true;
    }
    JSValue result = found
        ? JS_NewStringLen(context, style + value_start,
                          value_end - value_start)
        : JS_NewString(context, "");
    JS_FreeCString(context, wanted);
    return result;
}

/* Layout records the actual positioning ancestor, which can differ from the
   DOM parent. Reuse its padding box without another cascade. */
static int computed_style_positioning_width(
    DomBridge *bridge, const lxb_dom_node_t *node,
    const LayoutNodeBox *box, int flow_width)
{
    if (box == NULL || box->positioned_ancestor_distance == 0)
        return flow_width;
    if (box->positioned_ancestor_distance == UINT8_MAX)
        return bridge->layout->width;
    const lxb_dom_node_t *ancestor = node;
    for (unsigned step = box->positioned_ancestor_distance;
         step != 0 && ancestor != NULL; step--) ancestor = ancestor->parent;
    const LayoutNodeBox *parent = ancestor == NULL ? NULL
        : layout_box_for_node(bridge->layout, ancestor);
    return parent == NULL ? 0 : parent->client_width;
}

/* A ring of resolved ancestor styles, retained for the page's lifetime:
   64 x 348 bytes = 22 KiB on the PSP (384 bytes, 24 KiB on 64-bit hosts).
   ChatGPT send (1251 reads) misses 590 of 5330 lookups at 64, 128 and 256
   slots alike (594 at 48) in the same 17 ms; misses there come from
   mutation clears, not capacity. Only a whole-document sweep (715
   elements, three passes) gains from more (2264 -> 1914 misses, 22 ->
   17 ms host at 256), which does not pay for 87 KiB on the PSP. A bounded
   index now avoids scanning the replacement ring on each lookup. */
#define COMPUTED_STYLE_CACHE_SLOTS 64u
_Static_assert(COMPUTED_STYLE_CACHE_SLOTS <= UINT8_MAX
    && (COMPUTED_STYLE_CACHE_SLOTS & (COMPUTED_STYLE_CACHE_SLOTS - 1u)) == 0
    && sizeof(((DomBridge *) 0)->computed_style_cache.buckets)
        == COMPUTED_STYLE_CACHE_SLOTS
    && sizeof(((DomBridge *) 0)->computed_style_cache.links)
        == COMPUTED_STYLE_CACHE_SLOTS,
    "computed-style index must match its bounded replacement ring");
struct ComputedStyleCacheEntry {
    const lxb_dom_node_t *node;
    ComputedStyle style;
    /* computed_style_parent_key() of the parent style it was resolved
       from, and the cache epoch it was last known valid in. */
    uint64_t parent_key;
    uint32_t stamp;
};

/* The identity of a parent style, as the layout reuse cache keys its
   entries (layout_style_parent_hash): two 32-bit FNV lanes over the bytes.
   Unequal padding can only cost a recomputation. */
static uint64_t computed_style_parent_key(const ComputedStyle *style)
{
    if (style == NULL) return UINT64_C(0x9e3779b97f4a7c15);
    const unsigned char *bytes = (const unsigned char *) style;
    uint32_t low = UINT32_C(2166136261);
    uint32_t high = UINT32_C(0x85ebca6b);
    size_t length = sizeof(*style);
    while (length >= sizeof(uint32_t)) {
        uint32_t word = 0;
        memcpy(&word, bytes, sizeof(word));
        low = (low ^ word) * UINT32_C(16777619);
        high = (high ^ (word + UINT32_C(0x9e3779b9)))
               * UINT32_C(2246822519);
        bytes += sizeof(word);
        length -= sizeof(word);
    }
    while (length-- != 0) {
        low = (low ^ *bytes) * UINT32_C(16777619);
        high = (high ^ (uint32_t) (*bytes++ + 0x9eu)) * UINT32_C(2246822519);
    }
    return ((uint64_t) high << 32) | low;
}

static size_t computed_style_cache_bucket(const lxb_dom_node_t *node)
{
    uintptr_t key = (uintptr_t) node >> 3;
    key ^= key >> 7;
    key ^= key >> 13;
    return key & (COMPUTED_STYLE_CACHE_SLOTS - 1u);
}

static void computed_style_cache_remove(DomBridge *bridge, size_t slot)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    const lxb_dom_node_t *node = cache->entries[slot].node;
    if (node == NULL) return;
    uint8_t *link = &cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; *link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = *link - 1u;
        if (at == slot) {
            *link = cache->links[at];
            break;
        }
        link = &cache->links[at];
    }
    cache->entries[slot].node = NULL;
    cache->links[slot] = 0;
}

/* The ancestor cache for this read, emptied when any cascade input moved
   (the computed-style memo's key); NULL when styles depend on layout
   (container queries and units) or no Budget room is left. */
static bool bridge_node_within(const lxb_dom_node_t *node,
                               const lxb_dom_node_t *scope)
{
    for (unsigned depth = 0; node != NULL && depth < 256u;
         node = node->parent, depth++)
        if (node == scope) return true;
    return false;
}

/* Which selector features the cache's scoped invalidation must respect,
   as the layout reuse cache reads them (layout_reuse_note_selector_
   dependencies): :has() can restyle ancestors, structural pseudo-classes
   and sibling combinators restyle siblings, focus selectors restyle
   through state the mutation journal does not name. */
/* The key of the element a structural test in `selector` at `at`
   concerns: the compound holding `at` (a positional pseudo-class), or the
   one after it (a sibling combinator). Zero when no key bounds it. */
static void bridge_computed_style_note_key(uint32_t *keys, uint8_t *count,
                                           bool *any, uint32_t key)
{
    if (key == 0) {
        *any = true;
        return;
    }
    for (uint8_t i = 0; i < *count; i++)
        if (keys[i] == key) return;
    if (*count == COMPUTED_STYLE_CUSTOM_KEY_LIMIT) {
        *any = true;
        return;
    }
    keys[(*count)++] = key;
}

/* Every structural test in a custom-property selector, as keys of the
   elements whose custom properties it can switch: `sibling` only for the
   tests an attribute change can move (sibling combinators, `of S`
   counts), otherwise positional pseudo-classes too. */
static void bridge_computed_style_custom_keys(
    const char *selector, bool sibling, uint32_t *keys, uint8_t *count,
    bool *any)
{
    size_t length = strlen(selector);
    for (size_t at = 0; at < length && !*any; at++) {
        char value = selector[at];
        if (value == '+' || value == '~') {
            size_t next = at + 1;
            while (next < length && isspace((unsigned char) selector[next]))
                next++;
            bridge_computed_style_note_key(
                keys, count, any, next < length
                    ? style_selector_compound_key_at(selector, length, next)
                    : 0);
        } else if (value == ':') {
            const char *rest = selector + at;
            bool of = strncmp(rest, ":nth-child(", 11) == 0
                || strncmp(rest, ":nth-last-child(", 16) == 0;
            bool positional = strncmp(rest, ":nth-", 5) == 0
                || strncmp(rest, ":first-", 7) == 0
                || strncmp(rest, ":last-", 6) == 0
                || strncmp(rest, ":only-", 6) == 0
                || strncmp(rest, ":empty", 6) == 0;
            if (positional && (!sibling || of))
                bridge_computed_style_note_key(
                    keys, count, any,
                    style_selector_compound_key_at(selector, length, at));
        }
    }
}

static void bridge_computed_style_flags(DomBridge *bridge)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    const Stylesheet *sheet = bridge->stylesheet;
    if (cache->flags_sheet == (const void *) sheet
        && cache->flags_generation == sheet->build_generation) return;
    cache->flags_sheet = sheet;
    cache->flags_generation = sheet->build_generation;
    cache->flags_has = cache->flags_structure = cache->flags_focus = false;
    cache->flags_sibling = cache->flags_empty_sibling = false;
    cache->empty_key_count = 0;
    cache->empty_any = false;
    cache->flags_custom_has = cache->flags_has_escaped = false;
    cache->custom_sibling_key_count = cache->custom_structure_key_count = 0;
    cache->custom_sibling_any = cache->custom_structure_any = false;
    memset(&cache->structure, 0, sizeof(cache->structure));
    memset(&cache->sibling_structure, 0, sizeof(cache->sibling_structure));
    for (size_t pass = 0; pass < 2; pass++) {
        size_t count = pass == 0 ? sheet->count : sheet->custom_rule_count;
        for (size_t i = 0; i < count; i++) {
            const char *selector = pass == 0 ? sheet->rules[i].selector
                                             : sheet->custom_rules[i].selector;
            if (selector == NULL || strpbrk(selector, ":+~") == NULL)
                continue;
            if (strstr(selector, ":has(") != NULL) {
                cache->flags_has = true;
                if (pass == 1) cache->flags_custom_has = true;
                /* The :has() classifier compares attribute names as
                   written; an escaped selector keeps every change
                   conservative. */
                if (strchr(selector, '\\') != NULL)
                    cache->flags_has_escaped = true;
            }
            if (strstr(selector, ":focus") != NULL) cache->flags_focus = true;
            bool empty = strstr(selector, ":empty") != NULL
                || strstr(selector, ":blank") != NULL;
            bool sibling = strchr(selector, '+') != NULL
                || strchr(selector, '~') != NULL
                || strstr(selector, " of ") != NULL;
            bool structure = sibling || empty
                || strstr(selector, ":nth-") != NULL
                || strstr(selector, ":first-") != NULL
                || strstr(selector, ":last-") != NULL
                || strstr(selector, ":only-") != NULL;
            if (!structure) continue;
            cache->flags_structure = true;
            /* An element's :empty can restyle outside its own subtree only
               through a sibling test (layout_reuse_empty_reaches_siblings):
               keyed by the compound that tests it. */
            if (empty && sibling) {
                cache->flags_empty_sibling = true;
                size_t length = strlen(selector);
                for (const char *at = selector; !cache->empty_any
                     && (at = strpbrk(at, ":")) != NULL; at++) {
                    if (strncmp(at, ":empty", 6) != 0
                        && strncmp(at, ":blank", 6) != 0) continue;
                    bridge_computed_style_note_key(
                        cache->empty_keys, &cache->empty_key_count,
                        &cache->empty_any,
                        style_selector_compound_key_at(
                            selector, length, (size_t) (at - selector)));
                }
            }
            /* As the layout reuse cache notes them
               (layout_reuse_note_selector_dependencies). A '+' inside an
               An+B argument counts too: conservative. */
            style_selector_structure_keys(selector, strlen(selector),
                                          &cache->structure);
            if (sibling) {
                cache->flags_sibling = true;
                style_selector_structure_keys(selector, strlen(selector),
                                              &cache->sibling_structure);
            }
            if (pass == 1) {
                bridge_computed_style_custom_keys(
                    selector, false, cache->custom_structure_keys,
                    &cache->custom_structure_key_count,
                    &cache->custom_structure_any);
                if (sibling)
                    bridge_computed_style_custom_keys(
                        selector, true, cache->custom_sibling_keys,
                        &cache->custom_sibling_key_count,
                        &cache->custom_sibling_any);
            }
        }
    }
}

/* Whether a structural test that reaches descendants (`.a + .b .c`,
   `tr:last-child > *`) can concern one of `parent`'s children, as
   layout_reuse_structure_deep asks: then a change to a child restyles its
   siblings' subtrees, not only the siblings. Past the child bound, assume
   it can. */
static bool bridge_computed_style_structure_deep(
    const StyleStructureKeys *keys, const lxb_dom_node_t *parent)
{
    if (!keys->reaches) return false;
    if (keys->any) return true;
    size_t children = 0;
    for (const lxb_dom_node_t *child = parent->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (++children > 512u) return true;
        if (style_element_carries_any_key_unsorted(child, keys->keys, keys->count))
            return true;
    }
    return false;
}

/* Queue `node` for a subtree drop; false when the list is full. */
static bool bridge_computed_style_scope(DomBridge *bridge,
                                        lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    for (size_t i = 0; i < cache->dirty_count; i++)
        if (cache->dirty[i] == node) return true;
    if (cache->dirty_count == sizeof(cache->dirty) / sizeof(cache->dirty[0]))
        return false;
    cache->dirty[cache->dirty_count++] = node;
    return true;
}

/* Queue `node`'s own entry for a drop; false when the list is full. */
static bool bridge_computed_style_shallow(DomBridge *bridge,
                                          lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    for (size_t i = 0; i < cache->shallow_count; i++)
        if (cache->shallow[i] == node) return true;
    if (cache->shallow_count
        == sizeof(cache->shallow) / sizeof(cache->shallow[0]))
        return false;
    cache->shallow[cache->shallow_count++] = node;
    return true;
}

/* An element whose :has()-dependent match may move: its own entry, or its
   subtree where a custom-property rule can be the one that moves. */
static void bridge_computed_style_has_drop(void *opaque, lxb_dom_node_t *node)
{
    DomBridge *bridge = opaque;
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    bool queued = cache->flags_custom_has
        ? bridge_computed_style_scope(bridge, node)
        : bridge_computed_style_shallow(bridge, node);
    if (!queued) cache->dirty_all = true;
}

static struct ComputedStyleCacheEntry *computed_style_cache_find(
    DomBridge *bridge, const lxb_dom_node_t *node);

/* Whether any retained style belongs to `scope` or its subtree. A change
   whose reach lies inside a subtree nothing was read from has nothing to
   drop (entries are only added by reads, which apply the drops first). */
static bool bridge_computed_style_cached_within(const DomBridge *bridge,
                                                const lxb_dom_node_t *scope)
{
    const struct ComputedStyleCacheEntry *entries =
        bridge->computed_style_cache.entries;
    for (size_t i = 0; entries != NULL && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        if (entries[i].node != NULL
            && bridge_node_within(entries[i].node, scope)) return true;
    return false;
}

/* A change at `node` (`tree`: inserted or about to be removed; otherwise
   an attribute a sibling test reads) that structural tests let reach its
   siblings. The siblings' own styles go and their subtrees re-key off
   them, as in layout's reuse cache, unless a test reaches their
   descendants or a custom-property rule can switch a sibling's custom
   properties (not in ComputedStyle: that sibling's subtree goes). False
   when only the parent's subtree covers it. */
static bool bridge_computed_style_siblings(DomBridge *bridge,
                                           lxb_dom_node_t *node,
                                           lxb_dom_node_t *parent, bool tree)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (parent == NULL || parent->type != LXB_DOM_NODE_TYPE_ELEMENT
        || bridge_computed_style_structure_deep(
               tree ? &cache->structure : &cache->sibling_structure, parent))
        return false;
    bool custom_any = tree ? cache->custom_structure_any
                           : cache->custom_sibling_any;
    const uint32_t *custom_keys = tree ? cache->custom_structure_keys
                                       : cache->custom_sibling_keys;
    size_t custom_count = tree ? cache->custom_structure_key_count
                               : cache->custom_sibling_key_count;
    if (custom_any || !bridge_computed_style_shallow(bridge, parent)
        || !bridge_computed_style_scope(bridge, node)) return false;
    size_t children = 0;
    for (lxb_dom_node_t *child = parent->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || child == node)
            continue;
        if (++children > 512u) return false;
        if (custom_count != 0
            && style_element_carries_any_key_unsorted(child, custom_keys,
                                             custom_count)) {
            if (!bridge_computed_style_scope(bridge, child)) return false;
        } else if (computed_style_cache_find(bridge, child) != NULL
                   && !bridge_computed_style_shallow(bridge, child)) {
            /* Only a cached sibling has anything to drop. */
            return false;
        }
    }
    return true;
}

/* A connected mutation: remember what cached styles it can change, instead
   of emptying the cache (pages mutate between reads). */
static void bridge_computed_style_note_mutation(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length, bool relational,
    const uint32_t *changed_tokens, size_t changed_token_count)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (cache->entries == NULL || cache->dirty_all) return;
    if (bridge->stylesheet == NULL || node == NULL) {
        cache->dirty_all = true;
        return;
    }
    /* Retained nodes are followed (below) only while the captured inputs
       name this document and sheet, as in the flush: otherwise the next
       read clears everything without touching them. */
    if (cache->inputs.sheet != (const void *) bridge->stylesheet
        || cache->inputs.dom != (const void *) (bridge->document == NULL
                                                ? NULL
                                                : bridge->document->html))
        return;
    bridge_computed_style_flags(bridge);
    /* A change inside a shadow tree can change which of the host's light
       children render (shadow composition): those are outside its scope. */
    if (bridge->shadow_root_count != 0
        && document_shadow_carrier_containing(node) != NULL) {
        cache->dirty_all = true;
        return;
    }
    bool known = kind == SCRIPT_MUTATION_ATTRIBUTE
        || kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_TEXT
        || kind == SCRIPT_MUTATION_CHILD_LIST
        || kind == SCRIPT_MUTATION_INNER_HTML;
    /* Engine state (focus, hover and similar) travels as data-tilefinch-*
       attributes and can restyle through selectors a subtree scope
       misses. */
    bool engine_state = attribute != NULL && attribute_length >= 15u
        && strncasecmp(attribute, "data-tilefinch-", 15u) == 0;
    /* Unobserved author bookkeeping, a data-* attribute no selector tests,
       cannot change a cascade (layout's reuse check treats it the same way;
       see history_runtime.inc). Engine state keeps its semantics. */
    if (kind == SCRIPT_MUTATION_ATTRIBUTE && !engine_state
        && attribute != NULL && attribute_length > 5u
        && strncasecmp(attribute, "data-", 5u) == 0
        && !stylesheet_selectors_reference_attribute_prefix(
               bridge->stylesheet, attribute, attribute_length))
        return;
    if (!known || (engine_state && cache->flags_focus)) {
        cache->dirty_all = true;
        return;
    }
    bool tree = kind == SCRIPT_MUTATION_TEXT
        || kind == SCRIPT_MUTATION_CHILD_LIST
        || kind == SCRIPT_MUTATION_INNER_HTML;
    /* Nothing retained, nothing to drop. */
    bool retained = false;
    for (size_t i = 0; !retained && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        retained = cache->entries[i].node != NULL;
    if (!retained) return;
    /* A change a :has() rule may observe drops the elements whose answers
       can move, found from the changed position as layout's reuse cache
       finds them (style_has_invalidation.c), rather than every style. */
    if (relational && cache->flags_has && cache->flags_has_escaped) {
        cache->dirty_all = true;
        return;
    }
    if (relational && cache->flags_has) {
        char name[64];
        const char *has_attribute = NULL;
        /* An inline-style record names the CSS property; the attribute a
           selector can read is `style`. */
        if (kind == SCRIPT_MUTATION_INLINE_STYLE) {
            has_attribute = "style";
        } else if (!tree && attribute != NULL
                   && attribute_length < sizeof(name)) {
            for (size_t i = 0; i < attribute_length; i++)
                name[i] = (char) tolower((unsigned char) attribute[i]);
            name[attribute_length] = '\0';
            has_attribute = name;
        }
        bool exact_tokens = changed_tokens != NULL
            && changed_token_count <= SCRIPT_MUTATION_TOKEN_LIMIT;
        if (
#ifndef TILEFINCH_NO_TRACE
            getenv("TILEFINCH_DISABLE_HAS_SCOPED_INVALIDATION") != NULL ||
#endif
            !style_has_note_change(
                bridge->stylesheet, node, tree, has_attribute,
                exact_tokens ? changed_tokens : NULL,
                exact_tokens ? changed_token_count : 0,
                &cache->has_pending, bridge_computed_style_has_drop, bridge,
                NULL)) {
            cache->dirty_all = true;
            return;
        }
        cache->has_active = cache->has_pending.active;
        if (cache->dirty_all) return;
    }
    lxb_dom_node_t *scope = node;
    lxb_dom_node_t *parent = node->parent;
    /* Everything below reaches at most the grandparent's subtree. */
    lxb_dom_node_t *reach = parent == NULL ? node
        : (parent->parent != NULL ? parent->parent : parent);
    if (!bridge_computed_style_cached_within(bridge, reach)) return;
    /* A child list or text change can flip the parent's :empty, which a
       sibling test turns into its siblings' styles: the grandparent's
       subtree. */
    bool empty_reaches = tree && cache->flags_empty_sibling && parent != NULL
        && parent->type == LXB_DOM_NODE_TYPE_ELEMENT
        && (cache->empty_any
            || style_element_carries_any_key_unsorted(parent, cache->empty_keys,
                                             cache->empty_key_count));
    if (empty_reaches) {
        scope = parent->parent != NULL ? parent->parent : parent;
    } else if (kind == SCRIPT_MUTATION_CHILD_LIST
               && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        /* An element inserted or about to leave: its subtree, the parent
           (:empty) and, when structural tests exist, the siblings whose
           positions move. */
        if (!cache->flags_structure
            ? bridge_computed_style_shallow(bridge, parent)
                && bridge_computed_style_scope(bridge, node)
            : bridge_computed_style_siblings(bridge, node, parent, true))
            return;
        scope = parent;
    } else if (tree || engine_state) {
        if (parent != NULL) scope = parent;
    } else if (cache->flags_sibling && parent != NULL) {
        /* An attribute can reach other elements' matches only through a
           sibling combinator or an `of S` count (positional
           pseudo-classes read positions, :has() is handled above). */
        if (bridge_computed_style_siblings(bridge, node, parent, false))
            return;
        scope = parent;
    }
    if (scope == NULL || !bridge_computed_style_scope(bridge, scope))
        cache->dirty_all = true;
}

/* The container states' signature for the sheet's current container
   generation. Every layout pass rebuilds the states (bumping the
   generation) although they seldom change; retained styles compare what
   container queries and units can read instead. */
static uint64_t bridge_container_signature(DomBridge *bridge)
{
    const Stylesheet *sheet = bridge->stylesheet;
    if (bridge->computed_style_container_sheet != (const void *) sheet
        || bridge->computed_style_container_generation
               != sheet->container_state_generation) {
        bridge->computed_style_container_sheet = sheet;
        bridge->computed_style_container_generation =
            sheet->container_state_generation;
        bridge->computed_style_container_signature =
            style_container_layout_state_signature(sheet);
    }
    return bridge->computed_style_container_signature;
}

/* Whether retained styles keyed by `inputs` still hold; a container
   generation whose states did not change is adopted in place. */
static bool computed_style_inputs_hold(DomBridge *bridge,
                                       ComputedStyleInputs *inputs)
{
    const Stylesheet *sheet = bridge->stylesheet;
    if (inputs->sheet != (const void *) sheet
        || inputs->dom != (const void *) (bridge->document == NULL ? NULL
                                          : bridge->document->html)
        || inputs->sheet_generation != sheet->build_generation
        || inputs->fullscreen_node != sheet->fullscreen_node
        || inputs->host_generation != document_style_generation())
        return false;
    if (inputs->container_generation == sheet->container_state_generation)
        return true;
    if (inputs->container_signature != bridge_container_signature(bridge))
        return false;
    inputs->container_generation = sheet->container_state_generation;
    return true;
}

static void computed_style_inputs_capture(DomBridge *bridge,
                                          ComputedStyleInputs *inputs)
{
    const Stylesheet *sheet = bridge->stylesheet;
    *inputs = (ComputedStyleInputs) {
        .sheet = sheet,
        .dom = bridge->document == NULL ? NULL : bridge->document->html,
        .sheet_generation = sheet->build_generation,
        .container_generation = sheet->container_state_generation,
        .container_signature = bridge_container_signature(bridge),
        .host_generation = document_style_generation(),
        .fullscreen_node = sheet->fullscreen_node
    };
}

/* Lexbor is removing `node` from its parent, possibly to destroy it: drop
   its retained style before the address can name another node. Descendants
   stay: they are still allocated, and whatever reconnects them is a
   mutation the cache sees. */
static void bridge_computed_style_node_removed(void *opaque,
                                               const lxb_dom_node_t *node)
{
    DomBridge *bridge = opaque;
    if (bridge->computed_style_memo.node == node)
        bridge->computed_style_memo.node = NULL;
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (cache->entries == NULL) return;
    /* Queued drops must not name a node whose address can be reused. */
    style_has_pending_retire(&cache->has_pending, node);
    for (size_t i = 0; i < cache->shallow_count; i++) {
        if (cache->shallow[i] != node) continue;
        cache->shallow[i] = cache->shallow[--cache->shallow_count];
        break;
    }
    uint8_t link = cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = link - 1u;
        if (cache->entries[at].node == node) {
            computed_style_cache_remove(bridge, at);
            return;
        }
        link = cache->links[at];
    }
}

static struct ComputedStyleCacheEntry *bridge_computed_style_cache(
    DomBridge *bridge)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (bridge->stylesheet == NULL) return NULL;
    if (cache->entries == NULL) {
        /* Entries outlive script entries, so a node must be evicted when
           Lexbor removes it; without a listener slot there is no ring. */
        if (!cache->removal_listening) {
            cache->removal_listening = document_node_removal_listen(
                bridge_computed_style_node_removed, bridge);
            if (!cache->removal_listening) return NULL;
        }
        cache->entries = budget_calloc(bridge->budget,
                                       COMPUTED_STYLE_CACHE_SLOTS,
                                       sizeof(*cache->entries));
        if (cache->entries == NULL) return NULL;
        cache->inputs.sheet = NULL;
    }
    uint64_t content_generation = bridge->document == NULL
        ? 0 : bridge->document->content_generation;
    bool same_inputs = computed_style_inputs_hold(bridge, &cache->inputs);
    if (cache->has_active && cache->has_pending.styles_all)
        cache->dirty_all = true;
    if (same_inputs && cache->content_generation != content_generation
        && !cache->dirty_all) {
        /* Only the mutated subtrees' styles can have changed, and the own
           styles of the elements queued alone: their descendants are
           checked against their parents' styles when next read. Keyed
           :has() subjects go alone too, or with their subtrees when a
           custom-property rule can be what moved. */
        const lxb_dom_node_t *subjects[COMPUTED_STYLE_CACHE_SLOTS];
        size_t subject_count = 0;
        for (size_t i = 0; i < COMPUTED_STYLE_CACHE_SLOTS; i++) {
            const lxb_dom_node_t *node = cache->entries[i].node;
            if (node == NULL) continue;
            bool drop = false;
            for (size_t d = 0; !drop && d < cache->dirty_count; d++)
                drop = bridge_node_within(node, cache->dirty[d]);
            for (size_t d = 0; !drop && d < cache->shallow_count; d++)
                drop = node == cache->shallow[d];
            if (!drop && cache->has_active
                && style_has_pending_selects(bridge->stylesheet,
                                             &cache->has_pending, node)) {
                drop = true;
                if (cache->flags_custom_has)
                    subjects[subject_count++] = node;
            }
            if (drop) computed_style_cache_remove(bridge, i);
        }
        for (size_t i = 0; subject_count != 0
                           && i < COMPUTED_STYLE_CACHE_SLOTS; i++) {
            const lxb_dom_node_t *node = cache->entries[i].node;
            for (size_t s = 0; node != NULL && s < subject_count; s++) {
                if (!bridge_node_within(node, subjects[s])) continue;
                computed_style_cache_remove(bridge, i);
                break;
            }
        }
        cache->dirty_count = 0;
        cache->shallow_count = 0;
        cache->has_active = false;
        memset(&cache->has_pending, 0, sizeof(cache->has_pending));
        cache->content_generation = content_generation;
        cache->scoped_clears++;
        cache->epoch++;
        style_variable_cache_lease_clear(&cache->variables);
    }
    if (!same_inputs || cache->content_generation != content_generation) {
        if (cache->inputs.sheet != NULL
            && cache->inputs.host_generation != document_style_generation())
            cache->host_clears++;
        cache->full_clears++;
        cache->dirty_count = 0;
        cache->shallow_count = 0;
        cache->has_active = false;
        memset(&cache->has_pending, 0, sizeof(cache->has_pending));
        cache->epoch++;
        cache->dirty_all = false;
        style_variable_cache_lease_clear(&cache->variables);
        for (size_t i = 0; i < COMPUTED_STYLE_CACHE_SLOTS; i++)
            cache->entries[i].node = NULL;
        memset(cache->buckets, 0, sizeof(cache->buckets));
        memset(cache->links, 0, sizeof(cache->links));
        computed_style_inputs_capture(bridge, &cache->inputs);
        cache->content_generation = content_generation;
        cache->next = 0;
    }
    return cache->entries;
}

void js_rt_bridge_computed_style_cache_free(DomBridge *bridge)
{
    if (bridge == NULL) return;
    if (bridge->computed_style_cache.removal_listening)
        document_node_removal_unlisten(bridge_computed_style_node_removed,
                                       bridge);
    bridge->computed_style_cache.removal_listening = false;
    style_variable_cache_lease_release(&bridge->computed_style_cache.variables);
    budget_free(bridge->budget, bridge->computed_style_cache.entries);
    bridge->computed_style_cache.entries = NULL;
    bridge->computed_style_memo.node = NULL;
}

/* A document is leaving the bridge (script_runtime_detach_document): its
   tree may next be destroyed wholesale, which removes no node one by one,
   so the removal listener never evicts them. Forget every retained node
   and queued drop now, before any address can dangle or be reused; the
   next read starts from a full clear. */
void js_rt_bridge_computed_style_cache_forget(DomBridge *bridge)
{
    if (bridge == NULL) return;
    bridge->computed_style_memo.node = NULL;
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (cache->entries == NULL) return;
    for (size_t i = 0; i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        cache->entries[i].node = NULL;
    memset(cache->buckets, 0, sizeof(cache->buckets));
    memset(cache->links, 0, sizeof(cache->links));
    cache->next = 0;
    cache->dirty_count = 0;
    cache->shallow_count = 0;
    cache->dirty_all = false;
    cache->has_active = false;
    memset(&cache->has_pending, 0, sizeof(cache->has_pending));
    /* No captured inputs hold: the next read clears and recaptures. */
    cache->inputs.sheet = NULL;
    cache->epoch++;
    style_variable_cache_lease_clear(&cache->variables);
}

bool js_rt_bridge_computed_style_cache_holds(const DomBridge *bridge,
                                             const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return false;
    if (bridge->computed_style_memo.node == node) return true;
    for (size_t i = 0; bridge->computed_style_cache.entries != NULL
         && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        if (bridge->computed_style_cache.entries[i].node == node) return true;
    return false;
}

size_t js_rt_bridge_computed_style_cache_bytes(const DomBridge *bridge)
{
    if (bridge == NULL) return 0;
    return (bridge->computed_style_cache.entries == NULL ? 0
            : COMPUTED_STYLE_CACHE_SLOTS
                * sizeof(*bridge->computed_style_cache.entries))
        + style_variable_cache_lease_bytes(
            &bridge->computed_style_cache.variables);
}

static struct ComputedStyleCacheEntry *computed_style_cache_find(
    DomBridge *bridge, const lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    uint8_t link = cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = link - 1u;
        cache->lookup_probes++;
        if (cache->entries[at].node == node) return &cache->entries[at];
        link = cache->links[at];
    }
    return NULL;
}

static bool bridge_computed_style(DomBridge *bridge,
                                  lxb_dom_node_t *node, ComputedStyle *result,
                                  int *containing_width, int *content_width)
{
    struct ComputedStyleCacheEntry *cache =
        bridge_computed_style_cache(bridge);
    const uint32_t epoch = bridge->computed_style_cache.epoch;
    if (cache != NULL && containing_width == NULL) {
        const struct ComputedStyleCacheEntry *hit =
            computed_style_cache_find(bridge, node);
        if (hit != NULL && hit->stamp == epoch) {
            *result = hit->style;
            /* A hit skips style_for_node's per-node unit basis setup.
               Inline units may exist without the authored-sheet flag. */
            style_container_units_for_node(
                (Stylesheet *) bridge->stylesheet, node);
            bridge->computed_style_cache.hits++;
            return true;
        }
    }
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node; at != NULL && count < 64;
         at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) ancestors[count++] = at;
    }
    ComputedStyle computed = {0};
    bool have_parent = false;
    int flow_width = bridge->layout == NULL ? 0 : bridge->layout->width;
    /* var() lookups resolve by matching custom-property rules up the
       ancestor chain. A layout build memoizes them per build; this keeps
       them across reads under the cache's validity (see above). Lending
       allocates nothing: the first var() lookup creates the table. */
    __typeof__(bridge->computed_style_cache) *state =
        &bridge->computed_style_cache;
    state->variables.budget = bridge->budget;
    bool variable_cache = cache != NULL && style_variable_cache_attach(
        (Stylesheet *) bridge->stylesheet, &state->variables);
    /* Without geometry, start from the nearest ancestor known valid in
       this epoch. */
    if (cache != NULL && containing_width == NULL) {
        for (size_t at = 1; at < count; at++) {
            const struct ComputedStyleCacheEntry *hit =
                computed_style_cache_find(bridge, ancestors[at]);
            if (hit == NULL || hit->stamp != epoch) continue;
            computed = hit->style;
            have_parent = true;
            bridge->computed_style_cache.hits++;
            count = at;
            break;
        }
    }
    while (count != 0) {
        lxb_dom_node_t *at = ancestors[--count];
        struct ComputedStyleCacheEntry *hit = cache == NULL ? NULL
            : computed_style_cache_find(bridge, at);
        /* An entry from an earlier epoch still holds when the parent
           style it was resolved from is the parent's current style: its
           own inputs are unchanged unless a drop removed it. */
        uint64_t parent_key = cache == NULL ? 0
            : computed_style_parent_key(have_parent ? &computed : NULL);
        if (hit != NULL
            && (hit->stamp == epoch || hit->parent_key == parent_key)) {
            computed = hit->style;
            hit->stamp = epoch;
            style_container_units_for_node(
                (Stylesheet *) bridge->stylesheet, at);
            bridge->computed_style_cache.hits++;
        } else {
            /* Each inherited style can run a substantial selector scan.
               Native calls do not hit VM opcode polls, so service the
               existing bounded task watchdog between ancestors rather than
               hiding the whole chain inside one uninterruptible
               getComputedStyle call. */
            if (bridge->host != NULL
                && !js_rt_runtime_native_checkpoint(bridge->host)) {
                if (variable_cache)
                    style_variable_cache_detach(
                        (Stylesheet *) bridge->stylesheet, &state->variables);
                return false;
            }
            computed = style_for_node(bridge->stylesheet, at,
                                      have_parent ? &computed : NULL);
            bridge->computed_style_cache.misses++;
            if (hit != NULL) {
                /* A stale entry is refreshed in place. */
                hit->style = computed;
                hit->parent_key = parent_key;
                hit->stamp = epoch;
            } else if (cache != NULL) {
                __typeof__(bridge->computed_style_cache) *state =
                    &bridge->computed_style_cache;
                computed_style_cache_remove(bridge, state->next);
                cache[state->next].node = at;
                cache[state->next].style = computed;
                cache[state->next].parent_key = parent_key;
                cache[state->next].stamp = epoch;
                size_t bucket = computed_style_cache_bucket(at);
                state->links[state->next] = state->buckets[bucket];
                state->buckets[bucket] = (uint8_t) (state->next + 1u);
                state->next = (state->next + 1u) % COMPUTED_STYLE_CACHE_SLOTS;
            }
        }
        have_parent = true;
        /* Only geometry readers need this work. Resolve each ancestor's
           padding while its cascade is already in hand; cancellation stays
           on the single checked walk above. No scratch styles or second
           selector pass are needed for nested percentage padding. */
        if (containing_width != NULL && bridge->layout != NULL) {
            const LayoutNodeBox *box = layout_box_for_node(bridge->layout, at);
            /* An inline box or a display: contents element is not a
               containing block: percentages inside it refer to the nearest
               block container, so it passes the width through unchanged. */
            bool container = computed.display != DISPLAY_INLINE
                && computed.display != DISPLAY_CONTENTS;
            int basis = computed_style_positioning_width(
                bridge, at, box, flow_width);
            *containing_width = basis;
            if (box != NULL && (container || at == node)) {
                int left = 0, right = 0;
                (void) style_length_resolve(bridge->stylesheet,
                    computed.padding.left, basis, &left);
                (void) style_length_resolve(bridge->stylesheet,
                    computed.padding.right, basis, &right);
                int64_t width = (int64_t) box->client_width
                    - (left > 0 ? left : 0) - (right > 0 ? right : 0);
                flow_width = width > 0 ? (int) width : 0;
            }
            *content_width = flow_width;
        }
    }
    if (variable_cache)
        style_variable_cache_detach((Stylesheet *) bridge->stylesheet,
                                    &state->variables);
    *result = computed;
    return true;
}

static bool svg_presentation_attribute_relevant(
    lxb_dom_node_t *node, const char *property, size_t length)
{
    if (node == NULL || property == NULL || node->ns != LXB_NS_SVG) {
        return false;
    }
    size_t tag_length = 0;
    const char *tag = document_element_name(node, &tag_length);
#define SVG_TAG(wanted) \
    (tag != NULL && tag_length == sizeof(wanted) - 1u \
     && strncasecmp(tag, (wanted), sizeof(wanted) - 1u) == 0)
#define SVG_PROP(wanted) property_equal( \
    property, length, (wanted), sizeof(wanted) - 1u)
    bool text = SVG_TAG("text") || SVG_TAG("tspan") || SVG_TAG("textPath");
    bool graphics = SVG_TAG("g") || SVG_TAG("path") || SVG_TAG("rect")
        || SVG_TAG("circle") || SVG_TAG("ellipse") || SVG_TAG("line")
        || SVG_TAG("polyline") || SVG_TAG("polygon") || text;
    if (SVG_PROP("display") || SVG_PROP("visibility")) return graphics;
    if (SVG_PROP("clip-path") || SVG_PROP("clip-rule")
        || SVG_PROP("color") || SVG_PROP("cursor")
        || SVG_PROP("fill") || SVG_PROP("fill-opacity")
        || SVG_PROP("fill-rule")
        || SVG_PROP("filter") || SVG_PROP("mask")
        || SVG_PROP("opacity") || SVG_PROP("pointer-events")
        || SVG_PROP("stroke") || SVG_PROP("stroke-dasharray")
        || SVG_PROP("stroke-dashoffset") || SVG_PROP("stroke-linecap")
        || SVG_PROP("stroke-linejoin") || SVG_PROP("stroke-miterlimit")
        || SVG_PROP("stroke-opacity") || SVG_PROP("stroke-width")
        || SVG_PROP("transform") || SVG_PROP("vector-effect")) {
        return graphics;
    }
    if (SVG_PROP("direction") || SVG_PROP("font-family")
        || SVG_PROP("font-size") || SVG_PROP("font-size-adjust")
        || SVG_PROP("font-stretch") || SVG_PROP("font-style")
        || SVG_PROP("font-variant") || SVG_PROP("font-weight")
        || SVG_PROP("letter-spacing") || SVG_PROP("text-anchor")
        || SVG_PROP("text-decoration") || SVG_PROP("text-overflow")
        || SVG_PROP("text-rendering") || SVG_PROP("unicode-bidi")
        || SVG_PROP("white-space") || SVG_PROP("word-spacing")
        || SVG_PROP("writing-mode") || SVG_PROP("alignment-baseline")
        || SVG_PROP("baseline-shift") || SVG_PROP("dominant-baseline")
        || SVG_PROP("glyph-orientation-vertical")) return text;
    if (SVG_PROP("overflow")) return SVG_TAG("svg");
    if (SVG_PROP("cx") || SVG_PROP("cy") || SVG_PROP("r")) {
        return SVG_TAG("circle");
    }
    if (SVG_PROP("rx") || SVG_PROP("ry") || SVG_PROP("x")
        || SVG_PROP("y") || SVG_PROP("width") || SVG_PROP("height")) {
        return SVG_TAG("rect");
    }
    if (SVG_PROP("d")
        || SVG_PROP("marker-start") || SVG_PROP("marker-mid")
        || SVG_PROP("marker-end") || SVG_PROP("paint-order")
        || SVG_PROP("shape-rendering")) return SVG_TAG("path");
    if (SVG_PROP("stop-color") || SVG_PROP("stop-opacity")) {
        return SVG_TAG("stop");
    }
    if (SVG_PROP("flood-color") || SVG_PROP("flood-opacity")) {
        return SVG_TAG("feFlood");
    }
    if (SVG_PROP("lighting-color")) {
        return SVG_TAG("feDiffuseLighting")
            || SVG_TAG("feSpecularLighting");
    }
    if (SVG_PROP("color-interpolation")) {
        return SVG_TAG("linearGradient") || SVG_TAG("radialGradient");
    }
    if (SVG_PROP("color-interpolation-filters")) return SVG_TAG("filter");
    if (SVG_PROP("mask-type")) return SVG_TAG("mask");
    if (SVG_PROP("image-rendering")) return SVG_TAG("image");
    return false;
#undef SVG_PROP
#undef SVG_TAG
}

/* Write a scaled integer as the shortest decimal with at most three
   fractional digits: 2000/1000 -> "2", 1536/512 -> "3", 171/512 -> "0.334". */
static void computed_style_format_scaled(char *output, size_t output_size,
                                         unsigned long value,
                                         unsigned long scale)
{
    unsigned long whole = value / scale;
    unsigned long thousandths =
        ((value % scale) * 1000ul + scale / 2ul) / scale;
    if (thousandths >= 1000ul) {
        whole++;
        thousandths = 0;
    }
    if (thousandths == 0) {
        snprintf(output, output_size, "%lu", whole);
        return;
    }
    char digits[4];
    snprintf(digits, sizeof(digits), "%03lu", thousandths);
    size_t length = 3;
    while (length > 0 && digits[length - 1] == '0') digits[--length] = '\0';
    snprintf(output, output_size, "%lu.%s", whole, digits);
}

/* The properties js_computed_style_get() resolves for an ordinary rendered
   element, in strcmp order. This one table answers every CSSOM question about
   the *set* of supported properties -- `name in style`, `length`, `item()`,
   iteration -- so membership can never drift from a second list kept in
   script. `longhand` is false for shorthands and legacy aliases, which are
   attributes of the declaration but are not enumerated.

   It lives in read-only data rather than the realm heap: script asks for the
   enumeration only when a page actually enumerates a computed style, which is
   rare, and asks membership questions without materializing anything.

   tests/suites/foundation_document.inc requires every entry to resolve to a
   non-empty value; add a name here only together with its serializer. */
enum {
    /* Resolved from retained authored text or a shared initial value rather
       than a ComputedStyle field. */
    CSP_SPARSE_SCROLL = 1u << 0,
    CSP_SPARSE_MODERN = 1u << 1,
    /* A used value: needs a layout even when none has been built yet. */
    CSP_USED_GEOMETRY = 1u << 2,
    /* Resolves from the current DOM and cascade alone, so a pending DOM
       mutation does not have to be laid out first. Never a property whose
       value is a used size or depends on a box. */
    CSP_STYLE_ONLY    = 1u << 3
};

/* One row per property: identifier, name, longhand, flags. Rows are in strcmp
   order of the name, because lookup is a binary search. The enumeration, the
   table and the identifiers the getter dispatches on are all generated from
   this list, so a serializer can only be written for a registered property. */
#define COMPUTED_STYLE_PROPERTIES(X) \
    X(WEBKIT_APPEARANCE, "-webkit-appearance", false, 0) \
    X(WEBKIT_BACKDROP_FILTER, "-webkit-backdrop-filter", false, CSP_SPARSE_MODERN) \
    X(WEBKIT_LINE_CLAMP, "-webkit-line-clamp", true, 0) \
    X(WEBKIT_TEXT_SIZE_ADJUST, "-webkit-text-size-adjust", false, CSP_SPARSE_MODERN) \
    X(WEBKIT_USER_SELECT, "-webkit-user-select", false, CSP_SPARSE_MODERN) \
    X(ALIGN_CONTENT, "align-content", true, CSP_STYLE_ONLY) \
    X(ALIGN_ITEMS, "align-items", true, CSP_STYLE_ONLY) \
    X(ALIGN_SELF, "align-self", true, CSP_STYLE_ONLY) \
    X(APPEARANCE, "appearance", true, 0) \
    X(ASPECT_RATIO, "aspect-ratio", true, 0) \
    X(BACKDROP_FILTER, "backdrop-filter", true, CSP_SPARSE_MODERN) \
    X(BACKFACE_VISIBILITY, "backface-visibility", true, CSP_SPARSE_MODERN) \
    X(BACKGROUND_CLIP, "background-clip", true, 0) \
    X(BACKGROUND_COLOR, "background-color", true, CSP_STYLE_ONLY) \
    X(BACKGROUND_IMAGE, "background-image", true, 0) \
    X(BACKGROUND_ORIGIN, "background-origin", true, 0) \
    X(BACKGROUND_POSITION, "background-position", true, 0) \
    X(BACKGROUND_SIZE, "background-size", true, 0) \
    X(BORDER_BOTTOM_COLOR, "border-bottom-color", true, 0) \
    X(BORDER_BOTTOM_LEFT_RADIUS, "border-bottom-left-radius", true, 0) \
    X(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius", true, 0) \
    X(BORDER_BOTTOM_STYLE, "border-bottom-style", true, 0) \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width", true, 0) \
    X(BORDER_COLLAPSE, "border-collapse", true, 0) \
    X(BORDER_END_END_RADIUS, "border-end-end-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_END_START_RADIUS, "border-end-start-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE, "border-image", false, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_OUTSET, "border-image-outset", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_REPEAT, "border-image-repeat", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_SLICE, "border-image-slice", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_SOURCE, "border-image-source", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_WIDTH, "border-image-width", true, CSP_SPARSE_MODERN) \
    X(BORDER_LEFT_COLOR, "border-left-color", true, 0) \
    X(BORDER_LEFT_STYLE, "border-left-style", true, 0) \
    X(BORDER_LEFT_WIDTH, "border-left-width", true, 0) \
    X(BORDER_RADIUS, "border-radius", false, 0) \
    X(BORDER_RIGHT_COLOR, "border-right-color", true, 0) \
    X(BORDER_RIGHT_STYLE, "border-right-style", true, 0) \
    X(BORDER_RIGHT_WIDTH, "border-right-width", true, 0) \
    X(BORDER_SPACING, "border-spacing", true, 0) \
    X(BORDER_START_END_RADIUS, "border-start-end-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_START_START_RADIUS, "border-start-start-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_TOP_COLOR, "border-top-color", true, 0) \
    X(BORDER_TOP_LEFT_RADIUS, "border-top-left-radius", true, 0) \
    X(BORDER_TOP_RIGHT_RADIUS, "border-top-right-radius", true, 0) \
    X(BORDER_TOP_STYLE, "border-top-style", true, 0) \
    X(BORDER_TOP_WIDTH, "border-top-width", true, 0) \
    X(BORDER_WIDTH, "border-width", false, 0) \
    X(BOTTOM, "bottom", true, 0) \
    X(BOX_SHADOW, "box-shadow", true, 0) \
    X(BOX_SIZING, "box-sizing", true, CSP_STYLE_ONLY) \
    X(CAPTION_SIDE, "caption-side", true, 0) \
    X(CLIP_PATH, "clip-path", true, 0) \
    X(COLOR, "color", true, CSP_STYLE_ONLY) \
    X(COLOR_SCHEME, "color-scheme", true, CSP_SPARSE_MODERN) \
    X(COLUMN_GAP, "column-gap", true, 0) \
    X(CONTAIN, "contain", true, 0) \
    X(CONTENT, "content", true, 0) \
    X(CONTENT_VISIBILITY, "content-visibility", true, 0) \
    X(CURSOR, "cursor", true, CSP_SPARSE_SCROLL) \
    X(DIRECTION, "direction", true, CSP_STYLE_ONLY) \
    X(DISPLAY, "display", true, CSP_STYLE_ONLY) \
    X(FILTER, "filter", true, 0) \
    X(FLEX, "flex", false, 0) \
    X(FLEX_BASIS, "flex-basis", true, 0) \
    X(FLEX_DIRECTION, "flex-direction", true, CSP_STYLE_ONLY) \
    X(FLEX_GROW, "flex-grow", true, 0) \
    X(FLEX_SHRINK, "flex-shrink", true, 0) \
    X(FLEX_WRAP, "flex-wrap", true, CSP_STYLE_ONLY) \
    X(FLOAT, "float", true, 0) \
    X(FONT_FAMILY, "font-family", true, 0) \
    X(FONT_KERNING, "font-kerning", true, CSP_SPARSE_MODERN) \
    X(FONT_SIZE, "font-size", true, CSP_STYLE_ONLY) \
    X(FONT_STYLE, "font-style", true, CSP_STYLE_ONLY) \
    X(FONT_WEIGHT, "font-weight", true, CSP_STYLE_ONLY) \
    X(GAP, "gap", false, 0) \
    X(GRID_TEMPLATE_AREAS, "grid-template-areas", true, 0) \
    X(GRID_TEMPLATE_COLUMNS, "grid-template-columns", true, 0) \
    X(GRID_TEMPLATE_ROWS, "grid-template-rows", true, 0) \
    X(HEIGHT, "height", true, CSP_USED_GEOMETRY) \
    X(HYPHENS, "hyphens", true, CSP_SPARSE_MODERN) \
    X(ISOLATION, "isolation", true, CSP_SPARSE_MODERN) \
    X(JUSTIFY_CONTENT, "justify-content", true, CSP_STYLE_ONLY) \
    X(JUSTIFY_ITEMS, "justify-items", true, 0) \
    X(JUSTIFY_SELF, "justify-self", true, 0) \
    X(LEFT, "left", true, 0) \
    X(LETTER_SPACING, "letter-spacing", true, CSP_STYLE_ONLY) \
    X(LINE_HEIGHT, "line-height", true, CSP_STYLE_ONLY) \
    X(LIST_STYLE_POSITION, "list-style-position", true, 0) \
    X(LIST_STYLE_TYPE, "list-style-type", true, 0) \
    X(MARGIN, "margin", false, 0) \
    X(MARGIN_BOTTOM, "margin-bottom", true, 0) \
    X(MARGIN_LEFT, "margin-left", true, 0) \
    X(MARGIN_RIGHT, "margin-right", true, 0) \
    X(MARGIN_TOP, "margin-top", true, 0) \
    X(MAX_HEIGHT, "max-height", true, 0) \
    X(MAX_WIDTH, "max-width", true, 0) \
    X(MIN_HEIGHT, "min-height", true, 0) \
    X(MIN_WIDTH, "min-width", true, 0) \
    X(MIX_BLEND_MODE, "mix-blend-mode", true, CSP_SPARSE_MODERN) \
    X(OBJECT_FIT, "object-fit", true, 0) \
    X(OBJECT_POSITION, "object-position", true, 0) \
    X(OPACITY, "opacity", true, CSP_STYLE_ONLY) \
    X(ORDER, "order", true, CSP_STYLE_ONLY) \
    X(OUTLINE_COLOR, "outline-color", true, 0) \
    X(OUTLINE_OFFSET, "outline-offset", true, 0) \
    X(OUTLINE_STYLE, "outline-style", true, 0) \
    X(OUTLINE_WIDTH, "outline-width", true, 0) \
    X(OVERFLOW, "overflow", false, CSP_STYLE_ONLY) \
    X(OVERFLOW_X, "overflow-x", true, CSP_STYLE_ONLY) \
    X(OVERFLOW_Y, "overflow-y", true, CSP_STYLE_ONLY) \
    X(OVERSCROLL_BEHAVIOR, "overscroll-behavior", false, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_BLOCK, "overscroll-behavior-block", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_INLINE, "overscroll-behavior-inline", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_X, "overscroll-behavior-x", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_Y, "overscroll-behavior-y", true, CSP_SPARSE_SCROLL) \
    X(PADDING, "padding", false, 0) \
    X(PADDING_BOTTOM, "padding-bottom", true, 0) \
    X(PADDING_LEFT, "padding-left", true, 0) \
    X(PADDING_RIGHT, "padding-right", true, 0) \
    X(PADDING_TOP, "padding-top", true, 0) \
    X(PERSPECTIVE, "perspective", true, 0) \
    X(PLACE_CONTENT, "place-content", false, 0) \
    X(PLACE_ITEMS, "place-items", false, 0) \
    X(PLACE_SELF, "place-self", false, 0) \
    X(POINTER_EVENTS, "pointer-events", true, CSP_STYLE_ONLY) \
    X(POSITION, "position", true, CSP_STYLE_ONLY) \
    X(RESIZE, "resize", true, CSP_SPARSE_MODERN) \
    X(RIGHT, "right", true, 0) \
    X(ROTATE, "rotate", true, CSP_SPARSE_MODERN) \
    X(ROW_GAP, "row-gap", true, 0) \
    X(SCALE, "scale", true, CSP_SPARSE_MODERN) \
    X(SCROLL_BEHAVIOR, "scroll-behavior", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN, "scroll-margin", false, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_BOTTOM, "scroll-margin-bottom", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_LEFT, "scroll-margin-left", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_RIGHT, "scroll-margin-right", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_TOP, "scroll-margin-top", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING, "scroll-padding", false, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_BOTTOM, "scroll-padding-bottom", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_LEFT, "scroll-padding-left", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_RIGHT, "scroll-padding-right", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_TOP, "scroll-padding-top", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_ALIGN, "scroll-snap-align", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_STOP, "scroll-snap-stop", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_TYPE, "scroll-snap-type", true, CSP_SPARSE_SCROLL) \
    X(SCROLLBAR_COLOR, "scrollbar-color", true, CSP_SPARSE_SCROLL) \
    X(SCROLLBAR_GUTTER, "scrollbar-gutter", true, 0) \
    X(SCROLLBAR_WIDTH, "scrollbar-width", true, CSP_SPARSE_SCROLL) \
    X(TAB_SIZE, "tab-size", true, CSP_SPARSE_MODERN) \
    X(TABLE_LAYOUT, "table-layout", true, 0) \
    X(TEXT_ALIGN, "text-align", true, CSP_STYLE_ONLY) \
    X(TEXT_DECORATION_LINE, "text-decoration-line", true, CSP_STYLE_ONLY) \
    X(TEXT_INDENT, "text-indent", true, CSP_USED_GEOMETRY) \
    X(TEXT_OVERFLOW, "text-overflow", true, 0) \
    X(TEXT_RENDERING, "text-rendering", true, CSP_SPARSE_MODERN) \
    X(TEXT_SHADOW, "text-shadow", true, 0) \
    X(TEXT_SIZE_ADJUST, "text-size-adjust", true, CSP_SPARSE_MODERN) \
    X(TEXT_TRANSFORM, "text-transform", true, CSP_STYLE_ONLY) \
    X(TEXT_WRAP, "text-wrap", false, CSP_SPARSE_MODERN) \
    X(TEXT_WRAP_STYLE, "text-wrap-style", true, CSP_SPARSE_MODERN) \
    X(TOP, "top", true, 0) \
    X(TOUCH_ACTION, "touch-action", true, CSP_SPARSE_MODERN) \
    X(TRANSFORM, "transform", true, 0) \
    X(TRANSFORM_ORIGIN, "transform-origin", true, CSP_USED_GEOMETRY) \
    X(TRANSFORM_STYLE, "transform-style", true, CSP_SPARSE_MODERN) \
    X(TRANSITION, "transition", false, 0) \
    X(TRANSITION_DELAY, "transition-delay", true, 0) \
    X(TRANSITION_DURATION, "transition-duration", true, 0) \
    X(TRANSITION_PROPERTY, "transition-property", true, 0) \
    X(TRANSITION_TIMING_FUNCTION, "transition-timing-function", true, 0) \
    X(TRANSLATE, "translate", true, CSP_SPARSE_MODERN) \
    X(UNICODE_BIDI, "unicode-bidi", true, 0) \
    X(USER_SELECT, "user-select", true, CSP_SPARSE_MODERN) \
    X(VERTICAL_ALIGN, "vertical-align", true, CSP_STYLE_ONLY) \
    X(VISIBILITY, "visibility", true, CSP_STYLE_ONLY) \
    X(WHITE_SPACE, "white-space", true, CSP_STYLE_ONLY) \
    X(WIDTH, "width", true, CSP_USED_GEOMETRY) \
    X(WILL_CHANGE, "will-change", true, 0) \
    X(WORD_SPACING, "word-spacing", true, CSP_STYLE_ONLY) \
    X(WRITING_MODE, "writing-mode", true, 0) \
    X(Z_INDEX, "z-index", true, CSP_STYLE_ONLY)

typedef enum {
#define X(id, name, longhand, flags) CSP_##id,
    COMPUTED_STYLE_PROPERTIES(X)
#undef X
    CSP_COUNT,
    /* Not a registered property: a custom property, an SVG presentation
       attribute, or a name nothing supports. */
    CSP_NONE = CSP_COUNT
} ComputedStylePropertyId;

typedef struct {
    const char *name;
    bool longhand;
    uint8_t flags;
} ComputedStyleProperty;

static const ComputedStyleProperty computed_style_properties[] = {
#define X(id, name, longhand, flags) { name, longhand, flags },
    COMPUTED_STYLE_PROPERTIES(X)
#undef X
};

#define COMPUTED_STYLE_PROPERTY_COUNT \
    (sizeof(computed_style_properties) / sizeof(computed_style_properties[0]))

static ComputedStylePropertyId computed_style_property_find(
    const char *name)
{
    size_t low = 0, high = COMPUTED_STYLE_PROPERTY_COUNT;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = strcmp(name, computed_style_properties[middle].name);
        if (order == 0) return (ComputedStylePropertyId) middle;
        if (order < 0) high = middle;
        else low = middle + 1u;
    }
    return CSP_NONE;
}

static bool computed_style_transition_id(ComputedStylePropertyId id)
{
    return id == CSP_TRANSITION || id == CSP_TRANSITION_DELAY
        || id == CSP_TRANSITION_DURATION || id == CSP_TRANSITION_PROPERTY
        || id == CSP_TRANSITION_TIMING_FUNCTION;
}

/* Computed times serialize in seconds, as browsers do: 300ms is 0.3s. */
static size_t computed_transition_time(double ms, char *output, size_t size)
{
    int written = snprintf(output, size, "%gs", ms / 1000.0);
    return written < 0 ? 0 : (size_t) written;
}

static void computed_transition_value(ComputedStylePropertyId id,
                                      const StyleTransitionComputed *t,
                                      char *output, size_t size)
{
    size_t used = 0;
    output[0] = '\0';
    unsigned count = id == CSP_TRANSITION_DELAY ? t->delay_count
        : id == CSP_TRANSITION_DURATION ? t->duration_count
        : id == CSP_TRANSITION_TIMING_FUNCTION ? t->timing_count
        : t->property_count;
    for (unsigned i = 0; i < count && used + 1u < size; i++) {
        char item[160];
        if (id == CSP_TRANSITION_DELAY) {
            computed_transition_time(t->delay_ms[i], item, sizeof(item));
        } else if (id == CSP_TRANSITION_DURATION) {
            computed_transition_time(t->duration_ms[i], item, sizeof(item));
        } else if (id == CSP_TRANSITION_TIMING_FUNCTION) {
            snprintf(item, sizeof(item), "%s", t->timings[i]);
        } else if (id == CSP_TRANSITION_PROPERTY) {
            snprintf(item, sizeof(item), "%s", t->properties[i]);
        } else {
            /* The shorthand pairs each property with the other lists,
               which repeat to its length. */
            char duration[40], delay[40];
            computed_transition_time(
                t->duration_ms[i % t->duration_count], duration,
                sizeof(duration));
            computed_transition_time(
                t->delay_ms[i % t->delay_count], delay, sizeof(delay));
            snprintf(item, sizeof(item), "%s %s %s %s", t->properties[i],
                     duration, t->timings[i % t->timing_count], delay);
        }
        int written = snprintf(output + used, size - used, "%s%s",
                               i == 0 ? "" : ", ", item);
        if (written < 0 || (size_t) written >= size - used) break;
        used += (size_t) written;
    }
}

/* __tilefinchTransitionSnapshot(handle, names) ->
   [properties, durations_ms, delays_ms, values]: the transitions whose delay
   plus duration is positive (empty when there are none) and the computed
   values of names, resolved without a synchronous layout. The watcher needs
   those values while no transition applies too: a class can bring the
   transition and the new value together. */
JSValue js_transition_snapshot(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || bridge->stylesheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    StyleTransitionComputed transition;
    (void) style_transition_computed(bridge->stylesheet, node, PSEUDO_NONE,
                                     &transition);
    bool animated = false;
    for (unsigned i = 0; i < transition.property_count && !animated; i++) {
        double duration = transition.duration_ms[i % transition.duration_count];
        double delay = transition.delay_ms[i % transition.delay_count];
        animated = (duration > 0.0 ? duration : 0.0) + delay > 0.0
            && strcmp(transition.properties[i], "none") != 0;
    }
    JSValue result = JS_NewArray(context);
    JSValue properties = JS_NewArray(context);
    JSValue durations = JS_NewArray(context);
    JSValue delays = JS_NewArray(context);
    JSValue values = JS_NewArray(context);
    bool ok = !JS_IsException(result) && !JS_IsException(properties)
        && !JS_IsException(durations) && !JS_IsException(delays)
        && !JS_IsException(values);
    for (unsigned i = 0; ok && animated && i < transition.property_count;
         i++) {
        ok = JS_SetPropertyUint32(
                 context, properties, i,
                 JS_NewString(context, transition.properties[i])) >= 0
            && JS_SetPropertyUint32(
                 context, durations, i,
                 JS_NewFloat64(context, transition.duration_ms[
                     i % transition.duration_count])) >= 0
            && JS_SetPropertyUint32(
                 context, delays, i,
                 JS_NewFloat64(context, transition.delay_ms[
                     i % transition.delay_count])) >= 0;
    }
    uint32_t name_count = 0;
    if (ok && argc > 1 && JS_IsArray(context, argv[1])) {
        JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
        ok = !JS_IsException(length_value)
            && JS_ToUint32(context, &name_count, length_value) >= 0;
        JS_FreeValue(context, length_value);
    }
    if (name_count > 32) name_count = 32;
    bool saved = bridge->computed_style_without_layout;
    bridge->computed_style_without_layout = true;
    for (uint32_t i = 0; ok && i < name_count; i++) {
        JSValue name = JS_GetPropertyUint32(context, argv[1], i);
        if (JS_IsException(name)) { ok = false; break; }
        JSValueConst arguments[2] = { argv[0], name };
        JSValue value = js_computed_style_get(context, JS_UNDEFINED, 2,
                                              arguments);
        JS_FreeValue(context, name);
        if (JS_IsException(value)) { ok = false; break; }
        ok = JS_SetPropertyUint32(context, values, i, value) >= 0;
    }
    bridge->computed_style_without_layout = saved;
    /* JS_SetPropertyUint32 consumes its value whether or not the insertion
       succeeds, so a list is released here only if it was never offered. */
    JSValue *lists[4] = { &properties, &durations, &delays, &values };
    for (uint32_t i = 0; i < 4; i++) {
        JSValue list = *lists[i];
        *lists[i] = JS_UNDEFINED;
        if (ok) ok = JS_SetPropertyUint32(context, result, i, list) >= 0;
        else JS_FreeValue(context, list);
    }
    if (!ok) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* __tilefinchAttributeChangeMayAffectHas(name, oldValue, newValue) -> whether
   that attribute change can alter a :has() match anywhere, by the classifier
   the attribute setter uses for style invalidation (null: absent). Without
   values (a CSSOM style write) any change of the attribute is assumed. The
   transition watcher then rescans every element rather than the changed
   element's parent subtree. */
JSValue js_attribute_change_may_affect_has(JSContext *context,
                                           JSValueConst this_value,
                                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_TRUE;
    const char *text[3] = { NULL, NULL, NULL };
    size_t length[3] = { 0, 0, 0 };
    bool ok = true;
    for (int i = 0; i < 3 && i < argc && ok; i++) {
        if (i > 0 && (JS_IsNull(argv[i]) || JS_IsUndefined(argv[i])))
            continue;
        text[i] = JS_ToCStringLen(context, &length[i], argv[i]);
        ok = text[i] != NULL;
    }
    bool known = argc >= 3, sensitive = true;
    /* Class and id tokens need the values; without them stay conservative. */
    bool tokenized = text[0] != NULL
        && ((length[0] == 5 && strncasecmp(text[0], "class", 5) == 0)
            || (length[0] == 2 && strncasecmp(text[0], "id", 2) == 0));
    if (ok && (known || !tokenized)) {
        sensitive = stylesheet_attribute_change_may_affect_has(
            bridge == NULL ? NULL : bridge->stylesheet, text[0], length[0],
            known ? text[1] : NULL, known ? length[1] : 0,
            /* Absent to a value no selector names: any change. */
            known ? text[2] : "\x01", known ? length[2] : 1);
    }
    for (int i = 0; i < 3; i++) JS_FreeCString(context, text[i]);
    return ok ? JS_NewBool(context, sensitive) : JS_EXCEPTION;
}

/* __tilefinchStyleReach(handle, scopes) -> whether the element is one of the
   scope elements (handles, at most 16) or inside one, walking the native
   tree, where a shadow root is a child of its host. An element not under
   the document, or past the depth bound, answers true: the caller treats
   true as "may have changed". */
JSValue js_style_reach(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL) return JS_TRUE;
    lxb_dom_node_t *scopes[16];
    uint32_t scope_count = 0;
    if (argc > 1 && JS_IsArray(context, argv[1])) {
        JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
        uint32_t length = 0;
        bool ok = !JS_IsException(length_value)
            && JS_ToUint32(context, &length, length_value) >= 0;
        JS_FreeValue(context, length_value);
        if (!ok) return JS_EXCEPTION;
        if (length > 16) return JS_TRUE;
        for (uint32_t i = 0; i < length; i++) {
            JSValue value = JS_GetPropertyUint32(context, argv[1], i);
            if (JS_IsException(value)) return JS_EXCEPTION;
            lxb_dom_node_t *scope = js_rt_bridge_node_arg(context, bridge,
                                                          value);
            JS_FreeValue(context, value);
            if (scope == NULL) return JS_TRUE;
            scopes[scope_count++] = scope;
        }
    }
    unsigned depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL && depth < 4096;
         at = at->parent, depth++) {
        for (uint32_t i = 0; i < scope_count; i++)
            if (at == scopes[i]) return JS_TRUE;
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return JS_FALSE;
    }
    return JS_TRUE;
}

/* __tilefinchComputedStyleSupport(name) -> whether the declaration has that
   property (a dashed, lower-case name; custom properties always qualify).
   __tilefinchComputedStyleSupport()     -> a fresh array of the enumerable
   longhands, in order. */
JSValue js_computed_style_support(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char *name = JS_ToCString(context, argv[0]);
        if (name == NULL) return JS_EXCEPTION;
        bool supported = (name[0] == '-' && name[1] == '-')
            || computed_style_property_find(name) != CSP_NONE;
        JS_FreeCString(context, name);
        return JS_NewBool(context, supported);
    }
    JSValue names = JS_NewArray(context);
    if (JS_IsException(names)) return names;
    uint32_t at = 0;
    for (size_t i = 0; i < COMPUTED_STYLE_PROPERTY_COUNT; i++) {
        if (!computed_style_properties[i].longhand) continue;
        if (JS_SetPropertyUint32(
                context, names, at++,
                JS_NewString(context, computed_style_properties[i].name))
            < 0) {
            JS_FreeValue(context, names);
            return JS_EXCEPTION;
        }
    }
    return names;
}

/* A padding edge is a packed StyleLength: a plain pixel count, or a tagged
   percentage/math value that means nothing until it is resolved. */
static bool computed_style_edge_is_fixed(StyleLength edge)
{
    return edge >= -STYLE_LENGTH_DIRECT_LIMIT
        && edge <= STYLE_LENGTH_DIRECT_LIMIT;
}

/* Used pixels of one edge, sharing the width resolved by the cascade walk. */
static int computed_style_used_padding(
    DomBridge *bridge, int containing_width,
    StyleLength edge)
{
    if (computed_style_edge_is_fixed(edge)) return edge < 0 ? 0 : edge;
    int used = 0;
    if (!style_length_resolve(
            bridge->stylesheet, edge,
            containing_width, &used)
        || used < 0) used = 0;
    return used;
}

static bool computed_style_replaced_inline(const lxb_dom_node_t *node)
{
    return node->local_name == LXB_TAG_IMG
        || node->local_name == LXB_TAG_CANVAS
        || node->local_name == LXB_TAG_VIDEO
        || node->local_name == LXB_TAG_SVG;
}

/* The resolved value of width or height is the used value, in pixels, only
   for an element that generates a box the property applies to. False means
   the caller reports the computed value instead: a pseudo-element (its host's
   box is not its box), `display: none` or `contents`, a non-replaced inline
   (width and height do not apply to it, whatever was authored), or an element
   with no layout box yet. */
static TILEFINCH_OUT_OF_LINE bool computed_style_used_box_size(
    DomBridge *bridge, const lxb_dom_node_t *node,
    const ComputedStyle *style, int containing_width,
    PseudoElement pseudo, bool vertical, char *value, size_t value_size)
{
    if (pseudo != PSEUDO_NONE
        || style->display == DISPLAY_NONE
        || style->display == DISPLAY_CONTENTS
        || (style->display == DISPLAY_INLINE
            && !computed_style_replaced_inline(node))) return false;
    const LayoutNodeBox *box = bridge->layout == NULL ? NULL
        : layout_box_for_node(bridge->layout, node);
    if (box == NULL) return false;
    int used = vertical ? box->height : box->width;
    if (!style->box_sizing_border_box) {
        used -= vertical
            ? style->border.top + style->border.bottom
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.top)
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.bottom)
            : style->border.left + style->border.right
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.left)
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.right);
    }
    if (used < 0) used = 0;
    snprintf(value, value_size, "%dpx", used);
    return true;
}

/* The value of a sparse modern property with no authored declaration on the
   element. Inherited typography state is retained in the style; everything
   else reports its initial keyword. */
static TILEFINCH_OUT_OF_LINE const char *computed_style_sparse_modern_initial(
    const ComputedStyle *style, ComputedStylePropertyId id)
{
    static char tab_size[8];
    if (id == CSP_TAB_SIZE) {
        snprintf(tab_size, sizeof(tab_size), "%u",
                 computed_style_tab_size(style));
        return tab_size;
    }
    if (id == CSP_HYPHENS)
        return computed_style_hyphens_none(style) ? "none" : "manual";
    if (id == CSP_FONT_KERNING)
        return computed_style_kerning_none(style) ? "none" : "auto";
    if (id == CSP_TEXT_RENDERING)
        return "auto";
    if (id == CSP_MIX_BLEND_MODE
        || id == CSP_COLOR_SCHEME)
        return "normal";
    if (id == CSP_BACKFACE_VISIBILITY)
        return "visible";
    if (id == CSP_TRANSFORM_STYLE)
        return "flat";
    if (id == CSP_BORDER_IMAGE_SLICE)
        return "100%";
    if (id == CSP_BORDER_IMAGE_WIDTH)
        return "1";
    if (id == CSP_BORDER_IMAGE_OUTSET)
        return "0";
    if (id == CSP_BORDER_IMAGE_REPEAT)
        return "stretch";
    if (id == CSP_BORDER_IMAGE)
        return "none 100% / 1 / 0 stretch";
    /* translate, rotate, scale, backdrop-filter, border-image-source. */
    return "none";
}

/* Resolved values for fields the cascade already retains but CSSOM never
   serialized. Each case decodes the field's stored representation; none of
   them reads authored text, so `inherit`, `var()`, relative units and
   !important are already settled by the time a value gets here. Returns
   false for a property this function does not own. */
static TILEFINCH_OUT_OF_LINE bool computed_style_serialize_retained(
    const lxb_dom_node_t *node, const ComputedStyle *style,
    ComputedStylePropertyId id, char *value, size_t value_size)
{
    if (id == CSP_LINE_HEIGHT) {
        /* `normal` stays a keyword even though layout later picks a number
           from the face; a length, percentage or multiplier has already been
           resolved against the element's own font size, in device pixels. */
        if (style->line_height == STYLE_LINE_HEIGHT_ZERO) {
            snprintf(value, value_size, "0px");
        } else if (style->line_height <= 0) {
            snprintf(value, value_size, "normal");
        } else {
            snprintf(value, value_size, "%dpx", style->line_height);
        }
    } else if (id == CSP_TEXT_ALIGN) {
        static const char *const names[] = {
            "start", "center", "end", "left", "right"
        };
        size_t at = style->text_align;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0]) ? names[at] : "start");
    } else if (id == CSP_VERTICAL_ALIGN) {
        static const char *const names[] = {
            "baseline", "super", "sub", "middle", "top", "bottom",
            "text-top", "text-bottom"
        };
        size_t at = style->vertical_align;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0])
                     ? names[at] : "baseline");
    } else if (id == CSP_DIRECTION) {
        snprintf(value, value_size, "%s",
                 computed_style_direction_rtl(style) ? "rtl" : "ltr");
    } else if (id == CSP_WRITING_MODE) {
        static const char *const names[] = {
            "horizontal-tb", "vertical-rl", "vertical-lr"
        };
        unsigned at = computed_style_writing_mode(style);
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0])
                     ? names[at] : "horizontal-tb");
    } else if (id == CSP_UNICODE_BIDI) {
        static const char *const names[] = {
            "normal", "embed", "isolate", "bidi-override",
            "isolate-override", "plaintext"
        };
        size_t at = style->unicode_bidi;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0]) ? names[at] : "normal");
    } else if (id == CSP_LETTER_SPACING) {
        /* Retained as whole pixels; zero is the `normal` keyword. */
        if (style->letter_spacing == 0) snprintf(value, value_size, "normal");
        else snprintf(value, value_size, "%dpx", (int) style->letter_spacing);
    } else if (id == CSP_WORD_SPACING) {
        snprintf(value, value_size, "%dpx", style->word_spacing);
    } else if (id == CSP_TEXT_DECORATION_LINE) {
        /* Not inherited: only a line this element itself declares. The
           engine paints underlines alone, so that is the only line kept. */
        snprintf(value, value_size, "%s",
                 computed_style_has_text_underline(style)
                     ? "underline" : "none");
    } else if (id == CSP_FLEX_GROW) {
        computed_style_format_scaled(
            value, value_size,
            style->flex_grow > 0 ? (unsigned long) style->flex_grow : 0ul,
            1000ul);
    } else if (id == CSP_FLEX_SHRINK) {
        computed_style_format_scaled(
            value, value_size, (unsigned long) style->flex_shrink, 512ul);
    } else if (id == CSP_FLEX_BASIS) {
        if (!style->has_flex_basis) {
            snprintf(value, value_size, "auto");
        } else if (style->flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, value_size, "min-content");
        } else if (style->flex_basis == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(value, value_size, "max-content");
        } else if (style->flex_basis == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(value, value_size, "fit-content");
        } else if (style->flex_basis_percent
                   && style->flex_basis_offset != 0) {
            snprintf(value, value_size, "calc(%d%% %c %dpx)",
                     style->flex_basis,
                     style->flex_basis_offset < 0 ? '-' : '+',
                     style->flex_basis_offset < 0
                         ? -style->flex_basis_offset
                         : style->flex_basis_offset);
        } else {
            snprintf(value, value_size, "%d%s", style->flex_basis,
                     style->flex_basis_percent ? "%" : "px");
        }
    } else if (id == CSP_LIST_STYLE_TYPE) {
        static const char *const names[] = {
            NULL, "none", "disc", "circle", "square", "decimal",
            "decimal-leading-zero", "lower-alpha", "upper-alpha",
            "lower-roman", "upper-roman"
        };
        size_t at = style->list_style_none ? 1u : style->list_style_type;
        const char *keyword =
            at < sizeof(names) / sizeof(names[0]) ? names[at] : NULL;
        if (keyword == NULL) {
            /* Nothing authored: the user-agent default follows the nearest
               list container, exactly as marker generation decides it. */
            keyword = "disc";
            for (const lxb_dom_node_t *at_node = node; at_node != NULL;
                 at_node = at_node->parent) {
                if (at_node->type != LXB_DOM_NODE_TYPE_ELEMENT
                    || at_node->ns != LXB_NS_HTML) continue;
                if (at_node->local_name == LXB_TAG_OL) keyword = "decimal";
                if (at_node->local_name == LXB_TAG_OL
                    || at_node->local_name == LXB_TAG_UL) break;
            }
        }
        snprintf(value, value_size, "%s", keyword);
    } else if (id == CSP_LIST_STYLE_POSITION) {
        snprintf(value, value_size, "%s",
                 style->list_style_inside ? "inside" : "outside");
    } else if (id == CSP_TABLE_LAYOUT) {
        snprintf(value, value_size, "%s",
                 style->table_layout_fixed ? "fixed" : "auto");
    } else if (id == CSP_ASPECT_RATIO) {
        /* The older case owns an authored ratio; this is the initial value. */
        snprintf(value, value_size, "auto");
    } else {
        return false;
    }
    return true;
}

/* The value of `name` (dashed, lower-case) on the element `node` for
   __tilefinchComputedStyleGet and __tilefinchComputedStyleRead; argv[2],
   when argc > 2, names the pseudo-element. The caller owns `name`. */
/* A serialized value as a JS string, noting whether it is empty. */
static JSValue computed_style_string(JSContext *context, const char *text,
                                     bool *empty)
{
    *empty = text[0] == '\0';
    return JS_NewString(context, text);
}

static JSValue computed_style_value(JSContext *context, DomBridge *bridge,
                                    lxb_dom_node_t *node, const char *name,
                                    size_t name_length, int argc,
                                    JSValueConst *argv, bool *empty)
{
    *empty = false;
    /* The single lookup every later decision dispatches on. Names arrive
       dashed and lower-case (__tilefinchCssName). */
    const ComputedStylePropertyId id = computed_style_property_find(name);
    const bool custom_property = name_length >= 3 && name[0] == '-'
        && name[1] == '-';
    /* A name nothing resolves (not registered, not a custom property, not
       an SVG presentation attribute this element takes) serializes as ""
       whatever the cascade says: answer before any layout or cascade. */
    if (id == CSP_NONE && !custom_property
        && !svg_presentation_attribute_relevant(node, name, name_length)) {
        return computed_style_string(context, "", empty);
    }
    const unsigned property_flags =
        id == CSP_NONE ? 0u : computed_style_properties[id].flags;
    bool used_geometry_property = (property_flags & CSP_USED_GEOMETRY) != 0;
    /* Hydration often toggles a class and immediately asks whether a node
       is visible. These values resolve from the current DOM/cascade without
       rebuilding geometry. Keep the conservative path for stylesheet changes
       and container-dependent rules, whose cascade needs fresh layout.
       Container-relative units may exist only in style attributes; the
       last layout's collected container state reveals those too. */
    bool independent_style_property = (property_flags & CSP_STYLE_ONLY) != 0;
    bool style_only = independent_style_property
        && bridge->mutations.count != 0
        && !bridge->mutations.overflowed
        && !bridge->mutations.resource_rebuild_required
        && !bridge->mutations.conservative_resource_scan
        && !stylesheet_has_container_queries(bridge->stylesheet)
        && !style_container_layout_state_present(bridge->stylesheet);
    /* Transitions and the watcher's snapshots need the cascade, not
       geometry: only a stylesheet change waiting to be applied forces the
       flush. */
    bool cascade_only = computed_style_transition_id(id)
        || bridge->computed_style_without_layout;
    bool sheet_stale = bridge->relayout_dirty != NULL
        && *bridge->relayout_dirty
        && (bridge->mutations.overflowed
            || bridge->mutations.resource_rebuild_required
            || bridge->mutations.conservative_resource_scan);
    if (cascade_only ? sheet_stale
        : ((bridge->relayout_dirty != NULL && *bridge->relayout_dirty
            && !style_only)
           || (bridge->layout == NULL && used_geometry_property))) {
        (void) js_rt_bridge_flush_synchronous_layout(bridge);
    }
    if (custom_property) {
        /* A custom property's value comes from the custom-property rules
           of the element and its ancestors (style_custom_property_value),
           not from the element's computed style: no cascade. */
        PseudoElement custom_pseudo = PSEUDO_NONE;
        if (argc > 2) {
            size_t pseudo_length = 0;
            const char *pseudo_name =
                JS_ToCStringLen(context, &pseudo_length, argv[2]);
            if (pseudo_name == NULL) {
                return JS_EXCEPTION;
            }
            if ((pseudo_length == 8 && memcmp(pseudo_name, "::before", 8) == 0)
                || (pseudo_length == 7
                    && memcmp(pseudo_name, ":before", 7) == 0))
                custom_pseudo = PSEUDO_BEFORE;
            else if ((pseudo_length == 7
                      && memcmp(pseudo_name, "::after", 7) == 0)
                     || (pseudo_length == 6
                         && memcmp(pseudo_name, ":after", 6) == 0))
                custom_pseudo = PSEUDO_AFTER;
            JS_FreeCString(context, pseudo_name);
        }
        style_container_units_for_node((Stylesheet *) bridge->stylesheet,
                                       node);
        char custom_value[640] = "";
        (void) style_custom_property_value(
            bridge->stylesheet, node, custom_pseudo, name, name_length,
            custom_value, sizeof(custom_value));
        return computed_style_string(context, custom_value, empty);
    }
    if (computed_style_transition_id(id)) {
        PseudoElement transition_pseudo = PSEUDO_NONE;
        if (argc > 2) {
            const char *pseudo_name = JS_ToCString(context, argv[2]);
            if (pseudo_name == NULL) {
                return JS_EXCEPTION;
            }
            if (strcmp(pseudo_name, "::before") == 0
                || strcmp(pseudo_name, ":before") == 0)
                transition_pseudo = PSEUDO_BEFORE;
            else if (strcmp(pseudo_name, "::after") == 0
                     || strcmp(pseudo_name, ":after") == 0)
                transition_pseudo = PSEUDO_AFTER;
            JS_FreeCString(context, pseudo_name);
        }
        StyleTransitionComputed transition;
        (void) style_transition_computed(bridge->stylesheet, node,
                                         transition_pseudo, &transition);
        char transition_value[640];
        computed_transition_value(id, &transition, transition_value,
                                  sizeof(transition_value));
        return computed_style_string(context, transition_value, empty);
    }
    ComputedStyle style;
    bool padding_geometry = id == CSP_WIDTH || id == CSP_HEIGHT
        || id == CSP_PADDING || id == CSP_PADDING_TOP
        || id == CSP_PADDING_RIGHT || id == CSP_PADDING_BOTTOM
        || id == CSP_PADDING_LEFT;
    int containing_width = 0, content_width = 0;
    /* Used widths still need their geometry walk. Container-dependent
       cascades are reusable under the container-state generation, after
       the same synchronous layout flush required above. */
    bool memoizable = !padding_geometry;
    __typeof__(bridge->computed_style_memo) *memo =
        &bridge->computed_style_memo;
    uint64_t content_generation = bridge->document == NULL
        ? 0 : bridge->document->content_generation;
    if (memoizable && memo->node == node
        && memo->content_generation == content_generation
        && computed_style_inputs_hold(bridge, &memo->inputs)) {
        style = memo->style;
        style_container_units_for_node(
            (Stylesheet *) bridge->stylesheet, node);
    } else {
        if (!bridge_computed_style(bridge, node, &style,
                                   padding_geometry ? &containing_width
                                                    : NULL,
                                   &content_width)) {
            memo->node = NULL;
            return js_rt_throw_task_interruption(
                context, "computed style interrupted");
        }
        memo->node = memoizable ? node : NULL;
        if (memoizable) {
            computed_style_inputs_capture(bridge, &memo->inputs);
            memo->content_generation = content_generation;
            memo->style = style;
        }
    }
    /* A light child the flat tree leaves out has no box (style_for_node
       resolves it display:none for layout), but getComputedStyle reports
       its cascaded display, as browsers do for an unslotted element. */
    if (style.display == DISPLAY_NONE && node->parent != NULL
        && document_shadow_carrier_of_host(node->parent) != NULL
        && !document_shadow_light_child_rendered(
               document_shadow_carrier_of_host(node->parent), node)) {
        ComputedStyle parent_style;
        if (bridge_computed_style(bridge, node->parent, &parent_style,
                                  NULL, NULL)) {
            parent_style.shadow_host = 0;
            ComputedStyle cascaded = style_for_node(
                bridge->stylesheet, node, &parent_style);
            style.display = cascaded.display;
        }
    }
    PseudoElement pseudo = PSEUDO_NONE;
    if (argc > 2) {
        size_t pseudo_length = 0;
        const char *pseudo_name =
            JS_ToCStringLen(context, &pseudo_length, argv[2]);
        if (pseudo_name == NULL) {
            return JS_EXCEPTION;
        }
        if ((pseudo_length == 8 && memcmp(pseudo_name, "::before", 8) == 0)
            || (pseudo_length == 7
                && memcmp(pseudo_name, ":before", 7) == 0)) {
            pseudo = PSEUDO_BEFORE;
        } else if ((pseudo_length == 7
                    && memcmp(pseudo_name, "::after", 7) == 0)
                   || (pseudo_length == 6
                       && memcmp(pseudo_name, ":after", 6) == 0)) {
            pseudo = PSEUDO_AFTER;
        }
        if (pseudo != PSEUDO_NONE) {
            /* A pseudo-element's containing block is its originating
               element, whose style this is. */
            ComputedStyle parent_style = style;
            style = style_for_pseudo(bridge->stylesheet, node, pseudo,
                                     &parent_style);
        }
        JS_FreeCString(context, pseudo_name);
    }
    if (pseudo != PSEUDO_NONE) containing_width = content_width;
    /* The bounded grid-template-areas parser accepts up to 511 source
       bytes. Canonical row separators can add a few bytes, so retain enough
       local space to return the complete computed value rather than a
       misleading truncated prefix. */
    char value[640] = "";
    static const char *display_names[] = {
        "inline", "inline-block", "inline-flex", "inline-grid", "block",
        "flow-root", "flex", "grid", "table", "table-row", "table-cell",
        "table-row-group", "table-header-group", "table-footer-group",
        "table-column", "contents", "none"
    };
    bool svg_presentation = svg_presentation_attribute_relevant(
        node, name, name_length);
    bool retained_presentation = svg_presentation
        && style_retained_presentation_value(
            bridge->stylesheet, node, name, name_length,
            value, sizeof(value));
    size_t presentation_length = 0;
    const char *presentation = !retained_presentation && svg_presentation
        ? document_attribute(node, name, &presentation_length) : NULL;
    /* Retained authored text belongs to the element. A pseudo-element still
       has these properties; it reports their computed or initial value. */
    bool sparse_scroll = (property_flags & CSP_SPARSE_SCROLL) != 0;
    bool sparse_modern = (property_flags & CSP_SPARSE_MODERN) != 0;
    bool retained_scroll = sparse_scroll && pseudo == PSEUDO_NONE
        && (retained_scroll_box_shorthand(
                bridge->stylesheet, node, name, name_length,
                value, sizeof(value))
            || style_retained_property_value(
                bridge->stylesheet, node, name, name_length,
                value, sizeof(value)));
    if (!retained_scroll
        && sparse_scroll
        && id == CSP_CURSOR) {
        /* Inherited: a pseudo-element's parent is its originating element. */
        for (lxb_dom_node_t *at = pseudo != PSEUDO_NONE ? node : node->parent;
             at != NULL && !retained_scroll; at = at->parent) {
            retained_scroll = style_retained_property_value(
                bridge->stylesheet, at, name, name_length,
                value, sizeof(value));
        }
    }
    /* Vendor aliases are retained under their standard names. */
    const char *retained_modern_name =
        id == CSP_WEBKIT_USER_SELECT ? "user-select"
        : id == CSP_WEBKIT_TEXT_SIZE_ADJUST ? "text-size-adjust"
        : id == CSP_WEBKIT_BACKDROP_FILTER ? "backdrop-filter" : name;
    bool retained_modern = sparse_modern && pseudo == PSEUDO_NONE
        && style_retained_property_value(
            bridge->stylesheet, node, retained_modern_name,
            strlen(retained_modern_name), value, sizeof(value));
    bool serialize_computed_modern = sparse_modern
        && (id == CSP_USER_SELECT
            || id == CSP_WEBKIT_USER_SELECT
            || id == CSP_TOUCH_ACTION
            || id == CSP_RESIZE
            || id == CSP_TEXT_WRAP
            || id == CSP_TEXT_WRAP_STYLE
            || id == CSP_ISOLATION
            || id == CSP_TEXT_SIZE_ADJUST
            || id == CSP_WEBKIT_TEXT_SIZE_ADJUST);
    bool authored_text_adjust_none = retained_modern
        && (id == CSP_TEXT_SIZE_ADJUST
            || id == CSP_WEBKIT_TEXT_SIZE_ADJUST)
        && strcasecmp(value, "none") == 0;
    bool authored_touch_manipulation = retained_modern
        && id == CSP_TOUCH_ACTION
        && strcasecmp(value, "manipulation") == 0;
    bool authored_touch_combined = retained_modern
        && id == CSP_TOUCH_ACTION
        && (strcasecmp(value, "pan-x pan-y") == 0
            || strcasecmp(value, "pan-y pan-x") == 0);
    if (retained_presentation || retained_scroll
        || (retained_modern && !serialize_computed_modern)) {
        /* Already serialized into value. */
    } else if (presentation != NULL
               && presentation_length < sizeof(value)) {
        /* SVG presentation attributes participate at author origin with
           lower specificity than an authored rule.  The retained branches
           above have already given CSS its precedence; serialize the
           attribute before sparse-property initial values so `cursor` on a
           graphics element does not collapse back to `auto`. */
        memcpy(value, presentation, presentation_length);
        value[presentation_length] = '\0';
    } else if (sparse_modern) {
        if (id == CSP_USER_SELECT
            || id == CSP_WEBKIT_USER_SELECT) {
            static const char *const values[] = {
                "auto", "text", "none", "all"
            };
            unsigned mode = computed_style_user_select(&style);
            snprintf(value, sizeof(value), "%s",
                     mode < 4u ? values[mode] : "auto");
        } else if (id == CSP_TOUCH_ACTION) {
            static const char *const values[] = {
                "auto", "none", "pan-x", "pan-y"
            };
            snprintf(value, sizeof(value), "%s",
                     authored_touch_manipulation ? "manipulation"
                     : (authored_touch_combined ? "pan-x pan-y"
                     : (style.touch_action < 4u
                        ? values[style.touch_action] : "auto")));
        } else if (id == CSP_RESIZE) {
            static const char *const values[] = {
                "none", "both", "horizontal", "vertical"
            };
            snprintf(value, sizeof(value), "%s",
                     style.resize_mode < 4u
                         ? values[style.resize_mode] : "none");
        } else if (id == CSP_TEXT_WRAP
                   || id == CSP_TEXT_WRAP_STYLE) {
            StyleTextWrap mode = computed_style_text_wrap(&style);
            snprintf(value, sizeof(value), "%s",
                     mode == STYLE_TEXT_WRAP_BALANCE ? "balance"
                     : (mode == STYLE_TEXT_WRAP_PRETTY ? "pretty"
                        : (id == CSP_TEXT_WRAP_STYLE
                           ? "auto" : "wrap")));
        } else if (id == CSP_ISOLATION) {
            snprintf(value, sizeof(value), "%s",
                     computed_style_isolation_isolate(&style)
                         ? "isolate" : "auto");
        } else if (id == CSP_TEXT_SIZE_ADJUST
                   || id == CSP_WEBKIT_TEXT_SIZE_ADJUST) {
            if (authored_text_adjust_none) {
                snprintf(value, sizeof(value), "100%%");
            } else if (style.font_size_unit != 0u) {
                snprintf(value, sizeof(value), "%u%%",
                         (unsigned) style.font_size_unit - 1u);
            } else {
                snprintf(value, sizeof(value), "auto");
            }
        } else if (id == CSP_BORDER_START_START_RADIUS
                   || id == CSP_BORDER_START_END_RADIUS
                   || id == CSP_BORDER_END_START_RADIUS
                   || id == CSP_BORDER_END_END_RADIUS) {
            /* Not authored as a logical longhand, so the used value is the
               physical corner this one maps to under the element's writing
               mode and direction. The name is border-<block>-<inline>-radius;
               corners run top-left, top-right, bottom-right, bottom-left. */
            bool block_start = id == CSP_BORDER_START_START_RADIUS
                || id == CSP_BORDER_START_END_RADIUS;
            bool inline_start = (id == CSP_BORDER_START_START_RADIUS
                                 || id == CSP_BORDER_END_START_RADIUS)
                != computed_style_direction_rtl(&style);
            unsigned writing_mode = computed_style_writing_mode(&style);
            bool top = writing_mode == 0u ? block_start : inline_start;
            bool left = writing_mode == 0u ? inline_start
                : (writing_mode == 2u ? block_start : !block_start);
            snprintf(value, sizeof(value), "%dpx",
                     style_border_radius_corner(
                         stylesheet_border_radius_code(
                             bridge->stylesheet, &style),
                         top ? (left ? 0u : 1u) : (left ? 3u : 2u)));
        } else {
            /* Nothing authored on this element. `none` is the initial value
               of only some of these; the rest have a retained inherited
               state or a different initial keyword. */
            snprintf(value, sizeof(value), "%s",
                     computed_style_sparse_modern_initial(&style, id));
        }
    } else if (sparse_scroll) {
        const char *initial =
            id == CSP_SCROLL_SNAP_ALIGN
                ? "none"
            : id == CSP_SCROLL_SNAP_STOP
                ? "normal"
            : id == CSP_SCROLL_SNAP_TYPE
                ? "none"
            : id == CSP_SCROLLBAR_COLOR
                ? "auto"
            : id == CSP_SCROLLBAR_WIDTH
                ? "auto"
            : id == CSP_CURSOR
                ? "auto"
            : id == CSP_SCROLL_BEHAVIOR
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_X
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_Y
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_INLINE
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_BLOCK
                ? "auto"
                : "0px";
        snprintf(value, sizeof(value), "%s", initial);
    } else if (name_length >= 3 && name[0] == '-' && name[1] == '-') {
        (void) style_custom_property_value(
            bridge->stylesheet, node, pseudo, name, name_length,
            value, sizeof(value));
    } else if (id == CSP_DISPLAY) {
        /* CSS Display 2.7: floats and absolutely positioned boxes compute
           to their blockified display; a list item serializes as such. */
        DisplayMode display = style.display;
        if (style.float_mode != FLOAT_NONE || style.out_of_flow
            || style.fixed_position) {
            if (display == DISPLAY_INLINE
                || display == DISPLAY_INLINE_BLOCK) display = DISPLAY_BLOCK;
            else if (display == DISPLAY_INLINE_FLEX) display = DISPLAY_FLEX;
            else if (display == DISPLAY_INLINE_GRID) display = DISPLAY_GRID;
        }
        snprintf(value, sizeof(value), "%s",
                 display == DISPLAY_BLOCK && style.list_item
                     ? "list-item" : display_names[display]);
    } else if (id == CSP_VISIBILITY) {
        snprintf(value, sizeof(value), "%s",
                 style.visibility_hidden ? "hidden" : "visible");
    } else if (id == CSP_OPACITY) {
        /* Painting keeps opacity in one byte on the PSP. Recover the
           shortest decimal (up to thousandths) which quantizes to that byte
           so common authored values such as .5 and .25 retain their CSS
           computed serialization instead of leaking 8-bit paint rounding. */
        bool formatted = false;
        unsigned scale = 10;
        for (int digits = 1; digits <= 3 && !formatted; digits++, scale *= 10) {
            unsigned candidate =
                ((unsigned) style.opacity * scale + 127u) / 255u;
            if ((candidate * 255u + scale / 2u) / scale != style.opacity) {
                continue;
            }
            snprintf(value, sizeof(value), "%u.%0*u",
                     candidate / scale, digits, candidate % scale);
            size_t used = strlen(value);
            while (used > 1 && value[used - 1] == '0') value[--used] = '\0';
            if (used > 1 && value[used - 1] == '.') value[--used] = '\0';
            formatted = true;
        }
        if (!formatted) snprintf(value, sizeof(value), "%.3g",
                                 (double) style.opacity / 255.0);
    } else if (id == CSP_COLOR) {
        (void) serialize_computed_color(
            value, sizeof(value), style.color, style.color_alpha);
    } else if (id == CSP_BACKGROUND_COLOR) {
        (void) serialize_computed_color(
            value, sizeof(value),
            style.has_background ? style.background : 0,
            style.has_background ? style.background_alpha : 0);
    } else if (id == CSP_BACKGROUND_ORIGIN
               || id == CSP_BACKGROUND_CLIP) {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        bool origin = id == CSP_BACKGROUND_ORIGIN;
        StylePaintBox box = origin ? STYLE_PAINT_BOX_PADDING
                                   : STYLE_PAINT_BOX_BORDER;
        if (paint != NULL && paint->background_count != 0
            && (paint->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0) {
            box = (StylePaintBox) (origin
                ? paint->backgrounds[0].origin
                : paint->backgrounds[0].clip);
        }
        snprintf(value, sizeof(value), "%s",
                 box == STYLE_PAINT_BOX_CONTENT ? "content-box"
                 : (box == STYLE_PAINT_BOX_PADDING
                    ? "padding-box"
                    : (box == STYLE_PAINT_BOX_TEXT
                       ? "text" : "border-box")));
    } else if (id == CSP_BORDER_SPACING) {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        unsigned x = 0, y = 0;
        if (paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
            x = paint->table_spacing_x;
            y = paint->table_spacing_y;
        }
        snprintf(value, sizeof(value), "%upx %upx", x, y);
    } else if (id == CSP_BACKGROUND_IMAGE) {
        if (style.background_image == NULL || style.background_image[0] == '\0') {
            snprintf(value, sizeof(value), "none");
        } else {
            snprintf(value, sizeof(value), "url(\"%s\")",
                     style.background_image);
        }
    } else if (id == CSP_CONTENT) {
        if (!style.generated_content) {
            snprintf(value, sizeof(value), "none");
        } else if (style.generated_text != NULL) {
            snprintf(value, sizeof(value), "\"%.*s\"",
                     (int) style.generated_text_length,
                     style.generated_text);
        } else {
            snprintf(value, sizeof(value), "\"\"");
        }
    } else if (id == CSP_BACKGROUND_SIZE) {
        if ((style.background_size_flags
             & STYLE_BACKGROUND_SIZE_EXPLICIT) != 0) {
            char width[24], height[24];
            if ((style.background_size_flags
                 & STYLE_BACKGROUND_WIDTH_AUTO) != 0) {
                snprintf(width, sizeof(width), "auto");
            } else {
                snprintf(width, sizeof(width), "%u%s",
                         style.background_width,
                         (style.background_size_flags
                          & STYLE_BACKGROUND_WIDTH_PERCENT) != 0
                           ? "%" : "px");
            }
            if ((style.background_size_flags
                 & STYLE_BACKGROUND_HEIGHT_AUTO) != 0) {
                snprintf(height, sizeof(height), "auto");
            } else {
                snprintf(height, sizeof(height), "%u%s",
                         style.background_height,
                         (style.background_size_flags
                          & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
                           ? "%" : "px");
            }
            snprintf(value, sizeof(value), "%s %s", width, height);
        } else {
            snprintf(value, sizeof(value), "%s", style.background_fit == 1
                     ? "cover" : (style.background_fit == 2
                                   ? "contain" : "auto"));
        }
    } else if (id == CSP_BACKGROUND_POSITION) {
        snprintf(value, sizeof(value), "%d%% %d%%",
                 style.background_position_x,
                 style.background_position_y);
    } else if (id == CSP_BOX_SHADOW) {
        size_t box_shadow_count = stylesheet_box_shadow_count(
            bridge->stylesheet, &style);
        if (box_shadow_count == 0) {
            snprintf(value, sizeof(value), "none");
        } else {
            size_t used = 0;
            for (size_t i = 0; i < box_shadow_count
                               && i < STYLE_BOX_SHADOW_LIMIT; i++) {
                const StyleBoxShadow *shadow = stylesheet_box_shadow(
                    bridge->stylesheet, &style, i);
                if (shadow == NULL) continue;
                uint32_t argb = style_box_shadow_uses_current_color(shadow)
                    ? ((uint32_t) style.color_alpha << 24)
                        | (style.color & UINT32_C(0x00ffffff))
                    : shadow->argb;
                unsigned alpha = (argb >> 24) & 255u;
                uint32_t color = argb & UINT32_C(0x00ffffff);
                char serialized_color[48];
                (void) serialize_computed_color(
                    serialized_color, sizeof(serialized_color), color, alpha);
                int written = snprintf(
                    value + used, sizeof(value) - used,
                    "%s%s %dpx %dpx %dpx %dpx%s",
                    i == 0 ? "" : ", ", serialized_color,
                    shadow->offset_x, shadow->offset_y,
                    style_box_shadow_blur(shadow), shadow->spread,
                    style_box_shadow_is_inset(shadow) ? " inset" : "");
                if (written < 0 || (size_t) written >= sizeof(value) - used) {
                    value[sizeof(value) - 1] = '\0';
                    break;
                }
                used += (size_t) written;
            }
        }
    } else if (id == CSP_TEXT_SHADOW) {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        size_t count = paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TEXT_SHADOW) != 0
            ? paint->text_shadow_count : 0;
        if (count == 0) {
            snprintf(value, sizeof(value), "none");
        } else {
            size_t used = 0;
            if (count > STYLE_BOX_SHADOW_LIMIT) {
                count = STYLE_BOX_SHADOW_LIMIT;
            }
            for (size_t i = 0; i < count; i++) {
                const StyleBoxShadow *shadow = &paint->text_shadows[i];
                uint32_t argb = style_box_shadow_uses_current_color(shadow)
                    ? ((uint32_t) style.color_alpha << 24)
                      | (style.color & UINT32_C(0x00ffffff))
                    : shadow->argb;
                char serialized_color[48];
                (void) serialize_computed_color(
                    serialized_color, sizeof(serialized_color),
                    argb & UINT32_C(0x00ffffff), (argb >> 24) & 255u);
                int written = snprintf(
                    value + used, sizeof(value) - used,
                    "%s%s %dpx %dpx %dpx",
                    i == 0 ? "" : ", ", serialized_color,
                    shadow->offset_x, shadow->offset_y,
                    style_box_shadow_blur(shadow));
                if (written < 0 || (size_t) written >= sizeof(value) - used) {
                    value[sizeof(value) - 1] = '\0';
                    break;
                }
                used += (size_t) written;
            }
        }
    } else if (id == CSP_OBJECT_FIT) {
        const char *fit = "fill";
        if (style.object_fit == STYLE_OBJECT_FIT_COVER) fit = "cover";
        else if (style.object_fit == STYLE_OBJECT_FIT_CONTAIN) fit = "contain";
        else if (style.object_fit == STYLE_OBJECT_FIT_NONE) fit = "none";
        else if (style.object_fit == STYLE_OBJECT_FIT_SCALE_DOWN) {
            fit = "scale-down";
        }
        snprintf(value, sizeof(value), "%s", fit);
    } else if (id == CSP_OBJECT_POSITION) {
        int x = style_object_position_percent(style.object_position_x);
        int y = style_object_position_percent(style.object_position_y);
        int offset_x = style_object_position_offset(style.object_position_x);
        int offset_y = style_object_position_offset(style.object_position_y);
        if (offset_x == 0 && offset_y == 0) {
            snprintf(value, sizeof(value), "%d%% %d%%", x, y);
        } else {
            snprintf(value, sizeof(value),
                     "calc(%d%% + %dpx) calc(%d%% + %dpx)",
                     x, offset_x, y, offset_y);
        }
    } else if (id == CSP_APPEARANCE
               || id == CSP_WEBKIT_APPEARANCE) {
        static const char *const appearance_names[] = {
            "none", "auto", "base", "base-select", "button", "checkbox",
            "listbox", "menulist-button", "meter", "progress-bar", "radio",
            "searchfield", "textarea", "textfield"
        };
        size_t appearance = style.appearance & STYLE_APPEARANCE_MASK;
        if (appearance >= sizeof(appearance_names)
                          / sizeof(appearance_names[0])) {
            appearance = APPEARANCE_NONE;
        }
        snprintf(value, sizeof(value), "%s", appearance_names[appearance]);
    } else if (id == CSP_JUSTIFY_SELF) {
        static const char *const justify_self_names[] = {
            "auto", "start", "center", "end", "stretch", "baseline"
        };
        size_t justify_self = computed_style_justify_self(&style);
        if (justify_self >= sizeof(justify_self_names)
                            / sizeof(justify_self_names[0])) {
            justify_self = ALIGN_SELF_AUTO;
        }
        snprintf(value, sizeof(value), "%s",
                 justify_self_names[justify_self]);
    } else if (id == CSP_JUSTIFY_ITEMS) {
        static const char *const item_names[] = {
            "start", "center", "end", "stretch", "baseline"
        };
        size_t item = style.justify_items;
        if (item >= sizeof(item_names) / sizeof(item_names[0])) {
            item = ALIGN_STRETCH;
        }
        snprintf(value, sizeof(value), "%s", item_names[item]);
    } else if (id == CSP_PLACE_SELF) {
        static const char *const self_names[] = {
            "auto", "start", "center", "end", "stretch", "baseline"
        };
        size_t align = style.align_self;
        size_t justify = computed_style_justify_self(&style);
        if (align >= sizeof(self_names) / sizeof(self_names[0])) {
            align = ALIGN_SELF_AUTO;
        }
        if (justify >= sizeof(self_names) / sizeof(self_names[0])) {
            justify = ALIGN_SELF_AUTO;
        }
        if (align == justify) {
            snprintf(value, sizeof(value), "%s", self_names[align]);
        } else {
            snprintf(value, sizeof(value), "%s %s",
                     self_names[align], self_names[justify]);
        }
    } else if (id == CSP_PLACE_ITEMS) {
        static const char *const item_names[] = {
            "start", "center", "end", "stretch", "baseline"
        };
        size_t align = style.align_items;
        size_t justify = style.justify_items;
        if (align >= sizeof(item_names) / sizeof(item_names[0])) {
            align = ALIGN_STRETCH;
        }
        if (justify >= sizeof(item_names) / sizeof(item_names[0])) {
            justify = ALIGN_STRETCH;
        }
        if (align == justify) {
            snprintf(value, sizeof(value), "%s", item_names[align]);
        } else {
            snprintf(value, sizeof(value), "%s %s",
                     item_names[align], item_names[justify]);
        }
    } else if (id == CSP_PLACE_CONTENT) {
        static const char *const content_names[] = {
            "start", "center", "end", "space-between", "space-around",
            "space-evenly", "stretch"
        };
        size_t align = style.align_content;
        size_t justify = style.justify_content;
        if (align >= sizeof(content_names) / sizeof(content_names[0])) {
            align = JUSTIFY_STRETCH;
        }
        if (justify >= sizeof(content_names) / sizeof(content_names[0])) {
            justify = JUSTIFY_START;
        }
        if (align == justify) {
            snprintf(value, sizeof(value), "%s", content_names[align]);
        } else {
            snprintf(value, sizeof(value), "%s %s",
                     content_names[align], content_names[justify]);
        }
    } else if (id == CSP_FONT_SIZE) {
        int fixed = computed_style_font_size_fixed(&style);
        if ((fixed & 63) == 0) {
            snprintf(value, sizeof(value), "%dpx", fixed / 64);
        } else {
            snprintf(value, sizeof(value), "%.6fpx", fixed / 64.0);
        }
    } else if (id == CSP_FONT_FAMILY) {
        FontFamily family = font_family_is_web(style.font_family)
            ? font_family_web_fallback(style.font_family)
            : style.font_family;
        snprintf(value, sizeof(value), "%s",
                 family == FONT_MONOSPACE ? "monospace"
                 : family == FONT_SERIF ? "serif" : "sans-serif");
    } else if (id == CSP_TEXT_INDENT) {
        const LayoutNodeBox *box = bridge->layout == NULL ? NULL
            : layout_box_for_node(bridge->layout, node);
        int used_indent = 0;
        if (!style_length_resolve(
                bridge->stylesheet, style.text_indent,
                box != NULL ? box->content_width : 0, &used_indent)) {
            used_indent = 0;
        }
        snprintf(value, sizeof(value), "%dpx", used_indent);
    } else if (id == CSP_TEXT_OVERFLOW) {
        snprintf(value, sizeof(value), "%s",
                 computed_style_text_overflow_ellipsis(&style)
                   ? "ellipsis" : "clip");
    } else if (id == CSP_FONT_WEIGHT) {
        snprintf(value, sizeof(value), "%u",
                 style.font_weight != 0 ? style.font_weight
                                         : (style.font_bold ? 700u : 400u));
    } else if (id == CSP_FONT_STYLE) {
        snprintf(value, sizeof(value), "%s",
                 style.font_italic ? "italic" : "normal");
    } else if (id == CSP_POSITION) {
        snprintf(value, sizeof(value), "%s", style.fixed_position ? "fixed"
                 : style.sticky_position ? "sticky"
                 : style.out_of_flow ? "absolute"
                 : style.relative_position ? "relative" : "static");
    } else if (id == CSP_TOP) {
        if (!style.has_top) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.top,
                      style.inset_percent_mask & STYLE_INSET_TOP_PERCENT
                      ? "%" : "px");
    } else if (id == CSP_RIGHT) {
        if (!style.has_right) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.right,
                      style.inset_percent_mask & STYLE_INSET_RIGHT_PERCENT
                      ? "%" : "px");
    } else if (id == CSP_BOTTOM) {
        if (!style.has_bottom) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.bottom,
                      style.inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT
                      ? "%" : "px");
    } else if (id == CSP_LEFT) {
        if (!style.has_left) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.left,
                      style.inset_percent_mask & STYLE_INSET_LEFT_PERCENT
                      ? "%" : "px");
    } else if (id == CSP_OVERFLOW) {
        const char *x = style.overflow_x_clip_only ? "clip"
            : computed_style_overflow_x_hidden(&style) ? "hidden"
            : (style.overflow_x_scroll ? "auto" : "visible");
        const char *y = style.overflow_y_clip_only
            ? "clip" : (style.overflow_y_scroll ? "auto" : "visible");
        if (strcmp(x, y) == 0) snprintf(value, sizeof(value), "%s", x);
        else snprintf(value, sizeof(value), "%s %s", x, y);
    } else if (id == CSP_OVERFLOW_X) {
        snprintf(value, sizeof(value), "%s",
                 style.overflow_x_clip_only ? "clip"
                 : computed_style_overflow_x_hidden(&style) ? "hidden"
                 : (style.overflow_x_scroll ? "auto" : "visible"));
    } else if (id == CSP_OVERFLOW_Y) {
        snprintf(value, sizeof(value), "%s", style.overflow_y_clip_only
                 ? "clip" : (style.overflow_y_scroll ? "auto" : "visible"));
    } else if (id == CSP_SCROLLBAR_GUTTER) {
        snprintf(value, sizeof(value), "%s",
                 style.scrollbar_gutter_stable ? "stable" : "auto");
    } else if (id == CSP_BORDER_RADIUS) {
        int code = stylesheet_border_radius_code(
            bridge->stylesheet, &style);
        if (style_border_radius_is_packed(code)) {
            snprintf(value, sizeof(value), "%dpx %dpx %dpx %dpx",
                     style_border_radius_corner(code, 0),
                     style_border_radius_corner(code, 1),
                     style_border_radius_corner(code, 2),
                     style_border_radius_corner(code, 3));
        } else {
            snprintf(value, sizeof(value), "%dpx", code);
        }
    } else if (id == CSP_BORDER_TOP_LEFT_RADIUS
               || id == CSP_BORDER_TOP_RIGHT_RADIUS
               || id == CSP_BORDER_BOTTOM_RIGHT_RADIUS
               || id == CSP_BORDER_BOTTOM_LEFT_RADIUS) {
        int code = stylesheet_border_radius_code(
            bridge->stylesheet, &style);
        unsigned corner = name[7] == 't'
            ? (name[11] == 'l' ? 0u : 1u)
            : (name[14] == 'r' ? 2u : 3u);
        snprintf(value, sizeof(value), "%dpx",
                 style_border_radius_corner(code, corner));
    } else if (id == CSP_BORDER_COLLAPSE) {
        snprintf(value, sizeof(value), "%s",
                 style.table_border_collapse ? "collapse" : "separate");
    } else if (id == CSP_CAPTION_SIDE) {
        snprintf(value, sizeof(value), "%s",
                 style.order >= 200000 ? "bottom" : "top");
    } else if (id == CSP_OUTLINE_WIDTH) {
        snprintf(value, sizeof(value), "%upx",
                 computed_style_outline_width(&style));
    } else if (id == CSP_OUTLINE_STYLE) {
        static const char *const outline_names[] = {
            "none", "solid", "dashed", "dotted"
        };
        unsigned outline = computed_style_outline_style(&style);
        if (outline >= sizeof(outline_names) / sizeof(outline_names[0])) {
            outline = STYLE_OUTLINE_NONE;
        }
        snprintf(value, sizeof(value), "%s", outline_names[outline]);
    } else if (id == CSP_OUTLINE_OFFSET) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_outline_offset(&style));
    } else if (id == CSP_OUTLINE_COLOR) {
        uint32_t color = (style.outline_state & STYLE_OUTLINE_CURRENT_COLOR)
            ? style.color : style.outline_color;
        uint8_t alpha = (style.outline_state & STYLE_OUTLINE_CURRENT_COLOR)
            ? style.color_alpha : style.outline_alpha;
        if (alpha == 255) {
            snprintf(value, sizeof(value), "rgb(%u, %u, %u)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u));
        } else {
            snprintf(value, sizeof(value), "rgba(%u, %u, %u, %.3g)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u),
                     (double) alpha / 255.0);
        }
    } else if (id == CSP_CLIP_PATH) {
        unsigned clip = computed_style_clip_path_type(&style);
        if (clip == STYLE_CLIP_PATH_CIRCLE) {
            snprintf(value, sizeof(value), "circle()");
        } else if (clip == STYLE_CLIP_PATH_INSET) {
            unsigned inset = computed_style_clip_path_inset(&style);
            unsigned radius = computed_style_clip_path_radius(&style);
            const char *unit =
                (style.clip_path_state & STYLE_CLIP_PATH_INSET_PERCENT)
                ? "%" : "px";
            if (radius != 0) {
                snprintf(value, sizeof(value), "inset(%u%s round %upx)",
                         inset, unit, radius);
            } else {
                snprintf(value, sizeof(value), "inset(%u%s)", inset, unit);
            }
        } else {
            snprintf(value, sizeof(value), "none");
        }
    } else if (id == CSP_WHITE_SPACE) {
        static const char *const white_space_names[] = {
            "normal", "nowrap", "pre", "pre-wrap", "pre-line",
            "break-spaces"
        };
        size_t mode = style.white_space_mode;
        if (mode >= sizeof(white_space_names)
                    / sizeof(white_space_names[0])) {
            mode = WHITE_SPACE_NORMAL;
        }
        snprintf(value, sizeof(value), "%s", white_space_names[mode]);
    } else if (id == CSP_TEXT_TRANSFORM) {
        static const char *const transform_names[] = {
            "none", "uppercase", "lowercase", "capitalize"
        };
        size_t transform = style.text_transform;
        if (transform >= sizeof(transform_names)
                         / sizeof(transform_names[0])) {
            transform = TEXT_TRANSFORM_NONE;
        }
        snprintf(value, sizeof(value), "%s", transform_names[transform]);
    } else if (id == CSP_BOX_SIZING) {
        snprintf(value, sizeof(value), "%s",
                 style.box_sizing_border_box ? "border-box" : "content-box");
    } else if (id == CSP_FLEX) {
        char basis[32];
        if (!style.has_flex_basis) {
            snprintf(basis, sizeof(basis), "auto");
        } else if (style.flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(basis, sizeof(basis), "min-content");
        } else if (style.flex_basis == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(basis, sizeof(basis), "max-content");
        } else if (style.flex_basis == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(basis, sizeof(basis), "fit-content");
        } else {
            snprintf(basis, sizeof(basis), "%d%s", style.flex_basis,
                     style.flex_basis_percent ? "%" : "px");
        }
        snprintf(value, sizeof(value), "%.3g %.3g %s",
                 (double) style.flex_grow / 1000.0,
                 (double) style.flex_shrink / 512.0, basis);
    } else if (id == CSP_FLEX_DIRECTION) {
        static const char *direction_names[] = {
            "row", "row-reverse", "column", "column-reverse"
        };
        snprintf(value, sizeof(value), "%s",
                 direction_names[style.flex_direction]);
    } else if (id == CSP_FLEX_WRAP) {
        snprintf(value, sizeof(value), "%s", style.flex_wrap_reverse
                 ? "wrap-reverse" : (style.flex_wrap ? "wrap" : "nowrap"));
    } else if (id == CSP_JUSTIFY_CONTENT) {
        static const char *justify_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        size_t justify = style.justify_content;
        if (justify >= sizeof(justify_names) / sizeof(justify_names[0])) {
            justify = JUSTIFY_START;
        }
        snprintf(value, sizeof(value), "%s", justify_names[justify]);
    } else if (id == CSP_ALIGN_ITEMS) {
        static const char *align_names[] = {
            "flex-start", "center", "flex-end", "stretch", "baseline"
        };
        size_t align = style.align_items;
        if (align >= sizeof(align_names) / sizeof(align_names[0])) {
            align = ALIGN_STRETCH;
        }
        snprintf(value, sizeof(value), "%s", align_names[align]);
    } else if (id == CSP_ALIGN_SELF) {
        static const char *align_self_names[] = {
            "auto", "flex-start", "center", "flex-end", "stretch",
            "baseline"
        };
        snprintf(value, sizeof(value), "%s",
                 align_self_names[style.align_self]);
    } else if (id == CSP_ALIGN_CONTENT) {
        static const char *align_content_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        snprintf(value, sizeof(value), "%s",
                 align_content_names[style.align_content]);
    } else if (id == CSP_ORDER) {
        snprintf(value, sizeof(value), "%d", style.order);
    } else if (id == CSP_Z_INDEX) {
        if (style.has_z_index) snprintf(value, sizeof(value), "%d", style.z_index);
        else snprintf(value, sizeof(value), "auto");
    } else if (id == CSP_WIDTH) {
        /* The resolved value of width is the used value whenever the element
           generates a box: a percentage or sizing keyword must come back in
           pixels. Only an element without a box, or a non-replaced inline,
           reports its computed value. */
        if (computed_style_used_box_size(
                bridge, node, &style, containing_width, pseudo, false,
                value, sizeof(value))) {
            /* Used pixels. */
        } else if (!style.has_width) {
            snprintf(value, sizeof(value), "auto");
        } else {
            if (computed_style_width_min_content(&style)) {
                snprintf(value, sizeof(value), "min-content");
            } else if (computed_style_width_max_content(&style)) {
                snprintf(value, sizeof(value), "max-content");
            } else if (computed_style_width_fit_content(&style)) {
                snprintf(value, sizeof(value), "fit-content");
            } else {
                snprintf(value, sizeof(value), "%d%s", style.width,
                         style.width_percent ? "%" : "px");
            }
        }
    } else if (id == CSP_MIN_WIDTH) {
        if (style.min_width_auto) snprintf(value, sizeof(value), "auto");
        else if (style.min_width == STYLE_LENGTH_MIN_CONTENT)
            snprintf(value, sizeof(value), "min-content");
        else if (style.min_width == STYLE_LENGTH_MAX_CONTENT)
            snprintf(value, sizeof(value), "max-content");
        else if (style.min_width == STYLE_LENGTH_FIT_CONTENT)
            snprintf(value, sizeof(value), "fit-content");
        else snprintf(value, sizeof(value), "%d%s", style.min_width,
                      style.min_width_percent ? "%" : "px");
    } else if (id == CSP_MAX_WIDTH) {
        if (style.max_width == STYLE_LENGTH_NONE) {
            snprintf(value, sizeof(value), "none");
        } else if (style.max_width == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, sizeof(value), "min-content");
        } else if (style.max_width == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(value, sizeof(value), "max-content");
        } else if (style.max_width == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(value, sizeof(value), "fit-content");
        } else snprintf(value, sizeof(value), "%d%s", style.max_width,
                      style.max_width_percent ? "%" : "px");
    } else if (id == CSP_HEIGHT) {
        if (computed_style_used_box_size(
                bridge, node, &style, containing_width, pseudo, true,
                value, sizeof(value))) {
            /* Used pixels. */
        } else if (!style.has_height) {
            snprintf(value, sizeof(value), "auto");
        } else {
            snprintf(value, sizeof(value), "%d%s", style.height,
                     style.height_percent ? "%" : "px");
        }
    } else if (id == CSP_MIN_HEIGHT) {
        snprintf(value, sizeof(value), "%d%s", style.min_height,
                 style.min_height_percent ? "%" : "px");
    } else if (id == CSP_MAX_HEIGHT) {
        if (style.max_height == STYLE_LENGTH_NONE) {
            snprintf(value, sizeof(value), "none");
        } else if (style.max_height == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, sizeof(value), "min-content");
        }
        else snprintf(value, sizeof(value), "%d%s", style.max_height,
                      style.max_height_percent ? "%" : "px");
    } else if (id == CSP_MARGIN_TOP) {
        if (style.margin_top_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.top);
    } else if (id == CSP_MARGIN_RIGHT) {
        if (style.margin_right_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.right);
    } else if (id == CSP_MARGIN_BOTTOM) {
        if (style.margin_bottom_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.bottom);
    } else if (id == CSP_MARGIN_LEFT) {
        if (style.margin_left_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.left);
    } else if (id == CSP_MARGIN) {
        /* Four values, as `padding` reports them; an auto side keeps its
           keyword. */
        const int sides[4] = { style.margin.top, style.margin.right,
                               style.margin.bottom, style.margin.left };
        const bool automatic[4] = {
            style.margin_top_auto, style.margin_right_auto,
            style.margin_bottom_auto, style.margin_left_auto
        };
        size_t used = 0;
        for (unsigned side = 0; side < 4u && used < sizeof(value); side++) {
            int written = automatic[side]
                ? snprintf(value + used, sizeof(value) - used, "%sauto",
                           side == 0 ? "" : " ")
                : snprintf(value + used, sizeof(value) - used, "%s%dpx",
                           side == 0 ? "" : " ", sides[side]);
            if (written < 0) break;
            used += (size_t) written;
        }
    } else if (id == CSP_PADDING_TOP) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.top));
    } else if (id == CSP_PADDING_RIGHT) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.right));
    } else if (id == CSP_PADDING_BOTTOM) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.bottom));
    } else if (id == CSP_PADDING_LEFT) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.left));
    } else if (id == CSP_PADDING) {
        snprintf(value, sizeof(value), "%dpx %dpx %dpx %dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.top),
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.right),
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.bottom),
                 computed_style_used_padding(
                     bridge, containing_width, style.padding.left));
    } else if (id == CSP_BORDER_TOP_WIDTH) {
        snprintf(value, sizeof(value), "%dpx", style.border.top);
    } else if (id == CSP_BORDER_RIGHT_WIDTH) {
        snprintf(value, sizeof(value), "%dpx", style.border.right);
    } else if (id == CSP_BORDER_BOTTOM_WIDTH) {
        snprintf(value, sizeof(value), "%dpx", style.border.bottom);
    } else if (id == CSP_BORDER_LEFT_WIDTH) {
        snprintf(value, sizeof(value), "%dpx", style.border.left);
    } else if (id == CSP_BORDER_WIDTH) {
        snprintf(value, sizeof(value), "%dpx %dpx %dpx %dpx",
                 style.border.top, style.border.right,
                 style.border.bottom, style.border.left);
    } else if (id == CSP_BORDER_TOP_STYLE
               || id == CSP_BORDER_RIGHT_STYLE
               || id == CSP_BORDER_BOTTOM_STYLE
               || id == CSP_BORDER_LEFT_STYLE) {
        static const char *const border_lines[] = {
            "none", "solid", "dashed", "dotted"
        };
        StyleBorderSide side =
            name[7] == 't' ? STYLE_BORDER_TOP
            : (name[7] == 'r' ? STYLE_BORDER_RIGHT
               : (name[7] == 'b' ? STYLE_BORDER_BOTTOM
                                  : STYLE_BORDER_LEFT));
        unsigned line = computed_style_border_line(&style, side);
        if (line >= sizeof(border_lines) / sizeof(border_lines[0])) {
            line = STYLE_BORDER_NONE;
        }
        snprintf(value, sizeof(value), "%s", border_lines[line]);
    } else if (id == CSP_BORDER_TOP_COLOR
               || id == CSP_BORDER_RIGHT_COLOR
               || id == CSP_BORDER_BOTTOM_COLOR
               || id == CSP_BORDER_LEFT_COLOR) {
        StyleBorderSide side =
            name[7] == 't' ? STYLE_BORDER_TOP
            : (name[7] == 'r' ? STYLE_BORDER_RIGHT
               : (name[7] == 'b' ? STYLE_BORDER_BOTTOM
                                  : STYLE_BORDER_LEFT));
        uint8_t alpha = 255;
        uint32_t color = stylesheet_border_color(
            bridge->stylesheet, &style, side, &alpha);
        if (alpha == 255) {
            snprintf(value, sizeof(value), "rgb(%u, %u, %u)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u));
        } else {
            snprintf(value, sizeof(value), "rgba(%u, %u, %u, %.3g)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u),
                     (double) alpha / 255.0);
        }
    } else if (id == CSP_TRANSFORM) {
        if (!style.has_transform) {
            snprintf(value, sizeof(value), "none");
        } else {
            snprintf(value, sizeof(value), "translate(%d%s, %d%s)",
                     style.transform_x,
                     style.transform_x_percent ? "%" : "px",
                     style.transform_y,
                     style.transform_y_percent ? "%" : "px");
        }
    } else if (id == CSP_TRANSFORM_ORIGIN) {
        uint16_t x = style_object_position_encode(50, 0);
        uint16_t y = style_object_position_encode(50, 0);
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        if (paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN) != 0) {
            x = paint->transform_origin_x;
            y = paint->transform_origin_y;
        }
        const LayoutNodeBox *box = bridge->layout == NULL ? NULL
            : layout_box_for_node(bridge->layout, node);
        if (box != NULL) {
            int used_x = box->width
                * style_object_position_percent(x) / 100
                + style_object_position_offset(x);
            int used_y = box->height
                * style_object_position_percent(y) / 100
                + style_object_position_offset(y);
            snprintf(value, sizeof(value), "%dpx %dpx", used_x, used_y);
        } else {
            snprintf(value, sizeof(value), "%d%% %d%%",
                     style_object_position_percent(x),
                     style_object_position_percent(y));
        }
    } else if (id == CSP_PERSPECTIVE) {
        snprintf(value, sizeof(value), "%s",
                 style.has_perspective ? "1px" : "none");
    } else if (id == CSP_FILTER) {
        static const char *const filter_names[] = {
            "none", "grayscale(1)", "invert(1)", "sepia(1)",
            "brightness(1.25)", "brightness(0.75)",
            "contrast(1.25)", "saturate(1.25)"
        };
        unsigned filter = computed_style_filter_code(&style);
        if (!style.has_filter
            || filter >= sizeof(filter_names) / sizeof(filter_names[0])) {
            filter = STYLE_FILTER_NONE;
        }
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        bool low = paint != NULL
            && (paint->reserved & STYLE_PAINT_FILTER_LOW_AMOUNT) != 0;
        if (low && filter == STYLE_FILTER_CONTRAST) {
            snprintf(value, sizeof(value), "contrast(0.75)");
        } else if (low && filter == STYLE_FILTER_SATURATE) {
            snprintf(value, sizeof(value), "saturate(0.75)");
        } else {
            snprintf(value, sizeof(value), "%s", filter_names[filter]);
        }
    } else if (id == CSP_CONTAIN) {
        snprintf(value, sizeof(value), "%s",
                 style.has_layout_containment ? "paint" : "none");
    } else if (id == CSP_CONTENT_VISIBILITY) {
        snprintf(value, sizeof(value), "%s",
                 style.content_visibility == STYLE_CONTENT_VISIBILITY_HIDDEN
                    ? "hidden"
                    : (style.content_visibility
                           == STYLE_CONTENT_VISIBILITY_AUTO
                       ? "auto" : "visible"));
    } else if (id == CSP_WEBKIT_LINE_CLAMP) {
        unsigned clamp = computed_style_line_clamp(&style);
        if (clamp == 0) snprintf(value, sizeof(value), "none");
        else snprintf(value, sizeof(value), "%u",
                      clamp);
    } else if (id == CSP_WILL_CHANGE) {
        snprintf(value, sizeof(value), "%s",
                 style.will_change_transform ? "transform" : "auto");
    } else if (id == CSP_POINTER_EVENTS) {
        snprintf(value, sizeof(value), "%s",
                 style.pointer_events_none ? "none" : "auto");
    } else if (id == CSP_FLOAT) {
        static const char *const float_names[] = {
            "none", "left", "right"
        };
        size_t mode = style.float_mode;
        if (mode >= sizeof(float_names) / sizeof(float_names[0])) mode = 0;
        snprintf(value, sizeof(value), "%s", float_names[mode]);
    } else if (id == CSP_ASPECT_RATIO
               && style.aspect_width > 0 && style.aspect_height > 0) {
        snprintf(value, sizeof(value), "%d / %d", style.aspect_width,
                 style.aspect_height);
    } else if (id == CSP_GRID_TEMPLATE_AREAS) {
        (void) stylesheet_serialize_grid_template_areas(
            bridge->stylesheet, &style, value, sizeof(value));
    } else if (id == CSP_GRID_TEMPLATE_COLUMNS) {
        (void) stylesheet_serialize_grid_template_tracks(
            bridge->stylesheet, &style, false, value, sizeof(value));
    } else if (id == CSP_GRID_TEMPLATE_ROWS) {
        (void) stylesheet_serialize_grid_template_tracks(
            bridge->stylesheet, &style, true, value, sizeof(value));
    } else if (id == CSP_GAP
               || id == CSP_COLUMN_GAP) {
        if (computed_style_gap_is_percent(style.gap)) {
            snprintf(value, sizeof(value), "%d%%",
                     computed_style_gap_percent(style.gap));
        } else {
            snprintf(value, sizeof(value), "%dpx", style.gap);
        }
    } else if (id == CSP_ROW_GAP) {
        snprintf(value, sizeof(value), "%dpx", style.row_gap);
    } else {
        /* Reached only after every older case declined, so none of their
           paths grew. */
        (void) computed_style_serialize_retained(
            node, &style, id, value, sizeof(value));
    }
    return computed_style_string(context, value, empty);
}

/* HTML innerText for a connected, rendered element: the text as laid
   out, not the raw descendant text. display:none subtrees and elements that
   are never rendered (script, style, noscript while scripting runs,
   template, replaced elements' fallback) contribute nothing; block-level
   boxes separate lines, paragraphs a blank line, table rows a line and
   cells a tab; <br> is a line break; collapsible white space collapses
   across element boundaries and vanishes at line edges; visibility:hidden
   drops text but not visible descendants; text-transform applies (ASCII
   letters). Returns undefined when the receiver is not being rendered, for
   which script answers textContent as specified. Styles come from the
   bridge's computed-style cache (one resolution per element); the walk is
   iterative, bounded in nodes, depth and output bytes. */
#define RENDERED_TEXT_NODE_LIMIT 200000u
#define RENDERED_TEXT_BYTE_LIMIT (1024u * 1024u)
#define RENDERED_TEXT_DEPTH_LIMIT 256u

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
    bool truncated;
    bool at_start;
    bool pending_space;
    uint8_t pending_break;
} RenderedText;

typedef struct {
    lxb_dom_node_t *node;
    uint8_t white_space;
    uint8_t transform;
    bool visible;
    int8_t after;
} RenderedTextFrame;

static bool rendered_text_reserve(RenderedText *text, size_t extra)
{
    if (text->failed || text->truncated) return false;
    if (extra > RENDERED_TEXT_BYTE_LIMIT - text->length) {
        text->truncated = true;
        return false;
    }
    if (text->length + extra <= text->capacity) return true;
    size_t capacity = text->capacity == 0 ? 256u : text->capacity;
    while (capacity < text->length + extra) capacity *= 2u;
    if (capacity > RENDERED_TEXT_BYTE_LIMIT) {
        capacity = RENDERED_TEXT_BYTE_LIMIT;
    }
    char *grown = budget_realloc(text->budget, text->data, capacity);
    if (grown == NULL) {
        text->failed = true;
        return false;
    }
    text->data = grown;
    text->capacity = capacity;
    return true;
}

static void rendered_text_put(RenderedText *text, const char *bytes,
                              size_t length)
{
    if (length == 0 || !rendered_text_reserve(text, length)) return;
    memcpy(text->data + text->length, bytes, length);
    text->length += length;
}

/* Required line breaks collapse to their largest run and vanish at the
   start and end of the result. */
static void rendered_text_flush(RenderedText *text)
{
    if (text->pending_break == 0) return;
    if (text->length != 0) {
        for (uint8_t i = 0; i < text->pending_break; i++) {
            rendered_text_put(text, "\n", 1);
        }
    }
    text->pending_break = 0;
    text->at_start = true;
    text->pending_space = false;
}

static void rendered_text_break(RenderedText *text, uint8_t count)
{
    if (count > text->pending_break) text->pending_break = count;
    text->pending_space = false;
}

static void rendered_text_literal(RenderedText *text, const char *bytes,
                                  size_t length)
{
    rendered_text_flush(text);
    rendered_text_put(text, bytes, length);
    if (length != 0) text->at_start = bytes[length - 1] == '\n';
    text->pending_space = false;
}

static char rendered_text_case(char byte, uint8_t transform, bool word_start)
{
    if (transform == TEXT_TRANSFORM_UPPERCASE
        || (transform == TEXT_TRANSFORM_CAPITALIZE && word_start)) {
        return byte >= 'a' && byte <= 'z' ? (char) (byte - 32) : byte;
    }
    if (transform == TEXT_TRANSFORM_LOWERCASE) {
        return byte >= 'A' && byte <= 'Z' ? (char) (byte + 32) : byte;
    }
    return byte;
}

static bool rendered_text_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'
           || byte == '\f';
}

static void rendered_text_append(RenderedText *text,
                                 const RenderedTextFrame *context,
                                 const char *data, size_t length)
{
    if (!context->visible || length == 0) return;
    uint8_t mode = context->white_space;
    bool preserve = mode == WHITE_SPACE_PRE || mode == WHITE_SPACE_PRE_WRAP
                    || mode == WHITE_SPACE_BREAK_SPACES;
    bool keep_lines = mode == WHITE_SPACE_PRE_LINE;
    if (preserve) {
        rendered_text_flush(text);
        if (text->pending_space && !text->at_start) {
            rendered_text_put(text, " ", 1);
        }
        text->pending_space = false;
    }
    bool word_start = text->at_start || text->pending_space;
    for (size_t i = 0; i < length && !text->failed && !text->truncated;
         i++) {
        char byte = data[i];
        if (byte == '\r') {
            if (i + 1 < length && data[i + 1] == '\n') continue;
            byte = '\n';
        }
        if (preserve) {
            char out = rendered_text_case(byte, context->transform,
                                          word_start);
            rendered_text_put(text, &out, 1);
            text->at_start = byte == '\n';
            word_start = rendered_text_space(byte);
            continue;
        }
        if (byte == '\n' && keep_lines) {
            rendered_text_literal(text, "\n", 1);
            word_start = true;
            continue;
        }
        if (rendered_text_space(byte)) {
            if (!text->at_start && text->pending_break == 0) {
                text->pending_space = true;
            }
            word_start = true;
            continue;
        }
        rendered_text_flush(text);
        if (text->pending_space && !text->at_start) {
            rendered_text_put(text, " ", 1);
        }
        char out = rendered_text_case(byte, context->transform, word_start);
        rendered_text_put(text, &out, 1);
        text->at_start = false;
        text->pending_space = false;
        word_start = false;
    }
}

static bool rendered_text_skipped(lxb_dom_node_t *node)
{
    static const char *const skipped[] = {
        "script", "style", "noscript", "template", "head", "title", "meta",
        "link", "base", "canvas", "video", "audio", "iframe", "object",
        "embed", "select", "datalist"
    };
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    if (name == NULL) return false;
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (strlen(skipped[i]) == length
            && strncasecmp(name, skipped[i], length) == 0) return true;
    }
    return false;
}

static bool rendered_text_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && strncasecmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *rendered_text_next_element(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node->next; at != NULL; at = at->next) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) return at;
    }
    return NULL;
}

static bool rendered_text_style(DomBridge *bridge, lxb_dom_node_t *node,
                                ComputedStyle *style)
{
    return bridge_computed_style(bridge, node, style, NULL, NULL);
}

JSValue js_dom_rendered_text(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL || argc < 1) {
        return JS_UNDEFINED;
    }
    lxb_dom_node_t *root = js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (root == NULL || root->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !bridge_node_is_connected(root)) return JS_UNDEFINED;
    ComputedStyle style;
    /* Not being rendered: the receiver or an ancestor is undisplayed. */
    for (lxb_dom_node_t *at = root; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!rendered_text_style(bridge, at, &style)) return JS_UNDEFINED;
        if (style.display == DISPLAY_NONE || style.hidden) {
            return JS_UNDEFINED;
        }
    }
    if (!rendered_text_style(bridge, root, &style)) return JS_UNDEFINED;
    RenderedText text = {.budget = bridge->budget, .at_start = true};
    RenderedTextFrame stack[RENDERED_TEXT_DEPTH_LIMIT];
    size_t depth = 0;
    stack[depth++] = (RenderedTextFrame) {
        .node = root, .white_space = style.white_space_mode,
        .transform = (uint8_t) style.text_transform,
        .visible = !style.visibility_hidden, .after = 0
    };
    lxb_dom_node_t *at = root->first_child;
    uint32_t visited = 0;
    while (depth != 0 && !text.failed && !text.truncated) {
        if (at == NULL) {
            /* Leave the innermost open element. */
            RenderedTextFrame *closing = &stack[--depth];
            if (closing->after < 0) rendered_text_literal(&text, "\t", 1);
            else if (closing->after > 0) {
                rendered_text_break(&text, (uint8_t) closing->after);
            }
            at = depth == 0 ? NULL : closing->node->next;
            continue;
        }
        if (++visited > RENDERED_TEXT_NODE_LIMIT) break;
        const RenderedTextFrame *parent = &stack[depth - 1];
        if (at->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *data = document_text_data(at, &length);
            if (data != NULL) {
                rendered_text_append(&text, parent, data, length);
            }
            at = at->next;
            continue;
        }
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT
            || rendered_text_skipped(at)) {
            at = at->next;
            continue;
        }
        if (!rendered_text_style(bridge, at, &style)) {
            text.failed = true;
            break;
        }
        if (style.display == DISPLAY_NONE || style.hidden
            || depth == RENDERED_TEXT_DEPTH_LIMIT) {
            at = at->next;
            continue;
        }
        if (rendered_text_name_is(at, "br")) {
            if (!style.visibility_hidden)
                rendered_text_literal(&text, "\n", 1);
            at = at->next;
            continue;
        }
        int8_t after = 0;
        if (rendered_text_name_is(at, "p")) {
            after = 2;
        } else if (style.display == DISPLAY_BLOCK
                   || style.display == DISPLAY_FLOW_ROOT
                   || style.display == DISPLAY_FLEX
                   || style.display == DISPLAY_GRID
                   || style.display == DISPLAY_TABLE) {
            after = 1;
        } else if (style.display == DISPLAY_TABLE_ROW) {
            if (rendered_text_next_element(at) != NULL) after = 1;
        } else if (style.display == DISPLAY_TABLE_CELL) {
            if (rendered_text_next_element(at) != NULL) after = -1;
        }
        if (after > 0 && style.display != DISPLAY_TABLE_ROW) {
            rendered_text_break(&text, (uint8_t) after);
        }
        stack[depth++] = (RenderedTextFrame) {
            .node = at, .white_space = style.white_space_mode,
            .transform = (uint8_t) style.text_transform,
            .visible = !style.visibility_hidden, .after = after
        };
        at = at->first_child;
    }
    if (text.failed) {
        budget_free(bridge->budget, text.data);
        return JS_ThrowOutOfMemory(context);
    }
    JSValue value = JS_NewStringLen(
        context, text.data == NULL ? "" : text.data, text.length);
    budget_free(bridge->budget, text.data);
    return value;
}

JSValue js_computed_style_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || bridge->stylesheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT || argc < 2) {
        return JS_NewString(context, "");
    }
    size_t name_length = 0;
    const char *name = JS_ToCStringLen(context, &name_length, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    bool empty = false;
    JSValue value = computed_style_value(context, bridge, node, name,
                                         name_length, argc, argv, &empty);
    JS_FreeCString(context, name);
    return value;
}

/* __tilefinchComputedStyleRead(handle, name, pseudo) -> the value, or
   undefined for the script's general path. One call for the common read
   (getComputedStyle(el).prop), which otherwise canonicalizes the name,
   asks whether the element is connected and reads the value in three.
   `name` is canonicalized exactly as __tilefinchCssName does (custom
   properties as they are, cssFloat, then each ASCII capital to "-" and its
   lower case). Undefined when the element is not a connected native
   element (script decides with its own tree), the name is not a string,
   the value needs script's post-processing (flex-basis and the scroll
   margins and paddings resolve em terms) or is empty for a non-custom
   name (script falls back to inline text). */
JSValue js_computed_style_read(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL || argc < 2
        || !JS_IsString(argv[1])) return JS_UNDEFINED;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !bridge_node_is_connected(node)) return JS_UNDEFINED;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[1]);
    if (text == NULL) return JS_EXCEPTION;
    char canonical[128];
    size_t used = 0;
    bool usable = strlen(text) == length;
    if (!usable) {
        /* An embedded NUL: the general path decides. */
    } else if (length >= 2 && text[0] == '-' && text[1] == '-') {
        usable = length < sizeof(canonical);
        if (usable) memcpy(canonical, text, used = length);
    } else if (length == 8 && memcmp(text, "cssFloat", 8) == 0) {
        memcpy(canonical, "float", used = 5);
    } else {
        for (size_t i = 0; usable && i < length; i++) {
            unsigned char byte = (unsigned char) text[i];
            bool capital = byte >= 'A' && byte <= 'Z';
            if (used + (capital ? 2u : 1u) >= sizeof(canonical)) {
                usable = false;
                break;
            }
            if (capital) {
                canonical[used++] = '-';
                canonical[used++] = (char) (byte + 32u);
            } else {
                canonical[used++] = (char) byte;
            }
        }
    }
    JS_FreeCString(context, text);
    if (!usable) return JS_UNDEFINED;
    canonical[used] = '\0';
    bool custom = used >= 2 && canonical[0] == '-' && canonical[1] == '-';
    if (!custom
        && ((used == 10 && memcmp(canonical, "flex-basis", 10) == 0)
            || (used >= 13 && memcmp(canonical, "scroll-margin", 13) == 0)
            || (used >= 14
                && memcmp(canonical, "scroll-padding", 14) == 0)))
        return JS_UNDEFINED;
    bool empty = false;
    JSValue value = computed_style_value(context, bridge, node, canonical,
                                         used, argc, argv, &empty);
    if (empty && !custom && !JS_IsException(value)) {
        JS_FreeValue(context, value);
        return JS_UNDEFINED;
    }
    return value;
}

JSValue js_style_set(JSContext *context, JSValueConst this_value,
                     int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || argc < 3) return JS_FALSE;
    size_t wanted_length = 0, value_length = 0;
    const char *wanted = JS_ToCStringLen(context, &wanted_length, argv[1]);
    const char *value = JS_ToCStringLen(context, &value_length, argv[2]);
    if (wanted == NULL || value == NULL || wanted_length == 0
        || wanted_length > 96 || value_length > 512) {
        if (wanted != NULL) JS_FreeCString(context, wanted);
        if (value != NULL) JS_FreeCString(context, value);
        return JS_FALSE;
    }
    size_t old_length = 0;
    const char *old = document_attribute(node, "style", &old_length);
    char updated[1024];
    size_t used = 0;
    InlineDeclaration declaration;
    for (size_t at = 0;
         inline_declaration_next(old, old_length, &at, &declaration);) {
        if (inline_declaration_named(old, &declaration,
                                     wanted, wanted_length)
            || declaration.end == declaration.start) continue;
        size_t span = declaration.end - declaration.start;
        if (used + span + 1 >= sizeof(updated)) goto style_too_large;
        memcpy(updated + used, old + declaration.start, span); used += span;
        updated[used++] = ';';
    }
    if (value_length != 0) {
        if (used + wanted_length + value_length + 3 >= sizeof(updated)) {
            goto style_too_large;
        }
        memcpy(updated + used, wanted, wanted_length); used += wanted_length;
        updated[used++] = ':';
        updated[used++] = ' ';
        memcpy(updated + used, value, value_length); used += value_length;
        updated[used++] = ';';
    }
    updated[used] = '\0';
    /* CSSOM changes the style attribute, not a selector-visible attribute
       named after the property. Preserve :has([style]) invalidation while
       allowing unrelated :has() rules to keep ancestor/sibling caches. The
       old attribute storage is valid only before the DOM setter below. */
    bool relational = stylesheet_attribute_change_may_affect_has(
        bridge == NULL ? NULL : bridge->stylesheet, "style", 5,
        old, old_length, updated, used);
    lxb_status_t status;
    document_style_quiet_begin();
    status = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) "style", 5,
        (const lxb_char_t *) updated, used) == NULL
        ? LXB_STATUS_ERROR : LXB_STATUS_OK;
    document_style_quiet_end();
    if (status == LXB_STATUS_OK) {
        uint32_t modern = sparse_modern_property_mask(
            wanted, wanted_length);
        if (modern != 0 && bridge->stylesheet != NULL) {
            /* The mask is a monotonic presence summary, not stylesheet
               content. CSSOM can introduce one of these sparse properties
               after stylesheet construction, so keep the summary in sync
               before the mutation-triggered relayout resolves the node. */
            ((Stylesheet *) bridge->stylesheet)->modern_property_mask
                |= modern;
            ((Stylesheet *) bridge->stylesheet)->modern_typography_mask
                |= sparse_modern_typography_mask(wanted, wanted_length);
        }
        document_style_attribute_set_cssom_authorized(node, true);
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_INLINE_STYLE, node, wanted, wanted_length,
            relational, NULL, 0);
    }
    JS_FreeCString(context, value);
    JS_FreeCString(context, wanted);
    return JS_NewBool(context, status == LXB_STATUS_OK);

style_too_large:
    JS_FreeCString(context, value);
    JS_FreeCString(context, wanted);
    return JS_FALSE;
}

static ScriptMutationKind bridge_child_mutation_kind(
    DomBridge *bridge, lxb_dom_node_t *parent, lxb_dom_node_t *child)
{
    if (bridge != NULL && bridge->document != NULL
        && bridge->document->html != NULL && child != NULL
        && child->ns == LXB_NS_HTML && child->local_name == LXB_TAG_SCRIPT
        && parent != NULL
        && parent == lxb_dom_interface_node(
            lxb_html_document_head_element(bridge->document->html))
        && (child->parent == NULL || child->parent == parent)) {
        /* Keep the head's :empty state invariant as well. A sibling element
           guarantees nonemptiness before and after the move/removal. Bound
           this optional optimization independently of document size. */
        lxb_dom_node_t *sibling = parent->first_child;
        for (unsigned i = 0; sibling != NULL && i < 64;
             i++, sibling = sibling->next) {
            if (sibling != child && sibling->type == LXB_DOM_NODE_TYPE_ELEMENT)
                return SCRIPT_MUTATION_HEAD_SCRIPT;
        }
    }
    return SCRIPT_MUTATION_CHILD_LIST;
}

/* Record the connected parent a removed or moved child left, on the most
   recent record for (kind, node) whether it was new or coalesced. */
static void bridge_mutation_note_scope(
    DomBridge *bridge, ScriptMutationKind kind, const lxb_dom_node_t *node,
    lxb_dom_node_t *scope, bool last)
{
    if (scope == NULL || scope->type == LXB_DOM_NODE_TYPE_DOCUMENT) return;
    ScriptMutationJournal *journal = &bridge->mutations;
    for (size_t reverse = journal->count; reverse != 0; reverse--) {
        ScriptMutationRecord *record = &journal->records[reverse - 1];
        if (record->kind != kind || record->node != node) continue;
        if (record->scope == NULL) {
            record->scope = scope;
            record->removed_last = last;
        }
        return;
    }
}

/* Whether connected `node` has no later element sibling (bounded). */
static bool bridge_node_is_last_element(const lxb_dom_node_t *node)
{
    unsigned visited = 0;
    for (const lxb_dom_node_t *later = node->next; later != NULL;
         later = later->next) {
        if (later->type == LXB_DOM_NODE_TYPE_ELEMENT || ++visited > 64u)
            return false;
    }
    return true;
}

/* Journals the removal of connected `node` from its parent, classifying
   its subtree while it is still connected (bridge_mutated() ignores
   detached construction trees). */
static void bridge_note_removal(DomBridge *bridge, lxb_dom_node_t *node)
{
    BridgeMutationResourceFlags removed_resources =
        bridge_mutation_resource_subtree(bridge->document, node);
    ScriptMutationKind removal_kind =
        bridge_child_mutation_kind(bridge, node->parent, node);
    bridge_mutated(bridge, removal_kind, node, NULL, 0);
    bridge_mutation_note_scope(bridge, removal_kind, node, node->parent,
                               bridge_node_is_last_element(node));
    if ((removed_resources & (BRIDGE_MUTATION_RESOURCE_IMAGE
                              | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0) {
        bridge->mutations.resource_rebuild_required = true;
        if ((removed_resources & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0)
            bridge->mutations.image_rebuild_required = true;
        if ((removed_resources & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0)
            bridge->mutations.stylesheet_rebuild_required = true;
        bridge->mutations.image_resource_scan_required = false;
        if (tilefinch_trace_mutation_policy()) {
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            fprintf(stderr, "tilefinch: mutation-policy removal node=%.*s "
                    "flags=%u connected=%d\n", (int) name_length,
                    name == NULL ? "" : name, (unsigned) removed_resources,
                    (int) bridge_node_is_connected(node));
        }
    }
}

/* What a connected node about to move leaves behind. */
typedef struct {
    /* Journaled as a removal: it moves under a detached parent. */
    bool departed;
    /* It moves within the page: the :has() entries its old position can
       move (stylesheet_tree_change_has_entries), probed before the move. */
    bool probed;
    bool relational;
    uint64_t has_entries;
    uint32_t has_serial;
} BridgeDeparture;

/* A connected node about to move under a detached parent leaves the live
   page: journal it as the removal it is, before the move. One moving
   within the page has its old position probed now, for the record the
   move makes. */
static BridgeDeparture bridge_note_departure(DomBridge *bridge,
                                             lxb_dom_node_t *parent,
                                             lxb_dom_node_t *node)
{
    BridgeDeparture departure = {0};
    if (bridge == NULL || node == NULL || node->parent == NULL
        || !bridge_node_is_connected(node)) return departure;
    if (!bridge_node_is_connected(parent)) {
        bridge_note_removal(bridge, node);
        departure.departed = true;
        return departure;
    }
    departure.probed = true;
    departure.relational = stylesheet_tree_change_has_entries(
        bridge->stylesheet, node, node->parent, &departure.has_entries,
        &departure.has_serial);
    return departure;
}

static void bridge_child_inserted(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    bool was_detached, lxb_dom_node_t *old_parent,
    BridgeDeparture departure)
{
    /* A successful move into a detached construction tree still removes
       content from the live page: journaled as a removal before the move
       (bridge_note_departure), or, unclassified, as its old owner's. */
    if (!was_detached && !bridge_node_is_connected(node)) {
        if (!departure.departed)
            bridge_mutated(bridge, SCRIPT_MUTATION_UNKNOWN, old_parent,
                           NULL, 0);
        return;
    }
    size_t before = bridge->mutations.count;
    /* A move also changed the parent it left, which the classification
       of the new position does not see. */
    if (was_detached) {
        bridge_mutated(bridge, kind, node, NULL, 0);
    } else if (departure.probed && node->parent != NULL) {
        /* Both positions probed: the old one before the move, the new one
           now. */
        uint64_t entries = UINT64_MAX;
        uint32_t serial = 0;
        bool relational = stylesheet_tree_change_has_entries(
            bridge->stylesheet, node, node->parent, &entries, &serial);
        if (serial == 0 || serial != departure.has_serial) {
            entries = UINT64_MAX;
            serial = 0;
        } else {
            entries |= departure.has_entries;
        }
        bridge_mutated_summarized(bridge, kind, node, NULL, 0,
                                  relational || departure.relational, NULL,
                                  0, entries, serial);
    } else {
        bridge_mutated_with_relational(bridge, kind, node, NULL, 0, true,
                                       NULL, 0);
    }
    /* A prior removal/move of this node must win over a later re-insertion.
       Only a newly recorded child mutation can start a transient probe. */
    if ((kind == SCRIPT_MUTATION_CHILD_LIST || kind == SCRIPT_MUTATION_HEAD_SCRIPT)
        && bridge->mutations.count > before) {
        bridge->mutations.records[before].inserted_from_detached = was_detached;
    }
    if (!was_detached)
        bridge_mutation_note_scope(bridge, kind, node, old_parent, false);
}

JSValue js_dom_append(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *parent = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    lxb_dom_node_t *child = argc > 1
        ? js_rt_bridge_node_arg(context, bridge, argv[1]) : NULL;
    if (parent == NULL || child == NULL || parent == child) return JS_FALSE;
    ScriptMutationKind kind = bridge_child_mutation_kind(bridge, parent, child);
    bool was_detached = !bridge_node_is_connected(child);
    lxb_dom_node_t *old_parent = child->parent;
    BridgeDeparture departure = bridge_note_departure(bridge, parent, child);
    document_style_quiet_begin();
    lxb_dom_exception_code_t status = lxb_dom_node_append_child(parent, child);
    document_style_quiet_end();
    if (status == LXB_DOM_EXCEPTION_OK) {
        bridge_child_inserted(bridge, kind, child, was_detached, old_parent,
                              departure);
    }
    return JS_NewBool(context, status == LXB_DOM_EXCEPTION_OK);
}

JSValue js_dom_prepare_dynamic_subtree(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *parent = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (parent == NULL) return JS_FALSE;
    return JS_NewBool(
        context, js_rt_dynamic_prepare_subtree(context, parent) >= 0);
}

JSValue js_dom_append_many(JSContext *context,
                           JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *parent = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (parent == NULL || argc < 2) return JS_FALSE;
    JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
    uint32_t count = 0;
    if (JS_IsException(length_value)
        || JS_ToUint32(context, &count, length_value) < 0) {
        JS_FreeValue(context, length_value);
        return JS_EXCEPTION;
    }
    JS_FreeValue(context, length_value);
    if (count > DOM_MUTATION_BATCH_LIMIT) return JS_FALSE;

    lxb_dom_node_t **nodes = count == 0 ? NULL : budget_calloc(
        bridge->budget, count, sizeof(*nodes));
    if (count != 0 && nodes == NULL) return JS_ThrowOutOfMemory(context);
    for (uint32_t index = 0; index < count; index++) {
        JSValue value = JS_GetPropertyUint32(context, argv[1], index);
        if (JS_IsException(value)) {
            budget_free(bridge->budget, nodes);
            return JS_EXCEPTION;
        }
        nodes[index] = js_rt_bridge_node_arg(context, bridge, value);
        JS_FreeValue(context, value);
        if (nodes[index] == NULL || nodes[index] == parent
            || js_rt_node_is_strict_descendant(parent, nodes[index])) {
            budget_free(bridge->budget, nodes);
            return JS_FALSE;
        }
    }

    /* This is the relevant part of the DOM "insert" algorithm for the
       already-normalized list supplied by Element.append: establish every
       connection first, then run post-connection steps in argument order. */
    for (uint32_t index = 0; index < count; index++) {
        bool was_detached = !bridge_node_is_connected(nodes[index]);
        lxb_dom_node_t *old_parent = nodes[index]->parent;
        BridgeDeparture departure =
            bridge_note_departure(bridge, parent, nodes[index]);
        document_style_quiet_begin();
        lxb_dom_exception_code_t appended =
            lxb_dom_node_append_child(parent, nodes[index]);
        document_style_quiet_end();
        if (appended != LXB_DOM_EXCEPTION_OK) {
            budget_free(bridge->budget, nodes);
            return JS_FALSE;
        }
        bridge_child_inserted(
            bridge, SCRIPT_MUTATION_CHILD_LIST, nodes[index], was_detached,
            old_parent, departure);
    }
    if (js_rt_dynamic_prepare_subtree(context, parent) < 0) {
        budget_free(bridge->budget, nodes);
        return JS_FALSE;
    }
    budget_free(bridge->budget, nodes);
    return JS_TRUE;
}

JSValue js_dom_insert_before(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *parent = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    lxb_dom_node_t *node = argc > 1
        ? js_rt_bridge_node_arg(context, bridge, argv[1]) : NULL;
    lxb_dom_node_t *child = argc > 2
        ? js_rt_bridge_node_arg(context, bridge, argv[2]) : NULL;
    if (parent == NULL || node == NULL || child == NULL
        || node == parent) return JS_FALSE;
    /* The DOM pre-insert algorithm validates ancestry and reference-child
       ownership before detaching an existing node.  Calling Lexbor's raw
       list primitive here used to make insertBefore differ from appendChild:
       it could corrupt a tree for ancestor cycles, and it rejected the
       standards-defined insertBefore(node, node) no-op. */
    ScriptMutationKind kind = bridge_child_mutation_kind(bridge, parent, node);
    bool was_detached = !bridge_node_is_connected(node);
    lxb_dom_node_t *old_parent = node->parent;
    BridgeDeparture departure = bridge_note_departure(bridge, parent, node);
    document_style_quiet_begin();
    lxb_dom_exception_code_t status = lxb_dom_node_insert_before_spec(
        parent, node, child);
    document_style_quiet_end();
    if (status == LXB_DOM_EXCEPTION_OK) {
        bridge_child_inserted(bridge, kind, node, was_detached, old_parent,
                              departure);
    }
    return JS_NewBool(context, status == LXB_DOM_EXCEPTION_OK);
}

JSValue js_dom_remove(JSContext *context, JSValueConst this_value,
                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || node->parent == NULL) return JS_FALSE;
    bridge_note_removal(bridge, node);
    document_style_quiet_begin();
    lxb_dom_node_remove(node);
    document_style_quiet_end();
    return JS_TRUE;
}

JSValue js_dom_record_event(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->events_dispatched++;
    }
    return JS_UNDEFINED;
}

JSValue js_dom_record_event_handler(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL && bridge->result != NULL) {
        bridge->result->event_handlers_invoked++;
    }
    return JS_UNDEFINED;
}
