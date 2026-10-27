/* The retained matched-rule table: per-element lists of the rules that
   matched at the last resolution, replayed instead of re-matching, and the
   invalidation that drops lists a DOM or rule change could make stale.
   Split out of style_resolve.c; the cascade records into it through
   style_retained_begin/commit and reads it through style_retained_lookup. */

#include "style_budget_category.h"
#include "style_internal.h"
#include "style_cache_internal.h"
#include "tilefinch/platform.h"

#include <string.h>

/* ---- Retained matched-rule table (see style_internal.h). ---------------- */

StyleRetainedMatches *style_retained_matches_create(Budget *budget)
{
    if (budget == NULL) return NULL;
    StyleRetainedMatches *table = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*table));
    if (table == NULL) return NULL;
    table->slots = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, STYLE_RETAINED_MATCH_CAPACITY,
        sizeof(*table->slots));
    if (table->slots == NULL) {
        budget_free(budget, table);
        return NULL;
    }
    table->budget = budget;
    table->free_entry = UINT16_MAX;
    return table;
}

void style_retained_matches_destroy(StyleRetainedMatches *table)
{
    if (table == NULL) return;
    Budget *budget = table->budget;
    budget_free(budget, table->slots);
    budget_free(budget, table->entries);
    memset(table, 0, sizeof(*table));
    budget_free(budget, table);
}

size_t style_retained_matches_bytes(const StyleRetainedMatches *table)
{
    if (table == NULL) return 0;
    return sizeof(*table)
           + STYLE_RETAINED_MATCH_CAPACITY * sizeof(*table->slots)
           + table->entry_capacity * sizeof(*table->entries);
}

void style_retained_matches_clear(StyleRetainedMatches *table)
{
    if (table == NULL || table->slots == NULL) return;
    table->growth_refused = false;
    if (table->entry_count == 0) return;
    memset(table->slots, 0,
           STYLE_RETAINED_MATCH_CAPACITY * sizeof(*table->slots));
    table->occupied = 0;
    table->entry_count = 0;
    table->free_entry = UINT16_MAX;
}

static StyleRetainedMatchEntry *style_retained_slot(
    const StyleRetainedMatches *table, size_t slot)
{
    uint16_t index = table->slots[slot];
    return index == 0 ? NULL : &table->entries[index - 1u];
}

static void style_retained_remove(StyleRetainedMatches *table,
                                  StyleRetainedMatchEntry *entry)
{
    table->slots[entry->slot] = 0;
    size_t index = (size_t) (entry - table->entries);
    memset(entry, 0, sizeof(*entry));
    entry->slot = table->free_entry;
    table->free_entry = (uint16_t) index;
    table->occupied--;
}

static StyleRetainedMatchEntry *style_retained_allocate(
    StyleRetainedMatches *table, size_t slot)
{
    size_t index;
    if (table->free_entry != UINT16_MAX) {
        index = table->free_entry;
        table->free_entry = table->entries[index].slot;
    } else {
        if (table->entry_count == table->entry_capacity) {
            if (table->growth_refused) return NULL;
            size_t capacity = table->entry_capacity == 0
                ? 128u : table->entry_capacity * 2u;
            if (capacity > STYLE_RETAINED_MATCH_CAPACITY) return NULL;
            StyleRetainedMatchEntry *entries = budget_realloc_category(
                table->budget, BUDGET_CATEGORY_LAYOUT, table->entries,
                capacity * sizeof(*entries));
            /* Optional memoization: refusal changes neither the computed
               answer nor any existing key/payload association. */
            if (entries == NULL) {
                table->growth_refused = true;
                return NULL;
            }
            table->entries = entries;
            table->entry_capacity = capacity;
        }
        index = table->entry_count++;
    }
    StyleRetainedMatchEntry *entry = &table->entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->slot = (uint16_t) slot;
    table->slots[slot] = (uint16_t) (index + 1u);
    table->occupied++;
    return entry;
}

StyleRetainedMatches *style_retained_matches_attach(
    const Stylesheet *sheet, StyleRetainedMatches *table)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return NULL;
    StyleRetainedMatches *previous = sheet->resolve_scratch->retained_matches;
    sheet->resolve_scratch->retained_matches = table;
    return previous;
}

static bool style_retained_node_within(const lxb_dom_node_t *node,
                                       const lxb_dom_node_t *ancestor)
{
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at == ancestor) return true;
    }
    return false;
}

static size_t style_retained_home(const lxb_dom_node_t *node,
                                  PseudoElement pseudo);

static void style_retained_forget_node(StyleRetainedMatches *table,
                                       const lxb_dom_node_t *node)
{
    static const PseudoElement pseudos[] = {
        PSEUDO_NONE, PSEUDO_BEFORE, PSEUDO_AFTER, PSEUDO_PLACEHOLDER
    };
    for (size_t p = 0; p < sizeof(pseudos) / sizeof(pseudos[0]); p++) {
        size_t home = style_retained_home(node, pseudos[p]);
        for (size_t probe = 0; probe < STYLE_RETAINED_MATCH_PROBE_LIMIT;
             probe++) {
            StyleRetainedMatchEntry *entry = style_retained_slot(table,
                (home + probe) & (STYLE_RETAINED_MATCH_CAPACITY - 1u));
            /* Invalidation leaves holes and reinsertion can duplicate a key.
               Retirement must clear the entire bounded probe, not just the
               prefix visible to lookup, before the DOM storage is freed. */
            if (entry != NULL && entry->node == node
                && entry->pseudo == (uint8_t) pseudos[p]) {
                style_retained_remove(table, entry);
            }
        }
    }
}

void style_retained_matches_forget_node(StyleRetainedMatches *table,
                                        const lxb_dom_node_t *node)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0
        || node == NULL) return;
    style_retained_forget_node(table, node);
}

size_t style_retained_matches_drop_selected(
    StyleRetainedMatches *table,
    bool (*selects)(void *opaque, const lxb_dom_node_t *node), void *opaque)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0
        || selects == NULL) return 0;
    size_t dropped = 0;
    for (size_t i = 0; i < table->entry_count; i++) {
        /* As in style_retained_matches_invalidate_rules: dropping every
           list is always correct, so a cancelled scan does that. */
        if ((i & 2047u) == 2047u
            && !tilefinch_platform_cooperate("style-invalidate", i)) {
            dropped += table->occupied;
            style_retained_matches_clear(table);
            return dropped;
        }
        StyleRetainedMatchEntry *entry = &table->entries[i];
        if (entry->node == NULL || !selects(opaque, entry->node)) continue;
        style_retained_remove(table, entry);
        dropped++;
    }
    return dropped;
}

void style_retained_matches_forget_subtree(
    StyleRetainedMatches *table, const lxb_dom_node_t *root)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0
        || root == NULL) return;
    const lxb_dom_node_t *node = root;
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            style_retained_forget_node(table, node);
            if (table->occupied == 0) return;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL) {
            node = node->parent;
        }
        node = node == NULL || node == root ? NULL : node->next;
    }
}

void style_retained_matches_invalidate_within(
    StyleRetainedMatches *table, const lxb_dom_node_t *scope)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0) return;
    if (scope == NULL) {
        style_retained_matches_clear(table);
        return;
    }
    /* Entries are keyed by element, so the entries inside a scope are those
       of its current subtree. Forgetting each by direct probe (three
       pseudo keys, bounded probes) beats sweeping the whole table while the
       subtree is small; a large one keeps the sweep. */
    enum { DIRECT_LIMIT = STYLE_RETAINED_MATCH_CAPACITY
                          / (3u * STYLE_RETAINED_MATCH_PROBE_LIMIT) };
    size_t elements = 0;
    const lxb_dom_node_t *walk = scope;
    while (walk != NULL && elements <= DIRECT_LIMIT) {
        if (walk->type == LXB_DOM_NODE_TYPE_ELEMENT) elements++;
        if (walk->first_child != NULL) {
            walk = walk->first_child;
            continue;
        }
        while (walk != NULL && walk != scope && walk->next == NULL)
            walk = walk->parent;
        walk = walk == NULL || walk == scope ? NULL : walk->next;
    }
    if (elements <= DIRECT_LIMIT) {
        style_retained_matches_forget_subtree(table, scope);
        return;
    }
    for (size_t i = 0; i < table->entry_count; i++) {
        StyleRetainedMatchEntry *entry = &table->entries[i];
        if (entry->node != NULL
            && style_retained_node_within(entry->node, scope)) {
            style_retained_remove(table, entry);
        }
    }
}

void style_retained_matches_invalidate_ancestors(
    StyleRetainedMatches *table, const lxb_dom_node_t *node)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0) return;
    if (node == NULL) {
        style_retained_matches_clear(table);
        return;
    }
    /* The entries of `node` and its ancestors, by direct probe along the
       ancestor chain rather than a sweep of the whole table. */
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        style_retained_forget_node(table, at);
        if (table->occupied == 0) return;
    }
}

static size_t style_retained_home(const lxb_dom_node_t *node,
                                  PseudoElement pseudo)
{
    uintptr_t bits = ((uintptr_t) node >> 4)
                     + (uintptr_t) pseudo * (uintptr_t) UINT32_C(0x9e3779b1);
    bits *= (uintptr_t) UINT32_C(2654435761);
    return (size_t) (bits ^ (bits >> 15))
           & (STYLE_RETAINED_MATCH_CAPACITY - 1u);
}

bool style_retained_matches_supported(const Stylesheet *sheet)
{
    return sheet != NULL && sheet->count <= UINT16_MAX;
}

static bool style_retained_usable(const Stylesheet *sheet)
{
    return style_retained_matches_supported(sheet)
           && sheet->resolve_scratch != NULL
           && sheet->resolve_scratch->retained_matches != NULL
           && sheet->resolve_scratch->retained_matches->slots != NULL;
}

const StyleRetainedMatchEntry *style_retained_lookup(
    const Stylesheet *sheet, const lxb_dom_node_t *node,
    PseudoElement pseudo)
{
    if (node == NULL || !style_retained_usable(sheet)) return NULL;
    StyleRetainedMatches *table = sheet->resolve_scratch->retained_matches;
    size_t home = style_retained_home(node, pseudo);
    for (size_t probe = 0; probe < STYLE_RETAINED_MATCH_PROBE_LIMIT; probe++) {
        const StyleRetainedMatchEntry *entry = style_retained_slot(table,
            (home + probe) & (STYLE_RETAINED_MATCH_CAPACITY - 1u));
        if (entry == NULL) break;
        if (entry->node == node && entry->pseudo == (uint8_t) pseudo) {
            table->hits++;
            return entry;
        }
    }
    table->misses++;
    return NULL;
}

/* A retained pseudo-element list generates no box when no matched rule can
   supply `content` (mirrors style_pseudo_known_absent). */
bool style_retained_pseudo_absent(const Stylesheet *sheet,
                                  const StyleRetainedMatchEntry *entry)
{
    for (size_t i = 0; i < entry->count; i++) {
        size_t index = entry->rules[i];
        if (index >= sheet->count) return false;
        const StyleDeclaration *declaration = stylesheet_rule_declaration(
            sheet, &sheet->rules[index]);
        if (declaration != NULL
            && ((declaration->inherit_mask & S_CONTENT) != 0
                || ((declaration->mask & S_CONTENT) != 0
                    && (declaration->values_flags
                        & STYLE_DECLARATION_VALUES_GENERATED_CONTENT) != 0)
                || declaration->deferred_declarations != NULL)) return false;
    }
    return true;
}

void style_retained_matches_invalidate_tokens(
    StyleRetainedMatches *table, const Stylesheet *sheet,
    const lxb_dom_node_t *const *nodes, size_t node_count,
    const uint32_t *hashes, size_t hash_count, bool parent_scope)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0) return;
    uint32_t affected[STYLE_RETAINED_AFFECTED_RULE_LIMIT];
    size_t affected_count = sheet == NULL || sheet->count > UINT16_MAX
        ? SIZE_MAX
        : stylesheet_rules_affected_by_tokens(
              sheet, hashes, hash_count, affected,
              STYLE_RETAINED_AFFECTED_RULE_LIMIT);
    if (affected_count == SIZE_MAX) {
        table->token_fallbacks++;
        for (size_t i = 0; i < node_count; i++) {
            const lxb_dom_node_t *scope = nodes[i];
            if (scope == NULL) continue;
            if (parent_scope && scope->parent != NULL) scope = scope->parent;
            style_retained_matches_invalidate_within(table, scope);
        }
        return;
    }
    table->token_invalidations++;
    table->token_affected_rules += affected_count;
    for (size_t i = 0; i < table->entry_count; i++) {
        StyleRetainedMatchEntry *entry = &table->entries[i];
        if (entry->node == NULL) continue;
        bool drop = false;
        for (size_t n = 0; !drop && n < node_count; n++) {
            drop = entry->node == nodes[n];
        }
        if (drop) {
            style_retained_remove(table, entry);
            table->token_dropped++;
        }
    }
    style_retained_matches_invalidate_rules(
        table, sheet, affected, affected_count);
}

static uint32_t style_retained_key_hash(unsigned type, const char *key,
                                        size_t length)
{
    return style_fnv1a(STYLE_FNV1A_SEED ^ type, key, length,
                       type == SELECTOR_TAG)
        | 1u; /* 0 marks an empty slot */
}

static bool style_retained_key_present(const uint32_t *set, size_t mask,
                                       uint32_t hash)
{
    for (size_t at = hash & mask;; at = (at + 1u) & mask) {
        if (set[at] == hash) return true;
        if (set[at] == 0) return false;
    }
}

/* The fast keys (id, class or tag of the rightmost compound) of `rules`,
   hashed into an open-addressed set of 2^n slots at most half full. NULL
   when a rule has no fast key or the set cannot be allocated. A colliding
   key only drops a list that could have been kept. */
static uint32_t *style_retained_fast_key_set(
    const Stylesheet *sheet, const uint32_t *rules, size_t count,
    size_t *mask)
{
    size_t slots = 16;
    while (slots < count * 2u) slots *= 2u;
    uint32_t *set = budget_calloc(sheet->budget, slots, sizeof(*set));
    if (set == NULL) return NULL;
    *mask = slots - 1u;
    for (size_t a = 0; a < count; a++) {
        const StyleRule *rule = &sheet->rules[rules[a]];
        const char *key = rule->has_fast_key ? style_rule_fast_key(rule)
                                             : NULL;
        if (key == NULL) {
            budget_free(sheet->budget, set);
            return NULL;
        }
        /* rule_fast_matches reads any other keyed type as a tag. */
        unsigned type = rule->type == SELECTOR_ID ? SELECTOR_ID
            : rule->type == SELECTOR_CLASS ? SELECTOR_CLASS : SELECTOR_TAG;
        uint32_t hash = style_retained_key_hash(
            type, key, rule->fast_key_length);
        size_t at = hash & *mask;
        while (set[at] != 0 && set[at] != hash) at = (at + 1u) & *mask;
        set[at] = hash;
    }
    return set;
}

/* Whether the subject carries any key of the set: the per-entry answer of
   rule_fast_matches over every rule the set was built from. */
static bool style_retained_subject_in_key_set(
    const StyleMatchSubject *subject, const uint32_t *set, size_t mask)
{
    if (subject->id != NULL && style_retained_key_present(set, mask,
            style_retained_key_hash(SELECTOR_ID, subject->id,
                                    subject->id_length))) return true;
    if (subject->tag != NULL && style_retained_key_present(set, mask,
            style_retained_key_hash(SELECTOR_TAG, subject->tag,
                                    subject->tag_length))) return true;
    const char *classes = subject->classes;
    size_t length = subject->classes == NULL ? 0 : subject->classes_length;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) classes[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) classes[end])) end++;
        if (end > at && style_retained_key_present(set, mask,
                style_retained_key_hash(SELECTOR_CLASS, classes + at,
                                        end - at))) return true;
        at = end;
    }
    return false;
}

void style_retained_matches_invalidate_rules(
    StyleRetainedMatches *table, const Stylesheet *sheet,
    const uint32_t *rules, size_t count)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0
        || sheet == NULL || count == 0) return;
    /* Keyless rules (`*`, attribute- or pseudo-only rightmost compounds)
       can select any element by their fast key alone. `:root` can only
       select the document element, and late module sheets commonly open
       with it to publish custom properties. A few others (`.panel > *`)
       are answered exactly below; past that bound the whole table goes. */
    enum { KEYLESS_EXACT_LIMIT = 8 };
    bool root_only = false;
    size_t keyless = 0;
    for (size_t a = 0; a < count; a++) {
        if (rules[a] >= sheet->count) {
            table->token_dropped += table->occupied;
            style_retained_matches_clear(table);
            return;
        }
        const StyleRule *rule = &sheet->rules[rules[a]];
        if (rule->has_fast_key) continue;
        if (rule->selector != NULL && rule->selector_length == 5
            && memcmp(rule->selector, ":root", 5) == 0) {
            root_only = true;
            continue;
        }
        if (++keyless > KEYLESS_EXACT_LIMIT) {
            table->token_dropped += table->occupied;
            style_retained_matches_clear(table);
            return;
        }
    }
    /* A late module sheet brings thousands of rules: testing each against
       each list (2,275 x ~300 on chatgpt.com) cost 4 s on the PSP. Its fast
       keys go into one hashed set instead, looked up by the few keys each
       element carries; keyless rules keep the exact test below. */
    enum { KEY_SET_MINIMUM = 16 };
    size_t keyed = 0;
    for (size_t a = 0; a < count; a++)
        keyed += sheet->rules[rules[a]].has_fast_key ? 1u : 0u;
    uint32_t *keyed_rules = NULL, *key_set = NULL;
    size_t key_mask = 0;
    if (keyed >= KEY_SET_MINIMUM) {
        keyed_rules = budget_malloc(sheet->budget,
                                    keyed * sizeof(*keyed_rules));
        size_t at = 0;
        for (size_t a = 0; keyed_rules != NULL && a < count; a++) {
            if (sheet->rules[rules[a]].has_fast_key)
                keyed_rules[at++] = rules[a];
        }
        key_set = keyed_rules == NULL || at != keyed ? NULL
            : style_retained_fast_key_set(sheet, keyed_rules, keyed,
                                          &key_mask);
        budget_free(sheet->budget, keyed_rules);
    }
    for (size_t i = 0; i < table->entry_count; i++) {
        /* A full table is ~16k lists on a long article, 70 ms on the PSP.
           Dropping every list is always correct, so a cancelled scan does
           exactly that instead of holding input. */
        if ((i & 2047u) == 2047u
            && !tilefinch_platform_cooperate("style-invalidate", i)) {
            table->token_dropped += table->occupied;
            style_retained_matches_clear(table);
            budget_free(sheet->budget, key_set);
            return;
        }
        StyleRetainedMatchEntry *entry = &table->entries[i];
        if (entry->node == NULL) continue;
        StyleMatchSubject subject = {0};
        style_match_subject_prepare((lxb_dom_node_t *) entry->node, &subject);
        bool drop = root_only && entry->node->parent != NULL
            && entry->node->parent->type == LXB_DOM_NODE_TYPE_DOCUMENT;
        if (!drop && key_set != NULL)
            drop = style_retained_subject_in_key_set(
                &subject, key_set, key_mask);
        for (size_t a = 0; !drop && a < count; a++) {
            const StyleRule *rule = &sheet->rules[rules[a]];
            if (rule->has_fast_key) {
                drop = key_set == NULL
                    && rule_fast_matches(sheet, rule, &subject);
            } else if (!(rule->selector != NULL && rule->selector_length == 5
                         && memcmp(rule->selector, ":root", 5) == 0)) {
                /* Mutation can remove a match as well as create one. The
                   retained answer is the only evidence of the old DOM. */
                for (size_t k = 0; !drop && k < entry->count; k++)
                    drop = entry->rules[k] == rules[a];
                drop = drop || rule_matches(sheet, rules[a], rule,
                                    (lxb_dom_node_t *) entry->node, &subject);
            }
        }
        if (drop) {
            style_retained_remove(table, entry);
            table->token_dropped++;
        }
    }
    budget_free(sheet->budget, key_set);
}

bool style_rule_candidate_plan(const Stylesheet *sheet,
    const uint32_t *rules, size_t count, StyleRuleCandidatePlan *plan)
{
    if (count > 64u) return false;
    memset(plan, 0, sizeof(*plan));
    for (size_t i = 0; i < count; i++) {
        if (rules[i] >= sheet->count) return false;
        const StyleRule *rule = &sheet->rules[rules[i]];
        const char *key = rule->has_fast_key ? style_rule_fast_key(rule) : NULL;
        if (key == NULL) {
            plan->keyless[plan->keyless_count++] = rules[i];
            continue;
        }
        uint32_t hash = style_retained_key_hash(rule->type, key,
                                               rule->fast_key_length);
        size_t at = hash & 127u;
        while (plan->keys[at] != 0 && plan->keys[at] != hash)
            at = (at + 1u) & 127u;
        plan->keys[at] = hash;
    }
    return true;
}

bool style_rules_may_select_node(const Stylesheet *sheet,
    StyleRetainedMatches *table, const StyleRuleCandidatePlan *plan,
    lxb_dom_node_t *node)
{
    StyleMatchSubject subject = style_match_subject(sheet, node);
    if (style_retained_subject_in_key_set(&subject, plan->keys, 127u))
        return true;
    if (plan->keyless_count == 0) return false;
    const StyleRetainedMatchEntry *old = NULL;
    if (table != NULL && table->slots != NULL) {
        size_t home = style_retained_home(node, PSEUDO_NONE);
        for (size_t i = 0; i < STYLE_RETAINED_MATCH_PROBE_LIMIT; i++) {
            const StyleRetainedMatchEntry *entry = style_retained_slot(table,
                (home + i) & (STYLE_RETAINED_MATCH_CAPACITY - 1u));
            if (entry == NULL) break;
            if (entry->node == node && entry->pseudo == PSEUDO_NONE) {
                old = entry;
                break;
            }
        }
    }
    for (size_t i = 0; i < plan->keyless_count; i++) {
        uint32_t index = plan->keyless[i];
        const StyleRule *rule = &sheet->rules[index];
        /* The old list covers a removed focus match; the current DOM
           covers newly focused subjects. Missing evidence stays broad. */
        if (old == NULL || rule->pseudo != PSEUDO_NONE) return true;
        for (size_t j = 0; j < old->count; j++)
            if (old->rules[j] == index) return true;
        if (rule_matches(sheet, index, rule, node, &subject)) return true;
    }
    return false;
}

void style_retained_matches_remap(StyleRetainedMatches *table,
                                  const uint16_t *remap, size_t old_count)
{
    if (table == NULL || table->entries == NULL || table->occupied == 0) return;
    for (size_t i = 0; i < table->entry_count; i++) {
        StyleRetainedMatchEntry *entry = &table->entries[i];
        if (entry->node == NULL) continue;
        bool valid = remap != NULL;
        for (size_t k = 0; valid && k < entry->count; k++) {
            valid = entry->rules[k] < old_count
                    && remap[entry->rules[k]] != UINT16_MAX;
        }
        if (!valid) {
            style_retained_remove(table, entry);
            continue;
        }
        for (size_t k = 0; k < entry->count; k++) {
            entry->rules[k] = remap[entry->rules[k]];
        }
    }
}

bool style_retained_begin(const Stylesheet *sheet)
{
    if (!style_retained_usable(sheet)) return false;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->retained_pending_active) {
        /* A nested element resolution would interleave its matches with
           the outer element's list. Record neither. */
        scratch->retained_pending_valid = false;
        return false;
    }
    scratch->retained_pending_active = true;
    scratch->retained_pending_valid = true;
    scratch->retained_pending_count = 0;
    return true;
}

void style_retained_commit(const Stylesheet *sheet,
                           const lxb_dom_node_t *node,
                           PseudoElement pseudo)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch == NULL) return;
    bool valid = scratch->retained_pending_active
                 && scratch->retained_pending_valid
                 && !sheet->selector_cooperate_cancelled;
    scratch->retained_pending_active = false;
    scratch->retained_pending_valid = false;
    StyleRetainedMatches *table = scratch->retained_matches;
    if (!valid || node == NULL || table == NULL || table->slots == NULL) return;
    size_t home = style_retained_home(node, pseudo);
    size_t slot = home;
    for (size_t probe = 0; probe < STYLE_RETAINED_MATCH_PROBE_LIMIT; probe++) {
        slot = (home + probe) & (STYLE_RETAINED_MATCH_CAPACITY - 1u);
        const StyleRetainedMatchEntry *entry = style_retained_slot(table, slot);
        if (entry == NULL
            || (entry->node == node && entry->pseudo == (uint8_t) pseudo))
            break;
    }
    /* A full probe window replaces its last slot; every surviving entry is
       still exact for its own element, so a displaced one only misses. */
    StyleRetainedMatchEntry *entry = style_retained_slot(table, slot);
    if (entry == NULL) entry = style_retained_allocate(table, slot);
    if (entry == NULL) return;
    entry->node = node;
    entry->pseudo = (uint8_t) pseudo;
    entry->count = scratch->retained_pending_count;
    memcpy(entry->rules, scratch->retained_pending_rules,
           entry->count * sizeof(entry->rules[0]));
    table->stores++;
}
