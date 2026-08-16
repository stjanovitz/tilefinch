static const StylePropertyEntry style_property_table[] = {
    { "gap", 3, style_property_gap },
    { "top", 3, style_property_top },
    { "clip", 4, style_property_clip },
    { "flex", 4, style_property_flex },
    { "font", 4, style_property_font },
    { "grid", 4, style_property_grid_template },
    { "left", 4, style_property_left },
    { "mask", 4, style_property_mask },
    { "clear", 5, style_property_clear },
    { "color", 5, style_property_color },
    { "float", 5, style_property_float },
    { "inset", 5, style_property_inset },
    { "order", 5, style_property_order },
    { "right", 5, style_property_right },
    { "width", 5, style_property_width },
    { "border", 6, style_property_border },
    { "bottom", 6, style_property_bottom },
    { "filter", 6, style_property_fixed_containing_block },
    { "height", 6, style_property_height },
    { "margin", 6, style_property_margin },
    { "contain", 7, style_property_fixed_containing_block },
    { "content", 7, style_property_content },
    { "display", 7, style_property_display },
    { "opacity", 7, style_property_opacity },
    { "outline", 7, style_property_outline },
    { "padding", 7, style_property_padding },
    { "row-gap", 7, style_property_row_gap },
    { "z-index", 7, style_property_z_index },
    { "grid-row", 8, style_property_grid_row },
    { "overflow", 8, style_property_overflow },
    { "position", 8, style_property_position },
    { "clip-path", 9, style_property_clip_path },
    { "direction", 9, style_property_direction },
    { "flex-flow", 9, style_property_flex_flow },
    { "flex-grow", 9, style_property_flex_grow },
    { "flex-wrap", 9, style_property_flex_wrap },
    { "font-size", 9, style_property_font_size },
    { "grid-area", 9, style_property_grid_area },
    { "mask-size", 9, style_property_mask_size },
    { "max-width", 9, style_property_max_width },
    { "min-width", 9, style_property_min_width },
    { "transform", 9, style_property_transform },
    { "word-wrap", 9, style_property_overflow_wrap },
    { "align-self", 10, style_property_align_self },
    { "appearance", 10, style_property_appearance },
    { "background", 10, style_property_background },
    { "block-size", 10, style_property_block_size },
    { "border-top", 10, style_property_border_top },
    { "box-shadow", 10, style_property_box_shadow },
    { "box-sizing", 10, style_property_box_sizing },
    { "column-gap", 10, style_property_column_gap },
    { "flex-basis", 10, style_property_flex_basis },
    { "font-style", 10, style_property_font_style },
    { "list-style", 10, style_property_list_style },
    { "margin-top", 10, style_property_margin_top },
    { "mask-image", 10, style_property_mask_image },
    { "max-height", 10, style_property_max_height },
    { "min-height", 10, style_property_min_height },
    { "object-fit", 10, style_property_object_fit },
    { "overflow-x", 10, style_property_overflow },
    { "overflow-y", 10, style_property_overflow },
    { "place-self", 10, style_property_place_self },
    { "text-align", 10, style_property_text_align },
    { "visibility", 10, style_property_visibility },
    { "word-break", 10, style_property_word_break },
    { "align-items", 11, style_property_align_items },
    { "border-left", 11, style_property_border_left },
    { "counter-set", 11, style_property_counter_set },
    { "flex-shrink", 11, style_property_flex_shrink },
    { "font-family", 11, style_property_font_family },
    { "font-weight", 11, style_property_font_weight },
    { "grid-column", 11, style_property_grid_column },
    { "inline-size", 11, style_property_inline_size },
    { "inset-block", 11, style_property_inset_inline },
    { "line-height", 11, style_property_line_height },
    { "margin-left", 11, style_property_margin_left },
    { "mask-repeat", 11, style_property_mask_repeat },
    { "padding-top", 11, style_property_padding_top },
    { "perspective", 11, style_property_fixed_containing_block },
    { "place-items", 11, style_property_place_items },
    { "text-indent", 11, style_property_text_indent },
    { "text-shadow", 11, style_property_text_shadow },
    { "white-space", 11, style_property_white_space },
    { "will-change", 11, style_property_fixed_containing_block },
    { "-webkit-mask", 12, style_property_mask },
    { "aspect-ratio", 12, style_property_aspect_ratio },
    { "border-block", 12, style_property_border_inline },
    { "border-color", 12, style_property_border_color },
    { "border-right", 12, style_property_border_right },
    { "border-style", 12, style_property_border_style },
    { "border-width", 12, style_property_border_width },
    { "caption-side", 12, style_property_caption_side },
    { "grid-row-end", 12, style_property_grid_row_end },
    { "inset-inline", 12, style_property_inset_inline },
    { "justify-self", 12, style_property_justify_self },
    { "margin-block", 12, style_property_margin_block },
    { "margin-right", 12, style_property_margin_right },
    { "padding-left", 12, style_property_padding_left },
    { "table-layout", 12, style_property_table_layout },
    { "unicode-bidi", 12, style_property_unicode_bidi },
    { "word-spacing", 12, style_property_word_spacing },
    { "writing-mode", 12, style_property_writing_mode },
    { "align-content", 13, style_property_align_content },
    { "border-bottom", 13, style_property_border_bottom },
    { "border-inline", 13, style_property_border_inline },
    { "border-radius", 13, style_property_border_radius },
    { "counter-reset", 13, style_property_counter_reset },
    { "grid-template", 13, style_property_grid_template },
    { "justify-items", 13, style_property_justify_items },
    { "margin-bottom", 13, style_property_margin_bottom },
    { "margin-inline", 13, style_property_margin_inline },
    { "mask-position", 13, style_property_mask_position },
    { "outline-color", 13, style_property_outline_component },
    { "outline-style", 13, style_property_outline_component },
    { "outline-width", 13, style_property_outline_component },
    { "overflow-wrap", 13, style_property_overflow_wrap },
    { "padding-block", 13, style_property_padding_block },
    { "padding-right", 13, style_property_padding_right },
    { "place-content", 13, style_property_place_content },
    { "text-overflow", 13, style_property_text_overflow },
    { "border-spacing", 14, style_property_border_spacing },
    { "flex-direction", 14, style_property_flex_direction },
    { "grid-auto-flow", 14, style_property_grid_auto_flow },
    { "grid-auto-rows", 14, style_property_grid_auto_rows },
    { "grid-row-start", 14, style_property_grid_row_start },
    { "letter-spacing", 14, style_property_letter_spacing },
    { "max-block-size", 14, style_property_max_block_size },
    { "min-block-size", 14, style_property_min_block_size },
    { "outline-offset", 14, style_property_outline_component },
    { "padding-bottom", 14, style_property_padding_bottom },
    { "padding-inline", 14, style_property_padding_inline },
    { "pointer-events", 14, style_property_pointer_events },
    { "text-transform", 14, style_property_text_transform },
    { "vertical-align", 14, style_property_vertical_align },
    { "background-clip", 15, style_property_background_box },
    { "background-size", 15, style_property_background_size },
    { "border-collapse", 15, style_property_border_collapse },
    { "grid-column-end", 15, style_property_grid_column_end },
    { "inset-block-end", 15, style_property_inset_inline_start },
    { "justify-content", 15, style_property_justify_content },
    { "list-style-type", 15, style_property_list_style },
    { "max-inline-size", 15, style_property_max_inline_size },
    { "min-inline-size", 15, style_property_min_inline_size },
    { "object-position", 15, style_property_object_position },
    { "text-decoration", 15, style_property_text_decoration },
    { "background-color", 16, style_property_background },
    { "background-image", 16, style_property_background },
    { "border-block-end", 16, style_property_border_inline_start },
    { "border-top-color", 16, style_property_border_color },
    { "border-top-style", 16, style_property_border_style },
    { "border-top-width", 16, style_property_border_top_width },
    { "inset-inline-end", 16, style_property_inset_inline_start },
    { "margin-block-end", 16, style_property_margin_block_end },
    { "scrollbar-gutter", 16, style_property_scrollbar_gutter },
    { "transform-origin", 16, style_property_transform_origin },
    { "background-origin", 17, style_property_background_box },
    { "background-repeat", 17, style_property_background_repeat },
    { "border-inline-end", 17, style_property_border_inline_start },
    { "border-left-color", 17, style_property_border_color },
    { "border-left-style", 17, style_property_border_style },
    { "border-left-width", 17, style_property_border_top_width },
    { "counter-increment", 17, style_property_counter_increment },
    { "grid-auto-columns", 17, style_property_grid_auto_columns },
    { "grid-column-start", 17, style_property_grid_column_start },
    { "inset-block-start", 17, style_property_inset_inline_start },
    { "margin-inline-end", 17, style_property_margin_inline_end },
    { "padding-block-end", 17, style_property_padding_block_end },
    { "-webkit-appearance", 18, style_property_appearance },
    { "-webkit-line-clamp", 18, style_property_webkit_line_clamp },
    { "-webkit-mask-image", 18, style_property_mask_image },
    { "border-block-color", 18, style_property_border_color },
    { "border-block-start", 18, style_property_border_inline_start },
    { "border-block-width", 18, style_property_border_block_width },
    { "border-right-color", 18, style_property_border_color },
    { "border-right-style", 18, style_property_border_style },
    { "border-right-width", 18, style_property_border_top_width },
    { "content-visibility", 18, style_property_content_visibility },
    { "grid-template-rows", 18, style_property_grid_template_rows },
    { "inset-inline-start", 18, style_property_inset_inline_start },
    { "margin-block-start", 18, style_property_margin_block_start },
    { "padding-inline-end", 18, style_property_padding_inline_end },
    { "background-position", 19, style_property_background_position },
    { "border-bottom-color", 19, style_property_border_color },
    { "border-bottom-style", 19, style_property_border_style },
    { "border-bottom-width", 19, style_property_border_top_width },
    { "border-inline-color", 19, style_property_border_color },
    { "border-inline-start", 19, style_property_border_inline_start },
    { "border-inline-width", 19, style_property_border_inline_width },
    { "grid-template-areas", 19, style_property_grid_template_areas },
    { "list-style-position", 19, style_property_list_style },
    { "margin-inline-start", 19, style_property_margin_inline_start },
    { "padding-block-start", 19, style_property_padding_block_start },
    { "overflow-clip-margin", 20, style_property_overflow_clip_margin },
    { "padding-inline-start", 20, style_property_padding_inline_start },
    { "text-decoration-line", 20, style_property_text_decoration },
    { "grid-template-columns", 21, style_property_grid_template_columns },
    { "text-underline-offset", 21, style_property_text_underline_offset },
    { "border-block-end-color", 22, style_property_border_color },
    { "border-block-end-width", 22, style_property_border_block_end_width },
    { "border-top-left-radius", 22, style_property_border_corner_radius },
    { "border-inline-end-color", 23, style_property_border_color },
    { "border-inline-end-width", 23, style_property_border_inline_end_width },
    { "border-top-right-radius", 23, style_property_border_corner_radius },
    { "border-block-start-color", 24, style_property_border_color },
    { "border-block-start-width", 24, style_property_border_block_start_width },
    { "border-bottom-left-radius", 25, style_property_border_corner_radius },
    { "border-inline-start-color", 25, style_property_border_color },
    { "border-inline-start-width", 25, style_property_border_inline_start_width },
    { "border-bottom-right-radius", 26, style_property_border_corner_radius },
};

static const StylePropertyEntry *style_property_lookup(const char *name,
                                                       size_t name_length)
{
    char normalized[64];
    if (name_length >= sizeof(normalized)) return NULL;
    size_t written = 0;
    for (size_t at = 0; at < name_length;) {
        if (at + 1 < name_length && name[at] == '/'
            && name[at + 1] == '*') {
            at += 2;
            while (at + 1 < name_length
                   && !(name[at] == '*' && name[at + 1] == '/')) at++;
            if (at + 1 >= name_length) return NULL;
            at += 2;
            if (written == 0 || normalized[written - 1] != ' ') {
                normalized[written++] = ' ';
            }
            continue;
        }
        normalized[written++] =
            (char) tolower((unsigned char) name[at++]);
    }
    while (written > 0 && normalized[written - 1] == ' ') written--;
    name = normalized;
    name_length = written;
    size_t low = 0;
    size_t high = sizeof(style_property_table)
                  / sizeof(style_property_table[0]);
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const StylePropertyEntry *entry = &style_property_table[mid];
        int order = name_length < entry->name_length ? -1
                    : (name_length > entry->name_length ? 1
                       : memcmp(name, entry->name, name_length));
        if (order == 0) return entry;
        if (order < 0) high = mid;
        else low = mid + 1;
    }
    return NULL;
}

bool style_property_name_is_logical(const char *name, size_t length)
{
    #define LOGICAL_EDGE(prefix) \
        (span_case_equal(name, length, prefix) \
         || (length == sizeof(prefix "-start") - 1u \
             && strncasecmp(name, prefix "-start", length) == 0) \
         || (length == sizeof(prefix "-end") - 1u \
             && strncasecmp(name, prefix "-end", length) == 0))
    return span_case_equal(name, length, "inline-size")
        || span_case_equal(name, length, "block-size")
        || span_case_equal(name, length, "min-inline-size")
        || span_case_equal(name, length, "max-inline-size")
        || span_case_equal(name, length, "min-block-size")
        || span_case_equal(name, length, "max-block-size")
        || LOGICAL_EDGE("margin-inline")
        || LOGICAL_EDGE("padding-inline")
        || LOGICAL_EDGE("margin-block")
        || LOGICAL_EDGE("padding-block")
        || LOGICAL_EDGE("inset-inline")
        || LOGICAL_EDGE("inset-block");
    #undef LOGICAL_EDGE
}

static bool style_deferred_font_math(const char *text, size_t length)
{
    static const char *const names[] = {"calc(", "min(", "max(", "clamp("};
    for (size_t at = 0; at < length; at++) {
        for (size_t name = 0; name < sizeof(names) / sizeof(names[0]);
             name++) {
            size_t name_length = strlen(names[name]);
            if (at + name_length > length) continue;
            bool equal = true;
            for (size_t i = 0; i < name_length; i++) {
                if (tolower((unsigned char) text[at + i])
                    != (unsigned char) names[name][i]) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }
    }
    return false;
}

typedef struct {
    size_t declaration_count;
    bool has_variable;
    bool has_logical;
} StyleDeferredBlockInfo;

static bool style_deferred_block_info(const char *text, size_t length,
                                      StyleDeferredBlockInfo *info)
{
    if (text == NULL || info == NULL) return false;
    for (size_t i = 0; i + 4 <= length; i++) {
        if (memcmp(text + i, "var(", 4) == 0) {
            info->has_variable = true;
            break;
        }
    }
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            trim(&name, &name_length);
            info->has_logical = info->has_logical
                || style_property_name_is_logical(name, name_length);
            if (info->declaration_count == SIZE_MAX) return false;
            info->declaration_count++;
        }
        at = end + (end < length);
    }
    return true;
}

static bool style_deferred_program_scan(
    const char *text, size_t length, bool block_has_variable,
    bool block_has_logical, StyleDeferredInstruction *instructions,
    size_t instruction_capacity, size_t *instruction_count)
{
    if (text == NULL || instruction_count == NULL) return false;
    size_t count = 0;
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        if (text[at] == '}') {
            at++;
            continue;
        }
        if (text[at] == '{') {
            int depth = 1;
            for (at++; at < length && depth > 0; at++) {
                if (text[at] == '{') depth++;
                else if (text[at] == '}') depth--;
            }
            continue;
        }
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1u;
            trim(&name, &name_length);
            trim(&value, &value_length);
            const StylePropertyEntry *entry = style_property_lookup(
                name, name_length);
            /* var() fallback is declaration-block scoped in the compact
               parser: replay the whole block so an invalid later value can
               retain an earlier declaration of the same property. Pure
               em/rem deferral can replay only its affected properties. */
            bool needs_runtime_basis =
                block_has_variable || block_has_logical;
            for (size_t i = 0; i < value_length; i++) {
                if (i + 4 <= value_length
                    && memcmp(value + i, "var(", 4) == 0) {
                    needs_runtime_basis = true;
                    break;
                }
                size_t unit_length = 0;
                if (i + 3 <= value_length
                    && tolower((unsigned char) value[i]) == 'r'
                    && tolower((unsigned char) value[i + 1]) == 'e'
                    && tolower((unsigned char) value[i + 2]) == 'm') {
                    unit_length = 3;
                } else if (i + 2 <= value_length
                           && tolower((unsigned char) value[i]) == 'e'
                           && tolower((unsigned char) value[i + 1]) == 'm') {
                    unit_length = 2;
                } else if (i + 3 <= value_length
                           && tolower((unsigned char) value[i]) == 'c'
                           && tolower((unsigned char) value[i + 1]) == 'q'
                           && (tolower((unsigned char) value[i + 2]) == 'w'
                               || tolower((unsigned char) value[i + 2]) == 'h'
                               || tolower((unsigned char) value[i + 2]) == 'i'
                               || tolower((unsigned char) value[i + 2])
                                  == 'b')) {
                    unit_length = 3;
                } else if (i + 5 <= value_length
                           && strncasecmp(value + i, "cqmin", 5) == 0) {
                    unit_length = 5;
                } else if (i + 5 <= value_length
                           && strncasecmp(value + i, "cqmax", 5) == 0) {
                    unit_length = 5;
                }
                if (unit_length == 0 || i == 0) continue;
                unsigned char before = (unsigned char) value[i - 1];
                unsigned char after = i + unit_length < value_length
                    ? (unsigned char) value[i + unit_length] : 0;
                if ((isdigit(before) || before == '.' || before == ')')
                    && !(isalnum(after) || after == '-' || after == '_'
                         || after >= 0x80u)) {
                    needs_runtime_basis = true;
                    break;
                }
            }
            if (!needs_runtime_basis
                && (span_case_equal(name, name_length, "font-size")
                    || span_case_equal(name, name_length, "font"))) {
                needs_runtime_basis =
                    style_deferred_font_math(value, value_length);
            }
            if (entry != NULL && needs_runtime_basis) {
                size_t value_offset = (size_t) (value - text);
                size_t property_index =
                    (size_t) (entry - style_property_table);
                if (value_offset > UINT32_MAX || value_length > UINT16_MAX
                    || property_index > UINT8_MAX || count >= UINT16_MAX) {
                    return false;
                }
                if (instructions != NULL) {
                    if (count >= instruction_capacity) return false;
                    instructions[count] = (StyleDeferredInstruction) {
                        .value_offset = (uint32_t) value_offset,
                        .value_length = (uint16_t) value_length,
                        .property_index = (uint8_t) property_index,
                        .important =
                            declaration_is_important(value, value_length)
                                ? 1u : 0u
                    };
                }
                count++;
            }
        }
        at = end + (end < length);
    }
    *instruction_count = count;
    return true;
}

bool style_compile_deferred_program(Stylesheet *sheet, const char *text,
                                    size_t length, uint32_t *program_offset,
                                    uint16_t *program_count)
{
    if (sheet == NULL || text == NULL || program_offset == NULL
        || program_count == NULL) return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_COMPILED_DEFERRED") != NULL) return false;
#endif
    _Static_assert(
        sizeof(style_property_table) / sizeof(style_property_table[0])
            <= UINT8_MAX + 1u,
        "deferred property indices must fit in one byte");
    StyleDeferredBlockInfo info = {0};
    if (!style_deferred_block_info(text, length, &info)
        || info.declaration_count > UINT16_MAX
        || sheet->deferred_instruction_count > UINT32_MAX
        || info.declaration_count
            > UINT32_MAX - sheet->deferred_instruction_count) {
        return false;
    }
    size_t offset = sheet->deferred_instruction_count;
    size_t maximum_required = offset + info.declaration_count;
    if (maximum_required > sheet->deferred_instruction_capacity) {
        size_t capacity =
            sheet->deferred_instruction_capacity == 0
                ? 64u : sheet->deferred_instruction_capacity;
        while (capacity < maximum_required) {
            if (capacity > SIZE_MAX / 2u) return false;
            capacity *= 2u;
        }
        if (capacity
            > SIZE_MAX / sizeof(*sheet->deferred_instructions)) {
            return false;
        }
        StyleDeferredInstruction *instructions = budget_realloc(
            sheet->budget, sheet->deferred_instructions,
            capacity * sizeof(*instructions));
        if (instructions == NULL) return false;
        sheet->deferred_instructions = instructions;
        sheet->deferred_instruction_capacity = capacity;
    }
    size_t emitted = 0;
    if (!style_deferred_program_scan(
            text, length, info.has_variable, info.has_logical,
            info.declaration_count == 0
                ? NULL : sheet->deferred_instructions + offset,
            info.declaration_count, &emitted)
        || emitted > info.declaration_count) {
        return false;
    }
    size_t required = offset + emitted;
    sheet->deferred_instruction_count = required;
    sheet->deferred_program_bytes =
        required * sizeof(*sheet->deferred_instructions);
    *program_offset = (uint32_t) offset;
    *program_count = (uint16_t) emitted;
    return true;
}

static bool span_has_em_unit(const char *text, size_t length)
{
    for (size_t i = 0; i + 2 <= length; i++) {
        if (tolower((unsigned char) text[i]) == 'e'
            && tolower((unsigned char) text[i + 1]) == 'm') return true;
    }
    return false;
}

static bool declaration_absolute_font_basis(
    Stylesheet *sheet, const char *text, size_t length,
    int important_filter, int *basis);

static bool style_parse_property_declaration(
    const StylePropertyEntry *entry, StylePropertyParse *parse,
    uint64_t *revert_rule_mask, uint64_t *revert_rule_mask_high)
{
    if (entry == NULL || parse == NULL) return false;
    bool revert_rule = span_case_equal(
        parse->value, parse->value_length, "revert-rule");
    if (!revert_rule) {
        if (!entry->handler(parse)) return false;
        if (revert_rule_mask != NULL) {
            *revert_rule_mask &=
                ~(*parse->mask | *parse->inherit_mask);
        }
        if (revert_rule_mask_high != NULL) {
            *revert_rule_mask_high &= ~*parse->mask_high;
        }
        return true;
    }

    /* `revert-rule` removes this property's declarations from the complete
       qualified rule, rather than supplying a computed value. Ask the
       ordinary property handler which longhands the declaration controls by
       parsing `initial` into scratch state; this keeps shorthand expansion
       centralized and makes the sparse rollback masks property-agnostic. */
    static const char initial[] = "initial";
    ComputedStyle scratch = *parse->style;
    uint64_t affected = 0;
    uint64_t affected_high = 0;
    uint64_t affected_inherit = 0;
    StylePropertyParse probe = *parse;
    probe.value = initial;
    probe.value_length = sizeof(initial) - 1u;
    probe.style = &scratch;
    probe.mask = &affected;
    probe.mask_high = &affected_high;
    probe.inherit_mask = &affected_inherit;
    if (!entry->handler(&probe)) return false;

    *parse->mask &= ~affected;
    *parse->mask_high &= ~affected_high;
    *parse->inherit_mask &= ~affected;
    if (revert_rule_mask != NULL) {
        *revert_rule_mask |= affected | affected_inherit;
    }
    if (revert_rule_mask_high != NULL) {
        *revert_rule_mask_high |= affected_high;
    }
    return true;
}

bool style_apply_deferred_program(
    Stylesheet *sheet, const char *text, size_t length,
    uint32_t program_offset, uint16_t program_count, bool important,
    ComputedStyle *style, uint64_t *mask, uint64_t *mask_high,
    uint64_t *inherit_mask)
{
    if (sheet == NULL || text == NULL || style == NULL || mask == NULL
        || mask_high == NULL || inherit_mask == NULL
        || program_offset > sheet->deferred_instruction_count
        || program_count
            > sheet->deferred_instruction_count - program_offset) {
        return false;
    }
    StyleResolveScratch saved_scratch = *sheet->resolve_scratch;
    int font_basis = STYLE_DEFAULT_FONT_PX;
    if (declaration_absolute_font_basis(
            sheet, text, length, important ? 1 : 0, &font_basis)) {
        sheet->resolve_scratch->font_resolution_active = true;
        sheet->resolve_scratch->font_resolution_inherited = font_basis;
    }
    for (size_t i = 0; i < program_count; i++) {
        const StyleDeferredInstruction *instruction =
            &sheet->deferred_instructions[program_offset + i];
        if ((instruction->important != 0) != important) continue;
        if (instruction->property_index
                >= sizeof(style_property_table)
                    / sizeof(style_property_table[0])
            || instruction->value_offset > length
            || instruction->value_length
                > length - instruction->value_offset) {
            *sheet->resolve_scratch = saved_scratch;
            return false;
        }
        const StylePropertyEntry *entry =
            &style_property_table[instruction->property_index];
        const char *value = text + instruction->value_offset;
        size_t value_length = instruction->value_length;
        /* Image-bearing shorthands can contain a valid near-capacity URL
           followed by position/size/repeat tokens. Keep this transient
           expansion bounded, but do not force those declarations to their
           initial value merely because they exceed the math parser's smaller
           source buffer. Nothing from this array is retained. */
        char resolved_value[512];
        const char initial_value[] = "initial";
        /* An unresolved var() makes the declaration invalid at computed-value
           time.  It has already won the cascade, so the affected longhands
           take their initial values instead of exposing an older declaration.
           Route that reset through the ordinary property handler so shorthand
           expansion stays centralized. */
        if (!style_resolve_value(
                sheet, value, value_length, resolved_value,
                sizeof(resolved_value), 0)) {
            value = initial_value;
            value_length = sizeof(initial_value) - 1u;
        }
        StylePropertyParse parse = {
            .sheet = sheet,
            .name = entry->name,
            .name_length = entry->name_length,
            .value = value,
            .value_length = value_length,
            .style = style,
            .mask = mask,
            .mask_high = mask_high,
            .inherit_mask = inherit_mask,
            .important = important
        };
        int saved_font_basis =
            sheet->resolve_scratch->font_resolution_inherited;
        bool font_sized_property =
            span_case_equal(
                entry->name, entry->name_length, "font-size")
            || span_case_equal(
                entry->name, entry->name_length, "font");
        /*
         * A corrected cascade locks non-font em lengths to this element's
         * final computed font. A relative font-size itself remains relative
         * to the parent, however. Switch basis only around that instruction;
         * switching it for the entire deferred declaration block made an
         * earlier letter-spacing:1em depend on source order when a later
         * font shorthand changed the element font.
         */
        if (font_sized_property
            && sheet->resolve_scratch->font_resolution_locked) {
            sheet->resolve_scratch->font_resolution_inherited =
                sheet->resolve_scratch->font_resolution_parent;
        }
        if (!entry->handler(&parse)) {
            *sheet->resolve_scratch = saved_scratch;
            return false;
        }
        sheet->resolve_scratch->font_resolution_inherited = saved_font_basis;
    }
    bool font_ch_pending = sheet->resolve_scratch->font_ch_pending;
    *sheet->resolve_scratch = saved_scratch;
    sheet->resolve_scratch->font_ch_pending =
        sheet->resolve_scratch->font_ch_pending || font_ch_pending;
    return true;
}

/* Non-font em lengths use the element's computed font size, including when
   font-size appears later in the same declaration block.  Rules are compiled
   once, so discover an absolute winning font-size before parsing the other
   properties rather than making source order observable.  Relative
   font-sizes remain deferred to normal computed-value resolution. */
static bool declaration_absolute_font_basis(
    Stylesheet *sheet, const char *text, size_t length,
    int important_filter, int *basis)
{
    if (basis == NULL) return false;
    bool found = false;
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(
                    value, value_length);
                trim(&value, &value_length);
            }
            if ((important_filter < 0
                 || important == (important_filter != 0))
                && name_length == sizeof("font-size") - 1
                && memcmp(name, "font-size", name_length) == 0
                && memchr(value, '(', value_length) == NULL
                && memchr(value, '%', value_length) == NULL
                && !span_has_em_unit(value, value_length)) {
                uint8_t unit = FONT_SIZE_UNIT_ABSOLUTE;
                uint8_t fraction = 0;
                int pixels = style_parse_font_size(
                    sheet, value, value_length, &unit, &fraction);
                if (unit == FONT_SIZE_UNIT_ABSOLUTE) {
                    int fixed = pixels * 64 + fraction;
                    if (fixed < STYLE_FONT_MIN_PX * 64) {
                        fixed = STYLE_FONT_MIN_PX * 64;
                    }
                    if (fixed > STYLE_FONT_MAX_PX * 64) {
                        fixed = STYLE_FONT_MAX_PX * 64;
                    }
                    *basis = (fixed + 32) / 64;
                    found = true;
                }
            }
        }
        at = end + (end < length);
    }
    return found;
}

/* Single-pass variant for inline style attributes: lexes the declaration
   block once and routes each declaration to the normal or !important
   accumulator, so style_for_node no longer scans the attribute twice.
   Application order at the two cascade points is unchanged. */
bool style_parse_declarations_split(Stylesheet *sheet, const char *text,
                                    size_t length,
                                    ComputedStyle *normal_style,
                                    uint64_t *normal_mask,
                                    uint64_t *normal_mask_high,
                                    uint64_t *normal_inherit_mask,
                                    ComputedStyle *important_style,
                                    uint64_t *important_mask,
                                    uint64_t *important_mask_high,
                                    uint64_t *important_inherit_mask,
                                    uint64_t *normal_revert_rule_mask,
                                    uint64_t *normal_revert_rule_mask_high,
                                    uint64_t *important_revert_rule_mask,
                                    uint64_t *important_revert_rule_mask_high)
{
    StyleResolveScratch saved_scratch = {0};
    bool restore_scratch = sheet != NULL && sheet->resolve_scratch != NULL;
    int normal_basis = STYLE_DEFAULT_FONT_PX;
    int important_basis = STYLE_DEFAULT_FONT_PX;
    bool have_normal_basis = declaration_absolute_font_basis(
        sheet, text, length, 0, &normal_basis);
    bool have_important_basis = declaration_absolute_font_basis(
        sheet, text, length, 1, &important_basis);
    if (restore_scratch) saved_scratch = *sheet->resolve_scratch;
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        if (text[at] == '}') {
            at++;
            continue;
        }
        if (text[at] == '{') {
            int depth = 1;
            for (at++; at < length && depth > 0; at++) {
                if (text[at] == '{') depth++;
                else if (text[at] == '}') depth--;
            }
            continue;
        }
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(
                    value, value_length);
                trim(&value, &value_length);
            }
            const StylePropertyEntry *entry = style_property_lookup(
                name, name_length);
            if (entry != NULL) {
                if (restore_scratch
                    && (important ? have_important_basis
                                  : have_normal_basis)) {
                    sheet->resolve_scratch->font_resolution_active = true;
                    sheet->resolve_scratch->font_resolution_inherited =
                        important ? important_basis : normal_basis;
                }
                StylePropertyParse parse = {
                    .sheet = sheet,
                    .name = entry->name,
                    .name_length = entry->name_length,
                    .value = value,
                    .value_length = value_length,
                    .style = important ? important_style : normal_style,
                    .mask = important ? important_mask : normal_mask,
                    .mask_high = important ? important_mask_high
                                           : normal_mask_high,
                    .inherit_mask = important ? important_inherit_mask
                                              : normal_inherit_mask,
                    .important = important,
                };
                if (!style_parse_property_declaration(
                        entry, &parse,
                        important ? important_revert_rule_mask
                                  : normal_revert_rule_mask,
                        important ? important_revert_rule_mask_high
                                  : normal_revert_rule_mask_high)) {
                    if (restore_scratch) {
                        *sheet->resolve_scratch = saved_scratch;
                    }
                    return false;
                }
            }
        }
        at = end + (end < length);
    }
    if (restore_scratch) *sheet->resolve_scratch = saved_scratch;
    return true;
}

bool style_parse_declarations(Stylesheet *sheet, const char *text,
                               size_t length, ComputedStyle *style,
                               uint64_t *mask, uint64_t *mask_high,
                               uint64_t *inherit_mask,
                               bool collect_variables,
                               int important_filter,
                               uint64_t *revert_rule_mask,
                               uint64_t *revert_rule_mask_high)
{
    uint64_t local_inherit = 0;
    if (inherit_mask == NULL) inherit_mask = &local_inherit;
    StyleResolveScratch saved_scratch = {0};
    bool restore_scratch = sheet != NULL && sheet->resolve_scratch != NULL;
    int font_basis = STYLE_DEFAULT_FONT_PX;
    bool have_font_basis = declaration_absolute_font_basis(
        sheet, text, length, important_filter, &font_basis);
    if (restore_scratch) {
        saved_scratch = *sheet->resolve_scratch;
        if (have_font_basis) {
            sheet->resolve_scratch->font_resolution_active = true;
            sheet->resolve_scratch->font_resolution_inherited = font_basis;
        }
    }
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        if (text[at] == '}') {
            at++;
            continue;
        }
        if (text[at] == '{') {
            int depth = 1;
            for (at++; at < length && depth > 0; at++) {
                if (text[at] == '{') depth++;
                else if (text[at] == '}') depth--;
            }
            continue;
        }
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(
                    value, value_length);
                trim(&value, &value_length);
            }
            if (important_filter >= 0
                && important != (important_filter != 0)) {
                at = end + (end < length);
                continue;
            }
            if (collect_variables && name_length > 2 && name[0] == '-' && name[1] == '-') {
                if (!style_set_variable(
                        sheet, name, name_length, value, value_length)) {
                    if (restore_scratch) {
                        *sheet->resolve_scratch = saved_scratch;
                    }
                    return false;
                }
            } else {
                const StylePropertyEntry *entry = style_property_lookup(
                    name, name_length);
                if (entry != NULL) {
                    StylePropertyParse parse = {
                        .sheet = sheet,
                        .name = entry->name,
                        .name_length = entry->name_length,
                        .value = value,
                        .value_length = value_length,
                        .style = style,
                        .mask = mask,
                        .mask_high = mask_high,
                        .inherit_mask = inherit_mask,
                        .important = important,
                    };
                    if (!style_parse_property_declaration(
                            entry, &parse, revert_rule_mask,
                            revert_rule_mask_high)) {
                        if (restore_scratch) {
                            *sheet->resolve_scratch = saved_scratch;
                        }
                        return false;
                    }
                }
            }
        }
        at = end + (end < length);
    }
    if (restore_scratch) *sheet->resolve_scratch = saved_scratch;
    return true;
}
