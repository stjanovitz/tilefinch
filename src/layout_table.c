/* Table layout: cell spans, row measurement, and column tracks.
   Split out of layout.c. */

#include "layout_internal.h"
#include "tilefinch/integer_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void translate_table_cell_contents(LayoutDocument *layout,
                                          lxb_dom_node_t *node, int dy)
{
    if (layout == NULL || node == NULL || dy == 0) return;
    LayoutNodeBox *root = NULL;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].node == node) {
            root = &layout->node_boxes[i];
            break;
        }
    }
    if (root == NULL) return;
    size_t command_start = root->scroll_command_start;
    size_t command_end = root->scroll_command_end;
    if (command_start > layout->count) command_start = layout->count;
    if (command_end > layout->count) command_end = layout->count;
    for (size_t i = command_start; i < command_end; i++) {
        layout->commands[i].y += dy;
    }
    for (size_t i = 0; i < layout->link_count; i++) {
        if (layout->links[i].node != node
            && layout_node_within(layout->links[i].node, node)) {
            layout->links[i].y += dy;
        }
    }
    for (size_t i = 0; i < layout->control_count; i++) {
        if (layout->controls[i].node != node
            && layout_node_within(layout->controls[i].node, node)) {
            layout->controls[i].y += dy;
        }
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        if (box->node != node && layout_node_within(box->node, node)) {
            box->y += dy;
        }
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *box = &layout->node_boxes[i];
        if (box->positioned_ancestor_distance == 0 || box->node == node
            || !layout_node_within(box->node, node)) continue;
        bool nested_out_of_flow = false;
        for (lxb_dom_node_t *ancestor = box->node->parent;
             ancestor != NULL && ancestor != node;
             ancestor = ancestor->parent) {
            const LayoutNodeBox *ancestor_box = layout_box_for_node(
                layout, ancestor);
            if (ancestor_box != NULL
                && ancestor_box->positioned_ancestor_distance != 0) {
                nested_out_of_flow = true;
                break;
            }
        }
        if (!nested_out_of_flow) {
            translate_node_subtree(layout, box->node, 0, -dy);
        }
    }
}


size_t table_cell_span(lxb_dom_node_t *cell)
{
    size_t length = 0;
    const char *text = document_attribute(cell, "colspan", &length);
    if (text == NULL || length == 0 || length > 4) return 1;
    unsigned value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 1;
        value = value * 10u + (unsigned) (text[i] - '0');
    }
    if (value < 1) return 1;
    return value > 16 ? 16 : value;
}

size_t table_cell_row_span(lxb_dom_node_t *cell)
{
    size_t length = 0;
    const char *text = document_attribute(cell, "rowspan", &length);
    if (text == NULL || length == 0 || length > 5) return 1;
    unsigned value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 1;
        value = value * 10u + (unsigned) (text[i] - '0');
    }
    /* HTML's zero value means "through the end of the row group". The
       handheld layout has a documented 64-row cooperative table bound, so
       use that same finite horizon rather than retaining an unbounded span. */
    if (value == 0 || value > 64) return 64;
    return value;
}

#define TABLE_PLACEMENT_LIMIT 512u

static size_t table_bounded_span_attribute(
    lxb_dom_node_t *node, const char *name);

static size_t table_declared_column_count(lxb_dom_node_t *table)
{
    size_t count = 0;
    for (lxb_dom_node_t *child = table->first_child;
         child != NULL && count < 16; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (layout_node_name_is(child, "col")) {
            count += table_bounded_span_attribute(child, "span");
            continue;
        }
        if (!layout_node_name_is(child, "colgroup")) continue;
        size_t nested = 0;
        for (lxb_dom_node_t *col = child->first_child;
             col != NULL && count + nested < 16; col = col->next) {
            if (col->type == LXB_DOM_NODE_TYPE_ELEMENT
                && layout_node_name_is(col, "col")) {
                nested += table_bounded_span_attribute(col, "span");
            }
        }
        count += nested != 0
            ? nested : table_bounded_span_attribute(child, "span");
    }
    return count > 16 ? 16 : count;
}

static bool table_build_placements(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, TableCellPlacement **placements_out,
    size_t *placement_count_out, size_t *column_count_out,
    bool *has_row_spans)
{
    TableCellPlacement *placements = NULL;
    size_t count = 0, capacity = 0, columns = 0;
    size_t declared_columns = table_declared_column_count(table);
    uint8_t occupied[16] = {0};
    FlatItemIterator rows;
    FlatItem row;
    flat_iterator_init(&rows, context, table, table_style);
    while (flat_iterator_next(&rows, &row)) {
        if (row.anonymous_text
            || row.style.display != DISPLAY_TABLE_ROW) continue;
        if (!layout_cooperate(context, row.node)) {
            budget_free(context->layout->budget, placements);
            return false;
        }
        for (size_t i = 0; i < 16; i++) {
            if (occupied[i] != 0) occupied[i]--;
        }
        size_t cursor = 0;
        ComputedStyle row_style = layout_style_for_node(
            context, row.node, &row.parent_style);
        for (lxb_dom_node_t *cell = row.node->first_child;
             cell != NULL; cell = cell->next) {
            if (cell->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            ComputedStyle cell_style = layout_style_for_node(
                context, cell, &row_style);
            if (cell_style.display != DISPLAY_TABLE_CELL) continue;
            size_t span = table_cell_span(cell);
            size_t column = cursor;
            while (column < 16) {
                while (column < 16 && occupied[column] != 0) column++;
                if (column >= 16) break;
                size_t available = 0;
                while (column + available < 16
                       && occupied[column + available] == 0
                       && available < span) {
                    available++;
                }
                if (available == span) break;
                column += available + 1;
            }
            if (column >= 16) break;
            if (span > 1 && declared_columns > column
                && span > declared_columns - column) {
                span = declared_columns - column;
            }
            if (span > 16 - column) span = 16 - column;
            size_t row_span = table_cell_row_span(cell);
            if (row_span > 1) *has_row_spans = true;
            if (count < TABLE_PLACEMENT_LIMIT) {
                if (count == capacity) {
                    size_t next = capacity == 0 ? 32 : capacity * 2;
                    if (next > TABLE_PLACEMENT_LIMIT) {
                        next = TABLE_PLACEMENT_LIMIT;
                    }
                    TableCellPlacement *grown = budget_realloc(
                        context->layout->budget, placements,
                        next * sizeof(*grown));
                    if (grown == NULL) {
                        budget_free(context->layout->budget, placements);
                        return false;
                    }
                    placements = grown;
                    capacity = next;
                }
                placements[count++] = (TableCellPlacement) {
                    .cell = cell,
                    .column = (uint8_t) column,
                    .column_span = (uint8_t) span,
                    .row_span = (uint8_t) row_span
                };
            }
            for (size_t i = 0; i < span; i++) {
                occupied[column + i] = (uint8_t) row_span;
            }
            cursor = column + span;
            if (cursor > columns) columns = cursor;
        }
    }
    *placements_out = placements;
    *placement_count_out = count;
    *column_count_out = columns;
    return true;
}

bool table_cell_placement(const TableTracks *tracks, lxb_dom_node_t *cell,
                          size_t *column, size_t *column_span,
                          size_t *row_span)
{
    if (tracks == NULL || cell == NULL) return false;
    for (size_t i = 0; i < tracks->placement_count; i++) {
        const TableCellPlacement *placement = &tracks->placements[i];
        if (placement->cell != cell) continue;
        if (column != NULL) *column = placement->column;
        if (column_span != NULL) *column_span = placement->column_span;
        if (row_span != NULL) *row_span = placement->row_span;
        return true;
    }
    return false;
}

static const TableCellPlacement *table_find_placement(
    const TableCellPlacement *placements, size_t placement_count,
    lxb_dom_node_t *cell)
{
    for (size_t i = 0; i < placement_count; i++) {
        if (placements[i].cell == cell) return &placements[i];
    }
    return NULL;
}

static void table_measure_row(LayoutContext *context, lxb_dom_node_t *row,
                              const ComputedStyle *row_parent,
                              int available_width, bool border_collapse,
                              unsigned border_spacing_x,
                              const TableCellPlacement *placements,
                              size_t placement_count,
                              int preferred[16],
                              int minimum[16],
                              size_t *column_count)
{
    size_t column = 0;
    ComputedStyle row_style = layout_style_for_node(context, row, row_parent);
    for (lxb_dom_node_t *cell = row->first_child; cell != NULL;
         cell = cell->next) {
        if (cell->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        ComputedStyle cell_style = layout_style_for_node(context, cell,
                                                         &row_style);
        if (cell_style.display != DISPLAY_TABLE_CELL) continue;
        size_t span = table_cell_span(cell);
        const TableCellPlacement *placement = table_find_placement(
            placements, placement_count, cell);
        if (placement != NULL) {
            column = placement->column;
            span = placement->column_span;
        }
        if (column >= 16) break;
        if (span > 16 - column) span = 16 - column;
        int wanted = 0;
        int floor = 0;
        intrinsic_text_widths(
            context, cell, &row_style, available_width, &wanted, &floor);
        resolve_padding(context->sheet, &cell_style, available_width);
        if (cell_style.has_width) {
            int authored = resolve_declared_length(
                context->sheet, cell_style.width,
                cell_style.width_percent, available_width);
            if (!cell_style.box_sizing_border_box) {
                authored += cell_style.padding.left + cell_style.padding.right
                            + cell_style.border.left + cell_style.border.right;
            }
            if (authored > wanted) wanted = authored;
            if (authored > floor) floor = authored;
        }
        if (border_collapse) {
            /* Only the inward half of a collapsed border belongs to the
               column track. Apply this after authored-width handling too:
               CSS cell widths include the full border box, while the table
               reserves the two outward halves as its outer gutters. */
            int outward = cell_style.border.left / 2
                          + cell_style.border.right / 2;
            wanted = wanted > outward ? wanted - outward : 0;
            floor = floor > outward ? floor - outward : 0;
        } else if (span > 1 && border_spacing_x != 0) {
            int internal = (int) border_spacing_x * ((int) span - 1);
            wanted = wanted > internal ? wanted - internal : 0;
            floor = floor > internal ? floor - internal : 0;
        }
        if (wanted < (int) span * 8) wanted = (int) span * 8;
        if (floor < (int) span * 8) floor = (int) span * 8;
        int per_track = (wanted + (int) span - 1) / (int) span;
        int floor_per_track = (floor + (int) span - 1) / (int) span;
        for (size_t i = 0; i < span; i++) {
            if (per_track > preferred[column + i]) {
                preferred[column + i] = per_track;
            }
            if (floor_per_track > minimum[column + i]) {
                minimum[column + i] = floor_per_track;
            }
        }
        column += span;
    }
    if (column > *column_count) *column_count = column;
}

static void table_collapsed_row_boundaries(
    LayoutContext *context, lxb_dom_node_t *row,
    const ComputedStyle *row_parent, int boundaries[17],
    int first_width[17], bool seen[17], bool *conflict,
    const TableCellPlacement *placements, size_t placement_count,
    int *row_top, int *row_bottom)
{
    size_t column = 0;
    size_t visited = 0;
    ComputedStyle row_style = layout_style_for_node(context, row, row_parent);
    for (lxb_dom_node_t *cell = row->first_child;
         cell != NULL && column < 16 && visited < 256;
         cell = cell->next, visited++) {
        if (cell->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        ComputedStyle cell_style = layout_style_for_node(
            context, cell, &row_style);
        if (cell_style.display != DISPLAY_TABLE_CELL) continue;
        size_t span = table_cell_span(cell);
        const TableCellPlacement *placement = table_find_placement(
            placements, placement_count, cell);
        if (placement != NULL) {
            column = placement->column;
            span = placement->column_span;
        }
        if (span > 16 - column) span = 16 - column;
        int left_width = cell_style.border.left;
        int right_width = cell_style.border.right;
        int top = cell_style.border.top / 2;
        int bottom = cell_style.border.bottom
                     - cell_style.border.bottom / 2;
        if (top > *row_top) *row_top = top;
        if (bottom > *row_bottom) *row_bottom = bottom;
        int left = left_width / 2;
        int right = right_width / 2;
        if (left > boundaries[column]) boundaries[column] = left;
        if (right > boundaries[column + span]) {
            boundaries[column + span] = right;
        }
        if (seen[column] && first_width[column] != left_width) {
            *conflict = true;
        } else if (!seen[column]) {
            seen[column] = true;
            first_width[column] = left_width;
        }
        if (seen[column + span]
            && first_width[column + span] != right_width) {
            *conflict = true;
        } else if (!seen[column + span]) {
            seen[column + span] = true;
            first_width[column + span] = right_width;
        }
        column += span;
    }
}

static bool table_collapsed_boundaries(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int boundaries[17],
    const TableCellPlacement *placements, size_t placement_count,
    lxb_dom_node_t **first_row, lxb_dom_node_t **last_row,
    int *top_gutter, int *bottom_gutter)
{
    if (!table_style->table_border_collapse) return false;
    int first_width[17] = {0};
    bool seen[17] = {false};
    bool conflict = false;
    FlatItemIterator rows;
    FlatItem row;
    size_t measured_rows = 0;
    flat_iterator_init(&rows, context, table, table_style);
    while (flat_iterator_next(&rows, &row)) {
        if (row.anonymous_text
            || row.style.display != DISPLAY_TABLE_ROW) continue;
        if (context->preview_y_limit > 0
            && measured_rows >= LAYOUT_PREVIEW_TABLE_ROW_LIMIT) {
            context->preview_truncated = true;
            break;
        }
        if (!layout_cooperate(context, row.node)) break;
        int row_top = 0, row_bottom = 0;
        table_collapsed_row_boundaries(
            context, row.node, &row.parent_style, boundaries,
            first_width, seen, &conflict, placements, placement_count,
            &row_top, &row_bottom);
        if (measured_rows == 0) {
            if (first_row != NULL) *first_row = row.node;
            if (top_gutter != NULL) *top_gutter = row_top;
        }
        if (last_row != NULL) *last_row = row.node;
        if (bottom_gutter != NULL) *bottom_gutter = row_bottom;
        measured_rows++;
    }
    /* Conflict detection is retained for diagnostics and future colour/style
       arbitration, but collapsed geometry applies even when every adjacent
       edge happens to have the same authored width. Returning `conflict`
       here made the ordinary uniform-border case behave as `separate`. */
    (void) conflict;
    return true;
}

static size_t table_row_cache_home(lxb_dom_node_t *row, int available_width)
{
    size_t hash = layout_pointer_hash(row);
    hash ^= (size_t) (unsigned) available_width * (size_t) 0x9e3779b1u;
    return hash & (LAYOUT_REUSE_TABLE_ROW_CAPACITY - 1u);
}

static bool table_row_cache_get(LayoutContext *context, lxb_dom_node_t *row,
                                int available_width, int preferred[16],
                                int minimum[16], size_t *column_count)
{
    if (context->reuse == NULL) return false;
    if (context->reuse->table_rows == NULL) {
        context->reuse->stats.table_row_misses++;
        return false;
    }
    size_t home = table_row_cache_home(row, available_width);
    for (size_t probe = 0; probe < LAYOUT_REUSE_TABLE_ROW_PROBE_LIMIT;
         probe++) {
        LayoutReuseTableRowEntry *entry = &context->reuse->table_rows[
            (home + probe) & (LAYOUT_REUSE_TABLE_ROW_CAPACITY - 1u)];
        if (entry->row == row
            && entry->available_width == available_width) {
            memcpy(preferred, entry->preferred, sizeof(entry->preferred));
            memcpy(minimum, entry->minimum, sizeof(entry->minimum));
            *column_count = entry->count;
            entry->stamp = ++context->reuse->clock;
            context->reuse->stats.table_row_hits++;
            return true;
        }
    }
    context->reuse->stats.table_row_misses++;
    return false;
}

static void table_row_cache_put(LayoutContext *context, lxb_dom_node_t *row,
                                int available_width,
                                const int preferred[16],
                                const int minimum[16], size_t column_count)
{
    if (context->reuse == NULL) return;
    if (context->reuse->table_rows == NULL) {
        context->reuse->table_rows = budget_calloc(
            context->reuse->budget, LAYOUT_REUSE_TABLE_ROW_CAPACITY,
            sizeof(*context->reuse->table_rows));
        if (context->reuse->table_rows == NULL) return;
        context->reuse->stats.retained_bytes +=
            LAYOUT_REUSE_TABLE_ROW_CAPACITY
            * sizeof(*context->reuse->table_rows);
    }
    size_t home = table_row_cache_home(row, available_width);
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < LAYOUT_REUSE_TABLE_ROW_PROBE_LIMIT;
         probe++) {
        size_t slot = (home + probe)
                      & (LAYOUT_REUSE_TABLE_ROW_CAPACITY - 1u);
        LayoutReuseTableRowEntry *entry =
            &context->reuse->table_rows[slot];
        if (entry->row == NULL || entry->row == row) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
    LayoutReuseTableRowEntry *entry =
        &context->reuse->table_rows[replacement];
    *entry = (LayoutReuseTableRowEntry) {
        .row = row,
        .available_width = available_width,
        .count = column_count,
        .stamp = ++context->reuse->clock
    };
    memcpy(entry->preferred, preferred, sizeof(entry->preferred));
    memcpy(entry->minimum, minimum, sizeof(entry->minimum));
}

static void table_merge_row(const int row_preferred[16],
                            const int row_minimum[16],
                            size_t row_column_count, int preferred[16],
                            int minimum[16], size_t *column_count)
{
    if (row_column_count > 16) row_column_count = 16;
    for (size_t i = 0; i < row_column_count; i++) {
        if (row_preferred[i] > preferred[i]) {
            preferred[i] = row_preferred[i];
        }
        if (row_minimum[i] > minimum[i]) minimum[i] = row_minimum[i];
    }
    if (row_column_count > *column_count) {
        *column_count = row_column_count;
    }
}

static size_t table_bounded_span_attribute(
    lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    const char *text = document_attribute(node, name, &length);
    if (text == NULL || length == 0 || length > 4) return 1;
    unsigned value = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return 1;
        value = value * 10u + (unsigned) (text[i] - '0');
    }
    if (value < 1) return 1;
    return value > 16 ? 16 : value;
}

static void table_apply_column_constraint(
    LayoutContext *context, ComputedStyle *style, int available_width,
    size_t column, size_t span, int preferred[16], int minimum[16],
    size_t *column_count)
{
    if (column >= 16) return;
    if (span > 16 - column) span = 16 - column;
    if (column + span > *column_count) *column_count = column + span;
    if (!style->has_width || style->width_max_content) return;
    int authored = resolve_declared_length(
        context->sheet, style->width, style->width_percent, available_width);
    if (authored < 0) authored = 0;
    int previous = 0;
    for (size_t i = 0; i < span; i++) {
        int next = tilefinch_mul_div_int(authored, (int) i + 1, (int) span);
        int width = next - previous;
        if (width > preferred[column + i]) preferred[column + i] = width;
        if (width > minimum[column + i]) minimum[column + i] = width;
        previous = next;
    }
}

static void table_measure_columns(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int available_width,
    int preferred[16], int minimum[16], size_t *column_count)
{
    size_t column = 0;
    for (lxb_dom_node_t *child = table->first_child;
         child != NULL && column < 16; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        bool group = layout_node_name_is(child, "colgroup");
        bool single = layout_node_name_is(child, "col");
        if (!group && !single) continue;
        ComputedStyle group_style = layout_style_for_node(
            context, child, table_style);
        if (single) {
            size_t span = table_bounded_span_attribute(child, "span");
            table_apply_column_constraint(
                context, &group_style, available_width, column, span,
                preferred, minimum, column_count);
            column += span;
            continue;
        }
        size_t group_start = column;
        bool have_columns = false;
        for (lxb_dom_node_t *col = child->first_child;
             col != NULL && column < 16; col = col->next) {
            if (col->type != LXB_DOM_NODE_TYPE_ELEMENT
                || !layout_node_name_is(col, "col")) continue;
            have_columns = true;
            size_t span = table_bounded_span_attribute(col, "span");
            ComputedStyle col_style = layout_style_for_node(
                context, col, &group_style);
            table_apply_column_constraint(
                context, &col_style, available_width, column, span,
                preferred, minimum, column_count);
            column += span;
        }
        if (!have_columns) {
            column += table_bounded_span_attribute(child, "span");
            if (column > 16) column = 16;
        }
        table_apply_column_constraint(
            context, &group_style, available_width, group_start,
            column - group_start, preferred, minimum, column_count);
    }
}

static bool table_measure_rows(LayoutContext *context, lxb_dom_node_t *table,
                               const ComputedStyle *table_style,
                               int available_width, int preferred[16],
                               int minimum[16], size_t *column_count,
                               bool collapsed_geometry,
                               const TableCellPlacement *placements,
                               size_t placement_count, bool has_row_spans)
{
    unsigned spacing_x = 0;
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(table_style));
    if (!collapsed_geometry && paint != NULL
        && (paint->components & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
        spacing_x = paint->table_spacing_x;
    }
    FlatItemIterator rows;
    FlatItem row;
    size_t measured_rows = 0;
    flat_iterator_init(&rows, context, table, table_style);
    while (flat_iterator_next(&rows, &row)) {
        if (row.anonymous_text
            || row.style.display != DISPLAY_TABLE_ROW) continue;
        if (context->preview_y_limit > 0
            && measured_rows >= LAYOUT_PREVIEW_TABLE_ROW_LIMIT) {
            context->preview_truncated = true;
            break;
        }
        if (!layout_cooperate(context, row.node)) return false;
        int row_preferred[16] = {0};
        int row_minimum[16] = {0};
        size_t row_column_count = 0;
        if (has_row_spans || !table_row_cache_get(
                context, row.node, available_width, row_preferred,
                row_minimum, &row_column_count)) {
            table_measure_row(
                context, row.node, &row.parent_style, available_width,
                collapsed_geometry, spacing_x, placements, placement_count,
                row_preferred, row_minimum, &row_column_count);
            if (context->cancelled) return false;
            if (!has_row_spans) {
                table_row_cache_put(
                    context, row.node, available_width, row_preferred,
                    row_minimum, row_column_count);
            }
        }
        table_merge_row(
            row_preferred, row_minimum, row_column_count, preferred,
            minimum, column_count);
        measured_rows++;
    }
    return true;
}

int table_intrinsic_width(LayoutContext *context, lxb_dom_node_t *table,
                          const ComputedStyle *table_style,
                          int available_width)
{
    if (context == NULL || table == NULL || table_style == NULL
        || available_width <= 0) return 0;
    int preferred[16] = {0};
    int minimum[16] = {0};
    int boundaries[17] = {0};
    size_t count = 0;
    TableCellPlacement *placements = NULL;
    size_t placement_count = 0;
    bool has_row_spans = false;
    if (!table_build_placements(
            context, table, table_style, &placements, &placement_count,
            &count, &has_row_spans)) {
        return 0;
    }
    table_measure_columns(
        context, table, table_style, available_width,
        preferred, minimum, &count);
    bool collapsed_geometry = table_collapsed_boundaries(
        context, table, table_style, boundaries, placements, placement_count,
        NULL, NULL, NULL, NULL);
    if (context->cancelled) {
        budget_free(context->layout->budget, placements);
        return 0;
    }
    if (!table_measure_rows(context, table, table_style, available_width,
                            preferred, minimum, &count,
                            collapsed_geometry, placements, placement_count,
                            has_row_spans)) {
        budget_free(context->layout->budget, placements);
        return 0;
    }
    int width = table_style->border.left + table_style->border.right
                + table_style->padding.left + table_style->padding.right;
    if (collapsed_geometry && count != 0) {
        width += boundaries[0] + boundaries[count];
    } else {
        const StylePaintStack *paint = stylesheet_paint_stack(
            context->sheet, computed_style_paint_stack_id(table_style));
        if (paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
            width = layout_add_coordinate(
                width, (int) paint->table_spacing_x * ((int) count + 1));
        }
    }
    for (size_t i = 0; i < count; i++) {
        width = layout_add_coordinate(width, preferred[i]);
    }
    for (lxb_dom_node_t *child = table->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !layout_node_name_is(child, "caption")) continue;
        int caption = intrinsic_text_width(
            context, child, table_style, available_width);
        if (caption > width) width = caption;
    }
    budget_free(context->layout->budget, placements);
    return width > available_width ? available_width : width;
}

static bool table_fixed_first_row_tracks(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int available_width,
    TableTracks *tracks)
{
    int column_preferred[16] = {0};
    int column_minimum[16] = {0};
    size_t column_constraints = 0;
    table_measure_columns(
        context, table, table_style, available_width,
        column_preferred, column_minimum, &column_constraints);
    FlatItemIterator rows;
    FlatItem row;
    flat_iterator_init(&rows, context, table, table_style);
    while (flat_iterator_next(&rows, &row)) {
        if (row.anonymous_text
            || row.style.display != DISPLAY_TABLE_ROW) continue;
        size_t column = 0;
        bool specified[16] = {false};
        memset(tracks->widths, 0, sizeof(tracks->widths));
        for (size_t i = 0; i < column_constraints && i < 16; i++) {
            if (column_preferred[i] <= 0) continue;
            tracks->widths[i] = column_preferred[i];
            specified[i] = true;
        }
        ComputedStyle row_style = layout_style_for_node(
            context, row.node, &row.parent_style);
        for (lxb_dom_node_t *cell = row.node->first_child;
             cell != NULL && column < 16; cell = cell->next) {
            if (cell->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            ComputedStyle cell_style = layout_style_for_node(
                context, cell, &row_style);
            if (cell_style.display != DISPLAY_TABLE_CELL) continue;
            size_t span = table_cell_span(cell);
            const TableCellPlacement *placement = table_find_placement(
                tracks->placements, tracks->placement_count, cell);
            if (placement != NULL) {
                column = placement->column;
                span = placement->column_span;
            }
            if (span > 16 - column) span = 16 - column;
            resolve_padding(context->sheet, &cell_style, available_width);
            if (cell_style.has_width) {
                int authored = resolve_declared_length(
                    context->sheet, cell_style.width,
                    cell_style.width_percent, available_width);
                if (!cell_style.box_sizing_border_box) {
                    authored += cell_style.padding.left
                                + cell_style.padding.right
                                + cell_style.border.left
                                + cell_style.border.right;
                }
                if (authored < 0) authored = 0;
                int seen = 0;
                for (size_t i = 0; i < span; i++) {
                    int next = tilefinch_mul_div_int(
                        authored, (int) i + 1, (int) span);
                    if (!specified[column + i]) {
                        tracks->widths[column + i] = next - seen;
                        specified[column + i] = true;
                    }
                    seen = next;
                }
            }
            column += span;
        }
        if (column_constraints > column) column = column_constraints;
        if (column == 0) return false;
        int reserved = 0;
        size_t automatic = 0;
        for (size_t i = 0; i < column; i++) {
            if (specified[i]) reserved += tracks->widths[i];
            else automatic++;
        }
        int remaining = available_width > reserved
                        ? available_width - reserved : 0;
        int automatic_seen = 0;
        int pixels_seen = 0;
        for (size_t i = 0; i < column; i++) {
            if (specified[i]) continue;
            automatic_seen++;
            int pixels = automatic != 0
                ? tilefinch_mul_div_int(
                      remaining, automatic_seen, (int) automatic)
                : 0;
            tracks->widths[i] = pixels - pixels_seen;
            pixels_seen = pixels;
        }
        if (automatic == 0 && remaining > 0) {
            int seen = 0;
            for (size_t i = 0; i < column; i++) {
                int pixels = tilefinch_mul_div_int(
                    remaining, (int) i + 1, (int) column);
                tracks->widths[i] += pixels - seen;
                seen = pixels;
            }
        }
        for (size_t i = 0; i < column; i++) {
            if (tracks->widths[i] < 1) tracks->widths[i] = 1;
        }
        tracks->count = column;
        return true;
    }
    return false;
}

const TableTracks *table_tracks_for_table(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int available_width)
{
    if (table == NULL || table_style == NULL || available_width < 8) {
        return NULL;
    }
    for (size_t i = 0; i < context->table_track_count; i++) {
        if (context->table_tracks[i].table == table
            && context->table_tracks[i].available_width == available_width) {
            return &context->table_tracks[i];
        }
    }
    if (context->table_track_count
        >= sizeof(context->table_tracks) / sizeof(context->table_tracks[0])) {
        return NULL;
    }
    int preferred[16] = {0};
    int minimum[16] = {0};
    int boundaries[17] = {0};
    size_t count = 0;
    bool has_row_spans = false;
    TableTracks *tracks =
        &context->table_tracks[context->table_track_count++];
    *tracks = (TableTracks) {
        .table = table, .available_width = available_width
    };
    const StylePaintStack *table_paint = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(table_style));
    if (!table_style->table_border_collapse && table_paint != NULL
        && (table_paint->components
            & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
        tracks->spacing_x = table_paint->table_spacing_x;
        tracks->spacing_y = table_paint->table_spacing_y;
    }
    if (!table_build_placements(
            context, table, table_style, &tracks->placements,
            &tracks->placement_count, &count, &has_row_spans)) {
        context->table_track_count--;
        return NULL;
    }
    table_measure_columns(
        context, table, table_style, available_width,
        preferred, minimum, &count);
    lxb_dom_node_t *first_row = NULL, *last_row = NULL;
    int top_gutter = 0, bottom_gutter = 0;
    bool collapsed_geometry = table_collapsed_boundaries(
        context, table, table_style, boundaries, tracks->placements,
        tracks->placement_count, &first_row, &last_row,
        &top_gutter, &bottom_gutter);
    if (context->cancelled) {
        budget_free(context->layout->budget, tracks->placements);
        context->table_track_count--;
        return NULL;
    }
    if (!table_measure_rows(context, table, table_style, available_width,
                            preferred, minimum, &count,
                            collapsed_geometry, tracks->placements,
                            tracks->placement_count, has_row_spans)) {
        budget_free(context->layout->budget, tracks->placements);
        context->table_track_count--;
        return NULL;
    }
    if (count == 0) {
        budget_free(context->layout->budget, tracks->placements);
        context->table_track_count--;
        return NULL;
    }
    tracks->count = count;
    tracks->collapsed_left_gutter =
        collapsed_geometry ? boundaries[0] : 0;
    tracks->collapsed_right_gutter =
        collapsed_geometry ? boundaries[count] : 0;
    tracks->first_row = first_row;
    tracks->last_row = last_row;
    tracks->collapsed_top_gutter =
        collapsed_geometry ? top_gutter : 0;
    tracks->collapsed_bottom_gutter =
        collapsed_geometry ? bottom_gutter : 0;
    tracks->border_collapse = collapsed_geometry;
    int grid_available = available_width
                         - tracks->collapsed_left_gutter
                         - tracks->collapsed_right_gutter
                         - (int) tracks->spacing_x * ((int) count + 1);
    if (grid_available < 0) grid_available = 0;
    if (table_style->table_layout_fixed && table_style->has_width
        && table_fixed_first_row_tracks(
               context, table, table_style, grid_available, tracks)) {
        return tracks;
    }
    int reserved = 0;
    for (size_t i = 0; i < count; i++) {
        if (minimum[i] < 8) minimum[i] = 8;
        if (preferred[i] < minimum[i]) preferred[i] = minimum[i];
        reserved += minimum[i];
    }
    int distributable = grid_available > reserved
                        ? grid_available - reserved : 0;
    long long weight_total = 0;
    for (size_t i = 0; i < count; i++) {
        int weight = preferred[i] > minimum[i]
                     ? preferred[i] - minimum[i] : 1;
        weight_total += weight;
    }
    long long weight_seen = 0;
    int pixels_seen = 0;
    for (size_t i = 0; i < count; i++) {
        int weight = preferred[i] > minimum[i]
                     ? preferred[i] - minimum[i] : 1;
        weight_seen += weight;
        int pixels = 0;
        if (weight_total > 0) {
            pixels = weight_total <= INT_MAX && weight_seen <= INT_MAX
                ? tilefinch_mul_div_int(
                      distributable, (int) weight_seen,
                      (int) weight_total)
                : (int) ((long long) distributable * weight_seen
                         / weight_total);
        }
        tracks->widths[i] = minimum[i] + pixels - pixels_seen;
        pixels_seen = pixels;
    }
    return tracks;
}

const TableTracks *table_tracks_for_row(
    LayoutContext *context, lxb_dom_node_t *row, int available_width)
{
    lxb_dom_node_t *table = row == NULL ? NULL : row->parent;
    while (table != NULL && !layout_node_name_is(table, "table")) {
        table = table->parent;
    }
    if (table == NULL) return NULL;
    for (size_t i = 0; i < context->table_track_count; i++) {
        if (context->table_tracks[i].table == table
            && context->table_tracks[i].available_width == available_width) {
            return &context->table_tracks[i];
        }
    }
    return NULL;
}

bool table_cell_uses_collapsed_geometry(
    const LayoutContext *context, const lxb_dom_node_t *cell)
{
    if (context == NULL || cell == NULL) return false;
    lxb_dom_node_t *table = cell->parent;
    while (table != NULL && !layout_node_name_is(table, "table")) {
        table = table->parent;
    }
    if (table == NULL) return false;
    for (size_t i = 0; i < context->table_track_count; i++) {
        if (context->table_tracks[i].table == table) {
            return context->table_tracks[i].border_collapse;
        }
    }
    return false;
}
