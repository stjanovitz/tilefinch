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
        "window.setterData={\"label\":\"set\"};</script>"
        /* Unquoted keys are valid JavaScript but not strict JSON. This must
           retain the compiler fallback rather than broadening the shortcut. */
        "<script>window.javascriptLiteral={fallback:true};</script>"
        "<script>globalThis.pocSummary=bootstrapData.label+':' +"
        "bootstrapData.items.length+':'+flags.length+':' +"
        "javascriptLiteral.fallback+':'+setterSeen;</script>"
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
                   "ready:3:2:true:set:data-script") == 0);
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
    const char *test_name =
        "dynamic byte reservation waits for actual peer length";
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
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_quota_rejected
            == 0u);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation,
        "quota-overlap:execute:a,load:a,execute:b,load:b",
        32, &advances));
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
        "for(const name of ['a','b','c']){"
        "const script=document.createElement('script');"
        "script.src='/'+name+'.js';"
        "script.addEventListener('load',()=>{"
        "schedulerEvents.push('load:'+name);"
        "globalThis.pocSummary='scheduler:'+schedulerEvents.join(',');});"
        "script.addEventListener('error',()=>{"
        "schedulerEvents.push('error:'+name);"
        "globalThis.pocSummary='scheduler:'+schedulerEvents.join(',');});"
        "document.head.appendChild(script);}"
        "globalThis.pocSummary='scheduler:pending';";
    static const char a_source[] =
        "globalThis.schedulerEvents.push('execute:a');";
    static const char b_source[] =
        "globalThis.schedulerEvents.push('execute:b');";
    static const char c_source[] =
        "globalThis.schedulerEvents.push('execute:c');";
    ScriptReplayRecord records[3] = {
        {.url = "https://scheduler-backpressure.test/a.js",
         .source = a_source, .delay_pumps = 4},
        {.url = "https://scheduler-backpressure.test/b.js",
         .source = b_source},
        {.url = "https://scheduler-backpressure.test/c.js",
         .source = c_source}
    };
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(write_replay_fixture(directory, records, 3));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    REQUIRE(browser_session_init(&browser, budget, 1u * MIB));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser,
        "https://scheduler-backpressure.test/", empty_page,
        3, 48u * MIB, 8u * MIB));
    navigation_ready = true;
    REQUIRE(evaluate(
        &navigation, append_scripts, "<append-scheduler-backpressure>"));
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(navigation.page.script_result.dynamic_scripts_queued == 3u);
    if (navigation.page.script_result.dynamic_scripts_started != 1u) {
        fprintf(stderr,
                "%s started=%zu completed=%zu failed=%zu pending=%zu\n",
                test_name,
                navigation.page.script_result.dynamic_scripts_started,
                navigation.page.script_result.dynamic_scripts_completed,
                navigation.page.script_result.dynamic_scripts_failed,
                navigation.page.script_result.pending_tasks);
    }
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 1u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation,
        "scheduler:execute:a,load:a,execute:b,load:b,execute:c,load:c",
        48, &advances));
    REQUIRE(navigation.page.script_result.dynamic_scripts_started == 3u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 3u);
    REQUIRE(navigation.page.script_result.dynamic_scripts_failed == 0u);
    REQUIRE(navigation.page.script_result.network_failures == 0u);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 3);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
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
    static const char invalid_syntax[] = "const = 1";
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
