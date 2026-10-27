/* Background page work: image retargeting, web-font memory fallback,
   pending bytecode stores, network cancellation, deferred images and
   background resources. */

/* Identifies the committed layout and image table a retarget step judged. */
static bool navigation_image_retarget_current(const NavigationSession *session)
{
    const NavigationPage *page = &session->page;
    return page->image_retarget.settled
        && page->image_retarget.commands == page->layout.commands
        && page->image_retarget.count == page->layout.count
        && page->image_retarget.images == page->images.count
        && page->image_retarget.relayouts
               == session->incremental_relayouts
                  + session->performance.fast_relayouts
                  + session->performance.full_relayouts
        && page->image_retarget.loads == session->loads_committed;
}

static void navigation_image_retarget_settle(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    images_retarget_plan_discard(&page->images);
    page->image_retarget.commands = page->layout.commands;
    page->image_retarget.count = page->layout.count;
    page->image_retarget.images = page->images.count;
    page->image_retarget.relayouts = session->incremental_relayouts
        + session->performance.fast_relayouts
        + session->performance.full_relayouts;
    page->image_retarget.loads = session->loads_committed;
    page->image_retarget.settled = true;
    page->image_retarget.retries = 0;
}

/* Whether any decoded raster could change at all: canvas pages repaint
   (and publish layout generations) every frame, and must not pay a command
   scan per frame for nothing. */
static bool navigation_image_retarget_candidates(
    const NavigationSession *session)
{
    const ImageResources *images = &session->page.images;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *item = &images->items[i];
        if (!item->is_canvas && item->owns_encoded
            && item->encoded != NULL
            && (item->pixels != NULL
                || (item->retarget_flags & IMAGE_RETARGET_DROPPED) != 0)
            && (item->retarget_flags
                & (IMAGE_RETARGET_PINNED | IMAGE_RETARGET_REFUSED)) == 0)
            return true;
    }
    return false;
}

bool navigation_image_retarget_pending(const NavigationSession *session)
{
    return session != NULL && session->page.loaded
        && !session->page.image_continuation_pending
        && !session->page.visual_image_publication_pending
        && session->page.visual_batch_targets == NULL
        && session->page.images.count != 0
        && session->page.layout.count != 0
        && !navigation_image_retarget_current(session)
        && navigation_image_retarget_candidates(session);
}

static size_t navigation_web_font_reserve(const NavigationSession *session)
{
    size_t reserve = session->budget->limit / 8u;
    if (reserve < 512u * 1024u) reserve = 512u * 1024u;
    return reserve > 3u * 1024u * 1024u ? 3u * 1024u * 1024u : reserve;
}

static bool navigation_web_font_fallback_pending(const NavigationSession *session)
{
    return session->web_fonts_enabled && !session->page.web_font_fallback_attempted
        && (session->web_font_mode == WEB_FONTS_OFF
            || (session->web_font_mode == WEB_FONTS_SMART
                && (session->page.web_font_pressure_fallback
                    || budget_remaining(session->budget) < navigation_web_font_reserve(session))));
}

bool navigation_set_web_font_mode(NavigationSession *session, WebFontMode mode)
{
    if (session == NULL || mode < WEB_FONTS_ON || mode >= WEB_FONTS_MODE_COUNT) return false;
    if (session->web_font_mode == mode) return true;
    session->web_font_mode = mode;
    session->page.web_font_fallback_attempted = false;
    session->page.web_font_pressure_fallback = false;
    if (mode == WEB_FONTS_OFF) {
        fonts_external_loader_cancel(&session->page.external_font_loader,
            session->page.resource_scheduler, &session->page.external_fonts);
    } else if (session->page.loaded && session->web_fonts_enabled) {
        (void) navigation_load_web_fonts(session, session->page.document_url);
        navigation_attach_layout(session);
    }
    return true;
}

/* Retire page fonts only after a complete fallback layout can replace the
   incumbent. Refusal/cancellation restores faces and leaves the page intact.
   A pressure episode gets one attempt, not a retry on every idle frame. */
static TILEFINCH_OUT_OF_LINE bool navigation_fallback_web_fonts(NavigationSession *session)
{
    if (script_runtime_has_pending_mutations(session->page.runtime)) return true;
    session->page.web_font_fallback_attempted = true;
    /* Smart substitutes built-in text faces, but private-use icon runs have
       no portable built-in replacement. Keep their already-admitted families;
       explicit Off still drops every face. No new font is loaded here. */
    uint8_t icon_families = 0;
    if (session->web_font_mode == WEB_FONTS_SMART) {
        for (size_t i = 0; i < session->page.layout.count; i++) {
            const DrawCommand *command = &session->page.layout.commands[i];
            if (command->type != DRAW_TEXT || command->text == NULL
                || command->text_length > 4) continue;
            FontFamily family = draw_command_font_family(command);
            if (family < TILEFINCH_WEB_FONT_BASE) continue;
            unsigned slot = (family - TILEFINCH_WEB_FONT_BASE)
                / TILEFINCH_WEB_FONT_FAMILY_STRIDE;
            if (slot >= TILEFINCH_WEB_FONT_FAMILY_LIMIT) continue;
            unsigned codepoint = 0;
            if (font_utf8_next(command->text, command->text_length, &codepoint)
                    == command->text_length
                && ((codepoint >= 0xe000u && codepoint <= 0xf8ffu)
                    || (codepoint >= 0xf0000u && codepoint <= 0xffffdu)
                    || (codepoint >= 0x100000u && codepoint <= 0x10fffdu)))
                icon_families |= (uint8_t) (1u << slot);
        }
    }
    FontFace retained[TILEFINCH_WEB_FONT_FAMILY_LIMIT][TILEFINCH_WEB_FONT_WEIGHT_LIMIT] = {{{0}}};
    bool any = false;
    for (unsigned family = 0; family < TILEFINCH_WEB_FONT_FAMILY_LIMIT; family++) {
        for (unsigned slot = 0; slot < TILEFINCH_WEB_FONT_WEIGHT_LIMIT; slot++) {
            FontFace *face = stylesheet_web_font_face_at(&session->page.stylesheet, family, slot);
            if (face == NULL) continue;
            retained[family][slot] = *face;
            if ((icon_families & (1u << family)) != 0) continue;
            any |= face->loaded;
            memset(face, 0, sizeof(*face));
        }
    }
    if (!any) return true;
    int offset = 0;
    NavigationEntry *entry = session->history_count == 0 ? NULL : &session->history[session->history_index];
    const DrawCommand *anchor = navigation_scroll_text_anchor(&session->page.layout,
        entry == NULL ? 0 : entry->scroll_y, &offset);
    LayoutDocument next = {0};
    uint64_t started = navigation_now_us();
    layout_reuse_cache_begin_font_publication(session->page.layout_reuse);
    bool built = navigation_build_layout(session, &next, session->fonts, session->images);
    layout_reuse_cache_end_font_publication(session->page.layout_reuse);
    if (!built) {
        layout_destroy(&next);
        for (unsigned family = 0; family < TILEFINCH_WEB_FONT_FAMILY_LIMIT; family++) {
            for (unsigned slot = 0; slot < TILEFINCH_WEB_FONT_WEIGHT_LIMIT; slot++) {
                FontFace *face = stylesheet_web_font_face_at(&session->page.stylesheet, family, slot);
                if (face != NULL) *face = retained[family][slot];
            }
        }
        layout_reuse_cache_reset(session->page.layout_reuse);
        /* Input interruption is temporary. Retry at a later idle turn;
           allocation refusal is latched to avoid pressure thrashing. */
        session->page.web_font_fallback_attempted = !session->layout_build_cancelled;
        return false;
    }
    layout_transfer_scroll_state(&session->page.layout, &next);
    int scroll = navigation_scroll_for_text_anchor(&next, anchor, offset,
        entry == NULL ? 0 : entry->scroll_y);
    /* Equal advances do not imply equal glyph pixels. Both display lists
       borrow the same now-empty font set, so command equality cannot detect
       the old face. Invalidate every retained tile once on successful drop. */
    session->relayout_damage_valid = true;
    session->relayout_damage_left = session->relayout_damage_top = 0;
    session->relayout_damage_right = session->page.layout.scroll_width > next.scroll_width
        ? session->page.layout.scroll_width : next.scroll_width;
    if (session->relayout_damage_right < next.width) session->relayout_damage_right = next.width;
    session->relayout_damage_bottom = session->page.layout.height > next.height
        ? session->page.layout.height : next.height;
    layout_destroy(&session->page.layout);
    session->page.layout = next;
    session->page.external_fonts.retained_encoded_bytes = 0;
    for (unsigned family = 0; family < TILEFINCH_WEB_FONT_FAMILY_LIMIT; family++)
        for (unsigned slot = 0; slot < TILEFINCH_WEB_FONT_WEIGHT_LIMIT; slot++) {
            FontFace *face = &retained[family][slot];
            if ((icon_families & (1u << family)) == 0)
                font_face_destroy(face);
            else if (face->loaded)
                session->page.external_fonts.retained_encoded_bytes += face->data_length;
        }
    session->incremental_relayouts++;
    session->performance.fast_relayouts++;
    uint64_t elapsed = navigation_record_layout_slices(session, started, &next);
    session->performance.layout_us += elapsed;
    session->performance.relayout_us += elapsed;
    if (entry != NULL) entry->scroll_y = scroll;
    navigation_attach_layout(session);
    navigation_clamp_scroll_to_layout(session);
    return true;
}

NavigationBackgroundWorkOutcome navigation_run_web_font_memory_work(
    NavigationSession *session)
{
    if (session == NULL || !session->page.loaded)
        return NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
    if (session->web_font_mode == WEB_FONTS_SMART
        && fonts_external_loader_pending(&session->page.external_font_loader)) {
        size_t free = budget_remaining(session->budget);
        size_t reserve = navigation_web_font_reserve(session);
        size_t workspace = font_face_load_workspace_limit(session->maximum_font_face_backend_bytes);
        if (free < reserve || workspace > free - reserve
            || session->maximum_font_file_bytes > free - reserve - workspace)
            session->page.web_font_pressure_fallback = true;
    }
    if (!navigation_web_font_fallback_pending(session))
        return NAVIGATION_BACKGROUND_WORK_SUCCESS;
    session->page.web_font_pressure_fallback = session->web_font_mode == WEB_FONTS_SMART;
    fonts_external_loader_cancel(&session->page.external_font_loader,
        session->page.resource_scheduler, &session->page.external_fonts);
    return navigation_fallback_web_fonts(session)
        ? NAVIGATION_BACKGROUND_WORK_SUCCESS : NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL;
}

bool navigation_background_resources_pending(
    const NavigationSession *session)
{
    return session != NULL && session->page.loaded
        && (navigation_web_font_fallback_pending(session)
            || fonts_external_loader_pending(
                &session->page.external_font_loader)
            || session->page.image_continuation_pending
            || session->page.visual_image_publication_pending
            || session->page.visual_batch_targets != NULL);
}

/* One bounded display-retarget step once the page's image loading is done.
   A changed surface publishes an empty-damage layout generation, so the
   render shell drops scaled and decoded copies keyed by the old surface
   while every painted tile stays valid. */
bool navigation_run_image_retarget_work(NavigationSession *session)
{
    if (!navigation_image_retarget_pending(session)) return false;
    NavigationPage *page = &session->page;
    ImageRetargetOutcome step = images_retarget_display_step(
        &page->images, &page->layout, session->browser_session,
        session->incremental_relayouts + session->performance.fast_relayouts
            + session->performance.full_relayouts);
    switch (step) {
    case IMAGE_RETARGET_CHANGED:
        session->images = &page->images;
        navigation_ensure_layout_reuse(session);
        layout_reuse_cache_update_images(page->layout_reuse, &page->images);
        session->relayout_damage_valid = false;
        session->incremental_relayouts++;
        images_retarget_plan_rebase(&page->images,
            session->incremental_relayouts + session->performance.fast_relayouts
                + session->performance.full_relayouts);
        page->image_retarget.settled = false;
        break;
    case IMAGE_RETARGET_RETRY:
        if (++page->image_retarget.retries >= 8u) {
            navigation_image_retarget_settle(session);
        } else {
            page->image_retarget.settled = false;
        }
        break;
    case IMAGE_RETARGET_IDLE:
    case IMAGE_RETARGET_STALE:
    default:
        navigation_image_retarget_settle(session);
        break;
    }
    return step == IMAGE_RETARGET_CHANGED;
}

/* One deferred classic bytecode store (js_module_loader.c), for the page
   realm first and then its frames. */
static bool navigation_store_one_pending_bytecode(ScriptRuntime *runtime,
                                                  ScriptResult *result)
{
    return runtime != NULL
        && script_runtime_store_pending_bytecode(runtime, result);
}

bool navigation_store_pending_script_bytecode(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    if (navigation_store_one_pending_bytecode(session->page.runtime,
                                              &session->page.script_result))
        return true;
    for (size_t i = 0; i < session->page.frame_count; i++) {
        NavigationFrame *frame = &session->page.frames[i];
        if (navigation_store_one_pending_bytecode(frame->runtime,
                                                  &frame->script_result))
            return true;
    }
    return false;
}

bool navigation_script_bytecode_pending(const NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    if (script_runtime_pending_bytecode_count(session->page.runtime) != 0)
        return true;
    for (size_t i = 0; i < session->page.frame_count; i++)
        if (script_runtime_pending_bytecode_count(
                session->page.frames[i].runtime) != 0) return true;
    return false;
}

static void navigation_deferred_images_finish(NavigationSession *session);

size_t navigation_cancel_network_work(
    NavigationSession *session, const char *reason)
{
    if (session == NULL) return 0;
    size_t cancelled = fetch_scheduler_cancel_all(
        session->page.resource_scheduler, reason);
    cancelled += script_runtime_cancel_network(
        session->page.runtime, reason);
    for (size_t i = 0; i < session->page.frame_count; i++) {
        cancelled += script_runtime_cancel_network(
            session->page.frames[i].runtime, reason);
    }
    navigation_deferred_images_finish(session);
    navigation_destroy_linked_video_preview(session, &session->page);
    session->page.linked_video_preview_attempts = LINKED_VIDEO_PREVIEW_ATTEMPTS;
    return cancelled;
}

static void navigation_deferred_images_finish(NavigationSession *session)
{
    if (session == NULL) return;
    images_priority_load_destroy(session->page.deferred_image_job);
    session->page.deferred_image_job = NULL;
    session->page.deferred_image_batch_count = 0;
    session->page.deferred_image_publication_age = 0;
    budget_free(session->budget, session->page.deferred_image_targets);
    session->page.deferred_image_targets = NULL;
    session->page.deferred_image_count = 0;
    session->page.deferred_image_cursor = 0;
    session->page.deferred_image_rank_scroll_y = 0;
    session->page.deferred_image_rank_valid = false;
    session->page.image_continuation_pending = false;
}

static void navigation_deferred_image_failed(
    NavigationSession *session, ImagePriorityTarget *target)
{
    NavigationPage *page = &session->page;
    session->performance.background_image_failures++;
    if (target != NULL && target->retry_count == 0) {
        /* Eight owner-idle slices are long enough for a just-retired worker
           descriptor or short CDN hiccup to clear, yet do not hold input,
           layout, or the rest of the browser loop. The target stays current
           and is retried exactly once. */
        target->retry_count = 1;
        target->retry_delay = 8;
        return;
    }
    if (target != NULL) {
        target->retry_count = 0;
        target->retry_delay = 0;
    }
    page->deferred_image_cursor++;
}

static size_t navigation_deferred_image_available_prefix(
    const NavigationPage *page, size_t maximum)
{
    if (page == NULL || page->deferred_image_targets == NULL) return 0;
    size_t available = 0;
    while (available < maximum
           && page->deferred_image_cursor + available
                  < page->deferred_image_count) {
        const ImagePriorityTarget *target =
            &page->deferred_image_targets[
                page->deferred_image_cursor + available];
        if (!image_resource_available(
                images_find_node(&page->images, target->node))) break;
        available++;
    }
    return available;
}

bool navigation_prioritize_deferred_image(
    NavigationSession *session, lxb_dom_node_t *node)
{
    if (session == NULL || node == NULL) return false;
    NavigationPage *page = &session->page;
    size_t found = page->deferred_image_count;
    for (size_t at = page->deferred_image_cursor;
         at < page->deferred_image_count; at++) {
        if (page->deferred_image_targets[at].node == node) {
            found = at;
            break;
        }
    }
    if (found == page->deferred_image_count) return false;
    if (page->deferred_image_rank_valid
        && found >= page->deferred_image_eligible_end) return false;
    if (page->deferred_image_job != NULL
        && images_priority_load_contains_node(
               page->deferred_image_job, node)) {
        return true;
    }
    if (page->deferred_image_job != NULL) {
        /* Focus changes are latency-sensitive. Do not cancel the current
           transport/decode batch merely to prioritize a different row: job
           destruction synchronously cancels and takes its descriptors, rolls
           back optional image state, and throws away useful network progress.
           Preserve that admitted prefix and rotate the focused target to the
           first position after it instead. It will start as soon as the
           bounded one-or-two-image batch retires, while the input path remains
           allocation-free and transport-free. */
        size_t active_count = images_priority_load_target_count(
            page->deferred_image_job);
        if (active_count == 0) active_count = 1u;
        size_t promote_at = page->deferred_image_cursor;
        if (active_count <= page->deferred_image_count - promote_at) {
            promote_at += active_count;
        } else {
            promote_at = page->deferred_image_count;
        }
        if (found < promote_at) return true;
        if (found != promote_at) {
            ImagePriorityTarget focused = page->deferred_image_targets[found];
            size_t shifted = found - promote_at;
            memmove(
                &page->deferred_image_targets[promote_at + 1u],
                &page->deferred_image_targets[promote_at],
                shifted * sizeof(*page->deferred_image_targets));
            page->deferred_image_targets[promote_at] = focused;
        }
        const NavigationEntry *entry = navigation_current(session);
        page->deferred_image_rank_scroll_y = entry == NULL ? 0 : entry->scroll_y;
        return true;
    }
    if (found != page->deferred_image_cursor) {
        /* Focus is a temporary priority, not permission to shuffle every
           bystander. Rotate the focused target to the cursor so the natural
           viewport order of all remaining thumbnails stays stable. A swap
           teleported the old head to the focused row's former position and
           repeated d-pad moves could therefore strand the first card near
           the end of the queue. */
        ImagePriorityTarget focused = page->deferred_image_targets[found];
        size_t shifted = found - page->deferred_image_cursor;
        memmove(
            &page->deferred_image_targets[
                page->deferred_image_cursor + 1u],
            &page->deferred_image_targets[page->deferred_image_cursor],
            shifted * sizeof(*page->deferred_image_targets));
        page->deferred_image_targets[page->deferred_image_cursor] = focused;
    }
    /* Keep the explicit focus order until scrolling changes. Otherwise an
       all-visible result list can immediately swap the old first row back. */
    const NavigationEntry *entry = navigation_current(session);
    page->deferred_image_rank_scroll_y = entry == NULL ? 0 : entry->scroll_y;
    return true;
}

static uint64_t navigation_deferred_image_distance(
    const NavigationSession *session, const ImagePriorityTarget *target,
    int scroll_y, uint16_t *display_width, uint16_t *display_height)
{
    if (display_width != NULL) *display_width = target->display_width;
    if (display_height != NULL) *display_height = target->display_height;
    int64_t top = scroll_y;
    int64_t bottom = top + session->viewport.css_height;
    const LayoutNodeBox *box = layout_box_for_node(&session->page.layout, target->node);
    if (box != NULL && box->width > 0 && box->height > 0) {
        int width = layout_css_to_visual(&session->page.layout, box->width);
        int height = layout_css_to_visual(&session->page.layout, box->height);
        if (display_width != NULL && width > 0 && width <= UINT16_MAX) {
            *display_width = (uint16_t) width;
        }
        if (display_height != NULL && height > 0 && height <= UINT16_MAX) {
            *display_height = (uint16_t) height;
        }
        int64_t box_top = box->y;
        int64_t box_bottom = box_top + box->height;
        if (box_bottom < top) return (uint64_t) (top - box_bottom);
        if (box_top > bottom) return (uint64_t) (box_top - bottom);
        return 0;
    }
    return UINT64_MAX;
}

static bool navigation_deferred_target_live(
    NavigationPage *page, const ImagePriorityTarget *target)
{
    return target->weak_handle == 0
        || (page->runtime != NULL
            && script_runtime_node_handle_resolve_connected(
                   page->runtime, target->weak_handle) == target->node);
}

/* A missing node box is not a visibility test (inline replaced elements
   need not have one). Resolve the bounded ancestor chain using the layout
   style cache instead, and never inspect a retired weak target. */
bool navigation_image_target_visible(
    NavigationSession *session, const ImagePriorityTarget *target,
    bool painted_only)
{
    if (session == NULL || target == NULL || target->node == NULL) return false;
    if (!navigation_deferred_target_live(&session->page, target)) return false;
    lxb_dom_node_t *ancestors[64];
    size_t count = 0;
    for (lxb_dom_node_t *node = target->node; node != NULL; node = node->parent) {
        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        if (count == sizeof(ancestors) / sizeof(ancestors[0])) return false;
        ancestors[count++] = node;
    }
    ComputedStyle style = layout_initial_root_style();
    while (count != 0) {
        ComputedStyle next;
        layout_reuse_cache_resolve_style(session->page.layout_reuse,
            &session->page.stylesheet, session->fonts, ancestors[--count],
            &style, &next);
        if (next.display == DISPLAY_NONE || next.hidden
            || (painted_only && (next.opacity == 0
                || next.content_visibility == STYLE_CONTENT_VISIBILITY_HIDDEN))) return false;
        style = next;
    }
    return !painted_only || !style.visibility_hidden;
}

/* Reader retains raw resource ownership while presenting a native extracted
   copy. Match only inside the engine-owned root, never a public DOM marker.
   The original source/weak handle remains the fetch authority. */
static lxb_dom_node_t *navigation_deferred_reader_image(
    NavigationPage *page, const ImagePriorityTarget *target)
{
    lxb_dom_node_t *root = page->reader_root;
    if (root == NULL || (page->reader_root_handle != 0
        && (page->runtime == NULL || script_runtime_node_handle_resolve_connected(
            page->runtime, page->reader_root_handle) != root))) return NULL;
    if (!navigation_deferred_target_live(page, target)) return NULL;
    size_t length = 0;
    const char *source = image_select_source(&page->stylesheet, target->node, &length);
    if (source == NULL || length == 0) return NULL;
    size_t visited = 0;
    for (lxb_dom_node_t *node = root; node != NULL && visited++ < 8192u;) {
        if (node_name_is(node, "img")) {
            size_t candidate_length = 0;
            const char *candidate = image_select_source(&page->stylesheet, node, &candidate_length);
            if (candidate != NULL && length == candidate_length
                && memcmp(source, candidate, length) == 0) return node;
        }
        if (node->first_child != NULL) { node = node->first_child; continue; }
        while (node != root && node->next == NULL) node = node->parent;
        if (node == root) break;
        node = node->next;
    }
    return NULL;
}

static void navigation_rank_deferred_images(NavigationSession *session, int scroll_y)
{
    NavigationPage *page = &session->page;
    uint64_t distances[NAVIGATION_VISIBLE_IMAGE_PRIORITY_LIMIT];
    size_t start = page->deferred_image_cursor;
    for (size_t i = start; i < page->deferred_image_count; i++) {
        ImagePriorityTarget *target = &page->deferred_image_targets[i];
        ImagePriorityTarget presentation = *target;
        bool visible = navigation_image_target_visible(session, target, false);
        if (!visible) {
            lxb_dom_node_t *reader = navigation_deferred_reader_image(page, target);
            if (reader != NULL) {
                presentation.node = reader;
                presentation.weak_handle = 0;
                visible = navigation_image_target_visible(session, &presentation, false);
            }
        }
        uint64_t distance = UINT64_MAX;
        if (visible) {
            distance = navigation_deferred_image_distance(session, &presentation,
                scroll_y, &target->display_width, &target->display_height);
            /* Unknown inline geometry remains admissible for eager images.
               Explicit lazy images wait for positive viewport evidence. */
            if (navigation_attribute_equals(target->node, "loading", "lazy")) {
                if (distance != 0) distance = UINT64_MAX;
            } else if (distance == UINT64_MAX) distance--;
        }
        distances[i] = distance;
    }
    /* Stable bounded insertion sort: compute geometry/styles once per
       target, not once per comparison or once per downloaded image. */
    for (size_t i = start + 1; i < page->deferred_image_count; i++) {
        ImagePriorityTarget target = page->deferred_image_targets[i];
        uint64_t distance = distances[i];
        size_t at = i;
        while (at > start && distance < distances[at - 1]) {
            page->deferred_image_targets[at] = page->deferred_image_targets[at - 1];
            distances[at] = distances[at - 1];
            at--;
        }
        page->deferred_image_targets[at] = target;
        distances[at] = distance;
    }
    page->deferred_image_rank_scroll_y = scroll_y;
    page->deferred_image_rank_valid = true;
    page->deferred_image_eligible_end = start;
    while (page->deferred_image_eligible_end < page->deferred_image_count
           && distances[page->deferred_image_eligible_end] != UINT64_MAX)
        page->deferred_image_eligible_end++;
    if (distances[start] == UINT64_MAX) page->image_continuation_pending = false;
}

static bool navigation_publish_fixed_image_paint(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    LayoutDocument *layout = &page->layout;
    /* A live realm is safe too when its journal is clean and the exact
       committed generations still match. Queued author tasks run later;
       any mutation they make must go through ordinary replacement flow. */
    if ((page->runtime != NULL
         && script_runtime_has_pending_mutations(page->runtime))
        || page->layout_provisional
        || session->style_rebuild_required || layout->count == 0
        || layout->image_content_generation != page->document.content_generation
        || layout->image_style_generation != document_style_generation()
        || layout->image_sheet_generation != page->stylesheet.build_generation
        || layout->image_resource_count > page->images.count)
        return false;
    /* Resource-table growth can move every ImageResource, not just the new
       images. Preflight the entire list without dereferencing old pointers.
       Unknown paint/intrinsic forms retain the transactional full rebuild. */
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (draw_command_filter_code(command) != 0
            || draw_command_backdrop_blur(command) != 0) return false;
    }
    /* Already-laid-out intrinsic images and SVGs need no new proof. Only the
       newly appended suffix can change geometry. Table moves are rebound by
       navigation_rebind_appended_images before reaching this publication. */
    for (size_t i = layout->image_resource_count; i < page->images.count; i++) {
        const ImageResource *image = &page->images.items[i];
        if (image->is_background || image->is_canvas)
            return false;
        if (image->is_mask && (image->pseudo != PSEUDO_NONE
            || image != images_find_mask_node(&page->images, image->node)))
            return false;
        bool found = false;
        for (size_t j = 0; j < layout->count; j++) {
            const DrawCommand *command = &layout->commands[j];
            if (command->type == DRAW_IMAGE
                && command->text == (const char *) image->node
                && (image->is_mask
                    ? (command->font_family & LAYOUT_IMAGE_SLOT_MASK) != 0
                    : command->text_length == 0
                      && (command->font_family & LAYOUT_IMAGE_SLOT_MASK) == 0)
                && command->width > 0 && command->height > 0) {
                if (image->is_mask && command->image == NULL) {
                    DrawCommand probe = *command;
                    if (!layout_bind_fixed_mask_image(&page->stylesheet, &probe, image))
                        return false;
                }
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    bool unresolved = false;
    int alt_margin = 0;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (size_t i = 0; i < layout->count; i++) {
        DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->opacity_scale != 0
            && (command->radius & LAYOUT_TEXT_IMAGE_ALT) != 0
            && image_resource_available(images_find_node(&page->images,
                (lxb_dom_node_t *) command->image))) {
            command->opacity_scale = 0;
            command->radius |= LAYOUT_TEXT_FIND_EXCLUDED;
            int margin = command->font_size > 0 ? command->font_size : command->height;
            if (margin > alt_margin) alt_margin = margin;
        }
        if (command->type != DRAW_IMAGE || command->text == NULL) continue;
        bool mask_slot = (command->font_family & LAYOUT_IMAGE_SLOT_MASK) != 0;
        if (!mask_slot && command->text_length != 0) continue;
        const ImageResource *next = mask_slot
            ? images_find_mask_node(&page->images, (lxb_dom_node_t *) command->text)
            : images_find_node(&page->images, (lxb_dom_node_t *) command->text);
        bool available = image_resource_available(next);
        if (command->image == NULL && available) {
            int r = command->x > INT_MAX - command->width
                ? INT_MAX : command->x + command->width;
            int b = command->y > INT_MAX - command->height
                ? INT_MAX : command->y + command->height;
            if (command->x < left) left = command->x;
            if (command->y < top) top = command->y;
            if (r > right) right = r;
            if (b > bottom) bottom = b;
        }
        if (mask_slot && command->image == NULL && available) {
            /* Preflight above proved the stable layer ID. Its decoded
               intrinsic dimensions only affect paint, never this box. */
            (void) layout_bind_fixed_mask_image(&page->stylesheet, command, next);
        } else command->image = available ? next : NULL;
        if (!available) unresolved = true;
    }
    for (size_t i = 0; i < layout->visual_priority_count; i++) {
        const ImagePriorityTarget *target = &layout->visual_priority_targets[i];
        const ImageResource *image = target->kind == IMAGE_PRIORITY_KIND_MASK
            ? images_find_mask_source(&page->images, target->node,
                target->source, target->pseudo)
            : target->kind == IMAGE_PRIORITY_KIND_DOCUMENT
                ? images_find_node(&page->images, target->node) : NULL;
        if (!image_resource_available(image))
            unresolved = true;
    }
    if (!layout->visual_priority_overflow)
        layout->unresolved_external_visuals = unresolved;
    /* The retained command boxes already include layout translations and
       transforms. Fixed/sticky images keep those boxes; the renderer rebinds
       their resources and retires any fixed-image raster it previously built.
       Expanding filters declined above.
       Keep unrelated tiles and the active frame job, not just flow geometry. */
    /* Fallback glyph ink may overhang its advance box. Retire neighboring
       tiles conservatively too, including large/italic authored alt fonts. */
    if (alt_margin > 0 && right > left && bottom > top) {
        left = left < INT_MIN + alt_margin ? INT_MIN : left - alt_margin;
        top = top < INT_MIN + alt_margin ? INT_MIN : top - alt_margin;
        right = right > INT_MAX - alt_margin ? INT_MAX : right + alt_margin;
        bottom = bottom > INT_MAX - alt_margin ? INT_MAX : bottom + alt_margin;
    }
    session->relayout_damage_valid = right > left && bottom > top;
    session->relayout_damage_left = session->relayout_damage_valid ? left : 0;
    session->relayout_damage_top = session->relayout_damage_valid ? top : 0;
    session->relayout_damage_right = session->relayout_damage_valid ? right : 0;
    session->relayout_damage_bottom = session->relayout_damage_valid ? bottom : 0;
    session->canvas_paint_damage_pending = true;
    layout->image_resource_count = page->images.count;
    session->incremental_relayouts++;
    /* Publish availability to DOM image consumers too. This queues load
       tasks for a later runtime turn; it must not invoke onload in the
       middle of the retained-paint transaction. */
    if (page->runtime != NULL) {
        /* A CSSOM snapshot may borrow the former image table or contain
           empty slots. Keep it from being adopted over these new pixels. */
        script_runtime_set_layout(page->runtime, layout, session->viewport.css_height);
        script_runtime_set_images(page->runtime, &page->images);
    }
    return true;
}

static bool navigation_publish_deferred_images(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->deferred_image_publication_age == 0) return true;
    page->deferred_image_publication_age = 0;
    if (session->deferred_image_ready != NULL)
        session->deferred_image_ready(session->deferred_image_ready_opaque, session);
    /* A pending visual batch owns the next replacement flow. Let its short
       coalescing window include this completed document-image batch. */
    if (page->visual_image_publication_pending) return true;
    if (navigation_publish_fixed_image_paint(session)) return true;
    if (!navigation_relayout(session)) {
        session->performance.background_image_failures++;
        navigation_deferred_images_finish(session);
        return false;
    }
    session->performance.background_image_relayouts++;
    return true;
}

static void navigation_rebind_appended_images(NavigationSession *session,
                                             uintptr_t previous_base,
                                             size_t previous_count)
{
    if (previous_base == (uintptr_t) session->page.images.items
        && previous_count <= session->page.images.count) return;
    (void) layout_rebind_image_resources(&session->page.layout,
        previous_base, previous_count, &session->page.images);
    if (session->image_table_moved != NULL)
        session->image_table_moved(session->image_table_moved_opaque,
            session, previous_base, previous_count);
}

/* An optional image publication also yields to page input queued during
   the decode before it. */
#define NAVIGATION_IMAGE_PUBLICATION \
    (TILEFINCH_COOPERATE_OPTIONAL_PUBLICATION \
     | TILEFINCH_COOPERATE_IMAGE_PUBLICATION)

static NavigationBackgroundWorkOutcome navigation_publish_visual_images(
    NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (!page->visual_image_publication_pending)
        return NAVIGATION_BACKGROUND_WORK_SUCCESS;
    /* No author mutation is part of this static resource publication. New
       input can abandon its replacement layout, keeping both the incumbent
       and decoded resources for the next idle turn. Intrinsic masks and
       backgrounds still require full flow. */
    bool optional = page->runtime == NULL && !session->style_rebuild_required;
    bool allowed = !optional
        || tilefinch_platform_cooperate_at(
            "optional-image-publication", 1, NAVIGATION_IMAGE_PUBLICATION);
    bool painted = allowed && navigation_publish_fixed_image_paint(session);
    /* Masks and intrinsic images genuinely need flow. Give the existing
       bounded document-image job at most two owner pumps to join that flow,
       rather than laying the whole page out separately for adjacent arrivals.
       A slow response never extends this window to its network deadline. */
    if (allowed && !painted && optional && page->image_continuation_pending
        && page->deferred_image_targets != NULL
        && page->visual_image_publication_age++ < 2u) {
        (void) navigation_run_deferred_image_work(session);
        if (page->image_continuation_pending) {
            (void) tilefinch_platform_cooperate_at(
                "optional-image-publication", 0, NAVIGATION_IMAGE_PUBLICATION);
            return page->loaded ? NAVIGATION_BACKGROUND_WORK_SUCCESS
                : NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
        }
    }
    bool adopted = painted || (allowed && navigation_relayout(session));
    if (optional)
        (void) tilefinch_platform_cooperate_at(
                "optional-image-publication", 0, NAVIGATION_IMAGE_PUBLICATION);
    if (!page->loaded) return NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
    if (!allowed || (!adopted && page->loaded && session->layout_build_cancelled))
        return NAVIGATION_BACKGROUND_WORK_SUCCESS;
    page->visual_image_publication_pending = false;
    page->visual_image_publication_age = 0;
    if (!adopted) {
        session->performance.background_image_failures++;
        return NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL;
    }
    if (!painted) session->performance.background_image_relayouts++;
    return NAVIGATION_BACKGROUND_WORK_SUCCESS;
}

static bool navigation_run_deferred_image(NavigationSession *session)
{
    NavigationPage *page = &session->page;
    if (page->deferred_image_targets == NULL
        || page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
        return true;
    }
    if (page->deferred_image_job != NULL) {
        size_t count = images_priority_load_target_count(page->deferred_image_job);
        bool current = count <= page->deferred_image_count - page->deferred_image_cursor;
        for (size_t at = 0; current && at < count; at++)
            current = navigation_deferred_target_live(page,
                &page->deferred_image_targets[page->deferred_image_cursor + at]);
        /* Resolve weak identities before the source check can inspect DOM.
           Already-published resources belong to the page, including any
           table changes made by author script between idle pumps. */
        if (!current || !images_priority_load_sources_current(page->deferred_image_job)) {
            images_priority_load_commit_progress(page->deferred_image_job);
            images_priority_load_destroy(page->deferred_image_job);
            page->deferred_image_job = NULL;
            page->deferred_image_batch_count = 0;
            page->deferred_image_rank_valid = false;
            (void) navigation_publish_deferred_images(session);
            return true;
        }
    }
    if (page->deferred_image_job == NULL) {
        ImagePriorityTarget *current =
            &page->deferred_image_targets[page->deferred_image_cursor];
        if (!navigation_deferred_target_live(page, current)) {
            page->deferred_image_cursor++;
            return true;
        }
        if (current->retry_delay != 0) {
            current->retry_delay--;
            return true;
        }
        const NavigationEntry *entry = navigation_current(session);
        int scroll_y = entry == NULL ? 0 : entry->scroll_y;
        /* Both static and mutable plans need a complete stable ranking.
           Recompute after scrolling or layout changes, not per request. */
        if (current->retry_count == 0
            && (!page->deferred_image_rank_valid
                || page->deferred_image_rank_scroll_y != scroll_y)) {
            session->performance.background_image_rank_scans++;
            navigation_rank_deferred_images(session, scroll_y);
            current = &page->deferred_image_targets[
                page->deferred_image_cursor];
        }
        if (page->deferred_image_rank_valid
            && page->deferred_image_cursor >= page->deferred_image_eligible_end) {
            page->image_continuation_pending = false;
            return true;
        }
        ImagePriorityTarget *target = current;
        /* An inline image can be represented by display commands without
           its own node-box entry. Missing geometry lowers priority; it is
           not proof that the image is hidden or should be dropped. */
        if (!navigation_deferred_target_live(page, target)) {
            page->deferred_image_cursor++;
            return true;
        }
        /* The ordinary image count remains the successful-page budget. Add
           back only attempts already lost to failure, so a failed visible
           prefix does not crowd an offscreen thumbnail out while a large
           document still cannot turn the retry rule into extra successes.
           The image loader also clamps this value to its 128-node hard cap. */
        size_t maximum_attempts = session->maximum_images;
        if (page->images.stats.failed <= SIZE_MAX - maximum_attempts) {
            maximum_attempts += page->images.stats.failed;
        } else {
            maximum_attempts = SIZE_MAX;
        }
        size_t batch_count = 1u;
        if (target->retry_count == 0
            && page->deferred_image_cursor + 1u
                   < page->deferred_image_eligible_end) {
            ImagePriorityTarget *next = &page->deferred_image_targets[
                page->deferred_image_cursor + 1u];
            uint16_t next_width = next->display_width;
            uint16_t next_height = next->display_height;
            uint64_t current_distance = navigation_deferred_image_distance(
                session, target, scroll_y, NULL, NULL);
            uint64_t next_distance = navigation_deferred_image_distance(
                session, next, scroll_y, &next_width, &next_height);
            /* Pair within the visible/offscreen priority class. Offscreen
               eager images otherwise force a separate page flow each, even
               though the existing two-response transaction can publish them
               together. Never hold a visible image for an offscreen peer. */
            if ((current_distance == 0) == (next_distance == 0)
                && navigation_deferred_target_live(page, next)
                && next->retry_count == 0) {
                next->display_width = next_width;
                next->display_height = next_height;
                batch_count = 2u;
            }
        }
        page->deferred_image_job = images_priority_load_begin_batch(
            &page->document, &page->stylesheet, &page->images, target,
            batch_count,
            session->budget, page->resource_base_url, page->document_url,
            page->referrer_policy, maximum_attempts,
            session->maximum_image_bytes,
            session->maximum_image_file_bytes,
            session->maximum_decoded_image_bytes,
            session->resource_timeout_ms, page->resource_scheduler,
            session->browser_session);
        if (page->deferred_image_job == NULL) {
            navigation_deferred_image_failed(session, target);
            return true;
        }
        page->deferred_image_batch_count = (uint8_t) batch_count;
    }

    size_t loaded_before = page->images.stats.loaded;
    uintptr_t image_base = (uintptr_t) page->images.items;
    size_t image_count = page->images.count;
    session->performance.background_image_batches++;
    ImagePriorityLoadStatus status = images_priority_load_pump(
        page->deferred_image_job);
    navigation_rebind_appended_images(session, image_base, image_count);
    size_t admitted_batch = images_priority_load_target_count(
        page->deferred_image_job);
    if (admitted_batch != 0
        && admitted_batch < page->deferred_image_batch_count) {
        page->deferred_image_batch_count = (uint8_t) admitted_batch;
    }
    size_t added = page->images.stats.loaded >= loaded_before
        ? page->images.stats.loaded - loaded_before : 0;
    if (added <= SIZE_MAX
                 - session->performance.background_images_loaded) {
        session->performance.background_images_loaded += added;
    } else {
        session->performance.background_images_loaded = SIZE_MAX;
    }
    session->images = &page->images;
    navigation_ensure_layout_reuse(session);
    layout_reuse_cache_update_images(page->layout_reuse, &page->images);
    if (added != 0) {
        if (page->deferred_image_publication_age == 0)
            page->deferred_image_publication_age = 1;
        /* Decoded resources are page-owned even while layout publication
           waits briefly for the other member of this two-image batch. */
        images_priority_load_commit_progress(page->deferred_image_job);
    }
    if (page->deferred_image_publication_age != 0) {
        /* At most three further owner pumps: a slow second response cannot
           hold the first image until its network timeout. No bigger queue,
           retained DOM cursor, or unbounded settle delay. */
        if (status == IMAGE_PRIORITY_LOAD_PENDING
            && page->deferred_image_publication_age++ < 4) return true;
        if (!navigation_publish_deferred_images(session)) return true;
    }
    if (status == IMAGE_PRIORITY_LOAD_PENDING) return true;
    images_priority_load_destroy(page->deferred_image_job);
    page->deferred_image_job = NULL;
    ImagePriorityTarget *target =
        &page->deferred_image_targets[page->deferred_image_cursor];
    size_t batch_count = page->deferred_image_batch_count == 0
        ? 1u : page->deferred_image_batch_count;
    page->deferred_image_batch_count = 0;
    size_t available = navigation_deferred_image_available_prefix(
        page, batch_count);
    if (status == IMAGE_PRIORITY_LOAD_FAILED || available < batch_count) {
        /* Individual image failures are intentionally soft inside the image
           loader, so a completed batch can still contain a missing suffix.
           Attribute either shape to the first still-missing target rather
           than re-fetching a thumbnail that is already visible. If
           completions arrived out of order, the prefix remains zero and the
           ordinary retry path safely revisits target 0; a retained target 1
           resource is reused later without another transport request. */
        page->deferred_image_cursor += available;
        target = page->deferred_image_cursor < page->deferred_image_count
            ? &page->deferred_image_targets[page->deferred_image_cursor]
            : NULL;
        if (target != NULL) navigation_deferred_image_failed(session, target);
    } else {
        for (size_t at = 0; at < batch_count; at++) {
            ImagePriorityTarget *completed = &page->deferred_image_targets[
                page->deferred_image_cursor + at];
            completed->retry_count = 0;
            completed->retry_delay = 0;
        }
        page->deferred_image_cursor += batch_count;
    }
    if (page->deferred_image_cursor >= page->deferred_image_count) {
        navigation_deferred_images_finish(session);
    }
    return true;
}

bool navigation_image_publication_allowed(const NavigationSession *session)
{
    return session != NULL && (session->maximum_publication_work_units == 0
        || navigation_relayout_preview_eligible(session)
        || session->page.layout.layout_work_units <= session->maximum_publication_work_units);
}

bool navigation_run_deferred_image_work(NavigationSession *session)
{
    if (session == NULL || !session->page.loaded) return false;
    if (!session->page.image_continuation_pending
        || session->page.deferred_image_targets == NULL) return true;
    /* Pages that relayout provisionally show each batch in a fraction of
       a second and complete in preemptible idle work. */
    if (!navigation_image_publication_allowed(session)) {
        /* Every batch is published through a full relayout costing about
           what the committed layout cost. On a page this large that is
           seconds of blocked input per pair of images; keep the fallback
           boxes and finish the queue instead. */
#ifdef TILEFINCH_PSP_VALIDATION_LOG
        char message[160];
        snprintf(message, sizeof(message),
                 "tilefinch-deferred-images: skipped targets=%zu work=%zu limit=%zu",
                 session->page.deferred_image_count,
                 session->page.layout.layout_work_units,
                 session->maximum_publication_work_units);
        tilefinch_platform_log_message(message);
#endif
        navigation_deferred_images_finish(session);
        return true;
    }
    return navigation_run_deferred_image(session);
}

#if !defined(__PSP__)

void navigation_test_refuse_next_background_font_relayout(void)
{
    tilefinch_test_faults()->refuse_next_background_font_relayout = true;
}
#endif

NavigationBackgroundWorkOutcome navigation_run_background_resources(
    NavigationSession *session)
{
    if (session == NULL || !session->page.loaded)
        return NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
    if (session->page.visual_image_publication_pending)
        return navigation_publish_visual_images(session);
    NavigationBackgroundWorkOutcome outcome =
        navigation_run_web_font_memory_work(session);
    if (fonts_external_loader_pending(
            &session->page.external_font_loader)) {
        bool face_loaded = false;
        size_t failures_before = session->page.external_fonts.failed;
        uint64_t started = navigation_now_us();
        session->performance.background_font_slices++;
        if (!fonts_external_loader_step(
                &session->page.external_font_loader,
                &session->page.stylesheet, session->budget,
                session->page.resource_base_url,
                session->page.document_url,
                session->page.referrer_policy,
                session->maximum_font_attempts,
                session->maximum_font_bytes,
                session->maximum_font_file_bytes,
                session->maximum_font_face_backend_bytes,
                session->page.resource_scheduler,
                session->browser_session,
                &session->page.document.content_security_policy,
                &session->page.external_fonts, 0, &face_loaded)) {
            fonts_external_loader_cancel(
                &session->page.external_font_loader,
                session->page.resource_scheduler,
                &session->page.external_fonts);
            session->performance.background_font_failures++;
        }
        if (session->page.external_fonts.failed > failures_before) {
            size_t added_failures =
                session->page.external_fonts.failed - failures_before;
            if (added_failures <= SIZE_MAX
                    - session->performance.background_font_failures) {
                session->performance.background_font_failures +=
                    added_failures;
            } else {
                session->performance.background_font_failures = SIZE_MAX;
            }
        }
        uint64_t elapsed = navigation_record_slice(
            session, NAVIGATION_SLICE_RESOURCE, started, 1);
        session->performance.resource_us += elapsed;
        session->performance.font_resource_us += elapsed;
        if (face_loaded) {
            session->performance.background_fonts_loaded++;
            /* Face changes invalidate sizing and ch-dependent styles, not
               the font-independent cascade. Use the same transaction as
               application-font publication. */
            layout_reuse_cache_begin_font_publication(session->page.layout_reuse);
            bool relaid_out = false;
#if !defined(__PSP__)
            if (tilefinch_test_faults()->refuse_next_background_font_relayout) {
                tilefinch_test_faults()->refuse_next_background_font_relayout = false;
            } else
#endif
            {
                relaid_out = navigation_relayout(session);
            }
            layout_reuse_cache_end_font_publication(session->page.layout_reuse);
            if (!relaid_out) {
                fonts_external_loader_cancel(
                    &session->page.external_font_loader,
                    session->page.resource_scheduler,
                    &session->page.external_fonts);
                session->performance.background_font_failures++;
                /* The fast relayout path constructs a replacement before
                   retiring the current display list. Preserve that valid
                   committed page, classify the optional face as refused,
                   and continue image/tile idle work without manufacturing a
                   page-network error. */
                outcome = NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL;
            } else {
                session->performance.background_font_relayouts++;
            }
        }
    }
    if (session->page.visual_batch_targets != NULL) {
        NavigationPage *page = &session->page;
        ImagePriorityTarget *targets = page->visual_batch_targets;
        size_t count = page->visual_batch_count;
        page->visual_batch_targets = NULL;
        page->visual_batch_count = 0;
        size_t loaded_before = page->images.stats.loaded;
        uintptr_t image_base = (uintptr_t) page->images.items;
        size_t image_count = page->images.count;
        bool loaded = images_load_external_priority_targets(
            &page->document, &page->stylesheet, &page->images, targets,
            count, session->budget, page->resource_base_url,
            page->document_url, page->referrer_policy,
            session->maximum_images, session->maximum_image_bytes,
            session->maximum_image_file_bytes,
            session->maximum_decoded_image_bytes,
            session->resource_timeout_ms, page->resource_scheduler,
            session->browser_session);
        budget_free(session->budget, targets);
        navigation_rebind_appended_images(session, image_base, image_count);
        session->images = &page->images;
        navigation_ensure_layout_reuse(session);
        layout_reuse_cache_update_images(page->layout_reuse, &page->images);
        if (loaded && page->images.stats.loaded > loaded_before) {
            session->performance.background_images_loaded +=
                page->images.stats.loaded - loaded_before;
            page->visual_image_publication_pending = true;
            return navigation_publish_visual_images(session);
        } else if (!loaded) {
            session->performance.background_image_failures++;
        }
        return outcome;
    }
    if (!session->page.image_continuation_pending) return outcome;
    if (session->page.deferred_image_targets != NULL) {
        return navigation_run_deferred_image(session)
            ? outcome : NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
    }
    ImageResources *images = &session->page.images;
    size_t loaded_before = images->stats.loaded;
    uintptr_t image_base = (uintptr_t) images->items;
    size_t image_count = images->count;
    session->performance.background_image_batches++;
    navigation_ensure_layout_reuse(session);
    bool loaded = images_load_external_reusing_layout_styles(
        &session->page.document, &session->page.stylesheet, images,
        session->budget, session->page.resource_base_url,
        session->page.document_url, session->page.referrer_policy,
        session->maximum_images, session->maximum_image_bytes,
        session->maximum_image_file_bytes,
        session->maximum_decoded_image_bytes,
        session->resource_timeout_ms, session->page.resource_scheduler,
        session->browser_session, session->page.layout_reuse,
        session->fonts, session->viewport.css_width);
    navigation_rebind_appended_images(session, image_base, image_count);
    if (!loaded) {
        session->page.image_continuation_pending = false;
        session->performance.background_image_failures++;
        /* The loader transactionally removes only the optional suffix it
           appended during this pass.  Priority resources referenced by the
           committed layout remain owned, so pressure cannot blank or close
           an already usable page. */
        session->images = images;
        layout_reuse_cache_update_images(session->page.layout_reuse, images);
        return outcome;
    }
    session->images = images;
    layout_reuse_cache_update_images(session->page.layout_reuse, images);
    size_t added = images->stats.loaded >= loaded_before
        ? images->stats.loaded - loaded_before : 0;
    if (added <= SIZE_MAX
                 - session->performance.background_images_loaded) {
        session->performance.background_images_loaded += added;
    } else {
        session->performance.background_images_loaded = SIZE_MAX;
    }
    if (added != 0) {
        if (!navigation_relayout(session)) {
            session->page.image_continuation_pending = false;
            session->performance.background_image_failures++;
            /* A fast replacement refusal leaves the incumbent layout valid:
               no author DOM was changed. Do not retire that usable page for
               an optional image. Destructive/full-rebuild failures already
               retire the page in navigation_relayout_failed. */
            return session->page.loaded
                ? NAVIGATION_BACKGROUND_WORK_SOFT_REFUSAL
                : NAVIGATION_BACKGROUND_WORK_HARD_FAILURE;
        }
        session->performance.background_image_relayouts++;
    }
    /* The complete authoritative traversal ran once from the currently
       committed DOM. Retrying it in small stateless attempt batches would
       change semantics because corrupt early resources are intentionally not
       retained in the successful-image table and could consume quota again. */
    session->page.image_continuation_pending = false;
    return outcome;
}
