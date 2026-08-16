/* Bounded CSS float exclusions shared by block and inline formatting.
   Keeping the active set and all band transitions here prevents the two
   formatting paths from acquiring subtly different collision semantics. */

#include "layout_internal.h"

void update_float_band(LineState *line)
{
    if (!line->floats_enabled) return;
    line->layout->performance.float_band_queries++;
    int left = line->base_left;
    int right = line->base_right;
    for (size_t i = 0; i < line->float_count; i++) {
        line->layout->performance.float_exclusion_probes++;
        const FloatExclusion *exclusion = &line->floats[i];
        if (exclusion->top > line->y
            || exclusion->bottom <= line->y) continue;
        if (exclusion->side == FLOAT_LEFT
            && exclusion->right > left) {
            left = exclusion->right;
        } else if (exclusion->side == FLOAT_RIGHT
                   && exclusion->x < right) {
            right = exclusion->x;
        }
    }
    line->start_x = left;
    line->right = right > left ? right : left;
    line_cursor_set(line, line->start_x);
}

void update_float_bounds(LineState *line)
{
    if (!line->floats_enabled) return;
    for (;;) {
        update_float_band(line);
        if (line->right - line->start_x >= 8) return;
        int next_bottom = INT_MAX;
        for (size_t i = 0; i < line->float_count; i++) {
            line->layout->performance.float_exclusion_probes++;
            const FloatExclusion *exclusion = &line->floats[i];
            if (exclusion->top <= line->y
                && exclusion->bottom > line->y
                && exclusion->bottom < next_bottom) {
                next_bottom = exclusion->bottom;
            }
        }
        if (next_bottom == INT_MAX) {
            line->right = line->start_x + 8;
            return;
        }
        line->y = next_bottom;
    }
}

void update_float_bounds_for_span(LineState *line, int height)
{
    if (!line->floats_enabled || height <= 0) return;
    int left = line->base_left;
    int right = line->base_right;
    int span_bottom = layout_add_coordinate(line->y, height);
    line->layout->performance.float_band_queries++;
    for (size_t i = 0; i < line->float_count; i++) {
        line->layout->performance.float_exclusion_probes++;
        const FloatExclusion *exclusion = &line->floats[i];
        if (exclusion->top >= span_bottom
            || exclusion->bottom <= line->y) continue;
        if (exclusion->side == FLOAT_LEFT && exclusion->right > left) {
            left = exclusion->right;
        } else if (exclusion->side == FLOAT_RIGHT
                   && exclusion->x < right) {
            right = exclusion->x;
        }
    }
    line->start_x = left;
    line->right = right > left ? right : left;
    line_cursor_set(line, left);
}

void clear_line_floats(LineState *line, ClearMode clear)
{
    if (!line->floats_enabled || clear == CLEAR_NONE) return;
    int bottom = line->y;
    for (size_t i = 0; i < line->float_count; i++) {
        line->layout->performance.float_exclusion_probes++;
        const FloatExclusion *exclusion = &line->floats[i];
        bool matches = clear == CLEAR_BOTH
            || (clear == CLEAR_LEFT && exclusion->side == FLOAT_LEFT)
            || (clear == CLEAR_RIGHT && exclusion->side == FLOAT_RIGHT);
        if (matches && exclusion->bottom > bottom) {
            bottom = exclusion->bottom;
        }
    }
    line->y = bottom;
    update_float_bounds(line);
}

void make_float_slot(LineState *line)
{
    if (line->float_count < ACTIVE_FLOAT_LIMIT) return;
    int bottom = line->y;
    for (size_t i = 0; i < line->float_count; i++) {
        line->layout->performance.float_exclusion_probes++;
        if (line->floats[i].bottom > bottom) bottom = line->floats[i].bottom;
    }
    line->y = bottom;
    line->float_count = 0;
    update_float_bounds(line);
}

bool layout_place_float(LayoutContext *context, lxb_dom_node_t *node,
                        const ComputedStyle *parent,
                        const ComputedStyle *style, LineState *line,
                        int containing_height)
{
    if (context == NULL || node == NULL || parent == NULL || style == NULL
        || line == NULL || style->float_mode == FLOAT_NONE) return false;
    if (line->line_height != 0 || line->line_height_fixed != 0
        || line->x != line->start_x) {
        layout_flush_line(line);
    }
    int normal_flow_y = line->y;
    clear_line_floats(line, style->clear_mode);
    make_float_slot(line);
    update_float_band(line);
    int containing_width = line->base_right - line->base_left;
    int desired_width = 0;
    bool definite_float_width = style->has_width
                                && !style->width_max_content;
    if (definite_float_width) {
        desired_width = resolve_declared_length(
            context->sheet, style->width, style->width_percent,
            containing_width);
        if (!style->box_sizing_border_box) {
            desired_width += style->padding.left + style->padding.right
                + style->border.left + style->border.right;
        }
        desired_width += style->margin.left + style->margin.right;
    }
    int minimum_width = 0;
    if (style->width_max_content) {
        if (computed_style_width_min_content(style)) {
            desired_width = intrinsic_min_text_width_ignoring_own_width(
                context, node, parent, containing_width);
            minimum_width = desired_width;
        } else {
            int limit = computed_style_width_max_content(style)
                ? LAYOUT_COORDINATE_LIMIT : containing_width;
            desired_width = intrinsic_text_width(
                context, node, parent, limit);
            minimum_width = intrinsic_min_text_width_ignoring_own_width(
                context, node, parent, containing_width);
        }
    } else if (!definite_float_width) {
        intrinsic_text_widths(
            context, node, parent, containing_width,
            &desired_width, &minimum_width);
    } else {
        minimum_width = intrinsic_min_text_width(
            context, node, parent, containing_width);
    }
    if (desired_width < minimum_width) desired_width = minimum_width;
    if (!definite_float_width && desired_width < 8) desired_width = 8;
    if (!definite_float_width && desired_width > containing_width) {
        desired_width = containing_width;
    }
    int available = line->right - line->start_x;
    while (desired_width > available) {
        int next_bottom = INT_MAX;
        for (size_t i = 0; i < line->float_count; i++) {
            line->layout->performance.float_exclusion_probes++;
            const FloatExclusion *exclusion = &line->floats[i];
            if (exclusion->top <= line->y
                && exclusion->bottom > line->y
                && exclusion->bottom < next_bottom) {
                next_bottom = exclusion->bottom;
            }
        }
        if (next_bottom == INT_MAX) break;
        line->y = next_bottom;
        update_float_band(line);
        available = line->right - line->start_x;
    }
    if (!definite_float_width && desired_width > available) {
        desired_width = available;
    }
    if (!definite_float_width && desired_width < 8) desired_width = 8;
    int float_x = style->float_mode == FLOAT_RIGHT
        ? line->right - desired_width : line->start_x;
    int float_top = line->y;
    int float_bottom = float_top;
    if (!layout_block(context, node, parent, float_x, float_top,
                      desired_width, containing_height, true,
                      &line->positioned_box, &float_bottom)) {
        return false;
    }
    if (float_bottom <= float_top) float_bottom = float_top + 1;
    line->floats[line->float_count++] = (FloatExclusion) {
        .x = float_x,
        .right = float_x + desired_width,
        .top = float_top,
        .bottom = float_bottom,
        .side = style->float_mode
    };
    line->y = normal_flow_y;
    update_float_band(line);
    return true;
}
