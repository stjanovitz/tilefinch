/* Row-direction flex formatting context: the pre-scan that measures flex
   bases and shrink weights, the main-axis distribution and item layout walk,
   and the cross-axis line distribution and alignment. CSS tables in row
   direction share this path. Split out of layout_block.c, where it ran as one
   phase of layout_block_impl. */

#include "layout_block_internal.h"
#include "tilefinch/integer_math.h"

/* A percentage cell's width once the row's percentages are cut at 100%
   (CSS Tables 3, as Chromium does): a cell whose percentage would take the
   row past its width only gets what is left, down to none. `used` is the
   row width the earlier cells' percentages took. The percentage sets the
   cell's border box, padding and borders included, whatever its
   box-sizing; the caller raises it to the cell's content minimum. */
static int flex_table_percent_cap(LayoutContext *context,
                                  const ComputedStyle *style,
                                  int content_width, int *used, int width)
{
    if (!style->has_width || !style->width_percent) return width;
    int percent = resolve_declared_length(
        context->sheet, style->width, true, content_width);
    if (percent < 0) percent = 0;
    int left = layout_subtract_coordinate(content_width, *used);
    if (left < 0) left = 0;
    int kept = percent < left ? percent : left;
    *used = layout_add_coordinate(*used, kept);
    return kept;
}

/* A percentage cell's share of a CSS table row's leftover width: its
   percentage of the row, without padding or borders. */
static int flex_table_percent_weight(LayoutContext *context,
                                     const ComputedStyle *style,
                                     int content_width)
{
    int width = resolve_declared_length(
        context->sheet, style->width, true, content_width);
    return width > 0 ? width : 0;
}

/* border-spacing of a display:table box in the separated borders model;
   false when it has none. */
static bool flex_css_table_spacing(LayoutContext *context,
                                   const ComputedStyle *style,
                                   int *spacing_x, int *spacing_y)
{
    if (style->display != DISPLAY_TABLE || style->table_border_collapse) {
        return false;
    }
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(style));
    if (paint == NULL
        || (paint->components & STYLE_PAINT_COMPONENT_TABLE_SPACING) == 0
        || (paint->table_spacing_x == 0 && paint->table_spacing_y == 0)) {
        return false;
    }
    *spacing_x = paint->table_spacing_x;
    *spacing_y = paint->table_spacing_y;
    return true;
}

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
    bool table_row = frame->table_row;
    bool css_table_row = frame->css_table_row;
    bool anonymous_cell_row = frame->anonymous_cell_row;
    bool reverse_row = frame->reverse_row != computed_style_direction_rtl(style);
    JustifyContent justify = style_align_content_in_axis(style->justify_content,
                                                        frame->reverse_row);
    if (justify == JUSTIFY_LEFT) justify = reverse_row ? JUSTIFY_END : JUSTIFY_START;
    if (justify == JUSTIFY_RIGHT) justify = reverse_row ? JUSTIFY_START : JUSTIFY_END;
    size_t flex_children = 0;
    size_t css_table_auto_children = 0;
    size_t total_main_auto_margins = 0;
    int total_basis = 0;
    int total_hypothetical_basis = 0;
    /* The bases with each inflexible item (flex-grow 0) frozen at its
       hypothetical size, the free space growth distributes (Flexbox
       9.7 step 3). */
    int total_grow_frozen_basis = 0;
    int total_grow = 0;
    int anonymous_percentage_width = 0;
    /* A CSS table row narrower than its cells' specified widths: the sum
       of the content minimums, what the percentage cells and the fixed
       cells ask for beyond theirs (CSS Tables 3 "min-content-percentage"
       and "min-content-specified" guesses), and the percentage of the row
       the cells so far have taken (percentages past 100% are cut). */
    int table_content_minimum = 0;
    int table_percent_extra = 0;
    int table_fixed_extra = 0;
    size_t table_fixed_cells = 0;
    size_t table_percent_cells = 0;
    int table_percent_used = 0;
    /* A CSS table row without auto cells hands its leftover width to the
       fixed cells in proportion to their widths, or with no fixed cells to
       the percentage cells in proportion to their percentages. */
    long long table_fixed_weight = 0;
    long long table_percent_weight = 0;
    /* Auto cells share a CSS table row's leftover width in proportion to
       their max-content widths (CSS Tables 3, as Chromium does). */
    long long table_auto_weight = 0;
    long long total_shrink_weight = 0;
    int total_shrink_factor = 0;
    bool planned = false;
    FlexOrderPlan *row_order = &scratch->row_order;
    *row_order = (FlexOrderPlan) {0};
    /* A wrapping row sizes each line from that line's own items
       (flex_line_metrics below); the whole-container totals the pre-scan
       measures are only read by a single-line row. Measuring every item's
       basis and min-content here walked the subtree of every card of a
       long wrapping list, and a load preview could not stop at its depth
       limit before it. */
    bool line_sized = style->flex_wrap && !table_row && !css_table_row
        && !anonymous_cell_row;
    if (flex_row && !replaced_content) {
        if (!flex_order_plan_build(row_order, context, node, style)) {
            return false;
        }
        size_t count = row_order->item_count;
        if (!style->flex_wrap && !table_row && !css_table_row
            && !anonymous_cell_row && count && count <= FLEX_MAIN_LIMIT) {
            row_order->main_items = budget_malloc_category(row_order->budget,
                BUDGET_CATEGORY_LAYOUT, count * sizeof(*row_order->main_items));
            planned = row_order->main_items != NULL;
        }
        FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
        FlatItem *item = &scratch->traversal.flex.item;
        flex_iterator_init(iterator, context, node, style, row_order);
        /* The plan already counted in-flow cells. Only the table owns
           their widths; re-scanning for flex growth both costs a walk
           and applies a flex declaration to a non-flex item. */
        if (table_row) flex_children = count;
        while (!table_row && flex_iterator_next(iterator, item)) {
            if (item->style.out_of_flow || item->style.fixed_position) continue;
            flex_children++;
            if (css_table_row && !item->style.has_width) {
                css_table_auto_children++;
            }
            if (line_sized) continue;
            bool previous_intrinsic_pair_mode =
                context->intrinsic_pair_mode;
            context->intrinsic_pair_mode = true;
            if (planned && flex_children <= count) {
                flex_measure_row_main(context, item, &row_order->main_items[flex_children - 1u],
                    content_width, child_containing_height,
                    frame->definite_height
                    && flex_item_alignment(style, &item->style) == ALIGN_STRETCH);
                total_main_auto_margins += item->style.margin_left_auto
                    + item->style.margin_right_auto;
                context->intrinsic_pair_mode = previous_intrinsic_pair_mode;
                continue;
            }
            int child_basis = flex_child_basis(context, item, content_width, child_containing_height);
            int child_minimum = flex_child_row_minimum(
                context, item, content_width, css_table_row);
            context->intrinsic_pair_mode = previous_intrinsic_pair_mode;
            if (css_table_row) {
                bool fixed = item->style.has_width
                    && !item->style.width_percent;
                child_basis = flex_table_percent_cap(
                    context, &item->style, content_width,
                    &table_percent_used, child_basis);
                int extra = item->style.has_width
                    && child_basis > child_minimum
                    ? child_basis - child_minimum : 0;
                table_content_minimum = layout_add_coordinate(
                    table_content_minimum, child_minimum);
                if (fixed) {
                    table_fixed_extra = layout_add_coordinate(
                        table_fixed_extra, extra);
                    table_fixed_cells++;
                } else if (item->style.has_width) {
                    table_percent_extra = layout_add_coordinate(
                        table_percent_extra, extra);
                    table_percent_cells++;
                }
            }
            if (css_table_row && item->style.has_width
                && !item->style.width_percent) {
                /* A fixed cell's width is its preferred width; content
                   wider than it widens the cell. */
                if (child_basis < child_minimum) child_basis = child_minimum;
                child_minimum = child_basis;
            }
            if (css_table_row && child_basis < child_minimum) {
                child_basis = child_minimum;
            }
            if (css_table_row && !item->style.has_width) {
                table_auto_weight += child_basis;
            }
            if (css_table_row && item->style.has_width) {
                if (item->style.width_percent) {
                    table_percent_weight += flex_table_percent_weight(
                        context, &item->style, content_width);
                } else {
                    table_fixed_weight += child_basis;
                }
            }
            if (anonymous_cell_row && item->style.has_width
                && item->style.width_percent) {
                int percent_width = resolve_declared_length(
                    context->sheet, item->style.width, true, content_width);
                if (percent_width > 0) {
                    /* A percentage column constrains the shrink-to-fit
                       table, not just the cell. E.g. a 1% submit column
                       with a nonempty button pushes the table to the
                       available width; the auto input receives the rest. */
                    int required = child_basis >= percent_width
                        ? content_width : tilefinch_mul_div_int(
                            child_basis, content_width, percent_width);
                    if (required > anonymous_percentage_width)
                        anonymous_percentage_width = required;
                }
            }
            total_basis = layout_add_coordinate(total_basis, child_basis);
            total_hypothetical_basis = layout_add_coordinate(
                total_hypothetical_basis,
                child_basis < child_minimum ? child_minimum : child_basis);
            total_grow_frozen_basis = layout_add_coordinate(
                total_grow_frozen_basis,
                item->style.flex_grow == 0 && child_basis < child_minimum
                    ? child_minimum : child_basis);
            total_grow = layout_add_coordinate(
                total_grow, item->style.flex_grow);
            total_main_auto_margins += item->style.margin_left_auto + item->style.margin_right_auto;
            if (item->style.flex_shrink > 0) {
                int shrink_capacity = child_basis - child_minimum;
                if (shrink_capacity > 0) {
                    /* CSS Flexbox scales flex-shrink by the outer flex base
                       size. The minimum is a clamp, not the weighting input;
                       using remaining capacity here made equal bases shrink
                       unequally when their text minima differed. */
                    total_shrink_weight += (long long) child_basis
                                           * item->style.flex_shrink;
                    total_shrink_factor = layout_add_coordinate(
                        total_shrink_factor, item->style.flex_shrink);
                }
            }
        }
    }
    if (flex_row && flex_children != 0 && !replaced_content) {
        layout_flush_line(line);
        const TableTracks *table_tracks = table_row && context->placing_table_row == node
            ? context->placing_row_tracks : NULL;
        /* The anonymous row of a separated-borders display:table box keeps
           border-spacing around and between its cells, as an explicit row
           takes it from its table's tracks. */
        int css_spacing_x = 0, css_spacing_y = 0;
        bool css_table_spacing = css_table_row && !anonymous_cell_row
            && flex_css_table_spacing(context, style, &css_spacing_x,
                                      &css_spacing_y);
        int gaps = table_row ? 0 : css_table_spacing
            ? layout_clamp_coordinate(
                  (int64_t) css_spacing_x * (int64_t) (flex_children + 1u))
            : layout_clamp_coordinate(
                  (int64_t) style->gap * (int64_t) (flex_children - 1u));
        planned = planned && flex_children <= row_order->item_count;
        if (planned) {
            total_basis = flex_resolve_main(row_order->main_items, flex_children,
                layout_subtract_coordinate(content_width, gaps));
            total_grow = 0;
            total_shrink_weight = 0;
        }
        if (!planned && !table_row
            && layout_add_coordinate(total_hypothetical_basis, gaps)
                   > content_width) {
            /* A zero flex base can already be frozen at its content minimum.
               Reserve that space before distributing shrink to its siblings,
               even when the unclamped bases would fit the row. */
            total_basis = total_hypothetical_basis;
        } else if (!planned && !table_row) {
            /* A zero flex base with a content minimum (flex: 0 1 0%) does
               not grow, so its siblings share only what it leaves. */
            total_basis = total_grow_frozen_basis;
        }
        int remaining = layout_subtract_coordinate(
            layout_subtract_coordinate(content_width, total_basis), gaps);
        /* A specified cell width is the column's preferred width, not a
           floor (CSS Tables 3 column minimums are the content minimums).
           When the row cannot give every specified cell its width, auto
           cells keep their content minimum; the percentage cells grow from
           theirs toward their percentage widths first, then the fixed cells
           toward their specified widths, each in proportion to what it asks
           for beyond its minimum. */
        int table_room = layout_subtract_coordinate(content_width, gaps);
        int table_percent_guess = layout_add_coordinate(
            table_content_minimum, table_percent_extra);
        bool table_squeezed = css_table_row
            && table_fixed_cells + table_percent_cells != 0
            && table_room < layout_add_coordinate(
                   table_percent_guess, table_fixed_extra);
        bool squeeze_percent = table_squeezed
            && table_room < table_percent_guess;
        int squeeze_room = !table_squeezed ? 0
            : layout_subtract_coordinate(
                  table_room, squeeze_percent ? table_content_minimum
                                              : table_percent_guess);
        if (squeeze_room < 0) squeeze_room = 0;
        int squeeze_span = squeeze_percent ? table_percent_extra
                                           : table_fixed_extra;
        int squeeze_seen = 0;
        int table_percent_seen = 0;
        /* No auto cell takes the leftover width: give it to the fixed
           cells, else to the percentage cells (CSS Tables 3 distribution,
           as Chromium does). An anonymous-cell row shrink-wraps instead. */
        bool table_leftover_to_specified = css_table_row
            && !anonymous_cell_row && css_table_auto_children == 0
            && !table_squeezed && remaining > 0
            && (table_fixed_weight > 0 || table_percent_weight > 0);
        long long leftover_seen = 0;
        const int shrink_remaining = total_shrink_factor < 512
            ? tilefinch_mul_div_int(remaining, total_shrink_factor, 512)
            : remaining;
        if (anonymous_cell_row && remaining > 0) {
            /* The anonymous table wrapper around an otherwise complete run
               of table-cell children shrink-wraps its row. The containing
               block itself still retains its ordinary used width. */
            int required_extra = anonymous_percentage_width - total_basis - gaps;
            remaining = required_extra > 0 ? required_extra : 0;
        }
        int cursor_x = content_x;
        /* Only baseline-aligned children with actual inline formatting need
           this temporary side table. Keep retained box sizes unchanged. */
        size_t baseline_limit = flex_children < 128u ? flex_children : 128u;
        bool baseline_allocation_tried = false;
        if (table_tracks != NULL && table_tracks->border_collapse) {
            cursor_x = layout_add_coordinate(
                cursor_x, table_tracks->collapsed_left_gutter);
        } else if (table_tracks != NULL) {
            cursor_x = layout_add_coordinate(
                cursor_x, table_tracks->spacing_x);
        }
        if (css_table_spacing) {
            cursor_x = layout_add_coordinate(cursor_x, css_spacing_x);
        }
        int flex_gap = table_row ? 0
                       : css_table_spacing ? css_spacing_x : style->gap;
        long long css_table_auto_seen = 0;
        trace_flex_sizing(context, "container", node, NULL, NULL,
                          content_width, total_basis, remaining, cursor_x,
                          0, 0);
        if (total_grow == 0 && total_main_auto_margins == 0)
            cursor_x = layout_add_coordinate(cursor_x, layout_content_offset(
                justify, remaining, (int) flex_children, &flex_gap));
        int cursor_y = line->y;
        if (table_row && table_tracks != NULL
            && !table_tracks->border_collapse) {
            cursor_y = layout_add_coordinate(
                cursor_y, table_tracks->spacing_y);
        } else if (css_table_spacing) {
            cursor_y = layout_add_coordinate(cursor_y, css_spacing_y);
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
        size_t main_index = 0;
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
                /* A load preview stops at the first line that starts below
                   its depth limit: later lines cannot move earlier ones. */
                if (!first_flex_line
                    && layout_preview_limit_reached(
                           context, layout_add_coordinate(
                                        row_bottom, style->row_gap))) break;
                FlexLineMetrics metrics = flex_line_metrics(
                    context, item_start,
                    &scratch->traversal.flex.lookahead_item,
                    content_width, child_containing_height, style->gap);
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
                if (line_total_grow == 0 && line_auto_margins == 0)
                    cursor_x = layout_add_coordinate(cursor_x, layout_content_offset(
                        justify, line_remaining, (int) metrics.count, &flex_gap));
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
            } else if (table_row) {
                child_width = content_width / (int) flex_children;
            } else if (planned && main_index < flex_children) {
                child_width = row_order->main_items[main_index++].result;
            } else {
                child_width = flex_child_basis(context, item,
                                                content_width, child_containing_height);
            }
            if (css_table_row) {
                int table_minimum = flex_child_row_minimum(
                    context, item, content_width, true);
                bool fixed = child_style->has_width
                    && !child_style->width_percent;
                bool percent = child_style->has_width
                    && child_style->width_percent;
                child_width = flex_table_percent_cap(
                    context, child_style, content_width,
                    &table_percent_seen, child_width);
                if (table_squeezed) {
                    int extra = child_style->has_width
                        && child_width > table_minimum
                        ? child_width - table_minimum : 0;
                    if (percent && !squeeze_percent) {
                        /* Past the percentage guess: full percentage. */
                        child_width = layout_add_coordinate(
                            table_minimum, extra);
                    } else {
                        if (percent != squeeze_percent) extra = 0;
                        int before = squeeze_span > 0
                            ? tilefinch_mul_div_int(
                                  squeeze_room, squeeze_seen, squeeze_span)
                            : 0;
                        squeeze_seen = layout_add_coordinate(
                            squeeze_seen, extra);
                        int after = squeeze_span > 0
                            ? tilefinch_mul_div_int(
                                  squeeze_room, squeeze_seen, squeeze_span)
                            : 0;
                        child_width = layout_add_coordinate(
                            table_minimum, after - before);
                    }
                } else {
                    if (fixed && child_width > table_minimum) {
                        table_minimum = child_width;
                    }
                    if (child_width < table_minimum) {
                        child_width = table_minimum;
                    }
                }
            }
            if (table_leftover_to_specified && child_style->has_width) {
                bool percent_share = table_fixed_weight == 0;
                long long weight = child_style->width_percent != percent_share
                    ? 0
                    : percent_share
                        ? flex_table_percent_weight(
                              context, child_style, content_width)
                        : child_width;
                long long total = percent_share ? table_percent_weight
                                                : table_fixed_weight;
                int before = (int) ((long long) remaining * leftover_seen
                                    / total);
                leftover_seen += weight;
                int after = (int) ((long long) remaining * leftover_seen
                                   / total);
                child_width = layout_add_coordinate(
                    child_width, after - before);
            }
            if (css_table_row && remaining > 0
                && !child_style->has_width
                && css_table_auto_children != 0) {
                bool weighted = table_auto_weight > 0;
                long long total = weighted
                    ? table_auto_weight
                    : (long long) css_table_auto_children;
                int before = (int) ((long long) remaining
                                    * css_table_auto_seen / total);
                css_table_auto_seen += weighted ? child_width : 1;
                int after = (int) ((long long) remaining
                                   * css_table_auto_seen / total);
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
                        available_grow < 1000 ? 1000 : available_grow));
            } else if (remaining < 0 && !style->flex_wrap && !table_squeezed
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
                int before = (int) ((long long) shrink_remaining
                                    * shrink_weight_seen
                                    / total_shrink_weight);
                int after = (int) ((long long) shrink_remaining * next_weight
                                   / total_shrink_weight);
                child_width = layout_add_coordinate(
                    child_width, after - before);
                if (child_width < child_minimum) child_width = child_minimum;
                shrink_weight_seen = next_weight;
            }
            int used_minimum = table_row || planned || table_squeezed
                ? 0 : flex_child_row_minimum(
                    context, item, content_width, css_table_row);
            if (css_table_row && !table_squeezed && child_style->has_width
                && !child_style->width_percent) {
                used_minimum = child_basis;
            }
            if (child_width < used_minimum) child_width = used_minimum;
            if (child_width < 0) child_width = 0;
            trace_flex_sizing(context, "assigned", node, item->node,
                              child_style, content_width, total_basis,
                              remaining, cursor_x, child_basis, child_width);
            int child_bottom = cursor_y;
            size_t command_start = context->layout->count;
            if (item->generated_pseudo != PSEUDO_NONE) {
                if (!layout_generated_flex_item(context, item, cursor_x,
                    cursor_y, child_width, content_width, child_containing_height,
                    &child_bottom)) {
                    flex_order_plan_destroy(row_order);
                    return false;
                }
            } else if (item->anonymous_text) {
                if (!layout_anonymous_text(context, item, node, cursor_x,
                                           cursor_y, child_width,
                                           &child_bottom)) {
                    flex_order_plan_destroy(row_order);
                    return false;
                }
            } else {
                /* The flex container's content box is the item's containing
                   block: percentage max-/min-width, padding and margins
                   resolve against it, not against the main size the line
                   assigned (CSS Flexbox 9.2 / CSS 2.1 10.1). Guardian's
                   section title is a `max-width: 74%` flex item; 74% of
                   its own 107 px basis wrapped "Opinion". */
                lxb_dom_node_t *saved_basis_node =
                    context->percentage_basis_node;
                int saved_basis_width = context->percentage_basis_width;
                lxb_dom_node_t *saved_flex_node = context->assigned_flex_node;
                int saved_flex_height = context->assigned_flex_height;
                bool saved_flex_minimum = context->assigned_flex_minimum;
                if (!table_row && !css_table_row && !anonymous_cell_row) {
                    context->percentage_basis_node = item->node;
                    context->percentage_basis_width = content_width;
                    /* In a definite, single-line row, a stretched item's
                       cross size is definite too (Flexbox 9.8). Assign it
                       before descending so percentage-height children use
                       the stretched box, not its provisional natural height.
                       Wrapped/auto-height lines still need the later line
                       measurement and must not inherit this constraint. */
                    if (frame->definite_height && !style->flex_wrap
                        && !child_style->has_height
                        /* Native editable controls have a separate late
                           cross-size/text-centering path and no ordinary
                           percentage-height child formatting context. */
                        && !document_node_is_tag(item->node, LXB_TAG_INPUT)
                        && !document_node_is_tag(item->node, LXB_TAG_TEXTAREA)
                        && !child_style->margin_top_auto
                        && !child_style->margin_bottom_auto
                        && flex_item_alignment(style, child_style)
                           == ALIGN_STRETCH) {
                        int height = child_containing_height
                            - child_style->margin.top
                            - child_style->margin.bottom;
                        if (!child_style->box_sizing_border_box)
                            height -= child_style->padding.top
                                + child_style->padding.bottom
                                + child_style->border.top
                                + child_style->border.bottom;
                        if (height > 0) {
                            context->assigned_flex_node = item->node;
                            context->assigned_flex_height = height;
                            context->assigned_flex_minimum = false;
                        }
                    }
                }
                bool laid_out = layout_block(
                    context, item->node, &item->parent_style, cursor_x,
                    cursor_y, child_width, child_containing_height, true,
                    descendant_positioned_box, &child_bottom);
                if (laid_out && context->completed_baseline_node == item->node
                    && flex_item_alignment(style, child_style) == ALIGN_BASELINE
                    && row_order->baseline_count < baseline_limit) {
                    if (!baseline_allocation_tried) {
                        baseline_allocation_tried = true;
                        row_order->baselines = budget_malloc_category(row_order->budget,
                            BUDGET_CATEGORY_LAYOUT, baseline_limit * sizeof(FlexBaseline));
                    }
                    if (row_order->baselines != NULL) {
                        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                                      item->node);
                        if (box != NULL) {
                            row_order->baselines[row_order->baseline_count++] =
                                (FlexBaseline){ item->node,
                                    context->completed_first_baseline - box->y };
                        }
                    }
                }
                context->percentage_basis_node = saved_basis_node;
                context->percentage_basis_width = saved_basis_width;
                context->assigned_flex_node = saved_flex_node;
                context->assigned_flex_height = saved_flex_height;
                context->assigned_flex_minimum = saved_flex_minimum;
                if (!laid_out) {
                    flex_order_plan_destroy(row_order);
                    return false;
                }
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
        if (!table_row && !css_table_row && !anonymous_cell_row && reverse_row)
            reverse_flex_row_items(context, node, style, row_order, content_x, content_width);
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
            const LayoutNodeBox *aligned_box = flex_item_box(
                context->layout, aligned_item);
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
        line->y = css_table_spacing
            ? layout_add_coordinate(row_bottom, css_spacing_y) : row_bottom;
        line_cursor_set(line, line->start_x);
        line->line_height = 0;
        line->line_height_fixed = 0;
        line->y_fixed_valid = false;
    }
    flex_order_plan_destroy(row_order);
    return true;
}
