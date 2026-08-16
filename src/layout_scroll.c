#include "tilefinch/layout.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/platform.h"

#include "layout_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
    SCROLL_AXIS_X = 1u,
    SCROLL_AXIS_Y = 2u,
    SCROLL_MANDATORY = 1u << 2,
    SCROLL_SMOOTH = 1u << 3,
    SCROLL_OVERSCROLL_X_SHIFT = 4,
    SCROLL_OVERSCROLL_Y_SHIFT = 6,
    SCROLL_SCROLLBAR_SHIFT = 8
};

static int16_t scroll_clamp_i16(int value)
{
    if (value < INT16_MIN) return INT16_MIN;
    if (value > INT16_MAX) return INT16_MAX;
    return (int16_t) value;
}

static bool scroll_retained(const Stylesheet *sheet, lxb_dom_node_t *node,
                            const char *name, char *value, size_t capacity)
{
    return style_retained_property_value(
        sheet, node, name, strlen(name), value, capacity);
}

static unsigned scroll_tokens(const char *value, const char **starts,
                              size_t *lengths, unsigned capacity)
{
    size_t length = strlen(value);
    unsigned count = 0;
    for (size_t at = 0; at < length && count < capacity;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        if (at == length) break;
        size_t start = at;
        unsigned parentheses = 0;
        char quote = 0;
        while (at < length) {
            char c = value[at];
            if (quote != 0) {
                if (c == '\\' && at + 1 < length) at++;
                else if (c == quote) quote = 0;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '(') {
                parentheses++;
            } else if (c == ')' && parentheses != 0) {
                parentheses--;
            } else if (parentheses == 0
                       && isspace((unsigned char) c)) {
                break;
            }
            at++;
        }
        starts[count] = value + start;
        lengths[count] = at - start;
        count++;
    }
    return count;
}

static int scroll_length(const Stylesheet *sheet, const char *value,
                         size_t length, int reference, bool allow_auto)
{
    if (allow_auto && length == 4
        && strncasecmp(value, "auto", 4) == 0) return 0;
    bool percent = false;
    int parsed = style_parse_length(sheet, value, length, 0, &percent);
    if (percent) {
        parsed = (int) ((int64_t) parsed * reference / 100);
    }
    return parsed;
}

static unsigned overscroll_code(const char *value, size_t length)
{
    if (length == 7 && strncasecmp(value, "contain", 7) == 0) return 1u;
    if (length == 4 && strncasecmp(value, "none", 4) == 0) return 2u;
    return 0u;
}

static uint16_t rgb565_from_rgb888(uint32_t rgb)
{
    return tilefinch_rgb565_pack_u8(
        (rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu);
}

static void scroll_container_style(
    const Stylesheet *sheet, const LayoutNodeBox *box,
    LayoutScrollContainer *container)
{
    char value[96];
    if (scroll_retained(
            sheet, box->node, "scroll-snap-type", value, sizeof(value))) {
        const char *parts[2] = {0};
        size_t lengths[2] = {0};
        unsigned count = scroll_tokens(value, parts, lengths, 2);
        if (count != 0) {
            if ((lengths[0] == 1
                 && tolower((unsigned char) parts[0][0]) == 'x')
                || (lengths[0] == 6
                    && strncasecmp(parts[0], "inline", 6) == 0)) {
                container->flags |= SCROLL_AXIS_X;
            } else if ((lengths[0] == 1
                        && tolower((unsigned char) parts[0][0]) == 'y')
                       || (lengths[0] == 5
                           && strncasecmp(parts[0], "block", 5) == 0)) {
                container->flags |= SCROLL_AXIS_Y;
            } else if (lengths[0] == 4
                       && strncasecmp(parts[0], "both", 4) == 0) {
                container->flags |= SCROLL_AXIS_X | SCROLL_AXIS_Y;
            }
            if (count > 1 && lengths[1] == 9
                && strncasecmp(parts[1], "mandatory", 9) == 0) {
                container->flags |= SCROLL_MANDATORY;
            }
        }
    }
    if (scroll_retained(
            sheet, box->node, "scroll-behavior", value, sizeof(value))
        && strcasecmp(value, "smooth") == 0) {
        container->flags |= SCROLL_SMOOTH;
    }
    unsigned overscroll_x = 0, overscroll_y = 0;
    StyleRetainedPairValues overscroll = {0};
    if (style_retained_pair_values(
            sheet, box->node, "overscroll-behavior",
            "overscroll-behavior-x", "overscroll-behavior-y",
            &overscroll)) {
        if (overscroll.present_mask & 1u) {
            overscroll_x = overscroll_code(
                overscroll.values[0], strlen(overscroll.values[0]));
        }
        if (overscroll.present_mask & 2u) {
            overscroll_y = overscroll_code(
                overscroll.values[1], strlen(overscroll.values[1]));
        }
    }
    container->flags |= (uint16_t) (overscroll_x
                                    << SCROLL_OVERSCROLL_X_SHIFT);
    container->flags |= (uint16_t) (overscroll_y
                                    << SCROLL_OVERSCROLL_Y_SHIFT);

    unsigned scrollbar = LAYOUT_SCROLLBAR_AUTO;
    if (scroll_retained(
            sheet, box->node, "scrollbar-width", value, sizeof(value))) {
        if (strcasecmp(value, "thin") == 0) {
            scrollbar = LAYOUT_SCROLLBAR_THIN;
        } else if (strcasecmp(value, "none") == 0) {
            scrollbar = LAYOUT_SCROLLBAR_NONE;
        }
    }
    container->flags |= (uint16_t) (scrollbar << SCROLL_SCROLLBAR_SHIFT);
    if (scroll_retained(
            sheet, box->node, "scrollbar-color", value, sizeof(value))
        && strcasecmp(value, "auto") != 0) {
        const char *parts[2] = {0};
        size_t lengths[2] = {0};
        if (scroll_tokens(value, parts, lengths, 2) == 2) {
            uint32_t color = 0;
            uint8_t alpha = 0;
            if (style_color_parse(
                    parts[0], lengths[0], &color, &alpha)
                && alpha >= 128) {
                container->thumb_rgb565 = rgb565_from_rgb888(color);
            }
            if (style_color_parse(
                    parts[1], lengths[1], &color, &alpha)
                && alpha >= 128) {
                container->track_rgb565 = rgb565_from_rgb888(color);
            }
        }
    }
    int edges[4] = {0};
    StyleRetainedBoxValues box_values;
    if (style_retained_box_values(
            sheet, box->node, "scroll-padding", &box_values)) {
        for (size_t side = 0; side < 4; side++) {
            if ((box_values.present_mask & (1u << side)) == 0) continue;
            edges[side] = scroll_length(
                sheet, box_values.values[side],
                strlen(box_values.values[side]),
                box->client_width, true);
        }
    }
    container->padding_top = scroll_clamp_i16(edges[0]);
    container->padding_right = scroll_clamp_i16(edges[1]);
    container->padding_bottom = scroll_clamp_i16(edges[2]);
    container->padding_left = scroll_clamp_i16(edges[3]);
}

static int scroll_container_index_for_box(const LayoutDocument *layout,
                                          size_t box_index)
{
    for (size_t i = 0; i < layout->scroll_container_count; i++) {
        if (layout->scroll_containers[i].node_box_index == box_index) {
            return (int) i;
        }
    }
    return -1;
}

static size_t scroll_box_index(const LayoutDocument *layout,
                               lxb_dom_node_t *node)
{
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    return box == NULL ? SIZE_MAX : (size_t) (box - layout->node_boxes);
}

static int scroll_container_for_node(const LayoutDocument *layout,
                                     lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        size_t box_index = scroll_box_index(layout, at);
        if (box_index == SIZE_MAX) continue;
        int index = scroll_container_index_for_box(layout, box_index);
        if (index >= 0) return index;
    }
    return -1;
}

static uint8_t snap_alignment(const char *value, size_t length)
{
    if (length == 5 && strncasecmp(value, "start", 5) == 0) return 1;
    if (length == 6 && strncasecmp(value, "center", 6) == 0) return 2;
    if (length == 3 && strncasecmp(value, "end", 3) == 0) return 3;
    return 0;
}

static bool scroll_add_candidate(LayoutDocument *layout,
                                 const Stylesheet *sheet,
                                 size_t box_index)
{
    const LayoutNodeBox *box = &layout->node_boxes[box_index];
    char value[96];
    if (!scroll_retained(
            sheet, box->node, "scroll-snap-align",
            value, sizeof(value))) return true;
    const char *parts[2] = {0};
    size_t lengths[2] = {0};
    unsigned count = scroll_tokens(value, parts, lengths, 2);
    uint8_t block = count == 0 ? 0 : snap_alignment(parts[0], lengths[0]);
    uint8_t inline_axis = count < 2
        ? block : snap_alignment(parts[1], lengths[1]);
    if (block == 0 && inline_axis == 0) return true;
    int container_index = scroll_container_for_node(
        layout, box->node == NULL ? NULL : box->node->parent);
    if (container_index < 0) return true;
    const LayoutScrollContainer *container =
        &layout->scroll_containers[container_index];
    if ((container->flags & (SCROLL_AXIS_X | SCROLL_AXIS_Y)) == 0) {
        return true;
    }
    if (layout->scroll_snap_candidate_count
        == LAYOUT_SCROLL_SNAP_CANDIDATE_LIMIT) {
        layout->scroll_metadata_truncated = true;
        return true;
    }
    if (layout->scroll_snap_candidates == NULL) {
        layout->scroll_snap_candidates = budget_calloc(
            layout->budget, LAYOUT_SCROLL_SNAP_CANDIDATE_LIMIT,
            sizeof(*layout->scroll_snap_candidates));
        if (layout->scroll_snap_candidates == NULL) {
            layout->scroll_metadata_truncated = true;
            return true;
        }
    }
    LayoutScrollSnapCandidate *candidate =
        &layout->scroll_snap_candidates[
            layout->scroll_snap_candidate_count++];
    candidate->node_box_index = (uint32_t) box_index;
    candidate->container_index = (uint16_t) container_index;
    candidate->align_x = inline_axis;
    candidate->align_y = block;
    if (scroll_retained(
            sheet, box->node, "scroll-snap-stop", value, sizeof(value))
        && strcasecmp(value, "always") == 0) {
        candidate->stop_always = 1;
    }
    int edges[4] = {0};
    StyleRetainedBoxValues box_values;
    if (style_retained_box_values(
            sheet, box->node, "scroll-margin", &box_values)) {
        for (size_t side = 0; side < 4; side++) {
            if ((box_values.present_mask & (1u << side)) == 0) continue;
            edges[side] = scroll_length(
                sheet, box_values.values[side],
                strlen(box_values.values[side]), box->width, false);
        }
    }
    candidate->margin_top = scroll_clamp_i16(edges[0]);
    candidate->margin_right = scroll_clamp_i16(edges[1]);
    candidate->margin_bottom = scroll_clamp_i16(edges[2]);
    candidate->margin_left = scroll_clamp_i16(edges[3]);
    return true;
}

bool layout_build_scroll_metadata(LayoutDocument *layout,
                                  const Stylesheet *stylesheet)
{
    if (layout == NULL || stylesheet == NULL
        || layout->node_box_count == 0) return true;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        bool scrollable_x = box->clips_x && !box->clip_only_x
            && box->content_width > box->client_width;
        bool scrollable_y = box->clips_y && !box->clip_only_y
            && box->content_height > box->client_height;
        if (!scrollable_x && !scrollable_y) continue;
        if ((layout->scroll_container_count & 15u) == 0
            && !tilefinch_platform_cooperate(
                "layout-scroll-style",
                layout->scroll_container_count)) return false;
        if (layout->scroll_containers == NULL) {
            layout->scroll_containers = budget_calloc(
                layout->budget, LAYOUT_SCROLL_CONTAINER_LIMIT,
                sizeof(*layout->scroll_containers));
            if (layout->scroll_containers == NULL) {
                layout->scroll_metadata_truncated = true;
                return true;
            }
        }
        if (layout->scroll_container_count
            == LAYOUT_SCROLL_CONTAINER_LIMIT) {
            layout->scroll_metadata_truncated = true;
            break;
        }
        LayoutScrollContainer *container =
            &layout->scroll_containers[layout->scroll_container_count++];
        container->node_box_index = (uint32_t) i;
        if (stylesheet->has_scroll_interaction_rules) {
            scroll_container_style(stylesheet, box, container);
        }
    }
    bool snap = false;
    for (size_t i = 0; i < layout->scroll_container_count; i++) {
        if ((layout->scroll_containers[i].flags
             & (SCROLL_AXIS_X | SCROLL_AXIS_Y)) != 0) {
            snap = true;
            break;
        }
    }
    if (snap) {
        for (size_t i = 0; i < layout->node_box_count; i++) {
            if ((i & 31u) == 0
                && !tilefinch_platform_cooperate(
                    "layout-scroll-snap", i)) {
                return false;
            }
            if (!scroll_add_candidate(layout, stylesheet, i)) return false;
        }
    }
    return true;
}

static int scroll_apply_axis(int current, int maximum, int delta,
                             int *remaining)
{
    int64_t requested = (int64_t) current + delta;
    int next = requested < 0 ? 0
        : (requested > maximum ? maximum : (int) requested);
    if (remaining != NULL) {
        int64_t rest = requested - next;
        *remaining = rest < INT_MIN ? INT_MIN
            : (rest > INT_MAX ? INT_MAX : (int) rest);
    }
    return next;
}

bool layout_scroll_node_chain(LayoutDocument *layout, lxb_dom_node_t *node,
                              int delta_x, int delta_y,
                              int *remaining_x, int *remaining_y)
{
    if (remaining_x != NULL) *remaining_x = delta_x;
    if (remaining_y != NULL) *remaining_y = delta_y;
    if (layout == NULL || node == NULL) return false;
    int rest_x = delta_x, rest_y = delta_y;
    bool changed = false;
    for (lxb_dom_node_t *at = node; at != NULL
         && (rest_x != 0 || rest_y != 0); at = at->parent) {
        size_t box_index = scroll_box_index(layout, at);
        int container_index = box_index == SIZE_MAX ? -1
            : scroll_container_index_for_box(layout, box_index);
        if (container_index < 0) continue;
        LayoutScrollContainer *container =
            &layout->scroll_containers[container_index];
        LayoutNodeBox *box = &layout->node_boxes[box_index];
        int next_rest_x = rest_x, next_rest_y = rest_y;
        int maximum_x = box->content_width - box->client_width;
        int maximum_y = box->content_height - box->client_height;
        int next_x = scroll_apply_axis(
            box->scroll_x, maximum_x < 0 ? 0 : maximum_x,
            rest_x, &next_rest_x);
        int next_y = scroll_apply_axis(
            box->scroll_y, maximum_y < 0 ? 0 : maximum_y,
            rest_y, &next_rest_y);
        changed |= next_x != box->scroll_x || next_y != box->scroll_y;
        box->scroll_x = next_x;
        box->scroll_y = next_y;
        unsigned policy_x =
            (container->flags >> SCROLL_OVERSCROLL_X_SHIFT) & 3u;
        unsigned policy_y =
            (container->flags >> SCROLL_OVERSCROLL_Y_SHIFT) & 3u;
        rest_x = policy_x == 0 ? next_rest_x : 0;
        rest_y = policy_y == 0 ? next_rest_y : 0;
    }
    if (remaining_x != NULL) *remaining_x = rest_x;
    if (remaining_y != NULL) *remaining_y = rest_y;
    return changed;
}

static int snap_target_axis(int position, int size, int viewport,
                            int leading_padding, int trailing_padding,
                            int leading_margin, int trailing_margin,
                            uint8_t alignment)
{
    if (alignment == 1) return position - leading_margin - leading_padding;
    if (alignment == 2) {
        int available = viewport - leading_padding - trailing_padding;
        return position + size / 2
            - (leading_padding + (available > 0 ? available : viewport) / 2);
    }
    if (alignment == 3) {
        return position + size + trailing_margin
            - viewport + trailing_padding;
    }
    return 0;
}

bool layout_scroll_node_settle(LayoutDocument *layout, lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return false;
    int index = scroll_container_for_node(layout, node);
    if (index < 0) return false;
    LayoutScrollContainer *container = &layout->scroll_containers[index];
    LayoutNodeBox *container_box =
        &layout->node_boxes[container->node_box_index];
    int best_x = container_box->scroll_x;
    int best_y = container_box->scroll_y;
    int distance_x = INT_MAX, distance_y = INT_MAX;
    for (size_t i = 0; i < layout->scroll_snap_candidate_count; i++) {
        const LayoutScrollSnapCandidate *candidate =
            &layout->scroll_snap_candidates[i];
        if (candidate->container_index != (uint16_t) index
            || candidate->node_box_index >= layout->node_box_count) continue;
        const LayoutNodeBox *box =
            &layout->node_boxes[candidate->node_box_index];
        if ((container->flags & SCROLL_AXIS_X) && candidate->align_x != 0) {
            int target = snap_target_axis(
                box->x - container_box->x, box->width,
                container_box->client_width,
                container->padding_left, container->padding_right,
                candidate->margin_left, candidate->margin_right,
                candidate->align_x);
            int distance = target - container_box->scroll_x;
            if (distance < 0) distance = -distance;
            if (distance < distance_x) {
                distance_x = distance;
                best_x = target;
            }
        }
        if ((container->flags & SCROLL_AXIS_Y) && candidate->align_y != 0) {
            int target = snap_target_axis(
                box->y - container_box->y, box->height,
                container_box->client_height,
                container->padding_top, container->padding_bottom,
                candidate->margin_top, candidate->margin_bottom,
                candidate->align_y);
            int distance = target - container_box->scroll_y;
            if (distance < 0) distance = -distance;
            if (distance < distance_y) {
                distance_y = distance;
                best_y = target;
            }
        }
    }
    if ((container->flags & SCROLL_MANDATORY) == 0) {
        if (distance_x > container_box->client_width / 2) {
            best_x = container_box->scroll_x;
        }
        if (distance_y > container_box->client_height / 2) {
            best_y = container_box->scroll_y;
        }
    }
    int old_x = container_box->scroll_x, old_y = container_box->scroll_y;
    (void) layout_scroll_node(
        layout, container_box->node, best_x, best_y);
    return old_x != container_box->scroll_x
        || old_y != container_box->scroll_y;
}

bool layout_scroll_node_reveal(LayoutDocument *layout,
                               lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return false;
    const LayoutNodeBox *target = layout_box_for_node(layout, node);
    if (target == NULL) return false;
    bool changed = false;
    /* Inner containers must move first. The retained boxes are authored
       geometry, while scroll_x/y is a presentation offset, so the same
       target coordinates remain valid as each ancestor is adjusted. */
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        size_t box_index = scroll_box_index(layout, at);
        int container_index = box_index == SIZE_MAX ? -1
            : scroll_container_index_for_box(layout, box_index);
        if (container_index < 0) continue;
        LayoutScrollContainer *container =
            &layout->scroll_containers[container_index];
        LayoutNodeBox *box = &layout->node_boxes[box_index];
        int margin_top = 0, margin_right = 0;
        int margin_bottom = 0, margin_left = 0;
        for (size_t i = 0; i < layout->scroll_snap_candidate_count; i++) {
            const LayoutScrollSnapCandidate *candidate =
                &layout->scroll_snap_candidates[i];
            if (candidate->container_index == (uint16_t) container_index
                && candidate->node_box_index
                   == (uint32_t) (target - layout->node_boxes)) {
                margin_top = candidate->margin_top;
                margin_right = candidate->margin_right;
                margin_bottom = candidate->margin_bottom;
                margin_left = candidate->margin_left;
                break;
            }
        }
        int x = box->scroll_x, y = box->scroll_y;
        int target_left = target->x - box->x - margin_left;
        int target_right = target->x - box->x + target->width + margin_right;
        int target_top = target->y - box->y - margin_top;
        int target_bottom =
            target->y - box->y + target->height + margin_bottom;
        int visible_left = x + container->padding_left;
        int visible_right = x + box->client_width
            - container->padding_right;
        int visible_top = y + container->padding_top;
        int visible_bottom = y + box->client_height
            - container->padding_bottom;
        if (target_left < visible_left) {
            x = target_left - container->padding_left;
        } else if (target_right > visible_right) {
            x = target_right - box->client_width
                + container->padding_right;
        }
        if (target_top < visible_top) {
            y = target_top - container->padding_top;
        } else if (target_bottom > visible_bottom) {
            y = target_bottom - box->client_height
                + container->padding_bottom;
        }
        int old_x = box->scroll_x, old_y = box->scroll_y;
        (void) layout_scroll_node(layout, box->node, x, y);
        changed |= box->scroll_x != old_x || box->scroll_y != old_y;
    }
    return changed;
}

LayoutCursor layout_cursor_for_node(const Stylesheet *stylesheet,
                                    lxb_dom_node_t *node)
{
    if (stylesheet == NULL || !stylesheet->has_cursor_rules) {
        return LAYOUT_CURSOR_AUTO;
    }
    char value[96];
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (!scroll_retained(
                stylesheet, at, "cursor", value, sizeof(value))) continue;
        if (strcasecmp(value, "none") == 0) return LAYOUT_CURSOR_HIDDEN;
        if (strcasecmp(value, "pointer") == 0
            || strcasecmp(value, "grab") == 0
            || strcasecmp(value, "grabbing") == 0) {
            return LAYOUT_CURSOR_POINTER;
        }
        if (strcasecmp(value, "text") == 0
            || strcasecmp(value, "vertical-text") == 0) {
            return LAYOUT_CURSOR_TEXT;
        }
        if (strcasecmp(value, "crosshair") == 0
            || strcasecmp(value, "cell") == 0) {
            return LAYOUT_CURSOR_CROSSHAIR;
        }
        if (strcasecmp(value, "move") == 0
            || strcasecmp(value, "all-scroll") == 0) {
            return LAYOUT_CURSOR_MOVE;
        }
        if (strcasecmp(value, "wait") == 0
            || strcasecmp(value, "progress") == 0) {
            return LAYOUT_CURSOR_WAIT;
        }
        if (strcasecmp(value, "not-allowed") == 0
            || strcasecmp(value, "no-drop") == 0) {
            return LAYOUT_CURSOR_NOT_ALLOWED;
        }
        if (strcasecmp(value, "ew-resize") == 0
            || strcasecmp(value, "col-resize") == 0
            || strcasecmp(value, "e-resize") == 0
            || strcasecmp(value, "w-resize") == 0) {
            return LAYOUT_CURSOR_RESIZE_HORIZONTAL;
        }
        if (strcasecmp(value, "ns-resize") == 0
            || strcasecmp(value, "row-resize") == 0
            || strcasecmp(value, "n-resize") == 0
            || strcasecmp(value, "s-resize") == 0) {
            return LAYOUT_CURSOR_RESIZE_VERTICAL;
        }
        return LAYOUT_CURSOR_AUTO;
    }
    return LAYOUT_CURSOR_AUTO;
}

LayoutScrollbarWidth layout_root_scrollbar_width(
    const LayoutDocument *layout, const Stylesheet *stylesheet)
{
    if (layout == NULL || stylesheet == NULL) return LAYOUT_SCROLLBAR_AUTO;
    char value[96];
    /* Root scrolling is frontend-owned, so html/body may not be represented
       by a local scroll container. CSS Scrollbars still propagates the root
       element's value to the viewport. */
    for (unsigned pass = 0; pass < 2; pass++) {
        const char *wanted = pass == 0 ? "html" : "body";
        lxb_dom_node_t *root = NULL;
        for (size_t i = 0; i < layout->node_box_count; i++) {
            lxb_dom_node_t *node = layout->node_boxes[i].node;
            size_t name_length = 0;
            const char *name = node == NULL ? NULL
                : document_element_name(node, &name_length);
            if (name != NULL && name_length == 4
                && strncasecmp(name, wanted, 4) == 0) {
                root = node;
                break;
            }
        }
        if (root == NULL
            || !scroll_retained(
                stylesheet, root, "scrollbar-width",
                value, sizeof(value))) continue;
        if (strcasecmp(value, "none") == 0) return LAYOUT_SCROLLBAR_NONE;
        if (strcasecmp(value, "thin") == 0) return LAYOUT_SCROLLBAR_THIN;
        return LAYOUT_SCROLLBAR_AUTO;
    }
    return LAYOUT_SCROLLBAR_AUTO;
}
