static void script_frontier_probe(void *opaque, size_t script_index,
                                  bool active)
{
    ScriptFrontierProbe *probe = opaque;
    if (probe == NULL) return;
    if (!active) {
        probe->final_calls++;
        probe->final_index = script_index;
        return;
    }
    if (probe->active_calls < sizeof(probe->indices)
                              / sizeof(probe->indices[0])) {
        probe->indices[probe->active_calls] = script_index;
    }
    probe->active_calls++;
}

static bool test_navigation_keeps_server_dom_after_author_script_error(void)
{
    static const char html[] =
        "<!doctype html><title>Usable result</title><body>"
        "<main id=server-content><h1>Server-rendered page</h1>"
        "<a href='/next'>Continue reading</a></main>"
        "<script>throw new Error('missing optional dependency')</script>"
        "<script>globalThis.pocSummary='later-script-ran'</script>"
        "</body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://soft-script.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true);
    lxb_dom_node_t *content = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html),
        "server-content") : NULL;
    bool ok = committed && navigation.page.loaded && content != NULL
        && navigation.script_failed == 1u
        && navigation.page.script_result.scripts_evaluated == 1u
        && strcmp(navigation.page.script_result.summary,
                  "later-script-ran") == 0
        && strstr(navigation.page.script_result.error,
                  "missing optional dependency") != NULL;
    if (!ok) {
        fprintf(stderr,
                "soft script ready=%d committed=%d loaded=%d failed=%zu "
                "evaluated=%zu summary=\"%s\" error=\"%s\" nav=\"%s\"\n",
                ready, committed, navigation.page.loaded,
                navigation.script_failed,
                navigation.page.script_result.scripts_evaluated,
                navigation.page.script_result.summary,
                navigation.page.script_result.error,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_script_frontier_snapshot_survives_author_mutation(void)
{
    static const char html[] =
        "<!doctype html><title>Frontier snapshot</title><body>"
        "<div id=mover><script id=first>"
        "globalThis.frontierOrder=['first'];"
        "document.body.appendChild(document.getElementById('mover'));"
        "</script></div>"
        "<script id=second>frontierOrder.push('second')</script>"
        "<script id=third>frontierOrder.push('third');"
        "globalThis.pocSummary=frontierOrder.join(',')</script>"
        "</body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptFrontierProbe probe = {0};
    bool executed = committed
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document,
            script_frontier_probe, &probe);
    bool ok = executed
        && strcmp(navigation.page.script_result.summary,
                  "first,second,third") == 0
        && navigation.script_discovered == 3
        && probe.active_calls == 3 && probe.final_calls == 1
        && probe.indices[0] == 0 && probe.indices[1] == 1
        && probe.indices[2] == 2 && probe.final_index == 3;
    if (!ok) {
        fprintf(stderr,
                "frontier snapshot ready=%d committed=%d executed=%d "
                "active=%zu final=%zu/%zu discovered=%zu summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, executed, probe.active_calls,
                probe.final_calls, probe.final_index,
                navigation.script_discovered,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_script_frontier_skips_destroyed_snapshot_nodes(void)
{
    static const char html[] =
        "<!doctype html><title>Frontier destruction</title><body>"
        "<script>"
        "document.body.innerHTML='<main id=replacement>replacement</main>';"
        "globalThis.pocSummary='current-script-survived'"
        "</script>"
        "<script>globalThis.pocSummary='stale-script-ran'</script>"
        "</body>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier-destroy.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    ScriptFrontierProbe probe = {0};
    bool executed = committed
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document,
            script_frontier_probe, &probe);
    bool ok = executed
        && strcmp(navigation.page.script_result.summary,
                  "current-script-survived") == 0
        && navigation.script_discovered == 1
        && navigation.script_parser_blocking == 1
        && probe.active_calls == 1 && probe.final_calls == 1
        && probe.indices[0] == 0 && probe.final_index == 1;
    if (!ok) {
        fprintf(stderr,
                "frontier destruction ready=%d committed=%d executed=%d "
                "active=%zu final=%zu/%zu discovered=%zu summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, executed, probe.active_calls,
                probe.final_calls, probe.final_index,
                navigation.script_discovered,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_script_frontier_snapshot_capacity_is_explicit(void)
{
    static const char prefix[] =
        "<!doctype html><title>Frontier capacity</title><body>";
    static const char script[] = "<script></script>";
    static const char suffix[] = "</body>";
    enum { SCRIPT_COUNT = SCRIPT_DOM_HANDLE_SLOT_CAPACITY + 1 };
    size_t length = sizeof(prefix) - 1
        + SCRIPT_COUNT * (sizeof(script) - 1) + sizeof(suffix) - 1;
    char *html = malloc(length + 1);
    if (html == NULL) return false;
    size_t at = 0;
    memcpy(html + at, prefix, sizeof(prefix) - 1);
    at += sizeof(prefix) - 1;
    for (size_t i = 0; i < SCRIPT_COUNT; i++) {
        memcpy(html + at, script, sizeof(script) - 1);
        at += sizeof(script) - 1;
    }
    memcpy(html + at, suffix, sizeof(suffix));

    Budget budget;
    budget_init(&budget, 64 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frontier-cap.test/", html,
        length, 480, NULL, NULL, true);
    bool rejected = committed
        && !navigation_execute_document_scripts_streaming_from(
            &navigation, &navigation.page.document, NULL, NULL);
    bool ok = rejected
        && strstr(navigation.last_error, "script snapshot") != NULL
        && (strstr(navigation.last_error, "exceeded 2048 elements") != NULL
            || strstr(navigation.last_error,
                      "handle capacity exhausted") != NULL);
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    free(html);
    return ok && clean;
}

static bool test_preserved_runtime_retires_replaced_document(void)
{
    static const char old_html[] =
        "<!doctype html><title>Old realm</title><body>"
        "<div id=old><script>"
        "globalThis.rebindHeld=document.getElementById('old');"
        "globalThis.rebindCount=1"
        "</script></div></body>";
    static const char new_html[] =
        "<!doctype html><title>New realm</title><body>"
        "<div id=target>new</div></body>";
    static const char probe[] =
        "document.body.innerHTML='<main id=target><span>inner</span></main>';"
        "const target=document.getElementById('target');"
        "target.textContent='safe';"
        "globalThis.pocSummary=String(++globalThis.rebindCount)+':' +"
        "document.body.textContent+':' +"
        "String(globalThis.rebindHeld.textContent)+':' +"
        "String(globalThis.rebindHeld.isConnected)";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) navigation_enable_scripts(&navigation, 4 * MIB, 1000);
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://rebind.test/old", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    lxb_dom_node_t *old_node = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "old") : NULL;
    long old_handle = old_node == NULL ? 0
        : script_runtime_node_handle(runtime, old_node);
    generation = committed ? navigation_begin(&navigation) : 0;
    bool replaced = committed && old_handle != 0
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://rebind.test/new", new_html,
            sizeof(new_html) - 1, 480, NULL, NULL, false);
    ScriptResult diagnostic = {0};
    bool evaluated = replaced
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, probe, "<rebind-retirement-probe>",
            &diagnostic);
    bool ok = evaluated
        && navigation.page.runtime == runtime
        && script_runtime_node_handle_resolve(runtime, old_handle) == NULL
        && strcmp(diagnostic.summary, "2:safe::false") == 0;
    if (!ok) {
        bool runtime_retained = navigation.page.runtime == runtime;
        bool old_live = runtime_retained
            && script_runtime_node_handle_resolve(runtime, old_handle) != NULL;
        fprintf(stderr,
                "rebind retirement ready=%d committed=%d replaced=%d "
                "evaluated=%d same-runtime=%d old-live=%d summary=\"%s\" "
                "error=\"%s\"\n",
                ready, committed, replaced, evaluated,
                runtime_retained, old_live,
                diagnostic.summary, diagnostic.error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_transient_document_survives_active_rebind_until_detach(void)
{
    static const char old_html[] =
        "<!doctype html><title>Active old</title><body>"
        "<div id=old-live>old</div></body>";
    static const char new_html[] =
        "<!doctype html><title>Active new</title><body>"
        "<div id=live>live</div></body>";
    static const char corpus_html[] =
        "<!doctype html><body><script id=corpus-script>"
        "globalThis.corpusRan=(globalThis.corpusRan||0)+1"
        "</script></body>";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://owner.test/old", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    lxb_dom_node_t *old_node = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "old-live")
        : NULL;
    long old_handle = old_node == NULL ? 0
        : script_runtime_node_handle(runtime, old_node);

    PocDocument corpus = {0};
    bool corpus_parsed = committed && document_parse(
        &corpus, &budget, corpus_html, sizeof(corpus_html) - 1, 64);
    lxb_dom_node_t *corpus_node = corpus_parsed ? test_find_id(
        lxb_dom_interface_node(corpus.html), "corpus-script") : NULL;
    long corpus_handle = corpus_node == NULL ? 0
        : script_runtime_node_handle(runtime, corpus_node);
    bool corpus_registered = corpus_handle != 0
        && script_runtime_register_script_source_node(
            runtime, corpus_node, 7, 0);
    bool corpus_executed = corpus_registered
        && navigation_execute_document_scripts_streaming_from(
            &navigation, &corpus, NULL, NULL);

    generation = corpus_executed ? navigation_begin(&navigation) : 0;
    bool replaced = corpus_executed
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://owner.test/new", new_html,
            sizeof(new_html) - 1, 480, NULL, NULL, false);
    bool corpus_survived = replaced
        && navigation.page.runtime == runtime
        && script_runtime_node_handle_resolve(runtime, old_handle) == NULL
        && script_runtime_node_handle_resolve(runtime, corpus_handle)
             == corpus_node;

    ScriptRuntime *detach_runtime = navigation.page.runtime;
    if (corpus_parsed && detach_runtime != NULL) {
        script_runtime_detach_document(detach_runtime, &corpus);
        script_runtime_clear_script_source_nodes(detach_runtime);
    }
    bool corpus_retired = corpus_handle != 0 && detach_runtime != NULL
        && script_runtime_node_handle_resolve(
               detach_runtime, corpus_handle) == NULL;
    document_destroy(&corpus);

    static const char probe[] =
        "const live=document.getElementById('live');"
        "live.innerHTML='<b>inner</b>';live.textContent='safe';"
        "globalThis.pocSummary=String(globalThis.corpusRan)+':' +"
        "live.textContent";
    ScriptResult diagnostic = {0};
    bool evaluated = corpus_survived && corpus_retired
        && script_runtime_evaluate_diagnostic(
            detach_runtime, probe, "<transient-retirement-probe>",
            &diagnostic);
    bool ok = evaluated && strcmp(diagnostic.summary, "1:safe") == 0;
    if (!ok) {
        fprintf(stderr,
                "transient retirement ready=%d committed=%d parsed=%d "
                "registered=%d executed=%d replaced=%d survived=%d "
                "retired=%d evaluated=%d summary=\"%s\" error=\"%s\"\n",
                ready, committed, corpus_parsed, corpus_registered,
                corpus_executed, replaced, corpus_survived, corpus_retired,
                evaluated, diagnostic.summary, diagnostic.error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool test_preserved_runtime_dynamic_import_owns_scheduler(void)
{
    static const char old_html[] =
        "<!doctype html><title>Module old</title><body>"
        "<script type=module>"
        "globalThis.loadAfterSwap=()=>import('./late.js');"
        "globalThis.pocSummary='module-swap:root'"
        "</script></body>";
    static const char new_html[] =
        "<!doctype html><title>Module new</title><body>new</body>";
    static const char start_import[] =
        "globalThis.pocSummary='module-swap:pending';"
        "loadAfterSwap().then(module=>{"
        "globalThis.pocSummary='module-swap:'+module.value"
        "}).catch(error=>{globalThis.pocSummary='module-swap:error:'+error})";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 128 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://module-swap.test/page", old_html,
        sizeof(old_html) - 1, 480, NULL, NULL, true);
    ScriptRuntime *runtime = committed ? navigation.page.runtime : NULL;
    FetchScheduler *page_scheduler = committed
        ? navigation.page.resource_scheduler : NULL;
    FetchSchedulerDomain *domain = committed
        ? navigation.page.fetch_domain : NULL;
    FetchSchedulerDomainMetrics before_swap = {0};
    bool scheduler_still_lazy = page_scheduler != NULL
        && fetch_scheduler_domain_metrics(domain, &before_swap)
        && before_swap.active_views == 1;

    generation = scheduler_still_lazy ? navigation_begin(&navigation) : 0;
    bool replaced = scheduler_still_lazy
        && navigation_commit_html_preserve_runtime(
            &navigation, generation, "https://module-swap.test/page#next",
            new_html, sizeof(new_html) - 1, 480, NULL, NULL, false);
    FetchSchedulerDomainMetrics after_swap = {0};
    bool realm_retained = replaced && navigation.page.runtime == runtime
        && navigation.page.fetch_domain == domain
        && fetch_scheduler_domain_metrics(domain, &after_swap)
        && after_swap.active_views == 0;

    bool replay_ready = realm_retained && module_swap_replay_begin();
    ScriptResult diagnostic = {0};
    bool imported = replay_ready
        && script_runtime_evaluate_diagnostic(
            runtime, start_import, "<module-swap-import>", &diagnostic);
    FetchSchedulerDomainMetrics after_import = {0};
    bool realm_scheduler_created = imported
        && fetch_scheduler_domain_metrics(domain, &after_import)
        && after_import.active_views == 1;
    for (size_t i = 0; imported && i < 8
         && strcmp(diagnostic.summary, "module-swap:late-ok") != 0; i++) {
        imported = navigation_advance_runtime(&navigation, 0, 32);
        diagnostic = navigation.page.script_result;
    }
    if (replay_ready) fetch_trace_end();
    bool ok = imported && realm_scheduler_created
        && strcmp(diagnostic.summary, "module-swap:late-ok") == 0;
    if (!ok) {
        fprintf(stderr,
                "module swap ready=%d committed=%d lazy=%d views=%zu "
                "replaced=%d retained=%d views-after=%zu replay=%d "
                "imported=%d realm-views=%zu summary=\"%s\" error=\"%s\" "
                "nav=\"%s\"\n",
                ready, committed, scheduler_still_lazy,
                before_swap.active_views, replaced, realm_retained,
                after_swap.active_views, replay_ready, imported,
                after_import.active_views, diagnostic.summary, diagnostic.error,
                navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static bool install_module_loader_then_fail(NavigationSession *navigation)
{
    ExternalScriptMetrics metrics = {0};
    ScriptResult result = {0};
    bool executed = document_scripts_execute(
        &navigation->page.document, navigation->page.runtime,
        navigation->budget, navigation->browser_session,
        navigation->page.resource_base_url,
        "https://module-swap.test/failure",
        navigation->page.referrer_policy, 8, 64 * 1024, 32 * 1024, 1000,
        navigation->page.resource_scheduler, &metrics, &result);
    /* The replay-missing external script fails after the retained module
       loader has been installed. Returning from this helper poisons the
       caller-owned metrics frame under ASan; a later import must use the
       context's copy. */
    /* The root module was evaluated explicitly above and the loader may now
       omit already-evaluated candidates from its executable accounting. The
       regression boundary is the failed pass after installing the retained
       module context; the later import verifies that context's lifetime. */
    return !executed;
}

static bool test_delayed_import_after_module_pipeline_failure(void)
{
    static const char html[] =
        "<!doctype html><body>"
        "<script id=root type=module>"
        "globalThis.loadAfterFailure=()=>import('./late.js')"
        "</script><script src='/missing.js'></script></body>";

    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 128 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
        navigation.defer_document_script_pipeline = true;
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://module-swap.test/failure", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    lxb_dom_node_t *root = committed ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "root") : NULL;
    ScriptResult module_result = {0};
    bool module_ready = root != NULL
        && script_runtime_evaluate_inline(
            navigation.page.runtime, root, true, &module_result);
    bool replay_ready = module_ready && module_swap_replay_begin();
    bool failed_after_install = replay_ready
        && install_module_loader_then_fail(&navigation);
    ScriptResult diagnostic = {0};
    bool imported = replay_ready
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "globalThis.pocSummary='module-failure:pending';"
            "loadAfterFailure().then(module=>{"
            "globalThis.pocSummary='module-failure:'+module.value"
            "}).catch(error=>{globalThis.pocSummary='module-failure:error:'"
            "+error})",
            "<module-failure-import>", &diagnostic);
    for (size_t i = 0; imported && i < 8
         && strcmp(diagnostic.summary, "module-failure:late-ok") != 0; i++) {
        imported = navigation_advance_runtime(&navigation, 0, 32);
        diagnostic = navigation.page.script_result;
    }
    if (replay_ready) fetch_trace_end();
    bool ok = imported
        && strcmp(diagnostic.summary, "module-failure:late-ok") == 0;
    if (!ok) {
        fprintf(stderr,
                "module failure ready=%d committed=%d module=%d failed=%d "
                "replay=%d imported=%d summary=\"%s\" error=\"%s\"\n",
                ready, committed, module_ready, failed_after_install,
                replay_ready, imported, diagnostic.summary, diagnostic.error);
    }
    if (ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}
