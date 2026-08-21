#define NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT 128u

static bool navigation_commit_checkpoint(
    NavigationSession *session, const char *phase, size_t completed_work)
{
    if (tilefinch_platform_cooperate(phase, completed_work)) return true;
    if (session != NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "navigation cancelled during %s",
                 phase == NULL ? "page construction" : phase);
    }
    return false;
}

static bool navigation_background_images_enabled(void)
{
#ifndef TILEFINCH_NO_TRACE
    return getenv("TILEFINCH_EXPERIMENTAL_BACKGROUND_IMAGES") != NULL;
#else
    return false;
#endif
}

static bool navigation_document_may_have_images(
    const PocDocument *document, const Stylesheet *stylesheet)
{
    if (stylesheet != NULL && stylesheet->image_url_count != 0) return true;
    if (document == NULL || document->html == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    for (lxb_dom_node_t *node = root; node != NULL;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (node_name_is(node, "img")
                || node_name_is(node, "svg"))) {
            return true;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return false;
}

static bool navigation_preview_node_visible(
    const LayoutDocument *preview, const lxb_dom_node_t *node,
    int viewport_height, bool *located)
{
    if (located != NULL) *located = false;
    if (preview == NULL || node == NULL || viewport_height <= 0) return false;
    for (size_t i = 0; i < preview->node_box_count; i++) {
        const LayoutNodeBox *box = &preview->node_boxes[i];
        if (box->node != node) continue;
        if (located != NULL) *located = true;
        if (box->height > 0 && box->y < viewport_height
            && (int64_t) box->y + box->height > 0) return true;
    }
    return false;
}

static bool navigation_priority_target_equal(
    const ImagePriorityTarget *left, const ImagePriorityTarget *right)
{
    return left != NULL && right != NULL
        && left->node == right->node && left->source == right->source
        && left->kind == right->kind && left->pseudo == right->pseudo;
}

static bool navigation_preview_has_unresolved_visible_visuals(
    const LayoutDocument *preview, int viewport_height)
{
    if (preview == NULL) return false;
    if (preview->visual_priority_overflow) return true;
    for (size_t i = 0; i < preview->visual_priority_count; i++) {
        bool located = false;
        if (navigation_preview_node_visible(
                preview, preview->visual_priority_targets[i].node,
                viewport_height, &located)
            || !located) return true;
    }
    return false;
}

static bool navigation_preview_has_visible_content(
    const LayoutDocument *preview, int viewport_width, int viewport_height)
{
    if (preview == NULL || viewport_width <= 0 || viewport_height <= 0) {
        return false;
    }
    for (size_t i = 0; i < preview->count; i++) {
        const DrawCommand *command = &preview->commands[i];
        if (command->width <= 0 || command->height <= 0
            || command->x >= viewport_width
            || (int64_t) command->x + command->width <= 0
            || command->y >= viewport_height
            || (int64_t) command->y + command->height <= 0) {
            continue;
        }
        if (command->type == DRAW_IMAGE && command->image != NULL) {
            return true;
        }
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length == 0) {
            continue;
        }
        for (size_t byte = 0; byte < command->text_length; byte++) {
            unsigned char character =
                (unsigned char) command->text[byte];
            if (character != ' ' && character != '\t'
                && character != '\n' && character != '\r'
                && character != '\f') {
                return true;
            }
        }
    }
    return false;
}

static bool navigation_image_priority_contains(
    const ImagePriorityTarget *targets, size_t count,
    const ImagePriorityTarget *candidate);
static bool navigation_svg_has_renderable_content(lxb_dom_node_t *svg);

static void navigation_collect_visible_svg_descendants(
    lxb_dom_node_t *root, ImagePriorityTarget *targets,
    size_t capacity, size_t *count)
{
    size_t visited = 0;
    for (lxb_dom_node_t *node = root == NULL ? NULL : root->first_child;
         node != NULL && *count < capacity && visited++ < 32u;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node_name_is(node, "svg")
            && navigation_svg_has_renderable_content(node)) {
            ImagePriorityTarget candidate = {
                .node = node,
                .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
                .pseudo = (uint8_t) PSEUDO_NONE
            };
            if (!navigation_image_priority_contains(
                    targets, *count, &candidate)) {
                targets[(*count)++] = candidate;
            }
            /* Inline SVG is an atomic replaced image; its descendants cannot
               contain independently paintable HTML resources. */
            while (node != root && node->next == NULL) node = node->parent;
            if (node == root) break;
            node = node->next;
            continue;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
}

static void navigation_collect_visible_image_priority(
    const LayoutDocument *preview, int viewport_height,
    ImagePriorityTarget *targets, size_t capacity, size_t *count)
{
    if (count == NULL) return;
    *count = 0;
    if (preview == NULL || targets == NULL || capacity == 0
        || viewport_height <= 0) return;
    for (size_t i = 0; i < preview->node_box_count; i++) {
        const LayoutNodeBox *box = &preview->node_boxes[i];
        if (box->node == NULL || box->height <= 0
            || box->y >= viewport_height
            || (int64_t) box->y + box->height <= 0) continue;
        if (box->width > 0 && box->width <= 192 && box->height <= 96) {
            navigation_collect_visible_svg_descendants(
                box->node, targets, capacity, count);
            if (*count == capacity) break;
        }
        if (!node_name_is(box->node, "img")) continue;
        int display_width = layout_css_to_visual(preview, box->width);
        int display_height = layout_css_to_visual(preview, box->height);
        ImagePriorityTarget candidate = {
            .node = box->node,
            .display_width = display_width > 0
                    && display_width <= UINT16_MAX
                ? (uint16_t) display_width : 0,
            .display_height = display_height > 0
                    && display_height <= UINT16_MAX
                ? (uint16_t) display_height : 0,
            .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
            .pseudo = (uint8_t) PSEUDO_NONE
        };
        bool duplicate = false;
        for (size_t j = 0; j < *count; j++) {
            if (navigation_priority_target_equal(
                    &targets[j], &candidate)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        targets[(*count)++] = candidate;
        if (*count == capacity) break;
    }
    for (size_t i = 0;
         i < preview->visual_priority_count && *count < capacity; i++) {
        const ImagePriorityTarget *candidate =
            &preview->visual_priority_targets[i];
        bool located = false;
        if (!navigation_preview_node_visible(
                preview, candidate->node, viewport_height, &located)
            && located) continue;
        bool duplicate = false;
        for (size_t j = 0; j < *count; j++) {
            if (navigation_priority_target_equal(
                    &targets[j], candidate)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) targets[(*count)++] = *candidate;
    }
}

static bool navigation_image_priority_contains(
    const ImagePriorityTarget *targets, size_t count,
    const ImagePriorityTarget *candidate)
{
    for (size_t i = 0; targets != NULL && i < count; i++) {
        if (navigation_priority_target_equal(&targets[i], candidate)) {
            return true;
        }
    }
    return false;
}

static bool navigation_attribute_equals(
    lxb_dom_node_t *node, const char *name, const char *value)
{
    size_t length = 0;
    const char *attribute = document_attribute(node, name, &length);
    size_t value_length = value == NULL ? 0 : strlen(value);
    return attribute != NULL && length == value_length
        && strncasecmp(attribute, value, length) == 0;
}

/* A speculative layout can be refused precisely when priority matters most.
   Preserve a useful bounded order without style/layout allocation: explicit
   fetchpriority=high images first, then eager markup images in DOM order.
   Lazy images remain for the document-wide continuation. */
static size_t navigation_collect_markup_image_priority(
    const PocDocument *document, ImagePriorityTarget *targets,
    size_t capacity, size_t existing_count)
{
    if (document == NULL || document->html == NULL || targets == NULL
        || existing_count >= capacity) return 0;
    size_t count = existing_count;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    for (unsigned pass = 0; pass < 2 && count < capacity; pass++) {
        size_t visited = 0;
        for (lxb_dom_node_t *node = root; node != NULL
             && visited++ < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;) {
            if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
                && node_name_is(node, "img")) {
                bool high = navigation_attribute_equals(
                    node, "fetchpriority", "high");
                bool lazy = navigation_attribute_equals(
                    node, "loading", "lazy");
                if ((pass == 0 && high) || (pass == 1 && !high && !lazy)) {
                    ImagePriorityTarget candidate = {
                        .node = node,
                        .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
                        .pseudo = (uint8_t) PSEUDO_NONE
                    };
                    if (!navigation_image_priority_contains(
                            targets, count, &candidate)) {
                        targets[count++] = candidate;
                    }
                }
            }
            if (node->first_child != NULL) {
                node = node->first_child;
                continue;
            }
            while (node != root && node->next == NULL) node = node->parent;
            if (node == root) break;
            node = node->next;
        }
    }
    return count - existing_count;
}

/* Inline SVGs have no network latency, but rasterizing them still consumes
   the same bounded image table and decoded-byte budget as external images.
   Seed a few early document icons before eager raster fallback so compact
   responsive navigation cannot disappear behind large hero artwork. The
   priority loader resolves the real ancestor style chain and harmlessly
   skips desktop/hidden variants. */
static bool navigation_svg_has_renderable_content(lxb_dom_node_t *svg)
{
    size_t visited = 0;
    for (lxb_dom_node_t *node = svg == NULL ? NULL : svg->first_child;
         node != NULL && visited++ < 64u;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (node_name_is(node, "path")
                || node_name_is(node, "rect")
                || node_name_is(node, "circle")
                || node_name_is(node, "ellipse")
                || node_name_is(node, "line")
                || node_name_is(node, "polyline")
                || node_name_is(node, "polygon")
                || node_name_is(node, "use")
                || node_name_is(node, "text"))) return true;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != svg && node->next == NULL) node = node->parent;
        if (node == svg) break;
        node = node->next;
    }
    return false;
}

static size_t navigation_collect_markup_svg_priority(
    const PocDocument *document, ImagePriorityTarget *targets,
    size_t capacity, size_t existing_count)
{
    enum { NAVIGATION_MARKUP_SVG_PRIORITY_LIMIT = 24 };
    if (document == NULL || document->html == NULL || targets == NULL
        || existing_count >= capacity) return 0;
    size_t count = existing_count;
    size_t added = 0, visited = 0;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    for (lxb_dom_node_t *node = root; node != NULL
         && count < capacity
         && added < NAVIGATION_MARKUP_SVG_PRIORITY_LIMIT
         && visited++ < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node_name_is(node, "svg")
            && navigation_svg_has_renderable_content(node)) {
            ImagePriorityTarget candidate = {
                .node = node,
                .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
                .pseudo = (uint8_t) PSEUDO_NONE
            };
            if (!navigation_image_priority_contains(
                    targets, count, &candidate)) {
                targets[count++] = candidate;
                added++;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return count - existing_count;
}

static size_t navigation_progressive_preview_estimate_nodes(size_t nodes)
{
    const size_t base = 768u * 1024u;
    const size_t per_node = 160u;
    if (nodes > (SIZE_MAX - base) / per_node) return SIZE_MAX;
    return base + nodes * per_node;
}

static size_t navigation_progressive_preview_estimate(
    const PocDocument *document)
{
    return navigation_progressive_preview_estimate_nodes(
        document == NULL ? 0 : document->node_count);
}

static bool navigation_collect_layout_image_geometry(
    const LayoutDocument *layout, ImagePriorityTarget *targets,
    size_t capacity, int viewport_height, size_t *visible_count,
    size_t *count)
{
    if (count == NULL || visible_count == NULL) return false;
    *count = 0;
    *visible_count = 0;
    if (layout == NULL || targets == NULL || capacity == 0) return false;
    /* Keep visible boxes first, followed by below-fold boxes in layout order.
       This preserves the first-render priority set while making the suffix a
       directly copyable deferred queue. */
    for (unsigned pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < layout->node_box_count; i++) {
            const LayoutNodeBox *box = &layout->node_boxes[i];
            if (box->node == NULL || !node_name_is(box->node, "img")
                || box->width <= 0 || box->height <= 0) continue;
            bool visible = box->y < viewport_height
                && (int64_t) box->y + box->height > 0;
            if (visible != (pass == 0)) continue;
            int width = layout_css_to_visual(layout, box->width);
            int height = layout_css_to_visual(layout, box->height);
            if (width <= 0 || height <= 0
                || width > UINT16_MAX || height > UINT16_MAX) return false;
            size_t existing = *count;
            for (size_t j = 0; j < *count; j++) {
                if (targets[j].node == box->node
                    && targets[j].kind == IMAGE_PRIORITY_KIND_DOCUMENT) {
                    existing = j;
                    break;
                }
            }
            if (existing != *count) {
                if (width > targets[existing].display_width) {
                    targets[existing].display_width = (uint16_t) width;
                }
                if (height > targets[existing].display_height) {
                    targets[existing].display_height = (uint16_t) height;
                }
                continue;
            }
            if (*count == capacity) return false;
            targets[(*count)++] = (ImagePriorityTarget) {
                .node = box->node,
                .display_width = (uint16_t) width,
                .display_height = (uint16_t) height,
                .kind = IMAGE_PRIORITY_KIND_DOCUMENT,
                .pseudo = (uint8_t) PSEUDO_NONE
            };
            if (pass == 0) {
                *visible_count = *count;
            }
        }
    }
    return *count != 0;
}

static bool navigation_document_images_are_defer_safe(
    const PocDocument *document, const Stylesheet *stylesheet)
{
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || stylesheet->image_url_count != 0) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t visited = 0;
    for (lxb_dom_node_t *node = root; node != NULL
         && visited++ < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (node_name_is(node, "svg")) return false;
            if (node_name_is(node, "video")) {
                size_t poster_length = 0;
                if (document_attribute(node, "poster", &poster_length)
                    != NULL && poster_length != 0) return false;
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return visited < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;
}

static bool navigation_plan_static_image_geometry(
    NavigationSession *session, const FontSet *fonts,
    const ImageResources *images, ImagePriorityTarget *targets,
    size_t capacity, size_t *count)
{
    if (count != NULL) *count = 0;
    if (session == NULL || count == NULL
        || session->page.document.node_count > 512u) return false;
    size_t estimate = navigation_progressive_preview_estimate(
        &session->page.document);
    if (estimate > 2u * 1024u * 1024u
        || budget_pressure_required(
               session->budget, estimate, 1u * 1024u * 1024u)) {
        return false;
    }
    navigation_ensure_layout_reuse(session);
    LayoutReuseStats reuse_before = {0}, reuse_after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &reuse_before);
    LayoutDocument probe = {0};
    uint64_t started = navigation_now_us();
    bool built = layout_build_context_reuse(
        &probe, session->budget, &session->page.document,
        &session->page.stylesheet, fonts, images, &session->viewport,
        session->page.layout_reuse);
    uint64_t elapsed = built
        ? navigation_record_layout_slices(session, started, &probe)
        : navigation_record_slice(
              session, NAVIGATION_SLICE_LAYOUT, started,
              session->page.document.node_count);
    session->performance.layout_us += elapsed;
    layout_reuse_cache_stats(session->page.layout_reuse, &reuse_after);
    navigation_record_layout_reuse(session, &reuse_before, &reuse_after);
    size_t total_count = 0;
    bool collected = built && navigation_collect_layout_image_geometry(
        &probe, targets, capacity, session->viewport.css_height,
        count, &total_count);
    if (collected && total_count != 0) {
        /* Retain the complete bounded plan, including the visible prefix.
           Successfully loaded prefix entries are cheap no-ops when the idle
           queue revisits them. A prefix response that failed transiently,
           however, now receives the same background retry opportunity as an
           offscreen image instead of remaining blank for the page lifetime. */
        size_t deferred_count = total_count;
        ImagePriorityTarget *deferred = budget_malloc_category(
            session->budget, BUDGET_CATEGORY_RESOURCE,
            deferred_count * sizeof(*deferred));
        if (deferred == NULL) {
            collected = false;
        } else {
            memcpy(deferred, targets,
                   deferred_count * sizeof(*deferred));
            session->page.deferred_image_targets = deferred;
            session->page.deferred_image_count = deferred_count;
        }
    }
    layout_destroy(&probe);
    if (!collected) *count = 0;
    return collected;
}

static bool navigation_try_progressive_preview(
    NavigationSession *session, NavigationDocumentStream *streaming,
    const NavigationPage *previous_page, bool has_deferred_visuals,
    const FontSet *fonts, const ImageResources *images,
    bool visual_resources_complete, LayoutDocument *authoritative_layout,
    ImagePriorityTarget *image_priority_targets,
    size_t image_priority_capacity,
    size_t *image_priority_count)
{
    if (image_priority_count != NULL) *image_priority_count = 0;
    if (session == NULL || streaming == NULL || previous_page != NULL
        || authoritative_layout == NULL
        || session->progressive_paint == NULL
        || session->performance.partial_paints != 0
        || !session->external_resources_enabled
        || (session->loads_committed != 0
            && session->performance.low_memory_last_rollback_available)
        || !has_deferred_visuals) return false;
#ifndef TILEFINCH_NO_TRACE
    /* Lab-only A/B control. Release/PSP builds compile out the environment
       branch and always use the bounded policy selected by the embedder. */
    if (getenv("TILEFINCH_DISABLE_BOUNDED_LAYOUT_PREVIEW") != NULL) {
        return false;
    }
#endif
    session->performance.progressive_layout_attempts++;
    size_t estimate = navigation_progressive_preview_estimate(
        &session->page.document);
    const size_t maximum_speculation = 4u * 1024u * 1024u;
    const size_t final_reserve = 1u * 1024u * 1024u;
    if (estimate > maximum_speculation
        || budget_pressure_required(
               session->budget, estimate, final_reserve)) {
        session->performance.progressive_layout_skips++;
        budget_record_pressure(
            session->budget, BUDGET_PRESSURE_SPECULATION, estimate, 0);
        return false;
    }

    navigation_ensure_layout_reuse(session);
    int preview_limit = session->viewport.css_height;
    int lookahead = preview_limit / 2;
    if (lookahead > INT_MAX - preview_limit) preview_limit = INT_MAX;
    else preview_limit += lookahead;
    session->performance.progressive_preview_y_limit = preview_limit;
    LayoutDocument preview = {0};
    LayoutReuseStats reuse_before = {0}, reuse_after = {0};
    layout_reuse_cache_stats(session->page.layout_reuse, &reuse_before);
    bool truncated = false;
    uint64_t layout_started = navigation_now_us();
    bool built = layout_build_context_preview(
        &preview, session->budget, &session->page.document,
        &session->page.stylesheet, fonts, images, &session->viewport,
        session->page.layout_reuse, preview_limit, &truncated);
    uint64_t layout_elapsed = built
        ? navigation_record_layout_slices(session, layout_started, &preview)
        : navigation_record_slice(
              session, NAVIGATION_SLICE_LAYOUT, layout_started,
              session->page.document.node_count);
    session->performance.layout_us += layout_elapsed;
    session->performance.progressive_layout_us += layout_elapsed;
    layout_reuse_cache_stats(session->page.layout_reuse, &reuse_after);
    navigation_record_layout_reuse(
        session, &reuse_before, &reuse_after);
    if (!built) {
        session->performance.progressive_layout_failures++;
        layout_destroy(&preview);
        return false;
    }
    session->performance.progressive_preview_commands = preview.count;
    session->performance.progressive_preview_node_boxes =
        preview.node_box_count;
    navigation_collect_visible_image_priority(
        &preview, session->viewport.css_height, image_priority_targets,
        image_priority_capacity, image_priority_count);
    if (!visual_resources_complete
        && navigation_preview_has_unresolved_visible_visuals(
               &preview, session->viewport.css_height)) {
        session->performance.progressive_visual_readiness_skips++;
        layout_destroy(&preview);
        return false;
    }
    if (session->performance.first_layout_us == 0
        && session->navigation_started_us != 0) {
        session->performance.first_layout_us =
            navigation_now_us() - session->navigation_started_us;
    }
    /* The preview limit is the only semantic difference in the layout
       builder: every early exit marks the result truncated. When the result
       also reports no unresolved external visual, it is authoritative even
       if the resource pipeline has unrelated below-fold work left. Move it
       directly into page state so a small page does not immediately repeat
       the same style, intrinsic-size, focus, and paint-order work. */
    if (visual_resources_complete && !truncated
        && !preview.unresolved_external_visuals) {
        *authoritative_layout = preview;
        memset(&preview, 0, sizeof(preview));
        session->performance.progressive_layout_adoptions++;
        session->performance.final_layout_us += layout_elapsed;
        return true;
    }
    /* A genuinely truncated prefix is paint-only. Its reuse cache carries
       safe style and measurement work into the authoritative build below. */
    session->performance.partial_layouts++;
    const LayoutDocument *paint_layout = &preview;
    uint64_t paint_started = navigation_now_us();
    bool painted = session->progressive_paint(
        session->progressive_paint_opaque, session, paint_layout);
    uint64_t paint_elapsed = navigation_record_slice(
        session, NAVIGATION_SLICE_PAINT, paint_started, paint_layout->count);
    session->performance.progressive_paint_us += paint_elapsed;
    if (painted) {
        session->performance.partial_paints++;
        if (session->performance.first_paint_us == 0
            && session->navigation_started_us != 0) {
            session->performance.first_paint_us =
                navigation_now_us() - session->navigation_started_us;
        }
    }
    layout_destroy(&preview);
    (void) truncated;
    (void) visual_resources_complete;
    return false;
}

static void navigation_paint_authoritative_layout(
    NavigationSession *session, NavigationDocumentStream *streaming,
    const NavigationPage *previous_page, bool has_deferred_visuals,
    const LayoutDocument *layout)
{
    if (session == NULL || streaming == NULL || previous_page != NULL
        || layout == NULL || session->progressive_paint == NULL
        || !session->external_resources_enabled || !has_deferred_visuals
        || session->performance.partial_paints != 0) return;
    uint64_t paint_started = navigation_now_us();
    bool painted = session->progressive_paint(
        session->progressive_paint_opaque, session, layout);
    uint64_t paint_elapsed = navigation_record_slice(
        session, NAVIGATION_SLICE_PAINT, paint_started, layout->count);
    session->performance.progressive_paint_us += paint_elapsed;
    if (!painted) return;
    session->performance.partial_paints++;
    if (session->performance.first_paint_us == 0
        && session->navigation_started_us != 0) {
        session->performance.first_paint_us =
            navigation_now_us() - session->navigation_started_us;
    }
}

static bool navigation_document_requires_runtime(
    const NavigationSession *session, const PocDocument *document)
{
    if (session == NULL || document == NULL || document->html == NULL) {
        return true;
    }
    if (session->trace_page_capabilities) return true;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    size_t visited = 0;
    while (node != NULL) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) return true;
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        bool script = name != NULL && name_length == 6
            && memcmp(name, "script", 6) == 0;
        if (name != NULL
            && ((name_length == 6 && memcmp(name, "iframe", 6) == 0)
                || (name_length == 5 && memcmp(name, "frame", 5) == 0))) {
            return true;
        }
        if (script) {
            size_t type_length = 0;
            const char *type = document_attribute(node, "type", &type_length);
            while (type_length != 0
                   && isspace((unsigned char) *type)) {
                type++;
                type_length--;
            }
            while (type_length != 0
                   && isspace((unsigned char) type[type_length - 1u])) {
                type_length--;
            }
            bool executable = type == NULL || type_length == 0
                || (type_length == 6 && strncasecmp(type, "module", 6) == 0)
                || (type_length == 15
                    && strncasecmp(type, "text/javascript", 15) == 0)
                || (type_length == 22
                    && strncasecmp(
                        type, "application/javascript", 22) == 0);
            if (executable) return true;
        }
        if (name != NULL) {
            lxb_dom_element_t *element = lxb_dom_interface_element(node);
            for (lxb_dom_attr_t *attribute = element->first_attr;
                 attribute != NULL; attribute = attribute->next) {
                size_t attribute_name_length = 0;
                const lxb_char_t *attribute_name =
                    lxb_dom_attr_local_name(
                        attribute, &attribute_name_length);
                if (attribute_name != NULL && attribute_name_length > 2
                    && (attribute_name[0] == 'o'
                        || attribute_name[0] == 'O')
                    && (attribute_name[1] == 'n'
                        || attribute_name[1] == 'N')
                    && attribute->value != NULL
                    && attribute->value->length != 0) return true;
            }
            size_t href_length = 0;
            const char *href = document_attribute(
                node, "href", &href_length);
            static const char javascript_scheme[] = "javascript:";
            while (href_length != 0
                   && isspace((unsigned char) *href)) {
                href++;
                href_length--;
            }
            if (href != NULL
                && href_length >= sizeof(javascript_scheme) - 1u
                && strncasecmp(
                    href, javascript_scheme,
                    sizeof(javascript_scheme) - 1u) == 0) return true;
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL) {
            node = node->parent;
        }
        if (node == root) break;
        if (node != NULL) node = node->next;
    }
    return false;
}

static bool navigation_finish_authoritative_layout(
    NavigationSession *session, const char *url,
    NavigationDocumentStream *streaming,
    const NavigationPage *previous_page,
    bool progressive_preview_candidate,
    const FontSet *fonts, const ImageResources *images)
{
    /* A completed client shell can contain substantial authored text yet
       yield no meaningful paint commands. Retry that exceptional case once
       with the bounded static-content policy. The old layout remains owned
       until the replacement proves it exposes more content, so recovery is
       both off the normal path and transactional. */
    const char *body_text = session->page.layout.count <= 1u
        ? document_body_text(&session->page.document) : NULL;
    if (body_text != NULL && session->page.document.body_text_length >= 64u
        && !session->page.stylesheet.static_custom_element_fallback) {
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
        (void) document_install_static_shell_fallback(
            &session->page.document);
        LayoutDocument recovered = {0};
        uint64_t recovery_started = navigation_now_us();
        if (navigation_build_layout(session, &recovered, fonts, images)
            && recovered.count > session->page.layout.count) {
            layout_destroy(&session->page.layout);
            session->page.layout = recovered;
            uint64_t elapsed = navigation_record_layout_slices(
                session, recovery_started, &session->page.layout);
            session->performance.layout_us += elapsed;
            session->performance.final_layout_us += elapsed;
        } else {
            layout_destroy(&recovered);
        }
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-layout",
            session->page.layout.layout_work_units)) return false;
    if (session->performance.first_layout_us == 0
        && session->navigation_started_us != 0) {
        session->performance.first_layout_us =
            navigation_now_us() - session->navigation_started_us;
    }
    navigation_paint_authoritative_layout(
        session, streaming, previous_page, progressive_preview_candidate,
        &session->page.layout);
    if (session->page.runtime != NULL) {
        navigation_attach_layout(session);
        (void) script_runtime_consume_relayout(session->page.runtime);
    }
    session->page.loaded = true;
    uint64_t phase_started = navigation_now_us();
    if (!navigation_resource_fingerprint(
            session, &session->page.document,
            &session->resource_fingerprint)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "resource fingerprint traversal failed");
        return false;
    }
    uint64_t fingerprint_finished = navigation_now_us();
    session->performance.resource_fingerprint_us +=
        fingerprint_finished >= phase_started
            ? fingerprint_finished - phase_started : 0;
    session->style_rebuild_required = false;
    session->fonts = fonts;
    session->images = images;
    if (session->document_scripts_enabled
        && session->page.runtime != NULL
        && !session->defer_document_script_pipeline
        && !navigation_load_frames(session, url)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "frame messaging failed: %.440s",
                 session->page.script_result.error);
        return false;
    }
    return navigation_commit_checkpoint(
        session, "navigation-finalize",
        session->page.document.node_count);
}

static bool navigation_commit_document(
    NavigationSession *session, uint64_t generation, const char *url,
    PocDocument *document, int viewport_width, const FontSet *fonts,
    const ImageResources *images, bool record_history,
    NavigationDocumentStream *streaming,
    FetchSchedulerDomain *shared_fetch_domain,
    bool allow_runtime,
    bool defer_authoritative_layout,
    LayoutBuildJob **deferred_layout_job,
    bool *deferred_preview_candidate)
{
    if (deferred_layout_job != NULL) *deferred_layout_job = NULL;
    if (deferred_preview_candidate != NULL) {
        *deferred_preview_candidate = false;
    }
    if (session == NULL || session->budget == NULL || generation == 0
        || generation != session->generation || session->cancelled
        || url == NULL || document == NULL || document->html == NULL
        || viewport_width <= 0) return false;
    ReaderDocumentAnalysis reader_analysis = {0};
    if (session->prepare_reader_candidates
        && !reader_document_prepare(document, &reader_analysis)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "reader candidate analysis failed within the page budget");
        return false;
    }
    NavigationCandidatePrepareCallback candidate_prepare =
        session->candidate_prepare;
    NavigationCandidateFinalizeCallback candidate_abort =
        session->candidate_abort;
    NavigationCandidateFinalizeCallback candidate_commit =
        session->candidate_commit;
    void *candidate_opaque = session->candidate_commit_opaque;
    bool isolates_remote_document = streaming != NULL
        && streaming->isolates_remote_document;
    char response_referrer_policy[
        sizeof(session->pending_response_referrer_policy)];
    snprintf(response_referrer_policy, sizeof(response_referrer_policy),
             "%s", session->pending_response_referrer_policy);
    char resource_base_url_storage[NAVIGATION_URL_LIMIT];
    if (!navigation_document_resource_base(
            document, url, resource_base_url_storage)) {
        return false;
    }
    const char *resource_base_url = resource_base_url_storage;
    bool fetch_domain_from_stream = streaming != NULL
        && streaming->fetch_domain != NULL;
    FetchSchedulerDomain *retained_fetch_domain =
        shared_fetch_domain == NULL
            ? session->page.fetch_domain : shared_fetch_domain;
    FetchSchedulerDomain *candidate_fetch_domain =
        fetch_domain_from_stream
            ? streaming->fetch_domain
            : (retained_fetch_domain == NULL
                 ? fetch_scheduler_domain_create(
                       session->budget,
                       NAVIGATION_PAGE_FETCH_SLOT_LIMIT)
                 : fetch_scheduler_domain_retain(retained_fetch_domain));
    if (candidate_fetch_domain == NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "page fetch scheduler domain startup failed");
        return false;
    }
    NavigationPage *previous_page = NULL;
    if (page_has_owned_state(&session->page)) {
        previous_page = budget_malloc_category(
            session->budget, BUDGET_CATEGORY_SESSION,
            sizeof(*previous_page));
        if (previous_page == NULL) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "navigation transaction scratch allocation failed");
            if (!fetch_domain_from_stream) {
                fetch_scheduler_domain_release(candidate_fetch_domain);
            }
            return false;
        }
        *previous_page = session->page;
    }
    ViewportContext previous_viewport = session->viewport;
    const FontSet *previous_fonts = session->fonts;
    const ImageResources *previous_images = session->images;
    uint64_t previous_resource_fingerprint = session->resource_fingerprint;
    bool previous_style_rebuild_required =
        session->style_rebuild_required;
    bool previous_relayout_damage_valid = session->relayout_damage_valid;
    int previous_relayout_damage_left = session->relayout_damage_left;
    int previous_relayout_damage_top = session->relayout_damage_top;
    int previous_relayout_damage_right = session->relayout_damage_right;
    int previous_relayout_damage_bottom = session->relayout_damage_bottom;
    bool candidate_hook_attempted = false;
    session->pending_response_referrer_policy[0] = '\0';
    memset(&session->page, 0, sizeof(session->page));
    session->page.fetch_domain = candidate_fetch_domain;
    if (fetch_domain_from_stream) streaming->fetch_domain = NULL;
    snprintf(session->page.referrer_policy,
             sizeof(session->page.referrer_policy), "%s",
             response_referrer_policy);
    session->page.document = *document;
    if (session->prepare_reader_candidates)
        session->page.reader_analysis = reader_analysis;
    memset(document, 0, sizeof(*document));
    snprintf(session->page.resource_base_url,
             sizeof(session->page.resource_base_url), "%s",
             resource_base_url);
    snprintf(session->page.document_url,
             sizeof(session->page.document_url), "%s", url);
    if (streaming != NULL
        && streaming->stylesheet_event_baseline_adopted) {
        memcpy(session->page.stylesheet_events,
               streaming->stylesheet_events,
               streaming->stylesheet_event_count
                   * sizeof(*session->page.stylesheet_events));
        session->page.stylesheet_event_count =
            streaming->stylesheet_event_count;
    }
    if (streaming != NULL) {
        session->page.stylesheet_resources =
            streaming->stylesheet_resources;
        memset(&streaming->stylesheet_resources, 0,
               sizeof(streaming->stylesheet_resources));
    }
    bool streamed_runtime = streaming != NULL && streaming->runtime != NULL;
    if (streamed_runtime) {
        script_runtime_set_synchronous_layout_callback(
            streaming->runtime, NULL, NULL);
        script_runtime_set_layout(
            streaming->runtime, NULL, streaming->viewport.css_height);
        if (streaming->cssom_layout_ready) {
            layout_destroy(&streaming->cssom_layout);
            memset(&streaming->cssom_layout, 0,
                   sizeof(streaming->cssom_layout));
            streaming->cssom_layout_ready = false;
        }
        session->page.runtime = streaming->runtime;
        if (streaming->runtime_document_replacement) {
            if (!script_runtime_rebind_document(
                    session->page.runtime, &session->page.document,
                    &streaming->script_result)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "JavaScript document rebind failed: %.430s",
                         streaming->script_result.error);
                streaming->runtime_transferred = true;
                goto fail;
            }
        } else {
            /* The parser-owned PocDocument struct moved into the committed
               page, but its Lexbor tree and JavaScript realm did not change.
               Preserve handles and script identity across that storage move. */
            script_runtime_relocate_document_storage(
                session->page.runtime, &session->page.document);
        }
        session->page.script_result = streaming->script_result;
        streaming->runtime_transferred = true;
    } else if (streaming != NULL
               && streaming->script_disabled_after_failure) {
        session->page.script_result = streaming->script_result;
        session->script_discovered = streaming->scripts.early.discovered;
        session->script_attempted = streaming->scripts.early.attempted;
        session->script_loaded = streaming->scripts.early.loaded;
        session->script_failed = streaming->scripts.early.failed;
        session->script_skipped_quota =
            streaming->scripts.early.skipped_quota;
        session->script_skipped_pressure =
            streaming->scripts.early.skipped_pressure;
        session->script_inline_data_quota_exemptions =
            streaming->scripts.early.inline_data_quota_exemptions;
        session->script_cost_class_rejections =
            streaming->scripts.early.cost_class_rejections;
        session->script_watchdog_classification_misses =
            streaming->scripts.early.watchdog_classification_misses;
        session->script_watchdog_classification_miss_bytes =
            streaming->scripts.early.watchdog_classification_miss_bytes;
        session->script_watchdog_classification_miss_loops =
            streaming->scripts.early.watchdog_classification_miss_loops;
        session->script_watchdog_classification_miss_flags =
            streaming->scripts.early.watchdog_classification_miss_flags;
    }
    if (streaming != NULL && streaming->scheduler != NULL) {
        session->page.resource_scheduler = streaming->scheduler;
        streaming->scheduler_transferred = true;
    }
    if (!viewport_context_resolve(&session->viewport,
                                  &session->page.document,
                                  viewport_width, 272, 980)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "mobile viewport resolution failed");
        goto fail;
    }
    navigation_apply_declared_viewport(session, viewport_width, 272);
    if (streaming != NULL
        && streaming->preview_layout_reuse != NULL) {
        if (streaming->viewport.css_width == session->viewport.css_width
            && streaming->viewport.css_height
                == session->viewport.css_height) {
            session->page.layout_reuse =
                streaming->preview_layout_reuse;
            streaming->preview_layout_reuse = NULL;
        } else {
            layout_reuse_cache_destroy(
                streaming->preview_layout_reuse);
            streaming->preview_layout_reuse = NULL;
        }
    }
    bool optional_work_shed = streaming != NULL
        && streaming->optional_work_shed;
    if (!optional_work_shed && (session->document_scripts_enabled
        || session->external_resources_enabled
        || session->web_fonts_enabled)) {
        size_t maximum_file = session->document_scripts_enabled
            ? session->maximum_script_file_bytes : 0;
        if (session->external_resources_enabled
            && session->maximum_stylesheet_file_bytes > maximum_file) {
            maximum_file = session->maximum_stylesheet_file_bytes;
        }
        if (session->external_resources_enabled
            && session->maximum_image_file_bytes > maximum_file) {
            maximum_file = session->maximum_image_file_bytes;
        }
        if (session->web_fonts_enabled
            && session->maximum_font_file_bytes > maximum_file) {
            maximum_file = session->maximum_font_file_bytes;
        }
        size_t reserved = maximum_file <= SIZE_MAX / 4u
                          ? maximum_file * 4u : maximum_file;
        if (session->page.resource_scheduler == NULL) {
            session->page.resource_scheduler =
                fetch_scheduler_create_in_domain(
                    session->page.fetch_domain, 12, reserved);
            if (session->page.resource_scheduler != NULL) {
                (void) fetch_scheduler_enable_background_transport(
                    session->page.resource_scheduler, true);
            }
        }
        if (session->page.resource_scheduler == NULL) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "document resource scheduler startup failed");
            goto fail;
        }
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-setup",
            session->page.document.node_count)) goto fail;
    bool adopted_streaming_stylesheet = false;
    bool adopted_compiled_stylesheet = false;
    bool stylesheet_external_current = false;
    bool user_stylesheet_current = false;
    bool script_document_may_have_changed = false;
    uint64_t adopted_stylesheet_fingerprint = 0;
    uint64_t phase_started = navigation_now_us();
    if (streaming != NULL && streaming->blocking_stylesheet_ready
        && streaming->blocking_stylesheet_fingerprint_valid
        && streaming->viewport.css_width == session->viewport.css_width
        && streaming->viewport.css_height == session->viewport.css_height
        && strcmp(streaming->resource_base_url,
                  session->page.resource_base_url) == 0
        && strcmp(streaming->referrer_policy,
                  session->page.referrer_policy) == 0) {
        uint64_t fingerprint_started = navigation_now_us();
        uint64_t final_fingerprint = 0;
        bool fingerprinted = navigation_stream_stylesheet_fingerprint(
            streaming, &session->page.document, &final_fingerprint);
        uint64_t fingerprint_finished = navigation_now_us();
        session->performance.blocking_stylesheet_fingerprint_us +=
            fingerprint_finished >= fingerprint_started
                ? fingerprint_finished - fingerprint_started : 0;
        if (fingerprinted
            && final_fingerprint
               == streaming->blocking_stylesheet_fingerprint) {
            session->page.stylesheet = streaming->blocking_stylesheet;
            layout_reuse_cache_rebind_stylesheet(
                session->page.layout_reuse,
                &streaming->blocking_stylesheet,
                &session->page.stylesheet);
            memset(&streaming->blocking_stylesheet, 0,
                   sizeof(streaming->blocking_stylesheet));
            streaming->blocking_stylesheet_ready = false;
            session->page.external_stylesheets =
                streaming->blocking_stylesheets;
            adopted_streaming_stylesheet = true;
            stylesheet_external_current = true;
            user_stylesheet_current = true;
            adopted_stylesheet_fingerprint = final_fingerprint;
            session->page.compiled_stylesheet_fingerprint =
                final_fingerprint;
            session->page.compiled_stylesheet_resource_signature =
                browser_session_stylesheet_cache_signature(
                    session->browser_session,
                    tilefinch_platform_monotonic_time_ns());
            session->page.compiled_stylesheet_build_generation =
                session->page.stylesheet.build_generation;
            session->page.compiled_stylesheet_cacheable =
                session->page.compiled_stylesheet_resource_signature != 0
                && session->page.external_stylesheets.skipped_pressure == 0
                && session->performance.stylesheet_pressure_fallbacks == 0;
            session->performance.blocking_stylesheet_adoptions++;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
    }
    if (!adopted_streaming_stylesheet && streaming != NULL) {
        uint64_t fingerprint_started = navigation_now_us();
        uint64_t final_fingerprint = 0;
        bool fingerprinted = navigation_stream_stylesheet_fingerprint(
            streaming, &session->page.document, &final_fingerprint);
        session->performance.blocking_stylesheet_fingerprint_us +=
            navigation_now_us() - fingerprint_started;
        if (fingerprinted
            && navigation_compiled_stylesheet_cache_take(
                session, final_fingerprint, &session->page.stylesheet,
                &session->page.external_stylesheets)) {
            adopted_compiled_stylesheet = true;
            stylesheet_external_current = true;
            user_stylesheet_current = true;
            adopted_stylesheet_fingerprint = final_fingerprint;
            session->page.compiled_stylesheet_fingerprint =
                final_fingerprint;
            session->page.compiled_stylesheet_resource_signature =
                browser_session_stylesheet_cache_signature(
                    session->browser_session,
                    tilefinch_platform_monotonic_time_ns());
            session->page.compiled_stylesheet_build_generation =
                session->page.stylesheet.build_generation;
            session->page.compiled_stylesheet_cacheable =
                session->page.compiled_stylesheet_resource_signature != 0
                && session->page.external_stylesheets.skipped_pressure == 0
                && session->performance.stylesheet_pressure_fallbacks == 0;
        }
    }
    if (!adopted_streaming_stylesheet && !adopted_compiled_stylesheet
        && !navigation_build_stylesheet(session)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "stylesheet construction failed");
        goto fail;
    }
    session->performance.style_us += navigation_record_slice(
        session, NAVIGATION_SLICE_STYLE, phase_started,
        session->page.document.node_count);
    if (!navigation_commit_checkpoint(
            session, "navigation-style",
            session->page.document.node_count)) goto fail;
    if (session->external_resources_enabled) {
        if (stylesheet_document_resources_prepare_complete_census(
                &session->page.stylesheet_resources)) {
            stylesheet_external_current = false;
            user_stylesheet_current = false;
            session->page.compiled_stylesheet_cacheable = false;
        }
        phase_started = navigation_now_us();
        /* Parser-time author code may leave temporary QuickJS objects alive
           immediately before the authoritative stylesheet pass. A normal GC
           checkpoint is standards-safe here and lets visible presentation
           reclaim capacity before optional script state can crowd it out. */
        if (session->page.runtime != NULL) {
            (void) script_runtime_collect_and_trim(session->page.runtime);
        }
        if (!stylesheet_external_current
            && !stylesheets_load_external_tracked_with_context(
                &session->page.document, &session->page.stylesheet,
                session->budget, resource_base_url, url,
                session->page.referrer_policy,
                session->maximum_stylesheets,
                session->maximum_stylesheet_bytes,
                session->maximum_stylesheet_file_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session,
                &session->page.stylesheet_resources,
                &session->page.external_stylesheets)) {
            if (!navigation_install_usable_stylesheet_fallback(session)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "stylesheet fallback exceeded its budget");
                goto fail;
            }
            stylesheet_external_current = true;
            user_stylesheet_current = false;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
        /* Preserve the final bounded retry when an individual stylesheet
           failed even though the aggregate discovery pass completed. */
        stylesheet_external_current = stylesheet_external_current
            || session->page.external_stylesheets.failed == 0;
        if (streaming == NULL
            || !streaming->stylesheet_event_baseline_adopted) {
            navigation_adopt_stylesheet_events(
                &session->page.document, session->page.resource_base_url,
                session->page.stylesheet_events,
                &session->page.stylesheet_event_count);
        }
        if (!adopted_streaming_stylesheet && !adopted_compiled_stylesheet) {
            uint64_t stylesheet_elapsed = navigation_record_resource_slices(
                session, phase_started, &session->page.external_stylesheets,
                NULL);
            session->performance.resource_us += stylesheet_elapsed;
            session->performance.stylesheet_resource_us +=
                stylesheet_elapsed;
        }
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-stylesheets",
            session->page.external_stylesheets.discovered)) goto fail;
    if (session->user_css_length != 0 && !user_stylesheet_current) {
        phase_started = navigation_now_us();
        if (!stylesheet_add_user_css(&session->page.stylesheet,
                                     session->user_css,
                                     session->user_css_length)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "script-blocking user stylesheet exceeded its budget");
            goto fail;
        }
        session->performance.style_us += navigation_record_slice(
            session, NAVIGATION_SLICE_STYLE, phase_started,
            session->page.stylesheet.count);
        user_stylesheet_current = true;
    }
    phase_started = navigation_now_us();
    bool runtime_required = allow_runtime && session->scripts_enabled
        && (streaming == NULL
            || !streaming->script_disabled_after_failure)
        && (streaming == NULL || streamed_runtime
            || navigation_document_requires_runtime(
                session, &session->page.document));
    if (runtime_required) {
        if (session->page.runtime == NULL) {
            session->page.runtime = navigation_create_runtime(
                session, &session->page.document, url,
                session->page.referrer_policy,
                session->document_scripts_enabled
                    || session->defer_document_script_pipeline,
                &session->viewport, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
                !isolates_remote_document, session->page.fetch_domain,
                &session->page.stylesheet,
                &session->page.script_result);
        }
        if (session->page.runtime == NULL) {
            snprintf(session->last_error, sizeof(session->last_error), "%s",
                     session->page.script_result.error[0] == '\0'
                       ? "JavaScript runtime startup failed"
                       : session->page.script_result.error);
            goto fail;
        }
        script_runtime_set_stylesheet(session->page.runtime,
                                      &session->page.stylesheet);
        script_document_may_have_changed = true;
        if (!page_capability_trace_setup(session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "page capability trace setup failed: %.430s",
                     session->page.script_result.error);
            goto fail;
        }
        if (session->document_scripts_enabled
            && !session->defer_document_script_pipeline) {
            ExternalScriptMetrics metrics;
            bool scripts_ok = streamed_runtime
                ? document_scripts_finish_streaming(
                    &session->page.document, session->page.runtime,
                    session->budget, session->browser_session,
                    resource_base_url, url,
                    session->page.referrer_policy,
                    session->maximum_scripts,
                    session->maximum_script_bytes,
                    session->maximum_script_file_bytes,
                    session->script_timeout_ms,
                    session->page.resource_scheduler, &streaming->scripts,
                    &metrics, &session->page.script_result)
                : document_scripts_execute(
                    &session->page.document, session->page.runtime,
                    session->budget, session->browser_session,
                    resource_base_url, url,
                    session->page.referrer_policy,
                    session->maximum_scripts,
                    session->maximum_script_bytes,
                    session->maximum_script_file_bytes,
                    session->script_timeout_ms,
                    session->page.resource_scheduler, &metrics,
                    &session->page.script_result);
            if (!scripts_ok) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "%s",
                         session->page.script_result.error[0] == '\0'
                           ? "document script pipeline failed"
                           : session->page.script_result.error);
                page_capability_trace_report(session);
                snprintf(session->last_script_error_context,
                         sizeof(session->last_script_error_context), "%s",
                         session->page.script_result.error_source_context);
                if (session->last_script_error_context[0] == '\0') {
                    capture_cached_script_error(
                        session, session->page.script_result.error);
                }
                /* Exhausting the separately bounded author realm should not
                   discard an otherwise usable parsed target. Retire the
                   poisoned realm and finish this candidate without further
                   script. Infrastructure failures and cancellation remain
                   transactional failures. */
                if (!navigation_retire_exhausted_script_realms(session)) {
                    goto fail;
                }
            }
            session->script_discovered = metrics.discovered;
            session->script_attempted = metrics.attempted;
            session->script_loaded = metrics.loaded;
            session->script_failed = metrics.failed;
            session->script_skipped_cross_origin =
                metrics.skipped_cross_origin;
            session->script_skipped_module = metrics.skipped_module;
            session->script_skipped_nomodule = metrics.skipped_nomodule;
            session->script_skipped_quota = metrics.skipped_quota;
            session->script_skipped_pressure = metrics.skipped_pressure;
            session->script_pressure_collections =
                metrics.pressure_collections;
            session->script_pressure_reclaimed_bytes =
                metrics.pressure_reclaimed_bytes;
            session->script_pressure_capped_requests =
                metrics.pressure_capped_requests;
            session->script_bytes = metrics.bytes;
            session->script_cache_hits = metrics.cache_hits;
            session->script_parser_blocking = metrics.parser_blocking;
            session->script_deferred = metrics.deferred;
            session->script_asynchronous = metrics.asynchronous;
            session->script_modules = metrics.modules;
            session->script_module_map_hits = metrics.module_map_hits;
            session->script_inline_data_fast_paths =
                metrics.inline_data_fast_paths;
            session->script_inline_data_fast_path_bytes =
                metrics.inline_data_fast_path_bytes;
            session->script_inline_data_quota_exemptions =
                metrics.inline_data_quota_exemptions;
            session->script_cost_class_rejections =
                metrics.cost_class_rejections;
            session->script_watchdog_classification_misses =
                metrics.watchdog_classification_misses;
            session->script_watchdog_classification_miss_bytes =
                metrics.watchdog_classification_miss_bytes;
            session->script_watchdog_classification_miss_loops =
                metrics.watchdog_classification_miss_loops;
            session->script_watchdog_classification_miss_flags =
                metrics.watchdog_classification_miss_flags;
            /* A rejected/throwing author script is diagnostic evidence, not
               a reason to discard the already parsed server-rendered page. */
            navigation_record_soft_script_error(session, &metrics);
        }
    }
    session->performance.script_us += navigation_record_script_slices(
        session, phase_started, &session->page.script_result,
        session->script_attempted
            + session->page.script_result.scripts_evaluated);
    if (!navigation_commit_checkpoint(
            session, "navigation-scripts",
            session->script_attempted
                + session->page.script_result.scripts_evaluated)) goto fail;
    if (script_document_may_have_changed) {
        phase_started = navigation_now_us();
        bool rebuild_stylesheet = true;
        if (adopted_streaming_stylesheet || adopted_compiled_stylesheet) {
            uint64_t fingerprint_started = navigation_now_us();
            uint64_t post_script_fingerprint = 0;
            bool fingerprinted = navigation_stream_stylesheet_fingerprint(
                streaming, &session->page.document,
                &post_script_fingerprint);
            uint64_t fingerprint_finished = navigation_now_us();
            session->performance.blocking_stylesheet_fingerprint_us +=
                fingerprint_finished >= fingerprint_started
                    ? fingerprint_finished - fingerprint_started : 0;
            rebuild_stylesheet = !fingerprinted
                || post_script_fingerprint
                   != adopted_stylesheet_fingerprint;
            if (!rebuild_stylesheet) {
                session->performance.blocking_stylesheet_final_reuses++;
            }
        }
        if (rebuild_stylesheet) {
            session->page.compiled_stylesheet_cacheable = false;
            stylesheet_destroy(&session->page.stylesheet);
            if (!navigation_build_stylesheet(session)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "post-script stylesheet construction failed");
                goto fail;
            }
            stylesheet_external_current = false;
            user_stylesheet_current = false;
        }
        if (session->page.runtime != NULL) {
            script_runtime_set_stylesheet(session->page.runtime,
                                          &session->page.stylesheet);
        }
        /* Everything before this boundary is reflected by the rebuilt
           stylesheet.  Keep mutations made by subsequent stylesheet load
           handlers pending so a handler which changes href or inserts
           another resource cannot be discarded at commit. */
        if (session->page.runtime != NULL) {
            (void) script_runtime_consume_mutations(
                session->page.runtime, NULL);
        }
        session->performance.style_us += navigation_record_slice(
            session, NAVIGATION_SLICE_STYLE, phase_started,
            session->page.document.node_count);
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-post-script-style",
            session->page.document.node_count)) goto fail;
    phase_started = navigation_now_us();
    bool progressive_preview_candidate =
        stylesheet_web_font_set(&session->page.stylesheet) != NULL;
    if (session->external_resources_enabled) {
        if (session->page.runtime != NULL) {
            (void) script_runtime_collect_and_trim(session->page.runtime);
        }
        if (!stylesheet_external_current
            && !stylesheets_load_external_tracked_with_context(
                &session->page.document, &session->page.stylesheet,
                session->budget, session->page.resource_base_url, url,
                session->page.referrer_policy,
                session->maximum_stylesheets,
                session->maximum_stylesheet_bytes,
                session->maximum_stylesheet_file_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session,
                &session->page.stylesheet_resources,
                &session->page.external_stylesheets)) {
            if (!navigation_install_usable_stylesheet_fallback(session)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "stylesheet fallback exceeded its budget");
                goto fail;
            }
            stylesheet_external_current = true;
            user_stylesheet_current = false;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
        if (!navigation_dispatch_stylesheet_events(
                session, session->page.resource_base_url)
            && !navigation_retire_exhausted_script_realms(session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "stylesheet completion event dispatch failed");
            goto fail;
        }
        uint64_t stylesheet_finished = navigation_now_us();
        session->performance.stylesheet_resource_us +=
            stylesheet_finished >= phase_started
                ? stylesheet_finished - phase_started : 0;
        session->performance.resource_us +=
            navigation_record_resource_slices(
                session, phase_started, &session->page.external_stylesheets,
                NULL);
    }
    phase_started = navigation_now_us();
    if (session->user_css_length != 0 && !user_stylesheet_current
        && !stylesheet_add_user_css(&session->page.stylesheet,
                                    session->user_css,
                                    session->user_css_length)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "user stylesheet exceeded its budget");
        goto fail;
    }
    session->performance.style_us += navigation_record_slice(
        session, NAVIGATION_SLICE_STYLE, phase_started,
        session->page.stylesheet.count);
    /* A bounded script pass can leave standards-based custom-element shells
       unresolved even though their light DOM is a perfectly usable static
       fallback.  Many production component libraries deliberately hide
       :not(:defined) hosts while waiting for their runtime.  Once this
       navigation has skipped or failed author script, that wait cannot be
       satisfied reliably; reveal readable light DOM while retaining compact
       author fallbacks for navigation chrome. Successful script pipelines
       keep ordinary custom-element semantics. */
    bool static_script_fallback =
        (streaming != NULL && streaming->script_disabled_after_failure)
        || session->script_failed != 0
        || session->script_skipped_quota != 0
        || session->script_skipped_pressure != 0
        || session->script_cost_class_rejections != 0;
    if (static_script_fallback) {
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
        (void) document_install_static_shell_fallback(
            &session->page.document);
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-final-style",
            session->page.stylesheet.count)) goto fail;
    /* Cache only source-derived sheets. Author script CSSOM mutations can
       compile rules that have no representation in the DOM fingerprint;
       replaying those on another document would be incorrect. */
    session->page.compiled_stylesheet_cacheable = false;
    if (streaming != NULL && session->script_attempted == 0) {
        uint64_t final_fingerprint = 0;
        if (navigation_stream_stylesheet_fingerprint(
                streaming, &session->page.document,
                &final_fingerprint)) {
            session->page.compiled_stylesheet_fingerprint =
                final_fingerprint;
            session->page.compiled_stylesheet_resource_signature =
                browser_session_stylesheet_cache_signature(
                    session->browser_session,
                    tilefinch_platform_monotonic_time_ns());
            session->page.compiled_stylesheet_build_generation =
                session->page.stylesheet.build_generation;
            session->page.compiled_stylesheet_cacheable =
                session->page.compiled_stylesheet_resource_signature != 0
                && session->page.external_stylesheets.skipped_pressure == 0
                && session->performance.stylesheet_pressure_fallbacks == 0;
        }
    }
    if (session->web_fonts_enabled) {
        phase_started = navigation_now_us();
        if (!navigation_load_web_fonts(session, url)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "external font pipeline could not start within its budget");
            goto fail;
        }
        size_t font_work = session->page.external_fonts.sources_discovered;
        if (session->page.external_fonts.attempted
                <= SIZE_MAX - font_work) {
            font_work += session->page.external_fonts.attempted;
        } else {
            font_work = SIZE_MAX;
        }
        uint64_t font_elapsed = navigation_record_slice(
            session, NAVIGATION_SLICE_RESOURCE, phase_started, font_work);
        session->performance.resource_us += font_elapsed;
        session->performance.font_resource_us += font_elapsed;
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-fonts",
            session->page.external_fonts.attempted)) goto fail;

    ImagePriorityTarget image_priority_targets[
        NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
    size_t image_priority_count = 0;
    bool static_image_plan = false;
    bool pre_image_preview_candidate =
        session->external_resources_enabled
        && navigation_document_may_have_images(
            &session->page.document, &session->page.stylesheet);
    bool final_layout_ready = false;
    if (pre_image_preview_candidate) {
        (void) navigation_try_progressive_preview(
            session, streaming, previous_page, true, fonts, images, false,
            &session->page.layout, image_priority_targets,
            NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT, &image_priority_count);
        session->performance.progressive_image_priority_nodes =
            image_priority_count;
        /* Browser-authored static documents are deliberately small and have
           no author script capable of invalidating this probe. Build one
           bounded no-image layout to obtain exact boxes; ordinary web pages
           retain the streaming-preview/markup policy above. */
        if (image_priority_count == 0 && !allow_runtime
            && streaming == NULL) {
            static_image_plan = navigation_plan_static_image_geometry(
                session, fonts, images, image_priority_targets,
                NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT,
                &image_priority_count);
            session->performance.progressive_image_priority_nodes =
                image_priority_count;
        }
    }
    size_t markup_priority_capacity = NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT;
    if (session->maximum_images < markup_priority_capacity) {
        markup_priority_capacity = session->maximum_images;
    }
    size_t svg_priority_capacity = markup_priority_capacity;
    /* The markup fallback is intentionally less authoritative than a
       successful viewport layout. Leave a small quota for the complete
       style-aware traversal so late responsive icons, CSS backgrounds, and
       masks cannot be crowded out by eager offscreen <img> elements. */
    if (markup_priority_capacity >= 8u) {
        size_t reserve = markup_priority_capacity / 4u;
        if (reserve > 8u) reserve = 8u;
        markup_priority_capacity -= reserve;
    }
    size_t layout_priority_count = image_priority_count;
    size_t markup_priority_added = pre_image_preview_candidate
        ? navigation_collect_markup_svg_priority(
              &session->page.document, image_priority_targets,
              svg_priority_capacity, image_priority_count)
        : 0;
    image_priority_count += markup_priority_added;
    if (pre_image_preview_candidate && layout_priority_count == 0
        && !static_image_plan) {
        size_t raster_added = navigation_collect_markup_image_priority(
            &session->page.document, image_priority_targets,
            markup_priority_capacity, image_priority_count);
        image_priority_count += raster_added;
        markup_priority_added += raster_added;
    }
    session->performance.markup_image_priority_nodes =
        markup_priority_added;

    if (session->external_resources_enabled) {
        phase_started = navigation_now_us();
        size_t loaded_before = session->page.images.stats.loaded;
        if (image_priority_count != 0
            && !images_load_external_priority_targets(
                &session->page.document, &session->page.stylesheet,
                &session->page.images, image_priority_targets,
                image_priority_count, session->budget,
                session->page.resource_base_url, url,
                session->page.referrer_policy, session->maximum_images,
                session->maximum_image_bytes,
                session->maximum_image_file_bytes,
                session->maximum_decoded_image_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "visible image priority pipeline exceeded its budget");
            goto fail;
        }
        if (session->page.images.stats.loaded >= loaded_before) {
            session->performance.progressive_image_priority_loaded =
                session->page.images.stats.loaded - loaded_before;
        }
        if (image_priority_count != 0) {
            images = &session->page.images;
            layout_reuse_cache_update_images(
                session->page.layout_reuse, images);
            size_t remaining_priority_count = 0;
            final_layout_ready = navigation_try_progressive_preview(
                session, streaming, previous_page, true, fonts, images,
                true, &session->page.layout,
                image_priority_targets,
                NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT,
                &remaining_priority_count);
        }
        bool continue_images_after_paint =
            session->performance.partial_paints != 0
            && navigation_background_images_enabled();
        bool defer_static_document_images = static_image_plan
            && session->page.deferred_image_count != 0
            && !allow_runtime && streaming == NULL
            && navigation_document_images_are_defer_safe(
                   &session->page.document, &session->page.stylesheet);
        if (continue_images_after_paint || defer_static_document_images) {
            session->page.image_continuation_pending = true;
        } else {
            budget_free(session->budget,
                        session->page.deferred_image_targets);
            session->page.deferred_image_targets = NULL;
            session->page.deferred_image_count = 0;
            navigation_ensure_layout_reuse(session);
            size_t retained_before = session->page.images.stats
                .priority_retained_on_failure;
            if (!images_load_external_reusing_layout_styles(
                    &session->page.document, &session->page.stylesheet,
                    &session->page.images, session->budget,
                    session->page.resource_base_url, url,
                    session->page.referrer_policy,
                    session->maximum_images, session->maximum_image_bytes,
                    session->maximum_image_file_bytes,
                    session->maximum_decoded_image_bytes,
                    session->resource_timeout_ms,
                    session->page.resource_scheduler,
                    session->browser_session, session->page.layout_reuse,
                    fonts, session->viewport.css_width)) {
                if (session->page.images.stats.priority_retained_on_failure
                        > retained_before
                    && session->page.images.count != 0) {
                    session->performance.image_pressure_fallbacks++;
                    session->page.image_continuation_pending = false;
                } else {
                    snprintf(session->last_error,
                             sizeof(session->last_error),
                             "external image pipeline exceeded its budget");
                    goto fail;
                }
            }
        }
        images = &session->page.images;
        layout_reuse_cache_update_images(
            session->page.layout_reuse, images);
        progressive_preview_candidate = session->page.images.count != 0
            || session->page.stylesheet.image_url_count != 0
            || progressive_preview_candidate;
        uint64_t image_finished = navigation_now_us();
        session->performance.image_resource_us +=
            image_finished >= phase_started
                ? image_finished - phase_started : 0;
        session->performance.resource_us +=
            navigation_record_resource_slices(
                session, phase_started, NULL, &session->page.images.stats);
    }
    if (!navigation_commit_checkpoint(
            session, "navigation-images",
            session->page.images.stats.work_units)) goto fail;
    if (!final_layout_ready && session->performance.partial_paints == 0) {
        final_layout_ready = navigation_try_progressive_preview(
            session, streaming, previous_page,
            progressive_preview_candidate, fonts, images, true,
            &session->page.layout, NULL, 0, NULL);
    }
    if (!final_layout_ready) {
        phase_started = navigation_now_us();
        if (defer_authoritative_layout && previous_page == NULL
            && !record_history && candidate_prepare == NULL
            && deferred_layout_job != NULL
            && session->page.document.node_count >= 256) {
            navigation_ensure_layout_reuse(session);
            LayoutBuildJob *job = layout_build_job_begin(
                session->budget, &session->page.document,
                &session->page.stylesheet, fonts, images,
                &session->viewport, session->page.layout_reuse);
            if (job == NULL) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "resumable layout startup failed");
                goto fail;
            }
            *deferred_layout_job = job;
            if (deferred_preview_candidate != NULL) {
                *deferred_preview_candidate =
                    progressive_preview_candidate;
            }
            return true;
        }
        if (!navigation_build_layout(session, &session->page.layout,
                                     fonts, images)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "layout construction failed");
            goto fail;
        }
        uint64_t final_layout_elapsed = navigation_record_layout_slices(
            session, phase_started, &session->page.layout);
        session->performance.layout_us += final_layout_elapsed;
        session->performance.final_layout_us += final_layout_elapsed;
    }
    if (!navigation_finish_authoritative_layout(
            session, url, streaming, previous_page,
            progressive_preview_candidate, fonts, images)) goto fail;
    if (candidate_prepare != NULL) {
        lxb_html_document_t *candidate_document = session->page.document.html;
        ScriptRuntime *candidate_runtime = session->page.runtime;
        DrawCommand *candidate_commands = session->page.layout.commands;
        FetchScheduler *candidate_scheduler =
            session->page.resource_scheduler;
        FetchSchedulerDomain *candidate_domain =
            session->page.fetch_domain;
        candidate_hook_attempted = true;
        if (!candidate_prepare(candidate_opaque, session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "candidate commit preparation failed");
            goto fail;
        }
        if (generation != session->generation || session->cancelled
            || !session->page.loaded
            || session->page.document.html != candidate_document
            || session->page.runtime != candidate_runtime
            || session->page.layout.commands != candidate_commands
            || session->page.resource_scheduler != candidate_scheduler
            || session->page.fetch_domain != candidate_domain) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "candidate changed during commit preparation");
            goto fail;
        }
    }
    if (record_history
        && !history_push(session, url, session->page.document.title)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "history entry allocation failed");
        goto fail;
    }
    if (isolates_remote_document) {
        /* The streamed candidate was deliberately built without the
           incumbent page's sectional backing.  Clear the session-level
           copy only at this no-fail commit point: earlier cancellation or
           commit failure must leave both the incumbent runtime and its
           binding usable.  Clearing an existing top-level runtime with an
           empty binding is non-allocating and cannot fail. */
        (void) navigation_rebind_top_level_remote_document(session, NULL);
    }
    if (candidate_hook_attempted) {
        candidate_commit(candidate_opaque);
    }
    navigation_compiled_stylesheet_cache_store(session, previous_page);
    page_destroy_value(session, previous_page);
    budget_free(session->budget, previous_page);
    session->replacement_failure_overlay_active = false;
    session->replacement_failure_message[0] = '\0';
    session->loads_committed++;
    return true;

fail:
    if (candidate_hook_attempted) {
        candidate_abort(candidate_opaque);
    }
    bool rollback_progressive_frame =
        session->performance.partial_paints != 0
        && session->progressive_paint != NULL;
    page_destroy(session);
    if (previous_page != NULL) session->page = *previous_page;
    budget_free(session->budget, previous_page);
    session->viewport = previous_viewport;
    session->fonts = previous_fonts;
    session->images = previous_images;
    session->resource_fingerprint = previous_resource_fingerprint;
    session->style_rebuild_required = previous_style_rebuild_required;
    session->relayout_damage_valid = previous_relayout_damage_valid;
    session->relayout_damage_left = previous_relayout_damage_left;
    session->relayout_damage_top = previous_relayout_damage_top;
    session->relayout_damage_right = previous_relayout_damage_right;
    session->relayout_damage_bottom = previous_relayout_damage_bottom;
    if (rollback_progressive_frame) {
        (void) session->progressive_paint(
            session->progressive_paint_opaque, session, NULL);
        session->performance.progressive_paint_rollbacks++;
    }
    return false;
}
