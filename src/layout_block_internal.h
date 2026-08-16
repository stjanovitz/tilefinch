/* Internal seam header for the block-layout translation units.
   Shared by src/layout_block.c and the layout_block_*.c units split out of
   it.  Only tiny, hot helpers live here as `static inline`; everything else
   is declared and defined in exactly one TU. */

#ifndef TILEFINCH_LAYOUT_BLOCK_INTERNAL_H
#define TILEFINCH_LAYOUT_BLOCK_INTERNAL_H

#include "layout_internal.h"

#include <limits.h>
#include <stdint.h>

/* --- layout_block_geometry.c ------------------------------------------- */

/* Border/padding insets for the requested paint box. */
void layout_block_style_paint_box_insets(const ComputedStyle *style,
                                         StylePaintBox box,
                                         int *left, int *top,
                                         int *right, int *bottom);

void layout_block_set_paint_layer_object_position(
    DrawCommand *command, const StylePaintLayer *layer);

void layout_block_size_paint_image_command(
    DrawCommand *command, const StylePaintLayer *layer,
    const ImageResource *image, int area_width, int area_height);

/* style_pixel_height() plus the percentage case against a definite
   containing-block height. */
int layout_block_style_resolved_height(const Stylesheet *sheet,
                                       const ComputedStyle *style,
                                       int width_reference,
                                       int containing_height);

int layout_block_resolve_positioned_inset(const Stylesheet *sheet, int value,
                                          uint8_t percent_mask,
                                          uint8_t edge, int reference);

/* --- Decoration paint plan ---------------------------------------------- */

/* A block's decorations are emitted before its children are laid out, so the
   border-box height is not yet known.  The emission phase records the command
   indices and the geometry it derived; the back-patch phase resizes those
   exact commands once the content bottom is final.  Always passed by pointer:
   it lives in a layout_block_impl frame that recurses to depth 48. */
typedef struct LayoutBlockPaintPlan {
    const StylePaintStack *paint_stack;
    const StylePaintLayer *mask_layer;
    const StylePaintLayer *background_geometry;
    const ImageResource *element_mask;
    const ImageResource *element_background;
    const ImageResource *background_layer_resources[STYLE_PAINT_LAYER_LIMIT];
    size_t background_layer_indices[STYLE_PAINT_LAYER_LIMIT];
    uint8_t background_layer_stack_indices[STYLE_PAINT_LAYER_LIMIT];
    uint8_t background_layer_count;
    /* (size_t) -1 when the corresponding command was not emitted. */
    size_t background_index;
    size_t background_gradient_index;
    size_t background_image_index;
    size_t background_overlay_index;
    size_t mask_gradient_index;
    size_t rounded_border_index;
    size_t shadow_index_start;
    size_t shadow_command_count;
    int border_radius_code;
    int background_inset_left;
    int background_inset_top;
    int background_inset_right;
    int background_inset_bottom;
    uint32_t border_colors[STYLE_BORDER_SIDE_COUNT];
    uint8_t border_alphas[STYLE_BORDER_SIDE_COUNT];
    bool rounded_border;
} LayoutBlockPaintPlan;

/* --- Block layout frame -------------------------------------------------- */

/* The subset of layout_block_impl's frame that an extracted formatting-context
   section needs.  Every member is established before the section runs and is
   never reassigned by it; the sections mutate only through the pointers (and
   through the LayoutContext).  Always passed by pointer -- ComputedStyle is
   496 bytes and layout_block_impl recurses to depth 48. */
typedef struct LayoutBlockFrame {
    lxb_dom_node_t *node;
    ComputedStyle *style;
    LayoutBlockScratch *scratch;
    LineState *line;
    PositionedBox *descendant_positioned_box;
    int content_x;
    int content_width;
    int child_containing_height;
    int declared_content_height;
    bool definite_height;
    bool grid_minimum_block_size;
    bool replaced_content;
    bool grid;
    bool flex_row;
    bool reverse_row;
    bool table_row;
    bool css_table_row;
    bool anonymous_cell_row;
} LayoutBlockFrame;

/* --- layout_block_grid.c ------------------------------------------------- */

bool layout_block_grid_section(LayoutContext *context,
                               const LayoutBlockFrame *frame);

/* --- layout_block_flexrow.c ---------------------------------------------- */

bool layout_block_flexrow_section(LayoutContext *context,
                                  const LayoutBlockFrame *frame);

/* --- layout_block_decoration.c ------------------------------------------ */

bool layout_block_emit_decoration(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int outer_x, int outer_y, int outer_width,
    LayoutBlockPaintPlan *plan);

bool layout_block_patch_decoration(
    LayoutContext *context, const ComputedStyle *style,
    const LayoutBlockPaintPlan *plan, int outer_x, int outer_y,
    int outer_width, int content_bottom, int border_height,
    bool collapsed_table_cell, size_t *insertion_index_io,
    size_t *scroll_command_start_io);

/* --- layout_block.c ----------------------------------------------------- */

/* Margin collapsing recurses over page-controlled depth while holding the
   largest objects layout has -- a ComputedStyle is 496 bytes, a FlatItem
   1008 -- so its frames were the widest on the stack, and it counted its own
   depth from zero rather than continuing the layout tree's, which let the
   two budgets sum against pspsdk's 256 KB default with no guard page.  The
   per-level state now lives in the same bounded heap arena layout_block
   uses, indexed by the shared depth. */
LayoutBlockScratch *layout_block_scratch_for_depth(
    LayoutContext *context, size_t depth);

/* --- layout_block_margins.c -------------------------------------------- */

int layout_block_collapsed_block_top_margin(LayoutContext *context,
                                            lxb_dom_node_t *node,
                                            const ComputedStyle *style,
                                            size_t depth);

int layout_block_collapsed_block_bottom_margin(LayoutContext *context,
                                               lxb_dom_node_t *node,
                                               const ComputedStyle *style,
                                               size_t depth);

bool layout_block_list_item_starts_with_block(LayoutContext *context,
                                              lxb_dom_node_t *node,
                                              const ComputedStyle *style);

bool layout_block_list_item_parent_has_columns(LayoutContext *context,
                                               lxb_dom_node_t *node);

/* The margin walk continues the layout tree's depth budget rather than
   starting a fresh one.  Two independent 64-level budgets summed on the same
   C stack; sharing one keeps the total bounded by a single budget, and the
   depth also selects the scratch slot, which is free precisely because no
   layout_block frame has descended past this point yet. */
static inline size_t collapse_walk_origin(const LayoutContext *context)
{
    return context == NULL ? 0 : context->tree_call_depth;
}

/* --- Collapsed-margin accumulator accessors ---------------------------- */

static inline int collapsed_margin_value(const CollapsedMargin *margin)
{
    if (margin == NULL || !margin->valid) return 0;
    int64_t value = (int64_t) margin->largest_positive
                    + margin->smallest_negative;
    if (value > INT_MAX) return INT_MAX;
    if (value < INT_MIN) return INT_MIN;
    return (int) value;
}

static inline void collapsed_margin_reset(CollapsedMargin *margin, int value)
{
    *margin = (CollapsedMargin) {
        .largest_positive = value > 0 ? value : 0,
        .smallest_negative = value < 0 ? value : 0,
        .valid = true
    };
}

/* Return the amount by which the previously laid-out, additive margin pair
   must be shortened.  Keeping both extrema makes empty-block chains correct
   for arbitrary mixtures of positive and negative margins without a heap
   worklist or per-node retained state. */
static inline int collapsed_margin_add(CollapsedMargin *margin, int value)
{
    int before = collapsed_margin_value(margin);
    if (!margin->valid) {
        collapsed_margin_reset(margin, value);
        return 0;
    }
    if (value > margin->largest_positive) {
        margin->largest_positive = value;
    }
    if (value < margin->smallest_negative) {
        margin->smallest_negative = value;
    }
    int after = collapsed_margin_value(margin);
    int64_t reduction = (int64_t) before + value - after;
    if (reduction > INT_MAX) return INT_MAX;
    if (reduction < INT_MIN) return INT_MIN;
    return (int) reduction;
}

/* --- Block margin-collapsing predicates -------------------------------- */

static inline bool block_parent_shares_child_margins(
    const ComputedStyle *style)
{
    return style != NULL && style->display == DISPLAY_BLOCK
           && style->float_mode == FLOAT_NONE
           && !style->out_of_flow && !style->fixed_position
           && !style->overflow_x_scroll && !style->overflow_y_scroll
           && !style->overflow_x_clip_only && !style->overflow_y_clip_only;
}

static inline bool block_establishes_formatting_context(
    const ComputedStyle *style)
{
    return style != NULL
           && (style->overflow_x_scroll || style->overflow_y_scroll
               || style->overflow_x_clip_only || style->overflow_y_clip_only
               || style->display == DISPLAY_FLOW_ROOT
               || style->display == DISPLAY_FLEX
               || style->display == DISPLAY_GRID
               || style->display == DISPLAY_TABLE);
}

static inline bool block_parent_collapses_top(const ComputedStyle *style)
{
    return block_parent_shares_child_margins(style)
           && style->border.top == 0 && style->padding.top == 0;
}

static inline bool block_parent_collapses_bottom(const ComputedStyle *style,
                                                 bool definite_height)
{
    return block_parent_shares_child_margins(style)
           && style->border.bottom == 0 && style->padding.bottom == 0
           && !definite_height;
}

static inline bool block_item_has_collapsible_margins(
    const ComputedStyle *style)
{
    return style != NULL && is_block_display(style->display)
           && style->display != DISPLAY_TABLE_ROW
           && style->display != DISPLAY_TABLE_CELL
           && style->float_mode == FLOAT_NONE
           && !style->out_of_flow && !style->fixed_position
           && style->clear_mode == CLEAR_NONE;
}

#endif /* TILEFINCH_LAYOUT_BLOCK_INTERNAL_H */
