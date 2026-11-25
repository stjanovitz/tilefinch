/* Table layout: cell spans, row measurement, and column tracks.
   Split out of layout.c. */

#include "layout_internal.h"
#include "tilefinch/integer_math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Next node of a pre-order walk of `root`'s subtree; `skip` passes over
   the current node's children. */
static lxb_dom_node_t *table_cell_walk_next(lxb_dom_node_t *at,
                                            const lxb_dom_node_t *root,
                                            bool skip)
{
    if (!skip && at->first_child != NULL) return at->first_child;
    while (at != root && at->next == NULL) at = at->parent;
    return at == root ? NULL : at->next;
}

/* Move a vertically aligned cell's content down by dy, leaving the cell's
   own box, background and interaction regions in place. Work is bounded by
   the cell: the box index finds the root, links and controls come from the
   range the cell recorded (everything its layout produced), and boxes from a
   walk of its DOM subtree. Positioned descendants keep their positions. */
void translate_table_cell_contents(LayoutDocument *layout,
                                          lxb_dom_node_t *node, int dy)
{
    if (layout == NULL || node == NULL || dy == 0) return;
    LayoutNodeBox *root = layout_box_for_node_mutable(layout, node);
    if (root == NULL) return;
    size_t root_index = (size_t) (root - layout->node_boxes);
    if (layout->node_interaction_ranges == NULL
        || root_index >= layout->node_interaction_capacity) return;
    size_t command_start = root->scroll_command_start;
    size_t command_end = root->scroll_command_end;
    if (command_start > layout->count) command_start = layout->count;
    if (command_end > layout->count) command_end = layout->count;
    for (size_t i = command_start; i < command_end; i++) {
        layout->commands[i].y += dy;
    }
    const uint32_t *interactions = layout->node_interaction_ranges
        + root_index * LAYOUT_NODE_INTERACTION_STRIDE;
    size_t link_start = interactions[LAYOUT_NODE_LINK_START];
    size_t link_end = interactions[LAYOUT_NODE_LINK_END];
    if (link_end > layout->link_count) link_end = layout->link_count;
    for (size_t i = link_start; i < link_end; i++) {
        layout->performance.table_cell_shift_visits++;
        if (layout->links[i].node != node
            && layout_node_within(layout->links[i].node, node)) {
            layout->links[i].y += dy;
        }
    }
    size_t control_start = interactions[LAYOUT_NODE_CONTROL_START];
    size_t control_end = interactions[LAYOUT_NODE_CONTROL_END];
    if (control_end > layout->control_count)
        control_end = layout->control_count;
    for (size_t i = control_start; i < control_end; i++) {
        layout->performance.table_cell_shift_visits++;
        if (layout->controls[i].node != node
            && layout_node_within(layout->controls[i].node, node)) {
            layout->controls[i].y += dy;
        }
    }
    for (lxb_dom_node_t *at = node->first_child; at != NULL;
         at = table_cell_walk_next(at, node, false)) {
        layout->performance.table_cell_shift_visits++;
        LayoutNodeBox *box = layout_box_for_node_mutable(layout, at);
        if (box != NULL) box->y += dy;
    }
    /* Undo the move for each outermost positioned descendant; a positioned
       box nested in another moves with it. */
    for (lxb_dom_node_t *at = node->first_child; at != NULL;) {
        layout->performance.table_cell_shift_visits++;
        const LayoutNodeBox *box = layout_box_for_node(layout, at);
        bool positioned = box != NULL
            && box->positioned_ancestor_distance != 0;
        if (positioned) translate_node_subtree(layout, at, 0, -dy);
        at = table_cell_walk_next(at, node, positioned);
    }
}


static bool table_is_html_cell(const lxb_dom_node_t *cell)
{
    return cell != NULL && cell->ns == LXB_NS_HTML
        && (document_node_is_tag(cell, LXB_TAG_TD) || document_node_is_tag(cell, LXB_TAG_TH));
}

size_t table_cell_span(lxb_dom_node_t *cell)
{
    if (!table_is_html_cell(cell)) return 1;
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
    if (!table_is_html_cell(cell)) return 1;
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

void table_expand_row_spans(LayoutContext *context, const TableTracks *tracks,
                            const ComputedStyle *table_style)
{
    if (tracks == NULL || !tracks->has_row_spans) return;
    for (size_t i = 0; i < tracks->placement_count; i++) {
        const TableCellPlacement *place = &tracks->placements[i];
        if (place->row_span <= 1) continue;
        lxb_dom_node_t *row = place->cell->parent;
        const LayoutNodeBox *cell_box = layout_box_for_node(context->layout,
                                                           place->cell);
        if (row == NULL || cell_box == NULL) continue;
        int end = cell_box->y + cell_box->height;
        unsigned rows = 0, visited = 0;
        /* A rowspan stops at its row group. Unpublished preview rows
           have no box and cannot supply a definite extent yet. */
        for (lxb_dom_node_t *at = row;
             at != NULL && rows < place->row_span && visited++ < TABLE_PLACEMENT_LIMIT;
             at = at->next) {
            if (!document_node_is_tag(at, LXB_TAG_TR)) continue;
            const LayoutNodeBox *box = layout_box_for_node(context->layout, at);
            if (box == NULL) continue;
            rows++;
            int bottom = box->y + box->height;
            if (bottom > end) end = bottom;
        }
        if (end <= cell_box->y + cell_box->height) continue;
        ComputedStyle group = row->parent == tracks->table ? *table_style
            : layout_style_for_node(context, row->parent, table_style);
        ComputedStyle row_style = layout_style_for_node(context, row, &group);
        ComputedStyle cell = layout_style_for_node(context, place->cell, &row_style);
        layout_expand_table_cell(context, place->cell, &cell, end - cell_box->y);
    }
}

static size_t table_bounded_span_attribute(
    lxb_dom_node_t *node, const char *name);

static size_t table_declared_column_count(lxb_dom_node_t *table)
{
    size_t count = 0;
    for (lxb_dom_node_t *child = table->first_child;
         child != NULL && count < 16; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (document_node_is_tag(child, LXB_TAG_COL)) {
            count += table_bounded_span_attribute(child, "span");
            continue;
        }
        if (!document_node_is_tag(child, LXB_TAG_COLGROUP)) continue;
        size_t nested = 0;
        for (lxb_dom_node_t *col = child->first_child;
             col != NULL && count + nested < 16; col = col->next) {
            if (col->type == LXB_DOM_NODE_TYPE_ELEMENT
                && document_node_is_tag(col, LXB_TAG_COL)) {
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
    size_t reach = declared_columns;
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
                    .row_span = (uint8_t) row_span,
                    .row_column_span = (uint8_t) span
                };
            }
            for (size_t i = 0; i < span; i++) {
                occupied[column + i] = (uint8_t) row_span;
            }
            cursor = column + span;
            if (cursor > columns) columns = cursor;
            if (column >= reach) reach = column + 1;
        }
    }
    /* An auto-layout table has no column past the last one a cell starts
       in or a <col> makes, as in Chromium, which gives a column no cell
       starts in no width: a colspan=1000 cell over two columns makes a
       table two columns wide. A fixed layout keeps the first row's
       columns. */
    if (!(table_style->table_layout_fixed && table_style->has_width)
        && columns > reach) {
        for (size_t i = 0; i < count; i++) {
            TableCellPlacement *placement = &placements[i];
            if (placement->column_span > reach - placement->column) {
                placement->column_span =
                    (uint8_t) (reach - placement->column);
            }
        }
        columns = reach;
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

/* The CSS Tables 3 column constraints beside each column's maximum
   (`preferred`) and content minimum (`minimum`): which columns a cell
   covers (`seen`), which have a fixed width (`constrained`, whose maximum
   is then the fixed width, never their minimum), and which have a
   percentage width (`percent_columns`, the largest raw percentage in
   hundredths of a percent in `percent`). A row also keeps its first
   spanning cells, which are distributed once every row is merged. */
typedef struct {
    uint16_t percent[16];
    uint16_t seen;
    uint16_t constrained;
    uint16_t percent_columns;
    uint8_t span_count;
    TableSpanCell spans[LAYOUT_TABLE_ROW_SPAN_CELLS];
} TableColumnFloors;

/* A width that is a pure percentage (no length part), in hundredths of a
   percent of the table grid. */
static bool table_width_points(const Stylesheet *sheet,
                               const ComputedStyle *style, uint16_t *points)
{
    if (!style->has_width || !style->width_percent
        || style->width_max_content) return false;
    int at = resolve_declared_length(sheet, style->width, true, 10000);
    int twice = resolve_declared_length(sheet, style->width, true, 20000);
    int linear = twice - 2 * at;
    if (linear < -1 || linear > 1) return false;
    *points = at > 65535 ? 65535 : (uint16_t) at;
    return true;
}

/* Merge one cell's (or col's, or row's) contribution into column `i`, as
   Chromium does: minimums take the largest; a fixed-width contribution's
   maximum replaces an auto column's, and auto maximums do not raise a
   fixed column's. */
static void table_column_encompass(int preferred[16], int minimum[16],
                                   TableColumnFloors *floors, size_t i,
                                   int min, int max, bool constrained,
                                   bool percent, uint16_t points)
{
    uint16_t bit = (uint16_t) (1u << i);
    if ((floors->seen & bit) == 0) {
        minimum[i] = min;
        preferred[i] = max;
        if (constrained) floors->constrained |= bit;
    } else {
        if (min > minimum[i]) minimum[i] = min;
        bool column_constrained = (floors->constrained & bit) != 0;
        if (column_constrained == constrained) {
            if (max > preferred[i]) preferred[i] = max;
        } else if (constrained) {
            preferred[i] = max;
            floors->constrained |= bit;
        }
    }
    if (preferred[i] < minimum[i]) preferred[i] = minimum[i];
    floors->seen |= bit;
    if (percent) {
        floors->percent_columns |= bit;
        if (points > floors->percent[i]) floors->percent[i] = points;
    }
}

/* Split a cell's minimum and maximum evenly over the `span` columns from
   `column` (a spanning cell past the ones a row or table keeps). */
static void table_span_even(int preferred[16], int minimum[16],
                            TableColumnFloors *floors, size_t column,
                            size_t span, int min, int max, bool constrained,
                            bool percent, uint16_t points)
{
    int per_track = (max + (int) span - 1) / (int) span;
    int floor_per_track = (min + (int) span - 1) / (int) span;
    for (size_t i = 0; i < span; i++) {
        table_column_encompass(preferred, minimum, floors, column + i,
                               floor_per_track, per_track, constrained,
                               percent, (uint16_t) (points / span));
    }
}

static void table_measure_row(LayoutContext *context, lxb_dom_node_t *row,
                              const ComputedStyle *row_parent,
                              int available_width, bool border_collapse,
                              unsigned border_spacing_x,
                              const TableCellPlacement *placements,
                              size_t placement_count,
                              int preferred[16],
                              int minimum[16],
                              TableColumnFloors *floors,
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
            span = placement->row_column_span;
        }
        if (column >= 16) break;
        if (span > 16 - column) span = 16 - column;
        int wanted = 0;
        int floor = 0;
        intrinsic_text_widths(
            context, cell, &row_style, available_width, &wanted, &floor);
        uint16_t points = 0;
        bool percent = table_width_points(context->sheet, &cell_style,
                                          &points);
        bool fixed = cell_style.has_width && !percent
                     && !cell_style.width_max_content;
        if (fixed) {
            /* The min-content measure of a fixed-width cell is its width;
               the column minimum is its content's. Its width is the
               column maximum. */
            floor = intrinsic_min_text_width_internal(
                context, cell, &row_style, available_width, true);
            resolve_padding(context->sheet, &cell_style, available_width);
            int authored = resolve_declared_length(
                context->sheet, cell_style.width,
                cell_style.width_percent, available_width);
            if (!cell_style.box_sizing_border_box) {
                authored += cell_style.padding.left + cell_style.padding.right
                            + cell_style.border.left + cell_style.border.right;
            }
            wanted = authored;
        }
        if (wanted < floor) wanted = floor;
        if (border_collapse) {
            /* Only the inward half of a collapsed border belongs to the
               column track. Apply this after authored-width handling too:
               CSS cell widths include the full border box, while the table
               reserves the two outward halves as its outer gutters. */
            int outward = cell_style.border.left / 2
                          + cell_style.border.right / 2;
            wanted = wanted > outward ? wanted - outward : 0;
            floor = floor > outward ? floor - outward : 0;
        }
        /* A spanning cell waits until every single-column cell is merged
           (table_distribute_spans), which takes off the spacing between
           its columns once the table's columns are known. Its fixed width
           only raises its columns' maximums, as in Chromium: it never
           makes them fixed columns, which an auto cell in another row
           could then no longer widen. */
        if (span > 1 && floors->span_count < LAYOUT_TABLE_ROW_SPAN_CELLS) {
            floors->spans[floors->span_count++] = (TableSpanCell) {
                .minimum = floor, .maximum = wanted, .points = points,
                .column = (uint8_t) column, .span = (uint8_t) span
            };
        } else {
            if (!border_collapse && span > 1) {
                int internal = (int) border_spacing_x * ((int) span - 1);
                wanted = wanted > internal ? wanted - internal : 0;
                floor = floor > internal ? floor - internal : 0;
            }
            table_span_even(preferred, minimum, floors, column, span, floor,
                            wanted, fixed && span == 1, percent, points);
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
    lxb_dom_node_t **first_row, int *top_gutter, int *bottom_gutter)
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
        if (context->preview_y_limit > 0 && !context->preview_through
            && measured_rows >= LAYOUT_PREVIEW_TABLE_ROW_LIMIT) {
            layout_note_preview_inexact(context, INT_MIN);
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
                                int minimum[16], TableColumnFloors *floors,
                                size_t *column_count)
{
    if (context->reuse == NULL || context->container_live) return false;
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
            memcpy(floors->percent, entry->percent, sizeof(entry->percent));
            floors->seen = entry->seen;
            floors->constrained = entry->constrained;
            floors->percent_columns = entry->percent_columns;
            floors->span_count = entry->span_count;
            memcpy(floors->spans, entry->spans, sizeof(entry->spans));
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
                                const int minimum[16],
                                const TableColumnFloors *floors,
                                size_t column_count)
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
        /* A row measured at two widths (an auto-width table's own width,
           then its columns) keeps an entry for each. */
        if (entry->row == NULL
            || (entry->row == row
                && entry->available_width == available_width)) {
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
        .seen = floors->seen,
        .constrained = floors->constrained,
        .percent_columns = floors->percent_columns,
        .span_count = floors->span_count,
        .stamp = ++context->reuse->clock
    };
    memcpy(entry->spans, floors->spans, sizeof(entry->spans));
    memcpy(entry->preferred, preferred, sizeof(entry->preferred));
    memcpy(entry->minimum, minimum, sizeof(entry->minimum));
    memcpy(entry->percent, floors->percent, sizeof(entry->percent));
}

static void table_merge_row(const int row_preferred[16],
                            const int row_minimum[16],
                            const TableColumnFloors *row_floors,
                            size_t row_column_count, int preferred[16],
                            int minimum[16], TableColumnFloors *floors,
                            size_t *column_count)
{
    if (row_column_count > 16) row_column_count = 16;
    for (size_t i = 0; i < row_column_count; i++) {
        if (((row_floors->seen >> i) & 1u) == 0) continue;
        table_column_encompass(
            preferred, minimum, floors, i, row_minimum[i], row_preferred[i],
            ((row_floors->constrained >> i) & 1u) != 0,
            ((row_floors->percent_columns >> i) & 1u) != 0,
            row_floors->percent[i]);
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
    TableColumnFloors *floors, size_t *column_count)
{
    if (column >= 16) return;
    if (span > 16 - column) span = 16 - column;
    if (column + span > *column_count) *column_count = column + span;
    if (!style->has_width || style->width_max_content) return;
    uint16_t points = 0;
    bool percent = floors != NULL
                   && table_width_points(context->sheet, style, &points);
    int width = percent ? 0 : resolve_declared_length(
        context->sheet, style->width, style->width_percent, available_width);
    if (width < 0) width = 0;
    /* A col (or colgroup) that spans columns gives each of them its
       width, as HTML's column model and Chromium do. */
    for (size_t i = 0; i < span; i++) {
        if (floors != NULL) {
            /* A col's width constrains the column's maximum, not its
               minimum. */
            table_column_encompass(preferred, minimum, floors, column + i,
                                   0, width, !percent, percent, points);
            continue;
        }
        if (width > preferred[column + i]) preferred[column + i] = width;
        if (width > minimum[column + i]) minimum[column + i] = width;
    }
}

static void table_measure_columns(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int available_width,
    int preferred[16], int minimum[16], TableColumnFloors *floors,
    size_t *column_count)
{
    size_t column = 0;
    for (lxb_dom_node_t *child = table->first_child;
         child != NULL && column < 16; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        bool group = document_node_is_tag(child, LXB_TAG_COLGROUP);
        bool single = document_node_is_tag(child, LXB_TAG_COL);
        if (!group && !single) continue;
        ComputedStyle group_style = layout_style_for_node(
            context, child, table_style);
        if (single) {
            size_t span = table_bounded_span_attribute(child, "span");
            table_apply_column_constraint(
                context, &group_style, available_width, column, span,
                preferred, minimum, floors, column_count);
            column += span;
            continue;
        }
        /* A colgroup's width is the width of each of its columns that
           has none of its own. */
        bool have_columns = false;
        for (lxb_dom_node_t *col = child->first_child;
             col != NULL && column < 16; col = col->next) {
            if (col->type != LXB_DOM_NODE_TYPE_ELEMENT
                || !document_node_is_tag(col, LXB_TAG_COL)) continue;
            have_columns = true;
            size_t span = table_bounded_span_attribute(col, "span");
            ComputedStyle col_style = layout_style_for_node(
                context, col, &group_style);
            table_apply_column_constraint(
                context, col_style.has_width ? &col_style : &group_style,
                available_width, column, span, preferred, minimum, floors,
                column_count);
            column += span;
        }
        if (!have_columns) {
            size_t span = table_bounded_span_attribute(child, "span");
            table_apply_column_constraint(
                context, &group_style, available_width, column, span,
                preferred, minimum, floors, column_count);
            column += span;
        }
    }
}

static void table_distribute(const int minimum[16], const int maximum[16],
                             const TableColumnFloors *floors, size_t count,
                             int target, int widths[16], bool *squeezed);

/* A table's spanning cells, one per column and span with the largest
   minimum and maximum; cells past the limit split evenly. */
#define TABLE_SPAN_CELL_LIMIT 16u

static void table_note_span(TableSpanCell spans[TABLE_SPAN_CELL_LIMIT],
                            size_t *count, const TableSpanCell *cell,
                            int preferred[16], int minimum[16],
                            TableColumnFloors *floors)
{
    for (size_t i = 0; i < *count; i++) {
        TableSpanCell *same = &spans[i];
        if (same->column != cell->column || same->span != cell->span) {
            continue;
        }
        if (cell->minimum > same->minimum) same->minimum = cell->minimum;
        if (cell->maximum > same->maximum) same->maximum = cell->maximum;
        if (cell->points > same->points) same->points = cell->points;
        return;
    }
    if (*count < TABLE_SPAN_CELL_LIMIT) {
        /* Keep equal-length spans in column order when they are spread.
           An overlapping span sees the widths left by earlier spans. */
        size_t i = *count;
        for (; i > 0 && spans[i - 1].column > cell->column; i--) {
            spans[i] = spans[i - 1];
        }
        spans[i] = *cell;
        (*count)++;
        return;
    }
    table_span_even(preferred, minimum, floors, cell->column, cell->span,
                    cell->minimum, cell->maximum, false, cell->points != 0,
                    cell->points);
}

/* Spanning cells after the single-column ones, as in Chromium: smallest
   span first, ties by starting column. Each cell's minimum and then its
   maximum spread over its columns by the guesses table_distribute makes
   for a whole table, which raise the columns' minimums and maximums.
   A 200px cell over 60px and
   100px columns makes them 75 and 125; a 100px cell over 30px and 300px
   columns leaves them. A percentage cell first gives the part of its
   percentage its percentage columns do not hold to its other columns, in
   proportion to their maximums, which makes them percentage columns. */
static void table_distribute_spans(const TableSpanCell *spans, size_t count,
                                   int preferred[16], int minimum[16],
                                   TableColumnFloors *floors)
{
    for (size_t length = 1; length <= 16; length++) {
        for (size_t s = 0; s < count; s++) {
            if (spans[s].span != length) continue;
            size_t start = spans[s].column;
            int *low = minimum + start, *high = preferred + start;
            int surplus = spans[s].points;
            long long others = 0;
            int free_columns = 0;
            for (size_t i = 0; i < length; i++) {
                if ((floors->percent_columns >> (start + i)) & 1u) {
                    surplus -= floors->percent[start + i];
                } else {
                    others += high[i];
                    free_columns++;
                }
            }
            for (size_t i = 0; surplus > 0 && i < length; i++) {
                uint16_t bit = (uint16_t) (1u << (start + i));
                if (floors->percent_columns & bit) continue;
                floors->percent[start + i] = (uint16_t) (others > 0
                    ? surplus * (long long) high[i] / others
                    : surplus / free_columns);
                floors->percent_columns |= bit;
            }
            /* Only the spanned columns' marks: a fixed or percentage
               column past the span would take the room. */
            unsigned mask = (1u << length) - 1u;
            TableColumnFloors part = {
                .constrained =
                    (uint16_t) ((floors->constrained >> start) & mask),
                .percent_columns =
                    (uint16_t) ((floors->percent_columns >> start) & mask)
            };
            memcpy(part.percent, floors->percent + start,
                   length * sizeof(part.percent[0]));
            int widths[16];
            bool squeezed;
            /* The widths never fall below the minimums, so the first
               spread writes the new minimums in place. */
            table_distribute(low, high, &part, length, spans[s].minimum,
                             low, &squeezed);
            table_distribute(low, high, &part, length, spans[s].maximum,
                             widths, &squeezed);
            for (size_t i = 0; i < length; i++) {
                if (widths[i] > high[i]) high[i] = widths[i];
            }
            floors->seen |= (uint16_t) (mask << start);
        }
    }
}

static bool table_measure_rows(LayoutContext *context, lxb_dom_node_t *table,
                               const ComputedStyle *table_style,
                               int available_width, int preferred[16],
                               int minimum[16], TableColumnFloors *floors,
                               size_t *column_count,
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
    TableSpanCell spans[TABLE_SPAN_CELL_LIMIT];
    size_t span_count = 0;
    /* Rows measure (and the row cache keeps) the spans their cells give
       them; the table's columns (table_build_placements) cut them. */
    size_t limit = *column_count;
    flat_iterator_init(&rows, context, table, table_style);
    while (flat_iterator_next(&rows, &row)) {
        if (row.anonymous_text
            || row.style.display != DISPLAY_TABLE_ROW) continue;
        if (context->preview_y_limit > 0 && !context->preview_through
            && measured_rows >= LAYOUT_PREVIEW_TABLE_ROW_LIMIT) {
            layout_note_preview_inexact(context, INT_MIN);
            break;
        }
        if (!layout_cooperate(context, row.node)) return false;
        int row_preferred[16] = {0};
        int row_minimum[16] = {0};
        TableColumnFloors row_floors = {0};
        size_t row_column_count = 0;
        if (has_row_spans || !table_row_cache_get(
                context, row.node, available_width, row_preferred,
                row_minimum, &row_floors, &row_column_count)) {
            table_measure_row(
                context, row.node, &row.parent_style, available_width,
                collapsed_geometry, spacing_x, placements, placement_count,
                row_preferred, row_minimum, &row_floors, &row_column_count);
            if (context->cancelled) return false;
            if (!has_row_spans) {
                table_row_cache_put(
                    context, row.node, available_width, row_preferred,
                    row_minimum, &row_floors, row_column_count);
            }
        }
        if (row_column_count > limit) row_column_count = limit;
        table_merge_row(
            row_preferred, row_minimum, &row_floors, row_column_count,
            preferred, minimum, floors, column_count);
        for (size_t i = 0; i < row_floors.span_count; i++) {
            TableSpanCell cell = row_floors.spans[i];
            if (cell.column >= limit) continue;
            if (cell.span > limit - cell.column) {
                cell.span = (uint8_t) (limit - cell.column);
            }
            int internal = (int) spacing_x * (cell.span - 1);
            cell.minimum = cell.minimum > internal
                ? cell.minimum - internal : 0;
            cell.maximum = cell.maximum > internal
                ? cell.maximum - internal : 0;
            table_note_span(spans, &span_count, &cell, preferred, minimum,
                            floors);
        }
        measured_rows++;
    }
    table_distribute_spans(spans, span_count, preferred, minimum, floors);
    /* Every column a cell starts in is at least 8 wide, but an empty cell
       weighs nothing when a spanning cell is spread, as in Chromium. A
       column no cell starts in (one a <col> or a span makes) keeps only
       what its col or spanning cells give it, which Chromium gives no
       width. */
    unsigned starts = 0;
    for (size_t i = 0; i < placement_count; i++) {
        starts |= 1u << placements[i].column;
    }
    for (size_t i = 0; i < *column_count; i++) {
        if (((starts >> i) & 1u) && minimum[i] < 8) minimum[i] = 8;
        if (preferred[i] < minimum[i]) preferred[i] = minimum[i];
    }
    return true;
}

/* CSS table fix-up for a display:table element (not <table>) with no row
   child: consecutive table-cell children form one anonymous row, which
   layout_block.c lays out as that row. Measure those cells so the table
   shrinks to fit them; without this an anonymous-row table measured as
   zero columns and collapsed to its own padding. Only an all-cell run is
   measured here; mixed or text content keeps the previous behaviour. */
static void table_measure_anonymous_row(
    LayoutContext *context, lxb_dom_node_t *table,
    const ComputedStyle *table_style, int available_width,
    int preferred[16], size_t *column_count)
{
    if (document_node_is_tag(table, LXB_TAG_TABLE)) return;
    int widths[16] = {0};
    size_t cells = 0;
    FlatItemIterator items;
    FlatItem item;
    flat_iterator_init(&items, context, table, table_style);
    while (flat_iterator_next(&items, &item)) {
        if (!item.anonymous_text
            && (item.style.out_of_flow || item.style.fixed_position)) {
            continue;
        }
        if (item.anonymous_text || item.style.display != DISPLAY_TABLE_CELL
            || cells == 16) return;
        if (!layout_cooperate(context, item.node)) return;
        int wanted = 0;
        int floor = 0;
        intrinsic_text_widths(context, item.node, &item.parent_style,
                              available_width, &wanted, &floor);
        resolve_padding(context->sheet, &item.style, available_width);
        if (item.style.has_width) {
            int authored = resolve_declared_length(
                context->sheet, item.style.width,
                item.style.width_percent, available_width);
            if (!item.style.box_sizing_border_box) {
                authored += item.style.padding.left
                            + item.style.padding.right
                            + item.style.border.left
                            + item.style.border.right;
            }
            if (authored > wanted) wanted = authored;
        }
        widths[cells++] = wanted < 8 ? 8 : wanted;
    }
    for (size_t i = 0; i < cells; i++) preferred[i] = widths[i];
    *column_count = cells;
}

static long long table_grid_maximum(const int preferred[16],
                                    const TableColumnFloors *floors,
                                    size_t count, int available);

int table_intrinsic_width(LayoutContext *context, lxb_dom_node_t *table,
                          const ComputedStyle *table_style,
                          int available_width)
{
    if (context == NULL || table == NULL || table_style == NULL
        || available_width <= 0) return 0;
    int preferred[16] = {0};
    int minimum[16] = {0};
    TableColumnFloors floors = {0};
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
        preferred, minimum, &floors, &count);
    bool collapsed_geometry = table_collapsed_boundaries(
        context, table, table_style, boundaries, placements, placement_count,
        NULL, NULL, NULL);
    if (context->cancelled) {
        budget_free(context->layout->budget, placements);
        return 0;
    }
    if (!table_measure_rows(context, table, table_style, available_width,
                            preferred, minimum, &floors, &count,
                            collapsed_geometry, placements, placement_count,
                            has_row_spans)) {
        budget_free(context->layout->budget, placements);
        return 0;
    }
    if (count == 0) {
        table_measure_anonymous_row(context, table, table_style,
                                    available_width, preferred, &count);
        if (context->cancelled) {
            budget_free(context->layout->budget, placements);
            return 0;
        }
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
    long long grid = table_grid_maximum(preferred, &floors, count,
                                        available_width);
    width = layout_add_coordinate(
        width, grid > INT_MAX ? INT_MAX : (int) grid);
    for (lxb_dom_node_t *child = table->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !document_node_is_tag(child, LXB_TAG_CAPTION)) continue;
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
        column_preferred, column_minimum, NULL, &column_constraints);
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

/* Spread `room` over `count` columns in proportion to `weight` (cumulative,
   so the parts sum to `room`), adding each part to `widths`. */
static void table_spread(int room, const long long weight[16], size_t count,
                         int widths[16])
{
    long long total = 0;
    for (size_t i = 0; i < count; i++) total += weight[i];
    if (room <= 0 || total <= 0) return;
    /* Weights can each approach INT_MAX; keep room * seen in range. */
    int shift = 0;
    while ((total >> shift) > INT_MAX) shift++;
    total = 0;
    for (size_t i = 0; i < count; i++) total += weight[i] >> shift;
    if (total <= 0) return;
    long long seen = 0;
    int given = 0;
    for (size_t i = 0; i < count; i++) {
        seen += weight[i] >> shift;
        int part = (int) ((long long) room * seen / total);
        widths[i] = layout_add_coordinate(widths[i], part - given);
        given = part;
    }
}

/* Column percentages cut at 100% in column order (Chromium): a column
   whose percentage would take the grid past 100% keeps only what is left,
   down to none, and stays a percentage column. Returns the points left. */
static int table_column_points(const TableColumnFloors *floors,
                               size_t count, int points[16])
{
    int left = 10000;
    for (size_t i = 0; i < count; i++) {
        points[i] = 0;
        if (((floors->percent_columns >> i) & 1u) == 0) continue;
        int kept = floors->percent[i] < left ? floors->percent[i] : left;
        points[i] = kept;
        left -= kept;
    }
    return left;
}

/* The grid's maximum width for an auto-width table: the columns'
   maximums, widened so each percentage column can have its percentage at
   its maximum and the other columns the rest at theirs (CSS Tables 3 and
   Chromium). Percentages that leave the other columns nothing fill
   `available`. */
static long long table_grid_maximum(const int preferred[16],
                                    const TableColumnFloors *floors,
                                    size_t count, int available)
{
    int points[16];
    int left = table_column_points(floors, count, points);
    long long total = 0;
    long long others = 0;
    long long estimate = 0;
    for (size_t i = 0; i < count; i++) {
        total += preferred[i];
        if (((floors->percent_columns >> i) & 1u) == 0) {
            others += preferred[i];
        } else if (points[i] > 0) {
            long long needed = (long long) preferred[i] * 10000 / points[i];
            if (needed > estimate) estimate = needed;
        }
    }
    if (floors->percent_columns != 0 && others > 0) {
        long long needed = left > 0 ? others * 10000 / left
                                    : (long long) available;
        if (needed > estimate) estimate = needed;
    }
    return estimate > total ? estimate : total;
}

/* Spread `room` over the columns of `group` in proportion to `weight`, or
   evenly when no column of the group has weight. */
static void table_spread_group(int room, const long long weight[16],
                               uint16_t group, size_t count, int widths[16])
{
    long long part[16] = {0};
    long long total = 0;
    for (size_t i = 0; i < count; i++) {
        if (((group >> i) & 1u) == 0) continue;
        part[i] = weight[i] > 0 ? weight[i] : 0;
        total += part[i];
    }
    if (total == 0) {
        for (size_t i = 0; i < count; i++) part[i] = (group >> i) & 1u;
    }
    table_spread(room, part, count, widths);
}

/* CSS Tables 3 width distribution for an auto-layout table, as Chromium
   implements it. Every column has a content minimum and a maximum (a fixed
   column's width, else its max-content). Four guesses grow from all
   minimums: percentage columns to their percentage of the grid, then fixed
   columns to their widths, then auto columns to their max-content. The
   grid's width falls between two guesses, and the columns that grow in
   that step share the room in proportion to what each still asks for.
   Past every maximum, the auto columns take the rest in proportion to
   their maximums, else the fixed columns, else the percentage columns by
   percentage. A grid narrower than every minimum overflows at them.
   `squeezed` is set when fixed columns end below their widths. */
static void table_distribute(const int minimum[16], const int maximum[16],
                             const TableColumnFloors *floors, size_t count,
                             int target, int widths[16], bool *squeezed)
{
    int points[16];
    (void) table_column_points(floors, count, points);
    uint16_t percent = floors->percent_columns;
    uint16_t fixed = (uint16_t) (floors->constrained & ~percent);
    uint16_t automatic = 0;
    long long guess_minimum = 0, guess_percent = 0, guess_specified = 0;
    long long guess_maximum = 0;
    int percent_width[16] = {0};
    long long percent_weight[16] = {0}, grow_weight[16] = {0};
    long long max_weight[16] = {0}, point_weight[16] = {0};
    for (size_t i = 0; i < count; i++) {
        uint16_t bit = (uint16_t) (1u << i);
        int low = minimum[i];
        int high = maximum[i] > low ? maximum[i] : low;
        widths[i] = low;
        guess_minimum += low;
        max_weight[i] = high;
        if (percent & bit) {
            int wanted = tilefinch_mul_div_int(target, points[i], 10000);
            percent_width[i] = wanted > low ? wanted : low;
            percent_weight[i] = percent_width[i] - low;
            point_weight[i] = points[i];
            guess_percent += percent_width[i];
            guess_specified += percent_width[i];
            guess_maximum += percent_width[i];
        } else if (fixed & bit) {
            grow_weight[i] = high - low;
            guess_percent += low;
            guess_specified += high;
            guess_maximum += high;
        } else {
            automatic |= bit;
            grow_weight[i] = high - low;
            guess_percent += low;
            guess_specified += low;
            guess_maximum += high;
        }
    }
    *squeezed = target < guess_specified;
    if (target <= guess_minimum) return;
    if (target <= guess_percent) {
        table_spread_group((int) (target - guess_minimum), percent_weight,
                           percent, count, widths);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if ((percent >> i) & 1u) widths[i] = percent_width[i];
    }
    if (target <= guess_specified) {
        table_spread_group((int) (target - guess_percent), grow_weight,
                           fixed, count, widths);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if ((fixed >> i) & 1u) widths[i] = (int) max_weight[i];
    }
    if (target <= guess_maximum) {
        table_spread_group((int) (target - guess_specified), grow_weight,
                           automatic, count, widths);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if ((automatic >> i) & 1u) widths[i] = (int) max_weight[i];
    }
    int room = (int) (target - guess_maximum);
    if (automatic != 0) {
        table_spread_group(room, max_weight, automatic, count, widths);
    } else if (fixed != 0) {
        table_spread_group(room, max_weight, fixed, count, widths);
    } else {
        table_spread_group(room, point_weight, percent, count, widths);
    }
}

/* A slot for `table`'s tracks: a free one, else the least recently used
   slot whose table does not enclose `table`. Slots of enclosing tables are
   still in use by their rows, so they are never taken; NULL when every slot
   belongs to one (nesting deeper than LAYOUT_TABLE_TRACK_SLOTS). */
static TableTracks *table_tracks_acquire(LayoutContext *context,
                                         const lxb_dom_node_t *table)
{
    TableTracks *victim = NULL;
    for (size_t i = 0; i < context->table_track_count; i++) {
        TableTracks *slot = &context->table_tracks[i];
        if (slot->table == NULL) return slot;
        if (layout_node_within(table, slot->table)) continue;
        if (victim == NULL || slot->last_use < victim->last_use) {
            victim = slot;
        }
    }
    if (context->table_track_count < LAYOUT_TABLE_TRACK_SLOTS) {
        return &context->table_tracks[context->table_track_count++];
    }
    if (victim != NULL) {
        budget_free(context->layout->budget, victim->placements);
        *victim = (TableTracks) {0};
    }
    return victim;
}

static void table_tracks_release(LayoutContext *context, TableTracks *tracks)
{
    budget_free(context->layout->budget, tracks->placements);
    *tracks = (TableTracks) {0};
    if (tracks == &context->table_tracks[context->table_track_count - 1u]) {
        context->table_track_count--;
    }
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
            context->table_tracks[i].last_use = ++context->table_track_clock;
            return &context->table_tracks[i];
        }
    }
    TableTracks *tracks = table_tracks_acquire(context, table);
    if (tracks == NULL) return NULL;
    int preferred[16] = {0};
    int minimum[16] = {0};
    TableColumnFloors floors = {0};
    int boundaries[17] = {0};
    size_t count = 0;
    bool has_row_spans = false;
    *tracks = (TableTracks) {
        .table = table, .available_width = available_width,
        .last_use = ++context->table_track_clock
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
        table_tracks_release(context, tracks);
        return NULL;
    }
    tracks->has_row_spans = has_row_spans;
    table_measure_columns(
        context, table, table_style, available_width,
        preferred, minimum, &floors, &count);
    lxb_dom_node_t *first_row = NULL;
    int top_gutter = 0, bottom_gutter = 0;
    bool collapsed_geometry = table_collapsed_boundaries(
        context, table, table_style, boundaries, tracks->placements,
        tracks->placement_count, &first_row, &top_gutter, &bottom_gutter);
    if (context->cancelled) {
        table_tracks_release(context, tracks);
        return NULL;
    }
    if (!table_measure_rows(context, table, table_style, available_width,
                            preferred, minimum, &floors, &count,
                            collapsed_geometry, tracks->placements,
                            tracks->placement_count, has_row_spans)) {
        table_tracks_release(context, tracks);
        return NULL;
    }
    if (count == 0) {
        table_tracks_release(context, tracks);
        return NULL;
    }
    tracks->count = count;
    tracks->collapsed_left_gutter =
        collapsed_geometry ? boundaries[0] : 0;
    tracks->collapsed_right_gutter =
        collapsed_geometry ? boundaries[count] : 0;
    tracks->first_row = first_row;
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
    table_distribute(minimum, preferred, &floors, count, grid_available,
                     tracks->widths, &tracks->squeezed);
    return tracks;
}

static const TableTracks *table_tracks_for_cell(
    const LayoutContext *context, const lxb_dom_node_t *cell)
{
    return context && cell && cell->parent == context->placing_table_row
        ? context->placing_row_tracks : NULL;
}

bool table_cell_uses_collapsed_geometry(
    const LayoutContext *context, const lxb_dom_node_t *cell)
{
    const TableTracks *tracks = table_tracks_for_cell(context, cell);
    return tracks != NULL && tracks->border_collapse;
}

/* Whether table_expand_row_spans will size this rowspan cell: the table
   kept tracks and placed it. Others fall back to their own row. */
bool table_cell_row_span_placed(const LayoutContext *context,
                                lxb_dom_node_t *cell)
{
    return table_cell_placement(table_tracks_for_cell(context, cell), cell,
                                NULL, NULL, NULL);
}
