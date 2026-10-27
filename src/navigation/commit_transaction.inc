#define NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT 128u

static void navigation_record_compiled_stylesheet(
    NavigationSession *session, uint64_t fingerprint)
{
    session->page.compiled_stylesheet_fingerprint = fingerprint;
    session->page.compiled_stylesheet_resource_signature =
        browser_session_stylesheet_cache_signature(
            session->browser_session, tilefinch_platform_monotonic_time_ns());
    session->page.compiled_stylesheet_build_generation =
        session->page.stylesheet.build_generation;
    session->page.compiled_stylesheet_cacheable =
        session->page.compiled_stylesheet_resource_signature != 0
        && session->page.external_stylesheets.skipped_pressure == 0
        && session->performance.stylesheet_pressure_fallbacks == 0;
}

static bool navigation_replace_server_body_with_runtime(
    void *opaque, PocDocument *document,
    const char *markup, size_t length)
{
    return script_runtime_replace_document_body(
        opaque, document, markup, length);
}

static bool navigation_restore_server_body_if_degraded(
    NavigationSession *session, const DocumentBodySnapshot *snapshot)
{
    if (session == NULL || snapshot == NULL) return false;
    ScriptRuntime *runtime = session->page.runtime;
    return document_body_snapshot_restore_if_degraded(
        &session->page.document, snapshot,
        runtime == NULL ? NULL : navigation_replace_server_body_with_runtime,
        runtime);
}

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

/* A large page without a script realm commits with only the screens around
   the top laid out and without the whole-document image walk (together
   ~20 s of the PlayStation Portable article on the PSP): the reader can
   scroll, follow links and extend the layout while idle work completes it
   and the deferred queue loads its images nearest-first. Nothing else
   reads its geometry in between. */
#define NAVIGATION_PROVISIONAL_COMMIT_NODES 3000u

static void navigation_queue_discovered_visuals(NavigationSession *session);

static bool navigation_provisional_commit_eligible(
    const NavigationSession *session,
    const NavigationDocumentStream *streaming)
{
    return streaming != NULL && session->page.runtime == NULL
        && session->relayout_preview_threshold_us != UINT64_MAX
        && session->page.document.node_count
               >= NAVIGATION_PROVISIONAL_COMMIT_NODES
        && session->viewport.css_height > 0;
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

#define NAVIGATION_MARKUP_IMAGE_ANCESTOR_LIMIT 64u

/* Computed styles of the most recently resolved root-to-image element
   chain. Sibling images share most of their ancestors, so each candidate
   resolves only the part of its chain that differs from the previous one. */
typedef struct {
    const Stylesheet *sheet;
    ComputedStyle *styles;
    lxb_dom_node_t *nodes[NAVIGATION_MARKUP_IMAGE_ANCESTOR_LIMIT];
    size_t depth;
    size_t first_hidden;
} NavigationMarkupImageStyles;

/* The style-aware image traversal never reaches an element below a
   display:none or engine-hidden ancestor (including the element itself):
   such an image is never painted. Returns false only when the resolved
   chain proves that; an unavailable chain keeps the eager markup policy. */
static bool navigation_markup_image_rendered(
    NavigationMarkupImageStyles *memo, lxb_dom_node_t *node)
{
    if (memo == NULL || memo->sheet == NULL || memo->styles == NULL) {
        return true;
    }
    lxb_dom_node_t *chain[NAVIGATION_MARKUP_IMAGE_ANCESTOR_LIMIT];
    size_t depth = 0;
    for (lxb_dom_node_t *at = node; at != NULL; at = at->parent) {
        if (at->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (depth == NAVIGATION_MARKUP_IMAGE_ANCESTOR_LIMIT) return true;
        chain[depth++] = at;
    }
    /* chain[] runs image-to-root; the memo runs root-to-image. */
    size_t shared = 0;
    while (shared < depth && shared < memo->depth
           && memo->nodes[shared] == chain[depth - 1u - shared]) shared++;
    if (memo->first_hidden < shared) return false;
    memo->depth = shared;
    memo->first_hidden = SIZE_MAX;
    for (size_t level = shared; level < depth; level++) {
        lxb_dom_node_t *element = chain[depth - 1u - level];
        memo->styles[level] = style_for_node(
            memo->sheet, element,
            level == 0 ? NULL : &memo->styles[level - 1u]);
        memo->nodes[level] = element;
        memo->depth = level + 1u;
        if (memo->styles[level].display == DISPLAY_NONE
            || memo->styles[level].hidden) {
            memo->first_hidden = level;
            return false;
        }
    }
    return true;
}

/* A speculative layout can be refused precisely when priority matters most.
   Preserve a useful bounded order without layout allocation: explicit
   fetchpriority=high images first, then eager markup images in DOM order.
   Lazy images remain for the document-wide continuation, and images below
   hidden markup are left to it too (it never loads them). Resolving their
   bounded ancestor chains is optional: under allocation refusal every eager
   markup image stays eligible, as before. */
static size_t navigation_collect_markup_image_priority(
    Budget *budget, const Stylesheet *sheet, const PocDocument *document,
    ImagePriorityTarget *targets, size_t capacity, size_t existing_count)
{
    if (document == NULL || document->html == NULL || targets == NULL
        || existing_count >= capacity) return 0;
    NavigationMarkupImageStyles memo = {
        .sheet = sheet,
        .first_hidden = SIZE_MAX
    };
    if (budget != NULL && sheet != NULL) {
        memo.styles = budget_malloc_category(
            budget, BUDGET_CATEGORY_STYLE,
            NAVIGATION_MARKUP_IMAGE_ANCESTOR_LIMIT * sizeof(*memo.styles));
    }
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
                if (((pass == 0 && high) || (pass == 1 && !high && !lazy))
                    && navigation_markup_image_rendered(&memo, node)) {
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
    if (memo.styles != NULL) budget_free(budget, memo.styles);
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
    /* Symbols and definitions draw only through <use>, so a sprite sheet
       does not take one of the bounded early-icon slots. */
    return image_inline_svg_draws_content(svg, 64u, NULL);
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
    /* Keep visible eager boxes first, followed by below-fold eager boxes and
       finally explicit lazy boxes in layout order.  The caller loads only the
       first prefix before paint, while retaining the complete list as its
       geometry-aware idle queue.  A visible loading=lazy image therefore
       reserves the right box without silently becoming paint-blocking again. */
    for (unsigned pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < layout->node_box_count; i++) {
            const LayoutNodeBox *box = &layout->node_boxes[i];
            if (box->node == NULL || !node_name_is(box->node, "img")
                || box->width <= 0 || box->height <= 0) continue;
            bool visible = box->y < viewport_height
                && (int64_t) box->y + box->height > 0;
            bool lazy = navigation_attribute_equals(
                box->node, "loading", "lazy");
            if ((pass == 0 && (!visible || lazy))
                || (pass == 1 && (visible || lazy))
                || (pass == 2 && !lazy)) continue;
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
    const PocDocument *document, const Stylesheet *stylesheet,
    const ImageResources *images)
{
    if (document == NULL || document->html == NULL || stylesheet == NULL
        || stylesheet->image_url_count != 0) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t visited = 0;
    for (lxb_dom_node_t *node = root; node != NULL
         && visited++ < NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT;) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            if (node_name_is(node, "svg")) {
                /* Already-published inline artwork needs no network or
                   discovery in the eager pass. It must not force unrelated
                   lazy <img> requests onto the commit path. An unprocessed
                   or refused SVG keeps the existing conservative fallback. */
                const ImageResource *image = images_find_node(images, node);
                if (image == NULL || image->pixels == NULL) return false;
            }
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
    size_t capacity, size_t *count, bool *layout_ready)
{
    if (count != NULL) *count = 0;
    if (layout_ready != NULL) *layout_ready = false;
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
    navigation_layout_reuse_sample(session->page.layout_reuse, &reuse_before);
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
    navigation_layout_reuse_sample(session->page.layout_reuse, &reuse_after);
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
    /* With no eager visible images and no other image-producing content,
       the complete plan is already the final no-image layout. The caller
       defers this entire image queue and cannot run author script. Move the
       layout into the candidate page so its ordinary abort/commit owner
       handles every later failure; never retain a second layout allocation.
       Nonempty external image tables keep the existing rebuild path. */
    if (collected && total_count != 0 && *count == 0
        && layout_ready != NULL && session->page.layout.commands == NULL
        && session->page.images.count == 0
        && (images == NULL || images->count == 0)
        && navigation_document_images_are_defer_safe(
               &session->page.document, &session->page.stylesheet, images)) {
        session->page.layout = probe;
        memset(&probe, 0, sizeof(probe));
        *layout_ready = true;
        session->performance.static_image_layout_adoptions++;
        session->performance.final_layout_us += elapsed;
    }
    layout_destroy(&probe);
    if (!collected) *count = 0;
    return collected;
}

/* Network documents can commit useful layout before their ordinary images.
   Do not keep a DOM traversal cursor across author turns. The entire small
   plan is bounded and every mutable target carries a native weak handle;
   CSS images, posters and inline SVG retain the authoritative loader path. */
static bool navigation_plan_mutable_document_images(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->runtime == NULL || page->deferred_image_targets != NULL
        || page->document.html == NULL) return false;
    ImagePriorityTarget *targets = budget_calloc_category(
        session->budget, BUDGET_CATEGORY_RESOURCE,
        NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT, sizeof(*targets));
    if (targets == NULL) return false;
    lxb_dom_node_t *root = lxb_dom_interface_node(page->document.html);
    size_t visited = 0, count = 0;
    for (lxb_dom_node_t *node = root; node != NULL;) {
        if (++visited > NAVIGATION_DOM_TRAVERSAL_NODE_LIMIT) goto decline;
        if (node_name_is(node, "img")) {
            if (count == NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT) goto decline;
            long handle = script_runtime_node_weak_handle(page->runtime, node);
            if (handle == 0) goto decline;
            targets[count++] = (ImagePriorityTarget) {
                .node = node, .weak_handle = handle,
                .kind = IMAGE_PRIORITY_KIND_DOCUMENT
            };
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    if (count == 0) goto decline;
    page->deferred_image_targets = targets;
    page->deferred_image_count = count;
    return true;
decline:
    budget_free(session->budget, targets);
    return false;
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
    navigation_layout_reuse_sample(session->page.layout_reuse, &reuse_before);
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
    navigation_layout_reuse_sample(session->page.layout_reuse, &reuse_after);
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

static bool navigation_document_has_author_runtime_content(
    const PocDocument *document)
{
    if (document == NULL || document->html == NULL) return false;
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
            bool executable = script_type_attribute_classify(
                type, type_length, NULL);
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

static bool navigation_document_requires_runtime(
    const NavigationSession *session, const PocDocument *document)
{
    if (session == NULL || document == NULL || document->html == NULL) {
        return true;
    }
    return NAVIGATION_TRACES_PAGE_CAPABILITIES(session)
        || navigation_document_has_author_runtime_content(document);
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
        && session->page.script_degradation_observed
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
    /* Publish the exact resource/font owners used by the committed layout
       before attaching its runtime. Canvas dirty snapshots queued by inline
       scripts can then join the page resource table during that attach. */
    session->fonts = fonts;
    session->images = images;
    /* A complete committed layout's cost predicts in-place relayouts
       (navigation_attach_layout records later ones); a provisional commit
       was chosen because the page is slow, and later relayouts are too. */
    session->page.layout_provisional =
        session->page.layout.content_limit_y != 0;
    session->page.layout_complete_us = session->page.layout_provisional
        ? session->relayout_preview_threshold_us
        : session->page.layout.performance.total_us;
    if (!session->page.layout_provisional
        && session->page.visual_discovery_pending)
        navigation_queue_discovered_visuals(session);
    bool attach_relayout_requested = false;
    if (session->page.runtime != NULL) {
        navigation_attach_layout(session);
        /* Resource completion handlers run after the resource pass. A
           preload handler promoting rel to stylesheet (or changing href)
           still needs another pass even though this layout covers its DOM. */
        attach_relayout_requested =
            script_runtime_has_pending_resource_mutations(session->page.runtime)
            || script_runtime_consume_relayout(session->page.runtime);
    } else {
        /* Font admission follows the committed layout, not the existence
           of an author runtime. Static pages need the same demand filter. */
        navigation_update_font_usage(session);
    }
    session->page.loaded = true;
    script_runtime_heavy_commit(session->page.runtime);
    script_runtime_arm_memory_rescue(session->page.runtime);
    /* Runtime attachment publishes canvas surfaces queued by parser-time
       scripts.  A newly created or resized surface is a structural layout
       mutation: the authoritative layout built immediately above could only
       reserve the fallback canvas box and does not yet contain DRAW_IMAGE.
       Honor that exact pending mutation before exposing the committed page.
       Same-size pixel updates retain the paint-only damage fast path. */
    bool missing_canvas_surface = false;
    if (attach_relayout_requested) {
        for (size_t image_at = 0;
             image_at < session->page.images.count; image_at++) {
            const ImageResource *image =
                &session->page.images.items[image_at];
            if (!image->is_canvas || !image_resource_available(image)
                || layout_box_for_node(
                       &session->page.layout, image->node) == NULL) {
                continue;
            }
            bool painted = false;
            for (size_t command_at = 0;
                 command_at < session->page.layout.count; command_at++) {
                const DrawCommand *command =
                    &session->page.layout.commands[command_at];
                if (command->type == DRAW_IMAGE
                    && command->image == image) {
                    painted = true;
                    break;
                }
            }
            if (!painted) {
                missing_canvas_surface = true;
                break;
            }
        }
    }
    if (missing_canvas_surface && !navigation_relayout(session)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "runtime-attachment relayout failed");
        return false;
    }
    uint64_t phase_started = navigation_now_us();
    if (!navigation_resource_fingerprint(
            session, &session->page.document,
            &session->resource_fingerprint)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "resource fingerprint traversal failed");
        return false;
    }
    navigation_note_committed_fingerprint(session);
    uint64_t fingerprint_finished = navigation_now_us();
    session->performance.resource_fingerprint_us +=
        fingerprint_finished >= phase_started
            ? fingerprint_finished - phase_started : 0;
    session->style_rebuild_required = false;
    if (session->document_scripts_enabled
        && session->page.runtime != NULL
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

/* The session state a commit replaces besides the page itself, which a
   refused commit restores. The page moves separately: the incumbent is
   parked in a Budget-owned copy while the candidate is built in place.
   Commit-time state that lives outside NavigationPage belongs here, so a
   new field is captured and restored in one place. */
typedef struct {
    ViewportContext viewport;
    const FontSet *fonts;
    const ImageResources *images;
    uint64_t resource_fingerprint;
    bool style_rebuild_required;
    bool relayout_damage_valid;
    int relayout_damage_left;
    int relayout_damage_top;
    int relayout_damage_right;
    int relayout_damage_bottom;
    bool canvas_paint_damage_pending;
} NavigationCommitSnapshot;

static void commit_snapshot_capture(const NavigationSession *session,
                                    NavigationCommitSnapshot *snapshot)
{
    *snapshot = (NavigationCommitSnapshot) {
        .viewport = session->viewport,
        .fonts = session->fonts,
        .images = session->images,
        .resource_fingerprint = session->resource_fingerprint,
        .style_rebuild_required = session->style_rebuild_required,
        .relayout_damage_valid = session->relayout_damage_valid,
        .relayout_damage_left = session->relayout_damage_left,
        .relayout_damage_top = session->relayout_damage_top,
        .relayout_damage_right = session->relayout_damage_right,
        .relayout_damage_bottom = session->relayout_damage_bottom,
        .canvas_paint_damage_pending = session->canvas_paint_damage_pending
    };
}

static void commit_snapshot_restore(NavigationSession *session,
                                    const NavigationCommitSnapshot *snapshot)
{
    session->viewport = snapshot->viewport;
    session->fonts = snapshot->fonts;
    session->images = snapshot->images;
    session->resource_fingerprint = snapshot->resource_fingerprint;
    session->style_rebuild_required = snapshot->style_rebuild_required;
    session->relayout_damage_valid = snapshot->relayout_damage_valid;
    session->relayout_damage_left = snapshot->relayout_damage_left;
    session->relayout_damage_top = snapshot->relayout_damage_top;
    session->relayout_damage_right = snapshot->relayout_damage_right;
    session->relayout_damage_bottom = snapshot->relayout_damage_bottom;
    session->canvas_paint_damage_pending =
        snapshot->canvas_paint_damage_pending;
}

/* The transaction state navigation_commit_document carries across its
   phases: the parked incumbent and the session snapshot that roll back on
   failure, the candidate hooks captured at entry, and the facts one phase
   establishes for a later one. */
typedef struct {
    /* The commit's inputs. images moves to the page's own table once the
       images phase has loaded it. */
    uint64_t generation;
    const char *url;
    PocDocument *document;
    int viewport_width;
    const FontSet *fonts;
    const ImageResources *images;
    bool record_history;
    NavigationDocumentStream *streaming;
    bool allow_runtime;
    bool defer_authoritative_layout;
    LayoutBuildJob **deferred_layout_job;
    bool *deferred_preview_candidate;
    FetchSchedulerDomain *candidate_fetch_domain;
    bool fetch_domain_from_stream;
    /* Transaction state. */
    NavigationPage *previous_page;
    NavigationCommitSnapshot snapshot;
    bool replace_history;
    ReaderDocumentAnalysis reader_analysis;
    NavigationCandidatePrepareCallback candidate_prepare;
    NavigationCandidatePrepareCallback candidate_graph_prepare;
    NavigationCandidateFinalizeCallback candidate_abort;
    NavigationCandidateFinalizeCallback candidate_commit;
    void *candidate_opaque;
    bool candidate_hook_attempted;
    char resource_base_url[NAVIGATION_URL_LIMIT];
    bool streamed_runtime;
    bool optional_work_shed;
    bool adopted_streaming_stylesheet;
    bool adopted_compiled_stylesheet;
    bool stylesheet_external_current;
    bool user_stylesheet_current;
    bool script_document_may_have_changed;
    uint64_t adopted_stylesheet_fingerprint;
    bool progressive_preview_candidate;
    bool final_layout_ready;
    bool provisional_commit;
} NavigationCommitTxn;

/* Setup: move the candidate document, the streamed realm, canvases,
   stylesheet events and scheduler into the emptied page, then resolve its
   viewport and start its resource scheduler. */
static TILEFINCH_OUT_OF_LINE bool commit_setup(NavigationSession *session,
                                               NavigationCommitTxn *txn)
{
    char response_referrer_policy[
        sizeof(session->pending_response_referrer_policy)];
    snprintf(response_referrer_policy, sizeof(response_referrer_policy),
             "%s", session->pending_response_referrer_policy);
    session->pending_response_referrer_policy[0] = '\0';
    session->canvas_paint_damage_pending = false;
    memset(&session->page, 0, sizeof(session->page));
    session->committed_scroll_reflection_pending = false;
    if (txn->streaming != NULL) {
        session->page.images = txn->streaming->canvas_images;
        memset(&txn->streaming->canvas_images, 0,
               sizeof(txn->streaming->canvas_images));
    }
    session->page.fetch_domain = txn->candidate_fetch_domain;
    if (txn->fetch_domain_from_stream) txn->streaming->fetch_domain = NULL;
    snprintf(session->page.referrer_policy,
             sizeof(session->page.referrer_policy), "%s",
             response_referrer_policy);
    session->page.document = *txn->document;
    if (session->prepare_reader_candidates)
        session->page.reader_analysis = txn->reader_analysis;
    memset(txn->document, 0, sizeof(*txn->document));
    snprintf(session->page.resource_base_url,
             sizeof(session->page.resource_base_url), "%s",
             txn->resource_base_url);
    snprintf(session->page.document_url,
             sizeof(session->page.document_url), "%s", txn->url);
    navigation_refresh_adopt_header(session, txn->url);
    if (txn->streaming != NULL
        && txn->streaming->stylesheet_event_baseline_adopted) {
        memcpy(session->page.stylesheet_events,
               txn->streaming->stylesheet_events,
               txn->streaming->stylesheet_event_count
                   * sizeof(*session->page.stylesheet_events));
        session->page.stylesheet_event_count =
            txn->streaming->stylesheet_event_count;
    }
    if (txn->streaming != NULL) {
        session->page.stylesheet_resources =
            txn->streaming->stylesheet_resources;
        memset(&txn->streaming->stylesheet_resources, 0,
               sizeof(txn->streaming->stylesheet_resources));
    }
    txn->streamed_runtime = txn->streaming != NULL && txn->streaming->runtime != NULL;
    if (txn->streamed_runtime) {
        script_runtime_set_synchronous_layout_callback(
            txn->streaming->runtime, NULL, NULL);
        script_runtime_set_layout(
            txn->streaming->runtime, NULL, txn->streaming->viewport.css_height);
        if (txn->streaming->cssom_layout_ready) {
            layout_destroy(&txn->streaming->cssom_layout);
            memset(&txn->streaming->cssom_layout, 0,
                   sizeof(txn->streaming->cssom_layout));
            txn->streaming->cssom_layout_ready = false;
        }
        session->page.runtime = txn->streaming->runtime;
        /* The parser-owned PocDocument struct moved into the committed
           page, but its Lexbor tree and JavaScript realm did not change.
           Preserve handles and script identity across that storage move. */
        script_runtime_relocate_document_storage(
            session->page.runtime, &session->page.document);
        session->page.script_result = txn->streaming->script_result;
        txn->streaming->runtime_transferred = true;
        /* canvas_images moved from streaming storage into page storage before
           this realm was adopted. Rebind before any remaining deferred script
           runs so a late drawing cannot publish into the emptied staging
           table. */
        script_runtime_set_images(
            session->page.runtime, &session->page.images);
    } else if (txn->streaming != NULL
               && txn->streaming->script_disabled_after_failure) {
        session->page.script_result = txn->streaming->script_result;
        session->script_discovered = txn->streaming->scripts.early.discovered;
        session->script_attempted = txn->streaming->scripts.early.attempted;
        session->script_loaded = txn->streaming->scripts.early.loaded;
        session->script_failed = txn->streaming->scripts.early.failed;
        session->script_skipped_quota =
            txn->streaming->scripts.early.skipped_quota;
        session->script_skipped_pressure =
            txn->streaming->scripts.early.skipped_pressure;
        session->script_inline_data_quota_exemptions =
            txn->streaming->scripts.early.inline_data_quota_exemptions;
        session->script_cost_class_rejections =
            txn->streaming->scripts.early.cost_class_rejections;
        session->script_watchdog_classification_misses =
            txn->streaming->scripts.early.watchdog_classification_misses;
        session->script_watchdog_classification_miss_bytes =
            txn->streaming->scripts.early.watchdog_classification_miss_bytes;
        session->script_watchdog_classification_miss_loops =
            txn->streaming->scripts.early.watchdog_classification_miss_loops;
        session->script_watchdog_classification_miss_flags =
            txn->streaming->scripts.early.watchdog_classification_miss_flags;
    }
    if (txn->streaming != NULL) {
        const ExternalScriptMetrics *early = &txn->streaming->scripts.early;
        if (early->skipped_pressure != 0u)
            session->page.script_memory_limited = true;
        bool shed_author_script_work =
            txn->streaming->script_disabled_after_failure
            && session->scripts_enabled
            && session->document_scripts_enabled
            && navigation_document_has_author_runtime_content(
                   &session->page.document);
        if (shed_author_script_work
            || early->failed != 0u || early->skipped_cross_origin != 0u
            || early->skipped_quota != 0u
            || early->skipped_pressure != 0u
            || early->cost_class_rejections != 0u) {
            session->page.script_degradation_observed = true;
        }
    }
    if (txn->streaming != NULL && txn->streaming->scheduler != NULL) {
        session->page.resource_scheduler = txn->streaming->scheduler;
        txn->streaming->scheduler_transferred = true;
    }
    if (!viewport_context_resolve(&session->viewport,
                                  &session->page.document,
                                  txn->viewport_width, 272, 980)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "mobile viewport resolution failed");
        return false;
    }
    navigation_apply_declared_viewport(session, txn->viewport_width, 272);
    if (session->page.runtime != NULL) {
        script_runtime_set_viewport(
            session->page.runtime, session->viewport.css_width,
            session->viewport.css_height, session->viewport.device_width,
            session->viewport.device_height);
    }
    if (txn->streaming != NULL
        && txn->streaming->preview_layout_reuse != NULL) {
        if (txn->streaming->viewport.css_width == session->viewport.css_width
            && txn->streaming->viewport.css_height
                == session->viewport.css_height) {
            session->page.layout_reuse =
                txn->streaming->preview_layout_reuse;
            txn->streaming->preview_layout_reuse = NULL;
            /* The preview withholds matched-rule lists while the parser
               still appends; the committed document is complete, so the
               page cache may retain them from its first full build on
               (navigation_ensure_layout_reuse enables them for a cache the
               page creates itself). Without them every relayout re-ran
               every selector against every node. */
            layout_reuse_cache_enable_retained_matches(
                session->page.layout_reuse);
        } else {
            layout_reuse_cache_destroy(
                txn->streaming->preview_layout_reuse);
            txn->streaming->preview_layout_reuse = NULL;
        }
    }
    txn->optional_work_shed = txn->streaming != NULL
        && txn->streaming->optional_work_shed;
    if (!txn->optional_work_shed && (session->document_scripts_enabled
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
            /* Native local entry surfaces have no initial network resources.
               Ordinary documents retain eager admission, including the curl
               reservation used by their font/resource pressure decisions. */
            bool local_entry = site_adapter_handles_navigation("GET", txn->url)
                && !site_adapter_navigation_requires_network("GET", txn->url);
            session->page.resource_scheduler =
                local_entry ? fetch_scheduler_create_deferred_in_domain(
                    session->page.fetch_domain, 12, reserved)
                : fetch_scheduler_create_in_domain(
                    session->page.fetch_domain, 12, reserved);
            if (session->page.resource_scheduler != NULL) {
                (void) fetch_scheduler_enable_background_transport(
                    session->page.resource_scheduler, true);
            }
        }
        if (session->page.resource_scheduler == NULL) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "document resource scheduler startup failed");
            return false;
        }
    }
    return navigation_commit_checkpoint(
            session, "navigation-setup",
            session->page.document.node_count);
}

/* Style: adopt the stream's blocking stylesheet when its inputs are
   unchanged (appending a pure suffix of body styles), else a cached
   compiled sheet, else build one. */
static TILEFINCH_OUT_OF_LINE bool commit_style(NavigationSession *session,
                                               NavigationCommitTxn *txn)
{
    uint64_t phase_started = navigation_now_us();
    if (txn->streaming != NULL && txn->streaming->blocking_stylesheet_ready
        && txn->streaming->blocking_stylesheet_fingerprint_valid
        && txn->streaming->viewport.css_width == session->viewport.css_width
        && txn->streaming->viewport.css_height == session->viewport.css_height
        && txn->streaming->blocking_stylesheet.viewport_width
            == session->viewport.css_width
        && txn->streaming->blocking_stylesheet.viewport_height
            == session->viewport.css_height
        && strcmp(txn->streaming->resource_base_url,
                  session->page.resource_base_url) == 0
        && strcmp(txn->streaming->referrer_policy,
                  session->page.referrer_policy) == 0) {
        uint64_t fingerprint_started = navigation_now_us();
        uint64_t final_fingerprint = 0;
        bool fingerprinted = navigation_stream_stylesheet_fingerprint(
            txn->streaming, &session->page.document, &final_fingerprint);
        uint64_t fingerprint_finished = navigation_now_us();
        session->performance.blocking_stylesheet_fingerprint_us +=
            fingerprint_finished >= fingerprint_started
                ? fingerprint_finished - fingerprint_started : 0;
        /* Pages commonly place <style> elements in the body after the
           preview's stylesheet was compiled (per-template styles). Those are
           a pure suffix of the same inputs; append them rather than parsing
           every external sheet again. User CSS already in the sheet is its
           own cascade origin, so author rules appended after it still order
           by origin first; the continuation itself refuses user CSS that
           changed the parse context. */
        if (fingerprinted
            && final_fingerprint
               != txn->streaming->blocking_stylesheet_fingerprint
            && navigation_stream_continue_stylesheet_suffix(
                   txn->streaming, &session->page.document, final_fingerprint,
                   true)) {
            session->performance.blocking_stylesheet_commit_continuations++;
        }
        if (fingerprinted
            && final_fingerprint
               == txn->streaming->blocking_stylesheet_fingerprint) {
            session->page.stylesheet = txn->streaming->blocking_stylesheet;
            layout_reuse_cache_rebind_stylesheet(
                session->page.layout_reuse,
                &txn->streaming->blocking_stylesheet,
                &session->page.stylesheet);
            memset(&txn->streaming->blocking_stylesheet, 0,
                   sizeof(txn->streaming->blocking_stylesheet));
            txn->streaming->blocking_stylesheet_ready = false;
            session->page.external_stylesheets =
                txn->streaming->blocking_stylesheets;
            txn->adopted_streaming_stylesheet = true;
            txn->stylesheet_external_current = true;
            txn->user_stylesheet_current = true;
            txn->adopted_stylesheet_fingerprint = final_fingerprint;
            navigation_record_compiled_stylesheet(session, final_fingerprint);
            session->performance.blocking_stylesheet_adoptions++;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
    }
    if (!txn->adopted_streaming_stylesheet && txn->streaming != NULL) {
        uint64_t fingerprint_started = navigation_now_us();
        uint64_t final_fingerprint = 0;
        bool fingerprinted = navigation_stream_stylesheet_fingerprint(
            txn->streaming, &session->page.document, &final_fingerprint);
        session->performance.blocking_stylesheet_fingerprint_us +=
            navigation_now_us() - fingerprint_started;
        if (fingerprinted
            && navigation_compiled_stylesheet_cache_take(
                session, final_fingerprint, &session->page.stylesheet,
                &session->page.external_stylesheets)) {
            txn->adopted_compiled_stylesheet = true;
            txn->stylesheet_external_current = true;
            txn->user_stylesheet_current = true;
            txn->adopted_stylesheet_fingerprint = final_fingerprint;
            navigation_record_compiled_stylesheet(session, final_fingerprint);
        }
    }
    if (!txn->adopted_streaming_stylesheet && !txn->adopted_compiled_stylesheet
        && !navigation_build_stylesheet(session)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "stylesheet construction failed");
        return false;
    }
    session->performance.style_us += navigation_record_slice(
        session, NAVIGATION_SLICE_STYLE, phase_started,
        session->page.document.node_count);
    return navigation_commit_checkpoint(
            session, "navigation-style",
            session->page.document.node_count);
}

/* External stylesheets: load what the adopted sheet does not already
   reflect, falling back to a usable sheet, and adopt the stylesheet load
   events. */
static TILEFINCH_OUT_OF_LINE bool commit_external_sheets(NavigationSession *session,
                                                         NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    if (session->external_resources_enabled) {
        if (stylesheet_document_resources_prepare_complete_census(
                &session->page.stylesheet_resources)) {
            txn->stylesheet_external_current = false;
            txn->user_stylesheet_current = false;
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
        if (!txn->stylesheet_external_current
            && !stylesheets_load_external_tracked_with_context(
                &session->page.document, &session->page.stylesheet,
                session->budget, txn->resource_base_url, txn->url,
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
                return false;
            }
            txn->stylesheet_external_current = true;
            txn->user_stylesheet_current = false;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
        /* Preserve the final bounded retry when an individual stylesheet
           failed even though the aggregate discovery pass completed. */
        txn->stylesheet_external_current = txn->stylesheet_external_current
            || session->page.external_stylesheets.failed == 0;
        if (txn->streaming == NULL
            || !txn->streaming->stylesheet_event_baseline_adopted) {
            navigation_adopt_stylesheet_events(
                &session->page.document, session->page.resource_base_url,
                session->page.stylesheet_events,
                &session->page.stylesheet_event_count);
        }
        if (!txn->adopted_streaming_stylesheet && !txn->adopted_compiled_stylesheet) {
            uint64_t stylesheet_elapsed = navigation_record_resource_slices(
                session, phase_started, &session->page.external_stylesheets,
                NULL);
            session->performance.resource_us += stylesheet_elapsed;
            session->performance.stylesheet_resource_us +=
                stylesheet_elapsed;
        }
    }
    return navigation_commit_checkpoint(
            session, "navigation-stylesheets",
            session->page.external_stylesheets.discovered);
}

/* Scripts: add script-blocking user CSS, admit the realm (degrading to a
   static page on a measured Budget refusal) and run the document's
   scripts, restoring the server body when hydration degrades. */
static TILEFINCH_OUT_OF_LINE bool commit_scripts(NavigationSession *session,
                                                 NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    if (session->user_css_length != 0 && !txn->user_stylesheet_current) {
        phase_started = navigation_now_us();
        if (!stylesheet_add_user_css(&session->page.stylesheet,
                                     session->user_css,
                                     session->user_css_length)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "script-blocking user stylesheet exceeded its budget");
            return false;
        }
        session->performance.style_us += navigation_record_slice(
            session, NAVIGATION_SLICE_STYLE, phase_started,
            session->page.stylesheet.count);
        txn->user_stylesheet_current = true;
    }
    phase_started = navigation_now_us();
    bool runtime_required = txn->allow_runtime && session->scripts_enabled
        && (txn->streaming == NULL
            || !txn->streaming->script_disabled_after_failure)
        && (txn->streaming == NULL || txn->streamed_runtime
            || navigation_document_requires_runtime(
                session, &session->page.document));
    if (runtime_required) {
        size_t runtime_failure_count = session->budget->failure_count;
        if (session->page.runtime == NULL) {
            session->page.runtime = navigation_create_runtime(
                session, &session->page.document, txn->url,
                txn->streaming == NULL ? "" : txn->streaming->document_referrer,
                txn->streaming == NULL ? 0 : txn->streaming->started_us,
                session->page.referrer_policy,
                session->document_scripts_enabled,
                &session->viewport, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
                session->page.fetch_domain,
                &session->page.stylesheet, &session->page.images,
                txn->fonts,
                &session->page.script_result);
        }
        if (session->page.runtime == NULL) {
            if (session->budget->failure_count == runtime_failure_count) {
                snprintf(
                    session->last_error, sizeof(session->last_error), "%s",
                    session->page.script_result.error[0] == '\0'
                      ? "JavaScript runtime startup failed"
                      : session->page.script_result.error);
                return false;
            }
            /* A realm used only for optional author behavior must not make a
               complete server-rendered document disappear when its bounded
               heap cannot be admitted beside that document. Keep genuine
               bootstrap/contract failures fatal, but turn a measured Budget
               refusal into a static-page degradation. */
            session->page.script_degradation_observed = true;
            session->page.script_result.success = false;
            session->page.script_memory_limited = true;
            session->page.script_result.error[0] = '\0';
            session->page.script_result.error_source_context[0] = '\0';
            snprintf(
                session->page.script_result.summary,
                sizeof(session->page.script_result.summary), "%s",
                "JavaScript stopped at the page memory limit; static content preserved");
            if (session->page.resource_scheduler != NULL) {
                fetch_scheduler_destroy(session->page.resource_scheduler);
                session->page.resource_scheduler = NULL;
            }
        }
        if (session->page.runtime != NULL) {
            script_runtime_set_stylesheet(session->page.runtime,
                                          &session->page.stylesheet);
            /* Streaming storage has moved, but deferred image sizing still
               runs before publication. Supply real image boxes instead of
               guessed CSS percentages. Do not turn unrelated library feature
               probes into full-page layouts during this admission phase. */
            script_runtime_set_image_sizing_layout_callback(
                session->page.runtime, navigation_flush_candidate_geometry,
                session);
            txn->script_document_may_have_changed = true;
            if (!page_capability_trace_setup(session)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "page capability trace setup failed: %.430s",
                         session->page.script_result.error);
                return false;
            }
            if (session->document_scripts_enabled) {
            ExternalScriptMetrics metrics = {0};
            DocumentBodySnapshot server_snapshot = {0};
            const DocumentBodySnapshot *rollback_snapshot = NULL;
            if (txn->streamed_runtime
                && txn->streaming->server_body_snapshot.markup != NULL) {
                rollback_snapshot = &txn->streaming->server_body_snapshot;
            } else if (document_body_snapshot_capture(
                           &session->page.document, &server_snapshot)) {
                rollback_snapshot = &server_snapshot;
            }
            bool scripts_ok = txn->streamed_runtime
                ? document_scripts_finish_streaming(
                    &session->page.document, session->page.runtime,
                    session->budget, session->browser_session,
                    txn->resource_base_url, txn->url,
                    session->page.referrer_policy,
                    session->maximum_scripts,
                    session->maximum_script_bytes,
                    session->maximum_script_file_bytes,
                    session->script_timeout_ms,
                    session->page.resource_scheduler, &txn->streaming->scripts,
                    &metrics, &session->page.script_result)
                : document_scripts_execute(
                    &session->page.document, session->page.runtime,
                    session->budget, session->browser_session,
                    txn->resource_base_url, txn->url,
                    session->page.referrer_policy,
                    session->maximum_scripts,
                    session->maximum_script_bytes,
                    session->maximum_script_file_bytes,
                    session->script_timeout_ms,
                    session->page.resource_scheduler, &metrics,
                    &session->page.script_result);
            if (script_runtime_document_refresh_failed(
                    session->page.runtime)) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "could not refresh mutated candidate DOM");
                document_body_snapshot_destroy(&server_snapshot);
                return false;
            }
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
                if (!navigation_retire_exhausted_script_realms(
                        session, false)) {
                    document_body_snapshot_destroy(&server_snapshot);
                    return false;
                }
            }
            bool hydration_degraded = !scripts_ok || metrics.failed != 0
                || metrics.skipped_quota != 0
                || metrics.skipped_pressure != 0
                || metrics.cost_class_rejections != 0;
            if (hydration_degraded || metrics.skipped_cross_origin != 0u)
                session->page.script_degradation_observed = true;
            if (metrics.skipped_pressure != 0u)
                session->page.script_memory_limited = true;
            if (hydration_degraded) {
                (void) navigation_restore_server_body_if_degraded(
                    session, rollback_snapshot);
            }
            document_body_snapshot_destroy(&server_snapshot);
            /* Ready-event geometry is an author-read snapshot, not the
               final image/font publication. Release it before those
               resource transients and the authoritative build overlap. */
            if (session->page.runtime != NULL)
                script_runtime_set_layout(session->page.runtime, NULL,
                                          session->viewport.css_height);
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
    }
    if (txn->streaming != NULL && txn->streaming->script_disabled_after_failure
        && txn->streaming->server_body_snapshot.markup != NULL) {
        (void) navigation_restore_server_body_if_degraded(
            session, &txn->streaming->server_body_snapshot);
    }
    session->performance.script_us += navigation_record_script_slices(
        session, phase_started, &session->page.script_result,
        session->script_attempted
            + session->page.script_result.scripts_evaluated);
    return navigation_commit_checkpoint(
            session, "navigation-scripts",
            session->script_attempted
                + session->page.script_result.scripts_evaluated);
}

/* Post-script style: rebuild the sheet if scripts changed what it was
   compiled from, and drop reused styles the script mutations reached. */
static TILEFINCH_OUT_OF_LINE bool commit_post_script_style(NavigationSession *session,
                                                           NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    if (txn->script_document_may_have_changed) {
        phase_started = navigation_now_us();
        bool rebuild_stylesheet = true;
        if (txn->adopted_streaming_stylesheet || txn->adopted_compiled_stylesheet) {
            uint64_t fingerprint_started = navigation_now_us();
            uint64_t post_script_fingerprint = 0;
            bool fingerprinted = navigation_stream_stylesheet_fingerprint(
                txn->streaming, &session->page.document,
                &post_script_fingerprint);
            uint64_t fingerprint_finished = navigation_now_us();
            session->performance.blocking_stylesheet_fingerprint_us +=
                fingerprint_finished >= fingerprint_started
                    ? fingerprint_finished - fingerprint_started : 0;
            rebuild_stylesheet = !fingerprinted
                || post_script_fingerprint
                   != txn->adopted_stylesheet_fingerprint;
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
                return false;
            }
            txn->stylesheet_external_current = false;
            txn->user_stylesheet_current = false;
        }
        if (session->page.runtime != NULL) {
            script_runtime_set_stylesheet(session->page.runtime,
                                          &session->page.stylesheet);
        }
        /* Everything before this boundary is reflected by the rebuilt
           stylesheet.  Keep mutations made by subsequent stylesheet load
           handlers pending so a handler which changes href or inserts
           another resource cannot be discarded at commit.  The layout
           cache, though, may be the streaming preview's, holding styles
           resolved before these scripts ran: drop what their changes
           reached (all of it when the journal cannot say) so the commit
           layout does not keep, say, a button's style from before its
           class changed. */
        if (session->page.runtime != NULL) {
            ScriptMutationJournal committed = {0};
            if (script_runtime_consume_mutations(
                    session->page.runtime, &committed)
                && session->page.layout_reuse != NULL)
                navigation_prepare_layout_reuse(session, &committed, false, NULL);
        }
        session->performance.style_us += navigation_record_slice(
            session, NAVIGATION_SLICE_STYLE, phase_started,
            session->page.document.node_count);
    }
    return navigation_commit_checkpoint(
            session, "navigation-post-script-style",
            session->page.document.node_count);
}

/* Final style: load late external sheets and their events, add user CSS,
   apply the static-script fallback, and prepare Reader candidates when
   needed. */
static TILEFINCH_OUT_OF_LINE bool commit_final_style(NavigationSession *session,
                                                     NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    phase_started = navigation_now_us();
    txn->progressive_preview_candidate =
        stylesheet_web_font_set(&session->page.stylesheet) != NULL;
    if (session->external_resources_enabled) {
        if (session->page.runtime != NULL) {
            (void) script_runtime_collect_and_trim(session->page.runtime);
        }
        if (!txn->stylesheet_external_current
            && !stylesheets_load_external_tracked_with_context(
                &session->page.document, &session->page.stylesheet,
                session->budget, session->page.resource_base_url, txn->url,
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
                return false;
            }
            txn->stylesheet_external_current = true;
            txn->user_stylesheet_current = false;
            if (session->page.runtime != NULL) {
                script_runtime_set_stylesheet(
                    session->page.runtime, &session->page.stylesheet);
            }
        }
        if (!navigation_dispatch_stylesheet_events(
                session, session->page.resource_base_url)
            && !navigation_retire_exhausted_script_realms(
                    session, false)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "stylesheet completion event dispatch failed");
            return false;
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
    if (session->user_css_length != 0 && !txn->user_stylesheet_current
        && !stylesheet_add_user_css(&session->page.stylesheet,
                                    session->user_css,
                                    session->user_css_length)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "user stylesheet exceeded its budget");
        return false;
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
    /* Session counters are retained diagnostics and can describe an earlier
       page.  Only the document-local fact may relax custom-element hiding for
       this commit; otherwise a clean blank shell after a failed navigation
       can accidentally inherit the previous page's fallback. */
    bool static_script_fallback =
        session->page.script_degradation_observed;
    if (static_script_fallback) {
        stylesheet_enable_static_custom_element_fallback(
            &session->page.stylesheet);
        (void) document_install_static_shell_fallback(
            &session->page.document);
    }
    /* A full Reader style census is optional and expensive on an ordinary
       healthy commit. Defer it until an explicit Reader request unless this
       document already carries hard script-degradation evidence and may need
       automatic recovery. */
    if (session->prepare_reader_candidates
        && (static_script_fallback || txn->optional_work_shed
            || session->user_css_length != 0u)) {
        /* Build the extracted root outside the author-visible tree and retain
           that exact identity before any later author turn. Reserve the fixed
           weak-handle slot first so exhaustion declines Reader without
           leaving an untracked public marker behind. */
        if (!script_runtime_node_handle_capacity_available(
                session->page.runtime)) {
            txn->reader_analysis = (ReaderDocumentAnalysis) {
                .prepared = true, .bounded_out = true
            };
        } else {
            lxb_dom_node_t *prepared_root = NULL;
            bool prepared = reader_document_prepare_detached_with_stylesheet(
                &session->page.document, &session->page.stylesheet,
                &txn->reader_analysis, &prepared_root);
            if (!prepared) {
                txn->reader_analysis = (ReaderDocumentAnalysis) {
                    .prepared = true, .kind = READER_PAGE_RAW,
                    .bounded_out = true
                };
            } else if (prepared_root != NULL
                       && txn->reader_analysis.kind != READER_PAGE_RAW) {
                long handle = session->page.runtime == NULL ? 0
                    : script_runtime_node_weak_handle(
                          session->page.runtime, prepared_root);
                if (session->page.runtime != NULL && handle == 0) {
                    reader_document_discard_exact_prepared_view(
                        &session->page.document, prepared_root,
                        txn->reader_analysis.kind);
                    txn->reader_analysis = (ReaderDocumentAnalysis) {
                        .prepared = true, .kind = READER_PAGE_RAW,
                        .bounded_out = true
                    };
                } else {
                    session->page.reader_root = prepared_root;
                    session->page.reader_root_kind = txn->reader_analysis.kind;
                    session->page.reader_root_handle = handle;
                    /* An explicit Reader stylesheet with no remaining author
                       tasks can enter Reader in this same native commit. When
                       work remains, keep the exact tree private until the
                       BrowserEngine settle boundary connects it. */
                    if (session->user_css_length != 0u
                        && session->page.script_result.pending_tasks == 0u) {
                        (void) reader_document_connect_prepared_view(
                            &session->page.document, prepared_root,
                            txn->reader_analysis.kind);
                    }
                }
            }
        }
        session->page.reader_analysis = txn->reader_analysis;
    }
    return navigation_commit_checkpoint(
            session, "navigation-final-style",
            session->page.stylesheet.count);
}

/* Fonts: record the compiled sheet for reuse and start the page's web
   fonts. */
static TILEFINCH_OUT_OF_LINE bool commit_fonts(NavigationSession *session,
                                               NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    /* Cache only source-derived sheets. Author script CSSOM mutations can
       compile rules that have no representation in the DOM fingerprint;
       replaying those on another document would be incorrect. */
    session->page.compiled_stylesheet_cacheable = false;
    if (txn->streaming != NULL && session->script_attempted == 0) {
        uint64_t final_fingerprint = 0;
        if (navigation_stream_stylesheet_fingerprint(
                txn->streaming, &session->page.document,
                &final_fingerprint)) {
            navigation_record_compiled_stylesheet(session, final_fingerprint);
        }
    }
    if (session->web_fonts_enabled) {
        phase_started = navigation_now_us();
        if (!navigation_load_web_fonts(session, txn->url)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "external font pipeline could not start within its budget");
            return false;
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
    return navigation_commit_checkpoint(
            session, "navigation-fonts",
            session->page.external_fonts.attempted);
}

/* Images: reserve the frame tail, load the visible-priority images (from a
   preview layout, a static plan or the markup) and plan the rest. */
static TILEFINCH_OUT_OF_LINE bool commit_images(NavigationSession *session,
                                                NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;

    ImagePriorityTarget image_priority_targets[
        NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
    size_t image_priority_count = 0;
    bool static_image_plan = false;
    /* A committed LayoutDocument borrows ImageResource pointers. Reserve the
       bounded child-frame tail before the first preview/final layout so an
       asynchronously completed iframe can append its snapshot without
       moving that table underneath the visible page. */
    size_t reserved_image_capacity = session->maximum_images;
    if (reserved_image_capacity
            <= SIZE_MAX - NAVIGATION_FRAME_LIMIT) {
        reserved_image_capacity += NAVIGATION_FRAME_LIMIT;
    } else {
        reserved_image_capacity = SIZE_MAX;
    }
    if (!images_reserve_capacity(
            &session->page.images, session->budget,
            reserved_image_capacity)) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "frame image metadata exceeded its budget");
        return false;
    }
    bool pre_image_preview_candidate =
        session->external_resources_enabled
        && navigation_document_may_have_images(
            &session->page.document, &session->page.stylesheet);
    if (pre_image_preview_candidate) {
        (void) navigation_try_progressive_preview(
            session, txn->streaming, txn->previous_page, true, txn->fonts, txn->images, false,
            &session->page.layout, image_priority_targets,
            NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT, &image_priority_count);
        session->performance.progressive_image_priority_nodes =
            image_priority_count;
        /* Browser-authored static documents are deliberately small and have
           no author script capable of invalidating this probe. Build one
           bounded no-image layout to obtain exact boxes; ordinary web pages
           retain the streaming-preview/markup policy above. */
        if (image_priority_count == 0 && !txn->allow_runtime
            && txn->streaming == NULL) {
            static_image_plan = navigation_plan_static_image_geometry(
                session, txn->fonts, txn->images, image_priority_targets,
                NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT,
                &image_priority_count, &txn->final_layout_ready);
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
        /* A Reader navigation's sheet hides the author article that the raw
           toggle restores, and the Reader clone aliases those images' shared
           backing; keep that navigation's markup images eager. */
        size_t raster_added = navigation_collect_markup_image_priority(
            session->budget,
            session->prepare_reader_candidates
                ? NULL : &session->page.stylesheet,
            &session->page.document, image_priority_targets,
            markup_priority_capacity, image_priority_count);
        image_priority_count += raster_added;
        markup_priority_added += raster_added;
    }
    session->performance.markup_image_priority_nodes =
        markup_priority_added;

    txn->provisional_commit =
        navigation_provisional_commit_eligible(session, txn->streaming);
    bool defer_mutable_document_images = !txn->provisional_commit
        && session->external_resources_enabled
        && txn->streaming != NULL && txn->allow_runtime && session->progressive_paint != NULL
        && navigation_plan_mutable_document_images(session);
    if (defer_mutable_document_images) {
        size_t retained = 0;
        for (size_t i = 0; i < image_priority_count; i++) {
            const ImagePriorityTarget *target = &image_priority_targets[i];
            if (target->kind == IMAGE_PRIORITY_KIND_DOCUMENT
                && node_name_is(target->node, "img")) continue;
            image_priority_targets[retained++] = *target;
        }
        image_priority_count = retained;
    }

    if (session->external_resources_enabled) {
        phase_started = navigation_now_us();
        size_t loaded_before = session->page.images.stats.loaded;
        bool image_pipeline_refused = false;
        if (image_priority_count != 0
            && !images_load_external_priority_targets(
                &session->page.document, &session->page.stylesheet,
                &session->page.images, image_priority_targets,
                image_priority_count, session->budget,
                session->page.resource_base_url, txn->url,
                session->page.referrer_policy, session->maximum_images,
                session->maximum_image_bytes,
                session->maximum_image_file_bytes,
                session->maximum_decoded_image_bytes,
                session->resource_timeout_ms,
                session->page.resource_scheduler,
                session->browser_session)) {
            if (session->page.images.stats.load_cancelled) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "visible image priority pipeline cancelled");
                return false;
            }
            /* Images are optional. The loader rolled back its suffix; even
               a first-image refusal must not discard the usable document.
               Do not immediately repeat the same load in the full pass. */
            session->performance.image_pressure_fallbacks++;
            image_pipeline_refused = true;
        }
        if (session->page.images.stats.loaded >= loaded_before) {
            session->performance.progressive_image_priority_loaded =
                session->page.images.stats.loaded - loaded_before;
        }
        if (image_priority_count != 0 && !image_pipeline_refused) {
            txn->images = &session->page.images;
            layout_reuse_cache_update_images(
                session->page.layout_reuse, txn->images);
            size_t remaining_priority_count = 0;
            txn->final_layout_ready = navigation_try_progressive_preview(
                session, txn->streaming, txn->previous_page, true, txn->fonts, txn->images,
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
            && !txn->allow_runtime && txn->streaming == NULL
            && navigation_document_images_are_defer_safe(
                   &session->page.document, &session->page.stylesheet, &session->page.images);
        if (image_pipeline_refused) {
            budget_free(session->budget, session->page.deferred_image_targets);
            session->page.deferred_image_targets = NULL;
            session->page.deferred_image_count = 0;
            session->page.image_continuation_pending = false;
            session->page.visual_discovery_pending = false;
        } else if (txn->provisional_commit) {
            /* The viewport's priority visuals are loaded above; the rest
               are discovered by the first complete layout. */
            budget_free(session->budget,
                        session->page.deferred_image_targets);
            session->page.deferred_image_targets = NULL;
            session->page.deferred_image_count = 0;
            session->page.image_continuation_pending = false;
            session->page.visual_discovery_pending = true;
        } else if ((continue_images_after_paint
                    && !defer_mutable_document_images)
                   || defer_static_document_images) {
            session->page.image_continuation_pending = true;
        } else {
            if (!defer_mutable_document_images) {
                budget_free(session->budget,
                            session->page.deferred_image_targets);
                session->page.deferred_image_targets = NULL;
                session->page.deferred_image_count = 0;
            }
            navigation_ensure_layout_reuse(session);
            session->page.image_continuation_pending = defer_mutable_document_images;
            if (!images_load_external_reusing_layout_styles_deferred(
                    &session->page.document, &session->page.stylesheet,
                    &session->page.images, session->budget,
                    session->page.resource_base_url, txn->url,
                    session->page.referrer_policy,
                    session->maximum_images, session->maximum_image_bytes,
                    session->maximum_image_file_bytes,
                    session->maximum_decoded_image_bytes,
                    session->resource_timeout_ms,
                    session->page.resource_scheduler,
                    session->browser_session, session->page.layout_reuse,
                    txn->fonts, session->viewport.css_width,
                    defer_mutable_document_images)) {
                if (session->page.images.stats.load_cancelled) {
                    snprintf(session->last_error,
                             sizeof(session->last_error),
                             "external image pipeline cancelled");
                    return false;
                }
                session->performance.image_pressure_fallbacks++;
                session->page.image_continuation_pending = false;
                budget_free(session->budget, session->page.deferred_image_targets);
                session->page.deferred_image_targets = NULL;
                session->page.deferred_image_count = 0;
            }
        }
        txn->images = &session->page.images;
        layout_reuse_cache_update_images(
            session->page.layout_reuse, txn->images);
        txn->progressive_preview_candidate = session->page.images.count != 0
            || session->page.stylesheet.image_url_count != 0
            || txn->progressive_preview_candidate;
        uint64_t image_finished = navigation_now_us();
        session->performance.image_resource_us +=
            image_finished >= phase_started
                ? image_finished - phase_started : 0;
        session->performance.resource_us +=
            navigation_record_resource_slices(
                session, phase_started, NULL, &session->page.images.stats);
    }
    return navigation_commit_checkpoint(
            session, "navigation-images",
            session->page.images.stats.work_units);
}

typedef enum {
    NAVIGATION_COMMIT_LAYOUT_FAILED,
    /* The authoritative layout became a resumable job the caller runs;
       the commit is complete without the publish phase. */
    NAVIGATION_COMMIT_LAYOUT_DEFERRED,
    NAVIGATION_COMMIT_LAYOUT_BUILT
} NavigationCommitLayoutOutcome;

/* Layout: build the authoritative layout (a progressive preview, a
   provisional prefix, a deferred job, or the full build with one minimal-
   stylesheet retry under pressure) and finish it. */
static TILEFINCH_OUT_OF_LINE NavigationCommitLayoutOutcome commit_layout(
    NavigationSession *session, NavigationCommitTxn *txn)
{
    uint64_t phase_started = 0;
    if (!txn->final_layout_ready && session->performance.partial_paints == 0) {
        txn->final_layout_ready = navigation_try_progressive_preview(
            session, txn->streaming, txn->previous_page,
            txn->progressive_preview_candidate, txn->fonts, txn->images, true,
            &session->page.layout, NULL, 0, NULL);
    }
    if (!txn->final_layout_ready) {
        phase_started = navigation_now_us();
        if (session->page.images.count != 0) {
            /* Parser-time canvases are page resources even when ordinary
               external image loading is disabled (for example a data-only
               local document). Include them in the authoritative layout. */
            txn->images = &session->page.images;
        }
        if (txn->provisional_commit) {
            layout_destroy(&session->page.layout);
            memset(&session->page.layout, 0, sizeof(session->page.layout));
            navigation_ensure_layout_reuse(session);
            bool truncated = false;
            /* Through the screen the reader scrolled the load preview to,
               and two more, so the commit lands where they are reading. */
            int limit = 3 * session->viewport.css_height;
            int reader_y = session->provisional_reader_y;
            if (reader_y > 0 && reader_y <= INT_MAX - limit)
                limit += reader_y;
            if (layout_build_context_preview(
                    &session->page.layout, session->budget,
                    &session->page.document, &session->page.stylesheet,
                    txn->fonts, txn->images, &session->viewport,
                    session->page.layout_reuse, limit, &truncated)) {
                if (truncated) {
                    LayoutDocument *layout = &session->page.layout;
                    layout->content_limit_y =
                        layout->height > 0 ? layout->height : 1;
                    layout->height = layout->content_limit_y
                        + 2 * session->viewport.css_height;
                    session->performance.provisional_relayouts++;
                }
                txn->final_layout_ready = true;
                uint64_t elapsed = navigation_record_layout_slices(
                    session, phase_started, &session->page.layout);
                session->performance.layout_us += elapsed;
                session->performance.final_layout_us += elapsed;
            } else {
                /* Fall back to the ordinary complete build. */
                layout_destroy(&session->page.layout);
                memset(&session->page.layout, 0,
                       sizeof(session->page.layout));
            }
        }
    }
    if (!txn->final_layout_ready) {
        phase_started = navigation_now_us();
        if (txn->defer_authoritative_layout && txn->previous_page == NULL
            && !txn->record_history && txn->candidate_prepare == NULL
            && txn->deferred_layout_job != NULL
            && session->page.document.node_count >= 256) {
            navigation_ensure_layout_reuse(session);
            size_t layout_failure_count = session->budget->failure_count;
            size_t injected_failure_count = session->budget->injected_failure_count;
            LayoutBuildJob *job = layout_build_job_begin(
                session->budget, &session->page.document,
                &session->page.stylesheet, txn->fonts, txn->images,
                &session->viewport, session->page.layout_reuse);
            if (job == NULL
                && session->budget->failure_count > layout_failure_count
                && session->budget->injected_failure_count == injected_failure_count
                && navigation_install_minimal_stylesheet_fallback(session)) {
                /* Author CSS is optional when it crowds a complete server
                   document out of its first layout. Rebuild the cheap
                   bounded stylesheet and retry once; invalid layout inputs
                   still fail without taking this pressure-only path. */
                layout_reuse_cache_destroy(session->page.layout_reuse);
                session->page.layout_reuse = NULL;
                navigation_ensure_layout_reuse(session);
                job = layout_build_job_begin(
                    session->budget, &session->page.document,
                    &session->page.stylesheet, txn->fonts, txn->images,
                    &session->viewport, session->page.layout_reuse);
            }
            if (job == NULL) {
                snprintf(session->last_error, sizeof(session->last_error),
                         "resumable layout startup failed");
                return NAVIGATION_COMMIT_LAYOUT_FAILED;
            }
            *txn->deferred_layout_job = job;
            if (txn->deferred_preview_candidate != NULL) {
                *txn->deferred_preview_candidate =
                    txn->progressive_preview_candidate;
            }
            return NAVIGATION_COMMIT_LAYOUT_DEFERRED;
        }
        size_t layout_failure_count = session->budget->failure_count;
        size_t injected_failure_count = session->budget->injected_failure_count;
        bool layout_built = navigation_build_layout(
            session, &session->page.layout, txn->fonts, txn->images);
        unsigned failed_phase = session->page.layout.performance.failed_phase;
        if (!layout_built && !session->layout_build_cancelled
            && session->budget->failure_count > layout_failure_count
            && session->budget->injected_failure_count == injected_failure_count) {
            /* Match deferred startup's pressure recovery. Author CSS must
               not strand a valid new document merely because this caller
               builds synchronously. Release failed layout/cache ownership
               before replacing optional styles, then retry exactly once.
               Cancellation and synthetic refusals never take this path.
               layout_destroy clears the failed phase; a retry that cannot
               start reports the first build's. */
            layout_destroy(&session->page.layout);
            layout_reuse_cache_destroy(session->page.layout_reuse);
            session->page.layout_reuse = NULL;
            if (navigation_install_minimal_stylesheet_fallback(session)) {
                layout_built = navigation_build_layout(
                    session, &session->page.layout, txn->fonts, txn->images);
                failed_phase = session->page.layout.performance.failed_phase;
            }
        }
        if (!layout_built) {
            navigation_note_layout_failure(
                session, failed_phase,
                session->layout_build_cancelled,
                session->budget->failure_count - layout_failure_count);
            return NAVIGATION_COMMIT_LAYOUT_FAILED;
        }
        uint64_t final_layout_elapsed = navigation_record_layout_slices(
            session, phase_started, &session->page.layout);
        session->performance.layout_us += final_layout_elapsed;
        session->performance.final_layout_us += final_layout_elapsed;
    }
    if (!navigation_finish_authoritative_layout(
            session, txn->url, txn->streaming, txn->previous_page,
            txn->progressive_preview_candidate, txn->fonts, txn->images))
        return NAVIGATION_COMMIT_LAYOUT_FAILED;
    return NAVIGATION_COMMIT_LAYOUT_BUILT;
}

/* Publish: run the candidate hooks and record history. Past this phase the
   commit cannot fail. */
static TILEFINCH_OUT_OF_LINE bool commit_publish(NavigationSession *session,
                                                 NavigationCommitTxn *txn)
{
    if (txn->candidate_prepare != NULL) {
        txn->candidate_hook_attempted = true;
        if (txn->candidate_graph_prepare != NULL
            && !txn->candidate_graph_prepare(txn->candidate_opaque, session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "candidate state restoration failed");
            return false;
        }
        lxb_html_document_t *candidate_document = session->page.document.html;
        ScriptRuntime *candidate_runtime = session->page.runtime;
        DrawCommand *candidate_commands = session->page.layout.commands;
        FetchScheduler *candidate_scheduler =
            session->page.resource_scheduler;
        FetchSchedulerDomain *candidate_domain =
            session->page.fetch_domain;
        txn->candidate_hook_attempted = true;
        if (!txn->candidate_prepare(txn->candidate_opaque, session)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "candidate commit preparation failed");
            return false;
        }
        if (txn->generation != session->generation || session->cancelled
            || !session->page.loaded
            || session->page.document.html != candidate_document
            || session->page.runtime != candidate_runtime
            || session->page.layout.commands != candidate_commands
            || session->page.resource_scheduler != candidate_scheduler
            || session->page.fetch_domain != candidate_domain) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "candidate changed during commit preparation");
            return false;
        }
    }
    if ((txn->record_history
         && !history_push(session, txn->url, session->page.document.title))
        || (txn->replace_history
            && !history_replace_current(session, txn->url,
                                        session->page.document.title))) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "history entry allocation failed");
        return false;
    }
    return true;
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
    NavigationCommitTxn txn = {0};
    txn.generation = generation;
    txn.url = url;
    txn.document = document;
    txn.viewport_width = viewport_width;
    txn.fonts = fonts;
    txn.images = images;
    txn.record_history = record_history;
    txn.streaming = streaming;
    txn.allow_runtime = allow_runtime;
    txn.defer_authoritative_layout = defer_authoritative_layout;
    txn.deferred_layout_job = deferred_layout_job;
    txn.deferred_preview_candidate = deferred_preview_candidate;
    /* A direct commit (a site adapter's page, committed HTML) may be the
       armed history "replace"; a load took its arming when it started. */
    txn.replace_history = navigation_take_history_replace(
        session, generation, url, record_history);
    txn.candidate_prepare = session->candidate_prepare;
    txn.candidate_graph_prepare = session->candidate_graph_prepare;
    txn.candidate_abort = session->candidate_abort;
    txn.candidate_commit = session->candidate_commit;
    txn.candidate_opaque = session->candidate_commit_opaque;
    if (!navigation_document_resource_base(
            document, url, txn.resource_base_url)) {
        return false;
    }
    txn.fetch_domain_from_stream = streaming != NULL
        && streaming->fetch_domain != NULL;
    FetchSchedulerDomain *retained_fetch_domain =
        shared_fetch_domain == NULL
            ? session->page.fetch_domain : shared_fetch_domain;
    txn.candidate_fetch_domain =
        txn.fetch_domain_from_stream
            ? streaming->fetch_domain
            : (retained_fetch_domain == NULL
                 ? fetch_scheduler_domain_create(
                       session->budget,
                       NAVIGATION_PAGE_FETCH_SLOT_LIMIT)
                 : fetch_scheduler_domain_retain(retained_fetch_domain));
    if (txn.candidate_fetch_domain == NULL) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "page fetch scheduler domain startup failed");
        return false;
    }
    if (page_has_owned_state(&session->page)) {
        txn.previous_page = budget_malloc_category(
            session->budget, BUDGET_CATEGORY_SESSION,
            sizeof(*txn.previous_page));
        if (txn.previous_page == NULL) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "navigation transaction scratch allocation failed");
            if (!txn.fetch_domain_from_stream) {
                fetch_scheduler_domain_release(txn.candidate_fetch_domain);
            }
            return false;
        }
        *txn.previous_page = session->page;
    }
    commit_snapshot_capture(session, &txn.snapshot);
    if (!commit_setup(session, &txn)) goto fail;
    if (!commit_style(session, &txn)) goto fail;
    if (!commit_external_sheets(session, &txn)) goto fail;
    if (!commit_scripts(session, &txn)) goto fail;
    if (!commit_post_script_style(session, &txn)) goto fail;
    if (!commit_final_style(session, &txn)) goto fail;
    if (!commit_fonts(session, &txn)) goto fail;
    if (!commit_images(session, &txn)) goto fail;
    switch (commit_layout(session, &txn)) {
    case NAVIGATION_COMMIT_LAYOUT_FAILED:
        goto fail;
    case NAVIGATION_COMMIT_LAYOUT_DEFERRED:
        return true;
    case NAVIGATION_COMMIT_LAYOUT_BUILT:
        break;
    }
    if (!commit_publish(session, &txn)) goto fail;
    if (txn.candidate_hook_attempted) {
        txn.candidate_commit(txn.candidate_opaque);
    }
    navigation_compiled_stylesheet_cache_store(session, txn.previous_page);
    page_destroy_value(session, txn.previous_page);
    budget_free(session->budget, txn.previous_page);
    session->replacement_failure_overlay_active = false;
    session->replacement_failure_message[0] = '\0';
    session->page.committed_us = navigation_now_us();
    session->loads_committed++;
    return true;

fail:
    if (txn.candidate_hook_attempted) {
        txn.candidate_abort(txn.candidate_opaque);
    }
    bool rollback_progressive_frame =
        session->performance.partial_paints != 0
        && session->progressive_paint != NULL;
    page_destroy(session);
    if (txn.previous_page != NULL) session->page = *txn.previous_page;
    budget_free(session->budget, txn.previous_page);
    commit_snapshot_restore(session, &txn.snapshot);
    if (rollback_progressive_frame) {
        (void) session->progressive_paint(
            session->progressive_paint_opaque, session, NULL);
        session->performance.progressive_paint_rollbacks++;
    }
    return false;
}
