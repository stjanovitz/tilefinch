/* Grid item placement: the occupancy bitmap, line resolution, and the
   sequential auto-placement pass used by layout_block_grid.c. Split out of
   layout_flex.c. */

#include "layout_internal.h"

#include <string.h>

void grid_placement_init(GridPlacementState *state, int columns, int rows,
                         const ComputedStyle *container)
{
    if (state == NULL) return;
    *state = (GridPlacementState) {0};
    if (columns < 1) columns = 1;
    if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
    if (rows < 1) rows = 1;
    if (rows > GRID_PLACEMENT_ROW_LIMIT) {
        rows = GRID_PLACEMENT_ROW_LIMIT;
    }
    state->columns = (uint8_t) columns;
    state->rows = (uint8_t) rows;
    state->explicit_columns = state->columns;
    state->explicit_rows = state->rows;
    state->flow_column = computed_style_grid_auto_flow_column(container);
    state->dense = computed_style_grid_auto_flow_dense(container);
}

static bool grid_placement_fits(const GridPlacementState *state,
                                int row, int column,
                                int row_span, int column_span)
{
    if (state == NULL || row < 0 || column < 0
        || row_span < 1 || column_span < 1
        || row + row_span > GRID_PLACEMENT_ROW_LIMIT
        || column + column_span > state->columns) return false;
    uint32_t mask = ((UINT32_C(1) << column_span) - 1u) << column;
    for (int offset = 0; offset < row_span; offset++) {
        if ((state->occupied[row + offset] & mask) != 0) return false;
    }
    return true;
}

static void grid_placement_mark(GridPlacementState *state,
                                int row, int column,
                                int row_span, int column_span)
{
    uint32_t mask = ((UINT32_C(1) << column_span) - 1u) << column;
    for (int offset = 0; offset < row_span; offset++) {
        state->occupied[row + offset] |= mask;
    }
}

static int grid_line_position(unsigned encoded, int origin,
                              int explicit_tracks)
{
    int line = computed_style_decode_grid_line(encoded);
    return line < 0 ? origin + explicit_tracks + 1 + line
                    : origin + line - 1;
}

typedef struct {
    int row;
    int column;
    int row_span;
    int column_span;
    bool explicit_row;
    bool explicit_column;
} GridResolvedArea;

/* Resolve an item's definite lines and spans against the state's explicit
   grid and origin, clamped into the bounded placement envelope. Axes
   without a definite line keep position 0 and explicit_* false. */
static void grid_resolve_area(const GridPlacementState *state,
                              const ComputedStyle *style,
                              GridResolvedArea *area)
{
    int column_start = computed_style_grid_column_start(style);
    int column_end = computed_style_grid_column_end(style);
    int column_span = computed_style_grid_column_span(style);
    int row_start = computed_style_grid_row_start(style);
    int row_end = computed_style_grid_row_end(style);
    int row_span = computed_style_grid_row_span(style);
    bool explicit_column = column_start != 0;
    bool explicit_row = row_start != 0;
    bool explicit_column_end = column_end != 0;
    bool explicit_row_end = row_end != 0;
    if (column_span < 1) column_span = 1;
    if (row_span < 1) row_span = 1;
    int column = explicit_column
        ? grid_line_position(
              (unsigned) column_start, state->column_origin,
              state->explicit_columns)
        : (explicit_column_end
           ? grid_line_position(
                 (unsigned) column_end, state->column_origin,
                 state->explicit_columns) - column_span
           : 0);
    int row = explicit_row
        ? grid_line_position(
              (unsigned) row_start, state->row_origin,
              state->explicit_rows)
        : (explicit_row_end
           ? grid_line_position(
                 (unsigned) row_end, state->row_origin,
                 state->explicit_rows) - row_span
           : 0);
    if (!explicit_column && explicit_column_end) {
        explicit_column = true;
    } else if (explicit_column && explicit_column_end) {
        int end = grid_line_position(
            (unsigned) column_end, state->column_origin,
            state->explicit_columns);
        if (end > column) column_span = end - column;
    }
    if (!explicit_row && explicit_row_end) {
        explicit_row = true;
    } else if (explicit_row && explicit_row_end) {
        int end = grid_line_position(
            (unsigned) row_end, state->row_origin,
            state->explicit_rows);
        if (end > row) row_span = end - row;
    }
    if (column_span > state->columns) column_span = state->columns;
    if (row_span > GRID_PLACEMENT_ROW_LIMIT) {
        row_span = GRID_PLACEMENT_ROW_LIMIT;
    }
    if (column < 0) column = 0;
    if (row < 0) row = 0;
    if (column + column_span > state->columns) {
        column = state->columns - column_span;
    }
    *area = (GridResolvedArea) {
        .row = row, .column = column,
        .row_span = row_span, .column_span = column_span,
        .explicit_row = explicit_row, .explicit_column = explicit_column
    };
}

bool grid_item_locked_to_major_axis(const ComputedStyle *style,
                                    const ComputedStyle *container)
{
    if (style == NULL) return false;
    return computed_style_grid_auto_flow_column(container)
        ? computed_style_grid_column_start(style) != 0
          || computed_style_grid_column_end(style) != 0
        : computed_style_grid_row_start(style) != 0
          || computed_style_grid_row_end(style) != 0;
}

/* Earliest minor-axis start that fits a locked item, at or after `from`
   (sparse packing keeps step-2 items in a track in document order). */
static int grid_locked_minor_position(const GridPlacementState *state,
                                      const GridResolvedArea *area,
                                      int from)
{
    if (state->flow_column) {
        for (int candidate = from;
             candidate + area->row_span <= GRID_PLACEMENT_ROW_LIMIT;
             candidate++) {
            if (grid_placement_fits(state, candidate, area->column,
                                    area->row_span, area->column_span)) {
                return candidate;
            }
        }
    } else {
        for (int candidate = from;
             candidate + area->column_span <= state->columns;
             candidate++) {
            if (grid_placement_fits(state, area->row, candidate,
                                    area->row_span, area->column_span)) {
                return candidate;
            }
        }
    }
    return -1;
}

bool grid_placement_reserve(GridPlacementState *state, LayoutContext *context,
                            lxb_dom_node_t *container,
                            const ComputedStyle *container_style,
                            const FlexOrderPlan *order,
                            GridItemPlacementResolver resolve,
                            FlexItemIterator *iterator, FlatItem *item)
{
    if (state == NULL || context == NULL || iterator == NULL
        || item == NULL || state->columns == 0) return true;
    state->locked_count = 0;
    state->locked_next = 0;
    state->reserved = true;
    /* Sparse step 2 places each locked item after the earlier locked items
       of its own track; one cursor per major-axis track. */
    uint8_t track_cursor[GRID_PLACEMENT_ROW_LIMIT];
    memset(track_cursor, 0, sizeof(track_cursor));
    for (int phase = 0; phase < 2; phase++) {
        flex_iterator_init(iterator, context, container, container_style,
                           order);
        while (flex_iterator_next(iterator, item)) {
            if (context->cancelled) return false;
            if (resolve != NULL) {
                resolve(context, container, container_style, &item->style);
            }
            if (item->style.out_of_flow || item->style.fixed_position) {
                continue;
            }
            GridResolvedArea area;
            grid_resolve_area(state, &item->style, &area);
            bool both = area.explicit_row && area.explicit_column;
            if (phase == 0) {
                /* Step 1: items positioned in both axes may overlap; they
                   only claim their cells for later auto-placement. */
                if (both
                    && area.row + area.row_span <= GRID_PLACEMENT_ROW_LIMIT) {
                    grid_placement_mark(state, area.row, area.column,
                                        area.row_span, area.column_span);
                }
                continue;
            }
            bool locked = state->flow_column
                ? area.explicit_column : area.explicit_row;
            if (both || !locked) continue;
            if (state->locked_count == GRID_LOCKED_ITEM_LIMIT) break;
            /* Step 2: locked items take the earliest free minor-axis
               position in their track before any auto-placed item. */
            int track = state->flow_column ? area.column : area.row;
            int from = state->dense || track < 0
                       || track >= GRID_PLACEMENT_ROW_LIMIT
                ? 0 : track_cursor[track];
            int position = grid_locked_minor_position(state, &area, from);
            if (position < 0) {
                state->locked_positions[state->locked_count++] =
                    GRID_LOCKED_NO_FIT;
                continue;
            }
            state->locked_positions[state->locked_count++] =
                (uint8_t) position;
            if (state->flow_column) {
                grid_placement_mark(state, position, area.column,
                                    area.row_span, area.column_span);
            } else {
                grid_placement_mark(state, area.row, position,
                                    area.row_span, area.column_span);
            }
            if (track >= 0 && track < GRID_PLACEMENT_ROW_LIMIT) {
                int next = position + (state->flow_column
                                       ? area.row_span : area.column_span);
                track_cursor[track] = (uint8_t) (next > UINT8_MAX
                                                 ? UINT8_MAX : next);
            }
        }
    }
    return !context->cancelled;
}

bool grid_place_item(GridPlacementState *state, const ComputedStyle *style,
                     GridItemPlacement *placement)
{
    if (state == NULL || style == NULL || placement == NULL
        || state->columns == 0) return false;
    GridResolvedArea area;
    grid_resolve_area(state, style, &area);
    int row = area.row;
    int column = area.column;
    int row_span = area.row_span;
    int column_span = area.column_span;
    bool explicit_row = area.explicit_row;
    bool explicit_column = area.explicit_column;
    bool locked = !(explicit_row && explicit_column)
        && (state->flow_column ? explicit_column : explicit_row);

    bool found = false;
    bool reserved_locked = false;
    if (locked && state->reserved
        && state->locked_next < state->locked_count) {
        /* grid_placement_reserve() already chose this item's area and
           claimed its cells; keep it out of the auto-placement cursor. */
        uint8_t position = state->locked_positions[state->locked_next++];
        reserved_locked = true;
        if (position != GRID_LOCKED_NO_FIT) {
            if (state->flow_column) row = position;
            else column = position;
            found = true;
        }
    } else if (explicit_row && explicit_column) {
        /* Explicitly positioned grid items may overlap. Occupancy only
           constrains auto-placement; treating an occupied explicit area as
           a placement failure incorrectly moves later authored items. */
        found = row + row_span <= GRID_PLACEMENT_ROW_LIMIT
                && column + column_span <= state->columns;
    } else if (explicit_row) {
        for (int candidate = 0;
             candidate + column_span <= state->columns; candidate++) {
            if (!grid_placement_fits(
                    state, row, candidate, row_span, column_span)) continue;
            column = candidate;
            found = true;
            break;
        }
    } else if (explicit_column) {
        for (int candidate = 0;
             candidate + row_span <= GRID_PLACEMENT_ROW_LIMIT; candidate++) {
            if (!grid_placement_fits(
                    state, candidate, column, row_span, column_span)) continue;
            row = candidate;
            found = true;
            break;
        }
    } else if (state->flow_column) {
        int candidate_row = state->dense ? 0 : state->cursor_row;
        int candidate_column = state->dense ? 0 : state->cursor_column;
        while (candidate_column + column_span <= state->columns) {
            if (candidate_row + row_span <= state->rows
                && grid_placement_fits(
                    state, candidate_row, candidate_column,
                    row_span, column_span)) {
                row = candidate_row;
                column = candidate_column;
                found = true;
                break;
            }
            candidate_row++;
            if (candidate_row >= state->rows) {
                candidate_row = 0;
                candidate_column++;
            }
        }
    } else {
        int candidate_row = state->dense ? 0 : state->cursor_row;
        int candidate_column = state->dense ? 0 : state->cursor_column;
        while (candidate_row + row_span <= GRID_PLACEMENT_ROW_LIMIT) {
            if (candidate_column + column_span <= state->columns
                && grid_placement_fits(
                    state, candidate_row, candidate_column,
                    row_span, column_span)) {
                row = candidate_row;
                column = candidate_column;
                found = true;
                break;
            }
            candidate_column++;
            if (candidate_column >= state->columns) {
                candidate_column = 0;
                candidate_row++;
            }
        }
    }
    if (!found) {
        /* Explicit collisions and documents beyond the bounded row envelope
           retain content deterministically in the final available cell. */
        row = GRID_PLACEMENT_ROW_LIMIT - row_span;
        column = state->columns - column_span;
    }
    grid_placement_mark(state, row, column, row_span, column_span);
    if (!reserved_locked && (!explicit_row || !explicit_column)) {
        int next_column = column;
        int next_row = row;
        if (state->flow_column) {
            next_row += row_span;
            if (next_row >= state->rows) {
                next_row = 0;
                next_column++;
            }
        } else {
            next_column += column_span;
            if (next_column >= state->columns) {
                next_column = 0;
                next_row++;
            }
        }
        if (next_column >= state->columns) {
            next_column = state->columns - 1;
        }
        if (next_row >= GRID_PLACEMENT_ROW_LIMIT) {
            next_row = GRID_PLACEMENT_ROW_LIMIT - 1;
        }
        state->cursor_row = (uint8_t) next_row;
        state->cursor_column = (uint8_t) next_column;
    }
    *placement = (GridItemPlacement) {
        .column = (uint8_t) column,
        .column_span = (uint8_t) column_span,
        .row = (uint8_t) row,
        .row_span = (uint8_t) row_span
    };
    return found;
}
