/* Flex and flat-item layout: intrinsic sizing, order plans, item
   iteration, line metrics, distribution, and alignment.
   Split out of layout.c. */

#include "layout_internal.h"
#include "tilefinch/integer_math.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int intrinsic_text_width_impl(LayoutContext *context,
                                     lxb_dom_node_t *node,
                                     const ComputedStyle *parent, int limit,
                                     bool include_positioned_root);
static int intrinsic_min_text_width_internal(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent, int limit, bool ignore_own_width);

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
 * The engine's existing twelve-track/eight-explicit-row limits keep both the
 * work and scratch fixed.  Recursion follows nested subgrids only and shares
 * the layout tree's depth ceiling and cancellation hook.
 */
static bool intrinsic_grid_track_widths(
    LayoutContext *context, lxb_dom_node_t *container,
    const ComputedStyle *container_style, int column_override,
    int row_override, int limit, bool minimum_phase, size_t depth,
    int widths[GRID_TRACK_LIMIT])
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

    uint8_t types[GRID_TRACK_LIMIT] = {0};
    int gap = computed_style_resolve_gap(container_style->gap, limit);
    for (int column = 0; column < columns; column++) {
        uint8_t type = column_override > 0
            ? GRID_TRACK_AUTO
            : stylesheet_grid_track_type(
                context->sheet, container_style, false, (unsigned) column);
        unsigned value = column_override > 0
            ? GRID_TRACK_AUTO_VALUE
            : stylesheet_grid_track_value(
                context->sheet, container_style, false, (unsigned) column);
        unsigned floor = column_override > 0 ? 0
            : stylesheet_grid_track_minimum(
                context->sheet, container_style, false, (unsigned) column);
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
    placement.explicit_columns = (uint8_t) columns;
    placement.explicit_rows = (uint8_t) rows;
    FlexOrderPlan order;
    if (!flex_order_plan_build(&order, context, container, container_style)) {
        return false;
    }
    FlexItemIterator scan;
    FlatItem item;
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
                    limit, minimum_phase, depth + 1, local)) {
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
    if (!intrinsic_grid_track_widths(
            context, node, style, 0, 0, limit, minimum_phase, 0, widths)) {
        return 0;
    }
    int columns = (int) stylesheet_grid_track_count(
        context->sheet, style, false);
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
               minimums)
           && intrinsic_grid_track_widths(
               context, node, style, columns, rows, limit, false, 0,
               maximums);
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
    if (context->reuse != NULL) {
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

static int intrinsic_replaced_width(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style)
{
    if (context == NULL || node == NULL || style == NULL) return 0;
    const ImageResource *image = images_find_node(context->images, node);
    int natural_width = image_resource_available(image)
        ? image_resource_intrinsic_width(image) : 0;
    int natural_height = image_resource_available(image)
        ? image_resource_intrinsic_height(image) : 0;
    if (natural_width <= 0
        && (layout_node_name_is(node, "video")
            || layout_node_name_is(node, "iframe"))) {
        natural_width = 300;
        natural_height = 150;
    }
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
    if (!style->has_width && style->has_height && !style->height_percent
        && natural_width > 0 && natural_height > 0) {
        int height = style_pixel_height(
            context->sheet, style, natural_width);
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
        }
    }
    return natural_width;
}

int intrinsic_text_width(LayoutContext *context, lxb_dom_node_t *node,
                                const ComputedStyle *parent, int limit)
{
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

static bool intrinsic_wraps_single_atomic(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *parent)
{
    bool found = false;
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
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
                || layout_node_name_is(child, "img")
                || layout_node_name_is(child, "svg")
                || layout_node_name_is(child, "video");
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
        const char *text = document_text_data(node, &length);
        if (text == NULL) return 0;
        const FontFace *face = font_context_face_variant(
            context->fonts, context->web_fonts, parent->font_family,
            parent->font_italic, style_uses_bold_face(parent));
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
            int word_fixed = measured_text_width_fixed(
                face, metric_family, text + at, end - at,
                computed_style_font_size_fixed(parent),
                synthetic_bold, metric_bold, parent->font_scale,
                parent->letter_spacing);
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
                    int segment_width = discard ? 0 : measured_text_width(
                        face, metric_family, text + segment_at,
                        segment_length,
                        computed_style_font_size_fixed(parent),
                        synthetic_bold, metric_bold, parent->font_scale,
                        parent->letter_spacing);
                    if (break_for_minimum && !discard) {
                        segment_width = 0;
                        size_t segment_end = segment_at + segment_length;
                        for (size_t character_at = segment_at;
                             character_at < segment_end;) {
                        size_t character_length = utf8_character_length(
                            text + character_at,
                            segment_end - character_at);
                        if (character_length == 0) break;
                        int character_width = measured_text_width(
                            face, metric_family, text + character_at,
                            character_length,
                            computed_style_font_size_fixed(parent),
                            synthetic_bold, metric_bold,
                            parent->font_scale, 0);
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
                              layout_fixed_from_integer(
                                  parent->letter_spacing * 2))
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
    if (style.has_width && !style.width_max_content
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
    if (layout_node_name_is(node, "img") || layout_node_name_is(node, "svg")
        || layout_node_name_is(node, "video")
        || layout_node_name_is(node, "iframe")) {
        int replaced_width = intrinsic_replaced_width(
            context, node, &style);
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
        && (style.appearance & STYLE_APPEARANCE_MASK) != APPEARANCE_NONE) {
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
    const ImageResource *mask = style.mask_image != NULL
        ? images_find_mask_source(
            context->images, node, style.mask_image, PSEUDO_NONE)
        : images_find_mask_node(context->images, node);
    if (image_resource_available(mask)) {
        int width = style.has_width
                    ? resolve_declared_length(
                        context->sheet, style.width,
                        style.width_percent, limit)
                    : mask->width;
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
    int inline_width = generated_inline_pseudo_width(
        context, node, &style, PSEUDO_BEFORE);
    int block_width = 0;
    size_t horizontal_flex_items = 0;
    /* Flex layout promotes the children of display:contents wrappers to
       flex items. Intrinsic sizing must walk that same flattened sequence:
       measuring the wrapper as one ordinary block returns only its widest
       child, so a max-content horizontal navigation is clipped even though
       its items were positioned side by side. */
    if (horizontal_flex) {
        FlatItemIterator iterator;
        FlatItem item;
        flat_iterator_init(&iterator, context, node, &style);
        while (flat_iterator_next(&iterator, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) {
                continue;
            }
            int item_width = intrinsic_text_width(
                context, item.node, &item.parent_style, limit);
            if (item_width <= 0) continue;
            int gap = horizontal_flex_items == 0
                ? 0 : computed_style_resolve_gap(style.gap, limit);
            if (inline_width > limit - gap
                || inline_width + gap > limit - item_width) {
                return limit;
            }
            inline_width += gap + item_width;
            horizontal_flex_items++;
        }
        int after_width = generated_inline_pseudo_width(
            context, node, &style, PSEUDO_AFTER);
        if (inline_width > limit - after_width) return limit;
        block_width = inline_width + after_width;
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
    int boundary_space_width = -1;
    bool pending_boundary_space = false;
    bool has_inline_text = false;
    bool previous_atomic = false;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT
            && layout_node_name_is(child, "br")) {
            if (inline_width > block_width) block_width = inline_width;
            inline_width = 0;
            pending_boundary_space = false;
            has_inline_text = false;
            previous_atomic = false;
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
                || layout_node_name_is(child, "img")
                || layout_node_name_is(child, "svg")
                || layout_node_name_is(child, "video");
        }
        bool leading_space = false, trailing_space = false;
        bool child_has_text = false;
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t text_length = 0;
            const char *text = document_text_data(child, &text_length);
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
        if (block) {
            if (inline_width > block_width) block_width = inline_width;
            inline_width = 0;
            pending_boundary_space = false;
            has_inline_text = false;
            previous_atomic = false;
            if (child_width > block_width) block_width = child_width;
        } else {
            if (child_width > 0) {
                if (has_inline_text && child_has_unit
                    && (pending_boundary_space || leading_space)) {
                    if (boundary_space_width < 0) {
                        const FontFace *space_face =
                            font_context_face_variant(
                                context->fonts, context->web_fonts,
                                style.font_family, style.font_italic,
                                style_uses_bold_face(&style));
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
                                    + style.letter_spacing * 2;
                } else if (has_inline_text && child_has_unit
                           && !(previous_atomic && child_atomic)) {
                    inline_width += style.letter_spacing;
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
    inline_width += generated_inline_pseudo_width(
        context, node, &style, PSEUDO_AFTER);
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

static int intrinsic_min_text_width_internal(LayoutContext *context,
                                             lxb_dom_node_t *node,
                                             const ComputedStyle *parent,
                                             int limit,
                                             bool ignore_own_width)
{
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
        const char *text = document_text_data(node, &length);
        if (text == NULL) return 0;
        if (parent->white_space_mode == WHITE_SPACE_NOWRAP
            || parent->white_space_mode == WHITE_SPACE_PRE) {
            return intrinsic_text_width(context, node, parent, limit);
        }
        const FontFace *face = font_context_face_variant(
            context->fonts, context->web_fonts, parent->font_family,
            parent->font_italic, style_uses_bold_face(parent));
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
                int segment_width = discard ? 0 : measured_text_width(
                    face, metric_family, text + segment_at, segment_length,
                    computed_style_font_size_fixed(parent),
                    synthetic_bold, metric_bold, parent->font_scale,
                    parent->letter_spacing);
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
                        int character_width = measured_text_width(
                            face, metric_family, text + character_at,
                            character_length,
                            computed_style_font_size_fixed(parent),
                            synthetic_bold, metric_bold,
                            parent->font_scale, 0);
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
    if (layout_node_name_is(node, "img") || layout_node_name_is(node, "svg")
        || layout_node_name_is(node, "video")
        || layout_node_name_is(node, "iframe")) {
        int replaced_width = intrinsic_replaced_width(
            context, node, &style);
        if (replaced_width > 0) {
            /* A percentage-sized HTML image has a cyclic inline-size while
               its containing block is being measured at min-content.  That
               percentage contributes zero here; the final replaced size is
               resolved after flex/grid assigns a definite containing width.
               Inline SVG retains its own intrinsic sizing rules. */
            int width = layout_node_name_is(node, "img") && style.has_width
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
        && (style.appearance & STYLE_APPEARANCE_MASK) != APPEARANCE_NONE) {
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
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        int child_width = intrinsic_min_text_width_internal(
            context, child, &style, limit, false);
        if (child_width > widest) widest = child_width;
        if (widest >= limit) return limit;
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

int grid_required_columns(const ComputedStyle *style, int current_columns)
{
    int required = current_columns < 1 ? 1 : current_columns;
    int start = computed_style_grid_column_start(style);
    int end = computed_style_grid_column_end(style);
    int span = computed_style_grid_column_span(style);
    if (start > 0 && !computed_style_grid_line_is_negative(start)) {
        int last = start - 1 + (span > 0 ? span : 1);
        if (end > start && !computed_style_grid_line_is_negative(end)) {
            last = end - 1;
        }
        if (last > required) required = last;
    } else if (end > 1 && !computed_style_grid_line_is_negative(end)) {
        int last = end - 1;
        if (last > required) required = last;
    } else if (span > required) {
        required = span;
    }
    if (required > GRID_TRACK_LIMIT) required = GRID_TRACK_LIMIT;
    return required;
}

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
    unsigned mask = ((1u << column_span) - 1u) << column;
    for (int offset = 0; offset < row_span; offset++) {
        if ((state->occupied[row + offset] & mask) != 0) return false;
    }
    return true;
}

static void grid_placement_mark(GridPlacementState *state,
                                int row, int column,
                                int row_span, int column_span)
{
    unsigned mask = ((1u << column_span) - 1u) << column;
    for (int offset = 0; offset < row_span; offset++) {
        state->occupied[row + offset] |= (uint16_t) mask;
    }
}

static int grid_line_position(unsigned encoded, int origin,
                              int explicit_tracks)
{
    int line = computed_style_decode_grid_line(encoded);
    return line < 0 ? origin + explicit_tracks + 1 + line
                    : origin + line - 1;
}

bool grid_place_item(GridPlacementState *state, const ComputedStyle *style,
                     GridItemPlacement *placement)
{
    if (state == NULL || style == NULL || placement == NULL
        || state->columns == 0) return false;
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

    bool found = false;
    if (explicit_row && explicit_column) {
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
    if (!explicit_row || !explicit_column) {
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

AlignItems flex_item_alignment(const ComputedStyle *container,
                                      const ComputedStyle *item)
{
    if (item->align_self == ALIGN_SELF_START) return ALIGN_START;
    if (item->align_self == ALIGN_SELF_CENTER) return ALIGN_CENTER;
    if (item->align_self == ALIGN_SELF_END) return ALIGN_END;
    if (item->align_self == ALIGN_SELF_STRETCH) return ALIGN_STRETCH;
    if (item->align_self == ALIGN_SELF_BASELINE) return ALIGN_BASELINE;
    return container->align_items;
}

/* Expand an auto-sized row-flex item to its used cross size after the line's
   tallest item is known. The retained background/border geometry and the
   item's own full-box interaction region grow in place; descendants keep
   their normal block position. */
static void stretch_flex_item_cross(LayoutDocument *layout,
                                    lxb_dom_node_t *node,
                                    const ComputedStyle *style,
                                    int target_height)
{
    LayoutNodeBox *box = layout_box_for_node_mutable(layout, node);
    if (box == NULL || target_height <= box->height) return;
    size_t box_index = (size_t) (box - layout->node_boxes);
    if (layout->node_interaction_ranges == NULL
        || box_index >= layout->node_interaction_capacity) return;
    const uint32_t *interactions =
        layout->node_interaction_ranges
        + box_index * LAYOUT_NODE_INTERACTION_STRIDE;
    int old_height = box->height;
    int delta = target_height - old_height;
    int old_bottom = box->y + old_height;
    size_t command_end = box->command_end < layout->count
                         ? box->command_end : layout->count;
    for (size_t i = box->command_start; i < command_end; i++) {
        if (i >= box->scroll_command_start
            && i < box->scroll_command_end) continue;
        DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT) continue;
        if (style->border.bottom > 0
            && command->y == old_bottom - style->border.bottom
            && command->height == style->border.bottom) {
            command->y += delta;
        } else if (command->y == box->y
                   && command->height == old_height) {
            command->height = target_height;
        } else if (command->y == box->y + style->border.top
                   && command->height
                      == old_height - style->border.top
                                      - style->border.bottom) {
            command->height += delta;
        }
    }
    size_t link_start =
        interactions[LAYOUT_NODE_LINK_START] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_START] : layout->link_count;
    size_t link_end =
        interactions[LAYOUT_NODE_LINK_END] < layout->link_count
        ? interactions[LAYOUT_NODE_LINK_END] : layout->link_count;
    for (size_t i = link_start; i < link_end; i++) {
        LinkRegion *link = &layout->links[i];
        if (link->node == node) {
            link->x = box->x;
            link->y = box->y;
            link->width = box->width;
            link->height = target_height;
        }
    }
    size_t control_start =
        interactions[LAYOUT_NODE_CONTROL_START] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_START] : layout->control_count;
    size_t control_end =
        interactions[LAYOUT_NODE_CONTROL_END] < layout->control_count
        ? interactions[LAYOUT_NODE_CONTROL_END] : layout->control_count;
    for (size_t i = control_start; i < control_end; i++) {
        ControlRegion *control = &layout->controls[i];
        if (control->node == node) {
            control->y = box->y;
            control->height = target_height;
        }
    }
    box->height = target_height;
    box->client_height += delta;
    if (box->content_height < box->client_height) {
        box->content_height = box->client_height;
    }
}

/* Move the in-flow contents of a table cell within its row-height shell.  An
   absolutely/fixed positioned descendant is positioned against its
   containing block, not the cell's vertical-align content offset.  Move the
   retained range first, then undo that translation once for every top-level
   out-of-flow subtree so paint, hit regions and node geometry stay coherent. */

static bool text_has_content(lxb_dom_node_t *node)
{
    size_t length = 0;
    const char *text = document_text_data(node, &length);
    if (text == NULL) return false;
    for (size_t i = 0; i < length; i++) {
        if (!isspace((unsigned char) text[i])) return true;
    }
    return false;
}

static lxb_dom_node_t *flat_node_after(lxb_dom_node_t *node,
                                       lxb_dom_node_t *container)
{
    while (node != NULL && node != container) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static bool flat_parent_style(LayoutContext *context,
                              lxb_dom_node_t *container,
                              const ComputedStyle *container_style,
                              lxb_dom_node_t *parent,
                              ComputedStyle *result)
{
    if (parent == container) {
        *result = *container_style;
        return true;
    }
    lxb_dom_node_t *path[LAYOUT_TREE_CALL_DEPTH_LIMIT];
    size_t count = 0;
    for (lxb_dom_node_t *at = parent; at != container; at = at->parent) {
        if (at == NULL || at->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return false;
        }
        if (count == LAYOUT_TREE_CALL_DEPTH_LIMIT) {
            layout_tree_note_fallback(context, at, "display-contents");
            *result = *container_style;
            return true;
        }
        path[count++] = at;
    }
    *result = *container_style;
    while (count != 0) {
        lxb_dom_node_t *at = path[--count];
        ComputedStyle style = layout_style_for_node(context, at, result);
        bool flattened = style.display == DISPLAY_CONTENTS
            || style.display == DISPLAY_TABLE_ROW_GROUP
            || style.display == DISPLAY_TABLE_HEADER_GROUP
            || style.display == DISPLAY_TABLE_FOOTER_GROUP;
        if (!flattened || style.hidden) return false;
        *result = style;
    }
    return true;
}

void flat_iterator_init(FlatItemIterator *iterator,
                               LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *container_style)
{
    if (context != NULL) context->flat_iterator_passes++;
    *iterator = (FlatItemIterator) {
        .context = context,
        .container = container,
        .cursor = container != NULL ? container->first_child : NULL,
        .container_style = *container_style
    };
}

__attribute__((noinline))
bool flat_iterator_next(FlatItemIterator *iterator, FlatItem *item)
{
    while (iterator->cursor != NULL) {
        lxb_dom_node_t *node = iterator->cursor;
        iterator->cursor = flat_node_after(node, iterator->container);
        ComputedStyle parent_style;
        if (!flat_parent_style(iterator->context, iterator->container,
                               &iterator->container_style, node->parent,
                               &parent_style)) continue;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            if (!iterator->include_whitespace && !text_has_content(node)) {
                continue;
            }
            *item = (FlatItem) {
                .node = node,
                .parent_style = parent_style,
                .style = layout_style_for_node(iterator->context, node,
                                               &parent_style),
                .anonymous_text = true
            };
            iterator->context->flat_iterator_yields++;
            return true;
        }
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (layout_node_is_hidden_input(node)) continue;
        ComputedStyle style = layout_style_for_node(
            iterator->context, node, &parent_style);
        if (style.display == DISPLAY_NONE
            || style.display == DISPLAY_TABLE_COLUMN || style.hidden) {
            continue;
        }
        if (style.display == DISPLAY_CONTENTS
            || style.display == DISPLAY_TABLE_ROW_GROUP
            || style.display == DISPLAY_TABLE_HEADER_GROUP
            || style.display == DISPLAY_TABLE_FOOTER_GROUP) {
            if (node->first_child != NULL) iterator->cursor = node->first_child;
            continue;
        }
        *item = (FlatItem) {
            .node = node,
            .parent_style = parent_style,
            .style = style,
            .anonymous_text = false
        };
        iterator->context->flat_iterator_yields++;
        return true;
    }
    return false;
}

/* The top margin exposed by a borderless block may include the first
   in-flow block descendant's margin.  Resolve that bounded chain before the
   caller places the parent's border box; doing it only inside the parent
   would leave the parent and first child at different y coordinates. */

void flex_order_plan_destroy(FlexOrderPlan *plan)
{
    if (plan == NULL) return;
    if (plan->values != NULL && plan->values != plan->inline_values) {
        budget_free(plan->budget, plan->values);
    }
    plan->values = NULL;
    plan->count = 0;
    plan->capacity = 0;
    plan->active = false;
}

bool flex_order_plan_build(FlexOrderPlan *plan,
                                  LayoutContext *context,
                                  lxb_dom_node_t *container,
                                  const ComputedStyle *style)
{
    *plan = (FlexOrderPlan) {.budget = context->layout->budget};
    plan->values = plan->inline_values;
    plan->capacity = sizeof(plan->inline_values)
                     / sizeof(plan->inline_values[0]);
    FlatItemIterator scan;
    FlatItem item;
    flat_iterator_init(&scan, context, container, style);
    while (flat_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        size_t insert = 0;
        while (insert < plan->count && plan->values[insert] < item.style.order) {
            insert++;
        }
        if (insert < plan->count && plan->values[insert] == item.style.order) {
            continue;
        }
        if (plan->count == plan->capacity) {
            size_t capacity = plan->capacity * 2;
            int *values;
            if (plan->values == plan->inline_values) {
                values = budget_malloc(plan->budget,
                                       capacity * sizeof(*values));
                if (values != NULL) {
                    memcpy(values, plan->inline_values,
                           plan->count * sizeof(*values));
                }
            } else {
                values = budget_realloc(plan->budget, plan->values,
                                        capacity * sizeof(*values));
            }
            if (values == NULL) {
                flex_order_plan_destroy(plan);
                return false;
            }
            plan->values = values;
            plan->capacity = capacity;
        }
        if (insert < plan->count) {
            memmove(plan->values + insert + 1, plan->values + insert,
                    (plan->count - insert) * sizeof(*plan->values));
        }
        plan->values[insert] = item.style.order;
        plan->count++;
    }
    plan->active = plan->count > 1
                   || (plan->count == 1 && plan->values[0] != 0);
    return true;
}

void flex_iterator_init(FlexItemIterator *iterator,
                               LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *style,
                               const FlexOrderPlan *plan)
{
    if (context != NULL) context->flex_iterator_passes++;
    *iterator = (FlexItemIterator) {.plan = plan};
    flat_iterator_init(&iterator->source, context, container, style);
}

__attribute__((noinline))
bool flex_iterator_next(FlexItemIterator *iterator, FlatItem *item)
{
    if (iterator->plan == NULL || !iterator->plan->active) {
        bool found = flat_iterator_next(&iterator->source, item);
        if (found) iterator->source.context->flex_iterator_yields++;
        return found;
    }
    while (iterator->order_index < iterator->plan->count) {
        while (flat_iterator_next(&iterator->source, item)) {
            if (item->style.order
                == iterator->plan->values[iterator->order_index]) {
                iterator->source.context->flex_iterator_yields++;
                return true;
            }
        }
        iterator->order_index++;
        if (iterator->order_index < iterator->plan->count) {
            LayoutContext *context = iterator->source.context;
            lxb_dom_node_t *container = iterator->source.container;
            ComputedStyle container_style = iterator->source.container_style;
            flat_iterator_init(&iterator->source,
                               context, container, &container_style);
        }
    }
    return false;
}

void flat_text_link(lxb_dom_node_t *node, lxb_dom_node_t *container,
                           const char **url, size_t *url_length,
                           lxb_dom_node_t **link_node)
{
    *url = NULL;
    *url_length = 0;
    *link_node = NULL;
    for (lxb_dom_node_t *at = node != NULL ? node->parent : NULL;
         at != NULL && at != container; at = at->parent) {
        if (!layout_node_name_is(at, "a")) continue;
        const char *href = document_attribute(at, "href", url_length);
        if (href != NULL && *url_length != 0) {
            *url = href;
            *link_node = at;
        }
        return;
    }
}

bool layout_anonymous_text(LayoutContext *context,
                                  const FlatItem *item,
                                  lxb_dom_node_t *container,
                                  int x, int y, int width, int *bottom)
{
    size_t command_start = context->layout->count;
    LineState line = {
        .start_x = x,
        .x = x,
        .right = x + (width < 8 ? 8 : width),
        .y = y,
        .line_gap = 0,
        .layout = context->layout,
        .command_start = context->layout->count,
        .link_start = context->layout->link_count,
        .control_start = context->layout->control_count,
        .node_box_start = context->layout->node_box_count,
        .text_align = computed_style_used_text_align(&item->style),
        .direction_rtl = computed_style_direction_rtl(&item->style),
        .find_block_start = true
    };
    size_t length = 0;
    const char *text = document_text_data(item->node, &length);
    const char *link_url = NULL;
    size_t link_length = 0;
    lxb_dom_node_t *link_node = NULL;
    flat_text_link(item->node, container, &link_url, &link_length, &link_node);
    if (text != NULL
        && !flow_text(context, &line, text, length, &item->style,
                      link_url, link_length, link_node)) return false;
    layout_flush_line(&line);
    line_finish_vertical(&line);
    /* Collapsible whitespace at the start or end of an anonymous inline box
       does not generate a line box.  In particular, indentation around block
       children must not acquire a synthetic minimum height. */
    *bottom = line.y;
    if (!add_node_box(context->layout, item->node, x, y, width,
                      *bottom - y, width, *bottom - y,
                      width, *bottom - y,
                      item->style.padding.left + item->style.padding.right,
                      item->style.padding.top + item->style.padding.bottom,
                      false, false, 0, 0, 0, 0,
                      false, false,
                      false, true,
                      command_start, context->layout->count,
                      command_start, context->layout->count,
                      line.link_start, context->layout->link_count,
                      line.control_start,
                      context->layout->control_count)) return false;
    return true;
}

int flex_child_basis(LayoutContext *context, const FlatItem *item,
                            int content_width)
{
    context->flex_basis_resolutions++;
    ComputedStyle resolved_style = item->style;
    resolve_padding(context->sheet, &resolved_style, content_width);
    resolve_margin(context->sheet, &resolved_style, content_width);
    const ComputedStyle *child_style = &resolved_style;
    int basis;
    bool explicit_size = false;
    if (child_style->has_flex_basis) {
        if (child_style->flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            basis = intrinsic_min_text_width_ignoring_own_width(
                context, item->node, &item->parent_style, content_width);
        } else if (child_style->flex_basis == STYLE_LENGTH_MAX_CONTENT
                   || child_style->flex_basis
                          == STYLE_LENGTH_FIT_CONTENT) {
            basis = intrinsic_text_width(
                context, item->node, &item->parent_style,
                child_style->flex_basis == STYLE_LENGTH_MAX_CONTENT
                    ? LAYOUT_COORDINATE_LIMIT : content_width);
        } else {
            basis = resolve_declared_length(
                context->sheet, child_style->flex_basis,
                child_style->flex_basis_percent, content_width);
            explicit_size = true;
        }
    } else if (child_style->width_max_content) {
        basis = computed_style_width_min_content(child_style)
            ? intrinsic_min_text_width_ignoring_own_width(
                  context, item->node, &item->parent_style, content_width)
            : intrinsic_text_width(context, item->node,
                                   &item->parent_style,
                                   computed_style_width_max_content(child_style)
                                       ? LAYOUT_COORDINATE_LIMIT
                                       : content_width);
    } else if (child_style->has_width) {
        basis = resolve_declared_length(
            context->sheet, child_style->width,
            child_style->width_percent, content_width);
        explicit_size = true;
    } else {
        basis = intrinsic_text_width(context, item->node,
                                     &item->parent_style, content_width);
    }
    int edges = child_style->padding.left + child_style->padding.right
                + child_style->border.left + child_style->border.right;
    int margins = child_style->margin.left + child_style->margin.right;
    if (explicit_size) {
        if (child_style->box_sizing_border_box) {
            if (basis < edges) basis = edges;
        } else {
            basis += edges;
        }
        basis += margins;
    }
    if (basis < margins) basis = margins;
    basis = constrain_border_box_width(
        context, item->node, &item->parent_style, child_style,
        content_width, basis - margins, NULL) + margins;
    if (basis < 8) basis = 8;
    if (basis > content_width) basis = content_width;
    return basis;
}

static int flex_child_minimum(LayoutContext *context, const FlatItem *item,
                              int content_width)
{
    context->flex_minimum_resolutions++;
    ComputedStyle resolved_style = item->style;
    resolve_padding(context->sheet, &resolved_style, content_width);
    resolve_margin(context->sheet, &resolved_style, content_width);
    const ComputedStyle *style = &resolved_style;
    if (style->min_width_auto) {
        /* `clip` clips paint but is deliberately not a scrollable overflow
           value. Flexbox therefore keeps the item's content-based automatic
           minimum for clip, while hidden/auto/scroll suppress it. */
        if (style->overflow_x_scroll
            && !style->overflow_x_clip_only) return 0;
        int minimum = intrinsic_min_text_width_internal(
            context, item->node, &item->parent_style, content_width, true);
        if (style->has_width && !style->width_max_content) {
            int margins = style->margin.left + style->margin.right;
            int edges = style->padding.left + style->padding.right
                        + style->border.left + style->border.right;
            int specified = resolve_declared_length(
                context->sheet, style->width, style->width_percent,
                content_width) + margins;
            if (!style->box_sizing_border_box) specified += edges;
            if (minimum > specified) minimum = specified;
        }
        return minimum;
    }
    int margins = style->margin.left + style->margin.right;
    int minimum = constrain_border_box_width(
        context, item->node, &item->parent_style, style,
        content_width, 0, NULL) + margins;
    return minimum > 0 ? minimum : 0;
}

int flex_child_row_minimum(LayoutContext *context,
                                  const FlatItem *item, int content_width,
                                  bool css_table_row)
{
    if (css_table_row && item->style.has_width
        && item->style.width_percent) {
        /* Percentage cell widths participate in table distribution; they do
           not cap the cell's content minimum as a definite width would. */
        return intrinsic_min_text_width_internal(
            context, item->node, &item->parent_style,
            content_width, true);
    }
    return flex_child_minimum(context, item, content_width);
}

/*
 * Measure one wrapped flex line without retaining a per-child side table.
 * The caller starts this scan at the first in-flow item on the line, so each
 * item participates in one bounded look-ahead scan and one layout pass.
 */
FlexLineMetrics flex_line_metrics(LayoutContext *context,
                                         FlexItemIterator *iterator,
                                         FlatItem *item,
                                         int content_width, int gap)
{
    FlexLineMetrics metrics = {0};
    while (flex_iterator_next(iterator, item)) {
        if (item->style.out_of_flow || item->style.fixed_position) {
            continue;
        }
        int basis = flex_child_basis(context, item, content_width);
        int occupied = metrics.basis;
        if (metrics.count != 0) occupied += gap * (int) metrics.count;
        if (metrics.count != 0 && occupied + basis > content_width) break;
        metrics.count++;
        metrics.basis += basis;
        metrics.grow += item->style.flex_grow;
        if (item->style.margin_left_auto) metrics.auto_main_margins++;
        if (item->style.margin_right_auto) metrics.auto_main_margins++;
    }
    return metrics;
}

static bool flex_item_cross_metrics(const LayoutDocument *layout,
                                    const FlatItem *item,
                                    int *origin, int *height)
{
    const LayoutNodeBox *box = layout_box_for_node(layout, item->node);
    if (box == NULL) return false;
    *origin = box->y - item->style.margin.top;
    *height = box->height + item->style.margin.top
              + item->style.margin.bottom;
    return true;
}

/* Return the first in-flow text baseline relative to the item's margin-box
   top. A textless flex item synthesizes its baseline at the margin-box end. */
static bool flex_item_baseline(LayoutContext *context, const FlatItem *item,
                               int origin, int height, int *baseline)
{
    const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                   item->node);
    if (box == NULL) return false;
    size_t end = box->command_end < context->layout->count
                 ? box->command_end : context->layout->count;
    for (size_t i = box->command_start; i < end; i++) {
        const DrawCommand *command = &context->layout->commands[i];
        if (command->type != DRAW_TEXT || command->text_length == 0) continue;
        const FontFace *face = font_context_face_variant(
            context->fonts, context->web_fonts,
            draw_command_font_family(command),
            draw_command_font_italic(command), draw_uses_bold_face(command));
        int ascent = font_ascent_at_size(
            face, draw_command_text_font_size_fixed(command));
        if (ascent < 0) ascent = 6 * (command->scale > 0 ? command->scale : 1);
        *baseline = command->y + ascent - origin;
        if (*baseline < 0) *baseline = 0;
        if (*baseline > height) *baseline = height;
        return true;
    }
    *baseline = height;
    return true;
}

void reverse_flex_row_items(LayoutContext *context,
                                   lxb_dom_node_t *container,
                                   const ComputedStyle *style,
                                   const FlexOrderPlan *order_plan,
                                   int content_x, int content_width)
{
    if (style->flex_direction != FLEX_ROW_REVERSE) return;
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, container, style, order_plan);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                       item.node);
        if (box == NULL) continue;
        int target_x = content_x + content_width
                       - (box->x - content_x) - box->width
                       + item.style.margin.left - item.style.margin.right;
        int dx = target_x - box->x;
        if (dx != 0) {
            trace_flex_translation(context, "row-reverse", container,
                                   item.node, style->align_items,
                                   content_x, content_width, box->x,
                                   box->width, target_x, dx);
            translate_node_subtree(context->layout, item.node, dx, 0);
        }
    }
}

/* Reflect wrapped rows across the used cross axis. Performing this after
   line packing and per-item alignment makes cross-start/end, baseline,
   automatic margins, links, and controls follow the reversed axis without a
   retained line table. Physical margins keep their authored sides. */
void reverse_flex_cross_items(LayoutContext *context,
                                     lxb_dom_node_t *container,
                                     const ComputedStyle *style,
                                     const FlexOrderPlan *order_plan,
                                     int content_top, int content_height)
{
    if (!style->flex_wrap_reverse || content_height <= 0) return;
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, container, style, order_plan);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                       item.node);
        if (box == NULL) continue;
        int target_y = content_top + content_height
                       - (box->y - content_top) - box->height
                       + item.style.margin.top - item.style.margin.bottom;
        int dy = target_y - box->y;
        if (dy != 0) {
            translate_node_subtree(context->layout, item.node, 0, dy);
        }
    }
}

void reverse_flex_column_items(LayoutContext *context,
                                      lxb_dom_node_t *container,
                                      const ComputedStyle *style,
                                      const FlexOrderPlan *order_plan,
                                      int content_top, int content_height)
{
    if (style->flex_direction != FLEX_COLUMN_REVERSE
        || content_height <= 0) return;
    FlexItemIterator iterator;
    FlatItem item;
    flex_iterator_init(&iterator, context, container, style, order_plan);
    while (flex_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                       item.node);
        if (box == NULL) continue;
        int target_y = content_top + content_height
                       - (box->y - content_top) - box->height
                       + item.style.margin.top - item.style.margin.bottom;
        int dy = target_y - box->y;
        if (dy != 0) {
            translate_node_subtree(context->layout, item.node, 0, dy);
        }
    }
}

/* Pack a column-axis flex container after its children have been measured.
   Only two integers per possible track are transiently retained; item
   geometry remains in the existing node-box table.  This keeps wrapping
   bounded by the page budget without a second retained flex layout tree. */
bool place_wrapped_flex_columns(LayoutContext *context,
                               lxb_dom_node_t *container,
                               const ComputedStyle *style,
                               const FlexOrderPlan *order_plan,
                               int content_x, int content_width,
                               int content_top, int content_height,
                               size_t item_count)
{
    if (item_count == 0 || content_height <= 0) return true;
    if (item_count > SIZE_MAX / (5 * sizeof(int))) return false;
    int *tracks = budget_malloc(context->layout->budget,
                                item_count * 5 * sizeof(*tracks));
    if (tracks == NULL) return false;
    int *widths = tracks;
    int *origins = tracks + item_count;
    int *used_heights = origins + item_count;
    int *item_counts = used_heights + item_count;
    int *auto_margins = item_counts + item_count;
    memset(tracks, 0, item_count * 5 * sizeof(*tracks));

    FlexItemIterator scan;
    FlatItem item;
    flex_iterator_init(&scan, context, container, style, order_plan);
    size_t column = 0;
    int used_height = 0;
    while (flex_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                       item.node);
        if (box == NULL) continue;
        int outer_height = box->height + item.style.margin.top
                           + item.style.margin.bottom;
        int next_height = used_height == 0
                          ? outer_height
                          : used_height + style->row_gap + outer_height;
        if (used_height != 0 && next_height > content_height
            && column + 1 < item_count) {
            used_heights[column] = used_height;
            column++;
            used_height = outer_height;
        } else {
            used_height = next_height;
        }
        item_counts[column]++;
        if (item.style.margin_top_auto) auto_margins[column]++;
        if (item.style.margin_bottom_auto) auto_margins[column]++;
        int outer_width = box->width + item.style.margin.left
                          + item.style.margin.right;
        if (outer_width > widths[column]) widths[column] = outer_width;
    }
    used_heights[column] = used_height;
    size_t column_count = column + 1;
    int natural_width = style->gap * (int) (column_count - 1);
    for (size_t i = 0; i < column_count; i++) {
        natural_width = layout_add_coordinate(natural_width, widths[i]);
    }
    int free_width = content_width - natural_width;
    if (free_width < 0) free_width = 0;
    int initial_offset = 0;
    int between_extra = 0;
    int stretch_each = 0;
    int stretch_remainder = 0;
    if (style->align_content == JUSTIFY_CENTER) {
        initial_offset = free_width / 2;
    } else if (style->align_content == JUSTIFY_END) {
        initial_offset = free_width;
    } else if (style->align_content == JUSTIFY_SPACE_BETWEEN
               && column_count > 1) {
        between_extra = free_width / (int) (column_count - 1);
    } else if (style->align_content == JUSTIFY_SPACE_AROUND) {
        between_extra = free_width / (int) column_count;
        initial_offset = between_extra / 2;
    } else if (style->align_content == JUSTIFY_SPACE_EVENLY) {
        between_extra = free_width / (int) (column_count + 1);
        initial_offset = between_extra;
    } else if (style->align_content == JUSTIFY_STRETCH) {
        stretch_each = free_width / (int) column_count;
        stretch_remainder = free_width % (int) column_count;
    }

    if (!style->flex_wrap_reverse) {
        int cursor = content_x + initial_offset;
        for (size_t i = 0; i < column_count; i++) {
            origins[i] = cursor;
            int stretch = stretch_each
                          + ((int) i < stretch_remainder ? 1 : 0);
            cursor = layout_add_coordinate(
                cursor, widths[i] + stretch + style->gap + between_extra);
        }
    } else {
        int cursor = content_x + content_width - initial_offset;
        for (size_t i = 0; i < column_count; i++) {
            int stretch = stretch_each
                          + ((int) i < stretch_remainder ? 1 : 0);
            origins[i] = cursor - widths[i] - stretch;
            cursor = layout_subtract_coordinate(
                origins[i], style->gap + between_extra);
        }
    }

    FlexItemIterator place;
    flex_iterator_init(&place, context, container, style, order_plan);
    column = 0;
    used_height = 0;
    int item_index = 0;
    int auto_margin_seen = 0;
    while (flex_iterator_next(&place, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        const LayoutNodeBox *box = layout_box_for_node(context->layout,
                                                       item.node);
        if (box == NULL) continue;
        int outer_height = box->height + item.style.margin.top
                           + item.style.margin.bottom;
        int next_height = used_height == 0
                          ? outer_height
                          : used_height + style->row_gap + outer_height;
        if (used_height != 0 && next_height > content_height
            && column + 1 < column_count) {
            column++;
            used_height = 0;
            item_index = 0;
            auto_margin_seen = 0;
        }
        int before = used_height == 0 ? 0 : style->row_gap;
        used_height += before;
        int free_height = content_height - used_heights[column];
        if (free_height < 0) free_height = 0;
        int main_offset = 0;
        if (auto_margins[column] != 0) {
            bool auto_before = style->flex_direction == FLEX_COLUMN_REVERSE
                ? item.style.margin_bottom_auto
                : item.style.margin_top_auto;
            if (auto_before) auto_margin_seen++;
            main_offset = tilefinch_mul_div_int(
                free_height, auto_margin_seen, auto_margins[column]);
        } else if (style->justify_content == JUSTIFY_CENTER) {
            main_offset = free_height / 2;
        } else if (style->justify_content == JUSTIFY_END) {
            main_offset = free_height;
        } else if (style->justify_content == JUSTIFY_SPACE_BETWEEN
                   && item_counts[column] > 1) {
            main_offset = tilefinch_mul_div_int(
                free_height, item_index, item_counts[column] - 1);
        } else if (style->justify_content == JUSTIFY_SPACE_AROUND) {
            main_offset = tilefinch_mul_div_int(
                free_height, 2 * item_index + 1, 2 * item_counts[column]);
        } else if (style->justify_content == JUSTIFY_SPACE_EVENLY) {
            main_offset = tilefinch_mul_div_int(
                free_height, item_index + 1, item_counts[column] + 1);
        }
        int target_y;
        if (style->flex_direction == FLEX_COLUMN_REVERSE) {
            target_y = content_top + content_height - used_height
                       - item.style.margin.bottom - box->height
                       - main_offset;
        } else {
            target_y = content_top + used_height + item.style.margin.top
                       + main_offset;
        }
        int track_width = widths[column] + stretch_each
            + ((int) column < stretch_remainder ? 1 : 0);
        int target_x = style->flex_wrap_reverse
            ? origins[column] + track_width - item.style.margin.right
              - box->width
            : origins[column] + item.style.margin.left;
        translate_node_subtree(context->layout, item.node,
                               target_x - box->x, target_y - box->y);
        used_height += outer_height;
        if (auto_margins[column] != 0) {
            bool auto_after = style->flex_direction == FLEX_COLUMN_REVERSE
                ? item.style.margin_top_auto
                : item.style.margin_bottom_auto;
            if (auto_after) auto_margin_seen++;
        }
        item_index++;
    }
    budget_free(context->layout->budget, tracks);
    return true;
}

int distribute_flex_rows(LayoutContext *context,
                                lxb_dom_node_t *container,
                                const ComputedStyle *style,
                                const FlexOrderPlan *order_plan,
                                int declared, int *stretch_free,
                                size_t *stretch_lines)
{
    *stretch_free = 0;
    *stretch_lines = 0;
    if (!style->flex_wrap || declared <= 0) return declared;
    FlexItemIterator scan;
    FlatItem item;
    flex_iterator_init(&scan, context, container, style, order_plan);
    size_t line_count = 0;
    int line_origin = 0;
    int natural_top = 0;
    int natural_bottom = 0;
    while (flex_iterator_next(&scan, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        int origin = 0, height = 0;
        if (!flex_item_cross_metrics(context->layout, &item,
                                     &origin, &height)) continue;
        if (line_count == 0 || origin != line_origin) {
            line_origin = origin;
            if (line_count == 0) natural_top = origin;
            line_count++;
        }
        if (origin + height > natural_bottom) natural_bottom = origin + height;
    }
    int free_cross = declared - (natural_bottom - natural_top);
    if (line_count == 0
        || style->align_content == JUSTIFY_START) return declared;
    if (free_cross > 0
        && style->align_content == JUSTIFY_STRETCH) {
        *stretch_free = free_cross;
        *stretch_lines = line_count;
    }
    FlexItemIterator place;
    flex_iterator_init(&place, context, container, style, order_plan);
    size_t line_index = 0;
    bool have_line = false;
    line_origin = 0;
    while (flex_iterator_next(&place, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position) continue;
        int origin = 0, height = 0;
        if (!flex_item_cross_metrics(context->layout, &item,
                                     &origin, &height)) continue;
        if (!have_line) {
            line_origin = origin;
            have_line = true;
        } else if (origin != line_origin) {
            line_origin = origin;
            line_index++;
        }
        int offset = 0;
        if (style->align_content == JUSTIFY_CENTER) {
            offset = free_cross / 2;
        } else if (style->align_content == JUSTIFY_END) {
            offset = free_cross;
        } else if (style->align_content == JUSTIFY_SPACE_BETWEEN) {
            if (free_cross > 0 && line_count > 1) {
                offset = tilefinch_mul_div_int(
                    free_cross, line_index, line_count - 1);
            }
        } else if (style->align_content == JUSTIFY_SPACE_AROUND) {
            /* The overflow fallback for distributed space-around is safe
               center; it must translate the intact line group rather than
               distributing a negative gap through it. */
            offset = free_cross < 0
                ? free_cross / 2
                : tilefinch_mul_div_int(
                    free_cross, 2 * line_index + 1, 2 * line_count);
        } else if (style->align_content == JUSTIFY_SPACE_EVENLY) {
            offset = free_cross < 0
                ? free_cross / 2
                : tilefinch_mul_div_int(
                    free_cross, line_index + 1, line_count + 1);
        } else if (style->align_content == JUSTIFY_STRETCH
                   && free_cross > 0) {
            offset = tilefinch_mul_div_int(
                free_cross, line_index, line_count);
        }
        if (offset != 0) {
            translate_node_subtree(context->layout, item.node, 0, offset);
        }
    }
    return declared;
}

/*
 * Resolve row cross-axis alignment after the line's tallest item is known.
 * Two streaming scans per line avoid a retained child table while keeping
 * links, controls, and descendant boxes attached to translated items.
 */
void align_flex_row_items(LayoutContext *context,
                                 lxb_dom_node_t *container,
                                 const ComputedStyle *style,
                                 const FlexOrderPlan *order_plan,
                                 int declared, int stretch_free,
                                 size_t stretch_lines)
{
    FlexItemIterator next_line;
    flex_iterator_init(&next_line, context, container, style, order_plan);
    size_t line_index = 0;
    for (;;) {
        FlexItemIterator line_start = next_line;
        FlexItemIterator scan = line_start;
        FlatItem item;
        size_t line_items = 0;
        int line_origin = 0;
        int line_height = 0;
        int baseline_ascent = 0;
        int baseline_descent = 0;
        bool boundary = false;
        while (true) {
            FlexItemIterator before = scan;
            if (!flex_iterator_next(&scan, &item)) break;
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            int origin = 0, height = 0;
            if (!flex_item_cross_metrics(context->layout, &item,
                                         &origin, &height)) continue;
            if (line_items == 0) line_origin = origin;
            else if (style->flex_wrap && origin != line_origin) {
                next_line = before;
                boundary = true;
                break;
            }
            if (height > line_height) line_height = height;
            AlignItems alignment = flex_item_alignment(style, &item.style);
            if (alignment == ALIGN_BASELINE
                && !item.style.margin_top_auto
                && !item.style.margin_bottom_auto) {
                int baseline = 0;
                if (flex_item_baseline(context, &item, origin, height,
                                       &baseline)) {
                    if (baseline > baseline_ascent) {
                        baseline_ascent = baseline;
                    }
                    if (height - baseline > baseline_descent) {
                        baseline_descent = height - baseline;
                    }
                }
            }
            line_items++;
        }
        if (line_items == 0) break;
        if (!style->flex_wrap) {
            if (declared > line_height) line_height = declared;
        }
        if (stretch_free > 0 && line_index < stretch_lines) {
            int before = tilefinch_mul_div_int(
                stretch_free, line_index, stretch_lines);
            int after = tilefinch_mul_div_int(
                stretch_free, line_index + 1, stretch_lines);
            line_height += after - before;
        }
        int baseline_height = baseline_ascent + baseline_descent;
        if (baseline_height > line_height) {
            int delta = baseline_height - line_height;
            line_height = baseline_height;
            if (boundary) {
                FlexItemIterator shift = next_line;
                FlatItem later;
                while (flex_iterator_next(&shift, &later)) {
                    if (later.style.out_of_flow
                        || later.style.fixed_position) continue;
                    translate_node_subtree(context->layout, later.node,
                                           0, delta);
                }
            }
        }
        FlexItemIterator place = line_start;
        size_t placed = 0;
        while (placed < line_items && flex_iterator_next(&place, &item)) {
            if (item.style.out_of_flow || item.style.fixed_position) continue;
            int origin = 0, height = 0;
            if (!flex_item_cross_metrics(context->layout, &item,
                                         &origin, &height)) continue;
            int free_cross = line_height - height;
            bool table_cell = style->display == DISPLAY_TABLE_ROW
                              && !item.anonymous_text
                              && item.style.display == DISPLAY_TABLE_CELL;
            if (table_cell) {
                int target_height = line_height - item.style.margin.top
                                    - item.style.margin.bottom;
                int content_offset = 0;
                if (free_cross > 0) {
                    if (item.style.vertical_align == VERTICAL_MIDDLE) {
                        content_offset = (free_cross + 1) / 2;
                    } else if (item.style.vertical_align
                               == VERTICAL_BOTTOM) {
                        content_offset = free_cross;
                    }
                }
                if (target_height > 0) {
                    stretch_flex_item_cross(context->layout, item.node,
                                            &item.style, target_height);
                }
                if (content_offset > 0) {
                    translate_table_cell_contents(context->layout,
                                                  item.node,
                                                  content_offset);
                }
                placed++;
                continue;
            }
            AlignItems alignment = flex_item_alignment(style, &item.style);
            if (item.style.margin_top_auto
                || item.style.margin_bottom_auto) {
                int offset = item.style.margin_top_auto
                    ? (item.style.margin_bottom_auto
                       ? free_cross / 2 : free_cross)
                    : 0;
                if (offset > 0) {
                    translate_node_subtree(context->layout, item.node,
                                           0, offset);
                }
                placed++;
                continue;
            }
            if (alignment == ALIGN_STRETCH && !item.style.has_height) {
                int target_height = line_height - item.style.margin.top
                                    - item.style.margin.bottom;
                if (target_height > 0) {
                    stretch_flex_item_cross(context->layout, item.node,
                                            &item.style, target_height);
                }
                placed++;
                continue;
            }
            if (alignment == ALIGN_BASELINE) {
                int baseline = 0;
                if (flex_item_baseline(context, &item, origin, height,
                                       &baseline)) {
                    int offset = baseline_ascent - baseline;
                    if (offset > 0) {
                        translate_node_subtree(context->layout, item.node,
                                               0, offset);
                    }
                }
                placed++;
                continue;
            }
            int offset = alignment == ALIGN_CENTER
                         ? free_cross / 2 : free_cross;
            if ((alignment == ALIGN_CENTER || alignment == ALIGN_END)
                && offset > 0) {
                translate_node_subtree(context->layout, item.node, 0, offset);
            }
            placed++;
        }
        line_index++;
        if (!boundary) break;
    }
}
