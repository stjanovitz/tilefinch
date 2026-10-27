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

/* Site-census report (tools/site-census, `census` command): first-paint
   timebase, off-screen share of the retained layout, per-image decoded and
   painted sizes, and Reader suitability. Read-only; lab frontend only. */
static void interactive_census_report(BrowserEngine *engine,
                                      NavigationSession *navigation)
{
    const LayoutDocument *layout = &navigation->page.layout;
    int viewport_height = navigation->viewport.css_height > 0
        ? navigation->viewport.css_height : 272;
    printf("census-timebase navigation-start-us=%llu first-paint-us=%llu "
           "completion-us=%llu viewport-height=%d\n",
           (unsigned long long) navigation->navigation_started_us,
           (unsigned long long) navigation->performance.first_paint_us,
           (unsigned long long) navigation->performance.completion_us,
           viewport_height);
    size_t below = 0, below_two = 0;
    for (size_t i = 0; i < layout->count; i++) {
        if (layout->commands[i].y >= viewport_height) below++;
        if (layout->commands[i].y >= 2 * viewport_height) below_two++;
    }
    size_t boxes_below = 0, boxes_below_two = 0;
    for (size_t i = 0; i < layout->node_box_count; i++) {
        if (layout->node_boxes[i].y >= viewport_height) boxes_below++;
        if (layout->node_boxes[i].y >= 2 * viewport_height) boxes_below_two++;
    }
    printf("census-layout commands=%zu below=%zu below2=%zu command-size=%zu "
           "boxes=%zu boxes-below=%zu boxes-below2=%zu box-size=%zu "
           "document-height=%d\n",
           layout->count, below, below_two, sizeof(*layout->commands),
           layout->node_box_count, boxes_below, boxes_below_two,
           sizeof(*layout->node_boxes), layout->height);

    const ImageResources *images = &navigation->page.images;
    for (size_t i = 0; i < images->count; i++) {
        const ImageResource *image = &images->items[i];
        size_t painted_area = 0;
        int painted_width = 0, painted_height = 0, top = -1;
        size_t paints = 0;
        for (size_t c = 0; c < layout->count; c++) {
            const DrawCommand *command = &layout->commands[c];
            if (command->type != DRAW_IMAGE || command->image != image)
                continue;
            paints++;
            if (top < 0 || command->y < top) top = command->y;
            int w = command->width > 0 ? command->width : 0;
            int h = command->height > 0 ? command->height : 0;
            if ((size_t) w * (size_t) h > painted_area) {
                painted_area = (size_t) w * (size_t) h;
                painted_width = w;
                painted_height = h;
            }
        }
        size_t decoded = image->pixels == NULL ? 0
            : (size_t) (image->width > 0 ? image->width : 0)
                * (size_t) (image->height > 0 ? image->height : 0) * 4u;
        /* Several resources can alias one decoded surface (a background
           shared by many elements); `surface` names it within this report so
           the census counts its bytes once. */
        size_t surface = i;
        for (size_t j = 0; j < i; j++) {
            if (image->pixels != NULL
                && images->items[j].pixels == image->pixels) {
                surface = j;
                break;
            }
        }
        /* What backs the surface: SVG markup (rasterized again at the
           painted size), decoded pixels with their encoded bytes (a raster
           display retargeting can resize), encoded bytes only (decoded at
           paint), pixels alone (SVG rasters too small to keep their markup,
           adopted cache copies) or a canvas. */
        const char *origin = image->is_canvas ? "canvas"
            : (image->retarget_flags & IMAGE_RETARGET_VECTOR) != 0
                && image->encoded != NULL ? "vector"
            : image->pixels == NULL ? "encoded"
            : image->encoded != NULL ? "raster"
            : "pixels";
        printf("census-image index=%zu kind=%s source=%dx%d decoded=%dx%d "
               "decoded-bytes=%zu encoded-bytes=%zu painted=%dx%d paints=%zu "
               "top=%d surface=%zu origin=%s\n",
               i, image->is_canvas ? "canvas" : image->is_mask ? "mask"
                   : image->is_background ? "background" : "image",
               image->source_width, image->source_height,
               image->width, image->height, decoded, image->encoded_length,
               painted_width, painted_height, paints, top, surface, origin);
    }

    ReaderDocumentAnalysis analysis = {0};
    bool analyzed = browser_engine_analyze_reader(engine, &analysis);
    printf("census-reader analyzed=%s kind=%s high-confidence=%s "
           "visited=%u visible-text=%u extracted-bytes=%u extracted-nodes=%u "
           "listing-entries=%u bounded=%s truncated=%s blank=%s\n",
           analyzed ? "yes" : "no", reader_page_kind_name(analysis.kind),
           analysis.high_confidence ? "yes" : "no",
           (unsigned) analysis.visited_nodes,
           (unsigned) analysis.visible_text_bytes,
           (unsigned) analysis.extracted_bytes,
           (unsigned) analysis.extracted_nodes,
           (unsigned) analysis.listing_entries,
           analysis.bounded_out ? "yes" : "no",
           analysis.extraction_truncated ? "yes" : "no",
           browser_engine_page_is_visually_blank(engine) ? "yes" : "no");
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

/* Wall latency includes scheduling and I/O waits; this separate owner-thread
   clock prices the action's computation without charging other processes. */
static bool interaction_cpu_us(uint64_t *value)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec time;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0) {
        *value = (uint64_t) time.tv_sec * UINT64_C(1000000)
            + (uint64_t) time.tv_nsec / UINT64_C(1000);
        return true;
    }
#endif
    *value = 0;
    return false;
}

/* 64-bit FNV-1a over the presented frame's bytes (low byte first), so an
   A/B can prove frames identical without writing them out
   (tools/site-census --focus-moves). The frame-hash command and the
   interaction-latency frame-fnv1a field both use it, so they agree on
   identical pixels. */
static uint64_t loop_frame_hash(const uint16_t *frame, size_t pixels)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; frame != NULL && i < pixels; i++) {
        hash = (hash ^ (frame[i] & 0xffu)) * UINT64_C(1099511628211);
        hash = (hash ^ (frame[i] >> 8)) * UINT64_C(1099511628211);
    }
    return hash;
}

typedef struct {
    uint64_t raster_us;
    uint64_t frame_us;
    uint64_t focus_us;
    uint64_t present_us;
    uint64_t total_us;
    uint64_t setup_us, tiles_us, overflow_us, sticky_us, fixed_us;
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
    uint64_t setup_before = cache == NULL ? 0 : cache->frame_setup_us;
    uint64_t tiles_before = cache == NULL ? 0 : cache->frame_tile_us;
    uint64_t overflow_before = cache == NULL ? 0 : cache->frame_overflow_us;
    uint64_t sticky_before = cache == NULL ? 0 : cache->frame_sticky_us;
    uint64_t fixed_before = cache == NULL ? 0 : cache->frame_fixed_us;
    const NavigationEntry *entry = navigation_current(navigation);
    if (entry == NULL
        || !browser_engine_render_frame(engine, NULL)) {
        return false;
    }
    uint64_t focus_started = interaction_now_us();
    int x = 0, y = 0, width = 0, height = 0;
    (void) controller;
    if (browser_engine_focus_indicator_rect(
            engine, &x, &y, &width, &height)) {
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
        || render_write_frame_ppm(
            path, cache->frame, cache->frame_pixels, 480, 272);
    if (timing != NULL) {
        *timing = (InteractionPaintTiming) {
            .raster_us = cache->raster_us - raster_before,
            .frame_us = cache->frame_us - frame_before,
            .focus_us = focus_finished - focus_started,
            .present_us = present_finished - present_started,
            .total_us = interaction_now_us() - started,
            .setup_us = cache->frame_setup_us - setup_before,
            .tiles_us = cache->frame_tile_us - tiles_before,
            .overflow_us = cache->frame_overflow_us - overflow_before,
            .sticky_us = cache->frame_sticky_us - sticky_before,
            .fixed_us = cache->frame_fixed_us - fixed_before
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

static lxb_dom_node_t *loop_find_element_tag(lxb_dom_node_t *node,
                                             const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (; node != NULL; node = node->next) {
        size_t tag_length = 0;
        const char *tag = document_element_name(node, &tag_length);
        if (tag != NULL && tag_length == wanted_length
            && strncasecmp(tag, wanted, tag_length) == 0) return node;
        lxb_dom_node_t *found = loop_find_element_tag(
            node->first_child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
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

/* Every loop command, once: its help fragment (NULL when an earlier
   entry's fragment or the alias line covers it, or for private-console
   plumbing), whether it stands for a button press or typed address, and
   whether a private session may run it. Help, input classification and
   the private allowlist are all read from here. */
enum {
    /* A button press or a typed address: restarts the refresh loop guard,
       as a PSP press does. Waiting and inspection (tick, until, idle, drain,
       status, render...) are not input. */
    LAB_CMD_USER_INPUT = 1u << 0,
    /* Allowed in a private console session (a privacy boundary: anything
       that reads or prints page content stays off this list). */
    LAB_CMD_PRIVATE_OK = 1u << 1
};

typedef struct {
    const char *name;
    unsigned flags;
    const char *help;
} LabCommand;

#define LAB_INPUT LAB_CMD_USER_INPUT
#define LAB_PRIVATE LAB_CMD_PRIVATE_OK
static const LabCommand lab_commands[] = {
    {"status", LAB_PRIVATE, "status"},
    {"render", LAB_PRIVATE, "render [FILE]"},
    {"frame-hash", LAB_PRIVATE, "frame-hash [LABEL]"},
    {"up", LAB_INPUT | LAB_PRIVATE, "up|down [HELD]"},
    {"down", LAB_INPUT | LAB_PRIVATE, NULL},
    {"dpad-up", LAB_INPUT, NULL},
    {"dpad-down", LAB_INPUT, NULL},
    {"page-up", LAB_INPUT | LAB_PRIVATE, "page-up|page-down"},
    {"page-down", LAB_INPUT | LAB_PRIVATE, NULL},
    {"top", LAB_INPUT | LAB_PRIVATE, "top|bottom"},
    {"bottom", LAB_INPUT | LAB_PRIVATE, NULL},
    {"scroll-by", LAB_INPUT | LAB_PRIVATE, "scroll-by PIXELS"},
    {"focus-next", LAB_INPUT | LAB_PRIVATE, "focus-next|focus-prev"},
    {"focus-prev", LAB_INPUT | LAB_PRIVATE, NULL},
    {"dpad-right", LAB_INPUT, NULL},
    {"dpad-left", LAB_INPUT, NULL},
    {"tap", LAB_INPUT | LAB_PRIVATE, "tap X Y"},
    {"click", LAB_INPUT | LAB_PRIVATE, "click TARGET|X Y"},
    {"focus-up", LAB_INPUT | LAB_PRIVATE,
        "focus-up|focus-down|focus-left|focus-right"},
    {"focus-down", LAB_INPUT | LAB_PRIVATE, NULL},
    {"focus-left", LAB_INPUT | LAB_PRIVATE, NULL},
    {"focus-right", LAB_INPUT | LAB_PRIVATE, NULL},
    {"focus-id", LAB_INPUT | LAB_PRIVATE, "focus-id ID"},
    {"focus-url", LAB_INPUT, "focus-url URL"},
    {"link-urls", 0u, "link-urls"},
    {"body-contains", 0u, "body-contains TEXT"},
    {"history-controls", 0u, "history-controls"},
    {"control-value", 0u, "control-value ID"},
    {"anchor", 0u, "anchor ID"},
    {"node-text", 0u, "node-text ID"},
    {"text-checksum", 0u, "text-checksum TAG"},
    {"node-attribute", 0u, "node-attribute ID NAME"},
    {"cross", LAB_INPUT | LAB_PRIVATE, "cross|activate"},
    {"activate", LAB_INPUT | LAB_PRIVATE, NULL},
    {"type", LAB_INPUT, "type TEXT"},
    {"backspace", LAB_INPUT, "backspace"},
    {"profile", 0u, "profile [LABEL]"},
    {"work", 0u, "work [LABEL]"},
    {"census", 0u, "census"},
    {"js", 0u, "js EXPRESSION"},
    {"stop-scripts", 0u, "stop-scripts"},
    {"heavy-answer", LAB_PRIVATE, "heavy-answer run|stop"},
    {"tick", LAB_PRIVATE, "tick [COUNT [MS]]"},
    {"pump", LAB_PRIVATE, "pump [COUNT [MS]]"},
    {"until", 0u, "until MAX-TURNS MS EXPRESSION"},
    {"idle", LAB_PRIVATE, "idle [COUNT]"},
    {"drain", LAB_PRIVATE, "drain [MAX-TURNS [SETTLE-MS]]"},
    {"media-status", 0u,
        "media-status|media-play|media-pause|media-seek SECONDS|"
        "media-open|media-close|media-run [COUNT [MS]]|play-page-media"},
    {"media-play", 0u, NULL},
    {"media-pause", 0u, NULL},
    {"media-seek", 0u, NULL},
    {"media-open", 0u, NULL},
    {"media-close", 0u, NULL},
    {"media-run", 0u, NULL},
    {"play-page-media", 0u, NULL},
    {"reader-on", LAB_INPUT, "reader-on|reader-off"},
    {"reader-off", LAB_INPUT, NULL},
    {"basic-on", LAB_INPUT, "basic-on"},
    {"go", LAB_INPUT, "go URL"},
    {"reload", LAB_INPUT | LAB_PRIVATE, "reload"},
    {"back", LAB_INPUT | LAB_PRIVATE, "back|circle"},
    {"circle", LAB_INPUT, NULL},
    {"ltrigger", LAB_INPUT, NULL},
    {"forward", LAB_INPUT | LAB_PRIVATE, "forward"},
    {"rtrigger", LAB_INPUT, NULL},
    {"mark-steady", 0u, "mark-steady"},
    {"script-report", 0u, "script-report"},
    {"refresh-status", 0u, "refresh-status"},
    {"help", 0u, "help"},
    {"quit", LAB_PRIVATE, "quit"},
    {"exit", LAB_PRIVATE, NULL},
    /* Private-console plumbing (private_snapshot.inc), not for scripts. */
    {"private-text", LAB_INPUT | LAB_PRIVATE, NULL},
    {"private-focus", LAB_INPUT | LAB_PRIVATE, NULL},
    {"private-focus-action", LAB_INPUT | LAB_PRIVATE, NULL},
    {"private-feedback", LAB_PRIVATE, NULL},
    {"private-snapshot", LAB_PRIVATE, NULL},
    {"private-open", LAB_PRIVATE, NULL},
    {"private-export-youtube-login", LAB_PRIVATE, NULL},
};
#undef LAB_INPUT
#undef LAB_PRIVATE

static const LabCommand *lab_command_find(const char *name)
{
    for (size_t i = 0; i < sizeof(lab_commands) / sizeof(lab_commands[0]); i++)
        if (strcmp(name, lab_commands[i].name) == 0) return &lab_commands[i];
    return NULL;
}

static bool lab_command_has(const char *name, unsigned flag)
{
    const LabCommand *command = lab_command_find(name);
    return command != NULL && (command->flags & flag) != 0;
}

static void loop_help(void)
{
    fputs("loop commands: ", stdout);
    const char *separator = "";
    for (size_t i = 0; i < sizeof(lab_commands) / sizeof(lab_commands[0]); i++) {
        if (lab_commands[i].help == NULL) continue;
        printf("%s%s", separator, lab_commands[i].help);
        separator = ", ";
    }
    putchar('\n');
    puts("PSP aliases: dpad-up/down scroll, dpad-left/right move focus, "
         "ltrigger goes back, rtrigger goes forward, cross activates");
}

static bool loop_command_is_user_input(const char *line)
{
    return lab_command_has(line, LAB_CMD_USER_INPUT);
}

static const char *loop_refresh_state_name(NavigationRefreshState state)
{
    switch (state) {
    case NAVIGATION_REFRESH_NONE: return "none";
    case NAVIGATION_REFRESH_PENDING: return "pending";
    case NAVIGATION_REFRESH_DUE: return "due";
    case NAVIGATION_REFRESH_FOLLOWED: return "followed";
    case NAVIGATION_REFRESH_CANCELLED: return "cancelled";
    case NAVIGATION_REFRESH_STOPPED: return "stopped";
    case NAVIGATION_REFRESH_REFUSED: return "refused";
    case NAVIGATION_REFRESH_OFFERED: return "offered";
    }
    return "unknown";
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
    bool idle_visual_changed = false;
    bool work_pending = browser_engine_run_idle_work(
        engine, &idle_visual_changed);
    (void) idle_visual_changed;
    /* The PSP loop pumps the nearest-first image queue in its own frame
       slot after idle work; so does a drain turn. */
    bool deferred_visual_changed = false;
    (void) browser_engine_run_deferred_image_work(
        engine, &deferred_visual_changed);
    if (navigation->page.image_continuation_pending
        && navigation->page.deferred_image_targets != NULL)
        work_pending = true;
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

static void private_console_wipe(void *memory, size_t length)
{
    volatile unsigned char *bytes = memory;
    while (length-- != 0) *bytes++ = 0;
}

static unsigned private_script_retry_count;
static bool private_service_entry_attempted;
#include "private_snapshot.inc"

/* Explicit private-console sessions may opt in by supplying this owner-only
   file in their disposable working directory. Never load it in normal lab or
   device browsing, follow a symlink, or serialize its input/exception values. */
static void private_console_load_outcome_classifier(NavigationSession *navigation)
{
    int fd = open("outcome-classifier.js", O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return;
    struct stat file;
    bool valid = fstat(fd, &file) == 0 && S_ISREG(file.st_mode)
        && file.st_uid == getuid() && (file.st_mode & 0077) == 0
        && file.st_nlink == 1 && file.st_size > 0 && file.st_size <= 16384;
    char *source = valid ? budget_malloc(navigation->budget,
                                         (size_t) file.st_size + 1) : NULL;
    if (source != NULL) {
        size_t length = (size_t) file.st_size, at = 0;
        while (at < length) {
            ssize_t got = read(fd, source + at, length - at);
            if (got <= 0) break;
            at += (size_t) got;
        }
        source[length] = '\0';
        if (at == length)
            (void) script_runtime_private_outcome_classifier(
                navigation->page.runtime, source, length);
        private_console_wipe(source, length + 1);
        budget_free(navigation->budget, source);
    }
    close(fd);
}

static bool private_console_command_allowed(const char *command)
{
    return lab_command_has(command, LAB_CMD_PRIVATE_OK);
}

static bool private_console_status(int fd, size_t sequence,
                                   BrowserEngine *engine,
                                   const BrowserController *controller,
                                   bool command_ok)
{
    if (fd < 0) return true;
    ControllerTextInputInfo info = {0};
    bool editable = browser_engine_text_input_info(engine, &info)
        && info.editable;
    char origin[272] = "unavailable";
    /* Private opens intentionally leave history unchanged. Report the live
       document's authority, including after an authentication handoff. */
    if (!tilefinch_url_origin(controller->navigation->page.document_url,
                             origin, sizeof(origin)))
        snprintf(origin, sizeof(origin), "unavailable");
    const ScriptResult *js = &controller->navigation->page.script_result;
    char record[768];
    int length = snprintf(record, sizeof(record),
        "S %zu %u %u %u %zu %zu %zu %s %u %zu %zu %zu %zu %zu %zu %zu %u\n",
        sequence, controller->navigation->page.loaded ? 1u : 0u,
        editable ? 1u : 0u, (unsigned) controller->focus_kind,
        controller->focus_index,
        controller->navigation->page.layout.control_count,
        controller->navigation->page.script_result.pending_tasks, origin,
        command_ok ? 1u : 0u, js->network_requests, js->network_failures,
        js->async_network_completed, js->async_network_timed_out,
        js->uncaught_callback_errors, js->promise_rejections_created,
        js->event_handlers_invoked, js->last_event_cancelled ? 1u : 0u);
    if (length <= 0 || (size_t) length >= sizeof(record)) return false;
    size_t at = 0;
    while (at < (size_t) length) {
        ssize_t written = write(fd, record + at, (size_t) length - at);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        at += (size_t) written;
    }
    return true;
}

/* Only rendered, unambiguous fields qualify. Never expose field values or
   choose a hidden input merely because it has a familiar name. */
static bool private_console_focus_field(BrowserEngine *engine,
                                       NavigationSession *navigation,
                                       const char *kind)
{
    bool password = kind != NULL && strcmp(kind, "password") == 0;
    if (!password && (kind == NULL || strcmp(kind, "username") != 0))
        return false;
    lxb_dom_node_t *selected = NULL;
    unsigned best_rank = 0;
    size_t matches = 0;
    const LayoutDocument *layout = &navigation->page.layout;
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        if (control->type != CONTROL_INPUT) continue;
        size_t length = 0;
        const char *type = document_attribute(control->node, "type", &length);
        bool is_password = type != NULL && length == 8
            && strncasecmp(type, "password", 8) == 0;
        unsigned rank = 0;
        if (password) rank = is_password ? 2u : 0u;
        else if (!is_password) {
            if (type == NULL || length == 0
                || (length == 4 && strncasecmp(type, "text", 4) == 0)) rank = 1;
            if (type != NULL && length == 5
                && strncasecmp(type, "email", 5) == 0) rank = 2;
            const char *autocomplete = document_attribute(
                control->node, "autocomplete", &length);
            if (autocomplete != NULL && length == 8
                && strncasecmp(autocomplete, "username", 8) == 0) rank = 3;
        }
        if (rank == 0 || rank < best_rank) continue;
        if (rank > best_rank) { best_rank = rank; matches = 0; }
        selected = control->node;
        matches++;
    }
    return matches == 1 && browser_engine_focus_node(engine, selected);
}

/* Select a unique rendered button within an ID-labelled action container.
   This uses the ordinary controller focus/activation path, not a standalone
   selector-dispatched click that leaves the edited field focused. */
static bool private_console_focus_action(BrowserEngine *engine,
                                        NavigationSession *navigation,
                                        const char *container_id)
{
    if (container_id == NULL) return false;
    size_t wanted = strlen(container_id);
    if (wanted == 0 || wanted > 128) return false;
    lxb_dom_node_t *selected = NULL;
    size_t matches = 0;
    const LayoutDocument *layout = &navigation->page.layout;
    for (size_t i = 0; i < layout->control_count; i++) {
        const ControlRegion *control = &layout->controls[i];
        if (control->type != CONTROL_BUTTON) continue;
        const LayoutNodeBox *box = layout_box_for_node(layout, control->node);
        if (box == NULL || box->width <= 0 || box->height <= 0) continue;
        size_t length = 0;
        if (document_attribute(control->node, "disabled", &length) != NULL)
            continue;
        lxb_dom_node_t *node = control->node;
        for (size_t depth = 0; node != NULL && depth < 128;
             depth++, node = node->parent) {
            const char *id = document_attribute(node, "id", &length);
            if (id != NULL && length == wanted
                && memcmp(id, container_id, wanted) == 0) {
                selected = control->node;
                matches++;
                break;
            }
        }
    }
    return matches == 1 && browser_engine_focus_node(engine, selected);
}

/* Return only a validation flag, never the author message or field value.
   Reveal the associated feedback locally rather than capturing an unchanged
   top-of-form viewport that hides a response below the handheld screen. */
static bool private_console_reveal_feedback(
    NavigationSession *navigation, const BrowserController *controller)
{
    lxb_dom_node_t *node = controller_focused_node(controller);
    if (node == NULL) return false;
    size_t length = 0;
    const char *invalid = document_attribute(node, "aria-invalid", &length);
    if (invalid == NULL || length != 4
        || strncasecmp(invalid, "true", 4) != 0) return false;
    const LayoutDocument *layout = &navigation->page.layout;
    const LayoutNodeBox *box = layout_box_for_node(layout, node);
    if (box == NULL || box->width <= 0 || box->height <= 0) return false;
    const char *description = document_attribute(node, "aria-describedby", &length);
    size_t token = 0;
    while (description != NULL && token < length
           && !isspace((unsigned char) description[token])) token++;
    /* A bounded first IDREF is sufficient for the local validation preview.
       Only retained, visible layout boxes are considered; no DOM walk/eval. */
    if (description != NULL && token > 0 && token <= 256) {
        for (size_t i = 0; i < layout->node_box_count; i++) {
            const LayoutNodeBox *candidate = &layout->node_boxes[i];
            size_t id_length = 0;
            const char *id = document_attribute(candidate->node, "id", &id_length);
            if (id != NULL && id_length == token
                && memcmp(id, description, token) == 0
                && candidate->width > 0 && candidate->height > 0) {
                box = candidate;
                break;
            }
        }
    }
    int height = navigation->viewport.css_height > 0
        ? navigation->viewport.css_height : 272;
    int scroll = box->y > height / 3 ? box->y - height / 3 : 0;
    int maximum = viewport_max_scroll_css(&navigation->viewport, layout->height);
    return navigation_set_scroll(navigation, scroll < maximum ? scroll : maximum);
}

/* Hex transport preserves spaces and UTF-8 without placing credentials in
   argv, command files, or the normal whitespace-trimming text command. */
static bool private_console_set_text(BrowserEngine *engine,
                                     NavigationSession *navigation,
                                     const BrowserController *controller,
                                     const char *hex,
                                     char *text, size_t capacity)
{
    TilefinchUrl url;
    /* Private opens do not update history. Authorize against the committed
       document receiving the text, including after redirects and handoffs. */
    if (!navigation->page.loaded
        || !tilefinch_url_parse(navigation->page.document_url, &url)
        || !tilefinch_url_is_secure(&url)) return false;
    if (hex == NULL) return browser_engine_replace_text(engine, "", 0);
    size_t length = strlen(hex);
    if ((length & 1u) != 0 || length / 2 >= capacity) return false;
    bool valid = true;
    for (size_t i = 0; i < length / 2; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            unsigned char c = (unsigned char) hex[2 * i + j];
            unsigned digit = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit > 15) valid = false;
            value = value * 16 + digit;
        }
        text[i] = (char) value;
        if (value == 0 || value == '\n' || value == '\r') valid = false;
    }
    bool ok = valid && browser_engine_replace_text(engine, text, length / 2);
    if (ok) {
        /* A cancelled beforeinput can report successful event dispatch without
           accepting the value. Return only equality, never the field contents. */
        char observed[1024];
        size_t observed_length = 0;
        ok = controller_text_value(controller, observed,
                                   sizeof(observed), &observed_length)
            && observed_length == length / 2
            && memcmp(observed, text, observed_length) == 0;
        private_console_wipe(observed, sizeof(observed));
    }
    private_console_wipe(text, capacity);
    return ok;
}

/* The loop state a command handler reads (run_command_loop's arguments). */
typedef struct {
    BrowserEngine *engine;
    Budget *budget;
    NavigationSession *navigation;
    BrowserController *controller;
    TileCache *cache;
    InteractiveMediaSession *media;
    InteractiveCommandScratch *scratch;
    const char *output_dir;
    bool capture_frames;
    bool pace_real_time;
    VisualEvidenceFrames *visual_frames;
} LabCommandContext;

/* Media commands: the page's media element and the interactive player. False when LINE is not one of them
   (or lacks its required argument), so the loop's ladder goes on. */
static bool lab_cmd_media(const LabCommandContext *context,
                          const char *line, char *argument,
                          bool *changed, bool *reset_cache, bool *ok,
                          size_t *frame_number)
{
    BrowserEngine *engine = context->engine;
    Budget *budget = context->budget;
    NavigationSession *navigation = context->navigation;
    BrowserController *controller = context->controller;
    TileCache *cache = context->cache;
    InteractiveMediaSession *media = context->media;
    InteractiveCommandScratch *scratch = context->scratch;
    const char *output_dir = context->output_dir;
    bool capture_frames = context->capture_frames;
    bool pace_real_time = context->pace_real_time;
    VisualEvidenceFrames *visual_frames = context->visual_frames;
    if (strcmp(line, "play-page-media") == 0) {
        memset(&scratch->action, 0, sizeof(scratch->action));
        *ok = controller_build_document_media_action(
            controller, &scratch->action);
        if (*ok) {
            char media_error[256] = {0};
            interactive_media_destroy(media);
            *ok = media != NULL && interactive_media_open(
                media, budget, navigation, scratch->action.url, true,
                media_error, sizeof(media_error));
#ifdef TILEFINCH_HAVE_HOST_MEDIA
            if (*ok) host_media_set_playing(media->player, true);
#endif
            if (!*ok) fprintf(stderr, "page media open failed: %s\n", media_error);
        }
        *reset_cache = *ok;
    } else if (strcmp(line, "media-status") == 0) {
        interactive_media_print_status(media);
        *changed = false;
    } else if (strcmp(line, "media-play") == 0) {
        if (media == NULL || media->player == NULL) *ok = false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
        else {
            host_media_set_playing(media->player, true);
            *ok = interactive_media_sync_ui(media);
        }
#endif
    } else if (strcmp(line, "media-pause") == 0) {
        if (media == NULL || media->player == NULL) *ok = false;
#ifdef TILEFINCH_HAVE_HOST_MEDIA
        else {
            host_media_set_playing(media->player, false);
            *ok = interactive_media_sync_ui(media);
        }
#endif
    } else if (strcmp(line, "media-seek") == 0
               && argument != NULL) {
        char *end = NULL;
        double seconds = strtod(argument, &end);
        *ok = end != argument && end != NULL && *end == '\0'
            && seconds >= 0.0 && seconds <= 86400.0
            && interactive_media_seek(
                   media, navigation, cache,
                   (uint64_t) (seconds * 1000000.0));
    } else if (strcmp(line, "media-open") == 0) {
        *ok = interactive_media_set_visible(
            media, navigation, true);
    } else if (strcmp(line, "media-close") == 0) {
        *ok = interactive_media_set_visible(
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
        *ok = media != NULL && media->player != NULL
            && milliseconds != 0;
        size_t first_media_frame = *frame_number;
        uint64_t media_last_ns = tilefinch_platform_monotonic_time_ns();
        for (unsigned i = 0; *ok && i < count; i++) {
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
            *ok = browser_engine_advance_runtime(
                     engine, advance_ms, 8, NULL)
                 && interactive_media_advance(
                     media, navigation, cache, advance_ms,
                     &media_changed);
            if (!*ok) break;
            if (!media_changed) {
#ifdef TILEFINCH_HAVE_HOST_MEDIA
                HostMediaStats media_stats;
                if (host_media_get_stats(media->player, &media_stats)
                    && media_stats.ended) break;
#endif
                continue;
            }
            snprintf(scratch->path, sizeof(scratch->path),
                     "%s/frame-%04zu.ppm", output_dir, *frame_number);
            if (!loop_render(
                    engine, cache, navigation, controller, media,
                    capture_frames ? scratch->path : NULL, NULL)
                || !visual_evidence_record_frame(
                    visual_frames, navigation)) {
                *ok = false;
                break;
            }
            (*frame_number)++;
        }
        printf("media-run captured=%zu tick-ms=%u\n",
               *frame_number - first_media_frame, milliseconds);
        interactive_media_print_status(media);
        *changed = false;
    } else {
        return false;
    }
    return true;
}

/* Read-only inspection of the live document: links, body text, saved
   history controls, control values, text checksums, node text and
   attributes. False when LINE is not one of them
   (or lacks its required argument), so the loop's ladder goes on. */
static bool lab_cmd_inspect(const LabCommandContext *context,
                            const char *line, char *argument,
                            bool *changed, bool *reset_cache, bool *ok,
                            size_t *frame_number)
{
    NavigationSession *navigation = context->navigation;
    InteractiveCommandScratch *scratch = context->scratch;
    (void) reset_cache;
    (void) frame_number;
    if (strcmp(line, "link-urls") == 0) {
        const NavigationEntry *entry = navigation_current(navigation);
        const char *base = entry == NULL
            ? navigation->page.document_url : entry->url;
        for (size_t at = 0; at < navigation->page.layout.link_count; at++) {
            const LinkRegion *link = &navigation->page.layout.links[at];
            char resolved[NAVIGATION_URL_LIMIT];
            if (link->url != NULL
                && fetch_resolve_url(base, link->url, resolved,
                                     sizeof(resolved))) {
                printf("loop-link index=%zu url=\"%s\"\n", at,
                       resolved);
            }
        }
        *changed = false;
    } else if (strcmp(line, "body-contains") == 0
               && argument != NULL) {
        const char *body = document_body_text(
            &navigation->page.document);
        *ok = body != NULL && strstr(body, argument) != NULL;
        printf("loop-body-contains found=%s text=\"%s\"\n",
               *ok ? "yes" : "no", argument);
        *changed = false;
    } else if (strcmp(line, "history-controls") == 0) {
        const NavigationEntry *entry = navigation_current(navigation);
        printf("loop-history-controls count=%zu",
               entry == NULL ? 0u : entry->control_count);
        if (entry != NULL) {
            for (size_t at = 0; at < entry->control_count; at++) {
                printf(" index=%u value=\"%.*s\"",
                       (unsigned) entry->controls[at].control_index,
                       (int) entry->controls[at].value_length,
                       entry->controls[at].value);
            }
        }
        putchar('\n');
        *changed = false;
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
        *ok = value != NULL;
        if (*ok) {
            printf("experimental-control id=\"%s\" value=\"%.*s\"\n",
                   argument, (int) value_length, value);
        }
    } else if (strcmp(line, "text-checksum") == 0 && argument != NULL) {
        lxb_dom_node_t *node = loop_find_element_tag(
            lxb_dom_interface_node(navigation->page.document.html), argument);
        size_t bytes = 0;
        uint32_t hash = 0;
        *ok = node != NULL && diagnostic_text_checksum(node, &bytes, &hash);
        printf("loop-text-checksum ok=%s bytes=%zu hash=%u\n",
               *ok ? "yes" : "no", bytes, (unsigned) hash);
    } else if (strcmp(line, "node-text") == 0 && argument != NULL) {
        lxb_dom_node_t *node = find_element_id(
            lxb_dom_interface_node(navigation->page.document.html),
            argument);
        scratch->text[0] = '\0';
        size_t used = 0;
        *ok = node != NULL;
        if (*ok) {
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
        *ok = value != NULL;
        if (*ok) {
            printf("experimental-node-attribute id=\"%s\" name=\"%s\" "
                   "value=\"%.*s\"\n", argument, name,
                   (int) value_length, value);
        }
    } else {
        return false;
    }
    return true;
}

static bool run_command_loop(FILE *commands, bool interactive,
                             const char *output_dir, BrowserEngine *engine,
                             Budget *budget,
                             NavigationSession *navigation,
                             BrowserController *controller, TileCache *cache,
                             uint16_t *frame, size_t frame_pixels,
                             InteractiveMediaSession *media,
                             const char *user_css, bool capture_frames,
                             bool pace_real_time,
                             VisualEvidenceFrames *visual_frames,
                             int private_console_fd)
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
    bool reader_active = false;
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
    size_t private_sequence = 0;
    if (private_console_fd >= 0) {
        browser_session_cookie_diagnostics(browser_engine_session(engine), true, NULL);
        browser_engine_set_youtube_signin_diagnostics(engine, true);
    }
    if (private_console_fd >= 0 && navigation->page.runtime != NULL) {
        if (script_runtime_xhr_diagnostics(navigation->page.runtime, true, NULL))
            private_console_load_outcome_classifier(navigation);
    }
    if (!private_console_status(private_console_fd, private_sequence++,
                                engine, controller, true)) goto failed;
    if (interactive) loop_help();

    const LabCommandContext command = {
        .engine = engine, .budget = budget, .navigation = navigation,
        .controller = controller, .cache = cache, .media = media,
        .scratch = scratch, .output_dir = output_dir,
        .capture_frames = capture_frames, .pace_real_time = pace_real_time,
        .visual_frames = visual_frames
    };
    /* A pointer click on anything but a text field completes through the
       ordinary activation, as on the PSP, so submit buttons submit. */
    bool pointer_activation_pending = false;
    while (true) {
        if (pointer_activation_pending) {
            pointer_activation_pending = false;
            snprintf(scratch->input, sizeof(scratch->input), "activate");
        } else {
            if (interactive) { fputs("psp> ", stdout); fflush(stdout); }
            if (fgets(scratch->input, sizeof(scratch->input), commands)
                == NULL) break;
        }
        char *line = trim_line(scratch->input);
        if (line[0] == '\0' || line[0] == '#') continue;
        uint64_t input_started = interaction_now_us();
        uint64_t input_cpu = 0;
        bool input_cpu_valid = interaction_cpu_us(&input_cpu);
        /* Input always wins over speculative work from the preceding frame.
           The latest scroll will schedule a replacement after presentation. */
        browser_engine_cancel_idle_work(engine);
        char *argument = strchr(line, ' ');
        if (argument != NULL) {
            *argument++ = '\0';
            while (*argument == ' ') argument++;
        }
        if (private_console_fd >= 0
            && !private_console_command_allowed(line)) goto failed;
        if (loop_command_is_user_input(line))
            browser_engine_note_user_input(engine);
        bool changed = true, reset_cache = false, ok = true;
        bool private_command_ok = true;
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
#ifndef TILEFINCH_NO_TRACE
        uint64_t focus_outline_before = controller->focus_outline_us;
#endif
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) break;
        else if (strcmp(line, "help") == 0) { loop_help(); changed = false; }
        else if (strcmp(line, "status") == 0) changed = false;
        else if (strcmp(line, "script-report") == 0) {
            /* JavaScript heap and compile census for the page that is live
               now. The post-load report printed before the loop covers only
               the first document; a page that reloads itself during startup
               needs this to measure its replacement document. */
            const ScriptResult *js = &navigation->page.script_result;
            printf("script-report loads=%zu runtime=%s memory-rescue=%s "
                   "geometry-builds=%zu sync-layouts=%zu\n",
                   navigation->loads_committed,
                   navigation->page.runtime == NULL ? "none" : "live",
                   navigation_page_memory_rescued(navigation)
                       ? "yes" : "no",
                   js->geometry_snapshot_builds,
                   js->geometry_synchronous_layouts);
            ScriptHeavyState heavy;
            if (browser_engine_heavy_page(engine, &heavy)) {
                static const char *const classes[] = {
                    "none", "content", "shell", "over"
                };
                printf("heavy-page class=%s script-bytes=%zu "
                       "restored=%zu/%zu estimate-ms=%u waiting=%zu/%zu "
                       "waiting-ms=%u oversized=%zu/%zu "
                       "refused=%zu largest-unit=%zu memory-needed=%zu "
                       "memory-free=%zu visible-text=%zu answered=%s\n",
                       classes[(unsigned) heavy.page_class & 3u],
                       heavy.script_bytes, heavy.restored_bytes,
                       heavy.disk_restored_bytes,
                       (unsigned) heavy.estimate_ms,
                       heavy.waiting_scripts, heavy.waiting_bytes,
                       (unsigned) heavy.waiting_ms,
                       heavy.oversized_scripts, heavy.oversized_bytes,
                       heavy.refused_scripts,
                       heavy.largest_unit_bytes, heavy.memory_needed,
                       heavy.memory_free, heavy.visible_text_bytes,
                       !heavy.answered ? "no"
                           : heavy.allowed ? "run" : "refuse");
            }
            if (navigation->page.runtime != NULL) {
                script_runtime_report_memory(navigation->page.runtime,
                                             stdout);
            }
            printf("script-modules compiled=%zu compile-us=%llu "
                   "restored=%zu restore-us=%llu misses=%zu stores=%zu "
                   "skips=%zu restore-failures=%zu\n",
                   js->module_compile_count, js->module_compile_us,
                   js->module_bytecode_cache_hits,
                   js->module_bytecode_restore_us,
                   js->module_bytecode_cache_misses,
                   js->module_bytecode_cache_stores,
                   js->module_bytecode_cache_admission_skips,
                   js->module_bytecode_cache_restore_failures);
            printf("script-compile attempts=%zu source-bytes=%zu "
                   "total-us=%llu inline=%llu external=%llu module=%llu "
                   "max-us=%llu max-bytes=%zu\n",
                   js->host_compile_attempts, js->host_compile_source_bytes,
                   (unsigned long long) js->host_compile_total_us,
                   (unsigned long long) js->compile_us[
                       SCRIPT_COMPILE_SOURCE_INLINE],
                   (unsigned long long) js->compile_us[
                       SCRIPT_COMPILE_SOURCE_EXTERNAL],
                   (unsigned long long) js->compile_us[
                       SCRIPT_COMPILE_SOURCE_MODULE],
                   (unsigned long long) js->max_nonpreemptible_compile_us,
                   js->max_nonpreemptible_compile_bytes);
            changed = false;
        }
        else if (strcmp(line, "refresh-status") == 0) {
            const NavigationRefresh *refresh = &navigation->page.refresh;
            const NavigationRefreshStats *stats = &navigation->refresh;
            printf("loop-refresh state=%s delay=%u elapsed-ms=%llu "
                   "header=%d reload=%d touched=%d url=\"%s\" chain=%zu "
                   "declared=%zu followed=%zu cancelled=%zu stopped=%zu "
                   "refused=%zu offered=%zu\n",
                   loop_refresh_state_name(refresh->state),
                   (unsigned) refresh->delay_seconds,
                   (unsigned long long) refresh->elapsed_ms,
                   refresh->from_header ? 1 : 0, refresh->reload ? 1 : 0,
                   refresh->user_touched ? 1 : 0,
                   refresh->url, stats->chain, stats->declared,
                   stats->followed, stats->cancelled, stats->stopped,
                   stats->refused, stats->offered);
            changed = false;
        }
        else if (strcmp(line, "mark-steady") == 0) {
            steady = true;
            steady_min = steady_max = budget->current;
            steady_samples = 1;
            changed = false;
        }
        else if (strcmp(line, "up") == 0 || strcmp(line, "dpad-up") == 0) {
            unsigned held = argument == NULL ? 0u
                : (unsigned) strtoul(argument, NULL, 10);
            ok = (strcmp(line, "dpad-up") != 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_UP))
                 && browser_engine_scroll_step(engine, -1, held);
        } else if (strcmp(line, "down") == 0
                   || strcmp(line, "dpad-down") == 0) {
            unsigned held = argument == NULL ? 0u
                : (unsigned) strtoul(argument, NULL, 10);
            ok = (strcmp(line, "dpad-down") != 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_DOWN))
                 && browser_engine_scroll_step(engine, 1, held);
        } else if (strcmp(line, "scroll-by") == 0) {
            char *end = NULL;
            long delta = argument == NULL ? 0 : strtol(argument, &end, 10);
            /* Exact native capture positioning must not depend on sticky
               obstructions changing the page-down step. */
            ok = argument != NULL && end != argument && *end == '\0'
                 && delta >= -61440 && delta <= 61440
                 && browser_engine_scroll_by(engine, (int) delta);
        } else if (strcmp(line, "page-up") == 0) {
            ok = browser_engine_scroll_page(engine, -1);
        } else if (strcmp(line, "page-down") == 0) {
            ok = browser_engine_scroll_page(engine, 1);
        } else if (strcmp(line, "top") == 0) {
            ok = browser_engine_scroll_to_edge(engine, false);
        } else if (strcmp(line, "bottom") == 0) {
            ok = browser_engine_scroll_to_edge(engine, true);
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
        } else if (strcmp(line, "focus-url") == 0 && argument != NULL) {
            lxb_dom_node_t *node = NULL;
            const NavigationEntry *entry = navigation_current(navigation);
            const char *base = entry == NULL
                ? navigation->page.document_url : entry->url;
            for (size_t at = 0;
                 at < navigation->page.layout.link_count && node == NULL;
                 at++) {
                const LinkRegion *link = &navigation->page.layout.links[at];
                char resolved[NAVIGATION_URL_LIMIT];
                if (link->url != NULL
                    && fetch_resolve_url(base, link->url, resolved,
                                         sizeof(resolved))
                    && strcmp(resolved, argument) == 0) {
                    node = link->node;
                }
            }
            ok = browser_engine_focus_node(engine, node);
        } else if (lab_cmd_inspect(&command, line, argument, &changed,
                                   &reset_cache, &ok, &frame_number)) {
        } else if (strcmp(line, "anchor") == 0 && argument != NULL) {
            int written = snprintf(
                scratch->reference, sizeof(scratch->reference), "#%s",
                argument);
            ok = written > 0
                 && (size_t) written < sizeof(scratch->reference)
                 && loop_anchor_document(navigation, argument);
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
        } else if (strcmp(line, "click") == 0 && argument != NULL) {
            char *end = NULL;
            long x = strtol(argument, &end, 10);
            if (end != argument) {
                char *y_end = NULL;
                long y = strtol(end, &y_end, 10);
                while (y_end != NULL && *y_end == ' ') y_end++;
                bool activate_pointer = false, page_changed = false;
                ok = y_end != end && y_end != NULL && *y_end == '\0'
                    && x >= 0 && y >= 0
                    && browser_engine_pointer_event(
                        engine, CONTROLLER_POINTER_DOWN, (int) x, (int) y,
                        &activate_pointer, &page_changed)
                    && browser_engine_pointer_event(
                        engine, CONTROLLER_POINTER_UP, (int) x, (int) y,
                        &activate_pointer, &page_changed);
                if (ok && activate_pointer) {
                    ControllerTextInputInfo text_info = {0};
                    if (browser_engine_text_input_info(engine, &text_info)
                        && text_info.editable)
                        ok = browser_engine_pointer_commit_click(engine);
                    else pointer_activation_pending = true;
                }
            } else {
                ok = navigation_dispatch_event(
                    navigation, argument, "click");
            }
        } else if (strcmp(line, "type") == 0 && argument != NULL) {
            ok = browser_engine_insert_text(
                engine, argument, strlen(argument));
        } else if (strcmp(line, "private-focus") == 0
                   && private_console_fd >= 0) {
            ok = private_console_focus_field(engine, navigation, argument);
        } else if (strcmp(line, "private-feedback") == 0
                   && private_console_fd >= 0) {
            ok = private_console_reveal_feedback(navigation, controller);
        } else if (strcmp(line, "private-focus-action") == 0
                   && private_console_fd >= 0) {
            ok = private_console_focus_action(engine, navigation, argument);
        } else if (strcmp(line, "private-text") == 0
                   && private_console_fd >= 0) {
            ok = private_console_set_text(engine, navigation, controller,
                                          argument, scratch->text,
                                          sizeof(scratch->text));
        } else if (strcmp(line, "profile") == 0) {
            /* Print and clear the page realm's sampling profile (enable
               with TILEFINCH_TRACE_JS_PROFILE=1), then the work record. */
            script_runtime_profile_report(navigation->page.runtime,
                                          argument == NULL ? "lab" : argument);
            tilefinch_work_print(argument == NULL ? "lab" : argument,
                                 navigation);
            ok = true;
        } else if (strcmp(line, "heavy-answer") == 0 && argument != NULL) {
            /* Answer a heavy page's waiting scripts: "run" or "stop". */
            ok = browser_engine_answer_heavy_page(
                engine, strcmp(argument, "run") == 0);
            printf("heavy-answer %s ok=%s\n", argument, ok ? "yes" : "no");
        } else if (strcmp(line, "stop-scripts") == 0) {
            ok = browser_engine_stop_page_scripts(engine);
            printf("stop-scripts ok=%s\n", ok ? "yes" : "no");
        } else if (strcmp(line, "census") == 0) {
            interactive_census_report(engine, navigation);
            ok = true;
        } else if (strcmp(line, "work") == 0) {
            /* The deterministic work record (LAB_USAGE.md, "Work vector"). */
            tilefinch_work_print(argument == NULL ? "lab" : argument,
                                 navigation);
            ok = true;
        } else if (strcmp(line, "backspace") == 0) {
            ok = browser_engine_backspace(engine);
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
        } else if (strcmp(line, "until") == 0) {
            /* Match the device's readiness probe rather than executing a
               fixed tail of timers after the observed reply is present.
               Probe evaluation does not drain jobs or refresh the document.
               This is trusted lab input, not an author-facing API. */
            char *end = NULL;
            unsigned long count = 0, milliseconds = 0;
            ok = argument != NULL && isdigit((unsigned char) *argument);
            if (ok) {
                count = strtoul(argument, &end, 10);
                ok = end != argument && *end == ' '
                    && count != 0 && count <= 10000;
            }
            if (ok) {
                while (*end == ' ') end++;
                char *number = end;
                ok = isdigit((unsigned char) *number);
                if (ok) {
                    milliseconds = strtoul(number, &end, 10);
                    ok = end != number && *end == ' '
                        && milliseconds <= 60000;
                }
            }
            if (ok) {
                while (*end == ' ') end++;
                int written = snprintf(scratch->script,
                    sizeof(scratch->script),
                    "__tilefinchClipboardWrite((%s)?'1':'');", end);
                ok = *end != '\0' && written > 0
                    && (size_t) written < sizeof(scratch->script);
            }
            unsigned long iterations = 0;
            bool matched = false;
            for (; ok; iterations++) {
                memset(&scratch->script_result, 0,
                       sizeof(scratch->script_result));
                ok = navigation->page.runtime != NULL
                    && script_runtime_evaluate_probe(
                        navigation->page.runtime, scratch->script,
                        "<until-js>", &scratch->script_result);
                matched = ok
                    && scratch->script_result.last_clipboard_text[0] == '1';
                if (!ok || matched || iterations == count) break;
                if (pace_real_time && milliseconds != 0) {
                    struct timespec delay = {
                        .tv_sec = (time_t) (milliseconds / 1000u),
                        .tv_nsec = (long) ((milliseconds % 1000u) * 1000000u)
                    };
                    nanosleep(&delay, NULL);
                }
                ok = browser_engine_advance_runtime(
                    engine, (unsigned) milliseconds, 8, NULL);
                bool media_changed = false;
                if (ok) {
                    ok = interactive_media_advance(
                        media, navigation, cache, (unsigned) milliseconds,
                        &media_changed);
                }
            }
            printf("loop-until matched=%s iterations=%lu cap=%lu "
                   "probe-ok=%s\n", matched ? "yes" : "no", iterations,
                   count, ok ? "yes" : "no");
            ok = ok && matched;
        } else if (strcmp(line, "tick") == 0 || strcmp(line, "pump") == 0) {
            /* tick isolates runtime/media. pump follows an ordinary browser
               turn, including resource, cache and tile maintenance. Private
               sign-in must not test an artificial runtime-only schedule. */
            bool full_turn = strcmp(line, "pump") == 0;
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
                if (pace_real_time && milliseconds != 0) {
                    struct timespec delay = {
                        .tv_sec = (time_t) (milliseconds / 1000u),
                        .tv_nsec = (long) (
                            (milliseconds % 1000u) * 1000000u)
                    };
                    nanosleep(&delay, NULL);
                }
                if (full_turn) {
                    bool quiet = false;
                    ok = loop_drain_turn(engine, navigation, media, cache,
                                         milliseconds, &quiet);
                    continue;
                }
                ok = browser_engine_advance_runtime(
                    engine, milliseconds, 8, NULL);
                bool media_changed = false;
                if (ok) {
                    ok = interactive_media_advance(
                        media, navigation, cache, milliseconds,
                        &media_changed);
                }
            }
        } else if (strcmp(line, "idle") == 0) {
            /* One page-idle turn at a time, so a script can place the
               reader between turns (for example a provisional page's
               layout extensions while paging). */
            unsigned count = argument == NULL
                ? 1u : (unsigned) strtoul(argument, NULL, 10);
            if (count == 0 || count > 10000) count = 1;
            /* run_idle_work reports whether work remains, not failure:
               stop at the first turn that leaves none. */
            for (unsigned i = 0; ok && i < count; i++) {
                bool visual_changed = false;
                if (!browser_engine_run_idle_work(engine, &visual_changed))
                    break;
            }
            printf("idle provisional=%d limit=%d height=%d "
                   "extensions=%zu completions=%zu relayouts=%zu "
                   "relayout-us=%llu bounded=%zu bounded-fallbacks=%zu "
                   "bounded-script=%zu bounded-input=%zu "
                   "geometry-builds=%zu\n",
                   navigation->page.layout_provisional ? 1 : 0,
                   navigation->page.layout.content_limit_y,
                   navigation->page.layout.height,
                   navigation->performance.provisional_extensions,
                   navigation->performance.provisional_completions,
                   navigation->incremental_relayouts,
                   (unsigned long long)
                       navigation->performance.relayout_us,
                   navigation->performance.bounded_focus_relayouts,
                   navigation->performance.bounded_focus_fallbacks,
                   navigation->performance.bounded_focus_script_completions,
                   navigation->performance.bounded_focus_input_settles,
                   navigation->page.script_result.geometry_snapshot_builds);
            const LayoutPerformance *performance =
                &navigation->page.layout.performance;
            printf("idle-layout total=%lluus root=%lluus flow=%lluus "
                   "style=%lluus/%llu hits=%llu misses=%llu bidi=%lluus "
                   "compact=%lluus paint-order=%lluus spatial=%lluus "
                   "finalize=%lluus\n",
                   (unsigned long long) performance->total_us,
                   (unsigned long long) performance->root_style_us,
                   (unsigned long long) performance->flow_us,
                   (unsigned long long) performance->style_resolve_us,
                   (unsigned long long) performance->style_resolutions,
                   (unsigned long long) performance->style_cache_hits,
                   (unsigned long long) performance->style_cache_misses,
                   (unsigned long long) performance->bidi_us,
                   (unsigned long long) performance->compact_us,
                   (unsigned long long) performance->paint_order_us,
                   (unsigned long long) performance->spatial_index_us,
                   (unsigned long long) performance->finalize_us);
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
        } else if (lab_cmd_media(&command, line, argument, &changed,
                                   &reset_cache, &ok, &frame_number)) {
        } else if (strcmp(line, "reader-on") == 0
                   || strcmp(line, "reader-off") == 0) {
            bool enable = strcmp(line, "reader-on") == 0;
            char anchor_before[BROWSER_TEXT_ANCHOR_LIMIT + 1u];
            char anchor_after[BROWSER_TEXT_ANCHOR_LIMIT + 1u];
            int anchor_y = 0, ignored_y = 0;
            const NavigationEntry *entry = navigation_current(navigation);
            int scroll_before = entry == NULL ? 0 : entry->scroll_y;
            bool have_anchor = browser_engine_capture_text_anchor(
                engine, anchor_before, &anchor_y);
            ReaderDocumentAnalysis analysis = {0};
            if (enable) {
                char reader_css[SITE_ADAPTER_READER_CSS_LIMIT];
                char adapter[32];
                const char *reader_url = entry == NULL
                    ? navigation->page.document_url : entry->url;
                ok = !reader_active
                    && browser_engine_prepare_reader(engine, &analysis)
                    && site_adapter_reader_css(
                        reader_url, SITE_ADAPTER_READER_FONT_SANS, 100u,
                        reader_css, sizeof(reader_css), adapter,
                        sizeof(adapter))
                    && browser_engine_apply_user_css(
                        engine, reader_css, strlen(reader_css))
                    && browser_engine_activate_reader_view(engine);
            } else {
                ok = reader_active && browser_engine_apply_user_css(
                    engine, user_css, user_css == NULL ? 0 : strlen(user_css));
            }
            if (ok) {
                reader_active = enable;
                browser_engine_set_reader_candidate_mode(engine, enable);
                if (have_anchor) {
                    (void) browser_engine_restore_text_anchor(
                        engine, anchor_before, anchor_y);
                }
                entry = navigation_current(navigation);
                int scroll_after = entry == NULL ? 0 : entry->scroll_y;
                bool after_anchor = browser_engine_capture_text_anchor(
                    engine, anchor_after, &ignored_y);
                printf("loop-reader mode=%s before-y=%d after-y=%d "
                       "anchor-before=\"%s\" anchor-after=\"%s\"\n",
                       enable ? "reader" : "raw", scroll_before,
                       scroll_after, have_anchor ? anchor_before : "",
                       after_anchor ? anchor_after : "");
            }
        } else if (strcmp(line, "basic-on") == 0) {
            /* Manual Basic view, as Page tools -> Basic view does it. */
            ReaderDocumentAnalysis analysis = {0};
            const NavigationEntry *entry = navigation_current(navigation);
            const char *basic_url = entry == NULL
                ? navigation->page.document_url : entry->url;
            char basic_css[SITE_ADAPTER_READER_CSS_LIMIT];
            char adapter[32];
            uint64_t basic_started = tilefinch_platform_monotonic_time_us();
            bool prepared = !reader_active
                && browser_engine_prepare_basic_view(engine, &analysis);
            uint64_t basic_prepared = tilefinch_platform_monotonic_time_us();
            ok = prepared
                && site_adapter_reader_css(
                    basic_url, SITE_ADAPTER_READER_FONT_SANS, 100u,
                    basic_css, sizeof(basic_css), adapter, sizeof(adapter))
                && browser_engine_apply_user_css(
                    engine, basic_css, strlen(basic_css))
                && browser_engine_activate_basic_view(engine);
            printf("loop-basic prepared=%s activated=%s visited=%u "
                   "nodes=%u bytes=%u forms=%u truncated=%s bounded=%s "
                   "prepare-us=%llu text=%u\n",
                   prepared ? "yes" : "no", ok ? "yes" : "no",
                   (unsigned) analysis.visited_nodes,
                   (unsigned) analysis.extracted_nodes,
                   (unsigned) analysis.extracted_bytes,
                   (unsigned) analysis.retained_forms,
                   analysis.extraction_truncated ? "yes" : "no",
                   analysis.bounded_out ? "yes" : "no",
                   (unsigned long long) (basic_prepared - basic_started),
                   (unsigned) analysis.visible_text_bytes);
            if (ok) {
                reader_active = true;
                browser_engine_set_reader_candidate_mode(engine, false);
            }
        } else if (strcmp(line, "go") == 0 && argument != NULL) {
            /* Site-census ledger: split the records by visit. */
            if (tilefinch_trace_census()) puts("census-visit command=go");
            ok = loop_load(engine, navigation, controller, argument, true, 0,
                           user_css);
            reset_cache = ok;
        } else if (strcmp(line, "reload") == 0) {
            /* Site-census ledger: split the records by visit. */
            if (tilefinch_trace_census()) puts("census-visit command=reload");
            const NavigationEntry *entry = navigation_current(navigation);
            int scroll_y = entry == NULL ? 0 : entry->scroll_y;
            snprintf(scratch->url, sizeof(scratch->url), "%s",
                     entry == NULL ? "" : entry->url);
            ok = entry != NULL && loop_load(
                engine, navigation, controller, scratch->url, false,
                scroll_y, user_css);
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
                ok = requested && loop_history(
                    engine, navigation, controller, false, user_css);
            }
            reset_cache = ok;
        } else if (strcmp(line, "forward") == 0
                   || strcmp(line, "rtrigger") == 0) {
            bool requested = strcmp(line, "forward") == 0
                  || host_platform_button(TILEFINCH_PLATFORM_BUTTON_RTRIGGER);
            ok = requested && loop_history(
                engine, navigation, controller, true, user_css);
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
                ok = browser_engine_execute_action(
                    engine, &scratch->action, 4 * MIB, 30000);
                /* BrowserEngine already promotes a configured controller
                   and render shell. Reapplying an empty lab profile here
                   needlessly relayouts the newly committed document before
                   its first command-loop frame. */
                if (ok && user_css != NULL) {
                    ok = loop_apply_profile(navigation, controller, user_css)
                        && browser_engine_refresh_render_shell(engine);
                }
                reset_cache = ok;
            }
        } else if (strcmp(line, "private-export-youtube-login") == 0
                   && private_console_fd >= 0) {
            /* Explicit local test action only: fixed filename in the keeper's
               owner-only working directory, never a page-selected path. The
               production serializer excludes passwords and non-YouTube cookies. */
            struct stat directory;
            BrowserSession *session = browser_engine_session(engine);
            ok = (argument == NULL || argument[0] == '\0')
                && stat(".", &directory) == 0 && S_ISDIR(directory.st_mode)
                && directory.st_uid == getuid() && (directory.st_mode & 0077) == 0
                && youtube_lite_session_signed_in(session)
                && youtube_login_save(session, "youtube-device-session.bin", (int64_t) time(NULL));
            changed = false;
        } else if (strcmp(line, "private-snapshot") == 0
                   && private_console_fd >= 0) {
            ok = private_snapshot_page(engine, navigation, argument);
            changed = false;
        } else if (strcmp(line, "private-open") == 0
                   && private_console_fd >= 0) {
            TilefinchUrl target;
            bool feed = argument != NULL
                && (strcmp(argument, "https://www.youtube.com/feed/recommended") == 0
                    || strcmp(argument, "https://www.youtube.com/feed/subscriptions") == 0);
            if (feed) {
                /* The incoming sign-in continuation may have shed scripts
                   while the incumbent's large realm was still resident.
                   Once committed, that incumbent is gone. An explicit feed
                   check may retry this static document once through normal
                   navigation, without exporting its URL or session state.
                   Do not retry live realms, disabled scripting, or loop. */
                bool at_service = tilefinch_url_same_origin(
                    navigation->page.document_url, "https://m.youtube.com")
                    || tilefinch_url_same_origin(
                        navigation->page.document_url, "https://www.youtube.com");
                /* Existing service credentials are enough to try the native
                   feed, which verifies them with YouTube. Do not restart the
                   Google flow merely because its final document stayed there. */
                bool provider_ready = at_service || youtube_lite_session_signed_in(
                    browser_engine_session(engine));
                if (!provider_ready && private_script_retry_count < 2u
                    && navigation->page.loaded
                    && navigation->scripts_enabled
                    && navigation->document_scripts_enabled
                    && navigation->page.runtime == NULL
                    && navigation->page.script_degradation_observed) {
                    private_script_retry_count++;
                    char retry_url[NAVIGATION_URL_LIMIT];
                    snprintf(retry_url, sizeof(retry_url), "%s",
                             navigation->page.document_url);
                    ok = tilefinch_url_potentially_trustworthy(retry_url)
                        && loop_load(engine, navigation, controller,
                                     retry_url, false, 0, user_css);
                    reset_cache = ok;
                    changed = false;
                    /* Let ordinary owner turns complete the service's page
                       before another explicitly requested feed check. */
                } else if (!provider_ready && !private_service_entry_attempted) {
                    /* Authentication belongs to Google. YouTube destinations
                       always enter the scriptless provider, never its website. */
                    private_service_entry_attempted = true;
                    ok = loop_load(engine, navigation, controller,
                        YOUTUBE_LITE_SIGNIN_URL,
                        false, 0, user_css);
                    reset_cache = ok;
                    changed = false;
                } else {
                    /* Once signed in, qualify the actual lightweight UI, not
                       a second data-only implementation. This local command
                       retains the session and records no navigation history. */
                    ok = loop_load(engine, navigation, controller, argument,
                                   false, 0, user_css);
                    reset_cache = ok;
                    changed = ok; /* Paint Home before its first lazy feed turn. */
                }
            } else {
                ok = argument != NULL && tilefinch_url_parse(argument, &target)
                    && tilefinch_url_potentially_trustworthy(argument)
                    && loop_load(engine, navigation, controller, argument,
                                 false, 0, user_css);
                /* Reuse the ordinary in-memory cookie jar and navigation policy;
                   never serialize session credentials or record this as history. */
                reset_cache = ok;
            }
            /* These explicit opens replace the document without adding a
               history entry. Its previous scroll is not a restore request:
               retaining it can incorrectly mark a new lazy feed as browsed. */
            if (ok) ok = controller_scroll_to_top(controller, 272);
        } else if (strcmp(line, "render") == 0) {
            if (argument != NULL && argument[0] != '\0') {
                ok = loop_render(
                    engine, cache, navigation, controller, media,
                    argument, NULL);
            }
            changed = false;
        } else if (strcmp(line, "frame-hash") == 0) {
            /* The presented frame's FNV-1a, so a census can compare the
               frame after every input without writing it out. */
            ok = loop_render(engine, cache, navigation, controller, media,
                             NULL, NULL);
            if (ok) {
                printf("loop-frame-hash label=%s hash=%016llx\n",
                       argument == NULL ? "-" : argument,
                       (unsigned long long) loop_frame_hash(
                           cache->frame, cache->frame_pixels));
            }
            changed = false;
        } else {
            fprintf(stderr, "loop unknown-command=\"%s\"\n", line);
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "loop command-failed=\"%s\"\n", line);
            if (private_console_fd < 0 || !navigation->page.loaded) goto failed;
            /* A missing field or refused edit is recoverable in the local UI.
               Acknowledge it without leaking the command or its value. */
            private_command_ok = false;
            changed = false;
        }
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
            uint64_t finished_cpu = 0;
            bool cpu_valid = interaction_cpu_us(&finished_cpu)
                && input_cpu_valid && finished_cpu >= input_cpu;
            uint64_t total_us = finished - input_started;
            uint64_t dispatch_us = dispatch_finished - input_started;
            uint64_t cache_us = cache_finished - dispatch_finished;
            printf("interaction-latency command=%s total-us=%llu "
                   "cpu-us=%llu cpu-clock-valid=%d "
                   "dispatch-us=%llu network-us=%llu script-us=%llu "
                   "runtime-us=%llu relayout-us=%llu cache-us=%llu "
                   "paint-us=%llu raster-us=%llu frame-us=%llu "
                   "focus-us=%llu present-us=%llu glyph-misses=%zu "
                   "image-decodes=%zu image-scales=%zu",
                   line, (unsigned long long) total_us,
                   (unsigned long long) (cpu_valid ? finished_cpu - input_cpu : 0),
                   (int) cpu_valid,
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
#ifndef TILEFINCH_NO_TRACE
            printf(" focus-outline-us=%llu", (unsigned long long)
                   (controller->focus_outline_us >= focus_outline_before
                       ? controller->focus_outline_us - focus_outline_before
                       : controller->focus_outline_us));
#endif
            printf(" setup-us=%llu tiles-us=%llu overflow-us=%llu sticky-us=%llu fixed-us=%llu",
                   (unsigned long long) paint.setup_us, (unsigned long long) paint.tiles_us,
                   (unsigned long long) paint.overflow_us, (unsigned long long) paint.sticky_us,
                   (unsigned long long) paint.fixed_us);
#ifndef TILEFINCH_NO_TRACE
            printf(" frame-fnv1a=%016llx",
                   (unsigned long long) loop_frame_hash(cache->frame,
                                                   cache->frame_pixels));
            /* Cumulative over the page: script geometry snapshot builds
               and synchronous layouts (the bounded focus A/B reads the
               difference across a run of moves). */
            printf(" geometry-builds=%zu sync-layouts=%zu",
                   navigation->page.script_result.geometry_snapshot_builds,
                   navigation->page.script_result.geometry_synchronous_layouts);
#endif
            putchar('\n');
            scratch->latency_samples++;
            scratch->latency_total_us += total_us;
            scratch->latency_dispatch_us += dispatch_us;
            scratch->latency_cache_us += cache_us;
            scratch->latency_paint_us += paint.total_us;
            if (total_us > scratch->latency_max_us) {
                scratch->latency_max_us = total_us;
            }
            if (!visual_evidence_record_frame(visual_frames, navigation)) {
                goto failed;
            }
            loop_print_status(controller, cache, frame_number++);
        } else {
            loop_print_status(controller, cache, frame_number);
        }
        if (private_console_fd >= 0 && !pointer_activation_pending) {
            private_console_wipe(scratch->input, sizeof(scratch->input));
            if (!private_console_status(private_console_fd, private_sequence++,
                                        engine, controller, private_command_ok)) goto failed;
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
    if (private_console_fd >= 0) private_console_wipe(scratch, sizeof(*scratch));
    budget_free(budget, scratch);
    return true;

failed:
    if (private_console_fd >= 0) private_console_wipe(scratch, sizeof(*scratch));
    budget_free(budget, scratch);
    return false;
}
