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
    /* Matches of a provisional layout are those laid out so far ("+"):
       the count refreshes when idle work completes it. */
    snapshot->truncated = engine->find.truncated
        || engine->navigation.page.layout_provisional;
    snapshot->wrapped = engine->find.wrapped;
}

void browser_engine_find_clear(BrowserEngine *engine)
{
    if (engine == NULL) return;
    page_find_clear(&engine->find);
    engine->find_navigation_generation = 0;
    engine->find_layout_generation = 0;
}

/* Rebuilds the index and selects the first match at or below the selected
   one's top, or the selected one itself when it kept its index and top (a
   line holding two matches, laid out further). */
static bool browser_engine_find_rebuild(BrowserEngine *engine,
                                        int nearest_y)
{
    char query[PAGE_FIND_QUERY_LIMIT + 1u];
    snprintf(query, sizeof(query), "%s", engine->find.query);
    size_t kept = engine->find.match_count != 0 ? engine->find.selected
                                                : SIZE_MAX;
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    if (engine->find.match_count != 0)
        (void) page_find_select_nearest(&engine->find, nearest_y);
    if (kept < engine->find.match_count
        && engine->find.matches[kept].top == nearest_y)
        engine->find.selected = kept;
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
   reads all of it, or with `through` one laid out only as far as the first
   of those nodes (navigation_layout_through). The render shell keeps tiles
   outside the damage. A bounded focus layout's completion waits for
   pending author mutations (navigation_complete_layout_step): they are
   published first, by the relayout their next runtime turn would run. */
static bool browser_engine_complete_layout(
    BrowserEngine *engine, const lxb_dom_node_t *const *through,
    size_t through_count)
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
        (void) (through_count != 0
            ? navigation_layout_through(&engine->navigation, through,
                                        through_count)
            : navigation_complete_layout_sync(&engine->navigation));
    if (engine->navigation.incremental_relayouts == before) return relaid;
    (void) controller_rebind_focus(&engine->controller);
    return browser_engine_apply_layout_damage(engine) && relaid;
}

/* Whether `match` is where the complete layout puts it, and so is the
   screen around it: in a provisional layout, drawn by commands recorded
   before the cut, with that screen above the cut and above any container
   the cut may have moved (navigation_exact_box's test). */
static bool browser_engine_find_match_exact(const BrowserEngine *engine,
                                            const PageFindMatch *match)
{
    const LayoutDocument *layout = &engine->navigation.page.layout;
    if (layout->content_limit_y == 0) return true;
    int64_t screen_end = (int64_t) (match->top > 0 ? match->top : 0)
        + engine->navigation.viewport.css_height;
    return match->end.command_index < layout->preview_cut_command_count
        && screen_end <= layout->content_limit_y
        && (!layout->preview_moved || screen_end <= layout->preview_moved_top);
}

/* The match the complete layout's index would select next, when the
   provisional index proves it: its matches before the cut are the
   complete index's first ones, in order. From the start (`direction` 0)
   that is the first at or below `document_y`, every one before it known to
   lie above; otherwise the selected match's neighbour. Wrapping needs the
   whole index. */
static bool browser_engine_find_proven(BrowserEngine *engine, int direction,
                                       int document_y)
{
    const PageFindIndex *find = &engine->find;
    if (!engine->navigation.page.layout_provisional) return true;
    if (direction != 0) {
        size_t next = direction > 0 ? find->selected + 1u
                                    : find->selected - 1u;
        return find->selected < find->match_count
            && (direction > 0 || find->selected != 0)
            && next < find->match_count
            && browser_engine_find_match_exact(engine, &find->matches[next]);
    }
    for (size_t i = 0; i < find->match_count; i++) {
        if (!browser_engine_find_match_exact(engine, &find->matches[i]))
            return false;
        if (find->matches[i].top >= document_y) return true;
    }
    return false;
}

/* The first text nodes after the cut where the query ends, reading the
   text in document order outside style and script as one run (a match may
   span inline elements): how far Find lays a provisional page out (as far
   as the first of them with a box, so text that is not displayed does not
   carry the layout to the end). A guide only: the index decides, and none
   seen completes the layout. Returns how many it stored. */
static size_t browser_engine_find_text_after_cut(
    BrowserEngine *engine, const lxb_dom_node_t *found[LAYOUT_THROUGH_TARGETS])
{
    const LayoutDocument *layout = &engine->navigation.page.layout;
    size_t cut = layout->preview_cut_box_count < layout->node_box_count
        ? layout->preview_cut_box_count : layout->node_box_count;
    lxb_dom_node_t *node = cut == 0 ? NULL : layout->node_boxes[cut - 1].node;
    bool skip = true;
    PageFindTextScan scan = {0};
    size_t count = 0;
    for (size_t visited = 0; node != NULL && visited < 65536u
                             && count < LAYOUT_THROUGH_TARGETS; visited++) {
        if (!skip && node->first_child != NULL) {
            node = node->first_child;
        } else {
            while (node != NULL && node->next == NULL) node = node->parent;
            if (node == NULL) break;
            node = node->next;
        }
        skip = node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && (node->local_name == LXB_TAG_STYLE
                || node->local_name == LXB_TAG_SCRIPT
                || node->local_name == LXB_TAG_TEMPLATE);
        if (node->type != LXB_DOM_NODE_TYPE_TEXT) continue;
        size_t length = 0;
        const char *text = document_text_data(node, &length);
        if (page_find_text_scan(engine->find.query, &scan, text, length))
            found[count++] = node;
    }
    return count;
}

/* Lays a provisional page out until the index proves the match Find moves
   to (browser_engine_find_proven): through the next text holding the
   query a few times, then completely. A completion refused for memory
   leaves the index of what is laid out, as before. */
static bool browser_engine_find_reach(BrowserEngine *engine, int direction,
                                      int document_y)
{
    for (unsigned round = 0;
         !browser_engine_find_proven(engine, direction, document_y);
         round++) {
        if (round == 4u) return true;
        const lxb_dom_node_t *through[LAYOUT_THROUGH_TARGETS] = {0};
        size_t through_count = direction >= 0 && round < 3u
            ? browser_engine_find_text_after_cut(engine, through) : 0;
        const PageFindMatch *selected = page_find_selected(&engine->find);
        int nearest_y = selected == NULL ? document_y : selected->top;
        if (!browser_engine_complete_layout(engine, through, through_count)
            || !browser_engine_find_rebuild(engine, nearest_y)) return false;
        if (direction == 0 && engine->find.match_count != 0)
            (void) page_find_select_nearest(&engine->find, document_y);
    }
    return true;
}

bool browser_engine_find_begin(BrowserEngine *engine, const char *query,
                               BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_input_ready(engine) || query == NULL) return false;
    /* Find matches every text run, including those below a placeholder:
       never answered from a bounded focus layout, which is settled first
       (refused when it cannot be). A scriptless provisional layout is laid
       out as far as the match it selects (browser_engine_find_reach). */
    if (!browser_engine_settle_bounded_layout(engine)
        || (engine->navigation.page.runtime != NULL
            && !browser_engine_complete_layout(engine, NULL, 0))) return false;
    browser_engine_find_clear(engine);
    if (!page_find_build(
            &engine->find, &engine->budget,
            &engine->navigation.page.layout, query)) return false;
    engine->find_navigation_generation = engine->navigation.generation;
    engine->find_layout_generation = engine->navigation.incremental_relayouts;
    const NavigationEntry *entry = navigation_current(&engine->navigation);
    int scroll_y = entry == NULL ? 0 : entry->scroll_y;
    if (engine->find.match_count != 0)
        (void) page_find_select_nearest(&engine->find, scroll_y);
    if (!browser_engine_find_reach(engine, 0, scroll_y)) return false;
    if (engine->find.match_count != 0)
        (void) browser_engine_find_jump(engine);
    browser_engine_find_copy_snapshot(engine, snapshot);
    clear_error(engine);
    return true;
}

bool browser_engine_find_move(BrowserEngine *engine, int direction,
                              BrowserFindSnapshot *snapshot)
{
    if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
    if (!browser_engine_find_refresh(engine) || direction == 0
        || !browser_engine_find_reach(engine, direction, 0)
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
