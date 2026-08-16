static bool test_cancel_discard_survives_allocation_failure(Budget *budget)
{
    const char *test_name = "allocation-free scheduler cancellation";
    static const char never_source[] = "globalThis.never=true;";
    static const char recovered_source[] = "globalThis.recovered=true;";
    ScriptReplayRecord records[] = {
        {.url = "https://discard.test/cancel.js", .source = never_source,
         .delay_pumps = 1, .external_cancel = true},
        {.url = "https://discard.test/recover.js",
         .source = recovered_source, .delay_pumps = 1,
         .external_cancel = false}
    };
    char directory[128] = {0}, error[256] = {0};
    bool fixture_ready = false, replaying = false, ok = true;
    FetchScheduler *scheduler = NULL;
    FetchResult *fetched = NULL;
    size_t baseline = budget->current;
    FetchRequest request = {
        .method = "GET", .allow_http_errors = true,
        .send_low_client_hints = true
    };

    REQUIRE(write_replay_fixture(directory, records, 2));
    fixture_ready = true;
    REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
    replaying = true;
    scheduler = fetch_scheduler_create(budget, 1, 64u * 1024u);
    REQUIRE(scheduler != NULL);
    uint64_t cancelled = fetch_scheduler_enqueue(
        scheduler, records[0].url, &request, 64u * 1024u, 5000);
    REQUIRE(cancelled != 0 && fetch_scheduler_pending(scheduler) == 1);
    size_t injected = budget->injected_failure_count;
    budget_inject_failure_after(budget, 0);
    REQUIRE(fetch_scheduler_cancel(scheduler, cancelled, "removed")
            && fetch_scheduler_discard(scheduler, cancelled)
            && fetch_scheduler_pending(scheduler) == 0
            && budget->injected_failure_count == injected);
    budget_clear_failure_injection(budget);
    uint64_t recovered = fetch_scheduler_enqueue(
        scheduler, records[1].url, &request, 64u * 1024u, 5000);
    REQUIRE(recovered != 0);
    fetched = fetch_result_create(budget);
    REQUIRE(fetched != NULL);
    bool success = false, taken = false;
    for (size_t step = 0; step < 8 && !taken; step++) {
        (void) fetch_scheduler_pump(scheduler, 1, 0);
        taken = fetch_scheduler_take(
            scheduler, recovered, &success, fetched);
    }
    REQUIRE(taken && success && fetched->length == strlen(recovered_source));

cleanup:
    budget_clear_failure_injection(budget);
    fetch_result_free(fetched);
    fetch_scheduler_destroy(scheduler);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, 2);
    if (budget->current != baseline) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current - baseline);
        ok = false;
    }
    return ok;
}

static bool test_connected_insertion_paths(Budget *budget)
{
    const char *test_name = "connected dynamic insertion paths";
    static const char insert_source[] =
        "globalThis.insertBeforeExecuted=true;";
    static const char detached_source[] =
        "globalThis.detachedExecuted=true;";
    static const char insert_script[] =
        "const inserted=document.createElement('script');"
        "inserted.src='/insert.js';inserted.addEventListener('load',()=>{"
        "globalThis.pocSummary='insert:load:' + "
        "(insertBeforeExecuted?'ran':'missing');});"
        "document.head.insertBefore(inserted,document.head.firstChild);"
        "globalThis.pocSummary='insert:pending';";
    static const char build_detached[] =
        "globalThis.detachedContainer=document.createElement('div');"
        "const nested=document.createElement('script');"
        "nested.src='/detached.js';nested.addEventListener('load',()=>{"
        "globalThis.pocSummary='detached:load:' + "
        "(detachedExecuted?'ran':'missing');});"
        "detachedContainer.appendChild(nested);"
        "globalThis.pocSummary='detached:unconnected';";
    static const char attach_detached[] =
        "document.body.appendChild(detachedContainer);"
        "globalThis.pocSummary='detached:attached';";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://insert.test/insert.js",
                       insert_source));
    REQUIRE(put_script(&browser, "https://insert.test/detached.js",
                       detached_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://insert.test/"));
    navigation_ready = true;
    size_t hits = browser.cache_hits;
    REQUIRE(evaluate(&navigation, insert_script, "<insert-before>"));
    REQUIRE(browser.cache_hits == hits);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, "insert:load:ran", 8, &advances));
    REQUIRE(browser.cache_hits == hits + 1u);

    hits = browser.cache_hits;
    size_t requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, build_detached, "<detached-build>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "detached:unconnected") == 0);
    REQUIRE(browser.cache_hits == hits
            && navigation.page.script_result.network_requests == requests);
    REQUIRE(evaluate(&navigation, attach_detached, "<detached-attach>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "detached:attached") == 0);
    REQUIRE(browser.cache_hits == hits);
    REQUIRE(advance_until(&navigation, "detached:load:ran", 8, &advances));
    REQUIRE(browser.cache_hits == hits + 1u);

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

static bool test_connected_src_mutation_and_namespaces(Budget *budget)
{
    const char *test_name = "connected script src mutation and namespaces";
    static const char property_source[] =
        "globalThis.propertyScriptExecuted=true;";
    static const char attribute_source[] =
        "globalThis.attributeScriptExecuted=true;";
    static const char html_namespace_source[] =
        "globalThis.htmlNamespaceExecuted=true;";
    static const char svg_namespace_source[] =
        "globalThis.svgNamespaceExecuted=true;";
    static const char append_without_sources[] =
        "globalThis.propertyScript=document.createElement('script');"
        "propertyScript.addEventListener('load',()=>{"
        "globalThis.propertyLoaded=true;});"
        "globalThis.attributeScript=document.createElement('script');"
        "attributeScript.addEventListener('load',()=>{globalThis.pocSummary="
        "'late-src:'+(propertyScriptExecuted&&propertyLoaded?'property-ok':"
        "'property-missing')+':' + "
        "(attributeScriptExecuted?'attribute-ok':'attribute-missing');});"
        "document.head.appendChild(propertyScript);"
        "document.head.appendChild(attributeScript);"
        "globalThis.pocSummary='late-src:inserted';";
    static const char assign_sources[] =
        "propertyScript.src='/property.js';"
        "attributeScript.setAttribute('src','/attribute.js');"
        "globalThis.pocSummary='late-src:assigned';";
    static const char append_namespaced[] =
        "globalThis.namespaceEvents=[];"
        "const htmlScript=document.createElementNS("
        "'http://www.w3.org/1999/xhtml','script');"
        "globalThis.htmlNamespaceForceAsync=htmlScript.async===true;"
        "htmlScript.src='/html-namespace.js';"
        "htmlScript.addEventListener('load',()=>namespaceEvents.push('html'));"
        "document.head.appendChild(htmlScript);"
        "const svgScript=document.createElementNS("
        "'http://www.w3.org/2000/svg','script');"
        "globalThis.svgNamespaceDefaultAsync=svgScript.async===false;"
        "svgScript.src='/svg-namespace.js';"
        "svgScript.addEventListener('load',()=>namespaceEvents.push("
        "'unexpected-svg-load'));"
        "svgScript.addEventListener('error',()=>namespaceEvents.push("
        "'unexpected-svg-error'));"
        "document.head.appendChild(svgScript);"
        "globalThis.pocSummary='namespace:appended';";
    static const char inspect_namespaced[] =
        "globalThis.pocSummary='namespace:' + "
        "(htmlNamespaceForceAsync?'html-force':'html-not-force')+':' + "
        "(htmlNamespaceExecuted?'html-ran':'html-missing')+':' + "
        "(svgNamespaceDefaultAsync?'svg-default-false':"
        "'svg-default-wrong')+':' + "
        "(globalThis.svgNamespaceExecuted?'svg-ran':'svg-inert')+':' + "
        "namespaceEvents.join(',');";
    static const char namespace_expected[] =
        "namespace:html-force:html-ran:svg-default-false:svg-inert:html";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://mutation.test/property.js",
                       property_source));
    REQUIRE(put_script(&browser, "https://mutation.test/attribute.js",
                       attribute_source));
    REQUIRE(put_script(&browser,
                       "https://mutation.test/html-namespace.js",
                       html_namespace_source));
    REQUIRE(put_script(&browser,
                       "https://mutation.test/svg-namespace.js",
                       svg_namespace_source));
    REQUIRE(start_runtime_page(&navigation, budget, &browser,
                               "https://mutation.test/"));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    size_t requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, append_without_sources,
                     "<append-without-src>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "late-src:inserted") == 0);
    REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    REQUIRE(browser.cache_hits == hits
            && navigation.page.script_result.network_requests == requests);
    REQUIRE(evaluate(&navigation, assign_sources, "<assign-connected-src>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "late-src:assigned") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "late-src:property-ok:attribute-ok", 8, &advances));
    REQUIRE(browser.cache_hits == hits + 2u);
    REQUIRE(navigation.page.script_result.network_requests == requests + 2u);

    hits = browser.cache_hits;
    requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, append_namespaced,
                     "<append-namespaced-scripts>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "namespace:appended") == 0);
    for (size_t step = 0; step < 4; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(evaluate(&navigation, inspect_namespaced,
                     "<inspect-namespaced-scripts>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   namespace_expected) == 0);
    REQUIRE(browser.cache_hits == hits + 1u);
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

static bool test_deep_tree_traversal_bounds(Budget *budget)
{
    const char *test_name = "bounded iterative DOM traversal";
    static const char deep_source[] =
        "globalThis.deepScriptExecuted=true;";
    static const char beyond_source[] =
        "globalThis.beyondTraversalScriptExecuted=true;";
    static const char build_deep[] =
        "try{globalThis.deepStage='create';globalThis.deepRoot=document.createElement('div');"
        "deepRoot.id='deep-root';let deepCursor=deepRoot;"
        "for(let index=0;index<96;index++){const child="
        "document.createElement('div');child.setAttribute('data-depth',"
        "String(index));deepCursor.appendChild(child);deepCursor=child;}"
        "globalThis.deepStage='script';const deepScript=document.createElement('script');"
        "deepScript.src='/deep.js';deepScript.addEventListener('load',()=>{"
        "globalThis.pocSummary='deep:' + "
        "(deepTraversalOk?'query-ok':'query-wrong')+':' + "
        "(deepScriptExecuted?'ran':'missing');});"
        "deepCursor.appendChild(deepScript);"
        "globalThis.deepStage='query-all';const deepMatches="
        "deepRoot.querySelectorAll('[data-depth]');"
        "globalThis.deepStage='iterator';const deepIterator=document.createNodeIterator("
        "deepRoot,NodeFilter.SHOW_ELEMENT);let iteratorCount=0,"
        "iteratorFirst=null,iteratorLast=null,iteratorNode;"
        "while((iteratorNode=deepIterator.nextNode())!==null){"
        "if(iteratorCount===0)iteratorFirst=iteratorNode;"
        "iteratorLast=iteratorNode;iteratorCount++;}"
        "globalThis.deepStage='scope';const scopeRoot=document.createElement('section'),"
        "scopeChild=document.createElement('em'),"
        "outsideRoot=document.createElement('aside');"
        "scopeRoot.appendChild(scopeChild);document.body.appendChild("
        "scopeRoot);document.body.appendChild(outsideRoot);"
        "const scopeIterator=document.createNodeIterator("
        "scopeRoot,NodeFilter.SHOW_ELEMENT);let scopeCount=0;"
        "while(scopeIterator.nextNode()!==null)scopeCount++;"
        "globalThis.deepStage='assert';globalThis.deepTraversalOk="
        "deepMatches.length===96&&"
        "deepMatches[0].getAttribute('data-depth')==='0'&&"
        "deepMatches[95].getAttribute('data-depth')==='95'&&"
        "deepRoot.querySelectorAll('#deep-root').length===0&&"
        "deepRoot.querySelector('script')===deepScript&&"
        "iteratorCount===98&&iteratorFirst===deepRoot&&"
        "iteratorLast===deepScript&&"
        "scopeCount===2;"
        "globalThis.deepStage='append';document.body.appendChild(deepRoot);"
        "globalThis.pocSummary='deep:pending:' + "
        "(deepTraversalOk?'query-ok':'query-wrong');}catch(error){"
        "globalThis.pocSummary='deep-exception:'+globalThis.deepStage+':' + "
        "String(error&&error.stack||error);}";
    /* Two half-sized containers keep each innerHTML parse inside the
       DOM_INNER_HTML_LIMIT while the combined subtree still exceeds the
       DOM_TRAVERSAL_VISIT_LIMIT (32768) with the script parked beyond
       the ceiling. */
    static const char build_beyond_ceiling[] =
        "try{globalThis.boundedStage='setup';"
        "globalThis.beyondTraversalScriptExecuted=false;"
        "globalThis.boundedRoot=document.createElement('div');"
        "boundedRoot.setAttribute('style','display:none');"
        "globalThis.boundedStage='first-half';"
        "const boundedHalfA=document.createElement('div');"
        "boundedHalfA.innerHTML='<span></span>'.repeat(16384);"
        "globalThis.boundedStage='second-half';"
        "const boundedHalfB=document.createElement('div');"
        "boundedHalfB.innerHTML='<span></span>'.repeat(16384)+"
        "'<script src=\"/beyond.js\"></script>';"
        "globalThis.boundedStage='assemble';"
        "boundedRoot.appendChild(boundedHalfA);"
        "boundedRoot.appendChild(boundedHalfB);"
        "globalThis.boundedStage='query';"
        "globalThis.boundedQueryLength="
        "boundedRoot.querySelectorAll('script').length;"
        "globalThis.boundedQueryNull="
        "boundedRoot.querySelector('script')===null;"
        "globalThis.boundedStage='connect';"
        "document.body.appendChild(boundedRoot);"
        "globalThis.pocSummary='bounded:' + boundedQueryLength+':' + "
        "(boundedQueryNull?'null':'found');}catch(error){"
        "globalThis.pocSummary='bounded-exception:'+boundedStage+':' + "
        "String(error&&error.stack||error);}";
    static const char inspect_bounded[] =
        "globalThis.pocSummary='bounded:' + boundedQueryLength+':' + "
        "(boundedQueryNull?'null':'found')+':' + "
        "(beyondTraversalScriptExecuted?'ran':'inert');";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://deep.test/deep.js", deep_source));
    REQUIRE(put_script(&browser, "https://deep.test/beyond.js",
                       beyond_source));
    /* This host-only traversal fixture constructs two detached 16K-node
       halves in one realm. The expanded standards/bootstrap surface plus
       this artificial tree failed at a 12 MiB heap and passed at 13 MiB;
       retain that measured test-only ceiling without changing production
       limits. */
    REQUIRE(start_runtime_page_with_heap_and_timeout(
        &navigation, budget, &browser, "https://deep.test/",
        13u * MIB, 5000));
    navigation_ready = true;

    size_t hits = browser.cache_hits;
    bool deep_evaluated = evaluate(
        &navigation, build_deep, "<build-deep-tree>");
    if (!deep_evaluated) {
        fprintf(stderr, "deep traversal diagnostic failed: %s\n",
                navigation.page.script_result.error);
    }
    REQUIRE(deep_evaluated);
    if (strcmp(navigation.page.script_result.summary,
               "deep:pending:query-ok") != 0) {
        fprintf(stderr, "deep traversal summary: %s\n",
                navigation.page.script_result.summary);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "deep:pending:query-ok") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, "deep:query-ok:ran", 8, &advances));
    REQUIRE(browser.cache_hits == hits + 1u);

    hits = browser.cache_hits;
    size_t requests = navigation.page.script_result.network_requests;
    size_t bounded_budget_before = budget->current;
    bool bounded_evaluated = evaluate(
        &navigation, build_beyond_ceiling,
        "<build-beyond-traversal-ceiling>");
    if (!bounded_evaluated) {
        fprintf(stderr, "bounded traversal diagnostic failed: %s\n",
                navigation.page.script_result.error);
        fprintf(stderr, "bounded traversal budget before=%zu current=%zu "
                "peak=%zu failures=%zu\n", bounded_budget_before,
                budget->current, budget->peak, budget->failure_count);
    }
    REQUIRE(bounded_evaluated);
    if (strcmp(navigation.page.script_result.summary,
               "bounded:0:null") != 0) {
        fprintf(stderr, "bounded traversal summary: %s\n",
                navigation.page.script_result.summary);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "bounded:0:null") == 0);
    for (size_t step = 0; step < 4; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(evaluate(&navigation, inspect_bounded,
                     "<inspect-traversal-ceiling>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "bounded:0:null:inert") == 0);
    REQUIRE(browser.cache_hits == hits);
    REQUIRE(navigation.page.script_result.network_requests == requests);

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

static bool test_parser_time_dynamic_insertion(Budget *budget)
{
    const char *test_name = "parser-time dynamic insertion";
    static const char parser_source[] =
        "globalThis.parserChunkExecuted=true;";
    static const char page[] =
        "<!doctype html><head><script>"
        "const chunk=document.createElement('script');"
        "chunk.src='/parser-chunk.js';"
        "chunk.addEventListener('load',()=>{globalThis.pocSummary="
        "'parser-chunk:load:'+(parserChunkExecuted?'ran':'missing');});"
        "document.head.appendChild(chunk);"
        "globalThis.pocSummary='parser-chunk:pending';"
        "</script></head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser,
                       "https://parser-dynamic.test/parser-chunk.js",
                       parser_source));
    size_t hits = browser.cache_hits;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://parser-dynamic.test/",
        page, 8, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "parser-chunk:pending") == 0);
    REQUIRE(browser.cache_hits == hits);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "parser-chunk:load:ran", 8, &advances));
    REQUIRE(advances > 0 && browser.cache_hits == hits + 1u);

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
