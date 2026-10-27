/* Provisional preview painting: the preview tile cache, the focus
   indicator, composing and capturing the provisional viewport, and the
   progressive preview paint. Its public scroll, focus and activation calls
   are in provisional_input.inc. */

/* Enough tiles for one screen at any scroll position: one row more than
   the viewport spans when it starts mid-tile. */
static size_t browser_engine_provisional_tile_capacity(
    size_t width, size_t height)
{
    size_t columns = (width + TILEFINCH_TILE_SIZE - 1u) / TILEFINCH_TILE_SIZE;
    size_t rows = (height + TILEFINCH_TILE_SIZE - 1u) / TILEFINCH_TILE_SIZE;
    return columns * (rows + 1u);
}

static int browser_engine_provisional_viewport_height(
    const BrowserEngine *engine, const LayoutDocument *layout)
{
    return layout != NULL && layout->viewport.css_height > 0
        ? layout->viewport.css_height
        : engine->config.device.framebuffer_height;
}

static int browser_engine_provisional_clamp(
    const BrowserEngine *engine, const LayoutDocument *layout, int *maximum)
{
    /* The pending document's viewport; the session's is the incumbent's
       (or unset before a first page). */
    int viewport_height =
        browser_engine_provisional_viewport_height(engine, layout);
    int limit = layout->height > viewport_height
        ? layout->height - viewport_height : 0;
    if (maximum != NULL) *maximum = limit;
    int y = engine->provisional_scroll_y;
    return y < 0 ? 0 : (y > limit ? limit : y);
}

/* The front cache is usable only while the stream still offers the layout
   it was built over; re-resolved image pointers invalidate what it derived
   from them (scaled clone, decoded-image cache). */
static TileCache *browser_engine_provisional_front(BrowserEngine *engine)
{
    if (!browser_preview_view_live(engine->provisional_view)) return NULL;
    TileCache *front =
        &engine->provisional_renders[engine->provisional_front];
    bool images_changed = false;
    const LayoutDocument *layout = navigation_load_retained_preview(
        engine->navigation_work.load, &images_changed);
    if (layout == NULL || layout != engine->provisional_layout) {
        browser_engine_drop_provisional_front(engine);
        return NULL;
    }
    if (images_changed) {
        size_t capacity = front->tile_capacity;
        tile_cache_destroy(front);
        memset(front, 0, sizeof(*front));
        if (!browser_engine_init_tile_cache(
                engine, front, layout, capacity)) {
            memset(front, 0, sizeof(*front));
            browser_engine_drop_provisional_front(engine);
            return NULL;
        }
        tile_cache_set_fast_text_raster(front, true);
    }
    return front;
}

/* Keep a focus indicator inside the padding boxes of the ancestors that
   clip its control (overflow hidden/clip), as the control itself is.
   chatgpt.com's composer textarea overflows its rounded form, which clips
   it; the indicator ran past the pill to the screen edge. A control clipped
   away entirely keeps its unclipped indicator so focus is never invisible. */
static void browser_engine_clip_focus_rect(
    const LayoutDocument *layout, const lxb_dom_node_t *node,
    int *x, int *y, int *width, int *height)
{
    if (layout == NULL || node == NULL) return;
    int left = *x, top = *y;
    int right = *x + *width, bottom = *y + *height;
    size_t steps = 0;
    for (const lxb_dom_node_t *at = node->parent;
         at != NULL && steps < 256u; at = at->parent, steps++) {
        const LayoutNodeBox *box = layout_box_for_node(layout, at);
        if (box == NULL || (!box->clips_x && !box->clips_y)) continue;
        if (box->clips_x) {
            int clip_left = box->x + box->clip_inset_left;
            int clip_right = clip_left + box->client_width;
            if (left < clip_left) left = clip_left;
            if (right > clip_right) right = clip_right;
        }
        if (box->clips_y) {
            int clip_top = box->y + box->clip_inset_top;
            int clip_bottom = clip_top + box->client_height;
            if (top < clip_top) top = clip_top;
            if (bottom > clip_bottom) bottom = clip_bottom;
        }
    }
    if (right <= left || bottom <= top) return;
    *x = left;
    *y = top;
    *width = right - left;
    *height = bottom - top;
}

bool browser_engine_focus_indicator_rect(const BrowserEngine *engine,
                                         int *x, int *y,
                                         int *width, int *height)
{
    if (engine == NULL || x == NULL || y == NULL || width == NULL
        || height == NULL
        || !controller_focused_rect(&engine->controller, x, y,
                                    width, height)) return false;
    browser_engine_clip_focus_rect(
        &engine->navigation.page.layout,
        controller_focused_node(&engine->controller), x, y, width, height);
    return true;
}

/* A node's first link region, else its first control region, in CSS page
   coordinates. */
static bool browser_engine_preview_region(
    const LayoutDocument *layout, const lxb_dom_node_t *node,
    int *x, int *y, int *width, int *height, const LinkRegion **link)
{
    if (link != NULL) *link = NULL;
    if (layout == NULL || node == NULL) return false;
    for (size_t i = 0; i < layout->link_count; i++) {
        const LinkRegion *region = &layout->links[i];
        if (region->node != node) continue;
        *x = region->x; *y = region->y;
        *width = region->width; *height = region->height;
        if (link != NULL) *link = region;
        return true;
    }
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *region = &layout->controls[i];
        if (region->node != node) continue;
        *x = region->x; *y = region->y;
        *width = region->width; *height = region->height;
        return true;
    }
    return false;
}

static void browser_engine_paint_provisional_focus(
    BrowserEngine *engine, const LayoutDocument *layout, int scroll_y)
{
    int x = 0, y = 0, width = 0, height = 0;
    if (engine->provisional_focus_node == NULL
        || !browser_engine_preview_region(
               layout, engine->provisional_focus_node,
               &x, &y, &width, &height, NULL)) return;
    const ViewportContext *viewport = &layout->viewport;
    render_paint_focus_outline(
        engine->provisional_frame, engine->provisional_frame_pixels,
        engine->config.device.framebuffer_width,
        engine->config.device.framebuffer_height,
        viewport_css_to_device(viewport, x),
        viewport_css_to_device(viewport, y - scroll_y),
        viewport_css_to_device(viewport, width),
        viewport_css_to_device(viewport, height));
}

/* Compose the preview frame at the reader's position from resident tiles,
   a checkerboard where one is missing, rasterizing at most raster_units
   first, and present it. */
static bool browser_engine_compose_provisional(
    BrowserEngine *engine, size_t raster_units)
{
    if (engine->provisional_view == BROWSER_PREVIEW_VIEW_NONE
        || engine->provisional_frame == NULL)
        return false;
    TileCache *front = browser_engine_provisional_front(engine);
    if (front == NULL) return false;
    int width = engine->config.device.framebuffer_width;
    int height = engine->config.device.framebuffer_height;
    int y = browser_engine_provisional_clamp(
        engine, engine->provisional_layout,
        &engine->provisional_maximum_scroll);
    engine->provisional_scroll_y = y;
    RenderFrameWorkResult prepared = tile_cache_prepare_frame_bounded(
        front, y, width, height,
        raster_units == 0 ? 0 : BROWSER_PROVISIONAL_RASTER_SLICE_US,
        raster_units);
    if (prepared == RENDER_FRAME_WORK_FAILED) return false;
    bool complete = prepared == RENDER_FRAME_WORK_READY;
    front->placeholder_missing = !complete;
    bool rendered = tile_cache_set_frame(
            front, engine->provisional_frame,
            engine->provisional_frame_pixels)
        && tile_cache_render_frame(front, y, width, height, NULL);
    front->placeholder_missing = false;
    if (!rendered) return false;
    browser_engine_paint_provisional_focus(
        engine, engine->provisional_layout, y);
    engine->provisional_view = complete
        ? BROWSER_PREVIEW_VIEW_LIVE : BROWSER_PREVIEW_VIEW_FILLING;
    (void) tilefinch_platform_present_rgb565(
        engine->provisional_frame, (size_t) width, (size_t) height,
        (size_t) width);
    return true;
}

/* Reject a first frame that is one flat surface or a lone border or
   placeholder row, structurally rather than by a pixel count: a line of
   text or a logo marks several scanlines, a rule marks one or two. (A
   count of two scanlines' worth of pixels rejected a page header logo at
   929 of 960 and accepted it at 960 on another run, so first paint flipped
   between about 4 s and the end of the load.) */
static bool browser_engine_provisional_frame_has_contrast(
    const uint16_t *frame, size_t width, size_t height)
{
    const size_t required_rows = 6u;
    const size_t required_pixels = width / 2u;
    size_t contrasting = 0, contrasting_rows = 0;
    uint16_t background = frame[0];
    for (size_t row = 0; row < height
         && (contrasting_rows < required_rows
             || contrasting < required_pixels); row++) {
        const uint16_t *line = frame + row * width;
        size_t before = contrasting;
        for (size_t x = 0; x < width; x++) {
            if (line[x] != background) contrasting++;
        }
        if (contrasting != before) contrasting_rows++;
    }
    return contrasting_rows >= required_rows
        && contrasting >= required_pixels;
}

static bool browser_engine_capture_provisional_viewport(
    BrowserEngine *engine, NavigationSession *candidate,
    const LayoutDocument *layout, size_t width, size_t height)
{
    if (engine == NULL || candidate == NULL || layout == NULL
        || width == 0 || height == 0 || width > INT_MAX
        || height > INT_MAX || width > SIZE_MAX / height) return false;
    BrowserEngineNavigationWork *work = &engine->navigation_work;
    if (work->metrics.provisional_capture_started_us == 0) {
        uint64_t now_us = tilefinch_platform_monotonic_time_us();
        work->metrics.provisional_capture_started_us =
            now_us >= work->started_us ? now_us - work->started_us : 0;
    }
    size_t pixels = width * height;
    if (pixels > SIZE_MAX / sizeof(uint16_t)) return false;
    const size_t frame_bytes = pixels * sizeof(uint16_t);
    bool retains = candidate->progressive_paint_retains;
    bool first = engine->provisional_view == BROWSER_PREVIEW_VIEW_NONE;
    if (engine->provisional_frame == NULL) {
        if (budget_pressure_required(
                &engine->budget, frame_bytes,
                BROWSER_PROVISIONAL_FINAL_RESERVE)) {
            budget_record_pressure(
                &engine->budget, BUDGET_PRESSURE_SPECULATION,
                frame_bytes, 0);
            return false;
        }
        uint64_t alloc_started_us = tilefinch_platform_monotonic_time_us();
        engine->provisional_frame = budget_malloc_category(
            &engine->budget, BUDGET_CATEGORY_RENDER, frame_bytes);
        uint64_t alloc_finished_us = tilefinch_platform_monotonic_time_us();
        if (alloc_finished_us >= alloc_started_us) {
            candidate->performance.streaming_preview_frame_alloc_us +=
                alloc_finished_us - alloc_started_us;
        }
        if (engine->provisional_frame == NULL) return false;
        engine->provisional_frame_pixels = pixels;
    }
    size_t capacity =
        browser_engine_provisional_tile_capacity(width, height);
    /* Two caches only when there is room for both; otherwise give up
       scrolling the preview on screen while its successor rasterizes. */
    if (browser_preview_view_live(engine->provisional_view)
        && budget_pressure_required(
               &engine->budget, capacity * sizeof(RenderTile),
               BROWSER_PROVISIONAL_FINAL_RESERVE)) {
        browser_engine_drop_provisional_render(engine);
    }
    unsigned back_index = browser_preview_view_live(engine->provisional_view)
        ? engine->provisional_front ^ 1u : 0u;
    TileCache *back = &engine->provisional_renders[back_index];
    if (back->budget != NULL) tile_cache_destroy(back);
    memset(back, 0, sizeof(*back));
    uint64_t cache_started_us = tilefinch_platform_monotonic_time_us();
    if (!browser_engine_init_tile_cache(engine, back, layout, capacity)) {
        memset(back, 0, sizeof(*back));
        if (first) browser_engine_reset_provisional_viewport(engine);
        return false;
    }
    uint64_t cache_ready_us = tilefinch_platform_monotonic_time_us();
    if (cache_ready_us >= cache_started_us) {
        candidate->performance.streaming_preview_cache_init_us +=
            cache_ready_us - cache_started_us;
    }
    tile_cache_set_fast_text_raster(back, true);
    /* Rasterize off screen in bounded slices; between them the frontend
       keeps scrolling (and filling) the preview already on screen, and each
       slice follows the reader's latest position. */
    uint64_t raster_started_us = tilefinch_platform_monotonic_time_us();
    RenderFrameWorkResult prepared = RENDER_FRAME_WORK_PENDING;
    int y = 0;
    for (size_t slice = 1; ; slice++) {
        y = browser_engine_provisional_clamp(engine, layout, NULL);
        prepared = tile_cache_prepare_frame_bounded(
            back, y, (int) width, (int) height,
            BROWSER_PROVISIONAL_RASTER_SLICE_US, SIZE_MAX);
        if (prepared != RENDER_FRAME_WORK_PENDING) break;
        if (!tilefinch_platform_cooperate("provisional-raster", slice)) {
            prepared = RENDER_FRAME_WORK_CANCELLED;
            break;
        }
    }
    uint64_t raster_finished_us = tilefinch_platform_monotonic_time_us();
    if (raster_finished_us >= raster_started_us) {
        candidate->performance.streaming_preview_raster_us +=
            raster_finished_us - raster_started_us;
    }
    candidate->performance.progressive_decoded_image_builds +=
        back->decoded_image_builds;
    candidate->performance.progressive_scaled_image_builds +=
        back->scaled_image_builds;
    bool composed = prepared == RENDER_FRAME_WORK_READY
        && tile_cache_set_frame(
               back, engine->provisional_frame, pixels)
        && tile_cache_render_frame(
               back, y, (int) width, (int) height, NULL);
    /* A structurally nonempty body can still rasterize as one flat
       background while its first visible content remains in a later
       response chunk. Publishing that frame would satisfy a timer without
       giving the user a page; the stream retries at a later checkpoint. */
    if (composed && first
        && !browser_engine_provisional_frame_has_contrast(
               engine->provisional_frame, width, height)) {
        candidate->performance.streaming_preview_empty_raster_skips++;
        composed = false;
    }
    if (!composed) {
        tile_cache_destroy(back);
        memset(back, 0, sizeof(*back));
        if (first) {
            browser_engine_reset_provisional_viewport(engine);
        } else {
            /* The frame may hold the rejected composition. */
            (void) browser_engine_compose_provisional(engine, 0);
        }
        return false;
    }
    browser_engine_paint_provisional_focus(engine, layout, y);
    __sync_synchronize();
    if (engine->provisional_view == BROWSER_PREVIEW_VIEW_NONE)
        engine->provisional_view = BROWSER_PREVIEW_VIEW_FRAME;
    engine->provisional_scroll_y = y;
    if (retains) {
        if (browser_preview_view_live(engine->provisional_view)) {
            TileCache *old =
                &engine->provisional_renders[engine->provisional_front];
            tile_cache_destroy(old);
            memset(old, 0, sizeof(*old));
        }
        engine->provisional_front = back_index;
        engine->provisional_layout = layout;
        engine->provisional_view = BROWSER_PREVIEW_VIEW_LIVE;
        engine->provisional_content_end = layout->content_limit_y != 0
            ? layout->content_limit_y : layout->height;
        engine->provisional_viewport_height =
            browser_engine_provisional_viewport_height(engine, layout);
        (void) browser_engine_provisional_clamp(
            engine, layout, &engine->provisional_maximum_scroll);
    } else {
        tile_cache_destroy(back);
        memset(back, 0, sizeof(*back));
    }
    work->metrics.provisional_paints++;
    work->metrics.provisional_bytes =
        budget_usable_size(engine->provisional_frame);
    uint64_t now_us = tilefinch_platform_monotonic_time_us();
    uint64_t relative_us = now_us >= work->started_us
        ? now_us - work->started_us : 0;
    if (work->metrics.provisional_first_present_us == 0)
        work->metrics.provisional_first_present_us = relative_us;
    int reach = layout->content_limit_y != 0
        ? layout->content_limit_y : layout->height;
    if (reach > work->metrics.provisional_reach_px) {
        work->metrics.provisional_reach_px = reach;
        work->metrics.provisional_reach_us = relative_us;
    }
    uint64_t present_started_us = now_us;
    (void) tilefinch_platform_present_rgb565(
        engine->provisional_frame, width, height, width);
    uint64_t present_finished_us = tilefinch_platform_monotonic_time_us();
    if (present_finished_us >= present_started_us) {
        candidate->performance.streaming_preview_present_us +=
            present_finished_us - present_started_us;
    }
    return true;
}

static bool browser_engine_paint_progressive_preview(
    void *opaque, NavigationSession *candidate,
    const LayoutDocument *layout)
{
    BrowserEngine *engine = opaque;
    if (engine == NULL || candidate == NULL
        || candidate->budget != &engine->budget
        || engine->config.device.framebuffer_width <= 0
        || engine->config.device.framebuffer_height <= 0) return false;
    size_t width = (size_t) engine->config.device.framebuffer_width;
    size_t height = (size_t) engine->config.device.framebuffer_height;
    if (width > SIZE_MAX / height
        || width * height > SIZE_MAX / sizeof(uint16_t)) return false;
    size_t pixels = width * height;
    bool asynchronous =
        engine->navigation_work.status == BROWSER_NAVIGATION_JOB_PENDING;
    if (asynchronous) {
        if (layout == NULL) {
            browser_engine_reset_provisional_viewport(engine);
            browser_engine_abort_candidate_shell(engine);
            return true;
        }
        return browser_engine_capture_provisional_viewport(
            engine, candidate, layout, width, height);
    }
    if (layout == NULL) {
        /* A failed cold-start candidate has no incumbent frame to restore.
           Clear its already-presented preview immediately; the frontend can
           then overlay the navigation error without retaining dead content. */
        if (engine->framebuffer != NULL
            && engine->framebuffer_pixels >= pixels) {
            for (size_t i = 0; i < pixels; i++)
                engine->framebuffer[i] = UINT16_C(0xffff);
            (void) tilefinch_platform_present_rgb565(
                engine->framebuffer, width, height, width);
        }
        browser_engine_abort_candidate_shell(engine);
        return true;
    }
    bool transient_preview =
        candidate->performance.partial_layouts != 0;
    if (!transient_preview
        && engine->config.tile_capacity != 0 && engine->framebuffer != NULL
        && engine->framebuffer_pixels >= pixels) {
        browser_engine_abort_candidate_shell(engine);
        bool painted = browser_engine_init_tile_cache(
                engine, &engine->candidate_render, layout,
                engine->config.tile_capacity)
            && tile_cache_set_frame(
                &engine->candidate_render, engine->framebuffer,
                engine->framebuffer_pixels)
            && tile_cache_render_frame(
                &engine->candidate_render, 0, (int) width, (int) height,
                NULL);
        if (!painted) {
            browser_engine_abort_candidate_shell(engine);
            return false;
        }
        engine->candidate_render_ready = true;
        (void) tilefinch_platform_present_rgb565(
            engine->framebuffer, width, height, width);
        /* Presentation is the first-paint boundary. Paint-only preparation
           below must not move this timestamp merely because the browser is
           not interactive yet. */
        if (candidate->performance.first_paint_us == 0
            && candidate->navigation_started_us != 0) {
            uint64_t now_us = tilefinch_platform_monotonic_time_us();
            candidate->performance.first_paint_us =
                now_us >= candidate->navigation_started_us
                ? now_us - candidate->navigation_started_us : 0;
        }
        int prefetch_y = candidate->viewport.css_height
                         + TILEFINCH_TILE_SIZE - 1;
        if (prefetch_y < layout->height) {
            tile_cache_prepare_startup_visuals(
                &engine->candidate_render, prefetch_y, (int) width,
                3000, 4);
        }
        candidate->performance.progressive_decoded_image_builds +=
            engine->candidate_render.decoded_image_builds;
        candidate->performance.progressive_scaled_image_builds +=
            engine->candidate_render.scaled_image_builds;
        return true;
    }
    if (transient_preview && engine->framebuffer != NULL
        && engine->framebuffer_pixels >= pixels) {
        size_t capacity = engine->config.tile_capacity;
        if (capacity == 0) {
            capacity = engine->config.device.maximum_tile_capacity;
        }
        if (capacity > 8) capacity = 8;
        TileCache preview = {0};
        bool painted = capacity != 0
            && browser_engine_init_tile_cache(
                   engine, &preview, layout, capacity)
            && tile_cache_set_frame(
                   &preview, engine->framebuffer,
                   engine->framebuffer_pixels)
            && tile_cache_render_frame(
                   &preview, 0, (int) width, (int) height, NULL);
        if (painted) {
            candidate->performance.progressive_decoded_image_builds +=
                preview.decoded_image_builds;
            candidate->performance.progressive_scaled_image_builds +=
                preview.scaled_image_builds;
            (void) tilefinch_platform_present_rgb565(
                engine->framebuffer, width, height, width);
        }
        if (preview.budget != NULL) tile_cache_destroy(&preview);
        return painted;
    }
    uint16_t *frame = budget_malloc_category(
        &engine->budget, BUDGET_CATEGORY_RENDER,
        pixels * sizeof(*frame));
    if (frame == NULL) return false;
    size_t capacity = engine->config.tile_capacity;
    if (capacity == 0) capacity = engine->config.device.maximum_tile_capacity;
    if (capacity > 8) capacity = 8;
    TileCache preview = {0};
    bool painted = capacity != 0
        && browser_engine_init_tile_cache(
               engine, &preview, layout, capacity)
        && tile_cache_set_frame(&preview, frame, pixels)
        && tile_cache_render_frame(
               &preview, 0, (int) width, (int) height, NULL);
    if (painted) {
        candidate->performance.progressive_decoded_image_builds +=
            preview.decoded_image_builds;
        candidate->performance.progressive_scaled_image_builds +=
            preview.scaled_image_builds;
        /* A host lab without a presentation service still has a genuine
           raster-ready first paint.  PSP and platform-simulator frontends
           install this service and receive the preview immediately. */
        (void) tilefinch_platform_present_rgb565(
            frame, width, height, width);
    }
    if (preview.budget != NULL) tile_cache_destroy(&preview);
    budget_free(&engine->budget, frame);
    return painted;
}
