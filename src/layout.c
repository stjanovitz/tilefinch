#include "tilefinch/layout.h"
#include "tilefinch/platform.h"

#include "layout_internal.h"
#include "style_cache_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint64_t layout_performance_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static void layout_read_trace_environment(LayoutDocument *layout)
{
#ifndef TILEFINCH_NO_TRACE
    layout->trace_flags =
        (getenv("TILEFINCH_TRACE_LAYOUT") != NULL ? LAYOUT_TRACE_LAYOUT : 0u)
        | (getenv("TILEFINCH_TRACE_PAINT") != NULL ? LAYOUT_TRACE_PAINT : 0u)
        | (getenv("TILEFINCH_TRACE_CLIP") != NULL ? LAYOUT_TRACE_CLIP : 0u)
        | (getenv("TILEFINCH_TRACE_FLEX_TRANSLATE") != NULL
           ? LAYOUT_TRACE_FLEX_TRANSLATE : 0u)
        | (getenv("TILEFINCH_TRACE_LAYOUT_SLICES") != NULL
           ? LAYOUT_TRACE_LAYOUT_SLICES : 0u)
        | (getenv("TILEFINCH_TRACE_LAYOUT_PROFILE") != NULL
           ? LAYOUT_TRACE_LAYOUT_PROFILE : 0u)
        | (getenv("TILEFINCH_TRACE_SCROLL_WIDTH") != NULL
           ? LAYOUT_TRACE_SCROLL_WIDTH : 0u);
    layout->trace_flex_class = getenv("TILEFINCH_TRACE_FLEX_CLASS");
    layout->trace_layout_class = getenv("TILEFINCH_TRACE_LAYOUT_CLASS");
    layout->trace_pseudo_class = getenv("TILEFINCH_TRACE_PSEUDO_CLASS");
    layout->trace_range_class = getenv("TILEFINCH_TRACE_RANGE_CLASS");
#else
    (void) layout;
#endif
}

static void layout_free_block_scratch(LayoutContext *context)
{
    if (context == NULL || context->layout == NULL) return;
    for (size_t i = 0; i < LAYOUT_BLOCK_SCRATCH_PAGE_COUNT; i++) {
        budget_free(context->layout->budget,
                    context->block_scratch_pages[i]);
        context->block_scratch_pages[i] = NULL;
    }
    context->block_scratch_page_count = 0;
}

static void layout_release_context(LayoutContext *context, Budget *budget)
{
    if (context == NULL) return;
    if (context->style_selector_cooperation_owned
        && context->sheet != NULL) {
        style_selector_cooperation_end((Stylesheet *) context->sheet);
    }
    if (context->style_variable_cache_owned && context->sheet != NULL) {
        style_variable_cache_end((Stylesheet *) context->sheet);
    }
    layout_free_block_scratch(context);
    for (size_t i = 0; i < context->table_track_count; i++) {
        budget_free(budget, context->table_tracks[i].placements);
    }
    budget_free(budget, context->stacking_contexts);
    budget_free(budget, context->visibility_ranges);
    budget_free(budget, context);
}

bool layout_tree_enter(LayoutContext *context,
                              lxb_dom_node_t *node,
                              const char *phase)
{
    if (context == NULL || context->cancelled) return false;
    if (context->tree_call_depth >= LAYOUT_TREE_CALL_DEPTH_LIMIT) {
        if (!context->depth_limit_reported) {
            size_t name_length = 0;
            const char *name = document_element_name(node, &name_length);
            if (name == NULL && node != NULL
                && node->type == LXB_DOM_NODE_TYPE_TEXT) {
                name = "#text";
                name_length = 5;
            }
            fprintf(stderr,
                    "layout-depth-limit phase=%s limit=%u node=%.*s\n",
                    phase == NULL ? "unknown" : phase,
                    (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
                    (int) name_length, name == NULL ? "" : name);
            context->depth_limit_reported = true;
        }
        context->depth_fallback_count++;
        return false;
    }
    context->tree_call_depth++;
    if (context->tree_call_depth > context->max_tree_call_depth) {
        context->max_tree_call_depth = context->tree_call_depth;
    }
    return true;
}

void layout_tree_leave(LayoutContext *context)
{
    if (context != NULL && context->tree_call_depth != 0) {
        context->tree_call_depth--;
    }
}

void layout_tree_note_fallback(LayoutContext *context,
                                      lxb_dom_node_t *node,
                                      const char *phase)
{
    if (context == NULL) return;
    context->depth_fallback_count++;
    if (context->depth_limit_reported) return;
    size_t name_length = 0;
    const char *name = document_element_name(node, &name_length);
    if (name == NULL && node != NULL
        && node->type == LXB_DOM_NODE_TYPE_TEXT) {
        name = "#text";
        name_length = 5;
    }
    fprintf(stderr, "layout-depth-limit phase=%s limit=%u node=%.*s\n",
            phase == NULL ? "unknown" : phase,
            (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
            (int) name_length, name == NULL ? "" : name);
    context->depth_limit_reported = true;
}

static unsigned style_font_weight(const ComputedStyle *style)
{
    if (style == NULL) return 400;
    if (style->font_weight != 0) return style->font_weight;
    return style->font_bold ? 700 : 400;
}

bool style_uses_bold_face(const ComputedStyle *style)
{
    return style_font_weight(style) >= LAYOUT_BOLD_FACE_WEIGHT;
}

bool style_uses_synthetic_weight(const FontSet *fonts,
                                        const WebFontSet *web_fonts,
                                        const ComputedStyle *style,
                                        const FontFace *face)
{
    unsigned weight = style_font_weight(style);
    return weight >= LAYOUT_SYNTHETIC_WEIGHT
           && (weight < LAYOUT_BOLD_FACE_WEIGHT
               || !font_context_face_is_bold(fonts, web_fonts, face));
}

/* DrawCommand is deliberately compact on PSP. Store CSS weights in ten-unit
   increments (600 becomes 60), which preserves the face-selection boundary
   without growing the display-list record. */
uint8_t draw_font_weight(const ComputedStyle *style)
{
    unsigned encoded = (style_font_weight(style) + 5u) / 10u;
    return (uint8_t) (encoded > 100u ? 100u : encoded);
}

uint8_t text_decoration_bits(const ComputedStyle *style)
{
    if (!computed_style_has_text_underline(style)
        && !computed_style_has_ancestor_text_underline(style)) return 0;
    uint8_t bits = LAYOUT_TEXT_DECORATION_UNDERLINE;
    int pixels = 0;
    if (computed_style_effective_text_underline_offset(style, &pixels)) {
        bits |= (uint8_t) ((pixels + 16)
                          << LAYOUT_TEXT_UNDERLINE_OFFSET_SHIFT);
    }
    return bits;
}

bool draw_uses_bold_face(const DrawCommand *command)
{
    return command != NULL
           && draw_command_font_weight_code(command)
              >= LAYOUT_BOLD_FACE_WEIGHT / 10;
}

void layout_finish_work_slice(LayoutContext *context)
{
    if (context == NULL || context->slice_units == 0) return;
    uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed_us = finished_ns >= context->slice_started_ns
        ? (finished_ns - context->slice_started_ns) / UINT64_C(1000) : 0;
    if (elapsed_us > context->layout->max_work_slice_us) {
        context->layout->max_work_slice_us = elapsed_us;
        context->layout->max_work_slice_units = context->slice_units;
        context->layout->max_work_slice_node = context->slice_last_node;
    }
    if (elapsed_us > 16000
        && LAYOUT_TRACE(context->layout, LAYOUT_SLICES)) {
        size_t name_length = 0, id_length = 0, class_length = 0;
        const char *name = document_element_name(
            context->slice_last_node, &name_length);
        const char *id = document_attribute(
            context->slice_last_node, "id", &id_length);
        const char *class_name = document_attribute(
            context->slice_last_node, "class", &class_length);
        fprintf(stderr,
                "layout-slice elapsed-us=%llu work=%zu total-work=%zu "
                "last=%.*s id=%.*s class=%.*s\n",
                (unsigned long long) elapsed_us, context->slice_units,
                context->layout->layout_work_units,
                (int) name_length, name == NULL ? "" : name,
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name);
    }
    context->slice_units = 0;
    context->slice_started_ns = finished_ns;
}

bool layout_cooperate(LayoutContext *context, lxb_dom_node_t *node)
{
    context->layout->layout_work_units++;
    context->slice_units++;
    context->slice_last_node = node;
    if (context->slice_units < LAYOUT_WORK_QUOTA) return true;
    size_t completed = context->layout->layout_work_units;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    bool keep_going = tilefinch_platform_cooperate("layout", completed);
    if (!keep_going) context->cancelled = true;
    return keep_going;
}

static bool layout_selector_cooperate(
    void *opaque, lxb_dom_node_t *node, size_t completed_visits)
{
    LayoutContext *context = opaque;
    if (context == NULL || context->cancelled) return false;
    /* A root style can perform substantial selector work before the first
       ordinary layout unit. Give that interval a visible slice as well. */
    if (context->slice_units == 0) context->slice_units = 1;
    context->slice_last_node = node;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    bool keep_going = tilefinch_platform_cooperate(
        "layout-style", completed_visits);
    if (!keep_going) context->cancelled = true;
    return keep_going;
}

static uint64_t layout_style_parent_hash(const ComputedStyle *style)
{
    const unsigned char *bytes = (const unsigned char *) style;
    uint32_t low = UINT32_C(2166136261);
    uint32_t high = UINT32_C(0x85ebca6b);
    size_t length = style == NULL ? 0 : sizeof(*style);
    while (length >= sizeof(uint32_t)) {
        uint32_t word = 0;
        memcpy(&word, bytes, sizeof(word));
        low = (low ^ word) * UINT32_C(16777619);
        high = (high ^ (word + UINT32_C(0x9e3779b9)))
               * UINT32_C(2246822519);
        bytes += sizeof(word);
        length -= sizeof(word);
    }
    uint32_t tail = 0;
    unsigned shift = 0;
    while (length-- != 0) {
        tail |= (uint32_t) *bytes++ << shift;
        shift += 8;
    }
    if (shift != 0) {
        low = (low ^ tail) * UINT32_C(16777619);
        high = (high ^ (tail + UINT32_C(0x9e3779b9)))
               * UINT32_C(2246822519);
    }
    return (uint64_t) high << 32 | low;
}

static void layout_reuse_clear_entries(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    memset(cache->styles, 0, sizeof(cache->styles));
    memset(cache->intrinsic, 0, sizeof(cache->intrinsic));
    if (cache->table_rows != NULL) {
        memset(cache->table_rows, 0,
               sizeof(*cache->table_rows)
                   * LAYOUT_REUSE_TABLE_ROW_CAPACITY);
    }
    cache->clock = 0;
}

static void layout_reuse_clear_sizing_entries(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    memset(cache->intrinsic, 0, sizeof(cache->intrinsic));
    if (cache->table_rows != NULL) {
        memset(cache->table_rows, 0,
               sizeof(*cache->table_rows)
                   * LAYOUT_REUSE_TABLE_ROW_CAPACITY);
    }
}

LayoutReuseCache *layout_reuse_cache_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    LayoutReuseCache *cache = budget_calloc(budget, 1, sizeof(*cache));
    if (cache == NULL) return NULL;
    cache->budget = budget;
    cache->stats.retained_bytes = sizeof(*cache);
    return cache;
}

void layout_reuse_cache_destroy(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    Budget *budget = cache->budget;
    if (cache->table_rows != NULL) {
        budget_free(budget, cache->table_rows);
    }
    memset(cache, 0, sizeof(*cache));
    budget_free(budget, cache);
}

void layout_reuse_cache_reset(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    layout_reuse_clear_entries(cache);
    cache->sheet = NULL;
    cache->sheet_generation = 0;
    cache->fonts = NULL;
    cache->images = NULL;
    cache->viewport_width = 0;
    cache->selector_has_has = false;
    cache->selector_has_focus_within = false;
    cache->selector_focus_has_sibling = false;
    cache->selector_has_structure = false;
    cache->stats.full_resets++;
}

static bool layout_node_is_within(lxb_dom_node_t *node,
                                  lxb_dom_node_t *ancestor)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

static bool layout_reuse_measurement_touches_node(
    lxb_dom_node_t *measurement, lxb_dom_node_t *changed)
{
    return measurement != NULL && changed != NULL
           && (layout_node_is_within(measurement, changed)
               || layout_node_is_within(changed, measurement));
}

static bool layout_reuse_image_has_stable_geometry(
    const ImageResource *image)
{
    if (image == NULL || image->node == NULL) return false;
    /* CSS backgrounds paint inside an already-sized box. They never change
       intrinsic/table geometry. */
    if (image->is_background && !image->is_mask) return true;
    size_t width_length = 0, height_length = 0;
    const char *width = document_attribute(
        image->node, "width", &width_length);
    const char *height = document_attribute(
        image->node, "height", &height_length);
    if (width == NULL || height == NULL || width_length == 0
        || height_length == 0 || width_length >= 32
        || height_length >= 32) return false;
    char width_value[32], height_value[32];
    memcpy(width_value, width, width_length);
    memcpy(height_value, height, height_length);
    width_value[width_length] = '\0';
    height_value[height_length] = '\0';
    return atoi(width_value) > 0 && atoi(height_value) > 0;
}

void layout_reuse_cache_update_images(LayoutReuseCache *cache,
                                      const ImageResources *images)
{
    if (cache == NULL || cache->images == images) return;
    if (images == NULL || images->items == NULL || images->count == 0) {
        layout_reuse_clear_sizing_entries(cache);
        cache->images = images;
        return;
    }
    bool invalidated = false;
    for (size_t i = 0; i < LAYOUT_REUSE_INTRINSIC_CAPACITY; i++) {
        LayoutIntrinsicCacheEntry *entry = &cache->intrinsic[i];
        for (size_t j = 0; entry->node != NULL && j < images->count; j++) {
            if (layout_reuse_image_has_stable_geometry(
                    &images->items[j])) continue;
            if (layout_reuse_measurement_touches_node(
                    entry->node, images->items[j].node)) {
                memset(entry, 0, sizeof(*entry));
                invalidated = true;
            }
        }
    }
    for (size_t i = 0; cache->table_rows != NULL
                       && i < LAYOUT_REUSE_TABLE_ROW_CAPACITY; i++) {
        LayoutReuseTableRowEntry *entry = &cache->table_rows[i];
        for (size_t j = 0; entry->row != NULL && j < images->count; j++) {
            if (layout_reuse_image_has_stable_geometry(
                    &images->items[j])) continue;
            if (layout_reuse_measurement_touches_node(
                    entry->row, images->items[j].node)) {
                memset(entry, 0, sizeof(*entry));
                invalidated = true;
            }
        }
    }
    cache->images = images;
    if (invalidated) cache->stats.scoped_invalidations++;
}

void layout_reuse_cache_rebind_stylesheet(
    LayoutReuseCache *cache, const Stylesheet *previous,
    const Stylesheet *replacement)
{
    if (cache == NULL || previous == NULL || replacement == NULL
        || cache->sheet != previous) return;
    /* This operation only translates the address of a memberwise-moved
       stylesheet during page adoption.  The generation records the rules
       which produced the cached ComputedStyles; copying the replacement's
       current generation here can silently bless stale entries when the
       source sheet was rebuilt between preview and adoption. */
    cache->sheet = replacement;
}

void layout_reuse_cache_invalidate_node_scoped(
    LayoutReuseCache *cache, lxb_dom_node_t *node,
    bool text_or_structure_sensitive)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    lxb_dom_node_t *style_scope = node;
    if ((text_or_structure_sensitive || cache->selector_has_structure)
        && node->parent != NULL) style_scope = node->parent;
    for (size_t i = 0; i < LAYOUT_REUSE_STYLE_CAPACITY; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node != NULL
            && layout_node_is_within(entry->node, style_scope)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (size_t i = 0; i < LAYOUT_REUSE_INTRINSIC_CAPACITY; i++) {
        LayoutIntrinsicCacheEntry *entry = &cache->intrinsic[i];
        if (entry->node != NULL
            && (layout_node_is_within(entry->node, node)
                || layout_node_is_within(node, entry->node))) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (size_t i = 0; cache->table_rows != NULL
                       && i < LAYOUT_REUSE_TABLE_ROW_CAPACITY; i++) {
        LayoutReuseTableRowEntry *entry = &cache->table_rows[i];
        if (entry->row != NULL
            && (layout_node_is_within(entry->row, node)
                || layout_node_is_within(node, entry->row))) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    cache->stats.scoped_invalidations++;
}

void layout_reuse_cache_invalidate_node(LayoutReuseCache *cache,
                                        lxb_dom_node_t *node,
                                        bool text_or_structure_sensitive)
{
    if (cache != NULL && cache->selector_has_has) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_cache_invalidate_node_scoped(
        cache, node, text_or_structure_sensitive);
}

void layout_reuse_cache_invalidate_focus(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    if (cache->selector_focus_has_sibling) {
        layout_reuse_cache_reset(cache);
        return;
    }
    /* Focus, :focus-within, and :has(:focus) without sibling combinators can
       change the focused subtree and its ancestor chain. Descendant cache
       keys include the complete parent style, so a changed inherited or
       geometric ancestor value naturally misses below the first recomputed
       ancestor. Intrinsic measurements touching the node are cleared by the
       ordinary scoped path. */
    layout_reuse_cache_invalidate_node_scoped(cache, node, false);
    for (size_t i = 0; i < LAYOUT_REUSE_STYLE_CAPACITY; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node != NULL
            && layout_node_is_within(node, entry->node)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

void layout_reuse_cache_stats(const LayoutReuseCache *cache,
                              LayoutReuseStats *stats)
{
    if (stats == NULL) return;
    if (cache == NULL) memset(stats, 0, sizeof(*stats));
    else *stats = cache->stats;
}

bool layout_reuse_cache_can_reuse_mutations(const LayoutReuseCache *cache)
{
    /* The mutation journal classifies exact before/after attribute changes
       against relational-selector dependencies. Unknown mutations still
       take the conservative reset path. */
    return cache != NULL;
}

static void layout_reuse_note_selector_dependencies(
    LayoutReuseCache *cache, const char *selector)
{
    if (cache == NULL || selector == NULL) return;
    if (strstr(selector, ":has(") != NULL) cache->selector_has_has = true;
    if (strstr(selector, ":focus-within") != NULL) {
        cache->selector_has_focus_within = true;
    }
    if ((strstr(selector, ":focus") != NULL)
        && (strchr(selector, '+') != NULL
            || strchr(selector, '~') != NULL)) {
        cache->selector_focus_has_sibling = true;
    }
    if (strstr(selector, ":nth-") != NULL
        || strstr(selector, ":first-") != NULL
        || strstr(selector, ":last-") != NULL
        || strstr(selector, ":only-") != NULL
        || strstr(selector, ":empty") != NULL
        || strchr(selector, '+') != NULL
        || strchr(selector, '~') != NULL) {
        cache->selector_has_structure = true;
    }
}

void layout_reuse_cache_prepare(LayoutReuseCache *cache,
                                const Stylesheet *sheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                int viewport_width)
{
    if (cache == NULL) return;
    uint64_t sheet_generation =
        sheet == NULL ? 0 : sheet->build_generation;
    if (cache->sheet == sheet
        && cache->sheet_generation == sheet_generation
        && cache->fonts == fonts
        && cache->viewport_width == viewport_width) {
        if (cache->images == images) return;
        layout_reuse_clear_sizing_entries(cache);
        cache->images = images;
        return;
    }
    layout_reuse_clear_entries(cache);
    cache->sheet = sheet;
    cache->sheet_generation = sheet_generation;
    cache->fonts = fonts;
    cache->images = images;
    cache->viewport_width = viewport_width;
    cache->selector_has_has = false;
    cache->selector_has_focus_within = false;
    cache->selector_focus_has_sibling = false;
    cache->selector_has_structure = false;
    if (sheet == NULL) return;
    for (size_t i = 0; i < sheet->count; i++) {
        layout_reuse_note_selector_dependencies(
            cache, sheet->rules[i].selector);
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        layout_reuse_note_selector_dependencies(
            cache, sheet->custom_rules[i].selector);
    }
}

static bool layout_reuse_style_get_hashed(LayoutReuseCache *cache,
                                          lxb_dom_node_t *node,
                                          uint64_t parent_hash,
                                          ComputedStyle *style)
{
    if (cache == NULL || node == NULL || style == NULL) return false;
    size_t home = layout_pointer_hash(node)
                  & (LAYOUT_REUSE_STYLE_CAPACITY - 1u);
    for (size_t probe = 0; probe < 8; probe++) {
        LayoutReuseStyleEntry *entry = &cache->styles[
            (home + probe) & (LAYOUT_REUSE_STYLE_CAPACITY - 1u)];
        if (entry->node == node && entry->parent_hash == parent_hash) {
            entry->stamp = ++cache->clock;
            cache->stats.style_hits++;
            *style = entry->style;
            return true;
        }
    }
    cache->stats.style_misses++;
    return false;
}

static void layout_reuse_style_put_hashed(LayoutReuseCache *cache,
                                          lxb_dom_node_t *node,
                                          uint64_t parent_hash,
                                          const ComputedStyle *style)
{
    if (cache == NULL || node == NULL || style == NULL) return;
    size_t home = layout_pointer_hash(node)
                  & (LAYOUT_REUSE_STYLE_CAPACITY - 1u);
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < 8; probe++) {
        size_t slot = (home + probe) & (LAYOUT_REUSE_STYLE_CAPACITY - 1u);
        LayoutReuseStyleEntry *entry = &cache->styles[slot];
        if (entry->node == NULL || entry->node == node) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
    cache->styles[replacement] = (LayoutReuseStyleEntry) {
        .node = node,
        .parent_hash = parent_hash,
        .style = *style,
        .stamp = ++cache->clock
    };
}

static void layout_resolve_canonical_style(
    const Stylesheet *sheet, const FontSet *fonts,
    const WebFontSet *web_fonts, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *result)
{
    *result = style_for_node(sheet, node, parent);
    if ((result->filter_code & STYLE_FONT_CH_PENDING) != 0
        && sheet != NULL && sheet->resolve_scratch != NULL) {
        const FontFace *face = font_context_face_variant(
            fonts, web_fonts, result->font_family,
            result->font_italic, style_uses_bold_face(result));
        FontFamily metric_family = font_context_metric_family(
            web_fonts, result->font_family, face);
        int width_fixed = font_text_width_for_family_at_size_fixed(
            face, metric_family, "0", 1,
            computed_style_font_size_fixed(result),
            style_uses_synthetic_weight(
                fonts, web_fonts, result, face),
            style_uses_bold_face(result));
        /* Use the same selected compatibility metric family as ordinary
           text measurement, so `ch` and wrapping cannot disagree. */
        int ch_basis = width_fixed > 0
            ? layout_fixed_ceil(width_fixed)
            : (result->font_size + 1) / 2;
        *result = style_for_node_with_ch_basis(
            sheet, node, parent, ch_basis);
    }
    size_t focus_length = 0;
    if (document_attribute(
            node, "data-tilefinch-focus", &focus_length) != NULL) {
        ComputedStyle normal = {0}, focused = {0};
        if (style_focus_change_is_outline_only(
                sheet, node, parent, &normal, &focused)) {
            /*
             * Keep the retained display list focus-neutral. The browser
             * compositor paints the authored outline from the live focused
             * style, so moving between outline-only targets does not leave
             * the previous target's outline in cached tiles.
             */
            result->outline_state = normal.outline_state;
            result->outline_color = normal.outline_color;
            result->outline_alpha = normal.outline_alpha;
        }
    }
}

bool layout_reuse_cache_resolve_style(LayoutReuseCache *cache,
                                      const Stylesheet *sheet,
                                      const FontSet *fonts,
                                      lxb_dom_node_t *node,
                                      const ComputedStyle *parent,
                                      ComputedStyle *result)
{
    if (result == NULL) return false;
    uint64_t parent_hash = cache == NULL
        ? 0 : layout_style_parent_hash(parent);
    if (layout_reuse_style_get_hashed(
            cache, node, parent_hash, result)) return true;
    layout_resolve_canonical_style(
        sheet, fonts, stylesheet_web_font_set(sheet),
        node, parent, result);
    layout_reuse_style_put_hashed(
        cache, node, parent_hash, result);
    return false;
}

ComputedStyle layout_initial_root_style(void)
{
    return (ComputedStyle) {
        .display = DISPLAY_BLOCK, .color = 0x000000,
        .color_alpha = 255,
        .background = 0xffffff, .has_background = true,
        .background_alpha = 255,
        .font_scale = 2, .font_size = 16,
        .root_font_size = 16,
        .font_family = FONT_SANS,
        .border_color = UINT32_MAX, .border_alpha = 255,
        .opacity = 255,
        .max_width = STYLE_LENGTH_NONE,
        .max_height = STYLE_LENGTH_NONE,
        .flex_direction = FLEX_ROW, .flex_shrink = 512,
        .align_items = ALIGN_STRETCH,
        .justify_items = ALIGN_STRETCH,
        .align_content = JUSTIFY_STRETCH
    };
}

__attribute__((noinline))
void layout_style_for_node_into(LayoutContext *context,
                                       lxb_dom_node_t *node,
                                       const ComputedStyle *parent,
                                       ComputedStyle *result)
{
    context->style_resolutions++;
    /* Open addressing over the fixed bounded array: hash the node pointer
       to a home slot and probe a short window, evicting the oldest stamp in
       the window on a miss.  This is the single owner of the cache keying
       decision (node pointer only; styles cannot change within one build). */
    const size_t mask = LAYOUT_STYLE_CACHE_CAPACITY - 1;
    size_t home = layout_pointer_hash(node) & mask;
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < LAYOUT_STYLE_CACHE_PROBE_LIMIT; probe++) {
        size_t slot = (home + probe) & mask;
        LayoutStyleCacheEntry *entry = &context->style_cache[slot];
        if (entry->node == node) {
            entry->stamp = ++context->style_cache_clock;
            context->style_cache_hits++;
            *result = entry->style;
            return;
        }
        if (entry->node == NULL) {
            replacement = slot;
            break;
        }
        if (entry->stamp < oldest) {
            oldest = entry->stamp;
            replacement = slot;
        }
    }
#ifndef TILEFINCH_NO_TRACE
    bool reused = false;
    uint64_t style_started_us = layout_performance_now_us();
    reused = layout_reuse_cache_resolve_style(
        context->reuse, context->sheet, context->fonts,
        node, parent, result);
    if (!reused) {
        context->style_resolve_us +=
            layout_performance_now_us() - style_started_us;
    }
#else
    (void) layout_reuse_cache_resolve_style(
        context->reuse, context->sheet, context->fonts,
        node, parent, result);
#endif
    LayoutStyleCacheEntry *entry = &context->style_cache[replacement];
    if (style_selector_cooperation_cancelled(context->sheet)) {
        context->cancelled = true;
        return;
    }
    entry->node = node;
    entry->style = *result;
    entry->stamp = ++context->style_cache_clock;
    context->style_cache_misses++;
}

ComputedStyle layout_style_for_node(LayoutContext *context,
                                           lxb_dom_node_t *node,
                                           const ComputedStyle *parent)
{
    ComputedStyle style;
    layout_style_for_node_into(context, node, parent, &style);
    return style;
}


bool layout_node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *selected_option_node(lxb_dom_node_t *node,
                                            lxb_dom_node_t **first)
{
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        if (layout_node_name_is(child, "option")) {
            if (*first == NULL) *first = child;
            size_t state_length = 0;
            const char *state = document_attribute(
                child, "data-tilefinch-option-selected", &state_length);
            bool selected = state != NULL
                ? state_length == 4 && memcmp(state, "true", 4) == 0
                : lxb_dom_element_has_attribute(
                      lxb_dom_interface_element(child),
                      (const lxb_char_t *) "selected", 8);
            if (selected) {
                return child;
            }
        }
        lxb_dom_node_t *selected = selected_option_node(child, first);
        if (selected != NULL) return selected;
    }
    return NULL;
}

lxb_dom_node_t *select_display_option(lxb_dom_node_t *select)
{
    lxb_dom_node_t *first = NULL;
    lxb_dom_node_t *selected = selected_option_node(select, &first);
    return selected == NULL ? first : selected;
}

bool attribute_is(lxb_dom_node_t *node, const char *name,
                         const char *wanted)
{
    size_t length = 0;
    const char *value = document_attribute(node, name, &length);
    return value != NULL && length == strlen(wanted)
           && strncasecmp(value, wanted, length) == 0;
}

bool layout_node_is_hidden_input(lxb_dom_node_t *node)
{
    return layout_node_name_is(node, "input")
        && attribute_is(node, "type", "hidden");
}

static bool node_descends_from(lxb_dom_node_t *node, lxb_dom_node_t *ancestor)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

bool node_effectively_disabled(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    if (lxb_dom_element_has_attribute(
            lxb_dom_interface_element(node),
            (const lxb_char_t *) "disabled", 8)) return true;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (!layout_node_name_is(at, "fieldset")
            || !lxb_dom_element_has_attribute(
                lxb_dom_interface_element(at),
                (const lxb_char_t *) "disabled", 8)) continue;
        lxb_dom_node_t *legend = NULL;
        for (lxb_dom_node_t *child = at->first_child; child != NULL;
             child = child->next) {
            if (layout_node_name_is(child, "legend")) { legend = child; break; }
        }
        if (legend != NULL && node_descends_from(node, legend)) continue;
        return true;
    }
    return false;
}

const char *first_text_data(lxb_dom_node_t *node, size_t *length)
{
    for (lxb_dom_node_t *child = node == NULL ? NULL : node->first_child;
         child != NULL; child = child->next) {
        const char *text = document_text_data(child, length);
        if (text != NULL && *length != 0) return text;
        text = first_text_data(child, length);
        if (text != NULL && *length != 0) return text;
    }
    *length = 0;
    return NULL;
}

static bool layout_node_index_rebuild(LayoutDocument *layout,
                                      size_t capacity)
{
    if (layout == NULL || capacity < 16
        || (capacity & (capacity - 1u)) != 0
        || capacity > SIZE_MAX / sizeof(*layout->node_index)) return false;
    LayoutNodeIndexEntry *entries = budget_calloc(
        layout->budget, capacity, sizeof(*entries));
    if (entries == NULL) return false;
    size_t mask = capacity - 1u;
    for (size_t i = 0; i < layout->node_index_capacity; i++) {
        LayoutNodeIndexEntry entry = layout->node_index[i];
        if (entry.node == NULL) continue;
        size_t slot = layout_pointer_hash(entry.node) & mask;
        while (entries[slot].node != NULL) slot = (slot + 1u) & mask;
        entries[slot] = entry;
    }
    budget_free(layout->budget, layout->node_index);
    layout->node_index = entries;
    layout->node_index_capacity = capacity;
    return true;
}

static bool layout_node_index_lookup(const LayoutDocument *layout,
                                     const lxb_dom_node_t *node,
                                     size_t *index)
{
    if (layout == NULL || node == NULL || layout->node_index == NULL
        || layout->node_index_capacity == 0) return false;
    size_t mask = layout->node_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    for (size_t probes = 0; probes < layout->node_index_capacity;
         probes++, slot = (slot + 1u) & mask) {
        const LayoutNodeIndexEntry *entry = &layout->node_index[slot];
        if (entry->node == NULL) return false;
        if (entry->node == node) {
            if (entry->index >= layout->node_box_count) return false;
            if (index != NULL) *index = entry->index;
            return true;
        }
    }
    return false;
}

static void layout_node_index_insert(LayoutDocument *layout,
                                     lxb_dom_node_t *node, size_t index)
{
    if (layout == NULL || node == NULL || index > UINT32_MAX
        || layout->node_index_growth_disabled) return;
    if (layout->node_index_capacity == 0
        && !layout_node_index_rebuild(layout, 128)) {
        layout->node_index_growth_disabled = true;
        return;
    }
    if (layout->node_index_count + 1u
        >= layout->node_index_capacity
             - layout->node_index_capacity / 4u) {
        if (layout->node_index_capacity > SIZE_MAX / 2u
            || !layout_node_index_rebuild(
                layout, layout->node_index_capacity * 2u)) {
            layout->node_index_growth_disabled = true;
            return;
        }
    }
    size_t mask = layout->node_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    while (layout->node_index[slot].node != NULL) {
        if (layout->node_index[slot].node == node) {
            layout->node_index[slot].index = (uint32_t) index;
            return;
        }
        slot = (slot + 1u) & mask;
    }
    layout->node_index[slot] = (LayoutNodeIndexEntry) {
        .node = node, .index = (uint32_t) index
    };
    layout->node_index_count++;
}

static size_t layout_node_box_index(const LayoutDocument *layout,
                                    const lxb_dom_node_t *node)
{
    size_t index = 0;
    if (layout_node_index_lookup(layout, node, &index)) return index;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].node == node) return i;
    }
    return SIZE_MAX;
}

static bool layout_set_node_clip_radius_code(
    LayoutDocument *layout, size_t index, int code)
{
    if (layout == NULL || index >= layout->node_box_capacity) return false;
    if (style_border_radius_is_packed(code)
        && layout->node_clip_radius_codes == NULL) {
        layout->node_clip_radius_codes = budget_calloc(
            layout->budget, layout->node_box_capacity,
            sizeof(*layout->node_clip_radius_codes));
        if (layout->node_clip_radius_codes == NULL) return false;
    }
    if (layout->node_clip_radius_codes != NULL) {
        layout->node_clip_radius_codes[index] =
            style_border_radius_is_packed(code) ? code : 0;
    }
    return true;
}

int layout_node_box_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box)
{
    if (layout == NULL || box == NULL || layout->node_boxes == NULL
        || box < layout->node_boxes
        || box >= layout->node_boxes + layout->node_box_count) return 0;
    size_t index = (size_t) (box - layout->node_boxes);
    int code = layout->node_clip_radius_codes == NULL
        ? 0 : layout->node_clip_radius_codes[index];
    return code != 0 ? code : (int) layout_node_box_clip_radius(box);
}

int layout_node_box_effective_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box)
{
    if (layout == NULL || box == NULL) return 0;
    int code = layout_node_box_clip_radius_code(layout, box);
    StyleOverflowClipBox clip_box = layout_node_box_clip_box(box);
    int border_inset_x = (box->width - box->client_width) / 2;
    int border_inset_y = (box->height - box->client_height) / 2;
    if (border_inset_x < 0) border_inset_x = 0;
    if (border_inset_y < 0) border_inset_y = 0;
    int border_inset = border_inset_x > border_inset_y
        ? border_inset_x : border_inset_y;
    /* CSS defines every overflow-clip edge as one cumulative offset from
       the padding edge. Do not clamp at the selected content/border edge and
       then expand: that loses curvature when a positive clip margin crosses
       an intermediate sharp edge. */
    code = style_border_radius_adjust(code, -border_inset);
    int outset = (int) layout_node_box_clip_margin(box);
    if (clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX) {
        outset += border_inset;
    } else if (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX) {
        int selected_inset = box->clip_inset_left > box->clip_inset_top
            ? box->clip_inset_left : box->clip_inset_top;
        int padding_inset = selected_inset - border_inset;
        if (padding_inset > 0) outset -= padding_inset;
    }
    return style_border_radius_outset(
        code, outset, box->client_width, box->client_height);
}

static void layout_build_focus_index(LayoutDocument *layout)
{
    if (layout == NULL || layout->focus_index != NULL) return;
    if (layout->link_count > SIZE_MAX - layout->control_count) return;
    size_t regions = layout->link_count + layout->control_count;
    if (regions == 0 || regions > UINT32_MAX || regions > SIZE_MAX / 2u
        || regions * 2u
               > SIZE_MAX / sizeof(LayoutFocusIndexEntry)) {
        return;
    }
    size_t capacity = 16;
    while (capacity < regions * 2u) {
        if (capacity > SIZE_MAX / 2u) return;
        capacity *= 2u;
    }
    LayoutFocusIndexEntry *entries = budget_calloc(
        layout->budget, capacity, sizeof(*entries));
    if (entries == NULL) return;
    size_t mask = capacity - 1u;
    for (size_t kind = 0; kind < 2; kind++) {
        size_t count = kind == 0 ? layout->link_count : layout->control_count;
        for (size_t i = 0; i < count; i++) {
            lxb_dom_node_t *node = kind == 0 ? layout->links[i].node
                                             : layout->controls[i].node;
            if (node == NULL) continue;
            size_t slot = layout_pointer_hash(node) & mask;
            while (entries[slot].node != NULL
                   && entries[slot].node != node) {
                slot = (slot + 1u) & mask;
            }
            if (entries[slot].node == NULL) entries[slot].node = node;
            if (kind == 0 && entries[slot].link_plus_one == 0) {
                entries[slot].link_plus_one = (uint32_t) i + 1u;
            } else if (kind == 1 && entries[slot].control_plus_one == 0) {
                entries[slot].control_plus_one = (uint32_t) i + 1u;
            }
        }
    }
    layout->focus_index = entries;
    layout->focus_index_capacity = capacity;
}

bool add_node_box(LayoutDocument *layout, lxb_dom_node_t *node,
                         int x, int y, int width, int height,
                         int client_width, int client_height,
                         int content_width, int content_height,
                         int padding_horizontal, int padding_vertical,
                         bool clips_x, bool clips_y, int clip_radius,
                         uint8_t overflow_clip_margin,
                         int clip_inset_left, int clip_inset_top,
                         bool clip_only_x, bool clip_only_y,
                         uint8_t positioned_ancestor_distance,
                         bool cssom_geometry_authoritative,
                         size_t command_start, size_t command_end,
                         size_t scroll_command_start,
                         size_t scroll_command_end,
                         size_t link_start, size_t link_end,
                         size_t control_start, size_t control_end)
{
    if (node == NULL || width < 0 || height < 0) return false;
    if (command_start > UINT32_MAX || command_end > UINT32_MAX
        || scroll_command_start > UINT32_MAX
        || scroll_command_end > UINT32_MAX
        || link_start > UINT32_MAX || link_end > UINT32_MAX
        || control_start > UINT32_MAX || control_end > UINT32_MAX
        || link_start > link_end || control_start > control_end) return false;
    if ((clips_x || clips_y) && LAYOUT_TRACE(layout, CLIP)) {
        size_t id_length = 0, class_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        const char *class_name = document_attribute(node, "class",
                                                     &class_length);
        fprintf(stderr, "layout-clip id=%.*s class=%.*s box=%d,%d,%dx%d "
                "client=%dx%d content=%dx%d axes=%d/%d clip-only=%d/%d "
                "radius=%d commands=%zu..%zu\n",
                (int) id_length, id == NULL ? "" : id,
                (int) class_length, class_name == NULL ? "" : class_name,
                x, y, width, height, client_width, client_height,
                content_width, content_height, clips_x, clips_y,
                clip_only_x, clip_only_y, clip_radius,
                scroll_command_start, scroll_command_end);
    }
    size_t existing = layout_node_box_index(layout, node);
    if (existing != SIZE_MAX) {
        size_t i = existing;
        if (layout->retain_container_padding
            && layout->node_container_padding_sums != NULL
            && i < layout->node_container_padding_capacity) {
            unsigned horizontal = padding_horizontal < 0 ? 0u
                : (padding_horizontal > UINT16_MAX
                   ? UINT16_MAX : (unsigned) padding_horizontal);
            unsigned vertical = padding_vertical < 0 ? 0u
                : (padding_vertical > UINT16_MAX
                   ? UINT16_MAX : (unsigned) padding_vertical);
            layout->node_container_padding_sums[i] =
                horizontal | (vertical << 16);
        }
        uint32_t *interactions = layout->node_interaction_ranges
            + i * LAYOUT_NODE_INTERACTION_STRIDE;
        interactions[LAYOUT_NODE_LINK_START] = (uint32_t) link_start;
        interactions[LAYOUT_NODE_LINK_END] = (uint32_t) link_end;
        interactions[LAYOUT_NODE_CONTROL_START] = (uint32_t) control_start;
        interactions[LAYOUT_NODE_CONTROL_END] = (uint32_t) control_end;
        layout->node_boxes[i] = (LayoutNodeBox) {
            .node = node, .x = x, .y = y, .width = width, .height = height,
            .client_width = client_width, .client_height = client_height,
            .content_width = content_width, .content_height = content_height,
            .scroll_x = layout->node_boxes[i].scroll_x,
            .scroll_y = layout->node_boxes[i].scroll_y,
            .command_start = (uint32_t) command_start,
            .command_end = (uint32_t) command_end,
            .scroll_command_start = (uint32_t) scroll_command_start,
            .scroll_command_end = (uint32_t) scroll_command_end,
            .clip_radius = (uint16_t) (
                (style_border_radius_maximum(clip_radius)
                     > (int) LAYOUT_CLIP_RADIUS_MASK
                    ? LAYOUT_CLIP_RADIUS_MASK
                    : style_border_radius_maximum(clip_radius))
                | ((overflow_clip_margin
                    & STYLE_OVERFLOW_CLIP_MARGIN_MASK)
                   << LAYOUT_CLIP_MARGIN_SHIFT)),
            .clips_x = clips_x, .clips_y = clips_y,
            .clip_only_x = clip_only_x, .clip_only_y = clip_only_y,
            .clip_flags_reserved = (uint8_t) (
                (overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
                >> STYLE_OVERFLOW_CLIP_BOX_SHIFT),
            .positioned_ancestor_distance = positioned_ancestor_distance,
            .cssom_geometry_authoritative = cssom_geometry_authoritative,
            .clip_inset_left = (uint8_t) (
                clip_inset_left > UINT8_MAX ? UINT8_MAX
                : (clip_inset_left < 0 ? 0 : clip_inset_left)),
            .clip_inset_top = (uint8_t) (
                clip_inset_top > UINT8_MAX ? UINT8_MAX
                : (clip_inset_top < 0 ? 0 : clip_inset_top))
        };
        int retained_clip_radius = clips_x && clips_y
            ? clip_radius : style_border_radius_maximum(clip_radius);
        if (!layout_set_node_clip_radius_code(
                layout, i, retained_clip_radius)) {
            return false;
        }
        layout_node_index_insert(layout, node, i);
        return true;
    }
    if (layout->node_box_count == layout->node_box_capacity) {
        size_t capacity = layout->node_box_capacity == 0
                          ? 64 : layout->node_box_capacity * 2;
        if (capacity > SIZE_MAX
                       / (LAYOUT_NODE_INTERACTION_STRIDE
                          * sizeof(uint32_t))) return false;
        uint32_t *interactions = budget_realloc(
            layout->budget, layout->node_interaction_ranges,
            capacity * LAYOUT_NODE_INTERACTION_STRIDE
                * sizeof(*interactions));
        if (interactions == NULL) return false;
        layout->node_interaction_ranges = interactions;
        layout->node_interaction_capacity = capacity;
        if (layout->retain_container_padding) {
            uint32_t *padding_sums = budget_realloc(
                layout->budget, layout->node_container_padding_sums,
                capacity * sizeof(*padding_sums));
            if (padding_sums == NULL) return false;
            memset(padding_sums + layout->node_container_padding_capacity, 0,
                   (capacity - layout->node_container_padding_capacity)
                       * sizeof(*padding_sums));
            layout->node_container_padding_sums = padding_sums;
            layout->node_container_padding_capacity = capacity;
        }
        LayoutNodeBox *boxes = budget_realloc(
            layout->budget, layout->node_boxes, capacity * sizeof(*boxes));
        if (boxes == NULL) return false;
        layout->node_boxes = boxes;
        if (layout->node_clip_radius_codes != NULL) {
            int32_t *codes = budget_realloc(
                layout->budget, layout->node_clip_radius_codes,
                capacity * sizeof(*codes));
            if (codes == NULL) return false;
            memset(codes + layout->node_box_capacity, 0,
                   (capacity - layout->node_box_capacity) * sizeof(*codes));
            layout->node_clip_radius_codes = codes;
        }
        layout->node_box_capacity = capacity;
    }
    size_t index = layout->node_box_count++;
    if (layout->retain_container_padding) {
        unsigned horizontal = padding_horizontal < 0 ? 0u
            : (padding_horizontal > UINT16_MAX
               ? UINT16_MAX : (unsigned) padding_horizontal);
        unsigned vertical = padding_vertical < 0 ? 0u
            : (padding_vertical > UINT16_MAX
               ? UINT16_MAX : (unsigned) padding_vertical);
        layout->node_container_padding_sums[index] =
            horizontal | (vertical << 16);
    }
    uint32_t *interactions =
        layout->node_interaction_ranges
        + index * LAYOUT_NODE_INTERACTION_STRIDE;
    interactions[LAYOUT_NODE_LINK_START] = (uint32_t) link_start;
    interactions[LAYOUT_NODE_LINK_END] = (uint32_t) link_end;
    interactions[LAYOUT_NODE_CONTROL_START] = (uint32_t) control_start;
    interactions[LAYOUT_NODE_CONTROL_END] = (uint32_t) control_end;
    layout->node_boxes[index] = (LayoutNodeBox) {
        .node = node, .x = x, .y = y, .width = width, .height = height,
        .client_width = client_width, .client_height = client_height,
        .content_width = content_width, .content_height = content_height,
        .command_start = (uint32_t) command_start,
        .command_end = (uint32_t) command_end,
        .scroll_command_start = (uint32_t) scroll_command_start,
        .scroll_command_end = (uint32_t) scroll_command_end,
        .clip_radius = (uint16_t) (
            (style_border_radius_maximum(clip_radius)
                 > (int) LAYOUT_CLIP_RADIUS_MASK
                ? LAYOUT_CLIP_RADIUS_MASK
                : style_border_radius_maximum(clip_radius))
            | ((overflow_clip_margin
                & STYLE_OVERFLOW_CLIP_MARGIN_MASK)
               << LAYOUT_CLIP_MARGIN_SHIFT)),
        .clips_x = clips_x, .clips_y = clips_y,
        .clip_only_x = clip_only_x, .clip_only_y = clip_only_y,
        .clip_flags_reserved = (uint8_t) (
            (overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
            >> STYLE_OVERFLOW_CLIP_BOX_SHIFT),
        .positioned_ancestor_distance = positioned_ancestor_distance,
        .cssom_geometry_authoritative = cssom_geometry_authoritative,
        .clip_inset_left = (uint8_t) (
            clip_inset_left > UINT8_MAX ? UINT8_MAX
            : (clip_inset_left < 0 ? 0 : clip_inset_left)),
        .clip_inset_top = (uint8_t) (
            clip_inset_top > UINT8_MAX ? UINT8_MAX
            : (clip_inset_top < 0 ? 0 : clip_inset_top))
    };
    int retained_clip_radius = clips_x && clips_y
        ? clip_radius : style_border_radius_maximum(clip_radius);
    if (!layout_set_node_clip_radius_code(
            layout, index, retained_clip_radius)) {
        return false;
    }
    layout_node_index_insert(layout, node, index);
    return true;
}

static void compact_layout_storage(LayoutDocument *layout)
{
    /* Interaction ranges are needed only while flex/grid/table placement can
       still translate item subtrees. Do not retain this parallel sidecar
       after geometry is final. */
    budget_free(layout->budget, layout->node_interaction_ranges);
    layout->node_interaction_ranges = NULL;
    layout->node_interaction_capacity = 0;
    if (layout->count != 0 && layout->count < layout->capacity) {
        DrawCommand *commands = budget_realloc(
            layout->budget, layout->commands,
            layout->count * sizeof(*commands));
        if (commands != NULL) {
            layout->commands = commands;
            layout->capacity = layout->count;
        }
    }
    if (layout->link_count != 0
        && layout->link_count < layout->link_capacity) {
        LinkRegion *links = budget_realloc(
            layout->budget, layout->links,
            layout->link_count * sizeof(*links));
        if (links != NULL) {
            layout->links = links;
            layout->link_capacity = layout->link_count;
        }
    }
    if (layout->control_count != 0
        && layout->control_count < layout->control_capacity) {
        ControlRegion *controls = budget_realloc(
            layout->budget, layout->controls,
            layout->control_count * sizeof(*controls));
        if (controls != NULL) {
            layout->controls = controls;
            layout->control_capacity = layout->control_count;
        }
    }
    if (layout->sticky_count != 0
        && layout->sticky_count < layout->sticky_capacity) {
        StickyRange *ranges = budget_realloc(
            layout->budget, layout->sticky_ranges,
            layout->sticky_count * sizeof(*ranges));
        if (ranges != NULL) {
            layout->sticky_ranges = ranges;
            layout->sticky_capacity = layout->sticky_count;
        }
    }
    if (layout->fixed_count != 0
        && layout->fixed_count < layout->fixed_capacity) {
        FixedRange *ranges = budget_realloc(
            layout->budget, layout->fixed_ranges,
            layout->fixed_count * sizeof(*ranges));
        if (ranges != NULL) {
            layout->fixed_ranges = ranges;
            layout->fixed_capacity = layout->fixed_count;
        }
    }
    if (layout->node_box_count == 0) {
        budget_free(layout->budget, layout->node_boxes);
        budget_free(layout->budget, layout->node_clip_radius_codes);
        layout->node_boxes = NULL;
        layout->node_clip_radius_codes = NULL;
        layout->node_box_capacity = 0;
    } else if (layout->node_box_count < layout->node_box_capacity) {
        LayoutNodeBox *boxes = budget_realloc(
            layout->budget, layout->node_boxes,
            layout->node_box_count * sizeof(*boxes));
        if (boxes != NULL) {
            layout->node_boxes = boxes;
            layout->node_box_capacity = layout->node_box_count;
        }
    }
}

void trace_flex_translation(LayoutContext *context,
                                   const char *phase,
                                   lxb_dom_node_t *container,
                                   lxb_dom_node_t *item,
                                   int alignment,
                                   int content_x,
                                   int content_width,
                                   int box_x,
                                   int box_width,
                                   int target_x,
                                   int dx)
{
    if (!LAYOUT_TRACE(context->layout, FLEX_TRANSLATE)
        || context->trace_flex_translate_lines++ >= 64) return;
    size_t container_class_length = 0;
    size_t item_class_length = 0;
    const char *container_class = document_attribute(
        container, "class", &container_class_length);
    const char *item_class = document_attribute(
        item, "class", &item_class_length);
    fprintf(stderr,
            "layout-flex-translate phase=%s container=%.*s item=%.*s "
            "alignment=%d content=%d/%d box=%d/%d target=%d dx=%d\n",
            phase,
            (int) container_class_length,
            container_class == NULL ? "" : container_class,
            (int) item_class_length,
            item_class == NULL ? "" : item_class,
            alignment, content_x, content_width, box_x, box_width,
            target_x, dx);
}

static bool trace_flex_container_matches(const LayoutContext *context,
                                         lxb_dom_node_t *container)
{
    const char *wanted =
        TILEFINCH_TRACE_COMPILED_IN != 0 ? context->layout->trace_flex_class
                                      : NULL;
    if (wanted == NULL || wanted[0] == '\0' || container == NULL) {
        return false;
    }
    size_t class_length = 0;
    const char *class_name = document_attribute(container, "class",
                                                 &class_length);
    return class_name != NULL && strstr(class_name, wanted) != NULL;
}

void trace_flex_sizing(LayoutContext *context, const char *phase,
                              lxb_dom_node_t *container,
                              lxb_dom_node_t *item_node,
                              const ComputedStyle *item_style,
                              int content_width,
                              int total_basis, int remaining, int cursor_x,
                              int basis, int used_width)
{
    if (!trace_flex_container_matches(context, container)
        || context->trace_flex_sizing_lines++ >= 128) return;
    size_t container_class_length = 0, item_class_length = 0;
    const char *container_class = document_attribute(
        container, "class", &container_class_length);
    const char *item_class = item_node == NULL ? NULL
        : document_attribute(item_node, "class", &item_class_length);
    fprintf(stderr,
            "layout-flex-size phase=%s container=%.*s item=%.*s "
            "content=%d total-basis=%d remaining=%d cursor=%d basis=%d "
            "used=%d grow=%d shrink=%d margins=%d/%d min=%d max=%d\n",
            phase, (int) container_class_length,
            container_class == NULL ? "" : container_class,
            (int) item_class_length, item_class == NULL ? "" : item_class,
            content_width, total_basis, remaining, cursor_x, basis,
            used_width, item_style == NULL ? 0 : item_style->flex_grow,
            item_style == NULL ? 0 : item_style->flex_shrink,
            item_style == NULL ? 0 : item_style->margin.left,
            item_style == NULL ? 0 : item_style->margin.right,
            item_style == NULL ? 0 : item_style->min_width,
            item_style == NULL ? 0 : item_style->max_width);
}

bool layout_batch_cooperate(LayoutContext *context,
                                   size_t work_units)
{
    if (context == NULL || work_units == 0) return true;
    if (work_units <= SIZE_MAX - context->layout->layout_work_units) {
        context->layout->layout_work_units += work_units;
    } else {
        context->layout->layout_work_units = SIZE_MAX;
    }
    context->slice_units = work_units;
    context->slice_last_node = NULL;
    layout_finish_work_slice(context);
    context->layout->cooperative_yields++;
    bool keep_going = tilefinch_platform_cooperate(
        "layout-index", context->layout->layout_work_units);
    if (!keep_going) context->cancelled = true;
    return keep_going;
}

bool layout_batch_checkpoint(LayoutContext *context, size_t at,
                                    size_t count, size_t *checkpoint_at)
{
    if (context == NULL || at + 1 < *checkpoint_at) return true;
    size_t completed = at + 1;
    size_t units = completed > count ? count : completed;
    units -= *checkpoint_at - 4096;
    *checkpoint_at = completed + 4096;
    return layout_batch_cooperate(context, units);
}

static void compact_layout_storage(LayoutDocument *layout);
static void layout_build_focus_index(LayoutDocument *layout);
static bool layout_collect_container_state(Stylesheet *stylesheet,
                                           Budget *budget,
                                           const LayoutDocument *layout);
static bool layout_document_has_inline_container_units(
    const PocDocument *document, const Stylesheet *stylesheet);

typedef enum {
    LAYOUT_JOB_INIT = 0,
    LAYOUT_JOB_FLOW,
    LAYOUT_JOB_COMPACT,
    LAYOUT_JOB_VISIBILITY,
    LAYOUT_JOB_PAINT_ORDER,
    LAYOUT_JOB_SPATIAL_INDEX,
    LAYOUT_JOB_SCROLL_METADATA,
    LAYOUT_JOB_FINISH,
    LAYOUT_JOB_CONTAINER_STATE,
    LAYOUT_JOB_DONE,
    LAYOUT_JOB_ERROR
} LayoutJobPhase;

struct LayoutBuildJob {
    Budget *budget;
    const PocDocument *document;
    const Stylesheet *stylesheet;
    const FontSet *fonts;
    const ImageResources *images;
    ViewportContext viewport;
    LayoutReuseCache *reuse;
    LayoutDocument layout;
    LayoutContext *context;
    lxb_dom_node_t *body;
    ComputedStyle html_style;
    ComputedStyle body_style;
    int viewport_width;
    int viewport_height;
    int html_height;
    int bottom;
    size_t fingerprint_at;
    uint64_t text_fingerprint;
    uint64_t build_active_us;
    LayoutJobPhase phase;
    LayoutBuildStatus status;
    size_t resumable_phases;
    size_t resumable_passes;
    uint64_t maximum_resumable_phase_us;
    uint64_t container_previous_signature;
    bool probe_pass;
    bool retain_container_padding;
    bool taken;
};

static void layout_job_fail(LayoutBuildJob *job,
                            LayoutBuildStatus status)
{
    if (job == NULL || job->status != LAYOUT_BUILD_PENDING) return;
    if (job->context != NULL) {
        layout_release_context(job->context, job->budget);
        job->context = NULL;
    }
    layout_destroy(&job->layout);
    job->phase = LAYOUT_JOB_ERROR;
    job->status = status;
}

static bool layout_job_init(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = budget_calloc(job->budget, 1, sizeof(*context));
    if (context == NULL) return false;
    job->context = context;
    memset(layout, 0, sizeof(*layout));
    layout->retain_container_padding = job->retain_container_padding;
    layout_read_trace_environment(layout);
    layout->budget = job->budget;
    layout->width = job->viewport_width;
    layout->scroll_width = job->viewport_width;
    layout->viewport = job->viewport;
    layout->fonts = job->fonts;
    layout->web_fonts = stylesheet_web_font_set(job->stylesheet);
    layout->page_background = 0xffffff;
    job->body = document_body_node(job->document);
    if (job->body == NULL) return false;

    context->layout = layout;
    context->sheet = job->stylesheet;
    context->fonts = job->fonts;
    context->web_fonts = layout->web_fonts;
    context->images = job->images;
    context->reuse = job->reuse;
    if (job->stylesheet != NULL) {
        context->style_variable_cache_owned = style_variable_cache_begin(
            (Stylesheet *) job->stylesheet, job->budget);
        context->style_variable_cache_bytes =
            style_variable_cache_bytes(job->stylesheet);
        context->style_rule_queries_at_start =
            job->stylesheet->rule_index_queries;
        context->style_rule_candidates_at_start =
            job->stylesheet->rule_index_candidates;
        context->style_variable_lookups_at_start =
            job->stylesheet->variable_lookup_calls;
        context->style_variable_rule_candidates_at_start =
            job->stylesheet->variable_rule_candidates;
        context->style_variable_cache_hits_at_start =
            job->stylesheet->variable_cache_hits;
        context->style_variable_cache_misses_at_start =
            job->stylesheet->variable_cache_misses;
        context->style_variable_cache_negative_hits_at_start =
            job->stylesheet->variable_cache_negative_hits;
        context->style_variable_cache_evictions_at_start =
            job->stylesheet->variable_cache_evictions;
        context->style_deferred_rule_applications_at_start =
            job->stylesheet->deferred_rule_applications;
        context->style_deferred_rule_us_at_start =
            job->stylesheet->deferred_rule_us;
    }
    layout_reuse_cache_prepare(job->reuse, job->stylesheet, job->fonts,
                               job->images, job->viewport_width);
    context->slice_started_ns = tilefinch_platform_monotonic_time_ns();
    if (job->stylesheet != NULL) {
        context->style_selector_cooperation_owned =
            style_selector_cooperation_begin(
                (Stylesheet *) job->stylesheet,
                layout_selector_cooperate, context);
    }

    ComputedStyle root = layout_initial_root_style();
    lxb_dom_node_t *html = job->body->parent;
    bool have_html = html != NULL
                     && html->type == LXB_DOM_NODE_TYPE_ELEMENT
                     && layout_node_name_is(html, "html");
    job->html_style = have_html
        ? layout_style_for_node(context, html, &root) : root;
    resolve_padding(context->sheet, &job->html_style, job->viewport_width);
    job->html_height = style_content_height(
        context->sheet, &job->html_style, job->viewport_width,
        job->viewport_height);
    if (!(job->html_style.has_height
          && (!job->html_style.height_percent || job->html_height > 0))) {
        job->html_height = job->viewport_height;
    }
    if (job->html_style.has_background) {
        layout->page_background = blend_color_over(
            job->html_style.background, job->html_style.background_alpha,
            layout->page_background);
    }
    job->body_style = layout_style_for_node(
        context, job->body, &job->html_style);
    if (job->body_style.has_background) {
        layout->page_background = blend_color_over(
            job->body_style.background, job->body_style.background_alpha,
            layout->page_background);
    }
    return !context->cancelled;
}

static bool layout_job_flow(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = job->context;
    PositionedBox initial_positioned_box = {
        .node = NULL, .x = 0, .y = 0,
        .width = job->viewport_width, .height = job->html_height
    };
    if (!layout_block(context, job->body, &job->html_style,
                      0, 0, job->viewport_width, job->html_height, false,
                      &initial_positioned_box, &job->bottom)
        || context->cancelled) return false;

    int document_bottom = job->bottom;
    const LayoutNodeBox *body_box = layout_box_for_node(layout, job->body);
    if (body_box != NULL && !body_box->clips_y) {
        int overflow_bottom = layout_add_coordinate(
            layout_add_coordinate(body_box->y, body_box->content_height),
            job->body_style.margin.bottom);
        if (overflow_bottom > document_bottom) {
            document_bottom = overflow_bottom;
        }
    }
    if (body_box != NULL && !body_box->clips_x) {
        int overflow_right = layout_add_coordinate(
            layout_add_coordinate(body_box->x, body_box->content_width),
            job->body_style.margin.right);
        if (overflow_right > layout->scroll_width) {
            layout->scroll_width = overflow_right;
        }
    }
    layout->height = document_bottom < job->viewport_height
        ? job->viewport_height : layout_clamp_coordinate(document_bottom);
    return true;
}

static void layout_job_fingerprint_commands(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    for (; job->fingerprint_at < layout->count; job->fingerprint_at++) {
        size_t i = job->fingerprint_at;
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL
            || draw_command_is_text_shadow(command)) continue;
        uint64_t structure[] = {
            i, command->text_length,
            (uint32_t) command->radius
                & (LAYOUT_TEXT_FIND_SPACE_BEFORE
                   | LAYOUT_TEXT_FIND_BLOCK_START)
        };
        for (size_t field = 0;
             field < sizeof(structure) / sizeof(structure[0]); field++) {
            for (size_t byte = 0; byte < sizeof(structure[field]); byte++) {
                job->text_fingerprint ^= (unsigned char) (
                    structure[field] >> (byte * 8u));
                job->text_fingerprint *= UINT64_C(1099511628211);
            }
        }
        for (size_t byte = 0; byte < command->text_length; byte++) {
            job->text_fingerprint ^= (unsigned char) command->text[byte];
            job->text_fingerprint *= UINT64_C(1099511628211);
        }
    }
    layout->text_fingerprint = job->text_fingerprint;
}

static void layout_job_capture_performance(LayoutBuildJob *job)
{
    LayoutDocument *layout = &job->layout;
    LayoutContext *context = job->context;
    const Stylesheet *stylesheet = job->stylesheet;
    layout->performance.style_resolutions = context->style_resolutions;
    layout->performance.style_cache_hits = context->style_cache_hits;
    layout->performance.style_cache_misses = context->style_cache_misses;
    layout->performance.style_resolve_us = context->style_resolve_us;
    if (stylesheet != NULL) {
        layout->performance.style_rule_queries =
            stylesheet->rule_index_queries
            - context->style_rule_queries_at_start;
        layout->performance.style_rule_candidates =
            stylesheet->rule_index_candidates
            - context->style_rule_candidates_at_start;
        layout->performance.style_variable_lookups =
            stylesheet->variable_lookup_calls
            - context->style_variable_lookups_at_start;
        layout->performance.style_variable_rule_candidates =
            stylesheet->variable_rule_candidates
            - context->style_variable_rule_candidates_at_start;
        layout->performance.style_variable_cache_hits =
            stylesheet->variable_cache_hits
            - context->style_variable_cache_hits_at_start;
        layout->performance.style_variable_cache_misses =
            stylesheet->variable_cache_misses
            - context->style_variable_cache_misses_at_start;
        layout->performance.style_variable_cache_negative_hits =
            stylesheet->variable_cache_negative_hits
            - context->style_variable_cache_negative_hits_at_start;
        layout->performance.style_variable_cache_evictions =
            stylesheet->variable_cache_evictions
            - context->style_variable_cache_evictions_at_start;
        layout->performance.style_variable_cache_bytes =
            context->style_variable_cache_bytes;
        layout->performance.style_deferred_rule_applications =
            stylesheet->deferred_rule_applications
            - context->style_deferred_rule_applications_at_start;
        layout->performance.style_deferred_rule_us =
            stylesheet->deferred_rule_us
            - context->style_deferred_rule_us_at_start;
    }
    layout->performance.intrinsic_width_visits =
        context->intrinsic_width_visits;
    layout->performance.intrinsic_min_visits =
        context->intrinsic_min_visits;
    layout->performance.intrinsic_cache_hits =
        context->intrinsic_cache_hits;
    layout->performance.intrinsic_cache_misses =
        context->intrinsic_cache_misses;
    layout->performance.intrinsic_paired_text_measurements =
        context->intrinsic_paired_text_measurements;
    layout->performance.margin_collapse_visits =
        context->margin_collapse_visits;
    layout->performance.margin_cache_hits = context->margin_cache_hits;
    layout->performance.margin_cache_misses = context->margin_cache_misses;
    layout->performance.flat_iterator_passes = context->flat_iterator_passes;
    layout->performance.flat_iterator_yields = context->flat_iterator_yields;
    layout->performance.flex_iterator_passes = context->flex_iterator_passes;
    layout->performance.flex_iterator_yields = context->flex_iterator_yields;
    layout->performance.flex_basis_requests = context->flex_basis_resolutions;
    layout->performance.flex_minimum_requests =
        context->flex_minimum_resolutions;
    layout->performance.resumable_phases = job->resumable_phases;
    layout->performance.resumable_passes = job->resumable_passes;
    layout->performance.maximum_resumable_phase_us =
        job->maximum_resumable_phase_us;
    layout->performance.total_us = job->build_active_us;
}

static void layout_job_record_phase(LayoutBuildJob *job,
                                    uint64_t started_us)
{
    uint64_t finished_us = layout_performance_now_us();
    uint64_t elapsed_us = finished_us >= started_us
        ? finished_us - started_us : 0;
    job->build_active_us += elapsed_us;
    job->resumable_phases++;
    if (elapsed_us > job->maximum_resumable_phase_us) {
        job->maximum_resumable_phase_us = elapsed_us;
    }
}

LayoutBuildJob *layout_build_job_begin(
    Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse)
{
    if (budget == NULL || document == NULL || viewport == NULL
        || viewport->css_width <= 0 || viewport->css_height <= 0
        || viewport->device_width <= 0 || viewport->device_height <= 0) {
        return NULL;
    }
    LayoutBuildJob *job = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*job));
    if (job == NULL) return NULL;
    job->budget = budget;
    job->document = document;
    job->stylesheet = stylesheet;
    job->fonts = fonts;
    job->images = images;
    job->viewport = *viewport;
    job->viewport_width = viewport->css_width;
    job->viewport_height = viewport->css_height;
    job->reuse = reuse;
    job->text_fingerprint = UINT64_C(1469598103934665603);
    job->phase = LAYOUT_JOB_INIT;
    job->status = LAYOUT_BUILD_PENDING;
    bool container_queries = stylesheet_has_container_queries(stylesheet)
        || layout_document_has_inline_container_units(document, stylesheet);
    job->probe_pass = container_queries;
    job->retain_container_padding = job->probe_pass;
    if (container_queries) {
        job->container_previous_signature =
            style_container_layout_state_signature(stylesheet);
    }
    return job;
}

LayoutBuildStatus layout_build_job_pump(LayoutBuildJob *job)
{
    if (job == NULL) return LAYOUT_BUILD_FAILED;
    if (job->status != LAYOUT_BUILD_PENDING) return job->status;
    uint64_t started_us = layout_performance_now_us();
    bool okay = true;
    switch (job->phase) {
    case LAYOUT_JOB_INIT:
        okay = layout_job_init(job);
        if (okay) {
            job->resumable_passes++;
            job->layout.performance.root_style_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_FLOW;
        }
        break;
    case LAYOUT_JOB_FLOW:
        okay = layout_job_flow(job);
        if (okay) {
            job->layout.performance.flow_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_COMPACT;
        }
        break;
    case LAYOUT_JOB_COMPACT:
        compact_layout_storage(&job->layout);
        layout_job_fingerprint_commands(job);
        job->layout.performance.compact_us =
            layout_performance_now_us() - started_us;
        job->phase = LAYOUT_JOB_VISIBILITY;
        break;
    case LAYOUT_JOB_VISIBILITY:
        okay = layout_resolve_visibility(job->context);
        if (okay) layout_build_focus_index(&job->layout);
        if (okay) {
            job->layout.performance.focus_index_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_PAINT_ORDER;
        }
        break;
    case LAYOUT_JOB_PAINT_ORDER:
        okay = build_paint_order(&job->layout, job->context);
        if (okay) {
            job->layout.performance.paint_order_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_SPATIAL_INDEX;
        }
        break;
    case LAYOUT_JOB_SPATIAL_INDEX:
        okay = build_spatial_index(&job->layout, job->context);
        if (okay) {
            job->layout.performance.spatial_index_us =
                layout_performance_now_us() - started_us;
            job->phase = LAYOUT_JOB_SCROLL_METADATA;
        }
        break;
    case LAYOUT_JOB_SCROLL_METADATA:
        okay = layout_build_scroll_metadata(
            &job->layout, job->stylesheet);
        if (okay) job->phase = LAYOUT_JOB_FINISH;
        break;
    case LAYOUT_JOB_FINISH:
        layout_finish_work_slice(job->context);
        job->layout.scroll_width = root_scroll_width_after_clipping(
            &job->layout, job->viewport_width);
        job->layout.performance.finalize_us =
            layout_performance_now_us() - started_us;
        layout_job_record_phase(job, started_us);
        layout_job_capture_performance(job);
        layout_release_context(job->context, job->budget);
        job->context = NULL;
        if (job->probe_pass) {
            job->phase = LAYOUT_JOB_CONTAINER_STATE;
            return job->status;
        }
        job->phase = LAYOUT_JOB_DONE;
        job->status = LAYOUT_BUILD_COMPLETE;
        return job->status;
    case LAYOUT_JOB_CONTAINER_STATE: {
        okay = layout_collect_container_state(
            (Stylesheet *) job->stylesheet, job->budget, &job->layout);
        if (!okay) break;
        uint64_t measured_signature =
            style_container_layout_state_signature(job->stylesheet);
        job->probe_pass = false;
        job->retain_container_padding = false;
        if (measured_signature == job->container_previous_signature) {
            layout_job_record_phase(job, started_us);
            job->layout.performance.total_us = job->build_active_us;
            job->layout.performance.resumable_phases =
                job->resumable_phases;
            job->layout.performance.resumable_passes =
                job->resumable_passes;
            job->layout.performance.maximum_resumable_phase_us =
                job->maximum_resumable_phase_us;
            job->phase = LAYOUT_JOB_DONE;
            job->status = LAYOUT_BUILD_COMPLETE;
            return job->status;
        }
        layout_destroy(&job->layout);
        layout_reuse_cache_reset(job->reuse);
        job->body = NULL;
        job->fingerprint_at = 0;
        job->text_fingerprint = UINT64_C(1469598103934665603);
        job->phase = LAYOUT_JOB_INIT;
        break;
    }
    case LAYOUT_JOB_DONE:
        job->status = LAYOUT_BUILD_COMPLETE;
        return job->status;
    case LAYOUT_JOB_ERROR:
    default:
        return job->status;
    }
    layout_job_record_phase(job, started_us);
    if (!okay || (job->context != NULL && job->context->cancelled)) {
        LayoutBuildStatus failure =
            job->context != NULL && job->context->cancelled
                ? LAYOUT_BUILD_CANCELLED : LAYOUT_BUILD_FAILED;
        layout_job_fail(job, failure);
        return job->status;
    }
    return job->status;
}

bool layout_build_job_take(LayoutBuildJob *job, LayoutDocument *layout)
{
    if (job == NULL || layout == NULL
        || job->status != LAYOUT_BUILD_COMPLETE || job->taken) return false;
    *layout = job->layout;
    memset(&job->layout, 0, sizeof(job->layout));
    job->taken = true;
    return true;
}

void layout_build_job_cancel(LayoutBuildJob *job)
{
    if (job == NULL || job->status != LAYOUT_BUILD_PENDING) return;
    if (job->context != NULL) job->context->cancelled = true;
    layout_job_fail(job, LAYOUT_BUILD_CANCELLED);
}

void layout_build_job_destroy(LayoutBuildJob *job)
{
    if (job == NULL) return;
    if (job->context != NULL) {
        layout_release_context(job->context, job->budget);
        job->context = NULL;
    }
    if (!job->taken) layout_destroy(&job->layout);
    Budget *budget = job->budget;
    memset(job, 0, sizeof(*job));
    budget_free(budget, job);
}

static bool layout_build_context_resumable_sync(
    LayoutDocument *layout, Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse)
{
    LayoutBuildJob *job = layout_build_job_begin(
        budget, document, stylesheet, fonts, images, viewport, reuse);
    if (job == NULL) return false;
    LayoutBuildStatus status = LAYOUT_BUILD_PENDING;
    while (status == LAYOUT_BUILD_PENDING) {
        status = layout_build_job_pump(job);
    }
    bool okay = status == LAYOUT_BUILD_COMPLETE
                && layout_build_job_take(job, layout);
    layout_build_job_destroy(job);
    return okay;
}

static bool layout_build_context_internal(
    LayoutDocument *layout, Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse, int preview_y_limit, bool *preview_truncated,
    bool retain_container_padding)
{
    if (layout == NULL || budget == NULL || document == NULL
        || viewport == NULL || viewport->css_width <= 0
        || viewport->css_height <= 0 || viewport->device_width <= 0
        || viewport->device_height <= 0) return false;
    uint64_t build_started_us = layout_performance_now_us();
    int viewport_width = viewport->css_width;
    int viewport_height = viewport->css_height;
    memset(layout, 0, sizeof(*layout));
    layout->retain_container_padding = retain_container_padding;
    layout_read_trace_environment(layout);
    layout->budget = budget;
    layout->width = viewport_width;
    layout->scroll_width = viewport_width;
    layout->viewport = *viewport;
    layout->fonts = fonts;
    layout->web_fonts = stylesheet_web_font_set(stylesheet);
    layout->page_background = 0xffffff;
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL) return false;
    LayoutContext *context = budget_calloc(budget, 1, sizeof(*context));
    if (context == NULL) return false;
    context->layout = layout;
    context->sheet = stylesheet;
    context->fonts = fonts;
    context->web_fonts = layout->web_fonts;
    context->images = images;
    context->reuse = reuse;
    context->preview_y_limit = preview_y_limit;
    if (stylesheet != NULL) {
        context->style_variable_cache_owned = style_variable_cache_begin(
            (Stylesheet *) stylesheet, budget);
        context->style_variable_cache_bytes =
            style_variable_cache_bytes(stylesheet);
        context->style_rule_queries_at_start =
            stylesheet->rule_index_queries;
        context->style_rule_candidates_at_start =
            stylesheet->rule_index_candidates;
        context->style_variable_lookups_at_start =
            stylesheet->variable_lookup_calls;
        context->style_variable_rule_candidates_at_start =
            stylesheet->variable_rule_candidates;
        context->style_variable_cache_hits_at_start =
            stylesheet->variable_cache_hits;
        context->style_variable_cache_misses_at_start =
            stylesheet->variable_cache_misses;
        context->style_variable_cache_negative_hits_at_start =
            stylesheet->variable_cache_negative_hits;
        context->style_variable_cache_evictions_at_start =
            stylesheet->variable_cache_evictions;
        context->style_deferred_rule_applications_at_start =
            stylesheet->deferred_rule_applications;
        context->style_deferred_rule_us_at_start =
            stylesheet->deferred_rule_us;
    }
    layout_reuse_cache_prepare(
        reuse, stylesheet, fonts, images, viewport_width);
    context->slice_started_ns = tilefinch_platform_monotonic_time_ns();
    if (stylesheet != NULL) {
        context->style_selector_cooperation_owned =
            style_selector_cooperation_begin(
                (Stylesheet *) stylesheet, layout_selector_cooperate,
                context);
    }
    if (LAYOUT_TRACE(layout, LAYOUT) && stylesheet != NULL) {
        fprintf(stderr, "layout-styles rules=%zu variables=%zu\n",
                stylesheet->count, stylesheet->variable_count);
        for (size_t i = 0; i < stylesheet->count; i++) {
            const StyleRule *rule = &stylesheet->rules[i];
            const StyleDeclaration *declaration =
                stylesheet_rule_declaration(stylesheet, rule);
            if (declaration == NULL) continue;
            if (strstr(rule->selector, "wm-fallback-layout") != NULL
                || strstr(rule->selector, "wm-composer-textarea") != NULL) {
                fprintf(stderr, "layout-rule selector=%s mask=%llu origin=%u pseudo=%d out=%d relative=%d sticky=%d fixed=%d\n",
                        rule->selector,
                        (unsigned long long) declaration->mask,
                        rule->origin, rule->pseudo,
                        declaration->values.out_of_flow,
                        declaration->values.relative_position,
                        declaration->values.sticky_position,
                        declaration->values.fixed_position);
            }
        }
    }
    uint64_t phase_started_us = layout_performance_now_us();
    ComputedStyle root = layout_initial_root_style();
    lxb_dom_node_t *html = body->parent;
    bool have_html = html != NULL
                     && html->type == LXB_DOM_NODE_TYPE_ELEMENT
                     && layout_node_name_is(html, "html");
    ComputedStyle html_style = have_html
        ? layout_style_for_node(context, html, &root) : root;
    resolve_padding(context->sheet, &html_style, viewport_width);
    int html_height = style_content_height(
        context->sheet, &html_style, viewport_width, viewport_height);
    if (!(html_style.has_height
          && (!html_style.height_percent || html_height > 0))) {
        html_height = viewport_height;
    }
    if (html_style.has_background) {
        layout->page_background = blend_color_over(
            html_style.background, html_style.background_alpha,
            layout->page_background);
    }
    ComputedStyle body_style = layout_style_for_node(
        context, body, &html_style);
    if (body_style.has_background) {
        layout->page_background = blend_color_over(
            body_style.background, body_style.background_alpha,
            layout->page_background);
    }
    int bottom = 0;
    PositionedBox initial_positioned_box = {
        .node = NULL,
        .x = 0, .y = 0, .width = viewport_width, .height = html_height
    };
    layout->performance.root_style_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    if (!layout_block(context, body, &html_style, 0, 0, viewport_width,
                      html_height, false, &initial_positioned_box, &bottom)
        || context->cancelled) {
        layout_release_context(context, budget);
        layout_destroy(layout);
        return false;
    }
    /* A definite root-body height fixes its border box, not the document's
       scrollable overflow area. Visible body overflow still extends the root
       scroller; an actual body scroll/clip container keeps that overflow
       local instead. */
    int document_bottom = bottom;
    const LayoutNodeBox *body_box = layout_box_for_node(layout, body);
    if (LAYOUT_TRACE(layout, LAYOUT)) {
        fprintf(stderr,
                "layout-root bottom=%d html-height=%d "
                "html-overflow=%d/%d html-clip-only=%d/%d "
                "body-overflow=%d/%d body-clip-only=%d/%d "
                "body-box=%d,%d,%dx%d client=%dx%d content=%dx%d "
                "clips=%d/%d\n",
                bottom, html_height,
                html_style.overflow_x_scroll, html_style.overflow_y_scroll,
                html_style.overflow_x_clip_only,
                html_style.overflow_y_clip_only,
                body_style.overflow_x_scroll, body_style.overflow_y_scroll,
                body_style.overflow_x_clip_only,
                body_style.overflow_y_clip_only,
                body_box == NULL ? 0 : body_box->x,
                body_box == NULL ? 0 : body_box->y,
                body_box == NULL ? 0 : body_box->width,
                body_box == NULL ? 0 : body_box->height,
                body_box == NULL ? 0 : body_box->client_width,
                body_box == NULL ? 0 : body_box->client_height,
                body_box == NULL ? 0 : body_box->content_width,
                body_box == NULL ? 0 : body_box->content_height,
                body_box != NULL && body_box->clips_x,
                body_box != NULL && body_box->clips_y);
    }
    if (body_box != NULL && !body_box->clips_y) {
        int overflow_bottom = layout_add_coordinate(
            layout_add_coordinate(body_box->y, body_box->content_height),
            body_style.margin.bottom);
        if (overflow_bottom > document_bottom) {
            document_bottom = overflow_bottom;
        }
    }
    if (body_box != NULL && !body_box->clips_x) {
        int overflow_right = layout_add_coordinate(
            layout_add_coordinate(body_box->x, body_box->content_width),
            body_style.margin.right);
        if (overflow_right > layout->scroll_width) {
            layout->scroll_width = overflow_right;
        }
    }
    layout->height = document_bottom < viewport_height
                     ? viewport_height
                     : layout_clamp_coordinate(document_bottom);
    layout->performance.flow_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    compact_layout_storage(layout);
    uint64_t text_fingerprint = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_TEXT || command->text == NULL
            || draw_command_is_text_shadow(command)) continue;
        uint64_t structure[] = {
            i, command->text_length,
            (uint32_t) command->radius
                & (LAYOUT_TEXT_FIND_SPACE_BEFORE
                   | LAYOUT_TEXT_FIND_BLOCK_START)
        };
        for (size_t field = 0;
             field < sizeof(structure) / sizeof(structure[0]); field++) {
            for (size_t byte = 0; byte < sizeof(structure[field]); byte++) {
                text_fingerprint ^= (unsigned char) (
                    structure[field] >> (byte * 8u));
                text_fingerprint *= UINT64_C(1099511628211);
            }
        }
        for (size_t byte = 0; byte < command->text_length; byte++) {
            text_fingerprint ^= (unsigned char) command->text[byte];
            text_fingerprint *= UINT64_C(1099511628211);
        }
    }
    layout->text_fingerprint = text_fingerprint;
    layout->performance.compact_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    if (!layout_resolve_visibility(context)) {
        if (LAYOUT_TRACE(layout, LAYOUT)) {
            fprintf(stderr, "layout-failure phase=visibility\n");
        }
        layout_release_context(context, budget);
        layout_destroy(layout);
        return false;
    }
    layout_build_focus_index(layout);
    layout->performance.focus_index_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    if (!build_paint_order(layout, context)) {
        if (LAYOUT_TRACE(layout, LAYOUT)) {
            fprintf(stderr, "layout-failure phase=paint-order\n");
        }
        layout_release_context(context, budget);
        layout_destroy(layout);
        return false;
    }
    layout->performance.paint_order_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    if (!build_spatial_index(layout, context)) {
        if (LAYOUT_TRACE(layout, LAYOUT)) {
            fprintf(stderr, "layout-failure phase=spatial-index\n");
        }
        layout_release_context(context, budget);
        layout_destroy(layout);
        return false;
    }
    layout->performance.spatial_index_us =
        layout_performance_now_us() - phase_started_us;
    phase_started_us = layout_performance_now_us();
    if (!layout_build_scroll_metadata(layout, stylesheet)) {
        if (LAYOUT_TRACE(layout, LAYOUT)) {
            fprintf(stderr, "layout-failure phase=scroll-metadata\n");
        }
        layout_release_context(context, budget);
        layout_destroy(layout);
        return false;
    }
    layout_finish_work_slice(context);
    layout->scroll_width = root_scroll_width_after_clipping(
        layout, viewport_width);
    if (LAYOUT_TRACE(layout, PAINT)) {
        size_t limit = layout->count < 24 ? layout->count : 24;
        for (size_t i = 0; i < limit; i++) {
            const DrawCommand *command = &layout->commands[i];
            fprintf(stderr, "layout-paint index=%zu type=%d box=%d,%d,%dx%d "
                    "opacity=%u z=%d flags=%u\n", i, command->type,
                    command->x, command->y, command->width, command->height,
                    command->opacity_scale, command->z_index,
                    layout->command_flags == NULL ? 0
                    : layout->command_flags[i]);
        }
    }
    if (LAYOUT_TRACE(layout, LAYOUT_PROFILE)) {
        fprintf(stderr,
                "layout-profile styles=%llu style-cache-hits=%llu "
                "style-cache-misses=%llu intrinsic-width-visits=%llu "
                "intrinsic-min-visits=%llu intrinsic-cache-hits=%llu "
                "intrinsic-cache-misses=%llu flex-basis=%llu flex-minimum=%llu "
                "tree-depth=%zu/%u depth-fallbacks=%zu fallback-visits=%zu "
                "block-scratch-pages=%zu block-scratch-bytes=%zu "
                "block-scratch-allocation-failures=%zu\n",
                (unsigned long long) context->style_resolutions,
                (unsigned long long) context->style_cache_hits,
                (unsigned long long) context->style_cache_misses,
                (unsigned long long) context->intrinsic_width_visits,
                (unsigned long long) context->intrinsic_min_visits,
                (unsigned long long) context->intrinsic_cache_hits,
                (unsigned long long) context->intrinsic_cache_misses,
                (unsigned long long) context->flex_basis_resolutions,
                (unsigned long long) context->flex_minimum_resolutions,
                context->max_tree_call_depth,
                (unsigned) LAYOUT_TREE_CALL_DEPTH_LIMIT,
                context->depth_fallback_count,
                context->fallback_visits,
                context->block_scratch_page_count,
                context->block_scratch_page_count
                    * LAYOUT_BLOCK_SCRATCH_PAGE_DEPTH
                    * sizeof(LayoutBlockScratch),
                context->block_scratch_allocation_failures);
    }
    layout->performance.finalize_us =
        layout_performance_now_us() - phase_started_us;
    layout->performance.style_resolutions = context->style_resolutions;
    layout->performance.style_cache_hits = context->style_cache_hits;
    layout->performance.style_cache_misses = context->style_cache_misses;
    layout->performance.style_resolve_us = context->style_resolve_us;
    if (stylesheet != NULL) {
        layout->performance.style_rule_queries =
            stylesheet->rule_index_queries
            - context->style_rule_queries_at_start;
        layout->performance.style_rule_candidates =
            stylesheet->rule_index_candidates
            - context->style_rule_candidates_at_start;
        layout->performance.style_variable_lookups =
            stylesheet->variable_lookup_calls
            - context->style_variable_lookups_at_start;
        layout->performance.style_variable_rule_candidates =
            stylesheet->variable_rule_candidates
            - context->style_variable_rule_candidates_at_start;
        layout->performance.style_variable_cache_hits =
            stylesheet->variable_cache_hits
            - context->style_variable_cache_hits_at_start;
        layout->performance.style_variable_cache_misses =
            stylesheet->variable_cache_misses
            - context->style_variable_cache_misses_at_start;
        layout->performance.style_variable_cache_negative_hits =
            stylesheet->variable_cache_negative_hits
            - context->style_variable_cache_negative_hits_at_start;
        layout->performance.style_variable_cache_evictions =
            stylesheet->variable_cache_evictions
            - context->style_variable_cache_evictions_at_start;
        layout->performance.style_variable_cache_bytes =
            context->style_variable_cache_bytes;
        layout->performance.style_deferred_rule_applications =
            stylesheet->deferred_rule_applications
            - context->style_deferred_rule_applications_at_start;
        layout->performance.style_deferred_rule_us =
            stylesheet->deferred_rule_us
            - context->style_deferred_rule_us_at_start;
    }
    layout->performance.intrinsic_width_visits =
        context->intrinsic_width_visits;
    layout->performance.intrinsic_min_visits = context->intrinsic_min_visits;
    layout->performance.intrinsic_cache_hits = context->intrinsic_cache_hits;
    layout->performance.intrinsic_cache_misses =
        context->intrinsic_cache_misses;
    layout->performance.intrinsic_paired_text_measurements =
        context->intrinsic_paired_text_measurements;
    layout->performance.margin_collapse_visits =
        context->margin_collapse_visits;
    layout->performance.margin_cache_hits = context->margin_cache_hits;
    layout->performance.margin_cache_misses =
        context->margin_cache_misses;
    layout->performance.flat_iterator_passes =
        context->flat_iterator_passes;
    layout->performance.flat_iterator_yields =
        context->flat_iterator_yields;
    layout->performance.flex_iterator_passes =
        context->flex_iterator_passes;
    layout->performance.flex_iterator_yields =
        context->flex_iterator_yields;
    layout->performance.flex_basis_requests =
        context->flex_basis_resolutions;
    layout->performance.flex_minimum_requests =
        context->flex_minimum_resolutions;
    layout->performance.total_us =
        layout_performance_now_us() - build_started_us;
    if (preview_truncated != NULL) {
        *preview_truncated = context->preview_truncated;
    }
    layout_release_context(context, budget);
    return true;
}

static bool layout_collect_container_state(Stylesheet *stylesheet,
                                           Budget *budget,
                                           const LayoutDocument *layout)
{
    if (stylesheet == NULL || budget == NULL || layout == NULL
        || !style_container_layout_state_begin(
               stylesheet, budget, layout->node_box_count)) return false;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if ((i & 31u) == 0
            && !tilefinch_platform_cooperate(
                "layout-container-state", i)) {
            style_container_layout_state_clear(stylesheet);
            return false;
        }
        const LayoutNodeBox *box = &layout->node_boxes[i];
        uint32_t padding = layout->node_container_padding_sums == NULL
            || i >= layout->node_container_padding_capacity
            ? 0 : layout->node_container_padding_sums[i];
        if (!style_container_layout_state_add(
                stylesheet, box->node, box->client_width,
                box->client_height, (int) (padding & UINT16_MAX),
                (int) (padding >> 16))) {
            style_container_layout_state_clear(stylesheet);
            return false;
        }
    }
    style_container_layout_state_finish(stylesheet);
    return true;
}

static bool layout_span_contains(const char *text, size_t length,
                                 const char *needle)
{
    size_t needle_length = strlen(needle);
    if (text == NULL || needle_length == 0 || needle_length > length) {
        return false;
    }
    for (size_t i = 0; i + needle_length <= length; i++) {
        size_t j = 0;
        while (j < needle_length
               && tolower((unsigned char) text[i + j])
                  == tolower((unsigned char) needle[j])) j++;
        if (j == needle_length) return true;
    }
    return false;
}

static bool layout_document_has_inline_container_units(
    const PocDocument *document, const Stylesheet *stylesheet)
{
    if (document == NULL) return false;
    if (stylesheet != NULL && stylesheet->block_inline_style_attributes) {
        return false;
    }
    if (document->inline_container_units_generation
        == document->content_generation) {
        return document->inline_container_units_present;
    }
    PocDocument *mutable_document = (PocDocument *) document;
    lxb_dom_node_t *node = document->html == NULL
        ? NULL : lxb_dom_interface_node(document->html);
    size_t visited = 0;
    while (node != NULL && visited++ < 16384) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            const char *style = stylesheet != NULL
                    && stylesheet->block_inline_style_attributes
                    && !document_style_attribute_cssom_authorized(node)
                ? NULL : document_attribute(node, "style", &length);
            if (style != NULL
                && (layout_span_contains(style, length, "cqw")
                    || layout_span_contains(style, length, "cqh")
                    || layout_span_contains(style, length, "cqi")
                    || layout_span_contains(style, length, "cqb")
                    || layout_span_contains(style, length, "cqmin")
                    || layout_span_contains(style, length, "cqmax"))) {
                mutable_document->inline_container_units_present = true;
                mutable_document->inline_container_units_generation =
                    document->content_generation;
                return true;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node->next == NULL) node = node->parent;
        if (node != NULL) node = node->next;
    }
    mutable_document->inline_container_units_present = false;
    mutable_document->inline_container_units_generation =
        document->content_generation;
    return false;
}

static bool layout_build_context_container_queries(
    LayoutDocument *layout, Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse, int preview_y_limit, bool *preview_truncated)
{
    if (preview_y_limit == 0 && preview_truncated == NULL) {
        /* The continuation object buys responsiveness only once the page is
           large enough to span meaningful work slices.  Keep tiny pages on
           the direct path: one fewer allocation and no phase-dispatch tax. */
        if (document != NULL && document->node_count < 256
            && !stylesheet_has_container_queries(stylesheet)
            && !layout_document_has_inline_container_units(
                   document, stylesheet)) {
            return layout_build_context_internal(
                layout, budget, document, stylesheet, fonts, images,
                viewport, reuse, 0, NULL, false);
        }
        return layout_build_context_resumable_sync(
            layout, budget, document, stylesheet, fonts, images,
            viewport, reuse);
    }
    if (!stylesheet_has_container_queries(stylesheet)
        && !layout_document_has_inline_container_units(
               document, stylesheet)) {
        return layout_build_context_internal(
            layout, budget, document, stylesheet, fonts, images, viewport,
            reuse, preview_y_limit, preview_truncated, false);
    }

    Stylesheet *mutable_sheet = (Stylesheet *) stylesheet;
    uint64_t previous_signature =
        style_container_layout_state_signature(stylesheet);
    LayoutDocument probe = {0};
    bool probe_truncated = false;
    if (!layout_build_context_internal(
            &probe, budget, document, stylesheet, fonts, images, viewport,
            reuse, preview_y_limit,
            preview_truncated == NULL ? NULL : &probe_truncated, true)) {
        return false;
    }
    uint64_t probe_us = probe.performance.total_us;
    if (!layout_collect_container_state(mutable_sheet, budget, &probe)) {
        layout_destroy(&probe);
        return false;
    }
    uint64_t measured_signature =
        style_container_layout_state_signature(stylesheet);
    if (measured_signature == previous_signature) {
        *layout = probe;
        if (preview_truncated != NULL) *preview_truncated = probe_truncated;
        return true;
    }

    /* The probe is never presented. Size containment makes the measured
       container geometry independent of descendant query results, so one
       authoritative continuation pass is sufficient and bounded. */
    layout_destroy(&probe);
    layout_reuse_cache_reset(reuse);
    bool built = layout_build_context_internal(
        layout, budget, document, stylesheet, fonts, images, viewport,
        reuse, preview_y_limit, preview_truncated, false);
    if (built) layout->performance.total_us += probe_us;
    return built;
}

bool layout_build_context_reuse(LayoutDocument *layout, Budget *budget,
                                const PocDocument *document,
                                const Stylesheet *stylesheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                const ViewportContext *viewport,
                                LayoutReuseCache *reuse)
{
    return layout_build_context_container_queries(
        layout, budget, document, stylesheet, fonts, images, viewport, reuse,
        0, NULL);
}

bool layout_build_context_preview(LayoutDocument *layout, Budget *budget,
                                  const PocDocument *document,
                                  const Stylesheet *stylesheet,
                                  const FontSet *fonts,
                                  const ImageResources *images,
                                  const ViewportContext *viewport,
                                  LayoutReuseCache *reuse, int y_limit,
                                  bool *truncated)
{
    if (truncated == NULL || y_limit <= 0) return false;
    *truncated = false;
    return layout_build_context_container_queries(
        layout, budget, document, stylesheet, fonts, images, viewport, reuse,
        y_limit, truncated);
}

bool layout_build_context(LayoutDocument *layout, Budget *budget,
                          const PocDocument *document,
                          const Stylesheet *stylesheet,
                          const FontSet *fonts,
                          const ImageResources *images,
                          const ViewportContext *viewport)
{
    return layout_build_context_reuse(
        layout, budget, document, stylesheet, fonts, images, viewport, NULL);
}

bool layout_build_viewport(LayoutDocument *layout, Budget *budget,
                           const PocDocument *document,
                           const Stylesheet *stylesheet,
                           const FontSet *fonts,
                           const ImageResources *images,
                           int viewport_width, int viewport_height,
                           int device_width, int device_height)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, viewport_width, viewport_height,
                               device_width, device_height)) return false;
    return layout_build_context(layout, budget, document, stylesheet, fonts,
                                images, &viewport);
}

bool layout_build(LayoutDocument *layout, Budget *budget,
                  const PocDocument *document, const Stylesheet *stylesheet,
                  const FontSet *fonts, const ImageResources *images,
                  int viewport_width)
{
    return layout_build_viewport(layout, budget, document, stylesheet, fonts,
                                 images, viewport_width, 272,
                                 viewport_width, 272);
}

int layout_css_to_visual(const LayoutDocument *layout, int value)
{
    return layout == NULL ? value
                          : viewport_css_to_device(&layout->viewport, value);
}

int layout_visual_to_css(const LayoutDocument *layout, int value)
{
    return layout == NULL ? value
                          : viewport_device_to_css(&layout->viewport, value);
}

static void *clone_array(Budget *budget, const void *source, size_t count,
                         size_t item_size)
{
    if (count == 0) return NULL;
    if (count > SIZE_MAX / item_size) return NULL;
    void *copy = budget_malloc(budget, count * item_size);
    if (copy != NULL) memcpy(copy, source, count * item_size);
    return copy;
}

void layout_note_unresolved_external_visual(
    LayoutContext *context, lxb_dom_node_t *node, const char *source,
    uint8_t kind, PseudoElement pseudo)
{
    if (context == NULL || context->layout == NULL || node == NULL) return;
    LayoutDocument *layout = context->layout;
    layout->unresolved_external_visuals = true;
    if (context->preview_y_limit <= 0) return;
    for (size_t i = 0; i < layout->visual_priority_count; i++) {
        const ImagePriorityTarget *target =
            &layout->visual_priority_targets[i];
        if (target->node == node && target->source == source
            && target->kind == kind
            && target->pseudo == (uint8_t) pseudo) return;
    }
    if (layout->visual_priority_count == LAYOUT_VISUAL_PRIORITY_LIMIT) {
        layout->visual_priority_overflow = true;
        return;
    }
    if (layout->visual_priority_count == layout->visual_priority_capacity) {
        size_t capacity = layout->visual_priority_capacity == 0
            ? 8u : layout->visual_priority_capacity * 2u;
        if (capacity > LAYOUT_VISUAL_PRIORITY_LIMIT) {
            capacity = LAYOUT_VISUAL_PRIORITY_LIMIT;
        }
        ImagePriorityTarget *targets = budget_realloc(
            layout->budget, layout->visual_priority_targets,
            capacity * sizeof(*targets));
        if (targets == NULL) {
            layout->visual_priority_overflow = true;
            return;
        }
        layout->visual_priority_targets = targets;
        layout->visual_priority_capacity = capacity;
    }
    layout->visual_priority_targets[layout->visual_priority_count++] =
        (ImagePriorityTarget) {
            .node = node,
            .source = source,
            .kind = kind,
            .pseudo = (uint8_t) pseudo
        };
}

bool layout_clone_visual(LayoutDocument *visual,
                         const LayoutDocument *source)
{
    if (visual == NULL || source == NULL || source->budget == NULL
        || source->viewport.scale_numerator <= 0
        || source->viewport.scale_denominator <= 0) {
        return false;
    }
    memset(visual, 0, sizeof(*visual));
    visual->budget = source->budget;
    visual->fonts = source->fonts;
    visual->web_fonts = source->web_fonts;
    visual->page_background = source->page_background;
    visual->unresolved_external_visuals =
        source->unresolved_external_visuals;
    /* The gradient table is referenced by fill commands, so it must travel
       with the visual clone that the rasterizer consumes. */
    memcpy(visual->gradients, source->gradients, sizeof(visual->gradients));
    visual->gradient_count = source->gradient_count;
    visual->count = visual->capacity = source->count;
    visual->link_count = visual->link_capacity = source->link_count;
    visual->control_count = visual->control_capacity = source->control_count;
    visual->sticky_count = visual->sticky_capacity = source->sticky_count;
    visual->fixed_count = visual->fixed_capacity = source->fixed_count;
    visual->node_box_count = visual->node_box_capacity = source->node_box_count;
    visual->paint_order_count = source->paint_order_count;
    visual->commands = clone_array(source->budget, source->commands,
                                   source->count, sizeof(*source->commands));
    visual->links = clone_array(source->budget, source->links,
                                source->link_count, sizeof(*source->links));
    visual->controls = clone_array(source->budget, source->controls,
                                   source->control_count,
                                   sizeof(*source->controls));
    visual->sticky_ranges = clone_array(
        source->budget, source->sticky_ranges, source->sticky_count,
        sizeof(*source->sticky_ranges));
    visual->fixed_ranges = clone_array(
        source->budget, source->fixed_ranges, source->fixed_count,
        sizeof(*source->fixed_ranges));
    visual->node_boxes = clone_array(
        source->budget, source->node_boxes, source->node_box_count,
        sizeof(*source->node_boxes));
    if (source->node_clip_radius_codes != NULL) {
        visual->node_clip_radius_codes = clone_array(
            source->budget, source->node_clip_radius_codes,
            source->node_box_count, sizeof(*source->node_clip_radius_codes));
    }
    /* Viewport scaling changes geometry, never CSS stacking order.  Preserve
       the authoritative order built while transient stacking-context
       metadata was available instead of trying to reconstruct it from the
       flattened display list. */
    visual->paint_order = clone_array(
        source->budget, source->paint_order, source->paint_order_count,
        sizeof(*source->paint_order));
    if ((source->count != 0 && visual->commands == NULL)
        || (source->link_count != 0 && visual->links == NULL)
        || (source->control_count != 0 && visual->controls == NULL)
        || (source->sticky_count != 0 && visual->sticky_ranges == NULL)
        || (source->fixed_count != 0 && visual->fixed_ranges == NULL)
        || (source->node_box_count != 0 && visual->node_boxes == NULL)
        || (source->node_clip_radius_codes != NULL
            && visual->node_clip_radius_codes == NULL)
        || (source->paint_order_count != 0
            && visual->paint_order == NULL)) {
        layout_destroy(visual);
        return false;
    }
    int numerator = source->viewport.scale_numerator;
    int denominator = source->viewport.scale_denominator;
    visual->width = viewport_scale_ceil(source->width, numerator, denominator);
    visual->scroll_width = viewport_scale_ceil(source->scroll_width, numerator,
                                                denominator);
    visual->height = viewport_scale_ceil(source->height, numerator, denominator);
    if (!viewport_context_init(&visual->viewport,
                               source->viewport.device_width,
                               source->viewport.device_height,
                               source->viewport.device_width,
                               source->viewport.device_height)) {
        layout_destroy(visual);
        return false;
    }
    visual->viewport.declared = source->viewport.declared;
    visual->viewport.device_width_declared =
        source->viewport.device_width_declared;
    for (size_t i = 0; i < visual->count; i++) {
        DrawCommand *command = &visual->commands[i];
        int text_x_fixed = command->type == DRAW_TEXT
            ? draw_command_text_x_fixed(command) : 0;
        int text_font_size_fixed = command->type == DRAW_TEXT
            ? draw_command_text_font_size_fixed(command) : 0;
        viewport_css_box_to_device(&source->viewport, &command->x,
                                   &command->width);
        viewport_css_box_to_device(&source->viewport, &command->y,
                                   &command->height);
        if (command->letter_spacing != 0) {
            int spacing = viewport_scale_floor(
                command->letter_spacing, numerator, denominator);
            if (spacing > INT8_MAX) spacing = INT8_MAX;
            if (spacing < INT8_MIN) spacing = INT8_MIN;
            command->letter_spacing = (int8_t) spacing;
        }
        int underline_offset = 0;
        if (command->type == DRAW_TEXT
            && draw_command_text_underline_offset(
                   command, &underline_offset)) {
            draw_command_set_text_underline_offset(
                command, viewport_scale_floor(
                    underline_offset, numerator, denominator));
        }
        if (command->type == DRAW_TEXT) {
            text_x_fixed = layout_fixed_scale_floor(
                text_x_fixed, numerator, denominator);
            draw_command_set_text_x_fixed(command, text_x_fixed);
            if (text_font_size_fixed > 0) {
                int64_t scaled = (int64_t) text_font_size_fixed * numerator;
                scaled = (scaled + denominator - 1) / denominator;
                if (scaled < 1) scaled = 1;
                if (scaled > INT_MAX) scaled = INT_MAX;
                draw_command_set_text_font_size_fixed(command, (int) scaled);
            }
            if (draw_command_is_text_shadow(command)) {
                draw_command_set_text_shadow_blur(
                    command, viewport_scale_ceil(
                        draw_command_text_shadow_blur(command),
                        numerator, denominator));
            }
        } else if (command->type == DRAW_IMAGE
                   && command->image_fit >= LAYOUT_IMAGE_FIT_SPRITE
                   && command->image_fit <= LAYOUT_IMAGE_FIT_SPRITE_TILE_XY) {
            /* A sprite DRAW_IMAGE overloads radius with the background's
               source-pixel (CSS) offset, which the rasterizer subtracts
               after converting device deltas back to CSS.  Scaling it here
               would apply the viewport ratio twice, sliding a downscaled
               crop with a large negative offset off its art. */
        } else {
            command->radius = layout_scale_radius_code(
                command->radius, numerator, denominator);
        }
        if (command->type == DRAW_IMAGE
            && style_border_radius_maximum(command->scale) > 0) {
            command->scale = layout_scale_radius_code(
                command->scale, numerator, denominator);
        }
        if ((command->type == DRAW_STROKE_RECT
             || command->type == DRAW_SHADOW_RECT)
            && command->scale > 0) {
            command->scale = viewport_scale_ceil(
                command->scale, numerator, denominator);
            if (command->scale < 1) command->scale = 1;
        }
    }
    for (size_t i = 0; i < visual->link_count; i++) {
        viewport_css_box_to_device(&source->viewport, &visual->links[i].x,
                                   &visual->links[i].width);
        viewport_css_box_to_device(&source->viewport, &visual->links[i].y,
                                   &visual->links[i].height);
    }
    for (size_t i = 0; i < visual->control_count; i++) {
        viewport_css_box_to_device(&source->viewport, &visual->controls[i].x,
                                   &visual->controls[i].width);
        viewport_css_box_to_device(&source->viewport, &visual->controls[i].y,
                                   &visual->controls[i].height);
    }
    for (size_t i = 0; i < visual->sticky_count; i++) {
        visual->sticky_ranges[i].origin_y = viewport_scale_floor(
            visual->sticky_ranges[i].origin_y, numerator, denominator);
        visual->sticky_ranges[i].top = viewport_scale_floor(
            visual->sticky_ranges[i].top, numerator, denominator);
    }
    for (size_t i = 0; i < visual->fixed_count; i++) {
        FixedRange *range = &visual->fixed_ranges[i];
        range->origin_y = viewport_scale_floor(range->origin_y, numerator,
                                               denominator);
        range->height = viewport_scale_ceil(range->height, numerator,
                                            denominator);
        range->inset = viewport_scale_floor(range->inset, numerator,
                                            denominator);
    }
    for (size_t i = 0; i < visual->node_box_count; i++) {
        LayoutNodeBox *box = &visual->node_boxes[i];
        int clip_radius_code = layout_node_box_clip_radius_code(visual, box);
        viewport_css_box_to_device(&source->viewport, &box->x, &box->width);
        viewport_css_box_to_device(&source->viewport, &box->y, &box->height);
        box->client_width = viewport_scale_ceil(box->client_width, numerator,
                                                denominator);
        box->client_height = viewport_scale_ceil(box->client_height, numerator,
                                                 denominator);
        box->content_width = viewport_scale_ceil(box->content_width, numerator,
                                                 denominator);
        box->content_height = viewport_scale_ceil(box->content_height, numerator,
                                                  denominator);
        box->scroll_x = viewport_scale_floor(box->scroll_x, numerator,
                                             denominator);
        box->scroll_y = viewport_scale_floor(box->scroll_y, numerator,
                                             denominator);
        clip_radius_code = layout_scale_radius_code(
            clip_radius_code, numerator, denominator);
        unsigned clip_radius = (unsigned)
            style_border_radius_maximum(clip_radius_code);
        unsigned clip_margin = (unsigned) viewport_scale_ceil(
            (int) layout_node_box_clip_margin(box), numerator, denominator);
        layout_node_box_set_clip_geometry(
            box, clip_radius, clip_margin, layout_node_box_clip_box(box));
        if (!layout_set_node_clip_radius_code(
                visual, i, clip_radius_code)) {
            layout_destroy(visual);
            return false;
        }
        layout_node_index_insert(visual, box->node, i);
    }
    layout_build_focus_index(visual);
    /* Parsed documents carry the authoritative stacking order. Small
       synthetic/embedder layouts may intentionally omit it, so retain the
       historical source-order fallback for that case. */
    if ((visual->paint_order_count != visual->count
         && !build_paint_order(visual, NULL))
        || !build_spatial_index(visual, NULL)) {
        layout_destroy(visual);
        return false;
    }
    return true;
}

void layout_destroy(LayoutDocument *layout)
{
    if (layout == NULL) return;
    if (layout->budget != NULL) {
        budget_free(layout->budget, layout->commands);
        budget_free(layout->budget, layout->links);
        budget_free(layout->budget, layout->node_interaction_ranges);
        budget_free(layout->budget, layout->controls);
        budget_free(layout->budget, layout->generated_text_storage);
        budget_free(layout->budget, layout->sticky_ranges);
        budget_free(layout->budget, layout->fixed_ranges);
        budget_free(layout->budget, layout->node_boxes);
        budget_free(layout->budget, layout->scroll_containers);
        budget_free(layout->budget, layout->scroll_snap_candidates);
        budget_free(layout->budget, layout->node_container_padding_sums);
        budget_free(layout->budget, layout->node_clip_radius_codes);
        budget_free(layout->budget, layout->visual_priority_targets);
        budget_free(layout->budget, layout->node_index);
        budget_free(layout->budget, layout->focus_index);
        budget_free(layout->budget, layout->paint_order);
        budget_free(layout->budget, layout->command_flags);
        budget_free(layout->budget, layout->spatial_band_offsets);
        budget_free(layout->budget, layout->spatial_band_orders);
        budget_free(layout->budget, layout->spatial_global_orders);
        budget_free(layout->budget, layout->overflow_orders);
        budget_free(layout->budget, layout->late_positioned_orders);
    }
    memset(layout, 0, sizeof(*layout));
}

const char *layout_retain_generated_text(
    LayoutDocument *layout, const char *text, size_t length)
{
    if (layout == NULL || layout->budget == NULL || text == NULL
        || length == 0 || length + 1 > LAYOUT_GENERATED_TEXT_LIMIT) {
        return NULL;
    }
    /* Repeated bullets and short counter values dominate real lists. Reuse
       their retained bytes before consuming the fixed PSP arena. */
    for (size_t at = 0; at < layout->generated_text_used;) {
        const char *candidate = layout->generated_text_storage + at;
        size_t candidate_length = strlen(candidate);
        if (candidate_length == length
            && memcmp(candidate, text, length) == 0) {
            return candidate;
        }
        at += candidate_length + 1;
    }
    if (layout->generated_text_used
        > LAYOUT_GENERATED_TEXT_LIMIT - length - 1) return NULL;
    if (layout->generated_text_storage == NULL) {
        layout->generated_text_storage = budget_malloc(
            layout->budget, LAYOUT_GENERATED_TEXT_LIMIT);
        if (layout->generated_text_storage == NULL) return NULL;
    }
    char *retained =
        layout->generated_text_storage + layout->generated_text_used;
    memcpy(retained, text, length);
    retained[length] = '\0';
    layout->generated_text_used += length + 1;
    return retained;
}

static void layout_node_box_clip_rect(const LayoutNodeBox *box,
                                      int *left, int *top,
                                      int *right, int *bottom)
{
    StyleOverflowClipBox clip_box = layout_node_box_clip_box(box);
    int margin = (int) layout_node_box_clip_margin(box);
    int width = clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX
        ? box->width
        : (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
           ? box->width - 2 * box->clip_inset_left : box->client_width);
    int height = clip_box == STYLE_OVERFLOW_CLIP_BORDER_BOX
        ? box->height
        : (clip_box == STYLE_OVERFLOW_CLIP_CONTENT_BOX
           ? box->height - 2 * box->clip_inset_top : box->client_height);
    if (width < 0) width = 0;
    if (height < 0) height = 0;
    *left = box->x + box->clip_inset_left - margin;
    *top = box->y + box->clip_inset_top - margin;
    *right = *left + width + 2 * margin;
    *bottom = *top + height + 2 * margin;
}

static bool point_in_rounded_clip(const LayoutDocument *layout,
                                  const LayoutNodeBox *box, int x, int y)
{
    if (box == NULL || !box->clips_x || !box->clips_y
        || layout_node_box_clip_radius(box) == 0) return true;
    int left, top, right, bottom;
    layout_node_box_clip_rect(box, &left, &top, &right, &bottom);
    int radius_code =
        layout_node_box_effective_clip_radius_code(layout, box);
    int width = right - left;
    int height = bottom - top;
    int radii[4];
    style_border_radius_resolve_corners(
        radius_code, width, height, radii);
    if (style_border_radius_maximum(radius_code) <= 0) return true;
    int64_t px = x;
    int64_t py = y;
    int radius = 0;
    int64_t cx = px;
    int64_t cy = py;
    if (px < left + radii[0] && py < top + radii[0]) {
        radius = radii[0];
        cx = left + radius - 1;
        cy = top + radius - 1;
    } else if (px >= right - radii[1] && py < top + radii[1]) {
        radius = radii[1];
        cx = right - radius;
        cy = top + radius - 1;
    } else if (px >= right - radii[2] && py >= bottom - radii[2]) {
        radius = radii[2];
        cx = right - radius;
        cy = bottom - radius;
    } else if (px < left + radii[3] && py >= bottom - radii[3]) {
        radius = radii[3];
        cx = left + radius - 1;
        cy = bottom - radius;
    } else {
        return true;
    }
    if (radius <= 0) return true;
    int64_t dx = px - cx;
    int64_t dy = py - cy;
    uint64_t absolute_x = dx < 0 ? (uint64_t) (-dx) : (uint64_t) dx;
    uint64_t absolute_y = dy < 0 ? (uint64_t) (-dy) : (uint64_t) dy;
    uint64_t limit = (uint64_t) radius;
    if (absolute_x >= limit || absolute_y >= limit) return false;
    uint64_t x_squared = absolute_x * absolute_x;
    uint64_t radius_squared = limit * limit;
    return absolute_y * absolute_y < radius_squared - x_squared;
}

const LinkRegion *layout_link_at(const LayoutDocument *layout, int x, int y)
{
    if (layout == NULL) return NULL;
    const LinkRegion *hit = NULL;
    for (size_t i = 0; i < layout->link_count; i++) {
        const LinkRegion *link = &layout->links[i];
        int translated_x = link->x, translated_y = link->y;
        bool clipped = false;
        for (lxb_dom_node_t *parent = link->node == NULL ? NULL
                                      : link->node->parent;
             parent != NULL; parent = parent->parent) {
            const LayoutNodeBox *box = layout_box_for_node(layout, parent);
            if (box == NULL) continue;
            int clip_left, clip_top, clip_right, clip_bottom;
            layout_node_box_clip_rect(
                box, &clip_left, &clip_top, &clip_right, &clip_bottom);
            bool outside = (box->clips_x
                            && (x < clip_left || x >= clip_right))
                           || (box->clips_y
                               && (y < clip_top || y >= clip_bottom));
            if (outside || !point_in_rounded_clip(layout, box, x, y)) {
                clipped = true;
                break;
            }
            translated_x -= box->scroll_x;
            translated_y -= box->scroll_y;
        }
        if (!clipped && x >= translated_x
            && x < translated_x + link->width && y >= translated_y
            && y < translated_y + link->height
            && (hit == NULL || link->z_index >= hit->z_index)) hit = link;
    }
    return hit;
}

const ControlRegion *layout_control_at(const LayoutDocument *layout,
                                       int x, int y)
{
    if (layout == NULL) return NULL;
    const ControlRegion *hit = NULL;
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        int translated_x = control->x, translated_y = control->y;
        bool clipped = false;
        for (lxb_dom_node_t *parent = control->node == NULL ? NULL
                                      : control->node->parent;
             parent != NULL; parent = parent->parent) {
            const LayoutNodeBox *box = layout_box_for_node(layout, parent);
            if (box == NULL) continue;
            int clip_left, clip_top, clip_right, clip_bottom;
            layout_node_box_clip_rect(
                box, &clip_left, &clip_top, &clip_right, &clip_bottom);
            bool outside = (box->clips_x
                            && (x < clip_left || x >= clip_right))
                           || (box->clips_y
                               && (y < clip_top || y >= clip_bottom));
            if (outside || !point_in_rounded_clip(layout, box, x, y)) {
                clipped = true;
                break;
            }
            translated_x -= box->scroll_x;
            translated_y -= box->scroll_y;
        }
        if (!clipped && x >= translated_x
            && x < translated_x + control->width && y >= translated_y
            && y < translated_y + control->height
            && (hit == NULL || control->z_index >= hit->z_index)) hit = control;
    }
    return hit;
}

const LayoutNodeBox *layout_box_for_node(const LayoutDocument *layout,
                                         const lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return NULL;
    size_t index = layout_node_box_index(layout, node);
    return index == SIZE_MAX ? NULL : &layout->node_boxes[index];
}

LayoutNodeBox *layout_box_for_node_mutable(
    LayoutDocument *layout, const lxb_dom_node_t *node)
{
    if (layout == NULL || node == NULL) return NULL;
    size_t index = layout_node_box_index(layout, node);
    return index == SIZE_MAX ? NULL : &layout->node_boxes[index];
}

bool layout_focus_for_node(const LayoutDocument *layout,
                           const lxb_dom_node_t *node,
                           bool *control, size_t *index)
{
    if (layout == NULL || node == NULL || layout->focus_index == NULL
        || layout->focus_index_capacity == 0) return false;
    size_t mask = layout->focus_index_capacity - 1u;
    size_t slot = layout_pointer_hash(node) & mask;
    for (size_t probes = 0; probes < layout->focus_index_capacity;
         probes++, slot = (slot + 1u) & mask) {
        const LayoutFocusIndexEntry *entry = &layout->focus_index[slot];
        if (entry->node == NULL) return false;
        if (entry->node != node) continue;
        if (entry->link_plus_one != 0) {
            if (control != NULL) *control = false;
            if (index != NULL) *index = entry->link_plus_one - 1u;
            return true;
        }
        if (entry->control_plus_one != 0) {
            if (control != NULL) *control = true;
            if (index != NULL) *index = entry->control_plus_one - 1u;
            return true;
        }
        return false;
    }
    return false;
}

bool layout_apply_focus_border_paint(
    LayoutDocument *layout, const Stylesheet *stylesheet,
    lxb_dom_node_t *node, const ComputedStyle *style, bool dry_run,
    int *left, int *top, int *right, int *bottom)
{
    int border_radius_code = stylesheet_border_radius_code(
        stylesheet, style);
    if (layout == NULL || stylesheet == NULL || node == NULL || style == NULL
        || style_border_radius_maximum(border_radius_code) <= 0
        || style->border.top <= 0
        || style->border.top != style->border.right
        || style->border.top != style->border.bottom
        || style->border.top != style->border.left) return false;
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    if (box == NULL || box->width <= 0 || box->height <= 0
        || box->command_start > box->command_end
        || box->command_end > layout->count) return false;

    uint8_t alpha = 0;
    uint32_t color = stylesheet_border_color(
        stylesheet, style, STYLE_BORDER_TOP, &alpha);
    for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
        uint8_t side_alpha = 0;
        uint32_t side_color = stylesheet_border_color(
            stylesheet, style, (StyleBorderSide) side, &side_alpha);
        if (side_color != color || side_alpha != alpha
            || computed_style_border_line(
                   style, (StyleBorderSide) side) != STYLE_BORDER_SOLID) {
            return false;
        }
    }

    size_t match = SIZE_MAX;
    for (size_t i = box->command_start; i < box->command_end; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_STROKE_RECT
            || command->x != box->x || command->y != box->y
            || command->width != box->width || command->height != box->height
            || command->scale != style->border.top
            || command->radius != border_radius_code
            || command->image_fit != LAYOUT_STROKE_SOLID) continue;
        if (match != SIZE_MAX) return false;
        match = i;
    }
    if (match == SIZE_MAX) return false;
    if (!dry_run) {
        layout->commands[match].color = color;
        layout->commands[match].opacity_scale = alpha_opacity_scale(alpha);
    }
    if (left != NULL) *left = box->x - style->border.top;
    if (top != NULL) *top = box->y - style->border.top;
    if (right != NULL) *right = box->x + box->width + style->border.right;
    if (bottom != NULL) *bottom = box->y + box->height + style->border.bottom;
    return true;
}

bool layout_scroll_node(LayoutDocument *layout, lxb_dom_node_t *node,
                        int scroll_x, int scroll_y)
{
    if (layout == NULL || node == NULL) return false;
    size_t index = layout_node_box_index(layout, node);
    if (index != SIZE_MAX) {
        LayoutNodeBox *box = &layout->node_boxes[index];
        int maximum_x = box->content_width - box->client_width;
        int maximum_y = box->content_height - box->client_height;
        if (!box->clips_x || box->clip_only_x || maximum_x < 0) maximum_x = 0;
        if (!box->clips_y || box->clip_only_y || maximum_y < 0) maximum_y = 0;
        if (scroll_x < 0) scroll_x = 0;
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_x > maximum_x) scroll_x = maximum_x;
        if (scroll_y > maximum_y) scroll_y = maximum_y;
        box->scroll_x = scroll_x;
        box->scroll_y = scroll_y;
        return true;
    }
    return false;
}

void layout_transfer_scroll_state(const LayoutDocument *previous,
                                  LayoutDocument *replacement)
{
    if (previous == NULL || replacement == NULL) return;
    for (size_t i = 0; i < previous->node_box_count; i++) {
        const LayoutNodeBox *box = &previous->node_boxes[i];
        if (box->node == NULL || (box->scroll_x == 0 && box->scroll_y == 0)) {
            continue;
        }
        /* Reapply through the public clamp so an axis that changed to
           overflow:clip resets to zero while the still-scrollable axis
           preserves its visual position. */
        (void) layout_scroll_node(
            replacement, box->node, box->scroll_x, box->scroll_y);
    }
}
