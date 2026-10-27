/* Layout reuse cache: preparation around a relayout and eviction under
   memory pressure. */

static bool navigation_journal_inserted(void *context,
                                        const lxb_dom_node_t *node);

static void navigation_prepare_layout_reuse(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool full_rebuild, const bool *local_focus)
{
    if (session == NULL || session->page.layout_reuse == NULL) return;
    navigation_bind_node_retirement(session);
    bool retire = layout_reuse_cache_node_retirement(session->page.layout_reuse);
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &before);
    /* An overflowed journal still names where its unrecorded changes are,
       unless a root was lost or freed nodes went unobserved. */
    bool destructive = full_rebuild || mutations == NULL
        || (mutations->overflowed
            && (mutations->overflow_root_count == 0
                || mutations->overflow_roots_lost
                || mutations->retirement_unobserved));
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
                    "exact=%d tokens=%u detached=%d", (int) record->kind,
                    (int) name_length, name == NULL ? "" : name,
                    record->attribute, (int) record->changed_tokens_exact,
                    (unsigned) record->changed_token_count,
                    (int) record->inserted_from_detached);
            /* Where the change sits: the record's node (or the parent a
               removal left) and up to five ancestors, each with its id or
               first class and its box in the incumbent layout. */
            lxb_dom_node_t *at = record->node != NULL
                && navigation_node_connected(record->node)
                ? record->node : record->scope;
            for (int depth = 0; at != NULL && depth < 6; depth++) {
                if (at->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    size_t tag_length = 0, id_length = 0, class_length = 0;
                    const char *tag = document_element_name(at, &tag_length);
                    const char *id = document_attribute(at, "id", &id_length);
                    const char *klass = document_attribute(at, "class",
                                                           &class_length);
                    size_t first = 0;
                    while (klass != NULL && first < class_length
                           && klass[first] != ' ') first++;
                    const LayoutNodeBox *box = layout_box_for_node(
                        &session->page.layout, at);
                    fprintf(stderr, " %s%.*s#%.*s.%.*s",
                            depth == 0 ? "at=" : "<", (int) tag_length,
                            tag == NULL ? "" : tag,
                            (int) (id_length > 24 ? 24 : id_length),
                            id == NULL ? "" : id,
                            (int) (first > 32 ? 32 : first),
                            klass == NULL ? "" : klass);
                    if (box != NULL)
                        fprintf(stderr, "[%d,%d %dx%d]", box->x, box->y,
                                box->width, box->height);
                }
                at = at->parent;
            }
            fputc('\n', stderr);
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
        const bool shadow_roots =
            script_runtime_has_shadow_roots(session->page.runtime);
        for (size_t i = 0; i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT
                || record->kind == SCRIPT_MUTATION_CANVAS) continue;
            /* Content inserted and detached again within the turn left
               every remaining element's inputs as they were; retirement
               evicts it from the cache if it is freed. */
            if (navigation_mutation_is_transient(mutations, i)) continue;
            if (shadow_roots) {
                layout_reuse_cache_invalidate_shadow_composition(
                    session->page.layout_reuse, record->scope);
                if (record->node != record->scope)
                    layout_reuse_cache_invalidate_shadow_composition(
                        session->page.layout_reuse, record->node);
            }
            /* Its structural :has() notes consider only the entries the
               bridge's probe saw the writes reach. */
            layout_reuse_cache_set_structure_filter(
                session->page.layout_reuse, record->has_entries,
                mutations->has_serial_mixed ? 0 : mutations->has_serial);
            /* The parent a removed or moved child left: its remaining
               children's structural answers changed. */
            if (record->scope != NULL && record->removed_last
                && record->kind == SCRIPT_MUTATION_CHILD_LIST && retire
#ifndef TILEFINCH_NO_TRACE
                && getenv("TILEFINCH_DISABLE_APPENDED_SCOPED_REUSE") == NULL
#endif
                ) {
                layout_reuse_cache_invalidate_removed_last(
                    session->page.layout_reuse, &session->page.stylesheet,
                    record->scope, record->node, record->relational);
            } else if (record->scope != NULL) {
                layout_reuse_cache_invalidate_children(
                    session->page.layout_reuse, record->scope, record->node,
                    record->relational);
            }
            if (record->node == NULL) continue;
            /* <style> content is not rendered; an appended sheet has already
               told the cache which lists the new rules can change. */
            if (navigation_style_owner(record->node) != NULL) continue;
            /* Unobserved author bookkeeping is not a style mutation. Keep
               intrinsic/row invalidation because generated content may read
               attr(data-*), but retain the already resolved cascade. Prefix
               dependency checks remain conservative for truncated journals;
               native data-tilefinch state keeps its ordinary semantics. */
            if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && strncmp(record->attribute, "data-", 5u) == 0
                && strncmp(record->attribute, "data-tilefinch", 14u) != 0
                && !stylesheet_selectors_reference_attribute_prefix(
                    &session->page.stylesheet, record->attribute,
                    strlen(record->attribute))) {
                layout_reuse_cache_invalidate_measurements(
                    session->page.layout_reuse, record->node);
                continue;
            }
            if (record->kind == SCRIPT_MUTATION_CHILD_LIST
                || record->kind == SCRIPT_MUTATION_INNER_HTML
                || record->kind == SCRIPT_MUTATION_UNKNOWN) {
                /* A child-list record carries the bridge's classification
                   of the insertion or removal; innerHTML and unknown
                   changes are always relational. */
                if (retire && record->kind != SCRIPT_MUTATION_CHILD_LIST) {
                    /* innerHTML and unknown records name the parent whose
                       children changed. */
                    layout_reuse_cache_invalidate_children(
                        session->page.layout_reuse, record->node, NULL,
                        record->relational);
                } else if (retire && record->inserted_from_detached
                           && navigation_node_connected(record->node)
#ifndef TILEFINCH_NO_TRACE
                           && getenv("TILEFINCH_DISABLE_APPENDED_SCOPED_REUSE")
                                  == NULL
#endif
                           && !layout_insertion_reaches_siblings(
                                  session->page.layout_reuse,
                                  &session->page.stylesheet, record->node,
                                  navigation_journal_inserted,
                                  (void *) mutations)) {
                    layout_reuse_cache_invalidate_appended(
                        session->page.layout_reuse, record->node,
                        record->relational);
                } else if (retire) {
                    layout_reuse_cache_invalidate_tree(
                        session->page.layout_reuse, record->node,
                        record->relational);
                } else if (record->relational) {
                    layout_reuse_cache_invalidate_node(
                        session->page.layout_reuse, record->node, true);
                } else {
                    layout_reuse_cache_invalidate_node_scoped(
                        session->page.layout_reuse, record->node, true);
                }
                continue;
            }
            if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && strcmp(
                    record->attribute, "data-tilefinch-focus") == 0) {
                /* Receipts are stack-local to this relayout and indexed by
                   the unchanged journal. Never reuse them across turns. */
                if (local_focus != NULL
                    && i < NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT
                    && local_focus[i]) {
                    layout_reuse_cache_invalidate_local_focus(
                        session->page.layout_reuse, record->node);
                } else {
                    layout_reuse_cache_invalidate_focus(
                        session->page.layout_reuse, record->node);
                }
            } else if (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                       && record->changed_tokens_exact) {
                layout_reuse_cache_invalidate_attribute(
                    session->page.layout_reuse, record->node,
                    script_mutation_record_tokens(mutations, record),
                    record->changed_token_count, false, record->relational);
            } else if (record->kind == SCRIPT_MUTATION_INLINE_STYLE
                       || (record->kind == SCRIPT_MUTATION_ATTRIBUTE
                           && strcmp(record->attribute, "style") == 0)) {
                layout_reuse_cache_invalidate_inline_style(
                    session->page.layout_reuse, record->node,
                    record->kind == SCRIPT_MUTATION_INLINE_STYLE
                        ? record->attribute : NULL,
                    record->relational);
            } else if (record->relational) {
                /* Scoped by the record's own classification: an unrelated
                   relational write in the same turn no longer widens it.
                   An inline declaration is visible to selectors only as
                   the style attribute. */
                layout_reuse_cache_invalidate_relational(
                    session->page.layout_reuse, record->node,
                    record->kind == SCRIPT_MUTATION_INLINE_STYLE ? "style"
                    : record->kind == SCRIPT_MUTATION_ATTRIBUTE
                        ? record->attribute : NULL,
                    record->kind == SCRIPT_MUTATION_TEXT);
            } else if (record->kind == SCRIPT_MUTATION_ATTRIBUTE) {
                layout_reuse_cache_invalidate_own_attribute(
                    session->page.layout_reuse, record->node,
                    record->attribute);
            } else {
                layout_reuse_cache_invalidate_node_scoped(
                    session->page.layout_reuse, record->node,
                    record->kind == SCRIPT_MUTATION_TEXT);
            }
        }
        layout_reuse_cache_set_structure_filter(session->page.layout_reuse,
                                                UINT64_MAX, 0);
        for (size_t i = 0;
             mutations->overflowed && i < mutations->overflow_root_count; i++)
        {
            layout_reuse_cache_invalidate_overflow_root(
                session->page.layout_reuse, mutations->overflow_roots[i]);
            if (shadow_roots)
                layout_reuse_cache_invalidate_shadow_composition(
                    session->page.layout_reuse, mutations->overflow_roots[i]);
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
    /* A reset clears nothing of these; they only grow within one cache. */
    if (after.has_scans >= before.has_scans
        && after.has_dropped >= before.has_dropped) {
        session->performance.layout_reuse_has_scoped +=
            after.has_scoped - before.has_scoped;
        session->performance.layout_reuse_has_fallbacks +=
            after.has_fallbacks - before.has_fallbacks;
        session->performance.layout_reuse_has_dropped +=
            after.has_dropped - before.has_dropped;
        session->performance.layout_reuse_has_scans +=
            after.has_scans - before.has_scans;
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL)
        fprintf(stderr, "mutation-journal-reuse full-resets=%zu "
                "has-scoped=%zu has-fallbacks=%zu has-dropped=%zu "
                "has-scans=%zu\n", after.full_resets - before.full_resets,
                after.has_scoped - before.has_scoped,
                after.has_fallbacks - before.has_fallbacks,
                after.has_dropped - before.has_dropped,
                after.has_scans - before.has_scans);
#endif
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

size_t navigation_yield_layout_text_measures(NavigationSession *session,
                                             NavigationLoad *load)
{
    size_t released = 0;
    if (session != NULL)
        released += layout_reuse_cache_yield_text_measures(
            session->page.layout_reuse);
    if (load != NULL && load->document_stream != NULL)
        released += layout_reuse_cache_yield_text_measures(
            load->document_stream->preview_layout_reuse);
    return released;
}

size_t navigation_yield_style_memos(NavigationSession *session,
                                    NavigationLoad *load)
{
    size_t released = 0;
    if (session != NULL) {
        released += stylesheet_yield_inline_scans(&session->page.stylesheet);
        released += stylesheet_yield_inline_scans(
            &session->compiled_stylesheet_cache);
        for (size_t i = 0; i < session->page.frame_count
                           && i < NAVIGATION_FRAME_LIMIT; i++) {
            NavigationFramePresentation *presentation =
                session->page.frames[i].presentation;
            if (presentation != NULL)
                released += stylesheet_yield_inline_scans(
                    &presentation->stylesheet);
        }
    }
    if (load != NULL && load->document_stream != NULL)
        released += stylesheet_yield_inline_scans(
            &load->document_stream->blocking_stylesheet);
    return released;
}

static void navigation_evict_layout_reuse_under_pressure(
    NavigationSession *session)
{
    if (session == NULL || session->page.layout_reuse == NULL) return;
    if (!budget_pressure_required(
            session->budget, LAYOUT_REUSE_STYLE_WORKING_BYTES,
            LAYOUT_REUSE_STYLE_RESERVE_BYTES)) return;
    /* Selector results are much smaller than resolved ComputedStyles and
       remain valid through this pressure decision. Reclaim values first;
       destroying and immediately recreating both tables makes every update
       repeat the expensive selector walk. Ordinary invalidation still owns
       all DOM/stylesheet changes. */
    (void) layout_reuse_cache_reclaim_styles(session->page.layout_reuse);
    if (!budget_pressure_required(
            session->budget, LAYOUT_REUSE_STYLE_WORKING_BYTES,
            LAYOUT_REUSE_STYLE_RESERVE_BYTES)) return;
    layout_reuse_cache_destroy(session->page.layout_reuse);
    session->page.layout_reuse = NULL;
    session->performance.layout_reuse_pressure_evictions++;
    session->performance.layout_reuse_retained_bytes = 0;
}
