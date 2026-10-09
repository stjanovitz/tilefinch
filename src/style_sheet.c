/* Stylesheet construction and storage: selector interning, custom
   properties, declaration/rule storage, @media/@supports/@layer,
   @font-face and web-font collection, rule ordering and the rule
   index, and the public stylesheet_* build/query/destroy API.
   Split out of style.c. */

#include "style_internal.h"
#include "style_cache_internal.h"
#include "tilefinch_test_faults.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <lexbor/tag/tag.h>

#include "tilefinch/platform.h"
#include "tilefinch/url.h"

#define STYLE_VARIABLE_CACHE_DEFAULT_ENTRIES 256u
/* The getComputedStyle lease is retained for the page's lifetime, unlike
   a layout build's table. ChatGPT send (1251 reads, 25 clears): 64 entries
   hit 801 lookups vs 818 at 256 in the same 17 ms (host), for 10 KiB
   instead of 40 KiB on the PSP (160-byte entries). A whole-document sweep
   (2145 reads, no clears) is the case that wants more: 24.5 vs 21.8 ms. */
#define STYLE_VARIABLE_CACHE_LEASE_ENTRIES 64u
#define STYLE_VARIABLE_CACHE_MIN_ENTRIES 32u
#define STYLE_VARIABLE_CACHE_MAX_ENTRIES 256u
#define STYLE_VARIABLE_CACHE_PROBES 8u

#define STYLE_PARSED_IR_MAGIC UINT32_C(0x54464952)
#define STYLE_PARSED_IR_VERSION UINT16_C(3)
#define STYLE_PARSED_IR_MAX_BYTES (256u * 1024u)
#define STYLE_PARSED_IR_CONTAINER_DEPTH 8u
#define STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES UINT16_C(1)
/* Built while prefers-color-scheme answered dark; the IR holds rules
   selected by media queries, so it is reused only under the same answer. */
#define STYLE_PARSED_IR_PREFERS_DARK UINT16_C(2)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t operation_count;
    uint32_t payload_bytes;
} StyleParsedIrHeader;

typedef struct {
    uint32_t declaration_length;
    uint16_t selector_length;
    /* STYLE_PARSED_IR_RULES, or a container scope: BEGIN carries the
       @container condition as its "selector" and no declarations; END
       carries one ignored byte. Only builders that allow container scopes
       (constructed sheets) record them. */
    uint16_t kind;
} StyleParsedIrOperation;

#define STYLE_PARSED_IR_RULES UINT16_C(0)
#define STYLE_PARSED_IR_CONTAINER_BEGIN UINT16_C(1)
#define STYLE_PARSED_IR_CONTAINER_END UINT16_C(2)
#define STYLE_PARSED_IR_KEYFRAMES UINT16_C(3)

/* Optional scheduler metadata. Refusal leaves ordinary styling intact and
   the bootstrap can still discover inline motion from the DOM. */
static void stylesheet_note_motion_source(
    Stylesheet *sheet, const char *header, size_t header_length,
    const char *body, size_t body_length, bool keyframes)
{
    enum { maximum_bytes = 64 * 1024, maximum_rule_bytes = 4096 };
    if (sheet == NULL || sheet->budget == NULL
        || sheet->motion_css_bounded_out || sheet->motion_css_source_blocked
        || sheet->parsing_scoped_source
        || sheet->current_container_query != 0) return;
    char declarations[maximum_rule_bytes];
    size_t kept = 0;
    if (!keyframes) {
        for (size_t at = 0; at < body_length;) {
            at = skip_css_space_and_comments(body, body_length, at);
            if (at == body_length) break;
            size_t end = find_declaration_end(body, body_length, at);
            const char *item = body + at;
            size_t length = end - at;
            trim(&item, &length);
            const char *colon = memchr(item, ':', length);
            if (colon != NULL) {
                const char *name = item;
                size_t name_length = (size_t) (colon - item);
                trim(&name, &name_length);
                bool animation = (name_length == 9
                    && span_case_equal(name, name_length, "animation"))
                    || (name_length > 10
                        && span_case_equal(name, 10, "animation-"))
                    || ((name_length == 17
                         || (name_length > 17 && name[17] == '-'))
                        && span_case_equal(name, 17, "-webkit-animation"));
                if (animation) {
                    if (length >= sizeof(declarations) - kept) return;
                    memcpy(declarations + kept, item, length);
                    kept += length;
                    declarations[kept++] = ';';
                }
            }
            at = end + (end < body_length);
        }
        if (kept == 0) return;
        body = declarations;
        body_length = kept;
    }
    if (header_length > maximum_rule_bytes
        || body_length > maximum_rule_bytes
        || (keyframes ? sheet->motion_css_keyframes >= 16
                      : sheet->motion_css_rules >= 128)) {
        sheet->motion_css_bounded_out = true;
        return;
    }
    size_t added = header_length + body_length + 3u;
    if (added > maximum_bytes - sheet->motion_css_length) {
        sheet->motion_css_bounded_out = true;
        return;
    }
    size_t needed = sheet->motion_css_length + added + 1u;
    if (needed > sheet->motion_css_capacity) {
        size_t capacity = sheet->motion_css_capacity != 0
            ? sheet->motion_css_capacity : 512u;
        while (capacity < needed && capacity < maximum_bytes + 1u) {
            capacity = capacity > (maximum_bytes + 1u) / 2u
                ? maximum_bytes + 1u : capacity * 2u;
        }
        char *source = budget_realloc(sheet->budget, sheet->motion_css, capacity);
        if (source == NULL) {
            sheet->motion_css_bounded_out = true;
            return;
        }
        sheet->motion_css = source;
        sheet->motion_css_capacity = capacity;
    }
    char *out = sheet->motion_css + sheet->motion_css_length;
    memcpy(out, header, header_length);
    out[header_length] = '{';
    memcpy(out + header_length + 1u, body, body_length);
    out[header_length + body_length + 1u] = '}';
    out[header_length + body_length + 2u] = '\n';
    sheet->motion_css_length += added;
    sheet->motion_css[sheet->motion_css_length] = '\0';
    if (keyframes) sheet->motion_css_keyframes++;
    else sheet->motion_css_rules++;
}

typedef struct {
    Budget *budget;
    unsigned char *data;
    size_t length;
    size_t capacity;
    size_t maximum_length;
    size_t operation_count;
    bool eligible;
    bool has_motion_keyframes;
    /* @container scopes are recorded as operations instead of
       disqualifying the IR (replay re-registers each query). */
    bool container_scopes;
} StyleParsedIrBuilder;

/* Stylesheet construction and mutation are confined to the browser thread.
   A process-wide sequence therefore gives stable-address sheets a cheap
   semantic identity without adding synchronization to the PSP hot path. */
static uint64_t stylesheet_generation_sequence;

typedef struct {
    const char *declarations;
    size_t declaration_length;
    uint32_t declaration_indices[2];
    uint8_t slots;
} StyleFontDeclarationFixup;

/* One context spans every authored CSS input in a stylesheet batch. Font
   faces are therefore discovered by the ordinary rule walk, while the small
   set of declarations that mention font-family can be finalized after later
   faces have been seen. The source spans are borrowed only until finish. */
typedef struct {
    Stylesheet *sheet;
    StyleFontDeclarationFixup *font_fixups;
    size_t font_fixup_count;
    size_t font_fixup_capacity;
    StyleParsedIrBuilder *parsed_ir;
    bool discover_font_faces;
} StyleCssParseContext;

/* Defined by queries_selectors.inc. Declaration retention is included first,
   so keep this local forward declaration beside the include boundary. */
static void selector_assign_fast_key(StyleRule *rule, const char *text,
                                     size_t length);

static void style_parsed_ir_builder_discard(StyleParsedIrBuilder *builder)
{
    builder->eligible = false;
    budget_free(builder->budget, builder->data);
    builder->data = NULL;
    builder->capacity = 0;
}

static void style_parsed_ir_builder_disqualify(StyleCssParseContext *context)
{
    if (context != NULL && context->parsed_ir != NULL) {
        style_parsed_ir_builder_discard(context->parsed_ir);
    }
}

static bool style_parsed_ir_builder_append(
    StyleCssParseContext *context, uint16_t kind, const char *selectors,
    size_t selector_length, const char *declarations,
    size_t declaration_length);

static bool style_parsed_ir_builder_record(
    StyleCssParseContext *context, const char *selectors,
    size_t selector_length, const char *declarations,
    size_t declaration_length)
{
    return style_parsed_ir_builder_append(
        context, STYLE_PARSED_IR_RULES, selectors, selector_length,
        declarations, declaration_length);
}

/* Records entering (`condition` non-NULL) or leaving an @container scope,
   or disqualifies the IR when the builder does not take container scopes. */
static void style_parsed_ir_builder_container(
    StyleCssParseContext *context, const char *condition,
    size_t condition_length)
{
    StyleParsedIrBuilder *builder = context == NULL ? NULL
        : context->parsed_ir;
    if (builder == NULL || !builder->eligible) return;
    if (!builder->container_scopes) {
        style_parsed_ir_builder_discard(builder);
        return;
    }
    static const char end_marker[] = "}";
    (void) style_parsed_ir_builder_append(
        context,
        condition != NULL ? STYLE_PARSED_IR_CONTAINER_BEGIN
                          : STYLE_PARSED_IR_CONTAINER_END,
        condition != NULL ? condition : end_marker,
        condition != NULL ? condition_length : 1u, "", 0);
}

static bool style_parsed_ir_builder_append(
    StyleCssParseContext *context, uint16_t kind, const char *selectors,
    size_t selector_length, const char *declarations,
    size_t declaration_length)
{
    StyleParsedIrBuilder *builder = context == NULL ? NULL
        : context->parsed_ir;
    if (builder == NULL || !builder->eligible) return true;
    if (selectors == NULL || declarations == NULL
        || selector_length == 0 || selector_length > UINT16_MAX
        || declaration_length > UINT32_MAX
        || sizeof(StyleParsedIrOperation) > SIZE_MAX - selector_length
        || sizeof(StyleParsedIrOperation) + selector_length
               > SIZE_MAX - declaration_length) {
        style_parsed_ir_builder_discard(builder);
        return true;
    }
    size_t added = sizeof(StyleParsedIrOperation) + selector_length
        + declaration_length;
    if (builder->length > builder->maximum_length
        || added > builder->maximum_length - builder->length) {
        style_parsed_ir_builder_discard(builder);
        return true;
    }
    size_t required = builder->length + added;
    if (required > builder->capacity) {
        size_t capacity = builder->capacity == 0 ? 4096u
            : builder->capacity;
        if (capacity > builder->maximum_length) {
            capacity = builder->maximum_length;
        }
        while (capacity < required) {
            size_t next = capacity > builder->maximum_length / 2u
                ? builder->maximum_length : capacity * 2u;
            if (next <= capacity) {
                style_parsed_ir_builder_discard(builder);
                return true;
            }
            capacity = next;
        }
        unsigned char *grown = budget_realloc(
            builder->budget, builder->data, capacity);
        if (grown == NULL) {
            style_parsed_ir_builder_discard(builder);
            return true;
        }
        builder->data = grown;
        builder->capacity = capacity;
    }
    StyleParsedIrOperation operation = {
        .declaration_length = (uint32_t) declaration_length,
        .selector_length = (uint16_t) selector_length,
        .kind = kind
    };
    memcpy(builder->data + builder->length, &operation, sizeof(operation));
    builder->length += sizeof(operation);
    memcpy(builder->data + builder->length, selectors, selector_length);
    builder->length += selector_length;
    memcpy(builder->data + builder->length, declarations,
           declaration_length);
    builder->length += declaration_length;
    builder->operation_count++;
    return true;
}


#include "style_sheet/declarations.inc"
#include "style_sheet/transitions.inc"
#include "style_sheet/queries_selectors.inc"
#include "style_sheet/parser.inc"

static int compare_rules(const void *left, const void *right)
{
    const StyleRule *a = left;
    const StyleRule *b = right;
    if (a->origin != b->origin) return a->origin < b->origin ? -1 : 1;
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->layer != b->layer) {
        if (!a->important) {
            if (a->layer == UINT_MAX) return 1;
            if (b->layer == UINT_MAX) return -1;
            return (a->layer >> 8) < (b->layer >> 8) ? -1 : 1;
        }
        if (a->layer == UINT_MAX) return -1;
        if (b->layer == UINT_MAX) return 1;
        return (a->layer >> 8) > (b->layer >> 8) ? -1 : 1;
    }
    if (a->specificity != b->specificity) return a->specificity < b->specificity ? -1 : 1;
    if (a->order != b->order) return a->order < b->order ? -1 : 1;
    return 0;
}

static void stylesheet_drop_rule_index(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    budget_free(sheet->budget, sheet->rule_index_buckets);
    budget_free(sheet->budget, sheet->rule_index_slots);
    budget_free(sheet->budget, sheet->rule_index_entries);
    budget_free(sheet->budget, sheet->rule_filters);
    budget_free(sheet->budget, sheet->rule_relational_tokens);
    budget_free(sheet->budget, sheet->rule_ancestor_tokens);
    sheet->rule_index_buckets = NULL;
    sheet->rule_index_slots = NULL;
    sheet->rule_index_payload_count = 0;
    sheet->rule_index_payload_capacity = 0;
    sheet->rule_index_entries = NULL;
    sheet->rule_filters = NULL;
    sheet->rule_relational_tokens = NULL;
    sheet->rule_relational_token_count = 0;
    sheet->rule_relational_token_capacity = 0;
    sheet->rule_ancestor_tokens = NULL;
    sheet->rule_index_bucket_count = 0;
    sheet->rule_index_universal_count = 0;
    sheet->rule_index_derived_keys = 0;
    sheet->rule_index_bytes = 0;
    sheet->rule_index_ready = false;
    sheet->rule_index_attempted = false;
    sheet->rule_ancestor_filter_active = false;
    sheet->rule_index_scoped_split = false;
}

static StyleTokenBloom style_selector_compound_bloom(
    const char *text, size_t length)
{
    if (text == NULL || length == 0) return style_token_bloom_empty();
    size_t at = 0;
    StyleTokenBloom bloom = style_token_bloom_empty();
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (at < length && name_character(text[at])) {
        size_t end = skip_selector_identifier(text, length, at);
        if (end != at && memchr(text + at, '\\', end - at) == NULL) {
            style_token_bloom_merge(
                &bloom, style_compound_token_bloom(
                    STYLE_SELECTOR_TAG, text + at, end - at));
        }
        at = end;
    } else if (at < length && text[at] == '*') {
        at++;
    }
    unsigned square = 0;
    unsigned round = 0;
    char quote = 0;
    for (; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == '\\' && at + 1u < length) at++;
            else if (value == quote) quote = 0;
            continue;
        }
        if (square != 0 && (value == '\'' || value == '"')) {
            quote = value;
        } else if (value == '[') {
            square++;
        } else if (value == ']' && square != 0) {
            square--;
        } else if (square == 0 && value == '(') {
            round++;
        } else if (square == 0 && value == ')' && round != 0) {
            round--;
        } else if (square == 0 && round == 0
                   && (value == '.' || value == '#')) {
            StyleSelectorOpcode opcode = value == '.'
                ? STYLE_SELECTOR_CLASS : STYLE_SELECTOR_ID;
            size_t begin = at + 1u;
            size_t end = skip_selector_identifier(text, length, begin);
            if (end != begin
                && memchr(text + begin, '\\', end - begin) == NULL) {
                style_token_bloom_merge(
                    &bloom, style_compound_token_bloom(
                        opcode, text + begin, end - begin));
            }
            if (end != begin) at = end - 1u;
        }
    }
    return bloom;
}

static StyleTokenBloom style_rule_compound_bloom(
    const Stylesheet *sheet, size_t rule_index)
{
    if (sheet == NULL || rule_index >= sheet->count) {
        return style_token_bloom_empty();
    }
    const StyleRule *rule = &sheet->rules[rule_index];
    size_t offset = style_rule_rightmost_compound(rule);
    if (rule->selector == NULL
        || offset >= rule->selector_length) {
        return style_token_bloom_empty();
    }
    return style_selector_compound_bloom(
        rule->selector + offset, rule->selector_length - offset);
}

static StyleTokenBloom style_rule_ancestor_bloom(
    const Stylesheet *sheet, size_t rule_index)
{
    if (sheet == NULL || !sheet->selector_program_ready
        || sheet->selector_program_offsets == NULL
        || rule_index >= sheet->count) return style_token_bloom_empty();
    uint16_t instruction = sheet->selector_program_offsets[rule_index];
    if (instruction == UINT16_MAX
        || instruction >= sheet->selector_program_instruction_count) {
        return style_token_bloom_empty();
    }
    const StyleRule *rule = &sheet->rules[rule_index];
    StyleTokenBloom bloom = style_token_bloom_empty();
    bool in_ancestor = false;
    while (instruction < sheet->selector_program_instruction_count) {
        const StyleSelectorInstruction *op =
            &sheet->selector_program[instruction++];
        switch ((StyleSelectorOpcode) op->opcode) {
        case STYLE_SELECTOR_PARENT:
        case STYLE_SELECTOR_ANCESTOR:
            in_ancestor = true;
            break;
        case STYLE_SELECTOR_ADJACENT:
        case STYLE_SELECTOR_GENERAL_SIBLING:
            /* A sibling itself is not an ancestor, but its parent chain is
               shared with the node we moved from. Keep already-proven tokens
               and resume collecting after the next parent/ancestor step. */
            in_ancestor = false;
            break;
        case STYLE_SELECTOR_TAG_ID:
            if (in_ancestor) {
                size_t length = 0;
                const char *name = (const char *) lxb_tag_name_by_id(
                    op->text_offset, &length);
                if (name == NULL) return style_token_bloom_empty();
                style_token_bloom_merge(&bloom, style_compound_token_bloom(
                    STYLE_SELECTOR_TAG, name, length));
            }
            break;
        case STYLE_SELECTOR_TAG:
        case STYLE_SELECTOR_CLASS:
        case STYLE_SELECTOR_ID:
            if (!in_ancestor) break;
            if (op->text_offset > rule->selector_length
                || op->text_length
                    > rule->selector_length - op->text_offset) {
                return style_token_bloom_empty();
            }
            style_token_bloom_merge(
                &bloom, style_compound_token_bloom(
                    (StyleSelectorOpcode) op->opcode,
                    rule->selector + op->text_offset, op->text_length));
            break;
        case STYLE_SELECTOR_COMPOUND:
            if (!in_ancestor) break;
            if (op->text_offset > rule->selector_length
                || op->text_length
                    > rule->selector_length - op->text_offset) {
                return style_token_bloom_empty();
            }
            style_token_bloom_merge(
                &bloom, style_selector_compound_bloom(
                    rule->selector + op->text_offset, op->text_length));
            break;
        case STYLE_SELECTOR_ATTRIBUTE_PRESENT:
        case STYLE_SELECTOR_ATTRIBUTE_NAME:
        case STYLE_SELECTOR_ATTRIBUTE_EXACT:
        case STYLE_SELECTOR_ATTRIBUTE_WORD:
        case STYLE_SELECTOR_ATTRIBUTE_PREFIX:
        case STYLE_SELECTOR_ATTRIBUTE_SUFFIX:
        case STYLE_SELECTOR_ATTRIBUTE_SUBSTRING:
        case STYLE_SELECTOR_ATTRIBUTE_DASH:
        case STYLE_SELECTOR_PSEUDO:
        case STYLE_SELECTOR_CLASS_TOKEN_PREFIX:
            /* Attribute and pseudo-class tests carry no ancestor token,
               exactly as their text form contributed none. */
            break;
        case STYLE_SELECTOR_END:
            return bloom;
        default:
            return style_token_bloom_empty();
        }
    }
    return style_token_bloom_empty();
}

/* The rule's rightmost compound, trimmed, when its prepared offset is the
   split the compiled program makes of the same text (both use
   style_selector_last_combinator, the string matcher on the trimmed
   selector the offset is relative to); false otherwise. */
static bool style_rule_matcher_compound(const StyleRule *rule,
                                        const char **text, size_t *length)
{
    if (rule == NULL || rule->selector == NULL) return false;
    size_t start = style_rule_rightmost_compound(rule);
    if (start >= rule->selector_length) return false;
    const char *whole = rule->selector;
    size_t whole_length = rule->selector_length;
    trim(&whole, &whole_length);
    if (whole != rule->selector
        || style_selector_rightmost_compound_start(whole, whole_length)
            != start) return false;
    *text = rule->selector + start;
    *length = rule->selector_length - start;
    trim(text, length);
    return *length != 0;
}

/* Required ancestor tokens as they are found: the Bloom bits of all of
   them, and the first one's exact hash. */
typedef struct {
    StyleTokenBloom bloom;
    uint32_t first_hash;
    bool found;
} StyleRequiredTokens;

static void style_required_token_add(StyleRequiredTokens *tokens,
                                     StyleSelectorOpcode opcode,
                                     const char *text, size_t length)
{
    style_token_bloom_merge(&tokens->bloom,
                            style_compound_token_bloom(opcode, text, length));
    if (!tokens->found) {
        tokens->first_hash = style_token_hash(opcode, text, length);
        tokens->found = true;
    }
}

/* Adds the decoded `.class` / `#id` tokens that open `compound`:
   compound_matches_depth tests a compound's tokens in order and fails at
   the first the element lacks, so every one of them is required of any
   element the compound matches. */
static void style_compound_leading_tokens(const char *compound,
                                          size_t length,
                                          StyleRequiredTokens *tokens)
{
    for (size_t at = 0; at < length
         && (compound[at] == '.' || compound[at] == '#');) {
        bool id = compound[at++] == '#';
        size_t end = skip_selector_identifier(compound, length, at);
        if (end == at) return;
        char scratch[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
        size_t decoded_length = 0;
        const char *decoded = style_selector_identifier_span(
            compound + at, end - at, scratch, sizeof(scratch),
            &decoded_length);
        if (decoded != NULL && decoded_length != 0) {
            style_required_token_add(
                tokens, id ? STYLE_SELECTOR_ID : STYLE_SELECTOR_CLASS,
                decoded, decoded_length);
        }
        at = end;
    }
}

/* Tokens a rule requires of some ancestor of its subject through a
   top-level :is()/:where() of its rightmost compound: for a one-option
   argument whose last combinator is a descendant or child one, the leading
   tokens of the compound left of that combinator. Tailwind's `**:` and `*:`
   variants compile to `:is(.\*\*\:mb-4 *)` and `:is(.\*\:p-2 > *)`, rules
   with no subject key that walked every element's whole ancestor chain
   (about 30 such rules and 1.4 M ancestor visits on xe.com). These tokens
   join the rule's ancestor Bloom, and for a universal-range rule the first
   becomes its exact ancestor token (StyleRuleFilter.ancestor_token), which
   rejects it for every element no ancestor of which carries the class.
   The compound is tokenized exactly as compound_matches_depth does, options
   are split by style_selector_list_option_end and the combinator by
   style_selector_last_combinator, as the functional matcher does. */
static void style_rule_functional_ancestor_tokens(
    const StyleRule *rule, StyleRequiredTokens *tokens)
{
    const char *text = NULL;
    size_t length = 0;
    if (!style_rule_matcher_compound(rule, &text, &length)) return;
    size_t at = 0;
    if (text[at] == '*') at++;
    else if (name_character(text[at]) || text[at] == '\\')
        at = skip_selector_identifier(text, length, at);
    while (at < length) {
        char value = text[at];
        if (value == '.' || value == '#') {
            size_t end = skip_selector_identifier(text, length, at + 1);
            if (end == at + 1) return;
            at = end;
        } else if (value == '[') {
            size_t end = at + 1;
            char quote = 0;
            while (end < length) {
                if (quote != 0) { if (text[end] == quote) quote = 0; }
                else if (text[end] == '\'' || text[end] == '"')
                    quote = text[end];
                else if (text[end] == ']') break;
                end++;
            }
            if (end == length) return;
            at = end + 1;
        } else if (value == ':') {
            if (at + 1 < length && text[at + 1] == ':') return;
            size_t name = ++at;
            while (at < length && name_character(text[at])) at++;
            StylePseudoKind kind = style_pseudo_kind(text + name, at - name);
            if (at >= length || text[at] != '(') continue;
            int depth = 1;
            size_t close = ++at;
            while (close < length && depth != 0) {
                if (text[close] == '(') depth++;
                else if (text[close] == ')') depth--;
                if (depth != 0) close++;
            }
            if (depth != 0) return;
            const char *option = text + at;
            size_t option_length = close - at;
            trim(&option, &option_length);
            at = close + 1;
            if (kind != STYLE_PSEUDO_IS || option_length == 0
                || style_selector_list_option_end(
                       option, option_length, 0) != option_length) continue;
            size_t split = 0;
            char combinator = 0;
            if (!style_selector_last_combinator(
                    option, option_length, &split, &combinator)
                || (combinator != ' ' && combinator != '>')) continue;
            size_t prefix_length = split;
            while (prefix_length != 0
                   && isspace((unsigned char) option[prefix_length - 1]))
                prefix_length--;
            if (prefix_length == 0) continue;
            size_t ancestor = style_selector_rightmost_compound_start(
                option, prefix_length);
            const char *compound = option + ancestor;
            size_t compound_length = prefix_length - ancestor;
            trim(&compound, &compound_length);
            style_compound_leading_tokens(compound, compound_length, tokens);
        } else if (isspace((unsigned char) value)) {
            at++;
        } else {
            return;
        }
    }
}

/* The same for the compiled program's ancestor compounds: a whole-text
   compound reached through a parent or ancestor step (not past a sibling
   step) is matched by compound_matches_depth against an ancestor of the
   subject, so its leading tokens are required of one. The Bloom above
   skips escaped tokens; these are decoded. */
static void style_rule_program_ancestor_tokens(
    const Stylesheet *sheet, size_t rule_index, StyleRequiredTokens *tokens)
{
    if (sheet == NULL || !sheet->selector_program_ready
        || sheet->selector_program_offsets == NULL
        || rule_index >= sheet->count) return;
    uint16_t instruction = sheet->selector_program_offsets[rule_index];
    if (instruction == UINT16_MAX
        || instruction >= sheet->selector_program_instruction_count) return;
    const StyleRule *rule = &sheet->rules[rule_index];
    bool in_ancestor = false;
    while (instruction < sheet->selector_program_instruction_count) {
        const StyleSelectorInstruction *op =
            &sheet->selector_program[instruction++];
        switch ((StyleSelectorOpcode) op->opcode) {
        case STYLE_SELECTOR_PARENT:
        case STYLE_SELECTOR_ANCESTOR:
            in_ancestor = true;
            break;
        case STYLE_SELECTOR_ADJACENT:
        case STYLE_SELECTOR_GENERAL_SIBLING:
            in_ancestor = false;
            break;
        case STYLE_SELECTOR_CLASS:
        case STYLE_SELECTOR_ID:
        case STYLE_SELECTOR_COMPOUND:
            if (!in_ancestor) break;
            if (op->text_offset > rule->selector_length
                || op->text_length
                    > rule->selector_length - op->text_offset) return;
            if ((StyleSelectorOpcode) op->opcode == STYLE_SELECTOR_COMPOUND) {
                const char *compound = rule->selector + op->text_offset;
                size_t compound_length = op->text_length;
                trim(&compound, &compound_length);
                style_compound_leading_tokens(
                    compound, compound_length, tokens);
            } else {
                style_required_token_add(
                    tokens, (StyleSelectorOpcode) op->opcode,
                    rule->selector + op->text_offset, op->text_length);
            }
            break;
        case STYLE_SELECTOR_END:
            return;
        default:
            break;
        }
    }
}

static const char *style_rule_index_derived_key(
    const StyleRule *rule, char *scratch, size_t capacity,
    SelectorType *type, size_t *key_length);

/* The one-based slot of `hash` in the sheet's exact ancestor-token table,
   added when there is room; 0 when the table is full or cannot be made. */
static uint8_t stylesheet_ancestor_token_slot(Stylesheet *sheet,
                                              uint32_t hash)
{
    static uint32_t next_stamp;
    if (sheet->rule_ancestor_tokens == NULL) {
        sheet->rule_ancestor_tokens = budget_calloc(
            sheet->budget, 1, sizeof(*sheet->rule_ancestor_tokens));
        if (sheet->rule_ancestor_tokens == NULL) return 0;
        if (++next_stamp == 0) next_stamp = 1;
        sheet->rule_ancestor_tokens->stamp = next_stamp;
    }
    struct StyleRuleAncestorTokens *table = sheet->rule_ancestor_tokens;
    unsigned found = style_rule_ancestor_token_find(table, hash);
    if (found != 0) return (uint8_t) found;
    if (table->count >= STYLE_RULE_ANCESTOR_TOKEN_LIMIT) return 0;
    size_t slot = hash & (STYLE_RULE_ANCESTOR_TOKEN_SLOTS - 1u);
    while (table->slots[slot] != 0)
        slot = (slot + 1u) & (STYLE_RULE_ANCESTOR_TOKEN_SLOTS - 1u);
    table->hashes[table->count++] = hash;
    table->slots[slot] = table->count;
    return table->count;
}

static bool selector_text_has_ci(const char *text, size_t length,
                                 const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length == 0 || length < needle_length) return false;
    for (size_t at = 0; at + needle_length <= length; at++) {
        size_t i = 0;
        while (i < needle_length
               && tolower((unsigned char) text[at + i]) == needle[i]) i++;
        if (i == needle_length) return true;
    }
    return false;
}

static bool attribute_selector_targets_identity(const char *text,
                                                size_t length)
{
    size_t at = 0;
    while (at < length && isspace((unsigned char) text[at])) at++;
    size_t begin = at;
    while (at < length && (isalnum((unsigned char) text[at])
                           || text[at] == '-' || text[at] == '_')) at++;
    size_t name_length = at - begin;
    return (name_length == 5 && strncasecmp(text + begin, "class", 5) == 0)
           || (name_length == 2 && strncasecmp(text + begin, "id", 2) == 0);
}

uint32_t stylesheet_identity_token_hash(bool id, const char *text,
                                        size_t length)
{
    uint32_t hash = UINT32_C(2166136261) ^ (id ? UINT32_C(0x23) : UINT32_C(0x2e));
    hash *= UINT32_C(16777619);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) text[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void relational_tokens_add(StyleRuleFilter *filter, uint32_t *tokens,
                                  uint32_t hash)
{
    for (size_t i = 0; i < filter->relational_count; i++) {
        if (tokens[i] == hash) return;
    }
    if (filter->relational_count == STYLE_RELATIONAL_TOKEN_LIMIT) {
        filter->relational_opaque = true;
        return;
    }
    tokens[filter->relational_count++] = hash;
}

static void style_rule_relational_filter(const StyleRule *rule,
                                         StyleRuleFilter *filter,
                                         uint32_t *tokens)
{
    filter->relational_count = 0;
    filter->relational_opaque = true;
    if (rule == NULL || rule->selector == NULL) return;
    const char *text = rule->selector;
    size_t length = rule->selector_length;
    size_t split = style_rule_rightmost_compound(rule);
    if (split > length) return;
    /* :has() makes an element's match depend on its descendants, `of S`
       nth forms on its siblings' classes, and escaped identifiers cannot be
       hashed like the live class list. */
    if (selector_text_has_ci(text, length, ":has(")
        || memchr(text, '\\', length) != NULL
        || (selector_text_has_ci(text, length, ":nth-")
            && selector_text_has_ci(text, length, " of "))) return;
    filter->relational_opaque = false;
    unsigned square = 0;
    unsigned parentheses = 0;
    char quote = 0;
    for (size_t at = 0; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == quote) quote = 0;
            continue;
        }
        if (square != 0) {
            if (value == '"' || value == '\'') quote = value;
            else if (value == ']') square--;
            continue;
        }
        if (value == '[') {
            square++;
            if ((at < split || parentheses != 0)
                && attribute_selector_targets_identity(
                    text + at + 1u, length - at - 1u)) {
                filter->relational_opaque = true;
            }
            continue;
        }
        if (value == '(') { parentheses++; continue; }
        if (value == ')') {
            if (parentheses != 0) parentheses--;
            continue;
        }
        /* A rightmost :is()/:where()/:not() can itself contain an ancestor
           or sibling selector. Its tokens are not local to this element. */
        if ((value == '.' || value == '#')
            && (at < split || parentheses != 0)) {
            size_t begin = at + 1u;
            size_t end = skip_selector_identifier(text, length, begin);
            if (end != begin) {
                relational_tokens_add(filter, tokens, stylesheet_identity_token_hash(
                    value == '#', text + begin, end - begin));
                at = end - 1u;
            }
        }
    }
}

size_t stylesheet_rules_affected_by_tokens(
    const Stylesheet *sheet, const uint32_t *hashes, size_t hash_count,
    uint32_t *out, size_t capacity)
{
    if (sheet == NULL || sheet->rule_filters == NULL || hashes == NULL
        || out == NULL) return SIZE_MAX;
    size_t count = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRuleFilter *filter = &sheet->rule_filters[i];
        bool affected = filter->relational_opaque;
        for (size_t r = 0; !affected && r < filter->relational_count; r++) {
            for (size_t h = 0; !affected && h < hash_count; h++) {
                affected = sheet->rule_relational_tokens[
                    filter->relational_offset + r] == hashes[h];
            }
        }
        if (!affected) continue;
        if (count == capacity || i > UINT32_MAX) return SIZE_MAX;
        out[count++] = (uint32_t) i;
    }
    return count;
}

/* Decodes a CSS identifier with escapes into `out`; false when it does
   not fit or is malformed. */
static bool identity_identifier_decode(const char *text, size_t length,
                                       char *out, size_t capacity,
                                       size_t *used)
{
    size_t written = 0;
    for (size_t at = 0; at < length;) {
        unsigned char value = (unsigned char) text[at++];
        uint32_t codepoint = value;
        if (value == '\\') {
            if (at >= length) return false;
            if (isxdigit((unsigned char) text[at])) {
                codepoint = 0;
                for (size_t digits = 0; digits < 6 && at < length
                     && isxdigit((unsigned char) text[at]); digits++, at++) {
                    unsigned char digit = (unsigned char) text[at];
                    codepoint = codepoint * 16u + (isdigit(digit)
                        ? (uint32_t) (digit - '0')
                        : (uint32_t) (tolower(digit) - 'a' + 10));
                }
                if (at < length && isspace((unsigned char) text[at])) at++;
                if (codepoint == 0 || codepoint > 0x10ffffu) return false;
            } else {
                codepoint = (unsigned char) text[at++];
            }
        }
        unsigned char bytes[4];
        size_t count = 1;
        if (codepoint <= 0x7fu || value != '\\') {
            bytes[0] = (unsigned char) codepoint;
        } else if (codepoint <= 0x7ffu) {
            bytes[0] = (unsigned char) (0xc0u | (codepoint >> 6));
            bytes[1] = (unsigned char) (0x80u | (codepoint & 63u));
            count = 2;
        } else if (codepoint <= 0xffffu) {
            bytes[0] = (unsigned char) (0xe0u | (codepoint >> 12));
            bytes[1] = (unsigned char) (0x80u | ((codepoint >> 6) & 63u));
            bytes[2] = (unsigned char) (0x80u | (codepoint & 63u));
            count = 3;
        } else {
            bytes[0] = (unsigned char) (0xf0u | (codepoint >> 18));
            bytes[1] = (unsigned char) (0x80u | ((codepoint >> 12) & 63u));
            bytes[2] = (unsigned char) (0x80u | ((codepoint >> 6) & 63u));
            bytes[3] = (unsigned char) (0x80u | (codepoint & 63u));
            count = 4;
        }
        if (count > capacity - written) return false;
        memcpy(out + written, bytes, count);
        written += count;
    }
    *used = written;
    return true;
}

static bool selector_attribute_name_byte(unsigned char value);

uint32_t stylesheet_identity_attribute_hash(const char *name, size_t length)
{
    uint32_t hash = (UINT32_C(2166136261) ^ UINT32_C(0x5b)) * UINT32_C(16777619);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) tolower((unsigned char) name[i]);
        hash *= UINT32_C(16777619);
    }
    return hash;
}

bool style_selector_identity_tokens(const char *text, size_t length,
                                    size_t split, bool every_position,
                                    StyleIdentityTokenVisit visit,
                                    void *context, bool *attributes_opaque)
{
    if (text == NULL) return true;
    if (split > length) split = 0;
    enum { NESTING = 32 };
    bool has_argument[NESTING] = {false};
    unsigned depth = 0, has_depth = 0;
    char quote = 0;
    unsigned square = 0;
    bool ok = true;
    for (size_t at = 0; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == '\\' && at + 1 < length) at++;
            else if (value == quote) quote = 0;
            continue;
        }
        if (square != 0) {
            if (value == '"' || value == '\'') quote = value;
            else if (value == ']') square--;
            continue;
        }
        if (value == '[') {
            square++;
            /* A [class]/[id] test reads identities no token names. */
            if ((every_position || at < split || depth != 0)
                && attribute_selector_targets_identity(
                       text + at + 1u, length - at - 1u)) ok = false;
            if (attributes_opaque != NULL && has_depth == 0
                && (every_position || !(at > split && depth == 0))) {
                size_t begin = at + 1u;
                while (begin < length && isspace((unsigned char) text[begin]))
                    begin++;
                size_t end = begin;
                while (end < length
                       && selector_attribute_name_byte(
                              (unsigned char) text[end])) end++;
                size_t next = end;
                while (next < length && isspace((unsigned char) text[next]))
                    next++;
                /* `*|`, `|name`, `ns|name` and escaped names. */
                char match = next < length ? text[next] : '\0';
                if (end == begin
                    || !(match == ']' || match == '='
                         || ((match == '~' || match == '|' || match == '^'
                              || match == '$' || match == '*')
                             && next + 1u < length
                             && text[next + 1u] == '=')))
                    *attributes_opaque = true;
                else
                    visit(context, stylesheet_identity_attribute_hash(
                        text + begin, end - begin));
            }
            continue;
        }
        if (value == '(') {
            bool has = at >= 4 && strncasecmp(text + at - 4, ":has", 4) == 0;
            if (depth < NESTING) has_argument[depth] = has;
            if (has) has_depth++;
            depth++;
            continue;
        }
        if (value == ')') {
            if (depth != 0) {
                depth--;
                if (depth < NESTING && has_argument[depth]) has_depth--;
            }
            continue;
        }
        if (value == '\\' && at + 1 < length) {
            at++;
            continue;
        }
        if (value != '.' && value != '#') continue;
        size_t begin = at + 1u;
        size_t end = skip_selector_identifier(text, length, begin);
        if (end == begin) continue;
        at = end - 1u;
        bool local = begin > split && depth == 0;
        if (!every_position && (local || has_depth != 0)) continue;
        char decoded[256];
        size_t decoded_length = 0;
        const char *name = text + begin;
        size_t name_length = end - begin;
        if (memchr(name, '\\', name_length) != NULL) {
            if (!identity_identifier_decode(name, name_length, decoded,
                                            sizeof(decoded),
                                            &decoded_length)) {
                ok = false;
                continue;
            }
            name = decoded;
            name_length = decoded_length;
        }
        visit(context, stylesheet_identity_token_hash(
            value == '#', name, name_length));
    }
    return ok;
}

/* Whether a selector can match its subject by way of a sibling: a sibling
   combinator or an :nth-* pseudo-class (whose `of S` reads siblings)
   anywhere, conservatively including an an+b's '+'. */
static bool identity_selector_reaches_siblings(const char *text,
                                               size_t length)
{
    char quote = 0;
    for (size_t at = 0; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == '\\' && at + 1 < length) at++;
            else if (value == quote) quote = 0;
            continue;
        }
        if (value == '"' || value == '\'') quote = value;
        else if (value == '\\') at++;
        else if (value == '+' || value == '~') return true;
        else if (value == ':' && at + 4 < length
                 && strncasecmp(text + at + 1, "nth-", 4) == 0) return true;
    }
    return false;
}

typedef struct {
    StyleIdentityScan *scan;
    const char *text;
    size_t length, split;
    bool ready;
} IdentityScanSelector;

/* The subject key and sibling reach are worked out for the few selectors
   that read an identity off their subject. */
static void identity_scan_visit(void *context, uint32_t hash)
{
    IdentityScanSelector *selector = context;
    StyleIdentityScan *scan = selector->scan;
    if (!selector->ready) {
        scan->subject_key = style_selector_compound_key_at(
            selector->text, selector->length, selector->split);
        scan->sibling = identity_selector_reaches_siblings(selector->text,
                                                           selector->length);
        selector->ready = true;
    }
    scan->nonlocal(scan->context, hash);
}

static bool identity_scan_selector(const char *text, size_t length,
                                   size_t split, StyleIdentityScan *scan)
{
    /* Most selectors are one compound with no functional argument or
       attribute test (atomic CSS): nothing in them is off the subject. */
    if (split == 0 && memchr(text, '(', length) == NULL
        && memchr(text, '[', length) == NULL) return true;
    IdentityScanSelector selector = {
        .scan = scan, .text = text, .length = length, .split = split
    };
    return style_selector_identity_tokens(text, length, split, false,
                                          identity_scan_visit, &selector,
                                          &scan->attributes_opaque);
}

bool stylesheet_structural_custom_rules(const Stylesheet *sheet,
                                        StyleKeyNamesVisit visit,
                                        void *context)
{
    for (size_t i = 0; sheet != NULL && i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        if (rule->selector == NULL) continue;
        size_t length = strlen(rule->selector);
        if (!identity_selector_reaches_siblings(rule->selector, length)
            && strstr(rule->selector, ":first-") == NULL
            && strstr(rule->selector, ":last-") == NULL
            && strstr(rule->selector, ":only-") == NULL
            && strstr(rule->selector, ":empty") == NULL) continue;
        StyleKeyNames entry = {
            style_selector_compound_key_at(
                rule->selector, length,
                style_selector_rightmost_compound_start(rule->selector,
                                                        length)),
            stylesheet_custom_property_name_bits(rule->name,
                                                 rule->name_length)
        };
        if (!visit(context, entry)) return false;
    }
    return true;
}

bool stylesheet_identity_scan(const Stylesheet *sheet,
                              StyleIdentityScan *scan)
{
    if (sheet == NULL || scan == NULL) return false;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (!identity_scan_selector(rule->selector, rule->selector_length,
                                    style_rule_rightmost_compound(rule),
                                    scan)) return false;
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        size_t length = strlen(rule->selector);
        if (!identity_scan_selector(
                rule->selector, length,
                style_selector_rightmost_compound_start(rule->selector,
                                                        length),
                scan)) return false;
        scan->names = stylesheet_custom_property_name_bits(
            rule->name, rule->name_length);
        if (!style_selector_identity_tokens(rule->selector, length, 0, true,
                                            scan->declares, scan->context,
                                            &scan->attributes_opaque))
            return false;
    }
    return true;
}

static bool class_list_has_token(const char *list, size_t length,
                                 const char *token, size_t token_length)
{
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) list[at])) at++;
        size_t begin = at;
        while (at < length && !isspace((unsigned char) list[at])) at++;
        if (at - begin == token_length
            && memcmp(list + begin, token, token_length) == 0) return true;
    }
    return false;
}

static bool change_tokens_add(uint32_t *hashes, size_t capacity,
                              size_t *count, uint32_t hash)
{
    for (size_t i = 0; i < *count; i++) {
        if (hashes[i] == hash) return true;
    }
    if (*count == capacity) return false;
    hashes[(*count)++] = hash;
    return true;
}

static bool class_list_difference_tokens(const char *from, size_t from_length,
                                         const char *against,
                                         size_t against_length,
                                         uint32_t *hashes, size_t capacity,
                                         size_t *count)
{
    size_t at = 0;
    while (at < from_length) {
        while (at < from_length && isspace((unsigned char) from[at])) at++;
        size_t begin = at;
        while (at < from_length && !isspace((unsigned char) from[at])) at++;
        if (at != begin
            && !class_list_has_token(against, against_length,
                                     from + begin, at - begin)
            && !change_tokens_add(hashes, capacity, count,
                                  stylesheet_identity_token_hash(
                                      false, from + begin, at - begin))) {
            return false;
        }
    }
    return true;
}

bool stylesheet_attribute_change_tokens(
    const char *name, size_t name_length,
    const char *old_value, size_t old_length,
    const char *new_value, size_t new_length, uint32_t *hashes,
    size_t capacity, size_t *count)
{
    if (name == NULL || hashes == NULL || count == NULL) return false;
    *count = 0;
    if (old_value == NULL) old_length = 0;
    if (new_value == NULL) new_length = 0;
    if (old_length > 4096u || new_length > 4096u) return false;
    if (name_length == 5 && strncasecmp(name, "class", 5) == 0) {
        return class_list_difference_tokens(
                   old_value, old_length, new_value, new_length,
                   hashes, capacity, count)
               && class_list_difference_tokens(
                   new_value, new_length, old_value, old_length,
                   hashes, capacity, count);
    }
    if (name_length == 2 && strncasecmp(name, "id", 2) == 0) {
        if (old_length != 0
            && !change_tokens_add(hashes, capacity, count,
                                  stylesheet_identity_token_hash(
                                      true, old_value, old_length))) {
            return false;
        }
        if (new_length != 0
            && !change_tokens_add(hashes, capacity, count,
                                  stylesheet_identity_token_hash(
                                      true, new_value, new_length))) {
            return false;
        }
        return true;
    }
    return false;
}

/* Whether rule `index` can hide or reveal an element or give it an image:
   what the image pass reads besides image elements themselves. */
static bool stylesheet_rule_affects_discovery(const Stylesheet *sheet,
                                              size_t index)
{
    const StyleDeclaration *declaration = stylesheet_rule_declaration(
        sheet, &sheet->rules[index]);
    const uint64_t discovery_mask = S_DISPLAY | S_VISIBILITY
        | S_BACKGROUND_IMAGE | S_MASK_IMAGE | S_CONTENT;
    uint8_t stack_id = 0;
    if (declaration != NULL && (declaration->values_flags
            & STYLE_DECLARATION_VALUES_PAINT_STACK) != 0) {
        ComputedStyle values;
        stylesheet_declaration_values(sheet, declaration, &values);
        stack_id = computed_style_paint_stack_id(&values);
    }
    const StylePaintStack *stack = stack_id == 0
        ? NULL : stylesheet_paint_stack(sheet, stack_id);
    bool stack_images = stack_id != 0
        && (stack == NULL
            || (stack->components
                & (STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE
                   | STYLE_PAINT_COMPONENT_MASK_IMAGE)) != 0);
    return declaration == NULL
        || (declaration->mask & discovery_mask) != 0
        || (declaration->inherit_mask & discovery_mask) != 0
        || declaration->deferred_declarations != NULL
        || declaration->deferred_program_count != 0
        || stack_images;
}

bool stylesheet_rules_affect_discovery(const Stylesheet *sheet,
                                       const uint32_t *indices, size_t count)
{
    if (sheet == NULL || (indices == NULL && count != 0)) return true;
    for (size_t i = 0; i < count; i++) {
        if (indices[i] >= sheet->count
            || stylesheet_rule_affects_discovery(sheet, indices[i]))
            return true;
    }
    return false;
}

static void stylesheet_prepare_rule_filters(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL || sheet->count == 0
        || sheet->count > SIZE_MAX / sizeof(StyleRuleFilter)) return;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLE_RULE_FILTER") != NULL) return;
#endif
    budget_free(sheet->budget, sheet->rule_ancestor_tokens);
    sheet->rule_ancestor_tokens = NULL;
    StyleRuleFilter *filters = budget_malloc(
        sheet->budget, sheet->count * sizeof(*filters));
    if (filters == NULL) return;
    bool useful = false;
    bool relational_refused = false;
    bool ancestors_useful = false;
    for (size_t i = 0; i < sheet->count; i++) {
        filters[i] = (StyleRuleFilter) {
            .compound = style_rule_compound_bloom(sheet, i),
            .ancestors = style_rule_ancestor_bloom(sheet, i)
        };
        StyleRequiredTokens required = {0};
        style_rule_functional_ancestor_tokens(&sheet->rules[i], &required);
        style_token_bloom_merge(&filters[i].ancestors, required.bloom);
        char key_scratch[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
        SelectorType key_type = SELECTOR_TAG;
        size_t key_length = 0;
        if (!sheet->rules[i].has_fast_key
            && style_rule_index_derived_key(
                   &sheet->rules[i], key_scratch, sizeof(key_scratch),
                   &key_type, &key_length) == NULL) {
            /* A universal-range rule: every element is its candidate. */
            if (!required.found)
                style_rule_program_ancestor_tokens(sheet, i, &required);
            if (required.found)
                filters[i].ancestor_token = (uint8_t)
                    style_rule_ancestor_token_bit(
                        stylesheet_ancestor_token_slot(
                            sheet, required.first_hash));
        }
        uint32_t tokens[STYLE_RELATIONAL_TOKEN_LIMIT];
        style_rule_relational_filter(&sheet->rules[i], &filters[i], tokens);
        size_t token_count = filters[i].relational_count;
        if (filters[i].relational_opaque) token_count = 0;
        if (token_count != 0 && !relational_refused) {
            size_t used = sheet->rule_relational_token_count;
            if (token_count > STYLE_RELATIONAL_POOL_TOKEN_LIMIT - used) {
                relational_refused = true;
            } else if (token_count
                       > sheet->rule_relational_token_capacity - used) {
                size_t capacity = sheet->rule_relational_token_capacity;
                capacity = capacity == 0 ? 128u : capacity * 2u;
                if (capacity > STYLE_RELATIONAL_POOL_TOKEN_LIMIT)
                    capacity = STYLE_RELATIONAL_POOL_TOKEN_LIMIT;
                uint32_t *grown = budget_realloc(
                    sheet->budget, sheet->rule_relational_tokens,
                    capacity * sizeof(*grown));
                if (grown == NULL) relational_refused = true;
                else {
                    sheet->rule_relational_tokens = grown;
                    sheet->rule_relational_token_capacity = capacity;
                }
            }
            if (!relational_refused) {
                filters[i].relational_offset = (uint32_t) used;
                memcpy(sheet->rule_relational_tokens + used, tokens,
                       token_count * sizeof(*tokens));
                sheet->rule_relational_token_count += token_count;
            }
        }
        if (token_count == 0 || relational_refused) {
            filters[i].relational_count = 0;
            if (token_count != 0) filters[i].relational_opaque = true;
        }
        const StyleDeclaration *declaration = stylesheet_rule_declaration(
            sheet, &sheet->rules[i]);
        filters[i].discovery = stylesheet_rule_affects_discovery(sheet, i);
        /* What an inline SVG raster resolves: currentColor and em sizes
           inherit from any matched element, var() may feed either, and the
           default viewport reads only the SVG's own width/height. */
        uint64_t set = declaration == NULL ? 0
            : declaration->mask | declaration->inherit_mask;
        filters[i].svg_raster = (uint8_t) (
            (declaration == NULL || (set & (S_COLOR | S_FONT_SCALE)) != 0
             || declaration->deferred_declarations != NULL
             || declaration->deferred_program_count != 0
                 ? STYLE_RULE_SVG_RASTER_INHERITED : 0)
            | ((set & (S_WIDTH | S_HEIGHT)) != 0
                 ? STYLE_RULE_SVG_RASTER_SIZE : 0));
        useful = useful || !style_token_bloom_empty_value(filters[i].compound)
            || !style_token_bloom_empty_value(filters[i].ancestors)
            || filters[i].ancestor_token != 0;
        ancestors_useful = ancestors_useful
            || !style_token_bloom_empty_value(filters[i].ancestors);
    }
    if (!useful) {
        budget_free(sheet->budget, filters);
        budget_free(sheet->budget, sheet->rule_relational_tokens);
        sheet->rule_relational_tokens = NULL;
        sheet->rule_relational_token_count = 0;
        sheet->rule_relational_token_capacity = 0;
        budget_free(sheet->budget, sheet->rule_ancestor_tokens);
        sheet->rule_ancestor_tokens = NULL;
        return;
    }
    sheet->rule_filters = filters;
    sheet->rule_ancestor_filter_active = ancestors_useful;
    sheet->rule_index_bytes += sheet->count * sizeof(*filters);
    sheet->rule_index_bytes += sheet->rule_relational_token_capacity
                              * sizeof(*sheet->rule_relational_tokens);
}

static void stylesheet_drop_custom_rule_index(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    budget_free(sheet->budget, sheet->custom_rule_index);
    sheet->custom_rule_index = NULL;
    sheet->custom_rule_index_count = 0;
    sheet->custom_rule_index_bytes = 0;
    sheet->custom_rule_index_ready = false;
    sheet->custom_rule_index_attempted = false;
}

static void stylesheet_suffix_state_begin(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    budget_free(sheet->budget, sheet->selector_append_offsets);
    sheet->selector_append_offsets = NULL;
    sheet->selector_append_offset_count = 0;
    sheet->selector_append_active = false;
    sheet->selector_append_restore_rule_index = sheet->rule_index_ready;
    bool preserve_selector_program = true;
#ifndef TILEFINCH_NO_TRACE
    preserve_selector_program =
        getenv("TILEFINCH_DISABLE_SELECTOR_APPEND_REUSE") == NULL;
#endif
    size_t append_offset_count = (size_t) sheet->next_order * 2u;
    bool append_offset_count_valid = sheet->next_order == 0
        || append_offset_count / 2u == (size_t) sheet->next_order;
    if (preserve_selector_program && sheet->selector_program_ready
        && sheet->selector_program != NULL
        && sheet->selector_program_offsets != NULL
        && sheet->next_order != 0
        && append_offset_count_valid
        && append_offset_count <= SIZE_MAX / sizeof(uint16_t)) {
        size_t count = append_offset_count;
        uint16_t *offsets = budget_malloc(
            sheet->budget, count * sizeof(*offsets));
        if (offsets != NULL) {
            for (size_t i = 0; i < count; i++) offsets[i] = UINT16_MAX;
            bool complete = true;
            for (size_t i = 0; i < sheet->count; i++) {
                const StyleRule *rule = &sheet->rules[i];
                size_t key = (size_t) rule->order * 2u
                    + (rule->important ? 1u : 0u);
                if (key >= count) {
                    complete = false;
                    break;
                }
                offsets[key] = sheet->selector_program_offsets[i];
            }
            if (complete) {
                sheet->selector_append_offsets = offsets;
                sheet->selector_append_offset_count = count;
                sheet->selector_append_active = true;
                /* Instructions and their old offset allocation remain owned
                   while parsing. No style resolution occurs until finish. */
                sheet->selector_program_ready = false;
                sheet->selector_program_attempted = false;
            } else {
                budget_free(sheet->budget, offsets);
            }
        }
    }
    if (!sheet->selector_append_active) {
        stylesheet_drop_selector_program(sheet);
    }
    stylesheet_drop_rule_index(sheet);
    stylesheet_drop_custom_rule_index(sheet);
}

static void stylesheet_suffix_state_finish(Stylesheet *sheet, bool parsed)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    bool restore_rule_index =
        sheet->selector_append_restore_rule_index;
    bool extended = false;
    size_t reused = 0;
    size_t compiled = 0;
    if (parsed && sheet->selector_append_active) {
        extended = stylesheet_extend_selector_program(
            sheet, sheet->selector_append_offsets,
            sheet->selector_append_offset_count, &reused, &compiled);
    }
    budget_free(sheet->budget, sheet->selector_append_offsets);
    sheet->selector_append_offsets = NULL;
    sheet->selector_append_offset_count = 0;
    sheet->selector_append_active = false;
    sheet->selector_append_restore_rule_index = false;
    if (!extended) stylesheet_drop_selector_program(sheet);
    if (extended) {
        sheet->selector_append_reused_rules += reused;
        sheet->selector_append_compiled_rules += compiled;
    }
    if (parsed && restore_rule_index) stylesheet_prepare_rule_index(sheet);
}

static void stylesheet_prepare_custom_rule_index(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL
        || sheet->custom_rule_index_ready
        || sheet->custom_rule_index_attempted) return;
    sheet->custom_rule_index_attempted = true;
    if (sheet->custom_rule_count == 0
        || sheet->custom_rule_count >= STYLE_CUSTOM_INDEX_SCOPED
        || sheet->custom_rule_count
             > SIZE_MAX / sizeof(StyleCustomRuleIndexEntry)) return;
    StyleCustomRuleIndexEntry *entries = budget_malloc(
        sheet->budget, sheet->custom_rule_count * sizeof(*entries));
    if (entries == NULL) return;
    uint64_t shadow_only = sheet->adopted_scopes_failed
        ? 0 : sheet->adopted_shadow_only_mask;
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        /* Retained rules pack declaration order in the low eight bits. */
        bool scoped = shadow_only != 0
            && (style_adopted_source_bit(
                    sheet, sheet->custom_rules[i].order >> 8)
                & shadow_only) != 0;
        entries[i] = (StyleCustomRuleIndexEntry) {
            .hash = custom_rule_name_hash(sheet->custom_rules[i].name,
                                          strlen(sheet->custom_rules[i].name)),
            .index = (uint32_t) i | (scoped ? STYLE_CUSTOM_INDEX_SCOPED : 0)
        };
    }
    qsort(entries, sheet->custom_rule_count, sizeof(*entries),
          compare_custom_rule_index_entries);
    sheet->custom_rule_index = entries;
    sheet->custom_rule_index_count = sheet->custom_rule_count;
    sheet->custom_rule_index_bytes =
        sheet->custom_rule_count * sizeof(*entries);
    sheet->custom_rule_index_ready = true;
}

static uint32_t style_rule_key_hash(SelectorType type, const char *text,
                                    size_t length)
{
    /* The open-addressed table consumes only the low log2(capacity) bits
       and confirms collisions with an exact type/length/text comparison.
       A 64-bit FNV multiply therefore bought no correctness while being
       particularly expensive on 32-bit Allegrex. */
    uint32_t hash = UINT32_C(2166136261) ^ (uint32_t) type;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (unsigned char) text[i]) * UINT32_C(16777619);
    }
    return hash == 0 ? UINT32_C(1) : hash;
}

const char *style_rule_fast_key(const StyleRule *rule)
{
    return rule != NULL && rule->has_fast_key && rule->selector != NULL
           && rule->fast_key_offset <= rule->selector_length
           && rule->fast_key_length
                <= rule->selector_length - rule->fast_key_offset
        ? rule->selector + rule->fast_key_offset : NULL;
}

/* The rule-index key of a rule without a stored fast key, derived from the
   matcher's own reading of its rightmost compound; NULL when there is none.
   A stored fast key must be escape-free and short (it is compared in place
   and its length kept in a byte), so Tailwind-style utility classes such as
   `.w-\[20\%\]` or `.md\:flex` used to land in the universal range and be
   matched in full against every element: 95% of the unindexed rules on the
   heavy census pages, about 300 selector tests per element.

   The compound matcher (compound_matches_depth) tests the compound's tokens
   in order and fails at the first one the element lacks, after decoding each
   identifier with style_selector_identifier_span. So the leading run of
   `.class` / `#id` tokens is required of every element the rule matches,
   exactly as decoded there: the rule can only match an element carrying the
   decoded ID (preferred) or the run's last decoded class. The compound starts
   at the rule's prepared rightmost-compound offset, the split both the
   string matcher and the compiled program use. Nothing else reads this key:
   every other fast-key consumer still treats the rule as keyless. */
static const char *style_rule_index_derived_key(
    const StyleRule *rule, char *scratch, size_t capacity,
    SelectorType *type, size_t *key_length)
{
    if (rule == NULL || rule->has_fast_key || rule->selector == NULL
        || scratch == NULL || type == NULL || key_length == NULL)
        return NULL;
    const char *text = NULL;
    size_t length = 0;
    if (!style_rule_matcher_compound(rule, &text, &length)) return NULL;
    size_t at = 0;
    if (length == 0 || (text[0] != '.' && text[0] != '#')) return NULL;
    const char *class_key = NULL, *id_key = NULL;
    size_t class_begin = 0, class_length = 0, id_begin = 0, id_length = 0;
    while (at < length && (text[at] == '.' || text[at] == '#')) {
        char marker = text[at++];
        size_t end = skip_selector_identifier(text, length, at);
        if (end == at) return NULL;
        if (marker == '#') {
            if (id_key == NULL) {
                id_key = text;
                id_begin = at;
                id_length = end - at;
            }
        } else {
            class_key = text;
            class_begin = at;
            class_length = end - at;
        }
        at = end;
    }
    const char *raw = id_key != NULL ? text + id_begin : text + class_begin;
    size_t raw_length = id_key != NULL ? id_length : class_length;
    if (id_key == NULL && class_key == NULL) return NULL;
    const char *key = style_selector_identifier_span(
        raw, raw_length, scratch, capacity, key_length);
    if (key == NULL || *key_length == 0) return NULL;
    *type = id_key != NULL ? SELECTOR_ID : SELECTOR_CLASS;
    return key;
}

/* The key a rule is indexed under: its stored fast key, else the derived
   one above; NULL for a rule that stays in the universal range. */
static const char *style_rule_index_key(
    const StyleRule *rule, char *scratch, SelectorType *type,
    size_t *key_length)
{
    if (rule->has_fast_key) {
        *type = (SelectorType) rule->type;
        *key_length = rule->fast_key_length;
        return style_rule_fast_key(rule);
    }
    return style_rule_index_derived_key(
        rule, scratch, STYLE_SELECTOR_IDENTIFIER_CAPACITY, type, key_length);
}

static bool style_rule_bucket_key_matches(
    const Stylesheet *sheet, const StyleRuleIndexBucket *bucket,
    SelectorType type, const char *text, size_t length, uint32_t hash,
    PseudoElement pseudo)
{
    if (bucket->representative == STYLE_RULE_INDEX_EMPTY
        || bucket->hash != hash
        || bucket->representative >= sheet->count) return false;
    const StyleRule *rule = &sheet->rules[bucket->representative];
    if (rule->pseudo != pseudo) return false;
    if (!rule->has_fast_key) {
        char scratch[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
        SelectorType derived_type = SELECTOR_TAG;
        size_t derived_length = 0;
        const char *derived = style_rule_index_derived_key(
            rule, scratch, sizeof(scratch), &derived_type, &derived_length);
        return derived != NULL && derived_type == type
            && derived_length == length
            && memcmp(derived, text, length) == 0;
    }
    const char *fast_key = style_rule_fast_key(rule);
    return rule->type == type
        && fast_key != NULL && rule->fast_key_length == length
        && memcmp(fast_key, text, length) == 0;
}

StyleRuleIndexBucket *style_rule_find_bucket(
    const Stylesheet *sheet, SelectorType type, const char *text,
    size_t length, bool create, PseudoElement pseudo)
{
    if (sheet == NULL || sheet->rule_index_slots == NULL
        || sheet->rule_index_bucket_count == 0 || text == NULL) return NULL;
    uint32_t hash = style_rule_key_hash(type, text, length)
        ^ ((uint32_t) pseudo * UINT32_C(0x9e3779b9));
    size_t mask = sheet->rule_index_bucket_count - 1;
    size_t slot = (size_t) hash & mask;
    for (size_t probes = 0; probes < sheet->rule_index_bucket_count;
         probes++, slot = (slot + 1) & mask) {
        uint32_t index = sheet->rule_index_slots[slot];
        if (index == 0) {
            if (!create) return NULL;
            Stylesheet *mutable_sheet = (Stylesheet *) sheet;
            if (sheet->rule_index_payload_count
                == sheet->rule_index_payload_capacity) {
                size_t capacity = sheet->rule_index_payload_capacity;
                capacity = capacity == 0 ? 128u
                    : capacity > sheet->count / 2u ? sheet->count : capacity * 2u;
                if (capacity > sheet->count) capacity = sheet->count;
                if (capacity <= sheet->rule_index_payload_count
                    || capacity > UINT32_MAX
                    || capacity > SIZE_MAX / sizeof(StyleRuleIndexBucket))
                    return NULL;
                StyleRuleIndexBucket *grown = budget_realloc(
                    sheet->budget, sheet->rule_index_buckets,
                    capacity * sizeof(*grown));
                if (grown == NULL) return NULL;
                mutable_sheet->rule_index_buckets = grown;
                mutable_sheet->rule_index_payload_capacity = capacity;
            }
            index = (uint32_t) mutable_sheet->rule_index_payload_count++;
            StyleRuleIndexBucket *bucket = &sheet->rule_index_buckets[index];
            *bucket = (StyleRuleIndexBucket) {
                .hash = hash, .representative = STYLE_RULE_INDEX_EMPTY
            };
            sheet->rule_index_slots[slot] = index + 1u;
            return bucket;
        }
        StyleRuleIndexBucket *bucket = &sheet->rule_index_buckets[index - 1u];
        if (style_rule_bucket_key_matches(sheet, bucket, type, text, length,
                                          hash, pseudo)) return bucket;
    }
    return NULL;
}

static void stylesheet_compact_storage(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
#define STYLE_SHRINK(field, count_field, capacity_field) do { \
        if (sheet->count_field != 0 \
            && sheet->capacity_field > sheet->count_field) { \
            void *compact = budget_realloc( \
                sheet->budget, sheet->field, \
                sheet->count_field * sizeof(*sheet->field)); \
            if (compact != NULL) { \
                sheet->field = compact; \
                sheet->capacity_field = sheet->count_field; \
            } \
        } \
    } while (0)
    STYLE_SHRINK(rules, count, capacity);
    STYLE_SHRINK(declarations, declaration_count, declaration_capacity);
    STYLE_SHRINK(declaration_values, declaration_value_bytes,
                 declaration_value_capacity);
    STYLE_SHRINK(variables, variable_count, variable_capacity);
    STYLE_SHRINK(custom_rules, custom_rule_count, custom_rule_capacity);
    STYLE_SHRINK(transition_rules, transition_rule_count,
                 transition_rule_capacity);
    STYLE_SHRINK(deferred_instructions, deferred_instruction_count,
                 deferred_instruction_capacity);
    STYLE_SHRINK(generated_texts, generated_text_count,
                 generated_text_capacity);
    STYLE_SHRINK(counter_operation_sets, counter_operation_set_count,
                 counter_operation_set_capacity);
    STYLE_SHRINK(image_urls, image_url_count, image_url_capacity);
#undef STYLE_SHRINK
}

void stylesheet_prepare_rule_index(Stylesheet *sheet)
{
    stylesheet_prepare_custom_rule_index(sheet);
    stylesheet_prepare_selector_program(sheet);
    if (sheet == NULL || sheet->budget == NULL || sheet->rule_index_ready
        || sheet->rule_index_attempted) return;
    sheet->rule_index_attempted = true;
    stylesheet_compact_storage(sheet);
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLE_INDEX") != NULL) return;
#endif
    if (sheet->count == 0 || sheet->count > UINT32_MAX
        || sheet->count > SIZE_MAX / 2u
        || sheet->count > SIZE_MAX / sizeof(uint32_t)) return;

    size_t bucket_count = 16;
    while (bucket_count < sheet->count * 2u) {
        if (bucket_count > SIZE_MAX / 2u) return;
        bucket_count *= 2u;
    }
    if (bucket_count > SIZE_MAX / sizeof(uint32_t)) return;
    uint32_t *slots = budget_calloc(
        sheet->budget, bucket_count, sizeof(*slots));
    uint32_t *entries = budget_malloc(
        sheet->budget, sheet->count * sizeof(*entries));
    if (slots == NULL || entries == NULL) {
        budget_free(sheet->budget, slots);
        budget_free(sheet->budget, entries);
        return;
    }
    sheet->rule_index_slots = slots;
    sheet->rule_index_entries = entries;
    sheet->rule_index_bucket_count = bucket_count;

    char key_scratch[STYLE_SELECTOR_IDENTIFIER_CAPACITY];
    sheet->rule_index_derived_keys = 0;
    size_t universal_count = 0;
    uint32_t universal_counts[3] = {0};
    uint32_t universal_scoped[3] = {0};
    /* Rules of adopted sheets only shadow roots adopt are kept after the
       other rules of each range, so elements outside every adopting
       carrier never see them as candidates. */
    uint64_t shadow_only = sheet->adopted_scopes_failed
        ? 0 : sheet->adopted_shadow_only_mask;
    sheet->rule_index_scoped_split = false;
    _Static_assert(PSEUDO_NONE == 0 && PSEUDO_BEFORE == 1 && PSEUDO_AFTER == 2,
                   "rule-index partitions must cover every pseudo element");
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (rule->pseudo > PSEUDO_AFTER) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        bool scoped = shadow_only != 0
            && (style_adopted_source_bit(sheet, rule->order)
                & shadow_only) != 0;
        if (scoped) sheet->rule_index_scoped_split = true;
        SelectorType key_type = SELECTOR_TAG;
        size_t length = 0;
        const char *fast_key = style_rule_index_key(
            rule, key_scratch, &key_type, &length);
        if (fast_key == NULL && !rule->has_fast_key) {
            universal_count++;
            universal_counts[rule->pseudo]++;
            if (scoped) universal_scoped[rule->pseudo]++;
            continue;
        }
        if (fast_key == NULL) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        if (!rule->has_fast_key) sheet->rule_index_derived_keys++;
        StyleRuleIndexBucket *bucket = style_rule_find_bucket(
            sheet, key_type, fast_key, length, true,
            (PseudoElement) rule->pseudo);
        if (bucket == NULL) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        if (bucket->representative == STYLE_RULE_INDEX_EMPTY) {
            bucket->representative = (uint32_t) i;
        }
        bucket->count++;
        if (!scoped) bucket->unscoped++;
    }
    size_t next = universal_count;
    for (size_t i = 0; i < bucket_count; i++) {
        if (slots[i] == 0) continue;
        StyleRuleIndexBucket *bucket =
            &sheet->rule_index_buckets[slots[i] - 1u];
        bucket->first = (uint32_t) next;
        next += bucket->count;
    }
    if (next != sheet->count) {
        stylesheet_drop_rule_index(sheet);
        sheet->rule_index_attempted = true;
        return;
    }
    uint32_t universal_at[3] = {
        0, universal_counts[0], universal_counts[0] + universal_counts[1]
    };
    uint32_t universal_scoped_at[3];
    for (size_t i = 0; i < 3; i++) {
        sheet->rule_index_universal_ends[i] = universal_at[i] + universal_counts[i];
        sheet->rule_index_universal_unscoped_ends[i] =
            sheet->rule_index_universal_ends[i] - universal_scoped[i];
        universal_scoped_at[i] = sheet->rule_index_universal_unscoped_ends[i];
    }
    /* Unscoped rules first, then scoped ones, each pass ascending. A
       bucket's `first` serves as its cursor and is moved back after. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sheet->count; i++) {
            const StyleRule *rule = &sheet->rules[i];
            bool scoped = sheet->rule_index_scoped_split
                && (style_adopted_source_bit(sheet, rule->order)
                    & shadow_only) != 0;
            SelectorType key_type = SELECTOR_TAG;
            size_t key_length = 0;
            const char *fast_key = style_rule_index_key(
                rule, key_scratch, &key_type, &key_length);
            if (fast_key == NULL && !rule->has_fast_key) {
                if (pass == 0)
                    entries[scoped ? universal_scoped_at[rule->pseudo]++
                                   : universal_at[rule->pseudo]++] =
                        (uint32_t) i;
                continue;
            }
            if (scoped != (pass == 1)) continue;
            StyleRuleIndexBucket *bucket = fast_key == NULL ? NULL
                : style_rule_find_bucket(
                      sheet, key_type, fast_key, key_length, false,
                      (PseudoElement) rule->pseudo);
            if (bucket == NULL || bucket->first >= sheet->count) {
                stylesheet_drop_rule_index(sheet);
                sheet->rule_index_attempted = true;
                return;
            }
            entries[bucket->first++] = (uint32_t) i;
        }
    }
    for (size_t i = 0; i < sheet->rule_index_payload_count; i++) {
        sheet->rule_index_buckets[i].first -= sheet->rule_index_buckets[i].count;
    }
    sheet->rule_index_universal_count = universal_count;
    sheet->rule_index_bytes = bucket_count * sizeof(*slots)
                              + sheet->rule_index_payload_capacity
                                * sizeof(*sheet->rule_index_buckets)
                              + sheet->count * sizeof(*entries);
    stylesheet_prepare_rule_filters(sheet);
    sheet->rule_index_ready = true;
}

static void update_cascade_ranges(Stylesheet *sheet)
{
    for (size_t phase = 0; phase < 4; phase++) {
        sheet->cascade_starts[phase] = sheet->count;
        sheet->cascade_ends[phase] = sheet->count;
    }
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        size_t phase = rule->important
            ? (rule->origin == 0 ? CASCADE_AUTHOR_IMPORTANT
                                 : CASCADE_USER_IMPORTANT)
            : (rule->origin == 0 ? CASCADE_AUTHOR_NORMAL
                                 : CASCADE_USER_NORMAL);
        if (sheet->cascade_starts[phase] == sheet->count) {
            sheet->cascade_starts[phase] = i;
        }
        sheet->cascade_ends[phase] = i + 1;
    }
}

static void stylesheet_rank_layer_children(Stylesheet *sheet,
                                           unsigned parent,
                                           unsigned *next_rank)
{
    for (size_t i = 0; i < sheet->layer_count; i++) {
        if (sheet->layer_parents[i] != parent) continue;
        stylesheet_rank_layer_children(sheet, (unsigned) i, next_rank);
        sheet->layer_ranks[i] = (uint8_t) (*next_rank)++;
    }
}

static void stylesheet_refresh_layer_keys(Stylesheet *sheet)
{
    unsigned next_rank = 1;
    stylesheet_rank_layer_children(sheet, UINT8_MAX, &next_rank);
    for (size_t i = 0; i < sheet->count; i++) {
        if (sheet->rules[i].layer == UINT_MAX) continue;
        unsigned id = sheet->rules[i].layer & UINT8_MAX;
        if (id < sheet->layer_count) {
            sheet->rules[i].layer =
                ((unsigned) sheet->layer_ranks[id] << 8) | id;
        }
    }
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        if (sheet->custom_rules[i].layer == UINT_MAX) continue;
        unsigned id = sheet->custom_rules[i].layer & UINT8_MAX;
        if (id < sheet->layer_count) {
            sheet->custom_rules[i].layer =
                ((unsigned) sheet->layer_ranks[id] << 8) | id;
        }
    }
    for (size_t i = 0; i < sheet->transition_rule_count; i++) {
        if (sheet->transition_rules[i].layer == UINT_MAX) continue;
        unsigned id = sheet->transition_rules[i].layer & UINT8_MAX;
        if (id < sheet->layer_count) {
            sheet->transition_rules[i].layer =
                ((unsigned) sheet->layer_ranks[id] << 8) | id;
        }
    }
}

static void stylesheet_finalize_rule_order(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    stylesheet_refresh_layer_keys(sheet);
    if (sheet->count > 1) {
        qsort(sheet->rules, sheet->count, sizeof(*sheet->rules),
              compare_rules);
    }
    sheet->focus_rule_index_ready = false;
    sheet->has_rule_index_ready = false;
    update_cascade_ranges(sheet);
    sheet->rule_batch_dirty = false;
}

bool stylesheet_build(Stylesheet *sheet, Budget *budget,
                      const PocDocument *document, int viewport_width)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, viewport_width, 272,
                               viewport_width, 272)) return false;
    return stylesheet_build_context(sheet, budget, document, &viewport);
}

static bool stylesheet_initialize(
    Stylesheet *sheet, Budget *budget, int viewport_width, int viewport_height)
{
    if (sheet == NULL || budget == NULL || viewport_width <= 0
        || viewport_height <= 0) return false;
    memset(sheet, 0, sizeof(*sheet));
    sheet->budget = budget;
    sheet->build_generation = stylesheet_next_generation();
    sheet->viewport_width = viewport_width;
    sheet->viewport_height = viewport_height;
    sheet->current_layer = UINT_MAX;
    memset(sheet->layer_parents, UINT8_MAX, sizeof(sheet->layer_parents));
    stylesheet_read_trace_environment(sheet);
    sheet->resolve_scratch = budget_calloc(
        budget, 1, sizeof(*sheet->resolve_scratch));
    return sheet->resolve_scratch != NULL;
}

bool stylesheet_build_context(Stylesheet *sheet, Budget *budget,
                              const PocDocument *document,
                              const ViewportContext *viewport)
{
    if (sheet == NULL || budget == NULL || document == NULL
        || document->html == NULL || viewport == NULL
        || viewport->css_width <= 0 || viewport->css_height <= 0) return false;
    if (!stylesheet_initialize(
            sheet, budget, viewport->css_width, viewport->css_height)) {
        return false;
    }
    sheet->block_inline_style_attributes =
        !tilefinch_csp_allows_style_attribute(
            &document->content_security_policy);
    sheet->noscript_rendered = document->noscript_rendered;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    StyleCssParseContext parse = {
        .sheet = sheet,
        .discover_font_faces = true
    };
    bool parsed = collect_styles(
        &parse, root, &document->content_security_policy);
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) {
        style_css_parse_context_dispose(&parse);
        stylesheet_destroy(sheet);
        return false;
    }
    stylesheet_finalize_rule_order(sheet);
    /* Adopted sheets cascade after every document source. */
    if (!stylesheet_add_adopted_sheets(sheet, document)) {
        stylesheet_destroy(sheet);
        return false;
    }
    return true;
}

bool stylesheet_build_context_deferred(
    Stylesheet *sheet, Budget *budget, const PocDocument *document,
    const ViewportContext *viewport)
{
    if (sheet == NULL || budget == NULL || document == NULL
        || document->html == NULL || viewport == NULL
        || viewport->css_width <= 0 || viewport->css_height <= 0) return false;
    if (!stylesheet_initialize(
            sheet, budget, viewport->css_width, viewport->css_height)) {
        return false;
    }
    sheet->block_inline_style_attributes =
        !tilefinch_csp_allows_style_attribute(
            &document->content_security_policy);
    sheet->noscript_rendered = document->noscript_rendered;
    sheet->document_rules_deferred = true;
    return true;
}

bool stylesheet_reset_document_rules(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL
        || sheet->viewport_width <= 0 || sheet->viewport_height <= 0) {
        return false;
    }
    Budget *budget = sheet->budget;
    int viewport_width = sheet->viewport_width;
    int viewport_height = sheet->viewport_height;
    bool block_inline_style_attributes =
        sheet->block_inline_style_attributes;
    bool noscript_rendered = sheet->noscript_rendered;
    stylesheet_destroy(sheet);
    bool initialized = stylesheet_initialize(
        sheet, budget, viewport_width, viewport_height);
    if (initialized) {
        sheet->document_rules_deferred = true;
        sheet->block_inline_style_attributes =
            block_inline_style_attributes;
        sheet->noscript_rendered = noscript_rendered;
    }
    return initialized;
}

bool stylesheet_begin_rule_batch(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL || sheet->rule_batch_active) {
        return false;
    }
    stylesheet_suffix_state_begin(sheet);
    sheet->rule_batch_active = true;
    sheet->rule_batch_dirty = false;
    return true;
}

bool stylesheet_end_rule_batch(Stylesheet *sheet)
{
    if (sheet == NULL || !sheet->rule_batch_active) return false;
    sheet->rule_batch_active = false;
    if (sheet->rule_batch_dirty) stylesheet_finalize_rule_order(sheet);
    stylesheet_suffix_state_finish(sheet, true);
    return true;
}

bool stylesheet_build_viewport(Stylesheet *sheet, Budget *budget,
                               const PocDocument *document,
                               int viewport_width, int viewport_height)
{
    ViewportContext viewport;
    if (!viewport_context_init(&viewport, viewport_width, viewport_height,
                               viewport_width, viewport_height)) return false;
    return stylesheet_build_context(sheet, budget, document, &viewport);
}

bool stylesheet_add_css(Stylesheet *sheet, const char *css, size_t length)
{
    return stylesheet_add_css_from_context(
        sheet, css, length, NULL, NULL);
}

void stylesheet_enable_static_custom_element_fallback(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->static_custom_element_fallback) return;
    sheet->static_custom_element_fallback = true;
    /* Matching changed without changing retained rules. Give layout/style
       reuse the same generation signal as an authored stylesheet mutation. */
    sheet->build_generation = stylesheet_next_generation();
}

bool stylesheet_add_css_from(Stylesheet *sheet, const char *css,
                             size_t length, const char *source_base_url)
{
    return stylesheet_add_css_from_context(
        sheet, css, length, source_base_url, NULL);
}

typedef bool (*StylesheetParseBody)(StyleCssParseContext *, const void *);

/* One parse scope owns scratch/origin restoration and suffix-index lifetime.
   As before, a rejected suffix may retain valid preceding rules: this is not
   an atomic rollback of authored declarations. */
static bool stylesheet_run_parse_transaction(
    Stylesheet *sheet, const char *base, const char *policy, unsigned origin,
    bool discover_font_faces, StyleParsedIrBuilder *parsed_ir,
    StylesheetParseBody body, const void *input)
{
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    unsigned saved_origin = sheet->current_origin;
    bool own_suffix_state = !sheet->rule_batch_active;
    if (own_suffix_state) stylesheet_suffix_state_begin(sheet);
    sheet->current_origin = origin;
    sheet->resolve_scratch->current_image_source_base = base;
    sheet->resolve_scratch->current_image_source_referrer_policy = policy;
    sheet->resolve_scratch->current_image_source_slot = 0;
    StyleCssParseContext parse = {
        .sheet = sheet, .parsed_ir = parsed_ir,
        .discover_font_faces = discover_font_faces
    };
    bool parsed = body(&parse, input);
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) style_css_parse_context_dispose(&parse);
    sheet->current_origin = saved_origin;
    *sheet->resolve_scratch = saved_scratch;
    if (parsed) {
        sheet->rule_batch_dirty = true;
        tilefinch_platform_trace_step("css-rule-order");
        if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    }
    tilefinch_platform_trace_step("css-suffix-finish");
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, parsed);
    tilefinch_platform_trace_step("css-added");
    return parsed;
}

typedef struct { const char *css; size_t length; } StyleCssTextInput;

static bool stylesheet_parse_text_body(StyleCssParseContext *parse,
                                       const void *opaque)
{
    const StyleCssTextInput *input = opaque;
    TILEFINCH_WORK_ADD(css_bytes, input->length);
    return parse_css_range(parse, input->css, 0, input->length);
}

typedef struct {
    const unsigned char *data;
    size_t operation_count;
} StyleParsedIrInput;

static bool stylesheet_parse_ir_body(StyleCssParseContext *parse,
                                     const void *opaque)
{
    const StyleParsedIrInput *input = opaque;
    Stylesheet *sheet = parse->sheet;
    size_t at = sizeof(StyleParsedIrHeader);
    bool parsed = true;
    /* Container scopes as the parser entered them: the queries a scope
       left behind, and how deep inside a scope whose query could not be
       registered (its rules are skipped, as the parser skips them). */
    uint8_t previous[STYLE_PARSED_IR_CONTAINER_DEPTH];
    size_t depth = 0, skipped = 0;
    uint8_t outer_query = sheet->current_container_query;
    for (size_t i = 0; parsed && i < input->operation_count; i++) {
        StyleParsedIrOperation operation;
        memcpy(&operation, input->data + at, sizeof(operation));
        at += sizeof(operation);
        const char *selectors = (const char *) input->data + at;
        at += operation.selector_length;
        const char *declarations = (const char *) input->data + at;
        at += operation.declaration_length;
        if (operation.kind == STYLE_PARSED_IR_CONTAINER_BEGIN) {
            uint8_t query = 0;
            if (skipped != 0) {
                skipped++;
            } else if (!style_register_container_query(
                           sheet, selectors, operation.selector_length,
                           sheet->current_container_query, &query)) {
                parsed = false;
            } else if (query == 0) {
                skipped = 1;
            } else {
                previous[depth++] = sheet->current_container_query;
                sheet->current_container_query = query;
            }
            continue;
        }
        if (operation.kind == STYLE_PARSED_IR_CONTAINER_END) {
            if (skipped != 0) skipped--;
            else sheet->current_container_query = previous[--depth];
            continue;
        }
        if (skipped != 0) continue;
        if (operation.kind == STYLE_PARSED_IR_KEYFRAMES) {
            stylesheet_note_motion_source(sheet, selectors,
                operation.selector_length, declarations,
                operation.declaration_length, true);
            continue;
        }
        parsed = selector_list_to_rules(
            parse, selectors, operation.selector_length,
            declarations, operation.declaration_length);
    }
    sheet->current_container_query = outer_query;
    return parsed;
}

typedef struct {
    lxb_dom_node_t *const *elements;
    size_t count;
    const TilefinchContentSecurityPolicy *policy;
} StyleElementsInput;

static bool stylesheet_parse_elements_body(StyleCssParseContext *parse,
                                           const void *opaque)
{
    const StyleElementsInput *input = opaque;
    bool parsed = true;
    for (size_t i = 0; parsed && i < input->count; i++) {
        lxb_dom_node_t *element = input->elements[i];
        size_t name_length = 0;
        const char *name = document_element_name(element, &name_length);
        size_t override_length = 0;
        const char *override = document_cssom_sheet_text(element, &override_length);
        if (name == NULL || !span_equal(name, name_length, "style")) {
            parsed = false;
            break;
        }
        stylesheet_note_style_source(parse->sheet, element);
        if (!tilefinch_csp_allows_inline_style(input->policy, element)) continue;
        unsigned scope_begin = parse->sheet->next_order;
        bool scoped = document_shadow_carrier_containing(element) != NULL;
        parse->sheet->parsing_scoped_source = scoped;
        if (override != NULL) {
            TILEFINCH_WORK_ADD(css_bytes, override_length);
            parsed = parse_css_range(parse, override, 0, override_length);
        }
        for (lxb_dom_node_t *child = override == NULL ? element->first_child : NULL;
             parsed && child != NULL; child = child->next) {
            size_t length = 0;
            const char *css = document_text_data(child, &length);
            if (css == NULL) continue;
            TILEFINCH_WORK_ADD(css_bytes, length);
            parsed = parse_css_range(parse, css, 0, length);
        }
        parse->sheet->parsing_scoped_source = false;
        if (scoped) {
            stylesheet_note_shadow_scope(parse->sheet, element, scope_begin,
                                         parse->sheet->next_order);
        }
    }
    return parsed;
}

static bool stylesheet_add_css_from_context_internal(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    StyleParsedIrBuilder *parsed_ir)
{
    if (sheet == NULL || sheet->budget == NULL
        || (css == NULL && length != 0)) return false;
    /* Empty response bodies are valid stylesheets.  Some transports expose
       them as a non-NULL sentinel while deterministic replay naturally uses
       { NULL, 0 }; both representations must be equivalent.  Returning
       before dropping the index also makes a semantic no-op allocation-free. */
    if (length == 0) return true;
    /* Parsing can append some valid rules before rejecting a malformed
       suffix.  Invalidate observers before the first possible mutation,
       including that conservative failure case. */
    sheet->build_generation = stylesheet_next_generation();
    char normalized_policy[STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY];
    const char *retained_policy = NULL;
    if (source_base_url != NULL) {
        if (!web_font_referrer_policy_normalize(
                source_referrer_policy, normalized_policy)) return false;
        retained_policy = normalized_policy;
    }
    StyleCssTextInput input = {css, length};
    return stylesheet_run_parse_transaction(sheet, source_base_url,
        retained_policy, sheet->current_origin, true, parsed_ir,
        stylesheet_parse_text_body, &input);
}

bool stylesheet_add_css_from_context(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy)
{
    return stylesheet_add_css_from_context_internal(
        sheet, css, length, source_base_url, source_referrer_policy, NULL);
}

static void style_parsed_ir_builder_finish(
    StyleParsedIrBuilder *builder, int viewport_width, int viewport_height,
    size_t source_length, bool parsed,
    unsigned char **ir_data, size_t *ir_length)
{
    if (builder == NULL || builder->budget == NULL
        || ir_data == NULL || ir_length == NULL) return;
    if (parsed && builder->eligible && builder->operation_count != 0
        && builder->operation_count <= UINT32_MAX
        && builder->length <= UINT32_MAX
        /* A response cache keeps IR only when it beats the source; a
           constructed sheet's IR stands in for its parse (see
           stylesheet_parse_adopted_sheet) and is bounded there. */
        && (builder->length < source_length || builder->container_scopes)) {
        unsigned char *result = budget_realloc(
            builder->budget, builder->data, builder->length);
        if (result != NULL) {
            StyleParsedIrHeader header = {
                .magic = STYLE_PARSED_IR_MAGIC,
                .version = STYLE_PARSED_IR_VERSION,
                .flags = (uint16_t) (
                    (builder->has_motion_keyframes
                         ? STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES : 0)
                    | (stylesheet_prefers_dark_color_scheme()
                         ? STYLE_PARSED_IR_PREFERS_DARK : 0)),
                .viewport_width = (uint32_t) viewport_width,
                .viewport_height = (uint32_t) viewport_height,
                .operation_count = (uint32_t) builder->operation_count,
                .payload_bytes = (uint32_t) (
                    builder->length - sizeof(StyleParsedIrHeader))
            };
            memcpy(result, &header, sizeof(header));
            *ir_data = result;
            *ir_length = builder->length;
            builder->data = NULL;
        }
    }
    budget_free(builder->budget, builder->data);
    builder->data = NULL;
}

static bool stylesheet_capture_ir(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    bool container_scopes, unsigned char **ir_data, size_t *ir_length);

bool stylesheet_add_css_from_context_capture_ir(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    unsigned char **ir_data, size_t *ir_length)
{
    return stylesheet_capture_ir(sheet, css, length, source_base_url,
                                 source_referrer_policy, false, ir_data,
                                 ir_length);
}

static bool stylesheet_capture_ir(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    bool container_scopes, unsigned char **ir_data, size_t *ir_length)
{
    if (ir_data != NULL) *ir_data = NULL;
    if (ir_length != NULL) *ir_length = 0;
    if (sheet == NULL || sheet->budget == NULL
        || ir_data == NULL || ir_length == NULL) return false;
    StyleParsedIrBuilder builder = {
        .budget = sheet->budget,
        .length = sizeof(StyleParsedIrHeader),
        /* Finish retains only IR smaller than the source. Stop growing as
           soon as that is impossible; later operations cannot shrink it. */
        .maximum_length = length > STYLE_PARSED_IR_MAX_BYTES
                || container_scopes
            ? STYLE_PARSED_IR_MAX_BYTES : (length != 0 ? length - 1u : 0),
        .eligible = length > sizeof(StyleParsedIrHeader),
        .container_scopes = container_scopes
    };
    bool parsed = stylesheet_add_css_from_context_internal(
        sheet, css, length, source_base_url, source_referrer_policy,
        &builder);
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
        fprintf(stderr,
                "tilefinch-stylesheet-ir action=capture parsed=%d "
                "eligible=%d operations=%zu bytes=%zu\n",
                parsed, builder.eligible, builder.operation_count,
                builder.length);
    }
#endif
    tilefinch_platform_trace_step("css-ir-finish");
    style_parsed_ir_builder_finish(
        &builder, sheet->viewport_width, sheet->viewport_height, length,
        parsed,
        ir_data, ir_length);
    tilefinch_platform_trace_step("css-ir-done");
    return parsed;
}

static bool stylesheet_parsed_ir_validate(
    const Stylesheet *sheet, const unsigned char *ir_data, size_t ir_length,
    StyleParsedIrHeader *header_out)
{
    if (sheet == NULL || ir_data == NULL
        || ir_length < sizeof(StyleParsedIrHeader)
        || ir_length > STYLE_PARSED_IR_MAX_BYTES) return false;
    StyleParsedIrHeader header;
    memcpy(&header, ir_data, sizeof(header));
    if (header.magic != STYLE_PARSED_IR_MAGIC
        || header.version != STYLE_PARSED_IR_VERSION
        || (header.flags & ~(STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES
                             | STYLE_PARSED_IR_PREFERS_DARK)) != 0
        || ((header.flags & STYLE_PARSED_IR_PREFERS_DARK) != 0)
               != stylesheet_prefers_dark_color_scheme()
        || header.operation_count == 0
        || header.operation_count
               > STYLE_PARSED_IR_MAX_BYTES
                   / (sizeof(StyleParsedIrOperation) + 1u)
        || header.viewport_width != (uint32_t) sheet->viewport_width
        || header.viewport_height != (uint32_t) sheet->viewport_height
        || header.payload_bytes != ir_length - sizeof(header)) return false;
    size_t at = sizeof(header);
    size_t container_depth = 0;
    for (size_t i = 0; i < header.operation_count; i++) {
        if (sizeof(StyleParsedIrOperation) > ir_length - at) return false;
        StyleParsedIrOperation operation;
        memcpy(&operation, ir_data + at, sizeof(operation));
        at += sizeof(operation);
        if (operation.kind > STYLE_PARSED_IR_KEYFRAMES
            || operation.selector_length == 0
            || operation.selector_length > ir_length - at) return false;
        /* Scopes must nest within the replay's fixed stack. */
        if (operation.kind == STYLE_PARSED_IR_CONTAINER_BEGIN) {
            if (++container_depth > STYLE_PARSED_IR_CONTAINER_DEPTH)
                return false;
        } else if (operation.kind == STYLE_PARSED_IR_CONTAINER_END) {
            if (container_depth == 0) return false;
            container_depth--;
        }
        at += operation.selector_length;
        if (operation.declaration_length > ir_length - at) return false;
        at += operation.declaration_length;
    }
    if (at != ir_length || container_depth != 0) return false;
    if (header_out != NULL) *header_out = header;
    return true;
}

bool stylesheet_parsed_ir_matches(
    const Stylesheet *sheet, const unsigned char *ir_data, size_t ir_length)
{
    return stylesheet_parsed_ir_validate(
        sheet, ir_data, ir_length, NULL);
}

StyleParsedIrApplyResult stylesheet_add_parsed_ir_from_context(
    Stylesheet *sheet, const unsigned char *ir_data, size_t ir_length,
    const char *source_base_url, const char *source_referrer_policy,
    size_t *operation_count)
{
    if (operation_count != NULL) *operation_count = 0;
    StyleParsedIrHeader header;
    if (sheet == NULL || sheet->budget == NULL
        || sheet->source_rule_limit_end != 0
        || sheet->source_rule_head_limit_end != 0
        || sheet->source_rule_secondary_limit_end != 0
        || sheet->source_rule_relevant_limit_end != 0
        || sheet->source_rule_priority_token_bloom != NULL
        || sheet->source_rule_priority_token_bloom_words != 0
        || sheet->source_rule_token_bloom != NULL
        || sheet->source_rule_token_bloom_words != 0
        || sheet->source_rule_tail_begin != NULL
        || !stylesheet_parsed_ir_validate(
               sheet, ir_data, ir_length, &header)) {
        return STYLE_PARSED_IR_REJECTED;
    }
    char normalized_policy[STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY];
    const char *retained_policy = NULL;
    if (source_base_url != NULL) {
        if (!web_font_referrer_policy_normalize(
                source_referrer_policy, normalized_policy)) {
            return STYLE_PARSED_IR_FAILED;
        }
        retained_policy = normalized_policy;
    }
    sheet->build_generation = stylesheet_next_generation();
    if ((header.flags & STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES) != 0) {
        sheet->has_motion_keyframes = true;
    }
    StyleParsedIrInput input = {ir_data, header.operation_count};
    if (!stylesheet_run_parse_transaction(sheet, source_base_url,
            retained_policy, sheet->current_origin, true, NULL,
            stylesheet_parse_ir_body, &input)) return STYLE_PARSED_IR_FAILED;
    if (operation_count != NULL) *operation_count = header.operation_count;
    return STYLE_PARSED_IR_APPLIED;
}

void stylesheet_note_style_source(Stylesheet *sheet,
                                  const lxb_dom_node_t *element)
{
    if (sheet == NULL || element == NULL || sheet->style_sources_bounded_out)
        return;
    for (size_t i = 0; i < sheet->style_source_count; i++) {
        if (sheet->style_source_nodes[i] == element) return;
    }
    if (sheet->style_source_count == STYLE_SOURCE_NODE_LIMIT) {
        sheet->style_sources_bounded_out = true;
        return;
    }
    sheet->style_source_first_order[sheet->style_source_count] =
        sheet->next_order;
    sheet->style_source_nodes[sheet->style_source_count++] = element;
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
    if (sheet->shadow_scope_count == sheet->shadow_scope_capacity) {
        if (sheet->shadow_scope_capacity >= STYLE_SHADOW_SCOPE_LIMIT) return;
        size_t capacity = sheet->shadow_scope_capacity == 0
            ? 8u : sheet->shadow_scope_capacity * 2u;
        if (capacity > STYLE_SHADOW_SCOPE_LIMIT)
            capacity = STYLE_SHADOW_SCOPE_LIMIT;
        StyleShadowScope *grown = budget_realloc(
            sheet->budget, sheet->shadow_scopes,
            capacity * sizeof(*grown));
        if (grown == NULL) return;
        sheet->shadow_scopes = grown;
        sheet->shadow_scope_capacity = (uint16_t) capacity;
    }
    /* Ascending by begin; a source parsed before an earlier one (a late
       insertion) shifts the later ranges up. */
    size_t at = sheet->shadow_scope_count;
    while (at > 0 && sheet->shadow_scopes[at - 1u].begin > begin) {
        sheet->shadow_scopes[at] = sheet->shadow_scopes[at - 1u];
        at--;
    }
    sheet->shadow_scopes[at] = (StyleShadowScope) {begin, end, carrier};
    sheet->shadow_scope_count++;
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

size_t stylesheet_style_source_count(const Stylesheet *sheet)
{
    return sheet == NULL ? 0 : sheet->style_source_count;
}

bool stylesheet_style_source_known(const Stylesheet *sheet,
                                   const lxb_dom_node_t *element)
{
    if (sheet == NULL || element == NULL) return false;
    for (size_t i = 0; i < sheet->style_source_count; i++) {
        if (sheet->style_source_nodes[i] == element) return true;
    }
    return false;
}

const lxb_dom_node_t *stylesheet_last_style_source(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->style_source_count == 0) return NULL;
    return sheet->style_source_nodes[sheet->style_source_count - 1];
}

/* Whether changing the class/id tokens `hashes` on some element can change
   which elements the rule matches. With own_key false only tokens outside
   the rightmost compound count: a change on the matched element itself is
   the caller's concern. */
static bool style_rule_tokens_may_change_match(
    const Stylesheet *sheet, const StyleRule *rule, const StyleRuleFilter *filter,
    const uint32_t *hashes, size_t count, bool own_key)
{
    if (filter->relational_opaque || (own_key && !rule->has_fast_key))
        return true;
    for (size_t r = 0; r < filter->relational_count; r++) {
        for (size_t h = 0; h < count; h++) {
            if (sheet->rule_relational_tokens[filter->relational_offset + r]
                == hashes[h]) return true;
        }
    }
    if (own_key
        && (rule->type == SELECTOR_CLASS || rule->type == SELECTOR_ID)) {
        const char *key = style_rule_fast_key(rule);
        if (key == NULL) return true;
        uint32_t hash = stylesheet_identity_token_hash(
            rule->type == SELECTOR_ID, key, rule->fast_key_length);
        for (size_t h = 0; h < count; h++) {
            if (hash == hashes[h]) return true;
        }
    }
    return false;
}

bool stylesheet_tokens_may_affect_discovery(
    const Stylesheet *sheet, const uint32_t *hashes, size_t count)
{
    if (sheet == NULL || hashes == NULL || count == 0) return true;
    if (sheet->rule_filters == NULL || !sheet->rule_index_ready) return true;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRuleFilter *filter = &sheet->rule_filters[i];
        if (filter->discovery
            && style_rule_tokens_may_change_match(
                   sheet, &sheet->rules[i], filter, hashes, count, true))
            return true;
    }
    return false;
}

typedef struct {
    uint32_t *hashes;
    size_t count;
    size_t capacity;
} SvgRasterTokenList;

static bool svg_raster_token_add(Stylesheet *sheet, SvgRasterTokenList *list,
                                 uint32_t hash)
{
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 64u : list->capacity * 2u;
        if (capacity > SIZE_MAX / sizeof(*list->hashes)) return false;
        uint32_t *grown = budget_realloc(
            sheet->budget, list->hashes, capacity * sizeof(*grown));
        if (grown == NULL) return false;
        list->hashes = grown;
        list->capacity = capacity;
    }
    list->hashes[list->count++] = hash;
    return true;
}

static size_t svg_raster_skip_escape(const char *text, size_t length,
                                     size_t at)
{
    /* `at` is the backslash; a hex escape may end in one space, which is
       part of the escape rather than a descendant combinator. */
    size_t next = at + 1u;
    unsigned digit = 0;
    size_t digits = 0;
    while (next < length && digits < 6
           && style_css_hex_digit((unsigned char) text[next], &digit)) {
        next++;
        digits++;
    }
    if (digits == 0) return next < length ? next + 1u : next;
    if (next < length && isspace((unsigned char) text[next])) next++;
    return next;
}

/* Appends the class/id tokens of one custom-rule selector, unescaped as a
   live class list spells them. With relational_only, just those outside the
   rightmost compound or inside a functional pseudo-class. False when the
   selector's token dependency cannot be listed (an identity attribute
   selector, `of S`) or the list could not grow. */
static bool svg_raster_selector_tokens(Stylesheet *sheet, const char *text,
                                       size_t length, bool relational_only,
                                       SvgRasterTokenList *list)
{
    if (selector_text_has_ci(text, length, ":nth-")
        && selector_text_has_ci(text, length, " of ")) return false;
    size_t split = 0;
    for (int pass = relational_only ? 0 : 1; pass < 2; pass++) {
        unsigned square = 0, parentheses = 0;
        char quote = 0;
        for (size_t at = 0; at < length; at++) {
            char value = text[at];
            if (quote != 0) {
                if (value == '\\') at++;
                else if (value == quote) quote = 0;
                continue;
            }
            if (value == '\\') {
                at = svg_raster_skip_escape(text, length, at) - 1u;
                continue;
            }
            if (square != 0) {
                if (value == '"' || value == '\'') quote = value;
                else if (value == ']') square--;
                continue;
            }
            bool counted = !relational_only || at < split
                || parentheses != 0;
            if (value == '[') {
                square++;
                if (pass == 1 && counted
                    && attribute_selector_targets_identity(
                           text + at + 1u, length - at - 1u)) return false;
                continue;
            }
            if (value == '(') { parentheses++; continue; }
            if (value == ')') {
                if (parentheses != 0) parentheses--;
                continue;
            }
            if (pass == 0) {
                if (parentheses == 0
                    && (isspace((unsigned char) value) || value == '>'
                        || value == '+' || value == '~')) split = at + 1u;
                continue;
            }
            if (value != '.' && value != '#') continue;
            size_t begin = at + 1u;
            size_t end = skip_selector_identifier(text, length, begin);
            if (end == begin) continue;
            at = end - 1u;
            if (!counted) continue;
            const char *token = text + begin;
            size_t token_length = end - begin;
            char unescaped[STYLE_CUSTOM_SELECTOR_CAPACITY];
            if (memchr(token, '\\', token_length) != NULL) {
                if (!css_unescape_value(token, token_length, unescaped,
                                        sizeof(unescaped))) return false;
                token = unescaped;
                token_length = strlen(unescaped);
            }
            if (!svg_raster_token_add(
                    sheet, list, stylesheet_identity_token_hash(
                        value == '#', token, token_length))) return false;
        }
    }
    return true;
}

static int compare_svg_raster_tokens(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *) left, b = *(const uint32_t *) right;
    return (a > b) - (a < b);
}

static void stylesheet_prepare_svg_raster_tokens(Stylesheet *sheet)
{
    if (sheet->svg_raster_tokens_ready
        && sheet->svg_raster_token_generation == sheet->build_generation
        && sheet->svg_raster_token_rules == sheet->custom_rule_count) return;
    budget_free(sheet->budget, sheet->svg_raster_tokens);
    sheet->svg_raster_tokens = NULL;
    sheet->svg_raster_token_count = 0;
    SvgRasterTokenList list = {0};
    bool opaque = false;
    for (size_t i = 0; !opaque && i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        bool custom_property = rule->name_length > 2
            && rule->name[0] == '-' && rule->name[1] == '-';
        if (!custom_property
            && retained_presentation_index(
                   rule->name, rule->name_length) < 0) continue;
        /* Of the presentation values only opacity is not inherited: it
           reaches a raster when it matches the SVG itself, whose own class
           changes always refresh it. */
        bool relational_only = !custom_property
            && span_case_equal(rule->name, rule->name_length, "opacity");
        opaque = rule->selector == NULL
            || !svg_raster_selector_tokens(
                   sheet, rule->selector, rule->selector_length,
                   relational_only, &list);
    }
    if (opaque || list.count > UINT32_MAX) {
        budget_free(sheet->budget, list.hashes);
        list = (SvgRasterTokenList) {0};
        opaque = true;
    } else if (list.count != 0) {
        qsort(list.hashes, list.count, sizeof(*list.hashes),
              compare_svg_raster_tokens);
        size_t unique = 1;
        for (size_t i = 1; i < list.count; i++) {
            if (list.hashes[i] != list.hashes[unique - 1u])
                list.hashes[unique++] = list.hashes[i];
        }
        uint32_t *compact = budget_realloc(
            sheet->budget, list.hashes, unique * sizeof(*compact));
        if (compact != NULL) list.hashes = compact;
        list.count = unique;
    }
    sheet->svg_raster_tokens = list.hashes;
    sheet->svg_raster_token_count = (uint32_t) list.count;
    sheet->svg_raster_tokens_opaque = opaque;
    sheet->svg_raster_token_generation = sheet->build_generation;
    sheet->svg_raster_token_rules = sheet->custom_rule_count;
    sheet->svg_raster_tokens_ready = true;
}

bool stylesheet_tokens_may_affect_svg_raster(
    const Stylesheet *const_sheet, const uint32_t *hashes, size_t count)
{
    if (const_sheet == NULL || hashes == NULL || count == 0
        || const_sheet->budget == NULL) return true;
    if (const_sheet->rule_filters == NULL || !const_sheet->rule_index_ready)
        return true;
    for (size_t i = 0; i < const_sheet->count; i++) {
        const StyleRuleFilter *filter = &const_sheet->rule_filters[i];
        if (filter->svg_raster != 0
            && style_rule_tokens_may_change_match(
                   const_sheet, &const_sheet->rules[i], filter, hashes, count,
                   (filter->svg_raster & STYLE_RULE_SVG_RASTER_INHERITED)
                       != 0)) return true;
    }
    /* Custom properties and SVG presentation values are custom rules, not
       indexed rules; their tokens are listed once per sheet generation. */
    Stylesheet *sheet = (Stylesheet *) const_sheet;
    stylesheet_prepare_svg_raster_tokens(sheet);
    if (sheet->svg_raster_tokens_opaque) return true;
    for (size_t h = 0; h < count; h++) {
        if (sheet->svg_raster_token_count != 0
            && bsearch(&hashes[h], sheet->svg_raster_tokens,
                       sheet->svg_raster_token_count,
                       sizeof(*sheet->svg_raster_tokens),
                       compare_svg_raster_tokens) != NULL) return true;
    }
    return false;
}

/* Attribute names borrowed from selector text while the sorted set is built;
   the set itself keeps only unique lower-cased copies. */
typedef struct {
    const char *text;
    size_t length;
} SelectorAttributeName;

typedef struct {
    SelectorAttributeName *names;
    size_t count;
    size_t capacity;
} SelectorAttributeNameList;

/* Bounds on one build: unique names held while scanning, and the retained
   set. A sheet beyond them answers conservatively. */
#define STYLE_SELECTOR_ATTRIBUTE_NAME_LIMIT 2048u
#define STYLE_SELECTOR_ATTRIBUTE_BYTE_LIMIT (64u * 1024u)

static int selector_attribute_lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int compare_selector_attribute_names(const void *left,
                                            const void *right)
{
    const SelectorAttributeName *a = left, *b = right;
    size_t shared = a->length < b->length ? a->length : b->length;
    for (size_t i = 0; i < shared; i++) {
        int x = selector_attribute_lower((unsigned char) a->text[i]);
        int y = selector_attribute_lower((unsigned char) b->text[i]);
        if (x != y) return x - y;
    }
    return (a->length > b->length) - (a->length < b->length);
}

/* Sorts the list and drops repeated names (ASCII case-insensitive). */
static void selector_attribute_names_compact(SelectorAttributeNameList *list)
{
    if (list->count < 2u) return;
    qsort(list->names, list->count, sizeof(*list->names),
          compare_selector_attribute_names);
    size_t unique = 1;
    for (size_t i = 1; i < list->count; i++) {
        if (compare_selector_attribute_names(
                &list->names[unique - 1u], &list->names[i]) != 0)
            list->names[unique++] = list->names[i];
    }
    list->count = unique;
}

static bool selector_attribute_name_byte(unsigned char value)
{
    return value >= 0x80 || isalnum(value) || value == '-' || value == '_';
}

/* Appends the attribute names one selector's attribute selectors test,
   borrowing the selector text. Escapes and quoted strings outside an
   attribute selector (escaped Tailwind class names, :lang("x")) are not
   attribute selectors. False when a name cannot be listed: an escaped or
   namespace-qualified name, a form this scan does not parse, or a full
   list. */
static bool selector_attribute_names_collect(
    Stylesheet *sheet, const char *text, size_t length,
    SelectorAttributeNameList *list)
{
    /* Most selectors test no attribute at all. */
    if (memchr(text, '[', length) == NULL) return true;
    char quote = 0;
    for (size_t at = 0; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == '\\') at++;
            else if (value == quote) quote = 0;
            continue;
        }
        if (value == '\\') {
            at = svg_raster_skip_escape(text, length, at) - 1u;
            continue;
        }
        if (value == '"' || value == '\'') {
            quote = value;
            continue;
        }
        if (value != '[') continue;
        size_t begin = at + 1u;
        while (begin < length && isspace((unsigned char) text[begin])) begin++;
        size_t end = begin;
        while (end < length
               && selector_attribute_name_byte((unsigned char) text[end])) {
            end++;
        }
        size_t next = end;
        while (next < length && isspace((unsigned char) text[next])) next++;
        /* `*|`, `|name`, `name\..` and `ns|name` all land here. */
        if (end == begin || next == length) return false;
        char match = text[next];
        if (match != ']' && match != '='
            && !((match == '~' || match == '|' || match == '^'
                  || match == '$' || match == '*')
                 && next + 1u < length && text[next + 1u] == '=')) {
            return false;
        }
        const SelectorAttributeName *last = list->count == 0 ? NULL
            : &list->names[list->count - 1u];
        bool repeated = last != NULL && last->length == end - begin
            && memcmp(last->text, text + begin, end - begin) == 0;
        if (!repeated && list->count == STYLE_SELECTOR_ATTRIBUTE_NAME_LIMIT) {
            selector_attribute_names_compact(list);
            if (list->count == STYLE_SELECTOR_ATTRIBUTE_NAME_LIMIT)
                return false;
        }
        if (!repeated && list->count == list->capacity) {
            size_t capacity = list->capacity == 0 ? 64u : list->capacity * 2u;
            SelectorAttributeName *grown = budget_realloc(
                sheet->budget, list->names, capacity * sizeof(*grown));
            if (grown == NULL) return false;
            list->names = grown;
            list->capacity = capacity;
        }
        if (!repeated) {
            list->names[list->count++] = (SelectorAttributeName) {
                text + begin, end - begin
            };
        }
        /* The value may hold escapes, quotes and brackets of its own. */
        char value_quote = 0;
        at = next;
        for (; at < length; at++) {
            char inner = text[at];
            if (value_quote != 0) {
                if (inner == '\\') at++;
                else if (inner == value_quote) value_quote = 0;
            } else if (inner == '\\') {
                at++;
            } else if (inner == '"' || inner == '\'') {
                value_quote = inner;
            } else if (inner == ']') {
                break;
            }
        }
        if (at >= length) return false;
    }
    return quote == 0;
}

static void stylesheet_prepare_selector_attribute_names(Stylesheet *sheet)
{
    if (sheet->selector_attribute_names_ready
        && sheet->selector_attribute_generation == sheet->build_generation
        && sheet->selector_attribute_rules == sheet->count
        && sheet->selector_attribute_custom_rules
               == sheet->custom_rule_count) return;
    budget_free(sheet->budget, sheet->selector_attribute_names);
    sheet->selector_attribute_names = NULL;
    sheet->selector_attribute_name_count = 0;
    sheet->selector_attribute_name_bytes = 0;
    SelectorAttributeNameList list = {0};
    bool opaque = false;
    for (size_t i = 0; !opaque && i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        opaque = rule->selector != NULL
            && !selector_attribute_names_collect(
                   sheet, rule->selector, rule->selector_length, &list);
    }
    for (size_t i = 0; !opaque && i < sheet->custom_rule_count; i++) {
        const StyleCustomRule *rule = &sheet->custom_rules[i];
        opaque = rule->selector != NULL
            && !selector_attribute_names_collect(
                   sheet, rule->selector, rule->selector_length, &list);
    }
    /* One allocation: sorted offsets, then "name\0" bytes. */
    size_t unique = 0, bytes = 0;
    if (!opaque) {
        selector_attribute_names_compact(&list);
        unique = list.count;
        for (size_t i = 0; i < unique; i++) bytes += list.names[i].length + 1u;
        opaque = bytes > STYLE_SELECTOR_ATTRIBUTE_BYTE_LIMIT;
    }
    uint32_t *offsets = NULL;
    if (!opaque && unique != 0) {
        offsets = budget_realloc(
            sheet->budget, NULL, unique * sizeof(*offsets) + bytes);
        opaque = offsets == NULL;
    }
    if (offsets != NULL) {
        char *names = (char *) (offsets + unique);
        size_t used = 0;
        for (size_t i = 0; i < unique; i++) {
            offsets[i] = (uint32_t) used;
            for (size_t c = 0; c < list.names[i].length; c++) {
                names[used++] = (char) selector_attribute_lower(
                    (unsigned char) list.names[i].text[c]);
            }
            names[used++] = '\0';
        }
        sheet->selector_attribute_names = offsets;
        sheet->selector_attribute_name_count = (uint32_t) unique;
        sheet->selector_attribute_name_bytes =
            unique * sizeof(*offsets) + bytes;
    }
    budget_free(sheet->budget, list.names);
    sheet->selector_attribute_names_opaque = opaque;
    sheet->selector_attribute_generation = sheet->build_generation;
    sheet->selector_attribute_rules = sheet->count;
    sheet->selector_attribute_custom_rules = sheet->custom_rule_count;
    sheet->selector_attribute_names_ready = true;
}

/* Orders a listed (lower-case) name against the first name_length bytes of
   the query: zero when the listed name begins with the query. */
static int selector_attribute_name_order(const char *listed,
                                         const char *name,
                                         size_t name_length)
{
    for (size_t i = 0; i < name_length; i++) {
        int x = (unsigned char) listed[i];
        int y = selector_attribute_lower((unsigned char) name[i]);
        if (x != y) return x - y;
    }
    return 0;
}

bool stylesheet_selectors_reference_attribute_prefix(
    const Stylesheet *const_sheet, const char *name, size_t name_length)
{
    if (const_sheet == NULL || name == NULL || name_length == 0) return true;
    if (const_sheet->count == 0 && const_sheet->custom_rule_count == 0)
        return false;
    if (const_sheet->budget == NULL) return true;
    /* The names are listed once per sheet generation and rule counts. */
    Stylesheet *sheet = (Stylesheet *) const_sheet;
    stylesheet_prepare_selector_attribute_names(sheet);
    if (sheet->selector_attribute_names_opaque) return true;
    const uint32_t *offsets = sheet->selector_attribute_names;
    const char *names = (const char *) (offsets
        + sheet->selector_attribute_name_count);
    size_t low = 0, high = sheet->selector_attribute_name_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (selector_attribute_name_order(
                names + offsets[middle], name, name_length) < 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    /* The first name not below the query begins with it, if any does. */
    return low < sheet->selector_attribute_name_count
        && selector_attribute_name_order(
               names + offsets[low], name, name_length) == 0;
}

void stylesheet_append_result_release(Stylesheet *sheet,
                                      StylesheetAppendResult *result)
{
    if (sheet == NULL || result == NULL) return;
    budget_free(sheet->budget, result->remap);
    result->remap = NULL;
    budget_free(sheet->budget, result->appended);
    result->appended = NULL;
    result->appended_count = 0;
}

static unsigned stylesheet_inserted_rule_order(
    unsigned order, unsigned tail_start, unsigned boundary, unsigned count)
{
    if (order == UINT_MAX) return order;
    if (order >= tail_start) return order - tail_start + boundary;
    return order >= boundary ? order + count : order;
}

static int stylesheet_compare_revert_order(const void *a, const void *b)
{
    unsigned left = ((const StyleRevertRuleMask *) a)->order;
    unsigned right = ((const StyleRevertRuleMask *) b)->order;
    return (left > right) - (left < right);
}

typedef struct {
    lxb_dom_node_t *const *elements;
    size_t count;
    const TilefinchContentSecurityPolicy *policy;
} StyleAppendElementsInput;

static bool stylesheet_append_elements_source(Stylesheet *sheet,
                                              void *opaque)
{
    const StyleAppendElementsInput *input = opaque;
    return stylesheet_append_style_elements(
        sheet, input->elements, input->count, input->policy);
}

bool stylesheet_append_style_elements_tracked(
    Stylesheet *sheet, lxb_dom_node_t *const *elements, size_t count,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t after_source, StylesheetAppendResult *result)
{
    StyleAppendElementsInput input = {
        elements, count, content_security_policy
    };
    return stylesheet_append_sources_tracked(
        sheet, stylesheet_append_elements_source, &input, after_source,
        result);
}

bool stylesheet_append_sources_tracked(
    Stylesheet *sheet, StylesheetSourceAppender append, void *opaque,
    size_t after_source, StylesheetAppendResult *result)
{
    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    if (sheet == NULL || sheet->budget == NULL || append == NULL) return false;
    size_t old_count = sheet->count;
    size_t old_sources = sheet->style_source_count;
    if (old_count > UINT16_MAX || after_source > old_sources) return false;
    unsigned *old_orders = old_count == 0 ? NULL
        : budget_malloc(sheet->budget, old_count * sizeof(*old_orders));
    bool old_identity_valid = old_count == 0 || old_orders != NULL;
    for (size_t i = 0; i < old_count; i++) {
        if (!old_identity_valid) break;
        if (sheet->rules[i].order > (UINT_MAX - 1u) / 2u) {
            old_identity_valid = false;
            break;
        }
        old_orders[i] = sheet->rules[i].order * 2u
            + (sheet->rules[i].important ? 1u : 0u);
    }
    uint64_t signature_before = stylesheet_parse_context_signature(sheet);
    unsigned tail_start = sheet->next_order;
    bool ok = append(sheet, opaque);
    if (!ok) {
        budget_free(sheet->budget, old_orders);
        return false;
    }
    result->old_count = old_count;
    result->context_changed =
        stylesheet_parse_context_signature(sheet) != signature_before;
    /* A selector's normal and important declarations share `order`. Their
       pair (order, importance) survives the cascade re-sort. The appended
       rules were numbered after every existing rule; move them to their
       document position by shifting the
       later rules up, then re-sort. Both the selector program and the rule
       index key on rule positions or orders, so they rebuild lazily. */
    unsigned appended_orders = sheet->next_order - tail_start;
    unsigned boundary = after_source < old_sources
        ? sheet->style_source_first_order[after_source] : tail_start;
    bool shifted = boundary != tail_start && appended_orders != 0;
    if (shifted) {
        for (size_t j = 0; j < sheet->count; j++) {
            sheet->rules[j].order = stylesheet_inserted_rule_order(
                sheet->rules[j].order, tail_start, boundary, appended_orders);
        }
        /* Custom/retained declarations and rollback masks share source-order
           identity with ordinary rules. Index invalidation alone cannot
           repair their cascade precedence after a non-tail insertion. */
        for (size_t j = 0; j < sheet->custom_rule_count; j++) {
            /* Retained rules pack declaration order in the low eight bits. */
            unsigned packed = sheet->custom_rules[j].order;
            unsigned order = stylesheet_inserted_rule_order(
                packed >> 8, tail_start, boundary, appended_orders);
            sheet->custom_rules[j].order =
                (order <= (UINT_MAX >> 8) ? order << 8
                                         : UINT_MAX - UINT8_MAX)
                | (packed & UINT8_MAX);
        }
        for (size_t j = 0; j < sheet->transition_rule_count; j++) {
            unsigned packed = sheet->transition_rules[j].order;
            unsigned order = stylesheet_inserted_rule_order(
                packed >> 8, tail_start, boundary, appended_orders);
            sheet->transition_rules[j].order =
                (order <= (UINT_MAX >> 8) ? order << 8
                                         : UINT_MAX - UINT8_MAX)
                | (packed & UINT8_MAX);
        }
        for (size_t j = 0; j < sheet->revert_rule_mask_count; j++) {
            sheet->revert_rule_masks[j].order = stylesheet_inserted_rule_order(
                sheet->revert_rule_masks[j].order, tail_start, boundary,
                appended_orders);
        }
        /* Shadow-tree style ranges move with their rules; a range never
           straddles the boundary, so its ends map with it. Re-sort after. */
        for (size_t j = 0; j < sheet->shadow_scope_count; j++) {
            StyleShadowScope *scope = &sheet->shadow_scopes[j];
            unsigned last = stylesheet_inserted_rule_order(
                scope->end - 1u, tail_start, boundary, appended_orders);
            scope->begin = stylesheet_inserted_rule_order(
                scope->begin, tail_start, boundary, appended_orders);
            scope->end = last + 1u;
        }
        for (size_t j = 1; j < sheet->shadow_scope_count; j++) {
            StyleShadowScope moving = sheet->shadow_scopes[j];
            size_t k = j;
            while (k > 0 && sheet->shadow_scopes[k - 1u].begin > moving.begin) {
                sheet->shadow_scopes[k] = sheet->shadow_scopes[k - 1u];
                k--;
            }
            sheet->shadow_scopes[k] = moving;
        }
        if (sheet->revert_rule_mask_count > 1) {
            qsort(sheet->revert_rule_masks, sheet->revert_rule_mask_count,
                  sizeof(*sheet->revert_rule_masks),
                  stylesheet_compare_revert_order);
        }
        stylesheet_drop_selector_program(sheet);
        stylesheet_drop_rule_index(sheet);
        stylesheet_drop_custom_rule_index(sheet);
        stylesheet_finalize_rule_order(sheet);
    }
    /* Keep the source list in document order, with first orders matching
       the moved rules. A source that parsed no rule (an empty block, a
       sheet whose fetch failed) still takes its place: it is a boundary. */
    if (boundary != tail_start && !sheet->style_sources_bounded_out) {
        for (size_t s = 0; s < sheet->style_source_count; s++) {
            unsigned order = sheet->style_source_first_order[s];
            if (s >= old_sources) {
                sheet->style_source_first_order[s] =
                    order - tail_start + boundary;
            } else if (order >= boundary) {
                sheet->style_source_first_order[s] = order + appended_orders;
            }
        }
        size_t new_sources = sheet->style_source_count - old_sources;
        if (new_sources != 0 && after_source < old_sources) {
            const lxb_dom_node_t *nodes[STYLE_SOURCE_NODE_LIMIT];
            unsigned firsts[STYLE_SOURCE_NODE_LIMIT];
            memcpy(nodes, sheet->style_source_nodes, sizeof(nodes));
            memcpy(firsts, sheet->style_source_first_order, sizeof(firsts));
            size_t at = 0;
            for (size_t s = 0; s < after_source; s++, at++) {
                sheet->style_source_nodes[at] = nodes[s];
                sheet->style_source_first_order[at] = firsts[s];
            }
            for (size_t s = old_sources; s < old_sources + new_sources; s++, at++) {
                sheet->style_source_nodes[at] = nodes[s];
                sheet->style_source_first_order[at] = firsts[s];
            }
            for (size_t s = after_source; s < old_sources; s++, at++) {
                sheet->style_source_nodes[at] = nodes[s];
                sheet->style_source_first_order[at] = firsts[s];
            }
        }
    }
    size_t order_span = (size_t) sheet->next_order;
    bool identity_span_valid = order_span <= (SIZE_MAX / 2u) - 1u
        && sheet->next_order <= (UINT_MAX - 1u) / 2u;
    order_span = identity_span_valid ? (order_span + 1u) * 2u : 0;
    uint16_t *by_order = old_identity_valid && identity_span_valid
        && order_span <= SIZE_MAX / sizeof(*by_order)
        && sheet->count <= UINT16_MAX
        ? budget_malloc(sheet->budget, order_span * sizeof(*by_order)) : NULL;
    uint8_t *old_present = by_order == NULL ? NULL
        : budget_calloc(sheet->budget, order_span, sizeof(*old_present));
    if (by_order == NULL || old_present == NULL) {
        budget_free(sheet->budget, by_order);
        budget_free(sheet->budget, old_present);
        budget_free(sheet->budget, old_orders);
        result->appended_bounded_out = true;
        return true;
    }
    memset(by_order, 0xff, order_span * sizeof(*by_order));
    bool remap_valid = true;
    for (size_t j = 0; j < sheet->count; j++) {
        if (sheet->rules[j].order > (UINT_MAX - 1u) / 2u) {
            remap_valid = false;
            break;
        }
        size_t key = (size_t) sheet->rules[j].order * 2u
            + (sheet->rules[j].important ? 1u : 0u);
        if (key >= order_span || by_order[key] != UINT16_MAX) {
            remap_valid = false;
            break;
        }
        by_order[key] = (uint16_t) j;
    }
    uint16_t *remap = old_count == 0 ? NULL
        : budget_malloc(sheet->budget, old_count * sizeof(*remap));
    if (old_count != 0 && remap == NULL) remap_valid = false;
    for (size_t i = 0; remap_valid && i < old_count; i++) {
        unsigned order = old_orders[i] / 2u;
        unsigned important = old_orders[i] & 1u;
        if (shifted && order != UINT_MAX && order >= boundary) {
            if (appended_orders > (UINT_MAX - 1u) / 2u - order) {
                remap_valid = false;
                break;
            }
            order += appended_orders;
        }
        size_t key = (size_t) order * 2u + important;
        if (key >= order_span || by_order[key] == UINT16_MAX) {
            remap_valid = false;
            break;
        }
        remap[i] = by_order[key];
        old_present[key] = 1;
    }
    if (remap_valid) {
        /* Every prior rule mapped, so the new rules are exactly the
           remainder; the module sheets a page inserts late can carry a
           few hundred of them. */
        size_t appended_capacity = sheet->count - old_count;
        result->appended = appended_capacity == 0 ? NULL
            : budget_malloc(sheet->budget,
                            appended_capacity * sizeof(*result->appended));
        if (appended_capacity != 0 && result->appended == NULL) {
            result->appended_bounded_out = true;
        }
        for (size_t j = 0; !result->appended_bounded_out && j < sheet->count;
             j++) {
            size_t key = (size_t) sheet->rules[j].order * 2u
                + (sheet->rules[j].important ? 1u : 0u);
            if (key < order_span && old_present[key]) continue;
            if (result->appended_count == appended_capacity) {
                result->appended_bounded_out = true;
                break;
            }
            result->appended[result->appended_count++] = (uint32_t) j;
        }
        result->remap = remap;
    } else {
        budget_free(sheet->budget, remap);
        result->appended_bounded_out = true;
    }
    budget_free(sheet->budget, by_order);
    budget_free(sheet->budget, old_present);
    budget_free(sheet->budget, old_orders);
    return true;
}

bool stylesheet_append_style_elements(
    Stylesheet *sheet, lxb_dom_node_t *const *elements, size_t count,
    const TilefinchContentSecurityPolicy *content_security_policy)
{
    if (sheet == NULL || sheet->budget == NULL
        || (elements == NULL && count != 0)) return false;
    if (count == 0) return true;
    sheet->build_generation = stylesheet_next_generation();
    StyleElementsInput input = {elements, count, content_security_policy};
    return stylesheet_run_parse_transaction(sheet, NULL, NULL,
        sheet->current_origin, true, NULL, stylesheet_parse_elements_body,
        &input);
}

bool stylesheet_add_style_element(
    Stylesheet *sheet, lxb_dom_node_t *element,
    const TilefinchContentSecurityPolicy *content_security_policy)
{
    lxb_dom_node_t *elements[] = {element};
    return stylesheet_append_style_elements(
        sheet, elements, 1, content_security_policy);
}

/* Parses one adopted sheet from its text, or replays the parsed form the
   document cached at this revision, and caches what a fresh parse built. A
   replay is the parse it stands for: the IR re-derives every declaration
   against this sheet's intern tables, and the selector fragment is
   verified against the rules it seeds (a miss changes nothing). */
static bool stylesheet_parse_adopted_sheet_text(
    Stylesheet *sheet, const PocDocument *document, lxb_dom_node_t *node,
    uint32_t revision, const char *css, size_t length);

static bool stylesheet_parse_adopted_sheet(Stylesheet *sheet,
                                           const PocDocument *document,
                                           lxb_dom_node_t *node,
                                           uint32_t revision)
{
    size_t length = 0;
    const char *css = document_cssom_sheet_text(node, &length);
    for (lxb_dom_node_t *child = css == NULL ? node->first_child : NULL; child != NULL;
         child = child->next) {
        /* The bridge keeps a constructed sheet's text in one node. */
        if (css != NULL) return stylesheet_add_style_element(sheet, node, NULL);
        css = document_text_data(child, &length);
    }
    stylesheet_note_style_source(sheet, node);
    if (css == NULL || length == 0) return true;
    /* A constructed sheet may hold :host/::slotted() rules: whether they
       apply is settled per adopting root (style_shadow_scope_admits). */
    sheet->parsing_scoped_source = true;
    bool parsed_ok = stylesheet_parse_adopted_sheet_text(
        sheet, document, node, revision, css, length);
    sheet->parsing_scoped_source = false;
    return parsed_ok;
}

static bool stylesheet_parse_adopted_sheet_text(
    Stylesheet *sheet, const PocDocument *document, lxb_dom_node_t *node,
    uint32_t revision, const char *css, size_t length)
{
    size_t rules_before = sheet->count;
    const unsigned char *ir = NULL, *fragment = NULL;
    size_t ir_bytes = 0, fragment_bytes = 0;
    bool cached = document_constructed_sheet_cache(
        document, node, revision, &ir, &ir_bytes, &fragment, &fragment_bytes);
    StyleParsedIrApplyResult replayed = STYLE_PARSED_IR_REJECTED;
    if (ir != NULL) {
        replayed = stylesheet_add_parsed_ir_from_context(
            sheet, ir, ir_bytes, NULL, NULL, NULL);
        if (replayed == STYLE_PARSED_IR_FAILED) return false;
    }
    unsigned char *new_ir = NULL;
    size_t new_ir_bytes = 0;
    /* A sheet parsed once (the common case) keeps no cache; one parsed
       again is likely to be parsed again, and keeps its form. */
    bool keep = cached || document_constructed_sheet_note_parse(
        document, node, revision);
    if (replayed != STYLE_PARSED_IR_APPLIED) {
        bool parsed = ir == NULL && keep
            ? stylesheet_capture_ir(sheet, css, length, NULL, NULL, true,
                                    &new_ir, &new_ir_bytes)
            : stylesheet_add_css_from_context(sheet, css, length, NULL, NULL);
        if (!parsed) {
            budget_free(sheet->budget, new_ir);
            return false;
        }
    }
    size_t rules_after = sheet->count;
    if (rules_after <= rules_before || !keep) {
        budget_free(sheet->budget, new_ir);
        return true;
    }
    if (fragment != NULL
        && stylesheet_compiled_fragment_apply(
               sheet, rules_before, rules_after, fragment, fragment_bytes)
           != 0) {
        budget_free(sheet->budget, new_ir);
        return true;
    }
    unsigned char *new_fragment = NULL;
    size_t new_fragment_bytes = 0;
    if (stylesheet_compiled_fragment_build(
            sheet, rules_before, rules_after, &new_fragment,
            &new_fragment_bytes))
        (void) stylesheet_compiled_fragment_apply(
            sheet, rules_before, rules_after, new_fragment,
            new_fragment_bytes);
    /* Keep what was rebuilt, beside the half that still matched. */
    (void) document_constructed_sheet_cache_store(
        document, node, revision,
        new_ir != NULL ? new_ir : (cached ? ir : NULL),
        new_ir != NULL ? new_ir_bytes : (cached ? ir_bytes : 0),
        new_fragment, new_fragment_bytes);
    budget_free(sheet->budget, new_ir);
    budget_free(sheet->budget, new_fragment);
    return true;
}

bool stylesheet_add_adopted_sheet(Stylesheet *sheet,
                                  const PocDocument *document,
                                  lxb_dom_node_t *node, uint32_t revision)
{
    if (sheet == NULL || node == NULL) return false;
    bool known = stylesheet_style_source_known(sheet, node);
    /* A constructed source cannot be parsed without an order boundary:
       untracked rules would have source bit zero and escape confinement.
       Decline the optional source before parsing, without invalidating
       the boundaries of adopted sources that already fit. */
    if (!known && (sheet->style_sources_bounded_out
                   || sheet->style_source_count == STYLE_SOURCE_NODE_LIMIT))
        return true;
    /* The parsed rules must stay contiguous for the selector fragment
       until it is applied: parse inside a rule batch (the caller's when
       one is open). */
    bool own_batch = !sheet->rule_batch_active
        && stylesheet_begin_rule_batch(sheet);
    bool parsed = stylesheet_parse_adopted_sheet(sheet, document, node,
                                                 revision);
    if (own_batch && !stylesheet_end_rule_batch(sheet)) parsed = false;
    if (!parsed) return false;
    if (!known && stylesheet_style_source_known(sheet, node)
        && sheet->adopted_source_count < UINT8_MAX) {
        sheet->adopted_source_count++;
        sheet->adopted_sources_signature = document_adopted_tier_hash(
            sheet->adopted_sources_signature, node, revision);
    }
    return true;
}

static int stylesheet_compare_scope_roots(const void *left,
                                          const void *right)
{
    uintptr_t a = (uintptr_t) ((const StyleAdoptedScopeRoot *) left)->root;
    uintptr_t b = (uintptr_t) ((const StyleAdoptedScopeRoot *) right)->root;
    return a < b ? -1 : a > b;
}

static uint64_t stylesheet_scope_mask_of(const StyleAdoptedScopeRoot *roots,
                                         size_t count,
                                         const lxb_dom_node_t *root)
{
    for (size_t i = 0; i < count; i++) {
        if (roots[i].root == root) return roots[i].mask;
    }
    return 0;
}

bool stylesheet_set_adopted_scopes(Stylesheet *sheet,
                                   const PocDocument *document,
                                   const lxb_dom_node_t **changed,
                                   size_t capacity, size_t *changed_count,
                                   bool *global)
{
    if (changed_count != NULL) *changed_count = 0;
    if (global != NULL) *global = false;
    if (sheet == NULL || sheet->budget == NULL) return false;
    size_t sources = sheet->adopted_source_count;
    if (sources > sheet->style_source_count || sheet->style_sources_bounded_out)
        sources = 0;
    size_t base = sheet->style_source_count - sources;
    StyleAdoptedScopeRoot roots[DOCUMENT_ADOPTION_ROOT_LIMIT];
    size_t root_count = 0;
    uint64_t document_mask = 0, shadow_mask = 0;
    uint8_t document_order[64] = {0};
    const lxb_dom_node_t *root = NULL;
    lxb_dom_node_t *const *list = NULL;
    size_t length = 0;
    bool active = false;
    for (size_t index = 0; sources != 0
         && document_adoption_list_at(document, index, &root, &list,
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
        if (root_count < DOCUMENT_ADOPTION_ROOT_LIMIT) {
            entry.mask = mask;
            roots[root_count++] = entry;
        }
    }
    uint64_t shadow_only = shadow_mask & ~document_mask;
    /* Keep order even for document-adopted sources shared by roots. */
    qsort(roots, root_count, sizeof(roots[0]),
          stylesheet_compare_scope_roots);
    /* Report what moved: every root whose confined sources differ. */
    size_t reported = 0;
    bool overflow = false;
    const StyleAdoptedScopeRoot *old = sheet->adopted_scope_roots;
    size_t old_count = sheet->adopted_scope_root_count;
    for (size_t pass = 0; pass < 2; pass++) {
        const StyleAdoptedScopeRoot *from = pass == 0 ? roots : old;
        size_t from_count = pass == 0 ? root_count : old_count;
        const StyleAdoptedScopeRoot *against = pass == 0 ? old : roots;
        size_t against_count = pass == 0 ? old_count : root_count;
        for (size_t i = 0; i < from_count; i++) {
            uint64_t other = stylesheet_scope_mask_of(
                against, against_count, from[i].root);
            bool same_order = false;
            for (size_t k = 0; k < against_count; k++) {
                if (against[k].root == from[i].root) {
                    same_order = memcmp(against[k].order, from[i].order,
                                        sizeof(from[i].order)) == 0;
                    break;
                }
            }
            if (other == from[i].mask && same_order) continue;
            /* A root listed with a different mask is reported once. A root
               only the old table lists is reported while it lives: the
               registry forgets a carrier before it is destroyed. */
            if (pass == 1 && (other != 0
                              || (from[i].root != NULL
                                  && !document_adoption_root_known(
                                         document, from[i].root)))) continue;
            if (from[i].root == NULL) overflow = true;
            if (changed != NULL && reported < capacity)
                changed[reported] = from[i].root;
            else overflow = true;
            reported++;
        }
    }
    if (changed_count != NULL)
        *changed_count = reported < capacity ? reported : capacity;
    if (global != NULL)
        *global = overflow || shadow_only != sheet->adopted_shadow_only_mask
            || document_mask != sheet->adopted_document_mask;
    sheet->adopted_document_mask = document_mask;
    memcpy(sheet->adopted_document_order, document_order,
           sizeof(document_order));
    StyleAdoptedScopeRoot *table = NULL;
    if (root_count != sheet->adopted_scope_root_count
        || sheet->adopted_scope_roots == NULL) {
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
            /* Keep the build and committed root table. Decline confined
               sources, but document scope/order needs no allocation. */
            sheet->adopted_scopes_failed = true;
            for (size_t k = 0; k < sources && k < 64u; k++) {
                if (document_order[k] != 0 && document_order[k] != k + 1u)
                    sheet->adopted_order_varies = true;
            }
            stylesheet_drop_rule_index(sheet);
            stylesheet_drop_custom_rule_index(sheet);
            if (global != NULL) *global = true;
            return false;
        }
        budget_free(sheet->budget, sheet->adopted_scope_roots);
        sheet->adopted_scope_roots = table;
    }
    if (root_count != 0)
        memcpy(sheet->adopted_scope_roots, roots,
               root_count * sizeof(roots[0]));
    sheet->adopted_scope_root_count = (uint8_t) root_count;
    /* The rule indexes set shadow-scoped rules apart by this mask. */
    if (shadow_only != sheet->adopted_shadow_only_mask
        || sheet->adopted_scopes_failed) {
        stylesheet_drop_rule_index(sheet);
        stylesheet_drop_custom_rule_index(sheet);
    }
    sheet->adopted_shadow_only_mask = shadow_only;
    sheet->adopted_scopes_failed = false;
    sheet->adopted_order_varies = false;
    for (size_t i = 0; i < root_count; i++) {
        for (size_t k = 0; k < sources && k < 64u; k++) {
            if (roots[i].order[k] != 0 && roots[i].order[k] != k + 1u)
                sheet->adopted_order_varies = true;
        }
    }
    return true;
}

bool stylesheet_add_adopted_sheets(Stylesheet *sheet,
                                   const PocDocument *document)
{
    lxb_dom_node_t *nodes[DOCUMENT_ADOPTED_TIER_LIMIT];
    uint32_t revisions[DOCUMENT_ADOPTED_TIER_LIMIT];
    size_t count = document_adopted_sheets_active(
        document, nodes, revisions, DOCUMENT_ADOPTED_TIER_LIMIT);
    /* Past the bound the tier is applied up to it. */
    if (count == SIZE_MAX) count = DOCUMENT_ADOPTED_TIER_LIMIT;
    for (size_t i = 0; i < count; i++) {
        if (!stylesheet_add_adopted_sheet(sheet, document, nodes[i],
                                          revisions[i]))
            return false;
    }
    /* Scope metadata is optional: a refusal suppresses confined sources
       rather than retiring the page's entire stylesheet. */
    (void) stylesheet_set_adopted_scopes(sheet, document, NULL, 0, NULL, NULL);
    return true;
}

static uint64_t stylesheet_signature_bytes(
    uint64_t hash, const void *bytes, size_t length)
{
    const unsigned char *source = bytes;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ source[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t stylesheet_signature_container_queries(
    uint64_t hash, const Stylesheet *sheet)
{
    uint8_t query_count = sheet->conditional_queries == NULL ? 0
        : sheet->conditional_queries->query_count;
    uint8_t name_count = sheet->conditional_queries == NULL ? 0
        : sheet->conditional_queries->name_count;
    hash = stylesheet_signature_bytes(
        hash, &query_count, sizeof(query_count));
    hash = stylesheet_signature_bytes(
        hash, &name_count, sizeof(name_count));
    if (sheet->conditional_queries == NULL) return hash;
    hash = stylesheet_signature_bytes(
        hash, sheet->conditional_queries->queries,
        query_count * sizeof(sheet->conditional_queries->queries[0]));
    hash = stylesheet_signature_bytes(
        hash, sheet->conditional_queries->names,
        name_count * sizeof(sheet->conditional_queries->names[0]));
    return hash;
}

uint64_t stylesheet_parse_context_signature(const Stylesheet *sheet)
{
    if (sheet == NULL) return 0;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = stylesheet_signature_bytes(
        hash, &sheet->variable_count, sizeof(sheet->variable_count));
    for (size_t i = 0; i < sheet->variable_count; i++) {
        const StyleVariable *variable = &sheet->variables[i];
        hash = stylesheet_signature_bytes(
            hash, variable->name, variable->name_length + 1u);
        hash = stylesheet_signature_bytes(
            hash, variable->value, variable->value_length + 1u);
    }
    /* Registered initial values answer var() lookups without an element. */
    hash = stylesheet_signature_bytes(
        hash, &sheet->registered_property_count,
        sizeof(sheet->registered_property_count));
    for (size_t i = 0; i < sheet->registered_property_count; i++) {
        const StyleRegisteredProperty *property =
            &sheet->registered_properties[i];
        hash = stylesheet_signature_bytes(
            hash, property->name, property->name_length + 1u);
        hash = stylesheet_signature_bytes(
            hash, &property->inherits, sizeof(property->inherits));
        if (property->initial_value != NULL) {
            hash = stylesheet_signature_bytes(
                hash, property->initial_value,
                property->initial_length + 1u);
        }
    }
    hash = stylesheet_signature_bytes(
        hash, &sheet->layer_count, sizeof(sheet->layer_count));
    hash = stylesheet_signature_bytes(
        hash, sheet->layer_names,
        sheet->layer_count * sizeof(sheet->layer_names[0]));
    hash = stylesheet_signature_bytes(
        hash, sheet->layer_parents,
        sheet->layer_count * sizeof(sheet->layer_parents[0]));
    /* These intern pools have hard structural caps.  A suffix which consumes
       a slot before an already-compiled external sheet would change which
       later declarations survive a clean document-order rebuild even when
       the suffix adds no global variables. */
    hash = stylesheet_signature_bytes(
        hash, &sheet->image_url_count, sizeof(sheet->image_url_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->generated_text_count,
        sizeof(sheet->generated_text_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->counter_operation_set_count,
        sizeof(sheet->counter_operation_set_count));
    uint8_t grid_area_name_count =
        sheet->grid_areas == NULL ? 0 : sheet->grid_areas->name_count;
    uint8_t grid_area_template_count =
        sheet->grid_areas == NULL ? 0 : sheet->grid_areas->template_count;
    uint8_t grid_track_template_count =
        sheet->grid_tracks == NULL ? 0 : (uint8_t) sheet->grid_tracks->count;
    uint8_t grid_line_name_count =
        sheet->grid_areas == NULL ? 0 : sheet->grid_areas->line_name_count;
    hash = stylesheet_signature_bytes(
        hash, &grid_area_name_count, sizeof(grid_area_name_count));
    hash = stylesheet_signature_bytes(
        hash, &grid_area_template_count, sizeof(grid_area_template_count));
    hash = stylesheet_signature_bytes(
        hash, &grid_track_template_count,
        sizeof(grid_track_template_count));
    hash = stylesheet_signature_bytes(
        hash, &grid_line_name_count, sizeof(grid_line_name_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->border_color_set_count,
        sizeof(sheet->border_color_set_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->math_program_count, sizeof(sheet->math_program_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->math_instruction_count,
        sizeof(sheet->math_instruction_count));
    hash = stylesheet_signature_bytes(
        hash, &sheet->custom_rule_count, sizeof(sheet->custom_rule_count));
    hash = stylesheet_signature_container_queries(hash, sheet);
    size_t image_source_count =
        sheet->image_sources == NULL ? 0 : sheet->image_sources->count;
    hash = stylesheet_signature_bytes(
        hash, &image_source_count, sizeof(image_source_count));
    size_t family_count =
        sheet->web_fonts == NULL ? 0 : sheet->web_fonts->family_count;
    hash = stylesheet_signature_bytes(
        hash, &family_count, sizeof(family_count));
    if (sheet->web_fonts != NULL) {
        for (size_t i = 0; i < family_count; i++) {
            const StyleWebFontFamily *family =
                &sheet->web_fonts->families[i];
            hash = stylesheet_signature_bytes(
                hash, family->name, strlen(family->name) + 1u);
            hash = stylesheet_signature_bytes(
                hash, &family->regular.source_count,
                sizeof(family->regular.source_count));
            hash = stylesheet_signature_bytes(
                hash, &family->bold.source_count,
                sizeof(family->bold.source_count));
        }
    }
    return hash;
}

bool stylesheet_add_user_css(Stylesheet *sheet, const char *css, size_t length)
{
    if (sheet == NULL || sheet->budget == NULL
        || (css == NULL && length != 0)) return false;
    if (length == 0) return true;
    sheet->build_generation = stylesheet_next_generation();
    StyleCssTextInput input = {css, length};
    return stylesheet_run_parse_transaction(sheet, NULL, NULL, 1, false,
        NULL, stylesheet_parse_text_body, &input);
}

/* `name` as a declared property in deferred declaration text: at the
   start or after `;`, `{` or whitespace, followed by `:`. A substring test
   took every `flex-direction` for `direction`. */
static bool style_span_declares_property_ci(const char *text, size_t length,
                                            const char *name)
{
    size_t name_length = strlen(name);
    for (size_t at = 0; at + name_length <= length; at++) {
        if (strncasecmp(text + at, name, name_length) != 0) continue;
        char before = at == 0 ? ';' : text[at - 1];
        if (before != ';' && before != '{' && !isspace((unsigned char) before))
            continue;
        size_t after = at + name_length;
        while (after < length && isspace((unsigned char) text[after])) after++;
        if (after < length && text[after] == ':') return true;
    }
    return false;
}

/* The selector requires [dir=rtl] on some element outside any functional
   pseudo-class (where :not() could negate it): it cannot match a document
   without dir="rtl" markup. */
static bool style_selector_requires_dir_rtl(const char *selector)
{
    int depth = 0;
    for (const char *at = selector; at != NULL && *at != '\0'; at++) {
        if (*at == '\\' && at[1] != '\0') {
            at++;
            continue;
        }
        if (*at == '(') depth++;
        else if (*at == ')' && depth > 0) depth--;
        if (*at != '[' || depth != 0) continue;
        const char *cursor = at + 1;
        while (isspace((unsigned char) *cursor)) cursor++;
        if (strncasecmp(cursor, "dir", 3) != 0) continue;
        cursor += 3;
        while (isspace((unsigned char) *cursor)) cursor++;
        if (*cursor != '=') continue;
        cursor++;
        while (isspace((unsigned char) *cursor)) cursor++;
        char quote = *cursor == '"' || *cursor == '\'' ? *cursor : 0;
        if (quote != 0) cursor++;
        if (strncasecmp(cursor, "rtl", 3) != 0) continue;
        cursor += 3;
        if (quote != 0) {
            if (*cursor != quote) continue;
            cursor++;
        }
        while (isspace((unsigned char) *cursor)) cursor++;
        if (*cursor == 'i' || *cursor == 'I' || *cursor == 's'
            || *cursor == 'S') {
            cursor++;
            while (isspace((unsigned char) *cursor)) cursor++;
        }
        if (*cursor == ']') return true;
    }
    return false;
}

bool stylesheet_direction_change_rules(const Stylesheet *sheet,
                                       bool rtl_markup_absent,
                                       size_t *indices, size_t capacity,
                                       size_t *count)
{
    if (count != NULL) *count = 0;
    if (sheet == NULL || count == NULL || (indices == NULL && capacity != 0))
        return false;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleDeclaration *declaration =
            stylesheet_rule_declaration(sheet, &sheet->rules[i]);
        if (declaration == NULL) continue;
        ComputedStyle values = {0};
        if ((declaration->mask_high & (S2_DIRECTION | S2_UNICODE_BIDI))
            != 0) {
            stylesheet_declaration_values(sheet, declaration, &values);
        }
        bool changes =
            ((declaration->mask_high & S2_DIRECTION) != 0
             && computed_style_direction_rtl(&values))
            || ((declaration->mask_high & S2_UNICODE_BIDI) != 0
                && (values.unicode_bidi == STYLE_UNICODE_BIDI_OVERRIDE
                    || values.unicode_bidi
                        == STYLE_UNICODE_BIDI_ISOLATE_OVERRIDE))
            || (declaration->deferred_declarations != NULL
                && (style_span_declares_property_ci(
                        declaration->deferred_declarations,
                        declaration->deferred_length, "direction")
                    || style_span_declares_property_ci(
                        declaration->deferred_declarations,
                        declaration->deferred_length, "unicode-bidi")));
        if (!changes) continue;
        if (rtl_markup_absent
            && style_selector_requires_dir_rtl(sheet->rules[i].selector))
            continue;
        if (*count == capacity) return false;
        indices[(*count)++] = i;
    }
    return true;
}

bool stylesheet_rule_index_matches(const Stylesheet *sheet, size_t index,
                                   lxb_dom_node_t *node)
{
    return style_rule_selector_matches(sheet, index, node);
}

size_t stylesheet_layout_island_selectors(const Stylesheet *sheet,
                                          const char **selectors,
                                          size_t capacity)
{
    if (sheet == NULL || (selectors == NULL && capacity != 0)) return 0;
    size_t count = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        const StyleDeclaration *declaration = stylesheet_rule_declaration(
            sheet, rule);
        if (declaration == NULL || (declaration->mask & S_DISPLAY) == 0
            || rule->pseudo != PSEUDO_NONE) continue;
        ComputedStyle values;
        stylesheet_declaration_values(sheet, declaration, &values);
        DisplayMode display = values.display;
        bool island = display == DISPLAY_FLEX
            || display == DISPLAY_INLINE_FLEX
            || display == DISPLAY_GRID
            || display == DISPLAY_INLINE_GRID
            || display == DISPLAY_TABLE;
        if ((declaration->mask & S_DISPLAY) == 0 || !island
            || rule->pseudo != PSEUDO_NONE || rule->selector[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t at = 0; at < count && at < capacity; at++) {
            if (strcmp(selectors[at], rule->selector) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        if (count < capacity) selectors[count] = rule->selector;
        count++;
    }
    return count < capacity ? count : capacity;
}

size_t stylesheet_web_font_source_count(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->web_fonts == NULL) return 0;
    size_t count = 0;
    for (size_t i = 0; i < sheet->web_fonts->family_count; i++) {
        count += sheet->web_fonts->families[i].regular.source_count;
        count += sheet->web_fonts->families[i].bold.source_count;
    }
    return count;
}

bool stylesheet_web_font_source(const Stylesheet *sheet, size_t index,
                                StylesheetWebFontSource *source)
{
    if (sheet == NULL || sheet->web_fonts == NULL || source == NULL) {
        return false;
    }
    for (size_t slot = 0; slot < sheet->web_fonts->family_count; slot++) {
        const StyleWebFontFamily *family =
            &sheet->web_fonts->families[slot];
        const StyleWebFontFaceSource *faces[2] = {
            &family->regular, &family->bold
        };
        for (size_t bold = 0; bold < 2; bold++) {
            for (size_t candidate = 0;
                 candidate < faces[bold]->source_count; candidate++) {
                if (index != 0) {
                    index--;
                    continue;
                }
                *source = (StylesheetWebFontSource) {
                    .reference = faces[bold]->references[candidate],
                    .source_base_url = faces[bold]->source_bases[candidate],
                    .source_referrer_policy =
                        faces[bold]->source_bases[candidate] == NULL
                        ? NULL
                        : faces[bold]
                              ->source_referrer_policies[candidate],
                    .family_slot = (unsigned) slot,
                    .bold = bold != 0
                };
                return true;
            }
        }
    }
    return false;
}

FontFace *stylesheet_web_font_face(Stylesheet *sheet, unsigned family_slot,
                                   bool bold)
{
    if (sheet == NULL || sheet->web_fonts == NULL
        || family_slot >= sheet->web_fonts->family_count
        || family_slot >= TILEFINCH_WEB_FONT_FAMILY_LIMIT) return NULL;
    WebFontFamilyFaces *faces =
        &sheet->web_fonts->loaded.families[family_slot];
    return bold ? &faces->bold : &faces->regular;
}

const WebFontSet *stylesheet_web_font_set(const Stylesheet *sheet)
{
    return sheet == NULL || sheet->web_fonts == NULL
           ? NULL : &sheet->web_fonts->loaded;
}

bool stylesheet_web_font_stats(const Stylesheet *sheet,
                               StylesheetWebFontStats *stats)
{
    if (sheet == NULL || stats == NULL) return false;
    *stats = sheet->web_fonts == NULL
             ? (StylesheetWebFontStats) {0}
             : sheet->web_fonts->stats;
    return true;
}

void stylesheet_destroy(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    style_selector_cooperation_end(sheet);
    style_variable_cache_end(sheet);
    style_container_layout_state_clear(sheet);
    style_container_log_release(sheet);
    if (sheet->budget != NULL) {
        for (size_t i = 0; i < sheet->declaration_count; i++) {
            budget_free(sheet->budget,
                        sheet->declarations[i].deferred_declarations);
        }
        budget_free(sheet->budget, sheet->adopted_scope_roots);
        sheet->adopted_scope_roots = NULL;
        budget_free(sheet->budget, sheet->shadow_scopes);
        sheet->shadow_scopes = NULL;
        sheet->shadow_scope_count = 0;
        sheet->shadow_scope_capacity = 0;
        budget_free(sheet->budget, sheet->rules);
        budget_free(sheet->budget, sheet->motion_css);
        budget_free(sheet->budget, sheet->focus_rule_indices);
        budget_free(sheet->budget, sheet->has_rule_indices);
        budget_free(sheet->budget, sheet->has_custom_rule_indices);
        budget_free(sheet->budget, sheet->has_class_hashes);
        style_has_plan_destroy(sheet);
        budget_free(sheet->budget, sheet->declarations);
        budget_free(sheet->budget, sheet->declaration_values);
        budget_free(sheet->budget, sheet->revert_rule_masks);
        budget_free(sheet->budget, sheet->declaration_index_slots);
        StyleTextChunk *chunk = sheet->selector_chunks;
        while (chunk != NULL) {
            StyleTextChunk *next = chunk->next;
            budget_free(sheet->budget, chunk);
            chunk = next;
        }
        budget_free(sheet->budget, sheet->variables);
        budget_free(sheet->budget, sheet->custom_rules);
        budget_free(sheet->budget, sheet->registered_properties);
        budget_free(sheet->budget, sheet->transition_rules);
        budget_free(sheet->budget, sheet->conditional_queries);
        if (sheet->paint_storage != NULL) {
            for (size_t block = 0;
                 block < sheet->paint_storage->capacity; block++) {
                budget_free(
                    sheet->budget, sheet->paint_storage->blocks[block]);
            }
            budget_free(sheet->budget, sheet->paint_storage->blocks);
            budget_free(sheet->budget, sheet->paint_storage);
        }
        if (sheet->grid_tracks != NULL) {
            for (size_t block = 0;
                 block < sheet->grid_tracks->capacity; block++) {
                budget_free(
                    sheet->budget, sheet->grid_tracks->blocks[block]);
            }
            budget_free(sheet->budget, sheet->grid_tracks->blocks);
            budget_free(sheet->budget, sheet->grid_tracks);
        }
        budget_free(sheet->budget, sheet->grid_areas);
        budget_free(sheet->budget, sheet->border_color_sets);
        budget_free(sheet->budget, sheet->custom_rule_index);
        budget_free(sheet->budget, sheet->svg_raster_tokens);
        budget_free(sheet->budget, sheet->selector_attribute_names);
        budget_free(sheet->budget, sheet->deferred_instructions);
        budget_free(sheet->budget, sheet->class_tokens);
        sheet->class_tokens = NULL;
        sheet->class_tokens_refused = false;
        budget_free(sheet->budget, sheet->resolve_scratch);
        budget_free(sheet->budget, sheet->rule_index_buckets);
        budget_free(sheet->budget, sheet->rule_index_slots);
        budget_free(sheet->budget, sheet->rule_index_entries);
        budget_free(sheet->budget, sheet->rule_filters);
        budget_free(sheet->budget, sheet->rule_relational_tokens);
        budget_free(sheet->budget, sheet->rule_ancestor_tokens);
        budget_free(sheet->budget, sheet->selector_program);
        budget_free(sheet->budget, sheet->selector_program_offsets);
        budget_free(sheet->budget, sheet->selector_fragment_program);
        budget_free(sheet->budget, sheet->selector_fragment_offsets);
        budget_free(sheet->budget, sheet->selector_fragment_counts);
        budget_free(sheet->budget, sheet->selector_append_offsets);
        budget_free(sheet->budget, sheet->math_instructions);
        budget_free(sheet->budget, sheet->math_programs);
        for (size_t i = 0; i < sheet->generated_text_count; i++) {
            budget_free(sheet->budget, sheet->generated_texts[i]);
        }
        budget_free(sheet->budget, sheet->generated_texts);
        budget_free(sheet->budget, sheet->counter_operation_sets);
        for (size_t i = 0; i < sheet->image_url_count; i++) {
            StyleImageReference *reference =
                (StyleImageReference *)
                    ((unsigned char *) sheet->image_urls[i]
                     - offsetof(StyleImageReference, reference));
            budget_free(sheet->budget, reference);
        }
        budget_free(sheet->budget, sheet->image_urls);
        if (sheet->image_sources != NULL) {
            for (size_t i = 0; i < sheet->image_sources->count; i++) {
                budget_free(sheet->budget,
                            sheet->image_sources->items[i].base_url);
            }
            budget_free(sheet->budget, sheet->image_sources->items);
            budget_free(sheet->budget, sheet->image_sources);
        }
        if (sheet->web_fonts != NULL) {
            web_font_set_destroy(&sheet->web_fonts->loaded);
            for (size_t slot = 0;
                 slot < sheet->web_fonts->family_count; slot++) {
                StyleWebFontFaceSource *faces[2] = {
                    &sheet->web_fonts->families[slot].regular,
                    &sheet->web_fonts->families[slot].bold
                };
                for (size_t bold = 0; bold < 2; bold++) {
                    for (size_t source = 0;
                         source < faces[bold]->source_count; source++) {
                        budget_free(sheet->budget,
                                    faces[bold]->source_bases[source]);
                        budget_free(sheet->budget,
                                    faces[bold]->references[source]);
                    }
                }
            }
            budget_free(sheet->budget, sheet->web_fonts);
        }
    }
    memset(sheet, 0, sizeof(*sheet));
}

const StyleGradient *stylesheet_background_overlay_gradient(
    const Stylesheet *sheet, uint16_t one_based_id)
{
    const StylePaintStack *stack = stylesheet_paint_stack(
        sheet, (uint8_t) one_based_id);
    if (stack == NULL || stack->background_count == 0
        || stack->backgrounds[0].kind != STYLE_PAINT_IMAGE_GRADIENT
        || stack->backgrounds[0].gradient.stop_count < 2) {
        return NULL;
    }
    return &stack->backgrounds[0].gradient;
}

const StyleGradient *stylesheet_background_gradient(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    const StylePaintStack *stack = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(style));
    if (stack == NULL
        || (stack->components & STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE) == 0) {
        return NULL;
    }
    for (size_t i = stack->background_count; i-- > 0;) {
        if (stack->backgrounds[i].kind == STYLE_PAINT_IMAGE_GRADIENT
            && stack->backgrounds[i].gradient.stop_count >= 2) {
            return &stack->backgrounds[i].gradient;
        }
    }
    return NULL;
}

size_t stylesheet_box_shadow_count(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    const StylePaintStack *stack = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(style));
    return stack != NULL
        && (stack->components & STYLE_PAINT_COMPONENT_BOX_SHADOW) != 0
        ? stack->box_shadow_count : 0;
}

const StyleBoxShadow *stylesheet_box_shadow(
    const Stylesheet *sheet, const ComputedStyle *style, size_t index)
{
    const StylePaintStack *stack = stylesheet_paint_stack(
        sheet, computed_style_paint_stack_id(style));
    if (stack == NULL
        || (stack->components & STYLE_PAINT_COMPONENT_BOX_SHADOW) == 0
        || index >= stack->box_shadow_count
        || index >= STYLE_BOX_SHADOW_LIMIT) return NULL;
    return &stack->box_shadows[index];
}

const StylePaintStack *stylesheet_paint_stack(
    const Stylesheet *sheet, uint8_t one_based_id)
{
    if (sheet == NULL || sheet->paint_storage == NULL
        || one_based_id == 0
        || one_based_id > sheet->paint_storage->count) {
        return NULL;
    }
    return style_paint_storage_const_slot(
        sheet->paint_storage, one_based_id - 1u);
}

int stylesheet_border_radius_code(
    const Stylesheet *sheet, const ComputedStyle *style)
{
    (void) sheet;
    if (style == NULL) return 0;
    return style->border_radius;
}
