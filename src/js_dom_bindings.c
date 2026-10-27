/* DOM bindings: selector queries, traversal, creation, content/attribute/style access, mutation
   bindings, and mutation-journal classification.  The node-handle table is
   js_dom_handles.inc, included below.  Split from js_runtime.c;
   shares the runtime internals through js_runtime_internal.h.  Exported
   binding symbols keep their js_* names so the orchestrator's install chain
   preserves the script-observable property order. */
#include "style_cssom.h"
#include "style_cache_internal.h"
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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "js_dom_handles.inc"

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

#include "js_event_state.inc"

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
    if ((scan->bridge->node_retention_flags[slot] & BRIDGE_NODE_RETAINING)
        != 0) return true;
    scan->retire_slots[slot / 8u] |= (unsigned char) (1u << (slot % 8u));
    if (slot < scan->first_slot) scan->first_slot = slot;
    if (slot > scan->last_slot) scan->last_slot = slot;
    return false;
}

static void bridge_adopted_sheet_unpin(DomBridge *bridge,
                                       lxb_dom_node_t *sheet);

/* The preflight walk below proves the depth/visit bounds before teardown.
   HTML-specific Lexbor destructors free their element record, but not its
   attributes. Deep destroy also follows child links, not template.content;
   the template destructor frees the fragment itself but not its children. */
static void bridge_prepare_owned_tree_destroy(DomBridge *bridge,
                                               lxb_dom_node_t *root,
                                               size_t ownership_depth)
{
    if (root == NULL || ownership_depth >= 8) return;
    DomDocumentOrderTraversal traversal = {.next = root, .boundary = root};
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT || at->ns != LXB_NS_HTML)
            continue;
        lxb_dom_element_t *html = lxb_dom_interface_element(at);
        for (size_t count = 0; html->first_attr != NULL
             && count < DOM_TRAVERSAL_VISIT_LIMIT; count++) {
            lxb_dom_attr_t *attr = html->first_attr;
            (void) lxb_dom_element_attr_remove(html, attr);
            lxb_dom_attr_interface_destroy(attr);
        }
        size_t length = 0;
        const char *name = document_element_name(at, &length);
        if (name == NULL || length != 8 || strncasecmp(name, "template", 8) != 0)
            continue;
        lxb_html_template_element_t *element = lxb_html_interface_template(at);
        lxb_dom_node_t *content = element->content == NULL ? NULL
            : lxb_dom_interface_node(element->content);
        if (content == NULL) continue;
        /* Child-link retirement does not cross template.content. Announce
           each owned fragment while all its nodes still exist, so native
           image/style/layout consumers cannot retain freed descendants. */
        images_retire_subtree(bridge->images, content);
        if (bridge->node_retirement != NULL)
            bridge->node_retirement(bridge->node_retirement_opaque, content);
        document_parser_insertions_discard_subtree(bridge->document, content);
        bridge_prepare_owned_tree_destroy(bridge, content, ownership_depth + 1u);
        for (size_t count = 0; content->first_child != NULL
             && count < DOM_TRAVERSAL_VISIT_LIMIT; count++)
            lxb_dom_node_destroy_deep(content->first_child);
    }
}
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
    images_retire_subtree(bridge->images, root);
    /* Every handle in the subtree, template contents included, is retired
       above. */
    js_rt_image_preload_forget_released(bridge);
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
    bridge_prepare_owned_tree_destroy(bridge, root, 0);
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
    /* Its tree's rooted event state may now move to its wrappers. */
    bridge_arm_wrapper_sweep(bridge);
    lxb_dom_node_t *root = bridge_owned_lifetime_root(node);
    (void) bridge_discard_unretained_detached_subtree(bridge, root);
}

/* Constructed stylesheets (document.h, document_adopted_sheets_active).
   A sheet's text lives in a CSSOM override keyed by a detached <style> held by its
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

static JSValue registered_property_result(JSContext *context, int status)
{
    if (status == 0) return JS_UNDEFINED;
    if (status < 0) return JS_ThrowOutOfMemory(context);
    JSValue global = JS_GetGlobalObject(context);
    JSValue constructor = JS_GetPropertyStr(context, global, "DOMException");
    JS_FreeValue(context, global);
    if (JS_IsException(constructor)) return constructor;
    const char *name = status == 1 ? "InvalidModificationError"
        : status == 2 ? "SyntaxError" : "QuotaExceededError";
    JSValue arguments[2] = {
        JS_NewString(context, "Property registration failed"),
        JS_NewString(context, name)
    };
    JSValue error = JS_IsException(arguments[0]) || JS_IsException(arguments[1])
        ? JS_EXCEPTION : JS_CallConstructor(context, constructor, 2, arguments);
    JS_FreeValue(context, arguments[0]);
    JS_FreeValue(context, arguments[1]);
    JS_FreeValue(context, constructor);
    return JS_IsException(error) ? error : JS_Throw(context, error);
}

/* Allocate the document record before changing the live cascade:
   an allocation refusal must not leave a ghost duplicate registration. */
JSValue js_css_register_property(JSContext *context, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->document == NULL)
        return registered_property_result(context, 3);
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(context, "Property definition required");
    /* Read Web IDL dictionary members once in lexicographic order. */
    JSValue member = JS_GetPropertyStr(context, argv[0], "inherits");
    if (JS_IsException(member)) return member;
    if (JS_IsUndefined(member))
        return JS_ThrowTypeError(context, "inherits required");
    bool inherits = JS_ToBool(context, member) != 0;
    JS_FreeValue(context, member);
    size_t name_length = 0, initial_length = 0, syntax_length = 1;
    const char *name = NULL, *initial = NULL, *syntax = NULL;
    member = JS_GetPropertyStr(context, argv[0], "initialValue");
    if (JS_IsException(member)) goto exception;
    if (!JS_IsUndefined(member)) {
        initial = JS_ToCStringLen(context, &initial_length, member);
        JS_FreeValue(context, member);
        if (initial == NULL) goto exception;
    }
    member = JS_GetPropertyStr(context, argv[0], "name");
    if (JS_IsException(member)) goto exception;
    if (JS_IsUndefined(member)) {
        JS_ThrowTypeError(context, "name required");
        goto exception;
    }
    name = JS_ToCStringLen(context, &name_length, member);
    JS_FreeValue(context, member);
    if (name == NULL) goto exception;
    member = JS_GetPropertyStr(context, argv[0], "syntax");
    if (JS_IsException(member)) goto exception;
    if (!JS_IsUndefined(member)) {
        syntax = JS_ToCStringLen(context, &syntax_length, member);
        JS_FreeValue(context, member);
        if (syntax == NULL) goto exception;
    }
    int status = 0;
    PocDocument *document = bridge->document;
    /* The dictionary's syntax defaults to "*". */
    const char *syntax_text = syntax == NULL ? "*" : syntax;
    if (strlen(syntax_text) != syntax_length
        || strlen(name) != name_length
        || (initial != NULL && (initial_length >= STYLE_CUSTOM_VALUE_CAPACITY
                                || strlen(initial) != initial_length)))
        status = 2;
    StyleRegistrationSyntax kind = status != 0 ? STYLE_REGISTRATION_INVALID
        : style_registered_property_check(name, name_length, syntax_text,
                                          syntax_length, initial,
                                          initial_length);
    if (kind == STYLE_REGISTRATION_INVALID) status = 2;
    bool length_syntax = kind == STYLE_REGISTRATION_LENGTH;
    for (DocumentRegisteredProperty *p = document->registered_properties;
         status == 0 && p != NULL; p = p->next)
        if (strcmp(name, p->name) == 0) status = 1;
    if (status == 0 && document->registered_property_count
                       >= STYLE_REGISTERED_PROPERTY_LIMIT) status = 3;
    if (status == 0) {
        BudgetAllocationOwner owner = document_allocation_owner_enter(document);
        DocumentRegisteredProperty *p = budget_malloc(document->budget,
            sizeof(*p) + name_length + 1u
            + (initial == NULL ? 0 : initial_length + 1u));
        document_allocation_owner_leave(document, owner);
        if (p == NULL) status = -1;
        else {
            char *storage = (char *) (p + 1);
            memcpy(storage, name, name_length + 1u);
            *p = (DocumentRegisteredProperty) {
                .name = storage,
                .initial_value = initial == NULL ? NULL : storage + name_length + 1u,
                .inherits = inherits,
                .length_syntax = length_syntax
            };
            if (initial != NULL)
                memcpy(storage + name_length + 1u, initial, initial_length + 1u);
            if (bridge->stylesheet != NULL
                && !stylesheet_register_api_property(
                    (Stylesheet *) bridge->stylesheet, p)) {
                budget_free(document->budget, p);
                status = -1;
            } else {
                p->next = document->registered_properties;
                document->registered_properties = p;
                document->registered_property_count++;
                if (bridge->stylesheet != NULL)
                    ((Stylesheet *) bridge->stylesheet)->api_registrations = p;
                bridge_note_adopted_styles_changed(bridge, NULL);
            }
        }
    }
    JS_FreeCString(context, initial);
    JS_FreeCString(context, name);
    JS_FreeCString(context, syntax);
    return registered_property_result(context, status);
exception:
    JS_FreeCString(context, initial);
    JS_FreeCString(context, name);
    JS_FreeCString(context, syntax);
    return JS_EXCEPTION;
}

/* CSSOM native text is independent of the owner's DOM children. */
JSValue js_dom_constructed_sheet_text(JSContext *context,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL || bridge->document == NULL
        || bridge->document->html == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_STYLE
        || node->ns != LXB_NS_HTML)
        return JS_FALSE;
    if (argc == 1) {
        size_t length = 0;
        const char *text = document_cssom_sheet_text(node, &length);
        return text == NULL ? JS_NULL : JS_NewStringLen(context, text, length);
    }
    bool constructed = argc < 3 || JS_ToBool(context, argv[2]);
    if (constructed && (node->parent != NULL || node->local_name != LXB_TAG_STYLE))
        return JS_FALSE;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[1]);
    if (text == NULL) return JS_EXCEPTION;
    bool active = false;
    bool ok = document_cssom_sheet_set_text(
        bridge->document, node, text, length, constructed, &active);
    JS_FreeCString(context, text);
    if (ok && active)
        bridge_note_adopted_styles_changed(bridge, constructed ? NULL : node);
    return JS_NewBool(context, ok);
}

JSValue js_dom_sheet_revision(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    return JS_NewUint32(context, document_cssom_sheet_revision(node));
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
        if (slot != SIZE_MAX) bridge_pin_node_slot(bridge, slot);
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
    /* A removed subtree a wrapper keeps may hold rooted event state that
       must move to its wrappers, as for bridge_note_removal. */
    if (parent->first_child != NULL) bridge_arm_wrapper_sweep(bridge);
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
    if (traversal == NULL || traversal->next == NULL) return NULL;
    size_t limit = traversal->visit_limit == 0
        ? DOM_TRAVERSAL_VISIT_LIMIT : traversal->visit_limit;
    if (traversal->visited >= limit) {
        traversal->exhausted = true;
        return NULL;
    }
    lxb_dom_node_t *node = traversal->next;
    traversal->visited++;
    traversal->current_depth = traversal->next_depth;
    if (node->first_child != NULL) {
        traversal->next = node->first_child;
        traversal->next_depth = traversal->current_depth + 1u;
        return node;
    }
    lxb_dom_node_t *at = node;
    size_t climbed = 0;
    while (at != NULL && at != traversal->boundary
           && at->next == NULL
           && climbed++ < limit) {
        at = at->parent;
    }
    if (climbed >= limit) traversal->exhausted = true;
    traversal->next = at == NULL || at == traversal->boundary
        || climbed >= limit ? NULL : at->next;
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
        bridge_reclaim_node_slots(bridge, source, false);
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
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = boundary
    };
    size_t shadow_depth = SIZE_MAX;
    BridgeQuerySelector query;
    bridge_query_selector_prepare(&query, selector, length);
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL; at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, boundary, &traversal, &shadow_depth)) continue;
        if (bridge_query_selector_matches(&query, at, boundary)) return at;
    }
    return NULL;
}

#include "js_dom_mutations.inc"

JSValue js_dom_body(JSContext *context, JSValueConst this_value,
                    int argc, JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *body = document_body_node(bridge->document);
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
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = boundary
    };
    size_t shadow_depth = SIZE_MAX;
    BridgeQuerySelector query;
    bridge_query_selector_prepare(&query, selector, length);
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < result_limit;
         at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, boundary, &traversal, &shadow_depth)) continue;
        if (bridge_query_selector_matches(&query, at, boundary)) {
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
                || bridge_node_is_shadow_root(bridge, parent)) break;
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
    JSValueConst wrap = bridge == NULL ? JS_UNDEFINED
        : js_rt_bridge_trusted_hook(bridge, SCRIPT_HOST_WRAP_NODE);
    if (!JS_IsFunction(context, wrap)) {
        return JS_ThrowInternalError(
            context, "trusted DOM wrapper is unavailable");
    }
    return JS_Call(context, wrap, JS_UNDEFINED, 1, &handle);
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
    walk->traversal = (DomDocumentOrderTraversal) {
        .next = root, .boundary = walk->boundary
    };
    return true;
}

lxb_dom_node_t *js_rt_element_walk_next(DomElementWalk *walk)
{
    for (lxb_dom_node_t *at = dom_document_order_next(&walk->traversal);
         at != NULL; at = dom_document_order_next(&walk->traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                walk->bridge, at, walk->boundary, &walk->traversal,
                &walk->shadow_depth)) continue;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) return at;
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
    JS_FreeCString(context, identifier);
    if (found == NULL) return JS_NULL;
    JSValue handle = bridge_node_handle_value(context, bridge, found);
    if (JS_IsException(handle)) return handle;
    JSValue wrapped = js_rt_wrap_dom_handle(context, handle);
    JS_FreeValue(context, handle);
    return wrapped;
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
        JSValue identifier = simple_id_selector_value(context, selector);
        if (JS_IsException(identifier)) {
            JS_FreeValue(context, found);
            return identifier;
        }
        if (found_handle == 0 && !JS_IsUndefined(identifier)) {
            JS_FreeValue(context, found);
            JSValue by_id = js_dom_get_element_by_id_method(
                context, this_value, 1, &identifier);
            JS_FreeValue(context, identifier);
            return by_id;
        }
        JS_FreeValue(context, identifier);
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
    if (scope == 0 && length == 0) {
        JSValue identifier = simple_id_selector_value(context, selector);
        if (JS_IsException(identifier)) {
            JS_FreeValue(context, values);
            return identifier;
        }
        if (!JS_IsUndefined(identifier)) {
            JSValue by_id = js_dom_get_element_by_id_method(
                context, this_value, 1, &identifier);
            if (JS_IsException(by_id)
                || (!JS_IsNull(by_id)
                    && JS_SetPropertyUint32(context, values, 0, by_id) < 0)) {
                if (JS_IsNull(by_id)) JS_FreeValue(context, by_id);
                JS_FreeValue(context, identifier);
                JS_FreeValue(context, values);
                return JS_EXCEPTION;
            }
            if (JS_IsNull(by_id)) JS_FreeValue(context, by_id);
        }
        JS_FreeValue(context, identifier);
    }
    for (uint32_t index = 0; index < length; index++) {
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
   handle is stale. */
JSValue js_dom_is_ancestor(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handles[2];
    size_t slots[2];
    if (argc < 2 || bridge == NULL) return JS_UNDEFINED;
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
    if (argc < 3 || bridge == NULL || !JS_IsArray(context, argv[1]))
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
   to script: shadow trees and
   ancestries past the script walks' 256-step bound
   (DOM_ROOT_NODE_ANCESTOR_LIMIT). */
JSValue js_dom_root_node(JSContext *context, JSValueConst this_value,
                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handles[2] = {0, 0};
    if (bridge == NULL || argc < 1 || bridge->shadow_root_count > 0)
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

/* __tilefinchNodeIdentity(handle[, array[, held]]): a new wrapper's
   identity in one call, [tagName, nodeType, namespaceURI]: the first two
   as __tilefinchTagName (|| "") and __tilefinchNodeType report them, the
   third an element's namespace URI, else null. When `held` (the bootstrap
   holds some detached tree's event state), a fourth entry: the handle
   whose holder the wrapper joins, or 0 (bridge_wrapper_event_holder), so
   a wrapper needs no second call. */
JSValue js_dom_node_identity(JSContext *context,
                             JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t handle = 0;
    size_t slot = 0;
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        && JS_ToInt64(context, &handle, argv[0]) >= 0 && handle > 0
        && js_rt_bridge_node_slot_for_handle(bridge, handle, &slot)
        ? bridge->nodes[slot] : NULL;
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
    if (JS_SetPropertyUint32(context, identity, 2, namespace_uri) < 0) {
        JS_FreeValue(context, identity);
        return JS_EXCEPTION;
    }
    if (argc > 2 && JS_ToBool(context, argv[2]) > 0
        && JS_SetPropertyUint32(
               context, identity, 3,
               JS_NewInt64(context, node == NULL ? 0
                           : bridge_wrapper_event_holder(bridge, slot))) < 0) {
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
 * realm with no script-side detached parent -- and
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
    if (bridge == NULL || !JS_IsObject(receiver)) return 0;
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
        if (ready > 0) {
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
            if (related == NULL) return JS_NULL;
            if (!bridge_node_is_shadow_root(bridge, related)) {
                JSValue wrapper = fast_getter_wrapper(context, bridge,
                                                      related);
                if (!JS_IsNull(wrapper) && !JS_IsUndefined(wrapper))
                    return wrapper;
            }
            return JS_Call(context, data[0], receiver, 0, NULL);
        }
        lxb_dom_node_t *parent = node->parent;
        bool visible = parent != NULL;
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
    if (bridge == NULL || bridge->document == NULL
        || bridge->document->html == NULL
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
    DomDocumentOrderTraversal traversal = {
        .next = node, .boundary = node
    };
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && *count < DOM_QUERY_RESULT_LIMIT;
         at = dom_document_order_next(&traversal)) {
        bool include = (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                        && (what_to_show & 1) != 0)
                       || (at->type == LXB_DOM_NODE_TYPE_TEXT
                           && (what_to_show & 4) != 0)
                       || (at->type == LXB_DOM_NODE_TYPE_COMMENT
                           && (what_to_show & 128) != 0);
        if (include) {
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
    DomDocumentOrderTraversal traversal = {
        .next = root, .boundary = root
    };
    uint32_t count = 0;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && count < 128;
         at = dom_document_order_next(&traversal)) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
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
    if (root == NULL) return array;
    DomDocumentOrderTraversal traversal = {
        .next = root, .boundary = root
    };
    uint32_t count = 0;
    size_t shadow_depth = SIZE_MAX;
    for (lxb_dom_node_t *at = dom_document_order_next(&traversal);
         at != NULL && count < DOM_QUERY_RESULT_LIMIT;
         at = dom_document_order_next(&traversal)) {
        if (bridge_query_node_hidden_by_shadow(
                bridge, at, root, &traversal, &shadow_depth)
            || at->type != LXB_DOM_NODE_TYPE_ELEMENT || at->ns != LXB_NS_HTML
            || (at->local_name != LXB_TAG_IFRAME
                && at->local_name != LXB_TAG_FRAME))
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

/* Text reads walk the whole subtree under their own, larger quota and
   throw rather than publish a prefix when they reach it. */
static DomDocumentOrderTraversal bridge_text_content_walk(lxb_dom_node_t *root)
{
    return (DomDocumentOrderTraversal) {
        .next = root, .boundary = root,
        .visit_limit = DOM_TEXT_CONTENT_NODE_LIMIT,
    };
}

static JSValue bridge_text_string(JSContext *context, const char *text, size_t length)
{
#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    return JS_NewStringLenUTF8RopePortable(context, text, length);
#else
    return JS_NewStringLen(context, text, length);
#endif
}

JSValue js_dom_get_text(JSContext *context, JSValueConst this_value,
                        int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0
                           ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL) return JS_NULL;
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
        return bridge_text_string(context,
            data->data == NULL ? "" : (const char *) data->data, data->length);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        && node->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)
    {
        if (node->type == LXB_DOM_NODE_TYPE_ATTRIBUTE) {
            size_t length = 0;
            const lxb_char_t *text = lxb_dom_attr_value(lxb_dom_interface_attr(node), &length);
            return bridge_text_string(context, text == NULL ? "" : (const char *) text, length);
        }
        return JS_NewString(context, "");
    }

    /* Complete results or an explicit quota error, never a silent prefix.
       These bounds cover the admitted document and the existing rendered-text
       traversal ceiling while keeping hostile script-built trees bounded. */
    size_t length = 0;
    DomDocumentOrderTraversal walk = bridge_text_content_walk(node);
    for (lxb_dom_node_t *at = dom_document_order_next(&walk); at != NULL;
         at = dom_document_order_next(&walk)) {
        if (at->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        size_t bytes = lxb_dom_interface_text(at)->char_data.data.length;
        if (bytes > DOM_TEXT_CONTENT_BYTE_LIMIT - length)
            return JS_ThrowRangeError(context, "textContent byte quota exceeded");
        length += bytes;
    }
    if (walk.exhausted)
        return JS_ThrowRangeError(context, "textContent node quota exceeded");
    char local[512];
    char *text = length <= sizeof(local) ? local
        : budget_malloc(bridge->budget, length);
    if (text == NULL) return JS_ThrowOutOfMemory(context);
    size_t used = 0;
    walk = bridge_text_content_walk(node);
    for (lxb_dom_node_t *at = dom_document_order_next(&walk); at != NULL;
         at = dom_document_order_next(&walk)) {
        if (at->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        const lexbor_str_t *data = &lxb_dom_interface_text(at)->char_data.data;
        if (data->length != 0) memcpy(text + used, data->data, data->length);
        used += data->length;
    }
    JSValue value = bridge_text_string(context, text, length);
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
            /* As bridge_detach_and_discard_children: the sweep hands the
               event state of a retained removed subtree to its wrappers. */
            if (node->first_child != NULL) bridge_arm_wrapper_sweep(bridge);
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
    if (!image_resource_available(image)) image = js_rt_image_preload_find(bridge, node);
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
                          source == NULL || length == 0 || available
                          || js_rt_image_preload_status(bridge, node) == 2
                          || js_rt_image_preload_broken(bridge, node));
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
    bool preload_ok = true;
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
        if (node->local_name == LXB_TAG_IMG && (source_attribute
            || (name_length == 6 && strncasecmp(name, "srcset", 6) == 0)
            || (name_length == 5 && strncasecmp(name, "sizes", 5) == 0)
            || (name_length == 11 && strncasecmp(name, "crossorigin", 11) == 0)))
            preload_ok = js_rt_image_preload_changed(bridge, node);
    }
    JS_FreeCString(context, value);
    JS_FreeCString(context, name);
    if (timed) {
        uint64_t finished_ns = js_rt_monotonic_time_ns();
        if (finished_ns > write_started_ns)
            bridge->attribute_write_ns += finished_ns - write_started_ns;
    }
    return preload_ok ? JS_NewBool(context, attribute != NULL) : JS_EXCEPTION;
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
    bool preload_ok = true;
    if (existed && status == LXB_STATUS_OK) {
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_ATTRIBUTE, node,
            name, name_length, relational_selector_sensitive,
            changed_tokens_exact ? changed_tokens : NULL,
            changed_token_count);
        if (node->local_name == LXB_TAG_IMG
            && (bridge_mutation_name_equal(name, name_length, "src")
                || bridge_mutation_name_equal(name, name_length, "srcset")
                || bridge_mutation_name_equal(name, name_length, "sizes")
                || bridge_mutation_name_equal(name, name_length, "crossorigin")))
            preload_ok = js_rt_image_preload_changed(bridge, node);
    }
    JS_FreeCString(context, name);
    return preload_ok ? JS_NewBool(context, status == LXB_STATUS_OK) : JS_EXCEPTION;
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
        && js_rt_property_name_equal(style + declaration->name_start,
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
    /* A CSSOM assignment of the existing declaration is not a DOM write.
       Require existing CSSOM provenance too: an equal string can otherwise
       newly authorize an inline style blocked by the document policy. */
    if (old != NULL && old_length == used && memcmp(old, updated, used) == 0
        && document_style_attribute_cssom_authorized(node)) {
        JS_FreeCString(context, value);
        JS_FreeCString(context, wanted);
        return JS_TRUE;
    }
    /* CSSOM changes the style attribute, not a selector-visible attribute
       named after the property. Preserve :has([style]) invalidation while
       allowing unrelated :has() rules to keep ancestor/sibling caches. The
       old attribute storage is valid only before the DOM setter below. */
    bool relational = stylesheet_attribute_change_may_affect_has(
        bridge == NULL ? NULL : bridge->stylesheet, "style", 5,
        old, old_length, updated, used);
    bool preserve_geometry = false;
    if (wanted_length == 7 && strncasecmp(wanted, "opacity", 7) == 0
        && document_style_attribute_cssom_authorized(node)) {
        char *end = NULL;
        float number = value_length == 0 ? 0.0f : strtof(value, &end);
        if (end != NULL && *end == '%') end++;
        while (end != NULL && isspace((unsigned char) *end)) end++;
        /* A numeric value cannot splice another declaration into this
           serializer. Other CSS syntax keeps the conservative path. */
        if (value_length == 0 || (end != value && isfinite(number)
            && end == value + value_length))
            preserve_geometry = js_rt_geometry_opacity_write_begin(bridge, node);
    }
    lxb_status_t status;
    document_style_quiet_begin();
    status = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) "style", 5,
        (const lxb_char_t *) updated, used) == NULL
        ? LXB_STATUS_ERROR : LXB_STATUS_OK;
    document_style_quiet_end();
    if (status == LXB_STATUS_OK) {
        /* Sparse properties written here reach the sheet's presence
           summary when style resolution reads the attribute
           (style_sparse_note_inline). */
        document_style_attribute_set_cssom_authorized(node, true);
        bridge_mutated_with_relational(
            bridge, SCRIPT_MUTATION_INLINE_STYLE, node, wanted, wanted_length,
            relational, NULL, 0);
        if (preserve_geometry) js_rt_geometry_opacity_write_end(bridge);
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
    /* The removed tree's rooted event state may move to its wrappers. */
    bridge_arm_wrapper_sweep(bridge);
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
    /* Lexbor's inserted callback visits every descendant of a moved tree.
       Reset only the actual insertion parent, not a style's unchanged Text
       descendants when its owner disconnects/reconnects. */
    if (node != NULL) document_cssom_sheet_reset(node->parent);
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
    bridge_event_state_joining(bridge, parent, child);
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
        bridge_event_state_joining(bridge, parent, nodes[index]);
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
    bridge_event_state_joining(bridge, parent, node);
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
