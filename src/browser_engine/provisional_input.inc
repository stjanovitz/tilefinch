/* Provisional preview input: the provisional viewport, scrolling, focus
   movement and activation inside the preview, and its raster step. */

bool browser_engine_provisional_viewport(
    const BrowserEngine *engine, BrowserProvisionalViewport *viewport)
{
    if (viewport != NULL) memset(viewport, 0, sizeof(*viewport));
    if (engine == NULL || viewport == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING
        || engine->provisional_view == BROWSER_PREVIEW_VIEW_NONE
        || engine->provisional_frame == NULL) {
        return false;
    }
    __sync_synchronize();
    bool live = browser_preview_view_live(engine->provisional_view);
    *viewport = (BrowserProvisionalViewport) {
        .pixels = engine->provisional_frame,
        .pixel_count = engine->provisional_frame_pixels,
        .scroll_y = engine->provisional_scroll_y,
        .maximum_scroll_y = engine->provisional_maximum_scroll,
        .content_end_y = live ? engine->provisional_content_end : 0,
        .viewport_height = live && engine->provisional_viewport_height > 0
            ? engine->provisional_viewport_height
            : engine->config.device.framebuffer_height,
        .complete = engine->provisional_view != BROWSER_PREVIEW_VIEW_FILLING,
        .ready = true
    };
    return true;
}

bool browser_engine_scroll_provisional_by(BrowserEngine *engine, int delta)
{
    if (engine == NULL || delta == 0
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING) return false;
    /* Only a live preview moves; a press on a frame nothing can move stays
       with the frontend until one is live again. */
    if (browser_engine_provisional_front(engine) == NULL) return false;
    (void) browser_engine_provisional_clamp(
        engine, engine->provisional_layout,
        &engine->provisional_maximum_scroll);
    int maximum = engine->provisional_maximum_scroll;
    int64_t target = (int64_t) engine->provisional_scroll_y + delta;
    int y = target < 0 ? 0 : (target > maximum ? maximum : (int) target);
    if (y == engine->provisional_scroll_y) return false;
    /* Move at once, with a checkerboard where tiles are not rasterized or
       content is not laid out yet; raster steps fill it in. The stream
       lays out the next preview, and a provisional commit the page,
       through where the reader is. */
    engine->provisional_scroll_y = y;
    engine->navigation.provisional_reader_y = y;
    engine->navigation_work.metrics.provisional_scrolls++;
    engine->navigation_work.metrics.provisional_scroll_y = y;
    (void) browser_engine_compose_provisional(engine, 0);
    return true;
}

bool browser_engine_scroll_provisional_page(
    BrowserEngine *engine, int direction)
{
    if (engine == NULL || direction == 0
        || browser_engine_provisional_front(engine) == NULL) return false;
    /* The same step as Page Down on a loaded page. */
    ViewportContext viewport = engine->provisional_layout->viewport;
    viewport.css_height = browser_engine_provisional_viewport_height(
        engine, engine->provisional_layout);
    int step = controller_page_step(
        engine->provisional_layout, &viewport,
        engine->provisional_scroll_y, direction);
    if (step <= 0) return false;
    return browser_engine_scroll_provisional_by(
        engine, direction > 0 ? step : -step);
}

bool browser_engine_provisional_focus_direction(
    BrowserEngine *engine, ControllerFocusDirection direction)
{
    if (engine == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING
        || browser_engine_provisional_front(engine) == NULL) return false;
    const LayoutDocument *layout = engine->provisional_layout;
    int top = engine->provisional_scroll_y;
    int bottom = top
        + browser_engine_provisional_viewport_height(engine, layout);
    bool backward = direction == CONTROLLER_FOCUS_LEFT
        || direction == CONTROLLER_FOCUS_UP;
    bool vertical = direction == CONTROLLER_FOCUS_UP
        || direction == CONTROLLER_FOCUS_DOWN;
    int fx = 0, fy = 0, fw = 0, fh = 0;
    bool from = engine->provisional_focus_node != NULL
        && browser_engine_preview_region(
               layout, engine->provisional_focus_node,
               &fx, &fy, &fw, &fh, NULL)
        && fy < bottom && fy + fh > top;
    lxb_dom_node_t *best = NULL;
    bool best_beam = false;
    int64_t best_score = INT64_MAX;
    for (size_t kind = 0; kind < 2; kind++) {
        size_t count = kind == 0 ? layout->link_count : layout->control_count;
        for (size_t i = 0; i < count; i++) {
            lxb_dom_node_t *node;
            int x, y, width, height;
            if (kind == 0) {
                const LinkRegion *r = &layout->links[i];
                node = r->node; x = r->x; y = r->y;
                width = r->width; height = r->height;
            } else {
                const ControlRegion *r = &layout->controls[i];
                node = r->node; x = r->x; y = r->y;
                width = r->width; height = r->height;
            }
            /* Only what the reader can see; scrolling reaches the rest. */
            if (node == NULL || node == engine->provisional_focus_node
                || width <= 0 || height <= 0
                || y >= bottom || y + height <= top) continue;
            int64_t score;
            bool beam = false;
            if (!from) {
                /* Enter at the first visible item, or the last going back. */
                score = (int64_t) y * 65536 + x;
                if (backward) score = -score;
            } else {
                int64_t from_c = vertical ? (int64_t) fy * 2 + fh
                                          : (int64_t) fx * 2 + fw;
                int64_t to_c = vertical ? (int64_t) y * 2 + height
                                        : (int64_t) x * 2 + width;
                if (backward ? to_c >= from_c : to_c <= from_c) continue;
                int64_t primary = to_c > from_c
                    ? to_c - from_c : from_c - to_c;
                int from_lo = vertical ? fx : fy;
                int from_hi = vertical ? fx + fw : fy + fh;
                int to_lo = vertical ? x : y;
                int to_hi = vertical ? x + width : y + height;
                beam = to_lo < from_hi && from_lo < to_hi;
                int64_t secondary = to_hi <= from_lo ? from_lo - to_hi
                    : (from_hi <= to_lo ? to_lo - from_hi : 0);
                score = primary * 1024 + secondary * 64;
                /* A target on the requested axis beats any diagonal one. */
                if (best != NULL && best_beam && !beam) continue;
                if (beam && !best_beam) best_score = INT64_MAX;
            }
            if (score >= best_score) continue;
            best_score = score;
            best_beam = beam;
            best = node;
        }
    }
    if (best == NULL) return false;
    engine->provisional_focus_node = best;
    engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_IDLE;
    (void) browser_engine_compose_provisional(engine, 0);
    return true;
}

BrowserPreviewActivation browser_engine_provisional_activate(
    BrowserEngine *engine, bool at_point, int device_x, int device_y,
    ControllerAction *action)
{
    if (action != NULL) memset(action, 0, sizeof(*action));
    if (engine == NULL || action == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING
        || browser_engine_provisional_front(engine) == NULL)
        return BROWSER_PREVIEW_ACTIVATION_NONE;
    const LayoutDocument *layout = engine->provisional_layout;
    lxb_dom_node_t *node = NULL;
    const LinkRegion *link = NULL;
    if (at_point) {
        const ViewportContext *viewport = &layout->viewport;
        int x = viewport_device_to_css(viewport, device_x);
        int y = viewport_device_to_css(viewport, device_y)
            + engine->provisional_scroll_y;
        /* The topmost region under the cursor: highest z, then latest. */
        int best_z = INT_MIN;
        for (size_t i = 0; i < layout->link_count; i++) {
            const LinkRegion *r = &layout->links[i];
            if (r->node == NULL || x < r->x || x >= r->x + r->width
                || y < r->y || y >= r->y + r->height
                || r->z_index < best_z) continue;
            best_z = r->z_index;
            node = r->node;
            link = r;
        }
        for (size_t i = 0; i < layout->control_count; i++) {
            const ControlRegion *r = &layout->controls[i];
            if (r->node == NULL || x < r->x || x >= r->x + r->width
                || y < r->y || y >= r->y + r->height
                || r->z_index < best_z) continue;
            best_z = r->z_index;
            node = r->node;
            link = NULL;
        }
        if (node == NULL) return BROWSER_PREVIEW_ACTIVATION_NONE;
    } else {
        int x, y, width, height;
        node = engine->provisional_focus_node;
        if (node == NULL
            || !browser_engine_preview_region(
                   layout, node, &x, &y, &width, &height, &link)) {
            /* Nothing focused: the first press focuses, as on a page. */
            engine->provisional_focus_node = NULL;
            return browser_engine_provisional_focus_direction(
                       engine, CONTROLLER_FOCUS_RIGHT)
                ? BROWSER_PREVIEW_ACTIVATION_FOCUSED
                : BROWSER_PREVIEW_ACTIVATION_NONE;
        }
    }
    engine->provisional_focus_node = node;
    bool same_document = false;
    if (link != NULL && link->url != NULL
        && navigation_load_resolve_preview_link(
               engine->navigation_work.load, link->url, link->url_length,
               action->url, sizeof(action->url), &same_document)
        && !same_document
        && (strncmp(action->url, "https://", 8) == 0
            || strncmp(action->url, "http://", 7) == 0)) {
        /* Leaving the page: the frontend cancels this load and follows. */
        action->type = CONTROLLER_ACTION_NAVIGATE;
        snprintf(action->method, sizeof(action->method), "GET");
        engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_IDLE;
        return BROWSER_PREVIEW_ACTIVATION_NAVIGATE;
    }
    /* A control, a link within this page, or anything but a plain web
       link acts on the committed page. */
    memset(action, 0, sizeof(*action));
    engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_WAITING;
    (void) browser_engine_compose_provisional(engine, 0);
    return BROWSER_PREVIEW_ACTIVATION_DEFERRED;
}

bool browser_engine_take_deferred_activation(BrowserEngine *engine)
{
    if (engine == NULL || engine->provisional_activation
            != BROWSER_DEFERRED_ACTIVATION_READY) return false;
    engine->provisional_activation = BROWSER_DEFERRED_ACTIVATION_IDLE;
    return true;
}

bool browser_engine_provisional_raster_step(BrowserEngine *engine)
{
    if (engine == NULL
        || engine->navigation_work.status
               != BROWSER_NAVIGATION_JOB_PENDING
        || engine->provisional_view != BROWSER_PREVIEW_VIEW_FILLING)
        return false;
    /* A raster slice (as many tiles as fit), then one recompose. */
    if (!browser_engine_compose_provisional(engine, SIZE_MAX)) {
        /* Stop retrying a raster that failed; a scroll composes again. */
        if (engine->provisional_view == BROWSER_PREVIEW_VIEW_FILLING)
            engine->provisional_view = BROWSER_PREVIEW_VIEW_LIVE;
        return false;
    }
    return engine->provisional_view == BROWSER_PREVIEW_VIEW_FILLING;
}
