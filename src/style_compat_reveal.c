/* Site-compatibility reveal heuristics: when the bounded script pipeline
   has shed a page's optional bootstrap (sheet->static_custom_element_fallback)
   or a server-rendered hero is staged behind script, reveal the narrow,
   recognizable shells that script would have revealed. Applied once per
   element after the cascade. Split out of style_resolve.c. */

#include "style_budget_category.h"
#include "style_match_internal.h"

#include <ctype.h>
#include <string.h>

static bool style_attribute_token_is(lxb_dom_node_t *node,
                                     const char *name,
                                     const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t wanted_length = strlen(wanted);
    if (value == NULL || wanted_length == 0) return false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t start = at;
        while (at < length && !isspace((unsigned char) value[at])) at++;
        if (at - start == wanted_length
            && memcmp(value + start, wanted, wanted_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool style_subtree_has_staged_text(lxb_dom_node_t *root)
{
    if (root == NULL) return false;
    bool staged = false;
    bool text = false;
    lxb_dom_node_t *node = root->first_child;
    for (size_t visited = 0; node != NULL && visited < 128u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            /* A media carousel commonly has staged text in badges or chips,
               but revealing the whole pane stacks its black/video surface
               over the useful server-rendered introduction. */
            if (style_tag_is(node, "video") || style_tag_is(node, "canvas")) {
                return false;
            }
            if (style_attribute_token_is(node, "class", "opacity-0")) {
                staged = true;
            }
            /* Script-built heroes also commonly stage their primary heading
               by placing opacity:0 on an anonymous generated wrapper.  A
               heading is stronger final-content evidence than a hashed class
               name and avoids retaining a site-specific token table. */
            if (style_tag_is(node, "h1")) staged = true;
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *value = document_text_data(node, &length);
            for (size_t at = 0; value != NULL && at < length; at++) {
                if (!isspace((unsigned char) value[at])) {
                    text = true;
                    break;
                }
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return staged && text;
}

static bool style_attribute_token_contains(lxb_dom_node_t *node,
                                           const char *name,
                                           const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    size_t wanted_length = wanted == NULL ? 0 : strlen(wanted);
    if (value == NULL || wanted_length == 0) return false;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) value[end])) end++;
        for (size_t i = at; i + wanted_length <= end; i++) {
            if (strncasecmp(value + i, wanted, wanted_length) == 0) {
                return true;
            }
        }
        at = end;
    }
    return false;
}

static bool style_subtree_has_actionable_control(lxb_dom_node_t *root)
{
    if (root == NULL) return false;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 128u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t ignored = 0;
            if (style_tag_is(node, "button") || style_tag_is(node, "input")
                || style_tag_is(node, "select")
                || (style_tag_is(node, "a")
                    && document_attribute(node, "href", &ignored) != NULL)) {
                return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return false;
}

static bool style_subtree_has_primary_heading(lxb_dom_node_t *root)
{
    if (root == NULL) return false;
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited < 128u; visited++) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (style_tag_is(node, "video") || style_tag_is(node, "canvas")) {
                return false;
            }
            if (style_tag_is(node, "h1")) return true;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return false;
}

/* Some server-rendered sites deliberately hide the whole page shell until their
   bootstrap adds a ready class.  If that bounded bootstrap is retired, keep
   the published document usable without broadly revealing hidden components.
   This check is paid only for the root/body while static degradation is
   active, uses no allocation, and requires both a semantic heading and a
   useful amount of authored text. */
static bool style_subtree_is_readable_page_shell(lxb_dom_node_t *root)
{
    if (root == NULL) return false;
    bool heading = false;
    size_t text_bytes = 0;
    lxb_dom_node_t *node = root->first_child;
    for (size_t visited = 0; node != NULL && visited < 512u; visited++) {
        bool skip_children = false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            skip_children = style_tag_is(node, "head")
                || style_tag_is(node, "script")
                || style_tag_is(node, "style")
                || style_tag_is(node, "template")
                || style_tag_is(node, "svg")
                || style_tag_is(node, "noscript")
                || style_tag_is(node, "dialog")
                || lxb_dom_element_has_attribute(
                       lxb_dom_interface_element(node),
                       (const lxb_char_t *) "hidden", 6u)
                || style_attribute_token_is(node, "aria-hidden", "true");
            if (!skip_children
                && (style_tag_is(node, "h1")
                    || style_tag_is(node, "h2"))) heading = true;
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *value = document_text_data(node, &length);
            for (size_t at = 0; value != NULL && at < length; at++) {
                if (!isspace((unsigned char) value[at])
                    && text_bytes < 128u) text_bytes++;
            }
        }
        if (!skip_children && node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return heading && text_bytes >= 64u;
}

static bool style_is_direct_main_child(lxb_dom_node_t *node)
{
    return node != NULL && node->parent != NULL
        && style_tag_is(node->parent, "main");
}

/* When the bounded script realm is retired, server-rendered pages can retain
   content staged behind the conventional opacity-0 utility while waiting for
   JavaScript. Reveal that explicit utility and already-visible-parent images.
   Selector-driven carousel panes, dialogs, and elevated fixed surfaces remain
   hidden, so degradation exposes authored content without stacking slides. */
static bool style_static_staged_content_reveals(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style)
{
    if (sheet == NULL || node == NULL || style == NULL || style->opacity != 0
        || style->display == DISPLAY_NONE || style->visibility_hidden
        || style->hidden || (parent != NULL && parent->opacity == 0)) {
        return false;
    }
    /* The bounded subtree walk only when the cheap gate leaves it open. */
    if (!sheet->static_custom_element_fallback
        && !style_subtree_has_primary_heading(node)) {
        return false;
    }
    size_t ignored = 0;
    if (document_attribute(node, "hidden", &ignored) != NULL
        || style_attribute_token_is(node, "aria-hidden", "true")
        || document_attribute(node, "aria-expanded", &ignored) != NULL
        || style_tag_is(node, "dialog")) {
        return false;
    }
    if (style->fixed_position && style->has_z_index && style->z_index > 0) {
        return false;
    }
    if (style_tag_is(node, "img") || style_tag_is(node, "picture")) {
        return true;
    }
    return style_attribute_token_is(node, "class", "opacity-0")
        || style_subtree_has_staged_text(node);
}

void style_compat_post_cascade(const Stylesheet *sheet,
                               lxb_dom_node_t *node,
                               const ComputedStyle *parent,
                               ComputedStyle *style)
{
    /* A text-rich server document may be staged behind body{display:none}
       until optional hydration adds a ready class.  Reveal only that whole-
       page shell after the script pipeline has activated the static fallback;
       element-level hidden content keeps its ordinary cascade semantics. */
    if (sheet != NULL && sheet->static_custom_element_fallback
        && (style_tag_is(node, "html") || style_tag_is(node, "body"))
        && (style->display == DISPLAY_NONE || style->visibility_hidden
            || style->opacity == 0)
        && !style->hidden
        && style_subtree_is_readable_page_shell(node)) {
        if (!lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "hidden", 6u)
            && !style_attribute_token_is(node, "aria-hidden", "true")) {
            if (style->display == DISPLAY_NONE) style->display = DISPLAY_BLOCK;
            style->visibility_hidden = false;
            if (style->opacity == 0) style->opacity = 255;
        }
    }
    /* Script-built mobile headers often ship their complete, accessible
       light DOM but hide it with a CSS-module `loading` token until the
       optional application bundle mounts. If that bundle was shed, reveal
       only a fixed actionable shell; ordinary spinners and modal content do
       not satisfy this narrow fallback. */
    if (sheet != NULL && sheet->static_custom_element_fallback
        && style->display == DISPLAY_NONE && style->fixed_position
        && style_attribute_token_contains(node, "class", "loading")
        && style_subtree_has_actionable_control(node)) {
        size_t ignored = 0;
        if (document_attribute(node, "aria-hidden", &ignored) == NULL) {
            style->display = DISPLAY_BLOCK;
            if (style->opacity == 0) style->opacity = 255;
        }
    }
    /* When script degradation is active, a primary server-rendered hero may
       remain behind a script-controlled wrapper.  Reveal only a non-modal
       wrapper containing an h1; successful pipelines preserve ordinary
       responsive and dialog hiding semantics. */
    if (sheet != NULL && style->display == DISPLAY_NONE && !style->hidden
        && !style->fixed_position
        && (sheet->static_custom_element_fallback
            || style_is_direct_main_child(node))
        && style_subtree_has_primary_heading(node)) {
        if (!lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "hidden", 6u)
            && !style_attribute_token_is(node, "aria-hidden", "true")
            && !style_tag_is(node, "dialog")) {
            style->display = DISPLAY_BLOCK;
            style->visibility_hidden = false;
            if (style->opacity == 0) style->opacity = 255;
        }
    }
    /* A common no-script/mobile bootstrap pattern stages a non-interactive
       full-viewport hero at display:none and opacity:0, then reveals it only
       after a script-side image preload. On a bounded runtime where that
       optional bootstrap fails, the page otherwise loses its entire visual
       backdrop. Treat only a URL-backed, zero-inset, non-elevated fixed
       backdrop as final-state content; modal/dialog surfaces normally carry a
       positive stacking level and do not match this narrow compatibility
       fallback. */
    if (sheet != NULL && sheet->static_custom_element_fallback
        && style->display == DISPLAY_NONE && style->opacity == 0
        && style->fixed_position && style->background_image_kind
               == STYLE_BACKGROUND_IMAGE_URL
        && style->background_image != NULL
        && style->has_top && style->has_right
        && style->has_bottom && style->has_left
        && style->top == 0 && style->right == 0
        && style->bottom == 0 && style->left == 0
        && (!style->has_z_index || style->z_index <= 0)) {
        style->display = DISPLAY_BLOCK;
        style->opacity = 255;
        /* This fallback is specifically a backdrop. Keep it below ordinary
           page content even though fixed layers are cached separately from
           their DOM-order tile content. */
        style->has_z_index = true;
        style->z_index = -1;
    }
    if (style_static_staged_content_reveals(sheet, node, parent, style)) {
        style->opacity = 255;
    }
}
