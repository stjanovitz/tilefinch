/* Focus-change classification and focus probes: whether a focus move can
   change anything beyond the focused element's own paint (outline, a simple
   shadow, a retained pseudo-element), and the probes that resolve the other
   focus state's style to prove it. Split out of style_resolve.c. */

#include "style_budget_category.h"
#include "style_internal.h"
#include "style_cache_internal.h"

#include <ctype.h>
#include <string.h>

static bool focus_shadow_rule_matches(const Stylesheet *sheet, lxb_dom_node_t *node,
                                     bool outset, bool exact)
{
    if (!stylesheet_prepare_focus_rule_index((Stylesheet *) sheet)) return false;
    StyleMatchSubject subject = style_match_subject(sheet, node);
    for (size_t i = 0; i < sheet->focus_rule_count; i++) {
        const StyleRule *rule = &sheet->rules[sheet->focus_rule_indices[i]];
        if (rule->pseudo != PSEUDO_NONE || !rule_fast_matches(sheet, rule, &subject)) continue;
        const StyleDeclaration *declaration = stylesheet_rule_declaration(sheet, rule);
        if (declaration == NULL) continue;
        /* Admit only actual shadow instructions, not unrelated deferred
           outlines in a block which also contains a concrete shadow:none. */
        bool candidate = style_deferred_focus_shadow(sheet, declaration, outset);
        if (!candidate && (declaration->mask_high & S2_BOX_SHADOW) != 0) {
            ComputedStyle values;
            stylesheet_declaration_values(sheet, declaration, &values);
            const StyleBoxShadow *shadow = stylesheet_box_shadow(sheet, &values, 0);
            candidate = shadow != NULL && (!outset || !style_box_shadow_is_inset(shadow));
        }
        if (candidate && (!exact || style_rule_selector_matches_subject(
                sheet, sheet->focus_rule_indices[i], node, &subject))) return true;
    }
    return false;
}

bool style_node_focus_shadow_may_match(const Stylesheet *sheet, lxb_dom_node_t *node,
                                     bool outset)
{
    return focus_shadow_rule_matches(sheet, node, outset, false);
}

static bool focus_token_at(const char *text, size_t length, size_t at,
                           size_t *token_length, bool *within)
{
    static const char focus[] = ":focus";
    if (text == NULL || at + sizeof(focus) - 1 > length
        || memcmp(text + at, focus, sizeof(focus) - 1) != 0) return false;
    size_t used = sizeof(focus) - 1;
    bool focus_within = false;
    if (at + used + 8 <= length
        && memcmp(text + at + used, "-visible", 8) == 0) {
        used += 8;
    } else if (at + used + 7 <= length
               && memcmp(text + at + used, "-within", 7) == 0) {
        used += 7;
        focus_within = true;
    }
    if (at + used < length) {
        unsigned char next = (unsigned char) text[at + used];
        if (isalnum(next) || next == '_' || next == '-') return false;
    }
    if (token_length != NULL) *token_length = used;
    if (within != NULL) *within = focus_within;
    return true;
}

static bool focus_token_inside_has(const char *text, size_t token_at)
{
    bool has_stack[16] = {0};
    size_t depth = 0;
    char quote = '\0';
    unsigned brackets = 0;
    for (size_t at = 0; at < token_at; at++) {
        char c = text[at];
        if (quote != '\0') {
            if (c == quote && (at == 0 || text[at - 1] != '\\')) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '[') {
            brackets++;
            continue;
        }
        if (c == ']' && brackets != 0) {
            brackets--;
            continue;
        }
        if (brackets != 0) continue;
        if (c == '(') {
            bool is_has = at >= 4
                && memcmp(text + at - 4, ":has", 4) == 0;
            if (depth < sizeof(has_stack) / sizeof(has_stack[0])) {
                has_stack[depth] = is_has;
            }
            depth++;
        } else if (c == ')' && depth != 0) {
            depth--;
        }
    }
    size_t bounded = depth < sizeof(has_stack) / sizeof(has_stack[0])
        ? depth : sizeof(has_stack) / sizeof(has_stack[0]);
    for (size_t i = 0; i < bounded; i++) {
        if (has_stack[i]) return true;
    }
    return false;
}

/* A descendant :has(:focus) cannot observe a focus move outside its anchor.
   Prove only a simple positive anchor here. Nested functions, selector lists,
   escapes and sibling-relative arguments retain the conservative fallback. */
static bool focus_has_anchor_absent(lxb_dom_node_t *node,
    const char *text, size_t length, size_t token_at)
{
    size_t has_at = SIZE_MAX;
    unsigned parentheses = 0u, brackets = 0u;
    char quote = '\0';
    for (size_t at = 0; at < length; at++) {
        if (text[at] == ',' || text[at] == '\\') return false;
        char c = text[at];
        if (quote != '\0') {
            if (c == quote) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '[') { if (++brackets > 16u) return false; continue; }
        if (c == ']') { if (brackets == 0u) return false; brackets--; continue; }
        if (brackets != 0u) continue;
        if ((at + 5u <= length && !memcmp(text + at, ":host", 5u))
            || (at + 6u <= length && !memcmp(text + at, ":scope", 6u))
            || (at + 2u <= length && text[at] == ':' && text[at + 1u] == ':'))
            return false;
        if (at + 5u <= token_at && !memcmp(text + at, ":has(", 5u)) {
            if (parentheses != 0u) return false;
            has_at = at;
            break;
        }
        if (c == '(') { if (++parentheses > 16u) return false; }
        if (c == ')') { if (parentheses == 0u) return false; parentheses--; }
    }
    if (has_at == SIZE_MAX || has_at == 0u) return false;
    unsigned char last = (unsigned char) text[has_at - 1u];
    if (!(isalnum(last) || last == '_' || last == '-' || last == '*'
          || last == ']' || last == ')'))
        return false;
    size_t end = has_at + 5u;
    for (; end < length && text[end] != ')'; end++) {
        char c = text[end];
        if (c == '(' || c == '[' || c == '\\' || c == '+' || c == '~'
            || c == ',' || c == '\'' || c == '"') return false;
    }
    if (end == length || token_at < has_at + 5u || token_at >= end)
        return false;
    for (size_t at = end + 1u; at < length; at++)
        if (text[at] == ',' || text[at] == '\\') return false;
    size_t visited = 0u;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (++visited > 128u) return false;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && style_selector_matches(at, text, has_at)) return false;
    }
    return true;
}

static bool focus_selector_probe_range(
    const char *text, size_t length, size_t token_at,
    size_t *probe_at, size_t *probe_length)
{
    if (text == NULL || token_at >= length
        || probe_at == NULL || probe_length == NULL) return false;
    size_t start = 0, end = length;
    size_t depth = 0, focus_depth = 0;
    size_t nested_start[16] = {0};
    unsigned brackets = 0;
    char quote = '\0';
    bool found = false;
    bool focus_in_has = focus_token_inside_has(text, token_at);
    for (size_t at = 0; at < length; at++) {
        char c = text[at];
        if (at == token_at) {
            focus_depth = depth;
            if (focus_in_has && depth != 0
                && depth <= sizeof(nested_start) / sizeof(nested_start[0])) {
                start = nested_start[depth - 1u];
            }
            found = true;
        }
        if (quote != '\0') {
            if (c == quote && (at == 0 || text[at - 1] != '\\')) quote = '\0';
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == '[') {
            brackets++;
            continue;
        }
        if (c == ']' && brackets != 0) {
            brackets--;
            continue;
        }
        if (brackets != 0) continue;
        if (c == '(') {
            if (depth < sizeof(nested_start) / sizeof(nested_start[0])) {
                nested_start[depth] = at + 1u;
            }
            depth++;
            continue;
        }
        if (c == ')') {
            if (found && focus_in_has && depth == focus_depth) {
                end = at;
                break;
            }
            if (depth != 0) depth--;
            continue;
        }
        bool combinator = c == '>' || c == '+' || c == '~'
            || isspace((unsigned char) c);
        if (!combinator || depth != (found ? focus_depth : depth)) continue;
        if (!found) {
            start = at + 1;
        } else {
            end = at;
            break;
        }
    }
    while (start < token_at && isspace((unsigned char) text[start])) start++;
    if (focus_in_has && start < token_at
        && (text[start] == '>' || text[start] == '+'
            || text[start] == '~')) {
        start++;
        while (start < token_at
               && isspace((unsigned char) text[start])) start++;
    }
    while (end > token_at && isspace((unsigned char) text[end - 1])) end--;
    if (!found || start >= end || token_at < start || token_at >= end) {
        return false;
    }
    *probe_at = start;
    *probe_length = end - start;
    return true;
}

static bool focus_retained_value_unchanged(
    const Stylesheet *sheet, lxb_dom_node_t *node, lxb_dom_node_t *subject,
    const StyleCustomRule *rule);

/* A non-inherited opacity declaration can be shadowed by a later responsive
   rule. Prove the winning value on every possible descendant, rather than
   rebuilding a page for a declaration which changes no used value. */
static bool focus_descendant_opacity_unchanged(
    const Stylesheet *sheet, lxb_dom_node_t *node, lxb_dom_node_t *root,
    const char *text, size_t text_length)
{
    size_t start = style_selector_rightmost_compound_start(text, text_length);
    if (start == 0 || start >= text_length) return false;
    const char *selector = text + start;
    size_t length = text_length - start;
    /* Stateful/relative/escaped subject probes need the ordinary path. */
    for (size_t i = 0; i < length; i++)
        if (!isalnum((unsigned char) selector[i])
            && selector[i] != '.' && selector[i] != '#'
            && selector[i] != '-' && selector[i] != '_'
            && selector[i] != '*') return false;
    const StyleCustomRule property = {.name = "opacity", .name_length = 7};
    size_t visits = 0;
    for (lxb_dom_node_t *at = root; at != NULL;) {
        if (++visits > 128u) return false;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && style_selector_matches(at, selector, length)
            && !focus_retained_value_unchanged(sheet, node, at, &property))
            return false;
        if (at->first_child != NULL) { at = at->first_child; continue; }
        while (at != root && at->next == NULL) at = at->parent;
        if (at == root) break;
        at = at->next;
    }
    return true;
}

/* A compound :focus-within carrier can change only on the focused node's
   ancestor chain. Boolean wrappers (:is/:not/:where) keep that scope, and a
   :has() argument is safe once no ancestor matches its carrier prefix;
   other relative selectors, escapes and excessive nesting stay
   conservative. */
static bool focus_within_may_affect_ancestor(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *text, size_t length, size_t token_at,
    const StyleCustomRule *custom_rule, bool self_local,
    const StyleRule *rule)
{
    /* Even inside :has(), :focus-within can change only on the focused
       node or an ancestor. Prove an unrelated simple carrier absent before
       treating the surrounding relative selector as globally unsafe. Keep
       complex/escaped probes conservative rather than parsing them twice. */
    if (focus_token_inside_has(text, token_at)) {
        size_t probe_at = 0, probe_length = 0;
        if (!focus_selector_probe_range(text, length, token_at,
                                       &probe_at, &probe_length)) return true;
        /* The prefix is a necessary condition for the carrier. Do not run
           :focus-within itself here: testing it on an ancestor can walk that
           ancestor's whole subtree merely to prove an unrelated class absent. */
        probe_length = token_at - probe_at;
        if (probe_length == 0) return true;
        for (size_t i = 0; i < probe_length; i++) {
            char c = text[probe_at + i];
            if (c == '(' || c == ')' || c == '\\' || c == ',') return true;
        }
        size_t depth = 0;
        bool carrier = false;
        for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
            if (++depth > 128u) return true;
            if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                && style_selector_matches(at, text + probe_at, probe_length)) {
                carrier = true;
                break;
            }
        }
        if (!carrier) return false;
    }
    size_t start = 0u, end = length, depth = 0u;
    char quote = 0;
    unsigned brackets = 0u;
    bool found = false;
    bool boolean[16] = {0}, contains_focus[16] = {0};
    for (size_t at = 0u; at < length; at++) {
        char c = text[at];
        if (c == '\\') return true;
        if (at == token_at) {
            if (quote != 0 || brackets != 0u
                || depth > 16u) return true;
            if (depth != 0u) contains_focus[depth - 1u] = true;
            found = true;
        }
        if (quote != 0) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') { quote = c; continue; }
        if (c == '[') { brackets++; continue; }
        if (c == ']' && brackets != 0u) { brackets--; continue; }
        if (brackets != 0u) continue;
        if (c == '(') {
            if (depth == 16u) return true;
            boolean[depth] =
                (at >= 3u && memcmp(text + at - 3u, ":is", 3u) == 0)
                || (at >= 4u && memcmp(text + at - 4u, ":not", 4u) == 0)
                || (at >= 6u && memcmp(text + at - 6u, ":where", 6u) == 0);
            contains_focus[depth++] = false;
            continue;
        }
        if (c == ')' && depth != 0u) {
            depth--;
            if (contains_focus[depth]) {
                if (!boolean[depth]) return true;
                if (depth != 0u) contains_focus[depth - 1u] = true;
            }
            continue;
        }
        if (depth != 0u) {
            if (c == '>' || c == '+' || c == '~'
                || isspace((unsigned char) c)) boolean[depth - 1u] = false;
            continue;
        }
        if (c == '>' || c == '+' || c == '~' || isspace((unsigned char) c)) {
            if (found) { end = at; break; }
            start = at + 1u;
        }
    }
    if (!found || end <= start || depth != 0u) return true;
    /* Retained custom selectors may still carry a pseudo-element suffix. */
    for (size_t at = start; at + 1u < end; at++) {
        if (text[at] != ':') continue;
        if (text[at + 1u] == ':'
            || (end - at >= 6u && memcmp(text + at, ":after", 6u) == 0)
            || (end - at >= 7u && memcmp(text + at, ":before", 7u) == 0))
            return true;
    }
    size_t ancestors = 0u;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (++ancestors > 128u) return true;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && style_selector_matches(at, text + start, end - start)) {
            /* The target's own concrete style is compared by the caller.
               A following compound or pseudo can affect other paint. */
            if (at == node && self_local && end == length) continue;
            if (custom_rule != NULL && focus_retained_value_unchanged(
                    sheet, node, at, custom_rule)) continue;
            if (custom_rule != NULL && custom_rule->pseudo == PSEUDO_NONE
                && custom_rule->name_length == 7
                && memcmp(custom_rule->name, "opacity", 7) == 0) {
                if (focus_descendant_opacity_unchanged(
                        sheet, node, at, text, length)) continue;
            }
            const StyleDeclaration *declaration = rule == NULL ? NULL
                : stylesheet_rule_declaration(sheet, rule);
            if (declaration != NULL && rule->pseudo == PSEUDO_NONE
                && declaration->mask == S_OPACITY
                && declaration->mask_high == 0
                && declaration->inherit_mask == 0
                && declaration->deferred_length == 0
                && focus_descendant_opacity_unchanged(
                    sheet, node, at, text, length)) continue;
            return true;
        }
    }
    return false;
}

typedef enum {
    FOCUS_TOKEN_LOCAL,     /* proven to restyle no other element */
    FOCUS_TOKEN_NONLOCAL,  /* may restyle another element */
    FOCUS_TOKEN_SUBJECT    /* on the subject compound: the caller decides */
} FocusTokenReach;

/* Where the :focus/:focus-within token at `at` (recognised by
   focus_token_at) can reach when `node`'s focus changes. `rightmost` is
   the start of the selector's subject compound; custom_rule, self_local and
   rule are passed to focus_within_may_affect_ancestor. */
static FocusTokenReach focus_token_reach(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *selector, size_t length, size_t at, bool within,
    size_t rightmost, const StyleCustomRule *custom_rule, bool self_local,
    const StyleRule *rule)
{
    if (focus_has_anchor_absent(node, selector, length, at))
        return FOCUS_TOKEN_LOCAL;
    if (within)
        return focus_within_may_affect_ancestor(sheet, node, selector, length,
                at, custom_rule, self_local, rule)
            ? FOCUS_TOKEN_NONLOCAL : FOCUS_TOKEN_LOCAL;
    if (at >= rightmost && !focus_token_inside_has(selector, at))
        return FOCUS_TOKEN_SUBJECT;
    size_t probe_at = 0, probe_length = 0;
    return !focus_selector_probe_range(selector, length, at,
                                       &probe_at, &probe_length)
            || style_selector_matches(node, selector + probe_at, probe_length)
        ? FOCUS_TOKEN_NONLOCAL : FOCUS_TOKEN_LOCAL;
}

#define FOCUS_VACUITY_VISIT_LIMIT 1024u

/* A forward focus rule (style_focus_selector_reaches_forward) can style
   only elements in `node`'s subtree or a later sibling's subtree. When no
   such element matches it in the probe's current state, it styles nothing
   there: both callers ask in both focus states. Bounded; false when unsure. */
static bool focus_rule_vacuous_forward(const Stylesheet *sheet,
                                       lxb_dom_node_t *node, size_t index,
                                       size_t *budget)
{
    const StyleRule *rule = &sheet->rules[index];
    if (node->parent == NULL
        || !style_focus_selector_reaches_forward(rule->selector)) return false;
    for (lxb_dom_node_t *top = node; top != NULL; top = top->next) {
        for (lxb_dom_node_t *at = top; at != NULL;) {
            if (at->type == LXB_DOM_NODE_TYPE_ELEMENT && at != node) {
                if (*budget == 0) return false;
                (*budget)--;
                if (style_rule_selector_matches(sheet, index, at)) return false;
            }
            if (at->first_child != NULL) { at = at->first_child; continue; }
            while (at != top && at->next == NULL) at = at->parent;
            if (at == top) break;
            at = at->next;
        }
    }
    return true;
}

/* Whether `selector` reads focus exactly once (one :focus, :focus-visible
   or :focus-within token, or one focus marker attribute). Probing the moved
   node in both states decides such a rule for every joint focus state; a
   rule that also reads another element's focus could match only in the
   pre-move state (A focused, B not), which no single-node probe sees. */
static bool focus_selector_reads_focus_once(const char *selector,
                                            size_t length)
{
    size_t reads = 0;
    for (size_t at = 0; at < length; at++) {
        size_t token_length = 0;
        if (focus_token_at(selector, length, at, &token_length, NULL)) {
            if (++reads > 1u) return false;
            at += token_length - 1u;
        } else if (selector[at] == '['
                   && length - at - 1u >= sizeof(STYLE_FOCUS_MARKER) - 1u
                   && memcmp(selector + at + 1u, STYLE_FOCUS_MARKER,
                             sizeof(STYLE_FOCUS_MARKER) - 1u) == 0) {
            if (++reads > 1u) return false;
        }
    }
    return reads == 1u;
}

static bool focus_selector_side_effects_are_local(
    const Stylesheet *sheet, lxb_dom_node_t *node, bool allow_pseudos,
    bool *pseudo_retry)
{
    if (sheet == NULL || node == NULL
        || !stylesheet_prepare_focus_rule_index(
            (Stylesheet *) sheet)) return false;
    size_t visits = FOCUS_VACUITY_VISIT_LIMIT;
    for (size_t focus_index = 0;
         focus_index < sheet->focus_rule_count; focus_index++) {
        size_t i = sheet->focus_rule_indices[focus_index];
        if (i >= sheet->count) return false;
        const StyleRule *rule = &sheet->rules[i];
        const char *selector = rule->selector;
        size_t length = rule->selector_length;
        size_t rightmost = style_rule_rightmost_compound(rule);
        for (size_t at = 0; at < length; at++) {
            size_t token_length = 0;
            bool within = false;
            if (!focus_token_at(
                    selector, length, at, &token_length, &within)) continue;
            FocusTokenReach reach = focus_token_reach(sheet, node, selector,
                length, at, within, rightmost, NULL,
                rule->pseudo == PSEUDO_NONE, rule);
            if (reach == FOCUS_TOKEN_NONLOCAL) {
                if (!focus_selector_reads_focus_once(selector, length)
                    || !focus_rule_vacuous_forward(sheet, node, i, &visits))
                    return false;
                break;
            }
            if (reach == FOCUS_TOKEN_SUBJECT && rule->pseudo != PSEUDO_NONE
                && (!allow_pseudos || rule->pseudo == PSEUDO_PLACEHOLDER)
                && style_rule_selector_matches(sheet, i, node)) {
                if (pseudo_retry != NULL && !allow_pseudos
                    && rule->pseudo != PSEUDO_PLACEHOLDER) *pseudo_retry = true;
                return false;
            }
            at += token_length - 1;
        }
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        const char *selector = rule->selector;
        size_t length = strlen(selector);
        if (strstr(selector, ":focus") == NULL) continue;
        size_t rightmost = style_selector_rightmost_compound_start(
            selector, length);
        for (size_t at = 0; at < length; at++) {
            size_t token_length = 0;
            bool within = false;
            if (!focus_token_at(
                    selector, length, at, &token_length, &within)) continue;
            FocusTokenReach reach = focus_token_reach(sheet, node, selector,
                length, at, within, rightmost, rule, false, NULL);
            if (reach == FOCUS_TOKEN_NONLOCAL) return false;
            if (reach == FOCUS_TOKEN_SUBJECT
                && style_selector_matches(node, selector, length)
                && !focus_retained_value_unchanged(sheet, node, node, rule)) {
                /*
                 * A directly matched custom property can inherit into any
                 * descendant even when this node's concrete ComputedStyle
                 * happens not to consume it. It is never a local paint-only
                 * change. Pseudo-element custom rules are covered as well.
                 */
                return false;
            }
            at += token_length - 1;
        }
    }
    return true;
}

/* A focus probe resolves the other focus state's style through the
   matcher's focus override (style_focus_override_swap), so the DOM is
   never touched and there is nothing to restore in it: the probe only
   puts the previous override back. */
typedef struct {
    lxb_dom_node_t *node;
    /* The node's variable-cache entries are dropped around each state
       change and at the end when set (the probe resolves variables). */
    Stylesheet *variables;
    bool focused;  /* The marker state the matcher sees outside the probe. */
    bool touched;  /* An override was set, so end must restore `saved`. */
    StyleFocusOverride saved;
} FocusOverrideProbe;

static void focus_override_probe_begin(FocusOverrideProbe *probe,
                                       lxb_dom_node_t *node,
                                       const Stylesheet *variables)
{
    probe->node = node;
    probe->variables = (Stylesheet *) variables;
    probe->touched = false;
    probe->focused = style_node_has_focus_marker(node);
}

/* Puts the probe in the `focused` state; false (no state change) for a
   node that is not an element. */
static bool focus_override_probe_set(FocusOverrideProbe *probe, bool focused)
{
    if (probe->node == NULL || probe->node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    StyleFocusOverride previous = style_focus_override_swap(
        (StyleFocusOverride) {probe->node, focused});
    if (!probe->touched) probe->saved = previous;
    probe->touched = true;
    if (probe->variables != NULL)
        style_variable_cache_invalidate_node(probe->variables, probe->node);
    return true;
}

static void focus_override_probe_end(FocusOverrideProbe *probe)
{
    if (probe->touched) (void) style_focus_override_swap(probe->saved);
    if (probe->variables != NULL)
        style_variable_cache_invalidate_node(probe->variables, probe->node);
}

/* Duplicate declarations (including a non-focus alternative in a selector
   list) need not rebuild layout when their winning retained value stays
   identical. Both queries must succeed; absent, overlong and inherited
   custom-property cases keep the conservative path. */
static bool focus_retained_value_unchanged(
    const Stylesheet *sheet, lxb_dom_node_t *node, lxb_dom_node_t *subject,
    const StyleCustomRule *rule)
{
    if (rule->pseudo != PSEUDO_NONE
        || (rule->name[0] == '-' && rule->name[1] == '-')) return false;
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, sheet);
    char before[STYLE_CUSTOM_RESOLVED_CAPACITY], after[STYLE_CUSTOM_RESOLVED_CAPACITY];
    bool found_before = style_retained_property_value(
        sheet, subject, rule->name, rule->name_length, before, sizeof(before));
    bool changed = focus_override_probe_set(&probe, !probe.focused);
    bool found_after = changed && style_retained_property_value(
        sheet, subject, rule->name, rule->name_length, after, sizeof(after));
    focus_override_probe_end(&probe);
    return found_before && found_after && strcmp(before, after) == 0;
}

static StylePaintStack focus_effective_paint(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    StylePaintStack paint = style_paint_stack_copy(sheet, style);
    for (size_t i = 0; i < paint.background_count; i++) {
        StylePaintLayer *layer = &paint.backgrounds[i];
        if (layer->kind != STYLE_PAINT_IMAGE_NONE) continue;
        /* Position, sizing and repetition of `none` produce no image pixels.
           Keep origin/clip, which can still affect a solid background. */
        layer->width = layer->height = 0;
        layer->position_x = layer->position_y = 0;
        layer->fit = layer->flags = layer->position_edges = 0;
        layer->origin = STYLE_PAINT_BOX_PADDING;
    }
    bool no_images = true;
    for (size_t i = 0; i < paint.background_count; i++)
        if (paint.backgrounds[i].kind != STYLE_PAINT_IMAGE_NONE) no_images = false;
    if (no_images && (!style->has_background || style->background_alpha == 0)) {
        memset(paint.backgrounds, 0, sizeof(paint.backgrounds));
        paint.background_count = 0;
        paint.components &= (uint8_t) ~(STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE
                                       | STYLE_PAINT_COMPONENT_BACKGROUND_BOX);
    }
    /* Explicit `none` has no pixels, despite retaining the component bit. */
    if (paint.box_shadow_count == 0) {
        paint.components &= (uint8_t) ~STYLE_PAINT_COMPONENT_BOX_SHADOW;
        paint.box_shadow_count = 0;
        memset(paint.box_shadows, 0, sizeof(paint.box_shadows));
    }
    return paint;
}

static bool computed_style_equal_without_outline(
    const Stylesheet *sheet, const ComputedStyle *left, const ComputedStyle *right)
{
    if (left == NULL || right == NULL) return false;
    ComputedStyle a = *left, b = *right;
    if (a.background_image_kind == STYLE_BACKGROUND_IMAGE_NONE
        && b.background_image_kind == STYLE_BACKGROUND_IMAGE_NONE) {
        a.background_width = b.background_width = 0;
        a.background_height = b.background_height = 0;
        a.background_position_x = b.background_position_x = 0;
        a.background_position_y = b.background_position_y = 0;
        a.background_fit = b.background_fit = 0;
        a.background_size_flags = b.background_size_flags = 0;
    }
    a.outline_state = b.outline_state = 0;
    a.outline_color = b.outline_color = 0;
    a.outline_alpha = b.outline_alpha = 0;
    StylePaintStack paint_a = focus_effective_paint(sheet, left);
    StylePaintStack paint_b = focus_effective_paint(sheet, right);
    if (memcmp(&paint_a, &paint_b, sizeof(paint_a)) != 0) return false;
    computed_style_set_paint_stack_id(&a, 0);
    computed_style_set_paint_stack_id(&b, 0);
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool focus_paint_equal(
    const Stylesheet *sheet, const ComputedStyle *left,
    const ComputedStyle *right)
{
    StylePaintStack a = focus_effective_paint(sheet, left);
    StylePaintStack b = focus_effective_paint(sheet, right);
    return memcmp(&a, &b, sizeof(a)) == 0;
}

static bool focus_simple_shadow(const StylePaintStack *paint)
{
    if (paint->box_shadow_count == 0) return true;
    if (paint->box_shadow_count != 1) return false;
    const StyleBoxShadow *shadow = &paint->box_shadows[0];
    return style_box_shadow_blur(shadow) == 0
        && (!style_box_shadow_is_inset(shadow)
            || (shadow->offset_x == 0 && shadow->offset_y == 0
                && shadow->spread > 0));
}

static bool focus_paint_equal_except_simple_shadow(
    const Stylesheet *sheet, const ComputedStyle *left,
    const ComputedStyle *right)
{
    StylePaintStack a = focus_effective_paint(sheet, left);
    StylePaintStack b = focus_effective_paint(sheet, right);
    /* An unchanged prefix may include blurred/inset layers. Only the final
       focus layer changes; its retained slot stays behind that prefix. */
    size_t prefix = 0;
    while (prefix < a.box_shadow_count && prefix < b.box_shadow_count
        && memcmp(&a.box_shadows[prefix], &b.box_shadows[prefix],
                  sizeof(a.box_shadows[0])) == 0) prefix++;
    if (prefix != 0) {
        a.box_shadow_count -= prefix;
        b.box_shadow_count -= prefix;
        memmove(a.box_shadows, a.box_shadows + prefix,
                a.box_shadow_count * sizeof(a.box_shadows[0]));
        memmove(b.box_shadows, b.box_shadows + prefix,
                b.box_shadow_count * sizeof(b.box_shadows[0]));
    }
    if (!focus_simple_shadow(&a) || !focus_simple_shadow(&b))
        return false;
    /* Layered inset updates do not yet have a distinct retained slot. */
    if (prefix != 0 && ((a.box_shadow_count && style_box_shadow_is_inset(&a.box_shadows[0]))
        || (b.box_shadow_count && style_box_shadow_is_inset(&b.box_shadows[0])))) return false;
    /* Outset geometry is retained, never moved at input time. Removal and
       recolouring are safe; changing an existing offset/spread is not. */
    if (a.box_shadow_count && b.box_shadow_count
        && (!style_box_shadow_is_inset(&a.box_shadows[0])
            || !style_box_shadow_is_inset(&b.box_shadows[0]))
        && (a.box_shadows[0].offset_x != b.box_shadows[0].offset_x
            || a.box_shadows[0].offset_y != b.box_shadows[0].offset_y
            || a.box_shadows[0].spread != b.box_shadows[0].spread
            || a.box_shadows[0].blur != b.box_shadows[0].blur)) return false;
    a.components &= (uint8_t) ~STYLE_PAINT_COMPONENT_BOX_SHADOW;
    b.components &= (uint8_t) ~STYLE_PAINT_COMPONENT_BOX_SHADOW;
    a.box_shadow_count = b.box_shadow_count = 0;
    memset(a.box_shadows, 0, sizeof(a.box_shadows));
    memset(b.box_shadows, 0, sizeof(b.box_shadows));
    return memcmp(&a, &b, sizeof(a)) == 0;
}

bool style_node_focus_outset_shadow(const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, const ComputedStyle *style, StyleBoxShadow *output,
    size_t *layer_index)
{
    size_t count = stylesheet_box_shadow_count(sheet, style);
    if (!style_node_focus_shadow_may_match(sheet, node, true)) return false;
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, NULL);
    /* The hypothetical focus is rolled back before returning. Preserve
       the real layout's caches rather than emptying them for every control;
       none of their selector/variable answers may cross into this probe. */
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->resolve_scratch->variable_cache = NULL;
    sheet->resolve_scratch->variable_cache_lease = NULL;
    sheet->resolve_scratch->ancestor_bloom_cache = NULL;
    sheet->resolve_scratch->retained_matches = NULL;
    bool changed = focus_override_probe_set(&probe, true);
    /* Fast keys deliberately over-admit functional selectors. Do not resolve
       every control's hypothetical cascade merely because a ring exists in
       an unrelated component. The override already reads focused, so the exact
       scoped selector test preserves :focus/:focus-within semantics. */
    style_class_tokens_scope_begin(sheet);
    bool matches = changed && focus_shadow_rule_matches(sheet, node, true, true);
    bool layered = false;
    StyleMatchSubject subject = style_match_subject(sheet, node);
    for (size_t i = 0; matches && i < sheet->focus_rule_count; i++) {
        size_t index = sheet->focus_rule_indices[i];
        const StyleRule *rule = &sheet->rules[index];
        const StyleDeclaration *d = stylesheet_rule_declaration(sheet, &sheet->rules[index]);
        if (rule->pseudo == PSEUDO_NONE && d != NULL && d->deferred_program_count != 0
            && rule_fast_matches(sheet, rule, &subject)
            && style_rule_selector_matches_subject(sheet, index, node, &subject)
            && style_deferred_focus_shadow_exceeds(sheet, d, node, count + 1u)) {
            layered = true;
            break;
        }
    }
    /* The retained fast path can add only one layer. Decline obviously
       larger effects before resolving a whole hypothetical cascade. This
       is only slot admission: full focus layout still resolves all rules,
       including an overriding simple shadow, without changing pixels. */
    ComputedStyle focused = matches && !layered ? style_for_node(sheet, node, parent) : *style;
    style_class_tokens_scope_end(sheet);
    focus_override_probe_end(&probe);
    *sheet->resolve_scratch = saved_scratch;
    size_t focused_count = stylesheet_box_shadow_count(sheet, &focused);
    if (!matches || layered || focused_count == 0
        || (count != focused_count && count != focused_count - 1u))
        return false;
    size_t index = focused_count - 1u;
    for (size_t i = 0; i < index; i++) {
        const StyleBoxShadow *a = stylesheet_box_shadow(sheet, style, i);
        const StyleBoxShadow *b = stylesheet_box_shadow(sheet, &focused, i);
        if (a == NULL || b == NULL || memcmp(a, b, sizeof(*a)) != 0) return false;
    }
    const StyleBoxShadow *shadow = stylesheet_box_shadow(sheet, &focused, index);
    if (shadow == NULL || style_box_shadow_is_inset(shadow)
        || style_box_shadow_blur(shadow) || shadow->offset_x || shadow->offset_y
        || shadow->spread <= 0) return false;
    if (count == focused_count) {
        const StyleBoxShadow *normal = stylesheet_box_shadow(sheet, style, index);
        if (normal == NULL || normal->offset_x != shadow->offset_x
            || normal->offset_y != shadow->offset_y || normal->spread != shadow->spread
            || normal->blur != shadow->blur) return false;
    }
    *output = *shadow;
    *layer_index = index;
    return true;
}

static bool focus_style_uniform_rounded_border(
    const Stylesheet *sheet, const ComputedStyle *style,
    uint32_t *color, uint8_t *alpha)
{
    if (sheet == NULL || style == NULL
        || style_border_radius_maximum(style->border_radius) <= 0
        || style->border.top <= 0
        || style->border.top != style->border.right
        || style->border.top != style->border.bottom
        || style->border.top != style->border.left) return false;
    uint32_t first_color = 0;
    uint8_t first_alpha = 0;
    for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
        if (computed_style_border_line(
                style, (StyleBorderSide) side) != STYLE_BORDER_SOLID) {
            return false;
        }
        uint8_t side_alpha = 0;
        uint32_t side_color = stylesheet_border_color(
            sheet, style, (StyleBorderSide) side, &side_alpha);
        if (side == 0) {
            first_color = side_color;
            first_alpha = side_alpha;
        } else if (side_color != first_color || side_alpha != first_alpha) {
            return false;
        }
    }
    if (color != NULL) *color = first_color;
    if (alpha != NULL) *alpha = first_alpha;
    return true;
}

bool style_focus_pseudo_visibility_slot(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    if (!style->generated_content || !style->out_of_flow || style->fixed_position
        || style->hidden || style->display == DISPLAY_NONE
        || style->generated_text == NULL || style->generated_text_length != 1
        || !ispunct((unsigned char) style->generated_text[0])
        || style->generated_expression || style->generated_attr_length
        || style->opacity != 255 || style->has_background
        || style->background_image || style->mask_image
        || style->border.top || style->border.right
        || style->border.bottom || style->border.left
        || style->has_transform || style->has_filter || style->has_perspective
        || style->clip_path_state || computed_style_outline_width(style))
        return false;
    return style_paint_stack_copy(sheet, style).components == 0;
}

static bool focus_pseudo_translation(
    const Stylesheet *sheet, const ComputedStyle *normal,
    const ComputedStyle *focused, int *dx, int *dy)
{
    if (memcmp(normal, focused, sizeof(*normal)) == 0) return true;
    if (!normal->generated_content || !normal->out_of_flow
        || normal->fixed_position || !normal->has_background
        || normal->generated_text_length != 0 || normal->generated_attr_length != 0
        || normal->generated_expression || normal->hidden || normal->visibility_hidden
        || normal->display == DISPLAY_NONE || normal->opacity != 255
        || normal->has_filter || normal->has_perspective || normal->clip_path_state != 0
        || normal->border.top || normal->border.right || normal->border.bottom || normal->border.left
        || normal->background_image_kind != STYLE_BACKGROUND_IMAGE_NONE
        || normal->background_image != NULL || normal->mask_image != NULL
        || computed_style_outline_width(normal) != 0
        || normal->transform_x_percent || normal->transform_y_percent
        || focused->transform_x_percent || focused->transform_y_percent
        || normal->transform_scale_q14 != 16384 || focused->transform_scale_q14 != 16384
        || normal->individual_rotate_quadrants || focused->individual_rotate_quadrants)
        return false;
    StylePaintStack paint = style_paint_stack_copy(sheet, normal);
    if (paint.components != 0 || !focus_paint_equal(sheet, normal, focused)) return false;
    ComputedStyle a = *normal, b = *focused;
    a.has_transform = b.has_transform = false;
    a.transform_x = b.transform_x = a.transform_y = b.transform_y = 0;
    if (memcmp(&a, &b, sizeof(a)) != 0) return false;
    int64_t x = (int64_t) focused->transform_x - normal->transform_x;
    int64_t y = (int64_t) focused->transform_y - normal->transform_y;
    if (x < -32767 || x > 32767 || y < -32767 || y > 32767) return false;
    *dx = (int) x; *dy = (int) y;
    return true;
}

/* Whether a colour change of `style` reaches only its text: no other paint
   of the element consumes currentColor (a background, a shadow or mask, a
   list marker; a border is compared by the caller). */
static bool focus_color_reaches_text_only(const Stylesheet *sheet,
                                          const ComputedStyle *style)
{
    StylePaintStack paint = focus_effective_paint(sheet, style);
    return !style->background_current_color && style->mask_image == NULL
        && !style->list_item
        && paint.box_shadow_count == 0 && paint.text_shadow_count == 0
        && paint.mask_count == 0 && paint.background_count == 0;
}

static bool focus_style_bordered(const ComputedStyle *style)
{
    return style->border.top || style->border.right
        || style->border.bottom || style->border.left;
}

/* The two states differ only in `color` and a solid background colour
   (outlines aside, which the compositor paints): the change a recolour of
   retained commands reproduces. */
static bool focus_color_paint_only(const Stylesheet *sheet,
                                   const ComputedStyle *normal,
                                   const ComputedStyle *focused)
{
    bool color = normal->color != focused->color
        || normal->color_alpha != focused->color_alpha;
    bool background = normal->background != focused->background
        || normal->background_alpha != focused->background_alpha;
    if (!color && !background) return false;
    if (color && (!focus_color_reaches_text_only(sheet, normal)
                  || !focus_color_reaches_text_only(sheet, focused)))
        return false;
    /* A recolour needs a retained background command in both states. */
    if (background && (!normal->has_background || !focused->has_background
            || normal->background_alpha == 0 || focused->background_alpha == 0
            || normal->background_current_color
            || focused->background_current_color
            || normal->background_image != NULL
            || focused->background_image != NULL
            || normal->background_image_kind != STYLE_BACKGROUND_IMAGE_NONE
            || focused->background_image_kind != STYLE_BACKGROUND_IMAGE_NONE))
        return false;
    if (!focus_paint_equal(sheet, normal, focused)) return false;
    ComputedStyle a = *normal, b = *focused;
    a.color = b.color;
    a.color_alpha = b.color_alpha;
    a.background = b.background;
    a.background_alpha = b.background_alpha;
    a.outline_state = b.outline_state = 0;
    a.outline_color = b.outline_color = 0;
    a.outline_alpha = b.outline_alpha = 0;
    /* A currentColor border resolves to `color`: unpainted, it may differ;
       painted, its colour must not (an explicit colour stays the same). */
    if (!focus_style_bordered(normal) && !focus_style_bordered(focused)) {
        a.border_color = b.border_color;
        a.border_alpha = b.border_alpha;
    }
    computed_style_set_paint_stack_id(&a, 0);
    computed_style_set_paint_stack_id(&b, 0);
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* Whether the element's own outline paints the same in both states.
   Layout keeps the retained display list focus-neutral only for an
   outline-only change; under any other change the focused state's outline
   is baked into retained commands that a paint-only skip does not patch. */
static bool focus_outline_painted(const ComputedStyle *style)
{
    bool current = (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0;
    return computed_style_outline_width(style) != 0
        && computed_style_outline_style(style) != STYLE_OUTLINE_NONE
        && (current ? style->color_alpha : style->outline_alpha) != 0;
}

static bool focus_outline_paint_equal(const ComputedStyle *normal,
                                      const ComputedStyle *focused)
{
    /* A transparent outline (the common `outline:1px solid transparent`
       focus idiom) paints nothing in either state. */
    if (!focus_outline_painted(normal) && !focus_outline_painted(focused))
        return true;
    if (normal->outline_state != focused->outline_state) return false;
    if ((normal->outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0)
        return normal->color == focused->color
            && normal->color_alpha == focused->color_alpha;
    return normal->outline_color == focused->outline_color
        && normal->outline_alpha == focused->outline_alpha;
}

static StyleFocusChange focus_change_classify(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused, StyleFocusPseudoTranslation *translation, bool *local,
    bool *pseudo_retry)
{
    if (local != NULL) *local = false;
    if (pseudo_retry != NULL) *pseudo_retry = false;
    if (sheet == NULL || node == NULL || normal == NULL || focused == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, sheet);
    ComputedStyle pseudo_focused[2];
    bool pseudo_ok = true;
    if (translation != NULL) *translation = (StyleFocusPseudoTranslation) {0};

    bool focused_set = focus_override_probe_set(&probe, true);
    bool ok = focused_set;
    if (focused_set) {
        *focused = style_for_node(sheet, node, parent);
        /*
         * Probe dependencies while the node reads focused. This proves both
         * ends of a move because every journaled old and new focus node is
         * independently restored to focused state here; no unfocused-state
         * selector skip can hide removal of a :has()/sibling effect.
         */
        ok = focus_selector_side_effects_are_local(sheet, node, translation != NULL, pseudo_retry);
        if (ok && translation != NULL) {
            for (unsigned i = 0; i < 2; i++)
                pseudo_focused[i] = style_for_pseudo(sheet, node,
                    i == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER, focused);
        }
    }
    bool normal_set = focused_set && focus_override_probe_set(&probe, false);
    ok = ok && normal_set;
    if (normal_set) {
        *normal = style_for_node(sheet, node, parent);
        ok = ok && focus_selector_side_effects_are_local(sheet, node, translation != NULL, pseudo_retry);
        if (ok && translation != NULL) {
            for (unsigned i = 0; i < 2; i++) {
                ComputedStyle pseudo_normal = style_for_pseudo(sheet, node,
                    i == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER, normal);
                /* Selectors may style a pseudo with content:none. No box
                   exists in either state, so its visibility cannot paint. */
                if (!pseudo_normal.generated_content
                    && !pseudo_focused[i].generated_content) continue;
                if (pseudo_normal.visibility_hidden != pseudo_focused[i].visibility_hidden
                    && style_focus_pseudo_visibility_slot(sheet, &pseudo_normal)
                    && style_focus_pseudo_visibility_slot(sheet, &pseudo_focused[i])) {
                    ComputedStyle visible_normal = pseudo_normal;
                    ComputedStyle visible_focused = pseudo_focused[i];
                    visible_normal.visibility_hidden = false;
                    visible_focused.visibility_hidden = false;
                    if (memcmp(&visible_normal, &visible_focused,
                               sizeof(visible_normal)) == 0) {
                        translation->visibility_changed |= (uint8_t) (1u << i);
                        if (pseudo_normal.visibility_hidden)
                            translation->normal_hidden |= (uint8_t) (1u << i);
                        if (pseudo_focused[i].visibility_hidden)
                            translation->focused_hidden |= (uint8_t) (1u << i);
                        translation->visibility_alpha[i] = pseudo_normal.color_alpha;
                        continue;
                    }
                }
                if (!focus_pseudo_translation(sheet, &pseudo_normal, &pseudo_focused[i],
                        &translation->dx[i], &translation->dy[i])) pseudo_ok = false;
            }
        }
    }
    focus_override_probe_end(&probe);
    if (local != NULL) *local = ok;
    if (!ok || !pseudo_ok) return STYLE_FOCUS_CHANGE_UNSAFE;
    if (translation != NULL && (translation->visibility_changed || translation->dx[0] || translation->dx[1]
            || translation->dy[0] || translation->dy[1])
        && (normal->has_transform || normal->has_perspective
            || normal->opacity != 255 || normal->has_filter)) return STYLE_FOCUS_CHANGE_UNSAFE;
    if (computed_style_equal_without_outline(sheet, normal, focused)) {
        return translation != NULL && (translation->visibility_changed || translation->dx[0] || translation->dx[1]
            || translation->dy[0] || translation->dy[1])
            ? STYLE_FOCUS_CHANGE_PSEUDO_TRANSLATION_ONLY : STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
    }

    /*
     * The retained display list can recolour a rounded border without
     * changing command geometry. Keep this list intentionally narrower than
     * the set of CSS paint properties: background, foreground, opacity,
     * filters, transforms and every inherited field still force layout.
     * One zero-offset, unblurred inset ring can use a reserved command slot;
     * complex shadows still need authoritative layout.
     */
    if (!focus_outline_paint_equal(normal, focused)) return STYLE_FOCUS_CHANGE_UNSAFE;
    bool shadow_changed = !focus_paint_equal(sheet, normal, focused);
    size_t normal_shadows = stylesheet_box_shadow_count(sheet, normal);
    size_t focused_shadows = stylesheet_box_shadow_count(sheet, focused);
    const StyleBoxShadow *changed_shadow = shadow_changed
        ? stylesheet_box_shadow(sheet,
            focused_shadows >= normal_shadows ? focused : normal,
            (focused_shadows >= normal_shadows ? focused_shadows : normal_shadows) - 1u)
        : NULL;
    if ((shadow_changed
         && !focus_paint_equal_except_simple_shadow(sheet, normal, focused))
        || normal->opacity != 255 || normal->has_transform || normal->has_filter) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    ComputedStyle a = *normal, b = *focused;
    a.outline_state = b.outline_state = 0;
    a.outline_color = b.outline_color = 0;
    a.outline_alpha = b.outline_alpha = 0;
    a.border_color = b.border_color = 0;
    a.border_alpha = b.border_alpha = 0;
    a.border_color_set = b.border_color_set = 0;
    computed_style_set_paint_stack_id(&a, 0);
    computed_style_set_paint_stack_id(&b, 0);
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        return !shadow_changed && (translation == NULL
                || (!translation->visibility_changed
                    && !translation->dx[0] && !translation->dx[1]
                    && !translation->dy[0] && !translation->dy[1]))
                && focus_color_paint_only(sheet, normal, focused)
            ? STYLE_FOCUS_CHANGE_COLOR_PAINT_ONLY : STYLE_FOCUS_CHANGE_UNSAFE;
    }

    if (normal->border_color == focused->border_color
        && normal->border_alpha == focused->border_alpha
        && normal->border_color_set == focused->border_color_set)
    {
        return !shadow_changed ? STYLE_FOCUS_CHANGE_OUTLINE_ONLY
            : style_box_shadow_is_inset(changed_shadow) ? STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY
            : STYLE_FOCUS_CHANGE_OUTSET_PAINT_ONLY;
    }

    uint32_t normal_color = 0, focused_color = 0;
    uint8_t normal_alpha = 0, focused_alpha = 0;
    if (!focus_style_uniform_rounded_border(
            sheet, normal, &normal_color, &normal_alpha)
        || !focus_style_uniform_rounded_border(
            sheet, focused, &focused_color, &focused_alpha)) {
        return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    /* An opaque rounded border changes background inner-clip geometry.
       Recolour only when crossing that boundary cannot expose a background. */
    if ((normal_alpha == 255) != (focused_alpha == 255)) {
        StylePaintStack paint = style_paint_stack_copy(sheet, normal);
        if (normal->background_alpha != 0 || normal->background_image != NULL
            || normal->background_image_kind != STYLE_BACKGROUND_IMAGE_NONE
            || paint.background_count != 0) return STYLE_FOCUS_CHANGE_UNSAFE;
    }
    if (shadow_changed) {
        return style_box_shadow_is_inset(changed_shadow) ? STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY
            : STYLE_FOCUS_CHANGE_OUTSET_PAINT_ONLY;
    }
    return normal_color != focused_color || normal_alpha != focused_alpha
        ? STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
        : STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
}

/* Elements whose rendering the text recolour cannot reach: replaced and
   form content, markers, quotes, and anything hosting a shadow tree. An
   image paints no colour; its alt text, when shown, borrows its alt
   attribute, which style_focus_color_texts lists beside the text nodes. */
static bool focus_color_element_plain(lxb_dom_node_t *node,
                                      const ComputedStyle *style)
{
    switch (node->local_name) {
    case LXB_TAG_SVG:
    case LXB_TAG_MATH: case LXB_TAG_CANVAS: case LXB_TAG_VIDEO:
    case LXB_TAG_AUDIO: case LXB_TAG_IFRAME: case LXB_TAG_OBJECT:
    case LXB_TAG_EMBED: case LXB_TAG_INPUT: case LXB_TAG_TEXTAREA:
    case LXB_TAG_SELECT: case LXB_TAG_OPTION: case LXB_TAG_OPTGROUP:
    case LXB_TAG_METER: case LXB_TAG_PROGRESS: case LXB_TAG_Q:
    case LXB_TAG_SUMMARY: case LXB_TAG_DETAILS: case LXB_TAG_SLOT:
    case LXB_TAG_TEMPLATE:
        return false;
    default:
        return node->local_name >= LXB_TAG__BEGIN
            && node->local_name < LXB_TAG__LAST_ENTRY
            && !style->shadow_host && !style->list_item;
    }
}

/* Whether ::before or ::after of `node` generates a box from `parent`. */
static bool focus_color_generates(const Stylesheet *sheet,
                                  lxb_dom_node_t *node,
                                  const ComputedStyle *parent)
{
    for (unsigned i = 0; i < 2; i++) {
        PseudoElement pseudo = i == 0 ? PSEUDO_BEFORE : PSEUDO_AFTER;
        if (style_pseudo_may_generate(sheet, pseudo)
            && style_for_pseudo(sheet, node, pseudo, parent).generated_content)
            return true;
    }
    return false;
}

static bool focus_color_subtree(const Stylesheet *sheet,
    lxb_dom_node_t *element, const ComputedStyle *normal,
    const ComputedStyle *focused, bool follows, StyleFocusColorTexts *texts,
    unsigned depth, size_t *elements)
{
    enum { DEPTH_LIMIT = 6, ELEMENT_LIMIT = 24 };
    for (lxb_dom_node_t *child = element->first_child; child != NULL;
         child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_node_t **list = follows ? texts->texts : texts->kept;
            size_t *count = follows ? &texts->text_count : &texts->kept_count;
            if (*count == STYLE_FOCUS_COLOR_TEXT_LIMIT) return false;
            list[(*count)++] = child;
            continue;
        }
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (depth >= DEPTH_LIMIT || ++*elements > ELEMENT_LIMIT) return false;
        /* No focus rule reaches a descendant (the change is local), so its
           two styles differ only through what it inherits. */
        ComputedStyle child_normal = style_for_node(sheet, child, normal);
        ComputedStyle child_focused = style_for_node(sheet, child, focused);
        if (!focus_color_element_plain(child, &child_normal)) return false;
        bool child_follows = memcmp(&child_normal, &child_focused,
                                    sizeof(child_normal)) != 0;
        if (child_follows
            && (child_normal.color != normal->color
                || child_normal.color_alpha != normal->color_alpha
                || child_focused.color != focused->color
                || child_focused.color_alpha != focused->color_alpha
                || child_normal.background != child_focused.background
                || child_normal.background_alpha
                       != child_focused.background_alpha
                || !focus_color_paint_only(sheet, &child_normal,
                                           &child_focused)
                || focus_color_generates(sheet, child, &child_normal)))
            return false;
        if (child->local_name == LXB_TAG_IMG) {
            /* Its alt text, when painted, borrows the alt attribute. */
            lxb_dom_node_t **list = child_follows ? texts->texts : texts->kept;
            size_t *count = child_follows ? &texts->text_count
                                          : &texts->kept_count;
            if (*count == STYLE_FOCUS_COLOR_TEXT_LIMIT) return false;
            list[(*count)++] = child;
        }
        if (!focus_color_subtree(sheet, child, &child_normal, &child_focused,
                                 child_follows, texts, depth + 1u, elements))
            return false;
    }
    return true;
}

bool style_focus_color_texts(const Stylesheet *sheet, lxb_dom_node_t *node,
                             const ComputedStyle *normal,
                             const ComputedStyle *focused,
                             StyleFocusColorTexts *texts)
{
    if (texts != NULL) texts->text_count = texts->kept_count = 0;
    if (sheet == NULL || node == NULL || normal == NULL || focused == NULL
        || texts == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !focus_color_element_plain(node, normal)
        || !focus_color_element_plain(node, focused)) return false;
    if (normal->color == focused->color
        && normal->color_alpha == focused->color_alpha) return true;
    /* Generated content inherits the colour: resolve the element's pseudos
       in both focus states. */
    if (style_pseudo_may_generate(sheet, PSEUDO_BEFORE)
        || style_pseudo_may_generate(sheet, PSEUDO_AFTER)) {
        FocusOverrideProbe probe;
        focus_override_probe_begin(&probe, node, sheet);
        bool generates = !focus_override_probe_set(&probe, true)
            || focus_color_generates(sheet, node, focused)
            || !focus_override_probe_set(&probe, false)
            || focus_color_generates(sheet, node, normal);
        focus_override_probe_end(&probe);
        if (generates) return false;
    }
    size_t elements = 0;
    return focus_color_subtree(sheet, node, normal, focused, true, texts,
                               0u, &elements);
}

StyleFocusChange style_focus_change_classify(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal, ComputedStyle *focused)
{
    return focus_change_classify(sheet, node, parent, normal, focused, NULL, NULL, NULL);
}

StyleFocusChange style_focus_pseudo_change_classify(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal, ComputedStyle *focused,
    StyleFocusPseudoTranslation *translation, bool *local)
{
    if (local != NULL) *local = false;
    if (translation == NULL) return STYLE_FOCUS_CHANGE_UNSAFE;
    *translation = (StyleFocusPseudoTranslation) {0};
    bool pseudo_retry = false;
    StyleFocusChange change = focus_change_classify(
        sheet, node, parent, normal, focused, NULL, local, &pseudo_retry);
    /* Ordinary controls need no pseudo styles. A geometry, dependency or
       stacking-order refusal cannot be repaired by resolving pseudos. */
    if (change != STYLE_FOCUS_CHANGE_UNSAFE || !pseudo_retry) return change;
    return focus_change_classify(sheet, node, parent, normal, focused, translation, local, NULL);
}

bool style_focus_change_is_local(const Stylesheet *sheet, lxb_dom_node_t *node)
{
    ComputedStyle normal, focused;
    StyleFocusPseudoTranslation translation;
    bool local;
    (void) focus_change_classify(sheet, node, NULL, &normal, &focused, &translation, &local, NULL);
    return local;
}

static bool focus_rule_may_reach_others(const Stylesheet *sheet,
    lxb_dom_node_t *node, size_t index, size_t *visits)
{
    const StyleRule *rule = &sheet->rules[index];
    const char *selector = rule->selector;
    size_t length = rule->selector_length;
    for (size_t at = 0; at < length; at++) {
        size_t token_length = 0;
        bool within = false;
        if (!focus_token_at(selector, length, at, &token_length, &within)) continue;
        if (focus_token_reach(sheet, node, selector, length, at, within,
                style_rule_rightmost_compound(rule), NULL,
                rule->pseudo == PSEUDO_NONE, rule) == FOCUS_TOKEN_NONLOCAL)
            return !focus_selector_reads_focus_once(selector, length)
                || !focus_rule_vacuous_forward(sheet, node, index, visits);
        at += token_length - 1u;
    }
    return false;
}

size_t style_focus_nonlocal_rules(const Stylesheet *sheet,
    lxb_dom_node_t *node, uint32_t *rules, size_t capacity)
{
    enum { RULE_LIMIT = 128 };
    if (sheet == NULL || node == NULL || rules == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || sheet->scopes.adopted_shadow_only_mask != 0 || sheet->scopes.shadow_scope_count != 0
        || style_adopted_scopes_document_only(sheet)
        || !stylesheet_prepare_focus_rule_index((Stylesheet *) sheet)
        || sheet->focus_rule_count > RULE_LIMIT) return SIZE_MAX;
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, NULL);
    bool affected[RULE_LIMIT] = {false};
    bool ok = true;
    size_t visits = FOCUS_VACUITY_VISIT_LIMIT;
    for (unsigned state = 0; ok && state < 2; state++) {
        for (size_t i = 0; i < sheet->focus_rule_count; i++) {
            const StyleRule *rule = &sheet->rules[sheet->focus_rule_indices[i]];
            if (strstr(rule->selector, ":scope") != NULL
                || strstr(rule->selector, ":host") != NULL) {
                ok = false;
                break;
            }
            affected[i] = affected[i] || focus_rule_may_reach_others(
                sheet, node, sheet->focus_rule_indices[i], &visits);
        }
        if (state == 0 && ok) ok = focus_override_probe_set(&probe, !probe.focused);
    }
    focus_override_probe_end(&probe);
    if (!ok) return SIZE_MAX;
    size_t count = 0;
    for (size_t i = 0; i < sheet->focus_rule_count; i++) {
        if (!affected[i]) continue;
        if (count == capacity) return SIZE_MAX;
        rules[count++] = sheet->focus_rule_indices[i];
    }
    return count;
}

/* Whether a focus-reading custom-declaration rule can restyle an element
   other than `node` when `node`'s focus changes, in the probe's current
   state (the custom-rule half of focus_rule_may_reach_others). */
static bool focus_custom_rule_may_reach_others(const Stylesheet *sheet,
    lxb_dom_node_t *node, const StyleCustomRule *rule)
{
    const char *selector = rule->selector;
    size_t length = strlen(selector);
    size_t rightmost = style_selector_rightmost_compound_start(
        selector, length);
    for (size_t at = 0; at < length; at++) {
        size_t token_length = 0;
        bool within = false;
        if (!focus_token_at(selector, length, at, &token_length, &within))
            continue;
        if (focus_token_reach(sheet, node, selector, length, at, within,
                rightmost, NULL, false, NULL) == FOCUS_TOKEN_NONLOCAL)
            return true;
        at += token_length - 1u;
    }
    return false;
}

#define FOCUS_CONFINED_CUSTOM_LIMIT 64u
#define FOCUS_CONFINED_VISIT_LIMIT 2048u

bool style_focus_restyle_confined(const Stylesheet *sheet,
    lxb_dom_node_t *node, StyleFocusSettled settled, void *context)
{
    enum { RULE_LIMIT = 128 };
    if (sheet == NULL || node == NULL || settled == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    uint32_t rules[RULE_LIMIT];
    size_t rule_count = style_focus_nonlocal_rules(sheet, node, rules,
                                                   RULE_LIMIT);
    if (rule_count == SIZE_MAX) return false;
    uint32_t custom[FOCUS_CONFINED_CUSTOM_LIMIT];
    size_t custom_count = 0;
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, NULL);
    for (unsigned state = 0; state < 2; state++) {
        for (size_t i = 0; i < sheet->custom_rule_count; i++) {
            const StyleCustomRule *rule = &sheet->custom_rules[i];
            if (!style_selector_reads_focus(rule->selector)) continue;
            bool listed = false;
            for (size_t j = 0; !listed && j < custom_count; j++)
                listed = custom[j] == (uint32_t) i;
            if (listed || !focus_custom_rule_may_reach_others(
                    sheet, node, rule)) continue;
            if (custom_count == FOCUS_CONFINED_CUSTOM_LIMIT) {
                focus_override_probe_end(&probe);
                return false;
            }
            custom[custom_count++] = (uint32_t) i;
        }
        if (state == 0 && !focus_override_probe_set(&probe, !probe.focused))
            break;
    }
    focus_override_probe_end(&probe);
    if (rule_count == 0 && custom_count == 0) return true;
    /* Only a forward rule's subjects are confined to a walk: the moved
       node's subtree (settled with it) and its later siblings' subtrees. */
    if (node->parent == NULL) return false;
    for (size_t i = 0; i < rule_count; i++)
        if (!style_focus_selector_reaches_forward(
                sheet->rules[rules[i]].selector)) return false;
    for (size_t i = 0; i < custom_count; i++)
        if (!style_focus_selector_reaches_forward(
                sheet->custom_rules[custom[i]].selector)) return false;
    /* Every element of that range the caller has not settled must be
       selected by none of these rules in either state. Custom rules are
       tested by their fast key, a superset of their subjects in both. */
    size_t visits = FOCUS_CONFINED_VISIT_LIMIT;
    bool confined = true;
    focus_override_probe_begin(&probe, node, NULL);
    for (unsigned state = 0; confined && state < 2; state++) {
        for (lxb_dom_node_t *top = node->next; confined && top != NULL;
             top = top->next) {
            for (lxb_dom_node_t *at = top; at != NULL;) {
                bool descend = false;
                if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
                    && !settled(at, context)) {
                    if (visits == 0) {
                        confined = false;
                        break;
                    }
                    visits--;
                    for (size_t i = 0; confined && i < rule_count; i++)
                        confined = !style_rule_selector_matches(
                            sheet, rules[i], at);
                    if (confined && state == 0 && custom_count != 0)
                        confined = !style_custom_rules_may_select_node(
                            sheet, custom, custom_count, at);
                    if (!confined) break;
                    descend = true;
                }
                /* A settled element's subtree is settled with it. */
                if (descend && at->first_child != NULL) {
                    at = at->first_child;
                    continue;
                }
                while (at != top && at->next == NULL) at = at->parent;
                if (at == top) break;
                at = at->next;
            }
        }
        if (confined && state == 0
            && !focus_override_probe_set(&probe, !probe.focused)) break;
    }
    focus_override_probe_end(&probe);
    return confined;
}

bool style_focus_selector_reaches_forward(const char *selector)
{
    if (selector == NULL) return false;
    size_t length = strlen(selector);
    /* Per open parenthesis: whether it is a plain boolean wrapper. */
    bool plain[16];
    size_t depth = 0;
    for (size_t at = 0; at < length; at++) {
        char c = selector[at];
        if (c == '\\') return false;
        if (c == '(') {
            if (depth == sizeof(plain) / sizeof(plain[0])) return false;
            plain[depth++] =
                (at >= 3u && memcmp(selector + at - 3u, ":is", 3u) == 0)
                || (at >= 4u && memcmp(selector + at - 4u, ":not", 4u) == 0)
                || (at >= 6u && memcmp(selector + at - 6u, ":where", 6u) == 0);
            continue;
        }
        if (c == ')') {
            if (depth == 0) return false;
            depth--;
            continue;
        }
        if (c != ':' && c != '[') continue;
        /* Shadow-crossing scopes reach outside the light tree. */
        if ((at + 5u <= length && memcmp(selector + at, ":host", 5u) == 0)
            || (at + 6u <= length && memcmp(selector + at, ":scope", 6u) == 0)
            || (at + 9u <= length && memcmp(selector + at, "::slotted", 9u) == 0)
            || (at + 6u <= length && memcmp(selector + at, "::part", 6u) == 0))
            return false;
        size_t token_length = 0;
        bool within = false;
        bool marker = c == '['
            && at + 1u + sizeof(STYLE_FOCUS_MARKER) - 1u <= length
            && memcmp(selector + at + 1u, STYLE_FOCUS_MARKER,
                      sizeof(STYLE_FOCUS_MARKER) - 1u) == 0;
        if (!marker && !focus_token_at(selector, length, at, &token_length,
                                       &within)) continue;
        if (within) return false;
        for (size_t i = 0; i < depth; i++)
            if (!plain[i]) return false;
    }
    return depth == 0;
}

bool style_focus_rules_restyle_by_lists(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->scopes.adopted_shadow_only_mask != 0
        || sheet->scopes.shadow_scope_count != 0
        || style_adopted_scopes_document_only(sheet)
        || !stylesheet_prepare_focus_rule_index((Stylesheet *) sheet))
        return false;
    for (size_t i = 0; i < sheet->focus_rule_count; i++) {
        const char *selector = sheet->rules[sheet->focus_rule_indices[i]].selector;
        if (strstr(selector, ":has(") != NULL || strstr(selector, ":scope") != NULL
            || strstr(selector, ":host") != NULL) return false;
    }
    return true;
}

/* A non-local ordinary focus rule need not invalidate unrelated variable
   inheritance. Prove every variable-bearing focus selector inert in both
   focus states; ambiguous relative/ancestor scopes keep the full reset. */
static bool focus_custom_properties_inert(const Stylesheet *sheet,
                                          lxb_dom_node_t *node)
{
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        if (strncmp(rule->name, "--", 2) != 0
            || !style_selector_reads_focus(rule->selector)) continue;
        if (rule->pseudo != PSEUDO_NONE
            || strstr(rule->selector, ":scope") != NULL
            || strstr(rule->selector, ":host") != NULL
            || strstr(rule->selector, "::slotted") != NULL) return false;
        const char *selector = rule->selector;
        size_t length = strlen(selector);
        size_t rightmost = style_selector_rightmost_compound_start(selector, length);
        for (size_t at = 0; at < length; at++) {
            size_t token_length = 0;
            bool within = false;
            if (!focus_token_at(selector, length, at, &token_length, &within)) continue;
            FocusTokenReach reach = focus_token_reach(sheet, node, selector,
                length, at, within, rightmost, NULL, false, NULL);
            if (reach == FOCUS_TOKEN_NONLOCAL
                || (reach == FOCUS_TOKEN_SUBJECT
                    && style_selector_matches(node, selector, length)))
                return false;
            at += token_length - 1u;
        }
    }
    return true;
}

bool style_focus_custom_properties_unchanged(const Stylesheet *sheet,
                                            lxb_dom_node_t *node)
{
    if (sheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || sheet->scopes.adopted_shadow_only_mask != 0 || sheet->scopes.shadow_scope_count != 0
        || style_adopted_scopes_document_only(sheet)) return false;
    FocusOverrideProbe probe;
    focus_override_probe_begin(&probe, node, NULL);
    bool ok = focus_custom_properties_inert(sheet, node)
        && focus_override_probe_set(&probe, !probe.focused)
        && focus_custom_properties_inert(sheet, node);
    focus_override_probe_end(&probe);
    return ok;
}

bool style_focus_change_is_outline_only(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused)
{
    return style_focus_change_classify(
        sheet, node, parent, normal, focused)
        == STYLE_FOCUS_CHANGE_OUTLINE_ONLY;
}
