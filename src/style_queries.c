/* Bounded CSS size-container queries.
 *
 * Definitions are retained once per stylesheet. Geometry lives only for the
 * duration of a layout build in an open-addressed table, keeping the common
 * per-node ComputedStyle unchanged on the PSP.
 */

#include "style_internal.h"

#include <stdlib.h>

/* Geometry table for a first pass with nothing measured yet (the measured
   collection sizes its own from the boxes). */
#define STYLE_CONTAINER_LOG_STATE_GUESS 512u
/* Evaluations one pass may log before it is declared unverifiable. */
#define STYLE_CONTAINER_LOG_INITIAL 256u
#define STYLE_CONTAINER_LOG_LIMIT 16384u

static uint32_t query_name_bit(Stylesheet *sheet, const char *name,
                               size_t length)
{
    /* The registering caller creates this table before parsing a name. */
    if (sheet == NULL || sheet->conditional_queries == NULL
        || name == NULL || length == 0
        || length >= STYLE_CONTAINER_NAME_CAPACITY) return 0;
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

/* The axes a condition's size features need a container to answer on (CSS
   Containment 3 size features): width and inline-size need the inline axis,
   height and block-size the block axis, and aspect-ratio and orientation,
   which relate the two, need both. min-/max- prefixes and range forms query
   the same features. False when the condition names a style() or
   scroll-state() query: this engine evaluates neither, so such a rule is
   dropped on purpose rather than retained to evaluate as unknown. */
static bool container_condition_axes(const char *condition, size_t length,
                                     uint8_t *needs_inline_size,
                                     uint8_t *needs_block_size)
{
    for (size_t at = 0; at < length;) {
        if (!query_identifier_character(condition[at])
            || isdigit((unsigned char) condition[at])) {
            at++;
            continue;
        }
        /* A whole word that does not start with a digit: `10px` is one
           dimension, never the identifier `px`. */
        size_t end = at;
        while (end < length && query_identifier_character(condition[end])) {
            end++;
        }
        const char *word = condition + at;
        size_t word_length = end - at;
        at = end;
        if (end < length && condition[end] == '(') {
            if (span_case_equal(word, word_length, "style")
                || span_case_equal(word, word_length, "scroll-state")) {
                return false;
            }
            continue;
        }
        if (span_case_equal(word, word_length, "orientation")) {
            *needs_inline_size = *needs_block_size = 1;
            continue;
        }
        if (word_length > 4 && (span_case_equal(word, 4, "min-")
                                || span_case_equal(word, 4, "max-"))) {
            word += 4;
            word_length -= 4;
        }
        if (span_case_equal(word, word_length, "width")
            || span_case_equal(word, word_length, "inline-size")) {
            *needs_inline_size = 1;
        } else if (span_case_equal(word, word_length, "height")
                   || span_case_equal(word, word_length, "block-size")) {
            *needs_block_size = 1;
        } else if (span_case_equal(word, word_length, "aspect-ratio")) {
            *needs_inline_size = *needs_block_size = 1;
        }
    }
    return true;
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
    trim(&prelude, &length);
    if (length == 0 || length >= STYLE_CONTAINER_QUERY_TEXT_CAPACITY) {
        return true;
    }
    if (sheet->conditional_queries == NULL) {
        sheet->conditional_queries = budget_calloc(
            sheet->budget, 1, sizeof(*sheet->conditional_queries));
        if (sheet->conditional_queries == NULL) return false;
    }
    StyleConditionalQueries *queries = sheet->conditional_queries;

    /* The prelude is [ <container-name> ]? <container-query>. A name is a
       <custom-ident> other than none, and, not and or (CSS Containment 3),
       so a leading `not` opens the condition instead of naming a
       container. An identifier directly followed by '(' is a function
       (style(), scroll-state()), which this engine does not query. */
    const char *name = prelude;
    size_t name_length = 0;
    while (name_length < length
           && query_identifier_character(prelude[name_length])) {
        name_length++;
    }
    if (name_length < length && prelude[name_length] == '(') {
        if (name_length != 0) return true;
    } else if (name_length != 0 && name_length < length
               && !isspace((unsigned char) prelude[name_length])) {
        return true;
    }
    if (span_case_equal(name, name_length, "not")) name_length = 0;
    else if (span_case_equal(name, name_length, "and")
             || span_case_equal(name, name_length, "or")
             || span_case_equal(name, name_length, "none")) return true;
    size_t condition_at = name_length;
    while (condition_at < length
           && isspace((unsigned char) prelude[condition_at])) {
        condition_at++;
    }
    if (condition_at == length) {
        /* A name-only query selects a named ancestor without requiring size
           containment. */
        if (name_length == 0) return true;
    } else if (prelude[condition_at] != '('
               && !(name_length == 0 && condition_at == 0)) {
        /* After a name, the condition starts with '(' or `not`. */
        size_t keyword = 0;
        while (condition_at + keyword < length
               && query_identifier_character(prelude[condition_at + keyword])) {
            keyword++;
        }
        if (!span_case_equal(prelude + condition_at, keyword, "not")) {
            return true;
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
        trim(&condition, &condition_length);
        if (condition_length == 0
            || condition_length >= sizeof(query.condition)) return true;
        memcpy(query.condition, condition, condition_length);
        query.condition[condition_length] = '\0';
        query.condition_length = (uint8_t) condition_length;
        /* A condition without any size feature (a media-only feature, say)
           can never match a container, so it is not retained. */
        if (!container_condition_axes(condition, condition_length,
                                      &query.needs_inline_size,
                                      &query.needs_block_size)
            || (!query.needs_inline_size && !query.needs_block_size)) {
            return true;
        }
    }
    /* Utility CSS repeats one prelude per class: identical queries share an
       id, so the 63 ids are spent on distinct conditions. */
    for (size_t i = 0; i < queries->query_count; i++) {
        const StyleContainerQuery *known = &queries->queries[i];
        if (known->name_bit == query.name_bit
            && known->parent == query.parent
            && known->condition_length == query.condition_length
            && memcmp(known->condition, query.condition,
                      query.condition_length) == 0) {
            *query_id = (uint8_t) (i + 1u);
            return true;
        }
    }
    if (queries->query_count >= STYLE_CONTAINER_QUERY_LIMIT) return true;
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
    trim(&value, &length);
    if (span_case_equal(value, length, "size")) {
        *type = STYLE_CONTAINER_TYPE_SIZE;
        return true;
    }
    if (span_case_equal(value, length, "inline-size")) {
        *type = STYLE_CONTAINER_TYPE_INLINE_SIZE;
        return true;
    }
    if (span_case_equal(value, length, "normal")) {
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
        if (!span_case_equal(value + at, end - at, "none")) {
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

static void container_state_store(Stylesheet *sheet, lxb_dom_node_t *node,
                                  uint32_t names, uint8_t type,
                                  int content_width, int content_height)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    size_t mask = scratch->container_state_capacity - 1u;
    size_t home = container_pointer_hash(node) & mask;
    content_width = content_width < 0 ? 0 : content_width;
    content_height = content_height < 0 ? 0 : content_height;
    for (size_t probe = 0; probe < 8; probe++) {
        StyleContainerState *state =
            &scratch->container_states[(home + probe) & mask];
        if (!state->occupied || state->node == node) {
            if (state->occupied && state->name_bits == names
                && state->type == type
                && state->content_width == content_width
                && state->content_height == content_height) return;
            if (!state->occupied) scratch->container_state_count++;
            *state = (StyleContainerState) {
                .node = node,
                .name_bits = names,
                .content_width = content_width,
                .content_height = content_height,
                .type = type,
                .occupied = true
            };
            sheet->container_state_generation++;
            /* A cached result may select this container through any of its
               ancestors or nested queries. The small bounded table is cheap
               to clear; keeping a result across this change is not valid. */
            if (scratch->container_match_cache != NULL)
                memset(scratch->container_match_cache, 0,
                    scratch->container_match_cache_capacity
                        * sizeof(*scratch->container_match_cache));
            if (STYLE_TRACE(sheet, LAYOUT)) {
                fprintf(stderr,
                        "style-container-state node=%p type=%u names=%08x "
                        "content=%dx%d\n",
                        (void *) node, (unsigned) type, (unsigned) names,
                        state->content_width, state->content_height);
            }
            return;
        }
    }
    /* A pathological collision cluster degrades by omitting this container,
       never by growing an unbounded chain. */
}

/* Whether `node` establishes a container, and its type and names. Resolved
   with the geometry table hidden: size-querying container-type is
   circular by definition. */
static void container_establishment(Stylesheet *sheet, lxb_dom_node_t *node,
                                    uint8_t *type, uint32_t *names)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    *type = STYLE_CONTAINER_TYPE_NONE;
    *names = 0;
    char type_value[96] = {0};
    char name_value[96] = {0};
    char shorthand[96] = {0};

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
            trim(&name, &name_length);
            trim(&type_text, &type_length);
            (void) parse_container_type_value(type_text, type_length, type);
            *names = container_name_bits(sheet, name, name_length);
        } else {
            const char *value = shorthand;
            size_t value_length = strlen(shorthand);
            trim(&value, &value_length);
            if (!span_case_equal(value, value_length, "none")
                && !parse_container_type_value(
                    value, value_length, type)) {
                /* A recognized type may stand alone. Any other non-none
                   token is the name-only shorthand and leaves the type at
                   normal. */
                *names = container_name_bits(sheet, value, value_length);
            }
        }
    }
    if (has_type) {
        (void) parse_container_type_value(
            type_value, strlen(type_value), type);
    }
    if (has_name) {
        *names = container_name_bits(sheet, name_value, strlen(name_value));
    }
}

bool style_container_layout_state_add(Stylesheet *sheet,
                                      lxb_dom_node_t *node,
                                      int content_width,
                                      int content_height,
                                      int padding_horizontal,
                                      int padding_vertical)
{
    return style_container_layout_state_add_contained(
        sheet, node, content_width, content_height, padding_horizontal,
        padding_vertical, -1);
}

bool style_container_layout_state_add_contained(Stylesheet *sheet,
                                                lxb_dom_node_t *node,
                                                int content_width,
                                                int content_height,
                                                int padding_horizontal,
                                                int padding_vertical,
                                                int size_contained_height)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL) {
        return false;
    }
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->container_states == NULL
        || scratch->container_state_capacity == 0) return false;

    uint8_t type = STYLE_CONTAINER_TYPE_NONE;
    uint32_t names = 0;
    container_establishment(sheet, node, &type, &names);
    if (type == STYLE_CONTAINER_TYPE_NONE && names == 0) return true;

    /* LayoutNodeBox client dimensions describe the padding box. Probe layout
       retains the already-resolved padding sums in a compact sidecar, so an
       actual container does not trigger a second ancestor/cascade walk. */
    content_width -= padding_horizontal;
    content_height -= padding_vertical;
    /* Size containment: the layout still sizes the box by its content, but
       its block-axis queries answer as browsers do, and never against
       content their own rules resize. */
    if (type == STYLE_CONTAINER_TYPE_SIZE && size_contained_height >= 0)
        content_height = size_contained_height;

    container_state_store(sheet, node, names, type, content_width,
                          content_height);
    return true;
}

void style_container_live_update(Stylesheet *sheet, lxb_dom_node_t *node,
                                 int content_width, int content_height)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || !style_container_log_active(sheet)
        || sheet->resolve_scratch->container_states == NULL) return;
    uint8_t type = STYLE_CONTAINER_TYPE_NONE;
    uint32_t names = 0;
    container_establishment(sheet, node, &type, &names);
    if (type == STYLE_CONTAINER_TYPE_NONE && names == 0) return;
    /* The height the last measured pass recorded stands (it knows, say,
       a flex item's stretched size, which this box cannot yet). A size
       container's contained height is the first guess without one. */
    const StyleContainerState *known = container_state_find(sheet, node);
    if (known != NULL) content_height = known->content_height;
    else if (content_height < 0 || type != STYLE_CONTAINER_TYPE_SIZE)
        content_height = 0;
    container_state_store(sheet, node, names, type, content_width,
                          content_height);
}

uint32_t style_container_consults(const Stylesheet *sheet)
{
    return sheet == NULL ? 0 : sheet->container_consults;
}

static StyleContainerLog *container_log_of(const Stylesheet *sheet)
{
    return sheet == NULL || sheet->resolve_scratch == NULL
        ? NULL : sheet->resolve_scratch->container_log;
}

bool style_container_log_active(const Stylesheet *sheet)
{
    const StyleContainerLog *log = container_log_of(sheet);
    return log != NULL && log->active;
}

bool style_container_log_begin(Stylesheet *sheet, Budget *budget,
                               const void *owner)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || budget == NULL
        || sheet->budget == NULL || owner == NULL
        || sheet->has_container_relative_units
        || sheet->conditional_queries == NULL
        || style_container_log_active(sheet)) return false;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->container_states == NULL) {
        /* No measured geometry yet: start an empty table the pass fills as
           it goes (an absent container answers as it did before). */
        if (!style_container_layout_state_begin(
                sheet, budget, STYLE_CONTAINER_LOG_STATE_GUESS)) return false;
    } else if (scratch->container_match_cache != NULL) {
        /* Results cached by an earlier pass were logged by that pass. */
        memset(scratch->container_match_cache, 0,
               scratch->container_match_cache_capacity
                   * sizeof(*scratch->container_match_cache));
    }
    StyleContainerLog *log = scratch->container_log;
    if (log == NULL) {
        log = budget_calloc_category(sheet->budget, BUDGET_CATEGORY_LAYOUT,
                                     1, sizeof(*log));
        if (log == NULL) return false;
        log->entries = budget_malloc_category(
            sheet->budget, BUDGET_CATEGORY_LAYOUT,
            STYLE_CONTAINER_LOG_INITIAL * sizeof(*log->entries));
        if (log->entries == NULL) {
            budget_free(sheet->budget, log);
            return false;
        }
        log->capacity = STYLE_CONTAINER_LOG_INITIAL;
        scratch->container_log = log;
    }
    log->count = 0;
    log->overflow = false;
    log->owner = owner;
    log->active = true;
    return true;
}

void style_container_log_end(Stylesheet *sheet, const void *owner)
{
    StyleContainerLog *log = container_log_of(sheet);
    if (log == NULL || log->owner != owner) return;
    log->active = false;
    log->owner = NULL;
}

void style_container_log_release(Stylesheet *sheet)
{
    StyleContainerLog *log = container_log_of(sheet);
    if (log == NULL) return;
    if (sheet->budget != NULL) {
        budget_free(sheet->budget, log->entries);
        budget_free(sheet->budget, log);
    }
    sheet->resolve_scratch->container_log = NULL;
}

static void container_log_note(Stylesheet *sheet, StyleContainerLog *log,
                               lxb_dom_node_t *node, uint8_t query_id,
                               bool matched)
{
    if (log->overflow) return;
    if (log->count == log->capacity) {
        size_t capacity = log->capacity * 2u;
        StyleContainerLogEntry *grown =
            capacity > STYLE_CONTAINER_LOG_LIMIT || sheet->budget == NULL
            ? NULL
            : budget_realloc_category(sheet->budget, BUDGET_CATEGORY_LAYOUT,
                                      log->entries,
                                      capacity * sizeof(*grown));
        if (grown == NULL) {
            /* Unverifiable, never wrong: the pass is rebuilt. */
            log->overflow = true;
            return;
        }
        log->entries = grown;
        log->capacity = capacity;
    }
    log->entries[log->count++] = (StyleContainerLogEntry) {
        .node = node, .query = query_id, .matched = matched
    };
}

void style_container_layout_state_clear(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return;
    sheet->container_state_generation++;
    sheet->container_state_complete = false;
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

bool style_container_layout_state_present(const Stylesheet *sheet)
{
    return sheet != NULL && sheet->resolve_scratch != NULL
        && sheet->resolve_scratch->container_states != NULL;
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

static bool query_matches_one(const Stylesheet *sheet, uint8_t query_id,
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
        /* Container selection stops at the first eligible ancestor even
           when its query evaluates false. */
        uint64_t bit = UINT64_C(1) << (query_id - 1u);
        if ((state->conditions_evaluated & bit) != 0)
            return (state->conditions_matched & bit) != 0;
        bool matched = style_container_condition_matches(
            query->condition, query->condition_length,
            state->content_width, state->content_height,
            sheet->viewport_width, sheet->viewport_height);
        state->conditions_evaluated |= bit;
        if (matched) state->conditions_matched |= bit;
        return matched;
    }
    return false;
}

static bool container_query_evaluate(const Stylesheet *sheet,
                                     uint8_t query_id, lxb_dom_node_t *node)
{
    if (query_id == 0) return true;
    if (query_id > sheet->conditional_queries->query_count) return false;
    const StyleContainerQuery *query =
        &sheet->conditional_queries->queries[query_id - 1u];
    return (query->parent == 0
            || container_query_evaluate(sheet, query->parent, node))
        && query_matches_one(sheet, query_id, query, node);
}

bool style_container_log_verified(const Stylesheet *sheet)
{
    const StyleContainerLog *log = container_log_of(sheet);
    if (log == NULL || log->overflow || sheet->conditional_queries == NULL
        || sheet->resolve_scratch->container_states == NULL) return false;
    for (size_t i = 0; i < log->count; i++) {
        const StyleContainerLogEntry *entry = &log->entries[i];
        if (container_query_evaluate(sheet, entry->query, entry->node)
            != entry->matched) return false;
    }
    return true;
}

bool style_container_query_matches(const Stylesheet *sheet, uint8_t query_id,
                                   lxb_dom_node_t *node)
{
    if (query_id == 0) return true;
    if (sheet != NULL) ((Stylesheet *) sheet)->container_consults++;
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
    if (matched) matched = query_matches_one(sheet, query_id, query, node);
    if (scratch->container_log != NULL && scratch->container_log->active)
        container_log_note((Stylesheet *) sheet, scratch->container_log,
                           node, query_id, matched);
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
    /* No container has been recorded: every lookup below would miss. */
    if (scratch->container_states == NULL
        || scratch->container_state_capacity == 0) return;
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
