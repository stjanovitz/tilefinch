/* Incremental style sources: collects inserted <style>, stylesheet <link>
   and adopted sheets and appends them to the live stylesheet. */

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

static bool navigation_style_candidate_add(
    lxb_dom_node_t **candidates, size_t *count, size_t capacity,
    lxb_dom_node_t *node)
{
    for (size_t i = 0; i < *count; i++) {
        if (candidates[i] == node) return true;
    }
    if (*count == capacity) return false;
    candidates[(*count)++] = node;
    return true;
}

static bool navigation_is_style_source_candidate(lxb_dom_node_t *node)
{
    return node_name_is(node, "style") || navigation_is_stylesheet_link(node)
        || navigation_is_style_preload(node);
}

/* A connected subtree's <style> elements and stylesheet or style-preload
   <link> elements. Returns false when the subtree is too large to inspect,
   holds a <base> (every URL would resolve anew) or holds a source the sheet
   already ingested: a subtree the journal records only as moved may have
   been edited while it was detached. */
static bool navigation_collect_subtree_sources(
    const Stylesheet *sheet, lxb_dom_node_t *root,
    lxb_dom_node_t **candidates, size_t *count, size_t capacity)
{
    enum { MAXIMUM_SUBTREE_WORK = 256 };
    size_t work = 0;
    for (lxb_dom_node_t *at = root; at != NULL;
         at = navigation_document_order_next(at, root)) {
        if (++work > MAXIMUM_SUBTREE_WORK) return false;
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (node_name_is(at, "base")) return false;
        if (!navigation_is_style_source_candidate(at)) continue;
        if (stylesheet_style_source_known(sheet, at)
            || !navigation_style_candidate_add(
                   candidates, count, capacity, at)) return false;
    }
    return true;
}

/* Whether the sheet a link names already has its rules applied: the ordered
   loaders apply one URL once, at its first reference, so a new or changed
   reference to it is not a plain insertion. Known sources are live here
   (a freed one leaves the sheet's sources bounded out). */
static bool navigation_link_url_applied(NavigationSession *session,
                                        lxb_dom_node_t *link)
{
    size_t href_length = 0;
    const char *href = document_attribute(link, "href", &href_length);
    if (href == NULL) return false;
    if (stylesheet_document_resources_link_applied(
            &session->page.stylesheet_resources,
            session->page.resource_base_url, href, href_length)) return true;
    const Stylesheet *sheet = &session->page.stylesheet;
    for (size_t i = 0; i < sheet->style_source_count; i++) {
        lxb_dom_node_t *source =
            (lxb_dom_node_t *) sheet->style_source_nodes[i];
        if (source == link || !node_name_is(source, "link")) continue;
        size_t length = 0;
        const char *other = document_attribute(source, "href", &length);
        if (other != NULL && length == href_length
            && memcmp(other, href, length) == 0) return true;
    }
    return false;
}

static const char *navigation_page_document_url(NavigationSession *session)
{
    if (session->page.document_url[0] != '\0')
        return session->page.document_url;
    const NavigationEntry *entry = navigation_current(session);
    return entry == NULL ? NULL : entry->url;
}

typedef struct {
    NavigationSession *session;
    lxb_dom_node_t *link;
} NavigationLinkAppend;

static bool navigation_append_link_source(Stylesheet *sheet, void *opaque)
{
    NavigationLinkAppend *input = opaque;
    NavigationSession *session = input->session;
    uint64_t started = navigation_now_us();
    bool loaded = stylesheets_append_ordered_suffix_with_context(
        sheet, session->budget, &input->link, 1,
        session->page.resource_base_url,
        navigation_page_document_url(session),
        session->page.referrer_policy,
        &session->page.document.content_security_policy,
        session->maximum_stylesheets, session->maximum_stylesheet_bytes,
        session->maximum_stylesheet_file_bytes, session->resource_timeout_ms,
        session->page.resource_scheduler, session->browser_session,
        &session->page.stylesheet_resources,
        &session->page.external_stylesheets);
    session->performance.style_append_load_us += navigation_now_us() - started;
    return loaded;
}

typedef struct {
    const PocDocument *document;
    lxb_dom_node_t *sheet;
    uint32_t revision;
} NavigationAdoptedAppend;

static bool navigation_append_adopted_source(Stylesheet *sheet, void *opaque)
{
    const NavigationAdoptedAppend *input = opaque;
    return stylesheet_add_adopted_sheet(sheet, input->document, input->sheet,
                                        input->revision);
}

static bool navigation_settle_style_preloads(
    NavigationSession *session, lxb_dom_node_t *const *nodes, size_t count)
{
    return count == 0 || stylesheets_settle_style_preloads_with_context(
        &session->page.stylesheet, session->budget, nodes, count,
        session->page.resource_base_url,
        navigation_page_document_url(session),
        session->page.referrer_policy,
        &session->page.document.content_security_policy,
        session->maximum_stylesheets, session->maximum_stylesheet_bytes,
        session->maximum_stylesheet_file_bytes, session->resource_timeout_ms,
        session->page.resource_scheduler, session->browser_session,
        &session->page.stylesheet_resources,
        &session->page.external_stylesheets);
}

/* A page's late initialization commonly inserts small <style> elements one
   after another, and switches a preloaded sheet's <link> to a stylesheet.
   Rebuilding the whole sheet for each re-parses and re-fetches every
   source, reloads every image and costs seconds on the device. When the
   journal proves the only stylesheet change is sources the sheet has not
   ingested (new <style> blocks, new or newly-stylesheet <link>s, style
   preloads), insert their rules at their document position in place and
   keep the retained matched-rule lists for elements the new rules cannot
   select. Every source the sheet already holds must still be connected, in
   the same order, and unchanged; image resources the journal cannot account
   for keep the full rebuild. */
static NavigationUserCssTransaction *navigation_apply_presentation_css_begin(
    NavigationSession *session, const char *css, size_t length,
    bool allow_author_fallback);

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

/* `images_rebuilt`: this relayout re-derives the whole image table anyway,
   so image elements the journal cannot account for are not a reason to
   refuse. */
static bool navigation_try_append_style_sources(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool images_rebuilt, bool *images_affected)
{
    enum { CANDIDATE_LIMIT = 8, ORDER_WALK_LIMIT = 16384 };
    *images_affected = false;
    uint64_t walk_started = navigation_now_us();
    if (session == NULL || mutations == NULL || mutations->overflowed
        || mutations->retirement_unobserved || mutations->count == 0
        || (mutations->image_rebuild_required && !images_rebuilt))
        return navigation_style_append_refused("journal");
    Stylesheet *sheet = &session->page.stylesheet;
    if (sheet->style_sources_bounded_out || sheet->count == 0)
        return navigation_style_append_refused("sources unknown");
    const lxb_dom_node_t *previous_adopted[64];
    size_t previous_count = sheet->scopes.adopted_source_count;
    bool previous_known = previous_count <= 64
        && previous_count <= sheet->style_source_count;
    if (previous_known)
        memcpy(previous_adopted,
            sheet->style_source_nodes + sheet->style_source_count - previous_count,
            previous_count * sizeof(previous_adopted[0]));
    lxb_dom_node_t *candidates[CANDIDATE_LIMIT];
    size_t count = 0;
    /* Whether every record describes its change exactly. A removal, a move,
       an innerHTML replacement or an unknown change may also have taken
       image elements away, which only the fingerprint can rule out. */
    bool precise = true;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        lxb_dom_node_t *node = record->node;
        if (record->kind == SCRIPT_MUTATION_HEAD_SCRIPT
            || record->kind == SCRIPT_MUTATION_CANVAS) continue;
        if (node == NULL) {
            /* Freed since. A freed source bounded the sheet's sources out
               (navigation_retire_subtree); what it left must not be one. */
            precise = false;
            if (record->scope != NULL
                && (navigation_style_owner(record->scope) != NULL
                    || stylesheet_style_source_known(sheet, record->scope)))
                return navigation_style_append_refused("source changed");
            continue;
        }
        if (node_name_is(node, "base"))
            return navigation_style_append_refused("base");
        lxb_dom_node_t *owner = navigation_style_owner(node);
        if (owner != NULL) {
            /* Content or attributes of an already ingested <style>. */
            if (stylesheet_style_source_known(sheet, owner))
                return navigation_style_append_refused("known style changed");
            if (!navigation_node_connected(owner)) {
                precise = false;
                continue;
            }
            if (!navigation_style_candidate_add(
                    candidates, &count, CANDIDATE_LIMIT, owner))
                return navigation_style_append_refused("candidates");
            continue;
        }
        if (node_name_is(node, "link")) {
            /* A link whose sheet is applied cannot change or leave without
               taking its rules along. Any other link holds no rule. */
            if (stylesheet_style_source_known(sheet, node)
                || navigation_link_url_applied(session, node))
                return navigation_style_append_refused("applied link changed");
            if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
                && record->kind != SCRIPT_MUTATION_INLINE_STYLE
                && !(record->kind == SCRIPT_MUTATION_CHILD_LIST
                     && record->inserted_from_detached)) precise = false;
            if (navigation_node_connected(node)
                && (navigation_is_stylesheet_link(node)
                    || navigation_is_style_preload(node))
                && !navigation_style_candidate_add(
                       candidates, &count, CANDIDATE_LIMIT, node))
                return navigation_style_append_refused("candidates");
            continue;
        }
        switch (record->kind) {
        case SCRIPT_MUTATION_CHILD_LIST:
        case SCRIPT_MUTATION_INNER_HTML:
            if (record->kind != SCRIPT_MUTATION_CHILD_LIST
                || !record->inserted_from_detached) precise = false;
            /* A removal leaves no candidate; the document walk below proves
               no ingested source went with it. */
            if (!navigation_node_connected(node)) continue;
            if (!navigation_collect_subtree_sources(
                    sheet, node, candidates, &count, CANDIDATE_LIMIT))
                return navigation_style_append_refused("inserted subtree");
            break;
        case SCRIPT_MUTATION_ATTRIBUTE:
        case SCRIPT_MUTATION_INLINE_STYLE:
        case SCRIPT_MUTATION_TEXT:
            break;
        case SCRIPT_MUTATION_UNKNOWN:
            precise = false;
            break;
        default:
            return navigation_style_append_refused("record kind");
        }
    }
    const char *document_url = navigation_page_document_url(session);
    bool fetch_links = session->external_resources_enabled
        && document_url != NULL && session->page.resource_scheduler != NULL
        && session->page.resource_base_url[0] != '\0'
        && session->resource_timeout_ms > 0;
    /* Cascade order is document order. One walk proves the ingested sources
       are all still connected and in their recorded order, and positions
       every candidate among them. */
    lxb_dom_node_t *root = lxb_dom_interface_node(session->page.document.html);
    lxb_dom_node_t *ordered[CANDIDATE_LIMIT];
    size_t after[CANDIDATE_LIMIT];
    size_t found = 0, seen = 0, steps = 0;
    for (lxb_dom_node_t *at = root; at != NULL;
         at = navigation_document_order_next(at, root)) {
        if (++steps > ORDER_WALK_LIMIT)
            return navigation_style_append_refused("document order");
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT
            || (at->local_name != LXB_TAG_STYLE
                && at->local_name != LXB_TAG_LINK)) continue;
        if (seen < sheet->style_source_count
            && sheet->style_source_nodes[seen] == at) {
            seen++;
            continue;
        }
        if (stylesheet_style_source_known(sheet, at))
            return navigation_style_append_refused("source order");
        for (size_t k = 0; k < count; k++) {
            if (candidates[k] != at) continue;
            ordered[found] = at;
            after[found] = seen;
            found++;
            break;
        }
    }
    if (found != count)
        return navigation_style_append_refused("candidate order");
    /* Adopted constructed sheets follow every document source, in the
       registry's order. The ones the sheet holds must be unchanged (same
       sheets, relative order and text revisions). Newly adopted sources may
       be inserted between them, using the same tracked order remap as an
       ordinary style insertion. Adoption by one more root of a sheet already
       held is no change at all. */
    lxb_dom_node_t *tier[DOCUMENT_ADOPTED_TIER_LIMIT];
    uint32_t tier_revisions[DOCUMENT_ADOPTED_TIER_LIMIT];
    uint32_t candidate_revisions[CANDIDATE_LIMIT];
    size_t tier_count = document_adopted_sheets_active(
        &session->page.document, tier, tier_revisions,
        DOCUMENT_ADOPTED_TIER_LIMIT);
    if (tier_count == SIZE_MAX)
        return navigation_style_append_refused("adopted bound");
    size_t first_adopted = found, known_adopted = 0;
    uint64_t known_adopted_hash = 0;
    for (size_t i = 0; i < tier_count; i++) {
        if (seen < sheet->style_source_count
            && sheet->style_source_nodes[seen] == tier[i]) {
            known_adopted_hash = document_adopted_tier_hash(
                known_adopted_hash, tier[i], tier_revisions[i]);
            known_adopted++;
            seen++;
            continue;
        }
        if (stylesheet_style_source_known(sheet, tier[i]))
            return navigation_style_append_refused("adopted order");
        if (found == CANDIDATE_LIMIT)
            return navigation_style_append_refused("candidates");
        ordered[found] = tier[i];
        after[found] = seen;
        candidate_revisions[found] = tier_revisions[i];
        found++;
    }
    if (known_adopted != sheet->scopes.adopted_source_count
        || known_adopted_hash != sheet->scopes.adopted_sources_signature)
        return navigation_style_append_refused("adopted changed");
    if (seen != sheet->style_source_count)
        return navigation_style_append_refused("source removed");
    /* A document source past the table's bound leaves every adopted source
       without a bit, so shadow-only rules would reach the whole document.
       The rebuild orders document sources first and declines the adopted
       sources that no longer fit instead. */
    if (sheet->scopes.adopted_source_count != 0
        && first_adopted > STYLE_SOURCE_NODE_LIMIT - sheet->style_source_count)
        return navigation_style_append_refused("source bound");
    /* The journal describes image changes exactly, or they are ruled out
       here: otherwise the rebuild must re-derive the image table. */
    uint64_t fingerprint = 0;
    if (!navigation_resource_fingerprint(
            session, &session->page.document, &fingerprint))
        return navigation_style_append_refused("fingerprint");
    if (!precise && !images_rebuilt
        && navigation_fingerprint_part_changed(session, 1))
        return navigation_style_append_refused("images unaccounted");
    session->performance.style_append_walk_us +=
        navigation_now_us() - walk_started;
    lxb_dom_node_t *preloads[CANDIDATE_LIMIT];
    size_t preload_count = 0;
    bool lists_valid = true, context_changed = false, links = false;
    bool discovery = false;
    size_t appended_total = 0, appended_sources = 0;
    for (size_t k = 0; k < found; k++) {
        lxb_dom_node_t *node = ordered[k];
        bool style = node_name_is(node, "style");
        if (!style) {
            links = true;
            /* Without external resources a full rebuild ignores links. */
            if (!fetch_links) continue;
            if (!navigation_is_stylesheet_link(node)) {
                preloads[preload_count++] = node;
                continue;
            }
        }
        /* Earlier candidates that became sources precede this one. */
        size_t position = after[k];
        for (size_t j = 0; j < k; j++) {
            if (stylesheet_style_source_known(sheet, ordered[j])) position++;
        }
        bool adopted = k >= first_adopted;
        StylesheetAppendResult step;
        NavigationLinkAppend link = {session, node};
        NavigationAdoptedAppend adoption = {
            &session->page.document, node,
            adopted ? candidate_revisions[k] : 0
        };
        uint64_t load_before = session->performance.style_append_load_us;
        uint64_t step_started = navigation_now_us();
        /* A constructed sheet goes through the builders' own path
           (stylesheet_add_adopted_sheet): exempt from style-src, recorded
           as an adopted source, its parsed form cached. */
        bool appended = adopted
            ? stylesheet_append_sources_tracked(
                  sheet, navigation_append_adopted_source, &adoption,
                  position, &step)
            : style
            ? stylesheet_append_style_elements_tracked(
                  sheet, &ordered[k], 1,
                  &session->page.document.content_security_policy,
                  position, &step)
            : stylesheet_append_sources_tracked(
                  sheet, navigation_append_link_source, &link, position,
                  &step);
        if (!appended) {
            session->performance.mutation_style_append_fallbacks++;
            return false;
        }
        /* A parse-context move changes declaration values, which the note
           below already discards; the exact selector answers survive it. */
        bool step_valid = step.remap != NULL && !step.appended_bounded_out;
        uint64_t note_started = navigation_now_us();
        uint64_t loaded = session->performance.style_append_load_us
            - load_before;
        uint64_t tracked = note_started - step_started;
        session->performance.style_append_track_us +=
            tracked > loaded ? tracked - loaded : 0;
        layout_reuse_cache_note_stylesheet_appended(
            session->page.layout_reuse, sheet, step.remap, step.old_count,
            step.appended, step.appended_count, step_valid);
        uint64_t discovery_started = navigation_now_us();
        session->performance.style_append_note_us +=
            discovery_started - note_started;
        lists_valid = lists_valid && step_valid;
        discovery = discovery || step.appended_bounded_out
            || stylesheet_rules_affect_discovery(
                   sheet, step.appended, step.appended_count);
        session->performance.style_append_discovery_us +=
            navigation_now_us() - discovery_started;
        context_changed = context_changed || step.context_changed;
        appended_total += step.appended_count;
        appended_sources++;
        stylesheet_append_result_release(sheet, &step);
    }
    if (!navigation_settle_style_preloads(session, preloads, preload_count)) {
        session->performance.mutation_style_append_fallbacks++;
        return false;
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-style-append: sources=%zu preloads=%zu "
                 "rules=%zu/%zu lists=%d context-changed=%d",
                 appended_sources, preload_count, appended_total,
                 sheet->count, (int) lists_valid, (int) context_changed);
        tilefinch_platform_log_message(message);
    }
#endif
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL)
        fprintf(stderr, "style-append sources=%zu preloads=%zu rules=%zu/%zu "
                "lists=%d context-changed=%d precise=%d\n", appended_sources,
                preload_count, appended_total, sheet->count, (int) lists_valid,
                (int) context_changed, (int) precise);
#endif
    /* Appending parses at the tail before the tracked insertion remaps it.
       Recompute the signature in final source order rather than keeping the
       parser's append-order hash when new adopted sources were interleaved.
       Optional sources refused by the source ceiling contribute no rules. */
    sheet->scopes.adopted_sources_signature = 0;
    for (size_t i = 0; i < tier_count; i++) {
        if (stylesheet_style_source_known(sheet, tier[i]))
            sheet->scopes.adopted_sources_signature = document_adopted_tier_hash(
                sheet->scopes.adopted_sources_signature, tier[i], tier_revisions[i]);
    }
    /* Which elements adopted rules reach follows the adoption lists: a
       root that began or stopped adopting a held sheet restyles its own
       subtree; a sheet gaining or losing document scope, everything. */
    const lxb_dom_node_t *rescoped[DOCUMENT_ADOPTION_ROOT_LIMIT];
    size_t rescoped_count = 0;
    bool rescoped_everything = false;
    if (!stylesheet_set_appended_adopted_scopes(
            sheet, &session->page.document,
            previous_known ? previous_adopted : NULL, previous_count, rescoped,
            DOCUMENT_ADOPTION_ROOT_LIMIT, &rescoped_count,
            &rescoped_everything)) rescoped_everything = true;
    if (session->page.layout_reuse != NULL) {
        if (rescoped_everything) {
            layout_reuse_cache_reset(session->page.layout_reuse);
        } else {
            /* :host and ::slotted can reach outside the carrier itself.
               Include its host and light children, not just the shadow tree. */
            for (size_t i = 0; i < rescoped_count; i++)
                layout_reuse_cache_invalidate_tree(
                    session->page.layout_reuse,
                    (lxb_dom_node_t *) (rescoped[i]->parent != NULL
                        ? rescoped[i]->parent : rescoped[i]), false);
        }
    }
    session->page.compiled_stylesheet_cacheable = false;
    if (session->page.runtime != NULL) {
        script_runtime_set_stylesheet(session->page.runtime, sheet);
    }
    session->resource_fingerprint = fingerprint;
    navigation_note_committed_fingerprint(session);
    /* A link completes with a load or error event after this relayout. */
    if (links) session->page.stylesheet_events_swept = false;
    /* New rules can give existing elements an image or reveal one, as the
       rebuild's image pass would find; new @font-face rules need loading. */
    *images_affected = discovery;
    if (context_changed && session->web_fonts_enabled && document_url != NULL
        && !navigation_load_web_fonts(session, document_url))
        session->performance.mutation_style_append_fallbacks++;
    session->performance.mutation_style_appends++;
    return true;
}

static bool navigation_rescope_unchanged_sources(NavigationSession *session)
{
    Stylesheet *sheet = &session->page.stylesheet;
    const ExternalStylesheetStats *external = &session->page.external_stylesheets;
    if (sheet->style_sources_bounded_out || sheet->scopes.adopted_scopes_failed
        || external->skipped_pressure != 0 || external->skipped_limit != 0
        || external->failed != 0 || external->truncated != 0
        || external->deadline_cancelled != 0) return false;
    lxb_dom_node_t *sources[DOCUMENT_ADOPTED_TIER_LIMIT];
    uint32_t revisions[DOCUMENT_ADOPTED_TIER_LIMIT];
    size_t count = document_adopted_sheets_active(&session->page.document,
        sources, revisions, DOCUMENT_ADOPTED_TIER_LIMIT);
    if (count == SIZE_MAX || count != sheet->scopes.adopted_source_count) return false;
    uint64_t signature = 0;
    for (size_t i = 0; i < count; i++)
        signature = document_adopted_tier_hash(signature, sources[i], revisions[i]);
    if (signature != sheet->scopes.adopted_sources_signature) return false;
    bool global = false;
    const lxb_dom_node_t *changed[DOCUMENT_ADOPTION_ROOT_LIMIT];
    size_t changed_count = 0;
    if (!stylesheet_set_adopted_scopes(sheet, &session->page.document,
            changed, DOCUMENT_ADOPTION_ROOT_LIMIT, &changed_count, &global)) return false;
    if (global || changed_count != 0)
        layout_reuse_cache_reset(session->page.layout_reuse);
    return true;
}
