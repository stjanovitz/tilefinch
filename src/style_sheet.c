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

static void style_parsed_ir_builder_disqualify(StyleCssParseContext *context)
{
    if (context != NULL && context->parsed_ir != NULL) {
        context->parsed_ir->eligible = false;
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
        builder->eligible = false;
        return true;
    }
    size_t added = sizeof(StyleParsedIrOperation) + selector_length
        + declaration_length;
    if (added > STYLE_PARSED_IR_MAX_BYTES - builder->length) {
        builder->eligible = false;
        return true;
    }
    size_t required = builder->length + added;
    if (required > builder->capacity) {
        size_t capacity = builder->capacity == 0 ? 4096u
            : builder->capacity;
        while (capacity < required) {
            size_t next = capacity > STYLE_PARSED_IR_MAX_BYTES / 2u
                ? STYLE_PARSED_IR_MAX_BYTES : capacity * 2u;
            if (next <= capacity) {
                builder->eligible = false;
                return true;
            }
            capacity = next;
        }
        unsigned char *grown = budget_realloc(
            builder->budget, builder->data, capacity);
        if (grown == NULL) {
            builder->eligible = false;
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
            /* The remaining left side is not necessarily in the candidate
               node's ancestor chain, so it cannot safely participate. */
            return style_token_bloom_empty();
        case STYLE_SELECTOR_TAG_ID:
            if (in_ancestor) {
                style_token_bloom_merge(
                    &bloom, style_compound_tag_id_bloom(op->text_offset));
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
        case STYLE_SELECTOR_END:
            return bloom;
        default:
            return style_token_bloom_empty();
        }
    }
    return style_token_bloom_empty();
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

static uint64_t style_rule_key_hash(SelectorType type, const char *text,
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
    return hash == 0 ? UINT64_C(1) : (uint64_t) hash;
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
    SelectorType type, const char *text, size_t length, uint64_t hash)
{
    if (bucket->representative == STYLE_RULE_INDEX_EMPTY
        || bucket->hash != hash
        || bucket->representative >= sheet->count) return false;
    const StyleRule *rule = &sheet->rules[bucket->representative];
    const char *fast_key = style_rule_fast_key(rule);
    return rule->has_fast_key && rule->type == type
        && fast_key != NULL && rule->fast_key_length == length
        && memcmp(fast_key, text, length) == 0;
}

StyleRuleIndexBucket *style_rule_find_bucket(
    const Stylesheet *sheet, SelectorType type, const char *text,
    size_t length, bool create)
{
    if (sheet == NULL || sheet->rule_index_buckets == NULL
        || sheet->rule_index_bucket_count == 0 || text == NULL) return NULL;
    uint64_t hash = style_rule_key_hash(type, text, length);
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
                                          hash)) return bucket;
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
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (!rule->has_fast_key) {
            universal_count++;
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
            sheet, (SelectorType) rule->type, fast_key, length, true);
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
    size_t universal_at = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        if (!rule->has_fast_key) {
            entries[universal_at++] = (uint32_t) i;
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
            rule->fast_key_length, false);
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
    update_cascade_ranges(sheet);
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
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->resolve_scratch->current_image_source_base = source_base_url;
    sheet->resolve_scratch->current_image_source_referrer_policy = retained_policy;
    sheet->resolve_scratch->current_image_source_slot = 0;
    bool own_suffix_state = !sheet->rule_batch_active;
    if (own_suffix_state) stylesheet_suffix_state_begin(sheet);
    StyleCssParseContext parse = {
        .sheet = sheet,
        .parsed_ir = parsed_ir,
        .discover_font_faces = true
    };
    bool parsed = parse_css_range(&parse, css, 0, length);
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) style_css_parse_context_dispose(&parse);
    *sheet->resolve_scratch = saved_scratch;
    if (!parsed) {
        if (own_suffix_state) stylesheet_suffix_state_finish(sheet, false);
        return false;
    }
    sheet->rule_batch_dirty = true;
    if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, true);
    return true;
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
        .eligible = true
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
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->resolve_scratch->current_image_source_base = source_base_url;
    sheet->resolve_scratch->current_image_source_referrer_policy =
        retained_policy;
    sheet->resolve_scratch->current_image_source_slot = 0;
    bool own_suffix_state = !sheet->rule_batch_active;
    if (own_suffix_state) stylesheet_suffix_state_begin(sheet);
    StyleCssParseContext parse = {
        .sheet = sheet,
        .discover_font_faces = true
    };
    size_t at = sizeof(header);
    bool parsed = true;
    for (size_t i = 0; parsed && i < header.operation_count; i++) {
        StyleParsedIrOperation operation;
        memcpy(&operation, ir_data + at, sizeof(operation));
        at += sizeof(operation);
        const char *selectors = (const char *) ir_data + at;
        at += operation.selector_length;
        const char *declarations = (const char *) ir_data + at;
        at += operation.declaration_length;
        parsed = selector_list_to_rules(
            &parse, selectors, operation.selector_length,
            declarations, operation.declaration_length);
    }
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) style_css_parse_context_dispose(&parse);
    *sheet->resolve_scratch = saved_scratch;
    if (!parsed) {
        if (own_suffix_state) stylesheet_suffix_state_finish(sheet, false);
        return STYLE_PARSED_IR_FAILED;
    }
    sheet->rule_batch_dirty = true;
    if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, true);
    if (operation_count != NULL) *operation_count = header.operation_count;
    return STYLE_PARSED_IR_APPLIED;
}

bool stylesheet_append_style_elements(
    Stylesheet *sheet, lxb_dom_node_t *const *elements, size_t count,
    const TilefinchContentSecurityPolicy *content_security_policy)
{
    if (sheet == NULL || sheet->budget == NULL
        || (elements == NULL && count != 0)) return false;
    if (count == 0) return true;
    sheet->build_generation = stylesheet_next_generation();
    bool own_suffix_state = !sheet->rule_batch_active;
    if (own_suffix_state) stylesheet_suffix_state_begin(sheet);
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->resolve_scratch->current_image_source_base = NULL;
    sheet->resolve_scratch->current_image_source_referrer_policy = NULL;
    sheet->resolve_scratch->current_image_source_slot = 0;
    StyleCssParseContext parse = {
        .sheet = sheet,
        .discover_font_faces = true
    };
    bool parsed = true;
    for (size_t i = 0; parsed && i < count; i++) {
        lxb_dom_node_t *element = elements[i];
        size_t name_length = 0;
        const char *name = document_element_name(element, &name_length);
        if (name == NULL || !span_equal(name, name_length, "style")) {
            parsed = false;
            break;
        }
        if (!tilefinch_csp_allows_inline_style(
                content_security_policy, element)) continue;
        for (lxb_dom_node_t *child = element->first_child;
             parsed && child != NULL; child = child->next) {
            size_t length = 0;
            const char *css = document_text_data(child, &length);
            if (css == NULL) continue;
            parsed = parse_css_range(&parse, css, 0, length);
        }
    }
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) style_css_parse_context_dispose(&parse);
    *sheet->resolve_scratch = saved_scratch;
    if (!parsed) {
        if (own_suffix_state) stylesheet_suffix_state_finish(sheet, false);
        return false;
    }
    sheet->rule_batch_dirty = true;
    if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, true);
    return true;
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
    bool own_suffix_state = !sheet->rule_batch_active;
    if (own_suffix_state) stylesheet_suffix_state_begin(sheet);
    unsigned previous_origin = sheet->current_origin;
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    sheet->current_origin = 1;
    sheet->resolve_scratch->current_image_source_base = NULL;
    sheet->resolve_scratch->current_image_source_referrer_policy = NULL;
    sheet->resolve_scratch->current_image_source_slot = 0;
    StyleCssParseContext parse = {
        .sheet = sheet,
        .discover_font_faces = false
    };
    bool parsed = parse_css_range(&parse, css, 0, length);
    parsed = parsed && style_css_parse_context_finish(&parse);
    if (!parsed) style_css_parse_context_dispose(&parse);
    sheet->current_origin = previous_origin;
    *sheet->resolve_scratch = saved_scratch;
    if (!parsed) {
        if (own_suffix_state) stylesheet_suffix_state_finish(sheet, false);
        return false;
    }
    sheet->rule_batch_dirty = true;
    if (!sheet->rule_batch_active) stylesheet_finalize_rule_order(sheet);
    if (own_suffix_state) stylesheet_suffix_state_finish(sheet, true);
    return true;
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
