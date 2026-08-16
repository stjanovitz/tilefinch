static bool test_sectioned_path(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    CompressedSectionStore store = {0};
    SectionStoreStreamBuilder builder = {0};
    bool ready = navigation_init(&navigation, &budget, 2)
        && prior_page(&navigation) && replay_begin();
    FetchSchedulerDomain *incumbent_domain = ready
        ? navigation.page.fetch_domain : NULL;
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_sectioned(
        &navigation, generation,
        "https://stream.test/document#section", 4096, 1000, 480,
        NULL, NULL, true, &store, &builder, 4096, 8192, 0) : NULL;
    NavigationLoadQuota quota = {.fetch = {2, 23, 100000}};
    for (size_t i = 0; load != NULL && i < 64
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    bool ok = load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && store.section_count == 0
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && store.section_count != 0
        && navigation.page.fetch_domain == incumbent_domain
        && strcmp(navigation.page.document.title, "Stream & split") == 0
        && strstr(navigation_current(&navigation)->url, "#section") != NULL;
    navigation_load_destroy(load);
    fetch_trace_end();
    section_store_destroy(&store);
    navigation_destroy(&navigation);
    return ok && budget.current == 0;
}

static bool test_authoritative_layout_resumes_across_finalize_ticks(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed
        && navigation_init(&navigation, &budget, 4)
        && resumable_layout_replay_begin();
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://layout-resume.test/document", 8192, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 4,
            .maximum_body_bytes = 4096,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 4096,
        .maximum_parser_time_us = 10000,
        .maximum_finalize_work_units = 4096,
        .maximum_finalize_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 32
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    size_t finalize_calls = 0;
    while (load != NULL && finalize_calls < 32
           && navigation_load_status(load) != NAVIGATION_LOAD_SUCCEEDED) {
        NavigationLoadStatus status = navigation_load_status(load);
        if (status != NAVIGATION_LOAD_READY_TO_FINISH
            && status != NAVIGATION_LOAD_FINALIZING) break;
        (void) navigation_load_finish(load, &quota);
        finalize_calls++;
    }
    NavigationLoadMetrics metrics = {0};
    bool ok = load != NULL
        && navigation_load_metrics(load, &metrics)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && navigation.page.loaded
        && navigation.page.document.node_count >= 300
        && navigation.page.layout.performance.resumable_phases >= 8
        && navigation.page.layout.performance.resumable_passes == 1
        && finalize_calls >= 10
        && metrics.finalize_yields >= 9;
    if (!ok) {
        fprintf(stderr,
                "resumable-finalize ready=%d load=%p status=%d loaded=%d "
                "nodes=%zu phases=%zu passes=%zu calls=%zu yields=%zu "
                "error=%s\n",
                ready, (void *) load,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.page.loaded, navigation.page.document.node_count,
                navigation.page.layout.performance.resumable_phases,
                navigation.page.layout.performance.resumable_passes,
                finalize_calls, metrics.finalize_yields,
                navigation.last_error);
    }
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_resumable_layout_cancel_preserves_incumbent(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed
        && navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation)
        && resumable_layout_replay_begin();
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation,
        "https://layout-resume.test/document", 8192, 1000, 480,
        NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {4, 8192, 10000},
        .maximum_parser_body_bytes = 8192,
        .maximum_parser_time_us = 10000,
        .maximum_finalize_work_units = 4096,
        .maximum_finalize_time_us = 10000
    };
    for (size_t i = 0; load != NULL && i < 32
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    for (size_t i = 0; load != NULL && i < 3; i++) {
        (void) navigation_load_finish(load, &quota);
    }
    bool was_finalizing = load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_FINALIZING;
    if (was_finalizing) {
        navigation_load_cancel(load, "cancel resumable layout test");
    }
    const NavigationEntry *current = navigation_current(&navigation);
    bool ok = was_finalizing
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && navigation.page.loaded && current != NULL
        && strcmp(current->url, "https://prior.test/#old") == 0
        && strcmp(navigation.page.document.title, "Prior") == 0;
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_response_headers_precede_parser_script_and_viewport(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation.declared_css_width = 1024;
        navigation.declared_css_height = 581;
    }
    bool replay_ready = navigation_ready
        && header_cookie_viewport_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool loaded = replay_ready && navigation_load_url(
        &navigation, generation, "https://headers.test/document",
        4096, 1000, 480, NULL, NULL, true);
    static const char geometry_probe[] =
        "globalThis.pocSummary=[innerWidth,innerHeight,"
        "document.documentElement.clientWidth,"
        "document.documentElement.clientHeight,document.body.clientWidth,"
        "visualViewport.width,screen.width,"
        "document.cookie.includes('header_cookie=seen')].join(':')";
    ScriptResult geometry = {0};
    bool geometry_ok = loaded && navigation.page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, geometry_probe,
            "<stream-viewport-geometry>", &geometry);
    bool ok = loaded
        && strcmp(navigation.page.script_result.summary,
                  "1024:581:true") == 0
        && geometry_ok
        && strcmp(geometry.summary,
                  "1024:581:1024:581:1008:1024:480:true") == 0;
    if (!ok) {
        fprintf(stderr,
                "header/viewport boundary loaded=%d parser=\"%s\" "
                "geometry=\"%s\" error=\"%s\"\n",
                loaded ? 1 : 0, navigation.page.script_result.summary,
                geometry.summary, navigation.last_error);
    }
    if (replay_ready) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_response_csp_gates_parser_and_resource_paths(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 32 * 1024,
            4, 64 * 1024, 32 * 1024, 128 * 1024, 1000);
    }
    bool replay_ready = navigation_ready && csp_gate_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool loaded = replay_ready && navigation_load_url(
        &navigation, generation, "https://csp.test/document",
        4096, 1000, 480, NULL, NULL, true);
    bool ok = loaded
        && navigation.page.document.content_security_policy.header_present
        && navigation.page.document.content_security_policy.valid
        && strcmp(navigation.page.script_result.summary,
                  "allowed-nonce") == 0
        && navigation.page.external_stylesheets.attempted == 0
        && navigation.page.images.stats.attempted == 0
        && navigation.frames_loaded == 0
        && navigation.frames_failed == 1;
    if (!ok) {
        fprintf(stderr,
                "CSP navigation gate loaded=%d summary=\"%s\" "
                "styles=%zu images=%zu frames=%zu/%zu error=\"%s\"\n",
                loaded ? 1 : 0, navigation.page.script_result.summary,
                navigation.page.external_stylesheets.attempted,
                navigation.page.images.stats.attempted,
                navigation.frames_loaded, navigation.frames_failed,
                navigation.last_error);
    }
    if (replay_ready) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_frame_response_policy_precedes_child_execution(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    bool replay_ready = navigation_ready && frame_policy_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool loaded = replay_ready && navigation_load_url(
        &navigation, generation, "https://frame-policy.test/document",
        4096, 1000, 480, NULL, NULL, true);
    for (size_t tick = 0; loaded && tick < 8
         && navigation.frames_failed == 0; tick++) {
        loaded = navigation_advance_runtime(&navigation, 1, 8);
    }
    bool ok = loaded && navigation.page.frame_count == 1
        && navigation.frames_discovered == 1
        && navigation.frames_loaded == 0
        && navigation.frames_failed == 1
        && !navigation.page.frames[0].loaded
        && navigation.page.frames[0].runtime == NULL
        && navigation.page.frames[0].document.html == NULL;
    if (!ok) {
        fprintf(stderr,
                "frame policy loaded=%d count=%zu discovered=%zu "
                "loaded-frames=%zu failed=%zu error=\"%s\"\n",
                loaded ? 1 : 0, navigation.page.frame_count,
                navigation.frames_discovered, navigation.frames_loaded,
                navigation.frames_failed, navigation.last_error);
    }
    if (replay_ready) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_preload_settlement_releases_scheduler_slots(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_document_scripts(
            &navigation, 2, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    bool replay_ready = navigation_ready && preload_stall_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool loaded = replay_ready && navigation_load_url(
        &navigation, generation, "https://preload-stall.test/document",
        4096, 1000, 480, NULL, NULL, true);
    if (replay_ready) fetch_trace_end();

    uint64_t probe_ids[4] = {0};
    FetchRequest request = {.method = "GET"};
    bool all_slots_available = loaded
        && navigation.page.resource_scheduler != NULL;
    for (size_t i = 0; all_slots_available && i < 4; i++) {
        fetch_inject_failure_once(FETCH_INJECT_CANCELLED);
        probe_ids[i] = fetch_scheduler_enqueue(
            navigation.page.resource_scheduler,
            "https://preload-stall.test/slot-probe", &request,
            1024, 100);
        if (probe_ids[i] == 0) all_slots_available = false;
    }
    fetch_inject_failure_once(FETCH_INJECT_NONE);
    FetchResult probe_result = {.budget = &budget};
    bool probes_reclaimed = true;
    for (size_t i = 0; i < 4; i++) {
        if (probe_ids[i] == 0) continue;
        bool success = true;
        if (!fetch_scheduler_take(navigation.page.resource_scheduler,
                                  probe_ids[i], &success, &probe_result)
            || success) {
            probes_reclaimed = false;
        }
        fetch_result_destroy(&probe_result);
    }

    bool ok = loaded && all_slots_available && probes_reclaimed
        && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Preload ownership") == 0
        && navigation.preloads_discovered == 2
        && navigation.preloads_launched == 2
        && navigation.preloads_completed == 0
        && navigation.preloads_failed == 0
        && navigation.preloads_deferred == 2
        /* Both preloads are pinned in flight for the whole load, holding
           their reservations. The byte pool budgets a lane for exactly that,
           so the authoritative headroom gate must not fire here: if tightening
           the reserve ever starves ordinary speculation, this catches it. */
        && navigation.preloads_headroom_skipped == 0;
    if (!ok) {
        fprintf(stderr,
                "preload settlement failed: loaded=%d scheduler=%p "
                "slots=%d reclaimed=%d preload=%zu/%zu/%zu/%zu/%zu "
                "headroom-skips=%zu ids=%llu,%llu,%llu,%llu error=%s\n",
                loaded ? 1 : 0,
                (void *) navigation.page.resource_scheduler,
                all_slots_available ? 1 : 0, probes_reclaimed ? 1 : 0,
                navigation.preloads_discovered,
                navigation.preloads_launched,
                navigation.preloads_completed,
                navigation.preloads_failed,
                navigation.preloads_deferred,
                navigation.preloads_headroom_skipped,
                (unsigned long long) probe_ids[0],
                (unsigned long long) probe_ids[1],
                (unsigned long long) probe_ids[2],
                (unsigned long long) probe_ids[3],
                navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_raw_scanner_preloads_parser_blocking_script(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 512 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 2000);
        navigation_enable_document_scripts(
            &navigation, 4, 128 * 1024, 64 * 1024, 2000);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 2000);
    }
    bool replay_ready = navigation_ready && script_preload_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    bool loaded = replay_ready && navigation_load_url(
        &navigation, generation, "https://script-preload.test/document",
        4096, 2000, 480, NULL, NULL, true);
    if (replay_ready) fetch_trace_end();
    bool ok = loaded && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Script preload") == 0
        && strcmp(navigation.page.script_result.summary,
                  "SCRIPT-PRELOAD-OK") == 0
        && navigation.preloads_discovered == 1
        && navigation.preloads_launched == 1
        && navigation.preloads_completed == 1
        && navigation.preloads_failed == 0
        && navigation.preloads_deferred == 0
        && navigation.script_cache_hits == 1;
    if (!ok) {
        fprintf(stderr,
                "raw script preload failed: loaded=%d summary=%s "
                "preload=%zu/%zu/%zu/%zu/%zu "
                "scripts=%zu/%zu/%zu/%zu cache=%zu result=%s error=%s\n",
                loaded ? 1 : 0, navigation.page.script_result.summary,
                navigation.preloads_discovered,
                navigation.preloads_launched,
                navigation.preloads_completed,
                navigation.preloads_failed,
                navigation.preloads_deferred,
                navigation.script_discovered,
                navigation.script_attempted,
                navigation.script_loaded,
                navigation.script_failed,
                navigation.script_cache_hits,
                navigation.page.script_result.error,
                navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_compiled_stylesheet_reuse_stays_in_navigation_ram(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 256 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    bool loaded = navigation_ready;
    for (size_t pass = 0; loaded && pass < 3; pass++) {
        bool replay_ready = progressive_visual_readiness_replay_begin();
        uint64_t generation = replay_ready
            ? navigation_begin(&navigation) : 0;
        loaded = replay_ready && navigation_load_url(
            &navigation, generation,
            "https://visual-readiness.test/document",
            4096, 1000, 480, NULL, NULL, true);
        if (replay_ready) fetch_trace_end();
    }
    bool cache_hit = loaded && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Visual readiness") == 0
        /* The current and rollback page must remain intact, so the first
           replacement only retains its predecessor. The third identical
           navigation is the first one eligible for zero-copy reuse. */
        && navigation.performance.compiled_stylesheet_cache_hits == 1
        && navigation.performance.compiled_stylesheet_cache_stores >= 1
        && navigation.performance.compiled_stylesheet_cache_size_skips == 0
        && navigation.performance.compiled_stylesheet_cache_pressure_skips
             == 0
        && navigation.page.stylesheet.count != 0
        && navigation.compiled_stylesheet_cache_ready
        && navigation.compiled_stylesheet_cache_retained_bytes != 0;
    size_t cache_evictions_before =
        navigation.performance.compiled_stylesheet_cache_evictions;
    budget.limit = budget.current + 512 * 1024;
    uint64_t pressure_generation = cache_hit
        ? navigation_begin(&navigation) : 0;
    NavigationLoad *pressure_load = pressure_generation == 0 ? NULL
        : navigation_load_begin_url(
              &navigation, pressure_generation,
              "https://visual-readiness.test/document",
              4096, 1000, 480, NULL, NULL, true);
    bool pressure_reclaimed = !navigation.compiled_stylesheet_cache_ready
        && navigation.performance.compiled_stylesheet_cache_evictions
             == cache_evictions_before + 1
        && budget.pressure[BUDGET_PRESSURE_STYLESHEET].decisions != 0;
    budget.limit = 16 * MIB;
    navigation_load_destroy(pressure_load);
    bool ok = cache_hit && pressure_reclaimed;
    if (!ok) {
        fprintf(stderr,
                "compiled stylesheet RAM reuse failed: loaded=%d title=%s "
                "hits=%zu misses=%zu stores=%zu size-skips=%zu "
                "pressure-skips=%zu rules=%zu ready=%d retained=%zu "
                "pressure-reclaimed=%d error=%s\n",
                loaded ? 1 : 0, navigation.page.document.title,
                navigation.performance.compiled_stylesheet_cache_hits,
                navigation.performance.compiled_stylesheet_cache_misses,
                navigation.performance.compiled_stylesheet_cache_stores,
                navigation.performance.compiled_stylesheet_cache_size_skips,
                navigation.performance.compiled_stylesheet_cache_pressure_skips,
                navigation.page.stylesheet.count,
                navigation.compiled_stylesheet_cache_ready ? 1 : 0,
                navigation.compiled_stylesheet_cache_retained_bytes,
                pressure_reclaimed ? 1 : 0, navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_compiled_stylesheet_cache_rechecks_inline_nonce(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 256 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
    }
    bool loaded = navigation_ready;
    for (size_t pass = 0; loaded && pass < 3; pass++) {
        bool replay_ready = stylesheet_cache_nonce_replay_begin(pass < 2);
        uint64_t generation = replay_ready
            ? navigation_begin(&navigation) : 0;
        loaded = replay_ready && navigation_load_url(
            &navigation, generation,
            "https://stylesheet-auth.test/document",
            4096, 1000, 480, NULL, NULL, true);
        if (replay_ready) fetch_trace_end();
    }
    lxb_dom_node_t *probe = loaded ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "probe")
        : NULL;
    ComputedStyle style = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&navigation.page.stylesheet, probe, NULL);
    bool ok = loaded && navigation.page.loaded && probe != NULL
        && style.color != 0x123456
        && navigation.performance.compiled_stylesheet_cache_hits == 0;
    if (!ok) {
        fprintf(stderr,
                "stylesheet nonce cache authority failed: loaded=%d "
                "color=%06x hits=%zu misses=%zu error=%s\n",
                loaded ? 1 : 0, style.color,
                navigation.performance.compiled_stylesheet_cache_hits,
                navigation.performance.compiled_stylesheet_cache_misses,
                navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool compiled_stylesheet_metadata_change_misses_cache(
    unsigned changed_variant)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 256 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    bool loaded = navigation_ready;
    for (size_t pass = 0; loaded && pass < 3; pass++) {
        bool replay_ready = stylesheet_cache_metadata_replay_begin(
            pass < 2 ? 0 : changed_variant);
        uint64_t generation = replay_ready
            ? navigation_begin(&navigation) : 0;
        loaded = replay_ready && navigation_load_url(
            &navigation, generation,
            "https://stylesheet-metadata.test/document",
            4096, 1000, 480, NULL, NULL, true);
        if (replay_ready) fetch_trace_end();
    }
    bool ok = loaded && navigation.page.loaded
        && navigation.performance.compiled_stylesheet_cache_hits == 0;
    if (!ok) {
        fprintf(stderr,
                "stylesheet metadata cache authority failed: variant=%u "
                "loaded=%d hits=%zu misses=%zu error=%s\n",
                changed_variant, loaded ? 1 : 0,
                navigation.performance.compiled_stylesheet_cache_hits,
                navigation.performance.compiled_stylesheet_cache_misses,
                navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_compiled_stylesheet_cache_rechecks_link_authority(void)
{
    return compiled_stylesheet_metadata_change_misses_cache(1)
        && compiled_stylesheet_metadata_change_misses_cache(2);
}

static bool test_critical_ch_body_is_not_page_visible(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    BrowserSession browser;
    bool ready = browser_session_init(&browser, &budget, 64 * 1024)
        && navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation) && critical_ch_replay_begin();
    if (!ready) return false;
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 2 * MIB, 1000);
    navigation_enable_document_scripts(
        &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    navigation_set_stream_delivery(
        &navigation, 7, UINT64_C(0xabcddcba), 13, 2, 0, 0);
    size_t history_before = navigation.history_count;
    uint64_t generation = navigation_begin(&navigation);
    NavigationLoad *load = navigation_load_begin_url(
        &navigation, generation, "https://critical.test/document", 4096,
        1000, 480, NULL, NULL, true);
    NavigationLoadQuota quota = {
        .fetch = {1, 13, 100000},
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    bool ok = load != NULL && mutate_incumbent_dom(&navigation);
    for (size_t i = 0; ok && i < 256
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING
         && navigation.client_hint_retries == 0; i++) {
        (void) navigation_load_pump(load, &quota);
        ok = navigation.page.loaded
            && strcmp(navigation.page.document.title, "Prior") == 0
            && navigation.history_count == history_before
            && navigation.performance.first_dom_us == 0
            && navigation.preloads_discovered == 0
            && critical_side_effect_absent(&browser);
    }
    ok = ok && navigation.client_hint_retries == 1
        && navigation_load_status(load) == NAVIGATION_LOAD_PENDING
        && document_body_text(&navigation.page.document) != NULL
        && strstr(document_body_text(&navigation.page.document),
                  "incumbent mutation survives") != NULL
        && incumbent_is_renderable(&navigation)
        && navigation.performance.first_dom_us == 0
        && navigation.preloads_discovered == 0
        && navigation.preloads_deferred == 0
        && critical_side_effect_absent(&browser);
    if (!ok) {
        fprintf(stderr,
                "critical retry boundary failed: retries=%zu status=%d "
                "title=%s error=%s\n",
                navigation.client_hint_retries,
                (int) navigation_load_status(load),
                navigation.page.document.title == NULL
                    ? "(null)" : navigation.page.document.title,
                navigation.last_error);
    }
    for (size_t i = 0; ok && i < 256
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && strcmp(navigation.page.document.title, "Critical-CH retry") == 0
        && strcmp(navigation.page.script_result.summary, "retry-clean") == 0
        && navigation.preloads_discovered == 1
        && navigation.preloads_launched == 0
        && navigation.preloads_completed == 0
        && navigation.preloads_failed == 0
        && navigation.preloads_deferred == 1
        && navigation.history_count == history_before + 1
        && critical_side_effect_absent(&browser);
    if (!ok) {
        fprintf(stderr,
                "critical completion failed: retries=%zu status=%d "
                "title=%s summary=%s error=%s\n",
                navigation.client_hint_retries,
                (int) navigation_load_status(load),
                navigation.page.document.title == NULL
                    ? "(null)" : navigation.page.document.title,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    navigation_load_destroy(load);
    fetch_trace_end();
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    return ok && budget.current == 0;
}

static bool test_redirected_critical_ch_retries_final_origin(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    BrowserSession browser;
    bool ready = browser_session_init(&browser, &budget, 64 * 1024)
        && navigation_init(&navigation, &budget, 4)
        && prior_page_at(&navigation, "https://critical.test/page")
        && redirected_critical_ch_replay_begin();
    if (!ready) return false;
    navigation_attach_browser_session(&navigation, &browser);
    navigation_enable_scripts(&navigation, 2 * MIB, 1000);
    navigation_enable_document_scripts(
        &navigation, 4, 32 * 1024, 16 * 1024, 1000);
    snprintf(navigation.pending_navigation_referer,
             sizeof(navigation.pending_navigation_referer), "%s",
             "https://critical.test/page");
    navigation.pending_navigation_same_origin = true;
    size_t history_before = navigation.history_count;
    uint64_t generation = navigation_begin(&navigation);
    bool loaded = navigation_load_url(
        &navigation, generation, "https://critical.test/start", 4096,
        1000, 480, NULL, NULL, true);
    char challenge_cookies[4096] = {0};
    char outside_cookies[4096] = {0};
    bool ok = loaded
        && navigation.client_hint_retries == 1
        && navigation.history_count == history_before + 1
        && strcmp(navigation_current(&navigation)->url,
                  "https://hinted.test/challenge") == 0
        && strcmp(navigation.page.document.title,
                  "Redirected Critical-CH retry") == 0
        && strcmp(navigation.page.script_result.summary,
                  "redirect-retry-clean") == 0
        && browser_session_cookie_get(
            &browser, "https://hinted.test/challenge", challenge_cookies,
            sizeof(challenge_cookies))
        && strstr(challenge_cookies, "retry_path=seen") != NULL
        && strstr(challenge_cookies, "discarded_redirect_body=") == NULL
        && browser_session_cookie_get(
            &browser, "https://hinted.test/outside", outside_cookies,
            sizeof(outside_cookies))
        && strstr(outside_cookies, "retry_path=") == NULL;
    fetch_trace_end();
    navigation_destroy(&navigation);
    browser_session_destroy(&browser);
    return ok && budget.current == 0;
}

static bool test_safe_section_before_eof(void)
{
    Budget budget;
    budget_init(&budget, 4 * MIB);
    CompressedSectionStore store = {0};
    SectionStoreStreamBuilder builder = {0};
    SectionRouteStream router = {0};
    char *first = budget_malloc(&budget, 7000);
    if (first == NULL) return false;
    size_t used = (size_t) snprintf(
        first, 7000, "<!doctype html><html><head><title>Progress</title>"
        "</head><body><article id=first>");
    for (size_t i = 0; i < 80 && used < 6800; i++) {
        int written = snprintf(
            first + used, 7000 - used,
            "<p>sealed paragraph %zu with bounded progressive text</p>", i);
        if (written < 0 || (size_t) written >= 7000 - used) {
            budget_free(&budget, first);
            return false;
        }
        used += (size_t) written;
    }
    int closing = snprintf(first + used, 7000 - used, "</article>");
    if (closing < 0 || (size_t) closing >= 7000 - used) {
        budget_free(&budget, first);
        return false;
    }
    used += (size_t) closing;
    bool ok = section_route_stream_begin(
        &router, &builder, &store, &budget, 4096, 8192, true)
        && section_route_stream_body(
            &router, (const unsigned char *) first, used)
        && !store.stream_complete && store.sealed_section_count > 0
        && store.sealed_section_count <= store.section_count
        && store.sealed_source_bytes < used + 128;
    char *provisional = NULL;
    size_t provisional_length = 0;
    ok = ok && section_store_extract_html(
        &store, 0, &provisional, &provisional_length)
        && provisional_length != 0
        && strstr(provisional, "sealed paragraph 0") != NULL;
    budget_free(&budget, provisional);
    static const char tail[] =
        "<article id=second><h2>Second</h2><p>arrived later</p></article>"
        "</body></html>";
    ok = ok && section_route_stream_body(
        &router, (const unsigned char *) tail, sizeof(tail) - 1)
        && section_route_stream_finish(&router)
        && store.stream_complete
        && store.sealed_section_count == store.section_count
        && store.sealed_source_bytes == store.source_length;
    section_route_stream_abort(&router);
    section_store_destroy(&store);
    budget_free(&budget, first);
    return ok && budget.current == 0;
}

static bool test_mismatched_close_is_not_sealed(void)
{
    Budget budget;
    budget_init(&budget, 4 * MIB);
    CompressedSectionStore store = {0};
    SectionStoreStreamBuilder builder = {0};
    SectionRouteStream router = {0};
    char *first = budget_malloc(&budget, 7000);
    if (first == NULL) return false;
    size_t used = (size_t) snprintf(
        first, 7000, "<!doctype html><html><body><article id=first>");
    for (size_t i = 0; i < 90 && used < 6800; i++) {
        int written = snprintf(
            first + used, 7000 - used,
            "<p>mismatched closing tag must not seal paragraph %zu</p>", i);
        if (written < 0 || (size_t) written >= 7000 - used) {
            budget_free(&budget, first);
            return false;
        }
        used += (size_t) written;
    }
    int closing = snprintf(first + used, 7000 - used, "</bogus>");
    if (closing < 0 || (size_t) closing >= 7000 - used) {
        budget_free(&budget, first);
        return false;
    }
    used += (size_t) closing;
    bool ok = section_route_stream_begin(
        &router, &builder, &store, &budget, 4096, 8192, true)
        && section_route_stream_body(
            &router, (const unsigned char *) first, used)
        && store.sealed_section_count == 0;
    static const char tail[] = "</article></body></html>";
    ok = ok && section_route_stream_body(
        &router, (const unsigned char *) tail, sizeof(tail) - 1)
        && store.sealed_section_count == 1;
    section_route_stream_abort(&router);
    section_store_destroy(&store);
    budget_free(&budget, first);
    return ok && budget.current == 0;
}

typedef struct {
    size_t active_calls;
    size_t final_calls;
    size_t indices[4];
    size_t final_index;
} ScriptFrontierProbe;
