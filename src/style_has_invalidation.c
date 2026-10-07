/* Scoped invalidation for :has() (see style_cache_internal.h).

   Each :has() occurrence of a rule is classified once per rule set from
   the selector text: where its anchor lies relative to the rule's subject
   (the subject itself, one of its ancestors, or elsewhere), whether its
   argument reads descendants or later siblings, whether the argument can
   read state outside that region, and which keys the anchor and the
   subject must carry. A change then walks from the changed node to the
   anchors whose answer it can move and drops only the cached state of the
   elements those answers style. */

#include "tilefinch/style.h"
#include "style_internal.h"
#include "style_cache_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    uint32_t rule;
    /* Keys the rule's subject and this :has()'s anchor must carry (a class,
       id or tag hash; 0 when none is known). A self anchor is keyed by
       either, so it holds the anchor key when it has one. */
    uint32_t subject_key;
    uint32_t anchor_key;
    /* Identity hashes (stylesheet_identity_token_hash) of the classes and
       ids the argument mentions: a class/id change elsewhere cannot move
       this answer. */
    uint32_t token_first;
    uint16_t token_count;
    uint8_t relation;
    uint8_t region;
    bool escapes;
    /* An identity the tokens cannot represent, or a [class]/[id] test. */
    bool any_identity;
    /* An argument element no key can find (`> *`, `:not(.a)`): any element
       insertion or removal, or any text change, may move the answer. */
    bool tree_keyless;
    bool text_keyless;
    /* A :focus-within: the anchor's answer is :has() of the focus marker
       (or the marker on the anchor itself), so only a focus move, or a
       tree change carrying the marker, can move it. */
    bool focus;
    /* The declared name of a custom-property rule (name bits), else 0. */
    uint64_t custom_names;
} StyleHasPlanEntry;

enum { HAS_ROLE_ANCHOR = 1, HAS_ROLE_SUBJECT = 2 };
/* Argument features (keys of the elements an argument's relative selectors
   end on): searched in an inserted or removed subtree (narrow), among its
   siblings when the argument tests positions or adjacency (sibling: a new
   or removed sibling cannot otherwise move whether an element matches),
   in the whole parent subtree when a position or adjacency change reaches
   descendants (wide), and on the parent whose :empty state a change can
   flip (empty). */
enum {
    HAS_FEATURE_NARROW = 1,
    HAS_FEATURE_WIDE = 2,
    HAS_FEATURE_EMPTY = 4,
    HAS_FEATURE_SIBLING = 8
};

typedef struct {
    uint32_t hash;
    uint16_t entry;
    uint8_t role;
} StyleHasPlanKey;

struct StyleHasPlan {
    Budget *budget;
    StyleHasPlanEntry *entries;
    size_t entry_count, entry_capacity;
    uint32_t *tokens;
    size_t token_count, token_capacity;
    /* Sorted by hash. */
    StyleHasPlanKey *keys;
    size_t key_count;
    uint16_t *unkeyed_anchors;
    size_t unkeyed_anchor_count;
    uint16_t *unkeyed_subjects;
    size_t unkeyed_subject_count;
    /* Sorted by hash; `role` holds HAS_FEATURE_* bits. */
    StyleHasPlanKey *features;
    size_t feature_count, feature_capacity;
    bool wide_features;
    /* Some key is an attribute name (a compound with no id, class or tag),
       so elements offer their attribute names as keys too. */
    bool attribute_keys;
    /* More occurrences than STYLE_HAS_PLAN_LIMIT, or an allocation failed:
       every change resets. */
    bool bounded;
    /* While analyzing a custom-property rule: its name bits. */
    uint64_t analyzing_custom_names;
    /* Distinguishes this build from any other (entry indices differ). */
    uint32_t serial;
};

#define HAS_KEY_TAG 't'
#define HAS_KEY_ATTRIBUTE 'a'
#define HAS_IDENTIFIER_CAPACITY 128u
#define HAS_COMPOUND_LIMIT 32u
#define HAS_NESTING_LIMIT 8u
/* Ancestors walked per change, and earlier/other siblings visited. Past
   the sibling bound, sibling-region anchors are found by key instead. */
#define HAS_DEPTH_LIMIT 1024u
#define HAS_SIBLING_LIMIT 256u

/* ---- Selector text. --------------------------------------------------- */

static bool has_ident_byte(unsigned char value)
{
    return value >= 0x80 || isalnum(value) || value == '_' || value == '-';
}

static size_t has_skip_string(const char *text, size_t at, size_t end)
{
    char quote = text[at++];
    while (at < end) {
        if (text[at] == '\\') at += 2;
        else if (text[at++] == quote) return at;
    }
    return end;
}

/* `at` is on '(' or '['; returns the index after the matching close. */
static size_t has_skip_block(const char *text, size_t at, size_t end)
{
    unsigned depth = 0;
    while (at < end) {
        char value = text[at];
        if (value == '"' || value == '\'') {
            at = has_skip_string(text, at, end);
            continue;
        }
        if (value == '\\') {
            at += 2;
            continue;
        }
        if (value == '(' || value == '[') {
            depth++;
        } else if ((value == ')' || value == ']') && depth != 0
                   && --depth == 0) {
            return at + 1;
        }
        at++;
    }
    return end;
}

static uint32_t has_key_hash(unsigned char type, const unsigned char *text,
                             size_t length)
{
    /* ASCII case folded, so a tag, or a class in a quirks-mode document,
       never misses; a collision only drops more. */
    uint32_t hash = (UINT32_C(2166136261) ^ type) * UINT32_C(16777619);
    for (size_t i = 0; i < length; i++) {
        unsigned char value = text[i];
        if (value >= 'A' && value <= 'Z') value = (unsigned char) (value + 32);
        hash = (hash ^ value) * UINT32_C(16777619);
    }
    return hash == 0 ? 1u : hash;
}

/* Decodes the identifier at `at` (CSS escapes resolved) into `out`. Returns
   the index after it; *length is SIZE_MAX when it does not fit or is
   malformed. */
static size_t has_read_identifier(const char *text, size_t at, size_t end,
                                  unsigned char *out, size_t *length)
{
    size_t used = 0;
    bool valid = true;
    while (at < end) {
        unsigned char value = (unsigned char) text[at];
        unsigned char decoded[4];
        size_t decoded_length = 1;
        if (value == '\\') {
            if (++at >= end || text[at] == '\n' || text[at] == '\r') {
                valid = false;
                break;
            }
            if (isxdigit((unsigned char) text[at])) {
                uint32_t codepoint = 0;
                for (size_t digits = 0; digits < 6 && at < end
                     && isxdigit((unsigned char) text[at]); digits++, at++) {
                    unsigned char digit = (unsigned char) text[at];
                    codepoint = codepoint * 16u + (isdigit(digit)
                        ? (uint32_t) (digit - '0')
                        : (uint32_t) (tolower(digit) - 'a' + 10));
                }
                if (at < end && isspace((unsigned char) text[at])) at++;
                if (codepoint == 0 || codepoint > 0x10ffffu) {
                    valid = false;
                    break;
                }
                if (codepoint <= 0x7fu) {
                    decoded[0] = (unsigned char) codepoint;
                } else if (codepoint <= 0x7ffu) {
                    decoded[0] = (unsigned char) (0xc0u | (codepoint >> 6));
                    decoded[1] = (unsigned char) (0x80u | (codepoint & 63u));
                    decoded_length = 2;
                } else if (codepoint <= 0xffffu) {
                    decoded[0] = (unsigned char) (0xe0u | (codepoint >> 12));
                    decoded[1] = (unsigned char) (0x80u
                                                  | ((codepoint >> 6) & 63u));
                    decoded[2] = (unsigned char) (0x80u | (codepoint & 63u));
                    decoded_length = 3;
                } else {
                    decoded[0] = (unsigned char) (0xf0u | (codepoint >> 18));
                    decoded[1] = (unsigned char) (0x80u
                                                  | ((codepoint >> 12) & 63u));
                    decoded[2] = (unsigned char) (0x80u
                                                  | ((codepoint >> 6) & 63u));
                    decoded[3] = (unsigned char) (0x80u | (codepoint & 63u));
                    decoded_length = 4;
                }
            } else {
                decoded[0] = (unsigned char) text[at++];
            }
        } else if (has_ident_byte(value)) {
            decoded[0] = value;
            at++;
        } else {
            break;
        }
        for (size_t i = 0; i < decoded_length; i++) {
            if (used < HAS_IDENTIFIER_CAPACITY) out[used] = decoded[i];
            used++;
        }
    }
    *length = valid && used <= HAS_IDENTIFIER_CAPACITY ? used : SIZE_MAX;
    return at;
}

static bool has_name_is(const char *text, size_t begin, size_t end,
                        const char *name)
{
    size_t length = strlen(name);
    return end - begin == length
        && strncasecmp(text + begin, name, length) == 0;
}

static bool has_text_contains(const char *text, size_t begin, size_t end,
                              const char *needle)
{
    size_t length = strlen(needle);
    for (size_t at = begin; at + length <= end; at++) {
        if (strncasecmp(text + at, needle, length) == 0) return true;
    }
    return false;
}

/* End of the selector-list item starting at `at`. */
static size_t has_item_end(const char *text, size_t at, size_t end)
{
    while (at < end) {
        char value = text[at];
        if (value == '"' || value == '\'') {
            at = has_skip_string(text, at, end);
        } else if (value == '\\') {
            at += 2;
        } else if (value == '(' || value == '[') {
            at = has_skip_block(text, at, end);
        } else if (value == ',') {
            return at;
        } else {
            at++;
        }
    }
    return end;
}

typedef struct {
    size_t begin, end;
    /* The combinator before this compound: 0 for none, ' ', '>', '+', '~'. */
    char combinator;
} HasCompound;

/* Splits one complex (or relative) selector into its compounds. */
static size_t has_compounds(const char *text, size_t at, size_t end,
                            HasCompound *out, size_t capacity, bool *overflow)
{
    size_t count = 0;
    char pending = 0;
    *overflow = false;
    while (at < end) {
        char value = text[at];
        if (isspace((unsigned char) value)) {
            at++;
            continue;
        }
        if (value == '>' || value == '+' || value == '~') {
            pending = value;
            at++;
            continue;
        }
        size_t start = at;
        while (at < end) {
            value = text[at];
            if (value == '"' || value == '\'') {
                at = has_skip_string(text, at, end);
            } else if (value == '\\') {
                at = at + 2 < end ? at + 2 : end;
            } else if (value == '(' || value == '[') {
                at = has_skip_block(text, at, end);
            } else if (isspace((unsigned char) value) || value == '>'
                       || value == '+' || value == '~' || value == ',') {
                break;
            } else {
                at++;
            }
        }
        if (at == start) {
            at++;
            continue;
        }
        if (count == capacity) {
            *overflow = true;
            return count;
        }
        out[count].begin = start;
        out[count].end = at;
        out[count].combinator = pending != 0 ? pending
                                : (count == 0 ? 0 : ' ');
        count++;
        pending = 0;
    }
    return count;
}

typedef struct {
    size_t name_begin, name_end, argument_begin, argument_end;
    bool functional;
} HasPseudo;

/* The next pseudo-class or pseudo-element in [*cursor, end), skipping
   attribute selectors and strings. A functional argument is not entered. */
static bool has_next_pseudo(const char *text, size_t *cursor, size_t end,
                            HasPseudo *pseudo)
{
    size_t at = *cursor;
    while (at < end) {
        char value = text[at];
        if (value == '"' || value == '\'') {
            at = has_skip_string(text, at, end);
            continue;
        }
        if (value == '\\') {
            at += 2;
            continue;
        }
        if (value == '[' || value == '(') {
            at = has_skip_block(text, at, end);
            continue;
        }
        if (value != ':') {
            at++;
            continue;
        }
        at++;
        if (at < end && text[at] == ':') at++;
        pseudo->name_begin = at;
        while (at < end && has_ident_byte((unsigned char) text[at])) at++;
        pseudo->name_end = at;
        pseudo->functional = at < end && text[at] == '(';
        if (pseudo->functional) {
            size_t close = has_skip_block(text, at, end);
            pseudo->argument_begin = at + 1;
            pseudo->argument_end = close > at + 1 && text[close - 1] == ')'
                ? close - 1 : close;
            at = close;
        } else {
            pseudo->argument_begin = pseudo->argument_end = at;
        }
        *cursor = at > end ? end : at;
        return true;
    }
    *cursor = end;
    return false;
}

static bool has_pseudo_is_logical(const char *text, const HasPseudo *pseudo)
{
    size_t b = pseudo->name_begin, e = pseudo->name_end;
    return has_name_is(text, b, e, "is") || has_name_is(text, b, e, "where")
        || has_name_is(text, b, e, "not")
        || has_name_is(text, b, e, "matches")
        || has_name_is(text, b, e, "any")
        || has_name_is(text, b, e, "-webkit-any")
        || has_name_is(text, b, e, "-moz-any");
}

/* The class, id or tag every element matching the compound carries (an
   id preferred, then a class): 0 when none is written. */
static uint32_t has_compound_key(const char *text, size_t at, size_t end,
                                 bool *attribute_key)
{
    unsigned char identifier[HAS_IDENTIFIER_CAPACITY];
    size_t length = 0;
    uint32_t id = 0, class_key = 0, tag = 0, attribute = 0;
    if (at < end && (has_ident_byte((unsigned char) text[at])
                     || text[at] == '\\')) {
        size_t after = has_read_identifier(text, at, end, identifier,
                                           &length);
        if (after < end && text[after] == '|') {
            at = after + 1;
        } else {
            if (length != SIZE_MAX && length != 0)
                tag = has_key_hash(HAS_KEY_TAG, identifier, length);
            at = after;
        }
    }
    while (at < end) {
        char value = text[at];
        if (value == '.' || value == '#') {
            size_t after = has_read_identifier(text, at + 1, end, identifier,
                                               &length);
            if (length != SIZE_MAX && length != 0) {
                uint32_t hash = has_key_hash((unsigned char) value,
                                             identifier, length);
                if (value == '#' && id == 0) id = hash;
                if (value == '.' && class_key == 0) class_key = hash;
            }
            at = after > at + 1 ? after : at + 1;
        } else if (value == '[') {
            /* [name...] (not [ns|name]): the element carries the name. */
            size_t name = at + 1;
            while (name < end && isspace((unsigned char) text[name])) name++;
            size_t after = has_read_identifier(text, name, end, identifier,
                                               &length);
            if (attribute == 0 && length != SIZE_MAX && length != 0
                && (after >= end || text[after] != '|'))
                attribute = has_key_hash(HAS_KEY_ATTRIBUTE, identifier,
                                         length);
            at = has_skip_block(text, at, end);
        } else if (value == '(') {
            at = has_skip_block(text, at, end);
        } else if (value == '"' || value == '\'') {
            at = has_skip_string(text, at, end);
        } else if (value == ':') {
            at++;
            while (at < end && (text[at] == ':'
                                || has_ident_byte((unsigned char) text[at])))
                at++;
        } else if (value == '\\') {
            at += 2;
        } else {
            at++;
        }
    }
    if (id != 0) return id;
    if (class_key != 0) return class_key;
    if (tag != 0) return tag;
    if (attribute != 0) *attribute_key = true;
    return attribute;
}

/* Descendant and/or sibling region of a relative selector list. */
static uint8_t has_argument_region(const char *text, size_t at, size_t end)
{
    uint8_t region = 0;
    while (at < end) {
        size_t item_end = has_item_end(text, at, end);
        size_t first = at;
        while (first < item_end && isspace((unsigned char) text[first]))
            first++;
        if (first < item_end) {
            region |= text[first] == '+' || text[first] == '~'
                ? STYLE_HAS_REGION_SIBLINGS : STYLE_HAS_REGION_DESCENDANTS;
        }
        at = item_end + 1;
    }
    return region == 0 ? STYLE_HAS_REGION_DESCENDANTS : region;
}

/* Whether any item of a selector list has a combinator (reads another
   element than the one it is matched against). */
static bool has_list_combines(const char *text, size_t at, size_t end)
{
    HasCompound compounds[HAS_COMPOUND_LIMIT];
    while (at < end) {
        size_t item_end = has_item_end(text, at, end);
        bool overflow = false;
        size_t count = has_compounds(text, at, item_end, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
        if (overflow || count > 1
            || (count == 1 && compounds[0].combinator != 0)) return true;
        at = item_end + 1;
    }
    return false;
}

/* Start of the `S` in an An+B "of S" argument, or `end`. */
static size_t has_nth_of(const char *text, size_t at, size_t end)
{
    for (; at + 3 < end; at++) {
        if (text[at] == '(' || text[at] == '[') {
            at = has_skip_block(text, at, end) - 1;
            continue;
        }
        if (isspace((unsigned char) text[at])
            && (text[at + 1] == 'o' || text[at + 1] == 'O')
            && (text[at + 2] == 'f' || text[at + 2] == 'F')
            && isspace((unsigned char) text[at + 3])) return at + 4;
    }
    return end;
}

/* Whether a :has() argument can read an element outside its anchor's
   region: an ancestor or sibling combinator under :is()/:not() (it can
   climb above the anchor), :disabled/:enabled (a disabled fieldset
   ancestor), `of S` among siblings (every earlier sibling's state), or a
   functional pseudo-class this classifier does not know. */
static bool has_argument_escapes(const char *text, size_t at, size_t end,
                                 bool siblings, unsigned depth)
{
    if (depth > HAS_NESTING_LIMIT) return true;
    HasPseudo pseudo;
    size_t cursor = at;
    while (has_next_pseudo(text, &cursor, end, &pseudo)) {
        size_t b = pseudo.name_begin, e = pseudo.name_end;
        if (!pseudo.functional) {
            if (has_name_is(text, b, e, "disabled")
                || has_name_is(text, b, e, "enabled")) return true;
            continue;
        }
        size_t ab = pseudo.argument_begin, ae = pseudo.argument_end;
        if (has_name_is(text, b, e, "has")) {
            bool nested_siblings = (has_argument_region(text, ab, ae)
                                    & STYLE_HAS_REGION_SIBLINGS) != 0;
            if (has_argument_escapes(text, ab, ae, nested_siblings,
                                     depth + 1)) return true;
        } else if (has_pseudo_is_logical(text, &pseudo)) {
            if (has_list_combines(text, ab, ae)
                || has_argument_escapes(text, ab, ae, siblings, depth + 1))
                return true;
        } else if (e - b > 4 && strncasecmp(text + b, "nth-", 4) == 0) {
            size_t of = has_nth_of(text, ab, ae);
            if (of < ae && (siblings || has_list_combines(text, of, ae)
                            || has_argument_escapes(text, of, ae, siblings,
                                                    depth + 1)))
                return true;
        } else {
            return true;
        }
    }
    return false;
}

/* ---- Classification. -------------------------------------------------- */

typedef struct {
    StyleHasPlan *plan;
    const char *text;
    uint32_t rule;
    uint32_t subject_key;
    bool other;
} HasAnalysis;

static bool has_plan_reserve(Budget *budget, void **items, size_t *capacity,
                             size_t count, size_t size)
{
    if (count < *capacity) return true;
    size_t grown = *capacity == 0 ? 16u : *capacity * 2u;
    void *resized = budget_realloc(budget, *items, grown * size);
    if (resized == NULL) return false;
    *items = resized;
    *capacity = grown;
    return true;
}

static void has_collect_tokens(HasAnalysis *analysis,
                               StyleHasPlanEntry *entry, size_t at,
                               size_t end)
{
    StyleHasPlan *plan = analysis->plan;
    const char *text = analysis->text;
    unsigned char identifier[HAS_IDENTIFIER_CAPACITY];
    entry->token_first = (uint32_t) plan->token_count;
    while (at < end) {
        char value = text[at];
        if (value == '"' || value == '\'') {
            at = has_skip_string(text, at, end);
            continue;
        }
        if (value == '[') {
            size_t name = at + 1, length = 0;
            while (name < end && isspace((unsigned char) text[name])) name++;
            (void) has_read_identifier(text, name, end, identifier, &length);
            if (length == SIZE_MAX
                || (length == 5 && strncasecmp((const char *) identifier,
                                               "class", 5) == 0)
                || (length == 2 && strncasecmp((const char *) identifier,
                                               "id", 2) == 0))
                entry->any_identity = true;
            at = has_skip_block(text, at, end);
            continue;
        }
        if (value == '\\') {
            at += 2;
            continue;
        }
        if (value != '.' && value != '#') {
            at++;
            continue;
        }
        size_t length = 0;
        size_t after = has_read_identifier(text, at + 1, end, identifier,
                                           &length);
        if (length == SIZE_MAX) {
            entry->any_identity = true;
        } else if (length != 0) {
            if (entry->token_count == UINT16_MAX
                || !has_plan_reserve(plan->budget, (void **) &plan->tokens,
                                     &plan->token_capacity, plan->token_count,
                                     sizeof(*plan->tokens))) {
                entry->any_identity = true;
            } else {
                plan->tokens[plan->token_count++] =
                    stylesheet_identity_token_hash(
                        value == '#', (const char *) identifier, length);
                entry->token_count++;
            }
        }
        at = after > at + 1 ? after : at + 1;
    }
}

#define HAS_FEATURE_ALTERNATIVES 16u

/* Keys one of which every element matching the compound carries: its own
   key, or every alternative of a top-level :is()/:where() whose items all
   end on keyed compounds. False when there are none. */
static bool has_compound_features(StyleHasPlan *plan, const char *text,
                                  size_t at, size_t end, uint32_t *keys,
                                  size_t *count, unsigned depth)
{
    uint32_t key = has_compound_key(text, at, end, &plan->attribute_keys);
    if (key != 0) {
        if (*count == HAS_FEATURE_ALTERNATIVES) return false;
        keys[(*count)++] = key;
        return true;
    }
    if (depth > HAS_NESTING_LIMIT) return false;
    HasPseudo pseudo;
    size_t cursor = at;
    while (has_next_pseudo(text, &cursor, end, &pseudo)) {
        if (!pseudo.functional || !has_pseudo_is_logical(text, &pseudo)
            || has_name_is(text, pseudo.name_begin, pseudo.name_end, "not"))
            continue;
        size_t saved = *count;
        bool keyed = true;
        for (size_t item = pseudo.argument_begin;
             keyed && item < pseudo.argument_end;) {
            size_t item_end = has_item_end(text, item, pseudo.argument_end);
            HasCompound compounds[HAS_COMPOUND_LIMIT];
            bool overflow = false;
            size_t n = has_compounds(text, item, item_end, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
            keyed = !overflow && n != 0
                && has_compound_features(plan, text, compounds[n - 1].begin,
                                         compounds[n - 1].end, keys, count,
                                         depth + 1);
            item = item_end + 1;
        }
        if (keyed) return true;
        *count = saved;
    }
    return false;
}

static void has_add_feature(StyleHasPlan *plan, uint32_t hash, size_t entry,
                            uint8_t role)
{
    if (!has_plan_reserve(plan->budget, (void **) &plan->features,
                          &plan->feature_capacity, plan->feature_count,
                          sizeof(*plan->features))) {
        plan->bounded = true;
        return;
    }
    plan->features[plan->feature_count++] =
        (StyleHasPlanKey) {hash, (uint16_t) entry, role};
    if ((role & HAS_FEATURE_WIDE) != 0) plan->wide_features = true;
}

static bool has_compound_positional(const char *text, size_t at, size_t end)
{
    return has_text_contains(text, at, end, ":first-")
        || has_text_contains(text, at, end, ":last-")
        || has_text_contains(text, at, end, ":nth-")
        || has_text_contains(text, at, end, ":only-")
        || has_text_contains(text, at, end, ":empty")
        || has_text_contains(text, at, end, ":blank");
}

/* The features of one relative selector list (one :has() argument). */
static void has_collect_argument_items(HasAnalysis *analysis, size_t index,
                                       size_t at, size_t end)
{
    StyleHasPlan *plan = analysis->plan;
    const char *text = analysis->text;
    while (at < end && !plan->bounded) {
        StyleHasPlanEntry *entry = &plan->entries[index];
        size_t item_end = has_item_end(text, at, end);
        HasCompound compounds[HAS_COMPOUND_LIMIT];
        bool overflow = false;
        size_t count = has_compounds(text, at, item_end, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
        if (overflow || count == 0) {
            entry->tree_keyless = entry->text_keyless = true;
            at = item_end + 1;
            continue;
        }
        char lead = compounds[0].combinator;
        /* A position or adjacency change among the new siblings can reach
           the rightmost element through their descendants. */
        bool wide = has_argument_escapes(text, at, item_end,
                                         lead == '+' || lead == '~', 0);
        bool sibling = false;
        for (size_t i = 0; i < count; i++) {
            char combinator = compounds[i].combinator;
            if (i != 0 && (combinator == '+' || combinator == '~'))
                sibling = true;
            else if (i != 0 && sibling)
                wide = true;
            if (i + 1 == count) break;
            if (has_compound_positional(text, compounds[i].begin,
                                        compounds[i].end)) wide = true;
            if (has_text_contains(text, compounds[i].begin, compounds[i].end,
                                  ":empty")
                || has_text_contains(text, compounds[i].begin,
                                     compounds[i].end, ":blank"))
                entry->tree_keyless = entry->text_keyless = true;
        }
        const HasCompound *last = &compounds[count - 1];
        bool empty = has_text_contains(text, last->begin, last->end, ":empty")
            || has_text_contains(text, last->begin, last->end, ":blank");
        uint32_t keys[HAS_FEATURE_ALTERNATIVES];
        size_t key_count = 0;
        if (!has_compound_features(plan, text, last->begin, last->end, keys,
                                   &key_count, 0)) {
            entry->tree_keyless = true;
            if (empty) entry->text_keyless = true;
        } else {
            bool adjacent = wide || sibling || lead == '+' || lead == '~'
                || has_compound_positional(text, last->begin, last->end);
            uint8_t role = (uint8_t) (HAS_FEATURE_NARROW
                | (wide ? HAS_FEATURE_WIDE : 0)
                | (empty ? HAS_FEATURE_EMPTY : 0)
                | (adjacent ? HAS_FEATURE_SIBLING : 0));
            for (size_t k = 0; k < key_count; k++)
                has_add_feature(plan, keys[k], index, role);
        }
        at = item_end + 1;
    }
}

/* The argument's own items and those of every :has() nested in it. */
static void has_collect_features(HasAnalysis *analysis, size_t index,
                                 size_t at, size_t end)
{
    const char *text = analysis->text;
    has_collect_argument_items(analysis, index, at, end);
    for (size_t nested = at; nested + 5 <= end; nested++) {
        if (strncasecmp(text + nested, ":has(", 5) != 0) continue;
        size_t close = has_skip_block(text, nested + 4, end);
        size_t argument_end = close > nested + 5 && text[close - 1] == ')'
            ? close - 1 : close;
        has_collect_argument_items(analysis, index, nested + 5, argument_end);
    }
}

static void has_add_entry(HasAnalysis *analysis, uint8_t relation,
                          uint32_t anchor_key, size_t argument_begin,
                          size_t argument_end)
{
    StyleHasPlan *plan = analysis->plan;
    if (plan->bounded) return;
    if (plan->entry_count == STYLE_HAS_PLAN_LIMIT
        || !has_plan_reserve(plan->budget, (void **) &plan->entries,
                             &plan->entry_capacity, plan->entry_count,
                             sizeof(*plan->entries))) {
        plan->bounded = true;
        return;
    }
    StyleHasPlanEntry *entry = &plan->entries[plan->entry_count++];
    memset(entry, 0, sizeof(*entry));
    entry->rule = analysis->rule;
    entry->custom_names = plan->analyzing_custom_names;
    entry->relation = relation;
    entry->subject_key = analysis->subject_key;
    entry->anchor_key = relation == STYLE_HAS_RELATION_SELF && anchor_key == 0
        ? analysis->subject_key : anchor_key;
    if (argument_end <= argument_begin) {
        /* A rule the classifier could not place: any change concerns it. */
        entry->any_identity = true;
        entry->tree_keyless = entry->text_keyless = true;
        return;
    }
    has_collect_tokens(analysis, entry, argument_begin, argument_end);
    has_collect_features(analysis, plan->entry_count - 1, argument_begin,
                         argument_end);
    if (relation == STYLE_HAS_RELATION_OTHER) return;
    entry->region = has_argument_region(analysis->text, argument_begin,
                                        argument_end);
    entry->escapes = has_argument_escapes(
        analysis->text, argument_begin, argument_end,
        (entry->region & STYLE_HAS_REGION_SIBLINGS) != 0, 0);
}

#define HAS_FOCUS_MARKER "data-tilefinch-focus"

/* One :focus-within of the rule, `relation` to its subject. The answer
   moves when the focus marker enters or leaves the anchor's subtree (the
   anchor included): the walk from the focus change's node visits exactly
   those anchors. */
static void has_add_focus_entry(HasAnalysis *analysis, uint8_t relation,
                                uint32_t anchor_key)
{
    StyleHasPlan *plan = analysis->plan;
    if (plan->bounded) return;
    if (plan->entry_count == STYLE_HAS_PLAN_LIMIT
        || !has_plan_reserve(plan->budget, (void **) &plan->entries,
                             &plan->entry_capacity, plan->entry_count,
                             sizeof(*plan->entries))) {
        plan->bounded = true;
        return;
    }
    size_t index = plan->entry_count++;
    StyleHasPlanEntry *entry = &plan->entries[index];
    memset(entry, 0, sizeof(*entry));
    entry->rule = analysis->rule;
    entry->relation = relation;
    entry->subject_key = analysis->subject_key;
    entry->anchor_key = relation == STYLE_HAS_RELATION_SELF && anchor_key == 0
        ? analysis->subject_key : anchor_key;
    entry->token_first = (uint32_t) plan->token_count;
    entry->region = STYLE_HAS_REGION_DESCENDANTS;
    entry->focus = true;
    entry->custom_names = plan->analyzing_custom_names;
    /* A removed or inserted subtree holding the focused element moves the
       answer too. */
    has_add_feature(plan, has_key_hash(HAS_KEY_ATTRIBUTE,
                                       (const unsigned char *) HAS_FOCUS_MARKER,
                                       sizeof(HAS_FOCUS_MARKER) - 1u),
                    index, HAS_FEATURE_NARROW);
    plan->attribute_keys = true;
}

static uint8_t has_relation_combine(uint8_t inner, uint8_t outer)
{
    if (inner == STYLE_HAS_RELATION_OTHER
        || outer == STYLE_HAS_RELATION_OTHER) return STYLE_HAS_RELATION_OTHER;
    return inner == STYLE_HAS_RELATION_SELF ? outer : inner;
}

static void has_analyze_list(HasAnalysis *analysis, size_t at, size_t end,
                             uint8_t outer_relation, uint32_t outer_key,
                             unsigned depth);

/* One complex selector whose subject is `outer_relation` to the rule's
   subject and carries `outer_key`. */
static void has_analyze_complex(HasAnalysis *analysis, size_t at, size_t end,
                                uint8_t outer_relation, uint32_t outer_key,
                                unsigned depth)
{
    const char *text = analysis->text;
    HasCompound compounds[HAS_COMPOUND_LIMIT];
    bool overflow = false;
    size_t count = has_compounds(text, at, end, compounds,
                                 HAS_COMPOUND_LIMIT, &overflow);
    if (overflow) {
        analysis->other = true;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (!has_text_contains(text, compounds[i].begin, compounds[i].end,
                               ":has(")
            && !has_text_contains(text, compounds[i].begin, compounds[i].end,
                                  ":focus-within")) continue;
        /* Only descendant and child combinators towards the subject keep
           the anchor on its ancestor chain. */
        uint8_t inner = STYLE_HAS_RELATION_SELF;
        for (size_t j = i + 1; j < count; j++) {
            char combinator = compounds[j].combinator;
            if (combinator == '+' || combinator == '~')
                inner = STYLE_HAS_RELATION_OTHER;
            else if (inner == STYLE_HAS_RELATION_SELF)
                inner = STYLE_HAS_RELATION_ANCESTOR;
        }
        uint8_t relation = has_relation_combine(inner, outer_relation);
        uint32_t key = has_compound_key(text, compounds[i].begin,
                                        compounds[i].end,
                                        &analysis->plan->attribute_keys);
        if (key == 0 && inner == STYLE_HAS_RELATION_SELF) key = outer_key;
        HasPseudo pseudo;
        size_t cursor = compounds[i].begin;
        while (has_next_pseudo(text, &cursor, compounds[i].end, &pseudo)) {
            if (!pseudo.functional) {
                if (has_name_is(text, pseudo.name_begin, pseudo.name_end,
                                "focus-within"))
                    has_add_focus_entry(analysis, relation, key);
                continue;
            }
            size_t ab = pseudo.argument_begin, ae = pseudo.argument_end;
            if (has_name_is(text, pseudo.name_begin, pseudo.name_end, "has")) {
                /* The :has() entry also covers a :focus-within inside. */
                has_add_entry(analysis, relation, key, ab, ae);
            } else if (has_text_contains(text, ab, ae, ":has(")
                       || has_text_contains(text, ab, ae, ":focus-within")) {
                if (has_pseudo_is_logical(text, &pseudo)) {
                    /* Each item's subject is this compound's element. */
                    has_analyze_list(analysis, ab, ae, relation, key,
                                     depth + 1);
                } else {
                    analysis->other = true;
                }
            }
        }
    }
}

static void has_analyze_list(HasAnalysis *analysis, size_t at, size_t end,
                             uint8_t outer_relation, uint32_t outer_key,
                             unsigned depth)
{
    if (depth > HAS_NESTING_LIMIT) {
        analysis->other = true;
        return;
    }
    while (at < end) {
        size_t item_end = has_item_end(analysis->text, at, end);
        has_analyze_complex(analysis, at, item_end, outer_relation,
                            outer_key, depth);
        at = item_end + 1;
    }
}

static void has_analyze_rule(StyleHasPlan *plan, uint32_t rule,
                             const char *text, size_t length)
{
    HasAnalysis analysis = {.plan = plan, .text = text, .rule = rule};
    if (has_item_end(text, 0, length) == length) {
        HasCompound compounds[HAS_COMPOUND_LIMIT];
        bool overflow = false;
        size_t count = has_compounds(text, 0, length, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
        if (!overflow && count != 0)
            analysis.subject_key = has_compound_key(
                text, compounds[count - 1].begin, compounds[count - 1].end,
                &plan->attribute_keys);
    }
    size_t before = plan->entry_count;
    has_analyze_list(&analysis, 0, length, STYLE_HAS_RELATION_SELF, 0, 0);
    if (analysis.other || plan->entry_count == before)
        has_add_entry(&analysis, STYLE_HAS_RELATION_OTHER, 0, 0, 0);
}

static int has_key_compare(const void *left, const void *right)
{
    const StyleHasPlanKey *a = left, *b = right;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    return (int) a->entry - (int) b->entry;
}

static void has_plan_index(StyleHasPlan *plan)
{
    size_t keys = 0, unkeyed_anchors = 0, unkeyed_subjects = 0;
    for (size_t i = 0; i < plan->entry_count; i++) {
        const StyleHasPlanEntry *entry = &plan->entries[i];
        if (entry->relation != STYLE_HAS_RELATION_OTHER) {
            if (entry->anchor_key != 0) keys++;
            else unkeyed_anchors++;
        }
        if (entry->subject_key != 0) keys++;
        else unkeyed_subjects++;
    }
    plan->keys = keys == 0 ? NULL
        : budget_malloc(plan->budget, keys * sizeof(*plan->keys));
    plan->unkeyed_anchors = unkeyed_anchors == 0 ? NULL
        : budget_malloc(plan->budget,
                        unkeyed_anchors * sizeof(*plan->unkeyed_anchors));
    plan->unkeyed_subjects = unkeyed_subjects == 0 ? NULL
        : budget_malloc(plan->budget,
                        unkeyed_subjects * sizeof(*plan->unkeyed_subjects));
    if ((keys != 0 && plan->keys == NULL)
        || (unkeyed_anchors != 0 && plan->unkeyed_anchors == NULL)
        || (unkeyed_subjects != 0 && plan->unkeyed_subjects == NULL)) {
        plan->bounded = true;
        return;
    }
    for (size_t i = 0; i < plan->entry_count; i++) {
        const StyleHasPlanEntry *entry = &plan->entries[i];
        if (entry->relation != STYLE_HAS_RELATION_OTHER) {
            if (entry->anchor_key != 0) {
                plan->keys[plan->key_count++] = (StyleHasPlanKey) {
                    entry->anchor_key, (uint16_t) i, HAS_ROLE_ANCHOR};
            } else {
                plan->unkeyed_anchors[plan->unkeyed_anchor_count++] =
                    (uint16_t) i;
            }
        }
        if (entry->subject_key != 0) {
            plan->keys[plan->key_count++] = (StyleHasPlanKey) {
                entry->subject_key, (uint16_t) i, HAS_ROLE_SUBJECT};
        } else {
            plan->unkeyed_subjects[plan->unkeyed_subject_count++] =
                (uint16_t) i;
        }
    }
    qsort(plan->keys, plan->key_count, sizeof(*plan->keys), has_key_compare);
    qsort(plan->features, plan->feature_count, sizeof(*plan->features),
          has_key_compare);
}

void style_has_plan_destroy(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    StyleHasPlan *plan = sheet->has_plan;
    sheet->has_plan = NULL;
    sheet->has_plan_ready = false;
    if (plan == NULL) return;
    Budget *budget = plan->budget;
    budget_free(budget, plan->entries);
    budget_free(budget, plan->tokens);
    budget_free(budget, plan->keys);
    budget_free(budget, plan->unkeyed_anchors);
    budget_free(budget, plan->unkeyed_subjects);
    budget_free(budget, plan->features);
    budget_free(budget, plan);
}

size_t style_has_plan_bytes(const Stylesheet *sheet)
{
    const StyleHasPlan *plan = sheet == NULL ? NULL : sheet->has_plan;
    if (plan == NULL) return 0;
    return sizeof(*plan)
        + plan->entry_capacity * sizeof(*plan->entries)
        + plan->token_capacity * sizeof(*plan->tokens)
        + plan->key_count * sizeof(*plan->keys)
        + plan->unkeyed_anchor_count * sizeof(*plan->unkeyed_anchors)
        + plan->unkeyed_subject_count * sizeof(*plan->unkeyed_subjects)
        + plan->feature_capacity * sizeof(*plan->features);
}

static const StyleHasPlan *has_plan_prepare(const Stylesheet *const_sheet)
{
    /* A memo on the sheet, like the has-rule index it follows. */
    Stylesheet *sheet = (Stylesheet *) const_sheet;
    if (!stylesheet_prepare_has_rule_index(sheet)) return NULL;
    if (sheet->has_plan_ready) return sheet->has_plan;
    style_has_plan_destroy(sheet);
    StyleHasPlan *plan = budget_calloc(sheet->budget, 1, sizeof(*plan));
    if (plan != NULL) {
        static uint32_t serials;
        plan->budget = sheet->budget;
        plan->serial = ++serials == 0 ? ++serials : serials;
        for (size_t k = 0; k < sheet->has_rule_count && !plan->bounded; k++) {
            uint32_t index = sheet->has_rule_indices[k];
            const StyleRule *rule = &sheet->rules[index];
            if (rule->selector != NULL)
                has_analyze_rule(plan, index, rule->selector,
                                 rule->selector_length);
        }
        for (size_t k = 0; k < sheet->has_custom_rule_count && !plan->bounded;
             k++) {
            uint32_t index = sheet->has_custom_rule_indices[k];
            const char *selector = sheet->custom_rules[index].selector;
            plan->analyzing_custom_names =
                stylesheet_custom_property_name_bits(
                    sheet->custom_rules[index].name,
                    sheet->custom_rules[index].name_length);
            if (selector != NULL)
                has_analyze_rule(plan, index | STYLE_HAS_RULE_CUSTOM,
                                 selector, strlen(selector));
            plan->analyzing_custom_names = 0;
        }
        /* :focus-within outside any :has() (those were analyzed above). */
        for (size_t i = 0; i < sheet->count && !plan->bounded; i++) {
            const StyleRule *rule = &sheet->rules[i];
            if (rule->selector != NULL
                && strstr(rule->selector, ":focus-within") != NULL
                && strstr(rule->selector, ":has(") == NULL)
                has_analyze_rule(plan, (uint32_t) i, rule->selector,
                                 rule->selector_length);
        }
        for (size_t i = 0; i < sheet->custom_rule_count && !plan->bounded;
             i++) {
            const char *selector = sheet->custom_rules[i].selector;
            if (selector == NULL || strstr(selector, ":focus-within") == NULL
                || strstr(selector, ":has(") != NULL) continue;
            plan->analyzing_custom_names =
                stylesheet_custom_property_name_bits(
                    sheet->custom_rules[i].name,
                    sheet->custom_rules[i].name_length);
            has_analyze_rule(plan, (uint32_t) i | STYLE_HAS_RULE_CUSTOM,
                             selector, strlen(selector));
            plan->analyzing_custom_names = 0;
        }
        if (!plan->bounded) has_plan_index(plan);
    }
    sheet->has_plan = plan;
    sheet->has_plan_ready = true;
    return plan;
}

size_t style_has_plan_summarize(const Stylesheet *sheet,
                                StyleHasPlanSummary *out, size_t capacity,
                                bool *bounded)
{
    const StyleHasPlan *plan = sheet == NULL ? NULL : has_plan_prepare(sheet);
    if (bounded != NULL) *bounded = plan == NULL || plan->bounded;
    if (plan == NULL) return 0;
    for (size_t i = 0; i < plan->entry_count && i < capacity; i++) {
        const StyleHasPlanEntry *entry = &plan->entries[i];
        out[i] = (StyleHasPlanSummary) {
            .rule = entry->rule,
            .relation = (StyleHasRelation) entry->relation,
            .region = entry->region,
            .escapes = entry->escapes,
            .subject_keyed = entry->subject_key != 0,
            .anchor_keyed = entry->anchor_key != 0,
        };
    }
    return plan->entry_count;
}

/* ---- Relevance of one change. ----------------------------------------- */

static const char *has_entry_text(const Stylesheet *sheet,
                                  const StyleHasPlanEntry *entry,
                                  size_t *length)
{
    uint32_t index = entry->rule & ~STYLE_HAS_RULE_CUSTOM;
    const char *text = NULL;
    if ((entry->rule & STYLE_HAS_RULE_CUSTOM) != 0) {
        if (index < sheet->custom_rule_count)
            text = sheet->custom_rules[index].selector;
        *length = text == NULL ? 0 : strlen(text);
    } else if (index < sheet->count) {
        text = sheet->rules[index].selector;
        *length = text == NULL ? 0 : sheet->rules[index].selector_length;
    }
    return text;
}

/* Whether `text` tests the attribute `name` ([name...], compared as a
   prefix since journals bound names) or a pseudo-class that reads it. */
static bool has_text_reads_attribute(const char *text, size_t length,
                                     const char *name)
{
    size_t name_length = strlen(name);
    for (size_t at = 0; at < length; at++) {
        if (text[at] != '[') continue;
        size_t begin = at + 1;
        while (begin < length && isspace((unsigned char) text[begin]))
            begin++;
        if (begin + name_length <= length
            && strncasecmp(text + begin, name, name_length) == 0) return true;
    }
    static const struct { const char *attribute, *pseudo; } pseudos[] = {
        {"data-tilefinch-focus", ":focus"},
        {"checked", ":checked"}, {"selected", ":checked"},
        {"disabled", ":disabled"}, {"disabled", ":enabled"},
        {"required", ":required"}, {"required", ":optional"},
        {"readonly", ":read-"}, {"contenteditable", ":read-"},
        {"href", ":link"}, {"href", ":any-link"},
        {"open", ":open"}, {"open", ":modal"},
        {"data-tilefinch-modal", ":modal"},
        {"data-tilefinch-popover-open", ":popover-open"},
        {"data-tilefinch-popover-open", ":open"},
        {"value", ":placeholder-shown"}, {"placeholder", ":placeholder-shown"},
        {"dir", ":dir("}, {"lang", ":lang("},
    };
    for (size_t i = 0; i < sizeof(pseudos) / sizeof(pseudos[0]); i++) {
        if (strcasecmp(name, pseudos[i].attribute) == 0
            && has_text_contains(text, 0, length, pseudos[i].pseudo))
            return true;
    }
    return false;
}

static bool has_entry_relevant(const Stylesheet *sheet,
                               const StyleHasPlan *plan,
                               const StyleHasPlanEntry *entry,
                               const char *attribute, const uint32_t *tokens,
                               size_t token_count)
{
    if (entry->focus) {
        /* Only a focus move (or an unknown write) moves a :focus-within;
           an anchor's class change reaches its rules through their
           relational tokens, not this plan. */
        return tokens == NULL
            && (attribute == NULL
                || strcasecmp(attribute, HAS_FOCUS_MARKER) == 0);
    }
    if (tokens != NULL) {
        if (entry->any_identity) return true;
        for (size_t t = 0; t < token_count; t++) {
            for (size_t k = 0; k < entry->token_count; k++) {
                if (plan->tokens[entry->token_first + k] == tokens[t])
                    return true;
            }
        }
        return false;
    }
    if (attribute == NULL || strcasecmp(attribute, "class") == 0
        || strcasecmp(attribute, "id") == 0) return true;
    size_t length = 0;
    const char *text = has_entry_text(sheet, entry, &length);
    return text == NULL || has_text_reads_attribute(text, length, attribute);
}

/* ---- The walk. -------------------------------------------------------- */

#define HAS_BIT(set, index) \
    ((((set)[(index) >> 5] >> ((index) & 31u)) & 1u) != 0)

typedef struct {
    const StyleHasPlan *plan;
    StyleHasPending *pending;
    uint32_t relevant[STYLE_HAS_PLAN_LIMIT / 32u];
    StyleHasDrop drop;
    void *opaque;
    StyleHasNoteStats *stats;
    size_t sibling_visits;
    bool sibling_overflow;
} HasWalk;

static void has_mark(StyleHasPending *pending, const StyleHasPlan *plan,
                     size_t index, bool anywhere)
{
    uint32_t *set = anywhere ? pending->anywhere : pending->under_roots;
    set[index >> 5] |= UINT32_C(1) << (index & 31u);
    pending->marked = true;
    pending->active = true;
    pending->plan = plan;
    if ((plan->entries[index].rule & STYLE_HAS_RULE_CUSTOM) == 0)
        pending->author_marked = true;
}

/* A relevant entry no walk or key can place: every computed style goes,
   and the retained lists its rule can select. */
static void has_unplaced(StyleHasPending *pending, const StyleHasPlan *plan,
                         size_t index, StyleHasNoteStats *stats,
                         bool *wiped)
{
    uint32_t rule = plan->entries[index].rule;
    pending->styles_all = true;
    pending->active = true;
    pending->plan = plan;
    if (!*wiped) {
        *wiped = true;
        stats->wipes++;
    }
    if ((rule & STYLE_HAS_RULE_CUSTOM) != 0) return;
    for (size_t i = 0; i < pending->unplaced_rule_count; i++) {
        if (pending->unplaced_rules[i] == rule) return;
    }
    if (pending->unplaced_rule_count == sizeof(pending->unplaced_rules)
                                        / sizeof(pending->unplaced_rules[0])) {
        /* Past the bound every retained list goes (UINT32_MAX). */
        pending->unplaced_rules[0] = UINT32_MAX;
        pending->unplaced_rule_count = 1;
        return;
    }
    if (pending->unplaced_rule_count == 1
        && pending->unplaced_rules[0] == UINT32_MAX) return;
    pending->unplaced_rules[pending->unplaced_rule_count++] = rule;
}

static void has_add_root(StyleHasPending *pending, const lxb_dom_node_t *node)
{
    for (size_t i = 0; i < pending->root_count; i++) {
        if (pending->roots[i] == node) return;
    }
    if (pending->root_count == STYLE_HAS_ROOT_LIMIT) {
        pending->roots_everywhere = true;
        return;
    }
    pending->roots[pending->root_count++] = node;
}

/* Calls `visit` with each key hash the element carries. */
typedef bool (*HasKeyVisit)(void *context, uint32_t hash);

static bool has_element_keys(const StyleHasPlan *plan,
                             const lxb_dom_node_t *node, HasKeyVisit visit,
                             void *context)
{
    StyleMatchSubject subject;
    style_match_subject_prepare((lxb_dom_node_t *) node, &subject);
    if (subject.id != NULL && subject.id_length != 0
        && visit(context, has_key_hash('#', (const unsigned char *) subject.id,
                                       subject.id_length))) return true;
    if (subject.tag != NULL && subject.tag_length != 0
        && visit(context, has_key_hash(HAS_KEY_TAG,
                                       (const unsigned char *) subject.tag,
                                       subject.tag_length))) return true;
    const char *classes = subject.classes;
    size_t length = classes == NULL ? 0 : subject.classes_length;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) classes[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) classes[end])) end++;
        if (end > at && visit(context, has_key_hash(
                '.', (const unsigned char *) classes + at, end - at)))
            return true;
        at = end;
    }
    if (!plan->attribute_keys) return false;
    size_t attributes = 0;
    for (const lxb_dom_attr_t *attribute =
             lxb_dom_interface_element((lxb_dom_node_t *) node)->first_attr;
         attribute != NULL && attributes < 256u;
         attribute = attribute->next, attributes++) {
        size_t name_length = 0;
        const lxb_char_t *name = lxb_dom_attr_qualified_name(
            (lxb_dom_attr_t *) attribute, &name_length);
        if (name != NULL && name_length != 0
            && visit(context, has_key_hash(HAS_KEY_ATTRIBUTE, name,
                                           name_length))) return true;
    }
    return false;
}

static size_t has_lower_bound(const StyleHasPlanKey *keys, size_t count,
                              uint32_t hash)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (keys[middle].hash < hash) low = middle + 1;
        else high = middle;
    }
    return low;
}

static size_t has_key_lower_bound(const StyleHasPlan *plan, uint32_t hash)
{
    return has_lower_bound(plan->keys, plan->key_count, hash);
}

typedef struct {
    HasWalk *walk;
    lxb_dom_node_t *node;
    uint8_t category;
    bool dropped;
} HasVisit;

static void has_visit_entry(HasVisit *visit, size_t index)
{
    HasWalk *walk = visit->walk;
    const StyleHasPlanEntry *entry = &walk->plan->entries[index];
    if (!HAS_BIT(walk->relevant, index)
        || (entry->region & visit->category) == 0) return;
    if (entry->relation == STYLE_HAS_RELATION_SELF) {
        if (!visit->dropped) {
            visit->dropped = true;
            walk->drop(walk->opaque, visit->node);
            walk->stats->drops++;
        }
        if (entry->custom_names != 0) {
            StyleHasPending *pending = walk->pending;
            bool known = false;
            for (size_t i = 0; !known && i < pending->name_root_count; i++)
                known = pending->name_roots[i] == visit->node;
            if (known || pending->names_everywhere) {
            } else if (pending->name_root_count == STYLE_HAS_ROOT_LIMIT) {
                pending->names_everywhere = true;
            } else {
                pending->name_roots[pending->name_root_count++] = visit->node;
            }
        }
    } else if (entry->relation == STYLE_HAS_RELATION_ANCESTOR) {
        has_add_root(walk->pending, visit->node);
        has_mark(walk->pending, walk->plan, index, false);
    }
}

static bool has_visit_key(void *context, uint32_t hash)
{
    HasVisit *visit = context;
    const StyleHasPlan *plan = visit->walk->plan;
    for (size_t k = has_key_lower_bound(plan, hash);
         k < plan->key_count && plan->keys[k].hash == hash; k++) {
        if (plan->keys[k].role == HAS_ROLE_ANCHOR)
            has_visit_entry(visit, plan->keys[k].entry);
    }
    return false;
}

static void has_visit(HasWalk *walk, lxb_dom_node_t *node, uint8_t category)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    walk->stats->visits++;
    HasVisit visit = {.walk = walk, .node = node, .category = category};
    (void) has_element_keys(walk->plan, node, has_visit_key, &visit);
    for (size_t i = 0; i < walk->plan->unkeyed_anchor_count; i++)
        has_visit_entry(&visit, walk->plan->unkeyed_anchors[i]);
}

static bool has_sibling_admitted(HasWalk *walk)
{
    if (walk->sibling_visits == HAS_SIBLING_LIMIT) {
        walk->sibling_overflow = true;
        return false;
    }
    walk->sibling_visits++;
    return true;
}

static void has_visit_children(HasWalk *walk, lxb_dom_node_t *parent)
{
    if (parent == NULL) return;
    for (lxb_dom_node_t *child = parent->first_child;
         child != NULL && has_sibling_admitted(walk); child = child->next)
        has_visit(walk, child, STYLE_HAS_REGION_SIBLINGS);
}

/* `relevant`, when given, names the entries a structure change can move
   (has_tree_probe); otherwise `bloom` summarizes them
   (style_has_entry_bloom_bit; UINT64_MAX: every entry). */
static bool has_note_change(const Stylesheet *sheet, lxb_dom_node_t *node,
                            bool structure, const char *attribute,
                            const uint32_t *tokens, size_t token_count,
                            const uint32_t *relevant, uint64_t bloom,
                            StyleHasPending *pending, StyleHasDrop drop,
                            void *opaque, StyleHasNoteStats *stats)
{
    StyleHasNoteStats scratch = {0};
    if (sheet == NULL || node == NULL || pending == NULL || drop == NULL)
        return false;
    const StyleHasPlan *plan = has_plan_prepare(sheet);
    if (plan == NULL || plan->bounded) return false;
    HasWalk walk = {
        .plan = plan, .pending = pending, .drop = drop, .opaque = opaque,
        .stats = stats == NULL ? &scratch : stats
    };
    /* Where an escaping argument's reads can reach anchors from: the
       changed element's subtree, or the changed child list's parent's. */
    lxb_dom_node_t *base = structure && node->parent != NULL
        && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT
        ? node->parent : node;
    bool any = false, siblings = false, wiped = false;
    for (size_t i = 0; i < plan->entry_count; i++) {
        const StyleHasPlanEntry *entry = &plan->entries[i];
        if (relevant != NULL ? !HAS_BIT(relevant, i)
            : structure ? (bloom & style_has_entry_bloom_bit(i)) == 0
            : !has_entry_relevant(sheet, plan, entry, attribute, tokens,
                                  token_count)) continue;
        walk.relevant[i >> 5] |= UINT32_C(1) << (i & 31u);
        any = true;
        if (entry->custom_names != 0) {
            pending->custom_names |= entry->custom_names;
            pending->active = true;
            pending->plan = plan;
        }
        if (entry->relation == STYLE_HAS_RELATION_OTHER) {
            if (entry->subject_key == 0)
                has_unplaced(pending, plan, i, walk.stats, &wiped);
            else
                has_mark(pending, plan, i, true);
            continue;
        }
        if (entry->escapes) {
            has_add_root(pending, base);
            has_mark(pending, plan, i, false);
        }
        if ((entry->region & STYLE_HAS_REGION_SIBLINGS) != 0) siblings = true;
    }
    if (!any) return true;
    /* Every entry relevant: this walk's visits cover any later one's. */
    bool covering = structure && relevant == NULL && bloom == UINT64_MAX;
    size_t depth = 0;
    for (lxb_dom_node_t *at = node;
         at != NULL && at->type != LXB_DOM_NODE_TYPE_DOCUMENT;
         at = at->parent) {
        if (++depth > HAS_DEPTH_LIMIT) return false;
        bool walked = false;
        for (size_t i = 0; !walked && i < pending->walked_count; i++)
            walked = pending->walked[i] == at;
        if (walked) break;
        has_visit(&walk, at, STYLE_HAS_REGION_DESCENDANTS);
        for (lxb_dom_node_t *earlier = at->prev;
             siblings && earlier != NULL && has_sibling_admitted(&walk);
             earlier = earlier->prev)
            has_visit(&walk, earlier, STYLE_HAS_REGION_SIBLINGS);
        if (covering && !walk.sibling_overflow
            && pending->walked_count < STYLE_HAS_WALKED_LIMIT)
            pending->walked[pending->walked_count++] = at;
    }
    if (structure && siblings) {
        /* Indices move for every sibling, before and after the change. */
        has_visit_children(&walk, node);
        if (node->parent != NULL
            && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT)
            has_visit_children(&walk, node->parent);
    }
    if (walk.sibling_overflow) {
        for (size_t i = 0; i < plan->entry_count; i++) {
            const StyleHasPlanEntry *entry = &plan->entries[i];
            if (!HAS_BIT(walk.relevant, i)
                || entry->relation == STYLE_HAS_RELATION_OTHER
                || (entry->region & STYLE_HAS_REGION_SIBLINGS) == 0) continue;
            if (entry->subject_key == 0)
                has_unplaced(pending, plan, i, walk.stats, &wiped);
            else
                has_mark(pending, plan, i, true);
        }
    }
    return true;
}

bool style_has_note_change(const Stylesheet *sheet, lxb_dom_node_t *node,
                           bool structure, const char *attribute,
                           const uint32_t *tokens, size_t token_count,
                           StyleHasPending *pending, StyleHasDrop drop,
                           void *opaque, StyleHasNoteStats *stats)
{
    return has_note_change(sheet, node, structure, attribute, tokens,
                           token_count, NULL, UINT64_MAX, pending, drop,
                           opaque, stats);
}

bool style_has_note_structure(const Stylesheet *sheet, lxb_dom_node_t *node,
                              uint64_t entries, uint32_t serial,
                              StyleHasPending *pending, StyleHasDrop drop,
                              void *opaque, StyleHasNoteStats *stats)
{
    const StyleHasPlan *plan = sheet == NULL ? NULL : has_plan_prepare(sheet);
    /* Indices a different build classified mean nothing here. */
    if (plan == NULL || plan->serial != serial) entries = UINT64_MAX;
    return has_note_change(sheet, node, true, NULL, NULL, 0, NULL, entries,
                           pending, drop, opaque, stats);
}

/* ---- Selecting cached entries for the pending changes. ---------------- */

typedef struct {
    const StyleHasPlan *plan;
    const StyleHasPending *pending;
    const lxb_dom_node_t *node;
    int under_roots;
} HasSelect;

static bool has_select_under_roots(HasSelect *select)
{
    if (select->under_roots < 0) {
        const StyleHasPending *pending = select->pending;
        bool under = pending->roots_everywhere;
        for (const lxb_dom_node_t *at = select->node->parent;
             !under && at != NULL; at = at->parent) {
            for (size_t i = 0; !under && i < pending->root_count; i++)
                under = pending->roots[i] == at;
        }
        select->under_roots = under ? 1 : 0;
    }
    return select->under_roots != 0;
}

static bool has_select_entry(HasSelect *select, size_t index)
{
    if (HAS_BIT(select->pending->anywhere, index)) return true;
    return HAS_BIT(select->pending->under_roots, index)
        && has_select_under_roots(select);
}

static bool has_select_key(void *context, uint32_t hash)
{
    HasSelect *select = context;
    const StyleHasPlan *plan = select->plan;
    for (size_t k = has_key_lower_bound(plan, hash);
         k < plan->key_count && plan->keys[k].hash == hash; k++) {
        if (plan->keys[k].role == HAS_ROLE_SUBJECT
            && has_select_entry(select, plan->keys[k].entry)) return true;
    }
    return false;
}

bool style_has_pending_selects(const Stylesheet *sheet,
                               const StyleHasPending *pending,
                               const lxb_dom_node_t *node)
{
    if (pending == NULL || !pending->active || node == NULL) return false;
    const StyleHasPlan *plan = sheet == NULL || !sheet->has_plan_ready
        ? NULL : sheet->has_plan;
    if (plan == NULL || plan->bounded || plan != pending->plan) return true;
    HasSelect select = {
        .plan = plan, .pending = pending, .node = node, .under_roots = -1
    };
    for (size_t i = 0; i < plan->unkeyed_subject_count; i++) {
        if (has_select_entry(&select, plan->unkeyed_subjects[i])) return true;
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return has_element_keys(plan, node, has_select_key, &select);
}

bool style_has_pending_names_reach(const StyleHasPending *pending,
                                   const lxb_dom_node_t *node)
{
    if (pending == NULL) return false;
    if (pending->names_everywhere) return true;
    for (const lxb_dom_node_t *at = node;
         at != NULL && pending->name_root_count != 0; at = at->parent) {
        for (size_t i = 0; i < pending->name_root_count; i++)
            if (pending->name_roots[i] == at) return true;
    }
    return false;
}

static size_t has_retire_roots(const lxb_dom_node_t **roots, size_t count,
                               const lxb_dom_node_t *root)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        bool inside = false;
        for (const lxb_dom_node_t *at = roots[i];
             !inside && at != NULL; at = at->parent)
            inside = at == root;
        if (!inside) roots[kept++] = roots[i];
    }
    return kept;
}

void style_has_pending_retire(StyleHasPending *pending,
                              const lxb_dom_node_t *root)
{
    if (pending == NULL || root == NULL) return;
    pending->root_count = (uint8_t) has_retire_roots(
        pending->roots, pending->root_count, root);
    /* A retired subject's readers are its descendants, retired with it. */
    pending->name_root_count = (uint8_t) has_retire_roots(
        pending->name_roots, pending->name_root_count, root);
    pending->walked_count = (uint8_t) has_retire_roots(
        pending->walked, pending->walked_count, root);
}

/* ---- Tree changes: can any :has() answer move? ------------------------ */

#define HAS_TREE_SUBTREE_LIMIT 512u

typedef struct {
    const StyleHasPlan *plan;
    uint32_t hit[STYLE_HAS_PLAN_LIMIT / 32u];
    uint8_t role;
    bool any;
    bool sibling_region;
    bool unbounded;
} HasTreeProbe;

static void has_tree_hit(HasTreeProbe *probe, size_t index)
{
    const StyleHasPlanEntry *entry = &probe->plan->entries[index];
    probe->hit[index >> 5] |= UINT32_C(1) << (index & 31u);
    probe->any = true;
    if (entry->relation == STYLE_HAS_RELATION_OTHER || entry->escapes
        || entry->anchor_key == 0) probe->unbounded = true;
    if ((entry->region & STYLE_HAS_REGION_SIBLINGS) != 0)
        probe->sibling_region = true;
}

static bool has_tree_feature(void *context, uint32_t hash)
{
    HasTreeProbe *probe = context;
    const StyleHasPlan *plan = probe->plan;
    for (size_t k = has_lower_bound(plan->features, plan->feature_count,
                                    hash);
         k < plan->feature_count && plan->features[k].hash == hash; k++) {
        if ((plan->features[k].role & probe->role) != 0)
            has_tree_hit(probe, plan->features[k].entry);
    }
    return false;
}

/* Probes `root`'s elements (itself when `inclusive`). False past the
   bound. */
static bool has_tree_subtree(HasTreeProbe *probe, lxb_dom_node_t *root,
                             bool inclusive)
{
    size_t visited = 0;
    lxb_dom_node_t *node = inclusive ? root : root->first_child;
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (++visited > HAS_TREE_SUBTREE_LIMIT) return false;
            (void) has_element_keys(probe->plan, node, has_tree_feature,
                                    probe);
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        node = node == NULL || node == root ? NULL : node->next;
    }
    return true;
}

static bool has_tree_anchor(void *context, uint32_t hash)
{
    HasTreeProbe *probe = context;
    const StyleHasPlan *plan = probe->plan;
    for (size_t k = has_key_lower_bound(plan, hash);
         k < plan->key_count && plan->keys[k].hash == hash; k++) {
        if (plan->keys[k].role == HAS_ROLE_ANCHOR
            && HAS_BIT(probe->hit, plan->keys[k].entry)) return true;
    }
    return false;
}

/* The entries whose argument a tree change at `node` under `parent` can
   reach, in probe->hit. False past the probe's bounds. */
static bool has_tree_probe(HasTreeProbe *probe, lxb_dom_node_t *node,
                           lxb_dom_node_t *parent)
{
    const StyleHasPlan *plan = probe->plan;
    bool element = node->type == LXB_DOM_NODE_TYPE_ELEMENT;
    for (size_t i = 0; i < plan->entry_count; i++) {
        if (element ? plan->entries[i].tree_keyless
                    : plan->entries[i].text_keyless) has_tree_hit(probe, i);
    }
    if (element) {
        /* Elements the change adds or removes, and the siblings whose
           positions and adjacency move with them. */
        probe->role = HAS_FEATURE_NARROW;
        if (!has_tree_subtree(probe, node, true)) return false;
        probe->role = HAS_FEATURE_SIBLING;
        size_t siblings = 0;
        for (lxb_dom_node_t *child = parent->first_child; child != NULL;
             child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++siblings > HAS_TREE_SUBTREE_LIMIT) return false;
            (void) has_element_keys(plan, child, has_tree_feature, probe);
        }
        if (plan->wide_features) {
            probe->role = HAS_FEATURE_WIDE;
            if (!has_tree_subtree(probe, parent, false)) return false;
        }
    }
    if (parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        probe->role = HAS_FEATURE_EMPTY;
        (void) has_element_keys(plan, parent, has_tree_feature, probe);
    }
    return true;
}

bool stylesheet_tree_change_may_affect_has(const Stylesheet *sheet,
                                           lxb_dom_node_t *node,
                                           lxb_dom_node_t *parent)
{
    uint64_t entries = 0;
    uint32_t serial = 0;
    return stylesheet_tree_change_has_entries(sheet, node, parent, &entries,
                                              &serial);
}

bool stylesheet_tree_change_has_entries(const Stylesheet *sheet,
                                        lxb_dom_node_t *node,
                                        lxb_dom_node_t *parent,
                                        uint64_t *entries, uint32_t *serial)
{
    *entries = UINT64_MAX;
    *serial = 0;
    if (sheet == NULL || node == NULL || parent == NULL) return true;
    const StyleHasPlan *plan = has_plan_prepare(sheet);
    if (plan == NULL || plan->bounded) return true;
    if (plan->entry_count == 0) {
        *entries = 0;
        return false;
    }
    HasTreeProbe probe = {.plan = plan};
    if (!has_tree_probe(&probe, node, parent)) return true;
    *serial = plan->serial;
    *entries = 0;
    for (size_t i = 0; i < plan->entry_count; i++)
        if (HAS_BIT(probe.hit, i)) *entries |= style_has_entry_bloom_bit(i);
    if (!probe.any) return false;
    if (probe.unbounded) return true;
    /* Only an anchor the change can reach moves: the parent and its
       ancestors, and for sibling arguments the siblings of each. */
    size_t depth = 0, siblings = 0;
    for (lxb_dom_node_t *at = parent;
         at != NULL && at->type != LXB_DOM_NODE_TYPE_DOCUMENT;
         at = at->parent) {
        if (++depth > HAS_DEPTH_LIMIT) return true;
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT
            && has_element_keys(plan, at, has_tree_anchor, &probe))
            return true;
        if (!probe.sibling_region) continue;
        lxb_dom_node_t *first = at->parent == NULL ? at
                                                   : at->parent->first_child;
        for (lxb_dom_node_t *other = first; other != NULL;
             other = other->next) {
            if (other->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++siblings > HAS_SIBLING_LIMIT) return true;
            if (has_element_keys(plan, other, has_tree_anchor, &probe))
                return true;
        }
    }
    if (probe.sibling_region) {
        for (lxb_dom_node_t *child = parent->first_child; child != NULL;
             child = child->next) {
            if (child->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++siblings > HAS_SIBLING_LIMIT) return true;
            if (has_element_keys(plan, child, has_tree_anchor, &probe))
                return true;
        }
    }
    return false;
}

/* ---- Changes inside the document's head. ----------------------------- */

typedef struct {
    uint32_t hash[32];
    size_t count;
    bool overflow;
} HasKeyList;

static bool has_collect_key(void *context, uint32_t hash)
{
    HasKeyList *keys = context;
    if (keys->count == sizeof(keys->hash) / sizeof(keys->hash[0])) {
        keys->overflow = true;
        return true;
    }
    keys->hash[keys->count++] = hash;
    return false;
}

/* Whether `head` or `html` matches the selector text before some :has()
   of `text` (the compound holding it, or the outermost pseudo-class whose
   argument holds it, cut off; a universal compound when nothing or only a
   combinator precedes it). */
static bool has_prefix_matches(const char *text, size_t length,
                               lxb_dom_node_t *head, lxb_dom_node_t *html)
{
    char buffer[256];
    size_t item = 0, open = 0;
    unsigned depth = 0;
    char quote = 0;
    for (size_t at = 0; at < length; at++) {
        char c = text[at];
        if (quote != 0) {
            if (c == '\\') at++;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '\\') { at++; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == ',' && depth == 0) { item = at + 1; continue; }
        if (c == '(' || c == '[') { depth++; continue; }
        if (c == ')' || c == ']') { if (depth != 0) depth--; continue; }
        if (c != ':') continue;
        if (depth == 0) open = at;
        if (length - at < 5 || strncasecmp(text + at, ":has(", 5) != 0)
            continue;
        size_t cut = depth == 0 ? at : open;
        while (item < cut && isspace((unsigned char) text[item])) item++;
        size_t prefix = cut - item;
        if (prefix + 2 > sizeof(buffer)) return true;
        memcpy(buffer, text + item, prefix);
        if (prefix == 0 || isspace((unsigned char) buffer[prefix - 1])
            || strchr(">+~", buffer[prefix - 1]) != NULL)
            buffer[prefix++] = '*';
        if (style_selector_matches(head, buffer, prefix)
            || style_selector_matches(html, buffer, prefix)) return true;
    }
    return false;
}

bool stylesheet_head_change_reaches_outside(const Stylesheet *sheet,
                                            lxb_dom_node_t *head,
                                            lxb_dom_node_t *node)
{
    lxb_dom_node_t *html = head == NULL ? NULL : head->parent;
    const StyleHasPlan *plan = sheet == NULL || html == NULL
        || html->type != LXB_DOM_NODE_TYPE_ELEMENT
        ? NULL : has_plan_prepare(sheet);
    if (plan == NULL || plan->bounded) return true;
    /* Only the entries whose argument can see the changed node. */
    HasTreeProbe probe = {.plan = plan};
    bool all = node == NULL || node->parent == NULL
        || !has_tree_probe(&probe, node, node->parent);
    HasKeyList keys = {0};
    (void) has_element_keys(plan, head, has_collect_key, &keys);
    (void) has_element_keys(plan, html, has_collect_key, &keys);
    if (keys.overflow) return true;
    for (size_t i = 0; i < plan->entry_count; i++) {
        const StyleHasPlanEntry *entry = &plan->entries[i];
        if (!all && !HAS_BIT(probe.hit, i)) continue;
        if (entry->anchor_key != 0) {
            for (size_t k = 0; k < keys.count; k++)
                if (keys.hash[k] == entry->anchor_key) return true;
            continue;
        }
        size_t length = 0;
        const char *text = has_entry_text(sheet, entry, &length);
#ifndef TILEFINCH_NO_TRACE
        ((Stylesheet *) sheet)->head_script_selector_scans++;
#endif
        if (text == NULL || has_prefix_matches(text, length, head, html))
            return true;
    }
    return false;
}

/* ---- Keys for layout's :empty sibling scope. -------------------------- */

static size_t has_skip_back_block(const char *text, size_t at)
{
    /* `at` is just after a ')' or ']'; returns the index of its opener. */
    unsigned depth = 0;
    while (at > 0) {
        char value = text[--at];
        if (value == ')' || value == ']') depth++;
        else if ((value == '(' || value == '[') && --depth == 0) return at;
    }
    return 0;
}

uint32_t style_selector_compound_key_at(const char *text, size_t length,
                                        size_t at)
{
    bool attribute_key = false;
    for (unsigned level = 0; level <= HAS_NESTING_LIMIT && at < length;
         level++) {
        size_t begin = at;
        while (begin > 0) {
            char value = text[begin - 1];
            if (value == ')' || value == ']') {
                begin = has_skip_back_block(text, begin);
                continue;
            }
            if (isspace((unsigned char) value) || value == '>'
                || value == '+' || value == '~' || value == ','
                || value == '(') break;
            begin--;
        }
        size_t end = at;
        while (end < length) {
            char value = text[end];
            if (value == '(' || value == '[') {
                end = has_skip_block(text, end, length);
                continue;
            }
            if (isspace((unsigned char) value) || value == '>'
                || value == '+' || value == '~' || value == ','
                || value == ')') break;
            end++;
        }
        uint32_t key = has_compound_key(text, begin, end, &attribute_key);
        if (key != 0) return key;
        /* The only compound of a logical pseudo-class's item is the same
           element as the compound holding that pseudo-class. */
        if (begin == 0 || text[begin - 1] != '(' || end >= length
            || (text[end] != ',' && text[end] != ')')) return 0;
        size_t name_end = begin - 1, name_begin = name_end;
        while (name_begin > 0
               && has_ident_byte((unsigned char) text[name_begin - 1]))
            name_begin--;
        if (name_begin == 0 || text[name_begin - 1] != ':') return 0;
        HasPseudo pseudo = {.name_begin = name_begin, .name_end = name_end};
        if (!has_pseudo_is_logical(text, &pseudo)) return 0;
        at = name_begin - 1;
    }
    return 0;
}

typedef struct {
    uint32_t hash;
    bool found;
} HasKeySearch;

static bool has_key_search(void *context, uint32_t hash)
{
    HasKeySearch *search = context;
    search->found = search->hash == hash;
    return search->found;
}

bool style_element_carries_key(const lxb_dom_node_t *node, uint32_t key)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    /* Any plan whose attribute keys are on: every key kind is offered. */
    StyleHasPlan plan = {.attribute_keys = true};
    HasKeySearch search = {.hash = key};
    return has_element_keys(&plan, node, has_key_search, &search);
}

typedef struct {
    const uint32_t *keys;
    size_t count;
} HasKeySetSearch;

static bool has_key_set_search(void *context, uint32_t hash)
{
    const HasKeySetSearch *search = context;
    size_t low = 0, high = search->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (search->keys[middle] < hash) low = middle + 1u;
        else high = middle;
    }
    return low < search->count && search->keys[low] == hash;
}

typedef struct {
    const StyleKeyNames *table;
    size_t count;
    uint64_t names;
} HasKeyNames;

static bool has_key_names_visit(void *context, uint32_t hash)
{
    HasKeyNames *search = context;
    size_t low = 0, high = search->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (search->table[middle].key < hash) low = middle + 1u;
        else high = middle;
    }
    for (; low < search->count && search->table[low].key == hash; low++)
        search->names |= search->table[low].names;
    return false;
}

uint64_t style_element_key_names(const lxb_dom_node_t *node,
                                 const StyleKeyNames *table, size_t count)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || count == 0) return 0;
    StyleHasPlan plan = {.attribute_keys = true};
    HasKeyNames search = {.table = table, .count = count};
    (void) has_element_keys(&plan, node, has_key_names_visit, &search);
    return search.names;
}

typedef struct {
    const uint32_t *keys;
    size_t count;
} HasKeyListSearch;

static bool has_key_list_search(void *context, uint32_t hash)
{
    const HasKeyListSearch *search = context;
    for (size_t i = 0; i < search->count; i++)
        if (search->keys[i] == hash) return true;
    return false;
}

bool style_element_carries_any_key_unsorted(const lxb_dom_node_t *node,
                                            const uint32_t *keys,
                                            size_t count)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || keys == NULL || count == 0) return false;
    StyleHasPlan plan = {.attribute_keys = true};
    HasKeyListSearch search = {.keys = keys, .count = count};
    return has_element_keys(&plan, node, has_key_list_search, &search);
}

bool style_element_carries_any_key(const lxb_dom_node_t *node,
                                   const uint32_t *keys, size_t count)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || count == 0) return false;
    StyleHasPlan plan = {.attribute_keys = true};
    HasKeySetSearch search = {.keys = keys, .count = count};
    return has_element_keys(&plan, node, has_key_set_search, &search);
}

/* ---- Structural reach, for layout's child-list scope. ----------------- */

static void has_structure_add(StyleStructureKeys *out, uint32_t key,
                              bool predecessor)
{
    if (key == 0) {
        out->any = true;
        return;
    }
    for (size_t i = 0; i < out->count; i++) {
        if (out->keys[i] == key) {
            out->predecessor[i] = out->predecessor[i] || predecessor;
            return;
        }
    }
    if (out->count == STYLE_STRUCTURE_KEY_LIMIT) {
        out->any = true;
        return;
    }
    out->keys[out->count] = key;
    out->predecessor[out->count] = predecessor;
    out->count++;
}

static void has_complex_structure(const char *text, size_t at, size_t end,
                                  unsigned depth, StyleStructureKeys *out);

static void has_list_structure(const char *text, size_t at, size_t end,
                               unsigned depth, StyleStructureKeys *out)
{
    if (depth > HAS_NESTING_LIMIT) {
        out->reaches = out->any = true;
        return;
    }
    while (at < end) {
        size_t item_end = has_item_end(text, at, end);
        has_complex_structure(text, at, item_end, depth, out);
        at = item_end + 1;
    }
}

/* A sibling-position test (a positional pseudo-class, or a sibling
   combinator) before a descendant or child combinator lets a changed
   child list restyle a sibling's descendants. The keys name the children
   that can carry such a test: the positional compound's, the compound
   after a sibling combinator, or, when that one has none, the compound
   before it (a predecessor, possibly the child that left). :has()
   arguments are the :has() plan's and are skipped. */
static void has_complex_structure(const char *text, size_t at, size_t end,
                                  unsigned depth, StyleStructureKeys *out)
{
    HasCompound compounds[HAS_COMPOUND_LIMIT];
    bool overflow = false, unused = false;
    size_t count = has_compounds(text, at, end, compounds,
                                 HAS_COMPOUND_LIMIT, &overflow);
    if (overflow) {
        out->reaches = out->any = true;
        return;
    }
    for (size_t i = 0; i < count; i++) {
        bool descends_after = false;
        for (size_t j = i + 1; j < count && !descends_after; j++)
            descends_after = compounds[j].combinator == ' '
                             || compounds[j].combinator == '>';
        bool positional = false;
        HasPseudo pseudo;
        size_t cursor = compounds[i].begin;
        while (has_next_pseudo(text, &cursor, compounds[i].end, &pseudo)) {
            size_t b = pseudo.name_begin, e = pseudo.name_end;
            if (has_name_is(text, b, e, "has")) continue;
            if ((e - b > 6 && strncasecmp(text + b, "first-", 6) == 0)
                || (e - b > 5 && strncasecmp(text + b, "last-", 5) == 0)
                || (e - b > 4 && strncasecmp(text + b, "nth-", 4) == 0)
                || (e - b > 5 && strncasecmp(text + b, "only-", 5) == 0))
                positional = true;
            if (!pseudo.functional) continue;
            if (has_pseudo_is_logical(text, &pseudo)) {
                if (has_compound_positional(text, pseudo.argument_begin,
                                            pseudo.argument_end))
                    positional = true;
                has_list_structure(text, pseudo.argument_begin,
                                   pseudo.argument_end, depth + 1, out);
            } else if (e - b > 4 && strncasecmp(text + b, "nth-", 4) == 0) {
                size_t of = has_nth_of(text, pseudo.argument_begin,
                                       pseudo.argument_end);
                if (of < pseudo.argument_end)
                    has_list_structure(text, of, pseudo.argument_end,
                                       depth + 1, out);
            }
        }
        if (positional && descends_after) {
            out->reaches = true;
            has_structure_add(out, has_compound_key(
                text, compounds[i].begin, compounds[i].end, &unused), false);
        }
        if (i + 1 < count && (compounds[i + 1].combinator == '+'
                              || compounds[i + 1].combinator == '~')) {
            bool later = false;
            for (size_t j = i + 2; j < count && !later; j++)
                later = compounds[j].combinator == ' '
                        || compounds[j].combinator == '>';
            if (!later) continue;
            out->reaches = true;
            uint32_t next = has_compound_key(text, compounds[i + 1].begin,
                                             compounds[i + 1].end, &unused);
            if (next != 0) {
                has_structure_add(out, next, false);
            } else {
                has_structure_add(out, has_compound_key(
                    text, compounds[i].begin, compounds[i].end, &unused),
                    true);
            }
        }
    }
}

void style_selector_structure_keys(const char *selector, size_t length,
                                   StyleStructureKeys *keys)
{
    if (selector != NULL && keys != NULL)
        has_list_structure(selector, 0, length, 0, keys);
}

/* ---- Tests counting from the end, for appended hidden siblings. ------- */

static bool has_trailing_name(const char *text, size_t begin, size_t end)
{
    return (end - begin > 5 && strncasecmp(text + begin, "last-", 5) == 0)
        || (end - begin > 9
            && strncasecmp(text + begin, "nth-last-", 9) == 0)
        || (end - begin > 5 && strncasecmp(text + begin, "only-", 5) == 0);
}

/* Whether [at, end) holds a pseudo-class whose name starts with one of
   `names` (ASCII case-insensitive): a scan for ':' rather than a substring
   search per name, since these run over every selector of a sheet. */
static bool has_text_pseudo(const char *text, size_t at, size_t end,
                            const char *const *names, size_t count)
{
    for (; at < end; at++) {
        if (text[at] != ':') continue;
        for (size_t i = 0; i < count; i++) {
            size_t length = strlen(names[i]);
            if (end - (at + 1) >= length
                && strncasecmp(text + at + 1, names[i], length) == 0)
                return true;
        }
    }
    return false;
}

static bool has_text_trailing(const char *text, size_t at, size_t end)
{
    static const char *const names[] = {"last-", "nth-last-", "only-"};
    return has_text_pseudo(text, at, end, names,
                           sizeof(names) / sizeof(names[0]));
}

static bool has_text_positional_or_has(const char *text, size_t at,
                                       size_t end, bool *nested_has)
{
    static const char *const positional[] = {
        "first-", "last-", "nth-", "only-", "empty", "blank"
    };
    static const char *const has[] = {"has("};
    *nested_has = has_text_pseudo(text, at, end, has, 1);
    return has_text_pseudo(text, at, end, positional,
                           sizeof(positional) / sizeof(positional[0]));
}

/* 1 when the compound [at, end) holds a trailing test outside :has() for
   the element it matches, 0 when not, -1 when one sits where the compound
   alone does not place it (a logical item with combinators, `of S`, an
   unknown functional pseudo-class). */
static int has_compound_trailing(const char *text, size_t at, size_t end,
                                 unsigned depth)
{
    if (depth > HAS_NESTING_LIMIT) return -1;
    int found = 0;
    HasPseudo pseudo;
    size_t cursor = at;
    while (has_next_pseudo(text, &cursor, end, &pseudo)) {
        size_t b = pseudo.name_begin, e = pseudo.name_end;
        if (has_name_is(text, b, e, "has")) continue;
        if (has_trailing_name(text, b, e)) {
            found = 1;
            if (pseudo.functional
                && has_text_trailing(text, pseudo.argument_begin,
                                     pseudo.argument_end)) return -1;
            continue;
        }
        if (!pseudo.functional
            || !has_text_trailing(text, pseudo.argument_begin,
                                  pseudo.argument_end)) continue;
        if (!has_pseudo_is_logical(text, &pseudo)) return -1;
        for (size_t item = pseudo.argument_begin;
             item < pseudo.argument_end;) {
            size_t item_end = has_item_end(text, item,
                                           pseudo.argument_end);
            HasCompound compounds[HAS_COMPOUND_LIMIT];
            bool overflow = false;
            size_t count = has_compounds(text, item, item_end, compounds,
                                         HAS_COMPOUND_LIMIT, &overflow);
            for (size_t i = 0; i < count && !overflow; i++) {
                int inner = has_compound_trailing(
                    text, compounds[i].begin, compounds[i].end, depth + 1);
                if (inner < 0 || (inner > 0 && count != 1)) return -1;
                if (inner > 0) found = 1;
            }
            if (overflow) return -1;
            item = item_end + 1;
        }
    }
    return found;
}

static void has_trailing_add(StyleTrailingKeys *out, uint32_t key,
                             size_t depth, bool at_least)
{
    if (key == 0 || depth > UINT8_MAX) {
        out->any = true;
        return;
    }
    for (size_t i = 0; i < out->count; i++) {
        if (out->keys[i] == key && out->depth[i] == depth
            && out->at_least[i] == at_least) return;
    }
    if (out->count == STYLE_TRAILING_KEY_LIMIT) {
        out->any = true;
        return;
    }
    out->keys[out->count] = key;
    out->depth[out->count] = (uint8_t) depth;
    out->at_least[out->count] = at_least;
    out->count++;
}

void style_selector_trailing_keys(const char *selector, size_t length,
                                  StyleTrailingKeys *keys)
{
    if (selector == NULL || keys == NULL || keys->any
        || !has_text_trailing(selector, 0, length)) return;
    for (size_t at = 0; at < length;) {
        size_t item_end = has_item_end(selector, at, length);
        HasCompound compounds[HAS_COMPOUND_LIMIT];
        bool overflow = false;
        size_t count = has_compounds(selector, at, item_end, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
        if (overflow) {
            keys->any = true;
            return;
        }
        for (size_t i = 0; i < count; i++) {
            int trailing = has_compound_trailing(
                selector, compounds[i].begin, compounds[i].end, 0);
            if (trailing == 0) continue;
            if (trailing < 0) {
                keys->any = true;
                return;
            }
            bool unused = false;
            uint32_t key = has_compound_key(selector, compounds[i].begin,
                                            compounds[i].end, &unused);
            if (key != 0) {
                has_trailing_add(keys, key, 0, false);
                continue;
            }
            /* Place it by the nearest keyed ancestor compound; a sibling
               combinator on the way leaves it unplaced. */
            bool at_least = false, placed = false;
            for (size_t j = i; j > 0 && !placed; j--) {
                char combinator = compounds[j].combinator;
                if (combinator != ' ' && combinator != '>') break;
                if (combinator == ' ') at_least = true;
                key = has_compound_key(selector, compounds[j - 1].begin,
                                       compounds[j - 1].end, &unused);
                if (key == 0) continue;
                has_trailing_add(keys, key, i - j + 1u, at_least);
                placed = true;
            }
            if (!placed) {
                keys->any = true;
                return;
            }
        }
        at = item_end + 1;
    }
}

/* ---- :has() argument keys, for inserted subtrees. --------------------- */

static void has_argument_bloom_add(uint32_t *bloom, uint32_t key)
{
    const uint32_t bits = STYLE_HAS_ARGUMENT_BLOOM_WORDS * 32u;
    uint32_t a = key % bits, b = (key >> 11) % bits;
    bloom[a >> 5] |= UINT32_C(1) << (a & 31u);
    bloom[b >> 5] |= UINT32_C(1) << (b & 31u);
}

static bool has_argument_bloom_test(const uint32_t *bloom, uint32_t key)
{
    const uint32_t bits = STYLE_HAS_ARGUMENT_BLOOM_WORDS * 32u;
    uint32_t a = key % bits, b = (key >> 11) % bits;
    return (bloom[a >> 5] & (UINT32_C(1) << (a & 31u))) != 0
        && (bloom[b >> 5] & (UINT32_C(1) << (b & 31u))) != 0;
}

static void has_argument_anchor_add(StyleHasArgumentKeys *keys,
                                    uint32_t key, uint8_t region)
{
    if (key == 0 || region == 0) {
        keys->any = true;
        return;
    }
    for (size_t i = 0; i < keys->anchor_count; i++) {
        if (keys->anchors[i] == key) {
            keys->anchor_regions[i] |= region;
            return;
        }
    }
    if (keys->anchor_count == STYLE_HAS_ANCHOR_LIMIT) {
        keys->any = true;
        return;
    }
    keys->anchors[keys->anchor_count] = key;
    keys->anchor_regions[keys->anchor_count] = region;
    keys->anchor_count++;
}

/* Logical selectors can inspect ancestors/siblings outside the :has()
   anchor even when its relative selector starts with a descendant. Such
   dependencies cannot be represented by the anchor-region summary. */
static bool has_argument_compound_escapes(const char *text, size_t at,
                                         size_t end, unsigned depth)
{
    if (depth > HAS_NESTING_LIMIT) return true;
    HasPseudo pseudo;
    while (has_next_pseudo(text, &at, end, &pseudo)) {
        if (!pseudo.functional) continue;
        if (has_name_is(text, pseudo.name_begin, pseudo.name_end, "has"))
            return true;
        if ((has_pseudo_is_logical(text, &pseudo)
             && has_list_combines(text, pseudo.argument_begin,
                                  pseudo.argument_end))
            || has_argument_compound_escapes(
                   text, pseudo.argument_begin, pseudo.argument_end,
                   depth + 1u)) return true;
    }
    return false;
}

/* One :has() argument [at, end): plain items go into the Bloom set; the
   regions the others reach from the anchor are returned. */
static uint8_t has_argument_keys_of(const char *text, size_t at, size_t end,
                                    StyleHasArgumentKeys *keys)
{
    uint32_t plain[HAS_COMPOUND_LIMIT];
    uint8_t regions = 0;
    while (at < end && !keys->any) {
        size_t item_end = has_item_end(text, at, end);
        HasCompound compounds[HAS_COMPOUND_LIMIT];
        bool overflow = false;
        size_t count = has_compounds(text, at, item_end, compounds,
                                     HAS_COMPOUND_LIMIT, &overflow);
        if (overflow) {
            keys->any = true;
            return 0;
        }
        bool simple = true;
        for (size_t i = 0; i < count; i++) {
            bool unused = false;
            plain[i] = has_compound_key(text, compounds[i].begin,
                                        compounds[i].end, &unused);
            bool nested_has = false;
            bool positional = has_text_positional_or_has(
                text, compounds[i].begin, compounds[i].end, &nested_has);
            if (nested_has || has_argument_compound_escapes(
                    text, compounds[i].begin, compounds[i].end, 0)) {
                keys->any = true;
                return 0;
            }
            if (plain[i] == 0
                || compounds[i].combinator == '+'
                || compounds[i].combinator == '~'
                || positional)
                simple = false;
        }
        if (simple && count != 0) {
            has_argument_bloom_add(keys->first, plain[0]);
            has_argument_bloom_add(keys->last, plain[count - 1]);
        } else if (count != 0) {
            char lead = compounds[0].combinator;
            regions |= lead == '+' || lead == '~'
                ? STYLE_HAS_REGION_SIBLINGS : STYLE_HAS_REGION_DESCENDANTS;
        }
        at = item_end + 1;
    }
    return regions;
}

/* Anchors for the :has() at `at`: the key every element of its compound
   carries, or else the keys of a :is()/:where() alternative list of single
   keyed compounds in it (`:is(.a, .b):has(...)`). */
static void has_argument_anchors_add(const char *text, size_t length,
                                     size_t at, uint8_t regions,
                                     StyleHasArgumentKeys *keys)
{
    uint32_t key = style_selector_compound_key_at(text, length, at);
    if (key != 0) {
        has_argument_anchor_add(keys, key, regions);
        return;
    }
    size_t begin = at;
    while (begin > 0) {
        char value = text[begin - 1];
        if (value == ')' || value == ']') {
            begin = has_skip_back_block(text, begin);
            continue;
        }
        if (isspace((unsigned char) value) || value == '>' || value == '+'
            || value == '~' || value == ',' || value == '(') break;
        begin--;
    }
    HasPseudo pseudo;
    size_t cursor = begin;
    while (has_next_pseudo(text, &cursor, at, &pseudo)) {
        size_t b = pseudo.name_begin, e = pseudo.name_end;
        if (!pseudo.functional || !has_pseudo_is_logical(text, &pseudo)
            || has_name_is(text, b, e, "not")) continue;
        uint32_t alternatives[8];
        size_t count = 0;
        bool keyed = true;
        for (size_t item = pseudo.argument_begin;
             keyed && item < pseudo.argument_end;) {
            size_t item_end = has_item_end(text, item, pseudo.argument_end);
            HasCompound compounds[2];
            bool overflow = false, unused = false;
            size_t parts = has_compounds(text, item, item_end, compounds,
                                         sizeof(compounds) / sizeof(compounds[0]),
                                         &overflow);
            keyed = parts == 1 && !overflow
                && count < sizeof(alternatives) / sizeof(alternatives[0]);
            if (keyed) {
                alternatives[count] = has_compound_key(
                    text, compounds[0].begin, compounds[0].end, &unused);
                keyed = alternatives[count++] != 0;
            }
            item = item_end + 1;
        }
        if (!keyed || count == 0) continue;
        for (size_t i = 0; i < count; i++)
            has_argument_anchor_add(keys, alternatives[i], regions);
        return;
    }
    keys->any = true;
}

void style_selector_has_argument_keys(const char *selector, size_t length,
                                      StyleHasArgumentKeys *keys)
{
    if (selector == NULL || keys == NULL || keys->any) return;
    for (size_t at = 0; at + 5 <= length && !keys->any; at++) {
        if (selector[at] == '"' || selector[at] == '\'') {
            at = has_skip_string(selector, at, length) - 1;
            continue;
        }
        if (selector[at] == '\\') {
            at++;
            continue;
        }
        if (selector[at] != ':'
            || strncasecmp(selector + at + 1, "has(", 4) != 0) continue;
        size_t close = has_skip_block(selector, at + 4, length);
        size_t argument_end = close > at + 5 && selector[close - 1] == ')'
            ? close - 1 : close;
        uint8_t regions = has_argument_keys_of(selector, at + 5,
                                               argument_end, keys);
        if (regions != 0 && !keys->any)
            has_argument_anchors_add(selector, length, at, regions, keys);
        /* Arguments are scanned whole; a nested :has() set `any`. */
        at = close - 1;
    }
}

static bool has_argument_bloom_visit(void *context, uint32_t hash)
{
    return has_argument_bloom_test(context, hash);
}

typedef struct {
    const StyleHasArgumentKeys *keys;
    uint8_t region;
} HasAnchorSearch;

static bool has_anchor_visit(void *context, uint32_t hash)
{
    const HasAnchorSearch *search = context;
    for (size_t i = 0; i < search->keys->anchor_count; i++) {
        if (search->keys->anchors[i] == hash
            && (search->keys->anchor_regions[i] & search->region) != 0)
            return true;
    }
    return false;
}

bool style_has_argument_keys_reach(const StyleHasArgumentKeys *keys,
                                   const lxb_dom_node_t *subtree,
                                   const lxb_dom_node_t *parent)
{
    if (keys == NULL || parent == NULL || keys->any
        || parent->type != LXB_DOM_NODE_TYPE_ELEMENT) return true;
    StyleHasPlan plan = {.attribute_keys = true};
    size_t visited = 0;
    bool last = false, first = false;
    const lxb_dom_node_t *at = subtree;
    while (at != NULL && !(last && first)) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (++visited > 256u) return true;
            last = last || has_element_keys(&plan, at,
                                            has_argument_bloom_visit,
                                            (void *) keys->last);
            first = first || has_element_keys(&plan, at,
                                              has_argument_bloom_visit,
                                              (void *) keys->first);
        }
        if (at->first_child != NULL && at->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            at = at->first_child;
            continue;
        }
        while (at != subtree && at->next == NULL) at = at->parent;
        if (at == subtree) break;
        at = at->next;
    }
    HasAnchorSearch descendants = {keys, STYLE_HAS_REGION_DESCENDANTS},
                    siblings = {keys, STYLE_HAS_REGION_SIBLINGS};
    size_t steps = 0;
    for (const lxb_dom_node_t *node = parent; node != NULL
         && node->type == LXB_DOM_NODE_TYPE_ELEMENT; node = node->parent) {
        if (++steps > HAS_DEPTH_LIMIT) return true;
        if (last && !first)
            first = has_element_keys(&plan, node, has_argument_bloom_visit,
                                     (void *) keys->first);
        if (keys->anchor_count != 0
            && has_element_keys(&plan, node, has_anchor_visit, &descendants))
            return true;
    }
    if (last && first) return true;
    if (keys->anchor_count == 0) return false;
    /* Sibling arguments: anchors before the subtree in place, or (for a
       change among the parent's children it cannot place) any child. */
    steps = 0;
    bool placed = subtree != NULL && subtree->parent == parent;
    for (const lxb_dom_node_t *earlier = placed ? subtree->prev
                                                : parent->last_child;
         earlier != NULL; earlier = earlier->prev) {
        if (earlier->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (++steps > HAS_SIBLING_LIMIT
            || has_element_keys(&plan, earlier, has_anchor_visit, &siblings))
            return true;
    }
    for (const lxb_dom_node_t *node = parent; node != NULL
         && node->type == LXB_DOM_NODE_TYPE_ELEMENT; node = node->parent) {
        for (const lxb_dom_node_t *earlier = node->prev; earlier != NULL;
             earlier = earlier->prev) {
            if (earlier->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            if (++steps > HAS_SIBLING_LIMIT
                || has_element_keys(&plan, earlier, has_anchor_visit,
                                    &siblings)) return true;
        }
    }
    return false;
}

bool style_trailing_keys_reach(const StyleTrailingKeys *keys,
                               const lxb_dom_node_t *sibling,
                               const lxb_dom_node_t *parent)
{
    if (keys == NULL) return true;
    if (keys->any) return true;
    for (size_t i = 0; i < keys->count; i++) {
        if (keys->depth[i] == 0) {
            if (style_element_carries_key(sibling, keys->keys[i]))
                return true;
            continue;
        }
        size_t level = 1;
        for (const lxb_dom_node_t *at = parent;
             at != NULL && at->type == LXB_DOM_NODE_TYPE_ELEMENT
             && level <= HAS_DEPTH_LIMIT;
             at = at->parent, level++) {
            if (level < keys->depth[i]) continue;
            if (style_element_carries_key(at, keys->keys[i])) return true;
            if (!keys->at_least[i]) break;
        }
    }
    return false;
}
