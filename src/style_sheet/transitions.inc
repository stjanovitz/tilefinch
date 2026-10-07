/* Transition declarations. Layout never animates, so ComputedStyle holds no
   transition state; these are retained only so getComputedStyle and the
   script transition lifecycle (motion.js) see the cascade. Each authored
   declaration keeps its text in the selector arena and is parsed when an
   element is asked about. */

static int transition_declaration_kind(const char *name, size_t length)
{
    if (length > 8 && strncasecmp(name, "-webkit-", 8) == 0) {
        name += 8;
        length -= 8;
    }
    if (length < 10 || strncasecmp(name, "transition", 10) != 0) return -1;
    if (length == 10) return STYLE_TRANSITION_SHORTHAND;
    if (span_case_equal(name, length, "transition-property"))
        return STYLE_TRANSITION_PROPERTY;
    if (span_case_equal(name, length, "transition-duration"))
        return STYLE_TRANSITION_DURATION;
    if (span_case_equal(name, length, "transition-delay"))
        return STYLE_TRANSITION_DELAY;
    if (span_case_equal(name, length, "transition-timing-function"))
        return STYLE_TRANSITION_TIMING;
    return -1;
}

/* The next top-level comma-separated item, trimmed. *at moves past its
   comma; an item ending the text leaves *at beyond length. */
static void transition_list_item(const char *text, size_t length,
                                 size_t *at, const char **item,
                                 size_t *item_length)
{
    size_t start = *at, depth = 0, end = start;
    for (; end < length; end++) {
        if (text[end] == '(') depth++;
        else if (text[end] == ')' && depth != 0) depth--;
        else if (text[end] == ',' && depth == 0) break;
    }
    *item = text + start;
    *item_length = end - start;
    trim(item, item_length);
    *at = end + 1;
}

/* The next whitespace-separated token of an item; functions stay whole. */
static bool transition_token(const char *text, size_t length, size_t *at,
                             const char **token, size_t *token_length)
{
    size_t start = *at;
    while (start < length && isspace((unsigned char) text[start])) start++;
    if (start >= length) return false;
    size_t end = start, depth = 0;
    for (; end < length; end++) {
        if (text[end] == '(') depth++;
        else if (text[end] == ')' && depth != 0) depth--;
        else if (depth == 0 && isspace((unsigned char) text[end])) break;
    }
    *token = text + start;
    *token_length = end - start;
    *at = end;
    return true;
}

static bool transition_time_ms(const char *text, size_t length, double *ms)
{
    char buffer[32];
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
    char *unit = NULL;
    double value = strtod(buffer, &unit);
    if (unit == buffer || !isfinite(value)) return false;
    if (strcasecmp(unit, "s") == 0) value *= 1000.0;
    else if (strcasecmp(unit, "ms") != 0) return false;
    *ms = value;
    return true;
}

/* A timing function, normalized as computed values serialize it: keywords
   lower-case, a function's arguments separated by ", ". */
static bool transition_timing(const char *text, size_t length,
                              char *output, size_t output_size)
{
    static const char *const keywords[] = {
        "ease", "linear", "ease-in", "ease-out", "ease-in-out",
        "step-start", "step-end"
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (span_case_equal(text, length, keywords[i])) {
            snprintf(output, output_size, "%s", keywords[i]);
            return true;
        }
    }
    static const char *const functions[] = {
        "cubic-bezier", "steps", "linear"
    };
    size_t open = 0;
    while (open < length && text[open] != '(') open++;
    if (open == length || text[length - 1] != ')') return false;
    const char *name = NULL;
    for (size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); i++) {
        if (span_case_equal(text, open, functions[i])) name = functions[i];
    }
    if (name == NULL) return false;
    size_t used = (size_t) snprintf(output, output_size, "%s(", name);
    size_t arguments_end = length - 1;
    for (size_t at = open + 1; at <= arguments_end;) {
        const char *argument = NULL;
        size_t argument_length = 0;
        transition_list_item(text, arguments_end, &at, &argument,
                             &argument_length);
        if (argument_length == 0) return false;
        /* Numbers serialize canonically (.2 is 0.2); keywords as written. */
        char number_text[32] = "";
        double number = 0.0;
        if (argument_length < sizeof(number_text)) {
            char copy[32];
            memcpy(copy, argument, argument_length);
            copy[argument_length] = '\0';
            char *end = NULL;
            number = strtod(copy, &end);
            if (end != copy && *end == '\0' && isfinite(number))
                snprintf(number_text, sizeof(number_text), "%g", number);
        }
        int written = number_text[0] != '\0'
            ? snprintf(output + used, output_size - used, "%s%s",
                       used > strlen(name) + 1u ? ", " : "", number_text)
            : snprintf(output + used, output_size - used, "%s%.*s",
                       used > strlen(name) + 1u ? ", " : "",
                       (int) argument_length, argument);
        if (written < 0 || (size_t) written >= output_size - used)
            return false;
        used += (size_t) written;
    }
    if (used + 2u > output_size) return false;
    output[used++] = ')';
    output[used] = '\0';
    return true;
}

static bool transition_property_name(const char *text, size_t length,
                                     char *output, size_t output_size)
{
    if (length == 0 || length >= output_size) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) text[i];
        bool valid = isalpha(character) || character == '-'
            || character == '_' || character >= 0x80
            || (i != 0 && isdigit(character));
        if (!valid) return false;
        output[i] = (char) tolower(character);
    }
    output[length] = '\0';
    return true;
}

static void transition_initial(StyleTransitionComputed *output)
{
    memset(output, 0, sizeof(*output));
    output->property_count = output->duration_count = 1;
    output->delay_count = output->timing_count = 1;
    snprintf(output->properties[0], sizeof(output->properties[0]), "all");
    snprintf(output->timings[0], sizeof(output->timings[0]), "ease");
}

/* Parses one declaration's value into the lists it sets: all four for the
   shorthand, one for a longhand. False for an invalid value. */
static bool transition_parse(StyleTransitionKind kind, const char *text,
                             size_t length, StyleTransitionComputed *output)
{
    StyleTransitionComputed parsed;
    transition_initial(&parsed);
    size_t items = 0;
    bool none = false;
    for (size_t at = 0; at <= length;) {
        const char *item = NULL;
        size_t item_length = 0;
        transition_list_item(text, length, &at, &item, &item_length);
        if (item_length == 0) return false;
        if (items == STYLE_TRANSITION_LIST_LIMIT) continue;
        size_t slot = items++;
        if (kind == STYLE_TRANSITION_PROPERTY) {
            if (!transition_property_name(
                    item, item_length, parsed.properties[slot],
                    sizeof(parsed.properties[slot]))) return false;
            none |= strcmp(parsed.properties[slot], "none") == 0;
            continue;
        }
        if (kind == STYLE_TRANSITION_DURATION
            || kind == STYLE_TRANSITION_DELAY) {
            double *list = kind == STYLE_TRANSITION_DURATION
                ? parsed.duration_ms : parsed.delay_ms;
            if (!transition_time_ms(item, item_length, &list[slot])
                || (kind == STYLE_TRANSITION_DURATION && list[slot] < 0.0))
                return false;
            continue;
        }
        if (kind == STYLE_TRANSITION_TIMING) {
            if (!transition_timing(item, item_length, parsed.timings[slot],
                                   sizeof(parsed.timings[slot])))
                return false;
            continue;
        }
        /* Shorthand: [property] [duration] [timing] [delay] [behavior] in
           any order, the first time being the duration. */
        snprintf(parsed.properties[slot], sizeof(parsed.properties[slot]),
                 "all");
        snprintf(parsed.timings[slot], sizeof(parsed.timings[slot]), "ease");
        parsed.duration_ms[slot] = parsed.delay_ms[slot] = 0.0;
        bool have_property = false, have_timing = false;
        bool have_behavior = false;
        unsigned times = 0;
        const char *token = NULL;
        size_t token_length = 0;
        for (size_t cursor = 0; transition_token(
                 item, item_length, &cursor, &token, &token_length);) {
            double time = 0.0;
            if (transition_time_ms(token, token_length, &time)) {
                if (times == 2u) return false;
                if (times++ == 0u) {
                    if (time < 0.0) return false;
                    parsed.duration_ms[slot] = time;
                } else {
                    parsed.delay_ms[slot] = time;
                }
            } else if (transition_timing(
                           token, token_length, parsed.timings[slot],
                           sizeof(parsed.timings[slot]))) {
                if (have_timing) return false;
                have_timing = true;
            } else if (span_case_equal(token, token_length, "normal")
                       || span_case_equal(token, token_length,
                                          "allow-discrete")) {
                if (have_behavior) return false;
                have_behavior = true;
            } else if (!have_property && transition_property_name(
                           token, token_length, parsed.properties[slot],
                           sizeof(parsed.properties[slot]))) {
                have_property = true;
            } else {
                return false;
            }
        }
        none |= strcmp(parsed.properties[slot], "none") == 0;
    }
    /* `none` is only valid as the whole property list. */
    if (none && items > 1) return false;
    if (kind == STYLE_TRANSITION_SHORTHAND || kind == STYLE_TRANSITION_PROPERTY) {
        memcpy(output->properties, parsed.properties,
               sizeof(output->properties));
        output->property_count = (uint8_t) items;
    }
    if (kind == STYLE_TRANSITION_SHORTHAND || kind == STYLE_TRANSITION_DURATION) {
        memcpy(output->duration_ms, parsed.duration_ms,
               sizeof(output->duration_ms));
        output->duration_count = (uint8_t) items;
    }
    if (kind == STYLE_TRANSITION_SHORTHAND || kind == STYLE_TRANSITION_DELAY) {
        memcpy(output->delay_ms, parsed.delay_ms, sizeof(output->delay_ms));
        output->delay_count = (uint8_t) items;
    }
    if (kind == STYLE_TRANSITION_SHORTHAND || kind == STYLE_TRANSITION_TIMING) {
        memcpy(output->timings, parsed.timings, sizeof(output->timings));
        output->timing_count = (uint8_t) items;
    }
    return true;
}

static bool transition_value_is_css_wide(const char *text, size_t length)
{
    return span_case_equal(text, length, "initial")
        || span_case_equal(text, length, "inherit")
        || span_case_equal(text, length, "unset")
        || span_case_equal(text, length, "revert")
        || span_case_equal(text, length, "revert-layer");
}

static bool transition_value_has_var(const char *text, size_t length)
{
    for (size_t i = 0; i + 4u <= length; i++) {
        if (strncasecmp(text + i, "var(", 4) == 0) return true;
    }
    return false;
}

/* Retains one authored transition declaration. A value that cannot parse
   is dropped as CSS drops an invalid declaration; one using var() is kept
   and checked when it is resolved. */
static bool add_transition_rule(Stylesheet *sheet, const char *selector,
                                size_t selector_length,
                                StyleTransitionKind kind, const char *value,
                                size_t value_length, unsigned specificity,
                                unsigned order, bool important,
                                PseudoElement pseudo)
{
    if (selector_length >= STYLE_SELECTOR_CAPACITY
        || value_length >= STYLE_SELECTOR_CAPACITY) return true;
    if (!transition_value_is_css_wide(value, value_length)
        && !transition_value_has_var(value, value_length)) {
        StyleTransitionComputed parsed;
        transition_initial(&parsed);
        if (!transition_parse(kind, value, value_length, &parsed)) return true;
    }
    if (sheet->transition_rule_count == sheet->transition_rule_capacity) {
        size_t capacity = sheet->transition_rule_capacity == 0
            ? 16 : sheet->transition_rule_capacity * 2;
        StyleTransitionRule *rules = budget_realloc(
            sheet->budget, sheet->transition_rules,
            capacity * sizeof(*rules));
        if (rules == NULL) return false;
        sheet->transition_rules = rules;
        sheet->transition_rule_capacity = capacity;
    }
    const char *stored_selector = stylesheet_store_selector(
        sheet, selector, selector_length);
    const char *stored_value = stylesheet_store_selector(
        sheet, value, value_length);
    if (stored_selector == NULL || stored_value == NULL) return false;
    sheet->transition_rules[sheet->transition_rule_count++] =
        (StyleTransitionRule) {
            .selector = stored_selector,
            .value = stored_value,
            .selector_length = (uint16_t) selector_length,
            .value_length = (uint16_t) value_length,
            .kind = (uint8_t) kind,
            .pseudo = (uint8_t) pseudo,
            .container_query = sheet->current_container_query,
            .important = important,
            .origin = sheet->current_origin,
            .layer = sheet->current_layer,
            .specificity = specificity,
            .order = order
        };
    return true;
}

static bool transition_rule_later(const StyleTransitionRule *candidate,
                                  const StyleTransitionRule *current)
{
    if (current == NULL) return true;
    if (candidate->important != current->important)
        return candidate->important;
    if (candidate->origin != current->origin)
        return candidate->origin > current->origin;
    if (candidate->layer != current->layer) {
        if (!candidate->important) {
            if (candidate->layer == UINT_MAX) return true;
            if (current->layer == UINT_MAX) return false;
            return (candidate->layer >> 8) > (current->layer >> 8);
        }
        if (candidate->layer == UINT_MAX) return false;
        if (current->layer == UINT_MAX) return true;
        return (candidate->layer >> 8) < (current->layer >> 8);
    }
    if (candidate->specificity != current->specificity)
        return candidate->specificity > current->specificity;
    return candidate->order >= current->order;
}

static void transition_consider(const StyleTransitionRule *rule,
                                const StyleTransitionRule *winners[4])
{
    for (unsigned longhand = 0; longhand < 4u; longhand++) {
        if ((rule->kind == STYLE_TRANSITION_SHORTHAND
             || rule->kind == longhand + 1u)
            && transition_rule_later(rule, winners[longhand]))
            winners[longhand] = rule;
    }
}

bool style_transition_computed(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    StyleTransitionComputed *output)
{
    if (output == NULL) return false;
    transition_initial(output);
    if (sheet == NULL || node == NULL) return true;
    const StyleTransitionRule *winners[4] = {NULL, NULL, NULL, NULL};
    if (sheet->transition_rule_count != 0) {
        Stylesheet *mutable_sheet = (Stylesheet *) sheet;
        stylesheet_prepare_rule_index(mutable_sheet);
        style_relative_selector_cache_begin(mutable_sheet);
        for (size_t i = 0; i < sheet->transition_rule_count; i++) {
            const StyleTransitionRule *rule = &sheet->transition_rules[i];
            if (rule->pseudo != (uint8_t) pseudo
                || !style_container_query_matches(
                       sheet, rule->container_query, node)
                || !style_adopted_rule_in_scope(
                       sheet, rule->order >> 8, rule->selector,
                       rule->selector_length, node, NULL)
                || !style_selector_matches_profiled(
                       sheet, node, rule->selector, rule->selector_length))
                continue;
            transition_consider(rule, winners);
        }
        style_relative_selector_cache_end(mutable_sheet);
    }
    /* The style attribute beats every rule of equal importance; among its
       own declarations the later one wins. */
    StyleTransitionRule inline_rules[8];
    size_t inline_count = 0;
    size_t attribute_length = 0;
    const char *attribute = pseudo != PSEUDO_NONE
            || (sheet->block_inline_style_attributes
                && !document_style_attribute_cssom_authorized(node))
        ? NULL : document_attribute(node, "style", &attribute_length);
    for (size_t at = 0; attribute != NULL && at < attribute_length
             && inline_count < sizeof(inline_rules) / sizeof(inline_rules[0]);) {
        at = skip_css_space_and_comments(attribute, attribute_length, at);
        if (at >= attribute_length) break;
        size_t end = find_declaration_end(attribute, attribute_length, at);
        size_t colon = at;
        while (colon < end && attribute[colon] != ':') colon++;
        if (colon < end) {
            const char *name = attribute + at;
            size_t name_length = colon - at;
            const char *value = attribute + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            int kind = transition_declaration_kind(name, name_length);
            if (kind >= 0 && value_length < UINT16_MAX) {
                StyleTransitionRule *rule = &inline_rules[inline_count];
                *rule = (StyleTransitionRule) {
                    .value = value,
                    .value_length = (uint16_t) value_length,
                    .kind = (uint8_t) kind,
                    .important = important,
                    .origin = 0,
                    .layer = UINT_MAX,
                    .specificity = UINT_MAX,
                    .order = (unsigned) inline_count
                };
                transition_consider(rule, winners);
                inline_count++;
            }
        }
        at = end + (end < attribute_length);
    }
    for (unsigned longhand = 0; longhand < 4u; longhand++) {
        const StyleTransitionRule *rule = winners[longhand];
        if (rule == NULL
            || transition_value_is_css_wide(rule->value, rule->value_length))
            continue;
        const char *value = rule->value;
        size_t value_length = rule->value_length;
        char resolved[STYLE_CUSTOM_RESOLVED_CAPACITY];
        if (transition_value_has_var(value, value_length)) {
            StyleResolveScratch *scratch = sheet->resolve_scratch;
            if (scratch == NULL) continue;
            StyleResolveScratch saved = *scratch;
            scratch->resolution_node = node;
            scratch->resolution_pseudo = pseudo;
            bool valid = style_resolve_value(
                sheet, value, value_length, resolved, sizeof(resolved), 0);
            *scratch = saved;
            if (!valid) continue;
            value = resolved;
            value_length = strlen(resolved);
        }
        StyleTransitionComputed parsed;
        transition_initial(&parsed);
        if (!transition_parse((StyleTransitionKind) rule->kind, value,
                              value_length, &parsed)) continue;
        switch (longhand) {
        case 0:
            memcpy(output->properties, parsed.properties,
                   sizeof(output->properties));
            output->property_count = parsed.property_count;
            break;
        case 1:
            memcpy(output->duration_ms, parsed.duration_ms,
                   sizeof(output->duration_ms));
            output->duration_count = parsed.duration_count;
            break;
        case 2:
            memcpy(output->delay_ms, parsed.delay_ms,
                   sizeof(output->delay_ms));
            output->delay_count = parsed.delay_count;
            break;
        default:
            memcpy(output->timings, parsed.timings, sizeof(output->timings));
            output->timing_count = parsed.timing_count;
            break;
        }
    }
    return true;
}
