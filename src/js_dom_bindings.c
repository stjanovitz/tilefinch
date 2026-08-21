/* DOM bindings: node handle registry, stable-node identity, selector
   queries, traversal, creation, content/attribute/style access, mutation
   bindings, and mutation-journal classification.  Split from js_runtime.c;
   shares the runtime internals through js_runtime_internal.h.  Exported
   binding symbols keep their js_* names so the orchestrator's install chain
   preserves the script-observable property order. */
#include "style_internal.h"
#undef budget_malloc
#undef budget_calloc
#undef budget_realloc
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"

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

static int64_t bridge_invalidate_node_slot_impl(
    DomBridge *bridge, size_t slot, bool notify)
{
    if (bridge == NULL || slot >= bridge->node_count
        || bridge->nodes[slot] == NULL) return 0;
    int64_t retired_handle = bridge_node_handle(bridge, slot);
    bridge->nodes[slot] = NULL;
    bridge->node_owner_document_identities[slot] = 0;
    /* Never wrap an incarnation: a wrapped handle could make an arbitrarily
       old JavaScript wrapper refer to an unrelated replacement node.  A slot
       that exhausts its hundreds of thousands of incarnations is retired. */
    if (bridge->node_generations[slot]
        < DOM_BRIDGE_NODE_GENERATION_MAX) {
        bridge->node_generations[slot]++;
    }
    bridge->node_retention_flags[slot] =
        notify ? 0 : BRIDGE_NODE_PENDING_RETIRE_NOTIFY;
    if (bridge->result != NULL
        && bridge->result->dom_handle_slots_live != 0) {
        bridge->result->dom_handle_slots_live--;
    }
    if (notify) bridge_notify_node_state_retired(bridge, retired_handle);
    return retired_handle;
}

void bridge_invalidate_node_slot(DomBridge *bridge, size_t slot)
{
    (void) bridge_invalidate_node_slot_impl(bridge, slot, true);
}

static void bridge_notify_pending_node_retirements(DomBridge *bridge)
{
    if (bridge == NULL) return;
    for (size_t slot = 0; slot < bridge->node_count; slot++) {
        if ((bridge->node_retention_flags[slot]
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

int64_t js_rt_bridge_register_node(DomBridge *bridge, lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return 0;
    uintptr_t owner = js_rt_node_owner_identity(node);
    size_t reusable = DOM_BRIDGE_NODE_LIMIT;
    for (size_t i = 0; i < bridge->node_count; i++) {
        if (bridge->nodes[i] == node) {
            if (bridge->node_owner_document_identities[i] == owner) {
                return bridge_node_handle(bridge, i);
            }
            /* An allocator may reuse a just-destroyed address for a node in a
               different document.  The captured owner prevents an old handle
               from silently acquiring that new identity. */
            bridge_invalidate_node_slot(bridge, i);
            reusable = i;
        }
        if (reusable == DOM_BRIDGE_NODE_LIMIT
            && bridge->nodes[i] == NULL
            && bridge->node_generations[i] != 0
            && bridge->node_generations[i]
                   < DOM_BRIDGE_NODE_GENERATION_MAX) {
            reusable = i;
        }
    }
    if (reusable == DOM_BRIDGE_NODE_LIMIT) {
        if (bridge->node_count == DOM_BRIDGE_NODE_LIMIT) {
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

lxb_dom_node_t *js_rt_bridge_node_arg(JSContext *context,
                                DomBridge *bridge, JSValueConst value)
{
    int64_t handle = 0;
    if (JS_ToInt64(context, &handle, value) < 0 || handle <= 0) return NULL;
    size_t slot = 0;
    return js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        ? bridge->nodes[slot] : NULL;
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
    lxb_dom_node_t *node = bridge->nodes[slot];
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
        return JS_FALSE;
    }
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
    size_t released = bridge_discard_unretained_detached_subtree(
        bridge, root);
    if (released != 0) {
        if (bridge->result != NULL
            && bridge->result->dom_handle_wrapper_releases != SIZE_MAX) {
            js_rt_saturating_add_size(
                &bridge->result->dom_handle_wrapper_releases, released);
        }
        return JS_TRUE;
    }
    return JS_FALSE;
}

typedef enum {
    BRIDGE_SUBTREE_NOT_FOUND = 0,
    BRIDGE_SUBTREE_FOUND,
    BRIDGE_SUBTREE_INDETERMINATE
} BridgeSubtreeSearchResult;

static BridgeSubtreeSearchResult bridge_live_subtree_contains_node(
    lxb_dom_node_t *root, const lxb_dom_node_t *candidate,
    size_t ownership_depth)
{
    if (root == NULL || candidate == NULL) return BRIDGE_SUBTREE_NOT_FOUND;
    if (ownership_depth >= 8) return BRIDGE_SUBTREE_INDETERMINATE;
    DomDocumentOrderTraversal traversal = {
        .next = root, .boundary = root
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (at == candidate) return BRIDGE_SUBTREE_FOUND;
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
                    bridge_live_subtree_contains_node(
                        content, candidate, ownership_depth + 1);
                if (nested != BRIDGE_SUBTREE_NOT_FOUND) return nested;
            }
        }
    }
    return traversal.next == NULL
        ? BRIDGE_SUBTREE_NOT_FOUND : BRIDGE_SUBTREE_INDETERMINATE;
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

static BridgeSubtreeSearchResult bridge_subtree_has_retained_identity(
    const DomBridge *bridge, const lxb_dom_node_t *root)
{
    if (bridge == NULL || root == NULL) return BRIDGE_SUBTREE_INDETERMINATE;
    for (size_t i = 0; i < bridge->node_count; i++) {
        if (bridge->nodes[i] == NULL
            || bridge->node_retention_flags[i] == 0) continue;
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(
                (lxb_dom_node_t *) root, bridge->nodes[i], 0);
        if (result != BRIDGE_SUBTREE_NOT_FOUND) return result;
    }
    return BRIDGE_SUBTREE_NOT_FOUND;
}

static size_t bridge_discard_unretained_detached_subtree(
    DomBridge *bridge, lxb_dom_node_t *root)
{
    if (bridge == NULL || root == NULL || root->parent != NULL
        || bridge_subtree_has_retained_identity(bridge, root)
               != BRIDGE_SUBTREE_NOT_FOUND) return 0;
    unsigned char script_states[SCRIPT_DYNAMIC_NODE_LIMIT] = {0};
    for (size_t i = 0; i < bridge->script_element_count; i++) {
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(
                root, bridge->script_elements[i].node, 0);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        script_states[i] = result == BRIDGE_SUBTREE_FOUND;
    }
    unsigned char retire_slots[(DOM_BRIDGE_NODE_LIMIT + 7u) / 8u] = {0};
    for (size_t i = 0; i < bridge->node_count; i++) {
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(
                root, bridge->nodes[i], 0);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        if (result == BRIDGE_SUBTREE_FOUND) {
            retire_slots[i / 8u] |= (unsigned char) (1u << (i % 8u));
        }
    }
    size_t mutation_count =
        bridge->mutations.count < SCRIPT_MUTATION_JOURNAL_LIMIT
            ? bridge->mutations.count : SCRIPT_MUTATION_JOURNAL_LIMIT;
    unsigned char mutation_slots[
        (SCRIPT_MUTATION_JOURNAL_LIMIT + 7u) / 8u] = {0};
    bool mutations_retired = false;
    for (size_t i = 0; i < mutation_count; i++) {
        lxb_dom_node_t *target = bridge->mutations.records[i].node;
        if (target == NULL) continue;
        BridgeSubtreeSearchResult result =
            bridge_live_subtree_contains_node(root, target, 0);
        if (result == BRIDGE_SUBTREE_INDETERMINATE) return 0;
        if (result == BRIDGE_SUBTREE_FOUND) {
            mutation_slots[i / 8u] |= (unsigned char) (1u << (i % 8u));
            mutations_retired = true;
        }
    }
    if (!document_control_state_discard_subtree(bridge->document, root)) {
        return 0;
    }
    /* A detached-node reclamation can retire a mutation target before the
       host consumes the journal. Preserve the conservative rebuild signal,
       but never let layout reuse dereference a retired raw pointer.

       Only the records this subtree actually owns may be cleared. Nulling
       the whole journal also raised `overflowed`, which the history runtime
       reads as a destructive tick and answers with a layout-reuse cache
       reset plus a full-document resource scan; since assigning to
       textContent discards one detached child at a time, every such
       assignment paid for both. */
    if (mutations_retired) {
        for (size_t i = 0; i < mutation_count; i++) {
            if ((mutation_slots[i / 8u]
                 & (unsigned char) (1u << (i % 8u))) == 0) continue;
            bridge->mutations.records[i].node = NULL;
        }
        bridge->mutations.conservative_resource_scan = true;
        bridge->mutations.overflowed = true;
    }
    js_rt_script_element_states_purge_marked(bridge, script_states);
    size_t released = 0;
    for (size_t i = 0; i < bridge->node_count; i++) {
        if ((retire_slots[i / 8u]
             & (unsigned char) (1u << (i % 8u))) != 0) {
            (void) bridge_invalidate_node_slot_impl(bridge, i, false);
            released++;
        }
    }
    lxb_dom_node_destroy_deep(root);
    /* Cleanup callbacks are JavaScript. Run them only after native teardown
       and handle retirement are complete, so author reentrancy cannot destroy
       the subtree a second time. */
    bridge_notify_pending_node_retirements(bridge);
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

static void bridge_detach_and_discard_children(
    DomBridge *bridge, lxb_dom_node_t *parent)
{
    if (bridge == NULL || parent == NULL) return;
    bool trace = getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL;
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
        lxb_dom_node_remove(child);
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
        return JS_NewInt64(context, js_rt_bridge_register_node(bridge, node));
    }
    size_t ordinal = 0, name_length = 0;
    lxb_dom_node_t *node = bridge_element_at_ordinal(
        lxb_dom_interface_node(bridge->document->html), wanted, &ordinal, 0);
    const char *name = document_element_name(node, &name_length);
    if (name == NULL || strlen(tag_name) != name_length
        || strncasecmp(name, tag_name, name_length) != 0) node = NULL;
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, node));
}

static bool selector_list_matches(lxb_dom_node_t *node,
                                  const char *selector, size_t length,
                                  const lxb_dom_node_t *scope)
{
    size_t start = 0;
    int square = 0, round = 0;
    for (size_t i = 0; i <= length; i++) {
        char value = i < length ? selector[i] : ',';
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

static void script_element_states_register_parsed_subtree(
    DomBridge *bridge, lxb_dom_node_t *container)
{
    if (bridge == NULL || container == NULL) return;
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
            bridge, at, true);
        if (state != NULL) {
            state->programmatic = false;
            state->force_async = false;
            state->already_started = true;
        }
    }
}

static bool script_element_states_clone_subtree(
    DomBridge *bridge, lxb_dom_node_t *source, lxb_dom_node_t *clone,
    bool deep)
{
    if (bridge == NULL || source == NULL || clone == NULL) return false;
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
        ScriptElementState *source_state = js_rt_script_element_state_find(
            bridge, source_node);
        if (source_state != NULL) {
            ScriptElementState *clone_state = js_rt_script_element_state_register(
                bridge, clone_node, source_state->html);
            if (clone_state == NULL) {
                memset(bridge->script_elements + initial_count, 0,
                       (bridge->script_element_count - initial_count)
                           * sizeof(bridge->script_elements[0]));
                bridge->script_element_count = initial_count;
                return false;
            }
            clone_state->programmatic = source_state->programmatic;
            clone_state->force_async = source_state->force_async;
            clone_state->already_started = source_state->already_started;
        }
        if (!deep) break;
    }
    return true;
}

static lxb_dom_node_t *selector_query(
    DomBridge *bridge, lxb_dom_node_t *node,
    const lxb_dom_node_t *boundary, const char *selector, size_t length)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, boundary)) return NULL;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (selector_list_matches(at, selector, length, boundary)
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

static bool bridge_mutation_subtree_contains_svg(lxb_dom_node_t *root)
{
    enum { MAXIMUM_WORK = 256, MAXIMUM_DEPTH = 48 };
    if (root == NULL) return false;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    for (;;) {
        /* Bounded-out is resource-sensitive: a conservative refresh is
           preferable to retaining a stale raster below a large subtree. */
        if (++work > MAXIMUM_WORK) return true;
        if (bridge_mutation_node_name_is(node, "svg")) return true;
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
    BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT = 1u << 2
} BridgeMutationResourceFlags;

static bool bridge_mutation_link_is_stylesheet(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *rel = document_attribute(node, "rel", &length);
    if (rel == NULL) return false;
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) rel[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) rel[at])) at++;
        if (at - start == sizeof("stylesheet") - 1
            && strncasecmp(rel + start, "stylesheet",
                           sizeof("stylesheet") - 1) == 0) return true;
    }
    return false;
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
        if (bridge_mutation_name_equal(name, length, "rel")) {
            return BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        return bridge_mutation_link_is_stylesheet(node)
               && (bridge_mutation_name_equal(name, length, "href")
                   || bridge_mutation_name_equal(name, length, "media")
                   || bridge_mutation_name_equal(name, length, "crossorigin")
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
    lxb_dom_node_t *root)
{
    /* DOM parent links let this be a stackless depth-first walk.  The caps
       prevent hostile author trees from turning one bridge call into an
       unbounded PSP stack/work boundary; bounded-out means "unknown", never
       "clean".  The root's siblings are intentionally outside the subtree. */
    enum { MAXIMUM_SUBTREE_WORK = 256, MAXIMUM_SUBTREE_DEPTH = 48 };
    if (root == NULL) return BRIDGE_MUTATION_RESOURCE_NONE;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    BridgeMutationResourceFlags result = BRIDGE_MUTATION_RESOURCE_NONE;
    for (;;) {
        if (++work > MAXIMUM_SUBTREE_WORK) {
            return result | BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT;
        }
        if (bridge_mutation_node_name_is(node, "style")) {
            result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            bool stylesheet_link =
                bridge_mutation_node_name_is(node, "link")
                && bridge_mutation_link_is_stylesheet(node);
            if (stylesheet_link) {
                result |= BRIDGE_MUTATION_RESOURCE_STYLESHEET;
            }
            static const char *const attributes[] = {
                "src", "srcset", "data-src", "data-srcset", "href",
                "xlink:href", "rel", "media", "type", "sizes", "poster"
            };
            for (size_t i = 0;
                 i < sizeof(attributes) / sizeof(attributes[0]); i++) {
                /* A connected rel mutation is destructive because its old
                   value may have removed a stylesheet. For an inserted
                   subtree the final rel is fully known; preload, icon and
                   modulepreload links must not masquerade as stylesheets. */
                if (!stylesheet_link
                    && bridge_mutation_node_name_is(node, "link")
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
    if (stored_length != attribute_length) return false;
    for (size_t i = 0; i < attribute_length; i++) {
        if (record->attribute[i]
            != (char) tolower((unsigned char) attribute[i])) return false;
    }
    return true;
}

static void bridge_mutated_with_relational(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length,
    bool relational_selector_sensitive)
{
    if (bridge == NULL) return;
    /* Mutating a detached construction tree cannot affect layout or the
       connected resource graph. Its eventual insertion is represented by
       one child-list record over the complete subtree. Besides avoiding
       wasted work, this keeps a framework assembling one card from
       overflowing the PSP's fixed mutation journal before it is appended. */
    if (node != NULL && !bridge_node_is_connected(node)) return;
    document_note_connected_mutation(bridge->document);
    /* Attribute and tree mutations can change which connected <base href> is
       first. Invalidating conservatively keeps the mutation fast and avoids a
       second subtree walk on the PSP hot path. */
    bridge->document_base_dirty = true;
    if (bridge->result != NULL) {
        bridge->result->dom_mutations++;
        bridge->result->relayout_required = true;
    }
    if (bridge->relayout_dirty != NULL) *bridge->relayout_dirty = true;

    ScriptMutationJournal *journal = &bridge->mutations;
    bool coalesced = false;
    for (size_t reverse = journal->count; reverse != 0; reverse--) {
        if (bridge_mutation_record_equal(
                &journal->records[reverse - 1], kind, node, attribute,
                attribute_length)) {
            coalesced = true;
            break;
        }
    }
    if (!coalesced && journal->count < SCRIPT_MUTATION_JOURNAL_LIMIT) {
        ScriptMutationRecord *record = &journal->records[journal->count++];
        record->kind = kind;
        record->node = node;
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
    } else if (!coalesced) {
        journal->overflowed = true;
        /* Records beyond the fixed journal cannot be independently audited,
           so retain the old whole-document fingerprint oracle. */
        journal->conservative_resource_scan = true;
    }

    bool resource_rebuild = false;
    bool image_resource_scan = false;
    bool image_resource_refresh = false;
    bool conservative_scan = false;
    switch (kind) {
    case SCRIPT_MUTATION_TEXT:
        if (node == NULL) conservative_scan = true;
        else resource_rebuild = bridge_mutation_inside_style(node);
        break;
    case SCRIPT_MUTATION_INLINE_STYLE:
        /* The prior declaration may have owned an image even when the new
           one does not. Keep only image-bearing properties destructive;
           ordinary geometry/color declarations are layout-only. */
        resource_rebuild = bridge_mutation_style_property_is_resource(
            attribute, attribute_length);
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
        resource_rebuild =
            bridge_mutation_name_equal(
                attribute, attribute_length, "style")
            || bridge_mutation_node_name_is(node, "style")
            || (flags & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
        if ((flags & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0) {
            if (bridge_mutation_node_name_is(node, "img")) {
                image_resource_refresh = true;
            } else {
                resource_rebuild = true;
            }
        }
        break;
    }
    case SCRIPT_MUTATION_INNER_HTML:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(node);
        resource_rebuild = node != NULL
            && (bridge_mutation_inside_style(node)
                || (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                               | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0);
        /* innerHTML may have removed the last resource, which cannot be
           inferred by walking only the replacement subtree. */
        conservative_scan = !resource_rebuild
            || (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        break;
    }
    case SCRIPT_MUTATION_CHILD_LIST:
    {
        BridgeMutationResourceFlags subtree =
            bridge_mutation_resource_subtree(node);
        bool connected = node != NULL && bridge_node_is_connected(node);
        if (!connected) {
            /* A removal can invalidate retained decoded surfaces and source
               order, so it remains destructive when resources are present. */
            resource_rebuild =
                (subtree & (BRIDGE_MUTATION_RESOURCE_IMAGE
                            | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0;
        } else {
            resource_rebuild =
                (subtree & BRIDGE_MUTATION_RESOURCE_STYLESHEET) != 0;
            image_resource_scan =
                (subtree & BRIDGE_MUTATION_RESOURCE_IMAGE) != 0;
        }
        conservative_scan =
            (subtree & BRIDGE_MUTATION_RESOURCE_BOUNDED_OUT) != 0;
        break;
    }
    case SCRIPT_MUTATION_UNKNOWN:
    default:
        conservative_scan = true;
        break;
    }
    bool inline_svg_sensitive =
        node != NULL
        && (bridge_mutation_inside_svg(node)
            || ((kind == SCRIPT_MUTATION_ATTRIBUTE
                 || kind == SCRIPT_MUTATION_INLINE_STYLE)
                && (bridge_mutation_name_equal(
                        attribute, attribute_length, "class")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "id")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "style")
                    || bridge_mutation_name_equal(
                        attribute, attribute_length, "color"))
               && bridge_mutation_subtree_contains_svg(node)));
    journal->resource_rebuild_required |= resource_rebuild;
    journal->image_resource_scan_required |= image_resource_scan;
    journal->image_resource_refresh_required |=
        image_resource_refresh || inline_svg_sensitive;
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
        && getenv("TILEFINCH_TRACE_MUTATION_POLICY") != NULL) {
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

static void bridge_mutated(DomBridge *bridge, ScriptMutationKind kind,
                           lxb_dom_node_t *node,
                           const char *attribute,
                           size_t attribute_length)
{
    /* Callers without exact pre-mutation state remain conservative. */
    bridge_mutated_with_relational(
        bridge, kind, node, attribute, attribute_length, true);
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
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, body));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, html));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, found));
}

#define DOM_QUERY_RESULT_LIMIT 128

static void query_all_nodes(DomBridge *bridge, lxb_dom_node_t *node,
                            const lxb_dom_node_t *boundary,
                            const char *selector, size_t length,
                            JSContext *context, JSValue array,
                            uint32_t *count, uint32_t result_limit)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, boundary)) return;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < result_limit;
         at = dom_document_order_next(&traversal)) {
        if (selector_list_matches(at, selector, length, boundary)
            && bridge_traversal_node_visible(
                bridge, at, &traversal)) {
            int64_t handle = js_rt_bridge_register_node(bridge, at);
            if (handle != 0) {
                (void) JS_SetPropertyUint32(
                    context, array, (*count)++,
                    JS_NewInt64(context, handle));
            }
        }
    }
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
    if (length <= 512 && result_limit != 0) {
        query_all_nodes(bridge, root, boundary, selector, length,
                        context, array, &count, result_limit);
    }
    JS_FreeCString(context, selector);
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
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < limit;
         at = dom_document_order_next(&traversal)) {
        if (selector_list_matches(at, selector, length, boundary)
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

static lxb_dom_node_t *find_element_id_exact(lxb_dom_node_t *node,
                                             const char *identifier,
                                             size_t length)
{
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        size_t value_length = 0;
        const char *value = document_attribute(at, "id", &value_length);
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
        if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
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
    lxb_dom_node_t *found = length <= 128
        ? find_element_id_exact(root, identifier, length) : NULL;
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
        JSValue handle = JS_NewInt64(
            context, js_rt_bridge_register_node(bridge, found));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, related));
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
        if (handle != 0) {
            (void) JS_SetPropertyUint32(context, array, count++,
                                        JS_NewInt64(context, handle));
        }
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
        if (handle != 0) {
            (void) JS_SetPropertyUint32(context, array, count++,
                                        JS_NewInt64(context, handle));
        }
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
        if (handle != 0) {
            (void) JS_SetPropertyUint32(context, array, count++,
                                        JS_NewInt64(context, handle));
        }
    }
    return array;
}

JSValue js_dom_tag_name(JSContext *context,
                        JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
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

JSValue js_dom_node_type(JSContext *context,
                         JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    int type = node == NULL ? 0
               : (node->type == LXB_DOM_NODE_TYPE_ELEMENT ? 1
                  : (node->type == LXB_DOM_NODE_TYPE_TEXT ? 3
                     : (node->type == LXB_DOM_NODE_TYPE_COMMENT ? 8
                     : (node->type == LXB_DOM_NODE_TYPE_DOCUMENT ? 9
                     : (node->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE ? 10
                                                                : 11)))));
    return JS_NewInt32(context, type);
}

static void append_descendants(DomBridge *bridge, lxb_dom_node_t *node,
                               int32_t what_to_show, JSContext *context,
                               JSValue array, uint32_t *count)
{
    DomDocumentOrderTraversal traversal;
    if (!bridge_document_order_traversal_init(
            bridge, &traversal, node, node)) return;
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
            if (handle != 0) {
                (void) JS_SetPropertyUint32(context, array, (*count)++,
                                            JS_NewInt64(context, handle));
            }
        }
    }
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
    if (root != NULL) append_descendants(bridge, root, what_to_show,
                                         context, array, &count);
    return array;
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
    return JS_NewInt64(context, js_rt_bridge_register_node(
        bridge, lxb_dom_interface_node(element->content)));
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
    lxb_dom_node_t *clone = node == NULL ? NULL
                                         : lxb_dom_node_clone(node, deep);
    if (clone != NULL
        && (!bridge_clear_cloned_node_user(clone, deep)
            || !script_element_states_clone_subtree(
                   bridge, node, clone, deep))) {
        lxb_dom_node_destroy_deep(clone);
        clone = NULL;
    }
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, clone));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(bridge, node));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(
        bridge, fragment == NULL ? NULL : lxb_dom_interface_node(fragment)));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(
        bridge, node == NULL ? NULL : lxb_dom_interface_node(node)));
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
    return JS_NewInt64(context, js_rt_bridge_register_node(
        bridge, node == NULL ? NULL : lxb_dom_interface_node(node)));
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
    size_t length = 0;
    lxb_char_t *text = lxb_dom_node_text_content(node, &length);
    JSValue value = text == NULL ? JS_NewString(context, "")
                                 : JS_NewStringLen(context,
                                     (const char *) text, length);
    if (text != NULL) {
        lxb_dom_document_destroy_text(node->owner_document, text);
    }
    return value;
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
    if (node->type == LXB_DOM_NODE_TYPE_TEXT
        || node->type == LXB_DOM_NODE_TYPE_COMMENT) {
        status = lxb_dom_node_text_content_set(
            node, (const lxb_char_t *) text, length);
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
                lxb_dom_node_remove(removed);
                (void) bridge_discard_unretained_detached_subtree(
                    bridge, removed);
            }
            if (replacement != NULL
                && lxb_dom_node_append_child(
                       node, lxb_dom_interface_node(replacement))
                       != LXB_DOM_EXCEPTION_OK) {
                lxb_dom_node_destroy_deep(
                    lxb_dom_interface_node(replacement));
                status = LXB_STATUS_ERROR;
            }
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
    size_t length = 0;
    const char *html = JS_ToCStringLen(context, &length, argv[1]);
    if (html == NULL) return JS_EXCEPTION;
    bool valid = false, changed = false;
    if (length <= DOM_INNER_HTML_LIMIT) {
        static const char div_name[] = "div";
        size_t context_name_length = sizeof(div_name) - 1;
        const char *context_name = div_name;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            context_name = document_element_name(node, &context_name_length);
        }
        lxb_dom_element_t *container = lxb_dom_document_create_element(
            &bridge->document->html->dom_document,
            (const lxb_char_t *) context_name, context_name_length, NULL);
        lxb_dom_node_t *container_node = container == NULL ? NULL
            : lxb_dom_interface_node(container);
        valid = container != NULL && context_name != NULL
            && document_set_element_inner_html(
                   bridge->document, container_node, html, length);
        if (valid) {
            bridge_detach_and_discard_children(bridge, node);
            changed = true;
            while (container_node->first_child != NULL) {
                lxb_dom_node_t *child = container_node->first_child;
                lxb_dom_node_remove(child);
                if (lxb_dom_node_append_child(node, child)
                    != LXB_DOM_EXCEPTION_OK) {
                    valid = false;
                    break;
                }
            }
        }
        if (container_node != NULL) {
            lxb_dom_node_destroy_deep(container_node);
        }
    }
    JS_FreeCString(context, html);
    if (changed) {
        script_element_states_register_parsed_subtree(bridge, node);
        bridge_mutated(bridge, SCRIPT_MUTATION_INNER_HTML, node, NULL, 0);
        (void) js_rt_dynamic_prepare_subtree(context, node);
    }
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
    size_t old_value_length = 0;
    const lxb_char_t *old_value = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, &old_value_length);
    bool old_present = old_value != NULL || lxb_dom_element_has_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    bool relational_selector_sensitive =
        stylesheet_attribute_change_may_affect_has(
            bridge == NULL ? NULL : bridge->stylesheet,
            name, name_length,
            old_present ? (old_value == NULL ? "" : (const char *) old_value)
                        : NULL,
            old_present ? old_value_length : 0,
            value, value_length);
    lxb_dom_attr_t *attribute = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length, (const lxb_char_t *) value, value_length);
    bool async_attribute = name_length == 5
        && strncasecmp(name, "async", 5) == 0;
    bool source_attribute = name_length == 3
        && strncasecmp(name, "src", 3) == 0;
    if (attribute != NULL) {
        if (name_length == 5 && strncasecmp(name, "style", 5) == 0) {
            document_style_attribute_set_cssom_authorized(node, false);
        }
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
            name, name_length, relational_selector_sensitive);
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
    return JS_NewBool(context, attribute != NULL);
}

JSValue js_dom_set_control_value(JSContext *context,
                                 JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || bridge->document == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT || argc < 2) {
        return JS_FALSE;
    }
    size_t length = 0;
    const char *value = JS_ToCStringLen(context, &length, argv[1]);
    if (value == NULL) return JS_EXCEPTION;
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
    int64_t handle = js_rt_bridge_register_node(bridge, owner);
    return handle > 0 ? JS_NewInt64(context, handle) : JS_NULL;
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
    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));
    JS_ComputeMemoryUsage(JS_GetRuntime(context), &usage);
    int64_t remaining =
        usage.malloc_limit > 0 && usage.malloc_size >= 0
            && usage.malloc_size < usage.malloc_limit
        ? usage.malloc_limit - usage.malloc_size : 0;
    return JS_NewInt64(context, remaining);
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
    lxb_status_t status = lxb_dom_element_remove_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        name_length);
    if (existed && status == LXB_STATUS_OK) {
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
            name, name_length, relational_selector_sensitive);
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

static bool sparse_scroll_property(const char *name, size_t length)
{
    static const char *const properties[] = {
        "cursor",
        "overscroll-behavior", "overscroll-behavior-x",
        "overscroll-behavior-y", "overscroll-behavior-inline",
        "overscroll-behavior-block",
        "scroll-behavior",
        "scroll-margin", "scroll-margin-top", "scroll-margin-right",
        "scroll-margin-bottom", "scroll-margin-left",
        "scroll-padding", "scroll-padding-top", "scroll-padding-right",
        "scroll-padding-bottom", "scroll-padding-left",
        "scroll-snap-align", "scroll-snap-stop", "scroll-snap-type",
        "scrollbar-color", "scrollbar-width"
    };
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
        if (property_equal(
                name, length, properties[i], strlen(properties[i]))) {
            return true;
        }
    }
    return false;
}

static bool sparse_modern_property(const char *name, size_t length)
{
    static const char *const properties[] = {
        "user-select", "-webkit-user-select", "touch-action",
        "text-size-adjust", "-webkit-text-size-adjust", "resize",
        "text-wrap", "text-wrap-style", "translate", "rotate", "scale",
        "isolation", "hyphens", "tab-size", "font-kerning",
        "text-rendering", "mix-blend-mode", "backdrop-filter",
        "-webkit-backdrop-filter", "border-start-start-radius",
        "border-start-end-radius", "border-end-start-radius",
        "border-end-end-radius"
    };
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
        if (property_equal(
                name, length, properties[i], strlen(properties[i]))) {
            return true;
        }
    }
    return false;
}

static uint16_t sparse_modern_property_mask(const char *name, size_t length)
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
    JSValue result = JS_NewString(context, "");
    for (size_t at = 0; style != NULL && at < style_length;) {
        size_t end = at;
        while (end < style_length && style[end] != ';') end++;
        size_t colon = at;
        while (colon < end && style[colon] != ':') colon++;
        size_t name_start = at, name_end = colon;
        while (name_start < name_end
               && isspace((unsigned char) style[name_start])) name_start++;
        while (name_end > name_start
               && isspace((unsigned char) style[name_end - 1])) name_end--;
        if (colon < end && property_equal(style + name_start,
                                          name_end - name_start,
                                          wanted, wanted_length)) {
            size_t value_start = colon + 1, value_end = end;
            while (value_start < value_end
                   && isspace((unsigned char) style[value_start])) value_start++;
            while (value_end > value_start
                   && isspace((unsigned char) style[value_end - 1])) value_end--;
            JS_FreeValue(context, result);
            result = JS_NewStringLen(context, style + value_start,
                                     value_end - value_start);
            break;
        }
        at = end + (end < style_length);
    }
    JS_FreeCString(context, wanted);
    return result;
}

static ComputedStyle bridge_computed_style(DomBridge *bridge,
                                           lxb_dom_node_t *node)
{
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node; at != NULL && count < 64;
         at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) ancestors[count++] = at;
    }
    ComputedStyle computed = {0};
    bool have_parent = false;
    while (count != 0) {
        computed = style_for_node(bridge->stylesheet, ancestors[--count],
                                  have_parent ? &computed : NULL);
        have_parent = true;
    }
    return computed;
}

static bool bridge_inline_property(lxb_dom_node_t *node,
                                   const char *wanted,
                                   size_t wanted_length,
                                   const char **value,
                                   size_t *value_length)
{
    size_t style_length = 0;
    const char *style = document_attribute(node, "style", &style_length);
    bool found = false;
    for (size_t at = 0; style != NULL && at < style_length;) {
        size_t end = at;
        int parentheses = 0;
        char quote = '\0';
        while (end < style_length) {
            char character = style[end];
            if (quote != '\0') {
                if (character == quote && (end == 0 || style[end - 1] != '\\')) {
                    quote = '\0';
                }
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
        size_t colon = at;
        while (colon < end && style[colon] != ':') colon++;
        size_t name_start = at, name_end = colon;
        while (name_start < name_end
               && isspace((unsigned char) style[name_start])) name_start++;
        while (name_end > name_start
               && isspace((unsigned char) style[name_end - 1])) name_end--;
        if (colon < end
            && property_equal(style + name_start, name_end - name_start,
                              wanted, wanted_length)) {
            size_t start = colon + 1, finish = end;
            while (start < finish
                   && isspace((unsigned char) style[start])) start++;
            while (finish > start
                   && isspace((unsigned char) style[finish - 1])) finish--;
            /* !important is cascade syntax, not part of the computed value. */
            size_t important = finish;
            while (important > start
                   && isspace((unsigned char) style[important - 1])) {
                important--;
            }
            if (important >= start + 10
                && strncasecmp(style + important - 10, "!important", 10)
                       == 0) {
                finish = important - 10;
                while (finish > start
                       && isspace((unsigned char) style[finish - 1])) finish--;
            }
            *value = style + start;
            *value_length = finish - start;
            found = true;
        }
        at = end + (end < style_length);
    }
    return found;
}

static bool bridge_transition_duration(DomBridge *bridge,
                                       lxb_dom_node_t *node,
                                       char *output, size_t output_size)
{
    const char *authored = NULL;
    size_t authored_length = 0;
    bool shorthand = false;
    if (!bridge_inline_property(
            node, "transition-duration", 19,
            &authored, &authored_length)) {
        shorthand = bridge_inline_property(
            node, "transition", 10, &authored, &authored_length);
    }
    if (authored == NULL) {
        snprintf(output, output_size, "0s");
        return true;
    }
    char resolved[256];
    StyleResolveScratch saved = *bridge->stylesheet->resolve_scratch;
    bridge->stylesheet->resolve_scratch->resolution_node = node;
    bridge->stylesheet->resolve_scratch->resolution_pseudo = PSEUDO_NONE;
    bool valid = style_resolve_value(
        bridge->stylesheet, authored, authored_length, resolved,
        sizeof(resolved), 0);
    *bridge->stylesheet->resolve_scratch = saved;
    if (!valid) {
        snprintf(output, output_size, "0s");
        return true;
    }
    const char *text = resolved;
    size_t length = strlen(resolved);
    for (size_t at = 0; at < length;) {
        while (at < length && (isspace((unsigned char) text[at])
                              || text[at] == ',')) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) text[end])
               && text[end] != ',') end++;
        if (end == at) break;
        char token[48];
        size_t token_length = end - at;
        if (token_length < sizeof(token)) {
            memcpy(token, text + at, token_length);
            token[token_length] = '\0';
            char *unit = NULL;
            double number = strtod(token, &unit);
            bool seconds = unit != token && number >= 0.0
                && ((strcasecmp(unit, "s") == 0)
                    || (strcasecmp(unit, "ms") == 0));
            if (seconds) {
                snprintf(output, output_size, "%.*s",
                         (int) token_length, text + at);
                return true;
            }
        }
        /* The first time in each transition shorthand is its duration.
           A longhand is only a comma-separated list of time values. */
        at = end;
        if (!shorthand && at < length && text[at] == ',') continue;
    }
    snprintf(output, output_size, "0s");
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
    bool used_geometry_property =
        property_equal(name, name_length, "width", 5)
        || property_equal(name, name_length, "height", 6)
        || property_equal(name, name_length, "text-indent", 11);
    if ((bridge->relayout_dirty != NULL && *bridge->relayout_dirty)
        || (bridge->layout == NULL && used_geometry_property)) {
        (void) js_rt_bridge_flush_synchronous_layout(bridge);
    }
    ComputedStyle style = bridge_computed_style(bridge, node);
    PseudoElement pseudo = PSEUDO_NONE;
    if (argc > 2) {
        size_t pseudo_length = 0;
        const char *pseudo_name =
            JS_ToCStringLen(context, &pseudo_length, argv[2]);
        if (pseudo_name == NULL) {
            JS_FreeCString(context, name);
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
            ComputedStyle parent = style;
            style = style_for_pseudo(bridge->stylesheet, node, pseudo,
                                     &parent);
        }
        JS_FreeCString(context, pseudo_name);
    }
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
    bool sparse_scroll = pseudo == PSEUDO_NONE
        && sparse_scroll_property(name, name_length);
    bool sparse_modern = pseudo == PSEUDO_NONE
        && sparse_modern_property(name, name_length);
    bool retained_scroll = sparse_scroll
        && (retained_scroll_box_shorthand(
                bridge->stylesheet, node, name, name_length,
                value, sizeof(value))
            || style_retained_property_value(
                bridge->stylesheet, node, name, name_length,
                value, sizeof(value)));
    if (!retained_scroll
        && sparse_scroll
        && property_equal(name, name_length, "cursor", 6)) {
        for (lxb_dom_node_t *at = node->parent;
             at != NULL && !retained_scroll; at = at->parent) {
            retained_scroll = style_retained_property_value(
                bridge->stylesheet, at, name, name_length,
                value, sizeof(value));
        }
    }
    bool retained_modern = sparse_modern
        && style_retained_property_value(
            bridge->stylesheet, node,
            property_equal(name, name_length, "-webkit-user-select", 19)
                ? "user-select"
                : (property_equal(
                       name, name_length, "-webkit-text-size-adjust", 24)
                   ? "text-size-adjust" : name),
            property_equal(name, name_length, "-webkit-user-select", 19)
                ? 11u
                : (property_equal(
                       name, name_length, "-webkit-text-size-adjust", 24)
                   ? 16u : name_length),
            value, sizeof(value));
    bool serialize_computed_modern = sparse_modern
        && (property_equal(name, name_length, "user-select", 11)
            || property_equal(name, name_length,
                              "-webkit-user-select", 19)
            || property_equal(name, name_length, "touch-action", 12)
            || property_equal(name, name_length, "resize", 6)
            || property_equal(name, name_length, "text-wrap", 9)
            || property_equal(name, name_length, "text-wrap-style", 15)
            || property_equal(name, name_length, "isolation", 9)
            || property_equal(name, name_length, "text-size-adjust", 16)
            || property_equal(name, name_length,
                              "-webkit-text-size-adjust", 24));
    bool authored_text_adjust_none = retained_modern
        && (property_equal(name, name_length, "text-size-adjust", 16)
            || property_equal(name, name_length,
                              "-webkit-text-size-adjust", 24))
        && strcasecmp(value, "none") == 0;
    bool authored_touch_manipulation = retained_modern
        && property_equal(name, name_length, "touch-action", 12)
        && strcasecmp(value, "manipulation") == 0;
    bool authored_touch_combined = retained_modern
        && property_equal(name, name_length, "touch-action", 12)
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
        if (property_equal(name, name_length, "user-select", 11)
            || property_equal(name, name_length,
                              "-webkit-user-select", 19)) {
            static const char *const values[] = {
                "auto", "text", "none", "all"
            };
            unsigned mode = computed_style_user_select(&style);
            snprintf(value, sizeof(value), "%s",
                     mode < 4u ? values[mode] : "auto");
        } else if (property_equal(name, name_length, "touch-action", 12)) {
            static const char *const values[] = {
                "auto", "none", "pan-x", "pan-y"
            };
            snprintf(value, sizeof(value), "%s",
                     authored_touch_manipulation ? "manipulation"
                     : (authored_touch_combined ? "pan-x pan-y"
                     : (style.touch_action < 4u
                        ? values[style.touch_action] : "auto")));
        } else if (property_equal(name, name_length, "resize", 6)) {
            static const char *const values[] = {
                "none", "both", "horizontal", "vertical"
            };
            snprintf(value, sizeof(value), "%s",
                     style.resize_mode < 4u
                         ? values[style.resize_mode] : "none");
        } else if (property_equal(name, name_length, "text-wrap", 9)
                   || property_equal(name, name_length,
                                     "text-wrap-style", 15)) {
            StyleTextWrap mode = computed_style_text_wrap(&style);
            snprintf(value, sizeof(value), "%s",
                     mode == STYLE_TEXT_WRAP_BALANCE ? "balance"
                     : (mode == STYLE_TEXT_WRAP_PRETTY ? "pretty"
                        : (property_equal(name, name_length,
                                          "text-wrap-style", 15)
                           ? "auto" : "wrap")));
        } else if (property_equal(name, name_length, "isolation", 9)) {
            snprintf(value, sizeof(value), "%s",
                     computed_style_isolation_isolate(&style)
                         ? "isolate" : "auto");
        } else if (property_equal(name, name_length, "text-size-adjust", 16)
                   || property_equal(name, name_length,
                                     "-webkit-text-size-adjust", 24)) {
            if (authored_text_adjust_none) {
                snprintf(value, sizeof(value), "100%%");
            } else if (style.font_size_unit != 0u) {
                snprintf(value, sizeof(value), "%u%%",
                         (unsigned) style.font_size_unit - 1u);
            } else {
                snprintf(value, sizeof(value), "auto");
            }
        } else if (property_equal(
                       name, name_length, "border-start-start-radius", 25)
                   || property_equal(
                       name, name_length, "border-start-end-radius", 23)
                   || property_equal(
                       name, name_length, "border-end-start-radius", 23)
                   || property_equal(
                       name, name_length, "border-end-end-radius", 21)) {
            snprintf(value, sizeof(value), "0px");
        } else {
            snprintf(value, sizeof(value), "none");
        }
    } else if (sparse_scroll) {
        const char *initial =
            property_equal(name, name_length, "scroll-snap-align", 17)
                ? "none"
            : property_equal(name, name_length, "scroll-snap-stop", 16)
                ? "normal"
            : property_equal(name, name_length, "scroll-snap-type", 16)
                ? "none"
            : property_equal(name, name_length, "scrollbar-color", 15)
                ? "auto"
            : property_equal(name, name_length, "scrollbar-width", 15)
                ? "auto"
            : property_equal(name, name_length, "cursor", 6)
                ? "auto"
            : property_equal(name, name_length, "scroll-behavior", 15)
                ? "auto"
            : property_equal(name, name_length, "overscroll-behavior", 19)
                ? "auto"
            : property_equal(name, name_length, "overscroll-behavior-x", 21)
                ? "auto"
            : property_equal(name, name_length, "overscroll-behavior-y", 21)
                ? "auto"
            : property_equal(
                  name, name_length, "overscroll-behavior-inline", 26)
                ? "auto"
            : property_equal(
                  name, name_length, "overscroll-behavior-block", 25)
                ? "auto"
                : "0px";
        snprintf(value, sizeof(value), "%s", initial);
    } else if (name_length >= 3 && name[0] == '-' && name[1] == '-') {
        (void) style_custom_property_value(
            bridge->stylesheet, node, pseudo, name, name_length,
            value, sizeof(value));
    } else if (property_equal(
                   name, name_length, "transition-duration", 19)) {
        (void) bridge_transition_duration(
            bridge, node, value, sizeof(value));
    } else if (property_equal(name, name_length, "display", 7)) {
        snprintf(value, sizeof(value), "%s", display_names[style.display]);
    } else if (property_equal(name, name_length, "visibility", 10)) {
        snprintf(value, sizeof(value), "%s",
                 style.visibility_hidden ? "hidden" : "visible");
    } else if (property_equal(name, name_length, "opacity", 7)) {
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
    } else if (property_equal(name, name_length, "color", 5)) {
        (void) serialize_computed_color(
            value, sizeof(value), style.color, style.color_alpha);
    } else if (property_equal(name, name_length, "background-color", 16)) {
        (void) serialize_computed_color(
            value, sizeof(value),
            style.has_background ? style.background : 0,
            style.has_background ? style.background_alpha : 0);
    } else if (property_equal(name, name_length, "background-origin", 17)
               || property_equal(name, name_length,
                                 "background-clip", 15)) {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(&style));
        bool origin = property_equal(
            name, name_length, "background-origin", 17);
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
    } else if (property_equal(name, name_length, "border-spacing", 14)) {
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
    } else if (property_equal(name, name_length, "background-image", 16)) {
        if (style.background_image == NULL || style.background_image[0] == '\0') {
            snprintf(value, sizeof(value), "none");
        } else {
            snprintf(value, sizeof(value), "url(\"%s\")",
                     style.background_image);
        }
    } else if (property_equal(name, name_length, "content", 7)) {
        if (!style.generated_content) {
            snprintf(value, sizeof(value), "none");
        } else if (style.generated_text != NULL) {
            snprintf(value, sizeof(value), "\"%.*s\"",
                     (int) style.generated_text_length,
                     style.generated_text);
        } else {
            snprintf(value, sizeof(value), "\"\"");
        }
    } else if (property_equal(name, name_length, "background-size", 15)) {
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
                           ? "%%" : "px");
            }
            if ((style.background_size_flags
                 & STYLE_BACKGROUND_HEIGHT_AUTO) != 0) {
                snprintf(height, sizeof(height), "auto");
            } else {
                snprintf(height, sizeof(height), "%u%s",
                         style.background_height,
                         (style.background_size_flags
                          & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
                           ? "%%" : "px");
            }
            snprintf(value, sizeof(value), "%s %s", width, height);
        } else {
            snprintf(value, sizeof(value), "%s", style.background_fit == 1
                     ? "cover" : (style.background_fit == 2
                                   ? "contain" : "auto"));
        }
    } else if (property_equal(name, name_length,
                              "background-position", 19)) {
        snprintf(value, sizeof(value), "%d%% %d%%",
                 style.background_position_x,
                 style.background_position_y);
    } else if (property_equal(name, name_length, "box-shadow", 10)) {
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
    } else if (property_equal(name, name_length, "text-shadow", 11)) {
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
    } else if (property_equal(name, name_length, "object-fit", 10)) {
        const char *fit = "fill";
        if (style.object_fit == STYLE_OBJECT_FIT_COVER) fit = "cover";
        else if (style.object_fit == STYLE_OBJECT_FIT_CONTAIN) fit = "contain";
        else if (style.object_fit == STYLE_OBJECT_FIT_NONE) fit = "none";
        else if (style.object_fit == STYLE_OBJECT_FIT_SCALE_DOWN) {
            fit = "scale-down";
        }
        snprintf(value, sizeof(value), "%s", fit);
    } else if (property_equal(name, name_length, "object-position", 15)) {
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
    } else if (property_equal(name, name_length, "appearance", 10)
               || property_equal(name, name_length,
                                 "-webkit-appearance", 18)) {
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
    } else if (property_equal(name, name_length, "justify-self", 12)) {
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
    } else if (property_equal(name, name_length, "justify-items", 13)) {
        static const char *const item_names[] = {
            "start", "center", "end", "stretch", "baseline"
        };
        size_t item = style.justify_items;
        if (item >= sizeof(item_names) / sizeof(item_names[0])) {
            item = ALIGN_STRETCH;
        }
        snprintf(value, sizeof(value), "%s", item_names[item]);
    } else if (property_equal(name, name_length, "place-self", 10)) {
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
    } else if (property_equal(name, name_length, "place-items", 11)) {
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
    } else if (property_equal(name, name_length, "place-content", 13)) {
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
    } else if (property_equal(name, name_length, "font-size", 9)) {
        int fixed = computed_style_font_size_fixed(&style);
        if ((fixed & 63) == 0) {
            snprintf(value, sizeof(value), "%dpx", fixed / 64);
        } else {
            snprintf(value, sizeof(value), "%.6fpx", fixed / 64.0);
        }
    } else if (property_equal(name, name_length, "font-family", 11)) {
        FontFamily family = font_family_is_web(style.font_family)
            ? font_family_web_fallback(style.font_family)
            : style.font_family;
        snprintf(value, sizeof(value), "%s",
                 family == FONT_MONOSPACE ? "monospace"
                 : family == FONT_SERIF ? "serif" : "sans-serif");
    } else if (property_equal(name, name_length, "text-indent", 11)) {
        const LayoutNodeBox *box = bridge->layout == NULL ? NULL
            : layout_box_for_node(bridge->layout, node);
        int used_indent = 0;
        if (!style_length_resolve(
                bridge->stylesheet, style.text_indent,
                box != NULL ? box->content_width : 0, &used_indent)) {
            used_indent = 0;
        }
        snprintf(value, sizeof(value), "%dpx", used_indent);
    } else if (property_equal(name, name_length, "text-overflow", 13)) {
        snprintf(value, sizeof(value), "%s",
                 computed_style_text_overflow_ellipsis(&style)
                   ? "ellipsis" : "clip");
    } else if (property_equal(name, name_length, "font-weight", 11)) {
        snprintf(value, sizeof(value), "%u",
                 style.font_weight != 0 ? style.font_weight
                                         : (style.font_bold ? 700u : 400u));
    } else if (property_equal(name, name_length, "font-style", 10)) {
        snprintf(value, sizeof(value), "%s",
                 style.font_italic ? "italic" : "normal");
    } else if (property_equal(name, name_length, "position", 8)) {
        snprintf(value, sizeof(value), "%s", style.fixed_position ? "fixed"
                 : style.sticky_position ? "sticky"
                 : style.out_of_flow ? "absolute"
                 : style.relative_position ? "relative" : "static");
    } else if (property_equal(name, name_length, "top", 3)) {
        if (!style.has_top) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.top,
                      style.inset_percent_mask & STYLE_INSET_TOP_PERCENT
                      ? "%" : "px");
    } else if (property_equal(name, name_length, "right", 5)) {
        if (!style.has_right) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.right,
                      style.inset_percent_mask & STYLE_INSET_RIGHT_PERCENT
                      ? "%" : "px");
    } else if (property_equal(name, name_length, "bottom", 6)) {
        if (!style.has_bottom) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.bottom,
                      style.inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT
                      ? "%" : "px");
    } else if (property_equal(name, name_length, "left", 4)) {
        if (!style.has_left) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%d%s", style.left,
                      style.inset_percent_mask & STYLE_INSET_LEFT_PERCENT
                      ? "%" : "px");
    } else if (property_equal(name, name_length, "overflow", 8)) {
        const char *x = style.overflow_x_clip_only ? "clip"
            : computed_style_overflow_x_hidden(&style) ? "hidden"
            : (style.overflow_x_scroll ? "auto" : "visible");
        const char *y = style.overflow_y_clip_only
            ? "clip" : (style.overflow_y_scroll ? "auto" : "visible");
        if (strcmp(x, y) == 0) snprintf(value, sizeof(value), "%s", x);
        else snprintf(value, sizeof(value), "%s %s", x, y);
    } else if (property_equal(name, name_length, "overflow-x", 10)) {
        snprintf(value, sizeof(value), "%s",
                 style.overflow_x_clip_only ? "clip"
                 : computed_style_overflow_x_hidden(&style) ? "hidden"
                 : (style.overflow_x_scroll ? "auto" : "visible"));
    } else if (property_equal(name, name_length, "overflow-y", 10)) {
        snprintf(value, sizeof(value), "%s", style.overflow_y_clip_only
                 ? "clip" : (style.overflow_y_scroll ? "auto" : "visible"));
    } else if (property_equal(name, name_length, "scrollbar-gutter", 16)) {
        snprintf(value, sizeof(value), "%s",
                 style.scrollbar_gutter_stable ? "stable" : "auto");
    } else if (property_equal(name, name_length, "border-radius", 13)) {
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
    } else if (property_equal(name, name_length,
                              "border-top-left-radius", 22)
               || property_equal(name, name_length,
                                 "border-top-right-radius", 23)
               || property_equal(name, name_length,
                                 "border-bottom-right-radius", 26)
               || property_equal(name, name_length,
                                 "border-bottom-left-radius", 25)) {
        int code = stylesheet_border_radius_code(
            bridge->stylesheet, &style);
        unsigned corner = name[7] == 't'
            ? (name[11] == 'l' ? 0u : 1u)
            : (name[14] == 'r' ? 2u : 3u);
        snprintf(value, sizeof(value), "%dpx",
                 style_border_radius_corner(code, corner));
    } else if (property_equal(name, name_length, "border-collapse", 15)) {
        snprintf(value, sizeof(value), "%s",
                 style.table_border_collapse ? "collapse" : "separate");
    } else if (property_equal(name, name_length, "caption-side", 12)) {
        snprintf(value, sizeof(value), "%s",
                 style.order >= 200000 ? "bottom" : "top");
    } else if (property_equal(name, name_length, "outline-width", 13)) {
        snprintf(value, sizeof(value), "%upx",
                 computed_style_outline_width(&style));
    } else if (property_equal(name, name_length, "outline-style", 13)) {
        static const char *const outline_names[] = {
            "none", "solid", "dashed", "dotted"
        };
        unsigned outline = computed_style_outline_style(&style);
        if (outline >= sizeof(outline_names) / sizeof(outline_names[0])) {
            outline = STYLE_OUTLINE_NONE;
        }
        snprintf(value, sizeof(value), "%s", outline_names[outline]);
    } else if (property_equal(name, name_length, "outline-offset", 14)) {
        snprintf(value, sizeof(value), "%dpx",
                 computed_style_outline_offset(&style));
    } else if (property_equal(name, name_length, "outline-color", 13)) {
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
    } else if (property_equal(name, name_length, "clip-path", 9)) {
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
    } else if (property_equal(name, name_length, "white-space", 11)) {
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
    } else if (property_equal(name, name_length, "text-transform", 14)) {
        static const char *const transform_names[] = {
            "none", "uppercase", "lowercase", "capitalize"
        };
        size_t transform = style.text_transform;
        if (transform >= sizeof(transform_names)
                         / sizeof(transform_names[0])) {
            transform = TEXT_TRANSFORM_NONE;
        }
        snprintf(value, sizeof(value), "%s", transform_names[transform]);
    } else if (property_equal(name, name_length, "box-sizing", 10)) {
        snprintf(value, sizeof(value), "%s",
                 style.box_sizing_border_box ? "border-box" : "content-box");
    } else if (property_equal(name, name_length, "flex-direction", 14)) {
        static const char *direction_names[] = {
            "row", "row-reverse", "column", "column-reverse"
        };
        snprintf(value, sizeof(value), "%s",
                 direction_names[style.flex_direction]);
    } else if (property_equal(name, name_length, "flex-wrap", 9)) {
        snprintf(value, sizeof(value), "%s", style.flex_wrap_reverse
                 ? "wrap-reverse" : (style.flex_wrap ? "wrap" : "nowrap"));
    } else if (property_equal(name, name_length, "justify-content", 15)) {
        static const char *justify_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        size_t justify = style.justify_content;
        if (justify >= sizeof(justify_names) / sizeof(justify_names[0])) {
            justify = JUSTIFY_START;
        }
        snprintf(value, sizeof(value), "%s", justify_names[justify]);
    } else if (property_equal(name, name_length, "align-items", 11)) {
        static const char *align_names[] = {
            "flex-start", "center", "flex-end", "stretch", "baseline"
        };
        size_t align = style.align_items;
        if (align >= sizeof(align_names) / sizeof(align_names[0])) {
            align = ALIGN_STRETCH;
        }
        snprintf(value, sizeof(value), "%s", align_names[align]);
    } else if (property_equal(name, name_length, "align-self", 10)) {
        static const char *align_self_names[] = {
            "auto", "flex-start", "center", "flex-end", "stretch",
            "baseline"
        };
        snprintf(value, sizeof(value), "%s",
                 align_self_names[style.align_self]);
    } else if (property_equal(name, name_length, "align-content", 13)) {
        static const char *align_content_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        snprintf(value, sizeof(value), "%s",
                 align_content_names[style.align_content]);
    } else if (property_equal(name, name_length, "order", 5)) {
        snprintf(value, sizeof(value), "%d", style.order);
    } else if (property_equal(name, name_length, "z-index", 7)) {
        if (style.has_z_index) snprintf(value, sizeof(value), "%d", style.z_index);
        else snprintf(value, sizeof(value), "auto");
    } else if (property_equal(name, name_length, "width", 5)) {
        if (style.has_width) {
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
        } else {
            const LayoutNodeBox *box = bridge->layout == NULL ? NULL
                : layout_box_for_node(bridge->layout, node);
            if (box != NULL) {
                int used = box->width;
                if (!style.box_sizing_border_box) {
                    used -= style.border.left + style.border.right
                            + style.padding.left + style.padding.right;
                }
                if (used < 0) used = 0;
                snprintf(value, sizeof(value), "%dpx", used);
            }
        }
    } else if (property_equal(name, name_length, "min-width", 9)) {
        if (style.min_width_auto) snprintf(value, sizeof(value), "auto");
        else if (style.min_width == STYLE_LENGTH_MIN_CONTENT)
            snprintf(value, sizeof(value), "min-content");
        else if (style.min_width == STYLE_LENGTH_MAX_CONTENT)
            snprintf(value, sizeof(value), "max-content");
        else if (style.min_width == STYLE_LENGTH_FIT_CONTENT)
            snprintf(value, sizeof(value), "fit-content");
        else snprintf(value, sizeof(value), "%d%s", style.min_width,
                      style.min_width_percent ? "%" : "px");
    } else if (property_equal(name, name_length, "max-width", 9)) {
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
    } else if (property_equal(name, name_length, "height", 6)
               && style.has_height) {
        snprintf(value, sizeof(value), "%d%s", style.height,
                 style.height_percent ? "%" : "px");
    } else if (property_equal(name, name_length, "min-height", 10)) {
        snprintf(value, sizeof(value), "%d%s", style.min_height,
                 style.min_height_percent ? "%" : "px");
    } else if (property_equal(name, name_length, "max-height", 10)) {
        if (style.max_height == STYLE_LENGTH_NONE) {
            snprintf(value, sizeof(value), "none");
        } else if (style.max_height == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, sizeof(value), "min-content");
        }
        else snprintf(value, sizeof(value), "%d%s", style.max_height,
                      style.max_height_percent ? "%" : "px");
    } else if (property_equal(name, name_length, "margin-top", 10)) {
        if (style.margin_top_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.top);
    } else if (property_equal(name, name_length, "margin-right", 12)) {
        if (style.margin_right_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.right);
    } else if (property_equal(name, name_length, "margin-bottom", 13)) {
        if (style.margin_bottom_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.bottom);
    } else if (property_equal(name, name_length, "margin-left", 11)) {
        if (style.margin_left_auto) snprintf(value, sizeof(value), "auto");
        else snprintf(value, sizeof(value), "%dpx", style.margin.left);
    } else if (property_equal(name, name_length, "padding-top", 11)) {
        snprintf(value, sizeof(value), "%dpx", style.padding.top);
    } else if (property_equal(name, name_length, "padding-right", 13)) {
        snprintf(value, sizeof(value), "%dpx", style.padding.right);
    } else if (property_equal(name, name_length, "padding-bottom", 14)) {
        snprintf(value, sizeof(value), "%dpx", style.padding.bottom);
    } else if (property_equal(name, name_length, "padding-left", 12)) {
        snprintf(value, sizeof(value), "%dpx", style.padding.left);
    } else if (property_equal(name, name_length, "padding", 7)) {
        snprintf(value, sizeof(value), "%dpx %dpx %dpx %dpx",
                 style.padding.top, style.padding.right,
                 style.padding.bottom, style.padding.left);
    } else if (property_equal(name, name_length, "border-top-width", 16)) {
        snprintf(value, sizeof(value), "%dpx", style.border.top);
    } else if (property_equal(name, name_length, "border-right-width", 18)) {
        snprintf(value, sizeof(value), "%dpx", style.border.right);
    } else if (property_equal(name, name_length, "border-bottom-width", 19)) {
        snprintf(value, sizeof(value), "%dpx", style.border.bottom);
    } else if (property_equal(name, name_length, "border-left-width", 17)) {
        snprintf(value, sizeof(value), "%dpx", style.border.left);
    } else if (property_equal(name, name_length, "border-width", 12)) {
        snprintf(value, sizeof(value), "%dpx %dpx %dpx %dpx",
                 style.border.top, style.border.right,
                 style.border.bottom, style.border.left);
    } else if (property_equal(name, name_length, "border-top-style", 16)
               || property_equal(
                      name, name_length, "border-right-style", 18)
               || property_equal(
                      name, name_length, "border-bottom-style", 19)
               || property_equal(
                      name, name_length, "border-left-style", 17)) {
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
    } else if (property_equal(name, name_length, "border-top-color", 16)
               || property_equal(
                      name, name_length, "border-right-color", 18)
               || property_equal(
                      name, name_length, "border-bottom-color", 19)
               || property_equal(
                      name, name_length, "border-left-color", 17)) {
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
    } else if (property_equal(name, name_length, "transform", 9)) {
        if (!style.has_transform) {
            snprintf(value, sizeof(value), "none");
        } else {
            snprintf(value, sizeof(value), "translate(%d%s, %d%s)",
                     style.transform_x,
                     style.transform_x_percent ? "%" : "px",
                     style.transform_y,
                     style.transform_y_percent ? "%" : "px");
        }
    } else if (property_equal(
                   name, name_length, "transform-origin", 16)) {
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
    } else if (property_equal(name, name_length, "perspective", 11)) {
        snprintf(value, sizeof(value), "%s",
                 style.has_perspective ? "1px" : "none");
    } else if (property_equal(name, name_length, "filter", 6)) {
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
    } else if (property_equal(name, name_length, "contain", 7)) {
        snprintf(value, sizeof(value), "%s",
                 style.has_layout_containment ? "paint" : "none");
    } else if (property_equal(
                   name, name_length, "content-visibility", 18)) {
        snprintf(value, sizeof(value), "%s",
                 style.content_visibility == STYLE_CONTENT_VISIBILITY_HIDDEN
                    ? "hidden"
                    : (style.content_visibility
                           == STYLE_CONTENT_VISIBILITY_AUTO
                       ? "auto" : "visible"));
    } else if (property_equal(
                   name, name_length, "-webkit-line-clamp", 18)) {
        unsigned clamp = computed_style_line_clamp(&style);
        if (clamp == 0) snprintf(value, sizeof(value), "none");
        else snprintf(value, sizeof(value), "%u",
                      clamp);
    } else if (property_equal(name, name_length, "will-change", 11)) {
        snprintf(value, sizeof(value), "%s",
                 style.will_change_transform ? "transform" : "auto");
    } else if (property_equal(name, name_length, "pointer-events", 14)) {
        snprintf(value, sizeof(value), "%s",
                 style.pointer_events_none ? "none" : "auto");
    } else if (property_equal(name, name_length, "float", 5)) {
        static const char *const float_names[] = {
            "none", "left", "right"
        };
        size_t mode = style.float_mode;
        if (mode >= sizeof(float_names) / sizeof(float_names[0])) mode = 0;
        snprintf(value, sizeof(value), "%s", float_names[mode]);
    } else if (property_equal(name, name_length, "aspect-ratio", 12)
               && style.aspect_width > 0 && style.aspect_height > 0) {
        snprintf(value, sizeof(value), "%d / %d", style.aspect_width,
                 style.aspect_height);
    } else if (property_equal(
                   name, name_length, "grid-template-areas", 19)) {
        (void) stylesheet_serialize_grid_template_areas(
            bridge->stylesheet, &style, value, sizeof(value));
    } else if (property_equal(
                   name, name_length, "grid-template-columns", 21)) {
        (void) stylesheet_serialize_grid_template_tracks(
            bridge->stylesheet, &style, false, value, sizeof(value));
    } else if (property_equal(
                   name, name_length, "grid-template-rows", 18)) {
        (void) stylesheet_serialize_grid_template_tracks(
            bridge->stylesheet, &style, true, value, sizeof(value));
    } else if (property_equal(name, name_length, "gap", 3)
               || property_equal(name, name_length, "column-gap", 10)) {
        if (computed_style_gap_is_percent(style.gap)) {
            snprintf(value, sizeof(value), "%d%%",
                     computed_style_gap_percent(style.gap));
        } else {
            snprintf(value, sizeof(value), "%dpx", style.gap);
        }
    } else if (property_equal(name, name_length, "row-gap", 7)) {
        snprintf(value, sizeof(value), "%dpx", style.row_gap);
    }
    JS_FreeCString(context, name);
    return JS_NewString(context, value);
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
    for (size_t at = 0; old != NULL && at < old_length;) {
        size_t end = at;
        while (end < old_length && old[end] != ';') end++;
        size_t colon = at;
        while (colon < end && old[colon] != ':') colon++;
        size_t name_start = at, name_end = colon;
        while (name_start < name_end
               && isspace((unsigned char) old[name_start])) name_start++;
        while (name_end > name_start
               && isspace((unsigned char) old[name_end - 1])) name_end--;
        bool replace = colon < end
            && property_equal(old + name_start, name_end - name_start,
                              wanted, wanted_length);
        if (!replace && end > at) {
            size_t span = end - at;
            if (used + span + 1 >= sizeof(updated)) goto style_too_large;
            memcpy(updated + used, old + at, span); used += span;
            updated[used++] = ';';
        }
        at = end + (end < old_length);
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
    lxb_status_t status;
    status = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) "style", 5,
        (const lxb_char_t *) updated, used) == NULL
        ? LXB_STATUS_ERROR : LXB_STATUS_OK;
    if (status == LXB_STATUS_OK) {
        uint16_t modern = sparse_modern_property_mask(
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
        bridge_mutated(bridge, SCRIPT_MUTATION_INLINE_STYLE, node,
                       wanted, wanted_length);
    }
    JS_FreeCString(context, value);
    JS_FreeCString(context, wanted);
    return JS_NewBool(context, status == LXB_STATUS_OK);

style_too_large:
    JS_FreeCString(context, value);
    JS_FreeCString(context, wanted);
    return JS_FALSE;
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
    lxb_dom_exception_code_t status = lxb_dom_node_append_child(parent, child);
    if (status == LXB_DOM_EXCEPTION_OK) {
        bridge_mutated(bridge, SCRIPT_MUTATION_CHILD_LIST, child, NULL, 0);
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
        if (lxb_dom_node_append_child(parent, nodes[index])
            != LXB_DOM_EXCEPTION_OK) {
            budget_free(bridge->budget, nodes);
            return JS_FALSE;
        }
        bridge_mutated(
            bridge, SCRIPT_MUTATION_CHILD_LIST, nodes[index], NULL, 0);
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
    lxb_dom_exception_code_t status = lxb_dom_node_insert_before_spec(
        parent, node, child);
    if (status == LXB_DOM_EXCEPTION_OK) {
        bridge_mutated(bridge, SCRIPT_MUTATION_CHILD_LIST, node, NULL, 0);
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
    /* Classify the connected subtree before detaching it. bridge_mutated()
       deliberately ignores detached construction trees. */
    BridgeMutationResourceFlags removed_resources =
        bridge_mutation_resource_subtree(node);
    bridge_mutated(bridge, SCRIPT_MUTATION_CHILD_LIST, node, NULL, 0);
    if ((removed_resources & (BRIDGE_MUTATION_RESOURCE_IMAGE
                              | BRIDGE_MUTATION_RESOURCE_STYLESHEET)) != 0) {
        bridge->mutations.resource_rebuild_required = true;
        bridge->mutations.image_resource_scan_required = false;
    }
    lxb_dom_node_remove(node);
    return JS_TRUE;
}

JSValue js_dom_record_event(JSContext *context,
                            JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    bridge->result->events_dispatched++;
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
