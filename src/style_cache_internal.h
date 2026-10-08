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
/* A cache entry holds a value shorter than STYLE_CUSTOM_SHORT_VALUE_CAPACITY
   inline. Longer resolved values go to one spill buffer of this size per
   cache, allocated by the first such value and emptied with the table;
   once it is full, further long values are simply not cached. */
#define STYLE_VARIABLE_CACHE_SPILL_BYTES 4096u
void style_variable_cache_end(Stylesheet *sheet);
/* A cache kept by an owner outside layout builds (the DOM bridge's
   getComputedStyle), attached only while it resolves and cleared by that
   owner whenever the DOM or sheet may have changed. The table is created
   by the first var() lookup that needs one, and only when the sheet
   declares a custom property (the gate style_variable_cache_begin applies
   to layout builds): pages without var() never allocate it. */
typedef struct StyleVariableCacheLease {
    struct StyleVariableCache *cache;
    Budget *budget;
    /* The sheet build found to declare no custom property. */
    uint64_t closed_generation;
    size_t creations;
    size_t clears;
    size_t hits;
    size_t misses;
    bool refused;
} StyleVariableCacheLease;
void style_variable_cache_lease_release(StyleVariableCacheLease *lease);
/* Empties the table; a no-op when none exists or nothing was stored. */
void style_variable_cache_lease_clear(StyleVariableCacheLease *lease);
size_t style_variable_cache_lease_bytes(const StyleVariableCacheLease *lease);
bool style_variable_cache_attach(Stylesheet *sheet,
                                 StyleVariableCacheLease *lease);
void style_variable_cache_detach(Stylesheet *sheet,
                                 StyleVariableCacheLease *lease);
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
    /* The exact ancestor-token mask of a node and all its ancestors, for
       the sheet's StyleRuleAncestorTokens table with this stamp. */
    struct {
        const lxb_dom_node_t *node;
        uint64_t mask;
        uint32_t stamp;
    } token_entries[STYLE_ANCESTOR_BLOOM_CACHE_CAPACITY];
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
_Static_assert(STYLE_RETAINED_MATCH_CAPACITY <= UINT16_MAX,
               "Retained match slots must fit index + 1 in uint16_t");
typedef struct {
    const lxb_dom_node_t *node;
    uint16_t rules[STYLE_RETAINED_MATCH_RULE_LIMIT];
    uint8_t count;
    uint8_t pseudo;
    uint16_t slot; /* Hash bucket while live, next free record otherwise. */
} StyleRetainedMatchEntry;
typedef struct StyleRetainedMatches {
    Budget *budget;
    /* Keep the original hash/probe space, but reserve payload only for
       populated slots. Zero is empty; other values are dense index + 1. */
    uint16_t *slots;
    StyleRetainedMatchEntry *entries;
    size_t entry_count, entry_capacity;
    uint16_t free_entry;
    bool growth_refused;
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
/* Match lists cannot retain geometry-dependent query admission. Use the
   same gate for allocation and lookup; eligibility can change with CSS. */
bool style_retained_matches_supported(const Stylesheet *sheet);
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
/* Drops `node`'s own lists (element and ::before/::after) by direct probe. */
void style_retained_matches_forget_node(StyleRetainedMatches *table,
                                        const lxb_dom_node_t *node);
/* Drops every list whose element `selects` answers true for; returns how
   many went. Bounded by the table; a cancelled scan drops every list. */
size_t style_retained_matches_drop_selected(
    StyleRetainedMatches *table,
    bool (*selects)(void *opaque, const lxb_dom_node_t *node), void *opaque);

/* ---- Scoped :has() invalidation (style_has_invalidation.c). -----------

   A :has() answer at an anchor element A reads A's descendants (a
   descendant or child argument) or A's later siblings and their subtrees
   (a sibling argument). A change at N can therefore only move the answers
   of anchors that are N's ancestors, or earlier siblings of N or of an
   ancestor (every sibling after a structural change, whose indices move).
   Each :has() of a rule is classified by where its anchor lies relative to
   the rule's subject: the subject itself, an ancestor of it, or anything
   else (sibling combinators outside the :has()); an argument that can read
   outside that region (an ancestor combinator inside :is(), :disabled, an
   `of S` among siblings) also covers the changed element's subtree.
   Scoped invalidation then drops exactly the cached styles and retained
   rule lists of the elements whose match can move: anchors found by that
   walk that carry the subject key, subjects under anchors found by it, and
   by subject key alone otherwise. A rule that cannot be keyed or located
   leaves the caller's full reset as the fallback. */
typedef struct StyleHasPlan StyleHasPlan;
#define STYLE_HAS_PLAN_LIMIT 2048u
#define STYLE_HAS_ROOT_LIMIT 16u
#define STYLE_HAS_WALKED_LIMIT 48u
typedef struct {
    /* Entries whose subjects are selected by key under `roots`, and by key
       anywhere. */
    uint32_t under_roots[STYLE_HAS_PLAN_LIMIT / 32u];
    uint32_t anywhere[STYLE_HAS_PLAN_LIMIT / 32u];
    const lxb_dom_node_t *roots[STYLE_HAS_ROOT_LIMIT];
    uint8_t root_count;
    /* Past STYLE_HAS_ROOT_LIMIT roots, `under_roots` means anywhere. */
    bool roots_everywhere;
    /* Some marked entry is an author rule (custom-property rules are never
       in the retained lists, only in computed styles). */
    bool author_marked;
    /* A relevant rule neither the walk nor a key can place: every computed
       style goes, and the retained lists those rules can select, found by
       matching them (style_retained_matches_invalidate_rules). */
    bool styles_all;
    uint8_t unplaced_rule_count;
    uint32_t unplaced_rules[8];
    /* Custom properties (stylesheet_custom_property_name_bits) a relevant
       custom-property rule declares. A subject passes them only to itself
       and its descendants, so a style that read one goes when one of its
       ancestors-or-self is a changed subject: one the walk dropped
       (`name_roots`, all of them past the limit) or one selected by key
       (style_has_pending_selects). */
    uint64_t custom_names;
    const lxb_dom_node_t *name_roots[STYLE_HAS_ROOT_LIMIT];
    uint8_t name_root_count;
    bool names_everywhere;
    /* Elements a walk with every entry relevant has visited, with their
       earlier siblings and ancestors: a later walk stops there (a turn's
       records share most of their ancestors). */
    const lxb_dom_node_t *walked[STYLE_HAS_WALKED_LIMIT];
    uint8_t walked_count;
    /* Some entry is marked in `under_roots` or `anywhere`. */
    bool marked;
    bool active;
    /* The plan the marks index; another one selects everything. */
    const StyleHasPlan *plan;
} StyleHasPending;
typedef struct {
    size_t visits;
    size_t drops;
    /* Changes that had to wipe every computed style (styles_all). */
    size_t wipes;
} StyleHasNoteStats;
typedef void (*StyleHasDrop)(void *opaque, lxb_dom_node_t *node);
/* Identity dependencies of a whole sheet (author and custom-property
   rules): `nonlocal` receives each class/id token hash a selector reads
   off its subject (outside :has() arguments), with `subject_key` set to a
   key the rule's subject must carry (style_element_carries_key; 0 when it
   need carry none) and `sibling` when the selector can reach the subject
   across siblings (a sibling combinator or an :nth-* pseudo-class);
   `declares`, with `names` set to the declared custom property's name
   bits, each token of a custom-property rule. False when some identity
   cannot be named. */
typedef void (*StyleIdentityTokenVisit)(void *context, uint32_t hash);
typedef struct {
    StyleIdentityTokenVisit nonlocal;
    StyleIdentityTokenVisit declares;
    void *context;
    uint64_t names;
    uint32_t subject_key;
    bool sibling;
    /* Set when some attribute selector's name cannot be hashed. */
    bool attributes_opaque;
} StyleIdentityScan;
/* A custom property a rule declares through a sibling or structural test
   (a sibling combinator, :nth-*, :first-/:last-/:only-, :empty): name bits
   (stylesheet_custom_property_name_bits) and a key its subject carries
   (style_element_carries_key; 0: any element). A child-list or attribute
   change can change it on siblings of the changed element. */
typedef struct {
    uint32_t key;
    uint64_t names;
} StyleKeyNames;
/* Calls `visit` with each such rule; false when it cannot list them. */
typedef bool (*StyleKeyNamesVisit)(void *context, StyleKeyNames entry);
bool stylesheet_structural_custom_rules(const Stylesheet *sheet,
                                        StyleKeyNamesVisit visit,
                                        void *context);
/* The names of `table` (sorted by key, `count` rows) an element's keys
   select. */
uint64_t style_element_key_names(const lxb_dom_node_t *node,
                                 const StyleKeyNames *table, size_t count);
/* The identity hash an attribute selector's name gets in the scan above
   (ASCII case folded; distinct from class and id hashes). */
uint32_t stylesheet_identity_attribute_hash(const char *name, size_t length);
bool stylesheet_identity_scan(const Stylesheet *sheet,
                              StyleIdentityScan *scan);
/* A change at `node` that a :has() rule may observe: its own state
   (`structure` false: an attribute, focus, inline style or control state,
   named by `attribute` or, for exact class/id changes, by the identity
   `tokens`; neither means any), or its place in the tree or its children
   (`structure` true: child-list, text and innerHTML changes, called with
   the inserted or removed node or the parent whose children changed).
   Elements whose answers can move are passed to `drop` now; the rest are
   recorded in `pending` for style_has_pending_selects. False when the
   change cannot be bounded, and the caller must reset its caches. */
bool style_has_note_change(const Stylesheet *sheet, lxb_dom_node_t *node,
                           bool structure, const char *attribute,
                           const uint32_t *tokens, size_t token_count,
                           StyleHasPending *pending, StyleHasDrop drop,
                           void *opaque, StyleHasNoteStats *stats);
/* Whether a cached entry of `node` must go for the pending changes. */
bool style_has_pending_selects(const Stylesheet *sheet,
                               const StyleHasPending *pending,
                               const lxb_dom_node_t *node);
/* The bit a plan entry sets in a probe's entry summary (below). */
static inline uint64_t style_has_entry_bloom_bit(size_t index)
{
    return UINT64_C(1) << ((((uint32_t) index + 1u) * UINT32_C(2654435761))
                           >> 26);
}
/* style_has_note_change for a tree change at `node` (or under it, for a
   removal's parent), considering only the entries in `entries`, the
   summary stylesheet_tree_change_has_entries gave while the change was
   visible (UINT64_MAX: all; from another plan build: all). */
bool style_has_note_structure(const Stylesheet *sheet, lxb_dom_node_t *node,
                              uint64_t entries, uint32_t serial,
                              StyleHasPending *pending, StyleHasDrop drop,
                              void *opaque, StyleHasNoteStats *stats);
/* Whether `node` or an ancestor is a subject the walk dropped for a rule
   declaring custom properties (see StyleHasPending.name_roots). */
bool style_has_pending_names_reach(const StyleHasPending *pending,
                                   const lxb_dom_node_t *node);
/* Forget roots inside a subtree about to be freed. */
void style_has_pending_retire(StyleHasPending *pending,
                              const lxb_dom_node_t *root);
/* The key (class, id, tag or attribute hash; 0 when none) every element
   matched by the compound containing `at` carries, looking through the
   logical pseudo-classes it sits in as their only compound; and whether an
   element carries such a key. Layout keys its :empty scope with these. */
uint32_t style_selector_compound_key_at(const char *text, size_t length,
                                        size_t at);
bool style_element_carries_key(const lxb_dom_node_t *node, uint32_t key);
/* Whether it carries one of `keys` (sorted ascending, `count` of them). */
bool style_element_carries_any_key(const lxb_dom_node_t *node,
                                   const uint32_t *keys, size_t count);
/* The same for an unsorted `keys` list (a linear search per element key):
   the bridge's structure keys keep a parallel predecessor array. */
bool style_element_carries_any_key_unsorted(const lxb_dom_node_t *node,
                                            const uint32_t *keys,
                                            size_t count);
/* Whether a sibling-position test (positional pseudo-class or sibling
   combinator) precedes a descendant or child combinator in a selector,
   outside :has() arguments: only then can a changed child list restyle a
   sibling's descendants rather than the siblings themselves, whose
   descendants re-key off their parent's style. `keys` name the children
   such a test can concern (accumulated over selectors): a current child
   carrying one, or, for `predecessor` keys, the child that left. */
#define STYLE_STRUCTURE_KEY_LIMIT 16u
typedef struct {
    uint32_t keys[STYLE_STRUCTURE_KEY_LIMIT];
    bool predecessor[STYLE_STRUCTURE_KEY_LIMIT];
    uint8_t count;
    bool reaches;
    /* A test no key bounds: every child list reaches descendants. */
    bool any;
} StyleStructureKeys;
void style_selector_structure_keys(const char *selector, size_t length,
                                   StyleStructureKeys *keys);
/* Where a test counting an element's position from the end of its
   siblings (:last-*, :nth-last-*, :only-*, outside :has() arguments) can
   sit: appending a later sibling changes only those answers. Each entry is
   a key carried by the element holding the test (`depth` 0) or, when that
   compound has none, by its ancestor `depth` levels up (`at_least`: that
   level or any above it, through a descendant combinator). `any` when some
   test cannot be placed by a key. */
#define STYLE_TRAILING_KEY_LIMIT 64u
typedef struct {
    uint32_t keys[STYLE_TRAILING_KEY_LIMIT];
    uint8_t depth[STYLE_TRAILING_KEY_LIMIT];
    bool at_least[STYLE_TRAILING_KEY_LIMIT];
    uint8_t count;
    bool any;
} StyleTrailingKeys;
void style_selector_trailing_keys(const char *selector, size_t length,
                                  StyleTrailingKeys *keys);
/* What a :has() argument can see of a newly inserted subtree, without the
   :has() plan (which stops at STYLE_HAS_PLAN_LIMIT entries). A plain
   argument (every compound keyed, no positional, :empty or :blank test,
   no sibling combinator) changes its answer only through a match whose
   rightmost element is in the new subtree and whose leftmost element is in
   it or above it: the keys of its first and last compounds go into two
   2048-bit Bloom sets. Any other argument can also change through the new
   subtree's position, so only at anchors related to it: the key of the
   compound holding the :has() goes into `anchors`, with the region its
   argument reaches from there (STYLE_HAS_REGION_*). `any` when such an
   anchor has no key, sits in a nested :has() argument, or the anchors
   overflow. */
#define STYLE_HAS_ARGUMENT_BLOOM_WORDS 64u
#define STYLE_HAS_ANCHOR_LIMIT 64u
typedef struct {
    uint32_t first[STYLE_HAS_ARGUMENT_BLOOM_WORDS];
    uint32_t last[STYLE_HAS_ARGUMENT_BLOOM_WORDS];
    uint32_t anchors[STYLE_HAS_ANCHOR_LIMIT];
    uint8_t anchor_regions[STYLE_HAS_ANCHOR_LIMIT];
    uint8_t anchor_count;
    bool any;
} StyleHasArgumentKeys;
void style_selector_has_argument_keys(const char *selector, size_t length,
                                      StyleHasArgumentKeys *keys);
/* Whether a tree change under element `parent` may move a :has() answer
   `keys` describe. `subtree` is the inserted (in place) or removed
   (detached) subtree, or NULL when only text or children whose keys are
   not at hand changed and no element joined or left (then no plain
   argument can move). Moves: an element of `subtree` may carry a plain
   argument's last key while it or `parent` or above carries a first key;
   an anchor key sits on `parent` or above (descendant region) or on an
   earlier sibling of `subtree` (any child of `parent` when `subtree` is not
   in place) or of `parent` or above (sibling region). True when `keys`
   cannot tell. */
bool style_has_argument_keys_reach(const StyleHasArgumentKeys *keys,
                                   const lxb_dom_node_t *subtree,
                                   const lxb_dom_node_t *parent);
/* Whether an element whose later siblings changed (`sibling`, under
   `parent`) can carry a test of `keys`. */
bool style_trailing_keys_reach(const StyleTrailingKeys *keys,
                               const lxb_dom_node_t *sibling,
                               const lxb_dom_node_t *parent);
/* Whether a tree change inside the document's `head` can move a :has()
   answer that styles an element outside it. An argument reads its anchor's
   descendants or later siblings, so such a change moves only answers
   anchored at the head, at its <html> parent, or inside the head, and an
   anchor inside the head styles only elements inside it (relation self,
   ancestor or sibling alike). False only when neither the head nor <html>
   can be any :has() anchor of the sheet: keyed anchors are compared with
   their keys, unkeyed ones matched as the selector text before the :has().
   Given the changed `node` (connected), only the :has() arguments that
   can see it count. True when the plan is bounded. */
bool stylesheet_head_change_reaches_outside(const Stylesheet *sheet,
                                            lxb_dom_node_t *head,
                                            lxb_dom_node_t *node);
/* Diagnostics and tests: the classified entries (built on demand). */
typedef enum {
    STYLE_HAS_RELATION_SELF = 0,
    STYLE_HAS_RELATION_ANCESTOR,
    STYLE_HAS_RELATION_OTHER
} StyleHasRelation;
enum {
    STYLE_HAS_REGION_DESCENDANTS = 1,
    STYLE_HAS_REGION_SIBLINGS = 2
};
typedef struct {
    uint32_t rule;
    StyleHasRelation relation;
    uint8_t region;
    bool escapes;
    bool subject_keyed;
    bool anchor_keyed;
} StyleHasPlanSummary;
#define STYLE_HAS_RULE_CUSTOM UINT32_C(0x80000000)
size_t style_has_plan_summarize(const Stylesheet *sheet,
                                StyleHasPlanSummary *out, size_t capacity,
                                bool *bounded);
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
