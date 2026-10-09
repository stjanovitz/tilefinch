static bool test_inert_script_types_do_not_consume_quota(Budget *budget)
{
    const char *test_name = "inert script data blocks do not consume quota";
    static const char page[] =
        "<!doctype html><head>"
        "<script type='application/ld+json'>{\"name\":\"fixture\"}</script>"
        "<script type='importmap'>{\"imports\":{}}</script>"
        "<script type='application/json'>{\"fixture\":true}</script>"
        "<script>globalThis.pocSummary='inert-types:executed';</script>"
        "</head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://inert-types.test/", page,
        1, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "inert-types:executed") == 0);
    REQUIRE(navigation.script_discovered == 1u);
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

static bool test_inline_json_assignments_avoid_compiler(Budget *budget)
{
    const char *test_name = "inline JSON assignments avoid compiler";
    static const char page[] =
        "<!doctype html><head>"
        "<script>Object.defineProperty(window,'setterData',{configurable:true,"
        "set(v){globalThis.setterSeen=v.label+':' +document.currentScript.id;}});"
        "</script>"
        "<script id='data-script'>window.bootstrapData={\"label\":\"ready\","
        "\"items\":[1,2,3]};globalThis.flags=[true,false];"
        "window.setterData={\"label\":\"set\"};"
        "var mediaDefinition=[{\"quality\":240}];</script>"
        /* Unquoted keys are valid JavaScript but not strict JSON. This must
           retain the compiler fallback rather than broadening the shortcut. */
        "<script>window.javascriptLiteral={fallback:true};</script>"
        "<script>globalThis.pocSummary=bootstrapData.label+':' +"
        "bootstrapData.items.length+':'+flags.length+':' +"
        "javascriptLiteral.fallback+':'+setterSeen+':' +"
        "mediaDefinition[0].quality;</script>"
        "</head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://inline-data.test/", page,
        8, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "ready:3:2:true:set:data-script:240") == 0);
    REQUIRE(navigation.script_inline_data_fast_paths == 1u);
    REQUIRE(navigation.script_inline_data_fast_path_bytes > 32u);
    REQUIRE(navigation.script_parser_blocking == 4u);
    REQUIRE(navigation.page.script_result.scripts_evaluated == 4u);

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

static bool test_inline_data_does_not_displace_logic_quota(Budget *budget)
{
    const char *test_name = "inline data does not displace logic quota";
    static const char page[] =
        "<!doctype html><head>"
        "<script>window.pagePayload={\"items\":[1,2,3]};</script>"
        "<script>globalThis.pocSummary='quota-data:'+pagePayload.items.length;"
        "</script></head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://data-quota.test/", page,
        1, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "quota-data:3") == 0);
    REQUIRE(navigation.script_inline_data_fast_paths == 1u);
    REQUIRE(navigation.script_inline_data_quota_exemptions == 1u);
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

static bool test_static_script_cost_profile(Budget *budget)
{
    (void) budget;
    const char *test_name = "static script cost profile";
    bool ok = true;
    char *heavy = NULL;
    static const char small[] =
        "const label='for while eval document.write';"
        "for(let i=0;i<2;i++){work(i)}";
    ScriptStaticCostProfile profile = {0};
    script_static_cost_profile(small, sizeof(small) - 1u, &profile);
    REQUIRE(profile.loops == 1u && profile.flags == 0u);

    heavy = make_padded_script_source(
        "while(ready){document.write(value);}", 400u * 1024u);
    REQUIRE(heavy != NULL);
    script_static_cost_profile(heavy, 400u * 1024u, &profile);
    REQUIRE(profile.loops == 1u);
    REQUIRE((profile.flags & SCRIPT_COST_SIGNAL_DOCUMENT_WRITE) != 0u);

cleanup:
    free(heavy);
    if (!ok) fprintf(stderr, "%s failed\n", test_name);
    return ok;
}

/* Generic authored bundles, not captured source. Exercise the actual stream
   checkpoint, both source origins and both source acquisition paths. */
static bool test_large_parser_bundles_follow_bounded_realm_policy(Budget *budget)
{
    const char *test_name = "large parser bundles use bounded realm policy";
    bool ok = true;
    for (size_t variant = 0; variant < 3u; variant++) {
        bool cdn = variant != 0u;
        bool cached = variant == 2u;
        const char *first_url = cdn ? "https://bundle-cdn.test/first.js"
            : "https://bundle-page.test/first.js";
        const char *second_url = cdn ? "https://bundle-cdn.test/second.js"
            : "https://bundle-page.test/second.js";
        char html[1024];
        snprintf(html, sizeof(html),
            "<!doctype html><body><p>Useful prefix</p>"
            "<script>globalThis.bundleOrder=['before'];</script>"
            "<script src='%s'></script><script src='%s'></script>"
            "<script>bundleOrder.push('after');globalThis.pocSummary="
            "bundleOrder.join(',')+':'+document.getElementById('written')."
            "textContent;</script><a href='/useful'>Useful link</a></body>",
            first_url, second_url);
        char *first = make_padded_script_source(
            "bundleOrder.push('first');document.write('<p id=written>wrote</p>');",
            600u * 1024u);
        char *second = make_padded_script_source(
            "if(document.getElementById('written'))bundleOrder.push('second');",
            600u * 1024u);
        ScriptReplayRecord records[3] = {
            {.url = "https://bundle-page.test/", .source = html,
             .document = true},
            {.url = first_url, .source = first},
            {.url = second_url, .source = second}
        };
        char directory[128] = {0}, error[256] = {0};
        BrowserSession browser = {0};
        NavigationSession navigation = {0};
        bool fixture_ready = false, replaying = false;
        bool browser_ready = false, navigation_ready = false;
        REQUIRE(first != NULL && second != NULL);
        REQUIRE(write_replay_fixture(directory, records, cached ? 1u : 3u));
        fixture_ready = true;
        REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
        replaying = true;
        REQUIRE(browser_session_init(&browser, budget, 2u * MIB));
        browser_ready = true;
        if (cached) {
            REQUIRE(put_script_for_document(&browser, first_url, first,
                                           "https://bundle-page.test/"));
            REQUIRE(put_script_for_document(&browser, second_url, second,
                                           "https://bundle-page.test/"));
        }
        REQUIRE(navigation_init(&navigation, budget, 2));
        navigation_ready = true;
        navigation.heavy_script_policy = SCRIPT_HEAVY_POLICY_RUN;
        navigation_attach_browser_session(&navigation, &browser);
        navigation_enable_scripts(&navigation, 8u * MIB, 1000);
        navigation_enable_document_scripts(
            &navigation, 8u, 2u * MIB, 700u * 1024u, 1000);
        navigation_set_fixed_script_memory_limits(&navigation, true);
        navigation_set_stream_delivery(&navigation, 43u, 0, 0, 0, 0, 0);
        uint64_t generation = navigation_begin(&navigation);
        REQUIRE(navigation_load_url(&navigation, generation,
            "https://bundle-page.test/", 4096u, 1000, 480,
            NULL, NULL, true));
        REQUIRE(navigation.page.runtime != NULL);
        if (strcmp(navigation.page.script_result.summary,
                   "before,first,second,after:wrote") != 0) {
            fprintf(stderr, "bundle variant=%zu summary=%s error=%s source=%s "
                "loaded=%zu failed=%zu quota=%zu pressure=%zu work=%zu\n",
                variant, navigation.page.script_result.summary,
                navigation.page.script_result.error,
                navigation.page.script_result.error_source_context,
                navigation.script_loaded, navigation.script_failed,
                navigation.script_skipped_quota, navigation.script_skipped_pressure,
                navigation.performance.parser_script_stage_work);
        }
        REQUIRE(strcmp(navigation.page.script_result.summary,
            "before,first,second,after:wrote") == 0);
        REQUIRE(navigation.performance.parser_script_stage_work > MIB);
        REQUIRE(navigation.performance.parser_script_stage_breakers == 0u);
        REQUIRE(navigation.script_cost_class_rejections == 0u);
        REQUIRE(navigation.script_skipped_quota == 0u);

cleanup:
        if (navigation_ready) navigation_destroy(&navigation);
        if (browser_ready) browser_session_destroy(&browser);
        if (replaying) fetch_trace_end();
        if (fixture_ready) remove_replay_fixture(directory, cached ? 1u : 3u);
        free(second);
        free(first);
        if (budget->current != 0) ok = false;
        if (!ok) return false;
    }
    return ok;
}

static bool test_script_realm_quota_caps_and_defaults(Budget *budget)
{
    const char *test_name = "script realm quota caps and defaults";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    PocDocument document = {0};
    ScriptRuntime *runtime = NULL;
    ScriptResult result = {0};
    ViewportContext viewport;
    bool document_ready = false, ok = true;

    REQUIRE(document_parse(
        &document, budget, empty_page, sizeof(empty_page) - 1u, 17));
    document_ready = true;
    REQUIRE(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptRuntimeOptions capped = {
        .viewport = viewport,
        .maximum_scripts = SIZE_MAX,
        .maximum_script_bytes = SIZE_MAX,
        .maximum_script_file_bytes = SIZE_MAX,
        .defer_document_scripts = true
    };
    runtime = script_runtime_create_configured(
        &document, budget, 8u * MIB, 1000,
        "https://realm-cap.test/", &capped, &result);
    REQUIRE(runtime != NULL && result.success);
    ScriptHeapGrowth growth;
    script_runtime_heap_growth(runtime, &growth);
    REQUIRE(growth.source_limit == 128u * MIB);

    /* Executable-count storage is a realm contract, not the 64-entry active
       dynamic-task queue.  These reservations carry metadata only. */
    for (size_t i = 0; i < 256u; i++) {
        ScriptQuotaReservation count = {0};
        REQUIRE(script_runtime_script_quota_reserve(
                    runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &count)
                == SCRIPT_QUOTA_RESERVE_GRANTED);
        REQUIRE(count.active && count.reserved_bytes == 0u);
        REQUIRE(script_runtime_script_quota_commit(runtime, &count, 0));
    }
    ScriptQuotaReservation count_limit = {0};
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &count_limit)
            == SCRIPT_QUOTA_RESERVE_REJECTED);
    REQUIRE(!count_limit.active);

    ScriptQuotaReservation five_mib = {0};
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                5u * MIB, &five_mib) == SCRIPT_QUOTA_RESERVE_GRANTED);
    REQUIRE(five_mib.reserved_bytes == 5u * MIB);
    REQUIRE(script_runtime_script_quota_commit(
        runtime, &five_mib, 5u * MIB));
    /* Fifteen 8 MiB metadata commits bring the total to 125 MiB. */
    for (size_t i = 0; i < 15u; i++) {
        ScriptQuotaReservation file_cap = {0};
        REQUIRE(script_runtime_script_quota_reserve(
                    runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                    9u * MIB, &file_cap) == SCRIPT_QUOTA_RESERVE_GRANTED);
        REQUIRE(file_cap.reserved_bytes == 8u * MIB);
        REQUIRE(script_runtime_script_quota_commit(
            runtime, &file_cap, 8u * MIB));
    }
    ScriptQuotaReservation tail = {0};
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                8u * MIB, &tail) == SCRIPT_QUOTA_RESERVE_GRANTED);
    REQUIRE(tail.reserved_bytes == 3u * MIB);
    REQUIRE(script_runtime_script_quota_commit(runtime, &tail, 3u * MIB));
    ScriptQuotaReservation total_limit = {0};
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                1u, &total_limit) == SCRIPT_QUOTA_RESERVE_REJECTED);
    REQUIRE(!total_limit.active);

    script_runtime_destroy(runtime);
    runtime = NULL;
    memset(&result, 0, sizeof(result));
    ScriptRuntimeOptions defaults = {
        .viewport = viewport,
        .defer_document_scripts = true
    };
    runtime = script_runtime_create_configured(
        &document, budget, 8u * MIB, 1000,
        "https://realm-default.test/", &defaults, &result);
    REQUIRE(runtime != NULL && result.success);
    script_runtime_heap_growth(runtime, &growth);
    REQUIRE(growth.source_limit == 2u * MIB);
    for (size_t i = 0; i < 64u; i++) {
        ScriptQuotaReservation count = {0};
        REQUIRE(script_runtime_script_quota_reserve(
                    runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &count)
                == SCRIPT_QUOTA_RESERVE_GRANTED);
        REQUIRE(script_runtime_script_quota_commit(runtime, &count, 0));
    }
    memset(&count_limit, 0, sizeof(count_limit));
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_NEW_EXECUTABLE, 0, &count_limit)
            == SCRIPT_QUOTA_RESERVE_REJECTED);
    for (size_t i = 0; i < 4u; i++) {
        ScriptQuotaReservation default_file = {0};
        REQUIRE(script_runtime_script_quota_reserve(
                    runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                    5u * MIB, &default_file)
                == SCRIPT_QUOTA_RESERVE_GRANTED);
        REQUIRE(default_file.reserved_bytes == 512u * 1024u);
        REQUIRE(script_runtime_script_quota_commit(
            runtime, &default_file, 512u * 1024u));
    }
    memset(&total_limit, 0, sizeof(total_limit));
    REQUIRE(script_runtime_script_quota_reserve(
                runtime, SCRIPT_QUOTA_PRECOUNTED_EXECUTABLE,
                1u, &total_limit) == SCRIPT_QUOTA_RESERVE_REJECTED);

cleanup:
    script_runtime_destroy(runtime);
    if (document_ready) document_destroy(&document);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_byte_reservation_waits_for_actual_length(
    Budget *budget)
{
    /* A network response holds no source bytes while in flight: both
       requests start at once and each is admitted against its exact length
       when it arrives, in arrival order. */
    const char *test_name =
        "dynamic responses hold no source quota while in flight";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_scripts[] =
        "globalThis.overlapEvents=[];"
        "const a=document.createElement('script');a.src='/a.js';"
        "a.addEventListener('load',()=>overlapEvents.push('load:a'));"
        "a.addEventListener('error',()=>overlapEvents.push('error:a'));"
        "const b=document.createElement('script');b.src='/b.js';"
        "b.addEventListener('load',()=>{overlapEvents.push('load:b');"
        "globalThis.pocSummary='quota-overlap:'+overlapEvents.join(',');});"
        "b.addEventListener('error',()=>{overlapEvents.push('error:b');"
        "globalThis.pocSummary='quota-overlap:'+overlapEvents.join(',');});"
        "document.head.append(a,b);"
        "globalThis.pocSummary='quota-overlap:pending';";
    char *a_source = make_padded_script_source(
        "globalThis.overlapEvents.push('execute:a');",
        128u * 1024u);
    char *b_source = make_padded_script_source(
        "globalThis.overlapEvents.push('execute:b');",
        400u * 1024u);
    ScriptReplayRecord records[2] = {
        {.url = "https://quota-overlap.test/a.js",
         .source = a_source, .delay_pumps = 4},
        {.url = "https://quota-overlap.test/b.js",
         .source = b_source, .delay_pumps = 0}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(a_source != NULL && b_source != NULL);
    REQUIRE(write_replay_fixture(directory, records, 2));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://quota-overlap.test/",
        empty_page, 2, 768u * 1024u, 512u * 1024u));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_scripts, "<append-quota-overlap>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "quota-overlap:pending") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "quota-overlap:pending") == 0);
    REQUIRE(navigation.page.script_result.dynamic_scripts_queued == 2u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 2u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);
    size_t advances = 0;
    /* Both are in flight; they run in arrival order. */
    for (size_t step = 0; step < 32u
         && strcmp(navigation.page.script_result.summary,
                   "quota-overlap:execute:a,load:a,execute:b,load:b") != 0
         && strcmp(navigation.page.script_result.summary,
                   "quota-overlap:execute:b,load:b,execute:a,load:a") != 0;
         step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
        advances = step + 1u;
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "quota-overlap:execute:a,load:a,execute:b,load:b") == 0
            || strcmp(navigation.page.script_result.summary,
                      "quota-overlap:execute:b,load:b,execute:a,load:a")
                   == 0);
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 2u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 2u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);
    REQUIRE(navigation.page.script_result.network_failures == 0u);
    REQUIRE(navigation.page.script_result.dynamic_script_bytes
            == 528u * 1024u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 2);
    free(b_source);
    free(a_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_dynamic_scheduler_backpressure_retries(Budget *budget)
{
    const char *test_name =
        "dynamic scheduler backpressure retries without script errors";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_scripts[] =
        "globalThis.schedulerEvents=[];"
        "for(const name of ['a','b','c','d','e','f']){"
        "const script=document.createElement('script');"
        "script.src='/'+name+'.js';"
        "const report=()=>{const loads=schedulerEvents.filter("
        "e=>e.startsWith('load:')).length;const errors=schedulerEvents"
        ".filter(e=>e.startsWith('error:')).length;"
        "globalThis.pocSummary='scheduler:loads='+loads+',errors='+errors;};"
        "script.addEventListener('load',()=>{"
        "schedulerEvents.push('load:'+name);report();});"
        "script.addEventListener('error',()=>{"
        "schedulerEvents.push('error:'+name);report();});"
        "document.head.appendChild(script);}"
        "globalThis.pocSummary='scheduler:pending';";
    static const char a_source[] =
        "globalThis.schedulerEvents.push('execute:a');";
    static const char b_source[] =
        "globalThis.schedulerEvents.push('execute:b');";
    static const char c_source[] =
        "globalThis.schedulerEvents.push('execute:c');";
    static const char d_source[] =
        "globalThis.schedulerEvents.push('execute:d');";
    static const char e_source[] =
        "globalThis.schedulerEvents.push('execute:e');";
    static const char f_source[] =
        "globalThis.schedulerEvents.push('execute:f');";
    /* Six scripts against SCRIPT_DYNAMIC_INFLIGHT_REQUESTS (4) in flight:
       the first four start, the rest wait for a completion and then start
       without an error event. The delayed first response holds its slot
       but not the others'. */
    ScriptReplayRecord records[6] = {
        {.url = "https://scheduler-backpressure.test/a.js",
         .source = a_source, .delay_pumps = 6},
        {.url = "https://scheduler-backpressure.test/b.js",
         .source = b_source},
        {.url = "https://scheduler-backpressure.test/c.js",
         .source = c_source},
        {.url = "https://scheduler-backpressure.test/d.js",
         .source = d_source},
        {.url = "https://scheduler-backpressure.test/e.js",
         .source = e_source},
        {.url = "https://scheduler-backpressure.test/f.js",
         .source = f_source}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 6));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser,
        "https://scheduler-backpressure.test/", empty_page,
        6, 2u * MIB, 512u * 1024u));
    navigation_ready = true;
    REQUIRE(evaluate(
        &navigation, append_scripts, "<append-scheduler-backpressure>"));
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(navigation.page.script_result.dynamic_scripts_queued == 6u);
    if (navigation.page.script_result.dynamic_scripts_started < 4u
        || navigation.page.script_result.dynamic_scripts_started == 6u) {
        fprintf(stderr,
                "%s started=%zu completed=%zu failed=%zu pending=%zu\n",
                test_name,
                navigation.page.script_result.dynamic_scripts_started,
                navigation.page.script_result.dynamic_scripts_completed,
                navigation.page.script_result.dynamic_scripts_failed,
                navigation.page.script_result.pending_tasks);
    }
    /* At most SCRIPT_DYNAMIC_INFLIGHT_REQUESTS are in flight; a request
       that completed in this turn frees a slot for the next. */
    REQUIRE(navigation.page.script_result.dynamic_scripts_started
                >= SCRIPT_DYNAMIC_INFLIGHT_REQUESTS
            && navigation.page.script_result.dynamic_scripts_started < 6u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "scheduler:loads=6,errors=0", 48, &advances));
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 6u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 6u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    REQUIRE(navigation.page.script_result.network_failures == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 6);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

/* m.vk.ru: a tracker host answered nothing for over five seconds while the
   app's own chunks waited behind it. With the PSP app's source limits, a
   stalled dynamic response must not hold the source quota or the response
   bound the scripts after it need. */
static bool test_stalled_dynamic_script_does_not_block_later_ones(
    Budget *budget)
{
    const char *test_name = "a stalled dynamic script does not block others";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_scripts[] =
        "globalThis.stallEvents=[];"
        "for(const name of ['tracker','app','vendor']){"
        "const script=document.createElement('script');"
        "script.src='/'+name+'.js';"
        "script.addEventListener('load',()=>{"
        "stallEvents.push('load:'+name);"
        "globalThis.pocSummary='stall:'+stallEvents.join(',');});"
        "script.addEventListener('error',()=>{"
        "stallEvents.push('error:'+name);"
        "globalThis.pocSummary='stall:'+stallEvents.join(',');});"
        "document.head.appendChild(script);}"
        "globalThis.pocSummary='stall:pending';";
    char *app_source = make_padded_script_source(
        "globalThis.stallEvents.push('execute:app');", 300u * 1024u);
    char *vendor_source = make_padded_script_source(
        "globalThis.stallEvents.push('execute:vendor');", 400u * 1024u);
    static const char tracker_source[] =
        "globalThis.stallEvents.push('execute:tracker');";
    ScriptReplayRecord records[3] = {
        {.url = "https://stalled-tracker.test/tracker.js",
         .source = tracker_source, .delay_pumps = 40},
        {.url = "https://stalled-tracker.test/app.js",
         .source = app_source},
        {.url = "https://stalled-tracker.test/vendor.js",
         .source = vendor_source}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(app_source != NULL && vendor_source != NULL);
    REQUIRE(write_replay_fixture(directory, records, 3));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://stalled-tracker.test/",
        empty_page, 8, 2u * MIB, 512u * 1024u));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_scripts, "<append-stall>"));
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 3u);
    size_t advances = 0;
    /* Both later scripts run long before the tracker answers. */
    REQUIRE(advance_until(
        &navigation,
        "stall:execute:app,load:app,execute:vendor,load:vendor", 12,
        &advances));
    REQUIRE(advance_until(
        &navigation,
        "stall:execute:app,load:app,execute:vendor,load:vendor,"
        "execute:tracker,load:tracker", 64, &advances));
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 3);
    free(vendor_source);
    free(app_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

/* A script admitted on the planned (median) compile working set whose
   compile needs far more is stopped by the realm's heap limit and refused
   like a script that never fit: an error event on its element, no realm
   retirement, and the next script runs. A literal table of small objects
   compiles at about eleven times its source (10.8 MB for 949 KB on the
   host), against a plan of about two. */
static bool test_compile_over_heap_is_refused_with_realm_intact(
    Budget *budget)
{
    const char *test_name = "a compile over the heap is refused alone";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_scripts[] =
        "globalThis.heavyEvents=[];"
        "for(const name of ['table','after']){"
        "const script=document.createElement('script');"
        "script.src='/'+name+'.js';script.async=false;"
        "script.addEventListener('load',()=>{"
        "heavyEvents.push('load:'+name);"
        "globalThis.pocSummary='heavy:'+heavyEvents.join(',');});"
        "script.addEventListener('error',()=>{"
        "heavyEvents.push('error:'+name);"
        "globalThis.pocSummary='heavy:'+heavyEvents.join(',');});"
        "document.head.appendChild(script);}"
        "globalThis.pocSummary='heavy:pending';";
    static const char after_source[] =
        "globalThis.heavyEvents.push('execute:after:'"
        "+(typeof globalThis.table));";
    size_t entries = 60000u, capacity = entries * 24u + 64u, used = 0;
    char *table_source = malloc(capacity);
    if (table_source != NULL) {
        used = (size_t) snprintf(table_source, capacity,
                                 "globalThis.table=[");
        for (size_t i = 0; i < entries && used < capacity; i++) {
            used += (size_t) snprintf(table_source + used, capacity - used,
                                      "%s{a:%zu,b:\"x\"}",
                                      i == 0 ? "" : ",", i);
        }
        if (used < capacity)
            used += (size_t) snprintf(table_source + used, capacity - used,
                                      "];");
    }
    ScriptReplayRecord records[2] = {
        {.url = "https://heavy-compile.test/table.js",
         .source = table_source},
        {.url = "https://heavy-compile.test/after.js",
         .source = after_source}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(table_source != NULL && used > 900u * 1024u && used < capacity);
    REQUIRE(write_replay_fixture(directory, records, 2));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://heavy-compile.test/",
        empty_page, 8, 16u * MIB, 4u * MIB));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_scripts, "<append-heavy>"));
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation,
        "heavy:error:table,execute:after:undefined,load:after", 32,
        &advances));
    REQUIRE(navigation.page.runtime != NULL);
    REQUIRE(!navigation.page.script_degradation_observed);
    REQUIRE(navigation.page.script_result.host_compile_memory_abandoned
            == 1u);
    REQUIRE(navigation.page.script_result.host_compile_memory_refusals
            == 1u);
    REQUIRE(strstr(navigation.page.script_result.error,
                   "out of memory") == NULL);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 2);
    free(table_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

/* The embedder's per-site answer for the gate page's site; any other URL
   (a wrong one passed to the resolver) refuses, which the cases notice. */
static ScriptHeavyPolicy heavy_gate_site_policy(void *opaque, const char *url)
{
    return strcmp(url, "https://heavy-gate.test/") == 0
        ? *(const ScriptHeavyPolicy *) opaque : SCRIPT_HEAVY_POLICY_REFUSE;
}

/* Heavy pages: on an empty page under the ASK policy, a big dynamic
   script that makes the page's script weight heavy waits, complete but not
   run, until the answer; a small script keeps running meanwhile. Run
   executes it; refuse fails it with an error event. RUN never holds.
   With `stale_default`, the session's default is another site's choice and
   `policy` comes from the per-site resolver: the realm must start with the
   resolver's answer, not the default, before any poll could correct it. */
static bool run_heavy_gate_case(Budget *budget, ScriptHeavyPolicy policy,
                                int answer, const char *expected,
                                const ScriptHeavyPolicy *stale_default)
{
    const char *test_name = "heavy page gate";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_scripts[] =
        "globalThis.heavyGate=[];"
        "for(const name of ['app','small']){"
        "const script=document.createElement('script');"
        "script.src='/'+name+'.js';"
        "script.addEventListener('load',()=>{heavyGate.push('load:'+name);"
        "globalThis.pocSummary='gate:'+heavyGate.join(',');});"
        "script.addEventListener('error',()=>{heavyGate.push('error:'+name);"
        "globalThis.pocSummary='gate:'+heavyGate.join(',');});"
        "document.head.appendChild(script);}"
        "globalThis.pocSummary='gate:pending';";
    char *app_source = make_padded_script_source(
        "globalThis.heavyGate.push('execute:app');", 2u * MIB);
    static const char small_source[] =
        "globalThis.heavyGate.push('execute:small');";
    ScriptReplayRecord records[2] = {
        {.url = "https://heavy-gate.test/app.js", .source = app_source},
        {.url = "https://heavy-gate.test/small.js", .source = small_source,
         .delay_pumps = 2}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(app_source != NULL);
    REQUIRE(write_replay_fixture(directory, records, 2));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page_heavy(
        &navigation, budget, &browser, "https://heavy-gate.test/",
        empty_page, 8, 16u * MIB, 4u * MIB,
        stale_default != NULL ? *stale_default : policy,
        stale_default != NULL ? heavy_gate_site_policy : NULL, &policy));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_scripts, "<append-gate>"));
    ScriptHeavyState state;
    if (policy == SCRIPT_HEAVY_POLICY_ASK) {
        size_t advances = 0;
        REQUIRE(advance_until(&navigation,
                              "gate:execute:small,load:small", 24,
                              &advances));
        /* The big body takes several transport turns to arrive. */
        for (size_t i = 0; i < 64u; i++) {
            REQUIRE(script_runtime_heavy_state(navigation.page.runtime,
                                               &state));
            if (state.waiting) break;
            REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
        }
        REQUIRE(strcmp(navigation.page.script_result.summary,
                       "gate:execute:small,load:small") == 0);
        REQUIRE(script_runtime_heavy_state(navigation.page.runtime, &state));
        if (!state.waiting) {
            fprintf(stderr, "heavy gate state class=%d waiting=%zu/%zu "
                    "script=%zu estimate=%u text=%zu answered=%d "
                    "summary=%s\n", (int) state.page_class,
                    state.waiting_scripts, state.waiting_bytes,
                    state.script_bytes, (unsigned) state.estimate_ms,
                    state.visible_text_bytes, (int) state.answered,
                    navigation.page.script_result.summary);
        }
        REQUIRE(state.waiting && state.waiting_scripts == 1u
                && state.waiting_bytes == 2u * MIB);
        REQUIRE(state.page_class == SCRIPT_HEAVY_CLASS_SHELL
                || state.page_class == SCRIPT_HEAVY_CLASS_OVER);
        REQUIRE(state.visible_text_bytes == 0u);
        REQUIRE(state.estimate_ms >= SCRIPT_HEAVY_START_MS);
        REQUIRE(script_runtime_answer_heavy(navigation.page.runtime,
                                            answer != 0));
    }
    REQUIRE(advance_until(&navigation, expected, 96, NULL));
    REQUIRE(script_runtime_heavy_state(navigation.page.runtime, &state));
    REQUIRE(!state.waiting && state.waiting_scripts == 0u);
    REQUIRE(state.refused_scripts
            == (policy == SCRIPT_HEAVY_POLICY_ASK && answer == 0 ? 1u : 0u));

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 2);
    free(app_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

/* A script refused as too large for the page (here over the realm's
   per-script ceiling) makes the page over the best case, so the PSP app
   explains it instead of leaving a silent empty shell (Bluesky's app bundle
   is over the 4 MiB ceiling). */
static bool test_oversized_script_marks_page_over(Budget *budget)
{
    const char *test_name = "an oversized script marks the page over";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_script[] =
        "const s=document.createElement('script');s.src='/app.js';"
        "s.addEventListener('error',()=>{globalThis.pocSummary='big:error'});"
        "s.addEventListener('load',()=>{globalThis.pocSummary='big:load'});"
        "document.head.appendChild(s);globalThis.pocSummary='big:pending';";
    char *app_source = make_padded_script_source(
        "globalThis.ranBig=true;", 200u * 1024u);
    ScriptReplayRecord record = {
        .url = "https://oversized.test/app.js", .source = app_source
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(app_source != NULL);
    REQUIRE(write_replay_fixture(directory, &record, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://oversized.test/",
        empty_page, 4, 2u * MIB, 64u * 1024u));
    navigation_ready = true;
    ScriptHeavyState state;
    REQUIRE(script_runtime_heavy_state(navigation.page.runtime, &state)
            && state.page_class == SCRIPT_HEAVY_CLASS_NONE);
    REQUIRE(evaluate(&navigation, append_script, "<append-oversized>"));
    REQUIRE(advance_until(&navigation, "big:error", 64, NULL));
    REQUIRE(script_runtime_heavy_state(navigation.page.runtime, &state));
    REQUIRE(state.page_class == SCRIPT_HEAVY_CLASS_OVER
            && state.oversized_scripts == 1u
            && state.oversized_bytes == 200u * 1024u && !state.waiting);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    free(app_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_heavy_page_gate(Budget *budget)
{
    /* The previous site was always allowed (RUN), or the setting is Basic
       view (REFUSE) while this site is always allowed. */
    const ScriptHeavyPolicy previous_site_run = SCRIPT_HEAVY_POLICY_RUN;
    const ScriptHeavyPolicy setting_refuse = SCRIPT_HEAVY_POLICY_REFUSE;
    return run_heavy_gate_case(
               budget, SCRIPT_HEAVY_POLICY_ASK, 1,
               "gate:execute:small,load:small,execute:app,load:app", NULL)
        && run_heavy_gate_case(
               budget, SCRIPT_HEAVY_POLICY_ASK, 0,
               "gate:execute:small,load:small,error:app", NULL)
        && run_heavy_gate_case(
               budget, SCRIPT_HEAVY_POLICY_RUN, 1,
               "gate:execute:small,load:small,execute:app,load:app", NULL)
        && run_heavy_gate_case(
               budget, SCRIPT_HEAVY_POLICY_ASK, 0,
               "gate:execute:small,load:small,error:app",
               &previous_site_run)
        && run_heavy_gate_case(
               budget, SCRIPT_HEAVY_POLICY_RUN, 1,
               "gate:execute:small,load:small,execute:app,load:app",
               &setting_refuse);
}

static char *make_resource_loader_envelope(size_t padding_per_statement)
{
    static const char first_prefix[] =
        "mw.loader.impl(function(){return['one',function(){"
        "segmentEvents.push('one');Promise.resolve().then(()=>{"
        "segmentEvents.push('microtask');globalThis.pocSummary="
        "'segments:'+segmentEvents.join(',');});globalThis.pocSummary="
        "'segments:'+segmentEvents.join(',');/*";
    static const char first_suffix[] = "*/}];});\n";
    static const char second_prefix[] =
        "mw.loader.impl(function(){return['two',function(){"
        "segmentEvents.push('two');globalThis.pocSummary="
        "'segments:'+segmentEvents.join(',');/*";
    static const char second_suffix[] = "*/}];});";
    size_t length = sizeof(first_prefix) - 1 + padding_per_statement
        + sizeof(first_suffix) - 1 + sizeof(second_prefix) - 1
        + padding_per_statement + sizeof(second_suffix);
    char *source = malloc(length);
    if (source == NULL) return NULL;
    size_t at = 0;
#define APPEND_RESOURCE_LOADER_LITERAL(value) do {                         \
    memcpy(source + at, (value), sizeof(value) - 1);                       \
    at += sizeof(value) - 1;                                               \
} while (0)
    APPEND_RESOURCE_LOADER_LITERAL(first_prefix);
    memset(source + at, 'a', padding_per_statement);
    at += padding_per_statement;
    APPEND_RESOURCE_LOADER_LITERAL(first_suffix);
    APPEND_RESOURCE_LOADER_LITERAL(second_prefix);
    memset(source + at, 'b', padding_per_statement);
    at += padding_per_statement;
    APPEND_RESOURCE_LOADER_LITERAL(second_suffix);
#undef APPEND_RESOURCE_LOADER_LITERAL
    source[at] = '\0';
    return source;
}

static bool test_oversized_resource_loader_envelope_is_segmented(
    Budget *budget)
{
    const char *test_name =
        "oversized ResourceLoader envelope is bounded and segmented";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char install_loader[] =
        "globalThis.segmentEvents=[];"
        "globalThis.mw={loader:{impl:factory=>{const registration=factory();"
        "registration[1]();}}};"
        "globalThis.pocSummary='segments:ready';";
    static const char append_recognized[] =
        "const accepted=document.createElement('script');"
        "accepted.src='/recognized.js';"
        "accepted.addEventListener('load',()=>{segmentEvents.push('load');"
        "globalThis.pocSummary='segments:'+segmentEvents.join(',');});"
        "accepted.addEventListener('error',()=>{segmentEvents.push('error');"
        "globalThis.pocSummary='segments:'+segmentEvents.join(',');});"
        "document.head.appendChild(accepted);"
        "globalThis.pocSummary='segments:pending';";
    static const char append_unrecognized[] =
        "const rejected=document.createElement('script');"
        "rejected.src='/unrecognized.js';"
        "rejected.addEventListener('load',()=>{"
        "globalThis.pocSummary='unrecognized:load';});"
        "rejected.addEventListener('error',()=>{"
        "globalThis.pocSummary='unrecognized:error';});"
        "document.head.appendChild(rejected);"
        "globalThis.pocSummary='unrecognized:pending';";
    static const char append_invalid[] =
        "globalThis.segmentEvents=[];"
        "const invalid=document.createElement('script');"
        "invalid.src='/invalid.js';"
        "invalid.addEventListener('load',()=>{"
        "globalThis.pocSummary='invalid:load';});"
        "invalid.addEventListener('error',()=>{"
        "globalThis.pocSummary='invalid:error:'+segmentEvents.join(',');});"
        "document.head.appendChild(invalid);"
        "globalThis.pocSummary='invalid:pending';";
    char *recognized = make_resource_loader_envelope(250u);
    char *invalid = make_resource_loader_envelope(250u);
    char *unrecognized = make_padded_script_source(
        "globalThis.unrecognizedExecuted=true;", 1024u);
    ScriptReplayRecord records[3] = {
        {.url = "https://resource-loader.test/recognized.js",
         .source = recognized},
        {.url = "https://resource-loader.test/invalid.js",
         .source = invalid},
        {.url = "https://resource-loader.test/unrecognized.js",
         .source = unrecognized}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(recognized != NULL && invalid != NULL && unrecognized != NULL);
    char *invalid_statement = strstr(invalid, "segmentEvents.push('two')");
    REQUIRE(invalid_statement != NULL);
    /* An error the load itself reports, a malformed number (the scan of a
       lazily compiled body rejects it too). An error that only a full
       parse finds, such as "const = 1", is reported by this function's
       first call when lazy bodies are preparsed. */
    static const char invalid_syntax[] = "x = 10in []";
    size_t invalid_statement_length = strlen("segmentEvents.push('two')");
    memcpy(invalid_statement, invalid_syntax, sizeof(invalid_syntax) - 1);
    memset(invalid_statement + sizeof(invalid_syntax) - 1, ' ',
           invalid_statement_length - (sizeof(invalid_syntax) - 1));
    REQUIRE(strlen(recognized) > 768u && strlen(unrecognized) > 768u);
    ScriptResourceLoaderPlan plan;
    REQUIRE(script_resource_loader_plan_create(
        budget, recognized, strlen(recognized), &plan));
    REQUIRE(plan.statement_count == 2u
            && plan.largest_statement_bytes <= 768u);
    script_resource_loader_plan_destroy(&plan);
    REQUIRE(write_replay_fixture(directory, records, 3));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 16u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://resource-loader.test/",
        empty_page, 4, 4096u, 768u));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, install_loader, "<install-loader>"));
    REQUIRE(evaluate(
        &navigation, append_recognized, "<append-recognized-envelope>"));
    REQUIRE(strcmp(
        navigation.page.script_result.summary, "segments:pending") == 0);

    size_t advances_to_load = 0;
    for (size_t advance = 0; advance < 16u; advance++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
        advances_to_load++;
        if (strcmp(navigation.page.script_result.summary,
                   "segments:one,two,microtask,load") == 0) break;
    }
    REQUIRE(advances_to_load >= 4u);
    if (strcmp(navigation.page.script_result.summary,
               "segments:one,two,microtask,load") != 0) {
        fprintf(stderr, "resource loader final summary=\"%s\" error=\"%s\"\n",
                navigation.page.script_result.summary,
                navigation.page.script_result.error);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "segments:one,two,microtask,load") == 0);
    REQUIRE(navigation.page.script_result.external_scripts_loaded == 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    REQUIRE(navigation.page.script_result.dynamic_script_bytes
            == strlen(recognized));
    REQUIRE(navigation.page.script_result.external_script_bytes
            == strlen(recognized));

    REQUIRE(evaluate(&navigation, append_invalid, "<append-invalid-envelope>"));
    size_t invalid_advances = 0;
    REQUIRE(advance_until(
        &navigation, "invalid:error:", 16, &invalid_advances));
    REQUIRE(invalid_advances > 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 1u);

    size_t quota_before =
        navigation.page.script_result.dynamic_scripts_quota_rejected;
    REQUIRE(evaluate(
        &navigation, append_unrecognized, "<append-unrecognized-oversize>"));
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "unrecognized:error", 16, &advances));
    REQUIRE(advances > 0u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == quota_before + 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 2u);
    REQUIRE(evaluate(
        &navigation,
        "globalThis.pocSummary=globalThis.unrecognizedExecuted?"
        "'unrecognized:executed':'unrecognized:not-executed';",
        "<verify-unrecognized>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "unrecognized:not-executed") == 0);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 3);
    free(unrecognized);
    free(invalid);
    free(recognized);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool test_small_late_script_reserve(Budget *budget)
{
    const char *test_name = "small late script scales its execution reserve";
    ScriptReplayRecord record = {
        .url = "https://late-script.test/ready.js",
        .source = "globalThis.pocSummary='late:ready';"
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;
    REQUIRE(write_replay_fixture(directory, &record, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 16384));
    browser_ready = true;
    REQUIRE(start_runtime_page(&navigation, budget, &browser, "https://late-script.test/"));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation,
        "const s=document.createElement('script');s.src='/ready.js';"
        "document.head.appendChild(s);globalThis.pocSummary='pending'", "<late-script>"));
    ScriptRuntime *runtime = navigation.page.runtime;
    JS_RunGC(runtime->runtime);
    runtime->base_memory_limit = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool)
        + 256u * 1024u;
    runtime->boot_window_active = false;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    REQUIRE(advance_until(&navigation, "late:ready", 32, NULL));
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 1
        && navigation.page.script_result.dynamic_scripts_failed == 0
        && script_runtime_heap_rejections(runtime) == 0);
cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    return ok;
}

static bool test_classic_continuation_uses_admitted_reserve(Budget *budget)
{
    const char *test_name = "continued classic script retains its admitted reserve";
    static const char source[] =
        "mw.loader.impl(function(){return ['first',function(){"
        "globalThis.retainedRegistration=new ArrayBuffer(320*1024);"
        "},{}];});"
        "mw.loader.impl(function(){return ['last',function(){"
        "globalThis.pocSummary='continued:complete';},{}];});";
    ScriptReplayRecord record = {
        .url = "https://continued-loader.test/loader.js", .source = source
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;
    REQUIRE(write_replay_fixture(directory, &record, 1));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 16384));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(&navigation, budget, &browser,
        "https://continued-loader.test/", "<!doctype html><body></body>",
        4, 4096, 160));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation,
        "globalThis.mw={loader:{impl:f=>f()[1]()}};"
        "const s=document.createElement('script');s.src='/loader.js';"
        "document.head.appendChild(s);globalThis.pocSummary='pending'",
        "<continued-loader>"));
    ScriptRuntime *runtime = navigation.page.runtime;
    JS_RunGC(runtime->runtime);
    runtime->base_memory_limit = budget_quickjs_pool_js_malloc_current(
        runtime->quickjs_pool) + 768u * 1024u;
    runtime->boot_window_active = false;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    REQUIRE(advance_until(&navigation, "continued:complete", 32, NULL));
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0);
cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 1);
    return ok;
}

static bool test_concurrent_dynamic_module_quota_progress(Budget *budget)
{
    const char *test_name =
        "module quota wait progresses native peers without reentry";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char root_a_prefix[] =
        "import {nestedValue} from './nested.js';"
        "globalThis.moduleEvents.push('execute:a:'+(nestedValue===7&&"
        "globalThis.peerRuns===0?'isolated':'reentrant'));"
        "globalThis.modulePhase='a-done';";
    static const char nested_source[] =
        "globalThis.moduleEvents.push('execute:nested:'+("
        "globalThis.peerRuns===0?'isolated':'reentrant'));"
        "export const nestedValue=7;";
    static const char root_b_source[] =
        "globalThis.peerRuns++;globalThis.moduleEvents.push("
        "'execute:b:'+globalThis.peerRuns+':'+globalThis.modulePhase);";
    static const char root_c_source[] =
        "globalThis.peerRuns++;globalThis.moduleEvents.push("
        "'execute:c:'+globalThis.peerRuns+':'+globalThis.modulePhase);";
    static const char root_d_source[] =
        "globalThis.peerRuns++;globalThis.moduleEvents.push("
        "'execute:d:'+globalThis.peerRuns+':'+globalThis.modulePhase);";
    static const char append_roots[] =
        "globalThis.moduleEvents=[];globalThis.peerRuns=0;"
        "globalThis.modulePhase='pending';"
        "const addRoot=name=>{const root=document.createElement('script');"
        "root.type='module';root.src='/root-'+name+'.js';"
        "if(name==='a')root.setAttribute('referrerpolicy','no-referrer');"
        "root.addEventListener('load',()=>{moduleEvents.push('load:'+name);"
        "globalThis.pocSummary='module-progress:'+moduleEvents.join(',');});"
        "root.addEventListener('error',()=>{moduleEvents.push('error:'+name);"
        "globalThis.pocSummary='module-progress:'+moduleEvents.join(',');});"
        "document.head.appendChild(root);};"
        "addRoot('a');addRoot('b');addRoot('c');addRoot('d');"
        "globalThis.pocSummary='module-progress:pending';";
    const size_t file_limit = 16u * 1024u;
    char *root_a_source = make_padded_script_source(
        root_a_prefix, file_limit);
    ScriptReplayRecord records[5] = {
        {.url = "https://module-progress.test/root-a.js",
         .source = root_a_source,
         .referrer_policy = "no-referrer",
         .response_referrer_policy = "origin", .module = true},
        {.url = "https://module-progress.test/root-b.js",
         .source = root_b_source, .module = true},
        {.url = "https://module-progress.test/root-c.js",
         .source = root_c_source, .module = true},
        {.url = "https://module-progress.test/root-d.js",
         .source = root_d_source, .module = true},
        {.url = "https://module-progress.test/nested.js",
         .source = nested_source,
         .referrer_url = "https://module-progress.test/root-a.js",
         .referrer_policy = "origin", .module = true}
    };
    static const char first_expected[] =
        "module-progress:execute:nested:isolated,execute:a:isolated,load:a";
    static const char expected[] =
        "module-progress:execute:nested:isolated,execute:a:isolated,load:a,"
        "execute:b:1:a-done,load:b,execute:c:2:a-done,load:c,"
        "execute:d:3:a-done,load:d";
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(root_a_source != NULL);
    REQUIRE(write_replay_fixture(directory, records, 5));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://module-progress.test/",
        empty_page, 5, 4u * file_limit, file_limit));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_roots, "<append-module-roots>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-progress:pending") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    if (strcmp(navigation.page.script_result.summary,
               first_expected) != 0) {
        fprintf(stderr,
                "%s first advance: summary=\"%s\" error=\"%s\" "
                "pending=%zu failures=%zu status=%ld url=\"%s\" "
                "type=\"%s\"\n",
                test_name, navigation.page.script_result.summary,
                navigation.page.script_result.error,
                navigation.page.script_result.pending_tasks,
                navigation.page.script_result.network_failures,
                navigation.page.script_result.last_network_status,
                navigation.page.script_result.last_network_url,
                navigation.page.script_result.last_network_content_type);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   first_expected) == 0);
    REQUIRE(navigation.page.script_result.dynamic_scripts_queued == 4u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 4u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_peak_pending == 4u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, expected, 8, &advances));
    REQUIRE(advances == 3u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 4u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 4u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);
    REQUIRE(navigation.page.script_result.network_failures == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 5);
    free(root_a_source);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

static bool run_dynamic_module_quota_case(
    Budget *budget, const char *test_name, size_t maximum_scripts,
    size_t maximum_total_bytes, bool expect_success,
    size_t expected_cache_hits)
{
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_module(&browser, "https://module-quota.test/quota-root.js",
                       quota_module_source));
    REQUIRE(put_module(&browser,
                       "https://module-quota.test/quota-shared.js",
                       quota_module_dependency_source));
    size_t maximum_file = strlen(quota_module_source)
        > strlen(quota_module_dependency_source)
        ? strlen(quota_module_source)
        : strlen(quota_module_dependency_source);
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://module-quota.test/",
        empty_page, maximum_scripts, maximum_total_bytes, maximum_file));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    REQUIRE(evaluate(&navigation, append_quota_module,
                     "<append-quota-module>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "module-quota:pending") == 0);
    size_t advances = 0;
    const char *expected = expect_success
        ? "module-quota:load:14" : "module-quota:error:0";
    REQUIRE(advance_until(&navigation, expected, 16, &advances));
    REQUIRE(advances > 0);
    /* The duplicate import must share one module-map entry, so the exact
       boundary case touches only the root and one dependency.  A count
       rejection occurs before dependency lookup; byte rejection may inspect
       the cache entry's bounded length but must not acquire/compile it. */
    REQUIRE(browser.cache_hits == hits + expected_cache_hits);
    if (expect_success) {
        REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
                == 0u);
    }

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

static bool test_cached_source_rejected_by_quota(Budget *budget)
{
    const char *test_name = "known cached source rejected by script quota";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char oversized_source[] =
        "globalThis.cachedQuotaExecuted=true;";
    static const char append_cached[] =
        "globalThis.cachedQuotaExecuted=false;"
        "const cached=document.createElement('script');"
        "cached.src='/cached-oversized.js';"
        "cached.addEventListener('load',()=>{globalThis.pocSummary="
        "'cached-quota:unexpected-load';});"
        "cached.addEventListener('error',()=>{globalThis.pocSummary="
        "'cached-quota:error:'+(cachedQuotaExecuted?'ran':'inert');});"
        "document.head.appendChild(cached);"
        "globalThis.pocSummary='cached-quota:pending';";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser,
                       "https://cached-quota.test/cached-oversized.js",
                       oversized_source));
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://cached-quota.test/",
        empty_page, 1, strlen(oversized_source) - 1u,
        strlen(oversized_source)));
    navigation_ready = true;

    REQUIRE(evaluate(&navigation, append_cached, "<append-cached-oversized>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "cached-quota:pending") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "cached-quota:error:inert", 8, &advances));
    REQUIRE(advances > 0);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            >= 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed >= 1u);
    REQUIRE(navigation.page.script_result.dynamic_script_bytes == 0u);

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

static bool test_failed_fetch_releases_byte_reservation(Budget *budget)
{
    const char *test_name = "failed fetch releases script byte reservation";
    static const char failed_source[] =
        "globalThis.failedFetch=true;";
    static const char recovery_source[] =
        "globalThis.quotaRecoveryExecuted=true;";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_failed[] =
        "const failed=document.createElement('script');"
        "failed.src='/failed.js';"
        "failed.addEventListener('load',()=>{globalThis.pocSummary="
        "'fetch-reservation:unexpected-load';});"
        "failed.addEventListener('error',()=>{globalThis.pocSummary="
        "'fetch-reservation:failed';});"
        "document.head.appendChild(failed);"
        "globalThis.pocSummary='fetch-reservation:first-pending';";
    static const char append_recovery[] =
        "globalThis.quotaRecoveryExecuted=false;"
        "const recovery=document.createElement('script');"
        "recovery.src='/recovery.js';"
        "recovery.addEventListener('load',()=>{globalThis.pocSummary="
        "'fetch-reservation:recovered:' + "
        "(quotaRecoveryExecuted?'ran':'missing');});"
        "recovery.addEventListener('error',()=>{globalThis.pocSummary="
        "'fetch-reservation:unexpected-error';});"
        "document.head.appendChild(recovery);"
        "globalThis.pocSummary='fetch-reservation:second-pending';";
    ScriptReplayRecord records[] = {
        {.url = "https://fetch-reservation.test/failed.js",
         .source = failed_source, .delay_pumps = 1,
         .external_cancel = false, .transport_failure = true},
        {.url = "https://fetch-reservation.test/recovery.js",
         .source = recovery_source, .delay_pumps = 1,
         .external_cancel = false}
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
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://fetch-reservation.test/",
        empty_page, 2, strlen(recovery_source), strlen(recovery_source)));
    navigation_ready = true;

    REQUIRE(evaluate(&navigation, append_failed, "<append-failed-fetch>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "fetch-reservation:first-pending") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "fetch-reservation:failed", 8, &advances));
    REQUIRE(evaluate(&navigation, append_recovery,
                     "<append-fetch-recovery>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "fetch-reservation:second-pending") == 0);
    REQUIRE(advance_until(
        &navigation, "fetch-reservation:recovered:ran", 8, &advances));
    REQUIRE(navigation.page.script_result.network_failures >= 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);

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

static bool run_static_dynamic_shared_quota_case(
    Budget *budget, const char *test_name, bool count_boundary)
{
    static const char first_source[] =
        "globalThis.quotaEvents.push('execute:first');";
    static const char second_source[] =
        "globalThis.quotaEvents.push('execute:second');";
    static const char bootstrap_source[] =
        "globalThis.quotaEvents=[];"
        "const first=document.createElement('script');first.src='/first.js';"
        "first.addEventListener('load',()=>quotaEvents.push('load:first'));"
        "const second=document.createElement('script');"
        "second.src='/second.js';"
        "second.addEventListener('load',()=>quotaEvents.push("
        "'unexpected-load:second'));"
        "second.addEventListener('error',()=>{quotaEvents.push("
        "'error:second');globalThis.pocSummary='shared-quota:' + "
        "quotaEvents.join(',');});"
        "document.head.appendChild(first);document.head.appendChild(second);"
        "globalThis.pocSummary='shared-quota:pending';";
    static const char page[] =
        "<!doctype html><head><script src='/bootstrap.js'></script></head>"
        "<body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://quota.test/bootstrap.js",
                       bootstrap_source));
    REQUIRE(put_script(&browser, "https://quota.test/first.js",
                       first_source));
    REQUIRE(put_script(&browser, "https://quota.test/second.js",
                       second_source));
    size_t maximum_total = strlen(bootstrap_source) + strlen(first_source);
    if (count_boundary) maximum_total += strlen(second_source);
    size_t maximum_file = strlen(bootstrap_source);
    if (strlen(first_source) > maximum_file) {
        maximum_file = strlen(first_source);
    }
    if (strlen(second_source) > maximum_file) {
        maximum_file = strlen(second_source);
    }
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://quota.test/", page,
        count_boundary ? 2 : 8, maximum_total, maximum_file));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "shared-quota:pending") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation,
        "shared-quota:execute:first,load:first,error:second",
        8, &advances));
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            >= 1u);

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
