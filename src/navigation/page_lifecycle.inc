static uint64_t navigation_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

struct NavigationFramePresentation {
    Stylesheet stylesheet;
    LayoutDocument layout;
    int width;
    int height;
};

static bool navigation_refresh_frame_presentation(
    NavigationSession *session, NavigationFrame *frame);
static bool navigation_refresh_frame_presentation_or_defer(
    NavigationSession *session, NavigationFrame *frame);

static NavigationFrame *navigation_frame_parent(
    NavigationSession *session, const NavigationFrame *frame)
{
    if (session == NULL || frame == NULL
        || frame->parent_frame_slot_plus_one == 0) return NULL;
    size_t slot = frame->parent_frame_slot_plus_one - 1u;
    if (slot >= session->page.frame_count) return NULL;
    NavigationFrame *parent = &session->page.frames[slot];
    return parent->retired ? NULL : parent;
}

static ScriptResult *navigation_frame_parent_result(
    NavigationSession *session, const NavigationFrame *frame)
{
    NavigationFrame *parent = navigation_frame_parent(session, frame);
    return parent == NULL ? (session == NULL ? NULL
                                             : &session->page.script_result)
                          : &parent->script_result;
}

static const LayoutDocument *navigation_frame_parent_layout(
    NavigationSession *session, const NavigationFrame *frame)
{
    NavigationFrame *parent = navigation_frame_parent(session, frame);
    if (parent == NULL) return session == NULL ? NULL : &session->page.layout;
    return parent->presentation == NULL ? NULL
                                        : &parent->presentation->layout;
}

static ImageResources *navigation_frame_parent_images(
    NavigationSession *session, const NavigationFrame *frame)
{
    NavigationFrame *parent = navigation_frame_parent(session, frame);
    return parent == NULL ? (session == NULL ? NULL : &session->page.images)
                          : &parent->images;
}

static void navigation_frame_presentation_destroy(
    NavigationSession *session, NavigationFrame *frame)
{
    if (session == NULL || frame == NULL || frame->presentation == NULL) {
        return;
    }
    if (frame->runtime != NULL) {
        script_runtime_set_layout(frame->runtime, NULL, 0);
        script_runtime_set_stylesheet(frame->runtime, NULL);
    }
    layout_destroy(&frame->presentation->layout);
    stylesheet_destroy(&frame->presentation->stylesheet);
    budget_free(session->budget, frame->presentation);
    frame->presentation = NULL;
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
    /* API registrations are document-owned and never reusable across a
       navigation, even when the external source bytes are identical. */
    sheet->api_registrations = NULL;
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
        || page->stylesheet.api_registrations != NULL
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

/* Last-resort layout admission for a complete server-rendered document.
   Unlike navigation_build_usable_stylesheet_fallback(), this deliberately
   does not rescan author <style> payloads: that scan may succeed while
   rebuilding the same multi-megabyte cascade which caused layout admission
   to fail. Inline style attributes remain available through ordinary style
   resolution; document rules are skipped so the DOM can still become a
   readable, navigable static page inside the PSP ceiling. */
static bool navigation_install_minimal_stylesheet_fallback(
    NavigationSession *session)
{
    if (session == NULL) return false;
#if !defined(__PSP__)
    if (tilefinch_test_faults()->refuse_next_minimal_stylesheet) {
        tilefinch_test_faults()->refuse_next_minimal_stylesheet = false;
        return false;
    }
#endif
    stylesheet_destroy(&session->page.stylesheet);
    if (!stylesheet_build_context_deferred(
            &session->page.stylesheet, session->budget,
            &session->page.document, &session->viewport)) return false;
    session->page.stylesheet.document_rules_deferred = false;
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
    layout_reuse_cache_enable_retained_matches(session->page.layout_reuse);
}

/* The layout_reuse_* performance counters are read only by the host lab,
   tests and the validation log; builds without tracing or that log neither
   sample the reuse cache around each build nor record the deltas. */
#if !defined(TILEFINCH_NO_TRACE) || defined(TILEFINCH_PSP_VALIDATION_LOG)
#define NAVIGATION_LAYOUT_REUSE_TELEMETRY 1
#else
#define NAVIGATION_LAYOUT_REUSE_TELEMETRY 0
#endif

static void navigation_layout_reuse_sample(const LayoutReuseCache *cache,
                                           LayoutReuseStats *stats)
{
#if NAVIGATION_LAYOUT_REUSE_TELEMETRY
    layout_reuse_cache_stats(cache, stats);
#else
    (void) cache;
    (void) stats;
#endif
}

static void navigation_record_layout_reuse(
    NavigationSession *session, const LayoutReuseStats *before,
    const LayoutReuseStats *after)
{
#if !NAVIGATION_LAYOUT_REUSE_TELEMETRY
    (void) session;
    (void) before;
    (void) after;
#else
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
    ADD_REUSE_DELTA(matched_hits, layout_reuse_matched_hits);
    ADD_REUSE_DELTA(matched_misses, layout_reuse_matched_misses);
    ADD_REUSE_DELTA(matched_stores, layout_reuse_matched_stores);
#undef ADD_REUSE_DELTA
    session->performance.layout_reuse_retained_bytes =
        after->retained_bytes;
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_MUTATION_JOURNAL") != NULL
        && after->style_misses >= before->style_misses)
        fprintf(stderr, "layout-reuse-build styles=%zu/%zu parent-misses=%zu "
                "evictions=%zu matched=%zu/%zu\n",
                after->style_hits - before->style_hits,
                after->style_misses - before->style_misses,
                after->style_parent_misses - before->style_parent_misses,
                after->style_evictions - before->style_evictions,
                after->matched_hits - before->matched_hits,
                after->matched_misses - before->matched_misses);
#endif
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    if (after->matched_hits != before->matched_hits
        || after->matched_stores != before->matched_stores) {
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-layout-reuse: styles=%zu/%zu matched=%zu/%zu/%zu "
                 "retained=%zu",
                 after->style_hits - before->style_hits,
                 after->style_misses - before->style_misses,
                 after->matched_hits - before->matched_hits,
                 after->matched_misses - before->matched_misses,
                 after->matched_stores - before->matched_stores,
                 after->retained_bytes);
        tilefinch_platform_log_message(message);
    }
#endif
#endif /* NAVIGATION_LAYOUT_REUSE_TELEMETRY */
}

static bool navigation_build_layout(NavigationSession *session,
                                    LayoutDocument *layout,
                                    const FontSet *fonts,
                                    const ImageResources *images)
{
    if (session == NULL || layout == NULL) return false;
    navigation_ensure_layout_reuse(session);
    LayoutReuseStats before = {0}, after = {0};
    navigation_layout_reuse_sample(session->page.layout_reuse, &before);
    layout->performance.cancelled = false;
    bool built = layout_build_context_reuse(
        layout, session->budget, &session->page.document,
        &session->page.stylesheet, fonts, images, &session->viewport,
        session->page.layout_reuse);
    navigation_layout_reuse_sample(session->page.layout_reuse, &after);
    navigation_record_layout_reuse(session, &before, &after);
    session->layout_build_cancelled = !built && layout->performance.cancelled;
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
    if (!session->web_fonts_enabled || session->web_font_mode == WEB_FONTS_OFF
        || session->page.web_font_pressure_fallback) return true;
    return fonts_external_loader_begin(
        &session->page.external_font_loader, &session->page.stylesheet,
        session->font_timeout_ms, &session->page.external_fonts);
}

static ScriptNavigationTiming navigation_timing_from_fetch(
    uint64_t navigation_started_us, uint64_t response_completed_us,
    uint64_t response_headers_elapsed_us, const FetchResult *fetch)
{
    ScriptNavigationTiming timing = {0};
    if (fetch == NULL || navigation_started_us == 0
        || response_completed_us < navigation_started_us) return timing;
    timing.response_complete = true;
    timing.response_end_us = response_completed_us - navigation_started_us;
    timing.measured = fetch->transport_timing.measured;
    timing.encoded_body_bytes_measured =
        fetch->transport_timing.encoded_body_bytes_measured;
    timing.name_lookup_us = fetch->transport_timing.name_lookup_us;
    timing.connect_us = fetch->transport_timing.connect_us;
    timing.appconnect_us = fetch->transport_timing.appconnect_us;
    timing.first_byte_us = fetch->transport_timing.first_byte_us;
    timing.total_us = fetch->transport_timing.total_us;
    timing.encoded_body_bytes =
        fetch->transport_timing.encoded_body_bytes;
    timing.decoded_body_bytes = fetch->length > UINT32_MAX
        ? UINT32_MAX : (uint32_t) fetch->length;
    timing.response_status = fetch->status_code > 0
        && fetch->status_code <= UINT16_MAX
            ? (uint16_t) fetch->status_code : 0;
    timing.next_hop_protocol =
        fetch_http_version_name(fetch->negotiated_http_version);
    timing.content_type = fetch->content_type;
    if (response_headers_elapsed_us != 0) {
        timing.response_start_us = response_headers_elapsed_us;
    } else if (timing.measured && timing.first_byte_us <= timing.total_us) {
        uint64_t final_hop_start_us = timing.total_us <= timing.response_end_us
            ? timing.response_end_us - timing.total_us : 0;
        timing.response_start_us = final_hop_start_us + timing.first_byte_us;
    }
    if (timing.response_start_us > timing.response_end_us) {
        timing.response_start_us = timing.response_end_us;
    }
    return timing;
}

/* Whether a document belongs to a Cloudflare managed challenge, by the two
   signals the native layer owns. The top-level response says so itself with
   `cf-mitigated: challenge`. The challenge (and Turnstile) frame does not
   carry that header, but its document URL is the service's own challenge
   platform. Neither is read from page script, and neither changes what the
   browser sends: this only sizes a heap. */
bool navigation_url_is_challenge_platform(const char *url)
{
    static const char platform[] =
        "https://challenges.cloudflare.com/cdn-cgi/challenge-platform/";
    return url != NULL
        && strncmp(url, platform, sizeof(platform) - 1u) == 0;
}

/* The script-heap limit for one new runtime. Only a child frame is raised.
   The interpreter that needs the room runs in the challenge frame; the
   top-level "Just a moment" document peaks near 5 MB and fits the ordinary
   limit, so it is not given room it does not use. */
size_t navigation_script_heap_limit(const NavigationSession *session,
                                    ScriptDocumentScope document_scope,
                                    const char *url)
{
    if (session == NULL) return 0;
    size_t limit = session->js_memory_limit;
    if (limit == 0 || limit >= NAVIGATION_MANAGED_CHALLENGE_JS_HEAP_BYTES
        || document_scope != SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME) return limit;
    /* Every frame of a challenge page takes part in the challenge, and the
       platform's own frame is one wherever it is embedded (Turnstile). */
    return strcmp(session->last_cf_mitigated, "challenge") == 0
            || navigation_url_is_challenge_platform(url)
        ? NAVIGATION_MANAGED_CHALLENGE_JS_HEAP_BYTES : limit;
}

static ScriptRuntime *navigation_create_runtime(
    NavigationSession *session, PocDocument *document, const char *url,
    const char *document_referrer, uint64_t navigation_started_us,
    const char *referrer_policy,
    bool defer_document_scripts,
    const ViewportContext *viewport, ScriptDocumentScope document_scope,
    FetchSchedulerDomain *fetch_domain,
    const Stylesheet *stylesheet, ImageResources *canvas_images,
    const FontSet *fonts,
    ScriptResult *result)
{
    if (session == NULL) return NULL;
    size_t js_memory_limit =
        navigation_script_heap_limit(session, document_scope, url);
    if (js_memory_limit != session->js_memory_limit)
        session->managed_challenge_heap_raises++;
    if (document_scope == SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
        && session->next_top_level_js_heap != 0) {
        /* An installed app's realm. The floor is consumed here so a later
           document in this tab (or a failed launch's successor) is back on
           the ordinary heap. */
        if (js_memory_limit != 0
            && session->next_top_level_js_heap > js_memory_limit) {
            js_memory_limit = session->next_top_level_js_heap;
            if (NAVIGATION_TEST_COUNTERS)
                session->installed_app_heap_raises++;
        }
        session->next_top_level_js_heap = 0;
    }
    ScriptRuntimeOptions options = {
        .session = session->browser_session,
        .fonts = fonts,
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
        .game_audio_internal_slots = session->game_audio_internal_slots,
        .dynamic_code_disabled = !tilefinch_csp_allows_dynamic_code(
            &document->content_security_policy),
        .document_scope = document_scope,
        .document_referrer = document_referrer,
        .navigation_started_us = navigation_started_us,
        .referrer_policy = referrer_policy
    };
    ScriptRuntime *runtime = script_runtime_create_configured(
        document, session->budget, js_memory_limit,
        session->js_timeout_ms, url, &options, result);
    if (runtime != NULL && canvas_images != NULL) {
        script_runtime_set_images(runtime, canvas_images);
    }
    if (runtime != NULL) {
        script_runtime_set_heavy_policy(
            runtime, session->heavy_policy_resolver != NULL
                         && options.top_level_url != NULL
                ? session->heavy_policy_resolver(
                      session->heavy_policy_opaque, options.top_level_url)
                : session->heavy_script_policy);
    }
    if (runtime != NULL && session->budget != NULL
        && !session->fixed_script_memory_limits) {
        /* The configured heap is a floor. A page that needs more (on the
           PSP chatgpt.com needs ~12 MB to start and more to send) grows
           while an eighth of the page Budget stays free for DOM, layout,
           paint and images (their transient peaks on chatgpt.com were ~2
           MB each), after optional caches give way, up to three quarters
           of the Budget. */
        script_runtime_enable_heap_growth(
            runtime, session->budget->limit / 4u * 3u,
            session->budget->limit / 8u);
    }
    return runtime;
}

static void navigation_prepare_layout_reuse(
    NavigationSession *session, const ScriptMutationJournal *mutations,
    bool full_rebuild, const bool *local_focus);
static bool navigation_focus_geometry_unchanged(
    NavigationSession *session, const ScriptMutationJournal *mutations);
static void navigation_evict_layout_reuse_under_pressure(
    NavigationSession *session);

static LayoutReuseCache *navigation_prepare_geometry_reuse(
    void *opaque, const ScriptMutationJournal *mutations,
    bool *geometry_unchanged)
{
    NavigationSession *session = opaque;
    if (geometry_unchanged != NULL) {
        *geometry_unchanged = navigation_focus_geometry_unchanged(session, mutations);
        if (*geometry_unchanged) return NULL;
    }
    /* The normal publication still consumes these mutations later. Apply
       its exact invalidation now as well, rather than using stale styles
       or resolving the entire document anew inside a geometry accessor. */
    /* A queued resource/style rebuild does not change this build's installed
       cascade. Its generation guards invalidate the cache when replacement
       rules actually arrive; the journal still invalidates changed DOM now. */
    navigation_prepare_layout_reuse(session, mutations, false, NULL);
    navigation_evict_layout_reuse_under_pressure(session);
    return session->page.layout_reuse;
}

static bool navigation_flush_candidate_geometry(void *opaque)
{
    NavigationSession *session = opaque;
    return session != NULL && session->page.runtime != NULL
        && script_runtime_flush_geometry_layout_reuse(session->page.runtime,
            &session->viewport, navigation_prepare_geometry_reuse, session);
}

static bool navigation_flush_committed_geometry(void *opaque)
{
    NavigationSession *session = opaque;
    if (session == NULL || !session->page.loaded) return false;
    bool bounded = session->page.layout.content_limit_y != 0
        && session->page.bounded_focus_region_end != 0;
    /* A refused complete build is not retried read by read: script answers
       from the proven region meanwhile (js_element_geometry), and the next
       publication starts over. */
    if (bounded && session->page.bounded_focus_geometry_refused)
        return false;
    /* Script reading geometry over a bounded focus layout completes it
       here (js_rt_bridge_layout). A page that does so after every move
       gains nothing from bounding them: stop after the second layout so
       completed. Each layout counts once, however many reads (later ones
       rebuild only for the page's own DOM changes, which an unbounded
       layout would pay as well). */
    if (bounded && !session->page.bounded_focus_completion_counted) {
        session->page.bounded_focus_completion_counted = true;
        session->performance.bounded_focus_script_completions++;
        if (++session->page.bounded_focus_script_completions >= 2u)
            session->page.bounded_focus_refused = true;
    }
    bool flushed;
#if !defined(__PSP__)
    if (bounded && tilefinch_test_faults()->refuse_bounded_geometry_builds) {
        tilefinch_test_faults()->bounded_geometry_build_attempts++;
        flushed = false;
    } else
#endif
    flushed = navigation_flush_candidate_geometry(session);
    if (!flushed && bounded) {
        /* No room for the complete layout: the page stops bounding, so the
           next focus move takes the complete build (or its own refusal). */
        session->page.bounded_focus_geometry_refused = true;
        session->page.bounded_focus_refused = true;
    }
    return flushed;
}

static void navigation_update_font_usage(NavigationSession *session)
{
    if (session != NULL && session->web_fonts_enabled) {
        uint64_t required = session->page.stylesheet.metric_web_font_faces;
        uint64_t preferred = session->page.stylesheet.metric_web_font_primary_faces;
        const WebFontSet *fonts = stylesheet_web_font_set(&session->page.stylesheet);
        for (size_t at = 0; at < session->page.layout.count; at++) {
            const DrawCommand *command = &session->page.layout.commands[at];
            if (command->type != DRAW_TEXT) continue;
            unsigned slot = font_family_web_slot(draw_command_font_family(command));
            if (fonts == NULL || slot >= TILEFINCH_WEB_FONT_FAMILY_LIMIT) continue;
            const WebFontFamilyFaces *family = &fonts->families[slot];
            /* Keep one normal fallback admissible if the requested face's
               own sources fail. Later layouts reconsider newly used weights. */
            required |= (uint64_t)font_family_face_usage_mask(
                family, draw_command_font_weight(command))
                << (slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT);
            size_t primary = font_family_face_slot_at_weight(family,
                draw_command_font_weight(command), false);
            if (primary != SIZE_MAX) preferred |= UINT64_C(1)
                << (slot * TILEFINCH_WEB_FONT_WEIGHT_LIMIT + primary);
        }
        fonts_external_loader_prioritize_faces(&session->page.external_font_loader,
            &session->page.stylesheet, required, preferred);
    }
}

static void navigation_attach_layout(NavigationSession *session)
{
    navigation_update_font_usage(session);
    if (session != NULL) {
        session->page.visual_image_publication_pending = false;
        session->page.layout_provisional =
            session->page.layout.content_limit_y != 0;
        /* Each publication proves its own region (relayout.inc sets it
           after this, for a bounded focus layout only). */
        session->page.bounded_focus_region_end = 0;
        session->page.bounded_focus_geometry_refused = false;
        session->page.bounded_focus_completion_counted = false;
        if (!session->page.layout_provisional) {
            session->page.layout_complete_us =
                session->page.layout.performance.total_us;
            session->page.layout_complete_commands =
                session->page.layout.count;
            session->page.layout_complete_node_boxes =
                session->page.layout.node_box_count;
        }
    }
    if (session != NULL && session->page.deferred_image_targets != NULL) {
        /* Every adopted layout moves boxes, including the relayout that
           publishes a batch, so the viewport distances behind the rank are
           stale after each one. Re-ranking costs one pass over the pending
           targets plus a sort; it is not amortised to once per scroll. */
        session->page.deferred_image_rank_valid = false;
        session->page.image_continuation_pending = true;
    }
    if (session != NULL && session->page.runtime != NULL) {
        script_runtime_set_layout(session->page.runtime,
                                  &session->page.layout,
                                  session->viewport.css_height);
        /* Commit installs the default once. Subsequent ordinary relayouts
           must preserve an explicitly supplied synchronous-layout callback. */
        if (!session->page.loaded)
            script_runtime_set_geometry_layout_callback(session->page.runtime,
                navigation_flush_committed_geometry, session);
        script_runtime_set_images(session->page.runtime,
                                  &session->page.images);
        /* The observer pass a commit queues reads its targets' geometry. A
           truncated (bounded focus) layout would be completed by that read
           at once, paying the complete build the bounding deferred; the
           pass waits for the completion instead, whose publication queues
           it (idle work, settled input, or script's own read). */
        if (!session->page.layout_provisional)
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
    if (data == NULL) return hash;
    /* Four bytes per 64-bit multiply (an FNV-style step on a word): the
       stylesheet fingerprint covers every inline <style> text and runs at
       each parser-blocking script and load preview, megabytes per load on
       a front page. Fingerprints are only compared within the process. */
    size_t i = 0;
    for (; length - i >= 4u; i += 4u) {
        uint32_t word;
        memcpy(&word, data + i, sizeof(word));
        hash = (hash ^ word) * UINT64_C(1099511628211);
    }
    for (; i < length; i++) {
        hash = (hash ^ (unsigned char) data[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

/* Attributes of resource elements the fingerprint covers. */
static const char *const resource_fingerprint_attributes[] = {
    "rel", "href", "xlink:href", "src", "srcset",
    "data-src", "data-srcset", "media", "type", "sizes",
    "poster"
};

/* `parts`, when present, also hashes the stylesheet sources (style, link,
   base and style text) into parts[0] and the image-bearing elements (img,
   video, source, use) into parts[1], so a changed fingerprint can say which
   kind of resource moved. parts[2] additionally identifies exact stylesheet
   sources and revisions, excluding which roots adopt those same sources. */
static bool resource_fingerprint_node(lxb_dom_node_t *node,
                                      uint64_t *hash, uint64_t *parts)
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
            uint64_t *part = parts == NULL ? NULL
                : &parts[image || source || use ? 1 : 0];
            *hash = fingerprint_bytes(*hash, name, name_length);
            if (part != NULL)
                *part = fingerprint_bytes(*part, name, name_length);
            if (style || link || base) {
                if (parts != NULL) {
                    parts[2] = fingerprint_bytes(parts[2],
                        (const char *) &node, sizeof(node));
                    parts[2] = fingerprint_bytes(parts[2], name, name_length);
                }
                static const char *const policy[] = {
                    "nonce", "integrity", "crossorigin", "referrerpolicy",
                    "disabled", "title"
                };
                for (size_t i = 0; i < sizeof(policy) / sizeof(policy[0]); i++) {
                    size_t length = 0;
                    const char *value = document_attribute(node, policy[i], &length);
                    /* Distinguish an absent boolean attribute from empty. */
                    bool present = value != NULL;
                    *hash = fingerprint_bytes(*hash, (const char *) &present, sizeof(present));
                    *hash = fingerprint_bytes(*hash, (const char *) &length, sizeof(length));
                    *hash = fingerprint_bytes(*hash, value, length);
                    if (parts != NULL) {
                        parts[2] = fingerprint_bytes(parts[2],
                            (const char *) &present, sizeof(present));
                        parts[2] = fingerprint_bytes(parts[2],
                            (const char *) &length, sizeof(length));
                        parts[2] = fingerprint_bytes(parts[2], value, length);
                        parts[0] = fingerprint_bytes(parts[0], (const char *) &present, sizeof(present));
                        parts[0] = fingerprint_bytes(parts[0], (const char *) &length, sizeof(length));
                        parts[0] = fingerprint_bytes(parts[0], value, length);
                    }
                }
            }
            for (size_t i = 0;
                 i < sizeof(resource_fingerprint_attributes)
                         / sizeof(resource_fingerprint_attributes[0]);
                 i++) {
                size_t length = 0;
                const char *value = document_attribute(
                    node, resource_fingerprint_attributes[i], &length);
                *hash = fingerprint_bytes(*hash, value, length);
                if (part != NULL)
                    *part = fingerprint_bytes(*part, value, length);
                if (parts != NULL && (style || link || base)) {
                    bool present = value != NULL;
                    parts[2] = fingerprint_bytes(parts[2],
                        (const char *) &present, sizeof(present));
                    parts[2] = fingerprint_bytes(parts[2],
                        (const char *) &length, sizeof(length));
                    parts[2] = fingerprint_bytes(parts[2], value, length);
                }
            }
        }
        if (style) style_depth++;
        if (style) {
            size_t length = 0;
            const char *override = document_cssom_sheet_text(node, &length);
            if (override != NULL) {
                static const char marker[] = "\xff" "cssom\0";
                *hash = fingerprint_bytes(*hash, marker, sizeof(marker));
                *hash = fingerprint_bytes(*hash, override, length);
                if (parts != NULL) {
                    parts[0] = fingerprint_bytes(parts[0], marker, sizeof(marker));
                    parts[0] = fingerprint_bytes(parts[0], override, length);
                    parts[2] = fingerprint_bytes(parts[2], marker, sizeof(marker));
                    parts[2] = fingerprint_bytes(parts[2], override, length);
                }
            }
        }
        if (node->type == LXB_DOM_NODE_TYPE_TEXT && style_depth != 0) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            *hash = fingerprint_bytes(*hash, text, length);
            if (parts != NULL)
                parts[0] = fingerprint_bytes(parts[0], text, length);
            if (parts != NULL)
                parts[2] = fingerprint_bytes(parts[2], text, length);
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

static bool page_resource_fingerprint_split(const PocDocument *document,
                                            uint64_t *hash, uint64_t *parts)
{
    if (hash == NULL) return false;
    *hash = 0;
    if (parts != NULL) parts[0] = parts[1] = parts[2] = 0;
    if (document == NULL || document->html == NULL) return true;
    /* Adopted constructed sheets are stylesheet sources outside the tree. */
    uint64_t adopted = document_adopted_sheets_signature(document);
    adopted ^= (uint64_t) document->registered_property_count << 32;
    *hash = UINT64_C(1469598103934665603) ^ adopted;
    if (parts != NULL) {
        parts[0] = UINT64_C(1469598103934665603) ^ adopted;
        parts[1] = UINT64_C(1469598103934665603);
        lxb_dom_node_t *sources[DOCUMENT_ADOPTED_TIER_LIMIT];
        uint32_t revisions[DOCUMENT_ADOPTED_TIER_LIMIT];
        size_t count = document_adopted_sheets_active(
            document, sources, revisions, DOCUMENT_ADOPTED_TIER_LIMIT);
        if (count == SIZE_MAX) return false;
        parts[2] = UINT64_C(1469598103934665603);
        for (size_t i = 0; i < count; i++)
            parts[2] = document_adopted_tier_hash(parts[2], sources[i], revisions[i]);
    }
    return resource_fingerprint_node(
        lxb_dom_interface_node(document->html), hash, parts);
}

static bool page_resource_fingerprint(const PocDocument *document,
                                      uint64_t *hash)
{
    return page_resource_fingerprint_split(document, hash, NULL);
}

/* Pairs the committed fingerprint with its parts when the last traversal
   produced it; any other commit leaves the parts unknown. */
static void navigation_note_committed_fingerprint(NavigationSession *session)
{
    if (session->resource_fingerprint_last == session->resource_fingerprint) {
        session->resource_fingerprint_parts[0] =
            session->resource_fingerprint_parts_last[0];
        session->resource_fingerprint_parts[1] =
            session->resource_fingerprint_parts_last[1];
        session->resource_fingerprint_parts[2] =
            session->resource_fingerprint_parts_last[2];
        session->resource_fingerprint_parts_owner =
            session->resource_fingerprint;
        session->resource_fingerprint_parts_valid = true;
    } else if (session->resource_fingerprint_parts_owner
               != session->resource_fingerprint) {
        session->resource_fingerprint_parts_valid = false;
    }
}

/* Whether the last traversal differs from the committed fingerprint in
   `part` (0 stylesheet sources/scopes, 1 image elements, 2 exact stylesheet
   source identity/content excluding adopting roots). Unknown counts as
   changed. */
static bool navigation_fingerprint_part_changed(
    const NavigationSession *session, unsigned part)
{
    return !session->resource_fingerprint_parts_valid
        || session->resource_fingerprint_parts_owner
               != session->resource_fingerprint
        || session->resource_fingerprint_parts[part]
               != session->resource_fingerprint_parts_last[part];
}

static bool navigation_resource_fingerprint(
    NavigationSession *session, const PocDocument *document, uint64_t *hash)
{
    if (session != NULL) session->performance.resource_fingerprint_scans++;
    uint64_t parts[3] = {0};
    if (page_resource_fingerprint_split(document, hash, parts)) {
        if (session != NULL) {
            session->resource_fingerprint_last = *hash;
            session->resource_fingerprint_parts_last[0] = parts[0];
            session->resource_fingerprint_parts_last[1] = parts[1];
            session->resource_fingerprint_parts_last[2] = parts[2];
        }
        return true;
    }
    if (session != NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "resource fingerprint traversal exceeded %u nodes",
                 NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT);
    }
    return false;
}

bool navigation_accept_native_resource_fingerprint(
    NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    uint64_t fingerprint = 0;
    if (!navigation_resource_fingerprint(
            session, &session->page.document, &fingerprint)) return false;
    session->resource_fingerprint = fingerprint;
    navigation_note_committed_fingerprint(session);
    return true;
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
    /* Ink outside the box: a blurred text shadow and an explicit
       underline (the same extents the renderer's cull uses). Retained
       canvas overlays repaint exactly this rectangle. */
    int ink_x = draw_command_text_shadow_blur(command), ink_y = ink_x;
    if (command->type == DRAW_TEXT
        && draw_command_text_has_underline(command)
        && draw_command_text_underline_offset(command, NULL)) {
        int extent = command->font_size < 0 ? 0 : command->font_size;
        if (extent > TILEFINCH_FONT_RASTER_PIXEL_LIMIT)
            extent = TILEFINCH_FONT_RASTER_PIXEL_LIMIT;
        ink_y += extent + 17;
    }
    int64_t x0 = (int64_t) command->x - ink_x;
    int64_t y0 = (int64_t) command->y - ink_y;
    int64_t x1 = (int64_t) command->x + command->width + ink_x;
    int64_t y1 = (int64_t) command->y + command->height + ink_y;
    int l = x0 < INT_MIN ? INT_MIN : (int) x0;
    int t = y0 < INT_MIN ? INT_MIN : (int) y0;
    int r = x1 > INT_MAX ? INT_MAX : (int) x1;
    int b = y1 > INT_MAX ? INT_MAX : (int) y1;
    if (*right <= *left) {
        *left = l; *top = t; *right = r; *bottom = b;
        return;
    }
    if (l < *left) *left = l;
    if (t < *top) *top = t;
    if (r > *right) *right = r;
    if (b > *bottom) *bottom = b;
}

static void damage_add_rect(int *left, int *top, int *right, int *bottom,
                            int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) return;
    DrawCommand command = {.x = x, .y = y, .width = width,
                           .height = height};
    damage_add(left, top, right, bottom, &command);
}

/* A solid box that differs only in height: a wrapper that now holds the
   rest of the page. Above its old and new bottom edge its corners, border
   and shadow are unchanged (a gradient stretches, and a filter, rotation or
   backdrop reaches further, so those repaint whole). */
static bool draw_command_height_only_change(const DrawCommand *old,
                                            const DrawCommand *next)
{
    if (old->height == next->height || old->image != NULL
        || (old->type != DRAW_FILL_RECT && old->type != DRAW_SHADOW_RECT
            && (old->type != DRAW_STROKE_RECT
                || old->image_fit != LAYOUT_STROKE_SOLID))
        || draw_command_fill_gradient(old, NULL)
        || draw_command_filter_code(old) != 0
        || draw_command_rotation_quadrants(old) != 0
        || draw_command_backdrop_blur(old) != 0) return false;
    DrawCommand same_height = *next;
    same_height.height = old->height;
    return draw_command_equal(old, &same_height);
}

static void damage_add_bottom_edges(int *left, int *top, int *right,
                                    int *bottom, const DrawCommand *old,
                                    const DrawCommand *next)
{
    int old_bottom = old->y + old->height;
    int next_bottom = next->y + next->height;
    int shorter = old_bottom < next_bottom ? old_bottom : next_bottom;
    int taller = old_bottom > next_bottom ? old_bottom : next_bottom;
    int margin = style_border_radius_maximum(old->radius)
        + 2 * (old->scale > 0 ? old->scale : 0) + 1;
    int from = shorter - margin > old->y ? shorter - margin : old->y;
    damage_add_rect(left, top, right, bottom, old->x, from, old->width,
                    taller - from);
}

/* Equal commands between two changes need not be repainted when their
   retained paint context is identical. Compare sidecars too: unchanged
   command bytes alone do not prove unchanged clipping, order or gradients.
   Padding differences merely decline this optimization. No DOM/image/text
   storage is dereferenced here; old commands may borrow retired storage. */
typedef struct {
    uint32_t clip_boxes[8];
    size_t clip_count;
} NavigationDamageContext;

static bool navigation_damage_context_equal(const LayoutDocument *old,
    const LayoutDocument *next, NavigationDamageContext *context)
{
    context->clip_count = 0;
    if (old->width != next->width || old->height != next->height
        || memcmp(&old->viewport, &next->viewport, sizeof(old->viewport)) != 0
        || old->fonts != next->fonts || old->web_fonts != next->web_fonts
        || old->scroll_generation != next->scroll_generation
        || old->node_box_count != next->node_box_count
        || old->bidi_command_count != 0 || next->bidi_command_count != 0
        || old->sticky_count != 0 || next->sticky_count != 0
        || old->fixed_count != 0 || next->fixed_count != 0
        || old->overflow_order_count != 0 || next->overflow_order_count != 0
        || old->late_positioned_order_count != 0
        || next->late_positioned_order_count != 0)
        return false;
#define SAME_SIDECAR(field, count_field) \
    if (old->count_field != next->count_field \
        || (old->count_field != 0 \
            && memcmp(old->field, next->field, \
                      old->count_field * sizeof(*old->field)) != 0)) \
        return false
    SAME_SIDECAR(gradients, gradient_count);
    SAME_SIDECAR(bidi_glyphs, bidi_glyph_count);
    SAME_SIDECAR(bidi_commands, bidi_command_count);
    SAME_SIDECAR(sticky_ranges, sticky_count);
    SAME_SIDECAR(fixed_ranges, fixed_count);
    SAME_SIDECAR(scroll_containers, scroll_container_count);
    SAME_SIDECAR(overflow_orders, overflow_order_count);
    SAME_SIDECAR(late_positioned_orders, late_positioned_order_count);
#undef SAME_SIDECAR
    for (size_t i = 0; i < old->node_box_count; i++) {
        LayoutNodeBox before = old->node_boxes[i], after = next->node_boxes[i];
        /* Range indices move when an outline is inserted. Geometry must
           agree, and each matched command must retain its clip membership. */
        if (before.clips_x || before.clips_y || before.clip_path_clip) {
            if (context->clip_count == 8) return false;
            context->clip_boxes[context->clip_count++] = (uint32_t) i;
        } else {
            /* Outline ink can grow a non-clipping wrapper's content extent
               without changing its box or any command. Page height is
               compared above; this metadata does not clip paint. */
            before.content_width = before.content_height = 0;
            after.content_width = after.content_height = 0;
        }
        before.command_start = before.command_end = 0;
        before.scroll_command_start = before.scroll_command_end = 0;
        after.command_start = after.command_end = 0;
        after.scroll_command_start = after.scroll_command_end = 0;
        if (memcmp(&before, &after, sizeof(before)) != 0) return false;
    }
    if ((old->node_clip_radius_codes == NULL)
            != (next->node_clip_radius_codes == NULL)
        || (old->node_clip_radius_codes != NULL
            && memcmp(old->node_clip_radius_codes, next->node_clip_radius_codes,
                      old->node_box_count * sizeof(*old->node_clip_radius_codes)) != 0))
        return false;
    const LayoutDocument *layouts[] = {old, next};
    for (size_t lane = 0; lane < 2; lane++) {
        const LayoutDocument *layout = layouts[lane];
        if (layout->paint_order_count != layout->count) return false;
        for (size_t i = 0; i < layout->count; i++) {
            const DrawCommand *command = &layout->commands[i];
            if ((layout->command_flags != NULL
                    && (layout->command_flags[i] & (LAYOUT_COMMAND_DYNAMIC_OVERFLOW
                        | LAYOUT_COMMAND_FIXED | LAYOUT_COMMAND_STICKY
                        | LAYOUT_COMMAND_LATE_POSITIONED)) != 0)
                || command->type == DRAW_SHADOW_RECT
                || draw_command_filter_code(command) != 0
                || draw_command_backdrop_blur(command) != 0
                || draw_command_rotation_quadrants(command) != 0)
                return false;
        }
    }
    return true;
}

static bool navigation_damage_commands_equal(const LayoutDocument *old,
    size_t before_order, const LayoutDocument *next, size_t after_order,
    const NavigationDamageContext *context)
{
    size_t i = old->paint_order[before_order], j = next->paint_order[after_order];
    if (!draw_command_equal(&old->commands[i], &next->commands[j])) return false;
    uint8_t before_flags = old->command_flags == NULL ? 0 : old->command_flags[i];
    uint8_t after_flags = next->command_flags == NULL ? 0 : next->command_flags[j];
    if (before_flags != after_flags) return false;
    for (size_t k = 0; k < context->clip_count; k++) {
        const LayoutNodeBox *before = &old->node_boxes[context->clip_boxes[k]];
        const LayoutNodeBox *after = &next->node_boxes[context->clip_boxes[k]];
        if ((i >= before->scroll_command_start && i < before->scroll_command_end)
              != (j >= after->scroll_command_start && j < after->scroll_command_end)
            || (i >= before->command_start && i < before->command_end)
              != (j >= after->command_start && j < after->command_end))
            return false;
    }
    return true;
}

/* Retain a fixed raster only with exact sidecar/command equality, not a
   fingerprint or a disjoint damage guess (fixed commands retain in-flow
   coordinates). Non-focus changes also require the renderer's immutable
   image-lifetime witness; the retired image table is never dereferenced here.
   Ambiguous contexts fall back to replacement. This proof allocates nothing. */
static bool navigation_fixed_focus_context_equal(
    const LayoutDocument *old, const LayoutDocument *next,
    bool resource_bodies_unchanged)
{
    if (old->fixed_count == 0 || old->fixed_count != next->fixed_count
        || old->fonts != next->fonts || old->web_fonts != next->web_fonts
        || old->width != next->width
        || memcmp(&old->viewport, &next->viewport, sizeof(old->viewport)) != 0
        || old->scroll_generation != next->scroll_generation
        || old->node_box_count != next->node_box_count
        || old->sticky_count != 0 || next->sticky_count != 0
        || old->bidi_command_count != 0 || next->bidi_command_count != 0
        || old->gradient_count != next->gradient_count
        || memcmp(old->gradients, next->gradients,
                  old->gradient_count * sizeof(*old->gradients)) != 0
        || memcmp(old->fixed_ranges, next->fixed_ranges,
                  old->fixed_count * sizeof(*old->fixed_ranges)) != 0
        || old->scroll_container_count != next->scroll_container_count
        || (old->scroll_container_count != 0
            && memcmp(old->scroll_containers, next->scroll_containers,
                old->scroll_container_count * sizeof(*old->scroll_containers)) != 0))
        return false;
    for (size_t i = 0; i < old->node_box_count; i++) {
        const LayoutNodeBox *a = &old->node_boxes[i], *b = &next->node_boxes[i];
        if ((a->clips_x || a->clips_y || a->clip_path_clip
             || b->clips_x || b->clips_y || b->clip_path_clip)
            && memcmp(a, b, sizeof(*a)) != 0) return false;
    }
    if ((old->node_clip_radius_codes == NULL) != (next->node_clip_radius_codes == NULL)
        || (old->node_clip_radius_codes != NULL
            && memcmp(old->node_clip_radius_codes, next->node_clip_radius_codes,
                old->node_box_count * sizeof(*old->node_clip_radius_codes)) != 0))
        return false;
    for (size_t r = 0; r < old->fixed_count; r++) {
        const FixedRange *range = &old->fixed_ranges[r];
        if (range->command_end > old->count || range->command_end > next->count)
            return false;
        for (size_t i = range->command_start; i < range->command_end; i++) {
            DrawCommand before = old->commands[i], after = next->commands[i];
            /* The old table may already have retired. Compare only command
               geometry here; the renderer verifies the new image against
               exact immutable-body lifetimes saved at raster construction. */
            if (!resource_bodies_unchanged && before.type == DRAW_IMAGE
                && after.type == DRAW_IMAGE) before.image = after.image = NULL;
            if (!draw_command_equal(&before, &after))
                return false;
            uint8_t a = old->command_flags == NULL ? 0 : old->command_flags[i];
            uint8_t b = next->command_flags == NULL ? 0 : next->command_flags[i];
            if (a != b) return false;
        }
    }
    /* A flow outline may be inserted after the header. Compare fixed paint
       order independently; the renderer still checks whether the new whole
       page can compose a fixed overlay before using its retained raster. */
    if (old->paint_order_count != old->count
        || next->paint_order_count != next->count
        || old->command_flags == NULL || next->command_flags == NULL)
        return false;
    size_t a = 0, b = 0;
    while (a < old->count || b < next->count) {
        while (a < old->count
            && (old->command_flags[old->paint_order[a]] & LAYOUT_COMMAND_FIXED) == 0) a++;
        while (b < next->count
            && (next->command_flags[next->paint_order[b]] & LAYOUT_COMMAND_FIXED) == 0) b++;
        if (a == old->count || b == next->count)
            return a == old->count && b == next->count;
        if (old->paint_order[a++] != next->paint_order[b++]) return false;
    }
    return true;
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
        /* Growing a provisional layout keeps every command above its old
           limit at the same index; only boxes that span the limit grow,
           and everything below it is damaged by the limit rule. Compare
           index by index there, so the page background's new height does
           not repaint the screens the reader is on. */
        bool extended = old_layout->content_limit_y != 0;
        NavigationDamageContext context;
        bool aligned_paint = navigation_damage_context_equal(old_layout,
                                                              new_layout, &context);
        size_t aligned = old_end < new_end ? old_end : new_end;
        if (aligned_paint) {
            /* Compare actual paint order, which need not equal DOM order. */
            size_t i = 0, j = 0;
            old_end = old_layout->count;
            new_end = new_layout->count;
            while (i < old_end || j < new_end) {
                if (i < old_end && j < new_end
                    && navigation_damage_commands_equal(old_layout, i,
                                                         new_layout, j, &context)) {
                    i++; j++;
                    continue;
                }
                /* Focus decorations are small insertions/removals. Bounded
                   lookahead also handles an adjacent changed paint command;
                   never search an unbounded list or allocate a diff table. */
                size_t old_skip = 0, new_skip = 0;
                for (size_t distance = 1; distance <= 8; distance++) {
                    if (i < old_end && distance < new_end - j
                        && navigation_damage_commands_equal(old_layout, i,
                            new_layout, j + distance, &context)) {
                        new_skip = distance; break;
                    }
                    if (j < new_end && distance < old_end - i
                        && navigation_damage_commands_equal(old_layout, i + distance,
                            new_layout, j, &context)) {
                        old_skip = distance; break;
                    }
                }
                if (old_skip == 0 && new_skip == 0) {
                    old_skip = i < old_end ? 1 : 0;
                    new_skip = j < new_end ? 1 : 0;
                }
                while (old_skip > 0) {
                    old_skip--;
                    damage_add(&left, &top, &right, &bottom,
                               &old_layout->commands[old_layout->paint_order[i++]]);
                }
                while (new_skip > 0) {
                    new_skip--;
                    damage_add(&left, &top, &right, &bottom,
                               &new_layout->commands[new_layout->paint_order[j++]]);
                }
            }
        } else {
        for (size_t i = prefix; i < old_end; i++) {
            if (extended && i < aligned) {
                const DrawCommand *old = &old_layout->commands[i];
                const DrawCommand *next = &new_layout->commands[i];
                if (draw_command_equal(old, next)) continue;
                if (draw_command_height_only_change(old, next)) {
                    damage_add_bottom_edges(&left, &top, &right, &bottom,
                                            old, next);
                    continue;
                }
            }
            damage_add(&left, &top, &right, &bottom,
                       &old_layout->commands[i]);
        }
        for (size_t i = prefix; i < new_end; i++) {
            if (extended && i < aligned
                && (draw_command_equal(&old_layout->commands[i],
                                       &new_layout->commands[i])
                    || draw_command_height_only_change(
                           &old_layout->commands[i],
                           &new_layout->commands[i]))) continue;
            damage_add(&left, &top, &right, &bottom,
                       &new_layout->commands[i]);
        }
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
        /* Rows past a provisional limit showed a placeholder rather than
           commands, so a changed limit repaints them all. */
        if (old_layout->content_limit_y != new_layout->content_limit_y) {
            int from = old_layout->content_limit_y;
            if (from == 0 || (new_layout->content_limit_y != 0
                              && new_layout->content_limit_y < from))
                from = new_layout->content_limit_y;
            int taller = old_layout->height > new_layout->height
                         ? old_layout->height : new_layout->height;
            int width = old_layout->width > new_layout->width
                        ? old_layout->width : new_layout->width;
            if (taller > from)
                damage_add_rect(&left, &top, &right, &bottom, 0, from,
                                width, taller - from);
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

static void navigation_clear_frame_messages(
    NavigationSession *session, NavigationPage *page, bool record_drops)
{
    if (session == NULL || page == NULL) return;
    size_t dropped = page->message_count;
    for (size_t i = 0; i < dropped; i++) {
        budget_free(session->budget, page->messages[i].json);
        budget_free(session->budget, page->messages[i].target_origin);
    }
    memset(page->messages, 0, sizeof(page->messages));
    page->message_count = 0;
    if (record_drops) {
        session->frame_messages_dropped =
            dropped <= SIZE_MAX - session->frame_messages_dropped
            ? session->frame_messages_dropped + dropped : SIZE_MAX;
    }
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
    navigation_clear_frame_messages(session, page, false);
    for (size_t i = 0; i < page->frame_count; i++) {
        navigation_frame_presentation_destroy(
            session, &page->frames[i]);
        script_runtime_destroy(page->frames[i].runtime);
        images_destroy(&page->frames[i].images);
        document_destroy(&page->frames[i].document);
    }
    fonts_external_loader_cancel(
        &page->external_font_loader, page->resource_scheduler,
        &page->external_fonts);
    images_priority_load_destroy(page->deferred_image_job);
    navigation_destroy_linked_video_preview(session, page);
    page->deferred_image_job = NULL;
    page->deferred_image_batch_count = 0;
    budget_free(session->budget, page->deferred_image_targets);
    page->deferred_image_targets = NULL;
    page->deferred_image_count = 0;
    page->deferred_image_cursor = 0;
    page->deferred_image_rank_scroll_y = 0;
    budget_free(session->budget, page->visual_batch_targets);
    page->visual_batch_targets = NULL;
    page->visual_batch_count = 0;
    page->deferred_image_rank_valid = false;
    layout_destroy(&page->layout);
    layout_reuse_cache_destroy(page->layout_reuse);
    page->layout_reuse = NULL;
    /* A queued canvas/WebGL microtask belongs to the dying realm. Detach its
       publication target before releasing page images so runtime teardown
       cannot repopulate a resource table whose owner is already gone. */
    script_runtime_set_images(page->runtime, NULL);
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
    if (session != NULL) {
        session->committed_scroll_reflection_pending = false;
        page_destroy_value(session, &session->page);
    }
}

static bool navigation_retire_failed_document_refresh(
    NavigationSession *session, ScriptRuntime *runtime)
{
    if (session == NULL || runtime == NULL
        || !script_runtime_document_refresh_failed(runtime)) return false;
    /* Every caller supplies either the selected top realm or a runtime read
       from this page's bounded frame table; no pointer-derived identity is
       accepted from author code. */
    /* The mutation is already committed. The incumbent layout may borrow
       storage from nodes it destroyed, so no static-content degradation is
       safe: retire the complete page before returning to input or paint. */
    page_destroy(session);
    snprintf(session->last_error, sizeof(session->last_error),
             "could not refresh mutated DOM");
    return true;
}

static bool navigation_dispatch_frame_parent_event(
    NavigationSession *session, ScriptRuntime *parent_runtime,
    size_t parent_frame_slot_plus_one, lxb_dom_node_t *node,
    const char *event_type)
{
    if (session == NULL || parent_runtime == NULL || node == NULL
        || event_type == NULL) return false;
    ScriptResult *result = &session->page.script_result;
    NavigationFrame *parent = NULL;
    if (parent_frame_slot_plus_one != 0) {
        size_t slot = parent_frame_slot_plus_one - 1u;
        if (slot >= session->page.frame_count) return false;
        parent = &session->page.frames[slot];
        if (parent->retired || parent->runtime != parent_runtime) return false;
        result = &parent->script_result;
    } else if (parent_runtime != session->page.runtime) {
        return false;
    }
    bool dispatched = script_runtime_dispatch_node(
        parent_runtime, node, event_type, result);
    if (!dispatched && navigation_retire_failed_document_refresh(
            session, parent_runtime)) return false;
    if (!dispatched) return true;
    if (!script_runtime_consume_relayout(parent_runtime)) return true;
    return parent == NULL ? navigation_relayout(session)
                          : navigation_refresh_frame_presentation_or_defer(
                                session, parent);
}

static void navigation_freeze_reader_root_provenance(NavigationPage *page)
{
    if (page == NULL || page->runtime == NULL
        || page->reader_root_handle == 0) return;
    lxb_dom_node_t *resolved = script_runtime_node_handle_resolve(
        page->runtime, page->reader_root_handle);
    if (resolved != NULL && resolved == page->reader_root) {
        /* No author turn can run after the caller destroys this realm. The
           exact document-owned node—connected or still-private—is therefore
           a safe native pointer until activation or ordinary page teardown. */
        page->reader_root = resolved;
        page->reader_root_handle = 0;
        return;
    }
    /* Author code already detached or replaced the native clone. Never let a
       public look-alike marker inherit its provenance after realm teardown. */
    page->reader_root = NULL;
    page->reader_root_handle = 0;
    page->reader_root_kind = READER_PAGE_RAW;
}

void navigation_retire_current_page_scripts(NavigationSession *session)
{
    if (session == NULL) return;
    if (session->page.runtime != NULL) {
        navigation_freeze_reader_root_provenance(&session->page);
        script_runtime_set_synchronous_layout_callback(
            session->page.runtime, NULL, NULL);
        script_runtime_set_layout(session->page.runtime, NULL, 0);
    }
    /* postMessage payloads retain raw realm identities. Once every realm is
       retired they can neither be delivered nor safely remain queued. */
    navigation_clear_frame_messages(session, &session->page, true);
    if (session->page.runtime != NULL)
        script_runtime_destroy(session->page.runtime);
    session->page.runtime = NULL;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        script_runtime_destroy(session->page.frames[i].runtime);
        session->page.frames[i].runtime = NULL;
    }
}

void navigation_retire_large_incumbent_realm(NavigationSession *session)
{
    /* A large application realm and its successor cannot coexist under the
       page ceiling. Keep the existing 4 MiB admission policy identical for
       frontend-owned and runtime-owned navigation; both retain static DOM
       and pixels for rollback if the replacement fails. */
    if (session != NULL && session->page.runtime != NULL
        && script_runtime_heap_used(session->page.runtime) >= 4u * 1024u * 1024u)
        navigation_retire_current_page_scripts(session);
}

static bool navigation_retire_exhausted_script_realms_mode(
    NavigationSession *session, bool allocator_rejection_observed,
    bool static_fallback)
{
    if (session == NULL || session->page.runtime == NULL) {
        return false;
    }
    /* QuickJS reports a refused allocation as "out of memory", which a page
       can also throw. Only believe the text when the realm's heap actually
       refused an allocation since the last check. */
    size_t heap_rejections =
        script_runtime_heap_rejections(session->page.runtime);
    bool new_rejection =
        heap_rejections > session->page.script_heap_rejections_seen;
    session->page.script_heap_rejections_seen = heap_rejections;
    bool explicit_oom = new_rejection
        && strstr(session->page.script_result.error, "out of memory") != NULL;
    /* With memory-pressure growth a small margin under the current limit is
       normal; exhaustion means the realm cannot obtain more. */
    size_t remaining = script_runtime_heap_available(
        session->page.runtime);
#ifndef TILEFINCH_NO_TRACE
    if (tilefinch_trace_script_failures()) {
        fprintf(stderr,
                "tilefinch-script-retirement: explicit-oom=%s "
                "heap-remaining=%zu error=\"%.160s\"\n",
                explicit_oom ? "yes" : "no", remaining,
                session->page.script_result.error);
    }
#endif
    /* A caught allocation failure leaves no exception text and can leave
       substantial nominal headroom when one oversized request was refused.
       Only the runtime pump may authorize that case, using the allocator's
       monotonic rejection counter sampled around the author task. */
    if (!allocator_rejection_observed
        && !explicit_oom && remaining >= 256u * 1024u) return false;
    /*
     * Author JavaScript exhausting its separately bounded realm must not
     * discard an otherwise usable document.  The DOM, completed stylesheet,
     * controls, and resource scheduler are independently owned by the page;
     * retire only the realms whose callbacks can no longer run reliably.
     * Keeping the ScriptResult makes the degraded state visible in device
     * diagnostics without attempting another allocation in the failed VM.
     */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
    {
        ScriptHeapGrowth growth;
        script_runtime_heap_growth(session->page.runtime, &growth);
        char message[320];
        snprintf(message, sizeof(message),
                 "tilefinch-script-memory-stop: limit=%zu floor=%zu "
                 "raises=%zu refusals=%zu refused=%zu reason=%u "
                 "budget-free=%zu remaining=%zu error=\"%.80s\"",
                 growth.limit, growth.floor, growth.raises, growth.refusals,
                 growth.refused_bytes, (unsigned) growth.refused_reason,
                 budget_remaining(session->budget), remaining,
                 session->page.script_result.error);
        tilefinch_platform_log_message(message);
    }
#endif
    navigation_retire_current_page_scripts(session);
    session->page.script_result.success = false;
    session->page.script_degradation_observed = true;
    session->page.script_memory_limited = true;
    snprintf(session->page.script_result.summary,
             sizeof(session->page.script_result.summary),
             "JavaScript stopped at its memory limit");
    if (static_fallback)
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
    return true;
}

static bool navigation_retire_exhausted_script_realms(
    NavigationSession *session, bool allocator_rejection_observed)
{
    return navigation_retire_exhausted_script_realms_mode(
        session, allocator_rejection_observed, true);
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

static bool navigation_is_style_preload(lxb_dom_node_t *node)
{
    if (!node_name_is(node, "link")) return false;
    size_t rel_length = 0, as_length = 0;
    const char *rel = document_attribute(node, "rel", &rel_length);
    const char *as = document_attribute(node, "as", &as_length);
    return rel != NULL && as != NULL && as_length == 5u
        && strncasecmp(as, "style", 5u) == 0
        && navigation_token_contains(rel, rel_length, "preload");
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
    lxb_dom_node_t *node, uint64_t resolved_hash, bool preload)
{
    for (size_t i = 0; i < count; i++) {
        if (records[i].element == node
            && records[i].resolved_url_hash == resolved_hash
            && records[i].preload == preload) return true;
    }
    return false;
}

static bool navigation_remember_stylesheet_event(
    NavigationStylesheetEventRecord *records, size_t *count,
    lxb_dom_node_t *node, uint64_t resolved_hash, bool preload)
{
    for (size_t i = 0; i < *count; i++) {
        if (records[i].element != node) continue;
        records[i].resolved_url_hash = resolved_hash;
        records[i].preload = preload;
        return true;
    }
    if (*count >= NAVIGATION_STYLESHEET_EVENT_LIMIT) return false;
    records[*count] = (NavigationStylesheetEventRecord) {
        .element = node, .resolved_url_hash = resolved_hash, .preload = preload
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
                records, count, node, hash, false);
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
        if (!navigation_is_stylesheet_link(node)
            && !navigation_is_style_preload(node)) continue;
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
static bool navigation_dispatch_stylesheet_events_timed(
    NavigationSession *session, const char *base_url);

/* Stylesheet load/error events are page JavaScript run inside the resource
   phase; time them (and the handlers alone) so the resource phase can be
   split into CSS work and author callbacks. */
static bool navigation_dispatch_stylesheet_events(
    NavigationSession *session, const char *base_url)
{
    uint64_t started = navigation_now_us();
    bool result = navigation_dispatch_stylesheet_events_timed(
        session, base_url);
    if (session != NULL) {
        uint64_t finished = navigation_now_us();
        session->performance.stylesheet_event_us +=
            finished >= started ? finished - started : 0;
    }
    return result;
}

static bool navigation_dispatch_stylesheet_events_timed(
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
        bool selected_preload = false;
        lxb_dom_node_t *root = lxb_dom_interface_node(
            session->page.document.html);
        for (lxb_dom_node_t *node = root; node != NULL;
             node = navigation_next_document_node(root, node)) {
            bool preload = !navigation_is_stylesheet_link(node);
            if (preload && !navigation_is_style_preload(node)) continue;
            char resolved[4096];
            uint64_t hash = 0;
            (void) navigation_resolve_stylesheet_link(
                node, base_url, resolved, &hash);
            if (hash == 0 || navigation_stylesheet_event_known(
                    session->page.stylesheet_events,
                    session->page.stylesheet_event_count, node, hash,
                    preload)) {
                continue;
            }
            StylesheetDocumentResourceState state =
                navigation_stylesheet_link_state(
                    session, node, base_url);
            if (preload) {
                const StylesheetDocumentResources *resources =
                    &session->page.stylesheet_resources;
                state = STYLESHEET_DOCUMENT_RESOURCE_EMPTY;
                for (size_t i = 0; i < resources->style_preload_count; i++)
                    if (resources->style_preloads[i].element == node) {
                        state = resources->style_preloads[i].state;
                        break;
                    }
            }
            if (state != STYLESHEET_DOCUMENT_RESOURCE_LOADED
                && state
                    != STYLESHEET_DOCUMENT_RESOURCE_TERMINAL_FAILURE) {
                continue;
            }
            selected = node;
            selected_hash = hash;
            selected_preload = preload;
            event_type = state == STYLESHEET_DOCUMENT_RESOURCE_LOADED
                ? "load" : "error";
            break;
        }
        if (selected == NULL) {
            session->page.stylesheet_events_swept = true;
            return true;
        }
        if (!navigation_remember_stylesheet_event(
                session->page.stylesheet_events,
                &session->page.stylesheet_event_count,
                selected, selected_hash, selected_preload)) return true;
        /* Dispatch exactly one callback before rescanning.  Author code and
           its promise jobs may replace the entire head, invalidating every
           other node pointer which existed at selection time. */
        uint64_t handler_started = navigation_now_us();
        bool dispatched_ok = script_runtime_dispatch_node(
            session->page.runtime, selected, event_type,
            &session->page.script_result);
        uint64_t handler_finished = navigation_now_us();
        session->performance.stylesheet_event_handler_us +=
            handler_finished >= handler_started
                ? handler_finished - handler_started : 0;
        session->performance.stylesheet_event_dispatches++;
        if (!dispatched_ok) {
            (void) navigation_retire_failed_document_refresh(
                session, session->page.runtime);
            return false;
        }
        /* A callback can promote this OR another link. Its next completion
           must follow the ordinary resource-install pass, not URL reuse. */
        if (script_runtime_has_pending_resource_mutations(
                session->page.runtime)) return true;
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

static void collect_document_stack_source_context_into(
    PocDocument *document, const char *stack, char *output,
    size_t output_size)
{
    static const char marker[] = "<inline-script>:1:";
    const char *at = stack;
    if (document == NULL || document->html == NULL || output == NULL
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
                lxb_dom_interface_node(document->html),
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

static void collect_stack_source_context_into(
    NavigationFrame *frame, const char *stack, char *output,
    size_t output_size)
{
    if (frame == NULL) return;
    collect_document_stack_source_context_into(
        &frame->document, stack, output, output_size);
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

#if NAVIGATION_CAPABILITY_TRACES_COMPILED
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
#endif
