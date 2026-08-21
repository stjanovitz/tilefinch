static uint64_t stylesheet_next_generation(void)
{
    stylesheet_generation_sequence++;
    if (stylesheet_generation_sequence == 0) stylesheet_generation_sequence++;
    return stylesheet_generation_sequence;
}

typedef struct {
    lxb_dom_node_t *node;
    uint32_t hash;
    uint32_t stamp;
    uint16_t name_length;
    uint8_t occupied;
    uint8_t found;
    char name[sizeof(((StyleCustomRule *) 0)->name)];
    char value[sizeof(((StyleCustomRule *) 0)->value)];
} StyleVariableCacheEntry;

typedef struct StyleVariableCache {
    Budget *budget;
    StyleVariableCacheEntry *entries;
    size_t capacity;
    uint32_t clock;
} StyleVariableCache;

static size_t variable_cache_capacity(void)
{
    size_t capacity = STYLE_VARIABLE_CACHE_DEFAULT_ENTRIES;
#ifndef TILEFINCH_NO_TRACE
    const char *configured = getenv("TILEFINCH_VARIABLE_CACHE_ENTRIES");
    if (configured != NULL && configured[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(configured, &end, 10);
        if (end != configured && *end == '\0'
            && parsed >= STYLE_VARIABLE_CACHE_MIN_ENTRIES
            && parsed <= STYLE_VARIABLE_CACHE_MAX_ENTRIES
            && (parsed & (parsed - 1u)) == 0) {
            capacity = (size_t) parsed;
        }
    }
#endif
    return capacity;
}

bool style_variable_cache_begin(Stylesheet *sheet, Budget *budget)
{
    if (sheet == NULL || budget == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache != NULL) {
        return false;
    }
    bool custom_properties = sheet->variable_count != 0;
    for (size_t i = 0; !custom_properties
                       && i < sheet->custom_rule_count; i++) {
        const char *name = sheet->custom_rules[i].name;
        custom_properties = name[0] == '-' && name[1] == '-';
    }
    if (!custom_properties) return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_VARIABLE_CACHE") != NULL) return false;
#endif
    size_t capacity = variable_cache_capacity();
    StyleVariableCache *cache = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*cache));
    if (cache == NULL) return false;
    cache->entries = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, capacity, sizeof(*cache->entries));
    if (cache->entries == NULL) {
        budget_free(budget, cache);
        return false;
    }
    cache->budget = budget;
    cache->capacity = capacity;
    sheet->resolve_scratch->variable_cache = cache;
    size_t bytes = sizeof(*cache) + capacity * sizeof(*cache->entries);
    if (bytes > sheet->variable_cache_peak_bytes) {
        sheet->variable_cache_peak_bytes = bytes;
    }
    return true;
}

void style_variable_cache_end(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache == NULL) return;
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    sheet->resolve_scratch->variable_cache = NULL;
    Budget *budget = cache->budget;
    budget_free(budget, cache->entries);
    budget_free(budget, cache);
}

void style_variable_cache_invalidate_node(
    Stylesheet *sheet, const lxb_dom_node_t *node)
{
    if (sheet == NULL || node == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache == NULL) return;
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    /*
     * Open-addressed lookup stops at the first empty entry, so deleting one
     * slot would make later colliding entries unreachable. Clearing the
     * bounded table is both correct and rare: this is used only while
     * comparing the one active focus marker during a layout snapshot.
     */
    memset(cache->entries, 0, cache->capacity * sizeof(*cache->entries));
}

size_t style_variable_cache_bytes(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache == NULL) return 0;
    const StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    return sizeof(*cache) + cache->capacity * sizeof(*cache->entries);
}

bool style_selector_cooperation_begin(
    Stylesheet *sheet, StyleSelectorCooperate cooperate, void *opaque)
{
    if (sheet == NULL || cooperate == NULL
        || sheet->selector_cooperate != NULL) return false;
    sheet->selector_cooperate = cooperate;
    sheet->selector_cooperate_opaque = opaque;
    sheet->selector_cooperate_visits = 0;
    sheet->selector_cooperate_quota = 8192u;
#ifndef TILEFINCH_NO_TRACE
    const char *configured = getenv("TILEFINCH_SELECTOR_COOPERATE_VISITS");
    if (configured != NULL && configured[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(configured, &end, 10);
        if (end != configured && *end == '\0'
            && parsed >= 64u && parsed <= 65536u) {
            sheet->selector_cooperate_quota = (size_t) parsed;
        }
    }
#endif
    sheet->selector_cooperate_next = sheet->selector_cooperate_quota;
    sheet->selector_cooperate_cancelled = false;
    return true;
}

void style_selector_cooperation_end(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    sheet->selector_cooperate = NULL;
    sheet->selector_cooperate_opaque = NULL;
    sheet->selector_cooperate_visits = 0;
    sheet->selector_cooperate_next = 0;
    sheet->selector_cooperate_quota = 0;
    sheet->selector_cooperate_cancelled = false;
}

bool style_selector_cooperation_cancelled(const Stylesheet *sheet)
{
    return sheet != NULL && sheet->selector_cooperate_cancelled;
}

static void stylesheet_read_trace_environment(Stylesheet *sheet)
{
#ifndef TILEFINCH_NO_TRACE
    sheet->trace_flags = (uint8_t) (
        (getenv("TILEFINCH_TRACE_PAINT") != NULL ? STYLE_TRACE_PAINT : 0u)
        | (getenv("TILEFINCH_TRACE_LAYOUT") != NULL ? STYLE_TRACE_LAYOUT : 0u));
#else
    (void) sheet;
#endif
}

static const char *stylesheet_store_selector(Stylesheet *sheet,
                                             const char *text,
                                             size_t length)
{
    if (sheet == NULL || sheet->budget == NULL || text == NULL
        || length >= STYLE_SELECTOR_CAPACITY
        || length == SIZE_MAX) return NULL;
    size_t required = length + 1u;
    StyleTextChunk *chunk = sheet->selector_chunks_tail;
    if (chunk == NULL || required > chunk->capacity - chunk->used) {
        size_t capacity = chunk == NULL
                          ? STYLE_SELECTOR_CHUNK_MIN_BYTES
                          : chunk->capacity;
        if (chunk != NULL && capacity < STYLE_SELECTOR_CHUNK_MAX_BYTES) {
            capacity *= 2u;
        }
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) return NULL;
            capacity *= 2u;
        }
        if (capacity > SIZE_MAX - sizeof(*chunk)) return NULL;
        size_t allocation = sizeof(*chunk) + capacity;
        chunk = budget_malloc(sheet->budget, allocation);
        if (chunk == NULL) return NULL;
        *chunk = (StyleTextChunk) {.capacity = capacity};
        if (sheet->selector_chunks_tail == NULL) {
            sheet->selector_chunks = chunk;
        } else {
            sheet->selector_chunks_tail->next = chunk;
        }
        sheet->selector_chunks_tail = chunk;
        sheet->selector_storage_bytes += allocation;
    }
    char *stored = chunk->data + chunk->used;
    memcpy(stored, text, length);
    stored[length] = '\0';
    chunk->used += required;
    sheet->selector_bytes += required;
    return stored;
}


static bool inline_variable_value(const Stylesheet *sheet,
                                  lxb_dom_node_t *node, const char *name,
                                  size_t name_length, char *output,
                                  size_t output_size, bool *important_output)
{
    size_t length = 0;
    const char *text = sheet != NULL && sheet->block_inline_style_attributes
            && !document_style_attribute_cssom_authorized(node)
        ? NULL : document_attribute(node, "style", &length);
    if (text == NULL) return false;
    bool found = false;
    bool found_important = false;
    for (size_t at = 0; at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *candidate = text + at;
            size_t candidate_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&candidate, &candidate_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            bool custom_property = name_length > 2
                && name[0] == '-' && name[1] == '-';
            if (candidate_length == name_length
                && (custom_property
                    ? memcmp(candidate, name, name_length) == 0
                    : strncasecmp(candidate, name, name_length) == 0)
                && value_length < output_size
                && (!found || important || !found_important)) {
                memcpy(output, value, value_length);
                output[value_length] = '\0';
                found = true;
                found_important = important;
            }
        }
        at = end + (end < length);
    }
    if (found && important_output != NULL) {
        *important_output = found_important;
    }
    return found;
}

static bool custom_rule_later(const StyleCustomRule *candidate,
                              const StyleCustomRule *current)
{
    if (current == NULL) return true;
    if (candidate->important != current->important) {
        return candidate->important;
    }
    if (candidate->origin != current->origin) {
        return candidate->origin > current->origin;
    }
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
    if (candidate->specificity != current->specificity) {
        return candidate->specificity > current->specificity;
    }
    return candidate->order >= current->order;
}

struct StyleCustomRuleIndexEntry {
    uint32_t hash;
    uint32_t index;
};

static uint32_t custom_rule_name_hash(const char *text, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) text[i];
        hash *= UINT32_C(16777619);
    }
    return hash == 0 ? UINT32_C(1) : hash;
}

static const char *const retained_presentation_names[
    STYLE_RETAINED_PRESENTATION_COUNT] = {
    "fill", "fill-opacity", "fill-rule", "stroke",
    "stroke-opacity", "stroke-width", "stroke-linecap",
    "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
    "stroke-dashoffset", "opacity"
};

const char *style_retained_presentation_name(size_t index)
{
    return index < STYLE_RETAINED_PRESENTATION_COUNT
        ? retained_presentation_names[index] : NULL;
}

static int retained_presentation_index(const char *name, size_t name_length)
{
    for (size_t i = 0; i < STYLE_RETAINED_PRESENTATION_COUNT; i++) {
        const char *wanted = retained_presentation_names[i];
        size_t wanted_length = strlen(wanted);
        if (name_length == wanted_length
            && strncasecmp(name, wanted, wanted_length) == 0) {
            return (int) i;
        }
    }
    return -1;
}

static int compare_custom_rule_index_entries(const void *left,
                                             const void *right)
{
    const StyleCustomRuleIndexEntry *a = left;
    const StyleCustomRuleIndexEntry *b = right;
    if (a->hash != b->hash) return a->hash < b->hash ? -1 : 1;
    if (a->index == b->index) return 0;
    return a->index < b->index ? -1 : 1;
}

static size_t custom_rule_index_lower_bound(
    const StyleCustomRuleIndexEntry *entries, size_t count, uint32_t hash)
{
    size_t begin = 0;
    while (begin < count) {
        size_t middle = begin + (count - begin) / 2u;
        if (entries[middle].hash < hash) begin = middle + 1u;
        else count = middle;
    }
    return begin;
}

static size_t variable_cache_home(const StyleVariableCache *cache,
                                  lxb_dom_node_t *node, uint32_t hash)
{
    uintptr_t node_bits = (uintptr_t) node;
    uint32_t mixed = hash ^ (uint32_t) node_bits;
#if UINTPTR_MAX > UINT32_MAX
    mixed ^= (uint32_t) (node_bits >> 32u);
#endif
    mixed ^= mixed >> 16u;
    mixed *= UINT32_C(0x7feb352d);
    mixed ^= mixed >> 15u;
    return (size_t) mixed & (cache->capacity - 1u);
}

/* -1 means uncached, 0 is a cached miss, and 1 is a cached value. */
static int variable_cache_lookup(Stylesheet *sheet, lxb_dom_node_t *node,
                                 const char *name, size_t name_length,
                                 uint32_t hash, char *output,
                                 size_t output_size)
{
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    if (cache == NULL || name_length >= sizeof(cache->entries[0].name)) {
        return -1;
    }
    size_t home = variable_cache_home(cache, node, hash);
    for (size_t probe = 0; probe < STYLE_VARIABLE_CACHE_PROBES; probe++) {
        StyleVariableCacheEntry *entry =
            &cache->entries[(home + probe) & (cache->capacity - 1u)];
        if (!entry->occupied) break;
        if (entry->node != node || entry->hash != hash
            || entry->name_length != name_length
            || memcmp(entry->name, name, name_length) != 0) continue;
        entry->stamp = ++cache->clock;
        sheet->variable_cache_hits++;
        if (!entry->found) {
            sheet->variable_cache_negative_hits++;
            return 0;
        }
        snprintf(output, output_size, "%s", entry->value);
        return 1;
    }
    sheet->variable_cache_misses++;
    return -1;
}

static void variable_cache_store(Stylesheet *sheet, lxb_dom_node_t *node,
                                 const char *name, size_t name_length,
                                 uint32_t hash, const char *value)
{
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    if (cache == NULL || name_length >= sizeof(cache->entries[0].name)) return;
    /* Long inline custom-property values remain owned by the DOM attribute.
       Never truncate one into the compact cache: a later var() lookup would
       observe a syntactically plausible but invalid URL. Re-reading the rare
       long value is cheaper than enlarging every cache entry on the PSP. */
    if (value != NULL && strlen(value) >= sizeof(cache->entries[0].value)) {
        return;
    }
    size_t home = variable_cache_home(cache, node, hash);
    StyleVariableCacheEntry *target = NULL;
    uint32_t oldest_age = 0;
    for (size_t probe = 0; probe < STYLE_VARIABLE_CACHE_PROBES; probe++) {
        StyleVariableCacheEntry *entry =
            &cache->entries[(home + probe) & (cache->capacity - 1u)];
        if (!entry->occupied) {
            target = entry;
            break;
        }
        uint32_t age = cache->clock - entry->stamp;
        if (target == NULL || age > oldest_age) {
            target = entry;
            oldest_age = age;
        }
    }
    if (target == NULL) return;
    if (target->occupied) sheet->variable_cache_evictions++;
    *target = (StyleVariableCacheEntry) {
        .node = node,
        .hash = hash,
        .stamp = ++cache->clock,
        .name_length = (uint16_t) name_length,
        .occupied = 1,
        .found = value != NULL
    };
    memcpy(target->name, name, name_length);
    target->name[name_length] = '\0';
    if (value != NULL) {
        snprintf(target->value, sizeof(target->value), "%s", value);
    }
}

static bool retained_rule_fast_matches(const StyleCustomRule *rule,
                                        const StyleMatchSubject *subject);

static bool local_variable_value(
    Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement wanted_pseudo,
    const char *name, size_t name_length, uint32_t hash,
    char *output, size_t output_size)
{
    const StyleCustomRule *winner = NULL;
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    size_t first = sheet->custom_rule_index_ready
        ? custom_rule_index_lower_bound(
              sheet->custom_rule_index,
              sheet->custom_rule_index_count, hash)
        : 0;
    size_t end = sheet->custom_rule_index_ready
        ? sheet->custom_rule_index_count : sheet->custom_rule_count;
    for (size_t i = first; i < end; i++) {
        size_t rule_index = i;
        if (sheet->custom_rule_index_ready) {
            if (sheet->custom_rule_index[i].hash != hash) break;
            rule_index = sheet->custom_rule_index[i].index;
        }
        sheet->variable_rule_candidates++;
        const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
        if (rule->pseudo != (uint8_t) wanted_pseudo
            || !style_container_query_matches(
                sheet, rule->container_query, node)
            || strlen(rule->name) != name_length
            || memcmp(rule->name, name, name_length) != 0
            || !retained_rule_fast_matches(rule, &subject)
            || !style_selector_matches_profiled(
                sheet, node, rule->selector,
                strlen(rule->selector))) continue;
        if (custom_rule_later(rule, winner)) winner = rule;
    }

    bool inline_important = false;
    StyleCustomRule inline_rule = {0};
    bool inline_wins = false;
    bool inline_found = wanted_pseudo == PSEUDO_NONE
        && inline_variable_value(
            sheet, node, name, name_length, output,
            output_size, &inline_important);
    if (inline_found) {
        inline_rule.important = inline_important;
        inline_rule.origin = 0;
        inline_rule.layer = UINT_MAX;
        inline_rule.specificity = UINT_MAX;
        inline_rule.order = UINT_MAX;
        inline_wins = custom_rule_later(&inline_rule, winner);
    }
    if (inline_wins) return true;
    if (winner == NULL) return false;
    snprintf(output, output_size, "%s", winner->value);
    return true;
}

static void variable_cache_store_nodes(
    Stylesheet *sheet, lxb_dom_node_t *const *nodes, size_t node_count,
    const char *name, size_t name_length, uint32_t hash, const char *value)
{
    for (size_t i = 0; i < node_count; i++) {
        variable_cache_store(
            sheet, nodes[i], name, name_length, hash, value);
    }
}

static bool lookup_variable(const Stylesheet *sheet, const char *name,
                            size_t name_length, char *output,
                            size_t output_size)
{
    if (sheet == NULL || output == NULL || output_size == 0) return false;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    mutable_sheet->variable_lookup_calls++;
    lxb_dom_node_t *resolution_node = sheet->resolve_scratch->resolution_node;
    uint32_t hash = custom_rule_name_hash(name, name_length);
    PseudoElement resolution_pseudo =
        sheet->resolve_scratch->resolution_pseudo;
    /* The bounded cache is otherwise keyed by node and property name. Salt
       pseudo lookups so an originating element and its generated boxes
       cannot reuse one another's winning custom property. */
    uint32_t cache_hash = hash
        ^ ((uint32_t) resolution_pseudo * UINT32_C(0x9e3779b9));
    int cached = variable_cache_lookup(
        mutable_sheet, resolution_node, name, name_length, cache_hash, output,
        output_size);
    if (cached >= 0) return cached != 0;

    /* Generated boxes cascade their own declarations before inheriting the
       originating element's ordinary environment. Keep that distinct cache
       key, but do not repeat the ancestor walk when the pseudo has no local
       winner. */
    if (resolution_pseudo != PSEUDO_NONE
        && local_variable_value(
            mutable_sheet, resolution_node, resolution_pseudo,
            name, name_length, hash, output, output_size)) {
        variable_cache_store(
            mutable_sheet, resolution_node, name, name_length,
            cache_hash, output);
        return true;
    }

    enum { CACHED_DESCENDANT_LIMIT = 64 };
    lxb_dom_node_t *uncached_nodes[CACHED_DESCENDANT_LIMIT];
    size_t uncached_count = 0;
    for (lxb_dom_node_t *node = resolution_node; node != NULL;
         node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        /* The ordinary-element lookup at the function entrance already
           established this first miss. Do not probe and count it twice. */
        int inherited = node == resolution_node
                && resolution_pseudo == PSEUDO_NONE
            ? -1
            : variable_cache_lookup(
                  mutable_sheet, node, name, name_length, hash,
                  output, output_size);
        if (inherited >= 0) {
            const char *value = inherited != 0 ? output : NULL;
            variable_cache_store_nodes(
                mutable_sheet, uncached_nodes, uncached_count,
                name, name_length, hash, value);
            if (resolution_pseudo != PSEUDO_NONE) {
                variable_cache_store(
                    mutable_sheet, resolution_node, name, name_length,
                    cache_hash, value);
            }
            return inherited != 0;
        }
        if (uncached_count < CACHED_DESCENDANT_LIMIT) {
            uncached_nodes[uncached_count++] = node;
        }
        if (local_variable_value(
                mutable_sheet, node, PSEUDO_NONE, name, name_length,
                hash, output, output_size)) {
            variable_cache_store_nodes(
                mutable_sheet, uncached_nodes, uncached_count,
                name, name_length, hash, output);
            if (resolution_pseudo != PSEUDO_NONE) {
                variable_cache_store(
                    mutable_sheet, resolution_node, name, name_length,
                    cache_hash, output);
            }
            return true;
        }
    }
    const char *global_value = NULL;
    for (size_t i = sheet->variable_count; i != 0; i--) {
        const StyleVariable *variable = &sheet->variables[i - 1];
        if (strlen(variable->name) == name_length
            && memcmp(variable->name, name, name_length) == 0) {
            snprintf(output, output_size, "%s", variable->value);
            global_value = output;
            break;
        }
    }
    variable_cache_store_nodes(
        mutable_sheet, uncached_nodes, uncached_count,
        name, name_length, hash, global_value);
    if (resolution_pseudo != PSEUDO_NONE) {
        variable_cache_store(
            mutable_sheet, resolution_node, name, name_length,
            cache_hash, global_value);
    }
    return global_value != NULL;
}

bool style_custom_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || name == NULL || name_length < 3 || name[0] != '-'
        || name[1] != '-' || output == NULL || output_size == 0) {
        return false;
    }
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = pseudo;
    char raw[sizeof(((StyleCustomRule *) 0)->value)];
    bool found = lookup_variable(
        sheet, name, name_length, raw, sizeof(raw));
    if (found) {
        /* CSSOM exposes a custom property's computed value, not the winning
           token stream.  Resolving here also maps cycles and unresolved
           references to the guaranteed-invalid empty serialization without
           retaining every custom property in ComputedStyle. */
        found = style_resolve_value(
            sheet, raw, strlen(raw), output, output_size, 0);
    }
    if (!found) output[0] = '\0';
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return found;
}

bool style_retained_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || name == NULL || name_length == 0
        || name_length >= sizeof(((StyleCustomRule *) 0)->name)
        || output == NULL || output_size == 0) {
        return false;
    }
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    uint32_t hash = custom_rule_name_hash(name, name_length);
    const StyleCustomRule *winner = NULL;
    size_t first = sheet->custom_rule_index_ready
        ? custom_rule_index_lower_bound(
              sheet->custom_rule_index, sheet->custom_rule_index_count, hash)
        : 0;
    size_t end = sheet->custom_rule_index_ready
        ? sheet->custom_rule_index_count : sheet->custom_rule_count;
    for (size_t i = first; i < end; i++) {
        size_t rule_index = i;
        if (sheet->custom_rule_index_ready) {
            if (sheet->custom_rule_index[i].hash != hash) break;
            rule_index = sheet->custom_rule_index[i].index;
        }
        const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
        if (rule->pseudo != PSEUDO_NONE
            || !style_container_query_matches(
                sheet, rule->container_query, node)
            || strlen(rule->name) != name_length
            || memcmp(rule->name, name, name_length) != 0
            || !retained_rule_fast_matches(rule, &subject)
            || !style_selector_matches_profiled(
                sheet, node, rule->selector, strlen(rule->selector))) {
            continue;
        }
        if (custom_rule_later(rule, winner)) winner = rule;
    }
    char inline_value[sizeof(((StyleCustomRule *) 0)->value)];
    bool inline_important = false;
    StyleCustomRule inline_rule = {0};
    if (inline_variable_value(
            sheet, node, name, name_length, inline_value,
            sizeof(inline_value),
            &inline_important)) {
        inline_rule.important = inline_important;
        inline_rule.origin = 0;
        inline_rule.layer = UINT_MAX;
        inline_rule.specificity = UINT_MAX;
        inline_rule.order = UINT_MAX;
        snprintf(inline_rule.value, sizeof(inline_rule.value), "%s",
                 inline_value);
        if (custom_rule_later(&inline_rule, winner)) winner = &inline_rule;
    }
    if (winner == NULL) {
        output[0] = '\0';
        style_relative_selector_cache_end((Stylesheet *) sheet);
        return false;
    }

    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = PSEUDO_NONE;
    bool found = style_resolve_value(
        sheet, winner->value, strlen(winner->value), output, output_size, 0);
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    if (!found) output[0] = '\0';
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return found;
}

typedef struct {
    const StyleCustomRule *rule;
    StyleCustomRule inline_rule;
    char inline_value[sizeof(((StyleCustomRule *) 0)->value)];
} RetainedRuleWinner;

static bool retained_rule_fast_matches(const StyleCustomRule *rule,
                                        const StyleMatchSubject *subject)
{
    if (rule == NULL || subject == NULL
        || rule->fast_key_offset == UINT8_MAX) return true;
    size_t selector_length = strlen(rule->selector);
    size_t offset = rule->fast_key_offset;
    if (offset >= selector_length) return true;
    size_t end = skip_selector_identifier(
        rule->selector, selector_length, offset);
    if (end <= offset) return true;
    const char *key = rule->selector + offset;
    size_t key_length = end - offset;
    char marker = offset == 0 ? '\0' : rule->selector[offset - 1u];
    if (marker == '#') {
        return subject->id != NULL && subject->id_length == key_length
            && memcmp(subject->id, key, key_length) == 0;
    }
    if (marker == '.') {
        return subject->classes != NULL
            && class_contains_length(
                subject->classes, subject->classes_length, key, key_length);
    }
    return subject->tag != NULL && subject->tag_length == key_length
        && memcmp(subject->tag, key, key_length) == 0;
}

static void retained_rule_winner(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, RetainedRuleWinner *output)
{
    memset(output, 0, sizeof(*output));
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    uint32_t hash = custom_rule_name_hash(name, name_length);
    size_t first = sheet->custom_rule_index_ready
        ? custom_rule_index_lower_bound(
              sheet->custom_rule_index, sheet->custom_rule_index_count, hash)
        : 0;
    size_t end = sheet->custom_rule_index_ready
        ? sheet->custom_rule_index_count : sheet->custom_rule_count;
    for (size_t i = first; i < end; i++) {
        size_t rule_index = i;
        if (sheet->custom_rule_index_ready) {
            if (sheet->custom_rule_index[i].hash != hash) break;
            rule_index = sheet->custom_rule_index[i].index;
        }
        const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
        if (rule->pseudo != PSEUDO_NONE
            || !style_container_query_matches(
                sheet, rule->container_query, node)
            || strlen(rule->name) != name_length
            || memcmp(rule->name, name, name_length) != 0
            || !retained_rule_fast_matches(rule, &subject)
            || !style_selector_matches_profiled(
                sheet, node, rule->selector, strlen(rule->selector))) {
            continue;
        }
        if (custom_rule_later(rule, output->rule)) output->rule = rule;
    }
    size_t length = 0;
    const char *text = sheet->block_inline_style_attributes
            && !document_style_attribute_cssom_authorized(node)
        ? NULL : document_attribute(node, "style", &length);
    unsigned declaration_order = 0;
    for (size_t at = 0; text != NULL && at < length; declaration_order++) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *candidate_name = text + at;
            size_t candidate_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&candidate_name, &candidate_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            if (candidate_length == name_length
                && strncasecmp(candidate_name, name, name_length) == 0
                && value_length < sizeof(output->inline_value)) {
                StyleCustomRule candidate = {
                    .important = important,
                    .origin = 0,
                    .layer = UINT_MAX,
                    .specificity = UINT_MAX,
                    .order = declaration_order
                };
                memcpy(candidate.value, value, value_length);
                candidate.value[value_length] = '\0';
                if (custom_rule_later(&candidate, output->rule)) {
                    output->inline_rule = candidate;
                    snprintf(output->inline_value,
                             sizeof(output->inline_value), "%s",
                             candidate.value);
                    output->rule = &output->inline_rule;
                }
            }
        }
        at = end + (end < length);
    }
}

static unsigned retained_box_tokens(
    const char *value, char output[4][STYLE_RETAINED_BOX_VALUE_CAPACITY])
{
    size_t cursor = 0, start = 0, end = 0;
    unsigned count = 0;
    size_t length = strlen(value);
    while (count < 4
           && style_next_top_level(
               value, length, ' ', &cursor, &start, &end)) {
        size_t token_length = end - start;
        if (token_length == 0
            || token_length >= STYLE_RETAINED_BOX_VALUE_CAPACITY) {
            return 0;
        }
        memcpy(output[count], value + start, token_length);
        output[count][token_length] = '\0';
        count++;
    }
    if (count == 0 || cursor < length) return 0;
    if (count == 1) {
        memcpy(output[1], output[0], STYLE_RETAINED_BOX_VALUE_CAPACITY);
        memcpy(output[2], output[0], STYLE_RETAINED_BOX_VALUE_CAPACITY);
        memcpy(output[3], output[0], STYLE_RETAINED_BOX_VALUE_CAPACITY);
    } else if (count == 2) {
        memcpy(output[2], output[0], STYLE_RETAINED_BOX_VALUE_CAPACITY);
        memcpy(output[3], output[1], STYLE_RETAINED_BOX_VALUE_CAPACITY);
    } else if (count == 3) {
        memcpy(output[3], output[1], STYLE_RETAINED_BOX_VALUE_CAPACITY);
    }
    return count;
}

bool style_retained_box_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *shorthand, StyleRetainedBoxValues *output)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || shorthand == NULL || output == NULL) return false;
    size_t shorthand_length = strlen(shorthand);
    if (shorthand_length == 0 || shorthand_length > 32) return false;
    memset(output, 0, sizeof(*output));
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);

    RetainedRuleWinner winners[5];
    retained_rule_winner(
        sheet, node, shorthand, shorthand_length, &winners[0]);
    static const char *const suffixes[4] = {
        "-top", "-right", "-bottom", "-left"
    };
    char names[4][48];
    for (size_t side = 0; side < 4; side++) {
        int written = snprintf(
            names[side], sizeof(names[side]), "%s%s",
            shorthand, suffixes[side]);
        if (written <= 0 || (size_t) written >= sizeof(names[side])) {
            style_relative_selector_cache_end((Stylesheet *) sheet);
            return false;
        }
        retained_rule_winner(
            sheet, node, names[side], strlen(names[side]),
            &winners[side + 1]);
    }

    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = PSEUDO_NONE;
    char shorthand_values[4][STYLE_RETAINED_BOX_VALUE_CAPACITY] = {{0}};
    bool shorthand_valid = false;
    if (winners[0].rule != NULL) {
        char resolved[STYLE_RETAINED_BOX_VALUE_CAPACITY];
        shorthand_valid = style_resolve_value(
            sheet, winners[0].rule->value,
            strlen(winners[0].rule->value), resolved, sizeof(resolved), 0)
            && retained_box_tokens(resolved, shorthand_values) != 0;
    }
    for (size_t side = 0; side < 4; side++) {
        const StyleCustomRule *winner = winners[side + 1].rule;
        if (shorthand_valid
            && custom_rule_later(winners[0].rule, winner)) {
            winner = winners[0].rule;
        }
        if (winner == NULL) continue;
        bool found;
        if (winner == winners[0].rule) {
            memcpy(output->values[side], shorthand_values[side],
                   sizeof(output->values[side]));
            found = true;
        } else {
            found = style_resolve_value(
                sheet, winner->value, strlen(winner->value),
                output->values[side], sizeof(output->values[side]), 0);
        }
        if (found) output->present_mask |= (uint8_t) (1u << side);
    }
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return output->present_mask != 0;
}

bool style_retained_pair_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *shorthand, const char *first_longhand,
    const char *second_longhand, StyleRetainedPairValues *output)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || shorthand == NULL || first_longhand == NULL
        || second_longhand == NULL || output == NULL) return false;
    memset(output, 0, sizeof(*output));
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);

    RetainedRuleWinner winners[3];
    retained_rule_winner(
        sheet, node, shorthand, strlen(shorthand), &winners[0]);
    retained_rule_winner(
        sheet, node, first_longhand, strlen(first_longhand), &winners[1]);
    retained_rule_winner(
        sheet, node, second_longhand, strlen(second_longhand), &winners[2]);

    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = PSEUDO_NONE;
    char shorthand_values[4][STYLE_RETAINED_BOX_VALUE_CAPACITY] = {{0}};
    bool shorthand_valid = false;
    if (winners[0].rule != NULL) {
        char resolved[STYLE_RETAINED_BOX_VALUE_CAPACITY];
        unsigned token_count = 0;
        if (style_resolve_value(
                sheet, winners[0].rule->value,
                strlen(winners[0].rule->value), resolved,
                sizeof(resolved), 0)) {
            token_count = retained_box_tokens(resolved, shorthand_values);
        }
        shorthand_valid = token_count == 1 || token_count == 2;
    }
    for (size_t component = 0; component < 2; component++) {
        const StyleCustomRule *winner = winners[component + 1].rule;
        if (shorthand_valid
            && custom_rule_later(winners[0].rule, winner)) {
            winner = winners[0].rule;
        }
        if (winner == NULL) continue;
        bool found;
        if (winner == winners[0].rule) {
            memcpy(output->values[component],
                   shorthand_values[component],
                   sizeof(output->values[component]));
            found = true;
        } else {
            found = style_resolve_value(
                sheet, winner->value, strlen(winner->value),
                output->values[component],
                sizeof(output->values[component]), 0);
        }
        if (found) output->present_mask |=
            (uint8_t) (1u << component);
    }
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return output->present_mask != 0;
}

bool style_retained_presentation_value(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    return style_retained_property_value(
        sheet, node, name, name_length, output, output_size);
}

bool style_retained_presentation_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    StyleRetainedPresentationValues *output)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || output == NULL) return false;
    memset(output, 0, sizeof(*output));
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);

    const StyleCustomRule *winners[STYLE_RETAINED_PRESENTATION_COUNT] = {0};
    bool inline_important[STYLE_RETAINED_PRESENTATION_COUNT] = {false};
    size_t inline_length = 0;
    const char *inline_text = sheet->block_inline_style_attributes
            && !document_style_attribute_cssom_authorized(node)
        ? NULL : document_attribute(node, "style", &inline_length);
    for (size_t at = 0; inline_text != NULL && at < inline_length;) {
        at = skip_css_space_and_comments(inline_text, inline_length, at);
        if (at >= inline_length) break;
        size_t end = find_declaration_end(
            inline_text, inline_length, at);
        size_t colon = at;
        while (colon < end && inline_text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = inline_text + at;
            size_t name_length = colon - at;
            const char *value = inline_text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            int property = retained_presentation_index(name, name_length);
            uint16_t bit = property < 0 ? 0
                : (uint16_t) (UINT16_C(1) << property);
            if (property >= 0
                && value_length
                   < STYLE_RETAINED_PRESENTATION_VALUE_CAPACITY
                && ((output->present_mask & bit) == 0
                    || important || !inline_important[property])) {
                memcpy(output->values[property], value, value_length);
                output->values[property][value_length] = '\0';
                output->present_mask |= bit;
                inline_important[property] = important;
            }
        }
        at = end + (end < inline_length);
    }

    enum { SELECTOR_CACHE_CAPACITY = 16 };
    typedef struct {
        const char *selector;
        uint32_t hash;
        uint16_t length;
        bool occupied;
        bool matched;
    } PresentationSelectorCache;
    PresentationSelectorCache selector_cache[SELECTOR_CACHE_CAPACITY] = {0};
    for (size_t property = 0;
         property < STYLE_RETAINED_PRESENTATION_COUNT; property++) {
        const char *name = retained_presentation_names[property];
        size_t name_length = strlen(name);
        uint32_t hash = custom_rule_name_hash(name, name_length);
        size_t first = sheet->custom_rule_index_ready
            ? custom_rule_index_lower_bound(
                  sheet->custom_rule_index,
                  sheet->custom_rule_index_count, hash)
            : 0;
        size_t end = sheet->custom_rule_index_ready
            ? sheet->custom_rule_index_count : sheet->custom_rule_count;
        for (size_t i = first; i < end; i++) {
            size_t rule_index = i;
            if (sheet->custom_rule_index_ready) {
                if (sheet->custom_rule_index[i].hash != hash) break;
                rule_index = sheet->custom_rule_index[i].index;
            }
            const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
            if (rule->pseudo != PSEUDO_NONE
                || !style_container_query_matches(
                    sheet, rule->container_query, node)
                || strlen(rule->name) != name_length
                || memcmp(rule->name, name, name_length) != 0
                || !retained_rule_fast_matches(rule, &subject)) continue;
            size_t selector_length = strlen(rule->selector);
            uint32_t selector_hash = custom_rule_name_hash(
                rule->selector, selector_length);
            PresentationSelectorCache *cached =
                &selector_cache[selector_hash
                                & (SELECTOR_CACHE_CAPACITY - 1u)];
            bool matched;
            if (cached->occupied && cached->hash == selector_hash
                && cached->length == selector_length
                && memcmp(cached->selector, rule->selector,
                          selector_length) == 0) {
                matched = cached->matched;
            } else {
                matched = style_selector_matches_profiled(
                    sheet, node, rule->selector, selector_length);
                *cached = (PresentationSelectorCache) {
                    .selector = rule->selector,
                    .hash = selector_hash,
                    .length = selector_length > UINT16_MAX
                        ? UINT16_MAX : (uint16_t) selector_length,
                    .occupied = true,
                    .matched = matched
                };
            }
            if (matched && custom_rule_later(rule, winners[property])) {
                winners[property] = rule;
            }
        }
    }

    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = PSEUDO_NONE;
    for (size_t property = 0;
         property < STYLE_RETAINED_PRESENTATION_COUNT; property++) {
        uint16_t bit = (uint16_t) (UINT16_C(1) << property);
        const char *value = winners[property] == NULL
            ? NULL : winners[property]->value;
        if ((output->present_mask & bit) != 0) {
            StyleCustomRule inline_rule = {
                .important = inline_important[property],
                .origin = 0,
                .layer = UINT_MAX,
                .specificity = UINT_MAX,
                .order = UINT_MAX
            };
            if (custom_rule_later(&inline_rule, winners[property])) {
                value = output->values[property];
            }
        }
        if (value == NULL) {
            output->present_mask &= (uint16_t) ~bit;
            continue;
        }
        char resolved[STYLE_RETAINED_PRESENTATION_VALUE_CAPACITY];
        bool found = style_resolve_value(
            sheet, value, strlen(value), resolved, sizeof(resolved), 0);
        if (!found) {
            output->present_mask &= (uint16_t) ~bit;
            output->values[property][0] = '\0';
            continue;
        }
        snprintf(output->values[property],
                 sizeof(output->values[property]), "%s", resolved);
        output->present_mask |= bit;
    }
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return true;
}

bool style_set_variable(Stylesheet *sheet, const char *name,
                         size_t name_length, const char *value,
                         size_t value_length)
{
    if (name_length == 0 || name_length >= sizeof(((StyleVariable *) 0)->name)
        || value_length >= sizeof(((StyleVariable *) 0)->value)) {
        if (sheet->diagnostic_custom_property_drops != UINT64_MAX) {
            sheet->diagnostic_custom_property_drops++;
        }
        return true;
    }
    for (size_t i = 0; i < sheet->variable_count; i++) {
        StyleVariable *variable = &sheet->variables[i];
        if (strlen(variable->name) == name_length
            && memcmp(variable->name, name, name_length) == 0) {
            memcpy(variable->value, value, value_length);
            variable->value[value_length] = '\0';
            return true;
        }
    }
    if (sheet->variable_count == sheet->variable_capacity) {
        size_t capacity = sheet->variable_capacity == 0
                          ? 16 : sheet->variable_capacity * 2;
        StyleVariable *variables = budget_realloc(
            sheet->budget, sheet->variables, capacity * sizeof(*variables));
        if (variables == NULL) return false;
        sheet->variables = variables;
        sheet->variable_capacity = capacity;
    }
    StyleVariable *variable = &sheet->variables[sheet->variable_count++];
    memcpy(variable->name, name, name_length);
    variable->name[name_length] = '\0';
    memcpy(variable->value, value, value_length);
    variable->value[value_length] = '\0';
    return true;
}

bool style_resolve_value(const Stylesheet *sheet, const char *text,
                         size_t length, char *output, size_t output_size,
                         unsigned depth)
{
    trim(&text, &length);
    bool has_comment = false;
    char quote = 0;
    for (size_t at = 0; at + 1 < length; at++) {
        if (quote != 0) {
            if (text[at] == '\\' && at + 1 < length) at++;
            else if (text[at] == quote) quote = 0;
            continue;
        }
        if (text[at] == '\'' || text[at] == '"') {
            quote = text[at];
        } else if (text[at] == '/' && text[at + 1] == '*') {
            has_comment = true;
            break;
        }
    }
    if (has_comment) {
        /* CSS comments are removed before property-value tokenization. Keep
           the exceptional normalization path stack-bounded; ordinary values
           remain allocation-free and retain their existing larger caps. */
        char uncommented[512];
        size_t written = 0;
        quote = 0;
        for (size_t at = 0; at < length;) {
            if (quote != 0) {
                if (written + 1 >= sizeof(uncommented)) return false;
                uncommented[written++] = text[at];
                if (text[at] == '\\' && at + 1 < length) {
                    at++;
                    if (written + 1 >= sizeof(uncommented)) return false;
                    uncommented[written++] = text[at];
                } else if (text[at] == quote) {
                    quote = 0;
                }
                at++;
                continue;
            }
            if (text[at] == '\'' || text[at] == '"') {
                if (written + 1 >= sizeof(uncommented)) return false;
                quote = text[at];
                uncommented[written++] = text[at++];
                continue;
            }
            if (at + 1 < length && text[at] == '/'
                && text[at + 1] == '*') {
                at += 2;
                while (at + 1 < length
                       && !(text[at] == '*' && text[at + 1] == '/')) at++;
                if (at + 1 >= length) return false;
                at += 2;
                if (written != 0 && uncommented[written - 1] != ' ') {
                    if (written + 1 >= sizeof(uncommented)) return false;
                    uncommented[written++] = ' ';
                }
                continue;
            }
            if (written + 1 >= sizeof(uncommented)) return false;
            uncommented[written++] = text[at++];
        }
        uncommented[written] = '\0';
        return style_resolve_value(sheet, uncommented, written, output,
                                   output_size, depth);
    }
    size_t declaration_length = declaration_value_length(text, length);
    if (declaration_length != length) {
        length = declaration_length;
        trim(&text, &length);
    }
    size_t first = length;
    for (size_t i = 0; i + 4 <= length; i++) {
        if (memcmp(text + i, "var(", 4) == 0) { first = i; break; }
    }
    if (first == length) {
        if (length >= output_size) return false;
        memcpy(output, text, length);
        output[length] = '\0';
        return true;
    }
    if (depth >= 4) return false;
    size_t written = 0;
    size_t at = 0;
    while (at < length) {
        size_t start = at;
        while (start + 4 <= length
               && memcmp(text + start, "var(", 4) != 0) start++;
        if (start + 4 > length) {
            size_t remainder = length - at;
            if (written + remainder >= output_size) return false;
            memcpy(output + written, text + at, remainder);
            written += remainder;
            break;
        }
        size_t prefix = start - at;
        if (written + prefix >= output_size) return false;
        memcpy(output + written, text + at, prefix);
        written += prefix;
        size_t close = start + 4;
        int nested = 1;
        while (close < length && nested != 0) {
            if (text[close] == '(') nested++;
            else if (text[close] == ')') nested--;
            if (nested != 0) close++;
        }
        if (nested != 0) return false;
        const char *inside = text + start + 4;
        size_t inside_length = close - start - 4;
        size_t comma = 0;
        nested = 0;
        while (comma < inside_length) {
            if (inside[comma] == '(') nested++;
            else if (inside[comma] == ')' && nested > 0) nested--;
            else if (inside[comma] == ',' && nested == 0) break;
            comma++;
        }
        const char *name = inside;
        size_t name_length = comma;
        trim(&name, &name_length);
        /* Inline design-token URLs commonly exceed the retained-rule value
           cap. The resolver consumes them directly from the DOM and keeps
           this bounded scratch local; retained stylesheet rules stay compact. */
        char raw[320];
        const char *replacement = NULL;
        size_t replacement_length = 0;
        if (lookup_variable(sheet, name, name_length, raw, sizeof(raw))) {
            replacement = raw;
            replacement_length = strlen(raw);
        }
        else if (comma < inside_length) {
            replacement = inside + comma + 1;
            replacement_length = inside_length - comma - 1;
        }
        else return false;
        char resolved[512];
        if (!style_resolve_value(sheet, replacement, replacement_length, resolved,
                           sizeof(resolved), depth + 1)) return false;
        size_t resolved_length = strlen(resolved);
        if (written + resolved_length >= output_size) return false;
        memcpy(output + written, resolved, resolved_length);
        written += resolved_length;
        at = close + 1;
    }
    output[written] = '\0';
    return true;
}

static bool style_declaration_equal(const StyleDeclaration *stored,
                                    const ComputedStyle *values,
                                    uint64_t mask, uint64_t mask_high,
                                    uint64_t inherit_mask,
                                    const char *deferred,
                                    size_t deferred_length)
{
    if (stored == NULL || values == NULL || stored->mask != mask
        || stored->mask_high != mask_high
        || stored->inherit_mask != inherit_mask
        || stored->deferred_length != deferred_length
        || memcmp(&stored->values, values, sizeof(*values)) != 0) {
        return false;
    }
    return deferred_length == 0
        || (stored->deferred_declarations != NULL && deferred != NULL
            && memcmp(stored->deferred_declarations, deferred,
                      deferred_length) == 0);
}

/*
 * This hash only selects an interning-table probe; exact equality is always
 * verified before reuse.  Process native 32-bit words so the PSP does one
 * native multiply per four bytes instead of a synthesized 64-bit multiply
 * for every byte of the compact ComputedStyle.
 */
static uint32_t style_hash_bytes(uint32_t hash, const void *data,
                                 size_t length)
{
    const unsigned char *bytes = data;
    while (length >= sizeof(uint32_t)) {
        uint32_t word = 0;
        memcpy(&word, bytes, sizeof(word));
        hash ^= word;
        hash *= UINT32_C(16777619);
        bytes += sizeof(word);
        length -= sizeof(word);
    }
    while (length-- != 0) {
        hash ^= *bytes++;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static uint32_t style_declaration_hash(const ComputedStyle *values,
                                       uint64_t mask, uint64_t mask_high,
                                       uint64_t inherit_mask,
                                       const char *deferred,
                                       size_t deferred_length)
{
    uint32_t hash = UINT32_C(2166136261);
    hash = style_hash_bytes(hash, values, sizeof(*values));
    hash = style_hash_bytes(hash, &mask, sizeof(mask));
    hash = style_hash_bytes(hash, &mask_high, sizeof(mask_high));
    hash = style_hash_bytes(hash, &inherit_mask, sizeof(inherit_mask));
    hash = style_hash_bytes(hash, &deferred_length,
                            sizeof(deferred_length));
    if (deferred_length != 0) {
        hash = style_hash_bytes(hash, deferred, deferred_length);
    }
    return hash;
}

static bool style_declaration_index_insert_hashed(
    Stylesheet *sheet, uint32_t declaration_index, uint32_t hash)
{
    if (sheet == NULL || sheet->declaration_index_slots == NULL
        || sheet->declaration_index_slot_count == 0
        || declaration_index >= sheet->declaration_count) return false;
    size_t mask = sheet->declaration_index_slot_count - 1u;
    size_t slot = (size_t) hash & mask;
    for (size_t probes = 0; probes < sheet->declaration_index_slot_count;
         probes++, slot = (slot + 1u) & mask) {
        if (sheet->declaration_index_slots[slot] == UINT32_MAX) {
            sheet->declaration_index_slots[slot] = declaration_index;
            return true;
        }
    }
    return false;
}

static bool style_declaration_index_insert(Stylesheet *sheet,
                                           uint32_t declaration_index)
{
    if (sheet == NULL || declaration_index >= sheet->declaration_count) {
        return false;
    }
    const StyleDeclaration *declaration =
        &sheet->declarations[declaration_index];
    uint32_t hash = style_declaration_hash(
        &declaration->values, declaration->mask, declaration->mask_high,
        declaration->inherit_mask,
        declaration->deferred_declarations, declaration->deferred_length);
    return style_declaration_index_insert_hashed(
        sheet, declaration_index, hash);
}

static bool style_declaration_index_prepare(Stylesheet *sheet,
                                            size_t required_count)
{
    if (sheet == NULL || required_count > UINT32_MAX) return false;
    size_t slots = sheet->declaration_index_slot_count;
    if (slots != 0 && required_count <= slots / 2u) return true;
    size_t capacity = slots == 0 ? 64u : slots;
    while (required_count > capacity / 2u) {
        if (capacity > SIZE_MAX / 2u) return false;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*sheet->declaration_index_slots)) {
        return false;
    }
    uint32_t *index = budget_malloc(
        sheet->budget, capacity * sizeof(*index));
    if (index == NULL) return false;
    for (size_t i = 0; i < capacity; i++) index[i] = UINT32_MAX;
    uint32_t *previous = sheet->declaration_index_slots;
    size_t previous_count = sheet->declaration_index_slot_count;
    sheet->declaration_index_slots = index;
    sheet->declaration_index_slot_count = capacity;
    for (size_t i = 0; i < sheet->declaration_count; i++) {
        if (!style_declaration_index_insert(sheet, (uint32_t) i)) {
            sheet->declaration_index_slots = previous;
            sheet->declaration_index_slot_count = previous_count;
            budget_free(sheet->budget, index);
            return false;
        }
    }
    budget_free(sheet->budget, previous);
    return true;
}

static bool add_declaration(Stylesheet *sheet, const ComputedStyle *values,
                            uint64_t mask, uint64_t mask_high,
                            uint64_t inherit_mask,
                            const char *deferred, size_t deferred_length,
                            bool force_unique,
                            uint32_t *index)
{
    if (sheet == NULL || values == NULL || index == NULL
        || (deferred == NULL && deferred_length != 0)) return false;

    bool indexed = !force_unique && sheet->declaration_count != SIZE_MAX
        && style_declaration_index_prepare(
            sheet, sheet->declaration_count + 1u);
    uint32_t declaration_hash = 0;
    if (indexed) {
        declaration_hash = style_declaration_hash(
            values, mask, mask_high, inherit_mask, deferred,
            deferred_length);
        size_t table_mask = sheet->declaration_index_slot_count - 1u;
        size_t slot = (size_t) declaration_hash & table_mask;
        for (size_t probes = 0;
             probes < sheet->declaration_index_slot_count;
             probes++, slot = (slot + 1u) & table_mask) {
            uint32_t candidate = sheet->declaration_index_slots[slot];
            if (candidate == UINT32_MAX) break;
            if (candidate < sheet->declaration_count
                && style_declaration_equal(
                    &sheet->declarations[candidate], values, mask, mask_high,
                    inherit_mask, deferred, deferred_length)) {
                *index = candidate;
                return true;
            }
        }
    } else {
        /* Allocation pressure may disable the optional interning index. Keep
           the common comma-list case compact with a fixed, bounded scan. */
        size_t first = sheet->declaration_count > 4
                       ? sheet->declaration_count - 4 : 0;
        for (size_t i = sheet->declaration_count; i != first; i--) {
            if (style_declaration_equal(&sheet->declarations[i - 1], values,
                                        mask, mask_high, inherit_mask,
                                        deferred, deferred_length)) {
                *index = (uint32_t) (i - 1);
                return true;
            }
        }
    }
    if (sheet->declaration_count >= UINT32_MAX) return false;
    if (sheet->declaration_count == sheet->declaration_capacity) {
        size_t capacity = sheet->declaration_capacity == 0
                          ? 32 : sheet->declaration_capacity * 2;
        if (capacity < sheet->declaration_capacity
            || capacity > SIZE_MAX / sizeof(*sheet->declarations)) {
            return false;
        }
        StyleDeclaration *declarations = budget_realloc(
            sheet->budget, sheet->declarations,
            capacity * sizeof(*declarations));
        if (declarations == NULL) return false;
        sheet->declarations = declarations;
        sheet->declaration_capacity = capacity;
    }
    char *deferred_copy = NULL;
    if (deferred_length != 0) {
        if (deferred_length == SIZE_MAX) return false;
        deferred_copy = budget_malloc(sheet->budget, deferred_length + 1u);
        if (deferred_copy == NULL) return false;
        memcpy(deferred_copy, deferred, deferred_length);
        deferred_copy[deferred_length] = '\0';
    }
    uint32_t deferred_program_offset = UINT32_MAX;
    uint16_t deferred_program_count = 0;
    if (deferred_copy != NULL) {
        (void) style_compile_deferred_program(
            sheet, deferred_copy, deferred_length,
            &deferred_program_offset, &deferred_program_count);
    }
    size_t declaration_index = sheet->declaration_count++;
    sheet->declarations[declaration_index] = (StyleDeclaration) {
        .values = *values,
        .mask = mask,
        .mask_high = mask_high,
        .inherit_mask = inherit_mask,
        .deferred_declarations = deferred_copy,
        .deferred_length = deferred_length,
        .deferred_program_offset = deferred_program_offset,
        .deferred_program_count = deferred_program_count
    };
    sheet->deferred_bytes += deferred_length != 0
                             ? deferred_length + 1u : 0;
    *index = (uint32_t) declaration_index;
    if (indexed) {
        (void) style_declaration_index_insert_hashed(
            sheet, (uint32_t) declaration_index, declaration_hash);
    }
    return true;
}

static bool add_rule(Stylesheet *sheet, StyleRule rule)
{
    const StyleDeclaration *declaration = stylesheet_rule_declaration(
        sheet, &rule);
    if (declaration == NULL) return false;
    if ((declaration->mask & S_OPACITY) != 0
        && STYLE_TRACE(sheet, PAINT)) {
        fprintf(stderr, "style-opacity selector=%s value=%u important=%d "
                "origin=%u\n", rule.selector, declaration->values.opacity,
                rule.important, rule.origin);
    }
    if ((declaration->mask & S_COLOR) != 0
        && declaration->values.color_alpha == 0
        && STYLE_TRACE(sheet, PAINT)) {
        fprintf(stderr, "style-transparent-color selector=%s important=%d "
                "origin=%u\n", rule.selector, rule.important, rule.origin);
    }
    if (sheet->count == sheet->capacity) {
        size_t capacity = sheet->capacity == 0 ? 32 : sheet->capacity * 2;
        StyleRule *rules = budget_realloc(sheet->budget, sheet->rules,
                                          capacity * sizeof(*rules));
        if (rules == NULL) return false;
        sheet->rules = rules;
        sheet->capacity = capacity;
    }
    sheet->rules[sheet->count++] = rule;
    sheet->focus_rule_index_ready = false;
    return true;
}

#define STYLE_REVERT_RULE_MASK_LIMIT 64u

static bool add_revert_rule_mask(Stylesheet *sheet, unsigned order,
                                 uint64_t mask, uint64_t mask_high)
{
    if (mask == 0 && mask_high == 0) return true;
    if (sheet == NULL || sheet->budget == NULL
        || sheet->revert_rule_mask_count >= STYLE_REVERT_RULE_MASK_LIMIT) {
        return false;
    }
    if (sheet->revert_rule_mask_count == sheet->revert_rule_mask_capacity) {
        size_t capacity = sheet->revert_rule_mask_capacity == 0
            ? 4u : sheet->revert_rule_mask_capacity * 2u;
        if (capacity > STYLE_REVERT_RULE_MASK_LIMIT) {
            capacity = STYLE_REVERT_RULE_MASK_LIMIT;
        }
        StyleRevertRuleMask *masks = budget_realloc(
            sheet->budget, sheet->revert_rule_masks,
            capacity * sizeof(*masks));
        if (masks == NULL) return false;
        sheet->revert_rule_masks = masks;
        sheet->revert_rule_mask_capacity = capacity;
    }
    sheet->revert_rule_masks[sheet->revert_rule_mask_count++] =
        (StyleRevertRuleMask) {
            .mask = mask,
            .mask_high = mask_high,
            .order = order
        };
    return true;
}

void stylesheet_rule_revert_mask(const Stylesheet *sheet,
                                 const StyleRule *rule,
                                 uint64_t *mask, uint64_t *mask_high)
{
    if (mask != NULL) *mask = 0;
    if (mask_high != NULL) *mask_high = 0;
    if (sheet == NULL || rule == NULL || sheet->revert_rule_masks == NULL) {
        return;
    }
    size_t low = 0;
    size_t high = sheet->revert_rule_mask_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        const StyleRevertRuleMask *candidate =
            &sheet->revert_rule_masks[middle];
        if (candidate->order < rule->order) low = middle + 1u;
        else high = middle;
    }
    if (low >= sheet->revert_rule_mask_count
        || sheet->revert_rule_masks[low].order != rule->order) {
        return;
    }
    if (mask != NULL) *mask = sheet->revert_rule_masks[low].mask;
    if (mask_high != NULL) {
        *mask_high = sheet->revert_rule_masks[low].mask_high;
    }
}

bool stylesheet_prepare_focus_rule_index(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return false;
    if (sheet->focus_rule_index_ready) return true;
    size_t count = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        if (strstr(sheet->rules[i].selector, ":focus") != NULL) count++;
    }
    if (count == 0) {
        budget_free(sheet->budget, sheet->focus_rule_indices);
        sheet->focus_rule_indices = NULL;
        sheet->focus_rule_count = 0;
        sheet->focus_rule_index_ready = true;
        return true;
    }
    uint32_t *indices = budget_realloc(
        sheet->budget, sheet->focus_rule_indices, count * sizeof(*indices));
    if (indices == NULL) return false;
    sheet->focus_rule_indices = indices;
    sheet->focus_rule_count = 0;
    for (size_t i = 0; i < sheet->count; i++) {
        if (strstr(sheet->rules[i].selector, ":focus") == NULL) continue;
        sheet->focus_rule_indices[sheet->focus_rule_count++] = (uint32_t) i;
    }
    sheet->focus_rule_index_ready = true;
    return true;
}

static bool add_custom_rule(Stylesheet *sheet, const char *selector,
                            size_t selector_length, const char *name,
                            size_t name_length, const char *value,
                            size_t value_length, unsigned specificity,
                            unsigned order, bool important,
                            PseudoElement pseudo)
{
    if (selector_length >= sizeof(((StyleCustomRule *) 0)->selector)
        || name_length >= sizeof(((StyleCustomRule *) 0)->name)
        || value_length >= sizeof(((StyleCustomRule *) 0)->value)) {
        if (sheet->diagnostic_custom_property_drops != UINT64_MAX) {
            sheet->diagnostic_custom_property_drops++;
        }
        return true;
    }
    if (sheet->custom_rule_count == sheet->custom_rule_capacity) {
        size_t capacity = sheet->custom_rule_capacity == 0
                          ? 16 : sheet->custom_rule_capacity * 2;
        StyleCustomRule *rules = budget_realloc(
            sheet->budget, sheet->custom_rules, capacity * sizeof(*rules));
        if (rules == NULL) return false;
        sheet->custom_rules = rules;
        sheet->custom_rule_capacity = capacity;
    }
    StyleCustomRule *rule = &sheet->custom_rules[sheet->custom_rule_count++];
    memset(rule, 0, sizeof(*rule));
    memcpy(rule->selector, selector, selector_length);
    rule->selector[selector_length] = '\0';
    memcpy(rule->name, name, name_length);
    rule->name[name_length] = '\0';
    memcpy(rule->value, value, value_length);
    rule->value[value_length] = '\0';
    rule->important = important;
    rule->pseudo = (uint8_t) pseudo;
    rule->container_query = sheet->current_container_query;
    rule->fast_key_offset = UINT8_MAX;
    StyleRule selector_metadata = {
        .selector = rule->selector,
        .selector_length = (uint16_t) selector_length
    };
    selector_assign_fast_key(
        &selector_metadata, rule->selector, selector_length);
    if (selector_metadata.has_fast_key
        && selector_metadata.fast_key_offset < UINT8_MAX) {
        rule->fast_key_offset = (uint8_t) selector_metadata.fast_key_offset;
    }
    rule->origin = sheet->current_origin;
    rule->layer = sheet->current_layer;
    rule->specificity = specificity;
    rule->order = order;
    return true;
}

static const char *retained_presentation_property(
    const char *name, size_t name_length)
{
    /* NanoSVG applies the winding rule stored as fill-rule when a path is
       used by a clipping path. Canonicalizing CSS clip-rule here preserves
       the authored clipping result without adding a thirteenth retained
       presentation field to every bounded batch. */
    if (span_case_equal(name, name_length, "clip-rule")) {
        return "fill-rule";
    }
    int index = retained_presentation_index(name, name_length);
    return index < 0 ? NULL : retained_presentation_names[index];
}

static bool retained_presentation_number(const char *value,
                                         size_t value_length,
                                         double minimum, double maximum,
                                         bool allow_percent)
{
    char copy[64];
    if (value_length == 0 || value_length >= sizeof(copy)) return false;
    memcpy(copy, value, value_length);
    copy[value_length] = '\0';
    char *end = NULL;
    double parsed = strtod(copy, &end);
    if (end == copy) return false;
    if (allow_percent && *end == '%' && end[1] == '\0') {
        parsed /= 100.0;
        end++;
    }
    return *end == '\0' && parsed >= minimum && parsed <= maximum;
}

static bool retained_presentation_length(Stylesheet *sheet,
                                         const char *value,
                                         size_t value_length,
                                         bool negative_allowed)
{
    StyleLength parsed = 0;
    bool percent = false;
    size_t programs = sheet->math_program_count;
    size_t instructions = sheet->math_instruction_count;
    bool valid = parse_style_length(
        sheet, value, value_length, &parsed, &percent);
    style_math_restore(sheet, programs, instructions);
    return valid && (negative_allowed || parsed >= 0);
}

static bool retained_presentation_value_supported(
    Stylesheet *sheet, const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    trim(&value, &value_length);
    static const char *const css_wide[] = {
        "inherit", "initial", "unset", "revert", "revert-layer"
    };
    for (size_t i = 0; i < sizeof(css_wide) / sizeof(css_wide[0]); i++) {
        if (span_case_equal(value, value_length, css_wide[i])) return true;
    }
    if (span_equal(name, name_length, "fill")
        || span_equal(name, name_length, "stroke")) {
        if (span_case_equal(value, value_length, "none")
            || span_case_equal(value, value_length, "currentcolor")) {
            return true;
        }
        uint32_t color = 0;
        uint8_t alpha = 0;
        if (style_parse_color_with_alpha(
                sheet, value, value_length, &color, &alpha)) return true;
        /* NanoSVG resolves local paint servers after Tilefinch has applied
           the element cascade.  Keep the accepted grammar deliberately
           local: external paint-server URLs are not implemented. */
        return value_length > 6
            && strncasecmp(value, "url(#", 5) == 0
            && value[value_length - 1] == ')'
            && memchr(value + 5, ')', value_length - 6) == NULL;
    }
    if (span_equal(name, name_length, "fill-opacity")
        || span_equal(name, name_length, "stroke-opacity")
        || span_equal(name, name_length, "opacity")) {
        return retained_presentation_number(
            value, value_length, 0.0, 1.0, true);
    }
    if (span_equal(name, name_length, "fill-rule")
        || span_equal(name, name_length, "clip-rule")) {
        return span_case_equal(value, value_length, "nonzero")
            || span_case_equal(value, value_length, "evenodd");
    }
    if (span_equal(name, name_length, "stroke-linecap")) {
        return span_case_equal(value, value_length, "butt")
            || span_case_equal(value, value_length, "round")
            || span_case_equal(value, value_length, "square");
    }
    if (span_equal(name, name_length, "stroke-linejoin")) {
        return span_case_equal(value, value_length, "miter")
            || span_case_equal(value, value_length, "round")
            || span_case_equal(value, value_length, "bevel");
    }
    if (span_equal(name, name_length, "stroke-miterlimit")) {
        return retained_presentation_number(
            value, value_length, 1.0, 1000000.0, false);
    }
    if (span_equal(name, name_length, "stroke-width")) {
        return retained_presentation_length(
            sheet, value, value_length, false);
    }
    if (span_equal(name, name_length, "stroke-dashoffset")) {
        return retained_presentation_length(
            sheet, value, value_length, true);
    }
    if (span_equal(name, name_length, "stroke-dasharray")) {
        if (span_case_equal(value, value_length, "none")) return true;
        size_t count = 0;
        for (size_t at = 0; at < value_length;) {
            while (at < value_length
                   && (isspace((unsigned char) value[at])
                       || value[at] == ',')) at++;
            size_t start = at;
            while (at < value_length
                   && !isspace((unsigned char) value[at])
                   && value[at] != ',') at++;
            if (start == at
                || ++count > 32
                || !retained_presentation_length(
                    sheet, value + start, at - start, false)) {
                return false;
            }
        }
        return count != 0;
    }
    return false;
}

static bool selector_has_direct_never_match_pseudo(
    const char *selector, size_t length)
{
    unsigned round = 0;
    unsigned square = 0;
    char quote = '\0';
    for (size_t at = 0; at < length; at++) {
        char value = selector[at];
        if (quote != '\0') {
            if (value == '\\' && at + 1u < length) at++;
            else if (value == quote) quote = '\0';
            continue;
        }
        if (square != 0 && (value == '\'' || value == '"')) {
            quote = value;
        } else if (value == '[') {
            square++;
        } else if (value == ']' && square != 0) {
            square--;
        } else if (square == 0 && value == '(') {
            round++;
        } else if (square == 0 && value == ')' && round != 0) {
            round--;
        } else if (square == 0 && round == 0 && value == ':'
                   && at + 1u < length && selector[at + 1u] != ':') {
            size_t name = at + 1u;
            size_t end = skip_selector_identifier(selector, length, name);
            size_t name_length = end - name;
            if (span_case_equal(selector + name, name_length, "hover")
                || span_case_equal(
                    selector + name, name_length, "active")
                || span_case_equal(
                    selector + name, name_length, "visited")
                || span_case_equal(
                    selector + name, name_length, "lang")) {
                return true;
            }
            if (end > at) at = end - 1u;
        }
    }
    return false;
}

static bool collect_custom_rules(Stylesheet *sheet, const char *selector,
                                 size_t selector_length,
                                 const char *declarations, size_t length,
                                 unsigned specificity, unsigned order,
                                 PseudoElement pseudo)
{
    /* These interaction states and :lang() are currently unsatisfied by the
       static matcher. Do not retain a second sparse copy of a rule that can
       never win; functional uses such as :not(:hover) remain eligible. */
    if (selector_has_direct_never_match_pseudo(
            selector, selector_length)) return true;
    static const char *const retained_layout_properties[] = {
        "container", "container-name", "container-type",
        "columns", "column-count", "column-width", "column-gap",
        "break-inside",
        "cursor",
        "overscroll-behavior", "overscroll-behavior-x",
        "overscroll-behavior-y", "overscroll-behavior-inline",
        "overscroll-behavior-block",
        "scroll-behavior",
        "scroll-margin", "scroll-margin-top", "scroll-margin-right",
        "scroll-margin-bottom", "scroll-margin-left",
        "scroll-padding", "scroll-padding-top", "scroll-padding-right",
        "scroll-padding-bottom", "scroll-padding-left",
        "scroll-snap-align", "scroll-snap-stop", "scroll-snap-type",
        "scrollbar-color", "scrollbar-width",
        "user-select", "-webkit-user-select", "touch-action",
        "text-size-adjust", "-webkit-text-size-adjust", "resize",
        "text-wrap", "text-wrap-style",
        "hyphens", "tab-size", "font-kerning", "text-rendering",
        "contain-intrinsic-size",
        "translate", "rotate", "scale", "isolation", "mix-blend-mode",
        "backdrop-filter", "-webkit-backdrop-filter",
        "border-start-start-radius", "border-start-end-radius",
        "border-end-start-radius", "border-end-end-radius"
    };
    unsigned declaration_order = 0;
    for (size_t at = 0; at < length; declaration_order++) {
        /* Comments are whitespace in a declaration list. */
        while (at < length) {
            while (at < length
                   && isspace((unsigned char) declarations[at])) at++;
            if (at + 1 >= length || declarations[at] != '/'
                || declarations[at + 1] != '*') break;
            at += 2;
            while (at + 1 < length
                   && !(declarations[at] == '*'
                        && declarations[at + 1] == '/')) at++;
            if (at + 1 >= length) return true;
            at += 2;
        }
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (declarations[end] == '(') parentheses++;
            else if (declarations[end] == ')' && parentheses > 0) {
                parentheses--;
            }
            if (declarations[end] == ';' && parentheses == 0) break;
            end++;
        }
        size_t colon = at;
        while (colon < end && declarations[colon] != ':') colon++;
        if (colon < end) {
            const char *name = declarations + at;
            size_t name_length = colon - at;
            const char *value = declarations + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            bool retained = name_length > 2
                && name[0] == '-' && name[1] == '-';
            const char *stored_name = name;
            size_t stored_name_length = name_length;
            if (!retained) {
                const char *canonical = retained_presentation_property(
                    name, name_length);
                if (canonical != NULL) {
                    retained = true;
                    stored_name = canonical;
                    stored_name_length = strlen(canonical);
                }
            }
            if (!retained) {
                if (span_case_equal(name, name_length,
                                    "-webkit-user-select")) {
                    retained = true;
                    stored_name = "user-select";
                    stored_name_length = sizeof("user-select") - 1u;
                } else if (span_case_equal(
                               name, name_length,
                               "-webkit-text-size-adjust")) {
                    retained = true;
                    stored_name = "text-size-adjust";
                    stored_name_length = sizeof("text-size-adjust") - 1u;
                }
            }
            if (!retained) {
                for (size_t i = 0;
                     i < sizeof(retained_layout_properties)
                         / sizeof(retained_layout_properties[0]); i++) {
                    const char *candidate = retained_layout_properties[i];
                    if (span_case_equal(name, name_length, candidate)) {
                        retained = true;
                        stored_name = candidate;
                        stored_name_length = strlen(candidate);
                        break;
                    }
                }
            }
            if (retained
                && (span_case_equal(stored_name, stored_name_length,
                                    "columns")
                    || span_case_equal(stored_name, stored_name_length,
                                       "column-count")
                    || span_case_equal(stored_name, stored_name_length,
                                       "column-width"))) {
                sheet->has_multicolumn_rules = true;
            }
            if (retained
                && span_case_equal(
                    stored_name, stored_name_length, "cursor")) {
                sheet->has_cursor_rules = true;
            }
            if (retained
                && ((stored_name_length >= 6
                     && strncasecmp(stored_name, "scroll", 6) == 0)
                    || (stored_name_length >= 11
                        && strncasecmp(
                               stored_name, "overscroll-", 11) == 0))) {
                sheet->has_scroll_interaction_rules = true;
            }
            if (retained) {
                uint16_t modern = 0;
                if (span_case_equal(stored_name, stored_name_length,
                                    "user-select")
                    || span_case_equal(stored_name, stored_name_length,
                                       "-webkit-user-select")) {
                    modern = STYLE_MODERN_USER_SELECT;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "touch-action")) {
                    modern = STYLE_MODERN_TOUCH_ACTION;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "text-size-adjust")
                           || span_case_equal(
                                  stored_name, stored_name_length,
                                  "-webkit-text-size-adjust")) {
                    modern = STYLE_MODERN_TEXT_SIZE_ADJUST;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "resize")) {
                    modern = STYLE_MODERN_RESIZE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "text-wrap")
                           || span_case_equal(stored_name, stored_name_length,
                                              "text-wrap-style")) {
                    modern = STYLE_MODERN_TEXT_WRAP;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "translate")) {
                    modern = STYLE_MODERN_TRANSLATE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "rotate")) {
                    modern = STYLE_MODERN_ROTATE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "scale")) {
                    modern = STYLE_MODERN_SCALE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "isolation")) {
                    modern = STYLE_MODERN_ISOLATION;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "hyphens")
                           || span_case_equal(stored_name, stored_name_length,
                                              "tab-size")
                           || span_case_equal(stored_name, stored_name_length,
                                              "font-kerning")
                           || span_case_equal(stored_name, stored_name_length,
                                              "text-rendering")) {
                    modern = STYLE_MODERN_TYPOGRAPHY;
                    if (span_case_equal(stored_name, stored_name_length,
                                        "hyphens")) {
                        sheet->modern_typography_mask
                            |= STYLE_MODERN_TYPOGRAPHY_HYPHENS;
                    } else if (span_case_equal(
                                   stored_name, stored_name_length,
                                   "tab-size")) {
                        sheet->modern_typography_mask
                            |= STYLE_MODERN_TYPOGRAPHY_TAB_SIZE;
                    } else if (span_case_equal(
                                   stored_name, stored_name_length,
                                   "font-kerning")) {
                        sheet->modern_typography_mask
                            |= STYLE_MODERN_TYPOGRAPHY_KERNING;
                    } else {
                        sheet->modern_typography_mask
                            |= STYLE_MODERN_TYPOGRAPHY_TEXT_RENDERING;
                    }
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "mix-blend-mode")) {
                    modern = STYLE_MODERN_MIX_BLEND;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "backdrop-filter")
                           || span_case_equal(stored_name, stored_name_length,
                                              "-webkit-backdrop-filter")) {
                    modern = STYLE_MODERN_BACKDROP_FILTER;
                } else if (stored_name_length >= 18
                           && strncasecmp(stored_name, "border-", 7) == 0
                           && strncasecmp(
                                  stored_name + stored_name_length - 7,
                                  "-radius", 7) == 0) {
                    modern = STYLE_MODERN_LOGICAL_RADIUS;
                }
                sheet->modern_property_mask |= modern;
            }
            if (retained
                && !add_custom_rule(sheet, selector, selector_length,
                                    stored_name, stored_name_length,
                                    value, value_length,
                                    specificity,
                                    (order <= (UINT_MAX >> 8)
                                         ? order << 8
                                         : UINT_MAX - UINT8_MAX)
                                        | (declaration_order > UINT8_MAX
                                               ? UINT8_MAX
                                               : declaration_order),
                                    important, pseudo)) {
                return false;
            }
        }
        at = end + (end < length);
    }
    return true;
}

static bool value_has_font_math_function(const char *text, size_t length)
{
    static const char *const names[] = {"calc", "min", "max", "clamp"};
    for (size_t at = 0; at < length; at++) {
        if (at != 0) {
            unsigned char previous = (unsigned char) text[at - 1];
            if (isalnum(previous) || previous == '-' || previous == '_') {
                continue;
            }
        }
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            size_t name_length = strlen(names[i]);
            if (at + name_length >= length
                || text[at + name_length] != '(') continue;
            bool equal = true;
            for (size_t j = 0; j < name_length; j++) {
                if (tolower((unsigned char) text[at + j])
                    != (unsigned char) names[i][j]) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }
    }
    return false;
}

typedef struct {
    bool deferred[2];
    bool font_family[2];
    bool font_relative[2];
    bool non_font_relative[2];
    bool logical[2];
    bool dynamic[2];
    bool ch_relative[2];
    bool container_relative[2];
} StyleDeferredDeclarationAnalysis;

static void declarations_analyze_deferred_resolution(
    const char *text, size_t length,
    StyleDeferredDeclarationAnalysis *analysis)
{
    if (text == NULL || analysis == NULL) return;
    for (size_t at = 0; at < length;) {
        size_t end = at;
        int parentheses = 0;
        while (end < length) {
            if (text[end] == '(') parentheses++;
            else if (text[end] == ')' && parentheses > 0) parentheses--;
            if (text[end] == ';' && parentheses == 0) break;
            end++;
        }
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            const char *value = text + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&name, &name_length);
            trim(&value, &value_length);
            bool important = declaration_is_important(value, value_length);
            size_t slot = important ? 1u : 0u;
            if (!(name_length > 2 && name[0] == '-' && name[1] == '-')) {
                if (span_case_equal(name, name_length, "font")
                    || span_case_equal(name, name_length, "font-family")) {
                    analysis->font_family[slot] = true;
                }
                bool logical_property =
                    style_property_name_is_logical(name, name_length);
                if (logical_property) {
                    analysis->deferred[slot] = true;
                    analysis->logical[slot] = true;
                }
                if ((span_case_equal(name, name_length, "font-size")
                     || span_case_equal(name, name_length, "font"))
                    && value_has_font_math_function(value, value_length)) {
                    analysis->deferred[slot] = true;
                    analysis->font_relative[slot] = true;
                    analysis->dynamic[slot] = true;
                }
                for (size_t i = colon + 1; i + 4 <= end; i++) {
                    if (memcmp(text + i, "var(", 4) == 0) {
                        analysis->deferred[slot] = true;
                        analysis->dynamic[slot] = true;
                        break;
                    }
                }
                /* Font-relative used lengths cannot be folded while a rule
                   is compiled: the element's inherited/computed font is not
                   known until the rule is applied. Retain only declaration
                   blocks that actually contain an em/rem token; the compact
                   deferred program reparses those properties against the
                   per-node font basis. */
                for (size_t i = colon + 1; i + 2 <= end; i++) {
                    size_t unit_length = 0;
                    if (i + 3 <= end
                        && tolower((unsigned char) text[i]) == 'r'
                        && tolower((unsigned char) text[i + 1]) == 'e'
                        && tolower((unsigned char) text[i + 2]) == 'm') {
                        unit_length = 3;
                    } else if (tolower((unsigned char) text[i]) == 'e'
                               && tolower((unsigned char) text[i + 1])
                                  == 'm') {
                        unit_length = 2;
                    } else if (tolower((unsigned char) text[i]) == 'c'
                               && tolower((unsigned char) text[i + 1])
                                  == 'h') {
                        unit_length = 2;
                        analysis->ch_relative[slot] = true;
                    }
                    if (unit_length == 0 || i == colon + 1) continue;
                    unsigned char before = (unsigned char) text[i - 1];
                    unsigned char after = i + unit_length < end
                        ? (unsigned char) text[i + unit_length] : 0;
                    bool value_before = isdigit(before) || before == '.'
                                        || before == ')';
                    bool identifier_after = isalnum(after) || after == '-'
                                            || after == '_'
                                            || after >= 0x80u;
                    if (value_before && !identifier_after) {
                        analysis->deferred[slot] = true;
                        analysis->font_relative[slot] = true;
                        analysis->dynamic[slot] = true;
                        if (!span_case_equal(name, name_length, "font-size")
                            && !span_case_equal(name, name_length, "font")) {
                            analysis->non_font_relative[slot] = true;
                        }
                        break;
                    }
                }
                /* Container-relative units depend on the nearest measured
                   query container, so stylesheet-build-time viewport
                   folding would freeze one value for every matching node.
                   Reuse the compact per-node deferred program. */
                static const char *const container_units[] = {
                    "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax"
                };
                for (size_t i = colon + 1; i < end; i++) {
                    for (size_t unit = 0;
                         unit < sizeof(container_units)
                                / sizeof(container_units[0]); unit++) {
                        size_t unit_length = strlen(container_units[unit]);
                        if (i + unit_length > end
                            || strncasecmp(
                                text + i, container_units[unit],
                                unit_length) != 0
                            || i == colon + 1) continue;
                        unsigned char before =
                            (unsigned char) text[i - 1];
                        unsigned char after = i + unit_length < end
                            ? (unsigned char) text[i + unit_length] : 0;
                        if ((isdigit(before) || before == '.'
                             || before == ')')
                            && !(isalnum(after) || after == '-'
                                 || after == '_' || after >= 0x80u)) {
                            analysis->deferred[slot] = true;
                            analysis->non_font_relative[slot] = true;
                            analysis->dynamic[slot] = true;
                            analysis->container_relative[slot] = true;
                            i = end;
                            break;
                        }
                    }
                }
            }
        }
        at = end + (end < length);
    }
}
