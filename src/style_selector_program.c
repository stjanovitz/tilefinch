/* Bounded compilation of the common tag/ID/class selector subset into
   compact right-to-left programs. Complex selectors remain on the string
   matcher in style_match.c. */

#include "style_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor/tag/tag.h>

void stylesheet_drop_selector_program(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    budget_free(sheet->budget, sheet->selector_program);
    budget_free(sheet->budget, sheet->selector_program_offsets);
    sheet->selector_program = NULL;
    sheet->selector_program_offsets = NULL;
    sheet->selector_program_instruction_count = 0;
    sheet->selector_program_rule_count = 0;
    sheet->selector_program_bytes = 0;
    sheet->selector_program_ready = false;
    sheet->selector_program_attempted = false;
}

#define STYLE_SELECTOR_PROGRAM_BUDGET (256u * 1024u)
#define STYLE_COMPILED_FRAGMENT_MAGIC UINT32_C(0x54465346)
#define STYLE_COMPILED_FRAGMENT_VERSION UINT16_C(1)
#define STYLE_COMPILED_FRAGMENT_MAX_BYTES (256u * 1024u)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t rule_count;
    uint32_t instruction_count;
    uint32_t selector_bytes;
} StyleCompiledFragmentHeader;

typedef struct {
    uint32_t selector_offset;
    uint32_t instruction_offset;
    uint16_t selector_length;
    uint16_t instruction_count;
    uint8_t important;
    uint8_t reserved[3];
} StyleCompiledFragmentEntry;

static bool style_selector_complex_compounds_enabled(void)
{
#ifndef TILEFINCH_NO_TRACE
    return getenv("TILEFINCH_DISABLE_COMPILED_COMPLEX_SELECTORS") == NULL;
#else
    return true;
#endif
}

typedef struct {
    const char *selector;
    StyleSelectorInstruction *instructions;
    size_t count;
    size_t capacity;
    size_t combinators;
    bool complex_compounds;
} StyleSelectorBuilder;

static bool style_selector_builder_emit(StyleSelectorBuilder *builder,
                                        StyleSelectorOpcode opcode,
                                        const char *text, size_t length)
{
    if (builder == NULL || length > UINT8_MAX
        || (text != NULL
            && (text < builder->selector
                || (size_t) (text - builder->selector) > UINT16_MAX))) {
        return false;
    }
    if (builder->instructions != NULL) {
        if (builder->count >= builder->capacity) return false;
        builder->instructions[builder->count] = (StyleSelectorInstruction) {
            .text_offset = text == NULL
                ? 0 : (uint16_t) (text - builder->selector),
            .text_length = (uint8_t) length,
            .opcode = (uint8_t) opcode
        };
    }
    builder->count++;
    return true;
}

static uint16_t style_selector_builtin_tag_id(
    const char *text, size_t length)
{
    if (text == NULL || length == 0) return UINT16_MAX;
    for (lxb_tag_id_t id = LXB_TAG__BEGIN;
         id < LXB_TAG__LAST_ENTRY && id <= UINT16_MAX; id++) {
        size_t name_length = 0;
        const char *name = (const char *) lxb_tag_name_by_id(
            id, &name_length);
        /* Preserve exact convergence with the string matcher. The parser's
           common HTML selector spelling is lowercase; unusual/custom or
           differently-cased names stay on the established text path. */
        if (name != NULL && name_length == length
            && memcmp(name, text, length) == 0) return (uint16_t) id;
    }
    return UINT16_MAX;
}

static bool style_selector_builder_emit_tag(
    StyleSelectorBuilder *builder, const char *text, size_t length)
{
    uint16_t tag_id = style_selector_builtin_tag_id(text, length);
    if (tag_id == UINT16_MAX) {
        return style_selector_builder_emit(
            builder, STYLE_SELECTOR_TAG, text, length);
    }
    if (builder == NULL) return false;
    if (builder->instructions != NULL) {
        if (builder->count >= builder->capacity) return false;
        builder->instructions[builder->count] =
            (StyleSelectorInstruction) {
                .text_offset = tag_id,
                .text_length = 0,
                .opcode = STYLE_SELECTOR_TAG_ID
            };
    }
    builder->count++;
    return true;
}

static bool style_selector_compile_simple_compound(
    StyleSelectorBuilder *builder, const char *text, size_t length)
{
    trim(&text, &length);
    if (length == 0) return false;
    size_t at = 0;
    if (text[at] == '*') {
        at++;
    } else if (name_character(text[at])) {
        size_t end = skip_selector_identifier(text, length, at);
        if (end == at || memchr(text + at, '\\', end - at) != NULL
            || !style_selector_builder_emit_tag(
                builder, text + at, end - at)) {
            return false;
        }
        at = end;
    }
    while (at < length) {
        if (text[at] == '.' || text[at] == '#') {
            char marker = text[at++];
            size_t end = skip_selector_identifier(text, length, at);
            if (end == at || memchr(text + at, '\\', end - at) != NULL
                || !style_selector_builder_emit(
                    builder,
                    marker == '.' ? STYLE_SELECTOR_CLASS
                                  : STYLE_SELECTOR_ID,
                    text + at, end - at)) {
                return false;
            }
            at = end;
        } else if (isspace((unsigned char) text[at])) {
            while (at < length
                   && isspace((unsigned char) text[at])) at++;
            if (at != length) return false;
        } else {
            return false;
        }
    }
    return true;
}

static bool style_selector_compile_compound(StyleSelectorBuilder *builder,
                                            const char *text, size_t length)
{
    trim(&text, &length);
    if (builder == NULL || length == 0) return false;
    size_t checkpoint = builder->count;
    if (style_selector_compile_simple_compound(builder, text, length)) {
        return true;
    }
    /* Keep complex standards-valid compounds in the compact combinator
       program too. The established string matcher remains their single
       semantic implementation; this opcode only avoids reparsing the full
       selector and its combinator prefixes on every candidate node. */
    builder->count = checkpoint;
    if (!builder->complex_compounds) return false;
    return style_selector_builder_emit(
        builder, STYLE_SELECTOR_COMPOUND, text, length);
}

static size_t style_selector_compile(const StyleRule *rule,
                                     StyleSelectorInstruction *instructions,
                                     size_t capacity,
                                     bool complex_compounds)
{
    if (rule == NULL || rule->selector == NULL
        || rule->selector_length == 0) return 0;
    StyleSelectorBuilder builder = {
        .selector = rule->selector,
        .instructions = instructions,
        .capacity = capacity,
        .complex_compounds = complex_compounds
    };
    const char *text = rule->selector;
    size_t length = rule->selector_length;
    trim(&text, &length);
    while (length != 0) {
        int square = 0;
        int round = 0;
        size_t split = length;
        char combinator = 0;
        for (size_t i = length; i != 0; i--) {
            char value = text[i - 1];
            if (value == ']') square++;
            else if (value == '[' && square > 0) square--;
            else if (value == ')') round++;
            else if (value == '(' && round > 0) round--;
            else if (square == 0 && round == 0
                     && (value == '>' || value == '+' || value == '~')) {
                split = i - 1;
                combinator = value;
                break;
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
                        split = left - 1;
                        combinator = text[left - 1];
                    } else {
                        split = i - 1;
                        combinator = ' ';
                    }
                    break;
                }
            }
        }
        const char *compound = combinator == 0
            ? text : text + split + 1;
        size_t compound_length = combinator == 0
            ? length : length - split - 1;
        if (!style_selector_compile_compound(
                &builder, compound, compound_length)) return 0;
        if (combinator == 0) {
            if (!style_selector_builder_emit(
                    &builder, STYLE_SELECTOR_END, NULL, 0)) return 0;
            return builder.count;
        }
        if (++builder.combinators > STYLE_SELECTOR_PROGRAM_DEPTH_LIMIT) {
            return 0;
        }
        StyleSelectorOpcode opcode = combinator == '>'
            ? STYLE_SELECTOR_PARENT
            : (combinator == '+' ? STYLE_SELECTOR_ADJACENT
               : (combinator == '~' ? STYLE_SELECTOR_GENERAL_SIBLING
                                    : STYLE_SELECTOR_ANCESTOR));
        if (!style_selector_builder_emit(&builder, opcode, NULL, 0)) return 0;
        length = split;
        while (length != 0
               && isspace((unsigned char) text[length - 1])) length--;
    }
    return 0;
}

static void stylesheet_release_fragment_seeds(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return;
    budget_free(sheet->budget, sheet->selector_fragment_program);
    budget_free(sheet->budget, sheet->selector_fragment_offsets);
    budget_free(sheet->budget, sheet->selector_fragment_counts);
    sheet->selector_fragment_program = NULL;
    sheet->selector_fragment_offsets = NULL;
    sheet->selector_fragment_counts = NULL;
    sheet->selector_fragment_instruction_count = 0;
    sheet->selector_fragment_offset_count = 0;
}

static bool style_fragment_size_add(size_t *total, size_t added)
{
    if (total == NULL || added > SIZE_MAX - *total) return false;
    *total += added;
    return *total <= STYLE_COMPILED_FRAGMENT_MAX_BYTES;
}

bool stylesheet_compiled_fragment_build(
    Stylesheet *sheet, size_t rule_begin, size_t rule_end,
    unsigned char **data, size_t *length)
{
    if (data != NULL) *data = NULL;
    if (length != NULL) *length = 0;
    if (sheet == NULL || sheet->budget == NULL || data == NULL
        || length == NULL || rule_begin >= rule_end
        || rule_end > sheet->count
        || rule_end - rule_begin > UINT32_MAX) return false;
    size_t rule_count = rule_end - rule_begin;
    bool complex_compounds = style_selector_complex_compounds_enabled();
    size_t selector_bytes = 0;
    size_t instruction_count = 0;
    for (size_t i = rule_begin; i < rule_end; i++) {
        const StyleRule *rule = &sheet->rules[i];
        size_t count = style_selector_compile(
            rule, NULL, 0, complex_compounds);
        if (rule->selector == NULL || rule->selector_length == 0
            || count > UINT16_MAX
            || selector_bytes > UINT32_MAX - rule->selector_length
            || instruction_count > UINT32_MAX - count) {
#ifndef TILEFINCH_NO_TRACE
            if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
                fprintf(stderr,
                        "style-fragment-skip invalid-rule=%zu selector=%p "
                        "length=%u instructions=%zu\n",
                        i - rule_begin, (const void *) rule->selector,
                        (unsigned) rule->selector_length, count);
            }
#endif
            return false;
        }
        selector_bytes += rule->selector_length;
        instruction_count += count;
    }
    if (instruction_count == 0) return false;
    if (selector_bytes > SIZE_MAX - 3u) return false;
    size_t selector_storage_bytes = (selector_bytes + 3u) & ~(size_t) 3u;
    if (selector_storage_bytes < selector_bytes) return false;
    size_t total = sizeof(StyleCompiledFragmentHeader);
    if (rule_count > SIZE_MAX / sizeof(StyleCompiledFragmentEntry)
        || !style_fragment_size_add(
               &total, rule_count * sizeof(StyleCompiledFragmentEntry))
        || !style_fragment_size_add(&total, selector_storage_bytes)
        || instruction_count
               > SIZE_MAX / sizeof(StyleSelectorInstruction)
        || !style_fragment_size_add(
               &total,
               instruction_count * sizeof(StyleSelectorInstruction))) {
#ifndef TILEFINCH_NO_TRACE
        if (getenv("TILEFINCH_TRACE_STYLESHEETS") != NULL) {
            fprintf(stderr,
                    "style-fragment-skip rules=%zu selectors=%zu "
                    "instructions=%zu limit=%u\n",
                    rule_count, selector_storage_bytes,
                    instruction_count,
                    (unsigned) STYLE_COMPILED_FRAGMENT_MAX_BYTES);
        }
#endif
        return false;
    }
    /* The fragment outlives this page-owned stylesheet when the session
       cache adopts it, so keep its ledger charge with session state rather
       than making retained cache bytes look like live style memory. */
    unsigned char *blob = budget_malloc_category(
        sheet->budget, BUDGET_CATEGORY_SESSION, total);
    if (blob == NULL) return false;
    StyleCompiledFragmentHeader header = {
        .magic = STYLE_COMPILED_FRAGMENT_MAGIC,
        .version = STYLE_COMPILED_FRAGMENT_VERSION,
        .reserved = complex_compounds ? 1u : 0u,
        .viewport_width = (uint32_t) sheet->viewport_width,
        .viewport_height = (uint32_t) sheet->viewport_height,
        .rule_count = (uint32_t) rule_count,
        .instruction_count = (uint32_t) instruction_count,
        .selector_bytes = (uint32_t) selector_storage_bytes
    };
    memcpy(blob, &header, sizeof(header));
    StyleCompiledFragmentEntry *entries =
        (StyleCompiledFragmentEntry *) (blob + sizeof(header));
    unsigned char *selectors = blob + sizeof(header)
        + rule_count * sizeof(*entries);
    if (selector_storage_bytes > selector_bytes) {
        memset(selectors + selector_bytes, 0,
               selector_storage_bytes - selector_bytes);
    }
    StyleSelectorInstruction *instructions =
        (StyleSelectorInstruction *) (selectors + selector_storage_bytes);
    size_t selector_at = 0;
    size_t instruction_at = 0;
    for (size_t i = 0; i < rule_count; i++) {
        const StyleRule *rule = &sheet->rules[rule_begin + i];
        size_t count = style_selector_compile(
            rule, NULL, 0, complex_compounds);
        entries[i] = (StyleCompiledFragmentEntry) {
            .selector_offset = (uint32_t) selector_at,
            .instruction_offset = (uint32_t) instruction_at,
            .selector_length = rule->selector_length,
            .instruction_count = (uint16_t) count,
            .important = rule->important ? 1u : 0u
        };
        memcpy(selectors + selector_at, rule->selector,
               rule->selector_length);
        if (count != 0) {
            size_t emitted = style_selector_compile(
                rule, instructions + instruction_at,
                instruction_count - instruction_at, complex_compounds);
            if (emitted != count) {
                budget_free(sheet->budget, blob);
                return false;
            }
        }
        selector_at += rule->selector_length;
        instruction_at += count;
    }
    *data = blob;
    *length = total;
    return true;
}

size_t stylesheet_compiled_fragment_apply(
    Stylesheet *sheet, size_t rule_begin, size_t rule_end,
    const unsigned char *data, size_t length)
{
    if (sheet == NULL || sheet->budget == NULL || data == NULL
        || length < sizeof(StyleCompiledFragmentHeader)
        || rule_begin >= rule_end || rule_end > sheet->count) return 0;
    StyleCompiledFragmentHeader header;
    memcpy(&header, data, sizeof(header));
    if (header.magic != STYLE_COMPILED_FRAGMENT_MAGIC
        || header.version != STYLE_COMPILED_FRAGMENT_VERSION
        || header.reserved
               != (style_selector_complex_compounds_enabled() ? 1u : 0u)
        || header.viewport_width != (uint32_t) sheet->viewport_width
        || header.viewport_height != (uint32_t) sheet->viewport_height
        || header.rule_count != rule_end - rule_begin) return 0;
    if ((size_t) header.rule_count
            > SIZE_MAX / sizeof(StyleCompiledFragmentEntry)
        || (size_t) header.instruction_count
            > SIZE_MAX / sizeof(StyleSelectorInstruction)) return 0;
    size_t entry_bytes = (size_t) header.rule_count
        * sizeof(StyleCompiledFragmentEntry);
    size_t instruction_bytes = (size_t) header.instruction_count
        * sizeof(StyleSelectorInstruction);
    size_t expected = sizeof(header);
    if (entry_bytes > SIZE_MAX - expected) return 0;
    expected += entry_bytes;
    if (header.selector_bytes > SIZE_MAX - expected) return 0;
    expected += header.selector_bytes;
    if (instruction_bytes > SIZE_MAX - expected) return 0;
    expected += instruction_bytes;
    if (expected != length || expected > STYLE_COMPILED_FRAGMENT_MAX_BYTES) {
        return 0;
    }
    const StyleCompiledFragmentEntry *entries =
        (const StyleCompiledFragmentEntry *) (data + sizeof(header));
    const unsigned char *selectors = data + sizeof(header) + entry_bytes;
    const StyleSelectorInstruction *instructions =
        (const StyleSelectorInstruction *)
            (selectors + header.selector_bytes);
    size_t seeded = 0;
    for (size_t i = 0; i < header.rule_count; i++) {
        StyleCompiledFragmentEntry entry;
        memcpy(&entry, &entries[i], sizeof(entry));
        const StyleRule *rule = &sheet->rules[rule_begin + i];
        if (entry.selector_offset > header.selector_bytes
            || entry.selector_length
                   > header.selector_bytes - entry.selector_offset
            || entry.instruction_offset > header.instruction_count
            || entry.instruction_count
                   > header.instruction_count - entry.instruction_offset
            || entry.selector_length != rule->selector_length
            || entry.important != (rule->important ? 1u : 0u)
            || memcmp(selectors + entry.selector_offset, rule->selector,
                      rule->selector_length) != 0) return 0;
        if (entry.instruction_count != 0) seeded++;
    }
    size_t offset_count = (size_t) sheet->next_order;
    if (seeded == 0 || offset_count > SIZE_MAX / 2u) return 0;
    offset_count *= 2u;
    if (offset_count == 0 || offset_count > UINT16_MAX
        || header.instruction_count > UINT16_MAX
        || sheet->selector_fragment_instruction_count
               > UINT16_MAX - header.instruction_count) return 0;
    uint16_t *offsets = budget_malloc(
        sheet->budget, offset_count * sizeof(*offsets));
    uint16_t *counts = budget_malloc(
        sheet->budget, offset_count * sizeof(*counts));
    size_t combined_instructions =
        sheet->selector_fragment_instruction_count
        + header.instruction_count;
    StyleSelectorInstruction *program = budget_malloc(
        sheet->budget, combined_instructions * sizeof(*program));
    if (offsets == NULL || counts == NULL || program == NULL) {
        budget_free(sheet->budget, offsets);
        budget_free(sheet->budget, counts);
        budget_free(sheet->budget, program);
        return 0;
    }
    for (size_t i = 0; i < offset_count; i++) {
        offsets[i] = UINT16_MAX;
        counts[i] = 0;
    }
    size_t old_offset_count = sheet->selector_fragment_offset_count;
    if (old_offset_count > offset_count) old_offset_count = offset_count;
    if (old_offset_count != 0) {
        memcpy(offsets, sheet->selector_fragment_offsets,
               old_offset_count * sizeof(*offsets));
        memcpy(counts, sheet->selector_fragment_counts,
               old_offset_count * sizeof(*counts));
    }
    size_t prior_instructions = sheet->selector_fragment_instruction_count;
    if (prior_instructions != 0) {
        memcpy(program, sheet->selector_fragment_program,
               prior_instructions * sizeof(*program));
    }
    if (header.instruction_count != 0) {
        memcpy(program + prior_instructions, instructions,
               header.instruction_count * sizeof(*program));
    }
    for (size_t i = 0; i < header.rule_count; i++) {
        StyleCompiledFragmentEntry entry;
        memcpy(&entry, &entries[i], sizeof(entry));
        if (entry.instruction_count == 0) continue;
        const StyleRule *rule = &sheet->rules[rule_begin + i];
        size_t key = (size_t) rule->order * 2u
            + (rule->important ? 1u : 0u);
        if (key >= offset_count
            || prior_instructions + entry.instruction_offset > UINT16_MAX) {
            budget_free(sheet->budget, offsets);
            budget_free(sheet->budget, counts);
            budget_free(sheet->budget, program);
            return 0;
        }
        offsets[key] = (uint16_t) (
            prior_instructions + entry.instruction_offset);
        counts[key] = entry.instruction_count;
    }
    budget_free(sheet->budget, sheet->selector_fragment_offsets);
    budget_free(sheet->budget, sheet->selector_fragment_counts);
    budget_free(sheet->budget, sheet->selector_fragment_program);
    sheet->selector_fragment_offsets = offsets;
    sheet->selector_fragment_counts = counts;
    sheet->selector_fragment_program = program;
    sheet->selector_fragment_offset_count = offset_count;
    sheet->selector_fragment_instruction_count = combined_instructions;
    return seeded;
}

void stylesheet_prepare_selector_program(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL
        || sheet->selector_program_ready
        || sheet->selector_program_attempted) return;
    sheet->selector_program_attempted = true;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_COMPILED_SELECTORS") != NULL
        || sheet->count == 0 || sheet->count > UINT16_MAX) {
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
#else
    if (sheet->count == 0 || sheet->count > UINT16_MAX) {
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
#endif
    size_t offset_bytes = sheet->count * sizeof(uint16_t);
    if (offset_bytes >= STYLE_SELECTOR_PROGRAM_BUDGET) {
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
    size_t instruction_limit =
        (STYLE_SELECTOR_PROGRAM_BUDGET - offset_bytes)
        / sizeof(StyleSelectorInstruction);
    if (instruction_limit >= UINT16_MAX) instruction_limit = UINT16_MAX - 1u;
#ifndef TILEFINCH_NO_TRACE
    bool complex_compounds = style_selector_complex_compounds_enabled();
#else
    bool complex_compounds = style_selector_complex_compounds_enabled();
#endif
    uint16_t *offsets = budget_malloc(sheet->budget, offset_bytes);
    if (offsets == NULL) {
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
    size_t wanted = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        size_t key = (size_t) rule->order * 2u
            + (rule->important ? 1u : 0u);
        size_t count = key < sheet->selector_fragment_offset_count
            && sheet->selector_fragment_offsets != NULL
            && sheet->selector_fragment_counts != NULL
            && sheet->selector_fragment_offsets[key] != UINT16_MAX
                ? sheet->selector_fragment_counts[key]
                : style_selector_compile(
                      rule, NULL, 0, complex_compounds);
        if (count != 0 && count <= instruction_limit - wanted) {
            offsets[i] = (uint16_t) count;
            wanted += count;
        } else {
            offsets[i] = UINT16_MAX;
        }
    }
    if (wanted == 0) {
        budget_free(sheet->budget, offsets);
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
    StyleSelectorInstruction *program = budget_malloc(
        sheet->budget, wanted * sizeof(*program));
    if (program == NULL) {
        budget_free(sheet->budget, offsets);
        stylesheet_release_fragment_seeds(sheet);
        return;
    }
    size_t at = 0;
    size_t compiled = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        size_t count = offsets[i];
        if (count == UINT16_MAX || count > wanted - at) continue;
        const StyleRule *rule = &sheet->rules[i];
        size_t key = (size_t) rule->order * 2u
            + (rule->important ? 1u : 0u);
        size_t emitted = 0;
        if (key < sheet->selector_fragment_offset_count
            && sheet->selector_fragment_offsets != NULL
            && sheet->selector_fragment_counts != NULL
            && sheet->selector_fragment_offsets[key] != UINT16_MAX
            && sheet->selector_fragment_counts[key] == count
            && sheet->selector_fragment_offsets[key]
                   <= sheet->selector_fragment_instruction_count
            && count <= sheet->selector_fragment_instruction_count
                       - sheet->selector_fragment_offsets[key]) {
            memcpy(program + at,
                   sheet->selector_fragment_program
                       + sheet->selector_fragment_offsets[key],
                   count * sizeof(*program));
            emitted = count;
        } else {
            emitted = style_selector_compile(
                rule, program + at, wanted - at, complex_compounds);
        }
        if (emitted != count || at >= UINT16_MAX) {
            offsets[i] = UINT16_MAX;
            continue;
        }
        offsets[i] = (uint16_t) at;
        at += emitted;
        compiled++;
    }
    sheet->selector_program = program;
    sheet->selector_program_offsets = offsets;
    sheet->selector_program_instruction_count = at;
    sheet->selector_program_rule_count = compiled;
    sheet->selector_program_bytes = offset_bytes
        + wanted * sizeof(*program);
    sheet->selector_program_ready = compiled != 0;
    stylesheet_release_fragment_seeds(sheet);
}

bool stylesheet_extend_selector_program(
    Stylesheet *sheet, const uint16_t *prefix_offsets,
    size_t prefix_offset_count, size_t *reused_rules,
    size_t *compiled_rules)
{
    if (reused_rules != NULL) *reused_rules = 0;
    if (compiled_rules != NULL) *compiled_rules = 0;
    if (sheet == NULL || sheet->budget == NULL
        || prefix_offsets == NULL || prefix_offset_count == 0
        || sheet->selector_program == NULL
        || sheet->selector_program_offsets == NULL
        || sheet->count == 0 || sheet->count > UINT16_MAX) return false;
    size_t offset_bytes = sheet->count * sizeof(uint16_t);
    if (offset_bytes >= STYLE_SELECTOR_PROGRAM_BUDGET) return false;
    size_t instruction_limit =
        (STYLE_SELECTOR_PROGRAM_BUDGET - offset_bytes)
        / sizeof(StyleSelectorInstruction);
    if (instruction_limit >= UINT16_MAX) instruction_limit = UINT16_MAX - 1u;
    if (sheet->selector_program_instruction_count > instruction_limit) {
        return false;
    }
#ifndef TILEFINCH_NO_TRACE
    bool complex_compounds = style_selector_complex_compounds_enabled();
#else
    bool complex_compounds = style_selector_complex_compounds_enabled();
#endif
    uint16_t *offsets = budget_malloc(sheet->budget, offset_bytes);
    if (offsets == NULL) return false;
    size_t wanted = 0;
    size_t reused = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        size_t key = (size_t) rule->order * 2u
            + (rule->important ? 1u : 0u);
        if (key < prefix_offset_count) {
            offsets[i] = prefix_offsets[key];
            if (offsets[i] != UINT16_MAX) reused++;
            continue;
        }
        size_t count = style_selector_compile(
            rule, NULL, 0, complex_compounds);
        if (count != 0
            && count <= instruction_limit
                         - sheet->selector_program_instruction_count
                         - wanted) {
            offsets[i] = (uint16_t) count;
            wanted += count;
        } else {
            offsets[i] = UINT16_MAX;
        }
    }
    size_t original_instruction_count =
        sheet->selector_program_instruction_count;
    if (wanted != 0) {
        StyleSelectorInstruction *program = budget_realloc(
            sheet->budget, sheet->selector_program,
            (original_instruction_count + wanted) * sizeof(*program));
        if (program == NULL) {
            budget_free(sheet->budget, offsets);
            return false;
        }
        sheet->selector_program = program;
    }
    size_t at = original_instruction_count;
    size_t compiled = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        const StyleRule *rule = &sheet->rules[i];
        size_t key = (size_t) rule->order * 2u
            + (rule->important ? 1u : 0u);
        if (key < prefix_offset_count) continue;
        size_t count = offsets[i];
        if (count == UINT16_MAX || count > wanted - (at - original_instruction_count)) {
            continue;
        }
        size_t emitted = style_selector_compile(
            rule, sheet->selector_program + at,
            original_instruction_count + wanted - at,
            complex_compounds);
        if (emitted != count || at >= UINT16_MAX) {
            offsets[i] = UINT16_MAX;
            continue;
        }
        offsets[i] = (uint16_t) at;
        at += emitted;
        compiled++;
    }
    budget_free(sheet->budget, sheet->selector_program_offsets);
    sheet->selector_program_offsets = offsets;
    sheet->selector_program_instruction_count = at;
    sheet->selector_program_rule_count = reused + compiled;
    sheet->selector_program_bytes = offset_bytes
        + (original_instruction_count + wanted)
              * sizeof(*sheet->selector_program);
    sheet->selector_program_ready = reused + compiled != 0;
    sheet->selector_program_attempted = true;
    if (reused_rules != NULL) *reused_rules = reused;
    if (compiled_rules != NULL) *compiled_rules = compiled;
    return sheet->selector_program_ready;
}
