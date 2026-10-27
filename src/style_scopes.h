#ifndef TILEFINCH_STYLE_SCOPES_H
#define TILEFINCH_STYLE_SCOPES_H

/* Adopted (constructed) and shadow-tree style scopes: which rules of
   which sources may apply to which elements. The state lives in
   Stylesheet.scopes (StyleScopes, tilefinch/style.h); src/style_scopes.c
   owns the table builders and the lookups the matcher and cascade use.

   Part of style_internal.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/style.h"
#include "style_match_internal.h"

/* A shadow-root carrier and the adopted sources (bits, see
   Stylesheet.scopes.adopted_shadow_only_mask) its list holds. */
typedef struct StyleAdoptedScopeRoot {
    const lxb_dom_node_t *root;
    uint64_t mask;
    /* Position in this root's list, plus one; zero means not adopted. */
    uint8_t order[64];
} StyleAdoptedScopeRoot;

/* The entry of `root` in a root table sorted by root address (each root
   listed once), or NULL. */
static inline const StyleAdoptedScopeRoot *style_adopted_scope_root_lookup(
    const StyleAdoptedScopeRoot *roots, size_t count,
    const lxb_dom_node_t *root)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        uintptr_t at = (uintptr_t) roots[middle].root;
        if (at == (uintptr_t) root) return &roots[middle];
        if (at < (uintptr_t) root) low = middle + 1u;
        else high = middle;
    }
    return NULL;
}

uint64_t style_adopted_cascade_order(const Stylesheet *sheet,
                                    const lxb_dom_node_t *node, unsigned order);
/* The per-element part of style_adopted_cascade_order: the root entries of
   the element's nearest adopting carrier and of the document, found once
   for every adopted rule the element matches. Zero-initialize it. */
typedef struct {
    const StyleAdoptedScopeRoot *carrier;
    const StyleAdoptedScopeRoot *document;
    bool ready;
} StyleAdoptedOrderContext;
uint64_t style_adopted_cascade_order_in(const Stylesheet *sheet,
                                        const lxb_dom_node_t *node,
                                        StyleAdoptedOrderContext *context,
                                        unsigned order);

/* Whether a rule of cascade order `order` may apply to `node`: false only
   for a rule of an adopted sheet that just shadow roots adopt, at an
   element outside every one of their carriers. */
bool style_adopted_scope_admits(const Stylesheet *sheet, unsigned order,
                                const lxb_dom_node_t *node,
                                const StyleMatchSubject *subject);
/* The adopted source (its bit, as in Stylesheet.scopes.adopted_shadow_only_mask)
   holding the rule of cascade order `order`; 0 for a document source. */
uint64_t style_adopted_source_bit(const Stylesheet *sheet, unsigned order);
/* Whether adopted rules are admitted by document scope (and ordered by the
   document's adoption) alone, failing confined sources closed: after a
   refused scope update, and while the sheet's sources are bounded out. A
   bounded-out sheet no longer vouches for its source addresses or the
   carriers its scopes were derived from (one may have been freed), but
   rule orders still say which adopted source holds each rule. Without an
   adopted source no rule has a source bit, so a bounded-out sheet (one
   restored from the compiled-stylesheet cache, or past
   STYLE_SOURCE_NODE_LIMIT sources) changes no admission and keeps the
   unconfined fast paths. */
static inline bool style_adopted_scopes_document_only(const Stylesheet *sheet)
{
    return sheet->scopes.adopted_scopes_failed
        || (sheet->style_sources_bounded_out
            && sheet->scopes.adopted_source_count != 0);
}
/* The adopted sources whose carriers contain `node`, memoized on
   `subject` when it was prepared for `node`. */
uint64_t style_subject_adopted_scope(const Stylesheet *sheet,
                                     const StyleMatchSubject *subject,
                                     const lxb_dom_node_t *node);
/* A shadow host (:host) and a host's light child (::slotted) are reached
   by their tree's scoped rules although they lie outside its carrier. */
bool style_subject_near_shadow_tree(const StyleMatchSubject *subject);
/* Whether the rule index keeps shadow-scoped rules apart (see
   StyleRuleIndexBucket.unscoped) and `subject` lies outside every
   adopting carrier, so its plan may leave those rules out. */
static inline bool style_subject_outside_adopted_scopes(
    const Stylesheet *sheet, const StyleMatchSubject *subject)
{
    return sheet->rule_index_scoped_split && subject != NULL
        && style_subject_adopted_scope(sheet, subject, subject->node) == 0
        && !style_subject_near_shadow_tree(subject);
}
/* The same for the custom-property index, which sorts by the current
   shadow-only mask whenever it is built (it is dropped when that moves). */
static inline bool style_subject_outside_custom_scopes(
    const Stylesheet *sheet, const StyleMatchSubject *subject)
{
    return sheet->custom_rule_index_ready
        && sheet->scopes.adopted_shadow_only_mask != 0 && subject != NULL
        && style_subject_adopted_scope(sheet, subject, subject->node) == 0
        && !style_subject_near_shadow_tree(subject);
}
/* One shadow tree's <style> (Stylesheet.scopes.shadow_scopes): rules with
   begin <= order < end belong to the tree under `carrier`. */
#define STYLE_SHADOW_SCOPE_LIMIT 256u
typedef struct StyleShadowScope {
    unsigned begin;
    unsigned end;
    const lxb_dom_node_t *carrier;
} StyleShadowScope;

/* Reserve before parsing a shadow <style>: a refused scope must not leave
   already-published rules applying to the whole document. */
bool stylesheet_reserve_shadow_scope(Stylesheet *sheet);
/* Records the rules [begin, end) just parsed from `element` as confined to
   its shadow tree. The caller must have reserved a slot first. */
void stylesheet_note_shadow_scope(Stylesheet *sheet,
                                  const lxb_dom_node_t *element,
                                  unsigned begin, unsigned end);
/* Rewrites a selector of a scoped source for the matcher: `:host(X)` to
   `:host:is(X)` and `P::slotted(X)` to `:host > :is(X)`. False when the
   selector uses either and must be dropped (an unscoped source, or no
   room); `output` is unused when *rewritten stays false. */
bool style_shadow_selector_rewrite(const Stylesheet *sheet,
                                   const char *selector, size_t length,
                                   char *output, size_t capacity,
                                   bool *rewritten);
/* Whether a shadow-confined rule (adopted by shadow roots only, or from a
   shadow tree's <style>) may apply to `node`, given its selector: a :host
   rule applies to the scope's host, a ::slotted() rule to the host's light
   children, any other rule inside the tree. */
bool style_shadow_scope_admits(const Stylesheet *sheet, unsigned order,
                               const char *selector, size_t selector_length,
                               const lxb_dom_node_t *node,
                               const StyleMatchSubject *subject);

/* `subject` (NULL when none is prepared) memoizes the element's scope for
   the rest of its resolution. */
static inline bool style_adopted_rule_in_scope(const Stylesheet *sheet,
                                               unsigned order,
                                               const char *selector,
                                               size_t selector_length,
                                               const lxb_dom_node_t *node,
                                               const StyleMatchSubject *subject)
{
    return (sheet->scopes.adopted_shadow_only_mask == 0
            && sheet->scopes.shadow_scope_count == 0
            && !style_adopted_scopes_document_only(sheet))
        || style_shadow_scope_admits(sheet, order, selector,
                                     selector_length, node, subject);
}

/* Frees the adopted-scope root table and the shadow-scope ranges
   (stylesheet_destroy). */
void style_scopes_release(Stylesheet *sheet);

#endif /* TILEFINCH_STYLE_SCOPES_H */
