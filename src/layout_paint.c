/* Display-list infrastructure: command emission, paint order,
   spatial bands, sticky/fixed ranges, and visual-range transforms.
   Split out of layout.c. */

#include "layout_internal.h"

_Static_assert((int) STYLE_MIX_BLEND_NORMAL == (int) LAYOUT_MIX_BLEND_NORMAL,
               "style/layout blend encodings must match");
_Static_assert((int) STYLE_MIX_BLEND_MULTIPLY
                   == (int) LAYOUT_MIX_BLEND_MULTIPLY,
               "style/layout blend encodings must match");
_Static_assert((int) STYLE_MIX_BLEND_SCREEN == (int) LAYOUT_MIX_BLEND_SCREEN,
               "style/layout blend encodings must match");
_Static_assert((int) STYLE_MIX_BLEND_DARKEN == (int) LAYOUT_MIX_BLEND_DARKEN,
               "style/layout blend encodings must match");

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int scale_around_center(int value, int origin_twice,
                               uint8_t scale_q6)
{
    int64_t delta_twice = (int64_t) value * 2 - origin_twice;
    int64_t scaled_twice = origin_twice
        + delta_twice * scale_q6 / 64;
    if (scaled_twice > (int64_t) INT_MAX * 2) return INT_MAX;
    if (scaled_twice < (int64_t) INT_MIN * 2) return INT_MIN;
    return (int) (scaled_twice < 0
                  ? (scaled_twice - 1) / 2 : (scaled_twice + 1) / 2);
}

static int scale_dimension(int value, uint8_t scale_q6)
{
    if (value <= 0 || scale_q6 == 0) return 0;
    int64_t scaled = ((int64_t) value * scale_q6 + 32) / 64;
    if (scaled > INT_MAX) return INT_MAX;
    return scaled < 1 ? 1 : (int) scaled;
}

static int scale_fixed_around_center(int value_fixed, int origin_twice,
                                     uint8_t scale_q6)
{
    int64_t origin_fixed = (int64_t) origin_twice * 32;
    int64_t scaled = origin_fixed
        + ((int64_t) value_fixed - origin_fixed) * scale_q6 / 64;
    return scaled > INT_MAX ? INT_MAX
           : (scaled < INT_MIN ? INT_MIN : (int) scaled);
}

static int bounded_origin_twice(int box_start, int box_size,
                                uint16_t encoded)
{
    int percent = style_object_position_percent(encoded);
    int offset = style_object_position_offset(encoded);
    int64_t coordinate = (int64_t) box_start
        + (int64_t) box_size * percent / 100 + offset;
    coordinate *= 2;
    if (coordinate > INT_MAX) return INT_MAX;
    if (coordinate < INT_MIN) return INT_MIN;
    return (int) coordinate;
}

void layout_transform_origin_twice(
    const Stylesheet *sheet, const ComputedStyle *style,
    int box_x, int box_y, int box_width, int box_height,
    int *origin_x_twice, int *origin_y_twice)
{
    uint16_t x = style_object_position_encode(50, 0);
    uint16_t y = style_object_position_encode(50, 0);
    const StylePaintStack *paint = style == NULL ? NULL
        : stylesheet_paint_stack(
              sheet, computed_style_paint_stack_id(style));
    if (paint != NULL
        && (paint->components
            & STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN) != 0) {
        x = paint->transform_origin_x;
        y = paint->transform_origin_y;
    }
    if (origin_x_twice != NULL) {
        *origin_x_twice = bounded_origin_twice(box_x, box_width, x);
    }
    if (origin_y_twice != NULL) {
        *origin_y_twice = bounded_origin_twice(box_y, box_height, y);
    }
}

/* CSS transforms alter painted and hit-test geometry but do not change normal
   flow. Scale the already-built retained range around the resolved border-box
   transform origin, leaving its flow bottom untouched. */
void layout_scale_range(LayoutDocument *layout, size_t command_start,
                        size_t link_start, size_t control_start,
                        size_t node_box_start, int origin_x_twice,
                        int origin_y_twice, uint8_t scale_q6,
                        lxb_dom_node_t *source)
{
    if (scale_q6 == 64) return;
    for (size_t i = command_start; i < layout->count; i++) {
        DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT) {
            int x_fixed = scale_fixed_around_center(
                draw_command_text_x_fixed(command), origin_x_twice,
                scale_q6);
            draw_command_set_text_x_fixed(command, x_fixed);
        } else {
            command->x = scale_around_center(command->x, origin_x_twice,
                                             scale_q6);
        }
        command->y = scale_around_center(command->y, origin_y_twice,
                                         scale_q6);
        command->width = scale_dimension(command->width, scale_q6);
        command->height = scale_dimension(command->height, scale_q6);
        if (command->type != DRAW_TEXT && command->type != DRAW_IMAGE) {
            command->radius = layout_scale_radius_code(
                command->radius, scale_q6, 64);
        }
        if (command->type == DRAW_TEXT) {
            int64_t size_fixed =
                (int64_t) draw_command_text_font_size_fixed(command)
                * scale_q6 + 32;
            size_fixed /= 64;
            if (size_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64) {
                size_fixed = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64;
            }
            draw_command_set_text_font_size_fixed(command,
                                                   (int) size_fixed);
            if (draw_command_is_text_shadow(command)) {
                draw_command_set_text_shadow_blur(
                    command, scale_dimension(
                        draw_command_text_shadow_blur(command), scale_q6));
            }
            int letter_spacing = scale_around_center(
                command->letter_spacing, 0, scale_q6);
            if (letter_spacing < INT8_MIN) letter_spacing = INT8_MIN;
            if (letter_spacing > INT8_MAX) letter_spacing = INT8_MAX;
            command->letter_spacing = (int8_t) letter_spacing;
            int underline_offset = 0;
            if (draw_command_text_underline_offset(
                    command, &underline_offset)) {
                draw_command_set_text_underline_offset(
                    command, scale_around_center(
                        underline_offset, 0, scale_q6));
            }
        }
        if (command->type == DRAW_TEXT
            || command->type == DRAW_STROKE_RECT
            || command->type == DRAW_SHADOW_RECT
            || command->type == DRAW_IMAGE) {
            command->scale = command->type == DRAW_IMAGE
                ? layout_scale_radius_code(command->scale, scale_q6, 64)
                : scale_dimension(command->scale, scale_q6);
            if (command->type == DRAW_TEXT
                && command->scale > TILEFINCH_BITMAP_FONT_SCALE_LIMIT) {
                command->scale = TILEFINCH_BITMAP_FONT_SCALE_LIMIT;
            }
        }
    }
    for (size_t i = link_start; i < layout->link_count; i++) {
        LinkRegion *link = &layout->links[i];
        link->x = scale_around_center(link->x, origin_x_twice, scale_q6);
        link->y = scale_around_center(link->y, origin_y_twice, scale_q6);
        link->width = scale_dimension(link->width, scale_q6);
        link->height = scale_dimension(link->height, scale_q6);
    }
    for (size_t i = control_start; i < layout->control_count; i++) {
        ControlRegion *control = &layout->controls[i];
        control->x = scale_around_center(control->x, origin_x_twice,
                                         scale_q6);
        control->y = scale_around_center(control->y, origin_y_twice,
                                         scale_q6);
        control->width = scale_dimension(control->width, scale_q6);
        control->height = scale_dimension(control->height, scale_q6);
    }
    for (size_t i = node_box_start; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        if (source != NULL && !layout_node_within(box->node, source)) continue;
        if (box->command_start < command_start
            || box->command_end > layout->count) continue;
        box->x = scale_around_center(box->x, origin_x_twice, scale_q6);
        box->y = scale_around_center(box->y, origin_y_twice, scale_q6);
        box->width = scale_dimension(box->width, scale_q6);
        box->height = scale_dimension(box->height, scale_q6);
        box->client_width = scale_dimension(box->client_width, scale_q6);
        box->client_height = scale_dimension(box->client_height, scale_q6);
        box->content_width = scale_dimension(box->content_width, scale_q6);
        box->content_height = scale_dimension(box->content_height, scale_q6);
        box->scroll_x = scale_dimension(box->scroll_x, scale_q6);
        box->scroll_y = scale_dimension(box->scroll_y, scale_q6);
        int clip_radius_code = layout_node_box_clip_radius_code(layout, box);
        clip_radius_code = layout_scale_radius_code(
            clip_radius_code, scale_q6, 64);
        int clip_radius = style_border_radius_maximum(clip_radius_code);
        int clip_margin = scale_dimension(
            (int) layout_node_box_clip_margin(box), scale_q6);
        layout_node_box_set_clip_geometry(
            box, (unsigned) clip_radius, (unsigned) clip_margin,
            layout_node_box_clip_box(box));
        if (layout->node_clip_radius_codes != NULL) {
            layout->node_clip_radius_codes[i] =
                style_border_radius_is_packed(clip_radius_code)
                    ? clip_radius_code : 0;
        }
    }
}

static int rotate_floor_half(int64_t value)
{
    int64_t result = value >= 0 ? value / 2 : -((-value + 1) / 2);
    return result > INT_MAX ? INT_MAX
        : (result < INT_MIN ? INT_MIN : (int) result);
}

static int rotate_ceil_half(int64_t value)
{
    int64_t result = value >= 0 ? (value + 1) / 2 : -((-value) / 2);
    return result > INT_MAX ? INT_MAX
        : (result < INT_MIN ? INT_MIN : (int) result);
}

static void rotate_point_twice(int64_t x, int64_t y,
                               int origin_x_twice, int origin_y_twice,
                               unsigned quadrants,
                               int64_t *rx, int64_t *ry)
{
    int64_t dx = x * 2 - origin_x_twice;
    int64_t dy = y * 2 - origin_y_twice;
    if (quadrants == 1u) {
        *rx = (int64_t) origin_x_twice - dy;
        *ry = (int64_t) origin_y_twice + dx;
    } else if (quadrants == 2u) {
        *rx = (int64_t) origin_x_twice - dx;
        *ry = (int64_t) origin_y_twice - dy;
    } else {
        *rx = (int64_t) origin_x_twice + dy;
        *ry = (int64_t) origin_y_twice - dx;
    }
}

static void rotate_rect_quadrants(int *x, int *y, int *width, int *height,
                                  int origin_x_twice, int origin_y_twice,
                                  unsigned quadrants)
{
    int64_t xs[4], ys[4];
    int64_t right = (int64_t) *x + *width;
    int64_t bottom = (int64_t) *y + *height;
    rotate_point_twice(*x, *y, origin_x_twice, origin_y_twice,
                       quadrants, &xs[0], &ys[0]);
    rotate_point_twice(right, *y, origin_x_twice, origin_y_twice,
                       quadrants, &xs[1], &ys[1]);
    rotate_point_twice(right, bottom, origin_x_twice, origin_y_twice,
                       quadrants, &xs[2], &ys[2]);
    rotate_point_twice(*x, bottom, origin_x_twice, origin_y_twice,
                       quadrants, &xs[3], &ys[3]);
    int64_t min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (unsigned i = 1; i < 4; i++) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    int left = rotate_floor_half(min_x);
    int top = rotate_floor_half(min_y);
    int right_px = rotate_ceil_half(max_x);
    int bottom_px = rotate_ceil_half(max_y);
    *x = left;
    *y = top;
    *width = right_px < left ? 0 : right_px - left;
    *height = bottom_px < top ? 0 : bottom_px - top;
}

/* The PSP display list is axis-aligned. Individual rotate therefore retains
   exact quarter turns: their painted bounds, hit regions, and descendants
   remain representable without an offscreen surface or per-command growth.
   Other angles are rejected by the property-value capability check. */
void layout_rotate_range_quadrants(LayoutDocument *layout,
                                   size_t command_start, size_t link_start,
                                   size_t control_start,
                                   size_t node_box_start,
                                   int origin_x_twice, int origin_y_twice,
                                   uint8_t quadrants,
                                   lxb_dom_node_t *source)
{
    if (layout == NULL || (quadrants &= 3u) == 0) return;
    for (size_t i = command_start; i < layout->count; i++) {
        DrawCommand *command = &layout->commands[i];
        unsigned combined = (draw_command_rotation_quadrants(command)
                             + quadrants) & 3u;
        rotate_rect_quadrants(
            &command->x, &command->y, &command->width, &command->height,
            origin_x_twice, origin_y_twice, quadrants);
        draw_command_set_rotation_quadrants(command, combined);
    }
    for (size_t i = link_start; i < layout->link_count; i++) {
        LinkRegion *link = &layout->links[i];
        rotate_rect_quadrants(
            &link->x, &link->y, &link->width, &link->height,
            origin_x_twice, origin_y_twice, quadrants);
    }
    for (size_t i = control_start; i < layout->control_count; i++) {
        ControlRegion *control = &layout->controls[i];
        rotate_rect_quadrants(
            &control->x, &control->y, &control->width, &control->height,
            origin_x_twice, origin_y_twice, quadrants);
    }
    for (size_t i = node_box_start; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        if (source != NULL && !layout_node_within(box->node, source)) continue;
        if (box->command_start < command_start
            || box->command_end > layout->count) continue;
        rotate_rect_quadrants(
            &box->x, &box->y, &box->width, &box->height,
            origin_x_twice, origin_y_twice, quadrants);
        if ((quadrants & 1u) != 0) {
            int temporary = box->client_width;
            box->client_width = box->client_height;
            box->client_height = temporary;
            temporary = box->content_width;
            box->content_width = box->content_height;
            box->content_height = temporary;
        }
    }
}


/* Intern a gradient ramp into the bounded per-document table, reusing an
   identical existing slot.  Returns false once the cap is reached, which
   leaves the caller with the solid fill it already emitted. */
bool layout_intern_gradient(LayoutDocument *layout,
                            const StyleGradient *gradient, size_t *slot)
{
    if (layout == NULL || gradient == NULL || slot == NULL) return false;
    if (gradient->stop_count < 2) return false;
    for (size_t i = 0; i < layout->gradient_count; i++) {
        if (memcmp(&layout->gradients[i], gradient,
                   sizeof(*gradient)) == 0) {
            *slot = i;
            return true;
        }
    }
    if (layout->gradient_count >= LAYOUT_GRADIENT_LIMIT) return false;
    layout->gradients[layout->gradient_count] = *gradient;
    *slot = layout->gradient_count++;
    return true;
}

static DrawCommand layout_normalize_command(DrawCommand command)
{
    if (command.opacity_scale == UINT16_MAX) command.opacity_scale = 0;
    else if (command.opacity_scale == 0) command.opacity_scale = 256;
    return command;
}

DrawCommand *layout_add_command(LayoutDocument *layout, DrawCommand command)
{
    command = layout_normalize_command(command);
    if (layout->count == layout->capacity) {
        size_t capacity = layout->capacity == 0 ? 128
                          : (layout->capacity < 4096
                             ? layout->capacity * 2
                             : layout->capacity + 4096);
        DrawCommand *commands = budget_realloc(layout->budget, layout->commands,
                                               capacity * sizeof(*commands));
        if (commands == NULL) return NULL;
        layout->commands = commands;
        layout->capacity = capacity;
    }
    layout->commands[layout->count] = command;
    return &layout->commands[layout->count++];
}

static size_t layout_build_text_shadow_commands(
    const LayoutContext *context, const ComputedStyle *style,
    const DrawCommand *text,
    DrawCommand shadows[STYLE_BOX_SHADOW_LIMIT])
{
    if (context == NULL || context->layout == NULL
        || context->sheet == NULL || style == NULL || text == NULL
        || text->type != DRAW_TEXT) return 0;
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(style));
    if (paint == NULL
        || (paint->components & STYLE_PAINT_COMPONENT_TEXT_SHADOW) == 0
        || paint->text_shadow_count == 0) {
        return 0;
    }
    size_t count = paint->text_shadow_count;
    if (count > STYLE_BOX_SHADOW_LIMIT) count = STYLE_BOX_SHADOW_LIMIT;
    /* CSS paints the first listed shadow above later shadows. Emit in reverse
       order, then let the caller append the foreground text. */
    for (size_t i = count; i-- > 0;) {
        const StyleBoxShadow *source = &paint->text_shadows[i];
        DrawCommand *shadow = &shadows[count - 1 - i];
        *shadow = *text;
        shadow->x = layout_add_coordinate(shadow->x, source->offset_x);
        shadow->y = layout_add_coordinate(shadow->y, source->offset_y);
        uint32_t argb = style_box_shadow_uses_current_color(source)
            ? ((uint32_t) style->color_alpha << 24)
              | (style->color & UINT32_C(0x00ffffff))
            : source->argb;
        shadow->color = argb & UINT32_C(0x00ffffff);
        shadow->opacity_scale = alpha_opacity_scale(
            (uint8_t) (argb >> 24));
        draw_command_set_text_shadow_blur(
            shadow, style_box_shadow_blur(source));
    }
    return count;
}

bool layout_add_text_shadow_commands(
    LayoutContext *context, const ComputedStyle *style,
    const DrawCommand *text)
{
    DrawCommand shadows[STYLE_BOX_SHADOW_LIMIT];
    size_t count = layout_build_text_shadow_commands(
        context, style, text, shadows);
    for (size_t i = 0; i < count; i++) {
        if (layout_add_command(context->layout, shadows[i]) == NULL) {
            return false;
        }
    }
    return true;
}

bool layout_insert_commands(LayoutContext *context, size_t index,
                            const DrawCommand *commands, size_t count)
{
    if (context == NULL || context->layout == NULL
        || (commands == NULL && count != 0)) return false;
    if (count == 0) return true;
    LayoutDocument *layout = context->layout;
    if (index > layout->count) index = layout->count;
    size_t previous_count = layout->count;
    if (count > SIZE_MAX - previous_count
        || previous_count + count > UINT32_MAX) return false;
    for (size_t i = 0; i < count; i++) {
        if (layout_add_command(layout, commands[i]) != NULL) continue;
        layout->count = previous_count;
        return false;
    }
    if (index < previous_count) {
        memmove(layout->commands + index + count, layout->commands + index,
                (previous_count - index) * sizeof(*layout->commands));
        /* layout_add_command normalized the temporary appended copies before
           reserving the range. The shift necessarily overwrites those
           copies, so normalize the caller-owned values again instead of
           copying raw opacity sentinels into the middle of the list. */
        for (size_t i = 0; i < count; i++) {
            layout->commands[index + i] =
                layout_normalize_command(commands[i]);
        }
        for (size_t i = 0; i < layout->sticky_count; i++) {
            StickyRange *range = &layout->sticky_ranges[i];
            if (index <= range->command_start) range->command_start += count;
            if (index < range->command_end) range->command_end += count;
        }
        for (size_t i = 0; i < layout->fixed_count; i++) {
            FixedRange *range = &layout->fixed_ranges[i];
            if (index <= range->command_start) range->command_start += count;
            if (index < range->command_end) range->command_end += count;
        }
        for (size_t i = 0; i < layout->node_box_count; i++) {
            LayoutNodeBox *box = &layout->node_boxes[i];
            if (index <= box->command_start) box->command_start += count;
            if (index < box->command_end) box->command_end += count;
            if (index <= box->scroll_command_start) {
                box->scroll_command_start += count;
            }
            if (index < box->scroll_command_end) {
                box->scroll_command_end += count;
            }
        }
        for (size_t i = 0; i < layout->link_count; i++) {
            LinkRegion *link = &layout->links[i];
            if ((link->url_length
                 & LAYOUT_LINK_TRANSIENT_COMMAND) == 0) continue;
            size_t command_index =
                (size_t) (-(int64_t) link->z_index - 1);
            if (command_index < index) continue;
            if (count <= (size_t) INT_MAX - 1u
                && command_index
                   <= (size_t) INT_MAX - 1u - count) {
                link->z_index = -(int) (command_index + count + 1u);
            } else {
                link->z_index = 0;
                link->url_length &= LAYOUT_LINK_URL_LENGTH_MASK;
            }
        }
        for (size_t i = 0; i < context->stacking_context_count; i++) {
            LayoutStackingContext *range = &context->stacking_contexts[i];
            if (index <= range->command_start) range->command_start += count;
            if (index < range->command_end) range->command_end += count;
            if (index < range->decoration_end) range->decoration_end += count;
        }
        for (size_t i = 0; i < context->visibility_range_count; i++) {
            LayoutVisibilityRange *range = &context->visibility_ranges[i];
            if (index <= range->command_start) range->command_start += count;
            if (index < range->command_end) range->command_end += count;
        }
    }
    return true;
}

bool layout_insert_command(LayoutContext *context, size_t index,
                           DrawCommand command)
{
    return layout_insert_commands(context, index, &command, 1);
}

bool layout_insert_text_shadow_commands(
    LayoutContext *context, const ComputedStyle *style,
    const DrawCommand *text, size_t index, size_t *inserted)
{
    DrawCommand shadows[STYLE_BOX_SHADOW_LIMIT];
    size_t count = layout_build_text_shadow_commands(
        context, style, text, shadows);
    if (inserted != NULL) *inserted = count;
    return layout_insert_commands(context, index, shadows, count);
}

uint16_t alpha_opacity_scale(uint8_t alpha)
{
    if (alpha == 0) return UINT16_MAX;
    return (uint16_t) (((unsigned) alpha * 256u + 127u) / 255u);
}

uint32_t blend_color_over(uint32_t foreground, uint8_t alpha,
                                 uint32_t background)
{
    unsigned inverse = 255u - alpha;
    unsigned red = (((foreground >> 16) & 255u) * alpha
                    + ((background >> 16) & 255u) * inverse + 127u) / 255u;
    unsigned green = (((foreground >> 8) & 255u) * alpha
                      + ((background >> 8) & 255u) * inverse + 127u) / 255u;
    unsigned blue = ((foreground & 255u) * alpha
                     + (background & 255u) * inverse + 127u) / 255u;
    return (red << 16) | (green << 8) | blue;
}

bool add_sticky_range(LayoutDocument *layout, size_t start,
                             size_t end, int origin_y, int top)
{
    if (start >= end) return true;
    if (layout->sticky_count == layout->sticky_capacity) {
        size_t capacity = layout->sticky_capacity == 0
                          ? 4 : layout->sticky_capacity * 2;
        StickyRange *ranges = budget_realloc(
            layout->budget, layout->sticky_ranges,
            capacity * sizeof(*ranges));
        if (ranges == NULL) return false;
        layout->sticky_ranges = ranges;
        layout->sticky_capacity = capacity;
    }
    layout->sticky_ranges[layout->sticky_count++] = (StickyRange) {
        start, end, origin_y, top
    };
    return true;
}

bool add_fixed_range(LayoutDocument *layout, size_t start, size_t end,
                            size_t link_start, size_t link_end,
                            size_t control_start, size_t control_end,
                            int origin_y, int height, int inset,
                            bool from_bottom)
{
    if (start >= end) return true;
    if (layout->fixed_count == layout->fixed_capacity) {
        size_t capacity = layout->fixed_capacity == 0
                          ? 4 : layout->fixed_capacity * 2;
        FixedRange *ranges = budget_realloc(
            layout->budget, layout->fixed_ranges,
            capacity * sizeof(*ranges));
        if (ranges == NULL) return false;
        layout->fixed_ranges = ranges;
        layout->fixed_capacity = capacity;
    }
    layout->fixed_ranges[layout->fixed_count++] = (FixedRange) {
        start, end, link_start, link_end, control_start, control_end,
        origin_y, height, inset, from_bottom
    };
    return true;
}


bool layout_node_within(const lxb_dom_node_t *node,
                        const lxb_dom_node_t *ancestor)
{
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

bool layout_positioned_command_escapes_clip(
    const LayoutDocument *layout, size_t command_index,
    const LayoutNodeBox *clip_box)
{
    if (layout == NULL || clip_box == NULL || clip_box->node == NULL) {
        return false;
    }
    /* Deferred out-of-flow layout can place a positioned sibling's command
       inside an earlier normal-flow sibling's numeric command interval. A
       range check alone would then clip the sibling even though it is not a
       DOM descendant (a search submit button was painted underneath its
       overflow:hidden input wrapper). Recover the innermost positioned
       range first; unlike an arbitrary narrow box, it identifies the
       deferred subtree that made the numeric intervals overlap. */
    const LayoutNodeBox *positioned_owner = NULL;
    size_t positioned_span = SIZE_MAX;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *candidate = &layout->node_boxes[i];
        if (candidate->positioned_ancestor_distance == 0
            || command_index < candidate->command_start
            || command_index >= candidate->command_end) continue;
        size_t span = candidate->command_end - candidate->command_start;
        if (positioned_owner == NULL || span < positioned_span) {
            positioned_owner = candidate;
            positioned_span = span;
        }
    }
    if (positioned_owner != NULL
        && !layout_node_within(positioned_owner->node, clip_box->node)) {
        /* A positioned ancestor can legitimately contain the clipping box;
           only a disjoint subtree escapes this clip. */
        return !layout_node_within(clip_box->node, positioned_owner->node);
    }
    if (positioned_owner != NULL) {
        const LayoutNodeBox *positioned = positioned_owner;
        uint8_t containing_distance =
            positioned->positioned_ancestor_distance;
        unsigned clip_distance = 1;
        for (lxb_dom_node_t *ancestor = positioned->node->parent;
             ancestor != NULL && clip_distance < UINT8_MAX;
             ancestor = ancestor->parent, clip_distance++) {
            if (ancestor != clip_box->node) continue;
            if (clip_distance < containing_distance) return true;
            break;
        }
    }
    return false;
}

void translate_node_subtree(LayoutDocument *layout,
                                   lxb_dom_node_t *node, int dx, int dy)
{
    LayoutNodeBox *root = layout_box_for_node_mutable(layout, node);
    if (root == NULL) return;
    size_t root_index = (size_t) (root - layout->node_boxes);
    if (layout->node_interaction_ranges == NULL
        || root_index >= layout->node_interaction_capacity) return;
    size_t command_start = root->command_start;
    size_t command_end = root->command_end;
    if (command_end > layout->count) command_end = layout->count;
    for (size_t i = command_start; i < command_end; i++) {
        bool viewport_fixed = false;
        for (size_t fixed = 0; fixed < layout->fixed_count; fixed++) {
            const FixedRange *range = &layout->fixed_ranges[fixed];
            if (i >= range->command_start && i < range->command_end) {
                viewport_fixed = true;
                break;
            }
        }
        if (viewport_fixed) continue;
        layout->commands[i].x += dx;
        layout->commands[i].y += dy;
    }
    for (size_t i = 0; i < layout->sticky_count; i++) {
        StickyRange *range = &layout->sticky_ranges[i];
        if (range->command_start >= command_start
            && range->command_end <= command_end) {
            range->origin_y += dy;
        }
    }
    const uint32_t *interactions =
        layout->node_interaction_ranges
        + root_index * LAYOUT_NODE_INTERACTION_STRIDE;
    size_t link_start =
        interactions[LAYOUT_NODE_LINK_START] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_START] : layout->link_count;
    size_t link_end =
        interactions[LAYOUT_NODE_LINK_END] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_END] : layout->link_count;
    for (size_t i = link_start; i < link_end; i++) {
        bool viewport_fixed = false;
        for (size_t fixed = 0; fixed < layout->fixed_count; fixed++) {
            const FixedRange *range = &layout->fixed_ranges[fixed];
            if (i >= range->link_start && i < range->link_end) {
                viewport_fixed = true;
                break;
            }
        }
        if (viewport_fixed) continue;
        layout->links[i].x += dx;
        layout->links[i].y += dy;
    }
    size_t control_start =
        interactions[LAYOUT_NODE_CONTROL_START] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_START] : layout->control_count;
    size_t control_end =
        interactions[LAYOUT_NODE_CONTROL_END] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_END] : layout->control_count;
    for (size_t i = control_start; i < control_end; i++) {
        bool viewport_fixed = false;
        for (size_t fixed = 0; fixed < layout->fixed_count; fixed++) {
            const FixedRange *range = &layout->fixed_ranges[fixed];
            if (i >= range->control_start && i < range->control_end) {
                viewport_fixed = true;
                break;
            }
        }
        if (viewport_fixed) continue;
        layout->controls[i].x += dx;
        layout->controls[i].y += dy;
    }

    /*
     * Node boxes are appended post-order, so they are not a simple index
     * interval. Walk this DOM subtree and use the existing box hash rather
     * than testing every document box with another ancestor walk.
     */
    lxb_dom_node_t *at = node;
    while (at != NULL) {
        LayoutNodeBox *box = layout_box_for_node_mutable(layout, at);
        if (box != NULL && box->command_start >= command_start
            && box->command_end <= command_end
            && box->positioned_ancestor_distance != UINT8_MAX) {
            box->x += dx;
            box->y += dy;
        }
        if (at->first_child != NULL) {
            at = at->first_child;
            continue;
        }
        while (at != node && at->next == NULL) at = at->parent;
        at = at == node ? NULL : at->next;
    }
}


#define PAINT_TREE_NONE UINT32_MAX

typedef struct {
    uint32_t first_child;
    uint32_t next_sibling;
} PaintTreeNode;

_Static_assert(sizeof(PaintTreeNode) == 8,
               "paint-order transient node grew");

typedef struct {
    const LayoutDocument *layout;
    const LayoutContext *context;
    const uint32_t *owners;
    uint32_t root;
} PaintTreeSortContext;

typedef struct {
    const lxb_dom_node_t *node;
    uint32_t context_plus_one;
} PaintContextMapEntry;

static int paint_tree_z_index(const PaintTreeSortContext *sort,
                              uint32_t node)
{
    if (node < sort->layout->count) {
        /* An effective z-index creates a context. Its value is also copied
           onto descendant commands for hit testing, but must not be applied
           a second time while sorting *inside* that context: doing so lets
           the ancestor's global z-index outrank a nested z-index sibling. */
        if (sort->owners[node] != sort->root) return 0;
        return sort->layout->commands[node].z_index;
    }
    return sort->context->stacking_contexts[
        node - sort->layout->count].z_index;
}

static uint32_t paint_tree_source_order(
    const PaintTreeSortContext *sort, uint32_t node)
{
    if (node < sort->layout->count) return node;
    return sort->context->stacking_contexts[
        node - sort->layout->count].command_start;
}

static unsigned paint_tree_group(const PaintTreeSortContext *sort,
                                 uint32_t node)
{
    bool positioned = node >= sort->layout->count;
    bool decoration = false;
    if (!positioned) {
        positioned = draw_command_has_positioned_phase(
            &sort->layout->commands[node]);
        uint32_t owner = sort->owners[node];
        if (owner != sort->root) {
            decoration = node < sort->context
                ->stacking_contexts[owner].decoration_end;
        }
    }
    if (decoration) return 0;
    int z_index = paint_tree_z_index(sort, node);
    if (z_index < 0) return 1;
    if (z_index > 0) return 4;
    return positioned ? 3 : 2;
}

static bool paint_tree_before(const PaintTreeSortContext *sort,
                              uint32_t left, uint32_t right)
{
    unsigned left_group = paint_tree_group(sort, left);
    unsigned right_group = paint_tree_group(sort, right);
    if (left_group != right_group) return left_group < right_group;
    if (left_group == 1 || left_group == 4) {
        int left_z = paint_tree_z_index(sort, left);
        int right_z = paint_tree_z_index(sort, right);
        if (left_z != right_z) return left_z < right_z;
    }
    return paint_tree_source_order(sort, left)
        <= paint_tree_source_order(sort, right);
}

static uint32_t paint_tree_split(PaintTreeNode *nodes, uint32_t head,
                                 size_t width)
{
    if (head == PAINT_TREE_NONE) return PAINT_TREE_NONE;
    while (--width != 0 && nodes[head].next_sibling != PAINT_TREE_NONE) {
        head = nodes[head].next_sibling;
    }
    uint32_t next = nodes[head].next_sibling;
    nodes[head].next_sibling = PAINT_TREE_NONE;
    return next;
}

static bool paint_order_work(LayoutContext *context, size_t *work)
{
    (*work)++;
    if (context == NULL || (*work & 4095u) != 0) return true;
    return layout_batch_cooperate(context, 4096);
}

static uint32_t paint_tree_merge(PaintTreeNode *nodes,
                                 uint32_t left, uint32_t right,
                                 const PaintTreeSortContext *sort,
                                 uint32_t *tail, LayoutContext *context,
                                 size_t *work, bool *ok)
{
    uint32_t head = PAINT_TREE_NONE;
    uint32_t *next = &head;
    uint32_t last = PAINT_TREE_NONE;
    while (left != PAINT_TREE_NONE || right != PAINT_TREE_NONE) {
        uint32_t selected;
        if (right == PAINT_TREE_NONE
            || (left != PAINT_TREE_NONE
                && paint_tree_before(sort, left, right))) {
            selected = left;
            left = nodes[left].next_sibling;
        } else {
            selected = right;
            right = nodes[right].next_sibling;
        }
        *next = selected;
        last = selected;
        next = &nodes[selected].next_sibling;
        if (!paint_order_work(context, work)) {
            *ok = false;
            return head;
        }
    }
    *next = PAINT_TREE_NONE;
    *tail = last;
    return head;
}

static uint32_t paint_tree_sort(PaintTreeNode *nodes, uint32_t head,
                                const PaintTreeSortContext *sort,
                                LayoutContext *context, size_t *work,
                                bool *ok)
{
    if (head == PAINT_TREE_NONE
        || nodes[head].next_sibling == PAINT_TREE_NONE) return head;
    for (size_t width = 1; width <= UINT32_MAX / 2u; width *= 2u) {
        uint32_t at = head;
        uint32_t sorted = PAINT_TREE_NONE;
        uint32_t sorted_tail = PAINT_TREE_NONE;
        size_t merges = 0;
        while (at != PAINT_TREE_NONE) {
            merges++;
            uint32_t left = at;
            uint32_t right = paint_tree_split(nodes, left, width);
            at = paint_tree_split(nodes, right, width);
            uint32_t merged_tail = PAINT_TREE_NONE;
            uint32_t merged = paint_tree_merge(
                nodes, left, right, sort, &merged_tail,
                context, work, ok);
            if (!*ok) return sorted;
            if (sorted == PAINT_TREE_NONE) sorted = merged;
            else nodes[sorted_tail].next_sibling = merged;
            sorted_tail = merged_tail;
        }
        head = sorted;
        if (merges <= 1) break;
    }
    return head;
}

bool build_paint_order(LayoutDocument *layout, LayoutContext *context)
{
    if (layout->count == 0) return true;
    size_t context_count = context == NULL
        ? 0 : context->stacking_context_count;
    if (layout->count > UINT32_MAX
        || context_count > UINT32_MAX - layout->count - 1u
        || context_count > SIZE_MAX / 2u) return false;
    size_t node_count = layout->count + context_count + 1u;
    if (node_count > SIZE_MAX / sizeof(PaintTreeNode)) return false;
    uint32_t root = (uint32_t) (node_count - 1u);
    uint32_t *order = budget_malloc(
        layout->budget, layout->count * sizeof(*order));
    PaintTreeNode *nodes = budget_malloc(
        layout->budget, node_count * sizeof(*nodes));
    uint32_t *stack = budget_malloc(
        layout->budget, (context_count + 1u) * sizeof(*stack));
    size_t context_map_capacity = 1u;
    while (context_map_capacity < context_count * 2u) {
        if (context_map_capacity > SIZE_MAX / 2u) {
            context_map_capacity = 0;
            break;
        }
        context_map_capacity *= 2u;
    }
    if (context_count != 0 && context_map_capacity == 0) {
        budget_free(layout->budget, stack);
        budget_free(layout->budget, nodes);
        budget_free(layout->budget, order);
        return false;
    }
    PaintContextMapEntry *context_map = context_count == 0 ? NULL
        : budget_calloc(layout->budget, context_map_capacity,
                        sizeof(*context_map));
    if (order == NULL || nodes == NULL || stack == NULL
        || (context_count != 0 && context_map == NULL)) {
        budget_free(layout->budget, context_map);
        budget_free(layout->budget, stack);
        budget_free(layout->budget, nodes);
        budget_free(layout->budget, order);
        return false;
    }
    layout_finish_work_slice(context);
    for (size_t i = 0; i < node_count; i++) {
        nodes[i].first_child = PAINT_TREE_NONE;
        nodes[i].next_sibling = PAINT_TREE_NONE;
    }
    for (size_t i = 0; i < layout->count; i++) order[i] = root;

    if (context_map != NULL) {
        size_t mask = context_map_capacity - 1u;
        for (size_t i = 0; i < context_count; i++) {
            const lxb_dom_node_t *node = context->stacking_contexts[i].node;
            size_t slot = layout_pointer_hash(node) & mask;
            while (context_map[slot].node != NULL) {
                slot = (slot + 1u) & mask;
            }
            context_map[slot].node = node;
            context_map[slot].context_plus_one = (uint32_t) i + 1u;
        }
    }

    size_t work = 0;
    /* Context records are appended post-order. Assign parents before
       children, then let the deepest range win command ownership. */
    for (size_t reverse = context_count; reverse != 0; reverse--) {
        size_t i = reverse - 1u;
        const LayoutStackingContext *range =
            &context->stacking_contexts[i];
        size_t end = range->command_end;
        if (end > layout->count) end = layout->count;
        for (size_t command = range->command_start;
             command < end; command++) {
            order[command] = (uint32_t) i;
            if (!paint_order_work(context, &work)) goto cancelled;
        }
    }

    /*
     * Commands are prepended in reverse source order. Contexts are linked
     * afterward and therefore remain ahead of equal-source commands, just as
     * in the former append-based tree. The stable merge sort decides every
     * observable paint phase.
     */
    for (size_t reverse = layout->count; reverse != 0; reverse--) {
        size_t i = reverse - 1u;
        uint32_t owner = order[i];
        uint32_t parent = owner == root
            ? root : (uint32_t) (layout->count + owner);
        nodes[i].next_sibling = nodes[parent].first_child;
        nodes[parent].first_child = (uint32_t) i;
        if (!paint_order_work(context, &work)) goto cancelled;
    }

    /* The post-order range stack is the common, allocation-free parent
       proof. Flex/grid moves or inserted decorations can make a valid child
       span cease to nest numerically, however. Only when the stack has no
       answer, recover the nearest context from actual DOM ancestry (the CSS
       relationship of record). This preserves the linear fast path and
       avoids treating a deferred positioned sibling as a root context. */
    size_t ancestry_work = 0;
    size_t context_stack_count = 0;
    for (size_t reverse = context_count; reverse != 0; reverse--) {
        size_t i = reverse - 1u;
        const LayoutStackingContext *range =
            &context->stacking_contexts[i];
        uint32_t node_index = (uint32_t) (layout->count + i);
        while (context_stack_count != 0) {
            size_t candidate_index = stack[context_stack_count - 1u];
            const LayoutStackingContext *candidate =
                &context->stacking_contexts[candidate_index];
            if (range->command_start >= candidate->command_start
                && range->command_end <= candidate->command_end
                && layout_node_within(range->node, candidate->node)) {
                break;
            }
            context_stack_count--;
            if (!paint_order_work(context, &work)) goto cancelled;
        }
        uint32_t parent = context_stack_count == 0
            ? root : (uint32_t) (
                layout->count + stack[context_stack_count - 1u]);
        if (parent == root) {
            const lxb_dom_node_t *ancestor = range->node == NULL
                ? NULL : range->node->parent;
            while (ancestor != NULL) {
                size_t slot = layout_pointer_hash(ancestor)
                              & (context_map_capacity - 1u);
                while (context_map[slot].node != NULL) {
                    if (context_map[slot].node == ancestor) {
                        uint32_t candidate =
                            context_map[slot].context_plus_one - 1u;
                        parent = (uint32_t) (layout->count + candidate);
                        ancestor = NULL;
                        break;
                    }
                    slot = (slot + 1u) & (context_map_capacity - 1u);
                    if (!paint_order_work(context, &work)) goto cancelled;
                }
                if (ancestor == NULL) break;
                ancestor = ancestor->parent;
                ancestry_work++;
                if (!paint_order_work(context, &work)) goto cancelled;
                if (ancestry_work >= LAYOUT_FALLBACK_VISIT_LIMIT) {
                    goto cancelled;
                }
            }
        }
        nodes[node_index].next_sibling = nodes[parent].first_child;
        nodes[parent].first_child = node_index;
        stack[context_stack_count++] = (uint32_t) i;
        if (!paint_order_work(context, &work)) goto cancelled;
    }
    budget_free(layout->budget, context_map);
    context_map = NULL;
    PaintTreeSortContext sort = {
        .layout = layout,
        .context = context,
        .owners = order,
        .root = root
    };
    for (size_t i = 0; i <= context_count; i++) {
        uint32_t node_index = i == context_count
            ? root : (uint32_t) (layout->count + i);
        bool ok = true;
        nodes[node_index].first_child = paint_tree_sort(
            nodes, nodes[node_index].first_child, &sort,
            context, &work, &ok);
        if (!ok) goto cancelled;
    }

    size_t stack_count = 0;
    size_t output = 0;
    uint32_t at = nodes[root].first_child;
    while (at != PAINT_TREE_NONE) {
        uint32_t next = nodes[at].next_sibling;
        if (at >= layout->count) {
            if (next != PAINT_TREE_NONE) stack[stack_count++] = next;
            at = nodes[at].first_child;
            if (at == PAINT_TREE_NONE && stack_count != 0) {
                at = stack[--stack_count];
            }
            continue;
        }
        order[output++] = at;
        at = next;
        if (at == PAINT_TREE_NONE && stack_count != 0) {
            at = stack[--stack_count];
        }
        if (!paint_order_work(context, &work)) goto cancelled;
    }
    if (output != layout->count) goto cancelled;
    if (context != NULL && (work & 4095u) != 0
        && !layout_batch_cooperate(context, work & 4095u)) goto cancelled;
    budget_free(layout->budget, stack);
    budget_free(layout->budget, nodes);
    layout->paint_order = order;
    layout->paint_order_count = layout->count;
    return true;

cancelled:
    budget_free(layout->budget, context_map);
    budget_free(layout->budget, stack);
    budget_free(layout->budget, nodes);
    budget_free(layout->budget, order);
    return false;
}

static void draw_command_vertical_ink_bounds(const LayoutDocument *layout,
                                             const DrawCommand *command,
                                             int64_t *top, int64_t *bottom)
{
    int shadow_blur = draw_command_text_shadow_blur(command);
    *top = (int64_t) command->y - shadow_blur;
    *bottom = (int64_t) command->y + command->height + shadow_blur;
    int underline_offset = 0;
    if (command->type != DRAW_TEXT
        || (command->image_fit & LAYOUT_TEXT_DECORATION_UNDERLINE) == 0
        || !draw_command_text_underline_offset(
               command, &underline_offset)) return;
    const FontFace *face = font_context_face_variant(
        layout->fonts, layout->web_fonts, draw_command_font_family(command),
        draw_command_font_italic(command), draw_uses_bold_face(command));
    int pixel_height_fixed = draw_command_text_font_size_fixed(command);
    if (pixel_height_fixed > TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64) {
        pixel_height_fixed = TILEFINCH_FONT_RASTER_PIXEL_LIMIT * 64;
    }
    if (pixel_height_fixed <= 0) return;
    int thickness = pixel_height_fixed >= 32 * 64 ? 2 : 1;
    int baseline = font_line_baseline_at_size(
        face, pixel_height_fixed, command->height);
    int zero = baseline > 0 ? baseline : command->height - thickness;
    int64_t underline_top = (int64_t) command->y + zero
                            + underline_offset;
    int64_t underline_bottom = underline_top + thickness;
    if (underline_top < *top) *top = underline_top;
    if (underline_bottom > *bottom) *bottom = underline_bottom;
}

static bool draw_commands_intersect(const LayoutDocument *layout,
                                    const DrawCommand *left,
                                    const DrawCommand *right)
{
    int64_t left_top = 0, left_bottom = 0;
    int64_t right_top = 0, right_bottom = 0;
    draw_command_vertical_ink_bounds(
        layout, left, &left_top, &left_bottom);
    draw_command_vertical_ink_bounds(
        layout, right, &right_top, &right_bottom);
    return (int64_t) left->x < (int64_t) right->x + right->width
        && (int64_t) right->x < (int64_t) left->x + left->width
        && left_top < right_bottom && right_top < left_bottom;
}

bool build_spatial_index(LayoutDocument *layout,
                                LayoutContext *context)
{
    if (layout->count == 0) return true;
    if (layout->count > UINT32_MAX) return false;
    layout->command_flags = budget_calloc(layout->budget, layout->count,
                                           sizeof(*layout->command_flags));
    if (layout->command_flags == NULL) return false;
    for (size_t range = 0; range < layout->fixed_count; range++) {
        size_t end = layout->fixed_ranges[range].command_end;
        if (end > layout->count) end = layout->count;
        for (size_t i = layout->fixed_ranges[range].command_start;
             i < end; i++) {
            layout->command_flags[i] |= LAYOUT_COMMAND_FIXED;
        }
    }
    for (size_t box_index = 0; box_index < layout->node_box_count;
         box_index++) {
        const LayoutNodeBox *box = &layout->node_boxes[box_index];
        if (!box->clips_x && !box->clips_y) continue;
        /* Horizontal scrolling changes x but leaves the command's vertical
         * band stable. Only a vertically scrollable clip must bypass the
         * document-y index. */
        bool dynamic = box->clips_y && box->content_height > box->height;
        uint8_t flags = LAYOUT_COMMAND_OVERFLOW
                        | (dynamic ? LAYOUT_COMMAND_DYNAMIC_OVERFLOW : 0)
                        | (box->clips_x ? LAYOUT_COMMAND_CLIPPED_X : 0);
        size_t end = box->scroll_command_end;
        if (end > layout->count) end = layout->count;
        for (size_t i = box->scroll_command_start; i < end; i++) {
            if (layout_positioned_command_escapes_clip(layout, i, box)) {
                continue;
            }
            layout->command_flags[i] |= flags;
        }
    }
    layout->spatial_band_count = ((size_t) layout->height
                                  + LAYOUT_SPATIAL_BAND_HEIGHT - 1)
                                 / LAYOUT_SPATIAL_BAND_HEIGHT;
    if (layout->spatial_band_count == 0) layout->spatial_band_count = 1;
    size_t *counts = budget_calloc(layout->budget,
                                   layout->spatial_band_count,
                                   sizeof(*counts));
    if (counts == NULL) return false;
    size_t global_count = 0, overflow_count = 0, total = 0;
    size_t all_overflow_count = 0;
    bool has_overflow = false;
    size_t checkpoint = 4096;
    for (size_t order = 0; order < layout->paint_order_count; order++) {
        if (!layout_batch_checkpoint(
                context, order, layout->paint_order_count,
                &checkpoint)) {
            budget_free(layout->budget, counts);
            return false;
        }
        size_t index = layout->paint_order[order];
        if (layout->command_flags[index] & LAYOUT_COMMAND_OVERFLOW) {
            has_overflow = true;
            all_overflow_count++;
        }
        if ((layout->command_flags[index]
             & (LAYOUT_COMMAND_OVERFLOW | LAYOUT_COMMAND_DYNAMIC_OVERFLOW))
              == (LAYOUT_COMMAND_OVERFLOW
                  | LAYOUT_COMMAND_DYNAMIC_OVERFLOW)
            && !(layout->command_flags[index] & LAYOUT_COMMAND_FIXED)) {
            overflow_count++;
        }
        if (layout->command_flags[index] & LAYOUT_COMMAND_FIXED) {
            continue;
        }
        const DrawCommand *command = &layout->commands[index];
        int64_t command_top = 0, command_bottom = 0;
        draw_command_vertical_ink_bounds(
            layout, command, &command_top, &command_bottom);
        if (command->height <= 0 || command_top >= layout->height
            || command_bottom <= 0) continue;
        int first = command_top <= 0 ? 0
                    : (int) (command_top / LAYOUT_SPATIAL_BAND_HEIGHT);
        int64_t last_y = command_bottom - 1;
        int last = last_y >= layout->height
                   ? (int) layout->spatial_band_count - 1
                   : (int) (last_y / LAYOUT_SPATIAL_BAND_HEIGHT);
        size_t span = (size_t) (last - first + 1);
        if (span > 8) global_count++;
        else {
            for (int band = first; band <= last; band++) {
                if (counts[band] == SIZE_MAX || total == SIZE_MAX) {
                    budget_free(layout->budget, counts);
                    return false;
                }
                counts[band]++;
                total++;
            }
        }
    }
    if (context != NULL && (layout->paint_order_count & 4095u) != 0
        && !layout_batch_cooperate(
            context, layout->paint_order_count & 4095u)) {
        budget_free(layout->budget, counts);
        return false;
    }
    layout->spatial_band_offsets = budget_malloc(
        layout->budget, (layout->spatial_band_count + 1)
                        * sizeof(*layout->spatial_band_offsets));
    layout->spatial_band_orders = total == 0 ? NULL : budget_malloc(
        layout->budget, total * sizeof(*layout->spatial_band_orders));
    layout->spatial_global_orders = global_count == 0 ? NULL : budget_malloc(
        layout->budget, global_count * sizeof(*layout->spatial_global_orders));
    layout->overflow_orders = overflow_count == 0 ? NULL : budget_malloc(
        layout->budget, overflow_count * sizeof(*layout->overflow_orders));
    uint32_t *all_overflow_orders = all_overflow_count == 0 ? NULL
        : budget_malloc(
              layout->budget,
              all_overflow_count * sizeof(*all_overflow_orders));
    if (layout->spatial_band_offsets == NULL
        || (total != 0 && layout->spatial_band_orders == NULL)
        || (global_count != 0 && layout->spatial_global_orders == NULL)
        || (overflow_count != 0 && layout->overflow_orders == NULL)
        || (all_overflow_count != 0 && all_overflow_orders == NULL)) {
        budget_free(layout->budget, all_overflow_orders);
        budget_free(layout->budget, counts);
        return false;
    }
    layout->spatial_band_offsets[0] = 0;
    for (size_t band = 0; band < layout->spatial_band_count; band++) {
        layout->spatial_band_offsets[band + 1] =
            layout->spatial_band_offsets[band] + counts[band];
        counts[band] = layout->spatial_band_offsets[band];
    }
    size_t global_at = 0, overflow_at = 0, all_overflow_at = 0;
    checkpoint = 4096;
    for (size_t order = 0; order < layout->paint_order_count; order++) {
        if (!layout_batch_checkpoint(
                context, order, layout->paint_order_count,
                &checkpoint)) {
            budget_free(layout->budget, counts);
            budget_free(layout->budget, all_overflow_orders);
            return false;
        }
        size_t index = layout->paint_order[order];
        if (layout->command_flags[index] & LAYOUT_COMMAND_OVERFLOW) {
            all_overflow_orders[all_overflow_at++] = (uint32_t) order;
        }
        if ((layout->command_flags[index]
             & (LAYOUT_COMMAND_OVERFLOW | LAYOUT_COMMAND_DYNAMIC_OVERFLOW))
              == (LAYOUT_COMMAND_OVERFLOW
                  | LAYOUT_COMMAND_DYNAMIC_OVERFLOW)
            && !(layout->command_flags[index] & LAYOUT_COMMAND_FIXED)) {
            layout->overflow_orders[overflow_at++] = (uint32_t) order;
        }
        if (layout->command_flags[index] & LAYOUT_COMMAND_FIXED) {
            continue;
        }
        const DrawCommand *command = &layout->commands[index];
        int64_t command_top = 0, command_bottom = 0;
        draw_command_vertical_ink_bounds(
            layout, command, &command_top, &command_bottom);
        if (command->height <= 0 || command_top >= layout->height
            || command_bottom <= 0) continue;
        int first = command_top <= 0 ? 0
                    : (int) (command_top / LAYOUT_SPATIAL_BAND_HEIGHT);
        int64_t last_y = command_bottom - 1;
        int last = last_y >= layout->height
                   ? (int) layout->spatial_band_count - 1
                   : (int) (last_y / LAYOUT_SPATIAL_BAND_HEIGHT);
        if ((size_t) (last - first + 1) > 8) {
            layout->spatial_global_orders[global_at++] = (uint32_t) order;
        } else {
            for (int band = first; band <= last; band++) {
                layout->spatial_band_orders[counts[band]++] = (uint32_t) order;
            }
        }
    }
    if (context != NULL && (layout->paint_order_count & 4095u) != 0
        && !layout_batch_cooperate(
            context, layout->paint_order_count & 4095u)) {
        budget_free(layout->budget, counts);
        budget_free(layout->budget, all_overflow_orders);
        return false;
    }
    if (has_overflow) {
        checkpoint = 4096;
        size_t late_work = 0;
        for (size_t order = 0; order < layout->paint_order_count; order++) {
            if (!layout_batch_checkpoint(
                    context, order, layout->paint_order_count,
                    &checkpoint)) {
                budget_free(layout->budget, counts);
                budget_free(layout->budget, all_overflow_orders);
                return false;
            }
            size_t index = layout->paint_order[order];
            const DrawCommand *candidate = &layout->commands[index];
            uint8_t flags = layout->command_flags[index];
            if ((flags & (LAYOUT_COMMAND_FIXED | LAYOUT_COMMAND_OVERFLOW)) != 0
                || candidate->z_index < 0
                || (candidate->z_index == 0
                    && !draw_command_has_positioned_phase(candidate))) {
                continue;
            }
            int64_t top = 0, bottom = 0;
            draw_command_vertical_ink_bounds(
                layout, candidate, &top, &bottom);
            if (candidate->height <= 0 || top >= layout->height
                || bottom <= 0) continue;
            int first = top <= 0 ? 0
                : (int) (top / LAYOUT_SPATIAL_BAND_HEIGHT);
            int64_t last_y = bottom - 1;
            int last = last_y >= layout->height
                ? (int) layout->spatial_band_count - 1
                : (int) (last_y / LAYOUT_SPATIAL_BAND_HEIGHT);
            bool overlaps_prior_overflow = false;
            if (last - first + 1 <= 8) {
                /* Overflow commands are normally a tiny subset of a page.
                   Scanning that unique paint-ordered subset avoids revisiting
                   ordinary commands (and the same overflow command through
                   several spatial bands) for every positioned candidate. */
                for (size_t at = 0;
                     at < all_overflow_at
                     && !overlaps_prior_overflow; at++) {
                    size_t prior_order = all_overflow_orders[at];
                    if (prior_order >= order) break;
                    size_t prior_index = layout->paint_order[prior_order];
                    if (draw_commands_intersect(
                            layout, candidate,
                            &layout->commands[prior_index])) {
                        overlaps_prior_overflow = true;
                    }
                    if (!paint_order_work(
                            context, &late_work)) {
                        budget_free(layout->budget, counts);
                        budget_free(
                            layout->budget, all_overflow_orders);
                        return false;
                    }
                }
            }
            if (overlaps_prior_overflow) {
                /* A stacking context is one atomic paint unit. Hoisting only
                   its intersecting background would paint that background
                   over later children such as a header logo. Keep the
                   entire smallest context in the exact overlay merge. */
                size_t range_start = index;
                size_t range_end = index + 1u;
                size_t smallest_span = SIZE_MAX;
                if (context != NULL) {
                    for (size_t context_index = 0;
                         context_index < context->stacking_context_count;
                         context_index++) {
                        const LayoutStackingContext *stacking =
                            &context->stacking_contexts[context_index];
                        if (index < stacking->command_start
                            || index >= stacking->command_end) continue;
                        size_t span = stacking->command_end
                                      - stacking->command_start;
                        if (span < smallest_span) {
                            smallest_span = span;
                            range_start = stacking->command_start;
                            range_end = stacking->command_end;
                        }
                        if (!paint_order_work(context, &late_work)) {
                            budget_free(layout->budget, counts);
                            budget_free(
                                layout->budget, all_overflow_orders);
                            return false;
                        }
                    }
                }
                if (range_end > layout->count) range_end = layout->count;
                for (size_t command_index = range_start;
                     command_index < range_end; command_index++) {
                    if ((layout->command_flags[command_index]
                         & (LAYOUT_COMMAND_FIXED
                            | LAYOUT_COMMAND_OVERFLOW)) == 0) {
                        layout->command_flags[command_index] |=
                            LAYOUT_COMMAND_LATE_POSITIONED;
                    }
                    if (!paint_order_work(context, &late_work)) {
                        budget_free(layout->budget, counts);
                        budget_free(layout->budget, all_overflow_orders);
                        return false;
                    }
                }
            }
        }
        size_t late_count = 0;
        for (size_t order = 0; order < layout->paint_order_count; order++) {
            size_t index = layout->paint_order[order];
            if (layout->command_flags[index]
                & LAYOUT_COMMAND_LATE_POSITIONED) late_count++;
        }
        if (late_count != 0) {
            layout->late_positioned_orders = budget_malloc(
                layout->budget,
                late_count * sizeof(*layout->late_positioned_orders));
            if (layout->late_positioned_orders == NULL) {
                budget_free(layout->budget, counts);
                budget_free(layout->budget, all_overflow_orders);
                return false;
            }
            size_t late_at = 0;
            for (size_t order = 0; order < layout->paint_order_count;
                 order++) {
                size_t index = layout->paint_order[order];
                if (layout->command_flags[index]
                    & LAYOUT_COMMAND_LATE_POSITIONED) {
                    layout->late_positioned_orders[late_at++] =
                        (uint32_t) order;
                }
            }
            layout->late_positioned_order_count = late_at;
        }
    }
    budget_free(layout->budget, all_overflow_orders);
    budget_free(layout->budget, counts);
    layout->spatial_band_order_count = total;
    layout->spatial_global_count = global_at;
    layout->overflow_order_count = overflow_at;
    return true;
}

int root_scroll_width_after_clipping(LayoutDocument *layout,
                                            int viewport_width)
{
    int maximum = viewport_width;
    bool trace = LAYOUT_TRACE(layout, SCROLL_WIDTH);
    size_t trace_lines = 0;
    size_t clip_count = 0;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].clips_x) clip_count++;
    }
    const LayoutNodeBox **clip_boxes = NULL;
    if (clip_count != 0) {
        size_t clip_bytes = clip_count * sizeof(*clip_boxes);
        size_t remaining = budget_remaining(layout->budget);
        if (clip_bytes < remaining && remaining - clip_bytes >= 128) {
            clip_boxes = budget_malloc(layout->budget, clip_bytes);
        }
        if (clip_boxes != NULL) {
            size_t at = 0;
            for (size_t i = 0; i < layout->node_box_count; i++) {
                if (layout->node_boxes[i].clips_x) {
                    clip_boxes[at++] = &layout->node_boxes[i];
                }
            }
        }
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        const LayoutNodeBox *candidate = &layout->node_boxes[i];
        bool clipped = false;
        size_t ancestor_count = clip_boxes == NULL
                                ? layout->node_box_count : clip_count;
        for (size_t j = 0; j < ancestor_count; j++) {
            const LayoutNodeBox *ancestor = clip_boxes == NULL
                ? &layout->node_boxes[j] : clip_boxes[j];
            if (ancestor == candidate || !ancestor->clips_x) continue;
            if (layout_node_within(candidate->node, ancestor->node)) {
                clipped = true;
                break;
            }
        }
        if (clipped) continue;
        int64_t right = (int64_t) candidate->x + candidate->width;
        if (right > maximum && right <= INT_MAX) {
            maximum = (int) right;
            if (trace && trace_lines++ < 64) {
                size_t id_length = 0, class_length = 0;
                const char *id = document_attribute(
                    candidate->node, "id", &id_length);
                const char *class_name = document_attribute(
                    candidate->node, "class", &class_length);
                fprintf(stderr,
                        "layout-scroll-width node=%.*s class=%.*s "
                        "box=%d,%d,%dx%d right=%d\n",
                        (int) id_length, id == NULL ? "" : id,
                        (int) class_length,
                        class_name == NULL ? "" : class_name,
                        candidate->x, candidate->y, candidate->width,
                        candidate->height, maximum);
                size_t depth = 0;
                for (lxb_dom_node_t *parent = candidate->node->parent;
                     parent != NULL && depth < 8;
                     parent = parent->parent, depth++) {
                    size_t parent_class_length = 0;
                    const char *parent_class = document_attribute(
                        parent, "class", &parent_class_length);
                    const LayoutNodeBox *parent_box = layout_box_for_node(
                        layout, parent);
                    fprintf(stderr,
                            "layout-scroll-parent depth=%zu class=%.*s "
                            "box=%d,%d,%dx%d clips=%d/%d\n",
                            depth, (int) parent_class_length,
                            parent_class == NULL ? "" : parent_class,
                            parent_box == NULL ? 0 : parent_box->x,
                            parent_box == NULL ? 0 : parent_box->y,
                            parent_box == NULL ? 0 : parent_box->width,
                            parent_box == NULL ? 0 : parent_box->height,
                            parent_box != NULL && parent_box->clips_x,
                            parent_box != NULL && parent_box->clips_y);
                }
            }
        }
    }
    budget_free(layout->budget, clip_boxes);
    for (size_t i = 0; i < layout->count; i++) {
        if ((layout->command_flags[i]
             & (LAYOUT_COMMAND_CLIPPED_X | LAYOUT_COMMAND_FIXED)) != 0) {
            continue;
        }
        int64_t right = (int64_t) layout->commands[i].x
                        + layout->commands[i].width;
        if (right > maximum && right <= INT_MAX) {
            maximum = (int) right;
            if (trace && trace_lines++ < 64) {
                fprintf(stderr,
                        "layout-scroll-width command=%zu type=%d "
                        "box=%d,%d,%dx%d right=%d\n",
                        i, layout->commands[i].type,
                        layout->commands[i].x, layout->commands[i].y,
                        layout->commands[i].width,
                        layout->commands[i].height, maximum);
            }
        }
    }
    return maximum;
}


void layout_translate_range(LayoutDocument *layout, size_t command_start,
                            size_t link_start, size_t control_start,
                            size_t node_box_start, int dx, int dy,
                            const char *phase,
                            lxb_dom_node_t *source)
{
    for (size_t i = command_start; i < layout->count; i++) {
        layout->commands[i].x += dx;
        layout->commands[i].y += dy;
    }
    for (size_t i = link_start; i < layout->link_count; i++) {
        layout->links[i].x += dx;
        layout->links[i].y += dy;
    }
    for (size_t i = control_start; i < layout->control_count; i++) {
        layout->controls[i].x += dx;
        layout->controls[i].y += dy;
    }
    for (size_t i = 0; i < layout->node_box_count; i++) {
        LayoutNodeBox *box = &layout->node_boxes[i];
        if (i < node_box_start) continue;
        if (source != NULL && !layout_node_within(box->node, source)) continue;
        if (box->command_start < command_start
            || box->command_end > layout->count) continue;
        const char *trace_class = layout->trace_range_class;
        if (trace_class != NULL && dx != 0) {
            size_t class_length = 0;
            const char *class_name = document_attribute(
                box->node, "class", &class_length);
            if (class_name != NULL && strstr(class_name, trace_class) != NULL) {
                size_t source_class_length = 0;
                const char *source_class = source == NULL ? NULL
                    : document_attribute(source, "class",
                                         &source_class_length);
                fprintf(stderr,
                        "layout-range-translate phase=%s source=%.*s "
                        "affected=%.*s command=%zu/%zu box=%d/%d "
                        "delta=%d/%d\n",
                        phase,
                        (int) source_class_length,
                        source_class == NULL ? "" : source_class,
                        (int) class_length, class_name,
                        command_start, (size_t) box->command_start,
                        box->x, box->width, dx, dy);
            }
        }
        box->x += dx;
        box->y += dy;
    }
}

/* Generated pseudo boxes can be inserted into the middle of an already-built
   command list.  The ordinary subtree helpers intentionally run to the end of
   the document, so expose a bounded command-only span for those inserted
   boxes.  A shallow view reuses the exact scale/rotation arithmetic without
   touching links, controls, or node boxes owned by the originating element. */
void layout_transform_command_span(
    LayoutDocument *layout, size_t command_start, size_t command_end,
    int origin_x_twice, int origin_y_twice, uint8_t scale_q6,
    uint8_t rotate_quadrants, int dx, int dy)
{
    if (layout == NULL || command_start >= command_end
        || command_start >= layout->count) return;
    if (command_end > layout->count) command_end = layout->count;
    LayoutDocument span = {0};
    span.commands = layout->commands + command_start;
    span.count = command_end - command_start;
    layout_scale_range(&span, 0, 0, 0, 0, origin_x_twice,
                       origin_y_twice, scale_q6, NULL);
    layout_rotate_range_quadrants(&span, 0, 0, 0, 0, origin_x_twice,
                                  origin_y_twice, rotate_quadrants, NULL);
    layout_translate_range(&span, 0, 0, 0, 0, dx, dy,
                           "pseudo-transform", NULL);
}

bool apply_visual_range(LayoutContext *context, lxb_dom_node_t *node,
                        size_t command_start, size_t link_start,
                        size_t control_start, const ComputedStyle *style,
                        bool flex_or_grid_item)
{
    LayoutDocument *layout = context->layout;
    const StylePaintStack *paint = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(style));
    unsigned blend_mode = paint == NULL ? STYLE_MIX_BLEND_NORMAL
        : paint->reserved & STYLE_PAINT_MIX_BLEND_MASK;
    bool effective_z_index = style->has_z_index
        && (style->out_of_flow || style->relative_position
            || style->fixed_position || style->sticky_position
            || flex_or_grid_item);
    if (style->opacity < 255) {
        for (size_t i = command_start; i < layout->count; i++) {
            unsigned combined = (unsigned) layout->commands[i].opacity_scale
                                * style->opacity / 255u;
            layout->commands[i].opacity_scale = (uint16_t) combined;
        }
    }
    if (computed_style_filter_code(style) != STYLE_FILTER_NONE) {
        for (size_t i = command_start; i < layout->count; i++) {
            draw_command_set_filter_code(
                &layout->commands[i], computed_style_filter_code(style));
            if (paint != NULL
                && (paint->reserved & STYLE_PAINT_FILTER_LOW_AMOUNT) != 0) {
                layout->commands[i].font_italic |=
                    LAYOUT_COMMAND_FILTER_LOW_AMOUNT;
            }
        }
    }
    if (blend_mode != STYLE_MIX_BLEND_NORMAL) {
        for (size_t i = command_start; i < layout->count; i++) {
            draw_command_set_blend_mode(&layout->commands[i], blend_mode);
        }
    }
    if (effective_z_index) {
        for (size_t i = command_start; i < layout->count; i++) {
            layout->commands[i].z_index = style->z_index;
        }
        for (size_t i = link_start; i < layout->link_count; i++) {
            layout->links[i].z_index = style->z_index;
        }
        for (size_t i = control_start; i < layout->control_count; i++) {
            layout->controls[i].z_index = style->z_index;
        }
    }
    if (style->out_of_flow || style->fixed_position
        || style->relative_position || style->sticky_position
        || effective_z_index) {
        for (size_t i = command_start; i < layout->count; i++) {
            layout->commands[i].font_weight |=
                LAYOUT_COMMAND_POSITIONED_PHASE;
        }
    }
    bool root_context = layout_node_name_is(node, "body")
        && node->parent != NULL && layout_node_name_is(node->parent, "html");
    bool positioned_context = style->fixed_position
        || style->sticky_position
        || style->has_filter
        || computed_style_isolation_isolate(style)
        || effective_z_index;
    if (!root_context && !positioned_context
        && style->opacity == 255 && !style->has_transform
        && !style->has_filter) return true;
    if (command_start > UINT32_MAX || layout->count > UINT32_MAX) return false;
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    size_t decoration_end = box == NULL
        ? command_start : box->scroll_command_start;
    if (decoration_end > layout->count) decoration_end = layout->count;
    for (size_t i = context->stacking_context_count; i != 0; i--) {
        LayoutStackingContext *existing =
            &context->stacking_contexts[i - 1u];
        if (existing->node != node) continue;
        existing->command_start = (uint32_t) command_start;
        existing->command_end = (uint32_t) layout->count;
        existing->decoration_end = (uint32_t) decoration_end;
        existing->z_index = effective_z_index ? style->z_index : 0;
        return true;
    }
    if (context->stacking_context_count
        == context->stacking_context_capacity) {
        size_t capacity = context->stacking_context_capacity == 0
            ? 16 : context->stacking_context_capacity * 2u;
        if (capacity > UINT32_MAX
            || capacity > SIZE_MAX / sizeof(*context->stacking_contexts)) {
            return false;
        }
        LayoutStackingContext *ranges = budget_realloc(
            layout->budget, context->stacking_contexts,
            capacity * sizeof(*ranges));
        if (ranges == NULL) return false;
        context->stacking_contexts = ranges;
        context->stacking_context_capacity = capacity;
    }
    context->stacking_contexts[context->stacking_context_count++] =
        (LayoutStackingContext) {
            .node = node,
            .command_start = (uint32_t) command_start,
            .command_end = (uint32_t) layout->count,
            .decoration_end = (uint32_t) decoration_end,
            .z_index = effective_z_index ? style->z_index : 0
        };
    return true;
}

bool layout_record_visibility_range(
    LayoutContext *context, size_t command_start, size_t link_start,
    size_t control_start, bool hidden)
{
    if (context == NULL || context->layout == NULL
        || command_start > UINT32_MAX || context->layout->count > UINT32_MAX
        || link_start > UINT32_MAX
        || context->layout->link_count > UINT32_MAX
        || control_start > UINT32_MAX
        || context->layout->control_count > UINT32_MAX) {
        return false;
    }
    if (context->visibility_range_count
        == context->visibility_range_capacity) {
        size_t capacity = context->visibility_range_capacity == 0
            ? 8u : context->visibility_range_capacity * 2u;
        if (capacity > SIZE_MAX / sizeof(*context->visibility_ranges)) {
            return false;
        }
        LayoutVisibilityRange *ranges = budget_realloc(
            context->layout->budget, context->visibility_ranges,
            capacity * sizeof(*ranges));
        if (ranges == NULL) return false;
        context->visibility_ranges = ranges;
        context->visibility_range_capacity = capacity;
    }
    context->visibility_ranges[context->visibility_range_count++] =
        (LayoutVisibilityRange) {
            .command_start = (uint32_t) command_start,
            .command_end = (uint32_t) context->layout->count,
            .link_start = (uint32_t) link_start,
            .link_end = (uint32_t) context->layout->link_count,
            .control_start = (uint32_t) control_start,
            .control_end = (uint32_t) context->layout->control_count,
            .hidden = hidden
        };
    return true;
}

static bool resolve_visibility_kind(
    LayoutContext *context, size_t count, unsigned kind)
{
    if (count == 0) return true;
    uint8_t *hidden = budget_calloc(
        context->layout->budget, count, sizeof(*hidden));
    if (hidden == NULL) return false;
    for (size_t reverse = context->visibility_range_count;
         reverse != 0; reverse--) {
        const LayoutVisibilityRange *range =
            &context->visibility_ranges[reverse - 1u];
        size_t start = kind == 0 ? range->command_start
            : (kind == 1 ? range->link_start : range->control_start);
        size_t end = kind == 0 ? range->command_end
            : (kind == 1 ? range->link_end : range->control_end);
        if (end > count) end = count;
        if (start > end) start = end;
        memset(hidden + start, range->hidden ? 1 : 0, end - start);
    }
    for (size_t i = 0; i < count; i++) {
        if (!hidden[i]) continue;
        if (kind == 0) {
            context->layout->commands[i].opacity_scale = 0;
        } else if (kind == 1) {
            context->layout->links[i].width = 0;
            context->layout->links[i].height = 0;
        } else {
            context->layout->controls[i].width = 0;
            context->layout->controls[i].height = 0;
        }
    }
    budget_free(context->layout->budget, hidden);
    return true;
}

bool layout_resolve_visibility(LayoutContext *context)
{
    if (context == NULL || context->layout == NULL
        || context->visibility_range_count == 0) return true;
    return resolve_visibility_kind(context, context->layout->count, 0)
        && resolve_visibility_kind(context, context->layout->link_count, 1)
        && resolve_visibility_kind(
            context, context->layout->control_count, 2);
}
