static bool parse_sizing_dimension_length(
    Stylesheet *sheet, const char *text, size_t length,
    StyleLength *value, bool *percent, int *em_thousandths);

static bool style_property_grid_template_columns(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        if (span_case_equal(value, value_length, "initial")
            || span_case_equal(value, value_length, "unset")
            || span_case_equal(value, value_length, "revert")
            || span_case_equal(value, value_length, "revert-layer")) {
            computed_style_set_grid_column_count(style, 0);
            computed_style_set_grid_subgrid_columns(style, false);
            computed_style_set_grid_column_template_id(style, 0);
            style->grid_min_column_width = 0;
            *mask |= S_GRID_COLUMNS;
        } else if (style_parse_grid_columns(
                       sheet, value, value_length, style)) {
            *mask |= S_GRID_COLUMNS;
        }
    
    return true;
}

static bool style_property_grid_template_areas(
    const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "unset")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        computed_style_set_grid_template_area_id(parse->style, 0);
        *parse->mask_high |= S2_GRID_TEMPLATE_AREAS;
    } else if (style_parse_grid_template_areas(
                   parse->sheet, parse->value, parse->value_length,
                   parse->style)) {
        *parse->mask_high |= S2_GRID_TEMPLATE_AREAS;
    }
    return true;
}

static bool style_property_writing_mode(const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (strcmp(resolved, "inherit") == 0
        || strcmp(resolved, "unset") == 0
        || strcmp(resolved, "revert") == 0
        || strcmp(resolved, "revert-layer") == 0) return true;
    unsigned mode = STYLE_WRITING_HORIZONTAL_TB;
    if (strcmp(resolved, "vertical-rl") == 0
        || strcmp(resolved, "sideways-rl") == 0) {
        mode = STYLE_WRITING_VERTICAL_RL;
    } else if (strcmp(resolved, "vertical-lr") == 0
               || strcmp(resolved, "sideways-lr") == 0) {
        mode = STYLE_WRITING_VERTICAL_LR;
    } else if (strcmp(resolved, "horizontal-tb") != 0
               && strcmp(resolved, "initial") != 0) {
        return true;
    }
    parse->style->filter_code = (uint8_t) (
        (parse->style->filter_code & ~STYLE_WRITING_MODE_MASK)
        | ((mode << STYLE_WRITING_MODE_SHIFT)
           & STYLE_WRITING_MODE_MASK));
    *parse->mask_high |= S2_WRITING_MODE;
    return true;
}

static bool style_property_direction(const StylePropertyParse *parse)
{
    char resolved[16];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) return true;
    if (strcmp(resolved, "inherit") == 0
        || strcmp(resolved, "unset") == 0
        || strcmp(resolved, "revert") == 0
        || strcmp(resolved, "revert-layer") == 0) return true;
    if (strcmp(resolved, "rtl") == 0) {
        parse->style->filter_code |= STYLE_DIRECTION_RTL;
    } else if (strcmp(resolved, "ltr") == 0
               || strcmp(resolved, "initial") == 0) {
        parse->style->filter_code &= (uint8_t) ~STYLE_DIRECTION_RTL;
    } else {
        return true;
    }
    *parse->mask_high |= S2_DIRECTION;
    return true;
}

static bool style_property_unicode_bidi(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) return true;
    parse->style->unicode_bidi_override =
        strcmp(resolved, "bidi-override") == 0
        || strcmp(resolved, "isolate-override") == 0;
    *parse->mask_high |= S2_UNICODE_BIDI;
    return true;
}

static bool style_property_grid_auto_flow(const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    bool column = false;
    bool dense = false;
    bool axis_seen = false;
    for (char *token = strtok(resolved, " \t\r\n");
         token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (strcmp(token, "row") == 0 || strcmp(token, "column") == 0) {
            if (axis_seen) return true;
            axis_seen = true;
            column = token[0] == 'c';
        } else if (strcmp(token, "dense") == 0) {
            if (dense) return true;
            dense = true;
        } else {
            return true;
        }
    }
    parse->style->containing_block_reserved =
        (uint8_t) ((column ? STYLE_GRID_AUTO_FLOW_COLUMN : 0)
                   | (dense ? STYLE_GRID_AUTO_FLOW_DENSE : 0));
    *parse->mask_high |= S2_GRID_AUTO_FLOW;
    return true;
}

static bool style_parse_grid_auto_track_text(
    const Stylesheet *sheet, char *resolved, uint16_t *track_value,
    uint16_t *track_type)
{
    if (sheet == NULL || resolved == NULL
        || track_value == NULL || track_type == NULL) return false;
    uint8_t parsed_type = GRID_TRACK_AUTO;
    unsigned parsed_value = 0;
    unsigned minimum = 0;
    if (!style_parse_grid_track(
            sheet, resolved, strlen(resolved), &parsed_type,
            &parsed_value, &minimum)
        || (minimum != 0
            && !(parsed_type == GRID_TRACK_FIXED
                 && minimum == parsed_value))) {
        return false;
    }
    if (parsed_value > UINT16_MAX) parsed_value = UINT16_MAX;
    *track_type = parsed_type;
    *track_value = (uint16_t) parsed_value;
    return true;
}

static bool style_parse_grid_auto_tracks(
    const StylePropertyParse *parse, uint16_t *track_value,
    uint16_t *track_type, uint16_t *second)
{
    char resolved[64];
    if (second != NULL) *second = 0;
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return false;
    }
    char *next = resolved;
    int depth = 0;
    while (*next != '\0') {
        if (*next == '(') depth++;
        else if (*next == ')' && depth > 0) depth--;
        else if (depth == 0 && isspace((unsigned char) *next)) break;
        next++;
    }
    if (*next != '\0') {
        *next++ = '\0';
        while (isspace((unsigned char) *next)) next++;
    }
    if (!style_parse_grid_auto_track_text(
            parse->sheet, resolved, track_value, track_type)) {
        return false;
    }
    if (second != NULL && *next != '\0') {
        char *end = next;
        depth = 0;
        while (*end != '\0') {
            if (*end == '(') depth++;
            else if (*end == ')' && depth > 0) depth--;
            else if (depth == 0 && isspace((unsigned char) *end)) break;
            end++;
        }
        *end = '\0';
        uint16_t second_type = 0;
        uint16_t second_value = 0;
        if (style_parse_grid_auto_track_text(
                parse->sheet, next, &second_value, &second_type)) {
            if (second_value > 16383u) second_value = 16383u;
            *second = (uint16_t) (
                ((second_type & 3u) << 14) | second_value);
        }
    }
    return true;
}

static bool style_property_grid_auto_columns(
    const StylePropertyParse *parse)
{
    uint16_t type = 0, value = 0, second = 0;
    if (style_parse_grid_auto_tracks(
            parse, &value, &type, &second)) {
        parse->style->grid_auto_column_type = type;
        parse->style->grid_auto_column_value = value;
        parse->style->grid_auto_column_second = second;
        *parse->mask_high |= S2_GRID_AUTO_COLUMNS;
    }
    return true;
}

static bool style_property_grid_auto_rows(const StylePropertyParse *parse)
{
    uint16_t type = 0, value = 0, second = 0;
    if (style_parse_grid_auto_tracks(
            parse, &value, &type, &second)) {
        parse->style->grid_auto_row_type = type;
        parse->style->grid_auto_row_value = value;
        parse->style->grid_auto_row_second = second;
        *parse->mask_high |= S2_GRID_AUTO_ROWS;
    }
    return true;
}

static bool style_property_grid_template_rows(
    const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "unset")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        computed_style_set_grid_subgrid_rows(parse->style, false);
        computed_style_set_grid_row_template_id(parse->style, 0);
        *parse->mask_high |= S2_GRID_UNIFORM_ROWS;
    } else if (style_parse_grid_rows(
                   parse->sheet, parse->value, parse->value_length,
                   parse->style)) {
        *parse->mask_high |= S2_GRID_UNIFORM_ROWS;
    }
    return true;
}

static bool style_property_grid_template(const StylePropertyParse *parse)
{
    char resolved[384];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) {
        return true;
    }
    char *slash = NULL;
    int parentheses = 0;
    for (char *at = resolved; *at != '\0'; at++) {
        if (*at == '(') parentheses++;
        else if (*at == ')' && parentheses > 0) parentheses--;
        else if (*at == '/' && parentheses == 0) {
            slash = at;
            break;
        }
    }
    if (slash == NULL) return true;
    ComputedStyle candidate = *parse->style;
    if (!style_parse_grid_rows(parse->sheet, resolved,
                               (size_t) (slash - resolved), &candidate)
        || !style_parse_grid_columns(parse->sheet, slash + 1,
                                     strlen(slash + 1), &candidate)) {
        return true;
    }
    computed_style_set_grid_column_count(
        parse->style, computed_style_grid_column_count(&candidate));
    parse->style->grid_min_column_width =
        candidate.grid_min_column_width;
    computed_style_set_grid_subgrid_columns(
        parse->style, computed_style_grid_subgrid_columns(&candidate));
    computed_style_set_grid_subgrid_rows(
        parse->style, computed_style_grid_subgrid_rows(&candidate));
    computed_style_set_grid_column_template_id(
        parse->style, computed_style_grid_column_template_id(&candidate));
    computed_style_set_grid_row_template_id(
        parse->style, computed_style_grid_row_template_id(&candidate));
    computed_style_set_grid_template_area_id(parse->style, 0);
    *parse->mask |= S_GRID_COLUMNS;
    *parse->mask_high |= S2_GRID_UNIFORM_ROWS | S2_GRID_TEMPLATE_AREAS;
    return true;
}

static bool style_property_grid_area(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "initial")
        || span_case_equal(parse->value, parse->value_length, "unset")
        || span_case_equal(parse->value, parse->value_length, "revert")
        || span_case_equal(parse->value, parse->value_length,
                           "revert-layer")) {
        memset(parse->style->grid_placement, 0,
               sizeof(parse->style->grid_placement));
        parse->style->grid_columns = (int) (
            (uint32_t) parse->style->grid_columns
            & ~COMPUTED_GRID_NAMED_LINE_MASK);
        *parse->mask_high |= S2_GRID_AREA_NAME
            | S2_GRID_ROW_START | S2_GRID_ROW_END | S2_GRID_ROW_SPAN
            | S2_GRID_COLUMN_START | S2_GRID_COLUMN_END
            | S2_GRID_COLUMN_SPAN;
        return true;
    }
    char resolved[192];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) {
        return true;
    }
    char *parts[4] = {resolved, NULL, NULL, NULL};
    size_t part_count = 1;
    for (char *at = resolved; *at != '\0'; at++) {
        if (*at != '/') continue;
        if (part_count >= 4) return true;
        *at = '\0';
        parts[part_count++] = at + 1;
    }
    int lines[4] = {0};
    int spans[4] = {0};
    if (part_count == 1
        && !style_parse_grid_line(parse->sheet, parts[0],
                                  strlen(parts[0]),
                                  &lines[0], &spans[0])) {
        if (!style_parse_grid_area_name(
                parse->sheet, parts[0], strlen(parts[0]), parse->style)) {
            return true;
        }
        *parse->mask_high |= S2_GRID_AREA_NAME
            | S2_GRID_ROW_START | S2_GRID_ROW_END | S2_GRID_ROW_SPAN
            | S2_GRID_COLUMN_START | S2_GRID_COLUMN_END
            | S2_GRID_COLUMN_SPAN;
        return true;
    }
    uint8_t names[4] = {0};
    for (size_t part = 0; part < part_count; part++) {
        if (!style_parse_grid_line_or_name(
                parse->sheet, parts[part], strlen(parts[part]),
                &lines[part], &spans[part], &names[part])) {
            return true;
        }
    }
    /*
     * grid-area expands in row-start / column-start / row-end / column-end
     * order. Missing components follow the CSS shorthand's auto fallback.
     */
    computed_style_set_grid_row_start(
        parse->style, lines[0] != 0
            ? computed_style_encode_grid_line(lines[0]) : 0);
    computed_style_set_grid_column_start(
        parse->style,
        part_count > 1 && lines[1] != 0
            ? computed_style_encode_grid_line(lines[1]) : 0);
    computed_style_set_grid_row_end(
        parse->style,
        part_count > 2 && lines[2] != 0
            ? computed_style_encode_grid_line(lines[2]) : 0);
    computed_style_set_grid_column_end(
        parse->style,
        part_count > 3 && lines[3] != 0
            ? computed_style_encode_grid_line(lines[3]) : 0);
    computed_style_set_grid_row_span(
        parse->style, spans[0] > 0 ? (unsigned) spans[0]
        : (part_count > 2 && spans[2] > 0 ? (unsigned) spans[2] : 0));
    computed_style_set_grid_column_span(
        parse->style, part_count > 1 && spans[1] > 0
        ? (unsigned) spans[1]
        : (part_count > 3 && spans[3] > 0 ? (unsigned) spans[3] : 0));
    computed_style_set_grid_row_start_name(parse->style, names[0]);
    computed_style_set_grid_column_start_name(
        parse->style, part_count > 1 ? names[1] : 0);
    computed_style_set_grid_row_end_name(
        parse->style, part_count > 2 ? names[2] : 0);
    computed_style_set_grid_column_end_name(
        parse->style, part_count > 3 ? names[3] : 0);
    *parse->mask_high |= S2_GRID_ROW_START | S2_GRID_ROW_END
                         | S2_GRID_ROW_SPAN | S2_GRID_COLUMN_START
                         | S2_GRID_COLUMN_END | S2_GRID_COLUMN_SPAN;
    return true;
}

static bool style_property_grid_column(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            char resolved[96];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            char *slash = strchr(resolved, '/');
            int start_line = 0, end_line = 0;
            int start_span = 0, end_span = 0;
            uint8_t start_name = 0, end_name = 0;
            bool parsed = slash == NULL
                ? style_parse_grid_line_or_name(
                      sheet, resolved, strlen(resolved),
                      &start_line, &start_span, &start_name)
                : (style_parse_grid_line_or_name(
                       sheet, resolved, (size_t) (slash - resolved),
                       &start_line, &start_span, &start_name)
                   && style_parse_grid_line_or_name(
                          sheet, slash + 1, strlen(slash + 1),
                          &end_line, &end_span, &end_name));
            if (parsed) {
                computed_style_set_grid_column_start(
                    style, start_line != 0
                        ? computed_style_encode_grid_line(start_line) : 0);
                computed_style_set_grid_column_end(
                    style, end_line != 0
                        ? computed_style_encode_grid_line(end_line) : 0);
                int span = end_span != 0 ? end_span : start_span;
                computed_style_set_grid_column_span(
                    style, span > 0 ? (unsigned) span : 0);
                computed_style_set_grid_column_start_name(
                    style, start_name);
                computed_style_set_grid_column_end_name(style, end_name);
                *mask_high |= S2_GRID_COLUMN_START
                              | S2_GRID_COLUMN_END
                              | S2_GRID_COLUMN_SPAN;
            }
        }
    
    return true;
}

static bool style_property_grid_column_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            int line = 0, span = 0;
        uint8_t name = 0;
        if (style_parse_grid_line_or_name(
                sheet, value, value_length, &line, &span, &name)) {
            computed_style_set_grid_column_start(
                style, line != 0
                    ? computed_style_encode_grid_line(line) : 0);
            computed_style_set_grid_column_start_name(style, name);
            *mask_high |= S2_GRID_COLUMN_START;
            if (span > 0) {
                computed_style_set_grid_column_span(
                    style, (unsigned) span);
                *mask_high |= S2_GRID_COLUMN_SPAN;
            }
        }
    
    return true;
}

static bool style_property_grid_column_end(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            int line = 0, span = 0;
        uint8_t name = 0;
        if (style_parse_grid_line_or_name(
                sheet, value, value_length, &line, &span, &name)) {
            computed_style_set_grid_column_end(
                style, line != 0
                    ? computed_style_encode_grid_line(line) : 0);
            computed_style_set_grid_column_end_name(style, name);
            *mask_high |= S2_GRID_COLUMN_END;
            if (span > 0) {
                computed_style_set_grid_column_span(
                    style, (unsigned) span);
                *mask_high |= S2_GRID_COLUMN_SPAN;
            }
        }
    
    return true;
}

static bool style_property_grid_row(const StylePropertyParse *parse)
{
    char resolved[96];
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)) {
        char *slash = strchr(resolved, '/');
        int start_line = 0, end_line = 0;
        int start_span = 0, end_span = 0;
        uint8_t start_name = 0, end_name = 0;
        bool parsed = slash == NULL
            ? style_parse_grid_line_or_name(
                  parse->sheet, resolved, strlen(resolved),
                  &start_line, &start_span, &start_name)
            : (style_parse_grid_line_or_name(
                   parse->sheet, resolved, (size_t) (slash - resolved),
                   &start_line, &start_span, &start_name)
               && style_parse_grid_line_or_name(
                      parse->sheet, slash + 1, strlen(slash + 1),
                      &end_line, &end_span, &end_name));
        if (parsed) {
            computed_style_set_grid_row_start(
                parse->style,
                start_line != 0
                    ? computed_style_encode_grid_line(start_line) : 0);
            computed_style_set_grid_row_end(
                parse->style,
                end_line != 0
                    ? computed_style_encode_grid_line(end_line) : 0);
            int span = end_span != 0 ? end_span : start_span;
            computed_style_set_grid_row_span(
                parse->style, span > 0 ? (unsigned) span : 0);
            computed_style_set_grid_row_start_name(
                parse->style, start_name);
            computed_style_set_grid_row_end_name(
                parse->style, end_name);
            *parse->mask_high |= S2_GRID_ROW_START
                                 | S2_GRID_ROW_END
                                 | S2_GRID_ROW_SPAN;
        }
    }
    return true;
}

static bool style_property_grid_row_start(const StylePropertyParse *parse)
{
    int line = 0, span = 0;
    uint8_t name = 0;
    if (style_parse_grid_line_or_name(
            parse->sheet, parse->value, parse->value_length,
            &line, &span, &name)) {
        computed_style_set_grid_row_start(
            parse->style, line != 0
                ? computed_style_encode_grid_line(line) : 0);
        computed_style_set_grid_row_start_name(parse->style, name);
        *parse->mask_high |= S2_GRID_ROW_START;
        if (span > 0) {
            computed_style_set_grid_row_span(
                parse->style, (unsigned) span);
            *parse->mask_high |= S2_GRID_ROW_SPAN;
        }
    }
    return true;
}

static bool style_property_grid_row_end(const StylePropertyParse *parse)
{
    int line = 0, span = 0;
    uint8_t name = 0;
    if (style_parse_grid_line_or_name(
            parse->sheet, parse->value, parse->value_length,
            &line, &span, &name)) {
        computed_style_set_grid_row_end(
            parse->style,
            line != 0 ? computed_style_encode_grid_line(line) : 0);
        computed_style_set_grid_row_end_name(parse->style, name);
        *parse->mask_high |= S2_GRID_ROW_END;
        if (span > 0) {
            computed_style_set_grid_row_span(
                parse->style, (unsigned) span);
            *parse->mask_high |= S2_GRID_ROW_SPAN;
        }
    }
    return true;
}

static bool style_property_flex_direction(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved, sizeof(resolved), 0)) {
            bool reverse = strstr(resolved, "reverse") != NULL;
            style->flex_direction = strstr(resolved, "column") != NULL
                ? (reverse ? FLEX_COLUMN_REVERSE : FLEX_COLUMN)
                : (reverse ? FLEX_ROW_REVERSE : FLEX_ROW);
            *mask |= S_FLEX_DIRECTION;
        }
    
    return true;
}

static bool style_property_flex_flow(const StylePropertyParse *parse)
{
    /* <'flex-direction'> || <'flex-wrap'> in either order.  Responsive skins
       can linearize infoboxes with flex-flow:column nowrap, so the shorthand
       carries real layout on live article pages. */
    Stylesheet *sheet = parse->sheet;
    ComputedStyle *style = parse->style;
    char resolved[48];
    if (!style_resolve_value(sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) {
        return true;
    }
    const char *at = resolved;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t') at++;
        if (*at == '\0') break;
        const char *end = at;
        while (*end != '\0' && *end != ' ' && *end != '\t') end++;
        size_t length = (size_t) (end - at);
        if (span_case_equal(at, length, "row")
            || span_case_equal(at, length, "row-reverse")
            || span_case_equal(at, length, "column")
            || span_case_equal(at, length, "column-reverse")) {
            bool column = length >= 6 && strncasecmp(at, "column", 6) == 0;
            bool reverse = length > 7
                           && strncasecmp(at + length - 7, "reverse", 7) == 0;
            style->flex_direction = column
                ? (reverse ? FLEX_COLUMN_REVERSE : FLEX_COLUMN)
                : (reverse ? FLEX_ROW_REVERSE : FLEX_ROW);
            *parse->mask |= S_FLEX_DIRECTION;
        } else if (span_case_equal(at, length, "nowrap")
                   || span_case_equal(at, length, "wrap")
                   || span_case_equal(at, length, "wrap-reverse")) {
            style->flex_wrap = !span_case_equal(at, length, "nowrap");
            style->flex_wrap_reverse =
                span_case_equal(at, length, "wrap-reverse");
            *parse->mask |= S_FLEX_WRAP;
        }
        at = end;
    }
    return true;
}

static bool style_property_flex_wrap(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            bool nowrap = strcmp(resolved, "nowrap") == 0;
            bool wrap = strcmp(resolved, "wrap") == 0;
            bool reverse = strcmp(resolved, "wrap-reverse") == 0;
            if (nowrap || wrap || reverse) {
                style->flex_wrap = !nowrap;
                style->flex_wrap_reverse = reverse;
                *mask |= S_FLEX_WRAP;
            }
        }
    
    return true;
}

static bool style_property_flex_grow(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            double grow = strtod(resolved, NULL);
            if (grow < 0.0) grow = 0.0;
            if (grow > 100.0) grow = 100.0;
            style->flex_grow = (int) (grow * 1000.0 + 0.5);
            *mask |= S_FLEX_GROW;
        }
    
    return true;
}

static bool style_property_flex_shrink(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            double shrink = strtod(resolved, NULL);
            if (shrink < 0.0) shrink = 0.0;
            if (shrink > 100.0) shrink = 100.0;
            style->flex_shrink =
                (uint16_t) (shrink * 512.0 + 0.5);
            *mask |= S_FLEX_SHRINK;
        }
    
    return true;
}

static bool style_property_flex_basis(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        if (style_keyword(sheet, value, value_length, "auto")
            || style_keyword(sheet, value, value_length, "content")) {
            style->has_flex_basis = false;
            style->flex_basis = 0;
            style->flex_basis_percent = false;
            style->flex_basis_offset = 0;
            *mask |= S_FLEX_BASIS;
        } else if (style_keyword(sheet, value, value_length, "min-content")
                   || style_keyword(sheet, value, value_length, "max-content")
                   || style_keyword(sheet, value, value_length, "fit-content")) {
            style->has_flex_basis = true;
            style->flex_basis = style_keyword(
                sheet, value, value_length, "min-content")
                ? STYLE_LENGTH_MIN_CONTENT
                : (style_keyword(sheet, value, value_length, "max-content")
                   ? STYLE_LENGTH_MAX_CONTENT : STYLE_LENGTH_FIT_CONTENT);
            style->flex_basis_percent = false;
            style->flex_basis_offset = 0;
            *mask |= S_FLEX_BASIS;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            if (parse_dimension_length(sheet, value, value_length,
                                       &parsed, &percent)) {
                style->has_flex_basis = true;
                style->flex_basis = parsed;
                style->flex_basis_percent = percent;
                style->flex_basis_offset = 0;
                *mask |= S_FLEX_BASIS;
            }
        }
    
    return true;
}

static bool style_property_flex(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[STYLE_MATH_SOURCE_CAPACITY];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            char *cursor = resolved;
            char *end = NULL;
            while (isspace((unsigned char) *cursor)) cursor++;
            if (strcmp(cursor, "none") == 0
                || strcmp(cursor, "initial") == 0
                || strcmp(cursor, "auto") == 0) {
                bool automatic = strcmp(cursor, "auto") == 0;
                style->flex_grow = automatic ? 1000 : 0;
                style->flex_shrink = strcmp(cursor, "none") == 0 ? 0 : 512;
                style->has_flex_basis = false;
                style->flex_basis = 0;
                style->flex_basis_percent = false;
                style->flex_basis_offset = 0;
                *mask |= S_FLEX_GROW | S_FLEX_SHRINK | S_FLEX_BASIS;
                return true;
            }
            size_t programs = sheet->math_program_count;
            size_t instructions = sheet->math_instruction_count;
            double grow = strtod(cursor, &end);
            if (end != cursor && isfinite(grow) && grow >= 0.0
                && grow <= 100.0) {
                int parsed_grow = (int) (grow * 1000.0 + 0.5);
                uint16_t parsed_shrink = 512;
                bool parsed_basis = false;
                bool basis_percent = false;
                bool valid_basis = true;
                StyleLength basis = 0;
                cursor = end;
                while (isspace((unsigned char) *cursor)) cursor++;
                double shrink = strtod(cursor, &end);
                if (end != cursor && isfinite(shrink)
                    && shrink >= 0.0 && shrink <= 100.0
                    && (*end == '\0'
                        || isspace((unsigned char) *end))) {
                    parsed_shrink =
                        (uint16_t) (shrink * 512.0 + 0.5);
                    cursor = end;
                }
                while (isspace((unsigned char) *cursor)) cursor++;
                if (*cursor != '\0') {
                    if (strcmp(cursor, "auto") == 0
                        || strcmp(cursor, "content") == 0) {
                        parsed_basis = false;
                    } else if (strcmp(cursor, "min-content") == 0
                               || strcmp(cursor, "max-content") == 0
                               || strcmp(cursor, "fit-content") == 0) {
                        parsed_basis = true;
                        basis = strcmp(cursor, "min-content") == 0
                            ? STYLE_LENGTH_MIN_CONTENT
                            : (strcmp(cursor, "max-content") == 0
                               ? STYLE_LENGTH_MAX_CONTENT
                               : STYLE_LENGTH_FIT_CONTENT);
                    } else if (!parse_dimension_length(
                                   sheet, cursor, strlen(cursor),
                                   &basis, &basis_percent)) {
                        style_math_restore(sheet, programs,
                                           instructions);
                        valid_basis = false;
                    } else {
                        parsed_basis = true;
                    }
                } else {
                    /* The omitted flex-basis in the numeric shorthand
                       computes to 0%, not auto. */
                    parsed_basis = true;
                    basis_percent = true;
                    basis = 0;
                }
                if (valid_basis) {
                    style->flex_grow = parsed_grow;
                    style->flex_shrink = parsed_shrink;
                    style->has_flex_basis = parsed_basis;
                    style->flex_basis = basis;
                    style->flex_basis_percent = basis_percent;
                    style->flex_basis_offset = 0;
                    *mask |= S_FLEX_GROW | S_FLEX_SHRINK
                             | S_FLEX_BASIS;
                }
            }
        }
    
    return true;
}

static bool style_property_table_layout(const StylePropertyParse *parse)
{
    char resolved[16];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (strcmp(resolved, "fixed") == 0
        || strcmp(resolved, "auto") == 0
        || strcmp(resolved, "initial") == 0
        || strcmp(resolved, "unset") == 0
        || strcmp(resolved, "revert") == 0
        || strcmp(resolved, "revert-layer") == 0) {
        parse->style->table_layout_fixed = strcmp(resolved, "fixed") == 0;
        *parse->mask_high |= S2_TABLE_LAYOUT;
    }
    return true;
}

static bool style_property_caption_side(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (strcmp(resolved, "bottom") == 0) {
        parse->style->order = 200000;
        *parse->mask_high |= S2_ORDER;
    } else if (strcmp(resolved, "top") == 0
               || strcmp(resolved, "initial") == 0
               || strcmp(resolved, "unset") == 0
               || strcmp(resolved, "revert") == 0
               || strcmp(resolved, "revert-layer") == 0) {
        parse->style->order = -200000;
        *parse->mask_high |= S2_ORDER;
    }
    return true;
}

static bool style_property_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[STYLE_MATH_SOURCE_CAPACITY] = {0};
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            bool automatic = strcmp(resolved, "auto") == 0
                || strcmp(resolved, "initial") == 0
                || strcmp(resolved, "unset") == 0
                || strcmp(resolved, "revert") == 0
                || strcmp(resolved, "revert-layer") == 0
                || strcmp(resolved, "inherit") == 0;
            /* All three modes share the intrinsic-layout dispatch bit while
               retaining distinct sentinels. Minerva, for example, narrows
               article tables with width:fit-content, whereas max-content is
               allowed to overflow the containing block. */
            bool min_content = strcmp(resolved, "min-content") == 0;
            bool max_content = strcmp(resolved, "max-content") == 0;
            bool fit_content = strcmp(resolved, "fit-content") == 0;
            bool intrinsic = min_content || max_content || fit_content;
            StyleLength parsed = 0;
            bool percent = false;
            int em_thousandths = 0;
            if (automatic || intrinsic
                || parse_sizing_dimension_length(
                    sheet, resolved, strlen(resolved), &parsed,
                    &percent, &em_thousandths)) {
                style->has_width = !automatic;
                style->width_max_content = intrinsic;
                style->width = min_content ? STYLE_LENGTH_MIN_CONTENT
                    : (max_content ? STYLE_LENGTH_MAX_CONTENT
                       : (fit_content ? STYLE_LENGTH_FIT_CONTENT
                          : (automatic ? 0 : parsed)));
                style->width_percent = !intrinsic && !automatic
                                       && percent;
                style->width_offset =
                    intrinsic || automatic ? 0 : em_thousandths;
                *mask |= S_WIDTH;
            }
        }
    
    return true;
}

static bool style_property_height(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[STYLE_MATH_SOURCE_CAPACITY] = {0};
        if (!style_resolve_value(sheet, value, value_length, resolved,
                                 sizeof(resolved), 0)) {
            return true;
        }
        bool intrinsic = strcmp(resolved, "max-content") == 0
                         || strcmp(resolved, "min-content") == 0
                         || strcmp(resolved, "fit-content") == 0;
        if (style_noninherited_length_is_auto(
                sheet, resolved, strlen(resolved))) {
            style->has_height = false;
            style->height = 0;
            style->height_percent = false;
            *mask |= S_HEIGHT;
        } else if (intrinsic) {
            /* A retained sentinel distinguishes intrinsic/auto block sizing
               for grid stretch while the ordinary content-height path still
               derives the used height from descendants. */
            style->has_height = true;
            style->height = STYLE_LENGTH_NONE;
            style->height_percent = false;
            *mask |= S_HEIGHT;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            if (parse_dimension_length(sheet, resolved, strlen(resolved),
                                       &parsed, &percent)) {
                style->has_height = true;
                style->height = parsed;
                style->height_percent = percent;
                *mask |= S_HEIGHT;
            }
        }
    
    return true;
}

static bool style_property_aspect_ratio(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[48];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            char *start = resolved;
            while (isspace((unsigned char) *start)) start++;
            if (strncmp(start, "auto", 4) == 0
                && isspace((unsigned char) start[4])) {
                start += 4;
                while (isspace((unsigned char) *start)) start++;
            }
            char *end = NULL;
            double numerator = strtod(start, &end);
            while (end != NULL && isspace((unsigned char) *end)) end++;
            double denominator = 1.0;
            bool valid = end != start;
            if (valid && end != NULL && *end == '/') {
                char *after = NULL;
                denominator = strtod(end + 1, &after);
                while (after != NULL
                       && isspace((unsigned char) *after)) after++;
                valid = after != end + 1 && after != NULL
                        && *after == '\0';
            } else {
                valid = end != NULL && *end == '\0';
            }
            if (valid && numerator > 0.0 && numerator <= 10000.0
                && denominator > 0.0 && denominator <= 10000.0) {
                int width = (int) (numerator * 1000.0 + 0.5);
                int height = (int) (denominator * 1000.0 + 0.5);
                if (width < 1) width = 1;
                if (height < 1) height = 1;
                int first = width, second = height;
                while (second != 0) {
                    int remainder = first % second;
                    first = second;
                    second = remainder;
                }
                style->aspect_width = width / first;
                style->aspect_height = height / first;
                *mask |= S_ASPECT_RATIO;
            }
        }
    
    return true;
}

static bool style_property_overflow(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            bool clip_only = strcmp(resolved, "clip") == 0;
            bool hidden = strcmp(resolved, "hidden") == 0;
            bool scroll = clip_only
                          || strstr(resolved, "auto") != NULL
                          || strstr(resolved, "scroll") != NULL
                          || strstr(resolved, "hidden") != NULL;
            if (!span_equal(name, name_length, "overflow-y")) {
                style->overflow_x_scroll = scroll;
                style->overflow_x_clip_only = clip_only;
                style->filter_code = (uint8_t) (
                    (style->filter_code & ~STYLE_OVERFLOW_X_HIDDEN)
                    | (hidden ? STYLE_OVERFLOW_X_HIDDEN : 0));
                *mask |= S_OVERFLOW_X;
            }
            if (!span_equal(name, name_length, "overflow-x")) {
                style->overflow_y_scroll = scroll;
                style->overflow_y_clip_only = clip_only;
                *mask |= S_OVERFLOW_Y;
            }
        }
    
    return true;
}

static bool style_property_scrollbar_gutter(
    const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (strcmp(resolved, "stable") == 0) {
        parse->style->scrollbar_gutter_stable = true;
        *parse->mask_high |= S2_SCROLLBAR_GUTTER;
    } else if (strcmp(resolved, "auto") == 0
               || strcmp(resolved, "initial") == 0
               || strcmp(resolved, "unset") == 0
               || strcmp(resolved, "revert") == 0) {
        parse->style->scrollbar_gutter_stable = false;
        *parse->mask_high |= S2_SCROLLBAR_GUTTER;
    }
    return true;
}

static bool style_property_border_collapse(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    if (strcmp(resolved, "collapse") != 0
        && strcmp(resolved, "separate") != 0
        && strcmp(resolved, "initial") != 0
        && strcmp(resolved, "unset") != 0
        && strcmp(resolved, "revert") != 0
        && strcmp(resolved, "revert-layer") != 0) {
        return true;
    }
    parse->style->table_border_collapse =
        strcmp(resolved, "collapse") == 0;
    *parse->mask_high |= S2_BORDER_COLLAPSE;
    return true;
}

static bool style_property_overflow_clip_margin(
    const StylePropertyParse *parse)
{
    char resolved[64];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) return true;
    StyleOverflowClipBox box = STYLE_OVERFLOW_CLIP_PADDING_BOX;
    unsigned length = 0;
    bool em = false;
    for (char *at = resolved; *at != '\0';) {
        while (isspace((unsigned char) *at)) at++;
        if (*at == '\0') break;
        char *end = at;
        while (*end != '\0' && !isspace((unsigned char) *end)) end++;
        char saved = *end;
        *end = '\0';
        if (strcmp(at, "padding-box") == 0) {
            box = STYLE_OVERFLOW_CLIP_PADDING_BOX;
        } else if (strcmp(at, "content-box") == 0) {
            box = STYLE_OVERFLOW_CLIP_CONTENT_BOX;
        } else if (strcmp(at, "border-box") == 0) {
            box = STYLE_OVERFLOW_CLIP_BORDER_BOX;
        } else {
            char *unit = NULL;
            double number = strtod(at, &unit);
            if (unit == at || number < 0.0 || !isfinite(number)
                || (strcmp(unit, "px") != 0 && strcmp(unit, "em") != 0
                    && !(number == 0.0 && *unit == '\0'))) {
                return true;
            }
            em = strcmp(unit, "em") == 0;
            double rounded = number + 0.5;
            length = rounded > STYLE_OVERFLOW_CLIP_MARGIN_MASK
                ? STYLE_OVERFLOW_CLIP_MARGIN_MASK : (unsigned) rounded;
        }
        *end = saved;
        at = end;
    }
    parse->style->overflow_clip_margin = (uint8_t) (
        ((unsigned) box << STYLE_OVERFLOW_CLIP_BOX_SHIFT)
        | (em ? STYLE_OVERFLOW_CLIP_MARGIN_EM : 0u) | length);
    *parse->mask_high |= S2_OVERFLOW_CLIP_MARGIN;
    return true;
}

/* Width constraints resolve `em` against the element's own computed font,
   which is not known while declarations are compiled. Retain a bounded
   thousandth coefficient in the existing *_offset field and materialize it
   after the cascade resolves font-size. Other lengths keep the compact
   StyleLength representation. */
static bool parse_sizing_dimension_length(
    Stylesheet *sheet, const char *text, size_t length,
    StyleLength *value, bool *percent, int *em_thousandths)
{
    char resolved[STYLE_MATH_SOURCE_CAPACITY];
    if (value == NULL || percent == NULL || em_thousandths == NULL
        || !style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)) {
        return false;
    }
    char *start = resolved;
    while (isspace((unsigned char) *start)) start++;
    char *end = NULL;
    double number = strtod(start, &end);
    if (end != start && isfinite(number)
        && end[0] == 'e' && end[1] == 'm') {
        char *tail = end + 2;
        while (isspace((unsigned char) *tail)) tail++;
        double scaled = number * 1000.0;
        if (*tail == '\0'
            && scaled >= -STYLE_LENGTH_DIRECT_LIMIT
            && scaled <= STYLE_LENGTH_DIRECT_LIMIT) {
            *value = 0;
            *percent = false;
            *em_thousandths = (int) (scaled < 0
                ? scaled - 0.5 : scaled + 0.5);
            return true;
        }
    }
    *em_thousandths = 0;
    return parse_dimension_length(
        sheet, resolved, strlen(resolved), value, percent);
}

static bool style_property_min_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            if (style_length_is_auto(sheet, value, value_length)) {
            style->min_width_auto = true;
            style->min_width = 0;
            style->min_width_percent = false;
            style->min_width_offset = 0;
            *mask |= S_MIN_WIDTH;
        } else if (style_keyword(sheet, value, value_length,
                                 "min-content")
                   || style_keyword(sheet, value, value_length,
                                    "max-content")
                   || style_keyword(sheet, value, value_length,
                                    "fit-content")) {
            style->min_width_auto = false;
            style->min_width = style_keyword(
                sheet, value, value_length, "min-content")
                ? STYLE_LENGTH_MIN_CONTENT
                : (style_keyword(sheet, value, value_length, "max-content")
                   ? STYLE_LENGTH_MAX_CONTENT : STYLE_LENGTH_FIT_CONTENT);
            style->min_width_percent = false;
            style->min_width_offset = 0;
            *mask |= S_MIN_WIDTH;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            int em_thousandths = 0;
            if (parse_sizing_dimension_length(
                    sheet, value, value_length, &parsed, &percent,
                    &em_thousandths)) {
                style->min_width_auto = false;
                style->min_width = parsed;
                style->min_width_percent = percent;
                style->min_width_offset = em_thousandths;
                *mask |= S_MIN_WIDTH;
            }
        }
    
    return true;
}

static bool style_property_min_height(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            if (style_length_is_auto(sheet, value, value_length)) {
            style->min_height = 0;
            style->min_height_percent = false;
            *mask |= S_MIN_HEIGHT;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            if (parse_dimension_length(sheet, value, value_length,
                                       &parsed, &percent)) {
                style->min_height = parsed;
                style->min_height_percent = percent;
                *mask |= S_MIN_HEIGHT;
            }
        }
    
    return true;
}

static bool style_property_max_width(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            bool unbounded = style_length_is_auto(sheet, value, value_length)
                         || style_keyword(sheet, value, value_length,
                                          "none");
        bool min_content = style_keyword(
            sheet, value, value_length, "min-content");
        bool max_content = style_keyword(
            sheet, value, value_length, "max-content");
        bool fit_content = style_keyword(
            sheet, value, value_length, "fit-content");
        if (unbounded) {
            style->max_width = STYLE_LENGTH_NONE;
            style->max_width_percent = false;
            style->max_width_offset = 0;
            *mask |= S_MAX_WIDTH;
        } else if (min_content) {
            /* This sentinel occupies otherwise-invalid StyleLength space, so
               the intrinsic constraint costs no retained per-node bytes. */
            style->max_width = STYLE_LENGTH_MIN_CONTENT;
            style->max_width_percent = false;
            style->max_width_offset = 0;
            *mask |= S_MAX_WIDTH;
        } else if (max_content || fit_content) {
            style->max_width = max_content ? STYLE_LENGTH_MAX_CONTENT
                                           : STYLE_LENGTH_FIT_CONTENT;
            style->max_width_percent = false;
            style->max_width_offset = 0;
            *mask |= S_MAX_WIDTH;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            int em_thousandths = 0;
            if (parse_sizing_dimension_length(
                    sheet, value, value_length, &parsed, &percent,
                    &em_thousandths)) {
                style->max_width = parsed;
                style->max_width_percent = percent;
                style->max_width_offset = em_thousandths;
                *mask |= S_MAX_WIDTH;
            }
        }
    
    return true;
}

static bool style_property_max_height(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool unbounded = style_length_is_auto(sheet, value, value_length)
                         || style_keyword(sheet, value, value_length,
                                          "none");
        bool min_content = style_keyword(
            sheet, value, value_length, "min-content");
        if (unbounded) {
            style->max_height = STYLE_LENGTH_NONE;
            style->max_height_percent = false;
            *mask |= S_MAX_HEIGHT;
        } else if (min_content) {
            style->max_height = STYLE_LENGTH_MIN_CONTENT;
            style->max_height_percent = false;
            *mask |= S_MAX_HEIGHT;
        } else {
            StyleLength parsed = 0;
            bool percent = false;
            if (parse_dimension_length(sheet, value, value_length,
                                       &parsed, &percent)) {
                style->max_height = parsed;
                style->max_height_percent = percent;
                *mask |= S_MAX_HEIGHT;
            }
        }
    
    return true;
}

static bool style_logical_vertical(const StylePropertyParse *parse)
{
    if (parse->sheet->resolve_scratch->logical_axes_locked) {
        return (parse->sheet->resolve_scratch->logical_axes
                & STYLE_WRITING_MODE_MASK) != 0;
    }
    return computed_style_writing_mode(parse->style)
           != STYLE_WRITING_HORIZONTAL_TB;
}

static bool style_property_inline_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_height(parse) : style_property_width(parse);
}

static bool style_property_block_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_width(parse) : style_property_height(parse);
}

static bool style_property_min_inline_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_min_height(parse) : style_property_min_width(parse);
}

static bool style_property_min_block_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_min_width(parse) : style_property_min_height(parse);
}

static bool style_property_max_inline_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_max_height(parse) : style_property_max_width(parse);
}

static bool style_property_max_block_size(const StylePropertyParse *parse)
{
    return style_logical_vertical(parse)
        ? style_property_max_width(parse) : style_property_max_height(parse);
}

static bool style_property_content_visibility(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    StyleContentVisibility visibility;
    if (strcmp(resolved, "auto") == 0) {
        visibility = STYLE_CONTENT_VISIBILITY_AUTO;
    } else if (strcmp(resolved, "hidden") == 0) {
        visibility = STYLE_CONTENT_VISIBILITY_HIDDEN;
    } else if (strcmp(resolved, "visible") == 0
               || strcmp(resolved, "initial") == 0
               || strcmp(resolved, "unset") == 0
               || strcmp(resolved, "revert") == 0
               || strcmp(resolved, "revert-layer") == 0) {
        visibility = STYLE_CONTENT_VISIBILITY_EXPLICIT_VISIBLE;
    } else {
        return true;
    }
    parse->style->content_visibility = visibility;
    /* Share contain's cascade slot to keep the common retained declaration
       record at 128 presence bits. apply_values() distinguishes the authored
       content-visibility marker from ordinary contain declarations. */
    *parse->mask_high |= S2_CONTAIN;
    return true;
}

static bool style_property_box_sizing(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->box_sizing_border_box =
                strstr(resolved, "border-box") != NULL;
            *mask |= S_BOX_SIZING;
        }
    
    return true;
}

static bool style_property_z_index(const StylePropertyParse *parse)
{
    if (span_case_equal(parse->value, parse->value_length, "inherit")) {
        *parse->inherit_mask |= S_Z_INDEX;
        *parse->mask |= S_Z_INDEX;
        return true;
    }
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->has_z_index = strcmp(resolved, "auto") != 0;
            style->z_index = style->has_z_index
                             ? (int) strtol(resolved, NULL, 10) : 0;
            if (style->z_index < -32768) style->z_index = -32768;
            if (style->z_index > 32767) style->z_index = 32767;
            *mask |= S_Z_INDEX;
        }
    
    return true;
}

static bool style_property_opacity(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            char *end = NULL;
            double opacity = strtod(resolved, &end);
            while (end != NULL && isspace((unsigned char) *end)) end++;
            if (end != NULL && *end == '%') opacity /= 100.0;
            if (opacity < 0.0) opacity = 0.0;
            if (opacity > 1.0) opacity = 1.0;
            style->opacity = (uint8_t) (opacity * 255.0 + 0.5);
            *mask |= S_OPACITY;
        }
    
    return true;
}

static bool style_property_transform(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            style_parse_transform_translation(sheet, value, value_length,
                                    style);
        *mask |= S_TRANSFORM;
    
    return true;
}

static bool style_property_fixed_containing_block(
    const StylePropertyParse *parse)
{
    char resolved[96];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    bool active = strcmp(resolved, "none") != 0
        && strcmp(resolved, "auto") != 0
        && strcmp(resolved, "normal") != 0
        && strcmp(resolved, "initial") != 0
        && strcmp(resolved, "unset") != 0
        && strcmp(resolved, "revert") != 0
        && strcmp(resolved, "revert-layer") != 0;
    if (span_equal(parse->name, parse->name_length, "perspective")) {
        parse->style->has_perspective = active;
        *parse->mask_high |= S2_PERSPECTIVE;
    } else if (span_equal(parse->name, parse->name_length, "filter")) {
        parse->style->has_filter = active;
        parse->style->filter_code &= ~STYLE_FILTER_CODE_MASK;
        StylePaintStack paint = style_paint_stack_copy(
            parse->sheet, parse->style);
        bool paint_changed =
            (paint.reserved & STYLE_PAINT_FILTER_LOW_AMOUNT) != 0;
        paint.reserved &= (uint8_t) ~STYLE_PAINT_FILTER_LOW_AMOUNT;
        if (active) {
            if (strstr(resolved, "grayscale(") != NULL) {
                parse->style->filter_code |= STYLE_FILTER_GRAYSCALE;
            } else if (strstr(resolved, "invert(") != NULL) {
                parse->style->filter_code |= STYLE_FILTER_INVERT;
            } else if (strstr(resolved, "sepia(") != NULL) {
                parse->style->filter_code |= STYLE_FILTER_SEPIA;
            } else if (strstr(resolved, "brightness(") != NULL) {
                const char *argument = strchr(resolved, '(');
                double amount = argument == NULL ? 1.0
                    : strtod(argument + 1, NULL);
                parse->style->filter_code |= amount < 1.0
                    ? STYLE_FILTER_DARKEN : STYLE_FILTER_BRIGHTEN;
            } else if (strstr(resolved, "contrast(") != NULL
                       || strstr(resolved, "saturate(") != NULL) {
                bool contrast = strstr(resolved, "contrast(") != NULL;
                const char *argument = strchr(resolved, '(');
                char *end = NULL;
                double amount = argument == NULL ? 1.0
                    : strtod(argument + 1, &end);
                if (end != NULL && *end == '%') amount /= 100.0;
                if (!isfinite(amount) || amount < 0.0) amount = 1.0;
                if (amount != 1.0) {
                    parse->style->filter_code |= contrast
                        ? STYLE_FILTER_CONTRAST : STYLE_FILTER_SATURATE;
                    if (amount < 1.0) {
                        paint.reserved |= STYLE_PAINT_FILTER_LOW_AMOUNT;
                    }
                    paint_changed = true;
                } else {
                    parse->style->has_filter = false;
                }
            }
        }
        if (paint_changed) {
            (void) style_apply_paint_stack(
                parse->sheet, parse->style, &paint, NULL);
        }
        *parse->mask_high |= S2_FILTER;
    } else if (span_equal(parse->name, parse->name_length, "contain")) {
        parse->style->has_layout_containment =
            active && (strstr(resolved, "layout") != NULL
                       || strstr(resolved, "paint") != NULL
                       || strcmp(resolved, "strict") == 0
                       || strcmp(resolved, "content") == 0);
        *parse->mask_high |= S2_CONTAIN;
    } else {
        parse->style->will_change_transform =
            active && (strstr(resolved, "transform") != NULL
                       || strstr(resolved, "perspective") != NULL
                       || strstr(resolved, "filter") != NULL);
        *parse->mask_high |= S2_WILL_CHANGE;
    }
    return true;
}

static bool style_property_pointer_events(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    parse->style->pointer_events_none = strcmp(resolved, "none") == 0;
    *parse->mask_high |= S2_POINTER_EVENTS;
    return true;
}

typedef bool (*StyleAlignmentParser)(const char *value, unsigned *result);

static const char *style_alignment_position(const char *value)
{
    if (strncmp(value, "safe ", 5) == 0) return value + 5;
    if (strncmp(value, "unsafe ", 7) == 0) return value + 7;
    return value;
}

static bool style_parse_align_items_keyword(const char *value,
                                            unsigned *result)
{
    value = style_alignment_position(value);
    if (strcmp(value, "normal") == 0 || strcmp(value, "initial") == 0
        || strcmp(value, "stretch") == 0) {
        *result = ALIGN_STRETCH;
    } else if (strcmp(value, "baseline") == 0
               || strcmp(value, "first baseline") == 0
               || strcmp(value, "last baseline") == 0) {
        *result = ALIGN_BASELINE;
    } else if (strcmp(value, "center") == 0) {
        *result = ALIGN_CENTER;
    } else if (strcmp(value, "end") == 0
               || strcmp(value, "self-end") == 0
               || strcmp(value, "flex-end") == 0) {
        *result = ALIGN_END;
    } else if (strcmp(value, "start") == 0
               || strcmp(value, "self-start") == 0
               || strcmp(value, "flex-start") == 0) {
        *result = ALIGN_START;
    } else {
        return false;
    }
    return true;
}

static bool style_parse_justify_items_keyword(const char *value,
                                              unsigned *result)
{
    value = style_alignment_position(value);
    if (strncmp(value, "legacy ", 7) == 0) value += 7;
    if (strcmp(value, "legacy") == 0
        || strcmp(value, "normal") == 0 || strcmp(value, "initial") == 0
        || strcmp(value, "stretch") == 0) {
        *result = ALIGN_STRETCH;
    } else if (strcmp(value, "baseline") == 0
               || strcmp(value, "first baseline") == 0
               || strcmp(value, "last baseline") == 0) {
        *result = ALIGN_BASELINE;
    } else if (strcmp(value, "center") == 0) {
        *result = ALIGN_CENTER;
    } else if (strcmp(value, "end") == 0
               || strcmp(value, "self-end") == 0
               || strcmp(value, "flex-end") == 0
               || strcmp(value, "right") == 0
               || strcmp(value, "right legacy") == 0) {
        *result = ALIGN_END;
    } else if (strcmp(value, "start") == 0
               || strcmp(value, "self-start") == 0
               || strcmp(value, "flex-start") == 0
               || strcmp(value, "left") == 0
               || strcmp(value, "left legacy") == 0) {
        *result = ALIGN_START;
    } else if (strcmp(value, "center legacy") == 0) {
        *result = ALIGN_CENTER;
    } else {
        return false;
    }
    return true;
}

static bool style_parse_justify_self_keyword(const char *value,
                                             unsigned *result)
{
    value = style_alignment_position(value);
    if (strcmp(value, "auto") == 0 || strcmp(value, "normal") == 0
        || strcmp(value, "initial") == 0) {
        *result = ALIGN_SELF_AUTO;
    } else if (strcmp(value, "stretch") == 0) {
        *result = ALIGN_SELF_STRETCH;
    } else if (strcmp(value, "baseline") == 0
               || strcmp(value, "first baseline") == 0
               || strcmp(value, "last baseline") == 0) {
        *result = ALIGN_SELF_BASELINE;
    } else if (strcmp(value, "center") == 0) {
        *result = ALIGN_SELF_CENTER;
    } else if (strcmp(value, "end") == 0
               || strcmp(value, "self-end") == 0
               || strcmp(value, "flex-end") == 0
               || strcmp(value, "right") == 0) {
        *result = ALIGN_SELF_END;
    } else if (strcmp(value, "start") == 0
               || strcmp(value, "self-start") == 0
               || strcmp(value, "flex-start") == 0
               || strcmp(value, "left") == 0) {
        *result = ALIGN_SELF_START;
    } else {
        return false;
    }
    return true;
}

static bool style_parse_align_self_keyword(const char *value,
                                           unsigned *result)
{
    const char *position = style_alignment_position(value);
    if (strcmp(position, "left") == 0
        || strcmp(position, "right") == 0) return false;
    return style_parse_justify_self_keyword(value, result);
}

static bool style_parse_align_content_keyword(const char *value,
                                              unsigned *result)
{
    value = style_alignment_position(value);
    if (strcmp(value, "normal") == 0 || strcmp(value, "initial") == 0
        || strcmp(value, "stretch") == 0) {
        *result = JUSTIFY_STRETCH;
    } else if (strcmp(value, "space-between") == 0) {
        *result = JUSTIFY_SPACE_BETWEEN;
    } else if (strcmp(value, "space-around") == 0) {
        *result = JUSTIFY_SPACE_AROUND;
    } else if (strcmp(value, "space-evenly") == 0) {
        *result = JUSTIFY_SPACE_EVENLY;
    } else if (strcmp(value, "center") == 0) {
        *result = JUSTIFY_CENTER;
    } else if (strcmp(value, "end") == 0
               || strcmp(value, "flex-end") == 0) {
        *result = JUSTIFY_END;
    } else if (strcmp(value, "start") == 0
               || strcmp(value, "flex-start") == 0
               || strcmp(value, "baseline") == 0
               || strcmp(value, "first baseline") == 0
               || strcmp(value, "last baseline") == 0) {
        /* The compact layout model has no cross-line baseline distribution;
           start is the standards-safe geometry fallback. */
        *result = JUSTIFY_START;
    } else {
        return false;
    }
    return true;
}

static bool style_parse_justify_content_keyword(const char *value,
                                                unsigned *result)
{
    value = style_alignment_position(value);
    if (strcmp(value, "normal") == 0 || strcmp(value, "initial") == 0
        || strcmp(value, "start") == 0
        || strcmp(value, "flex-start") == 0
        || strcmp(value, "left") == 0) {
        *result = JUSTIFY_START;
    } else if (strcmp(value, "space-between") == 0) {
        *result = JUSTIFY_SPACE_BETWEEN;
    } else if (strcmp(value, "space-around") == 0) {
        *result = JUSTIFY_SPACE_AROUND;
    } else if (strcmp(value, "space-evenly") == 0) {
        *result = JUSTIFY_SPACE_EVENLY;
    } else if (strcmp(value, "center") == 0) {
        *result = JUSTIFY_CENTER;
    } else if (strcmp(value, "end") == 0
               || strcmp(value, "flex-end") == 0
               || strcmp(value, "right") == 0) {
        *result = JUSTIFY_END;
    } else if (strcmp(value, "stretch") == 0) {
        *result = JUSTIFY_STRETCH;
    } else {
        return false;
    }
    return true;
}

static bool style_parse_alignment_pair(
    char *resolved, StyleAlignmentParser first_parser,
    StyleAlignmentParser second_parser, unsigned *first, unsigned *second)
{
    unsigned first_value = 0, second_value = 0;
    if (first_parser(resolved, &first_value)
        && second_parser(resolved, &second_value)) {
        *first = first_value;
        *second = second_value;
        return true;
    }
    if (first_parser == style_parse_align_content_keyword
        && second_parser == style_parse_justify_content_keyword
        && (strcmp(resolved, "baseline") == 0
            || strcmp(resolved, "first baseline") == 0
            || strcmp(resolved, "last baseline") == 0)
        && first_parser(resolved, &first_value)) {
        *first = first_value;
        *second = JUSTIFY_START;
        return true;
    }
    size_t length = strlen(resolved);
    for (size_t at = 1; at < length; at++) {
        if (!isspace((unsigned char) resolved[at])) continue;
        size_t right = at;
        while (right < length
               && isspace((unsigned char) resolved[right])) right++;
        if (right >= length) break;
        char saved = resolved[at];
        resolved[at] = '\0';
        bool valid = first_parser(resolved, &first_value)
                     && second_parser(resolved + right, &second_value);
        resolved[at] = saved;
        if (valid) {
            *first = first_value;
            *second = second_value;
            return true;
        }
    }
    return false;
}

static bool style_property_align_items(const StylePropertyParse *parse)
{
    char resolved[32];
    unsigned value = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_align_items_keyword(resolved, &value)) {
        parse->style->align_items = (uint8_t) value;
        *parse->mask |= S_ALIGN_ITEMS;
    }
    return true;
}

static bool style_property_justify_items(const StylePropertyParse *parse)
{
    char resolved[32];
    unsigned value = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_justify_items_keyword(resolved, &value)) {
        parse->style->justify_items = (uint8_t) value;
        *parse->mask_high |= S2_JUSTIFY_ITEMS;
    }
    return true;
}

static bool style_property_place_items(const StylePropertyParse *parse)
{
    char resolved[64];
    unsigned align = 0, justify = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_alignment_pair(
               resolved, style_parse_align_items_keyword,
               style_parse_justify_items_keyword, &align, &justify)) {
        parse->style->align_items = (uint8_t) align;
        parse->style->justify_items = (uint8_t) justify;
        *parse->mask |= S_ALIGN_ITEMS;
        *parse->mask_high |= S2_JUSTIFY_ITEMS;
    }
    return true;
}

static bool style_property_order(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            char *end = NULL;
            long order = strtol(resolved, &end, 10);
            while (end != NULL && isspace((unsigned char) *end)) end++;
            if (end != resolved && end != NULL && *end == '\0') {
                if (order < INT_MIN) order = INT_MIN;
                if (order > INT_MAX) order = INT_MAX;
                style->order = (int) order;
                *mask_high |= S2_ORDER;
            }
        }
    
    return true;
}

static bool style_property_align_self(const StylePropertyParse *parse)
{
    char resolved[32];
    unsigned value = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_align_self_keyword(resolved, &value)) {
        parse->style->align_self = (AlignSelf) value;
        *parse->mask_high |= S2_ALIGN_SELF;
    }
    return true;
}

static bool style_property_justify_self(const StylePropertyParse *parse)
{
    char resolved[32];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    unsigned value = 0;
    if (!style_parse_justify_self_keyword(resolved, &value)) return true;
    computed_style_set_justify_self(parse->style, (AlignSelf) value);
    *parse->mask_high |= S2_JUSTIFY_SELF;
    return true;
}

static bool style_property_align_content(const StylePropertyParse *parse)
{
    char resolved[32];
    unsigned value = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_align_content_keyword(resolved, &value)) {
        parse->style->align_content = (JustifyContent) value;
        *parse->mask_high |= S2_ALIGN_CONTENT;
    }
    return true;
}

static bool style_property_justify_content(const StylePropertyParse *parse)
{
    char resolved[32];
    unsigned value = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_justify_content_keyword(resolved, &value)) {
        parse->style->justify_content = (JustifyContent) value;
        *parse->mask |= S_JUSTIFY_CONTENT;
    }
    return true;
}

/* The 2009 WebKit box syntax remains common in mobile stylesheets, chiefly
   as the layout half of multiline line-clamp.  Map its bounded one-axis
   model onto the existing flex implementation instead of carrying a second
   layout algorithm.  The aliases intentionally share the canonical masks:
   a later modern declaration therefore wins in the ordinary cascade. */
static bool style_property_webkit_box_align(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    const char *mapped = strcmp(resolved, "start") == 0 ? "flex-start"
        : (strcmp(resolved, "end") == 0 ? "flex-end" : resolved);
    StylePropertyParse alias = *parse;
    alias.value = mapped;
    alias.value_length = strlen(mapped);
    return style_property_align_items(&alias);
}

static bool style_property_webkit_box_pack(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    const char *mapped = strcmp(resolved, "start") == 0 ? "flex-start"
        : (strcmp(resolved, "end") == 0 ? "flex-end"
           : (strcmp(resolved, "justify") == 0 ? "space-between" : resolved));
    StylePropertyParse alias = *parse;
    alias.value = mapped;
    alias.value_length = strlen(mapped);
    return style_property_justify_content(&alias);
}

static bool style_property_webkit_box_flex(const StylePropertyParse *parse)
{
    return style_property_flex_grow(parse);
}

static bool style_property_webkit_box_lines(const StylePropertyParse *parse)
{
    char resolved[16];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    const char *mapped = strcmp(resolved, "multiple") == 0 ? "wrap"
        : (strcmp(resolved, "single") == 0 ? "nowrap" : resolved);
    StylePropertyParse alias = *parse;
    alias.value = mapped;
    alias.value_length = strlen(mapped);
    return style_property_flex_wrap(&alias);
}

static bool style_property_webkit_box_orient(const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    bool vertical = strcmp(resolved, "vertical") == 0
        || strcmp(resolved, "block-axis") == 0;
    bool horizontal = strcmp(resolved, "horizontal") == 0
        || strcmp(resolved, "inline-axis") == 0;
    if (!vertical && !horizontal) return true;
    bool reverse = parse->style->flex_direction == FLEX_ROW_REVERSE
        || parse->style->flex_direction == FLEX_COLUMN_REVERSE;
    parse->style->flex_direction = vertical
        ? (reverse ? FLEX_COLUMN_REVERSE : FLEX_COLUMN)
        : (reverse ? FLEX_ROW_REVERSE : FLEX_ROW);
    *parse->mask |= S_FLEX_DIRECTION;
    return true;
}

static bool style_property_webkit_box_direction(const StylePropertyParse *parse)
{
    char resolved[16];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    if (strcmp(resolved, "normal") != 0 && strcmp(resolved, "reverse") != 0) {
        return true;
    }
    bool column = parse->style->flex_direction == FLEX_COLUMN
        || parse->style->flex_direction == FLEX_COLUMN_REVERSE;
    bool reverse = strcmp(resolved, "reverse") == 0;
    parse->style->flex_direction = column
        ? (reverse ? FLEX_COLUMN_REVERSE : FLEX_COLUMN)
        : (reverse ? FLEX_ROW_REVERSE : FLEX_ROW);
    *parse->mask |= S_FLEX_DIRECTION;
    return true;
}

static bool style_property_webkit_box_ordinal_group(
    const StylePropertyParse *parse)
{
    char resolved[24];
    if (!style_resolve_value(parse->sheet, parse->value, parse->value_length,
                             resolved, sizeof(resolved), 0)) return true;
    char *end = NULL;
    unsigned long group = strtoul(resolved, &end, 10);
    while (end != NULL && isspace((unsigned char) *end)) end++;
    if (end == resolved || end == NULL || *end != '\0' || group == 0) {
        return true;
    }
    if (group - 1u > (unsigned long) INT_MAX) group = (unsigned long) INT_MAX + 1u;
    parse->style->order = (int) (group - 1u);
    *parse->mask_high |= S2_ORDER;
    return true;
}

static bool style_property_place_self(const StylePropertyParse *parse)
{
    char resolved[64];
    unsigned align = 0, justify = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_alignment_pair(
               resolved, style_parse_align_self_keyword,
               style_parse_justify_self_keyword, &align, &justify)) {
        parse->style->align_self = (AlignSelf) align;
        computed_style_set_justify_self(
            parse->style, (AlignSelf) justify);
        *parse->mask_high |= S2_ALIGN_SELF | S2_JUSTIFY_SELF;
    }
    return true;
}

static bool style_property_place_content(const StylePropertyParse *parse)
{
    char resolved[64];
    unsigned align = 0, justify = 0;
    if (style_resolve_value(parse->sheet, parse->value, parse->value_length,
                            resolved, sizeof(resolved), 0)
        && style_parse_alignment_pair(
               resolved, style_parse_align_content_keyword,
               style_parse_justify_content_keyword, &align, &justify)) {
        parse->style->align_content = (JustifyContent) align;
        parse->style->justify_content = (JustifyContent) justify;
        *parse->mask_high |= S2_ALIGN_CONTENT;
        *parse->mask |= S_JUSTIFY_CONTENT;
    }
    return true;
}

static bool style_property_mask_image(const StylePropertyParse *parse)
{
    if (style_parse_mask_image_layers(
            parse->sheet, parse->value, parse->value_length,
            parse->style)) {
        *parse->mask |= S_MASK_IMAGE;
    }
    return true;
}

static bool style_property_mask(const StylePropertyParse *parse)
{
    bool parsed = style_parse_mask_shorthand(
            parse->sheet, parse->value, parse->value_length,
            parse->style);
    if (parsed) {
        *parse->mask |= S_MASK_IMAGE;
        *parse->mask_high |= S2_MASK_SIZE | S2_MASK_POSITION
                             | S2_MASK_REPEAT;
    }
    return true;
}

static bool style_property_mask_size(const StylePropertyParse *parse)
{
    if (style_parse_mask_layer_size(
            parse->sheet, parse->value, parse->value_length,
            parse->style)) {
        *parse->mask_high |= S2_MASK_SIZE;
    }
    return true;
}

static bool style_property_mask_position(const StylePropertyParse *parse)
{
    if (style_parse_mask_layer_position(
            parse->sheet, parse->value, parse->value_length,
            parse->style)) {
        *parse->mask_high |= S2_MASK_POSITION;
    }
    return true;
}

static bool style_property_mask_repeat(const StylePropertyParse *parse)
{
    if (style_parse_mask_layer_repeat(
            parse->sheet, parse->value, parse->value_length,
            parse->style)) {
        *parse->mask_high |= S2_MASK_REPEAT;
    }
    return true;
}

static bool style_property_border_spacing(const StylePropertyParse *parse)
{
    char resolved[64];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) return true;
    char *at = resolved;
    while (isspace((unsigned char) *at)) at++;
    char *end = NULL;
    double first = strtod(at, &end);
    if (end == at || first < 0.0 || !isfinite(first)) return true;
    if (strncasecmp(end, "px", 2) == 0) end += 2;
    else if (first != 0.0) return true;
    while (isspace((unsigned char) *end)) end++;
    double second = first;
    if (*end != '\0') {
        char *second_end = NULL;
        second = strtod(end, &second_end);
        if (second_end == end || second < 0.0 || !isfinite(second)) {
            return true;
        }
        if (strncasecmp(second_end, "px", 2) == 0) second_end += 2;
        else if (second != 0.0) return true;
        while (isspace((unsigned char) *second_end)) second_end++;
        if (*second_end != '\0') return true;
    }
    StylePaintStack stack = {0};
    uint8_t old_id = computed_style_paint_stack_id(parse->style);
    const StylePaintStack *old = stylesheet_paint_stack(
        parse->sheet, old_id);
    if (old != NULL) stack = *old;
    unsigned x = (unsigned) (first + 0.5);
    unsigned y = (unsigned) (second + 0.5);
    stack.table_spacing_x = (uint8_t) (x > 255u ? 255u : x);
    stack.table_spacing_y = (uint8_t) (y > 255u ? 255u : y);
    stack.components |= STYLE_PAINT_COMPONENT_TABLE_SPACING;
    uint8_t id = 0;
    StylePaintInternResult result =
        style_intern_paint_stack(parse->sheet, &stack, &id);
    if (result == STYLE_PAINT_INTERN_RETAINED) {
        computed_style_set_paint_stack_id(parse->style, id);
        *parse->mask_high |= S2_BACKGROUND_BOX;
    }
    return result == STYLE_PAINT_INTERN_RETAINED
        || result == STYLE_PAINT_INTERN_LIMIT;
}

static bool style_property_list_style(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[96];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            bool shorthand = parse->name_length == 10;
            bool position_only = parse->name_length == 19;
            if (shorthand || position_only) {
                if (strstr(resolved, "inside") != NULL) {
                    style->list_style_inside = true;
                } else if (strstr(resolved, "outside") != NULL
                           || position_only) {
                    style->list_style_inside = false;
                }
                *parse->mask_high |= S2_LIST_STYLE_POSITION;
            }
            if (!position_only) {
                ListStyleType type = LIST_STYLE_AUTO;
                if (strstr(resolved, "none") != NULL) {
                    type = LIST_STYLE_NONE;
                } else if (strstr(resolved, "decimal-leading-zero") != NULL) {
                    type = LIST_STYLE_DECIMAL_LEADING_ZERO;
                } else if (strstr(resolved, "lower-alpha") != NULL
                           || strstr(resolved, "lower-latin") != NULL) {
                    type = LIST_STYLE_LOWER_ALPHA;
                } else if (strstr(resolved, "upper-alpha") != NULL
                           || strstr(resolved, "upper-latin") != NULL) {
                    type = LIST_STYLE_UPPER_ALPHA;
                } else if (strstr(resolved, "lower-roman") != NULL) {
                    type = LIST_STYLE_LOWER_ROMAN;
                } else if (strstr(resolved, "upper-roman") != NULL) {
                    type = LIST_STYLE_UPPER_ROMAN;
                } else if (strstr(resolved, "decimal") != NULL) {
                    type = LIST_STYLE_DECIMAL;
                } else if (strstr(resolved, "circle") != NULL) {
                    type = LIST_STYLE_CIRCLE;
                } else if (strstr(resolved, "square") != NULL) {
                    type = LIST_STYLE_SQUARE;
                } else if (strstr(resolved, "disc") != NULL) {
                    type = LIST_STYLE_DISC;
                }
                style->list_style_type = (uint8_t) type;
                style->list_style_none = type == LIST_STYLE_NONE;
                *mask |= S_LIST_STYLE;
                *parse->mask_high |= S2_LIST_STYLE_TYPE;
            }
        }
    
    return true;
}

static bool parse_counter_operations(
    Stylesheet *sheet, const char *value, size_t value_length,
    int default_value, uint8_t *operation_id)
{
    char resolved[160];
    if (!style_resolve_value(sheet, value, value_length, resolved,
                             sizeof(resolved), 0)) return true;
    const char *at = resolved;
    while (isspace((unsigned char) *at)) at++;
    if (strcmp(at, "none") == 0 || strcmp(at, "initial") == 0
        || strcmp(at, "unset") == 0 || strcmp(at, "revert") == 0
        || strcmp(at, "revert-layer") == 0) {
        *operation_id = 0;
        return true;
    }
    StyleCounterOperations operations = {0};
    while (*at != '\0'
           && operations.count < STYLE_COUNTER_OPERATION_LIMIT) {
        while (isspace((unsigned char) *at)) at++;
        const char *name = at;
        while (isalnum((unsigned char) *at) || *at == '-'
               || *at == '_') at++;
        size_t name_length = (size_t) (at - name);
        if (name_length == 0 || name_length >= 32
            || (name_length == 4 && memcmp(name, "none", 4) == 0)) {
            return true;
        }
        const char *stored = style_store_generated_text(
            sheet, name, name_length);
        if (stored == NULL) return false;
        while (isspace((unsigned char) *at)) at++;
        int parsed_value = default_value;
        if (*at == '+' || *at == '-' || isdigit((unsigned char) *at)) {
            char *end = NULL;
            long number = strtol(at, &end, 10);
            if (end != at) {
                if (number > INT_MAX) number = INT_MAX;
                if (number < INT_MIN) number = INT_MIN;
                parsed_value = (int) number;
                at = end;
            }
        }
        operations.operations[operations.count++] =
            (StyleCounterOperation) {
                .name = stored,
                .value = parsed_value
            };
    }
    if (*at != '\0') {
        while (isspace((unsigned char) *at)) at++;
        if (*at != '\0') return true;
    }
    uint8_t id = style_store_counter_operations(sheet, &operations);
    if (operations.count != 0 && id == 0) return false;
    *operation_id = id;
    return true;
}

static bool style_property_counter_reset(const StylePropertyParse *parse)
{
    if (!parse_counter_operations(
            parse->sheet, parse->value, parse->value_length, 0,
            &parse->style->counter_reset_id)) return false;
    *parse->mask_high |= S2_COUNTER_RESET;
    return true;
}

static bool style_property_counter_increment(const StylePropertyParse *parse)
{
    if (!parse_counter_operations(
            parse->sheet, parse->value, parse->value_length, 1,
            &parse->style->counter_increment_id)) return false;
    *parse->mask_high |= S2_COUNTER_INCREMENT;
    return true;
}

static bool style_property_counter_set(const StylePropertyParse *parse)
{
    if (!parse_counter_operations(
            parse->sheet, parse->value, parse->value_length, 0,
            &parse->style->counter_set_id)) return false;
    *parse->mask_high |= S2_COUNTER_SET;
    return true;
}
