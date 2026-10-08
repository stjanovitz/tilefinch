/* Uses the public engine receivers and real raster jobs: a focus-node change
   alone is not evidence that the user can see the next result. */
int test_deferred_startup_journey(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = 12;
    char error[256] = {0};
    for (unsigned mutation = 0; mutation < 8; mutation++) {
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine != NULL && fetch_trace_replay_begin(mutation == 7
            ? TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-image-failure"
            : mutation >= 4
            ? TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-image-order"
            : TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images", error, sizeof(error)));
        CHECK(browser_engine_load_url(engine, "https://interactive-images.test/page", true));
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(nav->page.loaded && nav->page.image_continuation_pending
            && nav->page.deferred_image_count == (mutation >= 4 && mutation < 7 ? 5u : 3u)
            && nav->page.runtime != NULL);
        lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
        lxb_dom_node_t *hero = test_reader_find_id(root, "hero");
        lxb_dom_node_t *tail = test_reader_find_id(root, "tail");
        CHECK(hero != NULL && tail != NULL
            && nav->page.images.stats.loaded == 1 /* inline SVG stays eager */
            && images_find_node(&nav->page.images, hero) == NULL
            && browser_engine_render_frame(engine, NULL)
            && browser_engine_focus_node(engine, test_reader_find_id(root, "next")));
        ControllerAction action = {0};
        CHECK(browser_engine_activate(engine, &action)
            && action.type == CONTROLLER_ACTION_NAVIGATE
            && strstr(action.url, "/next") != NULL);
        int collapsed_height = nav->page.layout.height;
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "toggle"))
            && browser_engine_activate(engine, &action)
            && action.type == CONTROLLER_ACTION_CONTROL
            && nav->page.layout.height > collapsed_height
            && browser_engine_render_frame(engine, NULL)
            && nav->page.image_continuation_pending);
        if (mutation == 0) {
            CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
                "globalThis.startupTimer=setInterval(()=>{},1)",
                "<startup-idle-priority>", &nav->page.script_result));
            CHECK(browser_engine_advance_runtime(engine, 16, 4, NULL));
            CHECK(script_runtime_has_pending_author_work(nav->page.runtime)
                && !script_runtime_has_pending_startup_scripts(nav->page.runtime)
                && nav->page.script_result.runtime_ticks < 128u);
            CHECK(browser_engine_focus_move(engine, true)
                && browser_engine_render_frame(engine, NULL));
            /* An endless timer must not impose a 128-turn resource delay. */
            bool changed = false;
            CHECK(browser_engine_run_idle_work(engine, &changed)
                && nav->page.deferred_image_job != NULL);
            CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
                "clearInterval(startupTimer)", "<startup-idle-finished>",
                &nav->page.script_result));
        }
        if (mutation == 7) {
            for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
                bool changed = false;
                (void) browser_engine_run_deferred_image_work(engine, &changed);
                CHECK(nav->page.loaded && browser_engine_focus_move(engine, true)
                    && browser_engine_render_frame(engine, NULL));
            }
            CHECK(!nav->page.image_continuation_pending
                && !image_resource_available(images_find_node(&nav->page.images, hero))
                && image_resource_available(images_find_node(&nav->page.images, tail))
                && nav->performance.background_image_failures > 0
                && browser_engine_last_error(engine)[0] == '\0');
            goto teardown;
        }
        if (mutation >= 5) {
            /* All eager images are outside this viewport. The nearest two
               still share the bounded batch; hidden/lazy targets do not
               become eligible merely to fill it. */
            CHECK(navigation_set_scroll(nav, mutation == 5 ? 500 : 800)
                && navigation_run_deferred_image_work(nav));
            CHECK(nav->page.deferred_image_batch_count == (mutation == 5 ? 2u : 1u)
                && nav->page.deferred_image_targets[0].node == hero
                && nav->page.deferred_image_targets[1].node
                    == test_reader_find_id(root, "last"));
            /* At 800 the hero is visible and the next image is not. Keep
               that first publication independent of its offscreen peer. */
            if (mutation == 6) goto teardown;
            for (unsigned pump = 0; nav->page.image_continuation_pending
                    && pump < 512; pump++) {
                bool changed = false;
                (void) browser_engine_run_deferred_image_work(engine, &changed);
                CHECK(nav->page.loaded && browser_engine_last_error(engine)[0] == '\0');
            }
            CHECK(!nav->page.image_continuation_pending
                && nav->performance.background_images_loaded == 3u
                && nav->performance.background_image_relayouts == 2u
                && images_find_node(&nav->page.images,
                    test_reader_find_id(root, "hidden")) == NULL
                && images_find_node(&nav->page.images,
                    test_reader_find_id(root, "lazy")) == NULL
                && browser_engine_render_frame(engine, NULL));
            goto teardown;
        }
        if (mutation == 4) {
            lxb_dom_node_t *last = test_reader_find_id(root, "last");
            lxb_dom_node_t *lazy = test_reader_find_id(root, "lazy");
            lxb_dom_node_t *hidden = test_reader_find_id(root, "hidden");
            CHECK(navigation_run_deferred_image_work(nav));
            CHECK(nav->page.deferred_image_targets[0].node == tail);
            CHECK(nav->page.deferred_image_targets[1].node == last
                && nav->page.deferred_image_targets[2].node == hero);
            for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
                bool changed = false;
                (void) browser_engine_run_deferred_image_work(engine, &changed);
            }
            CHECK(!nav->page.image_continuation_pending
                && image_resource_available(images_find_node(&nav->page.images, tail))
                && nav->performance.background_image_relayouts == 2);
            fprintf(stderr, "image-queue-settle: loaded=%zu relayouts=%zu rank-scans=%zu layout-us=%llu\n",
                nav->performance.background_images_loaded,
                nav->performance.background_image_relayouts,
                nav->performance.background_image_rank_scans,
                (unsigned long long) nav->performance.layout_us);
            {
                CHECK(images_find_node(&nav->page.images, hidden) == NULL
                    && images_find_node(&nav->page.images, lazy) == NULL);
                CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
                    "document.getElementById('hidden-parent').style='display:block'",
                    "<image-queue-reveal>", &nav->page.script_result));
                bool revealed = navigation_relayout(nav);
                CHECK(revealed
                    && images_find_node(&nav->page.images, hidden) == NULL
                    && images_find_node(&nav->page.images, lazy) == NULL
                    && navigation_set_scroll(nav, 1500));
                for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
                    bool changed = false;
                    (void) browser_engine_run_deferred_image_work(engine, &changed);
                }
                CHECK(image_resource_available(images_find_node(&nav->page.images, hidden))
                    && image_resource_available(images_find_node(&nav->page.images, lazy)));
            }
            goto teardown;
        }
        CHECK(navigation_prioritize_deferred_image(nav, hero)
            && navigation_run_deferred_image_work(nav)
            && nav->page.deferred_image_job != NULL);
        /* Navigation/teardown must cancel a live batch without publishing
           through its borrowed DOM targets. */
        if (mutation == 3) goto teardown;
        if (mutation != 0) {
            CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
                mutation == 1 ? "document.getElementById('first').textContent=''"
                    : "document.getElementById('hero').setAttribute('src','/changed.svg')",
                "<deferred-image-mutation>", &nav->page.script_result));
            CHECK(navigation_run_deferred_image_work(nav)
                && nav->page.deferred_image_job == NULL);
        }
        for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
            bool changed = false;
            (void) browser_engine_run_deferred_image_work(engine, &changed);
            CHECK(nav->page.loaded);
        }
        if (nav->page.image_continuation_pending
            || !image_resource_available(images_find_node(&nav->page.images, tail)))
            fprintf(stderr, "deferred startup mutation=%u pending=%d cursor=%zu/%zu loaded=%zu attempts=%zu failed=%zu background-fail=%zu error=%s\n",
                mutation, nav->page.image_continuation_pending,
                nav->page.deferred_image_cursor, nav->page.deferred_image_count,
                nav->page.images.stats.loaded, nav->page.images.stats.attempted,
                nav->page.images.stats.failed, nav->performance.background_image_failures,
                nav->last_error);
        CHECK(!nav->page.image_continuation_pending
            && image_resource_available(images_find_node(&nav->page.images, tail))
            && browser_engine_render_frame(engine, NULL));
        if (mutation == 1) {
            CHECK(test_reader_find_id(root, "hero") == NULL);
        } else {
            const ImageResource *image = images_find_node(&nav->page.images, hero);
            CHECK(image_resource_available(image) && image->pixels != NULL
                && image->pixels[0] == 255
                && image->pixels[1] == (mutation == 2 ? 255 : 0));
        }
teardown:
        fetch_trace_end();
        CHECK(browser_engine_shutdown(engine));
        BrowserEngineMetrics metrics = {0};
        CHECK(browser_engine_metrics(engine, &metrics)
            && metrics.budget_current == 0 && metrics.budget_active_allocations == 0);
        browser_engine_destroy(engine);
    }
    return 0;
}

/* A batch whose resources are already committed must survive a publication
   relayout that rebuilds the image table. The rebuild reloads the batch like
   every eager image instead of excluding it; excluding it would present the
   page without the batch, then refetch it (or skip it) from the prefix
   accounting. */
int test_deferred_image_publication_survives_rebuild(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    config.resources.maximum_images = 12;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images",
        error, sizeof(error)));
    CHECK(browser_engine_load_url(engine, "https://interactive-images.test/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.image_continuation_pending
        && nav->page.deferred_image_count == 3u && nav->page.runtime != NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *hero = test_reader_find_id(root, "hero");
    lxb_dom_node_t *tail = test_reader_find_id(root, "tail");
    lxb_dom_node_t *last = test_reader_find_id(root, "last");
    CHECK(hero != NULL && tail != NULL && last != NULL
        && browser_engine_render_frame(engine, NULL));
    /* Start the first batch, then queue a stylesheet removal so the
       relayout that publishes it runs as a full resource rebuild. */
    bool changed = false;
    CHECK(browser_engine_run_deferred_image_work(engine, &changed)
        && nav->page.deferred_image_job != NULL
        && nav->page.deferred_image_batch_count == 2u);
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "document.querySelector('style').remove()",
        "<image-publication-rebuild>", &nav->page.script_result));
    size_t rebuilds_before = nav->performance.mutation_resource_rebuilds;
    for (unsigned pump = 0; nav->page.image_continuation_pending && pump < 512; pump++) {
        changed = false;
        (void) browser_engine_run_deferred_image_work(engine, &changed);
        CHECK(nav->page.loaded && browser_engine_last_error(engine)[0] == '\0');
    }
    FetchTraceReplayStats stats = {0};
    CHECK(fetch_trace_replay_stats(&stats));
    fprintf(stderr, "image-publication-rebuild: loaded=%zu relayouts=%zu "
        "failures=%zu rebuilds=%zu served=%zu\n",
        nav->performance.background_images_loaded,
        nav->performance.background_image_relayouts,
        nav->performance.background_image_failures,
        nav->performance.mutation_resource_rebuilds - rebuilds_before,
        stats.served_request_count);
    CHECK(!nav->page.image_continuation_pending
        && nav->performance.mutation_resource_rebuilds == rebuilds_before + 1u
        && nav->performance.background_image_failures == 0
        && nav->performance.background_images_loaded == 3u
        && image_resource_available(images_find_node(&nav->page.images, hero))
        && image_resource_available(images_find_node(&nav->page.images, tail))
        && image_resource_available(images_find_node(&nav->page.images, last))
        && browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
        && metrics.budget_current == 0 && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* A page whose layout cost more than the publication limit keeps its
   deferred images unloaded: the queue finishes without a batch job and
   without a relayout, and the page stays usable. */
int test_deferred_images_skip_oversized_pages(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = true;
    config.resources.maximum_images = 12;
    config.resources.maximum_publication_work_units = 1u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images",
        error, sizeof(error)));
    CHECK(browser_engine_load_url(engine, "https://interactive-images.test/page", true));
    NavigationSession *nav = browser_engine_navigation(engine);
    CHECK(nav->page.loaded && nav->page.image_continuation_pending
        && nav->page.deferred_image_count == 3u
        && nav->page.layout.layout_work_units > 1u
        && browser_engine_render_frame(engine, NULL));
    size_t relayouts = nav->performance.background_image_relayouts;
    bool changed = false;
    CHECK(!browser_engine_run_deferred_image_work(engine, &changed) && !changed);
    CHECK(!nav->page.image_continuation_pending
        && nav->page.deferred_image_job == NULL
        && nav->performance.background_image_relayouts == relayouts
        && nav->performance.background_image_failures == 0
        && nav->page.images.stats.loaded == 1
        && browser_engine_last_error(engine)[0] == '\0'
        && browser_engine_render_frame(engine, NULL));
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
        && metrics.budget_current == 0 && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* Bound pumps, not host wall time: replay delay and owner lifetimes must be
   deterministic on slow CI too. While loading, the incumbent remains usable. */
static int journey_finish_navigation(BrowserEngine *engine)
{
    BrowserNavigationJobStatus status = browser_engine_navigation_status(engine);
    for (unsigned pump = 0; pump < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pump++) {
        status = browser_engine_pump_navigation(engine, NULL);
        NavigationSession *nav = browser_engine_navigation(engine);
        if (nav->page.loaded && status == BROWSER_NAVIGATION_JOB_PENDING)
            CHECK(browser_engine_render_frame(engine, NULL));
    }
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED)
        fprintf(stderr, "journey navigation status=%d error=%s\n", (int) status,
            browser_engine_last_error(engine));
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
        && browser_engine_render_frame(engine, NULL));
    return 0;
}

static int script_free_browsing_journey(size_t memory_limit)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = memory_limit;
    config.javascript.enabled = false;
    config.javascript.document_scripts_enabled = false;
    config.resources.web_fonts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin_response_keyed(TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-pointer-search", error, sizeof(error)));
    CHECK(browser_engine_begin_navigation_url(engine, "https://search-journey.test/",
        65536, 1000, true) && journey_finish_navigation(engine) == 0);
    NavigationSession *nav = browser_engine_navigation(engine);
    lxb_dom_node_t *root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *search = test_reader_find_id(root, "searchInput");
    CHECK(nav->page.runtime == NULL && search != NULL
        && browser_engine_focus_node(engine, search));
    int x, y, width, height;
    CHECK(controller_focused_rect(browser_engine_controller(engine),
        &x, &y, &width, &height));
    x = viewport_css_to_device(&nav->page.layout.viewport, x + width / 2);
    y = viewport_css_to_device(&nav->page.layout.viewport,
        y - navigation_current(nav)->scroll_y + height / 2);
    bool activate = false, changed = false;
    CHECK(browser_engine_pointer_event(engine, CONTROLLER_POINTER_DOWN,
              x, y, &activate, &changed)
        && browser_engine_pointer_event(engine, CONTROLLER_POINTER_UP,
              x, y, &activate, &changed)
        && activate && browser_engine_pointer_commit_click(engine));
    ControllerTextInputInfo input = {0};
    CHECK(browser_engine_text_input_info(engine, &input) && input.editable
        && browser_engine_insert_text(engine, "psp", 3));
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_FORM_SUBMIT
        && strstr(action.url, "search=psp") != NULL
        && browser_engine_begin_navigation_action(engine, &action, 65536, 1000)
        && journey_finish_navigation(engine) == 0);
    CHECK(nav->page.runtime == NULL && nav->page.layout.link_count >= 2);
    lxb_dom_node_t *article = nav->page.layout.links[0].node;
    CHECK(browser_engine_focus_node(engine, article)
        && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN)
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_UP)
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_activate(engine, &action)
        && strcmp(action.url, "https://search-journey.test/wiki/PlayStation_Portable") == 0
        && browser_engine_begin_navigation_action(engine, &action, 65536, 1000)
        && journey_finish_navigation(engine) == 0);
    root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *toggle = test_reader_find_id(root, "history-toggle");
    int collapsed_height = nav->page.layout.height;
    CHECK(toggle != NULL && browser_engine_focus_node(engine, toggle)
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_CONTROL
        && nav->page.layout.height > collapsed_height
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_scroll_by(engine, 240)
        && browser_engine_render_frame(engine, NULL)
        && browser_engine_scroll_by(engine, -240)
        && browser_engine_begin_navigation_history(engine, false, 65536, 1000)
        && journey_finish_navigation(engine) == 0);
    CHECK(strstr(navigation_current(nav)->url, "/find?") != NULL
        && browser_engine_controller(engine)->focus_node != NULL
        && browser_engine_activate(engine, &action)
        && strcmp(action.url, "https://search-journey.test/wiki/PlayStation_Portable") == 0);
    CHECK(browser_engine_begin_navigation_history(engine, false, 65536, 1000)
        && journey_finish_navigation(engine) == 0
        && strcmp(navigation_current(nav)->url, "https://search-journey.test/") == 0
        && nav->page.runtime == NULL);
    /* Repeated restoration, including form values and native disclosures,
       must not depend on a live script realm or one lucky history traversal. */
    for (unsigned repeat = 0; repeat < 3; repeat++) {
        CHECK(browser_engine_begin_navigation_history(engine, true, 65536, 1000)
            && journey_finish_navigation(engine) == 0
            && strstr(navigation_current(nav)->url, "/find?") != NULL
            && browser_engine_begin_navigation_history(engine, true, 65536, 1000)
            && journey_finish_navigation(engine) == 0
            && strstr(navigation_current(nav)->url, "/wiki/") != NULL
            && browser_engine_begin_navigation_history(engine, false, 65536, 1000)
            && journey_finish_navigation(engine) == 0
            && browser_engine_begin_navigation_history(engine, false, 65536, 1000)
            && journey_finish_navigation(engine) == 0);
        root = lxb_dom_interface_node(nav->page.document.html);
        CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "searchInput"))
            && browser_engine_text_input_info(engine, &input)
            && browser_engine_render_frame(engine, NULL));
    }
    /* Timeout and cancellation keep the incumbent interactive. Pump the
       real asynchronous receiver, changing visible focus while it waits. */
    for (unsigned cancel = 0; cancel < 2; cancel++) {
        CHECK(browser_engine_begin_navigation_url(engine,
            "https://search-journey.test/unavailable", 65536, 1000, true));
        for (unsigned pump = 0; pump < 128 && browser_engine_navigation_pending(engine); pump++) {
            CHECK(browser_engine_focus_move(engine, true)
                && browser_engine_render_frame(engine, NULL));
            if (cancel && pump == 2) browser_engine_cancel_navigation(engine, "journey cancellation");
            else (void) browser_engine_pump_navigation(engine, NULL);
        }
        CHECK(!browser_engine_navigation_pending(engine) && nav->page.loaded
            && strcmp(navigation_current(nav)->url, "https://search-journey.test/") == 0
            && browser_engine_render_frame(engine, NULL));
    }
    /* Supersede a waiting request through the normal receiver. Its later
       timeout must neither replace the new article nor strand its controls. */
    CHECK(browser_engine_begin_navigation_url(engine,
        "https://search-journey.test/unavailable", 65536, 1000, true));
    for (unsigned pump = 0; pump < 2; pump++)
        CHECK(browser_engine_pump_navigation(engine, NULL) == BROWSER_NAVIGATION_JOB_PENDING
            && browser_engine_focus_move(engine, true)
            && browser_engine_render_frame(engine, NULL));
    /* The frontend cancels its existing owner before beginning a replacement;
       the engine intentionally refuses two simultaneous navigation owners. */
    browser_engine_cancel_navigation(engine, "superseded by article selection");
    CHECK(browser_engine_begin_navigation_url(engine,
        "https://search-journey.test/wiki/PlayStation_Portable", 65536, 1000, true)
        && journey_finish_navigation(engine) == 0);
    uint64_t generation = nav->generation;
    for (unsigned pump = 0; pump < 32; pump++) {
        CHECK(browser_engine_pump_navigation(engine, NULL) == BROWSER_NAVIGATION_JOB_SUCCEEDED
            && nav->generation == generation && nav->page.loaded
            && strcmp(navigation_current(nav)->url,
                "https://search-journey.test/wiki/PlayStation_Portable") == 0
            && browser_engine_last_error(engine)[0] == '\0');
    }
    root = lxb_dom_interface_node(nav->page.document.html);
    CHECK(browser_engine_focus_node(engine, test_reader_find_id(root, "history-toggle"))
        && browser_engine_activate(engine, &action)
        && action.type == CONTROLLER_ACTION_CONTROL
        && browser_engine_render_frame(engine, NULL));
    fetch_trace_end();

    /* A long article with inherited focus styling and an expensive ancestor
       selector. Repeated input must reuse existing styles, not rewalk prose. */
    char html[16384];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><style>body{color:#123456}body:has(.absent){color:red}"
        "a{display:block;height:40px}a:focus{outline:2px solid currentColor}"
        "</style><body><main><section><a id=first href='/one'>First</a>"
        "<a id=second href='/two'>Second</a>");
    for (unsigned i = 0; i < 300; i++) {
        int n = snprintf(html + used, sizeof(html) - used, "<p>Article row %u</p>", i);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    CHECK(browser_engine_commit_html(engine, "https://journey.test/article", html, used, true));
    root = lxb_dom_interface_node(nav->page.document.html);
    lxb_dom_node_t *first = test_reader_find_id(root, "first");
    CHECK(nav->page.layout_reuse != NULL && first != NULL
        && browser_engine_focus_node(engine, first));
    LayoutReuseStats before = {0}, after = {0};
    layout_reuse_cache_stats(nav->page.layout_reuse, &before);
    CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN));
    layout_reuse_cache_stats(nav->page.layout_reuse, &after);
    CHECK(after.style_hits > before.style_hits);
    BrowserController *controller = browser_engine_controller(engine);
    CHECK(controller->has_authored_focus_outline
        && controller->authored_focus_outline.color == 0x123456
        && controller->authored_focus_outline.width == 2
        && browser_engine_render_frame(engine, NULL));
    size_t count = 0;
    const uint16_t *pixels = browser_engine_framebuffer(engine, &count);
    uint64_t cached = frame_checksum(pixels, count);
    /* Operation without the optional retained cache has the same output. */
    LayoutReuseCache *reuse = nav->page.layout_reuse;
    nav->page.layout_reuse = NULL;
    CHECK(browser_engine_focus_node(engine, controller->focus_node)
        && browser_engine_render_frame(engine, NULL));
    pixels = browser_engine_framebuffer(engine, &count);
    CHECK(frame_checksum(pixels, count) == cached);
    nav->page.layout_reuse = reuse;
    static const char css[] = "body{color:#654321!important}";
    CHECK(browser_engine_apply_user_css(engine, css, sizeof(css) - 1)
        && browser_engine_focus_node(engine, first)
        && controller->has_authored_focus_outline
        && controller->authored_focus_outline.color == 0x654321);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics final = {0};
    CHECK(browser_engine_metrics(engine, &final)
        && final.budget_current == 0 && final.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

int test_script_free_browsing_journey(void)
{
    CHECK(script_free_browsing_journey(16u * MIB) == 0);
    CHECK(script_free_browsing_journey(24u * MIB) == 0);
    return 0;
}

/* chatgpt.com's composer is a <label> whose click handler focuses the
   prompt textarea from script. Controller focus stayed on the pressed
   control, so the PSP frontend's keyboard never opened; it now adopts the
   script-focused control after an activation. */
int test_script_focus_adoption(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Composer</title>"
        "<button autofocus id=open "
        "onclick=\"document.getElementById('prompt').focus()\">Ask</button>"
        "<textarea id=prompt></textarea>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://composer.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *open = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html), "open");
    CHECK(open != NULL && browser_engine_focus_node(engine, open));
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action));
    ControllerTextInputInfo before = {0};
    CHECK(!browser_engine_text_input_info(engine, &before)
        || !before.editable);
    CHECK(browser_engine_adopt_script_focus(engine));
    ControllerTextInputInfo after = {0};
    CHECK(browser_engine_text_input_info(engine, &after)
        && after.editable && after.multiline);
    browser_engine_destroy(engine);
    return 0;
}

/* A transactional navigation keeps the incumbent realm alive until commit.
   chatgpt.com's home realm (~10 MiB) and its conversation page did not fit
   the PSP ceiling together, so the new page's modules were refused and the
   answer never rendered. A large incumbent realm is retired when a
   navigation begins; a small one keeps its rollback. */
int test_large_incumbent_realm_retired_for_navigation(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 32u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 10u * MIB;
    config.resources.enabled = false;
    static const char large[] =
        "<!doctype html><title>App</title><script>"
        "var keep=[];for(var i=0;i<2400;i++)keep.push(new Array(256).fill(i));"
        "</script>";
    static const char small[] =
        "<!doctype html><title>Doc</title><script>var x=1;</script>";
    for (int round = 0; round < 2; round++) {
        char error[256] = {0};
        BrowserEngine *engine =
            browser_engine_create(&config, error, sizeof(error));
        CHECK(engine != NULL);
        const char *page = round == 0 ? large : small;
        CHECK(browser_engine_commit_html(engine, "https://app.test/",
            page, strlen(page), true));
        NavigationSession *navigation = browser_engine_navigation(engine);
        CHECK(navigation->page.runtime != NULL);
        size_t used = script_runtime_heap_used(navigation->page.runtime);
        CHECK(round == 0 ? used >= 4u * MIB : used < 4u * MIB);
        CHECK(browser_engine_begin_navigation_url(
            engine, "https://next.invalid/", 1u * MIB, 1000, true));
        CHECK(round == 0 ? navigation->page.runtime == NULL
                         : navigation->page.runtime != NULL);
        browser_engine_destroy(engine);
    }
    return 0;
}

/* The boot-window check is a full collection. chatgpt.com's settled heap
   never fits back under the base, and the check repeated every 256
   advances forever: a 1.8 s freeze every few seconds on the PSP. Failed
   checks now back off. */
int test_boot_window_check_backs_off(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 4u * MIB;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Heap</title><script>"
        "var keep=[];for(var i=0;i<1200;i++)keep.push(new Array(256).fill(i));"
        "</script>";
    CHECK(setenv("TILEFINCH_JS_BOOT_WINDOW_KB", "2048", 1) == 0);
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://heap.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned tick = 0; tick < 2048u; tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 2u, NULL);
    CHECK(unsetenv("TILEFINCH_JS_BOOT_WINDOW_KB") == 0);
    size_t checks =
        script_runtime_boot_window_checks(navigation->page.runtime);
    if (checks == 0 || checks > 4u)
        printf("boot-window checks=%zu runtime=%p summary=%s error=%s\n",
               checks, (void *) navigation->page.runtime,
               navigation->page.script_result.summary,
               navigation->page.script_result.error);
    CHECK(checks != 0 && checks <= 4u);
    browser_engine_destroy(engine);
    return 0;
}

/* ChatGPT's composer focuses its textarea a task after the click, so the
   frontend watches for script focus while the press's user activation is
   live. Only a text field is adopted: script focusing a button later must
   not pull controller focus away from where the user left it. */
int test_late_script_text_focus_adoption(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Composer</title>"
        "<button id=open onclick=\"setTimeout(function(){"
        "document.getElementById('prompt').focus()},0)\">Ask</button>"
        "<button id=menu onclick=\"setTimeout(function(){"
        "document.getElementById('other').focus()},0)\">Menu</button>"
        "<button id=other>Other</button>"
        "<textarea id=prompt></textarea>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://composer.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *root =
        lxb_dom_interface_node(navigation->page.document.html);
    lxb_dom_node_t *menu = test_reader_find_id(root, "menu");
    CHECK(menu != NULL && browser_engine_focus_node(engine, menu));
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action));
    for (unsigned tick = 0; tick < 4; tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    CHECK(!browser_engine_adopt_script_text_focus(engine));
    ControllerTextInputInfo info = {0};
    CHECK(!browser_engine_text_input_info(engine, &info));
    lxb_dom_node_t *open = test_reader_find_id(root, "open");
    CHECK(open != NULL && browser_engine_focus_node(engine, open));
    CHECK(browser_engine_activate(engine, &action));
    CHECK(!browser_engine_adopt_script_text_focus(engine));
    for (unsigned tick = 0; tick < 4; tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    CHECK(browser_engine_adopt_script_text_focus(engine));
    CHECK(browser_engine_text_input_info(engine, &info)
        && info.editable && info.multiline);
    browser_engine_destroy(engine);
    return 0;
}

/* scrollIntoView honours a scroll-margin written as CSS math. chatgpt.com
   scrolls each new turn into view with scroll-margin-top:max(0px,calc(...
   env(safe-area-inset-top) ...)) = 26 px; read as 0, the user's bubble
   scrolled under the sticky header's Log in button. */
int test_scroll_into_view_resolves_margin_math(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Margin</title><style>body{margin:0}"
        "#t{height:40px;scroll-margin-top:max(0px,calc(calc(max(1rem, "
        "env(safe-area-inset-top)) + 3.625rem) + .25rem - 3.25rem))}"
        "#u{height:40px;scroll-margin-top:clamp(2px,1rem,5px)}"
        "#s{height:200px;overflow:auto}</style><div id=s>"
        "<div style='height:600px'></div><div id=t>t</div>"
        "<div id=u>u</div><div style='height:600px'></div></div>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://margin.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL);
    bool margin_ok = script_runtime_evaluate_diagnostic(
        navigation->page.runtime,
        "(()=>{const s=document.getElementById('s'),"
        "rel=e=>Math.round(e.getBoundingClientRect().top"
        "-s.getBoundingClientRect().top);"
        "const t=document.getElementById('t');t.scrollIntoView();"
        "const a=rel(t);"
        "const u=document.getElementById('u');u.scrollIntoView();"
        "const b=rel(u);"
        "if(a!==26||b!==5)throw Error('tops '+a+','+b)})()",
        "<scroll-margin-math>", &navigation->page.script_result);
    if (!margin_ok)
        printf("scroll-margin %s\n", navigation->page.script_result.error);
    CHECK(margin_ok);
    browser_engine_destroy(engine);
    return 0;
}

/* A textarea with rows= is rows lines tall whatever its appearance, and an
   inline form control answers geometry queries. chatgpt.com's composer
   (rows=1, appearance:none) collapsed to 0 px empty and 64 px once typed
   into, overlapping the disclaimer, and its autosizer read a 0 height. */
static int test_textarea_rows_geometry_on_clock(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Rows</title><style>body{margin:0}"
        "textarea{line-height:24px;padding:0;border:0;width:100px;margin:0}"
        "textarea,.r{font:16px sans-serif;line-height:24px}"
        ".b{display:block}.n{appearance:none}</style>"
        "<textarea id=block class=b rows=1></textarea>"
        "<textarea id=plain class='b n' rows=1>typed</textarea>"
        "<textarea id=three class=b rows=3></textarea>"
        "<textarea id=native class=b></textarea>"
        "<div><textarea id=inline rows=1></textarea></div>"
        "<div id=ref class=r>typed</div>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://rows.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "(()=>{const h=id=>document.getElementById(id)"
        ".getBoundingClientRect().height,"
        "got=['block','plain','three','native','inline'].map(h).join();"
        "if(got!=='24,24,72,64,24')throw Error('heights '+got)})()",
        "<textarea-rows>", &navigation->page.script_result));
    /* Control text sits in its line box like ordinary text: 16 px text in
       a 24 px line starts half the leading below the line's top. It was
       drawn at the top, above centre (chatgpt.com's composer text sat
       higher than its send arrow). */
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "globalThis.pocSummary=['plain','ref'].map(id=>Math.round("
        "document.getElementById(id).getBoundingClientRect().top)).join()",
        "<text-tops>", &navigation->page.script_result));
    int plain_top = 0, ref_top = 0;
    CHECK(sscanf(navigation->page.script_result.summary, "%d,%d",
                 &plain_top, &ref_top) == 2);
    int plain_text_y = INT32_MIN, ref_text_y = INT32_MIN;
    int plain_text_height = 0, ref_text_height = 0;
    const LayoutDocument *layout = &navigation->page.layout;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type != DRAW_TEXT || command->text_length != 5u
            || memcmp(command->text, "typed", 5u) != 0) continue;
        if (command->y < ref_top) {
            plain_text_y = command->y;
            plain_text_height = command->height;
        } else {
            ref_text_y = command->y;
            ref_text_height = command->height;
        }
    }
    if (plain_text_y - plain_top != ref_text_y - ref_top
        || plain_text_height != ref_text_height)
        printf("textarea text offset=%d/%d div text offset=%d/%d\n",
               plain_text_y - plain_top, plain_text_height,
               ref_text_y - ref_top, ref_text_height);
    CHECK(plain_text_y != INT32_MIN && ref_text_y != INT32_MIN
          && plain_text_y - plain_top == ref_text_y - ref_top
          && plain_text_height == ref_text_height);
    browser_engine_destroy(engine);
    return 0;
}

/* This test is about control geometry, not the script watchdog.
   A commit on a JavaScript-enabled engine creates the page realm under
   the 10 s script watchdog, which reads the platform monotonic clock. A
   host stall of 10 s inside the commit (seen at high load; reproduced by
   stopping the process for 11 s) interrupted it, and the commit failed with
   "InternalError: interrupted". On the CPU-paced clock
   (tilefinch_test_clocks.h) the watchdog counts only this process's CPU
   time, as on an unloaded host. */
int test_textarea_rows_geometry(void)
{
    TestCpuPacedClock clock;
    test_cpu_paced_clock_install(&clock);
    int failed = test_textarea_rows_geometry_on_clock();
    tilefinch_platform_set_services(NULL);
    return failed;
}

/* A definite-height grid shares its free space between fr rows only after
   content-sized rows take theirs, and never cuts an fr row below its
   minimum. chatgpt.com's page grid (minmax(4.5rem,1fr) auto 1fr) drew its
   disclaimer over the composer: the auto row's content was not measured
   before the fr shares were fixed, the rows after it did not move when it
   grew, and the fr share overwrote the first row's minimum. */
int test_grid_fr_rows_share_after_content(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Grid</title><style>body{margin:0}"
        ".g{display:grid;height:272px;"
        "grid-template-rows:minmax(72px,1fr) auto 1fr}"
        ".m{grid-row:2}.d{grid-row:3;align-self:end;height:30px}"
        "</style>"
        "<div class=g id=fixed><div class=m style='height:124px'></div>"
        "<div class=d></div></div>"
        "<div class=g id=content><div class=m><div style='height:152px'>"
        "</div></div><div class=d></div></div>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://grid.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "(()=>{const at=id=>{const g=document.getElementById(id),"
        "o=g.getBoundingClientRect().top;return [...g.children].map(c=>"
        "Math.round(c.getBoundingClientRect().top-o)).join('/')},"
        "got=at('fixed')+' '+at('content');"
        "if(got!=='74/242 72/242')throw Error('rows '+got)})()",
        "<grid-fr-share>", &navigation->page.script_result));
    browser_engine_destroy(engine);
    return 0;
}

/* A definite-height grid stretches its auto rows over the free space, so
   an item aligned to the centre is centred (chatgpt.com's menu icon sat at
   the top of its 44 px button). A focus indicator stays inside the boxes
   that clip its control (its composer textarea overflows a clipping form). */
int test_grid_auto_row_stretch_and_focus_clip(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Icon</title><style>body{margin:0}"
        ".b{display:grid;place-items:center;width:44px;height:44px}"
        ".s{width:24px;height:24px}"
        ".clip{overflow:clip;width:100px;margin-left:20px}"
        "input{width:300px;display:block}</style>"
        "<div class=b id=b><span class=s id=s></span></div>"
        "<div class=clip><input id=field></div>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://icon.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "(()=>{const b=document.getElementById('b').getBoundingClientRect(),"
        "s=document.getElementById('s').getBoundingClientRect(),"
        "got=(s.left-b.left)+','+(s.top-b.top);"
        "if(got!=='10,10')throw Error('offset '+got)})()",
        "<grid-auto-stretch>", &navigation->page.script_result));
    lxb_dom_node_t *field = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html), "field");
    CHECK(field != NULL && browser_engine_focus_node(engine, field));
    int x = 0, y = 0, width = 0, height = 0;
    CHECK(browser_engine_focus_indicator_rect(
              engine, &x, &y, &width, &height)
          && x == 20 && width == 100 && height > 0);
    browser_engine_destroy(engine);
    return 0;
}

/* chatgpt.com's send button is a ::before disc sized only by
   `inset: 4px` whose rounding is `border-radius: inherit`. The pseudo
   collapsed to zero height and inherit was ignored, so no button showed. */
int test_inset_pseudo_disc(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = false;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Send</title><style>body{margin:0}"
        "b{display:block;position:relative;width:44px;height:44px;"
        "border-radius:999px}"
        "b::before{content:'';position:absolute;inset:4px;"
        "background:#0d0d0d;border-radius:inherit}</style><b></b>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://send.test/",
        page, sizeof(page) - 1u, true));
    const LayoutDocument *layout =
        &browser_engine_navigation(engine)->page.layout;
    bool disc = false;
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_FILL_RECT && command->color == 0x0d0d0d
            && command->x == 4 && command->y == 4
            && command->width == 36 && command->height == 36
            && command->radius != 0) disc = true;
    }
    CHECK(disc);
    browser_engine_destroy(engine);
    return 0;
}

/* Night mode is the reader's colour-scheme preference: a site's own
   prefers-color-scheme:dark theme applies (chatgpt.com's white wordmark and
   menu icon stayed black under the forced-dark pixel transform), both in
   CSS and in matchMedia. */
int test_night_mode_prefers_dark(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Scheme</title>"
        "<meta name=color-scheme content='light dark'><style>"
        "body{background:#fff}"
        "@media (prefers-color-scheme:dark){body{background:#000}}"
        "</style><p>x</p>";
    static const char probe[] =
        "(()=>{const got=getComputedStyle(document.body).backgroundColor+"
        "'|'+matchMedia('(prefers-color-scheme: dark)').matches+"
        "'|'+matchMedia('(prefers-color-scheme: light)').matches;"
        "if(got!==globalThis.expected)throw Error('scheme '+got)})()";
    char error[256] = {0};
    for (int dark = 0; dark < 2; dark++) {
        BrowserEngine *engine = browser_engine_create(
            &config, error, sizeof(error));
        CHECK(engine != NULL);
        CHECK(browser_engine_set_forced_dark(engine, dark != 0));
        CHECK(browser_engine_commit_html(engine, "https://scheme.test/",
            page, sizeof(page) - 1u, true));
        NavigationSession *navigation = browser_engine_navigation(engine);
        CHECK(navigation != NULL && navigation->page.runtime != NULL);
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            dark ? "globalThis.expected='rgb(0, 0, 0)|true|false'"
                 : "globalThis.expected='rgb(255, 255, 255)|false|true'",
            "<scheme-expected>", &navigation->page.script_result));
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            probe, "<scheme>", &navigation->page.script_result));
        browser_engine_destroy(engine);
    }
    stylesheet_set_prefers_dark_color_scheme(false);
    return 0;
}

/* A custom property's var() references resolve where it is declared:
   chatgpt.com's :root colours were rebuilt under every element that
   re-declared a lightningcss light/dark toggle. And with deferral on, a
   form that script submits after async work (chatgpt.com posts the prompt
   once verification finishes) is handed to the frontend as an action. */
int test_declared_var_and_script_form_submission(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Form</title><style>"
        ":root{--m:blue;--a:var(--m)}.c{--m:red;color:var(--a)}</style>"
        "<p class=c id=c>x</p>"
        "<form id=f method=post action='/conversation'>"
        "<input name=prompt value=hi>"
        "<input type=hidden name=token value=''></form><script>"
        "setTimeout(function(){var f=document.getElementById('f');"
        "f.querySelector('[name=token]').remove();"
        "var t=document.createElement('input');t.type='hidden';"
        "t.name='token';t.value='tok123';f.append(t);"
        "var l=document.createElement('input');l.type='hidden';"
        "l.name='long';l.value='L'.repeat(6000)+'end';f.append(l);"
        "f.addEventListener('formdata',function(e){"
        "if(f.method.toLowerCase()!=='post'||"
        "f.enctype!=='application/x-www-form-urlencoded')return;"
        "e.formData.set('observer','obs456')},{once:true});"
        "f.requestSubmit()},0)"
        "</script>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    browser_engine_set_defer_script_navigation(engine, true);
    CHECK(browser_engine_commit_html(engine, "https://form.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "(()=>{const c=getComputedStyle(document.getElementById('c')).color;"
        "if(c!=='rgb(0, 0, 255)')throw Error('var '+c)})()",
        "<declared-var>", &navigation->page.script_result));
    for (unsigned tick = 0; tick < 4; tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    ControllerAction action = {0};
    CHECK(browser_engine_take_script_form_submission(engine, &action)
        && action.type == CONTROLLER_ACTION_FORM_SUBMIT
        && strcasecmp(action.method, "POST") == 0
        && strcmp(action.url, "https://form.test/conversation") == 0
        && action.body_length != 0);
    /* ChatGPT's sentinel tokens are several KiB each: the body page script
       built must be sent whole, not re-serialized into the inline buffer. */
    const char *body = controller_action_body(&action);
    size_t body_length = controller_action_body_length(&action);
    if (strstr(body, "token=tok123") == NULL
        || strstr(body, "observer=obs456") == NULL
        || strstr(body, "LLLLend") == NULL)
        printf("form body length=%zu %.*s\n", body_length, 200, body);
    CHECK(strstr(body, "token=tok123") != NULL
        && strstr(body, "observer=obs456") != NULL
        && strstr(body, "LLLLend") != NULL && body_length > 6000u
        && strlen(body) == body_length);
    CHECK(!browser_engine_take_script_form_submission(engine, &action));
    browser_engine_destroy(engine);
    return 0;
}

/* A module's static dependencies are requested in parallel once it
   arrives, and the synchronous graph load consumes those requests instead of
   issuing its own one at a time (chatgpt.com's serialized imports froze the
   PSP for 35-53 s). The fixture's dynamic import of late.js pulls a.js and
   b.js; both must come from prefetches. */
int test_module_dependency_prefetch(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin_response_keyed(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-module-prefetch",
        error, sizeof(error)));
    size_t started_before = 0, consumed_before = 0;
    script_loader_module_prefetch_totals(&started_before, &consumed_before);
    CHECK(browser_engine_begin_navigation_url(
              engine, "http://127.0.0.1:8781/index.html", 65536, 1000, true)
          && journey_finish_navigation(engine) == 0);
    NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned tick = 0; tick < 64
         && strcmp(navigation->page.script_result.summary, "late=3") != 0;
         tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    size_t started = 0, consumed = 0;
    script_loader_module_prefetch_totals(&started, &consumed);
    if (strcmp(navigation->page.script_result.summary, "late=3") != 0
        || started - started_before != 2u
        || consumed - consumed_before != 2u)
        fprintf(stderr, "module prefetch summary=%s started=%zu "
                "consumed=%zu\n", navigation->page.script_result.summary,
                started - started_before, consumed - consumed_before);
    CHECK(strcmp(navigation->page.script_result.summary, "late=3") == 0
          && started - started_before == 2u
          && consumed - consumed_before == 2u);
    fetch_trace_end();
    browser_engine_destroy(engine);
    return 0;
}

/* The PSP defers page-script navigations so they run as cooperative jobs.
   With deferral on, advancing the runtime must leave the page in place and
   hand the request to the frontend instead of loading it synchronously. */
static int test_deferred_script_navigation_on_clock(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    static const char page[] =
        "<!doctype html><title>Recovering</title><p>startup</p><script>"
        "setTimeout(function(){location.replace('/again')},0)</script>";
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    browser_engine_set_defer_script_navigation(engine, true);
    CHECK(browser_engine_commit_html(engine, "https://recover.test/start",
        page, sizeof(page) - 1u, true));
    char url[NAVIGATION_URL_LIMIT] = {0};
    bool record_history = true;
    CHECK(!browser_engine_take_script_navigation(
        engine, url, sizeof(url), &record_history));
    for (unsigned tick = 0; tick < 4; tick++)
        (void) browser_engine_advance_runtime(engine, 16u, 4u, NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    CHECK(entry != NULL
        && strcmp(entry->url, "https://recover.test/start") == 0
        && navigation->page.loaded);
    CHECK(browser_engine_take_script_navigation(
        engine, url, sizeof(url), &record_history)
        && strcmp(url, "https://recover.test/again") == 0
        && !record_history
        && strcmp(navigation->pending_navigation_referer,
                  "https://recover.test/start") == 0);
    CHECK(!browser_engine_take_script_navigation(
        engine, url, sizeof(url), &record_history));
    browser_engine_destroy(engine);
    return 0;
}

/* This test is about script-navigation deferral, not the script watchdog.
   A commit on a JavaScript-enabled engine creates the page realm under
   the 10 s script watchdog, which reads the platform monotonic clock. A
   host stall of 10 s inside the commit (seen at high load; reproduced by
   stopping the process for 11 s) interrupted it, and the commit failed with
   "InternalError: interrupted". On the CPU-paced clock
   (tilefinch_test_clocks.h) the watchdog counts only this process's CPU
   time, as on an unloaded host. */
int test_deferred_script_navigation(void)
{
    TestCpuPacedClock clock;
    test_cpu_paced_clock_install(&clock);
    int failed = test_deferred_script_navigation_on_clock();
    tilefinch_platform_set_services(NULL);
    return failed;
}

int test_text_content_does_not_retain_scratch(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] = "<!doctype html><div hidden id=large></div>"
        "<div id=small>a<!--excluded--><b>\xc3\xa9</b>c</div><div hidden id=wide></div>";
    CHECK(browser_engine_commit_html(engine, "https://text-memory.test/",
        page, sizeof(page) - 1u, true));
    NavigationSession *nav = browser_engine_navigation(engine);
    lxb_dom_node_t *large = test_reader_find_id(
        lxb_dom_interface_node(nav->page.document.html), "large");
    lxb_dom_node_t *wide = test_reader_find_id(
        lxb_dom_interface_node(nav->page.document.html), "wide");
    CHECK(large != NULL && wide != NULL);
    const size_t part = 160u * 1024u;
    char *source = malloc(part);
    CHECK(source != NULL);
    memset(source, 'x', part);
    BudgetAllocationOwner owner = document_allocation_owner_enter(&nav->page.document);
    for (unsigned i = 0; i < 2; i++) {
        lxb_dom_text_t *text = lxb_dom_document_create_text_node(
            &nav->page.document.html->dom_document,
            (const lxb_char_t *) source, part);
        CHECK(text != NULL && lxb_dom_node_append_child(large,
            lxb_dom_interface_node(text)) == LXB_DOM_EXCEPTION_OK);
    }
    document_allocation_owner_leave(&nav->page.document, owner);
    free(source);
    owner = document_allocation_owner_enter(&nav->page.document);
    for (unsigned i = 0; i < 33000; i++) {
        lxb_dom_text_t *text = lxb_dom_document_create_text_node(
            &nav->page.document.html->dom_document, (const lxb_char_t *) "z", 1);
        CHECK(text != NULL && lxb_dom_node_append_child(wide,
            lxb_dom_interface_node(text)) == LXB_DOM_EXCEPTION_OK);
    }
    document_allocation_owner_leave(&nav->page.document, owner);
    CHECK(script_runtime_evaluate_diagnostic(nav->page.runtime,
        "globalThis.largeTextNode=document.getElementById('large');"
        "globalThis.wideTextNode=document.getElementById('wide');",
        "<text-memory-warm>", &nav->page.script_result));
    size_t before = nav->budget->categories[BUDGET_CATEGORY_DOM].current;
    uint64_t started = tilefinch_platform_monotonic_time_us();
    bool text_read_okay = script_runtime_evaluate_diagnostic(nav->page.runtime,
        "(()=>{for(let i=0;i<8;i++){const t=largeTextNode.textContent;"
        "if(t.length!==327680||t[0]!=='x'||t[t.length-1]!=='x')throw Error('text');}"
        "if(wideTextNode.textContent.length!==33000)throw Error('truncated wide tree');"
        "const s=document.getElementById('small');"
        "if(s.textContent!=='a\u00e9c'||s.childNodes[1].textContent!=='excluded')"
        "throw Error('text semantics');})()",
        "<text-memory-read>", &nav->page.script_result);
    if (!text_read_okay) fprintf(stderr, "text-memory: %s\n", nav->page.script_result.error);
    CHECK(text_read_okay);
    size_t after = nav->budget->categories[BUDGET_CATEGORY_DOM].current;
    printf("text-content-memory: bytes=%zu reads=8 dom-before=%zu dom-after=%zu elapsed-us=%llu\n",
        part * 2u, before, after,
        (unsigned long long)(tilefinch_platform_monotonic_time_us() - started));
    CHECK(after == before);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics) && metrics.budget_current == 0);
    browser_engine_destroy(engine);
    return 0;
}

int test_loading_interaction_journey(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    config.tile_capacity = 8;
    char html[16384];
    size_t used = (size_t) snprintf(html, sizeof(html),
        "<!doctype html><title>Interaction journey</title><style>"
        "body{margin:4px}a{display:block;height:100px}"
        "a:focus{outline:3px solid red}summary{height:30px}</style>"
        "<body><details id=section><summary id=toggle>History</summary>"
        "<p>Expanded article prose remains usable while tasks wait.</p>"
        "</details>");
    for (unsigned i = 0; i < 12; i++) {
        int n = snprintf(html + used, sizeof(html) - used,
            "<a id=result%u href='/watch/%u'>Result %u with useful title</a>", i, i, i);
        CHECK(n > 0 && (size_t) n < sizeof(html) - used);
        used += (size_t) n;
    }
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    uint64_t focus_times[32] = {0};
    size_t sample_count = 0;
    for (unsigned cycle = 0; cycle < 4; cycle++) {
        if (cycle == 2) {
            browser_engine_destroy(engine);
            /* Exercise a roomier host cache too; PSP's shipped cap stays 8. */
            config.device.maximum_tile_capacity = 16;
            config.tile_capacity = 16;
            engine = browser_engine_create(&config, error, sizeof(error));
            CHECK(engine != NULL);
        }
        CHECK(browser_engine_commit_html(engine, "https://journey.test/page",
            html, used, true));
        NavigationSession *navigation = browser_engine_navigation(engine);
        CHECK(navigation != NULL && navigation->page.runtime != NULL);
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            "globalThis.journeyTimer=setInterval(()=>{},1000)",
            "<journey-pending-task>", &navigation->page.script_result));
        CHECK(navigation->page.script_result.pending_tasks != 0);
        BrowserEngineMetrics before = {0}, after = {0};
        CHECK(browser_engine_metrics(engine, &before));
        CHECK(browser_engine_render_frame(engine, NULL));
        CHECK(browser_engine_metrics(engine, &after));
        /* Foreground publication must only schedule optional prefetch. */
        CHECK(after.idle_slices == before.idle_slices);
        CHECK(after.idle_jobs_scheduled > before.idle_jobs_scheduled);
        size_t pixel_count = 0;
        const uint16_t *initial_pixels = browser_engine_framebuffer(engine, &pixel_count);
        uint64_t previous = frame_checksum(initial_pixels, pixel_count);
        lxb_dom_node_t *first_result = test_reader_find_id(
            lxb_dom_interface_node(navigation->page.document.html), "result0");
        CHECK(first_result != NULL);
        bool focused = browser_engine_focus_node(engine, first_result);
        if (!focused) fprintf(stderr, "journey focus failed: links=%zu controls=%zu error=%s\n",
            navigation->page.layout.link_count, navigation->page.layout.control_count,
            browser_engine_last_error(engine));
        CHECK(focused);
        for (unsigned press = 0; press < 8; press++) {
            uint64_t started = tilefinch_platform_monotonic_time_us();
            CHECK(browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN));
            CHECK(browser_engine_metrics(engine, &before));
            BrowserRenderJobStatus status = BROWSER_RENDER_JOB_PENDING;
            for (unsigned slice = 0; slice < 128
                 && status == BROWSER_RENDER_JOB_PENDING; slice++)
                status = browser_engine_render_frame_bounded(engine, 1000, 1);
            CHECK(status == BROWSER_RENDER_JOB_COMPLETE);
            focus_times[sample_count++] = tilefinch_platform_monotonic_time_us() - started;
            const uint16_t *pixels = browser_engine_framebuffer(engine, &pixel_count);
            uint64_t next = frame_checksum(pixels, pixel_count);
            CHECK(next != previous);
            previous = next;
            CHECK(browser_engine_metrics(engine, &after)
                && after.idle_slices == before.idle_slices);
            CHECK(navigation->page.script_result.pending_tasks != 0);
            ControllerAction action = {0};
            CHECK(browser_engine_activate(engine, &action)
                && action.type == CONTROLLER_ACTION_NAVIGATE
                && strstr(action.url, "/watch/") != NULL);
        }
        CHECK(navigation_current(navigation)->scroll_y > 272);
        for (unsigned direction = 0; direction < 2; direction++) {
            size_t baseline_foreground = 0, baseline_total = 0;
            uint64_t scroll_frames[64] = {0};
            for (unsigned prefetch = 0; prefetch < 2; prefetch++) {
                CHECK(browser_engine_scroll_to_edge(engine, direction != 0)
                    && browser_engine_refresh_render_shell(engine)
                    && browser_engine_render_frame(engine, NULL));
                const TileCache *cache = browser_engine_render_metrics_view(engine);
                size_t foreground_tiles = 0, total_before = cache->rasterized;
                for (unsigned held = 0; held < 64; held++) {
                    CHECK(browser_engine_scroll_by(engine, direction ? -8 : 8));
                    size_t before_tiles = cache->rasterized;
                    CHECK(browser_engine_render_frame(engine, NULL));
                    foreground_tiles += cache->rasterized - before_tiles;
                    const uint16_t *pixels = browser_engine_framebuffer(engine, &pixel_count);
                    uint64_t checksum = frame_checksum(pixels, pixel_count);
                    if (prefetch) CHECK(checksum == scroll_frames[held]);
                    else scroll_frames[held] = checksum;
                    if (prefetch) (void) tile_cache_run_idle_work(
                        browser_engine_render_shell(engine), 1000, 1);
                }
                size_t total_tiles = cache->rasterized - total_before;
                if (!prefetch) {
                    baseline_foreground = foreground_tiles;
                    baseline_total = total_tiles;
                } else {
                    CHECK(foreground_tiles <= baseline_foreground);
                    CHECK(total_tiles <= baseline_total);
                }
                if (cycle % 2 == 0) fprintf(stderr, "held-scroll: capacity=%zu direction=%u prefetch=%u foreground-tiles=%zu total-tiles=%zu\n",
                    config.tile_capacity, direction, prefetch, foreground_tiles, total_tiles);
            }
        }
        /* A shell replacement resets its local frame count but must not
           strand input-to-publication diagnostics in this navigation. */
        CHECK(browser_engine_scroll_to_edge(engine, false)
            && browser_engine_render_frame(engine, NULL));
        uint64_t shell_serial = browser_engine_render_shell_serial(engine);
        CHECK(browser_engine_refresh_render_shell(engine)
            && browser_engine_render_shell_serial(engine) != shell_serial
            && browser_engine_render_metrics_view(engine)->frames_rendered == 0
            && browser_engine_render_frame(engine, NULL));
        lxb_dom_node_t *toggle = test_reader_find_id(
            lxb_dom_interface_node(navigation->page.document.html), "toggle");
        lxb_dom_node_t *section = test_reader_find_id(
            lxb_dom_interface_node(navigation->page.document.html), "section");
        CHECK(toggle != NULL && section != NULL);
        int collapsed_height = navigation->page.layout.height;
        ControllerAction disclosure = {0};
        CHECK(browser_engine_focus_node(engine, toggle)
            && browser_engine_activate(engine, &disclosure)
            && disclosure.type == CONTROLLER_ACTION_CONTROL
            && navigation->page.layout.height > collapsed_height
            && browser_engine_render_frame(engine, NULL));
        size_t open_length = 0;
        CHECK(document_attribute(section, "open", &open_length) != NULL);
        /* Pending raster work is cancelable and restarting it remains safe. */
        CHECK(browser_engine_scroll_by(engine, 200));
        (void) browser_engine_render_frame_bounded(engine, 1, 1);
        browser_engine_cancel_render_job(engine);
        CHECK(!browser_engine_render_frame_pending(engine)
            && browser_engine_render_frame(engine, NULL));
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        CHECK(browser_engine_metrics(engine, &after)
            && after.idle_slices > before.idle_slices);
        /* Cancellation and admission refusal must leave the incumbent
           interactive, including its pending realm tasks and render owner. */
        CHECK(fetch_trace_replay_begin(TILEFINCH_TEST_SOURCE_DIR
            "/tests/fixtures/http-browser-engine-async", error, sizeof(error)));
        CHECK(browser_engine_begin_navigation_url(engine,
            "https://async-engine.test/page", 65536, 1000, true));
        browser_engine_cancel_navigation(engine, "journey cancellation");
        fetch_trace_end();
        CHECK(navigation->page.loaded && browser_engine_render_frame(engine, NULL));
        Budget *budget = navigation->budget;
        size_t limit = budget->limit;
        budget->limit = budget->current;
        bool admitted = browser_engine_begin_navigation_url(engine,
            "https://journey.test/refused", 65536, 1000, true);
        budget->limit = limit;
        CHECK(!admitted && navigation->page.loaded
            && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_UP)
            && browser_engine_render_frame(engine, NULL));
        /* Model the page lifecycle around a native-player handoff without
           creating a second media pipeline or producing audio in host tests. */
        CHECK(browser_engine_set_page_visibility(engine, false));
        browser_engine_suspend_page_presentations(engine);
        CHECK(browser_engine_set_page_visibility(engine, true)
            && browser_engine_focus_direction(engine, CONTROLLER_FOCUS_DOWN)
            && browser_engine_render_frame(engine, NULL));
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            "globalThis.journeyChild=document.createElement('iframe');"
            "journeyChild.srcdoc='<p>Bounded child document</p>';"
            "document.body.appendChild(journeyChild)",
            "<journey-frame>", &navigation->page.script_result));
        for (unsigned tick = 0; tick < 8; tick++) {
            bool damage = false;
            CHECK(browser_engine_advance_runtime(engine, 16, 4, &damage));
        }
        /* srcdoc uses the local frame realm, not a remote NavigationFrame
           slot. Exercise that realm before detaching and replacing its page. */
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            "journeyChild.contentDocument.title='Journey child';"
            "if(journeyChild.contentWindow.document.title!=='Journey child')"
            "throw new Error('child realm not usable')",
            "<journey-frame-check>", &navigation->page.script_result));
        CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
            "journeyChild.parentNode.removeChild(journeyChild);journeyChild=null",
            "<journey-frame-retire>", &navigation->page.script_result));
        for (unsigned tick = 0; tick < 8; tick++) {
            bool damage = false;
            CHECK(browser_engine_advance_runtime(engine, 16, 4, &damage));
        }
        CHECK(browser_engine_render_frame(engine, NULL));
    }
    /* Fixed sample storage and insertion sort keep the reporting bounded. */
    for (size_t i = 1; i < sample_count; i++) {
        uint64_t value = focus_times[i];
        size_t at = i;
        while (at != 0 && focus_times[at - 1] > value) {
            focus_times[at] = focus_times[at - 1];
            at--;
        }
        focus_times[at] = value;
    }
    printf("interaction-journey: focus-to-raster-us n=%zu p50=%llu p95=%llu max=%llu\n",
        sample_count, (unsigned long long) focus_times[sample_count / 2],
        (unsigned long long) focus_times[(sample_count * 95u - 1u) / 100u],
        (unsigned long long) focus_times[sample_count - 1]);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics final = {0};
    CHECK(browser_engine_metrics(engine, &final)
        && final.budget_current == 0 && final.budget_active_allocations == 0
        && final.budget_external_reserved == 0);
    browser_engine_destroy(engine);
    return 0;
}
