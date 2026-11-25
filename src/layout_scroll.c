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
            sheet, box->node, "scroll-padding", NULL, &box_values)) {
        for (size_t side = 0; side < 4; side++) {
            if ((box_values.present_mask & (1u << side)) == 0) continue;
            int reference = side == 0 || side == 2
                ? box->client_height : box->client_width;
            edges[side] = scroll_length(
                sheet, box_values.values[side],
                strlen(box_values.values[side]),
                reference, true);
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
            sheet, box->node, "scroll-margin", NULL, &box_values)) {
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
    for (size_t i = 0; i < layout->sticky_count; i++) {
        StickyRange *range = &layout->sticky_ranges[i];
        const lxb_dom_node_t *node = range->node;
        for (unsigned depth = 0; node != NULL && depth < 256u; depth++) {
            node = node->parent;
            const LayoutNodeBox *box = layout_box_for_node(layout, node);
            if (box != NULL && box->clips_y && !box->clip_only_y
                && !document_node_is_tag(node, LXB_TAG_HTML)
                && !document_node_is_tag(node, LXB_TAG_BODY)) {
                range->scrollport_box_plus_one =
                    (size_t) (box - layout->node_boxes) + 1u;
                break;
            }
        }
    }
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

int layout_sticky_offset(const LayoutDocument *layout, const StickyRange *range,
                         int viewport_scroll, int viewport_height)
{
    int leading = viewport_scroll, extent = viewport_height;
    if (range->scrollport_box_plus_one != 0
        && range->scrollport_box_plus_one <= layout->node_box_count) {
        const LayoutNodeBox *port = &layout->node_boxes[
            range->scrollport_box_plus_one - 1u];
        leading = layout_add_coordinate(
            layout_add_coordinate(port->y, port->clip_inset_top), port->scroll_y);
        extent = port->client_height;
    }
    int offset = 0;
    if (range->bottom_inset != INT_MAX) {
        int limit = layout_subtract_coordinate(
            layout_add_coordinate(leading, extent), range->bottom_inset);
        int up = layout_subtract_coordinate(limit, range->bottom_y);
        if (up < offset) offset = up;
    }
    if (range->top != INT_MAX) {
        int limit = layout_add_coordinate(leading, range->top);
        int down = layout_subtract_coordinate(limit, range->origin_y);
        if (down > offset) offset = down;
    }
    if (offset > range->maximum_offset) return range->maximum_offset;
    if (offset < range->minimum_offset) return range->minimum_offset;
    return offset;
}

int layout_node_sticky_offset(const LayoutDocument *layout,
                              const lxb_dom_node_t *node,
                              int viewport_scroll, int viewport_height)
{
    if (layout == NULL || layout->sticky_count == 0 || node == NULL) return 0;
    int offset = 0;
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    if (box != NULL && box->positioned_ancestor_distance == UINT8_MAX)
        return 0; /* Escapes the ancestor scroll/sticky coordinate space. */
    for (size_t i = 0; i < layout->sticky_count; i++) {
        const StickyRange *range = &layout->sticky_ranges[i];
        if (node != range->node && box != NULL
            && (box->command_start < range->command_start
                || box->command_end > range->command_end)) continue;
        const lxb_dom_node_t *at = node;
        for (unsigned depth = 0; at != NULL && depth < 256u;
             depth++, at = at->parent) {
            if (at != range->node) continue;
            int candidate = layout_sticky_offset(
                layout, range, viewport_scroll, viewport_height);
            if (candidate > offset || (offset <= 0 && candidate < offset))
                offset = candidate;
            break;
        }
    }
    return offset;
}

static int scroll_apply_axis(int current, int minimum, int maximum, int delta,
                             int *remaining)
{
    int64_t requested = (int64_t) current + delta;
    int next = requested < minimum ? minimum
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
        if (maximum_x < 0) maximum_x = 0;
        int next_x = scroll_apply_axis(
            box->scroll_x, box->scroll_rtl ? -maximum_x : 0,
            box->scroll_rtl ? 0 : maximum_x,
            rest_x, &next_rest_x);
        int next_y = scroll_apply_axis(
            box->scroll_y, 0, maximum_y < 0 ? 0 : maximum_y,
            rest_y, &next_rest_y);
        if (next_x != box->scroll_x || next_y != box->scroll_y) {
            layout->scroll_generation++;
            layout_note_scroll_travel(layout, next_x - box->scroll_x,
                                      next_y - box->scroll_y);
        }
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

/* Unpinning a tall pinned stack.

   A small screen leaves little room under a page's fixed header: a stack
   of fixed bars (header, search bar, toolbar) sized for a phone can cover
   most of 272 pixels and leave a sliver to read through. When the
   position:fixed bars pinned to the viewport edges cover more than half of
   it, the stack is laid out as though position:absolute against the
   initial containing block instead: each bar keeps the place it has at the
   top of the document (or at the bottom of the first screen) and scrolls
   away with the page, and page steps are no longer shortened by it.

   Only viewport-fixed ranges count; sticky bars stick only inside their
   container and leave with it. A bar is a range that spans at least half
   the viewport width and is no taller than two thirds of the viewport
   (taller ones are dialogs, menus or app shells, left alone), not bound to
   a scrolling scene or a clip-path ancestor. A top stack starts with a bar
   in the top quarter and grows by bars touching what it covers; the bottom
   stack is its mirror image, as for page steps (controller.c). The top
   stack is unpinned when both stacks together cover more than half the
   viewport height; the bottom stack only when it alone does, since an
   unpinned footer lands over the first screen's content. Once a stack was
   unpinned on this page, it stays unpinned until it covers 40% or less, so
   a page whose bars change size a little between relayouts does not flip.
   The decision is made once per layout build, before any index is built
   over the commands, so frames pay nothing for it. */
enum {
    UNPIN_TOP = 1u,
    UNPIN_BOTTOM = 2u,
    UNPIN_SPAN_SCAN = 4096u
};

static bool unpin_range_spans_width(const LayoutDocument *layout,
                                    const FixedRange *range, int width)
{
    size_t end = range->command_end < layout->count
        ? range->command_end : layout->count;
    if (width <= 0 || end <= range->command_start) return false;
    if (end - range->command_start > UNPIN_SPAN_SCAN) return true;
    int left = INT_MAX, right = INT_MIN;
    for (size_t i = range->command_start; i < end; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->width <= 0 || command->height <= 0) continue;
        if (command->x < left) left = command->x;
        int command_right = layout_add_coordinate(command->x, command->width);
        if (command_right > right) right = command_right;
    }
    if (left < 0) left = 0;
    if (right > width) right = width;
    return right > left && right - left >= width / 2;
}

static bool unpin_range_nested(const LayoutDocument *layout, size_t index)
{
    const FixedRange *inner = &layout->fixed_ranges[index];
    for (size_t i = 0; i < layout->fixed_count; i++) {
        const FixedRange *outer = &layout->fixed_ranges[i];
        if (i != index && outer->command_start <= inner->command_start
            && inner->command_end <= outer->command_end
            && (outer->command_end - outer->command_start
                > inner->command_end - inner->command_start || i < index))
            return true;
    }
    return false;
}

/* Moves one fixed range's paint, regions, boxes and fragments by dy: what
   layout_translate_range does for a range that ends the layout. */
static void unpin_translate(LayoutDocument *layout, const FixedRange *range,
                            int dy)
{
    size_t end = range->command_end < layout->count
        ? range->command_end : layout->count;
    for (size_t i = range->command_start; i < end; i++)
        layout->commands[i].y = layout_add_coordinate(
            layout->commands[i].y, dy);
    for (size_t i = range->link_start;
         i < range->link_end && i < layout->link_count; i++)
        layout->links[i].y = layout_add_coordinate(layout->links[i].y, dy);
    for (size_t i = range->control_start;
         i < range->control_end && i < layout->control_count; i++)
        layout->controls[i].y = layout_add_coordinate(
            layout->controls[i].y, dy);
    for (size_t i = 0; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        if (box->command_start >= range->command_start
            && box->command_start < end && box->command_end <= end) {
            box->y = layout_add_coordinate(box->y, dy);
            /* Exact for CSSOM, as it was while the range was fixed. */
            box->cssom_geometry_authoritative = 1;
        }
    }
    LayoutInlineFragments *fragments = layout->inline_fragments;
    for (size_t i = 0; fragments != NULL && i < fragments->count; i++) {
        LayoutInlineFragment *fragment = &fragments->entries[i];
        if (fragment->command >= range->command_start
            && fragment->command < end)
            fragment->y = layout_add_coordinate(fragment->y, dy);
    }
    for (size_t i = 0; i < layout->sticky_count; i++) {
        StickyRange *sticky = &layout->sticky_ranges[i];
        if (sticky->command_start < range->command_start
            || sticky->command_end > end) continue;
        sticky->origin_y = layout_add_coordinate(sticky->origin_y, dy);
        sticky->bottom_y = layout_add_coordinate(sticky->bottom_y, dy);
    }
}

void layout_unpin_tall_fixed_stacks(LayoutDocument *layout,
                                    int viewport_height, uint8_t *state)
{
    uint8_t previous = state == NULL ? 0u : *state;
    if (state != NULL) *state = 0u;
    if (layout == NULL || layout->fixed_count == 0 || viewport_height <= 0)
        return;
    size_t count = layout->fixed_count;
    int tops[LAYOUT_FIXED_RANGE_LIMIT], bottoms[LAYOUT_FIXED_RANGE_LIMIT];
    uint8_t edge[LAYOUT_FIXED_RANGE_LIMIT] = {0};
    bool bar[LAYOUT_FIXED_RANGE_LIMIT] = {0};
    for (size_t i = 0; i < count && i < LAYOUT_FIXED_RANGE_LIMIT; i++) {
        const FixedRange *range = &layout->fixed_ranges[i];
        tops[i] = range->from_bottom
            ? viewport_height - range->inset - range->height : range->inset;
        bottoms[i] = layout_add_coordinate(tops[i], range->height);
        bar[i] = range->height > 0
            && range->height <= viewport_height - viewport_height / 3
            && range->scroll_end == INT_MAX && range->clip_node == NULL
            && bottoms[i] > 0 && tops[i] < viewport_height
            && !unpin_range_nested(layout, i)
            && unpin_range_spans_width(layout, range, layout->width);
    }
    /* Each pass adds at least one bar or stops. */
    int covered_top = 0, covered_from = viewport_height;
    for (size_t pass = 0; pass < count; pass++) {
        bool grew = false;
        for (size_t i = 0; i < count; i++) {
            if (!bar[i] || (edge[i] & UNPIN_TOP) != 0) continue;
            if ((tops[i] <= viewport_height / 4 || tops[i] <= covered_top)
                && bottoms[i] < viewport_height) {
                edge[i] |= UNPIN_TOP;
                if (bottoms[i] > covered_top) covered_top = bottoms[i];
                grew = true;
            }
        }
        if (!grew) break;
    }
    for (size_t pass = 0; pass < count; pass++) {
        bool grew = false;
        for (size_t i = 0; i < count; i++) {
            if (!bar[i] || edge[i] != 0) continue;
            if ((bottoms[i] >= viewport_height - viewport_height / 4
                 || bottoms[i] >= covered_from)
                && tops[i] > 0 && tops[i] >= covered_top) {
                edge[i] |= UNPIN_BOTTOM;
                if (tops[i] < covered_from) covered_from = tops[i];
                grew = true;
            }
        }
        if (!grew) break;
    }
    int covered_bottom = viewport_height - covered_from;
    /* More than half, or more than 40% for a stack this page unpinned. */
    int top_limit = (previous & UNPIN_TOP) != 0
        ? viewport_height * 2 / 5 : viewport_height / 2;
    int bottom_limit = (previous & UNPIN_BOTTOM) != 0
        ? viewport_height * 2 / 5 : viewport_height / 2;
    uint8_t unpin = 0u;
    if (covered_top > 0 && covered_top + covered_bottom > top_limit)
        unpin |= UNPIN_TOP;
    if (covered_bottom > bottom_limit) unpin |= UNPIN_BOTTOM;
    if (state != NULL) *state = unpin;
    if (unpin == 0u) return;
    for (size_t i = 0; i < count; i++) {
        if ((edge[i] & unpin) == 0) continue;
        unpin_translate(layout, &layout->fixed_ranges[i],
                        layout_subtract_coordinate(
                            tops[i], layout->fixed_ranges[i].origin_y));
    }
    /* Drop the unpinned ranges and those nested in them: their commands
       are ordinary document paint now. */
    bool dropped[LAYOUT_FIXED_RANGE_LIMIT] = {0};
    for (size_t i = 0; i < count; i++) {
        const FixedRange *range = &layout->fixed_ranges[i];
        dropped[i] = (edge[i] & unpin) != 0;
        for (size_t j = 0; !dropped[i] && j < count; j++) {
            const FixedRange *outer = &layout->fixed_ranges[j];
            dropped[i] = j != i && (edge[j] & unpin) != 0
                && outer->command_start <= range->command_start
                && range->command_end <= outer->command_end;
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (!dropped[i]) layout->fixed_ranges[kept++] = layout->fixed_ranges[i];
    }
    layout->fixed_count = kept;
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

/* A box whose scroll offset can move its content: it clips an axis that
   overflows. layout_scroll_node clamps every other box to zero. */
static bool scroll_box_scrollable(const LayoutNodeBox *box)
{
    return (box->clips_x && !box->clip_only_x
            && box->content_width > box->client_width)
        || (box->clips_y && !box->clip_only_y
            && box->content_height > box->client_height);
}

/* How many ancestors above a positioned box lie below its containing block.
   Paint leaves those clips (and so their scroll offsets) to the escaping
   content (layout_positioned_command_escapes_clip). */
static unsigned scroll_escape_span(const LayoutNodeBox *box)
{
    if (box == NULL || box->positioned_ancestor_distance == 0) return 0;
    if (box->positioned_ancestor_distance == UINT8_MAX) return UINT_MAX;
    return box->positioned_ancestor_distance - 1u;
}

enum { SCROLL_ANCESTOR_LIMIT = 256 };

bool layout_scroll_rect_reveal(LayoutDocument *layout, lxb_dom_node_t *node,
                               int x, int y, int width, int height)
{
    if (layout == NULL || node == NULL) return false;
    const LayoutNodeBox *target = layout_box_for_node(layout, node);
    unsigned skip = scroll_escape_span(target);
    bool changed = false;
    /* Inner containers move first, as scrollIntoView({block:"nearest"})
       does. The rect is authored geometry while scroll_x/y is a
       presentation offset, so each outer container sees the target where
       the containers inside it (already adjusted) now present it. */
    int inner_dx = 0, inner_dy = 0;
    unsigned steps = 0;
    for (lxb_dom_node_t *at = node->parent;
         at != NULL && steps < SCROLL_ANCESTOR_LIMIT;
         at = at->parent, steps++) {
        size_t box_index = scroll_box_index(layout, at);
        LayoutNodeBox *box = box_index == SIZE_MAX
            ? NULL : &layout->node_boxes[box_index];
        bool escaped = skip != 0;
        if (escaped) skip--;
        unsigned span = scroll_escape_span(box);
        if (span > skip) skip = span;
        if (escaped || box == NULL) continue;
        int container_index =
            scroll_container_index_for_box(layout, box_index);
        if (container_index < 0) {
            /* Past the bounded container table: still presents the target
               where its own offset puts it. */
            inner_dx -= box->scroll_x;
            inner_dy -= box->scroll_y;
            continue;
        }
        LayoutScrollContainer *container =
            &layout->scroll_containers[container_index];
        int margin_top = 0, margin_right = 0;
        int margin_bottom = 0, margin_left = 0;
        for (size_t i = 0; target != NULL
                 && i < layout->scroll_snap_candidate_count; i++) {
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
        int sx = box->scroll_x, sy = box->scroll_y;
        int left = x + inner_dx - box->x;
        int top = y + inner_dy - box->y;
        int target_left = left - margin_left;
        int target_right = left + width + margin_right;
        int target_top = top - margin_top;
        int target_bottom = top + height + margin_bottom;
        int visible_left = sx + container->padding_left;
        int visible_right = sx + box->client_width
            - container->padding_right;
        int visible_top = sy + container->padding_top;
        int visible_bottom = sy + box->client_height
            - container->padding_bottom;
        if (target_left < visible_left) {
            sx = target_left - container->padding_left;
        } else if (target_right > visible_right) {
            sx = target_right - box->client_width
                + container->padding_right;
        }
        if (target_top < visible_top) {
            sy = target_top - container->padding_top;
        } else if (target_bottom > visible_bottom) {
            sy = target_bottom - box->client_height
                + container->padding_bottom;
        }
        int old_x = box->scroll_x, old_y = box->scroll_y;
        (void) layout_scroll_node(layout, box->node, sx, sy);
        changed |= box->scroll_x != old_x || box->scroll_y != old_y;
        inner_dx -= box->scroll_x;
        inner_dy -= box->scroll_y;
    }
    return changed;
}

bool layout_scroll_node_reveal(LayoutDocument *layout,
                               lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return false;
    const LayoutNodeBox *target = layout_box_for_node(layout, node);
    if (target == NULL) return false;
    return layout_scroll_rect_reveal(layout, node, target->x, target->y,
                                     target->width, target->height);
}

void layout_scroll_placement_prepare(const LayoutDocument *layout,
                                     LayoutScrollPlacementIndex *index)
{
    if (index == NULL) return;
    memset(index, 0, sizeof(*index));
    if (layout == NULL) return;
    index->layout = layout;
    index->node_box_count = layout->node_box_count;
    index->complete = true;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if (box->node == NULL || !scroll_box_scrollable(box)) continue;
        if (index->count == LAYOUT_SCROLL_PLACEMENT_INDEX_LIMIT) {
            index->complete = false;
            return;
        }
        index->containers[index->count++] = box->node;
    }
}

enum { SCROLL_PLACEMENT_CLIP_LIMIT = 16 };

static const LayoutNodeBox *scroll_layout_box(const void *layout,
                                              const lxb_dom_node_t *node)
{
    return layout_box_for_node(layout, node);
}

static bool scroll_placement_walk(LayoutBoxLookup lookup, const void *boxes,
                                  const lxb_dom_node_t *node,
                                  bool overflow_clips,
                                  const lxb_dom_node_t *root,
                                  const LayoutObserverMargin *margin,
                                  LayoutScrollPlacement *placement)
{
    struct {
        const LayoutNodeBox *box;
        int dx, dy; /* offsets through this container */
    } applied[SCROLL_PLACEMENT_CLIP_LIMIT];
    size_t applied_count = 0;
    bool enclosed = false;
    int dx = 0, dy = 0;
    unsigned skip = scroll_escape_span(lookup(boxes, node));
    unsigned steps = 0;
    bool below_root = true, in_root = root == NULL;
    for (const lxb_dom_node_t *at = node->parent;
         at != NULL && steps < SCROLL_ANCESTOR_LIMIT;
         at = at->parent, steps++) {
        const LayoutNodeBox *box = lookup(boxes, at);
        if (at == root) {
            in_root = skip == 0;
            below_root = false;
        }
        bool escaped = skip != 0;
        if (escaped) skip--;
        unsigned span = scroll_escape_span(box);
        if (span > skip) skip = span;
        if (escaped || box == NULL) continue;
        /* Only a scrollable box carries an offset (layout_scroll_node
           clamps the rest to zero), so overflow clips add clips only. */
        if (overflow_clips ? !box->clips_x && !box->clips_y
                           : !scroll_box_scrollable(box)) continue;
        enclosed = true;
        dx -= box->scroll_x;
        dy -= box->scroll_y;
        if (below_root && applied_count < SCROLL_PLACEMENT_CLIP_LIMIT) {
            applied[applied_count].box = box;
            applied[applied_count].dx = dx;
            applied[applied_count].dy = dy;
            applied_count++;
        }
    }
    placement->dx = dx;
    placement->dy = dy;
    for (size_t i = 0; i < applied_count; i++) {
        const LayoutNodeBox *box = applied[i].box;
        /* A container moves with the containers outside it: everything
           accumulated after its own offset. */
        int left, top, right, bottom;
        layout_node_box_clip_rect(
            box, box->x + (dx - applied[i].dx), box->y + (dy - applied[i].dy),
            &left, &top, &right, &bottom);
        if (margin != NULL && !box->clip_path_clip
            && ((box->clips_x && !box->clip_only_x)
                || (box->clips_y && !box->clip_only_y))) {
            int offsets[4];
            int width = right - left;
            for (unsigned side = 0; side < 4; side++) {
                int64_t v = margin->values[side];
                if ((margin->percent_mask & (1u << side)) != 0)
                    v = v * width / 100;
                offsets[side] = layout_clamp_coordinate(v / 64);
            }
            left = layout_subtract_coordinate(left, offsets[3]);
            top = layout_subtract_coordinate(top, offsets[0]);
            right = layout_add_coordinate(right, offsets[1]);
            bottom = layout_add_coordinate(bottom, offsets[2]);
        }
        if (box->clips_x) {
            placement->clipped_x = true;
            if (left > placement->clip_left) placement->clip_left = left;
            if (right < placement->clip_right) placement->clip_right = right;
        }
        if (box->clips_y) {
            placement->clipped_y = true;
            if (top > placement->clip_top) placement->clip_top = top;
            if (bottom < placement->clip_bottom)
                placement->clip_bottom = bottom;
        }
    }
    return in_root && (root != NULL || enclosed);
}

bool layout_node_scroll_placement(const LayoutDocument *layout,
                                  const LayoutScrollPlacementIndex *index,
                                  const lxb_dom_node_t *node,
                                  LayoutScrollPlacement *placement)
{
    if (placement == NULL) return false;
    *placement = (LayoutScrollPlacement) {
        .clip_left = INT_MIN / 2, .clip_top = INT_MIN / 2,
        .clip_right = INT_MAX / 2, .clip_bottom = INT_MAX / 2
    };
    if (layout == NULL || node == NULL) return false;
    if (index != NULL && index->layout == layout && index->complete
        && index->node_box_count == layout->node_box_count) {
        /* Most regions lie in no scroll container: prove that with
           pointer comparisons before any box lookup. */
        bool enclosed = false;
        unsigned steps = 0;
        for (const lxb_dom_node_t *at = node->parent;
             at != NULL && !enclosed && steps < SCROLL_ANCESTOR_LIMIT;
             at = at->parent, steps++) {
            for (size_t i = 0; i < index->count; i++) {
                if (index->containers[i] != at) continue;
                enclosed = true;
                break;
            }
        }
        if (!enclosed) return false;
    }
    return scroll_placement_walk(scroll_layout_box, layout, node, false, NULL, NULL,
                                 placement);
}

bool layout_node_overflow_placement(const LayoutDocument *layout,
                                    const lxb_dom_node_t *node,
                                    LayoutScrollPlacement *placement)
{
    if (placement == NULL) return false;
    *placement = (LayoutScrollPlacement) {
        .clip_left = INT_MIN / 2, .clip_top = INT_MIN / 2,
        .clip_right = INT_MAX / 2, .clip_bottom = INT_MAX / 2
    };
    if (layout == NULL || node == NULL) return false;
    return scroll_placement_walk(scroll_layout_box, layout, node, true, NULL, NULL,
                                 placement);
}

bool layout_node_observer_placement(const LayoutDocument *layout,
                                    const lxb_dom_node_t *node,
                                    const lxb_dom_node_t *root,
                                    const LayoutObserverMargin *margin,
                                    LayoutScrollPlacement *placement)
{
    *placement = (LayoutScrollPlacement) {
        .clip_left = INT_MIN / 2, .clip_top = INT_MIN / 2,
        .clip_right = INT_MAX / 2, .clip_bottom = INT_MAX / 2
    };
    return scroll_placement_walk(scroll_layout_box, layout, node, true, root, margin,
                                 placement);
}

bool layout_boxes_overflow_placement(LayoutBoxLookup lookup,
                                     const void *boxes,
                                     const lxb_dom_node_t *node,
                                     LayoutScrollPlacement *placement)
{
    if (placement == NULL) return false;
    *placement = (LayoutScrollPlacement) {
        .clip_left = INT_MIN / 2, .clip_top = INT_MIN / 2,
        .clip_right = INT_MAX / 2, .clip_bottom = INT_MAX / 2
    };
    if (lookup == NULL || node == NULL) return false;
    return scroll_placement_walk(lookup, boxes, node, true, NULL, NULL, placement);
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
