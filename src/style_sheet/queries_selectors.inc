static void diagnostic_saturating_add(uint64_t *value, size_t increment)
{
    if (value == NULL || *value == UINT64_MAX || increment == 0) return;
    uint64_t add = (uint64_t) increment;
    *value = add > UINT64_MAX - *value ? UINT64_MAX : *value + add;
}

static bool span_contains_literal(const char *text, size_t length,
                                  const char *literal)
{
    size_t wanted = strlen(literal);
    if (text == NULL || wanted == 0 || wanted > length) return false;
    for (size_t i = 0; i + wanted <= length; i++) {
        if (memcmp(text + i, literal, wanted) == 0) return true;
    }
    return false;
}

static bool media_feature_known(const char *feature, size_t length)
{
    if (feature == NULL || length == 0) return false;
    static const char *prefixes[] = {
        "min-width:", "max-width:", "width:",
        "min-height:", "max-height:", "height:",
        "min-device-width:", "max-device-width:", "device-width:",
        "min-device-height:", "max-device-height:", "device-height:",
        "min-aspect-ratio:", "max-aspect-ratio:", "aspect-ratio:",
        "orientation:", "hover:", "any-hover:",
        "pointer:", "any-pointer:", "prefers-color-scheme:",
        "prefers-reduced-motion:", "prefers-contrast:", "forced-colors:"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t prefix_length = strlen(prefixes[i]);
        if (length >= prefix_length
            && memcmp(feature, prefixes[i], prefix_length) == 0) return true;
    }
    bool comparison = memchr(feature, '<', length) != NULL
                      || memchr(feature, '>', length) != NULL
                      || memchr(feature, '=', length) != NULL;
    return comparison && (span_contains_literal(feature, length, "width")
                          || span_contains_literal(feature, length, "height"));
}

static bool media_ratio_ranges_match(const char *query, const char *feature,
                                     int width, int height, int relation)
{
    size_t feature_length = strlen(feature);
    for (const char *match = strstr(query, feature); match != NULL;
         match = strstr(match + feature_length, feature)) {
        if (match > query
            && (isalnum((unsigned char) match[-1]) || match[-1] == '-')) {
            continue;
        }
        const char *value = match + feature_length;
        char *end = NULL;
        long numerator = strtol(value, &end, 10);
        if (end == value || numerator <= 0 || *end != '/') return false;
        value = end + 1;
        long denominator = strtol(value, &end, 10);
        if (end == value || denominator <= 0) return false;
        int64_t actual = (int64_t) width * denominator;
        int64_t threshold = (int64_t) height * numerator;
        if ((relation < 0 && actual > threshold)
            || (relation > 0 && actual < threshold)
            || (relation == 0 && actual != threshold)) return false;
    }
    return true;
}

static size_t media_unknown_feature_count(const char *query)
{
    if (query == NULL) return 0;
    size_t length = strlen(query);
    size_t unknown = 0;
    for (size_t at = 0; at < length; at++) {
        if (query[at] != '(') continue;
        size_t end = at + 1;
        unsigned depth = 1;
        while (end < length && depth != 0) {
            if (query[end] == '(') depth++;
            else if (query[end] == ')') depth--;
            if (depth != 0) end++;
        }
        if (depth != 0 || end <= at + 1
            || !media_feature_known(query + at + 1, end - at - 1)) {
            unknown++;
        }
        if (depth == 0) at = end;
    }
    return unknown;
}

static bool media_colon_ranges_match(const Stylesheet *sheet,
                                     const char *query,
                                     const char *feature,
                                     int actual, bool minimum)
{
    if (sheet == NULL || query == NULL || feature == NULL) return false;
    size_t feature_length = strlen(feature);
    for (const char *match = strstr(query, feature); match != NULL;
         match = strstr(match + feature_length, feature)) {
        const char *value = match + feature_length;
        int fallback = minimum ? 0 : 32767;
        int threshold = style_parse_length(NULL, value, strlen(value), fallback,
                                     NULL);
        if ((minimum && actual < threshold)
            || (!minimum && actual > threshold)) return false;
    }
    return true;
}

static bool media_colon_exact_match(const Stylesheet *sheet,
                                    const char *query,
                                    const char *feature, int actual)
{
    size_t feature_length = strlen(feature);
    for (const char *match = strstr(query, feature); match != NULL;
         match = strstr(match + feature_length, feature)) {
        if (match > query
            && (isalnum((unsigned char) match[-1]) || match[-1] == '-')) {
            continue;
        }
        const char *value = match + feature_length;
        int threshold = style_parse_length(sheet, value, strlen(value),
                                           INT_MIN, NULL);
        if (threshold == INT_MIN || actual != threshold) return false;
    }
    return true;
}

static bool media_clause_matches(const Stylesheet *sheet,
                                 const char *text, size_t length)
{
    char query[512];
    size_t output = 0;
    size_t consumed = 0;
    for (; consumed < length && output + 1 < sizeof(query); consumed++) {
        size_t i = consumed;
        if (!isspace((unsigned char) text[i])) {
            query[output++] = (char) tolower((unsigned char) text[i]);
        }
    }
    query[output] = '\0';
    size_t unknown = media_unknown_feature_count(query);
    if (consumed != length) unknown++;
    if (unknown != 0) {
        diagnostic_saturating_add(
            &((Stylesheet *) sheet)->diagnostic_unknown_media_features,
            unknown);
        return false;
    }
    /* Media types are evaluated before their feature tail.  The device is a
       screen/all environment, so the common responsive-image sentinel
       `not all` and an explicitly negated screen query can never match,
       while `not print` can.  Keeping this in the shared evaluator makes
       <source media>, @media, and the native matchMedia bridge agree. */
    if (strncmp(query, "notall", 6) == 0
        || strncmp(query, "notscreen", 9) == 0) return false;
    if (strstr(query, "print") != NULL && strstr(query, "screen") == NULL
        && strstr(query, "notprint") == NULL) return false;
    if (strstr(query, "prefers-color-scheme:dark") != NULL
        || strstr(query, "prefers-reduced-motion:reduce") != NULL
        || (strstr(query, "prefers-contrast:") != NULL
            && strstr(query, "prefers-contrast:no-preference") == NULL)
        || strstr(query, "forced-colors:active") != NULL
        || strstr(query, "hover:hover") != NULL
        || strstr(query, "any-hover:hover") != NULL
        || strstr(query, "pointer:fine") != NULL
        || strstr(query, "any-pointer:fine") != NULL
        || strstr(query, "orientation:portrait") != NULL) return false;
    struct DimensionRange {
        const char *name;
        int actual;
    } dimensions[] = {{"width", sheet->viewport_width},
                      {"height", sheet->viewport_height}};
    bool range_recognized = false;
    bool range_matches = true;
    for (size_t dimension = 0;
         dimension < sizeof(dimensions) / sizeof(dimensions[0]); dimension++) {
        const char *name = dimensions[dimension].name;
        int actual = dimensions[dimension].actual;
        const char *operators[] = {">=", "<=", ">", "<", "="};
        for (size_t operation = 0;
             operation < sizeof(operators) / sizeof(operators[0]); operation++) {
            char pattern[24];
            snprintf(pattern, sizeof(pattern), "%s%s", name,
                     operators[operation]);
            for (const char *match = strstr(query, pattern); match != NULL;
                 match = strstr(match + 1, pattern)) {
                const char *value = match + strlen(pattern);
                if ((operation == 2 || operation == 3)
                    && *value == '=') continue;
                int threshold = style_parse_length(sheet, value, strlen(value), 0,
                                             NULL);
                range_recognized = true;
                if ((operation == 0 && actual < threshold)
                    || (operation == 1 && actual > threshold)
                    || (operation == 2 && actual <= threshold)
                    || (operation == 3 && actual >= threshold)
                    || (operation == 4 && actual != threshold)) {
                    range_matches = false;
                }
            }
            snprintf(pattern, sizeof(pattern), "%s%s", operators[operation],
                     name);
            for (const char *match = strstr(query, pattern); match != NULL;
                 match = strstr(match + 1, pattern)) {
                if ((operation == 2 || operation == 3)
                    && match > query && match[-1] == '=') continue;
                const char *start = match;
                while (start > query) {
                    unsigned char character = (unsigned char) start[-1];
                    if (!(isalnum(character) || character == '.'
                          || character == '%' || character == '-')) break;
                    start--;
                }
                if (start == match) continue;
                int threshold = style_parse_length(sheet, start,
                                             (size_t) (match - start), 0,
                                             NULL);
                range_recognized = true;
                if ((operation == 0 && threshold < actual)
                    || (operation == 1 && threshold > actual)
                    || (operation == 2 && threshold <= actual)
                    || (operation == 3 && threshold >= actual)
                    || (operation == 4 && threshold != actual)) {
                    range_matches = false;
                }
            }
        }
    }
    if (range_recognized && !range_matches) return false;
    return media_colon_ranges_match(sheet, query, "min-width:",
                                    sheet->viewport_width, true)
           && media_colon_ranges_match(sheet, query, "max-width:",
                                       sheet->viewport_width, false)
           && media_colon_ranges_match(sheet, query, "min-height:",
                                       sheet->viewport_height, true)
           && media_colon_ranges_match(sheet, query, "max-height:",
                                       sheet->viewport_height, false)
           && media_colon_ranges_match(sheet, query, "min-device-width:",
                                       sheet->viewport_width, true)
           && media_colon_ranges_match(sheet, query, "max-device-width:",
                                       sheet->viewport_width, false)
           && media_colon_ranges_match(sheet, query, "min-device-height:",
                                       sheet->viewport_height, true)
           && media_colon_ranges_match(sheet, query, "max-device-height:",
                                       sheet->viewport_height, false)
           && media_colon_exact_match(sheet, query, "width:",
                                      sheet->viewport_width)
           && media_colon_exact_match(sheet, query, "height:",
                                      sheet->viewport_height)
           && media_colon_exact_match(sheet, query, "device-width:",
                                      sheet->viewport_width)
           && media_colon_exact_match(sheet, query, "device-height:",
                                      sheet->viewport_height)
           && media_ratio_ranges_match(query, "min-aspect-ratio:",
                                       sheet->viewport_width,
                                       sheet->viewport_height, 1)
           && media_ratio_ranges_match(query, "max-aspect-ratio:",
                                       sheet->viewport_width,
                                       sheet->viewport_height, -1)
           && media_ratio_ranges_match(query, "aspect-ratio:",
                                       sheet->viewport_width,
                                       sheet->viewport_height, 0);
}

static bool media_matches(const Stylesheet *sheet, const char *text, size_t length)
{
    for (size_t at = 0; at < length;) {
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (text[end] == ',' && parentheses == 0) break;
            end++;
        }
        if (media_clause_matches(sheet, text + at, end - at)) return true;
        at = end + (end < length);
    }
    return false;
}

bool stylesheet_media_matches(const Stylesheet *sheet, const char *query,
                              size_t query_length)
{
    return sheet != NULL && query != NULL
           && media_matches(sheet, query, query_length);
}

size_t skip_selector_identifier(const char *text, size_t length,
                                       size_t at)
{
    while (at < length) {
        unsigned char character = (unsigned char) text[at];
        if (isalnum(character) || character == '-' || character == '_'
            || character >= 0x80u) {
            at++;
        } else if (character == '\\' && at + 1 < length) {
            size_t escaped = at + 1;
            if (text[escaped] == '\n' || text[escaped] == '\r'
                || text[escaped] == '\f') break;
            size_t digits = 0;
            while (escaped < length && digits < 6
                   && isxdigit((unsigned char) text[escaped])) {
                escaped++;
                digits++;
            }
            if (digits != 0 && escaped < length
                && isspace((unsigned char) text[escaped])) {
                if (text[escaped] == '\r' && escaped + 1 < length
                    && text[escaped + 1] == '\n') escaped++;
                escaped++;
            } else if (digits == 0) {
                escaped++;
            }
            at = escaped;
        } else {
            break;
        }
    }
    return at;
}

static size_t selector_matching_delimiter(const char *text, size_t length,
                                          size_t open, char left, char right)
{
    unsigned depth = 1;
    char quote = 0;
    for (size_t i = open + 1; i < length; i++) {
        if (quote != 0) {
            if (text[i] == '\\' && i + 1 < length) i++;
            else if (text[i] == quote) quote = 0;
            continue;
        }
        if (text[i] == '\'' || text[i] == '"') {
            quote = text[i];
        } else if (text[i] == left) {
            depth++;
        } else if (text[i] == right && --depth == 0) {
            return i;
        }
    }
    return length;
}

static unsigned selector_specificity_depth(const char *text, size_t length,
                                           unsigned depth);

#define SELECTOR_SPECIFICITY_COMPONENT_MASK 1023u
#define SELECTOR_SPECIFICITY_ID_UNIT (1u << 20)
#define SELECTOR_SPECIFICITY_CLASS_UNIT (1u << 10)
#define SELECTOR_SPECIFICITY_TYPE_UNIT 1u

static unsigned selector_specificity_add(unsigned left, unsigned right)
{
    unsigned left_ids = (left >> 20) & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned right_ids = (right >> 20) & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned left_classes =
        (left >> 10) & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned right_classes =
        (right >> 10) & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned left_types = left & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned right_types = right & SELECTOR_SPECIFICITY_COMPONENT_MASK;
    unsigned ids = left_ids > SELECTOR_SPECIFICITY_COMPONENT_MASK - right_ids
        ? SELECTOR_SPECIFICITY_COMPONENT_MASK : left_ids + right_ids;
    unsigned classes =
        left_classes > SELECTOR_SPECIFICITY_COMPONENT_MASK - right_classes
        ? SELECTOR_SPECIFICITY_COMPONENT_MASK
        : left_classes + right_classes;
    unsigned types =
        left_types > SELECTOR_SPECIFICITY_COMPONENT_MASK - right_types
        ? SELECTOR_SPECIFICITY_COMPONENT_MASK : left_types + right_types;
    return (ids << 20) | (classes << 10) | types;
}

static unsigned selector_list_max_specificity(const char *text, size_t length,
                                              unsigned depth)
{
    unsigned maximum = 0;
    size_t begin = 0;
    unsigned parentheses = 0, brackets = 0;
    char quote = 0;
    for (size_t i = 0; i <= length; i++) {
        char character = i < length ? text[i] : ',';
        if (quote != 0) {
            if (character == '\\' && i + 1 < length) i++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        else if (character == '[') brackets++;
        else if (character == ']' && brackets != 0) brackets--;
        else if (character == ',' && parentheses == 0 && brackets == 0) {
            unsigned value = selector_specificity_depth(text + begin,
                                                        i - begin,
                                                        depth + 1);
            if (value > maximum) maximum = value;
            begin = i + 1;
        }
    }
    return maximum;
}

static size_t selector_nth_of_list(const char *text, size_t length)
{
    unsigned parentheses = 0, brackets = 0;
    char quote = 0;
    for (size_t at = 0; at + 2 < length; at++) {
        char character = text[at];
        if (quote != 0) {
            if (character == '\\' && at + 1 < length) at++;
            else if (character == quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') quote = character;
        else if (character == '(') parentheses++;
        else if (character == ')' && parentheses != 0) parentheses--;
        else if (character == '[') brackets++;
        else if (character == ']' && brackets != 0) brackets--;
        else if (parentheses == 0 && brackets == 0
                 && isspace((unsigned char) character)) {
            size_t word = at;
            while (word < length
                   && isspace((unsigned char) text[word])) word++;
            if (word + 2 < length
                && (text[word] == 'o' || text[word] == 'O')
                && (text[word + 1] == 'f' || text[word + 1] == 'F')
                && isspace((unsigned char) text[word + 2])) {
                word += 2;
                while (word < length
                       && isspace((unsigned char) text[word])) word++;
                return word;
            }
        }
    }
    return length;
}

static unsigned selector_specificity_depth(const char *text, size_t length,
                                           unsigned depth)
{
    if (depth >= 8) return 0;
    unsigned specificity = 0;
    for (size_t i = 0; i < length;) {
        unsigned char character = (unsigned char) text[i];
        if (character == '#') {
            specificity = selector_specificity_add(
                specificity, SELECTOR_SPECIFICITY_ID_UNIT);
            i = skip_selector_identifier(text, length, i + 1);
        } else if (character == '.') {
            specificity = selector_specificity_add(
                specificity, SELECTOR_SPECIFICITY_CLASS_UNIT);
            i = skip_selector_identifier(text, length, i + 1);
        } else if (character == '[') {
            specificity = selector_specificity_add(
                specificity, SELECTOR_SPECIFICITY_CLASS_UNIT);
            size_t close = selector_matching_delimiter(text, length, i,
                                                       '[', ']');
            i = close < length ? close + 1 : length;
        } else if (character == ':') {
            bool pseudo_element = i + 1 < length && text[i + 1] == ':';
            size_t name = i + (pseudo_element ? 2 : 1);
            size_t name_end = skip_selector_identifier(text, length, name);
            size_t close = length;
            if (name_end < length && text[name_end] == '(') {
                close = selector_matching_delimiter(text, length, name_end,
                                                    '(', ')');
            }
            if (pseudo_element) {
                specificity = selector_specificity_add(
                    specificity, SELECTOR_SPECIFICITY_TYPE_UNIT);
            } else if (name_end < length && text[name_end] == '('
                       && close < length) {
                size_t name_length = name_end - name;
                if (span_equal(text + name, name_length, "where")) {
                    /* :where() and its argument always have zero weight. */
                } else if (span_equal(text + name, name_length, "is")
                           || span_equal(text + name, name_length, "not")
                           || span_equal(text + name, name_length, "has")) {
                    specificity = selector_specificity_add(
                        specificity, selector_list_max_specificity(
                            text + name_end + 1,
                            close - name_end - 1, depth));
                } else {
                    specificity = selector_specificity_add(
                        specificity, SELECTOR_SPECIFICITY_CLASS_UNIT);
                    if (span_equal(text + name, name_length, "nth-child")
                        || span_equal(text + name, name_length,
                                      "nth-last-child")) {
                        const char *argument = text + name_end + 1;
                        size_t argument_length = close - name_end - 1;
                        size_t list = selector_nth_of_list(
                            argument, argument_length);
                        if (list < argument_length) {
                            specificity = selector_specificity_add(
                                specificity,
                                selector_list_max_specificity(
                                    argument + list,
                                    argument_length - list, depth));
                        }
                    }
                }
            } else {
                specificity = selector_specificity_add(
                    specificity, SELECTOR_SPECIFICITY_CLASS_UNIT);
            }
            i = close < length ? close + 1 : name_end;
        } else if ((isalpha(character) || character == '_')
                   && (i == 0 || isspace((unsigned char) text[i - 1])
                       || text[i - 1] == '>' || text[i - 1] == '+'
                       || text[i - 1] == '~')) {
            specificity = selector_specificity_add(
                specificity, SELECTOR_SPECIFICITY_TYPE_UNIT);
            i = skip_selector_identifier(text, length, i + 1);
        } else {
            i++;
        }
    }
    return specificity;
}

static bool selector_store_fast_key(StyleRule *rule, SelectorType type,
                                    const char *text, size_t length)
{
    if (rule == NULL || rule->selector == NULL || text == NULL || length == 0
        || length >= STYLE_FAST_KEY_CAPACITY || text < rule->selector
        || text > rule->selector + rule->selector_length
        || length > (size_t) (rule->selector + rule->selector_length - text)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        /* Escaped identifiers need CSS unescaping before direct comparison.
           Keep those rules on the full matcher rather than risk a false
           rejection. */
        if (text[i] == '\\') return false;
    }
    size_t offset = (size_t) (text - rule->selector);
    if (offset > UINT16_MAX || length > UINT8_MAX) return false;
    rule->fast_key_offset = (uint16_t) offset;
    rule->fast_key_length = (uint8_t) length;
    rule->type = (uint8_t) type;
    rule->has_fast_key = true;
    return true;
}

static void selector_assign_fast_key(StyleRule *rule, const char *text,
                                     size_t length)
{
    trim(&text, &length);
    if (length == 0) return;
    size_t selector_base = (size_t) (text - rule->selector);

    /* Find the rightmost compound selector. Only that compound describes the
       candidate element; prefixes describe ancestors or siblings. */
    size_t start = 0;
    int square = 0;
    int round = 0;
    for (size_t i = length; i != 0; i--) {
        char value = text[i - 1];
        if (value == ']') square++;
        else if (value == '[' && square > 0) square--;
        else if (value == ')') round++;
        else if (value == '(' && round > 0) round--;
        else if (square == 0 && round == 0
                 && (value == '>' || value == '+' || value == '~')) {
            start = i;
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
                start = right;
                break;
            }
        }
    }
    while (start < length && isspace((unsigned char) text[start])) start++;
    if (start == length) return;
    if (selector_base + start > UINT16_MAX) return;
    rule->rightmost_compound_offset = (uint16_t) (selector_base + start);

    const char *tag = NULL;
    size_t tag_length = 0;
    if (isalnum((unsigned char) text[start]) || text[start] == '-'
        || text[start] == '_') {
        size_t end = skip_selector_identifier(text, length, start);
        tag = text + start;
        tag_length = end - start;
    }

    const char *class_name = NULL;
    size_t class_length = 0;
    square = 0;
    round = 0;
    char quote = 0;
    for (size_t at = start; at < length; at++) {
        char value = text[at];
        if (quote != 0) {
            if (value == '\\' && at + 1 < length) at++;
            else if (value == quote) quote = 0;
            continue;
        }
        if (square > 0 && (value == '\'' || value == '"')) {
            quote = value;
        } else if (value == '[') {
            square++;
        } else if (value == ']' && square > 0) {
            square--;
        } else if (square == 0 && value == '(') {
            round++;
        } else if (square == 0 && value == ')' && round > 0) {
            round--;
        } else if (square == 0 && round == 0
                   && (value == '#' || value == '.')) {
            size_t key_start = at + 1;
            size_t key_end = skip_selector_identifier(text, length,
                                                       key_start);
            if (key_end == key_start) continue;
            if (value == '#') {
                /* Fast keys are compared byte-for-byte with DOM values.
                   An escaped identifier must first be decoded, so leave it
                   to the fully correct fallback matcher instead of pruning
                   the rule from the candidate set under its source spelling
                   (for example #a\ b matching id="a b"). */
                if (memchr(text + key_start, '\\',
                           key_end - key_start) == NULL
                    && selector_store_fast_key(
                           rule, SELECTOR_ID, text + key_start,
                           key_end - key_start)) return;
            } else {
                if (memchr(text + key_start, '\\',
                           key_end - key_start) == NULL) {
                    class_name = text + key_start;
                    class_length = key_end - key_start;
                }
            }
            at = key_end - 1;
        }
    }
    if (class_name != NULL
        && selector_store_fast_key(rule, SELECTOR_CLASS, class_name,
                                   class_length)) return;
    if (tag != NULL) {
        (void) selector_store_fast_key(rule, SELECTOR_TAG, tag, tag_length);
    }
}

typedef struct {
    uint32_t declaration_indices[2];
    uint8_t image_source_slots[2];
    bool has_rules[2];
    bool prepared;
    uint64_t revert_rule_mask;
    uint64_t revert_rule_mask_high;
} StyleParsedRuleDeclarations;

static bool style_css_record_font_fixup(
    StyleCssParseContext *context, const char *declarations,
    size_t declaration_length, size_t slot, uint32_t declaration_index)
{
    if (context == NULL || context->sheet == NULL || declarations == NULL
        || declaration_length > UINT32_MAX || slot > 1) return false;
    StyleFontDeclarationFixup *fixup = NULL;
    if (context->font_fixup_count != 0) {
        StyleFontDeclarationFixup *last =
            &context->font_fixups[context->font_fixup_count - 1u];
        if (last->declarations == declarations
            && last->declaration_length == declaration_length) fixup = last;
    }
    if (fixup == NULL) {
        if (context->font_fixup_count == context->font_fixup_capacity) {
            size_t capacity = context->font_fixup_capacity == 0
                ? 16u : context->font_fixup_capacity * 2u;
            if (capacity < context->font_fixup_capacity
                || capacity > SIZE_MAX / sizeof(*context->font_fixups)) {
                return false;
            }
            StyleFontDeclarationFixup *grown = budget_realloc(
                context->sheet->budget, context->font_fixups,
                capacity * sizeof(*grown));
            if (grown == NULL) return false;
            context->font_fixups = grown;
            context->font_fixup_capacity = capacity;
        }
        fixup = &context->font_fixups[context->font_fixup_count++];
        memset(fixup, 0, sizeof(*fixup));
        fixup->declarations = declarations;
        fixup->declaration_length = declaration_length;
    }
    fixup->declaration_indices[slot] = declaration_index;
    fixup->slots |= (uint8_t) (1u << slot);
    return true;
}

static bool prepare_rule_declarations(
    StyleCssParseContext *context, const char *declarations,
    size_t declaration_length, StyleParsedRuleDeclarations *parsed)
{
    Stylesheet *sheet = context == NULL ? NULL : context->sheet;
    if (sheet == NULL || declarations == NULL || parsed == NULL) return false;
    if (parsed->prepared) return true;
    ComputedStyle values[2] = {{0}};
    uint64_t masks[2] = {0};
    uint64_t masks_high[2] = {0};
    uint64_t inherit_masks[2] = {0};
    uint64_t revert_rule_masks[2] = {0};
    uint64_t revert_rule_masks_high[2] = {0};
    if (!style_parse_declarations_split(
            sheet, declarations, declaration_length,
            &values[0], &masks[0], &masks_high[0], &inherit_masks[0],
            &values[1], &masks[1], &masks_high[1], &inherit_masks[1],
            &revert_rule_masks[0], &revert_rule_masks_high[0],
            &revert_rule_masks[1], &revert_rule_masks_high[1])) {
        return false;
    }
    StyleDeferredDeclarationAnalysis deferred = {0};
    declarations_analyze_deferred_resolution(
        declarations, declaration_length, &deferred);
    uint64_t important_concrete_mask = masks[1] | inherit_masks[1];
    uint64_t important_concrete_mask_high = masks_high[1];
    for (int important = 0; important <= 1; important++) {
        size_t slot = (size_t) important;
        if (deferred.container_relative[slot]) {
            sheet->has_container_relative_units = true;
        }
        if (masks[slot] == 0 && masks_high[slot] == 0
            && !deferred.deferred[slot]) continue;
        uint8_t image_source_slot =
            sheet->resolve_scratch->current_image_source_slot;
        if (deferred.deferred[slot]
            && !stylesheet_current_image_source_slot(
                   sheet, &image_source_slot)) return false;
        bool font_fixup = context->discover_font_faces
            && deferred.font_family[slot] && !deferred.deferred[slot]
            && (masks[slot] & S_FONT_FAMILY) != 0
            && !font_family_is_web(values[slot].font_family);
        uint32_t declaration_index = 0;
        if (!add_declaration(
                sheet, &values[slot], masks[slot], masks_high[slot],
                inherit_masks[slot],
                deferred.deferred[slot] ? declarations : NULL,
                deferred.deferred[slot] ? declaration_length : 0,
                font_fixup,
                &declaration_index)) return false;
        if (declaration_index >= sheet->declaration_count) return false;
        if (font_fixup
            && !style_css_record_font_fixup(
                   context, declarations, declaration_length, slot,
                   declaration_index)) return false;
        StyleDeclaration *stored = &sheet->declarations[declaration_index];
        if (deferred.font_relative[slot]) {
            stored->deferred_program_reserved |=
                STYLE_DEFERRED_FONT_RELATIVE;
        }
        if (deferred.non_font_relative[slot]) {
            stored->deferred_program_reserved |=
                STYLE_DEFERRED_NON_FONT_RELATIVE;
        }
        if (deferred.logical[slot]) {
            stored->deferred_program_reserved |= STYLE_DEFERRED_LOGICAL;
        }
        if (deferred.dynamic[slot]) {
            stored->deferred_program_reserved |= STYLE_DEFERRED_DYNAMIC;
        }
        if (deferred.ch_relative[slot]) {
            stored->deferred_program_reserved |= STYLE_DEFERRED_CH;
        }
        parsed->declaration_indices[important] = declaration_index;
        parsed->image_source_slots[important] = image_source_slot;
        parsed->has_rules[important] = true;
    }
    parsed->revert_rule_mask = revert_rule_masks[1]
        | (revert_rule_masks[0] & ~important_concrete_mask);
    parsed->revert_rule_mask_high = revert_rule_masks_high[1]
        | (revert_rule_masks_high[0]
           & ~important_concrete_mask_high);
    parsed->prepared = true;
    return true;
}

static bool selector_to_rule(
    StyleCssParseContext *context, const char *selector, size_t length,
    const char *declarations, size_t declaration_length,
    StyleParsedRuleDeclarations *parsed)
{
    Stylesheet *sheet = context == NULL ? NULL : context->sheet;
    if (parsed == NULL) return false;
    trim(&selector, &length);
    if (length == 0) return true;
    if (sheet->source_rule_limit_end != 0) {
        size_t head_limit = sheet->source_rule_head_limit_end != 0
                            ? sheet->source_rule_head_limit_end
                            : sheet->source_rule_limit_end;
        if (sheet->count >= head_limit
            && (sheet->source_rule_tail_begin == NULL
                || selector < sheet->source_rule_tail_begin
                || sheet->count >= sheet->source_rule_limit_end)) {
            diagnostic_saturating_add(&sheet->diagnostic_selector_drops, 1);
            return true;
        }
    }
    PseudoElement pseudo = PSEUDO_NONE;
    if (length >= 7 && memcmp(selector + length - 7, "::after", 7) == 0) {
        pseudo = PSEUDO_AFTER;
        length -= 7;
        trim(&selector, &length);
    } else if (length >= 8
               && memcmp(selector + length - 8, "::before", 8) == 0) {
        pseudo = PSEUDO_BEFORE;
        length -= 8;
        trim(&selector, &length);
    } else if (length >= 6
               && memcmp(selector + length - 6, ":after", 6) == 0) {
        /* CSS 2 permits the legacy single-colon spelling for these two
           pseudo-elements; current pages and the upstream CSS2 suite still
           rely on it. */
        pseudo = PSEUDO_AFTER;
        length -= 6;
        trim(&selector, &length);
    } else if (length >= 7
               && memcmp(selector + length - 7, ":before", 7) == 0) {
        pseudo = PSEUDO_BEFORE;
        length -= 7;
        trim(&selector, &length);
    }
    unsigned specificity = selector_specificity_depth(selector, length, 0);
    StyleRule base;
    memset(&base, 0, sizeof(base));
    base.pseudo = (uint8_t) pseudo;
    if (length >= STYLE_SELECTOR_CAPACITY) {
        if (sheet->diagnostic_selector_drops != UINT64_MAX) {
            sheet->diagnostic_selector_drops++;
        }
        return true;
    }
    base.selector_length = (uint16_t) length;
    base.origin = sheet->current_origin;
    base.layer = sheet->current_layer;
    base.specificity = specificity;
    base.order = sheet->next_order++;
    if (!collect_custom_rules(sheet, selector, length, declarations,
                              declaration_length, specificity,
                              base.order, pseudo)) return false;
    if (span_equal(selector, length, ":root")) {
        ComputedStyle ignored = {0};
        uint64_t ignored_mask = 0;
        uint64_t ignored_mask_high = 0;
        if (!style_parse_declarations(sheet, declarations, declaration_length,
                                &ignored, &ignored_mask, &ignored_mask_high,
                                NULL, true, -1, NULL, NULL)) return false;
    }
    if (!prepare_rule_declarations(
            context, declarations, declaration_length, parsed)) return false;
    for (int important = 0; important <= 1; important++) {
        if (!parsed->has_rules[important]) continue;
        if (base.selector == NULL) {
            base.selector = stylesheet_store_selector(
                sheet, selector, length);
            if (base.selector == NULL) return false;
            selector_assign_fast_key(&base, base.selector, length);
        }
        StyleRule rule = base;
        style_rule_set_container_query(
            &rule, sheet->current_container_query);
        rule.important = important != 0;
        rule.image_source_slot = parsed->image_source_slots[important];
        rule.declaration_index = parsed->declaration_indices[important];
        if (!add_rule(sheet, rule)) return false;
        if (rule.important) sheet->important_rule_count++;
    }
    if (base.selector != NULL
        && !add_revert_rule_mask(
            sheet, base.order, parsed->revert_rule_mask,
            parsed->revert_rule_mask_high)) {
        return false;
    }
    return true;
}

static size_t find_matching_brace(const char *css, size_t open, size_t length)
{
    int depth = 1;
    char quote = 0;
    for (size_t i = open + 1; i < length; i++) {
        if (quote != 0) {
            if (css[i] == '\\' && i + 1 < length) i++;
            else if (css[i] == quote) quote = 0;
            continue;
        }
        if (css[i] == '\'' || css[i] == '"') { quote = css[i]; continue; }
        if (css[i] == '/' && i + 1 < length && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < length && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i++;
            continue;
        }
        if (css[i] == '{') depth++;
        else if (css[i] == '}' && --depth == 0) return i;
    }
    return length;
}

static bool supports_variable_syntax(const char *text, size_t length)
{
    if (text == NULL || length == 0) return false;
    unsigned parentheses = 0;
    char quote = 0;
    bool found = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) text[i];
        if (quote != 0) {
            if (character == '\\' && i + 1 < length) i++;
            else if (character == (unsigned char) quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = (char) character;
            continue;
        }
        if (character == '/' && i + 1 < length && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < length
                   && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 >= length) return false;
            i++;
            continue;
        }
        if (character == ';' || character == '{' || character == '}') {
            return false;
        }
        bool identifier_before = i != 0
            && (isalnum((unsigned char) text[i - 1])
                || text[i - 1] == '-' || text[i - 1] == '_');
        if (!identifier_before && i + 4 <= length
            && strncasecmp(text + i, "var(", 4) == 0) {
            size_t name = i + 4;
            while (name < length
                   && isspace((unsigned char) text[name])) name++;
            if (name + 2 >= length || text[name] != '-'
                || text[name + 1] != '-') return false;
            size_t end = name + 2;
            while (end < length) {
                unsigned char part = (unsigned char) text[end];
                if (isalnum(part) || part == '-' || part == '_'
                    || part >= 0x80u) {
                    end++;
                } else if (part == '\\' && end + 1 < length) {
                    end += 2;
                } else {
                    break;
                }
            }
            if (end == name + 2) return false;
            while (end < length
                   && isspace((unsigned char) text[end])) end++;
            if (end >= length || (text[end] != ',' && text[end] != ')')) {
                return false;
            }
            found = true;
        }
        if (character == '(') {
            if (++parentheses > 16) return false;
        } else if (character == ')') {
            if (parentheses == 0) return false;
            parentheses--;
        }
    }
    return found && quote == 0 && parentheses == 0;
}

static bool supports_implemented_property(Stylesheet *sheet,
                                          const char *name,
                                          size_t name_length)
{
    if (name_length > 2 && name[0] == '-' && name[1] == '-') return true;
    if (retained_presentation_property(name, name_length) != NULL) {
        return true;
    }
    static const char *const retained_modern[] = {
        "user-select", "-webkit-user-select", "touch-action",
        "text-size-adjust", "-webkit-text-size-adjust", "resize",
        "text-wrap", "text-wrap-style", "translate", "rotate", "scale",
        "isolation", "hyphens", "tab-size", "font-kerning",
        "text-rendering", "mix-blend-mode", "backdrop-filter",
        "-webkit-backdrop-filter", "border-start-start-radius",
        "border-start-end-radius", "border-end-start-radius",
        "border-end-end-radius"
    };
    for (size_t i = 0;
         i < sizeof(retained_modern) / sizeof(retained_modern[0]); i++) {
        if (span_case_equal(name, name_length, retained_modern[i])) {
            return true;
        }
    }
    if (span_case_equal(name, name_length, "container")
        || span_case_equal(name, name_length, "container-name")
        || span_case_equal(name, name_length, "container-type")
        || span_case_equal(name, name_length, "columns")
        || span_case_equal(name, name_length, "column-count")
        || span_case_equal(name, name_length, "column-width")
        || span_case_equal(name, name_length, "break-inside")) {
        return true;
    }
    static const char *const probes[] = {
        "0", "1", "0px", "none", "auto", "normal", "block",
        "visible", "static", "row", "nowrap", "start", "stretch",
        "contain", "serif", "solid", "#000"
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        char declaration[160];
        int written = snprintf(declaration, sizeof(declaration), "%.*s:%s",
                               (int) name_length, name, probes[i]);
        if (written < 0 || (size_t) written >= sizeof(declaration)) {
            return false;
        }
        size_t program_count = sheet->math_program_count;
        size_t instruction_count = sheet->math_instruction_count;
        ComputedStyle ignored = {0};
        uint64_t mask = 0, mask_high = 0;
        bool parsed = style_parse_declarations(
            sheet, declaration, (size_t) written, &ignored, &mask,
            &mask_high, NULL, false, -1, NULL, NULL);
        style_math_restore(sheet, program_count, instruction_count);
        if (parsed && (mask != 0 || mask_high != 0)) return true;
    }
    return false;
}

static bool supports_ascii_identifier_list(const char *value, size_t length)
{
    bool in_identifier = false;
    bool saw_identifier = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) value[i];
        if (isspace(c)) {
            in_identifier = false;
            continue;
        }
        if (!(isalnum(c) || c == '-' || c == '_') || (!in_identifier
            && isdigit(c))) return false;
        in_identifier = true;
        saw_identifier = true;
    }
    return saw_identifier;
}

static bool supports_positive_integer(const char *value, size_t length)
{
    if (length == 0 || length > 9) return false;
    unsigned result = 0;
    for (size_t i = 0; i < length; i++) {
        if (!isdigit((unsigned char) value[i])) return false;
        result = result * 10u + (unsigned) (value[i] - '0');
        if (result > 1000000u) return false;
    }
    return result != 0;
}

static bool supports_retained_layout_value(
    Stylesheet *sheet, const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    if (span_case_equal(value, value_length, "inherit")
        || span_case_equal(value, value_length, "initial")
        || span_case_equal(value, value_length, "unset")
        || span_case_equal(value, value_length, "revert")
        || span_case_equal(value, value_length, "revert-layer")) {
        return true;
    }
    if (span_case_equal(name, name_length, "user-select")
        || span_case_equal(name, name_length, "-webkit-user-select")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "text")
            || span_case_equal(value, value_length, "none")
            || span_case_equal(value, value_length, "all");
    }
    if (span_case_equal(name, name_length, "touch-action")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "none")
            || span_case_equal(value, value_length, "manipulation")
            || span_case_equal(value, value_length, "pan-x")
            || span_case_equal(value, value_length, "pan-y")
            || span_case_equal(value, value_length, "pan-x pan-y")
            || span_case_equal(value, value_length, "pan-y pan-x");
    }
    if (span_case_equal(name, name_length, "text-size-adjust")
        || span_case_equal(name, name_length, "-webkit-text-size-adjust")) {
        if (span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "none")) return true;
        char text[32];
        if (value_length < 2 || value_length >= sizeof(text)) return false;
        memcpy(text, value, value_length);
        text[value_length] = '\0';
        char *end = NULL;
        double number = strtod(text, &end);
        return end != text && *end == '%' && end[1] == '\0'
            && isfinite(number) && number >= 0.0 && number <= 250.0;
    }
    if (span_case_equal(name, name_length, "resize")) {
        return span_case_equal(value, value_length, "none")
            || span_case_equal(value, value_length, "both")
            || span_case_equal(value, value_length, "horizontal")
            || span_case_equal(value, value_length, "vertical")
            || span_case_equal(value, value_length, "block")
            || span_case_equal(value, value_length, "inline");
    }
    if (span_case_equal(name, name_length, "text-wrap")
        || span_case_equal(name, name_length, "text-wrap-style")) {
        return span_case_equal(value, value_length, "wrap")
            || span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "balance")
            || span_case_equal(value, value_length, "pretty");
    }
    if (span_case_equal(name, name_length, "contain-intrinsic-size")) {
        size_t cursor = 0, start = 0, end = 0, lengths = 0;
        while (style_next_top_level(
                   value, value_length, ' ', &cursor, &start, &end)) {
            if (span_case_equal(value + start, end - start, "auto")) {
                continue;
            }
            if (++lengths > 2
                || !retained_presentation_length(
                    sheet, value + start, end - start, false)) {
                return false;
            }
        }
        return lengths == 1 || lengths == 2;
    }
    if (span_case_equal(name, name_length, "isolation")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "isolate");
    }
    if (span_case_equal(name, name_length, "border-start-start-radius")
        || span_case_equal(name, name_length, "border-start-end-radius")
        || span_case_equal(name, name_length, "border-end-start-radius")
        || span_case_equal(name, name_length, "border-end-end-radius")) {
        return retained_presentation_length(
            sheet, value, value_length, false);
    }
    if (span_case_equal(name, name_length, "translate")) {
        if (span_case_equal(value, value_length, "none")) return true;
        size_t cursor = 0, start = 0, end = 0, count = 0;
        while (style_next_top_level(
                   value, value_length, ' ', &cursor, &start, &end)) {
            if (++count > 2
                || !retained_presentation_length(
                       sheet, value + start, end - start, true)) return false;
        }
        return count == 1 || count == 2;
    }
    if (span_case_equal(name, name_length, "scale")) {
        if (span_case_equal(value, value_length, "none")) return true;
        char text[64];
        if (value_length == 0 || value_length >= sizeof(text)) return false;
        memcpy(text, value, value_length);
        text[value_length] = '\0';
        char *end = NULL;
        double first = strtod(text, &end);
        if (end == text || !isfinite(first) || first < 0.0) return false;
        while (isspace((unsigned char) *end)) end++;
        if (*end == '\0') return true;
        char *second_end = NULL;
        double second = strtod(end, &second_end);
        while (second_end != NULL && isspace((unsigned char) *second_end)) {
            second_end++;
        }
        return second_end != end && *second_end == '\0'
            && isfinite(second) && second >= 0.0
            && fabs(first - second) < 0.000001;
    }
    if (span_case_equal(name, name_length, "rotate")) {
        if (span_case_equal(value, value_length, "none")) return true;
        char text[48];
        if (value_length == 0 || value_length >= sizeof(text)) return false;
        memcpy(text, value, value_length);
        text[value_length] = '\0';
        char *end = NULL;
        double angle = strtod(text, &end);
        if (end == text || !isfinite(angle)) return false;
        if (strncasecmp(end, "deg", 3) == 0) end += 3;
        else if (strncasecmp(end, "turn", 4) == 0) {
            angle *= 360.0;
            end += 4;
        } else return false;
        while (isspace((unsigned char) *end)) end++;
        double quarter = angle / 90.0;
        return *end == '\0'
            && fabs(quarter - round(quarter)) < 0.000001;
    }
    if (span_case_equal(name, name_length, "cursor")) {
        static const char *const values[] = {
            "auto", "default", "none", "context-menu", "help", "pointer",
            "progress", "wait", "cell", "crosshair", "text",
            "vertical-text", "alias", "copy", "move", "no-drop",
            "not-allowed", "grab", "grabbing", "e-resize", "n-resize",
            "ne-resize", "nw-resize", "s-resize", "se-resize",
            "sw-resize", "w-resize", "ew-resize", "ns-resize",
            "nesw-resize", "nwse-resize", "col-resize", "row-resize",
            "all-scroll", "zoom-in", "zoom-out"
        };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            if (span_case_equal(value, value_length, values[i])) return true;
        }
        return false;
    }
    if (span_case_equal(name, name_length, "scroll-behavior")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "smooth");
    }
    if (span_case_equal(name, name_length, "scrollbar-width")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "thin")
            || span_case_equal(value, value_length, "none");
    }
    if (span_case_equal(name, name_length, "scrollbar-color")) {
        if (span_case_equal(value, value_length, "auto")) return true;
        size_t split = 0;
        while (split < value_length
               && !isspace((unsigned char) value[split])) split++;
        while (split < value_length
               && isspace((unsigned char) value[split])) split++;
        if (split == 0 || split >= value_length) return false;
        uint32_t ignored_color = 0;
        uint8_t ignored_alpha = 0;
        return style_parse_color_with_alpha(
                   sheet, value, split, &ignored_color, &ignored_alpha)
            && style_parse_color_with_alpha(
                   sheet, value + split, value_length - split,
                   &ignored_color, &ignored_alpha);
    }
    if (span_case_equal(name, name_length, "overscroll-behavior")
        || span_case_equal(name, name_length, "overscroll-behavior-x")
        || span_case_equal(name, name_length, "overscroll-behavior-y")
        || span_case_equal(name, name_length, "overscroll-behavior-inline")
        || span_case_equal(name, name_length, "overscroll-behavior-block")) {
        const char *first = value;
        size_t first_length = value_length;
        const char *second = NULL;
        size_t second_length = 0;
        if (span_case_equal(name, name_length, "overscroll-behavior")) {
            size_t split = 0;
            while (split < value_length
                   && !isspace((unsigned char) value[split])) split++;
            first_length = split;
            while (split < value_length
                   && isspace((unsigned char) value[split])) split++;
            if (split < value_length) {
                second = value + split;
                second_length = value_length - split;
            }
        }
        bool first_valid =
            span_case_equal(first, first_length, "auto")
            || span_case_equal(first, first_length, "contain")
            || span_case_equal(first, first_length, "none")
            || span_case_equal(first, first_length, "chain");
        return first_valid
            && (second == NULL
                || span_case_equal(second, second_length, "auto")
                || span_case_equal(second, second_length, "contain")
                || span_case_equal(second, second_length, "none")
                || span_case_equal(second, second_length, "chain"));
    }
    if (span_case_equal(name, name_length, "scroll-snap-type")) {
        size_t split = 0;
        while (split < value_length
               && !isspace((unsigned char) value[split])) split++;
        bool axis = span_case_equal(value, split, "none")
            || span_case_equal(value, split, "x")
            || span_case_equal(value, split, "y")
            || span_case_equal(value, split, "block")
            || span_case_equal(value, split, "inline")
            || span_case_equal(value, split, "both");
        while (split < value_length
               && isspace((unsigned char) value[split])) split++;
        return axis && (split == value_length
            || span_case_equal(value + split, value_length - split,
                               "mandatory")
            || span_case_equal(value + split, value_length - split,
                               "proximity"));
    }
    if (span_case_equal(name, name_length, "scroll-snap-align")) {
        size_t split = 0;
        while (split < value_length
               && !isspace((unsigned char) value[split])) split++;
        bool first = span_case_equal(value, split, "none")
            || span_case_equal(value, split, "start")
            || span_case_equal(value, split, "end")
            || span_case_equal(value, split, "center");
        while (split < value_length
               && isspace((unsigned char) value[split])) split++;
        return first && (split == value_length
            || span_case_equal(value + split, value_length - split, "none")
            || span_case_equal(value + split, value_length - split, "start")
            || span_case_equal(value + split, value_length - split, "end")
            || span_case_equal(value + split, value_length - split, "center"));
    }
    if (span_case_equal(name, name_length, "scroll-snap-stop")) {
        return span_case_equal(value, value_length, "normal")
            || span_case_equal(value, value_length, "always");
    }
    if (span_case_equal(name, name_length, "scroll-padding")
        || span_case_equal(name, name_length, "scroll-margin")) {
        size_t cursor = 0, start = 0, end = 0, count = 0;
        while (style_next_top_level(
                   value, value_length, ' ', &cursor, &start, &end)) {
            if (++count > 4
                || (!(span_case_equal(
                          name, name_length, "scroll-padding")
                      && span_case_equal(
                          value + start, end - start, "auto"))
                    && !retained_presentation_length(
                        sheet, value + start, end - start,
                        span_case_equal(
                            name, name_length, "scroll-margin")))) {
                return false;
            }
        }
        return count != 0;
    }
    if (span_case_equal(name, name_length, "scroll-padding-top")
        || span_case_equal(name, name_length, "scroll-padding-right")
        || span_case_equal(name, name_length, "scroll-padding-bottom")
        || span_case_equal(name, name_length, "scroll-padding-left")) {
        return span_case_equal(value, value_length, "auto")
            || retained_presentation_length(
                sheet, value, value_length, false);
    }
    if (span_case_equal(name, name_length, "scroll-margin-top")
        || span_case_equal(name, name_length, "scroll-margin-right")
        || span_case_equal(name, name_length, "scroll-margin-bottom")
        || span_case_equal(name, name_length, "scroll-margin-left")) {
        return retained_presentation_length(
            sheet, value, value_length, true);
    }
    if (span_case_equal(name, name_length, "container-type")) {
        return span_case_equal(value, value_length, "normal")
            || span_case_equal(value, value_length, "size")
            || span_case_equal(value, value_length, "inline-size");
    }
    if (span_case_equal(name, name_length, "container-name")) {
        return span_case_equal(value, value_length, "none")
            || supports_ascii_identifier_list(value, value_length);
    }
    if (span_case_equal(name, name_length, "container")) {
        if (span_case_equal(value, value_length, "none")) return true;
        const char *slash = memchr(value, '/', value_length);
        if (slash == NULL) return false;
        const char *container_name = value;
        size_t container_name_length = (size_t) (slash - value);
        const char *container_type = slash + 1;
        size_t container_type_length =
            value_length - container_name_length - 1u;
        trim(&container_name, &container_name_length);
        trim(&container_type, &container_type_length);
        return supports_ascii_identifier_list(
                   container_name, container_name_length)
            && (span_case_equal(
                    container_type, container_type_length, "normal")
                || span_case_equal(
                    container_type, container_type_length, "size")
                || span_case_equal(
                    container_type, container_type_length, "inline-size"));
    }
    if (span_case_equal(name, name_length, "column-count")) {
        return span_case_equal(value, value_length, "auto")
            || supports_positive_integer(value, value_length);
    }
    if (span_case_equal(name, name_length, "column-width")) {
        return span_case_equal(value, value_length, "auto")
            || retained_presentation_length(
                sheet, value, value_length, false);
    }
    if (span_case_equal(name, name_length, "column-gap")) {
        return span_case_equal(value, value_length, "normal")
            || retained_presentation_length(
                sheet, value, value_length, false);
    }
    if (span_case_equal(name, name_length, "columns")) {
        size_t separator = 0;
        while (separator < value_length
               && !isspace((unsigned char) value[separator])) separator++;
        const char *first = value;
        size_t first_length = separator;
        while (separator < value_length
               && isspace((unsigned char) value[separator])) separator++;
        if (separator == value_length) {
            return span_case_equal(first, first_length, "auto")
                || supports_positive_integer(first, first_length)
                || retained_presentation_length(
                    sheet, first, first_length, false);
        }
        const char *second = value + separator;
        size_t second_length = value_length - separator;
        return ((span_case_equal(first, first_length, "auto")
                 || supports_positive_integer(first, first_length))
                && (span_case_equal(second, second_length, "auto")
                    || retained_presentation_length(
                        sheet, second, second_length, false)))
            || ((span_case_equal(second, second_length, "auto")
                 || supports_positive_integer(second, second_length))
                && (span_case_equal(first, first_length, "auto")
                    || retained_presentation_length(
                        sheet, first, first_length, false)));
    }
    if (span_case_equal(name, name_length, "break-inside")) {
        return span_case_equal(value, value_length, "auto")
            || span_case_equal(value, value_length, "avoid")
            || span_case_equal(value, value_length, "avoid-page")
            || span_case_equal(value, value_length, "avoid-column");
    }
    return false;
}

static bool supports_word_at(const char *text, size_t length, size_t at,
                             const char *word)
{
    size_t word_length = strlen(word);
    if (at + word_length > length) return false;
    for (size_t i = 0; i < word_length; i++) {
        if (tolower((unsigned char) text[at + i])
            != tolower((unsigned char) word[i])) return false;
    }
    return true;
}

static bool supports_selector_pseudo(const char *name, size_t length)
{
    static const char *const supported[] = {
        "active", "any-link", "checked", "defined", "disabled", "empty",
        "enabled", "first-child", "first-of-type", "focus",
        "focus-visible", "focus-within", "has", "hover", "is", "lang",
        "last-child", "last-of-type", "link", "modal", "not", "nth-child",
        "nth-last-child", "nth-last-of-type", "nth-of-type", "only-child",
        "only-of-type", "open", "optional", "popover-open", "required",
        "root", "visited", "where", "-moz-any", "-webkit-any",
        "before", "after", "marker"
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strlen(supported[i]) == length
            && strncasecmp(name, supported[i], length) == 0) return true;
    }
    return false;
}

static bool supports_selector_syntax(const char *text, size_t length)
{
    trim(&text, &length);
    if (length == 0 || length >= STYLE_SELECTOR_CAPACITY) return false;
    unsigned round = 0, square = 0;
    char quote = 0;
    bool token = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) text[i];
        if (quote != 0) {
            if (character == '\\' && i + 1 < length) i++;
            else if (character == (unsigned char) quote) quote = 0;
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = (char) character;
        } else if (character == '[') {
            if (++square > 8) return false;
            token = true;
        } else if (character == ']') {
            if (square == 0) return false;
            square--;
        } else if (character == '(') {
            if (++round > 8) return false;
        } else if (character == ')') {
            if (round == 0) return false;
            round--;
        } else if (character == '{' || character == '}' || character == ';'
                   || character == '@') {
            return false;
        } else if (character == ':') {
            size_t name = i + 1;
            if (name < length && text[name] == ':') name++;
            size_t end = name;
            while (end < length
                   && (isalnum((unsigned char) text[end])
                       || text[end] == '-' || text[end] == '_')) end++;
            if (end == name
                || !supports_selector_pseudo(
                       text + name, end - name)) return false;
            i = end - 1;
            token = true;
        } else if (!isspace(character) && character != ','
                   && character != '>' && character != '+'
                   && character != '~' && character != '&') {
            token = true;
        }
    }
    return token && quote == 0 && round == 0 && square == 0;
}

static bool supports_matches_depth(Stylesheet *sheet, const char *text,
                                   size_t length, unsigned depth)
{
    trim(&text, &length);
    if (depth >= 8 || length == 0) return false;
    if (length > 3 && supports_word_at(text, length, 0, "not")
        && isspace((unsigned char) text[3])) {
        return !supports_matches_depth(sheet, text + 4, length - 4,
                                       depth + 1);
    }
    if (text[0] == '(' && text[length - 1] == ')') {
        int nesting = 0;
        bool encloses_all = true;
        char quote = 0;
        for (size_t i = 0; i < length; i++) {
            if (quote != 0) {
                if (text[i] == '\\' && i + 1 < length) i++;
                else if (text[i] == quote) quote = 0;
                continue;
            }
            if (text[i] == '\'' || text[i] == '"') {
                quote = text[i];
            } else if (text[i] == '(') {
                nesting++;
            } else if (text[i] == ')' && --nesting == 0
                       && i + 1 < length) {
                encloses_all = false;
                break;
            }
        }
        if (encloses_all && quote == 0 && nesting == 0) {
            return supports_matches_depth(sheet, text + 1, length - 2,
                                           depth + 1);
        }
    }
    int nesting = 0, square = 0;
    char quote = 0;
    size_t operator_at = SIZE_MAX;
    size_t operator_length = 0;
    unsigned operator_kind = 0;
    for (size_t i = 0; i < length; i++) {
        if (quote != 0) {
            if (text[i] == '\\' && i + 1 < length) i++;
            else if (text[i] == quote) quote = 0;
            continue;
        }
        if (text[i] == '\'' || text[i] == '"') {
            quote = text[i];
        } else if (text[i] == '(') {
            nesting++;
        } else if (text[i] == ')') {
            if (nesting == 0) return false;
            nesting--;
        } else if (text[i] == '[') {
            square++;
        } else if (text[i] == ']') {
            if (square == 0) return false;
            square--;
        }
        if (nesting != 0 || square != 0 || !isspace((unsigned char) text[i])) {
            continue;
        }
        size_t word = i;
        while (word < length && isspace((unsigned char) text[word])) word++;
        unsigned kind = supports_word_at(text, length, word, "or")
                        && word + 2 < length
                        && isspace((unsigned char) text[word + 2])
            ? 1u
            : (supports_word_at(text, length, word, "and")
               && word + 3 < length
               && isspace((unsigned char) text[word + 3])
                   ? 2u : 0u);
        if (kind == 0) continue;
        if (operator_kind != 0 && operator_kind != kind) return false;
        operator_kind = kind;
        if (operator_at == SIZE_MAX) {
            operator_at = i;
            operator_length = (word - i) + (kind == 1 ? 2u : 3u);
        }
    }
    if (quote != 0 || nesting != 0 || square != 0) return false;
    if (operator_at != SIZE_MAX) {
        size_t right = operator_at + operator_length;
        while (right < length
               && isspace((unsigned char) text[right])) right++;
        bool left_result = supports_matches_depth(
            sheet, text, operator_at, depth + 1);
        bool right_result = supports_matches_depth(
            sheet, text + right, length - right, depth + 1);
        return operator_kind == 1
            ? left_result || right_result
            : left_result && right_result;
    }
    if (length > 10 && supports_word_at(text, length, 0, "selector")
        && text[8] == '(' && text[length - 1] == ')') {
        return supports_selector_syntax(text + 9, length - 10);
    }
    const char *colon = memchr(text, ':', length);
    if (colon == NULL) return false;
    const char *name = text;
    size_t name_length = (size_t) (colon - text);
    const char *value = colon + 1;
    size_t value_length = length - name_length - 1;
    trim(&name, &name_length);
    trim(&value, &value_length);
    char normalized_name[96];
    if (name_length == 0 || name_length >= sizeof(normalized_name)) {
        return false;
    }
    for (size_t i = 0; i < name_length; i++) {
        normalized_name[i] =
            (char) tolower((unsigned char) name[i]);
    }
    normalized_name[name_length] = '\0';
    name = normalized_name;
    if (span_case_equal(value, value_length, "revert-rule")) {
        return supports_implemented_property(sheet, name, name_length);
    }
    if (retained_presentation_property(name, name_length) != NULL) {
        return retained_presentation_value_supported(
            sheet, name, name_length, value, value_length);
    }
    if (span_case_equal(name, name_length, "container")
        || span_case_equal(name, name_length, "container-name")
        || span_case_equal(name, name_length, "container-type")
        || span_case_equal(name, name_length, "columns")
        || span_case_equal(name, name_length, "column-count")
        || span_case_equal(name, name_length, "column-width")
        || span_case_equal(name, name_length, "column-gap")
        || span_case_equal(name, name_length, "break-inside")
        || span_case_equal(name, name_length, "cursor")
        || span_case_equal(name, name_length, "overscroll-behavior")
        || span_case_equal(name, name_length, "overscroll-behavior-x")
        || span_case_equal(name, name_length, "overscroll-behavior-y")
        || span_case_equal(name, name_length, "overscroll-behavior-inline")
        || span_case_equal(name, name_length, "overscroll-behavior-block")
        || span_case_equal(name, name_length, "scroll-behavior")
        || span_case_equal(name, name_length, "scroll-margin")
        || span_case_equal(name, name_length, "scroll-margin-top")
        || span_case_equal(name, name_length, "scroll-margin-right")
        || span_case_equal(name, name_length, "scroll-margin-bottom")
        || span_case_equal(name, name_length, "scroll-margin-left")
        || span_case_equal(name, name_length, "scroll-padding")
        || span_case_equal(name, name_length, "scroll-padding-top")
        || span_case_equal(name, name_length, "scroll-padding-right")
        || span_case_equal(name, name_length, "scroll-padding-bottom")
        || span_case_equal(name, name_length, "scroll-padding-left")
        || span_case_equal(name, name_length, "scroll-snap-align")
        || span_case_equal(name, name_length, "scroll-snap-stop")
        || span_case_equal(name, name_length, "scroll-snap-type")
        || span_case_equal(name, name_length, "scrollbar-color")
        || span_case_equal(name, name_length, "scrollbar-width")
        || span_case_equal(name, name_length, "user-select")
        || span_case_equal(name, name_length, "-webkit-user-select")
        || span_case_equal(name, name_length, "touch-action")
        || span_case_equal(name, name_length, "text-size-adjust")
        || span_case_equal(name, name_length, "-webkit-text-size-adjust")
        || span_case_equal(name, name_length, "resize")
        || span_case_equal(name, name_length, "text-wrap")
        || span_case_equal(name, name_length, "text-wrap-style")
        || span_case_equal(name, name_length, "translate")
        || span_case_equal(name, name_length, "rotate")
        || span_case_equal(name, name_length, "scale")
        || span_case_equal(name, name_length, "isolation")
        || span_case_equal(name, name_length, "border-start-start-radius")
        || span_case_equal(name, name_length, "border-start-end-radius")
        || span_case_equal(name, name_length, "border-end-start-radius")
        || span_case_equal(name, name_length, "border-end-end-radius")) {
        return supports_retained_layout_value(
            sheet, name, name_length, value, value_length);
    }
    if (supports_variable_syntax(value, value_length)) {
        return supports_implemented_property(sheet, name, name_length);
    }
    if (span_equal(name, name_length, "display")) {
        char resolved[48];
        if (!style_resolve_value(sheet, value, value_length, resolved,
                           sizeof(resolved), 0)) return false;
        static const char *supported[] = {
            "none", "inline", "inline-block", "block", "contents",
            "flow-root", "list-item", "flex", "inline-flex", "grid",
            "inline-grid", "table", "table-row", "table-cell",
            "table-row-group", "table-header-group", "table-footer-group",
            "table-column", "table-column-group", "table-caption"
        };
        for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
            if (strcmp(resolved, supported[i]) == 0) return true;
        }
        return false;
    }
    char declaration[320];
    int written = snprintf(declaration, sizeof(declaration), "%.*s:%.*s",
                           (int) name_length, name, (int) value_length, value);
    if (written < 0 || (size_t) written >= sizeof(declaration)) return false;
    ComputedStyle ignored = {0};
    uint64_t mask = 0;
    uint64_t mask_high = 0;
    size_t program_count = sheet->math_program_count;
    size_t instruction_count = sheet->math_instruction_count;
    bool parsed = style_parse_declarations(
        sheet, declaration, (size_t) written, &ignored, &mask, &mask_high,
        NULL, false, -1, NULL, NULL);
    /* Capability and diagnostic probes are observational.  Retaining their
       packed programs can starve the fixed pool before a matching rule is
       ever resolved. */
    style_math_restore(sheet, program_count, instruction_count);
    return parsed && (mask != 0 || mask_high != 0);
}

static bool supports_matches(Stylesheet *sheet, const char *text,
                             size_t length)
{
    return supports_matches_depth(sheet, text, length, 0);
}

bool stylesheet_supports_matches(Stylesheet *sheet, const char *query,
                                 size_t query_length)
{
    return sheet != NULL && query != NULL
           && supports_matches(sheet, query, query_length);
}

#ifndef TILEFINCH_NO_TRACE
static void diagnostic_record_rejected_property(Stylesheet *sheet,
                                                 const char *name,
                                                 size_t name_length)
{
    if (sheet == NULL || name == NULL || name_length == 0) return;
    size_t retained = name_length;
    if (retained >= STYLE_DIAGNOSTIC_PROPERTY_NAME_CAPACITY) {
        retained = STYLE_DIAGNOSTIC_PROPERTY_NAME_CAPACITY - 1;
    }
    for (size_t i = 0; i < sheet->diagnostic_rejected_property_count; i++) {
        StyleDiagnosticProperty *entry =
            &sheet->diagnostic_rejected_properties[i];
        if (strlen(entry->name) == retained
            && memcmp(entry->name, name, retained) == 0) {
            if (entry->count != UINT32_MAX) entry->count++;
            return;
        }
    }
    if (sheet->diagnostic_rejected_property_count
        >= STYLE_DIAGNOSTIC_PROPERTY_CAPACITY) return;
    StyleDiagnosticProperty *entry =
        &sheet->diagnostic_rejected_properties[
            sheet->diagnostic_rejected_property_count++];
    memcpy(entry->name, name, retained);
    entry->name[retained] = '\0';
    entry->count = 1;
}

static void diagnose_declarations(Stylesheet *sheet, const char *text,
                                  size_t length)
{
    if (sheet == NULL || text == NULL) return;
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
            if (name_length > 2 && name[0] == '-' && name[1] == '-') {
                at = end + (end < length);
                continue;
            }
            diagnostic_saturating_add(&sheet->diagnostic_declarations, 1);
            if (declaration_is_important(value, value_length)) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            if (span_contains_literal(value, value_length, "var(")) {
                diagnostic_saturating_add(
                    &sheet->diagnostic_deferred_declarations, 1);
                at = end + (end < length);
                continue;
            }
            char condition[320];
            int written = snprintf(condition, sizeof(condition), "%.*s:%.*s",
                                   (int) name_length, name,
                                   (int) value_length, value);
            bool supported = written >= 0
                             && (size_t) written < sizeof(condition)
                             && supports_matches_depth(
                                    sheet, condition, (size_t) written, 0);
            if (supported) {
                diagnostic_saturating_add(
                    &sheet->diagnostic_supported_declarations, 1);
            } else {
                diagnostic_saturating_add(
                    &sheet->diagnostic_rejected_declarations, 1);
                diagnostic_record_rejected_property(
                    sheet, name, name_length);
            }
        }
        at = end + (end < length);
    }
}
#else
static void diagnose_declarations(Stylesheet *sheet, const char *text,
                                  size_t length)
{
    (void) sheet;
    (void) text;
    (void) length;
}
#endif

static unsigned register_layer(Stylesheet *sheet, const char *name,
                               size_t name_length)
{
    trim(&name, &name_length);
    _Static_assert(STYLE_LAYER_CAPACITY < UINT8_MAX,
                   "layer IDs must fit in the packed cascade key");
    unsigned parent = sheet->current_layer == UINT_MAX
        ? UINT8_MAX : (sheet->current_layer & UINT8_MAX);
    if (name_length == 0) {
        if (sheet->layer_count >= STYLE_LAYER_CAPACITY) {
            return STYLE_LAYER_CAPACITY - 1;
        }
        size_t anonymous = sheet->layer_count++;
        sheet->layer_parents[anonymous] = (uint8_t) parent;
        return (unsigned) anonymous;
    }

    /* A layer's durable identity is its qualified path.  Register every
       path component so nested order can be flattened according to the
       cascade spec without adding a heap-backed layer tree. */
    char qualified[STYLE_LAYER_NAME_CAPACITY];
    size_t qualified_length = 0;
    if (parent != UINT8_MAX && parent < sheet->layer_count) {
        qualified_length = strlen(sheet->layer_names[parent]);
        if (qualified_length != 0
            && qualified_length + 1 < sizeof(qualified)) {
            memcpy(qualified, sheet->layer_names[parent], qualified_length);
            qualified[qualified_length++] = '.';
        }
    }
    unsigned result = parent;
    for (size_t at = 0; at < name_length;) {
        while (at < name_length
               && isspace((unsigned char) name[at])) at++;
        size_t end = at;
        while (end < name_length && name[end] != '.') end++;
        size_t component_end = end;
        while (component_end > at
               && isspace((unsigned char) name[component_end - 1])) {
            component_end--;
        }
        size_t component_length = component_end - at;
        if (component_length == 0
            || qualified_length + component_length
                   >= sizeof(qualified)) {
            return result == UINT8_MAX ? STYLE_LAYER_CAPACITY - 1 : result;
        }
        memcpy(qualified + qualified_length, name + at, component_length);
        qualified_length += component_length;
        qualified[qualified_length] = '\0';

        size_t found = sheet->layer_count;
        for (size_t i = 0; i < sheet->layer_count; i++) {
            if (strlen(sheet->layer_names[i]) == qualified_length
                && memcmp(sheet->layer_names[i], qualified,
                          qualified_length) == 0) {
                found = i;
                break;
            }
        }
        if (found == sheet->layer_count) {
            if (sheet->layer_count >= STYLE_LAYER_CAPACITY) {
                return result == UINT8_MAX
                    ? STYLE_LAYER_CAPACITY - 1 : result;
            }
            found = sheet->layer_count++;
            memcpy(sheet->layer_names[found], qualified,
                   qualified_length + 1u);
            sheet->layer_parents[found] = (uint8_t) result;
        }
        result = (unsigned) found;
        if (end < name_length) {
            if (qualified_length + 1 >= sizeof(qualified)) return result;
            qualified[qualified_length++] = '.';
            at = end + 1;
        } else {
            at = end;
        }
    }
    return result;
}
