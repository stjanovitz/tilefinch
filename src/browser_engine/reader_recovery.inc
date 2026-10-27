/* Reader and Basic views, blank-page and Basic-view recovery, and the
   declared media card. */

static bool browser_engine_reader_root_matches(
    const PocDocument *document, lxb_dom_node_t *root, ReaderPageKind kind)
{
    if (document == NULL || root == NULL || kind == READER_PAGE_RAW
        || root->type != LXB_DOM_NODE_TYPE_ELEMENT
        || root->parent != document_body_node(document)) return false;
    size_t name_length = 0;
    const char *name = document_element_name(root, &name_length);
    size_t marker_length = 0;
    const char *marker = document_attribute(
        root, "data-tilefinch-reader-root", &marker_length);
    const char *expected = reader_page_kind_name(kind);
    size_t expected_length = strlen(expected);
    return name != NULL && name_length == 4u
        && strncasecmp(name, "main", 4u) == 0
        && marker != NULL && marker_length == expected_length
        && memcmp(marker, expected, expected_length) == 0;
}

/* Discover only the root just appended by the synchronous native extractor.
   Once that identity is retained, every later lookup goes through the stored
   pointer/weak handle and never searches an author-controlled marker. */
static lxb_dom_node_t *browser_engine_discover_reader_root(
    PocDocument *document, ReaderPageKind kind)
{
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_node_t *root = body == NULL ? NULL : body->last_child;
    return browser_engine_reader_root_matches(document, root, kind)
        ? root : NULL;
}

static bool browser_engine_retain_reader_root(
    NavigationPage *page, lxb_dom_node_t *root, ReaderPageKind kind)
{
    if (page == NULL || root == NULL) return false;
    long handle = page->runtime == NULL ? 0
        : script_runtime_node_weak_handle(page->runtime, root);
    if (page->runtime != NULL && handle == 0) return false;
    page->reader_root = root;
    page->reader_root_kind = kind;
    page->reader_root_handle = handle;
    return true;
}

static lxb_dom_node_t *browser_engine_resolve_reader_root(
    NavigationPage *page, ReaderPageKind kind)
{
    if (page == NULL || page->reader_root == NULL
        || page->reader_root_kind != kind) return NULL;
    lxb_dom_node_t *root = page->runtime == NULL
        ? (page->reader_root_handle == 0 ? page->reader_root : NULL)
        : (page->reader_root_handle == 0 ? NULL
            : script_runtime_node_handle_resolve_connected(
                  page->runtime, page->reader_root_handle));
    return root == page->reader_root
        && browser_engine_reader_root_matches(&page->document, root, kind)
        ? root : NULL;
}

static lxb_dom_node_t *browser_engine_establish_reader_root(
    NavigationPage *page, ReaderPageKind kind)
{
    lxb_dom_node_t *root = browser_engine_resolve_reader_root(page, kind);
    if (root != NULL) return root;
    /* Commit-time candidates are deliberately kept outside the author DOM
       while delayed hydration settles. Their weak handle reserves exact
       provenance before any author turn; connect only that same private root
       at the native activation boundary. */
    if (page != NULL && page->reader_root != NULL
        && page->reader_root_kind == kind
        && page->reader_root->parent == NULL) {
        lxb_dom_node_t *retained = page->runtime == NULL
            ? (page->reader_root_handle == 0 ? page->reader_root : NULL)
            : (page->reader_root_handle == 0 ? NULL
                : script_runtime_node_handle_resolve(
                      page->runtime, page->reader_root_handle));
        if (retained == page->reader_root
            && reader_document_connect_prepared_view(
                   &page->document, retained, kind)) {
            return browser_engine_resolve_reader_root(page, kind);
        }
        return NULL;
    }
    /* A nonempty provenance record that no longer resolves means that exact
       native root was detached or altered. Fail closed; never let a public
       marker elsewhere in the author DOM replace it. */
    if (page == NULL || page->reader_root != NULL
        || page->reader_root_handle != 0
        || page->reader_root_kind != READER_PAGE_RAW) return NULL;
    root = browser_engine_discover_reader_root(&page->document, kind);
    if (root == NULL) return NULL;
    /* Callers reserve handle capacity before the extractor mutates the DOM.
       Registration uses a fixed bridge table and cannot allocate, so failure
       here indicates an invalid lifecycle rather than a recoverable partial
       admission. Do not publish any provenance in that case. */
    return browser_engine_retain_reader_root(page, root, kind)
        ? root : NULL;
}

typedef enum {
    BROWSER_READER_ALIAS_PREPARE = 0,
    BROWSER_READER_ALIAS_DEFERRED
} BrowserReaderAliasPhase;

static void browser_engine_rebind_reader_visual_images(
    TileCache *cache, bool ready, const LayoutDocument *source_layout,
    const ImageAliasResult *alias_result, const ImageResources *images)
{
    if (cache == NULL || !ready || source_layout == NULL
        || alias_result == NULL || images == NULL
        || !cache->owns_visual_layout
        || cache->source_layout != source_layout
        || cache->layout != &cache->visual_layout) return;
    (void) layout_rebind_image_resources(
        &cache->visual_layout, (uintptr_t) alias_result->previous_items,
        alias_result->previous_count, images);
}

static void browser_engine_alias_reader_images(
    BrowserEngine *engine, NavigationSession *navigation,
    lxb_dom_node_t *reader_root, BrowserReaderAliasPhase phase)
{
    if (engine == NULL || navigation == NULL || reader_root == NULL) return;
    NavigationPage *page = &navigation->page;
    if (page->images.count != 0u) {
        ImageAliasResult alias_result =
            images_alias_existing_document_subtree(
                &page->stylesheet, &page->images, reader_root,
                page->resource_base_url, page->document_url,
                page->referrer_policy,
                engine->config.resources.maximum_images);
        if (alias_result.previous_items != NULL) {
            (void) layout_rebind_image_resources(
                &page->layout, (uintptr_t) alias_result.previous_items,
                alias_result.previous_count, &page->images);
            /* A scaled viewport keeps a cloned display list. Relocate only
               caches tied to this exact source layout; an in-flight candidate
               normally owns unrelated image resources. */
            browser_engine_rebind_reader_visual_images(
                &engine->render, engine->render_ready, &page->layout,
                &alias_result, &page->images);
            browser_engine_rebind_reader_visual_images(
                &engine->candidate_render, engine->candidate_render_ready,
                &page->layout, &alias_result, &page->images);
        }
        if (alias_result.aliased != 0u
            && phase == BROWSER_READER_ALIAS_PREPARE
            && page->deferred_image_job != NULL) {
            /* Preparation aliases only resources whose layout transaction is
               already committed. A deferred completion instead advances this
               boundary after its immediately-following relayout succeeds. */
            images_priority_load_commit_progress(
                page->deferred_image_job);
        }
        images_alias_result_release(&page->images, &alias_result);
        layout_reuse_cache_update_images(
            page->layout_reuse, &page->images);
    }
    if (phase == BROWSER_READER_ALIAS_PREPARE) {
        /* The Reader clone is a trusted native DOM mutation. Deferred alias
           publication changes only the resource table, so it does not need
           another document fingerprint scan. */
        (void) navigation_accept_native_resource_fingerprint(navigation);
    }
}

static void browser_engine_image_table_moved(
    void *opaque, NavigationSession *navigation, uintptr_t previous_base,
    size_t previous_count)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || navigation == NULL) return;
    TileCache *caches[] = {&engine->render, &engine->candidate_render};
    for (size_t i = 0; i < sizeof(caches) / sizeof(caches[0]); i++) {
        TileCache *cache = caches[i];
        if (cache->owns_visual_layout
            && cache->source_layout == &navigation->page.layout
            && cache->layout == &cache->visual_layout)
            (void) layout_rebind_image_resources(&cache->visual_layout,
                previous_base, previous_count, &navigation->page.images);
    }
}

static void browser_engine_deferred_reader_images_ready(
    void *opaque, NavigationSession *navigation)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || navigation == NULL
        || !navigation->page.reader_analysis.prepared
        || navigation->page.reader_analysis.kind == READER_PAGE_RAW) return;
    lxb_dom_node_t *reader_root = browser_engine_resolve_reader_root(
        &navigation->page, navigation->page.reader_analysis.kind);
    browser_engine_alias_reader_images(
        engine, navigation, reader_root, BROWSER_READER_ALIAS_DEFERRED);
}

bool browser_engine_prepare_reader(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (analysis != NULL) *analysis = (ReaderDocumentAnalysis) {0};
    if (engine == NULL || !engine->navigation.page.loaded) return false;
    ReaderDocumentAnalysis *stored =
        &engine->navigation.page.reader_analysis;
    /* Basic and Reader share one hidden extracted-root slot. Never reinterpret
       a prepared Basic tree as a Reader article or append a second clone. */
    if (stored->prepared && stored->kind == READER_PAGE_BASIC) {
        if (analysis != NULL) *analysis = *stored;
        return false;
    }
    bool prepared_now = !stored->prepared;
    if (prepared_now
        && !script_runtime_node_handle_capacity_available(
               engine->navigation.page.runtime)) {
        ReaderDocumentAnalysis refused = {
            .prepared = true, .bounded_out = true
        };
        if (analysis != NULL) *analysis = refused;
        return false;
    }
    if (prepared_now
        && !reader_document_prepare_with_layout(
               &engine->navigation.page.document,
               &engine->navigation.page.stylesheet,
               &engine->navigation.page.layout, stored)) return false;
    lxb_dom_node_t *reader_root = stored->kind != READER_PAGE_RAW
        ? browser_engine_establish_reader_root(
              &engine->navigation.page, stored->kind)
        : NULL;
    if (prepared_now && stored->kind != READER_PAGE_RAW
        && reader_root == NULL) {
        ReaderPageKind failed_kind = stored->kind;
        (void) reader_document_discard_prepared_view(
            &engine->navigation.page.document, failed_kind);
        *stored = (ReaderDocumentAnalysis) {0};
    }
    if (reader_root != NULL) {
        browser_engine_alias_reader_images(
            engine, &engine->navigation, reader_root,
            BROWSER_READER_ALIAS_PREPARE);
    }
    if (analysis != NULL) *analysis = *stored;
    /* Preparation is also the admission boundary used by frontends before
       applying Reader CSS. A successfully analyzed RAW page has no semantic
       Reader tree; report that as unavailable so a manual request cannot
       broadly restyle an error, login, or application shell. The stored
       analysis remains available through browser_engine_reader_analysis(). */
    return stored->kind != READER_PAGE_RAW && reader_root != NULL;
}

bool browser_engine_analyze_reader(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (analysis != NULL) *analysis = (ReaderDocumentAnalysis) {0};
    if (engine == NULL || analysis == NULL
        || !engine->navigation.page.loaded) return false;
    /* A connected extracted root is already authoritative. Report its stored
       analysis without rewalking source; otherwise keep this probe strictly
       temporary so low-confidence auto mode cannot block a later Basic view. */
    const ReaderDocumentAnalysis *stored =
        &engine->navigation.page.reader_analysis;
    if (stored->prepared && stored->kind != READER_PAGE_RAW) {
        *analysis = *stored;
        return browser_engine_establish_reader_root(
                   &engine->navigation.page, stored->kind) != NULL;
    }
    return reader_document_analyze_with_stylesheet(
        &engine->navigation.page.document,
        &engine->navigation.page.stylesheet, analysis);
}

bool browser_engine_prepare_basic_view(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (analysis != NULL) *analysis = (ReaderDocumentAnalysis) {0};
    if (engine == NULL || !engine->navigation.page.loaded) return false;
    ReaderDocumentAnalysis *stored =
        &engine->navigation.page.reader_analysis;
    if (stored->prepared && stored->kind == READER_PAGE_BASIC) {
        lxb_dom_node_t *root = browser_engine_resolve_reader_root(
            &engine->navigation.page, READER_PAGE_BASIC);
        if (analysis != NULL) *analysis = *stored;
        return root != NULL;
    }
    /* An installed Reader tree owns the one extracted presentation slot.
       Switching back to raw CSS is reversible; creating two hidden clones is
       deliberately not. */
    if (stored->prepared && stored->kind != READER_PAGE_RAW) {
        if (analysis != NULL) *analysis = *stored;
        return false;
    }
    if (!script_runtime_node_handle_capacity_available(
            engine->navigation.page.runtime)) {
        if (analysis != NULL) {
            *analysis = (ReaderDocumentAnalysis) {
                .prepared = true, .bounded_out = true
            };
        }
        return false;
    }
    ReaderDocumentAnalysis prepared = {0};
    if (!reader_document_prepare_basic_complete_with_layout(
            &engine->navigation.page.document,
            &engine->navigation.page.stylesheet,
            script_runtime_has_pending_mutations(engine->navigation.page.runtime)
                ? NULL : &engine->navigation.page.layout, &prepared)) {
        if (analysis != NULL) *analysis = prepared;
        return false;
    }
    if (analysis != NULL) *analysis = prepared;
    /* A Basic view shortened by its emit bound is admitted (and labeled);
       an omitted action is not. */
    if (prepared.kind != READER_PAGE_BASIC || prepared.bounded_out)
        return false;
    lxb_dom_node_t *root = browser_engine_establish_reader_root(
        &engine->navigation.page, READER_PAGE_BASIC);
    if (root == NULL) {
        (void) reader_document_discard_prepared_view(
            &engine->navigation.page.document, READER_PAGE_BASIC);
        return false;
    }
    *stored = prepared;
    browser_engine_alias_reader_images(
        engine, &engine->navigation, root,
        BROWSER_READER_ALIAS_PREPARE);
    return true;
}

static bool browser_engine_activate_extracted_view(
    BrowserEngine *engine, ReaderPageKind kind)
{
    if (engine == NULL || !engine->navigation.page.loaded) return false;
    NavigationPage *page = &engine->navigation.page;
    if (!page->reader_analysis.prepared
        || page->reader_analysis.kind != kind) return false;
    lxb_dom_node_t *root = browser_engine_resolve_reader_root(page, kind);
    if (root == NULL) return false;
    /* Freeze only after frontend CSS has succeeded so a failed presentation
       transaction leaves a functioning raw page and realm untouched. No
       author turn occurs between synchronous prepare, CSS application, and
       this exact-root validation. This makes the native clone's :last-child
       presentation and delegated actions stable for both Reader and Basic. */
    navigation_retire_current_page_scripts(&engine->navigation);
    bool realms_retired = page->runtime == NULL;
    for (size_t frame = 0; frame < page->frame_count; frame++)
        realms_retired = realms_retired
            && page->frames[frame].runtime == NULL;
    return realms_retired && page->reader_root == root
        && page->reader_root_handle == 0 && page->reader_root_kind == kind;
}

bool browser_engine_activate_reader_view(BrowserEngine *engine)
{
    if (engine == NULL) return false;
    ReaderPageKind kind = engine->navigation.page.reader_analysis.kind;
    if (kind != READER_PAGE_ARTICLE && kind != READER_PAGE_LISTING
        && kind != READER_PAGE_WATCH) return false;
    return browser_engine_activate_extracted_view(engine, kind);
}

bool browser_engine_activate_basic_view(BrowserEngine *engine)
{
    return browser_engine_activate_extracted_view(
        engine, READER_PAGE_BASIC);
}

bool browser_engine_page_is_visually_blank(const BrowserEngine *engine)
{
    return engine != NULL && engine->state == BROWSER_ENGINE_ACTIVE
        && engine->navigation_ready
        && engine->navigation.page.loaded
        && navigation_layout_is_visually_blank(
               &engine->navigation.page.layout);
}

static bool browser_engine_has_script_degradation(
    const NavigationSession *navigation)
{
    if (navigation == NULL || !navigation->page.loaded) return false;
    const ScriptResult *result = &navigation->page.script_result;
    bool heap_rejected = navigation->page.runtime != NULL
        && script_runtime_heap_rejections(navigation->page.runtime) != 0u;
    /* A realm retired by navigation_retire_exhausted_script_realms no longer
       exposes its allocator counter; that retirement sets
       script_degradation_observed. */
    return navigation->page.script_degradation_observed
        || result->scripts_failed != 0u
        || result->external_scripts_failed != 0u
        || result->dynamic_scripts_failed != 0u
        || result->dynamic_scripts_quota_rejected != 0u
        || result->host_compile_rejections != 0u
        || result->host_compile_watchdog_aborts != 0u
        || result->uncaught_callback_errors != 0u
        || result->interrupted || heap_rejected;
}

static bool browser_engine_recovery_wall_settled(
    const BrowserEngine *engine)
{
    if (engine == NULL || engine->navigation.page.committed_us == 0u)
        return false;
    uint64_t now_us = browser_engine_recovery_now_us();
    return now_us >= engine->navigation.page.committed_us
        && now_us - engine->navigation.page.committed_us
               >= BROWSER_RECOVERY_SETTLE_US;
}

static bool browser_engine_media_node_name_is(
    lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0u;
    const char *name = document_element_name(node, &length);
    return name != NULL && length == strlen(wanted)
        && strncasecmp(name, wanted, length) == 0;
}

static lxb_dom_node_t *browser_engine_media_walk_next(
    lxb_dom_node_t *root, lxb_dom_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != NULL && node != root) {
        if (node->next != NULL) return node->next;
        node = node->parent;
    }
    return NULL;
}

static bool browser_engine_media_card_connected(
    BrowserEngine *engine, lxb_dom_node_t *wanted)
{
    if (engine == NULL || wanted == NULL) return false;
    PocDocument *document = &engine->navigation.page.document;
    lxb_dom_node_t *root = document->html == NULL ? NULL
        : lxb_dom_interface_node(document->html);
    /* Exact provenance makes a bounded ancestry check sufficient. Unlike a
       prefix DOM walk, it still recognizes the one native card if author
       mutation moves it behind more than 16K siblings. */
    size_t depth = 0u;
    for (lxb_dom_node_t *node = wanted;
         node != NULL && depth++ < 16384u; node = node->parent) {
        if (node == root) return true;
    }
    return false;
}

static bool browser_engine_media_set_attribute(
    lxb_dom_node_t *node, const char *name, const char *value)
{
    return node != NULL && name != NULL && value != NULL
        && lxb_dom_element_set_attribute(
               lxb_dom_interface_element(node),
               (const lxb_char_t *) name, strlen(name),
               (const lxb_char_t *) value, strlen(value)) != NULL;
}

static lxb_dom_node_t *browser_engine_find_declared_media_anchor(
    BrowserEngine *engine, const MediaDeclaredVideo *declared,
    bool *author_video)
{
    if (author_video != NULL) *author_video = false;
    if (engine == NULL || declared == NULL) return NULL;
    PocDocument *document = &engine->navigation.page.document;
    lxb_dom_node_t *root = document->html == NULL ? NULL
        : lxb_dom_interface_node(document->html);
    if (root == NULL) return NULL;
    char declared_thumbnail[NAVIGATION_URL_LIMIT];
    bool have_thumbnail = declared->thumbnail_url[0] != '\0'
        && fetch_resolve_url(
               engine->navigation.page.resource_base_url,
               declared->thumbnail_url, declared_thumbnail,
               sizeof(declared_thumbnail));
    lxb_dom_node_t *matching_image = NULL;
    size_t nodes = 0u;
    for (lxb_dom_node_t *node = root;
         node != NULL && nodes++ < 16384u;
         node = browser_engine_media_walk_next(root, node)) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (browser_engine_media_node_name_is(node, "video")) {
            if (author_video != NULL) *author_video = true;
            return node;
        }
        if (!have_thumbnail || matching_image != NULL
            || !browser_engine_media_node_name_is(node, "img")) continue;
        const LayoutNodeBox *box = layout_box_for_node(
            &engine->navigation.page.layout, node);
        if (box == NULL || box->width <= 0 || box->height <= 0) continue;
        size_t source_length = 0u;
        const char *source = document_attribute(node, "src", &source_length);
        if (source == NULL || source_length == 0u
            || source_length >= NAVIGATION_URL_LIMIT) continue;
        char raw[NAVIGATION_URL_LIMIT];
        char resolved[NAVIGATION_URL_LIMIT];
        memcpy(raw, source, source_length);
        raw[source_length] = '\0';
        if (fetch_resolve_url(
                engine->navigation.page.resource_base_url, raw,
                resolved, sizeof(resolved))
            && strcmp(resolved, declared_thumbnail) == 0) {
            matching_image = node;
        }
    }
    return matching_image;
}

static void browser_engine_remove_media_card(
    lxb_dom_node_t *card, lxb_dom_node_t *wrapped_image)
{
    if (card == NULL) return;
    lxb_dom_node_t *parent = card->parent;
    if (wrapped_image != NULL && parent != NULL) {
        if (wrapped_image->parent == card) lxb_dom_node_remove(wrapped_image);
        (void) lxb_dom_node_insert_before_spec(parent, wrapped_image, card);
    }
    if (card->parent != NULL) lxb_dom_node_remove(card);
    lxb_dom_node_destroy_deep(card);
}

static bool browser_engine_commit_media_card(
    BrowserEngine *engine, lxb_dom_node_t *card,
    lxb_dom_node_t *wrapped_image)
{
    PocDocument *document = &engine->navigation.page.document;
    NavigationPage *page = &engine->navigation.page;
    /* Publish exact native provenance before layout observes the new node.
       The public marker remains useful for diagnostics and authored CSS, but
       is never an authority boundary. */
    document->declared_video_card_node = card;
    bool shell_was_ready = engine->render_ready;
    bool document_refreshed = document_refresh(document);
    if (document_refreshed && wrapped_image == NULL) {
        lxb_dom_node_t *nodes[] = {card};
        /* Poster refusal is soft: the browser-colored card and play glyph
           remain useful. A successful exact-node refresh is still charged to
           the ordinary page image budget and policy pipeline. */
        if (images_refresh_external_nodes(
                document, &page->stylesheet, &page->images,
                nodes, 1u, engine->navigation.budget,
                page->resource_base_url, page->document_url,
                page->referrer_policy, engine->navigation.maximum_images,
                engine->navigation.maximum_image_bytes,
                engine->navigation.maximum_image_file_bytes,
                engine->navigation.maximum_decoded_image_bytes,
                engine->navigation.resource_timeout_ms,
                page->resource_scheduler,
                engine->navigation.browser_session)) {
            engine->navigation.images = &page->images;
            layout_reuse_cache_update_images(
                page->layout_reuse, &page->images);
        }
    }
    bool fingerprint_accepted = document_refreshed
        && navigation_accept_native_resource_fingerprint(
               &engine->navigation);
    bool layout_adopted = fingerprint_accepted
        && navigation_relayout(&engine->navigation);
    if (!layout_adopted) {
        lxb_dom_node_t *nodes[] = {card};
        images_discard_nodes(&page->images, nodes, 1u);
        layout_reuse_cache_update_images(
            page->layout_reuse, &page->images);
        if (document->declared_video_card_node == card)
            document->declared_video_card_node = NULL;
        browser_engine_remove_media_card(card, wrapped_image);
        (void) document_refresh(document);
        (void) navigation_accept_native_resource_fingerprint(
            &engine->navigation);
        (void) navigation_relayout(&engine->navigation);
        if (shell_was_ready)
            (void) browser_engine_refresh_render_shell(engine);
        return false;
    }
    if (shell_was_ready && !browser_engine_refresh_render_shell(engine)) {
        /* The authoritative layout already contains card/node pointers. Never
           destroy the card and then attempt a fallible replacement layout:
           under the same pressure that refused the shell that would retain
           stale DOM pointers. Retire the page as one ownership unit. */
        engine->declared_media_card = NULL;
        engine->declared_media_card_generation = 0u;
        browser_engine_reset_shell(engine);
        navigation_discard_current_page(&engine->navigation);
        return false;
    }
    if (engine->controller_ready)
        (void) controller_rebind_focus(&engine->controller);
    return true;
}

BrowserDeclaredMediaCard browser_engine_prepare_declared_media_card(
    BrowserEngine *engine)
{
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready || !engine->navigation.page.loaded)
        return BROWSER_DECLARED_MEDIA_CARD_NONE;
    if (engine->declared_media_card_generation
            == engine->navigation.generation
        && engine->navigation.page.document.declared_video_card_node
               == engine->declared_media_card
        && browser_engine_media_card_connected(
               engine, engine->declared_media_card))
        return BROWSER_DECLARED_MEDIA_CARD_INSTALLED;
    if (engine->navigation.page.document.declared_video_card_node
            == engine->declared_media_card) {
        engine->navigation.page.document.declared_video_card_node = NULL;
    }
    engine->declared_media_card = NULL;
    engine->declared_media_card_generation = 0u;
    PocDocument *document = &engine->navigation.page.document;
    if (engine->declared_media_outcome_generation
            == engine->navigation.generation
        && engine->declared_media_outcome_content_generation
               == document->content_generation
        && (engine->declared_media_outcome
                == BROWSER_DECLARED_MEDIA_CARD_NONE
            || engine->declared_media_outcome
                == BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE)) {
        return engine->declared_media_outcome;
    }
    if (!browser_engine_has_script_degradation(&engine->navigation)) {
        engine->declared_media_recovery_generation = 0u;
        return BROWSER_DECLARED_MEDIA_CARD_NONE;
    }
    if (script_runtime_has_pending_author_work(
            engine->navigation.page.runtime)) {
        const uint64_t generation = engine->navigation.generation;
        const size_t runtime_tick =
            engine->navigation.page.script_result.runtime_ticks;
        if (engine->declared_media_recovery_generation != generation) {
            engine->declared_media_recovery_generation = generation;
            engine->declared_media_recovery_first_runtime_tick = runtime_tick;
            /* Basic and media-card recovery inspect the same unsettled page.
               Let the first observer seed both clocks so an undeclared page
               does not pay two consecutive settle windows. */
            if (engine->basic_view_recovery_generation != generation) {
                engine->basic_view_recovery_generation = generation;
                engine->basic_view_recovery_first_runtime_tick = runtime_tick;
            }
        }
        size_t first_tick =
            engine->declared_media_recovery_first_runtime_tick;
        size_t settled_ticks = runtime_tick >= first_tick
            ? runtime_tick - first_tick : BROWSER_BLANK_READER_SETTLE_TICKS;
        if (settled_ticks < BROWSER_BLANK_READER_SETTLE_TICKS)
            return BROWSER_DECLARED_MEDIA_CARD_DEFERRED;
    }
    if (!browser_engine_recovery_wall_settled(engine))
        return BROWSER_DECLARED_MEDIA_CARD_DEFERRED;

    /* Discovery and resolved-thumbnail walks are deliberately last. During
       the settle window this function is called once per dirty frame, so
       even a cached first scan would be wasted work on a page whose author
       runtime may still recover. */
    const MediaDeclaredVideo *declared = media_declared_video_cached(
        &engine->navigation.page.document);
    if (declared == NULL) {
        engine->declared_media_outcome_generation =
            engine->navigation.generation;
        engine->declared_media_outcome_content_generation =
            document->content_generation;
        engine->declared_media_outcome = BROWSER_DECLARED_MEDIA_CARD_NONE;
        return BROWSER_DECLARED_MEDIA_CARD_NONE;
    }
    bool author_video = false;
    lxb_dom_node_t *anchor = browser_engine_find_declared_media_anchor(
        engine, declared, &author_video);
    if (author_video) {
        engine->declared_media_outcome_generation =
            engine->navigation.generation;
        engine->declared_media_outcome_content_generation =
            document->content_generation;
        engine->declared_media_outcome = BROWSER_DECLARED_MEDIA_CARD_NONE;
        return BROWSER_DECLARED_MEDIA_CARD_NONE;
    }

    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL) {
        engine->declared_media_outcome_generation =
            engine->navigation.generation;
        engine->declared_media_outcome_content_generation =
            document->content_generation;
        engine->declared_media_outcome = BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE;
        return BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE;
    }
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    const char *tag = anchor == NULL ? "video" : "div";
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, (const lxb_char_t *) tag,
        strlen(tag), NULL);
    lxb_dom_node_t *card = element == NULL ? NULL
        : lxb_dom_interface_node(element);
    bool okay = card != NULL
        && browser_engine_media_set_attribute(
               card, "data-tilefinch-declared-media-card",
               anchor == NULL ? "synthetic" : "thumbnail")
        && browser_engine_media_set_attribute(card, "role", "button")
        && browser_engine_media_set_attribute(
               card, "aria-label",
               declared->title[0] == '\0'
                   ? "Play declared video in Tilefinch"
                   : declared->title)
        && browser_engine_media_set_attribute(
               card, "title",
               declared->title[0] == '\0'
                   ? "Play video" : declared->title);
    if (okay && declared->duration[0] != '\0')
        okay = browser_engine_media_set_attribute(
            card, "data-tilefinch-media-duration", declared->duration);
    if (okay && anchor == NULL) {
        okay = browser_engine_media_set_attribute(card, "controls", "controls")
            && browser_engine_media_set_attribute(
                card, "style",
                "display:block;width:100%;height:135px;max-height:50vh;"
                "box-sizing:border-box;margin:8px 0;border:2px solid #547696;"
                "border-radius:8px;background:#162431")
            && (declared->thumbnail_url[0] == '\0'
                || browser_engine_media_set_attribute(
                       card, "poster", declared->thumbnail_url));
        if (okay) okay = body->first_child == NULL
            ? lxb_dom_node_append_child(body, card) == LXB_DOM_EXCEPTION_OK
            : lxb_dom_node_insert_before_spec(
                  body, card, body->first_child) == LXB_DOM_EXCEPTION_OK;
    } else if (okay) {
        lxb_dom_node_t *parent = anchor->parent;
        okay = parent != NULL
            && browser_engine_media_set_attribute(
                card, "style",
                "display:inline-block;position:relative;max-width:100%;"
                "box-sizing:border-box;border:2px solid #547696;"
                "border-radius:8px;background:#162431")
            && lxb_dom_node_insert_before_spec(parent, card, anchor)
                   == LXB_DOM_EXCEPTION_OK
            && lxb_dom_node_append_child(card, anchor)
                   == LXB_DOM_EXCEPTION_OK;
    }
    if (!okay && card != NULL)
        browser_engine_remove_media_card(card, anchor);
    document_allocation_owner_leave(document, previous);
    if (!okay || !browser_engine_commit_media_card(engine, card, anchor)) {
        /* Allocation, mutation-journal and shell preparation refusals are
           transient.  Only a document-derived absence (such as no body)
           may latch UNAVAILABLE for this content generation. */
        return BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE;
    }
    engine->declared_media_card = card;
    engine->declared_media_card_generation = engine->navigation.generation;
    engine->declared_media_outcome_generation = engine->navigation.generation;
    engine->declared_media_outcome_content_generation =
        document->content_generation;
    engine->declared_media_outcome = BROWSER_DECLARED_MEDIA_CARD_INSTALLED;
    return BROWSER_DECLARED_MEDIA_CARD_INSTALLED;
}

BrowserBlankReaderRecovery browser_engine_prepare_blank_reader_recovery(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (analysis != NULL) *analysis = (ReaderDocumentAnalysis) {0};
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || !browser_engine_page_is_visually_blank(engine)
        || !browser_engine_has_script_degradation(&engine->navigation)) {
        if (engine != NULL) engine->blank_reader_recovery_generation = 0u;
        return BROWSER_BLANK_READER_RECOVERY_NONE;
    }
    if (script_runtime_has_pending_author_work(
            engine->navigation.page.runtime)) {
        const uint64_t generation = engine->navigation.generation;
        const size_t runtime_tick =
            engine->navigation.page.script_result.runtime_ticks;
        if (engine->blank_reader_recovery_generation != generation) {
            engine->blank_reader_recovery_generation = generation;
            engine->blank_reader_recovery_first_runtime_tick = runtime_tick;
        }
        const size_t first_tick =
            engine->blank_reader_recovery_first_runtime_tick;
        const size_t settled_ticks = runtime_tick >= first_tick
            ? runtime_tick - first_tick : BROWSER_BLANK_READER_SETTLE_TICKS;
        /* Give delayed hydration an ordinary bounded settle window. A
           permanent interval or self-scheduling animation must not suppress
           both semantic recovery and the native recovery surface forever. */
        if (settled_ticks < BROWSER_BLANK_READER_SETTLE_TICKS)
            return BROWSER_BLANK_READER_RECOVERY_DEFERRED;
    }
    /* Runtime ticks and elapsed wall time are independent settle evidence;
       accumulate them concurrently rather than making a recurring page wait
       one complete window and only then begin the other. */
    if (!browser_engine_recovery_wall_settled(engine))
        return BROWSER_BLANK_READER_RECOVERY_DEFERRED;
    engine->blank_reader_recovery_generation = 0u;
    ReaderDocumentAnalysis *stored =
        &engine->navigation.page.reader_analysis;
    if (!stored->prepared
        && !script_runtime_node_handle_capacity_available(
               engine->navigation.page.runtime)) {
        if (analysis != NULL) {
            *analysis = (ReaderDocumentAnalysis) {
                .prepared = true, .bounded_out = true
            };
        }
        return BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE;
    }
    if (!stored->prepared
        && !reader_document_prepare_complete_with_layout(
               &engine->navigation.page.document,
               &engine->navigation.page.stylesheet,
               &engine->navigation.page.layout, stored)) {
        return BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE;
    }
    ReaderDocumentAnalysis prepared = *stored;
    if (prepared.bounded_out || prepared.extraction_truncated) {
        if (analysis != NULL) *analysis = prepared;
        /* Strict automatic recovery deliberately installed no tree. Do not
           let that analysis-only result suppress a later explicit Reader
           request, whose bounded/truncated presentation is still useful and
           visibly labeled as shortened. */
        *stored = (ReaderDocumentAnalysis) {0};
        return BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE;
    }
    lxb_dom_node_t *reader_root = prepared.kind == READER_PAGE_RAW
        ? NULL : browser_engine_establish_reader_root(
              &engine->navigation.page, prepared.kind);
    bool available = reader_root != NULL;
    if (prepared.kind != READER_PAGE_RAW && reader_root == NULL) {
        (void) reader_document_discard_prepared_view(
            &engine->navigation.page.document, prepared.kind);
        *stored = (ReaderDocumentAnalysis) {0};
    }
    if (reader_root != NULL) {
        browser_engine_alias_reader_images(
            engine, &engine->navigation, reader_root,
            BROWSER_READER_ALIAS_PREPARE);
    }
    if (analysis != NULL) *analysis = prepared;
    return available ? BROWSER_BLANK_READER_RECOVERY_AVAILABLE
                     : BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE;
}

static bool browser_engine_has_visible_action_target(
    const BrowserEngine *engine)
{
    if (engine == NULL || !engine->navigation.page.loaded) return false;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    for (size_t i = 0; i < layout->link_count; i++) {
        const LinkRegion *link = &layout->links[i];
        if (link->node != NULL && link->width > 0 && link->height > 0)
            return true;
    }
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        if (control->node != NULL
            && control->width > 0 && control->height > 0) return true;
    }
    return false;
}

BrowserBasicViewRecovery browser_engine_prepare_basic_view_recovery(
    BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (analysis != NULL) *analysis = (ReaderDocumentAnalysis) {0};
    bool memory_limited = engine != NULL
        && (engine->navigation.page.script_memory_limited
            || engine->navigation.page.script_result.host_compile_memory_refusals
                   != 0u);
    if (engine == NULL || engine->state != BROWSER_ENGINE_ACTIVE
        || !engine->navigation_ready
        || !browser_engine_has_script_degradation(&engine->navigation)
        || (!memory_limited && !browser_engine_page_is_visually_blank(engine)
            && browser_engine_has_visible_action_target(engine))) {
        if (engine != NULL) engine->basic_view_recovery_generation = 0u;
        return BROWSER_BASIC_VIEW_RECOVERY_NONE;
    }
    ReaderDocumentAnalysis *stored =
        &engine->navigation.page.reader_analysis;
    if (stored->prepared && stored->kind == READER_PAGE_BASIC) {
        lxb_dom_node_t *root = browser_engine_resolve_reader_root(
            &engine->navigation.page, READER_PAGE_BASIC);
        if (analysis != NULL) *analysis = *stored;
        return root == NULL
            ? BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE
            : BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE;
    }
    if (stored->prepared && stored->kind != READER_PAGE_RAW) {
        if (analysis != NULL) *analysis = *stored;
        return BROWSER_BASIC_VIEW_RECOVERY_NONE;
    }
    if (script_runtime_has_pending_author_work(
            engine->navigation.page.runtime)) {
        const uint64_t generation = engine->navigation.generation;
        const size_t runtime_tick =
            engine->navigation.page.script_result.runtime_ticks;
        if (engine->basic_view_recovery_generation != generation) {
            engine->basic_view_recovery_generation = generation;
            engine->basic_view_recovery_first_runtime_tick = runtime_tick;
        }
        size_t first_tick =
            engine->basic_view_recovery_first_runtime_tick;
        size_t settled_ticks = runtime_tick >= first_tick
            ? runtime_tick - first_tick : BROWSER_BLANK_READER_SETTLE_TICKS;
        if (settled_ticks < BROWSER_BLANK_READER_SETTLE_TICKS)
            return BROWSER_BASIC_VIEW_RECOVERY_DEFERRED;
    }
    if (!browser_engine_recovery_wall_settled(engine))
        return BROWSER_BASIC_VIEW_RECOVERY_DEFERRED;
    engine->basic_view_recovery_generation = 0u;
    if (!script_runtime_node_handle_capacity_available(
            engine->navigation.page.runtime)) {
        if (analysis != NULL) {
            *analysis = (ReaderDocumentAnalysis) {
                .prepared = true, .bounded_out = true
            };
        }
        return BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE;
    }
    ReaderDocumentAnalysis prepared = {0};
    if (!reader_document_prepare_basic_complete_with_layout(
            &engine->navigation.page.document,
            &engine->navigation.page.stylesheet,
            script_runtime_has_pending_mutations(engine->navigation.page.runtime)
                ? NULL : &engine->navigation.page.layout, &prepared)) {
        if (analysis != NULL) *analysis = prepared;
        return BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE;
    }
    if (analysis != NULL) *analysis = prepared;
    if (prepared.kind != READER_PAGE_BASIC || prepared.bounded_out) {
        return BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE;
    }
    lxb_dom_node_t *root = browser_engine_establish_reader_root(
        &engine->navigation.page, READER_PAGE_BASIC);
    if (root == NULL) {
        (void) reader_document_discard_prepared_view(
            &engine->navigation.page.document, READER_PAGE_BASIC);
        return BROWSER_BASIC_VIEW_RECOVERY_UNAVAILABLE;
    }
    *stored = prepared;
    browser_engine_alias_reader_images(
        engine, &engine->navigation, root,
        BROWSER_READER_ALIAS_PREPARE);
    return BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE;
}

void browser_engine_set_reader_candidate_mode(BrowserEngine *engine,
                                              bool enabled)
{
    if (engine != NULL && engine->navigation_ready)
        navigation_set_reader_candidate_mode(&engine->navigation, enabled);
}

bool browser_engine_reader_analysis(
    const BrowserEngine *engine, ReaderDocumentAnalysis *analysis)
{
    if (engine == NULL || analysis == NULL
        || !engine->navigation.page.reader_analysis.prepared) return false;
    *analysis = engine->navigation.page.reader_analysis;
    return true;
}
