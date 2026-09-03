static int test_cooperative_site_adapter_navigation(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 512u * 1024u;
    config.maximum_document_bytes = 1u * MIB;
    config.tile_capacity = 0;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char incumbent[] =
        "<!doctype html><title>Stable incumbent</title><body>old";
    static const char url[] =
        "https://www.youtube.com/watch?v=TFTEST00001"
        "&tilefinch_view=comments";
    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube";
    BrowserNavigationJobQuota quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 37,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 1000
            },
            .maximum_parser_time_us = 1000
        }
    };
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://incumbent.test/", incumbent,
                 sizeof(incumbent) - 1, true)
          && fetch_trace_replay_begin(
                 fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, url, 1u * MIB, 1000, true)
          && browser_engine_pump_navigation(engine, &quota)
                 == BROWSER_NAVIGATION_JOB_PENDING);
    browser_engine_cancel_navigation(engine, "adapter cancellation test");
    CHECK(browser_engine_navigation_status(engine)
              == BROWSER_NAVIGATION_JOB_CANCELLED
          && strcmp(
                 browser_engine_navigation(engine)->page.document.title,
                 "Stable incumbent") == 0);
    fetch_trace_end();

    /* Rapid route changes must retire the candidate adapter completely and
       leave the incumbent usable. Vary the cancellation point across the
       transport and transform phases; this is the host equivalent of Back or
       a new search arriving while a watch page is still being assembled. */
    for (size_t cycle = 0; cycle < 24u; cycle++) {
        CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
              && browser_engine_begin_navigation_url(
                     engine, url, 1u * MIB, 1000, false));
        BrowserNavigationJobStatus cancelled_status =
            BROWSER_NAVIGATION_JOB_PENDING;
        size_t slices = 1u + cycle % 6u;
        for (size_t at = 0;
             at < slices
             && cancelled_status == BROWSER_NAVIGATION_JOB_PENDING;
             at++) {
            cancelled_status = browser_engine_pump_navigation(
                engine, &quota);
        }
        CHECK(cancelled_status == BROWSER_NAVIGATION_JOB_PENDING);
        browser_engine_cancel_navigation(
            engine, "rapid YouTube route cancellation test");
        BrowserEngineMetrics cancelled_metrics = {0};
        CHECK(browser_engine_navigation_status(engine)
                  == BROWSER_NAVIGATION_JOB_CANCELLED
              && browser_engine_metrics(engine, &cancelled_metrics)
              && cancelled_metrics.allocation_failures == 0
              && strcmp(
                     browser_engine_navigation(engine)->page.document.title,
                     "Stable incumbent") == 0);
        fetch_trace_end();
    }

    /* Optional continuation failure degrades to the watch page rather than
       discarding a valid primary response. Inject after begin so the already
       enqueued primary request is unaffected and the second enqueue consumes
       the deterministic failure. */
    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, url, 1u * MIB, 1000, true));
    fetch_inject_failure_once(FETCH_INJECT_TIMEOUT);
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && strstr(
                 browser_engine_navigation(engine)->page.document.title,
                 "Cooperative fixture video") != NULL);
    fetch_trace_end();

    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, url, 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    BrowserNavigationJobMetrics metrics = {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &metrics));
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED) {
        fprintf(stderr,
                "cooperative adapter failed: status=%d error=%s "
                "pumps=%zu body=%zu\n",
                (int) status, browser_engine_last_error(engine),
                metrics.pump_calls, metrics.load.body_bytes);
    }
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && metrics.pump_calls > 6
          && metrics.load.pump_calls > 4
          && metrics.load.body_bytes == 1190
          && metrics.maximum_transform_slice_us < UINT64_C(50000)
          && metrics.transform_slices > 6
          && metrics.maximum_irreducible_unit_us < UINT64_C(100000)
          && metrics.adapter_commit_us > 0
          && metrics.adapter_parse_us <= metrics.adapter_commit_us
          && metrics.adapter_style_us <= metrics.adapter_commit_us
          && metrics.adapter_resource_us <= metrics.adapter_commit_us
          && metrics.adapter_layout_us <= metrics.adapter_commit_us
          && metrics.adapter_runtime_us == 0
          && strstr(
                 browser_engine_navigation(engine)->page.document.title,
                 "Cooperative fixture video") != NULL
          && browser_engine_navigation(engine)->page.runtime == NULL
          && browser_engine_navigation(engine)->page.document.node_count > 20);
    const char *body_text = document_body_text(
        &browser_engine_navigation(engine)->page.document);
    CHECK(body_text != NULL
          && strstr(body_text, "Cooperative comment") != NULL
          && strstr(body_text, "Load more comments") == NULL);
    fetch_trace_end();

    /* Exercise the provider's user-facing watch subroutes through complete
       BrowserEngine transactions. Description is primary-only, an ordinary
       watch exposes both optional views, and neither may inherit comments
       from the prior document. */
    static const char description_url[] =
        "https://www.youtube.com/watch?v=TFTEST00001"
        "&tilefinch_view=description";
    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, description_url, 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    body_text = document_body_text(
        &browser_engine_navigation(engine)->page.document);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && body_text != NULL
          && strstr(body_text, "A bounded fixture.") != NULL
          && strstr(body_text, "View full description") == NULL
          && strstr(body_text, "Cooperative comment") == NULL);
    fetch_trace_end();

    static const char watch_url[] =
        "https://www.youtube.com/watch?v=TFTEST00001";
    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, watch_url, 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    body_text = document_body_text(
        &browser_engine_navigation(engine)->page.document);
    BrowserEngineMetrics route_metrics = {0};
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && browser_engine_metrics(engine, &route_metrics)
          && route_metrics.allocation_failures == 0
          && route_metrics.budget_peak < route_metrics.budget_limit
          && body_text != NULL
          && strstr(body_text, "View full description") != NULL
          && strstr(body_text, "View comments") != NULL
          && strstr(body_text, "Cooperative comment") == NULL);
    ControllerAction watch_play = {0};
    CHECK(browser_engine_activate(engine, &watch_play)
          && watch_play.type == CONTROLLER_ACTION_NAVIGATE
          && watch_play.prefer_native_media
          && strcmp(watch_play.url, watch_url) == 0);
    fetch_trace_end();

    /* Same-site repeat navigation should reach the same result with bounded
       logical work and no accumulating page allocations. Wall-clock timing is
       deliberately not asserted: pump counts and transform slices are stable
       across noisy host runners and correspond to work on the PSP. */
    BrowserEngineMetrics repeat_baseline = {0};
    CHECK(browser_engine_metrics(engine, &repeat_baseline));
    size_t repeat_pump_ceiling = 0;
    size_t repeat_transform_ceiling = 0;
    for (size_t cycle = 0; cycle < 16u; cycle++) {
        CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
              && browser_engine_begin_navigation_url(
                     engine, watch_url, 1u * MIB, 1000, false));
        status = BROWSER_NAVIGATION_JOB_PENDING;
        for (size_t pumps = 0;
             pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
             pumps++) {
            status = browser_engine_pump_navigation(engine, &quota);
        }
        BrowserNavigationJobMetrics repeat_job = {0};
        BrowserEngineMetrics repeat_engine = {0};
        CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
              && browser_engine_navigation_job_metrics(engine, &repeat_job)
              && browser_engine_metrics(engine, &repeat_engine)
              && repeat_engine.allocation_failures == 0
              && repeat_engine.budget_current
                     <= repeat_baseline.budget_current + 8u * 1024u
              && repeat_engine.budget_active_allocations
                     <= repeat_baseline.budget_active_allocations + 8u);
        if (cycle == 0) {
            repeat_pump_ceiling = repeat_job.pump_calls + 2u;
            repeat_transform_ceiling = repeat_job.transform_slices + 2u;
        } else {
            CHECK(repeat_job.pump_calls <= repeat_pump_ceiling
                  && repeat_job.transform_slices
                         <= repeat_transform_ceiling);
        }
        fetch_trace_end();
    }

    /* Provider cards contain several LinkRegions per semantic anchor. Focus
       traversal and history store the anchor ordinal, not a text fragment. */
    BrowserController *youtube_controller =
        browser_engine_controller(engine);
    lxb_dom_node_t *youtube_play =
        youtube_controller == NULL ? NULL : youtube_controller->focus_node;
    CHECK(youtube_play != NULL
          && youtube_controller->focus_kind == CONTROLLER_FOCUS_LINK
          && lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(youtube_play),
                 (const lxb_char_t *) "autofocus", 9));
    lxb_dom_node_t *youtube_search = NULL;
    for (size_t i = 0;
         youtube_controller != NULL
         && i < browser_engine_navigation(
                    engine)->page.layout.control_count; i++) {
        lxb_dom_node_t *node = browser_engine_navigation(
            engine)->page.layout.controls[i].node;
        size_t id_length = 0;
        const char *id = document_attribute(node, "id", &id_length);
        if (id != NULL && id_length == 9
            && memcmp(id, "yt-search", 9) == 0) {
            youtube_search = node;
            break;
        }
    }
    CHECK(youtube_search != NULL
          && browser_engine_focus_node(engine, youtube_search)
          && browser_engine_focus_node(engine, youtube_play));
    const LayoutNodeBox *youtube_play_box = layout_box_for_node(
        &browser_engine_navigation(engine)->page.layout, youtube_play);
    int youtube_focus_x = 0, youtube_focus_y = 0;
    int youtube_focus_width = 0, youtube_focus_height = 0;
    CHECK(youtube_play_box != NULL
          && controller_focused_rect(
                 youtube_controller, &youtube_focus_x, &youtube_focus_y,
                 &youtube_focus_width, &youtube_focus_height)
          && youtube_focus_x == youtube_play_box->x
          && youtube_focus_y == youtube_play_box->y
          && youtube_focus_width == youtube_play_box->width
          && youtube_focus_height == youtube_play_box->height);
    size_t play_fragments = 0;
    for (size_t i = 0;
         i < browser_engine_navigation(engine)->page.layout.link_count; i++) {
        if (browser_engine_navigation(engine)->page.layout.links[i].node
                == youtube_play) {
            play_fragments++;
        }
    }
    size_t raw_play_index = youtube_controller->focus_index;
    CHECK(play_fragments > 0
          && browser_engine_focus_move(engine, true)
          && youtube_controller->focus_node != youtube_play
          && browser_engine_focus_move(engine, false)
          && youtube_controller->focus_node == youtube_play);
    CHECK(browser_engine_scroll_by(engine, 83));
    const NavigationEntry *focused_watch =
        navigation_current(browser_engine_navigation(engine));
    CHECK(raw_play_index == 1
          && focused_watch != NULL
          && focused_watch->focus_kind == CONTROLLER_FOCUS_LINK
          && focused_watch->focus_index == 1
          && focused_watch->scroll_y > 0);
    int focused_watch_scroll = focused_watch->scroll_y;

    static const char away[] =
        "<!doctype html><title>Away</title><p>Temporary page";
    CHECK(browser_engine_commit_html(
              engine, "https://away.test/", away, sizeof(away) - 1u, true)
          && fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_begin_navigation_history(
                 engine, false, 1u * MIB, 1000));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    youtube_controller = browser_engine_controller(engine);
    focused_watch = navigation_current(browser_engine_navigation(engine));
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && browser_engine_last_navigation_return_target(engine)
                 == BROWSER_NAVIGATION_RETURN_FORWARD
          && focused_watch != NULL
          && focused_watch->scroll_y == focused_watch_scroll
          && youtube_controller->focus_kind == CONTROLLER_FOCUS_LINK
          && youtube_controller->focus_node != NULL
          && youtube_controller->focus_index
                 < browser_engine_navigation(engine)->page.layout.link_count
          && strstr(
                 browser_engine_navigation(engine)->page.layout.links[
                     youtube_controller->focus_index].url,
                 "watch?v=TFTEST00001") != NULL);
    fetch_trace_end();

    /* Once a normal provider page has supplied a validated API identity,
       search continuation should issue only the small POST.  This replay has
       no primary GET record, so any regression to re-fetching/re-decoding the
       mobile search page fails deterministically. */
    static const char continuation_fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube-continuation";
    static const char continuation_url[] =
        "https://www.youtube.com/results?search_query=fixture"
        "&tilefinch_token=NEXT";
    CHECK(fetch_trace_replay_begin(
              continuation_fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, continuation_url, 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 64 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    metrics = (BrowserNavigationJobMetrics) {0};
    youtube_controller = browser_engine_controller(engine);
    CHECK(browser_engine_navigation_job_metrics(engine, &metrics)
          && status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && metrics.load.body_bytes == 317
          && metrics.transform_slices > 6
          && strstr(
              browser_engine_navigation(engine)->page.document.title,
              "fixture - YouTube") != NULL
          && browser_engine_navigation(engine)->page.document.node_count
                 > 10
          && youtube_controller != NULL
          && youtube_controller->focus_kind == CONTROLLER_FOCUS_LINK
          && youtube_controller->focus_node != NULL
          && lxb_dom_element_has_attribute(
                 lxb_dom_interface_element(youtube_controller->focus_node),
                 (const lxb_char_t *) "autofocus", 9));
    fetch_trace_end();

    /* Rejection invalidates the cached provider configuration and retries
       exactly once through the ordinary primary-page route. */
    static const char continuation_fallback_fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube-continuation-fallback";
    CHECK(fetch_trace_replay_begin(
              continuation_fallback_fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, continuation_url, 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 96 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    metrics = (BrowserNavigationJobMetrics) {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &metrics)
          && status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && metrics.load.body_bytes == 621
          && strstr(
              browser_engine_navigation(engine)->page.document.title,
              "fixture - YouTube") != NULL);
    fetch_trace_end();

    /* The home surface declares the search box as its entry point. Its
       `autofocus` has to survive markup generation intact, or the engine
       falls through to the first focusable node, which is the brand link
       above the box. */
    static const char home_fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube-home";
    CHECK(fetch_trace_replay_begin(home_fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
                 engine, "https://www.youtube.com/", 1u * MIB, 1000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 128 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &quota);
    }
    youtube_controller = browser_engine_controller(engine);
    size_t home_focus_id_length = 0;
    const char *home_focus_id =
        youtube_controller == NULL || youtube_controller->focus_node == NULL
            ? NULL
            : document_attribute(
                  youtube_controller->focus_node, "id",
                  &home_focus_id_length);
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && youtube_controller != NULL
          && youtube_controller->focus_kind == CONTROLLER_FOCUS_CONTROL
          && home_focus_id != NULL
          && home_focus_id_length == 9
          && memcmp(home_focus_id, "yt-search", 9) == 0);
    fetch_trace_end();

    /* A realistically page-sized response must expose transport and CPU
       progress in separate bounded pumps. Repetitive padding keeps the
       fixture cheap on disk while forcing the same 768 KiB decode walk. */
    char large_fixture[128];
    size_t large_body_length = 0;
    CHECK(write_large_youtube_replay(
              large_fixture, &large_body_length));
    BrowserNavigationJobQuota large_quota = {
        .load = {
            .fetch = {
                .maximum_body_bytes = 16u * 1024u,
                .maximum_body_callbacks = 1,
                .maximum_time_us = 2000
            },
            .maximum_parser_time_us = 50000
        }
    };
    BrowserNavigationJobQuota build_cancel_quota = large_quota;
    build_cancel_quota.load.fetch.maximum_time_us = 1;
    char build_incumbent_title[256];
    snprintf(
        build_incumbent_title, sizeof(build_incumbent_title), "%s",
        browser_engine_navigation(engine)->page.document.title);
    CHECK(fetch_trace_replay_begin(
              large_fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
              engine,
              "https://www.youtube.com/results?search_query=ratchet",
              1u * MIB, 2000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(
            engine, &build_cancel_quota);
        BrowserNavigationJobMetrics progress = {0};
        if (browser_engine_navigation_job_metrics(engine, &progress)
            && progress.transform_slices >= 3u) break;
    }
    BrowserNavigationJobMetrics cancelled_build = {0};
    CHECK(status == BROWSER_NAVIGATION_JOB_PENDING
          && browser_engine_navigation_job_metrics(
              engine, &cancelled_build)
          && cancelled_build.transform_slices >= 3u
          && cancelled_build.transform_quota_overruns != 0);
    browser_engine_cancel_navigation(
        engine, "cooperative build cancellation test");
    CHECK(browser_engine_navigation_status(engine)
              == BROWSER_NAVIGATION_JOB_CANCELLED
          && strcmp(
              browser_engine_navigation(engine)->page.document.title,
              build_incumbent_title) == 0);
    fetch_trace_end();

    /* The result density is a browser preference, not a page-local link or
       query parameter. The adapter must apply it to an ordinary search URL. */
    CHECK(browser_engine_set_youtube_compact_results(engine, true));
    CHECK(fetch_trace_replay_begin(
              large_fixture, error, sizeof(error))
          && browser_engine_begin_navigation_url(
              engine,
              "https://www.youtube.com/results?search_query=ratchet",
              1u * MIB, 2000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 256 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &large_quota);
    }
    metrics = (BrowserNavigationJobMetrics) {0};
    lxb_dom_node_t *compact_body = document_body_node(
        &browser_engine_navigation(engine)->page.document);
    size_t compact_class_length = 0;
    const char *compact_class = compact_body == NULL ? NULL
        : document_attribute(
              compact_body, "class", &compact_class_length);
    CHECK(browser_engine_navigation_job_metrics(engine, &metrics)
          && status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && metrics.load.body_bytes == large_body_length
          && metrics.pump_calls > 80
          && metrics.maximum_transform_slice_us < UINT64_C(50000)
          && metrics.transform_slices > 12
          && metrics.maximum_irreducible_unit_us < UINT64_C(100000)
          && compact_class != NULL
          && compact_class_length == strlen("compact-results")
          && memcmp(compact_class, "compact-results",
                    compact_class_length) == 0
          && strstr(
              browser_engine_navigation(engine)->page.document.title,
              "ratchet") != NULL);
    char focused_provider_url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(browser_engine_focused_provider_media_url(
              engine, focused_provider_url,
              sizeof(focused_provider_url)));
    ControllerAction compact_play = {0};
    CHECK(browser_engine_activate(engine, &compact_play)
          && compact_play.prefer_native_media
          && strcmp(focused_provider_url, compact_play.url) == 0
          && browser_engine_focus_direction(
                 engine, CONTROLLER_FOCUS_RIGHT));
    CHECK(!browser_engine_focused_provider_media_url(
              engine, focused_provider_url,
              sizeof(focused_provider_url))
          && focused_provider_url[0] == '\0');
    ControllerAction compact_details = {0};
    CHECK(browser_engine_activate(engine, &compact_details)
          && !compact_details.prefer_native_media
          && strstr(compact_play.url, "search_query=") == NULL
          && strstr(compact_details.url, "search_query=") != NULL
          && strlen(compact_play.url)
                 == (size_t) (strstr(
                         compact_details.url, "&search_query=")
                     - compact_details.url)
          && memcmp(compact_play.url, compact_details.url,
                    strlen(compact_play.url)) == 0
          && browser_engine_focus_direction(
                 engine, CONTROLLER_FOCUS_LEFT));
    ControllerAction compact_play_again = {0};
    CHECK(browser_engine_activate(engine, &compact_play_again)
          && compact_play_again.prefer_native_media
          && strcmp(compact_play_again.url, compact_play.url) == 0
          && browser_engine_focus_direction(
                 engine, CONTROLLER_FOCUS_DOWN));
    ControllerAction compact_next_play = {0};
    bool compact_next_activated = browser_engine_activate(
        engine, &compact_next_play);
    CHECK(compact_next_activated
          && compact_next_play.prefer_native_media
          && strcmp(compact_next_play.url, compact_play.url) != 0);
    fetch_trace_end();

    /* The exact repeated search is rebuilt from the two-entry, cookie-bound
       RAM cache. It must not consume another trace response or report any
       network work; the compact preference is part of the cache variant. */
    CHECK(browser_engine_begin_navigation_url(
              engine,
              "https://www.youtube.com/results?search_query=ratchet",
              1u * MIB, 2000, true));
    status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0;
         pumps < 32 && status == BROWSER_NAVIGATION_JOB_PENDING;
         pumps++) {
        status = browser_engine_pump_navigation(engine, &large_quota);
    }
    BrowserNavigationJobMetrics cached_metrics = {0};
    CHECK(browser_engine_navigation_job_metrics(engine, &cached_metrics)
          && status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && cached_metrics.adapter_document_cache_hits == 1
          && cached_metrics.adapter_document_cache_stores == 0
          && cached_metrics.adapter_transport_samples == 0
          && cached_metrics.load.body_bytes == 0
          && strstr(
              browser_engine_navigation(engine)->page.document.title,
              "ratchet") != NULL);
    remove_large_youtube_replay(large_fixture);

    /* The provider's primary result action remains a canonical navigation URL
       with an explicit native-media preference. A frontend without the native
       route can ignore the hint and navigate; the adjacent Details action is
       deliberately unmarked. */
    static const char provider_actions[] =
        "<!doctype html><title>Provider results</title>"
        "<a autofocus data-tilefinch-provider-media "
        "href='https://www.youtube.com/watch?v=TFTEST00001'>"
        "<span><img alt='Preview'><span>Play</span></span></a>"
        "<a href='https://www.youtube.com/watch?v=TFTEST00001'>Details</a>";
    CHECK(browser_engine_commit_html(
        engine, "https://www.youtube.com/results?search_query=fixture",
        provider_actions, sizeof(provider_actions) - 1u, true));
    /* An incremental provider relayout may invalidate the controller's
       region index between autofocus and the user's first Cross press. The
       authored target is still unambiguous, so restoring it must allow that
       same press to activate Play instead of consuming a focus-only press. */
    BrowserController *provider_controller =
        browser_engine_controller(engine);
    CHECK(provider_controller != NULL);
    provider_controller->focus_kind = CONTROLLER_FOCUS_NONE;
    provider_controller->focus_index = 0;
    provider_controller->focus_node = NULL;
    provider_controller->focus_handle = 0;
    provider_controller->focus_link_url[0] = '\0';
    provider_controller->focus_link_url_length = 0;
    CHECK(browser_engine_restore_autofocus(engine));
    ControllerAction restored_provider_action = {0};
    CHECK(browser_engine_activate(engine, &restored_provider_action)
          && restored_provider_action.type == CONTROLLER_ACTION_NAVIGATE
          && restored_provider_action.prefer_native_media
          && strcmp(
                 restored_provider_action.url,
                 "https://www.youtube.com/watch?v=TFTEST00001") == 0);
    /* Also cover the narrower race: the press arrives with a stale region
       owner, activation discovers that it is gone, and the facade repairs
       authored autofocus before returning from that same call. */
    lxb_dom_node_t *retained_provider_node = provider_controller->focus_node;
    CHECK(retained_provider_node != NULL);
    provider_controller->focus_kind = CONTROLLER_FOCUS_LINK;
    provider_controller->focus_index = SIZE_MAX;
    provider_controller->focus_node = retained_provider_node;
    provider_controller->focus_handle = 0;
    provider_controller->focus_link_url[0] = '\0';
    provider_controller->focus_link_url_length = 0;
    char stale_focused_url[NAVIGATION_URL_LIMIT] = {0};
    CHECK(browser_engine_focused_provider_media_url(
              engine, stale_focused_url, sizeof(stale_focused_url))
          && strcmp(
                 stale_focused_url,
                 "https://www.youtube.com/watch?v=TFTEST00001") == 0);
    provider_controller->focus_node = NULL;
    ControllerAction stale_provider_action = {0};
    bool stale_provider_activated =
        browser_engine_activate(engine, &stale_provider_action);
    if (!stale_provider_activated
        || stale_provider_action.type != CONTROLLER_ACTION_NAVIGATE
        || !stale_provider_action.prefer_native_media) {
        fprintf(stderr,
                "stale provider activation: accepted=%d focus=%d index=%zu "
                "type=%d native=%d url=%s\n",
                stale_provider_activated ? 1 : 0,
                (int) provider_controller->focus_kind,
                provider_controller->focus_index,
                (int) stale_provider_action.type,
                stale_provider_action.prefer_native_media ? 1 : 0,
                stale_provider_action.url);
    }
    CHECK(stale_provider_activated
          && stale_provider_action.type == CONTROLLER_ACTION_NAVIGATE
          && stale_provider_action.prefer_native_media
          && strcmp(
                 stale_provider_action.url,
                 "https://www.youtube.com/watch?v=TFTEST00001") == 0);
    const LayoutDocument *provider_layout =
        &browser_engine_navigation(engine)->page.layout;
    lxb_dom_node_t *provider_node = NULL;
    for (size_t at = 0; at < provider_layout->link_count; at++) {
        lxb_dom_node_t *node = provider_layout->links[at].node;
        if (lxb_dom_element_has_attribute(
                lxb_dom_interface_element(node),
                (const lxb_char_t *) "data-tilefinch-provider-media",
                sizeof("data-tilefinch-provider-media") - 1u)) {
            provider_node = node;
            break;
        }
    }
    CHECK(provider_node != NULL
          && browser_engine_focus_node(engine, provider_node));
    ControllerAction provider_action = {0};
    bool provider_activated = browser_engine_activate(engine, &provider_action);
    if (!provider_activated
        || provider_action.type != CONTROLLER_ACTION_NAVIGATE
        || !provider_action.prefer_native_media
        || strcmp(
               provider_action.url,
               "https://www.youtube.com/watch?v=TFTEST00001") != 0) {
        fprintf(stderr,
                "provider action: activated=%d type=%d native=%d url=%s\n",
                provider_activated ? 1 : 0, (int) provider_action.type,
                provider_action.prefer_native_media ? 1 : 0,
                provider_action.url);
    }
    CHECK(provider_activated
          && provider_action.type == CONTROLLER_ACTION_NAVIGATE
          && provider_action.prefer_native_media
          && strcmp(
                 provider_action.url,
                 "https://www.youtube.com/watch?v=TFTEST00001") == 0);
    CHECK(browser_engine_focus_move(engine, true));
    ControllerAction details_action = {0};
    CHECK(browser_engine_activate(engine, &details_action)
          && details_action.type == CONTROLLER_ACTION_NAVIGATE
          && !details_action.prefer_native_media
          && strcmp(details_action.url, provider_action.url) == 0);

    /* Rapid Play/Details alternation must never turn the primary action into
       a document navigation, and cancelling a Details navigation must leave
       the result document and its controls immediately usable. Vary the
       cancellation point across transport and adapter transformation. */
    for (size_t cycle = 0; cycle < 16u; cycle++) {
        CHECK(browser_engine_focus_node(engine, provider_node));
        ControllerAction rapid_play = {0};
        CHECK(browser_engine_activate(engine, &rapid_play)
              && rapid_play.prefer_native_media
              && strcmp(rapid_play.url, provider_action.url) == 0
              && strcmp(browser_engine_navigation(engine)->page.document.title,
                        "Provider results") == 0
              && browser_engine_focus_move(engine, true));
        ControllerAction rapid_details = {0};
        CHECK(browser_engine_activate(engine, &rapid_details)
              && !rapid_details.prefer_native_media
              && strcmp(rapid_details.url, rapid_play.url) == 0
              && fetch_trace_replay_begin(fixture, error, sizeof(error))
              && browser_engine_begin_navigation_url(
                     engine, rapid_details.url, 1u * MIB, 1000, false));
        BrowserNavigationJobStatus rapid_status =
            BROWSER_NAVIGATION_JOB_PENDING;
        size_t slices = 1u + cycle % 6u;
        for (size_t at = 0;
             at < slices
             && rapid_status == BROWSER_NAVIGATION_JOB_PENDING; at++) {
            rapid_status = browser_engine_pump_navigation(engine, &quota);
        }
        CHECK(rapid_status == BROWSER_NAVIGATION_JOB_PENDING);
        browser_engine_cancel_navigation(
            engine, "rapid provider Details cancellation test");
        CHECK(browser_engine_navigation_status(engine)
                  == BROWSER_NAVIGATION_JOB_CANCELLED
              && strcmp(browser_engine_navigation(engine)->page.document.title,
                        "Provider results") == 0
              && browser_engine_focus_node(engine, provider_node));
        fetch_trace_end();
    }

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static lxb_dom_node_t *test_reader_find_id(lxb_dom_node_t *root,
                                           const char *id);
static lxb_dom_node_t *test_reader_root_node(PocDocument *document);

static int test_google_search_compatibility_adapter(void)
{
    static const char url[] =
        "https://www.google.com/search?q=PSP%20%26%20games%22%3E";
    CHECK(site_adapter_handles_navigation("GET", url)
          && !site_adapter_navigation_requires_network("GET", url)
          && !site_adapter_handles_navigation(
                 "GET", "https://www.google.com/search?q=PSP&tilefinch_raw=1")
          && !site_adapter_handles_navigation(
                 "GET", "https://notgoogle.test/search?q=PSP"));

    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL
          && browser_engine_begin_navigation_url(
                 engine, url, 64u * 1024u, 1000, true));
    BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
    for (size_t pumps = 0u;
         pumps < 32u && status == BROWSER_NAVIGATION_JOB_PENDING; pumps++) {
        status = browser_engine_pump_navigation(engine, NULL);
    }
    NavigationSession *navigation = browser_engine_navigation(engine);
    const char *body = navigation == NULL ? NULL
        : document_body_text(&navigation->page.document);
    lxb_dom_node_t *search = navigation == NULL ? NULL
        : actionable_find_id(&navigation->page.document, "compat-search");
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && navigation != NULL
          && strcmp(navigation->page.document.title, "Web search") == 0
          && navigation->page.runtime == NULL
          && body != NULL
          && strstr(body, "Google needs JavaScript") != NULL
          && strstr(body, "Search with DuckDuckGo") != NULL
          && strstr(body, "PSP & games\">") == NULL
          && search != NULL
          && browser_engine_focus_node(engine, search));
    ControllerAction action = {0};
    bool activated = browser_engine_activate(engine, &action);
    if (!activated || action.type != CONTROLLER_ACTION_FORM_SUBMIT
        || strstr(action.url, "duckduckgo") == NULL) {
        fprintf(stderr, "google compat activation=%d type=%d url=%s\n",
                activated ? 1 : 0, (int) action.type, action.url);
    }
    CHECK(activated
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strncmp(action.url, "https://lite.duckduckgo.com/lite/?q=",
                     sizeof("https://lite.duckduckgo.com/lite/?q=") - 1u) == 0
          && strstr(action.url, "PSP") != NULL
          && strstr(action.url, "games") != NULL);

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static const DrawCommand *test_reader_text_command(
    const LayoutDocument *layout, const char *text)
{
    if (layout == NULL || text == NULL) return NULL;
    size_t length = strlen(text);
    for (size_t at = 0; at < layout->count; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (command->type == DRAW_TEXT && command->text != NULL
            && command->text_length == length
            && memcmp(command->text, text, length) == 0) return command;
    }
    return NULL;
}

static int test_reader_presentation_adapter(void)
{
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(!site_adapter_handles_navigation(
              "GET", TILEFINCH_HOMEPAGE_URL)
          && !site_adapter_reader_css(
              TILEFINCH_HOMEPAGE_URL, SITE_ADAPTER_READER_FONT_SANS,
              100, css, sizeof(css), adapter, sizeof(adapter))
          && site_adapter_reader_css(
              "https://en.m.wikipedia.org/wiki/PSP",
              SITE_ADAPTER_READER_FONT_SERIF, 125,
              css, sizeof(css), adapter, sizeof(adapter))
          && strcmp(adapter, "reader-content-shape") == 0
          && strstr(css, "font-size:125%") != NULL
          && strstr(css, "font-family:serif") != NULL
          && strstr(css, "data-tilefinch-reader-root") != NULL
          && strstr(css, "body:not([data-tilefinch-reader-kind])") == NULL
          && strstr(css, "background:#f7f3ea!important") != NULL
          && strstr(css, "color:#292724!important") != NULL
          && strstr(css, "table-layout:fixed!important") != NULL
          && strstr(css, "overflow:hidden!important") != NULL
          && strlen(css) < SITE_ADAPTER_READER_CSS_LIMIT
          && site_adapter_reader_css(
              "https://news.example.test/article",
              SITE_ADAPTER_READER_FONT_SANS, 100,
              css, sizeof(css), adapter, sizeof(adapter))
          && strcmp(adapter, "reader-content-shape") == 0);

    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 4;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    /* Child selectors cannot hide anonymous boxes created by direct body
       text. Exercise the actual layout/paint list: the active Basic root
       must restore readable inherited metrics while the authored body text
       contributes no text command. */
    static const char direct_body_text[] =
        "<!doctype html><title>Basic inherited text</title>"
        "<body data-tilefinch-reader-kind=basic>RAW_DIRECT_MARKER"
        "<main data-tilefinch-reader-root=basic>"
        "<p>EXTRACTED_MARKER</p></main>RAW_TRAILING_MARKER</body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://basic-text.example.test/", direct_body_text,
                 sizeof(direct_body_text) - 1u, true)
          && browser_engine_apply_user_css(engine, css, strlen(css)));
    const LayoutDocument *basic_text_layout =
        &browser_engine_navigation(engine)->page.layout;
    const DrawCommand *extracted_text = test_reader_text_command(
        basic_text_layout, "EXTRACTED_MARKER");
    const DrawCommand *raw_direct = test_reader_text_command(
        basic_text_layout, "RAW_DIRECT_MARKER");
    const DrawCommand *raw_trailing = test_reader_text_command(
        basic_text_layout, "RAW_TRAILING_MARKER");
    bool raw_direct_paints = raw_direct != NULL
        && raw_direct->opacity_scale != 0u;
    bool raw_trailing_paints = raw_trailing != NULL
        && raw_trailing->opacity_scale != 0u;
    if (raw_direct_paints || raw_trailing_paints
        || extracted_text == NULL || extracted_text->opacity_scale == 0u
        || extracted_text->font_size <= 0 || extracted_text->height <= 0) {
        fprintf(stderr,
                "reader inherited text raw-direct=%p font=%d y=%d height=%d "
                "opacity=%u color=%08x raw-trailing=%p font=%d y=%d height=%d "
                "opacity=%u color=%08x extracted=%p font=%d height=%d "
                "opacity=%u color=%08x commands=%zu\n",
                (const void *) raw_direct,
                raw_direct == NULL ? -1 : raw_direct->font_size,
                raw_direct == NULL ? -1 : raw_direct->y,
                raw_direct == NULL ? -1 : raw_direct->height,
                raw_direct == NULL ? 0u : raw_direct->opacity_scale,
                raw_direct == NULL ? 0u : raw_direct->color,
                (const void *) raw_trailing,
                raw_trailing == NULL ? -1 : raw_trailing->font_size,
                raw_trailing == NULL ? -1 : raw_trailing->y,
                raw_trailing == NULL ? -1 : raw_trailing->height,
                raw_trailing == NULL ? 0u : raw_trailing->opacity_scale,
                raw_trailing == NULL ? 0u : raw_trailing->color,
                (const void *) extracted_text,
                extracted_text == NULL ? -1 : extracted_text->font_size,
                extracted_text == NULL ? -1 : extracted_text->height,
                extracted_text == NULL ? 0u : extracted_text->opacity_scale,
                extracted_text == NULL ? 0u : extracted_text->color,
                basic_text_layout->count);
    }
    CHECK(!raw_direct_paints && !raw_trailing_paints
          && extracted_text != NULL && extracted_text->font_size > 0
          && extracted_text->height > 0 && extracted_text->opacity_scale != 0u
          && browser_engine_render_frame(engine, NULL));
    static const char html[] =
        "<!doctype html><title>Reader</title>"
        "<header><nav><a href='/menu'>Menu</a></nav></header>"
        "<main><article><h1>Readable story</h1>"
        "<p>This first substantial paragraph introduces the complete report "
        "with enough readable prose to establish a useful article.</p>"
        "<p>The second paragraph continues the explanation without relying "
        "on page navigation, application chrome, or hidden controls.</p>"
        "<p>A third paragraph preserves a coherent narrative and gives the "
        "bounded classifier strong semantic evidence.</p>"
        "<p>The final paragraph concludes the story and keeps its extracted "
        "Reader tree independently useful.</p>"
        "<p>Additional supporting detail explains the result in plain "
        "language and keeps the article dominant over surrounding chrome.</p>"
        "<p>A closing note records the source context, implications, and "
        "practical next steps for readers using a compact display.</p>"
        "<a href='/next'>Next</a></article></main>"
        "<footer>Footer</footer>";
    ReaderDocumentAnalysis article_reader = {0};
    bool article_committed = engine != NULL && browser_engine_commit_html(
        engine, "https://news.example.test/article",
        html, sizeof(html) - 1u, true);
    size_t raw_links = article_committed
        ? browser_engine_navigation(engine)->page.layout.link_count : 0u;
    bool article_prepared = article_committed
        && browser_engine_prepare_reader(engine, &article_reader);
    bool article_applied = article_prepared
        && browser_engine_apply_user_css(engine, css, strlen(css));
    size_t reader_links = article_applied
        ? browser_engine_navigation(engine)->page.layout.link_count : 0u;
    if (!(article_committed && raw_links == 2u && article_prepared
          && article_reader.kind == READER_PAGE_ARTICLE
          && article_applied && reader_links == 1u)) {
        fprintf(stderr,
                "reader adapter committed=%d raw-links=%zu prepared=%d "
                "kind=%d text=%u extracted=%u applied=%d links=%zu\n",
                article_committed, raw_links, article_prepared,
                (int) article_reader.kind,
                article_reader.visible_text_bytes,
                article_reader.extracted_bytes, article_applied,
                reader_links);
    }
    CHECK(article_committed && raw_links == 2u && article_prepared
          && article_reader.kind == READER_PAGE_ARTICLE
          && article_applied && reader_links == 1u
          && strstr(
                 browser_engine_navigation(engine)->page.layout.links[0].url,
                 "/next") != NULL);
    ControllerAction reader_link = {0};
    CHECK(browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &reader_link)
          && reader_link.type == CONTROLLER_ACTION_NAVIGATE
          && strstr(reader_link.url, "/next") != NULL);

    /* Reader CSS configured before a candidate commit applies to that
       document's first authoritative layout; it need not build and discard
       the author presentation before simplifying the page. */
    static const char next_html[] =
        "<!doctype html><title>Next reader page</title>"
        "<header><a href='/hidden'>Hidden chrome</a></header>"
        "<article><h1>Next story</h1>"
        "<p>The next complete story begins with substantial server-rendered "
        "prose that remains useful without application hydration.</p>"
        "<p>Its second paragraph supplies enough coherent explanation for a "
        "bounded semantic Reader tree.</p>"
        "<p>The third paragraph keeps navigation chrome outside the selected "
        "article while preserving the destination link.</p>"
        "<p>The final paragraph closes the report with predictable readable "
        "content for the small-screen presentation.</p>"
        "<p>Further context preserves a substantial narrative and explains "
        "why the destination remains useful without application code.</p>"
        "<p>A concluding note records the outcome and practical next steps "
        "for readers following this link.</p>"
        "<a href='/third'>Third</a></article>";
    browser_engine_set_reader_candidate_mode(engine, true);
    CHECK(browser_engine_set_user_css(engine, css, strlen(css))
          && browser_engine_commit_html(
                 engine, "https://news.example.test/next",
                 next_html, sizeof(next_html) - 1u, true)
          && browser_engine_navigation(engine)->page.layout.link_count == 1
          && strstr(
                 browser_engine_navigation(engine)->page.layout.links[0].url,
                 "/third") != NULL);
    ReaderDocumentAnalysis candidate_reader = {0};
    CHECK(browser_engine_reader_analysis(engine, &candidate_reader)
          && candidate_reader.prepared);
    DrawCommand *reader_commands =
        browser_engine_navigation(engine)->page.layout.commands;
    budget_inject_failure_after(browser_engine_budget(engine), 0);
    CHECK(!browser_engine_apply_user_css(engine, "", 0));
    budget_clear_failure_injection(browser_engine_budget(engine));
    CHECK(browser_engine_navigation(engine)->page.loaded
          && browser_engine_navigation(engine)->page.layout.commands
                 == reader_commands
          && browser_engine_navigation(engine)->page.layout.link_count == 1);
    CHECK(browser_engine_apply_user_css(engine, "", 0));
    bool restored_hidden_link = false;
    for (size_t index = 0;
         index < browser_engine_navigation(engine)->page.layout.link_count;
         index++) {
        const char *url = browser_engine_navigation(
            engine)->page.layout.links[index].url;
        if (url != NULL && strstr(url, "/hidden") != NULL) {
            restored_hidden_link = true;
            break;
        }
    }
    CHECK(restored_hidden_link);

    /* Applying CSS and publishing its render shell is one transaction. A
       shell refusal after layout adoption must restore the previous sheet,
       layout, scroll position and usable shell rather than leaving the raw
       body hidden under an unactivated extracted view. */
    static const char baseline_css[] =
        "body{color:#203040} header{display:block}";
    CHECK(browser_engine_apply_user_css(
        engine, baseline_css, sizeof(baseline_css) - 1u));
    NavigationSession *css_transaction_navigation =
        browser_engine_navigation(engine);
    CHECK(navigation_set_scroll(css_transaction_navigation, 7));
    size_t css_transaction_owned =
        browser_engine_budget(engine)->current;
    browser_engine_test_refuse_next_render_shell_init();
    CHECK(!browser_engine_apply_user_css(engine, css, strlen(css))
          && css_transaction_navigation->user_css_length
                 == sizeof(baseline_css) - 1u
          && memcmp(css_transaction_navigation->user_css, baseline_css,
                    sizeof(baseline_css) - 1u) == 0
          && navigation_current(css_transaction_navigation)->scroll_y == 7
          && browser_engine_render_shell(engine) != NULL
          && browser_engine_budget(engine)->current
                 == css_transaction_owned);
    bool transaction_hidden_link = false;
    for (size_t index = 0;
         index < css_transaction_navigation->page.layout.link_count;
         index++) {
        const char *url =
            css_transaction_navigation->page.layout.links[index].url;
        if (url != NULL && strstr(url, "/hidden") != NULL) {
            transaction_hidden_link = true;
            break;
        }
    }
    CHECK(transaction_hidden_link);

    /* A manual Reader request on an application/login-shaped RAW document
       must decline before CSS or relayout. Preserve the exact layout owner,
       scroll, focused node, and already-rendered pixels. */
    static const char raw_shell[] =
        "<!doctype html><style>.spacer{height:700px}</style><body>"
        "<header>Account application</header><div id=app>"
        "<a id=account-link href=/account>Account</a><div class=spacer></div>"
        "<form><label>Email <input name=email></label>"
        "<button>Continue</button></form></div></body>";
    browser_engine_set_reader_candidate_mode(engine, false);
    CHECK(browser_engine_commit_html(
              engine, "https://app.example.test/login", raw_shell,
              sizeof(raw_shell) - 1u, true));
    NavigationSession *raw_navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *raw_link = test_reader_find_id(
        lxb_dom_interface_node(raw_navigation->page.document.html),
        "account-link");
    CHECK(raw_link != NULL);
    bool raw_rendered = browser_engine_render_frame(engine, NULL);
    if (!raw_rendered) {
        fprintf(stderr, "reader raw render failed: %s\n",
                browser_engine_last_error(engine));
    }
    CHECK(raw_rendered);
    CHECK(browser_engine_focus_node(engine, raw_link));
    DrawCommand *raw_commands = raw_navigation->page.layout.commands;
    size_t raw_command_count = raw_navigation->page.layout.count;
    int raw_scroll = navigation_current(raw_navigation)->scroll_y;
    lxb_dom_node_t *raw_focus = controller_focused_node(
        browser_engine_controller(engine));
    size_t raw_pixels = 0;
    const uint16_t *raw_frame = browser_engine_framebuffer(
        engine, &raw_pixels);
    uint64_t raw_checksum = frame_checksum(raw_frame, raw_pixels);
    ReaderDocumentAnalysis raw_reader = {0};
    CHECK(!browser_engine_prepare_reader(engine, &raw_reader)
          && raw_reader.prepared && raw_reader.kind == READER_PAGE_RAW
          && raw_navigation->page.layout.commands == raw_commands
          && raw_navigation->page.layout.count == raw_command_count
          && navigation_current(raw_navigation)->scroll_y == raw_scroll
          && controller_focused_node(browser_engine_controller(engine))
                 == raw_focus
          && browser_engine_framebuffer(engine, NULL) == raw_frame
          && frame_checksum(raw_frame, raw_pixels) == raw_checksum);

    /* A media page with a related rail makes its primary root both the
       preserved article and an ancestor of the listing. Path pruning must
       never hide the player inside that preserved root. */
    static const char media_html[] =
        "<!doctype html><head><meta property=og:type content=video.other>"
        "<script type=application/ld+json>{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/primary.mp4\"}</script></head><body>"
        "<main><div><video controls poster=/poster.png "
        "src=/primary.mp4></video></div>"
        "<h1>Primary media</h1><section>"
        "<article><a href=/video-0><img src=/0.png alt=Zero></a><span>1:20 10 views</span></article>"
        "<article><a href=/video-1><img src=/1.png alt=One></a><span>1:21 10 views</span></article>"
        "<article><a href=/video-2><img src=/2.png alt=Two></a><span>1:22 10 views</span></article>"
        "<article><a href=/video-3><img src=/3.png alt=Three></a><span>1:23 10 views</span></article>"
        "<article><a href=/video-4><img src=/4.png alt=Four></a><span>1:24 10 views</span></article>"
        "<article><a href=/video-5><img src=/5.png alt=Five></a><span>1:25 10 views</span></article>"
        "<article><a href=/video-6><img src=/6.png alt=Six></a><span>1:26 10 views</span></article>"
        "<article><a href=/video-7><img src=/7.png alt=Seven></a><span>1:27 10 views</span></article>"
        "</section></main></body>";
    browser_engine_set_reader_candidate_mode(engine, true);
    CHECK(browser_engine_set_user_css(engine, css, strlen(css))
          && browser_engine_commit_html(
                 engine, "https://media.example.test/watch", media_html,
                 sizeof(media_html) - 1u, true));
    ReaderDocumentAnalysis media_reader = {0};
    CHECK(browser_engine_reader_analysis(engine, &media_reader)
          && media_reader.kind == READER_PAGE_WATCH
          && media_reader.listing_entries == 8u);
    lxb_dom_node_t *media_node = NULL;
    for (size_t index = 0;
         index < browser_engine_navigation(engine)->page.layout.control_count;
         index++) {
        lxb_dom_node_t *node = browser_engine_navigation(
            engine)->page.layout.controls[index].node;
        size_t name_length = 0;
        const char *name = document_element_name(node, &name_length);
        if (name != NULL && name_length == 5u
            && memcmp(name, "video", 5u) == 0) {
            media_node = node;
            break;
        }
    }
    ControllerAction media_action = {0};
    CHECK(media_node != NULL
          && browser_engine_focus_node(engine, media_node)
          && browser_engine_activate(engine, &media_action)
          && media_action.type == CONTROLLER_ACTION_MEDIA
          && !media_action.media_audio_only
          && strcmp(media_action.url,
                    "https://media.example.test/primary.mp4") == 0);

    static const char audio_html[] =
        "<!doctype html><head><script type=application/ld+json>"
        "{\"@type\":\"MusicRecording\",\"name\":\"Field notes\","
        "\"audio\":{\"@type\":\"AudioObject\","
        "\"contentUrl\":\"/episode.m4a\"}}</script></head><body>"
        "<main><h1>Field notes</h1>"
        "<button aria-label='Play Field notes'>Play episode</button>"
        "<p>Episode notes remain readable.</p></main></body>";
    browser_engine_set_reader_candidate_mode(engine, true);
    CHECK(browser_engine_set_user_css(engine, css, strlen(css))
          && browser_engine_commit_html(
                 engine, "https://media.example.test/episode", audio_html,
                 sizeof(audio_html) - 1u, true));
    ReaderDocumentAnalysis audio_reader = {0};
    CHECK(browser_engine_reader_analysis(engine, &audio_reader)
          && audio_reader.kind == READER_PAGE_RAW
          && browser_engine_navigation(engine)->page.layout.control_count
                 == 1u);
    lxb_dom_node_t *audio_button = browser_engine_navigation(
        engine)->page.layout.controls[0].node;
    ControllerAction audio_action = {0};
    CHECK(browser_engine_focus_node(engine, audio_button)
          && browser_engine_activate(engine, &audio_action)
          && audio_action.type == CONTROLLER_ACTION_MEDIA
          && audio_action.media_audio_only
          && strcmp(audio_action.url,
                    "https://media.example.test/episode.m4a") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_blank_reader_frontend_recovery(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 4u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    browser_engine_test_set_recovery_time_us(UINT64_MAX);

#define RECOVERY_BODY_PROSE                                               \
    "<p>This first complete paragraph explains the server-rendered report " \
    "and gives the small-screen reader a coherent introduction.</p>"     \
    "<p>The second paragraph preserves useful evidence even when optional " \
    "application hydration cannot complete inside its device budget.</p>" \
    "<p>A third paragraph records practical details, relevant context, and " \
    "a clear result without depending on author script execution.</p>"   \
    "<p>The fourth paragraph keeps navigation separate from the article "  \
    "while retaining stable semantic text in source document order.</p>" \
    "<p>Further explanation makes the bounded semantic extraction useful " \
    "on its own rather than exposing an empty application shell.</p>"     \
    "<p>The closing paragraph summarizes the outcome and provides a "      \
    "predictable next step for readers on a compact display.</p>"
#define RECOVERY_PROSE                                                    \
    "<h1>Bounded trust report</h1>" RECOVERY_BODY_PROSE

    /* Authored hidden state alone is not degradation evidence. The cheap
       refusal must happen before Reader preparation mutates the DOM. */
    static const char healthy_hidden[] =
        "<!doctype html><style>body{display:none}</style><body><main>"
        RECOVERY_PROSE "</main></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://healthy-hidden.test/", healthy_hidden,
              sizeof(healthy_hidden) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    ReaderDocumentAnalysis analysis = {0};
    CHECK(navigation != NULL && navigation->page.layout.link_count == 0u
          && navigation->page.layout.control_count == 0u
          && browser_engine_page_is_visually_blank(engine)
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_NONE
          && !analysis.prepared);

    /* A blocked optional fetch rejects its promise, but that gauge is not
       hard evidence that page hydration failed. Even after more than the old
       twelve-tick threshold, automatic recovery must leave the opacity shell
       to its scheduled reveal. */
    static const char blocked_delayed_reveal[] =
        "<!doctype html><style>body{opacity:0}</style><body><article>"
        RECOVERY_PROSE "</article><script>"
        "setTimeout(()=>document.body.style.opacity='1',100);"
        "fetch('https://doubleclick.net/optional-widget')"
        "</script></body>";
    CHECK(browser_engine_content_blocker_configure(
              engine, CONTENT_BLOCKER_BASIC, NULL)
          && browser_engine_commit_html(
              engine, "https://blocked-reveal.test/",
              blocked_delayed_reveal,
              sizeof(blocked_delayed_reveal) - 1u, true));
    for (size_t tick = 0; tick < 16u; tick++) {
        CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
        analysis = (ReaderDocumentAnalysis) {0};
        CHECK(browser_engine_prepare_blank_reader_recovery(engine, &analysis)
                  == BROWSER_BLANK_READER_RECOVERY_NONE
              && !analysis.prepared);
    }
    navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL
          && navigation->page.script_result.promise_rejections != 0u
          && browser_engine_content_blocker_configure(
                 engine, CONTENT_BLOCKER_OFF, NULL));

    /* Commit-time Reader candidates remain private while delayed author work
       settles. A public lookalike appended in that interval must never become
       the native root; activation connects only the exact pre-author tree. */
    static const char deferred_reader_lookalike[] =
        "<!doctype html><style>article{opacity:0}</style><body><article>"
        RECOVERY_PROSE "</article><script>"
        "setTimeout(()=>{let fake=document.createElement('main');"
        "fake.id='author-reader-fake';"
        "fake.hidden=true;"
        "fake.setAttribute('data-tilefinch-reader-root','article');"
        "fake.textContent='AUTHOR LOOKALIKE';document.body.appendChild(fake)},0);"
        "throw new Error('hydration failed')</script></body>";
    browser_engine_set_reader_candidate_mode(engine, true);
    static const char staged_reader_css[] =
        "body[data-tilefinch-reader-kind=article]>"
        "[data-tilefinch-reader-root=article]{display:block!important}";
    CHECK(browser_engine_set_user_css(
              engine, staged_reader_css, sizeof(staged_reader_css) - 1u)
          && browser_engine_commit_html(
              engine, "https://reader-provenance.test/",
              deferred_reader_lookalike,
              sizeof(deferred_reader_lookalike) - 1u, true));
    navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *native_reader_root = navigation->page.reader_root;
    CHECK(navigation->page.reader_analysis.prepared
          && navigation->page.reader_analysis.kind == READER_PAGE_ARTICLE
          && native_reader_root != NULL && native_reader_root->parent == NULL
          && navigation->page.reader_root_handle != 0
          && test_reader_root_node(&navigation->page.document) == NULL);
    CHECK(navigation->page.script_result.pending_tasks != 0u
          && browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    lxb_dom_node_t *author_fake = actionable_find_id(
        &navigation->page.document, "author-reader-fake");
    CHECK(author_fake != NULL && author_fake != native_reader_root);
    navigation_retire_current_page_scripts(navigation);
    CHECK(navigation->page.runtime == NULL
          && navigation->page.reader_root == native_reader_root
          && navigation->page.reader_root_handle == 0
          && native_reader_root->parent == NULL);
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(browser_engine_prepare_reader(engine, &analysis)
          && navigation->page.reader_root == native_reader_root
          && native_reader_root->parent
                 == document_body_node(&navigation->page.document)
          && author_fake != navigation->page.reader_root);
    browser_engine_set_reader_candidate_mode(engine, false);
    CHECK(browser_engine_set_user_css(engine, "", 0u));

    /* A full-viewport gradient can share the resolved page-background base
       color while still being the page's entire visible presentation. */
    static const char gradient_only[] =
        "<!doctype html><style>html,body{margin:0;width:100%;height:100%;"
        "background:#fff}body{background-image:linear-gradient(#fff,#ccddee)}"
        "</style><body></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://gradient-only.test/", gradient_only,
              sizeof(gradient_only) - 1u, true)
          && !browser_engine_page_is_visually_blank(engine));

    static const char opacity_hidden_failed[] =
        "<!doctype html><style>article{opacity:0}</style><body><article>"
        RECOVERY_BODY_PROSE
        "<p><a href=/opacity-evidence>Retained evidence link</a></p>"
        "</article><script>throw new Error('bundle failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://opacity-hidden.test/", opacity_hidden_failed,
              sizeof(opacity_hidden_failed) - 1u, true));
    navigation = browser_engine_navigation(engine);
    browser_engine_test_set_recovery_time_us(
        navigation->page.committed_us);
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(browser_engine_page_is_visually_blank(engine)
          && browser_engine_prepare_blank_reader_recovery(engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_DEFERRED
          && !analysis.prepared);
    browser_engine_test_set_recovery_time_us(UINT64_MAX);
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    analysis = (ReaderDocumentAnalysis) {0};
    navigation = browser_engine_navigation(engine);
    bool opacity_link_blank = browser_engine_page_is_visually_blank(engine);
    BrowserBlankReaderRecovery opacity_link_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (!(navigation->page.layout.link_count != 0u && opacity_link_blank
          && opacity_link_recovery == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE)) {
        fprintf(stderr,
                "opacity-link links=%zu blank=%d recovery=%d prepared=%d "
                "kind=%d commands=%zu first-opacity=%u\n",
                navigation->page.layout.link_count, opacity_link_blank,
                (int) opacity_link_recovery, analysis.prepared,
                (int) analysis.kind, navigation->page.layout.count,
                navigation->page.layout.count == 0u ? 0u
                  : navigation->page.layout.commands[0].opacity_scale);
    }
    CHECK(navigation->page.layout.link_count != 0u && opacity_link_blank
          && opacity_link_recovery == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE);

    static const char visibility_hidden_failed[] =
        "<!doctype html><style>article{visibility:hidden}</style>"
        "<body><article>" RECOVERY_BODY_PROSE
        "<p><a href=/visibility-evidence>Retained evidence link</a></p>"
        "</article><script>throw new Error('bundle failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://visibility-hidden.test/",
              visibility_hidden_failed,
              sizeof(visibility_hidden_failed) - 1u, true));
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    analysis = (ReaderDocumentAnalysis) {0};
    navigation = browser_engine_navigation(engine);
    bool hidden_link_geometry = navigation->page.layout.link_count != 0u;
    for (size_t link = 0u; link < navigation->page.layout.link_count; link++) {
        if (navigation->page.layout.links[link].width != 0
            || navigation->page.layout.links[link].height != 0) {
            hidden_link_geometry = false;
        }
    }
    bool visibility_link_blank = browser_engine_page_is_visually_blank(engine);
    BrowserBlankReaderRecovery visibility_link_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (!(hidden_link_geometry && visibility_link_blank
          && visibility_link_recovery
                 == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE)) {
        fprintf(stderr,
                "visibility-link links=%zu hidden=%d blank=%d recovery=%d "
                "prepared=%d kind=%d first=%d,%d commands=%zu opacity=%u\n",
                navigation->page.layout.link_count, hidden_link_geometry,
                visibility_link_blank, (int) visibility_link_recovery,
                analysis.prepared, (int) analysis.kind,
                navigation->page.layout.link_count == 0u ? -1
                  : navigation->page.layout.links[0].width,
                navigation->page.layout.link_count == 0u ? -1
                  : navigation->page.layout.links[0].height,
                navigation->page.layout.count,
                navigation->page.layout.count == 0u ? 0u
                  : navigation->page.layout.commands[0].opacity_scale);
    }
    CHECK(hidden_link_geometry && visibility_link_blank
          && visibility_link_recovery
                 == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE);

    /* A failed login/application shell is genuinely degraded and blank but
       has no extracted semantic Reader tree, so it remains unavailable. */
    static const char failed_form_shell[] =
        "<!doctype html><style>body{display:none}</style><body>"
        "<div id=app><form><label>Account <input name=account></label>"
        "<button>Continue</button></form></div>"
        "<script>throw new Error('hydration failed')</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://failed-form.test/", failed_form_shell,
              sizeof(failed_form_shell) - 1u, true));
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    navigation = browser_engine_navigation(engine);
    analysis = (ReaderDocumentAnalysis) {0};
    BrowserBlankReaderRecovery form_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (!(navigation->page.script_degradation_observed
          && navigation->page.layout.link_count == 0u
          && navigation->page.layout.control_count == 0u
          && form_recovery == BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_RAW)) {
        fprintf(stderr,
                "blank form recovery scripts-failed=%zu degraded=%d "
                "commands=%zu links=%zu controls=%zu status=%d "
                "prepared=%d kind=%d confidence=%d bounded=%d pending=%zu\n",
                navigation->page.script_result.scripts_failed,
                navigation->page.script_degradation_observed,
                navigation->page.layout.count,
                navigation->page.layout.link_count,
                navigation->page.layout.control_count, (int) form_recovery,
                analysis.prepared, (int) analysis.kind,
                analysis.high_confidence, analysis.bounded_out,
                navigation->page.script_result.pending_tasks);
    }
    CHECK(navigation->page.script_degradation_observed
          && navigation->page.layout.link_count == 0u
          && navigation->page.layout.control_count == 0u
          && form_recovery == BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_RAW);

    /* NavigationSession's aggregate counters retain diagnostic history. A
       subsequent clean document must use its page-local degradation fact. */
    CHECK(browser_engine_commit_html(
              engine, "https://clean-after-failure.test/", healthy_hidden,
              sizeof(healthy_hidden) - 1u, true));
    navigation = browser_engine_navigation(engine);
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(!navigation->page.script_degradation_observed
          && navigation->page.layout.count == 0u
          && !navigation->page.stylesheet.static_custom_element_fallback
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_NONE
          && !analysis.prepared);

    /* A semantic prefix is useful for a manual Reader request but must not
       become an automatic failure surface when the bounded extractor could
       not inspect or retain the complete document. */
    static const char bounded_prefix[] =
        "<!doctype html><style>.staged{transform:scale(0)}</style><body>"
        "<article class=staged><h1>Oversized report</h1>";
    static const char bounded_unit[] =
        "<p>Bounded semantic paragraph with useful retained evidence.</p>";
    static const char bounded_suffix[] =
        "</article><script>throw new Error('bundle failed')</script>"
        "</body>";
    const size_t bounded_units = 600u;
    size_t bounded_length = sizeof(bounded_prefix) - 1u
        + bounded_units * (sizeof(bounded_unit) - 1u)
        + sizeof(bounded_suffix) - 1u;
    char *bounded_html = malloc(bounded_length + 1u);
    CHECK(bounded_html != NULL);
    size_t bounded_at = 0u;
    memcpy(bounded_html + bounded_at, bounded_prefix,
           sizeof(bounded_prefix) - 1u);
    bounded_at += sizeof(bounded_prefix) - 1u;
    for (size_t unit = 0; unit < bounded_units; unit++) {
        memcpy(bounded_html + bounded_at, bounded_unit,
               sizeof(bounded_unit) - 1u);
        bounded_at += sizeof(bounded_unit) - 1u;
    }
    memcpy(bounded_html + bounded_at, bounded_suffix,
           sizeof(bounded_suffix));
    CHECK(bounded_at + sizeof(bounded_suffix) - 1u == bounded_length
          && browser_engine_commit_html(
              engine, "https://bounded-reader.test/", bounded_html,
              bounded_length, true));
    free(bounded_html);
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    navigation = browser_engine_navigation(engine);
    PocDocument *bounded_document = &navigation->page.document;
    lxb_dom_node_t *bounded_body = document_body_node(bounded_document);
    size_t bounded_marker_length = 0u;
    CHECK(bounded_body != NULL
          && document_attribute(
                 bounded_body, "data-tilefinch-reader-kind",
                 &bounded_marker_length) == NULL
          && test_reader_root_node(bounded_document) == NULL);
    const size_t bounded_nodes_before = bounded_document->node_count;
    const size_t bounded_elements_before = bounded_document->element_count;
    const size_t bounded_attributes_before = bounded_document->attribute_count;
    const size_t bounded_text_before = bounded_document->text_bytes;
    const DrawCommand *bounded_commands_before =
        navigation->page.layout.commands;
    const size_t bounded_command_count_before =
        navigation->page.layout.count;
    const NavigationEntry *bounded_entry = navigation_current(navigation);
    const int bounded_scroll_before = bounded_entry == NULL
        ? 0 : bounded_entry->scroll_y;
    BrowserController *bounded_controller =
        browser_engine_controller(engine);
    const ControllerFocusKind bounded_focus_before =
        bounded_controller == NULL ? CONTROLLER_FOCUS_NONE
                                   : bounded_controller->focus_kind;
    const size_t bounded_focus_index_before = bounded_controller == NULL
        ? 0u : bounded_controller->focus_index;
    Budget *bounded_budget = browser_engine_budget(engine);
    const size_t bounded_budget_before = bounded_budget == NULL
        ? 0u : bounded_budget->current;
    analysis = (ReaderDocumentAnalysis) {0};
    BrowserBlankReaderRecovery bounded_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (!(navigation->page.script_degradation_observed
          && bounded_recovery == BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE
          && analysis.bounded_out && analysis.extraction_truncated)) {
        fprintf(stderr,
                "bounded recovery degraded=%d commands=%zu status=%d "
                "prepared=%d kind=%d confidence=%d bounded=%d truncated=%d "
                "visible=%u extracted=%u\n",
                navigation->page.script_degradation_observed,
                navigation->page.layout.count, (int) bounded_recovery,
                analysis.prepared, (int) analysis.kind,
                analysis.high_confidence, analysis.bounded_out,
                analysis.extraction_truncated, analysis.visible_text_bytes,
                analysis.extracted_bytes);
    }
    if (!(document_attribute(
              bounded_body, "data-tilefinch-reader-kind",
              &bounded_marker_length) == NULL
          && test_reader_root_node(bounded_document) == NULL
          && bounded_document->node_count == bounded_nodes_before
          && bounded_document->element_count == bounded_elements_before
          && bounded_document->attribute_count == bounded_attributes_before
          && bounded_document->text_bytes == bounded_text_before
          && navigation->page.layout.commands == bounded_commands_before
          && navigation->page.layout.count == bounded_command_count_before
          && (navigation_current(navigation) == NULL
                ? 0 : navigation_current(navigation)->scroll_y)
                 == bounded_scroll_before
          && (bounded_controller == NULL
                ? CONTROLLER_FOCUS_NONE : bounded_controller->focus_kind)
                 == bounded_focus_before
          && (bounded_controller == NULL
                ? 0u : bounded_controller->focus_index)
                 == bounded_focus_index_before
          && bounded_budget != NULL
          && bounded_budget->current == bounded_budget_before)) {
        fprintf(stderr,
                "bounded rollback marker=%s root=%p nodes=%zu/%zu "
                "elements=%zu/%zu attrs=%zu/%zu text=%zu/%zu "
                "commands=%p/%p count=%zu/%zu scroll=%d/%d "
                "focus=%d/%d index=%zu/%zu budget=%zu/%zu\n",
                document_attribute(
                    bounded_body, "data-tilefinch-reader-kind",
                    &bounded_marker_length) == NULL ? "none" : "present",
                (void *) test_reader_root_node(bounded_document),
                bounded_document->node_count, bounded_nodes_before,
                bounded_document->element_count, bounded_elements_before,
                bounded_document->attribute_count, bounded_attributes_before,
                bounded_document->text_bytes, bounded_text_before,
                (void *) navigation->page.layout.commands,
                (void *) bounded_commands_before,
                navigation->page.layout.count, bounded_command_count_before,
                navigation_current(navigation) == NULL
                    ? 0 : navigation_current(navigation)->scroll_y,
                bounded_scroll_before,
                bounded_controller == NULL
                    ? (int) CONTROLLER_FOCUS_NONE
                    : (int) bounded_controller->focus_kind,
                (int) bounded_focus_before,
                bounded_controller == NULL
                    ? 0u : bounded_controller->focus_index,
                bounded_focus_index_before,
                bounded_budget == NULL ? 0u : bounded_budget->current,
                bounded_budget_before);
    }
    CHECK(navigation->page.script_degradation_observed
          && bounded_recovery == BROWSER_BLANK_READER_RECOVERY_UNAVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE
          && analysis.bounded_out && analysis.extraction_truncated
          && document_attribute(
                 bounded_body, "data-tilefinch-reader-kind",
                 &bounded_marker_length) == NULL
          && test_reader_root_node(bounded_document) == NULL
          && bounded_document->node_count == bounded_nodes_before
          && bounded_document->element_count == bounded_elements_before
          && bounded_document->attribute_count == bounded_attributes_before
          && bounded_document->text_bytes == bounded_text_before
          && navigation->page.layout.commands == bounded_commands_before
          && navigation->page.layout.count == bounded_command_count_before
          && (navigation_current(navigation) == NULL
                ? 0 : navigation_current(navigation)->scroll_y)
                 == bounded_scroll_before
          && (bounded_controller == NULL
                ? CONTROLLER_FOCUS_NONE : bounded_controller->focus_kind)
                 == bounded_focus_before
          && (bounded_controller == NULL
                ? 0u : bounded_controller->focus_index)
                 == bounded_focus_index_before
          && bounded_budget != NULL
          && bounded_budget->current == bounded_budget_before);
    ReaderDocumentAnalysis bounded_manual = {0};
    CHECK(browser_engine_prepare_reader(engine, &bounded_manual)
          && bounded_manual.prepared && bounded_manual.bounded_out
          && bounded_manual.extraction_truncated
          && test_reader_root_node(bounded_document) != NULL);

    /* An unrelated author rejection is degradation evidence, but a live
       timer still owns the first chance to reveal the server-rendered body.
       Recovery must not install its semantic clone ahead of that task. */
    static const char delayed_reveal[] =
        "<!doctype html><style>.staged{display:none}</style>"
        "<body class=staged><article>" RECOVERY_PROSE "</article>"
        "<script>Promise.reject(new Error('optional widget failed'));"
        "setTimeout(()=>document.body.className='',1)</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://delayed-reveal.test/", delayed_reveal,
              sizeof(delayed_reveal) - 1u, true));
    navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *delayed_body =
        document_body_node(&navigation->page.document);
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(browser_engine_page_is_visually_blank(engine)
          && navigation->page.script_result.pending_tasks != 0u
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_NONE
          && !analysis.prepared
          && test_reader_root_node(&navigation->page.document) == NULL
          && document_attribute(
                 delayed_body, "data-tilefinch-reader-kind",
                 &bounded_marker_length) == NULL);
    bool delayed_layout_changed = false;
    CHECK(browser_engine_advance_runtime(
              engine, 2u, 4u, &delayed_layout_changed)
          && delayed_layout_changed
          && !browser_engine_page_is_visually_blank(engine)
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_NONE
          && test_reader_root_node(&navigation->page.document) == NULL
          && document_attribute(
                 delayed_body, "data-tilefinch-reader-kind",
                 &bounded_marker_length) == NULL);

    /* If the last author task settles without revealing pixels, retrying the
       same generation may then admit the complete semantic Reader tree. */
    static const char delayed_still_blank[] =
        "<!doctype html><style>article{visibility:hidden}</style>"
        "<body><article>" RECOVERY_BODY_PROSE
        "<p><a href=/settled-evidence>Retained evidence link</a></p>"
        "</article>"
        "<script>setTimeout(()=>document.querySelector('article')"
        ".setAttribute('data-settled','1'),1);"
        "throw new Error('bundle failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://delayed-blank.test/", delayed_still_blank,
              sizeof(delayed_still_blank) - 1u, true));
    navigation = browser_engine_navigation(engine);
    analysis = (ReaderDocumentAnalysis) {0};
    BrowserBlankReaderRecovery delayed_blank_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (delayed_blank_recovery != BROWSER_BLANK_READER_RECOVERY_DEFERRED) {
        fprintf(stderr,
                "delayed blank recovery=%d blank=%d degraded=%d pending=%zu "
                "runtime=%p prepared=%d kind=%d\n",
                (int) delayed_blank_recovery,
                browser_engine_page_is_visually_blank(engine),
                navigation->page.script_degradation_observed,
                navigation->page.script_result.pending_tasks,
                (void *) navigation->page.runtime,
                analysis.prepared, (int) analysis.kind);
    }
    CHECK(delayed_blank_recovery == BROWSER_BLANK_READER_RECOVERY_DEFERRED);
    CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
    BrowserBlankReaderRecovery settled_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    if (settled_recovery == BROWSER_BLANK_READER_RECOVERY_DEFERRED) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        settled_recovery = browser_engine_prepare_blank_reader_recovery(
            engine, &analysis);
    }
    CHECK(settled_recovery == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind != READER_PAGE_RAW);

    /* A recurring timer has no natural quiescent point. Delayed hydration
       keeps the first bounded window, then the same committed generation
       must expose recovery instead of deferring forever. */
    static const char recurring_still_blank[] =
        "<!doctype html><style>article{visibility:hidden}</style>"
        "<body><article>" RECOVERY_BODY_PROSE
        "<p><a href=/recurring-evidence>Retained evidence link</a></p>"
        "</article>"
        "<script>setInterval(()=>document.body.dataset.heartbeat='1',1);"
        "throw new Error('bundle failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://recurring-blank.test/",
              recurring_still_blank, sizeof(recurring_still_blank) - 1u,
              true));
    navigation = browser_engine_navigation(engine);
    analysis = (ReaderDocumentAnalysis) {0};
    BrowserBlankReaderRecovery recurring_recovery =
        browser_engine_prepare_blank_reader_recovery(engine, &analysis);
    CHECK(recurring_recovery == BROWSER_BLANK_READER_RECOVERY_DEFERRED);
    for (size_t turn = 0;
         turn < 16u
             && recurring_recovery == BROWSER_BLANK_READER_RECOVERY_DEFERRED;
         turn++) {
        CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
        recurring_recovery = browser_engine_prepare_blank_reader_recovery(
            engine, &analysis);
    }
    CHECK(recurring_recovery == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && script_runtime_has_pending_author_work(
                 navigation->page.runtime)
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE);

    /* Script failure never overrides already useful author pixels. */
    static const char visible_failed[] =
        "<!doctype html><body><main>" RECOVERY_PROSE
        "<a href=/visible-evidence>Read visible evidence</a>"
        "</main><script>throw new Error('late enhancement failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://visible-failed.test/", visible_failed,
              sizeof(visible_failed) - 1u, true));
    navigation = browser_engine_navigation(engine);
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(navigation->page.script_degradation_observed
          && navigation->page.layout.count != 0u
          && navigation->page.layout.link_count != 0u
          && !browser_engine_page_is_visually_blank(engine)
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_NONE
          && !analysis.prepared);

    /* A main-root article is deliberately below optional auto-Reader's high
       confidence threshold. Proven blank output plus a real script failure
       still admits its independently useful extracted semantic tree. */
    static const char degraded_article[] =
        "<!doctype html><style>.staged{transform:scale(0)}</style><body>"
        "<main class=staged>" RECOVERY_PROSE
        "<a href=/details>Read details</a></main>"
        "<script>throw new Error('bundle initialization failed')"
        "</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://degraded-article.test/", degraded_article,
              sizeof(degraded_article) - 1u, true));
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    navigation = browser_engine_navigation(engine);
    char cookie_css[CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    size_t cookie_css_length = 0u;
    CHECK(content_blocker_cookie_banner_css(
              cookie_css, sizeof(cookie_css), &cookie_css_length)
          && browser_engine_apply_user_css(
                 engine, cookie_css, cookie_css_length)
          && navigation->page.layout.count == 0u);
    CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
    analysis = (ReaderDocumentAnalysis) {0};
    CHECK(navigation->page.script_degradation_observed
          && navigation->page.layout.link_count == 0u
          && navigation->page.layout.control_count == 0u
          && browser_engine_prepare_blank_reader_recovery(
                 engine, &analysis)
                 == BROWSER_BLANK_READER_RECOVERY_AVAILABLE
          && analysis.prepared && analysis.kind == READER_PAGE_ARTICLE
          && !analysis.high_confidence);
    char css[SITE_ADAPTER_READER_CSS_LIMIT
             + CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://degraded-article.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, SITE_ADAPTER_READER_CSS_LIMIT,
              adapter, sizeof(adapter)));
    size_t css_length = strlen(css);
    CHECK(cookie_css_length
              <= sizeof(css) - css_length - 1u);
    memcpy(css + css_length, cookie_css, cookie_css_length + 1u);
    css_length += cookie_css_length;
    CHECK(browser_engine_apply_user_css(engine, css, css_length));
    navigation = browser_engine_navigation(engine);
    bool recovered_details_link = false;
    for (size_t link = 0; link < navigation->page.layout.link_count; link++) {
        if (navigation->page.layout.links[link].url != NULL
            && strstr(navigation->page.layout.links[link].url,
                      "/details") != NULL) {
            recovered_details_link = true;
            break;
        }
    }
    CHECK(navigation->page.layout.count != 0u
          && recovered_details_link
          && browser_engine_render_frame(engine, NULL));

#undef RECOVERY_PROSE
#undef RECOVERY_BODY_PROSE
    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_test_set_recovery_time_us(0u);
    browser_engine_destroy(engine);
    return 0;
}

static lxb_dom_node_t *test_declared_media_card_node(PocDocument *document)
{
    if (document == NULL || document->html == NULL) return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    size_t visited = 0u;
    for (lxb_dom_node_t *node = root;
         node != NULL && visited++ < 16384u;) {
        size_t length = 0u;
        if (document_attribute(
                node, "data-tilefinch-declared-media-card", &length)
            != NULL) return node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        node = node == NULL || node == root ? NULL : node->next;
    }
    return NULL;
}

static int test_declared_media_card_recovery(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 4u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    browser_engine_test_set_recovery_time_us(UINT64_MAX);

    static const char healthy[] =
        "<!doctype html><body><img width=320 height=180 src=/poster.jpg>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\",\"contentUrl\":\"/movie-240.mp4\","
        "\"thumbnailUrl\":\"/poster.jpg\"}</script><script>0</script>";
    CHECK(browser_engine_commit_html(
              engine, "https://card.example.test/watch", healthy,
              sizeof(healthy) - 1u, true)
          && browser_engine_prepare_declared_media_card(engine)
                 == BROWSER_DECLARED_MEDIA_CARD_NONE
          && media_declared_video_discovery_count(
                 &browser_engine_navigation(engine)->page.document) == 0u
          && test_declared_media_card_node(
                 &browser_engine_navigation(engine)->page.document) == NULL);

    static const char exact[] =
        "<!doctype html><style>img{display:block;width:320px;height:180px}"
        "</style><body><img id=poster src='/poster.jpg?exact=1'>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\",\"name\":\"Fixture video\","
        "\"duration\":\"PT1M\",\"thumbnailUrl\":"
        "\"/poster.jpg?exact=1\",\"contentUrl\":\"/movie-240.mp4\"}"
        "</script><script>document.body.addEventListener('click',function(e){"
        "e.preventDefault()});throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/watch", exact,
        sizeof(exact) - 1u, true));
    BrowserDeclaredMediaCard card =
        browser_engine_prepare_declared_media_card(engine);
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED
          && media_declared_video_discovery_count(
                 &browser_engine_navigation(engine)->page.document) == 0u);
    for (size_t tick = 0u;
         card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        card = browser_engine_prepare_declared_media_card(engine);
        if (card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED) {
            CHECK(media_declared_video_discovery_count(
                      &browser_engine_navigation(engine)->page.document)
                  == 0u);
        }
    }
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *node = test_declared_media_card_node(
        &navigation->page.document);
    size_t marker_length = 0u;
    const char *marker = node == NULL ? NULL : document_attribute(
        node, "data-tilefinch-declared-media-card", &marker_length);
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_INSTALLED
          && media_declared_video_discovery_count(
                 &navigation->page.document) == 1u
          && marker != NULL && marker_length == sizeof("thumbnail") - 1u
          && memcmp(marker, "thumbnail", marker_length) == 0);
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, node)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_MEDIA
          && action.media_node_handle == 0
          && strcmp(action.url,
                    "https://card.example.test/movie-240.mp4") == 0);
    BrowserFindSnapshot find = {0};
    CHECK(browser_engine_find_begin(engine, "Fixture video", &find)
          && find.match_count == 0u);
    browser_engine_find_clear(engine);

    static const char near_miss[] =
        "<!doctype html><style>img{display:block;width:320px;height:180px}"
        "</style><body><img src='/poster.jpg?exact=2'>"
        "<meta property=og:video content=/movie-360.mp4>"
        "<meta property=og:image content='/poster.jpg?exact=1'>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/near", near_miss,
        sizeof(near_miss) - 1u, true));
    card = browser_engine_prepare_declared_media_card(engine);
    for (size_t tick = 0u;
         card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        card = browser_engine_prepare_declared_media_card(engine);
    }
    navigation = browser_engine_navigation(engine);
    node = test_declared_media_card_node(&navigation->page.document);
    marker = node == NULL ? NULL : document_attribute(
        node, "data-tilefinch-declared-media-card", &marker_length);
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_INSTALLED
          && node == document_body_node(&navigation->page.document)->first_child
          && marker != NULL && marker_length == sizeof("synthetic") - 1u
          && memcmp(marker, "synthetic", marker_length) == 0);

    static const char divergent[] =
        "<!doctype html><meta property=og:video content=/declared-360.mp4>"
        "<script type=application/json>"
        "{\"download\":\"/unrelated-240.mp4\"}</script>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/divergent", divergent,
        sizeof(divergent) - 1u, true));
    card = browser_engine_prepare_declared_media_card(engine);
    for (size_t tick = 0u;
         card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        card = browser_engine_prepare_declared_media_card(engine);
    }
    navigation = browser_engine_navigation(engine);
    node = test_declared_media_card_node(&navigation->page.document);
    action = (ControllerAction) {0};
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_INSTALLED
          && node != NULL
          && browser_engine_focus_node(engine, node)
          && browser_engine_activate(engine, &action)
          && strcmp(action.url,
                    "https://card.example.test/declared-360.mp4") == 0);

    static const char thumbnail_rollback[] =
        "<!doctype html><style>img{display:block;width:320px;height:180px}"
        "</style><body><img src=/rollback-poster.jpg>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\","
        "\"thumbnailUrl\":\"/rollback-poster.jpg\","
        "\"contentUrl\":\"/rollback-240.mp4\"}</script>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/thumbnail-rollback",
        thumbnail_rollback, sizeof(thumbnail_rollback) - 1u, true));
    navigation = browser_engine_navigation(engine);
    PocDocument *thumbnail_document = &navigation->page.document;
    lxb_dom_node_t *thumbnail_body = document_body_node(thumbnail_document);
    lxb_dom_node_t *thumbnail_image = thumbnail_body->first_child;
    size_t thumbnail_nodes = thumbnail_document->node_count;
    size_t thumbnail_elements = thumbnail_document->element_count;
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_DEFERRED);
    for (size_t tick = 0u; tick < 12u; tick++)
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
    navigation_test_refuse_next_same_document_relayout();
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE
          && test_declared_media_card_node(thumbnail_document) == NULL
          && thumbnail_body->first_child == thumbnail_image
          && thumbnail_image->parent == thumbnail_body
          && thumbnail_document->node_count == thumbnail_nodes
          && thumbnail_document->element_count == thumbnail_elements);

    static const char rollback[] =
        "<!doctype html><body><article id=original>Stable source</article>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/movie-240.mp4\"}</script>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/rollback", rollback,
        sizeof(rollback) - 1u, true));
    navigation = browser_engine_navigation(engine);
    PocDocument *rollback_document = &navigation->page.document;
    lxb_dom_node_t *rollback_body = document_body_node(rollback_document);
    lxb_dom_node_t *original_first = rollback_body->first_child;
    size_t original_nodes = rollback_document->node_count;
    size_t original_elements = rollback_document->element_count;
    size_t original_attributes = rollback_document->attribute_count;
    size_t original_text = rollback_document->text_bytes;
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_DEFERRED);
    for (size_t tick = 0u; tick < 12u; tick++)
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
    navigation_test_refuse_next_same_document_relayout();
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE
          && test_declared_media_card_node(rollback_document) == NULL
          && navigation->page.loaded
          && rollback_body->first_child == original_first
          && rollback_document->node_count == original_nodes
          && rollback_document->element_count == original_elements
          && rollback_document->attribute_count == original_attributes
          && rollback_document->text_bytes == original_text);

    static const char independent_settle[] =
        "<!doctype html><body><main><h1>Retained page</h1>"
        "<p>Useful server text remains available when the application bundle "
        "does not start on this device.</p></main>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/settle-240.mp4\"}</script>"
        "<script>setInterval(function(){},1000);"
        "throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/independent-settle",
        independent_settle, sizeof(independent_settle) - 1u, true));
    ReaderDocumentAnalysis settle_analysis = {0};
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_DEFERRED
          && browser_engine_prepare_basic_view_recovery(
                 engine, &settle_analysis)
                 == BROWSER_BASIC_VIEW_RECOVERY_DEFERRED);
    for (size_t tick = 0u; tick < 12u; tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        if (tick + 1u < 12u) {
            CHECK(browser_engine_prepare_declared_media_card(engine)
                      == BROWSER_DECLARED_MEDIA_CARD_DEFERRED
                  && browser_engine_prepare_basic_view_recovery(
                         engine, &settle_analysis)
                         == BROWSER_BASIC_VIEW_RECOVERY_DEFERRED);
        }
    }
    navigation_test_refuse_next_same_document_relayout();
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE
          && browser_engine_prepare_basic_view_recovery(
                 engine, &settle_analysis)
                 == BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE);

    static const char uncorroborated[] =
        "<!doctype html><body><div data-src=/lazy-240.mp4>Lazy shell</div>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/uncorroborated",
        uncorroborated, sizeof(uncorroborated) - 1u, true));
    card = browser_engine_prepare_declared_media_card(engine);
    for (size_t tick = 0u;
         card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        card = browser_engine_prepare_declared_media_card(engine);
    }
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_NONE
          && test_declared_media_card_node(
                 &browser_engine_navigation(engine)->page.document) == NULL);
    PocDocument *uncorroborated_document =
        &browser_engine_navigation(engine)->page.document;
    size_t uncorroborated_scans =
        media_declared_video_discovery_count(uncorroborated_document);
    CHECK(uncorroborated_scans == 1u
          && browser_engine_prepare_declared_media_card(engine)
                 == BROWSER_DECLARED_MEDIA_CARD_NONE
          && media_declared_video_discovery_count(uncorroborated_document)
                 == uncorroborated_scans);

    static const char spoofed_marker[] =
        "<!doctype html><body><button "
        "data-tilefinch-declared-media-card=synthetic>Author button</button>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/spoof", spoofed_marker,
        sizeof(spoofed_marker) - 1u, true));
    card = browser_engine_prepare_declared_media_card(engine);
    for (size_t tick = 0u;
         card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        card = browser_engine_prepare_declared_media_card(engine);
    }
    navigation = browser_engine_navigation(engine);
    node = document_body_node(&navigation->page.document)->first_child;
    action = (ControllerAction) {0};
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_NONE
          && !document_is_declared_video_card(
                 &navigation->page.document, node)
          && browser_engine_focus_node(engine, node)
          && browser_engine_activate(engine, &action)
          && action.type != CONTROLLER_ACTION_MEDIA);

    static const char author_video[] =
        "<!doctype html><body><video controls></video>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\",\"name\":\"Fixture title\","
        "\"duration\":\"PT1M\","
        "\"contentUrl\":\"/movie-240.mp4\"}</script>"
        "<script>throw new Error('bundle failed')</script>";
    bool rung1_committed = browser_engine_commit_html(
        engine, "https://card.example.test/rung1", author_video,
        sizeof(author_video) - 1u, true);
    BrowserDeclaredMediaCard rung1_card = rung1_committed
        ? browser_engine_prepare_declared_media_card(engine)
        : BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE;
    for (size_t tick = 0u;
         rung1_card == BROWSER_DECLARED_MEDIA_CARD_DEFERRED && tick < 16u;
         tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
        rung1_card = browser_engine_prepare_declared_media_card(engine);
    }
    CHECK(rung1_committed
          && rung1_card == BROWSER_DECLARED_MEDIA_CARD_NONE
          && test_declared_media_card_node(
                 &browser_engine_navigation(engine)->page.document) == NULL);
    navigation = browser_engine_navigation(engine);
    CHECK(test_reader_text_command(
              &navigation->page.layout, "Fixture title") != NULL
          && test_reader_text_command(
                 &navigation->page.layout, "PT1M") != NULL);
    static const char blocked_csp[] =
        "content-security-policy: media-src 'none'\n";
    navigation = browser_engine_navigation(engine);
    CHECK(tilefinch_csp_parse_response_headers(
              &navigation->page.document.content_security_policy,
              "https://card.example.test/rung1", blocked_csp,
              sizeof(blocked_csp) - 1u, false));
    action = (ControllerAction) {0};
    CHECK(!controller_build_document_media_action(
        browser_engine_controller(engine), &action));

    static const char basic_video[] =
        "<!doctype html><head><script type=application/ld+json>"
        "{\"@context\":\"https://schema.org\","
        "\"@type\":\"VideoObject\",\"name\":\"Basic fixture\","
        "\"contentUrl\":\"/basic-240.mp4\"}</script></head><body>"
        "<main><h1>Basic fixture</h1>"
        "<p>Useful retained description for the declared video.</p>"
        "<p>Another bounded paragraph remains available after scripts retire."
        "</p></main></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/basic", basic_video,
        sizeof(basic_video) - 1u, true));
    ReaderDocumentAnalysis basic_analysis = {0};
    CHECK(browser_engine_prepare_basic_view(engine, &basic_analysis)
          && basic_analysis.kind == READER_PAGE_BASIC);
    char basic_css[SITE_ADAPTER_READER_CSS_LIMIT];
    char basic_adapter[32];
    CHECK(site_adapter_reader_css(
              "https://card.example.test/basic",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              basic_css, sizeof(basic_css), basic_adapter,
              sizeof(basic_adapter))
          && browser_engine_apply_user_css(
                 engine, basic_css, strlen(basic_css))
          && browser_engine_activate_basic_view(engine));
    navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *basic_media = NULL;
    for (size_t i = 0u; i < navigation->page.layout.control_count; i++) {
        lxb_dom_node_t *candidate = navigation->page.layout.controls[i].node;
        size_t candidate_marker_length = 0u;
        if (document_attribute(
                candidate, "data-tilefinch-declared-media-card",
                &candidate_marker_length) != NULL) {
            basic_media = candidate;
            break;
        }
    }
    action = (ControllerAction) {0};
    CHECK(basic_media != NULL
          && browser_engine_focus_node(engine, basic_media)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_MEDIA
          && action.media_node_handle == 0
          && strcmp(action.url,
                    "https://card.example.test/basic-240.mp4") == 0);

    static const char shell_refusal[] =
        "<!doctype html><body><article>Stable source</article>"
        "<script type=application/ld+json>"
        "{\"@type\":\"VideoObject\","
        "\"contentUrl\":\"/shell-refusal-240.mp4\"}</script>"
        "<script>throw new Error('bundle failed')</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://card.example.test/shell-refusal", shell_refusal,
        sizeof(shell_refusal) - 1u, true));
    CHECK(browser_engine_prepare_declared_media_card(engine)
              == BROWSER_DECLARED_MEDIA_CARD_DEFERRED);
    for (size_t tick = 0u; tick < 12u; tick++) {
        CHECK(browser_engine_advance_runtime(engine, 2u, 4u, NULL));
    }
    browser_engine_test_refuse_next_render_shell_init();
    card = browser_engine_prepare_declared_media_card(engine);
    navigation = browser_engine_navigation(engine);
    CHECK(card == BROWSER_DECLARED_MEDIA_CARD_UNAVAILABLE
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && !browser_engine_render_frame(engine, 0));

    Budget *budget = browser_engine_budget(engine);
    CHECK(browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    browser_engine_test_set_recovery_time_us(0u);
    return 0;
}

static int test_basic_view_admission_and_presentation(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 4u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    browser_engine_test_set_recovery_time_us(UINT64_MAX);

    static const char page[] =
        "<!doctype html><head><base href='https://search.example.test/catalog/'>"
        "</head><body><header><h1 id=top>Catalog</h1>"
        "<a id=details-jump href='https://catalog.example.test/#details'>"
        "Details</a></header>"
        "<nav><a id=legacy-jump "
        "href='https://catalog.example.test/#legacy'>Legacy section</a></nav>"
        "<main><p>Useful server-rendered catalog content.</p>"
        "<form role=search method=get action='find'>"
        "<label for=query>Search</label>"
        "<input id=query type=search name=q>"
        "<button id=search-submit type=submit>Go</button></form>"
        "<form method=post action='/signin'><label>Account"
        "<input name=account></label><button>Sign in</button></form>"
        "<p>Catalog background paragraph one.</p>"
        "<p>Catalog background paragraph two.</p>"
        "<p>Catalog background paragraph three.</p>"
        "<p>Catalog background paragraph four.</p>"
        "<p>Catalog background paragraph five.</p>"
        "<p>Catalog background paragraph six.</p>"
        "<p>Catalog background paragraph seven.</p>"
        "<p>Catalog background paragraph eight.</p>"
        "<div id=details><h2>Details</h2><p>Item details.</p>"
        "<figure><img src='/item.png' alt=Item>"
        "<figcaption>Item caption</figcaption></figure></div>"
        "<a name=legacy></a><p>Content after the legacy anchor.</p>"
        "</main></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://catalog.example.test/", page,
        sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    size_t raw_links = navigation->page.layout.link_count;
    size_t raw_controls = navigation->page.layout.control_count;
    ReaderDocumentAnalysis basic = {0};
    CHECK(raw_links >= 2u && raw_controls >= 4u);
    CHECK(browser_engine_prepare_basic_view(engine, &basic));
    CHECK(basic.prepared && basic.kind == READER_PAGE_BASIC
          && basic.retained_forms == 1u && !basic.bounded_out);
    CHECK(browser_engine_prepare_basic_view(engine, NULL));
    CHECK(!browser_engine_prepare_reader(engine, NULL));
    lxb_dom_node_t *body = document_body_node(&navigation->page.document);
    size_t kind_length = 0u;
    const char *kind = document_attribute(
        body, "data-tilefinch-reader-kind", &kind_length);
    CHECK(kind != NULL && kind_length == 5u
          && memcmp(kind, "basic", 5u) == 0);
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://catalog.example.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && strstr(css, "reader-root=basic") != NULL
          && browser_engine_apply_user_css(engine, css, strlen(css)));
    navigation = browser_engine_navigation(engine);
    bool authored_fragment = false;
    for (size_t i = 0; i < navigation->page.layout.link_count; i++) {
        const char *url = navigation->page.layout.links[i].url;
        if (url != NULL && strstr(url, "#details") != NULL
            && strstr(url, "tilefinch-extracted-") == NULL) {
            authored_fragment = true;
        }
    }
    CHECK(navigation->page.layout.link_count >= 2u
          && navigation->page.layout.link_count <= raw_links);
    CHECK(navigation->page.layout.control_count >= 2u
          && navigation->page.layout.control_count < raw_controls);
    CHECK(authored_fragment);
    /* Basic is specifically the script-independent recovery surface. Retire
       the otherwise idle realm before exercising its actions so the test
       pins native fragment and form behavior rather than QuickJS headroom. */
    CHECK(browser_engine_set_javascript_enabled(engine, false));
    navigation = browser_engine_navigation(engine);
    size_t loads_before = navigation->loads_started;
    uint64_t generation_before = navigation->generation;
    ControllerAction basic_action = {0};
    lxb_dom_node_t *details_jump = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html),
        "tilefinch-extracted-details-jump");
    lxb_dom_node_t *legacy_jump = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html),
        "tilefinch-extracted-legacy-jump");
    CHECK(details_jump != NULL && legacy_jump != NULL
          && browser_engine_focus_node(engine, details_jump)
          && browser_engine_activate(engine, &basic_action)
          && basic_action.type == CONTROLLER_ACTION_CONTROL
          && strcmp(navigation_current(navigation)->url,
                    "https://catalog.example.test/#details") == 0
          && navigation_current(navigation)->scroll_y > 0
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);
    int details_scroll = navigation_current(navigation)->scroll_y;
    bool legacy_focused = browser_engine_focus_node(engine, legacy_jump);
    bool legacy_activated = legacy_focused
        && browser_engine_activate(engine, &basic_action);
    bool legacy_ok = legacy_activated
        && basic_action.type == CONTROLLER_ACTION_CONTROL
        && strcmp(navigation_current(navigation)->url,
                  "https://catalog.example.test/#legacy") == 0
        && navigation_current(navigation)->scroll_y >= details_scroll
        && navigation->loads_started == loads_before
        && navigation->generation == generation_before;
    if (!legacy_ok) {
        fprintf(stderr,
                "basic legacy fragment node=%p focus=%d activate=%d type=%d "
                "outcome=%d url=%s scroll=%d details=%d loads=%zu/%zu generation=%llu/%llu "
                "error=%s script-error=%s\n",
                (void *) legacy_jump, legacy_focused ? 1 : 0,
                legacy_activated ? 1 : 0, (int) basic_action.type,
                (int) basic_action.activation_outcome,
                navigation_current(navigation)->url,
                navigation_current(navigation)->scroll_y, details_scroll,
                navigation->loads_started, loads_before,
                (unsigned long long) navigation->generation,
                (unsigned long long) generation_before,
                browser_engine_last_error(engine),
                navigation->page.script_result.error);
    }
    CHECK(legacy_ok);
    /* Basic view keeps the authored form action, including <base> URL
       semantics. The extracted ID is namespaced, but submission remains
       relative to the source document's computed resource base. */
    lxb_dom_node_t *basic_submit = test_reader_find_id(
        lxb_dom_interface_node(navigation->page.document.html),
        "tilefinch-extracted-search-submit");
    basic_action = (ControllerAction) {0};
    CHECK(basic_submit != NULL
          && browser_engine_focus_node(engine, basic_submit)
          && browser_engine_activate(engine, &basic_action)
          && basic_action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(basic_action.method, "GET") == 0
          && strcmp(basic_action.url,
                    "https://search.example.test/catalog/find?q=") == 0);
    /* Switching presentation CSS back off restores the authored tree. The
       hidden clone remains bounded and reusable; it never replaces raw DOM. */
    CHECK(browser_engine_apply_user_css(engine, "", 0u));
    navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.layout.link_count == raw_links
          && navigation->page.layout.control_count == raw_controls);
    CHECK(browser_engine_set_javascript_enabled(engine, true));

    /* A useful raw page with an authored action stays raw: preparation is an
       offer only for a blank/no-target degraded result. */
    static const char degraded_with_target[] =
        "<!doctype html><body><p>Useful body.</p><a href=/next>Next</a>"
        "<script>throw new Error('widget failed')</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://action.example.test/",
              degraded_with_target, sizeof(degraded_with_target) - 1u,
              true));
    basic = (ReaderDocumentAnalysis) {0};
    CHECK(browser_engine_prepare_basic_view_recovery(engine, &basic)
              == BROWSER_BASIC_VIEW_RECOVERY_NONE
          && !basic.prepared);

    /* A failed enhancement left useful text but no rendered action. Prepare
       a hidden Basic clone without touching the current layout; presentation
       remains an explicit frontend choice. */
    static const char degraded_no_target[] =
        "<!doctype html><style>form{display:none}</style><body>"
        "<h1>Search the archive</h1><p>The retained archive introduction is "
        "already useful while its search enhancement has failed.</p>"
        "<form role=search action='/find'><label>Search"
        "<input name=q></label><button>Go</button></form>"
        "<script>throw new Error('enhancement failed')</script></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://archive.example.test/",
              degraded_no_target, sizeof(degraded_no_target) - 1u, true));
    navigation = browser_engine_navigation(engine);
    const DrawCommand *commands_before = navigation->page.layout.commands;
    const size_t command_count_before = navigation->page.layout.count;
    basic = (ReaderDocumentAnalysis) {0};
    CHECK(navigation->page.script_degradation_observed);
    CHECK(navigation->page.layout.control_count == 0u);
    BrowserBasicViewRecovery basic_recovery =
        browser_engine_prepare_basic_view_recovery(engine, &basic);
    for (size_t turn = 0;
         turn < 16u
             && basic_recovery == BROWSER_BASIC_VIEW_RECOVERY_DEFERRED;
         turn++) {
        CHECK(browser_engine_advance_runtime(engine, 1u, 4u, NULL));
        basic_recovery = browser_engine_prepare_basic_view_recovery(
            engine, &basic);
    }
    CHECK(basic_recovery == BROWSER_BASIC_VIEW_RECOVERY_AVAILABLE);
    CHECK(basic.kind == READER_PAGE_BASIC && basic.retained_forms == 1u);
    CHECK(navigation->page.layout.commands == commands_before
          && navigation->page.layout.count == command_count_before);

    static const char healthy_no_target[] =
        "<!doctype html><body><h1>Plain document</h1>"
        "<p>No action is needed on this healthy page.</p></body>";
    CHECK(browser_engine_commit_html(
              engine, "https://healthy.example.test/", healthy_no_target,
              sizeof(healthy_no_target) - 1u, true));
    basic = (ReaderDocumentAnalysis) {0};
    CHECK(browser_engine_prepare_basic_view_recovery(engine, &basic)
              == BROWSER_BASIC_VIEW_RECOVERY_NONE
          && !basic.prepared);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_test_set_recovery_time_us(0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_basic_view_native_root_provenance(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><main id=author-lookalike "
        "data-tilefinch-reader-root=basic><a href=/attacker>"
        "Authored lookalike</a></main><h1>Retained catalog</h1>"
        "<p>Useful server-rendered catalog copy remains available when its "
        "enhancement cannot finish on this device.</p>"
        "<a id=source-link href=#target>Jump to target</a>"
        "<p id=target>Target paragraph.</p><script>0</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://provenance.example.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *document_root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *author_lookalike = test_reader_find_id(
        document_root, "author-lookalike");
    ReaderDocumentAnalysis analysis = {0};
    CHECK(navigation != NULL && navigation->page.runtime != NULL
          && author_lookalike != NULL
          && browser_engine_prepare_basic_view(engine, &analysis)
          && analysis.kind == READER_PAGE_BASIC
          /* Preparation remains transactional: a frontend still has a live raw
             page to restore when its CSS/layout admission fails. */
          && navigation->page.runtime != NULL
          && navigation->page.reader_root_handle != 0
          && navigation->page.reader_root != NULL
          && navigation->page.reader_root != author_lookalike
          && navigation->page.reader_root->parent
                 == document_body_node(&navigation->page.document));
    lxb_dom_node_t *native_root = navigation->page.reader_root;
    CHECK(test_reader_find_id(
              native_root, "tilefinch-extracted-source-link") != NULL);

    /* Reusing an already-prepared Basic tree must retain the exact native root,
       not rediscover the earlier author-controlled marker. */
    ReaderDocumentAnalysis repeated = {0};
    CHECK(browser_engine_prepare_basic_view(engine, &repeated)
          && repeated.prepared && repeated.kind == READER_PAGE_BASIC
          && navigation->page.reader_root == native_root
          && navigation->page.reader_root != author_lookalike);
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://provenance.example.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_activate_basic_view(engine)
          /* Activation freezes the exact native root only after presentation
             succeeds. The authored lookalike must never become authoritative. */
          && navigation->page.runtime == NULL
          && navigation->page.reader_root_handle == 0
          && navigation->page.reader_root == native_root
          && navigation->page.reader_root != author_lookalike);
    lxb_dom_node_t *native_link = test_reader_find_id(
        native_root, "tilefinch-extracted-source-link");
    ControllerAction action = {0};
    CHECK(native_link != NULL
          && browser_engine_focus_node(engine, native_link)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL);
    const NavigationEntry *current = navigation_current(navigation);
    CHECK(current != NULL
          && strcmp(current->url,
                    "https://provenance.example.test/#target") == 0);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_basic_view_handle_capacity_is_transactional(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><h1>Bounded catalog</h1>"
        "<p>Useful server-rendered catalog copy remains available while the "
        "native node-handle table is deliberately exhausted.</p>"
        "<form role=search action=/find><label>Search<input name=q></label>"
        "<button>Go</button></form><script>0</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://capacity.example.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.runtime != NULL);
    BudgetAllocationOwner previous = document_allocation_owner_enter(
        &navigation->page.document);
    size_t admitted = 0u;
    static const lxb_char_t name[] = "i";
    while (script_runtime_node_handle_capacity_available(
               navigation->page.runtime)) {
        lxb_dom_element_t *element = lxb_dom_document_create_element(
            &navigation->page.document.html->dom_document,
            name, sizeof(name) - 1u, NULL);
        CHECK(element != NULL);
        long handle = script_runtime_node_weak_handle(
            navigation->page.runtime, lxb_dom_interface_node(element));
        CHECK(handle != 0);
        admitted++;
    }
    document_allocation_owner_leave(
        &navigation->page.document, previous);
    CHECK(admitted != 0u
          && !script_runtime_node_handle_capacity_available(
                 navigation->page.runtime));
    Budget *budget = browser_engine_budget(engine);
    size_t before = budget->current;
    lxb_dom_node_t *body = document_body_node(&navigation->page.document);
    size_t root_count_before = 0u;
    for (lxb_dom_node_t *node = body->first_child;
         node != NULL; node = node->next) {
        size_t length = 0u;
        if (document_attribute(
                node, "data-tilefinch-reader-root", &length) != NULL) {
            root_count_before++;
        }
    }
    ReaderDocumentAnalysis reader = {0};
    CHECK(!browser_engine_prepare_reader(engine, &reader)
          && reader.prepared && reader.bounded_out
          && budget->current == before
          && navigation->page.reader_root == NULL
          && navigation->page.reader_root_handle == 0
          && navigation->page.reader_root_kind == READER_PAGE_RAW);
    ReaderDocumentAnalysis analysis = {0};
    CHECK(!browser_engine_prepare_basic_view(engine, &analysis)
          && analysis.prepared && analysis.bounded_out
          && budget->current == before);
    size_t marker_length = 0u;
    CHECK(document_attribute(
              body, "data-tilefinch-reader-kind", &marker_length) == NULL);
    size_t root_count_after = 0u;
    for (lxb_dom_node_t *node = body->first_child;
         node != NULL; node = node->next) {
        size_t length = 0u;
        if (document_attribute(
                node, "data-tilefinch-reader-root", &length) != NULL) {
            root_count_after++;
        }
    }
    CHECK(root_count_after == root_count_before);

    CHECK(browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_basic_view_bypasses_author_delegated_actions(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 8u;
    config.javascript.maximum_total_bytes = 64u * 1024u;
    config.javascript.maximum_file_bytes = 32u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><h1>Delegated action catalog</h1>"
        "<a id=jump href=#target>Jump to target</a>"
        "<p id=target>Target paragraph.</p>"
        "<form role=search action=/find><label>Search"
        "<input id=query name=q></label><button id=submit>Go</button></form>"
        "<script>document.addEventListener('click',e=>{"
        "document.body.setAttribute('data-author-click','yes');"
        "e.preventDefault()});document.addEventListener('submit',e=>{"
        "document.body.setAttribute('data-author-submit','yes');"
        "e.preventDefault()})</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://delegated.example.test/", page,
                 sizeof(page) - 1u, true));
    ReaderDocumentAnalysis analysis = {0};
    CHECK(browser_engine_prepare_basic_view(engine, &analysis));
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://delegated.example.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_activate_basic_view(engine));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.runtime == NULL
          && navigation->page.reader_root_handle == 0);
    lxb_dom_node_t *document_root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *jump = test_reader_find_id(
        document_root, "tilefinch-extracted-jump");
    lxb_dom_node_t *submit = test_reader_find_id(
        document_root, "tilefinch-extracted-submit");
    ControllerAction action = {0};
    CHECK(jump != NULL && submit != NULL
          && browser_engine_focus_node(engine, jump)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL);
    lxb_dom_node_t *body = document_body_node(&navigation->page.document);
    size_t value_length = 0u;
    CHECK(document_attribute(
              body, "data-author-click", &value_length) == NULL);
    action = (ControllerAction) {0};
    CHECK(browser_engine_focus_node(engine, submit)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(action.method, "GET") == 0
          && document_attribute(
                 body, "data-author-submit", &value_length) == NULL);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_basic_view_form_safety_edges(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 4u;
    config.javascript.maximum_total_bytes = 32u * 1024u;
    config.javascript.maximum_file_bytes = 16u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><base href='https://forms.example.test/base/'>"
        "<body><h1>Search retained records</h1>"
        "<p>This useful server-rendered form remains available in the "
        "bounded native Basic presentation.</p>"
        "<form role=search method=get action='find#results'>"
        "<label>Query<input id=ro-input name=q value=locked readonly></label>"
        "<label>Memo<textarea id=ro-textarea name=memo readonly>"
        "unchanged</textarea></label>"
        "<label>Live query<input id=live-input name=live "
        "value=authored></label>"
        "<label>Live note<textarea id=live-textarea name=notes>"
        "authored note</textarea></label>"
        "<label>Scope<select id=live-select name=scope>"
        "<option id=live-first value=all selected>All</option>"
        "<option id=live-second value=recent>Recent</option>"
        "</select></label>"
        "<button id=send type=submit>Search</button></form>"
        "<script>0</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://forms.example.test/base/index", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    PocDocument *document = &navigation->page.document;
    lxb_dom_node_t *document_root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *live_input = test_reader_find_id(
        document_root, "live-input");
    lxb_dom_node_t *live_textarea = test_reader_find_id(
        document_root, "live-textarea");
    lxb_dom_node_t *live_first = test_reader_find_id(
        document_root, "live-first");
    lxb_dom_node_t *live_second = test_reader_find_id(
        document_root, "live-second");
    static const char live_input_value[] = "snow & \"ice\"";
    static const char live_textarea_value[] = "memo <today> & later";
    CHECK(live_input != NULL && live_textarea != NULL
          && live_first != NULL && live_second != NULL
          && document_control_value_set(
                 document, live_input, live_input_value,
                 sizeof(live_input_value) - 1u)
          && document_control_value_set(
                 document, live_textarea, live_textarea_value,
                 sizeof(live_textarea_value) - 1u));
    BudgetAllocationOwner previous = document_allocation_owner_enter(document);
    lxb_dom_attr_t *first_marker = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(live_first),
        (const lxb_char_t *) "data-tilefinch-option-selected",
        sizeof("data-tilefinch-option-selected") - 1u,
        (const lxb_char_t *) "false", sizeof("false") - 1u);
    lxb_dom_attr_t *second_marker = lxb_dom_element_set_attribute(
        lxb_dom_interface_element(live_second),
        (const lxb_char_t *) "data-tilefinch-option-selected",
        sizeof("data-tilefinch-option-selected") - 1u,
        (const lxb_char_t *) "true", sizeof("true") - 1u);
    document_allocation_owner_leave(document, previous);
    CHECK(first_marker != NULL && second_marker != NULL);
    ReaderDocumentAnalysis analysis = {0};
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(browser_engine_prepare_basic_view(engine, &analysis)
          && site_adapter_reader_css(
                 "https://forms.example.test/base/index",
                 SITE_ADAPTER_READER_FONT_SANS, 100u,
                 css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_activate_basic_view(engine));
    navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *root = navigation->page.reader_root;
    lxb_dom_node_t *input = test_reader_find_id(
        root, "tilefinch-extracted-ro-input");
    lxb_dom_node_t *textarea = test_reader_find_id(
        root, "tilefinch-extracted-ro-textarea");
    lxb_dom_node_t *submit = test_reader_find_id(
        root, "tilefinch-extracted-send");
    ControllerTextInputInfo info = {0};
    char value[64];
    size_t value_length = 0u;
    CHECK(input != NULL && textarea != NULL && submit != NULL
          && browser_engine_focus_node(engine, input)
          && browser_engine_text_input_info(engine, &info)
          && !info.editable && !info.multiline
          && browser_engine_text_value(
                 engine, value, sizeof(value), &value_length)
          && value_length == 6u && memcmp(value, "locked", 6u) == 0
          && !browser_engine_replace_text(engine, "changed", 7u));
    info = (ControllerTextInputInfo) {0};
    value_length = 0u;
    CHECK(browser_engine_focus_node(engine, textarea)
          && browser_engine_text_input_info(engine, &info)
          && !info.editable && info.multiline
          && browser_engine_text_value(
                 engine, value, sizeof(value), &value_length)
          && value_length == 9u && memcmp(value, "unchanged", 9u) == 0
          && !browser_engine_replace_text(engine, "changed", 7u));
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, submit)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(action.method, "GET") == 0
          && strcmp(action.url,
                    "https://forms.example.test/base/find?"
                    "q=locked&memo=unchanged&"
                    "live=snow+%26+%22ice%22&"
                    "notes=memo+%3Ctoday%3E+%26+later&"
                    "scope=recent#results") == 0);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_basic_view_multiple_select_refuses_transactionally(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><h1>Filter records</h1>"
        "<p>Choose any number of categories from this authored form.</p>"
        "<form role=search action=/find><label>Query"
        "<input name=q></label><label>Categories"
        "<select name=tag multiple><option selected>one</option>"
        "<option selected>two</option></select></label>"
        "<button>Search</button></form></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://multi.example.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    Budget *budget = browser_engine_budget(engine);
    lxb_dom_node_t *body = document_body_node(&navigation->page.document);
    size_t before = budget->current;
    ReaderDocumentAnalysis analysis = {0};
    bool prepared = browser_engine_prepare_basic_view(engine, &analysis);
    CHECK(!prepared
          && analysis.prepared && analysis.bounded_out
          && budget->current == before
          && navigation->page.reader_root == NULL
          && navigation->page.reader_root_handle == 0
          && navigation->page.reader_root_kind == READER_PAGE_RAW);
    size_t marker_length = 0u;
    CHECK(document_attribute(
              body, "data-tilefinch-reader-kind", &marker_length) == NULL);
    for (lxb_dom_node_t *node = body->first_child;
         node != NULL; node = node->next) {
        CHECK(document_attribute(
                  node, "data-tilefinch-reader-root",
                  &marker_length) == NULL);
    }
    CHECK(browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_reader_activation_freezes_exact_native_root(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.javascript.heap_limit = 2u * MIB;
    config.javascript.maximum_scripts = 4u;
    config.javascript.maximum_total_bytes = 32u * 1024u;
    config.javascript.maximum_file_bytes = 16u * 1024u;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><main id=author-lookalike "
        "data-tilefinch-reader-root=article>Authored marker</main>"
        "<article><h1>Exact native Reader root</h1>"
        "<p>A sufficiently detailed server-rendered article gives the "
        "bounded semantic extractor a stable primary subtree. The native "
        "projection remains useful even when enhancement code is retired.</p>"
        "<p>Reader keeps headings, paragraphs, and links while hiding the "
        "author source tree through its conservative presentation sheet.</p>"
        "<p>A third paragraph preserves a coherent narrative and gives the "
        "bounded classifier strong semantic evidence for this article.</p>"
        "<p>Additional supporting detail records the source context, its "
        "implications, and practical next steps on a compact display.</p>"
        "<p>The final paragraph concludes the report without relying on "
        "application chrome, hidden controls, or author navigation code.</p>"
        "<a id=source href=#target>Jump to target</a>"
        "<h2 id=target>Target section</h2>"
        "<p>The target text supplies another meaningful paragraph and pins "
        "fragment routing inside the exact extracted tree.</p></article>"
        "<script>document.addEventListener('click',e=>{"
        "document.body.setAttribute('data-author-click','yes');"
        "e.preventDefault()})</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://reader-provenance.example.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *document_root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *lookalike = test_reader_find_id(
        document_root, "author-lookalike");
    ReaderDocumentAnalysis analysis = {0};
    CHECK(lookalike != NULL && navigation->page.runtime != NULL
          && browser_engine_prepare_reader(engine, &analysis)
          && analysis.kind == READER_PAGE_ARTICLE
          && navigation->page.reader_root != NULL
          && navigation->page.reader_root != lookalike
          && navigation->page.reader_root_handle != 0);
    lxb_dom_node_t *native_root = navigation->page.reader_root;
    CHECK(native_root == document_body_node(
              &navigation->page.document)->last_child
          && browser_engine_prepare_reader(engine, NULL)
          && navigation->page.reader_root == native_root);
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://reader-provenance.example.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_activate_reader_view(engine)
          && navigation->page.runtime == NULL
          && navigation->page.reader_root_handle == 0
          && navigation->page.reader_root == native_root
          && navigation->page.reader_root != lookalike);
    lxb_dom_node_t *link = test_reader_find_id(
        native_root, "tilefinch-extracted-source");
    ControllerAction action = {0};
    CHECK(link != NULL && browser_engine_focus_node(engine, link)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL);
    lxb_dom_node_t *body = document_body_node(&navigation->page.document);
    size_t value_length = 0u;
    CHECK(document_attribute(
              body, "data-author-click", &value_length) == NULL
          /* Raw restores retained author pixels but intentionally does not
             resurrect the page realm; a reload is required for script. */
          && browser_engine_apply_user_css(engine, "", 0u)
          && navigation->page.runtime == NULL
          && navigation->page.reader_root == native_root);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_reader_activation_retires_frame_without_main_runtime(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0u;
    config.javascript.enabled = false;
    config.javascript.document_scripts_enabled = false;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><article><h1>Frame retirement article</h1>"
        "<p>This substantial server-rendered article supplies enough useful "
        "prose for the bounded semantic Reader extraction on the device.</p>"
        "<p>The second paragraph preserves a coherent explanation while "
        "the lifecycle test attaches an independently owned child realm.</p>"
        "<p>A third paragraph keeps the article dominant over surrounding "
        "chrome and makes the native presentation independently useful.</p>"
        "<p>Additional detail records the context, implications, and next "
        "steps without relying on author-side application behavior.</p>"
        "<p>The closing paragraph gives the classifier a stable article "
        "shape and completes the bounded test document.</p></article></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://frame-retire.example.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    ReaderDocumentAnalysis analysis = {0};
    CHECK(navigation != NULL && navigation->page.runtime == NULL
          && browser_engine_prepare_reader(engine, &analysis)
          && analysis.kind == READER_PAGE_ARTICLE
          && navigation->page.reader_root != NULL
          && navigation->page.reader_root_handle == 0);
    NavigationFrame *frame = &navigation->page.frames[0];
    static const char child[] =
        "<!doctype html><body><p>Independent child realm</p></body>";
    Budget *budget = browser_engine_budget(engine);
    CHECK(document_parse(
              &frame->document, budget, child, sizeof(child) - 1u, 16u));
    frame->runtime = script_runtime_create_deferred(
        &frame->document, budget, 2u * MIB, 1000u,
        "https://frame-retire.example.test/child", NULL,
        &frame->script_result);
    frame->loaded = true;
    navigation->page.frame_count = 1u;
    CHECK(frame->runtime != NULL);
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(site_adapter_reader_css(
              "https://frame-retire.example.test/",
              SITE_ADAPTER_READER_FONT_SANS, 100u,
              css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_activate_reader_view(engine)
          && navigation->page.runtime == NULL
          && navigation->page.frames[0].runtime == NULL);

    CHECK(browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static lxb_dom_node_t *test_reader_root_node(PocDocument *document)
{
    lxb_dom_node_t *body = document_body_node(document);
    for (lxb_dom_node_t *node = body == NULL ? NULL : body->first_child;
         node != NULL; node = node->next) {
        size_t length = 0;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && document_attribute(
                   node, "data-tilefinch-reader-root", &length) != NULL) {
            return node;
        }
    }
    return NULL;
}

static lxb_dom_node_t *test_reader_first_image(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root;
    for (size_t visited = 0; node != NULL && visited++ < 2048u;) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 3u && memcmp(name, "img", 3u) == 0)
            return node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == NULL || node == root) break;
        node = node->next;
    }
    return NULL;
}

static lxb_dom_node_t *test_reader_find_id(lxb_dom_node_t *root,
                                           const char *wanted)
{
    lxb_dom_node_t *node = root;
    size_t wanted_length = strlen(wanted);
    for (size_t visited = 0; node != NULL && visited++ < 4096u;) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == wanted_length
            && memcmp(id, wanted, length) == 0) return node;
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next == NULL)
            node = node->parent;
        if (node == NULL || node == root) break;
        node = node->next;
    }
    return NULL;
}

static bool test_reader_drain_images(BrowserEngine *engine)
{
    for (size_t pumps = 0; pumps < 64u; pumps++) {
        NavigationSession *navigation = browser_engine_navigation(engine);
        if (navigation == NULL
            || !navigation_background_resources_pending(navigation)) {
            return navigation != NULL;
        }
        bool changed = false;
        if (!browser_engine_run_idle_work(engine, &changed)) return false;
    }
    return false;
}

static bool test_reader_image_pointer_is_current(
    const ImageResources *images, const ImageResource *image)
{
    if (images == NULL || image == NULL) return false;
    for (size_t i = 0; i < images->count; i++)
        if (image == &images->items[i]) return true;
    return false;
}

static int test_reader_manual_image_aliases(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 0;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = 8u;
    config.resources.maximum_image_bytes = 128u * 1024u;
    config.resources.maximum_image_file_bytes = 64u * 1024u;
    config.resources.maximum_decoded_image_bytes = 128u * 1024u;
    config.resources.timeout_ms = 1000;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char html[] =
        "<!doctype html><title>Reader image aliases</title>"
        "<style>html,body,main{opacity:0!important;visibility:hidden!important;"
        "transform:translateX(2048px)!important}</style><body>"
        "<article><h1>Reader image aliases</h1>"
        "<p>This long article introduction provides enough semantic text "
        "for a stable extracted reader document on a small device.</p>"
        "<img id=hero src=/hero.svg alt=Hero width=24 height=24>"
        "<p>The middle paragraph surrounds the retained illustration with "
        "ordinary readable prose and a useful descriptive caption.</p>"
        "<img id=tail src=/tail.svg alt=Tail width=24 height=24>"
        "<p>The final paragraph proves the complete article stays present "
        "after the bounded semantic tree is installed.</p>"
        "<p>Reader presentation keeps headings, paragraphs, links, images, "
        "captions, lists, tables, and code in predictable document order.</p>"
        "<p>The retained source page remains available when the user returns "
        "to raw presentation, without rebuilding its authored content.</p>"
        "<p>Small-screen reading benefits from stable margins, useful line "
        "lengths, clear contrast, and illustrations that do not disappear.</p>"
        "<p>This final semantic paragraph deliberately carries enough prose "
        "to qualify the fixture as a dominant long-form article.</p>"
        "</article></body>";
    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-deferred-document-images";
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(engine != NULL
          && site_adapter_reader_css(
                 "https://deferred-images.test/page",
                 SITE_ADAPTER_READER_FONT_SERIF, 100,
                 css, sizeof(css), adapter, sizeof(adapter))
          && fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_commit_html(
                 engine, "https://deferred-images.test/page",
                 html, sizeof(html) - 1u, true)
          && test_reader_drain_images(engine));
    fetch_trace_end();

    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *document_root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *hero = test_reader_find_id(document_root, "hero");
    const ImageResource *hero_image = images_find_node(
        &navigation->page.images, hero);
    size_t before_prepare = navigation->page.images.count;
    ReaderDocumentAnalysis manual = {0};
    CHECK(hero_image != NULL && image_resource_available(hero_image)
          && browser_engine_prepare_reader(engine, &manual));
    /* Alias-table growth may move ImageResources.items; resolve the owner
       again rather than retaining a borrowed entry pointer across prepare. */
    hero_image = images_find_node(&navigation->page.images, hero);
    lxb_dom_node_t *reader_root = test_reader_root_node(
        &navigation->page.document);
    lxb_dom_node_t *reader_image = test_reader_first_image(reader_root);
    const ImageResource *manual_image = images_find_node(
        &navigation->page.images, reader_image);
    if (!(reader_root != NULL && reader_image != NULL
          && manual_image != NULL && image_resource_available(manual_image)
          && image_resource_backing_identity(manual_image)
                 == image_resource_backing_identity(hero_image)
          && !manual_image->owns_pixels && !manual_image->owns_encoded
          && navigation->page.images.count == before_prepare + 2u)) {
        fprintf(stderr,
                "reader-alias manual root=%d clone=%d resource=%d "
                "available=%d same=%d owns=%d/%d count=%zu before=%zu "
                "duplicates=%zu reader=%d high=%d text=%u extracted=%u\n",
                reader_root != NULL, reader_image != NULL,
                manual_image != NULL,
                image_resource_available(manual_image),
                manual_image != NULL && hero_image != NULL
                    && image_resource_backing_identity(manual_image)
                       == image_resource_backing_identity(hero_image),
                manual_image != NULL && manual_image->owns_pixels,
                manual_image != NULL && manual_image->owns_encoded,
                navigation->page.images.count, before_prepare,
                navigation->page.images.stats.duplicate,
                (int) manual.kind, manual.high_confidence,
                manual.visible_text_bytes, manual.extracted_bytes);
    }
    CHECK(reader_root != NULL && reader_image != NULL
          && manual_image != NULL && image_resource_available(manual_image)
          && image_resource_backing_identity(manual_image)
                 == image_resource_backing_identity(hero_image)
          && !manual_image->owns_pixels && !manual_image->owns_encoded
          && navigation->page.images.count == before_prepare + 2u);
    size_t after_prepare = navigation->page.images.count;
    CHECK(browser_engine_prepare_reader(engine, NULL)
          && navigation->page.images.count == after_prepare
          && browser_engine_apply_user_css(engine, css, strlen(css)));
    lxb_dom_node_t *html_node = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *body_node = document_body_node(
        &navigation->page.document);
    ComputedStyle html_style = style_for_node(
        &navigation->page.stylesheet, html_node, NULL);
    ComputedStyle body_style = style_for_node(
        &navigation->page.stylesheet, body_node, &html_style);
    ComputedStyle reader_style = style_for_node(
        &navigation->page.stylesheet, reader_root, &body_style);
    CHECK(html_style.opacity == UINT8_MAX
          && !html_style.visibility_hidden && !html_style.has_transform
          && body_style.opacity == UINT8_MAX
          && body_style.visibility_hidden && !body_style.has_transform
          && reader_style.opacity == UINT8_MAX
          && !reader_style.visibility_hidden && !reader_style.has_transform);

    /* Candidate Reader prepares before image loading. Its clone should reach
       the same shared-backing result through ordinary loader deduplication;
       manual preparation above must be visually/resource-equivalent. */
    browser_engine_set_reader_candidate_mode(engine, true);
    CHECK(browser_engine_set_user_css(engine, css, strlen(css))
          && fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_commit_html(
                 engine, "https://deferred-images.test/candidate",
                 html, sizeof(html) - 1u, true)
          && test_reader_drain_images(engine));
    fetch_trace_end();
    navigation = browser_engine_navigation(engine);
    document_root = lxb_dom_interface_node(navigation->page.document.html);
    hero = test_reader_find_id(document_root, "hero");
    reader_root = test_reader_root_node(&navigation->page.document);
    reader_image = test_reader_first_image(reader_root);
    hero_image = images_find_node(&navigation->page.images, hero);
    const ImageResource *candidate_image = images_find_node(
        &navigation->page.images, reader_image);
    ReaderDocumentAnalysis candidate = {0};
    CHECK(browser_engine_reader_analysis(engine, &candidate)
          && candidate.prepared && reader_root != NULL
          && hero_image != NULL && candidate_image != NULL
          && image_resource_available(candidate_image)
          && image_resource_backing_identity(candidate_image)
                 == image_resource_backing_identity(hero_image));

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0
          && budget_active_allocations(budget, NULL) == 0);
    browser_engine_destroy(engine);
    return 0;
}

static int test_reader_alias_relocation_rebinds_declined_auto_layout(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = 16u;
    config.resources.maximum_image_bytes = 128u * 1024u;
    config.resources.maximum_image_file_bytes = 64u * 1024u;
    config.resources.maximum_decoded_image_bytes = 128u * 1024u;
    config.resources.timeout_ms = 1000;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    char html[16384];
    int prefix = snprintf(
        html, sizeof(html),
        "<!doctype html><title>Declined automatic Reader</title><body><main>"
        "<h1>Declined automatic Reader</h1>"
        "<p>This substantial first paragraph creates a useful manual Reader "
        "candidate without an article element, so automatic engagement must "
        "conservatively decline it while retaining the raw display list.</p>");
    CHECK(prefix > 0 && (size_t) prefix < sizeof(html));
    size_t used = (size_t) prefix;
    static const char image[] =
        "<img width=1 height=1 alt='Retained illustration' "
        "src='/hero.svg'>";
    for (size_t i = 0; i < 16u; i++) {
        size_t length = sizeof(image) - 1u;
        CHECK(length < sizeof(html) - used);
        memcpy(html + used, image, length);
        used += length;
    }
    int suffix = snprintf(
        html + used, sizeof(html) - used,
        "<p>This second substantial paragraph surrounds the bounded image "
        "set with ordinary prose and preserves stable raw image commands.</p>"
        "<p>This third substantial paragraph keeps classification deterministic "
        "without granting the high confidence required by automatic Reader.</p>"
        "<p>This fourth substantial paragraph completes the useful long-form "
        "shape and leaves rendering safe after the alias table moves.</p>"
        "<p>This fifth substantial paragraph adds concentrated readable text "
        "without changing the intentionally non-article semantic wrapper.</p>"
        "<p>This final substantial paragraph ensures the main subtree exceeds "
        "the deterministic density threshold while automatic Reader still "
        "declines the lower-confidence structural classification.</p>"
        "</main></body>");
    CHECK(suffix > 0 && (size_t) suffix < sizeof(html) - used);
    used += (size_t) suffix;

    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-deferred-document-images";
    CHECK(fetch_trace_replay_begin(fixture, error, sizeof(error))
          && browser_engine_commit_html(
                 engine, "https://deferred-images.test/relocation",
                 html, used, true)
          && test_reader_drain_images(engine));
    fetch_trace_end();
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation != NULL && navigation->page.images.count == 16u
          && navigation->page.images.capacity
                 == 16u + NAVIGATION_FRAME_LIMIT);
    TileCache *render = browser_engine_render_shell(engine);
    CHECK(render != NULL && render->owns_visual_layout
          && render->source_layout == &navigation->page.layout
          && render->layout == &render->visual_layout);
    ImageResource *previous_items = navigation->page.images.items;
    size_t image_commands = 0;
    for (size_t i = 0; i < navigation->page.layout.count; i++) {
        const DrawCommand *command = &navigation->page.layout.commands[i];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        CHECK(test_reader_image_pointer_is_current(
            &navigation->page.images, command->image));
        image_commands++;
    }
    CHECK(image_commands >= 16u);
    size_t relayouts_before = navigation->incremental_relayouts;
    ReaderDocumentAnalysis automatic = {0};
    bool prepared = browser_engine_prepare_reader(engine, &automatic);
    if (!(prepared && automatic.kind == READER_PAGE_ARTICLE
          && !automatic.high_confidence
          && navigation->incremental_relayouts == relayouts_before
          && navigation->page.images.items != previous_items
          && navigation->page.images.count == 32u)) {
        fprintf(stderr,
                "reader relocation prepared=%d kind=%d high=%d "
                "relayouts=%zu/%zu moved=%d images=%zu/%zu capacity=%zu "
                "text=%u extracted=%u\n",
                prepared, (int) automatic.kind, automatic.high_confidence,
                navigation->incremental_relayouts, relayouts_before,
                navigation->page.images.items != previous_items,
                navigation->page.images.count, (size_t) 32u,
                navigation->page.images.capacity,
                automatic.visible_text_bytes, automatic.extracted_bytes);
    }
    CHECK(prepared && automatic.kind == READER_PAGE_ARTICLE
          && !automatic.high_confidence
          && navigation->incremental_relayouts == relayouts_before
          && navigation->page.images.items != previous_items
          && navigation->page.images.count == 32u);
    image_commands = 0;
    for (size_t i = 0; i < navigation->page.layout.count; i++) {
        const DrawCommand *command = &navigation->page.layout.commands[i];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        CHECK(test_reader_image_pointer_is_current(
            &navigation->page.images, command->image));
        image_commands++;
    }
    CHECK(image_commands >= 16u);
    size_t visual_image_commands = 0;
    for (size_t i = 0; i < render->visual_layout.count; i++) {
        const DrawCommand *command = &render->visual_layout.commands[i];
        if (command->type != DRAW_IMAGE || command->image == NULL) continue;
        CHECK(test_reader_image_pointer_is_current(
            &navigation->page.images, command->image));
        visual_image_commands++;
    }
    CHECK(visual_image_commands >= 16u
          && browser_engine_render_frame(engine, NULL));

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_reader_deferred_images_alias_before_relayout(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 4u;
    config.progressive_first_paint = false;
    config.resources.enabled = true;
    config.resources.web_fonts_enabled = false;
    config.resources.maximum_images = 8u;
    config.resources.maximum_image_bytes = 128u * 1024u;
    config.resources.maximum_image_file_bytes = 64u * 1024u;
    config.resources.maximum_decoded_image_bytes = 128u * 1024u;
    config.resources.timeout_ms = 1000;
    config.javascript.enabled = false;
    config.javascript.document_scripts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char html[] =
        "<!doctype html><title>Deferred Reader illustrations</title><body>"
        "<article><h1>Deferred Reader illustrations</h1>"
        "<img id=hero loading=lazy src=/hero.svg alt='Hero illustration' "
        "width=48 height=48>"
        "<img id=tail loading=lazy src=/tail.svg alt='Tail illustration' "
        "width=48 height=48>"
        "<p>This long article introduction provides enough semantic text "
        "for a stable extracted reader document on a small device.</p>"
        "<p>The middle paragraph surrounds the deferred illustrations with "
        "ordinary readable prose and useful descriptive captions.</p>"
        "<p>The final paragraph proves the complete article stays present "
        "after the bounded semantic tree is installed.</p>"
        "<p>Reader presentation keeps headings, paragraphs, links, images, "
        "captions, lists, tables, and code in predictable document order.</p>"
        "<p>The retained source page remains available when the user returns "
        "to raw presentation, without rebuilding its authored content.</p>"
        "<p>Small-screen reading benefits from stable margins, useful line "
        "lengths, clear contrast, and illustrations that do not disappear.</p>"
        "<p>This final semantic paragraph deliberately carries enough prose "
        "to qualify the fixture as a dominant long-form article.</p>"
        "</article></body>";
    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-deferred-document-images";
    static const char blank[] =
        "<!doctype html><title>Blank</title><body>Blank";
    char css[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    CHECK(engine != NULL
          && site_adapter_reader_css(
                 "https://deferred-images.test/reader",
                 SITE_ADAPTER_READER_FONT_SERIF, 100,
                 css, sizeof(css), adapter, sizeof(adapter))
          && browser_engine_commit_html(
                 engine, "https://blank.test/", blank,
                 sizeof(blank) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    uint64_t generation = navigation == NULL ? 0u
        : navigation_begin(navigation);
    CHECK(navigation != NULL
          && fetch_trace_replay_begin(fixture, error, sizeof(error))
          && navigation_commit_static_html(
                 navigation, generation,
                 "https://deferred-images.test/reader",
                 html, sizeof(html) - 1u,
                 config.device.navigation_viewport_width,
                 NULL, NULL, true)
          && browser_engine_refresh_shell(engine));
    lxb_dom_node_t *document_root = lxb_dom_interface_node(
        navigation->page.document.html);
    lxb_dom_node_t *original_image = test_reader_find_id(
        document_root, "hero");
    if (!(navigation != NULL && original_image != NULL
          && navigation->page.images.stats.loaded == 0u
          && navigation->page.deferred_image_count == 2u
          && navigation->page.deferred_image_cursor == 0u)) {
        fprintf(stderr,
                "deferred reader setup navigation=%d original=%d "
                "loaded=%zu images=%zu deferred=%zu cursor=%zu pending=%d\n",
                navigation != NULL, original_image != NULL,
                navigation == NULL ? 0u
                    : navigation->page.images.stats.loaded,
                navigation == NULL ? 0u : navigation->page.images.count,
                navigation == NULL ? 0u
                    : navigation->page.deferred_image_count,
                navigation == NULL ? 0u
                    : navigation->page.deferred_image_cursor,
                navigation != NULL
                    && navigation->page.image_continuation_pending);
    }
    CHECK(navigation != NULL && original_image != NULL
          && navigation->page.images.stats.loaded == 0u
          && navigation->page.deferred_image_count == 2u
          && navigation->page.deferred_image_cursor == 0u);

    ReaderDocumentAnalysis reader = {0};
    CHECK(browser_engine_prepare_reader(engine, &reader)
          && reader.kind == READER_PAGE_ARTICLE
          && browser_engine_apply_user_css(engine, css, strlen(css)));
    lxb_dom_node_t *reader_root = test_reader_root_node(
        &navigation->page.document);
    lxb_dom_node_t *reader_image = test_reader_first_image(reader_root);
    CHECK(reader_root != NULL && reader_image != NULL
          && images_find_node(&navigation->page.images, reader_image) == NULL
          && browser_engine_render_frame(engine, NULL));
    size_t frame_pixels = 0;
    const uint16_t *frame = browser_engine_framebuffer(
        engine, &frame_pixels);
    uint64_t placeholder = frame_checksum(frame, frame_pixels);

    size_t visual_changes = 0;
    for (size_t pumps = 0; pumps < 32u; pumps++) {
        bool visual_changed = false;
        bool pending = browser_engine_run_deferred_image_work(
            engine, &visual_changed);
        if (visual_changed) {
            visual_changes++;
            CHECK(browser_engine_render_frame(engine, NULL));
        }
        if (!pending) break;
    }
    fetch_trace_end();

    const ImageResource *original_resource = images_find_node(
        &navigation->page.images, original_image);
    const ImageResource *reader_resource = images_find_node(
        &navigation->page.images, reader_image);
    size_t reader_paints = 0;
    for (size_t i = 0; i < navigation->page.layout.count; i++) {
        const DrawCommand *command = &navigation->page.layout.commands[i];
        if (command->type == DRAW_IMAGE
            && command->image == reader_resource
            && command->width > 0 && command->height > 0) {
            reader_paints++;
        }
    }
    frame = browser_engine_framebuffer(engine, &frame_pixels);
    uint64_t painted = frame_checksum(frame, frame_pixels);
    CHECK(visual_changes != 0u
          && navigation->page.images.stats.loaded == 2u
          && original_resource != NULL && reader_resource != NULL
          && image_resource_available(original_resource)
          && image_resource_available(reader_resource)
          && image_resource_backing_identity(reader_resource)
                 == image_resource_backing_identity(original_resource)
          && !reader_resource->owns_pixels
          && !reader_resource->owns_encoded
          && reader_paints != 0u
          && painted != placeholder);

    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL && browser_engine_shutdown(engine)
          && budget->current == 0u
          && budget_active_allocations(budget, NULL) == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static const char *test_youtube_preferred_language(void *opaque)
{
    (void) opaque;
    return "fr";
}

static TilefinchDateFormat test_youtube_date_format_value;

static TilefinchDateFormat test_youtube_preferred_date_format(void *opaque)
{
    (void) opaque;
    return test_youtube_date_format_value;
}

static bool test_youtube_watch_metadata_case(
    Budget *budget, const char *initial_data, const char *machine_date,
    const char *expected_date, const char *expected_views,
    const char *forbidden)
{
    char source[4096];
    int length = snprintf(
        source, sizeof(source),
        "<script>var ytInitialData = '%s';</script>"
        "<script>var ytInitialPlayerResponse={"
        "\"videoDetails\":{\"videoId\":\"TFTEST00001\","
        "\"title\":\"Fixture video\",\"author\":\"Fixture channel\","
        "\"lengthSeconds\":\"19\",\"viewCount\":\"23\"},"
        "\"microformat\":{\"playerMicroformatRenderer\":{"
        "\"publishDate\":\"%s\"}}};</script>",
        initial_data, machine_date);
    if (length <= 0 || (size_t) length >= sizeof(source)) return false;
    YoutubeLiteDocument document = {0};
    char error[256] = {0};
    bool built = youtube_lite_build_document(
        budget, "https://www.youtube.com/watch?v=TFTEST00001",
        source, (size_t) length, &document, error, sizeof(error));
    bool matched = built
        && strstr(document.html, expected_date) != NULL
        && (expected_views == NULL
            || strstr(document.html, expected_views) != NULL)
        && (forbidden == NULL || strstr(document.html, forbidden) == NULL);
    youtube_lite_document_destroy(&document);
    return matched;
}

static int test_youtube_localized_watch_metadata(void)
{
    static const char english[] =
        "{\"videoDescriptionHeaderRenderer\":{"
        "\"views\":{\"runs\":[{\"text\":\"23 views\"}]},"
        "\"publishDate\":{\"runs\":[{\"text\":\"Nov 10, \"},"
        "{\"text\":\"2014\"}]}}}";
    static const char japanese[] =
        "{\"videoDescriptionHeaderRenderer\":{"
        "\"views\":{\"runs\":[{\"text\":\"23\xe5\x9b\x9e\xe8\xa6\x96\xe8\x81\xb4\"}]},"
        "\"publishDate\":{\"runs\":[{\"text\":\"2014/11/10\"}]}}}";
    Budget budget;
    budget_init(&budget, 4u * MIB);
    TilefinchPlatformServices services = {
        .preferred_date_format = test_youtube_preferred_date_format
    };
    tilefinch_platform_set_services(&services);
    test_youtube_date_format_value = TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY;
    CHECK(test_youtube_watch_metadata_case(
              &budget, english, "2014-11-10T06:05:55-08:00",
              "Published Nov 10, 2014", "23 views", "23 views views")
          && test_youtube_watch_metadata_case(
              &budget, japanese, "2014-11-10T06:05:55-08:00",
              "Published 2014/11/10", "23\xe5\x9b\x9e\xe8\xa6\x96\xe8\x81\xb4", "23 views"));
    static const struct {
        TilefinchDateFormat format;
        const char *expected;
    } numeric[] = {
        {TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY, "Published 2005/04/23"},
        {TILEFINCH_DATE_FORMAT_MONTH_DAY_YEAR, "Published 04/23/2005"},
        {TILEFINCH_DATE_FORMAT_DAY_MONTH_YEAR, "Published 23/04/2005"}
    };
    for (size_t i = 0; i < sizeof(numeric) / sizeof(numeric[0]); i++) {
        test_youtube_date_format_value = numeric[i].format;
        CHECK(test_youtube_watch_metadata_case(
            &budget, "{}", "2005-04-23", numeric[i].expected,
            "23 views", "23 views views"));
    }
    CHECK(test_youtube_watch_metadata_case(
        &budget, "{}", "not-a-date", "Published not-a-date",
        "23 views", "23 views views"));
    tilefinch_platform_set_services(NULL);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

static int test_youtube_cooperative_build_convergence(void)
{
    static const char url[] =
        "https://www.youtube.com/watch?v=TFTEST00001"
        "&tilefinch_view=comments";
    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube";
    char primary_path[1024];
    char supplemental_path[1024];
    snprintf(primary_path, sizeof(primary_path), "%s/0000.body", fixture);
    snprintf(
        supplemental_path, sizeof(supplemental_path),
        "%s/0001.body", fixture);
    size_t primary_length = 0, supplemental_length = 0;
    char *primary = read_test_file(primary_path, &primary_length);
    char *supplemental =
        read_test_file(supplemental_path, &supplemental_length);
    CHECK(primary != NULL && supplemental != NULL);

    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    char error[256] = {0};
    YoutubeLiteDocument expected = {0}, actual = {0};
    YoutubeLiteResolverIdentity resolver_identity = {0};
    char cookie_header[256] = {0};
    TilefinchPlatformServices services = {
        .preferred_language = test_youtube_preferred_language,
        .preferred_date_format = test_youtube_preferred_date_format
    };
    test_youtube_date_format_value =
        TILEFINCH_DATE_FORMAT_YEAR_MONTH_DAY;
    tilefinch_platform_set_services(&services);
    CHECK(browser_session_init(&session, &budget, 512u * 1024u)
          && youtube_lite_build_document_with_comments(
              &budget, url, primary, primary_length,
              supplemental, supplemental_length,
              &expected, error, sizeof(error))
          && fetch_trace_replay_begin(fixture, error, sizeof(error))
          && youtube_lite_load(
              &budget, &session, url, 1u * MIB, 1000,
              &actual, error, sizeof(error))
          && youtube_lite_resolver_identity_get(
              &session, &resolver_identity)
          && resolver_identity.api_key[0] != '\0'
          && resolver_identity.visitor[0] != '\0'
          && browser_session_cookie_header(
              &session, "https://m.youtube.com/", cookie_header,
              sizeof(cookie_header))
          && strstr(cookie_header, "PREF=hl=fr&tz=UTC") != NULL
          && actual.html_length == expected.html_length
          && actual.result_count == expected.result_count
          && actual.route == expected.route
          && strstr(actual.html, "Published 23 avr. 2005") != NULL
          && strstr(actual.html, "1 234 vues") != NULL
          && strstr(actual.html, "1 234 vues views") == NULL
          && memcmp(
              actual.html, expected.html, expected.html_length) == 0);
    fetch_trace_end();
    tilefinch_platform_set_services(NULL);
    youtube_lite_document_destroy(&actual);
    youtube_lite_document_destroy(&expected);
    browser_session_destroy(&session);
    free(supplemental);
    free(primary);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}

/* A response whose body carries no initial-data marker (consent
   interstitial, error page, size-truncated capture) leaves the decode with
   nothing to hand forward.  The phase machine never consults timeout_ms, so
   if PREPARE can route such a job back to DECODE the pair spins forever and
   the synchronous entry point never returns.  Bound the pumps and require a
   terminal status; which terminal status the marker-less body reaches is the
   builder's business, only non-termination is the defect. */
static int test_youtube_missing_initial_data_terminates(void)
{
    static const char url[] =
        "https://www.youtube.com/watch?v=TFTEST00001";
    static const char fixture[] =
        TILEFINCH_TEST_SOURCE_DIR
        "/tests/fixtures/http-site-adapter-youtube-no-initial-data";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    BrowserSession session;
    char error[256] = {0};
    CHECK(browser_session_init(&session, &budget, 512u * 1024u)
          && fetch_trace_replay_begin(fixture, error, sizeof(error)));
    YoutubeLiteLoadJob *job = youtube_lite_load_begin(
        &budget, &session, url, 1u * MIB, 1000, error, sizeof(error));
    CHECK(job != NULL);
    FetchPumpQuota quota = {
        .maximum_body_bytes = 4096,
        .maximum_body_callbacks = 4,
        .maximum_time_us = 1000
    };
    /* Generous relative to the work actually required: the body is a single
       sub-slice read, so a correct job settles in a handful of pumps.  The
       livelock reproduces as this loop running out. */
    enum { PUMP_LIMIT = 256 };
    YoutubeLiteLoadStatus status = YOUTUBE_LITE_LOAD_PENDING;
    size_t pumps = 0;
    for (; pumps < PUMP_LIMIT; pumps++) {
        status = youtube_lite_load_pump(job, &quota);
        if (status != YOUTUBE_LITE_LOAD_PENDING) break;
    }
    CHECK(pumps < PUMP_LIMIT
          && status != YOUTUBE_LITE_LOAD_PENDING
          && youtube_lite_load_status(job) == status);
    youtube_lite_load_destroy(job);
    fetch_trace_end();
    browser_session_destroy(&session);
    CHECK(budget.current == 0 && budget.external_reserved == 0);
    return 0;
}
