#include "tilefinch/style.h"

#include "style_internal.h"
#include "style_cache_internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const StyleDeclaration *stylesheet_rule_declaration(
    const Stylesheet *sheet, const StyleRule *rule)
{
    if (sheet == NULL || rule == NULL
        || rule->declaration_index >= sheet->declaration_count) return NULL;
    return &sheet->declarations[rule->declaration_index];
}

size_t stylesheet_retained_bytes(const Stylesheet *sheet)
{
    if (sheet == NULL) return 0;
    size_t web_font_bytes = sheet->web_fonts == NULL ? 0
        : sizeof(*sheet->web_fonts)
          + sheet->web_fonts->retained_source_bytes;
    size_t image_source_bytes = sheet->image_sources == NULL ? 0
        : sizeof(*sheet->image_sources)
          + sheet->image_sources->capacity
              * sizeof(*sheet->image_sources->items)
          + sheet->image_sources->retained_base_bytes;
    return sheet->capacity * sizeof(*sheet->rules)
        + sheet->focus_rule_count * sizeof(*sheet->focus_rule_indices)
        + sheet->declaration_capacity * sizeof(*sheet->declarations)
        + sheet->declaration_index_slot_count
            * sizeof(*sheet->declaration_index_slots)
        + sheet->selector_program_bytes
        + sheet->selector_storage_bytes
        + sheet->variable_capacity * sizeof(*sheet->variables)
        + sheet->custom_rule_capacity * sizeof(*sheet->custom_rules)
        + sheet->generated_text_capacity * sizeof(*sheet->generated_texts)
        + sheet->counter_operation_set_capacity
            * sizeof(*sheet->counter_operation_sets)
        + sheet->image_url_capacity * sizeof(*sheet->image_urls)
        + sheet->math_instruction_capacity
            * sizeof(*sheet->math_instructions)
        + sheet->math_program_capacity * sizeof(*sheet->math_programs)
        + sheet->deferred_instruction_capacity
            * sizeof(*sheet->deferred_instructions)
        + sheet->deferred_bytes + sheet->generated_text_bytes
        + sheet->image_url_bytes + sheet->rule_index_bytes
        + web_font_bytes + image_source_bytes;
}

void stylesheet_prepare_for_document_reuse(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    sheet->selector_cooperate = NULL;
    sheet->selector_cooperate_opaque = NULL;
    sheet->selector_cooperate_visits = 0;
    sheet->selector_cooperate_next = 0;
    sheet->selector_cooperate_quota = 0;
    sheet->selector_cooperate_cancelled = false;
    style_variable_cache_end(sheet);
    style_container_layout_state_clear(sheet);
    if (sheet->resolve_scratch != NULL) {
        memset(sheet->resolve_scratch, 0, sizeof(*sheet->resolve_scratch));
    }
    sheet->relative_selector_cache_depth = 0;
    sheet->relative_selector_cache_epoch = 0;
    memset(sheet->relative_selector_cache, 0,
           sizeof(sheet->relative_selector_cache));
}

bool style_resolve_value(const Stylesheet *sheet, const char *text,
                          size_t length, char *output, size_t output_size,
                          unsigned depth);
bool computed_style_has_text_underline(const ComputedStyle *style)
{
    return style != NULL
        && (style->text_decoration_state & STYLE_TEXT_DECORATION_OWN) != 0;
}

bool computed_style_has_ancestor_text_underline(const ComputedStyle *style)
{
    return style != NULL
        && (style->text_decoration_state & STYLE_TEXT_DECORATION_ANCESTOR) != 0;
}

bool computed_style_text_underline_offset(const ComputedStyle *style,
                                          int *pixels)
{
    return style_decode_text_offset(style_text_offset_code(
        style, STYLE_TEXT_OFFSET_SHIFT, STYLE_TEXT_OFFSET_MASK), pixels);
}

bool computed_style_effective_text_underline_offset(
    const ComputedStyle *style, int *pixels)
{
    unsigned code = computed_style_has_text_underline(style)
        ? style_text_offset_code(style, STYLE_TEXT_OFFSET_SHIFT,
                                 STYLE_TEXT_OFFSET_MASK)
        : style_text_offset_code(style, STYLE_ANCESTOR_TEXT_OFFSET_SHIFT,
                                 STYLE_ANCESTOR_TEXT_OFFSET_MASK);
    return style_decode_text_offset(code, pixels);
}
