static uint64_t stylesheet_next_generation(void)
{
    stylesheet_generation_sequence++;
    if (stylesheet_generation_sequence == 0) stylesheet_generation_sequence++;
    return stylesheet_generation_sequence;
}

/* StyleVariableCacheEntry.found. */
enum {
    VARIABLE_CACHE_MISS = 0,
    VARIABLE_CACHE_INLINE = 1,
    VARIABLE_CACHE_SPILLED = 2
};

typedef struct {
    lxb_dom_node_t *node;
    uint32_t hash;
    uint32_t stamp;
    uint16_t name_length;
    uint8_t occupied;
    uint8_t found;
    /* The custom properties resolving it read (itself, and those its
       declared value referenced): a hit reports them as read too. */
    uint64_t reads;
    char name[STYLE_CUSTOM_NAME_CAPACITY];
    union {
        char text[STYLE_CUSTOM_SHORT_VALUE_CAPACITY];
        /* A longer value's NUL-terminated bytes in the cache's spill. */
        struct {
            uint32_t offset;
            uint32_t length;
        } spill;
    } value;
} StyleVariableCacheEntry;

typedef struct StyleVariableCache {
    Budget *budget;
    StyleVariableCacheEntry *entries;
    size_t capacity;
    /* STYLE_VARIABLE_CACHE_SPILL_BYTES, allocated by the first long value.
       Append-only until the table is emptied: an evicted entry's bytes are
       only reclaimed then. */
    char *spill;
    uint32_t spill_used;
    uint32_t clock;
    /* An entry was stored since the table was last emptied. */
    bool stored;
    bool spill_refused;
} StyleVariableCache;

static size_t variable_cache_footprint(const StyleVariableCache *cache)
{
    return cache == NULL ? 0
        : sizeof(*cache) + cache->capacity * sizeof(*cache->entries)
          + (cache->spill == NULL ? 0 : STYLE_VARIABLE_CACHE_SPILL_BYTES);
}

/* Empties the table, its spill included. */
static void variable_cache_empty(StyleVariableCache *cache)
{
    memset(cache->entries, 0, cache->capacity * sizeof(*cache->entries));
    cache->spill_used = 0;
    cache->spill_refused = false;
}

static size_t variable_cache_capacity(size_t capacity)
{
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

static bool variable_cache_disabled(void)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_VARIABLE_CACHE") != NULL) return true;
#endif
    return false;
}

/* Whether any rule declares a custom property: without one a var() lookup
   can only find inline values or fall back, and a cache would not pay. */
static bool stylesheet_declares_custom_properties(const Stylesheet *sheet)
{
    bool custom_properties = sheet->variable_count != 0;
    for (size_t i = 0; !custom_properties
                       && i < sheet->custom_rule_count; i++) {
        const char *name = sheet->custom_rules[i].name;
        custom_properties = name[0] == '-' && name[1] == '-';
    }
    return custom_properties;
}

static StyleVariableCache *variable_cache_create(Budget *budget,
                                                 size_t capacity)
{
    capacity = variable_cache_capacity(capacity);
    StyleVariableCache *cache = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, 1, sizeof(*cache));
    if (cache == NULL) return NULL;
    cache->entries = budget_calloc_category(
        budget, BUDGET_CATEGORY_LAYOUT, capacity, sizeof(*cache->entries));
    if (cache->entries == NULL) {
        budget_free(budget, cache);
        return NULL;
    }
    cache->budget = budget;
    cache->capacity = capacity;
    return cache;
}

static void variable_cache_destroy(StyleVariableCache *cache)
{
    if (cache == NULL) return;
    Budget *budget = cache->budget;
    budget_free(budget, cache->spill);
    budget_free(budget, cache->entries);
    budget_free(budget, cache);
}

bool style_variable_cache_begin(Stylesheet *sheet, Budget *budget)
{
    if (sheet == NULL || budget == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache != NULL
        || sheet->resolve_scratch->variable_cache_lease != NULL) {
        return false;
    }
    if (!stylesheet_declares_custom_properties(sheet)
        || variable_cache_disabled()) return false;
    StyleVariableCache *cache = variable_cache_create(
        budget, STYLE_VARIABLE_CACHE_DEFAULT_ENTRIES);
    if (cache == NULL) return false;
    sheet->resolve_scratch->variable_cache = cache;
    size_t bytes = variable_cache_footprint(cache);
    if (bytes > sheet->variable_cache_peak_bytes) {
        sheet->variable_cache_peak_bytes = bytes;
    }
    return true;
}

void style_variable_cache_end(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return;
    /* A lent cache belongs to its lease owner, which frees it. */
    if (sheet->resolve_scratch->variable_cache_lease != NULL) {
        sheet->resolve_scratch->variable_cache_lease = NULL;
        sheet->resolve_scratch->variable_cache = NULL;
        return;
    }
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    sheet->resolve_scratch->variable_cache = NULL;
    variable_cache_destroy(cache);
}

void style_variable_cache_lease_release(StyleVariableCacheLease *lease)
{
    if (lease == NULL) return;
    variable_cache_destroy(lease->cache);
    lease->cache = NULL;
    lease->refused = false;
    lease->closed_generation = 0;
}

void style_variable_cache_lease_clear(StyleVariableCacheLease *lease)
{
    StyleVariableCache *cache = lease == NULL ? NULL : lease->cache;
    /* An empty table stays empty: mutations between reads cost nothing. */
    if (cache == NULL || !cache->stored) return;
    variable_cache_empty(cache);
    cache->clock = 0;
    cache->stored = false;
    lease->clears++;
}

size_t style_variable_cache_lease_bytes(const StyleVariableCacheLease *lease)
{
    return variable_cache_footprint(lease == NULL ? NULL : lease->cache);
}

bool style_variable_cache_attach(Stylesheet *sheet,
                                 StyleVariableCacheLease *lease)
{
    if (sheet == NULL || lease == NULL || lease->budget == NULL
        || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache != NULL
        || sheet->resolve_scratch->variable_cache_lease != NULL) return false;
    sheet->resolve_scratch->variable_cache_lease = lease;
    sheet->resolve_scratch->variable_cache = lease->cache;
    return true;
}

void style_variable_cache_detach(Stylesheet *sheet,
                                 StyleVariableCacheLease *lease)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache_lease != lease) return;
    sheet->resolve_scratch->variable_cache_lease = NULL;
    sheet->resolve_scratch->variable_cache = NULL;
}

/* The attached lease's cache, created now: a var() lookup is the first
   point that proves a resolution needs one. Sheets without custom
   properties are remembered per build so their lookups skip the check. */
static StyleVariableCache *variable_cache_from_lease(Stylesheet *sheet)
{
    StyleVariableCacheLease *lease =
        sheet->resolve_scratch->variable_cache_lease;
    if (lease == NULL) return NULL;
    if (lease->cache == NULL) {
        if (lease->refused
            || lease->closed_generation == sheet->build_generation)
            return NULL;
        if (!stylesheet_declares_custom_properties(sheet)
            || variable_cache_disabled()) {
            lease->closed_generation = sheet->build_generation;
            return NULL;
        }
        lease->cache = variable_cache_create(
            lease->budget, STYLE_VARIABLE_CACHE_LEASE_ENTRIES);
        lease->refused = lease->cache == NULL;
        if (lease->cache == NULL) return NULL;
        lease->creations++;
    }
    sheet->resolve_scratch->variable_cache = lease->cache;
    return lease->cache;
}

void style_variable_cache_invalidate_node(
    Stylesheet *sheet, const lxb_dom_node_t *node)
{
    /* Focus classification temporarily edits a live attribute even inside a
       layout build. Exact selector answers must not cross either edit. */
    if (sheet != NULL && sheet->resolve_scratch != NULL
        && sheet->resolve_scratch->ancestor_bloom_cache != NULL) {
        StyleAncestorBloomCache *cache = sheet->resolve_scratch->ancestor_bloom_cache;
        memset(cache->entries, 0, sizeof(cache->entries));
        memset(cache->matched_ranges, 0, sizeof(cache->matched_ranges));
        if (cache->results != NULL) memset(cache->results, 0,
            STYLE_SELECTOR_RESULT_CACHE_CAPACITY * sizeof(*cache->results));
    }
    if (sheet == NULL || node == NULL || sheet->resolve_scratch == NULL
        || sheet->resolve_scratch->variable_cache == NULL) return;
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    /*
     * Open-addressed lookup stops at the first empty entry, so deleting one
     * slot would make later colliding entries unreachable. Clearing the
     * bounded table is both correct and rare: this is used only while
     * comparing the one active focus marker during a layout snapshot.
     */
    variable_cache_empty(cache);
}

size_t style_variable_cache_bytes(const Stylesheet *sheet)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL) return 0;
    return variable_cache_footprint(sheet->resolve_scratch->variable_cache);
}

bool style_selector_cooperation_begin(
    Stylesheet *sheet, StyleSelectorCooperate cooperate, void *opaque,
    StyleAncestorBloomCache *ancestor_cache)
{
    if (sheet == NULL || cooperate == NULL
        || sheet->selector_cooperate != NULL) return false;
    sheet->selector_cooperate = cooperate;
    sheet->selector_cooperate_opaque = opaque;
    sheet->selector_cooperate_visits = 0;
    /* A large rule set can spend >100ms in 8192 ancestor visits on Allegrex.
       Input is serviced by the owner at these allocation-free safe points. */
    sheet->selector_cooperate_quota = 1024u;
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
    if (sheet->resolve_scratch != NULL) {
        if (ancestor_cache != NULL) memset(ancestor_cache, 0, sizeof(*ancestor_cache));
#ifndef TILEFINCH_NO_TRACE
        if (getenv("TILEFINCH_DISABLE_ANCESTOR_CACHE") != NULL) ancestor_cache = NULL;
#endif
        sheet->resolve_scratch->ancestor_bloom_cache = ancestor_cache;
        if (ancestor_cache != NULL && sheet->count >= 64u) {
            ancestor_cache->results = budget_calloc_category(
                sheet->budget, BUDGET_CATEGORY_LAYOUT,
                STYLE_SELECTOR_RESULT_CACHE_CAPACITY,
                sizeof(*ancestor_cache->results));
        }
    }
    return true;
}

void style_selector_cooperation_end(Stylesheet *sheet)
{
    if (sheet == NULL) return;
    if (sheet->resolve_scratch != NULL) {
        StyleAncestorBloomCache *cache = sheet->resolve_scratch->ancestor_bloom_cache;
        if (cache != NULL) {
            budget_free(sheet->budget, cache->results);
            cache->results = NULL;
        }
        sheet->resolve_scratch->ancestor_bloom_cache = NULL;
    }
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

/* Reserves length + 1 bytes in the sheet's append-only text arena for the
   caller to fill. Callers bound `length`; stored text never moves before
   destruction. */
static char *stylesheet_reserve_text(Stylesheet *sheet, size_t length)
{
    if (sheet == NULL || sheet->budget == NULL
        || length >= STYLE_SELECTOR_CHUNK_MAX_BYTES) return NULL;
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
    chunk->used += required;
    sheet->selector_bytes += required;
    return stored;
}

/* Copies text (NUL-terminated) into the sheet's text arena. */
static const char *stylesheet_store_text(Stylesheet *sheet,
                                         const char *text, size_t length)
{
    if (text == NULL) return NULL;
    char *stored = stylesheet_reserve_text(sheet, length);
    if (stored == NULL) return NULL;
    memcpy(stored, text, length);
    stored[length] = '\0';
    return stored;
}

/* Whether a custom-property value of `length` bytes may be stored: within
   STYLE_CUSTOM_VALUE_CAPACITY and, when long, within what is left of the
   sheet's STYLE_CUSTOM_LONG_VALUE_BYTES. Charges an admitted long value. */
static bool stylesheet_admit_custom_value(Stylesheet *sheet, size_t length)
{
    if (length >= STYLE_CUSTOM_VALUE_CAPACITY) return false;
    if (length < STYLE_CUSTOM_SHORT_VALUE_CAPACITY) return true;
    if (length + 1u > STYLE_CUSTOM_LONG_VALUE_BYTES
                      - sheet->custom_value_long_bytes) return false;
    sheet->custom_value_long_bytes += length + 1u;
    return true;
}

static const char *stylesheet_store_selector(Stylesheet *sheet,
                                             const char *text,
                                             size_t length)
{
    if (length >= STYLE_SELECTOR_CAPACITY) return NULL;
    return stylesheet_store_text(sheet, text, length);
}


/* The winning declaration of `name` in the element's style attribute, as
   a span of the attribute text (stable while the DOM is). A value of
   `limit` bytes or more is ignored, as an oversized rule value is. */
static bool inline_variable_value(const Stylesheet *sheet,
                                  lxb_dom_node_t *node, const char *name,
                                  size_t name_length, size_t limit,
                                  const char **value_output,
                                  size_t *length_output,
                                  bool *important_output)
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
                && value_length < limit
                && (!found || important || !found_important)) {
                *value_output = value;
                *length_output = value_length;
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

static bool custom_rule_later(const Stylesheet *sheet, const lxb_dom_node_t *node,
                              const StyleCustomRule *candidate,
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
    if (!sheet->adopted_order_varies)
        return candidate->order >= current->order;
    uint64_t a = style_adopted_cascade_order(sheet, node, candidate->order >> 8);
    uint64_t b = style_adopted_cascade_order(sheet, node, current->order >> 8);
    return a != b ? a > b : candidate->order >= current->order;
}

struct StyleCustomRuleIndexEntry {
    uint32_t hash;
    /* The custom rule's index; STYLE_CUSTOM_INDEX_SCOPED marks a rule of a
       sheet only shadow roots adopt. Within one hash those sort after the
       others, so a lookup for an element outside every adopting carrier
       stops at the first. */
    uint32_t index;
};
#define STYLE_CUSTOM_INDEX_SCOPED UINT32_C(0x80000000)

static uint32_t custom_rule_name_hash(const char *text, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) text[i];
        hash *= UINT32_C(16777619);
    }
    return hash == 0 ? UINT32_C(1) : hash;
}

/* The @property registration of `name`, or NULL. */
static const StyleRegisteredProperty *stylesheet_registered_property(
    const Stylesheet *sheet, const char *name, size_t name_length,
    uint32_t hash)
{
    size_t begin = 0, end = sheet->registered_property_count;
    while (begin < end) {
        size_t middle = begin + (end - begin) / 2u;
        if (sheet->registered_properties[middle].hash < hash)
            begin = middle + 1u;
        else end = middle;
    }
    for (size_t i = begin; i < sheet->registered_property_count
                           && sheet->registered_properties[i].hash == hash;
         i++) {
        const StyleRegisteredProperty *property =
            &sheet->registered_properties[i];
        if (property->name_length == name_length
            && memcmp(property->name, name, name_length) == 0)
            return property;
    }
    return NULL;
}

/* Records an @property rule: `name` is its prelude, `block` its
   descriptors. A rule without syntax or inherits, or with a syntax other
   than "*" and no initial-value, is invalid and ignored; a later valid
   rule for the same name replaces an earlier one. False only when memory
   is refused. */
static bool stylesheet_register_property(Stylesheet *sheet,
                                         const char *name,
                                         size_t name_length,
                                         const char *block,
                                         size_t block_length)
{
    trim(&name, &name_length);
    if (name_length <= 2 || name_length >= STYLE_CUSTOM_NAME_CAPACITY
        || name[0] != '-' || name[1] != '-') return true;
    for (size_t i = 0; i < name_length; i++) {
        if (isspace((unsigned char) name[i])) return true;
    }
    bool has_syntax = false, universal = false, has_inherits = false;
    bool inherits = false;
    const char *initial = NULL;
    size_t initial_length = 0;
    for (size_t at = 0; at < block_length;) {
        at = skip_css_space_and_comments(block, block_length, at);
        if (at >= block_length) break;
        size_t end = find_declaration_end(block, block_length, at);
        size_t colon = at;
        while (colon < end && block[colon] != ':') colon++;
        if (colon < end) {
            const char *descriptor = block + at;
            size_t descriptor_length = colon - at;
            const char *value = block + colon + 1;
            size_t value_length = end - colon - 1;
            trim(&descriptor, &descriptor_length);
            trim(&value, &value_length);
            if (span_case_equal(descriptor, descriptor_length, "syntax")
                && value_length >= 2
                && (value[0] == '"' || value[0] == '\'')
                && value[value_length - 1] == value[0]) {
                const char *syntax = value + 1;
                size_t syntax_length = value_length - 2;
                trim(&syntax, &syntax_length);
                has_syntax = true;
                universal = syntax_length == 1 && syntax[0] == '*';
            } else if (span_case_equal(descriptor, descriptor_length,
                                       "inherits")) {
                has_inherits = span_case_equal(value, value_length, "true")
                    || span_case_equal(value, value_length, "false");
                inherits = span_case_equal(value, value_length, "true");
            } else if (span_case_equal(descriptor, descriptor_length,
                                       "initial-value")) {
                initial = value;
                initial_length = value_length;
            }
        }
        at = end + (end < block_length);
    }
    if (!has_syntax || !has_inherits || (!universal && initial == NULL))
        return true;
    uint32_t hash = custom_rule_name_hash(name, name_length);
    StyleRegisteredProperty *existing = (StyleRegisteredProperty *)
        stylesheet_registered_property(sheet, name, name_length, hash);
    if (existing == NULL && sheet->registered_property_count
                            >= STYLE_REGISTERED_PROPERTY_LIMIT) return true;
    if (initial != NULL
        && !stylesheet_admit_custom_value(sheet, initial_length)) {
        if (sheet->diagnostic_custom_property_drops != UINT64_MAX) {
            sheet->diagnostic_custom_property_drops++;
        }
        return true;
    }
    if (existing == NULL
        && sheet->registered_property_count
           == sheet->registered_property_capacity) {
        size_t capacity = sheet->registered_property_capacity == 0
            ? 16u : sheet->registered_property_capacity * 2u;
        StyleRegisteredProperty *properties = budget_realloc(
            sheet->budget, sheet->registered_properties,
            capacity * sizeof(*properties));
        if (properties == NULL) return false;
        sheet->registered_properties = properties;
        sheet->registered_property_capacity = capacity;
    }
    /* One arena string holds "name\0initial\0". */
    size_t text_length = name_length + 1u
        + (initial == NULL ? 0 : initial_length);
    char *text = stylesheet_reserve_text(sheet, text_length);
    if (text == NULL) return false;
    memcpy(text, name, name_length);
    text[name_length] = '\0';
    if (initial != NULL) memcpy(text + name_length + 1u, initial,
                                initial_length);
    text[text_length] = '\0';
    StyleRegisteredProperty property = {
        .name = text,
        .initial_value = initial == NULL ? NULL : text + name_length + 1u,
        .hash = hash,
        .initial_length = (uint16_t) initial_length,
        .name_length = (uint8_t) name_length,
        .inherits = inherits
    };
    if (existing != NULL) {
        *existing = property;
        return true;
    }
    size_t at = sheet->registered_property_count;
    while (at != 0 && sheet->registered_properties[at - 1u].hash > hash) at--;
    memmove(&sheet->registered_properties[at + 1u],
            &sheet->registered_properties[at],
            (sheet->registered_property_count - at)
                * sizeof(*sheet->registered_properties));
    sheet->registered_properties[at] = property;
    sheet->registered_property_count++;
    return true;
}

/* A registered property's initial value into `output`: false when it has
   none (the guaranteed-invalid value) or it does not fit. */
static bool registered_initial_value(const StyleRegisteredProperty *property,
                                     char *output, size_t output_size)
{
    if (property == NULL || property->initial_value == NULL
        || property->initial_length >= output_size) return false;
    memcpy(output, property->initial_value, property->initial_length + 1u);
    return true;
}

/* A registered property declared `initial` takes its initial value. */
static void registered_initial_keyword(
    const StyleRegisteredProperty *property, char *output,
    size_t output_size)
{
    if (property == NULL || property->initial_value == NULL) return;
    const char *value = output;
    size_t value_length = strlen(output);
    trim(&value, &value_length);
    if (span_case_equal(value, value_length, "initial"))
        (void) registered_initial_value(property, output, output_size);
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
    /* The scoped bit is the top bit: unscoped rules sort first. */
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
    if (cache == NULL) cache = variable_cache_from_lease(sheet);
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
        sheet->variable_read_names |= entry->reads;
        sheet->variable_cache_hits++;
        TILEFINCH_WORK_ADD(style_variable_cache_hits, 1);
        if (sheet->resolve_scratch->variable_cache_lease != NULL)
            sheet->resolve_scratch->variable_cache_lease->hits++;
        if (entry->found == VARIABLE_CACHE_MISS) {
            sheet->variable_cache_negative_hits++;
            return 0;
        }
        if (entry->found == VARIABLE_CACHE_INLINE) {
            style_copy_text(output, output_size, entry->value.text);
            return 1;
        }
        /* A long value the caller cannot hold is resolved afresh, which
           reports the same overflow; never truncate it. */
        if (entry->value.spill.length >= output_size) return -1;
        memcpy(output, cache->spill + entry->value.spill.offset,
               entry->value.spill.length + 1u);
        return 1;
    }
    sheet->variable_cache_misses++;
    TILEFINCH_WORK_ADD(style_variable_cache_misses, 1);
    if (sheet->resolve_scratch->variable_cache_lease != NULL)
        sheet->resolve_scratch->variable_cache_lease->misses++;
    return -1;
}

/* Copies a long value into the cache's spill: its offset there, or
   UINT32_MAX when it is not cached (spill full or refused). */
static uint32_t variable_cache_spill(Stylesheet *sheet,
                                     StyleVariableCache *cache,
                                     const char *value, size_t length)
{
    if (cache->spill == NULL) {
        if (cache->spill_refused) return UINT32_MAX;
        cache->spill = budget_malloc_category(
            cache->budget, BUDGET_CATEGORY_LAYOUT,
            STYLE_VARIABLE_CACHE_SPILL_BYTES);
        if (cache->spill == NULL) {
            cache->spill_refused = true;
            return UINT32_MAX;
        }
        size_t bytes = variable_cache_footprint(cache);
        if (bytes > sheet->variable_cache_peak_bytes)
            sheet->variable_cache_peak_bytes = bytes;
    }
    if (length + 1u > STYLE_VARIABLE_CACHE_SPILL_BYTES - cache->spill_used)
        return UINT32_MAX;
    uint32_t offset = cache->spill_used;
    memcpy(cache->spill + offset, value, length + 1u);
    cache->spill_used += (uint32_t) (length + 1u);
    return offset;
}

/* Stores one lookup result for each of `nodes`. A long value is spilled
   once and shared by every entry: the spill is only ever emptied with the
   whole table. */
static void variable_cache_store_nodes(
    Stylesheet *sheet, lxb_dom_node_t *const *nodes, size_t node_count,
    const char *name, size_t name_length, uint32_t hash, const char *value)
{
    StyleVariableCache *cache = sheet->resolve_scratch->variable_cache;
    if (cache == NULL || node_count == 0
        || name_length >= sizeof(cache->entries[0].name)) return;
    size_t value_length = value == NULL ? 0 : strlen(value);
    uint32_t spill = UINT32_MAX;
    if (value_length >= sizeof(cache->entries[0].value.text)) {
        spill = variable_cache_spill(sheet, cache, value, value_length);
        if (spill == UINT32_MAX) return;
    }
    for (size_t i = 0; i < node_count; i++) {
        size_t home = variable_cache_home(cache, nodes[i], hash);
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
        if (target == NULL) continue;
        if (target->occupied) sheet->variable_cache_evictions++;
        cache->stored = true;
        *target = (StyleVariableCacheEntry) {
            .node = nodes[i],
            .hash = hash,
            .stamp = ++cache->clock,
            .name_length = (uint16_t) name_length,
            .occupied = 1,
            .found = value == NULL ? VARIABLE_CACHE_MISS
                : spill != UINT32_MAX ? VARIABLE_CACHE_SPILLED
                : VARIABLE_CACHE_INLINE,
            .reads = sheet->variable_read_names
        };
        memcpy(target->name, name, name_length);
        target->name[name_length] = '\0';
        if (spill != UINT32_MAX) {
            target->value.spill.offset = spill;
            target->value.spill.length = (uint32_t) value_length;
        } else if (value != NULL) {
            memcpy(target->value.text, value, value_length + 1u);
        }
    }
}

static void variable_cache_store(Stylesheet *sheet, lxb_dom_node_t *node,
                                 const char *name, size_t name_length,
                                 uint32_t hash, const char *value)
{
    variable_cache_store_nodes(sheet, &node, 1, name, name_length, hash,
                               value);
}

static bool retained_rule_fast_matches(const Stylesheet *sheet,
                                        const StyleCustomRule *rule,
                                        const StyleMatchSubject *subject);

/* The custom rule for property `name` that wins on `node`: among rules for
   that name and pseudo-element whose container query and selector match,
   the latest in cascade order. `hash` is the name's custom_rule_name_hash
   and `subject` the node's prepared subject. `candidates`, when given,
   counts rules examined. The single scan behind variable, retained and
   presentation lookups. */
/* Whether a declaration block gives an image-bearing property a value
   containing var(). Property names are matched exactly (background-color
   is not image-bearing); a match errs toward "yes". */
static bool style_block_image_uses_variable(const char *text, size_t length)
{
    static const char *const image_properties[] = {
        "background", "background-image", "mask", "mask-image",
        "-webkit-mask", "-webkit-mask-image", "list-style",
        "list-style-image", "border-image", "border-image-source",
        "content", "cursor"
    };
    for (size_t at = 0; at < length;) {
        size_t end = at;
        while (end < length && text[end] != ';' && text[end] != '}') end++;
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            size_t name_start = at, name_end = colon;
            while (name_start < name_end
                   && (isspace((unsigned char) text[name_start])
                       || text[name_start] == '{'))
                name_start++;
            while (name_end > name_start
                   && isspace((unsigned char) text[name_end - 1u]))
                name_end--;
            bool image = false;
            for (size_t p = 0; !image && p < sizeof(image_properties)
                                               / sizeof(image_properties[0]);
                 p++) {
                size_t wanted = strlen(image_properties[p]);
                image = name_end - name_start == wanted
                    && strncasecmp(text + name_start, image_properties[p],
                                   wanted) == 0;
            }
            if (image) {
                for (size_t v = colon + 1u; v + 4u <= end; v++)
                    if (strncasecmp(text + v, "var(", 4) == 0) return true;
            }
        }
        at = end + 1u;
    }
    return false;
}

static const StyleCustomRule *custom_rule_winner(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, uint32_t hash,
    const StyleMatchSubject *subject, size_t *candidates)
{
    if (name_length >= STYLE_CUSTOM_NAME_CAPACITY) return NULL;
    const StyleCustomRule *winner = NULL;
    size_t first = sheet->custom_rule_index_ready
        ? custom_rule_index_lower_bound(
              sheet->custom_rule_index, sheet->custom_rule_index_count, hash)
        : 0;
    size_t end = sheet->custom_rule_index_ready
        ? sheet->custom_rule_index_count : sheet->custom_rule_count;
    bool outside = style_subject_outside_custom_scopes(sheet, subject);
    for (size_t i = first; i < end; i++) {
        size_t rule_index = i;
        if (sheet->custom_rule_index_ready) {
            uint32_t entry = sheet->custom_rule_index[i].index;
            if (sheet->custom_rule_index[i].hash != hash
                || (outside && (entry & STYLE_CUSTOM_INDEX_SCOPED) != 0))
                break;
            rule_index = entry & ~STYLE_CUSTOM_INDEX_SCOPED;
        }
        if (candidates != NULL) (*candidates)++;
        const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
        if (rule->pseudo != (uint8_t) pseudo
            || !style_container_query_matches(
                sheet, rule->container_query, node)
            || rule->name_length != name_length
            || memcmp(rule->name, name, name_length) != 0
            || !retained_rule_fast_matches(sheet, rule, subject)
            || !style_adopted_rule_in_scope(
                   sheet, rule->order >> 8, rule->selector,
                   rule->selector_length, node, subject)
            || !style_selector_matches_profiled(
                sheet, node, rule->selector, rule->selector_length)) continue;
        if (custom_rule_later(sheet, node, rule, winner)) winner = rule;
    }
    return winner;
}

/* The declared value of custom property `name` that wins on the element
   itself, as a span of stylesheet arena or style attribute text. */
static bool local_variable_value(
    Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement wanted_pseudo,
    const char *name, size_t name_length, uint32_t hash,
    const char **value, size_t *value_length)
{
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    size_t candidates = 0;
    const StyleCustomRule *winner = custom_rule_winner(
        sheet, node, wanted_pseudo, name, name_length, hash, &subject,
        &candidates);
    sheet->variable_rule_candidates += candidates;

    bool inline_important = false;
    StyleCustomRule inline_rule = {0};
    bool inline_wins = false;
    bool inline_found = wanted_pseudo == PSEUDO_NONE
        && inline_variable_value(
            sheet, node, name, name_length, STYLE_CUSTOM_VALUE_CAPACITY,
            value, value_length, &inline_important);
    if (inline_found) {
        inline_rule.important = inline_important;
        inline_rule.origin = 0;
        inline_rule.layer = UINT_MAX;
        inline_rule.specificity = UINT_MAX;
        inline_rule.order = UINT_MAX;
        inline_wins = custom_rule_later(sheet, node, &inline_rule, winner);
    }
    if (inline_wins) return true;
    if (winner == NULL) return false;
    *value = winner->value;
    *value_length = strlen(winner->value);
    return true;
}

bool style_resolve_value(const Stylesheet *sheet, const char *text,
                         size_t length, char *output, size_t output_size,
                         unsigned depth);

/* A custom property's var() references take their values on the element
   that declares it, and descendants inherit that result (CSS Variables
   3.1). Resolving at the use site instead let a descendant's override
   leak in: chatgpt.com's :root colours built from lightningcss's
   --lightningcss-light/-dark toggles came out in the wrong scheme under
   any element that re-declared a toggle. Bounded against reference
   cycles; an unresolvable value is kept raw (the former behaviour).

   The declarations being resolved are recorded so that a nested lookup of
   one of them is recognised as a reference cycle, which makes it invalid
   (CSS Variables 1, section 2.3). Merely letting the nesting bound stop a
   cycle re-resolved it at every level of both recursions, rescanning the
   inline style each time (a two-property cycle in style="" cost over a
   thousand lookups per use). The record lives in the sheet's resolve
   scratch, beside resolution_node, so it is scoped exactly like the
   resolution itself (the former nesting counter was process-global). */
static bool variable_resolving_contains(const StyleResolveScratch *scratch,
                                        const lxb_dom_node_t *node,
                                        PseudoElement pseudo,
                                        const char *name, size_t name_length)
{
    for (unsigned i = 0; i < scratch->variable_resolving_count; i++) {
        if (scratch->variable_resolving[i].node == node
            && scratch->variable_resolving[i].pseudo == pseudo
            && scratch->variable_resolving[i].name_length == name_length
            && memcmp(scratch->variable_resolving[i].name, name,
                      name_length) == 0)
            return true;
    }
    return false;
}

/* Resolves the declared value (`value`, `value_length`) of `name` on the
   element that declares it into `output`. False only when the result does
   not fit `output_size`: a value is never truncated. */
static bool variable_resolve_at_declaration(
    Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, const char *value,
    size_t value_length, char *output, size_t output_size)
{
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (span_contains_text(value, value_length, "var(")
        && scratch->variable_resolving_count
               < STYLE_VARIABLE_RESOLVING_LIMIT) {
        lxb_dom_node_t *saved_node = scratch->resolution_node;
        PseudoElement saved_pseudo = scratch->resolution_pseudo;
        scratch->resolution_node = node;
        scratch->resolution_pseudo = pseudo;
        unsigned slot = scratch->variable_resolving_count++;
        scratch->variable_resolving[slot].node = node;
        scratch->variable_resolving[slot].pseudo = pseudo;
        scratch->variable_resolving[slot].name = name;
        scratch->variable_resolving[slot].name_length = name_length;
        bool resolved = style_resolve_value(sheet, value, value_length,
                                            output, output_size, 0);
        scratch->variable_resolving_count--;
        scratch->resolution_node = saved_node;
        scratch->resolution_pseudo = saved_pseudo;
        if (resolved) return true;
    }
    /* No reference, or unresolvable here: the raw value. */
    if (value_length >= output_size) return false;
    memcpy(output, value, value_length);
    output[value_length] = '\0';
    return true;
}

uint64_t stylesheet_custom_property_name_bits(const char *name,
                                              size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; name != NULL && i < length; i++) {
        hash ^= (unsigned char) tolower((unsigned char) name[i]);
        hash *= UINT32_C(16777619);
    }
    return (UINT64_C(1) << (hash & 63u))
        | (UINT64_C(1) << ((hash >> 6) & 63u));
}

uint64_t stylesheet_inline_custom_property_bits(const char *text,
                                                size_t length)
{
    uint64_t bits = 0;
    for (size_t at = 0; text != NULL && at < length;) {
        at = skip_css_space_and_comments(text, length, at);
        if (at >= length) break;
        size_t end = find_declaration_end(text, length, at);
        size_t colon = at;
        while (colon < end && text[colon] != ':') colon++;
        if (colon < end) {
            const char *name = text + at;
            size_t name_length = colon - at;
            trim(&name, &name_length);
            if (name_length > 2 && name[0] == '-' && name[1] == '-')
                bits |= stylesheet_custom_property_name_bits(name,
                                                             name_length);
        }
        at = end + (end < length);
    }
    return bits;
}

static bool lookup_variable_reading(const Stylesheet *sheet,
                                    const char *name, size_t name_length,
                                    char *output, size_t output_size);

/* Records the names one lookup reads, the transitive ones included, both
   for the caller's resolution and for the variable-cache entries it
   stores (see StyleVariableCacheEntry.reads).

   Each var() is expanded where it is used, so a chain of custom properties
   that each reference the next several times expands exponentially
   (eleven references per level fit in a custom value, so five levels are
   161,051 lookups). Without the layout variable cache, as for properties
   declared only in style="", a few hundred bytes stalled layout.
   One outermost resolution (style_resolve_value or lookup, with all the
   re-resolution nested in it) may therefore expand at most
   STYLE_VARIABLE_SUBSTITUTION_LIMIT references; past that they are
   treated as undefined. Real token chains use a few dozen. */
static bool lookup_variable(const Stylesheet *sheet, const char *name,
                            size_t name_length, char *output,
                            size_t output_size)
{
    if (sheet == NULL || output == NULL || output_size == 0) return false;
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    if (scratch->variable_work_nesting == 0)
        scratch->variable_substitutions = 0;
    if (scratch->variable_substitutions >= STYLE_VARIABLE_SUBSTITUTION_LIMIT)
        return false;
    scratch->variable_substitutions++;
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    uint64_t outer = sheet->variable_read_names;
    mutable_sheet->variable_read_names =
        stylesheet_custom_property_name_bits(name, name_length);
    scratch->variable_work_nesting++;
    bool found = lookup_variable_reading(sheet, name, name_length, output,
                                         output_size);
    scratch->variable_work_nesting--;
    mutable_sheet->variable_read_names |= outer;
    return found;
}

static bool lookup_variable_reading(const Stylesheet *sheet,
                                    const char *name, size_t name_length,
                                    char *output, size_t output_size)
{
    Stylesheet *mutable_sheet = (Stylesheet *) sheet;
    mutable_sheet->variable_lookup_calls++;
    TILEFINCH_WORK_ADD(style_variable_lookups, 1);
    lxb_dom_node_t *resolution_node = sheet->resolve_scratch->resolution_node;
    uint32_t hash = custom_rule_name_hash(name, name_length);
    PseudoElement resolution_pseudo =
        sheet->resolve_scratch->resolution_pseudo;
    if (variable_resolving_contains(sheet->resolve_scratch, resolution_node,
                                    resolution_pseudo, name, name_length))
        return false;
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
       winner. A winner whose value does not fit `output` is not found and
       not cached (another caller's buffer may hold it). */
    const char *declared = NULL;
    size_t declared_length = 0;
    /* An @property registration decides inheritance and what an element
       no declaration reaches sees. */
    const StyleRegisteredProperty *registered =
        sheet->registered_property_count == 0 ? NULL
        : stylesheet_registered_property(sheet, name, name_length, hash);
    bool inherits = registered == NULL || registered->inherits;
    if (resolution_pseudo != PSEUDO_NONE
        && local_variable_value(
            mutable_sheet, resolution_node, resolution_pseudo,
            name, name_length, hash, &declared, &declared_length)) {
        if (!variable_resolve_at_declaration(
                mutable_sheet, resolution_node, resolution_pseudo,
                name, name_length, declared, declared_length,
                output, output_size)) return false;
        registered_initial_keyword(registered, output, output_size);
        variable_cache_store(
            mutable_sheet, resolution_node, name, name_length,
            cache_hash, output);
        return true;
    }

    enum { CACHED_DESCENDANT_LIMIT = 64 };
    lxb_dom_node_t *uncached_nodes[CACHED_DESCENDANT_LIMIT];
    size_t uncached_count = 0;
    /* A generated box of a non-inheriting property sees only its own
       declarations; an element, only its own. */
    for (lxb_dom_node_t *node = inherits || resolution_pseudo == PSEUDO_NONE
             ? resolution_node : NULL;
         node != NULL; node = inherits ? node->parent : NULL) {
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
                hash, &declared, &declared_length)) {
            if (!variable_resolve_at_declaration(
                    mutable_sheet, node, PSEUDO_NONE, name, name_length,
                    declared, declared_length, output, output_size))
                return false;
            registered_initial_keyword(registered, output, output_size);
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
    for (size_t i = inherits || resolution_node == NULL
                        ? sheet->variable_count : 0;
         i != 0; i--) {
        const StyleVariable *variable = &sheet->variables[i - 1];
        if (variable->name_length == name_length
            && memcmp(variable->name, name, name_length) == 0) {
            if (variable->value_length >= output_size) return false;
            memcpy(output, variable->value, variable->value_length + 1u);
            registered_initial_keyword(registered, output, output_size);
            global_value = output;
            break;
        }
    }
    if (global_value == NULL && registered != NULL
        && registered->initial_value != NULL) {
        if (!registered_initial_value(registered, output, output_size))
            return false;
        global_value = output;
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

static bool style_custom_property_value_scoped(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, char *output, size_t output_size);

bool style_custom_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    style_class_tokens_scope_begin(sheet);
    bool found = style_custom_property_value_scoped(
        sheet, node, pseudo, name, name_length, output, output_size);
    style_class_tokens_scope_end(sheet);
    return found;
}

static bool style_custom_property_value_scoped(
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
    char raw[STYLE_CUSTOM_RESOLVED_CAPACITY];
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

bool style_resolve_value_on_node(
    const Stylesheet *sheet, lxb_dom_node_t *node, const char *text,
    size_t length, char *output, size_t output_size)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || text == NULL || output == NULL || output_size == 0) {
        return false;
    }
    style_class_tokens_scope_begin(sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleResolveScratch *scratch = sheet->resolve_scratch;
    lxb_dom_node_t *saved_node = scratch->resolution_node;
    PseudoElement saved_pseudo = scratch->resolution_pseudo;
    scratch->resolution_node = node;
    scratch->resolution_pseudo = PSEUDO_NONE;
    bool found = style_resolve_value(
        sheet, text, length, output, output_size, 0);
    if (!found) output[0] = '\0';
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    style_relative_selector_cache_end((Stylesheet *) sheet);
    style_class_tokens_scope_end(sheet);
    return found;
}

static bool style_retained_property_value_scoped(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size);

/* Resolved once per retained property of every element: each candidate
   rule's class key is tested against the element's tokenized class list. */
bool style_retained_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    style_class_tokens_scope_begin(sheet);
    bool found = style_retained_property_value_scoped(
        sheet, node, name, name_length, output, output_size);
    style_class_tokens_scope_end(sheet);
    return found;
}

static bool style_retained_property_value_scoped(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size)
{
    if (sheet == NULL || sheet->resolve_scratch == NULL || node == NULL
        || name == NULL || name_length == 0
        || name_length >= STYLE_CUSTOM_NAME_CAPACITY
        || output == NULL || output_size == 0) {
        return false;
    }
    stylesheet_prepare_rule_index((Stylesheet *) sheet);
    style_relative_selector_cache_begin((Stylesheet *) sheet);
    StyleMatchSubject subject = {0};
    style_match_subject_prepare(node, &subject);
    uint32_t hash = custom_rule_name_hash(name, name_length);
    const StyleCustomRule *winner = custom_rule_winner(
        sheet, node, PSEUDO_NONE, name, name_length, hash, &subject, NULL);
    const char *value = winner == NULL ? NULL : winner->value;
    size_t value_length = value == NULL ? 0 : strlen(value);
    const char *inline_value = NULL;
    size_t inline_length = 0;
    bool inline_important = false;
    bool custom_property = name_length > 2 && name[0] == '-'
        && name[1] == '-';
    if (inline_variable_value(
            sheet, node, name, name_length,
            custom_property ? STYLE_CUSTOM_VALUE_CAPACITY
                            : STYLE_RETAINED_VALUE_CAPACITY,
            &inline_value, &inline_length, &inline_important)) {
        StyleCustomRule inline_rule = {
            .important = inline_important,
            .origin = 0,
            .layer = UINT_MAX,
            .specificity = UINT_MAX,
            .order = UINT_MAX
        };
        if (custom_rule_later(sheet, node, &inline_rule, winner)) {
            value = inline_value;
            value_length = inline_length;
        }
    }
    if (value == NULL) {
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
        sheet, value, value_length, output, output_size, 0);
    scratch->resolution_node = saved_node;
    scratch->resolution_pseudo = saved_pseudo;
    if (!found) output[0] = '\0';
    style_relative_selector_cache_end((Stylesheet *) sheet);
    return found;
}

typedef struct {
    const StyleCustomRule *rule;
    StyleCustomRule inline_rule;
    char inline_value[STYLE_RETAINED_VALUE_CAPACITY];
} RetainedRuleWinner;

static bool retained_rule_fast_matches(const Stylesheet *sheet,
                                        const StyleCustomRule *rule,
                                        const StyleMatchSubject *subject)
{
    if (rule == NULL || subject == NULL
        || rule->fast_key_offset == UINT8_MAX
        || rule->fast_key_length == 0) return true;
    size_t offset = rule->fast_key_offset;
    if (offset >= rule->selector_length
        || rule->fast_key_length > rule->selector_length - offset)
        return true;
    const char *key = rule->selector + offset;
    size_t key_length = rule->fast_key_length;
    char marker = offset == 0 ? '\0' : rule->selector[offset - 1u];
    if (marker == '#') {
        return subject->id != NULL && subject->id_length == key_length
            && memcmp(subject->id, key, key_length) == 0;
    }
    if (marker == '.')
        return style_subject_has_class(sheet, subject, key, key_length);
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
    output->rule = custom_rule_winner(
        sheet, node, PSEUDO_NONE, name, name_length, hash, &subject, NULL);
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
                if (custom_rule_later(sheet, node, &candidate, output->rule)) {
                    memcpy(output->inline_value, value, value_length);
                    output->inline_value[value_length] = '\0';
                    candidate.value = output->inline_value;
                    output->inline_rule = candidate;
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
            && custom_rule_later(sheet, node, winners[0].rule, winner)) {
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
            && custom_rule_later(sheet, node, winners[0].rule, winner)) {
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
        bool outside = style_subject_outside_custom_scopes(sheet, &subject);
        for (size_t i = first; i < end; i++) {
            size_t rule_index = i;
            if (sheet->custom_rule_index_ready) {
                uint32_t entry = sheet->custom_rule_index[i].index;
                if (sheet->custom_rule_index[i].hash != hash
                    || (outside
                        && (entry & STYLE_CUSTOM_INDEX_SCOPED) != 0)) break;
                rule_index = entry & ~STYLE_CUSTOM_INDEX_SCOPED;
            }
            const StyleCustomRule *rule = &sheet->custom_rules[rule_index];
            if (rule->pseudo != PSEUDO_NONE
                || !style_container_query_matches(
                    sheet, rule->container_query, node)
                || rule->name_length != name_length
                || memcmp(rule->name, name, name_length) != 0
                || !retained_rule_fast_matches(sheet, rule, &subject)
                /* Outside the selector-text cache: the same text in an
                   unscoped sheet matches regardless. */
                || !style_adopted_rule_in_scope(
                       sheet, rule->order >> 8, rule->selector,
                       rule->selector_length, node, &subject)) continue;
            size_t selector_length = rule->selector_length;
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
            if (matched && custom_rule_later(sheet, node, rule, winners[property])) {
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
            if (custom_rule_later(sheet, node, &inline_rule, winners[property])) {
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
    size_t index = sheet->variable_count;
    for (size_t i = 0; name_length < STYLE_CUSTOM_NAME_CAPACITY
                       && i < sheet->variable_count; i++) {
        if (sheet->variables[i].name_length == name_length
            && memcmp(sheet->variables[i].name, name, name_length) == 0) {
            index = i;
            break;
        }
    }
    if (name_length == 0 || name_length >= STYLE_CUSTOM_NAME_CAPACITY
        || !stylesheet_admit_custom_value(sheet, value_length)) {
        if (sheet->diagnostic_custom_property_drops != UINT64_MAX) {
            sheet->diagnostic_custom_property_drops++;
        }
        return true;
    }
    if (index == sheet->variable_count
        && sheet->variable_count == sheet->variable_capacity) {
        size_t capacity = sheet->variable_capacity == 0
                          ? 16 : sheet->variable_capacity * 2;
        StyleVariable *variables = budget_realloc(
            sheet->budget, sheet->variables, capacity * sizeof(*variables));
        if (variables == NULL) return false;
        sheet->variables = variables;
        sheet->variable_capacity = capacity;
    }
    /* One arena string holds "name\0value\0"; a redeclaration takes new
       text (the old bytes stay until the sheet is destroyed). */
    char *text = stylesheet_reserve_text(sheet,
                                         name_length + 1u + value_length);
    if (text == NULL) return false;
    memcpy(text, name, name_length);
    text[name_length] = '\0';
    memcpy(text + name_length + 1u, value, value_length);
    text[name_length + 1u + value_length] = '\0';
    if (index == sheet->variable_count) sheet->variable_count++;
    sheet->variables[index] = (StyleVariable) {
        .name = text,
        .value = text + name_length + 1u,
        .value_length = (uint16_t) value_length,
        .name_length = (uint8_t) name_length
    };
    return true;
}

static bool style_resolve_value_nested(const Stylesheet *sheet,
                                       const char *text, size_t length,
                                       char *output, size_t output_size,
                                       unsigned depth);

/* Scopes the var() expansion budget (see lookup_variable) to the
   outermost resolution. */
bool style_resolve_value(const Stylesheet *sheet, const char *text,
                         size_t length, char *output, size_t output_size,
                         unsigned depth)
{
    StyleResolveScratch *scratch =
        sheet == NULL ? NULL : sheet->resolve_scratch;
    if (scratch == NULL) {
        return style_resolve_value_nested(sheet, text, length, output,
                                          output_size, depth);
    }
    if (scratch->variable_work_nesting++ == 0)
        scratch->variable_substitutions = 0;
    bool resolved = style_resolve_value_nested(sheet, text, length, output,
                                               output_size, depth);
    scratch->variable_work_nesting--;
    return resolved;
}

static bool style_resolve_value_nested(const Stylesheet *sheet,
                                       const char *text, size_t length,
                                       char *output, size_t output_size,
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
        char uncommented[STYLE_CUSTOM_RESOLVED_CAPACITY];
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
        /* The looked-up value, already resolved where it was declared:
           at most STYLE_CUSTOM_RESOLVED_CAPACITY, never truncated (a longer
           one is not found). */
        char raw[STYLE_CUSTOM_RESOLVED_CAPACITY];
        const char *replacement = NULL;
        size_t replacement_length = 0;
        /* A custom property set to "initial" holds the guaranteed-invalid
           value: var() then takes its fallback, exactly as if it were
           undefined. lightningcss's light-dark() lowering depends on it
           ("--lightningcss-light: initial" selects every light fallback). */
        bool defined = lookup_variable(sheet, name, name_length, raw,
                                       sizeof(raw));
        if (defined) {
            const char *raw_value = raw;
            size_t raw_length = strlen(raw);
            trim(&raw_value, &raw_length);
            defined = !span_case_equal(raw_value, raw_length, "initial");
        }
        if (defined) {
            replacement = raw;
            replacement_length = strlen(raw);
        }
        else if (comma < inside_length) {
            replacement = inside + comma + 1;
            replacement_length = inside_length - comma - 1;
        }
        else return false;
        char resolved[STYLE_CUSTOM_RESOLVED_CAPACITY];
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

static bool style_declaration_equal(const Stylesheet *sheet,
                                    const StyleDeclaration *stored,
                                    const uint8_t *encoded,
                                    size_t encoded_length,
                                    uint64_t mask, uint64_t mask_high,
                                    uint64_t inherit_mask,
                                    const char *deferred,
                                    size_t deferred_length)
{
    if (sheet == NULL || stored == NULL || stored->mask != mask
        || stored->mask_high != mask_high
        || stored->inherit_mask != inherit_mask
        || stored->deferred_length != deferred_length
        || stored->values_length != encoded_length
        || (encoded_length != 0
            && (encoded == NULL || sheet->declaration_values == NULL
                || stored->values_offset > sheet->declaration_value_bytes
                || encoded_length > sheet->declaration_value_bytes
                                    - stored->values_offset
                || memcmp(sheet->declaration_values + stored->values_offset,
                          encoded, encoded_length) != 0))) {
        return false;
    }
    return deferred_length == 0
        || (stored->deferred_declarations != NULL && deferred != NULL
            && memcmp(stored->deferred_declarations, deferred,
                      deferred_length) == 0);
}

static bool style_declarations_equal(const Stylesheet *sheet,
                                     const StyleDeclaration *stored,
                                     const StyleDeclaration *candidate)
{
    return candidate != NULL
        && candidate->values_offset <= sheet->declaration_value_bytes
        && candidate->values_length
               <= sheet->declaration_value_bytes - candidate->values_offset
        && style_declaration_equal(
               sheet, stored,
               sheet->declaration_values == NULL ? NULL
                   : sheet->declaration_values + candidate->values_offset,
               candidate->values_length, candidate->mask,
               candidate->mask_high, candidate->inherit_mask,
               candidate->deferred_declarations,
               candidate->deferred_length);
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

static uint32_t style_declaration_hash(const uint8_t *encoded,
                                       size_t encoded_length,
                                       uint64_t mask, uint64_t mask_high,
                                       uint64_t inherit_mask,
                                       const char *deferred,
                                       size_t deferred_length)
{
    uint32_t hash = UINT32_C(2166136261);
    if (encoded_length != 0) {
        hash = style_hash_bytes(hash, encoded, encoded_length);
    }
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

static uint32_t style_stored_declaration_hash(
    const Stylesheet *sheet, const StyleDeclaration *declaration)
{
    const uint8_t *encoded = sheet->declaration_values == NULL
        || declaration->values_offset > sheet->declaration_value_bytes
        ? NULL : sheet->declaration_values + declaration->values_offset;
    return style_declaration_hash(
        encoded, encoded == NULL ? 0 : declaration->values_length,
        declaration->mask, declaration->mask_high,
        declaration->inherit_mask, declaration->deferred_declarations,
        declaration->deferred_length);
}

/* Appends encoded values to the sheet's value pool. Growth is admitted by
   the Budget and rolls back to the unchanged pool on refusal. */
static bool style_declaration_values_append(Stylesheet *sheet,
                                            const uint8_t *encoded,
                                            size_t length,
                                            uint32_t *offset)
{
    if (sheet == NULL || offset == NULL
        || (encoded == NULL && length != 0)) return false;
    if (sheet->declaration_value_bytes > UINT32_MAX
        || length > UINT32_MAX - sheet->declaration_value_bytes) {
        return false;
    }
    if (length > sheet->declaration_value_capacity
                 - sheet->declaration_value_bytes) {
        size_t required = sheet->declaration_value_bytes + length;
        size_t capacity = sheet->declaration_value_capacity == 0
            ? 1024u : sheet->declaration_value_capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2u) return false;
            capacity *= 2u;
        }
        uint8_t *grown = budget_realloc(
            sheet->budget, sheet->declaration_values, capacity);
        if (grown == NULL) return false;
        sheet->declaration_values = grown;
        sheet->declaration_value_capacity = capacity;
    }
    *offset = (uint32_t) sheet->declaration_value_bytes;
    if (length != 0) {
        memcpy(sheet->declaration_values + sheet->declaration_value_bytes,
               encoded, length);
    }
    sheet->declaration_value_bytes += length;
    return true;
}

/* Rewrites the value pool to hold exactly the encodings of the current
   declarations, dropping ones replaced by font fixups or discarded by
   reinterning. Optional: a refused allocation keeps the valid old pool. */
static void style_declaration_values_compact(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->declaration_values == NULL) return;
    size_t live = 0;
    for (size_t i = 0; i < sheet->declaration_count; i++) {
        live += sheet->declarations[i].values_length;
    }
    if (live >= sheet->declaration_value_bytes) return;
    uint8_t *compact = live == 0 ? NULL
        : budget_malloc(sheet->budget, live);
    if (live != 0 && compact == NULL) return;
    size_t at = 0;
    for (size_t i = 0; i < sheet->declaration_count; i++) {
        StyleDeclaration *declaration = &sheet->declarations[i];
        size_t length = declaration->values_length;
        if (length != 0) {
            memcpy(compact + at,
                   sheet->declaration_values + declaration->values_offset,
                   length);
        }
        declaration->values_offset = (uint32_t) at;
        at += length;
    }
    budget_free(sheet->budget, sheet->declaration_values);
    sheet->declaration_values = compact;
    sheet->declaration_value_bytes = live;
    sheet->declaration_value_capacity = live;
}

static uint16_t style_declaration_values_summary(const ComputedStyle *values)
{
    uint16_t flags = 0;
    if (values->generated_content) {
        flags |= STYLE_DECLARATION_VALUES_GENERATED_CONTENT;
    }
    if (computed_style_paint_stack_id(values) != 0) {
        flags |= STYLE_DECLARATION_VALUES_PAINT_STACK;
    }
    if (values->generated_attr != NULL) {
        flags |= STYLE_DECLARATION_VALUES_GENERATED_ATTR;
    }
    return flags;
}

/* Replaces a stored declaration's values (a post-parse font fixup). The
   previous encoding stays in the pool until the sheet is compacted. */
static bool style_declaration_values_replace(Stylesheet *sheet,
                                             StyleDeclaration *declaration,
                                             const ComputedStyle *values)
{
    uint8_t encoded[STYLE_DECLARATION_VALUES_ENCODED_MAX];
    size_t length = style_declaration_values_encode(
        values, encoded, sizeof(encoded));
    uint32_t offset = 0;
    if (!style_declaration_values_append(sheet, encoded, length, &offset)) {
        return false;
    }
    declaration->values_offset = offset;
    declaration->values_length = (uint16_t) length;
    declaration->values_flags = style_declaration_values_summary(values);
    return true;
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
    uint32_t hash = style_stored_declaration_hash(
        sheet, &sheet->declarations[declaration_index]);
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

    uint8_t encoded[STYLE_DECLARATION_VALUES_ENCODED_MAX];
    size_t encoded_length = style_declaration_values_encode(
        values, encoded, sizeof(encoded));
    bool indexed = !force_unique && sheet->declaration_count != SIZE_MAX
        && style_declaration_index_prepare(
            sheet, sheet->declaration_count + 1u);
    uint32_t declaration_hash = 0;
    if (indexed) {
        declaration_hash = style_declaration_hash(
            encoded, encoded_length, mask, mask_high, inherit_mask,
            deferred, deferred_length);
        size_t table_mask = sheet->declaration_index_slot_count - 1u;
        size_t slot = (size_t) declaration_hash & table_mask;
        for (size_t probes = 0;
             probes < sheet->declaration_index_slot_count;
             probes++, slot = (slot + 1u) & table_mask) {
            uint32_t candidate = sheet->declaration_index_slots[slot];
            if (candidate == UINT32_MAX) break;
            if (candidate < sheet->declaration_count
                && style_declaration_equal(
                    sheet, &sheet->declarations[candidate], encoded,
                    encoded_length, mask, mask_high, inherit_mask,
                    deferred, deferred_length)) {
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
            if (style_declaration_equal(sheet, &sheet->declarations[i - 1],
                                        encoded, encoded_length,
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
    if (deferred_length == SIZE_MAX) return false;
    size_t values_bytes_before = sheet->declaration_value_bytes;
    uint32_t values_offset = 0;
    if (!style_declaration_values_append(
            sheet, encoded, encoded_length, &values_offset)) return false;
    char *deferred_copy = NULL;
    if (deferred_length != 0) {
        deferred_copy = budget_malloc(sheet->budget, deferred_length + 1u);
        if (deferred_copy == NULL) {
            sheet->declaration_value_bytes = values_bytes_before;
            return false;
        }
        memcpy(deferred_copy, deferred, deferred_length);
        deferred_copy[deferred_length] = '\0';
    }
    uint32_t deferred_program_offset = UINT32_MAX;
    uint16_t deferred_program_count = 0;
    if (deferred_copy != NULL && !sheet->image_declarations_use_variables)
        sheet->image_declarations_use_variables =
            style_block_image_uses_variable(deferred_copy, deferred_length);
    if (deferred_copy != NULL) {
        (void) style_compile_deferred_program(
            sheet, deferred_copy, deferred_length,
            &deferred_program_offset, &deferred_program_count);
    }
    size_t declaration_index = sheet->declaration_count++;
    sheet->declarations[declaration_index] = (StyleDeclaration) {
        .values_offset = values_offset,
        .values_length = (uint16_t) encoded_length,
        .values_flags = style_declaration_values_summary(values),
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
    if ((declaration->mask & (S_OPACITY | S_COLOR)) != 0
        && STYLE_TRACE(sheet, PAINT)) {
        ComputedStyle values;
        stylesheet_declaration_values(sheet, declaration, &values);
        if ((declaration->mask & S_OPACITY) != 0) {
            fprintf(stderr, "style-opacity selector=%s value=%u "
                    "important=%d origin=%u\n", rule.selector,
                    values.opacity, rule.important, rule.origin);
        }
        if ((declaration->mask & S_COLOR) != 0 && values.color_alpha == 0) {
            fprintf(stderr, "style-transparent-color selector=%s "
                    "important=%d origin=%u\n", rule.selector,
                    rule.important, rule.origin);
        }
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

static bool has_rule_index_fill(Budget *budget, uint32_t **indices,
                                size_t *count, size_t total,
                                const char *(*selector_at)(
                                    const Stylesheet *, size_t),
                                const Stylesheet *sheet)
{
    size_t found = 0;
    for (size_t i = 0; i < total; i++) {
        const char *selector = selector_at(sheet, i);
        if (selector != NULL && strstr(selector, ":has(") != NULL) found++;
    }
    if (found == 0) {
        budget_free(budget, *indices);
        *indices = NULL;
        *count = 0;
        return true;
    }
    uint32_t *grown = budget_realloc(budget, *indices,
                                     found * sizeof(*grown));
    if (grown == NULL) return false;
    *indices = grown;
    *count = 0;
    for (size_t i = 0; i < total; i++) {
        const char *selector = selector_at(sheet, i);
        if (selector != NULL && strstr(selector, ":has(") != NULL)
            grown[(*count)++] = (uint32_t) i;
    }
    return true;
}

static const char *has_rule_selector(const Stylesheet *sheet, size_t index)
{
    return sheet->rules[index].selector;
}

static const char *has_custom_rule_selector(const Stylesheet *sheet,
                                            size_t index)
{
    return sheet->custom_rules[index].selector;
}

bool stylesheet_prepare_has_rule_index(Stylesheet *sheet)
{
    if (sheet == NULL || sheet->budget == NULL) return false;
    if (sheet->has_rule_index_ready
        && sheet->has_rule_index_rules == sheet->count
        && sheet->has_rule_index_custom_rules == sheet->custom_rule_count)
        return true;
    if (sheet->count > UINT32_MAX || sheet->custom_rule_count > UINT32_MAX
        || !has_rule_index_fill(sheet->budget, &sheet->has_rule_indices,
                                &sheet->has_rule_count, sheet->count,
                                has_rule_selector, sheet)
        || !has_rule_index_fill(sheet->budget,
                                &sheet->has_custom_rule_indices,
                                &sheet->has_custom_rule_count,
                                sheet->custom_rule_count,
                                has_custom_rule_selector, sheet)) {
        sheet->has_rule_index_ready = false;
        return false;
    }
    sheet->has_rule_index_rules = sheet->count;
    sheet->has_rule_index_custom_rules = sheet->custom_rule_count;
    sheet->has_rule_index_ready = true;
    sheet->has_class_hashes_ready = false;
    sheet->has_attribute_memo_count = 0;
    sheet->has_attribute_memo_next = 0;
    sheet->has_plan_ready = false;
    return true;
}

static bool add_custom_rule(Stylesheet *sheet, const char *selector,
                            size_t selector_length, const char *name,
                            size_t name_length, const char *value,
                            size_t value_length, unsigned specificity,
                            unsigned order, bool important,
                            PseudoElement pseudo)
{
    /* Retained standard properties keep their own short value limit. */
    bool custom_property = name_length > 2 && name[0] == '-'
        && name[1] == '-';
    if (selector_length >= STYLE_CUSTOM_SELECTOR_CAPACITY
        || name_length >= STYLE_CUSTOM_NAME_CAPACITY
        || (custom_property
            ? !stylesheet_admit_custom_value(sheet, value_length)
            : value_length >= STYLE_RETAINED_VALUE_CAPACITY)) {
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
    /* One arena string holds "selector\0name\0value\0". */
    char *stored = stylesheet_reserve_text(
        sheet, selector_length + 1u + name_length + 1u + value_length);
    if (stored == NULL) return false;
    memcpy(stored, selector, selector_length);
    stored[selector_length] = '\0';
    memcpy(stored + selector_length + 1u, name, name_length);
    stored[selector_length + 1u + name_length] = '\0';
    memcpy(stored + selector_length + 1u + name_length + 1u, value,
           value_length);
    stored[selector_length + 1u + name_length + 1u + value_length] = '\0';
    StyleCustomRule *rule = &sheet->custom_rules[sheet->custom_rule_count++];
    memset(rule, 0, sizeof(*rule));
    rule->selector = stored;
    rule->name = stored + selector_length + 1u;
    rule->value = rule->name + name_length + 1u;
    rule->name_length = (uint8_t) name_length;
    rule->selector_length = (uint8_t) selector_length;
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
        rule->fast_key_length = selector_metadata.fast_key_length;
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

/* Retention and deferred analysis share ordinary CSS string/comment bounds. */
static bool declaration_span(const char *text, size_t length,
                             size_t *at, size_t *end)
{
    *at = skip_css_space_and_comments(text, length, *at);
    if (*at >= length) return false;
    *end = find_declaration_end(text, length, *at);
    return true;
}

/* Properties the cascade does not decode into ComputedStyle but retains as
   authored text for a later consumer (layout, getComputedStyle, the sparse
   "modern" resolver). This one table decides what is retained and what
   @supports reports as implemented; a name added here must also get a value
   validator in supports_retained_layout_value(). Vendor aliases are stored
   under their standard name. */
static const char *const style_retained_layout_properties[] = {
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
    "user-select", "touch-action", "text-size-adjust", "resize",
    "text-wrap", "text-wrap-style",
    "hyphens", "tab-size", "font-kerning", "text-rendering",
    "contain-intrinsic-size",
    "translate", "rotate", "scale", "isolation", "mix-blend-mode",
    "backface-visibility", "transform-style", "color-scheme",
    "border-image", "border-image-source", "border-image-slice",
    "border-image-width", "border-image-outset", "border-image-repeat",
    "backdrop-filter",
    "border-start-start-radius", "border-start-end-radius",
    "border-end-start-radius", "border-end-end-radius"
};

/* The stored (standard) name of a retained layout property, or NULL. */
static const char *retained_layout_property(const char *name,
                                            size_t name_length)
{
    static const struct { const char *alias; const char *standard; }
        aliases[] = {
            { "-webkit-user-select", "user-select" },
            { "-webkit-text-size-adjust", "text-size-adjust" },
            { "-webkit-backdrop-filter", "backdrop-filter" }
        };
    if (name_length > 8 && name[0] == '-') {
        for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
            if (span_case_equal(name, name_length, aliases[i].alias))
                return aliases[i].standard;
        }
        return NULL;
    }
    for (size_t i = 0;
         i < sizeof(style_retained_layout_properties)
             / sizeof(style_retained_layout_properties[0]); i++) {
        if (span_case_equal(name, name_length,
                            style_retained_layout_properties[i]))
            return style_retained_layout_properties[i];
    }
    return NULL;
}

static int transition_declaration_kind(const char *name, size_t length);
static bool add_transition_rule(Stylesheet *sheet, const char *selector,
                                size_t selector_length,
                                StyleTransitionKind kind, const char *value,
                                size_t value_length, unsigned specificity,
                                unsigned order, bool important,
                                PseudoElement pseudo);

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
    unsigned declaration_order = 0;
    for (size_t at = 0; at < length; declaration_order++) {
        size_t end;
        if (!declaration_span(declarations, length, &at, &end)) break;
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
            unsigned packed_order = (order <= (UINT_MAX >> 8)
                                         ? order << 8
                                         : UINT_MAX - UINT8_MAX)
                | (declaration_order > UINT8_MAX
                       ? UINT8_MAX : declaration_order);
            int transition_kind =
                transition_declaration_kind(name, name_length);
            if (transition_kind >= 0
                && !add_transition_rule(
                       sheet, selector, selector_length,
                       (StyleTransitionKind) transition_kind, value,
                       value_length, specificity, packed_order, important,
                       pseudo)) return false;
            bool retained = name_length > 2
                && name[0] == '-' && name[1] == '-';
            const char *stored_name = name;
            size_t stored_name_length = name_length;
            if (!retained
                && span_case_equal(name, name_length, "color-scheme")) {
                retained = true;
                stored_name = "color-scheme";
                stored_name_length = sizeof("color-scheme") - 1u;
            }
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
                const char *standard =
                    retained_layout_property(name, name_length);
                if (standard != NULL) {
                    retained = true;
                    stored_name = standard;
                    stored_name_length = strlen(standard);
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
                uint32_t modern = 0;
                if (span_case_equal(stored_name, stored_name_length,
                                    "user-select")) {
                    modern = STYLE_MODERN_USER_SELECT;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "touch-action")) {
                    modern = STYLE_MODERN_TOUCH_ACTION;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "text-size-adjust")) {
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
                                           "backface-visibility")) {
                    modern = STYLE_MODERN_BACKFACE_VISIBILITY;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "transform-style")) {
                    modern = STYLE_MODERN_TRANSFORM_STYLE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "color-scheme")) {
                    modern = STYLE_MODERN_COLOR_SCHEME;
                } else if (stored_name_length >= 12u
                           && strncasecmp(
                                  stored_name, "border-image", 12u) == 0) {
                    modern = STYLE_MODERN_BORDER_IMAGE;
                } else if (span_case_equal(stored_name, stored_name_length,
                                           "backdrop-filter")) {
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
                                    specificity, packed_order,
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
    bool all_inherit[2];
} StyleDeferredDeclarationAnalysis;

static void declarations_analyze_deferred_resolution(
    const char *text, size_t length,
    StyleDeferredDeclarationAnalysis *analysis)
{
    if (text == NULL || analysis == NULL) return;
    for (size_t at = 0; at < length;) {
        size_t end;
        if (!declaration_span(text, length, &at, &end)) break;
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
            if (important) {
                value_length = declaration_value_length(value, value_length);
                trim(&value, &value_length);
            }
            if (!(name_length > 2 && name[0] == '-' && name[1] == '-')) {
                if (span_case_equal(name, name_length, "all")) {
                    /* Last declaration at this importance wins. Unsupported
                       CSS-wide values remain an ordinary ignored declaration. */
                    analysis->all_inherit[slot] = span_case_equal(
                        value, value_length, "inherit");
                }
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
