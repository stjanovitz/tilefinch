/* Stylesheet construction and storage: selector interning, custom
   properties, declaration/rule storage, @media/@supports/@layer,
   @font-face and web-font collection, rule ordering and the rule
   index, and the public stylesheet_* build/query/destroy API.
   Split out of style.c. */

#include "style_internal.h"
#include "style_cache_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <lexbor/tag/tag.h>

#include "tilefinch/platform.h"

#define STYLE_VARIABLE_CACHE_DEFAULT_ENTRIES 256u
#define STYLE_VARIABLE_CACHE_MIN_ENTRIES 32u
#define STYLE_VARIABLE_CACHE_MAX_ENTRIES 256u
#define STYLE_VARIABLE_CACHE_PROBES 8u

#define STYLE_PARSED_IR_MAGIC UINT32_C(0x54464952)
#define STYLE_PARSED_IR_VERSION UINT16_C(1)
#define STYLE_PARSED_IR_MAX_BYTES (256u * 1024u)
#define STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES UINT16_C(1)

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
    uint16_t reserved;
} StyleParsedIrOperation;

typedef struct {
    Budget *budget;
    unsigned char *data;
    size_t length;
    size_t capacity;
    size_t maximum_length;
    size_t operation_count;
    bool eligible;
    bool has_motion_keyframes;
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

static bool style_parsed_ir_builder_record(
    StyleCssParseContext *context, const char *selectors,
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
        .selector_length = (uint16_t) selector_length
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
    budget_free(sheet->budget, sheet->rule_index_entries);
    budget_free(sheet->budget, sheet->rule_filters);
    sheet->rule_index_buckets = NULL;
    sheet->rule_index_entries = NULL;
    sheet->rule_filters = NULL;
    sheet->rule_index_bucket_count = 0;
    sheet->rule_index_universal_count = 0;
    sheet->rule_index_bytes = 0;
    sheet->rule_index_ready = false;
    sheet->rule_index_attempted = false;
    sheet->rule_ancestor_filter_active = false;
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
    if (rule->selector == NULL
        || rule->rightmost_compound_offset >= rule->selector_length) {
        return style_token_bloom_empty();
    }
    return style_selector_compound_bloom(
        rule->selector + rule->rightmost_compound_offset,
        rule->selector_length - rule->rightmost_compound_offset);
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

static void relational_tokens_add(StyleRuleFilter *filter, uint32_t hash)
{
    for (size_t i = 0; i < filter->relational_count; i++) {
        if (filter->relational_tokens[i] == hash) return;
    }
    if (filter->relational_count == STYLE_RELATIONAL_TOKEN_LIMIT) {
        filter->relational_opaque = true;
        return;
    }
    filter->relational_tokens[filter->relational_count++] = hash;
}

static void style_rule_relational_filter(const StyleRule *rule,
                                         StyleRuleFilter *filter)
{
    filter->relational_count = 0;
    filter->relational_opaque = true;
    if (rule == NULL || rule->selector == NULL) return;
    const char *text = rule->selector;
    size_t length = rule->selector_length;
    size_t split = rule->rightmost_compound_offset;
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
                relational_tokens_add(filter, stylesheet_identity_token_hash(
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
                affected = filter->relational_tokens[r] == hashes[h];
            }
        }
        if (!affected) continue;
        if (count == capacity || i > UINT32_MAX) return SIZE_MAX;
        out[count++] = (uint32_t) i;
    }
    return count;
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

static void stylesheet_prepare_rule_filters(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL || sheet->count == 0
        || sheet->count > SIZE_MAX / sizeof(StyleRuleFilter)) return;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_STYLE_RULE_FILTER") != NULL) return;
#endif
    StyleRuleFilter *filters = budget_malloc(
        sheet->budget, sheet->count * sizeof(*filters));
    if (filters == NULL) return;
    bool useful = false;
    bool ancestors_useful = false;
    for (size_t i = 0; i < sheet->count; i++) {
        filters[i] = (StyleRuleFilter) {
            .compound = style_rule_compound_bloom(sheet, i),
            .ancestors = style_rule_ancestor_bloom(sheet, i)
        };
        style_rule_relational_filter(&sheet->rules[i], &filters[i]);
        const StyleDeclaration *declaration = stylesheet_rule_declaration(
            sheet, &sheet->rules[i]);
        const uint64_t discovery_mask = S_DISPLAY | S_VISIBILITY
            | S_BACKGROUND_IMAGE | S_MASK_IMAGE | S_CONTENT;
        uint8_t stack_id = declaration == NULL
            ? 0 : computed_style_paint_stack_id(&declaration->values);
        const StylePaintStack *stack = stack_id == 0
            ? NULL : stylesheet_paint_stack(sheet, stack_id);
        bool stack_images = stack_id != 0
            && (stack == NULL
                || (stack->components
                    & (STYLE_PAINT_COMPONENT_BACKGROUND_IMAGE
                       | STYLE_PAINT_COMPONENT_MASK_IMAGE)) != 0);
        filters[i].discovery = declaration == NULL
            || (declaration->mask & discovery_mask) != 0
            || (declaration->inherit_mask & discovery_mask) != 0
            || declaration->deferred_declarations != NULL
            || declaration->deferred_program_count != 0
            || stack_images;
        useful = useful || !style_token_bloom_empty_value(filters[i].compound)
            || !style_token_bloom_empty_value(filters[i].ancestors);
        ancestors_useful = ancestors_useful
            || !style_token_bloom_empty_value(filters[i].ancestors);
    }
    if (!useful) {
        budget_free(sheet->budget, filters);
        return;
    }
    sheet->rule_filters = filters;
    sheet->rule_ancestor_filter_active = ancestors_useful;
    sheet->rule_index_bytes += sheet->count * sizeof(*filters);
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
        || sheet->custom_rule_count > UINT32_MAX
        || sheet->custom_rule_count
             > SIZE_MAX / sizeof(StyleCustomRuleIndexEntry)) return;
    StyleCustomRuleIndexEntry *entries = budget_malloc(
        sheet->budget, sheet->custom_rule_count * sizeof(*entries));
    if (entries == NULL) return;
    for (size_t i = 0; i < sheet->custom_rule_count; i++) {
        entries[i] = (StyleCustomRuleIndexEntry) {
            .hash = custom_rule_name_hash(sheet->custom_rules[i].name,
                                          strlen(sheet->custom_rules[i].name)),
            .index = (uint32_t) i
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

static bool style_rule_bucket_key_matches(
    const Stylesheet *sheet, const StyleRuleIndexBucket *bucket,
    SelectorType type, const char *text, size_t length, uint32_t hash,
    PseudoElement pseudo)
{
    if (bucket->representative == STYLE_RULE_INDEX_EMPTY
        || bucket->hash != hash
        || bucket->representative >= sheet->count) return false;
    const StyleRule *rule = &sheet->rules[bucket->representative];
    const char *fast_key = style_rule_fast_key(rule);
    return rule->has_fast_key && rule->type == type && rule->pseudo == pseudo
        && fast_key != NULL && rule->fast_key_length == length
        && memcmp(fast_key, text, length) == 0;
}

StyleRuleIndexBucket *style_rule_find_bucket(
    const Stylesheet *sheet, SelectorType type, const char *text,
    size_t length, bool create, PseudoElement pseudo)
{
    if (sheet == NULL || sheet->rule_index_buckets == NULL
        || sheet->rule_index_bucket_count == 0 || text == NULL) return NULL;
    uint32_t hash = style_rule_key_hash(type, text, length)
        ^ ((uint32_t) pseudo * UINT32_C(0x9e3779b9));
    size_t mask = sheet->rule_index_bucket_count - 1;
    size_t slot = (size_t) hash & mask;
    for (size_t probes = 0; probes < sheet->rule_index_bucket_count;
         probes++, slot = (slot + 1) & mask) {
        StyleRuleIndexBucket *bucket = &sheet->rule_index_buckets[slot];
        if (bucket->representative == STYLE_RULE_INDEX_EMPTY) {
            if (!create) return NULL;
            bucket->hash = hash;
            return bucket;
        }
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
    STYLE_SHRINK(variables, variable_count, variable_capacity);
    STYLE_SHRINK(custom_rules, custom_rule_count, custom_rule_capacity);
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
    if (bucket_count > SIZE_MAX / sizeof(StyleRuleIndexBucket)) return;
    StyleRuleIndexBucket *buckets = budget_malloc(
        sheet->budget, bucket_count * sizeof(*buckets));
    uint32_t *entries = budget_malloc(
        sheet->budget, sheet->count * sizeof(*entries));
    if (buckets == NULL || entries == NULL) {
        budget_free(sheet->budget, buckets);
        budget_free(sheet->budget, entries);
        return;
    }
    for (size_t i = 0; i < bucket_count; i++) {
        buckets[i] = (StyleRuleIndexBucket) {
            .representative = STYLE_RULE_INDEX_EMPTY
        };
    }
    sheet->rule_index_buckets = buckets;
    sheet->rule_index_entries = entries;
    sheet->rule_index_bucket_count = bucket_count;

    size_t universal_count = 0;
    uint32_t universal_counts[3] = {0};
    _Static_assert(PSEUDO_NONE == 0 && PSEUDO_BEFORE == 1 && PSEUDO_AFTER == 2,
                   "rule-index partitions must cover every pseudo element");
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (rule->pseudo > PSEUDO_AFTER) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        if (!rule->has_fast_key) {
            universal_count++;
            universal_counts[rule->pseudo]++;
            continue;
        }
        const char *fast_key = style_rule_fast_key(rule);
        if (fast_key == NULL) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        size_t length = rule->fast_key_length;
        StyleRuleIndexBucket *bucket = style_rule_find_bucket(
            sheet, (SelectorType) rule->type, fast_key, length, true,
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
    }
    size_t next = universal_count;
    for (size_t i = 0; i < bucket_count; i++) {
        StyleRuleIndexBucket *bucket = &buckets[i];
        if (bucket->representative == STYLE_RULE_INDEX_EMPTY) continue;
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
    for (size_t i = 0; i < 3; i++) {
        sheet->rule_index_universal_ends[i] = universal_at[i] + universal_counts[i];
    }
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (!rule->has_fast_key) {
            entries[universal_at[rule->pseudo]++] = (uint32_t) i;
            continue;
        }
        const char *fast_key = style_rule_fast_key(rule);
        if (fast_key == NULL) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        StyleRuleIndexBucket *bucket = style_rule_find_bucket(
            sheet, (SelectorType) rule->type, fast_key,
            rule->fast_key_length, false, (PseudoElement) rule->pseudo);
        if (bucket == NULL || bucket->fill >= bucket->count) {
            stylesheet_drop_rule_index(sheet);
            sheet->rule_index_attempted = true;
            return;
        }
        entries[bucket->first + bucket->fill++] = (uint32_t) i;
    }
    sheet->rule_index_universal_count = universal_count;
    sheet->rule_index_bytes = bucket_count * sizeof(*buckets)
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
}

static void stylesheet_finalize_rule_order(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    stylesheet_refresh_layer_keys(sheet);
    qsort(sheet->rules, sheet->count, sizeof(*sheet->rules), compare_rules);
    sheet->focus_rule_index_ready = false;
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
    stylesheet_destroy(sheet);
    bool initialized = stylesheet_initialize(
        sheet, budget, viewport_width, viewport_height);
    if (initialized) {
        sheet->document_rules_deferred = true;
        sheet->block_inline_style_attributes =
            block_inline_style_attributes;
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
        if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    }
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, parsed);
    return parsed;
}

typedef struct { const char *css; size_t length; } StyleCssTextInput;

static bool stylesheet_parse_text_body(StyleCssParseContext *parse,
                                       const void *opaque)
{
    const StyleCssTextInput *input = opaque;
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
    size_t at = sizeof(StyleParsedIrHeader);
    bool parsed = true;
    for (size_t i = 0; parsed && i < input->operation_count; i++) {
        StyleParsedIrOperation operation;
        memcpy(&operation, input->data + at, sizeof(operation));
        at += sizeof(operation);
        const char *selectors = (const char *) input->data + at;
        at += operation.selector_length;
        const char *declarations = (const char *) input->data + at;
        at += operation.declaration_length;
        parsed = selector_list_to_rules(
            parse, selectors, operation.selector_length,
            declarations, operation.declaration_length);
    }
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
        if (name == NULL || !span_equal(name, name_length, "style")) {
            parsed = false;
            break;
        }
        stylesheet_note_style_source(parse->sheet, element);
        if (!tilefinch_csp_allows_inline_style(
                input->policy, element)) continue;
        for (lxb_dom_node_t *child = element->first_child;
             parsed && child != NULL; child = child->next) {
            size_t length = 0;
            const char *css = document_text_data(child, &length);
            if (css == NULL) continue;
            parsed = parse_css_range(parse, css, 0, length);
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
        && builder->length < source_length) {
        unsigned char *result = budget_realloc(
            builder->budget, builder->data, builder->length);
        if (result != NULL) {
            StyleParsedIrHeader header = {
                .magic = STYLE_PARSED_IR_MAGIC,
                .version = STYLE_PARSED_IR_VERSION,
                .flags = builder->has_motion_keyframes
                    ? STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES : 0,
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

bool stylesheet_add_css_from_context_capture_ir(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    unsigned char **ir_data, size_t *ir_length)
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
            ? STYLE_PARSED_IR_MAX_BYTES : (length != 0 ? length - 1u : 0),
        .eligible = length > sizeof(StyleParsedIrHeader)
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
    style_parsed_ir_builder_finish(
        &builder, sheet->viewport_width, sheet->viewport_height, length,
        parsed,
        ir_data, ir_length);
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
        || (header.flags & ~STYLE_PARSED_IR_HAS_MOTION_KEYFRAMES) != 0
        || header.operation_count == 0
        || header.operation_count
               > STYLE_PARSED_IR_MAX_BYTES
                   / (sizeof(StyleParsedIrOperation) + 1u)
        || header.viewport_width != (uint32_t) sheet->viewport_width
        || header.viewport_height != (uint32_t) sheet->viewport_height
        || header.payload_bytes != ir_length - sizeof(header)) return false;
    size_t at = sizeof(header);
    for (size_t i = 0; i < header.operation_count; i++) {
        if (sizeof(StyleParsedIrOperation) > ir_length - at) return false;
        StyleParsedIrOperation operation;
        memcpy(&operation, ir_data + at, sizeof(operation));
        at += sizeof(operation);
        if (operation.reserved != 0 || operation.selector_length == 0
            || operation.selector_length > ir_length - at) return false;
        at += operation.selector_length;
        if (operation.declaration_length > ir_length - at) return false;
        at += operation.declaration_length;
    }
    if (at != ir_length) return false;
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

bool stylesheet_tokens_may_affect_discovery(
    const Stylesheet *sheet, const uint32_t *hashes, size_t count)
{
    if (sheet == NULL || hashes == NULL || count == 0) return true;
    if (sheet->rule_filters == NULL || !sheet->rule_index_ready) return true;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRuleFilter *filter = &sheet->rule_filters[i];
        if (!filter->discovery) continue;
        const StyleRule *rule = &sheet->rules[i];
        if (filter->relational_opaque || !rule->has_fast_key) return true;
        for (size_t r = 0; r < filter->relational_count; r++) {
            for (size_t h = 0; h < count; h++) {
                if (filter->relational_tokens[r] == hashes[h]) return true;
            }
        }
        if (rule->type == SELECTOR_CLASS || rule->type == SELECTOR_ID) {
            const char *key = style_rule_fast_key(rule);
            if (key == NULL) return true;
            uint32_t hash = stylesheet_identity_token_hash(
                rule->type == SELECTOR_ID, key, rule->fast_key_length);
            for (size_t h = 0; h < count; h++) {
                if (hash == hashes[h]) return true;
            }
        }
    }
    return false;
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

bool stylesheet_append_style_elements_tracked(
    Stylesheet *sheet, lxb_dom_node_t *const *elements, size_t count,
    const TilefinchContentSecurityPolicy *content_security_policy,
    size_t after_source, StylesheetAppendResult *result)
{
    if (result == NULL) return false;
    memset(result, 0, sizeof(*result));
    if (sheet == NULL || sheet->budget == NULL) return false;
    size_t old_count = sheet->count;
    size_t old_sources = sheet->style_source_count;
    if (old_count > UINT16_MAX || after_source > old_sources) return false;
    unsigned *old_orders = old_count == 0 ? NULL
        : budget_malloc(sheet->budget, old_count * sizeof(*old_orders));
    if (old_count != 0 && old_orders == NULL) return false;
    for (size_t i = 0; i < old_count; i++) {
        old_orders[i] = sheet->rules[i].order;
    }
    uint64_t signature_before = stylesheet_parse_context_signature(sheet);
    unsigned tail_start = sheet->next_order;
    bool ok = stylesheet_append_style_elements(
        sheet, elements, count, content_security_policy);
    if (!ok) {
        budget_free(sheet->budget, old_orders);
        return false;
    }
    result->old_count = old_count;
    result->context_changed =
        stylesheet_parse_context_signature(sheet) != signature_before;
    /* Rule identity survives the cascade re-sort through the unique `order`
       assigned at parse. The appended rules were numbered after every
       existing rule; move them to their document position by shifting the
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
        for (size_t j = 0; j < sheet->revert_rule_mask_count; j++) {
            sheet->revert_rule_masks[j].order = stylesheet_inserted_rule_order(
                sheet->revert_rule_masks[j].order, tail_start, boundary,
                appended_orders);
        }
        if (sheet->revert_rule_mask_count > 1) {
            qsort(sheet->revert_rule_masks, sheet->revert_rule_mask_count,
                  sizeof(*sheet->revert_rule_masks),
                  stylesheet_compare_revert_order);
        }
        for (size_t s = 0; s < sheet->style_source_count; s++) {
            unsigned order = sheet->style_source_first_order[s];
            if (s >= old_sources) {
                sheet->style_source_first_order[s] =
                    order - tail_start + boundary;
            } else if (order >= boundary) {
                sheet->style_source_first_order[s] = order + appended_orders;
            }
        }
        stylesheet_drop_selector_program(sheet);
        stylesheet_drop_rule_index(sheet);
        stylesheet_drop_custom_rule_index(sheet);
        stylesheet_finalize_rule_order(sheet);
        /* Keep the source list in document order as well. */
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
    size_t order_span = (size_t) sheet->next_order + 1u;
    uint16_t *by_order = sheet->count <= UINT16_MAX
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
    for (size_t j = 0; j < sheet->count; j++) {
        unsigned order = sheet->rules[j].order;
        if ((size_t) order < order_span) by_order[order] = (uint16_t) j;
    }
    bool remap_valid = true;
    uint16_t *remap = old_count == 0 ? NULL
        : budget_malloc(sheet->budget, old_count * sizeof(*remap));
    if (old_count != 0 && remap == NULL) remap_valid = false;
    for (size_t i = 0; remap_valid && i < old_count; i++) {
        unsigned order = old_orders[i];
        if (shifted && order != UINT_MAX && order >= boundary) {
            order += appended_orders;
        }
        if ((size_t) order >= order_span || by_order[order] == UINT16_MAX) {
            remap_valid = false;
            break;
        }
        remap[i] = by_order[order];
        old_present[order] = 1;
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
            unsigned order = sheet->rules[j].order;
            if ((size_t) order < order_span && old_present[order]) continue;
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
            hash, variable->name, strlen(variable->name) + 1u);
        hash = stylesheet_signature_bytes(
            hash, variable->value, strlen(variable->value) + 1u);
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
        if (declaration == NULL) continue;
        DisplayMode display = declaration->values.display;
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
    if (sheet->budget != NULL) {
        for (size_t i = 0; i < sheet->declaration_count; i++) {
            budget_free(sheet->budget,
                        sheet->declarations[i].deferred_declarations);
        }
        budget_free(sheet->budget, sheet->rules);
        budget_free(sheet->budget, sheet->focus_rule_indices);
        budget_free(sheet->budget, sheet->declarations);
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
        budget_free(sheet->budget, sheet->deferred_instructions);
        budget_free(sheet->budget, sheet->resolve_scratch);
        budget_free(sheet->budget, sheet->rule_index_buckets);
        budget_free(sheet->budget, sheet->rule_index_entries);
        budget_free(sheet->budget, sheet->rule_filters);
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
