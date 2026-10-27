#ifndef TILEFINCH_STYLE_MATCH_INTERNAL_H
#define TILEFINCH_STYLE_MATCH_INTERNAL_H

/* Selector-matching internals: the rule index and rule filters, the
   selector program and token blooms, StyleMatchSubject and the class-token
   cache, container queries, the focus marker and state attributes, and the
   rule-matching helpers the cascade, the retained matches and the focus
   probes share. Part of style_internal.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fnv1a.h"
#include "style_cache_internal.h"
#include "style_values_internal.h"

#define STYLE_SELECTOR_CHUNK_MIN_BYTES 1024u
#define STYLE_SELECTOR_CHUNK_MAX_BYTES (16u * 1024u)

bool stylesheet_extend_selector_program(
    Stylesheet *sheet, const uint16_t *prefix_offsets,
    size_t prefix_offset_count, size_t *reused_rules,
    size_t *compiled_rules);

#define STYLE_RULE_INDEX_EMPTY UINT32_MAX
#define STYLE_RULE_INDEX_MAX_SOURCES 34

struct StyleRuleIndexBucket {
    uint32_t hash;
    uint32_t representative;
    uint32_t first;
    uint32_t count;
    /* The bucket's rules that are not shadow-scoped (adopted only by
       shadow roots) come first, ascending; the scoped ones follow,
       ascending. An element outside every adopting carrier is planned on
       the first part alone. Equal to `count` when nothing is scoped. */
    uint32_t unscoped;
};

#define STYLE_TOKEN_BLOOM_WORDS 2u
#define STYLE_RELATIONAL_TOKEN_LIMIT 8u
/* Optional cold metadata. Exhaustion makes further rules opaque, never
   drops a dependency or changes selector matching. */
#define STYLE_RELATIONAL_POOL_TOKEN_LIMIT (1024u * 1024u)

struct StyleRuleFilter {
    struct StyleTokenBloom {
        uint32_t words[STYLE_TOKEN_BLOOM_WORDS];
    } compound, ancestors;
    /* Hashes of every class/id token outside the rightmost compound
       (including inside :not()/:is()), so a class or id change whose tokens
       miss this list cannot change which elements this rule matches other
       than the changed element itself. `relational_opaque` marks rules whose
       dependency on other elements' classes or ids cannot be listed (:has(),
       `of S` nth forms, escaped identifiers, [class]/[id] attribute
       selectors, more tokens than fit). */
    uint32_t relational_offset;
    uint8_t relational_count;
    bool relational_opaque;
    /* The rule can change display/visibility or supply an image (background,
       mask, generated content) or carries deferred declarations that might.
       Image discovery resolves an element's rules only when one of its
       candidate rules has this set. */
    bool discovery;
    /* STYLE_RULE_SVG_RASTER_* bits: the rule can change what an inline SVG
       raster resolves. INHERITED (colour, font size, var()) reaches the SVG
       from any element the rule matches; SIZE (width/height) only from the
       SVG itself, so only tokens outside the rightmost compound count. */
    uint8_t svg_raster : 2;
    /* One-based ancestor-mask bit (style_rule_ancestor_token_bit) of a
       class or ID some ancestor of every element this rule matches
       carries; zero for none. Set only for rules in the universal
       range. */
    uint8_t ancestor_token : 6;
};

/* Exact ancestor requirements of universal-range rules. The 64-bit token
   Bloom saturates on utility-CSS documents (a dozen classes per element,
   twenty ancestors deep), so rules such as Tailwind's `:is(.\*\*\:x *)`
   that every element is a candidate for were walked up every element's
   whole ancestor chain. A rule naming one of these tokens is rejected
   without matching when no ancestor carries it. */
#define STYLE_RULE_ANCESTOR_TOKEN_LIMIT 255u
#define STYLE_RULE_ANCESTOR_TOKEN_SLOTS 512u
/* Tokens share the 63 bits of an ancestor mask (and the six-bit filter
   field) by index: past 63 tokens two share a bit, which can only admit a
   candidate the matcher then rejects. */
#define STYLE_RULE_ANCESTOR_TOKEN_BITS 63u
struct StyleRuleAncestorTokens {
    uint32_t hashes[STYLE_RULE_ANCESTOR_TOKEN_LIMIT];
    /* Open-addressed by hash: one-based index into `hashes`, 0 empty. */
    uint8_t slots[STYLE_RULE_ANCESTOR_TOKEN_SLOTS];
    uint8_t count;
    /* Distinguishes tables in per-build ancestor caches. */
    uint32_t stamp;
};

/* The mask bit (one-based, as kept in StyleRuleFilter.ancestor_token) of
   a token's one-based table index. */
static inline unsigned style_rule_ancestor_token_bit(unsigned index)
{
    return index == 0 ? 0u : (index - 1u) % STYLE_RULE_ANCESTOR_TOKEN_BITS + 1u;
}

static inline unsigned style_rule_ancestor_token_find(
    const struct StyleRuleAncestorTokens *tokens, uint32_t hash)
{
    for (size_t probe = 0, slot = hash & (STYLE_RULE_ANCESTOR_TOKEN_SLOTS - 1u);
         probe < STYLE_RULE_ANCESTOR_TOKEN_SLOTS;
         probe++, slot = (slot + 1u) & (STYLE_RULE_ANCESTOR_TOKEN_SLOTS - 1u)) {
        unsigned index = tokens->slots[slot];
        if (index == 0) return 0;
        if (tokens->hashes[index - 1u] == hash) return index;
    }
    return 0;
}
#define STYLE_RULE_SVG_RASTER_INHERITED UINT8_C(1)
#define STYLE_RULE_SVG_RASTER_SIZE UINT8_C(2)
void stylesheet_note_style_source(Stylesheet *sheet,
                                  const lxb_dom_node_t *element);
/* Rules whose match on some element could change when the class/id tokens
   with the given hashes change on another element. Returns SIZE_MAX when the
   filters are unavailable or more than `capacity` rules qualify. */
/* Calls `visit` with the identity hash of each class and id the selector
   tests: every one (`every_position`), or only those it reads on elements
   other than its subject (before `split`, the rightmost compound, or in a
   functional argument), leaving out :has() arguments, which the :has()
   plan scopes. False when some identity cannot be named by a token (a
   [class]/[id] test, an escape that does not decode). With
   `attributes_opaque`, attribute selectors' names are visited the same way
   (stylesheet_identity_attribute_hash), and one this scan cannot name
   (escaped, namespaced) sets *attributes_opaque. */
bool style_selector_identity_tokens(const char *text, size_t length,
                                    size_t split, bool every_position,
                                    StyleIdentityTokenVisit visit,
                                    void *context, bool *attributes_opaque);
size_t stylesheet_rules_affected_by_tokens(
    const Stylesheet *sheet, const uint32_t *hashes, size_t hash_count,
    uint32_t *out, size_t capacity);

typedef struct {
    const uint32_t *entries;
    uint32_t at;
    uint32_t end;
} StyleRuleIndexSource;

typedef enum {
    STYLE_SELECTOR_END,
    STYLE_SELECTOR_TAG,
    STYLE_SELECTOR_TAG_ID,
    STYLE_SELECTOR_CLASS,
    STYLE_SELECTOR_ID,
    STYLE_SELECTOR_COMPOUND,
    STYLE_SELECTOR_PARENT,
    STYLE_SELECTOR_ANCESTOR,
    STYLE_SELECTOR_ADJACENT,
    STYLE_SELECTOR_GENERAL_SIBLING,
    /* Compiled attribute tests. PRESENT carries the lowercase attribute
       name. NAME loads that attribute's value for the single compare
       instruction that follows it, whose text is the wanted value with
       quotes removed; the compare opcodes are ordered like the string
       matcher's operators (exact, word, prefix, suffix, substring, dash)
       and are case-sensitive, as an unflagged attribute selector is. */
    STYLE_SELECTOR_ATTRIBUTE_PRESENT,
    STYLE_SELECTOR_ATTRIBUTE_NAME,
    STYLE_SELECTOR_ATTRIBUTE_EXACT,
    STYLE_SELECTOR_ATTRIBUTE_WORD,
    STYLE_SELECTOR_ATTRIBUTE_PREFIX,
    STYLE_SELECTOR_ATTRIBUTE_SUFFIX,
    STYLE_SELECTOR_ATTRIBUTE_SUBSTRING,
    STYLE_SELECTOR_ATTRIBUTE_DASH,
    /* An argument-less pseudo-class; text_offset holds its StylePseudoKind
       and text_length is zero. */
    STYLE_SELECTOR_PSEUDO,
    /* Complete standard :is() union of class-attribute prefix tests at
       start and after all five HTML spaces; text carries the prefix. */
    STYLE_SELECTOR_CLASS_TOKEN_PREFIX
} StyleSelectorOpcode;

/* Pseudo-class classification shared by the string matcher and the
   selector compiler. */
typedef enum {
    STYLE_PSEUDO_UNKNOWN,
    STYLE_PSEUDO_INTERACTIVE,
    STYLE_PSEUDO_FOCUS,
    STYLE_PSEUDO_FOCUS_WITHIN,
    STYLE_PSEUDO_FULLSCREEN,
    STYLE_PSEUDO_ROOT,
    STYLE_PSEUDO_SCOPE,
    STYLE_PSEUDO_DEFINED,
    STYLE_PSEUDO_FIRST,
    STYLE_PSEUDO_LAST,
    STYLE_PSEUDO_FIRST_TYPE,
    STYLE_PSEUDO_LAST_TYPE,
    STYLE_PSEUDO_ONLY_CHILD,
    STYLE_PSEUDO_ONLY_TYPE,
    STYLE_PSEUDO_EMPTY,
    STYLE_PSEUDO_NOT,
    STYLE_PSEUDO_IS,
    STYLE_PSEUDO_HAS,
    STYLE_PSEUDO_DISABLED,
    STYLE_PSEUDO_ENABLED,
    STYLE_PSEUDO_CHECKED,
    STYLE_PSEUDO_REQUIRED,
    STYLE_PSEUDO_OPTIONAL,
    STYLE_PSEUDO_LINK,
    STYLE_PSEUDO_OPEN,
    STYLE_PSEUDO_MODAL,
    STYLE_PSEUDO_POPOVER_OPEN,
    STYLE_PSEUDO_NTH_CHILD,
    STYLE_PSEUDO_NTH_TYPE,
    STYLE_PSEUDO_NTH_LAST_CHILD,
    STYLE_PSEUDO_NTH_LAST_TYPE,
    /* :host as the matcher sees it: an element hosting a shadow tree. Which
       tree's rules may use it is decided by style_shadow_scope_admits. */
    STYLE_PSEUDO_HOST,
} StylePseudoKind;

StylePseudoKind style_pseudo_kind(const char *text, size_t length);

#define STYLE_SELECTOR_PROGRAM_DEPTH_LIMIT 32u
#define STYLE_SELECTOR_IDENTIFIER_CAPACITY 192u

struct StyleSelectorInstruction {
    uint16_t text_offset;
    uint8_t text_length;
    uint8_t opcode;
};

_Static_assert(sizeof(StyleSelectorInstruction) == 4,
               "selector instructions must remain PSP-compact");

typedef struct StyleTokenBloom StyleTokenBloom;

static inline StyleTokenBloom style_token_bloom_empty(void)
{
    return (StyleTokenBloom) {{0, 0}};
}

static inline void style_token_bloom_merge(StyleTokenBloom *destination,
                                           StyleTokenBloom source)
{
    for (size_t i = 0; i < STYLE_TOKEN_BLOOM_WORDS; i++) {
        destination->words[i] |= source.words[i];
    }
}

static inline bool style_token_bloom_empty_value(StyleTokenBloom bloom)
{
    return (bloom.words[0] | bloom.words[1]) == 0;
}

static inline bool style_token_bloom_missing(StyleTokenBloom required,
                                             StyleTokenBloom available)
{
    for (size_t i = 0; i < STYLE_TOKEN_BLOOM_WORDS; i++) {
        if ((required.words[i] & ~available.words[i]) != 0) return true;
    }
    return false;
}

static inline uint32_t style_token_hash(
    StyleSelectorOpcode opcode, const char *text, size_t length)
{
    return style_fnv1a(STYLE_FNV1A_SEED ^ (uint32_t) opcode, text, length,
                       opcode == STYLE_SELECTOR_TAG);
}

static inline StyleTokenBloom style_compound_token_bloom(
    StyleSelectorOpcode opcode, const char *text, size_t length)
{
    uint32_t hash = style_token_hash(opcode, text, length);
    StyleTokenBloom bloom = style_token_bloom_empty();
    for (size_t word = 0; word < STYLE_TOKEN_BLOOM_WORDS; word++) {
        uint32_t mixed = hash + UINT32_C(0x9e3779b9) * (uint32_t) word;
        mixed ^= mixed >> 16;
        mixed *= UINT32_C(0x7feb352d);
        mixed ^= mixed >> 15;
        bloom.words[word] = UINT32_C(1) << (mixed & 31u);
    }
    return bloom;
}

/* End of the selector-list option starting at `start`: the next
   top-level comma outside quotes, parentheses and brackets, or `length`. */
size_t style_selector_list_option_end(const char *text, size_t length,
                                      size_t start);
/* One complex selector (no top-level commas): the offset of its rightmost
   compound (SIZE_MAX when not located) and the stylesheet fast key that
   compound requires (key_length 0 when none). The same analysis the
   stylesheet stores per rule, for selectors matched outside a rule. */
bool style_selector_prepare_complex(const char *text, size_t length,
                                    size_t *rightmost,
                                    SelectorType *key_type,
                                    size_t *key_offset, size_t *key_length);
bool style_selector_matches_profiled(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *selector,
                                     size_t selector_length);
bool style_selector_matches_prepared(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *selector,
                                     size_t selector_length,
                                     size_t rightmost_compound_offset);
bool style_rule_selector_matches(const Stylesheet *sheet,
                                 size_t rule_index,
                                 lxb_dom_node_t *node);
/* Returns false only for an allocation failure. Invalid or capacity-bounded
   queries are ignored with a zero output id. */
bool style_register_container_query(Stylesheet *sheet,
                                    const char *prelude, size_t length,
                                    uint8_t parent_query,
                                    uint8_t *query_id);
bool style_container_query_matches(const Stylesheet *sheet, uint8_t query,
                                   lxb_dom_node_t *node);
void style_relative_selector_cache_begin(Stylesheet *sheet);
void style_relative_selector_cache_end(Stylesheet *sheet);
bool stylesheet_prepare_focus_rule_index(Stylesheet *sheet);
bool stylesheet_prepare_has_rule_index(Stylesheet *sheet);
/* The sheet's classified :has() rules (style_has_invalidation.c). */
void style_has_plan_destroy(Stylesheet *sheet);
size_t style_has_plan_bytes(const Stylesheet *sheet);

typedef struct {
    lxb_dom_node_t *node;
    const char *tag;
    size_t tag_length;
    const char *id;
    size_t id_length;
    const char *classes;
    size_t classes_length;
    uintptr_t tag_id;
    /* The adopted sources whose carriers contain the element (see
       style_adopted_scope_admits), found on first need. */
    uint64_t adopted_scope;
    bool adopted_scope_known;
} StyleMatchSubject;

void style_match_subject_prepare(lxb_dom_node_t *node,
                                 StyleMatchSubject *subject);
bool style_subject_tag_matches(const StyleMatchSubject *subject,
                               const char *wanted, size_t length);
bool style_rule_selector_matches_subject(
    const Stylesheet *sheet, size_t rule_index, lxb_dom_node_t *node,
    const StyleMatchSubject *subject);

/* An element's class list split once into tokens: a rule-candidate check
   compares hashes instead of rescanning the whole attribute (utility-class
   pages carry dozens of classes and test hundreds of candidate rules per
   element). A set is found by the list's address and length; one scope
   (see style_class_tokens_scope_begin) trusts that, and a later scope
   first compares the kept copy of the list, so a set outlives the scope
   that built it without ever answering for different text. */
#define STYLE_CLASS_TOKEN_LIMIT 96u
/* chatgpt.com send -> answer, tokenizations: 8 sets 48,919, 12 sets
   21,366, 16 sets 16,284 (about 1.3 KB per set on the PSP). */
#define STYLE_CLASS_TOKEN_SETS 12u
#define STYLE_CLASS_TOKEN_COPY 512u
typedef struct {
    const char *source;
    size_t length;
    uint32_t scope;
    uint16_t count;
    bool usable;
    /* One bit per token from its length and end bytes: most class tests
       fail, and a clear bit answers without hashing the wanted name. */
    uint64_t quick;
    char copy[STYLE_CLASS_TOKEN_COPY];
    uint32_t hashes[STYLE_CLASS_TOKEN_LIMIT];
    uint16_t offsets[STYLE_CLASS_TOKEN_LIMIT];
    uint8_t lengths[STYLE_CLASS_TOKEN_LIMIT];
    /* Open addressing over the hashes: token index + 1, 0 empty. */
    uint8_t slots[128];
} StyleClassTokens;

/* Prepared subjects (tag, id and class lookups) of the elements a
   compiled descendant/child walk visits, which every candidate rule
   repeats for the same ancestors. Valid only inside the scope that holds
   the DOM immutable, like the class token sets. */
#define STYLE_SUBJECT_CACHE_SIZE 64u
typedef struct StyleClassTokenCache {
    StyleClassTokens sets[STYLE_CLASS_TOKEN_SETS];
    /* The current outermost scope; never zero once a scope has begun. */
    uint32_t scope;
    uint8_t next;
    uint8_t last;
    StyleMatchSubject subjects[STYLE_SUBJECT_CACHE_SIZE];
    uint32_t subject_scopes[STYLE_SUBJECT_CACHE_SIZE];
} StyleClassTokenCache;

/* Whether the subject's class list contains `key`. Within a class-token
   scope (one style resolution: the DOM cannot change there) the list is
   tokenized once and cached in the sheet's scratch; elsewhere, and for
   short lists, a plain scan. */
bool style_subject_has_class(const Stylesheet *sheet,
                             const StyleMatchSubject *subject,
                             const char *key, size_t key_length);
/* A rule's fast-reject key against a prepared subject: an ID, a class, or
   (any other type) a tag name. Style, custom and transition rules store
   the key differently but all test it here. */
static inline bool style_subject_key_matches(const Stylesheet *sheet,
    const StyleMatchSubject *subject, SelectorType type,
    const char *key, size_t length)
{
    if (type == SELECTOR_ID)
        return subject->id != NULL && subject->id_length == length
            && memcmp(subject->id, key, length) == 0;
    if (type == SELECTOR_CLASS)
        return style_subject_has_class(sheet, subject, key, length);
    return style_subject_tag_matches(subject, key, length);
}
/* Bracket a stretch in which the DOM does not change. The outermost begin
   forgets every cached class list: attribute storage can be reused once
   the DOM changes, so a pointer and length identify a list only inside. */
void style_class_tokens_scope_begin(const Stylesheet *sheet);
void style_class_tokens_scope_end(const Stylesheet *sheet);

#define STYLE_CONTAINER_QUERY_LIMIT 63u
#define STYLE_CONTAINER_NAME_LIMIT 32u
#define STYLE_CONTAINER_QUERY_TEXT_CAPACITY 128u
#define STYLE_CONTAINER_NAME_CAPACITY 48u
#define STYLE_CONTAINER_WALK_LIMIT 64u

typedef struct {
    char condition[STYLE_CONTAINER_QUERY_TEXT_CAPACITY];
    uint32_t name_bit;
    uint8_t parent;
    uint8_t condition_length;
    uint8_t needs_inline_size;
    uint8_t needs_block_size;
} StyleContainerQuery;

struct StyleConditionalQueries {
    StyleContainerQuery queries[STYLE_CONTAINER_QUERY_LIMIT];
    char names[STYLE_CONTAINER_NAME_LIMIT][STYLE_CONTAINER_NAME_CAPACITY];
    uint8_t query_count;
    uint8_t name_count;
};

typedef struct StyleContainerState {
    lxb_dom_node_t *node;
    uint32_t name_bits;
    int content_width;
    int content_height;
    /* Container conditions answered against this geometry, by query id
       (bit id - 1): a condition depends only on the selected container's
       size and the sheet's fixed viewport, so each is evaluated once per
       stored state, not once per element. A store that changes the state
       starts both masks over. */
    uint64_t conditions_evaluated;
    uint64_t conditions_matched;
    uint8_t type;
    bool occupied;
} StyleContainerState;
_Static_assert(STYLE_CONTAINER_QUERY_LIMIT <= 64u,
               "query ids must fit the per-container condition masks");

typedef struct StyleContainerLogEntry {
    lxb_dom_node_t *node;
    uint8_t query;
    bool matched;
} StyleContainerLogEntry;

typedef struct StyleContainerLog {
    StyleContainerLogEntry *entries;
    size_t count;
    size_t capacity;
    const void *owner;
    bool active;
    bool overflow;
} StyleContainerLog;

typedef struct StyleContainerMatchCacheEntry {
    lxb_dom_node_t *node;
    uint8_t query;
    bool matched;
    bool occupied;
} StyleContainerMatchCacheEntry;

enum {
    STYLE_CONTAINER_TYPE_NONE,
    STYLE_CONTAINER_TYPE_INLINE_SIZE,
    STYLE_CONTAINER_TYPE_SIZE
};

/* The native attribute that marks the focused element for :focus and
   :focus-within. Focus probes never write it: they set the matcher's
   focus override below. */
#define STYLE_FOCUS_MARKER "data-tilefinch-focus"

/* A focus probe's answer to "is `node` focused?": while `node` is set, the
   matcher reads `focused` for that element's focus marker instead of the
   attribute, everywhere it reads the marker (:focus, :focus-within, and
   attribute selectors that name it, where a focused override reads as the
   marker with an empty value). Every other element reads its attribute. A
   probe swaps its override in and restores the previous one when done. */
typedef struct {
    const lxb_dom_node_t *node;
    bool focused;
} StyleFocusOverride;
StyleFocusOverride style_focus_override_swap(StyleFocusOverride next);
/* Whether `node` carries the focus marker, as the matcher sees it. */
bool style_node_has_focus_marker(lxb_dom_node_t *node);

/* Every attribute a supported state pseudo-class reads (simple_pseudo_matches
   in style_match.c), paired with text that finds the pseudo-class in a
   selector (":focus" also finds :focus-within and :focus-visible). An
   attribute write can change such a selector's answer only through these
   pairs or an [attribute] selector. */
typedef struct {
    const char *attribute;
    const char *pseudo;
} StylePseudoAttribute;
extern const StylePseudoAttribute style_pseudo_attributes[];
extern const size_t style_pseudo_attribute_count;

/* Ordinary selectors fit ten offset bits; the 63 one-based @container ids
   fit the remaining six. Keep this packed field and the PSP rule size. */
#define STYLE_RULE_COMPOUND_OFFSET_MASK UINT16_C(0x03ff)
#define STYLE_RULE_CONTAINER_QUERY_SHIFT 10u
_Static_assert(STYLE_SELECTOR_TEXT_CAPACITY
                   <= STYLE_RULE_COMPOUND_OFFSET_MASK + 1u
               && STYLE_CONTAINER_QUERY_LIMIT
                   <= (UINT16_MAX >> STYLE_RULE_CONTAINER_QUERY_SHIFT),
               "selector offsets and container ids must fit the rule field");

static inline uint8_t style_rule_container_query(const StyleRule *rule)
{
    return rule == NULL ? 0 : (uint8_t) (
        rule->rightmost_compound_offset >> STYLE_RULE_CONTAINER_QUERY_SHIFT);
}

static inline size_t style_rule_rightmost_compound(const StyleRule *rule)
{
    return rule == NULL ? 0
        : rule->rightmost_compound_offset & STYLE_RULE_COMPOUND_OFFSET_MASK;
}

static inline void style_rule_set_container_query(StyleRule *rule,
                                                  uint8_t query)
{
    if (rule == NULL) return;
    rule->rightmost_compound_offset = (uint16_t) (
        (rule->rightmost_compound_offset & STYLE_RULE_COMPOUND_OFFSET_MASK)
        | ((uint16_t) query << STYLE_RULE_CONTAINER_QUERY_SHIFT));
}

/* Finds the last top-level combinator of a complex selector: outside []
   and (), ignoring backslash-escaped spaces. On success *split indexes the
   combinator character (for a descendant combinator, the whitespace before
   the rightmost compound) and *combinator is '>', '+', '~' or ' '. Returns
   false, leaving both untouched, when text is a single compound. Inline:
   selector matching splits on every non-prepared match. */
static inline bool style_selector_last_combinator(
    const char *text, size_t length, size_t *split, char *combinator)
{
    int square = 0;
    int round = 0;
    for (size_t i = length; i != 0; i--) {
        char value = text[i - 1];
        if (value == ']') square++;
        else if (value == '[' && square > 0) square--;
        else if (value == ')') round++;
        else if (value == '(' && round > 0) round--;
        else if (square == 0 && round == 0
                 && (value == '>' || value == '+' || value == '~')) {
            *split = i - 1;
            *combinator = value;
            return true;
        } else if (square == 0 && round == 0
                   && isspace((unsigned char) value)) {
            size_t escaped = i - 1;
            size_t backslashes = 0;
            while (escaped != 0 && text[escaped - 1] == '\\') {
                escaped--;
                backslashes++;
            }
            if ((backslashes & 1u) != 0) continue;
            size_t right = i;
            while (right < length
                   && isspace((unsigned char) text[right])) right++;
            if (right < length) {
                size_t left = i - 1;
                while (left != 0
                       && isspace((unsigned char) text[left - 1])) left--;
                if (left != 0 && (text[left - 1] == '>'
                                  || text[left - 1] == '+'
                                  || text[left - 1] == '~')) {
                    *split = left - 1;
                    *combinator = text[left - 1];
                } else {
                    *split = i - 1;
                    *combinator = ' ';
                }
                return true;
            }
        }
    }
    return false;
}

/* Where the rightmost compound of a complex selector begins. */
static inline size_t style_selector_rightmost_compound_start(
    const char *text, size_t length)
{
    size_t split = 0;
    char combinator = 0;
    size_t start = style_selector_last_combinator(
        text, length, &split, &combinator) ? split + 1 : 0;
    while (start < length && isspace((unsigned char) text[start])) start++;
    return start;
}

/* style_match.c */
bool style_tag_is(lxb_dom_node_t *node, const char *wanted);
bool option_is_displayed_by_default(lxb_dom_node_t *option);
bool class_contains_length(const char *classes, size_t length,
                           const char *wanted, size_t wanted_length);

/* selector scanning shared with the sheet builder */
size_t skip_selector_identifier(const char *text, size_t length, size_t at);
/* The compound matcher's own identifier decoding: `text` itself when it has
   no escape, else its CSS-unescaped form in `scratch`; NULL when the matcher
   could never compare it (too long for `capacity`, or malformed). */
const char *style_selector_identifier_span(const char *text, size_t length,
                                           char *scratch, size_t capacity,
                                           size_t *decoded_length);

/* style_sheet.c: rule-index access shared with the resolver */
const char *style_rule_fast_key(const StyleRule *rule);
StyleRuleIndexBucket *style_rule_find_bucket(const Stylesheet *sheet,
                                             SelectorType type,
                                             const char *text, size_t length,
                                             bool create, PseudoElement pseudo);
void stylesheet_prepare_rule_index(Stylesheet *sheet);
/* Frees the rule index (and the custom-property index); both rebuild
   lazily. */
void stylesheet_drop_rule_index(Stylesheet *sheet);
void stylesheet_drop_custom_rule_index(Stylesheet *sheet);

/* style_selector_program.c */
void stylesheet_drop_selector_program(Stylesheet *sheet);
void stylesheet_prepare_selector_program(Stylesheet *sheet);

/* Rule matching shared by the cascade (style_resolve.c), the retained
   matched-rule table (style_retained_matches.c) and the focus probes
   (style_focus.c). */
static inline StyleMatchSubject style_match_subject(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    (void) sheet;
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    return subject;
}

static inline bool rule_fast_matches(const Stylesheet *sheet,
                                     const StyleRule *rule,
                                     const StyleMatchSubject *subject)
{
    if (!rule->has_fast_key) return true;
    const char *fast_key = style_rule_fast_key(rule);
    if (fast_key == NULL) return true;
    return style_subject_key_matches(sheet, subject, rule->type, fast_key,
                                     rule->fast_key_length);
}

static inline bool rule_matches(const Stylesheet *sheet, size_t rule_index,
                                const StyleRule *rule, lxb_dom_node_t *node,
                                const StyleMatchSubject *subject)
{
    return rule_fast_matches(sheet, rule, subject)
           && style_container_query_matches(
               sheet, style_rule_container_query(rule), node)
           && style_rule_selector_matches_subject(
               sheet, rule_index, node, subject);
}

#endif /* TILEFINCH_STYLE_MATCH_INTERNAL_H */
