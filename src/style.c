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

static const char *style_head_selector(const Stylesheet *sheet, size_t index)
{
    return index < sheet->count ? sheet->rules[index].selector
        : sheet->custom_rules[index - sheet->count].selector;
}

bool stylesheet_head_scripts_affect_ancestors(
    const Stylesheet *sheet, lxb_dom_node_t *head)
{
    if (sheet == NULL || head == NULL || head->parent == NULL
        || sheet->resolve_scratch == NULL) return true;
    StyleResolveScratch *cache = sheet->resolve_scratch;
    if (cache->head_script_summary == 0
        || cache->head_script_generation != sheet->build_generation) {
        cache->head_script_generation = sheet->build_generation;
        cache->head_script_subject_count = 0;
        cache->head_script_summary = 2; /* Any refusal stays conservative. */
        size_t count = sheet->count + sheet->custom_rule_count;
        for (size_t i = 0; i < count; i++) {
            const char *selector = style_head_selector(sheet, i);
            if (selector == NULL) continue;
#ifndef TILEFINCH_NO_TRACE
            ((Stylesheet *) sheet)->head_script_selector_scans++;
#endif
            size_t length = strlen(selector);
            if (strchr(selector, '\\') != NULL) return true;
            unsigned depth = 0;
            char quote = 0;
            for (size_t at = 0; at < length; at++) {
                char c = selector[at];
                if (quote != 0) { if (c == quote) quote = 0; continue; }
                if (c == '\'' || c == '"') { quote = c; continue; }
                if (c == '(' || c == '[') depth++;
                else if (c == ')' || c == ']') { if (depth != 0) depth--; }
                else if (c == ',' && depth == 0) return true;
            }
            size_t has_at = SIZE_MAX;
            bool complex_prefix = false;
            for (size_t at = 0; at < length; at++) {
                if (has_at == SIZE_MAX
                    && (selector[at] == '(' || selector[at] == ','))
                    complex_prefix = true;
                if (selector[at] != ':') continue;
                size_t remaining = length - at;
                if (remaining >= 4 && strncasecmp(selector + at, ":dir", 4) == 0)
                    return true;
                if (remaining >= 5 && strncasecmp(selector + at, ":has(", 5) == 0) {
                    if (complex_prefix || at == 0
                        || isspace((unsigned char) selector[at - 1])
                        || strchr(">+~|", selector[at - 1]) != NULL) return true;
                    has_at = at;
                    break;
                }
            }
            if (has_at != SIZE_MAX) {
                if (cache->head_script_subject_count == 16u) return true;
                unsigned slot = cache->head_script_subject_count++;
                cache->head_script_subjects[slot].selector_index = (uint32_t) i;
                cache->head_script_subjects[slot].prefix_length = (uint32_t) has_at;
            }
        }
        cache->head_script_summary = 1;
    }
    if (cache->head_script_summary != 1) return true;
    for (unsigned i = 0; i < cache->head_script_subject_count; i++) {
        const char *selector = style_head_selector(
            sheet, cache->head_script_subjects[i].selector_index);
        size_t length = cache->head_script_subjects[i].prefix_length;
        if (style_selector_matches(head, selector, length)
            || style_selector_matches(head->parent, selector, length)) return true;
    }
    return false;
}

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
        + sheet->declaration_capacity * sizeof(*sheet->declarations)
        + sheet->revert_rule_mask_capacity
            * sizeof(*sheet->revert_rule_masks)
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
    if (sheet->resolve_scratch != NULL) {
        memset(sheet->resolve_scratch, 0, sizeof(*sheet->resolve_scratch));
    }
    sheet->relative_selector_cache_depth = 0;
    sheet->relative_selector_cache_epoch = 0;
    memset(sheet->relative_selector_cache, 0,
           sizeof(sheet->relative_selector_cache));
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
