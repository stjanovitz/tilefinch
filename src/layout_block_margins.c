/* Block margin collapsing: the mutually recursive top/bottom margin walk,
   its per-node cache, and the list-item probes that share the flat-item
   traversal. Split out of layout_block.c. */

#include "layout_block_internal.h"

#include <ctype.h>
#include <string.h>

bool layout_block_list_item_starts_with_block(LayoutContext *context,
                                              lxb_dom_node_t *node,
                                              const ComputedStyle *style)
{
    FlatItemIterator iterator;
    FlatItem item;
    flat_iterator_init(&iterator, context, node, style);
    iterator.include_whitespace = false;
    while (flat_iterator_next(&iterator, &item)) {
        if (item.style.out_of_flow || item.style.fixed_position
            || item.style.display == DISPLAY_NONE) continue;
        return !item.anonymous_text && is_block_display(item.style.display);
    }
    return false;
}

bool layout_block_list_item_parent_has_columns(LayoutContext *context,
                                               lxb_dom_node_t *node)
{
    lxb_dom_node_t *parent = node == NULL ? NULL : node->parent;
    if (context == NULL || context->sheet == NULL || parent == NULL
        || parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    static const char *const properties[] = {
        "columns", "column-count", "column-width"
    };
    char value[96];
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
        if (style_retained_presentation_value(
                context->sheet, parent, properties[i],
                strlen(properties[i]), value, sizeof(value))) return true;
    }
    return false;
}

#define MARGIN_CACHE_TOP UINT8_C(1)
#define MARGIN_CACHE_BOTTOM UINT8_C(2)

static uint32_t margin_style_signature(const ComputedStyle *style,
                                       size_t depth)
{
    if (style == NULL) return 0;
    uint32_t hash = UINT32_C(2166136261);
#define MIX_MARGIN_FIELD(field) do {                                          \
        hash ^= (uint32_t) (style->field);                                    \
        hash *= UINT32_C(16777619);                                           \
    } while (0)
    MIX_MARGIN_FIELD(display);
    MIX_MARGIN_FIELD(margin.top);
    MIX_MARGIN_FIELD(margin.bottom);
    MIX_MARGIN_FIELD(padding.top);
    MIX_MARGIN_FIELD(padding.bottom);
    MIX_MARGIN_FIELD(border.top);
    MIX_MARGIN_FIELD(border.bottom);
    MIX_MARGIN_FIELD(min_height);
    MIX_MARGIN_FIELD(has_height);
    MIX_MARGIN_FIELD(min_height_percent);
    MIX_MARGIN_FIELD(overflow_x_scroll);
    MIX_MARGIN_FIELD(overflow_y_scroll);
    MIX_MARGIN_FIELD(overflow_x_clip_only);
    MIX_MARGIN_FIELD(overflow_y_clip_only);
    MIX_MARGIN_FIELD(out_of_flow);
    MIX_MARGIN_FIELD(fixed_position);
    MIX_MARGIN_FIELD(float_mode);
    MIX_MARGIN_FIELD(clear_mode);
#undef MIX_MARGIN_FIELD
    hash ^= (uint32_t) depth;
    hash *= UINT32_C(16777619);
    return hash;
}

static bool margin_cache_get(LayoutContext *context, lxb_dom_node_t *node,
                             const ComputedStyle *style, size_t depth,
                             uint8_t kind, int *value)
{
    if (context == NULL || node == NULL || style == NULL || value == NULL) {
        return false;
    }
    uint32_t signature = margin_style_signature(style, depth);
    size_t home = layout_pointer_hash(node)
                  & (LAYOUT_MARGIN_CACHE_CAPACITY - 1u);
    for (size_t probe = 0; probe < LAYOUT_MARGIN_CACHE_PROBE_LIMIT; probe++) {
        LayoutMarginCacheEntry *entry = &context->margin_cache[
            (home + probe) & (LAYOUT_MARGIN_CACHE_CAPACITY - 1u)];
        if (entry->node == NULL) break;
        if (entry->node == node && entry->style_signature == signature) {
            if ((entry->valid_mask & kind) == 0) break;
            entry->stamp = ++context->margin_cache_clock;
            *value = kind == MARGIN_CACHE_TOP ? entry->top : entry->bottom;
            context->margin_cache_hits++;
            return true;
        }
    }
    context->margin_cache_misses++;
    return false;
}

static void margin_cache_put(LayoutContext *context, lxb_dom_node_t *node,
                             const ComputedStyle *style, size_t depth,
                             uint8_t kind, int value)
{
    if (context == NULL || node == NULL || style == NULL) return;
    uint32_t signature = margin_style_signature(style, depth);
    size_t home = layout_pointer_hash(node)
                  & (LAYOUT_MARGIN_CACHE_CAPACITY - 1u);
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < LAYOUT_MARGIN_CACHE_PROBE_LIMIT; probe++) {
        size_t slot = (home + probe)
                      & (LAYOUT_MARGIN_CACHE_CAPACITY - 1u);
        LayoutMarginCacheEntry *entry = &context->margin_cache[slot];
        if (entry->node == NULL
            || (entry->node == node
                && entry->style_signature == signature)) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
    LayoutMarginCacheEntry *entry = &context->margin_cache[replacement];
    if (entry->node != node || entry->style_signature != signature) {
        *entry = (LayoutMarginCacheEntry) {
            .node = node,
            .style_signature = signature
        };
    }
    if (kind == MARGIN_CACHE_TOP) entry->top = value;
    else entry->bottom = value;
    entry->valid_mask |= kind;
    entry->stamp = ++context->margin_cache_clock;
}

/* 1 found a block margin, 0 found only empty inline wrappers, -1 encountered
   inline content which terminates the anonymous block's collapsible edge. */
static int collapsed_inline_first_block_top(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, size_t depth, int *margin);

static bool block_collapses_through(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, size_t depth)
{
    if (context != NULL) context->margin_collapse_visits++;
    if (context == NULL || node == NULL || style == NULL
        || depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT
        || !block_parent_collapses_top(style)
        || !block_parent_collapses_bottom(style, style->has_height)) {
        return false;
    }
    LayoutBlockScratch *scratch = layout_block_scratch_for_depth(
        context, depth);
    /* Without scratch the conservative answer matches the depth-limit
       branch above: assume the block does not collapse through. */
    if (scratch == NULL) return false;
    ComputedStyle *resolved = &scratch->traversal.collapse.through_resolved;
    ComputedStyle *child_style = &scratch->traversal.collapse.through_child;
    *resolved = *style;
    resolve_padding(context->sheet, resolved, 0);
    int minimum_height = 0;
    if (!resolve_computed_length(
            context->sheet, resolved->min_height,
            resolved->min_height_percent, 0, &minimum_height)) {
        minimum_height = 0;
    }
    if (resolved->border.top != 0 || resolved->border.bottom != 0
        || resolved->padding.top != 0 || resolved->padding.bottom != 0
        || minimum_height != 0) {
        return false;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            for (size_t i = 0; text != NULL && i < length; i++) {
                if (!isspace((unsigned char) text[i])) return false;
            }
            continue;
        }
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        *child_style = layout_style_for_node(context, child, resolved);
        if (child_style->display == DISPLAY_NONE || child_style->hidden
            || child_style->out_of_flow || child_style->fixed_position
            || child_style->float_mode != FLOAT_NONE) {
            continue;
        }
        if (!block_item_has_collapsible_margins(child_style)
            || !block_collapses_through(
                context, child, child_style, depth + 1)) {
            return false;
        }
    }
    return true;
}

int layout_block_collapsed_block_top_margin(LayoutContext *context,
                                            lxb_dom_node_t *node,
                                            const ComputedStyle *style,
                                            size_t depth)
{
    if (context != NULL) context->margin_collapse_visits++;
    if (style == NULL) return 0;
    int cached = 0;
    if (margin_cache_get(
            context, node, style, depth, MARGIN_CACHE_TOP, &cached)) {
        return cached;
    }
    CollapsedMargin collapsed;
    collapsed_margin_reset(&collapsed, style->margin.top);
    if (depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT
        || !block_parent_collapses_top(style)) {
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_TOP, value);
        return value;
    }
    LayoutBlockScratch *scratch = layout_block_scratch_for_depth(
        context, depth);
    if (scratch == NULL) {
        /* Same answer the depth-limit branch gives: report the declared
           margin rather than a collapsed one. */
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_TOP, value);
        return value;
    }
    ComputedStyle *before = &scratch->traversal.collapse.pseudo;
    *before = style_for_pseudo(
        context->sheet, node, PSEUDO_BEFORE, style);
    if (generated_pseudo_is_flow_block(before)) {
        (void) collapsed_margin_add(&collapsed, before->margin.top);
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_TOP, value);
        return value;
    }
    FlatItemIterator *iterator = &scratch->traversal.collapse.iterator;
    FlatItem *item = &scratch->traversal.collapse.item;
    flat_iterator_init(iterator, context, node, style);
    iterator->include_whitespace = false;
    while (flat_iterator_next(iterator, item)) {
        if (item->style.out_of_flow || item->style.fixed_position
            || item->style.float_mode != FLOAT_NONE) {
            continue;
        }
        if (item->anonymous_text
            || (!block_item_has_collapsible_margins(&item->style)
                && item->style.display != DISPLAY_INLINE
                && item->style.display != DISPLAY_CONTENTS)) {
            break;
        }
        if (block_item_has_collapsible_margins(&item->style)) {
            int descendant = layout_block_collapsed_block_top_margin(
                context, item->node, &item->style, depth + 1);
            (void) collapsed_margin_add(&collapsed, descendant);
            break;
        }
        int descendant = 0;
        int result = collapsed_inline_first_block_top(
            context, item->node, &item->style, depth + 1, &descendant);
        if (result > 0) {
            (void) collapsed_margin_add(&collapsed, descendant);
            break;
        }
        if (result < 0) break;
    }
    if (block_collapses_through(context, node, style, depth)) {
        (void) collapsed_margin_add(&collapsed, style->margin.bottom);
    }
    int value = collapsed_margin_value(&collapsed);
    margin_cache_put(context, node, style, depth, MARGIN_CACHE_TOP, value);
    return value;
}

int layout_collapsed_block_top_margin(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style)
{
    return layout_block_collapsed_block_top_margin(
        context, node, style, collapse_walk_origin(context));
}

static int collapsed_inline_first_block_top(
    LayoutContext *context, lxb_dom_node_t *node,
    const ComputedStyle *style, size_t depth, int *margin)
{
    if (context != NULL) context->margin_collapse_visits++;
    if (context == NULL || node == NULL || style == NULL || margin == NULL
        || depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT) return -1;
    LayoutBlockScratch *scratch = layout_block_scratch_for_depth(
        context, depth);
    if (scratch == NULL) return -1;
    ComputedStyle *child_style = &scratch->traversal.collapse.inline_child;
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            for (size_t i = 0; text != NULL && i < length; i++) {
                if (!isspace((unsigned char) text[i])) return -1;
            }
            continue;
        }
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        *child_style = layout_style_for_node(context, child, style);
        if (child_style->display == DISPLAY_NONE || child_style->hidden
            || child_style->out_of_flow || child_style->fixed_position
            || child_style->float_mode != FLOAT_NONE) {
            continue;
        }
        if (block_item_has_collapsible_margins(child_style)) {
            *margin = layout_block_collapsed_block_top_margin(
                context, child, child_style, depth + 1);
            return 1;
        }
        if (child_style->display == DISPLAY_INLINE
            || child_style->display == DISPLAY_CONTENTS) {
            int result = collapsed_inline_first_block_top(
                context, child, child_style, depth + 1, margin);
            if (result != 0) return result;
            continue;
        }
        return -1;
    }
    return 0;
}

int layout_block_collapsed_block_bottom_margin(LayoutContext *context,
                                               lxb_dom_node_t *node,
                                               const ComputedStyle *style,
                                               size_t depth)
{
    if (context != NULL) context->margin_collapse_visits++;
    if (style == NULL) return 0;
    int cached = 0;
    if (margin_cache_get(
            context, node, style, depth, MARGIN_CACHE_BOTTOM, &cached)) {
        return cached;
    }
    CollapsedMargin collapsed;
    collapsed_margin_reset(&collapsed, style->margin.bottom);
    if (depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT
        || !block_parent_collapses_bottom(style, style->has_height)) {
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_BOTTOM, value);
        return value;
    }
    LayoutBlockScratch *scratch = layout_block_scratch_for_depth(
        context, depth);
    if (scratch == NULL) {
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_BOTTOM, value);
        return value;
    }
    ComputedStyle *after = &scratch->traversal.collapse.pseudo;
    *after = style_for_pseudo(context->sheet, node, PSEUDO_AFTER, style);
    if (generated_pseudo_is_flow_block(after)) {
        (void) collapsed_margin_add(&collapsed, after->margin.bottom);
        int value = collapsed_margin_value(&collapsed);
        margin_cache_put(
            context, node, style, depth, MARGIN_CACHE_BOTTOM, value);
        return value;
    }
    FlatItemIterator *iterator = &scratch->traversal.collapse.iterator;
    FlatItem *item = &scratch->traversal.collapse.item;
    FlatItem *last = &scratch->traversal.collapse.last;
    memset(last, 0, sizeof(*last));
    bool have_last = false;
    flat_iterator_init(iterator, context, node, style);
    iterator->include_whitespace = false;
    while (flat_iterator_next(iterator, item)) {
        if (item->style.out_of_flow || item->style.fixed_position
            || item->style.float_mode != FLOAT_NONE) {
            continue;
        }
        *last = *item;
        have_last = true;
    }
    if (have_last && !last->anonymous_text
        && block_item_has_collapsible_margins(&last->style)) {
        int descendant = layout_block_collapsed_block_bottom_margin(
            context, last->node, &last->style, depth + 1);
        (void) collapsed_margin_add(&collapsed, descendant);
    }
    int value = collapsed_margin_value(&collapsed);
    margin_cache_put(
        context, node, style, depth, MARGIN_CACHE_BOTTOM, value);
    return value;
}
