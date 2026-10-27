/* Find in page and its highlights, plus the text anchors that keep a
   reading position across a view switch. */

static void browser_engine_find_copy_snapshot(
    const BrowserEngine *engine, BrowserFindSnapshot *snapshot)
{
    if (snapshot == NULL) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (engine == NULL || engine->find.query[0] == '\0') return;
    snprintf(snapshot->query, sizeof(snapshot->query), "%s",
             engine->find.query);
    snapshot->match_count = engine->find.match_count;
    snapshot->selected = engine->find.selected;
    snapshot->truncated = engine->find.truncated;
    snapshot->wrapped = engine->find.wrapped;
}

void browser_engine_find_clear(BrowserEngine *engine)
{
    if (engine == NULL) return;
    page_find_clear(&engine->find);
    engine->find_navigation_generation = 0;
    engine->find_layout_generation = 0;
}

static bool browser_engine_find_rebuild(BrowserEngine *engine,
                                        int nearest_y)
{
    char query[PAGE_FIND_QUERY_LIMIT + 1u];
    snprintf(query, sizeof(query), "%s", engine->find.query);
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    if (engine->find.match_count != 0)
        (void) page_find_select_nearest(&engine->find, nearest_y);
    return true;
}

static bool browser_engine_find_refresh(BrowserEngine *engine)
{
    if (engine == NULL || engine->find.query[0] == '\0') return false;
    if (!browser_engine_input_ready(engine)
        || engine->find_navigation_generation
               != engine->navigation.generation) {
        browser_engine_find_clear(engine);
        return false;
    }
    if (engine->find_layout_generation
            == engine->navigation.incremental_relayouts) return true;
    /* Matches span the whole page: never refresh them from a bounded
       focus layout. */
    if (!browser_engine_settle_bounded_layout(engine)) return false;
    if (engine->find_layout_generation
            == engine->navigation.incremental_relayouts) return true;
    const PageFindMatch *selected = page_find_selected(&engine->find);
    int nearest_y = selected == NULL ? 0 : selected->top;
    if (page_find_refresh_geometry(
            &engine->find, &engine->navigation.page.layout)) {
        engine->find_layout_generation =
            engine->navigation.incremental_relayouts;
        return true;
    }
    if (!browser_engine_find_rebuild(engine, nearest_y)) {
        browser_engine_find_clear(engine);
        return false;
    }
    return true;
}

static bool browser_engine_find_jump(BrowserEngine *engine)
{
    const PageFindMatch *match = page_find_selected(&engine->find);
    if (match == NULL) return false;
    if (match->fixed) return true;
    int context = engine->navigation.viewport.css_height / 3;
    int target = match->top > context ? match->top - context : 0;
    browser_engine_cancel_idle_work(engine);
    return navigation_set_scroll(&engine->navigation, target);
}

/* Adopts the complete layout of a provisional page now, for a caller that
   reads all of it. The render shell keeps tiles outside the damage. A
   bounded focus layout's completion waits for pending author mutations
   (navigation_complete_layout_step): they are published first, by the
   relayout their next runtime turn would run. */
static bool browser_engine_complete_layout(BrowserEngine *engine)
{
    /* Not gated by the idle retry backoff: this caller needs it now. */
    if (!engine->navigation.page.loaded
        || !engine->navigation.page.layout_provisional)
        return true;
    size_t before = engine->navigation.incremental_relayouts;
    ScriptRuntime *runtime = engine->navigation.page.runtime;
    bool relaid = true;
    if (navigation_bounded_focus_region_end(&engine->navigation) != 0
        && runtime != NULL && script_runtime_has_pending_mutations(runtime)) {
        (void) script_runtime_consume_relayout(runtime);
        relaid = navigation_relayout(&engine->navigation);
    }
    if (relaid && engine->navigation.page.loaded
        && engine->navigation.page.layout_provisional)
        (void) navigation_complete_layout_sync(&engine->navigation);
    if (engine->navigation.incremental_relayouts == before) return relaid;
    (void) controller_rebind_focus(&engine->controller);
    return browser_engine_apply_layout_damage(engine) && relaid;
}

bool browser_engine_find_begin(BrowserEngine *engine, const char *query,
                               BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_input_ready(engine) || query == NULL) return false;
    /* Find matches every text run, including those below a placeholder:
       never answered from a bounded focus layout, which is settled first
       (refused when it cannot be). A scriptless provisional layout is
       completed when it can be. */
    if (!browser_engine_settle_bounded_layout(engine)
        || !browser_engine_complete_layout(engine)) return false;
    browser_engine_find_clear(engine);
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    if (engine->find.match_count != 0) {
        (void) page_find_select_nearest(&engine->find, scroll_y);
        (void) browser_engine_find_jump(engine);
    }
    browser_engine_find_copy_snapshot(engine, snapshot);
    clear_error(engine);
    return true;
}

bool browser_engine_find_move(BrowserEngine *engine, int direction,
                              BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_find_refresh(engine) || direction == 0
        || !page_find_move(&engine->find, direction)) return false;
    (void) browser_engine_find_jump(engine);
    browser_engine_find_copy_snapshot(engine, snapshot);
    clear_error(engine);
    return true;
}

bool browser_engine_find_snapshot(BrowserEngine *engine,
                                  BrowserFindSnapshot *snapshot)
{
    if (snapshot == NULL) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_find_refresh(engine)) return false;
    browser_engine_find_copy_snapshot(engine, snapshot);
    return true;
}

bool browser_engine_capture_text_anchor(
    const BrowserEngine *engine,
    char anchor[BROWSER_TEXT_ANCHOR_LIMIT + 1u], int *document_y)
{
    if (anchor != NULL) anchor[0] = '\0';
    if (!browser_engine_input_ready(engine) || anchor == NULL) return false;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    /* A bounded focus layout is const here: answer only from its region,
       where its commands are the complete layout's own and come first. */
    int region_end = navigation_bounded_focus_region_end(&engine->navigation);
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (region_end != 0 && command->y >= region_end) return false;
        if (command->type != DRAW_TEXT || command->text == NULL
            || command->text_length < 4u
            || draw_command_is_text_shadow(command)
            || command->y + command->height < scroll_y) continue;
        size_t length = command->text_length;
        if (length > BROWSER_TEXT_ANCHOR_LIMIT)
            length = BROWSER_TEXT_ANCHOR_LIMIT;
        while (length != 0 && length < command->text_length
               && (((unsigned char) command->text[length] & 0xc0u)
                       == 0x80u)) length--;
        memcpy(anchor, command->text, length);
        anchor[length] = '\0';
        if (document_y != NULL) *document_y = command->y;
        return length >= 4u;
    }
    return false;
}

bool browser_engine_restore_text_anchor(
    BrowserEngine *engine, const char *anchor, int nearest_y)
{
    if (!browser_engine_input_ready(engine) || anchor == NULL
        || anchor[0] == '\0'
        || !browser_engine_settle_bounded_layout(engine)) return false;
    PageFindIndex temporary = {0};
    bool restored = page_find_build(
        &temporary, &engine->budget, &engine->navigation.page.layout,
        anchor);
    if (restored && temporary.match_count != 0) {
        (void) page_find_select_nearest(&temporary, nearest_y);
        const PageFindMatch *match = page_find_selected(&temporary);
        restored = match != NULL
            && (match->fixed
                || navigation_set_scroll(&engine->navigation, match->top));
    } else {
        restored = false;
    }
    page_find_clear(&temporary);
    return restored;
}

static void browser_engine_paint_find_highlights(BrowserEngine *engine,
                                                 int scroll_y)
{
    if (!browser_engine_find_refresh(engine)
        || engine->find.match_count == 0) return;
    const LayoutDocument *layout = &engine->navigation.page.layout;
    const ViewportContext *viewport = &engine->navigation.viewport;
    int viewport_width = engine->config.device.framebuffer_width;
    int viewport_height = engine->config.device.framebuffer_height;
    int visible_top = scroll_y;
    int visible_bottom = scroll_y + viewport->css_height;
    for (size_t match_index = 0;
         match_index < engine->find.match_count; match_index++) {
        const PageFindMatch *match = &engine->find.matches[match_index];
        if (!match->fixed
            && (match->bottom < visible_top || match->top > visible_bottom))
            continue;
        PageFindRect rects[PAGE_FIND_RECT_LIMIT];
        size_t count = page_find_match_rects(
            &engine->find, layout, match_index,
            rects, PAGE_FIND_RECT_LIMIT);
        for (size_t i = 0; i < count; i++) {
            int css_y = rects[i].fixed ? rects[i].y
                                      : rects[i].y - scroll_y;
            int left = viewport_css_to_device(viewport, rects[i].x);
            int top = viewport_css_to_device(viewport, css_y);
            int right = viewport_css_to_device(
                viewport, rects[i].x + rects[i].width);
            int bottom = viewport_css_to_device(
                viewport, css_y + rects[i].height);
            render_paint_find_highlight(
                engine->render.frame, engine->render.frame_pixels,
                viewport_width, viewport_height,
                left, top, right - left, bottom - top,
                match_index == engine->find.selected);
        }
    }
}
