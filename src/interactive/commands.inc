static char *read_file(Budget *budget, const char *path, size_t *length)
{
    unsigned char *data = NULL;
    return tilefinch_platform_read_asset(budget, path, 16 * MIB, &data, length)
           ? (char *) data : NULL;
}

static bool apply_user_css(NavigationSession *navigation, const char *path)
{
    if (path == NULL) return true;
    size_t length = 0;
    char *css = read_file(navigation->budget, path, &length);
    bool ok = css != NULL && length <= 128 * KIB
        && navigation_set_user_css(navigation, css, length);
    budget_free(navigation->budget, css);
    if (!ok) return false;
    return navigation_relayout(navigation);
}

static char *instrument_probe_source(Budget *budget, char *source,
                                     size_t *length)
{
    static const char original_suffix[] = "}();";
    static const char traced_suffix[] = "}.call(__tilefinchGlobalProxy);";
    if (source == NULL || length == NULL
        || *length < sizeof(original_suffix) - 1
        || memcmp(source + *length - (sizeof(original_suffix) - 1),
                  original_suffix, sizeof(original_suffix) - 1) != 0) {
        return source;
    }
    size_t prefix = *length - (sizeof(original_suffix) - 1);
    size_t traced_length = prefix + sizeof(traced_suffix) - 1;
    char *traced = budget_malloc_category(
        budget, BUDGET_CATEGORY_JAVASCRIPT, traced_length + 1);
    if (traced == NULL) return source;
    memcpy(traced, source, prefix);
    memcpy(traced + prefix, traced_suffix, sizeof(traced_suffix));
    budget_free(budget, source);
    *length = traced_length;
    return traced;
}

static bool loop_cache_owned(BrowserEngine *engine, TileCache *cache,
                             uint16_t *frame, size_t frame_pixels)
{
    size_t owned_pixels = 0;
    return browser_engine_render_shell(engine) == cache
        && browser_engine_framebuffer(engine, &owned_pixels) == frame
        && owned_pixels == frame_pixels;
}

static bool loop_cache_reset(BrowserEngine *engine, TileCache *cache,
                             uint16_t *frame, size_t frame_pixels)
{
    return browser_engine_refresh_render_shell(engine)
        && loop_cache_owned(engine, cache, frame, frame_pixels);
}

static uint64_t interaction_now_us(void)
{
    return tilefinch_platform_monotonic_time_us();
}

typedef struct {
    uint64_t raster_us;
    uint64_t frame_us;
    uint64_t focus_us;
    uint64_t present_us;
    uint64_t total_us;
} InteractionPaintTiming;

static bool loop_render(BrowserEngine *engine, TileCache *cache,
                        NavigationSession *navigation,
                        const BrowserController *controller,
                        const InteractiveMediaSession *media,
                        const char *path, InteractionPaintTiming *timing)
{
    uint64_t started = interaction_now_us();
    uint64_t raster_before = cache == NULL ? 0 : cache->raster_us;
    uint64_t frame_before = cache == NULL ? 0 : cache->frame_us;
    const NavigationEntry *entry = navigation_current(navigation);
    if (entry == NULL
        || !browser_engine_render_frame(engine, NULL)) {
        return false;
    }
    uint64_t focus_started = interaction_now_us();
    int x = 0, y = 0, width = 0, height = 0;
    if (controller_focused_rect(controller, &x, &y, &width, &height)) {
        const LayoutDocument *layout = &navigation->page.layout;
        int visual_x = viewport_css_to_device(&layout->viewport, x);
        int visual_y = viewport_css_to_device(
            &layout->viewport, y - entry->scroll_y);
        int visual_right = viewport_css_to_device(
            &layout->viewport, x + width);
        int visual_bottom = viewport_css_to_device(
            &layout->viewport, y - entry->scroll_y + height);
        int visual_width = visual_right > visual_x
                           ? visual_right - visual_x : 1;
        int visual_height = visual_bottom > visual_y
                            ? visual_bottom - visual_y : 1;
        render_paint_focus_outline(cache->frame, cache->frame_pixels,
                                   480, 272, visual_x, visual_y,
                                   visual_width, visual_height);
    }
    interactive_media_composite(media, cache->frame, 480, 272, 480);
    uint64_t focus_finished = interaction_now_us();
    uint64_t present_started = focus_finished;
    if (host_platform_simulator != NULL
        && !tilefinch_platform_present_rgb565(cache->frame, 480, 272, 480)) {
        return false;
    }
    uint64_t present_finished = interaction_now_us();
    bool ok = host_platform_simulator != NULL || path == NULL
        || render_write_frame_ppm(path, cache->frame, 480, 272);
    if (timing != NULL) {
        *timing = (InteractionPaintTiming) {
            .raster_us = cache->raster_us - raster_before,
            .frame_us = cache->frame_us - frame_before,
            .focus_us = focus_finished - focus_started,
            .present_us = present_finished - present_started,
            .total_us = interaction_now_us() - started
        };
    }
    return ok;
}

static void loop_print_status(const BrowserController *controller,
                              const TileCache *cache, size_t frame_number)
{
    const NavigationSession *navigation = controller->navigation;
    const NavigationEntry *entry = navigation_current(navigation);
    int focus_x = 0, focus_y = 0, focus_width = 0, focus_height = 0;
    bool focused = controller_focused_rect(
        controller, &focus_x, &focus_y, &focus_width, &focus_height);
    printf("loop frame=%zu url=\"%s\" title=\"%s\" scroll=%d/%d "
           "focus=%s:%zu rect=%d,%d,%d,%d links=%zu controls=%zu "
           "pending=%zu current=%zu peak=%zu tile-hits=%zu tile-misses=%zu "
           "tile-evictions=%zu loads=%zu relayouts=%zu\n",
           frame_number, entry == NULL ? "" : entry->url,
           navigation->page.document.title,
           entry == NULL ? 0 : entry->scroll_y,
           viewport_max_scroll_css(&navigation->viewport,
                                   navigation->page.layout.height),
           controller->focus_kind == CONTROLLER_FOCUS_LINK ? "link"
             : (controller->focus_kind == CONTROLLER_FOCUS_CONTROL
                  ? "control"
                  : (controller->focus_kind == CONTROLLER_FOCUS_POINTER
                       ? "pointer" : "none")),
           controller->focus_index, focused ? focus_x : 0,
           focused ? focus_y : 0, focused ? focus_width : 0,
           focused ? focus_height : 0,
           navigation->page.layout.link_count,
           navigation->page.layout.control_count,
           navigation->page.script_result.pending_tasks,
           navigation->budget->current, navigation->budget->peak,
           cache->hits, cache->misses, cache->evictions,
           navigation->loads_committed,
           navigation->incremental_relayouts);
}

static bool loop_apply_profile(NavigationSession *navigation,
                               BrowserController *controller,
                               const char *user_css)
{
    if (!apply_user_css(navigation, user_css)
        || !controller_init(controller, navigation)) return false;
    return controller_configure_viewport(controller, 272, 16);
}

static bool experimental_restore_section_page(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, size_t section, int restore_scroll,
    const char *restore_focus, const char *user_css, bool preserve_runtime)
{
    DocumentBackingSelection rollback = {0};
    const char *html = experimental->pager.current_html;
    size_t html_length = experimental->pager.current_length;
    if (experimental->pager.current_section != section) {
        if (!document_backing_prepare_section(
                &experimental->backing, section, 0, &rollback)) {
            /* A committed target DOM can be the transient peak that prevents
               the old fragment from being rematerialized.  Release that page
               and retry once while the compressed store remains intact. */
            navigation_discard_current_page(navigation);
            if (!document_backing_prepare_section(
                    &experimental->backing, section, 0, &rollback)) {
                return false;
            }
        }
        html = rollback.pager_selection.html;
        html_length = rollback.pager_selection.length;
    }
    bool reuse_runtime = preserve_runtime && navigation->page.runtime != NULL;
    bool previous_defer = navigation->defer_document_script_pipeline;
    if (!reuse_runtime && navigation->document_scripts_enabled)
        navigation->defer_document_script_pipeline = true;
    bool committed = browser_engine_commit_section_html(
        experimental->engine, &experimental->backing,
        rollback.active ? &rollback : NULL, experimental->locator,
        html, html_length, reuse_runtime, false);
    navigation->defer_document_script_pipeline = previous_defer;
    if (!committed) {
        document_backing_abort_section(&rollback);
        return false;
    }

    bool complete = true;
    if (!reuse_runtime && navigation->document_scripts_enabled
        && !experimental_execute_all_document_scripts(
               experimental, navigation)) complete = false;
    if (experimental_section_has_remote_mutations(experimental, section)
        && (!experimental_apply_remote_mutations(
                experimental, &navigation->page.document, section)
            || !document_refresh(&navigation->page.document)
            || !navigation_relayout(navigation))) complete = false;
    size_t restored_controls = 0;
    if (navigation->page.runtime != NULL
        && (!script_runtime_restore_section_state(
                navigation->page.runtime, section, &restored_controls,
                &navigation->page.script_result)
            || (restored_controls != 0
                && !navigation_relayout(navigation)))) complete = false;
    if (!loop_apply_profile(navigation, controller, user_css)) complete = false;
    if (restore_focus != NULL && restore_focus[0] != '\0') {
        lxb_dom_node_t *node = experimental_find_focus_key(
            &navigation->page.document, section, restore_focus);
        if (node != NULL) (void) controller_focus_node(controller, node);
    }
    int maximum = viewport_max_scroll_css(
        &navigation->viewport, navigation->page.layout.height);
    navigation_set_scroll(navigation, restore_scroll < 0 ? 0
        : (restore_scroll < maximum ? restore_scroll : maximum));
    section_pager_record_height(&experimental->pager,
                                navigation->page.layout.height);
    printf("experimental-rollback section=%zu scroll=%d status=%s\n",
           section, navigation_current(navigation) == NULL ? 0
               : navigation_current(navigation)->scroll_y,
           complete ? "restored" : "partial");
    return true;
}

static bool experimental_commit_section(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, size_t section, int direction,
    const char *user_css)
{
    if (experimental == NULL || !experimental->active) return false;
    if (experimental->body_source_replaced
        && section != experimental->pager.current_section) return false;
    size_t previous_section = experimental->pager.current_section;
    const NavigationEntry *previous_entry = navigation_current(navigation);
    int previous_scroll = previous_entry == NULL ? 0 : previous_entry->scroll_y;
    bool navigation_attempted = false;
    bool selection_committed = false;
    DocumentBackingSelection selection = {0};
    uint64_t swap_started_ns = tilefinch_platform_monotonic_time_ns();
    lxb_dom_node_t *focused = NULL;
    if (controller->focus_kind == CONTROLLER_FOCUS_LINK
        && controller->focus_index < navigation->page.layout.link_count) {
        focused = navigation->page.layout.links[controller->focus_index].node;
    } else if (controller->focus_kind == CONTROLLER_FOCUS_CONTROL
               && controller->focus_index
                  < navigation->page.layout.control_count) {
        focused = navigation->page.layout.controls[
            controller->focus_index].node;
    }
    char focus_key[256] = {0};
    size_t focus_key_length = 0;
    if (!experimental_focus_key(
            &navigation->page.document,
            experimental->pager.current_section, focused,
            focus_key, &focus_key_length)) return false;
    if (!section_pager_record_focus(
            &experimental->pager,
            focus_key_length == 0 ? NULL : focus_key,
            focus_key_length)) return false;
    size_t saved_controls = 0;
    if (navigation->page.runtime != NULL
        && !script_runtime_save_section_state(
               navigation->page.runtime,
               experimental->pager.current_section, &saved_controls,
               &navigation->page.script_result)) return false;
    section_pager_record_height(&experimental->pager,
                                navigation->page.layout.height);
    experimental_record_phase(
        &experimental->state_save_timing, swap_started_ns);
    uint64_t phase_started_ns = tilefinch_platform_monotonic_time_ns();
    if (!document_backing_prepare_section(
            &experimental->backing, section, direction, &selection)) {
        return false;
    }
    experimental_record_phase(
        &experimental->pager_timing, phase_started_ns);
    phase_started_ns = tilefinch_platform_monotonic_time_ns();
    navigation_attempted = true;
    if (!browser_engine_commit_section_html(
            experimental->engine, &experimental->backing, &selection,
            experimental->locator, selection.pager_selection.html,
            selection.pager_selection.length, true, false)) goto failed;
    selection_committed = true;
    experimental_record_phase(
        &experimental->commit_timing, phase_started_ns);
    phase_started_ns = tilefinch_platform_monotonic_time_ns();
    bool replayed_mutations = experimental_section_has_remote_mutations(
        experimental, section);
    if (replayed_mutations
        && (!experimental_apply_remote_mutations(
                experimental, &navigation->page.document, section)
            || !document_refresh(&navigation->page.document)
            || !navigation_relayout(navigation))) goto failed;
    if (!experimental_execute_section_scripts(
            experimental, navigation, section)) goto failed;
    experimental_record_phase(
        &experimental->content_timing, phase_started_ns);
    phase_started_ns = tilefinch_platform_monotonic_time_ns();
    size_t restored_controls = 0;
    if (navigation->page.runtime != NULL
        && (!script_runtime_restore_section_state(
                navigation->page.runtime, section, &restored_controls,
                &navigation->page.script_result)
            || (restored_controls != 0
                && !navigation_relayout(navigation)))) goto failed;
    if (!loop_apply_profile(navigation, controller, user_css)) goto failed;
    if (saved_controls != 0 || restored_controls != 0) {
        printf("experimental-state saved=%zu restored=%zu section=%zu\n",
               saved_controls, restored_controls, section);
    }
    const char *restore_focus = section_pager_focus(
        &experimental->pager, section);
    if (restore_focus != NULL) {
        lxb_dom_node_t *restore_node = experimental_find_focus_key(
            &navigation->page.document, section, restore_focus);
        if (restore_node == NULL
            || !controller_focus_node(controller, restore_node)) goto failed;
        if (restore_focus[0] == 'i' && restore_focus[1] == ':') {
            printf("experimental-focus restored id=\"%s\" section=%zu\n",
                   restore_focus + 2, section);
        } else {
            printf("experimental-focus restored key=\"%s\" section=%zu\n",
                   restore_focus, section);
        }
    }
    experimental_record_phase(
        &experimental->restore_timing, phase_started_ns);
    phase_started_ns = tilefinch_platform_monotonic_time_ns();
    section_pager_record_height(&experimental->pager,
                                navigation->page.layout.height);
    if ((direction < 0
         && !controller_scroll_to_bottom(controller, 272))
        || (direction >= 0
            && !controller_scroll_to_top(controller, 272))) goto failed;
    experimental_record_phase(
        &experimental->scroll_timing, phase_started_ns);
    experimental->swaps++;
    if (navigation->history_index < 64) {
        experimental->history_sections[navigation->history_index] = section;
        experimental->history_section_known[navigation->history_index] = true;
    }
    uint64_t ready_finished_ns = tilefinch_platform_monotonic_time_ns();
    uint64_t ready_us = ready_finished_ns >= swap_started_ns
        ? (ready_finished_ns - swap_started_ns) / UINT64_C(1000) : 0;
    experimental->ready_total_us += ready_us;
    if (ready_us > experimental->ready_max_us)
        experimental->ready_max_us = ready_us;
    experimental->timing_swaps++;
    experimental->pending_swap_started_ns = swap_started_ns;
    experimental->first_tile_pending = true;
    size_t logical_offset = 0;
    bool known_offset = section_pager_known_logical_offset(
        &experimental->pager, experimental->pager.current_section,
        &logical_offset);
    printf("experimental-pager section=%zu/%zu direction=%d height=%d "
           "logical-offset=%s%zu loads=%zu prefetches=%zu hits=%zu "
           "evictions=%zu swaps=%zu\n",
           experimental->pager.current_section,
           experimental->pager.store->section_count, direction,
           navigation->page.layout.height, known_offset ? "" : "unknown:",
           known_offset ? logical_offset : 0,
           experimental->pager.section_loads,
           experimental->pager.prefetches,
           experimental->pager.prefetch_hits,
           experimental->pager.prefetch_evictions, experimental->swaps);
    printf("experimental-swap-timing section=%zu decode-us=%llu "
           "prefetch-us=%llu state-us=%llu pager-us=%llu commit-us=%llu "
           "teardown-us=%llu gc-us=%llu parse-us=%llu build-us=%llu "
           "content-us=%llu restore-us=%llu scroll-us=%llu ready-us=%llu "
           "prefetch-hit=%s\n",
           section,
           (unsigned long long) experimental->pager.last_decode_us,
           (unsigned long long) experimental->pager.last_prefetch_us,
           (unsigned long long) experimental->state_save_timing.last_us,
           (unsigned long long) experimental->pager_timing.last_us,
           (unsigned long long) experimental->commit_timing.last_us,
           (unsigned long long)
             navigation->performance.last_swap_teardown_us,
           (unsigned long long) navigation->performance.last_swap_gc_us,
           (unsigned long long) navigation->performance.last_swap_parse_us,
           (unsigned long long) navigation->performance.last_swap_commit_us,
           (unsigned long long) experimental->content_timing.last_us,
           (unsigned long long) experimental->restore_timing.last_us,
           (unsigned long long) experimental->scroll_timing.last_us,
           (unsigned long long) ready_us,
           experimental->pager.last_select_prefetch_hit ? "yes" : "no");
    return true;

failed:
    document_backing_abort_section(&selection);
    if (!navigation_set_runtime_section_identity(
            navigation, previous_section)) {
        fprintf(stderr,
                "experimental-rollback section=%zu status=identity-failed\n",
                previous_section);
    }
    if (navigation_attempted
        && !experimental_restore_section_page(
               experimental, navigation, controller, previous_section,
               previous_scroll,
               focus_key_length == 0 ? NULL : focus_key, user_css, true)) {
        fprintf(stderr,
                "experimental-rollback section=%zu target=%zu "
                "pager-committed=%s status=failed\n",
                previous_section, section,
                selection_committed ? "yes" : "no");
    }
    return false;
}

static bool experimental_process_remote_elements(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, const char *user_css, bool *swapped)
{
    if (swapped != NULL) *swapped = false;
    if (experimental == NULL || !experimental->active
        || navigation == NULL || navigation->page.runtime == NULL) return true;
    if (experimental->body_source_replaced) {
        char ignored[129];
        size_t ignored_section = 0;
        while (script_runtime_take_remote_element_request(
                   navigation->page.runtime, ignored, sizeof(ignored),
                   &ignored_section)) {}
        return true;
    }
    for (size_t attempt = 0; attempt < 8; attempt++) {
        char identifier[129];
        size_t section = 0;
        if (!script_runtime_take_remote_element_request(
                navigation->page.runtime, identifier, sizeof(identifier),
                &section)) return true;
        if (section >= experimental->pager.store->section_count) return false;
        if (section == experimental->pager.current_section) continue;
        int direction = section < experimental->pager.current_section ? -1 : 1;
        if (!experimental_commit_section(
                experimental, navigation, controller, section, direction,
                user_css)) return false;
        if (swapped != NULL) *swapped = true;
        printf("experimental-remote-element id=\"%s\" section=%zu mode=deferred\n",
               identifier, section);
    }
    char overflow_identifier[129];
    size_t overflow_section = 0;
    return !script_runtime_take_remote_element_request(
        navigation->page.runtime, overflow_identifier,
        sizeof(overflow_identifier), &overflow_section);
}

static bool experimental_scroll_boundary(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, int direction, const char *user_css,
    bool *swapped)
{
    *swapped = false;
    if (experimental == NULL || !experimental->active) return true;
    if (experimental->body_source_replaced) return true;
    const NavigationEntry *entry = navigation_current(navigation);
    if (entry == NULL) return false;
    int maximum = viewport_max_scroll_css(&navigation->viewport,
                                          navigation->page.layout.height);
    bool boundary = direction < 0 ? entry->scroll_y <= 0
                                  : entry->scroll_y >= maximum;
    if (!boundary) return true;
    size_t current = experimental->pager.current_section;
    if ((direction < 0 && current == 0)
        || (direction > 0
            && current + 1 >= experimental->pager.store->section_count))
        return true;
    size_t target = direction < 0 ? current - 1 : current + 1;
    if (!experimental_commit_section(experimental, navigation, controller,
                                     target, direction, user_css)) return false;
    *swapped = true;
    return true;
}

static void experimental_leave_document(
    ExperimentalSectionContext *experimental)
{
    if (experimental == NULL || !experimental->active) return;
    document_backing_uninstall(experimental->navigation);
    document_backing_clear(&experimental->backing);
    CompressedSectionStore *store = experimental->pager.store;
    section_pager_destroy(&experimental->pager);
    experimental_script_state_destroy(experimental);
    experimental_remote_mutations_destroy(experimental);
    if (store != NULL) section_store_destroy(store);
    experimental->active = false;
}

static bool experimental_finish_external_attempt(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, const char *user_css, bool was_active,
    size_t previous_section, int previous_scroll, bool succeeded)
{
    if (!was_active) return succeeded;
    if (succeeded) {
        experimental_leave_document(experimental);
        return true;
    }
    const char *focus = section_pager_focus(
        &experimental->pager, previous_section);
    bool restored = experimental_install_navigation_callbacks(
            navigation, experimental)
        && experimental_restore_section_page(
            experimental, navigation, controller, previous_section,
            previous_scroll, focus, user_css, false);
    if (!restored) {
        fprintf(stderr,
                "experimental-navigation-rollback section=%zu status=failed\n",
                previous_section);
    }
    return false;
}

static bool experimental_anchor_reference(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, const char *reference,
    const char *user_css, bool *handled)
{
    *handled = false;
    if (experimental == NULL || !experimental->active || reference == NULL)
        return true;
    const char *fragment = strchr(reference, '#');
    if (fragment == NULL || fragment[1] == '\0') return true;
    size_t base_length = (size_t) (fragment - reference);
    size_t locator_length = strlen(experimental->locator);
    const char *locator_fragment = strchr(experimental->locator, '#');
    if (locator_fragment != NULL)
        locator_length = (size_t) (locator_fragment - experimental->locator);
    if (base_length != 0
        && (base_length != locator_length
            || memcmp(reference, experimental->locator, base_length) != 0))
        return true;
    char identifier[2048];
    size_t identifier_length = 0;
    const char *encoded = fragment + 1;
    for (size_t at = 0; encoded[at] != '\0'; at++) {
        unsigned char value = (unsigned char) encoded[at];
        if (value == '%' && encoded[at + 1] != '\0'
            && encoded[at + 2] != '\0'
            && isxdigit((unsigned char) encoded[at + 1])
            && isxdigit((unsigned char) encoded[at + 2])) {
            unsigned char high = (unsigned char) encoded[++at];
            unsigned char low = (unsigned char) encoded[++at];
            high = (unsigned char) (isdigit(high) ? high - '0'
                                      : (tolower(high) - 'a' + 10));
            low = (unsigned char) (isdigit(low) ? low - '0'
                                     : (tolower(low) - 'a' + 10));
            value = (unsigned char) ((high << 4) | low);
        }
        if (value == 0 || identifier_length + 1 >= sizeof(identifier))
            return true;
        identifier[identifier_length++] = (char) value;
    }
    identifier[identifier_length] = '\0';
    if (identifier_length == 0) return true;
    lxb_dom_node_t *local_node = experimental->body_source_replaced
        ? find_element_id(
              lxb_dom_interface_node(navigation->page.document.html),
              identifier)
        : NULL;
    size_t target_section = experimental->body_source_replaced
        ? experimental->pager.current_section : 0;
    if (!experimental->body_source_replaced
        && !section_store_find_anchor(experimental->pager.store, identifier,
                                      identifier_length, &target_section)) {
        return true;
    }
    *handled = true;
    char target_url[NAVIGATION_URL_LIMIT];
    int target_written = snprintf(
        target_url, sizeof(target_url), "%.*s%s",
        (int) locator_length, experimental->locator, fragment);
    if (target_written < 0 || (size_t) target_written >= sizeof(target_url)
        || !navigation_commit_same_document_url(navigation, target_url))
        return false;
    if (navigation->history_index < 64) {
        experimental->history_sections[navigation->history_index] =
            target_section;
        experimental->history_section_known[navigation->history_index] = true;
    }
    int direction = target_section < experimental->pager.current_section
                    ? -1 : 1;
    if (target_section != experimental->pager.current_section
        && !experimental_commit_section(
               experimental, navigation, controller, target_section,
               direction, user_css)) return false;
    lxb_dom_node_t *node = experimental->body_source_replaced
        ? local_node
        : find_element_id(
              lxb_dom_interface_node(navigation->page.document.html),
              identifier);
    const LayoutNodeBox *box = layout_box_for_node(&navigation->page.layout,
                                                   node);
    if (box == NULL) {
        if (!experimental->body_source_replaced) return false;
        printf("experimental-anchor id=\"%s\" section=%zu y=missing "
               "scroll=%d\n", identifier, target_section,
               navigation_current(navigation)->scroll_y);
        return true;
    }
    int maximum = viewport_max_scroll_css(&navigation->viewport,
                                          navigation->page.layout.height);
    int scroll = box->y < 0 ? 0 : (box->y < maximum ? box->y : maximum);
    navigation_set_scroll(navigation, scroll);
    printf("experimental-anchor id=\"%s\" section=%zu y=%d scroll=%d\n",
           identifier, target_section, box->y, scroll);
    return true;
}

static bool loop_anchor_document(NavigationSession *navigation,
                                 const char *identifier)
{
    if (navigation == NULL || identifier == NULL || *identifier == '\0') {
        return false;
    }
    lxb_dom_node_t *root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *node = find_element_id(root, identifier);
    const LayoutNodeBox *box = node == NULL ? NULL
        : layout_box_for_node(&navigation->page.layout, node);
    const NavigationEntry *entry = navigation_current(navigation);
    if (box == NULL || entry == NULL) return false;
    int maximum = viewport_max_scroll_css(
        &navigation->viewport, navigation->page.layout.height);
    int scroll = box->y < 0 ? 0 : (box->y < maximum ? box->y : maximum);
    navigation_set_scroll(navigation, scroll);
    printf("loop-anchor id=\"%s\" y=%d scroll=%d\n",
           identifier, box->y, scroll);
    return true;
}

static bool experimental_restore_history(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, bool forward, const char *user_css,
    bool *handled)
{
    *handled = false;
    if (experimental == NULL || !experimental->active) return true;
    if ((!forward && navigation->history_index == 0)
        || (forward
            && navigation->history_index + 1 >= navigation->history_count))
        return true;
    size_t target_index = forward ? navigation->history_index + 1
                                  : navigation->history_index - 1;
    if (target_index >= 64
        || !experimental->history_section_known[target_index]) return true;
    const NavigationEntry *target = &navigation->history[target_index];
    const char *fragment = strchr(target->url, '#');
    size_t target_base = fragment == NULL ? strlen(target->url)
                                          : (size_t) (fragment - target->url);
    const char *locator_fragment = strchr(experimental->locator, '#');
    size_t locator_base = locator_fragment == NULL
        ? strlen(experimental->locator)
        : (size_t) (locator_fragment - experimental->locator);
    if (target_base != locator_base
        || memcmp(target->url, experimental->locator, target_base) != 0)
        return true;
    char old_url[NAVIGATION_URL_LIMIT], target_url[NAVIGATION_URL_LIMIT];
    const NavigationEntry *current = navigation_current(navigation);
    snprintf(old_url, sizeof(old_url), "%s", current->url);
    snprintf(target_url, sizeof(target_url), "%s", target->url);
    int restore_scroll = target->scroll_y;
    size_t section = experimental->history_sections[target_index];
    size_t previous_section = experimental->pager.current_section;
    if (experimental->body_source_replaced)
        section = experimental->pager.current_section;
    const NavigationEntry *moved = NULL;
    if (!(forward ? navigation_forward(navigation, &moved)
                  : navigation_back(navigation, &moved))) return false;
    if (section != experimental->pager.current_section
        && !experimental_commit_section(
               experimental, navigation, controller, section,
               section < experimental->pager.current_section ? -1 : 1,
               user_css)) {
        const NavigationEntry *ignored = NULL;
        (void) (forward ? navigation_back(navigation, &ignored)
                        : navigation_forward(navigation, &ignored));
        return false;
    }
    if (!navigation_restore_same_document_url(
            navigation, target_url, old_url)) {
        const NavigationEntry *ignored = NULL;
        (void) (forward ? navigation_back(navigation, &ignored)
                        : navigation_forward(navigation, &ignored));
        if (experimental->pager.current_section != previous_section) {
            (void) experimental_commit_section(
                experimental, navigation, controller, previous_section,
                previous_section < experimental->pager.current_section
                    ? -1 : 1,
                user_css);
        }
        return false;
    }
    int maximum = viewport_max_scroll_css(&navigation->viewport,
                                          navigation->page.layout.height);
    navigation_set_scroll(navigation, restore_scroll < maximum
                                      ? restore_scroll : maximum);
    *handled = true;
    printf("experimental-history direction=%s index=%zu section=%zu "
           "scroll=%d url=\"%s\"\n",
           forward ? "forward" : "back", target_index, section,
           navigation_current(navigation)->scroll_y, target_url);
    return true;
}

static bool loop_load(BrowserEngine *engine, NavigationSession *navigation,
                      BrowserController *controller, const char *url,
                      bool record_history, int restore_scroll,
                      const char *user_css)
{
    bool loaded = engine != NULL
        ? browser_engine_load_url_with_limits(
              engine, url, 4 * MIB, 30000, record_history)
        : navigation_load_url(
              navigation, navigation_begin(navigation), url,
              4 * MIB, 30000, 480, navigation->fonts,
              navigation->images, record_history);
    if (!loaded
        || !loop_apply_profile(navigation, controller, user_css)) return false;
    if (restore_scroll > 0) {
        return controller_scroll_to_top(controller, 272)
            && controller_scroll_by(controller, restore_scroll, 272);
    }
    return true;
}

static bool experimental_rebuild_failure(const char *reason,
                                         const char *stage,
                                         const FetchResult *response)
{
    const char *error = response == NULL ? NULL : response->error;
    fprintf(stderr, "experimental-rebuild reason=\"%s\" stage=%s",
            reason == NULL ? "unknown" : reason,
            stage == NULL ? "unknown" : stage);
    if (error != NULL && error[0] != '\0') {
        fprintf(stderr, " error=\"%.200s\"", error);
    }
    fputc('\n', stderr);
    return false;
}

typedef struct {
    char request_url[NAVIGATION_URL_LIMIT];
    char cookies[4096];
    char referer[NAVIGATION_URL_LIMIT];
    char referrer_policy[
        sizeof(((NavigationSession *) 0)->page.referrer_policy)];
} ExperimentalRebuildRequestScratch;

static bool experimental_rebuild_document(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, size_t selected, int restore_scroll,
    const char *document_url, const char *user_css, const char *reason)
{
    if (experimental == NULL || experimental->store == NULL
        || experimental->builder == NULL || experimental->budget == NULL
        || navigation == NULL || controller == NULL || document_url == NULL
        || reason == NULL) {
        return experimental_rebuild_failure(reason, "precondition", NULL);
    }
    ExperimentalRebuildRequestScratch *request_scratch = NULL;
    if (experimental->fixture_source == NULL) {
        request_scratch = budget_calloc_category(
            experimental->budget, BUDGET_CATEGORY_RESOURCE, 1,
            sizeof(*request_scratch));
        if (request_scratch == NULL) {
            return experimental_rebuild_failure(
                reason, "request-scratch", NULL);
        }
        snprintf(request_scratch->request_url,
                 sizeof(request_scratch->request_url), "%s",
                 experimental->locator);
        /* Reload and history traversal arrive here from explicit controller
           commands. Preserve the outgoing document's referrer policy before
           its DOM is discarded, then describe the replacement fetch with the
           same immutable policy fields as ordinary top-level navigation. */
        snprintf(request_scratch->referrer_policy,
                 sizeof(request_scratch->referrer_policy), "%s",
                 navigation->page.referrer_policy);
        if (!fetch_compute_referrer(
                document_url, request_scratch->request_url,
                request_scratch->referrer_policy, request_scratch->referer,
                sizeof(request_scratch->referer))) {
            budget_free(experimental->budget, request_scratch);
            return experimental_rebuild_failure(
                reason, "referrer-policy", NULL);
        }
    }
    document_backing_uninstall(navigation);
    document_backing_clear(&experimental->backing);
    section_pager_destroy(&experimental->pager);
    experimental_script_state_destroy(experimental);
    experimental_remote_mutations_destroy(experimental);
    section_store_destroy(experimental->store);
    experimental->active = false;
    if (experimental->response != NULL
        && experimental->response->budget != NULL) {
        fetch_result_destroy(experimental->response);
    }
    navigation_discard_current_page(navigation);
    bool rebuilt = false;
    FetchStreamMetrics metrics = {0};
    if (experimental->fixture_source != NULL) {
        rebuilt = section_store_build(
            experimental->store, experimental->budget,
            experimental->fixture_source, experimental->fixture_length,
            EXPERIMENTAL_SECTION_BLOCK_BYTES,
            EXPERIMENTAL_SECTION_MAX_BYTES);
    } else {
        rebuilt = section_store_stream_begin(
            experimental->builder, experimental->store,
            experimental->budget, EXPERIMENTAL_SECTION_BLOCK_BYTES,
            EXPERIMENTAL_SECTION_MAX_BYTES);
        FetchStreamOptions stream = {
            .on_body = stream_compressed_section_body,
            .opaque = experimental->builder, .chunk_bytes = 16384
        };
        TilefinchRequestContext request_context = {
            .target_url = request_scratch->request_url,
            .initiator_url = document_url,
            .top_level_url = document_url,
            .method = "GET",
            .mode = TILEFINCH_REQUEST_MODE_NAVIGATE,
            .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
            .destination = TILEFINCH_DESTINATION_DOCUMENT,
            .top_level_navigation = true,
            .user_activated = true
        };
        FetchRequest transport = {
            .allow_http_errors = true,
            .send_low_client_hints = true,
            .accept = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            .upgrade_insecure_requests = true,
        };
        FetchPreparedPageRequest prepared;
        bool request_ready = fetch_prepare_page_request_context(
            &request_context, document_url, request_scratch->referrer_policy,
            experimental->browser_session, NULL, NULL, &transport,
            &prepared, NULL);
        const FetchRequest *request = request_ready
            ? fetch_prepared_page_request(&prepared) : NULL;
        rebuilt = rebuilt && request != NULL && fetch_request_stream_cancelable(
            experimental->budget, request_scratch->request_url, request,
            experimental->maximum_download_bytes, 30000, NULL, NULL,
            &stream, &metrics, experimental->response)
            && section_store_stream_finish(experimental->builder);
        if (!rebuilt) section_store_stream_abort(experimental->builder);
        if (rebuilt && experimental->response != NULL) {
            const char *effective = experimental->response->effective_url;
            if (effective[0] != '\0') {
                snprintf(experimental->locator,
                         sizeof(experimental->locator), "%s", effective);
            }
            navigation->last_http_status = experimental->response->status_code;
            snprintf(navigation->last_cf_mitigated,
                     sizeof(navigation->last_cf_mitigated), "%s",
                     experimental->response->cf_mitigated);
            snprintf(navigation->last_server,
                     sizeof(navigation->last_server), "%s",
                     experimental->response->server);
            for (size_t i = 0;
                 i < experimental->response->set_cookie_count; i++) {
                TilefinchRequestContext response_context = request_context;
                response_context.target_url = fetch_set_cookie_url(
                    experimental->response, i, experimental->locator);
                (void) browser_session_cookie_set_http_context(
                    experimental->browser_session, &response_context,
                    experimental->response->set_cookies[i]);
            }
        }
        budget_free(experimental->budget, request_scratch);
        request_scratch = NULL;
    }
    if (!rebuilt) {
        return experimental_rebuild_failure(
            reason, experimental->fixture_source == NULL
                ? "fetch-and-index" : "fixture-index",
            experimental->fixture_source == NULL
                ? experimental->response : NULL);
    }
    if (experimental->store->section_count == 0) {
        return experimental_rebuild_failure(reason, "empty-store", NULL);
    }
    if (selected >= experimental->store->section_count)
        selected = experimental->store->section_count - 1;
    if (!section_pager_init(&experimental->pager, experimental->store,
                            selected, 1)) {
        return experimental_rebuild_failure(reason, "pager-init", NULL);
    }
    if (!experimental_script_state_init(experimental)) {
        return experimental_rebuild_failure(reason, "script-state", NULL);
    }
    if (!experimental_install_navigation_callbacks(
            navigation, experimental)) {
        return experimental_rebuild_failure(reason, "backing-install", NULL);
    }
    if (!browser_engine_commit_section_html(
            experimental->engine, &experimental->backing, NULL,
            document_url, experimental->pager.current_html,
            experimental->pager.current_length, false, false)) {
        return experimental_rebuild_failure(reason, "document-commit", NULL);
    }
    if (!experimental_mark_section_scripts_executed(
            experimental, selected)) {
        return experimental_rebuild_failure(reason, "script-frontier", NULL);
    }
    if (!loop_apply_profile(navigation, controller, user_css)) {
        return experimental_rebuild_failure(reason, "controller-profile", NULL);
    }
    experimental->active = true;
    int maximum = viewport_max_scroll_css(&navigation->viewport,
                                          navigation->page.layout.height);
    navigation_set_scroll(navigation,
                          restore_scroll < maximum ? restore_scroll : maximum);
    section_pager_record_height(&experimental->pager,
                                navigation->page.layout.height);
    printf("experimental-%s section=%zu/%zu source=%zu stored=%zu "
           "chunks=%zu peak-buffer=%zu scroll=%d\n",
           reason, selected, experimental->store->section_count,
           experimental->store->source_length,
           experimental->store->stored_length, metrics.chunks_received,
           metrics.peak_buffered_bytes,
           navigation_current(navigation)->scroll_y);
    return true;
}

static bool experimental_reload_document(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, const char *user_css)
{
    if (experimental == NULL || !experimental->active) return false;
    size_t selected = experimental->pager.current_section;
    const NavigationEntry *entry = navigation_current(navigation);
    int restore_scroll = entry == NULL ? 0 : entry->scroll_y;
    char document_url[NAVIGATION_URL_LIMIT];
    snprintf(document_url, sizeof(document_url), "%s",
             entry == NULL ? experimental->locator : entry->url);
    return experimental_rebuild_document(
        experimental, navigation, controller, selected, restore_scroll,
        document_url, user_css, "reload");
}

static bool experimental_resume_history(
    ExperimentalSectionContext *experimental, NavigationSession *navigation,
    BrowserController *controller, bool forward, const char *user_css,
    bool *handled)
{
    *handled = false;
    if (experimental == NULL || experimental->active
        || ((!forward && navigation->history_index == 0)
            || (forward
                && navigation->history_index + 1
                   >= navigation->history_count))) return true;
    size_t target_index = forward ? navigation->history_index + 1
                                  : navigation->history_index - 1;
    if (target_index >= 64
        || !experimental->history_section_known[target_index]) return true;
    const NavigationEntry *target = &navigation->history[target_index];
    const char *fragment = strchr(target->url, '#');
    size_t target_base = fragment == NULL ? strlen(target->url)
                                          : (size_t) (fragment - target->url);
    const char *locator_fragment = strchr(experimental->locator, '#');
    size_t locator_base = locator_fragment == NULL
        ? strlen(experimental->locator)
        : (size_t) (locator_fragment - experimental->locator);
    if (target_base != locator_base
        || memcmp(target->url, experimental->locator, target_base) != 0)
        return true;
    char target_url[NAVIGATION_URL_LIMIT];
    snprintf(target_url, sizeof(target_url), "%s", target->url);
    int restore_scroll = target->scroll_y;
    size_t selected = experimental->history_sections[target_index];
    const NavigationEntry *moved = NULL;
    if (!(forward ? navigation_forward(navigation, &moved)
                  : navigation_back(navigation, &moved))) return false;
    *handled = true;
    return experimental_rebuild_document(
        experimental, navigation, controller, selected, restore_scroll,
        target_url, user_css, "resume");
}

static bool loop_history(BrowserEngine *engine,
                         NavigationSession *navigation,
                         BrowserController *controller, bool forward,
                         const char *user_css)
{
    if (engine != NULL) {
        if (!browser_engine_history_move(engine, forward)
            || !apply_user_css(navigation, user_css)) return false;
        return controller_configure_viewport(controller, 272, 16);
    }
    const NavigationEntry *entry = NULL;
    bool moved = forward ? navigation_forward(navigation, &entry)
                         : navigation_back(navigation, &entry);
    if (!moved || entry == NULL) return false;
    char url[NAVIGATION_URL_LIMIT];
    snprintf(url, sizeof(url), "%s", entry->url);
    int scroll_y = entry->scroll_y;
    return loop_load(NULL, navigation, controller, url, false, scroll_y,
                     user_css);
}

static void loop_help(void)
{
    puts("loop commands: status, render [FILE], up|down [HELD], "
         "page-up|page-down, top|bottom, section-walk COUNT, "
         "focus-next|focus-prev, "
         "focus-up|focus-down|focus-left|focus-right, "
         "focus-id ID, anchor ID, node-text ID, node-attribute ID NAME, "
         "tap X Y, cross|activate, "
         "type TEXT, backspace, "
         "click SELECTOR, tick [COUNT [MS]], "
         "drain [MAX-TURNS [SETTLE-MS]], "
         "media-status|media-play|media-pause|media-seek SECONDS|"
         "media-open|media-close|media-run [COUNT [MS]], "
         "go URL, reload, "
         "back|circle, forward, mark-steady, help, quit");
    puts("PSP aliases: dpad-up/down scroll, dpad-left/right move focus, "
         "ltrigger goes back, rtrigger goes forward, cross activates");
}

/* One census of every quantity that a zero-time engine turn can move.  The
   drain treats "nothing in this census changed" as "the turn did no work",
   which is what distinguishes a genuinely idle engine from one that merely
   has no timer due at this instant. */
typedef struct {
    size_t relayouts;
    size_t loads;
    size_t timer_callbacks;
    size_t events;
    size_t mutations;
    size_t network_requests;
    size_t async_network_completed;
    size_t dynamic_scripts_completed;
    size_t scripts_evaluated;
} LoopDrainCensus;

static LoopDrainCensus loop_drain_census(const NavigationSession *navigation)
{
    const ScriptResult *result = &navigation->page.script_result;
    return (LoopDrainCensus) {
        .relayouts = navigation->incremental_relayouts,
        .loads = navigation->loads_committed,
        .timer_callbacks = result->timer_callbacks_run,
        .events = result->events_dispatched,
        .mutations = result->dom_mutations,
        .network_requests = result->network_requests,
        .async_network_completed = result->async_network_completed,
        .dynamic_scripts_completed = result->dynamic_scripts_completed,
        .scripts_evaluated = result->scripts_evaluated
    };
}

static bool loop_drain_census_equal(const LoopDrainCensus *a,
                                    const LoopDrainCensus *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

/* Work the runtime still owes at the current clock: pending network
   requests, in-flight async fetches, and queued dynamic scripts.
   script_result.pending_tasks is that set plus every registered timer, and
   root_timers is the timer count from the same refresh, so the difference
   is the non-timer backlog.  Timers scheduled for a future instant are not
   backlog: a drain never advances the clock, exactly like the reference
   capture, whose page clock is frozen while Playwright settles. */
static size_t loop_drain_pending_work(const NavigationSession *navigation)
{
    const ScriptResult *result = &navigation->page.script_result;
    size_t pending = result->pending_tasks > result->root_timers
        ? result->pending_tasks - result->root_timers : 0;
    return pending + result->async_network_pending_logical
           + result->root_pending_network;
}

/* One drain turn: a runtime step of elapsed_ms, the background resource and
   tile continuation, and the media session.  Reports whether anything the
   census tracks moved, and whether resource work is still outstanding. */
static bool loop_drain_turn(BrowserEngine *engine,
                            NavigationSession *navigation,
                            InteractiveMediaSession *media,
                            TileCache *cache, unsigned elapsed_ms,
                            bool *quiet)
{
    LoopDrainCensus before = loop_drain_census(navigation);
    if (!browser_engine_advance_runtime(engine, elapsed_ms, 8, NULL)) {
        return false;
    }
    /* Runs the background resource continuation, refreshes the render shell
       when it relaid out, and prepares idle tiles; returns whether any of
       that work remains. */
    bool work_pending = browser_engine_run_idle_work(engine);
    bool media_changed = false;
    if (!interactive_media_advance(media, navigation, cache, elapsed_ms,
                                   &media_changed)) {
        return false;
    }
    LoopDrainCensus after = loop_drain_census(navigation);
    *quiet = !media_changed
             && loop_drain_census_equal(&before, &after)
             && !work_pending
             && !navigation_background_resources_pending(navigation)
             && !browser_engine_navigation_pending(engine)
             && loop_drain_pending_work(navigation) == 0;
    return true;
}

/* Advance the engine until it stops doing work, or until one of two bounds
   is reached.  A quiet turn - one that runs no callback, completes no
   request, mutates no node, relays out nothing, and leaves no resource,
   tile, or navigation work outstanding - means the engine is idle at the
   current clock, which is the state capture-reference.js reads Chrome in.

   A page whose remaining work sits behind a scheduled callback is not idle
   even though no zero-time turn can move it: the engine schedules as timers
   several things a reference browser does during its own settle (a section
   collapse can arrive on a requestIdleCallback one millisecond out).  So when
   a quiet turn still leaves callbacks registered, the drain releases one
   frame quantum of them and resumes, spending at most settle_budget_ms of
   replay clock in total.  Periodic timers therefore cost a bounded amount
   of clock rather than looping forever.

   The printed line names the exit: reason=idle drained with nothing left
   scheduled, reason=clock-budget went quiet with callbacks still scheduled
   beyond the budget, reason=iteration-cap ran out of turns with work still
   moving. */
#define LOOP_DRAIN_QUANTUM_MS 16u

static bool loop_drain_until_idle(BrowserEngine *engine,
                                  NavigationSession *navigation,
                                  InteractiveMediaSession *media,
                                  TileCache *cache,
                                  unsigned maximum_iterations,
                                  unsigned settle_budget_ms)
{
    unsigned iterations = 0, settled_ms = 0;
    bool drained = false, ok = true;
    const char *reason = "iteration-cap";
    while (ok && iterations < maximum_iterations) {
        bool quiet = false;
        iterations++;
        ok = loop_drain_turn(engine, navigation, media, cache, 0, &quiet);
        if (!ok || !quiet) continue;
        if (navigation->page.script_result.root_timers == 0) {
            drained = true;
            reason = "idle";
            break;
        }
        if (settled_ms + LOOP_DRAIN_QUANTUM_MS > settle_budget_ms) {
            drained = true;
            reason = "clock-budget";
            break;
        }
        settled_ms += LOOP_DRAIN_QUANTUM_MS;
        ok = loop_drain_turn(engine, navigation, media, cache,
                             LOOP_DRAIN_QUANTUM_MS, &quiet);
    }
    const ScriptResult *result = &navigation->page.script_result;
    printf("loop-drain drained=%s reason=%s iterations=%u cap=%u "
           "settle-ms=%u/%u pending-work=%zu timers=%zu pending-network=%zu "
           "resources=%s navigation=%s relayouts=%zu\n",
           drained ? "yes" : "no", reason, iterations, maximum_iterations,
           settled_ms, settle_budget_ms,
           loop_drain_pending_work(navigation), result->root_timers,
           result->async_network_pending_logical
             + result->root_pending_network,
           navigation_background_resources_pending(navigation)
             ? "pending" : "idle",
           browser_engine_navigation_pending(engine) ? "pending" : "idle",
           navigation->incremental_relayouts);
    return ok;
}

static char *trim_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t length = strlen(line);
    while (length != 0 && (line[length - 1] == '\n'
                           || line[length - 1] == '\r'
                           || line[length - 1] == ' '
                           || line[length - 1] == '\t')) {
        line[--length] = '\0';
    }
    return line;
}

typedef struct {
    char path[2048];
    char input[4096];
    char text[1024];
    char reference[2048];
    char script[4096];
    char url[NAVIGATION_URL_LIMIT];
    ControllerAction action;
    ScriptResult script_result;
    size_t latency_samples;
    uint64_t latency_total_us;
    uint64_t latency_max_us;
    uint64_t latency_dispatch_us;
    uint64_t latency_cache_us;
    uint64_t latency_paint_us;
} InteractiveCommandScratch;

static bool run_command_loop(FILE *commands, bool interactive,
                             const char *output_dir, BrowserEngine *engine,
                             Budget *budget,
                             NavigationSession *navigation,
                             BrowserController *controller, TileCache *cache,
                             uint16_t *frame, size_t frame_pixels,
                             InteractiveMediaSession *media,
                             const char *user_css, bool capture_frames,
                             bool pace_real_time,
                             ExperimentalSectionContext *experimental,
                             VisualEvidenceFrames *visual_frames)
{
    if (commands == NULL || navigation == NULL || controller == NULL
        || output_dir == NULL || output_dir[0] == '\0') return false;
    if (capture_frames && mkdir(output_dir, 0755) != 0) {
        struct stat info;
        if (stat(output_dir, &info) != 0 || !S_ISDIR(info.st_mode)) {
            return false;
        }
    }
    InteractiveCommandScratch *scratch = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION, 1, sizeof(*scratch));
    if (scratch == NULL) return false;
    if (!loop_cache_owned(engine, cache, frame, frame_pixels)) {
        goto failed;
    }
    size_t frame_number = 0;
    bool steady = false;
    size_t steady_min = 0, steady_max = 0, steady_samples = 0;
    snprintf(scratch->path, sizeof(scratch->path),
             "%s/frame-%04zu.ppm", output_dir,
             frame_number);
    if (!loop_render(engine, cache, navigation, controller, media,
                     capture_frames ? scratch->path : NULL, NULL)) {
        goto failed;
    }
    if (!visual_evidence_record_frame(visual_frames, navigation)) goto failed;
    loop_print_status(controller, cache, frame_number++);
    if (interactive) loop_help();

    while (true) {
        if (interactive) { fputs("psp> ", stdout); fflush(stdout); }
        if (fgets(scratch->input, sizeof(scratch->input), commands) == NULL)
            break;
        char *line = trim_line(scratch->input);
        if (line[0] == '\0' || line[0] == '#') continue;
        uint64_t input_started = interaction_now_us();
        /* Input always wins over speculative work from the preceding frame.
           The latest scroll will schedule a replacement after presentation. */
        browser_engine_cancel_idle_work(engine);
        char *argument = strchr(line, ' ');
        if (argument != NULL) {
            *argument++ = '\0';
            while (*argument == ' ') argument++;
        }
        bool changed = true, reset_cache = false, ok = true;
        uint64_t network_before = navigation->performance.network_us;
        uint64_t script_before = navigation->performance.script_us;
        uint64_t runtime_before = navigation->performance.runtime_us;
        uint64_t relayout_before_us = navigation->performance.relayout_us;
        size_t relayouts_before = navigation->incremental_relayouts;
        size_t loads_before = navigation->loads_committed;
        uint64_t media_generation_before =
            media == NULL ? 0 : media->generation;
        size_t glyph_misses_before = cache->glyph_cache_misses;
        size_t image_decodes_before = cache->decoded_image_builds;
        size_t image_scales_before = cache->scaled_image_builds;
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
        else if (strcmp(line, "help") == 0) { loop_help(); changed = false; }
        else if (strcmp(line, "status") == 0) changed = false;
        else if (strcmp(line, "mark-steady") == 0) {
            steady = true;
            steady_min = steady_max = budget->current;
            steady_samples = 1;
            changed = false;
        }
        else if (strcmp(line, "up") == 0 || strcmp(line, "dpad-up") == 0) {
            unsigned held = argument == NULL ? 0u
                : (unsigned) strtoul(argument, NULL, 10);
            bool swapped = false;
            ok = (strcmp(line, "dpad-up") != 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_UP))
                 && experimental_scroll_boundary(
                        experimental, navigation, controller, -1, user_css,
                        &swapped)
                 && (swapped
                     || browser_engine_scroll_step(engine, -1, held));
            reset_cache = swapped;
        } else if (strcmp(line, "down") == 0
                   || strcmp(line, "dpad-down") == 0) {
            unsigned held = argument == NULL ? 0u
                : (unsigned) strtoul(argument, NULL, 10);
            bool swapped = false;
            ok = (strcmp(line, "dpad-down") != 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_DOWN))
                 && experimental_scroll_boundary(
                        experimental, navigation, controller, 1, user_css,
                        &swapped)
                 && (swapped
                     || browser_engine_scroll_step(engine, 1, held));
            reset_cache = swapped;
        } else if (strcmp(line, "page-up") == 0) {
            bool swapped = false;
            ok = experimental_scroll_boundary(
                    experimental, navigation, controller, -1, user_css,
                    &swapped)
                 && (swapped
                     || browser_engine_scroll_page(engine, -1));
            reset_cache = swapped;
        } else if (strcmp(line, "page-down") == 0) {
            bool swapped = false;
            ok = experimental_scroll_boundary(
                    experimental, navigation, controller, 1, user_css,
                    &swapped)
                 && (swapped
                     || browser_engine_scroll_page(engine, 1));
            reset_cache = swapped;
        } else if (strcmp(line, "top") == 0) {
            if (experimental != NULL && experimental->active
                && !experimental->body_source_replaced
                && experimental->pager.current_section != 0) {
                ok = experimental_commit_section(
                    experimental, navigation, controller, 0, 1, user_css);
                reset_cache = ok;
            } else {
                ok = browser_engine_scroll_to_edge(engine, false);
            }
        } else if (strcmp(line, "bottom") == 0) {
            if (experimental != NULL && experimental->active
                && !experimental->body_source_replaced
                && experimental->pager.current_section + 1
                   < experimental->pager.store->section_count) {
                size_t last = experimental->pager.store->section_count - 1;
                ok = experimental_commit_section(
                         experimental, navigation, controller, last, -1,
                         user_css)
                     && browser_engine_scroll_to_edge(engine, true);
                reset_cache = ok;
            } else {
                ok = browser_engine_scroll_to_edge(engine, true);
            }
        } else if (strcmp(line, "section-walk") == 0
                   && argument != NULL) {
            size_t count = strtoul(argument, NULL, 10);
            if (count > 256) count = 256;
            ok = experimental != NULL && experimental->active && count != 0;
            size_t start = ok ? experimental->pager.current_section : 0;
            size_t walked = 0;
            while (ok && !experimental->body_source_replaced
                   && walked < count
                   && experimental->pager.current_section + 1
                      < experimental->pager.store->section_count) {
                ok = experimental_commit_section(
                    experimental, navigation, controller,
                    experimental->pager.current_section + 1, 1, user_css);
                if (ok) walked++;
            }
            while (ok && experimental->pager.current_section > start) {
                ok = experimental_commit_section(
                    experimental, navigation, controller,
                    experimental->pager.current_section - 1, -1, user_css);
            }
            if (ok) {
                printf("experimental-walk start=%zu distance=%zu end=%zu "
                       "swaps=%zu\n", start, walked,
                       experimental->pager.current_section,
                       experimental->swaps);
            }
            reset_cache = ok;
        } else if (strcmp(line, "focus-next") == 0
                   || strcmp(line, "dpad-right") == 0) {
            if (strcmp(line, "dpad-right") == 0
                && media != NULL && media->ui.visible) {
                ok = host_platform_button(TILEFINCH_PLATFORM_BUTTON_RIGHT)
                  && interactive_media_button(
                         media, navigation, cache, PSP_UI_BUTTON_RIGHT);
            } else {
                ok = (strcmp(line, "dpad-right") != 0
                      || host_platform_button(TILEFINCH_PLATFORM_BUTTON_RIGHT))
                     && browser_engine_focus_move(engine, true);
            }
        } else if (strcmp(line, "focus-prev") == 0
                   || strcmp(line, "dpad-left") == 0) {
            if (strcmp(line, "dpad-left") == 0
                && media != NULL && media->ui.visible) {
                ok = host_platform_button(TILEFINCH_PLATFORM_BUTTON_LEFT)
                  && interactive_media_button(
                         media, navigation, cache, PSP_UI_BUTTON_LEFT);
            } else {
                ok = (strcmp(line, "dpad-left") != 0
                      || host_platform_button(TILEFINCH_PLATFORM_BUTTON_LEFT))
                     && browser_engine_focus_move(engine, false);
            }
        } else if (strcmp(line, "focus-up") == 0) {
            ok = browser_engine_focus_direction(
                engine, CONTROLLER_FOCUS_UP);
        } else if (strcmp(line, "focus-down") == 0) {
            ok = browser_engine_focus_direction(
                engine, CONTROLLER_FOCUS_DOWN);
        } else if (strcmp(line, "focus-left") == 0) {
            ok = browser_engine_focus_direction(
                engine, CONTROLLER_FOCUS_LEFT);
        } else if (strcmp(line, "focus-right") == 0) {
            ok = browser_engine_focus_direction(
                engine, CONTROLLER_FOCUS_RIGHT);
        } else if (strcmp(line, "focus-id") == 0 && argument != NULL) {
            lxb_dom_node_t *node = find_element_id(
                lxb_dom_interface_node(navigation->page.document.html),
                argument);
            ok = browser_engine_focus_node(engine, node);
        } else if (strcmp(line, "control-value") == 0
                   && argument != NULL) {
            lxb_dom_node_t *node = find_element_id(
                lxb_dom_interface_node(navigation->page.document.html),
                argument);
            size_t value_length = 0;
            const char *value = node == NULL ? NULL
                : document_control_value(node, &value_length);
            if (value == NULL && node != NULL) {
                value = document_attribute(node, "value", &value_length);
            }
            ok = value != NULL;
            if (ok) {
                printf("experimental-control id=\"%s\" value=\"%.*s\"\n",
                       argument, (int) value_length, value);
            }
        } else if (strcmp(line, "node-text") == 0 && argument != NULL) {
            lxb_dom_node_t *node = find_element_id(
                lxb_dom_interface_node(navigation->page.document.html),
                argument);
            scratch->text[0] = '\0';
            size_t used = 0;
            ok = node != NULL;
            if (ok) {
                collect_node_text(node->first_child, scratch->text,
                                  sizeof(scratch->text), &used);
                printf("experimental-node id=\"%s\" text=\"%s\"\n",
                       argument, scratch->text);
            }
        } else if (strcmp(line, "node-attribute") == 0
                   && argument != NULL) {
            char *name = strchr(argument, ' ');
            if (name != NULL) {
                *name++ = '\0';
                while (*name == ' ') name++;
            }
            lxb_dom_node_t *node = name == NULL || *name == '\0' ? NULL
                : find_element_id(
                    lxb_dom_interface_node(navigation->page.document.html),
                    argument);
            size_t value_length = 0;
            const char *value = node == NULL ? NULL
                : document_attribute(node, name, &value_length);
            ok = value != NULL;
            if (ok) {
                printf("experimental-node-attribute id=\"%s\" name=\"%s\" "
                       "value=\"%.*s\"\n", argument, name,
                       (int) value_length, value);
            }
        } else if (strcmp(line, "anchor") == 0 && argument != NULL) {
            int written = snprintf(
                scratch->reference, sizeof(scratch->reference), "#%s",
                argument);
            bool handled = false;
            ok = written > 0
                 && (size_t) written < sizeof(scratch->reference)
                 && experimental_anchor_reference(
                        experimental, navigation, controller,
                        scratch->reference, user_css, &handled);
            if (ok && !handled) {
                ok = loop_anchor_document(navigation, argument);
            }
            reset_cache = ok;
        } else if (strcmp(line, "tap") == 0 && argument != NULL) {
            char *end = NULL;
            long x = strtol(argument, &end, 10);
            long y = end == argument ? -1 : strtol(end, NULL, 10);
            if (media != NULL && media->ui.visible) {
                ok = x >= 0 && y >= 0
                  && interactive_media_activate_at(
                         media, navigation, cache, (int) x, (int) y);
            } else {
                ok = x >= 0 && y >= 0
                    && browser_engine_focus_at(engine, (int) x, (int) y);
            }
        } else if (strcmp(line, "type") == 0 && argument != NULL) {
            ok = browser_engine_insert_text(
                engine, argument, strlen(argument));
        } else if (strcmp(line, "backspace") == 0) {
            ok = browser_engine_backspace(engine);
        } else if (strcmp(line, "click") == 0 && argument != NULL) {
            ok = navigation_dispatch_event(navigation, argument, "click");
        } else if (strcmp(line, "js") == 0 && argument != NULL) {
            /* Lab diagnostic: evaluate an expression in the page realm and
               print its completion value (or error), so command scripts can
               inspect live DOM/app state during fidelity work. */
            memset(&scratch->script_result, 0,
                   sizeof(scratch->script_result));
            int written = snprintf(
                scratch->script, sizeof(scratch->script),
                "__tilefinchClipboardWrite(String((%s)));", argument);
            ok = written > 0
                 && (size_t) written < sizeof(scratch->script)
                 && navigation->page.runtime != NULL
                 && script_runtime_evaluate_diagnostic(
                        navigation->page.runtime, scratch->script,
                        "<loop-js>", &scratch->script_result);
            printf("loop-js ok=%s value=\"%.1024s\" error=\"%.256s\"\n",
                   ok ? "yes" : "no",
                   scratch->script_result.last_clipboard_text,
                   scratch->script_result.error);
            ok = true;
        } else if (strcmp(line, "tick") == 0) {
            unsigned count = 1, milliseconds = 16;
            if (argument != NULL) {
                char *end = NULL;
                count = (unsigned) strtoul(argument, &end, 10);
                if (end != NULL && *end != '\0') {
                    milliseconds = (unsigned) strtoul(end, NULL, 10);
                }
            }
            if (count > 10000) count = 10000;
            for (unsigned i = 0; ok && i < count; i++) {
                ok = browser_engine_advance_runtime(
                    engine, milliseconds, 8, NULL);
                bool media_changed = false;
                if (ok) {
                    ok = interactive_media_advance(
                        media, navigation, cache, milliseconds,
                        &media_changed);
                }
            }
        } else if (strcmp(line, "drain") == 0) {
            unsigned maximum = 2048, settle_ms = 750;
            if (argument != NULL) {
                char *end = NULL;
                unsigned long requested = strtoul(argument, &end, 10);
                if (requested != 0 && requested <= 100000) {
                    maximum = (unsigned) requested;
                }
                if (end != NULL && *end != '\0') {
                    unsigned long budget = strtoul(end, NULL, 10);
                    if (budget <= 600000) settle_ms = (unsigned) budget;
                }
            }
            ok = loop_drain_until_idle(engine, navigation, media, cache,
                                       maximum, settle_ms);
        } else if (strcmp(line, "media-status") == 0) {
            interactive_media_print_status(media);
            changed = false;
        } else if (strcmp(line, "media-play") == 0) {
            if (media == NULL || media->player == NULL) ok = false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            else {
                host_media_set_playing(media->player, true);
                ok = interactive_media_sync_ui(media);
            }
#endif
        } else if (strcmp(line, "media-pause") == 0) {
            if (media == NULL || media->player == NULL) ok = false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            else {
                host_media_set_playing(media->player, false);
                ok = interactive_media_sync_ui(media);
            }
#endif
        } else if (strcmp(line, "media-seek") == 0
                   && argument != NULL) {
            char *end = NULL;
            double seconds = strtod(argument, &end);
            ok = end != argument && end != NULL && *end == '\0'
                && seconds >= 0.0 && seconds <= 86400.0
                && interactive_media_seek(
                       media, navigation, cache,
                       (uint64_t) (seconds * 1000000.0));
        } else if (strcmp(line, "media-open") == 0) {
            ok = interactive_media_set_visible(
                media, navigation, true);
        } else if (strcmp(line, "media-close") == 0) {
            ok = interactive_media_set_visible(
                media, navigation, false);
        } else if (strcmp(line, "media-run") == 0) {
            unsigned count = 300, milliseconds = 67;
            if (argument != NULL) {
                char *end = NULL;
                count = (unsigned) strtoul(argument, &end, 10);
                if (end != NULL && *end != '\0') {
                    milliseconds = (unsigned) strtoul(end, NULL, 10);
                }
            }
            if (count > 10000) count = 10000;
            ok = media != NULL && media->player != NULL
                && milliseconds != 0;
            size_t first_media_frame = frame_number;
            uint64_t media_last_ns = tilefinch_platform_monotonic_time_ns();
            for (unsigned i = 0; ok && i < count; i++) {
                if (pace_real_time) {
                    struct timespec delay = {
                        .tv_sec = (time_t) (milliseconds / 1000u),
                        .tv_nsec = (long) (
                            (milliseconds % 1000u) * 1000000u)
                    };
                    nanosleep(&delay, NULL);
                }
                unsigned advance_ms = milliseconds;
                if (pace_real_time) {
                    uint64_t now_ns = tilefinch_platform_monotonic_time_ns();
                    uint64_t elapsed_ns = now_ns >= media_last_ns
                        ? now_ns - media_last_ns : 0;
                    uint64_t measured_ms =
                        (elapsed_ns + UINT64_C(500000)) / UINT64_C(1000000);
                    if (measured_ms == 0) measured_ms = 1;
                    if (measured_ms > 1000) measured_ms = 1000;
                    advance_ms = (unsigned) measured_ms;
                    media_last_ns = now_ns;
                }
                bool media_changed = false;
                ok = browser_engine_advance_runtime(
                         engine, advance_ms, 8, NULL)
                     && interactive_media_advance(
                         media, navigation, cache, advance_ms,
                         &media_changed);
                if (!ok) break;
                if (!media_changed) {
#ifdef TILEFINCH_HAVE_HOST_MEDIA
                    HostMediaStats media_stats;
                    if (host_media_get_stats(media->player, &media_stats)
                        && media_stats.ended) break;
#endif
                    continue;
                }
                snprintf(scratch->path, sizeof(scratch->path),
                         "%s/frame-%04zu.ppm", output_dir, frame_number);
                if (!loop_render(
                        engine, cache, navigation, controller, media,
                        capture_frames ? scratch->path : NULL, NULL)
                    || !visual_evidence_record_frame(
                        visual_frames, navigation)) {
                    ok = false;
                    break;
                }
                frame_number++;
            }
            printf("media-run captured=%zu tick-ms=%u\n",
                   frame_number - first_media_frame, milliseconds);
            interactive_media_print_status(media);
            changed = false;
        } else if (strcmp(line, "go") == 0 && argument != NULL) {
            bool handled = false;
            ok = experimental_anchor_reference(
                experimental, navigation, controller, argument, user_css,
                &handled);
            if (ok && !handled) {
                bool was_active = experimental != NULL
                    && experimental->active;
                size_t previous_section = was_active
                    ? experimental->pager.current_section : 0;
                const NavigationEntry *previous = navigation_current(
                    navigation);
                int previous_scroll = previous == NULL ? 0
                    : previous->scroll_y;
                if (was_active) document_backing_uninstall(navigation);
                bool loaded = loop_load(
                    engine, navigation, controller, argument, true, 0,
                    user_css);
                ok = experimental_finish_external_attempt(
                    experimental, navigation, controller, user_css,
                    was_active, previous_section, previous_scroll, loaded);
            }
            reset_cache = ok;
        } else if (strcmp(line, "reload") == 0) {
            if (experimental != NULL && experimental->active) {
                ok = experimental_reload_document(
                    experimental, navigation, controller, user_css);
            } else {
                const NavigationEntry *entry = navigation_current(navigation);
                int scroll_y = entry == NULL ? 0 : entry->scroll_y;
                snprintf(scratch->url, sizeof(scratch->url), "%s",
                         entry == NULL ? "" : entry->url);
                ok = entry != NULL && loop_load(
                    engine, navigation, controller, scratch->url, false,
                    scroll_y, user_css);
            }
            reset_cache = ok;
        } else if (strcmp(line, "back") == 0 || strcmp(line, "circle") == 0
                   || strcmp(line, "ltrigger") == 0) {
            uint32_t button = strcmp(line, "circle") == 0
                              ? TILEFINCH_PLATFORM_BUTTON_CIRCLE
                              : TILEFINCH_PLATFORM_BUTTON_LTRIGGER;
            bool requested = strcmp(line, "back") == 0
                             || host_platform_button(button);
            bool handled = requested
                && media != NULL && media->ui.visible;
            if (handled) {
                ok = interactive_media_button(
                    media, navigation, cache, PSP_UI_BUTTON_CANCEL);
            }
            if (!handled) {
                ok = requested && experimental_restore_history(
                    experimental, navigation, controller, false, user_css,
                    &handled);
            }
            if (ok && !handled) {
                if (experimental != NULL && experimental->active) {
                    size_t previous_section =
                        experimental->pager.current_section;
                    const NavigationEntry *previous = navigation_current(
                        navigation);
                    int previous_scroll = previous == NULL ? 0
                        : previous->scroll_y;
                    size_t previous_history = navigation->history_index;
                    document_backing_uninstall(navigation);
                    bool loaded = loop_history(
                        engine, navigation, controller, false, user_css);
                    if (!loaded
                        && navigation->history_index != previous_history) {
                        const NavigationEntry *ignored = NULL;
                        (void) navigation_forward(navigation, &ignored);
                    }
                    ok = experimental_finish_external_attempt(
                        experimental, navigation, controller, user_css, true,
                        previous_section, previous_scroll, loaded);
                } else {
                    ok = experimental_resume_history(
                        experimental, navigation, controller, false, user_css,
                        &handled);
                    if (ok && !handled) {
                        ok = loop_history(
                            engine, navigation, controller, false, user_css);
                    }
                }
            }
            reset_cache = ok;
        } else if (strcmp(line, "forward") == 0
                   || strcmp(line, "rtrigger") == 0) {
            bool requested = strcmp(line, "forward") == 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_RTRIGGER);
            bool handled = false;
            ok = requested && experimental_restore_history(
                experimental, navigation, controller, true, user_css,
                &handled);
            if (ok && !handled) {
                if (experimental != NULL && experimental->active) {
                    size_t previous_section =
                        experimental->pager.current_section;
                    const NavigationEntry *previous = navigation_current(
                        navigation);
                    int previous_scroll = previous == NULL ? 0
                        : previous->scroll_y;
                    size_t previous_history = navigation->history_index;
                    document_backing_uninstall(navigation);
                    bool loaded = loop_history(
                        engine, navigation, controller, true, user_css);
                    if (!loaded
                        && navigation->history_index != previous_history) {
                        const NavigationEntry *ignored = NULL;
                        (void) navigation_back(navigation, &ignored);
                    }
                    ok = experimental_finish_external_attempt(
                        experimental, navigation, controller, user_css, true,
                        previous_section, previous_scroll, loaded);
                } else {
                    ok = experimental_resume_history(
                        experimental, navigation, controller, true, user_css,
                        &handled);
                    if (ok && !handled) {
                        ok = loop_history(
                            engine, navigation, controller, true, user_css);
                    }
                }
            }
            reset_cache = ok;
        } else if (strcmp(line, "activate") == 0
                   || strcmp(line, "cross") == 0) {
            bool media_activation = media != NULL && media->ui.visible;
            memset(&scratch->action, 0, sizeof(scratch->action));
            ok = media_activation
                ? (strcmp(line, "activate") == 0
                   || host_platform_button(TILEFINCH_PLATFORM_BUTTON_CROSS))
                  && interactive_media_button(
                         media, navigation, cache, PSP_UI_BUTTON_CONFIRM)
                : (strcmp(line, "activate") == 0
                   || host_platform_button(TILEFINCH_PLATFORM_BUTTON_CROSS))
                  && browser_engine_activate(engine, &scratch->action);
            if (!media_activation && ok
                && scratch->action.type == CONTROLLER_ACTION_MEDIA) {
                char media_error[256] = {0};
                interactive_media_destroy(media);
                ok = media != NULL && interactive_media_open(
                    media, budget, navigation, scratch->action.url, false,
                    media_error, sizeof(media_error));
#ifdef TILEFINCH_HAVE_HOST_MEDIA
                if (ok && media->player != NULL)
                    host_media_set_playing(media->player, true);
#endif
                if (!ok && media_error[0] != '\0')
                    fprintf(stderr, "page video open failed: %s\n",
                            media_error);
                reset_cache = ok;
            } else if (!media_activation && ok
                && (scratch->action.type == CONTROLLER_ACTION_NAVIGATE
                       || scratch->action.type
                              == CONTROLLER_ACTION_FORM_SUBMIT)) {
                bool handled = false;
                if (scratch->action.type == CONTROLLER_ACTION_NAVIGATE) {
                    ok = experimental_anchor_reference(
                        experimental, navigation, controller,
                        scratch->action.url, user_css, &handled);
                }
                if (ok && !handled) {
                    bool was_active = experimental != NULL
                        && experimental->active;
                    size_t previous_section = was_active
                        ? experimental->pager.current_section : 0;
                    const NavigationEntry *previous = navigation_current(
                        navigation);
                    int previous_scroll = previous == NULL ? 0
                        : previous->scroll_y;
                    if (was_active) document_backing_uninstall(navigation);
                    bool loaded = browser_engine_execute_action(
                        engine, &scratch->action, 4 * MIB, 30000);
                    /* BrowserEngine already promotes a configured controller
                       and render shell. Reapplying an empty lab profile here
                       needlessly relayouts the newly committed document before
                       its first command-loop frame. */
                    if (loaded && user_css != NULL) {
                        loaded = loop_apply_profile(
                            navigation, controller, user_css)
                            && browser_engine_refresh_render_shell(engine);
                    }
                    ok = experimental_finish_external_attempt(
                        experimental, navigation, controller, user_css,
                        was_active, previous_section, previous_scroll,
                        loaded);
                }
                reset_cache = ok;
            }
        } else if (strcmp(line, "render") == 0) {
            if (argument != NULL && argument[0] != '\0') {
                ok = loop_render(
                    engine, cache, navigation, controller, media,
                    argument, NULL);
            }
            changed = false;
        } else {
            fprintf(stderr, "loop unknown-command=\"%s\"\n", line);
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "loop command-failed=\"%s\"\n", line);
            goto failed;
        }
        bool remote_swapped = false;
        if (!experimental_process_remote_elements(
                experimental, navigation, controller, user_css,
                &remote_swapped)) goto failed;
        if (remote_swapped) reset_cache = true;
        if (!interactive_media_sync_route(
                media, budget, navigation)) goto failed;
        if (media != NULL
            && media->generation != media_generation_before) {
            reset_cache = true;
        }
        uint64_t dispatch_finished = interaction_now_us();
        if (navigation->loads_committed != loads_before) reset_cache = true;
        if (!reset_cache
            && navigation->incremental_relayouts != relayouts_before) {
            ok = browser_engine_apply_layout_damage(engine);
            if (!ok) goto failed;
        }
        if (reset_cache
            && !loop_cache_reset(
                   engine, cache, frame, frame_pixels)) goto failed;
        uint64_t cache_finished = interaction_now_us();
        if (steady) {
            if (budget->current < steady_min) steady_min = budget->current;
            if (budget->current > steady_max) steady_max = budget->current;
            steady_samples++;
        }
        if (changed) {
            snprintf(scratch->path, sizeof(scratch->path),
                     "%s/frame-%04zu.ppm", output_dir, frame_number);
            InteractionPaintTiming paint = {0};
            if (!loop_render(engine, cache, navigation, controller, media,
                             capture_frames ? scratch->path : NULL,
                             &paint)) {
                goto failed;
            }
            uint64_t finished = interaction_now_us();
            uint64_t total_us = finished - input_started;
            uint64_t dispatch_us = dispatch_finished - input_started;
            uint64_t cache_us = cache_finished - dispatch_finished;
            printf("interaction-latency command=%s total-us=%llu "
                   "dispatch-us=%llu network-us=%llu script-us=%llu "
                   "runtime-us=%llu relayout-us=%llu cache-us=%llu "
                   "paint-us=%llu raster-us=%llu frame-us=%llu "
                   "focus-us=%llu present-us=%llu glyph-misses=%zu "
                   "image-decodes=%zu image-scales=%zu\n",
                   line, (unsigned long long) total_us,
                   (unsigned long long) dispatch_us,
                   (unsigned long long)
                     (navigation->performance.network_us - network_before),
                   (unsigned long long)
                     (navigation->performance.script_us - script_before),
                   (unsigned long long)
                     (navigation->performance.runtime_us - runtime_before),
                   (unsigned long long)
                     (navigation->performance.relayout_us
                      - relayout_before_us),
                   (unsigned long long) cache_us,
                   (unsigned long long) paint.total_us,
                   (unsigned long long) paint.raster_us,
                   (unsigned long long) paint.frame_us,
                   (unsigned long long) paint.focus_us,
                   (unsigned long long) paint.present_us,
                   cache->glyph_cache_misses >= glyph_misses_before
                     ? cache->glyph_cache_misses - glyph_misses_before
                     : cache->glyph_cache_misses,
                   cache->decoded_image_builds >= image_decodes_before
                     ? cache->decoded_image_builds - image_decodes_before
                     : cache->decoded_image_builds,
                   cache->scaled_image_builds >= image_scales_before
                     ? cache->scaled_image_builds - image_scales_before
                     : cache->scaled_image_builds);
            scratch->latency_samples++;
            scratch->latency_total_us += total_us;
            scratch->latency_dispatch_us += dispatch_us;
            scratch->latency_cache_us += cache_us;
            scratch->latency_paint_us += paint.total_us;
            if (total_us > scratch->latency_max_us) {
                scratch->latency_max_us = total_us;
            }
            experimental_record_first_tile(experimental);
            if (!visual_evidence_record_frame(visual_frames, navigation)) {
                goto failed;
            }
            loop_print_status(controller, cache, frame_number++);
        } else {
            loop_print_status(controller, cache, frame_number);
        }
    }
    printf("loop status=PASS frames=%zu output-dir=\"%s\"\n",
           frame_number, output_dir);
    printf("loop steady=%s samples=%zu min=%zu max=%zu growth=%zu\n",
           steady ? "yes" : "no", steady_samples, steady_min, steady_max,
           steady && steady_max >= steady_min ? steady_max - steady_min : 0);
    printf("interaction-latency-summary samples=%zu total-us=%llu "
           "max-us=%llu dispatch-us=%llu cache-us=%llu paint-us=%llu\n",
           scratch->latency_samples,
           (unsigned long long) scratch->latency_total_us,
           (unsigned long long) scratch->latency_max_us,
           (unsigned long long) scratch->latency_dispatch_us,
           (unsigned long long) scratch->latency_cache_us,
           (unsigned long long) scratch->latency_paint_us);
    budget_free(budget, scratch);
    return true;

failed:
    budget_free(budget, scratch);
    return false;
}
