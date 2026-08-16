enum {
    STYLE_PHYSICAL_TOP,
    STYLE_PHYSICAL_RIGHT,
    STYLE_PHYSICAL_BOTTOM,
    STYLE_PHYSICAL_LEFT
};

static void style_logical_edge_indices(const Stylesheet *sheet,
                                       const ComputedStyle *style,
                                       bool inline_axis,
                                       unsigned *start, unsigned *end)
{
    uint8_t axes = sheet != NULL && sheet->resolve_scratch != NULL
                   && sheet->resolve_scratch->logical_axes_locked
        ? sheet->resolve_scratch->logical_axes : style->filter_code;
    unsigned mode = (axes & STYLE_WRITING_MODE_MASK)
                    >> STYLE_WRITING_MODE_SHIFT;
    bool rtl = (axes & STYLE_DIRECTION_RTL) != 0;
    if (mode == STYLE_WRITING_HORIZONTAL_TB) {
        *start = inline_axis ? (rtl ? STYLE_PHYSICAL_RIGHT
                                    : STYLE_PHYSICAL_LEFT)
                             : STYLE_PHYSICAL_TOP;
        *end = inline_axis ? (rtl ? STYLE_PHYSICAL_LEFT
                                  : STYLE_PHYSICAL_RIGHT)
                           : STYLE_PHYSICAL_BOTTOM;
    } else if (inline_axis) {
        *start = rtl ? STYLE_PHYSICAL_BOTTOM : STYLE_PHYSICAL_TOP;
        *end = rtl ? STYLE_PHYSICAL_TOP : STYLE_PHYSICAL_BOTTOM;
    } else {
        *start = mode == STYLE_WRITING_VERTICAL_RL
            ? STYLE_PHYSICAL_RIGHT : STYLE_PHYSICAL_LEFT;
        *end = mode == STYLE_WRITING_VERTICAL_RL
            ? STYLE_PHYSICAL_LEFT : STYLE_PHYSICAL_RIGHT;
    }
}

static int *style_edge_value(StyleEdges *edges, unsigned edge)
{
    if (edge == STYLE_PHYSICAL_TOP) return &edges->top;
    if (edge == STYLE_PHYSICAL_RIGHT) return &edges->right;
    if (edge == STYLE_PHYSICAL_BOTTOM) return &edges->bottom;
    return &edges->left;
}

static uint64_t style_margin_edge_mask(unsigned edge)
{
    if (edge == STYLE_PHYSICAL_TOP) return S_MARGIN_TOP;
    if (edge == STYLE_PHYSICAL_RIGHT) return S_MARGIN_RIGHT;
    if (edge == STYLE_PHYSICAL_BOTTOM) return S_MARGIN_BOTTOM;
    return S_MARGIN_LEFT;
}

static uint64_t style_padding_edge_mask(unsigned edge)
{
    if (edge == STYLE_PHYSICAL_TOP) return S_PADDING_TOP;
    if (edge == STYLE_PHYSICAL_RIGHT) return S_PADDING_RIGHT;
    if (edge == STYLE_PHYSICAL_BOTTOM) return S_PADDING_BOTTOM;
    return S_PADDING_LEFT;
}

static void style_margin_edge_set(ComputedStyle *style, unsigned edge,
                                  int value, bool automatic,
                                  bool percentage)
{
    *style_edge_value(&style->margin, edge) = value;
    if (edge == STYLE_PHYSICAL_TOP) {
        style->margin_top_auto = automatic;
        style->margin_top_percent = percentage;
    }
    else if (edge == STYLE_PHYSICAL_RIGHT) {
        style->margin_right_auto = automatic;
        style->margin_right_percent = percentage;
    } else if (edge == STYLE_PHYSICAL_BOTTOM) {
        style->margin_bottom_auto = automatic;
        style->margin_bottom_percent = percentage;
    } else {
        style->margin_left_auto = automatic;
        style->margin_left_percent = percentage;
    }
}

static bool style_property_margin(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            *mask |= style_parse_margin_box(sheet, value, value_length, style);
    
    return true;
}

static bool style_property_padding(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            *mask |= style_parse_padding_box(sheet, value, value_length,
                                   &style->padding);
    
    return true;
}

static bool style_property_margin_top(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->margin_top_auto = style_length_is_auto(sheet, value,
                                                value_length);
        bool percent = false;
        style->margin.top = style->margin_top_auto ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        style->margin_top_percent = percent;
        *mask |= S_MARGIN_TOP;
    
    return true;
}

static bool style_property_margin_right(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->margin_right_auto = style_length_is_auto(sheet, value,
                                                  value_length);
        bool percent = false;
        style->margin.right = style->margin_right_auto ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        style->margin_right_percent = percent;
        *mask |= S_MARGIN_RIGHT;
    
    return true;
}

static bool style_property_margin_bottom(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->margin_bottom_auto = style_length_is_auto(sheet, value,
                                                   value_length);
        bool percent = false;
        style->margin.bottom = style->margin_bottom_auto ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        style->margin_bottom_percent = percent;
        *mask |= S_MARGIN_BOTTOM;
    
    return true;
}

static bool style_property_margin_left(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->margin_left_auto = style_length_is_auto(sheet, value,
                                                 value_length);
        bool percent = false;
        style->margin.left = style->margin_left_auto ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        style->margin_left_percent = percent;
        *mask |= S_MARGIN_LEFT;
    
    return true;
}

static bool style_property_margin_inline(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        int values[2] = {0};
        bool automatic[2] = {false};
        bool percentages[2] = {false};
        if (style_parse_pair_with_auto(sheet, value, value_length, values,
                                 automatic, percentages)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, true, &start, &end);
            style_margin_edge_set(
                style, start, values[0], automatic[0], percentages[0]);
            style_margin_edge_set(
                style, end, values[1], automatic[1], percentages[1]);
            *mask |= style_margin_edge_mask(start)
                     | style_margin_edge_mask(end);
        }
    
    return true;
}

static bool style_property_margin_block(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        int values[2] = {0};
        bool automatic[2] = {false};
        bool percentages[2] = {false};
        if (style_parse_pair_with_auto(sheet, value, value_length, values,
                                 automatic, percentages)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, false, &start, &end);
            style_margin_edge_set(
                style, start, values[0], automatic[0], percentages[0]);
            style_margin_edge_set(
                style, end, values[1], automatic[1], percentages[1]);
            *mask |= style_margin_edge_mask(start)
                     | style_margin_edge_mask(end);
        }
    
    return true;
}

static bool style_property_margin_inline_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool automatic = style_length_is_auto(sheet, value, value_length);
        bool percent = false;
        int parsed = automatic ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        unsigned start, end;
        style_logical_edge_indices(sheet, style, true, &start, &end);
        style_margin_edge_set(style, start, parsed, automatic, percent);
        *mask |= style_margin_edge_mask(start);
    
    return true;
}

static bool style_property_margin_inline_end(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool automatic = style_length_is_auto(sheet, value, value_length);
        bool percent = false;
        int parsed = automatic ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        unsigned start, end;
        style_logical_edge_indices(sheet, style, true, &start, &end);
        style_margin_edge_set(style, end, parsed, automatic, percent);
        *mask |= style_margin_edge_mask(end);
    
    return true;
}

static bool style_property_margin_block_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool automatic = style_length_is_auto(sheet, value, value_length);
        bool percent = false;
        int parsed = automatic ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        unsigned start, end;
        style_logical_edge_indices(sheet, style, false, &start, &end);
        style_margin_edge_set(style, start, parsed, automatic, percent);
        *mask |= style_margin_edge_mask(start);
    
    return true;
}

static bool style_property_margin_block_end(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool automatic = style_length_is_auto(sheet, value, value_length);
        bool percent = false;
        int parsed = automatic ? 0
            : style_parse_length(sheet, value, value_length, 0, &percent);
        unsigned start, end;
        style_logical_edge_indices(sheet, style, false, &start, &end);
        style_margin_edge_set(style, end, parsed, automatic, percent);
        *mask |= style_margin_edge_mask(end);
    
    return true;
}

static bool style_property_padding_top(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            style->padding.top = parsed;
            *mask |= S_PADDING_TOP;
        }
    
    return true;
}

static bool style_property_padding_right(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            style->padding.right = parsed;
            *mask |= S_PADDING_RIGHT;
        }
    
    return true;
}

static bool style_property_padding_bottom(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            style->padding.bottom = parsed;
            *mask |= S_PADDING_BOTTOM;
        }
    
    return true;
}

static bool style_property_padding_left(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            style->padding.left = parsed;
            *mask |= S_PADDING_LEFT;
        }
    
    return true;
}

static bool style_property_padding_inline(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        int values[2] = {0};
        if (style_parse_padding_pair(sheet, value, value_length,
                                     &values[0], &values[1])) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, true, &start, &end);
            *style_edge_value(&style->padding, start) = values[0];
            *style_edge_value(&style->padding, end) = values[1];
            *mask |= style_padding_edge_mask(start)
                     | style_padding_edge_mask(end);
        }
    
    return true;
}

static bool style_property_padding_block(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        int values[2] = {0};
        if (style_parse_padding_pair(sheet, value, value_length,
                                     &values[0], &values[1])) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, false, &start, &end);
            *style_edge_value(&style->padding, start) = values[0];
            *style_edge_value(&style->padding, end) = values[1];
            *mask |= style_padding_edge_mask(start)
                     | style_padding_edge_mask(end);
        }
    
    return true;
}

static bool style_property_padding_inline_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, true, &start, &end);
            *style_edge_value(&style->padding, start) = parsed;
            *mask |= style_padding_edge_mask(start);
        }
    
    return true;
}

static bool style_property_padding_inline_end(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, true, &start, &end);
            *style_edge_value(&style->padding, end) = parsed;
            *mask |= style_padding_edge_mask(end);
        }
    
    return true;
}

static bool style_property_padding_block_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, false, &start, &end);
            *style_edge_value(&style->padding, start) = parsed;
            *mask |= style_padding_edge_mask(start);
        }
    
    return true;
}

static bool style_property_padding_block_end(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        StyleLength parsed = 0;
        if (parse_style_length(sheet, value, value_length, &parsed,
                               NULL)) {
            unsigned start, end;
            style_logical_edge_indices(sheet, style, false, &start, &end);
            *style_edge_value(&style->padding, end) = parsed;
            *mask |= style_padding_edge_mask(end);
        }
    
    return true;
}

static unsigned outline_style_code(const char *text, size_t length)
{
    if (span_case_equal(text, length, "none")
        || span_case_equal(text, length, "hidden")) {
        return STYLE_OUTLINE_NONE;
    }
    if (span_case_equal(text, length, "dashed")) {
        return STYLE_OUTLINE_DASHED;
    }
    if (span_case_equal(text, length, "dotted")) {
        return STYLE_OUTLINE_DOTTED;
    }
    if (span_case_equal(text, length, "solid")
        || span_case_equal(text, length, "double")
        || span_case_equal(text, length, "groove")
        || span_case_equal(text, length, "ridge")
        || span_case_equal(text, length, "inset")
        || span_case_equal(text, length, "outset")
        || span_case_equal(text, length, "auto")) {
        return STYLE_OUTLINE_SOLID;
    }
    return UINT_MAX;
}

static void outline_set_width_style(ComputedStyle *style,
                                    unsigned width, unsigned line_style)
{
    if (width > STYLE_OUTLINE_WIDTH_MASK) {
        width = STYLE_OUTLINE_WIDTH_MASK;
    }
    style->outline_state = (uint16_t) (
        (style->outline_state
         & ~(STYLE_OUTLINE_WIDTH_MASK | STYLE_OUTLINE_STYLE_MASK))
        | width
        | ((line_style << STYLE_OUTLINE_STYLE_SHIFT)
           & STYLE_OUTLINE_STYLE_MASK));
}

static bool style_property_outline(const StylePropertyParse *parse)
{
    char resolved[128];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    unsigned width = 3;
    unsigned line_style = STYLE_OUTLINE_NONE;
    bool recognized = false;
    bool color_set = false;
    uint32_t color = 0;
    uint8_t alpha = 255;
    for (size_t at = 0, length = strlen(resolved); at < length;) {
        while (at < length && isspace((unsigned char) resolved[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) resolved[end])) end++;
        if (end == at) break;
        unsigned candidate_style =
            outline_style_code(resolved + at, end - at);
        if (candidate_style != UINT_MAX) {
            line_style = candidate_style;
            recognized = true;
        } else if (span_case_equal(resolved + at, end - at,
                                   "currentcolor")) {
            parse->style->outline_state |= STYLE_OUTLINE_CURRENT_COLOR;
            color_set = true;
            recognized = true;
        } else if (style_parse_color_with_alpha(
                       parse->sheet, resolved + at, end - at,
                       &color, &alpha)) {
            parse->style->outline_state &= ~STYLE_OUTLINE_CURRENT_COLOR;
            color_set = true;
            recognized = true;
        } else {
            int parsed = style_parse_length(
                parse->sheet, resolved + at, end - at, -1, NULL);
            if (parsed >= 0) {
                width = (unsigned) parsed;
                recognized = true;
            }
        }
        at = end;
    }
    if (!recognized) return true;
    outline_set_width_style(parse->style, width, line_style);
    parse->style->outline_state |=
        STYLE_OUTLINE_DECL_WIDTH | STYLE_OUTLINE_DECL_STYLE;
    if (!color_set) parse->style->outline_state |=
        STYLE_OUTLINE_CURRENT_COLOR;
    else if ((parse->style->outline_state
              & STYLE_OUTLINE_CURRENT_COLOR) == 0) {
        parse->style->outline_color = color;
        parse->style->outline_alpha = alpha;
    }
    *parse->mask_high |= S2_OUTLINE_COLOR | S2_OUTLINE_STYLE;
    return true;
}

static bool style_property_outline_component(const StylePropertyParse *parse)
{
    char resolved[64];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (span_equal(parse->name, parse->name_length, "outline-color")) {
        if (strcasecmp(resolved, "currentcolor") == 0) {
            parse->style->outline_state |= STYLE_OUTLINE_CURRENT_COLOR;
        } else {
            uint32_t color = 0;
            uint8_t alpha = 255;
            if (!style_parse_color_with_alpha(
                    parse->sheet, resolved, strlen(resolved),
                    &color, &alpha)) return true;
            parse->style->outline_state &= ~STYLE_OUTLINE_CURRENT_COLOR;
            parse->style->outline_color = color;
            parse->style->outline_alpha = alpha;
        }
        *parse->mask_high |= S2_OUTLINE_COLOR;
        return true;
    }
    if (span_equal(parse->name, parse->name_length, "outline-offset")) {
        int offset = style_parse_length(
            parse->sheet, resolved, strlen(resolved), INT_MIN, NULL);
        if (offset == INT_MIN) return true;
        if (offset < -STYLE_OUTLINE_OFFSET_LIMIT) {
            offset = -STYLE_OUTLINE_OFFSET_LIMIT;
        }
        if (offset > STYLE_OUTLINE_OFFSET_LIMIT - 1) {
            offset = STYLE_OUTLINE_OFFSET_LIMIT - 1;
        }
        parse->style->outline_state = (uint16_t) (
            (parse->style->outline_state & ~STYLE_OUTLINE_OFFSET_MASK)
            | (((unsigned) offset & 63u)
               << STYLE_OUTLINE_OFFSET_SHIFT));
        *parse->mask_high |= S2_OUTLINE_OFFSET;
        return true;
    }
    unsigned line_style = outline_style_code(resolved, strlen(resolved));
    if (span_equal(parse->name, parse->name_length, "outline-style")
        && line_style != UINT_MAX) {
        outline_set_width_style(
            parse->style, computed_style_outline_width(parse->style),
            line_style);
        parse->style->outline_state |= STYLE_OUTLINE_DECL_STYLE;
        *parse->mask_high |= S2_OUTLINE_STYLE;
        return true;
    }
    if (span_equal(parse->name, parse->name_length, "outline-width")) {
        int width = style_parse_length(
            parse->sheet, resolved, strlen(resolved), -1, NULL);
        if (width < 0) {
            width = strcasecmp(resolved, "thin") == 0 ? 1
                : (strcasecmp(resolved, "medium") == 0 ? 3
                   : (strcasecmp(resolved, "thick") == 0 ? 5 : -1));
        }
        if (width >= 0) {
            outline_set_width_style(
                parse->style, (unsigned) width,
                computed_style_outline_style(parse->style));
            parse->style->outline_state |= STYLE_OUTLINE_DECL_WIDTH;
            *parse->mask_high |= S2_OUTLINE_STYLE;
        }
    }
    return true;
}

static bool style_property_border(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int width = 0;
        uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &width, &color, &alpha,
            &line_style, S_BORDER_ALL);
        /* A zero mask means nothing in the value parsed: drop the
           declaration and leave the cascaded border alone. */
        if (parsed != 0) {
            *mask |= parsed;
            style->border = (StyleEdges) {width, width, width, width};
            style->border_color = color;
            style->border_alpha = alpha;
            style->border_color_set = 0;
            for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
                computed_style_set_border_line(
                    style, (StyleBorderSide) side, line_style);
            }
            *parse->mask_high |= S2_BORDER_COLOR_ALL | S2_BORDER_LINE_ALL;
        }

    return true;
}

static bool style_property_border_top(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &style->border.top,
            &color, &alpha, &line_style, S_BORDER_TOP);
        if (parsed != 0) {
            *mask |= parsed;
            style_set_border_color(
                sheet, style, STYLE_BORDER_TOP, color, alpha);
            computed_style_set_border_line(
                style, STYLE_BORDER_TOP, line_style);
            *parse->mask_high |= S2_BORDER_COLOR_TOP | S2_BORDER_LINE_TOP;
        }
    
    return true;
}

static bool style_property_border_right(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &style->border.right,
            &color, &alpha, &line_style, S_BORDER_RIGHT);
        if (parsed != 0) {
            *mask |= parsed;
            style_set_border_color(
                sheet, style, STYLE_BORDER_RIGHT, color, alpha);
            computed_style_set_border_line(
                style, STYLE_BORDER_RIGHT, line_style);
            *parse->mask_high |=
                S2_BORDER_COLOR_RIGHT | S2_BORDER_LINE_RIGHT;
        }
    
    return true;
}

static bool style_property_border_bottom(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &style->border.bottom,
            &color, &alpha, &line_style, S_BORDER_BOTTOM);
        if (parsed != 0) {
            *mask |= parsed;
            style_set_border_color(
                sheet, style, STYLE_BORDER_BOTTOM, color, alpha);
            computed_style_set_border_line(
                style, STYLE_BORDER_BOTTOM, line_style);
            *parse->mask_high |=
                S2_BORDER_COLOR_BOTTOM | S2_BORDER_LINE_BOTTOM;
        }
    
    return true;
}

static bool style_property_border_left(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &style->border.left,
            &color, &alpha, &line_style, S_BORDER_LEFT);
        if (parsed != 0) {
            *mask |= parsed;
            style_set_border_color(
                sheet, style, STYLE_BORDER_LEFT, color, alpha);
            computed_style_set_border_line(
                style, STYLE_BORDER_LEFT, line_style);
            *parse->mask_high |= S2_BORDER_COLOR_LEFT | S2_BORDER_LINE_LEFT;
        }
    
    return true;
}

static bool style_property_border_inline(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int width = 0;
        uint64_t edges = span_equal(name, name_length,
                                    "border-inline")
                         ? S_BORDER_LEFT | S_BORDER_RIGHT
                         : S_BORDER_TOP | S_BORDER_BOTTOM;
        uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, &width, &color, &alpha,
            &line_style, edges);
        if (parsed != 0) {
            *mask |= parsed;
            if (edges & S_BORDER_LEFT) {
                style->border.left = style->border.right = width;
                style_set_border_color(
                    sheet, style, STYLE_BORDER_LEFT, color, alpha);
                style_set_border_color(
                    sheet, style, STYLE_BORDER_RIGHT, color, alpha);
                computed_style_set_border_line(
                    style, STYLE_BORDER_LEFT, line_style);
                computed_style_set_border_line(
                    style, STYLE_BORDER_RIGHT, line_style);
                *parse->mask_high |= S2_BORDER_COLOR_LEFT
                    | S2_BORDER_COLOR_RIGHT | S2_BORDER_LINE_LEFT
                    | S2_BORDER_LINE_RIGHT;
            } else {
                style->border.top = style->border.bottom = width;
                style_set_border_color(
                    sheet, style, STYLE_BORDER_TOP, color, alpha);
                style_set_border_color(
                    sheet, style, STYLE_BORDER_BOTTOM, color, alpha);
                computed_style_set_border_line(
                    style, STYLE_BORDER_TOP, line_style);
                computed_style_set_border_line(
                    style, STYLE_BORDER_BOTTOM, line_style);
                *parse->mask_high |= S2_BORDER_COLOR_TOP
                    | S2_BORDER_COLOR_BOTTOM | S2_BORDER_LINE_TOP
                    | S2_BORDER_LINE_BOTTOM;
            }
        }

    return true;
}

static bool style_property_border_inline_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int *edge = span_equal(name, name_length,
                               "border-inline-start")
                    ? &style->border.left
                    : (span_equal(name, name_length,
                                  "border-inline-end")
                       ? &style->border.right
                       : (span_equal(name, name_length,
                                     "border-block-start")
                          ? &style->border.top
                          : &style->border.bottom));
        uint64_t edge_mask = edge == &style->border.left
                             ? S_BORDER_LEFT
                             : (edge == &style->border.right
                                ? S_BORDER_RIGHT
                                : (edge == &style->border.top
                                   ? S_BORDER_TOP
                                   : S_BORDER_BOTTOM));
        uint32_t color = UINT32_MAX;
        uint8_t alpha = 255;
        unsigned line_style = STYLE_BORDER_NONE;
        uint64_t parsed = style_parse_border(
            sheet, value, value_length, edge, &color, &alpha,
            &line_style, edge_mask);
        if (parsed != 0) {
            StyleBorderSide side = edge == &style->border.left
                ? STYLE_BORDER_LEFT
                : (edge == &style->border.right ? STYLE_BORDER_RIGHT
                   : (edge == &style->border.top ? STYLE_BORDER_TOP
                                                 : STYLE_BORDER_BOTTOM));
            *mask |= parsed;
            style_set_border_color(sheet, style, side, color, alpha);
            computed_style_set_border_line(style, side, line_style);
            *parse->mask_high |= side == STYLE_BORDER_LEFT
                ? S2_BORDER_COLOR_LEFT | S2_BORDER_LINE_LEFT
                : (side == STYLE_BORDER_RIGHT
                   ? S2_BORDER_COLOR_RIGHT | S2_BORDER_LINE_RIGHT
                   : (side == STYLE_BORDER_TOP
                      ? S2_BORDER_COLOR_TOP | S2_BORDER_LINE_TOP
                      : S2_BORDER_COLOR_BOTTOM | S2_BORDER_LINE_BOTTOM));
        }
    
    return true;
}

static bool style_property_border_color(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
            uint32_t color = 0;
        uint8_t alpha = 255;
        if (style_parse_color_with_alpha(
                sheet, value, value_length, &color, &alpha)) {
            uint64_t visuals = 0;
            if (span_equal(name, name_length, "border-top-color")) {
                style_set_border_color(
                    sheet, style, STYLE_BORDER_TOP, color, alpha);
                visuals = S2_BORDER_COLOR_TOP;
            } else if (span_equal(name, name_length, "border-right-color")) {
                style_set_border_color(
                    sheet, style, STYLE_BORDER_RIGHT, color, alpha);
                visuals = S2_BORDER_COLOR_RIGHT;
            } else if (span_equal(name, name_length,
                                  "border-bottom-color")) {
                style_set_border_color(
                    sheet, style, STYLE_BORDER_BOTTOM, color, alpha);
                visuals = S2_BORDER_COLOR_BOTTOM;
            } else if (span_equal(name, name_length, "border-left-color")) {
                style_set_border_color(
                    sheet, style, STYLE_BORDER_LEFT, color, alpha);
                visuals = S2_BORDER_COLOR_LEFT;
            } else {
                style->border_color = color;
                style->border_alpha = alpha;
                style->border_color_set = 0;
                visuals = S2_BORDER_COLOR_ALL;
            }
            *parse->mask_high |= visuals;
        }
    
    return true;
}

static unsigned border_line_style(const char *text, size_t length)
{
    if (span_case_equal(text, length, "none")
        || span_case_equal(text, length, "hidden")) {
        return STYLE_BORDER_NONE;
    }
    if (span_case_equal(text, length, "dotted")) {
        return STYLE_BORDER_DOTTED;
    }
    if (span_case_equal(text, length, "dashed")) {
        return STYLE_BORDER_DASHED;
    }
    if (span_case_equal(text, length, "solid")
        || span_case_equal(text, length, "double")
        || span_case_equal(text, length, "groove")
        || span_case_equal(text, length, "ridge")
        || span_case_equal(text, length, "inset")
        || span_case_equal(text, length, "outset")) {
        return STYLE_BORDER_SOLID;
    }
    return UINT_MAX;
}

static bool style_property_border_style(const StylePropertyParse *parse)
{
    char resolved[96];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) return true;
    unsigned values[4] = {0};
    size_t count = 0;
    for (size_t at = 0, length = strlen(resolved);
         at < length && count < 4;) {
        while (at < length && isspace((unsigned char) resolved[at])) at++;
        size_t end = at;
        while (end < length
               && !isspace((unsigned char) resolved[end])) end++;
        if (end == at) break;
        unsigned line = border_line_style(resolved + at, end - at);
        if (line == UINT_MAX) return true;
        values[count++] = line;
        at = end;
    }
    if (count == 0) return true;
    unsigned sides[4] = {
        values[0],
        count > 1 ? values[1] : values[0],
        count > 2 ? values[2] : values[0],
        count > 3 ? values[3] : (count > 1 ? values[1] : values[0])
    };
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    uint64_t visual_mask = 0;
    if (span_equal(name, name_length, "border-top-style")) {
        computed_style_set_border_line(
            parse->style, STYLE_BORDER_TOP, sides[0]);
        visual_mask = S2_BORDER_LINE_TOP;
    } else if (span_equal(name, name_length, "border-right-style")) {
        computed_style_set_border_line(
            parse->style, STYLE_BORDER_RIGHT, sides[0]);
        visual_mask = S2_BORDER_LINE_RIGHT;
    } else if (span_equal(name, name_length, "border-bottom-style")) {
        computed_style_set_border_line(
            parse->style, STYLE_BORDER_BOTTOM, sides[0]);
        visual_mask = S2_BORDER_LINE_BOTTOM;
    } else if (span_equal(name, name_length, "border-left-style")) {
        computed_style_set_border_line(
            parse->style, STYLE_BORDER_LEFT, sides[0]);
        visual_mask = S2_BORDER_LINE_LEFT;
    } else {
        for (unsigned side = 0; side < STYLE_BORDER_SIDE_COUNT; side++) {
            computed_style_set_border_line(
                parse->style, (StyleBorderSide) side, sides[side]);
        }
        visual_mask = S2_BORDER_LINE_ALL;
    }
    *parse->mask_high |= visual_mask;
    return true;
}

static bool style_property_border_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            StyleEdges parsed = {0};
        if (span_case_equal(value, value_length, "initial")
            || span_case_equal(value, value_length, "unset")
            || span_case_equal(value, value_length, "revert")
            || span_case_equal(value, value_length, "revert-layer")) {
            parsed = (StyleEdges) {3, 3, 3, 3};
            style->border = parsed;
            *mask |= S_BORDER_ALL;
            return true;
        }
        uint64_t parsed_mask = style_parse_box(sheet, value, value_length,
                                         &parsed, false);
        style->border = parsed;
        if (parsed_mask != 0) *mask |= S_BORDER_ALL;
    
    return true;
}

static bool style_property_border_top_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int *edge = span_equal(name, name_length,
                               "border-top-width")
                    ? &style->border.top
                    : (span_equal(name, name_length,
                                  "border-right-width")
                       ? &style->border.right
                       : (span_equal(name, name_length,
                                     "border-bottom-width")
                          ? &style->border.bottom
                          : &style->border.left));
        uint64_t edge_mask = edge == &style->border.top
                             ? S_BORDER_TOP
                             : (edge == &style->border.right
                                ? S_BORDER_RIGHT
                                : (edge == &style->border.bottom
                                   ? S_BORDER_BOTTOM
                                   : S_BORDER_LEFT));
        if (span_case_equal(value, value_length, "initial")
            || span_case_equal(value, value_length, "unset")
            || span_case_equal(value, value_length, "revert")
            || span_case_equal(value, value_length, "revert-layer")) {
            *edge = 3;
        } else {
            *edge = style_parse_length(
                sheet, value, value_length, 0, NULL);
        }
        *mask |= edge_mask;
    
    return true;
}

static bool style_property_border_inline_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style_parse_pair(sheet, value, value_length, &style->border.left,
                   &style->border.right);
        *mask |= S_BORDER_LEFT | S_BORDER_RIGHT;
    
    return true;
}

static bool style_property_border_block_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style_parse_pair(sheet, value, value_length, &style->border.top,
                   &style->border.bottom);
        *mask |= S_BORDER_TOP | S_BORDER_BOTTOM;
    
    return true;
}

static bool style_property_border_inline_start_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->border.left = style_parse_length(sheet, value, value_length,
                                          0, NULL);
        *mask |= S_BORDER_LEFT;
    
    return true;
}

static bool style_property_border_inline_end_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->border.right = style_parse_length(sheet, value, value_length,
                                           0, NULL);
        *mask |= S_BORDER_RIGHT;
    
    return true;
}

static bool style_property_border_block_start_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->border.top = style_parse_length(sheet, value, value_length,
                                         0, NULL);
        *mask |= S_BORDER_TOP;
    
    return true;
}

static bool style_property_border_block_end_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style->border.bottom = style_parse_length(sheet, value, value_length,
                                            0, NULL);
        *mask |= S_BORDER_BOTTOM;
    
    return true;
}

static bool style_property_gap(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        style_parse_pair(sheet, value, value_length, &style->row_gap,
                         &style->gap);
        bool specified =
            !span_case_equal(value, value_length, "normal")
            && !span_case_equal(value, value_length, "initial")
            && !span_case_equal(value, value_length, "unset")
            && !span_case_equal(value, value_length, "revert")
            && !span_case_equal(value, value_length, "revert-layer");
        computed_style_set_grid_column_gap_specified(style, specified);
        computed_style_set_grid_row_gap_specified(style, specified);
        *mask |= S_GAP | S_ROW_GAP;
    
    return true;
}

static bool style_property_column_gap(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool percent = false;
        int parsed = style_parse_length(sheet, value, value_length, 0,
                                        &percent);
        style->gap = percent ? -parsed - 1 : parsed;
        computed_style_set_grid_column_gap_specified(
            style, !span_case_equal(value, value_length, "normal")
                   && !span_case_equal(value, value_length, "initial")
                   && !span_case_equal(value, value_length, "unset")
                   && !span_case_equal(value, value_length, "revert")
                   && !span_case_equal(
                          value, value_length, "revert-layer"));
        *mask |= S_GAP;
    
    return true;
}

static bool style_property_row_gap(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        style->row_gap = style_parse_length(sheet, value, value_length,
                                            0, NULL);
        computed_style_set_grid_row_gap_specified(
            style, !span_case_equal(value, value_length, "normal")
                   && !span_case_equal(value, value_length, "initial")
                   && !span_case_equal(value, value_length, "unset")
                   && !span_case_equal(value, value_length, "revert")
                   && !span_case_equal(
                          value, value_length, "revert-layer"));
        *mask |= S_ROW_GAP;
    
    return true;
}
