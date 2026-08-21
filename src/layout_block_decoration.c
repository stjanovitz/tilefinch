/* Block decorations: the paint-command emission phase that runs before a
   block's children are laid out, and the back-patch phase that resizes those
   same commands once the border-box height is known. The two halves
   communicate through LayoutBlockPaintPlan. Split out of layout_block.c. */

#include "layout_block_internal.h"

#include <stdio.h>

#define LAYOUT_BACKDROP_FILTER_COMMAND_LIMIT 4u

static unsigned layout_admit_backdrop_blur(
    LayoutContext *context, unsigned radius)
{
    if (context == NULL || radius == 0u
        || context->backdrop_filter_disabled) return 0u;
    if (context->backdrop_filter_count
          < LAYOUT_BACKDROP_FILTER_COMMAND_LIMIT) {
        context->backdrop_filter_count++;
        return radius;
    }
    /* All-or-nothing degradation avoids retaining one filtered fixed range
       that would still disable the fixed-layer cache while an attacker adds
       arbitrarily many unfiltered siblings. This scan happens once during
       layout, never in the frame loop. */
    for (size_t i = 0; i < context->layout->count; i++)
        draw_command_set_backdrop_blur(&context->layout->commands[i], 0u);
    context->backdrop_filter_count = 0;
    context->backdrop_filter_disabled = true;
    return 0u;
}

/* Emit the decoration commands for one block: outer box shadows, the rounded
   border stroke, the background fill, the background gradient, the background
   image, the overlay gradient, and a gradient mask.  Their heights are not
   known yet -- the command indices recorded in `plan` are back-patched by
   layout_block_patch_decoration() once the content bottom is final. */
bool layout_block_emit_decoration(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int outer_x, int outer_y, int outer_width,
    LayoutBlockPaintPlan *plan)
{
    const StylePaintStack *paint_stack = stylesheet_paint_stack(
        context->sheet, computed_style_paint_stack_id(style));
    unsigned backdrop_blur = paint_stack == NULL ? 0u
        : (paint_stack->reserved & STYLE_PAINT_BACKDROP_BLUR_MASK)
          >> STYLE_PAINT_BACKDROP_BLUR_SHIFT;
    /* Keep backdrop filtering out of ordinary scrolling content.  Fixed and
       sticky page chrome is the high-value mobile use, and bounding it here
       prevents long articles from turning a cosmetic declaration into
       repeated full-page pixel work on the PSP. */
    if (!style->fixed_position && !style->sticky_position) {
        backdrop_blur = 0u;
    }
    int border_radius_code = stylesheet_border_radius_code(
        context->sheet, style);
    const StylePaintLayer *mask_layer =
        paint_stack != NULL && paint_stack->mask_count != 0
        ? &paint_stack->masks[0] : NULL;
    const ImageResource *element_mask = mask_layer != NULL
        && mask_layer->kind == STYLE_PAINT_IMAGE_URL
        ? images_find_mask_source(context->images, node,
                                  mask_layer->image, PSEUDO_NONE)
        : images_find_mask_node(context->images, node);
    bool element_mask_declared = mask_layer != NULL
        && mask_layer->kind != STYLE_PAINT_IMAGE_NONE;
    bool layered_background = paint_stack != NULL
        && (paint_stack->components
            & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) != 0;
    const ImageResource *element_background = layered_background
        ? NULL
        : (style->background_image != NULL
           ? images_find_background_source(
               context->images, node, style->background_image, PSEUDO_NONE)
           : images_find_background_node(context->images, node));
    if (element_mask_declared
        && !image_resource_available(element_mask)) {
        if (mask_layer->kind == STYLE_PAINT_IMAGE_URL) {
            layout_note_unresolved_external_visual(
                context, node, mask_layer->image, IMAGE_PRIORITY_KIND_MASK,
                PSEUDO_NONE);
        }
    }
    if (layered_background) {
        for (size_t i = 0; i < paint_stack->background_count; i++) {
            const StylePaintLayer *layer = &paint_stack->backgrounds[i];
            if (layer->kind != STYLE_PAINT_IMAGE_URL) continue;
            const ImageResource *resource = images_find_background_source(
                context->images, node, layer->image, PSEUDO_NONE);
            if (!image_resource_available(resource)) {
                layout_note_unresolved_external_visual(
                    context, node, layer->image,
                    IMAGE_PRIORITY_KIND_BACKGROUND, PSEUDO_NONE);
            }
        }
    } else if (style->background_image_kind == STYLE_BACKGROUND_IMAGE_URL
        && !image_resource_available(element_background)) {
        layout_note_unresolved_external_visual(
            context, node, style->background_image,
            IMAGE_PRIORITY_KIND_BACKGROUND, PSEUDO_NONE);
    }
    size_t background_index = (size_t) -1;
    size_t background_gradient_index = (size_t) -1;
    size_t background_image_index = (size_t) -1;
    size_t background_overlay_index = (size_t) -1;
    size_t mask_gradient_index = (size_t) -1;
    size_t rounded_border_index = (size_t) -1;
    size_t background_layer_indices[STYLE_PAINT_LAYER_LIMIT] = {
        (size_t) -1, (size_t) -1, (size_t) -1
    };
    const ImageResource *background_layer_resources[
        STYLE_PAINT_LAYER_LIMIT] = {0};
    uint8_t background_layer_stack_indices[STYLE_PAINT_LAYER_LIMIT] = {0};
    uint8_t background_layer_count = 0;
    DrawCommand rounded_border_command = {0};
    bool rounded_border_pending = false;
    const StylePaintLayer *background_geometry =
        paint_stack != NULL && paint_stack->background_count != 0
        ? &paint_stack->backgrounds[
              layered_background ? paint_stack->background_count - 1u : 0u]
        : NULL;
    StylePaintBox background_clip = background_geometry != NULL
        && (paint_stack->components
            & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0
        ? (StylePaintBox) background_geometry->clip
        : STYLE_PAINT_BOX_BORDER;
    int background_inset_left = 0, background_inset_top = 0;
    int background_inset_right = 0, background_inset_bottom = 0;
    layout_block_style_paint_box_insets(
        style, background_clip, &background_inset_left,
        &background_inset_top, &background_inset_right,
        &background_inset_bottom);
    int background_radius = style_border_radius_adjust(
        border_radius_code, -background_inset_left);
    /* Outer `box-shadow` layers paint beneath the border and background, in
       reverse declaration order so the first-declared layer ends up on top
       exactly as CSS specifies.  Each layer is one DRAW_SHADOW_RECT whose
       box is the border box offset, inflated by the spread and then by the
       blur radius; the rasteriser derives the shadow rect back from that.
       The layer count and every geometry component are hard-clamped at parse
       time (STYLE_BOX_SHADOW_* in style.h), so this loop is bounded and adds
       at most four commands per element.

       APPROXIMATION: CSS clips an outer shadow to outside the border box.
       This paints the full silhouette and lets the background cover it,
       which is identical for any opaque background and only differs where a
       translucent box sits over its own shadow.  Clipping it would cost a
       second rounded-rect test per shadow pixel for that case alone.

       DESCOPED: inline-level boxes.  Inline backgrounds are emitted from
       four separate sites in layout_inline.c, half of them through
       layout_insert_command with a caller-owned insertion index, so shadowing
       them is a wider change than this one and no fidelity scenario can
       witness whether it helped (see the probe note below).  Block boxes are
       where the shadowed cards and panels live.

       DESCOPED: `inset` shadows parse and are retained, but do not paint.
       They need a clip-to-inside-the-border-box pass the span rasteriser has
       no path for, and painting one as an outer shadow would be worse than
       painting nothing.  The parse is kept so an inset layer does not make
       the whole declaration invalid and resurrect an older shadow.

       PROBE NOTE: no fidelity scenario moves under this feature.  That was
       verified, not assumed: forcing every parsed shadow to opaque magenta
       with a 24px blur left all six metrics on all eleven scoreboard rows
       identical to four decimal places.  The corpus does declare box-shadow,
       but never on a block box visible at a checkpoint.  fixtures/
       paint-features.html is therefore the only regression coverage. */
    size_t shadow_index_start = context->layout->count;
    size_t shadow_command_count = 0;
    size_t box_shadow_count = stylesheet_box_shadow_count(
        context->sheet, style);
    for (size_t i = box_shadow_count; i-- > 0;) {
        const StyleBoxShadow *shadow = stylesheet_box_shadow(
            context->sheet, style, i);
        if (shadow == NULL) continue;
        if (style_box_shadow_is_inset(shadow)) continue;
        uint32_t color = shadow->argb & 0x00ffffffu;
        uint8_t alpha = (uint8_t) ((shadow->argb >> 24) & 0xffu);
        if (style_box_shadow_uses_current_color(shadow)) {
            color = style->color;
            alpha = style->color_alpha;
        }
        if (alpha == 0) continue;
        int blur = style_box_shadow_blur(shadow);
        int spread = shadow->spread;
        int inflate = spread + blur;
        int width = outer_width + 2 * inflate;
        if (width <= 0) continue;
        /* The analytic shadow distance field is uniform-radius. Preserve the
           largest authored corner so a nonuniform box never leaks shadow
           through its silhouette; fills, images, borders, and masks retain
           the exact four-corner code. */
        int radius = style_border_radius_maximum(
            style_border_radius_adjust(border_radius_code, spread));
        DrawCommand layer = {
            .type = DRAW_SHADOW_RECT,
            .x = outer_x + shadow->offset_x - inflate,
            .y = outer_y + shadow->offset_y - inflate,
            .width = width,
            .height = 0,
            .color = color,
            .scale = blur,
            .radius = radius,
            .opacity_scale = alpha_opacity_scale(alpha)
        };
        if (layout_add_command(context->layout, layer) == NULL) return false;
        shadow_command_count++;
    }
    /* A rounded border is retained as one hollow stroke.  The old outer-fill
       plus inner-background approximation became a solid pill whenever the
       element's background was transparent.  Unequal/partial borders keep
       using the bounded four-side fallback below. */
    uint32_t border_colors[STYLE_BORDER_SIDE_COUNT] = {0};
    uint8_t border_alphas[STYLE_BORDER_SIDE_COUNT] = {0};
    for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
        border_colors[side] = stylesheet_border_color(
            context->sheet, style, (StyleBorderSide) side,
            &border_alphas[side]);
    }
    bool uniform_border_visual =
        border_colors[STYLE_BORDER_TOP] == border_colors[STYLE_BORDER_RIGHT]
        && border_colors[STYLE_BORDER_TOP]
           == border_colors[STYLE_BORDER_BOTTOM]
        && border_colors[STYLE_BORDER_TOP] == border_colors[STYLE_BORDER_LEFT]
        && border_alphas[STYLE_BORDER_TOP] == border_alphas[STYLE_BORDER_RIGHT]
        && border_alphas[STYLE_BORDER_TOP]
           == border_alphas[STYLE_BORDER_BOTTOM]
        && border_alphas[STYLE_BORDER_TOP] == border_alphas[STYLE_BORDER_LEFT]
        && computed_style_border_line(style, STYLE_BORDER_TOP)
           == computed_style_border_line(style, STYLE_BORDER_RIGHT)
        && computed_style_border_line(style, STYLE_BORDER_TOP)
           == computed_style_border_line(style, STYLE_BORDER_BOTTOM)
        && computed_style_border_line(style, STYLE_BORDER_TOP)
           == computed_style_border_line(style, STYLE_BORDER_LEFT);
    bool rounded_border = style_border_radius_maximum(border_radius_code) > 0
                          && uniform_border_visual
                          && computed_style_border_line(
                                 style, STYLE_BORDER_TOP)
                             != STYLE_BORDER_NONE
                          && style->border.top > 0
                          && style->border.top == style->border.right
                          && style->border.top == style->border.bottom
                          && style->border.top == style->border.left;
    /* Backgrounds nominally extend beneath a border-box clip.  For a fully
       opaque uniform rounded border those covered pixels are unobservable,
       and clipping the background at the inner edge is the compositing-
       equivalent form.  It is also the only form that rasterizes the two
       antialiased curves independently without letting the background's
       corner colour bleed through the partially covered outer stroke.
       Preserve literal border-box painting for translucent or nonuniform
       borders, where content beneath the border is observable. */
    if (rounded_border
        && border_alphas[STYLE_BORDER_TOP] == 255
        && background_clip == STYLE_PAINT_BOX_BORDER) {
        background_inset_left = style->border.left;
        background_inset_top = style->border.top;
        background_inset_right = style->border.right;
        background_inset_bottom = style->border.bottom;
        background_radius = style_border_radius_adjust(
            border_radius_code, -style->border.left);
    }
    if (rounded_border) {
        rounded_border_command = (DrawCommand) {
            .type = DRAW_STROKE_RECT, .x = outer_x,
            .y = outer_y, .width = outer_width, .height = 0,
            .color = border_colors[STYLE_BORDER_TOP],
            .scale = style->border.left,
            .radius = border_radius_code,
            .opacity_scale = alpha_opacity_scale(
                border_alphas[STYLE_BORDER_TOP])
        };
        rounded_border_pending = true;
    }
    /* A declared mask whose pixels have not arrived yet is transparent
       black, not an unmasked fallback. Keeping the box in layout prevents
       geometry shifts while suppressing the solid icon-colored rectangles
       that would otherwise flash during a streaming first paint. */
    if (!element_mask_declared)
        backdrop_blur = layout_admit_backdrop_blur(context, backdrop_blur);
    if ((style->has_background || backdrop_blur != 0u)
        && !element_mask_declared) {
        DrawCommand background = {.type = DRAW_FILL_RECT,
                                  .x = outer_x + background_inset_left,
                                  .y = outer_y + background_inset_top,
                                  .width = outer_width
                                      - background_inset_left
                                      - background_inset_right,
                                  .height = 0, .color = style->background,
                                  .scale = 1, .radius = background_radius,
                                  .opacity_scale = style->has_background
                                      ? alpha_opacity_scale(
                                            style->background_alpha)
                                      : 0};
        draw_command_set_backdrop_blur(&background, backdrop_blur);
        if (LAYOUT_TRACE(context->layout, PAINT)) {
            size_t class_length = 0;
            const char *class_name = document_attribute(
                node, "class", &class_length);
            fprintf(stderr,
                    "layout-paint-background class=%.*s box=%d,%d,%dx%d "
                    "clip=%u insets=%d/%d/%d/%d radius=%d "
                    "color=%06x alpha=%u scale=%u\n",
                    (int) class_length,
                    class_name == NULL ? "" : class_name,
                    background.x, background.y, background.width,
                    background.height, (unsigned) background_clip,
                    background_inset_top, background_inset_right,
                    background_inset_bottom, background_inset_left,
                    background.radius, (unsigned) background.color,
                    (unsigned) style->background_alpha,
                    (unsigned) background.opacity_scale);
        }
        background_index = context->layout->count;
        if (layout_add_command(context->layout, background) == NULL) return false;
    }
    /* Paint retained multi-layer backgrounds from back to front. CSS lists
       the topmost layer first, so reverse declaration order is the display-
       list order. Each command keeps its stack index for final-height
       geometry patching below. */
    if (layered_background && !element_mask_declared) {
        for (size_t i = paint_stack->background_count; i-- > 0;) {
            const StylePaintLayer *layer = &paint_stack->backgrounds[i];
            StylePaintBox clip = (paint_stack->components
                                  & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0
                ? (StylePaintBox) layer->clip : STYLE_PAINT_BOX_BORDER;
            int left = 0, top = 0, right = 0, bottom = 0;
            layout_block_style_paint_box_insets(
                style, clip, &left, &top, &right, &bottom);
            DrawCommand command = {
                .x = outer_x + left,
                .y = outer_y + top,
                .width = outer_width - left - right,
                .height = 0,
                .scale = style_border_radius_adjust(
                    border_radius_code, -left),
                .opacity_scale = alpha_opacity_scale(255)
            };
            const ImageResource *resource = NULL;
            if (layer->kind == STYLE_PAINT_IMAGE_GRADIENT
                && layer->gradient.stop_count >= 2) {
                size_t gradient_slot = 0;
                if (!layout_intern_gradient(
                        context->layout, &layer->gradient,
                        &gradient_slot)) continue;
                command.type = DRAW_FILL_RECT;
                draw_command_set_fill_gradient(&command, gradient_slot);
            } else if (layer->kind == STYLE_PAINT_IMAGE_URL) {
                resource = images_find_background_source(
                    context->images, node, layer->image, PSEUDO_NONE);
                if (!image_resource_available(resource)) continue;
                command.type = DRAW_IMAGE;
                command.image = resource;
                command.image_fit = layer->fit;
            } else {
                continue;
            }
            if (command.width <= 0) continue;
            if (background_layer_count >= STYLE_PAINT_LAYER_LIMIT) break;
            background_layer_indices[background_layer_count] =
                context->layout->count;
            background_layer_resources[background_layer_count] = resource;
            background_layer_stack_indices[background_layer_count] =
                (uint8_t) i;
            background_layer_count++;
            if (layout_add_command(context->layout, command) == NULL) {
                return false;
            }
        }
    }
    /* The gradient arm of the background-image union paints as its own fill
       rect layered over any background colour, mirroring CSS's image-over-
       colour order.  Reusing DRAW_FILL_RECT means border-radius, clipping,
       opacity and paint order all compose exactly as they do for a solid
       background. */
    const StyleGradient *primary_gradient =
        stylesheet_background_gradient(context->sheet, style);
    if (!layered_background
        && style->background_image_kind == STYLE_BACKGROUND_IMAGE_GRADIENT
        && primary_gradient != NULL
        && !element_mask_declared) {
        size_t gradient_slot = 0;
        if (layout_intern_gradient(context->layout,
                                   primary_gradient,
                                   &gradient_slot)) {
            DrawCommand gradient = {.type = DRAW_FILL_RECT,
                                    .x = outer_x + background_inset_left,
                                    .y = outer_y + background_inset_top,
                                    .width = outer_width
                                        - background_inset_left
                                        - background_inset_right,
                                    .height = 0,
                                    .color = style->background,
                                    .scale = 1,
                                    .radius = background_radius,
                                    .opacity_scale =
                                        alpha_opacity_scale(255)};
            draw_command_set_fill_gradient(&gradient, gradient_slot);
            background_gradient_index = context->layout->count;
            if (layout_add_command(context->layout, gradient) == NULL) {
                return false;
            }
        }
    }
    if (!layered_background
        && image_resource_available(element_background)
        && !element_mask_declared) {
        DrawCommand background_image = {
            .type = DRAW_IMAGE,
            .x = outer_x + background_inset_left,
            .y = outer_y + background_inset_top,
            .width = outer_width
                - background_inset_left - background_inset_right,
            .height = 0,
            .image = element_background,
            .image_fit = style->background_fit,
            .scale = background_radius
        };
        background_image_index = context->layout->count;
        if (layout_add_command(context->layout, background_image) == NULL) return false;
    }
    const StyleGradient *overlay =
        stylesheet_background_overlay_gradient(
            context->sheet,
            computed_style_background_overlay_gradient(style));
    if (!layered_background && overlay != NULL && !element_mask_declared) {
        size_t gradient_slot = 0;
        if (layout_intern_gradient(context->layout, overlay,
                                   &gradient_slot)) {
            DrawCommand gradient = {
                .type = DRAW_FILL_RECT,
                .x = outer_x + background_inset_left,
                .y = outer_y + background_inset_top,
                .width = outer_width
                    - background_inset_left - background_inset_right,
                .height = 0,
                .radius = background_radius,
                .opacity_scale = 256
            };
            draw_command_set_fill_gradient(&gradient, gradient_slot);
            background_overlay_index = context->layout->count;
            if (layout_add_command(context->layout, gradient) == NULL) {
                return false;
            }
        }
    }
    if (mask_layer != NULL
        && mask_layer->kind == STYLE_PAINT_IMAGE_GRADIENT
        && mask_layer->gradient.stop_count >= 2) {
        size_t gradient_slot = 0;
        if (layout_intern_gradient(context->layout, &mask_layer->gradient,
                                   &gradient_slot)) {
            DrawCommand mask = {
                .type = DRAW_FILL_RECT,
                .x = outer_x,
                .y = outer_y,
                .width = outer_width,
                .height = 0,
                .color = style->has_background
                    ? style->background : style->color,
                .radius = border_radius_code,
                .opacity_scale = alpha_opacity_scale(
                    style->has_background
                    ? style->background_alpha : style->color_alpha),
                .image_fit = LAYOUT_FILL_GRADIENT_AS_MASK
            };
            draw_command_set_fill_gradient(&mask, gradient_slot);
            mask_gradient_index = context->layout->count;
            if (layout_add_command(context->layout, mask) == NULL) {
                return false;
            }
        }
    }
    /* Borders paint above every background layer.  Deferring this one
       command also keeps rounded and square borders in the same CSS paint
       order; adding the rounded stroke before the background let an image
       overwrite it and expose pixels outside the padding curve. */
    if (rounded_border_pending) {
        rounded_border_index = context->layout->count;
        if (layout_add_command(
                context->layout, rounded_border_command) == NULL) {
            return false;
        }
    }
    plan->paint_stack = paint_stack;
    plan->mask_layer = mask_layer;
    plan->background_geometry = background_geometry;
    plan->element_mask = element_mask;
    plan->element_background = element_background;
    plan->background_index = background_index;
    plan->background_gradient_index = background_gradient_index;
    plan->background_image_index = background_image_index;
    plan->background_overlay_index = background_overlay_index;
    plan->mask_gradient_index = mask_gradient_index;
    plan->rounded_border_index = rounded_border_index;
    plan->shadow_index_start = shadow_index_start;
    plan->shadow_command_count = shadow_command_count;
    plan->background_layer_count = background_layer_count;
    for (size_t i = 0; i < STYLE_PAINT_LAYER_LIMIT; i++) {
        plan->background_layer_indices[i] = background_layer_indices[i];
        plan->background_layer_resources[i] =
            background_layer_resources[i];
        plan->background_layer_stack_indices[i] =
            background_layer_stack_indices[i];
    }
    plan->border_radius_code = border_radius_code;
    plan->background_inset_left = background_inset_left;
    plan->background_inset_top = background_inset_top;
    plan->background_inset_right = background_inset_right;
    plan->background_inset_bottom = background_inset_bottom;
    for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
        plan->border_colors[side] = border_colors[side];
        plan->border_alphas[side] = border_alphas[side];
    }
    plan->rounded_border = rounded_border;
    return true;
}

/* Resize the decoration commands recorded in `plan` now that the border-box
   height is final, then insert the square border strokes ahead of the block's
   own content so CSS paint order is preserved.  Both insertion cursors are
   advanced by the number of border commands inserted. */
bool layout_block_patch_decoration(
    LayoutContext *context, const ComputedStyle *style,
    const LayoutBlockPaintPlan *plan, int outer_x, int outer_y,
    int outer_width, int content_bottom, int border_height,
    bool collapsed_table_cell, size_t *insertion_index_io,
    size_t *scroll_command_start_io)
{
    const StylePaintStack *paint_stack = plan->paint_stack;
    const StylePaintLayer *background_geometry = plan->background_geometry;
    const ImageResource *element_background = plan->element_background;
    size_t background_index = plan->background_index;
    size_t background_gradient_index = plan->background_gradient_index;
    size_t background_image_index = plan->background_image_index;
    size_t background_overlay_index = plan->background_overlay_index;
    size_t mask_gradient_index = plan->mask_gradient_index;
    size_t rounded_border_index = plan->rounded_border_index;
    size_t shadow_index_start = plan->shadow_index_start;
    size_t shadow_command_count = plan->shadow_command_count;
    uint8_t background_layer_count = plan->background_layer_count;
    int background_inset_left = plan->background_inset_left;
    int background_inset_top = plan->background_inset_top;
    int background_inset_right = plan->background_inset_right;
    int background_inset_bottom = plan->background_inset_bottom;
    const uint32_t *border_colors = plan->border_colors;
    const uint8_t *border_alphas = plan->border_alphas;
    bool rounded_border = plan->rounded_border;
    size_t before_insertion_index = *insertion_index_io;
    size_t scroll_command_start = *scroll_command_start_io;
    /* The border-box height is only known now.  Each shadow layer inflated
       the border box by the same amount on all four sides, so the vertical
       inflation is recoverable from the horizontal one without retaining any
       per-layer bookkeeping. */
    for (size_t i = 0; i < shadow_command_count; i++) {
        DrawCommand *layer = &context->layout->commands[shadow_index_start + i];
        layer->height = layout_add_coordinate(
            content_bottom - outer_y, layer->width - outer_width);
    }
    if (background_index != (size_t) -1) {
        DrawCommand *background =
            &context->layout->commands[background_index];
        background->height =
            content_bottom - outer_y - background_inset_top
            - background_inset_bottom;
        if (background->height <= 0) {
            background->width = 0;
            background->opacity_scale = 0;
        }
    }
    if (background_gradient_index != (size_t) -1) {
        DrawCommand *gradient =
            &context->layout->commands[background_gradient_index];
        gradient->height =
            content_bottom - outer_y - background_inset_top
            - background_inset_bottom;
        if (gradient->height <= 0) {
            gradient->width = 0;
            gradient->opacity_scale = 0;
        }
    }
    if (background_overlay_index != (size_t) -1) {
        DrawCommand *gradient =
            &context->layout->commands[background_overlay_index];
        gradient->height =
            content_bottom - outer_y - background_inset_top
            - background_inset_bottom;
        if (gradient->height <= 0) {
            gradient->width = 0;
            gradient->opacity_scale = 0;
        }
    }
    if (mask_gradient_index != (size_t) -1) {
        DrawCommand *mask =
            &context->layout->commands[mask_gradient_index];
        mask->height = content_bottom - outer_y;
        if (mask->height <= 0) {
            mask->width = 0;
            mask->opacity_scale = 0;
        }
    }
    for (size_t i = 0; i < background_layer_count; i++) {
        size_t command_index = plan->background_layer_indices[i];
        size_t stack_index = plan->background_layer_stack_indices[i];
        if (command_index == (size_t) -1 || paint_stack == NULL
            || stack_index >= paint_stack->background_count) continue;
        const StylePaintLayer *layer =
            &paint_stack->backgrounds[stack_index];
        DrawCommand *command = &context->layout->commands[command_index];
        StylePaintBox clip = (paint_stack->components
                              & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0
            ? (StylePaintBox) layer->clip : STYLE_PAINT_BOX_BORDER;
        int clip_left = 0, clip_top = 0, clip_right = 0, clip_bottom = 0;
        layout_block_style_paint_box_insets(
            style, clip, &clip_left, &clip_top,
            &clip_right, &clip_bottom);
        int area_width = outer_width - clip_left - clip_right;
        int area_height = content_bottom - outer_y
                          - clip_top - clip_bottom;
        command->x = outer_x + clip_left;
        command->y = outer_y + clip_top;
        command->width = area_width;
        command->height = area_height;
        command->scale = style_border_radius_adjust(
            plan->border_radius_code, -clip_left);
        if (area_width <= 0 || area_height <= 0) {
            command->width = 0;
            command->opacity_scale = 0;
            continue;
        }
        const ImageResource *resource =
            plan->background_layer_resources[i];
        if (LAYOUT_TRACE(context->layout, PAINT)
            && context->trace_paint_lines++ < 64) {
            fprintf(stderr,
                    "layout-paint-layer stack=%zu kind=%u box=%d,%d,%dx%d "
                    "fit=%u flags=%u image=%dx%d source=%s\n",
                    stack_index, (unsigned) layer->kind,
                    command->x, command->y,
                    command->width, command->height,
                    (unsigned) layer->fit, (unsigned) layer->flags,
                    resource == NULL ? 0 : resource->source_width,
                    resource == NULL ? 0 : resource->source_height,
                    layer->image == NULL ? "" : layer->image);
        }
        if (command->type == DRAW_IMAGE
            && image_resource_available(resource)) {
            StylePaintBox origin = (paint_stack->components
                                    & STYLE_PAINT_COMPONENT_BACKGROUND_BOX)
                != 0 ? (StylePaintBox) layer->origin
                     : STYLE_PAINT_BOX_PADDING;
            int origin_left = 0, origin_top = 0;
            int origin_right = 0, origin_bottom = 0;
            layout_block_style_paint_box_insets(
                style, origin, &origin_left, &origin_top,
                &origin_right, &origin_bottom);
            command->x += origin_left - clip_left;
            command->y += origin_top - clip_top;
            layout_block_size_paint_image_command(
                command, layer, resource,
                outer_width - origin_left - origin_right,
                content_bottom - outer_y - origin_top - origin_bottom);
        }
    }
    if (background_image_index != (size_t) -1) {
        DrawCommand *background_image =
            &context->layout->commands[background_image_index];
        int area_width = outer_width - background_inset_left
                         - background_inset_right;
        int area_height = content_bottom - outer_y - background_inset_top
                          - background_inset_bottom;
        StylePaintBox origin_box = background_geometry != NULL
            && (paint_stack->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0
            ? (StylePaintBox) background_geometry->origin
            : STYLE_PAINT_BOX_PADDING;
        int origin_left = 0, origin_top = 0;
        int origin_right = 0, origin_bottom = 0;
        layout_block_style_paint_box_insets(
            style, origin_box, &origin_left, &origin_top,
            &origin_right, &origin_bottom);
        int origin_width = outer_width - origin_left - origin_right;
        int origin_height = content_bottom - outer_y - origin_top
                            - origin_bottom;
        int origin_delta_x = origin_left - background_inset_left;
        int origin_delta_y = origin_top - background_inset_top;
        if (origin_width < 0) origin_width = 0;
        if (origin_height < 0) origin_height = 0;
        background_image->height = area_height;
        if (area_height <= 0) {
            background_image->width = 0;
            background_image->opacity_scale = 0;
        }
        if (background_geometry != NULL
            && background_geometry->fit != 0) {
            background_image->image_fit = background_geometry->fit;
            layout_block_set_paint_layer_object_position(
                background_image, background_geometry);
        }
        if ((style->background_size_flags
             & STYLE_BACKGROUND_SIZE_EXPLICIT) != 0) {
            bool width_auto = (style->background_size_flags
                               & STYLE_BACKGROUND_WIDTH_AUTO) != 0;
            bool height_auto = (style->background_size_flags
                                & STYLE_BACKGROUND_HEIGHT_AUTO) != 0;
            int image_width = width_auto ? 0
                : ((style->background_size_flags
                    & STYLE_BACKGROUND_WIDTH_PERCENT) != 0
                   ? layout_scale_dimension(
                       area_width, style->background_width, 100)
                   : style->background_width);
            int image_height = height_auto ? 0
                : ((style->background_size_flags
                    & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
                   ? layout_scale_dimension(
                       area_height, style->background_height, 100)
                   : style->background_height);
            if (width_auto && height_auto) {
                image_width = element_background->width;
                image_height = element_background->height;
            } else if (width_auto && image_height > 0
                       && element_background->height > 0) {
                image_width = layout_scale_dimension(
                    image_height, element_background->width,
                    element_background->height);
            } else if (height_auto && image_width > 0
                       && element_background->width > 0) {
                image_height = layout_scale_dimension(
                    image_width, element_background->height,
                    element_background->width);
            }
            if (image_width < 1) image_width = 1;
            if (image_height < 1) image_height = 1;
            bool pixels = (style->background_size_flags
                           & STYLE_BACKGROUND_POSITION_PIXELS) != 0;
            int offset_x = origin_delta_x
                + (pixels ? style->background_position_x
                   : (origin_width - image_width)
                     * style->background_position_x / 100);
            int offset_y = origin_delta_y
                + (pixels ? style->background_position_y
                   : (origin_height - image_height)
                     * style->background_position_y / 100);
            bool tile_x = (style->background_size_flags
                           & STYLE_BACKGROUND_NO_REPEAT_X) == 0;
            bool tile_y = (style->background_size_flags
                           & STYLE_BACKGROUND_NO_REPEAT_Y) == 0;
            background_image->image_fit =
                tile_x && tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
                : (tile_x ? LAYOUT_IMAGE_FIT_SPRITE_TILE_X
                   : (tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                             : LAYOUT_IMAGE_FIT_SPRITE));
            draw_command_set_image_offset(background_image,
                                          offset_x, offset_y);
            draw_command_set_image_sprite_size(background_image,
                                               image_width, image_height);
        } else if (style->background_fit == 0) {
            /* CSS initial sizing: paint at the image's natural size,
               positioned by background-position and tiled per
               background-repeat, clipped by the paint area.  Pixel
               positions are sprite-sheet crops; percent positions align
               natural size against the area. */
            bool pixels = (style->background_size_flags
                           & STYLE_BACKGROUND_POSITION_PIXELS) != 0;
            int natural_width = element_background->source_width > 0
                                ? element_background->source_width
                                : element_background->width;
            int natural_height = element_background->source_height > 0
                                 ? element_background->source_height
                                 : element_background->height;
            int offset_x = pixels ? style->background_position_x
                : (origin_width - natural_width)
                  * style->background_position_x / 100;
            int offset_y = pixels ? style->background_position_y
                : (origin_height - natural_height)
                  * style->background_position_y / 100;
            offset_x += origin_delta_x;
            offset_y += origin_delta_y;
            bool tile_x = (style->background_size_flags
                           & STYLE_BACKGROUND_NO_REPEAT_X) == 0;
            bool tile_y = (style->background_size_flags
                           & STYLE_BACKGROUND_NO_REPEAT_Y) == 0;
            background_image->image_fit =
                tile_x && tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_XY
                : (tile_x ? LAYOUT_IMAGE_FIT_SPRITE_TILE_X
                   : (tile_y ? LAYOUT_IMAGE_FIT_SPRITE_TILE_Y
                             : LAYOUT_IMAGE_FIT_SPRITE));
            draw_command_set_image_offset(background_image,
                                          offset_x, offset_y);
        }
    }
    if (rounded_border_index != (size_t) -1) {
        context->layout->commands[rounded_border_index].height =
            content_bottom - outer_y;
    }
    DrawCommand square_borders[4];
    size_t square_border_count = 0;
    if (!rounded_border && style->border.top > 0
        && computed_style_border_line(
               style, STYLE_BORDER_TOP) != STYLE_BORDER_NONE) {
        int collapsed_left_outset =
            collapsed_table_cell ? style->border.left / 2 : 0;
        int collapsed_right_outset =
            collapsed_table_cell
                ? style->border.right - style->border.right / 2 : 0;
        square_borders[square_border_count++] = (DrawCommand) {
            .type = DRAW_FILL_RECT,
            .x = outer_x - collapsed_left_outset,
            .y = outer_y - (collapsed_table_cell
                            ? style->border.top / 2 : 0),
            .width = outer_width + collapsed_left_outset
                     + collapsed_right_outset,
            .height = style->border.top,
            .color = border_colors[STYLE_BORDER_TOP],
            .opacity_scale = alpha_opacity_scale(
                border_alphas[STYLE_BORDER_TOP])
        };
    }
    if (!rounded_border && style->border.bottom > 0
        && computed_style_border_line(
               style, STYLE_BORDER_BOTTOM) != STYLE_BORDER_NONE) {
        int collapsed_left_outset =
            collapsed_table_cell ? style->border.left / 2 : 0;
        int collapsed_right_outset =
            collapsed_table_cell
                ? style->border.right - style->border.right / 2 : 0;
        square_borders[square_border_count++] = (DrawCommand) {
            .type = DRAW_FILL_RECT,
            .x = outer_x - collapsed_left_outset,
            .y = content_bottom
                 - (collapsed_table_cell
                    ? style->border.bottom / 2
                    : style->border.bottom),
            .width = outer_width + collapsed_left_outset
                     + collapsed_right_outset,
            .height = style->border.bottom,
            .color = border_colors[STYLE_BORDER_BOTTOM],
            .opacity_scale = alpha_opacity_scale(
                border_alphas[STYLE_BORDER_BOTTOM])
        };
    }
    if (!rounded_border && style->border.left > 0 && border_height > 0
        && computed_style_border_line(
               style, STYLE_BORDER_LEFT) != STYLE_BORDER_NONE) {
        square_borders[square_border_count++] = (DrawCommand) {
            .type = DRAW_FILL_RECT,
            .x = outer_x - (collapsed_table_cell
                            ? style->border.left / 2 : 0),
            .y = outer_y,
            .width = style->border.left,
            .height = border_height,
            .color = border_colors[STYLE_BORDER_LEFT],
            .opacity_scale = alpha_opacity_scale(
                border_alphas[STYLE_BORDER_LEFT])
        };
    }
    if (!rounded_border && style->border.right > 0 && border_height > 0
        && computed_style_border_line(
               style, STYLE_BORDER_RIGHT) != STYLE_BORDER_NONE) {
        square_borders[square_border_count++] = (DrawCommand) {
            .type = DRAW_FILL_RECT,
            .x = outer_x + outer_width
                 - (collapsed_table_cell
                    ? style->border.right / 2
                    : style->border.right),
            .y = outer_y,
            .width = style->border.right,
            .height = border_height,
            .color = border_colors[STYLE_BORDER_RIGHT],
            .opacity_scale = alpha_opacity_scale(
                border_alphas[STYLE_BORDER_RIGHT])
        };
    }
    if (!layout_insert_commands(context, before_insertion_index,
                                square_borders, square_border_count)) {
        return false;
    }
    before_insertion_index += square_border_count;
    scroll_command_start += square_border_count;
    *insertion_index_io = before_insertion_index;
    *scroll_command_start_io = scroll_command_start;
    return true;
}
