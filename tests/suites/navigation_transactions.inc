static bool replacement_prepare(void *opaque,
                                NavigationSession *candidate)
{
    (void) opaque;
    (void) candidate;
    return true;
}

static void replacement_abort(void *opaque)
{
    CandidateHookProbe *probe = opaque;
    probe->replacement_abort_calls++;
}

static void replacement_commit(void *opaque)
{
    (void) opaque;
}

static bool replacing_prepare(void *opaque,
                              NavigationSession *candidate)
{
    (void) candidate;
    CandidateHookProbe *probe = opaque;
    return navigation_set_candidate_commit_hooks(
               probe->navigation, replacement_prepare,
               replacement_abort, replacement_commit, probe)
        && false;
}

static void original_abort(void *opaque)
{
    CandidateHookProbe *probe = opaque;
    probe->original_abort_calls++;
}

static void original_commit(void *opaque)
{
    (void) opaque;
}

static bool test_candidate_hook_set_is_transactional(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    if (!budget_install_lexbor(&budget)) return false;
    NavigationSession navigation;
    CandidateHookProbe probe = {.navigation = &navigation};
    bool ok = navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation)
        && navigation_set_candidate_commit_hooks(
               &navigation, replacing_prepare, original_abort,
               original_commit, &probe);
    static const char candidate[] =
        "<!doctype html><title>Rejected hook candidate</title><p>new";
    uint64_t generation = ok ? navigation_begin(&navigation) : 0;
    ok = ok && !navigation_commit_html(
        &navigation, generation, "https://candidate.test/", candidate,
        sizeof(candidate) - 1, 480, NULL, NULL, true);
    ok = ok && probe.original_abort_calls == 1
        && probe.replacement_abort_calls == 0
        && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Prior") == 0;
    navigation_destroy(&navigation);
    ok = ok && budget.current == 0 && budget_uninstall_lexbor(&budget);
    return ok;
}

static bool test_script_oom_retains_usable_target(void)
{
    static const char html[] =
        "<!doctype html><title>OOM survivor</title>"
        "<style>#survivor{display:block;color:#123456}</style>"
        "<body><p id=survivor>Readable target survives.</p>"
        "<script>document.addEventListener('DOMContentLoaded',()=>{"
        "globalThis.retained=new ArrayBuffer(8*1024*1024)})</script>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://oom-target.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    lxb_dom_node_t *survivor = committed
        ? test_find_id(lxb_dom_interface_node(navigation.page.document.html),
                       "survivor") : NULL;
    ComputedStyle style = survivor == NULL ? (ComputedStyle) {0}
        : style_for_node(&navigation.page.stylesheet, survivor, NULL);
    bool ok = committed && navigation.page.loaded && survivor != NULL
        && style.display == DISPLAY_BLOCK && style.color == 0x123456
        && strcmp(navigation.page.document.title, "OOM survivor") == 0
        && strstr(navigation.page.script_result.error,
                  "out of memory") != NULL;
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_candidate_cancel_preserves_late_incumbent_dom(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    bool ready = navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation) && replay_begin();
    if (!ready) return false;
    navigation_set_stream_delivery(
        &navigation, 0, UINT64_C(0x31415926), 17, 0, 0, 0);
    size_t history_before = navigation.history_count;
    uint64_t generation = navigation_begin(&navigation);
    NavigationLoad *load = navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true);
    NavigationLoadQuota quota = {
        .fetch = {1, 17, 100000},
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    bool ok = load != NULL && mutate_incumbent_dom(&navigation);
    NavigationLoadMetrics metrics = {0};
    for (size_t i = 0; ok && i < 64; i++) {
        (void) navigation_load_pump(load, &quota);
        if (navigation_load_metrics(load, &metrics)
            && metrics.stream.bytes_received != 0) break;
    }
    ok = ok && metrics.stream.bytes_received != 0;
    navigation_load_cancel(load, "candidate owner isolation probe");
    const char *body_text = document_body_text(&navigation.page.document);
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Prior") == 0
        && body_text != NULL
        && strstr(body_text, "incumbent mutation survives") != NULL
        && navigation.history_count == history_before
        && incumbent_is_renderable(&navigation);
    navigation_load_destroy(load);
    fetch_trace_end();

    static const char failing_candidate[] =
        "<!doctype html><title>failing candidate</title><body>discard me";
    DocumentParser parser;
    PocDocument unexpectedly_finished = {0};
    bool parser_began = document_parser_begin(&parser, &budget)
        && document_parser_feed(&parser, failing_candidate,
                                sizeof(failing_candidate) - 1)
        && mutate_incumbent_dom(&navigation);
    budget_inject_failure_after(&budget, 0);
    bool parser_failed = parser_began
        && !document_parser_finish(&parser, &unexpectedly_finished);
    budget_clear_failure_injection(&budget);
    document_parser_abort(&parser);
    document_destroy(&unexpectedly_finished);
    body_text = document_body_text(&navigation.page.document);
    ok = ok && parser_failed && body_text != NULL
        && strstr(body_text, "incumbent mutation survives") != NULL
        && incumbent_is_renderable(&navigation);
    navigation_destroy(&navigation);
    return ok && budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget.active_allocation_owner == BUDGET_ALLOCATION_OWNER_NONE
        && budget_categories_reconcile(&budget);
}

typedef struct {
    size_t freeze_calls;
    size_t outcome_calls;
    size_t success_calls;
    size_t failure_calls;
    bool return_frozen;
    bool saw_failure_overlay;
} LowMemoryReplacementProbe;

static bool low_memory_freeze(
    void *opaque, const NavigationSession *incumbent)
{
    LowMemoryReplacementProbe *probe = opaque;
    probe->freeze_calls++;
    return incumbent != NULL && incumbent->page.loaded
        && probe->return_frozen;
}

static void low_memory_outcome(
    void *opaque, const NavigationSession *navigation, bool success)
{
    LowMemoryReplacementProbe *probe = opaque;
    probe->outcome_calls++;
    if (success) probe->success_calls++;
    else probe->failure_calls++;
    probe->saw_failure_overlay = !success && navigation != NULL
        && navigation->replacement_failure_overlay_active;
}

static bool low_memory_reject_candidate(
    void *opaque, NavigationSession *candidate)
{
    (void) opaque;
    return candidate != NULL && candidate->page.loaded && false;
}

static void low_memory_finalize_candidate(void *opaque)
{
    size_t *calls = opaque;
    if (calls != NULL) (*calls)++;
}

static bool pump_until_ready(
    NavigationLoad *load, const NavigationLoadQuota *quota)
{
    for (size_t i = 0; load != NULL && i < 256
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, quota);
    }
    return load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH;
}

static bool test_streaming_dynamic_stylesheets_are_not_adopted(void)
{
    static const unsigned char one_css[] = "#probe{color:#123456}";
    static const unsigned char two_css[] = "#probe{display:flex}";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && put_stylesheet_cache(
            &browser, "https://style-stream.test/document",
            "https://style-stream.test/one.css",
            one_css, sizeof(one_css) - 1)
        && put_stylesheet_cache(
            &browser, "https://style-stream.test/document",
            "https://style-stream.test/two.css",
            two_css, sizeof(two_css) - 1)
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    bool replay_ready = navigation_ready
        && dynamic_style_stream_replay_begin();
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = replay_ready ? navigation_load_begin_url(
        &navigation, generation, "https://style-stream.test/document",
        4096, 1000, 480, NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {1, 23, 100000},
        .maximum_parser_body_bytes = 11,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    bool ok = replay_ready && pump_until_ready(load, &quota)
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && strcmp(navigation.page.script_result.summary,
                  "stream-style:ready") == 0
        && navigation.page.external_stylesheets.loaded == 2
        && navigation.page.external_stylesheets.cache_hits == 2
        && navigation.page.stylesheet_event_count == 2
        && navigation.page.script_result.events_dispatched >= 4;
    if (!ok) {
        fprintf(stderr,
                "dynamic style stream replay=%d load=%p status=%d "
                "summary=\"%s\" css=%zu/%zu records=%zu dispatched=%zu "
                "error=\"%s\" script=\"%s\"\n",
                replay_ready, (void *) load,
                load == NULL ? -1 : (int) navigation_load_status(load),
                navigation.page.script_result.summary,
                navigation.page.external_stylesheets.loaded,
                navigation.page.external_stylesheets.cache_hits,
                navigation.page.stylesheet_event_count,
                navigation.page.script_result.events_dispatched,
                navigation.last_error,
                navigation.page.script_result.error);
    }
    navigation_load_destroy(load);
    if (replay_ready) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_dynamic_stylesheet_event_rescan_is_reentrant_safe(void)
{
    static const char html[] =
        "<!doctype html><title>Style event rescan</title><body>READY</body>";
    static const unsigned char one_css[] = "body{color:#123456}";
    static const unsigned char two_css[] = "body{display:flex}";
    static const char install[] =
        "globalThis.pocSummary='style-rescan:pending';"
        "const second=document.createElement('link');"
        "second.rel='stylesheet';second.href='/two.css';"
        "second.addEventListener('load',()=>pocSummary='style-rescan:stale');"
        "const first=document.createElement('link');"
        "first.rel='stylesheet';first.href='/one.css';"
        "first.addEventListener('load',()=>{pocSummary='style-rescan:ok';"
        "document.head.innerHTML=''}) ;document.head.append(first,second)";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && put_stylesheet_cache(
            &browser, "https://style-event.test/document",
            "https://style-event.test/one.css",
            one_css, sizeof(one_css) - 1)
        && put_stylesheet_cache(
            &browser, "https://style-event.test/document",
            "https://style-event.test/two.css",
            two_css, sizeof(two_css) - 1)
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = navigation_ready ? navigation_begin(&navigation) : 0;
    bool committed = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://style-event.test/document",
        html, sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptResult diagnostic = {0};
    size_t events_before = navigation.page.script_result.events_dispatched;
    bool ok = committed
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, install, "<style-event-rescan>",
            &diagnostic)
        && navigation_relayout(&navigation)
        && strcmp(navigation.page.script_result.summary,
                  "style-rescan:ok") == 0
        && navigation.page.external_stylesheets.loaded == 2
        && navigation.page.external_stylesheets.cache_hits == 2
        && navigation.page.script_result.events_dispatched
               == events_before + 1;
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_stylesheet_event_microtask_exception_is_nonfatal(void)
{
    static const char html[] =
        "<!doctype html><title>Style event exception</title><body>READY</body>";
    static const unsigned char css[] = "body{color:#123456}";
    static const char install[] =
        "globalThis.pocSummary='style-job:pending';"
        "const link=document.createElement('link');"
        "link.rel='stylesheet';link.href='/author.css';"
        "link.addEventListener('load',()=>{"
        "pocSummary='style-job:handled';"
        "queueMicrotask(()=>{throw new Error('author style microtask')})});"
        "document.head.append(link)";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && put_stylesheet_cache(
            &browser, "https://style-job.test/document",
            "https://style-job.test/author.css",
            css, sizeof(css) - 1)
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = navigation_ready ? navigation_begin(&navigation) : 0;
    bool committed = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://style-job.test/document",
        html, sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptResult diagnostic = {0};
    bool ok = committed
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, install, "<style-job-install>",
            &diagnostic)
        && navigation_relayout(&navigation)
        && navigation.page.loaded
        && strcmp(navigation.page.script_result.summary,
                  "style-job:handled") == 0
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.page.external_stylesheets.cache_hits == 1
        && strstr(navigation.page.script_result.error,
                  "author style microtask") != NULL;
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_dynamic_stylesheet_href_base_and_reinsert_lifecycle(void)
{
    static const char html[] =
        "<!doctype html><base href='https://assets.style-event.test/nested/'>"
        "<title>Style lifecycle</title><body>READY</body>";
    static const unsigned char one_css[] = "body{color:#123456}";
    static const unsigned char two_css[] = "body{display:flex}";
    static const char install[] =
        "globalThis.pocSummary='style-lifecycle:pending';"
        "globalThis.styleLifecycleLoads=0;"
        "globalThis.styleLifecycleLink=document.createElement('link');"
        "styleLifecycleLink.rel='stylesheet';styleLifecycleLink.href='one.css';"
        "styleLifecycleLink.addEventListener('load',()=>{"
        "styleLifecycleLoads++;"
        "if(styleLifecycleLoads===1){styleLifecycleLink.href='two.css';"
        "pocSummary='style-lifecycle:mutated'}"
        "else if(styleLifecycleLoads===2)pocSummary='style-lifecycle:second';"
        "else if(styleLifecycleLoads===3)pocSummary='style-lifecycle:ok'});"
        "document.head.append(styleLifecycleLink)";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && put_stylesheet_cache(
            &browser, "https://style-event.test/document",
            "https://assets.style-event.test/nested/one.css",
            one_css, sizeof(one_css) - 1)
        && put_stylesheet_cache(
            &browser, "https://style-event.test/document",
            "https://assets.style-event.test/nested/two.css",
            two_css, sizeof(two_css) - 1)
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            1, 16 * 1024, 16 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = navigation_ready ? navigation_begin(&navigation) : 0;
    bool committed = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://style-event.test/document",
        html, sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptResult diagnostic = {0};
    size_t events_before = navigation.page.script_result.events_dispatched;
    bool ok = committed
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, install, "<style-lifecycle>",
            &diagnostic)
        && navigation_relayout(&navigation)
        && strcmp(navigation.page.script_result.summary,
                  "style-lifecycle:mutated") == 0
        && navigation_relayout(&navigation)
        && strcmp(navigation.page.script_result.summary,
                  "style-lifecycle:second") == 0
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.page.external_stylesheets.cache_hits == 1
        && navigation.page.stylesheet_event_count == 1
        && navigation.page.script_result.events_dispatched
               == events_before + 2;
    static const char remove_link[] =
        "styleLifecycleLink.remove();pocSummary='style-lifecycle:removed'";
    static const char reinsert_link[] =
        "document.head.append(styleLifecycleLink)";
    ok = ok
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, remove_link, "<style-remove>",
            &diagnostic)
        && navigation_relayout(&navigation)
        && navigation.page.stylesheet_event_count == 0
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, reinsert_link, "<style-reinsert>",
            &diagnostic)
        && navigation_relayout(&navigation)
        && strcmp(navigation.page.script_result.summary,
                  "style-lifecycle:ok") == 0
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.page.external_stylesheets.cache_hits == 1
        && navigation.page.stylesheet_event_count == 1
        && navigation.page.script_result.events_dispatched
               == events_before + 3;
    if (!ok) {
        fprintf(stderr,
                "style lifecycle committed=%d summary=\"%s\" css=%zu/%zu "
                "records=%zu events=%zu(before=%zu) error=\"%s\" "
                "script=\"%s\"\n",
                committed, navigation.page.script_result.summary,
                navigation.page.external_stylesheets.loaded,
                navigation.page.external_stylesheets.cache_hits,
                navigation.page.stylesheet_event_count,
                navigation.page.script_result.events_dispatched,
                events_before, navigation.last_error,
                navigation.page.script_result.error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_low_memory_replacement_success(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    LowMemoryReplacementProbe probe = {.return_frozen = true};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation)
        && navigation_set_scroll(&navigation, 37)
        && navigation_set_replacement_mode(
               &navigation, NAVIGATION_REPLACEMENT_LOW_MEMORY)
        && navigation_set_replacement_hooks(
               &navigation, low_memory_freeze, low_memory_outcome, &probe)
        && replay_begin();
    /* A small external/dynamic script state is safe to retire without page
       overlap; this is the progressively-enhanced document boundary. */
    navigation.script_loaded = ready ? 1 : 0;
    navigation.script_bytes = ready ? 64u * 1024u : 0;
    navigation.page.script_result.dynamic_scripts_completed = ready ? 1 : 0;
    navigation.page.script_result.dynamic_script_bytes = ready ? 4096 : 0;
    NavigationLoadQuota quota = {
        .fetch = {1, 23, 100000},
        .maximum_parser_body_bytes = 7,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    lxb_html_document_t *incumbent = navigation.page.document.html;
    size_t history_before = navigation.history_count;
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    bool ok = ready && pump_until_ready(load, &quota);
    NavigationLoadMetrics metrics = {0};
    ok = ok && navigation_load_metrics(load, &metrics)
        && metrics.staged_body_bytes == metrics.stream.bytes_received
        && metrics.maximum_staged_body_bytes == metrics.stream.bytes_received
        && metrics.parser_pump_us == 0
        && navigation.page.document.html == incumbent
        && navigation.page.loaded
        && probe.freeze_calls == 0
        && navigation.performance.low_memory_replacements_started == 0;

    bool first = ok && navigation_load_finish(load, &quota);
    const NavigationEntry *current = navigation_current(&navigation);
    ok = ok && !first
        && navigation_load_status(load) == NAVIGATION_LOAD_FINALIZING
        && !navigation.page.loaded && navigation.page.document.html == NULL
        && current != NULL
        && strcmp(current->url, "https://prior.test/#old") == 0
        && current->scroll_y == 37
        && navigation.history_count == history_before
        && probe.freeze_calls == 1
        && navigation.performance.low_memory_replacements_started == 1
        && !navigation.performance.low_memory_last_rollback_available
        && navigation.performance.low_memory_last_frozen_frame
        && navigation.performance.low_memory_last_released_bytes != 0;
    bool second = ok && navigation_load_finish(load, &quota);
    ok = ok && !second
        && navigation_load_status(load) == NAVIGATION_LOAD_FINALIZING
        && !navigation.page.loaded;
    bool third = ok && navigation_load_finish(load, &quota);
    ok = ok && third
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Stream & split") == 0
        && navigation.history_count == history_before + 1
        && probe.outcome_calls == 1 && probe.success_calls == 1
        && probe.failure_calls == 0
        && navigation.performance.low_memory_replacements_succeeded == 1
        && navigation.performance.low_memory_replacements_failed == 0
        && !navigation.replacement_failure_overlay_active;
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_low_memory_external_script_incumbent_falls_back(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    LowMemoryReplacementProbe probe = {.return_frozen = true};
    bool navigation_ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 2 * MIB, 512 * 1024, 1000);
    }
    bool replay_ready = navigation_ready
        && prior_page(&navigation)
        && navigation_set_replacement_mode(
               &navigation, NAVIGATION_REPLACEMENT_LOW_MEMORY)
        && navigation_set_replacement_hooks(
               &navigation, low_memory_freeze, low_memory_outcome, &probe)
        && replay_begin();
    /* Model the externally evaluated incumbent state which is the guarded
       boundary. These counters are the same page-local telemetry populated by
       the normal document script pipeline. */
    navigation.script_loaded = replay_ready ? 3 : 0;
    navigation.script_bytes = replay_ready ? 128u * 1024u + 1u : 0;
    NavigationLoadQuota quota = {
        .fetch = {1, 23, 100000},
        .maximum_parser_body_bytes = 7,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    lxb_html_document_t *incumbent = navigation.page.document.html;
    uint64_t generation = replay_ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = replay_ready ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    bool ok = replay_ready && pump_until_ready(load, &quota);
    NavigationLoadMetrics metrics = {0};
    ok = ok && navigation_load_metrics(load, &metrics)
        && metrics.maximum_staged_body_bytes < metrics.stream.bytes_received
        && navigation.page.document.html == incumbent
        && navigation.performance.low_memory_transactional_fallbacks == 1
        && navigation.performance.low_memory_replacements_started == 0
        && navigation.performance.low_memory_last_rollback_available
        && probe.freeze_calls == 0 && probe.outcome_calls == 0;
    navigation_load_cancel(load, "external-script fallback regression");
    ok = ok
        && navigation.page.loaded
        && navigation.page.document.html == incumbent
        && navigation.performance.low_memory_replacements_started == 0
        && probe.freeze_calls == 0 && probe.outcome_calls == 0;
    navigation_load_destroy(load);
    if (replay_ready) fetch_trace_end();
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_low_memory_replacement_failure_boundaries(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    LowMemoryReplacementProbe probe = {.return_frozen = true};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation)
        && navigation_set_scroll(&navigation, 51)
        && navigation_set_replacement_mode(
               &navigation, NAVIGATION_REPLACEMENT_LOW_MEMORY)
        && navigation_set_replacement_hooks(
               &navigation, low_memory_freeze, low_memory_outcome, &probe)
        && replay_begin();
    NavigationLoadQuota quota = {
        .fetch = {1, 19, 100000},
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    lxb_html_document_t *incumbent = navigation.page.document.html;
    size_t history_before = navigation.history_count;
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ready ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    bool ok = ready && load != NULL;
    for (size_t i = 0; ok && i < 4; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    navigation_load_cancel(load, "pre-retirement cancellation");
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && navigation.page.loaded
        && navigation.page.document.html == incumbent
        && navigation.history_count == history_before
        && navigation_current(&navigation)->scroll_y == 51
        && probe.freeze_calls == 0 && probe.outcome_calls == 0
        && navigation.performance.low_memory_replacements_started == 0
        && navigation.performance.low_memory_last_rollback_available
        && !navigation.replacement_failure_overlay_active;
    navigation_load_destroy(load);
    if (ready) fetch_trace_end();

    size_t candidate_abort_calls = 0;
    bool replay_ready = ok && replay_begin();
    ok = ok && replay_ready
        && navigation_set_candidate_commit_hooks(
               &navigation, low_memory_reject_candidate,
               low_memory_finalize_candidate,
               low_memory_finalize_candidate, &candidate_abort_calls);
    generation = ok ? navigation_begin(&navigation) : 0;
    load = ok ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    ok = ok && pump_until_ready(load, &quota);
    while (ok && (navigation_load_status(load)
                      == NAVIGATION_LOAD_READY_TO_FINISH
                  || navigation_load_status(load)
                      == NAVIGATION_LOAD_FINALIZING)) {
        if (navigation_load_finish(load, &quota)) break;
    }
    const NavigationEntry *current = navigation_current(&navigation);
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_FAILED
        && !navigation.page.loaded && navigation.page.document.html == NULL
        && navigation.history_count == history_before
        && current != NULL
        && strcmp(current->url, "https://prior.test/#old") == 0
        && current->scroll_y == 51
        && candidate_abort_calls == 1
        && probe.freeze_calls == 1
        && probe.failure_calls == 1 && probe.success_calls == 0
        && probe.saw_failure_overlay
        && navigation.performance.low_memory_replacements_started == 1
        && navigation.performance.low_memory_replacements_failed == 1
        && !navigation.performance.low_memory_last_rollback_available
        && navigation.performance.low_memory_last_released_bytes != 0
        && navigation.replacement_failure_overlay_active
        && strstr(navigation.replacement_failure_message,
                  "candidate commit preparation") != NULL;
    navigation_load_destroy(load);
    if (replay_ready) fetch_trace_end();
    (void) navigation_set_candidate_commit_hooks(
        &navigation, NULL, NULL, NULL, NULL);
    if (lexbor_installed) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_pump_and_cancel(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    bool ready = navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation) && replay_begin();
    if (!ready) return false;
    navigation_set_stream_delivery(
        &navigation, 0, UINT64_C(0x5eed1234), 37, 2, 0, 0);
    size_t history_before = navigation.history_count;
    uint64_t generation = navigation_begin(&navigation);
    NavigationLoad *load = navigation_load_begin_url(
        &navigation, generation,
        "HTTPS://STREAM.TEST:443/document#retained", 4096, 1000, 480,
        NULL, NULL, true);
    NavigationLoadQuota quota = {
        .fetch = {1, 11, 100000},
        .maximum_parser_body_bytes = 5,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    bool ok = load != NULL;
    if (ok) {
        NavigationLoadStatus first = navigation_load_pump(load, &quota);
        ok = first == NAVIGATION_LOAD_PENDING
            && navigation.page.loaded
            && strcmp(navigation.page.document.title, "Prior") == 0
            && navigation.history_count == history_before;
    }
    for (size_t i = 0; ok && i < 128
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
        ok = navigation.page.loaded
            && strcmp(navigation.page.document.title, "Prior") == 0
            && navigation.history_count == history_before;
    }
    NavigationLoadMetrics metrics = {0};
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && navigation_load_metrics(load, &metrics)
        && metrics.pump_calls > 1 && metrics.quota_yields > 0
        && metrics.parser_quota_yields > 0
        && metrics.maximum_staged_body_bytes <= 11
        && metrics.staged_body_bytes == 383
        && navigation.performance.staged_body_slides_avoided > 0
        && navigation.performance.staged_body_slide_bytes_avoided > 0
        && !metrics.parser_time_quota_exceeded
        && metrics.stream.bytes_received == 383
        && metrics.stream.maximum_pump_bytes <= 11
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && strcmp(navigation.page.document.title, "Stream & split") == 0
        && navigation.history_count == history_before + 1
        && strstr(navigation_current(&navigation)->url, "#retained") != NULL;
    ok = ok && navigation_load_metrics(load, &metrics)
        && metrics.finalize_slices == 3
        && metrics.finalize_yields == 2
        && metrics.finalize_phase == 3
        && !metrics.finalize_quota_exceeded;
    navigation_load_destroy(load);
    fetch_trace_end();

    if (ok) ok = replay_begin();
    history_before = navigation.history_count;
    generation = navigation_begin(&navigation);
    load = ok ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    if (load != NULL) {
        (void) navigation_load_pump(load, &quota);
        navigation_load_cancel(load, "test cancellation");
        ok = ok
            && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
            && navigation.page.loaded
            && strcmp(navigation.page.document.title, "Stream & split") == 0
            && navigation.history_count == history_before;
    } else {
        ok = false;
    }
    navigation_load_destroy(load);
    fetch_trace_end();
    navigation_destroy(&navigation);
    return ok && budget.current == 0;
}

static bool cancel_during_finalize_phase(size_t finish_steps)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    NavigationSession navigation;
    bool ready = navigation_init(&navigation, &budget, 4)
        && prior_page(&navigation) && replay_begin();
    if (!ready) return false;
    ScriptRuntime *incumbent_runtime = navigation.page.runtime;
    lxb_html_document_t *incumbent_document = navigation.page.document.html;
    size_t history_before = navigation.history_count;
    uint64_t generation = navigation_begin(&navigation);
    NavigationLoad *load = navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true);
    NavigationLoadQuota quota = {
        .fetch = {1, 13, 100000},
        .maximum_parser_body_bytes = 7,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    for (size_t i = 0; load != NULL && i < 256
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    bool ok = load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH;
    for (size_t i = 0; ok && i < finish_steps; i++) {
        ok = !navigation_load_finish(load, &quota)
            && navigation_load_status(load) == NAVIGATION_LOAD_FINALIZING
            && navigation.page.runtime == incumbent_runtime
            && navigation.page.document.html == incumbent_document
            && navigation.history_count == history_before;
    }
    navigation_load_cancel(load, "finalization cancellation probe");
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && navigation.page.runtime == incumbent_runtime
        && navigation.page.document.html == incumbent_document
        && navigation.page.loaded
        && strcmp(navigation.page.document.title, "Prior") == 0
        && navigation.history_count == history_before
        && incumbent_is_renderable(&navigation);
    navigation_load_destroy(load);
    fetch_trace_end();
    navigation_destroy(&navigation);
    return ok && budget.current == 0;
}

static bool test_finalize_cancellation_is_transactional(void)
{
    return cancel_during_finalize_phase(1)
        && cancel_during_finalize_phase(2);
}
