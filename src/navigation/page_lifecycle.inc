static uint64_t navigation_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

static const char *const navigation_slice_names[NAVIGATION_SLICE_COUNT] = {
    "network", "parser", "script", "style", "resource", "layout",
    "paint", "runtime", "teardown"
};

const char *navigation_slice_phase_name(NavigationSlicePhase phase)
{
    return phase < NAVIGATION_SLICE_COUNT
        ? navigation_slice_names[phase] : "unknown";
}

static uint64_t navigation_record_slice(
    NavigationSession *session, NavigationSlicePhase phase,
    uint64_t started_us, size_t work_units)
{
    uint64_t finished_us = navigation_now_us();
    uint64_t elapsed_us = finished_us >= started_us
                          ? finished_us - started_us : 0;
    if (session == NULL || phase >= NAVIGATION_SLICE_COUNT) return elapsed_us;
    budget_mark_phase(session->budget, navigation_slice_phase_name(phase));
    NavigationSliceStats *stats = &session->performance.slices[phase];
    stats->total_us += elapsed_us;
    stats->slices++;
    if (work_units <= SIZE_MAX - stats->work_units) {
        stats->work_units += work_units;
    } else {
        stats->work_units = SIZE_MAX;
    }
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
        stats->max_work_units = work_units;
    }
    if (elapsed_us > session->performance.max_slice_us) {
        session->performance.max_slice_us = elapsed_us;
        session->performance.max_slice_work_units = work_units;
        session->performance.max_slice_phase = phase;
    }
    return elapsed_us;
}

static uint64_t navigation_record_layout_slices_elapsed(
    NavigationSession *session, uint64_t elapsed_us,
    const LayoutDocument *layout)
{
    if (session == NULL || layout == NULL) return elapsed_us;
    budget_mark_phase(session->budget, "layout");
    NavigationSliceStats *stats =
        &session->performance.slices[NAVIGATION_SLICE_LAYOUT];
    size_t slice_count = layout->cooperative_yields + 1;
    if (slice_count <= SIZE_MAX - stats->slices) stats->slices += slice_count;
    else stats->slices = SIZE_MAX;
    stats->total_us += elapsed_us;
    if (layout->layout_work_units <= SIZE_MAX - stats->work_units) {
        stats->work_units += layout->layout_work_units;
    } else {
        stats->work_units = SIZE_MAX;
    }
    if (layout->cooperative_yields
        <= SIZE_MAX - stats->cooperative_yields) {
        stats->cooperative_yields += layout->cooperative_yields;
    } else {
        stats->cooperative_yields = SIZE_MAX;
    }
    if (layout->max_work_slice_us > stats->max_us) {
        stats->max_us = layout->max_work_slice_us;
        stats->max_work_units = layout->max_work_slice_units;
    }
    if (layout->max_work_slice_us > session->performance.max_slice_us) {
        session->performance.max_slice_us = layout->max_work_slice_us;
        session->performance.max_slice_work_units =
            layout->max_work_slice_units;
        session->performance.max_slice_phase = NAVIGATION_SLICE_LAYOUT;
    }
    return elapsed_us;
}

static uint64_t navigation_record_layout_slices(
    NavigationSession *session, uint64_t started_us,
    const LayoutDocument *layout)
{
    uint64_t finished_us = navigation_now_us();
    uint64_t elapsed_us = finished_us >= started_us
                          ? finished_us - started_us : 0;
    return navigation_record_layout_slices_elapsed(
        session, elapsed_us, layout);
}

static uint64_t navigation_record_resource_slices(
    NavigationSession *session, uint64_t started_us,
    const ExternalStylesheetStats *stylesheets,
    const ExternalImageStats *images)
{
    uint64_t finished_us = navigation_now_us();
    uint64_t elapsed_us = finished_us >= started_us
                          ? finished_us - started_us : 0;
    if (session == NULL) return elapsed_us;
    budget_mark_phase(session->budget, "resource");
    NavigationSliceStats *stats =
        &session->performance.slices[NAVIGATION_SLICE_RESOURCE];
    size_t work_units = 0, yields = 0, slices = 0;
    uint64_t maximum_us = 0;
    size_t maximum_work = 0;
#define ADD_RESOURCE_MEASUREMENT(m) do {                                   \
    if ((m) != NULL && (m)->work_units != 0) {                             \
        size_t part_slices = (m)->cooperative_yields == SIZE_MAX           \
                             ? SIZE_MAX : (m)->cooperative_yields + 1;     \
        slices = part_slices > SIZE_MAX - slices                           \
                 ? SIZE_MAX : slices + part_slices;                        \
        yields = (m)->cooperative_yields > SIZE_MAX - yields               \
                 ? SIZE_MAX : yields + (m)->cooperative_yields;            \
        work_units = (m)->work_units > SIZE_MAX - work_units               \
                     ? SIZE_MAX : work_units + (m)->work_units;            \
        if ((m)->max_slice_us > maximum_us) {                              \
            maximum_us = (m)->max_slice_us;                                \
            maximum_work = (m)->max_slice_work_units;                      \
        }                                                                  \
    }                                                                      \
} while (0)
    ADD_RESOURCE_MEASUREMENT(stylesheets);
    ADD_RESOURCE_MEASUREMENT(images);
#undef ADD_RESOURCE_MEASUREMENT
    if (slices == 0) {
        slices = 1;
        maximum_us = elapsed_us;
    }
    stats->slices = slices > SIZE_MAX - stats->slices
                    ? SIZE_MAX : stats->slices + slices;
    stats->cooperative_yields = yields > SIZE_MAX - stats->cooperative_yields
                                ? SIZE_MAX
                                : stats->cooperative_yields + yields;
    stats->work_units = work_units > SIZE_MAX - stats->work_units
                        ? SIZE_MAX : stats->work_units + work_units;
    stats->total_us += elapsed_us;
    if (maximum_us > stats->max_us) {
        stats->max_us = maximum_us;
        stats->max_work_units = maximum_work;
    }
    if (maximum_us > session->performance.max_slice_us) {
        session->performance.max_slice_us = maximum_us;
        session->performance.max_slice_work_units = maximum_work;
        session->performance.max_slice_phase = NAVIGATION_SLICE_RESOURCE;
    }
    return elapsed_us;
}

static uint64_t navigation_record_script_slices(
    NavigationSession *session, uint64_t started_us,
    const ScriptResult *result, size_t fallback_work_units)
{
    uint64_t finished_us = navigation_now_us();
    uint64_t elapsed_us = finished_us >= started_us
                          ? finished_us - started_us : 0;
    if (session == NULL) return elapsed_us;
    budget_mark_phase(session->budget, "script");
    NavigationSliceStats *stats =
        &session->performance.slices[NAVIGATION_SLICE_SCRIPT];
    size_t yields = result == NULL ? 0 : result->script_cooperative_yields;
    size_t slices = yields == SIZE_MAX ? SIZE_MAX : yields + 1;
    size_t work_units = result == NULL || result->script_work_units == 0
                        ? fallback_work_units : result->script_work_units;
    uint64_t maximum_us = result == NULL ? 0 : result->script_max_slice_us;
    size_t maximum_work = result == NULL
                          ? fallback_work_units
                          : result->script_max_slice_work_units;
    if (maximum_us == 0) maximum_us = elapsed_us;
    stats->slices = slices > SIZE_MAX - stats->slices
                    ? SIZE_MAX : stats->slices + slices;
    stats->cooperative_yields = yields > SIZE_MAX - stats->cooperative_yields
                                ? SIZE_MAX
                                : stats->cooperative_yields + yields;
    stats->work_units = work_units > SIZE_MAX - stats->work_units
                        ? SIZE_MAX : stats->work_units + work_units;
    stats->total_us += elapsed_us;
    if (maximum_us > stats->max_us) {
        stats->max_us = maximum_us;
        stats->max_work_units = maximum_work;
    }
    if (maximum_us > session->performance.max_slice_us) {
        session->performance.max_slice_us = maximum_us;
        session->performance.max_slice_work_units = maximum_work;
        session->performance.max_slice_phase = NAVIGATION_SLICE_SCRIPT;
    }
    return elapsed_us;
}

#define NAVIGATION_COMPILED_STYLESHEET_CACHE_MAX_BYTES (1536u * 1024u)
#define NAVIGATION_COMPILED_STYLESHEET_CACHE_RESERVE_BYTES (2u * 1024u * 1024u)

static void navigation_compiled_stylesheet_cache_discard(
    NavigationSession *session)
{
    if (session == NULL || !session->compiled_stylesheet_cache_ready) return;
    stylesheet_destroy(&session->compiled_stylesheet_cache);
    memset(&session->compiled_stylesheet_cache_external_stats, 0,
           sizeof(session->compiled_stylesheet_cache_external_stats));
    session->compiled_stylesheet_cache_fingerprint = 0;
    session->compiled_stylesheet_cache_resource_signature = 0;
    session->compiled_stylesheet_cache_retained_bytes = 0;
    session->compiled_stylesheet_cache_ready = false;
    session->performance.compiled_stylesheet_cache_retained_bytes = 0;
    session->performance.compiled_stylesheet_cache_evictions++;
}

static void navigation_compiled_stylesheet_cache_reclaim_under_pressure(
    NavigationSession *session)
{
    if (session == NULL || !session->compiled_stylesheet_cache_ready) return;
    if (!budget_pressure_required(
            session->budget, 0,
            NAVIGATION_COMPILED_STYLESHEET_CACHE_RESERVE_BYTES)) return;
    size_t retained = session->compiled_stylesheet_cache_retained_bytes;
    navigation_compiled_stylesheet_cache_discard(session);
    budget_record_pressure(session->budget, BUDGET_PRESSURE_STYLESHEET,
                           retained, retained);
}

static void navigation_compiled_stylesheet_cache_move(
    NavigationSession *destination, NavigationSession *source)
{
    if (destination == NULL || source == NULL || destination == source) {
        return;
    }
    navigation_compiled_stylesheet_cache_discard(destination);
    destination->compiled_stylesheet_cache =
        source->compiled_stylesheet_cache;
    destination->compiled_stylesheet_cache_external_stats =
        source->compiled_stylesheet_cache_external_stats;
    destination->compiled_stylesheet_cache_fingerprint =
        source->compiled_stylesheet_cache_fingerprint;
    destination->compiled_stylesheet_cache_resource_signature =
        source->compiled_stylesheet_cache_resource_signature;
    destination->compiled_stylesheet_cache_retained_bytes =
        source->compiled_stylesheet_cache_retained_bytes;
    destination->compiled_stylesheet_cache_ready =
        source->compiled_stylesheet_cache_ready;
    memset(&source->compiled_stylesheet_cache, 0,
           sizeof(source->compiled_stylesheet_cache));
    memset(&source->compiled_stylesheet_cache_external_stats, 0,
           sizeof(source->compiled_stylesheet_cache_external_stats));
    source->compiled_stylesheet_cache_fingerprint = 0;
    source->compiled_stylesheet_cache_resource_signature = 0;
    source->compiled_stylesheet_cache_retained_bytes = 0;
    source->compiled_stylesheet_cache_ready = false;
    source->performance.compiled_stylesheet_cache_retained_bytes = 0;
    destination->performance.compiled_stylesheet_cache_retained_bytes =
        destination->compiled_stylesheet_cache_retained_bytes;
}

static bool navigation_compiled_stylesheet_cache_take(
    NavigationSession *session, uint64_t fingerprint, Stylesheet *sheet,
    ExternalStylesheetStats *external_stats)
{
    if (session == NULL || sheet == NULL || external_stats == NULL) {
        return false;
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_COMPILED_STYLESHEET_CACHE") != NULL) {
        return false;
    }
#endif
    uint64_t signature = browser_session_stylesheet_cache_signature(
        session->browser_session, tilefinch_platform_monotonic_time_ns());
    session->performance.compiled_stylesheet_cache_last_observed_signature =
        signature;
    session->performance.compiled_stylesheet_cache_last_observed_fingerprint =
        fingerprint;
    if (signature == 0) {
        navigation_compiled_stylesheet_cache_discard(session);
        session->performance.compiled_stylesheet_cache_misses++;
        return false;
    }
    if (session->compiled_stylesheet_cache_ready
        && session->compiled_stylesheet_cache_resource_signature
               != signature) {
        navigation_compiled_stylesheet_cache_discard(session);
    }
    if (!session->compiled_stylesheet_cache_ready
        || session->compiled_stylesheet_cache_fingerprint != fingerprint) {
        session->performance.compiled_stylesheet_cache_misses++;
        return false;
    }
    *sheet = session->compiled_stylesheet_cache;
    *external_stats = session->compiled_stylesheet_cache_external_stats;
    memset(&session->compiled_stylesheet_cache, 0,
           sizeof(session->compiled_stylesheet_cache));
    memset(&session->compiled_stylesheet_cache_external_stats, 0,
           sizeof(session->compiled_stylesheet_cache_external_stats));
    session->compiled_stylesheet_cache_fingerprint = 0;
    session->compiled_stylesheet_cache_resource_signature = 0;
    session->compiled_stylesheet_cache_retained_bytes = 0;
    session->compiled_stylesheet_cache_ready = false;
    session->performance.compiled_stylesheet_cache_retained_bytes = 0;
    session->performance.compiled_stylesheet_cache_hits++;
    return true;
}

static void navigation_compiled_stylesheet_cache_store(
    NavigationSession *session, NavigationPage *page)
{
    if (session == NULL || page == NULL) return;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_DISABLE_COMPILED_STYLESHEET_CACHE") != NULL) {
        return;
    }
#endif
    if (!page->compiled_stylesheet_cacheable
        || page->stylesheet.budget == NULL
        || page->external_stylesheets.skipped_pressure != 0) {
        session->performance.compiled_stylesheet_cache_unmarked_skips++;
        return;
    }
    if (page->stylesheet.build_generation
        != page->compiled_stylesheet_build_generation) {
        session->performance.compiled_stylesheet_cache_generation_skips++;
        return;
    }
    size_t retained = stylesheet_retained_bytes(&page->stylesheet);
    if (retained == 0
        || retained > NAVIGATION_COMPILED_STYLESHEET_CACHE_MAX_BYTES) {
        session->performance.compiled_stylesheet_cache_size_skips++;
        return;
    }
    if (budget_pressure_required(
            session->budget, retained,
            NAVIGATION_COMPILED_STYLESHEET_CACHE_RESERVE_BYTES)) {
        session->performance.compiled_stylesheet_cache_pressure_skips++;
        return;
    }
    navigation_compiled_stylesheet_cache_discard(session);
    /* The retired runtime is destroyed immediately after this move. Detach
       its borrowed bridge pointer first so teardown can never observe the
       zeroed NavigationPage field if runtime destruction later grows a
       callback-cleanup path. */
    script_runtime_set_stylesheet(page->runtime, NULL);
    stylesheet_prepare_for_document_reuse(&page->stylesheet);
    session->compiled_stylesheet_cache = page->stylesheet;
    session->compiled_stylesheet_cache_external_stats =
        page->external_stylesheets;
    session->compiled_stylesheet_cache_fingerprint =
        page->compiled_stylesheet_fingerprint;
    session->compiled_stylesheet_cache_resource_signature =
        page->compiled_stylesheet_resource_signature;
    session->performance.compiled_stylesheet_cache_last_stored_signature =
        page->compiled_stylesheet_resource_signature;
    session->performance.compiled_stylesheet_cache_last_stored_fingerprint =
        page->compiled_stylesheet_fingerprint;
    session->compiled_stylesheet_cache_retained_bytes = retained;
    session->compiled_stylesheet_cache_ready = true;
    session->performance.compiled_stylesheet_cache_retained_bytes = retained;
    session->performance.compiled_stylesheet_cache_stores++;
    memset(&page->stylesheet, 0, sizeof(page->stylesheet));
}

static bool navigation_build_stylesheet(NavigationSession *session)
{
    return session != NULL
           && (session->external_resources_enabled
               ? stylesheet_build_context_deferred(
                     &session->page.stylesheet, session->budget,
                     &session->page.document, &session->viewport)
               : stylesheet_build_context(
                     &session->page.stylesheet, session->budget,
                     &session->page.document, &session->viewport));
}

/* External author CSS is valuable but not page-critical.  If its bounded
   loader cannot finish, retain the DOM and install the strongest stylesheet
   that can still be built atomically: inline author rules first, then an
   initialized empty sheet whose computed-style defaults keep the document
   readable.  Never leave a partially built cascade attached to the page. */
static bool navigation_build_usable_stylesheet_fallback(
    Stylesheet *stylesheet, Budget *budget, const PocDocument *document,
    const ViewportContext *viewport)
{
    if (stylesheet == NULL || budget == NULL || document == NULL
        || viewport == NULL) return false;
    stylesheet_destroy(stylesheet);
    if (stylesheet_build_context(
            stylesheet, budget, document, viewport)) return true;
    stylesheet_destroy(stylesheet);
    if (!stylesheet_build_context_deferred(
            stylesheet, budget, document, viewport)) return false;
    stylesheet->document_rules_deferred = false;
    return true;
}

static bool navigation_install_usable_stylesheet_fallback(
    NavigationSession *session)
{
    if (session == NULL
        || !navigation_build_usable_stylesheet_fallback(
               &session->page.stylesheet, session->budget,
               &session->page.document, &session->viewport)) return false;
    session->page.compiled_stylesheet_cacheable = false;
    session->performance.stylesheet_pressure_fallbacks++;
    return true;
}

static void navigation_ensure_layout_reuse(NavigationSession *session)
{
    if (session == NULL || session->page.layout_reuse != NULL
        || session->page.document.node_count < 256
        || budget_pressure_required(
               session->budget, 96u * 1024u, 512u * 1024u)) return;
    session->page.layout_reuse =
        layout_reuse_cache_create(session->budget);
}

static void navigation_record_layout_reuse(
    NavigationSession *session, const LayoutReuseStats *before,
    const LayoutReuseStats *after)
{
    if (session == NULL || before == NULL || after == NULL) return;
#define ADD_REUSE_DELTA(field, target) do {                                \
    if (after->field >= before->field) {                                   \
        session->performance.target += after->field - before->field;       \
    }                                                                      \
} while (0)
    ADD_REUSE_DELTA(style_hits, layout_reuse_style_hits);
    ADD_REUSE_DELTA(style_misses, layout_reuse_style_misses);
    ADD_REUSE_DELTA(intrinsic_hits, layout_reuse_intrinsic_hits);
    ADD_REUSE_DELTA(intrinsic_misses, layout_reuse_intrinsic_misses);
    ADD_REUSE_DELTA(table_row_hits, layout_reuse_table_row_hits);
    ADD_REUSE_DELTA(table_row_misses, layout_reuse_table_row_misses);
    ADD_REUSE_DELTA(scoped_invalidations,
                    layout_reuse_scoped_invalidations);
    ADD_REUSE_DELTA(full_resets, layout_reuse_full_resets);
#undef ADD_REUSE_DELTA
    session->performance.layout_reuse_retained_bytes =
        after->retained_bytes;
}

static bool navigation_build_layout(NavigationSession *session,
                                    LayoutDocument *layout,
                                    const FontSet *fonts,
                                    const ImageResources *images)
{
    if (session == NULL || layout == NULL) return false;
    navigation_ensure_layout_reuse(session);
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &before);
    bool built = layout_build_context_reuse(
        layout, session->budget, &session->page.document,
        &session->page.stylesheet, fonts, images, &session->viewport,
        session->page.layout_reuse);
    layout_reuse_cache_stats(session->page.layout_reuse, &after);
    navigation_record_layout_reuse(session, &before, &after);
    if (built && session->page.layout_reuse != NULL
        && !layout_reuse_cache_can_reuse_mutations(
               session->page.layout_reuse)) {
        layout_reuse_cache_destroy(session->page.layout_reuse);
        session->page.layout_reuse = NULL;
        session->performance.layout_reuse_retained_bytes = 0;
    }
    return built;
}

static bool navigation_load_web_fonts(NavigationSession *session,
                                      const char *document_url)
{
    if (session == NULL || document_url == NULL) return false;
    fonts_external_loader_cancel(
        &session->page.external_font_loader,
        session->page.resource_scheduler, &session->page.external_fonts);
    memset(&session->page.external_fonts, 0,
           sizeof(session->page.external_fonts));
    if (!session->web_fonts_enabled) return true;
    return fonts_external_loader_begin(
        &session->page.external_font_loader, &session->page.stylesheet,
        session->font_timeout_ms, &session->page.external_fonts);
}

static ScriptRuntime *navigation_create_runtime(
    NavigationSession *session, PocDocument *document, const char *url,
    const char *referrer_policy, bool defer_document_scripts,
    const ViewportContext *viewport, ScriptDocumentScope document_scope,
    bool inherit_remote_document, FetchSchedulerDomain *fetch_domain,
    const Stylesheet *stylesheet,
    ScriptResult *result)
{
    if (session == NULL) return NULL;
    bool bind_remote_document = inherit_remote_document
        && document_scope == SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL;
    ScriptRuntimeOptions options = {
        .session = session->browser_session,
        .top_level_url = document_scope == SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME
            ? session->page.document_url : url,
        .fetch_scheduler_domain = fetch_domain,
        .stylesheet = stylesheet,
        .viewport = viewport == NULL ? session->viewport : *viewport,
        .execution_policy = session->script_execution_policy,
        .maximum_scripts = session->document_scripts_enabled
            ? session->maximum_scripts : 0,
        .maximum_script_bytes = session->document_scripts_enabled
            ? session->maximum_script_bytes : 0,
        .maximum_script_file_bytes = session->document_scripts_enabled
            ? session->maximum_script_file_bytes : 0,
        .defer_document_scripts = defer_document_scripts,
        .dynamic_code_disabled = !tilefinch_csp_allows_dynamic_code(
            &document->content_security_policy),
        .document_scope = document_scope,
        .referrer_policy = referrer_policy,
        .remote_element_lookup = bind_remote_document
            ? session->remote_element_lookup : NULL,
        .remote_element_opaque = bind_remote_document
            ? session->remote_element_opaque : NULL,
        .remote_selector_lookup = bind_remote_document
            ? session->remote_selector_lookup : NULL,
        .remote_selector_opaque = bind_remote_document
            ? session->remote_selector_opaque : NULL,
        .remote_selector_collect = bind_remote_document
            ? session->remote_selector_collect : NULL,
        .remote_selector_collect_opaque = bind_remote_document
            ? session->remote_selector_collect_opaque : NULL,
        .remote_descendant_collect = bind_remote_document
            ? session->remote_descendant_collect : NULL,
        .remote_descendant_collect_opaque = bind_remote_document
            ? session->remote_descendant_collect_opaque : NULL,
        .remote_node_read = bind_remote_document
            ? session->remote_node_read : NULL,
        .remote_node_read_opaque = bind_remote_document
            ? session->remote_node_read_opaque : NULL,
        .remote_node_write = bind_remote_document
            ? session->remote_node_write : NULL,
        .remote_node_write_opaque = bind_remote_document
            ? session->remote_node_write_opaque : NULL,
        .node_visibility = bind_remote_document
            ? session->node_visibility : NULL,
        .node_visibility_opaque = bind_remote_document
            ? session->node_visibility_opaque : NULL,
        .section_identity = bind_remote_document
            ? session->runtime_section_identity : 0
    };
    return script_runtime_create_configured(
        document, session->budget, session->js_memory_limit,
        session->js_timeout_ms, url, &options, result);
}

static void navigation_attach_layout(NavigationSession *session)
{
    if (session != NULL && session->page.runtime != NULL) {
        script_runtime_set_layout(session->page.runtime,
                                  &session->page.layout,
                                  session->viewport.css_height);
        script_runtime_set_images(session->page.runtime,
                                  session->images);
        script_runtime_intersection_recheck(session->page.runtime);
        script_runtime_media_recheck(session->page.runtime);
    }
}

static int navigation_max_scroll(const NavigationSession *session)
{
    return session == NULL ? 0
           : viewport_max_scroll_css(&session->viewport,
                                     session->page.layout.height);
}

static uint64_t fingerprint_bytes(uint64_t hash, const char *data,
                                  size_t length)
{
    for (size_t i = 0; data != NULL && i < length; i++) {
        hash = (hash ^ (unsigned char) data[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static bool resource_fingerprint_node(lxb_dom_node_t *node,
                                      uint64_t *hash)
{
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t style_depth = 0, visited = 0;
    while (node != NULL && node != boundary_parent) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        bool style = name != NULL && name_length == 5
                     && memcmp(name, "style", 5) == 0;
        bool link = name != NULL && name_length == 4
                    && memcmp(name, "link", 4) == 0;
        bool image = (name != NULL && name_length == 3
                      && memcmp(name, "img", 3) == 0)
                     || (name != NULL && name_length == 5
                         && memcmp(name, "video", 5) == 0);
        bool source = name != NULL && name_length == 6
                      && memcmp(name, "source", 6) == 0;
        bool base = name != NULL && name_length == 4
                    && memcmp(name, "base", 4) == 0;
        bool use = name != NULL && name_length == 3
                   && memcmp(name, "use", 3) == 0;
        if (style || link || image || source || base || use) {
            *hash = fingerprint_bytes(*hash, name, name_length);
            const char *attributes[] = {
                "rel", "href", "xlink:href", "src", "srcset",
                "data-src", "data-srcset", "media", "type", "sizes",
                "poster"
            };
            for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]);
                 i++) {
                size_t length = 0;
                const char *value = document_attribute(node, attributes[i],
                                                       &length);
                *hash = fingerprint_bytes(*hash, value, length);
            }
        }
        if (style) style_depth++;
        if (node->type == LXB_DOM_NODE_TYPE_TEXT && style_depth != 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            *hash = fingerprint_bytes(*hash, text, length);
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        for (;;) {
            size_t leave_name_length = 0;
            const char *leave_name = document_element_name(
                node, &leave_name_length);
            if (leave_name != NULL && leave_name_length == 5
                && memcmp(leave_name, "style", 5) == 0) style_depth--;
            if (node->next != NULL) {
                node = node->next;
                break;
            }
            node = node->parent;
            if (node == NULL || node == boundary_parent) return true;
        }
    }
    return true;
}

static bool page_resource_fingerprint(const PocDocument *document,
                                      uint64_t *hash)
{
    if (hash == NULL) return false;
    *hash = 0;
    if (document == NULL || document->html == NULL) return true;
    *hash = UINT64_C(1469598103934665603);
    return resource_fingerprint_node(
        lxb_dom_interface_node(document->html), hash);
}

static bool navigation_resource_fingerprint(
    NavigationSession *session, const PocDocument *document, uint64_t *hash)
{
    if (session != NULL) session->performance.resource_fingerprint_scans++;
    if (page_resource_fingerprint(document, hash)) return true;
    if (session != NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "resource fingerprint traversal exceeded %u nodes",
                 NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT);
    }
    return false;
}

static bool draw_command_equal(const DrawCommand *left,
                               const DrawCommand *right)
{
    bool text_equal = left->text_length == right->text_length
        && (left->text_length == 0 || left->text == right->text);
    /* Layout text may point directly into DOM storage. A mutation can retire
       the old node string before damage comparison, so content comparison
       would dereference stale Lexbor memory. Different storage is safely and
       conservatively treated as damaged; pointer identity remains exact. */
    return left->type == right->type && left->x == right->x
        && left->y == right->y && left->width == right->width
        && left->height == right->height && left->color == right->color
        && text_equal
        && left->scale == right->scale && left->font_size == right->font_size
        && left->font_family == right->font_family
        && left->font_weight == right->font_weight
        && left->font_italic == right->font_italic
        && left->letter_spacing == right->letter_spacing
        && left->image == right->image && left->radius == right->radius
        && left->opacity_scale == right->opacity_scale
        && left->image_fit == right->image_fit
        && left->z_index == right->z_index;
}

static void damage_add(int *left, int *top, int *right, int *bottom,
                       const DrawCommand *command)
{
    if (command->width <= 0 || command->height <= 0) return;
    if (*right <= *left) {
        *left = command->x; *top = command->y;
        *right = command->x + command->width;
        *bottom = command->y + command->height;
        return;
    }
    if (command->x < *left) *left = command->x;
    if (command->y < *top) *top = command->y;
    if (command->x + command->width > *right) {
        *right = command->x + command->width;
    }
    if (command->y + command->height > *bottom) {
        *bottom = command->y + command->height;
    }
}

static void damage_add_rect(int *left, int *top, int *right, int *bottom,
                            int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return;
    DrawCommand command = {.x = x, .y = y, .width = width,
                           .height = height};
    damage_add(left, top, right, bottom, &command);
}

static void navigation_measure_damage(NavigationSession *session,
                                      const LayoutDocument *old_layout,
                                      const LayoutDocument *new_layout)
{
    int left = 0, top = 0, right = 0, bottom = 0;
    if (old_layout->page_background != new_layout->page_background) {
        right = new_layout->width;
        bottom = old_layout->height > new_layout->height
                 ? old_layout->height : new_layout->height;
    } else {
        size_t prefix = 0;
        size_t shared = old_layout->count < new_layout->count
                        ? old_layout->count : new_layout->count;
        while (prefix < shared
               && draw_command_equal(&old_layout->commands[prefix],
                                     &new_layout->commands[prefix])) {
            prefix++;
        }
        size_t old_end = old_layout->count;
        size_t new_end = new_layout->count;
        while (old_end > prefix && new_end > prefix
               && draw_command_equal(&old_layout->commands[old_end - 1],
                                     &new_layout->commands[new_end - 1])) {
            old_end--;
            new_end--;
        }
        for (size_t i = prefix; i < old_end; i++) {
            damage_add(&left, &top, &right, &bottom,
                       &old_layout->commands[i]);
        }
        for (size_t i = prefix; i < new_end; i++) {
            damage_add(&left, &top, &right, &bottom,
                       &new_layout->commands[i]);
        }
        if (old_layout->height != new_layout->height) {
            int shorter = old_layout->height < new_layout->height
                          ? old_layout->height : new_layout->height;
            int taller = old_layout->height > new_layout->height
                         ? old_layout->height : new_layout->height;
            int width = old_layout->width > new_layout->width
                        ? old_layout->width : new_layout->width;
            damage_add_rect(&left, &top, &right, &bottom, 0, shorter,
                            width, taller - shorter);
        }
    }
    session->relayout_damage_valid = right > left && bottom > top;
    session->relayout_damage_left = left;
    session->relayout_damage_top = top;
    session->relayout_damage_right = right;
    session->relayout_damage_bottom = bottom;
}

static char *bounded_copy(Budget *budget, const char *text, size_t limit)
{
    if (text == NULL) text = "";
    if (limit == 0) return NULL;
    size_t length = strnlen(text, limit);
    if (length >= limit) length = limit - 1;
    char *copy = budget_malloc(budget, length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void capture_cached_script_error(NavigationSession *session,
                                        const char *stack)
{
    if (session == NULL || session->browser_session == NULL || stack == NULL)
        return;
    snprintf(session->last_script_error_context,
             sizeof(session->last_script_error_context),
             "cache-capture-pending");
    const char *open = strchr(stack, '(');
    const char *close = open == NULL ? NULL : strchr(open, ')');
    const char *caller_open = close == NULL ? NULL : strchr(close + 1, '(');
    const char *caller_close = caller_open == NULL
                               ? NULL : strchr(caller_open, ')');
    bool caller_anonymous = close != NULL && caller_open != NULL
        && (size_t) (caller_open - close) < 64
        && strstr(close, "<anonymous>") != NULL
        && strstr(close, "<anonymous>") < caller_open;
    if (caller_close != NULL && !caller_anonymous) {
        open = caller_open;
        close = caller_close;
    }
    if (close == NULL) return;
    const char *column = close;
    while (column > open && column[-1] != ':') column--;
    if (column == open) return;
    const char *line = column - 1;
    while (line > open && line[-1] != ':') line--;
    if (line == open) return;
    size_t url_length = (size_t) ((line - 1) - (open + 1));
    if (url_length == 0 || url_length >= NAVIGATION_URL_LIMIT) return;
    char url[NAVIGATION_URL_LIMIT];
    memcpy(url, open + 1, url_length); url[url_length] = '\0';
    const BrowserCacheEntry *entry = browser_session_cache_lookup(
        session->browser_session, url);
    if (entry == NULL) return;
    unsigned long line_number = strtoul(line, NULL, 10);
    unsigned long column_number = strtoul(column, NULL, 10);
    if (line_number == 0 || column_number == 0) return;
    size_t offset = 0;
    for (unsigned long current = 1;
         current < line_number && offset < entry->length; current++) {
        const unsigned char *newline = memchr(entry->data + offset, '\n',
                                              entry->length - offset);
        if (newline == NULL) return;
        offset = (size_t) (newline - entry->data) + 1;
    }
    if (column_number - 1 > entry->length - offset) return;
    offset += (size_t) column_number - 1;
    size_t start = offset > 900 ? offset - 900 : 0;
    size_t finish = offset + 900 < entry->length
                    ? offset + 900 : entry->length;
    size_t written = 0;
    for (size_t at = start; at < finish
         && written + 1 < sizeof(session->last_script_error_context); at++) {
        unsigned char character = entry->data[at];
        session->last_script_error_context[written++] =
            character < 32 || character == 127 ? ' ' : (char) character;
    }
    session->last_script_error_context[written] = '\0';
}

static void navigation_record_soft_script_error(
    NavigationSession *session, const ExternalScriptMetrics *metrics)
{
    if (session == NULL || metrics == NULL || metrics->failed == 0
        || session->page.script_result.error[0] == '\0') return;
    snprintf(session->last_script_error_context,
             sizeof(session->last_script_error_context), "%s",
             session->page.script_result.error_source_context);
    if (session->last_script_error_context[0] == '\0') {
        capture_cached_script_error(
            session, session->page.script_result.error);
    }
}

static void entry_destroy(Budget *budget, NavigationEntry *entry)
{
    budget_free(budget, entry->title);
    budget_free(budget, entry->url);
    memset(entry, 0, sizeof(*entry));
}

static bool page_has_owned_state(const NavigationPage *page)
{
    return page != NULL
        && (page->loaded || page->document.html != NULL
            || page->stylesheet.budget != NULL || page->layout.budget != NULL
            || page->layout_reuse != NULL
            || page->runtime != NULL || page->resource_scheduler != NULL
            || page->fetch_domain != NULL);
}

static void page_destroy_value(NavigationSession *session,
                               NavigationPage *page)
{
    if (session == NULL || !page_has_owned_state(page)) return;
    uint64_t started_us = navigation_now_us();
    size_t work_units = page->document.node_count;
    if (page->layout.count <= SIZE_MAX - work_units) {
        work_units += page->layout.count;
    } else {
        work_units = SIZE_MAX;
    }
    for (size_t i = 0; i < page->message_count; i++) {
        budget_free(session->budget, page->messages[i].json);
        budget_free(session->budget,
                    page->messages[i].target_origin);
    }
    for (size_t i = 0; i < page->frame_count; i++) {
        script_runtime_destroy(page->frames[i].runtime);
        document_destroy(&page->frames[i].document);
    }
    fonts_external_loader_cancel(
        &page->external_font_loader, page->resource_scheduler,
        &page->external_fonts);
    images_priority_load_destroy(page->deferred_image_job);
    page->deferred_image_job = NULL;
    budget_free(session->budget, page->deferred_image_targets);
    page->deferred_image_targets = NULL;
    layout_destroy(&page->layout);
    layout_reuse_cache_destroy(page->layout_reuse);
    page->layout_reuse = NULL;
    images_destroy(&page->images);
    stylesheet_destroy(&page->stylesheet);
    stylesheet_document_resources_destroy(&page->stylesheet_resources);
    script_runtime_destroy(page->runtime);
    fetch_scheduler_destroy(page->resource_scheduler);
    fetch_scheduler_domain_release(page->fetch_domain);
    document_destroy(&page->document);
    memset(page, 0, sizeof(*page));
    session->page_destroys++;
    (void) navigation_record_slice(
        session, NAVIGATION_SLICE_TEARDOWN, started_us, work_units);
}

static void page_destroy(NavigationSession *session)
{
    if (session != NULL) page_destroy_value(session, &session->page);
}

static bool navigation_retire_exhausted_script_realms(
    NavigationSession *session)
{
    if (session == NULL || session->page.runtime == NULL) {
        return false;
    }
    bool explicit_oom =
        strstr(session->page.script_result.error, "out of memory") != NULL;
    size_t remaining = script_runtime_heap_remaining(
        session->page.runtime);
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_SCRIPT_FAILURES") != NULL) {
        fprintf(stderr,
                "tilefinch-script-retirement: explicit-oom=%s "
                "heap-remaining=%zu error=\"%.160s\"\n",
                explicit_oom ? "yes" : "no", remaining,
                session->page.script_result.error);
    }
#endif
    if (!explicit_oom && remaining >= 256u * 1024u) return false;
    /*
     * Author JavaScript exhausting its separately bounded realm must not
     * discard an otherwise usable document.  The DOM, completed stylesheet,
     * controls, and resource scheduler are independently owned by the page;
     * retire only the realms whose callbacks can no longer run reliably.
     * Keeping the ScriptResult makes the degraded state visible in device
     * diagnostics without attempting another allocation in the failed VM.
     */
    script_runtime_set_synchronous_layout_callback(
        session->page.runtime, NULL, NULL);
    script_runtime_set_layout(session->page.runtime, NULL, 0);
    script_runtime_destroy(session->page.runtime);
    session->page.runtime = NULL;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        script_runtime_destroy(session->page.frames[i].runtime);
        session->page.frames[i].runtime = NULL;
    }
    session->page.script_result.success = false;
    snprintf(session->page.script_result.summary,
             sizeof(session->page.script_result.summary),
             "JavaScript stopped at its memory limit");
    stylesheet_enable_static_custom_element_fallback(
        &session->page.stylesheet);
    return true;
}

static bool navigation_queue_frame_message(void *opaque,
                                           ScriptRuntime *source,
                                           long target_frame_handle,
                                           const char *json,
                                           const char *target_origin)
{
    NavigationSession *session = opaque;
    if (session == NULL || source == NULL || json == NULL
        || target_origin == NULL
        || session->page.message_count == NAVIGATION_MESSAGE_LIMIT) {
        return false;
    }
    NavigationMessage *message =
        &session->page.messages[session->page.message_count];
    size_t json_length = strlen(json), origin_length = strlen(target_origin);
    if (json_length > 65536 || origin_length > NAVIGATION_URL_LIMIT) {
        return false;
    }
    if (strcmp(target_origin, "*") != 0
        && strcmp(target_origin, "null") != 0) {
        char canonical[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
        if (!tilefinch_url_origin(
                target_origin, canonical, sizeof(canonical))
            || strcmp(canonical, target_origin) != 0) return false;
    }
    message->json = budget_malloc(session->budget, json_length + 1);
    message->target_origin = budget_malloc(session->budget,
                                            origin_length + 1);
    if (message->json == NULL || message->target_origin == NULL) {
        budget_free(session->budget, message->json);
        budget_free(session->budget, message->target_origin);
        memset(message, 0, sizeof(*message));
        return false;
    }
    memcpy(message->json, json, json_length + 1);
    memcpy(message->target_origin, target_origin, origin_length + 1);
    message->source = source;
    if (session->next_frame_message_sequence != UINT64_MAX) {
        session->next_frame_message_sequence++;
    }
    message->sequence = session->next_frame_message_sequence;
    message->source_frame_handle = 0;
    if (source != session->page.runtime) {
        for (size_t i = 0; i < session->page.frame_count; i++) {
            if (!session->page.frames[i].retired
                && session->page.frames[i].runtime == source) {
                message->source_frame_handle =
                    session->page.frames[i].parent_handle;
                message->source_generation =
                    session->page.frames[i].lifecycle_generation;
                if (session->page.frames[i].opaque_origin) {
                    snprintf(message->source_origin,
                             sizeof(message->source_origin), "null");
                } else if (!tilefinch_url_origin(
                        session->page.frames[i].url,
                        message->source_origin,
                        sizeof(message->source_origin))) {
                    message->source_frame_handle = 0;
                }
                break;
            }
        }
        if (message->source_frame_handle == 0) {
            budget_free(session->budget, message->json);
            budget_free(session->budget, message->target_origin);
            memset(message, 0, sizeof(*message));
            return false;
        }
    } else if (!tilefinch_url_origin(
                   session->page.document_url, message->source_origin,
                   sizeof(message->source_origin))) {
        budget_free(session->budget, message->json);
        budget_free(session->budget, message->target_origin);
        memset(message, 0, sizeof(*message));
        return false;
    }
    message->target_frame_handle = target_frame_handle;
    if (source == session->page.runtime && target_frame_handle > 0) {
        for (size_t i = 0; i < session->page.frame_count; i++) {
            NavigationFrame *target = &session->page.frames[i];
            if (!target->retired
                && target->parent_handle == target_frame_handle) {
                message->target_generation = target->lifecycle_generation;
                break;
            }
        }
    }
    session->page.message_count++;
    session->frame_messages_posted++;
    if (source == session->page.runtime) session->frame_messages_to_child++;
    else session->frame_messages_to_parent++;
#if !defined(TILEFINCH_NO_TRACE)
    const char *frame_trace_value = getenv("TILEFINCH_TRACE_FRAME_MESSAGES");
    if (frame_trace_value != NULL) {
        /* Explicitly opt-in because challenge payloads can contain transient
           tokens. Keep each record bounded even though the queue itself
           accepts larger structured-clone JSON. */
        size_t trace_limit = 2048;
        char *trace_end = NULL;
        unsigned long requested = strtoul(frame_trace_value, &trace_end, 10);
        if (trace_end != frame_trace_value && *trace_end == '\0'
            && requested >= 256 && requested <= 65536) {
            trace_limit = (size_t) requested;
        }
        fprintf(stderr,
                "frame-message-json sequence=%llu direction=%s source=%ld target=%ld json=\"%.*s\"\n",
                (unsigned long long) message->sequence,
                source == session->page.runtime ? "parent-to-child"
                                                : "child-to-parent",
                message->source_frame_handle, message->target_frame_handle,
                (int) (json_length < trace_limit ? json_length : trace_limit),
                json);
    }
#endif
    const char *event = strstr(json, "\"event\":");
    if (event != NULL) {
        event = strchr(event + 8, '"');
        if (event != NULL) {
            event++;
            const char *end = strchr(event, '"');
            if (end != NULL) {
                size_t length = (size_t) (end - event);
                if (length >= sizeof(session->last_frame_message_event)) {
                    length = sizeof(session->last_frame_message_event) - 1;
                }
                memcpy(session->last_frame_message_event, event, length);
                session->last_frame_message_event[length] = '\0';
                size_t used = strlen(session->frame_message_event_log);
                if (used + length + 5
                    < sizeof(session->frame_message_event_log)) {
                    snprintf(session->frame_message_event_log + used,
                             sizeof(session->frame_message_event_log) - used,
                             "%s%s:%.*s",
                             used == 0 ? "" : ",",
                             source == session->page.runtime ? "P>C" : "C>P",
                             (int) length, event);
                }
                if (length == 6 && memcmp(event, "reject", 6) == 0) {
                    const char *reason = strstr(json, "\"reason\":");
                    if (reason != NULL) {
                        reason += 9;
                        while (*reason == ' ' || *reason == '"') reason++;
                        const char *end = reason;
                        while (*end != '\0' && *end != '"' && *end != ','
                               && *end != '}') end++;
                        size_t reason_length = (size_t) (end - reason);
                        if (reason_length >=
                            sizeof(session->last_frame_reject_reason)) {
                            reason_length =
                                sizeof(session->last_frame_reject_reason) - 1;
                        }
                        memcpy(session->last_frame_reject_reason, reason,
                               reason_length);
                        session->last_frame_reject_reason[reason_length] = '\0';
                    }
                }
            }
        }
    }
    return true;
}

static bool url_origin(const char *url, char *output, size_t capacity)
{
    return tilefinch_url_origin(url, output, capacity);
}

static void navigation_accept_response_cookies(
    BrowserSession *browser_session,
    const TilefinchRequestContext *request_context,
    const FetchResult *fetch, const char *fallback_url)
{
    if (browser_session == NULL || request_context == NULL || fetch == NULL
        || !tilefinch_request_sends_credentials(request_context)) return;
    for (size_t i = 0; i < fetch->set_cookie_count; i++) {
        TilefinchRequestContext response_context = *request_context;
        response_context.target_url = fetch_set_cookie_url(
            fetch, i, fallback_url);
        (void) browser_session_cookie_set_http_context(
            browser_session, &response_context, fetch->set_cookies[i]);
    }
}

static NavigationFrame *frame_for_handle(NavigationSession *session,
                                         long handle)
{
    for (size_t i = 0; i < session->page.frame_count; i++) {
        if (!session->page.frames[i].retired
            && session->page.frames[i].parent_handle == handle) {
            return &session->page.frames[i];
        }
    }
    return NULL;
}

static void navigation_drop_frame_messages(NavigationSession *session,
                                           const NavigationFrame *frame);

static bool navigation_deliver_frame_messages(NavigationSession *session)
{
    size_t delivered = 0;
    while (session->page.message_count != 0
           && delivered < NAVIGATION_MESSAGE_LIMIT) {
        NavigationMessage message = session->page.messages[0];
        memmove(session->page.messages, session->page.messages + 1,
                (session->page.message_count - 1)
                    * sizeof(*session->page.messages));
        session->page.message_count--;
        memset(&session->page.messages[session->page.message_count], 0,
               sizeof(*session->page.messages));
        ScriptRuntime *target = NULL;
        NavigationFrame *target_frame = NULL;
        long source_handle = message.source_frame_handle;
        bool lifecycle_valid = true;
        if (message.source == session->page.runtime) {
            target_frame = frame_for_handle(
                session, message.target_frame_handle);
            if (target_frame != NULL
                && target_frame->lifecycle_generation
                       == message.target_generation) {
                target = target_frame->runtime;
            } else {
                lifecycle_valid = false;
            }
        } else {
            target = session->page.runtime;
            bool source_found = false;
            for (size_t i = 0; i < session->page.frame_count; i++) {
                NavigationFrame *source_frame = &session->page.frames[i];
                if (!source_frame->retired
                    && source_frame->runtime == message.source
                    && source_frame->parent_handle
                           == message.source_frame_handle
                    && source_frame->lifecycle_generation
                           == message.source_generation) {
                    source_found = true;
                    break;
                }
            }
            lifecycle_valid = source_found;
        }
        bool target_allowed = lifecycle_valid && target != NULL;
        if (target_allowed && strcmp(message.target_origin, "*") != 0) {
            char receiver_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT] = {0};
            if (target_frame != NULL && target_frame->opaque_origin) {
                snprintf(receiver_origin, sizeof(receiver_origin), "null");
            } else {
                const char *receiver_url = target == session->page.runtime
                    ? session->page.document_url : target_frame->url;
                (void) url_origin(receiver_url, receiver_origin,
                                  sizeof(receiver_origin));
            }
            target_allowed = strcmp(message.target_origin,
                                    receiver_origin) == 0;
        }
        session->last_frame_message_sequence = message.sequence;
        session->last_frame_message_source = source_handle;
        session->last_frame_message_target =
            target == session->page.runtime ? 0 : message.target_frame_handle;
        bool ok = !target_allowed || script_runtime_dispatch_message(
            target, message.json, message.source_origin, source_handle,
            target == session->page.runtime
                ? &session->page.script_result
                : &target_frame->script_result);
        budget_free(session->budget, message.target_origin);
        budget_free(session->budget, message.json);
        if (!ok && target_frame != NULL) {
            /* A failed child browsing context is contained like a crashed
               renderer process. It must not turn a later postMessage into a
               top-level navigation failure or leave queued pointers to the
               realm which is about to be destroyed. */
            navigation_drop_frame_messages(session, target_frame);
            if (getenv("TILEFINCH_TRACE_JS_ROOTS") != NULL
                && target_frame->runtime != NULL) {
                script_runtime_report_roots(
                    target_frame->runtime, stderr, "frame-failed", "child",
                    target_frame->parent_handle);
            }
            script_runtime_destroy(target_frame->runtime);
            target_frame->runtime = NULL;
            document_destroy(&target_frame->document);
            target_frame->loaded = false;
            session->frames_failed++;
            session->frame_messages_dropped++;
            delivered++;
            continue;
        }
        if (!ok) return false;
        if (target_allowed) session->frame_messages_delivered++;
        else session->frame_messages_dropped++;
        delivered++;
    }
    return true;
}

static bool navigation_frame_element_connected(
    const NavigationSession *session, const NavigationFrame *frame)
{
    if (session == NULL || frame == NULL || session->page.runtime == NULL
        || frame->parent_handle <= 0) return false;
    lxb_dom_node_t *node = script_runtime_node_handle_resolve(
        session->page.runtime, frame->parent_handle);
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type == LXB_DOM_NODE_TYPE_DOCUMENT) return true;
    }
    return false;
}

static void navigation_drop_frame_messages(NavigationSession *session,
                                           const NavigationFrame *frame)
{
    size_t write = 0;
    for (size_t read = 0; read < session->page.message_count; read++) {
        NavigationMessage *message = &session->page.messages[read];
        bool drop = (message->source_frame_handle == frame->parent_handle
                     && message->source_generation
                            == frame->lifecycle_generation)
            || (message->source == session->page.runtime
                && message->target_frame_handle == frame->parent_handle
                && message->target_generation
                       == frame->lifecycle_generation);
        if (drop) {
            budget_free(session->budget, message->json);
            budget_free(session->budget, message->target_origin);
            session->frame_messages_dropped++;
            continue;
        }
        if (write != read) session->page.messages[write] = *message;
        write++;
    }
    for (size_t i = write; i < session->page.message_count; i++) {
        memset(&session->page.messages[i], 0,
               sizeof(session->page.messages[i]));
    }
    session->page.message_count = write;
}

static bool navigation_retire_detached_frames(NavigationSession *session)
{
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (frame->retired
            || navigation_frame_element_connected(session, frame)) continue;
        if (frame->fetch_pending) {
            (void) fetch_scheduler_cancel(session->page.resource_scheduler,
                                          frame->fetch_request_id,
                                          "iframe detached");
            (void) fetch_scheduler_discard(session->page.resource_scheduler,
                                           frame->fetch_request_id);
            frame->fetch_pending = false;
        }
        navigation_drop_frame_messages(session, frame);
        if (session->page.runtime != NULL
            && !script_runtime_set_frame_window_state(
                session->page.runtime, frame->parent_handle, false, false,
                false,
                NULL, &session->page.script_result)) return false;
        if (getenv("TILEFINCH_TRACE_JS_ROOTS") != NULL
            && frame->runtime != NULL) {
            script_runtime_report_roots(frame->runtime, stderr,
                                        "pre-detach", "child",
                                        frame->parent_handle);
        }
        script_runtime_destroy(frame->runtime);
        frame->runtime = NULL;
        document_destroy(&frame->document);
        frame->loaded = false;
        frame->retired = true;
        session->frames_detached++;
    }
    return true;
}

static bool node_name_is(lxb_dom_node_t *node, const char *wanted)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    return name != NULL && strlen(wanted) == length
           && memcmp(name, wanted, length) == 0;
}

static bool navigation_token_contains(const char *value, size_t length,
                                      const char *wanted);

static bool navigation_is_stylesheet_link(lxb_dom_node_t *node)
{
    if (!node_name_is(node, "link")) return false;
    size_t rel_length = 0;
    const char *rel = document_attribute(node, "rel", &rel_length);
    return rel != NULL
        && navigation_token_contains(rel, rel_length, "stylesheet");
}

static lxb_dom_node_t *navigation_next_document_node(lxb_dom_node_t *root,
                                                     lxb_dom_node_t *node)
{
    if (node->first_child != NULL) return node->first_child;
    while (node != root && node->next == NULL) node = node->parent;
    return node == root ? NULL : node->next;
}

static bool navigation_document_resource_base(PocDocument *document,
                                              const char *document_url,
                                              char output[
                                                  NAVIGATION_URL_LIMIT])
{
    return document_base_url(document, document_url, output,
                             NAVIGATION_URL_LIMIT);
}

static uint64_t navigation_hash_bytes(const char *bytes, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= length;
    hash *= UINT64_C(1099511628211);
    return hash == 0 ? UINT64_C(1) : hash;
}

static bool navigation_resolve_stylesheet_link(
    lxb_dom_node_t *node, const char *base_url, char resolved[4096],
    uint64_t *resolved_hash)
{
    if (resolved_hash != NULL) *resolved_hash = 0;
    size_t href_length = 0;
    const char *href = document_attribute(node, "href", &href_length);
    if (href == NULL || href_length >= 2048) {
        if (resolved_hash != NULL && href != NULL) {
            size_t bounded = href_length < 2048 ? href_length : 2048;
            *resolved_hash = navigation_hash_bytes(href, bounded)
                ^ (uint64_t) href_length;
            if (*resolved_hash == 0) *resolved_hash = UINT64_C(1);
        }
        return false;
    }
    char reference[2048];
    memcpy(reference, href, href_length);
    reference[href_length] = '\0';
    if (!fetch_resolve_url(base_url, reference, resolved, 4096)) {
        if (resolved_hash != NULL) {
            *resolved_hash = navigation_hash_bytes(href, href_length);
        }
        return false;
    }
    if (resolved_hash != NULL) {
        *resolved_hash = navigation_hash_bytes(resolved, strlen(resolved));
    }
    return true;
}

static bool navigation_stylesheet_event_known(
    const NavigationStylesheetEventRecord *records, size_t count,
    lxb_dom_node_t *node, uint64_t resolved_hash)
{
    for (size_t i = 0; i < count; i++) {
        if (records[i].element == node
            && records[i].resolved_url_hash == resolved_hash) return true;
    }
    return false;
}

static bool navigation_remember_stylesheet_event(
    NavigationStylesheetEventRecord *records, size_t *count,
    lxb_dom_node_t *node, uint64_t resolved_hash)
{
    for (size_t i = 0; i < *count; i++) {
        if (records[i].element != node) continue;
        records[i].resolved_url_hash = resolved_hash;
        return true;
    }
    if (*count >= NAVIGATION_STYLESHEET_EVENT_LIMIT) return false;
    records[*count] = (NavigationStylesheetEventRecord) {
        .element = node, .resolved_url_hash = resolved_hash
    };
    (*count)++;
    return true;
}

static void navigation_adopt_stylesheet_events(
    const PocDocument *document, const char *base_url,
    NavigationStylesheetEventRecord *records, size_t *count)
{
    if (document == NULL || document->html == NULL || base_url == NULL
        || records == NULL || count == NULL) return;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    for (lxb_dom_node_t *node = root; node != NULL;
         node = navigation_next_document_node(root, node)) {
        if (!navigation_is_stylesheet_link(node)) continue;
        char resolved[4096];
        uint64_t hash = 0;
        (void) navigation_resolve_stylesheet_link(
            node, base_url, resolved, &hash);
        if (hash != 0) {
            (void) navigation_remember_stylesheet_event(
                records, count, node, hash);
        }
    }
}

static void navigation_prune_stylesheet_events(NavigationPage *page)
{
    if (page == NULL || page->document.html == NULL
        || page->stylesheet_event_count == 0) return;
    bool connected[NAVIGATION_STYLESHEET_EVENT_LIMIT] = {0};
    lxb_dom_node_t *root = lxb_dom_interface_node(page->document.html);
    for (lxb_dom_node_t *node = root; node != NULL;
         node = navigation_next_document_node(root, node)) {
        if (!navigation_is_stylesheet_link(node)) continue;
        for (size_t i = 0; i < page->stylesheet_event_count; i++) {
            if (page->stylesheet_events[i].element == node) {
                connected[i] = true;
            }
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < page->stylesheet_event_count; i++) {
        if (connected[i]) page->stylesheet_events[kept++] =
            page->stylesheet_events[i];
    }
    memset(page->stylesheet_events + kept, 0,
           (page->stylesheet_event_count - kept)
               * sizeof(*page->stylesheet_events));
    page->stylesheet_event_count = kept;
}

static StylesheetDocumentResourceState navigation_stylesheet_link_state(
    const NavigationSession *session, lxb_dom_node_t *node,
    const char *base_url)
{
    size_t href_length = 0;
    const char *href = document_attribute(node, "href", &href_length);
    return stylesheet_document_resources_link_state(
        &session->page.stylesheet_resources, base_url,
        href, href_length);
}

/* Link stylesheet completion is an author-observable browser event.  Initial
   parser-owned links have already completed before the author runtime starts,
   so they are adopted without a late event.  Links appended by scripts are
   dispatched only after the bounded stylesheet ledger reaches a terminal
   state.  Recording the node before calling JavaScript makes re-entrant DOM
   work exactly-once without adding private attributes to the page. */
static bool navigation_dispatch_stylesheet_events(
    NavigationSession *session, const char *base_url)
{
    if (session == NULL || session->page.runtime == NULL
        || session->page.document.html == NULL || base_url == NULL) {
        return true;
    }
    for (size_t dispatched = 0;
         dispatched < NAVIGATION_STYLESHEET_EVENT_LIMIT; dispatched++) {
        navigation_prune_stylesheet_events(&session->page);
        lxb_dom_node_t *selected = NULL;
        const char *event_type = NULL;
        uint64_t selected_hash = 0;
        lxb_dom_node_t *root = lxb_dom_interface_node(
            session->page.document.html);
        for (lxb_dom_node_t *node = root; node != NULL;
             node = navigation_next_document_node(root, node)) {
            if (!navigation_is_stylesheet_link(node)) continue;
            char resolved[4096];
            uint64_t hash = 0;
            (void) navigation_resolve_stylesheet_link(
                node, base_url, resolved, &hash);
            if (hash == 0 || navigation_stylesheet_event_known(
                    session->page.stylesheet_events,
                    session->page.stylesheet_event_count, node, hash)) {
                continue;
            }
            StylesheetDocumentResourceState state =
                navigation_stylesheet_link_state(
                    session, node, base_url);
            if (state != STYLESHEET_DOCUMENT_RESOURCE_LOADED
                && state
                    != STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE) {
                continue;
            }
            selected = node;
            selected_hash = hash;
            event_type = state == STYLESHEET_DOCUMENT_RESOURCE_LOADED
                ? "load" : "error";
            break;
        }
        if (selected == NULL) return true;
        if (!navigation_remember_stylesheet_event(
                session->page.stylesheet_events,
                &session->page.stylesheet_event_count,
                selected, selected_hash)) return true;
        /* Dispatch exactly one callback before rescanning.  Author code and
           its promise jobs may replace the entire head, invalidating every
           other node pointer which existed at selection time. */
        if (!script_runtime_dispatch_node(
                session->page.runtime, selected, event_type,
                &session->page.script_result)) return false;
    }
    return true;
}

static bool copy_script_context(lxb_dom_node_t *node, size_t offset,
                                char *output, size_t output_size)
{
    for (; node != NULL; node = node->next) {
        if (node_name_is(node, "script")) {
            for (lxb_dom_node_t *child = node->first_child; child != NULL;
                 child = child->next) {
                size_t length = 0;
                const char *source = document_text_data(child, &length);
                if (source == NULL || offset == 0 || offset > length) continue;
                size_t center = offset - 1;
                size_t start = center > 180 ? center - 180 : 0;
                size_t end = center + 260 < length ? center + 260 : length;
                size_t written = 0;
                for (size_t i = start; i < end && written + 1 < output_size;
                     i++) {
                    unsigned char character = (unsigned char) source[i];
                    output[written++] = character < 32 || character == 127
                                        ? ' ' : (char) character;
                }
                output[written] = '\0';
                return true;
            }
        }
        if (node->first_child != NULL
            && copy_script_context(node->first_child, offset, output,
                                   output_size)) return true;
    }
    return false;
}

static void collect_stack_source_context_into(
    NavigationFrame *frame, const char *stack, char *output,
    size_t output_size)
{
    static const char marker[] = "<inline-script>:1:";
    const char *at = stack;
    if (frame->document.html == NULL || output == NULL
        || output_size == 0) return;
    for (size_t index = 0; index < 5; index++) {
        at = strstr(at, marker);
        if (at == NULL) break;
        at += sizeof(marker) - 1;
        char *end = NULL;
        unsigned long offset = strtoul(at, &end, 10);
        if (end == at || offset == 0) break;
        char context[512] = {0};
        if (copy_script_context(
                lxb_dom_interface_node(frame->document.html),
                (size_t) offset, context, sizeof(context))) {
            size_t used = strlen(output);
            if (used < output_size) {
                snprintf(output + used, output_size - used,
                         "%s[%lu] %s", used == 0 ? "" : " || ",
                         offset, context);
            }
        }
        at = end;
    }
}

static void collect_stack_source_context(NavigationFrame *frame,
                                         const char *stack)
{
    collect_stack_source_context_into(
        frame, stack, frame->reject_source_context,
        sizeof(frame->reject_source_context));
}

static bool stack_function_name(const char *stack,
                                 char *output, size_t output_size)
{
    const char *inline_marker = strstr(stack, " (<inline-script>");
    if (inline_marker == NULL || output_size < 2) return false;
    const char *start = inline_marker;
    while (start > stack
           && start[-1] != ' ' && start[-1] != '\n') start--;
    size_t length = (size_t) (inline_marker - start);
    if (length == 0 || length >= output_size) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) start[i];
        if (!(character == '_' || character == '$'
              || (character >= 'A' && character <= 'Z')
              || (character >= 'a' && character <= 'z')
              || (i != 0 && character >= '0' && character <= '9'))) {
            return false;
        }
    }
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool stack_generated_function_name(const char *stack,
                                          char *output,
                                          size_t output_size)
{
    const char *marker = strstr(stack, " (<input>:");
    if (marker == NULL || output_size < 2) return false;
    const char *start = marker;
    while (start > stack && start[-1] != ' ' && start[-1] != '\n') start--;
    size_t length = (size_t) (marker - start);
    if (length == 0 || length >= output_size) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char) start[i];
        if (!(character == '_' || character == '$'
              || (character >= 'A' && character <= 'Z')
              || (character >= 'a' && character <= 'z')
              || (i != 0 && character >= '0' && character <= '9'))) {
            return false;
        }
    }
    memcpy(output, start, length);
    output[length] = '\0';
    return true;
}

static bool stack_generated_location(const char *stack,
                                     unsigned long *line,
                                     unsigned long *column)
{
    const char *marker = strstr(stack, "<input>:");
    if (marker == NULL || line == NULL || column == NULL) return false;
    marker += strlen("<input>:");
    char *end = NULL;
    *line = strtoul(marker, &end, 10);
    if (end == marker || *end++ != ':' || *line == 0) return false;
    const char *column_start = end;
    *column = strtoul(column_start, &end, 10);
    return end != column_start && *column != 0;
}

static bool collect_iframes(lxb_dom_node_t *node,
                            lxb_dom_node_t **frames, size_t capacity,
                            size_t *count)
{
    if (node == NULL) return true;
    lxb_dom_node_t *boundary_parent = node->parent;
    size_t visited = 0;
    while (node != NULL && node != boundary_parent && *count < capacity) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        if (node_name_is(node, "iframe")) {
            if (*count < capacity) frames[(*count)++] = node;
        }
        if (*count == capacity) break;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != boundary_parent
               && node->next == NULL) node = node->parent;
        if (node == NULL || node == boundary_parent) break;
        node = node->next;
    }
    return true;
}

static bool frame_element_was_discovered(const NavigationPage *page,
                                         const lxb_dom_node_t *node)
{
    for (size_t i = 0; i < page->discovered_frame_element_count; i++) {
        if (page->discovered_frame_elements[i] == node) return true;
    }
    return false;
}

static bool remember_discovered_frame(NavigationPage *page,
                                      lxb_dom_node_t *node)
{
    if (frame_element_was_discovered(page, node)) return false;
    if (page->discovered_frame_element_count
        >= NAVIGATION_FRAME_DISCOVERY_LIMIT) return false;
    page->discovered_frame_elements[page->discovered_frame_element_count++] =
        node;
    return true;
}

static TilefinchRequestContext navigation_frame_request_context(
    const NavigationFrame *frame)
{
    return (TilefinchRequestContext) {
        .target_url = frame == NULL ? NULL : frame->url,
        .initiator_url = frame == NULL ? NULL : frame->parent_url,
        /* This implementation discovers only frames in the top-level
           document, so parent_url is also the candidate top-level URL. Do
           not consult history here: the candidate is not published until
           frame setup and all other fallible commit work succeeds. */
        .top_level_url = frame == NULL ? NULL : frame->parent_url,
        .method = "GET",
        .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_FRAME
    };
}

static uint64_t navigation_enqueue_frame_fetch(
    NavigationSession *session, NavigationFrame *frame,
    const char *client_hint_tokens, const char *client_hint_origin)
{
    if (session == NULL || frame == NULL
        || session->page.resource_scheduler == NULL) return 0;
    TilefinchRequestContext request_context =
        navigation_frame_request_context(frame);
    FetchRequest transport = {
        .allow_http_errors = true,
        .send_client_hints = client_hint_tokens != NULL,
        .client_hint_tokens = client_hint_tokens,
        .client_hint_origin = client_hint_origin,
        .send_low_client_hints = true,
        .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        .upgrade_insecure_requests = true,
        .user_agent = session->diagnostic_frame_safari
            ? "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Mobile/15E148 Safari/604.1"
            : NULL
    };
    FetchPreparedPageRequest prepared;
    if (!fetch_prepare_page_request_context(
            &request_context, frame->parent_url,
            session->page.referrer_policy, session->browser_session,
            &session->page.document.content_security_policy, NULL,
            &transport, &prepared, NULL)) return 0;
    const FetchRequest *request = fetch_prepared_page_request(&prepared);
    if (request == NULL) return 0;
    return fetch_scheduler_enqueue(
        session->page.resource_scheduler, frame->url, request,
        512 * 1024, 15000);
}

static bool navigation_load_frames(NavigationSession *session,
                                   const char *parent_url)
{
    if (session->page.runtime == NULL || parent_url == NULL) return true;
    lxb_dom_node_t *nodes[NAVIGATION_FRAME_DISCOVERY_LIMIT] = {0};
    size_t count = 0;
    if (!collect_iframes(
            lxb_dom_interface_node(session->page.document.html), nodes,
            NAVIGATION_FRAME_DISCOVERY_LIMIT, &count)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "frame discovery traversal exceeded %u nodes",
                 NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT);
        return false;
    }
    if (count == 0) return true;
    if (!script_runtime_configure_messaging(
            session->page.runtime, false, true, false,
            navigation_queue_frame_message,
            session)) return false;
    for (size_t i = 0; i < count; i++) {
        size_t srcdoc_length = 0;
        const char *srcdoc = document_attribute(
            nodes[i], "srcdoc", &srcdoc_length);
        /* Same-origin srcdoc/about:blank/Blob frames live in the bounded
           bootstrap frame realm. They must never also enqueue a remote
           fetch or dispatch a second load/error event here. */
        if (srcdoc != NULL) continue;
        long parent_handle = script_runtime_node_handle(
            session->page.runtime, nodes[i]);
        NavigationFrame *frame = NULL;
        for (size_t slot = 0; slot < session->page.frame_count; slot++) {
            NavigationFrame *candidate = &session->page.frames[slot];
            if (candidate->parent_handle == parent_handle) {
                frame = candidate;
                break;
            }
        }
        bool known = frame_element_was_discovered(&session->page, nodes[i]);
        size_t source_length = 0;
        const char *source = document_attribute(nodes[i], "src",
                                                &source_length);
        size_t sandbox_length = 0;
        const char *sandbox = document_attribute(
            nodes[i], "sandbox", &sandbox_length);
        uint32_t sandbox_flags = tilefinch_frame_sandbox_parse(
            sandbox, sandbox_length, sandbox != NULL);
        /* Missing src is the local about:blank document installed by the
           bootstrap. It is not a network failure. A later src mutation is
           intentionally reconsidered because no C frame slot is retained. */
        if (source == NULL || source_length == 0
            || source_length >= NAVIGATION_URL_LIMIT) continue;
        char reference[NAVIGATION_URL_LIMIT];
        memcpy(reference, source, source_length);
        reference[source_length] = '\0';
        char resolved_url[NAVIGATION_URL_LIMIT];
        if (!fetch_resolve_url(parent_url, reference, resolved_url,
                               sizeof(resolved_url))) {
            session->frames_failed++;
            continue;
        }
        if (frame != NULL && !frame->retired
            && strcmp(frame->url, resolved_url) == 0
            && frame->sandbox_flags == sandbox_flags) continue;
        if (!known && !remember_discovered_frame(&session->page, nodes[i])) {
            continue;
        }
        session->frames_discovered++;
        if (frame != NULL && !frame->retired) {
            if (frame->fetch_pending) {
                (void) fetch_scheduler_cancel(
                    session->page.resource_scheduler,
                    frame->fetch_request_id, "iframe source changed");
                (void) fetch_scheduler_discard(
                    session->page.resource_scheduler,
                    frame->fetch_request_id);
            }
            navigation_drop_frame_messages(session, frame);
            script_runtime_destroy(frame->runtime);
            document_destroy(&frame->document);
            frame->runtime = NULL;
            frame->fetch_pending = false;
            frame->loaded = false;
            frame->retired = true;
        }
        if (frame == NULL) {
            for (size_t slot = 0; slot < session->page.frame_count; slot++) {
                if (session->page.frames[slot].retired) {
                    frame = &session->page.frames[slot];
                    break;
                }
            }
        }
        bool append_frame = frame == NULL;
        if (append_frame) {
            if (session->page.frame_count >= NAVIGATION_FRAME_LIMIT) {
                session->frames_failed++;
                (void) navigation_dispatch_node_event(session, nodes[i],
                                                       "error");
                continue;
            }
            frame = &session->page.frames[session->page.frame_count];
        } else {
            /* Retirement has already released every owned pointer. Reusing
               the fixed slot keeps repeated iframe attach/detach bounded. */
            memset(frame, 0, sizeof(*frame));
        }
        snprintf(frame->url, sizeof(frame->url), "%s", resolved_url);
        snprintf(frame->parent_url, sizeof(frame->parent_url), "%s",
                 parent_url);
        frame->element = nodes[i];
        frame->parent_handle = parent_handle;
        frame->sandbox_flags = sandbox_flags;
        frame->opaque_origin = !tilefinch_frame_sandbox_allows(
            sandbox_flags, TILEFINCH_SANDBOX_ALLOW_SAME_ORIGIN);
        if (session->next_frame_lifecycle_generation != UINT64_MAX) {
            session->next_frame_lifecycle_generation++;
        }
        frame->lifecycle_generation =
            session->next_frame_lifecycle_generation;
        if (session->page.resource_scheduler == NULL) {
            session->frames_failed++;
            (void) navigation_dispatch_node_event(session, nodes[i],
                                                  "error");
            continue;
        }
        frame->fetch_request_id = navigation_enqueue_frame_fetch(
            session, frame, NULL, NULL);
        frame->fetch_pending = frame->fetch_request_id != 0;
        if (append_frame) session->page.frame_count++;
        char parent_origin[NAVIGATION_URL_LIMIT] = {0};
        char child_origin[NAVIGATION_URL_LIMIT] = {0};
        bool same_origin = url_origin(parent_url, parent_origin,
                                      sizeof(parent_origin))
            && url_origin(frame->url, child_origin, sizeof(child_origin))
            && strcmp(parent_origin, child_origin) == 0
            && !frame->opaque_origin;
        if (!script_runtime_set_frame_window_state(
                session->page.runtime, frame->parent_handle, true,
                same_origin, frame->opaque_origin, frame->url,
                &session->page.script_result)) return false;
        if (!frame->fetch_pending) {
            session->frames_failed++;
            (void) navigation_dispatch_node_event(session, nodes[i],
                                                  "error");
        }
    }
    return navigation_deliver_frame_messages(session);
}

static bool navigation_complete_frame(NavigationSession *session,
                                      NavigationFrame *frame,
                                      bool fetched_ok, FetchResult *fetch)
{
    frame->fetch_pending = false;
    frame->http_status = fetch->status_code;
    snprintf(frame->server, sizeof(frame->server), "%s", fetch->server);
    snprintf(frame->cf_ray, sizeof(frame->cf_ray), "%s", fetch->cf_ray);
    bool referrer_policy_header_present = false;
    if (!fetch_response_referrer_policy(
            fetch, &referrer_policy_header_present,
            frame->referrer_policy)) {
        /* A live/v9 result carries this policy outside the bounded ordinary
           header snapshot. Corrupt direct metadata is a response failure,
           not permission to fall back to a possibly truncated field list. */
        fetched_ok = false;
        frame->referrer_policy[0] = '\0';
    }
    TilefinchRequestContext request_context =
        navigation_frame_request_context(frame);
    if (fetched_ok) {
        navigation_accept_response_cookies(
            session->browser_session, &request_context, fetch,
            fetch->effective_url[0] == '\0'
                ? frame->url : fetch->effective_url);
    }
    const char *effective = fetch->effective_url[0] == '\0'
        ? frame->url : fetch->effective_url;
    char accepted_critical[1024] = {0};
    bool retry_client_hints = fetched_ok
        && !frame->client_hint_retried
        && tilefinch_url_potentially_trustworthy(effective)
        && fetch_accepted_critical_client_hints(
            fetch->accept_ch, fetch->critical_ch,
            accepted_critical, sizeof(accepted_critical))
        && accepted_critical[0] != '\0';
    if (retry_client_hints) {
        char retry_url[NAVIGATION_URL_LIMIT];
        if (!tilefinch_url_request_key(effective, retry_url,
                                    sizeof(retry_url))) {
            fetch_result_destroy(fetch);
            session->frames_failed++;
            (void) navigation_dispatch_node_event(
                session, frame->element, "error");
            return true;
        }
        char hint_origin[TILEFINCH_ORIGIN_SERIALIZED_LIMIT];
        if (!tilefinch_url_origin(retry_url, hint_origin,
                              sizeof(hint_origin))) {
            fetch_result_destroy(fetch);
            session->frames_failed++;
            (void) navigation_dispatch_node_event(
                session, frame->element, "error");
            return true;
        }
        /* Critical-CH belongs to the final response origin, not to an earlier
           redirector.  Restart that canonical URL directly so high-entropy
           hints and response-path cookies are sent only to the origin which
           requested them.  parent_url remains the immutable frame initiator
           used for SameSite, Referrer, and Sec-Fetch-Site decisions. */
        snprintf(frame->url, sizeof(frame->url), "%s", retry_url);
        frame->client_hint_retried = true;
        session->client_hint_retries++;
        frame->fetch_request_id = navigation_enqueue_frame_fetch(
            session, frame, accepted_critical, hint_origin);
        fetch_result_destroy(fetch);
        frame->fetch_pending = frame->fetch_request_id != 0;
        if (!frame->fetch_pending) {
            session->frames_failed++;
            (void) navigation_dispatch_node_event(
                session, frame->element, "error");
        }
        return true;
    }
    char final_url[NAVIGATION_URL_LIMIT];
    snprintf(final_url, sizeof(final_url), "%s", effective);
    bool final_same_origin = tilefinch_url_same_origin(
        frame->parent_url, final_url);
    bool exposed_same_origin = final_same_origin && !frame->opaque_origin;
    bool scripts_allowed = tilefinch_frame_sandbox_allows(
        frame->sandbox_flags, TILEFINCH_SANDBOX_ALLOW_SCRIPTS);
    TilefinchContentSecurityPolicy response_csp;
    bool security_snapshot_truncated =
        fetch->security.version == FETCH_RESPONSE_SECURITY_METADATA_VERSION
        ? fetch->security.csp_snapshot_state
              == FETCH_SECURITY_FIELD_TRUNCATED
          || fetch->security.frame_policy_snapshot_state
              == FETCH_SECURITY_FIELD_TRUNCATED
        : fetch->response_security_headers_truncated;
    bool response_policy_valid = tilefinch_csp_parse_response_headers(
        &response_csp, final_url, fetch->response_headers,
        fetch->response_headers_length,
        security_snapshot_truncated);
    bool embedding_allowed = response_policy_valid
        && tilefinch_frame_embedding_allowed(
               &response_csp, final_url, frame->parent_url,
               fetch->response_headers, fetch->response_headers_length);
    bool ok = fetched_ok && fetch->status_code >= 200
              && fetch->status_code < 400
              && embedding_allowed
              && document_parse(&frame->document, session->budget,
                                fetch->data, fetch->length, 512);
    if (ok) {
        frame->document.content_security_policy = response_csp;
    }
    fetch_result_destroy(fetch);
    if (ok) {
        /* Redirects change both the child document URL and the WindowProxy
           origin boundary. Do not retain the requested URL's permissions. */
        snprintf(frame->url, sizeof(frame->url), "%s", final_url);
        ok = script_runtime_set_frame_window_state(
            session->page.runtime, frame->parent_handle, true,
            exposed_same_origin, frame->opaque_origin, final_url,
            &session->page.script_result);
    }
    if (ok && scripts_allowed) {
        frame->runtime = navigation_create_runtime(
            session, &frame->document, frame->url,
            frame->referrer_policy, true, &session->viewport,
            SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME, false,
            session->page.fetch_domain,
            NULL,
            &frame->script_result);
        ok = frame->runtime != NULL
            && script_runtime_configure_messaging(
                frame->runtime, true, exposed_same_origin,
                frame->opaque_origin,
                navigation_queue_frame_message,
                session);
    }
    if (ok && frame->runtime != NULL && session->trace_frame_capabilities) {
        static const char trace_setup[] =
            "(()=>{const seen=new Map(),missing=new Set();"
            "globalThis.__tilefinchCapabilityTrace={seen,missing};"
            "globalThis.__tilefinchTraceObject=(target,label)=>new Proxy(target,{"
            "get(object,key,receiver){if(typeof key!=='symbol'){const name=label+'.'+String(key);seen.set(name,(seen.get(name)||0)+1);if(!(key in object))missing.add(name);}return Reflect.get(object,key,receiver);},"
            "set(object,key,value,receiver){if(typeof key!=='symbol'){const name=label+'.'+String(key)+'=';seen.set(name,(seen.get(name)||0)+1);if(!(key in object))missing.add(name);}return Reflect.set(object,key,value,receiver);}});"
            "for(const name of ['document','navigator','performance','crypto','screen','history'])if(globalThis[name])globalThis[name]=__tilefinchTraceObject(globalThis[name],name);"
            "for(const name of ['XMLHttpRequest','PerformanceObserver','Blob','URL','Worker','MutationObserver'])if(typeof globalThis[name]==='function')globalThis[name]=__tilefinchTraceObject(globalThis[name],name);})();";
        ok = script_runtime_evaluate_diagnostic(
            frame->runtime, trace_setup, "<frame-capability-trace>",
            &frame->script_result);
    }
    if (ok && frame->runtime != NULL) {
        ExternalScriptMetrics metrics = {0};
        char frame_base_url[NAVIGATION_URL_LIMIT];
        ok = navigation_document_resource_base(
            &frame->document, frame->url, frame_base_url);
        if (ok) {
            ok = document_scripts_execute(
                &frame->document, frame->runtime, session->budget,
                session->browser_session, frame_base_url, frame->url,
                frame->referrer_policy, 16, 1024 * 1024,
                512 * 1024, 15000, session->page.resource_scheduler,
                &metrics, &frame->script_result)
                && script_runtime_finish_loading(frame->runtime,
                                                 &frame->script_result);
        }
    }
    if (!ok) {
        session->frames_failed++;
        (void) navigation_dispatch_node_event(session, frame->element,
                                              "error");
        return true;
    }
    frame->loaded = true;
    session->frames_loaded++;
    if (getenv("TILEFINCH_TRACE_JS_ROOTS") != NULL) {
        script_runtime_report_roots(frame->runtime, stderr, "frame-loaded",
                                    "child", frame->parent_handle);
    }
    (void) navigation_dispatch_node_event(session, frame->element, "load");
    return true;
}

static bool navigation_progress_frames(NavigationSession *session)
{
    if (!navigation_retire_detached_frames(session)) return false;
    if (session->page.resource_scheduler == NULL) return true;
    (void) fetch_scheduler_pump(session->page.resource_scheduler,
                                NAVIGATION_FRAME_LIMIT, 4);
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (!frame->fetch_pending) continue;
        bool fetched_ok = false;
        FetchResult *fetch = fetch_result_create(session->budget);
        if (fetch == NULL) return false;
        if (!fetch_scheduler_take(session->page.resource_scheduler,
                                  frame->fetch_request_id,
                                  &fetched_ok, fetch)) {
            fetch_result_free(fetch);
            continue;
        }
        if (!navigation_complete_frame(session, frame, fetched_ok, fetch)) {
            fetch_result_free(fetch);
            return false;
        }
        fetch_result_free(fetch);
    }
    return navigation_deliver_frame_messages(session);
}

bool navigation_collect_frame_capability_trace(NavigationSession *session)
{
    if (session == NULL || !session->trace_frame_capabilities) return true;
    static const char report[] =
        "{globalThis.__tilefinchFrameSummaryBeforeTrace=globalThis.pocSummary;"
        "const trace=globalThis.__tilefinchCapabilityTrace,"
        "missing=trace?[...trace.missing].filter(name=>"
        "!/^element[.][a-z0-9-]+[.]__(handle|shadow)/.test(name)).sort():[],"
        "seen=trace?[...trace.seen.entries()].sort((a,b)=>"
        "b[1]-a[1]||a[0].localeCompare(b[0])).slice(0,64):[];"
        "globalThis.pocSummary='missing='+missing.join(',')+'|seen='+"
        "seen.map(item=>item[0]+'*'+item[1]).join(',');}";
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        char function_name[64] = {0};
        if (frame->runtime != NULL
            && stack_function_name(frame->script_result.last_reject_stack,
                                   function_name,
                                    sizeof(function_name))) {
            char select_function[320];
            snprintf(select_function, sizeof(select_function),
                     "try{globalThis.__tilefinchRejectFunction="
                     "__tilefinchDiagnosticLookup('%s')}catch(error){"
                     "globalThis.__tilefinchRejectLookupError=String(error)}",
                     function_name);
            if (!script_runtime_evaluate_diagnostic(
                    frame->runtime, select_function,
                    "<frame-reject-function>", &frame->script_result)) {
                return false;
            }
        }
        if (frame->runtime != NULL) {
            if (!script_runtime_evaluate_diagnostic(
                    frame->runtime, report, "<frame-capability-report>",
                    &frame->script_result)) return false;
            snprintf(frame->capability_trace,
                     sizeof(frame->capability_trace), "%s",
                     frame->script_result.summary);
            snprintf(session->last_frame_trace,
                     sizeof(session->last_frame_trace), "%s",
                     frame->capability_trace);
            snprintf(session->last_frame_trace_url,
                     sizeof(session->last_frame_trace_url), "%s",
                     frame->url);
            static const char restore[] =
                "globalThis.pocSummary="
                "globalThis.__tilefinchFrameSummaryBeforeTrace;";
            (void) script_runtime_evaluate_diagnostic(
                frame->runtime, restore, "<frame-capability-restore>",
                &frame->script_result);
        }
        collect_stack_source_context(frame,
            frame->script_result.last_reject_stack);
    }
    return true;
}
