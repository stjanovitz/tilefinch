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
    ControllerAction compact_play = {0};
    CHECK(browser_engine_activate(engine, &compact_play)
          && compact_play.prefer_native_media
          && browser_engine_focus_direction(
                 engine, CONTROLLER_FOCUS_RIGHT));
    ControllerAction compact_details = {0};
    CHECK(browser_engine_activate(engine, &compact_details)
          && !compact_details.prefer_native_media
          && strcmp(compact_details.url, compact_play.url) == 0
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
    remove_large_youtube_replay(large_fixture);

    /* The provider's primary result action remains a canonical navigation URL
       with an explicit native-media preference. A frontend without the native
       route can ignore the hint and navigate; the adjacent Details action is
       deliberately unmarked. */
    static const char provider_actions[] =
        "<!doctype html><title>Provider results</title>"
        "<a autofocus data-tilefinch-provider-media "
        "href='https://www.youtube.com/watch?v=TFTEST00001'>Play</a>"
        "<a href='https://www.youtube.com/watch?v=TFTEST00001'>Details</a>";
    CHECK(browser_engine_commit_html(
        engine, "https://www.youtube.com/results?search_query=fixture",
        provider_actions, sizeof(provider_actions) - 1u, true));
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
          && strstr(css, "data-tilefinch-reader-entry") != NULL
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
    config.tile_capacity = 0;
    char error[128] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char html[] =
        "<!doctype html><title>Reader</title>"
        "<header><nav><a href='/menu'>Menu</a></nav></header>"
        "<main><article><h1>Readable story</h1>"
        "<p>Long-form text.</p><a href='/next'>Next</a></article></main>"
        "<footer>Footer</footer>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://news.example.test/article",
                 html, sizeof(html) - 1u, true)
          && browser_engine_navigation(engine)->page.layout.link_count == 2
          && browser_engine_apply_user_css(engine, css, strlen(css))
          && browser_engine_navigation(engine)->page.layout.link_count == 1
          && strstr(
                 browser_engine_navigation(engine)->page.layout.links[0].url,
                 "/next") != NULL
          );
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
        "<article><p>Next story.</p><a href='/third'>Third</a></article>";
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
    CHECK(browser_engine_shutdown(engine));
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
