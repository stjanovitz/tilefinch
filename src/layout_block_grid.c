/* Grid formatting context: track sizing, item placement, subgrid inheritance,
   and the item layout/alignment walk. Split out of layout_block.c, where it
   ran as one phase of layout_block_impl. */

#include "layout_block_internal.h"
#include "tilefinch/integer_math.h"

#include <string.h>

static int grid_relative_line(unsigned encoded, int explicit_tracks)
{
    int line = computed_style_decode_grid_line(encoded);
    return line < 0 ? explicit_tracks + 1 + line : line - 1;
}

void grid_axis_extent(unsigned start, unsigned end, unsigned span,
                      int explicit_tracks, int *minimum_track,
                      int *maximum_track)
{
    if (minimum_track == NULL || maximum_track == NULL) return;
    int used_span = span > 0 ? (int) span : 1;
    if (start == 0 && end == 0) {
        /* An auto-placed spanning item still requires enough implicit tracks
           for its area even though its starting line is selected later. */
        if (span > 1 && used_span > *maximum_track) {
            *maximum_track = used_span;
        }
        return;
    }
    int first;
    int last;
    if (start != 0) {
        first = grid_relative_line(start, explicit_tracks);
        last = end != 0
            ? grid_relative_line(end, explicit_tracks)
            : first + used_span;
        if (last <= first) last = first + used_span;
    } else {
        last = grid_relative_line(end, explicit_tracks);
        first = last - used_span;
    }
    if (first < *minimum_track) *minimum_track = first;
    if (last > *maximum_track) *maximum_track = last;
}

static bool grid_track_accepts_max_content(uint8_t type, uint16_t value)
{
    return type == GRID_TRACK_AUTO
           && value != GRID_TRACK_MIN_CONTENT_VALUE;
}

static bool grid_track_stretches(uint8_t type, uint16_t value)
{
    return type == GRID_TRACK_AUTO && value == GRID_TRACK_AUTO_VALUE;
}

static void grid_defined_row_track(
    const Stylesheet *sheet, const ComputedStyle *style,
    unsigned row, unsigned declared_rows,
    uint16_t second_auto_row, uint8_t *type, unsigned *value)
{
    bool auto_sized = row >= declared_rows;
    unsigned auto_index = auto_sized ? row - declared_rows : 0;
    bool use_second = auto_sized && second_auto_row != 0
                      && (auto_index & 1u) != 0;
    *type = use_second
        ? (uint8_t) (second_auto_row >> 14)
        : (auto_sized
           ? style->grid_auto_row_type
           : stylesheet_grid_track_type(sheet, style, true, row));
    *value = use_second
        ? second_auto_row & 0x3fffu
        : (auto_sized
           ? style->grid_auto_row_value
           : stylesheet_grid_track_value(sheet, style, true, row));
}

static int grid_inherited_named_line(
    const LayoutContext *context, lxb_dom_node_t *container,
    bool rows, uint8_t name)
{
    if (context == NULL || name == 0
        || context->assigned_grid_tracks.node != container) return 0;
    const LayoutAssignedGridTracks *assigned =
        &context->assigned_grid_tracks;
    unsigned count = rows ? assigned->row_count : assigned->column_count;
    const uint8_t (*names)[STYLE_GRID_LINE_NAMES_PER_LINE] = rows
        ? assigned->row_line_names : assigned->column_line_names;
    for (unsigned line = 0;
         line <= count && line <= GRID_TRACK_REPEAT_LIMIT; line++) {
        for (unsigned slot = 0;
             slot < STYLE_GRID_LINE_NAMES_PER_LINE; slot++) {
            if (names[line][slot] == name) return (int) line + 1;
        }
    }
    return 0;
}

static void grid_resolve_item_placement(
    LayoutContext *context, lxb_dom_node_t *container,
    const ComputedStyle *container_style, ComputedStyle *item)
{
    (void) stylesheet_resolve_named_grid_area(
        context->sheet, container_style, item);
    (void) stylesheet_resolve_named_grid_lines(
        context->sheet, container_style, item);
#define RESOLVE_INHERITED_GRID_LINE(axis, field, rows_value)                 \
    do {                                                                     \
        uint8_t name =                                                       \
            computed_style_grid_##axis##_##field##_name(item);               \
        int line = grid_inherited_named_line(                                \
            context, container, rows_value, name);                           \
        if (line != 0) {                                                     \
            computed_style_set_grid_##axis##_##field(                        \
                item, computed_style_encode_grid_line(line));                \
            computed_style_set_grid_##axis##_##field##_name(item, 0);        \
        }                                                                    \
    } while (0)
    RESOLVE_INHERITED_GRID_LINE(row, start, true);
    RESOLVE_INHERITED_GRID_LINE(row, end, true);
    RESOLVE_INHERITED_GRID_LINE(column, start, false);
    RESOLVE_INHERITED_GRID_LINE(column, end, false);
#undef RESOLVE_INHERITED_GRID_LINE
}

static int grid_subgrid_track_size(
    int inherited_size, int inherited_gap, int used_gap,
    unsigned index, unsigned count)
{
    if (count < 2 || inherited_gap == used_gap) return inherited_size;
    int difference = inherited_gap - used_gap;
    int before = difference / 2;
    int after = difference - before;
    int adjusted = inherited_size;
    if (index != 0) adjusted += after;
    if (index + 1u < count) adjusted += before;
    return adjusted > 0 ? adjusted : 1;
}

/*
 * Grow the intrinsic tracks spanned by one item toward a contribution. The
 * track count is tiny and fixed, so a bounded redistribution pass is cheaper
 * than retained per-item scratch. fit-content() caps only the max-content
 * phase; its automatic minimum remains eligible in the first.
 */
static void grid_distribute_intrinsic_extra(
    int start, int span, int extra, bool maximum_phase,
    const uint8_t *types, const uint16_t *values, int *widths)
{
    if (extra <= 0 || types == NULL || values == NULL || widths == NULL) {
        return;
    }
    bool locked[GRID_TRACK_LIMIT] = {false};
    for (int pass = 0; pass < span && extra > 0; pass++) {
        int eligible = 0;
        for (int offset = 0; offset < span; offset++) {
            int track = start + offset;
            if (track < 0 || track >= GRID_TRACK_LIMIT) continue;
            if (locked[track] || types[track] != GRID_TRACK_AUTO) continue;
            if (maximum_phase
                && !grid_track_accepts_max_content(
                       types[track], values[track])) {
                continue;
            }
            eligible++;
        }
        if (eligible == 0) break;
        int share = extra / eligible;
        if (share < 1) share = 1;
        bool capped = false;
        for (int offset = 0; offset < span && extra > 0; offset++) {
            int track = start + offset;
            if (track < 0 || track >= GRID_TRACK_LIMIT) continue;
            if (locked[track] || types[track] != GRID_TRACK_AUTO) continue;
            if (maximum_phase
                && !grid_track_accepts_max_content(
                       types[track], values[track])) {
                continue;
            }
            int addition = share < extra ? share : extra;
            if (maximum_phase
                && (values[track] & GRID_TRACK_FIT_CONTENT_FLAG) != 0) {
                int cap = values[track] & GRID_TRACK_FIT_CONTENT_MASK;
                int capacity = cap - widths[track];
                if (capacity <= 0) {
                    locked[track] = true;
                    capped = true;
                    continue;
                }
                if (addition > capacity) {
                    addition = capacity;
                    locked[track] = true;
                    capped = true;
                }
            }
            widths[track] += addition;
            extra -= addition;
        }
        if (!capped) break;
    }
}

/* A minimum block size gives fr rows a definite amount of free space, but
   fixed-size children in neighbouring auto rows consume that space first.
   Seed only contributions that can be resolved without recursively laying
   out content; intrinsically-sized auto rows retain the ordinary bounded
   growth path below. */
static void grid_seed_definite_auto_row_contributions(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *container_style, LayoutBlockScratch *scratch,
    FlexOrderPlan *grid_order, const GridPlacementState *placement_template,
    int placement_rows, int grid_row_origin, int explicit_grid_rows,
    int declared_grid_rows,
    uint16_t second_implicit_row, int content_width,
    int containing_height, int *row_track_heights, int *fixed_space)
{
    if (context == NULL || node == NULL || container_style == NULL
        || scratch == NULL || grid_order == NULL || placement_template == NULL
        || row_track_heights == NULL || fixed_space == NULL) return;
    GridPlacementState placement = *placement_template;
    FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
    FlatItem *item = &scratch->traversal.flex.item;
    flex_iterator_init(
        iterator, context, node, container_style, grid_order);
    while (flex_iterator_next(iterator, item)) {
        grid_resolve_item_placement(
            context, node, container_style, &item->style);
        if (item->style.out_of_flow || item->style.fixed_position) continue;
        GridItemPlacement item_placement;
        (void) grid_place_item(&placement, &item->style, &item_placement);
        if (item->anonymous_text || item_placement.row_span != 1) continue;
        int explicit_row = item_placement.row - grid_row_origin;
        if (explicit_row < 0 || explicit_row >= explicit_grid_rows) continue;
        uint8_t type = GRID_TRACK_AUTO;
        unsigned value = 0;
        grid_defined_row_track(
            context->sheet, container_style, (unsigned) explicit_row,
            (unsigned) declared_grid_rows, second_implicit_row,
            &type, &value);
        (void) value;
        if (type != GRID_TRACK_AUTO
            || (!item->style.has_height && item->style.min_height <= 0)) {
            continue;
        }
        ComputedStyle measured = item->style;
        resolve_padding(context->sheet, &measured, content_width);
        int contribution = style_content_height(
            context->sheet, &measured, content_width, containing_height);
        contribution += measured.padding.top + measured.padding.bottom
            + measured.border.top + measured.border.bottom
            + measured.margin.top + measured.margin.bottom;
        int row = item_placement.row;
        if (row < 0 || row >= placement_rows
            || contribution <= row_track_heights[row]) continue;
        *fixed_space += contribution - row_track_heights[row];
        row_track_heights[row] = contribution;
    }
}

/* A grid item as laid out, for the fr re-share after layout. */
typedef struct {
    int row;
    int dy;
    int extent;
    AlignItems alignment;
    size_t command_start;
    size_t link_start;
    size_t control_start;
    size_t box_start;
} GridReshareItem;

#define GRID_RESHARE_ITEM_LIMIT 32

/* A row-spanning, end/center-aligned item whose rows were not yet final
   when it was laid out. */
typedef struct {
    lxb_dom_node_t *node;
    int row;
    int span;
    int offset;
    int height;
    bool center;
} GridSpanAlignItem;

#define GRID_SPAN_ALIGN_LIMIT 8

/* A laid-out grid item's row and the start of its output ranges. Kept only
   for grids whose items can arrive out of row order (items with definite
   rows are placed before auto-placed ones, CSS Grid 8.5), so a row that
   grows after a later row already holds content can move that content. */
typedef struct {
    int row;
    size_t command_start;
    size_t link_start;
    size_t control_start;
    size_t box_start;
} GridLaidItem;

typedef struct {
    GridLaidItem *items;
    size_t count;
    size_t capacity;
} GridLaidItems;

static void grid_laid_items_release(LayoutContext *context,
                                    GridLaidItems *laid)
{
    if (laid->items != NULL) budget_free(context->layout->budget, laid->items);
    *laid = (GridLaidItems) {0};
}

/* Row `grown_row` grew by `grown` after items in later rows were laid out:
   move exactly those items' output down. Each translation runs to the end
   of the output arrays, so walk the items in layout order and apply the
   change in the required cumulative offset at each item's start. Returns
   false when the record is incomplete (the caller keeps later rows fixed,
   as before). */
static bool grid_shift_later_rows(LayoutContext *context,
                                  const GridLaidItems *laid,
                                  int grown_row, int grown)
{
    if (laid->items == NULL || grown == 0) return false;
    int applied = 0;
    for (size_t i = 0; i < laid->count; i++) {
        const GridLaidItem *item = &laid->items[i];
        int needed = item->row > grown_row ? grown : 0;
        if (needed == applied) continue;
        layout_translate_range(
            context->layout, item->command_start, item->link_start,
            item->control_start, item->box_start, 0, needed - applied,
            "grid-row-growth", NULL);
        applied = needed;
    }
    return true;
}

/*
 * Rows are sized before content-sized rows are measured (only items with a
 * declared height seed them), so once every item is laid out a definite
 * grid's rows are resolved again against the final heights and the items
 * are moved with their rows:
 *
 * - Overflowing, its fr rows share the space again, shrinking only and
 *   never below their minimum or content. chatgpt.com's page grid
 *   (minmax(4.5rem,1fr) auto 1fr) otherwise pushed its composer onto the
 *   disclaimer in the last row.
 * - Underfilled, with align-content normal/stretch and no fr rows, its
 *   auto rows grow equally to fill it (CSS Grid "stretch auto tracks"), so
 *   an item aligned to the centre of a one-row icon button is centred.
 *
 * End- and centre-aligned items are realigned in a resized row. A
 * stretched item cannot shrink with its row, so a grid that would shrink
 * one is left as laid out.
 */
static void grid_resolve_final_rows(
    LayoutContext *context, const ComputedStyle *style,
    int *row_starts, int *row_extents, int rows,
    int declared_content_height, int grid_row_origin,
    int explicit_grid_rows, int declared_grid_rows,
    uint16_t second_implicit_row,
    const GridReshareItem *items, size_t item_count)
{
    if (rows <= 0 || rows > GRID_PLACEMENT_ROW_LIMIT || item_count == 0)
        return;
    int total = row_extents[rows - 1] - row_starts[0];
    if (total == declared_content_height) return;
    int heights[GRID_PLACEMENT_ROW_LIMIT];
    int floors[GRID_PLACEMENT_ROW_LIMIT];
    unsigned weights[GRID_PLACEMENT_ROW_LIMIT];
    bool automatic[GRID_PLACEMENT_ROW_LIMIT];
    bool stretched[GRID_PLACEMENT_ROW_LIMIT];
    int available = declared_content_height;
    unsigned total_weight = 0;
    int automatic_rows = 0;
    for (int row = 0; row < rows; row++) {
        heights[row] = row_extents[row] - row_starts[row];
        floors[row] = 0;
        weights[row] = 0;
        stretched[row] = false;
        if (row > 0) available -= row_starts[row] - row_extents[row - 1];
        int explicit_row = row - grid_row_origin;
        uint8_t type = GRID_TRACK_AUTO;
        unsigned value = 0;
        if (explicit_row >= 0 && explicit_row < explicit_grid_rows) {
            grid_defined_row_track(
                context->sheet, style, (unsigned) explicit_row,
                (unsigned) declared_grid_rows, second_implicit_row,
                &type, &value);
        } else {
            int implicit = explicit_row < 0
                ? explicit_row : explicit_row - explicit_grid_rows;
            bool second = second_implicit_row != 0 && (implicit & 1) != 0;
            type = second ? (uint8_t) (second_implicit_row >> 14)
                          : style->grid_auto_row_type;
            value = second ? second_implicit_row & 0x3fffu
                           : style->grid_auto_row_value;
        }
        automatic[row] = type == GRID_TRACK_AUTO;
        if (automatic[row]) automatic_rows++;
        if (type == GRID_TRACK_FLEX && value != 0) {
            weights[row] = value;
            total_weight += value;
            if (explicit_row >= 0 && explicit_row < explicit_grid_rows)
                floors[row] = (int) stylesheet_grid_track_minimum(
                    context->sheet, style, true, (unsigned) explicit_row);
        } else {
            available -= heights[row];
        }
    }
    for (size_t i = 0; i < item_count; i++) {
        const GridReshareItem *item = &items[i];
        if (item->extent > floors[item->row])
            floors[item->row] = item->extent;
        if (item->alignment == ALIGN_STRETCH) stretched[item->row] = true;
    }
    int resized[GRID_PLACEMENT_ROW_LIMIT];
    for (int row = 0; row < rows; row++) resized[row] = heights[row];
    bool changed = false;
    if (total > declared_content_height && total_weight != 0) {
        if (available < 0) available = 0;
        bool floored[GRID_PLACEMENT_ROW_LIMIT] = {false};
        unsigned sharing_weight = total_weight;
        int sharing_space = available;
        for (int pass = 0; pass < rows; pass++) {
            bool froze = false;
            for (int row = 0; row < rows; row++) {
                if (weights[row] == 0 || floored[row]
                    || sharing_weight == 0) continue;
                if ((int64_t) floors[row] * sharing_weight
                        <= (int64_t) sharing_space * weights[row]) continue;
                floored[row] = true;
                sharing_space -= floors[row];
                if (sharing_space < 0) sharing_space = 0;
                sharing_weight -= weights[row];
                froze = true;
            }
            if (!froze) break;
        }
        for (int row = 0; row < rows; row++) {
            if (weights[row] == 0) continue;
            int share = floored[row] || sharing_weight == 0 ? floors[row]
                : tilefinch_mul_div_int(sharing_space, (int) weights[row],
                                        (int) sharing_weight);
            if (share < floors[row]) share = floors[row];
            if (share < heights[row]) {
                if (stretched[row]) return;
                resized[row] = share;
                changed = true;
            }
        }
    } else if (total < declared_content_height && total_weight == 0
               && automatic_rows != 0
               && style->align_content == JUSTIFY_STRETCH) {
        int free_space = declared_content_height - total;
        int remaining = free_space;
        for (int row = 0; row < rows; row++) {
            if (!automatic[row]) continue;
            int grow = free_space / automatic_rows;
            if (remaining > grow * automatic_rows) {
                grow++;
                remaining--;
            }
            resized[row] += grow;
            changed = true;
        }
    }
    if (!changed) return;
    int starts[GRID_PLACEMENT_ROW_LIMIT];
    starts[0] = row_starts[0];
    for (int row = 1; row < rows; row++) {
        starts[row] = starts[row - 1] + resized[row - 1]
            + (row_starts[row] - row_extents[row - 1]);
    }
    int applied = 0;
    for (size_t i = 0; i < item_count; i++) {
        const GridReshareItem *item = &items[i];
        int dy = item->dy;
        if (resized[item->row] != heights[item->row]) {
            int free_space = resized[item->row] - item->extent;
            if (free_space < 0) free_space = 0;
            if (item->alignment == ALIGN_END) dy = free_space;
            else if (item->alignment == ALIGN_CENTER) dy = free_space / 2;
        }
        int needed = starts[item->row] - row_starts[item->row]
            + dy - item->dy;
        if (needed != applied) {
            layout_translate_range(
                context->layout, item->command_start, item->link_start,
                item->control_start, item->box_start, 0, needed - applied,
                "grid-final-rows", NULL);
            applied = needed;
        }
    }
    for (int row = 0; row < rows; row++) {
        row_starts[row] = starts[row];
        row_extents[row] = starts[row] + resized[row];
    }
}

bool layout_block_grid_section(LayoutContext *context,
                               const LayoutBlockFrame *frame)
{
    if (frame == NULL || !frame->grid) return true;
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
    bool definite_height = frame->definite_height;
    bool flex_track_space_definite = definite_height
        || frame->grid_minimum_block_size;
    bool replaced_content = frame->replaced_content;
    bool grid = frame->grid;
    size_t grid_children = 0;
    size_t grid_positioned_children = 0;
    size_t grid_locked_children = 0;
    const LayoutAssignedGridTracks *inherited_grid =
        context->assigned_grid_tracks.node == node
        ? &context->assigned_grid_tracks : NULL;
    bool inherited_grid_columns = inherited_grid != NULL
        && computed_style_grid_subgrid_columns(style)
        && inherited_grid->column_count != 0;
    bool inherited_grid_rows = inherited_grid != NULL
        && computed_style_grid_subgrid_rows(style)
        && inherited_grid->row_count != 0;
    int grid_column_gap = inherited_grid_columns
        && !computed_style_grid_column_gap_specified(style)
        ? inherited_grid->column_gap : style->gap;
    int grid_row_gap_base = inherited_grid_rows
        && !computed_style_grid_row_gap_specified(style)
        ? inherited_grid->row_gap : style->row_gap;
    int declared_grid_columns = inherited_grid_columns
        ? inherited_grid->column_count
        : (int) stylesheet_grid_track_count(
            context->sheet, style, false);
    int explicit_grid_columns = declared_grid_columns;
    unsigned named_area_columns =
        stylesheet_grid_template_area_columns(context->sheet, style);
    if (!inherited_grid_columns
        && (unsigned) explicit_grid_columns < named_area_columns) {
        explicit_grid_columns = (int) named_area_columns;
    }
    int declared_grid_rows = inherited_grid_rows
        ? inherited_grid->row_count
        : (int) stylesheet_grid_track_count(
            context->sheet, style, true);
    int explicit_grid_rows = declared_grid_rows;
    unsigned named_area_rows =
        stylesheet_grid_template_area_rows(context->sheet, style);
    if (!inherited_grid_rows
        && (unsigned) explicit_grid_rows < named_area_rows) {
        explicit_grid_rows = (int) named_area_rows;
    }
    int grid_min_column = 0;
    int grid_max_column = explicit_grid_columns;
    int grid_min_row = 0;
    int grid_max_row = explicit_grid_rows;
    if (grid && !replaced_content) {
        FlatItemIterator *iterator = &scratch->traversal.flat.iterator;
        FlatItem *item = &scratch->traversal.flat.item;
        flat_iterator_init(iterator, context, node, style);
        while (flat_iterator_next(iterator, item)) {
            grid_resolve_item_placement(
                context, node, style, &item->style);
            if (item->style.out_of_flow || item->style.fixed_position) {
                if (item->style.out_of_flow
                    && !item->style.fixed_position) {
                    grid_positioned_children++;
                }
                continue;
            }
            grid_children++;
            if (grid_item_locked_to_major_axis(&item->style, style)) {
                grid_locked_children++;
            }
            grid_axis_extent(
                computed_style_grid_column_start(&item->style),
                computed_style_grid_column_end(&item->style),
                computed_style_grid_column_span(&item->style),
                explicit_grid_columns, &grid_min_column, &grid_max_column);
            grid_axis_extent(
                computed_style_grid_row_start(&item->style),
                computed_style_grid_row_end(&item->style),
                computed_style_grid_row_span(&item->style),
                explicit_grid_rows, &grid_min_row, &grid_max_row);
        }
    }
    if (inherited_grid_columns) {
        grid_min_column = 0;
        grid_max_column = explicit_grid_columns;
    }
    if (inherited_grid_rows) {
        grid_min_row = 0;
        grid_max_row = explicit_grid_rows;
    }
    if (grid && (grid_children != 0 || grid_positioned_children != 0)
        && !replaced_content) {
        layout_flush_line(line);
        int columns = grid_max_column - grid_min_column;
        int grid_column_origin = -grid_min_column;
        int grid_row_origin = -grid_min_row;
        bool adaptive_columns = columns == 0
                                && style->grid_min_column_width > 0;
        if (columns == 0 && style->grid_min_column_width > 0) {
            columns = (content_width + grid_column_gap)
                      / (style->grid_min_column_width + grid_column_gap);
        }
        /* Column tracks that really exist. A grid whose only children are
           absolutely positioned has none, but layout still runs one
           placeholder column; an out-of-flow item must not take that
           zero-width placeholder as its grid area. */
        int track_columns = columns;
        if (columns < 1) columns = 1;
        int placement_rows = grid_max_row - grid_min_row;
        if (placement_rows < 1) placement_rows = 1;
        if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
        if (placement_rows > GRID_PLACEMENT_ROW_LIMIT) {
            placement_rows = GRID_PLACEMENT_ROW_LIMIT;
        }
        if (grid_column_origin >= columns) grid_column_origin = columns - 1;
        if (grid_row_origin >= placement_rows) {
            grid_row_origin = placement_rows - 1;
        }
        if (!inherited_grid_columns
            && computed_style_grid_auto_flow_column(style)) {
            int flow_columns =
                ((int) grid_children + placement_rows - 1) / placement_rows;
            if (flow_columns > columns) columns = flow_columns;
        }
        if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
        if (adaptive_columns && (size_t) columns > grid_children) {
            columns = (int) grid_children;
        }
        int available =
            content_width - grid_column_gap * (columns - 1);
        if (available < columns * 8) available = columns * 8;
        int track_widths[GRID_TRACK_LIMIT] = {0};
        int track_floors[GRID_TRACK_LIMIT] = {0};
        int track_starts[GRID_TRACK_LIMIT] = {0};
        uint8_t track_types[GRID_TRACK_LIMIT] = {0};
        uint16_t track_values[GRID_TRACK_LIMIT] = {0};
        FlexOrderPlan *grid_order = &scratch->row_order;
        *grid_order = (FlexOrderPlan) {0};
        if (!flex_order_plan_build(grid_order, context, node, style)) {
            return false;
        }
        unsigned flex_weight = 0;
        for (int column = 0; column < columns; column++) {
            int explicit_column = column - grid_column_origin;
            /*
             * grid-template-areas can enlarge the explicit grid beyond the
             * tracks named by grid-template-columns. Those extra tracks are
             * explicit for line placement, but grid-auto-columns supplies
             * their sizing function just as it does for ordinary implicit
             * tracks.
             */
            bool auto_sized_column = !adaptive_columns
                && (explicit_column < 0
                    || explicit_column >= declared_grid_columns);
            uint16_t implicit_column_second =
                !inherited_grid_columns
                ? style->grid_auto_column_second : 0;
            int auto_column_index = explicit_column < 0
                ? explicit_column
                : explicit_column - declared_grid_columns;
            bool use_second_implicit_column =
                auto_sized_column && implicit_column_second != 0
                && (auto_column_index & 1) != 0;
            uint8_t type = adaptive_columns
                ? GRID_TRACK_FLEX
                : (use_second_implicit_column
                   ? (uint8_t) (implicit_column_second >> 14)
                   : (auto_sized_column
                   ? style->grid_auto_column_type
                   : stylesheet_grid_track_type(
                       context->sheet, style, false,
                       (unsigned) explicit_column)));
            unsigned value = adaptive_columns ? 1000u
                : (use_second_implicit_column
                   ? implicit_column_second & 0x3fffu
                   : (auto_sized_column
                      ? style->grid_auto_column_value
                      : stylesheet_grid_track_value(
                          context->sheet, style, false,
                          (unsigned) explicit_column)));
            int minimum = adaptive_columns ? style->grid_min_column_width
                          : (auto_sized_column ? 0
                             : (int) stylesheet_grid_track_minimum(
                                 context->sheet, style, false,
                                 (unsigned) explicit_column));
            if (inherited_grid_columns && explicit_column >= 0
                && explicit_column < inherited_grid->column_count) {
                type = GRID_TRACK_FIXED;
                value = (unsigned) grid_subgrid_track_size(
                    inherited_grid->column_widths[explicit_column],
                    inherited_grid->column_gap, grid_column_gap,
                    (unsigned) explicit_column,
                    inherited_grid->column_count);
                int edge_inset = 0;
                if (explicit_column == 0) {
                    edge_inset += style->padding.left
                                  + style->border.left;
                }
                if (explicit_column + 1
                    == inherited_grid->column_count) {
                    edge_inset += style->padding.right
                                  + style->border.right;
                }
                if ((unsigned) edge_inset >= value) value = 1;
                else value -= (unsigned) edge_inset;
                minimum = (int) value;
            }
            track_types[column] = type;
            track_values[column] = value > UINT16_MAX
                ? UINT16_MAX : (uint16_t) value;
            track_floors[column] = minimum;
            if (type == GRID_TRACK_FIXED) {
                track_widths[column] = (int) value;
                track_floors[column] = track_widths[column];
            } else if (type == GRID_TRACK_PERCENT) {
                track_widths[column] = tilefinch_mul_div_int(
                    available, (int) value, 10000);
                track_floors[column] = track_widths[column];
            } else if (type == GRID_TRACK_FLEX) {
                track_widths[column] = 0;
                flex_weight += value;
            }
            if (type != GRID_TRACK_FLEX
                && track_widths[column] < track_floors[column]) {
                track_widths[column] = track_floors[column];
            }
        }
        /* Every placement pass below starts from the same state: the
           explicit grid plus, when any item is locked to a major-axis
           track, the areas CSS Grid 8.5 places before auto-placement. */
        GridPlacementState placement_template;
        grid_placement_init(&placement_template, columns, placement_rows,
                            style);
        placement_template.explicit_columns =
            (uint8_t) explicit_grid_columns;
        placement_template.explicit_rows = (uint8_t) explicit_grid_rows;
        placement_template.column_origin = (uint8_t) grid_column_origin;
        placement_template.row_origin = (uint8_t) grid_row_origin;
        if (grid_locked_children != 0
            && !grid_placement_reserve(
                   &placement_template, context, node, style, grid_order,
                   grid_resolve_item_placement,
                   &scratch->traversal.flex.iterator,
                   &scratch->traversal.flex.item)) {
            flex_order_plan_destroy(grid_order);
            return false;
        }
        GridPlacementState measured_placement = placement_template;
        FlexItemIterator *measurement =
            &scratch->traversal.flex.iterator;
        FlatItem *measured_item = &scratch->traversal.flex.item;
        flex_iterator_init(measurement, context, node, style, grid_order);
        while (flex_iterator_next(measurement, measured_item)) {
            grid_resolve_item_placement(
                context, node, style, &measured_item->style);
            if (measured_item->style.out_of_flow
                || measured_item->style.fixed_position) continue;
            GridItemPlacement item_placement;
            (void) grid_place_item(
                &measured_placement, &measured_item->style,
                &item_placement);
            int item_start = item_placement.column;
            int item_span = item_placement.column_span;
            if (!measured_item->anonymous_text
                && computed_style_grid_subgrid_columns(
                    &measured_item->style)) {
                int local_minimums[GRID_TRACK_LIMIT] = {0};
                int local_maximums[GRID_TRACK_LIMIT] = {0};
                int local_rows = computed_style_grid_subgrid_rows(
                    &measured_item->style)
                    ? item_placement.row_span
                    : (int) stylesheet_grid_track_count(
                        context->sheet, &measured_item->style, true);
                if (!intrinsic_grid_subgrid_column_requirements(
                        context, measured_item->node,
                        &measured_item->style, item_span, local_rows,
                        available, local_minimums, local_maximums)) {
                    flex_order_plan_destroy(grid_order);
                    return false;
                }
                ComputedStyle resolved_subgrid = measured_item->style;
                resolve_padding(
                    context->sheet, &resolved_subgrid, available);
                resolve_margin(
                    context->sheet, &resolved_subgrid, available);
                int local_gap = computed_style_resolve_gap(
                    resolved_subgrid.gap, available);
                int difference = grid_column_gap - local_gap;
                int before = difference / 2;
                int after = difference - before;
                int required_floors[GRID_TRACK_LIMIT] = {0};
                int required_preferred[GRID_TRACK_LIMIT] = {0};
                for (int offset = 0; offset < item_span; offset++) {
                    int column = item_start + offset;
                    if (column < 0 || column >= columns
                        || track_types[column] == GRID_TRACK_FIXED
                        || track_types[column] == GRID_TRACK_PERCENT) {
                        continue;
                    }
                    int adjustment = 0;
                    if (offset != 0) adjustment += after;
                    if (offset + 1 < item_span) adjustment += before;
                    int edge = 0;
                    if (offset == 0) {
                        edge += resolved_subgrid.padding.left
                                + resolved_subgrid.border.left
                                + resolved_subgrid.margin.left;
                    }
                    if (offset + 1 == item_span) {
                        edge += resolved_subgrid.padding.right
                                + resolved_subgrid.border.right
                                + resolved_subgrid.margin.right;
                    }
                    int floor = local_minimums[offset]
                                - adjustment + edge;
                    int preferred = local_maximums[offset]
                                    - adjustment + edge;
                    if (floor < 0) floor = 0;
                    if (preferred < floor) preferred = floor;
                    required_floors[offset] = floor;
                    required_preferred[offset] = preferred;
                }
                /*
                 * Intrinsic text advances are retained in 26.6 but each
                 * local track arrives here as an integer ceiling.  With a
                 * changed subgrid gutter, summing those independent ceilings
                 * biases the leading edge by one pixel per internal line.
                 * Reconcile the fixed total in source order so the parent
                 * and subgrid gutter centers remain aligned.
                 */
                if (local_gap != grid_column_gap && item_span > 1) {
                    for (int offset = 1; offset < item_span; offset++) {
                        if (required_floors[0] > 0) {
                            required_floors[0]--;
                            required_floors[offset]++;
                        }
                        if (required_preferred[0] > 0) {
                            required_preferred[0]--;
                            required_preferred[offset]++;
                        }
                    }
                }
                for (int offset = 0; offset < item_span; offset++) {
                    int column = item_start + offset;
                    if (column < 0 || column >= columns
                        || track_types[column] == GRID_TRACK_FIXED
                        || track_types[column] == GRID_TRACK_PERCENT) {
                        continue;
                    }
                    int floor = required_floors[offset];
                    int preferred = required_preferred[offset];
                    if (floor > track_floors[column]) {
                        track_floors[column] = floor;
                    }
                    if (track_types[column] == GRID_TRACK_AUTO
                        && preferred > track_widths[column]) {
                        track_widths[column] = preferred;
                    }
                }
                continue;
            }
            int intrinsic_tracks = 0;
            int minimum_occupied =
                grid_column_gap * (item_span - 1);
            for (int offset = 0; offset < item_span; offset++) {
                int column = item_start + offset;
                if (track_types[column] == GRID_TRACK_AUTO) {
                    intrinsic_tracks++;
                    minimum_occupied += track_floors[column];
                } else {
                    int contribution = track_widths[column];
                    if (track_floors[column] > contribution) {
                        contribution = track_floors[column];
                    }
                    minimum_occupied += contribution;
                }
            }
            if (intrinsic_tracks > 0) {
                int preferred = intrinsic_text_width(
                    context, measured_item->node,
                    &measured_item->parent_style, available);
                int floor = grid_item_minimum_contribution(
                    context, measured_item, available,
                    item_start, item_span, track_types);
                grid_distribute_intrinsic_extra(
                    item_start, item_span,
                    floor > minimum_occupied
                        ? floor - minimum_occupied : 0,
                    false, track_types, track_values, track_floors);
                for (int offset = 0; offset < item_span; offset++) {
                    int column = item_start + offset;
                    if (track_types[column] != GRID_TRACK_AUTO) continue;
                    if (track_widths[column] < track_floors[column]) {
                        track_widths[column] = track_floors[column];
                    }
                }
                int preferred_occupied =
                    grid_column_gap * (item_span - 1);
                for (int offset = 0; offset < item_span; offset++) {
                    preferred_occupied +=
                        track_widths[item_start + offset];
                }
                grid_distribute_intrinsic_extra(
                    item_start, item_span,
                    preferred > preferred_occupied
                        ? preferred - preferred_occupied : 0,
                    true, track_types, track_values, track_widths);
                for (int offset = 0; offset < item_span; offset++) {
                    int column = item_start + offset;
                    if (track_types[column] == GRID_TRACK_AUTO
                        && track_values[column]
                           == GRID_TRACK_MAX_CONTENT_VALUE
                        && track_floors[column] < track_widths[column]) {
                        track_floors[column] = track_widths[column];
                    }
                }
            }
        }
        int occupied = 0;
        for (int column = 0; column < columns; column++) {
            if (track_widths[column] < 0) track_widths[column] = 0;
            if (track_floors[column] < 0) track_floors[column] = 0;
            if (track_types[column] != GRID_TRACK_FLEX
                && track_floors[column] > track_widths[column]) {
                /* A specified min-width contributes to an automatic track's
                   base size even when it exceeds the item's max-content
                   text width.  Shrinking the floor to max-content lets the
                   item overflow a centered track and visibly shifts it. */
                track_widths[column] = track_floors[column];
            }
            occupied += track_widths[column];
        }
        if (occupied > available) {
            int deficit = occupied - available;
            int shrinkable = 0;
            for (int column = 0; column < columns; column++) {
                if (track_types[column] == GRID_TRACK_AUTO) {
                    shrinkable += track_widths[column]
                                  - track_floors[column];
                }
            }
            int target = deficit < shrinkable ? deficit : shrinkable;
            int removed = 0;
            if (target > 0 && shrinkable > 0) {
                for (int column = 0; column < columns; column++) {
                    if (track_types[column] != GRID_TRACK_AUTO) continue;
                    int capacity = track_widths[column]
                                   - track_floors[column];
                    int take = tilefinch_mul_div_int(
                        target, capacity, shrinkable);
                    if (take > capacity) take = capacity;
                    track_widths[column] -= take;
                    removed += take;
                }
                for (int column = 0; removed < target && column < columns;
                     column++) {
                    if (track_widths[column] > track_floors[column]) {
                        track_widths[column]--;
                        removed++;
                    }
                }
            }
            occupied -= removed;
        }
        int remaining = available - occupied;
        if (flex_weight != 0) {
            bool locked[GRID_TRACK_LIMIT] = {false};
            unsigned active_weight = flex_weight;
            int flex_space = remaining > 0 ? remaining : 0;
            for (int pass = 0; pass < columns && active_weight != 0; pass++) {
                bool changed = false;
                for (int column = 0; column < columns; column++) {
                    if (track_types[column] != GRID_TRACK_FLEX
                        || locked[column]) continue;
                    unsigned weight = adaptive_columns ? 1000u
                                      : track_values[column];
                    if (weight == 0) continue;
                    int share = flex_space > 0
                        ? tilefinch_mul_div_int(
                              flex_space, (int) weight,
                              (int) active_weight)
                        : 0;
                    if (share >= track_floors[column]) continue;
                    track_widths[column] = track_floors[column];
                    flex_space -= track_widths[column];
                    active_weight -= weight;
                    locked[column] = true;
                    changed = true;
                }
                if (!changed) break;
            }
            if (flex_space < 0) flex_space = 0;
            if (active_weight != 0) {
                int distributed = 0;
                for (int column = 0; column < columns; column++) {
                    if (track_types[column] != GRID_TRACK_FLEX
                        || locked[column]) continue;
                    unsigned weight = adaptive_columns ? 1000u
                                      : track_values[column];
                    if (weight == 0) continue;
                    int addition = tilefinch_mul_div_int(
                        flex_space, (int) weight, (int) active_weight);
                    track_widths[column] = addition;
                    distributed += addition;
                }
                for (int column = 0; distributed < flex_space
                                     && column < columns; column++) {
                    if (track_types[column] == GRID_TRACK_FLEX
                        && track_values[column] != 0
                        && !locked[column]) {
                        track_widths[column]++;
                        distributed++;
                    }
                }
            }
        } else if (remaining > 0
                   && (style->justify_content == JUSTIFY_STRETCH
                       /* The initial "normal" behaves as stretch for grid
                          (CSS Box Alignment 5.1): auto tracks fill the
                          container. Without it a grid's single auto column
                          shrank to its content (ChatGPT's 456px composer
                          column rendered 328px wide). */
                       || style->justify_content == JUSTIFY_START)) {
            int automatic = 0;
            for (int column = 0; column < columns; column++) {
                if (grid_track_stretches(
                        track_types[column], track_values[column])) {
                    automatic++;
                }
            }
            if (automatic > 0) {
                int each = remaining / automatic;
                int remainder = remaining - each * automatic;
                for (int column = 0; column < columns; column++) {
                    if (!grid_track_stretches(
                            track_types[column], track_values[column])) {
                        continue;
                    }
                    track_widths[column] += each;
                    if (remainder > 0) {
                        track_widths[column]++;
                        remainder--;
                    }
                }
            }
        }
        int final_occupied = 0;
        for (int column = 0; column < columns; column++) {
            final_occupied += track_widths[column];
        }
        int free_space = available - final_occupied;
        if (free_space < 0) free_space = 0;
        int distributed_gap = grid_column_gap;
        int leading_space = 0;
        if (!inherited_grid_columns
            && style->justify_content == JUSTIFY_CENTER) {
            leading_space = free_space / 2;
        } else if (!inherited_grid_columns
                   && style->justify_content == JUSTIFY_END) {
            leading_space = free_space;
        } else if (!inherited_grid_columns
                   && style->justify_content == JUSTIFY_SPACE_BETWEEN
                   && columns > 1) {
            distributed_gap += free_space / (columns - 1);
        } else if (!inherited_grid_columns
                   && style->justify_content == JUSTIFY_SPACE_AROUND) {
            int share = free_space / columns;
            leading_space = share / 2;
            distributed_gap += share;
        } else if (!inherited_grid_columns
                   && style->justify_content == JUSTIFY_SPACE_EVENLY) {
            int share = free_space / (columns + 1);
            leading_space = share;
            distributed_gap += share;
        }
        int track_cursor = content_x + leading_space;
        for (int column = 0; column < columns; column++) {
            /* An authored 0px or 0fr track stays empty: Guardian's grids
               bracket their content with `0px` edge tracks. */
            if (track_widths[column] < 1
                && !((track_types[column] == GRID_TRACK_FLEX
                      || track_types[column] == GRID_TRACK_FIXED)
                     && track_values[column] == 0)) {
                track_widths[column] = 1;
            }
            track_starts[column] = track_cursor;
            track_cursor += track_widths[column] + distributed_gap;
        }
        int row_track_heights[GRID_PLACEMENT_ROW_LIMIT] = {0};
        unsigned explicit_rows = (unsigned) explicit_grid_rows;
        int uniform_row_height = 0;
        if (!inherited_grid_rows && explicit_rows != 0) {
            uint8_t first_type = stylesheet_grid_track_type(
                context->sheet, style, true, 0);
            unsigned first_value = stylesheet_grid_track_value(
                context->sheet, style, true, 0);
            bool uniform = first_type == GRID_TRACK_FIXED && first_value != 0;
            for (unsigned row = 1; uniform && row < explicit_rows; row++) {
                uniform = stylesheet_grid_track_type(
                              context->sheet, style, true, row)
                              == GRID_TRACK_FIXED
                    && stylesheet_grid_track_value(
                           context->sheet, style, true, row) == first_value;
            }
            if (uniform) uniform_row_height = (int) first_value;
        }
        uint16_t second_implicit_row =
            !inherited_grid_rows ? style->grid_auto_row_second : 0;
        if (inherited_grid_rows) {
            for (unsigned row = 0;
                 row < explicit_rows
                 && row < LAYOUT_ASSIGNED_GRID_TRACK_LIMIT; row++) {
                int inherited_height = inherited_grid->row_heights[row];
                if (inherited_height <= 0) {
                    /*
                     * An unresolved automatic parent row stays indefinite
                     * during the subgrid's child layout.  Coercing it to one
                     * pixel makes stretch replace the descendant's natural
                     * height; preserving zero lets the ordinary row extent
                     * feed that natural contribution back to the parent.
                     */
                    row_track_heights[row] = 0;
                    continue;
                }
                row_track_heights[row] = grid_subgrid_track_size(
                    inherited_height, inherited_grid->row_gap,
                    grid_row_gap_base, row, explicit_rows);
                int edge_inset = 0;
                if (row == 0) {
                    edge_inset += style->padding.top + style->border.top;
                }
                if (row + 1u == explicit_rows) {
                    edge_inset += style->padding.bottom
                                  + style->border.bottom;
                }
                if (row_track_heights[row] <= edge_inset) {
                    row_track_heights[row] = 1;
                } else {
                    row_track_heights[row] -= edge_inset;
                }
            }
        } else if (uniform_row_height == 0) {
            int row_available = declared_content_height
                - grid_row_gap_base * ((int) explicit_rows - 1);
            if (row_available < 0) row_available = 0;
            int fixed_space = 0;
            unsigned flex_rows = 0;
            for (unsigned row = 0; row < explicit_rows; row++) {
                uint8_t type = GRID_TRACK_AUTO;
                unsigned value = 0;
                grid_defined_row_track(
                    context->sheet, style, row,
                    (unsigned) declared_grid_rows,
                    second_implicit_row, &type, &value);
                unsigned minimum = stylesheet_grid_track_minimum(
                    context->sheet, style, true, row);
                if (minimum != 0) {
                    row_track_heights[row] = (int) minimum;
                    /* A flexible row's minimum is a floor on its share of
                       the free space, not space taken before sharing. */
                    if (type != GRID_TRACK_FLEX)
                        fixed_space += row_track_heights[row];
                }
                if (type == GRID_TRACK_FIXED) {
                    row_track_heights[row] = (int) value;
                    if (minimum == 0) fixed_space += row_track_heights[row];
                } else if (type == GRID_TRACK_PERCENT
                           && definite_height) {
                    row_track_heights[row] = tilefinch_mul_div_int(
                        declared_content_height, (int) value, 100);
                    if (minimum == 0) fixed_space += row_track_heights[row];
                } else if (type == GRID_TRACK_FLEX) {
                    flex_rows += value;
                }
            }
            if (flex_track_space_definite && flex_rows != 0) {
                grid_seed_definite_auto_row_contributions(
                    context, node, style, scratch, grid_order,
                    &placement_template, placement_rows,
                    grid_row_origin, explicit_grid_rows, declared_grid_rows,
                    second_implicit_row, content_width,
                    definite_height ? declared_content_height : 0,
                    row_track_heights, &fixed_space);
            }
            int flex_space = row_available - fixed_space;
            if (flex_space < 0) flex_space = 0;
            if (flex_track_space_definite && flex_rows != 0) {
                /* CSS Grid "find the size of an fr": a flexible row whose
                   minimum exceeds its share keeps the minimum and leaves
                   the sharing, and the rest re-share what remains. Each
                   pass freezes at least one row, so explicit_rows passes
                   suffice. */
                bool floored[GRID_PLACEMENT_ROW_LIMIT] = {false};
                unsigned sharing_weight = flex_rows;
                int sharing_space = flex_space;
                for (unsigned pass = 0; pass < explicit_rows; pass++) {
                    bool froze = false;
                    for (unsigned row = 0; row < explicit_rows
                         && row < GRID_PLACEMENT_ROW_LIMIT; row++) {
                        uint8_t type = GRID_TRACK_AUTO;
                        unsigned value = 0;
                        grid_defined_row_track(
                            context->sheet, style, row,
                            (unsigned) declared_grid_rows,
                            second_implicit_row, &type, &value);
                        if (type != GRID_TRACK_FLEX || value == 0
                            || floored[row] || sharing_weight == 0)
                            continue;
                        unsigned minimum = stylesheet_grid_track_minimum(
                            context->sheet, style, true, row);
                        if (minimum == 0
                            || (int64_t) minimum * sharing_weight
                                   <= (int64_t) sharing_space * value)
                            continue;
                        floored[row] = true;
                        row_track_heights[row] = (int) minimum;
                        sharing_space -= (int) minimum;
                        if (sharing_space < 0) sharing_space = 0;
                        sharing_weight -= value;
                        froze = true;
                    }
                    if (!froze) break;
                }
                int distributed = 0;
                for (unsigned row = 0; row < explicit_rows
                     && row < GRID_PLACEMENT_ROW_LIMIT; row++) {
                    uint8_t type = GRID_TRACK_AUTO;
                    unsigned value = 0;
                    grid_defined_row_track(
                        context->sheet, style, row,
                        (unsigned) declared_grid_rows,
                        second_implicit_row, &type, &value);
                    if (type != GRID_TRACK_FLEX || floored[row]) continue;
                    unsigned weight = value;
                    if (weight == 0 || sharing_weight == 0) continue;
                    row_track_heights[row] = tilefinch_mul_div_int(
                        sharing_space, (int) weight, (int) sharing_weight);
                    distributed += row_track_heights[row];
                }
                for (unsigned row = 0; distributed < sharing_space
                     && row < explicit_rows
                     && row < GRID_PLACEMENT_ROW_LIMIT; row++) {
                    uint8_t type = GRID_TRACK_AUTO;
                    unsigned value = 0;
                    grid_defined_row_track(
                        context->sheet, style, row,
                        (unsigned) declared_grid_rows,
                        second_implicit_row, &type, &value);
                    if (type == GRID_TRACK_FLEX && value != 0
                        && !floored[row]) {
                        row_track_heights[row]++;
                        distributed++;
                    }
                }
            }
        }
        /* placement_rows is structurally capped at 64. Keep a complete
           snapshot because the remapping loop below may overwrite an entry
           before a shifted explicit row reads it. The former eight-entry
           cache indexed past its stack object on a ninth mixed-size row. */
        int explicit_row_heights[GRID_PLACEMENT_ROW_LIMIT] = {0};
        for (int row = 0; row < explicit_grid_rows
                          && row < GRID_PLACEMENT_ROW_LIMIT; row++) {
            explicit_row_heights[row] = uniform_row_height > 0
                ? uniform_row_height : row_track_heights[row];
        }
        for (int row = 0; row < placement_rows; row++) {
            int explicit_row = row - grid_row_origin;
            if (explicit_row >= 0
                && explicit_row < explicit_grid_rows
                && explicit_row < GRID_PLACEMENT_ROW_LIMIT) {
                if (uniform_row_height > 0) {
                    row_track_heights[row] = uniform_row_height;
                } else {
                    row_track_heights[row] =
                        explicit_row_heights[explicit_row];
                }
                continue;
            }
            int implicit = explicit_row < 0
                ? explicit_row : explicit_row - explicit_grid_rows;
            bool second = second_implicit_row != 0
                && (implicit & 1) != 0;
            uint8_t type = second
                ? (uint8_t) (second_implicit_row >> 14)
                : style->grid_auto_row_type;
            unsigned value = second
                ? second_implicit_row & 0x3fffu
                : style->grid_auto_row_value;
            if (type == GRID_TRACK_FIXED) {
                row_track_heights[row] = (int) value;
            }
        }
        int grid_row_gap = grid_row_gap_base;
        int row_leading_space = 0;
        if (!inherited_grid_rows
            && flex_track_space_definite && placement_rows > 0) {
            int occupied_rows = grid_row_gap * (placement_rows - 1);
            for (int row = 0; row < placement_rows; row++) {
                occupied_rows += row_track_heights[row];
            }
            int row_free_space = declared_content_height - occupied_rows;
            if (row_free_space < 0) row_free_space = 0;
            if (style->align_content == JUSTIFY_CENTER) {
                row_leading_space = row_free_space / 2;
            } else if (style->align_content == JUSTIFY_END) {
                row_leading_space = row_free_space;
            } else if (style->align_content == JUSTIFY_SPACE_BETWEEN
                       && placement_rows > 1) {
                grid_row_gap += row_free_space / (placement_rows - 1);
            } else if (style->align_content == JUSTIFY_SPACE_AROUND) {
                int share = row_free_space / placement_rows;
                row_leading_space = share / 2;
                grid_row_gap += share;
            } else if (style->align_content == JUSTIFY_SPACE_EVENLY) {
                int share = row_free_space / (placement_rows + 1);
                row_leading_space = share;
                grid_row_gap += share;
            }
        }
        int row_starts[GRID_PLACEMENT_ROW_LIMIT];
        int row_extents[GRID_PLACEMENT_ROW_LIMIT];
        for (int row = 0; row < GRID_PLACEMENT_ROW_LIMIT; row++) {
            row_starts[row] = -1;
            row_extents[row] = -1;
        }
        row_starts[0] = layout_add_coordinate(
            line->y, row_leading_space);
        row_extents[0] = layout_add_coordinate(
            row_starts[0], row_track_heights[0]);
        int initialized_rows = 1;
        int initially_known_rows =
            grid_row_origin + explicit_grid_rows;
        if (initially_known_rows > placement_rows) {
            initially_known_rows = placement_rows;
        }
        while (initialized_rows < initially_known_rows) {
            int previous = initialized_rows - 1;
            row_starts[initialized_rows] = layout_add_coordinate(
                row_extents[previous], grid_row_gap);
            row_extents[initialized_rows] = layout_add_coordinate(
                row_starts[initialized_rows],
                row_track_heights[initialized_rows]);
            initialized_rows++;
        }
        if (grid_positioned_children != 0) {
            FlatItemIterator *positioned_iterator =
                &scratch->traversal.flat.iterator;
            FlatItem *positioned_item = &scratch->traversal.flat.item;
            flat_iterator_init(positioned_iterator, context, node, style);
            while (flat_iterator_next(positioned_iterator,
                                      positioned_item)) {
                grid_resolve_item_placement(
                    context, node, style, &positioned_item->style);
                if (!positioned_item->style.out_of_flow
                    || positioned_item->style.fixed_position
                    || positioned_item->anonymous_text) {
                    continue;
                }
                int area_x = content_x;
                int area_y = line->y - style->padding.top;
                int area_width = content_width;
                int area_height = declared_content_height
                    + style->padding.top + style->padding.bottom;
                int column_start = computed_style_grid_column_start(
                    &positioned_item->style);
                int column_end = computed_style_grid_column_end(
                    &positioned_item->style);
                /* Lines that do not exist resolve to auto: the padding
                   edge of the grid container (CSS Grid 11.1). ChatGPT's
                   header is the sole, absolutely positioned child of a
                   rows-only grid; it was laid out in a 0px placeholder
                   column and collapsed to its own padding. */
                if (column_start > 0
                    && column_start != COMPUTED_GRID_LINE_LAST
                    && column_start <= track_columns) {
                    int first = column_start - 1;
                    int last = column_end > column_start
                               && column_end != COMPUTED_GRID_LINE_LAST
                        ? column_end - 1 : first + 1;
                    if (last > track_columns) last = track_columns;
                    area_x = track_starts[first];
                    area_width = 0;
                    for (int column = first; column < last; column++) {
                        if (column != first) area_width += distributed_gap;
                        area_width += track_widths[column];
                    }
                }
                int row_start = computed_style_grid_row_start(
                    &positioned_item->style);
                int row_end = computed_style_grid_row_end(
                    &positioned_item->style);
                if (row_start > 0
                    && row_start != COMPUTED_GRID_LINE_LAST
                    && row_start <= initialized_rows) {
                    int first = row_start - 1;
                    int last = row_end > row_start
                               && row_end != COMPUTED_GRID_LINE_LAST
                        ? row_end - 1 : first + 1;
                    if (last > initialized_rows) last = initialized_rows;
                    area_y = row_starts[first];
                    area_height = 0;
                    for (int row = first; row < last; row++) {
                        if (row != first) area_height += grid_row_gap;
                        area_height += row_track_heights[row];
                    }
                }
                if (area_height < 0) area_height = 0;
                PositionedBox grid_area = *descendant_positioned_box;
                grid_area.node = node;
                grid_area.x = area_x;
                grid_area.y = area_y;
                grid_area.width = area_width;
                grid_area.height = area_height;
                int positioned_bottom = area_y;
                if (!layout_block(
                        context, positioned_item->node,
                        &positioned_item->parent_style,
                        area_x, area_y, area_width, area_height, false,
                        &grid_area, &positioned_bottom)) {
                    flex_order_plan_destroy(grid_order);
                    return false;
                }
            }
        }
        GridPlacementState placement_state = placement_template;
        FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
        FlatItem *item = &scratch->traversal.flex.item;
        /* Latest row that already holds a laid-out item: rows after it can
           still move down when an earlier fr row grows to its content. */
        int laid_out_row = -1;
        GridLaidItems laid = {0};
        if (grid_locked_children != 0 && grid_children > 1
            && grid_children <= SIZE_MAX / sizeof(GridLaidItem)) {
            /* Transient and optional: without it an out-of-order row only
               loses the ability to push already laid-out later rows. */
            laid.items = budget_calloc(context->layout->budget,
                                       grid_children, sizeof(*laid.items));
            if (laid.items != NULL) laid.capacity = grid_children;
        }
        GridReshareItem reshare_items[GRID_RESHARE_ITEM_LIMIT];
        size_t reshare_count = 0;
        /* Items that span several content-sized rows are laid out before
           the later rows reach their final height, so their end/center
           block alignment waits for the rows (CSS Grid 10.4). Guardian's
           masthead menu button spans the logo and pillar rows and sits at
           their bottom; aligned at the first row's top it covered the
           edition picker. Bounded: further items keep start alignment. */
        GridSpanAlignItem span_align[GRID_SPAN_ALIGN_LIMIT];
        size_t span_align_count = 0;
        bool reshare_possible = !inherited_grid_rows
            && flex_track_space_definite;
        flex_iterator_init(iterator, context, node, style, grid_order);
        while (flex_iterator_next(iterator, item)) {
            grid_resolve_item_placement(
                context, node, style, &item->style);
            if (item->style.out_of_flow || item->style.fixed_position) {
                continue;
            }
            GridItemPlacement item_placement;
            (void) grid_place_item(
                &placement_state, &item->style, &item_placement);
            int grid_row = item_placement.row;
            while (initialized_rows <= grid_row) {
                int previous = initialized_rows - 1;
                row_starts[initialized_rows] = layout_add_coordinate(
                    row_extents[previous], grid_row_gap);
                row_extents[initialized_rows] = layout_add_coordinate(
                    row_starts[initialized_rows],
                    row_track_heights[initialized_rows]);
                initialized_rows++;
            }
            int row_top = row_starts[grid_row];
            if (layout_preview_limit_reached(context, row_top)) break;
            if (laid.items != NULL) {
                if (laid.count < laid.capacity) {
                    laid.items[laid.count++] = (GridLaidItem) {
                        .row = grid_row,
                        .command_start = context->layout->count,
                        .link_start = context->layout->link_count,
                        .control_start = context->layout->control_count,
                        .box_start = context->layout->node_box_count
                    };
                } else {
                    grid_laid_items_release(context, &laid);
                }
            }
            int column = item_placement.column;
            int column_span = item_placement.column_span;
            int assigned_cell_width =
                distributed_gap * (column_span - 1);
            for (int offset = 0; offset < column_span; offset++) {
                assigned_cell_width += track_widths[column + offset];
            }
            int child_x = track_starts[column];
            int child_bottom = row_top;
            if (item->anonymous_text) {
                reshare_possible = false;
                if (!layout_anonymous_text(context, item, node, child_x,
                                           row_top, assigned_cell_width,
                                           &child_bottom)) {
                    grid_laid_items_release(context, &laid);
                    flex_order_plan_destroy(grid_order);
                    return false;
                }
            } else {
                lxb_dom_node_t *saved_assigned_node =
                    context->assigned_grid_node;
                int saved_assigned_height = context->assigned_grid_height;
                bool saved_assigned_height_valid =
                    context->assigned_grid_height_valid;
                LayoutAssignedGridTracks saved_assigned_tracks =
                    context->assigned_grid_tracks;
                int assigned_cell_height = 0;
                bool definite_cell_height = true;
                size_t item_command_start = context->layout->count;
                size_t item_link_start = context->layout->link_count;
                size_t item_control_start = context->layout->control_count;
                size_t item_box_start = context->layout->node_box_count;
                AlignSelf item_justify_self =
                    computed_style_justify_self(&item->style);
                if (item_justify_self == ALIGN_SELF_AUTO) {
                    item_justify_self =
                        style->justify_items == ALIGN_CENTER
                            ? ALIGN_SELF_CENTER
                        : style->justify_items == ALIGN_END
                            ? ALIGN_SELF_END
                        : style->justify_items == ALIGN_START
                            ? ALIGN_SELF_START
                        : style->justify_items == ALIGN_BASELINE
                            ? ALIGN_SELF_BASELINE
                            : ALIGN_SELF_STRETCH;
                }
                bool inline_auto_margin =
                    item->style.margin_left_auto
                    || item->style.margin_right_auto;
                int grid_child_width = assigned_cell_width;
                if (!item->style.has_width
                    && (item_justify_self != ALIGN_SELF_STRETCH
                        || inline_auto_margin)) {
                    int minimum_width = 0;
                    intrinsic_text_widths(
                        context, item->node, &item->parent_style,
                        assigned_cell_width, &grid_child_width,
                        &minimum_width);
                    if (grid_child_width < minimum_width) {
                        grid_child_width = minimum_width;
                    }
                    if (grid_child_width < 8) grid_child_width = 8;
                    if (grid_child_width > assigned_cell_width) {
                        grid_child_width = assigned_cell_width;
                    }
                }
                for (int offset = 0;
                     offset < item_placement.row_span; offset++) {
                    int row = grid_row + offset;
                    bool collapsed_flex_track = false;
                    if (row < GRID_PLACEMENT_ROW_LIMIT
                        && row_track_heights[row] == 0) {
                        int explicit_row = row - grid_row_origin;
                        uint8_t row_type = GRID_TRACK_AUTO;
                        unsigned row_value = 0;
                        if (explicit_row >= 0
                            && explicit_row < explicit_grid_rows) {
                            grid_defined_row_track(
                                context->sheet, style,
                                (unsigned) explicit_row,
                                (unsigned) declared_grid_rows,
                                second_implicit_row, &row_type, &row_value);
                        } else {
                            int implicit = explicit_row < 0
                                ? explicit_row
                                : explicit_row - explicit_grid_rows;
                            bool second = second_implicit_row != 0
                                && (implicit & 1) != 0;
                            row_type = second
                                ? (uint8_t) (second_implicit_row >> 14)
                                : style->grid_auto_row_type;
                            row_value = second
                                ? second_implicit_row & 0x3fffu
                                : style->grid_auto_row_value;
                        }
                        collapsed_flex_track = row_type == GRID_TRACK_FLEX
                                               && row_value == 0;
                    }
                    if (row >= GRID_PLACEMENT_ROW_LIMIT
                        || (row_track_heights[row] <= 0
                            && !collapsed_flex_track)) {
                        definite_cell_height = false;
                        break;
                    }
                    assigned_cell_height += row_track_heights[row];
                    if (offset != 0) {
                        assigned_cell_height += grid_row_gap;
                    }
                }
                AlignItems item_alignment =
                    flex_item_alignment(style, &item->style);
                if (computed_style_grid_subgrid_columns(&item->style)
                    || computed_style_grid_subgrid_rows(&item->style)) {
                    LayoutAssignedGridTracks assigned = {0};
                    assigned.node = item->node;
                    if (computed_style_grid_subgrid_columns(&item->style)) {
                        assigned.column_count =
                            (uint8_t) column_span;
                        assigned.column_gap = distributed_gap;
                        int local_minimums[GRID_TRACK_LIMIT] = {0};
                        int local_maximums[GRID_TRACK_LIMIT] = {0};
                        int local_rows =
                            computed_style_grid_subgrid_rows(&item->style)
                            ? item_placement.row_span
                            : (int) stylesheet_grid_track_count(
                                context->sheet, &item->style, true);
                        if (!intrinsic_grid_subgrid_column_requirements(
                                context, item->node, &item->style,
                                column_span, local_rows,
                                assigned_cell_width,
                                local_minimums, local_maximums)) {
                            grid_laid_items_release(context, &laid);
                            flex_order_plan_destroy(grid_order);
                            return false;
                        }
                        ComputedStyle resolved_subgrid = item->style;
                        resolve_padding(
                            context->sheet, &resolved_subgrid,
                            assigned_cell_width);
                        int local_gap = computed_style_resolve_gap(
                            resolved_subgrid.gap, assigned_cell_width);
                        int difference = distributed_gap - local_gap;
                        int before = difference / 2;
                        int after = difference - before;
                        int candidate_widths[GRID_TRACK_LIMIT] = {0};
                        int candidate_total =
                            distributed_gap * (column_span - 1);
                        for (int offset = 0;
                             offset < column_span
                             && offset
                                < LAYOUT_ASSIGNED_GRID_TRACK_LIMIT;
                             offset++) {
                            int adjustment = 0;
                            if (offset != 0) adjustment += after;
                            if (offset + 1 < column_span) {
                                adjustment += before;
                            }
                            int edge = 0;
                            if (offset == 0) {
                                edge += resolved_subgrid.padding.left
                                        + resolved_subgrid.border.left;
                            }
                            if (offset + 1 == column_span) {
                                edge += resolved_subgrid.padding.right
                                        + resolved_subgrid.border.right;
                            }
                            int intrinsic_width =
                                local_maximums[offset] - adjustment + edge;
                            bool parent_track_is_intrinsic =
                                track_types[column + offset]
                                    == GRID_TRACK_AUTO
                                || track_types[column + offset]
                                    == GRID_TRACK_FLEX;
                            candidate_widths[offset] =
                                parent_track_is_intrinsic
                                && intrinsic_width > 0
                                    ? intrinsic_width
                                    : track_widths[column + offset];
                            candidate_total += candidate_widths[offset];
                        }
                        /*
                         * When the parent area is exactly the descendant's
                         * intrinsic total, retain the local per-track
                         * rounding that produced that total.  Parent track
                         * sizing has already reconciled the sum, but integer
                         * distribution can otherwise move a pixel across a
                         * changed subgrid gutter.  A larger fixed or
                         * stretched parent area continues to inherit the
                         * parent's settled tracks verbatim.
                         */
                        bool reconcile_intrinsic_rounding =
                            candidate_total == assigned_cell_width;
                        for (int offset = 0;
                             offset < column_span
                             && offset
                                < LAYOUT_ASSIGNED_GRID_TRACK_LIMIT;
                             offset++) {
                            assigned.column_widths[offset] =
                                reconcile_intrinsic_rounding
                                    ? candidate_widths[offset]
                                    : track_widths[column + offset];
                        }
                        for (int line_index = 0;
                             line_index <= column_span
                             && line_index <= GRID_TRACK_REPEAT_LIMIT;
                             line_index++) {
                            int parent_line =
                                column + line_index - grid_column_origin;
                            for (unsigned slot = 0;
                                 slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                                 slot++) {
                                uint8_t name =
                                    parent_line < 0 ? 0
                                    : stylesheet_grid_track_line_name(
                                        context->sheet, style, false,
                                        (unsigned) parent_line, slot);
                                if (name == 0 && inherited_grid_columns
                                    && column + line_index
                                       <= inherited_grid->column_count) {
                                    name = inherited_grid
                                        ->column_line_names[
                                            column + line_index][slot];
                                }
                                assigned.column_line_names[
                                    line_index][slot] = name;
                            }
                        }
                    }
                    if (computed_style_grid_subgrid_rows(&item->style)) {
                        int row_span = item_placement.row_span;
                        if (row_span > LAYOUT_ASSIGNED_GRID_TRACK_LIMIT) {
                            row_span = LAYOUT_ASSIGNED_GRID_TRACK_LIMIT;
                        }
                        assigned.row_count = (uint8_t) row_span;
                        assigned.row_gap = grid_row_gap;
                        for (int offset = 0; offset < row_span; offset++) {
                            assigned.row_heights[offset] =
                                row_track_heights[grid_row + offset];
                        }
                        for (int line_index = 0;
                             line_index <= row_span
                             && line_index <= GRID_TRACK_REPEAT_LIMIT;
                             line_index++) {
                            int parent_line =
                                grid_row + line_index - grid_row_origin;
                            for (unsigned slot = 0;
                                 slot < STYLE_GRID_LINE_NAMES_PER_LINE;
                                 slot++) {
                                uint8_t name =
                                    parent_line < 0 ? 0
                                    : stylesheet_grid_track_line_name(
                                        context->sheet, style, true,
                                        (unsigned) parent_line, slot);
                                if (name == 0 && inherited_grid_rows
                                    && grid_row + line_index
                                       <= inherited_grid->row_count) {
                                    name = inherited_grid
                                        ->row_line_names[
                                            grid_row + line_index][slot];
                                }
                                assigned.row_line_names[
                                    line_index][slot] = name;
                            }
                        }
                    }
                    context->assigned_grid_tracks = assigned;
                }
                bool saved_assigned_minimum = context->assigned_grid_minimum;
                context->assigned_grid_minimum = false;
                if (definite_cell_height
                    && item_alignment == ALIGN_STRETCH) {
                    context->assigned_grid_node = item->node;
                    context->assigned_grid_height = assigned_cell_height;
                    context->assigned_grid_height_valid = true;
                    /* In a row that can still grow (a single-row fr item
                       with no laid-out item in a later row), let content
                       extend the stretched item so the row grows below. */
                    int stretch_explicit_row = grid_row - grid_row_origin;
                    uint8_t stretch_row_type = GRID_TRACK_AUTO;
                    unsigned stretch_row_value = 0;
                    if (stretch_explicit_row >= 0
                        && stretch_explicit_row < explicit_grid_rows) {
                        grid_defined_row_track(
                            context->sheet, style,
                            (unsigned) stretch_explicit_row,
                            (unsigned) declared_grid_rows,
                            second_implicit_row, &stretch_row_type,
                            &stretch_row_value);
                    }
                    context->assigned_grid_minimum =
                        stretch_row_type == GRID_TRACK_FLEX
                        && stretch_row_value != 0
                        && item_placement.row_span == 1
                        && laid_out_row <= grid_row;
                }
                /* The grid area is a grid item's containing block (CSS
                   Grid 9.1): with definite rows a percentage height inside
                   the item resolves against the area, not the whole grid.
                   MDN's play editor (`height:100%`) sits in a 1fr row of a
                   fixed-height grid and otherwise claimed the full grid. */
                bool child_ok = layout_block(
                    context, item->node, &item->parent_style,
                    child_x, row_top, grid_child_width,
                    definite_cell_height && assigned_cell_height > 0
                        ? assigned_cell_height : child_containing_height,
                    !item->style.has_width,
                    descendant_positioned_box, &child_bottom);
                context->assigned_grid_node = saved_assigned_node;
                context->assigned_grid_height = saved_assigned_height;
                context->assigned_grid_height_valid =
                    saved_assigned_height_valid;
                context->assigned_grid_minimum = saved_assigned_minimum;
                context->assigned_grid_tracks = saved_assigned_tracks;
                if (!child_ok) {
                    grid_laid_items_release(context, &laid);
                    flex_order_plan_destroy(grid_order);
                    return false;
                }
                const LayoutNodeBox *item_box = layout_box_for_node(
                    context->layout, item->node);
                int item_dy = 0;
                if (item_box == NULL) reshare_possible = false;
                if (item_box != NULL) {
                    int dx = 0;
                    int dy = 0;
                    int inline_free = assigned_cell_width - item_box->width;
                    if (inline_free > 0) {
                        if (item->style.margin_left_auto
                            && item->style.margin_right_auto) {
                            dx = inline_free / 2;
                        } else if (item->style.margin_left_auto) {
                            dx = inline_free;
                        } else if (item_justify_self == ALIGN_SELF_CENTER) {
                            dx = inline_free / 2;
                        } else if (item_justify_self == ALIGN_SELF_END) {
                            dx = inline_free;
                        }
                    }
                    if (definite_cell_height) {
                        int block_free =
                            assigned_cell_height - item_box->height;
                        if (block_free > 0) {
                            if (item_alignment == ALIGN_CENTER) {
                                dy = block_free / 2;
                            } else if (item_alignment == ALIGN_END) {
                                dy = block_free;
                            }
                        }
                    }
                    if (dx != 0 || dy != 0) {
                        layout_translate_range(
                            context->layout, item_command_start,
                            item_link_start, item_control_start,
                            item_box_start, dx, dy, "grid-align",
                            item->node);
                        child_bottom = layout_add_coordinate(
                            child_bottom, dy);
                    }
                    item_dy = dy;
                    if (!definite_cell_height
                        && item_placement.row_span > 1
                        && (item_alignment == ALIGN_END
                            || item_alignment == ALIGN_CENTER)
                        && span_align_count < GRID_SPAN_ALIGN_LIMIT) {
                        span_align[span_align_count++] = (GridSpanAlignItem) {
                            .node = item->node,
                            .row = grid_row,
                            .span = item_placement.row_span,
                            .offset = item_box->y - row_top,
                            .height = item_box->height,
                            .center = item_alignment == ALIGN_CENTER
                        };
                    }
                }
                if (reshare_possible
                    && (reshare_count == GRID_RESHARE_ITEM_LIMIT
                        || item_placement.row_span != 1
                        || grid_row >= GRID_PLACEMENT_ROW_LIMIT
                        || (reshare_count != 0
                            && grid_row
                                < reshare_items[reshare_count - 1].row))) {
                    reshare_possible = false;
                }
                if (reshare_possible) {
                    reshare_items[reshare_count++] = (GridReshareItem) {
                        .row = grid_row,
                        .dy = item_dy,
                        .extent = child_bottom - item_dy - row_top,
                        .alignment = item_alignment,
                        .command_start = item_command_start,
                        .link_start = item_link_start,
                        .control_start = item_control_start,
                        .box_start = item_box_start
                    };
                }
            }
            int explicit_row = grid_row - grid_row_origin;
            uint8_t row_type = GRID_TRACK_AUTO;
            unsigned row_value = 0;
            if (explicit_row >= 0 && explicit_row < explicit_grid_rows) {
                grid_defined_row_track(
                    context->sheet, style, (unsigned) explicit_row,
                    (unsigned) declared_grid_rows, second_implicit_row,
                    &row_type, &row_value);
            }
            bool collapsed_flex_track = row_type == GRID_TRACK_FLEX
                                        && row_value == 0;
            if (row_track_heights[grid_row] <= 0
                && !collapsed_flex_track
                && child_bottom > row_extents[grid_row]) {
                int grown = child_bottom - row_extents[grid_row];
                row_extents[grid_row] = child_bottom;
                /* Explicit rows were positioned before any content was
                   measured. A content-sized row that grows must push the
                   later rows down, as a growing fr row does below, or the
                   next row's items land on top of it. */
                if (item_placement.row_span == 1
                    && (laid_out_row <= grid_row
                        || grid_shift_later_rows(
                               context, &laid, grid_row, grown))) {
                    for (int later = grid_row + 1; later < initialized_rows;
                         later++) {
                        row_starts[later] =
                            layout_add_coordinate(row_starts[later], grown);
                        row_extents[later] =
                            layout_add_coordinate(row_extents[later], grown);
                    }
                }
            } else if (row_type == GRID_TRACK_FLEX && !collapsed_flex_track
                       && item_placement.row_span == 1
                       && child_bottom > row_extents[grid_row]
                       && (laid_out_row <= grid_row
                           || grid_shift_later_rows(
                                  context, &laid, grid_row,
                                  child_bottom - row_extents[grid_row]))) {
                /* An fr row is minmax(auto, <flex>): it is never shorter
                   than its content (CSS Grid 7.2.4). Its share of the free
                   space was fixed before content was measured, so grow it
                   now and move the later rows, none of which holds a
                   laid-out item yet. ChatGPT's auto/1fr/auto page grid
                   otherwise drew its disclaimer (row 3) over the composer
                   whenever the middle row's content outgrew its share. */
                int grown = child_bottom - row_extents[grid_row];
                row_extents[grid_row] = child_bottom;
                for (int later = grid_row + 1; later < initialized_rows;
                     later++) {
                    row_starts[later] =
                        layout_add_coordinate(row_starts[later], grown);
                    row_extents[later] =
                        layout_add_coordinate(row_extents[later], grown);
                }
            }
            if (grid_row > laid_out_row) laid_out_row = grid_row;
        }
        grid_laid_items_release(context, &laid);
        if (reshare_possible)
            grid_resolve_final_rows(
                context, style, row_starts, row_extents, initialized_rows,
                declared_content_height, grid_row_origin,
                explicit_grid_rows, declared_grid_rows,
                second_implicit_row, reshare_items, reshare_count);
        for (size_t i = 0; i < span_align_count; i++) {
            const GridSpanAlignItem *span = &span_align[i];
            int last = span->row + span->span - 1;
            if (last >= initialized_rows) last = initialized_rows - 1;
            if (last <= span->row) continue;
            int area_height = row_extents[last] - row_starts[span->row];
            int free_space = area_height - span->height;
            if (free_space <= 0) continue;
            int target = span->center ? free_space / 2 : free_space;
            /* `offset` already holds the in-flow placement plus any
               relative offset; only the alignment shift moves. */
            if (target != 0) {
                translate_node_subtree(context->layout, span->node, 0,
                                       target);
            }
        }
        int grid_bottom = line->y;
        for (int row = 0; row < initialized_rows; row++) {
            if (row_extents[row] > grid_bottom) {
                grid_bottom = row_extents[row];
            }
        }
        line->y = grid_bottom;
        flex_order_plan_destroy(grid_order);
        line_cursor_set(line, line->start_x);
        line->line_height = 0;
        line->line_height_fixed = 0;
        line->y_fixed_valid = false;
    }
    return true;
}
