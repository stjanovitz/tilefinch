/* Adopted (constructed) and shadow-tree style scopes: the lookups the
   matcher and the cascade ask (which adopted source holds a rule, whether
   a confined rule may apply to an element, and the adopted cascade
   order), and the builders of the tables they read: the shadow-scope
   ranges, the :host/::slotted() rewrite, and the adopted-scope root table
   and its diff. Split out of style_match.c and style_sheet.c; see
   style_scopes.h. */

#include "style_budget_category.h"
#include "style_internal.h"
#include "tilefinch_test_faults.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The adopted sources of the nearest shadow tree. An outer adopting root
   does not style descendants inside a nested tree, even if that tree has
   no adopted sheets of its own. Host/slotted subjects pass their carrier
   explicitly in style_shadow_scope_admits. */
static uint64_t style_node_adopted_scope(const Stylesheet *sheet,
                                         const lxb_dom_node_t *node)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    const StyleClassTokenCache *tokens = sheet->class_tokens;
    bool memo = scratch != NULL && tokens != NULL
        && scratch->class_tokens_depth != 0;
    if (memo && scratch->adopted_scope_node == node
        && scratch->adopted_scope_tokens_scope == tokens->scope)
        return scratch->adopted_scope_mask;
    uint64_t mask = 0;
    size_t depth = 0;
    for (const lxb_dom_node_t *at = node; at != NULL && depth < 4096u;
         at = at->parent, depth++) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        const StyleAdoptedScopeRoot *entry = style_adopted_scope_root_lookup(
            sheet->scopes.adopted_scope_roots, sheet->scopes.adopted_scope_root_count, at);
        if (entry != NULL) mask |= entry->mask;
        if (document_node_is_shadow_carrier(at)) break;
    }
    if (memo) {
        scratch->adopted_scope_node = node;
        scratch->adopted_scope_tokens_scope = tokens->scope;
        scratch->adopted_scope_mask = mask;
    }
    return mask;
}

uint64_t style_adopted_source_bit(const Stylesheet *sheet, unsigned order)
{
    size_t count = sheet->scopes.adopted_source_count;
    /* Orders stay valid while the sources are bounded out: retiring a
       source frees no rule, and an append (which renumbers) is refused
       until the rebuild. Admission then fails confined bits closed
       (style_adopted_scopes_document_only). */
    if (count == 0 || count > sheet->style_source_count
        || order >= sheet->scopes.adopted_rule_order_end) return 0;
    size_t base = sheet->style_source_count - count;
    const unsigned *first = &sheet->style_source_first_order[base];
    if (order < first[0]) return 0;
    /* The last adopted source starting at or before `order` holds it. */
    size_t low = 0, high = count;
    while (high - low > 1u) {
        size_t middle = low + (high - low) / 2u;
        if (first[middle] <= order) low = middle;
        else high = middle;
    }
    return low < 64u ? UINT64_C(1) << low : 0;
}

uint64_t style_subject_adopted_scope(const Stylesheet *sheet,
                                     const StyleMatchSubject *subject,
                                     const lxb_dom_node_t *node)
{
    if (node == NULL) return 0;
    if (subject != NULL && subject->node == node) {
        /* The subject is the caller's, prepared for this one element. */
        StyleMatchSubject *memo = (StyleMatchSubject *) subject;
        if (!memo->adopted_scope_known) {
            memo->adopted_scope = style_node_adopted_scope(sheet, node);
            memo->adopted_scope_known = true;
        }
        return memo->adopted_scope;
    }
    return style_node_adopted_scope(sheet, node);
}

bool style_adopted_scope_admits(const Stylesheet *sheet, unsigned order,
                                const lxb_dom_node_t *node,
                                const StyleMatchSubject *subject)
{
    uint64_t bit = style_adopted_source_bit(sheet, order);
    if (bit != 0 && style_adopted_scopes_document_only(sheet))
        return (sheet->scopes.adopted_document_mask & bit) != 0;
    if ((sheet->scopes.adopted_shadow_only_mask & bit) == 0) return true;
    return (style_subject_adopted_scope(sheet, subject, node) & bit) != 0;
}

uint64_t style_adopted_cascade_order(const Stylesheet *sheet,
                                    const lxb_dom_node_t *node, unsigned order)
{
    StyleAdoptedOrderContext context = {0};
    return style_adopted_cascade_order_in(sheet, node, &context, order);
}

uint64_t style_adopted_cascade_order_in(const Stylesheet *sheet,
                                        const lxb_dom_node_t *node,
                                        StyleAdoptedOrderContext *context,
                                        unsigned order)
{
    uint64_t bit = style_adopted_source_bit(sheet, order);
    if (bit == 0) return order;
    size_t source = 0;
    while ((bit >> source) != 1u) source++;
    unsigned rank = (unsigned) source + 1u;
    if (style_adopted_scopes_document_only(sheet)) {
        if (sheet->scopes.adopted_document_order[source] != 0)
            rank = sheet->scopes.adopted_document_order[source];
        size_t base = sheet->style_source_count - sheet->scopes.adopted_source_count;
        return ((uint64_t) rank << 32)
            | (order - sheet->style_source_first_order[base + source]);
    }
    if (!context->ready) {
        /* Nearest adopting carrier first; :host/::slotted subjects may
           instead refer to the carrier attached to the element or its
           parent. Roots are unique, and the document's (NULL) sorts first. */
        const lxb_dom_node_t *carrier =
            document_shadow_carrier_containing(node);
        if (carrier == NULL) carrier = document_shadow_carrier_of_host(node);
        if (carrier == NULL && node != NULL)
            carrier = document_shadow_carrier_of_host(node->parent);
        context->carrier = carrier == NULL ? NULL
            : style_adopted_scope_root_lookup(sheet->scopes.adopted_scope_roots,
                  sheet->scopes.adopted_scope_root_count, carrier);
        context->document = sheet->scopes.adopted_scope_root_count != 0
            && sheet->scopes.adopted_scope_roots[0].root == NULL
            ? &sheet->scopes.adopted_scope_roots[0] : NULL;
        context->ready = true;
    }
    if (context->carrier != NULL && context->carrier->order[source] != 0)
        rank = context->carrier->order[source];
    else if (context->document != NULL
             && context->document->order[source] != 0)
        rank = context->document->order[source];
    size_t base = sheet->style_source_count - sheet->scopes.adopted_source_count;
    return ((uint64_t) rank << 32)
        | (order - sheet->style_source_first_order[base + source]);
}

typedef enum {
    STYLE_SHADOW_RULE_INSIDE,
    STYLE_SHADOW_RULE_HOST,
    STYLE_SHADOW_RULE_SLOTTED
} StyleShadowRuleKind;

/* Where a shadow-confined selector's subject lives, read from its text as
   style_shadow_selector_rewrite left it: a rightmost compound beginning
   with :host targets the host; the rewritten `:host > :is(X)` form of
   ::slotted() targets the host's light children. */
static StyleShadowRuleKind style_shadow_rule_kind(const char *selector,
                                                  size_t length)
{
    if (selector == NULL || length < 5) return STYLE_SHADOW_RULE_INSIDE;
    size_t rightmost = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < length; i++) {
        char c = selector[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < length) i++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth > 0) depth--;
        else if (depth == 0 && (c == ' ' || c == '>' || c == '+'
                                || c == '~')) rightmost = i + 1;
    }
    if (length - rightmost >= 5
        && memcmp(selector + rightmost, ":host", 5) == 0
        && (length - rightmost == 5
            || !(isalnum((unsigned char) selector[rightmost + 5])
                 || selector[rightmost + 5] == '-'))) {
        return STYLE_SHADOW_RULE_HOST;
    }
    if (length > 12 && memcmp(selector, ":host > :is(", 12) == 0
        && rightmost == 8) {
        return STYLE_SHADOW_RULE_SLOTTED;
    }
    return STYLE_SHADOW_RULE_INSIDE;
}

static const StyleShadowScope *style_shadow_scope_for_order(
    const Stylesheet *sheet, unsigned order)
{
    size_t low = 0, high = sheet->scopes.shadow_scope_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const StyleShadowScope *scope = &sheet->scopes.shadow_scopes[middle];
        if (order < scope->begin) high = middle;
        else if (order >= scope->end) low = middle + 1u;
        else return scope;
    }
    return NULL;
}

bool style_subject_near_shadow_tree(const StyleMatchSubject *subject)
{
    const lxb_dom_node_t *node = subject == NULL ? NULL : subject->node;
    return node != NULL
        && (document_shadow_carrier_of_host(node) != NULL
            || (node->parent != NULL
                && document_shadow_carrier_of_host(node->parent) != NULL));
}

bool style_shadow_scope_admits(const Stylesheet *sheet, unsigned order,
                               const char *selector, size_t selector_length,
                               const lxb_dom_node_t *node,
                               const StyleMatchSubject *subject)
{
    bool document_only = style_adopted_scopes_document_only(sheet);
    if (document_only
        && (style_adopted_source_bit(sheet, order)
            & ~sheet->scopes.adopted_document_mask) != 0) return false;
    uint64_t bit = document_only
        || sheet->scopes.adopted_shadow_only_mask == 0 ? 0
        : style_adopted_source_bit(sheet, order)
          & sheet->scopes.adopted_shadow_only_mask;
    const StyleShadowScope *scope = bit != 0 || sheet->scopes.shadow_scope_count == 0
        ? NULL : style_shadow_scope_for_order(sheet, order);
    if (bit == 0 && scope == NULL) return true;
    StyleShadowRuleKind kind = style_shadow_rule_kind(
        selector, selector_length);
    if (kind == STYLE_SHADOW_RULE_INSIDE) {
        if (bit != 0) {
            return (style_subject_adopted_scope(sheet, subject, node) & bit)
                != 0;
        }
        /* The nearest tree: an outer shadow tree's style does not reach
           into a component nested inside it. */
        return document_shadow_carrier_containing(node) == scope->carrier;
    }
    const lxb_dom_node_t *host = kind == STYLE_SHADOW_RULE_HOST
        ? node : (node == NULL ? NULL : node->parent);
    const lxb_dom_node_t *carrier = document_shadow_carrier_of_host(host);
    if (carrier == NULL) return false;
    if (kind == STYLE_SHADOW_RULE_SLOTTED
        && !document_shadow_light_child_rendered(
               carrier, (lxb_dom_node_t *) node)) return false;
    if (bit != 0) return (style_node_adopted_scope(sheet, carrier) & bit) != 0;
    return carrier == scope->carrier;
}

bool stylesheet_reserve_shadow_scope(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL
        || sheet->scopes.shadow_scope_count >= STYLE_SHADOW_SCOPE_LIMIT) return false;
    if (sheet->scopes.shadow_scope_count < sheet->scopes.shadow_scope_capacity) return true;
    size_t capacity = sheet->scopes.shadow_scope_capacity == 0
        ? 8u : sheet->scopes.shadow_scope_capacity * 2u;
    if (capacity > STYLE_SHADOW_SCOPE_LIMIT)
        capacity = STYLE_SHADOW_SCOPE_LIMIT;
    StyleShadowScope *grown = budget_realloc(
        sheet->budget, sheet->scopes.shadow_scopes, capacity * sizeof(*grown));
    if (grown == NULL) return false;
    sheet->scopes.shadow_scopes = grown;
    sheet->scopes.shadow_scope_capacity = (uint16_t) capacity;
    return true;
}

void stylesheet_note_shadow_scope(Stylesheet *sheet,
                                  const lxb_dom_node_t *element,
                                  unsigned begin, unsigned end)
{
    if (sheet == NULL || sheet->budget == NULL || element == NULL
        || end <= begin) return;
    const lxb_dom_node_t *carrier =
        document_shadow_carrier_containing(element);
    if (carrier == NULL) return;
    if (sheet->scopes.shadow_scope_count == sheet->scopes.shadow_scope_capacity) return;
    /* Ascending by begin; a source parsed before an earlier one (a late
       insertion) shifts the later ranges up. */
    size_t at = sheet->scopes.shadow_scope_count;
    while (at > 0 && sheet->scopes.shadow_scopes[at - 1u].begin > begin) {
        sheet->scopes.shadow_scopes[at] = sheet->scopes.shadow_scopes[at - 1u];
        at--;
    }
    sheet->scopes.shadow_scopes[at] = (StyleShadowScope) {begin, end, carrier};
    sheet->scopes.shadow_scope_count++;
}

static bool style_shadow_selector_mentions(const char *selector,
                                           size_t length, const char *word)
{
    size_t word_length = strlen(word);
    for (size_t i = 0; i + word_length <= length; i++) {
        if (memcmp(selector + i, word, word_length) == 0
            && (i + word_length == length
                || !(isalnum((unsigned char) selector[i + word_length])
                     || selector[i + word_length] == '-'))) return true;
    }
    return false;
}

/* The index just past the parenthesized group opening at `open`. */
static size_t style_shadow_group_end(const char *text, size_t length,
                                     size_t open)
{
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i < length; i++) {
        char c = text[i];
        if (quote != 0) {
            if (c == '\\' && i + 1 < length) i++;
            else if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0) {
            return i + 1;
        }
    }
    return length + 1;
}

bool style_shadow_selector_rewrite(const Stylesheet *sheet,
                                   const char *selector, size_t length,
                                   char *output, size_t capacity,
                                   bool *rewritten)
{
    *rewritten = false;
    bool host = style_shadow_selector_mentions(selector, length, ":host");
    bool slotted = style_shadow_selector_mentions(selector, length,
                                                  "::slotted");
    if (!host && !slotted) return true;
    if (sheet == NULL || !sheet->parsing_scoped_source) return false;
    size_t used = 0;
    const char *text = selector;
    size_t text_length = length;
    if (slotted) {
        /* `P::slotted(X)`: the assigned light child matching X. The slot
           part P is not kept: Tilefinch assigns by name only to decide
           whether a child renders, so X names the child directly. */
        size_t at = 0;
        while (at + 10 <= length && memcmp(selector + at, "::slotted(", 10))
            at++;
        size_t end = style_shadow_group_end(selector, length, at + 9);
        if (at + 10 > length || end != length) return false;
        int written = snprintf(output, capacity, ":host > :is(%.*s)",
                               (int) (end - at - 11), selector + at + 10);
        if (written < 0 || (size_t) written >= capacity) return false;
        *rewritten = true;
        return true;
    }
    /* `:host(X)` is the host when it matches X. */
    for (size_t i = 0; i < text_length;) {
        if (i + 6 <= text_length && memcmp(text + i, ":host(", 6) == 0) {
            size_t end = style_shadow_group_end(text, text_length, i + 5);
            if (end > text_length) return false;
            int written = snprintf(output + used, capacity - used,
                                   ":host:is(%.*s)", (int) (end - i - 7),
                                   text + i + 6);
            if (written < 0 || (size_t) written >= capacity - used)
                return false;
            used += (size_t) written;
            i = end;
            *rewritten = true;
            continue;
        }
        if (used + 1 >= capacity) return false;
        output[used++] = text[i++];
    }
    output[used] = '\0';
    return true;
}

static int stylesheet_compare_scope_roots(const void *left,
                                          const void *right)
{
    uintptr_t a = (uintptr_t) ((const StyleAdoptedScopeRoot *) left)->root;
    uintptr_t b = (uintptr_t) ((const StyleAdoptedScopeRoot *) right)->root;
    return a < b ? -1 : a > b;
}

static uint64_t stylesheet_remap_scope_mask(uint64_t mask,
                                            const uint8_t remap[64])
{
    uint64_t result = 0;
    for (size_t i = 0; i < 64; i++) {
        if ((mask & (UINT64_C(1) << i)) != 0 && remap[i] < 64)
            result |= UINT64_C(1) << remap[i];
    }
    return result;
}

/* The root table holds one entry per adoption list, the document's
   included; its count must not wrap at the registry's limit. */
_Static_assert(DOCUMENT_ADOPTION_ROOT_LIMIT
               <= (UINTMAX_C(1) << (8u * sizeof(((Stylesheet *) 0)
                                                ->scopes.adopted_scope_root_count))) - 1u,
               "adopted_scope_root_count must hold DOCUMENT_ADOPTION_ROOT_LIMIT");
/* Each adopted source owns one bit of the 64-bit scope masks and one
   rank slot of the order tables, and its rank (plus one) fits a byte. */
_Static_assert(DOCUMENT_ADOPTED_TIER_LIMIT <= 64u
               && sizeof(((StyleScopes *) 0)->adopted_document_order)
                  == 64u
               && sizeof(((StyleAdoptedScopeRoot *) 0)->order) == 64u,
               "scope masks and order tables must cover the adopted tier");

/* Sets adopted_order_varies when `order` lists a source out of place. */
static void stylesheet_note_adopted_order(Stylesheet *sheet,
                                          const uint8_t order[64],
                                          size_t sources)
{
    for (size_t k = 0; k < sources && k < 64u; k++) {
        if (order[k] != 0 && order[k] != k + 1u)
            sheet->scopes.adopted_order_varies = true;
    }
}

/* A refused scope update keeps the build and the committed root table.
   Confined sources are declined; document scope/order needs no
   allocation (adopted_document_mask/order are already current). */
static bool stylesheet_refuse_adopted_scopes(Stylesheet *sheet,
                                             const uint8_t document_order[64],
                                             size_t sources, bool *global)
{
    sheet->scopes.adopted_scopes_failed = true;
    stylesheet_note_adopted_order(sheet, document_order, sources);
    stylesheet_drop_rule_index(sheet);
    stylesheet_drop_custom_rule_index(sheet);
    if (global != NULL) *global = true;
    return false;
}

/* The roots whose confined sources or order differ between the new and
   the committed table (both sorted by root, each root once), written to
   `changed` up to `capacity`; the count is returned. With `remap`, old
   source bits are renumbered to the new ones first. A changed document
   entry, or more roots than fit, sets *overflow. */
static size_t stylesheet_adopted_scope_changes(const PocDocument *document,
    const StyleAdoptedScopeRoot *roots, size_t root_count,
    const StyleAdoptedScopeRoot *old, size_t old_count,
    const uint8_t *remap, size_t previous_count,
    const lxb_dom_node_t **changed, size_t capacity, bool *overflow)
{
    size_t reported = 0;
    bool tracked = remap != NULL;
    for (size_t pass = 0; pass < 2; pass++) {
        const StyleAdoptedScopeRoot *from = pass == 0 ? roots : old;
        size_t from_count = pass == 0 ? root_count : old_count;
        const StyleAdoptedScopeRoot *against = pass == 0 ? old : roots;
        size_t against_count = pass == 0 ? old_count : root_count;
        for (size_t i = 0; i < from_count; i++) {
            StyleAdoptedScopeRoot translated;
            const StyleAdoptedScopeRoot *entry = &from[i];
            if (tracked && pass == 1) {
                translated = *entry;
                translated.mask = stylesheet_remap_scope_mask(entry->mask, remap);
                memset(translated.order, 0, sizeof(translated.order));
                for (size_t k = 0; k < previous_count; k++)
                    translated.order[remap[k]] = entry->order[k];
                entry = &translated;
            }
            const StyleAdoptedScopeRoot *match = style_adopted_scope_root_lookup(
                against, against_count, entry->root);
            uint64_t other = match == NULL ? 0 : match->mask;
            if (tracked && pass == 0)
                other = stylesheet_remap_scope_mask(other, remap);
            bool same_order = false;
            if (match != NULL) {
                uint8_t order[64] = {0};
                const uint8_t *other_order = match->order;
                if (tracked && pass == 0) {
                    for (size_t bit = 0; bit < previous_count; bit++)
                        order[remap[bit]] = other_order[bit];
                    other_order = order;
                }
                same_order = memcmp(other_order, entry->order,
                                    sizeof(entry->order)) == 0;
            }
            if (other == entry->mask && same_order) continue;
            /* A root listed with a different mask is reported once. A root
               only the old table lists is reported while it lives: the
               registry forgets a carrier before it is destroyed. */
            if (pass == 1 && (other != 0
                              || (from[i].root != NULL
                                  && !document_adoption_root_known(
                                         document, from[i].root)))) continue;
            if (from[i].root == NULL) *overflow = true;
            if (changed != NULL && reported < capacity)
                changed[reported] = from[i].root;
            else *overflow = true;
            reported++;
        }
    }
    return reported;
}

/* The roots a scope update builds stay on the stack up to this many; more
   go to a budget block (DOCUMENT_ADOPTION_ROOT_LIMIT entries are 20 KiB). */
#define STYLE_ADOPTED_SCOPE_STACK_ROOTS 8u

bool stylesheet_set_appended_adopted_scopes(Stylesheet *sheet,
    const PocDocument *document,
    const lxb_dom_node_t *const *previous_sources, size_t previous_count,
    const lxb_dom_node_t **changed, size_t capacity, size_t *changed_count,
    bool *global)
{
    if (changed_count != NULL) *changed_count = 0;
    if (global != NULL) *global = false;
    if (sheet == NULL || sheet->budget == NULL) return false;
    size_t sources = sheet->scopes.adopted_source_count;
    if (sources > sheet->style_source_count || sheet->style_sources_bounded_out)
        sources = 0;
    size_t base = sheet->style_source_count - sources;
    uint8_t remap[64];
    bool tracked = previous_sources != NULL && previous_count <= 64
        && sources <= 64 && !sheet->scopes.adopted_scopes_failed;
    uint64_t existing = 0;
    memset(remap, 0xff, sizeof(remap));
    for (size_t i = 0; tracked && i < previous_count; i++) {
        for (size_t k = 0; k < sources; k++) {
            if (previous_sources[i] == sheet->style_source_nodes[base + k]) {
                remap[i] = (uint8_t) k;
                existing |= UINT64_C(1) << k;
                break;
            }
        }
        /* Removal is not an append. Fall back if the caller cannot prove
           that all previously scoped sources still exist. */
        if (remap[i] == UINT8_MAX) tracked = false;
    }
    /* Compute: every active root's confined sources and their order. */
    size_t active_lists = 0;
    bool active = false;
    for (size_t index = 0; sources != 0
         && document_adoption_list_at(document, index, NULL, NULL, NULL,
                                      &active); index++)
        if (active) active_lists++;
    StyleAdoptedScopeRoot stack_roots[STYLE_ADOPTED_SCOPE_STACK_ROOTS];
    StyleAdoptedScopeRoot *roots = stack_roots;
    if (active_lists > STYLE_ADOPTED_SCOPE_STACK_ROOTS)
        roots = budget_malloc(sheet->budget, active_lists * sizeof(*roots));
    size_t root_capacity = roots == NULL ? 0 : active_lists;
    size_t root_count = 0;
    uint64_t document_mask = 0, shadow_mask = 0;
    uint8_t document_order[64] = {0};
    const lxb_dom_node_t *root = NULL;
    lxb_dom_node_t *list[DOCUMENT_ADOPTED_SHEETS_PER_ROOT] = {0};
    size_t length = 0;
    bool refused = false;
    for (size_t index = 0; sources != 0
         && document_adoption_list_at(document, index, &root, list,
                                      &length, &active); index++) {
        if (!active) continue;
        StyleAdoptedScopeRoot entry = {.root = root};
        uint64_t mask = 0;
        for (size_t i = 0; i < length; i++) {
            for (size_t k = 0; k < sources && k < 64u; k++) {
                if (sheet->style_source_nodes[base + k] == list[i]) {
                    mask |= UINT64_C(1) << k;
                    entry.order[k] = (uint8_t) (i + 1u);
                    break;
                }
            }
        }
        if (mask == 0) continue;
        if (root == NULL) {
            document_mask |= mask;
            memcpy(document_order, entry.order, sizeof(document_order));
        } else {
            shadow_mask |= mask;
        }
        if (root_count < root_capacity) {
            entry.mask = mask;
            roots[root_count++] = entry;
        } else {
            refused = true;
        }
    }
    uint64_t shadow_only = shadow_mask & ~document_mask;
    if (refused) {
        /* The root block was refused: nothing to diff or install. */
        sheet->scopes.adopted_document_mask = document_mask;
        memcpy(sheet->scopes.adopted_document_order, document_order,
               sizeof(document_order));
        return stylesheet_refuse_adopted_scopes(sheet, document_order,
                                                sources, global);
    }
    /* Keep order even for document-adopted sources shared by roots. */
    if (root_count > 1)
        qsort(roots, root_count, sizeof(roots[0]),
              stylesheet_compare_scope_roots);
    /* Diff: report every root whose confined sources differ. */
    bool overflow = false;
    size_t reported = stylesheet_adopted_scope_changes(
        document, roots, root_count, sheet->scopes.adopted_scope_roots,
        sheet->scopes.adopted_scope_root_count, tracked ? remap : NULL,
        previous_count, changed, capacity, &overflow);
    if (changed_count != NULL)
        *changed_count = reported < capacity ? reported : capacity;
    uint64_t old_shadow = sheet->scopes.adopted_shadow_only_mask;
    uint64_t old_document = sheet->scopes.adopted_document_mask;
    if (tracked) {
        old_shadow = stylesheet_remap_scope_mask(old_shadow, remap);
        old_document = stylesheet_remap_scope_mask(old_document, remap);
    }
    if (global != NULL)
        *global = overflow
            || ((shadow_only ^ old_shadow) & (tracked ? existing : UINT64_MAX)) != 0
            || document_mask != old_document;
    /* Install. */
    sheet->scopes.adopted_document_mask = document_mask;
    memcpy(sheet->scopes.adopted_document_order, document_order,
           sizeof(document_order));
    StyleAdoptedScopeRoot *table = NULL;
    if (root_count != sheet->scopes.adopted_scope_root_count
        || sheet->scopes.adopted_scope_roots == NULL) {
        table = root_count == 0 ? NULL
            : budget_malloc(sheet->budget, root_count * sizeof(*table));
#ifndef __PSP__
        if (root_count != 0 && tilefinch_test_faults()->refuse_next_adopted_scope_table) {
            tilefinch_test_faults()->refuse_next_adopted_scope_table = false;
            budget_free(sheet->budget, table);
            table = NULL;
        }
#endif
        if (root_count != 0 && table == NULL) {
            if (roots != stack_roots) budget_free(sheet->budget, roots);
            return stylesheet_refuse_adopted_scopes(sheet, document_order,
                                                    sources, global);
        }
        budget_free(sheet->budget, sheet->scopes.adopted_scope_roots);
        sheet->scopes.adopted_scope_roots = table;
    }
    if (root_count != 0)
        memcpy(sheet->scopes.adopted_scope_roots, roots,
               root_count * sizeof(roots[0]));
    sheet->scopes.adopted_scope_root_count = (uint16_t) root_count;
    /* The rule indexes set shadow-scoped rules apart by this mask. */
    if (shadow_only != sheet->scopes.adopted_shadow_only_mask
        || sheet->scopes.adopted_scopes_failed) {
        stylesheet_drop_rule_index(sheet);
        stylesheet_drop_custom_rule_index(sheet);
    }
    sheet->scopes.adopted_shadow_only_mask = shadow_only;
    sheet->scopes.adopted_scopes_failed = false;
    sheet->scopes.adopted_order_varies = false;
    for (size_t i = 0; i < root_count; i++)
        stylesheet_note_adopted_order(sheet, roots[i].order, sources);
    if (roots != stack_roots) budget_free(sheet->budget, roots);
    return true;
}

bool stylesheet_set_adopted_scopes(Stylesheet *sheet,
                                   const PocDocument *document,
                                   const lxb_dom_node_t **changed,
                                   size_t capacity, size_t *changed_count,
                                   bool *global)
{
    return stylesheet_set_appended_adopted_scopes(sheet, document, NULL, 0,
        changed, capacity, changed_count, global);
}

void style_scopes_release(Stylesheet *sheet)
{
    budget_free(sheet->budget, sheet->scopes.adopted_scope_roots);
    sheet->scopes.adopted_scope_roots = NULL;
    budget_free(sheet->budget, sheet->scopes.shadow_scopes);
    sheet->scopes.shadow_scopes = NULL;
    sheet->scopes.shadow_scope_count = 0;
    sheet->scopes.shadow_scope_capacity = 0;
}
