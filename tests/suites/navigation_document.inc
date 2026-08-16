static bool test_parser_blocking_script_classification(void)
{
    static const char html[] =
        "<!doctype html><body>"
        "<script id='inline'></script>"
        "<script id='classic' src='/a.js'></script>"
        "<script id='defer' src='/b.js' defer></script>"
        "<script id='async' src='/c.js' async></script>"
        "<script id='module' type='module'></script>"
        "<script id='inert' type='application/json'></script>"
        "</body></html>";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    PocDocument document = {0};
    bool installed = budget_install_lexbor(&budget);
    bool parsed = installed && document_parse(
        &document, &budget, html, sizeof(html) - 1, 31);
    lxb_dom_node_t *root = parsed
        ? lxb_dom_interface_node(document.html) : NULL;
    bool inline_blocking = document_script_is_parser_blocking(
        test_find_id(root, "inline"));
    bool classic_blocking = document_script_is_parser_blocking(
        test_find_id(root, "classic"));
    bool defer_blocking = document_script_is_parser_blocking(
        test_find_id(root, "defer"));
    bool async_blocking = document_script_is_parser_blocking(
        test_find_id(root, "async"));
    bool module_blocking = document_script_is_parser_blocking(
        test_find_id(root, "module"));
    bool inert_blocking = document_script_is_parser_blocking(
        test_find_id(root, "inert"));
    bool ok = parsed && inline_blocking && classic_blocking
        && !defer_blocking && !async_blocking
        && !module_blocking && !inert_blocking;
    if (!ok) {
        fprintf(stderr,
                "script classification parsed=%d inline=%d classic=%d "
                "defer=%d async=%d module=%d inert=%d\n",
                parsed, inline_blocking, classic_blocking, defer_blocking,
                async_blocking, module_blocking, inert_blocking);
    }
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (installed) clean = budget_uninstall_lexbor(&budget) && clean;
    return ok && clean;
}

static lxb_dom_node_t *test_find_id(lxb_dom_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *value = document_attribute(node, "id", &length);
        if (value != NULL && strlen(id) == length
            && memcmp(value, id, length) == 0) return node;
        lxb_dom_node_t *found = test_find_id(node->first_child, id);
        if (found != NULL) return found;
    }
    return NULL;
}

static bool run_document_referrer_policy_case(
    const ReferrerNavigationReplay *fixture, const char *expected_policy)
{
    char replay_directory[128] = {0};
    bool replay_written = write_referrer_navigation_replay(
        replay_directory, fixture);
    Budget budget;
    budget_init(&budget, 20u * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = replay_written && lexbor_installed
        && browser_session_init(&browser, &budget, 256u * 1024u);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 4, 64u * 1024u, 32u * 1024u,
            1, 16u * 1024u, 16u * 1024u, 64u * 1024u, 1000);
    }
    char replay_error[256] = {0};
    bool replaying = navigation_ready && fetch_trace_replay_begin(
        replay_directory, replay_error, sizeof(replay_error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, fixture->document_url, 16u * 1024u,
        1000, 480, NULL, NULL, true);
    lxb_dom_node_t *probe = loaded ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "probe")
        : NULL;
    ComputedStyle style = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&navigation.page.stylesheet, probe, NULL);
    bool ok = loaded && probe != NULL
        && strcmp(navigation.page.referrer_policy, expected_policy) == 0
        && navigation.page.external_stylesheets.attempted == 1
        && navigation.page.external_stylesheets.loaded == 1
        && navigation.page.external_stylesheets.failed == 0
        && style.color == 0x123456;
    if (!ok) {
        fprintf(stderr,
                "document referrer case replay=%d loaded=%d policy=\"%s\" "
                "expected=\"%s\" css=%zu/%zu/%zu probe=%p color=%06x "
                "replay-error=\"%s\" navigation-error=\"%s\"\n",
                replaying ? 1 : 0, loaded ? 1 : 0,
                navigation.page.referrer_policy, expected_policy,
                navigation.page.external_stylesheets.attempted,
                navigation.page.external_stylesheets.loaded,
                navigation.page.external_stylesheets.failed,
                (void *) probe, style.color, replay_error,
                navigation.last_error);
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    if (replay_written) {
        remove_referrer_navigation_replay(replay_directory);
    }
    return ok && clean;
}

static bool test_valid_meta_referrer_overrides_response_policy(void)
{
    static const ReferrerNavigationReplay fixture = {
        .document_url = "https://document.example/page",
        .stylesheet_url = "https://assets.example/site.css",
        .html =
            "<!doctype html><head>"
            "<meta name='ReFeRrEr' content=' \tNo-ReFeRrEr \n'>"
            "<link rel=stylesheet href='https://assets.example/site.css'>"
            "</head><body><p id=probe>valid policy</p></body>",
        .css = "#probe{color:#123456}",
        .response_referrer_policy = "unsafe-url",
        .stylesheet_request_referrer = NULL,
        .stylesheet_request_policy = "no-referrer"
    };
    return run_document_referrer_policy_case(&fixture, "no-referrer");
}

static bool test_invalid_meta_referrer_preserves_response_policy(void)
{
    static const ReferrerNavigationReplay fixture = {
        .document_url = "https://document.example/page",
        .stylesheet_url = "https://assets.example/site.css",
        .html =
            "<!doctype html><head>"
            "<meta name='REFERRER' content=' Not-A-Referrer-Policy '>"
            "<link rel=stylesheet href='https://assets.example/site.css'>"
            "</head><body><p id=probe>invalid policy</p></body>",
        .css = "#probe{color:#123456}",
        .response_referrer_policy = "origin",
        .stylesheet_request_referrer = "https://document.example/",
        .stylesheet_request_policy = "origin"
    };
    return run_document_referrer_policy_case(&fixture, "origin");
}

/* Regression: a resource-driven full stylesheet rebuild must re-apply
   external CSS even when the page was loaded without recording history.
   The rebuild used to source the document URL from the current history
   entry, so a historyless load (or a trimmed history) collapsed the
   rebuilt stylesheet to the document's inline styles, losing every
   external sheet.  Observed on the PSP script EBOOT, which loads with
   record_history=false, as a wikipedia page ballooning to 12x its
   height once late script-driven rebuilds fired. */
static bool test_rebuild_without_history_keeps_external_css(void)
{
    static const ReferrerNavigationReplay fixture = {
        .document_url = "https://document.example/page",
        .stylesheet_url = "https://assets.example/site.css",
        .html =
            "<!doctype html><head>"
            "<link rel=stylesheet href='https://assets.example/site.css'>"
            "</head><body><p id=probe>historyless rebuild</p></body>",
        .css = "#probe{color:#123456}",
        .response_referrer_policy = "origin",
        .stylesheet_request_referrer = "https://document.example/",
        .stylesheet_request_policy = "origin"
    };
    char replay_directory[128] = {0};
    bool replay_written = write_referrer_navigation_replay(
        replay_directory, &fixture);
    Budget budget;
    budget_init(&budget, 20u * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = replay_written && lexbor_installed
        && browser_session_init(&browser, &budget, 256u * 1024u);
    bool navigation_ready = browser_ready
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_external_resources(
            &navigation, 4, 64u * 1024u, 32u * 1024u,
            1, 16u * 1024u, 16u * 1024u, 64u * 1024u, 1000);
    }
    char replay_error[256] = {0};
    bool replaying = navigation_ready && fetch_trace_replay_begin(
        replay_directory, replay_error, sizeof(replay_error));
    uint64_t generation = replaying ? navigation_begin(&navigation) : 0;
    /* record_history = false: the historyless load that triggers the bug. */
    bool loaded = replaying && navigation_load_url(
        &navigation, generation, fixture.document_url, 16u * 1024u,
        1000, 480, NULL, NULL, false);
    lxb_dom_node_t *probe = loaded ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "probe")
        : NULL;
    ComputedStyle before = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&navigation.page.stylesheet, probe, NULL);
    size_t rules_before = navigation.page.stylesheet.count;
    /* Force the resource-driven full rebuild.  The external record has
       already been consumed by the initial load, so a correct rebuild
       must serve the retained body rather than losing the sheet. */
    static const char user_css[] =
        "html{font-size:125%!important}";
    bool user_css_set = loaded && navigation_set_user_css(
        &navigation, user_css, sizeof(user_css) - 1);
    bool rebuilt = user_css_set && navigation_relayout(&navigation);
    probe = rebuilt ? test_find_id(
        lxb_dom_interface_node(navigation.page.document.html), "probe")
        : probe;
    ComputedStyle after = probe == NULL ? (ComputedStyle) {0}
        : style_for_node(&navigation.page.stylesheet, probe, NULL);
    lxb_dom_node_t *document_element = rebuilt
        ? lxb_dom_interface_node(navigation.page.document.html)
              ->first_child
        : NULL;
    while (document_element != NULL
           && document_element->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        document_element = document_element->next;
    }
    ComputedStyle root_after = document_element == NULL
        ? (ComputedStyle) {0}
        : style_for_node(
              &navigation.page.stylesheet, document_element, NULL);
    size_t rules_after = navigation.page.stylesheet.count;
    /* The fixture has no inline document styles, so the external rule is
       the sole source of the probe colour: if the rebuild collapsed to a
       document-only sheet, the probe would fall back to the default.  A
       preserved colour proves the external sheet was re-applied.  The raw
       rule count can legitimately differ (external application dedups),
       so it is not asserted beyond staying non-empty. */
    bool ok = loaded && user_css_set && rebuilt && probe != NULL
        && before.color == 0x123456
        && after.color == 0x123456
        && computed_style_font_size_fixed(&root_after) == 20 * 64
        && rules_after >= 1u
        && navigation.page.external_stylesheets.loaded == 1;
    if (!ok) {
        fprintf(stderr,
                "historyless rebuild loaded=%d rebuilt=%d probe=%p "
                "before-color=%06x after-color=%06x rules=%zu->%zu "
                "css-loaded=%zu replay-error=\"%s\" nav-error=\"%s\"\n",
                loaded, rebuilt, (void *) probe, before.color, after.color,
                rules_before, rules_after,
                navigation.page.external_stylesheets.loaded, replay_error,
                navigation.last_error);
    }
    if (replaying) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    if (replay_written) {
        remove_referrer_navigation_replay(replay_directory);
    }
    return ok && clean;
}

static const DrawCommand *test_image_command(
    const LayoutDocument *layout, const ImageResource *image)
{
    for (size_t i = 0; layout != NULL && i < layout->count; i++) {
        if (layout->commands[i].type == DRAW_IMAGE
            && layout->commands[i].image == image) return &layout->commands[i];
    }
    return NULL;
}

static const DrawCommand *test_text_command(
    const LayoutDocument *layout, const char *text)
{
    size_t length = strlen(text);
    for (size_t i = 0; layout != NULL && i < layout->count; i++) {
        if (layout->commands[i].type == DRAW_TEXT
            && layout->commands[i].text_length == length
            && memcmp(layout->commands[i].text, text, length) == 0) {
            return &layout->commands[i];
        }
    }
    return NULL;
}

typedef struct {
    size_t calls;
} RemoteBindingProbe;

static bool remote_binding_selector_lookup(
    void *opaque, const char *selector, size_t length,
    size_t *section_index, char tag_name[32], char identifier[129],
    char stable_key[96])
{
    RemoteBindingProbe *probe = opaque;
    probe->calls++;
    if (length != 5 || memcmp(selector, "aside", 5) != 0) return false;
    *section_index = 1;
    snprintf(tag_name, 32, "aside");
    snprintf(identifier, 129, "remote-aside");
    snprintf(stable_key, 96, "id:remote-aside");
    return true;
}

static bool remote_aside_summary(NavigationSession *navigation,
                                 const char *expected,
                                 const char *source_name)
{
    ScriptResult result = {0};
    return navigation->page.runtime != NULL
        && script_runtime_evaluate_diagnostic(
            navigation->page.runtime,
            "globalThis.pocSummary=document.querySelector('aside')"
            "?'remote-bound':'remote-unbound'",
            source_name, &result)
        && strcmp(result.summary, expected) == 0;
}

static bool test_streamed_candidate_remote_binding_isolation(void)
{
    static const char explicit_section[] =
        "<!doctype html><title>Explicit section</title><body><script>"
        "globalThis.pocSummary=document.querySelector('aside')"
        "?'explicit-bound':'explicit-unbound'"
        "</script><main>section body</main></body>";
    static const char preserved_section[] =
        "<!doctype html><title>Preserved section</title>"
        "<body><main>next section body</main></body>";
    Budget budget;
    budget_init(&budget, 24 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool navigation_ready = lexbor_installed
        && navigation_init(&navigation, &budget, 4);
    if (navigation_ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 4, 32 * 1024, 16 * 1024, 1000);
        navigation_enable_external_resources(
            &navigation, 4, 64 * 1024, 16 * 1024,
            4, 64 * 1024, 16 * 1024, 256 * 1024, 1000);
    }
    bool ok = navigation_ready && prior_page(&navigation);
    RemoteBindingProbe probe = {0};
    if (ok) {
        navigation_set_remote_selector_lookup(
            &navigation, remote_binding_selector_lookup, &probe);
        ok = navigation.remote_selector_lookup
                == remote_binding_selector_lookup
            && navigation.remote_selector_opaque == &probe
            && remote_aside_summary(
                &navigation, "remote-bound", "<incumbent-binding>")
            && probe.calls == 1;
    }
    ScriptRuntime *incumbent_runtime = navigation.page.runtime;
    lxb_html_document_t *incumbent_document = navigation.page.document.html;
    NavigationLoadQuota quota = {
        .fetch = {1, 19, 100000},
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };

    bool replay_active = ok && remote_isolation_replay_begin();
    ok = ok && replay_active;
    uint64_t generation = ok ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ok ? navigation_load_begin_url(
        &navigation, generation,
        "https://remote-isolation.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    for (size_t i = 0; load != NULL && i < 128
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    ok = ok && load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && probe.calls == 1;
    if (load != NULL) {
        navigation_load_cancel(load, "candidate binding rollback probe");
    }
    ok = ok
        && navigation_load_status(load) == NAVIGATION_LOAD_CANCELLED
        && navigation.page.runtime == incumbent_runtime
        && navigation.page.document.html == incumbent_document
        && navigation.remote_selector_lookup
                == remote_binding_selector_lookup
        && navigation.remote_selector_opaque == &probe
        && strcmp(navigation.page.document.title, "Prior") == 0
        && remote_aside_summary(
            &navigation, "remote-bound", "<incumbent-after-cancel>")
        && probe.calls == 2;
    navigation_load_destroy(load);
    if (replay_active) fetch_trace_end();

    replay_active = ok && remote_isolation_replay_begin();
    ok = ok && replay_active;
    generation = ok ? navigation_begin(&navigation) : 0;
    load = ok ? navigation_load_begin_url(
        &navigation, generation,
        "https://remote-isolation.test/document", 4096, 1000, 480,
        NULL, NULL, true) : NULL;
    for (size_t i = 0; load != NULL && i < 128
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    ok = ok && load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && probe.calls == 2
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && strcmp(navigation.page.document.title, "Remote isolation") == 0
        && strcmp(navigation.page.script_result.summary,
                  "candidate-isolated") == 0
        && navigation.images == &navigation.page.images
        && navigation.remote_selector_lookup == NULL
        && navigation.remote_selector_opaque == NULL
        && navigation.runtime_section_identity == 0
        && remote_aside_summary(
            &navigation, "remote-unbound", "<committed-stream-binding>")
        && probe.calls == 2;
    navigation_load_destroy(load);
    if (replay_active) fetch_trace_end();

    if (ok) {
        navigation_set_remote_selector_lookup(
            &navigation, remote_binding_selector_lookup, &probe);
        generation = navigation_begin(&navigation);
        ok = navigation_commit_html(
                &navigation, generation,
                "https://remote-isolation.test/document#section",
                explicit_section, sizeof(explicit_section) - 1, 480,
                NULL, NULL, true)
            && strcmp(navigation.page.script_result.summary,
                      "explicit-bound") == 0
            && navigation.remote_selector_lookup
                    == remote_binding_selector_lookup
            && navigation.remote_selector_opaque == &probe
            && probe.calls == 3;
        ScriptRuntime *section_runtime = navigation.page.runtime;
        FetchSchedulerDomain *section_domain =
            navigation.page.fetch_domain;
        generation = ok ? navigation_begin(&navigation) : 0;
        ok = ok && navigation_commit_html_preserve_runtime(
                &navigation, generation,
                "https://remote-isolation.test/document#next-section",
                preserved_section, sizeof(preserved_section) - 1, 480,
                NULL, NULL, false)
            && navigation.page.runtime == section_runtime
            && navigation.page.fetch_domain == section_domain
            && navigation.remote_selector_lookup
                    == remote_binding_selector_lookup
            && navigation.remote_selector_opaque == &probe
            && remote_aside_summary(
                &navigation, "remote-bound", "<preserved-section-binding>")
            && probe.calls == 4;
    }

    if (navigation_ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool mutate_incumbent_dom(NavigationSession *navigation)
{
    static const lxb_char_t tag[] = "aside";
    static const lxb_char_t text[] = "incumbent mutation survives";
    PocDocument *document = &navigation->page.document;
    lxb_dom_node_t *body = document_body_node(document);
    lxb_dom_element_t *element = lxb_dom_document_create_element(
        &document->html->dom_document, tag, sizeof(tag) - 1, NULL);
    lxb_dom_text_t *text_node = lxb_dom_document_create_text_node(
        &document->html->dom_document, text, sizeof(text) - 1);
    if (body == NULL || element == NULL || text_node == NULL
        || lxb_dom_node_append_child(
               lxb_dom_interface_node(element),
               lxb_dom_interface_node(text_node)) != LXB_DOM_EXCEPTION_OK
        || lxb_dom_node_append_child(
               body, lxb_dom_interface_node(element))
               != LXB_DOM_EXCEPTION_OK
        || !document_refresh(document)) return false;
    const char *body_text = document_body_text(document);
    return body_text != NULL
        && strstr(body_text, "incumbent mutation survives") != NULL;
}

static bool incumbent_is_renderable(NavigationSession *navigation)
{
    Stylesheet stylesheet = {0};
    LayoutDocument layout = {0};
    bool ok = stylesheet_build(
            &stylesheet, navigation->budget, &navigation->page.document, 480)
        && layout_build(&layout, navigation->budget,
                        &navigation->page.document, &stylesheet,
                        NULL, NULL, 480)
        && layout.count != 0;
    layout_destroy(&layout);
    stylesheet_destroy(&stylesheet);
    return ok;
}

static bool test_bounded_finalize_preserves_history_pruning(void)
{
    Budget budget;
    budget_init(&budget, 16 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    const NavigationEntry *history_entry = NULL;
    bool navigation_ready = lexbor_installed
        && navigation_init(&navigation, &budget, 2);
    bool ok = navigation_ready
        && prior_page_at(&navigation, "https://history.test/a")
        && prior_page_at(&navigation, "https://history.test/b")
        && navigation.history_count == 2
        && navigation.history_index == 1
        && navigation.history_pruned == 0;
    bool replay_active = ok && replay_begin();
    ok = ok && replay_active;
    uint64_t generation = ok ? navigation_begin(&navigation) : 0;
    NavigationLoad *load = ok ? navigation_load_begin_url(
        &navigation, generation, "https://stream.test/document", 4096,
        1000, 480, NULL, NULL, true) : NULL;
    NavigationLoadQuota quota = {
        .fetch = {1, 17, 100000},
        .maximum_parser_body_bytes = 11,
        .maximum_parser_time_us = 100000,
        .maximum_finalize_work_units = 100000,
        .maximum_finalize_time_us = 1000000
    };
    for (size_t i = 0; load != NULL && i < 128
         && navigation_load_status(load) == NAVIGATION_LOAD_PENDING; i++) {
        (void) navigation_load_pump(load, &quota);
    }
    ok = ok && load != NULL
        && navigation_load_status(load) == NAVIGATION_LOAD_READY_TO_FINISH
        && finish_bounded(load, &quota)
        && navigation_load_status(load) == NAVIGATION_LOAD_SUCCEEDED
        && navigation.history_capacity == 2
        && navigation.history_count == 2
        && navigation.history_index == 1
        /* The bounded candidate was copied before history_push evicted A.
           Promotion must preserve the incumbent-side pruning telemetry along
           with the already-preserved history pointer/count/index. */
        && navigation.history_pruned == 1
        && strcmp(navigation.history[0].url,
                  "https://history.test/b") == 0
        && strcmp(navigation.history[1].url,
                  "https://stream.test/document") == 0
        && strcmp(navigation.page.document.title, "Stream & split") == 0
        && navigation_back(&navigation, &history_entry)
        && history_entry != NULL
        && strcmp(history_entry->url,
                  "https://history.test/b") == 0
        && navigation_forward(&navigation, &history_entry)
        && history_entry != NULL
        && strcmp(history_entry->url,
                  "https://stream.test/document") == 0;
    navigation_load_destroy(load);
    if (replay_active) fetch_trace_end();
    if (navigation_ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

static bool test_relayout_damage_tracks_paint_only_fields(void)
{
    static const char html[] =
        "<!doctype html><style>"
        "#picture{display:block;width:80px;height:40px;object-fit:contain}"
        "#fit-host.cover #picture{object-fit:cover}"
        "#tracking{display:block;width:120px;height:30px;letter-spacing:0}"
        "#tracking.tracked{letter-spacing:3px}"
        "</style><body><div id=fit-host>"
        "<img id=picture width=80 height=40></div>"
        "<input id=tracking value=ABCDE></body>";
    Budget budget;
    budget_init(&budget, 12 * MIB);
    bool lexbor_installed = budget_install_lexbor(&budget);
    NavigationSession navigation = {0};
    bool navigation_ready = lexbor_installed
        && navigation_init(&navigation, &budget, 2);
    if (navigation_ready) {
        navigation_enable_scripts(&navigation, 2 * MIB, 1000);
    }
    uint64_t generation = navigation_ready
        ? navigation_begin(&navigation) : 0;
    bool ok = navigation_ready && navigation_commit_html(
        &navigation, generation, "https://damage.test/", html,
        sizeof(html) - 1, 480, NULL, NULL, true);
    lxb_dom_node_t *root = ok
        ? lxb_dom_interface_node(navigation.page.document.html) : NULL;
    lxb_dom_node_t *picture = test_find_id(root, "picture");
    static unsigned char pixels[] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 255
    };
    ImageResource image = {
        .node = picture, .pixels = pixels, .width = 2, .height = 2
    };
    ImageResources images = {.items = &image, .count = 1};
    if (ok) {
        navigation.images = &images;
        ok = picture != NULL && navigation_relayout(&navigation);
    }
    const DrawCommand *initial_text = ok
        ? test_text_command(&navigation.page.layout, "ABCDE") : NULL;
    const DrawCommand *initial_image = ok
        ? test_image_command(&navigation.page.layout, &image) : NULL;
    DrawCommand old_text = initial_text == NULL
        ? (DrawCommand) {0} : *initial_text;
    DrawCommand old_image = initial_image == NULL
        ? (DrawCommand) {0} : *initial_image;
    ok = ok && initial_text != NULL && initial_image != NULL
        && old_text.letter_spacing == 0
        && old_image.image_fit == LAYOUT_IMAGE_FIT_CONTAIN
        && script_runtime_evaluate_diagnostic(
               navigation.page.runtime,
               "document.getElementById('tracking').setAttribute("
               "'class','tracked')", "<letter-spacing-damage>",
               &navigation.page.script_result)
        && script_runtime_consume_relayout(navigation.page.runtime)
        && navigation_relayout(&navigation);
    const DrawCommand *tracked_text = ok
        ? test_text_command(&navigation.page.layout, "ABCDE") : NULL;
    ok = ok && tracked_text != NULL
        /* Button text uses its assigned content width, so paint tracking is
           the only DrawCommand field changed by this relayout. */
        && tracked_text->x == old_text.x && tracked_text->y == old_text.y
        && tracked_text->width == old_text.width
        && tracked_text->height == old_text.height
        && tracked_text->letter_spacing == 3
        && navigation.relayout_damage_valid
        && navigation.relayout_damage_left <= tracked_text->x
        && navigation.relayout_damage_top <= tracked_text->y
        && navigation.relayout_damage_right
              >= tracked_text->x + tracked_text->width
        && navigation.relayout_damage_bottom
              >= tracked_text->y + tracked_text->height;
    ok = ok && script_runtime_evaluate_diagnostic(
               navigation.page.runtime,
               "document.getElementById('fit-host').setAttribute("
               "'class','cover')", "<image-fit-damage>",
               &navigation.page.script_result)
        && script_runtime_consume_relayout(navigation.page.runtime)
        && navigation_relayout(&navigation);
    const DrawCommand *covered_image = ok
        ? test_image_command(&navigation.page.layout, &image) : NULL;
    ok = ok && covered_image != NULL
        && covered_image->x == old_image.x && covered_image->y == old_image.y
        && covered_image->width == old_image.width
        && covered_image->height == old_image.height
        && covered_image->image == old_image.image
        && covered_image->image_fit == LAYOUT_IMAGE_FIT_COVER
        && navigation.relayout_damage_valid
        && navigation.relayout_damage_left <= covered_image->x
        && navigation.relayout_damage_top <= covered_image->y
        && navigation.relayout_damage_right
              >= covered_image->x + covered_image->width
        && navigation.relayout_damage_bottom
              >= covered_image->y + covered_image->height
        && navigation.performance.full_relayouts == 0;
    if (navigation_ready) navigation_destroy(&navigation);
    bool clean = budget.current == 0
        && budget_active_allocations(&budget, NULL) == 0
        && budget_categories_reconcile(&budget);
    if (lexbor_installed) {
        clean = budget_uninstall_lexbor(&budget) && clean;
    }
    return ok && clean;
}

typedef struct {
    NavigationSession *navigation;
    size_t original_abort_calls;
    size_t replacement_abort_calls;
} CandidateHookProbe;
