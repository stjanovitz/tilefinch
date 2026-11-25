/* Geometry read proofs: a script's forced geometry read that the
   journal's unapplied writes cannot have changed reads the layout it
   already has (navigation_prepare_geometry_reuse). See
   docs/engineering/STYLE_INVALIDATION.md, "Geometry read proofs". */

/* An attribute layout never reads from the DOM: only through computed
   style, which the cache's rows compare. Not id (an inline element with
   one keeps a box of its own), nor any name an image, control, table or
   link reads. */
static bool navigation_proof_attribute_styled_only(const char *name)
{
    return strcmp(name, "class") == 0 || strcmp(name, "style") == 0
        || (strncmp(name, "data-", 5u) == 0
            && strncmp(name, "data-tilefinch", 14u) != 0
            && strcmp(name, "data-placeholder") != 0)
        || strncmp(name, "aria-", 5u) == 0;
}

/* Whether record `record` (an attribute or inline-style write to a
   rendered element) can have changed the geometry only through computed
   styles: then the rows decide. Attribute selectors must be confined to
   the element's own style, and attr() reading it to a style a row checks
   (the element's own, when the write dropped its entry). */
static const char *navigation_proof_styled_refusal(
    const Stylesheet *sheet, const LayoutReuseCache *cache,
    const ScriptMutationRecord *record)
{
    lxb_dom_node_t *node = record->node;
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->ns != LXB_NS_HTML || !navigation_node_connected(node))
        return "styled-node";
    const char *name = record->kind == SCRIPT_MUTATION_INLINE_STYLE
        ? "style" : record->attribute;
    size_t length = strlen(name);
    if (length >= SCRIPT_MUTATION_ATTRIBUTE_LIMIT - 1u
        || !navigation_proof_attribute_styled_only(name))
        return "styled-name";
    if (!layout_reuse_proof_checks(cache, node)
        && stylesheet_declarations_read_attribute(sheet, name, length))
        return "styled-attr";
    if (stylesheet_selectors_reference_attribute_prefix(sheet, name, length)
        && !stylesheet_attribute_selectors_subject_only(sheet, name, length))
        return "styled-selector";
    return NULL;
}

static bool navigation_geometry_proof_disabled(void)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_GEOMETRY_PROOF") != NULL) return true;
#endif
    return false;
}

typedef struct {
    bool open;
    LayoutReuseStats before;
    /* Records the reading layout has not seen (reuse_applied clear when
       the read began). */
    uint64_t unapplied[(SCRIPT_MUTATION_JOURNAL_LIMIT + 63u) / 64u];
    const char *refusal;
} NavigationGeometryProof;

static void navigation_geometry_proof_trace(const NavigationGeometryProof *proof,
                                            const ScriptMutationJournal *mutations)
{
#ifndef TILEFINCH_NO_TRACE
    static int trace = -1;
    if (trace < 0) trace = getenv("TILEFINCH_TRACE_GEOMETRY_PROOF") != NULL;
    if (trace)
        fprintf(stderr, "geometry-proof %s records=%zu\n",
                proof->refusal == NULL ? "holds" : proof->refusal,
                mutations == NULL ? (size_t) 0 : mutations->count);
#else
    (void) proof;
    (void) mutations;
#endif
}

/* Before the journal is applied: whether a proof can start, and which
   records it must answer for. */
static void navigation_geometry_proof_begin(
    NavigationSession *session, ScriptMutationJournal *mutations,
    const LayoutDocument *reading, NavigationGeometryProof *proof)
{
    memset(proof, 0, sizeof(*proof));
    const Stylesheet *sheet = &session->page.stylesheet;
    if (navigation_geometry_proof_disabled()) {
        proof->refusal = "disabled";
        return;
    }
    if (!session->page.loaded || session->style_rebuild_required
        || session->page.visual_image_publication_pending
        || navigation_layout_provisional(session)
        || session->page.layout_reuse == NULL) {
        proof->refusal = "page";
        return;
    }
    /* No journal: the document moved without a record (a refresh), or its
       changes went unrecorded. */
    if (mutations == NULL) {
        proof->refusal = "no-journal";
        return;
    }
    /* A journal that audits every record, and no stylesheet or image
       source that must be rebuilt. The resource discovery other writes may
       need (an image refresh or scan, the conservative scan after
       innerHTML) is the next relayout's: a build now would read the same
       sheet and image table. */
    proof->refusal = mutations->overflowed ? "overflowed"
        : mutations->retirement_unobserved ? "retirement-unobserved"
        : mutations->resource_rebuild_required ? "resource-rebuild" : NULL;
    if (proof->refusal != NULL) return;
    if (script_runtime_has_shadow_roots(session->page.runtime)
        || stylesheet_has_container_queries(sheet)
        || style_container_layout_state_present(sheet)) {
        proof->refusal = "page-features";
        return;
    }
    size_t unapplied = 0;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->reuse_applied) continue;
        if (record->relational) {
            proof->refusal = "relational";
            return;
        }
        proof->unapplied[i / 64u] |= UINT64_C(1) << (i % 64u);
        unapplied++;
    }
    /* The document's generation moved, yet every record was already
       applied: something changed it without a record. */
    if (unapplied == 0) {
        proof->refusal = "no-record";
        return;
    }
    /* An image arrived, or changed size, since the reading layout. */
    if (reading != NULL && reading->reuse_serial != 0
        && reading->image_fingerprint
            != images_layout_fingerprint(&session->page.images)) {
        proof->refusal = "images";
        return;
    }
    if (!layout_reuse_proof_begin(session->page.layout_reuse, reading)) {
        proof->refusal = reading == NULL || reading->reuse_serial == 0
            ? "not-covered-layout" : "not-covered";
        return;
    }
    layout_reuse_cache_stats(session->page.layout_reuse, &proof->before);
    proof->open = true;
}

/* After the journal was applied to the cache: whether every record it
   must answer for is transient, renders nothing, or changed only styles,
   and every style the rows hold is as the reading layout used it. Closes
   the proof either way. */
static bool navigation_geometry_proof_finish(
    NavigationSession *session, ScriptMutationJournal *mutations,
    NavigationGeometryProof *proof)
{
    if (!proof->open) {
        navigation_geometry_proof_trace(proof, mutations);
        return false;
    }
    LayoutReuseCache *cache = session->page.layout_reuse;
    const Stylesheet *sheet = &session->page.stylesheet;
    bool head_checked = false;
    for (size_t i = 0; proof->refusal == NULL && i < mutations->count; i++) {
        if ((proof->unapplied[i / 64u] & (UINT64_C(1) << (i % 64u))) == 0)
            continue;
        const ScriptMutationRecord *record = &mutations->records[i];
        /* Content inserted and removed again before any build saw it. */
        if (navigation_mutation_is_transient(mutations, i)) {
            if (record->reuse_seen) proof->refusal = "transient-seen";
            continue;
        }
        const char *hidden = navigation_hidden_record_refusal(
            session, mutations, record, &head_checked);
        if (hidden == NULL) continue;
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            && record->kind != SCRIPT_MUTATION_INLINE_STYLE) {
            proof->refusal = hidden;
            continue;
        }
        proof->refusal = navigation_proof_styled_refusal(sheet, cache, record);
#ifndef TILEFINCH_NO_TRACE
        if (proof->refusal != NULL
            && getenv("TILEFINCH_TRACE_GEOMETRY_PROOF") != NULL)
            fprintf(stderr, "geometry-proof-record %s kind=%d attribute=%s "
                    "hidden=%s\n", proof->refusal, (int) record->kind,
                    record->attribute, hidden);
#endif
    }
    LayoutReuseStats after = {0};
    layout_reuse_cache_stats(cache, &after);
    /* A :has() answer moved, or the cache was reset: the rows do not say
       what changed. */
    if (proof->refusal == NULL
        && (after.full_resets != proof->before.full_resets
            || after.has_dropped != proof->before.has_dropped
            || after.has_fallbacks != proof->before.has_fallbacks))
        proof->refusal = "has-or-reset";
    proof->open = false;
    if (proof->refusal != NULL) {
        layout_reuse_proof_abort(cache);
        navigation_geometry_proof_trace(proof, mutations);
        return false;
    }
    LayoutReuseProofOutcome outcome =
        layout_reuse_proof_finish(cache, sheet, session->fonts);
    static const char *const outcomes[] = {
        [LAYOUT_REUSE_PROOF_HOLDS] = NULL,
        [LAYOUT_REUSE_PROOF_NOT_COVERED] = "rows-not-covered",
        [LAYOUT_REUSE_PROOF_OVERFLOW] = "rows-overflow",
        [LAYOUT_REUSE_PROOF_RESET] = "rows-reset",
        [LAYOUT_REUSE_PROOF_DIFFERS] = "rows-differ",
        [LAYOUT_REUSE_PROOF_PSEUDO] = "rows-pseudo",
        [LAYOUT_REUSE_PROOF_PATH] = "rows-path",
    };
    proof->refusal = outcomes[outcome];
    navigation_geometry_proof_trace(proof, mutations);
    return proof->refusal == NULL;
}

/* The geometry read's journal application (navigation_prepare_geometry_
   reuse), with a proof around it when the caller can use one. */
static void navigation_prepare_geometry_proof(
    NavigationSession *session, ScriptMutationJournal *mutations,
    bool *geometry_unchanged)
{
    NavigationGeometryProof proof;
    if (geometry_unchanged != NULL)
        navigation_geometry_proof_begin(
            session, mutations,
            script_runtime_geometry_reading(session->page.runtime), &proof);
    /* The normal publication still consumes these mutations later. Apply
       its exact invalidation now as well, rather than using stale styles
       or resolving the entire document anew inside a geometry accessor.
       A queued resource/style rebuild does not change this build's
       installed cascade. Its generation guards invalidate the cache when
       replacement rules actually arrive; the journal still invalidates
       changed DOM now. */
    navigation_prepare_layout_reuse(session, mutations, false, NULL);
    if (geometry_unchanged != NULL)
        *geometry_unchanged =
            navigation_geometry_proof_finish(session, mutations, &proof);
}
