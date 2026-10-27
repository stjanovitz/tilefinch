/* Relayout: image refresh after mutations, provisional layout and layout
   completion, hidden-content classification, navigation_relayout and its
   static fallback, scroll anchoring, and the user/presentation CSS
   transaction. */

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

/* The fingerprint covers resource elements' names and resource attributes
   and <style> text. A journal of only class, id, inline-style and other
   non-resource attribute writes cannot change it, so an image refresh for
   such a turn (an inline SVG recolour) keeps the committed fingerprint
   instead of walking the whole document to re-derive it. */
static bool navigation_mutations_keep_resource_fingerprint(
    const ScriptMutationJournal *mutations)
{
    if (mutations == NULL || mutations->overflowed) return false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind == SCRIPT_MUTATION_INLINE_STYLE) continue;
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE) return false;
        for (size_t a = 0;
             a < sizeof(resource_fingerprint_attributes)
                     / sizeof(resource_fingerprint_attributes[0]); a++) {
            if (strcasecmp(record->attribute,
                           resource_fingerprint_attributes[a]) == 0)
                return false;
        }
    }
    return true;
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

/* Load and error events of stylesheet links and style preloads that
   settled outside a full rebuild (the parser's preloads, a link the style
   append lane loaded), dispatched after the relayout committed so their
   handlers' DOM changes journal for the next one. A full rebuild dispatches
   its own between loading the sheets and laying out. */
static bool navigation_settle_style_preloads(
    NavigationSession *session, lxb_dom_node_t *const *nodes, size_t count);

/* Style preloads no stylesheet pass has seen (inserted after the page's
   last one) are fetched now, as the rebuild's loader would have. */
static bool navigation_settle_unseen_style_preloads(
    NavigationSession *session)
{
    enum { SETTLE_LIMIT = 8 };
    lxb_dom_node_t *pending[SETTLE_LIMIT];
    size_t count = 0;
    const StylesheetDocumentResources *resources =
        &session->page.stylesheet_resources;
    lxb_dom_node_t *root = lxb_dom_interface_node(session->page.document.html);
    for (lxb_dom_node_t *node = root; node != NULL && count < SETTLE_LIMIT;
         node = navigation_next_document_node(root, node)) {
        if (node->local_name != LXB_TAG_LINK
            || !navigation_is_style_preload(node)
            || navigation_is_stylesheet_link(node)) continue;
        bool seen = false;
        for (size_t i = 0; !seen && i < resources->style_preload_count; i++)
            seen = resources->style_preloads[i].element == node;
        for (size_t i = 0;
             !seen && i < session->page.stylesheet_event_count; i++)
            seen = session->page.stylesheet_events[i].element == node;
        if (!seen) pending[count++] = node;
    }
    return navigation_settle_style_preloads(session, pending, count);
}

static bool navigation_sweep_stylesheet_events(NavigationSession *session)
{
    if (session->page.stylesheet_events_swept
        || session->page.runtime == NULL
        /* Completion notifications are tasks, not an extra checkpoint inside
           the input task that happened to publish their layout. Finish the
           already-bounded microtask checkpoint before dispatching them. */
        || !script_runtime_message_dispatch_ready(session->page.runtime)
        || !session->external_resources_enabled
        || session->page.resource_base_url[0] == '\0'
        || session->page.document.html == NULL) return true;
    if (session->page.resource_scheduler != NULL
        && navigation_page_document_url(session) != NULL
        && !navigation_settle_unseen_style_preloads(session)) return false;
    return navigation_dispatch_stylesheet_events(
               session, session->page.resource_base_url)
        || navigation_retire_exhausted_script_realms(session, false);
}

static size_t navigation_live_deferred_nodes(
    const NavigationPage *page, lxb_dom_node_t **nodes);

/* Relayout phase lines go to the validation log on the PSP and, on the
   host, to stderr under TILEFINCH_TRACE_MUTATION_JOURNAL. A shipping PSP
   build has neither: it takes no phase stamps and formats nothing. */
#if defined(TILEFINCH_PSP_VALIDATION_LOG) || !defined(TILEFINCH_NO_TRACE)
#define NAVIGATION_RELAYOUT_PHASE_LOG 1
static bool navigation_relayout_phase_log_wanted(void)
{
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    return true;
#else
    return getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL;
#endif
}

static void navigation_relayout_phase_log_emit(const char *line)
{
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL)
        fprintf(stderr, "%s\n", line);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    tilefinch_platform_log_message(line);
#endif
}
#else
#define NAVIGATION_RELAYOUT_PHASE_LOG 0
#endif

#if NAVIGATION_RELAYOUT_PHASE_LOG
/* A fast relayout's prepare span: the journal and style append (its walk,
   source loads, order bookkeeping, reuse note, discovery check), the
   resource decision, reuse invalidation and the image pass, with the styles
   that pass resolved (the layout build reuses them). */
static void navigation_log_prepare_phases(
    NavigationSession *session, uint64_t started, uint64_t append_started,
    const uint64_t append_before[5], uint64_t decided,
    uint64_t reuse_prepared, uint64_t images_done,
    const LayoutReuseStats *images_before,
    const LayoutReuseStats *images_after, uint64_t events_us)
{
    if (!navigation_relayout_phase_log_wanted()) return;
    /* Styles the layout build resolved after the image pass. */
    LayoutReuseStats built = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &built);
    const NavigationPerformance *counters = &session->performance;
    uint64_t append[5] = {
        counters->style_append_walk_us - append_before[0],
        counters->style_append_load_us - append_before[1],
        counters->style_append_track_us - append_before[2],
        counters->style_append_note_us - append_before[3],
        counters->style_append_discovery_us - append_before[4]
    };
    char line[384];
    snprintf(line, sizeof(line),
        "tilefinch-relayout-prepare-phases: journal=%lluus decide=%lluus "
        "append-walk=%lluus append-load=%lluus append-track=%lluus "
        "append-note=%lluus append-discovery=%lluus reuse=%lluus "
        "images=%lluus image-styles=%zu/%zu build-styles=%zu/%zu "
        "evictions=%zu events=%lluus",
        (unsigned long long) (append_started - started),
        (unsigned long long) (decided - append_started),
        (unsigned long long) append[0], (unsigned long long) append[1],
        (unsigned long long) append[2], (unsigned long long) append[3],
        (unsigned long long) append[4],
        (unsigned long long) (reuse_prepared - decided),
        (unsigned long long) (images_done - reuse_prepared),
        images_after->style_hits - images_before->style_hits,
        images_after->style_misses - images_before->style_misses,
        built.style_hits - images_after->style_hits,
        built.style_misses - images_after->style_misses,
        built.style_evictions,
        (unsigned long long) events_us);
    navigation_relayout_phase_log_emit(line);
}
#endif

/* The image pass of a full rebuild without the stylesheet passes before
   it: the sheet is unchanged, so the table it derives is the rebuild's. */
static bool navigation_rebuild_images(NavigationSession *session)
{
    const char *document_url = navigation_page_document_url(session);
    if (document_url == NULL || session->page.resource_base_url[0] == '\0')
        return false;
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t started = navigation_now_us();
#endif
    /* The job borrows this image table (see the full rebuild). */
    images_priority_load_destroy(session->page.deferred_image_job);
    session->page.deferred_image_job = NULL;
    session->page.deferred_image_batch_count = 0;
    session->page.deferred_image_publication_age = 0;
    lxb_dom_node_t *deferred[NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
    size_t deferred_count =
        navigation_live_deferred_nodes(&session->page, deferred);
    /* Retained intrinsic sizes measured against the old table. */
    layout_reuse_cache_update_images(session->page.layout_reuse, NULL);
    navigation_ensure_layout_reuse(session);
    bool loaded = images_rebuild_external_reusing_rasters(
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
        session->fonts, session->viewport.css_width, deferred,
        deferred_count);
    session->images = &session->page.images;
    layout_reuse_cache_update_images(
        session->page.layout_reuse, session->images);
    session->performance.mutation_image_rebuilds++;
#if NAVIGATION_RELAYOUT_PHASE_LOG
    if (navigation_relayout_phase_log_wanted()) {
        const ExternalImageStats *stats = &session->page.images.stats;
        char message[320];
        snprintf(message, sizeof(message),
                 "tilefinch-relayout-image-rebuild: images=%lluus loaded=%d "
                 "count=%zu fetched=%zu decoded-cache=%zu http-cache=%zu "
                 "svg=%zu/%zu svg-us=%llu/%llu traversal=%lluus "
                 "styles=%lluus drain=%lluus",
                 (unsigned long long) (navigation_now_us() - started),
                 (int) loaded, session->page.images.count, stats->loaded,
                 stats->decoded_cache_hits, stats->cache_hits,
                 stats->inline_svg_rasterized,
                 stats->inline_svg_refresh_reused,
                 (unsigned long long) stats->inline_svg_serialize_us,
                 (unsigned long long) stats->inline_svg_rasterize_us,
                 (unsigned long long) stats->traversal_us,
                 (unsigned long long) stats->style_resolve_us,
                 (unsigned long long) stats->drain_us);
        navigation_relayout_phase_log_emit(message);
    }
#endif
    return loaded;
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

/* A consumed author mutation makes the incumbent unsuitable for publication,
   but it can still own the memory that a replacement needs. On a real Budget
   refusal, give that replacement one attempt without the invalid incumbent.
   Retain only bounded numeric scroll state; never read its borrowed DOM or
   paint commands after releasing it. Synthetic faults and cancellation must
   continue to exercise the ordinary retirement path. */
static bool navigation_retry_mutation_layout_under_pressure(
    NavigationSession *session, LayoutDocument *next, bool *released)
{
    struct {
        lxb_dom_node_t *node;
        int x, y;
    } scrolls[64];
    size_t count = 0;
    for (size_t i = 0; i < session->page.layout.node_box_count; i++) {
        const LayoutNodeBox *box = &session->page.layout.node_boxes[i];
        if (box->node == NULL || (box->scroll_x == 0 && box->scroll_y == 0))
            continue;
        if (count == sizeof(scrolls) / sizeof(scrolls[0])) return false;
        scrolls[count].node = box->node;
        scrolls[count].x = box->scroll_x;
        scrolls[count++].y = box->scroll_y;
    }
    layout_destroy(&session->page.layout);
    *released = true;
    session->performance.mutation_layout_pressure_retries++;
    if (!navigation_build_layout(session, next, session->fonts,
                                 session->images)) return false;
    for (size_t i = 0; i < count; i++) {
        /* Like layout_transfer_scroll_state, this is an identity lookup in
           the replacement, not a dereference of an old node. */
        (void) layout_scroll_node(next, scrolls[i].node,
                                 scrolls[i].x, scrolls[i].y);
    }
    session->performance.mutation_layout_pressure_recoveries++;
    return true;
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

/* A page without a script realm, whose last complete layout took long
   enough to be felt, can show the screens around the reader first: nothing
   else reads its geometry in between, and idle work completes the rest. */
static bool navigation_relayout_preview_eligible(
    const NavigationSession *session)
{
    return session->page.runtime == NULL
        && session->relayout_preview_threshold_us != UINT64_MAX
        && session->page.layout_complete_us
               >= session->relayout_preview_threshold_us
        && session->viewport.css_height > 0;
}

/* The viewport and the two screens below it, so the first page-downs land
   on laid-out content, and at least minimum_limit deep. A truncated result
   keeps keep_height as its scroll extent and marks where its content ends. */
static bool navigation_build_provisional_layout_limit(
    NavigationSession *session, LayoutDocument *layout, int keep_height,
    int minimum_limit, int *limit_out)
{
    const NavigationEntry *entry = navigation_current(session);
    int scroll_y = entry == NULL || entry->scroll_y < 0 ? 0 : entry->scroll_y;
    int limit = scroll_y + 3 * session->viewport.css_height;
    if (limit < minimum_limit) limit = minimum_limit;
    if (limit_out != NULL) *limit_out = limit;
    navigation_ensure_layout_reuse(session);
    bool truncated = false;
    layout->performance.cancelled = false;
    bool built = layout_build_context_preview(
        layout, session->budget, &session->page.document,
        &session->page.stylesheet, session->fonts, session->images,
        &session->viewport, session->page.layout_reuse, limit, &truncated);
    session->layout_build_cancelled = !built && layout->performance.cancelled;
    if (!built) return false;
    if (truncated) {
        layout->content_limit_y = layout->height > 0 ? layout->height : 1;
        /* Keep the previous page's extent, and always leave two screens
           of placeholder to scroll into: reaching them extends the layout
           (a page committed provisionally has no earlier extent). */
        int ahead = layout->content_limit_y
            + 2 * session->viewport.css_height;
        if (keep_height < ahead) keep_height = ahead;
        if (layout->height < keep_height) layout->height = keep_height;
    }
    return true;
}

static bool navigation_build_provisional_layout(
    NavigationSession *session, LayoutDocument *layout, int keep_height,
    int minimum_limit)
{
    return navigation_build_provisional_layout_limit(
        session, layout, keep_height, minimum_limit, NULL);
}

/* Whether `node` comes after `anchor` in document order without lying
   inside it: traversed later than every element completed before a cut
   whose last completed element is `anchor`. Unknown answers (a detached
   node, a tree deeper than the walk) count as later. */
static bool navigation_node_follows(const lxb_dom_node_t *node,
                                    const lxb_dom_node_t *anchor)
{
    enum { DEPTH = 256 };
    const lxb_dom_node_t *chain[DEPTH];
    size_t depth = 0;
    for (const lxb_dom_node_t *at = anchor; at != NULL; at = at->parent) {
        if (depth == DEPTH) return true;
        chain[depth++] = at;
    }
    const lxb_dom_node_t *below = NULL;
    size_t steps = 0;
    for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (++steps > DEPTH) return true;
        for (size_t i = 0; i < depth; i++) {
            if (chain[i] != at) continue;
            if (i == 0) return false; /* inside anchor (or anchor itself) */
            if (below == NULL) return false; /* an ancestor of anchor */
            /* Siblings under the common ancestor: which comes first? */
            const lxb_dom_node_t *mine = chain[i - 1];
            size_t walked = 0;
            for (const lxb_dom_node_t *sibling = mine->next;
                 sibling != NULL; sibling = sibling->next) {
                if (sibling == below) return true;
                if (++walked > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) break;
            }
            return walked > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;
        }
        below = at;
    }
    return true;
}

/* Whether a bounded build completed `node` with its whole subtree: `node`
   is the cut's last completed element `anchor`, lies inside it, or precedes
   it without being one of the ancestors the cut ran through. Unknown
   answers count as not completed. */
static bool navigation_node_completed(const lxb_dom_node_t *node,
                                      void *anchor)
{
    if (navigation_node_follows(node, anchor)) return false;
    size_t depth = 0;
    for (const lxb_dom_node_t *at = ((const lxb_dom_node_t *) anchor)->parent;
         at != NULL; at = at->parent) {
        if (at == node || ++depth > 256u) return false;
    }
    return true;
}

/* Whether the incumbent's `box` (or its focus entry `link`/`control`)
   paints in a viewport-fixed range: in every viewport, whatever its
   recorded y (the fixed box's static position). */
static bool navigation_incumbent_fixed(const LayoutDocument *incumbent,
                                       const LayoutNodeBox *box,
                                       size_t link, size_t control)
{
    for (size_t i = 0; i < incumbent->fixed_count; i++) {
        const FixedRange *range = &incumbent->fixed_ranges[i];
        if (box != NULL ? box->command_start >= range->command_start
                              && box->command_start < range->command_end
            : link != SIZE_MAX ? link >= range->link_start
                                     && link < range->link_end
            : control >= range->control_start
                  && control < range->control_end) return true;
    }
    return false;
}

/* Whether the journal's focus change can restyle anything in `node`'s
   subtree once style_focus_restyle_confined holds for every moved node:
   only the moved nodes, their subtrees and their later siblings' subtrees
   restyle. */
static bool navigation_journal_reaches(const ScriptMutationJournal *mutations,
                                       const lxb_dom_node_t *node)
{
    for (size_t i = 0; i < mutations->count; i++) {
        const lxb_dom_node_t *moved = mutations->records[i].node;
        if (moved == NULL) return true;
        size_t depth = 0;
        for (const lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
            if (++depth > 256u) return true;
            if (at->parent != moved->parent) continue;
            for (const lxb_dom_node_t *sibling = moved; sibling != NULL;
                 sibling = sibling->next) {
                if (sibling == at) return true;
            }
            break;
        }
        for (const lxb_dom_node_t *at = moved; at != NULL; at = at->parent)
            if (at == node) return true;
    }
    return false;
}

static bool navigation_bounded_focus_late_entry(int y, int height, int limit)
{
    return y < limit && (int64_t) y + height < limit;
}

/* The region of a bounded focus layout: the page above `limit` where it is
   provably the complete layout's own, so frontend reads confined to it
   give the complete layout's answers (script reads none of it). Boxes are
   recorded when their element's layout ends, so those recorded before the
   cut (LayoutDocument preview_cut_*) are complete; the proof, against the
   incumbent complete (or region-proven) layout:

   - the cut was exact: no table measured from its first rows only;
   - everything recorded after the cut either lies at or below `limit`
     (positioned content whose static position or containing block the cut
     shortened lands there) or starts in the region and reaches past
     `limit` (the ancestors the cut ran through: same top, and they cover
     the rest of the region either way);
   - nothing the incumbent drew in the region is missing (content traversed
     after the cut but painted above it, such as a late positioned box; a
     viewport-fixed box paints in every viewport, wherever it is recorded);
   - the region stops short of `limit` by as far as any completed box's
     bottom edge rose: late content moves with the content before it, so
     nothing the incumbent drew below `limit` rises into the region;
   - the ancestors the cut ran through, and every completed box reaching
     past the cut (a float the late lines wrap around), keep their columns,
     so late content keeps its widths and line breaks;
   - every scrolled box is complete, so it keeps its scroll offset;
   - every node the journal names is complete and a screen above the
     region's end, so revealing the focus scrolls within the region;
   - the focus change restyles nothing the cut did not complete. Late
     content the incumbent drew nowhere near the region could otherwise be
     revealed there (a later section shown by the focus, whose fixed,
     positioned or transformed content paints anywhere): the cut never
     traversed it, so nothing of it is recorded after the cut.

   Late content whose own and ancestors' styles are unchanged paints where
   the incumbent painted it, moved only with the completed content before
   it: so it is in the region only if it was there in the incumbent, at
   most the rise below it.

   Returns the region end, or 0 when the proof fails. */
static int navigation_bounded_focus_region(
    const NavigationSession *session, const LayoutDocument *incumbent,
    const LayoutDocument *next, const ScriptMutationJournal *mutations,
    int limit)
{
    if (next->preview_inexact || limit <= 0) return 0;
    int viewport = session->viewport.css_height;
    size_t cut_boxes = next->preview_cut_box_count;
    if (cut_boxes > next->node_box_count) cut_boxes = next->node_box_count;
    const lxb_dom_node_t *anchor = cut_boxes == 0 ? NULL
        : next->node_boxes[cut_boxes - 1].node;
    if (anchor == NULL) return 0;
    /* Late content moves with the completed content before it: as far up
       as any completed box's bottom edge rose. The region stops that much
       short of the incumbent's evidence. */
    size_t incumbent_complete = incumbent->content_limit_y == 0
        ? incumbent->node_box_count
        : incumbent->preview_cut_box_count < incumbent->node_box_count
        ? incumbent->preview_cut_box_count : incumbent->node_box_count;
    int64_t rise = 0;
    /* The flow continues past the cut from the last completed element's
       bottom edge; in the incumbent, from the last one it holds complete
       (in-flow content before either ends above it). */
    const LayoutNodeBox *last = &next->node_boxes[cut_boxes - 1];
    int64_t cut_y = (int64_t) last->y + last->height;
    int64_t incumbent_cut_y = INT64_MIN;
    for (size_t i = 0; i < cut_boxes; i++) {
        const LayoutNodeBox *box = &next->node_boxes[i];
        const LayoutNodeBox *before = layout_box_for_node(incumbent,
                                                          box->node);
        if (before == NULL
            || (size_t) (before - incumbent->node_boxes) >= incumbent_complete)
            continue;
        int64_t up = ((int64_t) before->y + before->height)
            - ((int64_t) box->y + box->height);
        if (up > rise) rise = up;
        incumbent_cut_y = (int64_t) before->y + before->height;
    }
    if (rise >= limit || incumbent_cut_y == INT64_MIN) return 0;
    /* A completed box beside the late content (a float running past the
       cut, in either layout) shapes its lines: it must keep its columns. */
    for (size_t i = 0; i < cut_boxes; i++) {
        const LayoutNodeBox *box = &next->node_boxes[i];
        const LayoutNodeBox *before = layout_box_for_node(incumbent,
                                                          box->node);
        bool compared = before != NULL
            && (size_t) (before - incumbent->node_boxes) < incumbent_complete;
        if (((int64_t) box->y + box->height > cut_y
             || (compared && (int64_t) before->y + before->height
                                 > incumbent_cut_y))
            && (!compared || before->x != box->x
                || before->width != box->width)) return 0;
    }
    /* So must the ancestors the cut ran through, which hold it. One a
       bounded incumbent never reached has no columns to compare: it keeps
       them when the change restyles nothing inside it (it is unsettled, so
       not itself; see the confinement below). */
    size_t depth = 0;
    for (const lxb_dom_node_t *at = anchor->parent; at != NULL;
         at = at->parent) {
        if (++depth > 256u) return 0;
        const LayoutNodeBox *box = layout_box_for_node(next, at);
        const LayoutNodeBox *before = layout_box_for_node(incumbent, at);
        if (box == NULL && before == NULL) continue;
        if (box != NULL && before == NULL && incumbent->content_limit_y != 0
            && !navigation_journal_reaches(mutations, at)) continue;
        if (box == NULL || before == NULL || box->x != before->x
            || box->width != before->width) return 0;
    }
    int window = limit;
    limit -= (int) rise;
    for (size_t i = cut_boxes; i < next->node_box_count; i++) {
        const LayoutNodeBox *box = &next->node_boxes[i];
        if (box->node != NULL && navigation_bounded_focus_late_entry(
                box->y, box->height, limit)) return 0;
    }
    for (size_t i = next->preview_cut_link_count; i < next->link_count; i++)
        if (navigation_bounded_focus_late_entry(
                next->links[i].y, next->links[i].height, limit)) return 0;
    for (size_t i = next->preview_cut_control_count;
         i < next->control_count; i++)
        if (navigation_bounded_focus_late_entry(
                next->controls[i].y, next->controls[i].height, limit)) return 0;
    for (size_t i = 0; i < incumbent->node_box_count; i++) {
        const LayoutNodeBox *box = &incumbent->node_boxes[i];
        if (box->node == NULL) continue;
        bool scrolled = box->scroll_x != 0 || box->scroll_y != 0;
        if (!scrolled && box->y >= window
            && !navigation_incumbent_fixed(incumbent, box, SIZE_MAX,
                                           SIZE_MAX)) continue;
        const LayoutNodeBox *kept = layout_box_for_node(next, box->node);
        /* A box this focus change removed is gone from the complete
           layout too; one the cut skipped comes after its last completed
           element. */
        if ((kept == NULL
             && (cut_boxes == 0 || navigation_node_follows(
                     box->node, next->node_boxes[cut_boxes - 1].node)))
            || (kept != NULL && scrolled
                && (size_t) (kept - next->node_boxes) >= cut_boxes)) return 0;
    }
    /* The same for the focus map, whose links need no node box. */
    for (size_t i = 0; i < incumbent->link_count + incumbent->control_count;
         i++) {
        bool control = i >= incumbent->link_count;
        const LinkRegion *link = control ? NULL : &incumbent->links[i];
        const ControlRegion *field = control
            ? &incumbent->controls[i - incumbent->link_count] : NULL;
        lxb_dom_node_t *node = control ? field->node : link->node;
        int y = control ? field->y : link->y;
        bool kept_control = false;
        size_t kept = 0;
        if (node == NULL
            || (y >= window && !navigation_incumbent_fixed(
                    incumbent, NULL, control ? SIZE_MAX : i,
                    control ? i - incumbent->link_count : SIZE_MAX))
            || layout_focus_for_node(next, node, &kept_control, &kept))
            continue;
        if (cut_boxes == 0 || navigation_node_follows(
                node, next->node_boxes[cut_boxes - 1].node)) return 0;
    }
    for (size_t i = 0; i < mutations->count; i++) {
        const lxb_dom_node_t *node = mutations->records[i].node;
        bool control = false;
        size_t index = 0;
        if (layout_focus_for_node(next, node, &control, &index)) {
            int y = control ? next->controls[index].y : next->links[index].y;
            int height = control ? next->controls[index].height
                                 : next->links[index].height;
            if (index >= (control ? next->preview_cut_control_count
                                  : next->preview_cut_link_count)
                || (int64_t) y + height + viewport > limit) return 0;
        } else if (layout_focus_for_node(incumbent, node, &control, &index)) {
            return 0;
        }
        const LayoutNodeBox *box = layout_box_for_node(next, node);
        if (box == NULL) {
            if (layout_box_for_node(incumbent, node) != NULL
                || !navigation_node_completed(node, (void *) anchor))
                return 0;
            continue;
        }
        if ((size_t) (box - next->node_boxes) >= cut_boxes
            || (int64_t) box->y + box->height + viewport > limit) return 0;
    }
    /* Content the cut never traversed records nothing, and the incumbent
       drew nothing of what this change reveals: a later section shown by
       the focus (its fixed, positioned or transformed content can paint
       anywhere in the region). Each moved node, completed above, must
       restyle nothing the cut did not complete. */
    for (size_t i = 0; i < mutations->count; i++)
        if (!style_focus_restyle_confined(&session->page.stylesheet,
                mutations->records[i].node, navigation_node_completed,
                (void *) anchor)) return 0;
    return limit;
}

/* After a provisional commit skipped the whole-document image walk, the
   first complete layout's unresolved visuals become the deferred image
   queue, loaded nearest the reader first. */
static void navigation_queue_discovered_visuals(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    page->visual_discovery_pending = false;
    const LayoutDocument *layout = &page->layout;
    size_t documents = 0, visuals = 0;
    for (size_t i = 0; i < layout->visual_priority_count; i++) {
        if (layout->visual_priority_targets[i].kind
                == IMAGE_PRIORITY_KIND_DOCUMENT) documents++;
        else visuals++;
    }
    /* Images load lazily, nearest the reader first, through the deferred
       queue (which handles only image elements). Masks and backgrounds
       are few distinct icons: one batch in the next idle pass. */
    if (documents != 0 && page->deferred_image_targets == NULL) {
        ImagePriorityTarget *targets = budget_malloc(
            session->budget, documents * sizeof(*targets));
        if (targets != NULL) {
            size_t at = 0;
            for (size_t i = 0; i < layout->visual_priority_count; i++) {
                if (layout->visual_priority_targets[i].kind
                    == IMAGE_PRIORITY_KIND_DOCUMENT)
                    targets[at++] = layout->visual_priority_targets[i];
            }
            page->deferred_image_targets = targets;
            page->deferred_image_count = documents;
            page->deferred_image_cursor = 0;
            page->deferred_image_rank_valid = false;
            page->image_continuation_pending = true;
        }
    }
    if (visuals != 0 && page->visual_batch_targets == NULL) {
        ImagePriorityTarget *targets = budget_malloc(
            session->budget, visuals * sizeof(*targets));
        if (targets != NULL) {
            size_t at = 0;
            for (size_t i = 0; i < layout->visual_priority_count; i++) {
                if (layout->visual_priority_targets[i].kind
                    != IMAGE_PRIORITY_KIND_DOCUMENT)
                    targets[at++] = layout->visual_priority_targets[i];
            }
            page->visual_batch_targets = targets;
            page->visual_batch_count = visuals;
        }
    }
}

static bool navigation_layout_provisional(const NavigationSession *session)
{
    return session != NULL && session->page.loaded
        && session->page.layout_provisional;
}

bool navigation_layout_completion_pending(const NavigationSession *session)
{
    return navigation_layout_provisional(session)
        && navigation_now_us() >= session->page.layout_completion_retry_us;
}

bool navigation_layout_completion_awaited(const NavigationSession *session)
{
    if (!navigation_layout_provisional(session)) return false;
    const NavigationEntry *entry = navigation_current(session);
    int64_t scroll_y = entry == NULL || entry->scroll_y < 0
        ? 0 : entry->scroll_y;
    int64_t viewport = session->viewport.css_height;
    /* The same test navigation_complete_layout_step extends on. */
    return scroll_y + 2 * viewport > session->page.layout.content_limit_y
        && scroll_y + viewport >= session->page.layout.height;
}

static NavigationLayoutCompletion navigation_complete_layout_step(
    NavigationSession *session, bool allow_extend)
{
    if (!navigation_layout_provisional(session))
        return NAVIGATION_LAYOUT_COMPLETION_NONE;
    ScriptRuntime *runtime = session->page.runtime;
    /* A bounded focus layout completes against the DOM its journal
       described. Author changes since then are the next relayout's to
       publish; building past them here would skip their invalidation. */
    if (runtime != NULL && script_runtime_has_pending_mutations(runtime))
        return NAVIGATION_LAYOUT_COMPLETION_CANCELLED;
    uint64_t started = navigation_now_us();
    const NavigationEntry *entry = navigation_current(session);
    int scroll_y = entry == NULL || entry->scroll_y < 0 ? 0 : entry->scroll_y;
    int limit = session->page.layout.content_limit_y;
    int old_height = session->page.layout.height;
    /* A reader scrolling toward the placeholder gets the next screens in a
       fraction of the whole build, which input may preempt again. A page
       with a script realm is completed outright: its frontend settles the
       layout before reading past the region, and a larger provisional
       layout would need its region proven again. */
    bool extend = allow_extend && runtime == NULL
        && scroll_y + 2 * session->viewport.css_height > limit;
    navigation_evict_layout_reuse_under_pressure(session);
    LayoutDocument next = {0};
    /* Visuals a provisional commit skipped are queued from the first
       complete layout, whichever step builds it: an extension can reach
       the end of the document too. */
    bool discover = session->page.visual_discovery_pending;
    if (discover) {
        navigation_ensure_layout_reuse(session);
        layout_reuse_cache_record_unresolved_visuals(
            session->page.layout_reuse, true);
    }
    /* Each extension lays the document out from the top, so it goes at
       least twice as deep as the last: a reader paging to depth D waits
       for about 2D of layout in total, not one wait every few screens. */
    int64_t doubled = 2 * (int64_t) limit;
    /* Script that read geometry meanwhile already built the complete
       layout (js_rt_bridge_layout): adopt it rather than build it again. */
    bool adopted = !extend && runtime != NULL && !discover
        && script_runtime_take_geometry_layout(
               runtime, &session->viewport, &next);
    bool built = adopted;
#if !defined(__PSP__)
    if (!built && tilefinch_test_faults()->refuse_next_layout_completion) {
        tilefinch_test_faults()->refuse_next_layout_completion = false;
        session->layout_build_cancelled = false;
        built = false;
    } else
#endif
    if (!built)
        built = extend
        ? navigation_build_provisional_layout(
              session, &next, old_height,
              doubled > INT_MAX ? INT_MAX : (int) doubled)
        : navigation_build_layout(
              session, &next, session->fonts, session->images);
    if (discover)
        layout_reuse_cache_record_unresolved_visuals(
            session->page.layout_reuse, false);
    if (!built) {
        if (session->layout_build_cancelled) {
            session->performance.provisional_cancellations++;
            return NAVIGATION_LAYOUT_COMPLETION_CANCELLED;
        }
        /* A refused build keeps the provisional screens; idle work tries
           again after a doubling wait. Giving up would leave the page's
           rest a checkerboard for good. */
        uint8_t failures = session->page.layout_completion_failures;
        if (failures < UINT8_MAX) failures++;
        session->page.layout_completion_failures = failures;
        unsigned shift = failures > 6u ? 5u : failures - 1u;
        session->page.layout_completion_retry_us =
            navigation_now_us() + (UINT64_C(500000) << shift);
        return NAVIGATION_LAYOUT_COMPLETION_FAILED;
    }
    session->page.layout_completion_failures = 0;
    session->page.layout_completion_retry_us = 0;
    layout_transfer_scroll_state(&session->page.layout, &next);
    navigation_measure_damage(session, &session->page.layout, &next);
    layout_destroy(&session->page.layout);
    session->page.layout = next;
    session->incremental_relayouts++;
    session->performance.fast_relayouts++;
    uint64_t elapsed = adopted ? navigation_now_us() - started
        : navigation_record_layout_slices(
              session, started, &session->page.layout);
    session->performance.layout_us += elapsed;
    session->performance.relayout_us += elapsed;
    navigation_attach_layout(session);
    navigation_clamp_scroll_to_layout(session);
    if (discover && !session->page.layout_provisional)
        navigation_queue_discovered_visuals(session);
    if (session->page.layout_provisional) {
        session->performance.provisional_extensions++;
        return NAVIGATION_LAYOUT_COMPLETION_EXTENDED;
    }
    session->performance.provisional_completions++;
    return NAVIGATION_LAYOUT_COMPLETION_ADOPTED;
}

NavigationLayoutCompletion navigation_complete_layout(
    NavigationSession *session)
{
    return navigation_complete_layout_step(session, true);
}

int navigation_bounded_focus_region_end(const NavigationSession *session)
{
    return navigation_layout_provisional(session)
        ? session->page.bounded_focus_region_end : 0;
}

bool navigation_complete_layout_sync(NavigationSession *session)
{
    NavigationLayoutCompletion result =
        navigation_complete_layout_step(session, false);
    return result == NAVIGATION_LAYOUT_COMPLETION_NONE
        || result == NAVIGATION_LAYOUT_COMPLETION_ADOPTED;
}

/* ---- Changes inside content that renders nothing -----------------------

   A consent stub inserting a display:none <iframe>, a script appended to
   <body>, an attribute written inside a closed menu: none of these can move
   a pixel, yet each used to cost a full relayout (3.5 s on a front page on
   the PSP). A journal is skipped only when every record is proven inert:

   - its change lies inside an element that computes display:none now (the
     outermost such element on its path: `hidden`, resolved through the
     page's reuse cache exactly as layout resolves it, after the journal's
     invalidations), and that element rendered nothing before either: a
     record whose own element is `hidden` must not be able to have changed
     its display (an insertion, which did not exist before; an allow-listed
     attribute or a non-display inline property that no selector reads);
   - it cannot restyle anything outside `hidden`: no :empty flip of the
     element whose children changed unless `hidden` lies above it, no
     sibling-position answer of an earlier sibling (layout's trailing-key
     facts) or later one (only siblings inserted this turn may follow a
     self-hidden insertion), and no :has() answer (the sheet's :has() plan
     walk finds nothing to drop or mark).

   Every other record keeps the ordinary relayout. Layout numbers list items
   whether or not they render, so a hidden <li> of its own never skips. */
static bool navigation_journal_inserted(void *context,
                                        const lxb_dom_node_t *node)
{
    const ScriptMutationJournal *mutations = context;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind == SCRIPT_MUTATION_CHILD_LIST
            && record->inserted_from_detached && record->node == node)
            return true;
    }
    return false;
}

/* Elements whose hidden content still reaches the page outside the box
   tree: the head (viewport and other metadata), inline SVG (rasterized
   from its DOM), form option lists, media and picture sources, plugins,
   and image maps. */
static bool navigation_hidden_content_escapes(const lxb_dom_node_t *element)
{
    switch (element->local_name) {
    case LXB_TAG_HEAD: case LXB_TAG_SVG: case LXB_TAG_SELECT:
    case LXB_TAG_DATALIST: case LXB_TAG_OPTION: case LXB_TAG_OPTGROUP:
    case LXB_TAG_VIDEO: case LXB_TAG_AUDIO: case LXB_TAG_PICTURE:
    case LXB_TAG_OBJECT: case LXB_TAG_EMBED: case LXB_TAG_MAP:
        return true;
    default:
        return false;
    }
}

/* The outermost element on the path from the root to `node` (inclusive)
   that computes display:none, or NULL when the path renders, leaves the
   document, crosses a non-element (a shadow root), or passes through an
   element whose hidden content escapes the box tree. */
static lxb_dom_node_t *navigation_display_none_root(
    NavigationSession *session, lxb_dom_node_t *node)
{
    lxb_dom_node_t *path[64];
    size_t count = 0;
    lxb_dom_node_t *at = node;
    if (at != NULL && at->type == LXB_DOM_NODE_TYPE_TEXT) at = at->parent;
    for (; at != NULL && at->type != LXB_DOM_NODE_TYPE_DOCUMENT;
         at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT
            || count == sizeof(path) / sizeof(path[0])
            || navigation_hidden_content_escapes(at)) return NULL;
        path[count++] = at;
    }
    if (at == NULL) return NULL;
    ComputedStyle style = layout_initial_root_style();
    while (count != 0) {
        ComputedStyle next;
        layout_reuse_cache_resolve_style(
            session->page.layout_reuse, &session->page.stylesheet,
            session->fonts, path[--count], &style, &next);
        if (next.display == DISPLAY_NONE) return path[count];
        style = next;
    }
    return NULL;
}

static bool navigation_strictly_within(const lxb_dom_node_t *node,
                                       const lxb_dom_node_t *ancestor)
{
    if (node == NULL || ancestor == NULL || node == ancestor) return false;
    for (const lxb_dom_node_t *at = node->parent; at != NULL; at = at->parent)
        if (at == ancestor) return true;
    return false;
}

/* Whether `element` keeps a child that defeats :empty (an element, or text
   with data, as style matching counts them) other than `except` and the
   children inserted this turn. */
static bool navigation_keeps_nonempty_child(
    const ScriptMutationJournal *mutations, const lxb_dom_node_t *element,
    const lxb_dom_node_t *except)
{
    size_t visited = 0;
    for (lxb_dom_node_t *child = element->first_child; child != NULL;
         child = child->next) {
        if (++visited > 512u) return false;
        if (child == except
            || navigation_journal_inserted((void *) mutations, child))
            continue;
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT) return true;
        if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(child, &length);
            if (text != NULL && length != 0) return true;
        }
    }
    return false;
}

/* An attribute whose value no presentational mapping or UA rule reads:
   writing it cannot change the display of the element that carries it,
   provided no author selector names it either. */
static bool navigation_attribute_inert_when_hidden(const Stylesheet *sheet,
                                                   const char *attribute)
{
    static const char *const names[] = {
        "name", "title", "src", "srcdoc", "allow", "allowfullscreen",
        "sandbox", "loading", "referrerpolicy", "role", "tabindex", "alt",
        "async", "defer", "nonce", "crossorigin", "integrity", "charset"
    };
    bool known = (strncmp(attribute, "data-", 5u) == 0
                  && strncmp(attribute, "data-tilefinch", 14u) != 0)
        || strncmp(attribute, "aria-", 5u) == 0;
    for (size_t i = 0; !known && i < sizeof(names) / sizeof(names[0]); i++)
        known = strcmp(attribute, names[i]) == 0;
    return known && !navigation_selector_mentions_attribute(sheet, attribute);
}

static const char *navigation_hidden_record_refusal(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    const ScriptMutationRecord *record, bool *head_checked)
{
    const Stylesheet *sheet = &session->page.stylesheet;
    uint32_t serial = mutations->has_serial_mixed ? 0 : mutations->has_serial;
    lxb_dom_node_t *node = record->node;
    switch (record->kind) {
    case SCRIPT_MUTATION_HEAD_SCRIPT:
        /* A relational record is checked for its own node. */
        if ((!*head_checked || record->relational)
            && !navigation_head_scripts_are_nonvisual(
                   &session->page, record->relational, node))
            return "head";
        *head_checked = true;
        return NULL;
    case SCRIPT_MUTATION_CHILD_LIST:
        if (record->inserted_from_detached) {
            if (node == NULL || !navigation_node_connected(node))
                return "insert-gone";
            lxb_dom_node_t *parent = node->parent;
            if (parent == NULL || parent->type != LXB_DOM_NODE_TYPE_ELEMENT)
                return "insert-parent";
            lxb_dom_node_t *hidden =
                navigation_display_none_root(session, node);
            if (hidden == NULL) return "insert-renders";
            if (hidden == node) {
                if (node->local_name == LXB_TAG_LI) return "insert-li";
                if (layout_insertion_reaches_siblings(
                        session->page.layout_reuse, sheet, node,
                        navigation_journal_inserted, (void *) mutations))
                    return "insert-siblings";
            }
            if (!navigation_strictly_within(parent, hidden)
                && !navigation_keeps_nonempty_child(mutations, parent, NULL))
                return "insert-empty";
            if (record->relational
                && !layout_tree_change_has_inert(
                       session->page.layout_reuse, sheet, node, parent,
                       record->has_entries, serial))
                return "insert-has";
            return NULL;
        }
        /* A removal; a node still connected was moved. */
        if (node != NULL && navigation_node_connected(node)) return "move";
        if (record->scope == NULL
            || !navigation_node_connected(record->scope)
            || record->scope->type != LXB_DOM_NODE_TYPE_ELEMENT)
            return "remove-scope";
        {
            lxb_dom_node_t *hidden =
                navigation_display_none_root(session, record->scope);
            if (hidden == NULL) return "remove-renders";
            /* Still nonempty now, so nonempty before: no :empty flip. */
            if (hidden == record->scope
                && !navigation_keeps_nonempty_child(
                       mutations, record->scope, NULL))
                return "remove-empty";
            /* A freed subtree's keys are gone: only the plan can tell. */
            if (record->relational
                && (node == NULL
                    ? !layout_has_tree_change_inert(
                          sheet, record->scope, record->has_entries, serial)
                    : !layout_tree_change_has_inert(
                          session->page.layout_reuse, sheet, node,
                          record->scope, record->has_entries, serial)))
                return "remove-has";
        }
        return NULL;
    case SCRIPT_MUTATION_TEXT:
    case SCRIPT_MUTATION_INNER_HTML:
    {
        if (node == NULL || !navigation_node_connected(node)) return "text-gone";
        lxb_dom_node_t *element = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            ? node : node->parent;
        if (element == NULL || element->type != LXB_DOM_NODE_TYPE_ELEMENT)
            return "text-parent";
        lxb_dom_node_t *hidden = navigation_display_none_root(session, element);
        if (hidden == NULL) return "text-renders";
        /* Its old children are unknown: only an element still holding
           another nonempty child cannot have flipped :empty. */
        if (!navigation_strictly_within(element, hidden)
            && (record->kind == SCRIPT_MUTATION_INNER_HTML || element == node
                || !navigation_keeps_nonempty_child(mutations, element,
                                                    node)))
            return "text-empty";
        /* innerHTML's old children are gone: only the plan can tell. */
        if (record->relational
            && (record->kind == SCRIPT_MUTATION_INNER_HTML
                ? !layout_has_tree_change_inert(
                      sheet, element, record->has_entries, serial)
                : !layout_tree_change_has_inert(
                      session->page.layout_reuse, sheet, NULL, element,
                      record->has_entries, serial)))
            return "text-has";
        return NULL;
    }
    case SCRIPT_MUTATION_ATTRIBUTE:
    case SCRIPT_MUTATION_INLINE_STYLE:
    {
        if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
            || !navigation_node_connected(node)) return "attribute-gone";
        if (record->relational) return "attribute-has";
        lxb_dom_node_t *hidden = navigation_display_none_root(session, node);
        if (hidden == NULL) return "attribute-renders";
        if (hidden != node) return NULL;
        /* The element that hides its subtree: the write must not have been
           able to change its display, nor reach its siblings. */
        if (record->kind == SCRIPT_MUTATION_ATTRIBUTE)
            return navigation_attribute_inert_when_hidden(
                       sheet, record->attribute) ? NULL : "attribute-own";
        if (record->attribute[0] == '\0'
            || strlen(record->attribute) >= SCRIPT_MUTATION_ATTRIBUTE_LIMIT - 1u
            || strcmp(record->attribute, "display") == 0
            || strcmp(record->attribute, "all") == 0
            || strncmp(record->attribute, "--", 2u) == 0
            || navigation_selector_mentions_attribute(sheet, "style"))
            return "style-own";
        return NULL;
    }
    default:
        return "kind";
    }
}

/* Whether the journal changed only content that renders nothing (see
   above). `reuse_prepared` reports that the journal's layout reuse
   invalidations were applied; the caller must not apply them again. */
static bool navigation_mutations_are_hidden_only(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool *reuse_prepared, const bool *local_focus)
{
    if (session == NULL || mutations == NULL || mutations->count == 0
        || mutations->overflowed || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required
        || mutations->conservative_resource_scan
        || mutations->retirement_unobserved
        || session->style_rebuild_required
        || session->page.layout_reuse == NULL) return false;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_HIDDEN_RELAYOUT_SKIP") != NULL)
        return false;
#endif
    for (size_t i = 0; i < mutations->count; i++) {
        ScriptMutationKind kind = mutations->records[i].kind;
        if (kind != SCRIPT_MUTATION_HEAD_SCRIPT
            && kind != SCRIPT_MUTATION_CHILD_LIST
            && kind != SCRIPT_MUTATION_TEXT
            && kind != SCRIPT_MUTATION_INNER_HTML
            && kind != SCRIPT_MUTATION_ATTRIBUTE
            && kind != SCRIPT_MUTATION_INLINE_STYLE) return false;
    }
    /* Resolve against styles the journal's changes already invalidated. */
    navigation_prepare_layout_reuse(session, mutations, false, local_focus);
    *reuse_prepared = true;
    bool head_checked = false;
    for (size_t i = 0; i < mutations->count; i++) {
        /* Inserted from a detached tree and gone again in the same turn
           (a measurement probe): the tree is as it was. */
        if (navigation_mutation_is_transient(mutations, i)) continue;
        const char *refusal = navigation_hidden_record_refusal(
            session, mutations, &mutations->records[i], &head_checked);
        if (refusal == NULL) continue;
#ifndef TILEFINCH_NO_TRACE
        if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL)
            fprintf(stderr, "hidden-relayout refused=%s record=%zu/%zu\n",
                    refusal, i, mutations->count);
#endif
        return false;
    }
    return true;
}

static bool navigation_relayout_body(NavigationSession *session,
                                     StyleWebFonts **retained_fonts,
                                     bool *restart_after_retirement);

/* These writes leave every node and text buffer borrowed by the incumbent
   intact. Structural/text mutations, dropped journal entries and resource
   replacement remain owner-only: the old display list may no longer be safe
   even if a later layout happens to look unchanged. Generated attr() content
   is retained in layout-owned storage, not in the changed attribute buffer. */
static bool navigation_mutations_keep_committed_layout_servable(
    const ScriptMutationJournal *mutations)
{
    if (mutations == NULL || mutations->overflowed
        || mutations->retirement_unobserved
        || mutations->resource_rebuild_required
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required
        || mutations->conservative_resource_scan) return false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind == SCRIPT_MUTATION_INLINE_STYLE) continue;
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || (strcmp(record->attribute, "class") != 0
                && strcmp(record->attribute, "style") != 0
                && strcmp(record->attribute, "data-tilefinch-focus") != 0))
            return false;
    }
    return true;
}

bool navigation_relayout(NavigationSession *session)
{
    unsigned ledger = work_ledger_enter(WORK_LEDGER_LAYOUT);
#ifndef TILEFINCH_NO_TRACE
    /* One line per relayout (TILEFINCH_TRACE_RELAYOUT_CENSUS): which path
       it took, how long, and how many element styles its build resolved. */
    bool census = session != NULL
        && getenv("TILEFINCH_TRACE_RELAYOUT_CENSUS") != NULL;
    NavigationPerformance before = {0};
    LayoutReuseStats styles_before = {0};
    uint64_t census_started = 0;
    if (census) {
        before = session->performance;
        layout_reuse_cache_stats(session->page.layout_reuse, &styles_before);
        census_started = navigation_now_us();
    }
#endif
    StyleWebFonts *retained_fonts = NULL;
    bool ok = false;
    for (unsigned attempt = 0; attempt < 2u; attempt++) {
        bool restart = false;
        ok = navigation_relayout_body(session, &retained_fonts, &restart);
        if (!restart) break;
    }
    if (retained_fonts != NULL)
        stylesheet_release_web_fonts(session->budget, retained_fonts);
#ifndef TILEFINCH_NO_TRACE
    if (census) {
        const NavigationPerformance *after = &session->performance;
        LayoutReuseStats styles_after = {0};
        layout_reuse_cache_stats(session->page.layout_reuse, &styles_after);
        const char *kind =
            after->full_relayouts != before.full_relayouts ? "full"
            : after->bounded_focus_relayouts != before.bounded_focus_relayouts
                ? "bounded"
            : after->bounded_focus_fallbacks != before.bounded_focus_fallbacks
                ? "bounded-fallback"
            : after->fast_relayouts != before.fast_relayouts ? "build"
            : after->hidden_relayout_skips != before.hidden_relayout_skips
                ? "skip-hidden"
            : after->semantic_relayout_skips != before.semantic_relayout_skips
                ? "skip-semantic"
            : after->focus_outline_relayout_skips
                      != before.focus_outline_relayout_skips
                  || after->focus_paint_relayout_skips
                         != before.focus_paint_relayout_skips
                ? "skip-focus" : "other";
        fprintf(stderr,
                "relayout-census kind=%s ok=%d us=%llu styles=%zu/%zu "
                "resets=%zu commands=%zu height=%d\n", kind, (int) ok,
                (unsigned long long) (navigation_now_us() - census_started),
                styles_after.style_hits - styles_before.style_hits,
                styles_after.style_misses - styles_before.style_misses,
                styles_after.full_resets - styles_before.full_resets,
                session->page.layout.count, session->page.layout.height);
    }
#endif
    work_ledger_leave(ledger);
    return ok;
}

/* A journal of nothing but the frontend's focus markers. */
static bool navigation_journal_focus_markers_only(
    const ScriptMutationJournal *mutations)
{
    if (mutations == NULL || mutations->count == 0 || mutations->overflowed
        || mutations->retirement_unobserved
        || mutations->resource_rebuild_required
        || mutations->conservative_resource_scan
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required) return false;
    for (size_t i = 0; i < mutations->count; i++) {
        const ScriptMutationRecord *record = &mutations->records[i];
        if (record->kind != SCRIPT_MUTATION_ATTRIBUTE
            || strcmp(record->attribute, "data-tilefinch-focus") != 0)
            return false;
    }
    return true;
}

/* Bounded focus relayout: a focus move on a page with a script realm lays
   out the viewport plus two screens (the scriptless relayout preview's
   shape) and leaves the rest to idle completion, which input preempts.
   A focus move re-flows the whole document today (the SC-1 form: 238 of
   317 ms per move on the PSP), although the reader sees one screen.

   It is offered only where the result can be held to the rule that script
   never observes a truncated layout (js_rt_bridge_layout): a page without
   container queries (their state would be collected from the truncated
   geometry and reach getComputedStyle), with room for the complete build
   that must follow, that has not been refused bounding (see
   NavigationPage bounded_focus_refused). The worker then proves the
   result's region before publishing it (navigation_bounded_focus_region). */
static bool navigation_bounded_focus_eligible(
    const NavigationSession *session, const ScriptMutationJournal *mutations)
{
    const NavigationPage *page = &session->page;
    if (page->runtime == NULL || !page->loaded || page->bounded_focus_refused
        || session->viewport.css_height <= 0
        || !navigation_journal_focus_markers_only(mutations)
        || stylesheet_has_container_queries(&page->stylesheet)
        || style_container_layout_state_present(&page->stylesheet))
        return false;
    /* The complete build that follows (idle, script or input) holds a whole
       layout beside this one: keep twice its size in reserve. A truncated
       incumbent (the previous bounded move's) holds a few screens, so the
       last complete layout's size is the prediction. */
    bool complete = page->layout.content_limit_y == 0;
    size_t commands = complete ? page->layout.count
        : page->layout_complete_commands;
    size_t boxes = complete ? page->layout.node_box_count
        : page->layout_complete_node_boxes;
    if (commands < page->layout.count) commands = page->layout.count;
    if (boxes < page->layout.node_box_count) boxes = page->layout.node_box_count;
    size_t estimate = commands * sizeof(DrawCommand)
        + boxes * sizeof(LayoutNodeBox);
    return !budget_pressure_required(session->budget, estimate, estimate);
}

/* What a relayout must do, classified from the session's rebuild state and
   the consumed mutation journal before any of its work runs. It reads both
   and writes neither. navigation_relayout_body refines the plan as its work
   reports back: a style append can settle the sheet, a changed resource
   fingerprint or a refused image rebuild can force the full rebuild. */
static NavigationRelayoutPlan navigation_relayout_plan(
    const NavigationSession *session, const ScriptMutationJournal *mutations,
    bool mutation_driven)
{
    NavigationRelayoutPlan plan = {.mutation_driven = mutation_driven};
    /* A sheet whose sources were bounded out by a retirement holds
       shadow-only adopted rules it now refuses everywhere (fail closed).
       Rebuild it rather than lay out its components unstyled. */
    const Stylesheet *page_sheet = &session->page.stylesheet;
    plan.rebuild_bounded_out_sheet = page_sheet->style_sources_bounded_out
        && page_sheet->scopes.adopted_source_count != 0
        && page_sheet->scopes.adopted_shadow_only_mask != 0;
    bool style_rebuild_required = session->style_rebuild_required
        || plan.rebuild_bounded_out_sheet;
    plan.conservative_scan = !mutation_driven
        || mutations->conservative_resource_scan;
    plan.full_rebuild = style_rebuild_required
        || mutations->resource_rebuild_required;
    /* image_rebuild (re-derive the image table alone, keeping the sheet,
       its external CSS, stylesheet events and web fonts) starts false. */
    plan.style_append_tried = mutation_driven
        && !style_rebuild_required
        && mutations->resource_rebuild_required
        && !mutations->retirement_unobserved;
    plan.pause_image_preloads = plan.full_rebuild || plan.conservative_scan
        || mutations->image_resource_scan_required
        || mutations->image_resource_refresh_required;
    plan.image_resource_scan = mutation_driven
        && mutations->image_resource_scan_required;
    plan.image_resource_refresh = mutation_driven
        && mutations->image_resource_refresh_required;
    plan.bounded_focus = mutation_driven && !plan.full_rebuild
        && navigation_bounded_focus_eligible(session, mutations);
    return plan;
}

#if !defined(__PSP__)
NavigationRelayoutPlan navigation_test_relayout_plan(
    const NavigationSession *session, const ScriptMutationJournal *mutations,
    bool mutation_driven)
{
    return navigation_relayout_plan(session, mutations, mutation_driven);
}
#endif

/* What a relayout's worker needs besides its plan: the consumed journal,
   the history entry, the start stamp and the resource fingerprint the plan
   settled on. */
typedef struct {
    const ScriptMutationJournal *mutations;
    const NavigationEntry *entry;
    uint64_t started;
    uint64_t fingerprint;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool full_style_rebuild;
#endif
#if NAVIGATION_RELAYOUT_PHASE_LOG
    /* The prepare-span stamps navigation_log_prepare_phases reports. */
    uint64_t append_started;
    uint64_t append_before[5];
    uint64_t decided;
    uint64_t reuse_prepared;
    uint64_t images_done;
    LayoutReuseStats images_before;
    LayoutReuseStats images_after;
#endif
} NavigationRelayoutPass;

/* The bounded focus build (NavigationRelayoutPlan bounded_focus): the
   viewport plus two screens, extended to keep the journal's nodes (the
   focus that left and the one that arrived) a screen inside the region.
   A result whose region cannot be proven is discarded for the complete
   build, and the page stops bounding: the failing structure (a late
   positioned box, a long table) is still there at the next move. */
static bool navigation_build_bounded_focus_layout(
    NavigationSession *session, LayoutDocument *next,
    const ScriptMutationJournal *mutations, int *region_end)
{
    *region_end = 0;
    const LayoutDocument *incumbent = &session->page.layout;
    int viewport = session->viewport.css_height;
    int64_t minimum = 0;
    for (size_t i = 0; i < mutations->count; i++) {
        const LayoutNodeBox *box =
            layout_box_for_node(incumbent, mutations->records[i].node);
        if (box == NULL) continue;
        int64_t reach = (int64_t) box->y + box->height + 3 * (int64_t) viewport;
        if (reach > minimum) minimum = reach;
    }
    int limit = 0;
    if (!navigation_build_provisional_layout_limit(
            session, next, incumbent->height,
            minimum > INT_MAX ? INT_MAX : (int) minimum, &limit))
        return false;
    if (next->content_limit_y == 0) return true;
    /* The page's extent (scroll range, scrollbar): the incumbent's, moved
       by however far this change moved the nearest element completed
       before the cut (and complete in the incumbent too), which everything
       after it follows in ordinary flow. The complete build corrects it if
       the flow was not ordinary. */
    size_t complete_before = incumbent->content_limit_y == 0
        ? incumbent->node_box_count : incumbent->preview_cut_box_count;
    int64_t height = incumbent->height;
    size_t cut_boxes = next->preview_cut_box_count < next->node_box_count
        ? next->preview_cut_box_count : next->node_box_count;
    for (size_t at = cut_boxes, tried = 0; at != 0 && tried < 64u;
         at--, tried++) {
        const LayoutNodeBox *last = &next->node_boxes[at - 1u];
        const LayoutNodeBox *before =
            layout_box_for_node(incumbent, last->node);
        if (before == NULL
            || (size_t) (before - incumbent->node_boxes) >= complete_before)
            continue;
        height += ((int64_t) last->y + last->height)
            - ((int64_t) before->y + before->height);
        break;
    }
    if (height < next->content_limit_y) height = next->content_limit_y;
    next->height = height > INT_MAX ? INT_MAX : (int) height;
    if (next->scroll_width < incumbent->scroll_width)
        next->scroll_width = incumbent->scroll_width;
    /* The region stops a screen short of the build's limit: content that
       moved there with this focus change (a row grown above it) is not
       mistaken for content the cut lost. The proof compares against the
       incumbent, which is evidence only where it is itself the complete
       layout's own: a bounded incumbent's region bounds this one too
       (content its cut lost below that region is missing from both). */
    int proof_limit = limit - viewport;
    if (incumbent->content_limit_y != 0) {
        int proven = navigation_bounded_focus_region_end(session);
        if (proof_limit > proven) proof_limit = proven;
    }
    *region_end = navigation_bounded_focus_region(
        session, incumbent, next, mutations, proof_limit);
    if (*region_end != 0) {
        session->performance.bounded_focus_relayouts++;
        return true;
    }
    session->performance.bounded_focus_fallbacks++;
    session->page.bounded_focus_refused = true;
    layout_destroy(next);
    memset(next, 0, sizeof(*next));
    return navigation_build_layout(session, next, session->fonts,
                                   session->images);
}

/* The incremental relayout: build the next layout against the retained
   sheet (or adopt the forced-geometry layout, or a provisional prefix),
   measure its damage against the incumbent and publish it. */
static TILEFINCH_OUT_OF_LINE bool navigation_relayout_incremental(
    NavigationSession *session, const NavigationRelayoutPlan *plan,
    const NavigationRelayoutPass *pass)
{
    const ScriptMutationJournal *mutations = pass->mutations;
    uint64_t started = pass->started;
    uint64_t fingerprint = pass->fingerprint;
    if (plan->image_resource_scan || plan->image_resource_refresh || plan->image_rebuild) {
        session->resource_fingerprint = fingerprint;
        navigation_note_committed_fingerprint(session);
    }
    navigation_evict_layout_reuse_under_pressure(session);
    LayoutDocument next = {0};
    size_t failures_before = session->budget->failure_count;
    size_t injected_failures_before = session->budget->injected_failure_count;
    int incumbent_height = session->page.layout.height;
    bool incumbent_released = false;
#if !defined(__PSP__)
    bool injected_refusal =
        navigation_test_consume_same_document_relayout_refusal();
    if (injected_refusal)
        budget_inject_failure_after(session->budget, 0u);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t relayout_prepared = navigation_now_us();
#endif
    bool provisional = navigation_relayout_preview_eligible(session);
    bool bounded = plan->bounded_focus && !provisional
        && !plan->image_rebuild && !plan->image_resource_scan
        && !plan->image_resource_refresh && !plan->style_append_tried;
    int bounded_region_end = 0;
    bool servable = !plan->image_rebuild && !plan->image_resource_refresh
        && (!plan->mutation_driven
            || navigation_mutations_keep_committed_layout_servable(
                   mutations));
    /* A stable incumbent can serve scroll feedback during either a full
       replacement or a scriptless preview. JS reflection is deferred to
       a later owner turn after adoption, so the checkpoint cannot mutate
       the DOM underneath either layout. Author turns and destructive
       journals stay queued. */
    if (servable) {
        session->committed_layout_servable = true;
        (void) tilefinch_platform_cooperate_at(
            "page-scroll-servable", 1,
            TILEFINCH_COOPERATE_SCROLL_SERVABLE);
    }
    /* Forced geometry already performed the complete flow for this exact
       generation. Resource work must be settled without replacing any
       borrowed image storage before ownership can move to presentation. */
    bool geometry_adopted = plan->mutation_driven && !provisional
        && !plan->style_append_tried && !plan->image_rebuild
        && !plan->image_resource_scan && !plan->image_resource_refresh
        && !mutations->overflowed && !mutations->retirement_unobserved
        && !mutations->resource_rebuild_required
        && script_runtime_take_geometry_layout(session->page.runtime,
            &session->viewport, &next);
    /* Forced geometry, when script already built it, is the complete
       layout at no cost: a bounded focus build gives way to it. */
    bool built = geometry_adopted ? true : provisional
        ? navigation_build_provisional_layout(
              session, &next, session->page.layout.height, 0)
        : bounded
        ? navigation_build_bounded_focus_layout(
              session, &next, mutations, &bounded_region_end)
        : navigation_build_layout(
              session, &next, session->fonts, session->images);
    if (servable) {
        session->committed_layout_servable = false;
        (void) tilefinch_platform_cooperate_at(
            "page-scroll-servable", 0,
            TILEFINCH_COOPERATE_SCROLL_SERVABLE);
    }
    if (built && next.content_limit_y != 0)
        session->performance.provisional_relayouts++;
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
    if (!built && plan->mutation_driven && !session->layout_build_cancelled
        && session->budget->failure_count > failures_before
        && session->budget->injected_failure_count == injected_failures_before) {
        built = navigation_retry_mutation_layout_under_pressure(
            session, &next, &incumbent_released);
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t relayout_built = navigation_now_us();
#endif
    if (!built) {
        return navigation_relayout_failed(session, plan->mutation_driven);
    }
    layout_transfer_scroll_state(&session->page.layout, &next);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    uint64_t relayout_transferred = navigation_now_us();
#endif
    if (incumbent_released) {
        session->relayout_damage_valid = true;
        session->relayout_damage_left = 0;
        session->relayout_damage_top = 0;
        session->relayout_damage_right = session->viewport.css_width;
        session->relayout_damage_bottom = incumbent_height > next.height
            ? incumbent_height : next.height;
    } else {
        navigation_measure_damage(session, &session->page.layout, &next);
        bool focus_only = plan->mutation_driven && !plan->full_rebuild
            && !plan->image_rebuild && !plan->image_resource_scan && !plan->image_resource_refresh
            && !plan->style_append_tried && !mutations->overflowed
            && !mutations->retirement_unobserved && mutations->count != 0;
        bool attributes_only = plan->mutation_driven && !plan->full_rebuild
            && !plan->style_append_tried && !mutations->overflowed
            && !mutations->retirement_unobserved && mutations->count != 0;
        for (size_t i = 0; attributes_only && i < mutations->count; i++) {
            ScriptMutationKind kind = mutations->records[i].kind;
            attributes_only = kind == SCRIPT_MUTATION_ATTRIBUTE
                || kind == SCRIPT_MUTATION_INLINE_STYLE;
        }
        for (size_t i = 0; focus_only && i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            focus_only = record->kind == SCRIPT_MUTATION_ATTRIBUTE
                && strcmp(record->attribute, "data-tilefinch-focus") == 0;
        }
        if ((focus_only || attributes_only)
            && navigation_fixed_focus_context_equal(
                &session->page.layout, &next, focus_only))
            session->fixed_paint_relayout_generation =
                session->incremental_relayouts + 1u;
    }
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
    if (plan->mutation_driven) {
        session->performance.mutation_fast_relayouts++;
    }
    uint64_t elapsed = geometry_adopted
        ? navigation_now_us() - started
        : navigation_record_layout_slices(session, started, &next);
    session->performance.layout_us += elapsed;
    session->performance.relayout_us += elapsed;
    navigation_attach_layout(session);
    if (session->page.layout_provisional && bounded_region_end != 0) {
        session->page.bounded_focus_region_end = bounded_region_end;
        script_runtime_set_layout_region(session->page.runtime,
                                         bounded_region_end);
    }
    navigation_clamp_scroll_to_layout(session);
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t events_started = navigation_now_us();
#endif
    if (plan->mutation_driven
        && !navigation_sweep_stylesheet_events(session)) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t events_done = navigation_now_us();
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    char relayout_phases[256];
    snprintf(relayout_phases, sizeof(relayout_phases),
        "tilefinch-relayout-phases: prepare=%lluus build=%lluus transfer=%lluus damage=%lluus destroy=%lluus attach=%lluus events=%lluus",
        (unsigned long long) (relayout_prepared - started),
        (unsigned long long) (relayout_built - relayout_prepared),
        (unsigned long long) (relayout_transferred - relayout_built),
        (unsigned long long) (relayout_damaged - relayout_transferred),
        (unsigned long long) (relayout_destroyed - relayout_damaged),
        (unsigned long long) (events_started - relayout_destroyed),
        (unsigned long long) (events_done - events_started));
    tilefinch_platform_log_message(relayout_phases);
#endif
#if NAVIGATION_RELAYOUT_PHASE_LOG
    navigation_log_prepare_phases(
        session, started, pass->append_started, pass->append_before,
        pass->decided, pass->reuse_prepared, pass->images_done,
        &pass->images_before, &pass->images_after,
        events_done - events_started);
#endif
    if (plan->mutation_driven && session->external_resources_enabled) {
        for (size_t i = 0; i < mutations->count; i++) {
            const ScriptMutationRecord *record = &mutations->records[i];
            if ((record->kind != SCRIPT_MUTATION_CHILD_LIST
                 && record->kind != SCRIPT_MUTATION_INNER_HTML)
                || record->node == NULL
                || !navigation_node_connected(record->node)) continue;
            if (!stylesheet_document_resources_note_dynamic_tokens(
                    &session->page.stylesheet_resources, record->node))
                continue;
            /* Use the existing presentation transaction: retained CSS
               is re-sampled against the hydrated DOM, while refusal
               leaves this already-published layout and sheet intact. */
            NavigationUserCssTransaction *transaction =
                navigation_apply_presentation_css_begin(
                    session, session->user_css, session->user_css_length,
                    false);
            if (transaction != NULL)
                navigation_apply_user_css_commit(session, transaction);
            break;
        }
    }
    return true;
}

/* The full rebuild: stylesheet, external CSS and its events, user CSS,
   the image table, web fonts and layout are all rebuilt from the DOM, and
   the old display list is destroyed first. Rare next to the incremental
   path, so it stays out of line. */
static TILEFINCH_OUT_OF_LINE bool navigation_relayout_full(
    NavigationSession *session, const NavigationRelayoutPlan *plan,
    const NavigationRelayoutPass *pass, StyleWebFonts **retained_fonts,
    bool *restart_after_retirement)
{
    (void) plan;
    const ScriptMutationJournal *mutations = pass->mutations;
    const NavigationEntry *entry = pass->entry;
    uint64_t started = pass->started;
    uint64_t fingerprint = pass->fingerprint;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool full_style_rebuild = pass->full_style_rebuild;
#endif
    /* Rebuildable owner caches must not consume the workspace for required
       style/layout. Do this only on the already-expensive full path, never
       for ordinary focus, scrolling or canvas updates. */
    size_t rebuild_reserve = session->budget->limit / 8u;
    if (rebuild_reserve > 4u*1024u*1024u) rebuild_reserve = 4u*1024u*1024u;
    if (session->reclaim_optional != NULL
        && budget_remaining(session->budget) < rebuild_reserve)
        session->reclaim_optional(session->reclaim_optional_opaque);
    int old_height = session->page.layout.height;
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    /* Full rebuild steps (fast relayouts print tilefinch-relayout-phases). */
    uint64_t full_steps[7] = {0};
    size_t full_step = 0;
#define FULL_RELAYOUT_STEP() \
    (full_step < 7 ? (void) (full_steps[full_step++] = navigation_now_us()) \
                   : (void) 0)
#else
#define FULL_RELAYOUT_STEP() ((void) 0)
#endif
    navigation_prepare_layout_reuse(session, mutations, true, NULL);
    navigation_evict_layout_reuse_under_pressure(session);
    fonts_external_loader_cancel(
        &session->page.external_font_loader,
        session->page.resource_scheduler, &session->page.external_fonts);
    layout_destroy(&session->page.layout);
    /* Keep only the independently owned font registry, not the old rules or
       layout. The wrapper releases it on every success/refusal path. */
    if (session->web_fonts_enabled && session->web_font_mode != WEB_FONTS_OFF) {
        if (*retained_fonts == NULL)
            *retained_fonts = session->page.stylesheet.web_fonts;
        else if (session->page.stylesheet.web_fonts != NULL)
            stylesheet_release_web_fonts(session->budget,
                session->page.stylesheet.web_fonts);
        session->page.stylesheet.web_fonts = NULL;
    }
    stylesheet_destroy(&session->page.stylesheet);
    FULL_RELAYOUT_STEP();
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
    bool user_css_added = false;
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
        /* Display-retarget counters describe the page, not one table. */
        ImageResources counters = {0};
        counters.retarget = session->page.images.retarget;
        images_prepare_external_rebuild(&session->page.images);
        FULL_RELAYOUT_STEP();
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
        FULL_RELAYOUT_STEP();
        if (!navigation_dispatch_stylesheet_events(
                session, session->page.resource_base_url)
            && !navigation_retire_exhausted_script_realms(session, false)) {
            session->page.loaded = false;
            return navigation_relayout_failed(session, true);
        }
        FULL_RELAYOUT_STEP();
        /* The user stylesheet completes the sheet before image discovery,
           as at commit. Appended after it, it moved the sheet's generation
           between discovery and the layout below, so layout discarded every
           style discovery had resolved into the shared reuse cache and
           resolved the whole document again (www.reddit.com on PPSSPP:
           about 29 s of a 60 s rebuild), and discovery fetched images under
           elements the user sheet hides. */
        if (session->user_css_length != 0
            && !stylesheet_add_user_css(&session->page.stylesheet,
                                        session->user_css,
                                        session->user_css_length)) {
            session->page.loaded = false;
            return navigation_relayout_failed(session, true);
        }
        user_css_added = true;
        stylesheet_share_unchanged_web_fonts(&session->page.stylesheet,
            *retained_fonts, session->maximum_font_face_backend_bytes);
        navigation_ensure_layout_reuse(session);
        bool images_loaded = false;
        /* A mutation can consume the shared ceiling while reclaimable VM
           storage still fits its separate heap limit. Retry only a genuine
           allocator refusal: collect once, then stop exhausted page realms
           as the required-layout recovery below does. Cancellation, policy
           errors and injected faults retain the ordinary fail-closed path. */
        for (unsigned attempt = 0; attempt < 3u; attempt++) {
            size_t image_failures = session->budget->failure_count;
            size_t image_injections = session->budget->injected_failure_count;
            images_loaded = images_rebuild_external_reusing_rasters(
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
                session->fonts, session->viewport.css_width, deferred, deferred_count);
            if (images_loaded || attempt == 2u || session->page.runtime == NULL
                || session->budget->failure_count == image_failures
                || session->budget->injected_failure_count != image_injections)
                break;
            if (!tilefinch_platform_cooperate("image-rebuild-retry", attempt))
                break;
            if (attempt == 0)
                (void) script_runtime_collect_and_trim(session->page.runtime);
            else {
                if (!navigation_retire_exhausted_script_realms_mode(
                        session, true, false)) break;
                /* CSS admission may already have skipped sources while
                   memory was tight. Rebuild it with the released headroom,
                   not with an unstyled or fallback-expanded component tree.
                   The wrapper retries once; the retired realm cannot
                   request another retry. Keep the original font owner. */
                session->page.images.retarget = counters.retarget;
                session->style_rebuild_required = true;
                *restart_after_retirement = true;
                return false;
            }
        }
        if (!images_loaded) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "could not rebuild page image resources");
            session->page.loaded = false;
            return navigation_relayout_failed(session, true);
        }
        session->page.images.retarget = counters.retarget;
        session->images = &session->page.images;
        FULL_RELAYOUT_STEP();
    }
    if (!user_css_added && session->user_css_length != 0
        && !stylesheet_add_user_css(&session->page.stylesheet,
                                    session->user_css,
                                    session->user_css_length)) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
    if (!user_css_added)
        stylesheet_share_unchanged_web_fonts(&session->page.stylesheet,
            *retained_fonts, session->maximum_font_face_backend_bytes);
    if (session->web_fonts_enabled && rebuild_url != NULL
        && !navigation_load_web_fonts(session, rebuild_url)) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
    FULL_RELAYOUT_STEP();
#if !defined(__PSP__)
    bool injected_refusal =
        navigation_test_consume_same_document_relayout_refusal();
    if (injected_refusal)
        budget_inject_failure_after(session->budget, 0u);
#endif
    size_t rebuild_failures = session->budget->failure_count;
    size_t rebuild_injections = session->budget->injected_failure_count;
    size_t layout_free = budget_remaining(session->budget);
    if (layout_free < rebuild_reserve)
        (void) images_reclaim_rebuildable_rasters(&session->page.images,
            session->browser_session, rebuild_reserve - layout_free);
    bool built = navigation_build_layout(
        session, &session->page.layout, session->fonts, session->images);
#if !defined(__PSP__)
    if (injected_refusal)
        budget_clear_failure_injection(session->budget);
#endif
    if (!built && !session->layout_build_cancelled
        && session->page.runtime != NULL
        && session->budget->failure_count > rebuild_failures
        && session->budget->injected_failure_count == rebuild_injections) {
        /* The old display list is already gone on this full-rebuild path.
           Reclaim transient VM storage once before refusing a required flow;
           never resurrect its borrowed pre-mutation pointers. */
        (void) script_runtime_collect_and_trim(session->page.runtime);
        layout_reuse_cache_destroy(session->page.layout_reuse);
        session->page.layout_reuse = NULL;
        size_t retry_failures = session->budget->failure_count;
        built = navigation_build_layout(session, &session->page.layout,
                                        session->fonts, session->images);
        if (!built && !session->layout_build_cancelled
            && session->budget->failure_count > retry_failures
            && session->budget->injected_failure_count == rebuild_injections) {
            /* This is a genuine shared-budget refusal after collection,
               not an author-thrown OOM or a synthetic fault. Stop all page
               realms before constructing a new static display list. No
               borrowed commands from the failed update are published. */
            layout_reuse_cache_destroy(session->page.layout_reuse);
            session->page.layout_reuse = NULL;
            if (navigation_retire_exhausted_script_realms(session, true))
                built = navigation_build_layout(session, &session->page.layout,
                                                session->fonts, session->images);
        }
    }
    if (!built) {
        session->page.loaded = false;
        return navigation_relayout_failed(session, true);
    }
    session->resource_fingerprint = fingerprint;
    navigation_note_committed_fingerprint(session);
    session->style_rebuild_required = false;
    session->relayout_damage_valid = true;
    session->relayout_damage_left = 0;
    session->relayout_damage_top = 0;
    session->relayout_damage_right = session->viewport.css_width;
    session->relayout_damage_bottom = old_height > session->page.layout.height
                                      ? old_height : session->page.layout.height;
    session->incremental_relayouts++;
    session->performance.full_relayouts++;
    if (mutations->resource_rebuild_required) {
        session->performance.mutation_resource_rebuilds++;
    }
    FULL_RELAYOUT_STEP();
    session->performance.relayout_us += navigation_record_layout_slices(
        session, started, &session->page.layout);
    navigation_attach_layout(session);
    navigation_clamp_scroll_to_layout(session);
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        /* Stamps: prepare end, [stylesheet built, external CSS loaded,
           stylesheet events dispatched, images loaded,] fonts/user CSS,
           layout built. Without external resources the bracketed stamps
           are absent: the stylesheet span then includes fonts. */
        uint64_t end = navigation_now_us();
        bool external = full_step == 7;
        uint64_t span[7] = {0};
        if (external) {
            for (size_t i = 1; i < 7; i++)
                span[i] = full_steps[i] - full_steps[i - 1];
        } else if (full_step == 3) {
            span[1] = full_steps[1] - full_steps[0];
            span[6] = full_steps[2] - full_steps[1];
        }
        char line[320];
        snprintf(line, sizeof(line),
            "tilefinch-relayout-full-phases: prepare=%lluus stylesheet=%lluus "
            "external-css=%lluus css-events=%lluus images=%lluus "
            "fonts=%lluus layout=%lluus attach=%lluus style-rebuild=%d "
            "resource-rebuild=%d overflow=%d conservative=%d "
            "append-tried=%d",
            (unsigned long long) (full_steps[0] - started),
            (unsigned long long) span[1], (unsigned long long) span[2],
            (unsigned long long) span[3], (unsigned long long) span[4],
            (unsigned long long) span[5], (unsigned long long) span[6],
            (unsigned long long) (full_step == 0 ? 0
                : end - full_steps[full_step - 1]),
            full_style_rebuild ? 1 : 0,
            mutations->resource_rebuild_required ? 1 : 0,
            mutations->overflowed ? 1 : 0,
            plan->conservative_scan ? 1 : 0, plan->style_append_tried ? 1 : 0);
        tilefinch_platform_log_message(line);
    }
#endif
#undef FULL_RELAYOUT_STEP
    return true;
}

static bool navigation_relayout_body(NavigationSession *session,
                                     StyleWebFonts **retained_fonts,
                                     bool *restart_after_retirement)
{
    if (session == NULL || !session->page.loaded) return false;
    tilefinch_platform_trace_step("relayout-prepare");
    session->focus_outline_cache_valid = false;
    session->focus_outline_cache_node = NULL;
    uint64_t started = navigation_now_us();
    const NavigationEntry *entry = navigation_current(session);
    layout_reuse_cache_set_focus_scroll(session->page.layout_reuse,
        entry == NULL ? 0 : entry->scroll_y);
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
    NavigationRelayoutPlan plan =
        navigation_relayout_plan(session, &mutations, mutation_driven);
    if (plan.rebuild_bounded_out_sheet)
        session->style_rebuild_required = true;
    if (plan.pause_image_preloads) {
        /* Detached-image jobs borrow this cascade and share the JPEG lane.
           Requeue them before changing resources, not after the replacement
           is attached: decoder contention is not a failed required layout.
           Child realms share that same process-wide decoder. */
        script_runtime_pause_image_preloads(session->page.runtime);
        for (size_t i = 0; i < session->page.frame_count; i++)
            script_runtime_pause_image_preloads(session->page.frames[i].runtime);
    }
    bool appended_images = false, style_append_done = false;
#if NAVIGATION_RELAYOUT_PHASE_LOG
    const NavigationPerformance *counters = &session->performance;
    uint64_t append_before[5] = {
        counters->style_append_walk_us, counters->style_append_load_us,
        counters->style_append_track_us, counters->style_append_note_us,
        counters->style_append_discovery_us
    };
    uint64_t append_started = navigation_now_us();
#endif
    if (plan.style_append_tried) {
        /* Image causes are settled by the image rebuild; stylesheet causes
           need the sheet part inserted in place. */
        bool sheet_settled = !mutations.stylesheet_rebuild_required;
        if (!sheet_settled
#ifndef TILEFINCH_NO_TRACE
            && getenv("TILEFINCH_DISABLE_STYLE_APPEND") == NULL
#endif
            && navigation_try_append_style_sources(
                   session, &mutations, mutations.image_rebuild_required,
                   &appended_images)) {
            sheet_settled = true;
            style_append_done = true;
        }
        if (sheet_settled) {
            plan.full_rebuild = false;
            plan.image_rebuild = mutations.image_rebuild_required;
        }
    }
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    bool full_style_rebuild = session->style_rebuild_required;
#endif
    plan.image_resource_scan = plan.image_resource_scan
        || (plan.mutation_driven && appended_images);
    /* The refresh lane re-rasterizes the SVG roots the journal names; a
       journal that dropped records cannot name them all. */
    if (mutations.overflowed && plan.image_resource_refresh) plan.image_rebuild = true;
    bool focus_border_paint = false;
    bool local_focus[NAVIGATION_FOCUS_PAINT_CANDIDATE_LIMIT] = {0};
    if (plan.mutation_driven
        && navigation_mutations_are_paint_only_focus(
            session, &mutations, &focus_border_paint, local_focus)) {
        if (focus_border_paint) {
            session->performance.focus_paint_relayout_skips++;
        } else {
            session->performance.focus_outline_relayout_skips++;
        }
        return true;
    }
    if (plan.mutation_driven
        && navigation_mutations_are_semantic_only(session, &mutations)) {
        session->performance.semantic_relayout_skips++;
        return true;
    }
    bool journal_reuse_prepared = false;
    if (plan.mutation_driven
        && navigation_mutations_are_hidden_only(session, &mutations,
                                                &journal_reuse_prepared, local_focus)) {
        session->performance.hidden_relayout_skips++;
        return true;
    }
    uint64_t fingerprint = session->resource_fingerprint;
    if (mutations.overflowed) {
        session->performance.mutation_journal_overflows++;
    }
    if (plan.mutation_driven && plan.conservative_scan) {
        session->performance.mutation_conservative_scans++;
    }
    /* A style append committed a fingerprint of this very DOM. */
    bool fingerprint_kept = plan.mutation_driven && !plan.full_rebuild
        && (style_append_done
            || (!plan.conservative_scan
                && navigation_mutations_keep_resource_fingerprint(
                       &mutations)));
    if ((plan.full_rebuild || plan.conservative_scan || plan.image_resource_scan
         || plan.image_resource_refresh || plan.image_rebuild) && !fingerprint_kept) {
        if (!navigation_resource_fingerprint(
                session, &session->page.document, &fingerprint)) {
            return navigation_relayout_failed(session, plan.mutation_driven);
        }
        /* The journal could not itemize every change. A moved stylesheet
           source needs the sheet rebuilt; moved image elements need only
           the image table. */
        if (!plan.full_rebuild && plan.conservative_scan
            && fingerprint != session->resource_fingerprint) {
            if (plan.mutation_driven && !mutations.retirement_unobserved
                && !navigation_fingerprint_part_changed(session, 0)) {
                plan.image_rebuild = true;
            } else {
                plan.full_rebuild = true;
            }
        }
        /* Overflow describes unknown mutations, not necessarily changed
           CSS. A complete source census lets us keep the admitted cascade
           while updating its adoption scopes. Changed source identity,
           content, policy, order or incomplete admission keeps the rebuild. */
        if (plan.full_rebuild && plan.mutation_driven
            && !session->style_rebuild_required
            && !mutations.retirement_unobserved
            && !navigation_fingerprint_part_changed(session, 2)
            && navigation_rescope_unchanged_sources(session)) {
            plan.full_rebuild = false;
            plan.image_rebuild = true;
            session->page.compiled_stylesheet_cacheable = false;
        }
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL)
        fprintf(stderr, "relayout-decision full=%d style-required=%d "
                "resource-rebuild=%d conservative=%d fingerprint-changed=%d "
                "image-scan=%d image-refresh=%d overflow=%d records=%zu "
                "image-rebuild=%d causes=%d/%d\n",
                (int) plan.full_rebuild, (int) session->style_rebuild_required,
                (int) mutations.resource_rebuild_required,
                (int) plan.conservative_scan,
                (int) (fingerprint != session->resource_fingerprint),
                (int) plan.image_resource_scan, (int) plan.image_resource_refresh,
                (int) mutations.overflowed, mutations.count,
                (int) plan.image_rebuild,
                (int) mutations.stylesheet_rebuild_required,
                (int) mutations.image_rebuild_required);
#endif
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t decided = navigation_now_us();
#endif
    /* Image discovery below reuses computed styles too. Invalidate the old
       hidden ancestry before that traversal, not only before final layout. */
    if (!plan.full_rebuild && !journal_reuse_prepared)
        navigation_prepare_layout_reuse(session, &mutations, false, local_focus);
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t reuse_prepared = navigation_now_us();
    LayoutReuseStats images_before = {0}, images_after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &images_before);
#endif
    if (!plan.full_rebuild && plan.image_rebuild && session->external_resources_enabled
        && !navigation_rebuild_images(session)) {
        /* The table is gone; the full rebuild reloads it with everything. */
        plan.full_rebuild = true;
    }
    if (!plan.full_rebuild && !plan.image_rebuild && session->external_resources_enabled
        && (plan.image_resource_scan || plan.image_resource_refresh)) {
        const char *document_url =
            session->page.document_url[0] == '\0'
            ? (entry == NULL ? NULL : entry->url)
            : session->page.document_url;
        bool images_loaded = document_url != NULL;
        bool scan_for_additions = plan.image_resource_scan;
        if (images_loaded && plan.image_resource_refresh) {
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
                plan.image_rebuild = true;
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
                    || images_refresh_external_nodes_reusing_layout_styles(
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
                    session->browser_session, session->page.layout_reuse,
                    session->fonts, session->viewport.css_width);
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
        if (!images_loaded && !plan.image_rebuild) plan.full_rebuild = true;
        if (plan.image_rebuild && !navigation_rebuild_images(session))
            plan.full_rebuild = true;
    }
#if NAVIGATION_RELAYOUT_PHASE_LOG
    uint64_t images_done = navigation_now_us();
    layout_reuse_cache_stats(session->page.layout_reuse, &images_after);
#endif
    NavigationRelayoutPass pass = {
        .mutations = &mutations, .entry = entry, .started = started,
        .fingerprint = fingerprint,
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        .full_style_rebuild = full_style_rebuild,
#endif
#if NAVIGATION_RELAYOUT_PHASE_LOG
        .append_started = append_started,
        .append_before = {
            append_before[0], append_before[1], append_before[2],
            append_before[3], append_before[4]
        },
        .decided = decided, .reuse_prepared = reuse_prepared,
        .images_done = images_done, .images_before = images_before,
        .images_after = images_after,
#endif
    };
    if (!plan.full_rebuild)
        return navigation_relayout_incremental(session, &plan, &pass);
    return navigation_relayout_full(session, &plan, &pass, retained_fonts,
                                    restart_after_retirement);
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
    navigation_prepare_layout_reuse(session, NULL, true, NULL);
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

static NavigationUserCssTransaction *navigation_apply_presentation_css_begin(
    NavigationSession *session, const char *css, size_t length,
    bool allow_author_fallback)
{
    if (session == NULL || session->budget == NULL || !session->page.loaded
        || (css == NULL && length != 0) || length > 128u * 1024u)
        return NULL;
    /* Scroll anchoring below reads the reader's text anchor out of the
       page's layout: a bounded focus layout is completed first. */
    if (navigation_bounded_focus_region_end(session) != 0
        && !navigation_complete_layout_sync(session)) return NULL;
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
    /* Changing presentation is not a new navigation. Keep validated response
       bytes and redirect/referrer provenance even after the HTTP cache evicts
       them, without mutating the incumbent ledger needed by rollback. */
    bool okay = !session->external_resources_enabled
        || stylesheet_document_resources_copy_for_rebuild(
               &next_resources, &session->page.stylesheet_resources);
    if (okay) okay = session->external_resources_enabled
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
        if (okay && !allow_author_fallback) {
            /* Optional re-sampling must not trade existing author styling
               for a pressure-skipped response in the replacement sheet. */
            for (size_t i = 0; i < session->page.stylesheet_resources.count; i++) {
                if (session->page.stylesheet_resources.items[i].rules_applied
                    && (i >= next_resources.count
                        || !next_resources.items[i].rules_applied)) {
                    okay = false;
                    break;
                }
            }
        }
        if (!okay && allow_author_fallback) {
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
    if (okay && session->web_fonts_enabled
        && session->web_font_mode != WEB_FONTS_OFF)
        stylesheet_share_unchanged_web_fonts(&next_sheet,
            session->page.stylesheet.web_fonts, session->maximum_font_face_backend_bytes);
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
    /* The incumbent sheet resolves nothing while the transaction is open,
       and the reclaim hook cannot reach it here: free its style-attribute
       memo now (a rollback rebuilds it on first use). */
    (void) stylesheet_drop_inline_scans(&transaction->previous_stylesheet);
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

NavigationUserCssTransaction *navigation_apply_user_css_begin(
    NavigationSession *session, const char *css, size_t length)
{
    return navigation_apply_presentation_css_begin(session, css, length, true);
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
