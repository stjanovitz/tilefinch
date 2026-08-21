static bool style_property_font(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "inherit")) {
        /* CSS-wide keyword on the shorthand: every font sub-property
           inherits.  Recorded as inherit bits and resolved against the
           parent's computed style at application time (the classic
           h1 { font: inherit } reset). */
        *parse->inherit_mask |= S_FONT_SCALE | S_FONT_FAMILY
                                | S_FONT_WEIGHT | S_FONT_STYLE
                                | S_LINE_HEIGHT;
        *parse->mask |= S_FONT_SCALE | S_FONT_FAMILY | S_FONT_WEIGHT
                        | S_FONT_STYLE | S_LINE_HEIGHT;
        return true;
    }
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            ComputedStyle font = {0};
        if (style_parse_font_shorthand(sheet, value, value_length, &font)) {
            style->font_scale = font.font_scale;
            style->font_size = font.font_size;
            style->font_size_fraction = font.font_size_fraction;
            style->font_size_unit = font.font_size_unit;
            style->font_family = font.font_family;
            style->font_weight = font.font_weight;
            style->font_bold = font.font_bold;
            style->font_italic = font.font_italic;
            style->line_height = font.line_height;
            *mask |= S_FONT_SCALE | S_FONT_FAMILY | S_FONT_WEIGHT
                     | S_FONT_STYLE | S_LINE_HEIGHT;
        }
    
    return true;
}

static bool style_property_font_size(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "inherit")
        || span_case_equal(parse->value, parse->value_length, "unset")) {
        /* CSS-wide keyword on the longhand (for example,
           .heading h2 { font-size: inherit } as a rhythm reset); font-size
           is inherited, so unset behaves the same way. */
        *parse->inherit_mask |= S_FONT_SCALE;
        *parse->mask |= S_FONT_SCALE;
        return true;
    }
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        /* initial resets to the CSS initial size.  revert targets the UA
           style, which this engine folds into element defaults before
           author rules apply, so the initial size is the closest honest
           stand-in -- better than falling through to the unknown-value
           14px path. */
        parse->style->font_size = STYLE_DEFAULT_FONT_PX;
        parse->style->font_size_fraction = 0;
        parse->style->font_size_unit = FONT_SIZE_UNIT_ABSOLUTE;
        parse->style->font_scale = 2;
        *parse->mask |= S_FONT_SCALE;
        return true;
    }
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int pixels = style_parse_font_size(
            sheet, value, value_length, &style->font_size_unit,
            &style->font_size_fraction);
        if (style->font_size_unit == FONT_SIZE_UNIT_ABSOLUTE) {
            int fixed = pixels * 64 + style->font_size_fraction;
            if (fixed < 6 * 64) fixed = 6 * 64;
            if (fixed > STYLE_FONT_MAX_PX * 64) {
                fixed = STYLE_FONT_MAX_PX * 64;
            }
            pixels = style_font_size_from_fixed(
                fixed, &style->font_size_fraction);
        }
        style->font_size = pixels;
        int fixed = computed_style_font_size_fixed(style);
        style->font_scale = fixed >= 20 * 64 ? 3
                            : (fixed >= 13 * 64 ? 2 : 1);
        *mask |= S_FONT_SCALE;
    
    return true;
}

static bool style_property_text_indent(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
    StyleLength indent = 0;
    if (parse_style_length(sheet, value, value_length, &indent, NULL)) {
        style->text_indent = indent;
        *mask_high |= S2_TEXT_INDENT;
    }
    
    return true;
}

static bool style_property_font_family(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[160];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            for (size_t i = 0; resolved[i] != '\0'; i++) {
                resolved[i] = (char) tolower((unsigned char) resolved[i]);
            }
            FontFamily family = FONT_SANS;
            if (style_font_family_component(sheet, resolved,
                                      strlen(resolved),
                                      &family)) {
                style->font_family = family;
                *mask |= S_FONT_FAMILY;
            }
        }
    
    return true;
}

static bool style_property_word_spacing(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->word_spacing = strstr(resolved, "normal") != NULL
                                  ? 0 : style_parse_length(
                                      sheet, resolved,
                                      strlen(resolved), 0, NULL);
            *mask |= S_WORD_SPACING;
        }
    
    return true;
}

static bool style_property_letter_spacing(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            int spacing = strstr(resolved, "normal") != NULL
                          ? 0 : style_parse_length(
                              sheet, resolved, strlen(resolved),
                              0, NULL);
            if (spacing < -128) spacing = -128;
            if (spacing > 127) spacing = 127;
            style->letter_spacing = (int8_t) spacing;
            *mask |= S_LETTER_SPACING;
        }
    
    return true;
}

static bool style_property_font_weight(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            uint16_t weight = 400;
            const char *weight_text = resolved;
            size_t weight_length = strlen(resolved);
            trim(&weight_text, &weight_length);
            if (style_parse_font_weight_component(
                    weight_text, weight_length, &weight)) {
                style->font_weight = weight;
                style->font_bold = weight >= 600;
                *mask |= S_FONT_WEIGHT;
            }
        }
    
    return true;
}

static bool style_property_font_style(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->font_italic = strstr(resolved, "italic") != NULL
                                 || strstr(resolved, "oblique") != NULL;
            *mask |= S_FONT_STYLE;
        }
    
    return true;
}

static bool style_property_vertical_align(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            if (strcmp(resolved, "super") == 0) {
                style->vertical_align = VERTICAL_SUPER;
            } else if (strcmp(resolved, "sub") == 0) {
                style->vertical_align = VERTICAL_SUB;
            } else if (strcmp(resolved, "middle") == 0) {
                style->vertical_align = VERTICAL_MIDDLE;
            } else if (strcmp(resolved, "top") == 0) {
                style->vertical_align = VERTICAL_TOP;
            } else if (strcmp(resolved, "bottom") == 0) {
                style->vertical_align = VERTICAL_BOTTOM;
            } else {
                style->vertical_align = VERTICAL_BASELINE;
            }
            *mask |= S_VERTICAL_ALIGN;
        }
    
    return true;
}

static bool style_property_line_height(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "inherit")
        || span_case_equal(parse->value, parse->value_length, "unset")) {
        *parse->inherit_mask |= S_LINE_HEIGHT;
        *parse->mask |= S_LINE_HEIGHT;
        return true;
    }
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        /* The initial line-height is `normal`, represented by zero until
           layout resolves it against the selected face. */
        parse->style->line_height = 0;
        *parse->mask |= S_LINE_HEIGHT;
        return true;
    }
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->line_height = style_parse_line_height(sheet, value, value_length);
        *mask |= S_LINE_HEIGHT;
    
    return true;
}

static bool style_property_text_decoration(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            bool underline = false;
        if (style_parse_text_decoration_underline(
                sheet, value, value_length, &underline)) {
            style_set_text_underline(style, underline);
            *mask_high |= S2_TEXT_DECORATION;
        }
    
    return true;
}

static bool style_property_text_underline_offset(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            unsigned offset_code = 0;
        if (style_parse_text_underline_offset(
                sheet, value, value_length, &offset_code)) {
            style_set_text_offset_code(
                style, offset_code, STYLE_TEXT_OFFSET_SHIFT,
                STYLE_TEXT_OFFSET_MASK);
            *mask_high |= S2_TEXT_UNDERLINE_OFFSET;
        }
    
    return true;
}

static bool style_property_text_align(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            if (strstr(resolved, "center") != NULL) {
                style->text_align = TEXT_ALIGN_CENTER;
            } else if (strstr(resolved, "right") != NULL) {
                style->text_align = TEXT_ALIGN_RIGHT;
            } else if (strstr(resolved, "left") != NULL) {
                style->text_align = TEXT_ALIGN_LEFT;
            } else if (strstr(resolved, "end") != NULL) {
                style->text_align = TEXT_ALIGN_END;
            } else {
                style->text_align = TEXT_ALIGN_START;
            }
            *mask |= S_TEXT_ALIGN;
        }
    
    return true;
}

static bool style_property_white_space(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
    if (span_case_equal(value, value_length, "inherit")
        || span_case_equal(value, value_length, "unset")) {
        *parse->inherit_mask |= S_WHITE_SPACE;
        return true;
    }
    if (span_case_equal(value, value_length, "initial")
        || span_case_equal(value, value_length, "revert")
        || span_case_equal(value, value_length, "revert-layer")) {
        style->white_space_mode = WHITE_SPACE_NORMAL;
        *mask |= S_WHITE_SPACE;
        return true;
    }
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->white_space_mode =
                strcmp(resolved, "nowrap") == 0 ? WHITE_SPACE_NOWRAP
                : strcmp(resolved, "pre") == 0 ? WHITE_SPACE_PRE
                : strcmp(resolved, "pre-wrap") == 0 ? WHITE_SPACE_PRE_WRAP
                : strcmp(resolved, "pre-line") == 0 ? WHITE_SPACE_PRE_LINE
                : strcmp(resolved, "break-spaces") == 0
                    ? WHITE_SPACE_BREAK_SPACES : WHITE_SPACE_NORMAL;
            *mask |= S_WHITE_SPACE;
        }
    
    return true;
}

static bool style_property_overflow_wrap(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            const char *keyword = resolved;
            size_t keyword_length = strlen(resolved);
            trim(&keyword, &keyword_length);
            OverflowWrap wrap = OVERFLOW_WRAP_NORMAL;
            bool recognized = true;
            if (span_case_equal(keyword, keyword_length,
                                "normal")) {
                wrap = OVERFLOW_WRAP_NORMAL;
            } else if (span_case_equal(keyword, keyword_length,
                                       "break-word")) {
                wrap = OVERFLOW_WRAP_BREAK_WORD;
            } else if (span_case_equal(keyword, keyword_length,
                                       "anywhere")) {
                wrap = OVERFLOW_WRAP_ANYWHERE;
            } else {
                recognized = false;
            }
            if (recognized) {
                style->overflow_wrap = (OverflowWrap) (
                    (style->overflow_wrap & ~STYLE_OVERFLOW_WRAP_MASK)
                    | wrap);
                *mask |= S_OVERFLOW_WRAP;
            }
        }
    
    return true;
}

static bool style_property_text_overflow(const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    const char *keyword = resolved;
    size_t length = strlen(resolved);
    trim(&keyword, &length);
    bool ellipsis = span_case_equal(keyword, length, "ellipsis");
    bool clip = span_case_equal(keyword, length, "clip")
        || span_case_equal(keyword, length, "initial")
        || span_case_equal(keyword, length, "unset")
        || span_case_equal(keyword, length, "revert")
        || span_case_equal(keyword, length, "revert-layer");
    if (!ellipsis && !clip) return true;
    parse->style->overflow_wrap = (OverflowWrap) (
        (parse->style->overflow_wrap & ~STYLE_TEXT_OVERFLOW_ELLIPSIS)
        | (ellipsis ? STYLE_TEXT_OVERFLOW_ELLIPSIS : 0));
    *parse->mask_high |= S2_TEXT_OVERFLOW;
    return true;
}

static bool style_property_webkit_line_clamp(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    const char *value = resolved;
    size_t length = strlen(resolved);
    trim(&value, &length);
    unsigned clamp = 0;
    if (span_case_equal(value, length, "none")
        || span_case_equal(value, length, "initial")
        || span_case_equal(value, length, "unset")
        || span_case_equal(value, length, "revert")
        || span_case_equal(value, length, "revert-layer")) {
        clamp = 0;
    } else {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (end == value || (size_t) (end - value) != length || parsed == 0) {
            return true;
        }
        clamp = parsed > 31 ? 31u : (unsigned) parsed;
    }
    parse->style->line_clamp = (uint8_t) (
        (parse->style->line_clamp & STYLE_TAB_SIZE_MASK) | clamp);
    parse->style->overflow_wrap = (OverflowWrap) (
        parse->style->overflow_wrap | STYLE_LINE_CLAMP_SPECIFIED);
    /* Clamp and ellipsis are applied by the same inline-overflow stage. */
    *parse->mask_high |= S2_TEXT_OVERFLOW;
    return true;
}

static bool style_property_word_break(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
    char resolved[32];
    if (style_resolve_value(sheet, value, value_length, resolved,
                            sizeof(resolved), 0)) {
        if (strstr(resolved, "break-all") != NULL) {
            style->word_break_mode = (uint8_t) (
                (style->word_break_mode & ~STYLE_WORD_BREAK_MASK)
                | WORD_BREAK_ALL);
        } else if (strstr(resolved, "keep-all") != NULL) {
            style->word_break_mode = (uint8_t) (
                (style->word_break_mode & ~STYLE_WORD_BREAK_MASK)
                | WORD_BREAK_KEEP_ALL);
        } else if (strstr(resolved, "break-word") != NULL) {
            style->word_break_mode = (uint8_t) (
                (style->word_break_mode & ~STYLE_WORD_BREAK_MASK)
                | WORD_BREAK_LEGACY);
        } else {
            style->word_break_mode = (uint8_t) (
                style->word_break_mode & ~STYLE_WORD_BREAK_MASK);
        }
        *mask |= S_WORD_BREAK;
    }
    
    return true;
}
static bool style_property_text_transform(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    TextTransformMode mode;
    if (strcmp(resolved, "none") == 0
        || strcmp(resolved, "initial") == 0
        || strcmp(resolved, "unset") == 0
        || strcmp(resolved, "revert") == 0
        || strcmp(resolved, "revert-layer") == 0) {
        mode = TEXT_TRANSFORM_NONE;
    } else if (strcmp(resolved, "uppercase") == 0) {
        mode = TEXT_TRANSFORM_UPPERCASE;
    } else if (strcmp(resolved, "lowercase") == 0) {
        mode = TEXT_TRANSFORM_LOWERCASE;
    } else if (strcmp(resolved, "capitalize") == 0) {
        mode = TEXT_TRANSFORM_CAPITALIZE;
    } else {
        return true;
    }
    parse->style->text_transform = mode;
    *parse->mask_high |= S2_TEXT_TRANSFORM;
    return true;
}
