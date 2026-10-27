/* Block formatting: length resolution, margin collapsing, positioning,
   and the main block layout algorithm with flex/grid/table dispatch.
   Split out of layout.c. */

#include "layout_block_internal.h"
#include "tilefinch/integer_math.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Fixed descendants paint in viewport space and never enlarge an ancestor's
   scrollable overflow.  Their range is registered before control returns to
   the ancestor, avoiding another retained flag on every draw command. */
static bool layout_command_is_fixed_descendant(
    const LayoutDocument *layout, size_t command_index)
{
    if (layout == NULL) return false;
    for (size_t i = 0; i < layout->fixed_count; i++) {
        const FixedRange *range = &layout->fixed_ranges[i];
        if (command_index >= range->command_start
            && command_index < range->command_end) return true;
    }
    return false;
}


/* Length resolution, paint-layer sizing, and box sizing live in
   src/layout_block_geometry.c.  The grid formatting context, together with
   its helper seam, lives in src/layout_block_grid.c. */

/* CSS 2.1 10.3.4: a block-level replaced element with width:auto takes the
   width an inline replaced element would, from its intrinsic size and
   ratio, instead of filling the line. Returns the border-box width, or 0
   when the element has no intrinsic size yet (it then fills the line like
   any other auto-width block). */
__attribute__((noinline))
static int layout_block_replaced_auto_width(LayoutContext *context,
                                            lxb_dom_node_t *node,
                                            const ComputedStyle *style,
                                            int available,
                                            int containing_height)
{
    if (style->has_width || style->width_max_content
        || !(style->generated_image || layout_replaced_element_tag(node)))
        return 0;
    LayoutReplacedInfo replaced;
    layout_replaced_info(context, node, style, &replaced);
    bool available_image = replaced.available;
    int intrinsic_width = replaced.natural_width;
    int intrinsic_height = replaced.natural_height;
    if (intrinsic_width <= 0 || intrinsic_height <= 0) return 0;
    int width = intrinsic_width;
    int height = layout_block_style_resolved_height(
        context->sheet, style, available, containing_height);
    if (height > 0 && style->box_sizing_border_box) {
        height -= style->padding.top + style->padding.bottom
                  + style->border.top + style->border.bottom;
    }
    if (height > 0 && (available_image || replaced.canvas
        || (style->aspect_width > 0 && style->aspect_height > 0))) {
        width = style->aspect_width > 0 && style->aspect_height > 0
            ? layout_scale_dimension(style->aspect_width, height,
                                     style->aspect_height)
            : layout_scale_dimension(intrinsic_width, height,
                                     intrinsic_height);
    }
    if (width <= 0) return 0;
    int edges = style->padding.left + style->padding.right
                + style->border.left + style->border.right;
    int outer = width > LAYOUT_COORDINATE_LIMIT - edges
        ? LAYOUT_COORDINATE_LIMIT : width + edges;
    /* An over-wide image still scales down to the line, as the replaced
       paint path below does for an automatic width. */
    return outer > available ? available : outer;
}

bool is_block_display(DisplayMode display)
{
    return display == DISPLAY_BLOCK || display == DISPLAY_FLOW_ROOT
           || display == DISPLAY_FLEX
           || display == DISPLAY_GRID || display == DISPLAY_TABLE
           || display == DISPLAY_TABLE_ROW || display == DISPLAY_TABLE_CELL;
}

__attribute__((noinline))
static int content_visibility_intrinsic_height(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    char value[96];
    if (!style_retained_property_value(
            sheet, node, "contain-intrinsic-size", 22,
            value, sizeof(value))) return 0;
    const char *parts[3] = {0};
    size_t lengths[3] = {0};
    size_t count = 0;
    for (size_t at = 0, total = strlen(value); at < total && count < 3;) {
        while (at < total && isspace((unsigned char) value[at])) at++;
        if (at == total) break;
        size_t start = at;
        while (at < total && !isspace((unsigned char) value[at])) at++;
        if (at - start == 4
            && strncasecmp(value + start, "auto", 4) == 0) continue;
        parts[count] = value + start;
        lengths[count++] = at - start;
    }
    if (count == 0 || count > 2) return 0;
    bool percent = false;
    int height = style_parse_length(
        sheet, parts[count == 1 ? 0 : 1],
        lengths[count == 1 ? 0 : 1], 0, &percent);
    return !percent && height > 0 ? height : 0;
}

/* The CollapsedMargin accessors and the block margin-collapsing predicates
   are `static inline` in layout_block_internal.h. */

#include "layout_block/multicolumn.inc"

/* Empty portal containers after a flexible main pane do not consume its
   remaining main size. Require a provable zero box, including pseudo content;
   unknown/automatic content keeps the general conservative path. */
static bool column_item_definitely_empty(LayoutContext *context,
                                         const FlatItem *item)
{
    const ComputedStyle *s = &item->style;
    if (item->anonymous_text || item->generated_pseudo != PSEUDO_NONE
        || item->node == NULL || item->node->first_child != NULL
        || s->display != DISPLAY_BLOCK || s->min_height != 0
        || (s->has_height && (s->height != 0 || s->height_percent))
        || s->aspect_height > 0 || s->padding.top || s->padding.bottom
        || s->border.top || s->border.bottom || s->margin.top || s->margin.bottom
        || s->margin_top_auto || s->margin_bottom_auto || s->generated_image)
        return false;
    if (style_node_pseudo_rules_may_affect_discovery(context->sheet, item->node)) {
        for (unsigned i = 0; i < 2; i++) {
            ComputedStyle pseudo = style_for_pseudo(context->sheet, item->node,
                i == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER, s);
            if (pseudo.generated_content || pseudo.generated_image) return false;
        }
    }
    return true;
}


/* Margin collapsing, its cache, and the list-item probes live in
   src/layout_block_margins.c. */



__attribute__((noinline))
static bool layout_block_fallback(LayoutContext *context,
                                  lxb_dom_node_t *node,
                                  const ComputedStyle *parent,
                                  int x, int y, int width,
                                  const PositionedBox *positioned_box,
                                  int *bottom)
{
    if (context == NULL || context->cancelled || node == NULL
        || parent == NULL || bottom == NULL) return false;
    int safe_width = width < 8 ? 8 : width;
    size_t command_start = context->layout->count;
    LineState line = {
        .start_x = x,
        .base_left = x,
        .x = x,
        .right = x + safe_width,
        .base_right = x + safe_width,
        .y = y,
        .line_gap = 0,
        .strut_fixed = layout_inline_style_line_height_fixed(context, parent),
        .strut_baseline = layout_inline_strut_baseline(context, parent, node),
        .atomics = &context->line_atomics,
        .atomic_base = context->line_atomics.count,
        .layout = context->layout,
        .command_start = context->layout->count,
        .link_start = context->layout->link_count,
        .control_start = context->layout->control_count,
        .node_box_start = context->layout->node_box_count,
        .text_align = computed_style_used_text_align(parent),
        .direction_rtl = computed_style_direction_rtl(parent),
        .find_block_start = true,
        .positioned_box = positioned_box == NULL
            ? (PositionedBox) {.x = x, .y = y, .width = safe_width}
            : *positioned_box
    };
    if (!flow_subtree_fallback(context, node, parent, &line,
                               NULL, 0, NULL)) return false;
    layout_flush_line(&line);
    line_finish_vertical(&line);
    *bottom = line.y < y ? y : line.y;
    int height = *bottom - y;
    return add_node_box(context->layout, node, x, y, safe_width, height,
                        safe_width, height, safe_width, height,
                        parent->padding.left + parent->padding.right,
                        parent->padding.top + parent->padding.bottom,
                        false, false, 0, 0, 0, 0, false, false,
                        false, true,
                        command_start, context->layout->count,
                        command_start, context->layout->count,
                        line.link_start, context->layout->link_count,
                        line.control_start, context->layout->control_count);
}

static bool layout_block_impl(LayoutContext *context, lxb_dom_node_t *node,
                              const ComputedStyle *parent, int x, int y,
                              int width, int containing_height,
                              bool assigned_width,
                              const PositionedBox *positioned_box,
                              int *bottom, LayoutBlockScratch *scratch,
                              FloatExclusion *float_output,
                              size_t float_output_capacity,
                              size_t *float_output_count);

LayoutBlockScratch *layout_block_scratch_for_depth(
    LayoutContext *context, size_t depth)
{
    if (context == NULL || context->layout == NULL
        || depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT) return NULL;
    size_t page_index = depth / LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH;
    if (context->block_scratch_pages[page_index] == NULL) {
        context->block_scratch_pages[page_index] = budget_calloc(
            context->layout->budget, LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH,
            sizeof(LayoutBlockScratch));
        if (context->block_scratch_pages[page_index] == NULL) {
            context->block_scratch_allocation_failures++;
            return NULL;
        }
        context->block_scratch_page_count++;
    }
    return &context->block_scratch_pages[page_index]
        [depth % LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH];
}

static bool layout_block_with_float_output(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int x, int y, int width,
    int containing_height, bool assigned_width,
    const PositionedBox *positioned_box, int *bottom,
    FloatExclusion *float_output, size_t float_output_capacity,
    size_t *float_output_count)
{
    if (float_output_count != NULL) *float_output_count = 0;
    if (!layout_tree_enter(context, node, "block")) {
        return context != NULL && !context->cancelled
            && layout_block_fallback(context, node, parent, x, y, width,
                                     positioned_box, bottom);
    }
    LayoutBlockScratch *scratch = layout_block_scratch_for_depth(
        context, context->tree_call_depth - 1);
    if (scratch == NULL) {
        layout_tree_leave(context);
        return context != NULL && !context->cancelled
            && layout_block_fallback(context, node, parent, x, y, width,
                                     positioned_box, bottom);
    }
    lxb_dom_node_t *link_container = context->link_container;
    scratch->line.has_baseline = false;
    bool success = layout_block_impl(context, node, parent, x, y, width,
                                     containing_height, assigned_width,
                                     positioned_box, bottom, scratch,
                                     float_output, float_output_capacity,
                                     float_output_count);
    context->completed_baseline_node = success && scratch->line.has_baseline
        ? node : NULL;
    context->completed_first_baseline = scratch->line.first_baseline;
    context->completed_last_baseline = scratch->line.last_baseline;
    context->link_container = link_container;
    layout_bidi_flow_destroy(scratch->bidi_flow);
    scratch->bidi_flow = NULL;
    if (!success && context != NULL && !context->failure_reported
        && LAYOUT_TRACE(context->layout, LAYOUT)) {
        size_t name_length = 0, id_length = 0, class_length = 0;
        const char *name = document_element_name(node, &name_length);
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr,
                "layout-failure depth=%zu cancelled=%d node=%.*s id=%.*s "
                "class=%.*s commands=%zu boxes=%zu links=%zu controls=%zu\n",
                context->tree_call_depth, context->cancelled,
                (int) name_length, name == NULL ? "" : name,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name,
                context->layout->count, context->layout->node_box_count,
                context->layout->link_count, context->layout->control_count);
        context->failure_reported = true;
    }
    layout_tree_leave(context);
    return success && !context->cancelled;
}

bool layout_block(LayoutContext *context, lxb_dom_node_t *node,
                  const ComputedStyle *parent, int x, int y, int width,
                  int containing_height, bool assigned_width,
                  const PositionedBox *positioned_box, int *bottom)
{
    return layout_block_with_float_output(
        context, node, parent, x, y, width, containing_height,
        assigned_width, positioned_box, bottom, NULL, 0, NULL);
}

/* Only ordinary static intermediates may be crossed by delayed paint.
   Their styles are resolved through the same retained cascade as flow;
   clipping/effect contexts keep their existing local paint discipline. */
static bool positioned_height_parent(LayoutContext *context,
    lxb_dom_node_t *node, lxb_dom_node_t *containing, ComputedStyle *result)
{
    lxb_dom_node_t *chain[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == 64) return false;
        chain[count++] = at;
    }
    ComputedStyle parent = layout_initial_root_style();
    bool inside = false;
    while (count != 0) {
        lxb_dom_node_t *at = chain[--count];
        ComputedStyle style;
        layout_style_for_node_into(context, at, &parent, &style);
        if (at == containing) {
            if (style.display == DISPLAY_INLINE
                || style.display == DISPLAY_CONTENTS
                || document_node_is_tag(at, LXB_TAG_HTML)) return false;
            inside = true;
        }
        else if (inside && (style.opacity != 255 || style.has_transform
            || style.overflow_x_scroll || style.overflow_y_scroll
            || style.overflow_x_clip_only || style.overflow_y_clip_only
            || computed_style_paint_stack_id(&style) != 0
            || computed_style_filter_code(&style) != STYLE_FILTER_NONE
            || computed_style_clip_path_type(&style) != STYLE_CLIP_PATH_NONE
            || document_node_is_shadow_carrier(at))) return false;
        parent = style;
    }
    if (result != NULL) *result = parent;
    return inside;
}

/* Rare (a deferred percentage-height positioned child), and it holds a
   ComputedStyle: kept out of the recursive layout_block_impl frame. */
__attribute__((noinline))
static bool finish_positioned_heights(LayoutContext *context,
    lxb_dom_node_t *node, const PositionedBox *box, int height)
{
    for (size_t i = 0; i < context->positioned_height_job_count;) {
        if (context->positioned_height_jobs[i].containing_node != node) {
            i++;
            continue;
        }
        lxb_dom_node_t *child = context->positioned_height_jobs[i].node;
        lxb_dom_node_t *link = context->positioned_height_jobs[i].link_container;
        int x = context->positioned_height_jobs[i].x;
        int y = context->positioned_height_jobs[i].y;
        int width = context->positioned_height_jobs[i].width;
        context->positioned_height_job_count--;
        memmove(&context->positioned_height_jobs[i],
            &context->positioned_height_jobs[i + 1],
            (context->positioned_height_job_count - i)
                * sizeof(context->positioned_height_jobs[0]));
        ComputedStyle parent;
        if (!positioned_height_parent(context, child, node, &parent)) return false;
        PositionedBox used = *box;
        used.height = height > 0 ? height : 0;
        used.height_provisional = false;
        int ignored = y;
        lxb_dom_node_t *saved_link = context->link_container;
        context->link_container = link;
        bool ok = layout_block(context, child, &parent, x, y, width,
                               used.height, false, &used, &ignored);
        context->link_container = saved_link;
        if (!ok) return false;
    }
    return true;
}

/* A block-level ::before/::after with clear (the clearfix idiom) is placed
   below the earlier floats it clears, exactly like an in-flow child; its
   bottom then becomes the container's content bottom (CSS 2.1 9.5.2). */
static int layout_block_clear_floats_y(const LineState *line,
                                       ClearMode clear, int y)
{
    if (line == NULL || clear == CLEAR_NONE) return y;
    for (size_t i = 0; i < line->float_count; i++) {
        const FloatExclusion *exclusion = &line->floats[i];
        bool matches = clear == CLEAR_BOTH
            || (clear == CLEAR_LEFT && exclusion->side == FLOAT_LEFT)
            || (clear == CLEAR_RIGHT && exclusion->side == FLOAT_RIGHT);
        if (matches && exclusion->bottom > y) y = exclusion->bottom;
    }
    return y;
}

/* A block whose used height is a synthetic motion scene (a keyframed
   virtual scroll range far taller than its content) or whose subtree has
   saturated the coordinate space is capped to a paintable height. Returns
   true, with the capped bottoms and the line moved, when it contained the
   box. Cold: the caller admits only those two cases. */
__attribute__((noinline))
static bool layout_block_contain_motion_scene(
    LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *style,
    LineState *line, int outer_y, size_t node_command_start,
    int viewport_height, int natural_content_height,
    bool synthetic_motion_scene, int content_border_bottom,
    int *content_bottom_io, int *painted_content_bottom_io)
{
    int content_bottom = *content_bottom_io;
    int painted_content_bottom = *painted_content_bottom_io;
    int scene_limit = layout_clamp_coordinate(
        (int64_t) viewport_height * 64);
    int static_motion_limit = viewport_height;
    if (natural_content_height > static_motion_limit) {
        static_motion_limit = natural_content_height;
    }
    int static_motion_maximum = layout_clamp_coordinate(
        (int64_t) viewport_height * 4);
    if (static_motion_limit > static_motion_maximum) {
        static_motion_limit = static_motion_maximum;
    }
    bool root_box = document_node_is_tag(node, LXB_TAG_HTML)
        || document_node_is_tag(node, LXB_TAG_BODY);
    bool saturated_subtree =
        painted_content_bottom >= LAYOUT_COORDINATE_LIMIT;
    int contained_height = synthetic_motion_scene
        ? static_motion_limit : scene_limit;
    int cap = layout_add_coordinate(outer_y, contained_height);
    if (root_box
        && context->layout->performance.coordinate_containments != 0u) {
        /* Recover the last coherently paintable static state around the
           first contained scene. Ignore giant retained backdrop/spacer
           commands, but preserve ordinary text, controls, and images
           immediately around that scene so Bottom remains useful. */
        int useful_cap = layout_add_coordinate(outer_y, viewport_height);
        int useful_limit = layout_add_coordinate(
            context->layout->performance.first_coordinate_clamp_y,
            static_motion_maximum);
        for (size_t i = node_command_start;
             i < context->layout->count; i++) {
            const DrawCommand *command = &context->layout->commands[i];
            if (command->height > static_motion_maximum) continue;
            int command_bottom = layout_add_coordinate(
                command->y, command->height);
            if (command_bottom < LAYOUT_COORDINATE_LIMIT
                && command_bottom <= useful_limit
                && command_bottom > useful_cap) {
                useful_cap = command_bottom;
            }
        }
        cap = useful_cap;
    } else if (saturated_subtree
        && content_bottom < LAYOUT_COORDINATE_LIMIT) {
        /* Flow has already incorporated each contained child's used
           height. Prefer that truthful edge over stale, pre-clip paint
           coordinates retained inside an unsupported scroll scene. This
           also prevents every ancestor from re-expanding the same scene. */
        int minimum_flow_cap = layout_add_coordinate(
            outer_y, viewport_height);
        cap = content_bottom > minimum_flow_cap
            ? content_bottom : minimum_flow_cap;
    }
    if (cap < content_bottom || cap < painted_content_bottom) {
        if (context->layout->performance.coordinate_containments == 0u)
            context->layout->performance.first_coordinate_clamp_y = outer_y;
        context->layout->performance.coordinate_containments++;
        if (painted_content_bottom >= LAYOUT_COORDINATE_LIMIT)
            context->layout->performance.coordinate_clamps++;
        *content_bottom_io = cap;
        *painted_content_bottom_io = cap;
        line->y = layout_subtract_coordinate(
            layout_subtract_coordinate(cap, style->padding.bottom),
            content_border_bottom);
        return true;
    }
    return false;
}

/* A box's transform: scale and quarter-turn rotation about its origin,
   then translation, applied to everything the box emitted. */
__attribute__((noinline))
static void layout_block_apply_transform(
    LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *style,
    int outer_x, int outer_y, int outer_width, int border_height,
    size_t node_command_start, size_t node_link_start,
    size_t node_control_start, size_t node_box_start)
{
    int origin_x_twice = 0, origin_y_twice = 0;
    layout_transform_origin_twice(
        context->sheet, style, outer_x, outer_y,
        outer_width, border_height,
        &origin_x_twice, &origin_y_twice);
    layout_scale_range(context->layout, node_command_start, node_link_start,
                node_control_start, node_box_start,
                origin_x_twice, origin_y_twice,
                style->transform_scale_q14, node);
    layout_rotate_range_quadrants(
        context->layout, node_command_start, node_link_start,
        node_control_start, node_box_start,
        origin_x_twice, origin_y_twice,
        style->individual_rotate_quadrants, node);
    int dx, dy;
    style_transform_resolve_translation(context->sheet, style,
        outer_width, border_height, &dx, &dy);
    layout_translate_range(context->layout, node_command_start, node_link_start,
                    node_control_start, node_box_start, dx, dy,
                    "transform", node);
}

/* The ::marker of a display:list-item block: its ordinal from the list's
   start, reversed and value attributes, its text, and its outside or inside
   placement on the first line. Cold: the caller admits only list items. */
__attribute__((noinline))
static bool layout_block_list_marker(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *style,
                                     LineState *line, int content_x)
{
    bool ordered = node->parent != NULL
                   && document_node_is_tag(node->parent, LXB_TAG_OL);
    int position = 1;
    if (ordered) {
        bool reversed = document_attribute(
            node->parent, "reversed", &(size_t) {0}) != NULL;
        int next = 1;
        size_t start_length = 0;
        const char *start = document_attribute(
            node->parent, "start", &start_length);
        if (start != NULL && start_length != 0 && start_length < 16) {
            char value[16];
            memcpy(value, start, start_length);
            value[start_length] = '\0';
            char *end = NULL;
            long parsed = strtol(value, &end, 10);
            if (end != value) next = (int) parsed;
        } else if (reversed) {
            next = 0;
            for (lxb_dom_node_t *child = node->parent->first_child;
                 child != NULL; child = child->next) {
                if (child->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && document_node_is_tag(child, LXB_TAG_LI)) next++;
            }
        }
        for (lxb_dom_node_t *child = node->parent->first_child;
             child != NULL; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT
                || !document_node_is_tag(child, LXB_TAG_LI)) continue;
            size_t value_length = 0;
            const char *authored = document_attribute(
                child, "value", &value_length);
            if (authored != NULL && value_length != 0
                && value_length < 16) {
                char value[16];
                memcpy(value, authored, value_length);
                value[value_length] = '\0';
                char *end = NULL;
                long parsed = strtol(value, &end, 10);
                if (end != value) next = (int) parsed;
            }
            if (child == node) {
                position = next;
                break;
            }
            next += reversed ? -1 : 1;
        }
    }
    ListStyleType marker_type = (ListStyleType) style->list_style_type;
    if (marker_type == LIST_STYLE_AUTO) {
        marker_type = ordered ? LIST_STYLE_DECIMAL : LIST_STYLE_DISC;
    }
    char marker[32];
    size_t marker_length = list_marker_text(
        marker_type, position, marker, sizeof(marker));
    const char *retained_marker = marker_length == 0 ? NULL
        : layout_retain_generated_text(
            context->layout, marker, marker_length);
    if (retained_marker == NULL) marker_length = 0;
    size_t marker_command = context->layout->count;
    size_t marker_link = context->layout->link_count;
    size_t marker_control = context->layout->control_count;
    size_t marker_node_box = context->layout->node_box_count;
    if (marker_length != 0
        && !flow_text(context, line, retained_marker, marker_length,
                   style, NULL, 0, NULL)) return false;
    int marker_width = line->x - content_x;
    /* CSS Counter Styles: disc/circle/square and the numeric styles
       carry a " " suffix, so an outside marker ends one space before
       the content edge, and an inside one is followed by a space. */
    int marker_gap = marker_length != 0
        ? layout_style_space_width(context, style) : 0;
    if (!style->list_style_inside) {
        layout_translate_range(context->layout, marker_command,
                        marker_link, marker_control, marker_node_box,
                        -(marker_width + marker_gap), 0, "list-marker",
                        node);
        line_cursor_set(line, content_x);
        if (marker_length != 0
            && layout_block_list_item_parent_has_columns(context, node)
            && layout_block_list_item_starts_with_block(context, node, style)) {
            /* In a column box an outside marker shares the first
               principal block's line; it does not consume a fragment
               line before that block. Inline list content keeps the
               marker's ordinary strut. */
            line->line_height = 0;
            line->line_height_fixed = 0;
            line->has_text_character = false;
            line->y_fixed_valid = false;
        }
    } else if (marker_width != 0) {
        line_cursor_set(line, line->x + marker_gap);
    }
    line->pending_space = false;
    return true;
}

/* An unstyled <hr>: the historical two-pixel rule in the text colour,
   an 8px box, and the element's own visual and transform ranges. Authored
   borders, height or background take the ordinary box path instead. */
__attribute__((noinline))
static bool layout_block_hr_rule(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int outer_x, int outer_y, int outer_width, size_t node_command_start,
    size_t node_link_start, size_t node_control_start,
    size_t node_box_start, bool flex_or_grid_item, int *bottom)
{
    /* Keep the historical unstyled separator, but authored rules use
       the ordinary box/decorations path: a height:0 border is not a
       fixed two-pixel line in the inherited text color. */
    DrawCommand rule = {.type = DRAW_FILL_RECT, .x = outer_x,
                        .y = outer_y + 3, .width = outer_width,
                        .height = 2, .color = style->color, .scale = 1,
                        .opacity_scale = alpha_opacity_scale(
                            style->color_alpha)};
    if (layout_add_command(context->layout, rule) == NULL) return false;
    *bottom = outer_y + 8 + style->margin.bottom;
    if (!add_node_box(context->layout, node, outer_x, outer_y,
                      outer_width, 8, outer_width, 8,
                      outer_width, 8,
                      style->padding.left + style->padding.right,
                      style->padding.top + style->padding.bottom,
                      false, false, 0, 0, 0, 0,
                      false, false,
                      style->out_of_flow || style->fixed_position,
                      !style->out_of_flow && !style->fixed_position,
                      node_command_start, context->layout->count,
                      context->layout->count, context->layout->count,
                      node_link_start, context->layout->link_count,
                      node_control_start,
                      context->layout->control_count)) {
        return false;
    }
    if (parent != NULL
        && style->visibility_hidden != parent->visibility_hidden
        && !layout_record_visibility_range(
            context, node_command_start, node_link_start,
            node_control_start, style->visibility_hidden)) {
        return false;
    }
    if (!apply_visual_range(context, node, node_command_start,
                            node_link_start, node_control_start, style,
                            flex_or_grid_item)) {
        return false;
    }
    if (style->has_transform) {
        layout_block_apply_transform(
            context, node, style, outer_x, outer_y, outer_width, 8,
            node_command_start, node_link_start, node_control_start,
            node_box_start);
    }
    return true;
}

/* A textarea the user has resized keeps that border-box size on the
   resizable axes. */
__attribute__((noinline))
static void layout_block_apply_textarea_resize(lxb_dom_node_t *node,
                                               ComputedStyle *style)
{
    int resized_width = 0, resized_height = 0;
    if (document_control_resize(
            node, &resized_width, &resized_height)) {
        if (style->resize_mode == STYLE_RESIZE_BOTH
            || style->resize_mode == STYLE_RESIZE_HORIZONTAL) {
            style->has_width = true;
            style->width_percent = false;
            style->width = resized_width;
        }
        if (style->resize_mode == STYLE_RESIZE_BOTH
            || style->resize_mode == STYLE_RESIZE_VERTICAL) {
            style->has_height = true;
            style->height_percent = false;
            style->height = resized_height;
        }
        style->box_sizing_border_box = true;
    }
}

typedef enum {
    LAYOUT_BLOCK_POSITIONED_FAILED,
    LAYOUT_BLOCK_POSITIONED_IN_FLOW,
    /* Moved to its inset position; it does not advance the flow. */
    LAYOUT_BLOCK_POSITIONED_OUT_OF_FLOW
} LayoutBlockPositionedResult;

/* Final placement of an absolutely positioned, relative, sticky or fixed
   box once its size is known: inset translation, the provisional-bottom
   fix-up, and sticky and fixed range registration. */
__attribute__((noinline))
static LayoutBlockPositionedResult layout_block_finalize_positioned(
    LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *style,
    const PositionedBox *positioned_box, int outer_x, int outer_y,
    int outer_width, int border_height, int content_bottom,
    int positioning_height, int resolved_left, int resolved_right,
    int resolved_top, int resolved_bottom, bool fixed_captured,
    size_t node_command_start, size_t node_link_start,
    size_t node_control_start, size_t node_box_start)
{
    if (style->out_of_flow) {
        /* CSS 10.3.7/10.6.4: the inset positions the margin edge, so the
           border box lands at inset + margin.  Auto margins are stored as
           zero, which matches the non-over-constrained resolution.  This
           also honours the accessibility idiom of hiding a positioned
           element with a large negative margin. */
        int target_x = outer_x;
        int target_y = outer_y;
        if (style->has_left) {
            target_x = positioned_box->x + resolved_left
                       + style->margin.left;
        } else if (style->has_right) {
            target_x = positioned_box->x + positioned_box->width
                       - outer_width - resolved_right - style->margin.right;
        }
        bool provisional_bottom = false;
        if (style->has_top) {
            target_y = positioned_box->y + resolved_top + style->margin.top;
        } else if (style->has_bottom) {
            target_y = positioned_box->y + positioning_height
                       - border_height - resolved_bottom
                       - style->margin.bottom;
            provisional_bottom = (positioned_box->height <= 0
                                  || positioned_box->height_provisional)
                && positioned_box->node != NULL
                && positioned_box->node != node;
        }
        layout_translate_range(context->layout, node_command_start, node_link_start,
                        node_control_start, node_box_start,
                        target_x - outer_x,
                        target_y - outer_y, "out-of-flow", node);
        /* CSS 2.1 10.6.4: `bottom` refers to the containing block's final
           padding box. An auto-height block's bottom is not known yet;
           use the provisional bound above and correct it when the block
           finishes, including growth beyond an authored minimum height. */
        if (provisional_bottom
            && context->bottom_fixup_count < LAYOUT_BOTTOM_FIXUP_LIMIT) {
            size_t at = context->bottom_fixup_count++;
            context->bottom_fixups[at].node = node;
            context->bottom_fixups[at].containing_node = positioned_box->node;
            context->bottom_fixups[at].assumed_bottom =
                layout_add_coordinate(positioned_box->y, positioning_height);
        }
        return LAYOUT_BLOCK_POSITIONED_OUT_OF_FLOW;
    }
    if (style->relative_position) {
        int dx = style->has_left ? resolved_left
                 : (style->has_right ? -resolved_right : 0);
        int dy = style->has_top ? resolved_top
                 : (style->has_bottom ? -resolved_bottom : 0);
        layout_translate_range(context->layout, node_command_start, node_link_start,
                        node_control_start, node_box_start,
                        dx, dy, "relative", node);
    }
    /* A sticky box sticks vertically only against a non-auto `top` (CSS
       Positioned Layout 3, 3.4): with `top:auto` it stays in flow like a
       relative box. washingtonpost.com makes its whole <main> sticky with
       no inset; treated as `top:0` the page froze under the header and
       every page press showed the same screen. (`bottom` stickiness is not
       modelled.) */
    if (style->sticky_position && style->has_top) {
        int sticky_top = resolved_top;
        const LayoutNodeBox *sticky_box = layout_box_for_node(
            context->layout, node);
        int sticky_origin_y = sticky_box == NULL
            ? outer_y : sticky_box->y;
        int sticky_bottom_y = layout_add_coordinate(
            sticky_origin_y,
            sticky_box == NULL ? border_height : sticky_box->height);
        if (!add_sticky_range(context->layout, node_command_start,
                              context->layout->count, sticky_origin_y,
                              sticky_top, sticky_bottom_y)) return LAYOUT_BLOCK_POSITIONED_FAILED;
    }
    if (style->fixed_position) {
        if (fixed_captured) {
            int target_x = outer_x;
            int target_y = outer_y;
            if (style->has_left) {
                target_x = positioned_box->fixed_x + resolved_left
                           + style->margin.left;
            } else if (style->has_right) {
                target_x = positioned_box->fixed_x
                    + positioned_box->fixed_width - outer_width
                    - resolved_right - style->margin.right;
            }
            if (style->has_top) {
                target_y = positioned_box->fixed_y + resolved_top
                           + style->margin.top;
            } else if (style->has_bottom) {
                target_y = positioned_box->fixed_y + positioning_height
                    - border_height - resolved_bottom - style->margin.bottom;
            }
            layout_translate_range(
                context->layout, node_command_start, node_link_start,
                node_control_start, node_box_start,
                target_x - outer_x, target_y - outer_y,
                "fixed-containing-block", node);
            return LAYOUT_BLOCK_POSITIONED_OUT_OF_FLOW;
        }
        int target_x = outer_x;
        if (style->has_left) target_x = resolved_left;
        else if (style->has_right) {
            target_x = context->layout->width - outer_width - resolved_right;
        }
        if (target_x != outer_x) {
            layout_translate_range(context->layout, node_command_start,
                            node_link_start, node_control_start,
                            node_box_start, target_x - outer_x, 0,
                            "fixed", node);
        }
        bool from_bottom = style->has_bottom && !style->has_top;
        int inset = from_bottom ? resolved_bottom
                    : (style->has_top ? resolved_top : 0);
        if (!add_fixed_range(context->layout, node_command_start,
                             context->layout->count, node_link_start,
                             context->layout->link_count, node_control_start,
                             context->layout->control_count, outer_y,
                             content_bottom - outer_y, inset,
                             from_bottom)) return LAYOUT_BLOCK_POSITIONED_FAILED;
        return LAYOUT_BLOCK_POSITIONED_OUT_OF_FLOW;
    }
    return LAYOUT_BLOCK_POSITIONED_IN_FLOW;
}

/* A column flex container's in-flow item count, growing-item count, and
   its single trailing growing item (followed only by provably empty
   items), if any. Out of line: the 1 KiB FlatItem it iterates with must not
   sit in the recursive layout_block_impl frame. */
__attribute__((noinline))
static void layout_block_count_column_items(
    LayoutContext *context, lxb_dom_node_t *node, const ComputedStyle *style,
    const FlexOrderPlan *column_order, size_t *in_flow_count,
    size_t *growing_count, lxb_dom_node_t **growing_tail)
{
    size_t column_in_flow_count = 0;
    size_t column_growing_count = 0;
    lxb_dom_node_t *column_growing_tail = NULL;
    FlexItemIterator count_iterator;
    FlatItem count_item;
    flex_iterator_init(
        &count_iterator, context, node, style, column_order);
    while (flex_iterator_next(&count_iterator, &count_item)) {
        if (!count_item.style.out_of_flow
            && !count_item.style.fixed_position) {
            column_in_flow_count++;
            if (count_item.style.flex_grow > 0) {
                column_growing_count++;
                column_growing_tail = count_item.node;
            } else if (column_growing_tail != NULL
                && !column_item_definitely_empty(context, &count_item)) {
                column_growing_tail = NULL;
            }
        }
    }
    *in_flow_count = column_in_flow_count;
    *growing_count = column_growing_count;
    *growing_tail = column_growing_tail;
}

/* The in-flow child phase of block layout, between the box's own sizing
   and its height and paint finalization: column-flex and table-group
   ordering, every in-flow, floated and out-of-flow child, ::after, multicol
   redistribution, float containment, the natural and max-height-clamped
   content height, trailing-margin collapse, button content centering and
   column-flex main-axis distribution. The flex-row and grid sections have
   already run when this is entered. */
typedef struct LayoutBlockFlowState {
    lxb_dom_node_t *node;
    ComputedStyle *style;
    const PositionedBox *positioned_box;
    LayoutBlockScratch *scratch;
    LineState *line;
    PositionedBox *descendant_positioned_box;
    const MulticolumnLayout *multicolumn;
    const GeneratedPseudoFlow *after_pseudo_flow;
    const TableTracks *own_table_tracks;
    int content_x;
    int content_width;
    int containing_height;
    int child_containing_height;
    int declared_content_height;
    int outer_y;
    int content_border_top;
    int content_border_bottom;
    size_t node_box_start;
    size_t node_link_start;
    size_t node_control_start;
    size_t scroll_command_start;
    bool definite_height;
    bool clamped_vertical_box;
    bool flex_container;
    bool flex_row;
    bool grid;
    bool replaced_content;
    bool margin_collapse_context;
    bool after_flex_item;
    /* In and out. */
    CollapsedMargin trailing_margin;
    bool first_flow_content;
    int flow_bottom_margin;
    /* Out, on success. */
    int natural_content_height;
    bool after_pseudo_flowed_inline;
} LayoutBlockFlowState;

__attribute__((noinline))
static bool layout_block_flow_children(LayoutContext *context,
                                       LayoutBlockFlowState *flow)
{
    lxb_dom_node_t *node = flow->node;
    ComputedStyle *style = flow->style;
    LayoutBlockScratch *scratch = flow->scratch;
    LineState *line = flow->line;
    PositionedBox *descendant_positioned_box =
        flow->descendant_positioned_box;
    MulticolumnLayout multicolumn = *flow->multicolumn;
    const GeneratedPseudoFlow after_pseudo_flow = *flow->after_pseudo_flow;
    CollapsedMargin trailing_margin = flow->trailing_margin;
    bool first_flow_content = flow->first_flow_content;
    int flow_bottom_margin = flow->flow_bottom_margin;
    bool ok = false;
    bool column_flex = !flow->clamped_vertical_box
                       && (style->display == DISPLAY_FLEX
                        || style->display == DISPLAY_INLINE_FLEX)
                       && (style->flex_direction == FLEX_COLUMN
                           || style->flex_direction == FLEX_COLUMN_REVERSE);
    bool table_group_order = style->display == DISPLAY_TABLE;
    bool reverse_column = style->flex_direction == FLEX_COLUMN_REVERSE;
    bool column_item_seen = false;
    size_t column_item_count = 0;
    size_t column_auto_main_margins = 0;
    int column_flow_top = line->y;
    bool cut_before = context->preview_truncated;
    /* Every exit, failure or not, leaves through `out`, so a plan that grew
       onto the budget (more than four distinct order values) is always
       released. */
    FlexOrderPlan *column_order = &scratch->column_order;
    *column_order = (FlexOrderPlan) {0};
    if ((column_flex || table_group_order)
        && !flex_order_plan_build(column_order, context, node, style)) {
        goto out;
    }
    size_t column_in_flow_count = 0;
    size_t column_growing_count = 0;
    lxb_dom_node_t *column_growing_tail = NULL;
    if (column_flex) {
        layout_block_count_column_items(
            context, node, style, column_order, &column_in_flow_count,
            &column_growing_count, &column_growing_tail);
    }
    FlexItemIterator *flow_iterator = &scratch->traversal.flex.iterator;
    FlatItem *flow_item = &scratch->traversal.flex.item;
    flex_iterator_init(flow_iterator, context, node, style,
                       column_flex || table_group_order
                           ? column_order : NULL);
    flow_iterator->source.include_whitespace = !column_flex;
    while (!(flow->flex_row || flow->grid || flow->replaced_content)
           && flex_iterator_next(flow_iterator, flow_item)) {
        if (line->clamp_pending) {
            if (!layout_line_clamp_overflow(line)) {
                goto out;
            }
            break;
        }
        resolve_padding(context->sheet, &flow_item->style, flow->content_width);
        resolve_margin(context->sheet, &flow_item->style, flow->content_width);
        if (flow_item->style.out_of_flow || flow_item->style.fixed_position) {
            if (flow_item->anonymous_text) continue;
            bool static_block = !column_flex
                && is_block_display(flow_item->style.display)
                && flow_item->style.out_of_flow
                && !flow_item->style.has_top
                && !flow_item->style.has_bottom;
            if (static_block) {
                /* A block-level positioned box splits the surrounding
                   anonymous block boxes even though the box itself remains
                   out of flow.  Its auto inset therefore uses the current
                   hypothetical block position after any preceding inline
                   line; the positioned box itself does not advance it. */
                layout_flush_line(line);
                /* The hypothetical block position is an integer used value.
                   Do not expose the floor of the fractional line-box
                   accumulator to an out-of-flow child; ordinary following
                   blocks consume the corresponding ceil. */
                line_finish_vertical(line);
            }
            int positioned_bottom = line->y;
            if (flow_item->style.out_of_flow
                && ((flow_item->style.has_height && flow_item->style.height_percent)
                    || (flow_item->style.has_top
                        && (flow_item->style.inset_percent_mask & STYLE_INSET_TOP_PERCENT))
                    || (flow_item->style.has_bottom
                        && (flow_item->style.inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT)))
                && (flow_item->style.has_top || flow_item->style.has_bottom)
                && (flow_item->style.has_left || flow_item->style.has_right)
                && descendant_positioned_box->height_provisional
                && descendant_positioned_box->node != NULL
                && context->positioned_height_job_count < LAYOUT_BOTTOM_FIXUP_LIMIT
                && positioned_height_parent(context, flow_item->node,
                    descendant_positioned_box->node, NULL)) {
                size_t at = context->positioned_height_job_count++;
                context->positioned_height_jobs[at].node = flow_item->node;
                context->positioned_height_jobs[at].containing_node =
                    descendant_positioned_box->node;
                context->positioned_height_jobs[at].link_container =
                    context->link_container;
                context->positioned_height_jobs[at].x = flow->content_x;
                context->positioned_height_jobs[at].y = line->y;
                context->positioned_height_jobs[at].width = flow->content_width;
                continue;
            }
            if (!layout_block(context, flow_item->node,
                              &flow_item->parent_style, flow->content_x, line->y,
                              flow->content_width, flow->child_containing_height, false,
                              descendant_positioned_box,
                              &positioned_bottom)) {
                goto out;
            }
            /* The static inline position of an absolutely positioned child
               still participates in a column flex container's cross-axis
               alignment when both horizontal insets are auto. */
            if (column_flex && flow_item->style.out_of_flow
                && !flow_item->style.has_left
                && !flow_item->style.has_right) {
                const LayoutNodeBox *child_box = layout_box_for_node(
                    context->layout, flow_item->node);
                AlignItems alignment = flex_item_alignment(
                    style, &flow_item->style);
                if (child_box != NULL
                    && (alignment == ALIGN_CENTER
                        || alignment == ALIGN_END)) {
                    int target_x = alignment == ALIGN_CENTER
                        ? flow->content_x + (flow->content_width - child_box->width) / 2
                        : flow->content_x + flow->content_width - child_box->width;
                    translate_node_subtree(
                        context->layout, flow_item->node,
                        target_x - child_box->x, 0);
                }
            }
            continue;
        }
        if (layout_preview_limit_reached(context, line->y)) break;
        if (!column_flex && flow_item->style.float_mode != FLOAT_NONE) {
            if (flow_item->style.display == DISPLAY_NONE
                || flow_item->style.hidden
                || flow_item->anonymous_text) {
                continue;
            }
            trailing_margin.valid = false;
            first_flow_content = false;
            line->positioned_box = *descendant_positioned_box;
            if (!layout_place_float(
                    context, flow_item->node, &flow_item->parent_style,
                    &flow_item->style, line, flow->child_containing_height)) {
                goto out;
            }
            continue;
        }
        bool block_item = column_flex
                          || (!flow_item->anonymous_text
                              && is_block_display(flow_item->style.display));
        if (block_item) {
            bool had_inline_content = line->x != line->start_x
                                      || line->line_height != 0
                                      || line->line_height_fixed != 0;
            if (had_inline_content) {
                trailing_margin.valid = false;
                first_flow_content = false;
            }
            int unadvanced_empty_line_y = line->y;
            layout_flush_line(line);
            if (!column_flex && !had_inline_content) {
                line->y = unadvanced_empty_line_y;
                line->y_fixed_valid = false;
                update_float_band(line);
            }
            line_finish_vertical(line);
            if (column_flex && column_item_seen) {
                line->y = layout_add_coordinate(line->y, style->row_gap);
            }
            bool margin_item = flow->margin_collapse_context
                && is_block_display(flow_item->style.display)
                && flow_item->style.display != DISPLAY_TABLE_ROW
                && flow_item->style.display != DISPLAY_TABLE_CELL
                && flow_item->style.float_mode == FLOAT_NONE
                && !flow_item->style.out_of_flow
                && !flow_item->style.fixed_position;
            bool collapsible_item = margin_item
                && flow_item->style.clear_mode == CLEAR_NONE;
            if (!collapsible_item && !column_flex) {
                trailing_margin.valid = false;
            }
            int child_y = line->y;
            int item_top_margin = flow_item->style.margin.top;
            if (margin_item) {
                item_top_margin = layout_block_collapsed_block_top_margin(
                    context, flow_item->node, &flow_item->style,
                    collapse_walk_origin(context));
            }
            if (collapsible_item) {
                int reduction = 0;
                if (trailing_margin.valid) {
                    reduction = collapsed_margin_add(
                        &trailing_margin, item_top_margin);
                } else if (first_flow_content
                           && block_parent_collapses_top(style)) {
                    collapsed_margin_reset(&trailing_margin,
                                           item_top_margin);
                    reduction = item_top_margin;
                } else {
                    collapsed_margin_reset(&trailing_margin,
                                           item_top_margin);
                }
                child_y = layout_subtract_coordinate(child_y, reduction);
            } else if (margin_item) {
                collapsed_margin_reset(&trailing_margin, item_top_margin);
            }
            if (!column_flex
                && flow_item->style.clear_mode != CLEAR_NONE) {
                int clear_bottom = child_y;
                for (size_t i = 0; i < line->float_count; i++) {
                    line->layout->performance.float_exclusion_probes++;
                    const FloatExclusion *exclusion = &line->floats[i];
                    bool matches =
                        flow_item->style.clear_mode == CLEAR_BOTH
                        || (flow_item->style.clear_mode == CLEAR_LEFT
                            && exclusion->side == FLOAT_LEFT)
                        || (flow_item->style.clear_mode == CLEAR_RIGHT
                            && exclusion->side == FLOAT_RIGHT);
                    if (matches && exclusion->bottom > clear_bottom) {
                        clear_bottom = exclusion->bottom;
                    }
                }
                int hypothetical_top = layout_add_coordinate(
                    child_y, item_top_margin);
                if (clear_bottom > hypothetical_top) {
                    child_y = layout_add_coordinate(
                        child_y, clear_bottom - hypothetical_top);
                }
            }
            int child_bottom = child_y;
            int child_x = flow->content_x;
            int child_width = multicolumn.active
                ? multicolumn.column_width : flow->content_width;
            bool assigned_column_cross_size = false;
            AlignItems item_alignment = flex_item_alignment(
                style, &flow_item->style);
            if (column_flex && !flow_item->style.has_width
                && item_alignment != ALIGN_STRETCH
                && !flow_item->style.margin_left_auto
                && !flow_item->style.margin_right_auto) {
                /*
                 * An auto cross-size is stretchable only when the item's
                 * used self-alignment is stretch.  Block layout otherwise
                 * fills the available inline size, so give non-stretched
                 * column-flex items their shrink-to-fit outer width before
                 * laying them out.  The intrinsic helpers include the
                 * item's own margins, borders, and padding.
                 */
                int minimum_width = 0;
                intrinsic_text_widths(
                    context, flow_item->node, &flow_item->parent_style,
                    flow->content_width, &child_width, &minimum_width);
                if (child_width < minimum_width) {
                    child_width = minimum_width;
                }
                if (child_width < 8) child_width = 8;
                if (child_width > flow->content_width) {
                    child_width = flow->content_width;
                }
                assigned_column_cross_size = true;
            }
            /* A block establishing its own formatting context (overflow
               other than visible, flex/flow->grid/table) must not underlap
               floats; it narrows to the float-free band at its top edge. */
            if (!column_flex && line->float_count > 0
                && !flow_item->anonymous_text
                && block_establishes_formatting_context(&flow_item->style)) {
                int required_width = 0;
                resolve_padding(context->sheet, &flow_item->style,
                                flow->content_width);
                if (flow_item->style.has_width
                    && !flow_item->style.width_max_content) {
                    required_width = resolve_declared_length(
                        context->sheet, flow_item->style.width,
                        flow_item->style.width_percent, flow->content_width);
                    if (!flow_item->style.box_sizing_border_box) {
                        required_width = layout_add_coordinate(required_width,
                            flow_item->style.padding.left
                            + flow_item->style.padding.right
                            + flow_item->style.border.left
                            + flow_item->style.border.right);
                    }
                }
                required_width = constrain_border_box_width(
                    context, flow_item->node, &flow_item->parent_style,
                    &flow_item->style, flow->content_width, required_width, NULL);
                for (;;) {
                    /* child_y is the margin edge. Clearance and float
                       avoidance both constrain the BFC's border edge. */
                    int band_y = layout_add_coordinate(
                        child_y, item_top_margin);
                    int band_left = flow->content_x;
                    int band_right = flow->content_x + flow->content_width;
                    int next_bottom = INT_MAX;
                    line->layout->performance.float_band_queries++;
                    for (size_t i = 0; i < line->float_count; i++) {
                        line->layout->performance.float_exclusion_probes++;
                        const FloatExclusion *exclusion = &line->floats[i];
                        if (exclusion->top > band_y
                            || exclusion->bottom <= band_y) continue;
                        if (exclusion->bottom < next_bottom) {
                            next_bottom = exclusion->bottom;
                        }
                        if (exclusion->side == FLOAT_LEFT) {
                            if (exclusion->right > band_left) {
                                band_left = exclusion->right;
                            }
                        } else if (exclusion->x < band_right) {
                            band_right = exclusion->x;
                        }
                    }
                    int band_width = band_right - band_left;
                    int margins = flow_item->style.margin.left
                                  + flow_item->style.margin.right;
                    /*
                     * An auto-width BFC may shrink beside a float, but its
                     * margin box still has to fit in that band.  If even its
                     * horizontal margins do not fit, advance to the next
                     * float bottom and recompute instead of manufacturing a
                     * negative content box under the float.
                     */
                    if (band_width >= 0
                        && band_width >= layout_add_coordinate(
                            margins, required_width)) {
                        child_x = band_left;
                        child_width = flow_item->style.has_width
                            ? flow->content_width : band_width;
                        break;
                    }
                    if (next_bottom == INT_MAX) break;
                    child_y = layout_subtract_coordinate(
                        next_bottom, item_top_margin);
                }
            }
            PositionedBox flow_positioned_box = *descendant_positioned_box;
            if (!column_flex) {
                flow_positioned_box.float_exclusions = line->floats;
                flow_positioned_box.float_count = line->float_count;
            }
            if (flow_item->anonymous_text) {
                if (!layout_anonymous_text(context, flow_item, node,
                                           child_x, child_y, child_width,
                                           &child_bottom)) {
                    goto out;
                }
            } else {
                if (table_group_order && flow->own_table_tracks != NULL
                    && flow_item->node == flow->own_table_tracks->first_row) {
                    child_y = layout_add_coordinate(
                        child_y, flow->own_table_tracks->collapsed_top_gutter);
                }
                size_t propagated_count = 0;
                size_t propagated_capacity = !column_flex
                    ? ACTIVE_FLOAT_LIMIT : 0;
                FloatExclusion *propagated = propagated_capacity != 0
                    ? scratch->child_float_output : NULL;
                lxb_dom_node_t *saved_flex_node =
                    context->assigned_flex_node;
                int saved_flex_height = context->assigned_flex_height;
                bool saved_flex_minimum =
                    context->assigned_flex_minimum;
                if (column_flex) {
                    bool growing_single_item =
                        column_in_flow_count == 1
                        && flow_item->style.flex_grow > 0
                        && flow->definite_height
                        && flow->declared_content_height > 0;
                    bool growing_tail_item = column_growing_count == 1
                        && flow_item->node == column_growing_tail
                        && flow_item->style.flex_grow > 0 && flow->definite_height
                        && (!flow_item->style.min_height_auto
                            || (flow_item->style.overflow_y_scroll
                                && !flow_item->style.overflow_y_clip_only));
                    bool definite_minimum =
                        !flow_item->style.has_height
                        && flow_item->style.min_height > 0;
                    int assigned_main = growing_tail_item
                        ? flow->declared_content_height - (child_y - column_flow_top)
                          - flow_item->style.margin.top
                          - flow_item->style.margin.bottom
                        : growing_single_item
                        ? flow->declared_content_height
                          - flow_item->style.margin.top
                          - flow_item->style.margin.bottom
                        : (definite_minimum
                           ? style_content_height(
                               context->sheet, &flow_item->style,
                               child_width, flow->child_containing_height)
                           : 0);
                    if (growing_tail_item
                        && column_item_count < column_in_flow_count
                        && column_in_flow_count - column_item_count > 1u) {
                        size_t trailing = column_in_flow_count - column_item_count - 1u;
                        assigned_main = layout_subtract_coordinate(assigned_main,
                            tilefinch_mul_div_int(style->row_gap,
                                trailing > INT_MAX ? INT_MAX : (int) trailing, 1));
                    }
                    if (growing_tail_item && !flow_item->style.box_sizing_border_box) {
                        resolve_padding(context->sheet, &flow_item->style, flow->content_width);
                        assigned_main -= flow_item->style.padding.top
                            + flow_item->style.padding.bottom
                            + flow_item->style.border.top
                            + flow_item->style.border.bottom;
                    }
                    trace_flex_sizing(context, "column-main", node,
                        flow_item->node, &flow_item->style,
                        flow->declared_content_height, (int) column_in_flow_count,
                        (int) column_growing_count, (int) column_item_count,
                        flow_item->style.min_height_auto, assigned_main);
                    if (assigned_main > 0) {
                        context->assigned_flex_node = flow_item->node;
                        context->assigned_flex_height = assigned_main;
                        context->assigned_flex_minimum = definite_minimum
                            && !growing_single_item && !growing_tail_item;
                    }
                }
                bool child_ok = layout_block_with_float_output(
                        context, flow_item->node,
                        &flow_item->parent_style, child_x, child_y,
                        child_width, flow->child_containing_height,
                        assigned_column_cross_size, &flow_positioned_box,
                        &child_bottom, propagated, propagated_capacity,
                        &propagated_count);
                context->assigned_flex_node = saved_flex_node;
                context->assigned_flex_height = saved_flex_height;
                context->assigned_flex_minimum = saved_flex_minimum;
                if (!child_ok) {
                    goto out;
                }
                /* Block containers export their last in-flow line baseline.
                   A naturally sized column flex container exports its first
                   item's baseline instead. Forward the completed child result
                   while it is available: an enclosing inline-block otherwise
                   scans descendant ink and mistakes a later flex item for the
                   container's baseline. Explicit/reversed/distributed columns
                   keep the existing fallback because they translate later. */
                bool natural_column = column_flex
                    && style->flex_direction == FLEX_COLUMN && !style->flex_wrap
                    && !style->has_height && style->min_height == 0
                    && style->justify_content == JUSTIFY_START;
                if ((!column_flex || (natural_column && !line->has_baseline))
                    && context->completed_baseline_node == flow_item->node) {
                    if (!line->has_baseline)
                        line->first_baseline = context->completed_first_baseline;
                    line->last_baseline = natural_column ? line->first_baseline
                        : context->completed_last_baseline;
                    line->has_baseline = true;
                } else if (natural_column && !line->has_baseline) {
                    const LayoutNodeBox *child_box = layout_box_for_node(
                        context->layout, flow_item->node);
                    if (child_box != NULL) {
                        int baseline = child_box->y + child_box->height;
                        (void) layout_box_first_text_baseline(
                            context->layout, child_box, &baseline);
                        line->first_baseline = line->last_baseline = baseline;
                        line->has_baseline = true;
                    }
                }
                for (size_t i = 0; i < propagated_count; i++) {
                    if (line->float_count == ACTIVE_FLOAT_LIMIT) {
                        size_t kept = 0;
                        for (size_t j = 0; j < line->float_count; j++) {
                            if (line->floats[j].bottom > propagated[i].top)
                                line->floats[kept++] = line->floats[j];
                        }
                        line->float_count = kept;
                    }
                    if (line->float_count < ACTIVE_FLOAT_LIMIT)
                        line->floats[line->float_count++] = propagated[i];
                }
                if (table_group_order && flow->own_table_tracks != NULL
                    && flow_item->node == flow->own_table_tracks->last_row) {
                    child_bottom = layout_add_coordinate(
                        child_bottom,
                        flow->own_table_tracks->collapsed_bottom_gutter);
                }
            }
            if (column_flex
                && (item_alignment == ALIGN_CENTER
                    || item_alignment == ALIGN_END)
                && !flow_item->style.margin_left_auto
                && !flow_item->style.margin_right_auto) {
                const LayoutNodeBox *child_box = layout_box_for_node(
                    context->layout, flow_item->node);
                if (child_box != NULL) {
                    int margin_width = flow_item->style.margin.left
                                       + flow_item->style.margin.right;
                    int target_x = item_alignment == ALIGN_CENTER
                        ? flow->content_x
                          + (flow->content_width - child_box->width
                             - margin_width) / 2
                          + flow_item->style.margin.left
                        : flow->content_x + flow->content_width - child_box->width
                          - flow_item->style.margin.right;
                    int dx = target_x - child_box->x;
                    if (dx != 0) {
                        trace_flex_translation(context, "column-cross",
                                               node, flow_item->node,
                                               item_alignment, flow->content_x,
                                               flow->content_width, child_box->x,
                                               child_box->width, target_x,
                                               dx);
                        translate_node_subtree(context->layout,
                                               flow_item->node, dx, 0);
                    }
                }
            }
            line->y = child_bottom;
            /* Block layout returns an integer flow bottom. Any fractional
               inline accumulator belongs to the preceding anonymous line
               and must not be reused by a later BR or inline sibling. */
            line->y_fixed_valid = false;
            if (margin_item) {
                const LayoutNodeBox *child_box = layout_box_for_node(
                    context->layout, flow_item->node);
                bool empty_block = child_box != NULL
                                   && child_box->height == 0;
                int item_bottom_margin = layout_block_collapsed_block_bottom_margin(
                    context, flow_item->node, &flow_item->style,
                    collapse_walk_origin(context));
                if (empty_block) {
                    line->y = layout_subtract_coordinate(
                        line->y, collapsed_margin_add(
                            &trailing_margin, item_bottom_margin));
                } else {
                    collapsed_margin_reset(
                        &trailing_margin,
                        item_bottom_margin);
                }
            }
            first_flow_content = false;
            if (column_flex) {
                column_item_seen = true;
                column_item_count++;
                if (flow_item->style.margin_top_auto) {
                    column_auto_main_margins++;
                }
                if (flow_item->style.margin_bottom_auto) {
                    column_auto_main_margins++;
                }
            }
            update_float_bounds(line);
            line_cursor_set(line, line->start_x);
            line->line_height = 0;
            line->line_height_fixed = 0;
            line->y_fixed_valid = false;
            line->command_start = context->layout->count;
            line->link_start = context->layout->link_count;
            line->control_start = context->layout->control_count;
            line->node_box_start = context->layout->node_box_count;
            continue;
        }
        const char *flow_link = NULL;
        size_t flow_link_length = 0;
        lxb_dom_node_t *flow_link_node = NULL;
        /* Inline content reached through a display:contents <a href>
           (flattened by the iterator) still belongs to that link. */
        flat_text_link(flow_item->node, node, &flow_link,
                       &flow_link_length, &flow_link_node);
        /*
         * The block establishes the closest mutual ancestor for adjacent
         * top-level inline fragments. Nested inline traversal performs the
         * same handoff internally; without this outer handoff, sibling spans
         * incorrectly inherited the first span's tracking.
         */
        if (line->has_text_character) {
            line->letter_boundary_spacing = style->letter_spacing_fixed;
        }
        if (!flow_inline(context, flow_item->node, &flow_item->parent_style,
                         line, flow_link, flow_link_length,
                         flow_link_node)) {
            goto out;
        }
        if (line->x != line->start_x || line->line_height != 0) {
            trailing_margin.valid = false;
            first_flow_content = false;
        }
    }
    bool after_pseudo_flowed_inline = flow->after_flex_item;
    if (!flow->replaced_content && !after_pseudo_flow.active
        && !flow->after_flex_item
        && !flow_generated_inline_pseudo(
            context, node, style, PSEUDO_AFTER, line, NULL, 0, NULL,
            &after_pseudo_flowed_inline)) {
        goto out;
    }
    /* A provisional (preview) cut inside this flow leaves its items' totals
       partial. The passes below that move the items by those totals
       (column balancing, button centering, column-flex distribution,
       wrapping and reversal) would then place the items laid out before
       the cut where the complete layout never does: such a cut is not a
       complete prefix (layout_note_preview_cut). */
    if (!cut_before && context->preview_truncated
        && (multicolumn.active
            || (column_flex && (flow->declared_content_height > 0
                                || reverse_column || style->flex_wrap))
            || (flow->definite_height
                && document_node_is_tag(node, LXB_TAG_BUTTON))))
        layout_note_preview_cut(context, true);
    if (!flow->replaced_content) {
        layout_flush_line(line);
        line_finish_vertical(line);
    }
    if (multicolumn.active) {
        int content_top = layout_add_coordinate(
            layout_add_coordinate(flow->outer_y, flow->content_border_top),
            style->padding.top);
        if (!multicolumn_redistribute(
                context, node, style, &multicolumn, flow->content_x, content_top,
                flow->definite_height ? flow->declared_content_height : 0, &line->y)) {
            goto out;
        }
        trailing_margin.valid = false;
        first_flow_content = false;
    }
    if (after_pseudo_flow.active) {
        int reduction = 0;
        if (trailing_margin.valid) {
            reduction = collapsed_margin_add(
                &trailing_margin, after_pseudo_flow.margin_top);
        } else if (first_flow_content
                   && block_parent_collapses_top(style)) {
            collapsed_margin_reset(&trailing_margin, style->margin.top);
            reduction = collapsed_margin_add(
                &trailing_margin, after_pseudo_flow.margin_top);
        } else {
            collapsed_margin_reset(&trailing_margin,
                                   after_pseudo_flow.margin_top);
        }
        int pseudo_y = layout_add_coordinate(
            layout_subtract_coordinate(line->y, reduction),
            after_pseudo_flow.margin_top);
        pseudo_y = layout_block_clear_floats_y(
            line, after_pseudo_flow.clear_mode, pseudo_y);
        int pseudo_width = flow->content_width - after_pseudo_flow.margin_left
                           - after_pseudo_flow.margin_right;
        if (pseudo_width < 0) pseudo_width = 0;
        if (!paint_pseudo(
                context, node, style, PSEUDO_AFTER,
                flow->content_x + after_pseudo_flow.margin_left, pseudo_y,
                pseudo_width, after_pseudo_flow.border_height,
                SIZE_MAX, after_pseudo_flow.border_height, NULL)) {
            goto out;
        }
        line->y = layout_add_coordinate(
            layout_add_coordinate(
                pseudo_y, after_pseudo_flow.border_height),
            after_pseudo_flow.margin_bottom);
        if (after_pseudo_flow.border_height == 0
            && !after_pseudo_flow.formatting_root) {
            line->y = layout_subtract_coordinate(
                line->y, collapsed_margin_add(
                    &trailing_margin, after_pseudo_flow.margin_bottom));
        } else {
            collapsed_margin_reset(&trailing_margin,
                                   after_pseudo_flow.margin_bottom);
        }
    }
    if (column_flex && flow->definite_height && column_item_count != 0) {
        line->y = layout_add_coordinate(line->y,
            layout_fit_column_scroller(context, node, style, column_order,
                                       flow->content_width, flow->declared_content_height));
    }
    /* CSS 2.1 10.6.7: an auto-height formatting context root extends to
       the bottom margin edge of the floats placed inside it. Floats the
       caller passed in belong to the enclosing context. */
    if (!column_flex && block_contains_own_floats(style)) {
        int float_bottom = INT_MIN;
        for (size_t i = 0; i < line->float_count; i++) {
            const FloatExclusion *candidate = &line->floats[i];
            bool inherited = false;
            for (size_t j = 0; j < flow->positioned_box->float_count; j++) {
                const FloatExclusion *outer =
                    &flow->positioned_box->float_exclusions[j];
                if (candidate->x == outer->x
                    && candidate->right == outer->right
                    && candidate->top == outer->top
                    && candidate->bottom == outer->bottom
                    && candidate->side == outer->side) {
                    inherited = true;
                    break;
                }
            }
            if (!inherited && candidate->bottom > float_bottom) {
                float_bottom = candidate->bottom;
            }
        }
        int flow_end = layout_subtract_coordinate(
            line->y, trailing_margin.valid
                         ? collapsed_margin_value(&trailing_margin) : 0);
        if (float_bottom > flow_end) {
            line->y = float_bottom;
            trailing_margin.valid = false;
        }
    }
    int content_top_for_height = layout_add_coordinate(
        layout_add_coordinate(flow->outer_y, flow->content_border_top),
        style->padding.top);
    int trailing_for_height = trailing_margin.valid
        ? collapsed_margin_value(&trailing_margin) : 0;
    int natural_content_height =
        line->y - trailing_for_height - content_top_for_height;
    if (natural_content_height < 0) natural_content_height = 0;
    int maximum_content_height = 0;
    bool maximum_height_clamped =
        style->max_height != STYLE_LENGTH_NONE
        && (!style->max_height_percent || flow->containing_height > 0)
        && resolve_computed_length(
            context->sheet, style->max_height, style->max_height_percent,
            flow->containing_height, &maximum_content_height);
    if (maximum_height_clamped && style->box_sizing_border_box) {
        maximum_content_height -=
            style->padding.top + style->padding.bottom
            + flow->content_border_top + flow->content_border_bottom;
    }
    if (maximum_content_height < 0) maximum_content_height = 0;
    maximum_height_clamped =
        maximum_height_clamped
        && natural_content_height > maximum_content_height;
    if (maximum_height_clamped) {
        /* A clamping max-height establishes the used flow bottom and keeps a
           child's end margin from escaping past it. Overflow still paints,
           but the following sibling starts at the constrained border box. */
        line->y = layout_add_coordinate(
            content_top_for_height, maximum_content_height);
        flow_bottom_margin = style->margin.bottom;
        trailing_margin.valid = false;
    } else if (!multicolumn.active && trailing_margin.valid
               && block_parent_collapses_bottom(style, flow->definite_height)) {
        int trailing = collapsed_margin_value(&trailing_margin);
        line->y = layout_subtract_coordinate(line->y, trailing);
        int content_top = layout_add_coordinate(
            layout_add_coordinate(flow->outer_y, flow->content_border_top),
            style->padding.top);
        if (flow->declared_content_height > line->y - content_top) {
            /* CSS 2.1 computes an auto-height block before constraining it
               with min-height.  The child's collapsed end margin is outside
               that tentative height, but once min-height enlarges the box it
               must not be reintroduced into the following sibling's flow. */
            flow_bottom_margin = style->margin.bottom;
        } else {
            (void) collapsed_margin_add(&trailing_margin,
                                        style->margin.bottom);
            flow_bottom_margin = collapsed_margin_value(&trailing_margin);
        }
    }
    int declared_height = flow->declared_content_height;
    /* A definite-height flow button centers its anonymous content box.
       Flex/flow->grid buttons already apply their authored alignment. Root-level
       positioned children are still deferred until the containing box is
       final. A positioned icon contained by an in-flow descendant moves
       with that descendant; only a box anchored outside that content
       subtree prevents translating the whole range. */
    if (flow->definite_height && !flow->flex_container && !flow->grid && !maximum_height_clamped
        && document_node_is_tag(node, LXB_TAG_BUTTON)
        && natural_content_height > 0
        && declared_height > natural_content_height) {
        bool positioned = false;
        for (size_t i = flow->node_box_start; i < context->layout->node_box_count; i++) {
            const LayoutNodeBox *box = &context->layout->node_boxes[i];
            unsigned distance = box->positioned_ancestor_distance;
            if (distance == 0) continue;
            lxb_dom_node_t *owner = box->node;
            for (unsigned step = 0; owner != NULL && step < distance; step++)
                owner = owner->parent;
            if (distance == UINT8_MAX || owner == NULL || owner == node
                || !layout_node_within(owner, node)) positioned = true;
        }
        if (!positioned) {
            int offset = (declared_height - natural_content_height) / 2;
            layout_translate_range(context->layout, flow->scroll_command_start,
                flow->node_link_start, flow->node_control_start, flow->node_box_start,
                0, offset, "button-content", node);
            if (line->has_baseline) {
                line->first_baseline += offset;
                line->last_baseline += offset;
            }
        }
    }
    if (column_flex && column_item_count != 0 && declared_height > 0) {
        int used_height = line->y - column_flow_top;
        int free_height = declared_height - used_height;
        if (free_height > 0
            && (column_auto_main_margins != 0
                || style->justify_content != JUSTIFY_START)) {
            size_t item_index = 0;
            size_t auto_margin_seen = 0;
            FlexItemIterator *iterator = &scratch->traversal.flex.iterator;
            FlatItem *item = &scratch->traversal.flex.item;
            flex_iterator_init(iterator, context, node, style,
                               column_order);
            while (flex_iterator_next(iterator, item)) {
                if (item->style.out_of_flow || item->style.fixed_position) {
                    continue;
                }
                int offset = 0;
                if (column_auto_main_margins != 0) {
                    bool auto_before = reverse_column
                                       ? item->style.margin_bottom_auto
                                       : item->style.margin_top_auto;
                    if (auto_before) auto_margin_seen++;
                    offset = tilefinch_mul_div_int(
                        free_height, auto_margin_seen,
                        column_auto_main_margins);
                } else if (style->justify_content == JUSTIFY_CENTER) {
                    offset = free_height / 2;
                } else if (style->justify_content == JUSTIFY_END) {
                    offset = free_height;
                } else if (style->justify_content == JUSTIFY_SPACE_BETWEEN) {
                    if (column_item_count > 1) {
                        offset = tilefinch_mul_div_int(
                            free_height, item_index, column_item_count - 1);
                    }
                } else if (style->justify_content == JUSTIFY_SPACE_AROUND) {
                    offset = tilefinch_mul_div_int(
                        free_height, 2 * item_index + 1,
                        2 * column_item_count);
                } else if (style->justify_content == JUSTIFY_SPACE_EVENLY) {
                    offset = tilefinch_mul_div_int(
                        free_height, item_index + 1,
                        column_item_count + 1);
                }
                if (offset != 0) {
                    translate_node_subtree(context->layout, item->node,
                                           0, offset);
                }
                if (column_auto_main_margins != 0) {
                    bool auto_after = reverse_column
                                      ? item->style.margin_top_auto
                                      : item->style.margin_bottom_auto;
                    if (auto_after) auto_margin_seen++;
                }
                item_index++;
            }
        }
    }
    if (column_flex && column_item_count != 0
        && style->flex_wrap && declared_height > 0) {
        if (!place_wrapped_flex_columns(
                context, node, style, column_order, flow->content_x, flow->content_width,
                column_flow_top, declared_height, column_item_count)) {
            goto out;
        }
    } else if (column_flex && column_item_count != 0) {
        int used_height = line->y - column_flow_top;
        int reverse_height = declared_height > 0
                             ? declared_height : used_height;
        reverse_flex_column_items(context, node, style, column_order,
                                  column_flow_top, reverse_height);
    }
    flow->natural_content_height = natural_content_height;
    flow->after_pseudo_flowed_inline = after_pseudo_flowed_inline;
    flow->trailing_margin = trailing_margin;
    flow->first_flow_content = first_flow_content;
    flow->flow_bottom_margin = flow_bottom_margin;
    ok = true;
out:
    flex_order_plan_destroy(column_order);
    return ok;
}

/* Decoration emission and back-patching live in
   src/layout_block_decoration.c. */


static bool layout_block_impl(LayoutContext *context, lxb_dom_node_t *node,
                              const ComputedStyle *parent, int x, int y,
                              int width, int containing_height,
                              bool assigned_width,
                              const PositionedBox *positioned_box,
                              int *bottom, LayoutBlockScratch *scratch,
                              FloatExclusion *float_output,
                              size_t float_output_capacity,
                              size_t *float_output_count)
{
    if (context->cancelled || !layout_cooperate(context, node)) return false;
    /* The caller's container decides whether this box inherits an enclosing
       link; this box is then the container for its own descendants. The
       wrapper restores the caller's value. */
    lxb_dom_node_t *link_container = context->link_container;
    context->link_container = node;
    size_t node_command_start = context->layout->count;
    size_t node_link_start = context->layout->link_count;
    size_t node_control_start = context->layout->control_count;
    size_t node_box_start = context->layout->node_box_count;
    ComputedStyle *style = &scratch->style;
    layout_style_for_node_into(context, node, parent, style);
    bool flex_or_grid_item = parent != NULL
        && (parent->display == DISPLAY_FLEX
            || parent->display == DISPLAY_INLINE_FLEX
            || parent->display == DISPLAY_GRID
            || parent->display == DISPLAY_INLINE_GRID);
    /* A hidden input has no CSS box even when its parent establishes flex or
       grid formatting.  Waiting until control painting is too late: its
       value text has already become a draw command by then. */
    if (layout_node_is_hidden_input(node)) {
        *bottom = y;
        return true;
    }
    /* Flex row/column follow the inline/block axes. The current text layout
       remains horizontal, but mapping the flex axis here gives vertical
       writing modes their correct physical sizing and ordering without
       changing the authored value exposed through CSSOM. */
    unsigned writing_mode = computed_style_writing_mode(style);
    if (writing_mode != STYLE_WRITING_HORIZONTAL_TB) {
        switch (style->flex_direction) {
        case FLEX_ROW:
            style->flex_direction = FLEX_COLUMN;
            break;
        case FLEX_ROW_REVERSE:
            style->flex_direction = FLEX_COLUMN_REVERSE;
            break;
        case FLEX_COLUMN:
            style->flex_direction =
                writing_mode == STYLE_WRITING_VERTICAL_RL
                    ? FLEX_ROW_REVERSE : FLEX_ROW;
            break;
        case FLEX_COLUMN_REVERSE:
            style->flex_direction =
                writing_mode == STYLE_WRITING_VERTICAL_RL
                    ? FLEX_ROW : FLEX_ROW_REVERSE;
            break;
        }
    }
    if (document_node_is_tag(node, LXB_TAG_TEXTAREA)
        && style->resize_mode != STYLE_RESIZE_NONE) {
        layout_block_apply_textarea_resize(node, style);
    }
    if (context->assigned_grid_node == node
        && context->assigned_grid_height_valid
        && !style->has_height
        && style->align_self != ALIGN_SELF_START
        && style->align_self != ALIGN_SELF_CENTER
        && style->align_self != ALIGN_SELF_END) {
        style->has_height = true;
        style->height_percent = false;
        style->height = context->assigned_grid_height;
    }
    if (context->assigned_flex_node == node
        && context->assigned_flex_height > 0
        && !context->assigned_flex_minimum) {
        style->has_height = true;
        style->height_percent = false;
        style->height = context->assigned_flex_height;
    }
    /* Percentage padding and margins refer to the width of the containing
       block. For an out-of-flow box that is its positioned ancestor's padding
       box (or the fixed containing block), not the flow width it was reached
       through. */
    int percentage_basis = width;
    bool assigned_basis = context->percentage_basis_node == node;
    if (assigned_basis) {
        percentage_basis = context->percentage_basis_width;
        context->percentage_basis_node = NULL;
    }
    if (style->fixed_position) {
        percentage_basis = positioned_box->fixed_node != NULL
            ? positioned_box->fixed_width : context->layout->width;
    } else if (style->out_of_flow) {
        percentage_basis = positioned_box->width;
    }
    resolve_padding(context->sheet, style, percentage_basis);
    resolve_margin(context->sheet, style, percentage_basis);
    /* Set only by layout_read_trace_environment, which TILEFINCH_NO_TRACE
       compiles out; the constant lets both class-trace blocks below compile
       out of shipping builds as well. */
    const char *trace_class = TILEFINCH_TRACE_COMPILED_IN != 0
        ? context->layout->trace_layout_class : NULL;
    if (trace_class != NULL && trace_class[0] != '\0') {
        size_t class_length = 0, parent_class_length = 0;
        const char *class_name = document_attribute(
            node, "class", &class_length);
        const char *parent_class = node->parent == NULL ? NULL
            : document_attribute(node->parent, "class",
                                 &parent_class_length);
        if (class_name != NULL
            && strstr(class_name, trace_class) != NULL) {
            fprintf(stderr,
                    "layout-class class=%.*s parent=%.*s display=%d "
                    "float=%d clear=%d flex=%d/%d/%u basis=%d/%d/%d "
                    "gap=%d x=%d y=%d width=%d assigned=%d "
                    "declared=%d/%d min=%d max=%d margin=%d/%d "
                    "padding=%d/%d overflow=%d/%d color=%06x\n",
                    (int) class_length, class_name,
                    (int) parent_class_length,
                    parent_class == NULL ? "" : parent_class,
                    style->display, style->float_mode, style->clear_mode,
                    style->flex_direction, style->flex_grow,
                    style->flex_shrink, style->has_flex_basis,
                    style->flex_basis_percent, style->flex_basis_offset,
                    style->gap,
                    x, y, width, assigned_width,
                    style->has_width, style->width, style->min_width,
                    style->max_width, style->margin.left, style->margin.right,
                    style->padding.left, style->padding.right,
                    style->overflow_x_scroll, style->overflow_y_scroll,
                    (unsigned) style->color);
        }
    }
    /* CSS Overflow propagates a block body's overflow to the viewport when
       the HTML root remains visible in both axes. The body's used overflow
       is visible in that case: it must contribute to the document scroller,
       not become a viewport-sized nested clip. */
    bool root_body_overflow_propagated = parent != NULL
        && document_node_is_tag(node, LXB_TAG_BODY) && node->parent != NULL
        && document_node_is_tag(node->parent, LXB_TAG_HTML)
        && !parent->overflow_x_scroll && !parent->overflow_y_scroll;
    if (root_body_overflow_propagated) {
        style->overflow_x_scroll = false;
        style->overflow_y_scroll = false;
        style->overflow_x_clip_only = false;
        style->overflow_y_clip_only = false;
    }
    if ((style->color_alpha < 255 || style->opacity < 255)
        && LAYOUT_TRACE(context->layout, PAINT)
        && context->trace_paint_lines++ < 64) {
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr, "layout-block-alpha color=%u opacity=%u id=%.*s "
                "class=%.*s\n", style->color_alpha, style->opacity,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name);
    }
    if (style->display == DISPLAY_NONE
        || style->display == DISPLAY_TABLE_COLUMN || style->hidden) {
        *bottom = y;
        return true;
    }
    bool positioned = style->out_of_flow || style->fixed_position;
    if (!positioned && layout_preview_limit_reached(context, y)) {
        *bottom = y;
        return true;
    }
    bool fixed_captured = style->fixed_position
        && positioned_box->fixed_node != NULL;
    int positioning_width = positioned ? percentage_basis
                                       : positioned_box->width;
    int viewport_height = context->layout->viewport.css_height > 0
                          ? context->layout->viewport.css_height : 272;
    int positioning_height = style->fixed_position
        ? (fixed_captured && positioned_box->fixed_height > 0
               ? positioned_box->fixed_height : viewport_height)
        : (positioned_box->height > 0 ? positioned_box->height
                                      : viewport_height);
    /* Percentage sizes of positioned boxes use their positioned containing
       block, not the normal-flow height passed through intermediate nodes.
       Flex-assigned parents can have a definite used height without an
       authored height on those intermediate flow paths. */
    if (positioned) containing_height = positioning_height;
    int resolved_left = layout_block_resolve_positioned_inset(
        context->sheet, style->left, style->inset_percent_mask,
        STYLE_INSET_LEFT_PERCENT, positioning_width);
    int resolved_right = layout_block_resolve_positioned_inset(
        context->sheet, style->right, style->inset_percent_mask,
        STYLE_INSET_RIGHT_PERCENT, positioning_width);
    int resolved_top = layout_block_resolve_positioned_inset(
        context->sheet, style->top, style->inset_percent_mask,
        STYLE_INSET_TOP_PERCENT, positioning_height);
    int resolved_bottom = layout_block_resolve_positioned_inset(
        context->sheet, style->bottom, style->inset_percent_mask,
        STYLE_INSET_BOTTOM_PERCENT, positioning_height);
    bool opposing_width = positioned && !style->has_width
                          && style->has_left && style->has_right;
    int opposing_outer_width = positioning_width - resolved_left
                               - resolved_right - style->margin.left
                               - style->margin.right;
    if (opposing_outer_width < 0) opposing_outer_width = 0;
    bool opposing_height = positioned && !style->has_height
                           && style->has_top && style->has_bottom;
    int opposing_outer_height = positioning_height - resolved_top
                                - resolved_bottom - style->margin.top
                                - style->margin.bottom;
    if (opposing_outer_height < 0) opposing_outer_height = 0;
    int exposed_top_margin = layout_block_collapsed_block_top_margin(
        context, node, style, collapse_walk_origin(context));
    int outer_y = layout_add_coordinate(y, exposed_top_margin);
    int margin_left = style->margin.left;
    int margin_right = style->margin.right;
    /* Out-of-flow percentages use the positioned containing block. A fixed
       child of a shrink-wrapped flex item is still viewport-wide at 100%. */
    int sizing_width = positioned ? positioning_width : width;
    int outer_width = sizing_width - margin_left - margin_right;
    int replaced_auto_width = assigned_width || positioned
        ? 0 : layout_block_replaced_auto_width(
                  context, node, style, outer_width, containing_height);
    if (opposing_width) {
        outer_width = opposing_outer_width;
    } else if (positioned && !assigned_width && !style->has_width) {
        /* CSS 2.1 gives an absolutely positioned, auto-width box the
           shrink-to-fit width.  The ordinary intrinsic helper deliberately
           ignores out-of-flow roots, so use the bounded root-inclusive
           variant while keeping positioned descendants excluded. */
        int intrinsic = intrinsic_positioned_width(
            context, node, parent, sizing_width);
        intrinsic -= margin_left + margin_right;
        if (intrinsic > 0 && intrinsic < outer_width) outer_width = intrinsic;
    } else if (!assigned_width && style->width_max_content) {
        bool minimum = computed_style_width_min_content(style);
        bool maximum = computed_style_width_max_content(style);
        int measure_limit = maximum ? LAYOUT_COORDINATE_LIMIT : sizing_width;
        int intrinsic = minimum
            ? intrinsic_min_text_width_ignoring_own_width(
                  context, node, parent, measure_limit)
            /* Positioned roots still own in-flow text. The ordinary
               intrinsic helper excludes them when measuring an ancestor. */
            : positioned
                ? intrinsic_positioned_width(
                      context, node, parent, measure_limit)
                : intrinsic_text_width(context, node, parent, measure_limit);
        intrinsic -= margin_left + margin_right;
        if (intrinsic < 0) intrinsic = 0;
        if (maximum) outer_width = intrinsic;
        else if (intrinsic < outer_width) outer_width = intrinsic;
    } else if (!assigned_width && style->display == DISPLAY_TABLE
               && !style->has_width) {
        /* The principal table box uses the automatic table algorithm; it is
           not an ordinary block that fills its containing block. Captions,
           cell widths, padding, and borders all contribute through the
           shared max-content measurement. */
        int intrinsic = table_intrinsic_width(
            context, node, style, sizing_width);
        intrinsic -= margin_left + margin_right;
        if (intrinsic > 0 && intrinsic < outer_width) outer_width = intrinsic;
    } else if (replaced_auto_width > 0) {
        outer_width = replaced_auto_width;
    } else if (!assigned_width && style->has_width) {
        int requested = resolve_declared_length(
            context->sheet, style->width, style->width_percent,
            sizing_width);
        int edges = style->padding.left + style->padding.right
                    + style->border.left + style->border.right;
        outer_width = style->box_sizing_border_box
                      ? requested : requested + edges;
    }
    bool has_maximum_width = false;
    outer_width = constrain_border_box_width(
        context, node, parent, style,
        assigned_basis ? percentage_basis : sizing_width, outer_width,
        &has_maximum_width);
    int free_margin = sizing_width - outer_width
                      - margin_left - margin_right;
    if (free_margin < 0) free_margin = 0;
    if (style->margin_left_auto && style->margin_right_auto) {
        margin_left += free_margin / 2;
    } else if (style->margin_left_auto) {
        margin_left += free_margin;
    }
    int outer_x = x + margin_left;
    if (outer_width < 8 && !assigned_width
        && !style->has_width && !has_maximum_width) {
        outer_width = 8;
    }

    if (document_node_is_tag(node, LXB_TAG_HR) && !style->has_height
        && style->border.top == 0 && style->border.right == 0
        && style->border.bottom == 0 && style->border.left == 0
        && !style->has_background) {
        return layout_block_hr_rule(
            context, node, parent, style, outer_x, outer_y, outer_width,
            node_command_start, node_link_start, node_control_start,
            node_box_start, flex_or_grid_item, bottom);
    }

    LayoutBlockPaintPlan paint_plan;
    if (!layout_block_emit_decoration(context, node, style, parent, outer_x, outer_y,
                                      outer_width, &paint_plan)) {
        return false;
    }
    size_t before_insertion_index = context->layout->count;
    size_t scroll_command_start = before_insertion_index;

    bool collapsed_table_cell =
        style->display == DISPLAY_TABLE_CELL
        && table_cell_uses_collapsed_geometry(context, node);
    int content_border_left = collapsed_table_cell
        ? (style->border.left + 1) / 2 : style->border.left;
    int content_border_right = collapsed_table_cell
        ? (style->border.right + 1) / 2 : style->border.right;
    int content_border_top = collapsed_table_cell
        ? (style->border.top + 1) / 2 : style->border.top;
    int content_border_bottom = collapsed_table_cell
        ? (style->border.bottom + 1) / 2 : style->border.bottom;
    int content_x = layout_add_coordinate(
        layout_add_coordinate(outer_x, content_border_left),
        style->padding.left);
    int content_width = outer_width - content_border_left
                        - content_border_right
                        - style->padding.left - style->padding.right;
    if (content_width < 0) content_width = 0;
    /* An auto block size laid out without the box's children: what a
       `size` container's queries read (size containment). -1: definite. */
    int contained_height = -1;
    if ((context->container_live || context->layout->retain_container_padding)
        && !opposing_height
        && !(style->has_height && style->height != STYLE_LENGTH_NONE
             && (!style->height_percent || containing_height > 0))) {
        contained_height = style_content_height(
            context->sheet, style, content_width, containing_height);
    }
    /* Before any descendant is styled: its container queries then answer
       against this box, not the previous pass's (style.h). */
    if (context->container_live) {
        style_container_live_update(
            (Stylesheet *) context->sheet, node, content_width,
            contained_height);
    }
    MulticolumnLayout multicolumn = multicolumn_prepare(
        context, node, style, content_width);
    if (context->cancelled) return false;
    style->gap = computed_style_resolve_gap(style->gap, content_width);
    const TableTracks *own_table_tracks = NULL;
    if (style->display == DISPLAY_TABLE && document_node_is_tag(node, LXB_TAG_TABLE)) {
        /* Resolve all flattened row-group ancestors while the table's own
           computed style is available.  A single row's parent style cannot
           represent sibling tbody/thead author overrides. */
        own_table_tracks = table_tracks_for_table(
            context, node, style, content_width);
    }
    int declared_content_height = opposing_height
        ? opposing_outer_height - style->padding.top - style->padding.bottom
          - content_border_top - content_border_bottom
        : style_content_height(context->sheet, style, content_width,
                               containing_height);
    if (declared_content_height < 0) declared_content_height = 0;
    bool content_visibility_hidden = style->content_visibility
        == STYLE_CONTENT_VISIBILITY_HIDDEN;
    if (content_visibility_hidden && !style->has_height) {
        int intrinsic = content_visibility_intrinsic_height(
            context->sheet, node);
        if (intrinsic > declared_content_height) {
            declared_content_height = intrinsic;
        }
    }
    int native_control_height = layout_control_default_height(node);
    if (!style->has_height && document_node_is_tag(node, LXB_TAG_INPUT)
        && native_control_height == 0) {
        /* Blockification does not remove the anonymous single-line value
           box. Match inline controls even when the value is empty or native
           appearance is disabled; padding/borders are added by normal flow. */
        int text_height = layout_fixed_ceil(computed_style_font_size_fixed(style));
        if (text_height < 1) text_height = 7 * style->font_scale;
        if (declared_content_height < text_height)
            declared_content_height = text_height;
    }
    int textarea_rows_height = style->has_height ? 0
        : layout_textarea_rows_content_height(context, node, style);
    if (textarea_rows_height > declared_content_height)
        declared_content_height = textarea_rows_height;
    if (!style->has_height && native_control_height > 0
        && textarea_rows_height == 0
        && (style->appearance & STYLE_APPEARANCE_MASK) != APPEARANCE_NONE) {
        int native_content_height = native_control_height
            - style->padding.top - style->padding.bottom
            - content_border_top - content_border_bottom;
        if (native_content_height < 1) native_content_height = 1;
        if (declared_content_height < native_content_height) {
            declared_content_height = native_content_height;
        }
    }
    bool definite_height = opposing_height || (style->has_height
        && style->height != STYLE_LENGTH_NONE
        && (!style->height_percent || containing_height > 0));
    style->row_gap = computed_style_resolve_gap(
        style->row_gap, definite_height ? declared_content_height : 0);
    int child_containing_height = definite_height
                                  ? declared_content_height : 0;
    /* Only an authored definite height propagates a percentage-height basis.
       Positioned descendants still use their separate initial containing
       block; an auto-height body is not itself the viewport. */
    PositionedBox *descendant_positioned_box =
        &scratch->descendant_positioned_box;
    *descendant_positioned_box = *positioned_box;
    if (style->relative_position || style->sticky_position
        || style->out_of_flow || style->fixed_position
        || layout_style_establishes_fixed_containing_block(style)) {
        descendant_positioned_box->node = node;
        descendant_positioned_box->x = outer_x + style->border.left;
        descendant_positioned_box->y = layout_add_coordinate(
            outer_y, content_border_top);
        descendant_positioned_box->width = outer_width - style->border.left
                                          - style->border.right;
        /* A definite min-height is already a valid lower bound for the
           padding-box containing block even while authored height remains
           auto. Publishing it here avoids incorrectly positioning a
           bottom-anchored asset against the viewport fallback. Normal-flow
           growth beyond that bound remains a later auto-height boundary. */
        descendant_positioned_box->height =
            definite_height || declared_content_height > 0
                ? declared_content_height + style->padding.top
                  + style->padding.bottom
                : 0;
        descendant_positioned_box->height_provisional = !definite_height;
        if (descendant_positioned_box->width < 0) {
            descendant_positioned_box->width = 0;
        }
    }
    if (layout_style_establishes_fixed_containing_block(style)) {
        descendant_positioned_box->fixed_node = node;
        descendant_positioned_box->fixed_x = outer_x + style->border.left;
        descendant_positioned_box->fixed_y = layout_add_coordinate(
            outer_y, content_border_top);
        descendant_positioned_box->fixed_width =
            outer_width - style->border.left - style->border.right;
        descendant_positioned_box->fixed_height =
            definite_height || declared_content_height > 0
                ? declared_content_height + style->padding.top
                  + style->padding.bottom
                : 0;
        if (descendant_positioned_box->fixed_width < 0) {
            descendant_positioned_box->fixed_width = 0;
        }
    }
    /* The prefixed clamp operates only in a vertical legacy box. Keeping
       the computed count on ordinary blocks is useful for CSSOM, but must
       not truncate their normally wrapping text. */
    bool clamped_vertical_box = computed_style_line_clamp(style) != 0
        && (style->display == DISPLAY_FLEX
            || style->display == DISPLAY_INLINE_FLEX)
        && (style->flex_direction == FLEX_COLUMN
            || style->flex_direction == FLEX_COLUMN_REVERSE);
    LineState *line = &scratch->line;
    *line = (LineState) {
        .start_x = content_x,
        .base_left = content_x,
        .x = content_x,
        .right = content_x + content_width,
        .base_right = content_x + content_width,
        .y = layout_add_coordinate(
            layout_add_coordinate(outer_y, content_border_top),
            style->padding.top),
        .line_gap = 0,
        .strut_fixed = layout_inline_style_line_height_fixed(context, style),
        .strut_baseline = layout_inline_strut_baseline(context, style, node),
        .atomics = &context->line_atomics,
        .atomic_base = context->line_atomics.count,
        .layout = context->layout,
        .command_start = context->layout->count,
        .link_start = context->layout->link_count,
        .control_start = context->layout->control_count,
        .node_box_start = context->layout->node_box_count,
        .text_align = computed_style_used_text_align(style),
        .direction_rtl = computed_style_direction_rtl(style),
        .inline_overflow_ellipsis = computed_style_text_overflow_ellipsis(style)
            && (computed_style_overflow_x_hidden(style)
                || style->overflow_x_clip_only),
        .clamp_limit = clamped_vertical_box
            ? computed_style_line_clamp(style) : 0,
        .positioned_box = *descendant_positioned_box,
        .containing_height = child_containing_height,
        .floats_enabled = true,
        .first_inline_block_collapses_top =
            !multicolumn.active && block_parent_collapses_top(style)
    };
    /* CSS 2.1 9.4.1/9.5: floats of an enclosing context never intrude
       into a formatting context root (the root itself was already placed
       beside them), so its lines start with no exclusions. Passing them in
       let unrelated earlier floats shift its content and use up the
       bounded active-float slots, dropping the root's own floats. */
    size_t inherited_float_count = block_contains_own_floats(style)
        ? 0 : positioned_box->float_count;
    if (inherited_float_count > ACTIVE_FLOAT_LIMIT) {
        inherited_float_count = ACTIVE_FLOAT_LIMIT;
    }
    /* A descendant with a negative margin can reach back into these floats.
       Retire exclusions only when a new float needs a bounded slot, not at
       an ordinary block boundary. Formatting roots still inherit none. */
    for (size_t i = 0; i < inherited_float_count; i++) {
        line->floats[i] = positioned_box->float_exclusions[i];
    }
    line->float_count = inherited_float_count;
    update_float_bounds(line);
    int text_indent = 0;
    if (!style_length_resolve(
            context->sheet, style->text_indent,
            content_width, &text_indent)) {
        text_indent = 0;
    }
    line->first_line_indent = text_indent;
    line->start_x += text_indent;
    line_cursor_set(line, line->start_x);
    if (context->document_bidi_text_present
        || context->document_bidi_markup_present
        || (context->sheet != NULL
            && context->sheet->has_bidi_declarations)) {
        scratch->bidi_flow = layout_bidi_flow_create(
            context, node, style, &line->bidi_pipeline_expected);
        line->bidi_flow = scratch->bidi_flow;
    }

    /*
     * Adjacent child margins collapse in a block formatting context even
     * when the container's own edge does not share those margins.  A table
     * cell, flow-root and inline-block collapse sibling margins internally,
     * but block_parent_collapses_{top,bottom}() still prevents the result
     * escaping through the container edge.
     */
    bool margin_collapse_context =
        !multicolumn.active
        && (style->display == DISPLAY_BLOCK
            || style->display == DISPLAY_FLOW_ROOT
            || style->display == DISPLAY_INLINE_BLOCK
            || style->display == DISPLAY_TABLE_CELL);
    bool first_flow_content = true;
    CollapsedMargin trailing_margin = {0};
    int flow_bottom_margin = style->margin.bottom;
    GeneratedPseudoFlow before_pseudo_flow = {0};
    GeneratedPseudoFlow after_pseudo_flow = {0};
    /* Generated content flows with the box's inline content in every
       block container, not only display:block -- an inline-block li's
       ::after separator belongs after its last line, not at the box
       origin (Minerva's hlist footers). */
    if (!content_visibility_hidden
        && ((style->display == DISPLAY_BLOCK
         || style->display == DISPLAY_FLOW_ROOT)
            || style->display == DISPLAY_INLINE_BLOCK
            || ((style->display == DISPLAY_FLEX
                 || style->display == DISPLAY_INLINE_FLEX)
                && style->flex_direction == FLEX_COLUMN))) {
        before_pseudo_flow = generated_pseudo_flow(
            context, node, style, PSEUDO_BEFORE,
            content_width, child_containing_height);
        after_pseudo_flow = generated_pseudo_flow(
            context, node, style, PSEUDO_AFTER,
            content_width, child_containing_height);
    }
    if (before_pseudo_flow.active || after_pseudo_flow.active) {
        /* Generated block fragments need their own fragmentation records.
           Keep this bounded path on ordinary direct block children. */
        multicolumn.active = false;
        line->first_inline_block_collapses_top =
            block_parent_collapses_top(style);
        margin_collapse_context =
            style->display == DISPLAY_BLOCK
            || style->display == DISPLAY_FLOW_ROOT
            || style->display == DISPLAY_INLINE_BLOCK
            || style->display == DISPLAY_TABLE_CELL;
    }
    if (before_pseudo_flow.active) {
        int reduction = 0;
        if (block_parent_collapses_top(style)) {
            collapsed_margin_reset(&trailing_margin,
                                   before_pseudo_flow.margin_top);
            reduction = before_pseudo_flow.margin_top;
        } else {
            collapsed_margin_reset(&trailing_margin,
                                   before_pseudo_flow.margin_top);
        }
        int pseudo_y = layout_add_coordinate(
            layout_subtract_coordinate(line->y, reduction),
            before_pseudo_flow.margin_top);
        pseudo_y = layout_block_clear_floats_y(
            line, before_pseudo_flow.clear_mode, pseudo_y);
        int pseudo_width = content_width - before_pseudo_flow.margin_left
                           - before_pseudo_flow.margin_right;
        if (pseudo_width < 0) pseudo_width = 0;
        if (!paint_pseudo(
                context, node, style, PSEUDO_BEFORE,
                content_x + before_pseudo_flow.margin_left, pseudo_y,
                pseudo_width, before_pseudo_flow.border_height,
                SIZE_MAX, before_pseudo_flow.border_height, NULL)) {
            return false;
        }
        line->y = layout_add_coordinate(
            layout_add_coordinate(
                pseudo_y, before_pseudo_flow.border_height),
            before_pseudo_flow.margin_bottom);
        if (before_pseudo_flow.border_height == 0
            && !before_pseudo_flow.formatting_root) {
            line->y = layout_subtract_coordinate(
                line->y, collapsed_margin_add(
                    &trailing_margin, before_pseudo_flow.margin_bottom));
        } else {
            collapsed_margin_reset(&trailing_margin,
                                   before_pseudo_flow.margin_bottom);
        }
        first_flow_content = false;
    }
    bool generated_flex_row = (style->display == DISPLAY_FLEX
                              || style->display == DISPLAY_INLINE_FLEX)
        && (style->flex_direction == FLEX_ROW
            || style->flex_direction == FLEX_ROW_REVERSE);
    bool before_flex_item = generated_flex_row && layout_generated_flex_in_flow(
        context, node, style, PSEUDO_BEFORE);
    bool after_flex_item = generated_flex_row && layout_generated_flex_in_flow(
        context, node, style, PSEUDO_AFTER);
    bool before_pseudo_flowed_inline = before_flex_item;
    if (!content_visibility_hidden && !before_flex_item && !before_pseudo_flow.active
        && !flow_generated_inline_pseudo(
            context, node, style, PSEUDO_BEFORE, line, NULL, 0, NULL,
            &before_pseudo_flowed_inline)) {
        return false;
    }
    if (before_pseudo_flowed_inline) first_flow_content = false;

    bool replaced_image = false;
    if ((style->generated_image || layout_replaced_element_tag(node))
        && !layout_block_replaced_section(
               context, node, parent, style, line, content_x, content_width,
               containing_height, paint_plan.border_radius_code,
               &replaced_image)) {
        return false;
    }

    bool replaced_mask = false;
    if ((paint_plan.element_mask != NULL || paint_plan.mask_layer != NULL)
        && !layout_block_mask_section(
               context, node, style, line, content_x, content_width,
               containing_height, &paint_plan, &replaced_mask)) {
        return false;
    }
    bool missing_image_alt = false;
    if (document_node_is_tag(node, LXB_TAG_IMG) && !replaced_image
        && !layout_block_missing_image_alt(
               context, node, style, line, &missing_image_alt)) {
        return false;
    }
    bool replaced_content = style->generated_image || replaced_image || replaced_mask
        || missing_image_alt
        || document_node_is_tag(node, LXB_TAG_TEXTAREA)
        || content_visibility_hidden;

    if (!replaced_content) {
        size_t placeholder_length = 0;
        const char *placeholder = document_attribute(
            node, "data-placeholder", &placeholder_length);
        if (placeholder != NULL && placeholder_length != 0
            && !flow_text(context, line, placeholder, placeholder_length,
                          style, NULL, 0, NULL)) return false;
    }

    /* CSS renders markers only on display:list-item boxes; an li
       restyled to block, inline-block, flex, etc. loses its marker (tab
       bars, Wikipedia's menu drawer rows). */
    if (document_node_is_tag(node, LXB_TAG_LI) && !style->list_style_none
        && style->display == DISPLAY_BLOCK && style->list_item
        && !layout_block_list_marker(context, node, style, line, content_x)) {
        return false;
    }

    bool table_row = style->display == DISPLAY_TABLE_ROW;
    /* WebKit's multiline clamp convention uses a vertical legacy box, but
       its contents still form ordinary inline lines.  The compatibility
       parser maps that box to flex for CSSOM and non-clamped layouts; select
       block flow here so the existing bounded ellipsis stage owns the text. */
    bool flex_container = !clamped_vertical_box
        && (style->display == DISPLAY_FLEX
            || style->display == DISPLAY_INLINE_FLEX);
    bool grid = style->display == DISPLAY_GRID
                || style->display == DISPLAY_INLINE_GRID;
    bool css_table_row = false;
    bool anonymous_cell_row = false;
    if (!table_row && !flex_container && !grid && !replaced_content) {
        size_t cell_count = 0;
        bool have_explicit_row = false;
        bool only_table_cells = true;
        /* Outside a CSS table (display:table on a non-table element) only
           an all-cell run matters, so the first other child settles it:
           an ordinary block must not resolve every child's style twice
           (100+ references in one list were a 0.1 s stall on the PSP). */
        bool css_table_container = style->display == DISPLAY_TABLE
            && !document_node_is_tag(node, LXB_TAG_TABLE);
        FlatItemIterator *cell_scan = &scratch->traversal.flat.iterator;
        FlatItem *cell = &scratch->traversal.flat.item;
        flat_iterator_init(cell_scan, context, node, style);
        while (flat_iterator_next(cell_scan, cell)) {
            if (cell->style.out_of_flow || cell->style.fixed_position) {
                continue;
            }
            if (!cell->anonymous_text
                && cell->style.display == DISPLAY_TABLE_ROW) {
                have_explicit_row = true;
                break;
            }
            if (cell->anonymous_text
                || cell->style.display != DISPLAY_TABLE_CELL) {
                only_table_cells = false;
                if (!css_table_container) break;
            }
            cell_count++;
        }
        /* CSS table fix-up wraps consecutive improper children in one
           anonymous row, and each non-cell child in an anonymous cell. A
           run of table-cell children under an ordinary block also acquires
           anonymous table and row wrappers; when the run occupies the
           whole container, the used row layout is identical and needs no
           retained wrapper boxes. */
        bool anonymous_cells = only_table_cells && cell_count != 0;
        anonymous_cell_row =
            anonymous_cells && style->display != DISPLAY_TABLE;
        bool anonymous_row_in_css_table =
            style->display == DISPLAY_TABLE
            && !document_node_is_tag(node, LXB_TAG_TABLE);
        css_table_row = !have_explicit_row && cell_count != 0
                        && (anonymous_cells
                            || anonymous_row_in_css_table);
    }
    bool flex_row = (flex_container
                     && (style->flex_direction == FLEX_ROW
                         || style->flex_direction == FLEX_ROW_REVERSE))
                    || table_row || css_table_row;
    bool reverse_row = style->flex_direction == FLEX_ROW_REVERSE;
    /* These formatting contexts now live in separate translation units.
       Keep their admission predicates at the call seam: otherwise every
       ordinary block pays both calls, and the grid callee reserves a 2.4 KiB
       frame and resolves grid metadata before reaching its internal `grid`
       condition. Construct the transfer frame here too: filling seventeen
       fields for every ordinary article block was unnecessary refactor-only
       work even after the calls themselves were gated. */
    if (grid || flex_row) {
        LayoutBlockFrame frame = {
            .node = node,
            .style = style,
            .scratch = scratch,
            .line = line,
            .descendant_positioned_box = descendant_positioned_box,
            .content_x = content_x,
            .content_width = content_width,
            .child_containing_height = child_containing_height,
            .declared_content_height = declared_content_height,
            .definite_height = definite_height,
            /* A definite min-height constrains the grid's available block
               space even though it does not make descendant percentage
               heights definite. Flex tracks may consume that floor; ordinary
               percentage tracks retain the authored-height rule. */
            .grid_minimum_block_size = grid
                && !style->has_height && declared_content_height > 0
                && style->min_height > 0
                && (!style->min_height_percent || containing_height > 0),
            .replaced_content = replaced_content,
            .grid = grid,
            .flex_row = flex_row,
            .reverse_row = reverse_row,
            .table_row = table_row,
            .css_table_row = css_table_row,
            .anonymous_cell_row = anonymous_cell_row
        };
        if (grid && !layout_block_grid_section(context, &frame)) return false;
        if (flex_row && !layout_block_flexrow_section(context, &frame)) {
            return false;
        }
    }
    if (flex_row && !replaced_content) {
        /* An out-of-flow flex child is laid out after the in-flow items have
           established the container's used cross size.  Percentage heights
           otherwise see an indefinite zero basis when the flex container's
           height is auto, collapsing common full-bleed hero assets even
           though their containing block has a definite used padding box. */
        PositionedBox used_positioned_box = *descendant_positioned_box;
        int used_padding_bottom = line->y;
        int declared_padding_bottom = layout_add_coordinate(
            used_positioned_box.y,
            declared_content_height + style->padding.top
            + style->padding.bottom);
        if (declared_padding_bottom > used_padding_bottom) {
            used_padding_bottom = declared_padding_bottom;
        }
        used_positioned_box.height = layout_subtract_coordinate(
            used_padding_bottom, used_positioned_box.y);
        if (used_positioned_box.height < 0) used_positioned_box.height = 0;
        if (used_positioned_box.node == node)
            used_positioned_box.height_provisional = false;

        FlatItemIterator *iterator = &scratch->traversal.flat.iterator;
        FlatItem *item = &scratch->traversal.flat.item;
        flat_iterator_init(iterator, context, node, style);
        while (flat_iterator_next(iterator, item)) {
            if (item->anonymous_text
                || !(item->style.out_of_flow || item->style.fixed_position)) {
                continue;
            }
            int positioned_bottom = used_positioned_box.y;
            if (!layout_block(
                    context, item->node, &item->parent_style,
                    used_positioned_box.x, used_positioned_box.y,
                    used_positioned_box.width, used_positioned_box.height,
                    false, &used_positioned_box, &positioned_bottom)) {
                return false;
            }
            if (item->style.fixed_position) continue;
            const LayoutNodeBox *positioned_box_for_item =
                layout_box_for_node(context->layout, item->node);
            if (positioned_box_for_item == NULL) continue;
            int target_x = positioned_box_for_item->x;
            int target_y = positioned_box_for_item->y;
            if (!item->style.has_left && !item->style.has_right) {
                if (style->justify_content == JUSTIFY_CENTER
                    || style->justify_content == JUSTIFY_SPACE_AROUND
                    || style->justify_content == JUSTIFY_SPACE_EVENLY) {
                    target_x = used_positioned_box.x
                        + (used_positioned_box.width
                           - positioned_box_for_item->width) / 2;
                } else if (style->justify_content == JUSTIFY_END) {
                    target_x = used_positioned_box.x
                        + used_positioned_box.width
                        - positioned_box_for_item->width;
                }
            }
            bool static_cross_center =
                item->style.align_self == ALIGN_SELF_CENTER
                || ((item->style.align_self == ALIGN_SELF_AUTO
                     || item->style.align_self == ALIGN_SELF_STRETCH)
                    && style->align_items == ALIGN_CENTER);
            bool static_cross_end =
                item->style.align_self == ALIGN_SELF_END
                || ((item->style.align_self == ALIGN_SELF_AUTO
                     || item->style.align_self == ALIGN_SELF_STRETCH)
                    && style->align_items == ALIGN_END);
            if (!item->style.has_top && !item->style.has_bottom) {
                if (static_cross_center) {
                    target_y = used_positioned_box.y
                        + (used_positioned_box.height
                           - positioned_box_for_item->height) / 2;
                } else if (static_cross_end) {
                    target_y = used_positioned_box.y
                        + used_positioned_box.height
                        - positioned_box_for_item->height;
                }
            }
            int dx = target_x - positioned_box_for_item->x;
            int dy = target_y - positioned_box_for_item->y;
            if (dx != 0 || dy != 0) {
                translate_node_subtree(context->layout, item->node, dx, dy);
            }
        }
    }
    LayoutBlockFlowState flow = {
        .node = node,
        .style = style,
        .positioned_box = positioned_box,
        .scratch = scratch,
        .line = line,
        .descendant_positioned_box = descendant_positioned_box,
        .multicolumn = &multicolumn,
        .after_pseudo_flow = &after_pseudo_flow,
        .own_table_tracks = own_table_tracks,
        .content_x = content_x,
        .content_width = content_width,
        .containing_height = containing_height,
        .child_containing_height = child_containing_height,
        .declared_content_height = declared_content_height,
        .outer_y = outer_y,
        .content_border_top = content_border_top,
        .content_border_bottom = content_border_bottom,
        .node_box_start = node_box_start,
        .node_link_start = node_link_start,
        .node_control_start = node_control_start,
        .scroll_command_start = scroll_command_start,
        .definite_height = definite_height,
        .clamped_vertical_box = clamped_vertical_box,
        .flex_container = flex_container,
        .flex_row = flex_row,
        .grid = grid,
        .replaced_content = replaced_content,
        .margin_collapse_context = margin_collapse_context,
        .after_flex_item = after_flex_item,
        .trailing_margin = trailing_margin,
        .first_flow_content = first_flow_content,
        .flow_bottom_margin = flow_bottom_margin
    };
    if (!layout_block_flow_children(context, &flow)) return false;
    flow_bottom_margin = flow.flow_bottom_margin;
    int natural_content_height = flow.natural_content_height;
    bool after_pseudo_flowed_inline = flow.after_pseudo_flowed_inline;
    size_t scroll_command_end = 0;
    int declared_height = declared_content_height;
    /* Collapsing an empty descendant's start and end margins may move the
       flow cursor back to the parent's content edge.  It must never move it
       above that edge: an auto-height block can have a zero-height border
       box, but not a negative one. */
    int minimum_content_bottom = layout_add_coordinate(
        layout_add_coordinate(outer_y, content_border_top),
        style->padding.top);
    if (line->y < minimum_content_bottom) {
        line->y = minimum_content_bottom;
    }
    int declared_content_bottom = layout_add_coordinate(
        minimum_content_bottom, declared_height);
    int declared_bottom = layout_add_coordinate(
        layout_add_coordinate(declared_content_bottom,
                              style->padding.bottom),
        content_border_bottom);
    bool table_minimum_height = style->display == DISPLAY_TABLE_ROW
                                || style->display == DISPLAY_TABLE_CELL
                                || (context->assigned_flex_node == node
                                    && context->assigned_flex_minimum)
                                || (context->assigned_grid_node == node
                                    && context->assigned_grid_minimum);
    if (definite_height && declared_height >= 0
        && (!table_minimum_height || line->y < declared_content_bottom)) {
        /* CSS table row/cell heights are minimums: content is allowed to grow
           the box.  Ordinary block heights continue to establish a fixed
           content box whose visible overflow does not change normal flow. */
        line->y = declared_content_bottom;
    } else if (declared_height > 0
               && layout_add_coordinate(
                      layout_add_coordinate(line->y, style->padding.bottom),
                      content_border_bottom) < declared_bottom) {
        line->y = declared_content_bottom;
    }
    int content_bottom = layout_add_coordinate(
        layout_add_coordinate(line->y, style->padding.bottom),
        content_border_bottom);
    int painted_content_bottom = content_bottom;
    for (size_t i = node_command_start; i < context->layout->count; i++) {
        if (layout_command_is_fixed_descendant(context->layout, i)) continue;
        /* A box-shadow is ink overflow only: it never extends scrollable
           overflow (CSS Backgrounds 3 7.1, CSS Overflow 3 2.2). Counting it
           let weather.gov's shadowed <main> scroll 6px of body past its
           bottom edge. */
        if (context->layout->commands[i].type == DRAW_SHADOW_RECT) continue;
        int command_bottom = layout_add_coordinate(
            context->layout->commands[i].y,
            context->layout->commands[i].height);
        if (command_bottom > painted_content_bottom) {
            painted_content_bottom = command_bottom;
        }
    }
    for (size_t i = node_box_start;
         i < context->layout->node_box_count; i++) {
        const LayoutNodeBox *child_box = &context->layout->node_boxes[i];
        if (child_box->command_start < node_command_start
            || child_box->command_end > context->layout->count
            || child_box->positioned_ancestor_distance == UINT8_MAX
            || layout_command_is_fixed_descendant(
                   context->layout, child_box->command_start)) {
            continue;
        }
        int child_bottom = layout_add_coordinate(
            child_box->y, child_box->content_height);
        if (child_bottom > painted_content_bottom) {
            painted_content_bottom = child_bottom;
        }
    }
    /* Unrepresentable or synthetic motion-scene heights: the cold arm caps
       the box (layout_block_contain_motion_scene). */
    int scene_limit = layout_clamp_coordinate(
        (int64_t) viewport_height * 64);
    bool synthetic_motion_scene = context->sheet != NULL
        && context->sheet->has_motion_keyframes
        /* Out-of-flow spacers establish virtual scroll ranges without
           pushing later document content away. Unrelated keyframes do not
           make those ranges unsupported motion scenes. Saturation below
           still bounds genuinely unrepresentable coordinates. */
        && !positioned
        && declared_height > scene_limit
        && natural_content_height < viewport_height * 4;
    /* A clipped descendant can still have commands at its pre-containment
       coordinates. The root overflow walk sees those retained commands, so
       it must close the final escape hatch too; otherwise nested scenes are
       individually bounded while the document still reports the sentinel. */
    bool coordinate_contained = (synthetic_motion_scene
                                 || painted_content_bottom
                                    >= LAYOUT_COORDINATE_LIMIT)
        && layout_block_contain_motion_scene(
               context, node, style, line, outer_y, node_command_start,
               viewport_height, natural_content_height,
               synthetic_motion_scene, content_border_bottom,
               &content_bottom, &painted_content_bottom);
    if (coordinate_contained) flow_bottom_margin = 0;
    int border_height = content_bottom - outer_y;
    if (!layout_block_patch_decoration(
            context, node, style, &paint_plan, outer_x, outer_y, outer_width,
            content_bottom, border_height, collapsed_table_cell,
            &before_insertion_index, &scroll_command_start)) {
        return false;
    }
    /* Generated inline content belongs to the element's scrollable content.
       Paint it after the decorations so the single content range remains
       contiguous; the batch insertion fixes every retained command range
       once regardless of how many square border sides the box has. */
    if ((!before_pseudo_flow.active && !before_pseudo_flowed_inline
         && !paint_pseudo(context, node, style, PSEUDO_BEFORE,
                      outer_x, outer_y, outer_width, border_height,
                      before_insertion_index, 0, descendant_positioned_box))
        || (!after_pseudo_flow.active && !after_pseudo_flowed_inline
            && !paint_pseudo(context, node, style, PSEUDO_AFTER,
                         outer_x, outer_y, outer_width, border_height,
                         SIZE_MAX, 0, descendant_positioned_box))) {
        return false;
    }
    scroll_command_end = context->layout->count;
    unsigned outline_width = computed_style_outline_width(style);
    unsigned outline_style = computed_style_outline_style(style);
    if (outline_width != 0 && outline_style != STYLE_OUTLINE_NONE
        && border_height > 0) {
        int offset = computed_style_outline_offset(style);
        int outset = offset + (int) outline_width;
        DrawCommand outline = {
            .type = DRAW_STROKE_RECT,
            .x = outer_x - outset,
            .y = outer_y - outset,
            .width = outer_width + 2 * outset,
            .height = border_height + 2 * outset,
            .color = (style->outline_state
                      & STYLE_OUTLINE_CURRENT_COLOR) != 0
                     ? style->color : style->outline_color,
            .scale = (int) outline_width,
            .radius = style_border_radius_adjust(
                paint_plan.border_radius_code, outset),
            .opacity_scale = alpha_opacity_scale(
                (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0
                ? style->color_alpha : style->outline_alpha),
            .image_fit = outline_style == STYLE_OUTLINE_DASHED
                ? LAYOUT_STROKE_DASHED
                : (outline_style == STYLE_OUTLINE_DOTTED
                   ? LAYOUT_STROKE_DOTTED : LAYOUT_STROKE_SOLID)
        };
        if (outline.width > 0 && outline.height > 0
            && layout_add_command(context->layout, outline) == NULL) {
            return false;
        }
    }
    if (layout_block_may_paint_control(context, node)
        && !layout_block_control_paint(context, node, style, outer_x,
                                       outer_y, outer_width, border_height,
                                       content_bottom)) {
        return false;
    }
    {
        size_t href_length = 0;
        const char *href = NULL;
        lxb_dom_node_t *link_node = node;
        if (document_node_is_tag(node, LXB_TAG_A)) {
            href = document_attribute(node, "href", &href_length);
        } else {
            (void) layout_container_link(node, link_container, &href,
                                         &href_length, &link_node);
        }
        if (href != NULL && href_length != 0) {
            DrawCommand link_box = {
                .x = outer_x, .y = outer_y,
                .width = outer_width, .height = border_height,
                .z_index = style->has_z_index ? style->z_index : 0
            };
            if (!layout_add_link(
                    context->layout, &link_box, SIZE_MAX,
                    href, href_length, link_node)) return false;
        }
    }
    /* painted_content_bottom already includes the box's end padding.  Adding
       it again creates a false scroll tail for padded root descendants. */
    /* Scrollable overflow is measured from the padding edge, not the
       outer border edge. The normal-flow bottom includes our own bottom
       border; overflowing descendants do not. */
    int content_height = painted_content_bottom - outer_y - content_border_top;
    if (painted_content_bottom == content_bottom)
        content_height -= content_border_bottom;
    int client_width = outer_width - content_border_left
                       - content_border_right;
    int client_height = border_height - content_border_top
                        - content_border_bottom;
    if (client_width < 0) client_width = 0;
    if (client_height < 0) client_height = 0;
    if (content_height < client_height) content_height = client_height;
    int scroll_width = client_width;
    /* Node boxes are appended in DOM post-order. Scan them backwards so a
       clipping descendant is seen before its children and can suppress that
       child's otherwise-visible overflow from the ancestor scroll extent.
       One command-range floor is sufficient because post-order subtree
       ranges are nested or disjoint, never interleaved. */
    size_t clipped_subtree_start = SIZE_MAX;
    for (size_t i = context->layout->node_box_count;
         i-- > node_box_start;) {
        const LayoutNodeBox *child_box = &context->layout->node_boxes[i];
        if (child_box->command_start < scroll_command_start
            || child_box->command_end > scroll_command_end
            || child_box->positioned_ancestor_distance == UINT8_MAX
            || layout_command_is_fixed_descendant(
                   context->layout, child_box->command_start)) {
            continue;
        }
        if (clipped_subtree_start != SIZE_MAX) {
            if (child_box->command_start >= clipped_subtree_start) continue;
            clipped_subtree_start = SIZE_MAX;
        }
        int child_width = child_box->clips_x
                          ? child_box->width : child_box->content_width;
        int right = child_box->x + child_width - outer_x
                    + style->padding.right;
        if (right > scroll_width) scroll_width = right;
        if (child_box->clips_x) {
            clipped_subtree_start = child_box->command_start;
        }
    }
    uint8_t positioned_ancestor_distance = 0;
    if (style->out_of_flow || style->fixed_position) {
        positioned_ancestor_distance = UINT8_MAX;
        lxb_dom_node_t *containing_node = style->fixed_position
            ? positioned_box->fixed_node : positioned_box->node;
        if (containing_node != NULL) {
            unsigned distance = 1;
            for (lxb_dom_node_t *ancestor = node->parent;
                 ancestor != NULL && distance < UINT8_MAX;
                 ancestor = ancestor->parent, distance++) {
                if (ancestor == containing_node) {
                    positioned_ancestor_distance = (uint8_t) distance;
                    break;
                }
            }
        }
    }
    StyleOverflowClipBox overflow_clip_box =
        computed_style_overflow_clip_box(style);
    int overflow_clip_inset_left =
        overflow_clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX ? 0
        : style->border.left
          + (overflow_clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
             ? style->padding.left : 0);
    int overflow_clip_inset_top =
        overflow_clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX ? 0
        : style->border.top
          + (overflow_clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
             ? style->padding.top : 0);
    bool clips_x = style->overflow_x_scroll;
    bool clips_y = style->overflow_y_scroll || coordinate_contained;
    int clip_radius = paint_plan.border_radius_code;
    unsigned clip_path_type = computed_style_clip_path_type(style);
    if (clip_path_type != STYLE_CLIP_PATH_NONE) {
        clips_x = true;
        clips_y = true;
        unsigned clip_path_inset = computed_style_clip_path_inset(style);
        int inset = (style->clip_path_state
                     & STYLE_CLIP_PATH_INSET_PERCENT) != 0
            ? ((outer_width < border_height ? outer_width : border_height)
               * (int) clip_path_inset / 100)
            : (int) clip_path_inset;
        if (clip_path_type == STYLE_CLIP_PATH_CIRCLE) inset = 0;
        if (inset > overflow_clip_inset_left) {
            overflow_clip_inset_left = inset;
        }
        if (inset > overflow_clip_inset_top) {
            overflow_clip_inset_top = inset;
        }
        int path_width = outer_width - 2 * inset;
        int path_height = border_height - 2 * inset;
        int path_radius = clip_path_type == STYLE_CLIP_PATH_CIRCLE
            ? (path_width < path_height ? path_width : path_height) / 2
            : (int) computed_style_clip_path_radius(style);
        if (path_radius > style_border_radius_maximum(clip_radius)) {
            clip_radius = path_radius;
        }
    }
    if (context->positioned_height_job_count != 0
        && !finish_positioned_heights(context, node, descendant_positioned_box,
            border_height - style->border.top - style->border.bottom)) return false;
    if (!add_node_box(context->layout, node, outer_x, outer_y, outer_width,
                      border_height, client_width, client_height,
                      scroll_width, content_height,
                      style->padding.left + style->padding.right,
                      style->padding.top + style->padding.bottom,
                      clips_x, clips_y,
                      clip_radius, style->overflow_clip_margin,
                      overflow_clip_inset_left, overflow_clip_inset_top,
                      style->overflow_x_clip_only,
                      style->overflow_y_clip_only,
                      positioned_ancestor_distance,
                      positioned_ancestor_distance == 0,
                      node_command_start, context->layout->count,
                      scroll_command_start, scroll_command_end,
                      node_link_start, context->layout->link_count,
                      node_control_start, context->layout->control_count)) {
        return false;
    }
    layout_note_contained_height(context->layout, node, contained_height);
    for (size_t i = 0; i < context->bottom_fixup_count;) {
        if (context->bottom_fixups[i].containing_node != node) {
            i++;
            continue;
        }
        int padding_bottom = layout_subtract_coordinate(
            layout_add_coordinate(outer_y, border_height),
            style->border.bottom);
        int dy = padding_bottom - context->bottom_fixups[i].assumed_bottom;
        if (dy != 0) {
            translate_node_subtree(context->layout,
                                   context->bottom_fixups[i].node, 0, dy);
        }
        context->bottom_fixups[i] =
            context->bottom_fixups[--context->bottom_fixup_count];
    }
    if (clip_path_type != STYLE_CLIP_PATH_NONE) {
        LayoutNodeBox *clip_box = layout_box_for_node_mutable(
            context->layout, node);
        if (clip_box != NULL) clip_box->clip_path_clip = 1;
        /* clip-path is not a containing block for fixed descendants, but it
           still clips them (CSS Masking 5.1). A viewport-fixed descendant
           therefore shows only where this box's clip region is on screen,
           and is retired once that region has scrolled away. A fixed clip
           owner moves with its descendants, which the ordinary overflow
           geometry of a fixed range already handles. */
        if (clip_box != NULL && !style->fixed_position) {
            int clip_bottom = layout_add_coordinate(
                clip_box->y,
                clip_box->height - (int) clip_box->clip_inset_top);
            for (size_t i = 0; i < context->layout->fixed_count; i++) {
                FixedRange *range = &context->layout->fixed_ranges[i];
                if (range->command_start < node_command_start
                    || range->command_end > context->layout->count
                    || range->clip_node != NULL) continue;
                range->clip_node = node;
                if (clip_bottom < range->scroll_end) {
                    range->scroll_end = clip_bottom;
                }
            }
        }
    }
    if (parent != NULL
        && style->visibility_hidden != parent->visibility_hidden
        && !layout_record_visibility_range(
            context, node_command_start, node_link_start,
            node_control_start, style->visibility_hidden)) {
        return false;
    }
    if (trace_class != NULL && trace_class[0] != '\0') {
        size_t class_length = 0;
        const char *class_name = document_attribute(
            node, "class", &class_length);
        if (class_name != NULL && strstr(class_name, trace_class) != NULL) {
            fprintf(stderr,
                    "layout-class-box class=%.*s x=%d y=%d width=%d "
                    "height=%d content=%d/%d bottom=%d font=%d/%d "
                    "margin=%d/%d/%d/%d padding=%d/%d/%d/%d\n",
                    (int) class_length, class_name, outer_x, outer_y,
                    outer_width, border_height, scroll_width, content_height,
                    content_bottom + flow_bottom_margin,
                    style->font_size, style->line_height,
                    style->margin.top, style->margin.right,
                    style->margin.bottom, style->margin.left,
                    style->padding.top, style->padding.right,
                    style->padding.bottom, style->padding.left);
        }
    }
    if (!apply_visual_range(context, node, node_command_start,
                            node_link_start, node_control_start, style,
                            flex_or_grid_item)) {
        return false;
    }
    if (style->has_transform) {
        layout_block_apply_transform(
            context, node, style, outer_x, outer_y, outer_width,
            border_height, node_command_start, node_link_start,
            node_control_start, node_box_start);
    }
    /* Sticky descendants are constrained by their first enclosing block.
       Their ranges are registered in post-order, so the current node is the
       first finalized containing box that can publish the lower boundary.
       Bind before registering this node's own sticky range. */
    const LayoutNodeBox *sticky_containing_box = layout_box_for_node(
        context->layout, node);
    if (sticky_containing_box != NULL) {
        layout_bound_sticky_ranges(
            context->layout, node_command_start, context->layout->count,
            layout_add_coordinate(sticky_containing_box->y,
                                  sticky_containing_box->height));
    }
    if (style->out_of_flow || style->relative_position
        || style->sticky_position || style->fixed_position) {
        LayoutBlockPositionedResult placed = layout_block_finalize_positioned(
            context, node, style, positioned_box, outer_x, outer_y,
            outer_width, border_height, content_bottom, positioning_height,
            resolved_left, resolved_right, resolved_top, resolved_bottom,
            fixed_captured, node_command_start, node_link_start,
            node_control_start, node_box_start);
        if (placed == LAYOUT_BLOCK_POSITIONED_FAILED) return false;
        if (placed == LAYOUT_BLOCK_POSITIONED_OUT_OF_FLOW) {
            *bottom = y;
            return true;
        }
    }
    if (float_output != NULL && float_output_count != NULL
        && float_output_capacity != 0
        && style->display == DISPLAY_BLOCK
        && style->float_mode == FLOAT_NONE
        && !style->out_of_flow && !style->fixed_position
        && !block_establishes_formatting_context(style)) {
        size_t emitted = 0;
        for (size_t i = 0; i < line->float_count
                           && emitted < float_output_capacity; i++) {
            line->layout->performance.float_exclusion_probes++;
            const FloatExclusion *candidate = &line->floats[i];
            bool inherited = false;
            if (positioned_box != NULL) {
                for (size_t j = 0; j < positioned_box->float_count; j++) {
                    const FloatExclusion *existing =
                        &positioned_box->float_exclusions[j];
                    if (candidate->x == existing->x
                        && candidate->right == existing->right
                        && candidate->top == existing->top
                        && candidate->bottom == existing->bottom
                        && candidate->side == existing->side) {
                        inherited = true;
                        break;
                    }
                }
            }
            if (!inherited) float_output[emitted++] = *candidate;
        }
        *float_output_count = emitted;
    }
    *bottom = layout_add_coordinate(content_bottom, flow_bottom_margin);
    return true;
}
