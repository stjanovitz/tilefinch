#include "tilefinch/style.h"

#include "style_internal.h"
#include "style_cache_internal.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool stylesheet_head_scripts_affect_ancestors(
    const Stylesheet *sheet, lxb_dom_node_t *head, lxb_dom_node_t *node)
{
    return stylesheet_head_change_reaches_outside(sheet, head, node);
}

const StyleDeclaration *stylesheet_rule_declaration(
    const Stylesheet *sheet, const StyleRule *rule)
{
    if (sheet == NULL || rule == NULL
        || rule->declaration_index >= sheet->declaration_count) return NULL;
    return &sheet->declarations[rule->declaration_index];
}

size_t style_declaration_values_encode(const ComputedStyle *values,
                                       uint8_t *encoded, size_t capacity)
{
    if (values == NULL || encoded == NULL
        || capacity < STYLE_DECLARATION_VALUES_ENCODED_MAX) return 0;
    const unsigned char *bytes = (const unsigned char *) values;
    size_t written = 0;
    size_t word = 0;
    while (word < STYLE_DECLARATION_VALUE_WORDS) {
        uint32_t value;
        memcpy(&value, bytes + word * 4u, sizeof(value));
        if (value == 0) {
            word++;
            continue;
        }
        size_t first = word;
        while (word < STYLE_DECLARATION_VALUE_WORDS) {
            memcpy(&value, bytes + word * 4u, sizeof(value));
            if (value == 0) break;
            word++;
        }
        size_t count = word - first;
        encoded[written++] = (uint8_t) first;
        encoded[written++] = (uint8_t) count;
        memcpy(encoded + written, bytes + first * 4u, count * 4u);
        written += count * 4u;
    }
    return written;
}

void stylesheet_declaration_values(const Stylesheet *sheet,
                                   const StyleDeclaration *declaration,
                                   ComputedStyle *values)
{
    if (values == NULL) return;
    memset(values, 0, sizeof(*values));
    if (sheet == NULL || declaration == NULL
        || sheet->declaration_values == NULL
        || declaration->values_offset > sheet->declaration_value_bytes
        || declaration->values_length
               > sheet->declaration_value_bytes - declaration->values_offset)
        return;
    const uint8_t *encoded =
        sheet->declaration_values + declaration->values_offset;
    size_t length = declaration->values_length;
    unsigned char *bytes = (unsigned char *) values;
    for (size_t at = 0; length - at >= 2u;) {
        size_t first = encoded[at];
        size_t count = encoded[at + 1u];
        at += 2u;
        /* The pool is written only by the encoder; a malformed run means
           memory corruption, so stop rather than write out of bounds. */
        if (first > STYLE_DECLARATION_VALUE_WORDS
            || count > STYLE_DECLARATION_VALUE_WORDS - first
            || count * 4u > length - at) return;
        memcpy(bytes + first * 4u, encoded + at, count * 4u);
        at += count * 4u;
    }
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
    size_t paint_bytes = 0;
    if (sheet->paint_storage != NULL) {
        paint_bytes = sizeof(*sheet->paint_storage)
            + sheet->paint_storage->capacity
                * sizeof(*sheet->paint_storage->blocks);
        for (size_t i = 0; i < sheet->paint_storage->capacity; i++) {
            if (sheet->paint_storage->blocks[i] != NULL) {
                paint_bytes += STYLE_PAINT_STACK_BLOCK_SIZE
                    * sizeof(*sheet->paint_storage->blocks[i]);
            }
        }
    }
    size_t grid_track_bytes = 0;
    if (sheet->grid_tracks != NULL) {
        grid_track_bytes = sizeof(*sheet->grid_tracks)
            + sheet->grid_tracks->capacity
                * sizeof(*sheet->grid_tracks->blocks);
        for (size_t i = 0; i < sheet->grid_tracks->capacity; i++) {
            if (sheet->grid_tracks->blocks[i] != NULL) {
                grid_track_bytes += STYLE_GRID_TRACK_BLOCK_SIZE
                    * sizeof(*sheet->grid_tracks->blocks[i]);
            }
        }
    }
    return sheet->capacity * sizeof(*sheet->rules)
        + sheet->focus_rule_count * sizeof(*sheet->focus_rule_indices)
        + sheet->has_rule_count * sizeof(*sheet->has_rule_indices)
        + sheet->has_custom_rule_count
            * sizeof(*sheet->has_custom_rule_indices)
        + sheet->has_class_hash_count * sizeof(*sheet->has_class_hashes)
        + style_has_plan_bytes(sheet)
        + sheet->selector_attribute_name_bytes
        + sheet->declaration_capacity * sizeof(*sheet->declarations)
        + sheet->declaration_value_capacity
        + sheet->revert_rule_mask_capacity
            * sizeof(*sheet->revert_rule_masks)
        + sheet->declaration_index_slot_count
            * sizeof(*sheet->declaration_index_slots)
        + sheet->selector_program_bytes
        + sheet->selector_storage_bytes
        + sheet->variable_capacity * sizeof(*sheet->variables)
        + sheet->custom_rule_capacity * sizeof(*sheet->custom_rules)
        + sheet->registered_property_capacity
            * sizeof(*sheet->registered_properties)
        + sheet->transition_rule_capacity * sizeof(*sheet->transition_rules)
        + sheet->generated_text_capacity * sizeof(*sheet->generated_texts)
        + sheet->counter_operation_set_capacity
            * sizeof(*sheet->counter_operation_sets)
        + sheet->image_url_capacity * sizeof(*sheet->image_urls)
        + sheet->math_instruction_capacity
            * sizeof(*sheet->math_instructions)
        + sheet->math_program_capacity * sizeof(*sheet->math_programs)
        + sheet->deferred_instruction_capacity
            * sizeof(*sheet->deferred_instructions)
        + sheet->custom_rule_index_bytes
        + (sheet->conditional_queries == NULL ? 0
           : sizeof(*sheet->conditional_queries))
        + (sheet->grid_areas == NULL ? 0 : sizeof(*sheet->grid_areas))
        + sheet->border_color_set_capacity
            * sizeof(*sheet->border_color_sets)
        + (sheet->resolve_scratch == NULL ? 0
           : sizeof(*sheet->resolve_scratch))
        + paint_bytes + grid_track_bytes
        + sheet->deferred_bytes + sheet->generated_text_bytes
        + sheet->image_url_bytes + sheet->rule_index_bytes
        + web_font_bytes + image_source_bytes;
}

void stylesheet_prepare_for_document_reuse(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    style_selector_cooperation_end(sheet);
    sheet->selector_cooperate = NULL;
    sheet->selector_cooperate_opaque = NULL;
    sheet->selector_cooperate_visits = 0;
    sheet->selector_cooperate_next = 0;
    sheet->selector_cooperate_quota = 0;
    sheet->selector_cooperate_cancelled = false;
    style_variable_cache_end(sheet);
    style_container_layout_state_clear(sheet);
    style_container_log_release(sheet);
    if (sheet->resolve_scratch != NULL) {
        memset(sheet->resolve_scratch, 0, sizeof(*sheet->resolve_scratch));
    }
    sheet->relative_selector_cache_depth = 0;
    sheet->relative_selector_cache_epoch = 0;
    memset(sheet->relative_selector_cache, 0,
           sizeof(sheet->relative_selector_cache));
    /* The source elements belonged to the retired document. */
    memset(sheet->style_source_nodes, 0, sizeof(sheet->style_source_nodes));
    sheet->style_source_count = 0;
    sheet->style_sources_bounded_out = true;
}

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
