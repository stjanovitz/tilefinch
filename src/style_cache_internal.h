#ifndef TILEFINCH_STYLE_CACHE_INTERNAL_H
#define TILEFINCH_STYLE_CACHE_INTERNAL_H

#include "tilefinch/style.h"

typedef bool (*StyleSelectorCooperate)(
    void *opaque, lxb_dom_node_t *node, size_t completed_visits);

/*
 * A variable cache is valid only while one immutable layout snapshot is
 * being built.  Keeping its lifetime at this boundary prevents DOM/style
 * mutations between layouts from observing stale custom-property values.
 */
bool style_variable_cache_begin(Stylesheet *sheet, Budget *budget);
void style_variable_cache_end(Stylesheet *sheet);
void style_variable_cache_invalidate_node(
    Stylesheet *sheet, const lxb_dom_node_t *node);
size_t style_variable_cache_bytes(const Stylesheet *sheet);

/* Selector programs can contain bounded but still expensive ancestor and
   sibling walks. Layout installs this transient callback for one immutable
   build so those walks share its input/cancellation boundary. */
#define STYLE_ANCESTOR_BLOOM_CACHE_CAPACITY 128u
#define STYLE_SELECTOR_RESULT_CACHE_CAPACITY 4096u
#define STYLE_MATCHED_RANGE_CACHE_CAPACITY 64u
#define STYLE_MATCHED_RANGE_RULE_LIMIT 8u
/* Exact pseudo-element matched rule lists, never computed values. Ordinary
   element styles already have a resolved-style cache in layout. Only used in
   the immutable selector-cooperation scope; overflow keeps the normal scan. */
typedef struct {
    const lxb_dom_node_t *node;
    uint32_t start, end;
    uint32_t rules[STYLE_MATCHED_RANGE_RULE_LIMIT];
    uint8_t pseudo, count;
} StyleMatchedRangeCacheEntry;
_Static_assert(STYLE_MATCHED_RANGE_CACHE_CAPACITY
    * sizeof(StyleMatchedRangeCacheEntry) <= 4096u,
    "matched-rule memo must remain below 4 KiB per layout");
typedef struct {
    const lxb_dom_node_t *node;
    uint32_t rule_index;
    uint16_t instruction;
    uint8_t depth;
    bool matched;
} StyleSelectorResultCacheEntry;
typedef struct StyleAncestorBloomCache {
    struct {
        const lxb_dom_node_t *node;
        uint32_t words[2];
    } entries[STYLE_ANCESTOR_BLOOM_CACHE_CAPACITY];
    StyleSelectorResultCacheEntry *results;
    StyleMatchedRangeCacheEntry matched_ranges[STYLE_MATCHED_RANGE_CACHE_CAPACITY];
} StyleAncestorBloomCache;

bool style_selector_cooperation_begin(
    Stylesheet *sheet, StyleSelectorCooperate cooperate, void *opaque,
    StyleAncestorBloomCache *ancestor_cache);
void style_selector_cooperation_end(Stylesheet *sheet);
bool style_selector_cooperation_cancelled(const Stylesheet *sheet);

/* Layout may omit a pseudo box only when its cached complete rule set proves
   that no content declaration can create it. This is not a computed-style API. */
bool style_pseudo_known_absent(const Stylesheet *sheet,
    const lxb_dom_node_t *node, PseudoElement pseudo);
/* Returns a zeroed, non-generated sentinel when absence is proven. Callers
   must test generated_content before using the other fields. */
ComputedStyle style_for_layout_pseudo(const Stylesheet *sheet,
    lxb_dom_node_t *node, PseudoElement pseudo, const ComputedStyle *parent);

#endif
