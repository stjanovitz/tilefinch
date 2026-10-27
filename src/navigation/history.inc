/* Session history: back/forward, same-document URL commits and restores,
   fragment targets and scrolling, and scroll clamping. The host-only
   refusal hooks at the top belong to relayout.inc; they stay here so the
   translation unit keeps its order. */

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

/* Whether the committed layout paints anything a reader could see. */
static bool navigation_layout_paints_content(const LayoutDocument *layout)
{
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if ((command->type == DRAW_TEXT && command->text_length != 0)
            || (command->type == DRAW_IMAGE && command->width > 0
                && command->height > 0)) return true;
    }
    return false;
}

bool navigation_bot_wall(const NavigationSession *session,
                         NavigationBotWall *wall)
{
    if (wall != NULL) memset(wall, 0, sizeof(*wall));
    if (session == NULL || wall == NULL || !session->page.loaded) {
        return false;
    }
    long status = session->last_http_status;
    const NavigationEntry *entry = navigation_current(session);
    const char *vendor = session->last_bot_wall_vendor[0] != '\0'
        ? session->last_bot_wall_vendor
        : tilefinch_bot_wall_title_vendor(
              status, entry == NULL ? NULL : entry->title);
    bool refused_script = session->script_skipped_quota != 0
        || session->page.script_result.dynamic_scripts_quota_rejected != 0;
    bool blank = vendor == NULL && status >= 400 && refused_script
        && !navigation_layout_paints_content(&session->page.layout);
    if (vendor == NULL && !blank) return false;
    wall->status = status;
    wall->blank_after_refused_script = blank;
    if (vendor != NULL) {
        snprintf(wall->vendor, sizeof(wall->vendor), "%s", vendor);
    }
    const char *url = session->page.document_url[0] != '\0'
        ? session->page.document_url
        : (entry == NULL ? NULL : entry->url);
    (void) tilefinch_bot_wall_site(url, wall->site, sizeof(wall->site));
    return true;
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
        staged[index].focus_identity = source->focus_identity;
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
    /* The target may lie below a provisional layout's placeholder. */
    if (target != NULL) (void) navigation_complete_layout_sync(session);
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
        if (session->committed_layout_servable) {
            /* Scroll feedback during a replacement build must not reenter
               author code: even the advisory viewport setter is observable. */
            session->committed_scroll_reflection_pending = true;
        } else {
            (void) script_runtime_set_page_scroll(session->page.runtime, next);
        }
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
       Update native geometry now, but never invoke an author viewport setter
       from the publication tail: it could delete the newly laid-out text. */
    if (session->page.runtime != NULL) {
        session->committed_scroll_reflection_pending = true;
    }
    entry->scroll_y = maximum;
}
