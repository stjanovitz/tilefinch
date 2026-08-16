static const char *generated_content_string_end(const char *at)
{
    char quote = *at++;
    while (*at != '\0') {
        if (*at == '\\' && at[1] != '\0') {
            at += 2;
        } else if (*at++ == quote) {
            break;
        }
    }
    return at;
}

static bool compile_literal_generated_content(
    const char *resolved, char *output, size_t capacity, size_t *length)
{
    size_t written = 0;
    const char *at = resolved;
    while (*at != '\0') {
        while (isspace((unsigned char) *at)) at++;
        if (*at == '\0') break;
        if (*at == '\'' || *at == '"') {
            char decoded[STYLE_GENERATED_TEXT_CAPACITY];
            size_t decoded_length = style_decode_generated_text(
                at, decoded, sizeof(decoded));
            if (decoded_length > capacity - written - 1) return false;
            memcpy(output + written, decoded, decoded_length);
            written += decoded_length;
            at = generated_content_string_end(at);
            continue;
        }
        static const char no_open[] = "no-open-quote";
        static const char no_close[] = "no-close-quote";
        size_t token_length = 0;
        while (at[token_length] != '\0'
               && !isspace((unsigned char) at[token_length])) {
            token_length++;
        }
        if ((token_length == sizeof(no_open) - 1
             && memcmp(at, no_open, token_length) == 0)
            || (token_length == sizeof(no_close) - 1
                && memcmp(at, no_close, token_length) == 0)) {
            at += token_length;
            continue;
        }
        /* attr(), counter(s)(), quotes, URLs, and newer content tokens are
           evaluated against the originating node at layout time. */
        return false;
    }
    output[written] = '\0';
    *length = written;
    return true;
}

static bool style_property_content(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[96];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->generated_content = strcmp(resolved, "none") != 0
                                       && strcmp(resolved, "normal") != 0;
            style->generated_attr = NULL;
            style->generated_attr_length = 0;
            style->generated_expression = false;
            char decoded[STYLE_GENERATED_TEXT_CAPACITY];
            const char *attribute = resolved;
            size_t attribute_length = strlen(resolved);
            trim(&attribute, &attribute_length);
            if (style->generated_content && attribute_length > 6
                && memcmp(attribute, "attr(", 5) == 0
                && attribute[attribute_length - 1] == ')') {
                attribute += 5;
                attribute_length -= 6;
                trim(&attribute, &attribute_length);
                bool valid = attribute_length != 0
                             && attribute_length < 32;
                for (size_t i = 0; valid && i < attribute_length; i++) {
                    unsigned char character =
                        (unsigned char) attribute[i];
                    valid = isalnum(character) || character == '-'
                            || character == '_' || character == ':';
                }
                style->generated_attr = valid ? style_store_generated_text(
                    sheet, attribute, attribute_length) : NULL;
                style->generated_attr_length =
                    style->generated_attr == NULL
                    ? 0 : (uint8_t) attribute_length;
            }
            size_t decoded_length = 0;
            bool literal = style->generated_content
                && style->generated_attr == NULL
                && compile_literal_generated_content(
                    resolved, decoded, sizeof(decoded), &decoded_length);
            style->generated_text_length = literal
                ? (uint8_t) decoded_length : 0;
            style->generated_text = style->generated_text_length == 0
                ? NULL : style_store_generated_text(
                      sheet, decoded, style->generated_text_length);
            if (style->generated_content && style->generated_attr == NULL
                && !literal) {
                size_t expression_length = strlen(resolved);
                if (expression_length < STYLE_GENERATED_TEXT_CAPACITY) {
                    style->generated_text = style_store_generated_text(
                        sheet, resolved, expression_length);
                    style->generated_text_length =
                        style->generated_text == NULL
                        ? 0 : (uint8_t) expression_length;
                    style->generated_expression =
                        style->generated_text != NULL;
                }
            }
            if (style->generated_text == NULL) {
                style->generated_text_length = 0;
                style->generated_expression = false;
            }
            *mask |= S_CONTENT;
        }
    
    return true;
}

static bool style_property_clip_path(const StylePropertyParse *parse)
{
    char resolved[96];
    if (!style_resolve_value(parse->sheet, parse->value,
                             parse->value_length, resolved,
                             sizeof(resolved), 0)) {
        return true;
    }
    char *text = resolved;
    while (isspace((unsigned char) *text)) text++;
    uint16_t state = 0;
    if (strcmp(text, "none") == 0 || strcmp(text, "initial") == 0
        || strcmp(text, "unset") == 0 || strcmp(text, "revert") == 0
        || strcmp(text, "revert-layer") == 0) {
        state = STYLE_CLIP_PATH_NONE;
    } else if (strncasecmp(text, "circle(", 7) == 0) {
        state = STYLE_CLIP_PATH_CIRCLE
            | (UINT16_C(63) << STYLE_CLIP_PATH_RADIUS_SHIFT);
    } else if (strncasecmp(text, "inset(", 6) == 0) {
        char *at = text + 6;
        while (isspace((unsigned char) *at)) at++;
        char *unit = NULL;
        double value = strtod(at, &unit);
        if (unit == at || !isfinite(value) || value < 0.0) return true;
        bool percent = *unit == '%';
        if (percent) unit++;
        else if (unit[0] == 'p' && unit[1] == 'x') unit += 2;
        else if (value != 0.0) return true;
        unsigned inset = (unsigned) (value + 0.5);
        if (inset > 127u) inset = 127u;
        unsigned radius = 0;
        char *round = strstr(unit, "round");
        if (round != NULL) {
            round += 5;
            while (isspace((unsigned char) *round)) round++;
            char *radius_unit = NULL;
            double parsed = strtod(round, &radius_unit);
            if (radius_unit != round && isfinite(parsed) && parsed > 0.0) {
                radius = (unsigned) (parsed + 0.5);
                if (radius > 63u) radius = 63u;
            }
        }
        state = (uint16_t) (
            STYLE_CLIP_PATH_INSET
            | (inset << STYLE_CLIP_PATH_INSET_SHIFT)
            | (percent ? STYLE_CLIP_PATH_INSET_PERCENT : 0u)
            | (radius << STYLE_CLIP_PATH_RADIUS_SHIFT));
    } else {
        /* polygon(), path() and geometry boxes are not approximated: an
           incorrect security/visibility clip is worse than dropping it. */
        return true;
    }
    parse->style->clip_path_state = state;
    *parse->mask_high |= S2_CLIP_PATH;
    return true;
}

/* Insets accept the same bounded <length-percentage> programs as sizing and
   spacing. Keeping the compact integer-percentage representation means this
   adds no bytes to ComputedStyle, while mixed calc() expressions retain a
   program reference until the containing block is known. */
static bool style_parse_inset_component(
    Stylesheet *sheet, const char *text, size_t length,
    int *value, bool *automatic, bool *percent)
{
    if (value == NULL || automatic == NULL || percent == NULL) return false;
    *automatic = style_length_is_auto(sheet, text, length);
    *percent = false;
    *value = 0;
    if (*automatic) return true;
    StyleLength parsed = 0;
    if (!parse_dimension_length(sheet, text, length, &parsed, percent)) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool style_parse_inset_components(
    Stylesheet *sheet, const char *text, size_t length, size_t capacity,
    int values[4], bool automatic[4], bool percentages[4], size_t *count)
{
    if (text == NULL || values == NULL || automatic == NULL
        || percentages == NULL || count == NULL || capacity == 0
        || capacity > 4) return false;
    *count = 0;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) text[at])) at++;
        if (at == length) break;
        if (*count == capacity) return false;
        size_t end = at;
        unsigned parentheses = 0;
        char quote = '\0';
        bool escaped = false;
        while (end < length) {
            char character = text[end];
            if (quote != '\0') {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == quote) quote = '\0';
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '(') {
                parentheses++;
            } else if (character == ')' && parentheses != 0) {
                parentheses--;
            } else if (parentheses == 0
                       && isspace((unsigned char) character)) {
                break;
            }
            end++;
        }
        if (end == at || quote != '\0' || parentheses != 0
            || !style_parse_inset_component(
                   sheet, text + at, end - at, &values[*count],
                   &automatic[*count], &percentages[*count])) {
            return false;
        }
        (*count)++;
        at = end;
    }
    return *count != 0;
}

static bool style_property_inset(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            int values[4] = {0};
        bool automatic[4] = {false};
        bool percentages[4] = {false};
        size_t count = 0;
        if (style_parse_inset_components(
                sheet, value, value_length, 4, values, automatic,
                percentages, &count)) {
            size_t right = count > 1 ? 1 : 0;
            size_t bottom = count > 2 ? 2 : 0;
            size_t left = count > 3 ? 3 : right;
            style->has_top = !automatic[0];
            style->has_right = !automatic[right];
            style->has_bottom = !automatic[bottom];
            style->has_left = !automatic[left];
            style->top = values[0]; style->right = values[right];
            style->bottom = values[bottom]; style->left = values[left];
            style->inset_percent_mask =
                (percentages[0] ? STYLE_INSET_TOP_PERCENT : 0)
                | (percentages[right]
                   ? STYLE_INSET_RIGHT_PERCENT : 0)
                | (percentages[bottom]
                   ? STYLE_INSET_BOTTOM_PERCENT : 0)
                | (percentages[left]
                   ? STYLE_INSET_LEFT_PERCENT : 0);
            *mask |= S_TOP | S_RIGHT | S_BOTTOM | S_LEFT;
        }
    
    return true;
}

static bool style_property_inset_inline(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        int values_storage[4] = {0};
        bool automatic_storage[4] = {false};
        bool percentages_storage[4] = {false};
        size_t count = 0;
        if (style_parse_inset_components(
                sheet, value, value_length, 2, values_storage,
                automatic_storage, percentages_storage, &count)) {
            if (count == 1) {
                values_storage[1] = values_storage[0];
                automatic_storage[1] = automatic_storage[0];
                percentages_storage[1] = percentages_storage[0];
            }
            int *values = values_storage;
            bool *automatic = automatic_storage;
            bool *percentages = percentages_storage;
            bool inline_axis = span_equal(
                name, name_length, "inset-inline");
            unsigned start, end;
            style_logical_edge_indices(
                sheet, style, inline_axis, &start, &end);
            unsigned edges[2] = {start, end};
            for (size_t i = 0; i < 2; i++) {
                unsigned edge = edges[i];
                uint8_t percent_bit;
                uint64_t property_bit;
                int *destination;
                if (edge == STYLE_PHYSICAL_TOP) {
                    destination = &style->top;
                    style->has_top = !automatic[i];
                    percent_bit = STYLE_INSET_TOP_PERCENT;
                    property_bit = S_TOP;
                } else if (edge == STYLE_PHYSICAL_RIGHT) {
                    destination = &style->right;
                    style->has_right = !automatic[i];
                    percent_bit = STYLE_INSET_RIGHT_PERCENT;
                    property_bit = S_RIGHT;
                } else if (edge == STYLE_PHYSICAL_BOTTOM) {
                    destination = &style->bottom;
                    style->has_bottom = !automatic[i];
                    percent_bit = STYLE_INSET_BOTTOM_PERCENT;
                    property_bit = S_BOTTOM;
                } else {
                    destination = &style->left;
                    style->has_left = !automatic[i];
                    percent_bit = STYLE_INSET_LEFT_PERCENT;
                    property_bit = S_LEFT;
                }
                *destination = values[i];
                style->inset_percent_mask &= (uint8_t) ~percent_bit;
                if (percentages[i]) {
                    style->inset_percent_mask |= percent_bit;
                }
                *mask |= property_bit;
            }
        }
    
    return true;
}

static bool style_property_inset_inline_start(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *name = parse->name;
    size_t name_length = parse->name_length;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
        bool automatic = false;
        bool percent = false;
        int pixels = 0;
        if (!style_parse_inset_component(
                sheet, value, value_length, &pixels, &automatic, &percent)) {
            return true;
        }
        bool present = !automatic;
        bool inline_axis = span_equal(
            name, name_length, "inset-inline-start")
            || span_equal(name, name_length, "inset-inline-end");
        bool start_side = span_equal(
            name, name_length, "inset-inline-start")
            || span_equal(name, name_length, "inset-block-start");
        unsigned start, end;
        style_logical_edge_indices(
            sheet, style, inline_axis, &start, &end);
        unsigned edge = start_side ? start : end;
        if (edge == STYLE_PHYSICAL_LEFT) {
            style->has_left = present; style->left = pixels;
            style->inset_percent_mask &= ~STYLE_INSET_LEFT_PERCENT;
            if (percent) style->inset_percent_mask |=
                STYLE_INSET_LEFT_PERCENT;
            *mask |= S_LEFT;
        } else if (edge == STYLE_PHYSICAL_RIGHT) {
            style->has_right = present; style->right = pixels;
            style->inset_percent_mask &= ~STYLE_INSET_RIGHT_PERCENT;
            if (percent) style->inset_percent_mask |=
                STYLE_INSET_RIGHT_PERCENT;
            *mask |= S_RIGHT;
        } else if (edge == STYLE_PHYSICAL_TOP) {
            style->has_top = present; style->top = pixels;
            style->inset_percent_mask &= ~STYLE_INSET_TOP_PERCENT;
            if (percent) style->inset_percent_mask |=
                STYLE_INSET_TOP_PERCENT;
            *mask |= S_TOP;
        } else {
            style->has_bottom = present; style->bottom = pixels;
            style->inset_percent_mask &= ~STYLE_INSET_BOTTOM_PERCENT;
            if (percent) style->inset_percent_mask |=
                STYLE_INSET_BOTTOM_PERCENT;
            *mask |= S_BOTTOM;
        }
    
    return true;
}

static bool style_property_top(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            bool automatic = false;
        bool percent = false;
        int parsed = 0;
        if (!style_parse_inset_component(
                sheet, value, value_length, &parsed, &automatic, &percent)) {
            return true;
        }
        style->has_top = !automatic;
        style->top = parsed;
        style->inset_percent_mask &= ~STYLE_INSET_TOP_PERCENT;
        if (percent) style->inset_percent_mask |=
            STYLE_INSET_TOP_PERCENT;
        *mask |= S_TOP;
    
    return true;
}

static bool style_property_right(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            bool automatic = false;
        bool percent = false;
        int parsed = 0;
        if (!style_parse_inset_component(
                sheet, value, value_length, &parsed, &automatic, &percent)) {
            return true;
        }
        style->has_right = !automatic;
        style->right = parsed;
        style->inset_percent_mask &= ~STYLE_INSET_RIGHT_PERCENT;
        if (percent) style->inset_percent_mask |=
            STYLE_INSET_RIGHT_PERCENT;
        *mask |= S_RIGHT;
    
    return true;
}

static bool style_property_bottom(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            bool automatic = false;
        bool percent = false;
        int parsed = 0;
        if (!style_parse_inset_component(
                sheet, value, value_length, &parsed, &automatic, &percent)) {
            return true;
        }
        style->has_bottom = !automatic;
        style->bottom = parsed;
        style->inset_percent_mask &= ~STYLE_INSET_BOTTOM_PERCENT;
        if (percent) style->inset_percent_mask |=
            STYLE_INSET_BOTTOM_PERCENT;
        *mask |= S_BOTTOM;
    
    return true;
}

static bool style_property_left(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            bool automatic = false;
        bool percent = false;
        int parsed = 0;
        if (!style_parse_inset_component(
                sheet, value, value_length, &parsed, &automatic, &percent)) {
            return true;
        }
        style->has_left = !automatic;
        style->left = parsed;
        style->inset_percent_mask &= ~STYLE_INSET_LEFT_PERCENT;
        if (percent) style->inset_percent_mask |=
            STYLE_INSET_LEFT_PERCENT;
        *mask |= S_LEFT;
    
    return true;
}

static bool style_property_clip(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            bool empty = false;
        if (parse_empty_clip_rect(sheet, value, value_length,
                                  &empty)) {
            style->clip_rect_empty = empty;
            *mask_high |= S2_CLIP_RECT;
        }
    
    return true;
}

static bool style_property_visibility(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->visibility_hidden = strcmp(resolved, "hidden") == 0
                                       || strcmp(resolved, "collapse") == 0;
            *mask |= S_VISIBILITY;
        }
    
    return true;
}

static bool style_property_position(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask = parse->mask;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->fixed_position = strcmp(resolved, "fixed") == 0;
            style->out_of_flow = strcmp(resolved, "absolute") == 0;
            style->relative_position = strcmp(resolved, "relative") == 0;
            style->sticky_position = strcmp(resolved, "sticky") == 0
                                     || strcmp(resolved,
                                               "-webkit-sticky") == 0;
            *mask |= S_POSITION;
        }
    
    return true;
}

static bool style_property_float(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            style->float_mode = strcmp(resolved, "left") == 0
                || strcmp(resolved, "inline-start") == 0
                ? FLOAT_LEFT
                : (strcmp(resolved, "right") == 0
                   || strcmp(resolved, "inline-end") == 0
                   ? FLOAT_RIGHT : FLOAT_NONE);
            *mask_high |= S2_FLOAT;
        }
    
    return true;
}

static bool style_property_clear(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    uint64_t *mask_high = parse->mask_high;
            char resolved[32];
        if (style_resolve_value(sheet, value, value_length, resolved,
                          sizeof(resolved), 0)) {
            if (strcmp(resolved, "left") == 0
                || strcmp(resolved, "inline-start") == 0) {
                style->clear_mode = CLEAR_LEFT;
            } else if (strcmp(resolved, "right") == 0
                       || strcmp(resolved, "inline-end") == 0) {
                style->clear_mode = CLEAR_RIGHT;
            } else if (strcmp(resolved, "both") == 0) {
                style->clear_mode = CLEAR_BOTH;
            } else {
                style->clear_mode = CLEAR_NONE;
            }
            *mask_high |= S2_CLEAR;
        }
    
    return true;
}

static bool style_radius_tokens(const Stylesheet *sheet,
                                const char *text, size_t length,
                                int radii[4], size_t *count)
{
    char resolved[96];
    if (count == NULL
        || !style_resolve_value(
            sheet, text, length, resolved, sizeof(resolved), 0)
        || strchr(resolved, '/') != NULL) return false;
    *count = 0;
    size_t resolved_length = strlen(resolved);
    size_t at = 0;
    for (; at < resolved_length && *count < 4;) {
        while (at < resolved_length
               && isspace((unsigned char) resolved[at])) at++;
        size_t end = at;
        while (end < resolved_length
               && !isspace((unsigned char) resolved[end])) end++;
        if (end == at) break;
        bool percent = false;
        int radius = style_parse_length(
            sheet, resolved + at, end - at, -1, &percent);
        if (radius < 0 || percent) return false;
        radii[(*count)++] = radius;
        at = end;
    }
    while (at < resolved_length
           && isspace((unsigned char) resolved[at])) at++;
    if (*count == 0) return false;
    if (*count == 1) {
        radii[1] = radii[2] = radii[3] = radii[0];
    } else if (*count == 2) {
        radii[2] = radii[0];
        radii[3] = radii[1];
    } else if (*count == 3) {
        radii[3] = radii[1];
    }
    return at == resolved_length;
}

static bool style_property_border_radius(const StylePropertyParse *parse)
{
    int radii[4] = {0};
    size_t count = 0;
    if (!style_radius_tokens(parse->sheet, parse->value,
                             parse->value_length, radii, &count)) {
        return true;
    }
    parse->style->border_radius = radii[0] == radii[1]
                                  && radii[0] == radii[2]
                                  && radii[0] == radii[3]
        ? radii[0]
        : style_border_radius_pack(
              radii[0], radii[1], radii[2], radii[3]);
    *parse->mask |= S_BORDER_RADIUS;
    return true;
}

static bool style_property_border_corner_radius(
    const StylePropertyParse *parse)
{
    int radii[4] = {0};
    size_t count = 0;
    if (!style_radius_tokens(parse->sheet, parse->value,
                             parse->value_length, radii, &count)
        || count > 2 || radii[0] != radii[1]) {
        /* Elliptical corners need two radii in every raster predicate. Do not
           collapse them to a circle and silently alter authored geometry. */
        return true;
    }
    size_t corner = parse->name[7] == 't'
        ? (parse->name[11] == 'l' ? 0u : 1u)
        : (parse->name[14] == 'r' ? 2u : 3u);
    uint8_t present = (*parse->mask & S_BORDER_RADIUS) != 0
        ? style_border_radius_present(parse->style->border_radius) : 0;
    parse->style->border_radius = style_border_radius_set_corner_present(
        parse->style->border_radius, present, corner, radii[0]);
    *parse->mask |= S_BORDER_RADIUS;
    return true;
}

/* An unparseable box-shadow leaves both style and mask untouched, so the P1
   invalid-value fallback keeps whatever shadow was cascaded before it. */
static bool style_property_box_shadow(const StylePropertyParse *parse)
{
    StyleBoxShadow shadows[STYLE_BOX_SHADOW_LIMIT] = {0};
    uint8_t count = 0;
    if (!style_parse_box_shadow(parse->sheet, parse->value,
                                parse->value_length, shadows, &count)) {
        return true;
    }
    StylePaintStack stack = style_paint_stack_copy(
        parse->sheet, parse->style);
    memset(stack.box_shadows, 0, sizeof(stack.box_shadows));
    memcpy(stack.box_shadows, shadows, sizeof(shadows));
    stack.box_shadow_count = count;
    stack.components |= STYLE_PAINT_COMPONENT_BOX_SHADOW;
    bool retained = false;
    if (!style_apply_paint_stack(
            parse->sheet, parse->style, &stack, &retained)) return false;
    if (!retained) return true;
    *parse->mask_high |= S2_BOX_SHADOW;
    return true;
}

static bool style_property_text_shadow(const StylePropertyParse *parse)
{
    const char *value = parse->value;
    size_t length = parse->value_length;
    while (length != 0 && isspace((unsigned char) *value)) {
        value++;
        length--;
    }
    while (length != 0
           && isspace((unsigned char) value[length - 1])) {
        length--;
    }
    /* text-shadow is inherited. Leaving inherit/unset absent from this
       declaration preserves the inherited paint component already seeded
       by default_style(). */
    if (span_case_equal(value, length, "inherit")
        || span_case_equal(value, length, "unset")) {
        return true;
    }

    StyleBoxShadow shadows[STYLE_BOX_SHADOW_LIMIT] = {0};
    uint8_t count = 0;
    bool reset = span_case_equal(value, length, "initial");
    if (!reset
        && !style_parse_text_shadow(
               parse->sheet, value, length, shadows, &count)) {
        return true;
    }
    StylePaintStack stack = {0};
    const StylePaintStack *current = stylesheet_paint_stack(
        parse->sheet, computed_style_paint_stack_id(parse->style));
    if (current != NULL) stack = *current;
    memset(stack.text_shadows, 0, sizeof(stack.text_shadows));
    memcpy(stack.text_shadows, shadows, sizeof(shadows));
    stack.text_shadow_count = count;
    stack.components |= STYLE_PAINT_COMPONENT_TEXT_SHADOW;
    uint8_t id = 0;
    StylePaintInternResult result =
        style_intern_paint_stack(parse->sheet, &stack, &id);
    if (result == STYLE_PAINT_INTERN_LIMIT) return true;
    if (result != STYLE_PAINT_INTERN_RETAINED) return false;
    computed_style_set_paint_stack_id(parse->style, id);
    /* Optional paint components share one presence lane; apply_paint_values
       merges only the component bits present in this declaration. */
    *parse->mask_high |= S2_BACKGROUND_BOX;
    return true;
}

static bool style_property_background_repeat(const StylePropertyParse *parse)
{
    Stylesheet *sheet = parse->sheet;
    const char *value = parse->value;
    size_t value_length = parse->value_length;
    ComputedStyle *style = parse->style;
    char resolved[48];
    if (!style_resolve_value(sheet, value, value_length, resolved,
                             sizeof(resolved), 0)) return true;
    uint8_t cleared = (uint8_t) ~(STYLE_BACKGROUND_NO_REPEAT_X
                                  | STYLE_BACKGROUND_NO_REPEAT_Y);
    style->background_size_flags &= cleared;
    if (strcmp(resolved, "no-repeat") == 0) {
        style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_X
                                        | STYLE_BACKGROUND_NO_REPEAT_Y;
    } else if (strcmp(resolved, "repeat-x") == 0) {
        style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_Y;
    } else if (strcmp(resolved, "repeat-y") == 0) {
        style->background_size_flags |= STYLE_BACKGROUND_NO_REPEAT_X;
    }
    *parse->mask_high |= S2_BACKGROUND_REPEAT;
    return true;
}
