#if !defined(__PSP__)
void navigation_test_refuse_next_static_fallback_layout(void)
{
    tilefinch_test_faults()->refuse_next_static_fallback_layout = true;
}

static bool navigation_test_consume_static_fallback_layout_refusal(void)
{
    bool pending = tilefinch_test_faults()->refuse_next_static_fallback_layout;
    tilefinch_test_faults()->refuse_next_static_fallback_layout = false;
    return pending;
}

void navigation_test_refuse_next_same_document_relayout(void)
{
    tilefinch_test_faults()->refuse_next_same_document_relayout = true;
}

void navigation_test_refuse_next_relayout(void)
{
    tilefinch_test_faults()->refuse_next_same_document_relayout = true;
}

static bool navigation_test_consume_same_document_relayout_refusal(void)
{
    bool pending = tilefinch_test_faults()->refuse_next_same_document_relayout;
    tilefinch_test_faults()->refuse_next_same_document_relayout = false;
    return pending;
}
#endif

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

static bool navigation_urls_share_document(const char *left,
                                            const char *right)
{
    char current_url[NAVIGATION_URL_LIMIT];
    char target_url[NAVIGATION_URL_LIMIT];
    if (left == NULL || right == NULL
        || !tilefinch_url_normalize(
               left, current_url, sizeof(current_url))
        || !tilefinch_url_normalize(right, target_url, sizeof(target_url))) {
        return false;
    }
    char *current_fragment = strchr(current_url, '#');
    char *target_fragment = strchr(target_url, '#');
    if (current_fragment != NULL) *current_fragment = '\0';
    if (target_fragment != NULL) *target_fragment = '\0';
    return strcmp(current_url, target_url) == 0;
}

const char *navigation_active_document_url(
    const NavigationSession *session)
{
    if (session == NULL) return NULL;
    const NavigationEntry *current = navigation_current(session);
    if (!session->page.loaded || session->page.document_url[0] == '\0')
        return current == NULL ? NULL : current->url;
    /* Same-document history entries are the active URL, including their
       fragment. A cross-document redirect keeps the requested entry URL for
       traversal, but the committed effective URL is the principal. */
    if (current != NULL
        && navigation_urls_share_document(
               current->url, session->page.document_url)) {
        return current->url;
    }
    return session->page.document_url;
}

static bool navigation_urls_are_distinct_same_request(const char *left,
                                                       const char *right)
{
    char current_url[NAVIGATION_URL_LIMIT];
    char target_url[NAVIGATION_URL_LIMIT];
    return left != NULL && right != NULL
        && tilefinch_url_normalize(left, current_url, sizeof(current_url))
        && tilefinch_url_normalize(right, target_url, sizeof(target_url))
        && strcmp(current_url, target_url) != 0
        && navigation_urls_share_document(current_url, target_url);
}

bool navigation_url_is_same_document(
    const NavigationSession *session, const char *url)
{
    const char *current_url = navigation_active_document_url(session);
    return current_url != NULL && url != NULL
        && navigation_urls_are_distinct_same_request(current_url, url);
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
        || strlen(url) >= NAVIGATION_URL_LIMIT
        || !navigation_url_is_same_document(session, url)) return false;
    char old_url[NAVIGATION_URL_LIMIT];
    const char *active_url = navigation_active_document_url(session);
    if (active_url == NULL) return false;
    snprintf(old_url, sizeof(old_url), "%s", active_url);
    char observed_old_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, old_url, observed_old_base)) {
        return false;
    }
    char next_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, url, next_base)) return false;
    /* Allocate and publish native history first. A broken page realm cannot
       turn a fragment link into a network request or dead control; URL
       reflection/hashchange is advisory after the native commit. */
    if (!history_push(session, url, session->page.document.title)) return false;
    if (session->page.runtime != NULL) {
        (void) script_runtime_commit_same_document(
            session->page.runtime, url, old_url,
            &session->page.script_result);
    }
    /* hashchange is allowed to mutate the first connected <base>.  Publish
       the resource base from the post-callback DOM, not the pre-dispatch
       snapshot, so later relative links/forms agree with document.baseURI.
       Native history is already committed here; a rare bounded
       recalculation refusal therefore keeps the last valid preflight value
       rather than reporting a transactional failure after publication. */
    char post_callback_base[NAVIGATION_URL_LIMIT];
    if (navigation_document_resource_base(
            &session->page.document, url, post_callback_base)) {
        snprintf(next_base, sizeof(next_base), "%s", post_callback_base);
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
        || strcmp(current->url, url) != 0
        || !navigation_urls_are_distinct_same_request(old_url, url)) {
        return false;
    }
    char observed_old_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, old_url, observed_old_base)) {
        return false;
    }
    char next_base[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            &session->page.document, url, next_base)) return false;
    if (session->page.runtime != NULL) {
        (void) script_runtime_restore_same_document(
            session->page.runtime, url, old_url,
            &session->page.script_result);
    }
    /* popstate/hashchange can likewise replace or edit <base>. */
    char post_callback_base[NAVIGATION_URL_LIMIT];
    if (navigation_document_resource_base(
            &session->page.document, url, post_callback_base)) {
        snprintf(next_base, sizeof(next_base), "%s", post_callback_base);
    }
    snprintf(session->page.resource_base_url,
             sizeof(session->page.resource_base_url), "%s", next_base);
    return true;
}

bool navigation_settle_same_document_events(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    ScriptRuntime *runtime = session->page.runtime;
    if (runtime == NULL) return true;

    /* The event callbacks themselves are synchronous, but DOM mutations are
       journaled for a bounded transactional relayout.  Settle that journal
       before BrowserEngine resolves fragment or restored-history geometry.
       A page scroll requested by the callback is applied against the new
       layout, then the user agent's fragment/history scroll may override it. */
    int requested_scroll = 0;
    bool has_requested_scroll = script_runtime_consume_scroll(
        runtime, &requested_scroll);
    bool relayout_requested = script_runtime_consume_relayout(runtime);
    if (relayout_requested) {
        bool relaid_out = navigation_relayout(session);
        if (!relaid_out) return false;
    }
    if (has_requested_scroll) {
        int maximum = navigation_max_scroll(session);
        if (!navigation_set_scroll(
                session, requested_scroll < maximum
                    ? requested_scroll : maximum)) return false;
    }
    return true;
}

static bool navigation_fragment_decode(const TilefinchUrl *parsed,
                                       char *output, size_t capacity,
                                       size_t *length)
{
    if (length != NULL) *length = 0;
    if (parsed == NULL || output == NULL || capacity == 0
        || parsed->fragment_length >= capacity) return false;
    size_t used = 0;
    const char *source = parsed->value + parsed->fragment_offset;
    for (size_t at = 0; at < parsed->fragment_length; at++) {
        unsigned char value = (unsigned char) source[at];
        if (value == '%' && at + 2u < parsed->fragment_length
            && isxdigit((unsigned char) source[at + 1u])
            && isxdigit((unsigned char) source[at + 2u])) {
            unsigned char high = (unsigned char) source[++at];
            unsigned char low = (unsigned char) source[++at];
            high = (unsigned char) (isdigit(high) ? high - '0'
                                      : tolower(high) - 'a' + 10);
            low = (unsigned char) (isdigit(low) ? low - '0'
                                     : tolower(low) - 'a' + 10);
            value = (unsigned char) ((high << 4) | low);
        }
        if (value == 0 || used + 1u >= capacity) return false;
        output[used++] = (char) value;
    }
    output[used] = '\0';
    if (length != NULL) *length = used;
    return true;
}

static lxb_dom_node_t *navigation_fragment_target(
    NavigationSession *session, const char *identifier, size_t length)
{
    if (session == NULL || identifier == NULL || length == 0
        || session->page.document.html == NULL) return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(
        session->page.document.html);
    lxb_dom_node_t *body = document_body_node(&session->page.document);
    size_t reader_kind_length = 0;
    const char *reader_kind = body == NULL ? NULL
        : document_attribute(
              body, "data-tilefinch-reader-kind", &reader_kind_length);
    /* Never discover this root from an author-controlled attribute. The
       BrowserEngine retains the exact native clone through a generation-safe
       handle; an authored direct-child lookalike must not win fragment
       routing while the actual extracted tree is active. */
    lxb_dom_node_t *extracted_root = NULL;
    if (reader_kind != NULL && reader_kind_length != 0u
        && session->page.reader_root_kind != READER_PAGE_RAW) {
        const char *expected = reader_page_kind_name(
            session->page.reader_root_kind);
        size_t expected_length = strlen(expected);
        if (expected_length == reader_kind_length
            && memcmp(expected, reader_kind, expected_length) == 0) {
            if (session->page.runtime != NULL) {
                extracted_root = session->page.reader_root_handle == 0
                    ? NULL : script_runtime_node_handle_resolve_connected(
                          session->page.runtime,
                          session->page.reader_root_handle);
            } else if (session->page.reader_root_handle == 0) {
                extracted_root = session->page.reader_root;
            }
            if (extracted_root != NULL
                && extracted_root->parent != body) extracted_root = NULL;
        }
    }
    char extracted_identifier[NAVIGATION_URL_LIMIT];
    static const char prefix[] = "tilefinch-extracted-";
    size_t prefix_length = sizeof(prefix) - 1u;
    bool try_extracted = extracted_root != NULL
        && length < sizeof(extracted_identifier) - prefix_length;
    if (try_extracted) {
        memcpy(extracted_identifier, prefix, prefix_length);
        memcpy(extracted_identifier + prefix_length, identifier, length);
        extracted_identifier[prefix_length + length] = '\0';
    }
    /* Prefer either extracted anchor form before the hidden raw source tree.
       Basic/Reader keeps authored href fragments stable and prefixes only
       clone targets, including legacy <a name> anchors. */
    unsigned pass_count = try_extracted ? 4u : 2u;
    for (unsigned pass = 0; pass < pass_count; pass++) {
        bool extracted_pass = try_extracted && pass < 2u;
        bool legacy_name = try_extracted ? (pass == 1u || pass == 3u)
                                         : pass == 1u;
        const char *wanted = extracted_pass
            ? extracted_identifier : identifier;
        size_t wanted_length = extracted_pass
            ? prefix_length + length : length;
        size_t visited = 0;
        lxb_dom_node_t *scan_root = extracted_pass
            ? extracted_root : root;
        lxb_dom_node_t *node = scan_root;
        while (node != NULL && visited++ < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) {
            size_t candidate_length = 0;
            const char *candidate = legacy_name && node->type
                        == LXB_DOM_NODE_TYPE_ELEMENT
                    && node->local_name == LXB_TAG_A
                ? document_attribute(node, "name", &candidate_length)
                : (!legacy_name
                    ? document_attribute(node, "id", &candidate_length)
                    : NULL);
            const LayoutNodeBox *box = candidate != NULL
                    && candidate_length == wanted_length
                    && memcmp(candidate, wanted, wanted_length) == 0
                ? layout_box_for_node(&session->page.layout, node) : NULL;
            /* Extracted passes are restricted to the exact native root that
               matches the body's active presentation marker. This provenance
               makes its neutral markers and empty legacy anchors safe
               zero-size targets while preventing a hidden raw-tree duplicate
               (or an authored look-alike elsewhere) from winning. */
            if (box != NULL) return node;
            if (node->first_child != NULL) {
                node = node->first_child;
                continue;
            }
            while (node != NULL && node != scan_root && node->next == NULL)
                node = node->parent;
            node = node == NULL || node == scan_root ? NULL : node->next;
        }
    }
    return NULL;
}

bool navigation_scroll_to_fragment(
    NavigationSession *session, const char *url)
{
    TilefinchUrl parsed;
    if (session == NULL || url == NULL
        || !tilefinch_url_parse(url, &parsed)) return false;
    if (!parsed.has_fragment || parsed.fragment_length == 0)
        return navigation_set_scroll(session, 0);
    char identifier[NAVIGATION_URL_LIMIT];
    size_t length = 0;
    if (!navigation_fragment_decode(
            &parsed, identifier, sizeof(identifier), &length)) return true;
    lxb_dom_node_t *target = navigation_fragment_target(
        session, identifier, length);
    if (target == NULL && length == 3u
        && strncasecmp(identifier, "top", 3u) == 0)
        return navigation_set_scroll(session, 0);
    const LayoutNodeBox *box = target == NULL ? NULL
        : layout_box_for_node(&session->page.layout, target);
    if (box == NULL) return true;
    int maximum = viewport_max_scroll_css(
        &session->viewport, session->page.layout.height);
    int scroll = box->y < 0 ? 0 : (box->y < maximum ? box->y : maximum);
    return navigation_set_scroll(session, scroll);
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
    if (entry != NULL && entry->scroll_y != next
        && session->page.deferred_image_targets != NULL) {
        session->page.image_continuation_pending = true;
        session->page.deferred_image_rank_valid = false;
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

static bool navigation_head_scripts_are_nonvisual(const NavigationPage *page)
{
    if (page == NULL || page->document.html == NULL) return false;
    lxb_dom_node_t *head = lxb_dom_interface_node(
        lxb_html_document_head_element(page->document.html));
    if (head == NULL || head->parent == NULL
        || head->parent->local_name != LXB_TAG_HTML) return false;
    lxb_dom_node_t *previous = head->prev;
    for (unsigned i = 0; previous != NULL && i < 64;
         i++, previous = previous->prev) {
        if (previous->type == LXB_DOM_NODE_TYPE_ELEMENT) return false;
    }
    if (previous != NULL) return false;
    const Stylesheet *sheet = &page->stylesheet;
    /* The sheet caches only lexical dependencies, never live DOM matches.
       Only head/html can observe this hidden subtree from outside it. */
    if (stylesheet_head_scripts_affect_ancestors(sheet, head)) return false;
    ComputedStyle root = style_for_node(sheet, head->parent, NULL);
    ComputedStyle style = style_for_node(sheet, head, &root);
    return style.display == DISPLAY_NONE;
}

static void navigation_retire_subtree(void *opaque,
                                      const lxb_dom_node_t *root)
{
    NavigationSession *session = opaque;
    if (session == NULL || session->page.layout_reuse == NULL) return;
    layout_reuse_cache_retire_subtree(session->page.layout_reuse, root);
}

/* Idempotent: the page runtime reports every freed subtree to the page
   cache from the first journal consumption on; earlier frees stayed
   conservative through the journal's overflow signal. */
static void navigation_bind_node_retirement(NavigationSession *session)
{
    if (session == NULL || session->page.runtime == NULL
        || session->page.layout_reuse == NULL) return;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_NODE_RETIREMENT") != NULL) return;
#endif
    script_runtime_set_node_retirement_callback(
        session->page.runtime, navigation_retire_subtree, session);
    layout_reuse_cache_enable_node_retirement(session->page.layout_reuse);
}

static bool navigation_mutation_is_transient(
    const ScriptMutationJournal *mutations, size_t index)
{
    const ScriptMutationRecord *record = &mutations->records[index];
    if (record->node == NULL) return false; /* retired/unknown, stay conservative */
    if ((record->kind == SCRIPT_MUTATION_CHILD_LIST
         || record->kind == SCRIPT_MUTATION_HEAD_SCRIPT)
        && !record->inserted_from_detached) return false;
    if (record->kind != SCRIPT_MUTATION_CHILD_LIST
        && record->kind != SCRIPT_MUTATION_HEAD_SCRIPT
        && record->kind != SCRIPT_MUTATION_ATTRIBUTE
        && record->kind != SCRIPT_MUTATION_INLINE_STYLE
        && record->kind != SCRIPT_MUTATION_TEXT
        && record->kind != SCRIPT_MUTATION_INNER_HTML) return false;
    const lxb_dom_node_t *ancestors[64];
    size_t depth = 0;
    for (const lxb_dom_node_t *at = record->node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT || depth == 64) return false;
        ancestors[depth++] = at;
    }
    /* Every accepted record is inside a now-detached root whose *earlier*
       first child mutation inserted detached content. Connected-node moves
       and pre-existing removals remain observable changes even if their final
       parent happens to be a transient probe. No allocation or DOM rewrite. */
    for (size_t i = 0; i <= index; i++) {
        const ScriptMutationRecord *first = &mutations->records[i];
        if (!first->inserted_from_detached) continue;
        for (size_t ancestor = 0; ancestor < depth; ancestor++) {
            if (ancestors[ancestor] == first->node) return true;
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
    bool head_checked = false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (navigation_mutation_is_transient(mutations, i)) continue;
        if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT) {
            if (!head_checked
                && !navigation_head_scripts_are_nonvisual(&session->page))
                return false;
            head_checked = true;
            continue;
        }
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
        /* Retained colour writes must not undo ancestor compositing. */
        if (parent->opacity != 255 || parent->has_transform || parent->has_filter)
            return false;
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
        if (candidates[i].change == STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
            || candidates[i].change == STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY) {
            const ComputedStyle *target = candidates[i].focused_now
                ? &candidates[i].focused : &candidates[i].normal;
            if (!layout_apply_focus_border_paint(
                    &session->page.layout, &session->page.stylesheet,
                    candidates[i].node, target,
                    candidates[i].change == STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY, true,
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
            if (candidates[i].change != STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
                && candidates[i].change != STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY) continue;
            const ComputedStyle *target = candidates[i].focused_now
                ? &candidates[i].focused : &candidates[i].normal;
            int left = 0, top = 0, right = 0, bottom = 0;
            if (!layout_apply_focus_border_paint(
                    &session->page.layout, &session->page.stylesheet,
                    candidates[i].node, target,
                    candidates[i].change == STYLE_FOCUS_CHANGE_INSET_PAINT_ONLY, false,
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

static bool navigation_node_connected(const lxb_dom_node_t *node)
{
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
    }
    return false;
}

static lxb_dom_node_t *navigation_style_owner(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_ELEMENT && node_name_is(at, "style"))
            return at;
    }
    return NULL;
}

static bool navigation_link_is_stylesheet(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || !node_name_is(node, "link")) return false;
    size_t length = 0;
    const char *rel = document_attribute(node, "rel", &length);
    if (rel == NULL) return false;
    size_t at = 0;
    while (at < length) {
        while (at < length && isspace((unsigned char) rel[at])) at++;
        size_t begin = at;
        while (at < length && !isspace((unsigned char) rel[at])) at++;
        if (at - begin == 10 && strncasecmp(rel + begin, "stylesheet", 10) == 0)
            return true;
    }
    return false;
}

static lxb_dom_node_t *navigation_document_order_next(
    lxb_dom_node_t *node, const lxb_dom_node_t *root)
{
    if (node == NULL) return NULL;
    if (node->first_child != NULL) return node->first_child;
    for (lxb_dom_node_t *at = node; at != NULL && at != root; at = at->parent) {
        if (at->next != NULL) return at->next;
    }
    return NULL;
}

/* An inserted subtree's <style>/<link> stylesheet sources. Returns false
   when the subtree is too large to inspect or holds a stylesheet source
   that cannot be appended (a <link>, or a <style> the sheet already knows). */
static bool navigation_collect_inserted_styles(
    const Stylesheet *sheet, lxb_dom_node_t *root,
    lxb_dom_node_t **candidates, size_t *count, size_t capacity)
{
    enum { MAXIMUM_SUBTREE_WORK = 256 };
    size_t work = 0;
    for (lxb_dom_node_t *at = root; at != NULL;
         at = navigation_document_order_next(at, root)) {
        if (++work > MAXIMUM_SUBTREE_WORK) return false;
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (navigation_link_is_stylesheet(at)) return false;
        if (!node_name_is(at, "style")) continue;
        if (stylesheet_style_source_known(sheet, at)) return false;
        bool present = false;
        for (size_t i = 0; !present && i < *count; i++) {
            present = candidates[i] == at;
        }
        if (present) continue;
        if (*count == capacity) return false;
        candidates[(*count)++] = at;
    }
    return true;
}

/* A page's late initialization commonly inserts small <style> elements one
   after another. Rebuilding the whole sheet for each costs seconds on the
   device and resets every retained style answer. When the journal proves
   the only stylesheet change is newly inserted <style> elements that follow
   every source the sheet already ingested, append them in place and keep
   the retained matched-rule lists for elements the new rules cannot select. */
static bool navigation_style_append_refused(const char *reason)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL) {
        fprintf(stderr, "style-append refused: %s\n", reason);
    }
#endif
    (void) reason;
    return false;
}

static bool navigation_try_append_inserted_styles(
    NavigationSession *session, const ScriptMutationJournal *mutations)
{
    enum { CANDIDATE_LIMIT = 8, ORDER_WALK_LIMIT = 16384 };
    if (session == NULL || mutations == NULL || mutations->overflowed
        || mutations->count == 0) return navigation_style_append_refused("journal");
    Stylesheet *sheet = &session->page.stylesheet;
    if (sheet->style_sources_bounded_out || sheet->count == 0)
        return navigation_style_append_refused("sources unknown");
    lxb_dom_node_t *candidates[CANDIDATE_LIMIT];
    size_t count = 0;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        lxb_dom_node_t *node = record->node;
        if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT
            || record->kind == SCRIPT_MUTATION_CANVAS) continue;
        if (node == NULL) return navigation_style_append_refused("retired node");
        lxb_dom_node_t *owner = navigation_style_owner(node);
        if (owner != NULL) {
            /* Content of an already ingested <style> changed. */
            if (stylesheet_style_source_known(sheet, owner))
                return navigation_style_append_refused("known style changed");
            if (!navigation_node_connected(owner))
                return navigation_style_append_refused("style detached");
            bool present = false;
            for (size_t k = 0; !present && k < count; k++) {
                present = candidates[k] == owner;
            }
            if (!present) {
                if (count == CANDIDATE_LIMIT) return false;
                candidates[count++] = owner;
            }
            continue;
        }
        switch (record->kind) {
        case SCRIPT_MUTATION_CHILD_LIST:
            if (!record->inserted_from_detached
                || !navigation_node_connected(node))
                return navigation_style_append_refused("child list not an insertion");
            if (!navigation_collect_inserted_styles(
                    sheet, node, candidates, &count, CANDIDATE_LIMIT))
                return navigation_style_append_refused("inserted subtree");
            break;
        case SCRIPT_MUTATION_INNER_HTML:
            /* The live replacement cannot tell us whether the destroyed
               subtree contained a stylesheet. Without removal provenance,
               rebuild rather than retain possibly deleted rules. */
            return navigation_style_append_refused("innerHTML replacement");
        case SCRIPT_MUTATION_ATTRIBUTE:
        case SCRIPT_MUTATION_INLINE_STYLE:
        case SCRIPT_MUTATION_TEXT:
            if (node_name_is(node, "link"))
                return navigation_style_append_refused("link mutation");
            break;
        default:
            return navigation_style_append_refused("record kind");
        }
    }
    if (count == 0) return navigation_style_append_refused("no candidates");
    /* Cascade order is document order. One walk positions every candidate
       among the sources the sheet already holds. */
    lxb_dom_node_t *root = lxb_dom_interface_node(session->page.document.html);
    lxb_dom_node_t *ordered[CANDIDATE_LIMIT];
    size_t after[CANDIDATE_LIMIT];
    size_t found = 0;
    size_t seen_sources = 0;
    size_t steps = 0;
    for (lxb_dom_node_t *at = root; at != NULL && found < count
                                      && steps < ORDER_WALK_LIMIT;
         at = navigation_document_order_next(at, root), steps++) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        bool candidate = false;
        for (size_t k = 0; !candidate && k < count; k++) {
            candidate = candidates[k] == at;
        }
        if (candidate) {
            ordered[found] = at;
            after[found] = seen_sources;
            found++;
        } else if (stylesheet_style_source_known(sheet, at)) {
            seen_sources++;
        }
    }
    if (found != count) return navigation_style_append_refused("document order");
    StylesheetAppendResult result;
    bool lists_valid = true;
    size_t appended_total = 0;
    for (size_t k = 0; k < count; k++) {
        StylesheetAppendResult step;
        if (!stylesheet_append_style_elements_tracked(
                sheet, &ordered[k], 1,
                &session->page.document.content_security_policy,
                after[k] + k, &step)) {
            session->performance.mutation_style_append_fallbacks++;
            return false;
        }
        /* A parse-context move changes declaration values, which the note
           below already discards; the exact selector answers survive it. */
        bool step_valid = step.remap != NULL && !step.appended_bounded_out;
        layout_reuse_cache_note_stylesheet_appended(
            session->page.layout_reuse, sheet, step.remap, step.old_count,
            step.appended, step.appended_count, step_valid);
        lists_valid = lists_valid && step_valid;
        appended_total += step.appended_count;
        result = step;
        stylesheet_append_result_release(sheet, &step);
    }
    result.remap = NULL;
    result.appended = NULL; /* released with the last step above */
    result.appended_count = appended_total;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-style-append: elements=%zu rules=%zu/%zu lists=%d "
                 "context-changed=%d",
                 count, result.appended_count, sheet->count,
                 (int) lists_valid, (int) result.context_changed);
        tilefinch_platform_log_message(message);
    }
#endif
    session->page.compiled_stylesheet_cacheable = false;
    if (session->page.runtime != NULL) {
        script_runtime_set_stylesheet(session->page.runtime, sheet);
    }
    uint64_t fingerprint = 0;
    if (navigation_resource_fingerprint(
            session, &session->page.document, &fingerprint)) {
        session->resource_fingerprint = fingerprint;
    }
    session->performance.mutation_style_appends++;
    return true;
}

static void navigation_prepare_layout_reuse(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool full_rebuild)
{
    if (session == NULL || session->page.layout_reuse == NULL) return;
    navigation_bind_node_retirement(session);
    bool retire = layout_reuse_cache_node_retirement(session->page.layout_reuse);
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &before);
    bool destructive = full_rebuild || mutations == NULL
        || mutations->overflowed;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL) {
        fprintf(stderr, "mutation-journal full-rebuild=%d records=%zu "
                "overflowed=%d relational=%d\n", (int) full_rebuild,
                mutations == NULL ? 0 : mutations->count,
                mutations != NULL && mutations->overflowed,
                mutations != NULL && mutations->relational_selector_sensitive);
        for (size_t i = 0; mutations != NULL && i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            size_t name_length = 0;
            const char *name = record->node == NULL ? NULL
                : document_element_name(record->node, &name_length);
            fprintf(stderr, "mutation-record kind=%d node=%.*s attribute=%s "
                    "exact=%d tokens=%u detached=%d\n", (int) record->kind,
                    (int) name_length, name == NULL ? "" : name,
                    record->attribute, (int) record->changed_tokens_exact,
                    (unsigned) record->changed_token_count,
                    (int) record->inserted_from_detached);
        }
    }
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (mutations != NULL && mutations->count != 0) {
        char message[240];
        size_t used = (size_t) snprintf(message, sizeof(message),
            "tilefinch-mutation-journal: full=%d records=%zu overflow=%d",
            (int) full_rebuild, mutations->count, (int) mutations->overflowed);
        for (size_t i = 0; i < mutations->count && used < sizeof(message) - 40u; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            size_t name_length = 0;
            const char *name = record->node == NULL ? NULL
                : document_element_name(record->node, &name_length);
            used += (size_t) snprintf(message + used, sizeof(message) - used,
                " %d:%.*s:%s:%d/%u", (int) record->kind, (int) (name_length > 8 ? 8 : name_length),
                name == NULL ? "" : name, record->attribute,
                (int) record->changed_tokens_exact, (unsigned) record->changed_token_count);
        }
        tilefinch_platform_log_message(message);
    }
#endif
    if (!destructive) {
        for (size_t i = 0; i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            ScriptMutationKind kind = record->kind;
            /* A subtree inserted from a detached tree removed nothing: the
               retained answers of existing elements can only change through
               structural selectors, which the scoped path bounds to the
               parent. Head scripts render nothing. Moves, removals and
               innerHTML replacements may have freed elements the lists still
               name, so they stay destructive. */
            if (kind == SCRIPT_MUTATION_CHILD_LIST
                && record->inserted_from_detached && record->node != NULL
                && navigation_node_connected(record->node)
#ifndef TILEFINCH_NO_TRACE
                && getenv("TILEFINCH_DISABLE_INSERT_SCOPED_REUSE") == NULL
#endif
                ) continue;
            if (kind == SCRIPT_MUTATION_INNER_HTML
                || kind == SCRIPT_MUTATION_CHILD_LIST
                || kind == SCRIPT_MUTATION_UNKNOWN) {
                /* With node retirement every freed subtree already left the
                   cache, so these reduce to structural changes under their
                   recorded scope. */
                if (retire) continue;
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
            if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT
                || record->kind == SCRIPT_MUTATION_CANVAS) continue;
            /* The parent a removed or moved child left: its remaining
               children's structural answers changed. */
            if (record->scope != NULL) {
                layout_reuse_cache_invalidate_structure(
                    session->page.layout_reuse, record->scope);
            }
            if (record->node == NULL) continue;
            /* <style> content is not rendered; an appended sheet has already
               told the cache which lists the new rules can change. */
            if (navigation_style_owner(record->node) != NULL) continue;
            if (record->kind == SCRIPT_MUTATION_CHILD_LIST
                || record->kind == SCRIPT_MUTATION_INNER_HTML
                || record->kind == SCRIPT_MUTATION_UNKNOWN) {
                if (retire) {
                    layout_reuse_cache_invalidate_structure(
                        session->page.layout_reuse, record->node);
                } else {
                    layout_reuse_cache_invalidate_node(
                        session->page.layout_reuse, record->node, true);
                }
                continue;
            }
            if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && strcmp(
                    record->attribute, "data-tilefinch-focus") == 0) {
                layout_reuse_cache_invalidate_focus(
                    session->page.layout_reuse, record->node);
            } else if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                       && record->changed_tokens_exact) {
                layout_reuse_cache_invalidate_attribute(
                    session->page.layout_reuse, record->node,
                    record->changed_tokens, record->changed_token_count,
                    false, mutations->relational_selector_sensitive);
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
        layout_reuse_cache_flush_invalidations(session->page.layout_reuse);
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
    if (after.matched_token_invalidations >= before.matched_token_invalidations) {
        session->performance.layout_reuse_matched_token_invalidations +=
            after.matched_token_invalidations - before.matched_token_invalidations;
    }
    if (after.matched_token_fallbacks >= before.matched_token_fallbacks) {
        session->performance.layout_reuse_matched_token_fallbacks +=
            after.matched_token_fallbacks - before.matched_token_fallbacks;
    }
    if (after.matched_token_dropped >= before.matched_token_dropped) {
        session->performance.layout_reuse_matched_token_dropped +=
            after.matched_token_dropped - before.matched_token_dropped;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (after.matched_token_invalidations != before.matched_token_invalidations
        || after.matched_token_fallbacks != before.matched_token_fallbacks) {
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-layout-invalidate: token=%zu/%zu dropped=%zu "
                 "affected=%zu",
                 after.matched_token_invalidations
                     - before.matched_token_invalidations,
                 after.matched_token_fallbacks - before.matched_token_fallbacks,
                 after.matched_token_dropped - before.matched_token_dropped,
                 after.matched_token_affected_rules
                     - before.matched_token_affected_rules);
        tilefinch_platform_log_message(message);
    }
#endif
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

static bool navigation_collect_image_descendants(
    lxb_dom_node_t *root, lxb_dom_node_t **nodes,
    size_t *count, size_t capacity, size_t *work, bool *cancelled)
{
    if (root == NULL) return true;
    lxb_dom_node_t *node = root;
    /* The bridge's cheap 256-node test means "unknown", not "rebuild all
       resources". Resolve that uncertainty once here with a shared bounded,
       constant-stack walk. In particular a large text section need not lose
       its stylesheet and decoded images merely because its class changed. */
    for (;;) {
        if (*work == NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        ++*work;
        if ((*work & 31u) == 0
            && !tilefinch_platform_cooperate("resource", *work)) {
            *cancelled = true;
            return false;
        }
        if ((node_name_is(node, "svg") || node_name_is(node, "img"))
            && !navigation_image_refresh_add(
                nodes, count, capacity, node)) return false;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) break;
        node = node->next;
    }
    return true;
}

static size_t navigation_mutation_image_refresh_nodes(
    const ScriptMutationJournal *mutations, lxb_dom_node_t **nodes,
    size_t capacity, bool *bounded_out, bool *cancelled)
{
    if (bounded_out != NULL) *bounded_out = false;
    *cancelled = false;
    if (mutations == NULL || nodes == NULL || capacity == 0) return 0;
    size_t count = 0, work = 0;
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
                    || strcmp(record->attribute, "color") == 0
                    || strcmp(record->attribute, "hidden") == 0));
        if (ancestor_style_change
            && !navigation_collect_image_descendants(
                record->node, nodes, &count, capacity, &work, cancelled)) {
            if (bounded_out != NULL) *bounded_out = true;
            return count;
        }
    }
    return count;
}

static bool navigation_mutations_are_canvas_paint_only(
    NavigationSession *session, const ScriptMutationJournal *mutations)
{
    if (session == NULL || mutations == NULL || mutations->count == 0
        || mutations->overflowed) return false;
    int left = 0, top = 0, right = 0, bottom = 0;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_CANVAS
            || strcmp(record->attribute, "paint") != 0
            || record->node == NULL) return false;
        const ImageResource *image = images_find_node(
            &session->page.images, record->node);
        const LayoutNodeBox *box = layout_box_for_node(
            &session->page.layout, record->node);
        if (image == NULL || !image->is_canvas || box == NULL
            || box->width <= 0 || box->height <= 0) return false;
        int box_right = box->x > INT_MAX - box->width
            ? INT_MAX : box->x + box->width;
        int box_bottom = box->y > INT_MAX - box->height
            ? INT_MAX : box->y + box->height;
        if (right <= left || bottom <= top) {
            left = box->x;
            top = box->y;
            right = box_right;
            bottom = box_bottom;
        } else {
            if (box->x < left) left = box->x;
            if (box->y < top) top = box->y;
            if (box_right > right) right = box_right;
            if (box_bottom > bottom) bottom = box_bottom;
        }
    }
    session->relayout_damage_valid = right > left && bottom > top;
    session->relayout_damage_left = left;
    session->relayout_damage_top = top;
    session->relayout_damage_right = right;
    session->relayout_damage_bottom = bottom;
    session->canvas_paint_damage_pending = true;
    session->incremental_relayouts++;
    return true;
}

static bool navigation_relayout_failed(
    NavigationSession *session, bool must_retire_page)
{
    if (must_retire_page) {
        /* Author mutation has already committed and its journal has been
           consumed. The incumbent layout borrows text and node pointers from
           that pre-mutation DOM, so retaining any part of the page graph on a
           replacement refusal would publish dangling native references.
           Retire the page atomically; BrowserEngine retires its shell before
           returning the failure to the frontend. */
        page_destroy(session);
    }
    return false;
}

/* Pending targets the ordinary image loader must leave alone. A target that
   already holds a committed resource is the batch currently being published:
   a rebuild that drops the image table must reload it like every eager image,
   or the publication relayout would present the page without it and the
   prefix accounting would then skip or refetch the batch. */
static size_t navigation_live_deferred_nodes(
    const NavigationPage *page, lxb_dom_node_t **nodes)
{
    size_t count = 0;
    for (size_t at = page->deferred_image_cursor;
         at < page->deferred_image_count && count < NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT; at++) {
        const ImagePriorityTarget *target = &page->deferred_image_targets[at];
        if (target->weak_handle != 0 && (page->runtime == NULL
            || script_runtime_node_handle_resolve_connected(page->runtime,
                target->weak_handle) != target->node)) continue;
        if (images_find_node(&page->images, target->node) != NULL) continue;
        nodes[count++] = target->node;
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
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (session->page.document.node_count > 8192) {
        printf("tilefinch-large-page-relayout: begin nodes=%zu\n",
                       session->page.document.node_count);
    }
#endif
    ScriptMutationJournal mutations = {0};
    bool mutation_driven = session->page.runtime != NULL
        && script_runtime_consume_mutations(session->page.runtime,
                                            &mutations);
    if (mutation_driven) {
        for (size_t i = 0; i < mutations.count; i++) {
            if (mutations.records[i].kind == SCRIPT_MUTATION_CANVAS) {
                /* Canvas snapshots are always page-owned. Ordinary browser
                   loads already use this table; this also makes standalone
                   local fixtures adopt the same path after their first
                   canvas publication. */
                session->images = &session->page.images;
                break;
            }
        }
    }
    if (mutation_driven
        && navigation_mutations_are_canvas_paint_only(
            session, &mutations)) return true;
    session->canvas_paint_damage_pending = false;
    bool conservative_scan = !mutation_driven
        || mutations.conservative_resource_scan;
    bool full_rebuild = session->style_rebuild_required
        || mutations.resource_rebuild_required;
    if (mutation_driven && !session->style_rebuild_required
        && mutations.resource_rebuild_required
#ifndef TILEFINCH_NO_TRACE
        && getenv("TILEFINCH_DISABLE_STYLE_APPEND") == NULL
#endif
        && navigation_try_append_inserted_styles(session, &mutations)) {
        full_rebuild = false;
    }
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
            return navigation_relayout_failed(session, mutation_driven);
        }
        if (!full_rebuild && conservative_scan) {
            full_rebuild = fingerprint != session->resource_fingerprint;
        }
    }
    /* Image discovery below reuses computed styles too. Invalidate the old
       hidden ancestry before that traversal, not only before final layout. */
    if (!full_rebuild)
        navigation_prepare_layout_reuse(session, &mutations, false);
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
            bool refresh_bounded_out = false, refresh_cancelled = false;
            size_t candidate_count =
                navigation_mutation_image_refresh_nodes(
                    &mutations, candidates,
                    sizeof(candidates) / sizeof(candidates[0]),
                    &refresh_bounded_out, &refresh_cancelled);
            if (refresh_cancelled)
                return navigation_relayout_failed(session, true);
            if (refresh_bounded_out) {
                full_rebuild = true;
                images_loaded = false;
            }
            size_t refresh_count = 0;
            for (size_t i = 0;
                 !refresh_bounded_out && i < candidate_count; i++) {
                if (images_find_node(
                        &session->page.images, candidates[i]) == NULL) {
                    bool queued = false;
                    for (size_t at = session->page.deferred_image_cursor;
                         at < session->page.deferred_image_count; at++) {
                        const ImagePriorityTarget *target =
                            &session->page.deferred_image_targets[at];
                        if (target->node == candidates[i]
                            && (target->weak_handle == 0
                                || (session->page.runtime != NULL
                                    && script_runtime_node_handle_resolve_connected(
                                        session->page.runtime, target->weak_handle)
                                           == candidates[i]))) {
                            queued = true;
                            break;
                        }
                    }
                    /* Revealing a queued image is not new resource
                       discovery. Let attach_layout re-rank/re-arm it; do
                       not turn a disclosure input into synchronous fetch. */
                    if (queued) continue;
                    /* Frameworks commonly connect a lazy <img> before
                       assigning src. There is no stale surface to retire;
                       let the deduplicating additive traversal discover it
                       alongside any CSS image in the new subtree. */
                    scan_for_additions = true;
                } else {
                    /* Ancestor visibility/color changes need not discard a
                       decoded photograph. Only a direct image mutation can
                       change its source; inline SVG still needs repainting. */
                    bool direct_image_change = !node_name_is(candidates[i], "img");
                    for (size_t m = 0; !direct_image_change && m < mutations.count; ++m)
                        direct_image_change = mutations.records[m].node == candidates[i];
                    if (direct_image_change) refresh[refresh_count++] = candidates[i];
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
            lxb_dom_node_t *deferred[NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
            size_t deferred_count = navigation_live_deferred_nodes(&session->page, deferred);
            images_loaded = images_load_external_reusing_layout_styles_excluding(
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
                session->fonts, session->viewport.css_width, deferred, deferred_count);
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
        navigation_evict_layout_reuse_under_pressure(session);
        LayoutDocument next = {0};
#if !defined(__PSP__)
        bool injected_refusal =
            navigation_test_consume_same_document_relayout_refusal();
        if (injected_refusal)
            budget_inject_failure_after(session->budget, 0u);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t relayout_prepared = navigation_now_us();
#endif
        bool built = navigation_build_layout(
            session, &next, session->fonts, session->images);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t relayout_built = navigation_now_us();
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        if (session->page.document.node_count > 8192) {
            printf("tilefinch-large-page-relayout: built=%d elapsed=%lluus "
                "flow=%lluus spatial=%lluus commands=%zu\n", built,
                (unsigned long long) (navigation_now_us() - started),
                (unsigned long long) next.performance.flow_us,
                (unsigned long long) next.performance.spatial_index_us,
                next.count);
        }
#endif
#if !defined(__PSP__)
        if (injected_refusal)
            budget_clear_failure_injection(session->budget);
#endif
        if (!built) {
            return navigation_relayout_failed(session, mutation_driven);
        }
        layout_transfer_scroll_state(&session->page.layout, &next);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t relayout_transferred = navigation_now_us();
#endif
        navigation_measure_damage(session, &session->page.layout, &next);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t relayout_damaged = navigation_now_us();
#endif
        layout_destroy(&session->page.layout);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        uint64_t relayout_destroyed = navigation_now_us();
#endif
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
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char relayout_phases[256];
        snprintf(relayout_phases, sizeof(relayout_phases),
            "tilefinch-relayout-phases: prepare=%lluus build=%lluus transfer=%lluus damage=%lluus destroy=%lluus attach=%lluus",
            (unsigned long long) (relayout_prepared - started),
            (unsigned long long) (relayout_built - relayout_prepared),
            (unsigned long long) (relayout_transferred - relayout_built),
            (unsigned long long) (relayout_damaged - relayout_transferred),
            (unsigned long long) (relayout_destroyed - relayout_damaged),
            (unsigned long long) (navigation_now_us() - relayout_destroyed));
        tilefinch_platform_log_message(relayout_phases);
#endif
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
        return navigation_relayout_failed(session, true);
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
            return navigation_relayout_failed(session, true);
        }
        /* The job borrows this image table. Retire it before a full
           resource rebuild replaces the table; the pending targets remain
           weak and will be re-admitted against the rebuilt stylesheet. */
        images_priority_load_destroy(session->page.deferred_image_job);
        session->page.deferred_image_job = NULL;
        session->page.deferred_image_batch_count = 0;
        session->page.deferred_image_publication_age = 0;
        lxb_dom_node_t *deferred[NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
        size_t deferred_count = navigation_live_deferred_nodes(&session->page, deferred);
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
            return navigation_relayout_failed(session, true);
        }
        if (!navigation_dispatch_stylesheet_events(
                session, session->page.resource_base_url)
            && !navigation_retire_exhausted_script_realms(session, false)) {
            session->page.loaded = false;
            return navigation_relayout_failed(session, true);
        }
        navigation_ensure_layout_reuse(session);
        if (!images_load_external_reusing_layout_styles_excluding(
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
                session->fonts, session->viewport.css_width, deferred, deferred_count)) {
            session->page.loaded = false;
            return navigation_relayout_failed(session, true);
        }
        session->images = &session->page.images;
    }
    if (session->user_css_length != 0
        && !stylesheet_add_user_css(&session->page.stylesheet,
                                    session->user_css,
                                    session->user_css_length)) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
    if (session->web_fonts_enabled && rebuild_url != NULL
        && !navigation_load_web_fonts(session, rebuild_url)) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
#if !defined(__PSP__)
    bool injected_refusal =
        navigation_test_consume_same_document_relayout_refusal();
    if (injected_refusal)
        budget_inject_failure_after(session->budget, 0u);
#endif
    bool built = navigation_build_layout(
        session, &session->page.layout, session->fonts, session->images);
#if !defined(__PSP__)
    if (injected_refusal)
        budget_clear_failure_injection(session->budget);
#endif
    if (!built) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
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

/* Script retirement changes only the static-degradation cascade. Rebuild the
   layout against the already-admitted stylesheet/images and adopt it only
   after success; the general relayout path may destructively rebuild those
   resources when it notices an abandoned author mutation. */
static bool navigation_relayout_static_fallback(
    NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    session->focus_outline_cache_valid = false;
    session->focus_outline_cache_node = NULL;
    uint64_t fingerprint = 0;
    bool fingerprint_valid = page_resource_fingerprint(
        &session->page.document, &fingerprint);
    navigation_prepare_layout_reuse(session, NULL, true);
    navigation_evict_layout_reuse_under_pressure(session);
    LayoutDocument next = {0};
    uint64_t started = navigation_now_us();
#if !defined(__PSP__)
    if (navigation_test_consume_static_fallback_layout_refusal()) {
        budget_inject_failure_after(session->budget, 0);
    }
#endif
    if (!navigation_build_layout(
            session, &next, session->fonts, session->images)) {
        if (fingerprint_valid) session->resource_fingerprint = fingerprint;
        return false;
    }
    /* A pending canvas publication belongs to the retained old layout until
       the replacement is ready to adopt. A refused build must leave that
       damage visible to the next frame/retry rather than losing it. */
    session->canvas_paint_damage_pending = false;
    layout_transfer_scroll_state(&session->page.layout, &next);
    navigation_measure_damage(session, &session->page.layout, &next);
    layout_destroy(&session->page.layout);
    session->page.layout = next;
    session->incremental_relayouts++;
    session->performance.fast_relayouts++;
    uint64_t elapsed = navigation_record_layout_slices(
        session, started, &next);
    session->performance.layout_us += elapsed;
    session->performance.relayout_us += elapsed;
    if (fingerprint_valid) session->resource_fingerprint = fingerprint;
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

struct NavigationUserCssTransaction {
    NavigationSession *session;
    char *previous_user_css;
    size_t previous_user_css_length;
    Stylesheet previous_stylesheet;
    StylesheetDocumentResources previous_resources;
    ExternalStylesheetStats previous_external;
    LayoutDocument previous_layout;
    LayoutReuseCache *previous_layout_reuse;
    size_t previous_incremental_relayouts;
    int previous_scroll;
    int previous_damage_left;
    int previous_damage_top;
    int previous_damage_right;
    int previous_damage_bottom;
    bool previous_style_rebuild_required;
    bool previous_damage_valid;
    bool active;
};

static void navigation_user_css_transaction_release(
    NavigationSession *session, NavigationUserCssTransaction *transaction,
    bool destroy_previous)
{
    if (session == NULL || transaction == NULL) return;
    if (destroy_previous) {
        layout_destroy(&transaction->previous_layout);
        stylesheet_destroy(&transaction->previous_stylesheet);
        stylesheet_document_resources_destroy(
            &transaction->previous_resources);
        layout_reuse_cache_destroy(transaction->previous_layout_reuse);
        budget_free(session->budget, transaction->previous_user_css);
    }
    transaction->active = false;
    budget_free(session->budget, transaction);
}

NavigationUserCssTransaction *navigation_apply_user_css_begin(
    NavigationSession *session, const char *css, size_t length)
{
    if (session == NULL || session->budget == NULL || !session->page.loaded
        || (css == NULL && length != 0) || length > 128u * 1024u)
        return NULL;
    NavigationUserCssTransaction *transaction = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_STYLE, 1u, sizeof(*transaction));
    if (transaction == NULL) return NULL;
    transaction->session = session;
    char *copy = NULL;
    if (length != 0) {
        copy = budget_malloc(session->budget, length + 1u);
        if (copy == NULL) {
            navigation_user_css_transaction_release(
                session, transaction, false);
            return NULL;
        }
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
        if (!okay) {
            /* Presentation overrides such as Reader mode must remain
               available when optional author CSS was unavailable during the
               original navigation (or has since fallen out of cache). Match
               commit-time degradation instead of making the toggle depend
               on re-fetching every author sheet. */
            stylesheet_document_resources_destroy(&next_resources);
            memset(&next_resources, 0, sizeof(next_resources));
            memset(&next_external, 0, sizeof(next_external));
            okay = navigation_build_usable_stylesheet_fallback(
                &next_sheet, session->budget, &session->page.document,
                &session->viewport);
            if (okay) session->performance.stylesheet_pressure_fallbacks++;
        }
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
        navigation_user_css_transaction_release(
            session, transaction, false);
        return NULL;
    }

    const NavigationEntry *entry = navigation_current(session);
    int old_scroll = entry == NULL ? 0 : entry->scroll_y;
    int viewport_offset = 0;
    const DrawCommand *anchor = navigation_scroll_text_anchor(
        &session->page.layout, old_scroll, &viewport_offset);
    int anchored_scroll = navigation_scroll_for_text_anchor(
        &next_layout, anchor, viewport_offset, old_scroll);
    int old_height = session->page.layout.height;
    transaction->previous_user_css = session->user_css;
    transaction->previous_user_css_length = session->user_css_length;
    transaction->previous_stylesheet = session->page.stylesheet;
    transaction->previous_resources = session->page.stylesheet_resources;
    transaction->previous_external = session->page.external_stylesheets;
    transaction->previous_layout = session->page.layout;
    transaction->previous_layout_reuse = session->page.layout_reuse;
    transaction->previous_incremental_relayouts =
        session->incremental_relayouts;
    transaction->previous_scroll = old_scroll;
    transaction->previous_style_rebuild_required =
        session->style_rebuild_required;
    transaction->previous_damage_valid = session->relayout_damage_valid;
    transaction->previous_damage_left = session->relayout_damage_left;
    transaction->previous_damage_top = session->relayout_damage_top;
    transaction->previous_damage_right = session->relayout_damage_right;
    transaction->previous_damage_bottom = session->relayout_damage_bottom;
    transaction->active = true;
    session->page.layout_reuse = NULL;
    session->page.layout = next_layout;
    session->page.stylesheet = next_sheet;
    session->page.stylesheet_resources = next_resources;
    session->page.external_stylesheets = next_external;
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
    return transaction;
}

void navigation_apply_user_css_commit(
    NavigationSession *session, NavigationUserCssTransaction *transaction)
{
    if (session == NULL || transaction == NULL || !transaction->active
        || transaction->session != session) return;
    navigation_user_css_transaction_release(session, transaction, true);
}

bool navigation_apply_user_css_rollback(
    NavigationSession *session, NavigationUserCssTransaction *transaction)
{
    if (session == NULL || transaction == NULL || !transaction->active
        || transaction->session != session) return false;
    layout_destroy(&session->page.layout);
    stylesheet_destroy(&session->page.stylesheet);
    stylesheet_document_resources_destroy(
        &session->page.stylesheet_resources);
    layout_reuse_cache_destroy(session->page.layout_reuse);
    budget_free(session->budget, session->user_css);
    session->user_css = transaction->previous_user_css;
    session->user_css_length = transaction->previous_user_css_length;
    session->page.stylesheet = transaction->previous_stylesheet;
    session->page.stylesheet_resources = transaction->previous_resources;
    session->page.external_stylesheets = transaction->previous_external;
    session->page.layout = transaction->previous_layout;
    session->page.layout_reuse = transaction->previous_layout_reuse;
    session->incremental_relayouts =
        transaction->previous_incremental_relayouts;
    session->style_rebuild_required =
        transaction->previous_style_rebuild_required;
    session->relayout_damage_valid = transaction->previous_damage_valid;
    session->relayout_damage_left = transaction->previous_damage_left;
    session->relayout_damage_top = transaction->previous_damage_top;
    session->relayout_damage_right = transaction->previous_damage_right;
    session->relayout_damage_bottom = transaction->previous_damage_bottom;
    if (session->page.runtime != NULL) {
        script_runtime_set_stylesheet(
            session->page.runtime, &session->page.stylesheet);
    }
    navigation_attach_layout(session);
    /* The incumbent layout never changed while it was held by the
       transaction, so its saved scroll position is already valid for that
       exact layout. Restore it after rebinding the runtime and do not clamp
       it against the rejected candidate's geometry. */
    (void) navigation_set_scroll(session, transaction->previous_scroll);
    transaction->previous_user_css = NULL;
    transaction->previous_user_css_length = 0;
    memset(&transaction->previous_stylesheet, 0,
           sizeof(transaction->previous_stylesheet));
    memset(&transaction->previous_resources, 0,
           sizeof(transaction->previous_resources));
    memset(&transaction->previous_external, 0,
           sizeof(transaction->previous_external));
    memset(&transaction->previous_layout, 0,
           sizeof(transaction->previous_layout));
    transaction->previous_layout_reuse = NULL;
    navigation_user_css_transaction_release(session, transaction, false);
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

static void navigation_deferred_images_finish(NavigationSession *session);

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
    navigation_deferred_images_finish(session);
    return cancelled;
}

static void navigation_deferred_images_finish(NavigationSession *session)
{
    if (session == NULL) return;
    images_priority_load_destroy(session->page.deferred_image_job);
    session->page.deferred_image_job = NULL;
    session->page.deferred_image_batch_count = 0;
    session->page.deferred_image_publication_age = 0;
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

static size_t navigation_deferred_image_available_prefix(
    const NavigationPage *page, size_t maximum)
{
    if (page == NULL || page->deferred_image_targets == NULL) return 0;
    size_t available = 0;
    while (available < maximum
           && page->deferred_image_cursor + available
                  < page->deferred_image_count) {
        const ImagePriorityTarget *target =
            &page->deferred_image_targets[
                page->deferred_image_cursor + available];
        if (!image_resource_available(
                images_find_node(&page->images, target->node))) break;
        available++;
    }
    return available;
}

bool navigation_prioritize_deferred_image(
    NavigationSession *session, lxb_dom_node_t *node)
{
    if (session == NULL || node == NULL) return false;
    NavigationPage *page = &session->page;
    size_t found = page->deferred_image_count;
    for (size_t at = page->deferred_image_cursor;
         at < page->deferred_image_count; at++) {
        if (page->deferred_image_targets[at].node == node) {
            found = at;
            break;
        }
    }
    if (found == page->deferred_image_count) return false;
    if (page->deferred_image_rank_valid
        && found >= page->deferred_image_eligible_end) return false;
    if (page->deferred_image_job != NULL
        && images_priority_load_contains_node(
               page->deferred_image_job, node)) {
        return true;
    }
    if (page->deferred_image_job != NULL) {
        /* Focus changes are latency-sensitive. Do not cancel the current
           transport/decode batch merely to prioritize a different row: job
           destruction synchronously cancels and takes its descriptors, rolls
           back optional image state, and throws away useful network progress.
           Preserve that admitted prefix and rotate the focused target to the
           first position after it instead. It will start as soon as the
           bounded one-or-two-image batch retires, while the input path remains
           allocation-free and transport-free. */
        size_t active_count = images_priority_load_target_count(
            page->deferred_image_job);
        if (active_count == 0) active_count = 1u;
        size_t promote_at = page->deferred_image_cursor;
        if (active_count <= page->deferred_image_count - promote_at) {
            promote_at += active_count;
        } else {
            promote_at = page->deferred_image_count;
        }
        if (found < promote_at) return true;
        if (found != promote_at) {
            ImagePriorityTarget focused = page->deferred_image_targets[found];
            size_t shifted = found - promote_at;
            memmove(
                &page->deferred_image_targets[promote_at + 1u],
                &page->deferred_image_targets[promote_at],
                shifted * sizeof(*page->deferred_image_targets));
            page->deferred_image_targets[promote_at] = focused;
        }
        const NavigationEntry *entry = navigation_current(session);
        page->deferred_image_rank_scroll_y = entry == NULL ? 0 : entry->scroll_y;
        return true;
    }
    if (found != page->deferred_image_cursor) {
        /* Focus is a temporary priority, not permission to shuffle every
           bystander. Rotate the focused target to the cursor so the natural
           viewport order of all remaining thumbnails stays stable. A swap
           teleported the old head to the focused row's former position and
           repeated d-pad moves could therefore strand the first card near
           the end of the queue. */
        ImagePriorityTarget focused = page->deferred_image_targets[found];
        size_t shifted = found - page->deferred_image_cursor;
        memmove(
            &page->deferred_image_targets[
                page->deferred_image_cursor + 1u],
            &page->deferred_image_targets[page->deferred_image_cursor],
            shifted * sizeof(*page->deferred_image_targets));
        page->deferred_image_targets[page->deferred_image_cursor] = focused;
    }
    /* Keep the explicit focus order until scrolling changes. Otherwise an
       all-visible result list can immediately swap the old first row back. */
    const NavigationEntry *entry = navigation_current(session);
    page->deferred_image_rank_scroll_y = entry == NULL ? 0 : entry->scroll_y;
    return true;
}

static uint64_t navigation_deferred_image_distance(
    const NavigationSession *session, const ImagePriorityTarget *target,
    int scroll_y, uint16_t *display_width, uint16_t *display_height)
{
    if (display_width != NULL) *display_width = target->display_width;
    if (display_height != NULL) *display_height = target->display_height;
    int64_t top = scroll_y;
    int64_t bottom = top + session->viewport.css_height;
    const LayoutNodeBox *box = layout_box_for_node(&session->page.layout, target->node);
    if (box != NULL && box->width > 0 && box->height > 0) {
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

static bool navigation_deferred_target_live(
    NavigationPage *page, const ImagePriorityTarget *target)
{
    return target->weak_handle == 0
        || (page->runtime != NULL
            && script_runtime_node_handle_resolve_connected(
                   page->runtime, target->weak_handle) == target->node);
}

/* A missing node box is not a visibility test (inline replaced elements
   need not have one). Resolve the bounded ancestor chain using the layout
   style cache instead, and never inspect a retired weak target. */
static bool navigation_deferred_image_visible(
    NavigationSession *session, const ImagePriorityTarget *target)
{
    if (!navigation_deferred_target_live(&session->page, target)) return false;
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *node = target->node; node != NULL; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = node;
    }
    ComputedStyle style = layout_initial_root_style();
    while (count != 0) {
        ComputedStyle next;
        layout_reuse_cache_resolve_style(session->page.layout_reuse,
            &session->page.stylesheet, session->fonts, ancestors[--count],
            &style, &next);
        if (next.display == DISPLAY_NONE || next.hidden) return false;
        style = next;
    }
    return true;
}

/* Reader retains raw resource ownership while presenting a native extracted
   copy. Match only inside the engine-owned root, never a public DOM marker.
   The original source/weak handle remains the fetch authority. */
static lxb_dom_node_t *navigation_deferred_reader_image(
    NavigationPage *page, const ImagePriorityTarget *target)
{
    lxb_dom_node_t *root = page->reader_root;
    if (root == NULL || (page->reader_root_handle != 0
        && (page->runtime == NULL || script_runtime_node_handle_resolve_connected(
            page->runtime, page->reader_root_handle) != root))) return NULL;
    if (!navigation_deferred_target_live(page, target)) return NULL;
    size_t length = 0;
    const char *source = document_attribute(target->node, "src", &length);
    if (source == NULL || length == 0) return NULL;
    size_t visited = 0;
    for (lxb_dom_node_t *node = root; node != NULL && visited++ < 8192u;) {
        if (node_name_is(node, "img")) {
            size_t candidate_length = 0;
            const char *candidate = document_attribute(node, "src", &candidate_length);
            if (candidate != NULL && length == candidate_length
                && memcmp(source, candidate, length) == 0) return node;
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return NULL;
}

static void navigation_rank_deferred_images(NavigationSession *session, int scroll_y)
{
    NavigationPage *page = &session->page;
    uint64_t distances[NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
    size_t start = page->deferred_image_cursor;
    for (size_t i = start; i < page->deferred_image_count; i++) {
        ImagePriorityTarget *target = &page->deferred_image_targets[i];
        ImagePriorityTarget presentation = *target;
        bool visible = navigation_deferred_image_visible(session, target);
        if (!visible) {
            lxb_dom_node_t *reader = navigation_deferred_reader_image(page, target);
            if (reader != NULL) {
                presentation.node = reader;
                presentation.weak_handle = 0;
                visible = navigation_deferred_image_visible(session, &presentation);
            }
        }
        uint64_t distance = UINT64_MAX;
        if (visible) {
            distance = navigation_deferred_image_distance(session, &presentation,
                scroll_y, &target->display_width, &target->display_height);
            /* Unknown inline geometry remains admissible for eager images.
               Explicit lazy images wait for positive viewport evidence. */
            if (navigation_attribute_equals(target->node, "loading", "lazy")) {
                if (distance != 0) distance = UINT64_MAX;
            } else if (distance == UINT64_MAX) distance--;
        }
        distances[i] = distance;
    }
    /* Stable bounded insertion sort: compute geometry/styles once per
       target, not once per comparison or once per downloaded image. */
    for (size_t i = start + 1; i < page->deferred_image_count; i++) {
        ImagePriorityTarget target = page->deferred_image_targets[i];
        uint64_t distance = distances[i];
        size_t at = i;
        while (at > start && distance < distances[at - 1]) {
            page->deferred_image_targets[at] = page->deferred_image_targets[at - 1];
            distances[at] = distances[at - 1];
            at--;
        }
        page->deferred_image_targets[at] = target;
        distances[at] = distance;
    }
    page->deferred_image_rank_scroll_y = scroll_y;
    page->deferred_image_rank_valid = true;
    page->deferred_image_eligible_end = start;
    while (page->deferred_image_eligible_end < page->deferred_image_count
           && distances[page->deferred_image_eligible_end] != UINT64_MAX)
        page->deferred_image_eligible_end++;
    if (distances[start] == UINT64_MAX) page->image_continuation_pending = false;
}

static bool navigation_publish_deferred_images(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->deferred_image_publication_age == 0) return true;
    page->deferred_image_publication_age = 0;
    if (session->deferred_image_ready != NULL)
        session->deferred_image_ready(session->deferred_image_ready_opaque, session);
    if (!navigation_relayout(session)) {
        session->performance.background_image_failures++;
        navigation_deferred_images_finish(session);
        return false;
    }
    session->performance.background_image_relayouts++;
    return true;
}

static bool navigation_run_deferred_image(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->deferred_image_targets == NULL
        || page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
        return true;
    }
    if (page->deferred_image_job != NULL) {
        size_t count = images_priority_load_target_count(page->deferred_image_job);
        bool current = count <= page->deferred_image_count - page->deferred_image_cursor;
        for (size_t at = 0; current && at < count; at++)
            current = navigation_deferred_target_live(page,
                &page->deferred_image_targets[page->deferred_image_cursor + at]);
        /* Resolve weak identities before the source check can inspect DOM.
           Already-published resources belong to the page, including any
           table changes made by author script between idle pumps. */
        if (!current || !images_priority_load_sources_current(page->deferred_image_job)) {
            images_priority_load_commit_progress(page->deferred_image_job);
            images_priority_load_destroy(page->deferred_image_job);
            page->deferred_image_job = NULL;
            page->deferred_image_batch_count = 0;
            page->deferred_image_rank_valid = false;
            (void) navigation_publish_deferred_images(session);
            return true;
        }
    }
    if (page->deferred_image_job == NULL) {
        ImagePriorityTarget *current =
            &page->deferred_image_targets[page->deferred_image_cursor];
        if (!navigation_deferred_target_live(page, current)) {
            page->deferred_image_cursor++;
            return true;
        }
        if (current->retry_delay != 0) {
            current->retry_delay--;
            return true;
        }
        const NavigationEntry *entry = navigation_current(session);
        int scroll_y = entry == NULL ? 0 : entry->scroll_y;
        /* Both static and mutable plans need a complete stable ranking.
           Recompute after scrolling or layout changes, not per request. */
        if (current->retry_count == 0
            && (!page->deferred_image_rank_valid
                || page->deferred_image_rank_scroll_y != scroll_y)) {
            session->performance.background_image_rank_scans++;
            navigation_rank_deferred_images(session, scroll_y);
            current = &page->deferred_image_targets[
                page->deferred_image_cursor];
        }
        if (page->deferred_image_rank_valid
            && page->deferred_image_cursor >= page->deferred_image_eligible_end) {
            page->image_continuation_pending = false;
            return true;
        }
        ImagePriorityTarget *target = current;
        /* An inline image can be represented by display commands without
           its own node-box entry. Missing geometry lowers priority; it is
           not proof that the image is hidden or should be dropped. */
        if (!navigation_deferred_target_live(page, target)) {
            page->deferred_image_cursor++;
            return true;
        }
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
        size_t batch_count = 1u;
        if (target->retry_count == 0
            && page->deferred_image_cursor + 1u
                   < page->deferred_image_eligible_end) {
            ImagePriorityTarget *next = &page->deferred_image_targets[
                page->deferred_image_cursor + 1u];
            uint16_t next_width = next->display_width;
            uint16_t next_height = next->display_height;
            uint64_t current_distance = navigation_deferred_image_distance(
                session, target, scroll_y, NULL, NULL);
            uint64_t next_distance = navigation_deferred_image_distance(
                session, next, scroll_y, &next_width, &next_height);
            /* Pair within the visible/offscreen priority class. Offscreen
               eager images otherwise force a separate page flow each, even
               though the existing two-response transaction can publish them
               together. Never hold a visible image for an offscreen peer. */
            if ((current_distance == 0) == (next_distance == 0)
                && navigation_deferred_target_live(page, next)
                && next->retry_count == 0) {
                next->display_width = next_width;
                next->display_height = next_height;
                batch_count = 2u;
            }
        }
        page->deferred_image_job = images_priority_load_begin_batch(
            &page->document, &page->stylesheet, &page->images, target,
            batch_count,
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
        page->deferred_image_batch_count = (uint8_t) batch_count;
    }

    size_t loaded_before = page->images.stats.loaded;
    session->performance.background_image_batches++;
    ImagePriorityLoadStatus status = images_priority_load_pump(
        page->deferred_image_job);
    size_t admitted_batch = images_priority_load_target_count(
        page->deferred_image_job);
    if (admitted_batch != 0
        && admitted_batch < page->deferred_image_batch_count) {
        page->deferred_image_batch_count = (uint8_t) admitted_batch;
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
        if (page->deferred_image_publication_age == 0)
            page->deferred_image_publication_age = 1;
        /* Decoded resources are page-owned even while layout publication
           waits briefly for the other member of this two-image batch. */
        images_priority_load_commit_progress(page->deferred_image_job);
    }
    if (page->deferred_image_publication_age != 0) {
        /* At most three further owner pumps: a slow second response cannot
           hold the first image until its network timeout. No bigger queue,
           retained DOM cursor, or unbounded settle delay. */
        if (status == IMAGE_PRIORITY_LOAD_PENDING
            && page->deferred_image_publication_age++ < 4) return true;
        if (!navigation_publish_deferred_images(session)) return true;
    }
    if (status == IMAGE_PRIORITY_LOAD_PENDING) return true;
    images_priority_load_destroy(page->deferred_image_job);
    page->deferred_image_job = NULL;
    ImagePriorityTarget *target =
        &page->deferred_image_targets[page->deferred_image_cursor];
    size_t batch_count = page->deferred_image_batch_count == 0
        ? 1u : page->deferred_image_batch_count;
    page->deferred_image_batch_count = 0;
    size_t available = navigation_deferred_image_available_prefix(
        page, batch_count);
    if (status == IMAGE_PRIORITY_LOAD_FAILED || available < batch_count) {
        /* Individual image failures are intentionally soft inside the image
           loader, so a completed batch can still contain a missing suffix.
           Attribute either shape to the first still-missing target rather
           than re-fetching a thumbnail that is already visible. If
           completions arrived out of order, the prefix remains zero and the
           ordinary retry path safely revisits target 0; a retained target 1
           resource is reused later without another transport request. */
        page->deferred_image_cursor += available;
        target = page->deferred_image_cursor < page->deferred_image_count
            ? &page->deferred_image_targets[page->deferred_image_cursor]
            : NULL;
        if (target != NULL) navigation_deferred_image_failed(session, target);
    } else {
        for (size_t at = 0; at < batch_count; at++) {
            ImagePriorityTarget *completed = &page->deferred_image_targets[
                page->deferred_image_cursor + at];
            completed->retry_count = 0;
            completed->retry_delay = 0;
        }
        page->deferred_image_cursor += batch_count;
    }
    if (page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
    }
    return true;
}

bool navigation_run_deferred_image_work(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    if (!session->page.image_continuation_pending
        || session->page.deferred_image_targets == NULL) return true;
    if (session->maximum_publication_work_units != 0
        && session->page.layout.layout_work_units
               > session->maximum_publication_work_units) {
        /* Every batch is published through a full relayout costing about
           what the committed layout cost. On a page this large that is
           seconds of blocked input per pair of images; keep the fallback
           boxes and finish the queue instead. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-deferred-images: skipped targets=%zu work=%zu limit=%zu",
                 session->page.deferred_image_count,
                 session->page.layout.layout_work_units,
                 session->maximum_publication_work_units);
        tilefinch_platform_log_message(message);
#endif
        navigation_deferred_images_finish(session);
        return true;
    }
    return navigation_run_deferred_image(session);
}

#if !defined(__PSP__)

void navigation_test_refuse_next_background_font_relayout(void)
{
    tilefinch_test_faults()->refuse_next_background_font_relayout = true;
}
#endif

NavigationBackgroundWorkOutcome navigation_run_background_resources(
    NavigationSession *session)
{
    if (session == NULL || !session->page.loaded)
        return NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
    NavigationBackgroundWorkOutcome outcome =
        NAVIGATION_BACKGROUND_WORK_SUCCESS;
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
            bool relaid_out = false;
#if !defined(__PSP__)
            if (tilefinch_test_faults()->refuse_next_background_font_relayout) {
                tilefinch_test_faults()->refuse_next_background_font_relayout = false;
            } else
#endif
            {
                relaid_out = navigation_relayout(session);
            }
            if (!relaid_out) {
                fonts_external_loader_cancel(
                    &session->page.external_font_loader,
                    session->page.resource_scheduler,
                    &session->page.external_fonts);
                session->performance.background_font_failures++;
                /* The fast relayout path constructs a replacement before
                   retiring the current display list. Preserve that valid
                   committed page, classify the optional face as refused,
                   and continue image/tile idle work without manufacturing a
                   page-network error. */
                outcome = NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL;
            } else {
                session->performance.background_font_relayouts++;
            }
        }
    }
    if (!session->page.image_continuation_pending) return outcome;
    if (session->page.deferred_image_targets != NULL) {
        return navigation_run_deferred_image(session)
            ? outcome : NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
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
        return outcome;
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
            return NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
        }
        session->performance.background_image_relayouts++;
    }
    /* The complete authoritative traversal ran once from the currently
       committed DOM. Retrying it in small stateless attempt batches would
       change semantics because corrupt early resources are intentionally not
       retained in the successful-image table and could consume quota again. */
    session->page.image_continuation_pending = false;
    return outcome;
}

static void navigation_accumulate_script_metrics(
    NavigationSession *session, const ExternalScriptMetrics *metrics)
{
    if (metrics->failed != 0u || metrics->skipped_cross_origin != 0u
        || metrics->skipped_quota != 0u
        || metrics->skipped_pressure != 0u
        || metrics->cost_class_rejections != 0u) {
        session->page.script_degradation_observed = true;
    }
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
    const char *document_url;
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
        if (tilefinch_trace_script_failures()) {
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
                walk->document, walk->document_url,
                walk->session->page.resource_base_url)) {
            walk->failed = true;
            break;
        }
        script_runtime_invalidate_document_base(
            walk->session->page.runtime);
        if (document_scripts_process_closed(
                walk->session->page.runtime, walk->session->budget,
                walk->session->browser_session,
                walk->session->page.resource_base_url,
                walk->document_url, walk->session->page.referrer_policy,
                &walk->session->page.document.content_security_policy,
                walk->maximum_scripts, walk->maximum_bytes,
                walk->session->maximum_script_file_bytes,
                walk->session->script_timeout_ms,
                walk->session->page.resource_scheduler,
                script, walk->state)
            == DOCUMENT_SCRIPT_PROCESS_HARD_FAILURE) {
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
    const char *document_url = navigation_active_document_url(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL
        || document_url == NULL) {
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
        .document_url = document_url,
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
            document, document_url, session->page.resource_base_url)) {
        return false;
    }
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = snapshot_ok && !walk.failed && document_scripts_finish_streaming(
        document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        document_url,
        session->page.referrer_policy, remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &state, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
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
        && navigation_load_frames(session, document_url);
}

bool navigation_execute_document_scripts_from(
    NavigationSession *session, PocDocument *document)
{
    if (session == NULL || document == NULL
        || !session->document_scripts_enabled) return true;
    const char *document_url = navigation_active_document_url(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL
        || document_url == NULL) {
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
            document, document_url, session->page.resource_base_url)) {
        return false;
    }
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = document_scripts_execute(
        document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        document_url, session->page.referrer_policy,
        remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
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
        || !navigation_load_frames(session, document_url)) return false;
    return true;
}

bool navigation_execute_current_body_scripts(NavigationSession *session)
{
    if (session == NULL || !session->document_scripts_enabled) return true;
    const char *document_url = navigation_active_document_url(session);
    if (!session->page.loaded || session->page.runtime == NULL
        || session->page.resource_scheduler == NULL
        || document_url == NULL) {
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
            &session->page.document, document_url,
            session->page.resource_base_url)) return false;
    script_runtime_invalidate_document_base(session->page.runtime);
    bool ok = document_body_scripts_execute(
        &session->page.document, session->page.runtime, session->budget,
        session->browser_session, session->page.resource_base_url,
        document_url, session->page.referrer_policy,
        remaining_scripts, remaining_bytes,
        session->maximum_script_file_bytes, session->script_timeout_ms,
        session->page.resource_scheduler, &metrics,
        &session->page.script_result);
    session->performance.script_us += navigation_record_script_slices(
        session, started, &session->page.script_result,
        metrics.attempted + session->page.script_result.scripts_evaluated);
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
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

bool navigation_layout_is_visually_blank(
    const LayoutDocument *layout)
{
    if (layout == NULL) return false;
    /* Interaction tables are not paint evidence. visibility:hidden zeros
       their geometry only at the final visibility pass, while opacity:0
       deliberately preserves hit testing. In both cases a retained link or
       control can coexist with a completely blank framebuffer. Ordinary
       visible links and controls contribute draw commands below. */
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->width <= 0 || command->height <= 0) continue;
        /* Visibility:hidden and opacity:0 retain geometry for stable layout,
           hit-test generations, and later author toggles, but rasterize no
           pixels. They cannot make an otherwise blank page useful. */
        if (command->opacity_scale == 0u) continue;
        /* A page-wide background rectangle is still a blank presentation.
           Text, images, outlines, shadows, and any second painted layer are
           observable output and must keep author runtime semantics. */
        if (command->type != DRAW_FILL_RECT
            || command->color != layout->page_background
            || draw_command_fill_gradient(command, NULL)
            || draw_command_backdrop_blur(command) != 0u
            || draw_command_filter_code(command) != 0u
            || draw_command_rotation_quadrants(command) != 0u
            || draw_command_blend_mode(command) != LAYOUT_MIX_BLEND_NORMAL
            || command->radius != 0
            || command->opacity_scale != 256u
            || command->x > 0 || command->y > 0
            || command->width < layout->viewport.css_width
            || command->height < layout->viewport.css_height) return false;
    }
    return true;
}

static bool navigation_recovery_semantic_element(lxb_dom_node_t *node)
{
    return node_name_is(node, "main") || node_name_is(node, "article")
        || node_name_is(node, "section") || node_name_is(node, "p")
        || node_name_is(node, "li") || node_name_is(node, "blockquote")
        || node_name_is(node, "pre") || node_name_is(node, "table")
        || node_name_is(node, "details") || node_name_is(node, "h1")
        || node_name_is(node, "h2") || node_name_is(node, "h3")
        || node_name_is(node, "h4") || node_name_is(node, "h5")
        || node_name_is(node, "h6");
}

static bool navigation_recovery_excluded_element(lxb_dom_node_t *node)
{
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    size_t length = 0;
    const char *aria_hidden = document_attribute(
        node, "aria-hidden", &length);
    return node_name_is(node, "script") || node_name_is(node, "style")
        || node_name_is(node, "template") || node_name_is(node, "noscript")
        || node_name_is(node, "svg") || node_name_is(node, "dialog")
        || node_name_is(node, "nav") || node_name_is(node, "aside")
        || node_name_is(node, "footer") || node_name_is(node, "form")
        || lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "hidden", 6u)
        || lxb_dom_element_has_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) "inert", 5u)
        || (aria_hidden != NULL && length == 4u
            && strncasecmp(aria_hidden, "true", 4u) == 0);
}

/* This is evidence for static recovery, not a visibility override. Count a
   bounded amount of prose inside semantic content, excluding navigation,
   forms, dialogs, and authored hidden/inert subtrees. The subsequent style
   fallback remains responsible for its narrow, content-shaped reveal rules. */
static bool navigation_document_has_bounded_retained_content(
    PocDocument *document)
{
    lxb_dom_node_t *root = document_body_node(document);
    if (root == NULL) return false;
    enum { RETAINED_TEXT_SCAN_LIMIT = 8192 };
    size_t text_bytes = 0, semantic_text_bytes = 0, inspected_bytes = 0;
    lxb_dom_node_t *node = root->first_child;
    for (size_t visited = 0; node != NULL && visited < 1024u; visited++) {
        bool skip_children = false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            skip_children = navigation_recovery_excluded_element(node);
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            bool semantic = false;
            lxb_dom_node_t *ancestor = node->parent;
            for (size_t depth = 0;
                 ancestor != NULL && ancestor != root && depth < 64u;
                 depth++, ancestor = ancestor->parent) {
                if (navigation_recovery_semantic_element(ancestor)) {
                    semantic = true;
                    break;
                }
            }
            for (size_t at = 0; text != NULL && at < length
                 && inspected_bytes < RETAINED_TEXT_SCAN_LIMIT; at++) {
                inspected_bytes++;
                if (isspace((unsigned char) text[at])) continue;
                if (text_bytes != SIZE_MAX) text_bytes++;
                if (semantic && semantic_text_bytes != SIZE_MAX)
                    semantic_text_bytes++;
                if (text_bytes >= 128u && semantic_text_bytes >= 96u)
                    return true;
                /* Plain server-rendered prose frequently uses only divs.
                   Enough bounded non-whitespace text is useful recovery
                   evidence even without an article/main/p ancestor. */
                if (text_bytes >= 512u) return true;
            }
            if (inspected_bytes == RETAINED_TEXT_SCAN_LIMIT) return false;
        }
        if (!skip_children && node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        node = node == root ? NULL : node->next;
    }
    return false;
}

static bool navigation_blank_recovery_has_useful_content(
    NavigationSession *session)
{
    if (session == NULL) return false;
    bool retained = navigation_document_has_bounded_retained_content(
        &session->page.document);
    ReaderDocumentAnalysis analysis = session->page.reader_analysis;
    if (analysis.prepared
        && (analysis.bounded_out || analysis.extraction_truncated)) {
        return false;
    }
    /* Avoid allocating a Reader clone on the ordinary complete-prose path:
       this predicate runs inside the allocator-refusal recovery boundary.
       Conservatively decline documents outside the extractor's structural
       and byte ceilings; the frontend may still offer an explicit Reader
       attempt later. */
    if (retained) {
        if (session->page.document.node_count > 512u
            || session->page.document.body_text_length > 256u * 1024u) {
            return false;
        }
        return true;
    }
    /* A RAW analysis may have been cached before a late author mutation.
       RAW installs no markers or clone, so it is safe to analyze the settled
       document once more at this rare allocator-failure boundary. */
    bool analyzed_for_recovery =
        !analysis.prepared || analysis.kind == READER_PAGE_RAW;
    if (analyzed_for_recovery) {
        analysis = (ReaderDocumentAnalysis) {0};
        if (!reader_document_prepare_complete_with_stylesheet(
                &session->page.document, &session->page.stylesheet,
                &analysis)) {
            analysis = (ReaderDocumentAnalysis) {
                .prepared = true,
                .bounded_out = true
            };
        }
        session->page.reader_analysis = analysis;
    }
    /* Realm retirement is permanent. A bounded prefix can establish useful
       prose without proving that the uninspected tail is not a form or app
       shell, so never make that irreversible transition from a truncated
       extraction. Complete retained semantic text remains a valid fallback
       even when its score is below Reader's ARTICLE threshold. */
    if (analysis.bounded_out || analysis.extraction_truncated) {
        if (analyzed_for_recovery) {
            /* This strict preflight installed no tree. Leave explicit Reader
               free to perform its separately permitted shortened extraction
               later rather than caching analysis-only as presentation-ready. */
            session->page.reader_analysis.prepared = false;
        }
        return false;
    }
    return analysis.kind != READER_PAGE_RAW;
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
    size_t heap_rejections_before = script_runtime_heap_rejections(
        session->page.runtime);
    bool advanced = script_runtime_advance(
        session->page.runtime, elapsed_ms, callback_budget,
        &session->page.script_result);
    /* A top-level realm owns the document transition once it requests one.
       Do not let refresh retirement or stale child-frame work from the
       outgoing document veto that committed replacement in the same tick. */
    if (script_runtime_has_pending_navigation(session->page.runtime)) {
        goto consume_script_navigation;
    }
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
    if (!advanced) {
        capture_cached_script_error(session,
                                    session->page.script_result.error);
        /*
         * Once the authoritative document is committed, a later author task
         * exhausting its bounded realm must not take the browser shell or
         * readable page down with it. Parser-time failures still fail closed
         * because their DOM may be incomplete; this path is safe to degrade
         * to the already-committed static document.
         */
        if (navigation_retire_exhausted_script_realms(session, false)) {
            /* Retirement enables the static-content cascade after the last
               script-driven layout may already have hidden the page shell.
               Rebuild immediately while the old layout is still retained;
               a refused rebuild leaves that committed layout usable. */
            (void) navigation_relayout_static_fallback(session);
            session->performance.runtime_us += navigation_record_slice(
                session, NAVIGATION_SLICE_RUNTIME, runtime_started,
                callback_budget);
            return true;
        }
        unsigned long generated_line = 0, generated_column = 0;
        bool captured_generated_source = false;
        if (tilefinch_trace_eval_source()
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
    if (tilefinch_trace_callback_source()
        && session->last_script_error_context[0] == '\0'
        && session->page.script_result.last_uncaught_callback_error[0]
            != '\0') {
        collect_document_stack_source_context_into(
            &session->page.document,
            session->page.script_result.last_uncaught_callback_error,
            session->last_script_error_context,
            sizeof(session->last_script_error_context));
    }
    const char *current_url = navigation_active_document_url(session);
    if (session->document_scripts_enabled && current_url != NULL
        && !navigation_load_frames(session, current_url)) {
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
        bool frame_advanced = script_runtime_advance(
            frame->runtime, elapsed_ms, callback_budget,
            &frame->script_result);
        /* A child callback may fail after the top realm requested a reload.
           The child belongs to the page being replaced and must not suppress
           that already-published top-level navigation. */
        if (script_runtime_has_pending_navigation(session->page.runtime)) {
            goto consume_script_navigation;
        }
        if (navigation_retire_failed_document_refresh(
                session, frame->runtime)) return false;
        if (!frame_advanced) {
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
        if (script_runtime_consume_relayout(frame->runtime)) {
            bool frame_presented = navigation_refresh_frame_presentation(
                session, frame);
            if (!frame_presented && !session->page.loaded) return false;
        }
        if (tilefinch_trace_callback_source()
            && frame->callback_error_source_context[0] == '\0'
            && frame->script_result.last_uncaught_callback_error[0] != '\0') {
            collect_stack_source_context_into(
                frame, frame->script_result.last_uncaught_callback_error,
                frame->callback_error_source_context,
                sizeof(frame->callback_error_source_context));
        }
    }
    if (!navigation_deliver_frame_messages(session)) {
        if (navigation_retire_exhausted_script_realms(session, false)) {
            session->performance.frame_message_soft_failures++;
            (void) navigation_relayout_static_fallback(session);
            session->performance.runtime_us += navigation_record_slice(
                session, NAVIGATION_SLICE_RUNTIME, runtime_started,
                callback_budget);
            return true;
        }
        snprintf(session->last_error, sizeof(session->last_error),
                 "cross-frame message delivery failed: %.450s",
                 session->page.script_result.error);
        return false;
    }
consume_script_navigation:
    ;
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
        const char *current_url = navigation_active_document_url(session);
        if (current_url != NULL) {
            snprintf(session->pending_navigation_referer,
                     sizeof(session->pending_navigation_referer), "%s",
                     current_url);
            session->pending_navigation_same_origin =
                navigation_same_origin(current_url, navigation_url);
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
    size_t heap_rejections_after = script_runtime_heap_rejections(
        session->page.runtime);
    bool relayout_requested =
        script_runtime_consume_relayout(session->page.runtime);
    /* A page task may catch an actual QuickJS allocation refusal, leaving the
       scheduler alive even though its last mutation hid the entire authored
       shell. Distinguish that allocator fact from an author merely throwing
       text containing "out of memory", and degrade only a blank, text-rich
       committed page. */
    bool allocator_rejected =
        heap_rejections_after > heap_rejections_before;
    bool ok = true;
    if (relayout_requested) {
        ok = navigation_relayout(session);
    }
    /* Inspect the successfully settled layout, not the pre-mutation command
       count. Only a real allocator rejection plus blank paint/control output
       and bounded useful content may retire a realm. Hard relayout/security
       failures still return above their ordinary fail-closed path. */
    if (ok && allocator_rejected
        && navigation_layout_is_visually_blank(&session->page.layout)
        && navigation_blank_recovery_has_useful_content(session)
        && navigation_retire_exhausted_script_realms(session, true)) {
        (void) navigation_relayout_static_fallback(session);
    }
    session->performance.runtime_us += navigation_record_slice(
        session, NAVIGATION_SLICE_RUNTIME, runtime_started, callback_budget);
    return ok;
}

bool navigation_set_gamepad_state(
    NavigationSession *session, const TilefinchGamepadState *state)
{
    if (session == NULL || state == NULL || !session->page.loaded)
        return false;
    if (session->page.runtime == NULL) return !state->connected;
    return script_runtime_set_gamepad_state(session->page.runtime, state);
}

bool navigation_dispatch_event(NavigationSession *session,
                               const char *selector,
                               const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded) return false;
    if (!script_runtime_dispatch(session->page.runtime, selector, event_type,
                                 &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
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

bool navigation_dispatch_node_event(NavigationSession *session,
                                    lxb_dom_node_t *node,
                                    const char *event_type)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || node == NULL) return false;
    if (!script_runtime_dispatch_node(session->page.runtime, node, event_type,
                                      &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
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
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
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

static NavigationFrame *navigation_loaded_frame_for_element(
    NavigationSession *session, ScriptRuntime *parent_runtime,
    lxb_dom_node_t *frame_element)
{
    if (session == NULL || frame_element == NULL
        || !session->page.loaded) return NULL;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *candidate = &session->page.frames[i];
        if (!candidate->retired && candidate->loaded
            && candidate->runtime != NULL
            && candidate->presentation != NULL
            && candidate->parent_runtime == parent_runtime
            && candidate->element == frame_element) {
            return candidate;
        }
    }
    return NULL;
}

static bool navigation_finish_frame_activation(
    NavigationSession *session, NavigationFrame *frame,
    lxb_dom_node_t *target)
{
    if (session == NULL || frame == NULL || target == NULL) return false;
    if (!script_runtime_dispatch_activation_node(
            frame->runtime, target, &frame->script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, frame->runtime);
        return false;
    }
    bool relayout = script_runtime_consume_relayout(frame->runtime);
    if (relayout) {
        bool frame_presented = navigation_refresh_frame_presentation(
            session, frame);
        if (!frame_presented && !session->page.loaded) return false;
    }
    return navigation_deliver_frame_messages(session);
}

static bool navigation_finish_frame_pointer_click(
    NavigationSession *session, NavigationFrame *frame,
    lxb_dom_node_t *target, int client_x, int client_y,
    int offset_x, int offset_y)
{
    if (session == NULL || frame == NULL || target == NULL) return false;
    long target_handle = script_runtime_node_handle(
        frame->runtime, target);
    if (target_handle == 0) return false;
    static const unsigned phases[] = {2u, 3u, 5u};
    static const unsigned buttons[] = {1u, 0u, 0u};
    bool trace = tilefinch_trace_frame_controls();
    for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        lxb_dom_node_t *live_target =
            script_runtime_node_handle_resolve_connected(
                frame->runtime, target_handle);
        if (live_target == NULL) {
            if (trace)
                fprintf(stderr, "frame-pointer-phase=%u failure=resolve\n",
                        phases[i]);
            script_runtime_node_handle_release(frame->runtime, target_handle);
            return false;
        }
        if (!script_runtime_dispatch_pointer_node(
                frame->runtime, live_target, phases[i],
                client_x, client_y, offset_x, offset_y, buttons[i],
                &frame->script_result)) {
            if (trace)
                fprintf(stderr,
                        "frame-pointer-phase=%u failure=dispatch error=\"%s\"\n",
                        phases[i], frame->script_result.error);
            script_runtime_node_handle_release(frame->runtime, target_handle);
            (void) navigation_retire_failed_document_refresh(
                session, frame->runtime);
            return false;
        }
        if (trace)
            fprintf(stderr, "frame-pointer-phase=%u success\n", phases[i]);
    }
    script_runtime_node_handle_release(frame->runtime, target_handle);
    bool relayout = script_runtime_consume_relayout(frame->runtime);
    if (relayout) {
        bool frame_presented = navigation_refresh_frame_presentation(
            session, frame);
        if (!frame_presented && !session->page.loaded) return false;
    }
    bool delivered = navigation_deliver_frame_messages(session);
    if (trace && !delivered)
        fprintf(stderr, "frame-pointer-failure=message-delivery\n");
    return delivered;
}

bool navigation_activate_frame(NavigationSession *session,
                               lxb_dom_node_t *frame_element)
{
    NavigationFrame *frame = navigation_loaded_frame_for_element(
        session, session == NULL ? NULL : session->page.runtime,
        frame_element);
    if (frame == NULL) return false;
    bool trace_frame_controls =
        tilefinch_trace_frame_messages()
        || tilefinch_trace_frame_controls();
    lxb_dom_node_t *target = NULL;
    for (size_t depth = 0; depth < NAVIGATION_FRAME_LIMIT; depth++) {
        const LayoutDocument *layout = &frame->presentation->layout;
        const ControlRegion *nested_region = NULL;
        uint64_t nested_area = 0;
        if (trace_frame_controls) {
            fprintf(stderr,
                    "frame-activation-controls frame=%zu depth=%zu "
                    "controls=%zu links=%zu boxes=%zu\n",
                    (size_t) (frame - session->page.frames), depth,
                    layout->control_count, layout->link_count,
                    layout->node_box_count);
        }
        for (size_t i = 0; i < layout->control_count; i++) {
            const ControlRegion *control = &layout->controls[i];
            if (trace_frame_controls) {
                size_t name_length = 0, type_length = 0, role_length = 0,
                       id_length = 0;
                const char *name = document_element_name(
                    control->node, &name_length);
                const char *type = document_attribute(
                    control->node, "type", &type_length);
                const char *role = document_attribute(
                    control->node, "role", &role_length);
                const char *id = document_attribute(
                    control->node, "id", &id_length);
                fprintf(stderr,
                        "frame-activation-control index=%zu kind=%u "
                        "rect=%d,%d,%d,%d tag=\"%.*s\" type=\"%.*s\" "
                        "role=\"%.*s\" id=\"%.*s\"\n",
                        i, (unsigned) control->type, control->x, control->y,
                        control->width, control->height,
                        (int) name_length, name == NULL ? "" : name,
                        (int) type_length, type == NULL ? "" : type,
                        (int) role_length, role == NULL ? "" : role,
                        (int) id_length, id == NULL ? "" : id);
            }
            if (control->node == NULL || control->width <= 0
                || control->height <= 0
                || control->type == CONTROL_RESIZE) continue;
            if (control->type == CONTROL_FRAME) {
                uint64_t area = (uint64_t) (unsigned) control->width
                              * (uint64_t) (unsigned) control->height;
                if (area > nested_area) {
                    nested_area = area;
                    nested_region = control;
                }
                continue;
            }
            target = control->node;
            if (control->type == CONTROL_TOGGLE) break;
        }
        if (target != NULL) break;
        if (nested_region == NULL) return false;
        NavigationFrame *nested = navigation_loaded_frame_for_element(
            session, frame->runtime, nested_region->node);
        if (nested == NULL) return false;
        frame = nested;
    }
    if (target == NULL) return false;
    return navigation_finish_frame_activation(session, frame, target);
}

bool navigation_activate_frame_at(NavigationSession *session,
                                  lxb_dom_node_t *frame_element,
                                  int offset_x, int offset_y)
{
    NavigationFrame *frame = navigation_loaded_frame_for_element(
        session, session == NULL ? NULL : session->page.runtime,
        frame_element);
    if (frame == NULL || offset_x < 0 || offset_y < 0) return false;
    for (size_t depth = 0; depth < NAVIGATION_FRAME_LIMIT; depth++) {
        const LayoutDocument *layout = &frame->presentation->layout;
        bool trace_frame_controls =
            tilefinch_trace_frame_controls();
        if (trace_frame_controls) {
            fprintf(stderr,
                    "frame-pointer-hit depth=%zu point=%d,%d controls=%zu "
                    "links=%zu boxes=%zu\n",
                    depth, offset_x, offset_y, layout->control_count,
                    layout->link_count, layout->node_box_count);
        }
        const ControlRegion *control = layout_control_at(
            layout, offset_x, offset_y);
        if (control != NULL && control->type == CONTROL_FRAME) {
            NavigationFrame *nested = navigation_loaded_frame_for_element(
                session, frame->runtime, control->node);
            if (nested == NULL) return false;
            offset_x -= control->x;
            offset_y -= control->y;
            if (offset_x < 0 || offset_y < 0) return false;
            frame = nested;
            continue;
        }
        lxb_dom_node_t *target = control == NULL ? NULL : control->node;
        int target_x = control == NULL ? 0 : control->x;
        int target_y = control == NULL ? 0 : control->y;
        if (target == NULL) {
            const LinkRegion *link = layout_link_at(
                layout, offset_x, offset_y);
            if (link != NULL) {
                target = link->node;
                target_x = link->x;
                target_y = link->y;
            }
        }
        if (target == NULL) {
            /* Layout stores the deepest painted containing box first, which
               is the pointer target used by the top-level controller too. */
            for (size_t i = 0; i < layout->node_box_count; i++) {
                const LayoutNodeBox *box = &layout->node_boxes[i];
                if (box->node != NULL && box->width > 0 && box->height > 0
                    && offset_x >= box->x
                    && offset_x < box->x + box->width
                    && offset_y >= box->y
                    && offset_y < box->y + box->height) {
                    target = box->node;
                    target_x = box->x;
                    target_y = box->y;
                    break;
                }
            }
        }
        return target != NULL
            && navigation_finish_frame_pointer_click(
                session, frame, target, offset_x, offset_y,
                offset_x - target_x, offset_y - target_y);
    }
    return false;
}

bool navigation_dispatch_node_pointer(
    NavigationSession *session, lxb_dom_node_t *node, unsigned phase,
    int client_x, int client_y, int offset_x, int offset_y,
    unsigned buttons)
{
    if (session == NULL || session->page.runtime == NULL
        || !session->page.loaded || (node == NULL && phase != 1u)) return false;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t pointer_started = navigation_now_us();
#endif
    if (!script_runtime_dispatch_pointer_node(
        session->page.runtime, node, phase, client_x, client_y,
            offset_x, offset_y, buttons, &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t pointer_dispatched = navigation_now_us();
#endif
    int scroll_y = 0;
    if (script_runtime_consume_scroll(session->page.runtime, &scroll_y)) {
        int maximum = navigation_max_scroll(session);
        navigation_set_scroll(session, scroll_y < maximum ? scroll_y : maximum);
    }
    bool finished = !script_runtime_consume_relayout(session->page.runtime)
        || navigation_relayout(session);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (phase == 5u) {
        char pointer_phases[160];
        snprintf(pointer_phases, sizeof(pointer_phases),
            "tilefinch-pointer-phases: script=%lluus settle=%lluus",
            (unsigned long long) (pointer_dispatched - pointer_started),
            (unsigned long long) (navigation_now_us() - pointer_dispatched));
        tilefinch_platform_log_message(pointer_phases);
    }
#endif
    return finished;
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
            &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
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
            &session->page.script_result)) {
        (void) navigation_retire_failed_document_refresh(
            session, session->page.runtime);
        return false;
    }
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
    if (navigation_retire_failed_document_refresh(
            session, session->page.runtime)) return false;
    bool relayout_ok = !script_runtime_consume_relayout(session->page.runtime)
                       || navigation_relayout(session);
    return evaluated && relayout_ok;
}

void navigation_destroy(NavigationSession *session)
{
    if (session == NULL) return;
    page_destroy(session);
    /* Direct NavigationSession owners do not pass through BrowserEngine's
       teardown coordinator. Reap the process-wide JPEG slot here as well so
       its retained body and arena cannot outlive this session's Budget. */
    (void) images_decode_worker_shutdown(session->budget);
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
