static int test_committed_document_glyph_script_hints(void)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char html[] =
        "<!doctype html><title>Scripts</title><body>"
        "&#x0416; &#x1ED9; &#x6F22; &#x3042; &#xD55C;";
    const uint8_t expected = DOCUMENT_GLYPH_SCRIPT_CYRILLIC
        | DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED
        | DOCUMENT_GLYPH_SCRIPT_HAN
        | DOCUMENT_GLYPH_SCRIPT_JAPANESE
        | DOCUMENT_GLYPH_SCRIPT_KOREAN;
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://scripts.test/", html,
                 sizeof(html) - 1u, true)
          && browser_engine_glyph_script_mask(engine) == expected);
    browser_engine_destroy(engine);
    return 0;
}

static int test_responsive_navigation_convergence(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 512u * 1024u;
    config.maximum_document_bytes = 1u * MIB;
    config.tile_capacity = 2;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char incumbent[] =
        "<!doctype html><title>Incumbent</title><body>stable";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://incumbent.test/", incumbent,
                 sizeof(incumbent) - 1, true)
          && fetch_trace_replay_begin(
                 TILEFINCH_TEST_SOURCE_DIR
                     "/tests/fixtures/http-browser-engine-async",
                 error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://async-engine.test/page",
                 64u * 1024u, 1000, true));
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 17,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 1000
            },
            .maximum_parser_body_bytes = 17,
            .maximum_parser_time_us = 1000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    BrowserNavigationJobStatus status =
        BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    BrowserNavigationJobMetrics metrics = {0};
    (void) browser_engine_navigation_job_metrics(engine, &metrics);
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED) {
        fprintf(stderr, "responsive navigation failed: status=%d error=%s "
                "pumps=%zu body=%zu\n", (int) status,
                browser_engine_last_error(engine), metrics.pump_calls,
                metrics.load.body_bytes);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && metrics.pump_calls > 3
          && metrics.load.body_bytes == 210
          && strcmp(
                 browser_engine_navigation(engine)->page.document.title,
                 "Async candidate") == 0
          && browser_engine_render_frame(engine, NULL));
    size_t pixels = 0;
    const uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    uint64_t responsive_hash = frame_checksum(frame, pixels);
    size_t responsive_nodes =
        browser_engine_navigation(engine)->page.document.node_count;
    size_t responsive_commands =
        browser_engine_navigation(engine)->page.layout.count;
    fetch_trace_end();

    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR
                  "/tests/fixtures/http-browser-engine-async",
              error, sizeof(error))
          && browser_engine_load_url_with_limits(
                 engine, "https://async-engine.test/page",
                 64u * 1024u, 1000, false)
          && browser_engine_render_frame(engine, NULL));
    frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(browser_engine_navigation(engine)->page.document.node_count
              == responsive_nodes
          && browser_engine_navigation(engine)->page.layout.count
              == responsive_commands
          && frame_checksum(frame, pixels) == responsive_hash);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_scrollable_provisional_navigation(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 1u * MIB;
    config.tile_capacity = 4;
    config.resources.enabled = true;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char incumbent[] =
        "<!doctype html><title>Stable incumbent</title>"
        "<body style='margin:0;background:#fff'>rollback-safe";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://stable.test/", incumbent,
                 sizeof(incumbent) - 1, true)
          && browser_engine_render_frame(engine, NULL));
    size_t pixels = 0;
    const uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    uint64_t incumbent_hash = frame_checksum(frame, pixels);
    ProvisionalPresentProbe present_probe = {0};
    TilefinchPlatformServices present_services = {
        .context = &present_probe,
        .present_rgb565 = capture_provisional_present
    };
    tilefinch_platform_set_services(&present_services);
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(
        navigation, 64, 0, 0, 0, 0, 0);

    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 64,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 10000
            },
            .maximum_parser_body_bytes = 64,
            .maximum_parser_time_us = 10000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-preview",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://stream-preview.test/document",
                 4096, 1000, true));
    BrowserProvisionalViewport provisional = {0};
    BrowserNavigationJobMetrics job = {0};
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING
             && !browser_engine_provisional_viewport(
                    engine, &provisional);
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    if (status != BROWSER_NAVIGATION_JOB_PENDING
        || !provisional.ready || provisional.frame_count != 2) {
        (void) browser_engine_navigation_job_metrics(engine, &job);
        fprintf(stderr,
                "provisional pending=%d ready=%d frames=%zu scroll=%d/%d "
                "paints=%zu bytes=%zu stream=%zu/%zu/%zu partial=%zu/%zu "
                "budget=%zu/%zu status=%d error=\"%s\"\n",
                status == BROWSER_NAVIGATION_JOB_PENDING,
                provisional.ready, provisional.frame_count,
                provisional.scroll_y, provisional.maximum_scroll_y,
                job.provisional_paints, job.provisional_bytes,
                navigation->performance.streaming_preview_checks,
                navigation->performance.streaming_preview_attempts,
                navigation->performance.streaming_preview_paints,
                navigation->performance.partial_layouts,
                navigation->performance.partial_paints,
                browser_engine_budget(engine)->current,
                browser_engine_budget(engine)->limit,
                (int) status, browser_engine_last_error(engine));
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING
          && provisional.ready
          && provisional.pixels != NULL
          && provisional.pixel_count == 480u * 272u
          && provisional.frame_count == 2
          && provisional.current_frame == 0
          && provisional.scroll_y == 0
          && provisional.maximum_scroll_y > 0);
    CHECK(navigation->performance.progressive_preview_y_limit
              == navigation->viewport.css_height
                   + navigation->viewport.css_height / 2);
    uint64_t provisional_top_hash =
        frame_checksum(provisional.pixels, provisional.pixel_count);
    CHECK(present_probe.calls == 1
          && present_probe.last_checksum == provisional_top_hash);
    CHECK(browser_engine_scroll_provisional_page(engine, 1)
          && browser_engine_provisional_viewport(engine, &provisional)
          && provisional.current_frame == 1
          && provisional.scroll_y == provisional.maximum_scroll_y
          && frame_checksum(
                 provisional.pixels, provisional.pixel_count)
                 != provisional_top_hash);
    CHECK(present_probe.calls == 2
          && present_probe.last_checksum
                 == frame_checksum(
                        provisional.pixels, provisional.pixel_count));
    job = (BrowserNavigationJobMetrics) {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &job)
          && job.provisional_paints == 1
          && job.provisional_scrolls == 1
          && job.provisional_frame_count == 2
          && job.provisional_bytes >= 2u * 480u * 272u
                                        * sizeof(uint16_t));
    browser_engine_cancel_navigation(engine, "provisional rollback test");
    CHECK(browser_engine_navigation_status(engine)
              == BROWSER_NAVIGATION_JOB_CANCELLED
          && !browser_engine_provisional_viewport(engine, &provisional)
          && strcmp(navigation->page.document.title,
                    "Stable incumbent") == 0
          && present_probe.calls == 3
          && present_probe.last_checksum == incumbent_hash
          && browser_engine_render_frame(engine, NULL));
    frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(frame_checksum(frame, pixels) == incumbent_hash);
    fetch_trace_end();

    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-preview",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://stream-preview.test/document",
                 4096, 1000, true));
    memset(&provisional, 0, sizeof(provisional));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
        if (browser_engine_provisional_viewport(
                engine, &provisional)
            && provisional.current_frame == 0) {
            (void) browser_engine_scroll_provisional_page(engine, 1);
        }
    }
    const NavigationEntry *entry = navigation_current(navigation);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && entry != NULL
          && entry->scroll_y > 0
          && strcmp(navigation->page.document.title,
                    "Streaming preview") == 0
          && !browser_engine_provisional_viewport(engine, &provisional)
          && browser_engine_render_frame(engine, NULL)
          && browser_engine_navigation_job_metrics(engine, &job)
          && job.provisional_scroll_y == entry->scroll_y);
    int committed_scroll = entry->scroll_y;
    frame = browser_engine_framebuffer(engine, &pixels);
    uint64_t enabled_hash = frame_checksum(frame, pixels);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    config.progressive_first_paint = false;
    engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(
        navigation, 64, 0, 0, 0, 0, 0);
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stream-preview",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://stream-preview.test/document",
                 4096, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && navigation_set_scroll(navigation, committed_scroll)
          && browser_engine_render_frame(engine, NULL));
    frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(frame_checksum(frame, pixels) == enabled_hash);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    tilefinch_platform_set_services(NULL);
    return 0;
}
