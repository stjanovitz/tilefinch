/* Row-direction flex formatting context: the pre-scan that measures flex
   bases and shrink weights, the main-axis distribution and item layout walk,
   and the cross-axis line distribution and alignment. CSS tables in row
   direction share this path. Split out of layout_block.c, where it ran as one
   phase of layout_block_impl. */

#include "layout_block_internal.h"
#include "tilefinch/integer_math.h"

bool layout_block_flexrow_section(LayoutContext *context,
                                  const LayoutBlockFrame *frame)
{
    if (frame == NULL || !frame->flex_row) return true;
    lxb_dom_node_t *node = frame->node;
    ComputedStyle *style = frame->style;
    LayoutBlockScratch *scratch = frame->scratch;
    LineState *line = frame->line;
    PositionedBox *descendant_positioned_box =
        frame->descendant_positioned_box;
    int content_x = frame->content_x;
    int content_width = frame->content_width;
    int child_containing_height = frame->child_containing_height;
    int declared_content_height = frame->declared_content_height;
    bool replaced_content = frame->replaced_content;
    bool flex_row = frame->flex_row;
    bool reverse_row = frame->reverse_row;
    bool table_row = frame->table_row;
    bool css_table_row = frame->css_table_row;
    bool anonymous_cell_row = frame->anonymous_cell_row;
    size_t flex_children = 0;
    size_t css_table_auto_children = 0;
    size_t total_main_auto_margins = 0;
    int total_basis = 0;
    int total_hypothetical_basis = 0;
    int total_grow = 0;
    long long total_shrink_weight = 0;
    FlexOrderPlan *row_order = &scratch->row_order;
    *row_order = (FlexOrderPlan) {0};
    if (flex_row && !replaced_content) {
        if (!flex_order_plan_build(row_order, context, node, style)) {
            return false;
        }
        FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
        FlatItem *item = &scratch->traversal.flex.item;
        flex_iterator_init(iterator, context, node, style, row_order);
        while (flex_iterator_next(iterator, item)) {
            if (item->style.out_of_flow || item->style.fixed_position) continue;
            flex_children++;
            if (css_table_row && !item->style.has_width) {
                css_table_auto_children++;
            }
            bool previous_intrinsic_pair_mode =
                context->intrinsic_pair_mode;
            if (!table_row) context->intrinsic_pair_mode = true;
            int child_basis = table_row ? 0
                              : flex_child_basis(context, item,
                                                 content_width);
            int child_minimum = table_row ? 0 : flex_child_row_minimum(
                context, item, content_width, css_table_row);
            context->intrinsic_pair_mode = previous_intrinsic_pair_mode;
            if (css_table_row && item->style.has_width
                && !item->style.width_percent) {
                child_minimum = child_basis;
            }
            if (css_table_row && child_basis < child_minimum) {
                child_basis = child_minimum;
            }
            total_basis = layout_add_coordinate(total_basis, child_basis);
            total_hypothetical_basis = layout_add_coordinate(
                total_hypothetical_basis,
                child_basis < child_minimum ? child_minimum : child_basis);
            total_grow = layout_add_coordinate(
                total_grow, item->style.flex_grow);
            if (!table_row) {
                if (item->style.margin_left_auto) {
                    total_main_auto_margins++;
                }
                if (item->style.margin_right_auto) {
                    total_main_auto_margins++;
                }
            }
            if (!table_row && item->style.flex_shrink > 0) {
                int shrink_capacity = child_basis - child_minimum;
                if (shrink_capacity > 0) {
                    /* CSS Flexbox scales flex-shrink by the outer flex base
                       size. The minimum is a clamp, not the weighting input;
                       using remaining capacity here made equal bases shrink
                       unequally when their text minima differed. */
                    total_shrink_weight += (long long) child_basis
                                           * item->style.flex_shrink;
                }
            }
        }
    }
    if (flex_row && flex_children != 0 && !replaced_content) {
        layout_flush_line(line);
        const TableTracks *table_tracks = table_row
            ? table_tracks_for_row(context, node, content_width)
            : NULL;
        int gaps = table_row ? 0 : layout_clamp_coordinate(
            (int64_t) style->gap * (int64_t) (flex_children - 1u));
        if (!table_row
            && layout_add_coordinate(total_basis, gaps) > content_width) {
            total_basis = total_hypothetical_basis;
        }
        int remaining = layout_subtract_coordinate(
            layout_subtract_coordinate(content_width, total_basis), gaps);
        if (anonymous_cell_row && remaining > 0) {
            /* The anonymous table wrapper around an otherwise complete run
               of table-cell children shrink-wraps its row. The containing
               block itself still retains its ordinary used width. */
            remaining = 0;
        }
        int cursor_x = content_x;
        if (table_tracks != NULL && table_tracks->border_collapse) {
            cursor_x = layout_add_coordinate(
                cursor_x, table_tracks->collapsed_left_gutter);
        } else if (table_tracks != NULL) {
            cursor_x = layout_add_coordinate(
                cursor_x, table_tracks->spacing_x);
        }
        int flex_gap = table_row ? 0 : style->gap;
        size_t css_table_auto_seen = 0;
        trace_flex_sizing(context, "container", node, NULL, NULL,
                          content_width, total_basis, remaining, cursor_x,
                          0, 0);
        if (remaining > 0 && total_grow == 0
            && total_main_auto_margins == 0) {
            if (style->justify_content == JUSTIFY_CENTER) {
                cursor_x = layout_add_coordinate(cursor_x, remaining / 2);
            } else if (style->justify_content == JUSTIFY_END) {
                cursor_x = layout_add_coordinate(cursor_x, remaining);
            } else if (style->justify_content == JUSTIFY_SPACE_BETWEEN
                       && flex_children > 1) {
                flex_gap = layout_add_coordinate(
                    flex_gap, remaining / (int) (flex_children - 1));
            } else if (style->justify_content == JUSTIFY_SPACE_AROUND) {
                int distributed = remaining / (int) flex_children;
                cursor_x = layout_add_coordinate(
                    cursor_x, distributed / 2);
                flex_gap = layout_add_coordinate(flex_gap, distributed);
            } else if (style->justify_content == JUSTIFY_SPACE_EVENLY) {
                int distributed = remaining / ((int) flex_children + 1);
                cursor_x = layout_add_coordinate(cursor_x, distributed);
                flex_gap = layout_add_coordinate(flex_gap, distributed);
            }
        }
        int cursor_y = line->y;
        if (table_row && table_tracks != NULL
            && !table_tracks->border_collapse) {
            cursor_y = layout_add_coordinate(
                cursor_y, table_tracks->spacing_y);
        }
        int anonymous_cursor_fixed = layout_fixed_from_integer(cursor_x);
        int row_bottom = cursor_y;
        size_t table_child_index = 0;
        size_t line_items_remaining = 0;
        int line_remaining = remaining;
        int line_total_grow = total_grow;
        size_t line_auto_margins = total_main_auto_margins;
        size_t auto_margin_seen = 0;
        int auto_margin_space = remaining > 0 && total_grow == 0
                                && total_main_auto_margins != 0
                                ? remaining : 0;
        long long shrink_weight_seen = 0;
        bool first_flex_line = true;
        FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
        FlexItemIterator *item_start =
            &scratch->traversal.flex.saved_iterator;
        FlatItem *item = &scratch->traversal.flex.item;
        flex_iterator_init(iterator, context, node, style, row_order);
        while (true) {
            *item_start = *iterator;
            if (!flex_iterator_next(iterator, item)) break;
            if (item->style.out_of_flow || item->style.fixed_position) continue;
            const ComputedStyle *child_style = &item->style;
            if (anonymous_cell_row) {
                cursor_x = layout_fixed_floor(anonymous_cursor_fixed);
            }
            if (style->flex_wrap && !table_row
                && line_items_remaining == 0) {
                FlexLineMetrics metrics = flex_line_metrics(
                    context, item_start,
                    &scratch->traversal.flex.lookahead_item,
                    content_width, style->gap);
                line_items_remaining = metrics.count;
                int line_gaps = layout_clamp_coordinate(
                    (int64_t) style->gap
                    * (int64_t) (metrics.count - 1u));
                line_remaining = layout_subtract_coordinate(
                    layout_subtract_coordinate(
                        content_width, metrics.basis), line_gaps);
                line_total_grow = metrics.grow;
                line_auto_margins = metrics.auto_main_margins;
                auto_margin_seen = 0;
                auto_margin_space = line_remaining > 0
                                    && line_total_grow == 0
                                    && line_auto_margins != 0
                                    ? line_remaining : 0;
                if (!first_flex_line) {
                    cursor_y = layout_add_coordinate(
                        row_bottom, style->row_gap);
                }
                first_flex_line = false;
                cursor_x = content_x;
                flex_gap = style->gap;
                if (line_remaining > 0 && line_total_grow == 0
                    && line_auto_margins == 0) {
                    if (style->justify_content == JUSTIFY_CENTER) {
                        cursor_x = layout_add_coordinate(
                            cursor_x, line_remaining / 2);
                    } else if (style->justify_content == JUSTIFY_END) {
                        cursor_x = layout_add_coordinate(
                            cursor_x, line_remaining);
                    } else if (style->justify_content
                               == JUSTIFY_SPACE_BETWEEN
                               && metrics.count > 1) {
                        flex_gap = layout_add_coordinate(
                            flex_gap,
                            line_remaining / (int) (metrics.count - 1));
                    } else if (style->justify_content
                               == JUSTIFY_SPACE_AROUND) {
                        int distributed = line_remaining
                                          / (int) metrics.count;
                        cursor_x = layout_add_coordinate(
                            cursor_x, distributed / 2);
                        flex_gap = layout_add_coordinate(
                            flex_gap, distributed);
                    } else if (style->justify_content
                               == JUSTIFY_SPACE_EVENLY) {
                        int distributed = line_remaining
                                          / ((int) metrics.count + 1);
                        cursor_x = layout_add_coordinate(
                            cursor_x, distributed);
                        flex_gap = layout_add_coordinate(
                            flex_gap, distributed);
                    }
                }
            }
            bool auto_before = reverse_row
                               ? child_style->margin_right_auto
                               : child_style->margin_left_auto;
            bool auto_after = reverse_row
                              ? child_style->margin_left_auto
                              : child_style->margin_right_auto;
            if (auto_margin_space > 0 && auto_before) {
                int before = tilefinch_mul_div_int(
                    auto_margin_space, auto_margin_seen, line_auto_margins);
                auto_margin_seen++;
                int after = tilefinch_mul_div_int(
                    auto_margin_space, auto_margin_seen, line_auto_margins);
                cursor_x = layout_add_coordinate(
                    cursor_x, after - before);
            }
            size_t table_span = table_row && !item->anonymous_text
                                ? table_cell_span(item->node) : 1;
            size_t placed_column = table_child_index;
            if (table_row && table_tracks != NULL
                && !item->anonymous_text) {
                (void) table_cell_placement(
                    table_tracks, item->node, &placed_column,
                    &table_span, NULL);
                table_child_index = placed_column;
                cursor_x = layout_add_coordinate(
                    content_x,
                    table_tracks->border_collapse
                        ? table_tracks->collapsed_left_gutter
                        : table_tracks->spacing_x);
                for (size_t i = 0;
                     i < placed_column && i < table_tracks->count; i++) {
                    cursor_x = layout_add_coordinate(
                        cursor_x,
                        layout_add_coordinate(
                            table_tracks->widths[i],
                            table_tracks->spacing_x));
                }
            }
            int child_width = 0;
            if (table_row && table_tracks != NULL
                && table_child_index < table_tracks->count) {
                if (table_span > table_tracks->count - table_child_index) {
                    table_span = table_tracks->count - table_child_index;
                }
                for (size_t i = 0; i < table_span; i++) {
                    child_width = layout_add_coordinate(
                        child_width,
                        table_tracks->widths[table_child_index + i]);
                }
                if (!table_tracks->border_collapse && table_span > 1) {
                    child_width = layout_add_coordinate(
                        child_width,
                        layout_clamp_coordinate(
                            (int64_t) table_tracks->spacing_x
                            * (int64_t) (table_span - 1u)));
                }
                /* A spanning cell's authored width covers the complete grid
                   area, including the internal border-spacing gutters. The
                   track solver distributes that constraint, but available-
                   width growth can make the sum larger; retain the explicit
                   non-percent cell width as the used border-box floor and
                   ceiling for this bounded table path. */
                if (!table_tracks->border_collapse && table_span > 1
                    && child_style->has_width
                    && !child_style->width_max_content
                    && !child_style->width_percent) {
                    int authored = resolve_declared_length(
                        context->sheet, child_style->width, false,
                        content_width);
                    if (!child_style->box_sizing_border_box) {
                        authored += child_style->padding.left
                                    + child_style->padding.right
                                    + child_style->border.left
                                    + child_style->border.right;
                    }
                    if (authored > 0) child_width = authored;
                }
            } else if (table_row) {
                child_width = content_width / (int) flex_children;
            } else {
                child_width = flex_child_basis(context, item,
                                                content_width);
            }
            if (css_table_row) {
                int table_minimum = flex_child_row_minimum(
                    context, item, content_width, true);
                if (child_style->has_width
                    && !child_style->width_percent) {
                    table_minimum = child_width;
                }
                if (child_width < table_minimum) {
                    child_width = table_minimum;
                }
            }
            if (css_table_row && remaining > 0
                && !child_style->has_width
                && css_table_auto_children != 0) {
                int before = tilefinch_mul_div_int(
                    remaining, css_table_auto_seen, css_table_auto_children);
                css_table_auto_seen++;
                int after = tilefinch_mul_div_int(
                    remaining, css_table_auto_seen, css_table_auto_children);
                child_width = layout_add_coordinate(
                    child_width, after - before);
            }
            int child_basis = child_width;
            int available = style->flex_wrap ? line_remaining : remaining;
            int available_grow = style->flex_wrap ? line_total_grow
                                                 : total_grow;
            if (available > 0 && available_grow > 0
                && child_style->flex_grow > 0) {
                child_width = layout_add_coordinate(
                    child_width,
                    tilefinch_mul_div_int(
                        available, child_style->flex_grow,
                        available_grow));
            } else if (remaining < 0 && !style->flex_wrap
                       && child_style->flex_shrink > 0
                       && total_shrink_weight > 0) {
                int child_minimum = flex_child_row_minimum(
                    context, item, content_width, css_table_row);
                if (css_table_row && child_style->has_width
                    && !child_style->width_percent) {
                    child_minimum = child_basis;
                }
                int shrink_capacity = child_width - child_minimum;
                if (shrink_capacity < 0) shrink_capacity = 0;
                long long child_weight = shrink_capacity == 0 ? 0
                    : (long long) child_basis * child_style->flex_shrink;
                long long next_weight = shrink_weight_seen + child_weight;
                int before = (int) ((long long) remaining
                                    * shrink_weight_seen
                                    / total_shrink_weight);
                int after = (int) ((long long) remaining * next_weight
                                   / total_shrink_weight);
                child_width = layout_add_coordinate(
                    child_width, after - before);
                if (child_width < child_minimum) child_width = child_minimum;
                shrink_weight_seen = next_weight;
            }
            int used_minimum = table_row ? 0 : flex_child_row_minimum(
                context, item, content_width, css_table_row);
            if (css_table_row && child_style->has_width
                && !child_style->width_percent) {
                used_minimum = child_basis;
            }
            if (child_width < used_minimum) child_width = used_minimum;
            if (child_width < 8) child_width = 8;
            trace_flex_sizing(context, "assigned", node, item->node,
                              child_style, content_width, total_basis,
                              remaining, cursor_x, child_basis, child_width);
            int child_bottom = cursor_y;
            size_t command_start = context->layout->count;
            if (item->anonymous_text) {
                if (!layout_anonymous_text(context, item, node, cursor_x,
                                           cursor_y, child_width,
                                           &child_bottom)) {
                    flex_order_plan_destroy(row_order);
                    return false;
                }
            } else if (!layout_block(context, item->node,
                                     &item->parent_style,
                                     cursor_x, cursor_y, child_width,
                                     child_containing_height, true,
                                     descendant_positioned_box,
                                     &child_bottom)) {
                flex_order_plan_destroy(row_order);
                return false;
            }
            if (child_bottom > row_bottom) row_bottom = child_bottom;
            int painted_right = layout_add_coordinate(
                cursor_x, child_width);
            for (size_t command_index = command_start;
                 command_index < context->layout->count; command_index++) {
                const DrawCommand *painted =
                    &context->layout->commands[command_index];
                int right = layout_add_coordinate(
                    painted->x, painted->width);
                if (right > painted_right) painted_right = right;
            }
            trace_flex_sizing(context, "painted", node, item->node,
                              child_style, content_width, total_basis,
                              remaining, cursor_x, child_width,
                              painted_right - cursor_x);
            if (anonymous_cell_row && !item->anonymous_text) {
                anonymous_cursor_fixed = layout_fixed_add(
                    anonymous_cursor_fixed,
                    layout_single_text_advance_fixed(
                        context, item->node, child_style, child_width));
                cursor_x = layout_fixed_floor(anonymous_cursor_fixed);
            } else {
                cursor_x = layout_add_coordinate(cursor_x, child_width);
            }
            if (auto_margin_space > 0 && auto_after) {
                int before = tilefinch_mul_div_int(
                    auto_margin_space, auto_margin_seen, line_auto_margins);
                auto_margin_seen++;
                int after = tilefinch_mul_div_int(
                    auto_margin_space, auto_margin_seen, line_auto_margins);
                cursor_x = layout_add_coordinate(
                    cursor_x, after - before);
            }
            cursor_x = layout_add_coordinate(cursor_x, flex_gap);
            if (table_row) {
                size_t next_column = placed_column + table_span;
                if (next_column > table_child_index) {
                    table_child_index = next_column;
                }
            }
            if (style->flex_wrap && line_items_remaining > 0) {
                line_items_remaining--;
            }
        }
        reverse_flex_row_items(context, node, style, row_order,
                               content_x, content_width);
        int flex_flow_top = line->y;
        int stretch_free = 0;
        size_t stretch_lines = 0;
        int cross_height = distribute_flex_rows(
            context, node, style, row_order, declared_content_height,
            &stretch_free, &stretch_lines);
        align_flex_row_items(context, node, style, row_order,
                             declared_content_height, stretch_free,
                             stretch_lines);
        FlexItemIterator *aligned_items = &scratch->traversal.flex.iterator;
        FlatItem *aligned_item = &scratch->traversal.flex.item;
        flex_iterator_init(aligned_items, context, node, style, row_order);
        while (flex_iterator_next(aligned_items, aligned_item)) {
            if (aligned_item->style.out_of_flow
                || aligned_item->style.fixed_position) continue;
            const LayoutNodeBox *aligned_box = layout_box_for_node(
                context->layout, aligned_item->node);
            if (aligned_box == NULL) continue;
            int aligned_bottom = layout_add_coordinate(
                layout_add_coordinate(aligned_box->y, aligned_box->height),
                aligned_item->style.margin.bottom);
            if (aligned_bottom > row_bottom) row_bottom = aligned_bottom;
        }
        int cross_bottom = layout_add_coordinate(
            flex_flow_top, cross_height);
        if (cross_height > 0 && row_bottom < cross_bottom) {
            row_bottom = cross_bottom;
        }
        reverse_flex_cross_items(
            context, node, style, row_order, flex_flow_top,
            cross_height > 0 ? cross_height : row_bottom - flex_flow_top);
        line->y = row_bottom;
        line_cursor_set(line, line->start_x);
        line->line_height = 0;
        line->line_height_fixed = 0;
        line->y_fixed_valid = false;
    }
    flex_order_plan_destroy(row_order);
    return true;
}
