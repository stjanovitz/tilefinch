static void register_layer_list(Stylesheet *sheet, const char *text,
                                size_t length)
{
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && text[end] != ',') end++;
        (void) register_layer(sheet, text + at, end - at);
        at = end + (end < length);
    }
}

static StyleWebFonts *stylesheet_web_fonts_get(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return NULL;
    if (sheet->web_fonts != NULL) return sheet->web_fonts;
    sheet->web_fonts = budget_calloc(sheet->budget, 1,
                                     sizeof(*sheet->web_fonts));
    return sheet->web_fonts;
}

static bool css_header_rule(const char *header, size_t header_length,
                            const char *keyword,
                            const char **remainder,
                            size_t *remainder_length)
{
    size_t keyword_length = strlen(keyword);
    if (header_length < keyword_length) return false;
    for (size_t i = 0; i < keyword_length; i++) {
        if (tolower((unsigned char) header[i])
            != tolower((unsigned char) keyword[i])) return false;
    }
    if (header_length > keyword_length) {
        unsigned char boundary = (unsigned char) header[keyword_length];
        if (!(isspace(boundary) || boundary == '(')) return false;
    }
    if (remainder != NULL) *remainder = header + keyword_length;
    if (remainder_length != NULL) {
        *remainder_length = header_length - keyword_length;
    }
    return true;
}

static bool css_unescape_value(const char *text, size_t length,
                               char *output, size_t capacity)
{
    if (text == NULL || output == NULL || capacity == 0) return false;
    trim(&text, &length);
    if (length >= 2 && (text[0] == '\'' || text[0] == '"')
        && text[length - 1] == text[0]) {
        text++;
        length -= 2;
    }
    size_t written = 0;
    for (size_t at = 0; at < length;) {
        unsigned char character = (unsigned char) text[at++];
        if (character == '\\' && at < length) {
            unsigned digit = 0;
            if (style_css_hex_digit((unsigned char) text[at], &digit)) {
                unsigned codepoint = 0;
                size_t digits = 0;
                while (at < length && digits < 6
                       && style_css_hex_digit((unsigned char) text[at], &digit)) {
                    codepoint = codepoint * 16u + digit;
                    at++;
                    digits++;
                }
                if (at < length
                    && isspace((unsigned char) text[at])) at++;
                if (codepoint == 0 || codepoint > 0x10ffffu
                    || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
                    codepoint = 0xfffdu;
                }
                if (!style_font_family_append_codepoint(
                        output, capacity, &written, codepoint)) return false;
                continue;
            }
            character = (unsigned char) text[at++];
            if (character == '\n' || character == '\r' || character == '\f') {
                continue;
            }
        }
        if (written + 1 >= capacity || character == 0) return false;
        output[written++] = (char) character;
    }
    if (written == 0) return false;
    output[written] = '\0';
    return true;
}

static size_t css_function_close(const char *text, size_t length,
                                 size_t open)
{
    int depth = 1;
    char quote = 0;
    for (size_t at = open + 1; at < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '\\' && at + 1 < length) at++;
        else if (character == '(') depth++;
        else if (character == ')' && --depth == 0) return at;
    }
    return length;
}

static bool css_function_at(const char *text, size_t length,
                            const char *name, size_t *open)
{
    size_t at = skip_css_space_and_comments(text, length, 0);
    size_t name_length = strlen(name);
    if (at + name_length > length) return false;
    for (size_t i = 0; i < name_length; i++) {
        if (tolower((unsigned char) text[at + i])
            != tolower((unsigned char) name[i])) return false;
    }
    at += name_length;
    while (at < length && isspace((unsigned char) text[at])) at++;
    if (at >= length || text[at] != '(') return false;
    *open = at;
    return true;
}

static bool web_font_format_supported(const char *text, size_t length)
{
    char format[32];
    if (!css_unescape_value(text, length, format, sizeof(format))) {
        return false;
    }
    for (size_t i = 0; format[i] != '\0'; i++) {
        format[i] = (char) tolower((unsigned char) format[i]);
    }
    return strcmp(format, "woff") == 0
        || strcmp(format, "truetype") == 0
        || strcmp(format, "opentype") == 0;
}

static bool web_font_source_item(const char *text, size_t length,
                                 char reference[2048])
{
    size_t url_open = 0;
    if (!css_function_at(text, length, "url", &url_open)) return false;
    size_t url_close = css_function_close(text, length, url_open);
    if (url_close == length
        || !css_unescape_value(text + url_open + 1,
                               url_close - url_open - 1,
                               reference, 2048)) return false;
    /* URL schemes are ASCII case-insensitive.  Retaining a DATA: URL would
       both violate the no-inline-font boundary and, more subtly, consume one
       of the two bounded fallback slots ahead of a usable network source. */
    if (strlen(reference) >= 5
        && style_font_span_equal(reference, 5, "data:")) return false;
    size_t tail = skip_css_space_and_comments(text, length, url_close + 1);
    if (tail == length) return true;
    size_t format_open = 0;
    if (!css_function_at(text + tail, length - tail, "format",
                         &format_open)) return false;
    format_open += tail;
    size_t format_close = css_function_close(text, length, format_open);
    if (format_close == length
        || !web_font_format_supported(text + format_open + 1,
                                      format_close - format_open - 1)) {
        return false;
    }
    return skip_css_space_and_comments(text, length, format_close + 1)
           == length;
}

static size_t web_font_sources(const char *text, size_t length,
                               char references[STYLE_WEB_FONT_SOURCE_LIMIT]
                                              [2048])
{
    size_t count = 0;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        int parentheses = 0;
        char quote = 0;
        while (end < length) {
            char character = text[end];
            if (quote != 0) {
                if (character == '\\' && end + 1 < length) end++;
                else if (character == quote) quote = 0;
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '/' && end + 1 < length
                       && text[end + 1] == '*') {
                end += 2;
                while (end + 1 < length
                       && !(text[end] == '*' && text[end + 1] == '/')) end++;
                if (end + 1 < length) end++;
            } else if (character == '(') {
                parentheses++;
            } else if (character == ')' && parentheses > 0) {
                parentheses--;
            } else if (character == ',' && parentheses == 0) {
                break;
            }
            end++;
        }
        char candidate[2048];
        if (web_font_source_item(text + at, end - at, candidate)) {
            bool duplicate = false;
            for (size_t i = 0; i < count; i++) {
                if (strcmp(references[i], candidate) == 0) duplicate = true;
            }
            if (!duplicate && count < STYLE_WEB_FONT_SOURCE_LIMIT) {
                memcpy(references[count], candidate,
                       strlen(candidate) + 1u);
                count++;
            }
        }
        at = end + (end < length);
    }
    return count;
}

static char *web_font_copy(Stylesheet *sheet, const char *text,
                           size_t *retained_bytes)
{
    if (sheet == NULL || sheet->budget == NULL || text == NULL) return NULL;
    size_t length = strlen(text);
    if (length >= 2048
        || (retained_bytes != NULL
            && *retained_bytes > SIZE_MAX - length - 1u)) return NULL;
    char *copy = budget_malloc(sheet->budget, length + 1u);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length + 1u);
    if (retained_bytes != NULL) {
        *retained_bytes += length + 1u;
    }
    return copy;
}

static bool web_font_referrer_policy_normalize(
    const char *policy,
    char normalized[STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY])
{
    static const char *known[] = {
        "no-referrer", "no-referrer-when-downgrade", "origin",
        "origin-when-cross-origin", "same-origin", "strict-origin",
        "strict-origin-when-cross-origin", "unsafe-url"
    };
    if (normalized == NULL) return false;
    normalized[0] = '\0';
    if (policy == NULL || policy[0] == '\0') return true;
    size_t length = strlen(policy);
    if (length >= STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY) return false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (!style_font_span_equal(policy, length, known[i])) continue;
        memcpy(normalized, known[i], strlen(known[i]) + 1u);
        return true;
    }
    return false;
}

static void register_font_face(Stylesheet *sheet, const char *declarations,
                               size_t length, const char *source_base_url,
                               const char *source_referrer_policy)
{
    StyleWebFonts *fonts = stylesheet_web_fonts_get(sheet);
    if (fonts == NULL) return;
    fonts->stats.declarations_discovered++;
    const char *family_value = NULL;
    size_t family_length = 0;
    const char *source_value = NULL;
    size_t source_length = 0;
    uint16_t weight = 400;
    bool normal_style = true;
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(declarations, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(declarations, length, at);
        size_t colon = at;
        while (colon < end && declarations[colon] != ':') colon++;
        if (colon < end) {
            const char *name = declarations + at;
            size_t name_length = colon - at;
            const char *value = declarations + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            if (style_font_span_equal(name, name_length, "font-family")) {
                family_value = value;
                family_length = value_length;
            } else if (style_font_span_equal(name, name_length, "src")) {
                source_value = value;
                source_length = value_length;
            } else if (style_font_span_equal(name, name_length, "font-weight")) {
                if (!style_parse_font_weight_component(value, value_length,
                                                 &weight)) {
                    normal_style = false;
                }
            } else if (style_font_span_equal(name, name_length, "font-style")
                       && !style_font_span_equal(value, value_length, "normal")) {
                normal_style = false;
            }
        }
        at = end + (end < length);
    }
    char family[STYLE_WEB_FONT_NAME_CAPACITY];
    char references[STYLE_WEB_FONT_SOURCE_LIMIT][2048];
    size_t source_count = source_value == NULL ? 0
        : web_font_sources(source_value, source_length, references);
    if (!normal_style || family_value == NULL || source_count == 0
        || !style_canonical_font_family_name(family_value, family_length,
                                       family)) {
        fonts->stats.unsupported_sources++;
        return;
    }
    size_t family_slot = fonts->family_count;
    for (size_t i = 0; i < fonts->family_count; i++) {
        if (strcmp(fonts->families[i].name, family) == 0) {
            family_slot = i;
            break;
        }
    }
    if (family_slot == fonts->family_count) {
        if (fonts->family_count == TILEFINCH_WEB_FONT_FAMILY_LIMIT) {
            fonts->stats.skipped_family_limit++;
            return;
        }
        memcpy(fonts->families[family_slot].name, family,
               strlen(family) + 1u);
        fonts->family_count++;
    }
    StyleWebFontFaceSource *destination = weight >= 600
        ? &fonts->families[family_slot].bold
        : &fonts->families[family_slot].regular;
    for (size_t source = 0; source < source_count; source++) {
        bool duplicate = false;
        for (size_t i = 0; i < destination->source_count; i++) {
            const char *retained_base = destination->source_bases[i];
            bool same_base = (retained_base == NULL
                              && source_base_url == NULL)
                || (retained_base != NULL && source_base_url != NULL
                    && strcmp(retained_base, source_base_url) == 0);
            bool same_policy = retained_base == NULL
                ? source_base_url == NULL
                : source_base_url != NULL
                  && source_referrer_policy != NULL
                  && strcmp(destination->source_referrer_policies[i],
                            source_referrer_policy) == 0;
            if (same_base && same_policy
                && strcmp(destination->references[i], references[source])
                       == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            fonts->stats.duplicate_sources++;
            continue;
        }
        if (destination->source_count == STYLE_WEB_FONT_SOURCE_LIMIT) {
            fonts->stats.skipped_source_limit++;
            continue;
        }
        char *reference = web_font_copy(
            sheet, references[source], &fonts->retained_source_bytes);
        char *base = NULL;
        if (reference != NULL && source_base_url != NULL) {
            base = web_font_copy(sheet, source_base_url,
                                 &fonts->retained_source_bytes);
        }
        if (reference == NULL
            || (source_base_url != NULL && base == NULL)) {
            if (reference != NULL) {
                size_t bytes = strlen(reference) + 1u;
                if (bytes <= fonts->retained_source_bytes) {
                    fonts->retained_source_bytes -= bytes;
                }
                budget_free(sheet->budget, reference);
            }
            fonts->stats.skipped_source_limit++;
            continue;
        }
        size_t index = destination->source_count++;
        destination->references[index] = reference;
        destination->source_bases[index] = base;
        if (source_base_url != NULL) {
            memcpy(destination->source_referrer_policies[index],
                   source_referrer_policy,
                   strlen(source_referrer_policy) + 1u);
        }
        fonts->stats.sources_selected++;
    }
}

static size_t css_selector_item_end(const char *text, size_t length,
                                    size_t begin)
{
    int round = 0;
    int square = 0;
    char quote = 0;
    for (size_t at = begin; at < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
        } else if (character == '\\' && at + 1 < length) {
            /* Utility-class selectors escape commas inside their literal
               class token (for example clamp(...\,...)). They are not
               selector-list separators. Keep the escape pair intact for the
               matcher and skip its punctuation here. */
            at++;
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '(') {
            round++;
        } else if (character == ')' && round > 0) {
            round--;
        } else if (character == '[') {
            square++;
        } else if (character == ']' && square > 0) {
            square--;
        } else if (character == ',' && round == 0 && square == 0) {
            return at;
        }
    }
    return length;
}

static bool selector_list_to_rules(StyleCssParseContext *context,
                                   const char *selectors,
                                   size_t selector_length,
                                   const char *declarations,
                                   size_t declaration_length)
{
    Stylesheet *sheet = context == NULL ? NULL : context->sheet;
    if (!style_parsed_ir_builder_record(
            context, selectors, selector_length,
            declarations, declaration_length)) return false;
    diagnose_declarations(sheet, declarations, declaration_length);
    StyleParsedRuleDeclarations declaration_set = {0};
    size_t parsed_selectors = 0;
    for (size_t at = 0; at < selector_length;) {
        if ((parsed_selectors++ & 7u) == 0
            && !tilefinch_platform_cooperate(
                "stylesheet-selectors", at)) return false;
        size_t end = css_selector_item_end(selectors, selector_length, at);
        if (!selector_to_rule(context, selectors + at, end - at,
                              declarations, declaration_length,
                              &declaration_set)) {
            return false;
        }
        at = end + (end < selector_length);
    }
    return true;
}

static bool nested_selector_expand_one(const char *parent,
                                       size_t parent_length,
                                       const char *child,
                                       size_t child_length,
                                       char output[STYLE_SELECTOR_CAPACITY])
{
    trim(&parent, &parent_length);
    trim(&child, &child_length);
    if (parent_length == 0 || child_length == 0) return false;
    size_t used = 0;
    bool replaced = false;
    char quote = 0;
    int square = 0;
    for (size_t at = 0; at < child_length; at++) {
        char character = child[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < child_length) {
                if (used + 2 >= STYLE_SELECTOR_CAPACITY) return false;
                output[used++] = character;
                output[used++] = child[++at];
                continue;
            }
            if (character == quote) quote = 0;
        } else if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == '[') {
            square++;
        } else if (character == ']' && square > 0) {
            square--;
        } else if (character == '&' && square == 0) {
            if (used + parent_length >= STYLE_SELECTOR_CAPACITY) return false;
            memcpy(output + used, parent, parent_length);
            used += parent_length;
            replaced = true;
            continue;
        }
        if (used + 1 >= STYLE_SELECTOR_CAPACITY) return false;
        output[used++] = character;
    }
    if (!replaced) {
        if (parent_length + 1 + child_length
            >= STYLE_SELECTOR_CAPACITY) return false;
        memmove(output + parent_length + 1, output, used);
        memcpy(output, parent, parent_length);
        output[parent_length] = ' ';
        used += parent_length + 1;
    }
    output[used] = '\0';
    return quote == 0 && square == 0;
}

static bool parse_nested_style_block(StyleCssParseContext *context,
                                     const char *css,
                                     size_t begin, size_t end,
                                     const char *selectors,
                                     size_t selector_length,
                                     unsigned depth)
{
    Stylesheet *sheet = context == NULL ? NULL : context->sheet;
    if (depth > 8) return true;
    size_t declaration_start = begin;
    size_t item_start = begin;
    while (item_start < end) {
        item_start = skip_css_space_and_comments(css, end, item_start);
        if (item_start >= end) break;
        size_t delimiter = item_start;
        int round = 0;
        int square = 0;
        char quote = 0;
        for (; delimiter < end; delimiter++) {
            char character = css[delimiter];
            if (quote != 0) {
                if (character == '\\' && delimiter + 1 < end) delimiter++;
                else if (character == quote) quote = 0;
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '/' && delimiter + 1 < end
                       && css[delimiter + 1] == '*') {
                delimiter += 2;
                while (delimiter + 1 < end
                       && !(css[delimiter] == '*'
                            && css[delimiter + 1] == '/')) delimiter++;
                if (delimiter + 1 < end) delimiter++;
            } else if (character == '(') {
                round++;
            } else if (character == ')' && round > 0) {
                round--;
            } else if (character == '[') {
                square++;
            } else if (character == ']' && square > 0) {
                square--;
            } else if (round == 0 && square == 0
                       && (character == ';' || character == '{')) {
                break;
            }
        }
        if (delimiter >= end) break;
        if (css[delimiter] == ';') {
            item_start = delimiter + 1;
            continue;
        }

        if (item_start > declaration_start
            && !selector_list_to_rules(
                   context, selectors, selector_length,
                   css + declaration_start, item_start - declaration_start)) {
            return false;
        }
        size_t close = find_matching_brace(css, delimiter, end);
        if (close == end) return false;
        const char *header = css + item_start;
        size_t header_length = delimiter - item_start;
        trim(&header, &header_length);
        const char *condition = NULL;
        size_t condition_length = 0;
        if (css_header_rule(header, header_length, "@keyframes",
                            &condition, &condition_length)
            || css_header_rule(header, header_length, "@-webkit-keyframes",
                               &condition, &condition_length)) {
            if (context->parsed_ir != NULL) {
                context->parsed_ir->has_motion_keyframes = true;
            }
            sheet->has_motion_keyframes = true;
        } else if (css_header_rule(header, header_length, "@media",
                            &condition, &condition_length)) {
            if (media_matches(sheet, condition, condition_length)
                && !parse_nested_style_block(
                       context, css, delimiter + 1, close, selectors,
                       selector_length, depth + 1)) return false;
        } else if (css_header_rule(header, header_length, "@supports",
                                   &condition, &condition_length)) {
            bool supported = supports_matches(
                sheet, condition, condition_length);
            if (!supported) {
                diagnostic_saturating_add(
                    &sheet->diagnostic_supports_false_queries, 1);
            }
            if (supported
                && !parse_nested_style_block(
                       context, css, delimiter + 1, close, selectors,
                       selector_length, depth + 1)) return false;
        } else if (css_header_rule(header, header_length, "@container",
                                   &condition, &condition_length)) {
            style_parsed_ir_builder_disqualify(context);
            uint8_t previous_query = sheet->current_container_query;
            uint8_t query = 0;
            if (!style_register_container_query(
                    sheet, condition, condition_length, previous_query,
                    &query)) return false;
            if (query != 0) {
                sheet->current_container_query = query;
                bool parsed = parse_nested_style_block(
                    context, css, delimiter + 1, close, selectors,
                    selector_length, depth + 1);
                sheet->current_container_query = previous_query;
                if (!parsed) return false;
            }
        } else if (css_header_rule(header, header_length, "@layer",
                                   &condition, &condition_length)) {
            style_parsed_ir_builder_disqualify(context);
            unsigned previous_layer = sheet->current_layer;
            sheet->current_layer = register_layer(
                sheet, condition, condition_length);
            bool parsed = parse_nested_style_block(
                context, css, delimiter + 1, close, selectors,
                selector_length, depth + 1);
            sheet->current_layer = previous_layer;
            if (!parsed) return false;
        } else if (header_length != 0 && header[0] != '@') {
            char expanded[STYLE_SELECTOR_CAPACITY * 8];
            size_t expanded_length = 0;
            size_t combinations = 0;
            for (size_t parent_at = 0; parent_at < selector_length;) {
                size_t parent_end = css_selector_item_end(
                    selectors, selector_length, parent_at);
                for (size_t child_at = 0; child_at < header_length;) {
                    size_t child_end = css_selector_item_end(
                        header, header_length, child_at);
                    char one[STYLE_SELECTOR_CAPACITY];
                    if (++combinations > 32
                        || !nested_selector_expand_one(
                               selectors + parent_at, parent_end - parent_at,
                               header + child_at, child_end - child_at, one)) {
                        return false;
                    }
                    size_t one_length = strlen(one);
                    if (expanded_length + one_length
                            + (expanded_length != 0) + 1
                        > sizeof(expanded)) return false;
                    if (expanded_length != 0) {
                        expanded[expanded_length++] = ',';
                    }
                    memcpy(expanded + expanded_length, one, one_length);
                    expanded_length += one_length;
                    child_at = child_end + (child_end < header_length);
                }
                parent_at = parent_end + (parent_end < selector_length);
            }
            expanded[expanded_length] = '\0';
            if (!parse_nested_style_block(
                    context, css, delimiter + 1, close, expanded,
                    expanded_length, depth + 1)) return false;
        }
        declaration_start = item_start = close + 1;
    }
    if (declaration_start < end) {
        return selector_list_to_rules(
            context, selectors, selector_length,
            css + declaration_start, end - declaration_start);
    }
    return true;
}

static bool parse_css_range_nested(StyleCssParseContext *context,
                                   const char *css,
                                   size_t begin, size_t length,
                                   unsigned depth, bool font_faces_allowed)
{
    Stylesheet *sheet = context == NULL ? NULL : context->sheet;
    if (sheet == NULL || depth > 16) return true;
    bool parse_style_rules = depth <= 8;
    if (begin == 0) {
        size_t start = skip_css_space_and_comments(css, length, 0);
        size_t end = length;
        while (end > start && isspace((unsigned char) css[end - 1])) end--;
        if (end >= start + 12
            && memcmp(css + start, "<![CDATA[", 9) == 0
            && memcmp(css + end - 3, "]]>", 3) == 0) {
            begin = start + 9;
            length = end - 3;
        }
    }
    size_t parsed_rules = 0;
    for (size_t at = begin; at < length;) {
        if ((parsed_rules++ & 15u) == 0
            && !tilefinch_platform_cooperate(
                "stylesheet-parse", at)) return false;
        at = skip_css_space_and_comments(css, length, at);
        if (at >= length) break;
        size_t prelude = at;
        size_t brace = at;
        char quote = 0;
        int round = 0;
        int square = 0;
        while (brace < length) {
            char character = css[brace];
            if (quote != 0) {
                if (character == '\\' && brace + 1 < length) brace++;
                else if (character == quote) quote = 0;
            } else if (character == '\\' && brace + 1 < length) {
                /* Escaped punctuation in an arbitrary-value utility class
                   is syntax-neutral. In particular, an escaped quote must
                   not open a string and discard the stylesheet suffix. */
                brace++;
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '/' && brace + 1 < length
                       && css[brace + 1] == '*') {
                brace += 2;
                while (brace + 1 < length
                       && !(css[brace] == '*'
                            && css[brace + 1] == '/')) brace++;
                if (brace + 1 < length) brace++;
            } else if (character == '(') {
                round++;
            } else if (character == ')' && round > 0) {
                round--;
            } else if (character == '[') {
                square++;
            } else if (character == ']' && square > 0) {
                square--;
            } else if (round == 0 && square == 0
                       && (character == '{' || character == ';')) {
                break;
            }
            brace++;
        }
        if (brace == length) break;
        if (css[brace] == ';') {
            const char *statement = css + prelude;
            size_t statement_length = brace - prelude;
            trim(&statement, &statement_length);
            const char *layer_list = NULL;
            size_t layer_list_length = 0;
            if (parse_style_rules
                && css_header_rule(statement, statement_length, "@layer",
                                &layer_list, &layer_list_length)) {
                style_parsed_ir_builder_disqualify(context);
                register_layer_list(
                    sheet, layer_list, layer_list_length);
            }
            at = brace + 1;
            continue;
        }
        size_t close = find_matching_brace(css, brace, length);
        /* CSS Syntax closes an open block at EOF.  Minified production
           sheets use this recovery in practice (notably a final @layer
           utilities block); dropping the whole block leaves raw SVG and
           desktop fallbacks visible on a mobile viewport.  The recursive
           parser remains bounded and treats `length` as the implicit close. */
        const char *header = css + prelude;
        size_t header_length = brace - prelude;
        trim(&header, &header_length);
        const char *condition = NULL;
        size_t condition_length = 0;
        if (font_faces_allowed && context->discover_font_faces
            && css_header_rule(header, header_length, "@font-face",
                               &condition, &condition_length)) {
            style_parsed_ir_builder_disqualify(context);
            trim(&condition, &condition_length);
            if (condition_length == 0) {
                register_font_face(
                    sheet, css + brace + 1, close - brace - 1,
                    sheet->resolve_scratch->current_image_source_base,
                    sheet->resolve_scratch
                        ->current_image_source_referrer_policy);
            }
        } else if (parse_style_rules
            && (css_header_rule(header, header_length, "@keyframes",
                            &condition, &condition_length)
            || css_header_rule(header, header_length, "@-webkit-keyframes",
                               &condition, &condition_length))) {
            if (context->parsed_ir != NULL) {
                context->parsed_ir->has_motion_keyframes = true;
            }
            sheet->has_motion_keyframes = true;
        } else if (css_header_rule(header, header_length, "@media",
                            &condition, &condition_length)) {
            if (media_matches(sheet, condition, condition_length)
                && !parse_css_range_nested(
                       context, css, brace + 1, close,
                       depth + 1, font_faces_allowed)) return false;
        } else if (css_header_rule(header, header_length, "@supports",
                                   &condition, &condition_length)) {
            bool supported = supports_matches(
                sheet, condition, condition_length);
            if (parse_style_rules && !supported) {
                diagnostic_saturating_add(
                    &sheet->diagnostic_supports_false_queries, 1);
            }
            if (supported
                && !parse_css_range_nested(
                       context, css, brace + 1, close,
                       depth + 1, font_faces_allowed)) return false;
        } else if (css_header_rule(header, header_length, "@container",
                                   &condition, &condition_length)) {
            style_parsed_ir_builder_disqualify(context);
            if (!parse_style_rules) {
                at = close < length ? close + 1 : length;
                continue;
            }
            uint8_t previous_query = sheet->current_container_query;
            uint8_t query = 0;
            if (!style_register_container_query(
                    sheet, condition, condition_length, previous_query,
                    &query)) return false;
            if (query != 0) {
                sheet->current_container_query = query;
                bool parsed = parse_css_range_nested(
                    context, css, brace + 1, close, depth + 1, false);
                sheet->current_container_query = previous_query;
                if (!parsed) return false;
            }
        } else if (css_header_rule(header, header_length, "@layer",
                                   &condition, &condition_length)) {
            style_parsed_ir_builder_disqualify(context);
            if (!parse_style_rules) {
                if (!parse_css_range_nested(
                        context, css, brace + 1, close, depth + 1,
                        font_faces_allowed)) return false;
                at = close < length ? close + 1 : length;
                continue;
            }
            unsigned previous_layer = sheet->current_layer;
            sheet->current_layer = register_layer(
                sheet, condition, condition_length);
            bool parsed = parse_css_range_nested(
                context, css, brace + 1, close, depth + 1,
                font_faces_allowed);
            sheet->current_layer = previous_layer;
            if (!parsed) return false;
        } else if (parse_style_rules
                   && header_length != 0 && header[0] != '@') {
            if (!parse_nested_style_block(
                    context, css, brace + 1, close, header,
                    header_length, depth + 1)) return false;
        }
        at = close < length ? close + 1 : length;
    }
    return true;
}

static bool parse_css_range(StyleCssParseContext *context, const char *css,
                            size_t begin, size_t length)
{
    return parse_css_range_nested(
        context, css, begin, length, 0, true);
}

static void style_css_font_fixup_property(
    StyleCssParseContext *context, StyleFontDeclarationFixup *fixup,
    const char *name, size_t name_length, const char *value,
    size_t value_length, bool important)
{
    if (context == NULL || context->sheet == NULL || fixup == NULL) return;
    size_t slot = important ? 1u : 0u;
    if ((fixup->slots & (1u << slot)) == 0) return;
    uint32_t declaration_index = fixup->declaration_indices[slot];
    Stylesheet *sheet = context->sheet;
    if (declaration_index >= sheet->declaration_count) return;
    StyleDeclaration *declaration = &sheet->declarations[declaration_index];
    if ((declaration->mask & S_FONT_FAMILY) == 0
        || (declaration->inherit_mask & S_FONT_FAMILY) != 0) return;
    if (important) {
        value_length = declaration_value_length(value, value_length);
        trim(&value, &value_length);
    }
    FontFamily family = FONT_SANS;
    bool parsed = false;
    if (span_case_equal(name, name_length, "font-family")) {
        char resolved[160];
        parsed = style_resolve_value(
            sheet, value, value_length, resolved, sizeof(resolved), 0);
        if (parsed) {
            for (size_t i = 0; resolved[i] != '\0'; i++) {
                resolved[i] = (char) tolower((unsigned char) resolved[i]);
            }
            parsed = style_font_family_component(
                sheet, resolved, strlen(resolved), &family);
        }
    } else if (span_case_equal(name, name_length, "font")) {
        ComputedStyle font = {0};
        parsed = style_parse_font_shorthand(
            sheet, value, value_length, &font);
        family = font.font_family;
    }
    if (parsed) declaration->values.font_family = family;
}

static void style_css_font_fixup_apply(
    StyleCssParseContext *context, StyleFontDeclarationFixup *fixup)
{
    const char *text = fixup->declarations;
    size_t length = fixup->declaration_length;
    for (size_t at = 0; text != NULL && at < length;) {
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
            if (span_case_equal(name, name_length, "font-family")
                || span_case_equal(name, name_length, "font")) {
                style_css_font_fixup_property(
                    context, fixup, name, name_length, value, value_length,
                    declaration_is_important(value, value_length));
            }
        }
        at = end + (end < length);
    }
}

static void style_css_parse_context_dispose(StyleCssParseContext *context)
{
    if (context == NULL) return;
    if (context->sheet != NULL) {
        budget_free(context->sheet->budget, context->font_fixups);
    }
    context->font_fixups = NULL;
    context->font_fixup_count = 0;
    context->font_fixup_capacity = 0;
}

static void style_css_reintern_declarations(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->declaration_count == 0) return;
    size_t old_count = sheet->declaration_count;
    if (old_count > UINT32_MAX
        || old_count > SIZE_MAX / sizeof(uint32_t)) return;
    uint32_t *mapping = budget_malloc(
        sheet->budget, old_count * sizeof(*mapping));
    if (mapping == NULL
        || !style_declaration_index_prepare(sheet, old_count)
        || sheet->declaration_index_slots == NULL) {
        budget_free(sheet->budget, mapping);
        return;
    }
    for (size_t i = 0; i < sheet->declaration_index_slot_count; i++) {
        sheet->declaration_index_slots[i] = UINT32_MAX;
    }
    size_t retained = 0;
    size_t table_mask = sheet->declaration_index_slot_count - 1u;
    for (size_t old = 0; old < old_count; old++) {
        StyleDeclaration candidate = sheet->declarations[old];
        uint32_t hash = style_declaration_hash(
            &candidate.values, candidate.mask, candidate.mask_high,
            candidate.inherit_mask, candidate.deferred_declarations,
            candidate.deferred_length);
        size_t slot = (size_t) hash & table_mask;
        uint32_t duplicate = UINT32_MAX;
        for (size_t probes = 0;
             probes < sheet->declaration_index_slot_count;
             probes++, slot = (slot + 1u) & table_mask) {
            uint32_t indexed = sheet->declaration_index_slots[slot];
            if (indexed == UINT32_MAX) break;
            if (indexed < retained
                && style_declaration_equal(
                    &sheet->declarations[indexed], &candidate.values,
                    candidate.mask, candidate.mask_high,
                    candidate.inherit_mask, candidate.deferred_declarations,
                    candidate.deferred_length)) {
                duplicate = indexed;
                break;
            }
        }
        if (duplicate != UINT32_MAX) {
            mapping[old] = duplicate;
            if (candidate.deferred_declarations != NULL) {
                size_t bytes = candidate.deferred_length + 1u;
                if (bytes <= sheet->deferred_bytes) {
                    sheet->deferred_bytes -= bytes;
                }
                budget_free(sheet->budget, candidate.deferred_declarations);
            }
            continue;
        }
        if (retained != old) sheet->declarations[retained] = candidate;
        mapping[old] = (uint32_t) retained;
        size_t insert = (size_t) hash & table_mask;
        while (sheet->declaration_index_slots[insert] != UINT32_MAX) {
            insert = (insert + 1u) & table_mask;
        }
        sheet->declaration_index_slots[insert] = (uint32_t) retained;
        retained++;
    }
    for (size_t i = 0; i < sheet->count; i++) {
        uint32_t old = sheet->rules[i].declaration_index;
        if (old < old_count) sheet->rules[i].declaration_index = mapping[old];
    }
    sheet->declaration_count = retained;
    size_t declaration_capacity = 32u;
    while (declaration_capacity < retained
           && declaration_capacity <= SIZE_MAX / 2u) {
        declaration_capacity *= 2u;
    }
    if (declaration_capacity >= retained
        && declaration_capacity < sheet->declaration_capacity) {
        StyleDeclaration *smaller = budget_realloc(
            sheet->budget, sheet->declarations,
            declaration_capacity * sizeof(*smaller));
        if (smaller != NULL) {
            sheet->declarations = smaller;
            sheet->declaration_capacity = declaration_capacity;
        }
    }
    size_t index_slots = 64u;
    while (retained > index_slots / 2u
           && index_slots <= SIZE_MAX / 2u) index_slots *= 2u;
    if (index_slots < sheet->declaration_index_slot_count) {
        uint32_t *smaller = budget_realloc(
            sheet->budget, sheet->declaration_index_slots,
            index_slots * sizeof(*smaller));
        if (smaller != NULL) {
            sheet->declaration_index_slots = smaller;
            sheet->declaration_index_slot_count = index_slots;
        }
    }
    for (size_t i = 0; i < sheet->declaration_index_slot_count; i++) {
        sheet->declaration_index_slots[i] = UINT32_MAX;
    }
    for (size_t i = 0; i < retained; i++) {
        if (!style_declaration_index_insert(sheet, (uint32_t) i)) break;
    }
    budget_free(sheet->budget, mapping);
}

static bool style_css_parse_context_finish(StyleCssParseContext *context)
{
    if (context == NULL || context->sheet == NULL) return false;
    Stylesheet *sheet = context->sheet;
    bool completed = true;
    for (size_t i = 0; i < context->font_fixup_count; i++) {
        if ((i & 15u) == 0
            && !tilefinch_platform_cooperate("stylesheet-font-fixup", i)) {
            completed = false;
            break;
        }
        style_css_font_fixup_apply(context, &context->font_fixups[i]);
    }
    if (completed && context->font_fixup_count != 0) {
        /* Forward references were deliberately unique while their family
           identity was unresolved. Compact equivalent final declarations
           and rewrite rule indices so the optimization does not increase
           retained page memory. Failure of this optional pass merely keeps
           the already-correct declarations separate. */
        style_css_reintern_declarations(sheet);
    }
    style_css_parse_context_dispose(context);
    return completed;
}

static bool collect_styles(
    StyleCssParseContext *context, lxb_dom_node_t *node,
    const TilefinchContentSecurityPolicy *content_security_policy)
{
    for (; node != NULL; node = node->next) {
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL && span_equal(name, name_length, "style")
            && tilefinch_csp_allows_inline_style(
                   content_security_policy, node)) {
            for (lxb_dom_node_t *child = node->first_child; child != NULL; child = child->next) {
                size_t length = 0;
                const char *css = document_text_data(child, &length);
                if (css != NULL
                    && !parse_css_range(context, css, 0, length)) return false;
            }
        }
        if (node->first_child != NULL
            && !collect_styles(context, node->first_child,
                               content_security_policy)) return false;
    }
    return true;
}
