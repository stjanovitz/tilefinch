#define NAVIGATION_STREAM_PREVIEW_MINIMUM_BYTES (8u * 1024u)
#define NAVIGATION_STREAM_PREVIEW_MINIMUM_NODES 48u
#define NAVIGATION_STREAM_PREVIEW_MAXIMUM_CHECKS 4u

static bool navigation_stream_preview_stylesheet_ready(
    const NavigationDocumentStream *stream)
{
    if (stream == NULL) return false;
    if (stream->blocking_stylesheets.failed == 0) return true;
    /* A failed stylesheet is a settled network error, not a reason to leave
       the screen blank indefinitely. Preserve mobile-layout safety by
       requiring the first external author sheet to be loaded; later failed
       sheets may refine the page when the authoritative retry succeeds. */
    for (size_t i = 0; i < stream->stylesheet_input_count; i++) {
        const NavigationStylesheetInput *input =
            &stream->stylesheet_inputs[i];
        if (input->style) continue;
        size_t href_length = 0;
        const char *href = document_attribute(
            input->node, "href", &href_length);
        if (stylesheet_document_resources_link_state(
                   &stream->stylesheet_resources,
                   stream->resource_base_url, href, href_length)
                == STYLESHEET_DOCUMENT_RESOURCE_LOADED) {
            return true;
        }
        break;
    }
    /* A retained response can be demoted to a transient retry state even
       though its already-compiled rules remain usable. In that case require
       a substantial loaded author sheet, not merely a tiny reset or icon
       sheet, before allowing a provisional frame. */
    return stream->blocking_stylesheet.count >= 512u
        || (stream->blocking_stylesheet.count >= 128u
            && stream->blocking_stylesheets.bytes >= 32u * 1024u);
}

static bool navigation_stream_preview_node_count(
    const PocDocument *document, size_t *count)
{
    if (count != NULL) *count = 0;
    if (document == NULL || document->html == NULL || count == NULL) {
        return false;
    }
    lxb_dom_node_t *node = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *boundary_parent = node->parent;
    while (node != NULL && node != boundary_parent) {
        if (*count == NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return false;
        (*count)++;
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

static bool navigation_stream_preview_attribute_present(
    lxb_dom_node_t *node, const char *name)
{
    size_t length = 0;
    return document_attribute(node, name, &length) != NULL;
}

/* Reject only prefixes containing inert payloads, whitespace, and empty
   unstyled structural elements. Replaced/form elements, list markers,
   generated content, background resources, or any authored styling hook fail
   open to layout. An empty tag selected solely by a type selector may be
   deferred to the next prefix, but the authoritative layout is unchanged;
   this bounded transient trade avoids repeatedly laying out half-tokenized
   wrappers and script bootstraps that cannot yet help the user. */
static bool navigation_stream_preview_may_have_visible_pixels(
    NavigationDocumentStream *stream, lxb_dom_node_t *body,
    size_t *visited_output)
{
    if (visited_output != NULL) *visited_output = 0;
    if (stream == NULL || body == NULL) return true;
    const Stylesheet *sheet = &stream->blocking_stylesheet;
    if (sheet->generated_text_count != 0 || sheet->image_url_count != 0) {
        return true;
    }
    lxb_dom_node_t *node = body->first_child;
    size_t visited = 0;
    while (node != NULL && node != body) {
        if (visited == NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return true;
        visited++;
        if ((visited & 31u) == 0
            && !tilefinch_platform_cooperate(
                   "streaming-preview-visibility", visited)) {
            return true;
        }
        bool skip_children = false;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t hidden_length = 0;
            bool hidden = document_attribute(
                node, "hidden", &hidden_length) != NULL;
            bool inert = node_name_is(node, "script")
                || node_name_is(node, "style")
                || node_name_is(node, "template")
                || node_name_is(node, "link")
                || node_name_is(node, "meta")
                || node_name_is(node, "title")
                || node_name_is(node, "base")
                || (stream->session->scripts_enabled
                    && node_name_is(node, "noscript"));
            if (hidden || inert) {
                skip_children = true;
            } else {
                bool intrinsic = node_name_is(node, "img")
                    || node_name_is(node, "svg")
                    || node_name_is(node, "video")
                    || node_name_is(node, "canvas")
                    || node_name_is(node, "iframe")
                    || node_name_is(node, "object")
                    || node_name_is(node, "embed")
                    || node_name_is(node, "input")
                    || node_name_is(node, "select")
                    || node_name_is(node, "textarea")
                    || node_name_is(node, "button")
                    || node_name_is(node, "hr")
                    || node_name_is(node, "progress")
                    || node_name_is(node, "meter")
                    || node_name_is(node, "li")
                    || node_name_is(node, "summary")
                    || node_name_is(node, "fieldset");
                bool styled = navigation_stream_preview_attribute_present(
                        node, "style")
                    || navigation_stream_preview_attribute_present(
                           node, "class")
                    || navigation_stream_preview_attribute_present(node, "id")
                    || navigation_stream_preview_attribute_present(
                           node, "width")
                    || navigation_stream_preview_attribute_present(
                           node, "height")
                    || navigation_stream_preview_attribute_present(
                           node, "border")
                    || navigation_stream_preview_attribute_present(
                           node, "background")
                    || navigation_stream_preview_attribute_present(
                           node, "bgcolor");
                if (intrinsic || styled) {
                    if (visited_output != NULL) *visited_output = visited;
                    return true;
                }
            }
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            const char *text = document_text_data(node, &length);
            for (size_t i = 0; text != NULL && i < length; i++) {
                if (!isspace((unsigned char) text[i])) {
                    if (visited_output != NULL) *visited_output = visited;
                    return true;
                }
            }
        }
        if (!skip_children && node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != body && node->next == NULL) node = node->parent;
        if (node == body) break;
        node = node->next;
    }
    if (visited_output != NULL) *visited_output = visited;
    return false;
}

/* A streaming preview never becomes page state. It consumes a closed-head
   body prefix and a stylesheet checkpoint whose complete input fingerprint
   still matches the current incomplete DOM. The parser is between feed calls
   here, so every preceding blocking script has finished in order and the DOM
   is stable for the duration of layout and paint. At the explicit external-
   script checkpoint, the current script has not executed yet; its closing tag
   and all preceding DOM are stable while the immutable preview is built. A
   stylesheet encountered later in the body may cause the ordinary
   authoritative repaint but can never mutate or promote this preview. */
static void navigation_stream_try_pre_eof_preview(
    NavigationDocumentStream *stream)
{
    if (stream == NULL || stream->session == NULL || stream->parser == NULL
        || stream->pre_eof_preview_attempted
        || stream->pre_eof_preview_painted) return;
    NavigationSession *session = stream->session;
    PocDocument *document = &stream->parser->document;
    if (session->progressive_paint == NULL
        || !session->external_resources_enabled
        || (session->page.loaded
            && !session->progressive_paint_preserves_incumbent)
        || document->html == NULL
        || (!stream->head_closed && !stream->body_closed)
        || stream->preview_checkpoint_checks
             >= NAVIGATION_STREAM_PREVIEW_MAXIMUM_CHECKS) {
        return;
    }
    lxb_dom_node_t *body = document_body_node(document);
    if (body == NULL || body->first_child == NULL
        || (stream->parsed_body_bytes
                < NAVIGATION_STREAM_PREVIEW_MINIMUM_BYTES
            && stream->closed_element_count
                < NAVIGATION_STREAM_PREVIEW_MINIMUM_NODES)) {
        return;
    }
    /* The preload scanner starts render-blocking transfers as soon as their
       tags arrive. Do not synchronously construct or refresh the stylesheet,
       however, until a useful body prefix can actually be painted. A small
       prefix may otherwise spend seconds parsing CSS, consume the one
       refresh attempt, and still have no viewport to present. */
    uint64_t fingerprint = 0;
    if (!navigation_stream_stylesheet_fingerprint(
            stream, document, &fingerprint)) {
        session->performance.streaming_preview_style_mismatches++;
        return;
    }
    bool stylesheet_current =
        stream->blocking_stylesheet_ready
        && stream->blocking_stylesheet_fingerprint_valid
        && fingerprint == stream->blocking_stylesheet_fingerprint;
    if (!stylesheet_current) {
        if (stream->blocking_stylesheet_ready
            && stream->blocking_stylesheet_fingerprint_valid) {
            session->performance.streaming_preview_style_mismatches++;
        }
        if (stream->pre_eof_preview_style_refresh_attempted) return;
        stream->pre_eof_preview_style_refresh_attempted = true;
        session->performance.streaming_preview_style_refresh_attempts++;
        const size_t refresh_estimate = 1u * 1024u * 1024u;
        const size_t refresh_reserve = 1u * 1024u * 1024u;
        if (budget_pressure_required(
                session->budget, refresh_estimate, refresh_reserve)) {
            session->performance.streaming_preview_pressure_skips++;
            budget_record_pressure(
                session->budget, BUDGET_PRESSURE_SPECULATION,
                refresh_estimate, 0);
            return;
        }
        uint64_t refresh_started = navigation_now_us();
        bool refreshed = navigation_stream_try_continue_stylesheet(
            stream, document, fingerprint);
        if (!refreshed) {
            long preview_timeout_ms = session->resource_timeout_ms;
            if (preview_timeout_ms
                > NAVIGATION_SPECULATIVE_RESOURCE_TIMEOUT_MS) {
                preview_timeout_ms =
                    NAVIGATION_SPECULATIVE_RESOURCE_TIMEOUT_MS;
            }
            refreshed =
                navigation_stream_build_blocking_stylesheet_with_timeout(
                    stream, document, fingerprint, preview_timeout_ms);
        }
        uint64_t refresh_elapsed = navigation_elapsed_us(
            refresh_started, navigation_now_us());
        session->performance.streaming_preview_style_refresh_us +=
            refresh_elapsed;
        /* A discovery pass can complete while individual external sheets
           remain transiently unavailable.  Such a sheet is valid for the
           parser's eventual bounded retry, but it is not a visually safe
           provisional checkpoint: painting it would flash an unstyled
           document. */
        if (!refreshed
            || !navigation_stream_preview_stylesheet_ready(stream)) {
            session->performance.streaming_preview_style_refresh_failures++;
            return;
        }
        session->performance.streaming_preview_style_refreshes++;
    }
    bool partial_stylesheets = stream->blocking_stylesheets.failed != 0;
    if (!navigation_stream_preview_stylesheet_ready(stream)) return;

    size_t visibility_nodes = 0;
    session->performance.streaming_preview_visibility_checks++;
    bool may_have_visible_pixels =
        navigation_stream_preview_may_have_visible_pixels(
            stream, body, &visibility_nodes);
    if (visibility_nodes
        > SIZE_MAX - session->performance.streaming_preview_visibility_nodes) {
        session->performance.streaming_preview_visibility_nodes = SIZE_MAX;
    } else {
        session->performance.streaming_preview_visibility_nodes +=
            visibility_nodes;
    }
    if (!may_have_visible_pixels) {
        session->performance.streaming_preview_visibility_skips++;
        return;
    }

    stream->preview_checkpoint_checks++;
    session->performance.streaming_preview_checks++;

    size_t preview_node_count = 0;
    if (!navigation_stream_preview_node_count(
            document, &preview_node_count)) return;
    size_t estimate =
        navigation_progressive_preview_estimate_nodes(preview_node_count);
    const size_t maximum_speculation = 4u * 1024u * 1024u;
    const size_t final_reserve = 1u * 1024u * 1024u;
    if (estimate > maximum_speculation
        || budget_pressure_required(
               session->budget, estimate, final_reserve)) {
        session->performance.streaming_preview_pressure_skips++;
        budget_record_pressure(
            session->budget, BUDGET_PRESSURE_SPECULATION, estimate, 0);
        stream->pre_eof_preview_attempted = true;
        return;
    }

    if (stream->preview_layout_reuse == NULL
        && preview_node_count >= 256
        && !budget_pressure_required(
               session->budget, 96u * 1024u, 512u * 1024u)) {
        stream->preview_layout_reuse =
            layout_reuse_cache_create(session->budget);
    }
    stream->pre_eof_preview_attempted = true;
    session->performance.streaming_preview_attempts++;
    uint64_t attempt_now = navigation_now_us();
    uint64_t attempt_relative =
        session->navigation_started_us != 0
        && attempt_now >= session->navigation_started_us
            ? attempt_now - session->navigation_started_us : 0;
    if (session->performance.streaming_preview_first_attempt_us == 0) {
        session->performance.streaming_preview_first_attempt_us =
            attempt_relative;
    }
    session->performance.streaming_preview_last_attempt_us =
        attempt_relative;
    session->performance.progressive_layout_attempts++;
    int preview_limit = stream->viewport.css_height;
    unsigned lookahead_percent =
        session->progressive_preview_lookahead_percent;
    int lookahead = (int) (
        (int64_t) preview_limit * lookahead_percent / 100);
    if (lookahead > INT_MAX - preview_limit) preview_limit = INT_MAX;
    else preview_limit += lookahead;
    session->performance.progressive_preview_y_limit = preview_limit;

    LayoutDocument preview = {0};
    LayoutReuseStats reuse_before = {0}, reuse_after = {0};
#ifndef TILEFINCH_NO_TRACE
    uint64_t compound_rejections_before =
        stream->blocking_stylesheet.rule_compound_filter_rejections;
    uint64_t ancestor_rejections_before =
        stream->blocking_stylesheet.rule_ancestor_filter_rejections;
    uint64_t selector_calls_before =
        stream->blocking_stylesheet.selector_match_calls;
    uint64_t selector_matches_before =
        stream->blocking_stylesheet.selector_match_successes;
    uint64_t selector_compounds_before =
        stream->blocking_stylesheet.selector_compound_calls;
    uint64_t selector_ancestors_before =
        stream->blocking_stylesheet.selector_ancestor_visits;
    uint64_t selector_attributes_before =
        stream->blocking_stylesheet.selector_attribute_checks;
    uint64_t selector_pseudos_before =
        stream->blocking_stylesheet.selector_pseudo_checks;
#endif
    layout_reuse_cache_stats(
        stream->preview_layout_reuse, &reuse_before);
    bool truncated = false;
    uint64_t layout_started = navigation_now_us();
    bool built = layout_build_context_preview(
        &preview, session->budget, document,
        &stream->blocking_stylesheet, stream->fonts, stream->images,
        &stream->viewport, stream->preview_layout_reuse,
        preview_limit, &truncated);
    uint64_t layout_elapsed = built
        ? navigation_record_layout_slices(session, layout_started, &preview)
        : navigation_record_slice(
              session, NAVIGATION_SLICE_LAYOUT, layout_started,
              document->node_count);
    session->performance.layout_us += layout_elapsed;
    session->performance.progressive_layout_us += layout_elapsed;
    session->performance.streaming_preview_layout_us += layout_elapsed;
    layout_reuse_cache_stats(
        stream->preview_layout_reuse, &reuse_after);
    navigation_record_layout_reuse(
        session, &reuse_before, &reuse_after);
    if (!built) {
        session->performance.progressive_layout_failures++;
        layout_destroy(&preview);
        return;
    }
#ifndef TILEFINCH_NO_TRACE
    if (getenv("TILEFINCH_TRACE_PREVIEW_LAYOUT") != NULL) {
        fprintf(stderr,
                "preview-layout-attribution total=%llu root=%llu flow=%llu "
                "compact=%llu focus=%llu paint-order=%llu spatial=%llu "
                "finalize=%llu styles=%llu style-us=%llu cache=%llu/%llu "
                "rules=%llu/%llu compound-rejects=%llu/%llu index=%zu/%zu "
                "selectors=%llu/%llu compounds=%llu ancestors=%llu "
                "attributes=%llu pseudos=%llu "
                "style-copy-bytes=%llu "
                "intrinsic=%llu/%llu/%llu/%llu margin=%llu/%llu/%llu "
                "iterators=%llu/%llu+%llu/%llu\n",
                (unsigned long long) preview.performance.total_us,
                (unsigned long long) preview.performance.root_style_us,
                (unsigned long long) preview.performance.flow_us,
                (unsigned long long) preview.performance.compact_us,
                (unsigned long long) preview.performance.focus_index_us,
                (unsigned long long) preview.performance.paint_order_us,
                (unsigned long long) preview.performance.spatial_index_us,
                (unsigned long long) preview.performance.finalize_us,
                (unsigned long long) preview.performance.style_resolutions,
                (unsigned long long) preview.performance.style_resolve_us,
                (unsigned long long) preview.performance.style_cache_hits,
                (unsigned long long) preview.performance.style_cache_misses,
                (unsigned long long) preview.performance.style_rule_queries,
                (unsigned long long) preview.performance.style_rule_candidates,
                (unsigned long long)
                    stream->blocking_stylesheet
                        .rule_compound_filter_rejections
                    - compound_rejections_before,
                (unsigned long long)
                    stream->blocking_stylesheet
                        .rule_ancestor_filter_rejections
                    - ancestor_rejections_before,
                stream->blocking_stylesheet.rule_index_universal_count,
                stream->blocking_stylesheet.count,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_match_calls
                    - selector_calls_before,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_match_successes
                    - selector_matches_before,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_compound_calls
                    - selector_compounds_before,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_ancestor_visits
                    - selector_ancestors_before,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_attribute_checks
                    - selector_attributes_before,
                (unsigned long long)
                    stream->blocking_stylesheet.selector_pseudo_checks
                    - selector_pseudos_before,
                (unsigned long long) preview.performance.style_resolutions
                    * sizeof(ComputedStyle),
                (unsigned long long) preview.performance.intrinsic_width_visits,
                (unsigned long long) preview.performance.intrinsic_min_visits,
                (unsigned long long) preview.performance.intrinsic_cache_hits,
                (unsigned long long) preview.performance.intrinsic_cache_misses,
                (unsigned long long) preview.performance.margin_collapse_visits,
                (unsigned long long) preview.performance.margin_cache_hits,
                (unsigned long long) preview.performance.margin_cache_misses,
                (unsigned long long) preview.performance.flat_iterator_passes,
                (unsigned long long) preview.performance.flat_iterator_yields,
                (unsigned long long) preview.performance.flex_iterator_passes,
                (unsigned long long) preview.performance.flex_iterator_yields);
    }
#endif
    session->performance.progressive_preview_commands = preview.count;
    session->performance.progressive_preview_node_boxes =
        preview.node_box_count;
    session->performance.streaming_preview_source_bytes =
        stream->parsed_body_bytes;
    session->performance.streaming_preview_node_count =
        preview_node_count;
    if (!navigation_preview_has_visible_content(
            &preview, stream->viewport.css_width,
            stream->viewport.css_height)) {
        session->performance.streaming_preview_empty_raster_skips++;
        stream->pre_eof_preview_attempted = false;
        layout_destroy(&preview);
        return;
    }
    /*
     * A decoded image is not a prerequisite for a useful first viewport.
     * CSS and replaced-element geometry are already resolved at this point;
     * paint styled text and transparent image slots just as a conventional
     * browser does, then let bounded image continuation fill them. The
     * preview is destroyed rather than promoted, so an image intrinsic-size
     * correction cannot become authoritative geometry.
     */
    if (navigation_preview_has_unresolved_visible_visuals(
            &preview, stream->viewport.css_height)) {
        session->performance.streaming_preview_visual_readiness_skips++;
    }
    uint64_t content_now = navigation_now_us();
    if (session->navigation_started_us != 0
        && content_now >= session->navigation_started_us) {
        session->performance.streaming_preview_content_ready_us =
            content_now - session->navigation_started_us;
    }
    session->performance.partial_layouts++;
    if (session->performance.first_layout_us == 0
        && session->navigation_started_us != 0) {
        session->performance.first_layout_us =
            navigation_now_us() - session->navigation_started_us;
    }

    uint64_t paint_started = navigation_now_us();
    bool painted = session->progressive_paint(
        session->progressive_paint_opaque, session, &preview);
    uint64_t paint_elapsed = navigation_record_slice(
        session, NAVIGATION_SLICE_PAINT, paint_started, preview.count);
    session->performance.progressive_paint_us += paint_elapsed;
    session->performance.streaming_preview_paint_us += paint_elapsed;
    if (painted) {
        stream->pre_eof_preview_painted = true;
        session->performance.streaming_preview_paints++;
        session->performance.partial_paints++;
        if (partial_stylesheets) {
            session->performance
                .streaming_preview_partial_stylesheet_paints++;
        }
        if (session->performance.first_paint_us == 0
            && session->navigation_started_us != 0) {
            session->performance.first_paint_us =
                navigation_now_us() - session->navigation_started_us;
        }
    } else {
        /* The compositor can decline a structurally valid preview when its
           current viewport is still visually empty. Keep the immutable
           stylesheet checkpoint, but allow a later response prefix to make
           one more bounded layout/raster attempt. preview_checkpoint_checks
           remains the hard retry ceiling. */
        stream->pre_eof_preview_attempted = false;
    }
    layout_destroy(&preview);
    (void) truncated;
}
