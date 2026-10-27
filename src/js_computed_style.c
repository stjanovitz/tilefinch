/* getComputedStyle(): the registry of computed properties, the per-read
   memo and the ancestor-style ring with its mutation-scoped invalidation
   (js_rt_bridge_computed_style_note_mutation, called from the mutation
   journal), the transition snapshot, value resolution and serialization,
   and HTML innerText, which reads styles through the same ring. Split from
   js_dom_bindings.c; shares the runtime internals through
   js_runtime_internal.h. */
#include "style_internal.h"
#include "style_cache_internal.h"
#include "js_runtime_internal.h"
#include "tilefinch/platform.h"
#include "tilefinch_compiler.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void computed_style_scroll_start_sides(
    const ComputedStyle *style, unsigned *block, unsigned *in_line)
{
    unsigned mode = computed_style_writing_mode(style);
    bool rtl = computed_style_direction_rtl(style);
    *block = mode == STYLE_WRITING_HORIZONTAL_TB ? 0u
        : mode == STYLE_WRITING_VERTICAL_RL ? 1u : 3u;
    *in_line = mode == STYLE_WRITING_HORIZONTAL_TB ? (rtl ? 1u : 3u)
        : (rtl ? 2u : 0u);
}

static bool retained_scroll_box_value(
    const Stylesheet *stylesheet, lxb_dom_node_t *node,
    const ComputedStyle *style,
    const char *name, size_t name_length, char *output, size_t capacity)
{
    size_t base_length = name_length >= 13
        && strncasecmp(name, "scroll-margin", 13) == 0 ? 13u
        : name_length >= 14
          && strncasecmp(name, "scroll-padding", 14) == 0 ? 14u : 0u;
    if (base_length == 0) return false;
    int side = -1;
    if (name_length != base_length) {
        if (name[base_length] != '-') return false;
        static const char *const sides[] = {"top", "right", "bottom", "left"};
        for (size_t i = 0; i < 4; i++) {
            if (strcmp(name + base_length + 1u, sides[i]) == 0) side = (int) i;
        }
        if (side < 0) return false;
    }
    char shorthand[15];
    memcpy(shorthand, name, base_length);
    shorthand[base_length] = '\0';
    unsigned block, in_line;
    computed_style_scroll_start_sides(style, &block, &in_line);
    const char *suffixes[4];
    suffixes[block] = "block-start";
    suffixes[block ^ 2u] = "block-end";
    suffixes[in_line] = "inline-start";
    suffixes[in_line ^ 2u] = "inline-end";
    char alias_names[4][40];
    const char *aliases[4];
    for (size_t i = 0; i < 4; i++) {
        snprintf(alias_names[i], sizeof(alias_names[i]), "%s-%s",
                 shorthand, suffixes[i]);
        aliases[i] = alias_names[i];
    }
    StyleRetainedBoxValues values;
    if (!style_retained_box_values(
            stylesheet, node, shorthand, aliases, &values)) return false;
    if (side >= 0) {
        if ((values.present_mask & (1u << side)) == 0) return false;
        int written = snprintf(output, capacity, "%s", values.values[side]);
        return written >= 0 && (size_t) written < capacity;
    }
    if (values.present_mask != 0x0fu) return false;
    const char *top = values.values[0], *right = values.values[1];
    const char *bottom = values.values[2], *left = values.values[3];
    int written;
    if (strcmp(top, right) == 0 && strcmp(top, bottom) == 0
        && strcmp(top, left) == 0) {
        written = snprintf(output, capacity, "%s", top);
    } else if (strcmp(top, bottom) == 0 && strcmp(right, left) == 0) {
        written = snprintf(output, capacity, "%s %s", top, right);
    } else if (strcmp(right, left) == 0) {
        written = snprintf(
            output, capacity, "%s %s %s", top, right, bottom);
    } else {
        written = snprintf(
            output, capacity, "%s %s %s %s",
            top, right, bottom, left);
    }
    return written >= 0 && (size_t) written < capacity;
}

static int serialize_computed_color(char *output, size_t capacity,
                                    uint32_t color, unsigned alpha)
{
    unsigned red = (unsigned) ((color >> 16) & 255u);
    unsigned green = (unsigned) ((color >> 8) & 255u);
    unsigned blue = (unsigned) (color & 255u);
    return alpha == 255
        ? snprintf(output, capacity, "rgb(%u, %u, %u)",
                   red, green, blue)
        : snprintf(output, capacity, "rgba(%u, %u, %u, %.3g)",
                   red, green, blue, (double) alpha / 255.0);
}

/* Layout records the actual positioning ancestor, which can differ from the
   DOM parent. Reuse its padding box without another cascade. */
static int computed_style_positioning_width(
    DomBridge *bridge, const lxb_dom_node_t *node,
    const LayoutNodeBox *box, int flow_width)
{
    if (box == NULL || box->positioned_ancestor_distance == 0)
        return flow_width;
    const LayoutDocument *layout = js_rt_bridge_layout(bridge);
    if (layout == NULL) return flow_width;
    if (box->positioned_ancestor_distance == UINT8_MAX)
        return layout->width;
    const lxb_dom_node_t *ancestor = node;
    for (unsigned step = box->positioned_ancestor_distance;
         step != 0 && ancestor != NULL; step--) ancestor = ancestor->parent;
    const LayoutNodeBox *parent = ancestor == NULL ? NULL
        : layout_box_for_node(layout, ancestor);
    return parent == NULL ? 0 : parent->client_width;
}

/* A ring of resolved ancestor styles, retained for the page's lifetime:
   64 x 348 bytes = 22 KiB on the PSP (384 bytes, 24 KiB on 64-bit hosts).
   ChatGPT send (1251 reads) misses 590 of 5330 lookups at 64, 128 and 256
   slots alike (594 at 48) in the same 17 ms; misses there come from
   mutation clears, not capacity. Only a whole-document sweep (715
   elements, three passes) gains from more (2264 -> 1914 misses, 22 ->
   17 ms host at 256), which does not pay for 87 KiB on the PSP. A bounded
   index now avoids scanning the replacement ring on each lookup. */
#define COMPUTED_STYLE_CACHE_SLOTS 64u
_Static_assert(COMPUTED_STYLE_CACHE_SLOTS <= UINT8_MAX
    && (COMPUTED_STYLE_CACHE_SLOTS & (COMPUTED_STYLE_CACHE_SLOTS - 1u)) == 0
    && sizeof(((DomBridge *) 0)->computed_style_cache.buckets)
        == COMPUTED_STYLE_CACHE_SLOTS
    && sizeof(((DomBridge *) 0)->computed_style_cache.links)
        == COMPUTED_STYLE_CACHE_SLOTS,
    "computed-style index must match its bounded replacement ring");
struct ComputedStyleCacheEntry {
    const lxb_dom_node_t *node;
    ComputedStyle style;
    /* computed_style_parent_key() of the parent style it was resolved
       from, and the cache epoch it was last known valid in. */
    uint64_t parent_key;
    uint32_t stamp;
};

/* The identity of a parent style, as the layout reuse cache keys its
   entries (layout_style_parent_hash): two 32-bit FNV lanes over the bytes.
   Unequal padding can only cost a recomputation. */
static uint64_t computed_style_parent_key(const ComputedStyle *style)
{
    if (style == NULL) return UINT64_C(0x9e3779b97f4a7c15);
    const unsigned char *bytes = (const unsigned char *) style;
    uint32_t low = UINT32_C(2166136261);
    uint32_t high = UINT32_C(0x85ebca6b);
    size_t length = sizeof(*style);
    while (length >= sizeof(uint32_t)) {
        uint32_t word = 0;
        memcpy(&word, bytes, sizeof(word));
        low = (low ^ word) * UINT32_C(16777619);
        high = (high ^ (word + UINT32_C(0x9e3779b9)))
               * UINT32_C(2246822519);
        bytes += sizeof(word);
        length -= sizeof(word);
    }
    while (length-- != 0) {
        low = (low ^ *bytes) * UINT32_C(16777619);
        high = (high ^ (uint32_t) (*bytes++ + 0x9eu)) * UINT32_C(2246822519);
    }
    return ((uint64_t) high << 32) | low;
}

static size_t computed_style_cache_bucket(const lxb_dom_node_t *node)
{
    uintptr_t key = (uintptr_t) node >> 3;
    key ^= key >> 7;
    key ^= key >> 13;
    return key & (COMPUTED_STYLE_CACHE_SLOTS - 1u);
}

static void computed_style_cache_remove(DomBridge *bridge, size_t slot)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    const lxb_dom_node_t *node = cache->entries[slot].node;
    if (node == NULL) return;
    uint8_t *link = &cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; *link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = *link - 1u;
        if (at == slot) {
            *link = cache->links[at];
            break;
        }
        link = &cache->links[at];
    }
    cache->entries[slot].node = NULL;
    cache->links[slot] = 0;
}

/* The ancestor cache for this read, emptied when any cascade input moved
   (the computed-style memo's key); NULL when styles depend on layout
   (container queries and units) or no Budget room is left. */
/* Which selector features the cache's scoped invalidation must respect,
   as the layout reuse cache reads them (layout_reuse_note_selector_
   dependencies): :has() can restyle ancestors, structural pseudo-classes
   and sibling combinators restyle siblings, focus selectors restyle
   through state the mutation journal does not name. */
/* The key of the element a structural test in `selector` at `at`
   concerns: the compound holding `at` (a positional pseudo-class), or the
   one after it (a sibling combinator). Zero when no key bounds it. */
static void bridge_computed_style_note_key(uint32_t *keys, uint8_t *count,
                                           bool *any, uint32_t key)
{
    if (key == 0) {
        *any = true;
        return;
    }
    for (uint8_t i = 0; i < *count; i++)
        if (keys[i] == key) return;
    if (*count == COMPUTED_STYLE_CUSTOM_KEY_LIMIT) {
        *any = true;
        return;
    }
    keys[(*count)++] = key;
}

/* Every structural test in a custom-property selector, as keys of the
   elements whose custom properties it can switch: `sibling` only for the
   tests an attribute change can move (sibling combinators, `of S`
   counts), otherwise positional pseudo-classes too. */
static void bridge_computed_style_custom_keys(
    const char *selector, bool sibling, uint32_t *keys, uint8_t *count,
    bool *any)
{
    size_t length = strlen(selector);
    for (size_t at = 0; at < length && !*any; at++) {
        char value = selector[at];
        if (value == '+' || value == '~') {
            size_t next = at + 1;
            while (next < length && isspace((unsigned char) selector[next]))
                next++;
            bridge_computed_style_note_key(
                keys, count, any, next < length
                    ? style_selector_compound_key_at(selector, length, next)
                    : 0);
        } else if (value == ':') {
            const char *rest = selector + at;
            bool of = strncmp(rest, ":nth-child(", 11) == 0
                || strncmp(rest, ":nth-last-child(", 16) == 0;
            bool positional = strncmp(rest, ":nth-", 5) == 0
                || strncmp(rest, ":first-", 7) == 0
                || strncmp(rest, ":last-", 6) == 0
                || strncmp(rest, ":only-", 6) == 0
                || strncmp(rest, ":empty", 6) == 0;
            if (positional && (!sibling || of))
                bridge_computed_style_note_key(
                    keys, count, any,
                    style_selector_compound_key_at(selector, length, at));
        }
    }
}

static void bridge_computed_style_flags(DomBridge *bridge)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    const Stylesheet *sheet = bridge->stylesheet;
    if (cache->flags_sheet == (const void *) sheet
        && cache->flags_generation == sheet->build_generation) return;
    cache->flags_sheet = sheet;
    cache->flags_generation = sheet->build_generation;
    cache->flags_has = cache->flags_structure = cache->flags_focus = false;
    cache->flags_sibling = cache->flags_empty_sibling = false;
    cache->empty_key_count = 0;
    cache->empty_any = false;
    cache->flags_custom_has = cache->flags_has_escaped = false;
    cache->custom_sibling_key_count = cache->custom_structure_key_count = 0;
    cache->custom_sibling_any = cache->custom_structure_any = false;
    memset(&cache->structure, 0, sizeof(cache->structure));
    memset(&cache->sibling_structure, 0, sizeof(cache->sibling_structure));
    for (size_t pass = 0; pass < 2; pass++) {
        size_t count = pass == 0 ? sheet->count : sheet->custom_rule_count;
        for (size_t i = 0; i < count; i++) {
            const char *selector = pass == 0 ? sheet->rules[i].selector
                                             : sheet->custom_rules[i].selector;
            if (selector == NULL || strpbrk(selector, ":+~") == NULL)
                continue;
            if (strstr(selector, ":has(") != NULL) {
                cache->flags_has = true;
                if (pass == 1) cache->flags_custom_has = true;
                /* The :has() classifier compares attribute names as
                   written; an escaped selector keeps every change
                   conservative. */
                if (strchr(selector, '\\') != NULL)
                    cache->flags_has_escaped = true;
            }
            if (strstr(selector, ":focus") != NULL) cache->flags_focus = true;
            bool empty = strstr(selector, ":empty") != NULL
                || strstr(selector, ":blank") != NULL;
            bool sibling = strchr(selector, '+') != NULL
                || strchr(selector, '~') != NULL
                || strstr(selector, " of ") != NULL;
            bool structure = sibling || empty
                || strstr(selector, ":nth-") != NULL
                || strstr(selector, ":first-") != NULL
                || strstr(selector, ":last-") != NULL
                || strstr(selector, ":only-") != NULL;
            if (!structure) continue;
            cache->flags_structure = true;
            /* An element's :empty can restyle outside its own subtree only
               through a sibling test (layout_reuse_empty_reaches_siblings):
               keyed by the compound that tests it. */
            if (empty && sibling) {
                cache->flags_empty_sibling = true;
                size_t length = strlen(selector);
                for (const char *at = selector; !cache->empty_any
                     && (at = strpbrk(at, ":")) != NULL; at++) {
                    if (strncmp(at, ":empty", 6) != 0
                        && strncmp(at, ":blank", 6) != 0) continue;
                    bridge_computed_style_note_key(
                        cache->empty_keys, &cache->empty_key_count,
                        &cache->empty_any,
                        style_selector_compound_key_at(
                            selector, length, (size_t) (at - selector)));
                }
            }
            /* As the layout reuse cache notes them
               (layout_reuse_note_selector_dependencies). A '+' inside an
               An+B argument counts too: conservative. */
            style_selector_structure_keys(selector, strlen(selector),
                                          &cache->structure);
            if (sibling) {
                cache->flags_sibling = true;
                style_selector_structure_keys(selector, strlen(selector),
                                              &cache->sibling_structure);
            }
            if (pass == 1) {
                bridge_computed_style_custom_keys(
                    selector, false, cache->custom_structure_keys,
                    &cache->custom_structure_key_count,
                    &cache->custom_structure_any);
                if (sibling)
                    bridge_computed_style_custom_keys(
                        selector, true, cache->custom_sibling_keys,
                        &cache->custom_sibling_key_count,
                        &cache->custom_sibling_any);
            }
        }
    }
}

/* Whether a structural test that reaches descendants (`.a + .b .c`,
   `tr:last-child > *`) can concern one of `parent`'s children, as
   layout_reuse_structure_deep asks: then a change to a child restyles its
   siblings' subtrees, not only the siblings. Past the child bound, assume
   it can. */
static bool bridge_computed_style_structure_deep(
    const StyleStructureKeys *keys, const lxb_dom_node_t *parent)
{
    if (!keys->reaches) return false;
    if (keys->any) return true;
    size_t children = 0;
    for (const lxb_dom_node_t *child = parent->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (++children > 512u) return true;
        if (style_element_carries_any_key_unsorted(child, keys->keys, keys->count))
            return true;
    }
    return false;
}

/* Queue `node` for a subtree drop; false when the list is full. */
static bool bridge_computed_style_scope(DomBridge *bridge,
                                        lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    for (size_t i = 0; i < cache->dirty_count; i++)
        if (cache->dirty[i] == node) return true;
    if (cache->dirty_count == sizeof(cache->dirty) / sizeof(cache->dirty[0]))
        return false;
    cache->dirty[cache->dirty_count++] = node;
    return true;
}

/* Queue `node`'s own entry for a drop; false when the list is full. */
static bool bridge_computed_style_shallow(DomBridge *bridge,
                                          lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    for (size_t i = 0; i < cache->shallow_count; i++)
        if (cache->shallow[i] == node) return true;
    if (cache->shallow_count
        == sizeof(cache->shallow) / sizeof(cache->shallow[0]))
        return false;
    cache->shallow[cache->shallow_count++] = node;
    return true;
}

/* An element whose :has()-dependent match may move: its own entry, or its
   subtree where a custom-property rule can be the one that moves. */
static void bridge_computed_style_has_drop(void *opaque, lxb_dom_node_t *node)
{
    DomBridge *bridge = opaque;
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    bool queued = cache->flags_custom_has
        ? bridge_computed_style_scope(bridge, node)
        : bridge_computed_style_shallow(bridge, node);
    if (!queued) cache->dirty_all = true;
}

static struct ComputedStyleCacheEntry *computed_style_cache_find(
    DomBridge *bridge, const lxb_dom_node_t *node);

/* Whether any retained style belongs to `scope` or its subtree. A change
   whose reach lies inside a subtree nothing was read from has nothing to
   drop (entries are only added by reads, which apply the drops first). */
static bool bridge_computed_style_cached_within(const DomBridge *bridge,
                                                const lxb_dom_node_t *scope)
{
    const struct ComputedStyleCacheEntry *entries =
        bridge->computed_style_cache.entries;
    for (size_t i = 0; entries != NULL && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        if (entries[i].node != NULL
            && js_rt_node_within(entries[i].node, scope)) return true;
    return false;
}

/* A change at `node` (`tree`: inserted or about to be removed; otherwise
   an attribute a sibling test reads) that structural tests let reach its
   siblings. The siblings' own styles go and their subtrees re-key off
   them, as in layout's reuse cache, unless a test reaches their
   descendants or a custom-property rule can switch a sibling's custom
   properties (not in ComputedStyle: that sibling's subtree goes). False
   when only the parent's subtree covers it. */
static bool bridge_computed_style_siblings(DomBridge *bridge,
                                           lxb_dom_node_t *node,
                                           lxb_dom_node_t *parent, bool tree)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (parent == NULL || parent->type != LXB_DOM_NODE_TYPE_ELEMENT
        || bridge_computed_style_structure_deep(
               tree ? &cache->structure : &cache->sibling_structure, parent))
        return false;
    bool custom_any = tree ? cache->custom_structure_any
                           : cache->custom_sibling_any;
    const uint32_t *custom_keys = tree ? cache->custom_structure_keys
                                       : cache->custom_sibling_keys;
    size_t custom_count = tree ? cache->custom_structure_key_count
                               : cache->custom_sibling_key_count;
    if (custom_any || !bridge_computed_style_shallow(bridge, parent)
        || !bridge_computed_style_scope(bridge, node)) return false;
    size_t children = 0;
    for (lxb_dom_node_t *child = parent->first_child; child != NULL;
         child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || child == node)
            continue;
        if (++children > 512u) return false;
        if (custom_count != 0
            && style_element_carries_any_key_unsorted(child, custom_keys,
                                             custom_count)) {
            if (!bridge_computed_style_scope(bridge, child)) return false;
        } else if (computed_style_cache_find(bridge, child) != NULL
                   && !bridge_computed_style_shallow(bridge, child)) {
            /* Only a cached sibling has anything to drop. */
            return false;
        }
    }
    return true;
}

/* A connected mutation: remember what cached styles it can change, instead
   of emptying the cache (pages mutate between reads). */
void js_rt_bridge_computed_style_note_mutation(
    DomBridge *bridge, ScriptMutationKind kind, lxb_dom_node_t *node,
    const char *attribute, size_t attribute_length, bool relational,
    const uint32_t *changed_tokens, size_t changed_token_count)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (cache->entries == NULL || cache->dirty_all) return;
    if (bridge->stylesheet == NULL || node == NULL) {
        cache->dirty_all = true;
        return;
    }
    /* Retained nodes are followed (below) only while the captured inputs
       name this document and sheet, as in the flush: otherwise the next
       read clears everything without touching them. */
    if (cache->inputs.sheet != (const void *) bridge->stylesheet
        || cache->inputs.dom != (const void *) (bridge->document == NULL
                                                ? NULL
                                                : bridge->document->html))
        return;
    bridge_computed_style_flags(bridge);
    /* A change inside a shadow tree can change which of the host's light
       children render (shadow composition): those are outside its scope. */
    if (bridge->shadow_root_count != 0
        && document_shadow_carrier_containing(node) != NULL) {
        cache->dirty_all = true;
        return;
    }
    bool known = kind == SCRIPT_MUTATION_ATTRIBUTE
        || kind == SCRIPT_MUTATION_INLINE_STYLE
        || kind == SCRIPT_MUTATION_TEXT
        || kind == SCRIPT_MUTATION_CHILD_LIST
        || kind == SCRIPT_MUTATION_INNER_HTML;
    /* Engine state (focus, hover and similar) travels as data-tilefinch-*
       attributes and can restyle through selectors a subtree scope
       misses. */
    bool engine_state = attribute != NULL && attribute_length >= 15u
        && strncasecmp(attribute, "data-tilefinch-", 15u) == 0;
    /* Unobserved author bookkeeping, a data-* attribute no selector tests,
       cannot change a cascade (layout's reuse check treats it the same way;
       see navigation/layout_reuse.inc). Engine state keeps its semantics. */
    if (kind == SCRIPT_MUTATION_ATTRIBUTE && !engine_state
        && attribute != NULL && attribute_length > 5u
        && strncasecmp(attribute, "data-", 5u) == 0
        && !stylesheet_selectors_reference_attribute_prefix(
               bridge->stylesheet, attribute, attribute_length))
        return;
    if (!known || (engine_state && cache->flags_focus)) {
        cache->dirty_all = true;
        return;
    }
    bool tree = kind == SCRIPT_MUTATION_TEXT
        || kind == SCRIPT_MUTATION_CHILD_LIST
        || kind == SCRIPT_MUTATION_INNER_HTML;
    /* Nothing retained, nothing to drop. */
    bool retained = false;
    for (size_t i = 0; !retained && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        retained = cache->entries[i].node != NULL;
    if (!retained) return;
    /* A change a :has() rule may observe drops the elements whose answers
       can move, found from the changed position as layout's reuse cache
       finds them (style_has_invalidation.c), rather than every style. */
    if (relational && cache->flags_has && cache->flags_has_escaped) {
        cache->dirty_all = true;
        return;
    }
    if (relational && cache->flags_has) {
        char name[64];
        const char *has_attribute = NULL;
        /* An inline-style record names the CSS property; the attribute a
           selector can read is `style`. */
        if (kind == SCRIPT_MUTATION_INLINE_STYLE) {
            has_attribute = "style";
        } else if (!tree && attribute != NULL
                   && attribute_length < sizeof(name)) {
            for (size_t i = 0; i < attribute_length; i++)
                name[i] = (char) tolower((unsigned char) attribute[i]);
            name[attribute_length] = '\0';
            has_attribute = name;
        }
        bool exact_tokens = changed_tokens != NULL
            && changed_token_count <= SCRIPT_MUTATION_TOKEN_LIMIT;
        if (
#ifndef TILEFINCH_NO_TRACE
            getenv("TILEFINCH_DISABLE_HAS_SCOPED_INVALIDATION") != NULL ||
#endif
            !style_has_note_change(
                bridge->stylesheet, node, tree, has_attribute,
                exact_tokens ? changed_tokens : NULL,
                exact_tokens ? changed_token_count : 0,
                &cache->has_pending, bridge_computed_style_has_drop, bridge,
                NULL)) {
            cache->dirty_all = true;
            return;
        }
        cache->has_active = cache->has_pending.active;
        if (cache->dirty_all) return;
    }
    lxb_dom_node_t *scope = node;
    lxb_dom_node_t *parent = node->parent;
    /* Everything below reaches at most the grandparent's subtree. */
    lxb_dom_node_t *reach = parent == NULL ? node
        : (parent->parent != NULL ? parent->parent : parent);
    if (!bridge_computed_style_cached_within(bridge, reach)) return;
    /* A child list or text change can flip the parent's :empty, which a
       sibling test turns into its siblings' styles: the grandparent's
       subtree. */
    bool empty_reaches = tree && cache->flags_empty_sibling && parent != NULL
        && parent->type == LXB_DOM_NODE_TYPE_ELEMENT
        && (cache->empty_any
            || style_element_carries_any_key_unsorted(parent, cache->empty_keys,
                                             cache->empty_key_count));
    if (empty_reaches) {
        scope = parent->parent != NULL ? parent->parent : parent;
    } else if (kind == SCRIPT_MUTATION_CHILD_LIST
               && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        /* An element inserted or about to leave: its subtree, the parent
           (:empty) and, when structural tests exist, the siblings whose
           positions move. */
        if (!cache->flags_structure
            ? bridge_computed_style_shallow(bridge, parent)
                && bridge_computed_style_scope(bridge, node)
            : bridge_computed_style_siblings(bridge, node, parent, true))
            return;
        scope = parent;
    } else if (tree || engine_state) {
        if (parent != NULL) scope = parent;
    } else if (cache->flags_sibling && parent != NULL) {
        /* An attribute can reach other elements' matches only through a
           sibling combinator or an `of S` count (positional
           pseudo-classes read positions, :has() is handled above). */
        if (bridge_computed_style_siblings(bridge, node, parent, false))
            return;
        scope = parent;
    }
    if (scope == NULL || !bridge_computed_style_scope(bridge, scope))
        cache->dirty_all = true;
}

/* The container states' signature for the sheet's current container
   generation. Every layout pass rebuilds the states (bumping the
   generation) although they seldom change; retained styles compare what
   container queries and units can read instead. */
static uint64_t bridge_container_signature(DomBridge *bridge)
{
    const Stylesheet *sheet = bridge->stylesheet;
    if (bridge->computed_style_container_sheet != (const void *) sheet
        || bridge->computed_style_container_generation
               != sheet->container_state_generation) {
        bridge->computed_style_container_sheet = sheet;
        bridge->computed_style_container_generation =
            sheet->container_state_generation;
        bridge->computed_style_container_signature =
            style_container_layout_state_signature(sheet);
    }
    return bridge->computed_style_container_signature;
}

/* Whether retained styles keyed by `inputs` still hold; a container
   generation whose states did not change is adopted in place. */
static bool computed_style_inputs_hold(DomBridge *bridge,
                                       ComputedStyleInputs *inputs)
{
    const Stylesheet *sheet = bridge->stylesheet;
    if (inputs->sheet != (const void *) sheet
        || inputs->dom != (const void *) (bridge->document == NULL ? NULL
                                          : bridge->document->html)
        || inputs->sheet_generation != sheet->build_generation
        || inputs->fullscreen_node != sheet->fullscreen_node
        || inputs->host_generation != document_style_generation())
        return false;
    if (inputs->container_generation == sheet->container_state_generation)
        return true;
    if (inputs->container_signature != bridge_container_signature(bridge))
        return false;
    inputs->container_generation = sheet->container_state_generation;
    return true;
}

static void computed_style_inputs_capture(DomBridge *bridge,
                                          ComputedStyleInputs *inputs)
{
    const Stylesheet *sheet = bridge->stylesheet;
    *inputs = (ComputedStyleInputs) {
        .sheet = sheet,
        .dom = bridge->document == NULL ? NULL : bridge->document->html,
        .sheet_generation = sheet->build_generation,
        .container_generation = sheet->container_state_generation,
        .container_signature = bridge_container_signature(bridge),
        .host_generation = document_style_generation(),
        .fullscreen_node = sheet->fullscreen_node
    };
}

/* Lexbor is removing `node` from its parent, possibly to destroy it: drop
   its retained style before the address can name another node. Descendants
   stay: they are still allocated, and whatever reconnects them is a
   mutation the cache sees. */
static void bridge_computed_style_node_removed(void *opaque,
                                               const lxb_dom_node_t *node)
{
    DomBridge *bridge = opaque;
    if (bridge->computed_style_memo.node == node)
        bridge->computed_style_memo.node = NULL;
    if (bridge->transition_ancestors_memo.parent == node)
        bridge->transition_ancestors_memo.parent = NULL;
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (cache->entries == NULL) return;
    /* Queued drops must not name a node whose address can be reused. */
    style_has_pending_retire(&cache->has_pending, node);
    for (size_t i = 0; i < cache->shallow_count; i++) {
        if (cache->shallow[i] != node) continue;
        cache->shallow[i] = cache->shallow[--cache->shallow_count];
        break;
    }
    uint8_t link = cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = link - 1u;
        if (cache->entries[at].node == node) {
            computed_style_cache_remove(bridge, at);
            return;
        }
        link = cache->links[at];
    }
}

static struct ComputedStyleCacheEntry *bridge_computed_style_cache(
    DomBridge *bridge)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    if (bridge->stylesheet == NULL) return NULL;
    if (cache->entries == NULL) {
        /* Entries outlive script entries, so a node must be evicted when
           Lexbor removes it; without a listener slot there is no ring. */
        if (!cache->removal_listening) {
            cache->removal_listening = document_node_removal_listen(
                bridge_computed_style_node_removed, bridge);
            if (!cache->removal_listening) return NULL;
        }
        cache->entries = budget_calloc(bridge->budget,
                                       COMPUTED_STYLE_CACHE_SLOTS,
                                       sizeof(*cache->entries));
        if (cache->entries == NULL) return NULL;
        cache->inputs.sheet = NULL;
    }
    uint64_t content_generation = bridge->document == NULL
        ? 0 : bridge->document->content_generation;
    bool same_inputs = computed_style_inputs_hold(bridge, &cache->inputs);
    if (cache->has_active && cache->has_pending.styles_all)
        cache->dirty_all = true;
    if (same_inputs && cache->content_generation != content_generation
        && !cache->dirty_all) {
        /* Only the mutated subtrees' styles can have changed, and the own
           styles of the elements queued alone: their descendants are
           checked against their parents' styles when next read. Keyed
           :has() subjects go alone too, or with their subtrees when a
           custom-property rule can be what moved. */
        const lxb_dom_node_t *subjects[COMPUTED_STYLE_CACHE_SLOTS];
        size_t subject_count = 0;
        for (size_t i = 0; i < COMPUTED_STYLE_CACHE_SLOTS; i++) {
            const lxb_dom_node_t *node = cache->entries[i].node;
            if (node == NULL) continue;
            bool drop = false;
            for (size_t d = 0; !drop && d < cache->dirty_count; d++)
                drop = js_rt_node_within(node, cache->dirty[d]);
            for (size_t d = 0; !drop && d < cache->shallow_count; d++)
                drop = node == cache->shallow[d];
            if (!drop && cache->has_active
                && style_has_pending_selects(bridge->stylesheet,
                                             &cache->has_pending, node)) {
                drop = true;
                if (cache->flags_custom_has)
                    subjects[subject_count++] = node;
            }
            if (drop) computed_style_cache_remove(bridge, i);
        }
        for (size_t i = 0; subject_count != 0
                           && i < COMPUTED_STYLE_CACHE_SLOTS; i++) {
            const lxb_dom_node_t *node = cache->entries[i].node;
            for (size_t s = 0; node != NULL && s < subject_count; s++) {
                if (!js_rt_node_within(node, subjects[s])) continue;
                computed_style_cache_remove(bridge, i);
                break;
            }
        }
        cache->dirty_count = 0;
        cache->shallow_count = 0;
        cache->has_active = false;
        memset(&cache->has_pending, 0, sizeof(cache->has_pending));
        cache->content_generation = content_generation;
        cache->scoped_clears++;
        cache->epoch++;
        style_variable_cache_lease_clear(&cache->variables);
    }
    if (!same_inputs || cache->content_generation != content_generation) {
        if (cache->inputs.sheet != NULL
            && cache->inputs.host_generation != document_style_generation())
            cache->host_clears++;
        cache->full_clears++;
        cache->dirty_count = 0;
        cache->shallow_count = 0;
        cache->has_active = false;
        memset(&cache->has_pending, 0, sizeof(cache->has_pending));
        cache->epoch++;
        cache->dirty_all = false;
        style_variable_cache_lease_clear(&cache->variables);
        for (size_t i = 0; i < COMPUTED_STYLE_CACHE_SLOTS; i++)
            cache->entries[i].node = NULL;
        memset(cache->buckets, 0, sizeof(cache->buckets));
        memset(cache->links, 0, sizeof(cache->links));
        computed_style_inputs_capture(bridge, &cache->inputs);
        cache->content_generation = content_generation;
        cache->next = 0;
    }
    return cache->entries;
}

void js_rt_bridge_computed_style_cache_free(DomBridge *bridge)
{
    if (bridge == NULL) return;
    if (bridge->computed_style_cache.removal_listening)
        document_node_removal_unlisten(bridge_computed_style_node_removed,
                                       bridge);
    bridge->computed_style_cache.removal_listening = false;
    style_variable_cache_lease_release(&bridge->computed_style_cache.variables);
    budget_free(bridge->budget, bridge->computed_style_cache.entries);
    bridge->computed_style_cache.entries = NULL;
    bridge->computed_style_memo.node = NULL;
    bridge->transition_ancestors_memo.parent = NULL;
}

bool js_rt_bridge_computed_style_cache_holds(const DomBridge *bridge,
                                             const lxb_dom_node_t *node)
{
    if (bridge == NULL || node == NULL) return false;
    if (bridge->computed_style_memo.node == node) return true;
    for (size_t i = 0; bridge->computed_style_cache.entries != NULL
         && i < COMPUTED_STYLE_CACHE_SLOTS; i++)
        if (bridge->computed_style_cache.entries[i].node == node) return true;
    return false;
}

size_t js_rt_bridge_computed_style_cache_bytes(const DomBridge *bridge)
{
    if (bridge == NULL) return 0;
    return (bridge->computed_style_cache.entries == NULL ? 0
            : COMPUTED_STYLE_CACHE_SLOTS
                * sizeof(*bridge->computed_style_cache.entries))
        + style_variable_cache_lease_bytes(
            &bridge->computed_style_cache.variables);
}

static struct ComputedStyleCacheEntry *computed_style_cache_find(
    DomBridge *bridge, const lxb_dom_node_t *node)
{
    __typeof__(bridge->computed_style_cache) *cache =
        &bridge->computed_style_cache;
    uint8_t link = cache->buckets[computed_style_cache_bucket(node)];
    for (size_t visited = 0; link != 0 && visited < COMPUTED_STYLE_CACHE_SLOTS;
         visited++) {
        size_t at = link - 1u;
        cache->lookup_probes++;
        if (cache->entries[at].node == node) return &cache->entries[at];
        link = cache->links[at];
    }
    return NULL;
}

static bool bridge_computed_style(DomBridge *bridge,
                                  lxb_dom_node_t *node, ComputedStyle *result,
                                  int *containing_width, int *content_width)
{
    struct ComputedStyleCacheEntry *cache =
        bridge_computed_style_cache(bridge);
    const uint32_t epoch = bridge->computed_style_cache.epoch;
    if (cache != NULL && containing_width == NULL) {
        const struct ComputedStyleCacheEntry *hit =
            computed_style_cache_find(bridge, node);
        if (hit != NULL && hit->stamp == epoch) {
            *result = hit->style;
            /* A hit skips style_for_node's per-node unit basis setup.
               Inline units may exist without the authored-sheet flag. */
            style_container_units_for_node(
                (Stylesheet *) bridge->stylesheet, node);
            bridge->computed_style_cache.hits++;
            return true;
        }
    }
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node; at != NULL && count < 64;
         at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) ancestors[count++] = at;
    }
    ComputedStyle computed = {0};
    bool have_parent = false;
    const LayoutDocument *settled = js_rt_bridge_layout(bridge);
    int flow_width = settled == NULL ? 0 : settled->width;
    /* var() lookups resolve by matching custom-property rules up the
       ancestor chain. A layout build memoizes them per build; this keeps
       them across reads under the cache's validity (see above). Lending
       allocates nothing: the first var() lookup creates the table. */
    __typeof__(bridge->computed_style_cache) *state =
        &bridge->computed_style_cache;
    state->variables.budget = bridge->budget;
    bool variable_cache = cache != NULL && style_variable_cache_attach(
        (Stylesheet *) bridge->stylesheet, &state->variables);
    /* Without geometry, start from the nearest ancestor known valid in
       this epoch. */
    if (cache != NULL && containing_width == NULL) {
        for (size_t at = 1; at < count; at++) {
            const struct ComputedStyleCacheEntry *hit =
                computed_style_cache_find(bridge, ancestors[at]);
            if (hit == NULL || hit->stamp != epoch) continue;
            computed = hit->style;
            have_parent = true;
            bridge->computed_style_cache.hits++;
            count = at;
            break;
        }
    }
    while (count != 0) {
        lxb_dom_node_t *at = ancestors[--count];
        struct ComputedStyleCacheEntry *hit = cache == NULL ? NULL
            : computed_style_cache_find(bridge, at);
        /* An entry from an earlier epoch still holds when the parent
           style it was resolved from is the parent's current style: its
           own inputs are unchanged unless a drop removed it. */
        uint64_t parent_key = cache == NULL ? 0
            : computed_style_parent_key(have_parent ? &computed : NULL);
        if (hit != NULL
            && (hit->stamp == epoch || hit->parent_key == parent_key)) {
            computed = hit->style;
            hit->stamp = epoch;
            style_container_units_for_node(
                (Stylesheet *) bridge->stylesheet, at);
            bridge->computed_style_cache.hits++;
        } else {
            /* Each inherited style can run a substantial selector scan.
               Native calls do not hit VM opcode polls, so service the
               existing bounded task watchdog between ancestors rather than
               hiding the whole chain inside one uninterruptible
               getComputedStyle call. */
            if (bridge->host != NULL
                && !js_rt_runtime_native_checkpoint(bridge->host)) {
                if (variable_cache)
                    style_variable_cache_detach(
                        (Stylesheet *) bridge->stylesheet, &state->variables);
                return false;
            }
            computed = style_for_node(bridge->stylesheet, at,
                                      have_parent ? &computed : NULL);
            bridge->computed_style_cache.misses++;
            if (hit != NULL) {
                /* A stale entry is refreshed in place. */
                hit->style = computed;
                hit->parent_key = parent_key;
                hit->stamp = epoch;
            } else if (cache != NULL) {
                __typeof__(bridge->computed_style_cache) *state =
                    &bridge->computed_style_cache;
                computed_style_cache_remove(bridge, state->next);
                cache[state->next].node = at;
                cache[state->next].style = computed;
                cache[state->next].parent_key = parent_key;
                cache[state->next].stamp = epoch;
                size_t bucket = computed_style_cache_bucket(at);
                state->links[state->next] = state->buckets[bucket];
                state->buckets[bucket] = (uint8_t) (state->next + 1u);
                state->next = (state->next + 1u) % COMPUTED_STYLE_CACHE_SLOTS;
            }
        }
        have_parent = true;
        /* Only geometry readers need this work. Resolve each ancestor's
           padding while its cascade is already in hand; cancellation stays
           on the single checked walk above. No scratch styles or second
           selector pass are needed for nested percentage padding. */
        if (containing_width != NULL && settled != NULL) {
            const LayoutNodeBox *box = layout_box_for_node(settled, at);
            /* An inline box or a display: contents element is not a
               containing block: percentages inside it refer to the nearest
               block container, so it passes the width through unchanged. */
            bool container = computed.display != DISPLAY_INLINE
                && computed.display != DISPLAY_CONTENTS;
            int basis = computed_style_positioning_width(
                bridge, at, box, flow_width);
            *containing_width = basis;
            if (box != NULL && (container || at == node)) {
                int left = 0, right = 0;
                (void) style_length_resolve(bridge->stylesheet,
                    computed.padding.left, basis, &left);
                (void) style_length_resolve(bridge->stylesheet,
                    computed.padding.right, basis, &right);
                int64_t width = (int64_t) box->client_width
                    - (left > 0 ? left : 0) - (right > 0 ? right : 0);
                flow_width = width > 0 ? (int) width : 0;
            }
            *content_width = flow_width;
        }
    }
    if (variable_cache)
        style_variable_cache_detach((Stylesheet *) bridge->stylesheet,
                                    &state->variables);
    *result = computed;
    return true;
}

static bool svg_presentation_attribute_relevant(
    lxb_dom_node_t *node, const char *property, size_t length)
{
    if (node == NULL || property == NULL || node->ns != LXB_NS_SVG) {
        return false;
    }
    size_t tag_length = 0;
    const char *tag = document_element_name(node, &tag_length);
#define SVG_TAG(wanted) \
    (tag != NULL && tag_length == sizeof(wanted) - 1u \
     && strncasecmp(tag, (wanted), sizeof(wanted) - 1u) == 0)
#define SVG_PROP(wanted) js_rt_property_name_equal( \
    property, length, (wanted), sizeof(wanted) - 1u)
    bool text = SVG_TAG("text") || SVG_TAG("tspan") || SVG_TAG("textPath");
    bool graphics = SVG_TAG("g") || SVG_TAG("path") || SVG_TAG("rect")
        || SVG_TAG("circle") || SVG_TAG("ellipse") || SVG_TAG("line")
        || SVG_TAG("polyline") || SVG_TAG("polygon") || text;
    if (SVG_PROP("display") || SVG_PROP("visibility")) return graphics;
    if (SVG_PROP("clip-path") || SVG_PROP("clip-rule")
        || SVG_PROP("color") || SVG_PROP("cursor")
        || SVG_PROP("fill") || SVG_PROP("fill-opacity")
        || SVG_PROP("fill-rule")
        || SVG_PROP("filter") || SVG_PROP("mask")
        || SVG_PROP("opacity") || SVG_PROP("pointer-events")
        || SVG_PROP("stroke") || SVG_PROP("stroke-dasharray")
        || SVG_PROP("stroke-dashoffset") || SVG_PROP("stroke-linecap")
        || SVG_PROP("stroke-linejoin") || SVG_PROP("stroke-miterlimit")
        || SVG_PROP("stroke-opacity") || SVG_PROP("stroke-width")
        || SVG_PROP("transform") || SVG_PROP("vector-effect")) {
        return graphics;
    }
    if (SVG_PROP("direction") || SVG_PROP("font-family")
        || SVG_PROP("font-size") || SVG_PROP("font-size-adjust")
        || SVG_PROP("font-stretch") || SVG_PROP("font-style")
        || SVG_PROP("font-variant") || SVG_PROP("font-weight")
        || SVG_PROP("letter-spacing") || SVG_PROP("text-anchor")
        || SVG_PROP("text-decoration") || SVG_PROP("text-overflow")
        || SVG_PROP("text-rendering") || SVG_PROP("unicode-bidi")
        || SVG_PROP("white-space") || SVG_PROP("word-spacing")
        || SVG_PROP("writing-mode") || SVG_PROP("alignment-baseline")
        || SVG_PROP("baseline-shift") || SVG_PROP("dominant-baseline")
        || SVG_PROP("glyph-orientation-vertical")) return text;
    if (SVG_PROP("overflow")) return SVG_TAG("svg");
    if (SVG_PROP("cx") || SVG_PROP("cy") || SVG_PROP("r")) {
        return SVG_TAG("circle");
    }
    if (SVG_PROP("rx") || SVG_PROP("ry") || SVG_PROP("x")
        || SVG_PROP("y") || SVG_PROP("width") || SVG_PROP("height")) {
        return SVG_TAG("rect");
    }
    if (SVG_PROP("d")
        || SVG_PROP("marker-start") || SVG_PROP("marker-mid")
        || SVG_PROP("marker-end") || SVG_PROP("paint-order")
        || SVG_PROP("shape-rendering")) return SVG_TAG("path");
    if (SVG_PROP("stop-color") || SVG_PROP("stop-opacity")) {
        return SVG_TAG("stop");
    }
    if (SVG_PROP("flood-color") || SVG_PROP("flood-opacity")) {
        return SVG_TAG("feFlood");
    }
    if (SVG_PROP("lighting-color")) {
        return SVG_TAG("feDiffuseLighting")
            || SVG_TAG("feSpecularLighting");
    }
    if (SVG_PROP("color-interpolation")) {
        return SVG_TAG("linearGradient") || SVG_TAG("radialGradient");
    }
    if (SVG_PROP("color-interpolation-filters")) return SVG_TAG("filter");
    if (SVG_PROP("mask-type")) return SVG_TAG("mask");
    if (SVG_PROP("image-rendering")) return SVG_TAG("image");
    return false;
#undef SVG_PROP
#undef SVG_TAG
}

/* Write a scaled integer as the shortest decimal with at most three
   fractional digits: 2000/1000 -> "2", 1536/512 -> "3", 171/512 -> "0.334". */
static void computed_style_format_scaled(char *output, size_t output_size,
                                         unsigned long value,
                                         unsigned long scale)
{
    unsigned long whole = value / scale;
    unsigned long thousandths =
        ((value % scale) * 1000ul + scale / 2ul) / scale;
    if (thousandths >= 1000ul) {
        whole++;
        thousandths = 0;
    }
    if (thousandths == 0) {
        snprintf(output, output_size, "%lu", whole);
        return;
    }
    char digits[4];
    snprintf(digits, sizeof(digits), "%03lu", thousandths);
    size_t length = 3;
    while (length > 0 && digits[length - 1] == '0') digits[--length] = '\0';
    snprintf(output, output_size, "%lu.%s", whole, digits);
}

/* The properties js_computed_style_get() resolves for an ordinary rendered
   element, in strcmp order. This one table answers every CSSOM question about
   the *set* of supported properties -- `name in style`, `length`, `item()`,
   iteration -- so membership can never drift from a second list kept in
   script. `longhand` is false for shorthands and legacy aliases, which are
   attributes of the declaration but are not enumerated.

   It lives in read-only data rather than the realm heap: script asks for the
   enumeration only when a page actually enumerates a computed style, which is
   rare, and asks membership questions without materializing anything.

   tests/suites/foundation_document.inc requires every entry to resolve to a
   non-empty value; add a name here only together with its serializer. */
enum {
    /* Resolved from retained authored text or a shared initial value rather
       than a ComputedStyle field. */
    CSP_SPARSE_SCROLL = 1u << 0,
    CSP_SPARSE_MODERN = 1u << 1,
    /* A used value: needs a layout even when none has been built yet. */
    CSP_USED_GEOMETRY = 1u << 2,
    /* Resolves from the current DOM and cascade alone, so a pending DOM
       mutation does not have to be laid out first. Never a property whose
       value is a used size or depends on a box. */
    CSP_STYLE_ONLY    = 1u << 3
};

/* One row per property: identifier, name, longhand, flags. Rows are in strcmp
   order of the name, because lookup is a binary search. The enumeration, the
   table and the identifiers the getter dispatches on are all generated from
   this list, so a serializer can only be written for a registered property. */
#define COMPUTED_STYLE_PROPERTIES(X) \
    X(WEBKIT_APPEARANCE, "-webkit-appearance", false, 0) \
    X(WEBKIT_BACKDROP_FILTER, "-webkit-backdrop-filter", false, CSP_SPARSE_MODERN) \
    X(WEBKIT_LINE_CLAMP, "-webkit-line-clamp", true, 0) \
    X(WEBKIT_TEXT_SIZE_ADJUST, "-webkit-text-size-adjust", false, CSP_SPARSE_MODERN) \
    X(WEBKIT_USER_SELECT, "-webkit-user-select", false, CSP_SPARSE_MODERN) \
    X(ALIGN_CONTENT, "align-content", true, CSP_STYLE_ONLY) \
    X(ALIGN_ITEMS, "align-items", true, CSP_STYLE_ONLY) \
    X(ALIGN_SELF, "align-self", true, CSP_STYLE_ONLY) \
    X(APPEARANCE, "appearance", true, 0) \
    X(ASPECT_RATIO, "aspect-ratio", true, 0) \
    X(BACKDROP_FILTER, "backdrop-filter", true, CSP_SPARSE_MODERN) \
    X(BACKFACE_VISIBILITY, "backface-visibility", true, CSP_SPARSE_MODERN) \
    X(BACKGROUND_CLIP, "background-clip", true, 0) \
    X(BACKGROUND_COLOR, "background-color", true, CSP_STYLE_ONLY) \
    X(BACKGROUND_IMAGE, "background-image", true, 0) \
    X(BACKGROUND_ORIGIN, "background-origin", true, 0) \
    X(BACKGROUND_POSITION, "background-position", true, 0) \
    X(BACKGROUND_SIZE, "background-size", true, 0) \
    X(BORDER_BOTTOM_COLOR, "border-bottom-color", true, 0) \
    X(BORDER_BOTTOM_LEFT_RADIUS, "border-bottom-left-radius", true, 0) \
    X(BORDER_BOTTOM_RIGHT_RADIUS, "border-bottom-right-radius", true, 0) \
    X(BORDER_BOTTOM_STYLE, "border-bottom-style", true, 0) \
    X(BORDER_BOTTOM_WIDTH, "border-bottom-width", true, 0) \
    X(BORDER_COLLAPSE, "border-collapse", true, 0) \
    X(BORDER_END_END_RADIUS, "border-end-end-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_END_START_RADIUS, "border-end-start-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE, "border-image", false, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_OUTSET, "border-image-outset", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_REPEAT, "border-image-repeat", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_SLICE, "border-image-slice", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_SOURCE, "border-image-source", true, CSP_SPARSE_MODERN) \
    X(BORDER_IMAGE_WIDTH, "border-image-width", true, CSP_SPARSE_MODERN) \
    X(BORDER_LEFT_COLOR, "border-left-color", true, 0) \
    X(BORDER_LEFT_STYLE, "border-left-style", true, 0) \
    X(BORDER_LEFT_WIDTH, "border-left-width", true, 0) \
    X(BORDER_RADIUS, "border-radius", false, 0) \
    X(BORDER_RIGHT_COLOR, "border-right-color", true, 0) \
    X(BORDER_RIGHT_STYLE, "border-right-style", true, 0) \
    X(BORDER_RIGHT_WIDTH, "border-right-width", true, 0) \
    X(BORDER_SPACING, "border-spacing", true, 0) \
    X(BORDER_START_END_RADIUS, "border-start-end-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_START_START_RADIUS, "border-start-start-radius", true, CSP_SPARSE_MODERN) \
    X(BORDER_TOP_COLOR, "border-top-color", true, 0) \
    X(BORDER_TOP_LEFT_RADIUS, "border-top-left-radius", true, 0) \
    X(BORDER_TOP_RIGHT_RADIUS, "border-top-right-radius", true, 0) \
    X(BORDER_TOP_STYLE, "border-top-style", true, 0) \
    X(BORDER_TOP_WIDTH, "border-top-width", true, 0) \
    X(BORDER_WIDTH, "border-width", false, 0) \
    X(BOTTOM, "bottom", true, 0) \
    X(BOX_SHADOW, "box-shadow", true, 0) \
    X(BOX_SIZING, "box-sizing", true, CSP_STYLE_ONLY) \
    X(CAPTION_SIDE, "caption-side", true, 0) \
    X(CLIP_PATH, "clip-path", true, 0) \
    X(COLOR, "color", true, CSP_STYLE_ONLY) \
    X(COLOR_SCHEME, "color-scheme", true, CSP_SPARSE_MODERN) \
    X(COLUMN_GAP, "column-gap", true, 0) \
    X(CONTAIN, "contain", true, 0) \
    X(CONTENT, "content", true, 0) \
    X(CONTENT_VISIBILITY, "content-visibility", true, 0) \
    X(CURSOR, "cursor", true, CSP_SPARSE_SCROLL) \
    X(DIRECTION, "direction", true, CSP_STYLE_ONLY) \
    X(DISPLAY, "display", true, CSP_STYLE_ONLY) \
    X(FILTER, "filter", true, 0) \
    X(FLEX, "flex", false, 0) \
    X(FLEX_BASIS, "flex-basis", true, 0) \
    X(FLEX_DIRECTION, "flex-direction", true, CSP_STYLE_ONLY) \
    X(FLEX_GROW, "flex-grow", true, 0) \
    X(FLEX_SHRINK, "flex-shrink", true, 0) \
    X(FLEX_WRAP, "flex-wrap", true, CSP_STYLE_ONLY) \
    X(FLOAT, "float", true, 0) \
    X(FONT_FAMILY, "font-family", true, 0) \
    X(FONT_KERNING, "font-kerning", true, CSP_SPARSE_MODERN) \
    X(FONT_SIZE, "font-size", true, CSP_STYLE_ONLY) \
    X(FONT_STYLE, "font-style", true, CSP_STYLE_ONLY) \
    X(FONT_WEIGHT, "font-weight", true, CSP_STYLE_ONLY) \
    X(GAP, "gap", false, 0) \
    X(GRID_TEMPLATE_AREAS, "grid-template-areas", true, 0) \
    X(GRID_TEMPLATE_COLUMNS, "grid-template-columns", true, CSP_USED_GEOMETRY) \
    X(GRID_TEMPLATE_ROWS, "grid-template-rows", true, 0) \
    X(HEIGHT, "height", true, CSP_USED_GEOMETRY) \
    X(HYPHENS, "hyphens", true, CSP_SPARSE_MODERN) \
    X(ISOLATION, "isolation", true, CSP_SPARSE_MODERN) \
    X(JUSTIFY_CONTENT, "justify-content", true, CSP_STYLE_ONLY) \
    X(JUSTIFY_ITEMS, "justify-items", true, 0) \
    X(JUSTIFY_SELF, "justify-self", true, 0) \
    X(LEFT, "left", true, 0) \
    X(LETTER_SPACING, "letter-spacing", true, CSP_STYLE_ONLY) \
    X(LINE_HEIGHT, "line-height", true, CSP_STYLE_ONLY) \
    X(LIST_STYLE_POSITION, "list-style-position", true, 0) \
    X(LIST_STYLE_TYPE, "list-style-type", true, 0) \
    X(MARGIN, "margin", false, 0) \
    X(MARGIN_BOTTOM, "margin-bottom", true, 0) \
    X(MARGIN_LEFT, "margin-left", true, 0) \
    X(MARGIN_RIGHT, "margin-right", true, 0) \
    X(MARGIN_TOP, "margin-top", true, 0) \
    X(MAX_HEIGHT, "max-height", true, 0) \
    X(MAX_WIDTH, "max-width", true, 0) \
    X(MIN_HEIGHT, "min-height", true, 0) \
    X(MIN_WIDTH, "min-width", true, 0) \
    X(MIX_BLEND_MODE, "mix-blend-mode", true, CSP_SPARSE_MODERN) \
    X(OBJECT_FIT, "object-fit", true, 0) \
    X(OBJECT_POSITION, "object-position", true, 0) \
    X(OPACITY, "opacity", true, CSP_STYLE_ONLY) \
    X(ORDER, "order", true, CSP_STYLE_ONLY) \
    X(OUTLINE_COLOR, "outline-color", true, 0) \
    X(OUTLINE_OFFSET, "outline-offset", true, 0) \
    X(OUTLINE_STYLE, "outline-style", true, 0) \
    X(OUTLINE_WIDTH, "outline-width", true, 0) \
    X(OVERFLOW, "overflow", false, CSP_STYLE_ONLY) \
    X(OVERFLOW_X, "overflow-x", true, CSP_STYLE_ONLY) \
    X(OVERFLOW_Y, "overflow-y", true, CSP_STYLE_ONLY) \
    X(OVERSCROLL_BEHAVIOR, "overscroll-behavior", false, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_BLOCK, "overscroll-behavior-block", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_INLINE, "overscroll-behavior-inline", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_X, "overscroll-behavior-x", true, CSP_SPARSE_SCROLL) \
    X(OVERSCROLL_BEHAVIOR_Y, "overscroll-behavior-y", true, CSP_SPARSE_SCROLL) \
    X(PADDING, "padding", false, 0) \
    X(PADDING_BOTTOM, "padding-bottom", true, 0) \
    X(PADDING_LEFT, "padding-left", true, 0) \
    X(PADDING_RIGHT, "padding-right", true, 0) \
    X(PADDING_TOP, "padding-top", true, 0) \
    X(PERSPECTIVE, "perspective", true, 0) \
    X(PLACE_CONTENT, "place-content", false, 0) \
    X(PLACE_ITEMS, "place-items", false, 0) \
    X(PLACE_SELF, "place-self", false, 0) \
    X(POINTER_EVENTS, "pointer-events", true, CSP_STYLE_ONLY) \
    X(POSITION, "position", true, CSP_STYLE_ONLY) \
    X(RESIZE, "resize", true, CSP_SPARSE_MODERN) \
    X(RIGHT, "right", true, 0) \
    X(ROTATE, "rotate", true, CSP_SPARSE_MODERN) \
    X(ROW_GAP, "row-gap", true, 0) \
    X(SCALE, "scale", true, CSP_SPARSE_MODERN) \
    X(SCROLL_BEHAVIOR, "scroll-behavior", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN, "scroll-margin", false, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_BLOCK_END, "scroll-margin-block-end", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_BLOCK_START, "scroll-margin-block-start", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_BOTTOM, "scroll-margin-bottom", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_INLINE_END, "scroll-margin-inline-end", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_INLINE_START, "scroll-margin-inline-start", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_LEFT, "scroll-margin-left", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_RIGHT, "scroll-margin-right", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_MARGIN_TOP, "scroll-margin-top", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING, "scroll-padding", false, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_BLOCK_END, "scroll-padding-block-end", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_BLOCK_START, "scroll-padding-block-start", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_BOTTOM, "scroll-padding-bottom", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_INLINE_END, "scroll-padding-inline-end", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_INLINE_START, "scroll-padding-inline-start", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_LEFT, "scroll-padding-left", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_RIGHT, "scroll-padding-right", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_PADDING_TOP, "scroll-padding-top", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_ALIGN, "scroll-snap-align", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_STOP, "scroll-snap-stop", true, CSP_SPARSE_SCROLL) \
    X(SCROLL_SNAP_TYPE, "scroll-snap-type", true, CSP_SPARSE_SCROLL) \
    X(SCROLLBAR_COLOR, "scrollbar-color", true, CSP_SPARSE_SCROLL) \
    X(SCROLLBAR_GUTTER, "scrollbar-gutter", true, 0) \
    X(SCROLLBAR_WIDTH, "scrollbar-width", true, CSP_SPARSE_SCROLL) \
    X(TAB_SIZE, "tab-size", true, CSP_SPARSE_MODERN) \
    X(TABLE_LAYOUT, "table-layout", true, 0) \
    X(TEXT_ALIGN, "text-align", true, CSP_STYLE_ONLY) \
    X(TEXT_DECORATION_LINE, "text-decoration-line", true, CSP_STYLE_ONLY) \
    X(TEXT_INDENT, "text-indent", true, CSP_USED_GEOMETRY) \
    X(TEXT_OVERFLOW, "text-overflow", true, 0) \
    X(TEXT_RENDERING, "text-rendering", true, CSP_SPARSE_MODERN) \
    X(TEXT_SHADOW, "text-shadow", true, 0) \
    X(TEXT_SIZE_ADJUST, "text-size-adjust", true, CSP_SPARSE_MODERN) \
    X(TEXT_TRANSFORM, "text-transform", true, CSP_STYLE_ONLY) \
    X(TEXT_WRAP, "text-wrap", false, CSP_SPARSE_MODERN) \
    X(TEXT_WRAP_STYLE, "text-wrap-style", true, CSP_SPARSE_MODERN) \
    X(TOP, "top", true, 0) \
    X(TOUCH_ACTION, "touch-action", true, CSP_SPARSE_MODERN) \
    X(TRANSFORM, "transform", true, CSP_STYLE_ONLY) \
    X(TRANSFORM_ORIGIN, "transform-origin", true, CSP_USED_GEOMETRY) \
    X(TRANSFORM_STYLE, "transform-style", true, CSP_SPARSE_MODERN) \
    X(TRANSITION, "transition", false, 0) \
    X(TRANSITION_DELAY, "transition-delay", true, 0) \
    X(TRANSITION_DURATION, "transition-duration", true, 0) \
    X(TRANSITION_PROPERTY, "transition-property", true, 0) \
    X(TRANSITION_TIMING_FUNCTION, "transition-timing-function", true, 0) \
    X(TRANSLATE, "translate", true, CSP_SPARSE_MODERN) \
    X(UNICODE_BIDI, "unicode-bidi", true, 0) \
    X(USER_SELECT, "user-select", true, CSP_SPARSE_MODERN) \
    X(VERTICAL_ALIGN, "vertical-align", true, CSP_STYLE_ONLY) \
    X(VISIBILITY, "visibility", true, CSP_STYLE_ONLY) \
    X(WHITE_SPACE, "white-space", true, CSP_STYLE_ONLY) \
    X(WIDTH, "width", true, CSP_USED_GEOMETRY) \
    X(WILL_CHANGE, "will-change", true, 0) \
    X(WORD_SPACING, "word-spacing", true, CSP_STYLE_ONLY) \
    X(WRITING_MODE, "writing-mode", true, 0) \
    X(Z_INDEX, "z-index", true, CSP_STYLE_ONLY)

typedef enum {
#define X(id, name, longhand, flags) CSP_##id,
    COMPUTED_STYLE_PROPERTIES(X)
#undef X
    CSP_COUNT,
    /* Not a registered property: a custom property, an SVG presentation
       attribute, or a name nothing supports. */
    CSP_NONE = CSP_COUNT
} ComputedStylePropertyId;

typedef struct {
    const char *name;
    bool longhand;
    uint8_t flags;
} ComputedStyleProperty;

static const ComputedStyleProperty computed_style_properties[] = {
#define X(id, name, longhand, flags) { name, longhand, flags },
    COMPUTED_STYLE_PROPERTIES(X)
#undef X
};

#define COMPUTED_STYLE_PROPERTY_COUNT \
    (sizeof(computed_style_properties) / sizeof(computed_style_properties[0]))

static ComputedStylePropertyId computed_style_property_find(
    const char *name)
{
    size_t low = 0, high = COMPUTED_STYLE_PROPERTY_COUNT;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        int order = strcmp(name, computed_style_properties[middle].name);
        if (order == 0) return (ComputedStylePropertyId) middle;
        if (order < 0) high = middle;
        else low = middle + 1u;
    }
    return CSP_NONE;
}

static bool computed_style_transition_id(ComputedStylePropertyId id)
{
    return id == CSP_TRANSITION || id == CSP_TRANSITION_DELAY
        || id == CSP_TRANSITION_DURATION || id == CSP_TRANSITION_PROPERTY
        || id == CSP_TRANSITION_TIMING_FUNCTION;
}

/* Computed times serialize in seconds, as browsers do: 300ms is 0.3s. */
static size_t computed_transition_time(double ms, char *output, size_t size)
{
    int written = snprintf(output, size, "%gs", ms / 1000.0);
    return written < 0 ? 0 : (size_t) written;
}

/* Appends `text` to the transition value being built, counting bytes past
   `size` so a first pass can measure the whole value. */
static void computed_transition_append(char *output, size_t size,
                                       size_t *used, const char *text)
{
    size_t length = strlen(text);
    if (*used < size) {
        size_t copied = length < size - *used ? length : size - *used - 1u;
        memcpy(output + *used, text, copied);
        output[*used + copied] = '\0';
    }
    *used += length;
}

/* Serializes transition longhand or shorthand `id` into `output` (which
   may be NULL when `size` is 0) and returns its length, as snprintf does:
   a value with long linear() easings is measured, then built in full. */
static size_t computed_transition_value(ComputedStylePropertyId id,
                                        const StyleTransitionComputed *t,
                                        char *output, size_t size)
{
    size_t used = 0;
    if (size != 0) output[0] = '\0';
    unsigned count = id == CSP_TRANSITION_DELAY ? t->delay_count
        : id == CSP_TRANSITION_DURATION ? t->duration_count
        : id == CSP_TRANSITION_TIMING_FUNCTION ? t->timing_count
        : t->property_count;
    for (unsigned i = 0; i < count; i++) {
        char time[40];
        if (i != 0) computed_transition_append(output, size, &used, ", ");
        if (id == CSP_TRANSITION_DELAY) {
            computed_transition_time(t->delay_ms[i], time, sizeof(time));
            computed_transition_append(output, size, &used, time);
        } else if (id == CSP_TRANSITION_DURATION) {
            computed_transition_time(t->duration_ms[i], time, sizeof(time));
            computed_transition_append(output, size, &used, time);
        } else if (id == CSP_TRANSITION_TIMING_FUNCTION) {
            computed_transition_append(output, size, &used,
                                       style_transition_timing(t, i));
        } else if (id == CSP_TRANSITION_PROPERTY) {
            computed_transition_append(output, size, &used,
                                       t->properties[i]);
        } else {
            /* The shorthand pairs each property with the other lists,
               which repeat to its length. */
            computed_transition_append(output, size, &used,
                                       t->properties[i]);
            computed_transition_append(output, size, &used, " ");
            computed_transition_time(
                t->duration_ms[i % t->duration_count], time, sizeof(time));
            computed_transition_append(output, size, &used, time);
            computed_transition_append(output, size, &used, " ");
            computed_transition_append(output, size, &used,
                style_transition_timing(t, i % t->timing_count));
            computed_transition_append(output, size, &used, " ");
            computed_transition_time(
                t->delay_ms[i % t->delay_count], time, sizeof(time));
            computed_transition_append(output, size, &used, time);
        }
    }
    return used;
}

/* __tilefinchTransitionSnapshot(handle, names) ->
   [properties, durations_ms, delays_ms, values, rendered]: transitions whose delay
   plus duration is positive (empty when there are none) and the computed
   values of names, resolved without a synchronous layout. The watcher needs
   those values while no transition applies too: a class can bring the
   transition and the new value together. */
/* Whether `at` resolves display:none (*hidden). A style the cache already
   validated in this epoch is read in place; a miss resolves and caches the
   chain above it. False when the cascade was interrupted. */
static bool bridge_transition_display_none(DomBridge *bridge, bool style_cache,
                                           lxb_dom_node_t *at, bool *hidden)
{
    const struct ComputedStyleCacheEntry *hit = style_cache
        ? computed_style_cache_find(bridge, at) : NULL;
    if (hit != NULL && hit->stamp == bridge->computed_style_cache.epoch) {
        *hidden = hit->style.display == DISPLAY_NONE;
        return true;
    }
    ComputedStyle style;
    if (!bridge_computed_style(bridge, at, &style, NULL, NULL)) return false;
    *hidden = style.display == DISPLAY_NONE;
    return true;
}

JSValue js_transition_snapshot(JSContext *context,
                               JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || bridge->stylesheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return JS_NULL;
    /* The snapshot reads properties, durations and delays, never the
       easings, so they are not validated again or serialized. */
    StyleTransitionComputed transition;
    (void) style_transition_computed_untimed(bridge->stylesheet, node,
                                             PSEUDO_NONE, &transition);
    bool animated = false;
    for (unsigned i = 0; i < transition.property_count && !animated; i++) {
        double duration = transition.duration_ms[i % transition.duration_count];
        double delay = transition.delay_ms[i % transition.delay_count];
        animated = (duration > 0.0 ? duration : 0.0) + delay > 0.0
            && strcmp(transition.properties[i], "none") != 0;
    }
    /* Descendant discovery needs only the cascade's answer, not four
       temporary arrays or computed paint values for every visited node. */
    if (argc > 2 && JS_ToBool(context, argv[2]))
        return JS_NewBool(context, animated);
    JSValue result = JS_NewArray(context);
    JSValue properties = JS_NewArray(context);
    JSValue durations = JS_NewArray(context);
    JSValue delays = JS_NewArray(context);
    JSValue values = JS_NewArray(context);
    bool ok = !JS_IsException(result) && !JS_IsException(properties)
        && !JS_IsException(durations) && !JS_IsException(delays)
        && !JS_IsException(values);
    for (unsigned i = 0; ok && animated && i < transition.property_count;
         i++) {
        ok = JS_SetPropertyUint32(
                 context, properties, i,
                 JS_NewString(context, transition.properties[i])) >= 0
            && JS_SetPropertyUint32(
                 context, durations, i,
                 JS_NewFloat64(context, transition.duration_ms[
                     i % transition.duration_count])) >= 0
            && JS_SetPropertyUint32(
                 context, delays, i,
                 JS_NewFloat64(context, transition.delay_ms[
                     i % transition.delay_count])) >= 0;
    }
    uint32_t name_count = 0;
    if (ok && argc > 1 && JS_IsArray(context, argv[1])) {
        JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
        ok = !JS_IsException(length_value)
            && JS_ToUint32(context, &name_count, length_value) >= 0;
        JS_FreeValue(context, length_value);
    }
    if (name_count > 32) name_count = 32;
    /* Applies pending drops, so a stamp of this epoch is current. */
    const bool style_cache = ok && bridge_computed_style_cache(bridge) != NULL;
    bool hidden = false;
    bool interrupted = ok
        && !bridge_transition_display_none(bridge, style_cache, node, &hidden);
    bool rendered = !hidden;
    __typeof__(bridge->transition_ancestors_memo) *memo =
        &bridge->transition_ancestors_memo;
    uint64_t content_generation = bridge->document == NULL
        ? 0 : bridge->document->content_generation;
    if (ok && !interrupted && rendered && node->parent != NULL) {
        if (memo->parent == node->parent
            && memo->content_generation == content_generation
            && computed_style_inputs_hold(bridge, &memo->inputs)) {
            rendered = memo->rendered;
        } else {
            unsigned ancestors = 1;
            lxb_dom_node_t *at = node->parent;
            for (; at != NULL && ancestors < DOM_ROOT_NODE_ANCESTOR_LIMIT;
                 at = at->parent, ancestors++) {
                if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                if (!bridge_transition_display_none(bridge, style_cache, at,
                                                    &hidden)) {
                    interrupted = true;
                    break;
                }
                if (hidden) { rendered = false; break; }
            }
            if (at != NULL && ancestors == DOM_ROOT_NODE_ANCESTOR_LIMIT)
                rendered = false;
            memo->parent = NULL;
            if (!interrupted && style_cache) {
                memo->parent = node->parent;
                computed_style_inputs_capture(bridge, &memo->inputs);
                memo->content_generation = content_generation;
                memo->rendered = rendered;
            }
        }
    }
    if (interrupted) {
        JS_FreeValue(context, result);
        JS_FreeValue(context, properties);
        JS_FreeValue(context, durations);
        JS_FreeValue(context, delays);
        JS_FreeValue(context, values);
        return js_rt_throw_task_interruption(
            context, "transition style interrupted");
    }
    bool saved = bridge->computed_style_without_layout;
    bridge->computed_style_without_layout = true;
    for (uint32_t i = 0; ok && i < name_count; i++) {
        JSValue name = JS_GetPropertyUint32(context, argv[1], i);
        if (JS_IsException(name)) { ok = false; break; }
        JSValueConst arguments[2] = { argv[0], name };
        JSValue value = js_computed_style_get(context, JS_UNDEFINED, 2,
                                              arguments);
        JS_FreeValue(context, name);
        if (JS_IsException(value)) { ok = false; break; }
        ok = JS_SetPropertyUint32(context, values, i, value) >= 0;
    }
    bridge->computed_style_without_layout = saved;
    /* JS_SetPropertyUint32 consumes its value whether or not the insertion
       succeeds, so a list is released here only if it was never offered. */
    JSValue *lists[4] = { &properties, &durations, &delays, &values };
    for (uint32_t i = 0; i < 4; i++) {
        JSValue list = *lists[i];
        *lists[i] = JS_UNDEFINED;
        if (ok) ok = JS_SetPropertyUint32(context, result, i, list) >= 0;
        else JS_FreeValue(context, list);
    }
    if (ok) ok = JS_SetPropertyUint32(
        context, result, 4, JS_NewBool(context, rendered)) >= 0;
    if (!ok) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
    }
    return result;
}

/* __tilefinchAttributeChangeMayAffectHas(name, oldValue, newValue) -> whether
   that attribute change can alter a :has() match anywhere, by the classifier
   the attribute setter uses for style invalidation (null: absent). Without
   values (a CSSOM style write) any change of the attribute is assumed. The
   transition watcher then rescans every element rather than the changed
   element's parent subtree. */
JSValue js_attribute_change_may_affect_has(JSContext *context,
                                           JSValueConst this_value,
                                           int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (argc < 1) return JS_TRUE;
    const char *text[3] = { NULL, NULL, NULL };
    size_t length[3] = { 0, 0, 0 };
    bool ok = true;
    for (int i = 0; i < 3 && i < argc && ok; i++) {
        if (i > 0 && (JS_IsNull(argv[i]) || JS_IsUndefined(argv[i])))
            continue;
        text[i] = JS_ToCStringLen(context, &length[i], argv[i]);
        ok = text[i] != NULL;
    }
    bool known = argc >= 3, sensitive = true;
    /* Class and id tokens need the values; without them stay conservative. */
    bool tokenized = text[0] != NULL
        && ((length[0] == 5 && strncasecmp(text[0], "class", 5) == 0)
            || (length[0] == 2 && strncasecmp(text[0], "id", 2) == 0));
    if (ok && (known || !tokenized)) {
        sensitive = stylesheet_attribute_change_may_affect_has(
            bridge == NULL ? NULL : bridge->stylesheet, text[0], length[0],
            known ? text[1] : NULL, known ? length[1] : 0,
            /* Absent to a value no selector names: any change. */
            known ? text[2] : "\x01", known ? length[2] : 1);
    }
    for (int i = 0; i < 3; i++) JS_FreeCString(context, text[i]);
    return ok ? JS_NewBool(context, sensitive) : JS_EXCEPTION;
}

/* __tilefinchStyleReach(handle, scopes) -> whether the element is one of the
   scope elements (handles, at most 16) or inside one, walking the native
   tree, where a shadow root is a child of its host. An element not under
   the document, or past the depth bound, answers true: the caller treats
   true as "may have changed". */
JSValue js_style_reach(JSContext *context, JSValueConst this_value,
                       int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (node == NULL) return JS_TRUE;
    lxb_dom_node_t *scopes[16];
    uint32_t scope_count = 0;
    if (argc > 1 && JS_IsArray(context, argv[1])) {
        JSValue length_value = JS_GetPropertyStr(context, argv[1], "length");
        uint32_t length = 0;
        bool ok = !JS_IsException(length_value)
            && JS_ToUint32(context, &length, length_value) >= 0;
        JS_FreeValue(context, length_value);
        if (!ok) return JS_EXCEPTION;
        if (length > 16) return JS_TRUE;
        for (uint32_t i = 0; i < length; i++) {
            JSValue value = JS_GetPropertyUint32(context, argv[1], i);
            if (JS_IsException(value)) return JS_EXCEPTION;
            lxb_dom_node_t *scope = js_rt_bridge_node_arg(context, bridge,
                                                          value);
            JS_FreeValue(context, value);
            if (scope == NULL) return JS_TRUE;
            scopes[scope_count++] = scope;
        }
    }
    unsigned depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL && depth < 4096;
         at = at->parent, depth++) {
        for (uint32_t i = 0; i < scope_count; i++)
            if (at == scopes[i]) return JS_TRUE;
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return JS_FALSE;
    }
    return JS_TRUE;
}

/* __tilefinchTimingFunction(text) -> the <easing-function> serialized as a
   computed value, or null when it is invalid. CSS animations (motion.js)
   and Element.animate() validate through it, so they accept exactly what
   transition-timing-function accepts. */
JSValue js_timing_function(JSContext *context, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc < 1) return JS_NULL;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[0]);
    if (text == NULL) return JS_EXCEPTION;
    char computed[STYLE_TIMING_FUNCTION_CAPACITY];
    bool valid = style_timing_function(text, length, computed,
                                       sizeof(computed));
    JS_FreeCString(context, text);
    return valid ? JS_NewString(context, computed) : JS_NULL;
}

/* __tilefinchComputedStyleSupport(name) -> whether the declaration has that
   property (a dashed, lower-case name; custom properties always qualify).
   __tilefinchComputedStyleSupport()     -> a fresh array of the enumerable
   longhands, in order. */
JSValue js_computed_style_support(JSContext *context,
                                  JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) this_value;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char *name = JS_ToCString(context, argv[0]);
        if (name == NULL) return JS_EXCEPTION;
        bool supported = (name[0] == '-' && name[1] == '-')
            || computed_style_property_find(name) != CSP_NONE;
        JS_FreeCString(context, name);
        return JS_NewBool(context, supported);
    }
    JSValue names = JS_NewArray(context);
    if (JS_IsException(names)) return names;
    uint32_t at = 0;
    for (size_t i = 0; i < COMPUTED_STYLE_PROPERTY_COUNT; i++) {
        if (!computed_style_properties[i].longhand) continue;
        if (JS_SetPropertyUint32(
                context, names, at++,
                JS_NewString(context, computed_style_properties[i].name))
            < 0) {
            JS_FreeValue(context, names);
            return JS_EXCEPTION;
        }
    }
    return names;
}

/* A padding edge is a packed StyleLength: a plain pixel count, or a tagged
   percentage/math value that means nothing until it is resolved. */
static bool computed_style_edge_is_fixed(StyleLength edge)
{
    return edge >= -STYLE_LENGTH_DIRECT_LIMIT
        && edge <= STYLE_LENGTH_DIRECT_LIMIT;
}

/* Sparse scroll lengths still need computed-value serialization: callers
   commonly parse these values themselves rather than use scrollIntoView.
   Reuse the bounded math parser without interning a program or allocating.
   Percentages (padding only) and keywords retain their authored form. */
static TILEFINCH_OUT_OF_LINE void computed_style_scroll_length(
    const Stylesheet *sheet, const ComputedStyle *style,
    char *value, size_t capacity)
{
    StyleMathParser parser;
    StyleMathCandidate candidate;
    int root = -1;
    int em_fixed = computed_style_font_size_fixed(style);
    int rem_fixed = computed_style_root_font_size_fixed(style);
    /* This parser takes integral font bases. Keep the prior text rather
       than silently round a fractional relative-unit calculation. */
    if (((em_fixed | rem_fixed) & 63) != 0
        && (strstr(value, "em") != NULL || strstr(value, "ch") != NULL))
        return;
    int em = em_fixed / 64;
    int rem = rem_fixed / 64;
    if (!style_math_candidate(sheet, value, strlen(value), &parser,
            &candidate, &root, em, rem, (em + 1) / 2)
        || candidate.has_percent || !parser.nodes[root].folded) return;
    snprintf(value, capacity, "%.15gpx", parser.nodes[root].a / 256.0);
}

/* Used pixels of one edge, sharing the width resolved by the cascade walk. */
static int computed_style_used_padding(
    DomBridge *bridge, int containing_width,
    StyleLength edge)
{
    if (computed_style_edge_is_fixed(edge)) return edge < 0 ? 0 : edge;
    int used = 0;
    if (!style_length_resolve(
            bridge->stylesheet, edge,
            containing_width, &used)
        || used < 0) used = 0;
    return used;
}

static bool computed_style_replaced_inline(const lxb_dom_node_t *node)
{
    return node->local_name == LXB_TAG_IMG
        || node->local_name == LXB_TAG_CANVAS
        || node->local_name == LXB_TAG_VIDEO
        || node->local_name == LXB_TAG_SVG;
}

/* The resolved value of width or height is the used value, in pixels, only
   for an element that generates a box the property applies to. False means
   the caller reports the computed value instead: a pseudo-element (its host's
   box is not its box), `display: none` or `contents`, a non-replaced inline
   (width and height do not apply to it, whatever was authored), or an element
   with no layout box yet. */
static TILEFINCH_OUT_OF_LINE bool computed_style_used_box_size(
    DomBridge *bridge, const lxb_dom_node_t *node,
    const ComputedStyle *style, int containing_width,
    PseudoElement pseudo, bool vertical, char *value, size_t value_size)
{
    if (pseudo != PSEUDO_NONE
        || style->display == DISPLAY_NONE
        || style->display == DISPLAY_CONTENTS
        || (style->display == DISPLAY_INLINE
            && !computed_style_replaced_inline(node))) return false;
    const LayoutNodeBox *box = layout_box_for_node(js_rt_bridge_layout(bridge), node);
    if (box == NULL) return false;
    int used = vertical ? box->height : box->width;
    if (!style->box_sizing_border_box) {
        used -= vertical
            ? style->border.top + style->border.bottom
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.top)
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.bottom)
            : style->border.left + style->border.right
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.left)
              + computed_style_used_padding(
                    bridge, containing_width, style->padding.right);
    }
    if (used < 0) used = 0;
    snprintf(value, value_size, "%dpx", used);
    return true;
}

/* The value of a sparse modern property with no authored declaration on the
   element. Inherited typography state is retained in the style; everything
   else reports its initial keyword. */
static TILEFINCH_OUT_OF_LINE const char *computed_style_sparse_modern_initial(
    const ComputedStyle *style, ComputedStylePropertyId id)
{
    static char tab_size[8];
    if (id == CSP_TAB_SIZE) {
        snprintf(tab_size, sizeof(tab_size), "%u",
                 computed_style_tab_size(style));
        return tab_size;
    }
    if (id == CSP_HYPHENS)
        return computed_style_hyphens_none(style) ? "none" : "manual";
    if (id == CSP_FONT_KERNING)
        return computed_style_kerning_none(style) ? "none" : "auto";
    if (id == CSP_TEXT_RENDERING)
        return "auto";
    if (id == CSP_MIX_BLEND_MODE
        || id == CSP_COLOR_SCHEME)
        return "normal";
    if (id == CSP_BACKFACE_VISIBILITY)
        return "visible";
    if (id == CSP_TRANSFORM_STYLE)
        return "flat";
    if (id == CSP_BORDER_IMAGE_SLICE)
        return "100%";
    if (id == CSP_BORDER_IMAGE_WIDTH)
        return "1";
    if (id == CSP_BORDER_IMAGE_OUTSET)
        return "0";
    if (id == CSP_BORDER_IMAGE_REPEAT)
        return "stretch";
    if (id == CSP_BORDER_IMAGE)
        return "none 100% / 1 / 0 stretch";
    /* translate, rotate, scale, backdrop-filter, border-image-source. */
    return "none";
}

/* Resolved values for fields the cascade already retains but CSSOM never
   serialized. Each case decodes the field's stored representation; none of
   them reads authored text, so `inherit`, `var()`, relative units and
   !important are already settled by the time a value gets here. Returns
   false for a property this function does not own. */
static TILEFINCH_OUT_OF_LINE bool computed_style_serialize_retained(
    const lxb_dom_node_t *node, const ComputedStyle *style,
    ComputedStylePropertyId id, char *value, size_t value_size)
{
    if (id == CSP_LINE_HEIGHT) {
        /* `normal` stays a keyword even though layout later picks a number
           from the face; a length, percentage or multiplier has already been
           resolved against the element's own font size, in device pixels. */
        if (style->line_height == STYLE_LINE_HEIGHT_ZERO) {
            snprintf(value, value_size, "0px");
        } else if (style->line_height <= 0) {
            snprintf(value, value_size, "normal");
        } else {
            snprintf(value, value_size, "%dpx", style->line_height);
        }
    } else if (id == CSP_TEXT_ALIGN) {
        static const char *const names[] = {
            "start", "center", "end", "left", "right"
        };
        size_t at = style->text_align;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0]) ? names[at] : "start");
    } else if (id == CSP_VERTICAL_ALIGN) {
        static const char *const names[] = {
            "baseline", "super", "sub", "middle", "top", "bottom",
            "text-top", "text-bottom"
        };
        size_t at = style->vertical_align;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0])
                     ? names[at] : "baseline");
    } else if (id == CSP_DIRECTION) {
        snprintf(value, value_size, "%s",
                 computed_style_direction_rtl(style) ? "rtl" : "ltr");
    } else if (id == CSP_WRITING_MODE) {
        static const char *const names[] = {
            "horizontal-tb", "vertical-rl", "vertical-lr"
        };
        unsigned at = computed_style_writing_mode(style);
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0])
                     ? names[at] : "horizontal-tb");
    } else if (id == CSP_UNICODE_BIDI) {
        static const char *const names[] = {
            "normal", "embed", "isolate", "bidi-override",
            "isolate-override", "plaintext"
        };
        size_t at = style->unicode_bidi;
        snprintf(value, value_size, "%s",
                 at < sizeof(names) / sizeof(names[0]) ? names[at] : "normal");
    } else if (id == CSP_LETTER_SPACING) {
        if (style->letter_spacing_fixed == 0) snprintf(value, value_size, "normal");
        else snprintf(value, value_size, "%.6gpx", style->letter_spacing_fixed / 64.0);
    } else if (id == CSP_WORD_SPACING) {
        snprintf(value, value_size, "%dpx", style->word_spacing);
    } else if (id == CSP_TEXT_DECORATION_LINE) {
        /* Not inherited: only a line this element itself declares. The
           engine paints underlines alone, so that is the only line kept. */
        snprintf(value, value_size, "%s",
                 computed_style_has_text_underline(style)
                     ? "underline" : "none");
    } else if (id == CSP_FLEX_GROW) {
        computed_style_format_scaled(
            value, value_size,
            style->flex_grow > 0 ? (unsigned long) style->flex_grow : 0ul,
            1000ul);
    } else if (id == CSP_FLEX_SHRINK) {
        computed_style_format_scaled(
            value, value_size, (unsigned long) style->flex_shrink, 512ul);
    } else if (id == CSP_FLEX_BASIS) {
        if (!style->has_flex_basis) {
            snprintf(value, value_size, "auto");
        } else if (style->flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, value_size, "min-content");
        } else if (style->flex_basis == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(value, value_size, "max-content");
        } else if (style->flex_basis == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(value, value_size, "fit-content");
        } else if (style->flex_basis_percent
                   && style->flex_basis_offset != 0) {
            snprintf(value, value_size, "calc(%d%% %c %dpx)",
                     style->flex_basis,
                     style->flex_basis_offset < 0 ? '-' : '+',
                     style->flex_basis_offset < 0
                         ? -style->flex_basis_offset
                         : style->flex_basis_offset);
        } else {
            snprintf(value, value_size, "%d%s", style->flex_basis,
                     style->flex_basis_percent ? "%" : "px");
        }
    } else if (id == CSP_LIST_STYLE_TYPE) {
        static const char *const names[] = {
            NULL, "none", "disc", "circle", "square", "decimal",
            "decimal-leading-zero", "lower-alpha", "upper-alpha",
            "lower-roman", "upper-roman"
        };
        size_t at = style->list_style_none ? 1u : style->list_style_type;
        const char *keyword =
            at < sizeof(names) / sizeof(names[0]) ? names[at] : NULL;
        if (keyword == NULL) {
            /* Nothing authored: the user-agent default follows the nearest
               list container, exactly as marker generation decides it. */
            keyword = "disc";
            for (const lxb_dom_node_t *at_node = node; at_node != NULL;
                 at_node = at_node->parent) {
                if (at_node->type != LXB_DOM_NODE_TYPE_ELEMENT
                    || at_node->ns != LXB_NS_HTML) continue;
                if (at_node->local_name == LXB_TAG_OL) keyword = "decimal";
                if (at_node->local_name == LXB_TAG_OL
                    || at_node->local_name == LXB_TAG_UL) break;
            }
        }
        snprintf(value, value_size, "%s", keyword);
    } else if (id == CSP_LIST_STYLE_POSITION) {
        snprintf(value, value_size, "%s",
                 style->list_style_inside ? "inside" : "outside");
    } else if (id == CSP_TABLE_LAYOUT) {
        snprintf(value, value_size, "%s",
                 style->table_layout_fixed ? "fixed" : "auto");
    } else if (id == CSP_ASPECT_RATIO) {
        /* The older case owns an authored ratio; this is the initial value. */
        snprintf(value, value_size, "auto");
    } else {
        return false;
    }
    return true;
}

/* A serialized value as a JS string, noting whether it is empty. */
static JSValue computed_style_string(JSContext *context, const char *text,
                                     bool *empty)
{
    *empty = text[0] == '\0';
    return JS_NewString(context, text);
}

static ComputedStylePropertyId computed_style_scroll_physical_id(
    ComputedStylePropertyId id, const ComputedStyle *style)
{
    bool padding = false, block = false, start = false;
    switch (id) {
    case CSP_SCROLL_PADDING_BLOCK_START: padding = true; /* fall through */
    case CSP_SCROLL_MARGIN_BLOCK_START: block = true; start = true; break;
    case CSP_SCROLL_PADDING_BLOCK_END: padding = true; /* fall through */
    case CSP_SCROLL_MARGIN_BLOCK_END: block = true; break;
    case CSP_SCROLL_PADDING_INLINE_START: padding = true; /* fall through */
    case CSP_SCROLL_MARGIN_INLINE_START: start = true; break;
    case CSP_SCROLL_PADDING_INLINE_END: padding = true; /* fall through */
    case CSP_SCROLL_MARGIN_INLINE_END: break;
    default: return id;
    }
    unsigned block_side, inline_side;
    computed_style_scroll_start_sides(style, &block_side, &inline_side);
    unsigned side = (block ? block_side : inline_side) ^ (start ? 0u : 2u);
    static const ComputedStylePropertyId physical[2][4] = {
        {CSP_SCROLL_MARGIN_TOP, CSP_SCROLL_MARGIN_RIGHT,
         CSP_SCROLL_MARGIN_BOTTOM, CSP_SCROLL_MARGIN_LEFT},
        {CSP_SCROLL_PADDING_TOP, CSP_SCROLL_PADDING_RIGHT,
         CSP_SCROLL_PADDING_BOTTOM, CSP_SCROLL_PADDING_LEFT}
    };
    return physical[padding ? 1u : 0u][side];
}

/* What a read resolved: the element (or pseudo-element) style and the
   geometry inputs the serializer needs. `id` and `name` are the physical
   property once a logical scroll-margin/padding name has been mapped. */
typedef struct {
    DomBridge *bridge;
    lxb_dom_node_t *node;
    ComputedStyle style;
    PseudoElement pseudo;
    ComputedStylePropertyId id;
    const char *name;
    size_t name_length;
    unsigned property_flags;
    int containing_width;
} ComputedStyleRead;

/* Hands `value` back as the read's whole answer and ends resolution. */
static bool computed_style_answer(JSValue *answer, JSValue value)
{
    *answer = value;
    return false;
}

/* Resolves `name` (dashed, lower-case) on `node` for one read: the layout
   flush the property needs, then the cascade (memo, ancestor ring) and the
   pseudo-element argv[2] names. Returns true with `read` filled, or false
   with the read's whole answer in `answer` (custom properties, transition
   longhands, names nothing resolves, and exceptions). */
static bool computed_style_resolve(JSContext *context, DomBridge *bridge,
                                   lxb_dom_node_t *node, const char *name,
                                   size_t name_length, int argc,
                                   JSValueConst *argv,
                                   ComputedStyleRead *read,
                                   JSValue *answer, bool *empty)
{
    /* The single lookup every later decision dispatches on. Names arrive
       dashed and lower-case (__tilefinchCssName). */
    ComputedStylePropertyId id = computed_style_property_find(name);
    const bool custom_property = name_length >= 3 && name[0] == '-'
        && name[1] == '-';
    /* A name nothing resolves (not registered, not a custom property, not
       an SVG presentation attribute this element takes) serializes as ""
       whatever the cascade says: answer before any layout or cascade. */
    if (id == CSP_NONE && !custom_property
        && !svg_presentation_attribute_relevant(node, name, name_length)) {
        return computed_style_answer(answer, computed_style_string(context, "", empty));
    }
    const unsigned property_flags =
        id == CSP_NONE ? 0u : computed_style_properties[id].flags;
    bool used_geometry_property = (property_flags & CSP_USED_GEOMETRY) != 0;
    /* Hydration often toggles a class and immediately asks whether a node
       is visible. These values resolve from the current DOM/cascade without
       rebuilding geometry. A geometry-only callback cannot install pending
       stylesheets, so such reads need no scratch flow even during resource
       changes. Other host callbacks can refresh the cascade and retain the
       conservative path, as do container-dependent rules.
       Container-relative units may exist only in style attributes; the
       last layout's collected container state reveals those too. */
    bool independent_style_property = (property_flags & CSP_STYLE_ONLY) != 0;
    bool geometry_independent = !stylesheet_has_container_queries(bridge->stylesheet)
        && !style_container_layout_state_present(bridge->stylesheet);
    bool style_only = independent_style_property
        && (bridge->synchronous_layout_geometry_only
            || (bridge->mutations.count != 0
                && !bridge->mutations.overflowed
                && !bridge->mutations.resource_rebuild_required
                && !bridge->mutations.conservative_resource_scan))
        && geometry_independent;
    /* Transitions and the watcher's snapshots need the cascade, not
       geometry: only a stylesheet change waiting to be applied forces the
       flush. */
    bool cascade_only = computed_style_transition_id(id)
        || bridge->computed_style_without_layout;
    bool sheet_stale = bridge->relayout_dirty != NULL
        && *bridge->relayout_dirty
        && (bridge->mutations.overflowed
            || bridge->mutations.resource_rebuild_required
            || bridge->mutations.conservative_resource_scan);
    /* Overflow can request a later whole-page resource audit, but a
       geometry-only callback cannot install those sources. Rebuilding the
       same scratch flow for every transition property cannot freshen the
       cascade. Container-dependent rules still need their geometry. */
    bool cascade_refresh = sheet_stale
        && (!bridge->synchronous_layout_geometry_only || !geometry_independent);
    /* A value from the cascade alone never consults the layout, so a
       truncated one has nothing to hide from it: the style-only
       properties (but transform, whose percentage translation reads the
       box) and custom properties, without container-dependent rules. */
    bool layout_free = geometry_independent
        && (custom_property
            || (independent_style_property && id != CSP_TRANSFORM));
    if (cascade_only ? cascade_refresh
        : ((bridge->relayout_dirty != NULL && *bridge->relayout_dirty
            && !style_only)
           || (bridge->layout == NULL && used_geometry_property)
           /* Script never reads a truncated layout: a read that consumes
              layout completes it first (js_rt_bridge_layout). */
           || (js_rt_bridge_layout_truncated(bridge) && !layout_free))) {
#ifndef TILEFINCH_NO_TRACE
        if (tilefinch_trace_runtime_steps())
            fprintf(stderr, "computed-style-layout property=%s independent=%d queries=%d containers=%d\n",
                    name, (int) independent_style_property,
                    (int) stylesheet_has_container_queries(bridge->stylesheet),
                    (int) style_container_layout_state_present(bridge->stylesheet));
#endif
        if (js_rt_bridge_flush_synchronous_layout(bridge, node)) {
            if (!js_rt_notify_style_flushed(context)) return computed_style_answer(answer, JS_EXCEPTION);
            node = js_rt_bridge_node_arg(context, bridge, argv[0]);
            if (node == NULL || bridge->stylesheet == NULL)
                return computed_style_answer(answer, computed_style_string(context, "", empty));
        }
    }
    if (custom_property) {
        /* A custom property's value comes from the custom-property rules
           of the element and its ancestors (style_custom_property_value),
           not from the element's computed style: no cascade. */
        PseudoElement custom_pseudo = PSEUDO_NONE;
        if (argc > 2) {
            size_t pseudo_length = 0;
            const char *pseudo_name =
                JS_ToCStringLen(context, &pseudo_length, argv[2]);
            if (pseudo_name == NULL) {
                return computed_style_answer(answer, JS_EXCEPTION);
            }
            if ((pseudo_length == 8 && memcmp(pseudo_name, "::before", 8) == 0)
                || (pseudo_length == 7
                    && memcmp(pseudo_name, ":before", 7) == 0))
                custom_pseudo = PSEUDO_BEFORE;
            else if ((pseudo_length == 7
                      && memcmp(pseudo_name, "::after", 7) == 0)
                     || (pseudo_length == 6
                         && memcmp(pseudo_name, ":after", 6) == 0))
                custom_pseudo = PSEUDO_AFTER;
            JS_FreeCString(context, pseudo_name);
        }
        style_container_units_for_node((Stylesheet *) bridge->stylesheet,
                                       node);
        char custom_value[640] = "";
        (void) style_custom_property_value(
            bridge->stylesheet, node, custom_pseudo, name, name_length,
            custom_value, sizeof(custom_value));
        return computed_style_answer(answer, computed_style_string(context, custom_value, empty));
    }
    if (computed_style_transition_id(id)) {
        PseudoElement transition_pseudo = PSEUDO_NONE;
        if (argc > 2) {
            const char *pseudo_name = JS_ToCString(context, argv[2]);
            if (pseudo_name == NULL) {
                return computed_style_answer(answer, JS_EXCEPTION);
            }
            if (strcmp(pseudo_name, "::before") == 0
                || strcmp(pseudo_name, ":before") == 0)
                transition_pseudo = PSEUDO_BEFORE;
            else if (strcmp(pseudo_name, "::after") == 0
                     || strcmp(pseudo_name, ":after") == 0)
                transition_pseudo = PSEUDO_AFTER;
            JS_FreeCString(context, pseudo_name);
        }
        StyleTransitionComputed transition;
        (void) style_transition_computed(bridge->stylesheet, node,
                                         transition_pseudo, &transition);
        /* Keyword lists fit the stack buffer; long linear() easings are
           measured and built on the JS heap. */
        char transition_value[640];
        size_t length = computed_transition_value(
            id, &transition, transition_value, sizeof(transition_value));
        if (length < sizeof(transition_value))
            return computed_style_answer(answer, computed_style_string(context, transition_value, empty));
        char *long_value = js_malloc(context, length + 1u);
        if (long_value == NULL) return computed_style_answer(answer, JS_EXCEPTION);
        (void) computed_transition_value(id, &transition, long_value,
                                         length + 1u);
        JSValue value = computed_style_string(context, long_value, empty);
        js_free(context, long_value);
        return computed_style_answer(answer, value);
    }
    ComputedStyle *style = &read->style;
    bool padding_geometry = id == CSP_WIDTH || id == CSP_HEIGHT
        || id == CSP_PADDING || id == CSP_PADDING_TOP
        || id == CSP_PADDING_RIGHT || id == CSP_PADDING_BOTTOM
        || id == CSP_PADDING_LEFT;
    int containing_width = 0, content_width = 0;
    /* Used widths still need their geometry walk. Container-dependent
       cascades are reusable under the container-state generation, after
       the same synchronous layout flush required above. */
    bool memoizable = !padding_geometry;
    __typeof__(bridge->computed_style_memo) *memo =
        &bridge->computed_style_memo;
    uint64_t content_generation = bridge->document == NULL
        ? 0 : bridge->document->content_generation;
    if (memoizable && memo->node == node
        && memo->content_generation == content_generation
        && computed_style_inputs_hold(bridge, &memo->inputs)) {
        *style = memo->style;
        style_container_units_for_node(
            (Stylesheet *) bridge->stylesheet, node);
    } else {
        if (!bridge_computed_style(bridge, node, style,
                                   padding_geometry ? &containing_width
                                                    : NULL,
                                   &content_width)) {
            memo->node = NULL;
            return computed_style_answer(
                answer, js_rt_throw_task_interruption(
                            context, "computed style interrupted"));
        }
        memo->node = memoizable ? node : NULL;
        if (memoizable) {
            computed_style_inputs_capture(bridge, &memo->inputs);
            memo->content_generation = content_generation;
            memo->style = *style;
        }
    }
    /* A light child the flat tree leaves out has no box (style_for_node
       resolves it display:none for layout), but getComputedStyle reports
       its cascaded display, as browsers do for an unslotted element. */
    if (style->display == DISPLAY_NONE && node->parent != NULL
        && document_shadow_carrier_of_host(node->parent) != NULL
        && !document_shadow_light_child_rendered(
               document_shadow_carrier_of_host(node->parent), node)) {
        ComputedStyle parent_style;
        if (bridge_computed_style(bridge, node->parent, &parent_style,
                                  NULL, NULL)) {
            parent_style.shadow_host = 0;
            ComputedStyle cascaded = style_for_node(
                bridge->stylesheet, node, &parent_style);
            style->display = cascaded.display;
        }
    }
    PseudoElement pseudo = PSEUDO_NONE;
    if (argc > 2) {
        size_t pseudo_length = 0;
        const char *pseudo_name =
            JS_ToCStringLen(context, &pseudo_length, argv[2]);
        if (pseudo_name == NULL) {
            return computed_style_answer(answer, JS_EXCEPTION);
        }
        if ((pseudo_length == 8 && memcmp(pseudo_name, "::before", 8) == 0)
            || (pseudo_length == 7
                && memcmp(pseudo_name, ":before", 7) == 0)) {
            pseudo = PSEUDO_BEFORE;
        } else if ((pseudo_length == 7
                    && memcmp(pseudo_name, "::after", 7) == 0)
                   || (pseudo_length == 6
                       && memcmp(pseudo_name, ":after", 6) == 0)) {
            pseudo = PSEUDO_AFTER;
        }
        if (pseudo_length == 13 && memcmp(pseudo_name, "::placeholder", 13) == 0)
            pseudo = PSEUDO_PLACEHOLDER;
        if (pseudo != PSEUDO_NONE) {
            /* A pseudo-element's containing block is its originating
               element, whose style this is. */
            ComputedStyle parent_style = *style;
            *style = style_for_pseudo(bridge->stylesheet, node, pseudo,
                                     &parent_style);
        }
        JS_FreeCString(context, pseudo_name);
    }
    if (pseudo != PSEUDO_NONE) containing_width = content_width;
    ComputedStylePropertyId physical_id =
        computed_style_scroll_physical_id(id, style);
    if (physical_id != id) {
        id = physical_id;
        name = computed_style_properties[id].name;
        name_length = strlen(name);
    }
    read->bridge = bridge;
    read->node = node;
    read->pseudo = pseudo;
    read->id = id;
    read->name = name;
    read->name_length = name_length;
    read->property_flags = property_flags;
    read->containing_width = containing_width;
    return true;
}

/* Serializes a resolved read into `value`, which holds value_size bytes:
   at least 640, because the bounded grid-template-areas parser accepts up
   to 511 source bytes and canonical row separators can add a few, and a
   truncated prefix would be a misleading value. Retained authored text
   (SVG presentation attributes, sparse scroll and modern properties) takes
   precedence over the computed fields. Needs no JSContext. */
static void computed_style_serialize(const ComputedStyleRead *read,
                                     char *value, size_t value_size)
{
    DomBridge *bridge = read->bridge;
    lxb_dom_node_t *node = read->node;
    const ComputedStyle *style = &read->style;
    PseudoElement pseudo = read->pseudo;
    ComputedStylePropertyId id = read->id;
    const char *name = read->name;
    size_t name_length = read->name_length;
    unsigned property_flags = read->property_flags;
    int containing_width = read->containing_width;
    static const char *display_names[] = {
        "inline", "inline-block", "inline-flex", "inline-grid", "block",
        "flow-root", "flex", "grid", "table", "table-row", "table-cell",
        "table-row-group", "table-header-group", "table-footer-group",
        "table-column", "contents", "none"
    };
    bool svg_presentation = svg_presentation_attribute_relevant(
        node, name, name_length);
    bool retained_presentation = svg_presentation
        && style_retained_presentation_value(
            bridge->stylesheet, node, name, name_length,
            value, value_size);
    size_t presentation_length = 0;
    const char *presentation = !retained_presentation && svg_presentation
        ? document_attribute(node, name, &presentation_length) : NULL;
    /* Retained authored text belongs to the element. A pseudo-element still
       has these properties; it reports their computed or initial value. */
    bool sparse_scroll = (property_flags & CSP_SPARSE_SCROLL) != 0;
    bool sparse_modern = (property_flags & CSP_SPARSE_MODERN) != 0;
    bool retained_scroll = sparse_scroll && pseudo == PSEUDO_NONE
        && (retained_scroll_box_value(
                bridge->stylesheet, node, style, name, name_length,
                value, value_size)
            || style_retained_property_value(
                bridge->stylesheet, node, name, name_length,
                value, value_size));
    if (retained_scroll
        && ((id >= CSP_SCROLL_MARGIN_BOTTOM && id <= CSP_SCROLL_MARGIN_TOP)
            || (id >= CSP_SCROLL_PADDING_BOTTOM
                && id <= CSP_SCROLL_PADDING_TOP))) {
        computed_style_scroll_length(bridge->stylesheet, style,
                                     value, value_size);
    }
    if (!retained_scroll
        && sparse_scroll
        && id == CSP_CURSOR) {
        /* Inherited: a pseudo-element's parent is its originating element. */
        for (lxb_dom_node_t *at = pseudo != PSEUDO_NONE ? node : node->parent;
             at != NULL && !retained_scroll; at = at->parent) {
            retained_scroll = style_retained_property_value(
                bridge->stylesheet, at, name, name_length,
                value, value_size);
        }
    }
    /* Vendor aliases are retained under their standard names. */
    const char *retained_modern_name =
        id == CSP_WEBKIT_USER_SELECT ? "user-select"
        : id == CSP_WEBKIT_TEXT_SIZE_ADJUST ? "text-size-adjust"
        : id == CSP_WEBKIT_BACKDROP_FILTER ? "backdrop-filter" : name;
    bool retained_modern = sparse_modern && pseudo == PSEUDO_NONE
        && style_retained_property_value(
            bridge->stylesheet, node, retained_modern_name,
            strlen(retained_modern_name), value, value_size);
    bool serialize_computed_modern = sparse_modern
        && (id == CSP_USER_SELECT
            || id == CSP_WEBKIT_USER_SELECT
            || id == CSP_TOUCH_ACTION
            || id == CSP_RESIZE
            || id == CSP_TEXT_WRAP
            || id == CSP_TEXT_WRAP_STYLE
            || id == CSP_ISOLATION
            || id == CSP_TEXT_SIZE_ADJUST
            || id == CSP_WEBKIT_TEXT_SIZE_ADJUST);
    bool authored_text_adjust_none = retained_modern
        && (id == CSP_TEXT_SIZE_ADJUST
            || id == CSP_WEBKIT_TEXT_SIZE_ADJUST)
        && strcasecmp(value, "none") == 0;
    bool authored_touch_manipulation = retained_modern
        && id == CSP_TOUCH_ACTION
        && strcasecmp(value, "manipulation") == 0;
    bool authored_touch_combined = retained_modern
        && id == CSP_TOUCH_ACTION
        && (strcasecmp(value, "pan-x pan-y") == 0
            || strcasecmp(value, "pan-y pan-x") == 0);
    if (retained_presentation || retained_scroll
        || (retained_modern && !serialize_computed_modern)) {
        /* Already serialized into value. */
        return;
    }
    if (presentation != NULL
        && presentation_length < value_size) {
        /* SVG presentation attributes participate at author origin with
           lower specificity than an authored rule.  The retained branches
           above have already given CSS its precedence; serialize the
           attribute before sparse-property initial values so `cursor` on a
           graphics element does not collapse back to `auto`. */
        memcpy(value, presentation, presentation_length);
        value[presentation_length] = '\0';
        return;
    }
    if (sparse_modern) {
        if (id == CSP_USER_SELECT
            || id == CSP_WEBKIT_USER_SELECT) {
            static const char *const values[] = {
                "auto", "text", "none", "all"
            };
            unsigned mode = computed_style_user_select(style);
            snprintf(value, value_size, "%s",
                     mode < 4u ? values[mode] : "auto");
        } else if (id == CSP_TOUCH_ACTION) {
            static const char *const values[] = {
                "auto", "none", "pan-x", "pan-y"
            };
            snprintf(value, value_size, "%s",
                     authored_touch_manipulation ? "manipulation"
                     : (authored_touch_combined ? "pan-x pan-y"
                     : (style->touch_action < 4u
                        ? values[style->touch_action] : "auto")));
        } else if (id == CSP_RESIZE) {
            static const char *const values[] = {
                "none", "both", "horizontal", "vertical"
            };
            snprintf(value, value_size, "%s",
                     style->resize_mode < 4u
                         ? values[style->resize_mode] : "none");
        } else if (id == CSP_TEXT_WRAP
                   || id == CSP_TEXT_WRAP_STYLE) {
            StyleTextWrap mode = computed_style_text_wrap(style);
            snprintf(value, value_size, "%s",
                     mode == STYLE_TEXT_WRAP_BALANCE ? "balance"
                     : (mode == STYLE_TEXT_WRAP_PRETTY ? "pretty"
                        : (id == CSP_TEXT_WRAP_STYLE
                           ? "auto" : "wrap")));
        } else if (id == CSP_ISOLATION) {
            snprintf(value, value_size, "%s",
                     computed_style_isolation_isolate(style)
                         ? "isolate" : "auto");
        } else if (id == CSP_TEXT_SIZE_ADJUST
                   || id == CSP_WEBKIT_TEXT_SIZE_ADJUST) {
            if (authored_text_adjust_none) {
                snprintf(value, value_size, "100%%");
            } else if (style->font_size_unit != 0u) {
                snprintf(value, value_size, "%u%%",
                         (unsigned) style->font_size_unit - 1u);
            } else {
                snprintf(value, value_size, "auto");
            }
        } else if (id == CSP_BORDER_START_START_RADIUS
                   || id == CSP_BORDER_START_END_RADIUS
                   || id == CSP_BORDER_END_START_RADIUS
                   || id == CSP_BORDER_END_END_RADIUS) {
            /* Not authored as a logical longhand, so the used value is the
               physical corner this one maps to under the element's writing
               mode and direction. The name is border-<block>-<inline>-radius;
               corners run top-left, top-right, bottom-right, bottom-left. */
            bool block_start = id == CSP_BORDER_START_START_RADIUS
                || id == CSP_BORDER_START_END_RADIUS;
            bool inline_start = (id == CSP_BORDER_START_START_RADIUS
                                 || id == CSP_BORDER_END_START_RADIUS)
                != computed_style_direction_rtl(style);
            unsigned writing_mode = computed_style_writing_mode(style);
            bool top = writing_mode == 0u ? block_start : inline_start;
            bool left = writing_mode == 0u ? inline_start
                : (writing_mode == 2u ? block_start : !block_start);
            snprintf(value, value_size, "%dpx",
                     style_border_radius_corner(
                         stylesheet_border_radius_code(
                             bridge->stylesheet, style),
                         top ? (left ? 0u : 1u) : (left ? 3u : 2u)));
        } else {
            /* Nothing authored on this element. `none` is the initial value
               of only some of these; the rest have a retained inherited
               state or a different initial keyword. */
            snprintf(value, value_size, "%s",
                     computed_style_sparse_modern_initial(style, id));
        }
        return;
    }
    if (sparse_scroll) {
        const char *initial =
            id == CSP_SCROLL_SNAP_ALIGN
                ? "none"
            : id == CSP_SCROLL_SNAP_STOP
                ? "normal"
            : id == CSP_SCROLL_SNAP_TYPE
                ? "none"
            : id == CSP_SCROLLBAR_COLOR
                ? "auto"
            : id == CSP_SCROLLBAR_WIDTH
                ? "auto"
            : id == CSP_CURSOR
                ? "auto"
            : id == CSP_SCROLL_BEHAVIOR
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_X
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_Y
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_INLINE
                ? "auto"
            : id == CSP_OVERSCROLL_BEHAVIOR_BLOCK
                ? "auto"
                : "0px";
        snprintf(value, value_size, "%s", initial);
        return;
    }
    if (name_length >= 3 && name[0] == '-' && name[1] == '-') {
        (void) style_custom_property_value(
            bridge->stylesheet, node, pseudo, name, name_length,
            value, value_size);
        return;
    }
    switch (id) {
    case CSP_DISPLAY: {
        /* CSS Display 2.7: floats and absolutely positioned boxes compute
           to their blockified display; a list item serializes as such. */
        DisplayMode display = style->display;
        if (style->float_mode != FLOAT_NONE || style->out_of_flow
            || style->fixed_position) {
            if (display == DISPLAY_INLINE
                || display == DISPLAY_INLINE_BLOCK) display = DISPLAY_BLOCK;
            else if (display == DISPLAY_INLINE_FLEX) display = DISPLAY_FLEX;
            else if (display == DISPLAY_INLINE_GRID) display = DISPLAY_GRID;
        }
        snprintf(value, value_size, "%s",
                 display == DISPLAY_BLOCK && style->list_item
                     ? "list-item" : display_names[display]);
        break;
    }
    case CSP_VISIBILITY: {
        snprintf(value, value_size, "%s",
                 style->visibility_hidden ? "hidden" : "visible");
        break;
    }
    case CSP_OPACITY: {
        /* Painting keeps opacity in one byte on the PSP. Recover the
           shortest decimal (up to thousandths) which quantizes to that byte
           so common authored values such as .5 and .25 retain their CSS
           computed serialization instead of leaking 8-bit paint rounding. */
        bool formatted = false;
        unsigned scale = 10;
        for (int digits = 1; digits <= 3 && !formatted; digits++, scale *= 10) {
            unsigned candidate =
                ((unsigned) style->opacity * scale + 127u) / 255u;
            if ((candidate * 255u + scale / 2u) / scale != style->opacity) {
                continue;
            }
            snprintf(value, value_size, "%u.%0*u",
                     candidate / scale, digits, candidate % scale);
            size_t used = strlen(value);
            while (used > 1 && value[used - 1] == '0') value[--used] = '\0';
            if (used > 1 && value[used - 1] == '.') value[--used] = '\0';
            formatted = true;
        }
        if (!formatted) snprintf(value, value_size, "%.3g",
                                 (double) style->opacity / 255.0);
        break;
    }
    case CSP_COLOR: {
        (void) serialize_computed_color(
            value, value_size, style->color, style->color_alpha);
        break;
    }
    case CSP_BACKGROUND_COLOR: {
        (void) serialize_computed_color(
            value, value_size,
            style->has_background ? style->background : 0,
            style->has_background ? style->background_alpha : 0);
        break;
    }
    case CSP_BACKGROUND_ORIGIN:
    case CSP_BACKGROUND_CLIP: {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(style));
        bool origin = id == CSP_BACKGROUND_ORIGIN;
        StylePaintBox box = origin ? STYLE_PAINT_BOX_PADDING
                                   : STYLE_PAINT_BOX_BORDER;
        if (paint != NULL && paint->background_count != 0
            && (paint->components
                & STYLE_PAINT_COMPONENT_BACKGROUND_BOX) != 0) {
            box = (StylePaintBox) (origin
                ? paint->backgrounds[0].origin
                : paint->backgrounds[0].clip);
        }
        snprintf(value, value_size, "%s",
                 box == STYLE_PAINT_BOX_CONTENT ? "content-box"
                 : (box == STYLE_PAINT_BOX_PADDING
                    ? "padding-box"
                    : (box == STYLE_PAINT_BOX_TEXT
                       ? "text" : "border-box")));
        break;
    }
    case CSP_BORDER_SPACING: {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(style));
        unsigned x = 0, y = 0;
        if (paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TABLE_SPACING) != 0) {
            x = paint->table_spacing_x;
            y = paint->table_spacing_y;
        }
        snprintf(value, value_size, "%upx %upx", x, y);
        break;
    }
    case CSP_BACKGROUND_IMAGE: {
        if (style->background_image == NULL || style->background_image[0] == '\0') {
            snprintf(value, value_size, "none");
        } else {
            snprintf(value, value_size, "url(\"%s\")",
                     style->background_image);
        }
        break;
    }
    case CSP_CONTENT: {
        if (!style->generated_content) {
            snprintf(value, value_size, "none");
        } else if (style->generated_image) {
            snprintf(value, value_size, "url(\"%s\")", style->generated_text);
        } else if (style->generated_text != NULL) {
            snprintf(value, value_size, "\"%.*s\"",
                     (int) style->generated_text_length,
                     style->generated_text);
        } else {
            snprintf(value, value_size, "\"\"");
        }
        break;
    }
    case CSP_BACKGROUND_SIZE: {
        if ((style->background_size_flags
             & STYLE_BACKGROUND_SIZE_EXPLICIT) != 0) {
            char width[24], height[24];
            if ((style->background_size_flags
                 & STYLE_BACKGROUND_WIDTH_AUTO) != 0) {
                snprintf(width, sizeof(width), "auto");
            } else {
                snprintf(width, sizeof(width), "%u%s",
                         style->background_width,
                         (style->background_size_flags
                          & STYLE_BACKGROUND_WIDTH_PERCENT) != 0
                           ? "%" : "px");
            }
            if ((style->background_size_flags
                 & STYLE_BACKGROUND_HEIGHT_AUTO) != 0) {
                snprintf(height, sizeof(height), "auto");
            } else {
                snprintf(height, sizeof(height), "%u%s",
                         style->background_height,
                         (style->background_size_flags
                          & STYLE_BACKGROUND_HEIGHT_PERCENT) != 0
                           ? "%" : "px");
            }
            snprintf(value, value_size, "%s %s", width, height);
        } else {
            snprintf(value, value_size, "%s", style->background_fit == 1
                     ? "cover" : (style->background_fit == 2
                                   ? "contain" : "auto"));
        }
        break;
    }
    case CSP_BACKGROUND_POSITION: {
        snprintf(value, value_size, "%d%% %d%%",
                 style->background_position_x,
                 style->background_position_y);
        break;
    }
    case CSP_BOX_SHADOW: {
        size_t box_shadow_count = stylesheet_box_shadow_count(
            bridge->stylesheet, style);
        if (box_shadow_count == 0) {
            snprintf(value, value_size, "none");
        } else {
            size_t used = 0;
            for (size_t i = 0; i < box_shadow_count
                               && i < STYLE_BOX_SHADOW_LIMIT; i++) {
                const StyleBoxShadow *shadow = stylesheet_box_shadow(
                    bridge->stylesheet, style, i);
                if (shadow == NULL) continue;
                uint32_t argb = style_box_shadow_uses_current_color(shadow)
                    ? ((uint32_t) style->color_alpha << 24)
                        | (style->color & UINT32_C(0x00ffffff))
                    : shadow->argb;
                unsigned alpha = (argb >> 24) & 255u;
                uint32_t color = argb & UINT32_C(0x00ffffff);
                char serialized_color[48];
                (void) serialize_computed_color(
                    serialized_color, sizeof(serialized_color), color, alpha);
                int written = snprintf(
                    value + used, value_size - used,
                    "%s%s %dpx %dpx %dpx %dpx%s",
                    i == 0 ? "" : ", ", serialized_color,
                    shadow->offset_x, shadow->offset_y,
                    style_box_shadow_blur(shadow), shadow->spread,
                    style_box_shadow_is_inset(shadow) ? " inset" : "");
                if (written < 0 || (size_t) written >= value_size - used) {
                    value[value_size - 1] = '\0';
                    break;
                }
                used += (size_t) written;
            }
        }
        break;
    }
    case CSP_TEXT_SHADOW: {
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(style));
        size_t count = paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TEXT_SHADOW) != 0
            ? paint->text_shadow_count : 0;
        if (count == 0) {
            snprintf(value, value_size, "none");
        } else {
            size_t used = 0;
            if (count > STYLE_BOX_SHADOW_LIMIT) {
                count = STYLE_BOX_SHADOW_LIMIT;
            }
            for (size_t i = 0; i < count; i++) {
                const StyleBoxShadow *shadow = &paint->text_shadows[i];
                uint32_t argb = style_box_shadow_uses_current_color(shadow)
                    ? ((uint32_t) style->color_alpha << 24)
                      | (style->color & UINT32_C(0x00ffffff))
                    : shadow->argb;
                char serialized_color[48];
                (void) serialize_computed_color(
                    serialized_color, sizeof(serialized_color),
                    argb & UINT32_C(0x00ffffff), (argb >> 24) & 255u);
                int written = snprintf(
                    value + used, value_size - used,
                    "%s%s %dpx %dpx %dpx",
                    i == 0 ? "" : ", ", serialized_color,
                    shadow->offset_x, shadow->offset_y,
                    style_box_shadow_blur(shadow));
                if (written < 0 || (size_t) written >= value_size - used) {
                    value[value_size - 1] = '\0';
                    break;
                }
                used += (size_t) written;
            }
        }
        break;
    }
    case CSP_OBJECT_FIT: {
        const char *fit = "fill";
        if (style->object_fit == STYLE_OBJECT_FIT_COVER) fit = "cover";
        else if (style->object_fit == STYLE_OBJECT_FIT_CONTAIN) fit = "contain";
        else if (style->object_fit == STYLE_OBJECT_FIT_NONE) fit = "none";
        else if (style->object_fit == STYLE_OBJECT_FIT_SCALE_DOWN) {
            fit = "scale-down";
        }
        snprintf(value, value_size, "%s", fit);
        break;
    }
    case CSP_OBJECT_POSITION: {
        int x = style_object_position_percent(style->object_position_x);
        int y = style_object_position_percent(style->object_position_y);
        int offset_x = style_object_position_offset(style->object_position_x);
        int offset_y = style_object_position_offset(style->object_position_y);
        if (offset_x == 0 && offset_y == 0) {
            snprintf(value, value_size, "%d%% %d%%", x, y);
        } else {
            snprintf(value, value_size,
                     "calc(%d%% + %dpx) calc(%d%% + %dpx)",
                     x, offset_x, y, offset_y);
        }
        break;
    }
    case CSP_APPEARANCE:
    case CSP_WEBKIT_APPEARANCE: {
        static const char *const appearance_names[] = {
            "none", "auto", "base", "base-select", "button", "checkbox",
            "listbox", "menulist-button", "meter", "progress-bar", "radio",
            "searchfield", "textarea", "textfield"
        };
        size_t appearance = style->appearance & STYLE_APPEARANCE_MASK;
        if (appearance >= sizeof(appearance_names)
                          / sizeof(appearance_names[0])) {
            appearance = APPEARANCE_NONE;
        }
        snprintf(value, value_size, "%s", appearance_names[appearance]);
        break;
    }
    case CSP_JUSTIFY_SELF: {
        static const char *const justify_self_names[] = {
            "auto", "start", "center", "end", "stretch", "baseline"
        };
        size_t justify_self = computed_style_justify_self(style);
        if (justify_self >= sizeof(justify_self_names)
                            / sizeof(justify_self_names[0])) {
            justify_self = ALIGN_SELF_AUTO;
        }
        snprintf(value, value_size, "%s",
                 justify_self_names[justify_self]);
        break;
    }
    case CSP_JUSTIFY_ITEMS: {
        static const char *const item_names[] = {
            "start", "center", "end", "stretch", "baseline"
        };
        size_t item = style->justify_items;
        if (item >= sizeof(item_names) / sizeof(item_names[0])) {
            item = ALIGN_STRETCH;
        }
        snprintf(value, value_size, "%s", item_names[item]);
        break;
    }
    case CSP_PLACE_SELF: {
        static const char *const self_names[] = {
            "auto", "start", "center", "end", "stretch", "baseline"
        };
        size_t align = style->align_self;
        size_t justify = computed_style_justify_self(style);
        if (align >= sizeof(self_names) / sizeof(self_names[0])) {
            align = ALIGN_SELF_AUTO;
        }
        if (justify >= sizeof(self_names) / sizeof(self_names[0])) {
            justify = ALIGN_SELF_AUTO;
        }
        if (align == justify) {
            snprintf(value, value_size, "%s", self_names[align]);
        } else {
            snprintf(value, value_size, "%s %s",
                     self_names[align], self_names[justify]);
        }
        break;
    }
    case CSP_PLACE_ITEMS: {
        static const char *const item_names[] = {
            "start", "center", "end", "stretch", "baseline"
        };
        size_t align = style->align_items;
        size_t justify = style->justify_items;
        if (align >= sizeof(item_names) / sizeof(item_names[0])) {
            align = ALIGN_STRETCH;
        }
        if (justify >= sizeof(item_names) / sizeof(item_names[0])) {
            justify = ALIGN_STRETCH;
        }
        if (align == justify) {
            snprintf(value, value_size, "%s", item_names[align]);
        } else {
            snprintf(value, value_size, "%s %s",
                     item_names[align], item_names[justify]);
        }
        break;
    }
    case CSP_PLACE_CONTENT: {
        static const char *const content_names[] = {
            "start", "center", "end", "space-between", "space-around",
            "space-evenly", "stretch"
        };
        size_t align = style->align_content;
        size_t justify = style->justify_content;
        if (align >= sizeof(content_names) / sizeof(content_names[0])) {
            align = JUSTIFY_STRETCH;
        }
        if (justify >= sizeof(content_names) / sizeof(content_names[0])) {
            justify = JUSTIFY_START;
        }
        if (align == justify) {
            snprintf(value, value_size, "%s", content_names[align]);
        } else {
            snprintf(value, value_size, "%s %s",
                     content_names[align], content_names[justify]);
        }
        break;
    }
    case CSP_FONT_SIZE: {
        int fixed = computed_style_font_size_fixed(style);
        if ((fixed & 63) == 0) {
            snprintf(value, value_size, "%dpx", fixed / 64);
        } else {
            snprintf(value, value_size, "%.6fpx", fixed / 64.0);
        }
        break;
    }
    case CSP_FONT_FAMILY: {
        FontFamily family = font_family_is_web(style->font_family)
            ? font_family_web_fallback(style->font_family)
            : style->font_family;
        snprintf(value, value_size, "%s",
                 family == FONT_MONOSPACE ? "monospace"
                 : family == FONT_SERIF ? "serif" : "sans-serif");
        break;
    }
    case CSP_TEXT_INDENT: {
        const LayoutNodeBox *box = layout_box_for_node(js_rt_bridge_layout(bridge), node);
        int used_indent = 0;
        if (!style_length_resolve(
                bridge->stylesheet, style->text_indent,
                box != NULL ? box->content_width : 0, &used_indent)) {
            used_indent = 0;
        }
        snprintf(value, value_size, "%dpx", used_indent);
        break;
    }
    case CSP_TEXT_OVERFLOW: {
        snprintf(value, value_size, "%s",
                 computed_style_text_overflow_ellipsis(style)
                   ? "ellipsis" : "clip");
        break;
    }
    case CSP_FONT_WEIGHT: {
        snprintf(value, value_size, "%u",
                 style->font_weight != 0 ? style->font_weight
                                         : (style->font_bold ? 700u : 400u));
        break;
    }
    case CSP_FONT_STYLE: {
        snprintf(value, value_size, "%s",
                 style->font_italic ? "italic" : "normal");
        break;
    }
    case CSP_POSITION: {
        snprintf(value, value_size, "%s", style->fixed_position ? "fixed"
                 : style->sticky_position ? "sticky"
                 : style->out_of_flow ? "absolute"
                 : style->relative_position ? "relative" : "static");
        break;
    }
    case CSP_TOP: {
        if (!style->has_top) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%d%s", style->top,
                      style->inset_percent_mask & STYLE_INSET_TOP_PERCENT
                      ? "%" : "px");
        break;
    }
    case CSP_RIGHT: {
        if (!style->has_right) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%d%s", style->right,
                      style->inset_percent_mask & STYLE_INSET_RIGHT_PERCENT
                      ? "%" : "px");
        break;
    }
    case CSP_BOTTOM: {
        if (!style->has_bottom) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%d%s", style->bottom,
                      style->inset_percent_mask & STYLE_INSET_BOTTOM_PERCENT
                      ? "%" : "px");
        break;
    }
    case CSP_LEFT: {
        if (!style->has_left) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%d%s", style->left,
                      style->inset_percent_mask & STYLE_INSET_LEFT_PERCENT
                      ? "%" : "px");
        break;
    }
    case CSP_OVERFLOW: {
        const char *x = style->overflow_x_clip_only ? "clip"
            : computed_style_overflow_x_hidden(style) ? "hidden"
            : (style->overflow_x_scroll ? "auto" : "visible");
        const char *y = style->overflow_y_clip_only
            ? "clip" : style->overflow_y_hidden ? "hidden"
            : (style->overflow_y_scroll ? "auto" : "visible");
        if (strcmp(x, y) == 0) snprintf(value, value_size, "%s", x);
        else snprintf(value, value_size, "%s %s", x, y);
        break;
    }
    case CSP_OVERFLOW_X: {
        snprintf(value, value_size, "%s",
                 style->overflow_x_clip_only ? "clip"
                 : computed_style_overflow_x_hidden(style) ? "hidden"
                 : (style->overflow_x_scroll ? "auto" : "visible"));
        break;
    }
    case CSP_OVERFLOW_Y: {
        snprintf(value, value_size, "%s", style->overflow_y_clip_only
                 ? "clip" : style->overflow_y_hidden ? "hidden"
                 : (style->overflow_y_scroll ? "auto" : "visible"));
        break;
    }
    case CSP_SCROLLBAR_GUTTER: {
        snprintf(value, value_size, "%s",
                 style->scrollbar_gutter_stable ? "stable" : "auto");
        break;
    }
    case CSP_BORDER_RADIUS: {
        int code = stylesheet_border_radius_code(
            bridge->stylesheet, style);
        if (style_border_radius_is_packed(code)) {
            snprintf(value, value_size, "%dpx %dpx %dpx %dpx",
                     style_border_radius_corner(code, 0),
                     style_border_radius_corner(code, 1),
                     style_border_radius_corner(code, 2),
                     style_border_radius_corner(code, 3));
        } else {
            snprintf(value, value_size, "%dpx", code);
        }
        break;
    }
    case CSP_BORDER_TOP_LEFT_RADIUS:
    case CSP_BORDER_TOP_RIGHT_RADIUS:
    case CSP_BORDER_BOTTOM_RIGHT_RADIUS:
    case CSP_BORDER_BOTTOM_LEFT_RADIUS: {
        int code = stylesheet_border_radius_code(
            bridge->stylesheet, style);
        unsigned corner = name[7] == 't'
            ? (name[11] == 'l' ? 0u : 1u)
            : (name[14] == 'r' ? 2u : 3u);
        snprintf(value, value_size, "%dpx",
                 style_border_radius_corner(code, corner));
        break;
    }
    case CSP_BORDER_COLLAPSE: {
        snprintf(value, value_size, "%s",
                 style->table_border_collapse ? "collapse" : "separate");
        break;
    }
    case CSP_CAPTION_SIDE: {
        snprintf(value, value_size, "%s",
                 style->order >= 200000 ? "bottom" : "top");
        break;
    }
    case CSP_OUTLINE_WIDTH: {
        snprintf(value, value_size, "%upx",
                 computed_style_outline_width(style));
        break;
    }
    case CSP_OUTLINE_STYLE: {
        static const char *const outline_names[] = {
            "none", "solid", "dashed", "dotted"
        };
        unsigned outline = computed_style_outline_style(style);
        if (outline >= sizeof(outline_names) / sizeof(outline_names[0])) {
            outline = STYLE_OUTLINE_NONE;
        }
        snprintf(value, value_size, "%s", outline_names[outline]);
        break;
    }
    case CSP_OUTLINE_OFFSET: {
        snprintf(value, value_size, "%dpx",
                 computed_style_outline_offset(style));
        break;
    }
    case CSP_OUTLINE_COLOR: {
        uint32_t color = (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR)
            ? style->color : style->outline_color;
        uint8_t alpha = (style->outline_state & STYLE_OUTLINE_CURRENT_COLOR)
            ? style->color_alpha : style->outline_alpha;
        if (alpha == 255) {
            snprintf(value, value_size, "rgb(%u, %u, %u)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u));
        } else {
            snprintf(value, value_size, "rgba(%u, %u, %u, %.3g)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u),
                     (double) alpha / 255.0);
        }
        break;
    }
    case CSP_CLIP_PATH: {
        unsigned clip = computed_style_clip_path_type(style);
        if (clip == STYLE_CLIP_PATH_CIRCLE) {
            snprintf(value, value_size, "circle()");
        } else if (clip == STYLE_CLIP_PATH_INSET) {
            unsigned inset = computed_style_clip_path_inset(style);
            unsigned radius = computed_style_clip_path_radius(style);
            const char *unit =
                (style->clip_path_state & STYLE_CLIP_PATH_INSET_PERCENT)
                ? "%" : "px";
            if (radius != 0) {
                snprintf(value, value_size, "inset(%u%s round %upx)",
                         inset, unit, radius);
            } else {
                snprintf(value, value_size, "inset(%u%s)", inset, unit);
            }
        } else {
            snprintf(value, value_size, "none");
        }
        break;
    }
    case CSP_WHITE_SPACE: {
        static const char *const white_space_names[] = {
            "normal", "nowrap", "pre", "pre-wrap", "pre-line",
            "break-spaces"
        };
        size_t mode = style->white_space_mode;
        if (mode >= sizeof(white_space_names)
                    / sizeof(white_space_names[0])) {
            mode = WHITE_SPACE_NORMAL;
        }
        snprintf(value, value_size, "%s", white_space_names[mode]);
        break;
    }
    case CSP_TEXT_TRANSFORM: {
        static const char *const transform_names[] = {
            "none", "uppercase", "lowercase", "capitalize"
        };
        size_t transform = style->text_transform;
        if (transform >= sizeof(transform_names)
                         / sizeof(transform_names[0])) {
            transform = TEXT_TRANSFORM_NONE;
        }
        snprintf(value, value_size, "%s", transform_names[transform]);
        break;
    }
    case CSP_BOX_SIZING: {
        snprintf(value, value_size, "%s",
                 style->box_sizing_border_box ? "border-box" : "content-box");
        break;
    }
    case CSP_FLEX: {
        char basis[32];
        if (!style->has_flex_basis) {
            snprintf(basis, sizeof(basis), "auto");
        } else if (style->flex_basis == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(basis, sizeof(basis), "min-content");
        } else if (style->flex_basis == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(basis, sizeof(basis), "max-content");
        } else if (style->flex_basis == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(basis, sizeof(basis), "fit-content");
        } else {
            snprintf(basis, sizeof(basis), "%d%s", style->flex_basis,
                     style->flex_basis_percent ? "%" : "px");
        }
        snprintf(value, value_size, "%.3g %.3g %s",
                 (double) style->flex_grow / 1000.0,
                 (double) style->flex_shrink / 512.0, basis);
        break;
    }
    case CSP_FLEX_DIRECTION: {
        static const char *direction_names[] = {
            "row", "row-reverse", "column", "column-reverse"
        };
        snprintf(value, value_size, "%s",
                 direction_names[style->flex_direction]);
        break;
    }
    case CSP_FLEX_WRAP: {
        snprintf(value, value_size, "%s", style->flex_wrap_reverse
                 ? "wrap-reverse" : (style->flex_wrap ? "wrap" : "nowrap"));
        break;
    }
    case CSP_JUSTIFY_CONTENT: {
        static const char *justify_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        size_t justify = style->justify_content;
        if (justify >= sizeof(justify_names) / sizeof(justify_names[0])) {
            justify = JUSTIFY_START;
        }
        snprintf(value, value_size, "%s", justify_names[justify]);
        break;
    }
    case CSP_ALIGN_ITEMS: {
        static const char *align_names[] = {
            "flex-start", "center", "flex-end", "stretch", "baseline"
        };
        size_t align = style->align_items;
        if (align >= sizeof(align_names) / sizeof(align_names[0])) {
            align = ALIGN_STRETCH;
        }
        snprintf(value, value_size, "%s", align_names[align]);
        break;
    }
    case CSP_ALIGN_SELF: {
        static const char *align_self_names[] = {
            "auto", "flex-start", "center", "flex-end", "stretch",
            "baseline"
        };
        snprintf(value, value_size, "%s",
                 align_self_names[style->align_self]);
        break;
    }
    case CSP_ALIGN_CONTENT: {
        static const char *align_content_names[] = {
            "flex-start", "center", "flex-end", "space-between",
            "space-around", "space-evenly", "stretch"
        };
        snprintf(value, value_size, "%s",
                 align_content_names[style->align_content]);
        break;
    }
    case CSP_ORDER: {
        snprintf(value, value_size, "%d", style->order);
        break;
    }
    case CSP_Z_INDEX: {
        if (style->has_z_index) snprintf(value, value_size, "%d", style->z_index);
        else snprintf(value, value_size, "auto");
        break;
    }
    case CSP_WIDTH: {
        /* The resolved value of width is the used value whenever the element
           generates a box: a percentage or sizing keyword must come back in
           pixels. Only an element without a box, or a non-replaced inline,
           reports its computed value. */
        if (computed_style_used_box_size(
                bridge, node, style, containing_width, pseudo, false,
                value, value_size)) {
            /* Used pixels. */
        } else if (!style->has_width) {
            snprintf(value, value_size, "auto");
        } else {
            if (computed_style_width_min_content(style)) {
                snprintf(value, value_size, "min-content");
            } else if (computed_style_width_max_content(style)) {
                snprintf(value, value_size, "max-content");
            } else if (computed_style_width_fit_content(style)) {
                snprintf(value, value_size, "fit-content");
            } else {
                snprintf(value, value_size, "%d%s", style->width,
                         style->width_percent ? "%" : "px");
            }
        }
        break;
    }
    case CSP_MIN_WIDTH: {
        if (style->min_width_auto) snprintf(value, value_size, "auto");
        else if (style->min_width == STYLE_LENGTH_MIN_CONTENT)
            snprintf(value, value_size, "min-content");
        else if (style->min_width == STYLE_LENGTH_MAX_CONTENT)
            snprintf(value, value_size, "max-content");
        else if (style->min_width == STYLE_LENGTH_FIT_CONTENT)
            snprintf(value, value_size, "fit-content");
        else snprintf(value, value_size, "%d%s", style->min_width,
                      style->min_width_percent ? "%" : "px");
        break;
    }
    case CSP_MAX_WIDTH: {
        if (style->max_width == STYLE_LENGTH_NONE) {
            snprintf(value, value_size, "none");
        } else if (style->max_width == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, value_size, "min-content");
        } else if (style->max_width == STYLE_LENGTH_MAX_CONTENT) {
            snprintf(value, value_size, "max-content");
        } else if (style->max_width == STYLE_LENGTH_FIT_CONTENT) {
            snprintf(value, value_size, "fit-content");
        } else snprintf(value, value_size, "%d%s", style->max_width,
                      style->max_width_percent ? "%" : "px");
        break;
    }
    case CSP_HEIGHT: {
        if (computed_style_used_box_size(
                bridge, node, style, containing_width, pseudo, true,
                value, value_size)) {
            /* Used pixels. */
        } else if (!style->has_height) {
            snprintf(value, value_size, "auto");
        } else {
            snprintf(value, value_size, "%d%s", style->height,
                     style->height_percent ? "%" : "px");
        }
        break;
    }
    case CSP_MIN_HEIGHT: {
        snprintf(value, value_size, "%d%s", style->min_height,
                 style->min_height_percent ? "%" : "px");
        break;
    }
    case CSP_MAX_HEIGHT: {
        if (style->max_height == STYLE_LENGTH_NONE) {
            snprintf(value, value_size, "none");
        } else if (style->max_height == STYLE_LENGTH_MIN_CONTENT) {
            snprintf(value, value_size, "min-content");
        }
        else snprintf(value, value_size, "%d%s", style->max_height,
                      style->max_height_percent ? "%" : "px");
        break;
    }
    case CSP_MARGIN_TOP: {
        if (style->margin_top_auto) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%dpx", style->margin.top);
        break;
    }
    case CSP_MARGIN_RIGHT: {
        if (style->margin_right_auto) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%dpx", style->margin.right);
        break;
    }
    case CSP_MARGIN_BOTTOM: {
        if (style->margin_bottom_auto) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%dpx", style->margin.bottom);
        break;
    }
    case CSP_MARGIN_LEFT: {
        if (style->margin_left_auto) snprintf(value, value_size, "auto");
        else snprintf(value, value_size, "%dpx", style->margin.left);
        break;
    }
    case CSP_MARGIN: {
        /* Four values, as `padding` reports them; an auto side keeps its
           keyword. */
        const int sides[4] = { style->margin.top, style->margin.right,
                               style->margin.bottom, style->margin.left };
        const bool automatic[4] = {
            style->margin_top_auto, style->margin_right_auto,
            style->margin_bottom_auto, style->margin_left_auto
        };
        size_t used = 0;
        for (unsigned side = 0; side < 4u && used < value_size; side++) {
            int written = automatic[side]
                ? snprintf(value + used, value_size - used, "%sauto",
                           side == 0 ? "" : " ")
                : snprintf(value + used, value_size - used, "%s%dpx",
                           side == 0 ? "" : " ", sides[side]);
            if (written < 0) break;
            used += (size_t) written;
        }
        break;
    }
    case CSP_PADDING_TOP: {
        snprintf(value, value_size, "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.top));
        break;
    }
    case CSP_PADDING_RIGHT: {
        snprintf(value, value_size, "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.right));
        break;
    }
    case CSP_PADDING_BOTTOM: {
        snprintf(value, value_size, "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.bottom));
        break;
    }
    case CSP_PADDING_LEFT: {
        snprintf(value, value_size, "%dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.left));
        break;
    }
    case CSP_PADDING: {
        snprintf(value, value_size, "%dpx %dpx %dpx %dpx",
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.top),
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.right),
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.bottom),
                 computed_style_used_padding(
                     bridge, containing_width, style->padding.left));
        break;
    }
    case CSP_BORDER_TOP_WIDTH: {
        snprintf(value, value_size, "%dpx", style->border.top);
        break;
    }
    case CSP_BORDER_RIGHT_WIDTH: {
        snprintf(value, value_size, "%dpx", style->border.right);
        break;
    }
    case CSP_BORDER_BOTTOM_WIDTH: {
        snprintf(value, value_size, "%dpx", style->border.bottom);
        break;
    }
    case CSP_BORDER_LEFT_WIDTH: {
        snprintf(value, value_size, "%dpx", style->border.left);
        break;
    }
    case CSP_BORDER_WIDTH: {
        snprintf(value, value_size, "%dpx %dpx %dpx %dpx",
                 style->border.top, style->border.right,
                 style->border.bottom, style->border.left);
        break;
    }
    case CSP_BORDER_TOP_STYLE:
    case CSP_BORDER_RIGHT_STYLE:
    case CSP_BORDER_BOTTOM_STYLE:
    case CSP_BORDER_LEFT_STYLE: {
        static const char *const border_lines[] = {
            "none", "solid", "dashed", "dotted"
        };
        StyleBorderSide side =
            name[7] == 't' ? STYLE_BORDER_TOP
            : (name[7] == 'r' ? STYLE_BORDER_RIGHT
               : (name[7] == 'b' ? STYLE_BORDER_BOTTOM
                                  : STYLE_BORDER_LEFT));
        unsigned line = computed_style_border_line(style, side);
        if (line >= sizeof(border_lines) / sizeof(border_lines[0])) {
            line = STYLE_BORDER_NONE;
        }
        snprintf(value, value_size, "%s", border_lines[line]);
        break;
    }
    case CSP_BORDER_TOP_COLOR:
    case CSP_BORDER_RIGHT_COLOR:
    case CSP_BORDER_BOTTOM_COLOR:
    case CSP_BORDER_LEFT_COLOR: {
        StyleBorderSide side =
            name[7] == 't' ? STYLE_BORDER_TOP
            : (name[7] == 'r' ? STYLE_BORDER_RIGHT
               : (name[7] == 'b' ? STYLE_BORDER_BOTTOM
                                  : STYLE_BORDER_LEFT));
        uint8_t alpha = 255;
        uint32_t color = stylesheet_border_color(
            bridge->stylesheet, style, side, &alpha);
        if (alpha == 255) {
            snprintf(value, value_size, "rgb(%u, %u, %u)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u));
        } else {
            snprintf(value, value_size, "rgba(%u, %u, %u, %.3g)",
                     (unsigned) ((color >> 16) & 255u),
                     (unsigned) ((color >> 8) & 255u),
                     (unsigned) (color & 255u),
                     (double) alpha / 255.0);
        }
        break;
    }
    case CSP_TRANSFORM: {
        /* This serializer retains percentage translations rather than a
           box-dependent matrix. It needs the cascade, not scratch layout. */
        if (!style->has_transform) {
            snprintf(value, value_size, "none");
        } else if (style->transform_x > STYLE_LENGTH_DIRECT_LIMIT
                   || style->transform_y > STYLE_LENGTH_DIRECT_LIMIT) {
            const LayoutNodeBox *box = layout_box_for_node(js_rt_bridge_layout(bridge), node);
            int x, y;
            style_transform_resolve_translation(bridge->stylesheet, style,
                box == NULL ? 0 : box->width, box == NULL ? 0 : box->height,
                &x, &y);
            snprintf(value, value_size, "translate(%dpx, %dpx)", x, y);
        } else {
            snprintf(value, value_size, "translate(%d%s, %d%s)",
                     style->transform_x,
                     style->transform_x_percent ? "%" : "px",
                     style->transform_y,
                     style->transform_y_percent ? "%" : "px");
        }
        break;
    }
    case CSP_TRANSFORM_ORIGIN: {
        uint16_t x = style_object_position_encode(50, 0);
        uint16_t y = style_object_position_encode(50, 0);
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(style));
        if (paint != NULL
            && (paint->components
                & STYLE_PAINT_COMPONENT_TRANSFORM_ORIGIN) != 0) {
            x = paint->transform_origin_x;
            y = paint->transform_origin_y;
        }
        const LayoutNodeBox *box = layout_box_for_node(js_rt_bridge_layout(bridge), node);
        if (box != NULL) {
            int used_x = box->width
                * style_object_position_percent(x) / 100
                + style_object_position_offset(x);
            int used_y = box->height
                * style_object_position_percent(y) / 100
                + style_object_position_offset(y);
            snprintf(value, value_size, "%dpx %dpx", used_x, used_y);
        } else {
            snprintf(value, value_size, "%d%% %d%%",
                     style_object_position_percent(x),
                     style_object_position_percent(y));
        }
        break;
    }
    case CSP_PERSPECTIVE: {
        snprintf(value, value_size, "%s",
                 style->has_perspective ? "1px" : "none");
        break;
    }
    case CSP_FILTER: {
        static const char *const filter_names[] = {
            "none", "grayscale(1)", "invert(1)", "sepia(1)",
            "brightness(1.25)", "brightness(0.75)",
            "contrast(1.25)", "saturate(1.25)"
        };
        unsigned filter = computed_style_filter_code(style);
        if (!style->has_filter
            || filter >= sizeof(filter_names) / sizeof(filter_names[0])) {
            filter = STYLE_FILTER_NONE;
        }
        const StylePaintStack *paint = stylesheet_paint_stack(
            bridge->stylesheet, computed_style_paint_stack_id(style));
        bool low = paint != NULL
            && (paint->reserved & STYLE_PAINT_FILTER_LOW_AMOUNT) != 0;
        if (low && filter == STYLE_FILTER_CONTRAST) {
            snprintf(value, value_size, "contrast(0.75)");
        } else if (low && filter == STYLE_FILTER_SATURATE) {
            snprintf(value, value_size, "saturate(0.75)");
        } else {
            snprintf(value, value_size, "%s", filter_names[filter]);
        }
        break;
    }
    case CSP_CONTAIN: {
        snprintf(value, value_size, "%s",
                 style->has_layout_containment ? "paint" : "none");
        break;
    }
    case CSP_CONTENT_VISIBILITY: {
        snprintf(value, value_size, "%s",
                 style->content_visibility == STYLE_CONTENT_VISIBILITY_HIDDEN
                    ? "hidden"
                    : (style->content_visibility
                           == STYLE_CONTENT_VISIBILITY_AUTO
                       ? "auto" : "visible"));
        break;
    }
    case CSP_WEBKIT_LINE_CLAMP: {
        unsigned clamp = computed_style_line_clamp(style);
        if (clamp == 0) snprintf(value, value_size, "none");
        else snprintf(value, value_size, "%u",
                      clamp);
        break;
    }
    case CSP_WILL_CHANGE: {
        snprintf(value, value_size, "%s",
                 style->will_change_transform ? "transform" : "auto");
        break;
    }
    case CSP_POINTER_EVENTS: {
        snprintf(value, value_size, "%s",
                 style->pointer_events_none ? "none" : "auto");
        break;
    }
    case CSP_FLOAT: {
        static const char *const float_names[] = {
            "none", "left", "right"
        };
        size_t mode = style->float_mode;
        if (mode >= sizeof(float_names) / sizeof(float_names[0])) mode = 0;
        snprintf(value, value_size, "%s", float_names[mode]);
        break;
    }
    case CSP_ASPECT_RATIO:
        if (style->aspect_width > 0 && style->aspect_height > 0) {
            snprintf(value, value_size, "%d / %d", style->aspect_width,
                     style->aspect_height);
            break;
        }
        goto serialize_default;
    case CSP_GRID_TEMPLATE_AREAS: {
        (void) stylesheet_serialize_grid_template_areas(
            bridge->stylesheet, style, value, value_size);
        break;
    }
    case CSP_GRID_TEMPLATE_COLUMNS: {
        const int *widths = NULL;
        unsigned origin = 0;
        size_t count = pseudo == PSEUDO_NONE
            && (style->display == DISPLAY_GRID
                || style->display == DISPLAY_INLINE_GRID)
            && layout_box_for_node(js_rt_bridge_layout(bridge), node) != NULL
            ? layout_grid_columns_for_node(js_rt_bridge_layout(bridge), node,
                                           &widths, &origin)
            : 0;
        if (count != 0) {
            (void) stylesheet_serialize_used_grid_columns(bridge->stylesheet,
                style, widths, (unsigned) count, origin, value, value_size);
        } else {
            (void) stylesheet_serialize_grid_template_tracks(
                bridge->stylesheet, style, false, value, value_size);
        }
        break;
    }
    case CSP_GRID_TEMPLATE_ROWS: {
        (void) stylesheet_serialize_grid_template_tracks(
            bridge->stylesheet, style, true, value, value_size);
        break;
    }
    case CSP_GAP:
    case CSP_COLUMN_GAP: {
        if (computed_style_gap_is_percent(style->gap)) {
            snprintf(value, value_size, "%d%%",
                     computed_style_gap_percent(style->gap));
        } else {
            snprintf(value, value_size, "%dpx", style->gap);
        }
        break;
    }
    case CSP_ROW_GAP: {
        snprintf(value, value_size, "%dpx", style->row_gap);
        break;
    }
    default:
    serialize_default:
        /* Reached only after every older case declined, so none of their
           paths grew. */
        (void) computed_style_serialize_retained(
            node, style, id, value, value_size);
        break;
    }
}

/* The value of `name` (dashed, lower-case) on the element `node` for
   __tilefinchComputedStyleGet and __tilefinchComputedStyleRead; argv[2],
   when argc > 2, names the pseudo-element. The caller owns `name`. */
static JSValue computed_style_value(JSContext *context, DomBridge *bridge,
                                    lxb_dom_node_t *node, const char *name,
                                    size_t name_length, int argc,
                                    JSValueConst *argv, bool *empty)
{
    *empty = false;
    ComputedStyleRead read;
    JSValue answer;
    if (!computed_style_resolve(context, bridge, node, name, name_length,
                                argc, argv, &read, &answer, empty))
        return answer;
    char value[640] = "";
    computed_style_serialize(&read, value, sizeof(value));
    return computed_style_string(context, value, empty);
}

/* HTML innerText for a connected, rendered element: the text as laid
   out, not the raw descendant text. display:none subtrees and elements that
   are never rendered (script, style, noscript while scripting runs,
   template, replaced elements' fallback) contribute nothing; block-level
   boxes separate lines, paragraphs a blank line, table rows a line and
   cells a tab; <br> is a line break; collapsible white space collapses
   across element boundaries and vanishes at line edges; visibility:hidden
   drops text but not visible descendants; text-transform applies (ASCII
   letters). Returns undefined when the receiver is not being rendered, for
   which script answers textContent as specified. Styles come from the
   bridge's computed-style cache (one resolution per element); the walk is
   iterative, bounded in nodes, depth and output bytes. */
#define RENDERED_TEXT_NODE_LIMIT 200000u
#define RENDERED_TEXT_BYTE_LIMIT (1024u * 1024u)
#define RENDERED_TEXT_DEPTH_LIMIT 256u

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
    bool truncated;
    bool at_start;
    bool pending_space;
    uint8_t pending_break;
} RenderedText;

typedef struct {
    lxb_dom_node_t *node;
    uint8_t white_space;
    uint8_t transform;
    bool visible;
    int8_t after;
} RenderedTextFrame;

static bool rendered_text_reserve(RenderedText *text, size_t extra)
{
    if (text->failed || text->truncated) return false;
    if (extra > RENDERED_TEXT_BYTE_LIMIT - text->length) {
        text->truncated = true;
        return false;
    }
    if (text->length + extra <= text->capacity) return true;
    size_t capacity = text->capacity == 0 ? 256u : text->capacity;
    while (capacity < text->length + extra) capacity *= 2u;
    if (capacity > RENDERED_TEXT_BYTE_LIMIT) {
        capacity = RENDERED_TEXT_BYTE_LIMIT;
    }
    char *grown = budget_realloc(text->budget, text->data, capacity);
    if (grown == NULL) {
        text->failed = true;
        return false;
    }
    text->data = grown;
    text->capacity = capacity;
    return true;
}

static void rendered_text_put(RenderedText *text, const char *bytes,
                              size_t length)
{
    if (length == 0 || !rendered_text_reserve(text, length)) return;
    memcpy(text->data + text->length, bytes, length);
    text->length += length;
}

/* Required line breaks collapse to their largest run and vanish at the
   start and end of the result. */
static void rendered_text_flush(RenderedText *text)
{
    if (text->pending_break == 0) return;
    if (text->length != 0) {
        for (uint8_t i = 0; i < text->pending_break; i++) {
            rendered_text_put(text, "\n", 1);
        }
    }
    text->pending_break = 0;
    text->at_start = true;
    text->pending_space = false;
}

static void rendered_text_break(RenderedText *text, uint8_t count)
{
    if (count > text->pending_break) text->pending_break = count;
    text->pending_space = false;
}

static void rendered_text_literal(RenderedText *text, const char *bytes,
                                  size_t length)
{
    rendered_text_flush(text);
    rendered_text_put(text, bytes, length);
    if (length != 0) text->at_start = bytes[length - 1] == '\n';
    text->pending_space = false;
}

static char rendered_text_case(char byte, uint8_t transform, bool word_start)
{
    if (transform == TEXT_TRANSFORM_UPPERCASE
        || (transform == TEXT_TRANSFORM_CAPITALIZE && word_start)) {
        return byte >= 'a' && byte <= 'z' ? (char) (byte - 32) : byte;
    }
    if (transform == TEXT_TRANSFORM_LOWERCASE) {
        return byte >= 'A' && byte <= 'Z' ? (char) (byte + 32) : byte;
    }
    return byte;
}

static bool rendered_text_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r'
           || byte == '\f';
}

static void rendered_text_append(RenderedText *text,
                                 const RenderedTextFrame *context,
                                 const char *data, size_t length)
{
    if (!context->visible || length == 0) return;
    uint8_t mode = context->white_space;
    bool preserve = mode == WHITE_SPACE_PRE || mode == WHITE_SPACE_PRE_WRAP
                    || mode == WHITE_SPACE_BREAK_SPACES;
    bool keep_lines = mode == WHITE_SPACE_PRE_LINE;
    if (preserve) {
        rendered_text_flush(text);
        if (text->pending_space && !text->at_start) {
            rendered_text_put(text, " ", 1);
        }
        text->pending_space = false;
    }
    bool word_start = text->at_start || text->pending_space;
    for (size_t i = 0; i < length && !text->failed && !text->truncated;
         i++) {
        char byte = data[i];
        if (byte == '\r') {
            if (i + 1 < length && data[i + 1] == '\n') continue;
            byte = '\n';
        }
        if (preserve) {
            char out = rendered_text_case(byte, context->transform,
                                          word_start);
            rendered_text_put(text, &out, 1);
            text->at_start = byte == '\n';
            word_start = rendered_text_space(byte);
            continue;
        }
        if (byte == '\n' && keep_lines) {
            rendered_text_literal(text, "\n", 1);
            word_start = true;
            continue;
        }
        if (rendered_text_space(byte)) {
            if (!text->at_start && text->pending_break == 0) {
                text->pending_space = true;
            }
            word_start = true;
            continue;
        }
        rendered_text_flush(text);
        if (text->pending_space && !text->at_start) {
            rendered_text_put(text, " ", 1);
        }
        char out = rendered_text_case(byte, context->transform, word_start);
        rendered_text_put(text, &out, 1);
        text->at_start = false;
        text->pending_space = false;
        word_start = false;
    }
}

static bool rendered_text_skipped(lxb_dom_node_t *node)
{
    static const char *const skipped[] = {
        "script", "style", "noscript", "template", "head", "title", "meta",
        "link", "base", "canvas", "video", "audio", "iframe", "object",
        "embed", "select", "datalist"
    };
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    if (name == NULL) return false;
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); i++) {
        if (strlen(skipped[i]) == length
            && strncasecmp(name, skipped[i], length) == 0) return true;
    }
    return false;
}

static bool rendered_text_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && strncasecmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *rendered_text_next_element(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node->next; at != NULL; at = at->next) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) return at;
    }
    return NULL;
}

static bool rendered_text_style(DomBridge *bridge, lxb_dom_node_t *node,
                                ComputedStyle *style)
{
    return bridge_computed_style(bridge, node, style, NULL, NULL);
}

JSValue js_dom_rendered_text(JSContext *context, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL || argc < 1) {
        return JS_UNDEFINED;
    }
    lxb_dom_node_t *root = js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (root == NULL || root->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !bridge_node_is_connected(root)) return JS_UNDEFINED;
    ComputedStyle style;
    /* Not being rendered: the receiver or an ancestor is undisplayed. */
    for (lxb_dom_node_t *at = root; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (!rendered_text_style(bridge, at, &style)) return JS_UNDEFINED;
        if (style.display == DISPLAY_NONE || style.hidden) {
            return JS_UNDEFINED;
        }
    }
    if (!rendered_text_style(bridge, root, &style)) return JS_UNDEFINED;
    RenderedText text = {.budget = bridge->budget, .at_start = true};
    RenderedTextFrame stack[RENDERED_TEXT_DEPTH_LIMIT];
    size_t depth = 0;
    stack[depth++] = (RenderedTextFrame) {
        .node = root, .white_space = style.white_space_mode,
        .transform = (uint8_t) style.text_transform,
        .visible = !style.visibility_hidden, .after = 0
    };
    lxb_dom_node_t *at = root->first_child;
    uint32_t visited = 0;
    while (depth != 0 && !text.failed && !text.truncated) {
        if (at == NULL) {
            /* Leave the innermost open element. */
            RenderedTextFrame *closing = &stack[--depth];
            if (closing->after < 0) rendered_text_literal(&text, "\t", 1);
            else if (closing->after > 0) {
                rendered_text_break(&text, (uint8_t) closing->after);
            }
            at = depth == 0 ? NULL : closing->node->next;
            continue;
        }
        if (++visited > RENDERED_TEXT_NODE_LIMIT) break;
        const RenderedTextFrame *parent = &stack[depth - 1];
        if (at->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *data = document_text_data(at, &length);
            if (data != NULL) {
                rendered_text_append(&text, parent, data, length);
            }
            at = at->next;
            continue;
        }
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT
            || rendered_text_skipped(at)) {
            at = at->next;
            continue;
        }
        if (!rendered_text_style(bridge, at, &style)) {
            text.failed = true;
            break;
        }
        if (style.display == DISPLAY_NONE || style.hidden
            || depth == RENDERED_TEXT_DEPTH_LIMIT) {
            at = at->next;
            continue;
        }
        if (rendered_text_name_is(at, "br")) {
            if (!style.visibility_hidden)
                rendered_text_literal(&text, "\n", 1);
            at = at->next;
            continue;
        }
        int8_t after = 0;
        if (rendered_text_name_is(at, "p")) {
            after = 2;
        } else if (style.display == DISPLAY_BLOCK
                   || style.display == DISPLAY_FLOW_ROOT
                   || style.display == DISPLAY_FLEX
                   || style.display == DISPLAY_GRID
                   || style.display == DISPLAY_TABLE) {
            after = 1;
        } else if (style.display == DISPLAY_TABLE_ROW) {
            if (rendered_text_next_element(at) != NULL) after = 1;
        } else if (style.display == DISPLAY_TABLE_CELL) {
            if (rendered_text_next_element(at) != NULL) after = -1;
        }
        if (after > 0 && style.display != DISPLAY_TABLE_ROW) {
            rendered_text_break(&text, (uint8_t) after);
        }
        stack[depth++] = (RenderedTextFrame) {
            .node = at, .white_space = style.white_space_mode,
            .transform = (uint8_t) style.text_transform,
            .visible = !style.visibility_hidden, .after = after
        };
        at = at->first_child;
    }
    if (text.failed) {
        budget_free(bridge->budget, text.data);
        return JS_ThrowOutOfMemory(context);
    }
    JSValue value = JS_NewStringLen(
        context, text.data == NULL ? "" : text.data, text.length);
    budget_free(bridge->budget, text.data);
    return value;
}

JSValue js_computed_style_get(JSContext *context,
                              JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    lxb_dom_node_t *node = argc > 0 && bridge != NULL
        ? js_rt_bridge_node_arg(context, bridge, argv[0]) : NULL;
    if (bridge == NULL || bridge->stylesheet == NULL || node == NULL
        || node->type != LXB_DOM_NODE_TYPE_ELEMENT || argc < 2) {
        return JS_NewString(context, "");
    }
    size_t name_length = 0;
    const char *name = JS_ToCStringLen(context, &name_length, argv[1]);
    if (name == NULL) return JS_EXCEPTION;
    bool empty = false;
    JSValue value = computed_style_value(context, bridge, node, name,
                                         name_length, argc, argv, &empty);
    JS_FreeCString(context, name);
    return value;
}

/* __tilefinchComputedStyleRead(handle, name, pseudo) -> the value, or
   undefined for the script's general path. One call for the common read
   (getComputedStyle(el).prop), which otherwise canonicalizes the name,
   asks whether the element is connected and reads the value in three.
   `name` is canonicalized exactly as __tilefinchCssName does (custom
   properties as they are, cssFloat, then each ASCII capital to "-" and its
   lower case). Undefined when the element is not a connected native
   element (script decides with its own tree), the name is not a string,
   the value needs script's post-processing (flex-basis and the scroll
   margins and paddings resolve em terms) or is empty for a non-custom
   name (script falls back to inline text). */
JSValue js_computed_style_read(JSContext *context, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    if (bridge == NULL || bridge->stylesheet == NULL || argc < 2
        || !JS_IsString(argv[1])) return JS_UNDEFINED;
    lxb_dom_node_t *node = js_rt_bridge_node_arg(context, bridge, argv[0]);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !bridge_node_is_connected(node)) return JS_UNDEFINED;
    size_t length = 0;
    const char *text = JS_ToCStringLen(context, &length, argv[1]);
    if (text == NULL) return JS_EXCEPTION;
    char canonical[128];
    size_t used = 0;
    bool usable = strlen(text) == length;
    if (!usable) {
        /* An embedded NUL: the general path decides. */
    } else if (length >= 2 && text[0] == '-' && text[1] == '-') {
        usable = length < sizeof(canonical);
        if (usable) memcpy(canonical, text, used = length);
    } else if (length == 8 && memcmp(text, "cssFloat", 8) == 0) {
        memcpy(canonical, "float", used = 5);
    } else {
        for (size_t i = 0; usable && i < length; i++) {
            unsigned char byte = (unsigned char) text[i];
            bool capital = byte >= 'A' && byte <= 'Z';
            if (used + (capital ? 2u : 1u) >= sizeof(canonical)) {
                usable = false;
                break;
            }
            if (capital) {
                canonical[used++] = '-';
                canonical[used++] = (char) (byte + 32u);
            } else {
                canonical[used++] = (char) byte;
            }
        }
    }
    JS_FreeCString(context, text);
    if (!usable) return JS_UNDEFINED;
    canonical[used] = '\0';
    bool custom = used >= 2 && canonical[0] == '-' && canonical[1] == '-';
    if (!custom
        && ((used == 10 && memcmp(canonical, "flex-basis", 10) == 0)
            || (used >= 13 && memcmp(canonical, "scroll-margin", 13) == 0)
            || (used >= 14
                && memcmp(canonical, "scroll-padding", 14) == 0)))
        return JS_UNDEFINED;
    bool empty = false;
    JSValue value = computed_style_value(context, bridge, node, canonical,
                                         used, argc, argv, &empty);
    if (empty && !custom && !JS_IsException(value)) {
        JS_FreeValue(context, value);
        return JS_UNDEFINED;
    }
    return value;
}
