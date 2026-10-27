/* Flex and flat-item layout: order plans, item iteration, line metrics,
   distribution, and alignment. Split out of layout.c; intrinsic sizing
   lives in layout_intrinsic.c and grid placement in
   layout_grid_placement.c. */

#include "layout_internal.h"
#include "tilefinch/integer_math.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

AlignItems flex_item_alignment(const ComputedStyle *container,
                                      const ComputedStyle *item)
{
    if (item->align_self == ALIGN_SELF_START) return ALIGN_START;
    if (item->align_self == ALIGN_SELF_CENTER) return ALIGN_CENTER;
    if (item->align_self == ALIGN_SELF_END) return ALIGN_END;
    if (item->align_self == ALIGN_SELF_STRETCH) return ALIGN_STRETCH;
    if (item->align_self == ALIGN_SELF_BASELINE) return ALIGN_BASELINE;
    return container->align_items;
}

/* Expand an auto-sized row-flex item to its used cross size after the line's
   tallest item is known. The retained background/border geometry and the
   item's own full-box interaction region grow in place. Ordinary block
   descendants retain their positions; a nested row updates cross alignment. */
static void stretch_flex_item_cross(LayoutContext *context,
                                    const FlatItem *item,
                                    const ComputedStyle *style,
                                    int target_height)
{
    LayoutDocument *layout = context->layout;
    lxb_dom_node_t *node = item->node;
    LayoutNodeBox *box = item->generated_pseudo != PSEUDO_NONE
        ? item->generated_box : layout_box_for_node_mutable(layout, node);
    if (box == NULL || target_height < 0) return;
    /* A stretched cross size is definite, even when it equals the content
       height: a `size` container's queries read it, not its contained
       height. */
    if (item->generated_pseudo == PSEUDO_NONE)
        layout_note_contained_height(layout, node, -1);
    if (target_height == box->height) return;
    /* A column's flexible scroll viewport may shrink while its retained
       contents keep their natural extent. Ordinary cross-axis stretching
       still never shrinks non-scrollable content. */
    if (target_height < box->height
        && (!style->overflow_y_scroll || style->overflow_y_clip_only)) return;
    bool generated = item->generated_pseudo != PSEUDO_NONE;
    size_t box_index = generated ? 0 : (size_t) (box - layout->node_boxes);
    if (!generated && (layout->node_interaction_ranges == NULL
        || box_index >= layout->node_interaction_capacity)) return;
    const uint32_t *interactions =
        generated ? NULL : layout->node_interaction_ranges
        + box_index * LAYOUT_NODE_INTERACTION_STRIDE;
    int old_height = box->height;
    int delta = target_height - old_height;
    int old_bottom = box->y + old_height;
    size_t command_end = box->command_end < layout->count
                         ? box->command_end : layout->count;
    for (size_t i = box->command_start; i < command_end; i++) {
        if (i >= box->scroll_command_start
            && i < box->scroll_command_end) continue;
        DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT) {
            /* A blockified single-line input paints its value/placeholder
               centred in its provisional border box. Unlike normal block
               descendants or a multiline textarea, that anonymous text
               must follow the used cross size, including its shadows.
               Recompute the two centres rather than rounding delta/2 so
               odd sizes and initially undersized boxes keep exact offsets. */
            if (document_node_is_tag(node, LXB_TAG_INPUT)) {
                int offset = (target_height - command->height) / 2
                    - (old_height - command->height) / 2;
                command->y = layout_add_coordinate(command->y, offset);
            }
            continue;
        }
        if (style->border.bottom > 0
            && command->y == old_bottom - style->border.bottom
            && command->height == style->border.bottom) {
            command->y += delta;
        } else if (command->y == box->y
                   && command->height == old_height) {
            command->height = target_height;
        } else if (command->y == box->y + style->border.top
                   && command->height
                      == old_height - style->border.top
                                      - style->border.bottom) {
            command->height += delta;
        }
    }
    if (generated) {
        box->height = target_height;
        box->client_height += delta;
        return;
    }
    size_t link_start =
        interactions[LAYOUT_NODE_LINK_START] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_START] : layout->link_count;
    size_t link_end =
        interactions[LAYOUT_NODE_LINK_END] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_END] : layout->link_count;
    for (size_t i = link_start; i < link_end; i++) {
        LinkRegion *link = &layout->links[i];
        if (link->node == node) {
            link->x = box->x;
            link->y = box->y;
            link->width = box->width;
            link->height = target_height;
        }
    }
    size_t control_start =
        interactions[LAYOUT_NODE_CONTROL_START] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_START] : layout->control_count;
    size_t control_end =
        interactions[LAYOUT_NODE_CONTROL_END] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_END] : layout->control_count;
    for (size_t i = control_start; i < control_end; i++) {
        ControlRegion *control = &layout->controls[i];
        if (control->node == node) {
            control->y = box->y;
            control->height = target_height;
        }
    }
    box->height = target_height;
    box->client_height += delta;
    if (box->content_height < box->client_height) {
        box->content_height = box->client_height;
    }
    /* A nested auto-height row was aligned at its natural height before
       its parent stretched it. Its own center/end-aligned children must
       follow the new used cross size, not remain pinned to the top. Apply
       only the difference: replaying alignment would double the old offset.
       Wrapping rows require line distribution and stay on their existing
       path; this is the single-line cross-axis correction. */
    if ((style->display == DISPLAY_FLEX
         || style->display == DISPLAY_INLINE_FLEX)
        && (style->flex_direction == FLEX_ROW
            || style->flex_direction == FLEX_ROW_REVERSE)
        && !style->flex_wrap) {
        int edges = style->padding.top + style->padding.bottom
                    + style->border.top + style->border.bottom;
        int old_content = old_height - edges;
        int new_content = target_height - edges;
        FlatItemIterator iterator;
        FlatItem item;
        flat_iterator_init(&iterator, context, node, style);
        while (flat_iterator_next(&iterator, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            const LayoutNodeBox *child = layout_box_for_node(layout, item.node);
            if (child == NULL) continue;
            int outer = child->height + item.style.margin.top
                        + item.style.margin.bottom;
            int old_free = old_content > outer ? old_content - outer : 0;
            int new_free = new_content > outer ? new_content - outer : 0;
            AlignItems alignment = flex_item_alignment(style, &item.style);
            int offset = 0;
            if (item.style.margin_top_auto) {
                offset = item.style.margin_bottom_auto
                    ? new_free / 2 - old_free / 2 : new_free - old_free;
            } else if (!item.style.margin_bottom_auto) {
                if (alignment == ALIGN_CENTER)
                    offset = new_free / 2 - old_free / 2;
                else if (alignment == ALIGN_END)
                    offset = new_free - old_free;
            }
            if (offset != 0)
                translate_node_subtree(layout, item.node, 0, offset);
        }
    }
}

static void translate_flex_item(LayoutDocument *layout, const FlatItem *item,
                                int dx, int dy);

/* Final content-sized grid tracks are definite for stretched items. Carry
   that size through percentage-height descendants, including lifted
   display:contents children, without laying out the entire row twice. */
static void stretch_grid_item_depth(LayoutContext *context,
                                    const FlatItem *item,
                                    int target_height, unsigned depth)
{
    const LayoutNodeBox *box = flex_item_box(context->layout, item);
    if (box == NULL || target_height <= box->height || depth >= 16) return;
    int old_height = box->height;
    ComputedStyle style = item->style;
    resolve_padding(context->sheet, &style, box->width);
    int edges = style.padding.top + style.padding.bottom
                + style.border.top + style.border.bottom;
    ComputedStyle sized = style;
    sized.has_height = true;
    sized.height_percent = false;
    sized.height = target_height - (style.box_sizing_border_box ? 0 : edges);
    target_height = style_content_height(context->sheet, &sized, box->width,
                                         target_height) + edges;
    if (target_height <= old_height) return;
    int old_content = old_height > edges ? old_height - edges : 0;
    int content = target_height > edges ? target_height - edges : 0;
    bool column = (style.display == DISPLAY_FLEX
                   || style.display == DISPLAY_INLINE_FLEX)
                  && (style.flex_direction == FLEX_COLUMN
                      || style.flex_direction == FLEX_COLUMN_REVERSE)
                  && !style.flex_wrap;
    bool row = (style.display == DISPLAY_FLEX || style.display == DISPLAY_INLINE_FLEX)
        && !column && !style.flex_wrap;
    FlexOrderPlan order = {0};
    if (column && !flex_order_plan_build(&order, context, item->node, &style)) return;
    stretch_flex_item_cross(context, item, &style, target_height);
    box = flex_item_box(context->layout, item);
    if (box == NULL || box->height != target_height) {
        flex_order_plan_destroy(&order);
        return;
    }
    bool reverse = style.flex_direction == FLEX_COLUMN_REVERSE;
    FlexItemIterator iterator;
    FlatItem child;
    size_t count = 0;
    size_t auto_margins = 0;
    int used = 0;
    if (column) {
        flex_iterator_init(&iterator, context, item->node, &style, &order);
        while (flex_iterator_next(&iterator, &child)) {
            if (child.style.out_of_flow || child.style.fixed_position) continue;
            const LayoutNodeBox *child_box = flex_item_box(context->layout, &child);
            if (child_box == NULL) continue;
            if (count++) used = layout_add_coordinate(used, style.row_gap);
            used = layout_add_coordinate(used, child_box->height
                + child.style.margin.top + child.style.margin.bottom);
            auto_margins += child.style.margin_top_auto
                            + child.style.margin_bottom_auto;
        }
    }
    int growth = 0;
    flex_iterator_init(&iterator, context, item->node, &style, column ? &order : NULL);
    while (flex_iterator_next(&iterator, &child)) {
        if (child.style.fixed_position) continue;
        const LayoutNodeBox *child_box = flex_item_box(context->layout, &child);
        if (child_box == NULL) continue;
        int old_child_height = child_box->height;
        resolve_padding(context->sheet, &child.style, box->width);
        int child_edges = child.style.padding.top + child.style.padding.bottom
            + child.style.border.top + child.style.border.bottom;
        bool positioned = child.style.out_of_flow
            && (style.relative_position || style.out_of_flow);
        int reference = positioned
            ? target_height - style.border.top - style.border.bottom : content;
        if (!child.anonymous_text && child.generated_pseudo == PSEUDO_NONE
            && child.style.has_height && child.style.height_percent) {
            int height = style_content_height(context->sheet, &child.style,
                                              box->width, reference);
            stretch_grid_item_depth(context, &child, height + child_edges,
                                    depth + 1);
        } else if (positioned && !child.style.has_height
                   && child.style.has_top && child.style.has_bottom
                   && !child.anonymous_text && child.generated_pseudo == PSEUDO_NONE) {
            int padding_height = reference;
            int top = resolve_declared_length(context->sheet, child.style.top,
                (child.style.inset_percent_mask & STYLE_INSET_TOP_PERCENT) != 0, padding_height);
            int bottom = resolve_declared_length(context->sheet, child.style.bottom,
                (child.style.inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT) != 0, padding_height);
            int height = padding_height - top - bottom
                - child.style.margin.top - child.style.margin.bottom;
            stretch_grid_item_depth(context, &child, height, depth + 1);
        } else if (row && !child.style.out_of_flow && !child.style.has_height
                   && !child.style.margin_top_auto && !child.style.margin_bottom_auto
                   && flex_item_alignment(&style, &child.style) == ALIGN_STRETCH) {
            stretch_grid_item_depth(context, &child, content
                - child.style.margin.top - child.style.margin.bottom, depth + 1);
        }
        child_box = flex_item_box(context->layout, &child);
        int added = child_box != NULL ? child_box->height - old_child_height : 0;
        if (row && !child.style.out_of_flow && added > 0) {
            int old_outer = old_child_height + child.style.margin.top + child.style.margin.bottom;
            int before = content > old_outer ? content - old_outer : 0;
            int after = content > old_outer + added ? content - old_outer - added : 0;
            int offset = 0;
            AlignItems alignment = flex_item_alignment(&style, &child.style);
            if (child.style.margin_top_auto)
                offset = child.style.margin_bottom_auto ? after / 2 - before / 2 : after - before;
            else if (!child.style.margin_bottom_auto)
                offset = alignment == ALIGN_CENTER ? after / 2 - before / 2
                    : alignment == ALIGN_END ? after - before : 0;
            if (offset) translate_flex_item(context->layout, &child, 0, offset);
        }
        if (column && !child.style.out_of_flow) {
            int offset = reverse ? -growth - added : growth;
            if (offset) translate_flex_item(context->layout, &child, 0, offset);
            growth = layout_add_coordinate(growth, added);
        }
    }
    if (column && count != 0) {
        size_t index = 0, seen_margins = 0;
        int before = old_content > used ? old_content - used : 0;
        int after = content > used + growth ? content - used - growth : 0;
        flex_iterator_init(&iterator, context, item->node, &style, &order);
        while (flex_iterator_next(&iterator, &child)) {
            if (child.style.out_of_flow || child.style.fixed_position
                || flex_item_box(context->layout, &child) == NULL) continue;
            int extra = after - before;
            int offset = 0;
            if (auto_margins) {
                seen_margins += reverse ? child.style.margin_bottom_auto
                                        : child.style.margin_top_auto;
                offset = tilefinch_mul_div_int(after, seen_margins, auto_margins)
                    - tilefinch_mul_div_int(before, seen_margins, auto_margins);
            } else if (style.justify_content == JUSTIFY_CENTER) offset = after / 2 - before / 2;
            else if (style.justify_content == JUSTIFY_END) offset = extra;
            else if (style.justify_content == JUSTIFY_SPACE_BETWEEN && count > 1)
                offset = tilefinch_mul_div_int(after, index, count - 1)
                    - tilefinch_mul_div_int(before, index, count - 1);
            else if (style.justify_content == JUSTIFY_SPACE_AROUND)
                offset = tilefinch_mul_div_int(after, 2 * index + 1, 2 * count)
                    - tilefinch_mul_div_int(before, 2 * index + 1, 2 * count);
            else if (style.justify_content == JUSTIFY_SPACE_EVENLY)
                offset = tilefinch_mul_div_int(after, index + 1, count + 1)
                    - tilefinch_mul_div_int(before, index + 1, count + 1);
            if (reverse) offset = target_height - old_height - offset;
            if (offset) translate_flex_item(context->layout, &child, 0, offset);
            if (auto_margins) seen_margins += reverse ? child.style.margin_top_auto
                                                     : child.style.margin_bottom_auto;
            index++;
        }
    }
    flex_order_plan_destroy(&order);
}

void layout_stretch_grid_item(LayoutContext *context, const FlatItem *item,
                              int target_height)
{
    stretch_grid_item_depth(context, item, target_height, 0);
}

int layout_fit_column_scroller(LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, const FlexOrderPlan *order, int width, int height)
{
    if (height <= 0 || style->flex_wrap) return 0;
    FlexItemIterator iterator;
    FlatItem item;
    lxb_dom_node_t *scroller = NULL;
    int used = 0, old_height = 0;
    size_t count = 0;
    flex_iterator_init(&iterator, context, node, style, order);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = flex_item_box(context->layout, &item);
        if (box == NULL) return 0;
        if (count++) used = layout_add_coordinate(used, style->row_gap);
        used = layout_add_coordinate(used,
            box->height + item.style.margin.top + item.style.margin.bottom);
        if (item.style.flex_grow <= 0) {
            if (item.style.has_height && item.style.flex_shrink > 0) return 0;
            continue;
        }
        /* Other flexible items require full main-axis distribution. This
           retained fast path has one flexible viewport and content-sized
           siblings, whose automatic minima remain at their measured sizes. */
        if (scroller != NULL || item.anonymous_text
            || item.generated_pseudo != PSEUDO_NONE || item.style.has_height
            || item.style.min_height > 0
            || item.style.max_height != STYLE_LENGTH_NONE
            || !item.style.overflow_y_scroll || item.style.overflow_y_clip_only)
            return 0;
        scroller = item.node;
        old_height = box->height;
    }
    if (scroller == NULL) return 0;
    int target = layout_subtract_coordinate(height, used - old_height);
    int delta = 0;
    bool after = false;
    flex_iterator_init(&iterator, context, node, style, order);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        if (item.node == scroller) {
            resolve_padding(context->sheet, &item.style, width);
            int edges = item.style.padding.top + item.style.padding.bottom
                + item.style.border.top + item.style.border.bottom;
            if (target < edges) target = edges;
            stretch_flex_item_cross(context, &item, &item.style, target);
            const LayoutNodeBox *box = flex_item_box(context->layout, &item);
            if (box == NULL) return 0;
            delta = box->height - old_height;
            after = true;
        } else if (after && delta != 0) {
            translate_flex_item(context->layout, &item, 0, delta);
        }
    }
    return delta;
}

/* Move the in-flow contents of a table cell within its row-height shell.  An
   absolutely/fixed positioned descendant is positioned against its
   containing block, not the cell's vertical-align content offset.  Move the
   retained range first, then undo that translation once for every top-level
   out-of-flow subtree so paint, hit regions and node geometry stay coherent. */

static bool text_has_content(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *text = layout_text_data(node, &length);
    if (text == NULL) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isspace((unsigned char) text[i])) return true;
    }
    return false;
}

/* The next node in flattened (display:contents-lifted) flat-tree order:
   a slotted light child continues with the next node assigned to its
   slot, then after the slot (document_flat_next_sibling). */
static lxb_dom_node_t *flat_node_after(lxb_dom_node_t *node,
                                       lxb_dom_node_t *container)
{
    while (node != NULL && node != container) {
        lxb_dom_node_t *next = document_flat_next_sibling(node);
        if (next != NULL) return next;
        node = document_flat_parent(node);
    }
    return NULL;
}

static bool flat_parent_style(LayoutContext *context,
                              lxb_dom_node_t *container,
                              const ComputedStyle *container_style,
                              lxb_dom_node_t *parent,
                              ComputedStyle *result)
{
    if (parent == container) {
        *result = *container_style;
        return true;
    }
    lxb_dom_node_t *path[LAYOUT_TREE_CALL_DEPTH_LIMIT];
    size_t count = 0;
    for (lxb_dom_node_t *at = parent; at != container;
         at = document_flat_parent(at)) {
        if (at == NULL || at->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return false;
        }
        if (count == LAYOUT_TREE_CALL_DEPTH_LIMIT) {
            layout_tree_note_fallback(context, at, "display-contents");
            *result = *container_style;
            return true;
        }
        path[count++] = at;
    }
    *result = *container_style;
    while (count != 0) {
        lxb_dom_node_t *at = path[--count];
        ComputedStyle style = layout_style_for_node(context, at, result);
        bool flattened = style.display == DISPLAY_CONTENTS
            || style.display == DISPLAY_TABLE_ROW_GROUP
            || style.display == DISPLAY_TABLE_HEADER_GROUP
            || style.display == DISPLAY_TABLE_FOOTER_GROUP;
        if (!flattened || style.hidden) return false;
        *result = style;
    }
    return true;
}

void flat_iterator_init(FlatItemIterator *iterator,
                               LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *container_style)
{
    if (context != NULL) context->flat_iterator_passes++;
    *iterator = (FlatItemIterator) {
        .context = context,
        .container = container,
        .cursor = document_flat_first_child(container),
        .container_style = *container_style
    };
}

__attribute__((noinline))
bool flat_iterator_next(FlatItemIterator *iterator, FlatItem *item)
{
    while (iterator->cursor != NULL) {
        lxb_dom_node_t *node = iterator->cursor;
        iterator->cursor = flat_node_after(node, iterator->container);
        ComputedStyle parent_style;
        if (!flat_parent_style(iterator->context, iterator->container,
                               &iterator->container_style,
                               document_flat_parent(node),
                               &parent_style)) continue;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            if (!iterator->include_whitespace && !text_has_content(node)) {
                continue;
            }
            *item = (FlatItem) {
                .node = node,
                .parent_style = parent_style,
                .style = layout_style_for_node(iterator->context, node,
                                               &parent_style),
                .anonymous_text = true
            };
            iterator->context->flat_iterator_yields++;
            return true;
        }
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (layout_node_is_hidden_input(node)) continue;
        ComputedStyle style = layout_style_for_node(
            iterator->context, node, &parent_style);
        if (style.display == DISPLAY_NONE
            || style.display == DISPLAY_TABLE_COLUMN || style.hidden) {
            continue;
        }
        if (style.display == DISPLAY_CONTENTS
            || style.display == DISPLAY_TABLE_ROW_GROUP
            || style.display == DISPLAY_TABLE_HEADER_GROUP
            || style.display == DISPLAY_TABLE_FOOTER_GROUP) {
            lxb_dom_node_t *inside = document_flat_first_child(node);
            if (inside != NULL) iterator->cursor = inside;
            continue;
        }
        *item = (FlatItem) {
            .node = node,
            .parent_style = parent_style,
            .style = style,
            .anonymous_text = false
        };
        iterator->context->flat_iterator_yields++;
        return true;
    }
    return false;
}

/* The top margin exposed by a borderless block may include the first
   in-flow block descendant's margin.  Resolve that bounded chain before the
   caller places the parent's border box; doing it only inside the parent
   would leave the parent and first child at different y coordinates. */

void flex_order_plan_destroy(FlexOrderPlan *plan)
{
    if (plan == NULL) return;
    if (plan->values != NULL && plan->values != plan->inline_values) {
        budget_free(plan->budget, plan->values);
    }
    plan->values = NULL;
    plan->count = 0;
    plan->capacity = 0;
    plan->active = false;
}

bool flex_order_plan_build(FlexOrderPlan *plan,
                                  LayoutContext *context,
                                  lxb_dom_node_t *container,
                                  const ComputedStyle *style)
{
    *plan = (FlexOrderPlan) {.budget = context->layout->budget};
    plan->generated_boxes = plan->inline_generated_boxes;
    plan->values = plan->inline_values;
    plan->capacity = sizeof(plan->inline_values)
                     / sizeof(plan->inline_values[0]);
    FlexItemIterator scan;
    FlatItem item;
    flex_iterator_init(&scan, context, container, style, plan);
    while (flex_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        size_t insert = 0;
        while (insert < plan->count && plan->values[insert] < item.style.order) {
            insert++;
        }
        if (insert < plan->count && plan->values[insert] == item.style.order) {
            continue;
        }
        if (plan->count == FLEX_ORDER_VALUE_LIMIT) {
            /* More distinct authored order groups would make every later
               iterator rescan the full child list again. Degrade the whole
               container to stable DOM order rather than retain a partial,
               semantically misleading order plan or quadratic work. */
            flex_order_plan_destroy(plan);
            plan->budget = context->layout->budget;
            plan->values = plan->inline_values;
            plan->generated_boxes = plan->inline_generated_boxes;
            plan->capacity = sizeof(plan->inline_values)
                             / sizeof(plan->inline_values[0]);
            return true;
        }
        if (plan->count == plan->capacity) {
            size_t capacity = plan->capacity * 2;
            int *values;
            if (plan->values == plan->inline_values) {
                values = budget_malloc(plan->budget,
                                       capacity * sizeof(*values));
                if (values != NULL) {
                    memcpy(values, plan->inline_values,
                           plan->count * sizeof(*values));
                }
            } else {
                values = budget_realloc(plan->budget, plan->values,
                                        capacity * sizeof(*values));
            }
            if (values == NULL) {
                flex_order_plan_destroy(plan);
                return false;
            }
            plan->values = values;
            plan->capacity = capacity;
        }
        if (insert < plan->count) {
            memmove(plan->values + insert + 1, plan->values + insert,
                    (plan->count - insert) * sizeof(*plan->values));
        }
        plan->values[insert] = item.style.order;
        plan->count++;
    }
    plan->active = plan->count > 1
                   || (plan->count == 1 && plan->values[0] != 0);
    return true;
}

/* Generated children are virtual flex items: their geometry is transient
   in the container's plan, never a fabricated DOM node or retained handle. */
static bool flex_source_next(FlexItemIterator *iterator, FlatItem *item)
{
    FlatItemIterator *source = &iterator->source;
    const ComputedStyle *parent = &source->container_style;
    bool row = (parent->display == DISPLAY_FLEX
                 || parent->display == DISPLAY_INLINE_FLEX)
        && (parent->flex_direction == FLEX_ROW
            || parent->flex_direction == FLEX_ROW_REVERSE);
    if (!row) return flat_iterator_next(source, item);
    for (;;) {
        unsigned phase = iterator->generated_phase;
        if (phase == 1 && flat_iterator_next(source, item)) return true;
        if (phase >= 3) return false;
        iterator->generated_phase++;
        if (phase == 1) continue;
        PseudoElement pseudo = phase == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER;
        ComputedStyle generated = layout_style_for_pseudo(
            source->context, source->container, pseudo, parent);
        if (!generated.generated_content || generated.hidden
            || generated.display == DISPLAY_NONE || generated.out_of_flow
            || generated.fixed_position) continue;
        *item = (FlatItem) {
            .node = source->container, .parent_style = *parent,
            .style = generated, .generated_pseudo = pseudo,
            .generated_box = iterator->plan != NULL
                ? &iterator->plan->generated_boxes[phase == 0 ? 0 : 1] : NULL
        };
        return true;
    }
}

void flex_iterator_init(FlexItemIterator *iterator,
                               LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *style,
                               const FlexOrderPlan *plan)
{
    if (context != NULL) context->flex_iterator_passes++;
    *iterator = (FlexItemIterator) {.plan = plan};
    flat_iterator_init(&iterator->source, context, container, style);
}

__attribute__((noinline))
bool flex_iterator_next(FlexItemIterator *iterator, FlatItem *item)
{
    if (iterator->plan == NULL || !iterator->plan->active) {
        bool found = flex_source_next(iterator, item);
        if (found) iterator->source.context->flex_iterator_yields++;
        return found;
    }
    while (iterator->order_index < iterator->plan->count) {
        while (flex_source_next(iterator, item)) {
            if (item->style.order
                == iterator->plan->values[iterator->order_index]) {
                iterator->source.context->flex_iterator_yields++;
                return true;
            }
        }
        iterator->order_index++;
        if (iterator->order_index < iterator->plan->count) {
            LayoutContext *context = iterator->source.context;
            lxb_dom_node_t *container = iterator->source.container;
            ComputedStyle container_style = iterator->source.container_style;
            flat_iterator_init(&iterator->source,
                               context, container, &container_style);
            iterator->generated_phase = 0;
        }
    }
    return false;
}

const LayoutNodeBox *flex_item_box(const LayoutDocument *layout,
                                  const FlatItem *item)
{
    return item->generated_pseudo != PSEUDO_NONE
        ? item->generated_box : layout_box_for_node(layout, item->node);
}

static void translate_flex_item(LayoutDocument *layout, const FlatItem *item,
                                 int dx, int dy)
{
    if (item->generated_pseudo == PSEUDO_NONE) {
        translate_node_subtree(layout, item->node, dx, dy);
        return;
    }
    LayoutNodeBox *box = item->generated_box;
    if (box == NULL) return;
    for (size_t i = box->command_start;
         i < box->command_end && i < layout->count; i++) {
        layout->commands[i].x = layout_add_coordinate(layout->commands[i].x, dx);
        layout->commands[i].y = layout_add_coordinate(layout->commands[i].y, dy);
    }
    box->x = layout_add_coordinate(box->x, dx);
    box->y = layout_add_coordinate(box->y, dy);
}

void flat_text_link(lxb_dom_node_t *node, lxb_dom_node_t *container,
                           const char **url, size_t *url_length,
                           lxb_dom_node_t **link_node)
{
    *url = NULL;
    *url_length = 0;
    *link_node = NULL;
    for (lxb_dom_node_t *at = node != NULL ? node->parent : NULL;
         at != NULL && at != container; at = at->parent) {
        if (!document_node_is_tag(at, LXB_TAG_A)) continue;
        const char *href = document_attribute(at, "href", url_length);
        if (href != NULL && *url_length != 0) {
            *url = href;
            *link_node = at;
        }
        return;
    }
}

/*
 * HTML lets an <a href> wrap flow content. When the anchor is inline (a
 * block-in-inline split) or display:contents, it has no box of its own that
 * could carry the link, so the outermost boxes laid out inside it carry it
 * instead, as in browsers. Walk from the box's parent to its layout
 * container: an <a> met on the way owns the box. Deeper boxes have a nearer
 * container and so never reach the anchor, giving one region per outermost
 * box rather than one per descendant. A box laid out outside its parent
 * container's pass (the container is not an ancestor) inherits nothing.
 */
bool layout_container_link(const lxb_dom_node_t *node,
                           const lxb_dom_node_t *container,
                           const char **url, size_t *url_length,
                           lxb_dom_node_t **link_node)
{
    *url = NULL;
    *url_length = 0;
    *link_node = NULL;
    if (node == NULL || container == NULL) return false;
    const char *found_url = NULL;
    size_t found_length = 0;
    lxb_dom_node_t *found_node = NULL;
    bool anchor_seen = false;
    size_t steps = 0;
    for (lxb_dom_node_t *at = node->parent;
         at != NULL && steps < LAYOUT_TREE_CALL_DEPTH_LIMIT;
         at = at->parent, steps++) {
        if (at == container) {
            if (found_url == NULL) return false;
            *url = found_url;
            *url_length = found_length;
            *link_node = found_node;
            return true;
        }
        if (anchor_seen || !document_node_is_tag(at, LXB_TAG_A)) continue;
        anchor_seen = true;
        size_t length = 0;
        const char *href = document_attribute(at, "href", &length);
        if (href != NULL && length != 0) {
            found_url = href;
            found_length = length;
            found_node = at;
        }
    }
    return false;
}

bool layout_anonymous_text(LayoutContext *context,
                                  const FlatItem *item,
                                  lxb_dom_node_t *container,
                                  int x, int y, int width, int *bottom)
{
    size_t command_start = context->layout->count;
    LineState line = {
        .start_x = x,
        .x = x,
        .right = x + (width < 8 ? 8 : width),
        .y = y,
        .line_gap = 0,
        .layout = context->layout,
        .command_start = context->layout->count,
        .link_start = context->layout->link_count,
        .control_start = context->layout->control_count,
        .node_box_start = context->layout->node_box_count,
        .text_align = computed_style_used_text_align(&item->style),
        .direction_rtl = computed_style_direction_rtl(&item->style),
        .find_block_start = true
    };
    size_t length = 0;
    const char *text = layout_text_data(item->node, &length);
    const char *link_url = NULL;
    size_t link_length = 0;
    lxb_dom_node_t *link_node = NULL;
    flat_text_link(item->node, container, &link_url, &link_length, &link_node);
    if (text != NULL
        && !flow_text(context, &line, text, length, &item->style,
                      link_url, link_length, link_node)) return false;
    layout_flush_line(&line);
    line_finish_vertical(&line);
    /* Collapsible whitespace at the start or end of an anonymous inline box
       does not generate a line box.  In particular, indentation around block
       children must not acquire a synthetic minimum height. */
    *bottom = line.y;
    if (!add_node_box(context->layout, item->node, x, y, width,
                      *bottom - y, width, *bottom - y,
                      width, *bottom - y,
                      item->style.padding.left + item->style.padding.right,
                      item->style.padding.top + item->style.padding.bottom,
                      false, false, 0, 0, 0, 0,
                      false, false,
                      false, true,
                      command_start, context->layout->count,
                      command_start, context->layout->count,
                      line.link_start, context->layout->link_count,
                      line.control_start,
                      context->layout->control_count)) return false;
    return true;
}

int flex_child_basis(LayoutContext *context, const FlatItem *item,
                            int content_width)
{
    context->flex_basis_resolutions++;
    ComputedStyle resolved_style = item->style;
    resolve_padding(context->sheet, &resolved_style, content_width);
    resolve_margin(context->sheet, &resolved_style, content_width);
    const ComputedStyle *child_style = &resolved_style;
    if (item->generated_pseudo != PSEUDO_NONE) {
        int edges = child_style->padding.left + child_style->padding.right
                    + child_style->border.left + child_style->border.right;
        int width = layout_generated_flex_width(context, item, false);
        bool definite = child_style->has_flex_basis
            && child_style->flex_basis >= 0;
        if (definite || (child_style->has_width
                         && !child_style->width_max_content)) {
            width = resolve_declared_length(context->sheet,
                definite ? child_style->flex_basis : child_style->width,
                definite ? child_style->flex_basis_percent
                         : child_style->width_percent, content_width);
            if (child_style->box_sizing_border_box) edges = 0;
        }
        return layout_add_coordinate(layout_add_coordinate(width, edges),
            child_style->margin.left + child_style->margin.right);
    }
    int basis;
    bool explicit_size = false;
    if (child_style->has_flex_basis) {
        if (child_style->flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            basis = intrinsic_min_text_width_ignoring_own_width(
                context, item->node, &item->parent_style, content_width);
        } else if (child_style->flex_basis == STYLE_LENGTH_MAX_CONTENT
                   || child_style->flex_basis
                          == STYLE_LENGTH_FIT_CONTENT) {
            basis = intrinsic_text_width(
                context, item->node, &item->parent_style,
                child_style->flex_basis == STYLE_LENGTH_MAX_CONTENT
                    ? LAYOUT_COORDINATE_LIMIT : content_width);
        } else {
            basis = resolve_declared_length(
                context->sheet, child_style->flex_basis,
                child_style->flex_basis_percent, content_width);
            explicit_size = true;
        }
    } else if (child_style->width_max_content) {
        basis = computed_style_width_min_content(child_style)
            ? intrinsic_min_text_width_ignoring_own_width(
                  context, item->node, &item->parent_style, content_width)
            : intrinsic_text_width(context, item->node,
                                   &item->parent_style,
                                   computed_style_width_max_content(child_style)
                                       ? LAYOUT_COORDINATE_LIMIT
                                       : content_width);
    } else if (child_style->has_width) {
        basis = resolve_declared_length(
            context->sheet, child_style->width,
            child_style->width_percent, content_width);
        explicit_size = true;
    } else {
        basis = intrinsic_text_width(context, item->node,
                                     &item->parent_style, content_width);
    }
    int edges = child_style->padding.left + child_style->padding.right
                + child_style->border.left + child_style->border.right;
    int margins = child_style->margin.left + child_style->margin.right;
    if (explicit_size) {
        if (child_style->box_sizing_border_box) {
            if (basis < edges) basis = edges;
        } else {
            basis += edges;
        }
        basis += margins;
    } else {
        /* The intrinsic contribution counts percentage padding as zero
           (cyclic in general), but a flex item's containing block is the
           definite container: its content-sized base includes the padding
           resolved against it, as the item is later laid out with. */
        ComputedStyle zero_basis = item->style;
        resolve_padding(context->sheet, &zero_basis, 0);
        int percentage_padding =
            child_style->padding.left + child_style->padding.right
            - zero_basis.padding.left - zero_basis.padding.right;
        if (percentage_padding > 0) {
            basis = layout_add_coordinate(basis, percentage_padding);
        }
    }
    if (basis < margins) basis = margins;
    basis = constrain_border_box_width(
        context, item->node, &item->parent_style, child_style,
        content_width, basis - margins, NULL) + margins;
    /* Flex bases are box sizes, not text-flow widths. Empty and explicitly
       zero-width items may consume no main-axis space; a text safety floor
       here silently shrinks their neighbours inside intrinsic containers. */
    if (basis < 0) basis = 0;
    /* A definite flex base can legitimately overflow a smaller nowrap
       container; flex shrink and the automatic minimum decide whether it may
       contract. Wrapped lines do not run that container-wide shrink pass,
       however, so retain the historical per-line bound there. Without it a
       content-box `width:100%` item plus padding can overflow the viewport. */
    if ((!explicit_size || item->parent_style.flex_wrap)
        && basis > content_width) basis = content_width;
    return basis;
}

static int flex_child_minimum(LayoutContext *context, const FlatItem *item,
                              int content_width)
{
    context->flex_minimum_resolutions++;
    ComputedStyle resolved_style = item->style;
    resolve_padding(context->sheet, &resolved_style, content_width);
    resolve_margin(context->sheet, &resolved_style, content_width);
    const ComputedStyle *style = &resolved_style;
    if (item->generated_pseudo != PSEUDO_NONE) {
        int minimum = style->min_width_auto
            ? layout_generated_flex_width(context, item, true)
            : resolve_declared_length(context->sheet, style->min_width,
                                       style->min_width_percent, content_width);
        if (style->min_width_auto && style->overflow_x_scroll
            && !style->overflow_x_clip_only) minimum = 0;
        if (style->min_width_auto && style->has_width && !style->width_max_content) {
            int specified = resolve_declared_length(context->sheet, style->width,
                style->width_percent, content_width);
            if (minimum > specified) minimum = specified;
        }
        if (!style->box_sizing_border_box || style->min_width_auto)
            minimum += style->padding.left + style->padding.right
                       + style->border.left + style->border.right;
        return minimum + style->margin.left + style->margin.right;
    }
    if (style->min_width_auto) {
        /* `clip` clips paint but is deliberately not a scrollable overflow
           value. Flexbox therefore keeps the item's content-based automatic
           minimum for clip, while hidden/auto/scroll suppress it. */
        if (style->overflow_x_scroll
            && !style->overflow_x_clip_only) return 0;
        int margins = style->margin.left + style->margin.right;
        int edges = style->padding.left + style->padding.right
                    + style->border.left + style->border.right;
        int specified = 0;
        bool definite_width = style->has_width && !style->width_percent
            && !style->width_max_content;
        if (definite_width) {
            specified = resolve_declared_length(
                context->sheet, style->width, false, content_width)
                + margins;
            if (!style->box_sizing_border_box) specified += edges;
        }
        /* A flex item's definite width remains its automatic minimum even
           when padding on the parent leaves a smaller nominal content box.
           Measure through that width so fixed-size replaced controls (for
           example SVG icons) can overflow the content box instead of being
           silently squeezed below their authored size. */
        int measurement_limit = content_width;
        if (definite_width && specified > measurement_limit) {
            measurement_limit = specified;
        }
        int minimum = intrinsic_min_text_width_internal(
            context, item->node, &item->parent_style,
            measurement_limit, true);
        bool replaced = document_node_is_tag(item->node, LXB_TAG_IMG)
            || document_node_is_tag(item->node, LXB_TAG_SVG)
            || document_node_is_tag(item->node, LXB_TAG_VIDEO)
            || document_node_is_tag(item->node, LXB_TAG_IFRAME);
        /* An unloaded image or inline SVG can have no independently known
           intrinsic width even though its authored definite width reserves
           real geometry. In that one replaced-element case, the specified
           size is the only automatic-minimum suggestion; permitting a zero
           content suggestion squeezes fixed icons into the parent's leftover
           content width. Replaced content with a known natural width keeps
           the usual min(content suggestion, specified suggestion) rule. */
        if (definite_width && replaced
            && intrinsic_replaced_width(context, item->node, &item->parent_style, style) <= 0) {
            minimum = specified;
        }
        if (style->has_width && !style->width_max_content) {
            if (!definite_width) {
                specified = resolve_declared_length(
                    context->sheet, style->width, style->width_percent,
                    content_width) + margins;
                if (!style->box_sizing_border_box) specified += edges;
            }
            if (minimum > specified) minimum = specified;
        }
        return minimum;
    }
    int margins = style->margin.left + style->margin.right;
    int minimum = constrain_border_box_width(
        context, item->node, &item->parent_style, style,
        content_width, 0, NULL) + margins;
    return minimum > 0 ? minimum : 0;
}

int flex_child_row_minimum(LayoutContext *context,
                                  const FlatItem *item, int content_width,
                                  bool css_table_row)
{
    if (css_table_row && item->style.has_width
        && item->style.width_percent) {
        /* Percentage cell widths participate in table distribution; they do
           not cap the cell's content minimum as a definite width would. */
        return intrinsic_min_text_width_internal(
            context, item->node, &item->parent_style,
            content_width, true);
    }
    return flex_child_minimum(context, item, content_width);
}

/*
 * Measure one wrapped flex line without retaining a per-child side table.
 * The caller starts this scan at the first in-flow item on the line, so each
 * item participates in one bounded look-ahead scan and one layout pass.
 */
FlexLineMetrics flex_line_metrics(LayoutContext *context,
                                         FlexItemIterator *iterator,
                                         FlatItem *item,
                                         int content_width, int gap)
{
    FlexLineMetrics metrics = {0};
    while (flex_iterator_next(iterator, item)) {
        if (item->style.out_of_flow || item->style.fixed_position) {
            continue;
        }
        int basis = flex_child_basis(context, item, content_width);
        int occupied = metrics.basis;
        if (metrics.count != 0) {
            int gaps = layout_clamp_coordinate(
                (int64_t) gap * (int64_t) metrics.count);
            occupied = layout_add_coordinate(occupied, gaps);
        }
        if (metrics.count != 0
            && layout_add_coordinate(occupied, basis) > content_width) break;
        metrics.count++;
        metrics.basis = layout_add_coordinate(metrics.basis, basis);
        metrics.grow = layout_add_coordinate(
            metrics.grow, item->style.flex_grow);
        if (item->style.margin_left_auto) metrics.auto_main_margins++;
        if (item->style.margin_right_auto) metrics.auto_main_margins++;
    }
    return metrics;
}

static bool flex_item_cross_metrics(const LayoutDocument *layout,
                                    const FlatItem *item,
                                    int *origin, int *height)
{
    const LayoutNodeBox *box = flex_item_box(layout, item);
    if (box == NULL) return false;
    *origin = box->y - item->style.margin.top;
    *height = box->height + item->style.margin.top
              + item->style.margin.bottom;
    return true;
}

/* Return the first in-flow text baseline relative to the item's margin-box
   top, measured as inline layout measures it (half-leading included). A
   textless flex item synthesizes its baseline at the margin-box end. */
static bool flex_item_baseline(LayoutContext *context, const FlatItem *item,
                               int origin, int height, int *baseline)
{
    const LayoutNodeBox *box = flex_item_box(context->layout, item);
    if (box == NULL) return false;
    int text = 0;
    if (!layout_box_first_text_baseline(context->layout, box, &text)) {
        *baseline = height;
        return true;
    }
    *baseline = text - origin;
    if (*baseline < 0) *baseline = 0;
    if (*baseline > height) *baseline = height;
    return true;
}

void reverse_flex_row_items(LayoutContext *context,
                                   lxb_dom_node_t *container,
                                   const ComputedStyle *style,
                                   const FlexOrderPlan *order_plan,
                                   int content_x, int content_width)
{
    if (style->flex_direction != FLEX_ROW_REVERSE) return;
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, container, style, order_plan);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = flex_item_box(context->layout, &item);
        if (box == NULL) continue;
        int target_x = content_x + content_width
                       - (box->x - content_x) - box->width
                       + item.style.margin.left - item.style.margin.right;
        int dx = target_x - box->x;
        if (dx != 0) {
            trace_flex_translation(context, "row-reverse", container,
                                   item.node, style->align_items,
                                   content_x, content_width, box->x,
                                   box->width, target_x, dx);
            translate_flex_item(context->layout, &item, dx, 0);
        }
    }
}

static void reflect_flex_items_y(LayoutContext *context,
                                 lxb_dom_node_t *container,
                                 const ComputedStyle *style,
                                 const FlexOrderPlan *order_plan,
                                 int content_top, int content_height)
{
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, container, style, order_plan);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = flex_item_box(context->layout, &item);
        if (box == NULL) continue;
        int target_y = content_top + content_height
                       - (box->y - content_top) - box->height
                       + item.style.margin.top - item.style.margin.bottom;
        int dy = target_y - box->y;
        if (dy != 0) {
            translate_flex_item(context->layout, &item, 0, dy);
        }
    }
}

/* Reflect wrapped rows across the used cross axis. Performing this after
   line packing and per-item alignment makes cross-start/end, baseline,
   automatic margins, links, and controls follow the reversed axis without a
   retained line table. Physical margins keep their authored sides. The
   entry-point gates remain separate from the shared reflection operation. */
void reverse_flex_cross_items(LayoutContext *context,
                              lxb_dom_node_t *container,
                              const ComputedStyle *style,
                              const FlexOrderPlan *order_plan,
                              int content_top, int content_height)
{
    if (!style->flex_wrap_reverse || content_height <= 0) return;
    reflect_flex_items_y(context, container, style, order_plan,
                         content_top, content_height);
}

void reverse_flex_column_items(LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *style,
                               const FlexOrderPlan *order_plan,
                               int content_top, int content_height)
{
    if (style->flex_direction != FLEX_COLUMN_REVERSE
        || content_height <= 0) return;
    reflect_flex_items_y(context, container, style, order_plan,
                         content_top, content_height);
}

/* Pack a column-axis flex container after its children have been measured.
   Only two integers per possible track are transiently retained; item
   geometry remains in the existing node-box table.  This keeps wrapping
   bounded by the page budget without a second retained flex layout tree. */
bool place_wrapped_flex_columns(LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *style,
                               const FlexOrderPlan *order_plan,
                               int content_x, int content_width,
                               int content_top, int content_height,
                               size_t item_count)
{
    if (item_count == 0 || content_height <= 0) return true;
    if (item_count > SIZE_MAX / (5 * sizeof(int))) return false;
    int *tracks = budget_malloc(context->layout->budget,
                                item_count * 5 * sizeof(*tracks));
    if (tracks == NULL) return false;
    int *widths = tracks;
    int *origins = tracks + item_count;
    int *used_heights = origins + item_count;
    int *item_counts = used_heights + item_count;
    int *auto_margins = item_counts + item_count;
    memset(tracks, 0, item_count * 5 * sizeof(*tracks));

    FlexItemIterator scan;
    FlatItem item;
    flex_iterator_init(&scan, context, container, style, order_plan);
    size_t column = 0;
    int used_height = 0;
    while (flex_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = flex_item_box(context->layout, &item);
        if (box == NULL) continue;
        int outer_height = box->height + item.style.margin.top
                           + item.style.margin.bottom;
        int next_height = used_height == 0
                          ? outer_height
                          : used_height + style->row_gap + outer_height;
        if (used_height != 0 && next_height > content_height
            && column + 1 < item_count) {
            used_heights[column] = used_height;
            column++;
            used_height = outer_height;
        } else {
            used_height = next_height;
        }
        item_counts[column]++;
        if (item.style.margin_top_auto) auto_margins[column]++;
        if (item.style.margin_bottom_auto) auto_margins[column]++;
        int outer_width = box->width + item.style.margin.left
                          + item.style.margin.right;
        if (outer_width > widths[column]) widths[column] = outer_width;
    }
    used_heights[column] = used_height;
    size_t column_count = column + 1;
    int natural_width = style->gap * (int) (column_count - 1);
    for (size_t i = 0; i < column_count; i++) {
        natural_width = layout_add_coordinate(natural_width, widths[i]);
    }
    int free_width = content_width - natural_width;
    if (free_width < 0) free_width = 0;
    int initial_offset = 0;
    int between_extra = 0;
    int stretch_each = 0;
    int stretch_remainder = 0;
    if (style->align_content == JUSTIFY_CENTER) {
        initial_offset = free_width / 2;
    } else if (style->align_content == JUSTIFY_END) {
        initial_offset = free_width;
    } else if (style->align_content == JUSTIFY_SPACE_BETWEEN
               && column_count > 1) {
        between_extra = free_width / (int) (column_count - 1);
    } else if (style->align_content == JUSTIFY_SPACE_AROUND) {
        between_extra = free_width / (int) column_count;
        initial_offset = between_extra / 2;
    } else if (style->align_content == JUSTIFY_SPACE_EVENLY) {
        between_extra = free_width / (int) (column_count + 1);
        initial_offset = between_extra;
    } else if (style->align_content == JUSTIFY_STRETCH) {
        stretch_each = free_width / (int) column_count;
        stretch_remainder = free_width % (int) column_count;
    }

    if (!style->flex_wrap_reverse) {
        int cursor = content_x + initial_offset;
        for (size_t i = 0; i < column_count; i++) {
            origins[i] = cursor;
            int stretch = stretch_each
                          + ((int) i < stretch_remainder ? 1 : 0);
            cursor = layout_add_coordinate(
                cursor, widths[i] + stretch + style->gap + between_extra);
        }
    } else {
        int cursor = content_x + content_width - initial_offset;
        for (size_t i = 0; i < column_count; i++) {
            int stretch = stretch_each
                          + ((int) i < stretch_remainder ? 1 : 0);
            origins[i] = cursor - widths[i] - stretch;
            cursor = layout_subtract_coordinate(
                origins[i], style->gap + between_extra);
        }
    }

    FlexItemIterator place;
    flex_iterator_init(&place, context, container, style, order_plan);
    column = 0;
    used_height = 0;
    int item_index = 0;
    int auto_margin_seen = 0;
    while (flex_iterator_next(&place, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = flex_item_box(context->layout, &item);
        if (box == NULL) continue;
        int outer_height = box->height + item.style.margin.top
                           + item.style.margin.bottom;
        int next_height = used_height == 0
                          ? outer_height
                          : used_height + style->row_gap + outer_height;
        if (used_height != 0 && next_height > content_height
            && column + 1 < column_count) {
            column++;
            used_height = 0;
            item_index = 0;
            auto_margin_seen = 0;
        }
        int before = used_height == 0 ? 0 : style->row_gap;
        used_height += before;
        int free_height = content_height - used_heights[column];
        if (free_height < 0) free_height = 0;
        int main_offset = 0;
        if (auto_margins[column] != 0) {
            bool auto_before = style->flex_direction == FLEX_COLUMN_REVERSE
                ? item.style.margin_bottom_auto
                : item.style.margin_top_auto;
            if (auto_before) auto_margin_seen++;
            main_offset = tilefinch_mul_div_int(
                free_height, auto_margin_seen, auto_margins[column]);
        } else if (style->justify_content == JUSTIFY_CENTER) {
            main_offset = free_height / 2;
        } else if (style->justify_content == JUSTIFY_END) {
            main_offset = free_height;
        } else if (style->justify_content == JUSTIFY_SPACE_BETWEEN
                   && item_counts[column] > 1) {
            main_offset = tilefinch_mul_div_int(
                free_height, item_index, item_counts[column] - 1);
        } else if (style->justify_content == JUSTIFY_SPACE_AROUND) {
            main_offset = tilefinch_mul_div_int(
                free_height, 2 * item_index + 1, 2 * item_counts[column]);
        } else if (style->justify_content == JUSTIFY_SPACE_EVENLY) {
            main_offset = tilefinch_mul_div_int(
                free_height, item_index + 1, item_counts[column] + 1);
        }
        int target_y;
        if (style->flex_direction == FLEX_COLUMN_REVERSE) {
            target_y = content_top + content_height - used_height
                       - item.style.margin.bottom - box->height
                       - main_offset;
        } else {
            target_y = content_top + used_height + item.style.margin.top
                       + main_offset;
        }
        int track_width = widths[column] + stretch_each
            + ((int) column < stretch_remainder ? 1 : 0);
        int target_x = style->flex_wrap_reverse
            ? origins[column] + track_width - item.style.margin.right
              - box->width
            : origins[column] + item.style.margin.left;
        translate_flex_item(context->layout, &item,
                               target_x - box->x, target_y - box->y);
        used_height += outer_height;
        if (auto_margins[column] != 0) {
            bool auto_after = style->flex_direction == FLEX_COLUMN_REVERSE
                ? item.style.margin_top_auto
                : item.style.margin_bottom_auto;
            if (auto_after) auto_margin_seen++;
        }
        item_index++;
    }
    budget_free(context->layout->budget, tracks);
    return true;
}

int distribute_flex_rows(LayoutContext *context,
                                lxb_dom_node_t *container,
                                const ComputedStyle *style,
                                const FlexOrderPlan *order_plan,
                                int declared, int *stretch_free,
                                size_t *stretch_lines)
{
    *stretch_free = 0;
    *stretch_lines = 0;
    if (!style->flex_wrap || declared <= 0) return declared;
    FlexItemIterator scan;
    FlatItem item;
    flex_iterator_init(&scan, context, container, style, order_plan);
    size_t line_count = 0;
    int line_origin = 0;
    int natural_top = 0;
    int natural_bottom = 0;
    while (flex_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        int origin = 0, height = 0;
        if (!flex_item_cross_metrics(context->layout, &item,
                                     &origin, &height)) continue;
        if (line_count == 0 || origin != line_origin) {
            line_origin = origin;
            if (line_count == 0) natural_top = origin;
            line_count++;
        }
        if (origin + height > natural_bottom) natural_bottom = origin + height;
    }
    int free_cross = declared - (natural_bottom - natural_top);
    if (line_count == 0
        || style->align_content == JUSTIFY_START) return declared;
    if (free_cross > 0
        && style->align_content == JUSTIFY_STRETCH) {
        *stretch_free = free_cross;
        *stretch_lines = line_count;
    }
    FlexItemIterator place;
    flex_iterator_init(&place, context, container, style, order_plan);
    size_t line_index = 0;
    bool have_line = false;
    line_origin = 0;
    while (flex_iterator_next(&place, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        int origin = 0, height = 0;
        if (!flex_item_cross_metrics(context->layout, &item,
                                     &origin, &height)) continue;
        if (!have_line) {
            line_origin = origin;
            have_line = true;
        } else if (origin != line_origin) {
            line_origin = origin;
            line_index++;
        }
        int offset = 0;
        if (style->align_content == JUSTIFY_CENTER) {
            offset = free_cross / 2;
        } else if (style->align_content == JUSTIFY_END) {
            offset = free_cross;
        } else if (style->align_content == JUSTIFY_SPACE_BETWEEN) {
            if (free_cross > 0 && line_count > 1) {
                offset = tilefinch_mul_div_int(
                    free_cross, line_index, line_count - 1);
            }
        } else if (style->align_content == JUSTIFY_SPACE_AROUND) {
            /* The overflow fallback for distributed space-around is safe
               center; it must translate the intact line group rather than
               distributing a negative gap through it. */
            offset = free_cross < 0
                ? free_cross / 2
                : tilefinch_mul_div_int(
                    free_cross, 2 * line_index + 1, 2 * line_count);
        } else if (style->align_content == JUSTIFY_SPACE_EVENLY) {
            offset = free_cross < 0
                ? free_cross / 2
                : tilefinch_mul_div_int(
                    free_cross, line_index + 1, line_count + 1);
        } else if (style->align_content == JUSTIFY_STRETCH
                   && free_cross > 0) {
            offset = tilefinch_mul_div_int(
                free_cross, line_index, line_count);
        }
        if (offset != 0) {
            translate_flex_item(context->layout, &item, 0, offset);
        }
    }
    return declared;
}

/*
 * Resolve row cross-axis alignment after the line's tallest item is known.
 * Two streaming scans per line avoid a retained child table while keeping
 * links, controls, and descendant boxes attached to translated items.
 */
void align_flex_row_items(LayoutContext *context,
                                 lxb_dom_node_t *container,
                                 const ComputedStyle *style,
                                 const FlexOrderPlan *order_plan,
                                 int declared, int stretch_free,
                                 size_t stretch_lines)
{
    FlexItemIterator next_line;
    flex_iterator_init(&next_line, context, container, style, order_plan);
    size_t line_index = 0;
    for (;;) {
        FlexItemIterator line_start = next_line;
        FlexItemIterator scan = line_start;
        FlatItem item;
        size_t line_items = 0;
        int line_origin = 0;
        int line_height = 0;
        int baseline_ascent = 0;
        int baseline_descent = 0;
        bool boundary = false;
        while (true) {
            FlexItemIterator before = scan;
            if (!flex_iterator_next(&scan, &item)) break;
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            int origin = 0, height = 0;
            if (!flex_item_cross_metrics(context->layout, &item,
                                         &origin, &height)) continue;
            if (line_items == 0) line_origin = origin;
            else if (style->flex_wrap && origin != line_origin) {
                next_line = before;
                boundary = true;
                break;
            }
            if (height > line_height) line_height = height;
            AlignItems alignment = flex_item_alignment(style, &item.style);
            if (alignment == ALIGN_BASELINE
                && !item.style.margin_top_auto
                && !item.style.margin_bottom_auto) {
                int baseline = 0;
                if (flex_item_baseline(context, &item, origin, height,
                                       &baseline)) {
                    if (baseline > baseline_ascent) {
                        baseline_ascent = baseline;
                    }
                    if (height - baseline > baseline_descent) {
                        baseline_descent = height - baseline;
                    }
                }
            }
            line_items++;
        }
        if (line_items == 0) break;
        if (!style->flex_wrap) {
            if (declared > line_height) line_height = declared;
        }
        if (stretch_free > 0 && line_index < stretch_lines) {
            int before = tilefinch_mul_div_int(
                stretch_free, line_index, stretch_lines);
            int after = tilefinch_mul_div_int(
                stretch_free, line_index + 1, stretch_lines);
            line_height += after - before;
        }
        int baseline_height = baseline_ascent + baseline_descent;
        if (baseline_height > line_height) {
            int delta = baseline_height - line_height;
            line_height = baseline_height;
            if (boundary) {
                FlexItemIterator shift = next_line;
                FlatItem later;
                while (flex_iterator_next(&shift, &later)) {
                    if (later.style.out_of_flow
                        || later.style.fixed_position) continue;
                    translate_flex_item(context->layout, &later,
                                           0, delta);
                }
            }
        }
        FlexItemIterator place = line_start;
        size_t placed = 0;
        while (placed < line_items && flex_iterator_next(&place, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            int origin = 0, height = 0;
            if (!flex_item_cross_metrics(context->layout, &item,
                                         &origin, &height)) continue;
            int free_cross = line_height - height;
            bool table_cell = style->display == DISPLAY_TABLE_ROW
                              && !item.anonymous_text
                              && item.style.display == DISPLAY_TABLE_CELL;
            if (table_cell) {
                int target_height = line_height - item.style.margin.top
                                    - item.style.margin.bottom;
                int content_offset = 0;
                if (free_cross > 0) {
                    if (item.style.vertical_align == VERTICAL_MIDDLE) {
                        content_offset = (free_cross + 1) / 2;
                    } else if (item.style.vertical_align
                               == VERTICAL_BOTTOM) {
                        content_offset = free_cross;
                    }
                }
                if (target_height > 0) {
                    stretch_flex_item_cross(context, &item,
                                            &item.style, target_height);
                }
                if (content_offset > 0) {
                    translate_table_cell_contents(context->layout,
                                                  item.node,
                                                  content_offset);
                }
                placed++;
                continue;
            }
            AlignItems alignment = flex_item_alignment(style, &item.style);
            if (item.style.margin_top_auto
                || item.style.margin_bottom_auto) {
                int offset = item.style.margin_top_auto
                    ? (item.style.margin_bottom_auto
                       ? free_cross / 2 : free_cross)
                    : 0;
                if (offset > 0) {
                    translate_flex_item(context->layout, &item,
                                           0, offset);
                }
                placed++;
                continue;
            }
            if (alignment == ALIGN_STRETCH && !item.style.has_height) {
                int target_height = line_height - item.style.margin.top
                                    - item.style.margin.bottom;
                if (target_height > 0) {
                    stretch_flex_item_cross(context, &item,
                                            &item.style, target_height);
                }
                placed++;
                continue;
            }
            if (alignment == ALIGN_BASELINE) {
                int baseline = 0;
                if (flex_item_baseline(context, &item, origin, height,
                                       &baseline)) {
                    int offset = baseline_ascent - baseline;
                    if (offset > 0) {
                        translate_flex_item(context->layout, &item,
                                               0, offset);
                    }
                }
                placed++;
                continue;
            }
            int offset = alignment == ALIGN_CENTER
                         ? free_cross / 2 : free_cross;
            if ((alignment == ALIGN_CENTER || alignment == ALIGN_END)
                && offset > 0) {
                translate_flex_item(context->layout, &item, 0, offset);
            }
            placed++;
        }
        line_index++;
        if (!boundary) break;
    }
}
