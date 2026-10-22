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

/* A CSS <number> (or, when `integer`, an <integer>: no fraction or
   exponent), the whole span; a math function resolving to a number is
   accepted too. *number receives its value. */
static bool timing_number(const char *text, size_t length, bool integer,
                          double *number)
{
    if (length == 0) return false;
    if (text[length - 1] == ')') {
        /* Not capped at +/-10: steps(calc(12)) and cubic-bezier(0.3,
           calc(-12), 0.7, 1) are as valid as their literal forms. */
        int64_t thousandths = 0;
        if (!style_math_resolve_number_thousandths_wide(
                NULL, text, length, &thousandths))
            return false;
        /* An <integer> position rounds a math result to the nearest
           integer, as CSS Values requires. */
        *number = integer ? round((double) thousandths / 1000.0)
                          : (double) thousandths / 1000.0;
        return true;
    }
    size_t at = text[0] == '+' || text[0] == '-' ? 1u : 0u;
    size_t digits = 0, fraction = 0;
    while (at < length && isdigit((unsigned char) text[at])) at++, digits++;
    if (at < length && text[at] == '.') {
        if (integer) return false;
        at++;
        while (at < length && isdigit((unsigned char) text[at]))
            at++, fraction++;
        if (fraction == 0) return false;
    }
    if (digits + fraction == 0) return false;
    if (at < length && (text[at] == 'e' || text[at] == 'E')) {
        if (integer) return false;
        at++;
        if (at < length && (text[at] == '+' || text[at] == '-')) at++;
        size_t exponent = at;
        while (at < length && isdigit((unsigned char) text[at])) at++;
        if (at == exponent) return false;
    }
    char copy[40];
    if (at != length || length >= sizeof(copy)) return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    *number = strtod(copy, NULL);
    return isfinite(*number);
}

/* Appends `text` to the serialization, false when it does not fit. */
static bool timing_append(char *output, size_t output_size, size_t *used,
                          const char *text, size_t length)
{
    if (length >= output_size - *used) return false;
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

static bool timing_append_number(char *output, size_t output_size,
                                 size_t *used, double number,
                                 const char *suffix)
{
    char text[40];
    int written = snprintf(text, sizeof(text), "%g%s", number, suffix);
    return written > 0 && (size_t) written < sizeof(text)
        && timing_append(output, output_size, used, text, (size_t) written);
}

/* One linear() stop: a <number> and zero to two adjacent <percentage>s,
   in either order (`<number> && <percentage>{1,2}`). */
static bool timing_linear_stop(const char *text, size_t length,
                               char *output, size_t output_size,
                               size_t *used)
{
    size_t cursor = 0, at = 0, end = 0;
    unsigned tokens = 0, percentages = 0;
    bool number_seen = false, percentage_after_number = false;
    while (style_next_top_level_token(text, length, &cursor, &at, &end)) {
        if (++tokens > 3u) return false;
        double value = 0.0;
        bool percentage = end > at && text[end - 1] == '%';
        if (!timing_number(text + at, end - at - (percentage ? 1u : 0u),
                           false, &value)) return false;
        if (percentage) {
            /* Two percentages must be adjacent. */
            if (percentages == 1u && number_seen
                && !percentage_after_number) return false;
            percentage_after_number = number_seen;
            percentages++;
        } else {
            if (number_seen) return false;
            number_seen = true;
        }
        if ((tokens > 1u && !timing_append(output, output_size, used, " ", 1))
            || !timing_append_number(output, output_size, used, value,
                                     percentage ? "%" : "")) return false;
    }
    return number_seen;
}

bool style_timing_function(const char *text, size_t length,
                           char *output, size_t output_size)
{
    static const char *const keywords[] = {
        "ease", "linear", "ease-in", "ease-out", "ease-in-out",
        "step-start", "step-end"
    };
    static const char *const step_positions[] = {
        "jump-start", "jump-end", "jump-none", "jump-both", "start", "end"
    };
    if (text == NULL || output == NULL || output_size == 0) return false;
    trim(&text, &length);
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (span_case_equal(text, length, keywords[i])) {
            snprintf(output, output_size, "%s", keywords[i]);
            return true;
        }
    }
    size_t open = 0;
    while (open < length && text[open] != '(') open++;
    if (open == length || text[length - 1] != ')') return false;
    const char *arguments = text + open + 1;
    size_t arguments_length = length - open - 2u;
    /* The parenthesis that opens the arguments must be the one the final
       parenthesis closes (quoted parentheses do not count). */
    int depth = 0;
    char quote = 0;
    for (size_t i = open; i + 1u < length; i++) {
        char c = text[i];
        if (quote != 0) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '(') depth++;
        else if (c == ')' && --depth == 0) return false;
    }
    if (quote != 0 || depth != 1) return false;
    enum { CUBIC_BEZIER, STEPS, LINEAR } kind;
    if (span_case_equal(text, open, "cubic-bezier")) kind = CUBIC_BEZIER;
    else if (span_case_equal(text, open, "steps")) kind = STEPS;
    else if (span_case_equal(text, open, "linear")) kind = LINEAR;
    else return false;
    static const char *const names[] = {
        "cubic-bezier(", "steps(", "linear("
    };
    size_t used = 0;
    output[0] = '\0';
    if (!timing_append(output, output_size, &used, names[kind],
                       strlen(names[kind]))) return false;
    size_t count = 0, cursor = 0, at = 0, end = 0;
    double steps = 0.0;
    while (style_next_top_level(arguments, arguments_length, ',', &cursor,
                                &at, &end)) {
        const char *argument = arguments + at;
        size_t argument_length = end - at;
        if (argument_length == 0
            || (count != 0
                && !timing_append(output, output_size, &used, ", ", 2)))
            return false;
        if (kind == LINEAR) {
            if (!timing_linear_stop(argument, argument_length, output,
                                    output_size, &used)) return false;
        } else if (kind == STEPS && count == 1u) {
            size_t position = 0;
            while (position < sizeof(step_positions) / sizeof(step_positions[0])
                   && !span_case_equal(argument, argument_length,
                                       step_positions[position])) position++;
            if (position == sizeof(step_positions) / sizeof(step_positions[0])
                /* jump-none needs two or more steps. */
                || (position == 2u && steps < 2.0)
                || !timing_append(output, output_size, &used,
                                  step_positions[position],
                                  strlen(step_positions[position])))
                return false;
        } else {
            double number = 0.0;
            if ((kind == CUBIC_BEZIER && count >= 4u)
                || (kind == STEPS && count >= 2u)
                || !timing_number(argument, argument_length, kind == STEPS,
                                  &number)
                /* x1 and x2 are progress values in [0, 1]. */
                || (kind == CUBIC_BEZIER && (count & 1u) == 0
                    && (number < 0.0 || number > 1.0))
                || (kind == STEPS && number < 1.0)
                || !timing_append_number(output, output_size, &used,
                                         number, "")) return false;
            steps = number;
        }
        count++;
    }
    if ((kind == CUBIC_BEZIER && count != 4u)
        || (kind == STEPS && count == 0u)
        || (kind == LINEAR && count < 2u)) return false;
    return timing_append(output, output_size, &used, ")", 1);
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

/* The initial timing, `ease`, sits at offset 0 of every list's text. */
static const char transition_initial_timing[] = "ease";

static void transition_initial(StyleTransitionComputed *output)
{
    /* The timing text past what is used is never read: leave it. */
    memset(output, 0, offsetof(StyleTransitionComputed, timing_text));
    output->property_count = output->duration_count = 1;
    output->delay_count = output->timing_count = 1;
    snprintf(output->properties[0], sizeof(output->properties[0]), "all");
    memcpy(output->timing_text, transition_initial_timing,
           sizeof(transition_initial_timing));
    output->timing_text_used = (uint16_t) sizeof(transition_initial_timing);
}

/* Serializes the easing `text` as timing item `slot` of `output`, in the
   text its list shares: 1 when it is stored, 0 when it is not a valid
   easing and -1 when it is one but the list's text has no room for it. */
static int transition_set_timing(StyleTransitionComputed *output,
                                 size_t slot, const char *text,
                                 size_t length)
{
    char serialized[STYLE_TIMING_FUNCTION_CAPACITY];
    if (!style_timing_function(text, length, serialized,
                               sizeof(serialized))) return 0;
    size_t size = strlen(serialized) + 1u;
    if (size > sizeof(output->timing_text) - output->timing_text_used)
        return -1;
    memcpy(output->timing_text + output->timing_text_used, serialized, size);
    output->timing_offsets[slot] = output->timing_text_used;
    output->timing_text_used = (uint16_t) (output->timing_text_used + size);
    return 1;
}

/* Copies the timing list of `from` over that of `to`. */
static void transition_copy_timings(StyleTransitionComputed *to,
                                    const StyleTransitionComputed *from)
{
    memcpy(to->timing_offsets, from->timing_offsets,
           sizeof(to->timing_offsets));
    memcpy(to->timing_text, from->timing_text, from->timing_text_used);
    to->timing_text_used = from->timing_text_used;
    to->timing_count = from->timing_count;
}

/* Whether a token of a declaration the stylesheet already validated is its
   easing: an easing keyword or a function, since no other shorthand
   component holds a parenthesis. Only valid for validated text, where the
   token is then a valid easing. */
static bool transition_validated_timing_token(const char *text, size_t length)
{
    static const char *const keywords[] = {
        "ease", "linear", "ease-in", "ease-out", "ease-in-out",
        "step-start", "step-end"
    };
    if (memchr(text, '(', length) != NULL) return true;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (span_case_equal(text, length, keywords[i])) return true;
    }
    return false;
}

/* Parses one declaration's value into the lists it sets: all four for the
   shorthand, one for a longhand. False for an invalid value. Without
   `timings` the value must be one the stylesheet validated: its easings
   are recognized but not validated or serialized, and the timing list is
   left as it was. */
static bool transition_parse_lists(StyleTransitionKind kind, const char *text,
                                   size_t length, bool timings,
                                   StyleTransitionComputed *output)
{
    StyleTransitionComputed parsed;
    transition_initial(&parsed);
    size_t items = 0;
    bool none = false;
    size_t cursor = 0, item_start = 0, item_end = 0;
    while (style_next_top_level(text, length, ',', &cursor, &item_start,
                                &item_end)) {
        const char *item = text + item_start;
        size_t item_length = item_end - item_start;
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
            if (transition_set_timing(&parsed, slot, item, item_length) != 1)
                return false;
            continue;
        }
        /* Shorthand: [property] [duration] [timing] [delay] [behavior] in
           any order, the first time being the duration. */
        snprintf(parsed.properties[slot], sizeof(parsed.properties[slot]),
                 "all");
        parsed.timing_offsets[slot] = 0; /* the initial ease */
        parsed.duration_ms[slot] = parsed.delay_ms[slot] = 0.0;
        bool have_property = false, have_timing = false;
        bool have_behavior = false;
        unsigned times = 0;
        size_t token_cursor = 0, token_start = 0, token_end = 0;
        while (style_next_top_level_token(item, item_length, &token_cursor,
                                          &token_start, &token_end)) {
            const char *token = item + token_start;
            size_t token_length = token_end - token_start;
            double time = 0.0;
            int timing = 0;
            if (transition_time_ms(token, token_length, &time)) {
                if (times == 2u) return false;
                if (times++ == 0u) {
                    if (time < 0.0) return false;
                    parsed.duration_ms[slot] = time;
                } else {
                    parsed.delay_ms[slot] = time;
                }
            } else if (timings
                       ? (timing = transition_set_timing(
                              &parsed, slot, token, token_length)) != 0
                       : transition_validated_timing_token(
                             token, token_length)) {
                if (timing < 0 || have_timing) return false;
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
    if (timings && (kind == STYLE_TRANSITION_SHORTHAND
                    || kind == STYLE_TRANSITION_TIMING)) {
        parsed.timing_count = (uint8_t) items;
        transition_copy_timings(output, &parsed);
    }
    return true;
}

static bool transition_parse(StyleTransitionKind kind, const char *text,
                             size_t length, StyleTransitionComputed *output)
{
    return transition_parse_lists(kind, text, length, true, output);
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
    if (selector_length >= STYLE_SELECTOR_TEXT_CAPACITY
        || value_length >= STYLE_TRANSITION_VALUE_CAPACITY) return true;
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
    /* A long value (a linear() easing of many stops) draws on the sheet's
       long-value allowance; past it the declaration is dropped. */
    bool long_value = value_length >= STYLE_TRANSITION_SHORT_VALUE_CAPACITY;
    if (long_value && value_length + 1u > STYLE_CUSTOM_LONG_VALUE_BYTES
                                          - sheet->custom_value_long_bytes)
        return true;
    const char *stored_selector = stylesheet_store_selector(
        sheet, selector, selector_length);
    const char *stored_value = stylesheet_store_text(
        sheet, value, value_length);
    if (stored_selector == NULL || stored_value == NULL) return false;
    if (long_value) sheet->custom_value_long_bytes += value_length + 1u;
    uint8_t fast_key_length = 0;
    uint8_t fast_key_suffix = selector_fast_key_suffix(
        stored_selector, selector_length, &fast_key_length);
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
            .fast_key_suffix_bytes = fast_key_suffix,
            .fast_key_length = fast_key_length,
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

static bool transition_computed(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    bool timings, StyleTransitionComputed *output)
{
    if (output == NULL) return false;
    transition_initial(output);
    if (sheet == NULL || node == NULL) return true;
    const StyleTransitionRule *winners[4] = {NULL, NULL, NULL, NULL};
    if (sheet->transition_rule_count != 0) {
        Stylesheet *mutable_sheet = (Stylesheet *) sheet;
        stylesheet_prepare_rule_index(mutable_sheet);
        style_relative_selector_cache_begin(mutable_sheet);
        StyleMatchSubject subject = {0};
        style_match_subject_prepare(node, &subject);
        for (size_t i = 0; i < sheet->transition_rule_count; i++) {
            const StyleTransitionRule *rule = &sheet->transition_rules[i];
            if (rule->pseudo != (uint8_t) pseudo
                || !selector_fast_key_matches(
                       sheet, rule->selector, rule->selector_length,
                       rule->fast_key_suffix_bytes, rule->fast_key_length,
                       &subject)
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
    /* One shorthand commonly wins all four longhands: parse each distinct
       winner once and take every longhand it won from that parse. */
    unsigned longhands = timings ? 4u : 3u;
    for (unsigned longhand = 0; longhand < longhands; longhand++) {
        const StyleTransitionRule *rule = winners[longhand];
        bool parsed_already = false;
        for (unsigned earlier = 0; earlier < longhand; earlier++)
            parsed_already |= winners[earlier] == rule;
        if (rule == NULL || parsed_already
            || transition_value_is_css_wide(rule->value, rule->value_length))
            continue;
        const char *value = rule->value;
        size_t value_length = rule->value_length;
        char resolved[STYLE_CUSTOM_RESOLVED_CAPACITY];
        bool has_var = transition_value_has_var(value, value_length);
        /* Sheet rules without var() were validated when they were added
           (add_transition_rule); style-attribute and var() values were
           not, so they still validate their easings. */
        bool validated = rule->selector != NULL && !has_var;
        if (has_var) {
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
        if (!transition_parse_lists((StyleTransitionKind) rule->kind, value,
                                    value_length, timings || !validated,
                                    &parsed)) continue;
        for (unsigned target = longhand; target < longhands; target++) {
            if (winners[target] != rule) continue;
            switch (target) {
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
                transition_copy_timings(output, &parsed);
                break;
            }
        }
    }
    return true;
}

bool style_transition_computed(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    StyleTransitionComputed *output)
{
    return transition_computed(sheet, node, pseudo, true, output);
}

bool style_transition_computed_untimed(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    StyleTransitionComputed *output)
{
    return transition_computed(sheet, node, pseudo, false, output);
}
