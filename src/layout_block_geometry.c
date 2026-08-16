/* Block-layout geometry: length resolution, paint-layer sizing, and box
   sizing (heights, min/max widths, replaced-content constraints).
   Split out of layout_block.c. */

#include "layout_block_internal.h"
#include "tilefinch/integer_math.h"

bool resolve_computed_length(const Stylesheet *sheet, int value,
                                    bool percent, int reference,
                                    int *resolved)
{
    if (resolved == NULL) return false;
    /* Integral percentages retain the legacy small integer encoding. Mixed
       length-percentage expressions use a high tagged program reference. */
    if (percent && value > -1000000 && value < 1000000) {
        *resolved = tilefinch_mul_div_int(reference, value, 100);
        return true;
    }
    return style_length_resolve(sheet, value, reference, resolved);
}

int resolve_declared_length(const Stylesheet *sheet, int value,
                                   bool percent, int reference)
{
    int resolved = 0;
    (void) resolve_computed_length(sheet, value, percent, reference,
                                   &resolved);
    /* CSS sizing properties reject negative used values.  Packed mixed
       expressions cannot be clamped during cascade because their sign
       depends on the eventual percentage basis. */
    return resolved < 0 ? 0 : resolved;
}

void resolve_padding(const Stylesheet *sheet, ComputedStyle *style,
                            int containing_inline_width)
{
    if (style == NULL) return;
    int *edges[] = {&style->padding.top, &style->padding.right,
                    &style->padding.bottom, &style->padding.left};
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        int used = 0;
        if (!style_length_resolve(sheet, *edges[i],
                                  containing_inline_width, &used)
            || used < 0) used = 0;
        *edges[i] = used;
    }
}

void resolve_margin(const Stylesheet *sheet, ComputedStyle *style,
                    int containing_inline_width)
{
    if (style == NULL) return;
    int *edges[] = {&style->margin.top, &style->margin.right,
                    &style->margin.bottom, &style->margin.left};
    bool percentages[] = {
        style->margin_top_percent, style->margin_right_percent,
        style->margin_bottom_percent, style->margin_left_percent
    };
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        if (!percentages[i]) continue;
        int used = 0;
        if (!resolve_computed_length(
                sheet, *edges[i], true, containing_inline_width, &used)) {
            used = 0;
        }
        *edges[i] = used;
    }
    style->margin_top_percent = false;
    style->margin_right_percent = false;
    style->margin_bottom_percent = false;
    style->margin_left_percent = false;
}

void layout_block_style_paint_box_insets(const ComputedStyle *style,
                                         StylePaintBox box,
                                         int *left, int *top,
                                         int *right, int *bottom)
{
    *left = *top = *right = *bottom = 0;
    if (style == NULL || box == STYLE_PAINT_BOX_BORDER) return;
    *left = style->border.left;
    *top = style->border.top;
    *right = style->border.right;
    *bottom = style->border.bottom;
    if (box == STYLE_PAINT_BOX_CONTENT) {
        *left += style->padding.left;
        *top += style->padding.top;
        *right += style->padding.right;
        *bottom += style->padding.bottom;
    }
}

void layout_block_set_paint_layer_object_position(
    DrawCommand *command, const StylePaintLayer *layer)
{
    bool x_pixels =
        (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
        || (layer->position_edges
            & STYLE_PAINT_POSITION_X_PIXELS) != 0;
    bool y_pixels =
        (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
        || (layer->position_edges
            & STYLE_PAINT_POSITION_Y_PIXELS) != 0;
    bool from_right =
        (layer->position_edges
         & STYLE_PAINT_POSITION_FROM_RIGHT) != 0;
    bool from_bottom =
        (layer->position_edges
         & STYLE_PAINT_POSITION_FROM_BOTTOM) != 0;
    int x_percent = x_pixels ? (from_right ? 100 : 0)
                             : (from_right
                                ? 100 - layer->position_x
                                : layer->position_x);
    int y_percent = y_pixels ? (from_bottom ? 100 : 0)
                             : (from_bottom
                                ? 100 - layer->position_y
                                : layer->position_y);
    int x_offset = x_pixels
        ? (from_right ? -layer->position_x : layer->position_x) : 0;
    int y_offset = y_pixels
        ? (from_bottom ? -layer->position_y : layer->position_y) : 0;
    draw_command_set_object_position(
        command,
        style_object_position_encode(x_percent, x_offset),
        style_object_position_encode(y_percent, y_offset));
}

void layout_block_size_paint_image_command(
    DrawCommand *command, const StylePaintLayer *layer,
    const ImageResource *image, int area_width, int area_height)
{
    if (command == NULL || layer == NULL || image == NULL
        || area_width <= 0 || area_height <= 0) return;
    command->image_fit = layer->fit;
    if ((layer->flags & STYLE_BACKGROUND_SIZE_EXPLICIT) != 0) {
        bool width_auto = (layer->flags & STYLE_BACKGROUND_WIDTH_AUTO) != 0;
        bool height_auto = (layer->flags & STYLE_BACKGROUND_HEIGHT_AUTO) != 0;
        int width = width_auto ? 0
            : ((layer->flags & STYLE_BACKGROUND_WIDTH_PERCENT) != 0
               ? layout_scale_dimension(area_width, layer->width, 100)
               : layer->width);
        int height = height_auto ? 0
            : ((layer->flags & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
               ? layout_scale_dimension(area_height, layer->height, 100)
               : layer->height);
        int natural_width = image_resource_intrinsic_width(image);
        int natural_height = image_resource_intrinsic_height(image);
        if (width_auto && height_auto) {
            width = natural_width;
            height = natural_height;
        } else if (width_auto && height > 0 && natural_height > 0) {
            width = layout_scale_dimension(height, natural_width,
                                           natural_height);
        } else if (height_auto && width > 0 && natural_width > 0) {
            height = layout_scale_dimension(width, natural_height,
                                            natural_width);
        }
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        bool x_pixels =
            (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
            || (layer->position_edges
                & STYLE_PAINT_POSITION_X_PIXELS) != 0;
        bool y_pixels =
            (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
            || (layer->position_edges
                & STYLE_PAINT_POSITION_Y_PIXELS) != 0;
        int offset_x = x_pixels
            ? layer->position_x
            : (area_width - width) * layer->position_x / 100;
        int offset_y = y_pixels
            ? layer->position_y
            : (area_height - height) * layer->position_y / 100;
        if ((layer->position_edges
             & STYLE_PAINT_POSITION_FROM_RIGHT) != 0) {
            offset_x = area_width - width - offset_x;
        }
        if ((layer->position_edges
             & STYLE_PAINT_POSITION_FROM_BOTTOM) != 0) {
            offset_y = area_height - height - offset_y;
        }
        bool tile_x = (layer->flags & STYLE_BACKGROUND_NO_REPEAT_X) == 0;
        bool tile_y = (layer->flags & STYLE_BACKGROUND_NO_REPEAT_Y) == 0;
        command->image_fit =
            tile_x && tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
            : (tile_x ? LAYOUT_IMAGE_FIT_SPRITE_TILE_X
               : (tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                         : LAYOUT_IMAGE_FIT_SPRITE));
        draw_command_set_image_offset(command, offset_x, offset_y);
        draw_command_set_image_sprite_size(command, width, height);
        return;
    }
    if (layer->fit != 0) {
        layout_block_set_paint_layer_object_position(command, layer);
        return;
    }
    int natural_width = image->source_width > 0
        ? image->source_width : image->width;
    int natural_height = image->source_height > 0
        ? image->source_height : image->height;
    bool x_pixels =
        (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
        || (layer->position_edges
            & STYLE_PAINT_POSITION_X_PIXELS) != 0;
    bool y_pixels =
        (layer->flags & STYLE_BACKGROUND_POSITION_PIXELS) != 0
        || (layer->position_edges
            & STYLE_PAINT_POSITION_Y_PIXELS) != 0;
    int offset_x = x_pixels
        ? layer->position_x
        : (area_width - natural_width) * layer->position_x / 100;
    int offset_y = y_pixels
        ? layer->position_y
        : (area_height - natural_height) * layer->position_y / 100;
    if ((layer->position_edges
         & STYLE_PAINT_POSITION_FROM_RIGHT) != 0) {
        offset_x = area_width - natural_width - offset_x;
    }
    if ((layer->position_edges
         & STYLE_PAINT_POSITION_FROM_BOTTOM) != 0) {
        offset_y = area_height - natural_height - offset_y;
    }
    bool tile_x = (layer->flags & STYLE_BACKGROUND_NO_REPEAT_X) == 0;
    bool tile_y = (layer->flags & STYLE_BACKGROUND_NO_REPEAT_Y) == 0;
    command->image_fit =
        tile_x && tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
        : (tile_x ? LAYOUT_IMAGE_FIT_SPRITE_TILE_X
           : (tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                     : LAYOUT_IMAGE_FIT_SPRITE));
    draw_command_set_image_offset(command, offset_x, offset_y);
}


int style_pixel_height(const Stylesheet *sheet,
                              const ComputedStyle *style, int reference)
{
    if (style == NULL) return 0;
    if (!style->has_height) {
        if (style->aspect_width > 0 && style->aspect_height > 0
            && reference > 0) {
            int height = tilefinch_mul_div_int(
                reference, style->aspect_height, style->aspect_width);
            return height > 1000000 ? 1000000 : height;
        }
        return 0;
    }
    /* Percentage heights require a definite containing-block height. This
       layout pass tracks width but not that vertical constraint, so auto is
       the standards-compatible fallback here. */
    if (style->height_percent) return 0;
    int resolved = 0;
    return style_length_resolve(sheet, style->height, reference, &resolved)
           ? resolved : 0;
}

int layout_block_style_resolved_height(const Stylesheet *sheet,
                                       const ComputedStyle *style,
                                       int width_reference,
                                       int containing_height)
{
    if (style == NULL) return 0;
    if (style->has_height && style->height_percent) {
        if (containing_height <= 0) return 0;
        return resolve_declared_length(sheet, style->height, true,
                                       containing_height);
    }
    return style_pixel_height(sheet, style, width_reference);
}

int style_content_height(const Stylesheet *sheet,
                                const ComputedStyle *style,
                                int width_reference,
                                int containing_height)
{
    int declared = layout_block_style_resolved_height(sheet, style, width_reference,
                                         containing_height);
    int vertical_edges = style->padding.top + style->padding.bottom
                         + style->border.top + style->border.bottom;
    if (style->box_sizing_border_box && declared > 0) {
        declared -= vertical_edges;
        if (declared < 0) declared = 0;
    }
    int maximum_height = 0;
    bool has_maximum = style->max_height != STYLE_LENGTH_NONE
        && style->max_height != STYLE_LENGTH_MIN_CONTENT
        && (!style->max_height_percent || containing_height > 0)
        && resolve_computed_length(sheet, style->max_height,
                                   style->max_height_percent,
                                   containing_height, &maximum_height);
    if (has_maximum) {
        int maximum = style->box_sizing_border_box
                      ? maximum_height - vertical_edges
                      : maximum_height;
        if (maximum < 0) maximum = 0;
        /* max-height constrains an auto height after normal-flow content is
           measured; it does not turn auto into the maximum itself. */
        if (style->has_height && declared > maximum) declared = maximum;
    }
    int minimum_height = 0;
    if (!style->min_height_percent || containing_height > 0) {
        (void) resolve_computed_length(sheet, style->min_height,
                                       style->min_height_percent,
                                       containing_height, &minimum_height);
    }
    int minimum = style->box_sizing_border_box
                  ? minimum_height - vertical_edges : minimum_height;
    if (minimum < 0) minimum = 0;
    if (declared < minimum) declared = minimum;
    return declared;
}

int layout_block_resolve_positioned_inset(const Stylesheet *sheet, int value,
                                          uint8_t percent_mask,
                                          uint8_t edge, int reference)
{
    if ((percent_mask & edge) == 0) return value;
    int resolved = 0;
    return resolve_computed_length(sheet, value, true, reference, &resolved)
           ? resolved : 0;
}

int style_minimum_width(const Stylesheet *sheet,
                               const ComputedStyle *style,
                               int containing_width)
{
    if (style == NULL || style->min_width == STYLE_LENGTH_MIN_CONTENT
        || style->min_width == STYLE_LENGTH_MAX_CONTENT
        || style->min_width == STYLE_LENGTH_FIT_CONTENT) return 0;
    return resolve_declared_length(sheet, style->min_width,
                                   style->min_width_percent,
                                   containing_width);
}

bool style_maximum_width(const Stylesheet *sheet,
                                const ComputedStyle *style,
                                int containing_width, int *maximum)
{
    if (style == NULL || style->max_width == STYLE_LENGTH_NONE
        || style->max_width == STYLE_LENGTH_MIN_CONTENT
        || style->max_width == STYLE_LENGTH_MAX_CONTENT
        || style->max_width == STYLE_LENGTH_FIT_CONTENT
        || !resolve_computed_length(sheet, style->max_width,
                                    style->max_width_percent,
                                    containing_width, maximum)) return false;
    if (*maximum < 0) *maximum = 0;
    return true;
}

void constrain_replaced_content_size(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int containing_width, int containing_height,
    bool width_definite, bool height_definite,
    int *width, int *height)
{
    if (context == NULL || style == NULL || width == NULL || height == NULL
        || *width <= 0 || *height <= 0) return;
    int horizontal_edges = style->padding.left + style->padding.right
                           + style->border.left + style->border.right;
    int vertical_edges = style->padding.top + style->padding.bottom
                         + style->border.top + style->border.bottom;
    int original_width = *width;
    int original_height = *height;
    int border_width = original_width + horizontal_edges;
    border_width = constrain_border_box_width(
        context, node, parent, style, containing_width,
        border_width, NULL);
    int content_width = border_width - horizontal_edges;
    if (content_width < 0) content_width = 0;
    int content_height = original_height;
    if (content_width != original_width && !height_definite
        && original_width > 0) {
        content_height = layout_scale_dimension(
            original_height, content_width, original_width);
    }

    int maximum_height = 0;
    bool has_maximum =
        style->max_height != STYLE_LENGTH_NONE
        && style->max_height != STYLE_LENGTH_MIN_CONTENT
        && (!style->max_height_percent || containing_height > 0)
        && resolve_computed_length(
            context->sheet, style->max_height, style->max_height_percent,
            containing_height, &maximum_height);
    if (has_maximum) {
        int maximum = style->box_sizing_border_box
            ? maximum_height - vertical_edges : maximum_height;
        if (maximum < 0) maximum = 0;
        if (content_height > maximum) content_height = maximum;
    }
    int minimum_height = 0;
    if ((!style->min_height_percent || containing_height > 0)
        && resolve_computed_length(
            context->sheet, style->min_height, style->min_height_percent,
            containing_height, &minimum_height)) {
        int minimum = style->box_sizing_border_box
            ? minimum_height - vertical_edges : minimum_height;
        if (minimum < 0) minimum = 0;
        /* CSS sizing applies the minimum after the maximum. */
        if (content_height < minimum) content_height = minimum;
    }
    if (content_height != original_height && !width_definite
        && original_height > 0) {
        int ratio_width = layout_scale_dimension(
            original_width, content_height, original_height);
        int ratio_border = ratio_width + horizontal_edges;
        ratio_border = constrain_border_box_width(
            context, node, parent, style, containing_width,
            ratio_border, NULL);
        content_width = ratio_border - horizontal_edges;
        if (content_width < 0) content_width = 0;
    }
    *width = content_width;
    *height = content_height;
}

int constrain_border_box_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int containing_width, int candidate, bool *has_maximum)
{
    int edges = style->padding.left + style->padding.right
                + style->border.left + style->border.right;
    int maximum_width = 0;
    bool intrinsic_maximum = context->intrinsic_constraint_depth == 0
        && (style->max_width == STYLE_LENGTH_MIN_CONTENT
        || style->max_width == STYLE_LENGTH_MAX_CONTENT
        || style->max_width == STYLE_LENGTH_FIT_CONTENT);
    bool bounded = intrinsic_maximum || style_maximum_width(
        context->sheet, style, containing_width, &maximum_width);
    if (has_maximum != NULL) *has_maximum = bounded;
    if (bounded) {
        int maximum;
        if (intrinsic_maximum) {
            int measure_limit = style->max_width == STYLE_LENGTH_MAX_CONTENT
                ? LAYOUT_COORDINATE_LIMIT : containing_width;
            context->intrinsic_constraint_depth++;
            maximum = style->max_width == STYLE_LENGTH_MIN_CONTENT
                ? intrinsic_min_text_width_ignoring_own_width(
                      context, node, parent, measure_limit)
                : intrinsic_text_width(
                      context, node, parent, measure_limit);
            context->intrinsic_constraint_depth--;
            maximum -= style->margin.left + style->margin.right;
            if (maximum < 0) maximum = 0;
        } else {
            maximum = style->box_sizing_border_box
                      ? maximum_width : maximum_width + edges;
        }
        if (candidate > maximum) candidate = maximum;
    }
    /* CSS sizing resolves max first and min second, so a conflicting
       minimum wins. Keep that rule identical across formatting contexts. */
    if (!style->min_width_auto) {
        int minimum;
        bool intrinsic_minimum =
            style->min_width == STYLE_LENGTH_MIN_CONTENT
            || style->min_width == STYLE_LENGTH_MAX_CONTENT
            || style->min_width == STYLE_LENGTH_FIT_CONTENT;
        if (intrinsic_minimum && context->intrinsic_constraint_depth == 0) {
            int measure_limit = style->min_width == STYLE_LENGTH_MAX_CONTENT
                ? LAYOUT_COORDINATE_LIMIT : containing_width;
            context->intrinsic_constraint_depth++;
            minimum = style->min_width == STYLE_LENGTH_MIN_CONTENT
                ? intrinsic_min_text_width_ignoring_own_width(
                      context, node, parent, measure_limit)
                : intrinsic_text_width(
                      context, node, parent, measure_limit);
            context->intrinsic_constraint_depth--;
            minimum -= style->margin.left + style->margin.right;
            if (minimum < 0) minimum = 0;
        } else if (!intrinsic_minimum) {
            minimum = style_minimum_width(
                context->sheet, style, containing_width);
        } else {
            minimum = 0;
        }
        /* Intrinsic helpers return the complete border-box contribution
           (after margin removal above); only authored numeric content-box
           minima still need their padding and border added here. */
        if (!intrinsic_minimum && !style->box_sizing_border_box) {
            minimum += edges;
        }
        if (candidate < minimum) candidate = minimum;
    }
    /* An authored border-box size below its own padding and borders leaves
       a zero-sized content box; it cannot make the physical border box
       narrower than those edges. Keep this invariant shared by block,
       flex, grid, and native form-control layout. */
    if (candidate < edges) candidate = edges;
    return candidate > 0 ? candidate : 0;
}
