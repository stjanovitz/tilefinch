/* The replaced-content arm of block layout: an element replaced by an
   image, media or frame, or by CSS content, gets its intrinsic or declared
   box (src/layout_replaced.c), its image or alt text, and advances the
   line past it. Out of line so that an ordinary block neither runs it nor
   reserves its locals in the recursive layout_block_impl frame; the caller
   admits only replaced elements. *replaced_box reports whether the element
   took a replaced box. */

#include "layout_block_internal.h"

__attribute__((noinline))
bool layout_block_replaced_section(LayoutContext *context,
                                   lxb_dom_node_t *node,
                                   const ComputedStyle *parent,
                                   const ComputedStyle *style,
                                   LineState *line, int content_x,
                                   int content_width, int containing_height,
                                   int border_radius_code, bool *replaced_box)
{
    LayoutReplacedInfo replaced;
    layout_replaced_info(context, node, style, &replaced);
    layout_replaced_note_unresolved(context, node, style, &replaced);
    bool canvas_element = replaced.canvas;
    bool audio_controls = replaced.audio_controls;
    const ImageResource *block_image = replaced.image;
    bool image_available = replaced.available;
    int image_width = replaced.natural_width;
    int image_height = replaced.natural_height;
    int intrinsic_image_height = image_height;
    bool fallback_dimensions = replaced.fallback_dimensions;
    bool replaced_image = image_available || canvas_element
                          || audio_controls
                          || node->local_name == LXB_TAG_VIDEO
                          || node->local_name == LXB_TAG_IFRAME
                          || (style->has_width && style->has_height);
    int ratio_width = 0, ratio_height = 0;
    if (!replaced_image
        && layout_unloaded_image_ratio(
               node, style, &ratio_width, &ratio_height)) {
        /* An unloaded <img> keeps the box its ratio and one specified
           dimension define (see layout_unloaded_image_ratio). */
        image_width = ratio_width;
        image_height = ratio_height;
        replaced_image = true;
    }
    if (replaced_image) {
        if (style->has_width && !style->width_max_content) {
            int styled_width = resolve_declared_length(
                context->sheet, style->width, style->width_percent,
                content_width);
            if (styled_width > 0) {
                if (!fallback_dimensions) image_height = style->aspect_width > 0
                               && style->aspect_height > 0
                    ? layout_scale_dimension(
                        style->aspect_height, styled_width,
                        style->aspect_width)
                    : layout_scale_dimension(
                        image_height, styled_width, image_width);
                image_width = styled_width;
            }
        }
        int styled_height = layout_block_style_resolved_height(
            context->sheet, style, image_width, containing_height);
        if (styled_height > 0) {
            if (!style->has_width && image_height > 0 && !fallback_dimensions) {
                image_width = style->aspect_width > 0
                              && style->aspect_height > 0
                    ? layout_scale_dimension(
                        style->aspect_width, styled_height,
                        style->aspect_height)
                    : layout_scale_dimension(
                        image_width, styled_height, image_height);
            }
            image_height = styled_height;
        } else if (!style->has_height && style->aspect_width > 0
                   && style->aspect_height > 0 && image_width > 0) {
            image_height = layout_scale_dimension(
                style->aspect_height, image_width, style->aspect_width);
        }
        if (style->max_height == STYLE_LENGTH_MIN_CONTENT
            && intrinsic_image_height > 0
            && image_height > intrinsic_image_height) {
            image_height = intrinsic_image_height;
        }
        constrain_replaced_content_size(
            context, node, parent, style, content_width, containing_height,
            style->has_width && !style->width_max_content,
            style->has_height, !fallback_dimensions,
            &image_width, &image_height);
        bool definite_replaced_width = style->has_width
            && !style->width_max_content && !style->width_percent;
        if (image_width > content_width && !definite_replaced_width) {
            if (!fallback_dimensions) image_height = layout_scale_dimension(
                image_height, content_width, image_width);
            image_width = content_width;
        }
        if (image_width < 1) image_width = 1;
        if (image_height < 1) image_height = 1;
        bool fixed_image_slot = layout_fixed_image_slot(node, style, parent);
        if (image_available || fixed_image_slot) {
            DrawCommand command = {
                .type = DRAW_IMAGE, .x = content_x, .y = line->y,
                .width = image_width, .height = image_height,
                .image = image_available ? block_image : NULL,
                .image_fit = style->object_fit,
                .text = fixed_image_slot ? (const char *) node : NULL,
                .scale = border_radius_code
            };
            draw_command_set_object_position(
                &command, style->object_position_x,
                style->object_position_y);
            if (layout_add_command(context->layout, command) == NULL) {
                return false;
            }
            if (!image_available && !layout_add_replaced_alt_text(
                    context, node, style, content_x, line->y, image_width, image_height))
                return false;
        } else if (!canvas_element && !layout_add_replaced_alt_text(
                       context, node, style, content_x, line->y,
                       image_width, image_height)) {
            return false;
        }
        line->y = layout_add_coordinate(line->y, image_height);
    }
    *replaced_box = replaced_image;
    return true;
}

/* A box whose CSS mask is an image paints it as a replaced image: the
   mask's image (or, for an empty fixed-size icon, a slot completed when it
   arrives) at the box's declared or intrinsic size. The caller admits only
   boxes with a mask image or layer. */
__attribute__((noinline))
bool layout_block_mask_section(LayoutContext *context, lxb_dom_node_t *node,
                               const ComputedStyle *style, LineState *line,
                               int content_x, int content_width,
                               int containing_height,
                               const LayoutBlockPaintPlan *plan,
                               bool *replaced_mask_out)
{
    const ImageResource *block_mask = plan->element_mask;
    /* Empty fixed-size inline icons have stable flow before download. Keep
       their paint-layer ID until arrival; intrinsic mask positioning can
       then be completed without re-running document layout. */
    const StylePaintLayer *mask_layer = plan->mask_layer;
    bool fixed_mask_slot = node->first_child == NULL
        && style->display == DISPLAY_INLINE_BLOCK
        && mask_layer != NULL && mask_layer->kind == STYLE_PAINT_IMAGE_URL
        && style->has_width && style->has_height
        && !style->width_percent && !style->height_percent && !style->width_max_content
        && style->width > 0 && style->width <= STYLE_LENGTH_DIRECT_LIMIT
        && style->height > 0 && style->height <= STYLE_LENGTH_DIRECT_LIMIT
        && style->min_width >= 0 && style->min_width <= STYLE_LENGTH_DIRECT_LIMIT
        && style->min_height >= 0 && style->min_height <= STYLE_LENGTH_DIRECT_LIMIT
        && style->margin.left == 0 && style->margin.right == 0
        && style->margin.top == 0 && style->margin.bottom == 0
        && style->padding.left == 0 && style->padding.right == 0
        && style->padding.top == 0 && style->padding.bottom == 0
        && style->border.left == 0 && style->border.right == 0
        && style->border.top == 0 && style->border.bottom == 0;
    if (fixed_mask_slot) {
        const StylePaintStack *stack = stylesheet_paint_stack(
            context->sheet, computed_style_paint_stack_id(style));
        fixed_mask_slot = stack != NULL && stack->mask_count == 1;
    }
    bool replaced_mask = image_resource_available(block_mask) || fixed_mask_slot;
    if (replaced_mask) {
        int mask_width = style->has_width && !style->width_max_content
                         ? resolve_declared_length(
                             context->sheet, style->width,
                             style->width_percent, content_width)
                         : image_resource_intrinsic_width(block_mask);
        int mask_height = layout_block_style_resolved_height(
            context->sheet, style, mask_width, containing_height);
        if (mask_height <= 0)
            mask_height = image_resource_intrinsic_height(block_mask);
        int minimum_width = style_minimum_width(
            context->sheet, style, content_width);
        if (mask_width < minimum_width) mask_width = minimum_width;
        int minimum_height = 0;
        if ((!style->min_height_percent || containing_height > 0)
            && resolve_computed_length(
                context->sheet, style->min_height,
                style->min_height_percent, containing_height,
                &minimum_height)
            && mask_height < minimum_height) {
            mask_height = minimum_height;
        }
        DrawCommand command = {.type = DRAW_IMAGE, .x = content_x,
                               .y = line->y, .width = mask_width,
                               .height = mask_height,
                               .color = style->has_background
                                        ? style->background : style->color,
                               .image = image_resource_available(block_mask) ? block_mask : NULL,
                               .text = fixed_mask_slot ? (const char *) node : NULL,
                               .font_family = fixed_mask_slot ? LAYOUT_IMAGE_SLOT_MASK : 0,
                               .scale = plan->border_radius_code,
                               .opacity_scale = alpha_opacity_scale(
                                   style->has_background
                                   ? style->background_alpha
                                   : style->color_alpha)};
        if (fixed_mask_slot && !image_resource_available(block_mask)) {
            command.text_length = computed_style_paint_stack_id(style);
        } else if (plan->mask_layer != NULL) {
            layout_block_size_paint_image_command(
                &command, plan->mask_layer, block_mask,
                mask_width, mask_height);
        }
        layout_mask_background_gradient(context, style, &command);
        if (layout_add_command(context->layout, command) == NULL) return false;
        line->y = layout_add_coordinate(line->y, mask_height);
    }
    *replaced_mask_out = replaced_mask;
    return true;
}

/* An <img> without a replaced box flows its alt text as ordinary text and
   reserves that line. *flowed is set when it did. */
__attribute__((noinline))
bool layout_block_missing_image_alt(LayoutContext *context,
                                    lxb_dom_node_t *node,
                                    const ComputedStyle *style,
                                    LineState *line, bool *flowed)
{
    size_t alt_length = 0;
    const char *alt = document_attribute(node, "alt", &alt_length);
    if (alt != NULL && alt_length != 0) {
        ComputedStyle alt_style = layout_alt_text_flow_style(style);
        if (!flow_text(context, line, alt, alt_length, &alt_style,
                       NULL, 0, NULL)) return false;
        /* This replaced element has no intrinsic/declared box yet.
           Its flowed fallback must reserve its final line before the
           replaced-content tail skips ordinary child-line flushing. */
        layout_flush_line(line);
        line_finish_vertical(line);
        *flowed = true;
    }
    return true;
}
