static bool test_dynamic_classic_and_error(Budget *budget)
{
    const char *test_name = "dynamic classic task semantics";
    static const char classic_source[] =
        "globalThis.classicExecuted=true;"
        "globalThis.classicProbe=document.currentScript?.id==="
        "'dynamic-classic'?'current':'wrong';"
        "Promise.resolve().then(()=>{globalThis.classicAfter="
        "document.currentScript===null?'restored':'leaked';});";
    static const char error_source[] =
        "throw new Error('intentional dynamic script failure');";
    static const char append_classic[] =
        "globalThis.classicExecuted=false;"
        "const classic=document.createElement('script');"
        "classic.id='dynamic-classic';classic.src='/classic.js';"
        "classic.addEventListener('load',()=>{globalThis.pocSummary="
        "'classic:load:'+globalThis.classicProbe+':' + "
        "globalThis.classicAfter+':' + "
        "(document.currentScript===null?'load-restored':'load-leaked');});"
        "classic.addEventListener('error',()=>{globalThis.pocSummary="
        "'classic:unexpected-error';});"
        "document.head.appendChild(classic);globalThis.pocSummary="
        "'classic:append:'+(globalThis.classicExecuted?'ran':'pending');";
    static const char append_error[] =
        "const broken=document.createElement('script');"
        "broken.id='dynamic-error';broken.src='/error.js';"
        "broken.addEventListener('load',()=>{globalThis.pocSummary="
        "'error:unexpected-load';});"
        "broken.addEventListener('error',()=>{globalThis.pocSummary="
        "'error:delivered';});document.head.appendChild(broken);"
        "globalThis.pocSummary='error:append';";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://dynamic.test/classic.js",
                       classic_source));
    REQUIRE(put_script(&browser, "https://dynamic.test/error.js",
                       error_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://dynamic.test/"));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    size_t requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, append_classic, "<append-classic>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "classic:append:pending") == 0);
    REQUIRE(browser.cache_hits == hits);
    REQUIRE(navigation.page.script_result.network_requests == requests);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "classic:load:current:restored:load-restored",
        8, &advances));
    REQUIRE(advances > 0 && browser.cache_hits == hits + 1u);
    REQUIRE(navigation.page.script_result.network_requests == requests + 1u);

    hits = browser.cache_hits;
    requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, append_error, "<append-error>"));
    REQUIRE(strcmp(navigation.page.script_result.summary, "error:append") == 0);
    REQUIRE(browser.cache_hits == hits);
    REQUIRE(navigation.page.script_result.network_requests == requests);
    REQUIRE(advance_until(&navigation, "error:delivered", 8, &advances));
    REQUIRE(advances > 0 && browser.cache_hits == hits + 1u);
    REQUIRE(navigation.page.script_result.network_requests == requests + 1u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_module_dependency(Budget *budget)
{
    const char *test_name = "dynamic module task semantics";
    static const char module_source[] =
        "import {depValue} from './module-dependency.js';"
        "globalThis.moduleProbe=(document.currentScript===null?"
        "'current-null':'current-set')+':dep-'+depValue;";
    static const char dependency_source[] = "export const depValue=7;";
    static const char append_module[] =
        "const module=document.createElement('script');module.type='module';"
        "module.id='dynamic-module';module.src='/module.js';"
        "module.setAttribute('referrerpolicy',' OrIgIn ');"
        "module.addEventListener('load',()=>{globalThis.pocSummary="
        "'module:load:'+globalThis.moduleProbe;});"
        "module.addEventListener('error',()=>{globalThis.pocSummary="
        "'module:error';});document.head.appendChild(module);"
        "module.setAttribute('referrerpolicy','no-referrer');"
        "globalThis.pocSummary='module:append';";
    ScriptReplayRecord records[1] = {{
        .url = "https://module.test/module-dependency.js",
        .source = dependency_source,
        .referrer_url = "https://module.test/module.js",
        /* Enumerated referrerpolicy attributes are case-insensitive, but
           whitespace is not stripped. The invalid token snapshots the
           document's empty fallback, not the later no-referrer mutation. */
        .referrer_policy = "",
        .module = true
    }};
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_module(
        &browser, "https://module.test/module.js", module_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://module.test/"));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    size_t failures = budget->failure_count;
    REQUIRE(evaluate(&navigation, append_module, "<append-module>"));
    REQUIRE(strcmp(navigation.page.script_result.summary, "module:append") == 0);
    REQUIRE(browser.cache_hits == hits);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "module:load:current-null:dep-7", 16, &advances));
    REQUIRE(advances > 0 && advances <= 16);
    REQUIRE(browser.cache_hits == hits + 1u);
    REQUIRE(budget->failure_count == failures);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_static_module_referrer_policy(Budget *budget)
{
    const char *test_name =
        "static classic/module referrer-policy and descendant inheritance";
    static const char html[] =
        "<!doctype html><head><script src=/static-classic.js "
        "referrerpolicy='origin'></script>"
        "<script src=/static-invalid.js "
        "referrerpolicy=' OrIgIn '></script>"
        "<script type=module src=/static-root.js "
        "referrerpolicy='no-referrer'></script></head><body></body>";
    static const char classic_source[] =
        "globalThis.staticClassicPolicy=1;";
    static const char invalid_source[] =
        "globalThis.staticInvalidPolicy=2;";
    static const char root_source[] =
        "import {value} from './static-child.js';"
        "globalThis.pocSummary='static-policy:' + "
        "globalThis.staticClassicPolicy+':' + "
        "globalThis.staticInvalidPolicy+':'+value;";
    static const char child_source[] = "export const value=7;";
    ScriptReplayRecord records[4] = {
        {.url = "https://static-policy.test/static-classic.js",
         .source = classic_source, .referrer_policy = "origin",
         .validate_request_context = true},
        {.url = "https://static-policy.test/static-invalid.js",
         .source = invalid_source, .referrer_policy = "",
         .validate_request_context = true},
        {.url = "https://static-policy.test/static-root.js",
         .source = root_source,
         .referrer_policy = "no-referrer",
         .response_referrer_policy = "origin", .module = true},
        {.url = "https://static-policy.test/static-child.js",
         .source = child_source,
         .referrer_url = "https://static-policy.test/static-root.js",
         .referrer_policy = "origin", .module = true}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 4));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://static-policy.test/",
        html, 4, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "static-policy:1:2:7") == 0);
    REQUIRE(navigation.page.script_result.network_failures == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 4);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_repeated_static_module_roots_share_admission(Budget *budget)
{
    const char *test_name =
        "repeated static module roots share module-map admission";
    static const char html[] =
        "<!doctype html><head>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/shared-root.js></script>"
        "<script type=module src=/app-root.js></script>"
        "</head><body></body>";
    static const char shared_source[] =
        "globalThis.sharedRootExecutions="
        "(globalThis.sharedRootExecutions||0)+1;";
    static const char app_source[] =
        "globalThis.pocSummary='module-map:' + "
        "globalThis.sharedRootExecutions;";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_module(
        &browser, "https://module-map.test/shared-root.js", shared_source));
    REQUIRE(put_module(
        &browser, "https://module-map.test/app-root.js", app_source));
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://module-map.test/", html,
        2, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-map:1") == 0);
    REQUIRE(navigation.script_attempted == 2u);
    REQUIRE(navigation.script_loaded == 7u);
    REQUIRE(navigation.script_module_map_hits == 5u);
    REQUIRE(navigation.script_skipped_quota == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_deferred_bootstrap_keeps_bounded_admission(Budget *budget)
{
    const char *test_name =
        "deferred bootstrap keeps bounded admission ahead of async widgets";
    static const char html[] =
        "<!doctype html><head>"
        "<style>app-shell:not(:defined){visibility:hidden;opacity:0}</style>"
        "<script async src=/widget-a.js></script>"
        "<script async src=/widget-b.js></script>"
        "<script defer fetchpriority=high src=/app.js></script>"
        "</head><body><app-shell>Readable fallback</app-shell></body>";
    static const char widget_source[] =
        "globalThis.widgetRuns=(globalThis.widgetRuns||0)+1;";
    static const char app_source[] =
        "globalThis.pocSummary='critical-bootstrap:' + "
        "globalThis.widgetRuns;";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(
        &browser, "https://critical-bootstrap.test/widget-a.js",
        widget_source));
    REQUIRE(put_script(
        &browser, "https://critical-bootstrap.test/widget-b.js",
        widget_source));
    REQUIRE(put_script(
        &browser, "https://critical-bootstrap.test/app.js", app_source));
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser,
        "https://critical-bootstrap.test/", html,
        2, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "critical-bootstrap:1") == 0);
    REQUIRE(navigation.script_attempted == 2u);
    REQUIRE(navigation.script_loaded == 2u);
    REQUIRE(navigation.script_skipped_quota == 1u);
    /* The separate style test pins the exact :not(:defined) override. This
       integration assertion pins the navigation decision that activates it
       after bounded author-script admission leaves one root unresolved. */
    REQUIRE(navigation.page.stylesheet.static_custom_element_fallback);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_classic_ordering(Budget *budget)
{
    const char *test_name = "ordered dynamic classic scripts";
    static const char first_source[] =
        "globalThis.dynamicOrder.push('execute:first');";
    static const char second_source[] =
        "globalThis.dynamicOrder.push('execute:second');";
    static const char append_ordered[] =
        "globalThis.dynamicOrder=[];"
        "const first=document.createElement('script');first.async=false;"
        "first.src='/first.js';first.addEventListener('load',()=>{"
        "globalThis.dynamicOrder.push('load:first');});"
        "const second=document.createElement('script');second.async=false;"
        "second.src='/second.js';second.addEventListener('load',()=>{"
        "globalThis.dynamicOrder.push('load:second');globalThis.pocSummary="
        "'order:'+globalThis.dynamicOrder.join(',');});"
        "document.head.appendChild(first);document.head.appendChild(second);"
        "globalThis.pocSummary='order:append:'+globalThis.dynamicOrder.length;";
    static const char expected[] =
        "order:execute:first,load:first,execute:second,load:second";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://order.test/first.js", first_source));
    REQUIRE(put_script(&browser, "https://order.test/second.js", second_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://order.test/"));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    REQUIRE(evaluate(&navigation, append_ordered, "<append-ordered>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "order:append:0") == 0);
    REQUIRE(browser.cache_hits == hits);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, expected, 8, &advances));
    REQUIRE(advances >= 2 && advances <= 8);
    REQUIRE(browser.cache_hits == hits + 2u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_module_ordering(Budget *budget)
{
    const char *test_name = "dynamic module force-async and ordering";
    static const char ordered_first_source[] =
        "globalThis.moduleOrder.push('execute:ordered-first');";
    static const char ordered_second_source[] =
        "globalThis.moduleOrder.push('execute:ordered-second');";
    static const char default_slow_source[] =
        "globalThis.moduleReadyOrder.push('execute:slow');";
    static const char default_fast_source[] =
        "globalThis.moduleReadyOrder.push('execute:fast');";
    static const char append_ordered[] =
        "globalThis.moduleOrder=[];"
        "const first=document.createElement('script');first.type='module';"
        "first.async=false;first.src='/ordered-first.js';"
        "first.addEventListener('load',()=>moduleOrder.push("
        "'load:ordered-first'));"
        "const second=document.createElement('script');second.type='module';"
        "second.async=false;second.src='/ordered-second.js';"
        "second.addEventListener('load',()=>{moduleOrder.push("
        "'load:ordered-second');globalThis.pocSummary='module-order:' + "
        "moduleOrder.join(',');});"
        "document.head.appendChild(first);document.head.appendChild(second);"
        "globalThis.pocSummary='module-order:append';";
    static const char ordered_expected[] =
        "module-order:execute:ordered-first,load:ordered-first,"
        "execute:ordered-second,load:ordered-second";
    static const char append_default[] =
        "globalThis.moduleReadyOrder=[];"
        "const slow=document.createElement('script');slow.type='module';"
        "slow.src='/default-slow.js';slow.addEventListener('load',()=>{"
        "moduleReadyOrder.push('load:slow');globalThis.pocSummary="
        "'module-ready:'+moduleReadyOrder.join(',');});"
        "const fast=document.createElement('script');fast.type='module';"
        "fast.src='/default-fast.js';fast.addEventListener('load',()=>{"
        "moduleReadyOrder.push('load:fast');globalThis.pocSummary="
        "'module-fast:'+moduleReadyOrder.join(',');});"
        "document.head.appendChild(slow);document.head.appendChild(fast);"
        "globalThis.pocSummary='module-ready:append';";
    static const char default_fast_expected[] =
        "module-fast:execute:fast,load:fast";
    static const char default_expected[] =
        "module-ready:execute:fast,load:fast,execute:slow,load:slow";
    ScriptReplayRecord records[] = {
        {.url = "https://module-order.test/ordered-first.js",
         .source = ordered_first_source, .delay_pumps = 4,
         .external_cancel = false, .module = true},
        {.url = "https://module-order.test/default-slow.js",
         .source = default_slow_source, .delay_pumps = 4,
         .external_cancel = false, .module = true}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 2));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_module(&browser,
                       "https://module-order.test/ordered-second.js",
                       ordered_second_source));
    REQUIRE(put_module(&browser,
                       "https://module-order.test/default-fast.js",
                       default_fast_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://module-order.test/"));
    navigation_ready = true;

    REQUIRE(evaluate(&navigation, append_ordered, "<append-module-ordered>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-order:append") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-order:append") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, ordered_expected, 12, &advances));
    REQUIRE(advances > 0);

    REQUIRE(evaluate(&navigation, append_default, "<append-module-default>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-ready:append") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   default_fast_expected) == 0);
    REQUIRE(advance_until(&navigation, default_expected, 12, &advances));
    REQUIRE(advances > 0);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 2);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_default_dynamic_as_ready(Budget *budget)
{
    const char *test_name = "default dynamic scripts execute as ready";
    static const char slow_source[] =
        "globalThis.dynamicReadyOrder.push('execute:slow');";
    static const char fast_source[] =
        "globalThis.dynamicReadyOrder.push('execute:fast');";
    static const char append[] =
        "globalThis.dynamicReadyOrder=[];"
        "const slow=document.createElement('script');slow.src='/slow.js';"
        "slow.addEventListener('load',()=>{dynamicReadyOrder.push("
        "'load:slow');globalThis.pocSummary='ready-order:' + "
        "dynamicReadyOrder.join(',');});"
        "const fast=document.createElement('script');fast.src='/fast.js';"
        "fast.addEventListener('load',()=>{dynamicReadyOrder.push("
        "'load:fast');globalThis.pocSummary='fast-first:' + "
        "dynamicReadyOrder.join(',');});"
        "document.head.appendChild(slow);document.head.appendChild(fast);"
        "globalThis.pocSummary='ready-order:append';";
    ScriptReplayRecord records[] = {{
        .url = "https://ready.test/slow.js", .source = slow_source,
        .delay_pumps = 4, .external_cancel = false
    }};
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://ready.test/fast.js",
                       fast_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://ready.test/"));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append, "<append-as-ready>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "ready-order:append") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "fast-first:execute:fast,load:fast") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation,
        "ready-order:execute:fast,load:fast,execute:slow,load:slow",
        12, &advances));
    REQUIRE(advances > 0);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_script_cancellation(Budget *budget)
{
    const char *test_name = "dynamic script cancellation";
    static const char cancelled_source[] =
        "globalThis.cancelledScriptExecuted=true;";
    static const char append_cancelled[] =
        "const pending=document.createElement('script');"
        "pending.src='/cancelled.js';pending.addEventListener('load',()=>{"
        "globalThis.pocSummary='cancelled:unexpected-load';});"
        "pending.addEventListener('error',()=>{globalThis.pocSummary="
        "'cancelled:unexpected-error';});document.head.appendChild(pending);"
        "globalThis.pocSummary='cancelled:append';";
    static const char replacement[] =
        "<!doctype html><body><script>globalThis.pocSummary="
        "'replacement:ready';</script></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://cancel.test/cancelled.js",
                       cancelled_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://cancel.test/"));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    REQUIRE(evaluate(&navigation, append_cancelled, "<append-cancelled>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "cancelled:append") == 0);
    REQUIRE(browser.cache_hits == hits);
    uint64_t generation = navigation_begin(&navigation);
    REQUIRE(navigation_commit_html(
        &navigation, generation, "https://cancel.test/replacement",
        replacement, sizeof(replacement) - 1u, 480, NULL, NULL, true));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "replacement:ready") == 0);
    for (size_t step = 0; step < 4; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "replacement:ready") == 0);
    REQUIRE(browser.cache_hits == hits);

    REQUIRE(evaluate(&navigation, append_cancelled,
                     "<append-before-destroy>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "cancelled:append") == 0);
    REQUIRE(browser.cache_hits == hits);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_remove_pending_still_executes(Budget *budget)
{
    const char *test_name = "removed prepared dynamic script still executes";
    static const char detached_source[] =
        "globalThis.detachedProbe=(document.currentScript==="
        "pendingDynamic&&!pendingDynamic.isConnected)?"
        "'current-detached':'wrong';"
        "Promise.resolve().then(()=>{globalThis.detachedAfter="
        "document.currentScript===null?'restored':'leaked';});";
    static const char later_source[] =
        "globalThis.laterDynamicExecuted=true;";
    static const char append_pending[] =
        "globalThis.removedEvents=[];"
        "globalThis.pendingDynamic=document.createElement('script');"
        "pendingDynamic.src='/pending.js';"
        "pendingDynamic.addEventListener('load',()=>{removedEvents.push("
        "'load');globalThis.pocSummary='remove:completed:' + "
        "detachedProbe+':'+detachedAfter+':' + "
        "(document.currentScript===null?'load-restored':'load-leaked')+':' + "
        "(laterDynamicExecuted?'recovered':'later-missing')+':' + "
        "removedEvents.join(',');});"
        "pendingDynamic.addEventListener('error',()=>{removedEvents.push("
        "'error');globalThis.pocSummary='remove:unexpected-error';});"
        "document.head.appendChild(pendingDynamic);"
        "globalThis.pocSummary='remove:pending';";
    static const char remove_and_append[] =
        "pendingDynamic.remove();"
        "const later=document.createElement('script');later.src='/later.js';"
        "later.addEventListener('load',()=>{globalThis.laterLoaded=true;});"
        "document.head.appendChild(later);"
        "globalThis.pocSummary='remove:detached:' + "
        "(!pendingDynamic.isConnected?'yes':'no');";
    static const char expected[] =
        "remove:completed:current-detached:restored:load-restored:"
        "recovered:load";
    ScriptReplayRecord records[] = {{
        .url = "https://remove.test/pending.js",
        .source = detached_source, .delay_pumps = 4,
        .external_cancel = false
    }};
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://remove.test/later.js",
                       later_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://remove.test/"));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_pending, "<append-pending>"));
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "remove:pending") == 0);
    FetchSchedulerDomainMetrics domain_metrics = {0};
    REQUIRE(fetch_scheduler_domain_metrics(
                navigation.page.fetch_domain, &domain_metrics)
            && domain_metrics.maximum_active_slots
                   == NAVIGATION_PAGE_FETCH_SLOT_LIMIT
            && domain_metrics.active_views >= 2u
            && domain_metrics.peak_active_views >= 2u);
    REQUIRE(evaluate(&navigation, remove_and_append,
                     "<remove-and-recover>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "remove:detached:yes") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, expected, 12, &advances));
    REQUIRE(advances > 0);
    for (size_t step = 0; step < 2; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   expected) == 0);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}
