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

/* Exact matched-rule lists retained across layout builds, keyed by element.
   Only rule indices are stored, never computed values, so one bounded table
   covers a whole large document where the computed-style reuse cache cannot.
   The owner (the layout reuse cache) applies the same DOM, stylesheet and
   focus invalidation it applies to retained computed styles, and entries
   are consulted only while that owner attaches the table to the sheet's
   resolve scratch for one element resolution. */
#define STYLE_RETAINED_MATCH_RULE_LIMIT 12u
/* Elements plus their ::before/::after lists (absence is a zero-count
   entry), so about three keys per element. */
#define STYLE_RETAINED_MATCH_CAPACITY 32768u
#define STYLE_RETAINED_MATCH_PROBE_LIMIT 8u
#define STYLE_RETAINED_AFFECTED_RULE_LIMIT 64u
typedef struct {
    const lxb_dom_node_t *node;
    uint16_t rules[STYLE_RETAINED_MATCH_RULE_LIMIT];
    uint8_t count;
    uint8_t pseudo;
} StyleRetainedMatchEntry;
typedef struct StyleRetainedMatches {
    Budget *budget;
    StyleRetainedMatchEntry *entries;
    size_t occupied;
    size_t hits, misses, stores;
    size_t token_invalidations, token_fallbacks, token_dropped,
        token_affected_rules;
} StyleRetainedMatches;
/* A class/id change on `nodes` whose changed tokens are `words`: drop the
   changed elements' lists and every list of an element that could match a
   rule depending on those tokens. Falls back to dropping the changed
   elements' subtrees (their parents' when `parent_scope`) when the rule
   filters cannot bound the dependency. */
void style_retained_matches_invalidate_tokens(
    StyleRetainedMatches *table, const Stylesheet *sheet,
    const lxb_dom_node_t *const *nodes, size_t node_count,
    const uint32_t *hashes, size_t hash_count, bool parent_scope);
/* Translate every stored rule index through `remap` (old index -> new
   index); entries holding an index outside the map are dropped. */
void style_retained_matches_remap(StyleRetainedMatches *table,
                                  const uint16_t *remap, size_t old_count);
/* Drop every list whose element could be selected by one of `rules`
   (their rightmost fast key matches); a universal rule clears the table. */
void style_retained_matches_invalidate_rules(
    StyleRetainedMatches *table, const Stylesheet *sheet,
    const uint32_t *rules, size_t count);
StyleRetainedMatches *style_retained_matches_create(Budget *budget);
void style_retained_matches_destroy(StyleRetainedMatches *table);
void style_retained_matches_clear(StyleRetainedMatches *table);
/* Drop every entry whose element lies inside `scope` (inclusive). */
/* Drops the entries of every element in `root`'s subtree (inclusive) by
   direct probe, for a subtree about to be freed: O(subtree), not a table
   scan, since retirement can run once per discarded child. */
void style_retained_matches_forget_subtree(
    StyleRetainedMatches *table, const lxb_dom_node_t *root);
void style_retained_matches_invalidate_within(
    StyleRetainedMatches *table, const lxb_dom_node_t *scope);
/* Drop every entry whose element is an ancestor of `node` (inclusive). */
void style_retained_matches_invalidate_ancestors(
    StyleRetainedMatches *table, const lxb_dom_node_t *node);
size_t style_retained_matches_bytes(const StyleRetainedMatches *table);
/* Attach `table` to the sheet's resolve scratch for the caller's element
   resolutions; returns the previously attached table so it can be restored. */
StyleRetainedMatches *style_retained_matches_attach(
    const Stylesheet *sheet, StyleRetainedMatches *table);

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
