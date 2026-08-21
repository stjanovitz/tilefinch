#include "tilefinch/controller.h"

#include "tilefinch/fetch.h"
#include "tilefinch/media_discovery.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_NAVIGATION, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_NAVIGATION, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_NAVIGATION, (p), (s))

#define CONTROLLER_EDIT_LIMIT 512
#define CONTROLLER_DEFAULT_VIEWPORT_HEIGHT 272
#define CONTROLLER_DEFAULT_FOCUS_MARGIN 16

static bool controller_set_scroll(BrowserController *controller, int scroll_y,
                                  int viewport_height);

static bool fixed_region_offset(const LayoutDocument *layout, bool control,
                                size_t index, int viewport_height, int *dy)
{
    if (layout == NULL || dy == NULL || viewport_height <= 0) return false;
    for (size_t at = layout->fixed_count; at > 0; at--) {
        const FixedRange *range = &layout->fixed_ranges[at - 1];
        size_t start = control ? range->control_start : range->link_start;
        size_t end = control ? range->control_end : range->link_end;
        if (index < start || index >= end) continue;
        int target_y = range->from_bottom
                       ? viewport_height - range->inset - range->height
                       : range->inset;
        *dy = target_y - range->origin_y;
        return true;
    }
    return false;
}

static size_t focus_count(const BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL
        || !controller->navigation->page.loaded) return 0;
    const LayoutDocument *layout = &controller->navigation->page.layout;
    return layout->link_count + layout->control_count;
}

static bool node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static bool attribute_is(lxb_dom_node_t *node, const char *name,
                         const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    return value != NULL && strlen(wanted) == length
           && strncasecmp(value, wanted, length) == 0;
}

static bool has_attribute(lxb_dom_node_t *node, const char *name)
{
    return node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT
           && lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name));
}

static bool node_descends_from(lxb_dom_node_t *node, lxb_dom_node_t *ancestor)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

static bool node_effectively_disabled(lxb_dom_node_t *node)
{
    if (has_attribute(node, "disabled")) return true;
    if (node_name_is(node, "option")) {
        for (lxb_dom_node_t *at = node->parent; at != NULL;
             at = at->parent) {
            if ((node_name_is(at, "optgroup")
                 || node_name_is(at, "select"))
                && has_attribute(at, "disabled")) return true;
            if (node_name_is(at, "select")) break;
        }
    }
    for (lxb_dom_node_t *at = node == NULL ? NULL : node->parent;
         at != NULL; at = at->parent) {
        if (!node_name_is(at, "fieldset") || !has_attribute(at, "disabled")) {
            continue;
        }
        lxb_dom_node_t *legend = NULL;
        for (lxb_dom_node_t *child = at->first_child; child != NULL;
             child = child->next) {
            if (node_name_is(child, "legend")) { legend = child; break; }
        }
        if (legend != NULL && node_descends_from(node, legend)) continue;
        return true;
    }
    return false;
}

static bool append_url_encoded(char *output, size_t capacity, size_t *used,
                               const char *text, size_t length)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char) text[i];
        bool plain = (value >= 'a' && value <= 'z')
                     || (value >= 'A' && value <= 'Z')
                     || (value >= '0' && value <= '9')
                     || value == '-' || value == '_' || value == '.'
                     || value == '~';
        size_t needed = plain || value == ' ' ? 1 : 3;
        if (*used + needed >= capacity) return false;
        if (plain) output[(*used)++] = (char) value;
        else if (value == ' ') output[(*used)++] = '+';
        else {
            output[(*used)++] = '%';
            output[(*used)++] = hex[value >> 4];
            output[(*used)++] = hex[value & 15u];
        }
    }
    output[*used] = '\0';
    return true;
}

static bool append_form_pair(ControllerAction *action,
                             const char *name, size_t name_length,
                             const char *value, size_t value_length)
{
    if (action->body_length != 0) {
        if (action->body_length + 1 >= sizeof(action->body)) return false;
        action->body[action->body_length++] = '&';
    }
    if (!append_url_encoded(action->body, sizeof(action->body),
                            &action->body_length, name, name_length)
        || action->body_length + 1 >= sizeof(action->body)) return false;
    action->body[action->body_length++] = '=';
    action->body[action->body_length] = '\0';
    return append_url_encoded(action->body, sizeof(action->body),
                              &action->body_length, value, value_length);
}

static bool option_value(lxb_dom_node_t *select, const char **value,
                         size_t *value_length, lxb_char_t **allocated)
{
    lxb_dom_node_t *first = NULL, *selected = NULL;
    bool any_selected = false;
    for (lxb_dom_node_t *at = select->first_child; at != NULL;) {
        if (node_name_is(at, "option")) {
            bool disabled = node_effectively_disabled(at);
            if (!disabled && first == NULL) first = at;
            size_t state_length = 0;
            const char *state = document_attribute(
                at, "data-tilefinch-option-selected", &state_length);
            bool option_selected = state != NULL
                ? state_length == 4 && memcmp(state, "true", 4) == 0
                : has_attribute(at, "selected");
            if (option_selected) {
                any_selected = true;
                if (!disabled && selected == NULL) selected = at;
            }
        }
        if (at->first_child != NULL) {
            at = at->first_child;
            continue;
        }
        while (at != NULL && at != select && at->next == NULL) {
            at = at->parent;
        }
        if (at == NULL || at == select) break;
        at = at->next;
    }
    lxb_dom_node_t *option = selected != NULL ? selected
                             : (any_selected ? NULL : first);
    if (option == NULL) return false;
    *value = document_attribute(option, "value", value_length);
    if (*value == NULL) {
        *allocated = lxb_dom_node_text_content(option, value_length);
        *value = (const char *) *allocated;
    }
    return *value != NULL;
}

static bool serialize_form_controls(lxb_dom_node_t *node,
                                    lxb_dom_node_t *submitter,
                                    ControllerAction *action)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->next) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && !node_effectively_disabled(at)) {
            bool input = node_name_is(at, "input");
            bool textarea = node_name_is(at, "textarea");
            bool select = node_name_is(at, "select");
            bool button = node_name_is(at, "button");
            size_t name_length = 0;
            const char *name = (input || textarea || select || button)
                ? document_attribute(at, "name", &name_length) : NULL;
            bool successful = name != NULL && name_length != 0;
            if (input && (attribute_is(at, "type", "checkbox")
                          || attribute_is(at, "type", "radio"))
                && !has_attribute(at, "checked")) successful = false;
            if (input && (attribute_is(at, "type", "submit")
                          || attribute_is(at, "type", "button")
                          || attribute_is(at, "type", "reset"))) {
                successful = successful && at == submitter
                             && attribute_is(at, "type", "submit");
            }
            if (button) successful = successful && at == submitter
                                      && !attribute_is(at, "type", "button")
                                      && !attribute_is(at, "type", "reset");
            const char *value = NULL;
            size_t value_length = 0;
            lxb_char_t *allocated = NULL;
            if (successful) {
                if (textarea) {
                    value = document_control_value(at, &value_length);
                    if (value == NULL) {
                        allocated =
                            lxb_dom_node_text_content(at, &value_length);
                        value = (const char *) allocated;
                    }
                } else if (select) {
                    successful = option_value(at, &value, &value_length,
                                              &allocated);
                } else {
                    value = input
                        ? document_control_value(at, &value_length) : NULL;
                    if (value == NULL) {
                        value = document_attribute(
                            at, "value", &value_length);
                    }
                    if (value == NULL && input
                        && (attribute_is(at, "type", "checkbox")
                            || attribute_is(at, "type", "radio"))) {
                        value = "on"; value_length = 2;
                    }
                    if (value == NULL) { value = ""; value_length = 0; }
                }
                if (successful && value != NULL
                    && !append_form_pair(action, name, name_length,
                                         value, value_length)) {
                    if (allocated != NULL) {
                        lxb_dom_document_destroy_text(at->owner_document,
                                                      allocated);
                    }
                    return false;
                }
            }
            if (allocated != NULL) {
                lxb_dom_document_destroy_text(at->owner_document, allocated);
            }
        }
        if (at->first_child != NULL
            && !serialize_form_controls(at->first_child, submitter, action)) {
            return false;
        }
    }
    return true;
}

static lxb_dom_node_t *form_ancestor(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (node_name_is(at, "form")) return at;
    }
    return NULL;
}

static bool input_blocks_implicit_submission(lxb_dom_node_t *node)
{
    if (!node_name_is(node, "input")) return false;
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    if (type == NULL || length == 0) return true;
    static const char *const blocking_types[] = {
        "text", "search", "tel", "url", "email", "password",
        "date", "month", "week", "time", "datetime-local", "number"
    };
    for (size_t i = 0;
         i < sizeof(blocking_types) / sizeof(blocking_types[0]); i++) {
        if (strlen(blocking_types[i]) == length
            && strncasecmp(type, blocking_types[i], length) == 0) {
            return true;
        }
    }
    return false;
}

static bool node_is_submit_button(lxb_dom_node_t *node)
{
    return (node_name_is(node, "button")
            && !attribute_is(node, "type", "button")
            && !attribute_is(node, "type", "reset"))
           || (node_name_is(node, "input")
               && (attribute_is(node, "type", "submit")
                   || attribute_is(node, "type", "image")));
}

static bool media_type_supported(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *type = document_attribute(node, "type", &length);
    if (type == NULL || length == 0) return true;
    static const char mp4[] = "video/mp4";
    if (length < sizeof(mp4) - 1u
        || strncasecmp(type, mp4, sizeof(mp4) - 1u) != 0) return false;
    if (length == sizeof(mp4) - 1u) return true;
    unsigned char next = (unsigned char) type[sizeof(mp4) - 1u];
    return next == ';' || isspace(next);
}

static const char *media_source_reference(
    BrowserController *controller, lxb_dom_node_t *video, size_t *length)
{
    const char *source = document_attribute(video, "src", length);
    if (source != NULL && *length != 0) return source;
    unsigned child_elements = 0;
    for (lxb_dom_node_t *child = video == NULL ? NULL : video->first_child;
         child != NULL; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (child_elements++ >= 16u) break;
        if (!node_name_is(child, "source")) continue;
        /* Match the bootstrap's limit. A hostile element may contain the
           document's full node allowance; activation must remain a small,
           deterministic input operation rather than a second DOM walk. */
        if (!media_type_supported(child)) {
            continue;
        }
        size_t media_length = 0;
        const char *media = document_attribute(
            child, "media", &media_length);
        if (media != NULL && media_length != 0
            && !stylesheet_media_matches(
                   &controller->navigation->page.stylesheet,
                   media, media_length)) continue;
        source = document_attribute(child, "src", length);
        if (source != NULL && *length != 0) return source;
    }
    return NULL;
}

static void media_credentials_policy(
    lxb_dom_node_t *video, TilefinchRequestMode *mode,
    TilefinchCredentialsMode *credentials)
{
    *mode = TILEFINCH_REQUEST_MODE_NO_CORS;
    *credentials = TILEFINCH_CREDENTIALS_INCLUDE;
    size_t length = 0;
    const char *crossorigin = document_attribute(
        video, "crossorigin", &length);
    if (crossorigin == NULL) return;
    *mode = TILEFINCH_REQUEST_MODE_CORS;
    *credentials = length == sizeof("use-credentials") - 1u
            && strncasecmp(
                   crossorigin, "use-credentials",
                   sizeof("use-credentials") - 1u) == 0
        ? TILEFINCH_CREDENTIALS_INCLUDE
        : TILEFINCH_CREDENTIALS_SAME_ORIGIN;
}

static bool controller_build_media_action(
    BrowserController *controller, lxb_dom_node_t *video,
    ControllerAction *action)
{
    if (controller == NULL || video == NULL || action == NULL
        || !node_name_is(video, "video")) return false;
    size_t source_length = 0;
    const char *source = media_source_reference(
        controller, video, &source_length);
    bool discovered = false;
    MediaDiscoveryKind discovered_kind = MEDIA_DISCOVERY_NONE;
    if (source == NULL || source_length == 0) {
        MediaDiscoveryResult discovery = {0};
        if (!media_discover_document_candidate(
                &controller->navigation->page.document,
                action->body, sizeof(action->body), &discovery)) {
            return false;
        }
        source = action->body;
        source_length = strlen(action->body);
        discovered = true;
        discovered_kind = discovery.kind;
    }
    if (source_length == 0 || source_length >= NAVIGATION_URL_LIMIT) {
        return false;
    }
    /* ControllerAction already owns a bounded body scratch area. Reuse its
       first half while resolving instead of adding a 2 KiB frame to the
       activation hot path on Allegrex. Media actions never carry a form
       body, and the scratch is cleared before returning. */
    if (!discovered) {
        memcpy(action->body, source, source_length);
        action->body[source_length] = '\0';
    }
    const NavigationEntry *current = navigation_current(
        controller->navigation);
    const char *base = current == NULL ? NULL : current->url;
    if (base == NULL || !fetch_resolve_url(
            base, action->body, action->url, sizeof(action->url))) return false;
    if (!tilefinch_csp_allows_request(
            &controller->navigation->page.document.content_security_policy,
            TILEFINCH_DESTINATION_MEDIA, action->url)) return false;
    media_credentials_policy(
        video, &action->media_mode, &action->media_credentials);
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    /* Playback and its asynchronous DOM state reports may outlive a page
       wrapper. Pin the handle just as HTMLMediaElement.play() does; terminal
       state delivery releases it, while document teardown remains the bound
       if the user closes the native player first. */
    action->media_node_handle = runtime == NULL ? 0
        : script_runtime_node_handle(runtime, video);
    action->media_kind = discovered_kind;
    action->body[0] = '\0';
    action->body_length = 0;
    action->type = CONTROLLER_ACTION_MEDIA;
    return true;
}

static void form_implicit_controls(lxb_dom_node_t *node,
                                   lxb_dom_node_t **default_submitter,
                                   size_t *blocking_fields)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->next) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && !node_effectively_disabled(at)) {
            if (*default_submitter == NULL && node_is_submit_button(at)) {
                *default_submitter = at;
            }
            if (input_blocks_implicit_submission(at)) (*blocking_fields)++;
        }
        if (at->first_child != NULL) {
            form_implicit_controls(at->first_child, default_submitter,
                                   blocking_fields);
        }
    }
}

static bool controller_build_implicit_form_action(
    BrowserController *controller, lxb_dom_node_t *input,
    ControllerAction *action)
{
    lxb_dom_node_t *form = form_ancestor(input);
    if (form == NULL || node_effectively_disabled(input)
        || !input_blocks_implicit_submission(input)) return false;
    lxb_dom_node_t *submitter = NULL;
    size_t blocking_fields = 0;
    form_implicit_controls(form->first_child, &submitter, &blocking_fields);
    NavigationSession *navigation = controller->navigation;
    if (submitter != NULL) {
        if (navigation->page.runtime != NULL
            && !navigation_dispatch_node_activation(navigation, submitter)) {
            return false;
        }
        if (navigation->page.runtime != NULL
            && navigation->page.script_result.last_event_cancelled) {
            memset(action, 0, sizeof(*action));
            return true;
        }
        return controller_build_form_action(controller, form, submitter, true,
                                            action);
    }
    if (blocking_fields > 1) return false;
    return controller_build_form_action(controller, form, NULL, true, action);
}

bool controller_build_form_action(
    BrowserController *controller, lxb_dom_node_t *form,
    lxb_dom_node_t *submitter, bool dispatch_submit_event,
    ControllerAction *action)
{
    if (controller == NULL || controller->navigation == NULL
        || form == NULL || action == NULL || !node_name_is(form, "form")
        || (submitter != NULL && form_ancestor(submitter) != form)) {
        return false;
    }
    NavigationSession *navigation = controller->navigation;
    memset(action, 0, sizeof(*action));
    if (dispatch_submit_event && navigation->page.runtime != NULL
        && (!navigation_dispatch_node_submit_event(navigation, form,
                                                   submitter)
            || navigation->page.script_result.last_event_cancelled)) {
        return navigation->page.script_result.last_event_cancelled;
    }
    const NavigationEntry *current = navigation_current(navigation);
    if (current == NULL) return false;
    size_t action_length = 0;
    const char *target = document_attribute(form, "action", &action_length);
    char target_copy[NAVIGATION_URL_LIMIT];
    if (target == NULL || action_length == 0) {
        snprintf(target_copy, sizeof(target_copy), "%s", current->url);
    } else {
        if (action_length >= sizeof(target_copy)) return false;
        memcpy(target_copy, target, action_length);
        target_copy[action_length] = '\0';
    }
    if (!fetch_resolve_url(current->url, target_copy, action->url,
                           sizeof(action->url))) return false;
    if (!tilefinch_csp_allows_form_action(
            &navigation->page.document.content_security_policy,
            action->url)) return false;
    bool post = attribute_is(form, "method", "post");
    snprintf(action->method, sizeof(action->method), "%s",
             post ? "POST" : "GET");
    snprintf(action->content_type, sizeof(action->content_type),
             "application/x-www-form-urlencoded");
    action->type = CONTROLLER_ACTION_FORM_SUBMIT;
    if (!serialize_form_controls(form->first_child, submitter, action)) {
        return false;
    }
    if (!post && action->body_length != 0) {
        size_t url_length = strlen(action->url);
        size_t needed = 1 + action->body_length;
        if (url_length + needed >= sizeof(action->url)) return false;
        action->url[url_length++] = strchr(action->url, '?') == NULL
                                    ? '?' : '&';
        memcpy(action->url + url_length, action->body,
               action->body_length + 1);
        action->body_length = 0;
        action->body[0] = '\0';
    }
    return true;
}

static lxb_dom_node_t *retained_focus_node(
    const BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL) return NULL;
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    if (runtime != NULL && controller->focus_handle != 0) {
        return script_runtime_node_handle_resolve(
            runtime, controller->focus_handle);
    }
    return controller->focus_node;
}

static void retain_focus_node(BrowserController *controller,
                              lxb_dom_node_t *node)
{
    controller->focus_node = node;
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    controller->focus_handle = runtime == NULL || node == NULL
        ? 0 : script_runtime_node_weak_handle(runtime, node);
}

static lxb_dom_node_t *retained_pointer_down_node(
    const BrowserController *controller)
{
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    if (runtime != NULL) {
        return controller->pointer_down_handle == 0 ? NULL
            : script_runtime_node_handle_resolve(
                  runtime, controller->pointer_down_handle);
    }
    return controller->pointer_down_node;
}

static void retain_pointer_down_node(BrowserController *controller,
                                     lxb_dom_node_t *node)
{
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    controller->pointer_down_node = runtime == NULL ? node : NULL;
    controller->pointer_down_handle = runtime == NULL || node == NULL
        ? 0 : script_runtime_node_weak_handle(runtime, node);
    controller->pointer_down_active = node != NULL
        && (runtime == NULL || controller->pointer_down_handle != 0);
}

static lxb_dom_node_t *retained_pointer_hover_node(
    const BrowserController *controller)
{
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    if (runtime != NULL) {
        return controller->pointer_hover_handle == 0 ? NULL
            : script_runtime_node_handle_resolve(
                  runtime, controller->pointer_hover_handle);
    }
    return controller->pointer_hover_node;
}

static void retain_pointer_hover_node(BrowserController *controller,
                                      lxb_dom_node_t *node)
{
    ScriptRuntime *runtime = controller->navigation->page.runtime;
    controller->pointer_hover_node = runtime == NULL ? node : NULL;
    controller->pointer_hover_handle = runtime == NULL || node == NULL
        ? 0 : script_runtime_node_weak_handle(runtime, node);
}

static void clear_link_focus_identity(BrowserController *controller)
{
    controller->focus_link_url[0] = '\0';
    controller->focus_link_url_length = 0;
}

static bool link_region_matches_focus(const BrowserController *controller,
                                      const LinkRegion *link)
{
    return controller->focus_link_url_length != 0 && link != NULL
        && link->url_length == controller->focus_link_url_length
        && memcmp(link->url, controller->focus_link_url,
                  link->url_length) == 0;
}

static bool retain_link_focus(BrowserController *controller, size_t index)
{
    const LayoutDocument *layout = &controller->navigation->page.layout;
    if (index >= layout->link_count) return false;
    const LinkRegion *link = &layout->links[index];
    if (link->url_length == 0
        || link->url_length >= sizeof(controller->focus_link_url)) {
        return false;
    }
    controller->focus_kind = CONTROLLER_FOCUS_LINK;
    controller->focus_index = index;
    memcpy(controller->focus_link_url, link->url, link->url_length);
    controller->focus_link_url[link->url_length] = '\0';
    controller->focus_link_url_length = link->url_length;
    retain_focus_node(controller, link->node);
    return true;
}

static bool resolve_focus_node(BrowserController *controller,
                               lxb_dom_node_t *node)
{
    const LayoutDocument *layout = &controller->navigation->page.layout;
    /* A live retained node is stronger than a URL shared by several links. */
    if (node == NULL && controller->focus_kind == CONTROLLER_FOCUS_LINK
        && controller->focus_link_url_length != 0) {
        if (controller->focus_index < layout->link_count
            && link_region_matches_focus(
                controller, &layout->links[controller->focus_index])) {
            retain_focus_node(
                controller, layout->links[controller->focus_index].node);
            return true;
        }
        for (size_t i = 0; i < layout->link_count; i++) {
            if (!link_region_matches_focus(controller, &layout->links[i])) {
                continue;
            }
            controller->focus_index = i;
            retain_focus_node(controller, layout->links[i].node);
            return true;
        }
    }
    if (controller->focus_kind == CONTROLLER_FOCUS_POINTER
        && controller->pointer_node == node) return true;
    if (controller->focus_kind == CONTROLLER_FOCUS_LINK
        && controller->focus_index < layout->link_count
        && layout->links[controller->focus_index].node == node) {
        retain_focus_node(controller, node);
        return true;
    }
    if (controller->focus_kind == CONTROLLER_FOCUS_CONTROL
        && controller->focus_index < layout->control_count
        && layout->controls[controller->focus_index].node == node) {
        retain_focus_node(controller, node);
        return true;
    }
    bool control = false;
    size_t index = 0;
    if (layout_focus_for_node(layout, node, &control, &index)) {
        controller->focus_kind = control ? CONTROLLER_FOCUS_CONTROL
                                         : CONTROLLER_FOCUS_LINK;
        controller->focus_index = index;
        if (control) {
            clear_link_focus_identity(controller);
            retain_focus_node(controller, node);
        } else if (!retain_link_focus(controller, index)) {
            return false;
        }
        return true;
    }
    /* OOM while constructing the optional index must not change focus
       correctness. Only that case retains the former bounded linear path. */
    if (layout->focus_index != NULL) return false;
    for (size_t i = 0; i < layout->link_count; i++) {
        if (layout->links[i].node != node) continue;
        controller->focus_kind = CONTROLLER_FOCUS_LINK;
        controller->focus_index = i;
        return retain_link_focus(controller, i);
    }
    for (size_t i = 0; i < layout->control_count; i++) {
        if (layout->controls[i].node != node) continue;
        controller->focus_kind = CONTROLLER_FOCUS_CONTROL;
        controller->focus_index = i;
        clear_link_focus_identity(controller);
        retain_focus_node(controller, node);
        return true;
    }
    controller->focus_kind = CONTROLLER_FOCUS_NONE;
    controller->focus_index = 0;
    controller->focus_node = NULL;
    controller->focus_handle = 0;
    controller->has_authored_focus_outline = false;
    clear_link_focus_identity(controller);
    controller->pointer_node = NULL;
    return false;
}

static bool current_focus_region(const BrowserController *controller,
                                 bool *control, size_t *index)
{
    if (controller == NULL || controller->navigation == NULL
        || control == NULL || index == NULL) return false;
    const LayoutDocument *layout = &controller->navigation->page.layout;
    lxb_dom_node_t *focus_node = retained_focus_node(controller);
    bool want_control =
        controller->focus_kind == CONTROLLER_FOCUS_CONTROL;
    if (controller->focus_kind != CONTROLLER_FOCUS_LINK && !want_control) {
        return false;
    }
    if (focus_node == NULL
        && (want_control || controller->focus_link_url_length == 0)) {
        return false;
    }
    size_t candidate = controller->focus_index;
    if (!want_control && focus_node == NULL
        && controller->focus_link_url_length != 0) {
        if (candidate < layout->link_count
            && link_region_matches_focus(controller, &layout->links[candidate])) {
            *control = false;
            *index = candidate;
            return true;
        }
        for (size_t i = 0; i < layout->link_count; i++) {
            if (!link_region_matches_focus(controller, &layout->links[i])) {
                continue;
            }
            *control = false;
            *index = i;
            return true;
        }
        return false;
    }
    if ((!want_control && candidate < layout->link_count
         && layout->links[candidate].node == focus_node)
        || (want_control && candidate < layout->control_count
            && layout->controls[candidate].node == focus_node)) {
        *control = want_control;
        *index = candidate;
        return true;
    }
    if (layout_focus_for_node(
            layout, focus_node, control, index)) return true;
    if (layout->focus_index != NULL) return false;
    for (size_t i = 0; i < layout->link_count; i++) {
        if (layout->links[i].node != focus_node) continue;
        *control = false;
        *index = i;
        return true;
    }
    for (size_t i = 0; i < layout->control_count; i++) {
        if (layout->controls[i].node != focus_node) continue;
        *control = true;
        *index = i;
        return true;
    }
    return false;
}

static void controller_refresh_authored_focus_outline(
    BrowserController *controller, lxb_dom_node_t *node)
{
    if (controller == NULL || controller->navigation == NULL) return;
    controller->has_authored_focus_outline = false;
    controller->authored_focus_stylesheet_generation =
        controller->navigation->page.stylesheet.build_generation;
    controller->authored_focus_relayout_generation =
        controller->navigation->incremental_relayouts;
    if (node == NULL
        || (controller->focus_kind != CONTROLLER_FOCUS_LINK
            && controller->focus_kind != CONTROLLER_FOCUS_CONTROL)) return;
    if (controller->navigation->focus_outline_cache_valid
        && controller->navigation->focus_outline_cache_node == node
        && controller->navigation->focus_outline_cache_stylesheet_generation
               == controller->navigation->page.stylesheet.build_generation
        && controller->navigation->focus_outline_cache_relayout_generation
               == controller->navigation->incremental_relayouts) {
        controller->authored_focus_outline = (ControllerFocusOutline) {
            .color = controller->navigation->focus_outline_cache_color,
            .offset = controller->navigation->focus_outline_cache_offset,
            .alpha = controller->navigation->focus_outline_cache_alpha,
            .width = controller->navigation->focus_outline_cache_width,
            .style = controller->navigation->focus_outline_cache_style
        };
        controller->has_authored_focus_outline = true;
        controller->navigation->focus_outline_cache_valid = false;
        controller->navigation->focus_outline_cache_node = NULL;
        return;
    }
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return;
        ancestors[count++] = at;
    }
    ComputedStyle parent = {0};
    bool has_parent = false;
    while (count != 0) {
        parent = style_for_node(
            &controller->navigation->page.stylesheet,
            ancestors[--count], has_parent ? &parent : NULL);
        has_parent = true;
    }
    ComputedStyle normal = {0}, focused = {0};
    if (!style_focus_change_is_outline_only(
            &controller->navigation->page.stylesheet, node,
            has_parent ? &parent : NULL, &normal, &focused)) return;
    unsigned width = computed_style_outline_width(&focused);
    unsigned line_style = computed_style_outline_style(&focused);
    if (width == 0 || width > UINT8_MAX
        || line_style == STYLE_OUTLINE_NONE
        || line_style > UINT8_MAX) return;
    bool current_color =
        (focused.outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0;
    controller->authored_focus_outline = (ControllerFocusOutline) {
        .color = current_color ? focused.color : focused.outline_color,
        .offset = computed_style_outline_offset(&focused),
        .alpha = current_color ? focused.color_alpha : focused.outline_alpha,
        .width = (uint8_t) width,
        .style = (uint8_t) line_style
    };
    controller->has_authored_focus_outline =
        controller->authored_focus_outline.alpha != 0;
}

static bool synchronize_dom_focus(BrowserController *controller,
                                  lxb_dom_node_t *node)
{
    NavigationSession *navigation = controller->navigation;
    if (navigation->page.runtime != NULL) {
        /*
         * Focus movement is a user-agent operation. A page-realm failure
         * must not swallow the d-pad press; focus events are notifications,
         * and `focus` is not cancelable.
         */
        (void) navigation_dispatch_node_event(navigation, node, "focus");
    }
    bool resolved = resolve_focus_node(controller, node);
    if (!resolved) {
        controller->has_authored_focus_outline = false;
        return false;
    }
    controller_refresh_authored_focus_outline(controller, node);
    return true;
}

static bool select_flat_focus(BrowserController *controller, size_t flat)
{
    const LayoutDocument *layout = &controller->navigation->page.layout;
    lxb_dom_node_t *node = NULL;
    if (flat < layout->link_count) {
        if (!retain_link_focus(controller, flat)) return false;
        node = layout->links[flat].node;
    } else {
        controller->focus_kind = CONTROLLER_FOCUS_CONTROL;
        controller->focus_index = flat - layout->link_count;
        clear_link_focus_identity(controller);
        node = layout->controls[controller->focus_index].node;
        retain_focus_node(controller, node);
    }
    controller->focus_moves++;
    return synchronize_dom_focus(controller, node)
           && controller_reveal_focus(controller);
}

static size_t flat_focus(const BrowserController *controller)
{
    const LayoutDocument *layout = &controller->navigation->page.layout;
    return controller->focus_kind == CONTROLLER_FOCUS_CONTROL
           ? layout->link_count + controller->focus_index
           : controller->focus_index;
}

static lxb_dom_node_t *flat_focus_node(const LayoutDocument *layout,
                                      size_t flat)
{
    if (layout == NULL) return NULL;
    if (flat < layout->link_count) return layout->links[flat].node;
    flat -= layout->link_count;
    return flat < layout->control_count ? layout->controls[flat].node : NULL;
}

static bool controller_focus_relative(BrowserController *controller,
                                      bool forward)
{
    size_t count = focus_count(controller);
    if (count == 0) return false;
    bool had_focus = controller->focus_kind == CONTROLLER_FOCUS_LINK
        || controller->focus_kind == CONTROLLER_FOCUS_CONTROL;
    if (had_focus
        && !resolve_focus_node(controller, retained_focus_node(controller))) {
        controller->focus_kind = CONTROLLER_FOCUS_NONE;
        controller->has_authored_focus_outline = false;
        had_focus = false;
    }
    lxb_dom_node_t *current = had_focus
        ? retained_focus_node(controller) : NULL;
    size_t candidate = had_focus ? flat_focus(controller)
        : (forward ? count - 1u : 0u);
    /*
     * LinkRegion is a hit-test fragment, not a tab stop. A wrapped anchor may
     * contribute many adjacent regions; walk at most the raw region count and
     * select the next distinct DOM node without allocating a second focus map.
     */
    for (size_t visited = 0; visited < count; visited++) {
        candidate = forward
            ? (candidate + 1u == count ? 0u : candidate + 1u)
            : (candidate == 0 ? count - 1u : candidate - 1u);
        lxb_dom_node_t *node = flat_focus_node(
            &controller->navigation->page.layout, candidate);
        if (node == NULL || (current != NULL && node == current)) continue;
        return select_flat_focus(controller, candidate);
    }
    /* A page with one semantic focus target has nowhere to move. Treat the
       input as handled without dispatching a duplicate focus event. */
    return current != NULL;
}

bool controller_init(BrowserController *controller,
                     NavigationSession *navigation)
{
    if (controller == NULL || navigation == NULL
        || !navigation->page.loaded) return false;
    memset(controller, 0, sizeof(*controller));
    controller->navigation = navigation;
    controller->viewport_height = navigation->viewport.css_height > 0
                                  ? navigation->viewport.css_height
                                  : CONTROLLER_DEFAULT_VIEWPORT_HEIGHT;
    controller->focus_margin = viewport_device_to_css(
        &navigation->viewport, CONTROLLER_DEFAULT_FOCUS_MARGIN);
    if (controller->focus_margin < 1) controller->focus_margin = 1;
    return true;
}

bool controller_configure_viewport(BrowserController *controller,
                                   int viewport_height, int focus_margin)
{
    if (controller == NULL || controller->navigation == NULL
        || viewport_height <= 0 || focus_margin < 0) return false;
    const ViewportContext *viewport = &controller->navigation->viewport;
    int css_height = viewport_device_to_css(viewport, viewport_height);
    int css_margin = viewport_device_to_css(viewport, focus_margin);
    if (css_margin < 1 && focus_margin > 0) css_margin = 1;
    if (css_height <= 0 || css_margin * 2 >= css_height) return false;
    controller->viewport_height = css_height;
    controller->focus_margin = css_margin;
    return true;
}

bool controller_focus_next(BrowserController *controller)
{
    return controller_focus_relative(controller, true);
}

bool controller_focus_previous(BrowserController *controller)
{
    return controller_focus_relative(controller, false);
}

static bool focus_region_rect(const BrowserController *controller,
                              bool control, size_t index,
                              bool require_area,
                              int *x, int *y, int *width, int *height)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    const LayoutDocument *layout = &controller->navigation->page.layout;
    int rx, ry, rw, rh;
    if (control) {
        if (index >= layout->control_count) return false;
        const ControlRegion *region = &layout->controls[index];
        rx = region->x; ry = region->y;
        rw = region->width; rh = region->height;
    } else {
        if (index >= layout->link_count) return false;
        const LinkRegion *region = &layout->links[index];
        rx = region->x; ry = region->y;
        rw = region->width; rh = region->height;
    }
    if (require_area && (rw <= 0 || rh <= 0)) return false;
    int fixed_dy = 0;
    if (fixed_region_offset(layout, control, index,
                            controller->viewport_height, &fixed_dy)) {
        const NavigationEntry *entry =
            navigation_current(controller->navigation);
        ry += fixed_dy + (entry == NULL ? 0 : entry->scroll_y);
    }
    if (x != NULL) *x = rx;
    if (y != NULL) *y = ry;
    if (width != NULL) *width = rw;
    if (height != NULL) *height = rh;
    return true;
}

static bool focus_candidate_score(ControllerFocusDirection direction,
                                  int from_x, int from_y,
                                  int from_width, int from_height,
                                  int to_x, int to_y,
                                  int to_width, int to_height,
                                  int viewport_height, int64_t *score,
                                  bool *in_beam)
{
    int64_t from_cx = (int64_t) from_x * 2 + from_width;
    int64_t from_cy = (int64_t) from_y * 2 + from_height;
    int64_t to_cx = (int64_t) to_x * 2 + to_width;
    int64_t to_cy = (int64_t) to_y * 2 + to_height;
    int64_t primary = 0;
    int64_t secondary = 0;
    bool beam = false;
    if (direction == CONTROLLER_FOCUS_UP
        || direction == CONTROLLER_FOCUS_DOWN) {
        if ((direction == CONTROLLER_FOCUS_UP && to_cy >= from_cy)
            || (direction == CONTROLLER_FOCUS_DOWN && to_cy <= from_cy)) {
            return false;
        }
        primary = to_cy > from_cy ? to_cy - from_cy : from_cy - to_cy;
        int64_t from_right = (int64_t) from_x + from_width;
        int64_t to_right = (int64_t) to_x + to_width;
        beam = to_x < from_right && from_x < to_right;
        if (to_right <= from_x) secondary = from_x - to_right;
        else if (from_right <= to_x) secondary = to_x - from_right;
    } else {
        if ((direction == CONTROLLER_FOCUS_LEFT && to_cx >= from_cx)
            || (direction == CONTROLLER_FOCUS_RIGHT && to_cx <= from_cx)) {
            return false;
        }
        primary = to_cx > from_cx ? to_cx - from_cx : from_cx - to_cx;
        int64_t from_bottom = (int64_t) from_y + from_height;
        int64_t to_bottom = (int64_t) to_y + to_height;
        beam = to_y < from_bottom && from_y < to_bottom;
        if (to_bottom <= from_y) secondary = from_y - to_bottom;
        else if (from_bottom <= to_y) secondary = to_y - from_bottom;
    }
    int64_t off_beam_penalty =
        (int64_t) (viewport_height > 0 ? viewport_height : 272) * 512 + 1;
    *score = primary * 1024 + secondary * 64
             + (beam ? 0 : off_beam_penalty);
    if (in_beam != NULL) *in_beam = beam;
    return true;
}

bool controller_focus_direction(BrowserController *controller,
                                ControllerFocusDirection direction)
{
    size_t count = focus_count(controller);
    if (count == 0) return false;
    switch (direction) {
        case CONTROLLER_FOCUS_UP:
        case CONTROLLER_FOCUS_DOWN:
        case CONTROLLER_FOCUS_LEFT:
        case CONTROLLER_FOCUS_RIGHT:
            break;
        default:
            return false;
    }
    if ((controller->focus_kind == CONTROLLER_FOCUS_LINK
         || controller->focus_kind == CONTROLLER_FOCUS_CONTROL)
        && !resolve_focus_node(controller, retained_focus_node(controller))) {
        controller->focus_kind = CONTROLLER_FOCUS_NONE;
        controller->has_authored_focus_outline = false;
    }
    int from_x = 0, from_y = 0, from_width = 0, from_height = 0;
    if (!controller_focused_rect(controller, &from_x, &from_y,
                                 &from_width, &from_height)) {
        return direction == CONTROLLER_FOCUS_UP
                   || direction == CONTROLLER_FOCUS_LEFT
               ? controller_focus_previous(controller)
               : controller_focus_next(controller);
    }

    const LayoutDocument *layout = &controller->navigation->page.layout;
    lxb_dom_node_t *from_node = retained_focus_node(controller);
    bool found = false;
    bool best_in_beam = false;
    bool best_control = false;
    size_t best_index = 0;
    int64_t best_score = INT64_MAX;
    for (size_t kind = 0; kind < 2; kind++) {
        bool control = kind != 0;
        size_t region_count =
            control ? layout->control_count : layout->link_count;
        for (size_t index = 0; index < region_count; index++) {
            lxb_dom_node_t *node = control ? layout->controls[index].node
                                           : layout->links[index].node;
            if (node == NULL || node == from_node) continue;
            int x = 0, y = 0, width = 0, height = 0;
            if (!focus_region_rect(controller, control, index, true,
                                   &x, &y, &width, &height)) continue;
            int64_t score = 0;
            bool in_beam = false;
            if (!focus_candidate_score(
                    direction, from_x, from_y, from_width, from_height,
                    x, y, width, height, controller->viewport_height,
                    &score, &in_beam)) continue;
            /* A target aligned with the requested axis is preferable to any
               diagonal target. Without this tier, a nearer header/search
               control can steal Right from an adjacent card because its
               center distance outweighs the finite off-beam penalty. */
            if (found && best_in_beam && !in_beam) continue;
            if (found && best_in_beam == in_beam && score >= best_score)
                continue;
            best_score = score;
            best_in_beam = in_beam;
            best_control = control;
            best_index = index;
            found = true;
        }
    }
    if (found) {
        size_t flat = best_control
            ? layout->link_count + best_index : best_index;
        return select_flat_focus(controller, flat);
    }
    return direction == CONTROLLER_FOCUS_UP
               || direction == CONTROLLER_FOCUS_LEFT
           ? controller_focus_previous(controller)
           : controller_focus_next(controller);
}

typedef struct {
    ControllerFocusKind kind;
    size_t index;
    lxb_dom_node_t *node;
    int x;
    int y;
    int width;
    int height;
    int client_x;
    int client_y;
} ControllerHit;

static bool controller_hit_test(BrowserController *controller,
                                int device_x, int device_y,
                                ControllerHit *hit)
{
    if (controller == NULL || controller->navigation == NULL || hit == NULL
        || !controller->navigation->page.loaded) return false;
    const NavigationEntry *entry = navigation_current(controller->navigation);
    const LayoutDocument *layout = &controller->navigation->page.layout;
    int x = viewport_device_to_css(&layout->viewport, device_x);
    int client_y = viewport_device_to_css(&layout->viewport, device_y);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    int y = client_y + scroll_y;
    memset(hit, 0, sizeof(*hit));
    hit->client_x = x;
    hit->client_y = client_y;
    /* Later-painted overlapping regions are hit first. */
    for (size_t i = layout->control_count; i-- > 0;) {
        const ControlRegion *region = &layout->controls[i];
        int region_y = region->y;
        int fixed_dy = 0;
        if (fixed_region_offset(layout, true, i, controller->viewport_height,
                                &fixed_dy)) {
            region_y += fixed_dy + scroll_y;
        }
        if (x < region->x || x >= region->x + region->width
            || y < region_y || y >= region_y + region->height) continue;
        hit->kind = CONTROLLER_FOCUS_CONTROL;
        hit->index = i;
        hit->node = region->node;
        hit->x = region->x;
        hit->y = region_y;
        hit->width = region->width;
        hit->height = region->height;
        return true;
    }
    for (size_t i = layout->link_count; i-- > 0;) {
        const LinkRegion *region = &layout->links[i];
        int region_y = region->y;
        int fixed_dy = 0;
        if (fixed_region_offset(layout, false, i, controller->viewport_height,
                                &fixed_dy)) {
            region_y += fixed_dy + scroll_y;
        }
        if (x < region->x || x >= region->x + region->width
            || y < region_y || y >= region_y + region->height) continue;
        hit->kind = CONTROLLER_FOCUS_LINK;
        hit->index = i;
        hit->node = region->node;
        hit->x = region->x;
        hit->y = region_y;
        hit->width = region->width;
        hit->height = region->height;
        return true;
    }
    /* The earliest retained containing box is the deepest painted element. */
    for (size_t at = 0; at < layout->node_box_count; at++) {
        const LayoutNodeBox *box = &layout->node_boxes[at];
        if (box->node == NULL || box->width <= 0 || box->height <= 0
            || x < box->x || x >= box->x + box->width
            || y < box->y || y >= box->y + box->height) continue;
        hit->kind = CONTROLLER_FOCUS_POINTER;
        hit->node = box->node;
        hit->x = box->x;
        hit->y = box->y;
        hit->width = box->width;
        hit->height = box->height;
        return true;
    }
    return false;
}

static bool controller_select_hit(BrowserController *controller,
                                  const ControllerHit *hit,
                                  bool synchronize)
{
    controller->has_authored_focus_outline = false;
    if (hit->kind == CONTROLLER_FOCUS_LINK) {
        if (!retain_link_focus(controller, hit->index)) return false;
        controller->pointer_node = NULL;
    } else {
        controller->focus_kind = hit->kind;
        controller->focus_index = hit->index;
        clear_link_focus_identity(controller);
        retain_focus_node(controller, hit->node);
        controller->pointer_node =
            hit->kind == CONTROLLER_FOCUS_POINTER ? hit->node : NULL;
        if (controller->pointer_node != NULL) {
            controller->pointer_x = hit->x;
            controller->pointer_y = hit->y;
            controller->pointer_width = hit->width;
            controller->pointer_height = hit->height;
        }
    }
    controller->focus_moves++;
    if (synchronize && hit->kind != CONTROLLER_FOCUS_POINTER
        && !synchronize_dom_focus(controller, hit->node)) return false;
    return controller_reveal_focus(controller);
}

bool controller_focus_at(BrowserController *controller, int x, int y)
{
    ControllerHit hit;
    return controller_hit_test(controller, x, y, &hit)
        && controller_select_hit(controller, &hit, true);
}

bool controller_pointer_event(BrowserController *controller,
                              ControllerPointerPhase phase,
                              int x, int y, bool *activate)
{
    if (activate != NULL) *activate = false;
    if (controller == NULL || controller->navigation == NULL
        || !controller->navigation->page.loaded) return false;
    ControllerHit hit;
    bool have_hit = controller_hit_test(controller, x, y, &hit);
    const LayoutDocument *layout = &controller->navigation->page.layout;
    int client_x = viewport_device_to_css(&layout->viewport, x);
    int client_y = viewport_device_to_css(&layout->viewport, y);
    int offset_x = 0, offset_y = 0;
    if (have_hit) {
        const NavigationEntry *entry =
            navigation_current(controller->navigation);
        offset_x = hit.client_x - hit.x;
        offset_y = hit.client_y + (entry == NULL ? 0 : entry->scroll_y)
                   - hit.y;
    }
    NavigationSession *navigation = controller->navigation;
    if (phase == CONTROLLER_POINTER_MOVE) {
        if (controller->pointer_resize_active) return true;
        retain_pointer_hover_node(controller, have_hit ? hit.node : NULL);
        if (navigation->page.runtime == NULL) return true;
        return navigation_dispatch_node_pointer(
            navigation, have_hit ? hit.node : NULL, 1,
            have_hit ? hit.client_x : client_x,
            have_hit ? hit.client_y : client_y,
            offset_x, offset_y,
            controller->pointer_down_active ? 1u : 0u);
    }
    if (phase == CONTROLLER_POINTER_DOWN) {
        controller->pointer_click_pending = false;
        retain_pointer_down_node(controller, NULL);
        if (!have_hit || !controller_select_hit(controller, &hit, false)) {
            return true;
        }
        retain_pointer_down_node(controller, hit.node);
        if (!controller->pointer_down_active) return false;
        if (hit.kind == CONTROLLER_FOCUS_CONTROL
            && hit.index < layout->control_count
            && layout->controls[hit.index].type == CONTROL_RESIZE) {
            const LayoutNodeBox *box = layout_box_for_node(layout, hit.node);
            char resize[32] = "";
            if (box != NULL && style_retained_property_value(
                    &navigation->page.stylesheet, hit.node,
                    "resize", sizeof("resize") - 1u,
                    resize, sizeof(resize))) {
                controller->pointer_resize_mode =
                    strcasecmp(resize, "both") == 0 ? STYLE_RESIZE_BOTH
                    : ((strcasecmp(resize, "horizontal") == 0
                        || strcasecmp(resize, "inline") == 0)
                       ? STYLE_RESIZE_HORIZONTAL
                       : ((strcasecmp(resize, "vertical") == 0
                           || strcasecmp(resize, "block") == 0)
                          ? STYLE_RESIZE_VERTICAL : STYLE_RESIZE_NONE));
                controller->pointer_resize_active =
                    controller->pointer_resize_mode != STYLE_RESIZE_NONE;
                controller->pointer_resize_start_x = client_x;
                controller->pointer_resize_start_y = client_y;
                controller->pointer_resize_start_width = box->width;
                controller->pointer_resize_start_height = box->height;
            }
            return true;
        }
        if (navigation->page.runtime != NULL
            && !navigation_dispatch_node_pointer(
                navigation, hit.node, 2, hit.client_x, hit.client_y,
                offset_x, offset_y, 1)) {
            retain_pointer_down_node(controller, NULL);
            return false;
        }
        return true;
    }
    lxb_dom_node_t *down = retained_pointer_down_node(controller);
    if (phase == CONTROLLER_POINTER_CANCEL) {
        bool ok = down == NULL || navigation->page.runtime == NULL
            || navigation_dispatch_node_pointer(
                navigation, down, 4, client_x, client_y, 0, 0, 0);
        retain_pointer_down_node(controller, NULL);
        controller->pointer_resize_active = false;
        controller->pointer_click_pending = false;
        return ok;
    }
    if (phase != CONTROLLER_POINTER_UP) return false;
    if (controller->pointer_resize_active) {
        lxb_dom_node_t *resize_node = down;
        int width = controller->pointer_resize_start_width;
        int height = controller->pointer_resize_start_height;
        if (controller->pointer_resize_mode == STYLE_RESIZE_BOTH
            || controller->pointer_resize_mode == STYLE_RESIZE_HORIZONTAL) {
            width += client_x - controller->pointer_resize_start_x;
        }
        if (controller->pointer_resize_mode == STYLE_RESIZE_BOTH
            || controller->pointer_resize_mode == STYLE_RESIZE_VERTICAL) {
            height += client_y - controller->pointer_resize_start_y;
        }
        if (width < 40) width = 40;
        if (height < 24) height = 24;
        int maximum_width = layout->viewport.css_width > 0
            ? layout->viewport.css_width * 2 : 960;
        int maximum_height = layout->viewport.css_height > 0
            ? layout->viewport.css_height * 3 : 816;
        if (width > maximum_width) width = maximum_width;
        if (height > maximum_height) height = maximum_height;
        controller->pointer_resize_active = false;
        retain_pointer_down_node(controller, NULL);
        controller->pointer_click_pending = false;
        if (resize_node == NULL
            || !document_control_resize_set(
                &navigation->page.document, resize_node, width, height)
            || !navigation_relayout(navigation)
            || !resolve_focus_node(controller, resize_node)) return false;
        return controller_reveal_focus(controller);
    }
    lxb_dom_node_t *up = have_hit ? hit.node : down;
    bool same = down != NULL && have_hit && up == down;
    bool ok = up == NULL || navigation->page.runtime == NULL
        || navigation_dispatch_node_pointer(
            navigation, up, 3,
            have_hit ? hit.client_x : client_x,
            have_hit ? hit.client_y : client_y,
            offset_x, offset_y, 0);
    retain_pointer_down_node(controller, NULL);
    controller->pointer_click_pending = same && ok;
    controller->pointer_click_x = have_hit ? hit.client_x : client_x;
    controller->pointer_click_y = have_hit ? hit.client_y : client_y;
    controller->pointer_click_offset_x = offset_x;
    controller->pointer_click_offset_y = offset_y;
    if (activate != NULL) *activate = controller->pointer_click_pending;
    return ok;
}

void controller_pointer_discard_click(BrowserController *controller)
{
    if (controller != NULL) controller->pointer_click_pending = false;
}

static bool controller_dispatch_activation(BrowserController *controller,
                                           lxb_dom_node_t *node)
{
    NavigationSession *navigation = controller->navigation;
    bool click_only = controller->pointer_click_pending;
    controller->pointer_click_pending = false;
    if (navigation->page.runtime == NULL || node == NULL) return true;
    return click_only
        ? navigation_dispatch_node_pointer(
              navigation, node, 5, controller->pointer_click_x,
              controller->pointer_click_y,
              controller->pointer_click_offset_x,
              controller->pointer_click_offset_y, 0)
        : navigation_dispatch_node_activation(navigation, node);
}

bool controller_commit_pointer_click(BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    if (!controller->pointer_click_pending) return true;
    lxb_dom_node_t *node = retained_focus_node(controller);
    if (node == NULL) {
        controller->pointer_click_pending = false;
        return true;
    }
    return controller_dispatch_activation(controller, node);
}

bool controller_focus_node(BrowserController *controller,
                           lxb_dom_node_t *node)
{
    if (controller == NULL || controller->navigation == NULL
        || node == NULL) return false;
    if (!resolve_focus_node(controller, node)) return false;
    controller->focus_moves++;
    return synchronize_dom_focus(controller, node)
           && controller_reveal_focus(controller);
}

bool controller_focused_rect(const BrowserController *controller,
                             int *x, int *y, int *width, int *height)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    int rx, ry, rw, rh;
    bool control_focus = false;
    size_t region_index = 0;
    bool region = current_focus_region(
        controller, &control_focus, &region_index);
    if (region) {
        if (!focus_region_rect(
                controller, control_focus, region_index, false,
                               &rx, &ry, &rw, &rh)) return false;
        if (!control_focus) {
            const LayoutDocument *layout =
                &controller->navigation->page.layout;
            lxb_dom_node_t *node = retained_focus_node(controller);
            const LayoutNodeBox *box = layout_box_for_node(layout, node);
            /*
             * A block anchor contributes one authoritative border-box link
             * region as well as regions for its positioned/decorative
             * descendants. Unioning those fragments can pull the device
             * focus ring toward an overlaid play glyph even though the link's
             * visible silhouette is its border box. Prefer the exact retained
             * box region when it exists; ordinary multi-line inline anchors
             * have no such region and retain the fragment union below.
             */
            size_t block_box_index = SIZE_MAX;
            for (size_t i = 0; box != NULL && i < layout->link_count; i++) {
                const LinkRegion *link = &layout->links[i];
                if (link->node != node || link->x != box->x
                    || link->y != box->y || link->width != box->width
                    || link->height != box->height) continue;
                block_box_index = i;
                break;
            }
            bool block_box_region = block_box_index != SIZE_MAX;
            if (block_box_region) {
                const LinkRegion *link = &layout->links[block_box_index];
                bool contains_fragments = true;
                for (size_t fragment = 0;
                     fragment < layout->link_count; fragment++) {
                    const LinkRegion *part = &layout->links[fragment];
                    if (part->node != node) continue;
                    if (part->x < link->x || part->y < link->y
                        || part->x + part->width > link->x + link->width
                        || part->y + part->height
                               > link->y + link->height) {
                        contains_fragments = false;
                        break;
                    }
                }
                block_box_region = contains_fragments;
                if (block_box_region
                    && !focus_region_rect(
                           controller, false, block_box_index, false,
                           &rx, &ry, &rw, &rh)) return false;
            }
            if (block_box_region) goto resolved;
            int dy = ry - layout->links[region_index].y;
            int right = rx + rw, bottom = ry + rh;
            for (size_t i = 0; node != NULL && i < layout->link_count; i++) {
                const LinkRegion *link = &layout->links[i];
                if (link->node != node) continue;
                int y = link->y + dy;
                if (link->x < rx) rx = link->x;
                if (y < ry) ry = y;
                if (link->x + link->width > right)
                    right = link->x + link->width;
                if (y + link->height > bottom) bottom = y + link->height;
            }
            rw = right - rx;
            rh = bottom - ry;
        }
    } else if (controller->focus_kind == CONTROLLER_FOCUS_POINTER
               && controller->pointer_node != NULL) {
        rx = controller->pointer_x; ry = controller->pointer_y;
        rw = controller->pointer_width; rh = controller->pointer_height;
    } else {
        return false;
    }
resolved:
    if (x != NULL) *x = rx;
    if (y != NULL) *y = ry;
    if (width != NULL) *width = rw;
    if (height != NULL) *height = rh;
    return true;
}

bool controller_focused_outline_style(
    BrowserController *controller, ControllerFocusOutline *outline)
{
    if (controller == NULL || controller->navigation == NULL
        || outline == NULL) return false;
    if (controller->authored_focus_stylesheet_generation
            != controller->navigation->page.stylesheet.build_generation
        || controller->authored_focus_relayout_generation
            != controller->navigation->incremental_relayouts) {
        controller_refresh_authored_focus_outline(
            controller, retained_focus_node(controller));
    }
    if (!controller->has_authored_focus_outline) return false;
    *outline = controller->authored_focus_outline;
    return true;
}

bool controller_activate(BrowserController *controller,
                         ControllerAction *action)
{
    if (controller == NULL || controller->navigation == NULL
        || action == NULL) return false;
    memset(action, 0, sizeof(*action));
    NavigationSession *navigation = controller->navigation;
    if (controller->focus_kind != CONTROLLER_FOCUS_NONE
        && !resolve_focus_node(controller, retained_focus_node(controller))) {
        controller->pointer_click_pending = false;
        return false;
    }
    const LayoutDocument *layout = &navigation->page.layout;
    if (controller->focus_kind == CONTROLLER_FOCUS_LINK
        && controller->focus_index < layout->link_count) {
        const LinkRegion *link = &layout->links[controller->focus_index];
        const NavigationEntry *current = navigation_current(navigation);
        bool resolved = current != NULL
            && fetch_resolve_url(current->url, link->url, action->url,
                                 sizeof(action->url));
        if (!resolved) {
            size_t length = link->url_length;
            if (length >= sizeof(action->url)) length = sizeof(action->url) - 1;
            memcpy(action->url, link->url, length);
            action->url[length] = '\0';
        }
        lxb_dom_node_t *node = link->node;
        action->type = CONTROLLER_ACTION_NAVIGATE;
        controller->activations++;
        if (!controller_dispatch_activation(controller, node)) {
            return false;
        }
        if (navigation->page.runtime != NULL
            && navigation->page.script_result.last_event_cancelled) {
            action->type = CONTROLLER_ACTION_NONE;
            action->url[0] = '\0';
        }
        return true;
    }
    if (controller->focus_kind == CONTROLLER_FOCUS_CONTROL
        && controller->focus_index < layout->control_count) {
        lxb_dom_node_t *node = layout->controls[controller->focus_index].node;
        action->type = CONTROLLER_ACTION_CONTROL;
        controller->activations++;
        if (!controller_dispatch_activation(controller, node)) {
            return false;
        }
        if (navigation->page.runtime != NULL
            && navigation->page.script_result.last_event_cancelled) {
            action->type = CONTROLLER_ACTION_NONE;
            return true;
        }
        if (node_name_is(node, "video")) {
            return controller_build_media_action(
                controller, node, action);
        }
        bool submit = node_is_submit_button(node);
        if (submit && form_ancestor(node) != NULL) {
            return controller_build_form_action(
                controller, form_ancestor(node), node, true, action);
        }
        if (input_blocks_implicit_submission(node)
            && form_ancestor(node) != NULL) {
            return controller_build_implicit_form_action(controller, node,
                                                         action);
        }
        return true;
    }
    if (controller->focus_kind == CONTROLLER_FOCUS_POINTER
        && retained_focus_node(controller) != NULL) {
        lxb_dom_node_t *node = retained_focus_node(controller);
        action->type = CONTROLLER_ACTION_CONTROL;
        controller->activations++;
        if (!controller_dispatch_activation(controller, node)) return false;
        if (navigation->page.runtime != NULL
            && navigation->page.script_result.last_event_cancelled) {
            action->type = CONTROLLER_ACTION_NONE;
        } else if (node_name_is(node, "video")) {
            return controller_build_media_action(
                controller, node, action);
        }
        return true;
    }
    return false;
}

static bool set_control_value(BrowserController *controller,
                              const char *value, size_t length,
                              const char *data, const char *input_type)
{
    NavigationSession *navigation = controller->navigation;
    LayoutDocument *layout = &navigation->page.layout;
    if (!resolve_focus_node(controller, retained_focus_node(controller))) {
        return false;
    }
    if (controller->focus_kind != CONTROLLER_FOCUS_CONTROL
        || controller->focus_index >= layout->control_count) return false;
    ControlRegion control = layout->controls[controller->focus_index];
    if (control.type != CONTROL_INPUT && control.type != CONTROL_TEXTAREA
        && control.type != CONTROL_EDITABLE) {
        return false;
    }
    if (navigation->page.runtime != NULL) {
        if (!navigation_dispatch_node_input_event(
                navigation, control.node, "beforeinput", data, input_type,
                NULL)) {
            return false;
        }
        if (navigation->page.script_result.last_event_cancelled) return true;
    }
    BudgetAllocationOwner previous_owner =
        document_allocation_owner_enter(&navigation->page.document);
    lxb_status_t status;
    if (control.type == CONTROL_TEXTAREA || control.type == CONTROL_EDITABLE) {
        status = lxb_dom_node_text_content_set(
            control.node, (const lxb_char_t *) value, length);
    } else {
        status = lxb_dom_element_set_attribute(
            lxb_dom_interface_element(control.node),
            (const lxb_char_t *) "value", 5,
            (const lxb_char_t *) value, length) == NULL
                 ? LXB_STATUS_ERROR : LXB_STATUS_OK;
    }
    bool refreshed = status == LXB_STATUS_OK
        && document_refresh(&navigation->page.document);
    document_allocation_owner_leave(&navigation->page.document,
                                    previous_owner);
    if (!refreshed || !navigation_relayout(navigation)) return false;
    controller->text_edits++;
    if (navigation->page.runtime != NULL) {
        /* Re-find the corresponding control after relayout by stable node. */
        for (size_t i = 0; i < navigation->page.layout.control_count; i++) {
            if (navigation->page.layout.controls[i].node == control.node) {
                controller->focus_index = i;
                retain_focus_node(controller, control.node);
                break;
            }
        }
        if (!navigation_dispatch_node_input_event(
                navigation, control.node, "input", data, input_type, value)) {
            return false;
        }
    }
    return true;
}

static bool current_control_value(const BrowserController *controller,
                                  char output[CONTROLLER_EDIT_LIMIT + 1],
                                  size_t *length)
{
    const LayoutDocument *layout = &controller->navigation->page.layout;
    bool control_focus = false;
    size_t index = 0;
    if (!current_focus_region(controller, &control_focus, &index)
        || !control_focus) return false;
    const ControlRegion *control = &layout->controls[index];
    size_t source_length = 0;
    const char *source =
        document_control_value(control->node, &source_length);
    if (source == NULL && control->type == CONTROL_INPUT) {
        source = document_attribute(
            control->node, "value", &source_length);
    }
    lxb_char_t *allocated = NULL;
    if (control->type == CONTROL_TEXTAREA
        || control->type == CONTROL_EDITABLE) {
        allocated = lxb_dom_node_text_content(control->node, &source_length);
        source = (const char *) allocated;
    }
    if (source == NULL) source_length = 0;
    if (source_length > CONTROLLER_EDIT_LIMIT) source_length = CONTROLLER_EDIT_LIMIT;
    if (source_length != 0) memcpy(output, source, source_length);
    output[source_length] = '\0';
    if (allocated != NULL) {
        lxb_dom_document_destroy_text(control->node->owner_document,
                                      allocated);
    }
    *length = source_length;
    return control->type == CONTROL_INPUT || control->type == CONTROL_TEXTAREA
           || control->type == CONTROL_EDITABLE;
}

bool controller_text_value(const BrowserController *controller,
                           char *output, size_t capacity, size_t *length)
{
    if (controller == NULL || output == NULL || capacity == 0) return false;
    char value[CONTROLLER_EDIT_LIMIT + 1];
    size_t value_length = 0;
    if (!current_control_value(controller, value, &value_length)) return false;
    size_t copied = value_length < capacity - 1 ? value_length : capacity - 1;
    if (copied > 0) memcpy(output, value, copied);
    output[copied] = '\0';
    if (length != NULL) *length = copied;
    return true;
}

static bool autocomplete_is_sensitive(lxb_dom_node_t *node)
{
    static const char *const sensitive_tokens[] = {
        "current-password", "new-password", "one-time-code",
        "cc-number", "cc-csc"
    };
    size_t length = 0;
    const char *autocomplete =
        document_attribute(node, "autocomplete", &length);
    if (autocomplete == NULL) return false;
    size_t at = 0;
    while (at < length) {
        while (at < length
               && isspace((unsigned char) autocomplete[at])) at++;
        size_t start = at;
        while (at < length
               && !isspace((unsigned char) autocomplete[at])) at++;
        size_t token_length = at - start;
        for (size_t i = 0;
             i < sizeof(sensitive_tokens) / sizeof(sensitive_tokens[0]);
             i++) {
            size_t wanted = strlen(sensitive_tokens[i]);
            if (token_length == wanted
                && strncasecmp(
                       autocomplete + start,
                       sensitive_tokens[i], wanted) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool controller_text_input_info(
    const BrowserController *controller, ControllerTextInputInfo *info)
{
    if (info == NULL) return false;
    memset(info, 0, sizeof(*info));
    if (controller == NULL || controller->navigation == NULL) return false;
    bool control_focus = false;
    size_t index = 0;
    if (!current_focus_region(controller, &control_focus, &index)
        || !control_focus) return false;
    const LayoutDocument *layout = &controller->navigation->page.layout;
    if (index >= layout->control_count) return false;
    const ControlRegion *control = &layout->controls[index];
    if (control->type != CONTROL_INPUT
        && control->type != CONTROL_TEXTAREA
        && control->type != CONTROL_EDITABLE) return false;

    info->editable = true;
    info->multiline = control->type != CONTROL_INPUT;
    info->voice_allowed = !autocomplete_is_sensitive(control->node);
    if (control->type == CONTROL_INPUT) {
        size_t type_length = 0;
        const char *type =
            document_attribute(control->node, "type", &type_length);
        bool password = type != NULL && type_length == 8
            && strncasecmp(type, "password", 8) == 0;
        info->keyboard_url_mode =
            type != NULL && type_length == 3
            && strncasecmp(type, "url", 3) == 0;
        if (password) info->voice_allowed = false;
    }
    return true;
}

bool controller_replace_text(BrowserController *controller,
                             const char *utf8, size_t length)
{
    if (controller == NULL || utf8 == NULL) return false;
    char ignored[CONTROLLER_EDIT_LIMIT + 1];
    size_t previous_length = 0;
    if (!current_control_value(controller, ignored, &previous_length)) {
        return false;
    }
    (void) previous_length;
    if (length > CONTROLLER_EDIT_LIMIT) length = CONTROLLER_EDIT_LIMIT;
    char value[CONTROLLER_EDIT_LIMIT + 1];
    if (length > 0) memcpy(value, utf8, length);
    value[length] = '\0';
    return set_control_value(controller, value, length, value,
                             "insertReplacementText");
}

bool controller_insert_text(BrowserController *controller,
                            const char *utf8, size_t length)
{
    if (controller == NULL || utf8 == NULL) return false;
    char value[CONTROLLER_EDIT_LIMIT + 1];
    size_t current = 0;
    if (!current_control_value(controller, value, &current)) return false;
    if (length > CONTROLLER_EDIT_LIMIT - current) {
        length = CONTROLLER_EDIT_LIMIT - current;
    }
    memcpy(value + current, utf8, length);
    current += length;
    value[current] = '\0';
    char data[CONTROLLER_EDIT_LIMIT + 1];
    memcpy(data, utf8, length);
    data[length] = '\0';
    return set_control_value(controller, value, current, data, "insertText");
}

bool controller_backspace(BrowserController *controller)
{
    if (controller == NULL) return false;
    char value[CONTROLLER_EDIT_LIMIT + 1];
    size_t length = 0;
    if (!current_control_value(controller, value, &length) || length == 0) {
        return false;
    }
    do {
        length--;
    } while (length > 0 && ((unsigned char) value[length] & 0xc0u) == 0x80u);
    value[length] = '\0';
    return set_control_value(controller, value, length, NULL,
                             "deleteContentBackward");
}

bool controller_reveal_focus(BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL
        || controller->viewport_height <= 0) return false;
    bool nested_changed = layout_scroll_node_reveal(
        &controller->navigation->page.layout,
        retained_focus_node(controller));
    int y = 0, height = 0;
    if (!controller_focused_rect(controller, NULL, &y, NULL, &height)) {
        return nested_changed;
    }
    const NavigationEntry *entry = navigation_current(controller->navigation);
    if (entry == NULL) return false;
    int scroll_y = entry->scroll_y;
    int visible_top = scroll_y + controller->focus_margin;
    int visible_bottom = scroll_y + controller->viewport_height
                         - controller->focus_margin;
    int target = scroll_y;
    if (y < visible_top) {
        target = y - controller->focus_margin;
    } else if (y + height > visible_bottom) {
        target = y + height - controller->viewport_height
                 + controller->focus_margin;
    }
    return target == scroll_y
        ? true
        : controller_set_scroll(controller, target,
                                controller->viewport_height)
              || nested_changed;
}

static bool controller_set_scroll(BrowserController *controller, int scroll_y,
                                  int viewport_height)
{
    if (controller == NULL || controller->navigation == NULL
        || viewport_height <= 0) return false;
    int maximum = viewport_max_scroll_css(
        &controller->navigation->viewport,
        controller->navigation->page.layout.height);
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > maximum) scroll_y = maximum;
    return navigation_set_scroll(controller->navigation, scroll_y);
}

bool controller_scroll_by(BrowserController *controller, int delta_y,
                          int viewport_height)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    lxb_dom_node_t *target = retained_pointer_hover_node(controller);
    if (target == NULL) target = retained_focus_node(controller);
    int css_delta = viewport_device_to_css(
        &controller->navigation->viewport, delta_y);
    if (css_delta == 0) return true;
    int remaining_y = css_delta;
    bool nested = target != NULL && layout_scroll_node_chain(
        &controller->navigation->page.layout, target, 0, css_delta,
        NULL, &remaining_y);
    if (remaining_y == 0) return nested;
    const NavigationEntry *entry = navigation_current(controller->navigation);
    if (entry == NULL) return nested;
    long long requested = (long long) entry->scroll_y + remaining_y;
    if (requested < 0) requested = 0;
    if (requested > INT_MAX) requested = INT_MAX;
    return controller_set_scroll(controller, (int) requested,
                                 viewport_height) || nested;
}

bool controller_scroll_settle(BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    lxb_dom_node_t *target = retained_pointer_hover_node(controller);
    if (target == NULL) target = retained_focus_node(controller);
    return target != NULL && layout_scroll_node_settle(
        &controller->navigation->page.layout, target);
}

LayoutCursor controller_pointer_cursor(const BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL) {
        return LAYOUT_CURSOR_AUTO;
    }
    lxb_dom_node_t *node = retained_pointer_hover_node(controller);
    return layout_cursor_for_node(
        &controller->navigation->page.stylesheet, node);
}

LayoutScrollbarWidth controller_root_scrollbar_width(
    const BrowserController *controller)
{
    if (controller == NULL || controller->navigation == NULL) {
        return LAYOUT_SCROLLBAR_AUTO;
    }
    return layout_root_scrollbar_width(
        &controller->navigation->page.layout,
        &controller->navigation->page.stylesheet);
}

bool controller_scroll_step(BrowserController *controller, int direction,
                            unsigned held_frames, int viewport_height)
{
    if (direction != -1 && direction != 1) return false;
    unsigned acceleration = held_frames / 6u;
    if (acceleration > 10u) acceleration = 10u;
    int amount = 8 + (int) acceleration * 4;
    return controller_scroll_by(controller, direction * amount,
                                viewport_height);
}

bool controller_scroll_page(BrowserController *controller, int direction,
                            int viewport_height)
{
    if ((direction != -1 && direction != 1) || viewport_height <= 0) {
        return false;
    }
    if (controller == NULL || controller->navigation == NULL) return false;
    int css_viewport_height = controller->navigation->viewport.css_height;
    int css_overlap = css_viewport_height / 8;
    int minimum_overlap = viewport_device_to_css(
        &controller->navigation->viewport, 16);
    if (css_overlap < minimum_overlap) css_overlap = minimum_overlap;
    const NavigationEntry *entry = navigation_current(controller->navigation);
    if (entry == NULL) return false;
    int delta = css_viewport_height - css_overlap;
    int requested_delta = direction * delta;
    lxb_dom_node_t *target = retained_pointer_hover_node(controller);
    if (target == NULL) target = retained_focus_node(controller);
    int remaining_y = requested_delta;
    bool nested = target != NULL && layout_scroll_node_chain(
        &controller->navigation->page.layout, target,
        0, requested_delta, NULL, &remaining_y);
    if (nested) {
        nested |= layout_scroll_node_settle(
            &controller->navigation->page.layout, target);
    }
    if (remaining_y == 0) return nested;
    long long requested = (long long) entry->scroll_y + remaining_y;
    if (requested < 0) requested = 0;
    if (requested > INT_MAX) requested = INT_MAX;
    return controller_set_scroll(controller, (int) requested,
                                 viewport_height) || nested;
}

bool controller_scroll_to_top(BrowserController *controller,
                              int viewport_height)
{
    return controller_set_scroll(controller, 0, viewport_height);
}

bool controller_scroll_to_bottom(BrowserController *controller,
                                 int viewport_height)
{
    if (controller == NULL || controller->navigation == NULL) return false;
    return controller_set_scroll(controller,
                                 controller->navigation->page.layout.height,
                                 viewport_height);
}

bool controller_execute_action(BrowserController *controller,
                               const ControllerAction *action,
                               size_t maximum_bytes, long timeout_ms)
{
    if (controller == NULL || controller->navigation == NULL
        || action == NULL
        || (action->type != CONTROLLER_ACTION_NAVIGATE
            && action->type != CONTROLLER_ACTION_FORM_SUBMIT)) return false;
    NavigationSession *navigation = controller->navigation;
    ControllerAction copy = *action;
    const NavigationEntry *current = navigation_current(navigation);
    if (current != NULL) {
        snprintf(navigation->pending_navigation_referer,
                 sizeof(navigation->pending_navigation_referer), "%s",
                 current->url);
    }
    uint64_t generation = navigation_begin(navigation);
    const char *method = copy.type == CONTROLLER_ACTION_FORM_SUBMIT
                         ? copy.method : "GET";
    bool loaded = navigation_load_request(
        navigation, generation, copy.url, method,
        copy.body_length == 0 ? NULL : copy.body, copy.body_length,
        copy.content_type[0] == '\0' ? NULL : copy.content_type,
        maximum_bytes, timeout_ms,
        navigation->viewport.device_width == 0
          ? 480 : navigation->viewport.device_width,
        navigation->fonts, navigation->images, true);
    if (loaded) {
        controller->focus_kind = CONTROLLER_FOCUS_NONE;
        controller->focus_index = 0;
        controller->focus_node = NULL;
        controller->focus_handle = 0;
        controller->has_authored_focus_outline = false;
        clear_link_focus_identity(controller);
        controller->pointer_node = NULL;
        controller->pointer_hover_node = NULL;
        controller->pointer_hover_handle = 0;
    }
    return loaded;
}
