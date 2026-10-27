/* The page layout reuse cache: retained computed styles and the DOM
   mutation, selector-fact, :has() reach, focus-scope and structural-identity
   invalidation that keeps them valid between relayouts. Its public API is
   the layout_reuse_cache_* family in tilefinch/layout.h; layout.c owns the
   layout job that consumes it. */
#include "tilefinch/layout.h"

#include "layout_internal.h"
#include "style_cache_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void layout_reuse_clear_counters(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    if (cache->counter_entries != NULL) {
        cache->stats.retained_bytes -=
            cache->counter_capacity * sizeof(*cache->counter_entries);
        budget_free(cache->budget, cache->counter_entries);
    }
    cache->counter_entries = NULL;
    cache->counter_root = NULL;
    cache->counter_count = cache->counter_capacity = 0;
    cache->counter_bounded_out = false;
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

static LayoutReuseStylePayload *layout_reuse_payload(
    const LayoutReuseCache *cache, uint16_t index)
{
    return &cache->style_pages[index / LAYOUT_REUSE_STYLE_PAGE_SIZE]
                             [index % LAYOUT_REUSE_STYLE_PAGE_SIZE];
}

static void layout_reuse_drop_style(LayoutReuseCache *cache,
                                     LayoutReuseStyleEntry *entry)
{
    if (entry->node != NULL) {
        layout_reuse_payload(cache, entry->payload)->next_free =
            cache->style_payload_free;
        cache->style_payload_free = (uint16_t) (entry->payload + 1u);
    }
    memset(entry, 0, sizeof(*entry));
}

static bool layout_reuse_admit_payload(LayoutReuseCache *cache,
                                       uint16_t *index)
{
    if (cache->style_pressure_trimmed) return false;
    if (cache->style_payload_free != 0) {
        *index = (uint16_t) (cache->style_payload_free - 1u);
        cache->style_payload_free =
            layout_reuse_payload(cache, *index)->next_free;
        return true;
    }
    if (cache->style_payload_count == LAYOUT_REUSE_STYLE_CAPACITY_LIMIT)
        return false;
    *index = cache->style_payload_count;
    size_t page = *index / LAYOUT_REUSE_STYLE_PAGE_SIZE;
    if (cache->style_pages[page] == NULL) {
        if (cache->style_payload_refused) return false;
        /* Optional style payloads share the eviction policy's workspace
           reserve. A build can start roomy and consume that reserve midway;
           don't let caching its answers cause the required flow to refuse. */
        if (budget_pressure_required(cache->budget,
                LAYOUT_REUSE_STYLE_PAGE_SIZE * sizeof(LayoutReuseStylePayload),
                LAYOUT_REUSE_STYLE_WORKING_BYTES
                    + LAYOUT_REUSE_STYLE_RESERVE_BYTES)) return false;
        cache->style_pages[page] = budget_malloc(cache->budget,
            LAYOUT_REUSE_STYLE_PAGE_SIZE * sizeof(LayoutReuseStylePayload));
        if (cache->style_pages[page] == NULL) {
            cache->style_payload_refused = true;
            return false;
        }
        cache->stats.retained_bytes +=
            LAYOUT_REUSE_STYLE_PAGE_SIZE * sizeof(LayoutReuseStylePayload);
    }
    cache->style_payload_count++;
    return true;
}

static void layout_reuse_clear_styles(LayoutReuseCache *cache)
{
    memset(cache->styles, 0, (cache->style_mask + 1u) * sizeof(*cache->styles));
    cache->style_payload_count = 0;
    cache->style_payload_free = 0;
    cache->style_payload_refused = false;
}

static void layout_reuse_clear_entries(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    layout_reuse_clear_counters(cache);
    layout_reuse_clear_styles(cache);
    style_retained_matches_clear(cache->matches);
    cache->pending_active = false;
    cache->pending_overflow = false;
    cache->pending_parent_scope = false;
    cache->pending_node_count = 0;
    cache->pending_token_count = 0;
    memset(&cache->has_pending, 0, sizeof(cache->has_pending));
    cache->font_publication_active = false;
    memset(cache->intrinsic, 0, sizeof(cache->intrinsic));
    if (cache->table_rows != NULL) {
        memset(cache->table_rows, 0,
               sizeof(*cache->table_rows)
                   * LAYOUT_REUSE_TABLE_ROW_CAPACITY);
    }
    cache->clock = 0;
    cache->style_build_clock = 0;
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

void layout_reuse_container_state_changed(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    layout_reuse_clear_counters(cache);
    layout_reuse_clear_sizing_entries(cache);
    for (size_t i = 0; i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if ((entry->dependent & LAYOUT_REUSE_CONTAINER_DEPENDENT) != 0)
            layout_reuse_drop_style(cache, entry);
        else
            /* A pseudo alone can have a container condition even when the
               element's ordinary style does not consult one. */
            entry->pseudo_absent = 0;
    }
}

LayoutReuseCache *layout_reuse_cache_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    LayoutReuseCache *cache = budget_calloc(budget, 1, sizeof(*cache));
    if (cache == NULL) return NULL;
    cache->styles = budget_calloc(budget, LAYOUT_REUSE_STYLE_CAPACITY,
                                  sizeof(*cache->styles));
    if (cache->styles == NULL) {
        budget_free(budget, cache);
        return NULL;
    }
    cache->budget = budget;
    cache->style_mask = LAYOUT_REUSE_STYLE_CAPACITY - 1u;
    cache->structure_entries = UINT64_MAX;
    cache->stats.retained_bytes = sizeof(*cache)
        + LAYOUT_REUSE_STYLE_CAPACITY * sizeof(*cache->styles);
    return cache;
}

/* The builds since the last fit evicted live entries: a walk of more
   styled elements than the table holds. Each document-order walk then
   evicts what the next one needs first (an LRU over a cycle longer than
   itself keeps nothing), so every relayout of a large page resolved most
   of its styles again. Grow the table to hold the walk with probing room,
   when the budget spares it with a margin (a pressure eviction would
   otherwise discard it again); entries move over. */
static void layout_reuse_fit_styles(LayoutReuseCache *cache)
{
    size_t capacity = cache->style_mask + 1u;
    size_t wanted = capacity + cache->style_shortfall;
    size_t grown = capacity;
    while (grown < LAYOUT_REUSE_STYLE_CAPACITY_LIMIT
           && grown < wanted + wanted / 4u) grown *= 2u;
    if (grown == capacity
        || budget_pressure_required(cache->budget,
                                    grown * sizeof(*cache->styles),
                                    LAYOUT_REUSE_STYLE_GROWTH_RESERVE_BYTES)) return;
    LayoutReuseStyleEntry *styles =
        budget_calloc(cache->budget, grown, sizeof(*styles));
    if (styles == NULL) return;
    LayoutReuseStyleEntry *old = cache->styles;
    cache->styles = styles;
    cache->style_mask = grown - 1u;
    for (size_t i = 0; i < capacity; i++) {
        if (old[i].node == NULL) continue;
        size_t home = layout_pointer_hash(old[i].node) & cache->style_mask;
        bool moved = false;
        for (size_t probe = 0; probe < 8; probe++) {
            LayoutReuseStyleEntry *slot =
                &styles[(home + probe) & cache->style_mask];
            if (slot->node == NULL) {
                *slot = old[i];
                moved = true;
                break;
            }
        }
        if (!moved) layout_reuse_drop_style(cache, &old[i]);
    }
    budget_free(cache->budget, old);
    cache->stats.retained_bytes += (grown - capacity) * sizeof(*styles);
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_RELAYOUT_CENSUS") != NULL)
        fprintf(stderr, "layout-reuse-fit capacity=%zu->%zu wanted=%zu\n",
                capacity, grown, wanted);
#endif
}

size_t layout_reuse_cache_reclaim_styles(LayoutReuseCache *cache)
{
    if (cache == NULL) return 0;
    layout_reuse_clear_styles(cache);
    cache->style_pressure_trimmed = true;
    size_t released = 0;
    for (size_t i = 0; i < LAYOUT_REUSE_STYLE_PAGE_COUNT; i++) {
        if (cache->style_pages[i] == NULL) continue;
        budget_free(cache->budget, cache->style_pages[i]);
        cache->style_pages[i] = NULL;
        released += LAYOUT_REUSE_STYLE_PAGE_SIZE
            * sizeof(LayoutReuseStylePayload);
    }
    cache->stats.retained_bytes -= released;
    /* Run measurements are as optional as resolved values, and resume
       with them. */
    released += layout_text_measure_release(cache);
    return released;
}

void layout_reuse_cache_destroy(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    Budget *budget = cache->budget;
    layout_reuse_clear_counters(cache);
    if (cache->table_rows != NULL) {
        budget_free(budget, cache->table_rows);
    }
    style_retained_matches_destroy(cache->matches);
    budget_free(budget, cache->styles);
    for (size_t i = 0; i < LAYOUT_REUSE_STYLE_PAGE_COUNT; i++)
        budget_free(budget, cache->style_pages[i]);
    budget_free(budget, cache->identity_tokens);
    budget_free(budget, cache->identity_names);
    budget_free(budget, cache->structural_rules);
    (void) layout_text_measure_release(cache);
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
    cache->state_rule_count = 0;
    cache->state_rules_bounded = false;
    cache->selector_focus_has_sibling = false;
    cache->selector_has_structure = false;
    cache->selector_has_empty = false;
    cache->empty_key_count = 0;
    cache->empty_any = false;
    memset(&cache->structure, 0, sizeof(cache->structure));
    memset(&cache->trailing, 0, sizeof(cache->trailing));
    memset(&cache->has_arguments, 0, sizeof(cache->has_arguments));
    cache->hidden_facts_ready = false;
    cache->stats.full_resets++;
}

static void layout_reuse_note_selector_dependencies(
    LayoutReuseCache *cache, const char *selector, uint32_t rule_index);
static bool layout_reuse_empty_reaches_siblings(
    const LayoutReuseCache *cache, const lxb_dom_node_t *element);

void layout_reuse_cache_enable_retained_matches(LayoutReuseCache *cache)
{
    if (cache != NULL) cache->matches_enabled = true;
}

void layout_reuse_cache_record_unresolved_visuals(LayoutReuseCache *cache,
                                                  bool record)
{
    if (cache != NULL) cache->record_unresolved_visuals = record;
}

void layout_reuse_cache_note_stylesheet_appended(
    LayoutReuseCache *cache, const Stylesheet *sheet, const uint16_t *remap,
    size_t old_count, const uint32_t *appended, size_t appended_count,
    bool lists_valid)
{
    if (cache == NULL) return;
    if (sheet == NULL || cache->sheet != sheet) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_cache_flush_invalidations(cache);
    /* Values may change everywhere (a new rule can restyle any element the
       lists still cover), so only the exact selector answers are kept. */
    layout_reuse_clear_counters(cache);
    layout_reuse_clear_styles(cache);
    layout_reuse_clear_sizing_entries(cache);
    cache->clock = 0;
    if (cache->matches != NULL) {
        if (!lists_valid || remap == NULL) {
            style_retained_matches_clear(cache->matches);
        } else {
            style_retained_matches_remap(cache->matches, remap, old_count);
            style_retained_matches_invalidate_rules(
                cache->matches, sheet, appended, appended_count);
        }
    }
    /* Rule ordinals move when a style element is inserted before an old
       source. Relational invalidation must follow the same remap as matches
       (the sheet's :has() plan is rebuilt with its has-rule index). */
    for (size_t i = 0; i < cache->state_rule_count; i++) {
        uint32_t old = cache->state_rules[i];
        if (!lists_valid || remap == NULL || old >= old_count
            || remap[old] == UINT16_MAX) {
            cache->state_rules_bounded = true;
            break;
        }
        cache->state_rules[i] = remap[old];
    }
    bool had_has = cache->selector_has_has;
    /* Rescanned in full when next needed. */
    cache->hidden_facts_ready = false;
    memset(&cache->trailing, 0, sizeof(cache->trailing));
    memset(&cache->has_arguments, 0, sizeof(cache->has_arguments));
    for (size_t i = 0; i < appended_count; i++) {
        if (appended[i] < sheet->count) {
            layout_reuse_note_selector_dependencies(
                cache, sheet->rules[appended[i]].selector, appended[i]);
        }
    }
    if (cache->selector_has_has && !had_has && cache->matches != NULL) {
        style_retained_matches_clear(cache->matches);
    }
    cache->sheet_generation = sheet->build_generation;
    cache->stats.stylesheet_appends++;
}

void layout_reuse_cache_begin_font_publication(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    layout_reuse_clear_sizing_entries(cache);
    for (size_t i = 0; i <= cache->style_mask; i++) {
        if ((cache->styles[i].dependent & LAYOUT_REUSE_FONT_DEPENDENT) != 0)
            layout_reuse_drop_style(cache, &cache->styles[i]);
    }
    /* A document-order walk would otherwise evict the previous build's
       retained tail before reaching it. Keep those bounded entries through
       this transaction; ordinary per-build caches still serve new nodes. */
    cache->font_publication_active = true;
}

void layout_reuse_cache_end_font_publication(LayoutReuseCache *cache)
{
    if (cache != NULL) cache->font_publication_active = false;
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

size_t layout_rebind_image_resources(LayoutDocument *layout,
                                     uintptr_t previous_base,
                                     size_t previous_count,
                                     const ImageResources *images)
{
    if (layout == NULL || previous_base == 0 || previous_count == 0
        || images == NULL || (images->count != 0 && images->items == NULL)
        || previous_count > SIZE_MAX / sizeof(*images->items)) return 0;
    size_t rebound = 0;
    size_t previous_bytes = previous_count * sizeof(*images->items);
    for (size_t command_index = 0;
         command_index < layout->count; command_index++) {
        DrawCommand *command = &layout->commands[command_index];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        /* Append-only growth preserves indices. Compare numeric addresses
           only, never dereference the retired table or its old entries. */
        uintptr_t address = (uintptr_t) command->image;
        if (address < previous_base) continue;
        uintptr_t offset = address - previous_base;
        if (offset >= previous_bytes
            || offset % sizeof(*images->items) != 0u) continue;
        size_t image_index = (size_t) (
            offset / sizeof(*images->items));
        command->image = image_index < images->count
            ? &images->items[image_index] : NULL;
        rebound++;
    }
    return rebound;
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

static void layout_reuse_drop_subtree(LayoutReuseCache *cache,
                                      lxb_dom_node_t *scope,
                                      bool include_matches)
{
    for (size_t i = 0; i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node != NULL
            && layout_node_is_within(entry->node, scope)) {
            layout_reuse_drop_style(cache, entry);
        }
    }
    if (include_matches)
        style_retained_matches_invalidate_within(cache->matches, scope);
}

/* One element's own cached styles and lists; its descendants re-key off
   its style if that changes. */
static void layout_reuse_drop_element(LayoutReuseCache *cache,
                                      const lxb_dom_node_t *node,
                                      bool include_matches)
{
    size_t home = layout_pointer_hash((lxb_dom_node_t *) node)
                  & cache->style_mask;
    for (size_t probe = 0; probe < 8; probe++) {
        LayoutReuseStyleEntry *entry = &cache->styles[
            (home + probe) & cache->style_mask];
        if (entry->node == node) layout_reuse_drop_style(cache, entry);
    }
    if (include_matches) style_retained_matches_forget_node(cache->matches, node);
}

#define LAYOUT_REUSE_SHALLOW_CHILD_LIMIT 512u

/* Whether a change to `parent`'s child list can restyle descendants of
   its children (LayoutReuseCache.structure): a current child, or the one
   that left (`removed`, NULL when unknown), carries a key of a sibling
   test that reaches descendants. */
static bool layout_reuse_structure_deep(const LayoutReuseCache *cache,
                                        const lxb_dom_node_t *parent,
                                        const lxb_dom_node_t *removed,
                                        bool removed_unknown)
{
    const StyleStructureKeys *keys = &cache->structure;
    if (!keys->reaches) return false;
    if (keys->any) return true;
    for (size_t i = 0; i < keys->count; i++) {
        if (!keys->predecessor[i]) continue;
        if (removed_unknown
            || style_element_carries_key(removed, keys->keys[i]))
            return true;
    }
    size_t children = 0;
    for (const lxb_dom_node_t *child = parent->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (++children > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) return true;
        for (size_t i = 0; i < keys->count; i++) {
            if (style_element_carries_key(child, keys->keys[i])) return true;
        }
    }
    return false;
}

/* `parent`'s children changed: `changed` is a child inserted or moved in
   (its subtree may hold entries from elsewhere), or a changed text node,
   or NULL; `removed` the child that left (NULL when unknown). Positional
   pseudo-classes and sibling combinators restyle the parent (its :empty)
   and its children; their descendants re-key off their styles, unless a
   sibling test reaches them through a descendant combinator. An :empty
   test outside :has() can reach the parent's siblings. */
static bool layout_structural_add(void *context, StyleKeyNames entry)
{
    LayoutReuseCache *cache = context;
    if (entry.key == 0) {
        cache->structural_unkeyed_names |= entry.names;
        return true;
    }
    if (cache->structural_rule_count == cache->structural_rule_capacity) {
        size_t grown = cache->structural_rule_capacity == 0
            ? 16u : cache->structural_rule_capacity * 2u;
        StyleKeyNames *rules = budget_realloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT, cache->structural_rules,
            grown * sizeof(*rules));
        if (rules == NULL) return false;
        cache->structural_rules = rules;
        cache->structural_rule_capacity = grown;
    }
    cache->structural_rules[cache->structural_rule_count++] = entry;
    return true;
}

static int layout_structural_compare(const void *left, const void *right)
{
    uint32_t a = ((const StyleKeyNames *) left)->key;
    uint32_t b = ((const StyleKeyNames *) right)->key;
    return a < b ? -1 : a > b;
}

/* The custom-property names structural or sibling tests can change on
   `element`. */
static uint64_t layout_structural_names(LayoutReuseCache *cache,
                                        const lxb_dom_node_t *element)
{
    const Stylesheet *sheet = cache->sheet;
    if (sheet == NULL) return UINT64_MAX;
    if (!cache->structural_ready || cache->structural_sheet != sheet
        || cache->structural_generation != sheet->build_generation
        || cache->structural_custom_count != sheet->custom_rule_count) {
        cache->structural_rule_count = 0;
        cache->structural_unkeyed_names = 0;
        cache->structural_unbounded = !stylesheet_structural_custom_rules(
            sheet, layout_structural_add, cache);
        qsort(cache->structural_rules, cache->structural_rule_count,
              sizeof(*cache->structural_rules), layout_structural_compare);
        cache->structural_sheet = sheet;
        cache->structural_generation = sheet->build_generation;
        cache->structural_custom_count = sheet->custom_rule_count;
        cache->structural_ready = true;
    }
    if (cache->structural_unbounded) return UINT64_MAX;
    return cache->structural_unkeyed_names
        | style_element_key_names(element, cache->structural_rules,
                                  cache->structural_rule_count);
}

/* Restyling an element alone relies on its descendants re-keying off its
   computed style, which does not hold the custom properties it passes
   down. When a change restyles `parent` and its children beside the
   changed subtree, the styles under each of them that read a custom
   property a structural or sibling test may now give it must go too. */
static void layout_reuse_drop_structural_readers(LayoutReuseCache *cache,
                                                 lxb_dom_node_t *parent)
{
    lxb_dom_node_t *reached[LAYOUT_REUSE_SHALLOW_CHILD_LIMIT + 1u];
    uint64_t names[LAYOUT_REUSE_SHALLOW_CHILD_LIMIT + 1u];
    size_t count = 0;
    uint64_t any = 0;
    uint64_t own = layout_structural_names(cache, parent);
    if (own != 0) {
        reached[count] = parent;
        names[count++] = own;
        any |= own;
    }
    for (lxb_dom_node_t *child = parent->first_child;
         child != NULL && own != UINT64_MAX; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        uint64_t bits = layout_structural_names(cache, child);
        if (bits == 0) continue;
        if (count == LAYOUT_REUSE_SHALLOW_CHILD_LIMIT + 1u) {
            /* Too many: every reader under the parent. */
            reached[0] = parent;
            names[0] = any | bits;
            count = 1;
            break;
        }
        reached[count] = child;
        names[count++] = bits;
        any |= bits;
    }
    if (any == 0) return;
    for (size_t i = 0; i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node == NULL || (entry->variable_reads & any) == 0)
            continue;
        for (size_t r = 0; r < count; r++) {
            if ((entry->variable_reads & names[r]) != 0
                && layout_node_is_within(entry->node, reached[r])) {
                layout_reuse_drop_style(cache, entry);
                break;
            }
        }
    }
}

static void layout_reuse_invalidate_child_list(
    LayoutReuseCache *cache, lxb_dom_node_t *parent, lxb_dom_node_t *changed,
    const lxb_dom_node_t *removed, bool removed_unknown,
    bool include_matches, bool appended)
{
    /* `appended`: `changed` was inserted after every sibling that existed
       before, and no sibling test can see it from one of those
       (layout_insertion_reaches_siblings): they keep their styles. */
    bool elements = !appended && (changed == NULL
        || changed->type == LXB_DOM_NODE_TYPE_ELEMENT);
    if (parent->parent != NULL
        && layout_reuse_empty_reaches_siblings(cache, parent)) {
        layout_reuse_drop_subtree(cache, parent->parent, include_matches);
    } else if (elements && layout_reuse_structure_deep(
                   cache, parent, removed, removed_unknown)) {
        layout_reuse_drop_subtree(cache, parent, include_matches);
    } else {
        layout_reuse_drop_element(cache, parent, include_matches);
        size_t children = 0;
        for (lxb_dom_node_t *child = elements ? parent->first_child : NULL;
             child != NULL; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++children > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) {
                layout_reuse_drop_subtree(cache, parent, include_matches);
                break;
            }
            layout_reuse_drop_element(cache, child, include_matches);
        }
        layout_reuse_drop_structural_readers(cache, parent);
        if (changed != NULL && changed->type == LXB_DOM_NODE_TYPE_ELEMENT)
            layout_reuse_drop_subtree(cache, changed, include_matches);
    }
    layout_reuse_cache_invalidate_measurements(cache, parent);
}

static void layout_reuse_invalidate_scoped_internal(
    LayoutReuseCache *cache, lxb_dom_node_t *node,
    bool text_or_structure_sensitive, bool include_matches)
{
    if (text_or_structure_sensitive && node->parent != NULL
        && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        /* A text or child change at `node` under its parent. */
        layout_reuse_invalidate_child_list(cache, node->parent, node, NULL,
                                           false, include_matches, false);
        return;
    }
    lxb_dom_node_t *style_scope = node;
    if ((text_or_structure_sensitive || cache->selector_has_structure)
        && node->parent != NULL) style_scope = node->parent;
    if (!text_or_structure_sensitive && style_scope != node
        && style_scope->type == LXB_DOM_NODE_TYPE_ELEMENT
        && !layout_reuse_structure_deep(cache, style_scope, NULL, false)) {
        /* An attribute read by sibling tests restyles the siblings, and
           the node's own subtree. */
        layout_reuse_drop_subtree(cache, node, include_matches);
        size_t children = 0;
        for (lxb_dom_node_t *child = style_scope->first_child;
             child != NULL; child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++children > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) {
                layout_reuse_drop_subtree(cache, style_scope,
                                          include_matches);
                break;
            }
            layout_reuse_drop_element(cache, child, include_matches);
        }
        layout_reuse_drop_element(cache, style_scope, include_matches);
        layout_reuse_drop_structural_readers(cache, style_scope);
    } else {
        layout_reuse_drop_subtree(cache, style_scope, include_matches);
    }
    layout_reuse_cache_invalidate_measurements(cache, node);
}

/* The intrinsic and table-row measurements of `node`'s subtree and of its
   ancestors: every measurement a change inside the subtree can move. */
static void layout_reuse_drop_measurements(LayoutReuseCache *cache,
                                           lxb_dom_node_t *node)
{
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
}

void layout_reuse_cache_invalidate_measurements(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_clear_counters(cache);
    layout_reuse_drop_measurements(cache, node);
    cache->stats.scoped_invalidations++;
}

void layout_reuse_cache_invalidate_node_scoped(
    LayoutReuseCache *cache, lxb_dom_node_t *node,
    bool text_or_structure_sensitive)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_invalidate_scoped_internal(
        cache, node, text_or_structure_sensitive, true);
}

/* One element whose :has()-dependent match may move: its retained lists,
   its computed styles (its descendants miss through their parent's style
   key if its own changes) and the measurements that include it. */
static void layout_reuse_has_drop(void *opaque, lxb_dom_node_t *node)
{
    LayoutReuseCache *cache = opaque;
    style_retained_matches_forget_node(cache->matches, node);
    size_t home = layout_pointer_hash(node)
                  & cache->style_mask;
    for (size_t probe = 0; probe < 8; probe++) {
        LayoutReuseStyleEntry *entry = &cache->styles[
            (home + probe) & cache->style_mask];
        if (entry->node == node) layout_reuse_drop_style(cache, entry);
    }
    layout_reuse_cache_invalidate_measurements(cache, node);
    cache->stats.has_dropped++;
}

/* Scoped :has() invalidation for one change (style_has_note_change); a
   change it cannot bound resets the cache. False after a reset. */
static bool layout_reuse_has_note(LayoutReuseCache *cache,
                                  lxb_dom_node_t *node, bool structure,
                                  const char *attribute,
                                  const uint32_t *tokens, size_t token_count)
{
    if (!cache->selector_has_has) return true;
    StyleHasNoteStats stats = {0};
    if (cache->sheet == NULL
#ifndef TILEFINCH_NO_TRACE
        || getenv("TILEFINCH_DISABLE_HAS_SCOPED_INVALIDATION") != NULL
#endif
        || !(structure
             /* The entries the journal's probe saw the change reach. */
             ? style_has_note_structure(cache->sheet, node,
                                        cache->structure_entries,
                                        cache->structure_serial,
                                        &cache->has_pending,
                                        layout_reuse_has_drop, cache, &stats)
             : style_has_note_change(cache->sheet, node, false, attribute,
                                     tokens, token_count,
                                     &cache->has_pending,
                                     layout_reuse_has_drop, cache,
                                     &stats))) {
        layout_reuse_cache_reset(cache);
        cache->stats.has_fallbacks++;
        return false;
    }
    /* A rule nothing could place wipes the computed styles at the flush. */
    if (stats.wipes != 0) cache->stats.has_fallbacks++;
    else cache->stats.has_scoped++;
    return true;
}


static bool layout_reuse_has_selects(void *opaque,
                                     const lxb_dom_node_t *node)
{
    const LayoutReuseCache *cache = opaque;
    return style_has_pending_selects(cache->sheet, &cache->has_pending, node);
}

/* Ancestors shared by many readers are asked once per flush. */
typedef struct {
    struct {
        const lxb_dom_node_t *node;
        bool selected;
    } slots[128];
} LayoutHasSelectMemo;

static bool layout_reuse_has_selects_memo(LayoutReuseCache *cache,
                                          LayoutHasSelectMemo *memo,
                                          const lxb_dom_node_t *node)
{
    size_t slot = layout_pointer_hash((lxb_dom_node_t *) node) & 127u;
    if (memo->slots[slot].node == node) return memo->slots[slot].selected;
    bool selected = layout_reuse_has_selects(cache, node);
    memo->slots[slot].node = node;
    memo->slots[slot].selected = selected;
    return selected;
}

/* Whether a style reading a custom property a changed :has() or
   :focus-within rule declares can see the change: the rule's subject
   passes its custom properties down only to itself and its descendants,
   so one of the node's ancestors-or-self must be a changed subject, one
   the walk dropped or one selected by key. */
static bool layout_reuse_has_reaches_reader(LayoutReuseCache *cache,
                                            LayoutHasSelectMemo *memo,
                                            const lxb_dom_node_t *node)
{
    const StyleHasPending *pending = &cache->has_pending;
    if (style_has_pending_names_reach(pending, node)) return true;
    for (const lxb_dom_node_t *at = node;
         pending->marked && at != NULL
         && at->type == LXB_DOM_NODE_TYPE_ELEMENT; at = at->parent)
        if (layout_reuse_has_selects_memo(cache, memo, at)) return true;
    return false;
}

/* The keyed half of the journal's :has() changes: one pass over each
   table, however many records marked it. */
static void layout_reuse_has_flush(LayoutReuseCache *cache)
{
    StyleHasPending *pending = &cache->has_pending;
    if (!pending->active) return;
    size_t dropped = 0;
    LayoutHasSelectMemo memo;
    memset(&memo, 0, sizeof(memo));
    for (size_t i = 0; i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node == NULL) continue;
        bool drop = pending->styles_all;
        if (!drop && pending->marked)
            drop = layout_reuse_has_selects_memo(cache, &memo, entry->node);
        if (!drop && (entry->variable_reads & pending->custom_names) != 0)
            drop = layout_reuse_has_reaches_reader(cache, &memo,
                                                   entry->node);
        if (drop) {
            layout_reuse_drop_style(cache, entry);
            dropped++;
        }
    }
    if (pending->unplaced_rule_count != 0) {
        if (pending->unplaced_rules[0] == UINT32_MAX || cache->sheet == NULL) {
            style_retained_matches_clear(cache->matches);
        } else {
            style_retained_matches_invalidate_rules(
                cache->matches, cache->sheet, pending->unplaced_rules,
                pending->unplaced_rule_count);
        }
    }
    if (pending->author_marked) {
        dropped += style_retained_matches_drop_selected(
            cache->matches, layout_reuse_has_selects, cache);
    }
    if (dropped != 0) {
        layout_reuse_clear_counters(cache);
        layout_reuse_clear_sizing_entries(cache);
    }
    cache->stats.has_dropped += dropped;
    cache->stats.has_scans++;
    memset(&cache->has_pending, 0, sizeof(cache->has_pending));
}

typedef struct {
    LayoutReuseCache *cache;
    const StyleIdentityScan *scan;
    bool refused;
} LayoutIdentityCollect;

static void layout_identity_add_token(void *context, uint32_t hash)
{
    LayoutIdentityCollect *collect = context;
    LayoutReuseCache *cache = collect->cache;
    if (collect->refused) return;
    if (cache->identity_token_count == cache->identity_token_capacity) {
        size_t grown = cache->identity_token_capacity == 0
            ? 64u : cache->identity_token_capacity * 2u;
        struct LayoutIdentityDependent *tokens = budget_realloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT, cache->identity_tokens,
            grown * sizeof(*tokens));
        if (tokens == NULL) {
            collect->refused = true;
            return;
        }
        cache->identity_tokens = tokens;
        cache->identity_token_capacity = grown;
    }
    cache->identity_tokens[cache->identity_token_count++] =
        (struct LayoutIdentityDependent) {
            hash, collect->scan->subject_key, collect->scan->sibling};
}

static void layout_identity_add_name(void *context, uint32_t hash)
{
    LayoutIdentityCollect *collect = context;
    LayoutReuseCache *cache = collect->cache;
    if (collect->refused) return;
    if (cache->identity_name_count == cache->identity_name_capacity) {
        size_t grown = cache->identity_name_capacity == 0
            ? 64u : cache->identity_name_capacity * 2u;
        struct LayoutIdentityNames *names = budget_realloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT, cache->identity_names,
            grown * sizeof(*names));
        if (names == NULL) {
            collect->refused = true;
            return;
        }
        cache->identity_names = names;
        cache->identity_name_capacity = grown;
    }
    cache->identity_names[cache->identity_name_count++] =
        (struct LayoutIdentityNames) {hash, collect->scan->names};
}

static int layout_identity_token_compare(const void *left, const void *right)
{
    const struct LayoutIdentityDependent *a = left, *b = right;
    if (a->token != b->token) return a->token < b->token ? -1 : 1;
    if (a->subject_key != b->subject_key)
        return a->subject_key < b->subject_key ? -1 : 1;
    return (int) a->sibling - (int) b->sibling;
}

static int layout_identity_name_compare(const void *left, const void *right)
{
    uint32_t a = ((const struct LayoutIdentityNames *) left)->token;
    uint32_t b = ((const struct LayoutIdentityNames *) right)->token;
    return a < b ? -1 : a > b;
}

/* (Re)builds the identity tables for the bound sheet; false when they
   cannot scope a class change. */
static bool layout_identity_prepare(LayoutReuseCache *cache)
{
    const Stylesheet *sheet = cache->sheet;
    if (sheet == NULL) return false;
    if (cache->identity_sheet == sheet
        && cache->identity_generation == sheet->build_generation
        && cache->identity_rule_count == sheet->count
        && cache->identity_custom_count == sheet->custom_rule_count)
        return !cache->identity_unbounded;
    cache->identity_sheet = sheet;
    cache->identity_generation = sheet->build_generation;
    cache->identity_rule_count = sheet->count;
    cache->identity_custom_count = sheet->custom_rule_count;
    cache->identity_token_count = 0;
    cache->identity_name_count = 0;
    LayoutIdentityCollect collect = {.cache = cache};
    StyleIdentityScan scan = {
        .nonlocal = layout_identity_add_token,
        .declares = layout_identity_add_name,
        .context = &collect
    };
    collect.scan = &scan;
    bool bounded = stylesheet_identity_scan(sheet, &scan);
    cache->identity_unbounded = !bounded || collect.refused;
    cache->identity_attributes_opaque = scan.attributes_opaque;
    if (cache->identity_unbounded) return false;
    qsort(cache->identity_tokens, cache->identity_token_count,
          sizeof(*cache->identity_tokens), layout_identity_token_compare);
    /* Rules repeat (token, key) pairs: keep one of each. */
    size_t kept = 0;
    for (size_t i = 0; i < cache->identity_token_count; i++) {
        if (kept != 0
            && cache->identity_tokens[kept - 1].token
                   == cache->identity_tokens[i].token
            && cache->identity_tokens[kept - 1].subject_key
                   == cache->identity_tokens[i].subject_key) {
            cache->identity_tokens[kept - 1].sibling |=
                cache->identity_tokens[i].sibling;
            continue;
        }
        cache->identity_tokens[kept++] = cache->identity_tokens[i];
    }
    cache->identity_token_count = kept;
    qsort(cache->identity_names, cache->identity_name_count,
          sizeof(*cache->identity_names), layout_identity_name_compare);
    /* One row per token, its rules' names merged. */
    kept = 0;
    for (size_t i = 0; i < cache->identity_name_count; i++) {
        if (kept != 0 && cache->identity_names[kept - 1].token
                             == cache->identity_names[i].token) {
            cache->identity_names[kept - 1].names |=
                cache->identity_names[i].names;
            continue;
        }
        cache->identity_names[kept++] = cache->identity_names[i];
    }
    cache->identity_name_count = kept;
    /* Keep only what the tables hold until the sheet changes. */
    if (cache->identity_token_count != 0
        && cache->identity_token_count < cache->identity_token_capacity) {
        struct LayoutIdentityDependent *tokens = budget_realloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT, cache->identity_tokens,
            cache->identity_token_count * sizeof(*tokens));
        if (tokens != NULL) {
            cache->identity_tokens = tokens;
            cache->identity_token_capacity = cache->identity_token_count;
        }
    }
    if (cache->identity_name_count != 0
        && cache->identity_name_count < cache->identity_name_capacity) {
        struct LayoutIdentityNames *names = budget_realloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT, cache->identity_names,
            cache->identity_name_count * sizeof(*names));
        if (names != NULL) {
            cache->identity_names = names;
            cache->identity_name_capacity = cache->identity_name_count;
        }
    }
    return true;
}

/* A StyleX marker class on <main> is read by ~100 rules' ancestors. */
#define LAYOUT_IDENTITY_KEY_LIMIT 512u

static int layout_key_compare(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *) left, b = *(const uint32_t *) right;
    return a < b ? -1 : a > b;
}

/* What a change of `tokens` on an element reaches besides the element:
   the subject keys of the rules reading one of them off their subject
   (keys[0..*key_count), within the element's subtree, or its parent's
   when *sibling), and the custom-property names rules keyed by them
   declare. False when that cannot be bounded: the tables are unbounded,
   a reading rule's subject carries no key, or more keys than fit. */
static bool layout_identity_reach(LayoutReuseCache *cache,
                                  const uint32_t *tokens, size_t count,
                                  uint32_t *keys, size_t *key_count,
                                  bool *sibling, uint64_t *names)
{
    *names = 0;
    *key_count = 0;
    *sibling = false;
    if (!layout_identity_prepare(cache)) return false;
    for (size_t t = 0; t < count; t++) {
        size_t low = 0, high = cache->identity_token_count;
        while (low < high) {
            size_t middle = low + (high - low) / 2u;
            if (cache->identity_tokens[middle].token < tokens[t])
                low = middle + 1u;
            else high = middle;
        }
        for (; low < cache->identity_token_count
               && cache->identity_tokens[low].token == tokens[t]; low++) {
            const struct LayoutIdentityDependent *dependent =
                &cache->identity_tokens[low];
            if (dependent->subject_key == 0) return false;
            *sibling |= dependent->sibling;
            if (*key_count == LAYOUT_IDENTITY_KEY_LIMIT) return false;
            keys[(*key_count)++] = dependent->subject_key;
        }
        low = 0;
        high = cache->identity_name_count;
        while (low < high) {
            size_t middle = low + (high - low) / 2u;
            if (cache->identity_names[middle].token < tokens[t])
                low = middle + 1u;
            else high = middle;
        }
        for (; low < cache->identity_name_count
               && cache->identity_names[low].token == tokens[t]; low++)
            *names |= cache->identity_names[low].names;
    }
    qsort(keys, *key_count, sizeof(*keys), layout_key_compare);
    size_t kept = 0;
    for (size_t i = 0; i < *key_count; i++)
        if (kept == 0 || keys[kept - 1] != keys[i]) keys[kept++] = keys[i];
    *key_count = kept;
    return true;
}

/* Drops what a change reaching `keys`/`names` (layout_identity_reach) at
   `node` leaves stale: the element's own entries, and entries within the
   node's subtree (its parent's when `sibling`) carrying one of the keys or
   reading one of the names (a reached subject passes the names it now
   declares to its descendants, siblings' included). */
static void layout_reuse_drop_reached(LayoutReuseCache *cache,
                                      lxb_dom_node_t *node,
                                      const uint32_t *keys, size_t key_count,
                                      bool sibling, uint64_t names)
{
    lxb_dom_node_t *scope = sibling ? node->parent : node;
    layout_reuse_drop_element(cache, node, cache->matches == NULL);
    for (size_t i = 0;
         (names != 0 || key_count != 0) && i <= cache->style_mask;
         i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node == NULL
            || !layout_node_is_within(entry->node, scope)) continue;
        bool reached = (entry->variable_reads & names) != 0;
        if (!reached)
            reached = style_element_carries_any_key(entry->node, keys,
                                                    key_count);
        if (reached) layout_reuse_drop_style(cache, entry);
    }
    layout_reuse_cache_invalidate_measurements(cache, scope);
}

void layout_reuse_cache_invalidate_attribute(
    LayoutReuseCache *cache, lxb_dom_node_t *node,
    const uint32_t *changed_tokens, size_t changed_token_count,
    bool text_or_structure_sensitive, bool relational_selector_sensitive)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    if (relational_selector_sensitive
        && !layout_reuse_has_note(cache, node, false, NULL,
                                  changed_tokens == NULL ? NULL
                                                         : changed_tokens,
                                  changed_token_count)) return;
    if (changed_tokens == NULL) {
        layout_reuse_invalidate_scoped_internal(
            cache, node, text_or_structure_sensitive, true);
        return;
    }
    uint64_t names = 0;
    uint32_t keys[LAYOUT_IDENTITY_KEY_LIMIT];  /* 2 KiB of stack */
    size_t key_count = 0;
    bool sibling = false;
    if (!text_or_structure_sensitive
        && layout_identity_reach(cache, changed_tokens, changed_token_count,
                                 keys, &key_count, &sibling, &names)
        && !(sibling && (node->parent == NULL
                         || node->parent->type
                                != LXB_DOM_NODE_TYPE_ELEMENT))) {
        /* The element restyles and its descendants re-key off its style;
           beyond that the changed tokens reach only the elements carrying
           a subject key of a rule that reads them, and the readers of the
           custom properties rules keyed by them declare. */
        layout_reuse_drop_reached(cache, node, keys, key_count, sibling,
                                  names);
        if (key_count == 0) cache->stats.class_scoped++;
        else cache->stats.class_keyed++;
    } else {
        layout_reuse_invalidate_scoped_internal(
            cache, node, text_or_structure_sensitive, cache->matches == NULL);
    }
    /* Retained lists are narrowed by the changed tokens at the flush. */
    if (cache->matches == NULL) return;
    if (cache->pending_node_count == LAYOUT_REUSE_PENDING_NODE_LIMIT) {
        cache->pending_overflow = true;
    } else {
        cache->pending_nodes[cache->pending_node_count++] = node;
    }
    for (size_t i = 0; i < changed_token_count; i++) {
        bool present = false;
        for (size_t p = 0; !present && p < cache->pending_token_count; p++) {
            present = cache->pending_tokens[p] == changed_tokens[i];
        }
        if (present) continue;
        if (cache->pending_token_count == LAYOUT_REUSE_PENDING_TOKEN_LIMIT) {
            cache->pending_overflow = true;
            break;
        }
        cache->pending_tokens[cache->pending_token_count++] =
            changed_tokens[i];
    }
    cache->pending_parent_scope = cache->pending_parent_scope
        || text_or_structure_sensitive || cache->selector_has_structure;
    cache->pending_active = true;
}

void layout_reuse_cache_flush_invalidations(LayoutReuseCache *cache)
{
    if (cache == NULL) return;
    layout_reuse_has_flush(cache);
    if (!cache->pending_active) return;
    if (cache->matches != NULL) {
        if (cache->pending_overflow || cache->sheet == NULL) {
            style_retained_matches_clear(cache->matches);
        } else {
            style_retained_matches_invalidate_tokens(
                cache->matches, cache->sheet, cache->pending_nodes,
                cache->pending_node_count, cache->pending_tokens,
                cache->pending_token_count, cache->pending_parent_scope);
        }
    }
    cache->pending_active = false;
    cache->pending_overflow = false;
    cache->pending_parent_scope = false;
    cache->pending_node_count = 0;
    cache->pending_token_count = 0;
}

void *layout_reuse_cache_attach_matches(LayoutReuseCache *cache,
                                        const Stylesheet *sheet)
{
    return style_retained_matches_attach(
        sheet, cache != NULL && cache->sheet == sheet ? cache->matches : NULL);
}

void layout_reuse_cache_detach_matches(const Stylesheet *sheet,
                                       void *previous)
{
    (void) style_retained_matches_attach(sheet, previous);
}

void layout_reuse_cache_invalidate_structure(LayoutReuseCache *cache,
                                             lxb_dom_node_t *node)
{
    layout_reuse_cache_invalidate_tree(cache, node, true);
}

void layout_reuse_cache_invalidate_tree(LayoutReuseCache *cache,
                                        lxb_dom_node_t *node, bool relational)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    /* `node` was inserted, removed or moved, or its child list changed.
       Without :has() the effect is bound to the parent scope (structural
       pseudo-classes, sibling combinators). With :has() the answers that
       can move are found from the changed position (layout_reuse_has_note);
       descendants re-key off any parent style that changes. */
    if (relational && !layout_reuse_has_note(cache, node, true, NULL, NULL, 0))
        return;
    layout_reuse_invalidate_scoped_internal(cache, node, true, true);
}

void layout_reuse_cache_invalidate_appended(LayoutReuseCache *cache,
                                            lxb_dom_node_t *node,
                                            bool relational)
{
    if (cache == NULL || node == NULL || node->parent == NULL
        || node->parent->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        layout_reuse_cache_invalidate_tree(cache, node, relational);
        return;
    }
    if (relational && !layout_reuse_has_note(cache, node, true, NULL, NULL, 0))
        return;
    layout_reuse_invalidate_child_list(cache, node->parent, node, NULL,
                                       false, true, true);
}

void layout_reuse_cache_invalidate_children(LayoutReuseCache *cache,
                                            lxb_dom_node_t *parent,
                                            const lxb_dom_node_t *removed,
                                            bool relational)
{
    if (cache == NULL || parent == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    if (relational
        && !layout_reuse_has_note(cache, parent, true, NULL, NULL, 0)) return;
    layout_reuse_invalidate_child_list(cache, parent, NULL, removed,
                                       removed == NULL, true, false);
}

void layout_reuse_cache_enable_node_retirement(LayoutReuseCache *cache)
{
    if (cache != NULL) cache->node_retirement = true;
}

bool layout_reuse_cache_node_retirement(const LayoutReuseCache *cache)
{
    return cache != NULL && cache->node_retirement;
}

void layout_reuse_cache_retire_subtree(LayoutReuseCache *cache,
                                       const lxb_dom_node_t *root)
{
    if (cache == NULL || root == NULL) return;
    /* Computed-style reads also retain text/comment nodes. Retire every
       node kind before destruction, including single textContent children. */
    lxb_dom_node_t *scope = (lxb_dom_node_t *) root;
    if (root->type == LXB_DOM_NODE_TYPE_TEXT
        || root->type == LXB_DOM_NODE_TYPE_COMMENT) {
        /* These leaf nodes need only the bounded hash probe, not a full
           style-table walk for every textContent child. Other cache families
           and pending invalidations still retire below. */
        layout_reuse_drop_element(cache, scope, false);
    } else {
        for (size_t i = 0; i <= cache->style_mask; i++) {
            LayoutReuseStyleEntry *entry = &cache->styles[i];
            if (entry->node != NULL
                && layout_node_is_within(entry->node, scope)) {
                layout_reuse_drop_style(cache, entry);
            }
        }
    }
    for (size_t i = 0; i < LAYOUT_REUSE_INTRINSIC_CAPACITY; i++) {
        LayoutIntrinsicCacheEntry *entry = &cache->intrinsic[i];
        if (entry->node != NULL
            && layout_node_is_within(entry->node, scope)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    for (size_t i = 0; cache->table_rows != NULL
                       && i < LAYOUT_REUSE_TABLE_ROW_CAPACITY; i++) {
        LayoutReuseTableRowEntry *entry = &cache->table_rows[i];
        if (entry->row != NULL
            && layout_node_is_within(entry->row, scope)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    style_retained_matches_forget_subtree(cache->matches, scope);
    style_has_pending_retire(&cache->has_pending, scope);
    /* A pending token invalidation must not name a node about to die. */
    size_t kept = 0;
    for (size_t i = 0; i < cache->pending_node_count; i++) {
        const lxb_dom_node_t *pending = cache->pending_nodes[i];
        if (layout_node_is_within((lxb_dom_node_t *) pending, scope)) continue;
        cache->pending_nodes[kept++] = pending;
    }
    cache->pending_node_count = kept;
    cache->stats.retired_subtrees++;
}

void layout_reuse_cache_invalidate_relational(
    LayoutReuseCache *cache, lxb_dom_node_t *node, const char *attribute,
    bool text_or_structure_sensitive)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    if (!layout_reuse_has_note(cache, node, text_or_structure_sensitive,
                               attribute, NULL, 0)) return;
    /* The :has() side is noted; other selectors see an attribute write
       as any other. */
    if (attribute != NULL && !text_or_structure_sensitive) {
        layout_reuse_cache_invalidate_own_attribute(cache, node, attribute);
        return;
    }
    layout_reuse_cache_invalidate_node_scoped(
        cache, node, text_or_structure_sensitive);
}

void layout_reuse_cache_invalidate_overflow_root(LayoutReuseCache *cache,
                                                 lxb_dom_node_t *root)
{
    if (cache == NULL || root == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    /* Anchors inside the subtree have their subjects inside it too (or
       keyed anywhere); those at and above it are the walk's. */
    if (!layout_reuse_has_note(cache, root, true, NULL, NULL, 0)) return;
    layout_reuse_drop_subtree(cache, root, true);
    layout_reuse_cache_invalidate_measurements(cache, root);
    cache->stats.overflow_scoped++;
}

void layout_reuse_cache_invalidate_shadow_composition(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL || node == NULL) return;
    lxb_dom_node_t *carrier = document_shadow_carrier_containing(node);
    lxb_dom_node_t *host = carrier == NULL ? NULL : carrier->parent;
    if (host == NULL || host->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    /* A change inside a shadow tree can move which slot renders a light
       child: restyle the host's light children (their descendants re-key
       off them), and the measurements that include the host. */
    size_t children = 0;
    for (lxb_dom_node_t *child = host->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || child == carrier)
            continue;
        if (++children > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT) {
            layout_reuse_drop_subtree(cache, host, true);
            break;
        }
        layout_reuse_drop_element(cache, child, true);
    }
    layout_reuse_cache_invalidate_measurements(cache, host);
}

void layout_reuse_cache_set_structure_filter(LayoutReuseCache *cache,
                                             uint64_t entries,
                                             uint32_t serial)
{
    if (cache == NULL) return;
    cache->structure_entries = entries;
    cache->structure_serial = serial;
}

void layout_reuse_cache_invalidate_node(LayoutReuseCache *cache,
                                        lxb_dom_node_t *node,
                                        bool text_or_structure_sensitive)
{
    layout_reuse_cache_invalidate_relational(
        cache, node, NULL, text_or_structure_sensitive);
}

/* Longer journal names may be truncated prefixes (and coalesce). */
#define LAYOUT_REUSE_PROPERTY_NAME_LIMIT 31u

/* Attributes the style resolver or a pseudo-class reads without an
   attribute selector naming them, which a selector can then read on
   elements other than its subject (`input:checked + label`), or which the
   resolver reads on other elements: form-control state and validity
   (fieldset/optgroup/select disabling, option display, :checked,
   :required, :read-only, :placeholder-shown, :default, :in-range...),
   links, popovers and dialogs, direction and language, a details
   element's open state (its children), table presentation (cells read
   the table), and the navigation roles and ARIA states a custom element's
   fallbacks look up. */
static bool layout_attribute_reaches_other_elements(const char *name)
{
    static const char *const names[] = {
        "disabled", "multiple", "selected", "dir", "lang", "open", "role",
        "cellpadding", "cellspacing", "border", "rules", "frame", "align",
        "valign", "bgcolor", "background", "checked", "required",
        "readonly", "placeholder", "value", "type", "name", "form", "min",
        "max", "step", "pattern", "minlength", "maxlength", "href",
        "popover", "hidden", "inert", "contenteditable", "controls",
        "aria-expanded", "aria-hidden", "aria-selected", "aria-checked",
        "aria-disabled", "aria-current", "aria-pressed", "for", "list",
        "size", "span", "colspan", "rowspan", "start", "reversed",
    };
    if (strncmp(name, "data-tilefinch", 14) == 0) return true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

void layout_reuse_cache_invalidate_own_attribute(
    LayoutReuseCache *cache, lxb_dom_node_t *node, const char *attribute)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    size_t length = attribute == NULL ? 0 : strlen(attribute);
    /* A selector naming the attribute (in any position) can restyle other
       elements; so can the few attributes the resolver reads elsewhere.
       Otherwise the attribute is visible only to the element's own
       cascade (presentational mapping, its own pseudo-class state):
       its descendants re-key off its style if that changes. */
    if (length == 0 || length >= LAYOUT_REUSE_PROPERTY_NAME_LIMIT
        || strcmp(attribute, "class") == 0 || strcmp(attribute, "id") == 0
        || strcmp(attribute, "style") == 0 || cache->sheet == NULL
        || layout_attribute_reaches_other_elements(attribute)) {
        layout_reuse_cache_invalidate_node_scoped(cache, node, false);
        return;
    }
    if (!stylesheet_selectors_reference_attribute_prefix(
            cache->sheet, attribute, length)) {
        layout_reuse_drop_element(cache, node, true);
        layout_reuse_cache_invalidate_measurements(cache, node);
        cache->stats.own_attribute_scoped++;
        return;
    }
    /* Selectors name it: as for a class change, the element and the
       subjects of the rules that read it off another element. */
    uint32_t token = stylesheet_identity_attribute_hash(attribute, length);
    uint32_t keys[LAYOUT_IDENTITY_KEY_LIMIT];  /* 2 KiB of stack */
    size_t key_count = 0;
    bool sibling = false;
    uint64_t names = 0;
    if (!layout_identity_reach(cache, &token, 1, keys, &key_count, &sibling,
                               &names)
        || cache->identity_attributes_opaque
        || (sibling && (node->parent == NULL
                        || node->parent->type != LXB_DOM_NODE_TYPE_ELEMENT))) {
        layout_reuse_cache_invalidate_node_scoped(cache, node, false);
        return;
    }
    /* Retained match lists test attribute selectors by name. */
    layout_reuse_drop_reached(cache, node, keys, key_count, sibling, names);
    if (cache->matches != NULL)
        style_retained_matches_invalidate_within(cache->matches,
                                                 sibling ? node->parent
                                                         : node);
    cache->stats.own_attribute_keyed++;
}

void layout_reuse_cache_invalidate_inline_style(
    LayoutReuseCache *cache, lxb_dom_node_t *node, const char *property,
    bool relational)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    if (relational) {
        /* An inline declaration is visible to selectors only as the style
           attribute. */
        layout_reuse_cache_invalidate_relational(cache, node, "style", false);
        return;
    }
    /* Unless a selector reads the style attribute, the write changes this
       element's own cascade (its descendants re-key off its style if that
       changes) and the custom properties it declares, which descendants
       read by name, not through its computed style. */
    bool scoped = cache->sheet != NULL
        && !stylesheet_selectors_reference_attribute_prefix(
               cache->sheet, "style", 5);
    uint64_t names = 0;
    if (scoped && property != NULL) {
        size_t length = strlen(property);
        if (length == 0 || length >= LAYOUT_REUSE_PROPERTY_NAME_LIMIT) {
            scoped = false;
        } else if (length > 2 && property[0] == '-' && property[1] == '-') {
            names = stylesheet_custom_property_name_bits(property, length);
        }
    } else if (scoped) {
        /* The whole attribute: the names it declared when last resolved
           and the names it declares now. */
        const LayoutReuseStyleEntry *previous = NULL;
        size_t home = layout_pointer_hash(node)
                      & cache->style_mask;
        for (size_t probe = 0; probe < 8 && previous == NULL; probe++) {
            const LayoutReuseStyleEntry *entry = &cache->styles[
                (home + probe) & cache->style_mask];
            if (entry->node == node) previous = entry;
        }
        size_t length = 0;
        const char *text = document_attribute(node, "style", &length);
        if (previous == NULL) scoped = false;
        else names = previous->inline_variables
            | (text == NULL ? 0
               : stylesheet_inline_custom_property_bits(text, length));
    }
    if (!scoped) {
        layout_reuse_cache_invalidate_node_scoped(cache, node, false);
        return;
    }
    layout_reuse_drop_element(cache, node, false);
    for (size_t i = 0; names != 0 && i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node != NULL && (entry->variable_reads & names) != 0
            && layout_node_is_within(entry->node, node)) {
            layout_reuse_drop_style(cache, entry);
        }
    }
    layout_reuse_cache_invalidate_measurements(cache, node);
    cache->stats.inline_style_scoped++;
}

void layout_reuse_cache_set_focus_scroll(LayoutReuseCache *cache, int top)
{
    if (cache != NULL) cache->focus_scroll = top < 0 ? 0 : top;
}

void layout_reuse_cache_invalidate_local_focus(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_drop_subtree(cache, node, true);
    layout_reuse_cache_invalidate_measurements(cache, node);
}

void layout_reuse_cache_forget_focus_state(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL) return;
    if (node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    /* Descendants key on this element's style, so a changed value re-keys
       them; an unchanged one keeps their entries. */
    layout_reuse_drop_element(cache, node, true);
}

/* Following siblings a forward focus rule's reach is tested against; a
   longer run keeps the whole-table scan. */
#define LAYOUT_REUSE_FOCUS_SIBLING_LIMIT 64u

/* Whether `entry` lies in `node`'s subtree or in a later sibling's
   subtree: the only elements a forward focus rule can restyle
   (style_focus_selector_reaches_forward). */
static bool layout_node_follows_from(const lxb_dom_node_t *entry,
                                     const lxb_dom_node_t *node)
{
    const lxb_dom_node_t *top = entry;
    while (top != NULL && top->parent != node->parent) top = top->parent;
    if (top == NULL) return false;
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->next)
        if (at == top) return true;
    return false;
}

void layout_reuse_cache_invalidate_focus(
    LayoutReuseCache *cache, lxb_dom_node_t *node)
{
    if (cache == NULL || node == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    /* Local focus can change geometry or complex paint without changing
       unrelated selector answers. Keep full flow, but not a whole-page
       cascade reset caused by an unrelated :has() or custom focus rule. */
    if (style_focus_change_is_local(cache->sheet, node)) {
        layout_reuse_cache_invalidate_local_focus(cache, node);
        return;
    }
    if (cache->selector_focus_has_sibling) {
        /* Sibling focus rules can reach beyond this subtree, but only
           focus-reading selectors change their matched answer. Keep other
           lists and unrelated styles. Parent-style keys propagate inherited
           changes; sizing and geometry still take the complete flow. */
        uint32_t custom[64];
        uint32_t focus_rules[128];
        StyleRuleCandidatePlan plan;
        size_t custom_count = 0;
        bool bounded = true;
        bool variables_checked = false;
        for (size_t i = 0; i < cache->sheet->custom_rule_count; i++) {
            const StyleCustomRule *rule = &cache->sheet->custom_rules[i];
            if (!style_selector_reads_focus(rule->selector)) continue;
            /* Variable inheritance is not represented by the parent style
               key. Only keep unrelated styles when no variable selector can
               change in either state, not merely when it currently misses. */
            if (strncmp(rule->name, "--", 2) == 0) {
                if (!variables_checked
                    && !style_focus_custom_properties_unchanged(cache->sheet, node)) {
                    bounded = false;
                    break;
                }
                variables_checked = true;
                continue;
            }
            if (custom_count == 64u) {
                bounded = false;
                break;
            }
            custom[custom_count++] = (uint32_t) i;
        }
        size_t focus_count = bounded ? style_focus_nonlocal_rules(
            cache->sheet, node, focus_rules, 128u) : SIZE_MAX;
        if (focus_count == SIZE_MAX
            || !style_rule_candidate_plan(cache->sheet,
                focus_rules, focus_count, &plan)) {
            /* Too many focus rules (or reaching rules) to bound the
               subjects. As for a checkedness toggle: every computed style
               and measurement goes, and so do the retained lists any focus
               rule could select; every other list replays, which keeps the
               expensive half of style resolution. */
            if (cache->matches == NULL
                || !style_focus_rules_restyle_by_lists(cache->sheet)) {
                layout_reuse_cache_reset(cache);
                return;
            }
            layout_reuse_cache_flush_invalidations(cache);
            layout_reuse_clear_counters(cache);
            layout_reuse_clear_styles(cache);
            layout_reuse_clear_sizing_entries(cache);
            style_retained_matches_invalidate_within(cache->matches, node);
            style_retained_matches_invalidate_rules(cache->matches,
                cache->sheet, cache->sheet->focus_rule_indices,
                cache->sheet->focus_rule_count);
            cache->stats.scoped_invalidations++;
            return;
        }
        /* When every rule that can reach another element reads focus only
           on the element it moved on (no :focus-within, no :has()), its
           subjects lie in this node's subtree or a following sibling's:
           test only those entries, and drop only the measurements that
           include the parent. */
        bool forward = node->parent != NULL;
        size_t following = 0;
        for (const lxb_dom_node_t *at = node; forward && at != NULL;
             at = at->next)
            forward = ++following <= LAYOUT_REUSE_FOCUS_SIBLING_LIMIT;
        for (size_t i = 0; forward && i < focus_count; i++)
            forward = style_focus_selector_reaches_forward(
                cache->sheet->rules[focus_rules[i]].selector);
        for (size_t i = 0; forward && i < custom_count; i++)
            forward = style_focus_selector_reaches_forward(
                cache->sheet->custom_rules[custom[i]].selector);
        layout_reuse_cache_flush_invalidations(cache);
        /* Self-only focus rules cannot change another subject's answer.
           Descendant inheritance still follows the changed parent style. */
        layout_reuse_drop_subtree(cache, node, true);
        layout_reuse_clear_counters(cache);
        for (size_t i = 0; i <= cache->style_mask; i++) {
            LayoutReuseStyleEntry *entry = &cache->styles[i];
            if (entry->node != NULL
                && (!forward || layout_node_follows_from(entry->node, node))
                && (style_rules_may_select_node(
                    cache->sheet, cache->matches, &plan, entry->node)
                || style_custom_rules_may_select_node(cache->sheet, custom,
                    custom_count, entry->node)))
                layout_reuse_drop_style(cache, entry);
        }
        if (forward)
            layout_reuse_drop_measurements(cache, node->parent);
        else
            layout_reuse_clear_sizing_entries(cache);
        style_retained_matches_invalidate_rules(cache->matches, cache->sheet,
            focus_rules, focus_count);
        cache->stats.scoped_invalidations++;
        return;
    }
    /* A :has(:focus) answer moves like any other state a :has() reads, and
       so does a :focus-within: the :has() plan drops the anchors on this
       node's ancestor chain and the subjects their rules style (an
       ancestor's :focus-within can style a descendant whose parent style
       does not change). */
    if (!layout_reuse_has_note(cache, node, false, "data-tilefinch-focus",
                               NULL, 0)) return;
    /* Focus without sibling combinators or :focus-within can change the
       focused subtree and its ancestor chain. Descendant cache
       keys include the complete parent style, so a changed inherited or
       geometric ancestor value naturally misses below the first recomputed
       ancestor. Intrinsic measurements touching the node are cleared by the
       ordinary scoped path. */
    layout_reuse_cache_invalidate_node_scoped(cache, node, false);
    for (size_t i = 0; i <= cache->style_mask; i++) {
        LayoutReuseStyleEntry *entry = &cache->styles[i];
        if (entry->node != NULL
            && layout_node_is_within(node, entry->node)) {
            layout_reuse_drop_style(cache, entry);
        }
    }
    style_retained_matches_invalidate_ancestors(cache->matches, node);
}

void layout_reuse_cache_invalidate_checked(
    LayoutReuseCache *cache, lxb_dom_node_t *const *nodes, size_t count)
{
    if (cache == NULL || nodes == NULL || count == 0
        || cache->state_rules_bounded
        || cache->matches == NULL || cache->sheet == NULL) {
        layout_reuse_cache_reset(cache);
        return;
    }
    layout_reuse_cache_flush_invalidations(cache);
    /* Only rules that can observe checkedness change their answer, and the
       elements they can select are found by those rules' fast keys, however
       far a sibling or descendant combinator carries the state. Every other
       retained list replays. Computed styles and measurements are keyed by
       node, not by the rules that produced them, so they all go; that is
       the cheap half of style resolution. */
    layout_reuse_clear_counters(cache);
    layout_reuse_clear_styles(cache);
    layout_reuse_clear_sizing_entries(cache);
    for (size_t i = 0; i < count; i++) {
        if (nodes[i] != NULL)
            style_retained_matches_invalidate_within(cache->matches, nodes[i]);
    }
    style_retained_matches_invalidate_rules(
        cache->matches, cache->sheet, cache->state_rules,
        cache->state_rule_count);
    cache->stats.scoped_invalidations++;
}

void layout_reuse_cache_stats(const LayoutReuseCache *cache,
                              LayoutReuseStats *stats)
{
    if (stats == NULL) return;
    if (cache == NULL) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = cache->stats;
    if (cache->matches != NULL) {
        stats->retained_bytes += style_retained_matches_bytes(cache->matches);
        stats->matched_hits = cache->matches->hits;
        stats->matched_misses = cache->matches->misses;
        stats->matched_stores = cache->matches->stores;
        stats->matched_token_invalidations =
            cache->matches->token_invalidations;
        stats->matched_token_fallbacks = cache->matches->token_fallbacks;
        stats->matched_token_dropped = cache->matches->token_dropped;
        stats->matched_token_affected_rules =
            cache->matches->token_affected_rules;
    }
}

bool layout_reuse_cache_can_reuse_mutations(const LayoutReuseCache *cache)
{
    /* The mutation journal classifies exact before/after attribute changes
       against relational-selector dependencies. Unknown mutations still
       take the conservative reset path. */
    return cache != NULL;
}

/* The offset of the first `pseudo` (any text) at or after `from` in
   `selector` outside every :has() argument (those are the scoped :has()
   invalidation's), or SIZE_MAX. */
static size_t layout_selector_find_outside_has(const char *selector,
                                               const char *pseudo,
                                               size_t from)
{
    size_t length = strlen(pseudo);
    unsigned depth = 0, has_depth = 0;
    for (const char *at = selector; *at != '\0'; at++) {
        if (*at == '(') {
            depth++;
            if (has_depth == 0 && at - selector >= 4
                && strncmp(at - 4, ":has", 4) == 0) has_depth = depth;
        } else if (*at == ')' && depth != 0) {
            if (depth == has_depth) has_depth = 0;
            depth--;
        } else if (has_depth == 0 && (size_t) (at - selector) >= from
                   && strncmp(at, pseudo, length) == 0) {
            return (size_t) (at - selector);
        }
    }
    return SIZE_MAX;
}

static bool layout_selector_pseudo_outside_has(const char *selector,
                                               const char *pseudo)
{
    return layout_selector_find_outside_has(selector, pseudo, 0) != SIZE_MAX;
}

static void layout_reuse_note_empty_keys(LayoutReuseCache *cache,
                                         const char *selector,
                                         const char *pseudo)
{
    size_t length = strlen(selector);
    for (size_t at = layout_selector_find_outside_has(selector, pseudo, 0);
         at != SIZE_MAX;
         at = layout_selector_find_outside_has(selector, pseudo, at + 1)) {
        uint32_t key = style_selector_compound_key_at(selector, length, at);
        bool present = false;
        for (size_t i = 0; i < cache->empty_key_count; i++)
            present = present || cache->empty_keys[i] == key;
        if (present) continue;
        if (key == 0 || cache->empty_key_count == LAYOUT_REUSE_EMPTY_KEY_LIMIT)
            cache->empty_any = true;
        else
            cache->empty_keys[cache->empty_key_count++] = key;
    }
}

/* Whether a change to `element`'s children can restyle its siblings
   through an :empty test (see LayoutReuseCache.empty_keys). */
static bool layout_reuse_empty_reaches_siblings(
    const LayoutReuseCache *cache, const lxb_dom_node_t *element)
{
    if (!cache->selector_has_empty || element == NULL) return false;
    if (cache->empty_any) return true;
    for (size_t i = 0; i < cache->empty_key_count; i++) {
        if (style_element_carries_key(element, cache->empty_keys[i]))
            return true;
    }
    return false;
}


/* Which of the substrings the dependency notes test occur in a selector,
   found in one pass (the notes below used to call strstr fifteen times
   per selector, over every selector of every sheet a cache binds). Each
   flag means exactly "strstr(selector, needle) != NULL". */
enum {
    SELECTOR_HAS = 1u << 0,          /* ":has(" */
    SELECTOR_FOCUS_WITHIN = 1u << 1, /* ":focus-within" */
    SELECTOR_PLUS = 1u << 2,         /* '+' */
    SELECTOR_TILDE = 1u << 3,        /* '~' */
    SELECTOR_DASH_CHILD = 1u << 4,   /* "-child" */
    SELECTOR_OF_TYPE = 1u << 5,      /* "-of-type" */
    SELECTOR_EMPTY = 1u << 6,        /* ":empty" */
    SELECTOR_BLANK = 1u << 7,        /* ":blank" */
    SELECTOR_CHECKED = 1u << 8,      /* "checked" */
    SELECTOR_DEFAULT = 1u << 9,      /* ":default" */
    SELECTOR_INDETERMINATE = 1u << 10, /* ":indeterminate" */
    SELECTOR_VALID = 1u << 11,       /* "valid" */
    SELECTOR_FOCUS = 1u << 12,       /* ":focus" */
    SELECTOR_NTH = 1u << 13,         /* ":nth-" */
    SELECTOR_FIRST = 1u << 14,       /* ":first-" */
    SELECTOR_LAST = 1u << 15,        /* ":last-" */
    SELECTOR_ONLY = 1u << 16         /* ":only-" */
};

static bool layout_selector_text_at(const char *at, const char *needle,
                                    size_t length)
{
    return strncmp(at, needle, length) == 0;
}

static unsigned layout_selector_dependency_flags(const char *selector)
{
    unsigned flags = 0;
    for (const char *at = selector; *at != '\0'; at++) {
        switch (*at) {
        case '+': flags |= SELECTOR_PLUS; break;
        case '~': flags |= SELECTOR_TILDE; break;
        case '-':
            if (layout_selector_text_at(at, "-child", 6))
                flags |= SELECTOR_DASH_CHILD;
            if (layout_selector_text_at(at, "-of-type", 8))
                flags |= SELECTOR_OF_TYPE;
            break;
        case 'c':
            if (layout_selector_text_at(at, "checked", 7))
                flags |= SELECTOR_CHECKED;
            break;
        case 'v':
            if (layout_selector_text_at(at, "valid", 5))
                flags |= SELECTOR_VALID;
            break;
        case ':':
            switch (at[1]) {
            case 'h':
                if (layout_selector_text_at(at, ":has(", 5))
                    flags |= SELECTOR_HAS;
                break;
            case 'f':
                if (layout_selector_text_at(at, ":focus", 6)) {
                    flags |= SELECTOR_FOCUS;
                    if (layout_selector_text_at(at + 6, "-within", 7))
                        flags |= SELECTOR_FOCUS_WITHIN;
                }
                if (layout_selector_text_at(at, ":first-", 7))
                    flags |= SELECTOR_FIRST;
                break;
            case 'e':
                if (layout_selector_text_at(at, ":empty", 6))
                    flags |= SELECTOR_EMPTY;
                break;
            case 'b':
                if (layout_selector_text_at(at, ":blank", 6))
                    flags |= SELECTOR_BLANK;
                break;
            case 'd':
                if (layout_selector_text_at(at, ":default", 8))
                    flags |= SELECTOR_DEFAULT;
                break;
            case 'i':
                if (layout_selector_text_at(at, ":indeterminate", 14))
                    flags |= SELECTOR_INDETERMINATE;
                break;
            case 'n':
                if (layout_selector_text_at(at, ":nth-", 5))
                    flags |= SELECTOR_NTH;
                break;
            case 'l':
                if (layout_selector_text_at(at, ":last-", 6))
                    flags |= SELECTOR_LAST;
                break;
            case 'o':
                if (layout_selector_text_at(at, ":only-", 6))
                    flags |= SELECTOR_ONLY;
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }
    return flags;
}

static void layout_reuse_note_selector_dependencies(
    LayoutReuseCache *cache, const char *selector, uint32_t rule_index)
{
    if (cache == NULL || selector == NULL) return;
    /* Every dependency below needs a pseudo-class, a combinator or an
       attribute selector; most rules (plain classes) have none, and this
       runs over the whole sheet whenever a new one is bound. */
    if (strpbrk(selector, ":+~[") == NULL) return;
    unsigned flags = layout_selector_dependency_flags(selector);
    /* A :focus-within is a :has() of the focus marker: the sheet's :has()
       plan scopes both. */
    if ((flags & (SELECTOR_HAS | SELECTOR_FOCUS_WITHIN)) != 0)
        cache->selector_has_has = true;
    if ((flags & (SELECTOR_PLUS | SELECTOR_TILDE | SELECTOR_DASH_CHILD
                  | SELECTOR_OF_TYPE)) != 0)
        style_selector_structure_keys(selector, strlen(selector),
                                      &cache->structure);
    /* Only through a sibling combinator (or an `of S` count) can an
       element's :empty restyle anything outside its own subtree. */
    if ((flags & (SELECTOR_EMPTY | SELECTOR_BLANK)) != 0
        && (layout_selector_pseudo_outside_has(selector, ":empty")
            || layout_selector_pseudo_outside_has(selector, ":blank"))
        && (layout_selector_pseudo_outside_has(selector, "+")
            || layout_selector_pseudo_outside_has(selector, "~")
            || layout_selector_pseudo_outside_has(selector, " of "))) {
        cache->selector_has_empty = true;
        layout_reuse_note_empty_keys(cache, selector, ":empty");
        layout_reuse_note_empty_keys(cache, selector, ":blank");
    }
    if ((flags & (SELECTOR_CHECKED | SELECTOR_DEFAULT
                  | SELECTOR_INDETERMINATE | SELECTOR_VALID)) != 0) {
        /* Custom-property rules are matched afresh whenever a computed
           style is, so only retained author rules need tracking. A :has()
           that can see the state can change any ancestor's answer. */
        if ((flags & SELECTOR_HAS) != 0) {
            cache->state_rules_bounded = true;
        } else if (rule_index != UINT32_MAX) {
            if (cache->state_rule_count == LAYOUT_REUSE_STATE_RULE_LIMIT) {
                cache->state_rules_bounded = true;
            } else {
                cache->state_rules[cache->state_rule_count++] = rule_index;
            }
        }
    }
    if ((flags & SELECTOR_FOCUS) != 0
        && (flags & (SELECTOR_PLUS | SELECTOR_TILDE)) != 0) {
        cache->selector_focus_has_sibling = true;
    }
    if ((flags & (SELECTOR_NTH | SELECTOR_FIRST | SELECTOR_LAST
                  | SELECTOR_ONLY | SELECTOR_EMPTY | SELECTOR_PLUS
                  | SELECTOR_TILDE)) != 0) {
        cache->selector_has_structure = true;
    }
}

/* The selector facts a reuse cache derives from its sheet (the scan above
   over every selector: 7 ms on the host, 0.8 s on the PSP for an 11,700-rule
   front page) depend only on the sheet's contents. A sheet's build
   generation is globally monotonic and changes with its contents, so one
   remembered result, keyed by it and the rule counts, serves every cache
   later bound to the same sheet (a reset after a layout rebuild, a
   relayout's fresh cache, the image walk's). A few entries: the load
   preview's sheet and the page's alternate. */
#define LAYOUT_SELECTOR_FACTS_MEMO 4u
static struct LayoutSelectorFacts {
    uint64_t generation;
    size_t rule_count, custom_count;
    bool valid;
    bool selector_has_has;
    StyleStructureKeys structure;
    StyleTrailingKeys trailing;
    StyleHasArgumentKeys has_arguments;
    bool hidden_facts_ready;
    bool selector_has_empty;
    uint32_t empty_keys[LAYOUT_REUSE_EMPTY_KEY_LIMIT];
    uint8_t empty_key_count;
    bool empty_any;
    uint32_t state_rules[LAYOUT_REUSE_STATE_RULE_LIMIT];
    size_t state_rule_count;
    bool state_rules_bounded;
    bool selector_focus_has_sibling;
    bool selector_has_structure;
} layout_selector_facts_memo[LAYOUT_SELECTOR_FACTS_MEMO];
static unsigned layout_selector_facts_next;

static bool layout_selector_facts_restore(LayoutReuseCache *cache,
                                          const Stylesheet *sheet)
{
    const struct LayoutSelectorFacts *facts = NULL;
    for (unsigned i = 0; facts == NULL && i < LAYOUT_SELECTOR_FACTS_MEMO; i++) {
        const struct LayoutSelectorFacts *entry =
            &layout_selector_facts_memo[i];
        if (entry->valid && entry->generation == sheet->build_generation
            && entry->rule_count == sheet->count
            && entry->custom_count == sheet->custom_rule_count)
            facts = entry;
    }
    if (facts == NULL) return false;
    cache->selector_has_has = facts->selector_has_has;
    cache->structure = facts->structure;
    cache->trailing = facts->trailing;
    cache->has_arguments = facts->has_arguments;
    cache->hidden_facts_ready = facts->hidden_facts_ready;
    cache->selector_has_empty = facts->selector_has_empty;
    memcpy(cache->empty_keys, facts->empty_keys,
           sizeof(cache->empty_keys));
    cache->empty_key_count = facts->empty_key_count;
    cache->empty_any = facts->empty_any;
    memcpy(cache->state_rules, facts->state_rules,
           sizeof(cache->state_rules));
    cache->state_rule_count = facts->state_rule_count;
    cache->state_rules_bounded = facts->state_rules_bounded;
    cache->selector_focus_has_sibling =
        facts->selector_focus_has_sibling;
    cache->selector_has_structure =
        facts->selector_has_structure;
    cache->stats.selector_fact_reuses++;
    return true;
}

static void layout_selector_facts_store(const LayoutReuseCache *cache,
                                        const Stylesheet *sheet)
{
    if (sheet->build_generation == 0) return;
    struct LayoutSelectorFacts *facts =
        &layout_selector_facts_memo[layout_selector_facts_next];
    layout_selector_facts_next =
        (layout_selector_facts_next + 1u) % LAYOUT_SELECTOR_FACTS_MEMO;
    facts->generation = sheet->build_generation;
    facts->rule_count = sheet->count;
    facts->custom_count = sheet->custom_rule_count;
    facts->selector_has_has = cache->selector_has_has;
    facts->structure = cache->structure;
    facts->trailing = cache->trailing;
    facts->has_arguments = cache->has_arguments;
    facts->hidden_facts_ready = cache->hidden_facts_ready;
    facts->selector_has_empty = cache->selector_has_empty;
    memcpy(facts->empty_keys, cache->empty_keys,
           sizeof(facts->empty_keys));
    facts->empty_key_count = cache->empty_key_count;
    facts->empty_any = cache->empty_any;
    memcpy(facts->state_rules, cache->state_rules,
           sizeof(facts->state_rules));
    facts->state_rule_count = cache->state_rule_count;
    facts->state_rules_bounded = cache->state_rules_bounded;
    facts->selector_focus_has_sibling =
        cache->selector_focus_has_sibling;
    facts->selector_has_structure =
        cache->selector_has_structure;
    facts->valid = true;
}

/* The selector facts for `sheet`'s current build: the cache's when it is
   bound to that build, else the remembered ones; false when neither. The
   trailing and :has() argument keys are scanned from the sheet only when
   first asked for (one pass over its selectors, a tenth of the facts scan
   on a front page), so pages that never change hidden content never pay. */
typedef struct {
    const StyleTrailingKeys *trailing;
    const StyleHasArgumentKeys *has_arguments;
    bool structure;
} LayoutSheetFacts;

static void layout_hidden_facts_scan(const Stylesheet *sheet,
                                     StyleTrailingKeys *trailing,
                                     StyleHasArgumentKeys *has_arguments)
{
    memset(trailing, 0, sizeof(*trailing));
    memset(has_arguments, 0, sizeof(*has_arguments));
    size_t count = sheet->count + sheet->custom_rule_count;
    for (size_t i = 0; i < count; i++) {
        const char *selector = i < sheet->count
            ? sheet->rules[i].selector
            : sheet->custom_rules[i - sheet->count].selector;
        if (selector == NULL || strchr(selector, ':') == NULL) continue;
        size_t length = strlen(selector);
        style_selector_trailing_keys(selector, length, trailing);
        style_selector_has_argument_keys(selector, length, has_arguments);
    }
}

static bool layout_sheet_facts(LayoutReuseCache *cache,
                               const Stylesheet *sheet,
                               LayoutSheetFacts *facts)
{
    if (sheet == NULL) return false;
    if (cache != NULL && cache->sheet == sheet
        && cache->sheet_generation == sheet->build_generation) {
        if (!cache->hidden_facts_ready) {
            layout_hidden_facts_scan(sheet, &cache->trailing,
                                     &cache->has_arguments);
            cache->hidden_facts_ready = true;
        }
        facts->trailing = &cache->trailing;
        facts->has_arguments = &cache->has_arguments;
        facts->structure = cache->selector_has_structure;
        return true;
    }
    for (unsigned i = 0; sheet->build_generation != 0
                         && i < LAYOUT_SELECTOR_FACTS_MEMO; i++) {
        struct LayoutSelectorFacts *entry = &layout_selector_facts_memo[i];
        if (!entry->valid || entry->generation != sheet->build_generation
            || entry->rule_count != sheet->count
            || entry->custom_count != sheet->custom_rule_count) continue;
        if (!entry->hidden_facts_ready) {
            layout_hidden_facts_scan(sheet, &entry->trailing,
                                     &entry->has_arguments);
            entry->hidden_facts_ready = true;
        }
        facts->trailing = &entry->trailing;
        facts->has_arguments = &entry->has_arguments;
        facts->structure = entry->selector_has_structure;
        return true;
    }
    return false;
}

/* Whether an element sibling after `node` existed before this turn. */
static bool layout_later_sibling_existed(
    const lxb_dom_node_t *node,
    bool (*is_new)(void *context, const lxb_dom_node_t *node),
    void *context)
{
    for (const lxb_dom_node_t *later = node->next; later != NULL;
         later = later->next) {
        if (later->type == LXB_DOM_NODE_TYPE_ELEMENT
            && !is_new(context, later)) return true;
    }
    return false;
}

bool layout_insertion_reaches_siblings(
    LayoutReuseCache *cache, const Stylesheet *sheet,
    const lxb_dom_node_t *node,
    bool (*is_new)(void *context, const lxb_dom_node_t *node),
    void *context)
{
    LayoutSheetFacts facts;
    if (node == NULL || is_new == NULL
        || !layout_sheet_facts(cache, sheet, &facts)) return true;
    const lxb_dom_node_t *parent = node->parent;
    if (parent == NULL || parent->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return true;
    if (!facts.structure) return false;
    /* An earlier position moves the indices of every later sibling, and
       sibling combinators read it: only siblings inserted this turn (which
       the caller proves render nothing) may follow. */
    if (layout_later_sibling_existed(node, is_new, context)) return true;
    /* Earlier siblings keep their indices from the start; only tests
       counting from the end can see the new last sibling. */
    size_t siblings = 0;
    for (const lxb_dom_node_t *earlier = node->prev; earlier != NULL;
         earlier = earlier->prev) {
        if (earlier->type != LXB_DOM_NODE_TYPE_ELEMENT
            || is_new(context, earlier)) continue;
        if (++siblings > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT
            || style_trailing_keys_reach(facts.trailing, earlier, parent))
            return true;
    }
    return false;
}

void layout_reuse_cache_invalidate_removed_last(LayoutReuseCache *cache,
                                                const Stylesheet *sheet,
                                                lxb_dom_node_t *parent,
                                                const lxb_dom_node_t *removed,
                                                bool relational)
{
    /* The children left all came before the one that went: only a test
       counting from the end can see it gone. */
    LayoutSheetFacts facts;
    bool kept = cache != NULL && parent != NULL
        && layout_sheet_facts(cache, sheet, &facts);
    size_t siblings = 0;
    for (lxb_dom_node_t *child = kept && facts.structure
             ? parent->first_child : NULL;
         child != NULL; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (++siblings > LAYOUT_REUSE_SHALLOW_CHILD_LIMIT
            || style_trailing_keys_reach(facts.trailing, child, parent)) {
            kept = false;
            break;
        }
    }
    if (!kept) {
        layout_reuse_cache_invalidate_children(cache, parent, removed,
                                               relational);
        return;
    }
    if (relational
        && !layout_reuse_has_note(cache, parent, true, NULL, NULL, 0)) return;
    layout_reuse_invalidate_child_list(cache, parent, NULL, removed, false,
                                       true, true);
}

static void layout_has_count_drop(void *opaque, lxb_dom_node_t *node)
{
    (void) node;
    (*(size_t *) opaque)++;
}

bool layout_has_tree_change_inert(const Stylesheet *sheet,
                                  lxb_dom_node_t *node, uint64_t entries,
                                  uint32_t serial)
{
    if (sheet == NULL || node == NULL) return false;
    StyleHasPending pending;
    memset(&pending, 0, sizeof(pending));
    StyleHasNoteStats stats = {0};
    size_t drops = 0;
    bool bounded = style_has_note_structure(sheet, node, entries, serial,
                                            &pending, layout_has_count_drop,
                                            &drops, &stats);
    return bounded && drops == 0 && stats.wipes == 0 && !pending.marked
        && !pending.styles_all && pending.root_count == 0
        && !pending.roots_everywhere;
}

bool layout_tree_change_has_inert(
    LayoutReuseCache *cache, const Stylesheet *sheet,
    lxb_dom_node_t *subtree, lxb_dom_node_t *parent, uint64_t entries,
    uint32_t serial)
{
    if (parent == NULL) return false;
    LayoutSheetFacts facts;
    /* No :has() argument can match an element that joined or left, and
       none whose answer the change's position can move is anchored near
       it. */
    if (layout_sheet_facts(cache, sheet, &facts)
        && !style_has_argument_keys_reach(facts.has_arguments, subtree,
                                          parent))
        return true;
    return layout_has_tree_change_inert(
        sheet, subtree != NULL && subtree->parent == parent ? subtree : parent,
        entries, serial);
}

void layout_reuse_cache_expect_elements(LayoutReuseCache *cache,
                                        size_t elements)
{
    if (cache != NULL
        && elements > cache->style_mask + 1u + cache->style_shortfall)
        cache->style_shortfall = elements - (cache->style_mask + 1u);
}

void layout_reuse_cache_prepare(LayoutReuseCache *cache,
                                const Stylesheet *sheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                int viewport_width)
{
    if (cache == NULL) return;
    layout_reuse_cache_flush_invalidations(cache);
    if (cache->style_pressure_trimmed
        && !budget_pressure_required(cache->budget,
                                     LAYOUT_REUSE_STYLE_WORKING_BYTES,
                                     LAYOUT_REUSE_STYLE_RESERVE_BYTES))
        cache->style_pressure_trimmed = false;
    layout_text_measure_prepare(cache);
    /* A few probe collisions are not a walk longer than the table. */
    if (cache->style_shortfall > (cache->style_mask + 1u) / 16u
#ifndef TILEFINCH_NO_TRACE
        && getenv("TILEFINCH_DISABLE_STYLE_TABLE_GROWTH") == NULL
#endif
        ) layout_reuse_fit_styles(cache);
    cache->style_shortfall = 0;
    cache->style_build_clock = cache->clock;
    uint64_t sheet_generation =
        sheet == NULL ? 0 : sheet->build_generation;
    bool matches_supported = style_retained_matches_supported(sheet);
    if (!matches_supported) {
        /* Preparation is outside a resolution's attachment lifetime. A
           query added by CSS can make an old table permanently unusable. */
        if (cache->matches != NULL) {
            style_retained_matches_destroy(cache->matches);
            cache->matches = NULL;
        }
        cache->matches_attempted = false;
    }
    if (matches_supported && cache->matches_enabled && cache->matches == NULL
        && !cache->matches_attempted && sheet->count >= 64u) {
        cache->matches_attempted = true;
#ifndef TILEFINCH_NO_TRACE
        if (getenv("TILEFINCH_DISABLE_RETAINED_MATCHES") == NULL)
#endif
        cache->matches = style_retained_matches_create(cache->budget);
    }
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
    cache->selector_focus_has_sibling = false;
    cache->selector_has_structure = false;
    cache->selector_has_empty = false;
    cache->empty_key_count = 0;
    cache->empty_any = false;
    memset(&cache->structure, 0, sizeof(cache->structure));
    memset(&cache->trailing, 0, sizeof(cache->trailing));
    memset(&cache->has_arguments, 0, sizeof(cache->has_arguments));
    cache->hidden_facts_ready = false;
    cache->state_rule_count = 0;
    cache->state_rules_bounded = false;
    if (sheet == NULL) return;
    if (layout_selector_facts_restore(cache, sheet)) return;
    for (size_t i = 0; i < sheet->count; i++) {
        layout_reuse_note_selector_dependencies(
            cache, sheet->rules[i].selector, (uint32_t) i);
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        layout_reuse_note_selector_dependencies(
            cache, sheet->custom_rules[i].selector, UINT32_MAX);
    }
    layout_selector_facts_store(cache, sheet);
}

static bool layout_reuse_style_get_hashed(LayoutReuseCache *cache,
                                          lxb_dom_node_t *node,
                                          uint64_t parent_hash,
                                          ComputedStyle *style)
{
    if (cache == NULL || node == NULL || style == NULL) return false;
    size_t home = layout_pointer_hash(node)
                  & cache->style_mask;
#ifndef TILEFINCH_NO_TRACE
    bool parent_miss = false;
#endif
    for (size_t probe = 0; probe < 8; probe++) {
        LayoutReuseStyleEntry *entry = &cache->styles[
            (home + probe) & cache->style_mask];
        if (entry->node == node && entry->parent_hash == parent_hash) {
            if ((entry->dependent & LAYOUT_REUSE_CONTAINER_DEPENDENT) != 0
                && style_container_log_active(cache->sheet)) break;
            entry->stamp = ++cache->clock;
            cache->stats.style_hits++;
            *style = layout_reuse_payload(cache, entry->payload)->style;
            return true;
        }
#ifndef TILEFINCH_NO_TRACE
        if (entry->node == node) parent_miss = true;
#endif
    }
    cache->stats.style_misses++;
#ifndef TILEFINCH_NO_TRACE
    if (parent_miss) cache->stats.style_parent_misses++;
#endif
    return false;
}

static void layout_reuse_style_put_hashed(LayoutReuseCache *cache,
                                          lxb_dom_node_t *node,
                                          uint64_t parent_hash,
                                          const ComputedStyle *style,
                                          bool font_dependent,
                                          bool container_dependent,
                                          uint64_t variable_reads)
{
    if (cache == NULL || node == NULL || style == NULL) return;
    size_t home = layout_pointer_hash(node)
                  & cache->style_mask;
    size_t replacement = home;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < 8; probe++) {
        size_t slot = (home + probe) & cache->style_mask;
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
    if (cache->styles[replacement].node != NULL
        && cache->styles[replacement].node != node) {
        if (cache->font_publication_active) return;
        /* At the hard ceiling, a document-order scan must not evict the
           entries it has just used. A cyclic walk larger than this table
           otherwise loses almost every hit on every subsequent build.
           Decline optional admission; the freshly resolved style remains
           authoritative in the current build. Old, unused entries can
           still be replaced, and existing-node updates remain allowed. */
        if (cache->style_mask + 1u == LAYOUT_REUSE_STYLE_CAPACITY_LIMIT
            && cache->styles[replacement].stamp > cache->style_build_clock)
            return;
        cache->style_shortfall++;
#ifndef TILEFINCH_NO_TRACE
        cache->stats.style_evictions++;
#endif
    }
    uint16_t payload = cache->styles[replacement].payload;
    if (cache->styles[replacement].node == NULL
        && !layout_reuse_admit_payload(cache, &payload)) return;
    size_t inline_length = 0;
    const char *inline_style = document_attribute(node, "style",
                                                  &inline_length);
    cache->styles[replacement] = (LayoutReuseStyleEntry) {
        .node = node,
        .parent_hash = parent_hash,
        .payload = payload,
        .stamp = ++cache->clock,
        .variable_reads = variable_reads,
        .dependent = (uint8_t) ((font_dependent
                                 ? LAYOUT_REUSE_FONT_DEPENDENT : 0u)
                                | (container_dependent
                                   ? LAYOUT_REUSE_CONTAINER_DEPENDENT : 0u)),
        .inline_variables = inline_style == NULL ? 0
            : stylesheet_inline_custom_property_bits(inline_style,
                                                     inline_length)
    };
    layout_reuse_payload(cache, payload)->style = *style;
}

static void layout_resolve_canonical_style(
    const Stylesheet *sheet, const FontSet *fonts,
    const WebFontSet *web_fonts, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *result,
    bool *font_dependent, StyleRetainedMatches *matches)
{
    StyleRetainedMatches *previous_matches = NULL;
    if (matches != NULL) {
        previous_matches = style_retained_matches_attach(sheet, matches);
    }
    *result = style_for_node(sheet, node, parent);
    *font_dependent = (result->filter_code & STYLE_FONT_CH_PENDING) != 0;
    if ((result->filter_code & STYLE_FONT_CH_PENDING) != 0
        && sheet != NULL && sheet->resolve_scratch != NULL) {
        const FontFace *face = font_context_face_at_weight(
            fonts, web_fonts, result->font_family,
            result->font_italic, style_uses_bold_face(result), computed_style_font_weight(result));
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
    /* The focus probe below toggles a live marker attribute; exact lists
       recorded under either state must not be consulted or stored there. */
    if (matches != NULL) {
        (void) style_retained_matches_attach(sheet, previous_matches);
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

static LayoutReuseStyleEntry *layout_reuse_entry_for(
    const LayoutReuseCache *cache, const lxb_dom_node_t *node)
{
    size_t home = layout_pointer_hash((lxb_dom_node_t *) node)
                  & cache->style_mask;
    for (size_t probe = 0; probe < 8; probe++) {
        const LayoutReuseStyleEntry *entry = &cache->styles[
            (home + probe) & cache->style_mask];
        if (entry->node == node) return (LayoutReuseStyleEntry *) entry;
    }
    return NULL;
}

bool layout_reuse_pseudo_absent(const LayoutReuseCache *cache,
                                const Stylesheet *sheet,
                                const lxb_dom_node_t *node,
                                PseudoElement pseudo,
                                const ComputedStyle *parent)
{
    if (cache == NULL || sheet == NULL || cache->sheet != sheet
        || node == NULL || parent == NULL || pseudo == PSEUDO_NONE)
        return false;
    const LayoutReuseStyleEntry *entry = layout_reuse_entry_for(cache, node);
    return entry != NULL
        && (entry->pseudo_absent & (1u << pseudo)) != 0
        && memcmp(&layout_reuse_payload(cache, entry->payload)->style,
                  parent, sizeof(*parent)) == 0;
}

void layout_reuse_note_pseudo(LayoutReuseCache *cache,
                              const Stylesheet *sheet,
                              const lxb_dom_node_t *node,
                              PseudoElement pseudo,
                              const ComputedStyle *parent,
                              const ComputedStyle *result)
{
    if (cache == NULL || sheet == NULL || cache->sheet != sheet
        || node == NULL || parent == NULL || pseudo == PSEUDO_NONE) return;
    LayoutReuseStyleEntry *entry = layout_reuse_entry_for(cache, node);
    if (entry == NULL
        || memcmp(&layout_reuse_payload(cache, entry->payload)->style,
                  parent, sizeof(*parent)) != 0)
        return;
    uint8_t bit = (uint8_t) (1u << pseudo);
    /* A custom property can change what it generates without restyling
       the element: such pseudo-elements are always resolved. */
    if (!result->generated_content && sheet->variable_read_names == 0)
        entry->pseudo_absent |= bit;
    else
        entry->pseudo_absent &= (uint8_t) ~bit;
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
    bool font_dependent = false;
    /* The resolution records the custom properties it reads (the focus
       probe's resolutions included: a superset is safe). */
    if (sheet != NULL) ((Stylesheet *) sheet)->variable_read_names = 0;
    uint32_t consults = style_container_consults(sheet);
    layout_resolve_canonical_style(
        sheet, fonts, stylesheet_web_font_set(sheet),
        node, parent, result, &font_dependent,
        cache != NULL && cache->sheet == sheet ? cache->matches : NULL);
    layout_reuse_style_put_hashed(
        cache, node, parent_hash, result, font_dependent,
        style_container_consults(sheet) != consults,
        sheet == NULL ? 0 : sheet->variable_read_names);
    return false;
}
