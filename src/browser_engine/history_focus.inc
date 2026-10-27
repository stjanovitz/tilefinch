/* History and focus restore: per-entry focus identity and form-control
   state, same-document history actions, autofocus, and the initial view of
   a committed navigation. */

static void browser_engine_navigation_restore_history(
    BrowserEngine *engine, BrowserEngineNavigationWork *work)
{
    if (engine == NULL || work == NULL || !work->history_move
        || engine->navigation.history_count == 0
        || engine->navigation.history_index == work->previous_history_index) {
        return;
    }
    const NavigationEntry *ignored = NULL;
    (void) (work->history_forward
        ? navigation_back(&engine->navigation, &ignored)
        : navigation_forward(&engine->navigation, &ignored));
}

static lxb_dom_node_t *browser_engine_semantic_focus(
    const BrowserEngine *engine, int kind, size_t ordinal, size_t *raw_index)
{
    if (raw_index != NULL) *raw_index = SIZE_MAX;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    if (kind == CONTROLLER_FOCUS_CONTROL) {
        if (raw_index != NULL) *raw_index = ordinal;
        return ordinal < layout->control_count
            ? layout->controls[ordinal].node : NULL;
    }
    if (kind != CONTROLLER_FOCUS_LINK || layout->link_count == 0) return NULL;
    size_t i = 0;
    while (ordinal != 0 && ++i < layout->link_count) {
        if (layout->links[i].node != layout->links[i - 1u].node) ordinal--;
    }
    if (ordinal != 0) return NULL;
    if (raw_index != NULL) *raw_index = i;
    return layout->links[i].node;
}

static size_t browser_engine_focus_ordinal(const BrowserEngine *engine)
{
    int kind = engine->controller.focus_kind;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    size_t raw = engine->controller.focus_index;
    if (kind != CONTROLLER_FOCUS_LINK || raw >= layout->link_count) return raw;
    size_t ordinal = 0;
    for (size_t i = 1; i <= raw; i++) {
        if (layout->links[i].node != layout->links[i - 1u].node) ordinal++;
    }
    return ordinal;
}

static uint64_t browser_engine_focus_identity(
    const NavigationSession *navigation, int kind, size_t raw_index)
{
    const LayoutDocument *layout = &navigation->page.layout;
    lxb_dom_node_t *node = kind == CONTROLLER_FOCUS_LINK && raw_index < layout->link_count
        ? layout->links[raw_index].node
        : kind == CONTROLLER_FOCUS_CONTROL && raw_index < layout->control_count
            ? layout->controls[raw_index].node : NULL;
    if (node == NULL) return 0;
    size_t length = 0;
    const char *value = document_attribute(node, "id", &length);
    char resolved[NAVIGATION_URL_LIMIT];
    unsigned discriminator = 1;
    if (value == NULL || length == 0) {
        discriminator = kind == CONTROLLER_FOCUS_LINK ? 2 : 3;
        if (kind == CONTROLLER_FOCUS_LINK) {
            /* Hydration can change href spelling without changing its
               destination. Resolve only the focused/common-ordinal item;
               other candidates are visited only after that match fails. */
            value = layout->links[raw_index].url;
            const char *base = navigation->page.resource_base_url[0]
                ? navigation->page.resource_base_url : navigation->page.document_url;
            if (value != NULL && fetch_resolve_url(base, value, resolved, sizeof(resolved)))
                value = resolved;
            length = value == NULL ? 0 : strnlen(value, NAVIGATION_URL_LIMIT);
        } else value = document_attribute(node, "name", &length);
    }
    if (value == NULL || length == 0 || length >= NAVIGATION_URL_LIMIT) return 0;
    uint64_t hash = UINT64_C(14695981039346656037) ^ discriminator;
    for (size_t i = 0; i < length; i++)
        hash = (hash ^ (unsigned char) value[i]) * UINT64_C(1099511628211);
    return hash ? hash : 1;
}

static lxb_dom_node_t *browser_engine_history_focus(
    const BrowserEngine *engine, int kind, size_t ordinal, uint64_t identity)
{
    const LayoutDocument *layout = &engine->navigation.page.layout;
    if (identity != 0) {
        size_t raw_index;
        lxb_dom_node_t *usual = browser_engine_semantic_focus(engine, kind, ordinal, &raw_index);
        if (browser_engine_focus_identity(&engine->navigation, kind, raw_index) == identity) return usual;
        size_t count = kind == CONTROLLER_FOCUS_LINK ? layout->link_count
            : kind == CONTROLLER_FOCUS_CONTROL ? layout->control_count : 0;
        lxb_dom_node_t *found = NULL, *previous = NULL;
        size_t position = 0, best_distance = SIZE_MAX;
        for (size_t i = 0; i < count; i++) {
            lxb_dom_node_t *node = kind == CONTROLLER_FOCUS_LINK
                ? layout->links[i].node : layout->controls[i].node;
            if (node == previous) continue;
            previous = node;
            if (browser_engine_focus_identity(&engine->navigation, kind, i) == identity) {
                size_t distance = position > ordinal ? position - ordinal : ordinal - position;
                if (distance < best_distance) { found = node; best_distance = distance; }
                /* Later ordinals cannot be closer once a matching item at
                   or beyond the saved ordinal has been examined. */
                if (position >= ordinal) break;
            }
            position++;
        }
        if (found != NULL) return found;
    }
    return browser_engine_semantic_focus(engine, kind, ordinal, NULL);
}

/* Copies the restorable view of a history entry, the one place its
   control records are clamped. */
static void browser_engine_history_view_from_entry(
    BrowserHistoryView *view, const NavigationEntry *entry)
{
    view->scroll_y = entry->scroll_y;
    view->focus_kind = entry->focus_kind;
    view->focus_index = entry->focus_index;
    view->focus_identity = entry->focus_identity;
    view->control_count = entry->control_count;
    if (view->control_count > NAVIGATION_HISTORY_CONTROL_LIMIT)
        view->control_count = NAVIGATION_HISTORY_CONTROL_LIMIT;
    memcpy(view->controls, entry->controls,
           view->control_count * sizeof(*view->controls));
}

/* Applies a committed history document's saved focus, then its scroll.
   Returns whether the scroll position was applied. */
static bool browser_engine_navigation_restore_view(
    BrowserEngine *engine, const BrowserEngineNavigationWork *work)
{
    if (engine == NULL || work == NULL || !work->history_move) return false;
    lxb_dom_node_t *focus = browser_engine_history_focus(
        engine, work->restore.focus_kind, work->restore.focus_index,
        work->restore.focus_identity);
    if (focus != NULL) (void) controller_focus_node(&engine->controller, focus);
    /* Focus notifications may relayout or reveal. History owns the exact
       viewport, so apply its saved position after those side effects. */
    return navigation_set_scroll(
        &engine->navigation, work->restore.scroll_y);
}

static NavigationEntry *browser_engine_restorable_entry(BrowserEngine *engine)
{
    NavigationEntry *entry = engine != NULL
        && engine->navigation.history_count != 0
        && engine->navigation.history_index < engine->navigation.history_count
        ? &engine->navigation.history[engine->navigation.history_index]
        : NULL;
    if (entry == NULL
        || !browser_engine_history_url_matches_document(
               entry->url, engine->navigation.page.document_url)) {
        /* A redirected document does not own the traversed entry's saved
           controls or focus. Apply this also to ordinary input completion. */
        return NULL;
    }
    return entry;
}

static void browser_engine_store_current_focus(BrowserEngine *engine)
{
    NavigationEntry *entry = browser_engine_restorable_entry(engine);
    if (entry == NULL) return;
    entry->focus_kind = (int) engine->controller.focus_kind;
    entry->focus_index = browser_engine_focus_ordinal(engine);
    entry->focus_identity = browser_engine_focus_identity(
        &engine->navigation, entry->focus_kind, engine->controller.focus_index);
}

static lxb_dom_node_t *browser_engine_history_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static void browser_engine_store_current_controls(BrowserEngine *engine)
{
    NavigationEntry *entry = browser_engine_restorable_entry(engine);
    if (entry == NULL) return;
    entry->control_count = 0u;
    /* Disclosure visibility determines all subsequent control ordinals, so
       it is recorded first: one record whose bits are the open states by
       disclosure ordinal, leaving the other slots for values. A document
       with more disclosures than the bits hold saves no values, which
       could otherwise be assigned to the wrong controls. */
    lxb_dom_node_t *root = lxb_dom_interface_node(engine->navigation.page.document.html);
    lxb_dom_node_t *node = root;
    NavigationHistoryControl *disclosures = NULL;
    unsigned ordinal = 0u;
    for (unsigned visited = 0u; node != NULL && visited < BROWSER_ENGINE_DOM_WALK_LIMIT;
         visited++, node = browser_engine_history_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_DETAILS) continue;
        if (disclosures == NULL) {
            disclosures = &entry->controls[entry->control_count++];
            memset(disclosures, 0, sizeof(*disclosures));
            disclosures->value_length = NAVIGATION_HISTORY_CONTROL_DISCLOSURES;
        }
        if (ordinal >= 8u * sizeof(disclosures->value)) {
            disclosures->control_index = (uint16_t) ordinal;
            return;
        }
        if (lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
                (const lxb_char_t *) "open", 4u))
            disclosures->value[ordinal / 8u] |= (char) (1u << (ordinal % 8u));
        ordinal++;
    }
    if (disclosures != NULL) disclosures->control_index = (uint16_t) ordinal;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t index = 0;
         index < layout->control_count
         && entry->control_count < NAVIGATION_HISTORY_CONTROL_LIMIT;
         index++) {
        lxb_dom_node_t *node = layout->controls[index].node;
        size_t type_length = 0u;
        const char *type = node == NULL ? NULL
            : document_attribute(node, "type", &type_length);
        if (type != NULL
            && ((type_length == 8u
                 && strncasecmp(type, "password", 8u) == 0)
                || (type_length == 4u
                    && strncasecmp(type, "file", 4u) == 0))) {
            continue;
        }
        size_t value_length = 0u;
        char selected[128];
        size_t option_count = controller_select_history(
            node, selected, sizeof(selected), false);
        const char *value = node == NULL ? NULL
            : document_control_value(node, &value_length);
        if (option_count != 0) {
            value = selected;
            value_length = option_count;
        }
        if (value == NULL || (!option_count && value_length >= 128u) || index > UINT16_MAX)
            continue;
        NavigationHistoryControl *saved =
            &entry->controls[entry->control_count++];
        saved->control_index = (uint16_t) index;
        saved->value_length = (uint16_t) value_length | (option_count ? NAVIGATION_HISTORY_CONTROL_OPTIONS : 0u);
        if (value_length != 0u)
            memcpy(saved->value, value, value_length);
        if (value_length < sizeof(saved->value)) saved->value[value_length] = '\0';
    }
}

static bool browser_engine_restore_controls(
    NavigationSession *navigation,
    const NavigationHistoryControl *controls, size_t count)
{
    if (navigation == NULL || controls == NULL) return false;
    bool selected = false;
    if (count > NAVIGATION_HISTORY_CONTROL_LIMIT)
        count = NAVIGATION_HISTORY_CONTROL_LIMIT;
    bool disclosed = false;
    lxb_dom_node_t *root = lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *node = root;
    unsigned ordinal = 0u;
    const NavigationHistoryControl *disclosures = NULL;
    for (size_t at = 0; at < count; at++)
        if (controls[at].value_length == NAVIGATION_HISTORY_CONTROL_DISCLOSURES) disclosures = &controls[at];
    unsigned recorded = disclosures == NULL ? 0u : disclosures->control_index;
    if (recorded > 8u * sizeof(disclosures->value))
        recorded = 8u * sizeof(disclosures->value);
    for (unsigned visited = 0u; ordinal < recorded && node != NULL && visited < BROWSER_ENGINE_DOM_WALK_LIMIT;
         visited++, node = browser_engine_history_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_DETAILS) continue;
        bool wanted = (((unsigned char) disclosures->value[ordinal / 8u]
                        >> (ordinal % 8u)) & 1u) != 0u;
        ordinal++;
        bool open = lxb_dom_element_has_attribute(lxb_dom_interface_element(node),
            (const lxb_char_t *) "open", 4u);
        if (open == wanted) continue;
        if (wanted) {
            if (lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
                    (const lxb_char_t *) "open", 4u,
                    (const lxb_char_t *) "", 0u) == NULL) return false;
        } else if (lxb_dom_element_remove_attribute(lxb_dom_interface_element(node),
                (const lxb_char_t *) "open", 4u) != LXB_STATUS_OK) return false;
        disclosed = true;
    }
    if (disclosed) {
        layout_reuse_cache_reset(navigation->page.layout_reuse);
        if (!document_refresh(&navigation->page.document)
            || !navigation_relayout(navigation)) return false;
    }
    for (size_t at = 0; at < count; at++) {
        const NavigationHistoryControl *saved = &controls[at];
        if (saved->value_length == NAVIGATION_HISTORY_CONTROL_DISCLOSURES) continue;
        if (saved->control_index >= navigation->page.layout.control_count)
            continue;
        lxb_dom_node_t *node = navigation->page.layout.controls[
            saved->control_index].node;
        if (node != NULL) {
            if (saved->value_length & NAVIGATION_HISTORY_CONTROL_OPTIONS) {
                char state[128];
                memcpy(state, saved->value, sizeof(state));
                size_t restored = controller_select_history(node, state,
                    saved->value_length & NAVIGATION_HISTORY_CONTROL_LENGTH_MASK, true);
                if (restored == SIZE_MAX) return false;
                selected |= restored != 0;
            } else if (saved->value_length < sizeof(saved->value))
                (void) document_control_value_set(
                &navigation->page.document, node, saved->value,
                saved->value_length);
        }
    }
    /* Select labels are layout commands, unlike live editable text. Rebuild
       before candidate paint, never publish a label from the old selection. */
    if (!selected) return true;
    layout_reuse_cache_reset(navigation->page.layout_reuse);
    return document_refresh(&navigation->page.document)
        && navigation_relayout(navigation);
}

static bool browser_engine_settle_same_document_events(
    BrowserEngine *engine)
{
    if (engine != NULL
        && navigation_settle_same_document_events(&engine->navigation)) {
        return true;
    }
    /* A destructive author mutation whose replacement layout was refused
       retires the page graph. Drop tile/controller state immediately so no
       frontend call can observe its borrowed pointers after this boundary. */
    browser_engine_reset_shell(engine);
    return false;
}

static bool browser_engine_commit_same_document_action(
    BrowserEngine *engine, const char *url)
{
    /* Finish earlier input before publishing the native history entry. */
    if (!browser_engine_finish_input(engine, true)) return false;
    if (!navigation_commit_same_document_url(&engine->navigation, url)) {
        return false;
    }
    /* hashchange has now observed the new URL, so native history is
       authoritative even if its bounded transactional relayout is refused.
       Settle successful mutations before resolving the fragment against the
       retained layout and before publishing paint damage. */
    if (!browser_engine_settle_same_document_events(engine)) {
        return set_error_code(
            engine, TILEFINCH_SUBSYSTEM_RENDER,
            TILEFINCH_DIAGNOSTIC_RENDER_FAILED,
            "same-document-settle",
            "same-document event layout could not be settled");
    }
    if (!navigation_scroll_to_fragment(&engine->navigation, url)) return false;
    /* This entry is already authoritative. Retain its logical focus even if
       the following render-damage publication is refused. */
    browser_engine_store_current_focus(engine);
    if (!browser_engine_finish_input(engine, true)) return false;
    clear_error(engine);
    return true;
}

static void browser_engine_rollback_same_document_cursor(
    BrowserEngine *engine, bool forward)
{
    /* Used only when restore preflight fails before popstate/hashchange is
       dispatched. Once page script observes a history move, issuing inverse
       callbacks cannot reconstruct its state (or pruned forward entries). */
    const NavigationEntry *ignored = NULL;
    (void) (forward ? navigation_back(&engine->navigation, &ignored)
                    : navigation_forward(&engine->navigation, &ignored));
}

static lxb_dom_node_t *browser_engine_attribute_owner(
    lxb_dom_node_t *node, const lxb_char_t *name, size_t name_length)
{
    /* Inline layout can associate a link region with the nested image/span
       that produced its box rather than with the authored anchor. Walk only
       the small inline ancestry needed to recover attributes on that anchor;
       never turn autofocus/provider inspection into a document traversal. */
    for (unsigned depth = 0; node != NULL && depth < 8u;
         depth++, node = node->parent) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node), name, name_length)) {
            return node;
        }
    }
    return NULL;
}

static lxb_dom_node_t *browser_engine_autofocus_target(
    const BrowserEngine *engine)
{
    if (engine == NULL) return NULL;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t at = 0; at < layout->link_count; at++) {
        lxb_dom_node_t *node = layout->links[at].node;
        lxb_dom_node_t *owner = browser_engine_attribute_owner(
            node, (const lxb_char_t *) "autofocus", 9u);
        if (owner != NULL) return owner;
    }
    for (size_t at = 0; at < layout->control_count; at++) {
        lxb_dom_node_t *node = layout->controls[at].node;
        lxb_dom_node_t *owner = browser_engine_attribute_owner(
            node, (const lxb_char_t *) "autofocus", 9u);
        if (owner != NULL) return owner;
    }
    return NULL;
}

static bool browser_engine_apply_autofocus(BrowserEngine *engine)
{
    lxb_dom_node_t *target = browser_engine_autofocus_target(engine);
    if (target == NULL) return false;
    return controller_focus_node(&engine->controller, target);
}

static void browser_engine_apply_or_defer_autofocus(BrowserEngine *engine)
{
    lxb_dom_node_t *target = browser_engine_autofocus_target(engine);
    if (target != NULL)
        (void) controller_focus_node(&engine->controller, target);
    /* The commit layout can precede the first bounded font/resource relayout.
       Keep one obligation through idle settling even when the initial target
       is not represented by a focus region yet. This is not standing work:
       the retry clears itself once the first settled layout has been
       inspected. */
    engine->autofocus_pending =
        engine->navigation.page.document.autofocus_attribute_present;
}

static void browser_engine_apply_initial_navigation_view(
    BrowserEngine *engine, bool allow_autofocus)
{
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    if (entry != NULL && strchr(entry->url, '#') != NULL) {
        /* A cross-document fragment owns the initial viewport just as a
           same-document fragment does. Do not let deferred autofocus on a
           header control scroll it back to the top after resources settle.
           History restoration and explicit preview scrolling are handled
           by the caller and retain their existing precedence. */
        engine->autofocus_pending = false;
        (void) navigation_scroll_to_fragment(&engine->navigation, entry->url);
    } else if (allow_autofocus) {
        browser_engine_apply_or_defer_autofocus(engine);
    }
}

static bool browser_engine_retry_deferred_autofocus(
    BrowserEngine *engine, bool layout_still_settling)
{
    if (engine == NULL || !engine->autofocus_pending
        || browser_engine_navigation_pending(engine)) return false;
    lxb_dom_node_t *target = browser_engine_autofocus_target(engine);
    if (target == NULL) {
        if (!layout_still_settling)
            engine->autofocus_pending = false;
        return false;
    }
    if (!controller_focus_node(&engine->controller, target)) return false;
    engine->autofocus_pending = false;
    return true;
}
