bool navigation_back(NavigationSession *session, const NavigationEntry **entry)
{
    if (session == NULL || session->history_count == 0
        || session->history_index == 0) return false;
    session->history_index--;
    if (entry != NULL) *entry = &session->history[session->history_index];
    return true;
}

bool navigation_forward(NavigationSession *session,
                        const NavigationEntry **entry)
{
    if (session == NULL || session->history_count == 0
        || session->history_index + 1 >= session->history_count) return false;
    session->history_index++;
    if (entry != NULL) *entry = &session->history[session->history_index];
    return true;
}

const NavigationEntry *navigation_current(const NavigationSession *session)
{
    if (session == NULL || session->history_count == 0
        || session->history_index >= session->history_count) return NULL;
    return &session->history[session->history_index];
}

bool navigation_replace_history(
    NavigationSession *session, const NavigationHistoryRecord *records,
    size_t count, size_t current_index)
{
    if (session == NULL || records == NULL || count == 0
        || count > session->history_capacity || current_index >= count) {
        return false;
    }
    NavigationEntry staged[64] = {{0}};
    for (size_t index = 0; index < count; index++) {
        const NavigationHistoryRecord *source = &records[index];
        if (source->url == NULL || source->url[0] == '\0'
            || source->title == NULL
            || strnlen(source->url, NAVIGATION_URL_LIMIT)
                   >= NAVIGATION_URL_LIMIT
            || strnlen(source->title, NAVIGATION_TITLE_LIMIT)
                   >= NAVIGATION_TITLE_LIMIT) {
            for (size_t retained = 0; retained < index; retained++)
                entry_destroy(session->budget, &staged[retained]);
            return false;
        }
        staged[index].url = bounded_copy(
            session->budget, source->url, NAVIGATION_URL_LIMIT);
        staged[index].title = bounded_copy(
            session->budget, source->title, NAVIGATION_TITLE_LIMIT);
        if (staged[index].url == NULL || staged[index].title == NULL) {
            for (size_t retained = 0; retained <= index; retained++)
                entry_destroy(session->budget, &staged[retained]);
            return false;
        }
        staged[index].scroll_y =
            source->scroll_y < 0 ? 0 : source->scroll_y;
        staged[index].focus_kind = source->focus_kind;
        staged[index].focus_index = source->focus_index;
    }
    for (size_t index = 0; index < session->history_count; index++)
        entry_destroy(session->budget, &session->history[index]);
    memset(session->history, 0,
           session->history_capacity * sizeof(*session->history));
    memcpy(session->history, staged, count * sizeof(*staged));
    session->history_count = count;
    session->history_index = current_index;
    return true;
}

bool navigation_commit_same_document_url(NavigationSession *session,
                                         const char *url)
{
    const NavigationEntry *current = navigation_current(session);
    if (session == NULL || url == NULL || url[0] == '\0' || current == NULL
        || strlen(url) >= NAVIGATION_URL_LIMIT) return false;
    char old_url[NAVIGATION_URL_LIMIT];
    snprintf(old_url, sizeof(old_url), "%s", current->url);
    char observed_old_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, old_url, observed_old_base)) {
        return false;
    }
    char next_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, url, next_base)) return false;
    if (session->page.runtime != NULL
        && !script_runtime_commit_same_document(
               session->page.runtime, url, old_url,
               &session->page.script_result)) return false;
    if (!history_push(session, url, session->page.document.title)) {
        if (session->page.runtime != NULL) {
            (void) script_runtime_restore_same_document(
                session->page.runtime, old_url, url,
                &session->page.script_result);
        }
        return false;
    }
    snprintf(session->page.resource_base_url,
             sizeof(session->page.resource_base_url), "%s", next_base);
    return true;
}

bool navigation_restore_same_document_url(NavigationSession *session,
                                          const char *url,
                                          const char *old_url)
{
    const NavigationEntry *current = navigation_current(session);
    if (session == NULL || url == NULL || old_url == NULL || current == NULL
        || strcmp(current->url, url) != 0) return false;
    char observed_old_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, old_url, observed_old_base)) {
        return false;
    }
    char next_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, url, next_base)) return false;
    bool restored = session->page.runtime == NULL
        || script_runtime_restore_same_document(
               session->page.runtime, url, old_url,
               &session->page.script_result);
    if (restored) {
        snprintf(session->page.resource_base_url,
                 sizeof(session->page.resource_base_url), "%s", next_base);
    }
    return restored;
}

void navigation_discard_current_page(NavigationSession *session)
{
    if (session != NULL) page_destroy(session);
}

bool navigation_set_scroll(NavigationSession *session, int scroll_y)
{
    NavigationEntry *entry = session != NULL && session->history_count != 0
                             ? &session->history[session->history_index] : NULL;
    int next = scroll_y < 0 ? 0 : scroll_y;
    if (session != NULL && session->page.runtime != NULL) {
        /* Native scrolling is a user-agent operation; a page exception only
           prevents the advisory JS reflection, never the viewport movement. */
        (void) script_runtime_set_page_scroll(session->page.runtime, next);
    }
    if (entry != NULL) entry->scroll_y = next;
    return entry != NULL;
}

static void navigation_clamp_scroll_to_layout(NavigationSession *session)
{
    NavigationEntry *entry = session != NULL && session->history_count != 0
        ? &session->history[session->history_index] : NULL;
    if (entry == NULL) return;
    int maximum = viewport_max_scroll_css(
        &session->viewport, session->page.layout.height);
    if (entry->scroll_y <= maximum) return;
    /* A relayout may shrink the document under an existing scroll position.
       Keep the native viewport valid even if page script throws while its
       scroll state is being synchronized. */
    if (session->page.runtime != NULL) {
        (void) script_runtime_set_page_scroll(
            session->page.runtime, maximum);
    }
    entry->scroll_y = maximum;
}

static bool navigation_selector_mentions_attribute(
    const Stylesheet *sheet, const char *attribute)
{
    if (sheet == NULL || attribute == NULL || attribute[0] == '\0') {
        return true;
    }
    size_t attribute_length = strlen(attribute);
    size_t selector_count = sheet->count + sheet->custom_rule_count;
    for (size_t i = 0; i < selector_count; i++) {
        const char *selector = i < sheet->count
            ? sheet->rules[i].selector
            : sheet->custom_rules[i - sheet->count].selector;
        size_t length = selector == NULL ? 0 : strlen(selector);
        for (size_t at = 0; selector != NULL && at < length; at++) {
            if (selector[at] != '[') continue;
            size_t name = at + 1u;
            while (name < length
                   && isspace((unsigned char) selector[name])) name++;
            /* Namespace-qualified and escaped attribute names are rare and
               not worth approximating here. They remain conservative while
               ordinary unrelated [data-*] selectors no longer invalidate
               every ARIA-only mutation on a large page. */
            if (name < length && (selector[name] == '\\'
                                  || (selector[name] == '*'
                                      && name + 1u < length
                                      && selector[name + 1u] == '|'))) {
                return true;
            }
            size_t end = name;
            while (end < length) {
                unsigned char value = (unsigned char) selector[end];
                if (!(isalnum(value) || value == '-' || value == '_')) break;
                end++;
            }
            if (end < length && selector[end] == '|'
                && (end + 1u >= length || selector[end + 1u] != '=')) {
                return true;
            }
            if (end - name == attribute_length
                && strncasecmp(selector + name, attribute,
                               attribute_length) == 0) return true;
        }
    }
    for (size_t i = 0; i < sheet->declaration_count; i++) {
        const StyleDeclaration *declaration = &sheet->declarations[i];
        const ComputedStyle *values = &declaration->values;
        if (values->generated_attr != NULL
            && values->generated_attr_length == attribute_length
            && memcmp(values->generated_attr, attribute,
                      attribute_length) == 0) {
            return true;
        }
        /* A var()-dependent declaration is resolved against the element at
           match time, so its final attr() name is not necessarily present in
           values yet. Relayout rather than guessing about generated text. */
        if (declaration->deferred_declarations != NULL
            && strstr(declaration->deferred_declarations, "attr(") != NULL) {
            return true;
        }
    }
    return false;
}

static bool navigation_mutations_are_semantic_only(
    const NavigationSession *session, const ScriptMutationJournal *mutations)
{
    if (session == NULL || mutations == NULL || mutations->count == 0
        || mutations->overflowed || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required) {
        return false;
    }
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || strncmp(record->attribute, "aria-", 5) != 0
            || navigation_selector_mentions_attribute(
                   &session->page.stylesheet, record->attribute)) {
            return false;
        }
    }
    return true;
}

static bool navigation_focus_parent_style(
    const Stylesheet *sheet, lxb_dom_node_t *node, ComputedStyle *parent,
    bool *has_parent)
{
    if (sheet == NULL || node == NULL || parent == NULL
        || has_parent == NULL) return false;
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = at;
    }
    *parent = (ComputedStyle) {0};
    *has_parent = false;
    while (count != 0) {
        *parent = style_for_node(
            sheet, ancestors[--count], *has_parent ? parent : NULL);
        *has_parent = true;
    }
    return true;
}

typedef struct {
    lxb_dom_node_t *node;
    ComputedStyle normal;
    ComputedStyle focused;
    StyleFocusChange change;
    bool focused_now;
} NavigationFocusPaintCandidate;

static bool navigation_mutations_are_paint_only_focus(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool *border_paint)
{
    enum { NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT = 4 };
    if (border_paint != NULL) *border_paint = false;
    if (session == NULL || mutations == NULL || mutations->count == 0
        || mutations->count > NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT
        || mutations->overflowed || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required
        || mutations->conservative_resource_scan) return false;
    NavigationFocusPaintCandidate candidates[
        NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT] = {0};
    lxb_dom_node_t *focused_node = NULL;
    ComputedStyle focused_style = {0};
    bool has_border_paint = false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || record->node == NULL
            || strcmp(record->attribute, "data-tilefinch-focus") != 0) {
            return false;
        }
        ComputedStyle parent = {0};
        bool has_parent = false;
        if (!navigation_focus_parent_style(
                &session->page.stylesheet, record->node,
                &parent, &has_parent)
            || (candidates[i].change = style_focus_change_classify(
                    &session->page.stylesheet, record->node,
                    has_parent ? &parent : NULL,
                    &candidates[i].normal,
                    &candidates[i].focused))
                   == STYLE_FOCUS_CHANGE_UNSAFE) {
            return false;
        }
        candidates[i].node = record->node;
        size_t focus_length = 0;
        candidates[i].focused_now = document_attribute(
            record->node, "data-tilefinch-focus", &focus_length) != NULL;
        if (candidates[i].change == STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY) {
            const ComputedStyle *target = candidates[i].focused_now
                ? &candidates[i].focused : &candidates[i].normal;
            if (!layout_apply_focus_border_paint(
                    &session->page.layout, &session->page.stylesheet,
                    candidates[i].node, target, true,
                    NULL, NULL, NULL, NULL)) return false;
            has_border_paint = true;
        }
        if (candidates[i].focused_now) {
            focused_node = record->node;
            focused_style = candidates[i].focused;
        }
    }
    if (has_border_paint) {
        bool damage_valid = false;
        int damage_left = 0, damage_top = 0, damage_right = 0, damage_bottom = 0;
        for (size_t i = 0; i < mutations->count; i++) {
            if (candidates[i].change
                    != STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY) continue;
            const ComputedStyle *target = candidates[i].focused_now
                ? &candidates[i].focused : &candidates[i].normal;
            int left = 0, top = 0, right = 0, bottom = 0;
            if (!layout_apply_focus_border_paint(
                    &session->page.layout, &session->page.stylesheet,
                    candidates[i].node, target, false,
                    &left, &top, &right, &bottom)) return false;
            if (!damage_valid) {
                damage_left = left;
                damage_top = top;
                damage_right = right;
                damage_bottom = bottom;
                damage_valid = true;
            } else {
                if (left < damage_left) damage_left = left;
                if (top < damage_top) damage_top = top;
                if (right > damage_right) damage_right = right;
                if (bottom > damage_bottom) damage_bottom = bottom;
            }
        }
        if (!damage_valid) return false;
        session->focus_paint_damage_valid = true;
        session->focus_paint_damage_left = damage_left;
        session->focus_paint_damage_top = damage_top;
        session->focus_paint_damage_right = damage_right;
        session->focus_paint_damage_bottom = damage_bottom;
        session->focus_paint_generation++;
        if (border_paint != NULL) *border_paint = true;
    }
    unsigned width = computed_style_outline_width(&focused_style);
    unsigned line_style = computed_style_outline_style(&focused_style);
    if (focused_node != NULL && width != 0 && width <= UINT8_MAX
        && line_style != STYLE_OUTLINE_NONE && line_style <= UINT8_MAX) {
        bool current_color =
            (focused_style.outline_state & STYLE_OUTLINE_CURRENT_COLOR) != 0;
        session->focus_outline_cache_node = focused_node;
        session->focus_outline_cache_stylesheet_generation =
            session->page.stylesheet.build_generation;
        session->focus_outline_cache_relayout_generation =
            session->incremental_relayouts;
        session->focus_outline_cache_color =
            current_color ? focused_style.color : focused_style.outline_color;
        session->focus_outline_cache_offset =
            computed_style_outline_offset(&focused_style);
        session->focus_outline_cache_alpha =
            current_color
              ? focused_style.color_alpha : focused_style.outline_alpha;
        session->focus_outline_cache_width = (uint8_t) width;
        session->focus_outline_cache_style = (uint8_t) line_style;
        session->focus_outline_cache_valid =
            session->focus_outline_cache_alpha != 0;
    }
    return true;
}

static void navigation_prepare_layout_reuse(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool full_rebuild)
{
    if (session == NULL || session->page.layout_reuse == NULL) return;
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &before);
    bool destructive = full_rebuild || mutations == NULL
        || mutations->overflowed;
    if (!destructive) {
        for (size_t i = 0; i < mutations->count; i++) {
            ScriptMutationKind kind = mutations->records[i].kind;
            if (kind == SCRIPT_MUTATION_INNER_HTML
                || kind == SCRIPT_MUTATION_CHILD_LIST
                || kind == SCRIPT_MUTATION_UNKNOWN) {
                destructive = true;
                break;
            }
        }
    }
    if (destructive) {
        layout_reuse_cache_reset(session->page.layout_reuse);
    } else {
        for (size_t i = 0; i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && strcmp(
                    record->attribute, "data-tilefinch-focus") == 0) {
                layout_reuse_cache_invalidate_focus(
                    session->page.layout_reuse, record->node);
            } else if (mutations->relational_selector_sensitive) {
                layout_reuse_cache_invalidate_node(
                    session->page.layout_reuse, record->node,
                    record->kind == SCRIPT_MUTATION_TEXT);
            } else {
                layout_reuse_cache_invalidate_node_scoped(
                    session->page.layout_reuse, record->node,
                    record->kind == SCRIPT_MUTATION_TEXT);
            }
        }
    }
    layout_reuse_cache_stats(session->page.layout_reuse, &after);
    if (after.scoped_invalidations >= before.scoped_invalidations) {
        session->performance.layout_reuse_scoped_invalidations +=
            after.scoped_invalidations - before.scoped_invalidations;
    }
    if (after.full_resets >= before.full_resets) {
        session->performance.layout_reuse_full_resets +=
            after.full_resets - before.full_resets;
    }
}

static void navigation_evict_layout_reuse_under_pressure(
    NavigationSession *session)
{
    if (session == NULL || session->page.layout_reuse == NULL) return;
    if (!budget_pressure_required(
            session->budget, 384u * 1024u, 512u * 1024u)) return;
    layout_reuse_cache_destroy(session->page.layout_reuse);
    session->page.layout_reuse = NULL;
    session->performance.layout_reuse_pressure_evictions++;
    session->performance.layout_reuse_retained_bytes = 0;
}

static bool navigation_image_refresh_add(
    lxb_dom_node_t **nodes, size_t *count, size_t capacity,
    lxb_dom_node_t *node)
{
    if (node == NULL) return true;
    for (size_t known = 0; known < *count; known++) {
        if (nodes[known] == node) return true;
    }
    if (*count == capacity) return false;
    nodes[(*count)++] = node;
    return true;
}

static lxb_dom_node_t *navigation_inline_svg_root(lxb_dom_node_t *node)
{
    size_t depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL && depth++ < 64;
         at = at->parent) {
        if (node_name_is(at, "svg")) return at;
    }
    return NULL;
}

static bool navigation_collect_inline_svg_descendants(
    lxb_dom_node_t *root, lxb_dom_node_t **nodes,
    size_t *count, size_t capacity)
{
    enum { MAXIMUM_WORK = 256, MAXIMUM_DEPTH = 48 };
    if (root == NULL) return true;
    lxb_dom_node_t *node = root;
    size_t work = 0, depth = 0;
    for (;;) {
        if (++work > MAXIMUM_WORK) return false;
        if (node_name_is(node, "svg")
            && !navigation_image_refresh_add(
                nodes, count, capacity, node)) return false;
        if (node->first_child != NULL && depth < MAXIMUM_DEPTH) {
            node = node->first_child;
            depth++;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
            if (depth != 0) depth--;
        }
        if (node == root) break;
        node = node->next;
    }
    return true;
}

static size_t navigation_mutation_image_refresh_nodes(
    const ScriptMutationJournal *mutations, lxb_dom_node_t **nodes,
    size_t capacity, bool *bounded_out)
{
    if (bounded_out != NULL) *bounded_out = false;
    if (mutations == NULL || nodes == NULL || capacity == 0) return 0;
    size_t count = 0;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->node == NULL) continue;
        if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
            && node_name_is(record->node, "img")) {
            if (!navigation_image_refresh_add(
                    nodes, &count, capacity, record->node)) {
                if (bounded_out != NULL) *bounded_out = true;
                return count;
            }
        }
        lxb_dom_node_t *svg = navigation_inline_svg_root(record->node);
        if (svg != NULL) {
            if (!navigation_image_refresh_add(
                    nodes, &count, capacity, svg)) {
                if (bounded_out != NULL) *bounded_out = true;
                return count;
            }
            continue;
        }
        bool ancestor_style_change =
            record->kind == SCRIPT_MUTATION_INLINE_STYLE
            || (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && (strcmp(record->attribute, "class") == 0
                    || strcmp(record->attribute, "id") == 0
                    || strcmp(record->attribute, "style") == 0
                    || strcmp(record->attribute, "color") == 0));
        if (ancestor_style_change
            && !navigation_collect_inline_svg_descendants(
                record->node, nodes, &count, capacity)) {
            if (bounded_out != NULL) *bounded_out = true;
            return count;
        }
    }
    return count;
}

bool navigation_relayout(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    session->focus_outline_cache_valid = false;
    session->focus_outline_cache_node = NULL;
    uint64_t started = navigation_now_us();
    const NavigationEntry *entry = navigation_current(session);
    ScriptMutationJournal mutations = {0};
    bool mutation_driven = session->page.runtime != NULL
        && script_runtime_consume_mutations(session->page.runtime,
                                            &mutations);
    bool conservative_scan = !mutation_driven
        || mutations.conservative_resource_scan;
    bool full_rebuild = session->style_rebuild_required
        || mutations.resource_rebuild_required;
    bool image_resource_scan = mutation_driven
        && mutations.image_resource_scan_required;
    bool image_resource_refresh = mutation_driven
        && mutations.image_resource_refresh_required;
    bool focus_border_paint = false;
    if (mutation_driven
        && navigation_mutations_are_paint_only_focus(
            session, &mutations, &focus_border_paint)) {
        if (focus_border_paint) {
            session->performance.focus_paint_relayout_skips++;
        } else {
            session->performance.focus_outline_relayout_skips++;
        }
        return true;
    }
    if (mutation_driven
        && navigation_mutations_are_semantic_only(session, &mutations)) {
        session->performance.semantic_relayout_skips++;
        return true;
    }
    uint64_t fingerprint = session->resource_fingerprint;
    if (mutations.overflowed) {
        session->performance.mutation_journal_overflows++;
    }
    if (mutation_driven && conservative_scan) {
        session->performance.mutation_conservative_scans++;
    }
    if (full_rebuild || conservative_scan || image_resource_scan
        || image_resource_refresh) {
        if (!navigation_resource_fingerprint(
                session, &session->page.document, &fingerprint)) {
            return false;
        }
        if (!full_rebuild && conservative_scan) {
            full_rebuild = fingerprint != session->resource_fingerprint;
        }
    }
    if (!full_rebuild && session->external_resources_enabled
        && (image_resource_scan || image_resource_refresh)) {
        const char *document_url =
            session->page.document_url[0] == '\0'
            ? (entry == NULL ? NULL : entry->url)
            : session->page.document_url;
        bool images_loaded = document_url != NULL;
        bool scan_for_additions = image_resource_scan;
        if (images_loaded && image_resource_refresh) {
            lxb_dom_node_t *candidates[SCRIPT_MUTATION_JOURNAL_LIMIT];
            lxb_dom_node_t *refresh[SCRIPT_MUTATION_JOURNAL_LIMIT];
            bool refresh_bounded_out = false;
            size_t candidate_count =
                navigation_mutation_image_refresh_nodes(
                    &mutations, candidates,
                    sizeof(candidates) / sizeof(candidates[0]),
                    &refresh_bounded_out);
            if (refresh_bounded_out) {
                full_rebuild = true;
                images_loaded = false;
            }
            size_t refresh_count = 0;
            for (size_t i = 0;
                 !refresh_bounded_out && i < candidate_count; i++) {
                if (images_find_node(
                        &session->page.images, candidates[i]) == NULL) {
                    /* Frameworks commonly connect a lazy <img> before
                       assigning src. There is no stale surface to retire;
                       let the deduplicating additive traversal discover it
                       alongside any CSS image in the new subtree. */
                    scan_for_additions = true;
                } else {
                    refresh[refresh_count++] = candidates[i];
                }
            }
            if (!refresh_bounded_out) {
                images_loaded =
                    refresh_count == 0
                    || images_refresh_external_nodes(
                    &session->page.document, &session->page.stylesheet,
                    &session->page.images, refresh, refresh_count,
                    session->budget,
                    session->page.resource_base_url, document_url,
                    session->page.referrer_policy,
                    session->maximum_images, session->maximum_image_bytes,
                    session->maximum_image_file_bytes,
                    session->maximum_decoded_image_bytes,
                    session->resource_timeout_ms,
                    session->page.resource_scheduler,
                    session->browser_session);
            }
        }
        if (images_loaded && scan_for_additions) {
            navigation_ensure_layout_reuse(session);
            images_loaded = images_load_external_reusing_layout_styles(
                &session->page.document, &session->page.stylesheet,
                &session->page.images, session->budget,
                session->page.resource_base_url, document_url,
                session->page.referrer_policy,
                session->maximum_images, session->maximum_image_bytes,
                session->maximum_image_file_bytes,
                session->maximum_decoded_image_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session, session->page.layout_reuse,
                session->fonts, session->viewport.css_width);
        }
        session->images = &session->page.images;
        layout_reuse_cache_update_images(
            session->page.layout_reuse, session->images);
        session->performance.mutation_image_resource_scans++;
        if (!images_loaded) full_rebuild = true;
    }
    if (!full_rebuild) {
        if (image_resource_scan || image_resource_refresh) {
            session->resource_fingerprint = fingerprint;
        }
        navigation_prepare_layout_reuse(session, &mutations, false);
        navigation_evict_layout_reuse_under_pressure(session);
        LayoutDocument next = {0};
        if (!navigation_build_layout(session, &next, session->fonts,
                                     session->images)) {
            return false;
        }
        layout_transfer_scroll_state(&session->page.layout, &next);
        navigation_measure_damage(session, &session->page.layout, &next);
        layout_destroy(&session->page.layout);
        session->page.layout = next;
        session->incremental_relayouts++;
        session->performance.fast_relayouts++;
        if (mutation_driven) {
            session->performance.mutation_fast_relayouts++;
        }
        uint64_t elapsed = navigation_record_layout_slices(
            session, started, &next);
        session->performance.layout_us += elapsed;
        session->performance.relayout_us += elapsed;
        navigation_attach_layout(session);
        navigation_clamp_scroll_to_layout(session);
        return true;
    }
    int old_height = session->page.layout.height;
    navigation_prepare_layout_reuse(session, &mutations, true);
    navigation_evict_layout_reuse_under_pressure(session);
    fonts_external_loader_cancel(
        &session->page.external_font_loader,
        session->page.resource_scheduler, &session->page.external_fonts);
    layout_destroy(&session->page.layout);
    stylesheet_destroy(&session->page.stylesheet);
    if (!navigation_build_stylesheet(session)) {
        session->page.loaded = false;
        return false;
    }
    /* Source the document URL from the committed page, not the history
       entry: a resource-driven rebuild must re-request external CSS even
       when the page was loaded without recording history (or history was
       trimmed).  Without this, the rebuilt stylesheet silently loses every
       external sheet and collapses to the document's inline styles. */
    const char *rebuild_url = session->page.document_url[0] != '\0'
        ? session->page.document_url
        : (entry != NULL ? entry->url : NULL);
    if (session->external_resources_enabled && rebuild_url != NULL) {
        if (!navigation_document_resource_base(
                &session->page.document, rebuild_url,
                session->page.resource_base_url)) {
            session->page.loaded = false;
            return false;
        }
        images_destroy(&session->page.images);
        if (!stylesheets_load_external_tracked_with_context(
                &session->page.document, &session->page.stylesheet,
                session->budget, session->page.resource_base_url, rebuild_url,
                session->page.referrer_policy,
                session->maximum_stylesheets,
                session->maximum_stylesheet_bytes,
                session->maximum_stylesheet_file_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session,
                &session->page.stylesheet_resources,
                &session->page.external_stylesheets)) {
            session->page.loaded = false;
            return false;
        }
        if (!navigation_dispatch_stylesheet_events(
                session, session->page.resource_base_url)
            && !navigation_retire_exhausted_script_realms(session)) {
            session->page.loaded = false;
            return false;
        }
        navigation_ensure_layout_reuse(session);
        if (!images_load_external_reusing_layout_styles(
                &session->page.document, &session->page.stylesheet,
                &session->page.images, session->budget,
                session->page.resource_base_url,
                rebuild_url, session->page.referrer_policy,
                session->maximum_images, session->maximum_image_bytes,
                session->maximum_image_file_bytes,
                session->maximum_decoded_image_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session, session->page.layout_reuse,
                session->fonts, session->viewport.css_width)) {
            session->page.loaded = false;
            return false;
        }
        session->images = &session->page.images;
    }
    if (session->user_css_length != 0
        && !stylesheet_add_user_css(&session->page.stylesheet,
                                    session->user_css,
                                    session->user_css_length)) {
        session->page.loaded = false;
        return false;
    }
    if (session->web_fonts_enabled && rebuild_url != NULL
        && !navigation_load_web_fonts(session, rebuild_url)) {
        session->page.loaded = false;
        return false;
    }
    if (!navigation_build_layout(session, &session->page.layout,
                                 session->fonts, session->images)) {
        session->page.loaded = false;
        return false;
    }
    session->resource_fingerprint = fingerprint;
    session->style_rebuild_required = false;
    session->relayout_damage_valid = true;
    session->relayout_damage_left = 0;
    session->relayout_damage_top = 0;
    session->relayout_damage_right = session->viewport.css_width;
    session->relayout_damage_bottom = old_height > session->page.layout.height
                                      ? old_height : session->page.layout.height;
    session->incremental_relayouts++;
    session->performance.full_relayouts++;
    if (mutations.resource_rebuild_required) {
        session->performance.mutation_resource_rebuilds++;
    }
    session->performance.relayout_us += navigation_record_layout_slices(
        session, started, &session->page.layout);
    navigation_attach_layout(session);
    navigation_clamp_scroll_to_layout(session);
    return true;
}

static const DrawCommand *navigation_scroll_text_anchor(
    const LayoutDocument *layout, int scroll_y, int *viewport_offset)
{
    if (viewport_offset != NULL) *viewport_offset = 0;
    if (layout == NULL) return NULL;
    const DrawCommand *fallback = NULL;
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length == 0
            || draw_command_is_text_shadow(command)) continue;
        if (fallback == NULL) fallback = command;
        int64_t bottom = (int64_t) command->y + command->height;
        if (bottom <= scroll_y) continue;
        if (viewport_offset != NULL)
            *viewport_offset = command->y - scroll_y;
        return command;
    }
    return fallback;
}

static int navigation_scroll_for_text_anchor(
    const LayoutDocument *layout, const DrawCommand *anchor,
    int viewport_offset, int fallback)
{
    if (layout == NULL || anchor == NULL || anchor->text == NULL)
        return fallback;
    uintptr_t wanted = (uintptr_t) anchor->text;
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length == 0
            || draw_command_is_text_shadow(command)) continue;
        uintptr_t start = (uintptr_t) command->text;
        if (start > UINTPTR_MAX - command->text_length) continue;
        if (wanted >= start && wanted < start + command->text_length) {
            int64_t target = (int64_t) command->y - viewport_offset;
            return target <= 0 ? 0
                : (target > INT_MAX ? INT_MAX : (int) target);
        }
    }
    return fallback;
}

bool navigation_apply_user_css(NavigationSession *session, const char *css,
                               size_t length)
{
    if (session == NULL || session->budget == NULL || !session->page.loaded
        || (css == NULL && length != 0) || length > 128u * 1024u)
        return false;
    char *copy = NULL;
    if (length != 0) {
        copy = budget_malloc(session->budget, length + 1u);
        if (copy == NULL) return false;
        memcpy(copy, css, length);
        copy[length] = '\0';
    }

    Stylesheet next_sheet = {0};
    StylesheetDocumentResources next_resources = {0};
    ExternalStylesheetStats next_external = {0};
    LayoutDocument next_layout = {0};
    bool okay = session->external_resources_enabled
        ? stylesheet_build_context_deferred(
              &next_sheet, session->budget, &session->page.document,
              &session->viewport)
        : stylesheet_build_context(
              &next_sheet, session->budget, &session->page.document,
              &session->viewport);
    const char *document_url = session->page.document_url[0] == '\0'
        ? NULL : session->page.document_url;
    if (okay && session->external_resources_enabled) {
        okay = document_url != NULL
            && stylesheets_load_external_tracked_with_context(
                &session->page.document, &next_sheet, session->budget,
                session->page.resource_base_url, document_url,
                session->page.referrer_policy, session->maximum_stylesheets,
                session->maximum_stylesheet_bytes,
                session->maximum_stylesheet_file_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session, &next_resources, &next_external);
    }
    if (okay && length != 0)
        okay = stylesheet_add_user_css(&next_sheet, copy, length);
    uint64_t started = navigation_now_us();
    if (okay) {
        okay = layout_build_context_reuse(
            &next_layout, session->budget, &session->page.document,
            &next_sheet, session->fonts, session->images,
            &session->viewport, NULL);
    }
    if (!okay) {
        layout_destroy(&next_layout);
        stylesheet_destroy(&next_sheet);
        stylesheet_document_resources_destroy(&next_resources);
        budget_free(session->budget, copy);
        return false;
    }

    const NavigationEntry *entry = navigation_current(session);
    int old_scroll = entry == NULL ? 0 : entry->scroll_y;
    int viewport_offset = 0;
    const DrawCommand *anchor = navigation_scroll_text_anchor(
        &session->page.layout, old_scroll, &viewport_offset);
    int anchored_scroll = navigation_scroll_for_text_anchor(
        &next_layout, anchor, viewport_offset, old_scroll);
    int old_height = session->page.layout.height;
    layout_destroy(&session->page.layout);
    stylesheet_destroy(&session->page.stylesheet);
    stylesheet_document_resources_destroy(
        &session->page.stylesheet_resources);
    layout_reuse_cache_destroy(session->page.layout_reuse);
    session->page.layout_reuse = NULL;
    session->page.layout = next_layout;
    session->page.stylesheet = next_sheet;
    session->page.stylesheet_resources = next_resources;
    session->page.external_stylesheets = next_external;
    budget_free(session->budget, session->user_css);
    session->user_css = copy;
    session->user_css_length = length;
    session->style_rebuild_required = false;
    session->relayout_damage_valid = true;
    session->relayout_damage_left = 0;
    session->relayout_damage_top = 0;
    session->relayout_damage_right = session->viewport.css_width;
    session->relayout_damage_bottom = old_height > next_layout.height
        ? old_height : next_layout.height;
    session->incremental_relayouts++;
    session->performance.full_relayouts++;
    session->performance.relayout_us += navigation_record_layout_slices(
        session, started, &session->page.layout);
    if (session->page.runtime != NULL)
        script_runtime_set_stylesheet(
            session->page.runtime, &session->page.stylesheet);
    (void) navigation_set_scroll(session, anchored_scroll);
    navigation_attach_layout(session);
    navigation_clamp_scroll_to_layout(session);
    return true;
}

bool navigation_background_resources_pending(
    const NavigationSession *session)
{
    return session != NULL && session->page.loaded
        && (fonts_external_loader_pending(
                &session->page.external_font_loader)
            || session->page.image_continuation_pending);
}

size_t navigation_cancel_network_work(
    NavigationSession *session, const char *reason)
{
    if (session == NULL) return 0;
    size_t cancelled = fetch_scheduler_cancel_all(
        session->page.resource_scheduler, reason);
    cancelled += script_runtime_cancel_network(
        session->page.runtime, reason);
    for (size_t i = 0; i < session->page.frame_count; i++) {
        cancelled += script_runtime_cancel_network(
            session->page.frames[i].runtime, reason);
    }
    return cancelled;
}

static void navigation_deferred_images_finish(NavigationSession *session)
{
    if (session == NULL) return;
    images_priority_load_destroy(session->page.deferred_image_job);
    session->page.deferred_image_job = NULL;
    budget_free(session->budget, session->page.deferred_image_targets);
    session->page.deferred_image_targets = NULL;
    session->page.deferred_image_count = 0;
    session->page.deferred_image_cursor = 0;
    session->page.deferred_image_rank_scroll_y = 0;
    session->page.deferred_image_rank_valid = false;
    session->page.image_continuation_pending = false;
}

static void navigation_deferred_image_failed(
    NavigationSession *session, ImagePriorityTarget *target)
{
    NavigationPage *page = &session->page;
    session->performance.background_image_failures++;
    if (target != NULL && target->retry_count == 0) {
        /* Eight owner-idle slices are long enough for a just-retired worker
           descriptor or short CDN hiccup to clear, yet do not hold input,
           layout, or the rest of the browser loop. The target stays current
           and is retried exactly once. */
        target->retry_count = 1;
        target->retry_delay = 8;
        return;
    }
    if (target != NULL) {
        target->retry_count = 0;
        target->retry_delay = 0;
    }
    page->deferred_image_cursor++;
}

static uint64_t navigation_deferred_image_distance(
    const NavigationSession *session, const ImagePriorityTarget *target,
    int scroll_y, uint16_t *display_width, uint16_t *display_height)
{
    if (display_width != NULL) *display_width = target->display_width;
    if (display_height != NULL) *display_height = target->display_height;
    int64_t top = scroll_y;
    int64_t bottom = top + session->viewport.css_height;
    for (size_t i = 0; i < session->page.layout.node_box_count; i++) {
        const LayoutNodeBox *box = &session->page.layout.node_boxes[i];
        if (box->node != target->node || box->width <= 0
            || box->height <= 0) continue;
        int width = layout_css_to_visual(&session->page.layout, box->width);
        int height = layout_css_to_visual(&session->page.layout, box->height);
        if (display_width != NULL && width > 0 && width <= UINT16_MAX) {
            *display_width = (uint16_t) width;
        }
        if (display_height != NULL && height > 0 && height <= UINT16_MAX) {
            *display_height = (uint16_t) height;
        }
        int64_t box_top = box->y;
        int64_t box_bottom = box_top + box->height;
        if (box_bottom < top) return (uint64_t) (top - box_bottom);
        if (box_top > bottom) return (uint64_t) (box_top - bottom);
        return 0;
    }
    return UINT64_MAX;
}

static bool navigation_run_deferred_image(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->deferred_image_targets == NULL
        || page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
        return true;
    }
    if (page->deferred_image_job == NULL) {
        ImagePriorityTarget *current =
            &page->deferred_image_targets[page->deferred_image_cursor];
        if (current->retry_delay != 0) {
            current->retry_delay--;
            return true;
        }
        const NavigationEntry *entry = navigation_current(session);
        int scroll_y = entry == NULL ? 0 : entry->scroll_y;
        uint16_t best_width = current->display_width;
        uint16_t best_height = current->display_height;
        /* The committed plan is already viewport ordered. Scan the remaining
           layout boxes only when scrolling changes that order; doing this at
           every request start made a long thumbnail queue O(targets*boxes)
           once per image. A failed target is pinned until its retry resolves
           so a scroll cannot transfer or bypass that target's retry. */
        if (current->retry_count == 0
            && (!page->deferred_image_rank_valid
                || page->deferred_image_rank_scroll_y != scroll_y)) {
            session->performance.background_image_rank_scans++;
            size_t best = page->deferred_image_cursor;
            uint64_t best_distance = navigation_deferred_image_distance(
                session, &page->deferred_image_targets[best], scroll_y,
                &best_width, &best_height);
            for (size_t i = best + 1; i < page->deferred_image_count; i++) {
                uint64_t distance = navigation_deferred_image_distance(
                    session, &page->deferred_image_targets[i], scroll_y,
                    NULL, NULL);
                if (distance < best_distance) {
                    best = i;
                    best_distance = distance;
                }
            }
            if (best != page->deferred_image_cursor) {
                ImagePriorityTarget swap =
                    page->deferred_image_targets[page->deferred_image_cursor];
                page->deferred_image_targets[page->deferred_image_cursor] =
                    page->deferred_image_targets[best];
                page->deferred_image_targets[best] = swap;
                (void) navigation_deferred_image_distance(
                    session,
                    &page->deferred_image_targets[
                        page->deferred_image_cursor],
                    scroll_y, &best_width, &best_height);
            }
            page->deferred_image_rank_scroll_y = scroll_y;
            page->deferred_image_rank_valid = true;
            current = &page->deferred_image_targets[
                page->deferred_image_cursor];
        }
        ImagePriorityTarget *target = current;
        target->display_width = best_width;
        target->display_height = best_height;
        /* The ordinary image count remains the successful-page budget. Add
           back only attempts already lost to failure, so a failed visible
           prefix does not crowd an offscreen thumbnail out while a large
           document still cannot turn the retry rule into extra successes.
           The image loader also clamps this value to its 128-node hard cap. */
        size_t maximum_attempts = session->maximum_images;
        if (page->images.stats.failed <= SIZE_MAX - maximum_attempts) {
            maximum_attempts += page->images.stats.failed;
        } else {
            maximum_attempts = SIZE_MAX;
        }
        page->deferred_image_job = images_priority_load_begin(
            &page->document, &page->stylesheet, &page->images, target,
            session->budget, page->resource_base_url, page->document_url,
            page->referrer_policy, maximum_attempts,
            session->maximum_image_bytes,
            session->maximum_image_file_bytes,
            session->maximum_decoded_image_bytes,
            session->resource_timeout_ms, page->resource_scheduler,
            session->browser_session);
        if (page->deferred_image_job == NULL) {
            navigation_deferred_image_failed(session, target);
            return true;
        }
    }

    size_t loaded_before = page->images.stats.loaded;
    session->performance.background_image_batches++;
    ImagePriorityLoadStatus status = images_priority_load_pump(
        page->deferred_image_job);
    if (status == IMAGE_PRIORITY_LOAD_PENDING) return true;
    images_priority_load_destroy(page->deferred_image_job);
    page->deferred_image_job = NULL;
    ImagePriorityTarget *target =
        &page->deferred_image_targets[page->deferred_image_cursor];
    if (status == IMAGE_PRIORITY_LOAD_FAILED) {
        navigation_deferred_image_failed(session, target);
    } else {
        target->retry_count = 0;
        target->retry_delay = 0;
        page->deferred_image_cursor++;
    }
    size_t added = page->images.stats.loaded >= loaded_before
        ? page->images.stats.loaded - loaded_before : 0;
    if (added <= SIZE_MAX
                 - session->performance.background_images_loaded) {
        session->performance.background_images_loaded += added;
    } else {
        session->performance.background_images_loaded = SIZE_MAX;
    }
    session->images = &page->images;
    navigation_ensure_layout_reuse(session);
    layout_reuse_cache_update_images(page->layout_reuse, &page->images);
    if (added != 0) {
        if (!navigation_relayout(session)) {
            /* Optional pixels must never retire an already usable display
               list under pressure. The transactional relayout left it
               intact; stop the queue and keep browsing. */
            session->performance.background_image_failures++;
            navigation_deferred_images_finish(session);
            return true;
        }
        session->performance.background_image_relayouts++;
    }
    if (page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
    }
    return true;
}

bool navigation_run_background_resources(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    if (fonts_external_loader_pending(
            &session->page.external_font_loader)) {
        bool face_loaded = false;
        size_t failures_before = session->page.external_fonts.failed;
        uint64_t started = navigation_now_us();
        session->performance.background_font_slices++;
        if (!fonts_external_loader_step(
                &session->page.external_font_loader,
                &session->page.stylesheet, session->budget,
                session->page.resource_base_url,
                session->page.document_url,
                session->page.referrer_policy,
                session->maximum_font_attempts,
                session->maximum_font_bytes,
                session->maximum_font_file_bytes,
                session->maximum_font_face_backend_bytes,
                session->page.resource_scheduler,
                session->browser_session,
                &session->page.document.content_security_policy,
                &session->page.external_fonts, 0, &face_loaded)) {
            fonts_external_loader_cancel(
                &session->page.external_font_loader,
                session->page.resource_scheduler,
                &session->page.external_fonts);
            session->performance.background_font_failures++;
        }
        if (session->page.external_fonts.failed > failures_before) {
            size_t added_failures =
                session->page.external_fonts.failed - failures_before;
            if (added_failures <= SIZE_MAX
                    - session->performance.background_font_failures) {
                session->performance.background_font_failures +=
                    added_failures;
            } else {
                session->performance.background_font_failures = SIZE_MAX;
            }
        }
        uint64_t elapsed = navigation_record_slice(
            session, NAVIGATION_SLICE_RESOURCE, started, 1);
        session->performance.resource_us += elapsed;
        session->performance.font_resource_us += elapsed;
        if (face_loaded) {
            session->performance.background_fonts_loaded++;
            /* Fallback glyph advances participate in intrinsic sizing and
               line breaking.  Reusing those measurements after a face swap
               can produce a visually mixed layout, so invalidate the bounded
               layout cache before rebuilding against the newly loaded face. */
            layout_reuse_cache_destroy(session->page.layout_reuse);
            session->page.layout_reuse = NULL;
            session->performance.layout_reuse_retained_bytes = 0;
            if (!navigation_relayout(session)) {
                fonts_external_loader_cancel(
                    &session->page.external_font_loader,
                    session->page.resource_scheduler,
                    &session->page.external_fonts);
                session->performance.background_font_failures++;
                /* The fast relayout path constructs a replacement before
                   retiring the current display list. Preserve that valid
                   committed page and leave image continuation observable;
                   the caller may report this failed font slice and pump the
                   remaining background work on the next tick. */
                return false;
            }
            session->performance.background_font_relayouts++;
        }
    }
    if (!session->page.image_continuation_pending) return true;
    if (session->page.deferred_image_targets != NULL) {
        return navigation_run_deferred_image(session);
    }
    ImageResources *images = &session->page.images;
    size_t loaded_before = images->stats.loaded;
    session->performance.background_image_batches++;
    navigation_ensure_layout_reuse(session);
    bool loaded = images_load_external_reusing_layout_styles(
        &session->page.document, &session->page.stylesheet, images,
        session->budget, session->page.resource_base_url,
        session->page.document_url, session->page.referrer_policy,
        session->maximum_images, session->maximum_image_bytes,
        session->maximum_image_file_bytes,
        session->maximum_decoded_image_bytes,
        session->resource_timeout_ms, session->page.resource_scheduler,
        session->browser_session, session->page.layout_reuse,
        session->fonts, session->viewport.css_width);
    if (!loaded) {
        session->page.image_continuation_pending = false;
        session->performance.background_image_failures++;
        /* The loader transactionally removes only the optional suffix it
           appended during this pass.  Priority resources referenced by the
           committed layout remain owned, so pressure cannot blank or close
           an already usable page. */
        session->images = images;
        layout_reuse_cache_update_images(session->page.layout_reuse, images);
        return true;
    }
    session->images = images;
    layout_reuse_cache_update_images(session->page.layout_reuse, images);
    size_t added = images->stats.loaded >= loaded_before
        ? images->stats.loaded - loaded_before : 0;
    if (added <= SIZE_MAX
                 - session->performance.background_images_loaded) {
        session->performance.background_images_loaded += added;
    } else {
        session->performance.background_images_loaded = SIZE_MAX;
    }
    if (added != 0) {
        if (!navigation_relayout(session)) {
            session->page.image_continuation_pending = false;
            session->performance.background_image_failures++;
            session->page.loaded = false;
            return false;
        }
        session->performance.background_image_relayouts++;
    }
    /* The complete authoritative traversal ran once from the currently
       committed DOM. Retrying it in small stateless attempt batches would
       change semantics because corrupt early resources are intentionally not
       retained in the successful-image table and could consume quota again. */
    session->page.image_continuation_pending = false;
    return true;
}

static void navigation_accumulate_script_metrics(
    NavigationSession *session, const ExternalScriptMetrics *metrics)
{
    session->script_discovered += metrics->discovered;
    session->script_attempted += metrics->attempted;
    session->script_loaded += metrics->loaded;
    session->script_failed += metrics->failed;
    session->script_skipped_cross_origin += metrics->skipped_cross_origin;
    session->script_skipped_module += metrics->skipped_module;
    session->script_skipped_nomodule += metrics->skipped_nomodule;
    session->script_skipped_quota += metrics->skipped_quota;
    session->script_skipped_pressure += metrics->skipped_pressure;
    session->script_pressure_collections += metrics->pressure_collections;
    session->script_pressure_reclaimed_bytes +=
        metrics->pressure_reclaimed_bytes;
    session->script_pressure_capped_requests +=
        metrics->pressure_capped_requests;
    session->script_bytes += metrics->bytes;
    session->script_cache_hits += metrics->cache_hits;
    session->script_parser_blocking += metrics->parser_blocking;
    session->script_deferred += metrics->deferred;
    session->script_asynchronous += metrics->asynchronous;
    session->script_modules += metrics->modules;
    session->script_module_map_hits += metrics->module_map_hits;
    session->script_inline_data_fast_paths +=
        metrics->inline_data_fast_paths;
    session->script_inline_data_fast_path_bytes +=
        metrics->inline_data_fast_path_bytes;
    session->script_inline_data_quota_exemptions +=
        metrics->inline_data_quota_exemptions;
    session->script_cost_class_rejections += metrics->cost_class_rejections;
    session->script_watchdog_classification_misses +=
        metrics->watchdog_classification_misses;
    session->script_watchdog_classification_miss_bytes +=
        metrics->watchdog_classification_miss_bytes;
    session->script_watchdog_classification_miss_loops +=
        metrics->watchdog_classification_miss_loops;
    session->script_watchdog_classification_miss_flags |=
        metrics->watchdog_classification_miss_flags;
    navigation_record_soft_script_error(session, metrics);
}

typedef struct {
    NavigationSession *session;
    PocDocument *document;
    const NavigationEntry *entry;
    StreamingScriptState *state;
    NavigationScriptFrontierCallback frontier;
    void *frontier_opaque;
    size_t script_index;
    size_t maximum_scripts;
    size_t maximum_bytes;
    long *script_handles;
    size_t script_count;
    size_t script_capacity;
    bool failed;
} NavigationFrontierScriptWalk;

static bool navigation_frontier_script_snapshot(
    lxb_dom_node_t *node, NavigationFrontierScriptWalk *walk)
{
    if (walk == NULL) return false;
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) {
            snprintf(walk->session->last_error,
                     sizeof(walk->session->last_error),
                     "script snapshot traversal exceeded %u nodes",
                     NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT);
            return false;
        }
        if (node_name_is(node, "script")) {
            if (walk->script_count == NAVIGATION_SCRIPT_SNAPSHOT_LIMIT) {
                snprintf(walk->session->last_error,
                         sizeof(walk->session->last_error),
                         "script snapshot exceeded %u elements",
                         NAVIGATION_SCRIPT_SNAPSHOT_LIMIT);
                return false;
            }
            if (walk->script_count == walk->script_capacity) {
                size_t capacity = walk->script_capacity == 0
                    ? 16u : walk->script_capacity * 2u;
                if (capacity > NAVIGATION_SCRIPT_SNAPSHOT_LIMIT) {
                    capacity = NAVIGATION_SCRIPT_SNAPSHOT_LIMIT;
                }
                if (capacity <= walk->script_capacity
                    || capacity > SIZE_MAX / sizeof(*walk->script_handles)) {
                    snprintf(walk->session->last_error,
                             sizeof(walk->session->last_error),
                             "script snapshot capacity overflow");
                    return false;
                }
                long *script_handles = budget_realloc(
                    walk->session->budget, walk->script_handles,
                    capacity * sizeof(*walk->script_handles));
                if (script_handles == NULL) {
                    snprintf(walk->session->last_error,
                             sizeof(walk->session->last_error),
                             "script snapshot allocation failed at %zu "
                             "elements", walk->script_count);
                    return false;
                }
                walk->script_handles = script_handles;
                walk->script_capacity = capacity;
            }
            long handle = script_runtime_node_weak_handle(
                walk->session->page.runtime, node);
            if (handle == 0) {
                snprintf(walk->session->last_error,
                         sizeof(walk->session->last_error),
                         "script snapshot handle capacity exhausted at %zu "
                         "elements", walk->script_count);
                return false;
            }
            walk->script_handles[walk->script_count++] = handle;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) {
            node = node->parent;
        }
        if (node == NULL || node == boundary_parent) break;
        node = node->next;
    }
    return true;
}

static bool navigation_frontier_script_is_connected(
    const NavigationFrontierScriptWalk *walk, const lxb_dom_node_t *script)
{
    if (walk == NULL || walk->document == NULL || script == NULL) return false;
    size_t depth = 0;
    for (const lxb_dom_node_t *at = script; at != NULL; at = at->parent) {
        /* Lexbor may place the document element below an internal parser
           wrapper, so `document.html` is not a reliable ancestry sentinel.
           DOM connectivity is defined by reaching the owning Document. */
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
        if (++depth > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
    }
    return false;
}

static void navigation_frontier_script_execute(
    NavigationFrontierScriptWalk *walk)
{
    if (walk == NULL) return;
    for (size_t i = 0; i < walk->script_count && !walk->failed; i++) {
        lxb_dom_node_t *script = script_runtime_node_handle_resolve(
            walk->session->page.runtime, walk->script_handles[i]);
        /* An earlier author script may have removed and destroyed this
           candidate.  The generation-tagged handle can be checked without
           dereferencing the stale pointer retained by the original parse
           snapshot. */
        bool connected =
            navigation_frontier_script_is_connected(walk, script);
        if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
            fprintf(stderr,
                    "frontier-script index=%zu handle=%ld resolved=%s "
                    "connected=%s\n",
                    i, walk->script_handles[i],
                    script == NULL ? "no" : "yes",
                    connected ? "yes" : "no");
        }
        if (!connected) continue;
        if (walk->frontier != NULL) {
            walk->frontier(walk->frontier_opaque,
                           walk->script_index, true);
        }
        if (!navigation_document_resource_base(
                walk->document, walk->entry->url,
                walk->session->page.resource_base_url)) {
            walk->failed = true;
            break;
        }
        script_runtime_invalidate_document_base(
            walk->session->page.runtime);
        if (!document_scripts_process_closed(
                walk->session->page.runtime, walk->session->budget,
                walk->session->browser_session,
                walk->session->page.resource_base_url,
                walk->entry->url, walk->session->page.referrer_policy,
                &walk->session->page.document.content_security_policy,
                walk->maximum_scripts, walk->maximum_bytes,
                walk->session->maximum_script_file_bytes,
                walk->session->script_timeout_ms,
                walk->session->page.resource_scheduler,
                script, walk->state)) {
            walk->failed = true;
            break;
        }
        walk->script_index++;
    }
}

bool navigation_execute_document_scripts_streaming_from(
    NavigationSession *session, PocDocument *document,
    NavigationScriptFrontierCallback frontier, void *frontier_opaque)
{
    if (session == NULL || document == NULL
        || !session->document_scripts_enabled) return true;
    const NavigationEntry *entry = navigation_current(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL || entry == NULL) {
        return false;
    }
    size_t remaining_scripts = session->script_discovered
        < session->maximum_scripts
        ? session->maximum_scripts - session->script_discovered : 0;
    size_t remaining_bytes = session->script_bytes
        < session->maximum_script_bytes
        ? session->maximum_script_bytes - session->script_bytes : 0;
    StreamingScriptState state = {0};
    NavigationFrontierScriptWalk walk = {
        .session = session,
        .document = document,
        .entry = entry,
        .state = &state,
        .frontier = frontier,
        .frontier_opaque = frontier_opaque,
        .maximum_scripts = remaining_scripts,
        .maximum_bytes = remaining_bytes
    };
    uint64_t started = navigation_now_us();
    bool snapshot_ok = navigation_frontier_script_snapshot(
        lxb_dom_interface_node(document->html), &walk);
    if (snapshot_ok) navigation_frontier_script_execute(&walk);
    if (frontier != NULL) frontier(frontier_opaque, walk.script_index, false);
    budget_free(session->budget, walk.script_handles);
    walk.script_handles = NULL;
    ExternalScriptMetrics metrics = {0};
    if (!navigation_document_resource_base(
            document, entry->url, session->page.resource_base_url)) {
        return false;
    }
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = snapshot_ok && !walk.failed && document_scripts_finish_streaming(
        document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        entry->url,
        session->page.referrer_policy, remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &state, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (!ok) {
        if (snapshot_ok) {
            snprintf(session->last_error, sizeof(session->last_error), "%s",
                     session->page.script_result.error[0] == '\0'
                       ? "frontier document script pipeline failed"
                       : session->page.script_result.error);
        }
        return false;
    }
    navigation_accumulate_script_metrics(session, &metrics);
    bool static_shell_changed = false;
    if (metrics.failed != 0 || metrics.skipped_quota != 0
        || metrics.skipped_pressure != 0
        || metrics.cost_class_rejections != 0) {
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
        static_shell_changed = document_install_static_shell_fallback(
            &session->page.document);
    }
    bool runtime_relayout = script_runtime_consume_relayout(
        session->page.runtime);
    bool relayout = static_shell_changed || runtime_relayout;
    return (!relayout || navigation_relayout(session))
        && navigation_load_frames(session, entry->url);
}

bool navigation_execute_document_scripts_from(
    NavigationSession *session, PocDocument *document)
{
    if (session == NULL || document == NULL
        || !session->document_scripts_enabled) return true;
    const NavigationEntry *entry = navigation_current(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL || entry == NULL) {
        return false;
    }
    size_t remaining_scripts = session->script_discovered
        < session->maximum_scripts
        ? session->maximum_scripts - session->script_discovered : 0;
    size_t remaining_bytes = session->script_bytes
        < session->maximum_script_bytes
        ? session->maximum_script_bytes - session->script_bytes : 0;
    ExternalScriptMetrics metrics = {0};
    uint64_t started = navigation_now_us();
    if (!navigation_document_resource_base(
            document, entry->url, session->page.resource_base_url)) {
        return false;
    }
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = document_scripts_execute(
        document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        entry->url, session->page.referrer_policy,
        remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (!ok) {
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 session->page.script_result.error[0] == '\0'
                   ? "deferred document script pipeline failed"
                   : session->page.script_result.error);
        return false;
    }
    navigation_accumulate_script_metrics(session, &metrics);
    bool static_shell_changed = false;
    if (metrics.failed != 0 || metrics.skipped_quota != 0
        || metrics.skipped_pressure != 0
        || metrics.cost_class_rejections != 0) {
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
        static_shell_changed = document_install_static_shell_fallback(
            &session->page.document);
    }
    bool runtime_relayout = script_runtime_consume_relayout(
        session->page.runtime);
    bool relayout = static_shell_changed || runtime_relayout;
    if ((relayout && !navigation_relayout(session))
        || !navigation_load_frames(session, entry->url)) return false;
    return true;
}

bool navigation_execute_current_body_scripts(NavigationSession *session)
{
    if (session == NULL || !session->document_scripts_enabled) return true;
    const NavigationEntry *entry = navigation_current(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL || entry == NULL) {
        return false;
    }
    size_t remaining_scripts = session->script_discovered
        < session->maximum_scripts
        ? session->maximum_scripts - session->script_discovered : 0;
    size_t remaining_bytes = session->script_bytes
        < session->maximum_script_bytes
        ? session->maximum_script_bytes - session->script_bytes : 0;
    ExternalScriptMetrics metrics = {0};
    uint64_t started = navigation_now_us();
    if (!navigation_document_resource_base(
            &session->page.document, entry->url,
            session->page.resource_base_url)) return false;
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = document_body_scripts_execute(
        &session->page.document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        entry->url, session->page.referrer_policy,
        remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (!ok) {
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 session->page.script_result.error[0] == '\0'
                   ? "deferred section script pipeline failed"
                   : session->page.script_result.error);
        return false;
    }
    navigation_accumulate_script_metrics(session, &metrics);
    bool relayout = script_runtime_consume_relayout(session->page.runtime);
    return !relayout || navigation_relayout(session);
}

bool navigation_advance_runtime(NavigationSession *session,
                                unsigned elapsed_ms,
                                size_t callback_budget)
{
    if (session == NULL || !session->page.loaded) return false;
    /* A readable page whose bounded author realm was retired has no script
       work to advance. Treat that as a successful idle tick so the browser
       shell, background resources, scrolling, and navigation remain live. */
    if (session->page.runtime == NULL) return true;
    uint64_t runtime_started = navigation_now_us();
    if (!script_runtime_advance(session->page.runtime, elapsed_ms,
                                callback_budget,
                                &session->page.script_result)) {
        capture_cached_script_error(session,
                                    session->page.script_result.error);
        /*
         * Once the authoritative document is committed, a later author task
         * exhausting its bounded realm must not take the browser shell or
         * readable page down with it. Parser-time failures still fail closed
         * because their DOM may be incomplete; this path is safe to degrade
         * to the already-committed static document.
         */
        if (navigation_retire_exhausted_script_realms(session)) return true;
        unsigned long generated_line = 0, generated_column = 0;
        bool captured_generated_source = false;
        if (getenv("TILEFINCH_TRACE_EVAL_SOURCE") != NULL
            && stack_generated_location(session->page.script_result.error,
                                        &generated_line,
                                        &generated_column)) {
            char original_error[sizeof(session->page.script_result.error)];
            snprintf(original_error, sizeof(original_error), "%s",
                     session->page.script_result.error);
            char diagnostic[640];
            snprintf(diagnostic, sizeof(diagnostic),
                     "(()=>{const a=globalThis.__tilefinchEvalSources;"
                     "if(!Array.isArray(a)){globalThis.pocSummary="
                     "'eval-sources-unavailable';return;}for(let i=a.length-1;"
                     "i>=0;i--){const s=a[i];let at=0,ok=true;"
                     "for(let line=1;line<%lu;line++){const next=s.indexOf('\\n',at);"
                     "if(next<0){ok=false;break;}at=next+1;}if(!ok)continue;"
                     "const end=s.indexOf('\\n',at),limit=end<0?s.length:end;"
                     "if(%lu-1>limit-at)continue;at+=%lu-1;"
                     "globalThis.pocSummary=s.slice(Math.max(0,at-900),"
                     "Math.min(s.length,at+900));return;}"
                     "globalThis.pocSummary='eval-source-no-match count='+a.length+"
                     "' lengths='+a.map(s=>s.length).join(',');})()",
                     generated_line, generated_column, generated_column);
            if (script_runtime_evaluate_diagnostic(
                    session->page.runtime, diagnostic,
                    "<generated-eval-context>",
                    &session->page.script_result)) {
                snprintf(session->last_script_error_context,
                         sizeof(session->last_script_error_context),
                         "generated-eval %lu:%lu: %s", generated_line,
                         generated_column,
                         session->page.script_result.summary);
                captured_generated_source = true;
            }
            snprintf(session->page.script_result.error,
                     sizeof(session->page.script_result.error), "%s",
                     original_error);
            session->page.script_result.success = false;
        }
        char function_name[64] = {0};
        if (!captured_generated_source
            && stack_generated_function_name(session->page.script_result.error,
                                          function_name,
                                          sizeof(function_name))) {
            char original_error[sizeof(session->page.script_result.error)];
            snprintf(original_error, sizeof(original_error), "%s",
                     session->page.script_result.error);
            char diagnostic[320];
            snprintf(diagnostic, sizeof(diagnostic),
                     "try{globalThis.pocSummary=String("
                     "__tilefinchDiagnosticLookup('%s'))}catch(error){"
                     "globalThis.pocSummary='lookup-error:'+String(error)}",
                     function_name);
            if (script_runtime_evaluate_diagnostic(
                    session->page.runtime, diagnostic,
                    "<generated-function-source>",
                    &session->page.script_result)) {
                snprintf(session->last_script_error_context,
                         sizeof(session->last_script_error_context),
                         "generated-function %s: %s", function_name,
                         session->page.script_result.summary);
            }
            snprintf(session->page.script_result.error,
                     sizeof(session->page.script_result.error), "%s",
                     original_error);
            session->page.script_result.success = false;
        }
        page_capability_trace_report(session);
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 session->page.script_result.error[0] == '\0'
                   ? "JavaScript scheduler tick failed"
                   : session->page.script_result.error);
        return false;
    }
    const NavigationEntry *current_entry = navigation_current(session);
    if (session->document_scripts_enabled && current_entry != NULL
        && !navigation_load_frames(session, current_entry->url)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "dynamic frame loading failed: %.440s",
                 session->page.script_result.error);
        return false;
    }
    if (!navigation_progress_frames(session)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "dynamic frame completion failed: %.430s",
                 session->page.script_result.error);
        return false;
    }
    if (session->trace_frame_capabilities
        && session->page.frame_count != 0) {
        (void) navigation_collect_frame_capability_trace(session);
    }
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (!frame->loaded || frame->runtime == NULL) continue;
        if (!script_runtime_advance(frame->runtime, elapsed_ms,
                                    callback_budget,
                                    &frame->script_result)) {
            collect_stack_source_context(frame, frame->script_result.error);
            if (session->trace_frame_capabilities) {
                char name[64] = {0};
                if (stack_function_name(frame->script_result.error, name,
                                        sizeof(name))) {
                    char diagnostic[1024];
                    snprintf(diagnostic, sizeof(diagnostic),
                        "try{const s=String(__tilefinchDiagnosticLookup('%s')),"
                        "o=[...s.matchAll(/[A-Za-z0-9_$]+={(.*?)}[,;]/g)],"
                        "m=s.match(/},[A-Za-z0-9_$]+=([A-Za-z0-9_$]+)[,;]/),"
                        "d=__tilefinchDiagnosticLookup(m[1]);"
                        "decoded=o.map((x,i)=>i+':'+[...x[1].matchAll(/([A-Za-z0-9_$]+):([0-9.e]+)/g)].map(v=>v[1]+'='+d(Number(v[2]))).join(',')).join('|'),"
                        "missing=globalThis.__tilefinchCapabilityTrace?[...__tilefinchCapabilityTrace.missing].sort().slice(0,64):[];"
                        "globalThis.pocSummary=decoded+'|missing='+missing.join(',')}catch(e){globalThis.pocSummary='decode-error:'+String(e)}",
                        name);
                    (void) script_runtime_evaluate_diagnostic(
                        frame->runtime, diagnostic, "<frame-error-decode>",
                        &frame->script_result);
                }
            }
            snprintf(session->last_error, sizeof(session->last_error),
                     "frame scheduler failed: %.470s",
                     frame->script_result.error);
            return false;
        }
        if (getenv("TILEFINCH_TRACE_CALLBACK_SOURCE") != NULL
            && frame->callback_error_source_context[0] == '\0'
            && frame->script_result.last_uncaught_callback_error[0] != '\0') {
            collect_stack_source_context_into(
                frame, frame->script_result.last_uncaught_callback_error,
                frame->callback_error_source_context,
                sizeof(frame->callback_error_source_context));
        }
    }
    if (!navigation_deliver_frame_messages(session)) {
        if (navigation_retire_exhausted_script_realms(session)) {
            session->performance.frame_message_soft_failures++;
            return true;
        }
        snprintf(session->last_error, sizeof(session->last_error),
                 "cross-frame message delivery failed: %.450s",
                 session->page.script_result.error);
        return false;
    }
    char navigation_url[NAVIGATION_URL_LIMIT];
    bool replace = false;
    if (script_runtime_consume_navigation(session->page.runtime,
                                          navigation_url,
                                          sizeof(navigation_url),
                                          &replace)) {
        if (session->trace_frame_capabilities) {
            (void) navigation_collect_frame_capability_trace(session);
        }
        if (session->trace_page_capabilities) {
            page_capability_trace_report(session);
            snprintf(session->previous_page_trace,
                     sizeof(session->previous_page_trace), "%s",
                     session->last_page_trace);
        }
        const NavigationEntry *current = navigation_current(session);
        if (current != NULL) {
            snprintf(session->pending_navigation_referer,
                     sizeof(session->pending_navigation_referer), "%s",
                     current->url);
            session->pending_navigation_same_origin =
                navigation_same_origin(current->url, navigation_url);
        }
        uint64_t generation = navigation_begin(session);
        session->performance.runtime_us += navigation_record_slice(
            session, NAVIGATION_SLICE_RUNTIME, runtime_started,
            callback_budget);
        return navigation_load_url(session, generation, navigation_url,
                                   4 * 1024 * 1024, 30000,
                                   session->viewport.device_width,
                                   session->fonts, session->images,
                                   !replace);
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    bool ok = !script_runtime_consume_relayout(session->page.runtime)
              || navigation_relayout(session);
    session->performance.runtime_us += navigation_record_slice(
        session, NAVIGATION_SLICE_RUNTIME, runtime_started, callback_budget);
    return ok;
}

bool navigation_dispatch_event(NavigationSession *session,
                               const char *selector,
                               const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded) return false;
    if (!script_runtime_dispatch(session->page.runtime, selector, event_type,
                                 &session->page.script_result)) return false;
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_event(NavigationSession *session,
                                    lxb_dom_node_t *node,
                                    const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_node(session->page.runtime, node, event_type,
                                      &session->page.script_result)) {
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_activation(NavigationSession *session,
                                         lxb_dom_node_t *node)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_activation_node(
            session->page.runtime, node, &session->page.script_result)) {
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_pointer(
    NavigationSession *session, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || (node == NULL && phase != 1u)) return false;
    if (!script_runtime_dispatch_pointer_node(
        session->page.runtime, node, phase, client_x, client_y,
            offset_x, offset_y, buttons, &session->page.script_result)) {
        return false;
    }
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_input_event(NavigationSession *session,
                                          lxb_dom_node_t *node,
                                          const char *event_type,
                                          const char *data,
                                          const char *input_type,
                                          const char *current_value)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_input_node(
            session->page.runtime, node, event_type, data, input_type,
            current_value,
            &session->page.script_result)) return false;
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_dispatch_node_submit_event(NavigationSession *session,
                                           lxb_dom_node_t *form,
                                           lxb_dom_node_t *submitter)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || form == NULL) {
        return false;
    }
    if (!script_runtime_dispatch_submit_node(
            session->page.runtime, form, submitter,
            &session->page.script_result)) return false;
    return !script_runtime_consume_relayout(session->page.runtime)
           || navigation_relayout(session);
}

bool navigation_evaluate_external_script(NavigationSession *session,
                                         lxb_dom_node_t *script_node,
                                         const char *source,
                                         size_t source_length,
                                         const char *source_url)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || script_node == NULL) return false;
    size_t integrity_length = 0;
    const char *integrity = document_attribute(
        script_node, "integrity", &integrity_length);
    TilefinchIntegrityResult integrity_result =
        tilefinch_resource_integrity_verify(
            integrity, integrity_length, (const uint8_t *) source,
            source_length);
    if (integrity_result == TILEFINCH_INTEGRITY_MISMATCH
        || integrity_result == TILEFINCH_INTEGRITY_INVALID) {
        (void) navigation_dispatch_node_event(session, script_node, "error");
        return false;
    }
    bool evaluated = script_runtime_evaluate_external(
        session->page.runtime, script_node, source, source_length,
        source_url, &session->page.script_result);
    bool relayout_ok = !script_runtime_consume_relayout(session->page.runtime)
                       || navigation_relayout(session);
    return evaluated && relayout_ok;
}

void navigation_destroy(NavigationSession *session)
{
    if (session == NULL) return;
    page_destroy(session);
    navigation_compiled_stylesheet_cache_discard(session);
    if (session->budget != NULL) {
        for (size_t i = 0; i < session->history_count; i++) {
            entry_destroy(session->budget, &session->history[i]);
        }
        budget_free(session->budget, session->history);
        budget_free(session->budget, session->user_css);
    }
    memset(session, 0, sizeof(*session));
}
