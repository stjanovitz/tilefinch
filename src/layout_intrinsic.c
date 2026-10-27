/* Intrinsic sizing: max-content and min-content widths, their per-pass
   cache, replaced-element natural widths, and grid intrinsic track widths.
   Split out of layout_flex.c; block, float, table, grid, inline and flex
   layout all measure through it. */

#include "layout_internal.h"

#include <ctype.h>
#include <string.h>

static int intrinsic_text_width_impl(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *parent, int limit,
                                     bool include_positioned_root);

int grid_item_minimum_contribution(
    LayoutContext *context, const FlatItem *item, int available,
    int start, int span, const uint8_t track_types[GRID_TRACK_LIMIT])
{
    if (context == NULL || item == NULL || available <= 0) return 0;
    ComputedStyle resolved = item->style;
    resolve_padding(context->sheet, &resolved, available);
    resolve_margin(context->sheet, &resolved, available);
    if (resolved.min_width_auto) {
        bool scrollable = resolved.overflow_x_scroll
                          && !resolved.overflow_x_clip_only;
        bool spans_flexible_track = false;
        if (span > 1 && track_types != NULL) {
            for (int offset = 0; offset < span; offset++) {
                int track = start + offset;
                if (track >= 0 && track < GRID_TRACK_LIMIT
                    && track_types[track] == GRID_TRACK_FLEX) {
                    spans_flexible_track = true;
                    break;
                }
            }
        }
        /* Grid's automatic minimum is content-based only for a
           non-scrollable item that does not span multiple tracks including
           a flexible one. In every other case auto resolves to zero. */
        if (scrollable || spans_flexible_track) return 0;
        return intrinsic_min_text_width(
            context, item->node, &item->parent_style, available);
    }
    int margins = resolved.margin.left + resolved.margin.right;
    int minimum = constrain_border_box_width(
        context, item->node, &item->parent_style, &resolved,
        available, 0, NULL) + margins;
    return minimum > 0 ? minimum : 0;
}

/*
 * Collect the inline intrinsic contributions of a bounded Grid into its
 * physical tracks.  Ordinary intrinsic flow takes the widest block child,
 * which is not sufficient for inline-grid shrink wrapping, and a subgrid
 * must expose each descendant contribution to the corresponding parent
 * track instead of presenting one aggregate width for the spanning subgrid
 * item.
 *
 * The engine's fixed track and placement-row limits keep both the work and
 * scratch fixed.  Recursion follows nested subgrids only and shares
 * the layout tree's depth ceiling and cancellation hook.
 */
static void intrinsic_grid_resolve_placement(
    LayoutContext *context, lxb_dom_node_t *container,
    const ComputedStyle *container_style, ComputedStyle *item)
{
    (void) container;
    (void) stylesheet_resolve_named_grid_area(
        context->sheet, container_style, item);
    (void) stylesheet_resolve_named_grid_lines(
        context->sheet, container_style, item);
}

static bool intrinsic_grid_track_widths(
    LayoutContext *context, lxb_dom_node_t *container,
    const ComputedStyle *container_style, int column_override,
    int row_override, int limit, bool minimum_phase, size_t depth,
    int widths[GRID_TRACK_LIMIT], int *used_columns)
{
    if (context == NULL || container == NULL || container_style == NULL
        || widths == NULL || limit <= 0 || context->cancelled) return false;
    /*
     * Intrinsic feedback is an optimization of the parent sizing pass, not
     * authority to fail the page.  At the shared layout depth ceiling,
     * retain the already-collected outer requirements and let ordinary
     * block-layout fallback handle the deeper subtree.
     */
    if (depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT) return true;
    int columns = column_override > 0 ? column_override
        : (int) stylesheet_grid_track_count(
            context->sheet, container_style, false);
    int rows = row_override > 0 ? row_override
        : (int) stylesheet_grid_track_count(
            context->sheet, container_style, true);
    if (columns < 1) columns = 1;
    if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
    if (rows < 1) rows = 1;
    if (rows > GRID_PLACEMENT_ROW_LIMIT) rows = GRID_PLACEMENT_ROW_LIMIT;

    FlexOrderPlan order;
    if (!flex_order_plan_build(&order, context, container, container_style)) {
        return false;
    }
    FlexItemIterator scan;
    FlatItem item;
    /*
     * Items placed on implicit columns (grid-column: 2 with no template,
     * or a negative line) widen the grid exactly as layout_block_grid does.
     * Sizing only the explicit tracks dropped those items from the grid's
     * max-content width, so a shrink-wrapped grid squeezed them: the
     * Turnstile checkbox label wrapped onto a second line. A subgrid's
     * tracks are its parent's, so an override keeps the explicit count.
     */
    int explicit_columns = columns;
    int column_origin = 0;
    bool locked_items = false;
    if (column_override <= 0) {
        int minimum_column = 0, maximum_column = explicit_columns;
        flex_iterator_init(&scan, context, container, container_style,
                           &order);
        size_t extent_visits = 0;
        while (flex_iterator_next(&scan, &item)) {
            if (++extent_visits > GRID_PLACEMENT_ROW_LIMIT * GRID_TRACK_LIMIT)
                break;
            (void) stylesheet_resolve_named_grid_area(
                context->sheet, container_style, &item.style);
            (void) stylesheet_resolve_named_grid_lines(
                context->sheet, container_style, &item.style);
            if (item.style.out_of_flow || item.style.fixed_position)
                continue;
            if (grid_item_locked_to_major_axis(&item.style,
                                               container_style)) {
                locked_items = true;
            }
            grid_axis_extent(
                computed_style_grid_column_start(&item.style),
                computed_style_grid_column_end(&item.style),
                computed_style_grid_column_span(&item.style),
                explicit_columns, &minimum_column, &maximum_column);
        }
        if (minimum_column < -(GRID_TRACK_LIMIT - 1))
            minimum_column = -(GRID_TRACK_LIMIT - 1);
        column_origin = -minimum_column;
        columns = maximum_column - minimum_column;
        if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
        if (columns < 1) columns = 1;
    }

    uint8_t types[GRID_TRACK_LIMIT] = {0};
    int gap = computed_style_resolve_gap(container_style->gap, limit);
    for (int column = 0; column < columns; column++) {
        int explicit_column = column - column_origin;
        /* Implicit tracks take grid-auto-columns, alternating with its
           second value when it names two. */
        bool implicit = column_override <= 0
            && (explicit_column < 0 || explicit_column >= explicit_columns);
        int auto_index = explicit_column < 0
            ? explicit_column : explicit_column - explicit_columns;
        uint16_t second = container_style->grid_auto_column_second;
        bool use_second = implicit && second != 0 && (auto_index & 1) != 0;
        uint8_t type = column_override > 0
            ? GRID_TRACK_AUTO
            : implicit
            ? (use_second ? grid_auto_second_type(second)
                          : (uint8_t) container_style->grid_auto_column_type)
            : stylesheet_grid_track_type(
                context->sheet, container_style, false,
                (unsigned) explicit_column);
        unsigned value = column_override > 0
            ? GRID_TRACK_AUTO_VALUE
            : implicit
            ? (use_second ? grid_auto_second_value(second)
                          : (unsigned) container_style->grid_auto_column_value)
            : stylesheet_grid_track_value(
                context->sheet, container_style, false,
                (unsigned) explicit_column);
        unsigned floor = column_override > 0 || implicit ? 0
            : stylesheet_grid_track_minimum(
                context->sheet, container_style, false,
                (unsigned) explicit_column);
        types[column] = type;
        widths[column] = (int) floor;
        if (type == GRID_TRACK_FIXED && (int) value > widths[column]) {
            widths[column] = (int) value;
        }
        /* Intrinsic percentage tracks are cyclic and therefore automatic.
           Their fixed component, when present, is retained in the floor by
           the parsed minmax representation above. */
        if (type == GRID_TRACK_PERCENT) types[column] = GRID_TRACK_AUTO;
    }

    GridPlacementState placement;
    grid_placement_init(&placement, columns, rows, container_style);
    placement.explicit_columns = (uint8_t) explicit_columns;
    placement.explicit_rows = (uint8_t) rows;
    placement.column_origin = (uint8_t) column_origin;
    if (locked_items
        && !grid_placement_reserve(
               &placement, context, container, container_style, &order,
               intrinsic_grid_resolve_placement, &scan, &item)) {
        flex_order_plan_destroy(&order);
        return false;
    }
    if (used_columns != NULL) *used_columns = columns;
    flex_iterator_init(&scan, context, container, container_style, &order);
    size_t visits = 0;
    bool success = true;
    while (flex_iterator_next(&scan, &item)) {
        if (++visits > GRID_PLACEMENT_ROW_LIMIT * GRID_TRACK_LIMIT
            || !layout_cooperate(context, item.node)) {
            success = !context->cancelled;
            break;
        }
        (void) stylesheet_resolve_named_grid_area(
            context->sheet, container_style, &item.style);
        (void) stylesheet_resolve_named_grid_lines(
            context->sheet, container_style, &item.style);
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        GridItemPlacement placed;
        (void) grid_place_item(&placement, &item.style, &placed);
        int start = placed.column;
        int span = placed.column_span;
        if (start < 0 || start >= columns) continue;
        if (span < 1) span = 1;
        if (span > columns - start) span = columns - start;

        if (!item.anonymous_text
            && computed_style_grid_subgrid_columns(&item.style)) {
            int local[GRID_TRACK_LIMIT] = {0};
            int local_rows = computed_style_grid_subgrid_rows(&item.style)
                ? placed.row_span : (int) stylesheet_grid_track_count(
                    context->sheet, &item.style, true);
            if (!intrinsic_grid_track_widths(
                    context, item.node, &item.style, span, local_rows,
                    limit, minimum_phase, depth + 1, local, NULL)) {
                success = false;
                break;
            }
            ComputedStyle resolved = item.style;
            resolve_padding(context->sheet, &resolved, limit);
            resolve_margin(context->sheet, &resolved, limit);
            int local_gap = computed_style_resolve_gap(
                resolved.gap, limit);
            int difference = gap - local_gap;
            int before = difference / 2;
            int after = difference - before;
            for (int offset = 0; offset < span; offset++) {
                if (types[start + offset] == GRID_TRACK_FIXED) continue;
                int adjustment = 0;
                if (offset != 0) adjustment += after;
                if (offset + 1 < span) adjustment += before;
                int required = local[offset] - adjustment;
                if (offset == 0) {
                    required += resolved.padding.left
                                + resolved.border.left
                                + resolved.margin.left;
                }
                if (offset + 1 == span) {
                    required += resolved.padding.right
                                + resolved.border.right
                                + resolved.margin.right;
                }
                if (required > widths[start + offset]) {
                    widths[start + offset] = required;
                }
            }
            continue;
        }

        int contribution = minimum_phase
            ? grid_item_minimum_contribution(
                context, &item, limit, start, span, types)
            : intrinsic_text_width(
                context, item.node, &item.parent_style, limit);
        int occupied = gap * (span - 1);
        int eligible = 0;
        for (int offset = 0; offset < span; offset++) {
            occupied += widths[start + offset];
            if (types[start + offset] != GRID_TRACK_FIXED) eligible++;
        }
        int extra = contribution - occupied;
        while (extra > 0 && eligible > 0) {
            int share = extra / eligible;
            if (share < 1) share = 1;
            for (int offset = 0; offset < span && extra > 0; offset++) {
                if (types[start + offset] == GRID_TRACK_FIXED) continue;
                int addition = share < extra ? share : extra;
                widths[start + offset] += addition;
                extra -= addition;
            }
        }
    }
    flex_order_plan_destroy(&order);
    return success && !context->cancelled;
}

static int intrinsic_grid_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int limit, bool minimum_phase)
{
    int widths[GRID_TRACK_LIMIT] = {0};
    int columns = 1;
    if (!intrinsic_grid_track_widths(
            context, node, style, 0, 0, limit, minimum_phase, 0, widths,
            &columns)) {
        return 0;
    }
    if (columns < 1) columns = 1;
    if (columns > GRID_TRACK_LIMIT) columns = GRID_TRACK_LIMIT;
    int gap = computed_style_resolve_gap(style->gap, limit);
    int64_t total = (int64_t) gap * (columns - 1);
    for (int column = 0; column < columns; column++) {
        total += widths[column];
    }
    total += style->padding.left + style->padding.right
             + style->border.left + style->border.right
             + style->margin.left + style->margin.right;
    if (total < 0) return 0;
    return total < limit ? (int) total : limit;
}

bool intrinsic_grid_subgrid_column_requirements(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, int columns, int rows, int limit,
    int minimums[GRID_TRACK_LIMIT], int maximums[GRID_TRACK_LIMIT])
{
    if (minimums == NULL || maximums == NULL || columns < 1
        || columns > GRID_TRACK_LIMIT) return false;
    memset(minimums, 0, sizeof(int) * GRID_TRACK_LIMIT);
    memset(maximums, 0, sizeof(int) * GRID_TRACK_LIMIT);
    return intrinsic_grid_track_widths(
               context, node, style, columns, rows, limit, true, 0,
               minimums, NULL)
           && intrinsic_grid_track_widths(
               context, node, style, columns, rows, limit, false, 0,
               maximums, NULL);
}

enum {
    INTRINSIC_CACHE_MAXIMUM = 1,
    INTRINSIC_CACHE_MINIMUM = 2,
    INTRINSIC_CACHE_POSITIONED_MAXIMUM = 3
};

static size_t intrinsic_cache_home(lxb_dom_node_t *node, int limit,
                                   unsigned kind, bool ignore_own_width)
{
    size_t mixed = layout_pointer_hash(node);
    mixed ^= (size_t) (unsigned) limit * (size_t) UINT32_C(2654435761);
    mixed ^= (size_t) kind << 3;
    mixed ^= ignore_own_width ? (size_t) UINT32_C(0x9e3779b9) : 0;
    return mixed & (LAYOUT_INTRINSIC_CACHE_CAPACITY - 1u);
}

static bool intrinsic_cache_get(LayoutContext *context,
                                lxb_dom_node_t *node, int limit,
                                unsigned kind, bool ignore_own_width,
                                int *width)
{
    if (node == context->intrinsic_constraint_node) return false;
    size_t home = intrinsic_cache_home(
        node, limit, kind, ignore_own_width);
    for (size_t probe = 0; probe < LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT;
         probe++) {
        LayoutIntrinsicCacheEntry *entry = &context->intrinsic_cache[
            (home + probe) & (LAYOUT_INTRINSIC_CACHE_CAPACITY - 1u)];
        if (entry->node == NULL) break;
        if (entry->node == node && entry->limit == limit
            && entry->kind == kind
            && entry->ignore_own_width == ignore_own_width) {
            entry->stamp = ++context->intrinsic_cache_clock;
            context->intrinsic_cache_hits++;
            *width = entry->width;
            return true;
        }
    }
    /* A container-query pass cannot vouch for sizes measured under an
       earlier pass's container geometry. */
    if (context->reuse != NULL && !context->container_live) {
        size_t reuse_home = intrinsic_cache_home(
            node, limit, kind, ignore_own_width)
            & (LAYOUT_REUSE_INTRINSIC_CAPACITY - 1u);
        for (size_t probe = 0; probe < LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT;
             probe++) {
            LayoutIntrinsicCacheEntry *entry = &context->reuse->intrinsic[
                (reuse_home + probe)
                & (LAYOUT_REUSE_INTRINSIC_CAPACITY - 1u)];
            if (entry->node == node && entry->limit == limit
                && entry->kind == kind
                && entry->ignore_own_width == ignore_own_width) {
                entry->stamp = ++context->reuse->clock;
                context->reuse->stats.intrinsic_hits++;
                *width = entry->width;
                return true;
            }
        }
        context->reuse->stats.intrinsic_misses++;
    }
    context->intrinsic_cache_misses++;
    return false;
}

static void intrinsic_cache_put(LayoutContext *context,
                                lxb_dom_node_t *node, int limit,
                                unsigned kind, bool ignore_own_width,
                                int width)
{
    if (node == context->intrinsic_constraint_node) return;
    size_t home = intrinsic_cache_home(
        node, limit, kind, ignore_own_width);
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT;
         probe++) {
        size_t slot = (home + probe)
                      & (LAYOUT_INTRINSIC_CACHE_CAPACITY - 1u);
        LayoutIntrinsicCacheEntry *entry =
            &context->intrinsic_cache[slot];
        if (entry->node == NULL
            || (entry->node == node && entry->limit == limit
                && entry->kind == kind
                && entry->ignore_own_width == ignore_own_width)) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
    context->intrinsic_cache[replacement] = (LayoutIntrinsicCacheEntry) {
        .node = node,
        .limit = limit,
        .width = width,
        .kind = (uint8_t) kind,
        .ignore_own_width = ignore_own_width,
        .stamp = ++context->intrinsic_cache_clock
    };
    if (context->reuse != NULL) {
        size_t reuse_home = home
            & (LAYOUT_REUSE_INTRINSIC_CAPACITY - 1u);
        size_t reuse_replacement = reuse_home;
        uint64_t reuse_oldest = UINT64_MAX;
        for (size_t probe = 0; probe < LAYOUT_INTRINSIC_CACHE_PROBE_LIMIT;
             probe++) {
            size_t slot = (reuse_home + probe)
                & (LAYOUT_REUSE_INTRINSIC_CAPACITY - 1u);
            LayoutIntrinsicCacheEntry *entry =
                &context->reuse->intrinsic[slot];
            if (entry->node == NULL || (entry->node == node
                    && entry->limit == limit && entry->kind == kind
                    && entry->ignore_own_width == ignore_own_width)) {
                reuse_replacement = slot;
                break;
            }
            if (entry->stamp < reuse_oldest) {
                reuse_oldest = entry->stamp;
                reuse_replacement = slot;
            }
        }
        context->reuse->intrinsic[reuse_replacement] =
            (LayoutIntrinsicCacheEntry) {
                .node = node,
                .limit = limit,
                .width = width,
                .kind = (uint8_t) kind,
                .ignore_own_width = ignore_own_width,
                .stamp = ++context->reuse->clock
            };
    }
}

int intrinsic_replaced_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style)
{
    if (context == NULL || node == NULL || style == NULL) return 0;
    LayoutReplacedInfo replaced;
    layout_replaced_info(context, node, style, &replaced);
    int natural_width = replaced.natural_width;
    int natural_height = replaced.natural_height;
    if (natural_width <= 0) {
        size_t length = 0;
        const char *attribute = document_attribute(node, "width", &length);
        if (attribute != NULL && length != 0 && length < 32) {
            char value[32];
            memcpy(value, attribute, length);
            value[length] = '\0';
            natural_width = atoi(value);
        }
    }
    /* A height-only replaced flex/grid item contributes its aspect-ratio
       width. Returning the decoded source width over-allocates the flex item
       even though the later paint path correctly scales the image. */
    bool fallback_dimensions = replaced.fallback_dimensions;
    int height_basis = natural_width;
    bool definite_height = !style->height_percent;
    if (style->height_percent && parent != NULL
        && parent->has_height && !parent->height_percent
        && (parent->display == DISPLAY_GRID
            || parent->display == DISPLAY_INLINE_GRID)) {
        height_basis = style_pixel_height(context->sheet, parent, natural_width);
        if (parent->box_sizing_border_box)
            height_basis -= parent->padding.top + parent->padding.bottom
                + parent->border.top + parent->border.bottom;
        definite_height = height_basis > 0;
    }
    if (!fallback_dimensions && !style->has_width && style->has_height
        && definite_height
        && natural_width > 0 && natural_height > 0) {
        int height = style->height_percent
            ? resolve_declared_length(context->sheet,style->height,true,height_basis)
            : style_pixel_height(context->sheet,style,natural_width);
        if (style->box_sizing_border_box && height > 0) {
            height -= style->padding.top + style->padding.bottom
                      + style->border.top + style->border.bottom;
        }
        if (height > 0) {
            natural_width = style->aspect_width > 0
                            && style->aspect_height > 0
                ? layout_scale_dimension(
                    style->aspect_width, height, style->aspect_height)
                : layout_scale_dimension(
                    natural_width, height, natural_height);
            natural_height = height;
        }
    }
    /* Transfer a definite cross-axis min/max through the intrinsic ratio
       before flex/grid distributes inline size. Clamping only in the child
       layout leaves a too-large flex basis, which is then forced back onto
       the correctly height-constrained replaced element. Percentages remain
       unresolved here: this intrinsic query has no definite cross size. */
    if (!fallback_dimensions && !style->has_width
        && natural_width > 0 && natural_height > 0) {
        if (!style->has_height && style->aspect_width > 0 && style->aspect_height > 0)
            natural_height = layout_scale_dimension(
                style->aspect_height, natural_width, style->aspect_width);
        int height = natural_height, bound = 0;
        int edges = style->box_sizing_border_box
            ? style->padding.top + style->padding.bottom
                + style->border.top + style->border.bottom : 0;
        if (!style->max_height_percent
            && style->max_height != STYLE_LENGTH_NONE
            && style->max_height != STYLE_LENGTH_MIN_CONTENT
            && resolve_computed_length(context->sheet, style->max_height,
                                       false, 0, &bound)) {
            bound = bound > edges ? bound - edges : 0;
            if (height > bound) height = bound;
        }
        if (!style->min_height_percent
            && resolve_computed_length(context->sheet, style->min_height,
                                       false, 0, &bound)) {
            bound = bound > edges ? bound - edges : 0;
            if (height < bound) height = bound;
        }
        if (height != natural_height)
            natural_width = layout_scale_dimension(
                natural_width, height, natural_height);
    }
    return natural_width;
}

int intrinsic_text_width(LayoutContext *context, lxb_dom_node_t *node,
                                const ComputedStyle *parent, int limit)
{
    LAYOUT_FLOW_SCOPE(context, LAYOUT_FLOW_INTRINSIC);
    int cached = 0;
    if (context != NULL && node != NULL && limit > 0
        && intrinsic_cache_get(
               context, node, limit, INTRINSIC_CACHE_MAXIMUM, false,
               &cached)) return cached;
    if (!layout_tree_enter(context, node, "intrinsic-max")) {
        /* A conservative full-width contribution prevents deep content from
           collapsing into an unusably narrow flex/grid item. */
        return context != NULL && !context->cancelled && limit > 0 ? limit : 0;
    }
    int width = intrinsic_text_width_impl(
        context, node, parent, limit, false);
    layout_tree_leave(context);
    if (!context->cancelled && node != NULL && limit > 0) {
        intrinsic_cache_put(
            context, node, limit, INTRINSIC_CACHE_MAXIMUM, false, width);
    }
    return width;
}

int intrinsic_min_text_width_ignoring_own_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int limit)
{
    return intrinsic_min_text_width_internal(
        context, node, parent, limit, true);
}

static int constrain_intrinsic_outer_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style,
    int limit, int outer_width)
{
    int margins = style->margin.left + style->margin.right;
    int border_box = outer_width - margins;
    if (border_box < 0) border_box = 0;
    border_box = constrain_border_box_width(
        context, node, parent, style, limit, border_box, NULL);
    int constrained = border_box + margins;
    if (constrained < 0) constrained = 0;
    return constrained < limit ? constrained : limit;
}

int intrinsic_positioned_width(LayoutContext *context,
                               lxb_dom_node_t *node,
                               const ComputedStyle *parent, int limit)
{
    int cached = 0;
    if (context != NULL && node != NULL && limit > 0
        && intrinsic_cache_get(
               context, node, limit, INTRINSIC_CACHE_POSITIONED_MAXIMUM,
               false, &cached)) return cached;
    if (!layout_tree_enter(context, node, "intrinsic-positioned-max")) {
        return context != NULL && !context->cancelled && limit > 0 ? limit : 0;
    }
    int width = intrinsic_text_width_impl(
        context, node, parent, limit, true);
    layout_tree_leave(context);
    if (!context->cancelled && node != NULL && limit > 0) {
        intrinsic_cache_put(
            context, node, limit, INTRINSIC_CACHE_POSITIONED_MAXIMUM,
            false, width);
    }
    return width;
}

static bool intrinsic_text_has_visible_character(
    const char *text, size_t length)
{
    for (size_t at = 0; text != NULL && at < length;) {
        unsigned codepoint = 0;
        size_t used = font_utf8_next(text + at, length - at, &codepoint);
        if (used == 0) break;
        if (!font_codepoint_default_ignorable(codepoint)
            && !(codepoint < 0x80u
                 && isspace((unsigned char) codepoint))) {
            return true;
        }
        at += used;
    }
    return false;
}

static bool intrinsic_has_generated_inline_text(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, PseudoElement first, PseudoElement last)
{
    if (!style_node_pseudo_rules_may_affect_discovery(context->sheet, node))
        return false;
    for (PseudoElement p = first; p <= last; p++) {
        ComputedStyle generated = layout_style_for_pseudo(context, node, p, style);
        if (generated.generated_content && !generated.hidden
            && !generated.out_of_flow && !generated.fixed_position
            && generated.display == DISPLAY_INLINE
            && intrinsic_text_has_visible_character(
                generated.generated_text, generated.generated_text_length))
            return true;
    }
    return false;
}

static bool intrinsic_wraps_single_atomic(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent)
{
    bool found = false;
    for (lxb_dom_node_t *child = document_flat_first_child(node);
         child != NULL; child = document_flat_next_sibling(child)) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = layout_text_data(child, &length);
            if (intrinsic_text_has_visible_character(text, length)) {
                return false;
            }
            continue;
        }
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || found) return false;
        ComputedStyle style = layout_style_for_node(context, child, parent);
        found = style.display == DISPLAY_INLINE_BLOCK
                || style.display == DISPLAY_INLINE_FLEX
                || style.display == DISPLAY_INLINE_GRID
                || document_node_is_tag(child, LXB_TAG_IMG)
                || document_node_is_tag(child, LXB_TAG_SVG)
                || document_node_is_tag(child, LXB_TAG_VIDEO)
                || document_node_is_tag(child, LXB_TAG_AUDIO);
        if (!found) return false;
    }
    return found;
}

static int intrinsic_text_width_impl(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *parent, int limit,
                                     bool include_positioned_root)
{
    context->intrinsic_width_visits++;
    if (node == NULL || limit <= 0) return 0;
    if (!layout_cooperate(context, node)) return 0;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = layout_text_data(node, &length);
        if (text == NULL) return 0;
        const FontFace *face = font_context_face_at_weight(
            context->fonts, context->web_fonts, parent->font_family,
            parent->font_italic, style_uses_bold_face(parent), computed_style_font_weight(parent));
        bool metric_bold = style_uses_bold_face(parent);
        FontFamily metric_family = font_context_metric_family(
            context->web_fonts, parent->font_family, face);
        bool synthetic_bold = style_uses_synthetic_weight(
            context->fonts, context->web_fonts, parent, face);
        /* Accumulate in 26.6 fixed point exactly like flow_text and take
           the ceiling once at the end: max-content must never be narrower
           than the single line the inline flow will actually produce, or
           shrink-to-fit boxes wrap internally where reference browsers
           keep one line. */
        int width_fixed = 0;
        bool has_word = false;
        int minimum_width = 0;
        bool pair_intrinsics = context->intrinsic_pair_mode;
        bool break_for_minimum = pair_intrinsics
            && (computed_style_word_break(parent) == WORD_BREAK_ALL
                || computed_style_overflow_wrap(parent)
                       == OVERFLOW_WRAP_ANYWHERE);
        int limit_fixed = limit > INT_MAX / 64 ? INT_MAX
                          : layout_fixed_from_integer(limit);
        for (size_t at = 0; at < length;) {
            while (at < length && isspace((unsigned char) text[at])) at++;
            size_t end = at;
            while (end < length && !isspace((unsigned char) text[end])) end++;
            if (end == at) break;
            int word_fixed = layout_text_measure(
                context->reuse, face, metric_family, text + at, end - at,
                computed_style_font_size_fixed(parent),
                synthetic_bold, metric_bold, parent->font_scale,
                parent->letter_spacing_fixed, parent->text_transform,
                !computed_style_kerning_none(parent));
            if (pair_intrinsics) {
                int minimum_word = 0;
                for (size_t segment_at = at; segment_at < end;) {
                    bool discard = false;
                    size_t segment_length = utf8_line_segment_length(
                        text + segment_at, end - segment_at,
                        computed_style_word_break(parent)
                            == WORD_BREAK_KEEP_ALL,
                        computed_style_hyphens_none(parent),
                        &discard);
                    if (segment_length == 0) break;
                    int segment_width = discard ? 0 : measured_flow_text_width(
                        face, metric_family, text + segment_at,
                        segment_length,
                        computed_style_font_size_fixed(parent),
                        synthetic_bold, metric_bold, parent->font_scale,
                        parent->letter_spacing_fixed,
                        parent->text_transform == TEXT_TRANSFORM_CAPITALIZE
                            && segment_at != at ? TEXT_TRANSFORM_NONE
                            : parent->text_transform,
                        !computed_style_kerning_none(parent));
                    if (break_for_minimum && !discard) {
                        segment_width = 0;
                        size_t segment_end = segment_at + segment_length;
                        for (size_t character_at = segment_at;
                             character_at < segment_end;) {
                            size_t character_length = utf8_character_length(
                                text + character_at,
                                segment_end - character_at);
                            if (character_length == 0) break;
                            int character_width = measured_flow_text_width(
                                face, metric_family, text + character_at,
                                character_length,
                                computed_style_font_size_fixed(parent),
                                synthetic_bold, metric_bold,
                                parent->font_scale, 0,
                                parent->text_transform == TEXT_TRANSFORM_CAPITALIZE
                                    && character_at != at ? TEXT_TRANSFORM_NONE
                                    : parent->text_transform,
                                !computed_style_kerning_none(parent));
                            if (character_width > segment_width) {
                                segment_width = character_width;
                            }
                            character_at += character_length;
                        }
                    }
                    if (segment_width > minimum_word) {
                        minimum_word = segment_width;
                    }
                    segment_at += segment_length;
                }
                if (minimum_word > minimum_width) {
                    minimum_width = minimum_word;
                }
            }
            int space_fixed = font_text_width_for_family_at_size_fixed(
                face, metric_family, " ", 1,
                computed_style_font_size_fixed(parent), synthetic_bold,
                metric_bold);
            if (space_fixed < 0) {
                space_fixed = 6 * parent->font_scale * 64;
            }
            space_fixed = layout_fixed_add(
                space_fixed,
                layout_fixed_from_integer(parent->word_spacing));
            if (space_fixed < 0) space_fixed = 0;
            width_fixed = layout_fixed_add(
                width_fixed,
                layout_fixed_add(
                    has_word
                        ? layout_fixed_add(
                              space_fixed,
                              parent->letter_spacing_fixed * 2)
                        : 0,
                    word_fixed));
            has_word = true;
            if (width_fixed >= limit_fixed) return limit;
            at = end;
        }
        int maximum_width = layout_fixed_ceil(width_fixed);
        if (pair_intrinsics) {
            if (parent->white_space_mode == WHITE_SPACE_NOWRAP
                || parent->white_space_mode == WHITE_SPACE_PRE) {
                minimum_width = maximum_width;
            }
            if (minimum_width > limit) minimum_width = limit;
            intrinsic_cache_put(
                context, node, limit, INTRINSIC_CACHE_MINIMUM,
                false, minimum_width);
            context->intrinsic_paired_text_measurements++;
        }
        return maximum_width;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return 0;
    if (layout_node_is_hidden_input(node)) return 0;
    ComputedStyle style = layout_style_for_node(context, node, parent);
    /*
     * Percentage padding is cyclic while computing an intrinsic inline
     * contribution and therefore contributes zero; fixed and calc() length
     * components survive when resolved against a zero percentage basis.
     */
    resolve_padding(context->sheet, &style, 0);
    if (style.display == DISPLAY_NONE
        || style.display == DISPLAY_TABLE_COLUMN
        || style.hidden
        || ((style.out_of_flow || style.fixed_position)
            && !include_positioned_root)) {
        return 0;
    }
    /* A definite inline size is also the max-content contribution of a
       block-level descendant.  Restricting this to atomic inlines made
       width:max-content containers collapse around empty fixed-width flex
       children. */
    if (node != context->intrinsic_constraint_node
        && style.has_width && !style.width_max_content
        && !style.width_percent) {
        int width = resolve_declared_length(
            context->sheet, style.width, style.width_percent, limit);
        int edges = style.padding.left + style.padding.right
                    + style.border.left + style.border.right;
        if (!style.box_sizing_border_box) width += edges;
        width += style.margin.left + style.margin.right;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    if (style.generated_image || layout_replaced_element_tag(node)) {
        int replaced_width = intrinsic_replaced_width(
            context, node, parent, &style);
        if (replaced_width > 0) {
            int width = replaced_width + style.padding.left
                        + style.padding.right
                        + style.border.left + style.border.right
                        + style.margin.left + style.margin.right;
            return constrain_intrinsic_outer_width(
                context, node, parent, &style, limit, width);
        }
    }
    int control_width = layout_control_default_width(node);
    if (control_width > 0
        && ((style.appearance & STYLE_APPEARANCE_MASK) != APPEARANCE_NONE
            || document_node_is_tag(node, LXB_TAG_TEXTAREA)
            || (document_node_is_tag(node, LXB_TAG_INPUT)
                && layout_input_control_type(node) == CONTROL_INPUT))) {
        /* Removing native chrome does not remove a text field's intrinsic
           editing area. In a shrink-to-fit table/flex ancestor, percentage
           widths are cyclic: measuring only its empty DOM collapses it. */
        control_width += style.margin.left + style.margin.right;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, control_width);
    }
    /* A fixed-layout percentage-width table has a definite percentage basis
       when measured as a flex item's max-content contribution.  Retaining
       that contribution lets sibling flex items shrink from equal bases
       instead of collapsing the cyclic table to its borders. */
    if (style.display == DISPLAY_TABLE && style.table_layout_fixed
        && style.has_width && style.width_percent) {
        int width = resolve_declared_length(
            context->sheet, style.width, true, limit);
        int edges = style.padding.left + style.padding.right
                    + style.border.left + style.border.right;
        if (!style.box_sizing_border_box) width += edges;
        width += style.margin.left + style.margin.right;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    if (style.display == DISPLAY_TABLE) {
        /* A table contributes the combined column widths, not just the
           widest cell encountered by the ordinary block-child walker. */
        int width = table_intrinsic_width(context, node, &style, limit);
        width = layout_add_coordinate(
            width, style.margin.left + style.margin.right);
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    const ImageResource *mask = style.mask_image != NULL
        ? images_find_mask_source(
            context->images, node, style.mask_image, PSEUDO_NONE)
        : images_find_mask_node(context->images, node);
    if (image_resource_available(mask)) {
        int width = style.has_width
                    ? resolve_declared_length(
                        context->sheet, style.width,
                        style.width_percent, limit)
                    : image_resource_intrinsic_width(mask);
        int minimum_width = style_minimum_width(context->sheet, &style,
                                                limit);
        if (width < minimum_width) width = minimum_width;
        width += style.margin.left + style.margin.right;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    if (style.display == DISPLAY_GRID
        || style.display == DISPLAY_INLINE_GRID) {
        int width = intrinsic_grid_width(
            context, node, &style, limit, false);
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    bool horizontal_flex = (style.display == DISPLAY_FLEX
                            || style.display == DISPLAY_INLINE_FLEX)
                           && (style.flex_direction == FLEX_ROW
                               || style.flex_direction == FLEX_ROW_REVERSE);
    bool before_block = false;
    int inline_width = generated_inline_pseudo_width(
        context, node, &style, PSEUDO_BEFORE, &before_block);
    int block_width = before_block ? inline_width : 0;
    if (before_block) inline_width = 0;
    size_t horizontal_flex_items = 0;
    /* Flex layout promotes the children of display:contents wrappers to
       flex items. Intrinsic sizing must walk that same flattened sequence:
       measuring the wrapper as one ordinary block returns only its widest
       child, so a max-content horizontal navigation is clipped even though
       its items were positioned side by side. */
    if (horizontal_flex) {
        inline_width = 0;
        FlexItemIterator iterator;
        FlatItem item;
        flex_iterator_init(&iterator, context, node, &style, NULL);
        while (flex_iterator_next(&iterator, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) {
                continue;
            }
            int item_width = item.generated_pseudo != PSEUDO_NONE
                ? generated_inline_pseudo_width(context, node, &style,
                                                 item.generated_pseudo, NULL)
                : intrinsic_text_width(context, item.node, &item.parent_style, limit);
            /* An empty item has a zero max-content contribution, but it
               still occupies a position in the flex sequence and therefore
               contributes a gap beside the next item. */
            if (item_width < 0) item_width = 0;
            int gap = horizontal_flex_items == 0
                ? 0 : computed_style_resolve_gap(style.gap, limit);
            if (inline_width > limit - gap
                || inline_width + gap > limit - item_width) {
                return limit;
            }
            inline_width += gap + item_width;
            horizontal_flex_items++;
        }
        block_width = inline_width;
        block_width += style.border.left + style.border.right
                       + style.padding.left + style.padding.right
                       + style.margin.left + style.margin.right;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, block_width);
    }
    /* Text children measure their own words, but collapsed whitespace at
       a child boundary ("with <a>…" or "…</a> next") still produces one
       space in the inline flow.  Losing it makes shrink-to-fit boxes
       narrower than the line they must hold, wrapping their last inline
       piece internally. */
    int first_line_indent = 0;
    if (style.display != DISPLAY_INLINE && style.display != DISPLAY_CONTENTS) {
        /* Cyclic percentage indentation contributes zero to intrinsic
           sizing; a positive fixed component still occupies the first
           formatted line. Inline descendants inherit text-indent without
           starting another line, so they must not charge it again. */
        if (!style_length_resolve(
                context->sheet, style.text_indent, 0, &first_line_indent)
            || first_line_indent < 0) first_line_indent = 0;
    }
    bool first_line_indent_pending = true;
    if (inline_width > 0) {
        inline_width = layout_add_coordinate(inline_width, first_line_indent);
        first_line_indent_pending = false;
    }
    int boundary_space_width = -1;
    bool pending_boundary_space = false;
    bool has_inline_text = inline_width > 0
        && intrinsic_has_generated_inline_text(
            context, node, &style, PSEUDO_BEFORE, PSEUDO_BEFORE);
    bool previous_atomic = false;
    int float_left_width = 0, float_right_width = 0;
    for (lxb_dom_node_t *child = document_flat_first_child(node);
         child != NULL; child = document_flat_next_sibling(child)) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_node_is_tag(child, LXB_TAG_BR)) {
            if (inline_width > block_width) block_width = inline_width;
            inline_width = 0;
            pending_boundary_space = false;
            has_inline_text = false;
            previous_atomic = false;
            first_line_indent_pending = false;
            continue;
        }
        int child_width = intrinsic_text_width(context, child, &style, limit);
        bool block = false;
        bool child_atomic = false;
        ComputedStyle child_style = style;
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            child_style = layout_style_for_node(
                context, child, &style);
            block = is_block_display(child_style.display);
            child_atomic =
                child_style.display == DISPLAY_INLINE_BLOCK
                || child_style.display == DISPLAY_INLINE_FLEX
                || child_style.display == DISPLAY_INLINE_GRID
                || document_node_is_tag(child, LXB_TAG_IMG)
                || document_node_is_tag(child, LXB_TAG_SVG)
                || document_node_is_tag(child, LXB_TAG_VIDEO)
                || document_node_is_tag(child, LXB_TAG_AUDIO);
        }
        bool leading_space = false, trailing_space = false;
        bool child_has_text = false;
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t text_length = 0;
            const char *text = layout_text_data(child, &text_length);
            if (text != NULL && text_length != 0) {
                leading_space = isspace((unsigned char) text[0]);
                trailing_space =
                    isspace((unsigned char) text[text_length - 1]);
                child_has_text = intrinsic_text_has_visible_character(
                    text, text_length);
            }
        } else if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t text_length = 0;
            const char *text = first_text_data(child, &text_length);
            child_has_text = intrinsic_text_has_visible_character(
                text, text_length);
            if (!block && !child_atomic && text != NULL && text_length != 0) {
                leading_space = isspace((unsigned char) text[0]);
                /* A single text child is the common inline label case.
                   Its trailing source-space belongs to the next sibling's
                   collapsed boundary just as a direct text node does. */
                lxb_dom_node_t *only = document_flat_first_child(child);
                if (only != NULL && only->type == LXB_DOM_NODE_TYPE_TEXT
                    && document_flat_next_sibling(only) == NULL)
                    trailing_space = isspace((unsigned char) text[text_length - 1]);
            }
            /* Generated glyphs are text too. A following source-space must
               survive intrinsic sizing just as it survives actual flow. */
            if (!child_has_text && child_width > 0 && !block)
                child_has_text = intrinsic_has_generated_inline_text(
                    context, child, &child_style, PSEUDO_BEFORE, PSEUDO_AFTER);
            /*
             * A transparent wrapper around one atomic child remains part of
             * that atomic run. Do not infer atomicity merely from a non-zero
             * width: an empty ordinary inline can have borders or padding,
             * and those edges do not create letter-spacing boundaries.
             */
            if (!child_has_text && child_width > 0 && !block
                && !child_atomic) {
                child_atomic = intrinsic_wraps_single_atomic(
                    context, child, &child_style);
            }
        }
        bool child_has_unit = child_has_text || child_atomic;
        if (child_style.clear_mode != CLEAR_NONE
            && child->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            int row = layout_add_coordinate(float_left_width, float_right_width);
            if (row > block_width) block_width = row;
            if (child_style.clear_mode != CLEAR_RIGHT) float_left_width = 0;
            if (child_style.clear_mode != CLEAR_LEFT) float_right_width = 0;
        }
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT
            && child_style.float_mode != FLOAT_NONE
            && !child_style.out_of_flow && !child_style.fixed_position) {
            int *side = child_style.float_mode == FLOAT_LEFT
                ? &float_left_width : &float_right_width;
            *side = layout_add_coordinate(*side, child_width);
            if (layout_add_coordinate(float_left_width, float_right_width) >= limit)
                return limit;
            continue;
        }
        if (block) {
            if (inline_width > block_width) block_width = inline_width;
            inline_width = 0;
            pending_boundary_space = false;
            has_inline_text = false;
            previous_atomic = false;
            if (child_width > block_width) block_width = child_width;
        } else {
            if (child_width > 0) {
                if (first_line_indent_pending) {
                    inline_width = layout_add_coordinate(
                        inline_width, first_line_indent);
                    first_line_indent_pending = false;
                }
                if (has_inline_text && child_has_unit
                    && (pending_boundary_space || leading_space)) {
                    if (boundary_space_width < 0) {
                        const FontFace *space_face =
                            font_context_face_at_weight(
                                context->fonts, context->web_fonts,
                                style.font_family, style.font_italic,
                                style_uses_bold_face(&style), computed_style_font_weight(&style));
                        int space_fixed =
                            font_text_width_for_family_at_size_fixed(
                                space_face,
                                font_context_metric_family(
                                    context->web_fonts, style.font_family,
                                    space_face),
                                " ", 1,
                                computed_style_font_size_fixed(&style),
                                style_uses_synthetic_weight(
                                    context->fonts, context->web_fonts,
                                    &style, space_face),
                                style_uses_bold_face(&style));
                        boundary_space_width = space_fixed < 0
                            ? 6 * style.font_scale
                            : layout_fixed_ceil(space_fixed);
                    }
                    inline_width += boundary_space_width
                                    + layout_fixed_ceil(style.letter_spacing_fixed * 2);
                } else if (has_inline_text && child_has_unit
                           && !(previous_atomic && child_atomic)) {
                    inline_width += layout_fixed_ceil(style.letter_spacing_fixed);
                }
                pending_boundary_space = trailing_space;
            } else if (child->type == LXB_DOM_NODE_TYPE_TEXT
                       && (leading_space || trailing_space)) {
                pending_boundary_space = true;
            }
            inline_width += child_width;
            if (child_has_unit) previous_atomic = child_atomic;
            has_inline_text = has_inline_text || child_has_unit;
        }
        if (inline_width >= limit || block_width >= limit) return limit;
    }
    bool after_block = false;
    int after_width = generated_inline_pseudo_width(
        context, node, &style, PSEUDO_AFTER, &after_block);
    if (after_block) {
        if (after_width > block_width) block_width = after_width;
        after_width = 0;
    }
    if (has_inline_text && after_width > 0)
        inline_width = layout_add_coordinate(inline_width,
            layout_fixed_ceil(style.letter_spacing_fixed));
    if (first_line_indent_pending && after_width > 0) {
        inline_width = layout_add_coordinate(inline_width, first_line_indent);
    }
    inline_width += after_width;
    inline_width = layout_add_coordinate(inline_width,
        layout_add_coordinate(float_left_width, float_right_width));
    if (inline_width > block_width) block_width = inline_width;
    if (style.display != DISPLAY_CONTENTS
        && style.display != DISPLAY_TABLE_ROW_GROUP
        && style.display != DISPLAY_TABLE_HEADER_GROUP
        && style.display != DISPLAY_TABLE_FOOTER_GROUP) {
        block_width += style.border.left + style.border.right
                       + style.padding.left + style.padding.right
                       + style.margin.left + style.margin.right;
    }
    return constrain_intrinsic_outer_width(
        context, node, parent, &style, limit, block_width);
}

static int intrinsic_min_text_width_impl(LayoutContext *context,
                                         lxb_dom_node_t *node,
                                         const ComputedStyle *parent,
                                         int limit,
                                         bool ignore_own_width);

int intrinsic_min_text_width_internal(LayoutContext *context,
                                      lxb_dom_node_t *node,
                                      const ComputedStyle *parent,
                                      int limit,
                                      bool ignore_own_width)
{
    LAYOUT_FLOW_SCOPE(context, LAYOUT_FLOW_INTRINSIC);
    if (context == NULL) return 0;
    int cached = 0;
    if (node != NULL && limit > 0
        && intrinsic_cache_get(
               context, node, limit, INTRINSIC_CACHE_MINIMUM,
               ignore_own_width, &cached)) return cached;
    if (!layout_tree_enter(context, node, "intrinsic-min")) {
        return !context->cancelled && limit > 0 ? limit : 0;
    }
    int width = intrinsic_min_text_width_impl(
        context, node, parent, limit, ignore_own_width);
    layout_tree_leave(context);
    if (!context->cancelled && node != NULL && limit > 0) {
        intrinsic_cache_put(
            context, node, limit, INTRINSIC_CACHE_MINIMUM,
            ignore_own_width, width);
    }
    return width;
}

static int intrinsic_min_text_width_impl(LayoutContext *context,
                                         lxb_dom_node_t *node,
                                         const ComputedStyle *parent,
                                         int limit,
                                         bool ignore_own_width)
{
    context->intrinsic_min_visits++;
    if (node == NULL || limit <= 0) return 0;
    if (!layout_cooperate(context, node)) return 0;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        size_t length = 0;
        const char *text = layout_text_data(node, &length);
        if (text == NULL) return 0;
        if (parent->white_space_mode == WHITE_SPACE_NOWRAP
            || parent->white_space_mode == WHITE_SPACE_PRE) {
            return intrinsic_text_width(context, node, parent, limit);
        }
        const FontFace *face = font_context_face_at_weight(
            context->fonts, context->web_fonts, parent->font_family,
            parent->font_italic, style_uses_bold_face(parent), computed_style_font_weight(parent));
        bool metric_bold = style_uses_bold_face(parent);
        FontFamily metric_family = font_context_metric_family(
            context->web_fonts, parent->font_family, face);
        bool synthetic_bold = style_uses_synthetic_weight(
            context->fonts, context->web_fonts, parent, face);
        bool break_for_minimum = computed_style_word_break(parent)
                                     == WORD_BREAK_ALL
            || computed_style_overflow_wrap(parent)
                   == OVERFLOW_WRAP_ANYWHERE;
        int widest = 0;
        for (size_t at = 0; at < length;) {
            while (at < length && isspace((unsigned char) text[at])) at++;
            size_t end = at;
            while (end < length && !isspace((unsigned char) text[end])) end++;
            if (end == at) break;
            int word = 0;
            for (size_t segment_at = at; segment_at < end;) {
                bool discard = false;
                size_t segment_length = utf8_line_segment_length(
                    text + segment_at, end - segment_at,
                    computed_style_word_break(parent)
                        == WORD_BREAK_KEEP_ALL,
                    computed_style_hyphens_none(parent),
                    &discard);
                if (segment_length == 0) break;
                int segment_width = discard ? 0 : measured_flow_text_width(
                    face, metric_family, text + segment_at, segment_length,
                    computed_style_font_size_fixed(parent),
                    synthetic_bold, metric_bold, parent->font_scale,
                    parent->letter_spacing_fixed,
                    parent->text_transform == TEXT_TRANSFORM_CAPITALIZE
                        && segment_at != at ? TEXT_TRANSFORM_NONE
                        : parent->text_transform,
                    !computed_style_kerning_none(parent));
                if (break_for_minimum && !discard) {
                    /* anywhere adds opportunities inside each ordinary
                       Unicode line-breaking segment. */
                    segment_width = 0;
                    size_t segment_end = segment_at + segment_length;
                    for (size_t character_at = segment_at;
                         character_at < segment_end;) {
                        size_t character_length = utf8_character_length(
                            text + character_at,
                            segment_end - character_at);
                        if (character_length == 0) break;
                        int character_width = measured_flow_text_width(
                            face, metric_family, text + character_at,
                            character_length,
                            computed_style_font_size_fixed(parent),
                            synthetic_bold, metric_bold,
                            parent->font_scale, 0,
                            parent->text_transform == TEXT_TRANSFORM_CAPITALIZE
                                && character_at != at ? TEXT_TRANSFORM_NONE
                                : parent->text_transform,
                            !computed_style_kerning_none(parent));
                        if (character_width > segment_width) {
                            segment_width = character_width;
                        }
                        character_at += character_length;
                    }
                }
                if (segment_width > word) word = segment_width;
                segment_at += segment_length;
            }
            if (word > widest) widest = word;
            if (widest >= limit) return limit;
            at = end;
        }
        return widest;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return 0;
    if (layout_node_is_hidden_input(node)) return 0;
    ComputedStyle style = layout_style_for_node(context, node, parent);
    /* Match max-content: cyclic percentage padding contributes zero. */
    if (node == context->intrinsic_constraint_node) ignore_own_width = true;
    resolve_padding(context->sheet, &style, 0);
    if (style.display == DISPLAY_NONE
        || style.display == DISPLAY_TABLE_COLUMN
        || style.hidden || style.out_of_flow || style.fixed_position) {
        return 0;
    }
    /*
     * A definite max-content inline size is also the item's min-content
     * contribution. Treating the retained zero payload as a fixed width
     * collapses min-content grid tracks around width:max-content children.
     */
    if (!ignore_own_width && computed_style_width_max_content(&style)) {
        return intrinsic_text_width(context, node, parent, limit);
    }
    if (!ignore_own_width && style.has_width
        && !style.width_percent) {
        int width = resolve_declared_length(
            context->sheet, style.width, style.width_percent, limit);
        int margins = style.margin.left + style.margin.right;
        if (!style.box_sizing_border_box) {
            width += style.padding.left + style.padding.right
                     + style.border.left + style.border.right;
        }
        width += margins;
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    if (style.generated_image || layout_replaced_element_tag(node)) {
        int replaced_width = intrinsic_replaced_width(
            context, node, parent, &style);
        if (replaced_width > 0) {
            /* A percentage-sized HTML image has a cyclic inline-size while
               its containing block is being measured at min-content.  That
               percentage contributes zero here; the final replaced size is
               resolved after flex/grid assigns a definite containing width.
               Inline SVG retains its own intrinsic sizing rules. */
            int width = document_node_is_tag(node, LXB_TAG_IMG) && style.has_width
                        && style.width_percent ? 0 : replaced_width;
            width += style.padding.left + style.padding.right
                     + style.border.left + style.border.right
                     + style.margin.left + style.margin.right;
            return constrain_intrinsic_outer_width(
                context, node, parent, &style, limit, width);
        }
    }
    int control_width = layout_control_default_width(node);
    if (control_width > 0
        && ((style.appearance & STYLE_APPEARANCE_MASK) != APPEARANCE_NONE
            || document_node_is_tag(node, LXB_TAG_TEXTAREA)
            || (document_node_is_tag(node, LXB_TAG_INPUT)
                && layout_input_control_type(node) == CONTROL_INPUT))) {
        /* A percentage preferred size is cyclic at min-content. Keep the
           control's editing area for max-content, but let a compact flex
           ancestor shrink to its fixed basis, just like a percentage image. */
        if (style.has_width && style.width_percent) {
            control_width = style.padding.left + style.padding.right
                + style.border.left + style.border.right;
        }
        control_width += style.margin.left + style.margin.right;
        return control_width < limit ? control_width : limit;
    }
    if (style.display == DISPLAY_GRID
        || style.display == DISPLAY_INLINE_GRID) {
        int width = intrinsic_grid_width(
            context, node, &style, limit, true);
        return constrain_intrinsic_outer_width(
            context, node, parent, &style, limit, width);
    }
    int widest = 0;
    for (lxb_dom_node_t *child = document_flat_first_child(node);
         child != NULL; child = document_flat_next_sibling(child)) {
        int child_width = intrinsic_min_text_width_internal(
            context, child, &style, limit, false);
        if (child_width > widest) widest = child_width;
        if (widest >= limit) return limit;
    }
    /* Generated children have the same min-content and gap positions as
       real items, including in reversed and wrapping rows. */
    if ((style.display == DISPLAY_FLEX || style.display == DISPLAY_INLINE_FLEX)
        && (style.flex_direction == FLEX_ROW
            || style.flex_direction == FLEX_ROW_REVERSE)) {
        FlexItemIterator scan;
        FlatItem item;
        flex_iterator_init(&scan, context, node, &style, NULL);
        int total = 0;
        size_t count = 0;
        widest = 0;
        while (flex_iterator_next(&scan, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            /* A definite generated box contributes its specified inline
               size, not its own (possibly zero) flex automatic minimum. */
            int contribution = item.generated_pseudo != PSEUDO_NONE
                ? (item.style.has_width && !item.style.width_percent
                    ? generated_inline_pseudo_width(context,node,&style,item.generated_pseudo,NULL)
                    : flex_child_row_minimum(context, &item, limit, false))
                : intrinsic_min_text_width_internal(
                    context, item.node, &item.parent_style, limit, false);
            if (contribution > widest) widest = contribution;
            if (count++ != 0) total = layout_add_coordinate(total, style.gap);
            total = layout_add_coordinate(total, contribution);
        }
        if (!style.flex_wrap) widest = total;
    } else {
        /* Definite generated decorations participate in min-content just
           like real children. Without them an auto grid track can squeeze
           an icon-only control down to its padding. */
        for (unsigned i = 0; i < 2; i++) {
            PseudoElement pseudo = i == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER;
            ComputedStyle generated = layout_style_for_pseudo(
                context, node, pseudo, &style);
            if (!generated.has_width || generated.width_percent) continue;
            int contribution = generated_inline_pseudo_width(
                context, node, &style, pseudo, NULL);
            if (contribution > widest) widest = contribution;
        }
    }
    if (style.display != DISPLAY_CONTENTS
        && style.display != DISPLAY_TABLE_ROW_GROUP
        && style.display != DISPLAY_TABLE_HEADER_GROUP
        && style.display != DISPLAY_TABLE_FOOTER_GROUP) {
        widest += style.border.left + style.border.right
                  + style.padding.left + style.padding.right
                  + style.margin.left + style.margin.right;
    }
    if (style.min_width_auto && style.max_width == STYLE_LENGTH_NONE) {
        return widest < limit ? widest : limit;
    }
    return constrain_intrinsic_outer_width(
        context, node, parent, &style, limit, widest);
}

int intrinsic_min_text_width(LayoutContext *context,
                                    lxb_dom_node_t *node,
                                    const ComputedStyle *parent, int limit)
{
    return intrinsic_min_text_width_internal(context, node, parent, limit,
                                             false);
}

void intrinsic_text_widths(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int limit,
    int *maximum, int *minimum)
{
    if (maximum == NULL || minimum == NULL) return;
    bool previous = context != NULL && context->intrinsic_pair_mode;
    if (context != NULL) context->intrinsic_pair_mode = true;
    *maximum = intrinsic_text_width(context, node, parent, limit);
    *minimum = intrinsic_min_text_width(context, node, parent, limit);
    if (context != NULL) context->intrinsic_pair_mode = previous;
}
