static void experimental_record_phase(ExperimentalPhaseTiming *timing,
                                      uint64_t started_ns)
{
    uint64_t finished = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed = finished >= started_ns
        ? (finished - started_ns) / UINT64_C(1000) : 0;
    timing->last_us = elapsed;
    timing->total_us += elapsed;
    if (elapsed > timing->max_us) timing->max_us = elapsed;
}

typedef struct {
    TilefinchPlatformInput pending_input;
    bool input_pending;
    size_t input_polls;
    size_t asset_reads;
    size_t frames_presented;
    uint64_t last_frame_hash;
    const char *first_frame_output;
    bool first_frame_written;
} HostPlatformSimulator;

/* The BrowserEngine owns the browser-wide budget, session, navigation,
   controller, fonts, framebuffer, and render shell. The interactive frontend
   keeps non-owning aliases for its diagnostic views plus the experimental
   compressed-section state that has not moved behind the facade. */
typedef struct {
    BrowserEngine *engine;
    BrowserSession *engine_session;
    NavigationSession *engine_navigation;
    BrowserController *engine_controller;
    FontSet *engine_fonts;
    TileCache *engine_render;
    CompressedSectionStore section_store;
    SectionStoreStreamBuilder section_stream_builder;
    SectionRouteStream experimental_router;
    ExperimentalSectionContext experimental;
    FetchResult section_fetch;
} InteractiveApplicationContext;

static void experimental_bind_navigation(
    ExperimentalSectionContext *experimental,
    NavigationSession *navigation)
{
    experimental->navigation = navigation;
}

static HostPlatformSimulator *host_platform_simulator;

static bool stream_compressed_section_body(void *opaque,
                                           const unsigned char *data,
                                           size_t length)
{
    return section_store_stream_append(opaque, data, length);
}

static uint64_t experimental_initial_elapsed_us(
    const ExperimentalSectionContext *experimental)
{
    uint64_t now = tilefinch_platform_monotonic_time_ns();
    return experimental != NULL && experimental->initial_load_started_ns != 0
        && now >= experimental->initial_load_started_ns
        ? (now - experimental->initial_load_started_ns) / UINT64_C(1000) : 0;
}

static bool experimental_index_progress(
    void *opaque, const CompressedSectionStore *store,
    size_t indexed_source_bytes, bool final)
{
    ExperimentalSectionContext *experimental = opaque;
    if (experimental == NULL || store == NULL
        || experimental->navigation == NULL) return false;
    experimental->index_progress_callbacks++;
    /* The final notification is issued only after section construction and
       anchor sorting. Record that boundary before a last-section provisional
       paint so structural indexing and rendering remain separate metrics. */
    if (final) {
        experimental->index_complete_us =
            experimental_initial_elapsed_us(experimental);
    }
    if (!experimental->provisional_attempted
        && store->section_count > experimental->initial_section) {
        experimental->provisional_attempted = true;
        char *html = NULL;
        size_t html_length = 0;
        NavigationSession *navigation = experimental->navigation;
        bool scripts_enabled = navigation->scripts_enabled;
        bool document_scripts_enabled = navigation->document_scripts_enabled;
        bool external_resources_enabled =
            navigation->external_resources_enabled;
        navigation->scripts_enabled = false;
        navigation->document_scripts_enabled = false;
        navigation->external_resources_enabled = false;
        bool committed = section_store_extract_html(
                store, experimental->initial_section, &html, &html_length)
            && navigation_commit_html(
                navigation, experimental->initial_generation,
                experimental->locator, html, html_length, 480,
                experimental->initial_fonts, NULL, false);
        navigation->scripts_enabled = scripts_enabled;
        navigation->document_scripts_enabled = document_scripts_enabled;
        navigation->external_resources_enabled = external_resources_enabled;
        budget_free(experimental->budget, html);
        if (committed) {
            experimental->provisional_commit_us =
                experimental_initial_elapsed_us(experimental);
            size_t frame_pixels = 480u * 272u;
            uint16_t *frame = budget_malloc_category(
                experimental->budget, BUDGET_CATEGORY_RENDER,
                frame_pixels * sizeof(*frame));
            TileCache cache = {0};
            size_t tiles = experimental->initial_tile_capacity < 4
                ? experimental->initial_tile_capacity : 4;
            if (tiles == 0) tiles = 1;
            bool painted = frame != NULL
                && tile_cache_init(
                    &cache, experimental->budget,
                    &navigation->page.layout, tiles)
                && tile_cache_set_frame(&cache, frame, frame_pixels)
                && tile_cache_render_frame(&cache, 0, 480, 272, NULL);
            if (painted) {
                experimental->provisional_painted = true;
                experimental->first_paint_ready_us =
                    experimental_initial_elapsed_us(experimental);
                if (host_platform_simulator != NULL) {
                    experimental->provisional_presented =
                        tilefinch_platform_present_rgb565(
                            frame, 480, 272, 480);
                }
            }
            tile_cache_destroy(&cache);
            budget_free(experimental->budget, frame);
            navigation_discard_current_page(navigation);
        }
        printf("experimental-progressive-first-paint section=%zu "
               "indexed-bytes=%zu sections-ready=%zu commit=%s paint=%s "
               "present=%s elapsed-us=%llu\n",
               experimental->initial_section, indexed_source_bytes,
               store->section_count, committed ? "yes" : "no",
               experimental->provisional_painted ? "yes" : "no",
               experimental->provisional_presented ? "yes" : "no",
               (unsigned long long) experimental->first_paint_ready_us);
    }
    return true;
}

static void experimental_script_state_destroy(
    ExperimentalSectionContext *experimental)
{
    if (experimental == NULL || experimental->budget == NULL) return;
    budget_free(experimental->budget, experimental->executed_sections);
    experimental->executed_sections = NULL;
    experimental->executed_section_bytes = 0;
}

static void experimental_remote_mutation_destroy(
    ExperimentalSectionContext *experimental,
    ExperimentalRemoteMutation *mutation)
{
    if (experimental == NULL || experimental->budget == NULL
        || mutation == NULL) return;
    budget_free(experimental->budget, mutation->content);
    for (size_t i = 0; i < mutation->attribute_count; i++) {
        budget_free(experimental->budget, mutation->attributes[i].name);
        budget_free(experimental->budget, mutation->attributes[i].value);
    }
    budget_free(experimental->budget, mutation->attributes);
    memset(mutation, 0, sizeof(*mutation));
}

static void experimental_remote_mutations_destroy(
    ExperimentalSectionContext *experimental)
{
    if (experimental == NULL) return;
    for (size_t i = 0; i < EXPERIMENTAL_REMOTE_MUTATION_LIMIT; i++) {
        experimental_remote_mutation_destroy(
            experimental, &experimental->remote_mutations[i]);
    }
    experimental->remote_mutation_count = 0;
    experimental->remote_mutation_head = 0;
    experimental->body_source_replaced = false;
}

static bool experimental_script_state_init(
    ExperimentalSectionContext *experimental)
{
    if (experimental == NULL || experimental->store == NULL
        || experimental->budget == NULL
        || experimental->store->section_count == 0) return false;
    experimental_script_state_destroy(experimental);
    size_t bytes = (experimental->store->section_count + 7) / 8;
    experimental->executed_sections = budget_calloc(
        experimental->budget, bytes, 1);
    if (experimental->executed_sections == NULL) return false;
    experimental->executed_section_bytes = bytes;
    return true;
}

static bool experimental_section_scripts_executed(
    const ExperimentalSectionContext *experimental, size_t section)
{
    return experimental != NULL && experimental->executed_sections != NULL
        && section / 8 < experimental->executed_section_bytes
        && (experimental->executed_sections[section / 8]
            & (unsigned char) (1u << (section % 8))) != 0;
}

static bool experimental_mark_section_scripts_executed(
    ExperimentalSectionContext *experimental, size_t section)
{
    if (experimental == NULL || experimental->executed_sections == NULL
        || section / 8 >= experimental->executed_section_bytes) return false;
    experimental->executed_sections[section / 8] |=
        (unsigned char) (1u << (section % 8));
    return true;
}

static bool experimental_node_visible(void *opaque, size_t section_index,
                                      size_t element_ordinal,
                                      size_t node_ordinal,
                                      unsigned node_type)
{
    ExperimentalSectionContext *experimental = opaque;
    if (section_index == SCRIPT_NODE_VISIBILITY_DOCUMENT_BODY_SECTION) {
        return experimental == NULL || !experimental->script_frontier_active
            || experimental->script_body_visible;
    }
    return experimental == NULL || !experimental->script_frontier_active
        || section_index < experimental->script_visible_section
        || (section_index == experimental->script_visible_section
            && (node_type == LXB_DOM_NODE_TYPE_ELEMENT
                ? element_ordinal <= experimental->script_visible_ordinal
                : node_ordinal <= experimental->script_visible_node_ordinal));
}

static bool experimental_remote_element_lookup(
    void *opaque, const char *identifier, size_t length,
    size_t *section_index, char tag_name[32], char stable_key[96]);

static bool experimental_apply_remote_mutations(
    ExperimentalSectionContext *experimental, PocDocument *document,
    size_t section_index);
static lxb_dom_node_t *experimental_find_element_tag(
    lxb_dom_node_t *node, const char *wanted);

typedef struct {
    ExperimentalSectionContext *experimental;
    const char *selector;
    size_t selector_length;
    size_t section_index;
    TilefinchRemoteSelectorMatch *matches;
    size_t capacity;
    size_t count;
    size_t element_ordinal;
    bool in_source;
    bool saw_start;
    bool saw_end;
    bool overflow;
    bool first_only;
} ExperimentalSemanticQuery;

static bool experimental_selector_list_matches(lxb_dom_node_t *node,
                                                const char *selector,
                                                size_t length)
{
    size_t start = 0;
    int square = 0, round = 0;
    for (size_t i = 0; i <= length; i++) {
        char value = i < length ? selector[i] : ',';
        if (value == '[') square++;
        else if (value == ']' && square > 0) square--;
        else if (value == '(') round++;
        else if (value == ')' && round > 0) round--;
        if (value != ',' || square != 0 || round != 0) continue;
        size_t first = start, last = i;
        while (first < last
               && isspace((unsigned char) selector[first])) first++;
        while (last > first
               && isspace((unsigned char) selector[last - 1])) last--;
        if (last > first
            && style_selector_matches(node, selector + first,
                                      last - first)) return true;
        start = i + 1;
    }
    return false;
}

static bool experimental_comment_is(lxb_dom_node_t *node,
                                    const char *marker)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_COMMENT)
        return false;
    lxb_dom_comment_t *comment = lxb_dom_interface_comment(node);
    size_t length = comment->char_data.data.length;
    return strlen(marker) == length
        && memcmp(comment->char_data.data.data, marker, length) == 0;
}

static void experimental_semantic_query_walk(
    lxb_dom_node_t *node, ExperimentalSemanticQuery *query)
{
    for (; node != NULL && !query->overflow
                         && !(query->first_only && query->count != 0);
         node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            query->saw_start = true;
            query->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            query->saw_end = true;
            query->in_source = false;
            continue;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t ordinal = query->element_ordinal++;
            if (query->in_source
                && experimental_node_visible(
                       query->experimental, query->section_index, ordinal,
                       0, LXB_DOM_NODE_TYPE_ELEMENT)
                && experimental_selector_list_matches(
                       node, query->selector, query->selector_length)) {
                if (query->count == query->capacity) {
                    query->overflow = true;
                    return;
                }
                TilefinchRemoteSelectorMatch *match =
                    &query->matches[query->count++];
                memset(match, 0, sizeof(*match));
                match->section_index = query->section_index;
                size_t name_length = 0, id_length = 0;
                const char *name = document_element_name(node, &name_length);
                const char *id = document_attribute(node, "id", &id_length);
                if (name != NULL && name_length != 0 && name_length < 32) {
                    for (size_t i = 0; i < name_length; i++) {
                        match->tag_name[i] = (char) tolower(
                            (unsigned char) name[i]);
                    }
                    match->tag_name[name_length] = '\0';
                } else memcpy(match->tag_name, "div", 4);
                if (id != NULL && id_length != 0 && id_length <= 128) {
                    memcpy(match->identifier, id, id_length);
                    match->identifier[id_length] = '\0';
                }
                int written = snprintf(
                    match->stable_key, sizeof(match->stable_key),
                    "s:%zu:%zu:%s", query->section_index, ordinal,
                    match->tag_name);
                if (written <= 0
                    || (size_t) written >= sizeof(match->stable_key)) {
                    match->stable_key[0] = '\0';
                }
            }
        }
        experimental_semantic_query_walk(node->first_child, query);
    }
}

static bool experimental_semantic_selector_collect(
    ExperimentalSectionContext *experimental, const char *selector,
    size_t length, TilefinchRemoteSelectorMatch *matches, size_t capacity,
    size_t *match_count, bool first_only)
{
    if (experimental == NULL || experimental->store == NULL
        || selector == NULL || length == 0 || length > 512
        || matches == NULL || capacity == 0 || match_count == NULL)
        return false;
    experimental->semantic_queries++;
    uint64_t started_ns = tilefinch_platform_monotonic_time_ns();
    size_t sections_before = experimental->semantic_sections;
    size_t skipped_before = experimental->semantic_sections_skipped;
    size_t count = 0;
    bool success = false;
    bool can_reject_sections = experimental->remote_mutation_count == 0
        && section_store_selector_is_simple(selector, length);
    for (size_t section = 0;
         section < experimental->store->section_count; section++) {
        if (can_reject_sections
            && !section_store_simple_selector_maybe_matches_section(
                   experimental->store, section, selector, length)) {
            experimental->semantic_sections_skipped++;
            continue;
        }
        char *html = NULL;
        size_t html_length = 0;
        PocDocument document = {0};
        experimental->semantic_sections++;
        if (!section_store_extract_query_html(
                experimental->store, section, &html, &html_length)
            || !document_parse(&document, experimental->budget, html,
                               html_length, 4096)
            || !experimental_apply_remote_mutations(
                   experimental, &document, section)) {
            budget_free(experimental->budget, html);
            document_destroy(&document);
            experimental->semantic_failures++;
            goto finished;
        }
        ExperimentalSemanticQuery query = {
            .experimental = experimental,
            .selector = selector,
            .selector_length = length,
            .section_index = section,
            .matches = matches,
            .capacity = capacity,
            .count = count,
            .first_only = first_only
        };
        experimental_semantic_query_walk(
            lxb_dom_interface_node(document.html), &query);
        count = query.count;
        bool valid = query.saw_start
            && (query.saw_end || (first_only && count != 0))
            && !query.overflow;
        document_destroy(&document);
        budget_free(experimental->budget, html);
        if (!valid) {
            experimental->semantic_failures++;
            goto finished;
        }
        if (!tilefinch_platform_cooperate(
                "section-query",
                experimental->semantic_sections - sections_before)) {
            experimental->semantic_failures++;
            goto finished;
        }
        if (first_only && count != 0) break;
    }
    experimental->semantic_matches += count;
    *match_count = count;
    success = true;

finished:
    {
        uint64_t finished_ns = tilefinch_platform_monotonic_time_ns();
        uint64_t elapsed_us = finished_ns >= started_ns
            ? (finished_ns - started_ns) / UINT64_C(1000) : 0;
        experimental->semantic_total_us += elapsed_us;
        if (elapsed_us > experimental->semantic_max_us)
            experimental->semantic_max_us = elapsed_us;
        char display[97];
        size_t used = 0;
        for (size_t i = 0; i < length && used + 1 < sizeof(display); i++) {
            unsigned char byte = (unsigned char) selector[i];
            display[used++] = isprint(byte) && byte != '"' && byte != '\\'
                ? (char) byte : '?';
        }
        display[used] = '\0';
        printf("experimental-semantic-query selector=\"%s%s\" length=%zu "
               "mode=%s simple=%s sections=%zu skipped=%zu matches=%zu "
               "elapsed-us=%llu status=%s\n",
               display, used < length ? "..." : "", length,
               first_only ? "first" : "all",
               can_reject_sections ? "yes" : "no",
               experimental->semantic_sections - sections_before,
               experimental->semantic_sections_skipped - skipped_before,
               count, (unsigned long long) elapsed_us,
               success ? "ok" : "failed");
    }
    return success;
}

typedef struct {
    ExperimentalSectionContext *experimental;
    const char *identifier;
    size_t identifier_length;
    size_t section_index;
    size_t element_ordinal;
    bool in_source;
    bool saw_start;
    bool saw_end;
    bool found;
    TilefinchRemoteSelectorMatch match;
} ExperimentalExactIdQuery;

static void experimental_exact_id_walk(lxb_dom_node_t *node,
                                       ExperimentalExactIdQuery *query)
{
    for (; node != NULL && !query->found; node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            query->saw_start = true;
            query->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            query->saw_end = true;
            query->in_source = false;
            continue;
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t ordinal = query->element_ordinal++;
            size_t id_length = 0;
            const char *id = document_attribute(node, "id", &id_length);
            if (query->in_source && id != NULL
                && experimental_node_visible(
                       query->experimental, query->section_index, ordinal,
                       0, LXB_DOM_NODE_TYPE_ELEMENT)
                && id_length == query->identifier_length
                && memcmp(id, query->identifier, id_length) == 0) {
                TilefinchRemoteSelectorMatch *match = &query->match;
                memset(match, 0, sizeof(*match));
                match->section_index = query->section_index;
                size_t tag_length = 0;
                const char *tag = document_element_name(node, &tag_length);
                if (tag == NULL || tag_length == 0 || tag_length >= 32) {
                    memcpy(match->tag_name, "div", 4);
                } else {
                    for (size_t i = 0; i < tag_length; i++) {
                        match->tag_name[i] = (char) tolower(
                            (unsigned char) tag[i]);
                    }
                    match->tag_name[tag_length] = '\0';
                }
                memcpy(match->identifier, id, id_length);
                match->identifier[id_length] = '\0';
                int written = snprintf(
                    match->stable_key, sizeof(match->stable_key),
                    "s:%zu:%zu:%s", query->section_index, ordinal,
                    match->tag_name);
                query->found = written > 0
                    && (size_t) written < sizeof(match->stable_key);
                if (!query->found) match->stable_key[0] = '\0';
            }
        }
        experimental_exact_id_walk(node->first_child, query);
    }
}

static bool experimental_remote_element_lookup(
    void *opaque, const char *identifier, size_t length,
    size_t *section_index, char tag_name[32], char stable_key[96])
{
    ExperimentalSectionContext *experimental = opaque;
    if (stable_key != NULL) stable_key[0] = '\0';
    if (experimental == NULL || experimental->store == NULL
        || experimental->body_source_replaced
        || identifier == NULL || length == 0 || length > 128
        || section_index == NULL || tag_name == NULL) return false;
    size_t first_section = 0;
    size_t last_section = experimental->store->section_count;
    if (experimental->remote_mutation_count == 0) {
        char indexed_tag[32] = {0};
        if (!section_store_find_anchor_element(
                experimental->store, identifier, length,
                &first_section, indexed_tag)) return false;
        last_section = first_section + 1;
    }
    for (size_t section = first_section; section < last_section; section++) {
        char *html = NULL;
        size_t html_length = 0;
        PocDocument document = {0};
        if (!section_store_extract_query_html(
                experimental->store, section, &html, &html_length)
            || !document_parse(&document, experimental->budget, html,
                               html_length, 4096)
            || !experimental_apply_remote_mutations(
                   experimental, &document, section)) {
            document_destroy(&document);
            budget_free(experimental->budget, html);
            experimental->semantic_failures++;
            return false;
        }
        ExperimentalExactIdQuery query = {
            .experimental = experimental,
            .identifier = identifier,
            .identifier_length = length,
            .section_index = section
        };
        experimental_exact_id_walk(
            lxb_dom_interface_node(document.html), &query);
        bool valid = query.saw_start && (query.saw_end || query.found);
        document_destroy(&document);
        budget_free(experimental->budget, html);
        if (!valid) {
            experimental->semantic_failures++;
            return false;
        }
        if (!query.found) continue;
        *section_index = query.match.section_index;
        memcpy(tag_name, query.match.tag_name, sizeof(query.match.tag_name));
        if (stable_key != NULL) {
            memcpy(stable_key, query.match.stable_key,
                   sizeof(query.match.stable_key));
        }
        return true;
    }
    return false;
}

static bool experimental_remote_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    ExperimentalSectionContext *experimental = opaque;
    if (stable_key != NULL) stable_key[0] = '\0';
    if (experimental == NULL || experimental->store == NULL
        || experimental->body_source_replaced) return false;
    TilefinchRemoteSelectorMatch match;
    size_t count = 0;
    if (!experimental_semantic_selector_collect(
            experimental, selector, length, &match, 1, &count, true)
        || count == 0) return false;
    *section_index = match.section_index;
    memcpy(tag_name, match.tag_name, sizeof(match.tag_name));
    memcpy(identifier, match.identifier, sizeof(match.identifier));
    if (stable_key != NULL)
        memcpy(stable_key, match.stable_key, sizeof(match.stable_key));
    return true;
}

static bool experimental_remote_selector_collect(
    void *opaque, const char *selector, size_t length,
    TilefinchRemoteSelectorMatch *matches, size_t capacity, size_t *match_count)
{
    ExperimentalSectionContext *experimental = opaque;
    if (experimental == NULL || experimental->store == NULL
        || experimental->body_source_replaced
        || experimental->store->section_count <= 1) return false;
    return experimental_semantic_selector_collect(
        experimental, selector, length, matches, capacity,
        match_count, false);
}

typedef struct {
    ExperimentalSectionContext *experimental;
    unsigned root_special;
    int what_to_show;
    size_t section_index;
    TilefinchRemoteTraversalNode *nodes;
    size_t capacity;
    size_t count;
    size_t element_ordinal;
    size_t node_ordinal;
    bool in_source;
    bool saw_start;
    bool saw_end;
} ExperimentalTraversalQuery;

static unsigned experimental_node_special(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return 0;
    size_t length = 0;
    const char *tag = document_element_name(node, &length);
    if (tag == NULL) return 0;
    if (length == 4 && strncasecmp(tag, "html", 4) == 0) return 1;
    if (length == 4 && strncasecmp(tag, "head", 4) == 0) return 2;
    if (length == 4 && strncasecmp(tag, "body", 4) == 0) return 3;
    return 0;
}

static bool experimental_node_within_traversal_root(
    lxb_dom_node_t *node, unsigned root_special)
{
    if (root_special == 1) return true;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (experimental_node_special(at) == root_special) return true;
    }
    return false;
}

static void experimental_traversal_walk(
    lxb_dom_node_t *node, ExperimentalTraversalQuery *query)
{
    for (; node != NULL && query->count < query->capacity;
         node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            query->saw_start = true;
            query->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            query->saw_end = true;
            query->in_source = false;
            continue;
        }
        size_t node_ordinal = SIZE_MAX;
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            node_ordinal = query->node_ordinal++;
        }
        size_t element_ordinal = SIZE_MAX;
        unsigned special = 0;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            element_ordinal = query->element_ordinal++;
            special = experimental_node_special(node);
        }
        bool type_visible =
            (node->type == LXB_DOM_NODE_TYPE_ELEMENT
             && (query->what_to_show & 1) != 0)
            || (node->type == LXB_DOM_NODE_TYPE_TEXT
                && (query->what_to_show & 4) != 0)
            || (node->type == LXB_DOM_NODE_TYPE_COMMENT
                && (query->what_to_show & 128) != 0);
        bool structural = query->section_index == 0
            && query->root_special == 1 && (special == 2 || special == 3);
        bool in_root = experimental_node_within_traversal_root(
            node, query->root_special);
        if (type_visible && in_root && special != query->root_special
            && (query->in_source || structural)
            && experimental_node_visible(
                   query->experimental, query->section_index,
                   element_ordinal, node_ordinal, (unsigned) node->type)) {
            TilefinchRemoteTraversalNode *result =
                &query->nodes[query->count];
            memset(result, 0, sizeof(*result));
            result->section_index = query->section_index;
            result->node_type = (unsigned) node->type;
            result->special = special;
            int written = 0;
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                size_t tag_length = 0;
                const char *tag = document_element_name(node, &tag_length);
                if (tag == NULL || tag_length == 0 || tag_length >= 32)
                    return;
                for (size_t i = 0; i < tag_length; i++) {
                    result->tag_name[i] = (char) tolower(
                        (unsigned char) tag[i]);
                }
                result->tag_name[tag_length] = '\0';
                written = snprintf(
                    result->stable_key, sizeof(result->stable_key),
                    "s:%zu:%zu:%s", query->section_index,
                    element_ordinal, result->tag_name);
            } else {
                written = snprintf(
                    result->stable_key, sizeof(result->stable_key),
                    "n:%zu:%zu:%u", query->section_index, node_ordinal,
                    (unsigned) node->type);
            }
            if (written <= 0
                || (size_t) written >= sizeof(result->stable_key)) return;
            query->count++;
        }
        experimental_traversal_walk(node->first_child, query);
    }
}

static bool experimental_remote_descendant_collect(
    void *opaque, unsigned root_special, int what_to_show,
    TilefinchRemoteTraversalNode *nodes, size_t capacity, size_t *node_count)
{
    ExperimentalSectionContext *experimental = opaque;
    if (node_count != NULL) *node_count = 0;
    if (experimental == NULL || experimental->store == NULL
        || experimental->body_source_replaced
        || nodes == NULL || capacity == 0 || node_count == NULL
        || (root_special != 1 && root_special != 3)) return false;
    ExperimentalTraversalQuery query = {
        .experimental = experimental,
        .root_special = root_special,
        .what_to_show = what_to_show,
        .nodes = nodes,
        .capacity = capacity
    };
    if ((what_to_show & 1) != 0) {
        TilefinchRemoteTraversalNode *root = &nodes[query.count++];
        root->section_index = 0;
        root->node_type = LXB_DOM_NODE_TYPE_ELEMENT;
        root->special = root_special;
        memcpy(root->tag_name, root_special == 1 ? "html" : "body", 5);
    }
    for (size_t section = 0;
         section < experimental->store->section_count
         && query.count < capacity; section++) {
        char *html = NULL;
        size_t html_length = 0;
        PocDocument document = {0};
        query.section_index = section;
        query.element_ordinal = 0;
        query.node_ordinal = 0;
        query.in_source = false;
        query.saw_start = false;
        query.saw_end = false;
        bool success = section_store_extract_query_html(
                experimental->store, section, &html, &html_length)
            && document_parse(&document, experimental->budget, html,
                              html_length, 4096)
            && experimental_apply_remote_mutations(
                   experimental, &document, section);
        if (success) {
            experimental_traversal_walk(
                lxb_dom_interface_node(document.html), &query);
            success = query.saw_start
                && (query.saw_end || query.count == capacity);
        }
        document_destroy(&document);
        budget_free(experimental->budget, html);
        if (!success) return false;
    }
    *node_count = query.count;
    return true;
}

typedef enum {
    EXPERIMENTAL_REMOTE_KEY_ID = 0,
    EXPERIMENTAL_REMOTE_KEY_SELECTOR,
    EXPERIMENTAL_REMOTE_KEY_STABLE,
    EXPERIMENTAL_REMOTE_KEY_CONTEXT,
    EXPERIMENTAL_REMOTE_KEY_NODE
} ExperimentalRemoteKeyKind;

typedef struct {
    ExperimentalRemoteKeyKind kind;
    const char *value;
    size_t value_length;
    const char *tag;
    size_t tag_length;
    size_t section;
    size_t ordinal;
    size_t element_ordinal;
    size_t node_ordinal;
    unsigned node_type;
    bool in_source;
    bool saw_start;
    bool saw_end;
    lxb_dom_node_t *found;
} ExperimentalRemoteNodeFind;

static bool experimental_parse_key_number(const char **cursor,
                                          const char *end,
                                          char delimiter,
                                          size_t *value)
{
    const char *at = *cursor;
    if (at == end || !isdigit((unsigned char) *at)) return false;
    size_t parsed = 0;
    while (at < end && isdigit((unsigned char) *at)) {
        unsigned digit = (unsigned) (*at - '0');
        if (parsed > (SIZE_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        at++;
    }
    if (at == end || *at != delimiter) return false;
    *cursor = at + 1;
    *value = parsed;
    return true;
}

static bool experimental_parse_key_number_end(const char **cursor,
                                              const char *end,
                                              size_t *value)
{
    const char *at = *cursor;
    if (at == end || !isdigit((unsigned char) *at)) return false;
    size_t parsed = 0;
    while (at < end && isdigit((unsigned char) *at)) {
        unsigned digit = (unsigned) (*at - '0');
        if (parsed > (SIZE_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        at++;
    }
    if (at != end) return false;
    *cursor = at;
    *value = parsed;
    return true;
}

static bool experimental_parse_remote_key(
    const char *key, size_t length, size_t section,
    ExperimentalRemoteNodeFind *find)
{
    if (key == NULL || find == NULL || length <= 2 || key[1] != ':')
        return false;
    memset(find, 0, sizeof(*find));
    find->section = section;
    if (key[0] == 'i') {
        find->kind = EXPERIMENTAL_REMOTE_KEY_ID;
        find->value = key + 2;
        find->value_length = length - 2;
        return find->value_length != 0 && find->value_length <= 128;
    }
    const char *cursor = key + 2;
    const char *end = key + length;
    size_t keyed_section = 0;
    if (!experimental_parse_key_number(
            &cursor, end, ':', &keyed_section)
        || keyed_section != section) return false;
    if (key[0] == 'q') {
        find->kind = EXPERIMENTAL_REMOTE_KEY_SELECTOR;
        find->value = cursor;
        find->value_length = (size_t) (end - cursor);
        return find->value_length != 0 && find->value_length <= 512;
    }
    if (key[0] == 'n') {
        size_t node_type = 0;
        if (!experimental_parse_key_number(
                &cursor, end, ':', &find->ordinal)
            || !experimental_parse_key_number_end(
                   &cursor, end, &node_type)
            || node_type == 0 || node_type > UINT_MAX) return false;
        find->kind = EXPERIMENTAL_REMOTE_KEY_NODE;
        find->node_type = (unsigned) node_type;
        return true;
    }
    if ((key[0] != 's' && key[0] != 'c')
        || !experimental_parse_key_number(
               &cursor, end, ':', &find->ordinal)) return false;
    find->kind = key[0] == 's' ? EXPERIMENTAL_REMOTE_KEY_STABLE
                                : EXPERIMENTAL_REMOTE_KEY_CONTEXT;
    find->tag = cursor;
    find->tag_length = (size_t) (end - cursor);
    return find->tag_length != 0 && find->tag_length < 32;
}

static bool experimental_remote_node_matches(
    lxb_dom_node_t *node, const ExperimentalRemoteNodeFind *find,
    size_t ordinal)
{
    if (find->kind == EXPERIMENTAL_REMOTE_KEY_ID) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        return id != NULL && length == find->value_length
            && memcmp(id, find->value, length) == 0;
    }
    if (find->kind == EXPERIMENTAL_REMOTE_KEY_SELECTOR) {
        return experimental_selector_list_matches(
            node, find->value, find->value_length);
    }
    if (ordinal != find->ordinal) return false;
    size_t length = 0;
    const char *tag = document_element_name(node, &length);
    return tag != NULL && length == find->tag_length
        && strncasecmp(tag, find->tag, length) == 0;
}

static void experimental_remote_node_find_walk(
    lxb_dom_node_t *node, ExperimentalRemoteNodeFind *find)
{
    for (; node != NULL && find->found == NULL; node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            find->saw_start = true;
            find->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            find->saw_end = true;
            find->in_source = false;
            continue;
        }
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            size_t ordinal = find->node_ordinal++;
            if (find->kind == EXPERIMENTAL_REMOTE_KEY_NODE
                && find->in_source && ordinal == find->ordinal
                && (unsigned) node->type == find->node_type) {
                find->found = node;
                return;
            }
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t ordinal = find->element_ordinal++;
            if ((find->in_source
                 || find->kind == EXPERIMENTAL_REMOTE_KEY_CONTEXT)
                && experimental_remote_node_matches(node, find, ordinal)) {
                find->found = node;
                return;
            }
        }
        experimental_remote_node_find_walk(node->first_child, find);
    }
}

static ExperimentalRemoteMutation *experimental_remote_mutation_find(
    ExperimentalSectionContext *experimental, const char *stable_key,
    size_t stable_key_length, size_t section_index)
{
    if (experimental == NULL || stable_key == NULL) return NULL;
    for (size_t i = 0; i < EXPERIMENTAL_REMOTE_MUTATION_LIMIT; i++) {
        ExperimentalRemoteMutation *mutation =
            &experimental->remote_mutations[i];
        if (mutation->used && mutation->section_index == section_index
            && mutation->stable_key_length == stable_key_length
            && memcmp(mutation->stable_key, stable_key,
                      stable_key_length) == 0) return mutation;
    }
    return NULL;
}

static ExperimentalRemoteMutation *experimental_remote_mutation_create(
    ExperimentalSectionContext *experimental, const char *stable_key,
    size_t stable_key_length, size_t section_index)
{
    if (experimental == NULL || stable_key == NULL
        || stable_key_length == 0 || stable_key_length > 192) return NULL;
    size_t index = 0;
    if (experimental->remote_mutation_count
        < EXPERIMENTAL_REMOTE_MUTATION_LIMIT) {
        index = (experimental->remote_mutation_head
                 + experimental->remote_mutation_count)
                % EXPERIMENTAL_REMOTE_MUTATION_LIMIT;
        experimental->remote_mutation_count++;
    } else {
        index = experimental->remote_mutation_head;
        experimental_remote_mutation_destroy(
            experimental, &experimental->remote_mutations[index]);
        experimental->remote_mutation_head =
            (experimental->remote_mutation_head + 1)
            % EXPERIMENTAL_REMOTE_MUTATION_LIMIT;
        experimental->remote_mutation_evictions++;
    }
    ExperimentalRemoteMutation *mutation =
        &experimental->remote_mutations[index];
    mutation->used = true;
    mutation->stable_key_length = stable_key_length;
    mutation->section_index = section_index;
    memcpy(mutation->stable_key, stable_key, stable_key_length);
    mutation->stable_key[stable_key_length] = '\0';
    return mutation;
}

static bool experimental_remote_value_copy(
    ExperimentalSectionContext *experimental, const char *value,
    size_t value_length, char **copy)
{
    *copy = NULL;
    if (value_length == 0) return true;
    *copy = budget_malloc_category(
        experimental->budget, BUDGET_CATEGORY_RESOURCE, value_length + 1);
    if (*copy == NULL) return false;
    memcpy(*copy, value, value_length);
    (*copy)[value_length] = '\0';
    return true;
}

static ExperimentalRemoteAttributeMutation *
experimental_remote_attribute_find(ExperimentalRemoteMutation *mutation,
                                   const char *name, size_t name_length)
{
    if (mutation == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < mutation->attribute_count; i++) {
        ExperimentalRemoteAttributeMutation *attribute =
            &mutation->attributes[i];
        if (attribute->name_length == name_length
            && strncasecmp(attribute->name, name, name_length) == 0) {
            return attribute;
        }
    }
    return NULL;
}

static bool experimental_remote_node_write(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeWriteKind kind,
    const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    ExperimentalSectionContext *experimental = opaque;
    bool body_root = stable_key != NULL && stable_key_length == 6
        && memcmp(stable_key, "d:body", 6) == 0;
    bool html_root = stable_key != NULL && stable_key_length == 6
        && memcmp(stable_key, "d:html", 6) == 0;
    bool head_root = stable_key != NULL && stable_key_length == 6
        && memcmp(stable_key, "d:head", 6) == 0;
    bool document_root = body_root || html_root || head_root;
    if (experimental == NULL || experimental->store == NULL
        || experimental->budget == NULL || stable_key == NULL
        || stable_key_length == 0 || stable_key_length > 192
        || section_index >= experimental->store->section_count
        || kind < SCRIPT_REMOTE_NODE_WRITE_TEXT
        || kind > SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE
        || (kind != SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE
            && value == NULL)
        || value_length > EXPERIMENTAL_REMOTE_INNER_HTML_LIMIT
        || ((kind == SCRIPT_REMOTE_NODE_WRITE_SET_ATTRIBUTE
             || kind == SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE)
            && (name == NULL || name_length == 0 || name_length > 128))) {
        if (experimental != NULL) experimental->remote_write_failures++;
        return false;
    }
    ExperimentalRemoteNodeFind parsed;
    if ((!document_root && experimental->body_source_replaced)
        || (!document_root && !experimental_parse_remote_key(
                stable_key, stable_key_length, section_index, &parsed))) {
        experimental->remote_write_failures++;
        return false;
    }
    char *value_copy = NULL;
    if (kind != SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE
        && !experimental_remote_value_copy(
               experimental, value, value_length, &value_copy)) {
        experimental->remote_write_failures++;
        return false;
    }
    size_t mutation_section = document_root ? SIZE_MAX : section_index;
    ExperimentalRemoteMutation *mutation = experimental_remote_mutation_find(
        experimental, stable_key, stable_key_length, mutation_section);
    if (kind == SCRIPT_REMOTE_NODE_WRITE_TEXT
        || kind == SCRIPT_REMOTE_NODE_WRITE_INNER_HTML) {
        if (mutation == NULL) {
            mutation = experimental_remote_mutation_create(
                experimental, stable_key, stable_key_length,
                mutation_section);
        }
        if (mutation == NULL) {
            budget_free(experimental->budget, value_copy);
            experimental->remote_write_failures++;
            return false;
        }
        budget_free(experimental->budget, mutation->content);
        mutation->content = value_copy;
        mutation->content_length = value_length;
        mutation->content_kind = kind;
        mutation->content_written = true;
    } else {
        ExperimentalRemoteAttributeMutation *attribute =
            experimental_remote_attribute_find(
                mutation, name, name_length);
        char *name_copy = NULL;
        ExperimentalRemoteAttributeMutation *new_attributes = NULL;
        size_t new_capacity = 0;
        if (attribute == NULL) {
            if (mutation != NULL
                && mutation->attribute_count
                   >= EXPERIMENTAL_REMOTE_ATTRIBUTE_MUTATION_LIMIT) {
                budget_free(experimental->budget, value_copy);
                experimental->remote_write_failures++;
                return false;
            }
            if (!experimental_remote_value_copy(
                    experimental, name, name_length, &name_copy)) {
                budget_free(experimental->budget, value_copy);
                experimental->remote_write_failures++;
                return false;
            }
            for (size_t i = 0; i < name_length; i++) {
                name_copy[i] = (char) tolower((unsigned char) name_copy[i]);
            }
            size_t count = mutation == NULL ? 0 : mutation->attribute_count;
            size_t capacity = mutation == NULL
                              ? 0 : mutation->attribute_capacity;
            if (count == capacity) {
                new_capacity = capacity == 0 ? 4 : capacity * 2;
                if (new_capacity
                    > EXPERIMENTAL_REMOTE_ATTRIBUTE_MUTATION_LIMIT) {
                    new_capacity = EXPERIMENTAL_REMOTE_ATTRIBUTE_MUTATION_LIMIT;
                }
                if (mutation == NULL) {
                    new_attributes = budget_calloc_category(
                        experimental->budget, BUDGET_CATEGORY_RESOURCE,
                        new_capacity, sizeof(*new_attributes));
                } else {
                    new_attributes = budget_realloc_category(
                        experimental->budget, BUDGET_CATEGORY_RESOURCE,
                        mutation->attributes,
                        new_capacity * sizeof(*new_attributes));
                }
                if (new_attributes == NULL) {
                    budget_free(experimental->budget, name_copy);
                    budget_free(experimental->budget, value_copy);
                    experimental->remote_write_failures++;
                    return false;
                }
                if (new_capacity > capacity) {
                    memset(new_attributes + capacity, 0,
                           (new_capacity - capacity)
                           * sizeof(*new_attributes));
                }
                if (mutation != NULL) {
                    mutation->attributes = new_attributes;
                    mutation->attribute_capacity = new_capacity;
                }
            }
            if (mutation == NULL) {
                mutation = experimental_remote_mutation_create(
                    experimental, stable_key, stable_key_length,
                    mutation_section);
                if (mutation == NULL) {
                    budget_free(experimental->budget, new_attributes);
                    budget_free(experimental->budget, name_copy);
                    budget_free(experimental->budget, value_copy);
                    experimental->remote_write_failures++;
                    return false;
                }
                mutation->attributes = new_attributes;
                mutation->attribute_capacity = new_capacity;
            }
            attribute = &mutation->attributes[mutation->attribute_count++];
            attribute->name = name_copy;
            attribute->name_length = name_length;
        }
        budget_free(experimental->budget, attribute->value);
        attribute->value = value_copy;
        attribute->value_length = value_length;
        attribute->removed =
            kind == SCRIPT_REMOTE_NODE_WRITE_REMOVE_ATTRIBUTE;
    }
    experimental->remote_writes++;
    experimental->remote_write_bytes += stable_key_length + name_length
                                        + value_length;
    if ((body_root || html_root)
        && (kind == SCRIPT_REMOTE_NODE_WRITE_TEXT
            || kind == SCRIPT_REMOTE_NODE_WRITE_INNER_HTML)) {
        experimental->body_source_replaced = true;
        experimental->remote_geometry_cache_valid = false;
        printf("experimental-root-mutation root=%s mode=%s bytes=%zu\n",
               body_root ? "body" : "html",
               kind == SCRIPT_REMOTE_NODE_WRITE_TEXT ? "text" : "html",
               value_length);
    }
    return true;
}

static bool experimental_apply_remote_mutations_unscoped(
    ExperimentalSectionContext *experimental, PocDocument *document,
    size_t section_index)
{
    if (experimental == NULL || document == NULL
        || document->html == NULL) return false;
    ExperimentalRemoteMutation *mutations[EXPERIMENTAL_REMOTE_MUTATION_LIMIT];
    lxb_dom_node_t *targets[EXPERIMENTAL_REMOTE_MUTATION_LIMIT] = {0};
    bool suppressed[EXPERIMENTAL_REMOTE_MUTATION_LIMIT] = {0};
    size_t count = 0;
    for (size_t offset = 0;
         offset < experimental->remote_mutation_count; offset++) {
        size_t index = (experimental->remote_mutation_head + offset)
                       % EXPERIMENTAL_REMOTE_MUTATION_LIMIT;
        ExperimentalRemoteMutation *mutation =
            &experimental->remote_mutations[index];
        if (!mutation->used || mutation->section_index != section_index)
            continue;
        ExperimentalRemoteNodeFind find;
        if (!experimental_parse_remote_key(
                mutation->stable_key, mutation->stable_key_length,
                section_index, &find)) return false;
        experimental_remote_node_find_walk(
            lxb_dom_interface_node(document->html), &find);
        if (!find.saw_start) {
            if (!experimental_parse_remote_key(
                    mutation->stable_key, mutation->stable_key_length,
                    section_index, &find)) return false;
            find.saw_start = true;
            find.in_source = true;
            experimental_remote_node_find_walk(
                lxb_dom_interface_node(document->html), &find);
        }
        if (find.found == NULL) continue;
        mutations[count] = mutation;
        targets[count] = find.found;
        if (getenv("TILEFINCH_TRACE_REMOTE_MUTATIONS") != NULL) {
            size_t id_length = 0, tag_length = 0;
            const char *id = document_attribute(
                find.found, "id", &id_length);
            const char *tag = document_element_name(
                find.found, &tag_length);
            printf("experimental-remote-mutation-map section=%zu key=\"%s\" "
                   "tag=\"%.*s\" id=\"%.*s\" content=%s attrs=%zu\n",
                   section_index, mutation->stable_key,
                   (int) tag_length, tag == NULL ? "" : tag,
                   (int) id_length, id == NULL ? "" : id,
                   mutation->content_written ? "yes" : "no",
                   mutation->attribute_count);
        }
        count++;
    }
    static const char *root_keys[] = {"d:html", "d:head", "d:body"};
    lxb_dom_node_t *root_nodes[] = {
        experimental_find_element_tag(
            lxb_dom_interface_node(document->html), "html"),
        experimental_find_element_tag(
            lxb_dom_interface_node(document->html), "head"),
        document_body_node(document)
    };
    for (size_t root = 0; root < 3
         && count < EXPERIMENTAL_REMOTE_MUTATION_LIMIT; root++) {
        ExperimentalRemoteMutation *root_mutation =
            experimental_remote_mutation_find(
                experimental, root_keys[root], 6, SIZE_MAX);
        if (root_mutation != NULL && root_nodes[root] != NULL) {
            mutations[count] = root_mutation;
            targets[count] = root_nodes[root];
            count++;
        }
    }
    for (size_t i = 0; i < count; i++) {
        for (lxb_dom_node_t *ancestor = targets[i]->parent;
             ancestor != NULL && !suppressed[i]; ancestor = ancestor->parent) {
            for (size_t j = 0; j < count; j++) {
                if (targets[j] == ancestor && mutations[j]->content_written) {
                    suppressed[i] = true;
                    break;
                }
            }
        }
    }
    static const lxb_char_t empty[] = "";
    for (size_t i = 0; i < count; i++) {
        if (suppressed[i]
            || targets[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *element = lxb_dom_interface_element(targets[i]);
        for (size_t j = 0; j < mutations[i]->attribute_count; j++) {
            ExperimentalRemoteAttributeMutation *attribute =
                &mutations[i]->attributes[j];
            if (attribute->removed) {
                if (lxb_dom_element_remove_attribute(
                        element, (const lxb_char_t *) attribute->name,
                        attribute->name_length) != LXB_STATUS_OK) return false;
            } else if (lxb_dom_element_set_attribute(
                           element, (const lxb_char_t *) attribute->name,
                           attribute->name_length,
                           attribute->value == NULL ? empty
                               : (const lxb_char_t *) attribute->value,
                           attribute->value_length) == NULL) return false;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (suppressed[i] || !mutations[i]->content_written) continue;
        const lxb_char_t *content = mutations[i]->content == NULL ? empty
            : (const lxb_char_t *) mutations[i]->content;
        if (mutations[i]->content_kind == SCRIPT_REMOTE_NODE_WRITE_TEXT) {
            if (lxb_dom_node_text_content_set(
                    targets[i], content,
                    mutations[i]->content_length) != LXB_STATUS_OK) {
                return false;
            }
        } else if (targets[i]->type != LXB_DOM_NODE_TYPE_ELEMENT
                   || lxb_html_element_inner_html_set(
                          lxb_html_interface_element(targets[i]), content,
                          mutations[i]->content_length) == NULL) return false;
    }
    return true;
}

static bool experimental_apply_remote_mutations(
    ExperimentalSectionContext *experimental, PocDocument *document,
    size_t section_index)
{
    BudgetAllocationOwner previous =
        document_allocation_owner_enter(document);
    bool applied = experimental_apply_remote_mutations_unscoped(
        experimental, document, section_index);
    document_allocation_owner_leave(document, previous);
    return applied;
}

static bool experimental_section_has_remote_mutations(
    const ExperimentalSectionContext *experimental, size_t section_index)
{
    if (experimental == NULL) return false;
    for (size_t i = 0; i < EXPERIMENTAL_REMOTE_MUTATION_LIMIT; i++) {
        if (experimental->remote_mutations[i].used
            && (experimental->remote_mutations[i].section_index
                   == section_index
                || experimental->remote_mutations[i].section_index
                   == SIZE_MAX)) return true;
    }
    return false;
}

typedef struct {
    lxb_dom_node_t *target;
    size_t element_ordinal;
    size_t node_ordinal;
    size_t target_ordinal;
    size_t target_node_ordinal;
    bool in_source;
    bool target_in_source;
    bool found;
} ExperimentalRemoteOrdinalFind;

static void experimental_remote_ordinal_walk(
    lxb_dom_node_t *node, ExperimentalRemoteOrdinalFind *find)
{
    for (; node != NULL && !find->found; node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            find->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            find->in_source = false;
            continue;
        }
        size_t node_ordinal = SIZE_MAX;
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            node_ordinal = find->node_ordinal++;
            if (node == find->target
                && node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
                find->target_node_ordinal = node_ordinal;
                find->target_in_source = find->in_source;
                find->found = true;
                return;
            }
        }
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t ordinal = find->element_ordinal++;
            if (node == find->target) {
                find->target_ordinal = ordinal;
                find->target_node_ordinal = node_ordinal;
                find->target_in_source = find->in_source;
                find->found = true;
                return;
            }
        }
        experimental_remote_ordinal_walk(node->first_child, find);
    }
}

static size_t experimental_remote_subtree_node_count(lxb_dom_node_t *node)
{
    if (node == NULL) return 0;
    size_t count = node->type == LXB_DOM_NODE_TYPE_DOCUMENT ? 0 : 1;
    for (lxb_dom_node_t *child = node->first_child;
         child != NULL; child = child->next) {
        size_t added = experimental_remote_subtree_node_count(child);
        if (added > SIZE_MAX - count) return SIZE_MAX;
        count += added;
    }
    return count;
}

static lxb_dom_node_t *experimental_remote_node_relation(
    lxb_dom_node_t *node, ScriptRemoteNodeReadKind kind)
{
    if (node == NULL) return NULL;
    lxb_dom_node_t *related = NULL;
    if (kind == SCRIPT_REMOTE_NODE_PARENT_ELEMENT) {
        related = node->parent;
        while (related != NULL
               && related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            related = related->parent;
        }
        return related;
    }
    if (kind == SCRIPT_REMOTE_NODE_FIRST_ELEMENT_CHILD) {
        related = node->first_child;
        while (related != NULL
               && related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            related = related->next;
        }
        return related;
    }
    if (kind == SCRIPT_REMOTE_NODE_NEXT_ELEMENT_SIBLING) {
        related = node->next;
        while (related != NULL
               && related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            related = related->next;
        }
        return related;
    }
    if (kind == SCRIPT_REMOTE_NODE_PREVIOUS_ELEMENT_SIBLING) {
        related = node->prev;
        while (related != NULL
               && related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            related = related->prev;
        }
        return related;
    }
    if (kind == SCRIPT_REMOTE_NODE_FIRST_CHILD) {
        related = node->first_child;
        if (experimental_comment_is(
                related, "tilefinch-semantic-section-start")) {
            related = related->next;
        }
        return experimental_comment_is(
            related, "tilefinch-semantic-section-end") ? NULL : related;
    }
    if (kind == SCRIPT_REMOTE_NODE_LAST_CHILD) {
        related = node->last_child;
        if (experimental_comment_is(
                related, "tilefinch-semantic-section-end")) {
            related = related->prev;
        }
        return experimental_comment_is(
            related, "tilefinch-semantic-section-start") ? NULL : related;
    }
    if (kind == SCRIPT_REMOTE_NODE_NEXT_SIBLING) {
        related = node->next;
        return experimental_comment_is(
            related, "tilefinch-semantic-section-end") ? NULL : related;
    }
    if (kind == SCRIPT_REMOTE_NODE_PREVIOUS_SIBLING) {
        related = node->prev;
        return experimental_comment_is(
            related, "tilefinch-semantic-section-start") ? NULL : related;
    }
    return NULL;
}

static bool experimental_remote_relation_result(
    ExperimentalSectionContext *experimental, PocDocument *document,
    size_t section_index, lxb_dom_node_t *related,
    ScriptRemoteNodeReadResult *result)
{
    if (related == NULL) {
        result->is_null = true;
        return true;
    }
    result->related_node_type = (unsigned) related->type;
    ExperimentalRemoteOrdinalFind ordinal = {.target = related};
    if (related->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        experimental_remote_ordinal_walk(
            lxb_dom_interface_node(document->html), &ordinal);
        if (!ordinal.found) return false;
        if (!experimental_node_visible(
                experimental, section_index, SIZE_MAX,
                ordinal.target_node_ordinal, (unsigned) related->type)) {
            result->is_null = true;
            return true;
        }
        int written = snprintf(
            result->related_stable_key,
            sizeof(result->related_stable_key),
            "n:%zu:%zu:%u", section_index, ordinal.target_node_ordinal,
            (unsigned) related->type);
        if (written <= 0
            || (size_t) written >= sizeof(result->related_stable_key)) {
            return false;
        }
        result->related_section_index = section_index;
        return true;
    }
    experimental_remote_ordinal_walk(
        lxb_dom_interface_node(document->html), &ordinal);
    if (!ordinal.found) return false;
    if (!experimental_node_visible(
            experimental, section_index, ordinal.target_ordinal,
            ordinal.target_node_ordinal, (unsigned) related->type)) {
        result->is_null = true;
        return true;
    }
    size_t tag_length = 0;
    const char *tag = document_element_name(related, &tag_length);
    if (tag == NULL || tag_length == 0 || tag_length >= 32) return false;
    if (tag_length == 4 && strncasecmp(tag, "html", 4) == 0) {
        result->related_special = 1;
        return true;
    }
    if (tag_length == 4 && strncasecmp(tag, "head", 4) == 0) {
        result->related_special = 2;
        return true;
    }
    if (tag_length == 4 && strncasecmp(tag, "body", 4) == 0) {
        result->related_special = 3;
        return true;
    }
    for (size_t i = 0; i < tag_length; i++) {
        result->related_tag_name[i] = (char) tolower(
            (unsigned char) tag[i]);
    }
    result->related_tag_name[tag_length] = '\0';
    int written = snprintf(
        result->related_stable_key, sizeof(result->related_stable_key),
        "%c:%zu:%zu:%s", ordinal.target_in_source ? 's' : 'c',
        section_index, ordinal.target_ordinal, result->related_tag_name);
    if (written <= 0
        || (size_t) written >= sizeof(result->related_stable_key)) {
        return false;
    }
    result->related_section_index = section_index;
    return true;
}

static lxb_dom_node_t *experimental_find_marker_comment(
    lxb_dom_node_t *node, const char *marker)
{
    for (; node != NULL; node = node->next) {
        if (experimental_comment_is(node, marker)) return node;
        lxb_dom_node_t *found = experimental_find_marker_comment(
            node->first_child, marker);
        if (found != NULL) return found;
    }
    return NULL;
}

static bool experimental_optional_attribute_equal(
    lxb_dom_node_t *first, lxb_dom_node_t *second, const char *name)
{
    size_t first_length = 0, second_length = 0;
    const char *first_value = document_attribute(first, name, &first_length);
    const char *second_value = document_attribute(
        second, name, &second_length);
    return first_length == second_length
        && ((first_value == NULL && second_value == NULL)
            || (first_value != NULL && second_value != NULL
                && memcmp(first_value, second_value, first_length) == 0));
}

static bool experimental_boundary_parent_equal(lxb_dom_node_t *first,
                                               lxb_dom_node_t *second)
{
    while (first != NULL && second != NULL) {
        while (first != NULL
               && first->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            first = first->parent;
        }
        while (second != NULL
               && second->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            second = second->parent;
        }
        if (first == NULL || second == NULL) break;
        size_t first_length = 0, second_length = 0;
        const char *first_tag = document_element_name(first, &first_length);
        const char *second_tag = document_element_name(second, &second_length);
        if (first_tag == NULL || second_tag == NULL
            || first_length != second_length
            || strncasecmp(first_tag, second_tag, first_length) != 0
            || !experimental_optional_attribute_equal(first, second, "id")
            || !experimental_optional_attribute_equal(
                   first, second, "class")) return false;
        if (first_length == 4
            && strncasecmp(first_tag, "html", 4) == 0) return true;
        first = first->parent;
        second = second->parent;
    }
    return first == NULL && second == NULL;
}

static bool experimental_remote_boundary_relation(
    ExperimentalSectionContext *experimental, lxb_dom_node_t *target,
    size_t section_index, ScriptRemoteNodeReadKind kind,
    ScriptRemoteNodeReadResult *result)
{
    bool first_child = kind == SCRIPT_REMOTE_NODE_FIRST_CHILD;
    bool last_child = kind == SCRIPT_REMOTE_NODE_LAST_CHILD;
    bool next = kind == SCRIPT_REMOTE_NODE_NEXT_ELEMENT_SIBLING
        || kind == SCRIPT_REMOTE_NODE_NEXT_SIBLING || first_child;
    bool previous = kind == SCRIPT_REMOTE_NODE_PREVIOUS_ELEMENT_SIBLING
        || kind == SCRIPT_REMOTE_NODE_PREVIOUS_SIBLING || last_child;
    bool elements_only = kind == SCRIPT_REMOTE_NODE_NEXT_ELEMENT_SIBLING
        || kind == SCRIPT_REMOTE_NODE_PREVIOUS_ELEMENT_SIBLING;
    if ((!next && !previous)
        || (next && section_index + 1 >= experimental->store->section_count)
        || (previous && section_index == 0)) {
        result->is_null = true;
        return true;
    }
    size_t adjacent_section = next ? section_index + 1 : section_index - 1;
    char *html = NULL;
    size_t html_length = 0;
    PocDocument adjacent = {0};
    bool success = section_store_extract_query_html(
            experimental->store, adjacent_section, &html, &html_length)
        && document_parse(&adjacent, experimental->budget, html,
                          html_length, 4096)
        && experimental_apply_remote_mutations(
               experimental, &adjacent, adjacent_section);
    lxb_dom_node_t *marker = success ? experimental_find_marker_comment(
        lxb_dom_interface_node(adjacent.html),
        next ? "tilefinch-semantic-section-start"
             : "tilefinch-semantic-section-end") : NULL;
    lxb_dom_node_t *candidate = marker == NULL ? NULL
        : (next ? marker->next : marker->prev);
    while (elements_only && candidate != NULL
           && candidate->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        candidate = next ? candidate->next : candidate->prev;
    }
    if (experimental_comment_is(
            candidate, next ? "tilefinch-semantic-section-end"
                            : "tilefinch-semantic-section-start")) {
        candidate = NULL;
    }
    lxb_dom_node_t *target_parent = target == NULL ? NULL
        : (first_child || last_child ? target : target->parent);
    lxb_dom_node_t *marker_parent = marker == NULL ? NULL : marker->parent;
    if (success && candidate != NULL
        && candidate->parent == marker_parent
        && experimental_boundary_parent_equal(
               target_parent, marker_parent)) {
        success = experimental_remote_relation_result(
            experimental, &adjacent, adjacent_section, candidate, result);
    } else if (success) {
        result->is_null = true;
    }
    document_destroy(&adjacent);
    budget_free(experimental->budget, html);
    return success;
}

static bool experimental_measure_section_height(
    ExperimentalSectionContext *experimental, size_t section_index)
{
    if (experimental == NULL || experimental->navigation == NULL
        || section_index >= experimental->store->section_count) return false;
    int known_height = 0;
    if (section_pager_known_height(
            &experimental->pager, section_index, &known_height)) return true;
    char *html = NULL;
    size_t html_length = 0;
    PocDocument document = {0};
    LayoutDocument layout = {0};
    bool success = section_store_extract_html(
            experimental->store, section_index, &html, &html_length)
        && document_parse(&document, experimental->budget, html,
                          html_length, 4096)
        && experimental_apply_remote_mutations(
               experimental, &document, section_index)
        && layout_build_context(
               &layout, experimental->budget, &document,
               &experimental->navigation->page.stylesheet,
               experimental->navigation->fonts, NULL,
               &experimental->navigation->viewport)
        && layout.height > 0;
    if (success) {
        success = section_pager_record_section_height(
            &experimental->pager, section_index, layout.height);
        if (success) experimental->remote_geometry_layouts++;
    }
    layout_destroy(&layout);
    document_destroy(&document);
    budget_free(experimental->budget, html);
    return success;
}

static bool experimental_ensure_logical_offset(
    ExperimentalSectionContext *experimental, size_t section_index,
    size_t *offset)
{
    if (experimental == NULL || offset == NULL) return false;
    for (size_t section = 0; section < section_index; section++) {
        if (!experimental_measure_section_height(
                experimental, section)) return false;
    }
    return section_pager_known_logical_offset(
        &experimental->pager, section_index, offset);
}

static int experimental_clamp_geometry(long long value)
{
    if (value < INT_MIN) return INT_MIN;
    if (value > INT_MAX) return INT_MAX;
    return (int) value;
}

static int experimental_current_page_scroll(
    const ExperimentalSectionContext *experimental)
{
    const NavigationSession *navigation = experimental == NULL
        ? NULL : experimental->navigation;
    return navigation != NULL && navigation->history_count != 0
        && navigation->history_index < navigation->history_count
        ? navigation->history[navigation->history_index].scroll_y : 0;
}

static bool experimental_remote_geometry_cache_get(
    ExperimentalSectionContext *experimental, const char *stable_key,
    size_t stable_key_length, size_t section_index,
    ScriptRemoteNodeReadResult *result)
{
    NavigationSession *navigation = experimental == NULL
        ? NULL : experimental->navigation;
    if (navigation == NULL || !experimental->remote_geometry_cache_valid
        || experimental->remote_geometry_cache_key_length
           != stable_key_length
        || memcmp(experimental->remote_geometry_cache_key, stable_key,
                  stable_key_length) != 0
        || experimental->remote_geometry_cache_section != section_index
        || experimental->remote_geometry_cache_current_section
           != experimental->pager.current_section
        || experimental->remote_geometry_cache_writes
           != experimental->remote_writes
        || experimental->remote_geometry_cache_relayouts
           != navigation->incremental_relayouts
        || experimental->remote_geometry_cache_loads
           != navigation->loads_committed
        || experimental->remote_geometry_cache_scroll
           != experimental_current_page_scroll(experimental)) return false;
    *result = experimental->remote_geometry_cache_result;
    experimental->remote_geometry_cache_hits++;
    return true;
}

static void experimental_remote_geometry_cache_put(
    ExperimentalSectionContext *experimental, const char *stable_key,
    size_t stable_key_length, size_t section_index,
    const ScriptRemoteNodeReadResult *result)
{
    NavigationSession *navigation = experimental == NULL
        ? NULL : experimental->navigation;
    if (navigation == NULL || stable_key == NULL
        || stable_key_length == 0
        || stable_key_length >= sizeof(
               experimental->remote_geometry_cache_key)
        || result == NULL) return;
    memcpy(experimental->remote_geometry_cache_key, stable_key,
           stable_key_length);
    experimental->remote_geometry_cache_key[stable_key_length] = '\0';
    experimental->remote_geometry_cache_key_length = stable_key_length;
    experimental->remote_geometry_cache_section = section_index;
    experimental->remote_geometry_cache_current_section =
        experimental->pager.current_section;
    experimental->remote_geometry_cache_writes = experimental->remote_writes;
    experimental->remote_geometry_cache_relayouts =
        navigation->incremental_relayouts;
    experimental->remote_geometry_cache_loads = navigation->loads_committed;
    experimental->remote_geometry_cache_scroll =
        experimental_current_page_scroll(experimental);
    experimental->remote_geometry_cache_result = *result;
    experimental->remote_geometry_cache_valid = true;
}

static bool experimental_remote_geometry_result(
    ExperimentalSectionContext *experimental, PocDocument *document,
    lxb_dom_node_t *target, size_t section_index,
    ScriptRemoteNodeReadResult *result)
{
    if (experimental == NULL || experimental->navigation == NULL
        || document == NULL || target == NULL || result == NULL) return false;
    LayoutDocument layout = {0};
    bool success = layout_build_context(
        &layout, experimental->budget, document,
        &experimental->navigation->page.stylesheet,
        experimental->navigation->fonts, NULL,
        &experimental->navigation->viewport);
    const LayoutNodeBox *box = success
        ? layout_box_for_node(&layout, target) : NULL;
    if (success && layout.height > 0) {
        success = section_pager_record_section_height(
            &experimental->pager, section_index, layout.height);
        if (success) experimental->remote_geometry_layouts++;
    }
    size_t target_offset = 0, current_offset = 0;
    if (success) {
        success = experimental_ensure_logical_offset(
                experimental, section_index, &target_offset)
            && experimental_ensure_logical_offset(
                experimental, experimental->pager.current_section,
                &current_offset);
    }
    NavigationSession *navigation = experimental->navigation;
    int page_scroll = experimental_current_page_scroll(experimental);
    if (success && box != NULL) {
        long long y = (long long) target_offset + box->y
            - (long long) current_offset - page_scroll;
        for (size_t at = layout.fixed_count; at > 0; at--) {
            const FixedRange *range = &layout.fixed_ranges[at - 1];
            if (box->command_start < range->command_start
                || box->command_end > range->command_end) continue;
            y = range->from_bottom
                ? navigation->viewport.css_height - range->inset
                    - range->height
                : range->inset;
            break;
        }
        result->geometry_x = box->x;
        result->geometry_y = experimental_clamp_geometry(y);
        result->geometry_width = box->width;
        result->geometry_height = box->height;
        result->geometry_client_width = box->client_width;
        result->geometry_client_height = box->client_height;
        result->geometry_scroll_width = box->content_width;
        result->geometry_scroll_height = box->content_height;
        result->geometry_scroll_left = box->scroll_x;
        result->geometry_scroll_top = box->scroll_y;
    }
    layout_destroy(&layout);
    if (!success) experimental->remote_geometry_failures++;
    return success;
}

typedef struct {
    Budget *budget;
    char *data;
    size_t length;
    bool failed;
} ExperimentalRemoteHTMLBuffer;

static lxb_status_t experimental_remote_html_receive(
    const lxb_char_t *data, size_t length, void *opaque)
{
    ExperimentalRemoteHTMLBuffer *buffer = opaque;
    if (buffer->failed
        || length > EXPERIMENTAL_REMOTE_INNER_HTML_LIMIT - buffer->length) {
        buffer->failed = true;
        return LXB_STATUS_ERROR;
    }
    char *resized = budget_realloc_category(
        buffer->budget, BUDGET_CATEGORY_RESOURCE, buffer->data,
        buffer->length + length + 1);
    if (resized == NULL) {
        buffer->failed = true;
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }
    buffer->data = resized;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return LXB_STATUS_OK;
}

static bool experimental_remote_node_copy(
    ExperimentalSectionContext *experimental, const char *value,
    size_t length, ScriptRemoteNodeReadResult *result)
{
    if (length == 0) return true;
    result->value = budget_malloc_category(
        experimental->budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (result->value == NULL) return false;
    memcpy(result->value, value, length);
    result->value[length] = '\0';
    result->length = length;
    return true;
}

static bool experimental_remote_copy_string(
    ExperimentalSectionContext *experimental, const char *value,
    size_t length, char **copy)
{
    *copy = NULL;
    if (length == 0) return true;
    *copy = budget_malloc_category(
        experimental->budget, BUDGET_CATEGORY_RESOURCE, length + 1);
    if (*copy == NULL) return false;
    memcpy(*copy, value, length);
    (*copy)[length] = '\0';
    return true;
}

static void experimental_remote_node_result_clear(
    ExperimentalSectionContext *experimental,
    ScriptRemoteNodeReadResult *result)
{
    if (experimental == NULL || result == NULL) return;
    budget_free(experimental->budget, result->value);
    for (size_t i = 0; i < result->attribute_count; i++) {
        budget_free(experimental->budget, result->attributes[i].name);
        budget_free(experimental->budget, result->attributes[i].value);
    }
    budget_free(experimental->budget, result->attributes);
    memset(result, 0, sizeof(*result));
}

typedef struct {
    ExperimentalSectionContext *experimental;
    unsigned root_special;
    size_t section_index;
    size_t node_ordinal;
    bool in_source;
    bool saw_start;
    bool saw_end;
    char *data;
    size_t length;
    bool failed;
} ExperimentalRootTextRead;

static void experimental_root_text_walk(
    lxb_dom_node_t *node, ExperimentalRootTextRead *read)
{
    for (; node != NULL && !read->failed; node = node->next) {
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-start")) {
            read->saw_start = true;
            read->in_source = true;
            continue;
        }
        if (experimental_comment_is(
                node, "tilefinch-semantic-section-end")) {
            read->saw_end = true;
            read->in_source = false;
            continue;
        }
        size_t node_ordinal = SIZE_MAX;
        if (node->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            node_ordinal = read->node_ordinal++;
        }
        bool structural_head = false;
        if (read->section_index == 0 && read->root_special == 1) {
            for (lxb_dom_node_t *at = node->parent;
                 at != NULL; at = at->parent) {
                if (experimental_node_special(at) == 2) {
                    structural_head = true;
                    break;
                }
            }
        }
        if ((read->in_source || structural_head)
            && node->type == LXB_DOM_NODE_TYPE_TEXT
            && experimental_node_within_traversal_root(
                   node, read->root_special)
            && experimental_node_visible(
                   read->experimental, read->section_index, SIZE_MAX,
                   node_ordinal, (unsigned) node->type)) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            if (text == NULL
                || length > EXPERIMENTAL_REMOTE_INNER_HTML_LIMIT
                            - read->length) {
                read->failed = true;
                return;
            }
            if (length != 0) {
                char *resized = budget_realloc_category(
                    read->experimental->budget, BUDGET_CATEGORY_RESOURCE,
                    read->data, read->length + length + 1);
                if (resized == NULL) {
                    read->failed = true;
                    return;
                }
                read->data = resized;
                memcpy(read->data + read->length, text, length);
                read->length += length;
                read->data[read->length] = '\0';
            }
        }
        experimental_root_text_walk(node->first_child, read);
    }
}

static bool experimental_remote_root_text_read(
    ExperimentalSectionContext *experimental, unsigned root_special,
    ScriptRemoteNodeReadResult *result)
{
    ExperimentalRootTextRead read = {
        .experimental = experimental,
        .root_special = root_special
    };
    for (size_t section = 0;
         section < experimental->store->section_count; section++) {
        char *html = NULL;
        size_t html_length = 0;
        PocDocument document = {0};
        read.section_index = section;
        read.node_ordinal = 0;
        read.in_source = false;
        read.saw_start = false;
        read.saw_end = false;
        bool success = section_store_extract_query_html(
                experimental->store, section, &html, &html_length)
            && document_parse(&document, experimental->budget, html,
                              html_length, 4096)
            && experimental_apply_remote_mutations(
                   experimental, &document, section);
        if (success) {
            experimental_root_text_walk(
                lxb_dom_interface_node(document.html), &read);
            success = !read.failed && read.saw_start && read.saw_end;
        }
        document_destroy(&document);
        budget_free(experimental->budget, html);
        if (!success) {
            budget_free(experimental->budget, read.data);
            return false;
        }
    }
    result->value = read.data;
    result->length = read.length;
    return true;
}

static bool experimental_remote_node_read(
    void *opaque, const char *stable_key, size_t stable_key_length,
    size_t section_index, ScriptRemoteNodeReadKind kind,
    const char *name, size_t name_length, ScriptRemoteNodeReadResult *result)
{
    ExperimentalSectionContext *experimental = opaque;
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (experimental == NULL || experimental->store == NULL
        || experimental->budget == NULL || result == NULL
        || section_index >= experimental->store->section_count) return false;
    experimental->remote_reads++;
    bool body_root = stable_key_length == 6
        && memcmp(stable_key, "d:body", 6) == 0;
    bool html_root = stable_key_length == 6
        && memcmp(stable_key, "d:html", 6) == 0;
    if (kind == SCRIPT_REMOTE_NODE_TEXT && (body_root || html_root)) {
        bool success = false;
        if (experimental->body_source_replaced
            && experimental->navigation != NULL) {
            PocDocument *document =
                &experimental->navigation->page.document;
            lxb_dom_node_t *root = body_root ? document_body_node(document)
                : experimental_find_element_tag(
                      lxb_dom_interface_node(document->html), "html");
            size_t length = 0;
            lxb_char_t *text = root == NULL ? NULL
                : lxb_dom_node_text_content(root, &length);
            success = root != NULL && (text != NULL || length == 0)
                && experimental_remote_node_copy(
                    experimental, text == NULL ? "" : (const char *) text,
                    text == NULL ? 0 : length, result);
            if (text != NULL) {
                lxb_dom_document_destroy_text(root->owner_document, text);
            }
        } else {
            success = experimental_remote_root_text_read(
                experimental, body_root ? 3 : 1, result);
        }
        if (!success) experimental->remote_read_failures++;
        else experimental->remote_read_bytes += result->length;
        return success;
    }
    if (experimental->body_source_replaced) {
        experimental->remote_read_failures++;
        return false;
    }
    if (kind == SCRIPT_REMOTE_NODE_GEOMETRY) {
        experimental->remote_geometry_reads++;
        if (experimental_remote_geometry_cache_get(
                experimental, stable_key, stable_key_length,
                section_index, result)) return true;
    }
    ExperimentalRemoteNodeFind find;
    char *html = NULL;
    size_t html_length = 0;
    PocDocument document = {0};
    bool key_parsed = experimental_parse_remote_key(
        stable_key, stable_key_length, section_index, &find);
    bool html_extracted = key_parsed
        && section_store_extract_query_html(
               experimental->store, section_index, &html, &html_length);
    bool document_parsed = html_extracted
        && document_parse(&document, experimental->budget, html,
                          html_length, 4096);
    bool success = document_parsed;
    bool retained_source_target = success
        && experimental_remote_mutation_find(
               experimental, stable_key, stable_key_length,
               section_index) != NULL;
    if (retained_source_target) {
        experimental_remote_node_find_walk(
            lxb_dom_interface_node(document.html), &find);
        success = find.saw_start && find.found != NULL;
    }
    if (success) {
        success = experimental_apply_remote_mutations(
            experimental, &document, section_index);
    }
    if (success && !retained_source_target) {
        experimental_remote_node_find_walk(
            lxb_dom_interface_node(document.html), &find);
        /* The walk stops as soon as the exact node is found, so the closing
           marker can legitimately remain unvisited. */
        success = find.saw_start && find.found != NULL;
    }
    if (success && kind == SCRIPT_REMOTE_NODE_TEXT) {
        size_t length = 0;
        lxb_char_t *text = lxb_dom_node_text_content(find.found, &length);
        success = text != NULL
            ? experimental_remote_node_copy(
                  experimental, (const char *) text, length, result)
            : true;
        if (text != NULL) {
            lxb_dom_document_destroy_text(find.found->owner_document, text);
        }
    } else if (success && kind == SCRIPT_REMOTE_NODE_INNER_HTML) {
        ExperimentalRemoteHTMLBuffer buffer = {
            .budget = experimental->budget
        };
        size_t tag_length = 0;
        const char *tag = document_element_name(find.found, &tag_length);
        lxb_dom_node_t *container = find.found;
        /* Only an HTML-namespaced template carries the template interface.
           Lexbor builds `<svg><template>` as a plain element, so casting on
           the local name alone would read `content` past that allocation. */
        if (tag != NULL && tag_length == 8
            && find.found->ns == LXB_NS_HTML
            && strncasecmp(tag, "template", 8) == 0) {
            lxb_html_template_element_t *element =
                lxb_html_interface_template(find.found);
            if (element->content != NULL) {
                container = lxb_dom_interface_node(element->content);
            }
        }
        for (lxb_dom_node_t *child = container->first_child;
             child != NULL && !buffer.failed; child = child->next) {
            if (lxb_html_serialize_tree_cb(
                    child, experimental_remote_html_receive, &buffer)
                != LXB_STATUS_OK) buffer.failed = true;
        }
        success = !buffer.failed;
        if (success) {
            result->value = buffer.data;
            result->length = buffer.length;
            buffer.data = NULL;
        }
        budget_free(experimental->budget, buffer.data);
    } else if (success && kind == SCRIPT_REMOTE_NODE_ATTRIBUTE) {
        if (find.found->type != LXB_DOM_NODE_TYPE_ELEMENT
            || name == NULL || name_length == 0) {
            success = false;
        } else {
            lxb_dom_element_t *element = lxb_dom_interface_element(find.found);
            size_t length = 0;
            const lxb_char_t *value = lxb_dom_element_get_attribute(
                element, (const lxb_char_t *) name, name_length, &length);
            bool present = value != NULL || lxb_dom_element_has_attribute(
                element, (const lxb_char_t *) name, name_length);
            result->is_null = !present;
            success = !present || experimental_remote_node_copy(
                experimental, value == NULL ? "" : (const char *) value,
                value == NULL ? 0 : length, result);
        }
    } else if (success && kind == SCRIPT_REMOTE_NODE_ATTRIBUTES) {
        if (find.found->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            success = false;
        } else {
            lxb_dom_element_t *element = lxb_dom_interface_element(find.found);
            size_t count = 0;
            for (lxb_dom_attr_t *attribute = element->first_attr;
                 attribute != NULL && count < 64;
                 attribute = attribute->next) count++;
            if (count != 0) {
                result->attributes = budget_calloc_category(
                    experimental->budget, BUDGET_CATEGORY_RESOURCE,
                    count, sizeof(*result->attributes));
                success = result->attributes != NULL;
            }
            result->attribute_count = success ? count : 0;
            size_t index = 0;
            for (lxb_dom_attr_t *attribute = element->first_attr;
                 success && attribute != NULL && index < count;
                 attribute = attribute->next, index++) {
                size_t attribute_name_length = 0, value_length = 0;
                const lxb_char_t *attribute_name =
                    lxb_dom_attr_qualified_name(
                        attribute, &attribute_name_length);
                const lxb_char_t *value = lxb_dom_attr_value(
                    attribute, &value_length);
                ScriptRemoteNodeAttribute *copy =
                    &result->attributes[index];
                copy->name_length = attribute_name_length;
                copy->value_length = value == NULL ? 0 : value_length;
                success = attribute_name != NULL
                    && experimental_remote_copy_string(
                           experimental, (const char *) attribute_name,
                           attribute_name_length, &copy->name)
                    && experimental_remote_copy_string(
                           experimental,
                           value == NULL ? "" : (const char *) value,
                           copy->value_length, &copy->value);
                if (success) {
                    experimental->remote_read_bytes +=
                        copy->name_length + copy->value_length;
                }
            }
        }
    } else if (success && kind == SCRIPT_REMOTE_NODE_MATCHES) {
        if (name == NULL || name_length == 0 || name_length > 512) {
            success = false;
        } else {
            static const char matched[] = "1";
            static const char unmatched[] = "0";
            success = experimental_remote_node_copy(
                experimental,
                experimental_selector_list_matches(
                    find.found, name, name_length) ? matched : unmatched,
                1, result);
        }
    } else if (success && kind == SCRIPT_REMOTE_NODE_GEOMETRY) {
        success = experimental_remote_geometry_result(
            experimental, &document, find.found, section_index, result);
        if (success) {
            experimental_remote_geometry_cache_put(
                experimental, stable_key, stable_key_length,
                section_index, result);
        }
    } else if (success
               && kind >= SCRIPT_REMOTE_NODE_PARENT_ELEMENT
               && kind <= SCRIPT_REMOTE_NODE_LAST_CHILD) {
        lxb_dom_node_t *related = experimental_remote_node_relation(
            find.found, kind);
        success = related != NULL
            ? experimental_remote_relation_result(
                  experimental, &document, section_index, related, result)
            : experimental_remote_boundary_relation(
                  experimental, find.found, section_index, kind, result);
    }
    document_destroy(&document);
    budget_free(experimental->budget, html);
    if (!success) {
        if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
            fprintf(stderr,
                    "experimental-remote-read-failure key=\"%.*s\" "
                    "section=%zu kind=%d key-parsed=%s extracted=%s "
                    "document=%s saw-start=%s found=%s node-type=%u\n",
                    (int) stable_key_length, stable_key, section_index,
                    (int) kind, key_parsed ? "yes" : "no",
                    html_extracted ? "yes" : "no",
                    document_parsed ? "yes" : "no",
                    find.saw_start ? "yes" : "no",
                    find.found != NULL ? "yes" : "no",
                    find.found == NULL ? 0u : (unsigned) find.found->type);
        }
        experimental_remote_node_result_clear(experimental, result);
        experimental->remote_read_failures++;
        return false;
    }
    experimental->remote_read_bytes += result->length;
    return true;
}

static bool ascii_span_contains_folded(const char *text, size_t length,
                                       const char *needle)
{
    size_t needle_length = strlen(needle);
    if (needle_length == 0 || needle_length > length) return false;
    for (size_t at = 0; at + needle_length <= length; at++) {
        size_t i = 0;
        while (i < needle_length
               && tolower((unsigned char) text[at + i])
                  == tolower((unsigned char) needle[i])) i++;
        if (i == needle_length) return true;
    }
    return false;
}

static bool experimental_may_link_stylesheet(const char *html, size_t length)
{
    return html != NULL
        && ascii_span_contains_folded(html, length, "<link")
        && ascii_span_contains_folded(html, length, "stylesheet");
}

static bool experimental_preflight_external_layout(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    uint64_t *generation, const FontSet *fonts)
{
    if (experimental == NULL || navigation == NULL || generation == NULL
        || !experimental->active
        || !navigation->external_resources_enabled
        || !experimental_may_link_stylesheet(
               experimental->pager.current_html,
               experimental->pager.current_length)) return true;
    (void) generation;
    (void) fonts;
    PocDocument probe = {0};
    Stylesheet stylesheet = {0};
    ExternalStylesheetStats stats = {0};
    FetchScheduler *scheduler = NULL;
    ViewportContext viewport;
    char resource_base_url[NAVIGATION_URL_LIMIT] = {0};
    char document_policy[FETCH_REFERRER_POLICY_LIMIT] = {0};
    size_t maximum_file = navigation->maximum_stylesheet_file_bytes;
    size_t reserved = maximum_file <= SIZE_MAX / 4u
        ? maximum_file * 4u : maximum_file;
    bool ready = document_parse(
            &probe, navigation->budget,
            experimental->pager.current_html,
            experimental->pager.current_length, 512)
        && viewport_context_resolve(&viewport, &probe, 480, 272, 980)
        && stylesheet_build_context_deferred(
               &stylesheet, navigation->budget, &probe, &viewport);
    if (ready) {
        bool header_present = false;
        if (experimental->response != NULL) {
            ready = fetch_response_referrer_policy(
                experimental->response, &header_present, document_policy);
        }
        ready = ready
            && document_referrer_policy(
                   &probe, document_policy, document_policy,
                   sizeof(document_policy))
            && document_base_url(
                   &probe, experimental->locator, resource_base_url,
                   sizeof(resource_base_url));
        (void) header_present;
    }
    if (ready) {
        scheduler = fetch_scheduler_create(
            navigation->budget, 4, reserved);
        ready = scheduler != NULL
            && stylesheets_load_external_with_context(
                   &probe, &stylesheet, navigation->budget,
                   resource_base_url, experimental->locator,
                   document_policy,
                   navigation->maximum_stylesheets,
                   navigation->maximum_stylesheet_bytes,
                   navigation->maximum_stylesheet_file_bytes,
                   navigation->resource_timeout_ms, scheduler,
                   navigation->browser_session, &stats)
            && (navigation->user_css_length == 0
                || stylesheet_add_user_css(
                       &stylesheet, navigation->user_css,
                       navigation->user_css_length));
    }
    if (!ready) {
        fetch_scheduler_destroy(scheduler);
        stylesheet_destroy(&stylesheet);
        document_destroy(&probe);
        return false;
    }
    const char *selectors[128];
    size_t selector_count = stylesheet_layout_island_selectors(
        &stylesheet, selectors,
        sizeof(selectors) / sizeof(selectors[0]));
    size_t old_count = experimental->store->section_count;
    size_t old_section = experimental->pager.current_section;
    size_t source_offset =
        experimental->store->sections[old_section].source_offset;
    if (selector_count != 0) {
        if (!section_store_reindex_layout_selectors(
                experimental->store, selectors, selector_count)) {
            printf("experimental-layout-reindex status=skipped reason=budget "
                   "selectors=%zu\n", selector_count);
        } else {
            size_t selected = 0;
            while (selected + 1 < experimental->store->section_count
                   && source_offset
                        >= (size_t) experimental->store->sections[selected]
                                             .source_offset
                           + experimental->store->sections[selected]
                                                 .source_length) {
                selected++;
            }
            section_pager_destroy(&experimental->pager);
            if (!section_pager_init(
                    &experimental->pager, experimental->store,
                    selected, 1)
                || !experimental_script_state_init(experimental)
                || !navigation_set_runtime_section_identity(
                       navigation, selected)) {
                fetch_scheduler_destroy(scheduler);
                stylesheet_destroy(&stylesheet);
                document_destroy(&probe);
                return false;
            }
            printf("experimental-layout-reindex status=applied selectors=%zu "
                   "sections=%zu->%zu selected=%zu\n",
                   selector_count, old_count,
                   experimental->store->section_count, selected);
        }
    }
    printf("experimental-layout-preflight mode=style-only selectors=%zu "
           "stylesheets=%zu cache-hits=%zu\n",
           selector_count, stats.loaded, stats.cache_hits);
    fetch_scheduler_destroy(scheduler);
    stylesheet_destroy(&stylesheet);
    document_destroy(&probe);
    return true;
}

static bool experimental_execute_section_scripts(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    size_t section)
{
    if (navigation == NULL || !navigation->document_scripts_enabled
        || experimental_section_scripts_executed(experimental, section)) {
        return true;
    }
    if (!navigation_execute_current_body_scripts(navigation)
        || !experimental_mark_section_scripts_executed(
               experimental, section)) return false;
    printf("experimental-scripts section=%zu mode=first-materialization\n",
           section);
    return true;
}

typedef struct {
    ExperimentalSectionContext *experimental;
    char *data;
    size_t length;
    size_t limit;
    size_t scripts;
    size_t script_limit;
    bool failed;
} ExperimentalScriptCorpus;

static bool experimental_script_corpus_append(
    ExperimentalScriptCorpus *corpus, const char *data, size_t length)
{
    if (corpus == NULL || corpus->experimental == NULL
        || corpus->failed || data == NULL
        || length > corpus->limit - corpus->length) return false;
    char *resized = budget_realloc_category(
        corpus->experimental->budget, BUDGET_CATEGORY_RESOURCE,
        corpus->data, corpus->length + length + 1);
    if (resized == NULL) return false;
    corpus->data = resized;
    memcpy(corpus->data + corpus->length, data, length);
    corpus->length += length;
    corpus->data[corpus->length] = '\0';
    return true;
}

static lxb_status_t experimental_script_corpus_receive(
    const lxb_char_t *data, size_t length, void *opaque)
{
    ExperimentalScriptCorpus *corpus = opaque;
    if (!experimental_script_corpus_append(
            corpus, (const char *) data, length)) {
        corpus->failed = true;
        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }
    return LXB_STATUS_OK;
}

static void experimental_script_corpus_walk(
    lxb_dom_node_t *node, PocDocument *document, size_t section_index,
    bool body_visible, ExperimentalScriptCorpus *corpus)
{
    for (; node != NULL && !corpus->failed; node = node->next) {
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (tag != NULL && tag_length == 6
            && strncasecmp(tag, "script", 6) == 0) {
            if (corpus->scripts < corpus->script_limit) {
                ExperimentalRemoteOrdinalFind ordinal = {.target = node};
                experimental_remote_ordinal_walk(
                    lxb_dom_interface_node(document->html), &ordinal);
                size_t subtree_nodes =
                    experimental_remote_subtree_node_count(node);
                size_t frontier =
                    corpus->experimental->script_frontier_count;
                if (!ordinal.found
                    || subtree_nodes == 0 || subtree_nodes == SIZE_MAX
                    || subtree_nodes - 1
                       > SIZE_MAX - ordinal.target_node_ordinal
                    || frontier >= EXPERIMENTAL_SCRIPT_FRONTIER_LIMIT
                    || lxb_html_serialize_tree_cb(
                        node, experimental_script_corpus_receive, corpus)
                    != LXB_STATUS_OK
                    || !experimental_script_corpus_append(
                           corpus, "\n", 1)) {
                    corpus->failed = true;
                    return;
                }
                corpus->experimental->script_frontier_sections[frontier] =
                    section_index;
                corpus->experimental->script_frontier_ordinals[frontier] =
                    ordinal.target_ordinal;
                corpus->experimental->script_frontier_node_ordinals[frontier] =
                    ordinal.target_node_ordinal + subtree_nodes - 1;
                corpus->experimental->script_frontier_body_visible[frontier] =
                    body_visible;
                corpus->experimental->script_frontier_count++;
            }
            corpus->scripts++;
            continue;
        }
        experimental_script_corpus_walk(
            node->first_child, document, section_index, body_visible, corpus);
    }
}

static lxb_dom_node_t *experimental_find_element_tag(
    lxb_dom_node_t *node, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (; node != NULL; node = node->next) {
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (tag != NULL && tag_length == wanted_length
            && strncasecmp(tag, wanted, tag_length) == 0) return node;
        lxb_dom_node_t *found = experimental_find_element_tag(
            node->first_child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

static bool experimental_build_script_corpus(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    PocDocument *corpus_document, size_t *script_count,
    size_t *corpus_bytes)
{
    if (experimental == NULL || experimental->store == NULL
        || experimental->budget == NULL || navigation == NULL
        || corpus_document == NULL || script_count == NULL
        || corpus_bytes == NULL) return false;
    size_t overhead = navigation->maximum_scripts <=
                          (SIZE_MAX - 256) / 512
                      ? navigation->maximum_scripts * 512 + 256
                      : SIZE_MAX;
    if (overhead == SIZE_MAX
        || navigation->maximum_script_bytes > SIZE_MAX - overhead) {
        return false;
    }
    ExperimentalScriptCorpus corpus = {
        .experimental = experimental,
        .limit = navigation->maximum_script_bytes + overhead,
        .script_limit = navigation->maximum_scripts
    };
    experimental->script_frontier_count = 0;
    static const char prefix[] = "<!doctype html><html><head>";
    static const char body_open[] = "</head><body>";
    static const char suffix[] = "</body></html>";
    if (!experimental_script_corpus_append(
            &corpus, prefix, sizeof(prefix) - 1)) corpus.failed = true;
    for (size_t section = 0;
         section < experimental->store->section_count && !corpus.failed;
         section++) {
        char *html = NULL;
        size_t html_length = 0;
        PocDocument document = {0};
        if (!section_store_extract_html(
                experimental->store, section, &html, &html_length)
            || !document_parse(&document, experimental->budget, html,
                               html_length, 4096)) {
            document_destroy(&document);
            budget_free(experimental->budget, html);
            corpus.failed = true;
            break;
        }
        lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
        if (section == 0) {
            lxb_dom_node_t *head = experimental_find_element_tag(root, "head");
            if (head != NULL) {
                experimental_script_corpus_walk(
                    head->first_child, &document, section, false, &corpus);
            }
            if (!corpus.failed
                && !experimental_script_corpus_append(
                       &corpus, body_open, sizeof(body_open) - 1)) {
                corpus.failed = true;
            }
        }
        lxb_dom_node_t *body = document_body_node(&document);
        if (body != NULL) {
            experimental_script_corpus_walk(
                body->first_child, &document, section, true, &corpus);
        }
        document_destroy(&document);
        budget_free(experimental->budget, html);
    }
    if (!corpus.failed
        && !experimental_script_corpus_append(
               &corpus, suffix, sizeof(suffix) - 1)) corpus.failed = true;
    bool success = !corpus.failed
        && document_parse(corpus_document, experimental->budget,
                          corpus.data, corpus.length, 4096);
    *script_count = corpus.scripts;
    *corpus_bytes = corpus.length;
    budget_free(experimental->budget, corpus.data);
    if (!success) document_destroy(corpus_document);
    return success;
}

static void experimental_script_frontier(void *opaque, size_t script_index,
                                         bool active)
{
    ExperimentalSectionContext *experimental = opaque;
    if (experimental == NULL || !active
        || script_index >= experimental->script_frontier_count) {
        if (experimental != NULL) {
            experimental->script_frontier_active = false;
            if (experimental->navigation != NULL) {
                script_runtime_clear_current_script_source(
                    experimental->navigation->page.runtime);
            }
        }
        return;
    }
    experimental->script_visible_section =
        experimental->script_frontier_sections[script_index];
    experimental->script_visible_ordinal =
        experimental->script_frontier_ordinals[script_index];
    experimental->script_visible_node_ordinal =
        experimental->script_frontier_node_ordinals[script_index];
    experimental->script_body_visible =
        experimental->script_frontier_body_visible[script_index];
    experimental->script_frontier_active = true;
    if (experimental->navigation == NULL
        || !script_runtime_set_current_script_source(
               experimental->navigation->page.runtime,
               experimental->script_visible_section,
               experimental->script_visible_ordinal)) {
        experimental->script_frontier_failed = true;
    }
}

static void experimental_register_script_source_nodes_walk(
    lxb_dom_node_t *node, ExperimentalSectionContext *experimental,
    ScriptRuntime *runtime, size_t *script_index, bool *success)
{
    for (; node != NULL && *success; node = node->next) {
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (tag != NULL && tag_length == 6
            && strncasecmp(tag, "script", 6) == 0) {
            size_t index = (*script_index)++;
            if (index >= experimental->script_frontier_count
                || !script_runtime_register_script_source_node(
                       runtime, node,
                       experimental->script_frontier_sections[index],
                       experimental->script_frontier_ordinals[index])) {
                *success = false;
                return;
            }
        }
        experimental_register_script_source_nodes_walk(
            node->first_child, experimental, runtime,
            script_index, success);
    }
}

static bool experimental_execute_all_document_scripts(
    ExperimentalSectionContext *experimental, NavigationSession *navigation)
{
    if (navigation == NULL || !navigation->document_scripts_enabled)
        return true;
    PocDocument corpus = {0};
    size_t script_count = 0, corpus_bytes = 0;
    experimental->script_frontier_failed = false;
    bool success = experimental_build_script_corpus(
        experimental, navigation, &corpus, &script_count, &corpus_bytes);
    size_t registered = 0;
    if (success) {
        experimental_register_script_source_nodes_walk(
            lxb_dom_interface_node(corpus.html), experimental,
            navigation->page.runtime, &registered, &success);
        success = success
            && registered == experimental->script_frontier_count;
    }
    if (success) {
        success = navigation_execute_document_scripts_streaming_from(
                navigation, &corpus, experimental_script_frontier,
                experimental)
            && !experimental->script_frontier_failed;
    }
    script_runtime_detach_document(navigation->page.runtime, &corpus);
    script_runtime_clear_script_source_nodes(navigation->page.runtime);
    experimental->script_frontier_active = false;
    document_destroy(&corpus);
    if (!success) return false;
    memset(experimental->executed_sections, 0xff,
           experimental->executed_section_bytes);
    printf("experimental-scripts mode=document-order scripts=%zu bytes=%zu "
           "sections=%zu\n",
           script_count, corpus_bytes, experimental->store->section_count);
    return true;
}

static void experimental_record_first_tile(
    ExperimentalSectionContext *experimental)
{
    if (experimental == NULL || !experimental->first_tile_pending
        || experimental->pending_swap_started_ns == 0) return;
    uint64_t finished = tilefinch_platform_monotonic_time_ns();
    uint64_t elapsed = finished >= experimental->pending_swap_started_ns
        ? (finished - experimental->pending_swap_started_ns)
              / UINT64_C(1000)
        : 0;
    experimental->first_tile_total_us += elapsed;
    if (elapsed > experimental->first_tile_max_us)
        experimental->first_tile_max_us = elapsed;
    experimental->first_tile_samples++;
    experimental->first_tile_pending = false;
    printf("experimental-first-tile section=%zu elapsed-us=%llu\n",
           experimental->pager.current_section,
           (unsigned long long) elapsed);
    if (!section_pager_prefetch_pending(&experimental->pager)) {
        printf("experimental-prefetch status=skipped reason=budget\n");
    }
}

static bool experimental_install_navigation_callbacks(
    NavigationSession *navigation, ExperimentalSectionContext *experimental)
{
    DocumentBackingOps ops = {
        .element_lookup = experimental_remote_element_lookup,
        .selector_lookup = experimental_remote_selector_lookup,
        .selector_collect = experimental_remote_selector_collect,
        .descendant_collect = experimental_remote_descendant_collect,
        .node_read = experimental_remote_node_read,
        .node_write = experimental_remote_node_write,
        .node_visibility = experimental_node_visible
    };
    if (!document_backing_init_sections(
            &experimental->backing, experimental->store,
            &experimental->pager, &ops, experimental)) return false;
    return experimental->engine != NULL
        ? browser_engine_bind_document_backing(
              experimental->engine, &experimental->backing)
        : document_backing_install(&experimental->backing, navigation);
}
