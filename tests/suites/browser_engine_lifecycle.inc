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
          && config.javascript.maximum_total_bytes == 1u * MIB);
    browser_config_init(&config, &profile);
    CHECK(browser_config_apply_psp_memory_profile(
              &config, BROWSER_PSP_MEMORY_REALISTIC)
          && config.javascript.heap_limit == 5u * MIB
          && config.javascript.maximum_total_bytes == 2u * MIB);
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
    BrowserOptionalMemoryReclaim reclaimed = {0};
    CHECK(browser_engine_reclaim_optional_memory(engine, &reclaimed));
    CHECK(reclaimed.total_bytes
              == reclaimed.javascript_bytes
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
    CHECK(browser_engine_render_frame_bounded(engine, 10000, 1)
              == BROWSER_RENDER_JOB_PENDING);
    BrowserRenderJobStatus frame_job = BROWSER_RENDER_JOB_PENDING;
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
          && metrics.loads_started == 2 && metrics.loads_committed == 1
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
          && input_action.type == CONTROLLER_ACTION_NAVIGATE
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
