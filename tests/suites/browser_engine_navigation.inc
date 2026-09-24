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
        "&#x0416; &#x1ED9; &#x6F22; &#x3042; &#xD55C; &#x0633; &#x05E9;";
    const uint16_t expected = DOCUMENT_GLYPH_SCRIPT_CYRILLIC
        | DOCUMENT_GLYPH_SCRIPT_LATIN_EXTENDED
        | DOCUMENT_GLYPH_SCRIPT_HAN
        | DOCUMENT_GLYPH_SCRIPT_JAPANESE
        | DOCUMENT_GLYPH_SCRIPT_KOREAN
        | DOCUMENT_GLYPH_SCRIPT_ARABIC
        | DOCUMENT_GLYPH_SCRIPT_HEBREW;
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

static int test_animated_canvas_follows_transactional_page_move(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 64u * 1024u;
    config.tile_capacity = 2;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 3u * MIB;
    config.javascript.maximum_scripts = 4;
    config.javascript.maximum_total_bytes = 32u * 1024u;
    config.javascript.maximum_file_bytes = 16u * 1024u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    bool replay = engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/tests/fixtures/http-browser-engine-animated-canvas",
        error, sizeof(error));
    bool loaded = replay && browser_engine_load_url_with_limits(
        engine, "https://animated-canvas.test/page",
        64u * 1024u, 1000, true);
    if (engine == NULL || !replay || !loaded) {
        fprintf(stderr,
                "animated canvas setup engine=%d replay=%d loaded=%d "
                "trace-error=\"%s\" engine-error=\"%s\"\n",
                engine != NULL, replay, loaded, error,
                engine == NULL ? "" : browser_engine_last_error(engine));
    }
    CHECK(engine != NULL && replay && loaded);
    fetch_trace_end();

    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.layout_reuse != NULL
          && layout_reuse_cache_node_retirement(navigation->page.layout_reuse));
    LayoutReuseStats retirement_before = {0}, retirement_after = {0};
    layout_reuse_cache_stats(navigation->page.layout_reuse, &retirement_before);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "var retiredHost=document.createElement('div');"
        "retiredHost.innerHTML='<span>temporary</span>';"
        "document.body.appendChild(retiredHost);retiredHost.innerHTML='';",
        "<promoted-retirement>", &navigation->page.script_result));
    layout_reuse_cache_stats(navigation->page.layout_reuse, &retirement_after);
    CHECK(retirement_after.retired_subtrees > retirement_before.retired_subtrees);
    bool visible_changed = false;
    CHECK(navigation != NULL
          && navigation->page.images.count == 0
          && browser_engine_advance_runtime(
                 engine, 16, 4, &visible_changed));
    const ImageResource *image = navigation->page.images.count == 1
        ? &navigation->page.images.items[0] : NULL;
    CHECK(image != NULL && image->node != NULL && image->is_canvas
          && image->pixels != NULL
          && image->width == 4 && image->height == 2
          && visible_changed && browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* A preview that is rejected (only a rule has arrived) is retried as the
   body grows even without scripts, whose checkpoints used to be the only
   retry; a header-only preview is repainted once real content arrives, and
   the first-present time stays that of the first paint. */
static int test_provisional_preview_retries_and_refreshes(void)
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
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    ProvisionalPresentProbe present_probe = {0};
    TilefinchPlatformServices present_services = {
        .context = &present_probe,
        .present_rgb565 = capture_provisional_present
    };
    tilefinch_platform_set_services(&present_services);
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(navigation, 512, 0, 0, 0, 0, 0);
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 512,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 10000
            },
            .maximum_parser_body_bytes = 512,
            .maximum_parser_time_us = 10000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-refresh",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-refresh.test/document",
                 256u * 1024u, 1000, true));
    BrowserNavigationJobMetrics job = {0};
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    uint64_t first_present_at_first_paint = 0;
    size_t paints_before_commit = 0;
    for (size_t pumps = 0;
         pumps < 1024 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        (void) browser_engine_navigation_job_metrics(engine, &job);
        if (job.provisional_paints == 1
            && first_present_at_first_paint == 0)
            first_present_at_first_paint = job.provisional_first_present_us;
        paints_before_commit = job.provisional_paints;
        status = browser_engine_pump_navigation(engine, &quota);
    }
    (void) browser_engine_navigation_job_metrics(engine, &job);
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED
        || paints_before_commit != 2
        || navigation->performance.streaming_preview_empty_raster_skips == 0
        || first_present_at_first_paint == 0
        || job.provisional_first_present_us
               != first_present_at_first_paint) {
        fprintf(stderr,
                "preview-refresh status=%d paints=%zu "
                "empty-skips=%zu attempts=%zu first=%llu/%llu "
                "presents=%zu error=\"%s\"\n",
                (int) status, paints_before_commit,
                navigation->performance.streaming_preview_empty_raster_skips,
                navigation->performance.streaming_preview_attempts,
                (unsigned long long) first_present_at_first_paint,
                (unsigned long long) job.provisional_first_present_us,
                present_probe.calls, browser_engine_last_error(engine));
    }
    /* Rule rejected by the raster gate, header painted, then one repaint
       that filled the first screen and so ended refreshes. */
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && navigation->performance.streaming_preview_empty_raster_skips
                 >= 1
          && paints_before_commit == 2
          && first_present_at_first_paint != 0
          && job.provisional_first_present_us
                 == first_present_at_first_paint
          && strcmp(navigation->page.document.title,
                    "Preview refresh") == 0);
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* A large script-free page commits with only its top screens laid out
   (no whole-document layout or image walk before commit); page idle work
   then completes the layout, identical to a synchronous build. */
static bool provisional_commit_has_text(const LayoutDocument *layout,
                                        const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == length
            && memcmp(command->text, text, length) == 0) return true;
    }
    return false;
}

typedef struct {
    char extension_phase[48];
    char completion_phase[48];
} ProvisionalPhaseRecorder;

/* Remembers which phase each provisional-layout step ran under. */
static bool record_provisional_phase(void *context, const char *phase,
                                     size_t completed_work_units)
{
    ProvisionalPhaseRecorder *recorder = context;
    if (phase == NULL || completed_work_units == 0
        || strncmp(phase, "optional-layout", 15) != 0) return true;
    snprintf(recorder->completion_phase,
             sizeof(recorder->completion_phase), "%s", phase);
    return true;
}

static int test_large_static_page_commits_provisionally(void)
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
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-provisional-commit",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://provisional-commit.test/document",
                 256u * 1024u, 1000, true));
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, NULL);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && navigation->page.document.node_count >= 3000
          && navigation->page.runtime == NULL);
    int limit = navigation->page.layout.content_limit_y;
    CHECK(navigation->page.layout_provisional
          && limit > 0 && navigation->page.layout.height > limit
          && provisional_commit_has_text(&navigation->page.layout, "para")
          && !provisional_commit_has_text(&navigation->page.layout, "899")
          && navigation->performance.provisional_relayouts == 1);
    /* A reader at the bottom of the provisional extent waits for the next
       screens: that step runs under its own phase, which a frontend does
       not cancel for further scrolling down. */
    int bottom = navigation->page.layout.height
        - navigation->viewport.css_height;
    CHECK(!navigation_layout_completion_awaited(navigation)
          && navigation_set_scroll(navigation, bottom)
          && navigation_layout_completion_awaited(navigation));
    ProvisionalPhaseRecorder recorder = {0};
    TilefinchPlatformServices recording = {
        .context = &recorder, .cooperate = record_provisional_phase };
    tilefinch_platform_set_services(&recording);
    for (size_t turns = 0;
         turns < 64 && navigation->performance.provisional_extensions == 0;
         turns++) {
        bool visual_changed = false;
        (void) browser_engine_run_idle_work(engine, &visual_changed);
    }
    snprintf(recorder.extension_phase, sizeof(recorder.extension_phase),
             "%s", recorder.completion_phase);
    CHECK(navigation->performance.provisional_extensions == 1
          && navigation->page.layout.content_limit_y > limit
          && strcmp(recorder.extension_phase,
                    "optional-layout-extension-awaited") == 0
          && !navigation_layout_completion_awaited(navigation));
    CHECK(navigation_set_scroll(navigation, 0));
    for (size_t turns = 0;
         turns < 64 && navigation_layout_completion_pending(navigation);
         turns++) {
        bool visual_changed = false;
        (void) browser_engine_run_idle_work(engine, &visual_changed);
    }
    tilefinch_platform_set_services(NULL);
    CHECK(strcmp(recorder.completion_phase,
                 "optional-layout-completion") == 0);
    CHECK(!navigation->page.layout_provisional
          && navigation->page.layout.content_limit_y == 0
          && provisional_commit_has_text(&navigation->page.layout, "899")
          && navigation->performance.provisional_completions == 1);
    size_t completed_count = navigation->page.layout.count;
    int completed_height = navigation->page.layout.height;
    /* The same page relaid out synchronously. */
    navigation->relayout_preview_threshold_us = UINT64_MAX;
    CHECK(navigation_relayout(navigation)
          && navigation->page.layout.count == completed_count
          && navigation->page.layout.height == completed_height);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

static size_t preview_checkerboard_pixels(
    const BrowserProvisionalViewport *viewport)
{
    size_t count = 0;
    for (size_t i = 0; i < viewport->pixel_count; i++) {
        if (viewport->pixels[i] == 0xef7du || viewport->pixels[i] == 0xd69au)
            count++;
    }
    return count;
}

/* While a long page streams, the reader scrolls its preview past the laid-
   out content: every press moves at once (checkerboard where tiles or
   content are missing), bounded raster steps fill the screen, the stream
   lays out a deeper preview through the reader, and the commit keeps both
   the position and content laid out through it. */
static int test_preview_scrolls_ahead_during_load(void)
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
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    ProvisionalPresentProbe present_probe = {0};
    TilefinchPlatformServices present_services = {
        .context = &present_probe,
        .present_rgb565 = capture_provisional_present
    };
    tilefinch_platform_set_services(&present_services);
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(navigation, 512, 0, 0, 0, 0, 0);
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 512,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 10000
            },
            .maximum_parser_body_bytes = 512,
            .maximum_parser_time_us = 10000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-scroll-ahead",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-scroll-ahead.test/document",
                 256u * 1024u, 1000, true));
    BrowserProvisionalViewport viewport = {0};
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && !browser_engine_provisional_viewport(engine, &viewport);
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING && viewport.ready
          && viewport.complete && viewport.scroll_y == 0
          && viewport.content_end_y > 0 && viewport.viewport_height > 0
          && viewport.maximum_scroll_y > viewport.content_end_y
                 - viewport.viewport_height);
    int first_end = viewport.content_end_y;
    /* Page past the laid-out content: each press moves at once, with a
       checkerboard where tiles are not rasterized or content is not laid
       out yet. */
    size_t presses = 0;
    bool saw_placeholder = false;
    while (viewport.scroll_y + viewport.viewport_height <= first_end
           && presses < 16) {
        int before = viewport.scroll_y;
        size_t presents = present_probe.calls;
        CHECK(browser_engine_scroll_provisional_page(engine, 1)
              && browser_engine_provisional_viewport(engine, &viewport)
              && viewport.scroll_y > before
              && present_probe.calls == presents + 1);
        if (!viewport.complete) saw_placeholder = true;
        presses++;
    }
    CHECK(presses > 0 && saw_placeholder
          && navigation->provisional_reader_y == viewport.scroll_y
          && preview_checkerboard_pixels(&viewport) > 0);
    /* Raster steps fill the visible tiles; below the laid-out content the
       checkerboard stays. */
    size_t steps = 0;
    while (browser_engine_provisional_raster_step(engine) && steps < 64)
        steps++;
    CHECK(browser_engine_provisional_viewport(engine, &viewport)
          && viewport.complete
          && preview_checkerboard_pixels(&viewport) > 0);
    /* Parsing continues; the next preview is laid out through the reader
       and fills the rest of the screen. */
    int target = viewport.scroll_y;
    BrowserNavigationJobMetrics job = {0};
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && browser_engine_provisional_viewport(engine, &viewport)
             && viewport.content_end_y <= first_end;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    while (browser_engine_provisional_raster_step(engine)) {}
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING
          && browser_engine_provisional_viewport(engine, &viewport)
          && viewport.scroll_y == target && viewport.complete
          && viewport.content_end_y > first_end
          && viewport.content_end_y >= target + viewport.viewport_height
          && preview_checkerboard_pixels(&viewport) == 0
          && present_probe.last_checksum
                 == frame_checksum(viewport.pixels, viewport.pixel_count)
          && browser_engine_navigation_job_metrics(engine, &job)
          && job.provisional_paints >= 2
          && job.provisional_reach_px == viewport.content_end_y);
    /* Analog motion follows the stick at once: past the laid-out content
       it shows the checkerboard, and raster steps fill what is laid out. */
    int end = viewport.content_end_y;
    size_t presents = present_probe.calls;
    CHECK(browser_engine_scroll_provisional_by(
              engine, end - viewport.viewport_height / 2 - target)
          && browser_engine_provisional_viewport(engine, &viewport)
          && viewport.scroll_y == end - viewport.viewport_height / 2
          && present_probe.calls == presents + 1
          && preview_checkerboard_pixels(&viewport) > 0);
    while (browser_engine_provisional_raster_step(engine)) {}
    CHECK(browser_engine_provisional_viewport(engine, &viewport)
          && viewport.complete
          && preview_checkerboard_pixels(&viewport) > 0);
    int reader_y = viewport.scroll_y;
    for (size_t pumps = 0;
         pumps < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    const NavigationEntry *entry = navigation_current(navigation);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED && entry != NULL
          && entry->scroll_y == reader_y
          && navigation->page.layout_provisional
          && navigation->page.layout.content_limit_y
                 >= reader_y + navigation->viewport.css_height
          && navigation->provisional_reader_y == 0
          && !browser_engine_provisional_viewport(engine, &viewport));
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* The loading page is usable: D-pad focus moves over its visible links
   and controls with a focus ring, X on a web link hands back a navigation
   (the caller cancels this load), and a control waits for the commit,
   which keeps the focus and then asks for the activation. */
static int test_preview_focus_and_activation(void)
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
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    ProvisionalPresentProbe present_probe = {0};
    TilefinchPlatformServices present_services = {
        .context = &present_probe,
        .present_rgb565 = capture_provisional_present
    };
    tilefinch_platform_set_services(&present_services);
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(navigation, 512, 0, 0, 0, 0, 0);
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 512,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 10000
            },
            .maximum_parser_body_bytes = 512,
            .maximum_parser_time_us = 10000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-links",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-links.test/document",
                 256u * 1024u, 1000, true));
    BrowserProvisionalViewport viewport = {0};
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && !browser_engine_provisional_viewport(engine, &viewport);
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING && viewport.ready);
    uint64_t unfocused = frame_checksum(viewport.pixels, viewport.pixel_count);
    /* The first press focuses the first visible item: the search field. */
    ControllerAction action;
    CHECK(browser_engine_provisional_activate(engine, false, 0, 0, &action)
              == BROWSER_PREVIEW_ACTIVATION_FOCUSED
          && browser_engine_provisional_viewport(engine, &viewport)
          && frame_checksum(viewport.pixels, viewport.pixel_count)
                 != unfocused);
    /* Down to the same-page link, then a web link. */
    CHECK(browser_engine_provisional_focus_direction(
              engine, CONTROLLER_FOCUS_DOWN)
          && browser_engine_provisional_activate(
                 engine, false, 0, 0, &action)
                 == BROWSER_PREVIEW_ACTIVATION_DEFERRED
          && action.type == CONTROLLER_ACTION_NONE);
    CHECK(browser_engine_provisional_focus_direction(
              engine, CONTROLLER_FOCUS_DOWN)
          && browser_engine_provisional_activate(
                 engine, false, 0, 0, &action)
                 == BROWSER_PREVIEW_ACTIVATION_NAVIGATE
          && action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(action.url, "https://preview-links.test/p/0") == 0
          && strcmp(action.method, "GET") == 0);
    /* X under the cursor on the search field: it waits for the page. */
    CHECK(browser_engine_provisional_activate(engine, true, 10, 10, &action)
              == BROWSER_PREVIEW_ACTIVATION_DEFERRED
          && !browser_engine_take_deferred_activation(engine));
    for (size_t pumps = 0;
         pumps < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    ControllerTextInputInfo text = {0};
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && browser_engine_take_deferred_activation(engine)
          && !browser_engine_take_deferred_activation(engine)
          && browser_engine_text_input_info(engine, &text)
          && text.editable);
    fetch_trace_end();

    /* An activation the frontend never took belongs to its page only: the
       next navigation starts without it. */
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-links",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-links.test/document",
                 256u * 1024u, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && !browser_engine_provisional_viewport(engine, &viewport);
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(browser_engine_provisional_activate(engine, true, 10, 10, &action)
          == BROWSER_PREVIEW_ACTIVATION_DEFERRED);
    for (size_t pumps = 0;
         pumps < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    fetch_trace_end();
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && fetch_trace_replay_begin(
                 TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-links",
                 error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-links.test/document",
                 256u * 1024u, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && !browser_engine_take_deferred_activation(engine));
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0);
    browser_engine_destroy(engine);
    return 0;
}

/* Bytes are not content: while the parser is inside a long run of inline
   <style> elements, a reader waiting at the end of the preview gets no new layout (each
   would hold the parser up for nothing); once elements close again, the
   preview follows them. */
static int test_preview_waits_out_style_run(void)
{
    enum { STYLE_START = 2221, STYLE_END = 23773 };
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 1u * MIB;
    config.tile_capacity = 4;
    config.resources.enabled = true;
    config.javascript.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation_set_stream_delivery(navigation, 512, 0, 0, 0, 0, 0);
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 512,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 10000
            },
            .maximum_parser_body_bytes = 512,
            .maximum_parser_time_us = 10000,
            .maximum_finalize_work_units = 8,
            .maximum_finalize_time_us = 1000
        }
    };
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-preview-style-run",
              error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://preview-style-run.test/document",
                 256u * 1024u, 1000, true));
    BrowserProvisionalViewport viewport = {0};
    BrowserNavigationJobMetrics job = {0};
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && !browser_engine_provisional_viewport(engine, &viewport);
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING
          && browser_engine_navigation_job_metrics(engine, &job)
          && job.load.body_bytes < STYLE_START + 1024u);
    /* The reader pages to where the laid-out content ends; the lead
       already parsed is laid out for them, and then the style run starts. */
    while (browser_engine_scroll_provisional_page(engine, 1)) {}
    CHECK(navigation->provisional_reader_y > 0);
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && job.load.body_bytes < STYLE_START + 1024u;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
        CHECK(browser_engine_navigation_job_metrics(engine, &job));
        while (browser_engine_scroll_provisional_page(engine, 1)) {}
    }
    size_t paints = job.provisional_paints;
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && job.load.body_bytes < STYLE_END;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
        CHECK(browser_engine_navigation_job_metrics(engine, &job));
        if (job.load.body_bytes < STYLE_END)
            CHECK(job.provisional_paints == paints);
    }
    for (size_t pumps = 0;
         pumps < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING
             && job.provisional_paints == paints;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
        CHECK(browser_engine_navigation_job_metrics(engine, &job));
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING
          && job.provisional_paints == paints + 1u);
    for (size_t pumps = 0;
         pumps < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
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
        || !provisional.ready || provisional.maximum_scroll_y <= 0) {
        (void) browser_engine_navigation_job_metrics(engine, &job);
        fprintf(stderr,
                "provisional pending=%d ready=%d end=%d scroll=%d/%d "
                "paints=%zu bytes=%zu stream=%zu/%zu/%zu partial=%zu/%zu "
                "budget=%zu/%zu status=%d error=\"%s\"\n",
                status == BROWSER_NAVIGATION_JOB_PENDING,
                provisional.ready, provisional.content_end_y,
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
          && provisional.scroll_y == 0
          && provisional.content_end_y > 0
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
          && provisional.scroll_y > 0
          && provisional.scroll_y <= provisional.maximum_scroll_y
          && frame_checksum(
                 provisional.pixels, provisional.pixel_count)
                 != provisional_top_hash);
    CHECK(present_probe.calls >= 2
          && present_probe.last_checksum
                 == frame_checksum(
                        provisional.pixels, provisional.pixel_count));
    job = (BrowserNavigationJobMetrics) {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &job)
          && job.provisional_paints >= 1
          && job.provisional_scrolls == 1
          && job.provisional_bytes >= 480u * 272u * sizeof(uint16_t));
    browser_engine_cancel_navigation(engine, "provisional rollback test");
    CHECK(browser_engine_navigation_status(engine)
              == BROWSER_NAVIGATION_JOB_CANCELLED
          && !browser_engine_provisional_viewport(engine, &provisional)
          && strcmp(navigation->page.document.title,
                    "Stable incumbent") == 0
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
    bool pressed = false;
    for (size_t pumps = 0;
         pumps < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
        if (!pressed
            && browser_engine_provisional_viewport(engine, &provisional))
            pressed = browser_engine_scroll_provisional_page(engine, 1);
        while (browser_engine_provisional_raster_step(engine)) {}
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
