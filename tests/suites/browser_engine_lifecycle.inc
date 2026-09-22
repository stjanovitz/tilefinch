static int test_engine_lifecycle(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    CHECK(strcmp(profile.name, "psp-3000-333mhz") == 0
          && profile.target_clock_mhz == 333
          && profile.framebuffer_width == 480
          && profile.framebuffer_height == 272
          && profile.recommended_memory_limit
                 == BROWSER_PSP_REALISTIC_CONTENT_LIMIT
          && profile.minimum_non_page_reserve
                 == BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE
          && profile.maximum_tile_capacity == 8);

    BrowserConfig config;
    browser_config_init(&config, &profile);
    CHECK(config.memory_limit == BROWSER_PSP_REALISTIC_CONTENT_LIMIT
          && config.non_page_memory_reserve
                 == BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE
          && config.history_capacity == 16
          && config.session_cache_limit == 1u * MIB
          && config.tile_capacity == 8
          && config.progressive_first_paint
          && config.idle_work_budget_us == 500
          && config.idle_work_maximum_units == 8
          && config.javascript.heap_limit == 4u * MIB
          && config.javascript.maximum_total_bytes == 4u * MIB
          && browser_config_apply_psp_memory_profile(
                 &config, BROWSER_PSP_MEMORY_STRICT)
          && config.memory_limit == BROWSER_PSP_STRICT_CONTENT_LIMIT
          && config.non_page_memory_reserve
                 == BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE
          && config.history_capacity == 8
          && config.session_cache_limit == 512u * 1024u
          && config.tile_capacity == 8
          && config.javascript.heap_limit == 4u * MIB
          && config.javascript.maximum_total_bytes == 1u * MIB
          && config.javascript.maximum_file_bytes == 256u * 1024u);
    browser_config_init(&config, &profile);
    CHECK(browser_config_apply_psp_memory_profile(
              &config, BROWSER_PSP_MEMORY_REALISTIC)
          && config.javascript.heap_limit == 5u * MIB
          && config.javascript.maximum_total_bytes == 2u * MIB
          && config.javascript.maximum_file_bytes == 384u * 1024u);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 512u * 1024u;
    config.maximum_document_bytes = 2u * MIB;
    config.tile_capacity = 2;
    config.declared_css_width = 1024;
    config.declared_css_height = 581;
    config.javascript.enabled = true;
    DiagnosticProbe diagnostics = {.valid = true};
    config.diagnostics.callback = capture_diagnostic;
    config.diagnostics.opaque = &diagnostics;
    config.diagnostics.minimum_severity = TILEFINCH_DIAGNOSTIC_DEBUG;
    char error[256] = {0};
    CHECK(browser_config_validate(&config, error, sizeof(error)));

    const size_t configured_limit = config.memory_limit;
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_last_diagnostic_code(engine)
              == TILEFINCH_DIAGNOSTIC_LIFECYCLE
          && diagnostics.last_code == TILEFINCH_DIAGNOSTIC_LIFECYCLE
          && strcmp(diagnostics.last_name, "engine-created") == 0);
    config.memory_limit = 1;
    CHECK(browser_engine_config(engine)->memory_limit == configured_limit);
    CHECK(browser_engine_navigation(engine) != NULL
          && browser_engine_navigation(engine)->declared_css_width == 1024
          && browser_engine_navigation(engine)->declared_css_height == 581
          && browser_engine_session(engine) != NULL
          && browser_engine_controller(engine) == NULL
          && browser_engine_render_shell(engine) == NULL);

    /* Native HOME commits a script-free placeholder with policy disabled,
       then the first real navigation reapplies the configured policy. The
       placeholder must not allocate a realm merely to give chrome a valid
       document shape. */
    static const char home_placeholder[] =
        "<!doctype html><title>Tilefinch</title><body>";
    CHECK(browser_engine_set_javascript_enabled(engine, false)
          && browser_engine_commit_html(
                 engine, TILEFINCH_HOMEPAGE_URL, home_placeholder,
                 sizeof(home_placeholder) - 1u, false)
          && browser_engine_navigation(engine)->page.runtime == NULL
          && browser_engine_set_javascript_enabled(engine, true)
          && browser_engine_navigation(engine)->page.runtime == NULL);

    static const char page[] =
        "<!doctype html><title>Engine</title>"
        "<style>body{margin:4px}#probe{height:24px}"
        "</style><body><a href='#probe'>jump</a><input id=edit>"
        "<input type=password value=secret>"
        "<input autocomplete='section-login one-time-code' value=123456>"
        "<div id=probe>ready</div>";
    CHECK(browser_engine_commit_html(
        engine, "https://engine.test/", page, sizeof(page) - 1, true));
    CHECK(browser_engine_controller(engine) != NULL
          && browser_engine_navigation(engine)->page.runtime != NULL
          && browser_engine_render_shell(engine) != NULL);
    CHECK(browser_engine_navigation_view(engine)
              == browser_engine_navigation(engine)
          && browser_engine_controller_view(engine)
              == browser_engine_controller(engine)
          && browser_engine_render_metrics_view(engine)
              == browser_engine_render_shell(engine)
          && browser_engine_font_face(engine, FONT_SANS)
              == font_set_face(browser_engine_fonts(engine),
                               FONT_SANS));
    BrowserViewSnapshot view = {0};
    CHECK(browser_engine_view_snapshot(engine, &view)
          && strcmp(view.url, "https://engine.test/") == 0
          && strcmp(view.title, "Engine") == 0
          && view.secure && !view.can_go_back && !view.can_go_forward);
    TileCache *render_shell = browser_engine_render_shell(engine);
    CHECK(render_shell->owns_visual_layout
          && render_shell->layout == &render_shell->visual_layout
          && render_shell->source_layout
                 == &browser_engine_navigation(engine)->page.layout);
    size_t frame_pixels = 0;
    CHECK(browser_engine_framebuffer(engine, &frame_pixels) != NULL
          && frame_pixels == 480u * 272u);
    BrowserFrameView frame_view = {0};
    CHECK(browser_engine_frame_view(engine, &frame_view)
          && frame_view.pixels
                 == browser_engine_framebuffer(engine, NULL)
          && frame_view.pixel_count == frame_pixels
          && frame_view.width == 480 && frame_view.height == 272
          && frame_view.stride == 480);
    BrowserFrontendSnapshot frontend = {0};
    CHECK(browser_engine_frontend_snapshot(engine, &frontend)
          && frontend.frame.pixels == frame_view.pixels
          && frontend.frame.pixel_count == frame_view.pixel_count
          && frontend.navigation == browser_engine_navigation(engine)
          && frontend.controller == browser_engine_controller(engine));
    CHECK(!browser_engine_frontend_snapshot(NULL, &frontend)
          && frontend.frame.pixels == NULL
          && frontend.navigation == NULL
          && frontend.controller == NULL);
    CHECK(browser_engine_render_frame(engine, NULL));
    uint64_t light_frame_hash = frame_checksum(
        browser_engine_framebuffer(engine, NULL), frame_pixels);
    BrowserFindSnapshot find = {0};
    CHECK(browser_engine_find_begin(engine, "READY", &find)
          && strcmp(find.query, "READY") == 0
          && find.match_count == 1 && find.selected == 0
          && !find.truncated
          && browser_engine_find_snapshot(engine, &find)
          && browser_engine_render_frame(engine, NULL)
          && frame_checksum(
                 browser_engine_framebuffer(engine, NULL), frame_pixels)
                 != light_frame_hash);
    CHECK(browser_engine_find_move(engine, 1, &find)
          && find.selected == 0);
    browser_engine_find_clear(engine);
    CHECK(!browser_engine_find_snapshot(engine, &find)
          && browser_engine_render_frame(engine, NULL)
          && frame_checksum(
                 browser_engine_framebuffer(engine, NULL), frame_pixels)
                 == light_frame_hash);
    static const char user_css[] = "#probe{height:30px}";
    CHECK(browser_engine_set_user_css(
              engine, user_css, sizeof(user_css) - 1)
          && browser_engine_navigation_view(engine)
                 ->user_css_length == sizeof(user_css) - 1);
    CHECK(browser_engine_set_forced_dark(engine, true)
          && browser_engine_render_shell(engine)->forced_dark
          && browser_engine_render_frame(engine, NULL));
    CHECK(frame_checksum(
              browser_engine_framebuffer(engine, NULL), frame_pixels)
          != light_frame_hash);
    CHECK(browser_engine_set_forced_dark(engine, false)
          && !browser_engine_render_shell(engine)->forced_dark
          && browser_engine_render_frame(engine, NULL)
          && frame_checksum(
                 browser_engine_framebuffer(engine, NULL), frame_pixels)
                 == light_frame_hash);
    NavigationSession *pointer_navigation =
        browser_engine_navigation(engine);
    CHECK(pointer_navigation->page.layout.link_count != 0);
    const LinkRegion *pointer_link =
        &pointer_navigation->page.layout.links[0];
    int pointer_device_x = viewport_css_to_device(
        &pointer_navigation->viewport, pointer_link->x + 1);
    int pointer_device_y = viewport_css_to_device(
        &pointer_navigation->viewport, pointer_link->y + 1);
    size_t pointer_relayouts = pointer_navigation->incremental_relayouts;
    bool pointer_activate = true, pointer_page_changed = true;
    CHECK(browser_engine_pointer_event(
              engine, CONTROLLER_POINTER_MOVE,
              pointer_device_x, pointer_device_y,
              &pointer_activate, &pointer_page_changed)
          && !pointer_activate && !pointer_page_changed
          && pointer_navigation->incremental_relayouts
                 == pointer_relayouts);
    uint16_t *reclaim_reference = malloc(
        frame_pixels * sizeof(*reclaim_reference));
    CHECK(reclaim_reference != NULL);
    memcpy(
        reclaim_reference, browser_engine_framebuffer(engine, NULL),
        frame_pixels * sizeof(*reclaim_reference));
    BrowserController *reclaim_controller =
        browser_engine_controller(engine);
    ScriptRuntime *reclaim_runtime =
        browser_engine_navigation(engine)->page.runtime;
    BrowserOptionalMemoryReclaimJob no_pressure = {0};
    browser_engine_prepare_optional_memory_reclaim(
        engine, 0, &no_pressure);
    CHECK(!browser_engine_optional_memory_reclaim_pending(&no_pressure));
    BrowserOptionalMemoryReclaimJob reclaim_job = {0};
    browser_engine_prepare_optional_memory_reclaim(
        engine, SIZE_MAX, &reclaim_job);
    CHECK(browser_engine_optional_memory_reclaim_pending(&reclaim_job));
    size_t reclaim_pumps = 0;
    while (browser_engine_optional_memory_reclaim_pending(&reclaim_job)
           && reclaim_pumps < 128u) {
        (void) browser_engine_pump_optional_memory_reclaim(
            engine, &reclaim_job);
        reclaim_pumps++;
    }
    CHECK(!browser_engine_optional_memory_reclaim_pending(&reclaim_job));
    CHECK(reclaim_pumps >= 3u && reclaim_pumps < 128u);
    BrowserOptionalMemoryReclaim reclaimed = reclaim_job.reclaimed;
    CHECK(reclaimed.total_bytes
              == reclaimed.font_staging_bytes
                 + reclaimed.javascript_bytes
                 + reclaimed.session_cache_bytes
                 + reclaimed.render_cache_bytes);
    CHECK(browser_engine_controller(engine) == reclaim_controller
          && browser_engine_navigation(engine)->page.runtime
                 == reclaim_runtime
          && browser_engine_navigation(engine)->page.loaded
          && strcmp(browser_engine_navigation(engine)->page.document.title,
                    "Engine") == 0);
    CHECK(browser_engine_render_frame(engine, NULL));
    CHECK(memcmp(
              reclaim_reference, browser_engine_framebuffer(engine, NULL),
              frame_pixels * sizeof(*reclaim_reference)) == 0);
    free(reclaim_reference);
    size_t incumbent_history_count =
        browser_engine_navigation(engine)->history_count;
    BrowserController *incumbent_controller =
        browser_engine_controller(engine);
    FetchScheduler *incumbent_scheduler =
        pointer_navigation->page.resource_scheduler;
    if (incumbent_scheduler == NULL) {
        incumbent_scheduler = fetch_scheduler_create(
            browser_engine_budget(engine), 1, 4096);
        pointer_navigation->page.resource_scheduler = incumbent_scheduler;
    }
    FetchRequest incumbent_request = {.method = "GET"};
    CHECK(incumbent_scheduler != NULL);
    uint64_t incumbent_request_id = fetch_scheduler_enqueue(
        incumbent_scheduler, "https://incumbent.test/optional-thumbnail",
        &incumbent_request, 4096, 1000);
    if (incumbent_request_id == 0) {
        fprintf(stderr, "incumbent enqueue failed: %s\n",
                fetch_scheduler_last_error(incumbent_scheduler));
    }
    CHECK(browser_engine_pointer_event(
              engine, CONTROLLER_POINTER_DOWN,
              pointer_device_x, pointer_device_y,
              &pointer_activate, &pointer_page_changed)
          && incumbent_controller->pointer_down_active);
    CHECK(incumbent_request_id != 0
          && fetch_scheduler_pending(incumbent_scheduler) == 1
          && browser_engine_begin_navigation_url(
              engine, "https://cancel.test/candidate",
              64u * 1024u, 1000, true)
          && fetch_scheduler_pending(incumbent_scheduler) == 0
          && browser_engine_navigation_pending(engine)
          && browser_engine_controller(engine) == incumbent_controller
          && !incumbent_controller->pointer_down_active
          && browser_engine_navigation(engine)->page.loaded);
    bool incumbent_request_succeeded = true;
    FetchResult incumbent_result = {.budget = browser_engine_budget(engine)};
    CHECK(fetch_scheduler_take(
              incumbent_scheduler, incumbent_request_id,
              &incumbent_request_succeeded, &incumbent_result)
          && !incumbent_request_succeeded
          && strstr(
                 incumbent_result.error,
                 "superseded by a new navigation") != NULL);
    fetch_result_destroy(&incumbent_result);
    BrowserViewSnapshot pending_view = {0};
    CHECK(browser_engine_view_snapshot(engine, &pending_view)
          && pending_view.loading
          && strcmp(
                 pending_view.url,
                 "https://cancel.test/candidate") == 0
          && strcmp(pending_view.title, "Opening page") == 0
          && pending_view.secure);
    browser_engine_cancel_navigation(engine, "unit-test cancellation");
    BrowserNavigationJobMetrics navigation_job = {0};
    CHECK(browser_engine_navigation_status(engine)
              == BROWSER_NAVIGATION_JOB_CANCELLED
          && browser_engine_navigation_job_metrics(
                 engine, &navigation_job)
          && navigation_job.status == BROWSER_NAVIGATION_JOB_CANCELLED
          && navigation_job.incumbent_pages_preserved == 1
          && browser_engine_navigation(engine)->history_count
                 == incumbent_history_count
          && browser_engine_controller(engine) == incumbent_controller
          && strcmp(browser_engine_navigation(engine)->page.document.title,
                    "Engine") == 0);
    CHECK(browser_engine_pointer_event(
              engine, CONTROLLER_POINTER_UP,
              pointer_device_x, pointer_device_y,
              &pointer_activate, &pointer_page_changed)
          && !pointer_activate);
    BrowserRenderJobStatus frame_job = browser_engine_render_frame_bounded(
        engine, 10000, 1);
    CHECK(frame_job == BROWSER_RENDER_JOB_PENDING
          || frame_job == BROWSER_RENDER_JOB_COMPLETE);
    while (frame_job == BROWSER_RENDER_JOB_PENDING) {
        frame_job = browser_engine_render_frame_bounded(
            engine, 10000, 2);
    }
    CHECK(frame_job == BROWSER_RENDER_JOB_COMPLETE
          && !tile_cache_frame_work_pending(render_shell));
    TilefinchCancellation render_cancellation;
    tilefinch_cancellation_init(&render_cancellation);
    tilefinch_cancellation_request(&render_cancellation);
    CHECK(browser_engine_render_frame_bounded_cancelable(
              engine, 10000, 1, &render_cancellation)
              == BROWSER_RENDER_JOB_CANCELLED
          && !tile_cache_frame_work_pending(render_shell));

    BrowserEngineMetrics metrics;
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.state == BROWSER_ENGINE_ACTIVE
          && metrics.page_loaded
          && metrics.controller_ready && metrics.render_ready
          && metrics.backing_kind == DOCUMENT_BACKING_FULL
          && metrics.loads_started == 3 && metrics.loads_committed == 2
          && metrics.rendered_frames == 7
          && metrics.budget_current != 0
          && metrics.control_bytes != 0
          && metrics.non_page_memory_reserve
                 == BROWSER_PSP_MINIMUM_NON_PAGE_RESERVE
          && metrics.session_inline_accounting_bytes
                 == sizeof(((BrowserSession *) 0)->storage)
                    + sizeof(((BrowserSession *) 0)->cookies)
                    + sizeof(((BrowserSession *) 0)->cache)
                    + sizeof(((BrowserSession *) 0)->site_adapter_state)
                    + sizeof(((BrowserSession *) 0)
                                 ->site_adapter_document_cache)
                    + sizeof(((BrowserSession *) 0)->client_hints)
          && metrics.budget_external_reserved
                 >= metrics.session_inline_accounting_bytes
          && metrics.budget_external_reserved_peak
                 >= metrics.budget_external_reserved
          && metrics.diagnostics_emitted == diagnostics.events
          && metrics.last_diagnostic_subsystem == TILEFINCH_SUBSYSTEM_RENDER
          && metrics.last_diagnostic_code == TILEFINCH_DIAGNOSTIC_OK
          && browser_engine_last_diagnostic_code(engine)
               == TILEFINCH_DIAGNOSTIC_OK
          && strcmp(diagnostics.last_name, "frame-rendered") == 0
          && diagnostics.events >= 4 && diagnostics.valid);

    /* Transitional relayout callers can keep controller focus while the
       BrowserEngine remains the sole owner of framebuffer/cache lifecycle. */
    BrowserController *owned_controller =
        browser_engine_controller(engine);
    uint16_t *owned_frame =
        browser_engine_framebuffer(engine, &frame_pixels);
    CHECK(owned_controller != NULL
          && owned_controller->focus_kind == CONTROLLER_FOCUS_LINK);
    ControllerFocusKind focus_kind = owned_controller->focus_kind;
    size_t focus_index = owned_controller->focus_index;
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation->relayout_damage_left = 0;
    navigation->relayout_damage_top = 0;
    navigation->relayout_damage_right = 480;
    navigation->relayout_damage_bottom = 64;
    CHECK(browser_engine_apply_layout_damage(engine)
          && browser_engine_render_shell(engine) == render_shell
          && browser_engine_framebuffer(engine, &frame_pixels) == owned_frame
          && browser_engine_controller(engine) == owned_controller
          && owned_controller->focus_kind == focus_kind
          && owned_controller->focus_index == focus_index
          && browser_engine_refresh_render_shell(engine)
          && browser_engine_render_shell(engine) == render_shell
          && browser_engine_framebuffer(engine, &frame_pixels) == owned_frame
          && browser_engine_controller(engine) == owned_controller
          && owned_controller->focus_kind == focus_kind
          && owned_controller->focus_index == focus_index);
    ControllerAction input_action = {0};
    char text_value[32] = {0};
    size_t text_length = 0;
    ControllerTextInputInfo text_info = {0};
    bool runtime_layout_changed = true;
    CHECK(browser_engine_activate(engine, &input_action)
          && input_action.type == CONTROLLER_ACTION_CONTROL
          && browser_engine_focus_move(engine, true)
          && browser_engine_replace_text(engine, "abc", 3)
          && browser_engine_text_value(
                 engine, text_value, sizeof(text_value), &text_length)
          && text_length == 3 && strcmp(text_value, "abc") == 0
          && browser_engine_backspace(engine)
          && browser_engine_insert_text(engine, "c", 1)
          && browser_engine_text_value(
                 engine, text_value, sizeof(text_value), &text_length)
          && text_length == 3 && strcmp(text_value, "abc") == 0
          && browser_engine_activate(engine, &input_action)
          && input_action.type == CONTROLLER_ACTION_CONTROL
          && browser_engine_execute_action(
                 engine, &input_action, 1024, 1000)
          && browser_engine_scroll_by(engine, 0)
          && browser_engine_advance_runtime(
                 engine, 16, 8, &runtime_layout_changed)
          && !runtime_layout_changed
          && browser_engine_render_shell(engine) == render_shell
          && browser_engine_render_frame(engine, NULL));
    size_t no_visual_generation = navigation->incremental_relayouts;
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "setTimeout(()=>document.body.style.color='rgb(0,0,0)',0)",
              "<no-visual-runtime-mutation>",
              &navigation->page.script_result)
          && browser_engine_advance_runtime(
                 engine, 16, 8, &runtime_layout_changed)
          && navigation->incremental_relayouts == no_visual_generation + 1
          && !navigation->relayout_damage_valid
          && !runtime_layout_changed);
    BrowserEngineMetrics unchanged_metrics = {0};
    CHECK(browser_engine_metrics(engine, &unchanged_metrics)
          && unchanged_metrics.unchanged_runtime_frames_suppressed == 1);
    CHECK(browser_engine_text_input_info(engine, &text_info)
          && text_info.editable && text_info.voice_allowed
          && !text_info.keyboard_url_mode && !text_info.multiline
          && browser_engine_focus_move(engine, true)
          && browser_engine_text_input_info(engine, &text_info)
          && text_info.editable && !text_info.voice_allowed
          && browser_engine_focus_move(engine, true)
          && browser_engine_text_input_info(engine, &text_info)
          && text_info.editable && !text_info.voice_allowed);
    const NavigationEntry *saved_view = navigation_current(navigation);
    CHECK(saved_view != NULL
          && saved_view->focus_kind
               == (int) owned_controller->focus_kind
          && saved_view->focus_index == owned_controller->focus_index);

    /* A failed candidate keeps the current page/backing usable. */
    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL);
    budget_inject_failure_after(budget, 0);
    CHECK(!browser_engine_commit_html(
        engine, "https://failed.test/", "<p>candidate", 12, true));
    CHECK(browser_engine_last_diagnostic_code(engine)
              == TILEFINCH_DIAGNOSTIC_PARSE_FAILED
          && diagnostics.last_code == TILEFINCH_DIAGNOSTIC_PARSE_FAILED
          && strcmp(diagnostics.last_name, "navigation-failed") == 0);
    budget_clear_failure_injection(budget);
    CHECK(browser_engine_navigation(engine)->page.loaded
          && document_backing_kind(browser_engine_document_backing(engine))
                 == DOCUMENT_BACKING_FULL
          && browser_engine_controller(engine) != NULL
          && browser_engine_render_shell(engine) != NULL);
    CHECK(navigation->page.runtime != NULL
          && script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "document.body.setAttribute('data-writer-before','local');"
                 "globalThis.pocSummary=__tilefinchHasRemoteNodeWriter?"
                 "'writer-on':'writer-off'",
                 "<writer-before-backing-bind>",
                 &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary,
                    "writer-off") == 0);

    static const char section_source[] =
        "<!doctype html><title>Sections</title><body>"
        "<h1 id=one>One</h1><div id=probe>first</div>"
        "<h1 id=two>Two</h1><p>second</p></body>";
    CompressedSectionStore store;
    SectionPager pager;
    CHECK(section_store_build(&store, budget, section_source,
                              sizeof(section_source) - 1, 4096, 8192));
    CHECK(section_pager_init(&pager, &store, 0, 0));
    CHECK(store.section_count > 1);
    BackingProbe *probe = calloc(1, sizeof(*probe));
    CHECK(probe != NULL);
    DocumentBackingOps ops = {
        .selector_lookup = backing_selector_lookup,
        .node_write = backing_node_write
    };
    DocumentBacking sections;
    CHECK(document_backing_init_sections(
              &sections, &store, &pager, &ops, probe)
          && document_backing_section_count(&sections) == store.section_count
          && document_backing_current_html(&sections, NULL) != NULL
          && browser_engine_bind_document_backing(engine, &sections));
    size_t section_index = SIZE_MAX;
    char tag_name[32] = {0}, identifier[129] = {0}, stable_key[96] = {0};
    CHECK(navigation->remote_selector_lookup != NULL
          && navigation->remote_selector_lookup(
              navigation->remote_selector_opaque, "#probe", 6,
              &section_index, tag_name, identifier, stable_key)
          && probe->selector_calls == 1 && section_index == 0
          && strcmp(tag_name, "div") == 0
          && strcmp(stable_key, "id:probe") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "document.body.setAttribute('data-live-writer','on');"
              "globalThis.pocSummary=__tilefinchHasRemoteNodeWriter?"
              "'writer-on':'writer-off'",
              "<writer-after-backing-bind>",
              &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary,
                    "writer-on") == 0
          && probe->writer_calls == 1
          && probe->writer_section == 0
          && probe->writer_kind == SCRIPT_REMOTE_NODE_WRITE_SET_ATTRIBUTE
          && strcmp(probe->writer_key, "d:body") == 0
          && strcmp(probe->writer_name, "data-live-writer") == 0
          && strcmp(probe->writer_value, "on") == 0);

    /* Binding a compressed backing after the page runtime already exists
       must update that live top-level realm. The lookup queues a bounded
       materialization request because this backing has no remote read op. */
    probe->section_index = 1;
    static const char post_commit_bind_probe[] =
        "globalThis.__tilefinchBoundRemote=document.querySelector('aside');"
        "globalThis.pocSummary=__tilefinchBoundRemote?'bound':'missing'";
    bool post_commit_bound = navigation->page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
               navigation->page.runtime, post_commit_bind_probe,
               "<post-commit-backing-bind>",
               &navigation->page.script_result);
    if (!post_commit_bound
        || strcmp(navigation->page.script_result.summary, "bound") != 0
        || probe->selector_calls != 2) {
        fprintf(stderr, "post-commit binding ok=%d calls=%zu summary=%s error=%s\n",
                post_commit_bound, probe->selector_calls,
                navigation->page.script_result.summary,
                navigation->page.script_result.error);
    }
    CHECK(post_commit_bound
          && strcmp(navigation->page.script_result.summary, "bound") == 0
          && probe->selector_calls == 2);
    char remote_identifier[129] = {0};
    size_t remote_section = SIZE_MAX;
    CHECK(script_runtime_take_remote_element_request(
              navigation->page.runtime, remote_identifier,
              sizeof(remote_identifier), &remote_section)
          && strcmp(remote_identifier, "id:deferred") == 0
          && remote_section == 1);

    /* Queue one more request, then detach before releasing the backing's
       context. Both callbacks and queued requests must be gone on return. */
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "document.querySelector('aside')",
              "<queued-before-backing-uninstall>",
              &navigation->page.script_result)
          && script_runtime_take_remote_element_request(
                 navigation->page.runtime, remote_identifier,
                 sizeof(remote_identifier), &remote_section)
          && remote_section == 1
          && script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "document.querySelector('aside')",
                 "<pending-at-backing-uninstall>",
                 &navigation->page.script_result));
    size_t writer_calls_before_uninstall = probe->writer_calls;
    document_backing_uninstall(navigation);
    CHECK(!script_runtime_take_remote_element_request(
              navigation->page.runtime, remote_identifier,
              sizeof(remote_identifier), &remote_section)
          && script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "document.body.setAttribute('data-writer-after','local');"
                 "globalThis.pocSummary=__tilefinchHasRemoteNodeWriter?"
                 "'writer-on':'writer-off'",
                 "<writer-after-backing-uninstall>",
                 &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary,
                    "writer-off") == 0
          && probe->writer_calls == writer_calls_before_uninstall);

    /* Section replacement now crosses the facade as one ownership command:
       the pager selection, runtime identity, backing copy, controller, and
       render shell must all describe the newly committed section. */
    DocumentBackingSelection selection = {0};
    CHECK(browser_engine_bind_document_backing(engine, &sections)
          && document_backing_prepare_section(
                 &sections, 1, 1, &selection)
          && browser_engine_commit_section_html(
                 engine, &sections, &selection,
                 "https://engine.test/#two",
                 selection.pager_selection.html,
                 selection.pager_selection.length, true, false)
          && !selection.active
          && pager.current_section == 1
          && document_backing_current_section(
                 browser_engine_document_backing(engine)) == 1
          && navigation->runtime_section_identity == 1
          && browser_engine_controller(engine) != NULL
          && browser_engine_render_shell(engine) != NULL);
    document_backing_uninstall(navigation);
    free(probe);
    probe = NULL;
    CHECK(script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "globalThis.pocSummary=document.querySelector('aside')"
                 "===null?'cleared':'stale'",
                 "<post-uninstall-backing-probe>",
                 &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary, "cleared") == 0);

    DocumentBacking full;
    CHECK(navigation_set_runtime_section_identity(navigation, 7)
          && navigation->runtime_section_identity == 7
          && document_backing_init_full(&full, &navigation->page.document)
          && browser_engine_bind_document_backing(engine, &full)
          && navigation->remote_selector_lookup == NULL
          && navigation->runtime_section_identity == 0);
    section_pager_destroy(&pager);
    section_store_destroy(&store);

    CHECK(browser_engine_shutdown(engine));
    CHECK(browser_engine_metrics(engine, &metrics)
          && metrics.state == BROWSER_ENGINE_SHUTDOWN
          && metrics.budget_current == 0
          && metrics.budget_active_allocations == 0
          && browser_engine_navigation(engine) == NULL
          && browser_engine_framebuffer(engine, &frame_pixels) == NULL
          && frame_pixels == 0);
    CHECK(diagnostics.valid && diagnostics.events != 0
          && diagnostics.last_code == TILEFINCH_DIAGNOSTIC_LIFECYCLE
          && metrics.last_diagnostic_code == TILEFINCH_DIAGNOSTIC_LIFECYCLE
          && strcmp(diagnostics.last_name, "engine-shutdown") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    return 0;
}

static int test_page_video_activation(void)
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
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><title>Media</title><body>"
        "<video controls crossorigin=use-credentials width=320 height=180>"
        "<source src=hidden.mp4 type=video/mp4 media='not all'>"
        "<source src=skip.webm type=video/webm>"
        "<source src=movie.mp4 type='video/mp4; codecs=avc1'></video>";
    ControllerAction action = {0};
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://media.test/path/page.html", page,
                 sizeof(page) - 1u, true)
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_MEDIA
          && strcmp(action.url,
                    "https://media.test/path/movie.mp4") == 0
          && action.media_node_handle > 0
          && action.media_mode == TILEFINCH_REQUEST_MODE_CORS
          && action.media_credentials == TILEFINCH_CREDENTIALS_INCLUDE
          /* Execution belongs to the platform player; the engine must never
             accidentally reinterpret a media action as navigation. */
          && browser_engine_execute_action(engine, &action, 1024, 1000)
          && strcmp(navigation_current(browser_engine_navigation(engine))->url,
                    "https://media.test/path/page.html") == 0
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_page_video_data_candidate_activation(void)
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
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><title>Media data</title><body>"
        "<video controls width=320 height=180></video>"
        "<script>var mediaDefinition=["
        "{\"quality\":720,\"videoUrl\":\"/movie-720.mp4\"},"
        "{\"quality\":240,\"videoUrl\":\"/movie-240.mp4\"},"
        "{\"quality\":144,\"videoUrl\":\"/movie-144.mp4\"}];"
        "</script></body>";
    ControllerAction action = {0};
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://media-data.test/path/page.html", page,
                 sizeof(page) - 1u, true)
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_MEDIA
          && strcmp(action.url,
                    "https://media-data.test/movie-240.mp4") == 0
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_page_audio_activation(void)
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
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><title>Audio</title><body>"
        "<audio controls crossorigin=anonymous>"
        "<source src=skip.mp4 type=video/mp4>"
        "<source src=episode.m4a type=audio/mp4></audio>"
        "<audio src=background.m4a></audio>";
    ControllerAction action = {0};
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://media.test/path/page.html", page,
                 sizeof(page) - 1u, true)
          && browser_engine_navigation(engine)->page.layout.control_count == 1
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_MEDIA
          && action.media_audio_only
          && action.media_kind == MEDIA_DISCOVERY_AUDIO_MP4
          && strcmp(action.url,
                    "https://media.test/path/episode.m4a") == 0
          && action.media_node_handle > 0
          && action.media_mode == TILEFINCH_REQUEST_MODE_CORS
          && action.media_credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_structured_audio_preview_activation(void)
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
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><title>Structured audio</title><body>"
        "<button aria-label='Reproducir First'>Preview</button>"
        "<div href=/items/second><button>Play</button></div>"
        "<button>Play</button>"
        "<button id=after-mutation aria-label='Play First'>Preview</button>"
        "<button id=handled type=button>Play</button>"
        "<script id=catalog type=application/ld+json>["
        "{\"@type\":[\"Thing\",\"AudioObject\"],\"name\":\"First\","
        "\"url\":\"/items/first\",\"contentUrl\":\"/audio/first.m4a\"},"
        "{\"audio\":[{\"@type\":\"AudioObject\",\"name\":\"Second\","
        "\"contentUrl\":\"/audio/second.mp4\"}],"
        "\"name\":\"Album row\",\"url\":\"/items/second\","
        "\"@type\":[\"CreativeWork\",\"MusicRecording\"]},"
        "{\"contentUrl\":\"/not-a-media-context.m4a\"}]</script>"
        "<script>document.getElementById('handled').addEventListener("
        "'click',()=>{globalThis.pocSummary='handled'})</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://audio.test/album", page,
                 sizeof(page) - 1u, true));
    MediaStructuredAudioIndex index = {0};
    CHECK(media_discover_structured_audio(
              &browser_engine_navigation(engine)->page.document, &index)
          && index.candidate_count == 2u
          && index.inspected_bytes != 0u
          && index.inspected_nodes != 0u);
    ControllerAction first = {0}, second = {0}, ambiguous = {0};
    ControllerAction after_mutation = {0}, handled = {0};
    CHECK(browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &first)
          && first.type == CONTROLLER_ACTION_MEDIA
          && first.media_audio_only && first.media_node_handle == 0
          && first.media_kind == MEDIA_DISCOVERY_AUDIO_MP4
          && strcmp(first.url, "https://audio.test/audio/first.m4a") == 0);
    CHECK(browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &second)
          && second.type == CONTROLLER_ACTION_MEDIA
          && second.media_audio_only && second.media_node_handle == 0
          && strcmp(second.url, "https://audio.test/audio/second.mp4") == 0);
    CHECK(browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &ambiguous)
          && ambiguous.type == CONTROLLER_ACTION_CONTROL);
    ScriptResult mutation = {0};
    CHECK(script_runtime_evaluate_diagnostic(
              browser_engine_navigation(engine)->page.runtime,
              "document.getElementById('catalog').remove();"
              "globalThis.pocSummary='catalog-removed'",
              "<structured-audio-mutation>", &mutation)
          && mutation.success
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &after_mutation)
          && after_mutation.type == CONTROLLER_ACTION_CONTROL);
    CHECK(browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &handled)
          && handled.type == CONTROLLER_ACTION_CONTROL
          && strcmp(browser_engine_navigation(engine)->page.script_result.summary,
                    "handled") == 0
          && browser_engine_navigation(engine)->page.script_result
                 .event_handlers_invoked != 0u
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    engine = browser_engine_create(&config, error, sizeof(error));
    static const char blocked[] =
        "<!doctype html><body><button>Play</button>"
        "<script type=application/ld+json>"
        "{\"@type\":\"AudioObject\","
        "\"contentUrl\":\"https://cdn.test/blocked.m4a\"}</script>";
    ControllerAction blocked_action = {0};
    bool blocked_committed = engine != NULL && browser_engine_commit_html(
        engine, "https://audio.test/blocked", blocked,
        sizeof(blocked) - 1u, true);
    static const char blocked_csp[] =
        "content-security-policy: media-src 'none'\n";
    blocked_committed = blocked_committed
        && tilefinch_csp_parse_response_headers(
            &browser_engine_navigation(engine)->page.document
                 .content_security_policy,
            "https://audio.test/blocked", blocked_csp,
            sizeof(blocked_csp) - 1u, false);
    bool blocked_focused = blocked_committed
        && browser_engine_focus_move(engine, true);
    bool blocked_activated = blocked_focused
        && browser_engine_activate(engine, &blocked_action);
    CHECK(blocked_committed && blocked_focused && blocked_activated
          && blocked_action.type == CONTROLLER_ACTION_CONTROL
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_nomodule_capability_suppression(void)
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
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><body><script type=module>"
        "globalThis.modulePath='module'</script>"
        "<script nomodule>globalThis.modulePath='legacy'</script>"
        "<script nomodule src=https://never-fetched.invalid/legacy.js>"
        "</script><script>const dynamic=document.createElement('script');"
        "dynamic.noModule=true;dynamic.src='https://never-fetched.invalid/"
        "dynamic.js';document.body.appendChild(dynamic);"
        "document.addEventListener('DOMContentLoaded',()=>{"
        "globalThis.pocSummary=modulePath+':' +"
        "String(globalThis.dynamicLegacy||false)})</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://modules.test/", page,
                 sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(strcmp(navigation->page.script_result.summary,
                 "module:false") == 0
          && navigation->script_skipped_nomodule == 2u
          && navigation->page.script_result.dynamic_scripts_nomodule_skipped
                 == 1u
          && navigation->script_attempted == 0u
          && navigation->preloads_discovered == 0u
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    engine = browser_engine_create(&config, error, sizeof(error));
    static const char lone[] =
        "<!doctype html><body><script nomodule>"
        "globalThis.legacyRan=true</script><script>"
        "globalThis.pocSummary=String(globalThis.legacyRan||false)"
        "</script></body>";
    CHECK(engine != NULL
          && browser_engine_commit_html(
                 engine, "https://modules.test/lone", lone,
                 sizeof(lone) - 1u, true));
    navigation = browser_engine_navigation(engine);
    CHECK(strcmp(navigation->page.script_result.summary, "false") == 0
          && navigation->script_skipped_nomodule == 1u
          && navigation->script_attempted == 0u
          && navigation->preloads_discovered == 0u
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static bool computed_style_test_flush(void *opaque)
{
    return navigation_relayout(opaque);
}

static int test_computed_paint_style_does_not_force_layout(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.javascript.enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><style>body{margin:0}#target{width:100px}"
        ".toggled{font-weight:700;white-space:nowrap}</style>"
        "<div id=target>Paint-only style reads during hydration</div>";
    CHECK(engine != NULL && browser_engine_commit_html(engine,
        "https://style-read.test/", page, sizeof(page) - 1, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    script_runtime_set_synchronous_layout_callback(navigation->page.runtime,
        computed_style_test_flush, navigation);
    size_t before = navigation->incremental_relayouts;
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "var target=document.getElementById('target');"
        "for(var i=0;i<8;i++){target.style.color='rgb('+i+', 0, 0)';"
        "if(getComputedStyle(target).color!=='rgb('+i+', 0, 0)')throw Error('color');}"
        "target.style.visibility='hidden';"
        "if(getComputedStyle(target).visibility!=='hidden')throw Error('visibility');"
        /* Text properties resolve from the cascade alone as well. */
        "target.style.lineHeight='30px';target.style.textAlign='center';"
        "target.className='toggled';"
        "var read=getComputedStyle(target);"
        "if(read.lineHeight!=='30px'||read.textAlign!=='center'"
        "||read.fontWeight!=='700'||read.whiteSpace!=='nowrap')"
        "throw Error('text:'+read.lineHeight+'/'+read.textAlign+'/'"
        "+read.fontWeight+'/'+read.whiteSpace);"
        "target.style.visibility='visible';target.style.width='123px';",
        "<computed-paint-read>", &result) && result.success);
    CHECK(navigation->incremental_relayouts == before);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "if(getComputedStyle(target).width!=='123px')throw Error('width');",
        "<computed-geometry-read>", &result) && result.success
        && navigation->incremental_relayouts == before + 1);
    CHECK(script_runtime_evaluate_diagnostic(navigation->page.runtime,
        "var sheet=document.createElement('style');"
        "sheet.textContent='#target{color:rgb(22, 33, 44)!important}';"
        "document.head.appendChild(sheet);"
        "if(getComputedStyle(target).color!=='rgb(22, 33, 44)')throw Error('sheet');",
        "<computed-new-sheet>", &result) && result.success
        && navigation->incremental_relayouts == before + 2);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* getComputedStyle() reports resolved values, and only for the properties
   the engine supports. Every expectation here is an exact value: a test that
   merely required "not empty" would pass on a table of initial values while
   the cascade, inheritance and used sizes were all wrong. */
static int test_computed_style_resolved_values(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.javascript.enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><style>body{margin:0}"
        "#host{font-size:20px;line-height:1.5;text-align:right;direction:rtl;"
        "letter-spacing:2px;word-spacing:3px;list-style-type:square}"
        "#imp{line-height:30px!important;text-align:center!important}"
        "#flex{display:flex}#item{flex:2 3 40%;vertical-align:middle}"
        "#pct{line-height:150%;font-size:10px}#tab{tab-size:4}"
        "#half{width:50%;height:30px;padding:0 5px}"
        "#border{width:50%;box-sizing:border-box;padding:0 5px}"
        "#pctpad{width:50%;padding:0 10%}"
        "#pseudo{width:200px;height:100px}"
        "#pseudo::before{content:'b';display:block;width:20px;height:10px}"
        "#zero{line-height:0}#zerounit{line-height:0px}"
        "#zerolines{line-height:0}"
        "#zerofont{font:10px/0 serif}"
        "#gone{display:none}#gonesized{display:none;width:40px}"
        "#grand{width:800px;padding:0 10%}"
        "#narrow{width:400px;padding:0 10%}"
        "#nested{padding-left:10%;width:50%}"
        "#padhost{width:400px;padding:0 50px}"
        "#padhost::before{content:'b';display:block;padding-left:10%;"
        "width:20px}"
        "#ttop{vertical-align:text-top}#tbot{vertical-align:text-bottom}"
        "#cb{position:relative;width:400px;padding:0 20px}#cbwrap{width:100px}"
        "#cbabs{position:absolute;top:0;left:0;width:50px;padding:0 10%}"
        "#cbfix{position:fixed;top:0;left:0;width:50px;padding-left:10%}"
        "#cbin,#cbfin{padding-left:10%;width:20px}"
        "#invalidzero{line-height:23px;line-height:0banana}"
        "#invalidangle{line-height:24px;line-height:0deg}"
        "#invalidpercent{line-height:25px;line-height:0%%}"
        "#lhp::before{content:'b';font-size:10px;line-height:3}"
        "#lhq::before{content:'b';font-size:10px;line-height:150%}"
        "#calc0{line-height:calc(0)}#calc0px{line-height:calc(0px)}"
        "#calcdiff{line-height:calc(10px - 10px)}"
        "#calcsix{line-height:calc(2px * 3)}"
        "#strut{line-height:20px}#strutzero{line-height:0}"
        "#strutnext{line-height:20px}"
        "#inlwrap{width:400px}#inlpad{padding-left:10%}"
        "#badunit{line-height:23px;line-height:1banana}"
        "#badunit2{line-height:1.5;line-height:12garbage;font-size:10px}"
        "#rad{border-radius:1px 2px 3px 4px;direction:rtl;cursor:pointer;"
        "background-size:50% auto;margin:1px auto 3px 4px}"
        "#rad::before{content:'r'}"
        "#under{text-decoration:underline}"
        "#struck{text-decoration:line-through}"
        "</style>"
        "<div id=host><span id=child>x</span></div>"
        "<div id=imp style='line-height:10px;text-align:left'>y</div>"
        "<div id=flex><div id=item>z</div><div id=plainitem>w</div></div>"
        "<div id=pct>p</div><pre id=tab>t</pre><div id=plain>q</div>"
        "<div id=inl style='line-height:inherit;"
        "letter-spacing:var(--nope, 1px);word-spacing:2em'>i</div>"
        "<ol><li id=ordered>o</li></ol>"
        "<div id=half>h</div><div id=border>b</div>"
        "<div id=pctpad>p</div><div id=pseudo>s</div>"
        "<span id=sized style='width:100px;height:50px'>i</span>"
        "<div id=zero>z</div><div id=zerounit>z</div><div id=zerofont>z</div>"
        "<div id=gone>g</div><div id=gonesized>g</div>"
        "<div id=grand><div id=narrow><div id=nested>n</div></div></div>"
        "<div id=padhost>p</div>"
        "<span id=ttop>t</span><span id=tbot>t</span>"
        "<p id=under>u<span id=underchild>c</span></p>"
        "<p id=struck>s</p><a id=link href='/x'>l</a><div id=rad>r</div>"
        "<div id=cb><div id=cbwrap><div id=cbabs><div id=cbin>x</div></div>"
        "<div id=cbfix><div id=cbfin>y</div></div></div></div>"
        "<div id=strut><span id=strutzero>t</span></div>"
        "<div id=strutnext>n</div>"
        "<div id=inlwrap><span><span><span id=inlpad>p</span></span>"
        "wrapper text</span></div>"
        "<div id=badunit>b</div><div id=badunit2>b</div>"
        "<div id=lhp>p</div><div id=lhq>q</div><div id=calc0>a</div>"
        "<div id=calc0px>b</div><div id=calcdiff>c</div>"
        "<div id=calcsix>d</div><div id=invalidzero>i</div>"
        "<div id=invalidangle>a</div><div id=invalidpercent>p</div>"
        "<div id=zerolines>Visible<br>overflow<br>text</div>";
    CHECK(engine != NULL && browser_engine_commit_html(engine,
        "https://style-values.test/", page, sizeof(page) - 1, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    script_runtime_set_synchronous_layout_callback(navigation->page.runtime,
        computed_style_test_flush, navigation);
    ScriptResult result = {0};
    static const char source[] =
        "(()=>{const bad=[];"
        "const want=(id,name,expected)=>{const got=getComputedStyle("
        "document.getElementById(id)).getPropertyValue(name);"
        "if(got!==expected)bad.push(id+'.'+name+'='+JSON.stringify(got));};"
        /* Authored through a stylesheet, then inherited. A unitless
           line-height is resolved against each element's own font size. */
        "for(const id of['host','child']){want(id,'line-height','30px');"
        "want(id,'text-align','right');want(id,'direction','rtl');"
        "want(id,'letter-spacing','2px');want(id,'word-spacing','3px');"
        "want(id,'list-style-type','square');}"
        "want('child','vertical-align','baseline');"
        /* Raw inline text must never stand in for the resolved value. */
        "want('imp','line-height','30px');want('imp','text-align','center');"
        "want('inl','line-height','normal');"
        "want('inl','letter-spacing','1px');"
        "want('inl','word-spacing','32px');"
        /* Scaled integer fields decode to CSS numbers. */
        "want('item','flex-grow','2');want('item','flex-shrink','3');"
        "want('item','flex-basis','40%');"
        "want('item','vertical-align','middle');"
        "want('plainitem','flex-grow','0');"
        "want('plainitem','flex-shrink','1');"
        "want('plainitem','flex-basis','auto');"
        "want('pct','line-height','15px');want('tab','tab-size','4');"
        /* Initial values, including `normal` where layout uses a number. */
        "want('plain','line-height','normal');"
        "want('plain','text-align','start');want('plain','direction','ltr');"
        "want('plain','letter-spacing','normal');"
        "want('plain','word-spacing','0px');"
        "want('plain','vertical-align','baseline');"
        "want('plain','writing-mode','horizontal-tb');"
        "want('plain','unicode-bidi','normal');want('plain','tab-size','8');"
        "want('plain','table-layout','auto');"
        "want('plain','list-style-type','disc');"
        "want('plain','aspect-ratio','auto');"
        "want('ordered','list-style-type','decimal');"
        /* An out-of-flow box resolves percentages against its containing
           block: the positioned ancestor's padding box (400 + 2 * 20), not
           the 100px static wrapper it sits in; a fixed box uses the initial
           containing block. Layout must place the content where CSSOM says
           the padding ends. */
        "want('cbabs','padding-left','44px');"
        "want('cbabs','padding','0px 44px 0px 44px');"
        "want('cbin','padding-left','5px');"
        "want('cbfin','padding-left','5px');"
        "want('invalidzero','line-height','23px');"
        "want('invalidangle','line-height','24px');"
        "want('invalidpercent','line-height','25px');"
        "const inset=(outer,inner)=>document.getElementById(inner)"
        ".getBoundingClientRect().left-document.getElementById(outer)"
        ".getBoundingClientRect().left;"
        "if(inset('cbabs','cbin')!==44)bad.push('abs layout='+"
        "inset('cbabs','cbin'));"
        "const fixedPad=getComputedStyle(document.getElementById('cbfix'))"
        ".paddingLeft;if(fixedPad!==Math.round(innerWidth/10)+'px'"
        "||inset('cbfix','cbfin')!==parseInt(fixedPad))"
        "bad.push('fixed='+fixedPad+' layout='+inset('cbfix','cbfin'));"
        /* A pseudo-element resolves a relative line-height against its own
           font size, like an element. */
        "for(const[id,expected]of[['lhp','30px'],['lhq','15px']]){"
        "const got=getComputedStyle(document.getElementById(id),'::before')"
        ".lineHeight;if(got!==expected)bad.push(id+'::before='+got);}"
        /* A zero line-height run must not collapse its parent's line: the
           block keeps its own line-height as a strut, so the next block
           starts below it. */
        "{const strut=document.getElementById('strut').getBoundingClientRect(),"
        "next=document.getElementById('strutnext').getBoundingClientRect();"
        "if(strut.height!==20||next.top!==strut.top+20)"
        "bad.push('strut='+strut.height+'/'+next.top);}"
        /* Inline wrappers are not containing blocks: the percentage refers
           to the nearest block container (400px), not the span's box. */
        "want('inlpad','padding-left','40px');"
        /* An unknown unit is invalid and must not overwrite the earlier
           valid declaration, nor read as pixels. */
        "want('badunit','line-height','23px');"
        "want('badunit2','line-height','15px');"
        /* A calculated zero is an authored zero, not `normal`. */
        "want('calc0','line-height','0px');"
        "want('calc0px','line-height','0px');"
        "want('calcdiff','line-height','0px');"
        "want('calcsix','line-height','6px');"
        /* A logical radius that was not authored maps to its physical
           corner: under rtl the inline start is the right-hand side. */
        "want('rad','border-start-start-radius','2px');"
        "want('rad','border-start-end-radius','1px');"
        "want('rad','border-end-start-radius','3px');"
        "want('rad','border-end-end-radius','4px');"
        "want('rad','background-size','50% auto');"
        "want('rad','margin','1px auto 3px 4px');"
        /* A pseudo-element has the sparse properties too: inherited ones
           come from its originating element, the rest are initial. */
        "const radBefore=getComputedStyle(document.getElementById('rad'),"
        "'::before');"
        "for(const[name,expected]of[['cursor','pointer'],['tab-size','8'],"
        "['user-select','auto'],['scroll-margin-top','0px'],"
        "['border-end-end-radius','0px']])"
        "if(radBefore.getPropertyValue(name)!==expected)bad.push('before.'+"
        "name+'='+JSON.stringify(radBefore.getPropertyValue(name)));"
        /* Keywords layout treats as baseline still report as authored. */
        "want('ttop','vertical-align','text-top');"
        "want('tbot','vertical-align','text-bottom');"
        /* text-decoration-line is not inherited: a child of an underlined
           element draws the ancestor's line but declares none. Only the
           underline is retained; other lines are not painted either. */
        "want('under','text-decoration-line','underline');"
        "want('underchild','text-decoration-line','none');"
        "want('link','text-decoration-line','underline');"
        "want('plain','text-decoration-line','none');"
        "want('struck','text-decoration-line','none');"
        /* Width and height resolve to used pixels when there is a box. */
        /* No viewport declaration, so the page lays out at the 980 px
           fallback width. A percentage comes back in pixels. `width` names
           the content box by default and the border box under
           box-sizing:border-box, so both halves report 490px even though
           one of them also has 10px of padding outside that. */
        "want('plain','width','980px');"
        "want('half','width','490px');want('half','height','30px');"
        "want('border','width','490px');"
        "const auto=getComputedStyle(document.getElementById('plain')).height;"
        "if(!/^[1-9][0-9]*px$/.test(auto))bad.push('plain.height='+auto);"
        "if(getComputedStyle(document.getElementById('child')).width!=='auto')"
        "bad.push('inline width');"
        /* Percentage padding is a packed value until it is resolved against
           the containing block: 10% of 980 on each side. Subtracting the
           packed value once made this width 0px. */
        "want('pctpad','width','490px');"
        "want('pctpad','padding-left','98px');"
        "want('pctpad','padding','0px 98px 0px 98px');"
        "if(document.getElementById('pctpad').getBoundingClientRect().width"
        "!==686)bad.push('pctpad box');"
        /* Percentages chain: each level resolves against its own
           containing block's content box. #grand's 10% is of the 980px page;
           #narrow's is of #grand's 800px content box, not of its own 560px
           padding box (which is what a one-level approximation used, giving
           #nested 44px); #nested's is of #narrow's 400px content box. */
        "want('grand','padding-left','98px');want('grand','width','800px');"
        "want('narrow','padding-left','80px');want('narrow','width','400px');"
        "want('nested','padding-left','40px');want('nested','width','200px');"
        /* A pseudo-element's containing block is its originating element's
           content box (400px), not that element's parent (980px). */
        "const padded=getComputedStyle("
        "document.getElementById('padhost'),'::before');"
        "if(padded.paddingLeft!=='40px'||padded.width!=='20px')"
        "bad.push('pseudo padding='+padded.paddingLeft+'/'+padded.width);"
        /* A pseudo-element is not its originating element's box. */
        "const before=getComputedStyle("
        "document.getElementById('pseudo'),'::before');"
        "if(before.width!=='20px'||before.height!=='10px')"
        "bad.push('pseudo='+before.width+'x'+before.height);"
        "want('pseudo','width','200px');want('pseudo','height','100px');"
        /* Width and height do not apply to a non-replaced inline, so its
           authored values come back rather than its text box. */
        "want('sized','width','100px');want('sized','height','50px');"
        /* An authored zero is not the `normal` keyword. */
        "want('zero','line-height','0px');"
        "want('zerounit','line-height','0px');"
        "want('zerofont','line-height','0px');"
        "want('zerolines','height','0px');"
        /* No box: the computed value, which is `auto` when nothing is set. */
        "want('gone','width','auto');want('gone','height','auto');"
        "want('gonesized','width','40px');"
        /* The supported set: one list, no shorthands or aliases in it. */
        "const style=getComputedStyle(document.getElementById('plain'));"
        "const names=[...style];"
        "if(names.length!==style.length||names.length<150)"
        "bad.push('length='+style.length);"
        "if(names.join()!==[...names].sort().join()"
        "||new Set(names).size!==names.length)bad.push('order');"
        "for(const name of['flex','padding','overflow','gap',"
        "'-webkit-user-select'])"
        "if(names.includes(name))bad.push('enumerated '+name);"
        "for(const name of['height','lineHeight','line-height','cssFloat',"
        "'flexGrow','flex','--anything'])"
        "if(!(name in style))bad.push('missing '+name);"
        "if('bogusProperty' in style)bad.push('bogus');"
        "if(style[0]!==names[0]||style.item(names.length)!==''"
        "||Object.keys(style).length!==names.length)bad.push('indexed');"
        "const empty=names.filter(name=>style.getPropertyValue(name)==='');"
        "if(empty.length)bad.push('empty:'+empty.join('/'));"
        /* Registered but deliberately not enumerated: shorthands and legacy
           aliases are still attributes of the declaration, with values. */
        "for(const name of['border-image','border-radius','border-width',"
        "'flex','gap','overflow','overscroll-behavior','padding',"
        "'place-content','place-items','place-self','scroll-margin',"
        "'scroll-padding','text-wrap','-webkit-appearance',"
        "'-webkit-backdrop-filter','-webkit-user-select',"
        "'-webkit-text-size-adjust'])"
        "if(!(name in style)||style.getPropertyValue(name)===''"
        "||names.includes(name))bad.push('unlisted '+name);"
        /* The object is live across attachment, mutation and detachment,
           and a disconnected element has an empty declaration block. */
        "const node=document.createElement('div'),live=getComputedStyle(node);"
        "if(live.length!==0||live.display!==''||live.item(0)!==''"
        "||[...live].length!==0||!('display' in live))bad.push('detached');"
        "document.body.appendChild(node);"
        "if(live.length!==names.length||live.display!=='block'"
        "||live.textAlign!=='start')bad.push('attached');"
        "node.style.textAlign='center';"
        "if(live.textAlign!=='center')bad.push('mutated');"
        "node.remove();"
        "if(live.length!==0||live.display!=='')bad.push('removed');"
        "if(bad.length)throw Error(bad.join(' | '));})()";
    bool evaluated = script_runtime_evaluate_diagnostic(
        navigation->page.runtime, source, "<computed-resolved-values>",
        &result);
    if (!evaluated || !result.success)
        fprintf(stderr, "computed style: %s\n", result.error);
    CHECK(evaluated && result.success);
    /* Zero line-height overlaps the lines; it must not erase their glyph
       commands or advance the three baselines as normal-height lines. */
    size_t zero_ink_count = 0;
    int zero_ink_y = 0;
    for (size_t i = 0; i < navigation->page.layout.count; i++) {
        const DrawCommand *command = &navigation->page.layout.commands[i];
        if (command->type != DRAW_TEXT
            || (command->radius & LAYOUT_TEXT_ZERO_LINE_HEIGHT) == 0)
            continue;
        if ((command->text_length == 7
             && memcmp(command->text, "Visible", 7) == 0)
            || (command->text_length == 8
                && memcmp(command->text, "overflow", 8) == 0)
            || (command->text_length == 4
                && memcmp(command->text, "text", 4) == 0)) {
            CHECK(command->height > 0 && command->width > 0);
            if (zero_ink_count == 0) zero_ink_y = command->y;
            CHECK(command->y == zero_ink_y);
            zero_ink_count++;
        }
    }
    CHECK(zero_ink_count == 3);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Every property the computed-style registry answers for, read from a fixture
   page which authors most of them, across ordinary elements, replaced and
   form content, a table cell, both pseudo-elements, display:none, and an SVG
   shape. The checked-in golden makes any change to a resolved value a
   reviewable diff; with `golden_path` NULL the dump is only written, which is
   how the golden is regenerated:

     tilefinch-browser-engine-tests --computed-style-dump FIXTURE OUTPUT */
static int computed_style_dump(const char *fixture_path,
                               const char *output_path,
                               const char *golden_path)
{
    static char page[16384];
    FILE *fixture = fopen(fixture_path, "rb");
    CHECK(fixture != NULL);
    size_t page_length = fread(page, 1, sizeof(page), fixture);
    fclose(fixture);
    CHECK(page_length > 0 && page_length < sizeof(page));

    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.javascript.enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_commit_html(engine,
        "https://style-dump.test/", page, page_length, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    script_runtime_set_synchronous_layout_callback(navigation->page.runtime,
        computed_style_test_flush, navigation);

    /* Enumerable longhands come from the declaration itself. The rest are
       names enumeration omits: registered shorthands and aliases, SVG
       presentation attributes, custom properties, and names nothing
       supports. Missing one here only narrows the dump. */
    static const char setup[] =
        "(()=>{const names=[...getComputedStyle(document.body)].concat(["
        "'-webkit-appearance','-webkit-backdrop-filter',"
        "'-webkit-text-size-adjust','-webkit-user-select','border-image',"
        "'border-radius','border-width','flex','gap','overflow',"
        "'overscroll-behavior','padding','place-content','place-items',"
        "'place-self','scroll-margin','scroll-padding','text-wrap',"
        "'transition-duration','fill','stroke','stroke-width',"
        "'--accent','--gap','--local','--missing','bogus-property','margin',"
        "'border','font','background']);"
        "const targets=[];for(const node of document.querySelectorAll('[id]'))"
        "targets.push([node.id,null]);"
        "targets.push(['host','::before'],['host','::after'],"
        "['plain','::before'],['lhpseudo','::before']);"
        "globalThis.__dump={names,targets};"
        "globalThis.pocSummary=names.length+' '+targets.length;})()";
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime, setup, "<computed-style-dump>",
              &result) && result.success);
    unsigned name_count = 0, target_count = 0;
    CHECK(sscanf(result.summary, "%u %u", &name_count, &target_count) == 2
          && name_count >= 150 && target_count >= 10);

    FILE *output = fopen(output_path, "wb");
    CHECK(output != NULL);
    const unsigned chunk = 8;
    for (unsigned target = 0; target < target_count; target++) {
        for (unsigned first = 0; first < name_count; first += chunk) {
            char source[512];
            snprintf(source, sizeof(source),
                "(()=>{const[id,pseudo]=__dump.targets[%u],"
                "style=getComputedStyle(document.getElementById(id),pseudo);"
                "globalThis.pocSummary=(%u?'':'## '+id+(pseudo||'')+'\\n')+"
                "__dump.names.slice(%u,%u).map(name=>name+': '+"
                "JSON.stringify(style.getPropertyValue(name))).join('\\n');"
                "})()", target, first, first, first + chunk);
            bool evaluated = script_runtime_evaluate_diagnostic(
                navigation->page.runtime, source, "<computed-style-dump>",
                &result);
            if (!evaluated || !result.success)
                fprintf(stderr, "computed style dump: %s\n", result.error);
            /* A full summary buffer means a truncated chunk. */
            CHECK(evaluated && result.success
                  && strlen(result.summary) + 1 < sizeof(result.summary));
            fprintf(output, "%s\n", result.summary);
        }
    }
    fclose(output);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    if (golden_path == NULL) return 0;

    FILE *actual = fopen(output_path, "rb");
    FILE *golden = fopen(golden_path, "rb");
    CHECK(actual != NULL && golden != NULL);
    char actual_line[1024], golden_line[1024];
    char heading[128] = "";
    unsigned differences = 0;
    for (;;) {
        char *got = fgets(actual_line, sizeof(actual_line), actual);
        char *want = fgets(golden_line, sizeof(golden_line), golden);
        if (got == NULL && want == NULL) break;
        if (got != NULL && got[0] == '#')
            snprintf(heading, sizeof(heading), "%s", got);
        if (got == NULL || want == NULL || strcmp(got, want) != 0) {
            if (differences++ < 20)
                fprintf(stderr, "computed style golden: under %s  want %s"
                        "  got  %s", heading,
                        want == NULL ? "(end)\n" : want,
                        got == NULL ? "(end)\n" : got);
            if (got == NULL || want == NULL) break;
        }
    }
    fclose(actual);
    fclose(golden);
    if (differences != 0)
        fprintf(stderr, "computed style golden: %u line(s) differ; if the "
                "change is intended, regenerate with --computed-style-dump\n",
                differences);
    CHECK(differences == 0);
    return 0;
}

/* A frame taking part in a Cloudflare managed challenge gets a larger script
   heap, because the challenge interpreter does not fit the ordinary one. The
   signals are the top-level response's own `cf-mitigated: challenge` header
   and the challenge platform's document URL, so nothing a page writes can ask
   for the raise, and the top-level document never gets it. */
static int test_managed_challenge_script_heap(void)
{
    static const char page[] =
        "<!doctype html><title>Just a moment...</title>"
        "<script>globalThis.ran=true;</script><p>Verifying</p>";
    static const char platform_frame[] =
        "https://challenges.cloudflare.com/cdn-cgi/challenge-platform/h/g/"
        "turnstile/f/av0/rch/x/0x4AAAA/light/fbE/new/normal?lang=auto";
    static const char other_frame[] = "https://ads.example.test/frame";
    const size_t configured = 7u * MIB;
    const size_t raised = NAVIGATION_MANAGED_CHALLENGE_JS_HEAP_BYTES;
    const struct {
        const char *url;
        long status;
        const char *cf_mitigated;
        bool challenge_page;
    } cases[] = {
        { "https://ordinary-heap.test/", 200, "", false },
        /* A 403 from the same CDN that is not a challenge. */
        { "https://blocked-heap.test/", 403, "", false },
        { "https://challenge-heap.test/", 403, "challenge", true },
    };
    for (size_t at = 0; at < sizeof(cases) / sizeof(cases[0]); at++) {
        BrowserDeviceProfile profile;
        browser_device_profile_psp3000(&profile);
        BrowserConfig config;
        browser_config_init(&config, &profile);
        config.javascript.enabled = true;
        config.javascript.heap_limit = configured;
        char error[256] = {0};
        BrowserEngine *engine =
            browser_engine_create(&config, error, sizeof(error));
        char fixture[128];
        CHECK(engine != NULL
              && write_document_replay(fixture, cases[at].url, page,
                                       sizeof(page) - 1u, cases[at].status,
                                       cases[at].cf_mitigated)
              && fetch_trace_replay_begin(fixture, error, sizeof(error)));
        (void) browser_engine_load_url_with_limits(
            engine, cases[at].url, 1u * MIB, 1000, true);
        fetch_trace_end();
        NavigationSession *navigation = browser_engine_navigation(engine);
        ScriptRuntime *runtime =
            navigation == NULL ? NULL : navigation->page.runtime;
        CHECK(runtime != NULL);
        /* The top-level document keeps the configured heap even when it is
           the challenge page: its own script fits the ordinary limit. */
        CHECK(script_runtime_heap_remaining(runtime) <= configured);
        CHECK(navigation->managed_challenge_heap_raises == 0);
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL, cases[at].url)
              == configured);
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL, platform_frame)
              == configured);
        /* Every frame of a challenge page takes part in the challenge. */
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME, other_frame)
              == (cases[at].challenge_page ? raised : configured));
        /* The platform's own frame is raised wherever it is embedded, which
           is how a Turnstile widget on an ordinary page is covered. */
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME,
                  platform_frame) == raised);
        /* Never lowers, never stacks, and leaves disabled scripting alone. */
        size_t saved = navigation->js_memory_limit;
        navigation->js_memory_limit = 16u * MIB;
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME,
                  platform_frame) == 16u * MIB);
        navigation->js_memory_limit = 0;
        CHECK(navigation_script_heap_limit(
                  navigation, SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME,
                  platform_frame) == 0);
        navigation->js_memory_limit = saved;
        remove_large_youtube_replay(fixture);
        CHECK(browser_engine_shutdown(engine));
        browser_engine_destroy(engine);
    }
    /* Scheme, host and path prefix all have to match. */
    CHECK(navigation_url_is_challenge_platform(platform_frame));
    CHECK(!navigation_url_is_challenge_platform(
        "https://challenges.cloudflare.com/other"));
    CHECK(!navigation_url_is_challenge_platform(
        "https://challenges.cloudflare.com.evil.test/cdn-cgi/"
        "challenge-platform/"));
    CHECK(!navigation_url_is_challenge_platform(
        "http://challenges.cloudflare.com/cdn-cgi/challenge-platform/"));
    CHECK(!navigation_url_is_challenge_platform(
        "https://example.test/?next=https://challenges.cloudflare.com/"
        "cdn-cgi/challenge-platform/"));
    CHECK(!navigation_url_is_challenge_platform(NULL));
    CHECK(navigation_script_heap_limit(
              NULL, SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME, platform_frame) == 0);
    return 0;
}

static int test_native_text_sync_does_not_relayout_twice(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] =
        "<!doctype html><form action='/find'><input name=q type=search "
        "onclick='event.preventDefault()'>"
        "</form><script>let seen='';document.querySelector('input')"
        ".addEventListener('input',e=>{seen=e.target.value});</script>";
    CHECK(browser_engine_commit_html(engine, "https://search.test/", page,
              sizeof(page) - 1u, true) && browser_engine_focus_move(engine, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    size_t before = navigation->incremental_relayouts;
    CHECK(browser_engine_replace_text(engine, "psp", 3));
    CHECK(navigation->incremental_relayouts == before + 1u);
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && strcmp(action.url, "https://search.test/find?q=psp") == 0);
    /* Implicit submission still honors the default button's click and the
       form's submit cancellation; only the input's duplicate click is gone. */
    static const char cancelled_button[] =
        "<!doctype html><form action='/find'><input name=q type=search>"
        "<button onclick='event.preventDefault()'>Find</button></form>";
    CHECK(browser_engine_commit_html(engine, "https://search.test/", cancelled_button,
              sizeof(cancelled_button) - 1u, true)
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome == CONTROLLER_ACTIVATION_CANCELLED);
    static const char cancelled_form[] =
        "<!doctype html><form action='/find' onsubmit='event.preventDefault()'>"
        "<input name=q type=search></form>";
    CHECK(browser_engine_commit_html(engine, "https://search.test/", cancelled_form,
              sizeof(cancelled_form) - 1u, true)
          && browser_engine_focus_move(engine, true)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome == CONTROLLER_ACTIVATION_CANCELLED);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

typedef struct {
    uint64_t now_us;
    unsigned publications;
    unsigned ends;
} OptionalFontInputProbe;

typedef struct {
    uint64_t last_us, maximum_gap_us;
    uint64_t interrupt_delay_us, requested_us, acknowledged_us;
    size_t checkpoints;
    char maximum_gap_phase[48];
    char last_phase[48], maximum_gap_from[48];
} FontPublicationTiming;

/* The engine paints authored focus only. Native focus is composited by the
   frontend; use the same bounded overlay as the interactive host lab. */
static bool paint_host_focus(BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    const NavigationEntry *entry = navigation_current(navigation);
    int x, y, width, height;
    if (entry == NULL || !controller_focused_rect(browser_engine_controller(engine),
            &x, &y, &width, &height)) return false;
    const ViewportContext *viewport = &navigation->page.layout.viewport;
    int left = viewport_css_to_device(viewport, x);
    int top = viewport_css_to_device(viewport, y - entry->scroll_y);
    int right = viewport_css_to_device(viewport, x + width);
    int bottom = viewport_css_to_device(viewport, y - entry->scroll_y + height);
    if (right <= left || bottom <= top || right <= 0 || bottom <= 0
        || left >= 480 || top >= 272) return false;
    size_t count = 0;
    uint16_t *pixels = browser_engine_framebuffer(engine, &count);
    render_paint_focus_outline(pixels, count, 480, 272, left, top,
                              right - left, bottom - top);
    return true;
}

static bool font_publication_timing(void *opaque, const char *phase, size_t work)
{
    FontPublicationTiming *timing = opaque;
    uint64_t now = tilefinch_platform_monotonic_time_us();
    if (timing->interrupt_delay_us != 0
        && strcmp(phase, "optional-font-publication") == 0 && work != 0)
        timing->requested_us = now + timing->interrupt_delay_us;
    uint64_t gap = now - timing->last_us;
    if (gap > timing->maximum_gap_us) {
        timing->maximum_gap_us = gap;
        snprintf(timing->maximum_gap_phase, sizeof(timing->maximum_gap_phase),
                 "%s", phase == NULL ? "unknown" : phase);
        snprintf(timing->maximum_gap_from, sizeof(timing->maximum_gap_from),
                 "%s", timing->last_phase);
    }
    timing->last_us = now;
    snprintf(timing->last_phase, sizeof(timing->last_phase), "%s", phase);
    timing->checkpoints++;
    if (timing->requested_us != 0 && now >= timing->requested_us
        && strncmp(phase, "layout", 6) == 0) {
        if (timing->acknowledged_us == 0) timing->acknowledged_us = now;
        return false;
    }
    return true;
}

/* Opt-in host profiler: replay real page CSS/DOM through the ordinary engine
   with device-style staged fonts. This is not a live-site CTest dependency. */
static int profile_replay_rasters(BrowserEngine *engine)
{
    const NavigationSession *navigation = browser_engine_navigation(engine);
    for (unsigned view = 0; view < 3; view++) {
        if (view == 0) (void) browser_engine_scroll_to_edge(engine, false);
        else if (view == 1) (void) browser_engine_scroll_by(
            engine, navigation->page.layout.height / 2);
        else (void) browser_engine_scroll_to_edge(engine, true);
        CHECK(browser_engine_render_frame(engine, NULL));
        size_t pixel_count = 0;
        const uint16_t *pixels = browser_engine_framebuffer(engine, &pixel_count);
        CHECK(pixels != NULL && pixel_count == 480u * 272u);
        printf("font-raster view=%u checksum=%llu\n", view,
            (unsigned long long) frame_checksum(pixels, pixel_count));
    }
    return 0;
}

typedef enum {
    PROFILE_REPLAY_SYNC, PROFILE_REPLAY_SCRIPTED,
    PROFILE_REPLAY_NATIVE, PROFILE_REPLAY_NATIVE_STRICT
} ProfileReplayMode;

static int profile_staged_font_replay(const char *trace, const char *url,
                                     uint64_t interrupt_delay_us, ProfileReplayMode incremental,
                                     const char *search_query, unsigned settle_runtime)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    if (incremental) {
        config.memory_limit = 32u * MIB;
        config.javascript.enabled = true;
        config.javascript.document_scripts_enabled = true;
        config.javascript.heap_limit = 7u * MIB;
        config.resources.enabled = true;
        config.resources.maximum_stylesheets = 24;
        config.resources.maximum_stylesheet_bytes = 2112u * 1024u;
        config.resources.maximum_stylesheet_file_bytes = 768u * 1024u;
    }
    if (search_query != NULL || settle_runtime) {
        /* Match installed-app admission, not the roomier profiling heap. */
        config.history_capacity = 4;
        config.session_cache_limit = 640u * 1024u;
        config.javascript.heap_limit = 5u * MIB;
        config.javascript.maximum_scripts = 256;
        config.javascript.maximum_total_bytes = 2u * MIB;
        config.javascript.runtime_timeout_ms = 60000;
        config.javascript.network_timeout_ms = 15000;
        config.resources.maximum_images = 24;
        config.resources.maximum_image_bytes = 1536u * 1024u;
        config.resources.maximum_image_file_bytes = 512u * 1024u;
        config.resources.maximum_decoded_image_bytes = 3u * MIB;
        config.resources.timeout_ms = 15000;
    }
    bool native = incremental == PROFILE_REPLAY_NATIVE
        || incremental == PROFILE_REPLAY_NATIVE_STRICT;
    if (native) {
        config.memory_limit = (incremental == PROFILE_REPLAY_NATIVE_STRICT ? 16u : 24u) * MIB;
        config.javascript.enabled = false;
        config.javascript.document_scripts_enabled = false;
        config.history_capacity = 4;
        config.session_cache_limit = 640u * 1024u;
        config.resources.maximum_images = 24;
        config.resources.maximum_image_bytes = 1536u * 1024u;
        config.resources.maximum_decoded_image_bytes = 3u * MIB;
    }
    CHECK(browser_config_set_font_paths(&config,
        TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Latin.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Latin.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Oblique-Latin.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSans-Bold-Latin.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/DejaVuSerif-Bold-Latin.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Regular.ttf",
        TILEFINCH_TEST_SOURCE_DIR "/fonts/TilefinchSans-Bold.ttf",
        1536u * 1024u));
    browser_config_set_staged_font_loading(&config, true);
    char error[256] = {0};
    CHECK(fetch_trace_replay_begin_response_keyed(trace, error, sizeof(error)));
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_prepare_navigation_fonts(engine, "GET", url));
    uint64_t load_started = tilefinch_platform_monotonic_time_us(), first_preview_us = 0;
    if (incremental) {
        FontPublicationTiming load_timing = {
            .last_us = tilefinch_platform_monotonic_time_us()
        };
        TilefinchPlatformServices load_services = {
            .context = &load_timing, .cooperate = font_publication_timing
        };
        tilefinch_platform_set_services(&load_services);
        load_started = load_timing.last_us;
        uint64_t maximum_pump_us = 0;
        CHECK(browser_engine_begin_navigation_url(engine, url, 2u * MIB, 20000, true));
        (void) font_publication_timing(&load_timing, "navigation-begin-return", 0);
        if (native) {
            /* Replay opening verifies the captured body and may initialize
               host curl/proxy services. Report it, but do not confuse that
               host-only cost with an uninterrupted parser/layout slice. */
            printf("navigation-request request-begin-us=%llu\n",
                (unsigned long long) (load_timing.last_us - load_started));
            load_timing.maximum_gap_us = 0;
            load_timing.maximum_gap_phase[0] = '\0';
            load_timing.maximum_gap_from[0] = '\0';
        }
        BrowserNavigationJobQuota quota = { .load = {
            .fetch = { .maximum_body_bytes = 16u * 1024u,
                .maximum_body_callbacks = 1, .maximum_time_us = 2000 },
            .maximum_parser_body_bytes = 2048, .maximum_parser_time_us = 2000,
            .maximum_finalize_work_units = 2048, .maximum_finalize_time_us = 2000 } };
        BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
        for (size_t i = 0; i < 100000 && status == BROWSER_NAVIGATION_JOB_PENDING; i++) {
            uint64_t pump_started = tilefinch_platform_monotonic_time_us();
            status = browser_engine_pump_navigation(engine, &quota);
            uint64_t pump_us = tilefinch_platform_monotonic_time_us() - pump_started;
            if (pump_us > maximum_pump_us) maximum_pump_us = pump_us;
            (void) font_publication_timing(&load_timing, "navigation-pump-return", 0);
            BrowserProvisionalViewport preview = {0};
            if (!first_preview_us && browser_engine_provisional_viewport(engine, &preview))
                first_preview_us = tilefinch_platform_monotonic_time_us() - load_started;
        }
        tilefinch_platform_set_services(NULL);
        printf("navigation-responsiveness elapsed-us=%llu max-pump-us=%llu "
               "max-gap-us=%llu gap-ending-at=%s gap-starting-at=%s checkpoints=%zu\n",
            (unsigned long long) (tilefinch_platform_monotonic_time_us() - load_started),
            (unsigned long long) maximum_pump_us,
            (unsigned long long) load_timing.maximum_gap_us,
            load_timing.maximum_gap_phase, load_timing.maximum_gap_from,
            load_timing.checkpoints);
        CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    } else CHECK(browser_engine_load_url(engine, url, true));
    if (interrupt_delay_us != 0) CHECK(browser_engine_focus_move(engine, true));
    CHECK(browser_engine_render_frame(engine, NULL));
    if (native) printf("navigation-preview available=%u first-preview-us=%llu first-frame-us=%llu\n",
        first_preview_us != 0, (unsigned long long) first_preview_us,
        (unsigned long long) (tilefinch_platform_monotonic_time_us() - load_started));
    if (interrupt_delay_us != 0) CHECK(paint_host_focus(engine));
    const NavigationSession *navigation = browser_engine_navigation(engine);
    if (native) {
        FontPublicationTiming idle = { .last_us = tilefinch_platform_monotonic_time_us() };
        TilefinchPlatformServices services = { .context = &idle, .cooperate = font_publication_timing };
        uint64_t maximum_idle = 0;
        for (unsigned tick = 0; tick < 128; tick++) {
            bool changed = false;
            idle.last_us = tilefinch_platform_monotonic_time_us();
            uint64_t start = idle.last_us;
            tilefinch_platform_set_services(&services);
            (void) browser_engine_run_idle_work(engine, &changed);
            (void) font_publication_timing(&idle, "idle-return", 0);
            tilefinch_platform_set_services(NULL);
            uint64_t elapsed = tilefinch_platform_monotonic_time_us() - start;
            if (elapsed > maximum_idle) maximum_idle = elapsed;
            CHECK(navigation->page.loaded && browser_engine_last_error(engine)[0] == '\0');
            if (changed) CHECK(browser_engine_render_frame(engine, NULL));
        }
        BrowserEngineMetrics metrics = {0};
        /* Include the top/middle/bottom renders in the peak, rather than
           reporting an artificially low pre-raster memory watermark. */
        CHECK(profile_replay_rasters(engine) == 0
            && browser_engine_metrics(engine, &metrics));
        printf("navigation-native limit-bytes=%zu peak-bytes=%zu retained-bytes=%zu "
            "max-idle-us=%llu max-gap-us=%llu gap-ending-at=%s gap-starting-at=%s\n",
            config.memory_limit, metrics.budget_peak, metrics.budget_current,
            (unsigned long long) maximum_idle, (unsigned long long) idle.maximum_gap_us,
            idle.maximum_gap_phase, idle.maximum_gap_from);
        CHECK(browser_engine_shutdown(engine)
            && browser_engine_metrics(engine, &metrics)
            && metrics.budget_current == 0 && metrics.budget_active_allocations == 0);
        browser_engine_destroy(engine);
        fetch_trace_end();
        return 0;
    }
    if (settle_runtime) {
        size_t initial_loaded = navigation->page.images.stats.loaded;
        size_t initial_deferred = navigation->page.deferred_image_count;
        size_t initial_heap_rejections = script_runtime_heap_rejections(navigation->page.runtime);
        CHECK(browser_engine_focus_move(engine, true)
            && browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
        uint64_t runtime_us = 0, maximum_runtime_us = 0;
        uint64_t idle_us = 0, maximum_idle_us = 0;
        uint64_t font_publication_us = 0, image_publication_us = 0;
        uint64_t startup_done_us = 0, startup_idle_us = 0;
        size_t completed_dynamic = 0;
        uint64_t settle_started = tilefinch_platform_monotonic_time_us();
        unsigned settle_ticks = settle_runtime == 1u ? 240u : settle_runtime;
        for (unsigned tick = 0; tick < settle_ticks; tick++) {
            size_t previous_failed = navigation->page.script_result.dynamic_scripts_failed;
            size_t previous_uncaught = navigation->page.script_result.uncaught_callback_errors;
            uint64_t start = tilefinch_platform_monotonic_time_us();
            bool advanced = browser_engine_advance_runtime(engine, 16, 4, NULL);
            if (!advanced)
                fprintf(stderr, "settle-advance-failure tick=%u error=%s\n",
                    tick, browser_engine_last_error(engine));
            CHECK(advanced);
            if (navigation->page.script_result.dynamic_scripts_failed != previous_failed
                || navigation->page.script_result.uncaught_callback_errors != previous_uncaught)
                fprintf(stderr, "settle-exception tick=%u failed=%zu uncaught=%zu error=%s callback=%s\n",
                    tick, navigation->page.script_result.dynamic_scripts_failed,
                    navigation->page.script_result.uncaught_callback_errors,
                    navigation->page.script_result.error,
                    navigation->page.script_result.last_uncaught_callback_error);
            uint64_t elapsed = tilefinch_platform_monotonic_time_us() - start;
            runtime_us += elapsed;
            if (elapsed > maximum_runtime_us) maximum_runtime_us = elapsed;
            const ScriptResult *current_result = &navigation->page.script_result;
            if (current_result->dynamic_scripts_completed > completed_dynamic) {
                completed_dynamic = current_result->dynamic_scripts_completed;
                startup_done_us = tilefinch_platform_monotonic_time_us() - settle_started;
                startup_idle_us = idle_us;
            }
            bool changed = false;
            size_t font_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
            size_t image_relayouts = navigation->performance.background_image_relayouts;
            start = tilefinch_platform_monotonic_time_us();
            (void) browser_engine_run_idle_work(engine, &changed);
            elapsed = tilefinch_platform_monotonic_time_us() - start;
            idle_us += elapsed;
            if (elapsed > maximum_idle_us) maximum_idle_us = elapsed;
            if (font_set_loaded_bytes(browser_engine_fonts(engine)) != font_bytes)
                font_publication_us += elapsed;
            if (navigation->performance.background_image_relayouts != image_relayouts)
                image_publication_us += elapsed;
            CHECK(navigation->page.loaded && browser_engine_last_error(engine)[0] == '\0');
        }
        const ScriptResult *result = &navigation->page.script_result;
        printf("navigation-settle initial-loaded=%zu initial-deferred=%zu "
            "runtime-us=%llu max-runtime-us=%llu idle-us=%llu max-idle-us=%llu "
            "dynamic-started=%zu completed=%zu failed=%zu error=\"%s\" "
            "final-loaded=%zu image-relayouts=%zu heap-rejections=%zu "
            "resource-us=%llu image-us=%llu relayout-us=%llu\n",
            initial_loaded, initial_deferred,
            (unsigned long long) runtime_us, (unsigned long long) maximum_runtime_us,
            (unsigned long long) idle_us, (unsigned long long) maximum_idle_us,
            result->dynamic_scripts_started, result->dynamic_scripts_completed,
            result->dynamic_scripts_failed, result->error,
            navigation->page.images.stats.loaded,
            navigation->performance.background_image_relayouts,
            script_runtime_heap_rejections(navigation->page.runtime) - initial_heap_rejections,
            (unsigned long long) navigation->performance.resource_us,
            (unsigned long long) navigation->performance.image_resource_us,
            (unsigned long long) navigation->performance.relayout_us);
        printf("navigation-startup last-dynamic-completion-us=%llu preceding-idle-us=%llu\n",
            (unsigned long long) startup_done_us, (unsigned long long) startup_idle_us);
        LayoutReuseStats reuse_stats;
        layout_reuse_cache_stats(navigation->page.layout_reuse, &reuse_stats);
        printf("navigation-publication font-us=%llu image-us=%llu counter-builds=%zu "
            "counter-hits=%zu cache-bytes=%zu fingerprint=%llu height=%d\n",
            (unsigned long long) font_publication_us,
            (unsigned long long) image_publication_us,
            reuse_stats.counter_index_builds, reuse_stats.counter_index_hits,
            reuse_stats.retained_bytes,
            (unsigned long long) navigation->page.layout.text_fingerprint,
            navigation->page.layout.height);
        if (result->error[0] != '\0') {
            script_runtime_report_memory(navigation->page.runtime, stderr);
            budget_quickjs_pool_report_rejects(stderr);
            fprintf(stderr, "navigation-startup-error callback=\"%s\" task=\"%s\"\n",
                result->last_uncaught_callback_error, result->last_uncaught_callback_task);
        }
        CHECK(result->error[0] == '\0' && result->uncaught_callback_errors == 0
            && result->dynamic_scripts_failed == 0
            && result->dynamic_scripts_started == result->dynamic_scripts_completed
            && browser_engine_focus_move(engine, true)
            && browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
        printf("navigation-selectors compounds=%llu candidates=%llu ancestor-rejections=%llu\n",
            (unsigned long long) navigation->page.stylesheet.selector_compound_calls,
            (unsigned long long) navigation->page.stylesheet.rule_index_candidates,
            (unsigned long long) navigation->page.stylesheet.rule_ancestor_filter_rejections);
        CHECK(profile_replay_rasters(engine) == 0);
        /* A rendered focus ring alone does not prove that its author handler
           ran. Refresh the result after dispatch and require allocation-free
           failure accounting across repeated post-initialization interaction.
           Previously the final focus callback could OOM after the last result
           check while this replay still returned success. */
        size_t interaction_rejections = script_runtime_heap_rejections(navigation->page.runtime);
        for (unsigned interaction = 0; interaction < 8; ++interaction) {
            CHECK(browser_engine_focus_move(engine, (interaction & 1u) == 0u)
                && browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
            CHECK(browser_engine_advance_runtime(engine, 16, 4, NULL));
            CHECK(navigation->page.script_result.error[0] == '\0'
                && navigation->page.script_result.uncaught_callback_errors == 0);
        }
        CHECK(script_runtime_heap_rejections(navigation->page.runtime) == interaction_rejections);
        printf("navigation-post-settle interactions=8 uncaught=0 heap-rejections=0\n");
        const Budget *load_budget = browser_engine_budget(engine);
        printf("navigation-memory current=%zu peak=%zu allocations=%zu frees=%zu\n",
            load_budget->current, load_budget->peak,
            load_budget->allocation_count, load_budget->free_count);
        for (size_t category = 0; category < BUDGET_CATEGORY_COUNT; category++) {
            const BudgetCategoryStats *stats = &load_budget->categories[category];
            printf("navigation-memory-category id=%zu current=%zu peak=%zu allocations=%zu frees=%zu\n",
                category, stats->current, stats->peak,
                stats->allocation_count, stats->free_count);
        }
        CHECK(browser_engine_shutdown(engine));
        browser_engine_destroy(engine);
        fetch_trace_end();
        return 0;
    }
    if (search_query != NULL) {
        lxb_dom_node_t *input = test_reader_find_id(
            lxb_dom_interface_node(navigation->page.document.html), "searchInput");
        CHECK(input != NULL && browser_engine_focus_node(engine, input));
        int x, y, width, height;
        CHECK(controller_focused_rect(browser_engine_controller(engine),
              &x, &y, &width, &height));
        x = viewport_css_to_device(&navigation->page.layout.viewport, x + 8);
        y = viewport_css_to_device(&navigation->page.layout.viewport,
            y - navigation_current(navigation)->scroll_y + height / 2);
        bool activate = false, changed = false;
        CHECK(browser_engine_pointer_event(engine, CONTROLLER_POINTER_DOWN,
                  x, y, &activate, &changed)
              && browser_engine_pointer_event(engine, CONTROLLER_POINTER_UP,
                  x, y, &activate, &changed)
              && activate && browser_engine_pointer_commit_click(engine));
        ControllerTextInputInfo input_info = {0};
        CHECK(browser_engine_text_input_info(engine, &input_info) && input_info.editable
              && browser_engine_replace_text(engine, search_query, strlen(search_query)));
        ControllerAction action = {0};
        CHECK(browser_engine_activate(engine, &action));
        printf("search-journey action=%d outcome=%d url=%s\n",
               action.type, action.activation_outcome, action.url);
        CHECK(action.type == CONTROLLER_ACTION_FORM_SUBMIT
              && strstr(action.url, "search=psp") != NULL
              && browser_engine_begin_navigation_action(engine, &action, 2u * MIB, 60000));
        BrowserNavigationJobQuota quota = { .load = {
            .fetch = { .maximum_body_bytes = 16384, .maximum_body_callbacks = 1,
                .maximum_time_us = 2000 }, .maximum_parser_body_bytes = 2048,
            .maximum_parser_time_us = 2000, .maximum_finalize_work_units = 2048,
            .maximum_finalize_time_us = 2000 } };
        BrowserNavigationJobStatus status = BROWSER_NAVIGATION_JOB_PENDING;
        for (unsigned pump = 0; pump < 100000 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
            status = browser_engine_pump_navigation(engine, &quota);
        printf("search-journey status=%d title=%s error=%s\n", status,
               navigation->page.document.title, browser_engine_last_error(engine));
        CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED && navigation->page.loaded
              && strstr(navigation->page.document.title, "PSP") != NULL
              && browser_engine_render_frame(engine, NULL));
        bool found = false;
        for (size_t i = 0; i < navigation->page.layout.link_count; i++) {
            const LinkRegion *link = &navigation->page.layout.links[i];
            if (strstr(link->url, "/wiki/PlayStation_Portable") == NULL) continue;
            CHECK(browser_engine_focus_node(engine, link->node)
                  && browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
            found = true;
            break;
        }
        CHECK(found);
        puts("search-journey result-link-visible=1");
        CHECK(browser_engine_shutdown(engine));
        browser_engine_destroy(engine);
        fetch_trace_end();
        return 0;
    }
    printf("font-profile initial-bytes=%zu commands=%zu\n",
           font_set_loaded_bytes(browser_engine_fonts(engine)), navigation->page.layout.count);
    if (incremental) {
        const ExternalImageStats *stats = &navigation->page.images.stats;
        printf("navigation-images deferred=%zu loaded=%zu traversal=%lluus node=%lluus pseudo=%lluus drain=%lluus\n",
            navigation->page.deferred_image_count, stats->loaded,
            (unsigned long long) stats->traversal_us,
            (unsigned long long) stats->node_style_resolve_us,
            (unsigned long long) stats->pseudo_style_resolve_us,
            (unsigned long long) stats->drain_us);
    }
    LayoutReuseStats before = {0}, after = {0};
    FontPublicationTiming timing = { .interrupt_delay_us = interrupt_delay_us };
    TilefinchPlatformServices services = { .context = &timing,
        .cooperate = font_publication_timing };
    size_t publications = 0;
    for (size_t i = 0; i < 512 && publications == 0; i++) {
        CHECK(browser_engine_advance_runtime(engine, 16, 4, NULL));
        /* Borrow face identities only; the engine remains their sole owner.
           Count the old display list because publication replaces that list. */
        FontSet previous_fonts = *browser_engine_fonts(engine);
        size_t font_requests[32][4] = {{0}};
        for (size_t at = 0; at < navigation->page.layout.count; at++) {
            const DrawCommand *command = &navigation->page.layout.commands[at];
            if (command->type != DRAW_TEXT || command->font_family >= 32) continue;
            unsigned variant = (draw_command_font_italic(command) ? 1u : 0u)
                | (draw_command_font_weight_code(command) >= 65 ? 2u : 0u);
            font_requests[command->font_family][variant]++;
        }
        size_t loaded_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
        size_t relayouts = navigation->incremental_relayouts;
        size_t before_pixel_count = 0;
        const uint16_t *before_pixels = browser_engine_framebuffer(engine, &before_pixel_count);
        uint64_t before_checksum = frame_checksum(before_pixels, before_pixel_count);
        layout_reuse_cache_stats(navigation->page.layout_reuse, &before);
        timing.maximum_gap_us = 0;
        timing.checkpoints = 0;
        timing.last_us = tilefinch_platform_monotonic_time_us();
        uint64_t start = timing.last_us;
        tilefinch_platform_set_services(&services);
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        uint64_t returned_us = tilefinch_platform_monotonic_time_us();
        (void) font_publication_timing(&timing, "idle-return", 0);
        tilefinch_platform_set_services(NULL);
        if (timing.acknowledged_us != 0) {
            CHECK(navigation->page.loaded && navigation->layout_build_cancelled
                && font_set_loaded_bytes(browser_engine_fonts(engine)) == loaded_bytes
                && navigation->incremental_relayouts == relayouts && !changed);
            printf("font-interruption requested-after-us=%llu acknowledgement-us=%llu "
                   "drain-us=%llu total-response-us=%llu retained-page=1\n",
                (unsigned long long) interrupt_delay_us,
                (unsigned long long) (timing.acknowledged_us - timing.requested_us),
                (unsigned long long) (returned_us - timing.acknowledged_us),
                (unsigned long long) (returned_us - timing.requested_us));
            /* Dispatch only after rollback releases the private layout.
               A selected node is not enough: require new framebuffer pixels. */
            CHECK(browser_engine_focus_move(engine, true));
            BrowserRenderJobStatus render_status = BROWSER_RENDER_JOB_PENDING;
            unsigned render_slices = 0;
            while (render_status == BROWSER_RENDER_JOB_PENDING && render_slices < 128) {
                render_status = browser_engine_render_frame_bounded(engine, 1000, 1);
                render_slices++;
            }
            CHECK(paint_host_focus(engine));
            uint64_t rendered_us = tilefinch_platform_monotonic_time_us();
            size_t pixel_count = 0;
            const uint16_t *pixels = browser_engine_framebuffer(engine, &pixel_count);
            CHECK(render_status == BROWSER_RENDER_JOB_COMPLETE
                  && pixel_count == before_pixel_count
                  && frame_checksum(pixels, pixel_count) != before_checksum
                  && font_set_loaded_bytes(browser_engine_fonts(engine)) == loaded_bytes);
            printf("font-input-feedback render-us=%llu request-to-pixels-us=%llu "
                   "render-slices=%u pixels-changed=1\n",
                (unsigned long long) (rendered_us - returned_us),
                (unsigned long long) (rendered_us - timing.requested_us), render_slices);
            publications++;
            break;
        }
        if (font_set_loaded_bytes(browser_engine_fonts(engine)) > loaded_bytes
            && navigation->incremental_relayouts != relayouts) {
            publications++;
            layout_reuse_cache_stats(navigation->page.layout_reuse, &after);
            size_t text_commands = 0, changed_faces = 0;
            const WebFontSet *web_fonts = stylesheet_web_font_set(
                &navigation->page.stylesheet);
            for (unsigned family = 0; family < 32; family++) {
                for (unsigned variant = 0; variant < 4; variant++) {
                    size_t count = font_requests[family][variant];
                    if (count == 0) continue;
                    const FontFace *old_face = font_context_face_variant(
                        &previous_fonts, web_fonts, (FontFamily) family,
                        (variant & 1u) != 0, (variant & 2u) != 0);
                    const FontFace *new_face = font_context_face_variant(
                        browser_engine_fonts(engine), web_fonts, (FontFamily) family,
                        (variant & 1u) != 0, (variant & 2u) != 0);
                    bool changed_face = (old_face == NULL ? NULL : old_face->data)
                        != (new_face == NULL ? NULL : new_face->data);
                    text_commands += count;
                    if (changed_face) changed_faces += count;
                }
            }
            printf("font-usage text-commands=%zu changed-face-commands=%zu\n",
                   text_commands, changed_faces);
            printf("font-publication elapsed-us=%llu max-gap-us=%llu gap-ending-at=%s checkpoints=%zu "
                   "style-hits=%zu style-misses=%zu parent-misses=%zu evictions=%zu resets=%zu flow-us=%llu "
                   "ascii-segments=%llu fingerprint=%llu height=%d\n",
                (unsigned long long) (returned_us - start),
                (unsigned long long) timing.maximum_gap_us, timing.maximum_gap_phase,
                timing.checkpoints,
                after.style_hits - before.style_hits,
                after.style_misses - before.style_misses,
                after.style_parent_misses - before.style_parent_misses,
                after.style_evictions - before.style_evictions,
                after.full_resets - before.full_resets,
                (unsigned long long) navigation->page.layout.performance.flow_us,
                (unsigned long long) navigation->page.layout.performance.ascii_text_segments,
                (unsigned long long) navigation->page.layout.text_fingerprint,
                navigation->page.layout.height);
        }
    }
    if (publications != 1) fprintf(stderr,
        "font-profile final-bytes=%zu dirty=%d loaded=%d error=%s\n",
        font_set_loaded_bytes(browser_engine_fonts(engine)),
        script_runtime_has_pending_mutations(navigation->page.runtime),
        navigation->page.loaded, browser_engine_last_error(engine));
    CHECK(publications == 1 && navigation->page.loaded);
    CHECK(interrupt_delay_us == 0 || timing.acknowledged_us != 0);
    if (interrupt_delay_us == 0) {
        /* Raster comparison is deliberately outside the publication timer. */
        CHECK(profile_replay_rasters(engine) == 0);
    }
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

static uint64_t optional_font_probe_clock(void *opaque)
{
    return ((OptionalFontInputProbe *) opaque)->now_us;
}

static bool optional_font_probe_cooperate(void *opaque, const char *phase,
                                          size_t work)
{
    OptionalFontInputProbe *probe = opaque;
    if (strcmp(phase, "optional-font-publication") != 0) return true;
    if (work == 0) { probe->ends++; return true; }
    /* A new cursor/input sample wins before any metrics are changed. */
    return ++probe->publications != 1;
}

static int test_promoted_page_preserves_style_reuse(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-font-promotion",
        error, sizeof(error)) && browser_engine_load_url(engine,
        "https://font-promotion.test/page", true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    LayoutReuseCache *cache = navigation->page.layout_reuse;
    lxb_dom_node_t *node = lxb_dom_interface_node(navigation->page.document.html->body);
    ComputedStyle style;
    CHECK(cache != NULL && node != NULL);
    (void) layout_reuse_cache_resolve_style(cache, &navigation->page.stylesheet,
        navigation->fonts, node, NULL, &style);
    CHECK(layout_reuse_cache_resolve_style(cache, &navigation->page.stylesheet,
        navigation->fonts, node, NULL, &style));
    layout_reuse_cache_prepare(cache, &navigation->page.stylesheet,
        navigation->fonts, navigation->images, navigation->viewport.css_width);
    CHECK(layout_reuse_cache_resolve_style(cache, &navigation->page.stylesheet,
        navigation->fonts, node, NULL, &style));
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    fetch_trace_end();
    return 0;
}

static int test_staged_optional_fonts_relayout_before_repaint(void)
{
    CHECK(test_promoted_page_preserves_style_reuse() == 0);
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 64u * 1024u;
    config.tile_capacity = 2;
    char sans[512], serif[512], italic[512], bold[512];
    char serif_bold[512], metric[512], metric_bold[512];
    snprintf(sans, sizeof(sans), "%s/fonts/DejaVuSans-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(serif, sizeof(serif), "%s/fonts/DejaVuSerif-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(italic, sizeof(italic),
             "%s/fonts/DejaVuSans-Oblique-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(bold, sizeof(bold), "%s/fonts/DejaVuSans-Bold-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(serif_bold, sizeof(serif_bold),
             "%s/fonts/DejaVuSerif-Bold-Latin.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(metric, sizeof(metric), "%s/fonts/TilefinchSans-Regular.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    snprintf(metric_bold, sizeof(metric_bold),
             "%s/fonts/TilefinchSans-Bold.ttf",
             TILEFINCH_TEST_SOURCE_DIR);
    CHECK(browser_config_set_font_paths(
              &config, sans, serif, italic, bold, serif_bold,
              metric, metric_bold, 1536u * 1024u));
    browser_config_set_staged_font_loading(&config, true);
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(!browser_engine_baseline_fonts_ready(engine)
          && browser_engine_prepare_native_home_font(engine)
          && browser_engine_native_home_font(engine) != NULL
          && browser_engine_native_home_font(engine)->loaded
          && (browser_engine_font_face(engine, FONT_METRIC_SANS) == NULL
              || !browser_engine_font_face(engine, FONT_METRIC_SANS)->loaded)
          && !browser_engine_baseline_fonts_ready(engine));
    for (size_t i = 0;
         i < 4 && !browser_engine_baseline_fonts_ready(engine); i++) {
        CHECK(browser_engine_pump_baseline_fonts(engine));
    }
    CHECK(browser_engine_baseline_fonts_ready(engine));
    static const char page[] =
        "<!doctype html><style>body{margin:0}#probe{display:block;"
        "font:700 22px serif;width:180px}</style><body>"
        "<a id=probe autofocus data-tilefinch-provider-media "
        "href='https://www.youtube.com/watch?v=TFTEST00001'>"
        "Metric-sensitive text wraps across this bounded box</a>"
        "<input value='Search'>"
        "<p style='font:italic 16px sans-serif'>Italic face</p>"
        "<p style='font:bold 16px sans-serif'>Bold face</p>";
    CHECK(browser_engine_commit_html(
        engine, "https://font.test/", page, sizeof(page) - 1u, true)
          && browser_engine_focus_move(engine, true));
    size_t relayouts_before =
        browser_engine_navigation(engine)->incremental_relayouts;
    size_t visual_changes = 0;
    size_t pending_slices = 0;
    bool interrupted_staging = false;
    size_t baseline_font_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
    for (size_t i = 0; i < 64; i++) {
        bool visual_changed = false;
        bool pending = browser_engine_run_idle_work(
            engine, &visual_changed);
        if (visual_changed) {
            visual_changes++;
            CHECK(browser_engine_render_frame(engine, NULL));
        }
        if (pending) pending_slices++;
        if (visual_changes == 0) {
            CHECK(font_set_loaded_bytes(browser_engine_fonts(engine)) == baseline_font_bytes
                  && browser_engine_navigation(engine)->incremental_relayouts == relayouts_before);
        }
        /* Reclaim both a completed private face and the next partial read. */
        if (pending && i == 6 && !interrupted_staging) {
            BrowserOptionalMemoryReclaim font_reclaim = {0};
            CHECK(browser_engine_reclaim_optional_memory(
                      engine, &font_reclaim)
                  && font_reclaim.font_staging_bytes > 128u * 1024u
                  && font_reclaim.total_bytes
                         >= font_reclaim.font_staging_bytes);
            interrupted_staging = true;
        }
        const FontFace *serif_face = browser_engine_font_face_variant(
            engine, FONT_SERIF, false, true);
        if (font_set_face_is_bold(
                browser_engine_fonts(engine), serif_face)) break;
    }
    CHECK(interrupted_staging && pending_slices >= 2 && visual_changes == 1
          && browser_engine_navigation(engine)->incremental_relayouts
                 == relayouts_before + 1u
          && font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_SERIF, false, true)));
    ControllerAction action = {0};
    CHECK(browser_engine_activate(engine, &action));
    CHECK(action.type == CONTROLLER_ACTION_NAVIGATE
          && action.prefer_native_media
          && strcmp(action.url,
                    "https://www.youtube.com/watch?v=TFTEST00001") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    /* A failed combined publication must retain the fallback layout, refuse
       the optional batch once, and release every private font allocation. */
    engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL && browser_engine_commit_html(
        engine, "https://font.test/", page, sizeof(page) - 1u, true));
    baseline_font_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
    int fallback_height = browser_engine_navigation(engine)->page.layout.height;
    relayouts_before = browser_engine_navigation(engine)->incremental_relayouts;
    navigation_test_refuse_next_same_document_relayout();
    for (size_t i = 0; i < 64; i++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
    }
    CHECK(browser_engine_navigation(engine)->page.loaded
          && browser_engine_navigation(engine)->page.layout.height == fallback_height
          && browser_engine_navigation(engine)->incremental_relayouts == relayouts_before
          && font_set_loaded_bytes(browser_engine_fonts(engine)) == baseline_font_bytes
          && browser_engine_last_error(engine)[0] == '\0'
          && browser_engine_render_frame(engine, NULL)
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    /* A supervisor cancel at a cooperate checkpoint (Circle pressed while
       the idle pump publishes the batch) is not a refusal: the faces return
       to staging with the fallback layout intact and publish on a later
       pump instead of being marked failed for the rest of the page. */
    engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    for (size_t i = 0;
         i < 4 && !browser_engine_baseline_fonts_ready(engine); i++) {
        CHECK(browser_engine_pump_baseline_fonts(engine));
    }
    CHECK(browser_engine_baseline_fonts_ready(engine)
          && browser_engine_commit_html(
              engine, "https://font.test/", page, sizeof(page) - 1u, true)
          && browser_engine_focus_move(engine, true));
    baseline_font_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
    fallback_height = browser_engine_navigation(engine)->page.layout.height;
    relayouts_before = browser_engine_navigation(engine)->incremental_relayouts;
    tilefinch_test_faults()->cancel_next_layout_cooperate = true;
    OptionalFontInputProbe input_probe = { .now_us = 1000000 };
    TilefinchPlatformServices input_services = {
        .context = &input_probe,
        .monotonic_time_us = optional_font_probe_clock,
        .cooperate = optional_font_probe_cooperate
    };
    tilefinch_platform_set_services(&input_services);
    bool cancelled_once = false;
    visual_changes = 0;
    for (size_t i = 0; i < 64; i++) {
        input_probe.now_us += 50000;
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        if (changed) {
            visual_changes++;
            CHECK(browser_engine_render_frame(engine, NULL));
        }
        CHECK(browser_engine_navigation(engine)->page.loaded
              && browser_engine_last_error(engine)[0] == '\0');
        if (!cancelled_once
            && !tilefinch_test_faults()->cancel_next_layout_cooperate) {
            cancelled_once = true;
            CHECK(!changed
                  && browser_engine_navigation(engine)->page.layout.height
                         == fallback_height
                  && browser_engine_navigation(engine)->incremental_relayouts
                         == relayouts_before
                  && font_set_loaded_bytes(browser_engine_fonts(engine))
                         == baseline_font_bytes);
            CHECK(browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
            size_t count = 0;
            const uint16_t *pixels = browser_engine_framebuffer(engine, &count);
            uint64_t before = frame_checksum(pixels, count);
            CHECK(browser_engine_focus_move(engine, true)
                  && browser_engine_render_frame(engine, NULL) && paint_host_focus(engine));
            pixels = browser_engine_framebuffer(engine, &count);
            CHECK(frame_checksum(pixels, count) != before
                  && font_set_loaded_bytes(browser_engine_fonts(engine))
                         == baseline_font_bytes);
        }
        if (font_set_face_is_bold(
                browser_engine_fonts(engine),
                browser_engine_font_face_variant(
                    engine, FONT_SERIF, false, true))) break;
    }
    tilefinch_platform_set_services(NULL);
    CHECK(input_probe.publications == 3 && input_probe.ends == 3
          && cancelled_once && visual_changes == 1
          && browser_engine_navigation(engine)->incremental_relayouts
                 == relayouts_before + 1u
          && font_set_loaded_bytes(browser_engine_fonts(engine))
                 > baseline_font_bytes
          && font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_SERIF, false, true))
          && browser_engine_render_frame(engine, NULL)
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    /* A page whose layout cost more than the publication limit keeps its
       fallback faces: no staging reads, no batch relayout, no error. */
    /* The device entry point uses only browser_config_init; the guards
       must be on by default there, not only in the memory-profile helper. */
    CHECK(config.fonts.maximum_publication_work_units == 200000u
          && config.resources.maximum_publication_work_units == 200000u
          && config.startup_resource_deferral_us == 3000000u);
    BrowserConfig guarded = config;
    guarded.fonts.maximum_publication_work_units = 1u;
    engine = browser_engine_create(&guarded, error, sizeof(error));
    CHECK(engine != NULL);
    for (size_t i = 0;
         i < 4 && !browser_engine_baseline_fonts_ready(engine); i++) {
        CHECK(browser_engine_pump_baseline_fonts(engine));
    }
    CHECK(browser_engine_baseline_fonts_ready(engine)
          && browser_engine_commit_html(
              engine, "https://font.test/", page, sizeof(page) - 1u, true)
          && browser_engine_focus_move(engine, true));
    baseline_font_bytes = font_set_loaded_bytes(browser_engine_fonts(engine));
    fallback_height = browser_engine_navigation(engine)->page.layout.height;
    relayouts_before = browser_engine_navigation(engine)->incremental_relayouts;
    CHECK(browser_engine_navigation(engine)->page.layout.layout_work_units > 1u);
    for (size_t i = 0; i < 64; i++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        CHECK(!changed);
    }
    BrowserOptionalMemoryReclaim guard_reclaim = {0};
    CHECK(browser_engine_navigation(engine)->page.loaded
          && browser_engine_navigation(engine)->page.layout.height == fallback_height
          && browser_engine_navigation(engine)->incremental_relayouts == relayouts_before
          && font_set_loaded_bytes(browser_engine_fonts(engine)) == baseline_font_bytes
          && browser_engine_reclaim_optional_memory(engine, &guard_reclaim)
          && guard_reclaim.font_staging_bytes == 0
          && browser_engine_last_error(engine)[0] == '\0'
          && browser_engine_render_frame(engine, NULL));
    /* A per-page publication refusal is not a permanent failed font file.
       The following provider navigation must still admit its bold faces. */
    CHECK(browser_engine_prepare_navigation_fonts(engine, "GET",
              "https://www.youtube.com/results?search_query=fixture")
          && font_set_face_is_bold(browser_engine_fonts(engine),
              browser_engine_font_face_variant(engine, FONT_SANS, false, true))
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);

    /* Queued startup scripts defer optional faces, but only for the
       configured wall-clock window: an external script that never arrives
       must not hold the batch until the runtime turn cap. */
    BrowserConfig deferred = config;
    deferred.javascript.enabled = true;
    deferred.javascript.document_scripts_enabled = true;
    deferred.startup_resource_deferral_us = 2000000u;
    static const char deferred_page[] =
        "<!doctype html><style>body{margin:0}#probe{display:block;"
        "font:700 22px serif;width:180px}</style><body>"
        "<p id=probe>Metric-sensitive text wraps across this bounded box</p>"
        "<p style='font:italic 16px sans-serif'>Italic face</p>"
        "<p style='font:bold 16px sans-serif'>Bold face</p>"
        "<script>var late=document.createElement('script');"
        "late.src='/late.js';document.head.appendChild(late);</script>";
    engine = browser_engine_create(&deferred, error, sizeof(error));
    CHECK(engine != NULL);
    for (size_t i = 0;
         i < 4 && !browser_engine_baseline_fonts_ready(engine); i++) {
        CHECK(browser_engine_pump_baseline_fonts(engine));
    }
    OptionalFontInputProbe deferral_clock = { .now_us = 5000000 };
    TilefinchPlatformServices deferral_services = {
        .context = &deferral_clock,
        .monotonic_time_us = optional_font_probe_clock
    };
    tilefinch_platform_set_services(&deferral_services);
    CHECK(browser_engine_baseline_fonts_ready(engine));
    bool deferred_committed = browser_engine_commit_html(
        engine, "https://font.test/", deferred_page,
        sizeof(deferred_page) - 1u, true);
    if (!deferred_committed)
        fprintf(stderr, "deferred commit failed: %s\n",
                browser_engine_last_error(engine));
    CHECK(deferred_committed && browser_engine_render_frame(engine, NULL));
    CHECK(browser_engine_navigation(engine)->page.runtime != NULL
          && script_runtime_has_pending_startup_scripts(
                 browser_engine_navigation(engine)->page.runtime));
    relayouts_before = browser_engine_navigation(engine)->incremental_relayouts;
    for (size_t i = 0; i < 32; i++) {
        deferral_clock.now_us += 50000;
        bool changed = false;
        CHECK(browser_engine_run_idle_work(engine, &changed) && !changed);
    }
    BrowserOptionalMemoryReclaim deferred_reclaim = {0};
    CHECK(browser_engine_navigation(engine)->incremental_relayouts == relayouts_before
          && browser_engine_reclaim_optional_memory(engine, &deferred_reclaim)
          && deferred_reclaim.font_staging_bytes == 0);
    deferral_clock.now_us += 2000000;
    visual_changes = 0;
    for (size_t i = 0; i < 64; i++) {
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
        if (changed) {
            visual_changes++;
            CHECK(browser_engine_render_frame(engine, NULL));
        }
        if (font_set_face_is_bold(
                browser_engine_fonts(engine),
                browser_engine_font_face_variant(
                    engine, FONT_SERIF, false, true))) break;
    }
    CHECK(visual_changes == 1
          && browser_engine_navigation(engine)->incremental_relayouts
                 == relayouts_before + 1u
          && script_runtime_has_pending_startup_scripts(
                 browser_engine_navigation(engine)->page.runtime)
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    tilefinch_platform_set_services(NULL);

    /* A built-in provider has a known typography contract: its bold faces
       must be ready before the candidate can perform its first layout. A
       normal destination and provider Home keep staged loading. */
    engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    while (!browser_engine_baseline_fonts_ready(engine))
        CHECK(browser_engine_pump_baseline_fonts(engine));
    CHECK(browser_engine_prepare_navigation_fonts(
              engine, "GET", "https://example.test/")
          && !font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_SANS, false, true)));
    CHECK(browser_engine_prepare_navigation_fonts(
              engine, "GET", "https://www.youtube.com/")
          && !font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_SANS, false, true)));
    static const char provider_home[] =
        "<!doctype html><style>b{font-weight:700}</style>"
        "<body><b>Search videos</b><input autofocus>";
    CHECK(browser_engine_commit_html(
              engine, "https://www.youtube.com/", provider_home,
              sizeof(provider_home) - 1u, true));
    for (size_t i = 0; i < 8; i++) {
        bool visual_changed = false;
        (void) browser_engine_run_idle_work(engine, &visual_changed);
        CHECK(!visual_changed);
    }
    CHECK(!font_set_face_is_bold(
        browser_engine_fonts(engine),
        browser_engine_font_face_variant(engine, FONT_SANS, false, true)));
    CHECK(browser_engine_prepare_navigation_fonts(
              engine, "GET",
              "https://m.youtube.com/results?search_query=fixture")
          && font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_SANS, false, true))
          && font_set_face_is_bold(
                 browser_engine_fonts(engine),
                 browser_engine_font_face_variant(
                     engine, FONT_METRIC_SANS, false, true))
          && browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_deferred_image_relayout_requests_repaint(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 64u * 1024u;
    config.tile_capacity = 2;
    config.progressive_first_paint = false;
    config.resources.enabled = true;
    config.javascript.enabled = false;
    config.javascript.document_scripts_enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    static const char page[] =
        "<!doctype html><style>html,body{margin:0;background:#000}"
        "img{display:block;width:24px;height:24px}</style><body>"
        "<a href=/first><img loading=lazy src=/hero.svg>First</a>"
        "<a href=/second><img loading=lazy src=/tail.svg>Second</a>";
    CHECK(engine != NULL);
    static const char blank[] =
        "<!doctype html><style>html,body{margin:0;background:#000}</style>";
    CHECK(browser_engine_commit_html(
              engine, "https://blank.test/", blank,
              sizeof(blank) - 1u, true)
          && browser_engine_render_frame(engine, NULL));
    size_t pixels = 0;
    const uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    uint64_t placeholder = frame_checksum(frame, pixels);

    bool trace_ready = fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR
            "/fixtures/http-deferred-document-images",
        error, sizeof(error));
    NavigationSession *navigation = browser_engine_navigation(engine);
    uint64_t generation = trace_ready ? navigation_begin(navigation) : 0;
    bool committed = trace_ready && navigation_commit_static_html(
        navigation, generation, "https://deferred-images.test/page", page,
        sizeof(page) - 1u, config.device.navigation_viewport_width,
        NULL, NULL, true);
    if (!trace_ready || !committed
        || browser_engine_navigation(engine)->page.images.stats.loaded != 0u
        || browser_engine_navigation(engine)->page.deferred_image_count
               != 2u) {
        fprintf(stderr,
                "deferred setup trace=%d committed=%d loaded=%zu "
                "deferred=%zu error=\"%s\"\n",
                trace_ready, committed,
                browser_engine_navigation(engine)->page.images.stats.loaded,
                browser_engine_navigation(engine)->page.deferred_image_count,
                browser_engine_last_error(engine));
    }
    bool initial_rendered = browser_engine_render_frame(engine, NULL);
    bool initial_focused = browser_engine_focus_move(engine, true);
    if (!initial_rendered || !initial_focused) {
        fprintf(stderr,
                "deferred focus setup rendered=%d focused=%d links=%zu "
                "controls=%zu\n",
                initial_rendered, initial_focused,
                browser_engine_navigation(engine)->page.layout.link_count,
                browser_engine_navigation(engine)->page.layout.control_count);
    }
    CHECK(trace_ready && committed
          && browser_engine_navigation(engine)->page.images.stats.loaded == 0u
          && browser_engine_navigation(engine)->page.deferred_image_count
                 == 2u
          && initial_rendered && initial_focused);
    size_t visual_changes = 0;
    bool input_during_relayout = false;
    for (size_t pumps = 0; pumps < 16u; pumps++) {
        bool visual_changed = false;
        bool pending = browser_engine_run_deferred_image_work(
            engine, &visual_changed);
        if (visual_changed) {
            visual_changes++;
            CHECK(browser_engine_render_frame(engine, NULL));
            if (!input_during_relayout) {
                ControllerAction during_load = {0};
                CHECK(browser_engine_focus_direction(
                          engine, CONTROLLER_FOCUS_DOWN)
                      && browser_engine_activate(engine, &during_load)
                      && during_load.type == CONTROLLER_ACTION_NAVIGATE
                      && strcmp(during_load.url,
                                "https://deferred-images.test/second") == 0
                      && browser_engine_focus_direction(
                             engine, CONTROLLER_FOCUS_UP));
                input_during_relayout = true;
            }
        }
        if (!pending) break;
    }
    frame = browser_engine_framebuffer(engine, &pixels);
    uint64_t painted = frame == NULL ? 0 : frame_checksum(frame, pixels);
    if (visual_changes != 1u
        || browser_engine_navigation(engine)->page.images.stats.loaded != 2u
        || painted == placeholder) {
        fprintf(stderr,
                "deferred repaint changes=%zu loaded=%zu "
                "placeholder=%llx painted=%llx relayouts=%zu/%zu "
                "background=%zu/%zu\n",
                visual_changes,
                browser_engine_navigation(engine)->page.images.stats.loaded,
                (unsigned long long) placeholder,
                (unsigned long long) painted,
                browser_engine_navigation(engine)->performance.fast_relayouts,
                browser_engine_navigation(engine)->performance.full_relayouts,
                browser_engine_navigation(engine)->performance
                    .background_images_loaded,
                browser_engine_navigation(engine)->performance
                    .background_image_relayouts);
    }
    CHECK(visual_changes == 1u
          && input_during_relayout
          && browser_engine_navigation(engine)->page.images.stats.loaded
                 == 2u
          && painted != placeholder);
    const TileCache *deferred_render =
        browser_engine_render_metrics_view(engine);
    CHECK(deferred_render != NULL
          && deferred_render->frames_rendered >= visual_changes + 1u);
    ControllerAction focus_action = {0};
    CHECK(browser_engine_activate(engine, &focus_action)
          && focus_action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(focus_action.url,
                    "https://deferred-images.test/first") == 0);
    fetch_trace_end();
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static lxb_dom_node_t *actionable_find_id(PocDocument *document,
                                          const char *identifier)
{
    if (document == NULL || document->html == NULL || identifier == NULL)
        return NULL;
    lxb_dom_node_t *root = lxb_dom_interface_node(document->html);
    lxb_dom_node_t *node = root;
    size_t identifier_length = strlen(identifier);
    size_t visited = 0;
    while (node != NULL && visited++ < 65536u) {
        size_t length = 0;
        const char *value = document_attribute(node, "id", &length);
        if (value != NULL && length == identifier_length
            && memcmp(value, identifier, length) == 0) return node;
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

static bool actionable_has_attribute(lxb_dom_node_t *node,
                                      const char *name)
{
    size_t ignored = 0;
    return document_attribute(node, name, &ignored) != NULL;
}

static int test_same_document_and_script_free_defaults(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    /* This fixture checks recovery from deliberately endless handlers, not
       a 20 ms deadline. Leave setup enough room under ASan/UBSan too: the
       old wrapper implementation also times out during its inline script. */
    config.javascript.runtime_timeout_ms = 200u;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    static const char fragment_page[] =
        "<!doctype html><title>Fragments</title>"
        "<base id=base href='/page'><style>"
        "body{margin:0}a,input{display:block;height:24px}.space{height:700px}"
        "#target{display:block;height:0}</style>"
        "<body data-tilefinch-reader-kind=article>"
        "<a id=jump href='#target'>Jump</a>"
        "<a id=named-jump href='#legacy'>Named</a>"
        "<a id=failed-link href='#other' "
        "onclick='event.preventDefault();while(true){}'>Failed link</a>"
        "<form action=/submitted "
        "onsubmit='event.preventDefault();while(true){}'>"
        "<button id=failed-submit>Submit</button></form>"
        "<a id=void href='javascript:void(0)' "
        "onclick=\"document.body.setAttribute('data-void','yes')\">Void</a>"
        "<a id=cancel href='#other' onclick=\"event.preventDefault()\">Cancel</a>"
        "<a id=same href='https://actions.test/page#target'>Same</a>"
        "<input id=partial type=checkbox "
        "onclick=\"event.preventDefault();while(true){}\">"
        "<input id=getter-partial type=checkbox>"
        "<form id=getter-form action=/getter-submitted>"
        "<button id=getter-submit>Getter submit</button></form>"
        "<form id=realm-form><input id=realm-check type=checkbox checked>"
        "<button id=realm-reset type=reset>Reset realm control</button></form>"
        "<div class=space></div><a id=target></a>"
        "<a name=legacy style='display:none'></a>"
        "<div class=space></div>"
        "<div class=space></div><div id=other>Other</div>"
        "<a id=base-jump href='#base-target'>Change base</a>"
        "<a id=base-relative href='next'>Relative after hash</a>"
        "<div id=base-target>Base target</div>"
        "<script>globalThis.hashCount=0;globalThis.popCount=0;"
        "addEventListener('hashchange',()=>{hashCount++;"
        "if(location.hash==='#base-target')"
        "document.getElementById('base').setAttribute('href','/mutated/');});"
        "addEventListener('popstate',()=>popCount++);"
        "const getterInput=document.getElementById('getter-partial');"
        "getterInput.addEventListener('tilefinch-keepalive',()=>{});"
        "const getterDisabled=Object.getOwnPropertyDescriptor("
        "getterInput,'disabled');"
        "Object.defineProperty(getterInput,'disabled',{...getterDisabled,"
        "get(){if(this.id==='getter-partial'){document.body.setAttribute("
        "'data-getter-ran','yes');while(true){}}"
        "return getterDisabled.get.call(this);}});"
        "const getterForm=document.getElementById('getter-form');"
        "getterForm.addEventListener('tilefinch-keepalive',()=>{});"
        "getterForm.checkValidity=function(){"
        "this.setAttribute('data-check-ran','yes');"
        "throw new Error('hostile checkValidity');};</script>"
        "<main id=native-reader data-tilefinch-reader-root=article>"
        "<a name=tilefinch-extracted-legacy>Legacy anchor</a></main>";
    bool fragment_committed = browser_engine_commit_html(
        engine, "https://actions.test/page", fragment_page,
        sizeof(fragment_page) - 1u, true);
    if (!fragment_committed)
        fprintf(stderr, "fragment setup failed: %s\n",
                browser_engine_last_error(engine));
    CHECK(fragment_committed);
    NavigationSession *navigation = browser_engine_navigation(engine);
    PocDocument *document = &navigation->page.document;
    lxb_dom_node_t *jump = actionable_find_id(document, "jump");
    lxb_dom_node_t *named_jump = actionable_find_id(document, "named-jump");
    lxb_dom_node_t *failed_link = actionable_find_id(document, "failed-link");
    lxb_dom_node_t *failed_submit = actionable_find_id(
        document, "failed-submit");
    lxb_dom_node_t *target = actionable_find_id(document, "target");
    lxb_dom_node_t *void_link = actionable_find_id(document, "void");
    lxb_dom_node_t *cancel = actionable_find_id(document, "cancel");
    lxb_dom_node_t *same = actionable_find_id(document, "same");
    lxb_dom_node_t *partial = actionable_find_id(document, "partial");
    lxb_dom_node_t *getter_partial = actionable_find_id(
        document, "getter-partial");
    lxb_dom_node_t *getter_form = actionable_find_id(document, "getter-form");
    lxb_dom_node_t *getter_submit = actionable_find_id(
        document, "getter-submit");
    lxb_dom_node_t *base_jump = actionable_find_id(document, "base-jump");
    lxb_dom_node_t *base_relative = actionable_find_id(
        document, "base-relative");
    lxb_dom_node_t *realm_check = actionable_find_id(document, "realm-check");
    lxb_dom_node_t *realm_reset = actionable_find_id(document, "realm-reset");
    lxb_dom_node_t *reader_root = actionable_find_id(
        document, "native-reader");
    /* This lifecycle fixture predates the extractor tests and constructs its
       tiny presentation root inline. Explicitly retain that exact node as the
       native provenance seam; an unretained authored look-alike is covered by
       the separate Basic/Reader spoof regression. */
    navigation->page.reader_root = reader_root;
    navigation->page.reader_root_kind = READER_PAGE_ARTICLE;
    navigation->page.reader_root_handle =
        script_runtime_node_weak_handle(
            navigation->page.runtime, reader_root);
    const LayoutNodeBox *target_box = layout_box_for_node(
        &navigation->page.layout, target);
    CHECK(jump != NULL && named_jump != NULL && failed_link != NULL
          && failed_submit != NULL && getter_partial != NULL
          && getter_form != NULL && getter_submit != NULL
          && base_jump != NULL && base_relative != NULL && realm_check != NULL
          && realm_reset != NULL
          && reader_root != NULL
          && navigation->page.reader_root_handle != 0
          && target_box != NULL && target_box->height == 0
          && target_box->y > profile.framebuffer_height);
    size_t loads_before = navigation->loads_started;
    uint64_t generation_before = navigation->generation;
    ControllerAction action = {0};
    CHECK(browser_engine_focus_node(engine, jump)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL
          && action.activation_outcome == CONTROLLER_ACTIVATION_DELIVERED
          && navigation->history_count == 2u
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page#target") == 0
          && navigation_current(navigation)->scroll_y > 0
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before
          && !browser_engine_navigation_pending(engine));
    int target_scroll = navigation_current(navigation)->scroll_y;

    /* An exact self URL remains an ordinary reload candidate. It must not be
       mistaken for a fragment transition merely because request keys match. */
    CHECK(same != NULL && browser_engine_focus_node(engine, same)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(action.url, "https://actions.test/page#target") == 0
          && navigation->history_count == 2u
          && navigation->loads_started == loads_before
          && navigation_set_scroll(navigation, target_scroll));
    CHECK(!navigation_url_is_same_document(
        navigation, "HTTPS://ACTIONS.TEST:443/a/../page#target"));

    /* History restoration is immediate and request-free in both the
       cooperative begin API and the synchronous chrome API. */
    bool history_back = browser_engine_begin_navigation_history(
        engine, false, 1024u, 1000);
    CHECK(history_back
          && browser_engine_navigation_status(engine)
                 == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page") == 0
          && navigation_current(navigation)->scroll_y == 0
          && navigation->loads_started == loads_before);
    bool history_forward = browser_engine_history_move(engine, true);
    if (!history_forward
        || strcmp(navigation_current(navigation)->url,
                  "https://actions.test/page#target") != 0
        || navigation_current(navigation)->scroll_y != target_scroll
        || navigation->loads_started != loads_before) {
        fprintf(stderr,
                "same-document forward=%d url=%s scroll=%d/%d loads=%zu/%zu "
                "error=%s\n", history_forward,
                navigation_current(navigation)->url,
                navigation_current(navigation)->scroll_y, target_scroll,
                navigation->loads_started, loads_before,
                browser_engine_last_error(engine));
    }
    CHECK(history_forward
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page#target") == 0
          && navigation_current(navigation)->scroll_y == target_scroll
          && navigation->loads_started == loads_before);

    CHECK(void_link != NULL && browser_engine_focus_node(engine, void_link)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL
          && action.activation_outcome == CONTROLLER_ACTIVATION_DELIVERED
          && actionable_has_attribute(
                 document_body_node(document), "data-void")
          && navigation->history_count == 2u
          && navigation->loads_started == loads_before);
    CHECK(cancel != NULL && browser_engine_focus_node(engine, cancel)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome == CONTROLLER_ACTIVATION_CANCELLED
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page#target") == 0
          && navigation->history_count == 2u
          && navigation->loads_started == loads_before);
    bool events_probed = script_runtime_evaluate_diagnostic(
        navigation->page.runtime,
        "globalThis.pocSummary=hashCount+':'+popCount+':'"
        "+location.hash+':'+history.length",
        "<fragment-events>", &navigation->page.script_result);
    if (!events_probed
        || strcmp(navigation->page.script_result.summary,
                  "3:2:#target:2") != 0) {
        fprintf(stderr, "fragment events probe=%d summary=%s error=%s\n",
                events_probed, navigation->page.script_result.summary,
                navigation->page.script_result.error);
    }
    CHECK(events_probed
          && strcmp(navigation->page.script_result.summary,
                    "3:2:#target:2") == 0);
    /* Extracted trees prefix both id and legacy name targets, never the
       authored fragment exposed in location/history. The hidden raw anchor
       must not win, and committing the named target remains request-free. */
    CHECK(browser_engine_focus_node(engine, named_jump)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page#legacy") == 0
          && navigation_current(navigation)->scroll_y > target_scroll
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);

    /* Relative action resolution must observe a <base> mutation made by the
       just-dispatched hashchange listener. Run this while the realm is still
       healthy, before the deliberate watchdog failures below. */
    bool base_jump_focused = browser_engine_focus_node(engine, base_jump);
    bool base_jump_activated = base_jump_focused
        && browser_engine_activate(engine, &action);
    bool base_mutated = strcmp(navigation->page.resource_base_url,
                               "https://actions.test/mutated/") == 0;
    bool base_relative_focused = base_jump_activated
        && browser_engine_focus_node(engine, base_relative);
    bool base_relative_activated = base_relative_focused
        && browser_engine_activate(engine, &action);
    if (!base_jump_focused || !base_jump_activated
        || action.type != CONTROLLER_ACTION_NAVIGATE || !base_mutated
        || !base_relative_focused || !base_relative_activated
        || strcmp(action.url, "https://actions.test/mutated/next") != 0) {
        fprintf(stderr,
                "base activation jump-focus=%d jump-activate=%d mutated=%d "
                "relative-focus=%d relative-activate=%d type=%d url=%s "
                "current=%s base=%s outcome=%d error=%s\n",
                base_jump_focused, base_jump_activated, base_mutated,
                base_relative_focused, base_relative_activated,
                (int) action.type, action.url,
                navigation_current(navigation)->url,
                navigation->page.resource_base_url,
                (int) action.activation_outcome,
                navigation->page.script_result.error);
    }
    CHECK(base_jump_focused && base_jump_activated && base_mutated
          && base_relative_focused && base_relative_activated
          && action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(action.url,
                    "https://actions.test/mutated/next") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "globalThis.pocSummary=document.baseURI",
              "<post-hash-base>", &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary,
                    navigation->page.resource_base_url) == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "document.getElementById('base').setAttribute('href',"
              "'/before-restore/');globalThis.pocSummary=document.baseURI",
              "<pre-history-base>", &navigation->page.script_result)
          && browser_engine_history_move(engine, false)
          && strcmp(navigation->page.resource_base_url,
                    "https://actions.test/before-restore/") == 0
          && browser_engine_history_move(engine, true)
          && strcmp(navigation->page.resource_base_url,
                    "https://actions.test/mutated/") == 0);

    /* Entering the trusted dispatcher is an authoritative native fact. A
       hostile property getter/checkValidity callback can run before the JS
       event and handler counters advance; neither path may be mistaken for
       a pre-dispatch refusal and replay its native default. Run these before
       the deliberate watchdog failures below so each hostile boundary is
       independently attributed. */
    CHECK(browser_engine_focus_node(engine, getter_submit));
    size_t dispatches_before =
        navigation->page.script_result.event_dispatches_entered;
    size_t events_before = navigation->page.script_result.events_dispatched;
    bool check_activated = browser_engine_activate(engine, &action);
    if (!check_activated || action.type != CONTROLLER_ACTION_NONE
        || action.activation_outcome != CONTROLLER_ACTIVATION_RUNTIME_FAILED
        || action.url[0] != '\0'
        || navigation->page.script_result.event_dispatches_entered
               != dispatches_before + 2u
        || !navigation->page.script_result.event_dispatch_entered
        || navigation->page.script_result.events_dispatched <= events_before
        || !actionable_has_attribute(getter_form, "data-check-ran")) {
        fprintf(stderr,
                "checkValidity activation=%d type=%d outcome=%d url=%s "
                "entered=%zu/%zu event-entered=%d events=%zu/%zu ran=%d "
                "error=%s\n",
                check_activated, (int) action.type,
                (int) action.activation_outcome, action.url,
                navigation->page.script_result.event_dispatches_entered,
                dispatches_before,
                navigation->page.script_result.event_dispatch_entered,
                navigation->page.script_result.events_dispatched,
                events_before,
                actionable_has_attribute(getter_form, "data-check-ran"),
                navigation->page.script_result.error);
    }
    CHECK(check_activated
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_FAILED
          && action.url[0] == '\0'
          && navigation->page.script_result.event_dispatches_entered
                 == dispatches_before + 2u
          && navigation->page.script_result.event_dispatch_entered
          && navigation->page.script_result.events_dispatched > events_before
          && actionable_has_attribute(getter_form, "data-check-ran"));

    CHECK(browser_engine_focus_node(engine, getter_partial));
    dispatches_before =
        navigation->page.script_result.event_dispatches_entered;
    events_before = navigation->page.script_result.events_dispatched;
    bool getter_activated = browser_engine_activate(engine, &action);
    if (!getter_activated || action.type != CONTROLLER_ACTION_NONE
        || action.activation_outcome != CONTROLLER_ACTIVATION_RUNTIME_FAILED
        || navigation->page.script_result.event_dispatches_entered
               != dispatches_before + 1u
        || !navigation->page.script_result.event_dispatch_entered
        || navigation->page.script_result.events_dispatched != events_before
        || !actionable_has_attribute(
               document_body_node(document), "data-getter-ran")
        || actionable_has_attribute(getter_partial, "checked")) {
        fprintf(stderr,
                "getter activation activate=%d type=%d outcome=%d "
                "entered=%zu/%zu events=%zu/%zu ran=%d checked=%d error=%s\n",
                getter_activated, (int) action.type,
                (int) action.activation_outcome,
                navigation->page.script_result.event_dispatches_entered,
                dispatches_before,
                navigation->page.script_result.events_dispatched,
                events_before,
                actionable_has_attribute(
                    document_body_node(document), "data-getter-ran"),
                actionable_has_attribute(getter_partial, "checked"),
                navigation->page.script_result.error);
    }
    CHECK(getter_activated && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_FAILED
          && navigation->page.script_result.event_dispatches_entered
                 == dispatches_before + 1u
          && navigation->page.script_result.event_dispatch_entered
          && navigation->page.script_result.events_dispatched == events_before
          && actionable_has_attribute(
                 document_body_node(document), "data-getter-ran")
          && !actionable_has_attribute(getter_partial, "checked"));

    CHECK(browser_engine_focus_node(engine, failed_link)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_FAILED
          && strcmp(navigation_current(navigation)->url,
                    "https://actions.test/page#base-target") == 0
          && navigation->loads_started == loads_before);
    CHECK(browser_engine_focus_node(engine, failed_submit)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_FAILED
          && action.url[0] == '\0'
          && navigation->loads_started == loads_before);
    /* A failure after click delivery is not permission to replay a native
       default. The bootstrap already toggled this checkbox once; a second
       fallback toggle would silently undo it. */
    bool partial_focused = partial != NULL
        && browser_engine_focus_node(engine, partial);
    bool partial_activated = partial_focused
        && browser_engine_activate(engine, &action);
    if (!partial_activated || action.type != CONTROLLER_ACTION_NONE
        || action.activation_outcome != CONTROLLER_ACTIVATION_RUNTIME_FAILED
        || actionable_has_attribute(partial, "checked")) {
        fprintf(stderr,
                "partial activation focus=%d activate=%d type=%d outcome=%d "
                "checked=%d result=%d events=%zu error=%s\n",
                partial_focused, partial_activated, (int) action.type,
                (int) action.activation_outcome,
                actionable_has_attribute(partial, "checked"),
                navigation->page.script_result.success,
                navigation->page.script_result.events_dispatched,
                navigation->page.script_result.error);
    }
    CHECK(partial_activated
          && action.type == CONTROLLER_ACTION_NONE
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_FAILED
          && !actionable_has_attribute(partial, "checked"));

    /* Native history/bridge publication is authoritative even when the URL
       strings for the advisory JS callback cannot be allocated. The next
       Location property read must lazily reconcile every component rather
       than exposing the old href/hash split. */
    char previous_runtime_url[NAVIGATION_URL_LIMIT];
    CHECK(script_runtime_copy_document_url(
        navigation->page.runtime, previous_runtime_url,
        sizeof(previous_runtime_url)));
    static const char oom_url[] =
        "https://actions.test/reconciled/path?oom=1#current";
    budget_inject_failure_after(navigation->budget, 0u);
    bool callback_committed = script_runtime_commit_same_document(
        navigation->page.runtime, oom_url, previous_runtime_url,
        &navigation->page.script_result);
    budget_clear_failure_injection(navigation->budget);
    CHECK(!callback_committed
          && script_runtime_evaluate_diagnostic(
                 navigation->page.runtime,
                 "globalThis.pocSummary=[location.href,location.protocol,"
                 "location.host,location.pathname,location.search,"
                 "location.hash].join('|')",
                 "<same-document-location-reconcile>",
                 &navigation->page.script_result)
          && strcmp(navigation->page.script_result.summary,
                    "https://actions.test/reconciled/path?oom=1#current|"
                    "https:|actions.test|/reconciled/path|?oom=1|#current")
                 == 0
          && script_runtime_commit_same_document(
                 navigation->page.runtime, previous_runtime_url, oom_url,
                 &navigation->page.script_result));

    /* A parser-circuit teardown can retire the author realm while retaining
       its already-mutated DOM. Latch the authored checked state natively at
       the mutation boundary so a later script-free Reset does not mistake
       the live mutation for the default. */
    CHECK(script_runtime_evaluate_diagnostic(
              navigation->page.runtime,
              "document.getElementById('realm-check').checked=false;"
              "globalThis.pocSummary='realm-mutated'",
              "<checked-default-before-realm-teardown>",
              &navigation->page.script_result)
          && !actionable_has_attribute(realm_check, "checked")
          && browser_engine_set_javascript_enabled(engine, false)
          && navigation->page.runtime == NULL
          && browser_engine_focus_node(engine, realm_reset)
          && browser_engine_activate(engine, &action)
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_UNAVAILABLE
          && actionable_has_attribute(realm_check, "checked"));

    /* With no author realm, the controller supplies the bounded HTML
       defaults itself. These actions remain useful on script-free/fallback
       pages rather than becoming inert controls. */
    static const char controls_page[] =
        "<!doctype html><title>Controls</title><style>"
        "label,input,select,button,summary{display:block;height:24px}</style>"
        "<body><form id=form action=/find>"
        "<input id=check type=checkbox name=check>"
        "<input id=fault-check type=checkbox>"
        "<input id=old-radio type=radio name=pick checked>"
        "<label id=radio-label for=radio>Pick new</label>"
        "<input id=radio type=radio name=pick value=new>"
        "<input id=range type=range min=0 max=4 step=2 value=0>"
        "<input id=range-default type=range>"
        "<select id=select name=choice><option selected value=a>A</option>"
        "<option id=second value=b>B</option></select>"
        "<select id=select-default><option id=default-first>First</option>"
        "<option id=default-second>Second</option></select>"
        "<input id=query name=q value=psp>"
        "<textarea id=notes name=notes>authored</textarea>"
        "<button id=submit>Go</button>"
        "<button id=reset type=reset>Reset</button></form>"
        "<input id=orphan-fault type=checkbox>"
        "<input id=orphan-range type=range value=0 min=0 max=4 step=2>"
        "<details id=details><summary id=summary>More</summary>Text</details>";
    CHECK(browser_engine_commit_html(
        engine, "https://actions.test/controls", controls_page,
        sizeof(controls_page) - 1u, true));
    navigation = browser_engine_navigation(engine);
    document = &navigation->page.document;
    CHECK(navigation->page.runtime == NULL);
    lxb_dom_node_t *check = actionable_find_id(document, "check");
    lxb_dom_node_t *old_radio = actionable_find_id(document, "old-radio");
    lxb_dom_node_t *fault_check = actionable_find_id(document, "fault-check");
    lxb_dom_node_t *orphan_fault = actionable_find_id(
        document, "orphan-fault");
    lxb_dom_node_t *orphan_range = actionable_find_id(
        document, "orphan-range");
    lxb_dom_node_t *radio_label = actionable_find_id(document, "radio-label");
    lxb_dom_node_t *radio = actionable_find_id(document, "radio");
    lxb_dom_node_t *range = actionable_find_id(document, "range");
    lxb_dom_node_t *range_default = actionable_find_id(
        document, "range-default");
    lxb_dom_node_t *select = actionable_find_id(document, "select");
    lxb_dom_node_t *second = actionable_find_id(document, "second");
    lxb_dom_node_t *select_default = actionable_find_id(
        document, "select-default");
    lxb_dom_node_t *default_second = actionable_find_id(
        document, "default-second");
    lxb_dom_node_t *query = actionable_find_id(document, "query");
    lxb_dom_node_t *notes = actionable_find_id(document, "notes");
    lxb_dom_node_t *reset = actionable_find_id(document, "reset");
    lxb_dom_node_t *summary = actionable_find_id(document, "summary");
    lxb_dom_node_t *details = actionable_find_id(document, "details");
    lxb_dom_node_t *submit = actionable_find_id(document, "submit");
    lxb_dom_node_t *form = actionable_find_id(document, "form");
    CHECK(check != NULL && fault_check != NULL && orphan_fault != NULL
          && orphan_range != NULL && old_radio != NULL
          && radio_label != NULL
          && radio != NULL && range != NULL && range_default != NULL
          && select != NULL && second != NULL && select_default != NULL
          && default_second != NULL && reset != NULL && summary != NULL
          && details != NULL && submit != NULL && form != NULL
          && query != NULL && notes != NULL);
    CHECK(browser_engine_focus_node(engine, check)
          && browser_engine_activate(engine, &action)
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_UNAVAILABLE
          && actionable_has_attribute(check, "checked")
          && browser_engine_focus_node(engine, radio_label)
          && browser_engine_activate(engine, &action)
          && actionable_has_attribute(radio, "checked")
          && !actionable_has_attribute(old_radio, "checked")
          && browser_engine_focus_node(engine, summary)
          && browser_engine_activate(engine, &action)
          && actionable_has_attribute(details, "open")
          && browser_engine_focus_node(engine, select)
          && browser_engine_activate(engine, &action)
          && actionable_has_attribute(second,
                                      "data-tilefinch-option-selected")
          && browser_engine_focus_node(engine, select_default)
          && browser_engine_activate(engine, &action)
          && actionable_has_attribute(
                 default_second, "data-tilefinch-option-selected")
          && browser_engine_focus_node(engine, range)
          && browser_engine_activate(engine, &action));
    size_t value_length = 0;
    const char *value = document_control_value(range, &value_length);
    CHECK(value != NULL && value_length == 1u && value[0] == '2');
    CHECK(browser_engine_focus_node(engine, range_default)
          && browser_engine_activate(engine, &action));
    value = document_control_value(range_default, &value_length);
    CHECK(value != NULL && value_length == 2u
          && memcmp(value, "51", 2u) == 0);
    /* Native default mutation plus refresh/relayout is transactional under
       bounded page-allocation failures. Prime the retained default, then
       sweep enough allocation boundaries to exercise both admission and
       relayout rollback without leaving the checkbox half-toggled. */
    CHECK(browser_engine_focus_node(engine, fault_check)
          && browser_engine_activate(engine, &action)
          && browser_engine_activate(engine, &action)
          && !actionable_has_attribute(fault_check, "checked"));
    CHECK(browser_engine_focus_node(engine, fault_check));
    bool fault_checked_before = actionable_has_attribute(
        fault_check, "checked");
    size_t fault_owned_before = navigation->budget->current;
    size_t fault_states_before = document->control_state_count;
    budget_inject_failure_after(navigation->budget, 1u);
    bool fault_accepted = controller_activate(
        browser_engine_controller(engine), &action);
    budget_clear_failure_injection(navigation->budget);
    CHECK(!fault_accepted
          && actionable_has_attribute(fault_check, "checked")
                 == fault_checked_before
          && navigation->budget->current == fault_owned_before
          && document->control_state_count == fault_states_before);
    CHECK(browser_engine_focus_node(engine, orphan_fault));
    fault_owned_before = navigation->budget->current;
    fault_states_before = document->control_state_count;
    budget_inject_failure_after(navigation->budget, 1u);
    CHECK(!browser_engine_activate(engine, &action));
    budget_clear_failure_injection(navigation->budget);
    CHECK(!actionable_has_attribute(orphan_fault, "checked")
          && navigation->budget->current == fault_owned_before
          && document->control_state_count == fault_states_before);
    size_t range_owned_before = 0;
    size_t range_states_before = 0;
    CHECK(browser_engine_focus_node(engine, orphan_range));
    range_owned_before = navigation->budget->current;
    range_states_before = document->control_state_count;
    budget_inject_failure_after(navigation->budget, 3u);
    CHECK(!browser_engine_activate(engine, &action));
    budget_clear_failure_injection(navigation->budget);
    value = document_control_value(orphan_range, &value_length);
    CHECK(value == NULL
          && navigation->budget->current == range_owned_before
          && document->control_state_count == range_states_before);
    CHECK(browser_engine_focus_node(engine, range));
    range_owned_before = navigation->budget->current;
    range_states_before = document->control_state_count;
    budget_inject_failure_after(navigation->budget, 1u);
    CHECK(!browser_engine_activate(engine, &action));
    budget_clear_failure_injection(navigation->budget);
    value = document_control_value(range, &value_length);
    CHECK(value != NULL && value_length == 1u && value[0] == '2'
          && navigation->budget->current == range_owned_before
          && document->control_state_count == range_states_before);
    CHECK(browser_engine_focus_node(engine, query));
    size_t edit_owned_before = navigation->budget->current;
    budget_inject_failure_after(navigation->budget, 0u);
    CHECK(!browser_engine_replace_text(
        engine, "not-applied", sizeof("not-applied") - 1u));
    budget_clear_failure_injection(navigation->budget);
    value = document_control_value(query, &value_length);
    CHECK(value == NULL
          && navigation->budget->current == edit_owned_before);
    CHECK(browser_engine_replace_text(
              engine, "edited", sizeof("edited") - 1u)
          && browser_engine_focus_node(engine, notes)
          && browser_engine_replace_text(
              engine, "changed", sizeof("changed") - 1u));
    /* A failed edit after the larger live-value allocation has landed must
       restore both the prior bytes and the exact Budget charge. Keeping a
       513-byte allocation for the six-byte value would make repeated failed
       edits consume page budget even though no visible edit committed. */
    CHECK(browser_engine_focus_node(engine, query));
    edit_owned_before = navigation->budget->current;
    char oversized_edit[DOCUMENT_CONTROL_VALUE_LIMIT];
    memset(oversized_edit, 'x', sizeof(oversized_edit));
    budget_inject_failure_after(navigation->budget, 1u);
    CHECK(!browser_engine_replace_text(
        engine, oversized_edit, sizeof(oversized_edit)));
    budget_clear_failure_injection(navigation->budget);
    value = document_control_value(query, &value_length);
    CHECK(value != NULL && value_length == sizeof("edited") - 1u
          && memcmp(value, "edited", value_length) == 0
          && navigation->budget->current == edit_owned_before);
    CHECK(browser_engine_focus_node(engine, reset));
    size_t reset_owned_before = navigation->budget->current;
    size_t reset_states_before = document->control_state_count;
    budget_inject_failure_after(navigation->budget, 5u);
    CHECK(!browser_engine_activate(engine, &action));
    budget_clear_failure_injection(navigation->budget);
    value = document_control_value(query, &value_length);
    CHECK(actionable_has_attribute(check, "checked")
          && actionable_has_attribute(radio, "checked")
          && !actionable_has_attribute(old_radio, "checked")
          && actionable_has_attribute(
                 second, "data-tilefinch-option-selected")
          && value != NULL && value_length == sizeof("edited") - 1u
          && memcmp(value, "edited", value_length) == 0);
    value = document_control_value(notes, &value_length);
    CHECK(value != NULL && value_length == sizeof("changed") - 1u
          && memcmp(value, "changed", value_length) == 0
          && navigation->budget->current == reset_owned_before
          && document->control_state_count == reset_states_before);
    CHECK(browser_engine_activate(engine, &action)
          && !actionable_has_attribute(check, "checked")
          && actionable_has_attribute(old_radio, "checked")
          && !actionable_has_attribute(radio, "checked")
          && !actionable_has_attribute(
                 second, "data-tilefinch-option-selected"));
    value = document_control_value(range, &value_length);
    CHECK(value != NULL && value_length == 1u && value[0] == '0');
    value = document_control_value(query, &value_length);
    CHECK(value != NULL && value_length == sizeof("psp") - 1u
          && memcmp(value, "psp", value_length) == 0);
    value = document_control_value(notes, &value_length);
    CHECK(value != NULL && value_length == sizeof("authored") - 1u
          && memcmp(value, "authored", value_length) == 0);
    size_t script_free_loads = navigation->loads_started;
    bool submit_focused = browser_engine_focus_node(engine, submit);
    bool submit_activated = submit_focused
        && browser_engine_activate(engine, &action);
    if (!submit_activated || action.type != CONTROLLER_ACTION_FORM_SUBMIT
        || strcmp(action.url,
                  "https://actions.test/find?pick=on&choice=a&q=psp&notes=authored")
               != 0) {
        fprintf(stderr,
                "script-free submit focus=%d activate=%d type=%d outcome=%d "
                "method=%s url=%s body=%s\n", submit_focused,
                submit_activated, (int) action.type,
                (int) action.activation_outcome, action.method, action.url,
                action.body);
    }
    CHECK(submit_activated
          && action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && action.activation_outcome
                 == CONTROLLER_ACTIVATION_RUNTIME_UNAVAILABLE
          && strcmp(action.method, "GET") == 0
          && strcmp(action.url,
                    "https://actions.test/find?pick=on&choice=a&q=psp&notes=authored")
                 == 0
          && navigation->loads_started == script_free_loads);
    ControllerAction poisoned_action;
    memset(&poisoned_action, 0xa5, sizeof(poisoned_action));
    CHECK(controller_build_form_action(
              browser_engine_controller(engine), form, submit, false,
              &poisoned_action)
          && poisoned_action.type == CONTROLLER_ACTION_FORM_SUBMIT
          && poisoned_action.activation_outcome
                 == CONTROLLER_ACTIVATION_NOT_DISPATCHED
          && strcmp(poisoned_action.url,
                    "https://actions.test/find?pick=on&choice=a&q=psp&notes=authored")
                 == 0);

    /* Structural refusal is decided before Reset captures any defaults.
       A hostile oversized form therefore cannot fill the 128-state table or
       retain Budget ownership merely by being reset repeatedly. */
    char oversized_form[4096];
    size_t oversized_length = (size_t) snprintf(
        oversized_form, sizeof(oversized_form),
        "<!doctype html><style>button,input{display:block;height:1px}</style>"
        "<form><button id=overflow-reset type=reset>Reset</button>");
    CHECK(oversized_length < sizeof(oversized_form));
    for (size_t at = 0; at < 129u; at++) {
        int added = snprintf(
            oversized_form + oversized_length,
            sizeof(oversized_form) - oversized_length,
            "<input name=x%zu>", at);
        CHECK(added > 0
              && (size_t) added
                     < sizeof(oversized_form) - oversized_length);
        oversized_length += (size_t) added;
    }
    CHECK(oversized_length + sizeof("</form>") <= sizeof(oversized_form));
    memcpy(oversized_form + oversized_length, "</form>", sizeof("</form>"));
    oversized_length += sizeof("</form>") - 1u;
    CHECK(browser_engine_commit_html(
        engine, "https://actions.test/oversized-reset", oversized_form,
        oversized_length, true));
    navigation = browser_engine_navigation(engine);
    document = &navigation->page.document;
    lxb_dom_node_t *overflow_reset = actionable_find_id(
        document, "overflow-reset");
    CHECK(overflow_reset != NULL
          && browser_engine_focus_node(engine, overflow_reset));
    size_t overflow_owned_before = navigation->budget->current;
    size_t overflow_states_before = document->control_state_count;
    CHECK(!browser_engine_activate(engine, &action)
          && navigation->budget->current == overflow_owned_before
          && document->control_state_count == overflow_states_before);

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

/* Same-document events run synchronously inside the URL/history bridge.  The
   browser must settle their mutation journal before it resolves fragment or
   history geometry; waiting for the next ordinary runtime tick leaves the
   first painted frame and restored focus/scroll bound to stale boxes. */
static int test_same_document_event_mutations_settle_immediately(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    static const char page[] =
        "<!doctype html><title>Same-document settlement</title>"
        "<style>body{margin:0}a{display:block;height:24px}"
        "#hash-space,#pop-space{height:0}.tail{height:900px}</style>"
        "<a id=jump href=#target>Jump</a>"
        "<a id=refused-link href=#refused>Refused settlement</a>"
        "<div id=volatile><a href=#old>Borrowed text</a>"
        "<input value=borrowed></div>"
        "<div id=hash-space></div><div id=pop-space></div>"
        "<a id=target>Target</a><a id=refused>Refused target</a>"
        "<div class=tail></div>"
        "<script>"
        "addEventListener('hashchange',()=>{"
        "if(location.hash==='#refused')"
        "document.getElementById('volatile').textContent='replacement';"
        "document.getElementById('hash-space').style.height="
        "location.hash==='#target'?'360px':"
        "location.hash==='#refused'?'480px':'120px';});"
        "addEventListener('popstate',()=>{"
        "document.getElementById('pop-space').style.height="
        "location.hash==='#target'?'180px':'240px';});"
        "</script>";
    CHECK(browser_engine_commit_html(
        engine, "https://settle.test/page", page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    PocDocument *document = &navigation->page.document;
    lxb_dom_node_t *jump = actionable_find_id(document, "jump");
    lxb_dom_node_t *refused = actionable_find_id(document, "refused-link");
    lxb_dom_node_t *target = actionable_find_id(document, "target");
    lxb_dom_node_t *hash_space = actionable_find_id(document, "hash-space");
    lxb_dom_node_t *pop_space = actionable_find_id(document, "pop-space");
    const LayoutNodeBox *initial_target = layout_box_for_node(
        &navigation->page.layout, target);
    CHECK(jump != NULL && refused != NULL && target != NULL && hash_space != NULL
          && pop_space != NULL && initial_target != NULL);
    int initial_target_y = initial_target->y;
    size_t loads_before = navigation->loads_started;
    uint64_t generation_before = navigation->generation;
    size_t relayouts_before = navigation->incremental_relayouts;
    ControllerAction action = {0};

    CHECK(browser_engine_focus_node(engine, jump)
          && browser_engine_activate(engine, &action)
          && action.type == CONTROLLER_ACTION_CONTROL
          && strcmp(navigation_current(navigation)->url,
                    "https://settle.test/page#target") == 0);
    const LayoutNodeBox *hash_box = layout_box_for_node(
        &navigation->page.layout, hash_space);
    const LayoutNodeBox *pop_box = layout_box_for_node(
        &navigation->page.layout, pop_space);
    const LayoutNodeBox *target_box = layout_box_for_node(
        &navigation->page.layout, target);
    CHECK(hash_box != NULL && hash_box->height == 360
          && pop_box != NULL && pop_box->height == 0
          && target_box != NULL && target_box->y == initial_target_y + 360
          && navigation_current(navigation)->scroll_y == target_box->y
          && navigation->incremental_relayouts == relayouts_before + 1u
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);
    int target_scroll = navigation_current(navigation)->scroll_y;

    relayouts_before = navigation->incremental_relayouts;
    CHECK(browser_engine_begin_navigation_history(
              engine, false, 1024u, 1000)
          && browser_engine_navigation_status(engine)
                 == BROWSER_NAVIGATION_JOB_SUCCEEDED
          && strcmp(navigation_current(navigation)->url,
                    "https://settle.test/page") == 0
          && navigation_current(navigation)->scroll_y == 0);
    hash_box = layout_box_for_node(&navigation->page.layout, hash_space);
    pop_box = layout_box_for_node(&navigation->page.layout, pop_space);
    target_box = layout_box_for_node(&navigation->page.layout, target);
    CHECK(hash_box != NULL && hash_box->height == 120
          && pop_box != NULL && pop_box->height == 240
          && target_box != NULL && target_box->y == initial_target_y + 360
          && navigation->incremental_relayouts == relayouts_before + 1u
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);

    relayouts_before = navigation->incremental_relayouts;
    CHECK(browser_engine_history_move(engine, true)
          && strcmp(navigation_current(navigation)->url,
                    "https://settle.test/page#target") == 0
          && navigation_current(navigation)->scroll_y == target_scroll);
    hash_box = layout_box_for_node(&navigation->page.layout, hash_space);
    pop_box = layout_box_for_node(&navigation->page.layout, pop_space);
    target_box = layout_box_for_node(&navigation->page.layout, target);
    CHECK(hash_box != NULL && hash_box->height == 360
          && pop_box != NULL && pop_box->height == 180
          && target_box != NULL && target_box->y == initial_target_y + 540
          && navigation->incremental_relayouts == relayouts_before + 1u
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);

    /* Once the callback has destroyed text, link, and control nodes, a real
       replacement refusal must retire the complete page and shell. Keeping
       the incumbent pixels/layout would leave borrowed Lexbor pointers live. */
    size_t history_before_refusal = navigation->history_count;
    size_t relayouts_before_refusal = navigation->incremental_relayouts;
    CHECK(browser_engine_focus_node(engine, refused));
    navigation_test_refuse_next_same_document_relayout();
    CHECK(!browser_engine_activate(engine, &action)
          && strcmp(navigation_current(navigation)->url,
                    "https://settle.test/page#refused") == 0
          && navigation->history_count == history_before_refusal + 1u
          && navigation->incremental_relayouts == relayouts_before_refusal
          && navigation->loads_started == loads_before
          && navigation->generation == generation_before);
    CHECK(!navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && strstr(browser_engine_last_error(engine), "settled") != NULL);
    CHECK(!browser_engine_render_frame(engine, 0));
    CHECK(!browser_engine_activate(engine, &action));

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_contenteditable_relayout_refusal_retires_shell(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);
    static const char page[] =
        "<!doctype html><body><div id=edit contenteditable=true>"
        "Borrowed editable text</div><a href=/next>Next</a></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://editable.test/", page, sizeof(page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *editable = actionable_find_id(
        &navigation->page.document, "edit");
    CHECK(editable != NULL && browser_engine_focus_node(engine, editable));

    navigation_test_refuse_next_relayout();
    CHECK(!browser_engine_replace_text(
              engine, "replacement", sizeof("replacement") - 1u)
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && !browser_engine_render_frame(engine, 0));

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_author_relayout_refusal_retires_shell(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    static const char activation_page[] =
        "<!doctype html><body><div id=volatile>Borrowed activation text</div>"
        "<a id=go href=/next onclick=\"document.getElementById('volatile')"
        ".textContent='replacement'\">Go</a></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://author-refusal.test/", activation_page,
        sizeof(activation_page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *go = actionable_find_id(
        &navigation->page.document, "go");
    ControllerAction action = {0};
    CHECK(go != NULL && browser_engine_focus_node(engine, go));
    navigation_test_refuse_next_relayout();
    CHECK(!browser_engine_activate(engine, &action)
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && !browser_engine_render_frame(engine, 0));

    static const char timer_page[] =
        "<!doctype html><body><div id=volatile>Borrowed timer text</div>"
        "<script>setTimeout(()=>document.getElementById('volatile')"
        ".textContent='replacement',0)</script></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://author-refusal.test/timer", timer_page,
        sizeof(timer_page) - 1u, true));
    navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.script_result.pending_tasks != 0u);
    navigation_test_refuse_next_relayout();
    CHECK(!browser_engine_advance_runtime(engine, 1u, 4u, NULL)
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && !browser_engine_render_frame(engine, 0));

    static const char external_page[] =
        "<!doctype html><body><script id=external></script>"
        "<p>incumbent</p></body>";
    static const char external_source[] =
        "document.body.innerHTML='<p>replacement</p>'";
    CHECK(browser_engine_commit_html(
        engine, "https://refresh-refusal.test/external", external_page,
        sizeof(external_page) - 1u, true));
    navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *external = actionable_find_id(
        &navigation->page.document, "external");
    CHECK(external != NULL);
    script_runtime_test_refuse_next_document_refresh();
    CHECK(!navigation_evaluate_external_script(
              navigation, external, external_source,
              sizeof(external_source) - 1u,
              "https://refresh-refusal.test/external.js")
          && !navigation->page.loaded
          && navigation->page.document.html == NULL
          && navigation->page.runtime == NULL
          && navigation->page.layout.count == 0u
          && !browser_engine_render_frame(engine, 0));

    static const char sticky_page[] =
        "<!doctype html><body><p>sticky failure</p></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://refresh-refusal.test/sticky", sticky_page,
        sizeof(sticky_page) - 1u, true));
    navigation = browser_engine_navigation(engine);
    script_runtime_test_mark_document_refresh_failed(
        navigation->page.runtime);
    /* Even an otherwise successful idle advance must honor the sticky fatal
       boundary before frame/resource processing can reuse DOM state. */
    CHECK(!browser_engine_advance_runtime(engine, 1u, 4u, NULL)
          && !navigation->page.loaded
          && navigation->page.document.html == NULL
          && navigation->page.runtime == NULL
          && navigation->page.layout.count == 0u);

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_history_restores_bounded_form_state(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    static const char form_page[] =
        "<!doctype html><body><input id=query value=initial>"
        "<input id=secret type=password value=default></body>";
    static const char second_page[] =
        "<!doctype html><body><p>Second page</p></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://history-form.test/search", form_page,
        sizeof(form_page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *query = actionable_find_id(
        &navigation->page.document, "query");
    lxb_dom_node_t *secret = actionable_find_id(
        &navigation->page.document, "secret");
    CHECK(query != NULL && secret != NULL);
    CHECK(document_control_value_set(
              &navigation->page.document, query, "portable", 8u));
    CHECK(document_control_value_set(
              &navigation->page.document, secret, "private", 7u));
    /* Fragment entries retain the same document identity and must continue
       storing/restoring non-sensitive controls. */
    CHECK(navigation_commit_same_document_url(
        navigation, "https://history-form.test/search#filters"));
    CHECK(browser_engine_commit_html(
        engine, "https://history-form.test/results", second_page,
        sizeof(second_page) - 1u, true));
    char replay_directory[128] = {0};
    CHECK(write_history_form_replay(
              replay_directory, form_page, sizeof(form_page) - 1u,
              "https://history-form.test/search")
          && fetch_trace_replay_begin(
                 replay_directory, error, sizeof(error)));
    CHECK(browser_engine_history_move(engine, false));
    fetch_trace_end();
    remove_large_youtube_replay(replay_directory);

    navigation = browser_engine_navigation(engine);
    query = actionable_find_id(&navigation->page.document, "query");
    secret = actionable_find_id(&navigation->page.document, "secret");
    size_t value_length = 0u;
    const char *value = document_control_value(query, &value_length);
    CHECK(value != NULL && value_length == 8u
          && memcmp(value, "portable", 8u) == 0);
    value = document_control_value(secret, &value_length);
    CHECK(value == NULL || value_length != 7u
          || memcmp(value, "private", 7u) != 0);

    /* A history request can redirect to a new document whose form happens to
       use the same bounded ordinal.  The old document's value must not cross
       that validation/use boundary. */
    static const char redirected_page[] =
        "<!doctype html><body><input id=query value=destination></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://history-form.test/results", second_page,
        sizeof(second_page) - 1u, true));
    CHECK(write_history_form_replay(
              replay_directory, redirected_page,
              sizeof(redirected_page) - 1u,
              "https://other-origin.test/landing")
          && fetch_trace_replay_begin(
                 replay_directory, error, sizeof(error)));
    CHECK(browser_engine_history_move(engine, false));
    fetch_trace_end();
    remove_large_youtube_replay(replay_directory);
    navigation = browser_engine_navigation(engine);
    query = actionable_find_id(&navigation->page.document, "query");
    value = document_control_value(query, &value_length);
    size_t authored_length = 0u;
    const char *authored = document_attribute(
        query, "value", &authored_length);
    CHECK(authored != NULL && authored_length == 11u
          && memcmp(authored, "destination", 11u) == 0
          && (value == NULL || value_length != 8u
              || memcmp(value, "portable", 8u) != 0));
    static const char redirected_url[] =
        "https://other-origin.test/landing";
    CHECK(strncmp(navigation->page.document_url, redirected_url,
                  sizeof(redirected_url) - 1u) == 0
          && (navigation->page.document_url[sizeof(redirected_url) - 1u]
                  == '\0'
              || navigation->page.document_url[sizeof(redirected_url) - 1u]
                  == '#'));
    CHECK(navigation_url_is_same_document(
        navigation, "https://other-origin.test/landing#details"));
    CHECK(!navigation_url_is_same_document(
        navigation, "https://history-form.test/search#details"));
    BrowserViewSnapshot redirected_view = {0};
    CHECK(browser_engine_view_snapshot(engine, &redirected_view)
          && strcmp(redirected_view.url,
                    navigation->page.document_url) == 0
          && redirected_view.secure);

    /* Leaving the redirected document must not overwrite the original
       history entry's saved controls, either. */
    NavigationEntry *original_entry =
        &navigation->history[navigation->history_index];
    int original_focus_kind = original_entry->focus_kind;
    size_t original_focus_index = original_entry->focus_index;
    /* An out-of-layout ordinal is a legal stale restoration hint; an input
       on the redirected document must not replace it with its own ordinal. */
    original_entry->focus_index = NAVIGATION_HISTORY_CONTROL_LIMIT;
    CHECK(browser_engine_focus_move(engine, true));
    CHECK(original_entry->focus_kind == original_focus_kind
          && original_entry->focus_index == NAVIGATION_HISTORY_CONTROL_LIMIT);
    original_entry->focus_index = original_focus_index;
    CHECK(document_control_value_set(
        &navigation->page.document, query, "cross-origin", 12u));
    CHECK(browser_engine_commit_html(
        engine, "https://history-form.test/after-redirect", second_page,
        sizeof(second_page) - 1u, true));
    CHECK(write_history_form_replay(
              replay_directory, form_page, sizeof(form_page) - 1u,
              "https://history-form.test/search")
          && fetch_trace_replay_begin(
                 replay_directory, error, sizeof(error)));
    CHECK(browser_engine_history_move(engine, false));
    fetch_trace_end();
    remove_large_youtube_replay(replay_directory);
    navigation = browser_engine_navigation(engine);
    query = actionable_find_id(&navigation->page.document, "query");
    value = document_control_value(query, &value_length);
    CHECK(value != NULL && value_length == 8u
          && memcmp(value, "portable", 8u) == 0);

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_post_mutation_document_refresh_refusal_retires_shell(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    CHECK(engine != NULL);

    static const char activation_page[] =
        "<!doctype html><body><div id=volatile><a href=/borrowed>"
        "Borrowed activation text</a><input value=borrowed></div>"
        "<button id=replace onclick=\"document.getElementById('volatile')"
        ".textContent='replacement'\">Replace</button></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://refresh-refusal.test/activation", activation_page,
        sizeof(activation_page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    lxb_dom_node_t *replace = actionable_find_id(
        &navigation->page.document, "replace");
    ControllerAction action = {0};
    CHECK(replace != NULL && browser_engine_focus_node(engine, replace));
    script_runtime_test_refuse_next_document_refresh();
    bool activated = browser_engine_activate(engine, &action);
    CHECK(!activated
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && strstr(navigation->last_error,
                    "refresh mutated DOM") != NULL
          && !browser_engine_render_frame(engine, 0)
          && !browser_engine_activate(engine, &action));

    static const char timer_page[] =
        "<!doctype html><body><div id=volatile><a href=/borrowed>"
        "Borrowed timer text</a><input value=borrowed></div>"
        "<script>setTimeout(()=>document.getElementById('volatile')"
        ".innerHTML='<p>replacement</p>',0)</script></body>";
    CHECK(browser_engine_commit_html(
        engine, "https://refresh-refusal.test/timer", timer_page,
        sizeof(timer_page) - 1u, true));
    navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.script_result.pending_tasks != 0u);
    script_runtime_test_refuse_next_document_refresh();
    CHECK(!browser_engine_advance_runtime(engine, 1u, 4u, NULL)
          && !navigation->page.loaded
          && navigation->page.layout.count == 0u
          && browser_engine_render_metrics_view(engine) == NULL
          && !browser_engine_render_frame(engine, 0));

    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}
