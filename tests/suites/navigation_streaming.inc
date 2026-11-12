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
        && navigation.page.committed_us != 0u
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
    uint64_t incumbent_committed_us = navigation.page.committed_us;
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
        && navigation.page.committed_us == incumbent_committed_us
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

static bool test_parser_head_cssom_preserves_initialization(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2);
    char error[256] = {0};
    bool replay = ready && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-parser-head-cssom",
        error, sizeof(error));
    if (replay) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 37, 0, 0, 0, 0, 0);
    }
    uint64_t generation = replay ? navigation_begin(&navigation) : 0;
    bool loaded = replay && navigation_load_url(
        &navigation, generation, "https://head-cssom.test/document",
        4096, 1000, 480, NULL, NULL, true);
    lxb_dom_node_t *field = loaded ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "field") : NULL;
    size_t marker_length = 0;
    const char *marker = field == NULL ? NULL : document_attribute(
        field, "data-ready", &marker_length);
    bool ok = loaded && navigation.page.runtime != NULL
        && navigation.performance.parser_checkpoint_cssom_refusals == 0
        && navigation.performance.optional_work_sheds == 0
        && marker != NULL && marker_length == 3
        && memcmp(marker, "yes", 3) == 0
        && strcmp(navigation.page.script_result.summary,
                  "true|rgb(18, 52, 86)|true|37|73px") == 0;
    if (!ok) fprintf(stderr,
        "head CSSOM loaded=%d runtime=%p refusals=%zu sheds=%zu "
        "summary=\"%s\" error=\"%s\"\n", loaded,
        (void *) navigation.page.runtime,
        navigation.performance.parser_checkpoint_cssom_refusals,
        navigation.performance.optional_work_sheds,
        navigation.page.script_result.summary, navigation.last_error);
    if (replay) fetch_trace_end();
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

/* The streamed server markup declares its video in the head; a body
   script (as client hydration does) empties the head before any lookup.
   The declaration kept when the parser closed the head still answers,
   through a same-URL replaceState, until a fragment navigation changes
   the URL (pushState is covered by tilefinch-media-dom-tests). */
static bool test_streamed_declarations_survive_head_wipe(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2);
    char error[256] = {0};
    bool replay = ready && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-declarations-head-wipe",
        error, sizeof(error));
    if (replay) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = replay ? navigation_begin(&navigation) : 0;
    bool loaded = replay && navigation_load_url(
        &navigation, generation, "https://decl.test/news/video",
        4096, 1000, 480, NULL, NULL, true);
    PocDocument *document = &navigation.page.document;
    const MediaDeclaredVideo *declared =
        loaded ? media_declared_video_cached(document) : NULL;
    bool kept = loaded && navigation.page.runtime != NULL
        && strcmp(navigation.page.script_result.summary, "wiped:1:0") == 0
        && declared != NULL
        && strcmp(declared->media_url,
                  "https://cdn.decl.test/own/abs/index.m3u8") == 0
        && strcmp(declared->embed_url,
                  "https://players.decl.test/embed/4410023351") == 0
        && strcmp(declared->title, "Own story") == 0;
    ScriptResult result = {0};
    bool same_url = kept && script_runtime_evaluate_diagnostic(
        navigation.page.runtime,
        "history.replaceState({n:1},'',location.href);"
        "document.body.appendChild(document.createElement('p'));"
        "globalThis.pocSummary='same'", "<same-url>", &result)
        && media_declared_video_cached(document) != NULL;
    bool moved = same_url
        && navigation_commit_same_document_url(
               &navigation, "https://decl.test/news/video#comments")
        && media_declared_video_cached(document) == NULL
        && media_declared_video_or_embed_cached(document) == NULL;
    bool ok = kept && same_url && moved;
    if (!ok) fprintf(stderr,
        "streamed declarations loaded=%d kept=%d same=%d moved=%d "
        "summary=\"%s\" error=\"%s\"\n", loaded, kept, same_url, moved,
        navigation.page.script_result.summary, navigation.last_error);
    if (replay) fetch_trace_end();
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

/* The head declares nothing; the VideoObject arrives in a later body chunk
   and a later body script removes it, as hydration does. The declaration
   is kept when its data script closes, before that script can run. */
static bool test_streamed_body_declaration_survives_removal(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = installed && navigation_init(&navigation, &budget, 2);
    char error[256] = {0};
    bool replay = ready && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-declarations-body-removed",
        error, sizeof(error));
    if (replay) {
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 64, 0, 0, 0, 0, 0);
    }
    uint64_t generation = replay ? navigation_begin(&navigation) : 0;
    bool loaded = replay && navigation_load_url(
        &navigation, generation, "https://decl.test/news/body",
        4096, 1000, 480, NULL, NULL, true);
    const MediaDeclaredVideo *declared =
        loaded ? media_declared_video_cached(&navigation.page.document)
               : NULL;
    bool ok = loaded && navigation.page.runtime != NULL
        && strcmp(navigation.page.script_result.summary, "removed:0") == 0
        && declared != NULL
        && strcmp(declared->media_url,
                  "https://cdn.decl.test/own/body/index.m3u8") == 0;
    if (!ok) fprintf(stderr,
        "streamed body declaration loaded=%d declared=%d summary=\"%s\" "
        "error=\"%s\"\n", loaded, declared != NULL,
        navigation.page.script_result.summary, navigation.last_error);
    if (replay) fetch_trace_end();
    if (ready) navigation_destroy(&navigation);
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
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
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
        /* This fixture qualifies CSP/resource ordering, not the minimum-heap
           pressure boundary. Leave room for the boot-captured Worker security
           intrinsics; device defaults retain substantially more headroom. */
        navigation_enable_scripts(
            &navigation, 2 * MIB + 64 * 1024u, 1000);
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

/* A child whose response refuses embedding (X-Frame-Options: DENY) is never
   parsed or run: the response policy is checked before the child realm
   exists. Each child script writes a cookie, which outlives a frame that
   fails afterwards, so a check made after execution leaves "denied=ran"
   behind; the allowed sibling shows the cookie path works. */
static bool test_frame_response_policy_precedes_child_execution(void)
{
    static const char html[] =
        "<!doctype html><title>Frame parent</title>"
        "<iframe src='/allowed'></iframe><iframe src='/denied'></iframe>";
    static const char allowed_child[] =
        "<!doctype html><script>document.cookie='allowed=ran'</script>";
    static const char denied_child[] =
        "<!doctype html><script>document.cookie='denied=ran'</script>";
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
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = navigation_ready ? navigation_begin(&navigation) : 0;
    bool committed = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://frame-policy.test/document", html,
        sizeof(html) - 1u, 480, NULL, NULL, true)
        && navigation.page.frame_count == 2u;
    /* The unit fetch backend cannot answer frames: the commit's own frame
       fetches fail, and only the completions below are counted. */
    size_t failed_before = navigation.frames_failed;
    bool completed = committed
        && navigation_test_complete_frame_with_headers(
               &navigation, &navigation.page.frames[0], 200, allowed_child,
               sizeof(allowed_child) - 1u, NULL);
    /* The refused child is a contained frame failure, not a page failure:
       the completion itself still answers. */
    completed = completed
        && navigation_test_complete_frame_with_headers(
               &navigation, &navigation.page.frames[1], 200, denied_child,
               sizeof(denied_child) - 1u, "x-frame-options: DENY\n");
    char cookies[256] = {0};
    bool read = completed && browser_session_cookie_get(
        &browser, "https://frame-policy.test/", cookies, sizeof(cookies));
    bool ok = read
        && navigation.frames_loaded == 1u
        && navigation.frames_failed == failed_before + 1u
        && navigation.page.frames[0].loaded
        && !navigation.page.frames[1].loaded
        && navigation.page.frames[1].runtime == NULL
        && navigation.page.frames[1].document.html == NULL
        && strstr(cookies, "allowed=ran") != NULL
        && strstr(cookies, "denied=ran") == NULL;
    if (!ok) {
        fprintf(stderr,
                "frame policy committed=%d completed=%d count=%zu "
                "loaded-frames=%zu failed=%zu/%zu cookies=\"%s\" "
                "error=\"%s\"\n",
                committed, completed, navigation.page.frame_count,
                navigation.frames_loaded, failed_before,
                navigation.frames_failed, cookies,
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

/* Committing a fetched child gives its contentWindow.eval the child's response
   policy. The parent here has none, so the strict child is the case the commit
   must change. Expected results recorded from Chromium 152; the strict parent
   is in test_native_dynamic_code_policy. */
static bool test_frame_eval_obeys_child_response_policy(void)
{
    static const char html[] =
        "<!doctype html>"
        "<iframe id=lax src='https://frame-eval.test/lax'></iframe>"
        "<iframe id=strict src='https://frame-eval.test/strict'></iframe>"
        "<iframe id=none src='https://frame-eval.test/none'></iframe>";
    static const char child[] = "<!doctype html>";
    static const char *const headers[] = {
        "content-security-policy: script-src 'unsafe-eval'\n",
        "content-security-policy: script-src 'self'\n",
        NULL
    };
    Budget budget;
    budget_init(&budget, 32 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool ready = lexbor_installed && navigation_init(&navigation, &budget, 4);
    if (ready) {
        navigation_enable_scripts(&navigation, 3 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024, 32 * 1024, 1000);
    }
    uint64_t generation = ready ? navigation_begin(&navigation) : 0;
    bool committed = ready && navigation_commit_html(
        &navigation, generation, "https://frame-eval.test/", html,
        sizeof(html) - 1u, 480, NULL, NULL, true)
        && navigation.page.frame_count == 3u;
    for (size_t i = 0; committed && i < 3u; i++) {
        committed = navigation_test_complete_frame_with_headers(
            &navigation, &navigation.page.frames[i], 200, child,
            sizeof(child) - 1u, headers[i]);
    }
    ScriptResult result = {0};
    static const char probe[] =
        "(()=>{const attempt=fn=>{try{return fn()}catch(_){return 'blocked'}},"
        "row=id=>{const w=document.getElementById(id).contentWindow;"
        "return [attempt(()=>w.eval('40+2')),"
        "attempt(()=>new w.Function('return 7')()),"
        "attempt(()=>w.eval('(0,eval)(\"5\")'))].join()};"
        "globalThis.pocSummary=[attempt(()=>eval('1')),row('lax'),"
        "row('strict'),row('none')].join(' ')})()";
    bool probed = committed && navigation.frames_loaded == 3u
        && script_runtime_evaluate_diagnostic(
            navigation.page.runtime, probe, "<frame-eval-policy>", &result);
    bool ok = probed && strcmp(
        result.summary,
        "1 42,7,5 blocked,blocked,blocked 42,7,5") == 0;
    if (!ok) {
        fprintf(stderr,
                "frame eval policy committed=%d frames=%zu loaded=%zu "
                "failed=%zu probed=%d summary=\"%s\" error=\"%s\" nav=\"%s\"\n",
                committed, navigation.page.frame_count,
                navigation.frames_loaded, navigation.frames_failed, probed,
                result.summary, result.error, navigation.last_error);
    }
    if (ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

/* A child snapshot can stay published after the child frees the nodes in it:
   only one child is rebuilt per owner tick, and a refused rebuild keeps the old
   one. A press in between must reach what the child shows now and never a freed
   control. The unit fetch backend has no response path for frames, so a real
   child realm goes in the discovered slot. */
static bool test_stale_frame_snapshot_press(bool pointer, bool refuse_first)
{
    static const char page[] =
        "<!doctype html><body><iframe id=owned src=/child.html></iframe>";
    /* Both presses pick #gone over #keep while the snapshot lists it: it is the
       last control and is painted on top. */
    static const char child[] =
        "<!doctype html><body>"
        "<button id=keep style='position:absolute;left:0;top:0;width:80px;"
        "height:30px'>keep</button>"
        "<div id=wrap><button id=gone style='position:absolute;left:0;top:0;"
        "width:80px;height:30px'>gone</button></div>";
    Budget budget;
    budget_init(&budget, 32 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 64 * 1024);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 2);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 4 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 64 * 1024, 64 * 1024, 1000);
    }
    uint64_t generation = navigation_ready ? navigation_begin(&navigation) : 0;
    bool ok = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://frame-owner.test/", page,
        strlen(page), 480, NULL, NULL, true);
    for (size_t tick = 0; ok && tick < 8
         && navigation.frames_failed == 0; tick++) {
        ok = navigation_advance_runtime(&navigation, 1, 8);
    }
    ok = ok && navigation.page.frame_count == 1;
    NavigationFrame *frame = ok ? &navigation.page.frames[0] : NULL;
    ok = ok && script_runtime_set_frame_window_state(
             navigation.page.runtime, frame->parent_handle, true, true, false,
             "https://frame-owner.test/child.html", true,
             &navigation.page.script_result)
         && document_parse(&frame->document, &budget, child,
                           sizeof(child) - 1, 16);
    if (ok) {
        frame->runtime = script_runtime_create_deferred(
            &frame->document, &budget, 4 * MIB, 1000,
            "https://frame-owner.test/child.html", NULL, &frame->script_result);
        ok = frame->runtime != NULL;
    }
    if (ok) {
        frame->loaded = true;
        navigation.images = &navigation.page.images;
        ok = script_runtime_evaluate_diagnostic(
                 frame->runtime,
                 "document.getElementById('keep').addEventListener('click',"
                 "()=>globalThis.pocSummary='kept')", "<stale-frame>",
                 &frame->script_result)
             && navigation_test_refresh_frame_presentation(&navigation, frame)
             && frame->presentation != NULL;
    }
    /* The snapshot still names #gone; this frees it. */
    ok = ok && script_runtime_evaluate_diagnostic(
             frame->runtime, "document.getElementById('wrap').textContent=''",
             "<stale-frame>", &frame->script_result);
    bool refused_press = false, refused_dirty = true;
    if (ok && refuse_first) {
        navigation_test_refuse_next_frame_presentation();
        refused_press = pointer
            ? navigation_activate_frame_at(&navigation, frame->element, 5, 5)
            : navigation_activate_frame(&navigation, frame->element);
        refused_dirty = frame->presentation_dirty;
    }
    bool pressed = ok && (pointer
        ? navigation_activate_frame_at(&navigation, frame->element, 5, 5)
        : navigation_activate_frame(&navigation, frame->element));
    bool kept = ok && strcmp(frame->script_result.summary, "kept") == 0;
    if (!ok || refused_press || !refused_dirty || !pressed || !kept) {
        fprintf(stderr,
                "stale frame ok=%d refused_press=%d refused_dirty=%d "
                "pressed=%d kept=%d error=\"%s\"\n", ok, refused_press,
                refused_dirty, pressed, kept, navigation.last_error);
    }
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0;
    if (lexbor_installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && !refused_press && refused_dirty && pressed && kept && clean;
}

static bool test_stale_frame_snapshot_activation(void)
{
    return test_stale_frame_snapshot_press(false, false);
}

static bool test_stale_frame_snapshot_pointer(void)
{
    return test_stale_frame_snapshot_press(true, false);
}

static bool test_stale_frame_snapshot_refused_rebuild(void)
{
    return test_stale_frame_snapshot_press(false, true)
        && test_stale_frame_snapshot_press(true, true);
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

/* A same-URL response can gain a body stylesheet after the cached prefix
   was compiled. Context-changing user CSS must still refuse continuation;
   ordinary user CSS must retain the fast path. */
static bool test_cached_stylesheet_preserves_user_context_guard(void)
{
    static const char prefix[] =
        "<!doctype html><head><style>body{margin:0}</style></head>"
        "<body><p id=probe>Cache probe</p></body>";
    static const char expanded[] =
        "<!doctype html><head><style>body{margin:0}</style>"
        "<script>globalThis.ready=1</script></head><body>"
        "<style>@layer first{#probe{color:#ff0000}}"
        "@layer second{#probe{color:#0000ff}}</style>"
        "<p id=probe>Cache probe</p></body>";
    bool ok = true;
    for (unsigned pass = 0; pass < 2; pass++) {
        Budget budget;
        budget_init(&budget, 32u * MIB);
        bool installed = budget_install_lexbor(&budget);
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool browser_ready = installed
            && browser_session_init(&browser, &budget, 256u * 1024u);
        bool ready = browser_ready && navigation_init(&navigation, &budget, 4);
        const char *user_css = pass == 0 ? "@layer second,first;"
                                        : ".unused{color:#123456}";
        if (ready) {
            navigation_attach_browser_session(&navigation, &browser);
            navigation_enable_scripts(&navigation, 8u * MIB, 2000);
            navigation_enable_document_scripts(&navigation, 16,
                256u * 1024u, 64u * 1024u, 2000);
            navigation_enable_external_resources(&navigation, 4,
                64u * 1024u, 32u * 1024u, 1, 16u * 1024u,
                16u * 1024u, 64u * 1024u, 2000);
            ready = navigation_set_user_css(&navigation, user_css,
                                             strlen(user_css));
        }
        ReferrerNavigationReplay fixture = {
            .document_url = "https://style-context.test/",
            .stylesheet_url = "https://style-context.test/unused.css",
            .html = prefix, .css = "", .response_referrer_policy = "",
            .stylesheet_request_policy = ""
        };
        char directory[128] = {0}, error[256] = {0};
        bool written = ready && write_referrer_navigation_replay(directory,
                                                                  &fixture);
        bool replay = written && fetch_trace_replay_begin(directory, error,
                                                           sizeof(error));
        uint64_t generation = replay ? navigation_begin(&navigation) : 0;
        bool loaded = replay && navigation_load_url(&navigation, generation,
            fixture.document_url, 65536, 2000, 480, NULL, NULL, true);
        static const char other[] = "<!doctype html><p>Other page</p>";
        generation = loaded ? navigation_begin(&navigation) : 0;
        loaded = loaded && navigation_commit_static_html(&navigation,
            generation, "https://style-context.test/other", other,
            sizeof(other) - 1, 480, NULL, NULL, true);
        if (replay) fetch_trace_end();
        if (written) remove_referrer_navigation_replay(directory);
        fixture.html = expanded;
        written = loaded && write_referrer_navigation_replay(directory,
                                                              &fixture);
        replay = written && fetch_trace_replay_begin(directory, error,
                                                       sizeof(error));
        generation = replay ? navigation_begin(&navigation) : 0;
        loaded = replay && navigation_load_url(&navigation, generation,
            fixture.document_url, 65536, 2000, 480, NULL, NULL, true);
        ScriptResult result = {0};
        bool checked = loaded && navigation.page.runtime != NULL
            && script_runtime_evaluate_diagnostic(navigation.page.runtime,
                "globalThis.pocSummary=getComputedStyle("
                "document.getElementById('probe')).color", "<cache-probe>",
                &result)
            && strcmp(result.summary, "rgb(0, 0, 255)") == 0
            && navigation.performance.compiled_stylesheet_cache_hits == 1
            && navigation.performance.blocking_stylesheet_commit_continuations
                   == (pass == 0 ? 0u : 1u);
        if (!checked) fprintf(stderr,
            "cached user context pass=%u loaded=%d color=%s hits=%zu "
            "continuations=%zu error=%s\n", pass, loaded, result.summary,
            navigation.performance.compiled_stylesheet_cache_hits,
            navigation.performance.blocking_stylesheet_commit_continuations,
            navigation.last_error);
        if (replay) fetch_trace_end();
        if (written) remove_referrer_navigation_replay(directory);
        if (ready) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        ok = checked && budget.current == 0
            && budget_active_allocations(&budget, NULL) == 0 && ok;
        if (installed) ok = budget_uninstall_lexbor(&budget) && ok;
    }
    return ok;
}

static bool static_stylesheet_commit(NavigationSession *navigation,
                                     const char *html,
                                     TilefinchWorkCount *css_bytes)
{
    uint64_t generation = navigation_begin(navigation);
    TilefinchWorkCount before = tilefinch_work_tally.css_bytes;
    bool committed = generation != 0 && navigation_commit_static_html(
        navigation, generation, "https://static-style.test/page",
        html, strlen(html), 480, NULL, NULL, true);
    *css_bytes = tilefinch_work_tally.css_bytes - before;
    return committed && navigation->page.loaded;
}

static bool static_stylesheet_frame(Budget *budget,
                                    const NavigationSession *navigation,
                                    uint16_t *frame)
{
    TileCache cache = {0};
    bool ok = tile_cache_init(&cache, budget, &navigation->page.layout, 8)
        && tile_cache_set_frame(&cache, frame, 480u * 272u)
        && tile_cache_render_frame(&cache, 0, 480, 272, NULL);
    tile_cache_destroy(&cache);
    return ok;
}

/* A browser-authored static document (a site adapter's results or watch
   page, and Back to one) takes the compiled sheet the previous page left
   when its sheet inputs match, instead of compiling the same CSS again. */
static bool test_static_document_reuses_compiled_stylesheet(void)
{
    static const char results[] =
        "<!doctype html><title>Results</title><style>"
        "html,body{margin:0;background:#fff}h1{color:#c00;font-size:20px}"
        ".row{height:40px;border-bottom:2px solid #888}"
        "@media (max-width:500px){.row{background:#cde}}</style>"
        "<body><h1>Results</h1><div class=row>First result</div>"
        "<div class=row>Second result</div></body>";
    static const char watch[] =
        "<!doctype html><title>Watch</title><style>"
        "html,body{margin:0}.player{height:120px;background:#123}"
        "p{color:#363}</style><body><div class=player></div>"
        "<p>Watch page</p></body>";
    /* The same page with one declaration changed. */
    static const char results_changed[] =
        "<!doctype html><title>Results</title><style>"
        "html,body{margin:0;background:#fff}h1{color:#00c;font-size:20px}"
        ".row{height:40px;border-bottom:2px solid #888}"
        "@media (max-width:500px){.row{background:#cde}}</style>"
        "<body><h1>Results</h1><div class=row>First result</div>"
        "<div class=row>Second result</div></body>";
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
    size_t frame_bytes = 480u * 272u * sizeof(uint16_t);
    uint16_t *frames[4] = {0};
    for (size_t i = 0; navigation_ready && i < 4; i++) {
        frames[i] = budget_malloc(&budget, frame_bytes);
    }
    TilefinchWorkCount css[5] = {0};
    size_t hits[5] = {0};
    bool ok = navigation_ready && frames[0] != NULL && frames[1] != NULL
        && frames[2] != NULL && frames[3] != NULL;
    const char *pages[5] = { results, watch, results, watch,
                             results_changed };
    for (size_t i = 0; ok && i < 5; i++) {
        ok = static_stylesheet_commit(&navigation, pages[i], &css[i])
            && (i == 4 || static_stylesheet_frame(
                    &budget, &navigation, frames[i]));
        hits[i] = navigation.performance.compiled_stylesheet_cache_hits;
    }
    /* Pass 2 (results again) and pass 3 (watch again) each take the sheet
       the page before them left; the changed sheet compiles. */
    bool reused = ok && css[0] != 0 && css[1] != 0
        && hits[0] == 0 && hits[1] == 0
        && hits[2] == 1 && css[2] == 0
        && hits[3] == 2 && css[3] == 0
        && hits[4] == 2 && css[4] != 0
        && memcmp(frames[0], frames[2], frame_bytes) == 0
        && memcmp(frames[1], frames[3], frame_bytes) == 0
        && memcmp(frames[0], frames[1], frame_bytes) != 0
        && navigation.compiled_stylesheet_cache_ready
        && navigation.performance.compiled_stylesheet_cache_size_skips == 0;
    if (!reused) {
        fprintf(stderr,
                "static stylesheet reuse failed: ok=%d css=%llu/%llu/%llu/"
                "%llu/%llu hits=%zu/%zu/%zu/%zu/%zu frames=%d/%d error=%s\n",
                ok ? 1 : 0, (unsigned long long) css[0],
                (unsigned long long) css[1], (unsigned long long) css[2],
                (unsigned long long) css[3], (unsigned long long) css[4],
                hits[0], hits[1], hits[2], hits[3], hits[4],
                ok && memcmp(frames[0], frames[2], frame_bytes) == 0,
                ok && memcmp(frames[1], frames[3], frame_bytes) == 0,
                navigation.last_error);
    }
    for (size_t i = 0; i < 4; i++) budget_free(&budget, frames[i]);
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return reused && clean;
}

/* Media queries are answered when a sheet compiles, so night mode is part
   of the static key: a results page whose CSS has a prefers-color-scheme
   rule compiles again when night mode turns on, and again when it turns
   off, and only a revisit in the same mode takes the cached sheet. */
static bool test_static_stylesheet_cache_follows_dark_preference(void)
{
    static const char results[] =
        "<!doctype html><title>Results</title><style>"
        "html,body{margin:0;background:#fff}h1{color:#c00;font-size:20px}"
        ".row{height:40px;border-bottom:2px solid #888}"
        "@media (prefers-color-scheme:dark){html,body{background:#000}"
        "h1{color:#0f0}}</style>"
        "<body><h1>Results</h1><div class=row>First result</div>"
        "<div class=row>Second result</div></body>";
    static const char watch[] =
        "<!doctype html><title>Watch</title><style>"
        "html,body{margin:0}.player{height:120px;background:#123}"
        "p{color:#363}</style><body><div class=player></div>"
        "<p>Watch page</p></body>";
    enum { PASSES = 7 };
    /* Results light, watch light, then results and watch dark, results
       dark again (the first same-mode revisit), watch and results light. */
    const char *pages[PASSES] = { results, watch, results, watch, results,
                                  watch, results };
    const bool dark[PASSES] = { false, false, true, true, true,
                                false, false };
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
    size_t frame_bytes = 480u * 272u * sizeof(uint16_t);
    uint16_t *frames[PASSES] = {0};
    bool ok = navigation_ready;
    for (size_t i = 0; ok && i < PASSES; i++) {
        frames[i] = budget_malloc(&budget, frame_bytes);
        ok = frames[i] != NULL;
    }
    TilefinchWorkCount css[PASSES] = {0};
    size_t hits[PASSES] = {0};
    for (size_t i = 0; ok && i < PASSES; i++) {
        stylesheet_set_prefers_dark_color_scheme(dark[i]);
        ok = static_stylesheet_commit(&navigation, pages[i], &css[i])
            && static_stylesheet_frame(&budget, &navigation, frames[i]);
        hits[i] = navigation.performance.compiled_stylesheet_cache_hits;
    }
    stylesheet_set_prefers_dark_color_scheme(false);
    /* Each pass finds the sheet of the page two before it. */
    bool followed = ok
        && css[2] != 0 && hits[2] == 0
        && memcmp(frames[2], frames[0], frame_bytes) != 0
        && css[3] != 0 && hits[3] == 0
        && css[4] == 0 && hits[4] == 1
        && memcmp(frames[4], frames[2], frame_bytes) == 0
        && css[5] != 0 && hits[5] == 1
        && css[6] != 0 && hits[6] == 1
        && memcmp(frames[6], frames[0], frame_bytes) == 0
        && memcmp(frames[3], frames[1], frame_bytes) == 0;
    if (!followed) {
        fprintf(stderr, "static stylesheet dark key failed: ok=%d error=%s\n",
                ok ? 1 : 0, navigation.last_error);
        for (size_t i = 0; i < PASSES; i++) {
            fprintf(stderr, "  pass %zu dark=%d css=%llu hits=%zu "
                    "frame-equals-light=%d\n", i, dark[i] ? 1 : 0,
                    (unsigned long long) css[i], hits[i],
                    ok && memcmp(frames[i], frames[pages[i] == results
                        ? 0 : 1], frame_bytes) == 0);
        }
    }
    for (size_t i = 0; i < PASSES; i++) budget_free(&budget, frames[i]);
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return followed && clean;
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

/* Articles that share their <head> sources and differ in a body <style>
   (Wikipedia's TemplateStyles) reuse the retired page's compiled sheet cut
   back to the <head>. One session loads A, B, C, A: C starts from A's sheet
   (retired when B committed) and the second A from B's, and each must
   render exactly as a fresh session's load of the same article. B paints
   the probe red and borders every row, so a suffix leaking into C shows in
   the probe colour and the frame. Every article opens with a cookie notice
   and an ad, which the PSP's content-blocker sheets hide. */
typedef struct {
    uint64_t frame_hash;
    size_t rules;
    int height;
    uint32_t probe_color;
    int probe_border;
    /* The load's late night-mode flip or user-CSS set landed. */
    bool dark_flipped;
    bool user_css_set_late;
    /* The committed page recorded a head key for a later head take. */
    bool head_recorded;
    size_t history_index;
    size_t history_count;
    /* The pump in which the load tried a cached head (0: none) and the
       allocations made in that pump. */
    size_t take_pump;
    size_t take_allocations;
} HeadReuseSnapshot;

/* One load of a head-reuse session and what the PSP set up for it. */
typedef struct {
    const char *url;
    /* Back to the previous history entry, which must be `url`, loaded
       without recording history as the PSP's Back loads it. */
    bool back;
    bool dark;
    /* The PSP's presentation sheet (content blocker, cookie notices, Reader
       or Basic view, page font percent), set before the load; NULL for
       none. */
    const char *user_css;
    /* Nonzero: the load starts without user CSS and `user_css` is set after
       this pump (the set arriving late). */
    size_t user_css_after_pump;
    /* Nonzero: night mode turns on or off after this pump. */
    size_t dark_flip_after_pump;
    /* Set: head.css changed and the HTTP cache holds this response from
       this load on. */
    const char *head_css;
    /* Nonzero: in pump `fail_pump` the allocation after `fail_after`
       successful ones is refused. */
    size_t fail_pump;
    size_t fail_after;
} HeadReuseLoad;

/* Stores `css` as head.css's cached response (a revalidation that found
   it changed). */
static bool head_reuse_replace_head_css(BrowserSession *browser,
                                        const char *css)
{
    static const char url[] = "https://head-reuse.test/head.css";
    size_t length = strlen(css);
    unsigned char *copy = budget_malloc(browser->budget, length);
    if (copy == NULL) return false;
    memcpy(copy, css, length);
    BrowserSharedBody *body = browser_shared_body_take(
        browser->budget, copy, length);
    if (body == NULL) {
        budget_free(browser->budget, copy);
        return false;
    }
    TilefinchRequestContext context = {
        .target_url = url, .initiator_url = url, .top_level_url = url,
        .method = "GET", .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .destination = TILEFINCH_DESTINATION_STYLE
    };
    TilefinchResourceGrant grant = {
        .destination = TILEFINCH_DESTINATION_STYLE,
        .mode = TILEFINCH_REQUEST_MODE_NO_CORS,
        .credentials = TILEFINCH_CREDENTIALS_INCLUDE,
        .final_same_origin = true, .final_same_site = true,
        .cors_validated = true
    };
    bool stored = browser_session_cache_put_http_shared_resource(
            browser, url, body, "head-v2", NULL, "text/css", "immutable",
            NULL, 0, &context, &grant)
        && browser_session_cache_set_resource_response_provenance(
            browser, url, &context, url, "");
    browser_shared_body_release(body);
    return stored;
}

static bool head_reuse_load(NavigationSession *navigation, Budget *budget,
                            BrowserSession *browser,
                            const HeadReuseLoad *spec,
                            HeadReuseSnapshot *snapshot)
{
    enum { FRAME_WIDTH = 480, FRAME_HEIGHT = 272 };
    stylesheet_set_prefers_dark_color_scheme(spec->dark);
    const char *css = spec->user_css_after_pump != 0 ? NULL : spec->user_css;
    if (!navigation_set_user_css(navigation, css,
                                 css == NULL ? 0 : strlen(css))
        || (spec->head_css != NULL
            && !head_reuse_replace_head_css(browser, spec->head_css)))
        return false;
    if (spec->back) {
        const NavigationEntry *entry = NULL;
        if (!navigation_back(navigation, &entry) || entry == NULL
            || strcmp(entry->url, spec->url) != 0) return false;
    }
    uint64_t generation = navigation_begin(navigation);
    NavigationLoad *load = navigation_load_begin_url(
        navigation, generation, spec->url, 4096, 1000, FRAME_WIDTH, NULL,
        NULL, !spec->back);
    NavigationLoadQuota quota = {
        .fetch = {
            .maximum_body_callbacks = 1,
            .maximum_body_bytes = 64,
            .maximum_time_us = 10000
        },
        .maximum_parser_body_bytes = 64,
        .maximum_parser_time_us = 10000
    };
    const NavigationPerformance *performance = &navigation->performance;
    bool set_failed = false;
    for (size_t i = 0; load != NULL && i < 256; i++) {
        if (navigation_load_status(load) != NAVIGATION_LOAD_PENDING) break;
        const size_t pump = i + 1;
        size_t takes = performance->compiled_stylesheet_cache_head_hits
            + performance->compiled_stylesheet_cache_head_refusals;
        size_t allocations = budget->allocation_count;
        if (spec->fail_pump == pump)
            budget_inject_failure_after(budget, spec->fail_after);
        (void) navigation_load_pump(load, &quota);
        if (spec->fail_pump == pump) budget_clear_failure_injection(budget);
        if (snapshot->take_pump == 0
            && performance->compiled_stylesheet_cache_head_hits
                   + performance->compiled_stylesheet_cache_head_refusals
               != takes) {
            snapshot->take_pump = pump;
            snapshot->take_allocations =
                budget->allocation_count - allocations;
        }
        if (navigation_load_status(load) != NAVIGATION_LOAD_PENDING)
            continue;
        if (spec->dark_flip_after_pump == pump) {
            stylesheet_set_prefers_dark_color_scheme(
                !stylesheet_prefers_dark_color_scheme());
            snapshot->dark_flipped = true;
        }
        if (spec->user_css_after_pump == pump) {
            set_failed = !navigation_set_user_css(
                navigation, spec->user_css, strlen(spec->user_css));
            snapshot->user_css_set_late = true;
        }
    }
    bool loaded = load != NULL && !set_failed
        && finish_bounded(load, &quota);
    navigation_load_destroy(load);
    size_t frame_pixels = (size_t) FRAME_WIDTH * FRAME_HEIGHT;
    uint16_t *frame = loaded
        ? budget_malloc(budget, frame_pixels * sizeof(*frame)) : NULL;
    TileCache cache = {0};
    bool rendered = frame != NULL
        && tile_cache_init(&cache, budget, &navigation->page.layout, 4)
        && tile_cache_set_frame(&cache, frame, frame_pixels)
        && tile_cache_render_frame(
               &cache, 0, FRAME_WIDTH, FRAME_HEIGHT, NULL);
    lxb_dom_node_t *probe = rendered ? test_find_id(
        lxb_dom_interface_node(navigation->page.document.html), "probe")
        : NULL;
    if (probe != NULL) {
        ComputedStyle style = style_for_node(
            &navigation->page.stylesheet, probe, NULL);
        snapshot->frame_hash = hash_frame_pixels(frame, frame_pixels);
        snapshot->rules = navigation->page.stylesheet.count;
        snapshot->height = navigation->page.layout.height;
        snapshot->probe_color = style.color;
        snapshot->probe_border = style.border.bottom;
        snapshot->head_recorded =
            navigation->page.compiled_stylesheet_head_valid;
        snapshot->history_index = navigation->history_index;
        snapshot->history_count = navigation->history_count;
    }
    tile_cache_destroy(&cache);
    budget_free(budget, frame);
    return probe != NULL;
}

/* Runs `loads` in one session, JS off with progressive paint, as the PSP
   loads Wikipedia. *loaded counts the loads that rendered (the session
   stops at the first that does not) and *clean is whether teardown
   returned every byte. False when the session could not be set up. */
static bool head_reuse_run(const HeadReuseLoad *loads, size_t count,
                           HeadReuseSnapshot *snapshots,
                           NavigationPerformance *performance,
                           size_t *loaded, bool *clean, bool report)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    ProgressivePreviewProbe probe = {0};
    bool browser_ready = lexbor_installed
        && browser_session_init(&browser, &budget, 256 * 1024);
    bool ready = browser_ready
        && navigation_init(&navigation, &budget, 4)
        && navigation_set_progressive_paint_hook(
               &navigation, capture_progressive_preview, &probe);
    if (ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 2, 32 * 1024, 16 * 1024,
            2, 32 * 1024, 16 * 1024, 64 * 1024, 1000);
        navigation_set_stream_delivery(&navigation, 64, 0, 0, 0, 0, 0);
        /* The browser engine previews over the incumbent page. */
        navigation_set_progressive_paint_preserves_incumbent(
            &navigation, true);
    }
    char error[256] = {0};
    bool replay = ready && fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-stylesheet-head-reuse",
        error, sizeof(error));
    *loaded = 0;
    for (size_t i = 0; replay && i < count; i++) {
        snapshots[i] = (HeadReuseSnapshot) {0};
        if (!head_reuse_load(&navigation, &budget, &browser, &loads[i],
                             &snapshots[i])) break;
        (*loaded)++;
    }
    stylesheet_set_prefers_dark_color_scheme(false);
    if (replay) *performance = navigation.performance;
    if (report && replay && *loaded != count)
        fprintf(stderr, "head reuse load %zu (%s) failed: nav-error=%s\n",
                *loaded, loads[*loaded].url, navigation.last_error);
    if (replay) fetch_trace_end();
    if (ready || browser_ready) {
        if (ready) navigation_destroy(&navigation);
        browser_session_destroy(&browser);
    }
    *clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed)
        *clean = budget_uninstall_lexbor(&budget) && *clean;
    if (report && !replay)
        fprintf(stderr, "head reuse session failed: ready=%d error=%s\n",
                ready ? 1 : 0, error);
    return replay;
}

static bool head_reuse_session(const HeadReuseLoad *loads, size_t count,
                               HeadReuseSnapshot *snapshots,
                               NavigationPerformance *performance)
{
    size_t loaded = 0;
    bool clean = false;
    bool ready = head_reuse_run(loads, count, snapshots, performance,
                                &loaded, &clean, true);
    if (ready && !clean)
        fprintf(stderr, "head reuse session leaked at teardown\n");
    return ready && loaded == count && clean;
}

/* What a fresh session loads to render what `load` rendered: a late
   user-CSS set or night-mode flip that landed is in place from the start,
   Back is a plain load of its destination, and nothing is refused. */
static HeadReuseLoad head_reuse_fresh_twin(const HeadReuseLoad *load,
                                           bool dark_flipped,
                                           bool user_css_set_late,
                                           const char *head_css)
{
    return (HeadReuseLoad) {
        .url = load->url,
        .dark = load->dark != dark_flipped,
        .user_css = load->user_css_after_pump != 0 && !user_css_set_late
            ? NULL : load->user_css,
        .head_css = head_css
    };
}

static bool head_reuse_snapshots_equal(const HeadReuseSnapshot *a,
                                       const HeadReuseSnapshot *b)
{
    return a->frame_hash == b->frame_hash && a->rules == b->rules
        && a->height == b->height && a->probe_color == b->probe_color
        && a->probe_border == b->probe_border;
}

static void head_reuse_report(const char *name, const HeadReuseLoad *loads,
                              const HeadReuseSnapshot *reused,
                              const HeadReuseSnapshot *fresh, size_t count,
                              const NavigationPerformance *performance)
{
    fprintf(stderr, "%s: hits=%zu head-hits=%zu head-refusals=%zu "
            "continuations=%zu commit-continuations=%zu builds=%zu\n",
            name, performance->compiled_stylesheet_cache_hits,
            performance->compiled_stylesheet_cache_head_hits,
            performance->compiled_stylesheet_cache_head_refusals,
            performance->blocking_stylesheet_continuations,
            performance->blocking_stylesheet_commit_continuations,
            performance->blocking_stylesheet_builds);
    for (size_t i = 0; i < count; i++) {
        fprintf(stderr, "  load %zu %s%s dark=%d css=%s: reused %016llx "
                "rules=%zu h=%d probe=%06x/%d", i, loads[i].url,
                loads[i].back ? " (Back)" : "", loads[i].dark ? 1 : 0,
                loads[i].user_css == NULL ? "none" : "set",
                (unsigned long long) reused[i].frame_hash, reused[i].rules,
                reused[i].height, reused[i].probe_color,
                reused[i].probe_border);
        if (fresh != NULL)
            fprintf(stderr, " fresh %016llx rules=%zu h=%d probe=%06x/%d",
                    (unsigned long long) fresh[i].frame_hash, fresh[i].rules,
                    fresh[i].height, fresh[i].probe_color,
                    fresh[i].probe_border);
        fputc('\n', stderr);
    }
}

enum { HEAD_REUSE_MAX_LOADS = 8 };

/* Each load of `loads` alone in a fresh session, as reused[] says it
   rendered (late changes landed or not). */
static bool head_reuse_fresh_twins(const HeadReuseLoad *loads, size_t count,
                                   const HeadReuseSnapshot *reused,
                                   HeadReuseSnapshot *fresh)
{
    const char *head_css = NULL;
    bool ok = count <= HEAD_REUSE_MAX_LOADS;
    for (size_t i = 0; ok && i < count; i++) {
        if (loads[i].head_css != NULL) head_css = loads[i].head_css;
        HeadReuseLoad twin = head_reuse_fresh_twin(
            &loads[i], reused[i].dark_flipped, reused[i].user_css_set_late,
            head_css);
        NavigationPerformance unused = {0};
        ok = head_reuse_session(&twin, 1, &fresh[i], &unused);
    }
    return ok;
}

/* Runs `loads` in one session and each load alone in a fresh session,
   and requires every load to render as its fresh twin. */
static bool head_reuse_loads_match_fresh(const char *name,
                                         const HeadReuseLoad *loads,
                                         size_t count,
                                         HeadReuseSnapshot *reused,
                                         NavigationPerformance *performance)
{
    HeadReuseSnapshot fresh[HEAD_REUSE_MAX_LOADS];
    memset(fresh, 0, sizeof(fresh));
    bool ok = count <= HEAD_REUSE_MAX_LOADS
        && head_reuse_session(loads, count, reused, performance)
        && head_reuse_fresh_twins(loads, count, reused, fresh);
    bool same = ok;
    for (size_t i = 0; same && i < count; i++)
        same = head_reuse_snapshots_equal(&reused[i], &fresh[i]);
    if (ok && !same)
        head_reuse_report(name, loads, reused, fresh, count, performance);
    return same;
}

/* SIZE_MAX leaves a counter unchecked. */
static bool head_reuse_counted(const char *name,
                               const NavigationPerformance *performance,
                               size_t hits, size_t head_hits,
                               size_t head_refusals)
{
    bool counted =
        (hits == SIZE_MAX
         || performance->compiled_stylesheet_cache_hits == hits)
        && (head_hits == SIZE_MAX
            || performance->compiled_stylesheet_cache_head_hits == head_hits)
        && (head_refusals == SIZE_MAX
            || performance->compiled_stylesheet_cache_head_refusals
                   == head_refusals);
    if (!counted)
        fprintf(stderr, "%s: hits=%zu head-hits=%zu head-refusals=%zu\n",
                name, performance->compiled_stylesheet_cache_hits,
                performance->compiled_stylesheet_cache_head_hits,
                performance->compiled_stylesheet_cache_head_refusals);
    return counted;
}

static bool head_reuse_sequence_matches_fresh_load(const char *user_css,
                                                   size_t expected_hits)
{
    static const char *const sequence[] = {
        "https://head-reuse.test/a", "https://head-reuse.test/b",
        "https://head-reuse.test/c", "https://head-reuse.test/a"
    };
    const char *name = user_css == NULL
        ? "compiled head reuse" : "compiled head reuse with user CSS";
    HeadReuseLoad loads[4];
    for (size_t i = 0; i < 4; i++)
        loads[i] = (HeadReuseLoad) { .url = sequence[i],
                                     .user_css = user_css };
    HeadReuseSnapshot reused[4];
    NavigationPerformance performance = {0};
    bool same = head_reuse_loads_match_fresh(name, loads, 4, reused,
                                             &performance);
    /* B finds nothing cached: A is still the live page; C and the second A
       take a head each, or refuse it. */
    bool counted = same && head_reuse_counted(
        name, &performance, 0, expected_hits, 2 - expected_hits);
    bool unleaked = same && reused[1].probe_color == 0xff0000
        && reused[1].probe_border == 2
        && reused[2].probe_color == 0x0a0b0c && reused[2].probe_border == 0
        && reused[3].probe_color == 0x0a0b0c && reused[3].probe_border == 0;
    if (same && !unleaked)
        head_reuse_report(name, loads, reused, NULL, 4, &performance);
    return same && counted && unleaked;
}

static bool test_compiled_stylesheet_head_reuse_matches_fresh_load(void)
{
    /* With a user stylesheet like the PSP's content-blocker hiding rules
       (letter-spacing shows in the frame), and with one that interns a
       pool entry (a border colour): the latter changes the parse context
       a continuation is checked against, so the take is refused and the
       page builds its sheet afresh, as a streamed continuation would. */
    return head_reuse_sequence_matches_fresh_load(NULL, 2)
        && head_reuse_sequence_matches_fresh_load(
               ".row{letter-spacing:1px}.ad-banner{display:none!important}",
               2)
        && head_reuse_sequence_matches_fresh_load(
               ".row{letter-spacing:1px}h1{border-top:3px solid #445566}",
               0);
}

/* Loads `urls` in one session with night mode per `dark` and requires
   each load to render as a fresh session's load of it in the same mode,
   taking `expected_hits` whole cached sheets and `expected_head_hits`
   cached heads. */
static bool head_reuse_dark_sequence_matches_fresh_load(
    const char *name, const char *const *urls, const bool *dark,
    size_t count, size_t expected_hits, size_t expected_head_hits)
{
    HeadReuseLoad loads[HEAD_REUSE_MAX_LOADS];
    HeadReuseSnapshot reused[HEAD_REUSE_MAX_LOADS];
    if (count > HEAD_REUSE_MAX_LOADS) return false;
    for (size_t i = 0; i < count; i++)
        loads[i] = (HeadReuseLoad) { .url = urls[i], .dark = dark[i] };
    NavigationPerformance performance = {0};
    return head_reuse_loads_match_fresh(name, loads, count, reused,
                                        &performance)
        && head_reuse_counted(name, &performance, expected_hits,
                              expected_head_hits, SIZE_MAX);
}

/* Night mode is part of both streamed keys: head.css has a
   prefers-color-scheme rule, and a sheet (or head) compiled in one mode is
   never taken in the other. Each load finds the sheet of the load two
   before it. */
static bool test_compiled_stylesheet_reuse_follows_dark_preference(void)
{
    static const char *const exact[] = {
        "https://head-reuse.test/a", "https://head-reuse.test/b",
        "https://head-reuse.test/a", "https://head-reuse.test/b",
        "https://head-reuse.test/a", "https://head-reuse.test/b",
        "https://head-reuse.test/a"
    };
    /* A light twice; A dark misses A light; A dark again takes it; A
       light misses A dark. */
    static const bool exact_dark[] = {
        false, false, true, true, true, false, false
    };
    static const char *const head[] = {
        "https://head-reuse.test/a", "https://head-reuse.test/b",
        "https://head-reuse.test/c", "https://head-reuse.test/a",
        "https://head-reuse.test/b", "https://head-reuse.test/c"
    };
    /* C and A dark refuse light heads; B dark takes C's dark head; C
       light refuses A's dark head. */
    static const bool head_dark[] = {
        false, false, true, true, true, false
    };
    bool whole = head_reuse_dark_sequence_matches_fresh_load(
        "whole-sheet", exact, exact_dark,
        sizeof(exact) / sizeof(exact[0]), 1, 0);
    bool heads = head_reuse_dark_sequence_matches_fresh_load(
        "head", head, head_dark, sizeof(head) / sizeof(head[0]), 0, 1);
    return whole && heads;
}

/* The root's theme selection is part of the head key: TA and TB select
   the light theme of an inert data-color-theme registry, TC the dark one,
   over the same head links. TC must not take TA's head, which applied the
   light theme's href and skipped the dark one's. */
static bool test_compiled_head_key_follows_root_theme(void)
{
    static const char *const urls[] = {
        "https://head-reuse.test/ta", "https://head-reuse.test/tb",
        "https://head-reuse.test/tc"
    };
    static const bool light[] = { false, false, false };
    return head_reuse_dark_sequence_matches_fresh_load(
        "root-theme", urls, light, sizeof(urls) / sizeof(urls[0]), 0, 0);
}

/* A setting that changes while the first of `loads` is loading: night mode
   (`user_css` false) or the user stylesheet arriving late. The change
   lands after each pump in turn, and every load must render as a fresh
   load made with the change in place. */
static bool head_reuse_late_change_matches_fresh(
    const char *name, const HeadReuseLoad *loads, size_t count,
    bool user_css)
{
    HeadReuseLoad sweep[HEAD_REUSE_MAX_LOADS];
    HeadReuseSnapshot reused[HEAD_REUSE_MAX_LOADS];
    HeadReuseSnapshot fresh[HEAD_REUSE_MAX_LOADS];
    if (count == 0 || count > HEAD_REUSE_MAX_LOADS) return false;
    memcpy(sweep, loads, count * sizeof(*sweep));
    memset(reused, 0, sizeof(reused));
    memset(fresh, 0, sizeof(fresh));
    reused[0].dark_flipped = !user_css;
    reused[0].user_css_set_late = user_css;
    bool ok = head_reuse_fresh_twins(loads, count, reused, fresh);
    size_t landed = 0;
    for (size_t pump = 1; ok && pump <= 64; pump++) {
        if (user_css) sweep[0].user_css_after_pump = pump;
        else sweep[0].dark_flip_after_pump = pump;
        NavigationPerformance performance = {0};
        ok = head_reuse_session(sweep, count, reused, &performance);
        if (!ok || !(user_css ? reused[0].user_css_set_late
                              : reused[0].dark_flipped)) break;
        landed++;
        bool same = true;
        for (size_t i = 0; same && i < count; i++)
            same = head_reuse_snapshots_equal(&reused[i], &fresh[i]);
        if (!same) {
            fprintf(stderr, "%s, after pump %zu:\n", name, pump);
            head_reuse_report(name, sweep, reused, fresh, count,
                              &performance);
            ok = false;
        }
    }
    /* The load takes a few dozen pumps; every one of them was tried. */
    if (ok && landed < 8) {
        fprintf(stderr, "%s: only %zu pumps\n", name, landed);
        ok = false;
    }
    return ok;
}

/* Night mode turning on (or off) while a page loads: a streaming sheet
   compiled before the change answered prefers-color-scheme the other way,
   so inputs that arrive after the change must not be appended to it. */
static bool streaming_sheet_follows_mid_load_dark_flip(bool start_dark)
{
    /* D's last <style> follows its rows, after the first preview. */
    const HeadReuseLoad load = {
        .url = "https://head-reuse.test/d", .dark = start_dark
    };
    return head_reuse_late_change_matches_fresh(
        start_dark ? "mid-load night-mode flip from dark"
                   : "mid-load night-mode flip from light", &load, 1, false);
}

static bool test_streaming_sheet_follows_mid_load_dark_flip(void)
{
    bool to_dark = streaming_sheet_follows_mid_load_dark_flip(false);
    bool to_light = streaming_sheet_follows_mid_load_dark_flip(true);
    stylesheet_set_prefers_dark_color_scheme(false);
    return to_dark && to_light;
}

#define HEAD_REUSE_A "https://head-reuse.test/a"
#define HEAD_REUSE_B "https://head-reuse.test/b"
#define HEAD_REUSE_C "https://head-reuse.test/c"
#define HEAD_REUSE_D "https://head-reuse.test/d"

/* The content-blocker sheets the PSP adds to its presentation CSS. */
static bool head_reuse_blocker_css(char *cookie, size_t cookie_capacity,
                                   char *cosmetic, size_t cosmetic_capacity)
{
    size_t length = 0;
    return content_blocker_cookie_banner_css(
               cookie, cookie_capacity, &length)
        && content_blocker_cosmetic_css(
               cosmetic, cosmetic_capacity, &length);
}

/* The user stylesheet changing between articles: cookie notices hidden for
   A, B and C, then not for A, then hidden for B, then C on a site where
   only the cosmetic rules apply. Each load sees the sheet of the load two
   before it: C and the second B take a head compiled under the same user
   stylesheet; the second A and the second C must not take one compiled
   under another. */
static bool test_head_reuse_follows_user_css_changes(void)
{
    char cookie[CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    char cosmetic[CONTENT_BLOCKER_COSMETIC_CSS_LIMIT];
    if (!head_reuse_blocker_css(cookie, sizeof(cookie),
                                cosmetic, sizeof(cosmetic))) return false;
    const HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A, .user_css = cookie },
        { .url = HEAD_REUSE_B, .user_css = cookie },
        { .url = HEAD_REUSE_C, .user_css = cookie },
        { .url = HEAD_REUSE_A },
        { .url = HEAD_REUSE_B, .user_css = cookie },
        { .url = HEAD_REUSE_C, .user_css = cosmetic }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT];
    NavigationPerformance performance = {0};
    const char *name = "head reuse across user CSS changes";
    return head_reuse_loads_match_fresh(name, loads, COUNT, reused,
                                        &performance)
        && head_reuse_counted(name, &performance, 0, 2, 0);
}

/* Reader or Basic view (both present the page with the Reader sheet) and
   the page font percent toggled between articles: the second A and C
   load under the text-size sheet, B and the first C under Reader's. The
   second C takes the second A's head; the second B's take of the first
   C's is refused: adding the Reader sheet changes the parse context a
   continuation is checked against, as the border-colour sheet above
   does. */
static bool test_head_reuse_follows_reader_and_font_percent(void)
{
    char reader_b[SITE_ADAPTER_READER_CSS_LIMIT];
    char reader_c[SITE_ADAPTER_READER_CSS_LIMIT];
    char adapter[32];
    static const char font[] = "html{font-size:120%!important}";
    if (!site_adapter_reader_css(HEAD_REUSE_B, SITE_ADAPTER_READER_FONT_SANS,
                                 100u, reader_b, sizeof(reader_b), adapter,
                                 sizeof(adapter))
        || !site_adapter_reader_css(HEAD_REUSE_C,
                                    SITE_ADAPTER_READER_FONT_SANS, 100u,
                                    reader_c, sizeof(reader_c), adapter,
                                    sizeof(adapter))) return false;
    const HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A },
        { .url = HEAD_REUSE_B },
        { .url = HEAD_REUSE_C, .user_css = reader_c },
        { .url = HEAD_REUSE_A, .user_css = font },
        { .url = HEAD_REUSE_B, .user_css = reader_b },
        { .url = HEAD_REUSE_C, .user_css = font }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT];
    NavigationPerformance performance = {0};
    const char *name = "head reuse across Reader and text size";
    return head_reuse_loads_match_fresh(name, loads, COUNT, reused,
                                        &performance)
        && head_reuse_counted(name, &performance, 0, 1, 1);
}

/* head.css changes on the server before C: the HTTP cache's stylesheet
   signature moves, so neither C nor the second A may start from a head
   compiled from the old response. */
static bool test_head_reuse_drops_sheet_of_changed_head_link(void)
{
    static const char changed[] =
        "body{margin:0;font-size:16px}h1{color:#123456}"
        ".row{margin:0;padding:1px}#probe{color:#0d0e0f}";
    const HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A }, { .url = HEAD_REUSE_B },
        { .url = HEAD_REUSE_C, .head_css = changed },
        { .url = HEAD_REUSE_A }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT];
    NavigationPerformance performance = {0};
    const char *name = "head reuse after head.css changed";
    bool ok = head_reuse_loads_match_fresh(name, loads, COUNT, reused,
                                           &performance)
        && head_reuse_counted(name, &performance, 0, 0, 0);
    if (ok && (reused[2].probe_color != 0x0d0e0f
               || reused[3].probe_color != 0x0d0e0f)) {
        head_reuse_report(name, loads, reused, NULL, COUNT, &performance);
        ok = false;
    }
    return ok;
}

/* BL shares A's <head> but links a sheet from its body: its compiled sheet
   records no head key (the cut cannot drop an external body source), so
   the second A, which finds BL's sheet cached, takes no head from it.
   (body.css entering the HTTP cache also moves the stylesheet signature,
   so C cannot take A's head, compiled before it.) */
static bool test_head_reuse_skips_body_link_page(void)
{
    const HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A },
        { .url = "https://head-reuse.test/bl" },
        { .url = HEAD_REUSE_C }, { .url = HEAD_REUSE_A }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT];
    NavigationPerformance performance = {0};
    const char *name = "head reuse with a body link";
    bool ok = head_reuse_loads_match_fresh(name, loads, COUNT, reused,
                                           &performance)
        && head_reuse_counted(name, &performance, 0, 0, 0);
    if (ok && (!reused[0].head_recorded || reused[1].head_recorded
               || reused[1].probe_border != 3)) {
        fprintf(stderr, "%s: head recorded A=%d BL=%d, BL border %d\n",
                name, reused[0].head_recorded ? 1 : 0,
                reused[1].head_recorded ? 1 : 0, reused[1].probe_border);
        ok = false;
    }
    return ok;
}

/* A, B, then Back to A: A's sheet, retired when B committed, serves the
   Back load whole, which restores A's history entry without adding one. */
static bool test_head_reuse_back_takes_whole_sheet(void)
{
    const HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A }, { .url = HEAD_REUSE_B },
        { .url = HEAD_REUSE_A, .back = true }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT];
    NavigationPerformance performance = {0};
    const char *name = "head reuse with Back";
    bool ok = head_reuse_loads_match_fresh(name, loads, COUNT, reused,
                                           &performance)
        && head_reuse_counted(name, &performance, 1, 0, 0);
    if (ok && (reused[1].history_index != 1 || reused[1].history_count != 2
               || reused[2].history_index != 0
               || reused[2].history_count != 2)) {
        fprintf(stderr, "%s: history B %zu/%zu, Back %zu/%zu\n", name,
                reused[1].history_index, reused[1].history_count,
                reused[2].history_index, reused[2].history_count);
        ok = false;
    }
    return ok;
}

/* A refused allocation while C takes A's head (the cut, or the user
   stylesheet added back after it): the take is refused, the page builds
   its sheet afresh and renders as a fresh load, and teardown returns
   every byte. Each allocation of the take's pump is refused in turn. */
static bool test_head_reuse_take_survives_budget_refusal(void)
{
    static const char user_css[] = ".row{letter-spacing:1px}";
    HeadReuseLoad loads[] = {
        { .url = HEAD_REUSE_A, .user_css = user_css },
        { .url = HEAD_REUSE_B, .user_css = user_css },
        { .url = HEAD_REUSE_C, .user_css = user_css }
    };
    enum { COUNT = sizeof(loads) / sizeof(loads[0]) };
    HeadReuseSnapshot reused[COUNT], fresh[COUNT];
    NavigationPerformance performance = {0};
    bool ok = head_reuse_session(loads, COUNT, reused, &performance)
        && head_reuse_fresh_twins(loads, COUNT, reused, fresh)
        && head_reuse_counted("head take before refusals", &performance,
                              0, 1, 0)
        && reused[2].take_pump != 0;
    size_t refused = 0, failed = 0, trials = 0;
    const size_t window = reused[2].take_allocations + 16u;
    for (size_t after = 0; ok && after <= window; after++) {
        loads[2].fail_pump = reused[2].take_pump;
        loads[2].fail_after = after;
        HeadReuseSnapshot trial[COUNT];
        size_t loaded = 0;
        bool clean = false;
        performance = (NavigationPerformance) {0};
        bool ready = head_reuse_run(loads, COUNT, trial, &performance,
                                    &loaded, &clean, false);
        trials++;
        if (!ready || !clean || loaded < 2) {
            fprintf(stderr, "head take refusal %zu: ready=%d clean=%d "
                    "loaded=%zu\n", after, ready ? 1 : 0, clean ? 1 : 0,
                    loaded);
            ok = false;
            break;
        }
        if (loaded < COUNT) {
            failed++;
            continue;
        }
        if (performance.compiled_stylesheet_cache_head_refusals == 0)
            continue;
        refused++;
        if (performance.compiled_stylesheet_cache_head_hits != 0
            || !head_reuse_snapshots_equal(&trial[2], &fresh[2])) {
            fprintf(stderr, "head take refusal %zu:\n", after);
            head_reuse_report("head take refusal", loads, trial, fresh,
                              COUNT, &performance);
            ok = false;
        }
    }
    if (ok && refused == 0) {
        fprintf(stderr, "head take refusal: no refused take in %zu trials "
                "(%zu failed loads, pump %zu, %zu allocations)\n", trials,
                failed, reused[2].take_pump, reused[2].take_allocations);
        ok = false;
    }
    return ok;
}

/* The user stylesheet set while the first article is loading (the PSP's
   presentation sheet arriving late): the committed sheet must carry it,
   as a load that had it from the start does, and so must what that sheet
   later hands on: the whole sheet to the second D and its head to C. */
static bool test_late_user_css_reaches_committed_sheet(void)
{
    char cookie[CONTENT_BLOCKER_COOKIE_CSS_LIMIT];
    char cosmetic[CONTENT_BLOCKER_COSMETIC_CSS_LIMIT];
    if (!head_reuse_blocker_css(cookie, sizeof(cookie),
                                cosmetic, sizeof(cosmetic))) return false;
    const HeadReuseLoad single[] = {
        { .url = HEAD_REUSE_D, .user_css = cookie }
    };
    const HeadReuseLoad sequence[] = {
        { .url = HEAD_REUSE_D, .user_css = cookie },
        { .url = HEAD_REUSE_B, .user_css = cookie },
        { .url = HEAD_REUSE_D, .user_css = cookie },
        { .url = HEAD_REUSE_C, .user_css = cookie }
    };
    return head_reuse_late_change_matches_fresh(
               "late user CSS", single, 1, true)
        && head_reuse_late_change_matches_fresh(
               "late user CSS handed on", sequence,
               sizeof(sequence) / sizeof(sequence[0]), true);
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
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
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
    /* Accept-CH persists for the browsing session. A later navigation to the
       same origin must send the negotiated hint on its first request and
       consume the hinted trace record directly, without another restart. */
    size_t retries_after_first_navigation =
        navigation.client_hint_retries;
    bool repeat_trace_ready = ok && persisted_client_hint_replay_begin();
    uint64_t repeat_generation = repeat_trace_ready
        ? navigation_begin(&navigation) : 0;
    bool repeated = repeat_trace_ready && navigation_load_url(
        &navigation, repeat_generation,
        "https://critical.test/document", 4096, 1000, 480,
        NULL, NULL, true);
    ok = ok && repeated
        && navigation.client_hint_retries
               == retries_after_first_navigation
        && strcmp(navigation.page.document.title,
                  "Critical-CH retry") == 0
        && strcmp(navigation.page.script_result.summary,
                  "retry-clean") == 0;
    if (!ok) {
        fprintf(stderr,
                "cached Critical-CH navigation failed: trace=%d repeated=%d "
                "retries=%zu expected=%zu title=%s summary=%s error=%s\n",
                repeat_trace_ready ? 1 : 0, repeated ? 1 : 0,
                navigation.client_hint_retries,
                retries_after_first_navigation,
                navigation.page.document.title == NULL
                    ? "(null)" : navigation.page.document.title,
                navigation.page.script_result.summary,
                navigation.last_error);
    }
    if (repeat_trace_ready) fetch_trace_end();
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
    navigation_enable_scripts(&navigation, 3 * MIB, 1000);
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
        && strstr(challenge_cookies, "cf_clearance=") == NULL
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

typedef struct {
    size_t active_calls;
    size_t final_calls;
    size_t indices[4];
    size_t final_index;
} ScriptFrontierProbe;
