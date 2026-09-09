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
    for (unsigned mutation = 0; mutation < 7; mutation++) {
        BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
        CHECK(engine != NULL && fetch_trace_replay_begin(mutation >= 4
            ? TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-image-order"
            : TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-interactive-images", error, sizeof(error)));
        CHECK(browser_engine_load_url(engine, "https://interactive-images.test/page", true));
        NavigationSession *nav = browser_engine_navigation(engine);
        CHECK(nav->page.loaded && nav->page.image_continuation_pending
            && nav->page.deferred_image_count == (mutation >= 4 ? 5u : 3u)
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
