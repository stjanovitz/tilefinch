/* Bounded CSS size-container queries.
 *
 * Definitions are retained once per stylesheet. Geometry lives only for the
 * duration of a layout build in an open-addressed table, keeping the common
 * per-node ComputedStyle unchanged on the PSP.
 */

#include "style_internal.h"

#include <stdlib.h>
#include <strings.h>

typedef struct {
    const char *text;
    size_t length;
    const StyleContainerState *state;
    bool valid;
} ContainerConditionParser;

static void query_trim(const char **text, size_t *length)
{
    while (*length != 0 && isspace((unsigned char) (*text)[0])) {
        (*text)++;
        (*length)--;
    }
    while (*length != 0
           && isspace((unsigned char) (*text)[*length - 1])) {
        (*length)--;
    }
}

static bool query_span_case_equal(const char *text, size_t length,
                                  const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (length != wanted_length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static uint32_t query_name_bit(Stylesheet *sheet, const char *name,
                               size_t length)
{
    if (sheet == NULL || name == NULL || length == 0
        || length >= STYLE_CONTAINER_NAME_CAPACITY) return 0;
    if (sheet->conditional_queries == NULL) {
        sheet->conditional_queries = budget_calloc(
            sheet->budget, 1, sizeof(*sheet->conditional_queries));
        if (sheet->conditional_queries == NULL) return 0;
    }
    StyleConditionalQueries *queries = sheet->conditional_queries;
    for (size_t i = 0; i < queries->name_count; i++) {
        if (strlen(queries->names[i]) == length
            && memcmp(queries->names[i], name, length) == 0) {
            return UINT32_C(1) << i;
        }
    }
    if (queries->name_count >= STYLE_CONTAINER_NAME_LIMIT) return 0;
    size_t slot = queries->name_count++;
    memcpy(queries->names[slot], name, length);
    queries->names[slot][length] = '\0';
    return UINT32_C(1) << slot;
}

static bool query_identifier_character(char value)
{
    unsigned char c = (unsigned char) value;
    return isalnum(c) || value == '-' || value == '_';
}

bool style_register_container_query(Stylesheet *sheet,
                                    const char *prelude, size_t length,
                                    uint8_t parent_query,
                                    uint8_t *query_id)
{
    if (query_id == NULL) return false;
    *query_id = 0;
    if (sheet == NULL || sheet->budget == NULL || prelude == NULL) {
        return true;
    }
    query_trim(&prelude, &length);
    if (length == 0 || length >= STYLE_CONTAINER_QUERY_TEXT_CAPACITY) {
        return true;
    }
    if (sheet->conditional_queries == NULL) {
        sheet->conditional_queries = budget_calloc(
            sheet->budget, 1, sizeof(*sheet->conditional_queries));
        if (sheet->conditional_queries == NULL) return false;
    }
    StyleConditionalQueries *queries = sheet->conditional_queries;
    if (queries->query_count >= STYLE_CONTAINER_QUERY_LIMIT) return true;

    size_t condition_at = 0;
    while (condition_at < length && prelude[condition_at] != '(') {
        condition_at++;
    }
    const char *name = prelude;
    size_t name_length = condition_at;
    query_trim(&name, &name_length);
    if (condition_at == length) {
        /* A name-only query selects a named ancestor without requiring size
           containment. */
        if (name_length == 0) return true;
        for (size_t i = 0; i < name_length; i++) {
            if (!query_identifier_character(name[i])) return true;
        }
    } else if (name_length != 0) {
        for (size_t i = 0; i < name_length; i++) {
            if (!query_identifier_character(name[i])) return true;
        }
    }

    StyleContainerQuery query = {0};
    query.parent = parent_query;
    if (name_length != 0) {
        query.name_bit = query_name_bit(sheet, name, name_length);
        if (query.name_bit == 0) return true;
    }
    if (condition_at < length) {
        const char *condition = prelude + condition_at;
        size_t condition_length = length - condition_at;
        query_trim(&condition, &condition_length);
        if (condition_length == 0
            || condition_length >= sizeof(query.condition)) return true;
        memcpy(query.condition, condition, condition_length);
        query.condition[condition_length] = '\0';
        query.condition_length = (uint8_t) condition_length;
        for (size_t i = 0; i < condition_length; i++) {
            if (i + 5 <= condition_length
                && strncasecmp(condition + i, "width", 5) == 0) {
                query.needs_inline_size = 1;
            }
            if (i + 11 <= condition_length
                && strncasecmp(condition + i, "inline-size", 11) == 0) {
                query.needs_inline_size = 1;
            }
            if (i + 6 <= condition_length
                && strncasecmp(condition + i, "height", 6) == 0) {
                query.needs_block_size = 1;
            }
            if (i + 10 <= condition_length
                && strncasecmp(condition + i, "block-size", 10) == 0) {
                query.needs_block_size = 1;
            }
        }
        if (!query.needs_inline_size && !query.needs_block_size) return true;
    }
    queries->queries[queries->query_count++] = query;
    *query_id = queries->query_count;
    return true;
}

static size_t container_pointer_hash(const void *pointer)
{
    uintptr_t value = (uintptr_t) pointer;
    value ^= value >> 16;
    value *= (uintptr_t) UINT32_C(0x7feb352d);
    value ^= value >> 15;
    return (size_t) value;
}

static StyleContainerState *container_state_find(
    const Stylesheet *sheet, lxb_dom_node_t *node)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL) {
        return NULL;
    }
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->container_states == NULL
        || scratch->container_state_capacity == 0) return NULL;
    size_t mask = scratch->container_state_capacity - 1u;
    size_t home = container_pointer_hash(node) & mask;
    for (size_t probe = 0; probe < 8; probe++) {
        StyleContainerState *state =
            &scratch->container_states[(home + probe) & mask];
        if (!state->occupied) return NULL;
        if (state->node == node) return state;
    }
    return NULL;
}

static bool parse_container_type_value(const char *value, size_t length,
                                       uint8_t *type)
{
    query_trim(&value, &length);
    if (query_span_case_equal(value, length, "size")) {
        *type = STYLE_CONTAINER_TYPE_SIZE;
        return true;
    }
    if (query_span_case_equal(value, length, "inline-size")) {
        *type = STYLE_CONTAINER_TYPE_INLINE_SIZE;
        return true;
    }
    if (query_span_case_equal(value, length, "normal")) {
        *type = STYLE_CONTAINER_TYPE_NONE;
        return true;
    }
    return false;
}

static uint32_t container_name_bits(Stylesheet *sheet, const char *value,
                                    size_t length)
{
    if (sheet == NULL || sheet->conditional_queries == NULL) return 0;
    uint32_t bits = 0;
    for (size_t at = 0; at < length;) {
        while (at < length && isspace((unsigned char) value[at])) at++;
        size_t end = at;
        while (end < length && !isspace((unsigned char) value[end])) end++;
        if (end == at) break;
        if (!query_span_case_equal(value + at, end - at, "none")) {
            for (size_t i = 0;
                 i < sheet->conditional_queries->name_count; i++) {
                const char *known = sheet->conditional_queries->names[i];
                if (strlen(known) == end - at
                    && memcmp(known, value + at, end - at) == 0) {
                    bits |= UINT32_C(1) << i;
                }
            }
        }
        at = end;
    }
    return bits;
}

bool stylesheet_has_container_queries(const Stylesheet *sheet)
{
    if (sheet == NULL) return false;
    return sheet->has_container_relative_units
        || (sheet->conditional_queries != NULL
            && sheet->conditional_queries->query_count != 0);
}

bool style_container_layout_state_begin(Stylesheet *sheet, Budget *budget,
                                        size_t expected_containers)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || budget == NULL) {
        return false;
    }
    style_container_layout_state_clear(sheet);
    size_t capacity = 16;
    size_t wanted = expected_containers > 512 ? 512
        : expected_containers;
    while (capacity < wanted * 2 && capacity < 1024) capacity *= 2;
    StyleContainerState *states = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, capacity, sizeof(*states));
    if (states == NULL) return false;
    StyleContainerMatchCacheEntry *matches = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 64, sizeof(*matches));
    if (matches == NULL) {
        budget_free(budget, states);
        return false;
    }
    sheet->resolve_scratch->container_states = states;
    sheet->resolve_scratch->container_state_capacity = capacity;
    sheet->resolve_scratch->container_match_cache = matches;
    sheet->resolve_scratch->container_match_cache_capacity = 64;
    return true;
}

bool style_container_layout_state_add(Stylesheet *sheet,
                                      lxb_dom_node_t *node,
                                      int content_width,
                                      int content_height,
                                      int padding_horizontal,
                                      int padding_vertical)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL) {
        return false;
    }
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->container_states == NULL
        || scratch->container_state_capacity == 0) return false;

    char type_value[96] = {0};
    char name_value[96] = {0};
    char shorthand[96] = {0};
    uint8_t type = STYLE_CONTAINER_TYPE_NONE;
    uint32_t names = 0;

    /* Container establishment itself is resolved without the table being
       assembled. This avoids order-dependent partial state while node boxes
       are inserted. Size-querying container-type is circular by definition. */
    StyleContainerState *saved_states = scratch->container_states;
    size_t saved_capacity = scratch->container_state_capacity;
    scratch->container_states = NULL;
    scratch->container_state_capacity = 0;
    bool has_type = style_retained_presentation_value(
        sheet, node, "container-type", 14,
        type_value, sizeof(type_value));
    bool has_name = style_retained_presentation_value(
        sheet, node, "container-name", 14,
        name_value, sizeof(name_value));
    bool has_shorthand = style_retained_presentation_value(
        sheet, node, "container", 9, shorthand, sizeof(shorthand));
    scratch->container_states = saved_states;
    scratch->container_state_capacity = saved_capacity;

    if (has_shorthand) {
        const char *slash = strchr(shorthand, '/');
        if (slash != NULL) {
            const char *name = shorthand;
            size_t name_length = (size_t) (slash - shorthand);
            const char *type_text = slash + 1;
            size_t type_length = strlen(type_text);
            query_trim(&name, &name_length);
            query_trim(&type_text, &type_length);
            (void) parse_container_type_value(type_text, type_length, &type);
            names = container_name_bits(sheet, name, name_length);
        }
    }
    if (has_type) {
        (void) parse_container_type_value(
            type_value, strlen(type_value), &type);
    }
    if (has_name) {
        names = container_name_bits(sheet, name_value, strlen(name_value));
    }
    if (type == STYLE_CONTAINER_TYPE_NONE && names == 0) return true;

    /* LayoutNodeBox client dimensions describe the padding box. Probe layout
       retains the already-resolved padding sums in a compact sidecar, so an
       actual container does not trigger a second ancestor/cascade walk. */
    content_width -= padding_horizontal;
    content_height -= padding_vertical;

    size_t mask = scratch->container_state_capacity - 1u;
    size_t home = container_pointer_hash(node) & mask;
    for (size_t probe = 0; probe < 8; probe++) {
        StyleContainerState *state =
            &scratch->container_states[(home + probe) & mask];
        if (!state->occupied || state->node == node) {
            if (!state->occupied) scratch->container_state_count++;
            *state = (StyleContainerState) {
                .node = node,
                .name_bits = names,
                .content_width = content_width < 0 ? 0 : content_width,
                .content_height = content_height < 0 ? 0 : content_height,
                .type = type,
                .occupied = true
            };
            if (STYLE_TRACE(sheet, LAYOUT)) {
                fprintf(stderr,
                        "style-container-state node=%p type=%u names=%08x "
                        "content=%dx%d\n",
                        (void *) node, (unsigned) type, (unsigned) names,
                        state->content_width, state->content_height);
            }
            return true;
        }
    }
    /* A pathological collision cluster degrades by omitting this container,
       never by growing an unbounded chain. */
    return true;
}

void style_container_layout_state_finish(Stylesheet *sheet)
{
    (void) sheet;
}

void style_container_layout_state_clear(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->container_states != NULL && sheet->budget != NULL) {
        budget_free(sheet->budget, scratch->container_states);
    }
    if (scratch->container_match_cache != NULL && sheet->budget != NULL) {
        budget_free(sheet->budget, scratch->container_match_cache);
    }
    scratch->container_states = NULL;
    scratch->container_match_cache = NULL;
    scratch->container_state_capacity = 0;
    scratch->container_state_count = 0;
    scratch->container_match_cache_capacity = 0;
    scratch->container_inline_basis = 0;
    scratch->container_block_basis = 0;
    scratch->container_basis_active = false;
}

uint64_t style_container_layout_state_signature(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return 0;
    const StyleResolveScratch *scratch = sheet->resolve_scratch;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < scratch->container_state_capacity; i++) {
        const StyleContainerState *state = &scratch->container_states[i];
        if (!state->occupied) continue;
        uint64_t values[] = {
            (uint64_t) (uintptr_t) state->node,
            state->name_bits,
            (uint32_t) state->content_width,
            (uint32_t) state->content_height,
            state->type
        };
        for (size_t j = 0; j < sizeof(values) / sizeof(values[0]); j++) {
            hash ^= values[j];
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= scratch->container_state_count;
    return scratch->container_state_count == 0 ? 0 : hash;
}

static bool query_number_px(const char *text, size_t length, double *value)
{
    query_trim(&text, &length);
    if (length == 0 || length >= 48) return false;
    char copy[48];
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    double parsed = strtod(copy, &end);
    if (end == copy) return false;
    double scale = 1.0;
    if (*end == '\0' || strcmp(end, "px") == 0) {
        scale = 1.0;
    } else if (strcmp(end, "em") == 0 || strcmp(end, "rem") == 0) {
        scale = STYLE_DEFAULT_FONT_PX;
    } else {
        return false;
    }
    *value = parsed * scale;
    return true;
}

static bool query_compare(double actual, const char *op, size_t op_length,
                          double expected)
{
    if (op_length == 1 && op[0] == '<') return actual < expected;
    if (op_length == 2 && op[0] == '<' && op[1] == '=') {
        return actual <= expected;
    }
    if (op_length == 1 && op[0] == '>') return actual > expected;
    if (op_length == 2 && op[0] == '>' && op[1] == '=') {
        return actual >= expected;
    }
    if ((op_length == 1 && (op[0] == '=' || op[0] == ':'))
        || (op_length == 2 && op[0] == '=' && op[1] == '=')) {
        return actual == expected;
    }
    return false;
}

static bool query_feature_value(const StyleContainerState *state,
                                const char *text, size_t length,
                                bool *valid)
{
    query_trim(&text, &length);
    char compact[STYLE_CONTAINER_QUERY_TEXT_CAPACITY];
    size_t used = 0;
    for (size_t i = 0; i < length && used + 1 < sizeof(compact); i++) {
        if (!isspace((unsigned char) text[i])) {
            compact[used++] = (char) tolower((unsigned char) text[i]);
        }
    }
    compact[used] = '\0';
    bool minimum = strncmp(compact, "min-width:", 10) == 0
        || strncmp(compact, "min-height:", 11) == 0
        || strncmp(compact, "min-inline-size:", 16) == 0
        || strncmp(compact, "min-block-size:", 15) == 0;
    bool maximum = strncmp(compact, "max-width:", 10) == 0
        || strncmp(compact, "max-height:", 11) == 0
        || strncmp(compact, "max-inline-size:", 16) == 0
        || strncmp(compact, "max-block-size:", 15) == 0;
    const char *feature = strstr(compact, "inline-size");
    size_t feature_length = 11;
    bool block = false;
    if (feature == NULL) {
        feature = strstr(compact, "block-size");
        feature_length = 10;
        block = feature != NULL;
    }
    if (feature == NULL) {
        feature = strstr(compact, "width");
        feature_length = 5;
    }
    if (feature == NULL) {
        feature = strstr(compact, "height");
        feature_length = 6;
        block = feature != NULL;
    }
    if (feature == NULL) {
        *valid = false;
        return false;
    }
    double actual = block ? state->content_height : state->content_width;

    if (minimum || maximum) {
        const char *colon = strchr(compact, ':');
        double expected = 0;
        if (colon == NULL
            || !query_number_px(colon + 1, strlen(colon + 1), &expected)) {
            *valid = false;
            return false;
        }
        return minimum ? actual >= expected : actual <= expected;
    }

    if (feature == compact && used == feature_length) {
        return actual != 0;
    }

    size_t feature_at = (size_t) (feature - compact);
    if (feature_at == 0) {
        size_t op_at = feature_length;
        size_t op_length = compact[op_at] == '<' || compact[op_at] == '>'
            ? (compact[op_at + 1] == '=' ? 2u : 1u) : 1u;
        double expected = 0;
        if (op_at >= used
            || !query_number_px(
                compact + op_at + op_length,
                used - op_at - op_length, &expected)) {
            *valid = false;
            return false;
        }
        return query_compare(
            actual, compact + op_at, op_length, expected);
    }

    /* Range syntax: 75px <= width <= 150px. */
    size_t left_op_end = feature_at;
    size_t left_op_start = left_op_end;
    while (left_op_start != 0
           && (compact[left_op_start - 1] == '<'
               || compact[left_op_start - 1] == '>'
               || compact[left_op_start - 1] == '=')) left_op_start--;
    double left = 0;
    if (left_op_start == left_op_end
        || !query_number_px(compact, left_op_start, &left)) {
        *valid = false;
        return false;
    }
    bool left_match = query_compare(
        left, compact + left_op_start, left_op_end - left_op_start, actual);
    size_t right_op = feature_at + feature_length;
    if (right_op >= used) return left_match;
    size_t right_op_length = compact[right_op + 1] == '=' ? 2u : 1u;
    double right = 0;
    if (!query_number_px(
            compact + right_op + right_op_length,
            used - right_op - right_op_length, &right)) {
        *valid = false;
        return false;
    }
    return left_match && query_compare(
        actual, compact + right_op, right_op_length, right);
}

static bool query_outer_wrapped(const char *text, size_t length)
{
    if (length < 2 || text[0] != '(' || text[length - 1] != ')') {
        return false;
    }
    unsigned depth = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '(') depth++;
        else if (text[i] == ')') {
            if (depth == 0) return false;
            depth--;
            if (depth == 0 && i + 1 != length) return false;
        }
    }
    return depth == 0;
}

static bool query_word_at(const char *text, size_t length, size_t at,
                          const char *word)
{
    size_t word_length = strlen(word);
    if (at + word_length > length
        || strncasecmp(text + at, word, word_length) != 0) return false;
    bool left = at == 0
        || !query_identifier_character(text[at - 1]);
    bool right = at + word_length == length
        || !query_identifier_character(text[at + word_length]);
    return left && right;
}

static bool query_eval_expression(const StyleContainerState *state,
                                  const char *text, size_t length,
                                  unsigned depth, bool *valid)
{
    if (depth > 8) {
        *valid = false;
        return false;
    }
    query_trim(&text, &length);
    while (query_outer_wrapped(text, length)) {
        text++;
        length -= 2;
        query_trim(&text, &length);
    }
    if (length == 0) {
        *valid = false;
        return false;
    }

    unsigned nesting = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '(') nesting++;
        else if (text[i] == ')' && nesting != 0) nesting--;
        else if (nesting == 0
                 && (text[i] == ',' || query_word_at(text, length, i, "or"))) {
            size_t skip = text[i] == ',' ? 1u : 2u;
            bool left_valid = true, right_valid = true;
            bool left = query_eval_expression(
                state, text, i, depth + 1, &left_valid);
            bool right = query_eval_expression(
                state, text + i + skip, length - i - skip,
                depth + 1, &right_valid);
            *valid = left_valid && right_valid;
            return *valid && (left || right);
        }
    }
    nesting = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '(') nesting++;
        else if (text[i] == ')' && nesting != 0) nesting--;
        else if (nesting == 0 && query_word_at(text, length, i, "and")) {
            bool left_valid = true, right_valid = true;
            bool left = query_eval_expression(
                state, text, i, depth + 1, &left_valid);
            bool right = query_eval_expression(
                state, text + i + 3, length - i - 3,
                depth + 1, &right_valid);
            *valid = left_valid && right_valid;
            return *valid && left && right;
        }
    }
    if (query_word_at(text, length, 0, "not")) {
        bool child_valid = true;
        bool child = query_eval_expression(
            state, text + 3, length - 3, depth + 1, &child_valid);
        *valid = child_valid;
        return child_valid && !child;
    }
    return query_feature_value(state, text, length, valid);
}

static bool query_matches_one(const Stylesheet *sheet,
                              const StyleContainerQuery *query,
                              lxb_dom_node_t *node)
{
    unsigned walked = 0;
    for (lxb_dom_node_t *ancestor = node->parent;
         ancestor != NULL && walked++ < STYLE_CONTAINER_WALK_LIMIT;
         ancestor = ancestor->parent) {
        if (ancestor->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        StyleContainerState *state = container_state_find(sheet, ancestor);
        if (state == NULL) continue;
        if (query->name_bit != 0
            && (state->name_bits & query->name_bit) == 0) continue;
        if (query->condition_length == 0) return true;
        if (query->needs_block_size
            && state->type != STYLE_CONTAINER_TYPE_SIZE) continue;
        if (query->needs_inline_size
            && state->type != STYLE_CONTAINER_TYPE_INLINE_SIZE
            && state->type != STYLE_CONTAINER_TYPE_SIZE) continue;
        bool valid = true;
        bool matches = query_eval_expression(
            state, query->condition, query->condition_length, 0, &valid);
        /* Container selection stops at the first eligible ancestor even
           when its query evaluates false. */
        return valid && matches;
    }
    return false;
}

bool style_container_query_matches(const Stylesheet *sheet, uint8_t query_id,
                                   lxb_dom_node_t *node)
{
    if (query_id == 0) return true;
    if (sheet == NULL || sheet->conditional_queries == NULL
        || node == NULL || query_id > sheet->conditional_queries->query_count
        || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->container_states == NULL) return false;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    size_t cache_slot = 0;
    if (scratch->container_match_cache != NULL
        && scratch->container_match_cache_capacity != 0) {
        cache_slot = (container_pointer_hash(node)
                      ^ ((size_t) query_id * 33u))
                     & (scratch->container_match_cache_capacity - 1u);
        StyleContainerMatchCacheEntry *cached =
            &scratch->container_match_cache[cache_slot];
        if (cached->occupied && cached->node == node
            && cached->query == query_id) return cached->matched;
    }
    const StyleContainerQuery *query =
        &sheet->conditional_queries->queries[query_id - 1u];
    bool matched = query->parent == 0
        || style_container_query_matches(sheet, query->parent, node);
    if (matched) matched = query_matches_one(sheet, query, node);
    if (scratch->container_match_cache != NULL
        && scratch->container_match_cache_capacity != 0) {
        scratch->container_match_cache[cache_slot] =
            (StyleContainerMatchCacheEntry) {
                .node = node,
                .query = query_id,
                .matched = matched,
                .occupied = true
            };
    }
    return matched;
}

void style_container_units_for_node(Stylesheet *sheet,
                                    lxb_dom_node_t *node)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    scratch->container_basis_active = false;
    scratch->container_inline_basis = sheet->viewport_width;
    scratch->container_block_basis = sheet->viewport_height;
    unsigned walked = 0;
    for (lxb_dom_node_t *ancestor = node == NULL ? NULL : node->parent;
         ancestor != NULL && walked++ < STYLE_CONTAINER_WALK_LIMIT;
         ancestor = ancestor->parent) {
        if (ancestor->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        StyleContainerState *state = container_state_find(sheet, ancestor);
        if (state == NULL || state->type == STYLE_CONTAINER_TYPE_NONE) {
            continue;
        }
        scratch->container_inline_basis = state->content_width;
        if (state->type == STYLE_CONTAINER_TYPE_SIZE) {
            scratch->container_block_basis = state->content_height;
        }
        scratch->container_basis_active = true;
        if (STYLE_TRACE(sheet, LAYOUT)) {
            fprintf(stderr,
                    "style-container-units node=%p container=%p "
                    "basis=%dx%d type=%u\n",
                    (void *) node, (void *) ancestor,
                    scratch->container_inline_basis,
                    scratch->container_block_basis,
                    (unsigned) state->type);
        }
        return;
    }
}
