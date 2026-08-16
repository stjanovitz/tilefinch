/* Remote-node proxy tier: selector/descendant collection results, remote
   read/write/relation/geometry/attribute bindings, and the remote-element
   request queue used when another section owns the authoritative DOM.
   Split from js_runtime.c; shares internals through js_runtime_internal.h. */
#include "js_runtime_internal.h"

#include "tilefinch/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

JSValue js_remote_node_writer_active(JSContext *context,
                                     JSValueConst this_value,
                                     int argc,
                                     JSValueConst *argv)
{
    (void) this_value; (void) argc; (void) argv;
    DomBridge *bridge = JS_GetContextOpaque(context);
    return JS_NewBool(context, bridge != NULL
                               && bridge->remote_node_write != NULL);
}

void js_rt_bridge_queue_remote_element(DomBridge *bridge,
                                 const char *identifier,
                                 size_t length, size_t section)
{
    if (bridge == NULL || identifier == NULL || length == 0 || length > 128)
        return;
    for (size_t i = 0; i < bridge->remote_element_count; i++) {
        size_t at = (bridge->remote_element_head + i) % 8;
        /* One materialization satisfies every deferred lookup in the same
           section; retaining selector-specific duplicates causes needless
           reverse swaps and can leave the controller on the wrong section. */
        if (bridge->remote_element_sections[at] == section) return;
    }
    if (bridge->remote_element_count == 8) return;
    size_t at = (bridge->remote_element_head
                 + bridge->remote_element_count) % 8;
    memcpy(bridge->remote_element_identifiers[at], identifier, length);
    bridge->remote_element_identifiers[at][length] = '\0';
    bridge->remote_element_sections[at] = section;
    bridge->remote_element_count++;
}

JSValue js_remote_lookup_suppress(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge != NULL) {
        bridge->remote_lookup_suppressed = argc > 0
            && JS_ToBool(context, argv[0]) > 0;
    }
    return JS_UNDEFINED;
}

JSValue js_remote_selector_result(JSContext *context,
                                  JSValueConst selector,
                                  bool earlier_only)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_lookup_suppressed
        || bridge->remote_selector_lookup == NULL) return JS_NULL;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, selector);
    if (text == NULL) return JS_EXCEPTION;
    size_t section = 0;
    char tag_name[32] = "div";
    char identifier[129] = {0};
    char stable_key[96] = {0};
    bool remote = length != 0 && length <= 512
        && bridge->remote_selector_lookup(
               bridge->remote_selector_opaque, text, length,
               &section, tag_name, identifier, stable_key)
        && section != bridge->section_identity
        && (!earlier_only || section < bridge->section_identity);
    if (!remote) {
        JS_FreeCString(context, text);
        return JS_NULL;
    }
    const char *queue_key = stable_key[0] == '\0' ? text : stable_key;
    size_t queue_length = stable_key[0] == '\0'
                          ? length : strlen(stable_key);
    if (bridge->remote_node_read == NULL) {
        js_rt_bridge_queue_remote_element(bridge, queue_key, queue_length, section);
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue wrapper = JS_IsException(global) ? JS_EXCEPTION
        : JS_GetPropertyStr(
              context, global, stable_key[0] != '\0'
                ? "__tilefinchWrapRemoteStable"
                : (identifier[0] != '\0' ? "__tilefinchWrapRemote"
                                          : "__tilefinchWrapRemoteSelector"));
    if (JS_IsException(global) || JS_IsException(wrapper)) {
        JS_FreeCString(context, text);
        JS_FreeValue(context, wrapper);
        JS_FreeValue(context, global);
        return JS_EXCEPTION;
    }
    JSValue arguments[3] = {
        stable_key[0] != '\0' ? JS_NewString(context, stable_key)
            : (identifier[0] != '\0' ? JS_NewString(context, identifier)
                                      : JS_NewStringLen(context, text, length)),
        JS_NewString(context, tag_name),
        JS_NewInt64(context, (int64_t) section)
    };
    JS_FreeCString(context, text);
    JSValue result = JS_Call(context, wrapper, global, 3, arguments);
    for (size_t i = 0; i < 3; i++) JS_FreeValue(context, arguments[i]);
    JS_FreeValue(context, wrapper);
    JS_FreeValue(context, global);
    return result;
}

JSValue js_remote_selector_collection(JSContext *context,
                                      JSValueConst selector,
                                      JSValueConst local_values,
                                      uint32_t local_length,
                                      uint32_t *result_length)
{
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_lookup_suppressed
        || bridge->remote_selector_collect == NULL || bridge->host == NULL)
        return JS_NULL;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, selector);
    if (text == NULL) return JS_EXCEPTION;
    TilefinchRemoteSelectorMatch *matches = budget_calloc(
        bridge->budget, TILEFINCH_REMOTE_SELECTOR_MATCH_LIMIT,
        sizeof(*matches));
    size_t count = 0;
    bool complete = matches != NULL && length != 0 && length <= 512
        && bridge->remote_selector_collect(
               bridge->remote_selector_collect_opaque, text, length, matches,
               TILEFINCH_REMOTE_SELECTOR_MATCH_LIMIT, &count)
        && count != 0 && count <= TILEFINCH_REMOTE_SELECTOR_MATCH_LIMIT;
    size_t remote_sections[8] = {0};
    size_t remote_count = 0;
    size_t current_count = 0;
    for (size_t i = 0; complete && i < count; i++) {
        if (matches[i].section_index == bridge->section_identity) {
            current_count++;
            continue;
        }
        if (bridge->remote_node_read == NULL) {
            bool known = false;
            for (size_t j = 0; j < remote_count; j++) {
                if (remote_sections[j] == matches[i].section_index) {
                    known = true;
                }
            }
            if (!known) {
                if (remote_count == 8) { complete = false; break; }
                remote_sections[remote_count++] = matches[i].section_index;
            }
        }
        if (matches[i].identifier[0] == '\0') continue;
        for (size_t j = 0; j < i; j++) {
            if (matches[j].identifier[0] != '\0'
                && strcmp(matches[j].identifier,
                          matches[i].identifier) == 0) {
                complete = false;
                break;
            }
        }
    }
    /* Prefix/context elements and live DOM mutations appear in the local
       result but not the selected source interval.  Only merge when the
       current interval maps one-for-one, so the local wrappers remain exact. */
    if (complete && current_count != local_length) complete = false;
    JSValue values = complete ? JS_NewArray(context) : JS_NULL;
    JSValue global = complete ? JS_GetGlobalObject(context) : JS_UNDEFINED;
    bool exception = JS_IsException(values) || JS_IsException(global);
    if (exception) complete = false;
    size_t local_index = 0;
    for (size_t i = 0; complete && i < count; i++) {
        TilefinchRemoteSelectorMatch *match = &matches[i];
        if (match->section_index == bridge->section_identity) {
            JSValue handle = JS_GetPropertyUint32(
                context, local_values, (uint32_t) local_index++);
            JSValue value = JS_IsException(handle) ? JS_EXCEPTION
                : js_rt_wrap_dom_handle(context, handle);
            JS_FreeValue(context, handle);
            if (JS_IsException(value)
                || JS_SetPropertyUint32(context, values, (uint32_t) i,
                                        value) < 0) {
                exception = true;
                complete = false;
            }
            continue;
        }
        const char *queue_key = match->stable_key[0] == '\0'
                                ? text : match->stable_key;
        size_t queue_length = match->stable_key[0] == '\0'
                              ? length : strlen(match->stable_key);
        if (bridge->remote_node_read == NULL) {
            js_rt_bridge_queue_remote_element(bridge, queue_key, queue_length,
                                        match->section_index);
        }
        const char *function_name = match->stable_key[0] != '\0'
                                    ? "__tilefinchWrapRemoteStable"
                                    : (match->identifier[0] != '\0'
                                       ? "__tilefinchWrapRemote"
                                       : "__tilefinchWrapRemoteSelector");
        JSValue wrapper = JS_GetPropertyStr(context, global, function_name);
        JSValue arguments[3] = {
            match->stable_key[0] != '\0'
                ? JS_NewString(context, match->stable_key)
                : (match->identifier[0] != '\0'
                   ? JS_NewString(context, match->identifier)
                   : JS_NewStringLen(context, text, length)),
            JS_NewString(context, match->tag_name),
            JS_NewInt64(context, (int64_t) match->section_index)
        };
        JSValue value = JS_IsException(wrapper) ? JS_EXCEPTION
            : JS_Call(context, wrapper, global, 3, arguments);
        for (size_t j = 0; j < 3; j++) JS_FreeValue(context, arguments[j]);
        JS_FreeValue(context, wrapper);
        if (JS_IsException(value)
            || JS_SetPropertyUint32(context, values, (uint32_t) i, value) < 0) {
            exception = true;
            complete = false;
            break;
        }
    }
    JS_FreeValue(context, global);
    budget_free(bridge->budget, matches);
    JS_FreeCString(context, text);
    if (!complete) {
        JS_FreeValue(context, values);
        return exception ? JS_EXCEPTION : JS_NULL;
    }
    *result_length = (uint32_t) count;
    return values;
}

static JSValue js_remote_node_read(JSContext *context,
                                   JSValueConst this_value,
                                   int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_node_read == NULL || argc < 3)
        return JS_ThrowInternalError(context,
                                     "remote node reader unavailable");
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int64_t section_value = -1, kind_value = -1;
    if (JS_ToInt64(context, &section_value, argv[1]) < 0
        || JS_ToInt64(context, &kind_value, argv[2]) < 0
        || section_value < 0 || (uint64_t) section_value > SIZE_MAX
        || kind_value < SCRIPT_REMOTE_NODE_TEXT
        || (kind_value != SCRIPT_REMOTE_NODE_TEXT
            && kind_value != SCRIPT_REMOTE_NODE_INNER_HTML
            && kind_value != SCRIPT_REMOTE_NODE_ATTRIBUTE
            && kind_value != SCRIPT_REMOTE_NODE_MATCHES)
        || key_length == 0 || key_length > 192) {
        JS_FreeCString(context, key);
        return JS_ThrowTypeError(context, "invalid remote node read");
    }
    const char *name = NULL;
    size_t name_length = 0;
    if (kind_value == SCRIPT_REMOTE_NODE_ATTRIBUTE
        || kind_value == SCRIPT_REMOTE_NODE_MATCHES) {
        if (argc < 4) {
            JS_FreeCString(context, key);
            return JS_ThrowTypeError(context,
                                     "remote attribute name required");
        }
        name = JS_ToCStringLen(context, &name_length, argv[3]);
        if (name == NULL) {
            JS_FreeCString(context, key);
            return JS_EXCEPTION;
        }
        size_t maximum_name = kind_value == SCRIPT_REMOTE_NODE_MATCHES
                              ? 512 : 128;
        if (name_length == 0 || name_length > maximum_name) {
            JS_FreeCString(context, name);
            JS_FreeCString(context, key);
            return JS_ThrowTypeError(context,
                                     "invalid remote attribute name");
        }
    }
    ScriptRemoteNodeReadResult read = {0};
    bool success = bridge->remote_node_read(
        bridge->remote_node_read_opaque, key, key_length,
        (size_t) section_value, (ScriptRemoteNodeReadKind) kind_value,
        name, name_length, &read);
    if (name != NULL) JS_FreeCString(context, name);
    JS_FreeCString(context, key);
    if (!success || (read.value == NULL && read.length != 0)) {
        budget_free(bridge->budget, read.value);
        return JS_ThrowInternalError(context, "remote node read failed");
    }
    JSValue value = read.is_null ? JS_NULL
        : JS_NewStringLen(context, read.value == NULL ? "" : read.value,
                          read.length);
    budget_free(bridge->budget, read.value);
    return value;
}

static JSValue js_remote_node_write(JSContext *context,
                                    JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_node_write == NULL || argc < 4)
        return JS_ThrowInternalError(context,
                                     "remote node writer unavailable");
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int64_t section_value = -1, kind_value = -1;
    if (JS_ToInt64(context, &section_value, argv[1]) < 0
        || JS_ToInt64(context, &kind_value, argv[2]) < 0
        || section_value < 0 || (uint64_t) section_value > SIZE_MAX
        || kind_value < SCRIPT_REMOTE_NODE_WRITE_TEXT
        || kind_value > SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE
        || key_length == 0 || key_length > 192) {
        JS_FreeCString(context, key);
        return JS_ThrowTypeError(context, "invalid remote node write");
    }
    ScriptRemoteNodeWriteKind kind = (ScriptRemoteNodeWriteKind) kind_value;
    const char *name = NULL, *value = NULL;
    size_t name_length = 0, value_length = 0;
    if (kind == SCRIPT_REMOTE_NODE_WRITE_SET_ATTRIBUTE
        || kind == SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE) {
        name = JS_ToCStringLen(context, &name_length, argv[3]);
        if (name == NULL) {
            JS_FreeCString(context, key);
            return JS_EXCEPTION;
        }
        if (name_length == 0 || name_length > 128) {
            JS_FreeCString(context, name);
            JS_FreeCString(context, key);
            return JS_ThrowTypeError(context,
                                     "invalid remote attribute name");
        }
        if (kind == SCRIPT_REMOTE_NODE_WRITE_SET_ATTRIBUTE) {
            if (argc < 5) {
                JS_FreeCString(context, name);
                JS_FreeCString(context, key);
                return JS_ThrowTypeError(context,
                                         "remote attribute value required");
            }
            value = JS_ToCStringLen(context, &value_length, argv[4]);
        }
    } else {
        value = JS_ToCStringLen(context, &value_length, argv[3]);
    }
    if ((kind != SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE && value == NULL)
        || value_length > DOM_INNER_HTML_LIMIT) {
        if (value != NULL) JS_FreeCString(context, value);
        if (name != NULL) JS_FreeCString(context, name);
        JS_FreeCString(context, key);
        return value == NULL ? JS_EXCEPTION
            : JS_ThrowRangeError(context, "remote node write too large");
    }
    bool success = bridge->remote_node_write(
        bridge->remote_node_write_opaque, key, key_length,
        (size_t) section_value, kind, name, name_length,
        value, value_length);
    if (value != NULL) JS_FreeCString(context, value);
    if (name != NULL) JS_FreeCString(context, name);
    JS_FreeCString(context, key);
    if (!success)
        return JS_ThrowInternalError(context, "remote node write failed");
    return JS_TRUE;
}

void js_rt_remote_node_read_result_destroy(
DomBridge *bridge, ScriptRemoteNodeReadResult *read)
{
    if (bridge == NULL || read == NULL) return;
    budget_free(bridge->budget, read->value);
    if (read->attributes != NULL) {
        for (size_t i = 0; i < read->attribute_count; i++) {
            budget_free(bridge->budget, read->attributes[i].name);
            budget_free(bridge->budget, read->attributes[i].value);
        }
    }
    budget_free(bridge->budget, read->attributes);
    memset(read, 0, sizeof(*read));
}

static JSValue js_remote_node_relation(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_node_read == NULL || argc < 3)
        return JS_ThrowInternalError(context,
                                     "remote node reader unavailable");
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int64_t section_value = -1, relation = -1;
    if (JS_ToInt64(context, &section_value, argv[1]) < 0
        || JS_ToInt64(context, &relation, argv[2]) < 0
        || section_value < 0 || (uint64_t) section_value > SIZE_MAX
        || relation < 0 || relation > 7
        || key_length == 0 || key_length > 192) {
        JS_FreeCString(context, key);
        return JS_ThrowTypeError(context, "invalid remote node relation");
    }
    ScriptRemoteNodeReadKind kind = relation <= 3
        ? (ScriptRemoteNodeReadKind)
            (SCRIPT_REMOTE_NODE_PARENT_ELEMENT + relation)
        : (ScriptRemoteNodeReadKind)
            (SCRIPT_REMOTE_NODE_FIRST_CHILD + relation - 4);
    ScriptRemoteNodeReadResult read = {0};
    bool success = bridge->remote_node_read(
        bridge->remote_node_read_opaque, key, key_length,
        (size_t) section_value, kind, NULL, 0, &read);
    JS_FreeCString(context, key);
    if (!success) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return JS_ThrowInternalError(context, "remote node relation failed");
    }
    if (read.is_null) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return JS_NULL;
    }
    if ((read.related_special == 0
         && (read.related_stable_key[0] == '\0'
             || read.related_node_type == 0
             || (read.related_node_type == LXB_DOM_NODE_TYPE_ELEMENT
                 && read.related_tag_name[0] == '\0')))
        || read.related_special > 3) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return JS_ThrowInternalError(context,
                                     "invalid remote node relation result");
    }
    JSValue global = JS_GetGlobalObject(context);
    JSValue wrapper = JS_IsException(global) ? JS_EXCEPTION
        : JS_GetPropertyStr(context, global,
                            "__tilefinchWrapRemoteRelation");
    JSValue arguments[5] = {
        JS_NewString(context, read.related_stable_key),
        JS_NewString(context, read.related_tag_name),
        JS_NewInt64(context, (int64_t) read.related_section_index),
        JS_NewInt32(context, (int32_t) read.related_special),
        JS_NewInt32(context, (int32_t) read.related_node_type)
    };
    JSValue value = JS_IsException(global) || JS_IsException(wrapper)
        ? JS_EXCEPTION : JS_Call(context, wrapper, global, 5, arguments);
    for (size_t i = 0; i < 5; i++) JS_FreeValue(context, arguments[i]);
    JS_FreeValue(context, wrapper);
    JS_FreeValue(context, global);
    js_rt_remote_node_read_result_destroy(bridge, &read);
    return value;
}

static JSValue js_remote_node_geometry(JSContext *context,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_node_read == NULL || argc < 2)
        return JS_ThrowInternalError(context,
                                     "remote node reader unavailable");
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int64_t section_value = -1;
    if (JS_ToInt64(context, &section_value, argv[1]) < 0
        || section_value < 0 || (uint64_t) section_value > SIZE_MAX
        || key_length == 0 || key_length > 192) {
        JS_FreeCString(context, key);
        return JS_ThrowTypeError(context, "invalid remote node geometry");
    }
    ScriptRemoteNodeReadResult read = {0};
    bool success = bridge->remote_node_read(
        bridge->remote_node_read_opaque, key, key_length,
        (size_t) section_value, SCRIPT_REMOTE_NODE_GEOMETRY,
        NULL, 0, &read);
    JS_FreeCString(context, key);
    if (!success) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return JS_ThrowInternalError(context, "remote node geometry failed");
    }
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result)) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return result;
    }
#define SET_REMOTE_GEOMETRY(name, field) \
    do { \
        if (JS_SetPropertyStr(context, result, name, \
                              JS_NewInt32(context, read.field)) < 0) { \
            JS_FreeValue(context, result); \
            js_rt_remote_node_read_result_destroy(bridge, &read); \
            return JS_EXCEPTION; \
        } \
    } while (0)
    SET_REMOTE_GEOMETRY("x", geometry_x);
    SET_REMOTE_GEOMETRY("y", geometry_y);
    SET_REMOTE_GEOMETRY("width", geometry_width);
    SET_REMOTE_GEOMETRY("height", geometry_height);
    SET_REMOTE_GEOMETRY("clientWidth", geometry_client_width);
    SET_REMOTE_GEOMETRY("clientHeight", geometry_client_height);
    SET_REMOTE_GEOMETRY("scrollWidth", geometry_scroll_width);
    SET_REMOTE_GEOMETRY("scrollHeight", geometry_scroll_height);
    SET_REMOTE_GEOMETRY("scrollLeft", geometry_scroll_left);
    SET_REMOTE_GEOMETRY("scrollTop", geometry_scroll_top);
#undef SET_REMOTE_GEOMETRY
    js_rt_remote_node_read_result_destroy(bridge, &read);
    return result;
}

static JSValue js_remote_node_attributes(JSContext *context,
                                         JSValueConst this_value,
                                         int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->remote_node_read == NULL || argc < 2)
        return JS_ThrowInternalError(context,
                                     "remote node reader unavailable");
    size_t key_length = 0;
    const char *key = JS_ToCStringLen(context, &key_length, argv[0]);
    if (key == NULL) return JS_EXCEPTION;
    int64_t section_value = -1;
    if (JS_ToInt64(context, &section_value, argv[1]) < 0
        || section_value < 0 || (uint64_t) section_value > SIZE_MAX
        || key_length == 0 || key_length > 192) {
        JS_FreeCString(context, key);
        return JS_ThrowTypeError(context, "invalid remote node read");
    }
    ScriptRemoteNodeReadResult read = {0};
    bool success = bridge->remote_node_read(
        bridge->remote_node_read_opaque, key, key_length,
        (size_t) section_value, SCRIPT_REMOTE_NODE_ATTRIBUTES,
        NULL, 0, &read);
    JS_FreeCString(context, key);
    if (!success || read.attribute_count > 64
        || (read.attribute_count != 0 && read.attributes == NULL)) {
        js_rt_remote_node_read_result_destroy(bridge, &read);
        return JS_ThrowInternalError(context,
                                     "remote node attributes failed");
    }
    JSValue array = JS_NewArray(context);
    bool failed = JS_IsException(array);
    for (size_t i = 0; !failed && i < read.attribute_count; i++) {
        ScriptRemoteNodeAttribute *attribute = &read.attributes[i];
        if ((attribute->name == NULL && attribute->name_length != 0)
            || (attribute->value == NULL && attribute->value_length != 0)) {
            failed = true;
            break;
        }
        JSValue item = JS_NewObject(context);
        if (JS_IsException(item)
            || JS_SetPropertyStr(
                   context, item, "name",
                   JS_NewStringLen(context,
                       attribute->name == NULL ? "" : attribute->name,
                       attribute->name_length)) < 0
            || JS_SetPropertyStr(
                   context, item, "value",
                   JS_NewStringLen(context,
                       attribute->value == NULL ? "" : attribute->value,
                       attribute->value_length)) < 0
            || JS_SetPropertyUint32(context, array, (uint32_t) i, item) < 0) {
            JS_FreeValue(context, item);
            failed = true;
        }
    }
    js_rt_remote_node_read_result_destroy(bridge, &read);
    if (failed) {
        JS_FreeValue(context, array);
        return JS_EXCEPTION;
    }
    return array;
}

JSValue js_remote_descendant_collection(JSContext *context,
                                        DomBridge *bridge,
                                        unsigned root_special,
                                        int32_t what_to_show)
{
    if (bridge == NULL || bridge->remote_descendant_collect == NULL)
        return JS_NULL;
    TilefinchRemoteTraversalNode *nodes = budget_calloc(
        bridge->budget, TILEFINCH_REMOTE_TRAVERSAL_NODE_LIMIT,
        sizeof(*nodes));
    size_t count = 0;
    bool complete = nodes != NULL
        && bridge->remote_descendant_collect(
               bridge->remote_descendant_collect_opaque, root_special,
               what_to_show, nodes, TILEFINCH_REMOTE_TRAVERSAL_NODE_LIMIT,
               &count)
        && count <= TILEFINCH_REMOTE_TRAVERSAL_NODE_LIMIT;
    JSValue values = complete ? JS_NewArray(context) : JS_NULL;
    JSValue global = complete ? JS_GetGlobalObject(context) : JS_UNDEFINED;
    JSValue wrapper = complete && !JS_IsException(global)
        ? JS_GetPropertyStr(context, global, "__tilefinchWrapTraversalNode")
        : JS_UNDEFINED;
    bool exception = JS_IsException(values) || JS_IsException(global)
        || JS_IsException(wrapper) || (complete && !JS_IsFunction(
               context, wrapper));
    for (size_t i = 0; complete && !exception && i < count; i++) {
        TilefinchRemoteTraversalNode *node = &nodes[i];
        bool valid = (node->node_type == 1 || node->node_type == 3
                      || node->node_type == 8)
            && node->special <= 3
            && (node->special != 0 || node->stable_key[0] != '\0');
        if (!valid) {
            complete = false;
            break;
        }
        JSValue arguments[5] = {
            JS_NewString(context, node->stable_key),
            JS_NewString(context, node->tag_name),
            JS_NewInt64(context, (int64_t) node->section_index),
            JS_NewInt32(context, (int32_t) node->special),
            JS_NewInt32(context, (int32_t) node->node_type)
        };
        JSValue value = JS_Call(context, wrapper, global, 5, arguments);
        for (size_t j = 0; j < 5; j++) {
            if (JS_IsException(arguments[j])) exception = true;
            JS_FreeValue(context, arguments[j]);
        }
        if (JS_IsException(value)
            || JS_SetPropertyUint32(context, values, (uint32_t) i,
                                    value) < 0) {
            exception = true;
        }
    }
    JS_FreeValue(context, wrapper);
    JS_FreeValue(context, global);
    budget_free(bridge->budget, nodes);
    if (!complete || exception) {
        JS_FreeValue(context, values);
        return exception ? JS_EXCEPTION : JS_NULL;
    }
    return values;
}

static bool remote_document_binding_empty(
    const ScriptRemoteDocumentBinding *binding)
{
    return binding == NULL
        || (binding->element_lookup == NULL
            && binding->element_opaque == NULL
            && binding->selector_lookup == NULL
            && binding->selector_opaque == NULL
            && binding->selector_collect == NULL
            && binding->selector_collect_opaque == NULL
            && binding->descendant_collect == NULL
            && binding->descendant_collect_opaque == NULL
            && binding->node_read == NULL
            && binding->node_read_opaque == NULL
            && binding->node_write == NULL
            && binding->node_write_opaque == NULL
            && binding->node_visibility == NULL
            && binding->node_visibility_opaque == NULL
            && binding->section_identity == 0);
}

bool script_runtime_rebind_remote_document(
    ScriptRuntime *runtime, const ScriptRemoteDocumentBinding *binding)
{
    if (runtime == NULL) return false;
    bool allowed = runtime->document_scope
                       == SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
                   || remote_document_binding_empty(binding);
    ScriptRemoteDocumentBinding next = {0};
    if (allowed && binding != NULL) next = *binding;

    /* This host is single-threaded: updating the complete operation table in
       one call is the transaction boundary. Invalidate queued work in the
       same boundary so callers may release every old opaque after return. */
    runtime->bridge.remote_element_lookup = next.element_lookup;
    runtime->bridge.remote_element_opaque = next.element_opaque;
    runtime->bridge.remote_selector_lookup = next.selector_lookup;
    runtime->bridge.remote_selector_opaque = next.selector_opaque;
    runtime->bridge.remote_selector_collect = next.selector_collect;
    runtime->bridge.remote_selector_collect_opaque =
        next.selector_collect_opaque;
    runtime->bridge.remote_descendant_collect = next.descendant_collect;
    runtime->bridge.remote_descendant_collect_opaque =
        next.descendant_collect_opaque;
    runtime->bridge.remote_node_read = next.node_read;
    runtime->bridge.remote_node_read_opaque = next.node_read_opaque;
    runtime->bridge.remote_node_write = next.node_write;
    runtime->bridge.remote_node_write_opaque = next.node_write_opaque;
    runtime->bridge.node_visibility = next.node_visibility;
    runtime->bridge.node_visibility_opaque = next.node_visibility_opaque;
    runtime->bridge.section_identity = next.section_identity;
    memset(runtime->bridge.remote_element_sections, 0,
           sizeof(runtime->bridge.remote_element_sections));
    memset(runtime->bridge.remote_element_identifiers, 0,
           sizeof(runtime->bridge.remote_element_identifiers));
    runtime->bridge.remote_element_head = 0;
    runtime->bridge.remote_element_count = 0;
    runtime->bridge.remote_lookup_suppressed = false;
    return allowed;
}

bool script_runtime_take_remote_element_request(
    ScriptRuntime *runtime, char *identifier, size_t capacity,
    size_t *section_index)
{
    if (runtime == NULL || identifier == NULL || capacity == 0
        || section_index == NULL
        || runtime->bridge.remote_element_count == 0) return false;
    size_t at = runtime->bridge.remote_element_head;
    size_t length = strlen(runtime->bridge.remote_element_identifiers[at]);
    if (length >= capacity) return false;
    memcpy(identifier, runtime->bridge.remote_element_identifiers[at],
           length + 1);
    *section_index = runtime->bridge.remote_element_sections[at];
    runtime->bridge.remote_element_identifiers[at][0] = '\0';
    runtime->bridge.remote_element_head = (at + 1) % 8;
    runtime->bridge.remote_element_count--;
    return true;
}

bool js_remote_bindings_install(JSContext *context, JSValue global)
{
    return js_rt_install_function(context, global, "__tilefinchRemoteNodeRead",
                                  js_remote_node_read, 4)
        && js_rt_install_function(context, global, "__tilefinchRemoteNodeWrite",
                                  js_remote_node_write, 5)
        && js_rt_install_function(context, global,
                                  "__tilefinchRemoteNodeAttributes",
                                  js_remote_node_attributes, 2)
        && js_rt_install_function(context, global,
                                  "__tilefinchRemoteNodeRelation",
                                  js_remote_node_relation, 3)
        && js_rt_install_function(context, global,
                                  "__tilefinchRemoteNodeGeometry",
                                  js_remote_node_geometry, 2);
}
