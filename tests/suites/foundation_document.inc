    puts("test: style/layout");
    CHECK(stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(sheet.count >= 3);
    static const char user_css[] = "body{padding:7px}.box{color:#112233}";
    CHECK(stylesheet_add_css(&sheet, user_css, strlen(user_css)));
    CHECK(sheet.count >= 5);
    LayoutDocument layout;
    CHECK(layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480));
    CHECK(layout.count > 5 && layout.height >= 272);
    CHECK(layout.link_count >= 3);
    CHECK(layout_link_at(&layout, layout.links[0].x, layout.links[0].y) != NULL);

    ScriptResult scripts;
    puts("test: JavaScript");
    bool scripts_ok = scripts_run_document(&document, &budget, 4 * MIB,
                                           1000, &scripts);
    if (!scripts_ok) fprintf(stderr, "JavaScript runtime failed: %s\n",
                             scripts.error);
    CHECK(scripts_ok);
    CHECK(scripts.success && scripts.scripts_evaluated == 1);
    CHECK(scripts.dom_content_loaded_dispatched);
    CHECK(strstr(scripts.summary, "Test:2-4-6:") != NULL);

    puts("test: sectional query keeps document roots local");
    size_t root_query_baseline = budget.current;
    static const char root_query_page[] =
        "<!doctype html><html><head class=target><title>Root</title></head>"
        "<body><a id=locale hreflang=en-US>locale</a><p>body</p></body></html>";
    PocDocument root_query_document = {0};
    ViewportContext root_query_viewport;
    TestRemoteSelectorProbe root_query_probe = {0};
    ScriptResult root_query_result = {0};
    ScriptRuntimeOptions root_query_options = {0};
    CHECK(document_parse(&root_query_document, &budget, root_query_page,
                         strlen(root_query_page), 17)
          && viewport_context_init(
                 &root_query_viewport, 480, 272, 480, 272));
    root_query_options.viewport = root_query_viewport;
    root_query_options.remote_selector_lookup =
        test_remote_selector_lookup;
    root_query_options.remote_selector_opaque = &root_query_probe;
    root_query_options.section_identity = 2;
    ScriptRuntime *root_query_runtime = script_runtime_create_configured(
        &root_query_document, &budget, 4 * MIB, 1000,
        "https://example.test/section", &root_query_options,
        &root_query_result);
    static const char root_query_probe_source[] =
        "globalThis.pocSummary=["
        "document.querySelector('html')===document.documentElement,"
        "document.querySelector('head.target')===document.head,"
        "document.querySelector('body')===document.body,"
        "document.getElementById('locale').hreflang==='en-US'"
        "].every(Boolean)?'ROOT-QUERY-PASS':'ROOT-QUERY-FAIL'";
    CHECK(root_query_runtime != NULL && root_query_result.success
          && script_runtime_evaluate_diagnostic(
                 root_query_runtime, root_query_probe_source,
                 "<root-query-probe>", &root_query_result)
          && strcmp(root_query_result.summary, "ROOT-QUERY-PASS") == 0
          && root_query_probe.calls == 0);
    script_runtime_destroy(root_query_runtime);
    document_destroy(&root_query_document);
    CHECK(budget.current == root_query_baseline);

    puts("test: handle rebinding is not reachable from page script");
    size_t rebind_gate_baseline = budget.current;
    static const char rebind_gate_page[] =
        "<!doctype html><body><p id=first>one</p><p id=second>two</p></body>";
    PocDocument rebind_gate_document = {0};
    ScriptResult rebind_gate_result = {0};
    ScriptRuntimeOptions rebind_gate_options = {0};
    CHECK(document_parse(&rebind_gate_document, &budget, rebind_gate_page,
                         strlen(rebind_gate_page), 31)
          && viewport_context_init(
                 &rebind_gate_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *rebind_gate_runtime = script_runtime_create_configured(
        &rebind_gate_document, &budget, 4 * MIB, 1000,
        "https://example.test/rebind", &rebind_gate_options,
        &rebind_gate_result);
    /* __tilefinchRebindHandle reassigns the handle a wrapper captured, which is
       the setter __handle deliberately withholds. It has to be branded and
       kept off the enumerable surface, or one call repoints any wrapper at
       any node. */
    static const char rebind_gate_source[] =
        "(()=>{const first=document.getElementById('first'),"
        "second=document.getElementById('second'),"
        "before=first.__handle;"
        "let threw=false;"
        "try{first.__tilefinchRebindHandle(second.__handle);}"
        "catch(error){threw=error instanceof DOMException;}"
        "let threwWithGuess=false;"
        "try{first.__tilefinchRebindHandle({},second.__handle);}"
        "catch(error){threwWithGuess=error instanceof DOMException;}"
        "const descriptor=Object.getOwnPropertyDescriptor("
        "first,'__tilefinchRebindHandle');"
        "globalThis.pocSummary=threw&&threwWithGuess"
        "&&first.__handle===before&&first.textContent==='one'"
        "&&descriptor&&descriptor.enumerable===false"
        "&&descriptor.writable===false&&descriptor.configurable===false"
        "&&!Object.keys(first).includes('__tilefinchRebindHandle')"
        "?'REBIND-GATED':'REBIND-FAILED:'+threw+','+threwWithGuess+','"
        "+(first.__handle===before)+','+first.textContent+','"
        "+JSON.stringify(descriptor&&{e:descriptor.enumerable,"
        "w:descriptor.writable,c:descriptor.configurable});})()";
    CHECK(rebind_gate_runtime != NULL && rebind_gate_result.success
          && script_runtime_evaluate_diagnostic(
                 rebind_gate_runtime, rebind_gate_source,
                 "<rebind-gate-probe>", &rebind_gate_result)
          && strcmp(rebind_gate_result.summary, "REBIND-GATED") == 0);
    script_runtime_destroy(rebind_gate_runtime);
    document_destroy(&rebind_gate_document);
    CHECK(budget.current == rebind_gate_baseline);

    puts("test: cyclic detached ancestry cannot hang an ancestor walk");
    size_t ancestor_cycle_baseline = budget.current;
    static const char ancestor_cycle_page[] =
        "<!doctype html><body><p id=host>text</p></body>";
    PocDocument ancestor_cycle_document = {0};
    ScriptResult ancestor_cycle_result = {0};
    ScriptRuntimeOptions ancestor_cycle_options = {0};
    CHECK(document_parse(&ancestor_cycle_document, &budget,
                         ancestor_cycle_page, strlen(ancestor_cycle_page), 29)
          && viewport_context_init(
                 &ancestor_cycle_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *ancestor_cycle_runtime = script_runtime_create_configured(
        &ancestor_cycle_document, &budget, 4 * MIB, 1000,
        "https://example.test/cycle", &ancestor_cycle_options,
        &ancestor_cycle_result);
    /* __tilefinchDetachedParent is a writable property on the wrapper and both
       parentNode and parentElement return it verbatim, so two assignments
       make each node its own ancestor.  Every ancestor walk must terminate
       on that shape; an unbounded one spins until the watchdog fires. */
    static const char ancestor_cycle_source[] =
        "(()=>{const a=document.createElement('div'),"
        "b=document.createElement('div');"
        "a.__tilefinchDetachedParent=b;b.__tilefinchDetachedParent=a;"
        "const checks=[a.parentNode===b,b.parentNode===a,"
        "document.body.contains(a)===false,"
        "document.body.contains(b)===false,"
        "a.closest('#absent')===null,"
        "a.isContentEditable===false,"
        "a.getRootNode()!=null];"
        "globalThis.pocSummary=checks.every(Boolean)"
        "?'CYCLE-BOUNDED':'CYCLE-FAILED:'+checks.join(',');})()";
    CHECK(ancestor_cycle_runtime != NULL && ancestor_cycle_result.success
          && script_runtime_evaluate_diagnostic(
                 ancestor_cycle_runtime, ancestor_cycle_source,
                 "<ancestor-cycle-probe>", &ancestor_cycle_result)
          && strcmp(ancestor_cycle_result.summary, "CYCLE-BOUNDED") == 0);
    script_runtime_destroy(ancestor_cycle_runtime);
    document_destroy(&ancestor_cycle_document);
    CHECK(budget.current == ancestor_cycle_baseline);

    puts("test: detached reclamation retires only its own journal records");
    size_t journal_scope_baseline = budget.current;
    static const char journal_scope_page[] =
        "<!doctype html><body><p id=first><span>one</span></p>"
        "<p id=second><span>two</span></p></body>";
    PocDocument journal_scope_document = {0};
    ScriptResult journal_scope_result = {0};
    ScriptRuntimeOptions journal_scope_options = {0};
    CHECK(document_parse(&journal_scope_document, &budget,
                         journal_scope_page, strlen(journal_scope_page), 23)
          && viewport_context_init(
                 &journal_scope_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *journal_scope_runtime = script_runtime_create_configured(
        &journal_scope_document, &budget, 4 * MIB, 1000,
        "https://example.test/journal", &journal_scope_options,
        &journal_scope_result);
    /* Each textContent assignment detaches the old children and reclaims the
       unretained ones, and the TEXT record is appended afterwards.  So from
       the second assignment onward the reclamation runs against a non-empty
       journal.  It may retire only the records whose target it is actually
       destroying: clearing the whole journal also raises `overflowed`, which
       the history runtime reads as a destructive tick and answers with a
       layout-reuse reset plus a full-document resource scan. */
    static const char journal_scope_source[] =
        "document.getElementById('first').textContent='alpha';"
        "document.getElementById('second').textContent='beta'";
    ScriptMutationJournal journal_scope_journal = {0};
    CHECK(journal_scope_runtime != NULL && journal_scope_result.success
          && script_runtime_evaluate_diagnostic(
                 journal_scope_runtime, journal_scope_source,
                 "<journal-scope-probe>", &journal_scope_result)
          && script_runtime_consume_mutations(
                 journal_scope_runtime, &journal_scope_journal)
          && journal_scope_journal.count == 2
          && !journal_scope_journal.overflowed
          && journal_scope_journal.records[0].kind == SCRIPT_MUTATION_TEXT
          && journal_scope_journal.records[0].node != NULL
          && journal_scope_journal.records[1].kind == SCRIPT_MUTATION_TEXT
          && journal_scope_journal.records[1].node != NULL);
    script_runtime_destroy(journal_scope_runtime);
    document_destroy(&journal_scope_document);
    CHECK(budget.current == journal_scope_baseline);

    puts("test: stable-node ordinal walks stop at the recursion bound");
    size_t ordinal_depth_baseline = budget.current;
    static const char ordinal_depth_page[] =
        "<!doctype html><html><head><title>Deep</title></head>"
        "<body></body></html>";
    PocDocument ordinal_depth_document = {0};
    ScriptResult ordinal_depth_result = {0};
    ScriptRuntimeOptions ordinal_depth_options = {0};
    CHECK(document_parse(&ordinal_depth_document, &budget,
                         ordinal_depth_page, strlen(ordinal_depth_page), 19)
          && viewport_context_init(
                 &ordinal_depth_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *ordinal_depth_runtime = script_runtime_create_configured(
        &ordinal_depth_document, &budget, 4 * MIB, 1000,
        "https://example.test/deep", &ordinal_depth_options,
        &ordinal_depth_result);
    /* html/head/title/body occupy element ordinals 0..3 and the appended
       chain starts at 4, adding one nesting level per element, so ordinal 5
       sits well inside the walk's 48-level bound and ordinal 60 well outside
       it.  Beyond the bound the walk must report "not found", which callers
       already treat as an absent stable key.  The chain stops short of the
       separate nesting cap appendChild enforces, so this measures the walk's
       own bound rather than that one. */
    static const char ordinal_depth_source[] =
        "(()=>{let node=document.body;"
        "for(let i=0;i<60;i++){const child=document.createElement('div');"
        "node.appendChild(child);node=child;}"
        "const shallow=__tilefinchFindStableNode('s:0:5:div'),"
        "deep=__tilefinchFindStableNode('s:0:60:div');"
        "globalThis.pocSummary=(shallow?'shallow-found':'shallow-missing')"
        "+':'+(deep?'deep-found':'deep-missing')"
        "+':'+String(__tilefinchStableNodeKey(shallow)||'none');})()";
    CHECK(ordinal_depth_runtime != NULL && ordinal_depth_result.success
          && script_runtime_evaluate_diagnostic(
                 ordinal_depth_runtime, ordinal_depth_source,
                 "<ordinal-depth-probe>", &ordinal_depth_result)
          && strcmp(ordinal_depth_result.summary,
                    "shallow-found:deep-missing:s:0:5:div") == 0);
    script_runtime_destroy(ordinal_depth_runtime);
    document_destroy(&ordinal_depth_document);
    CHECK(budget.current == ordinal_depth_baseline);

    puts("test: cross-origin child runtime rejects top-level backing");
    size_t child_scope_baseline = budget.current;
    static const char child_scope_page[] =
        "<!doctype html><title>Child</title><body></body>";
    PocDocument child_scope_document = {0};
    ScriptResult child_scope_result = {0};
    TestRemoteSelectorProbe child_scope_probe = {0};
    ScriptRuntimeOptions child_scope_options = {0};
    CHECK(document_parse(&child_scope_document, &budget, child_scope_page,
                         strlen(child_scope_page), 17)
          && viewport_context_init(
                 &child_scope_options.viewport, 480, 272, 480, 272));
    child_scope_options.document_scope = SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME;
    /* Even an accidentally populated creation option is sanitized by the
       child scope; navigation's iframe path uses this same scope. */
    child_scope_options.remote_selector_lookup =
        test_remote_selector_lookup;
    child_scope_options.remote_selector_opaque = &child_scope_probe;
    ScriptRuntime *child_scope_runtime = script_runtime_create_configured(
        &child_scope_document, &budget, 4 * MIB, 1000,
        "https://cross-origin-frame.test/", &child_scope_options,
        &child_scope_result);
    static const char child_scope_source[] =
        "globalThis.pocSummary=document.querySelector('#outside')===null"
        "?'FRAME-ISOLATED':'FRAME-LEAK'";
    CHECK(child_scope_runtime != NULL
          && script_runtime_evaluate_diagnostic(
                 child_scope_runtime, child_scope_source,
                 "<cross-origin-child-scope>", &child_scope_result)
          && strcmp(child_scope_result.summary, "FRAME-ISOLATED") == 0
          && child_scope_probe.calls == 0);
    ScriptRemoteDocumentBinding rejected_child_binding = {
        .selector_lookup = test_remote_selector_lookup,
        .selector_opaque = &child_scope_probe,
        .section_identity = 3
    };
    CHECK(!script_runtime_rebind_remote_document(
              child_scope_runtime, &rejected_child_binding)
          && script_runtime_evaluate_diagnostic(
                 child_scope_runtime, child_scope_source,
                 "<cross-origin-child-rebind>", &child_scope_result)
          && strcmp(child_scope_result.summary, "FRAME-ISOLATED") == 0
          && child_scope_probe.calls == 0);
    script_runtime_destroy(child_scope_runtime);
    document_destroy(&child_scope_document);
    CHECK(budget.current == child_scope_baseline);

    puts("test: JavaScript request policy forwarding");
    size_t request_policy_baseline = budget.current;
    static const char request_policy_page[] =
        "<!doctype html><title>Request policy</title><body></body>";
    PocDocument request_policy_document = {0};
    ScriptResult request_policy_result = {0};
    CHECK(document_parse(&request_policy_document, &budget,
                         request_policy_page, strlen(request_policy_page),
                         17));
    ScriptRuntimeOptions request_policy_options = {
        .allow_test_network_primitive_overrides = true
    };
    CHECK(viewport_context_init(
        &request_policy_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *request_policy_runtime = script_runtime_create_configured(
        &request_policy_document, &budget, 4 * MIB, 1000,
        "https://example.test/policy", &request_policy_options,
        &request_policy_result);
    static const char request_policy_source[] =
        "(()=>{const calls=[],asyncNative=__tilefinchFetchAsync,"
        "syncNative=__tilefinchFetchSync;try{"
        "__tilefinchFetchAsync=(...args)=>{calls.push('fetch:'+args[5]+':'"
        "+args[6]);throw new Error('captured');};"
        "fetch('/data',{mode:'same-origin',credentials:'omit'}).catch(()=>{});"
        "__tilefinchFetchSync=(...args)=>{calls.push('xhr:'+args[5]+':'"
        "+args[6]);return{status:204,url:'/xhr',headers:'',body:''};};"
        "const xhr=new XMLHttpRequest;xhr.open('GET','/xhr',false);"
        "xhr.withCredentials=true;xhr.send();"
        "const checks=[];try{new Request('/bad',{credentials:'invalid'});"
        "checks.push(false);}catch(error){checks.push(error instanceof TypeError);}"
        "try{new Request('/bad',{method:'GET\\r\\nX-Injected: yes'});"
        "checks.push(false);}catch(error){checks.push(error instanceof TypeError);}"
        "try{new Headers({'Bad Name':'value'});checks.push(false);}"
        "catch(error){checks.push(error instanceof TypeError);}"
        "try{new Headers({'X-Test':'safe\\r\\nX-Injected: yes'});"
        "checks.push(false);}catch(error){checks.push(error instanceof TypeError);}"
        "const badMethodXhr=new XMLHttpRequest;try{"
        "badMethodXhr.open('GET\\r\\nX-Injected: yes','/bad');"
        "checks.push(false);}catch(error){checks.push(error instanceof DOMException"
        "&&error.name==='SyntaxError');}"
        "const forbiddenXhr=new XMLHttpRequest;try{"
        "forbiddenXhr.open('TRACE','/bad');checks.push(false);}catch(error){"
        "checks.push(error instanceof DOMException&&error.name==='SecurityError');}"
        "const badHeaderXhr=new XMLHttpRequest;badHeaderXhr.open('GET','/bad');"
        "try{badHeaderXhr.setRequestHeader('X-Test','safe\\nInjected');"
        "checks.push(false);}catch(error){checks.push(error instanceof DOMException"
        "&&error.name==='SyntaxError');}"
        "try{syncNative('POST','/bad','x',"
        "'text/plain\\r\\nX-Injected: yes','','cors','same-origin');"
        "checks.push(false);}catch(error){checks.push(error instanceof TypeError);}"
        "try{syncNative('GET','/bad',undefined,'text/plain\\x00hidden',"
        "'','cors','same-origin');checks.push(false);}catch(error){"
        "checks.push(error instanceof TypeError);}"
        "try{syncNative('GET','/bad',undefined,'','Host: attacker.test',"
        "'cors','same-origin');checks.push(false);}catch(error){"
        "checks.push(error instanceof TypeError);}"
        "try{asyncNative('GET\\nX-Injected: yes','/bad');"
        "checks.push(false);}catch(error){checks.push(error instanceof TypeError);}"
        "globalThis.pocSummary='pending';"
        "fetch('/bad',{method:'bad method'}).then("
        "()=>{globalThis.pocSummary='unexpected-resolution';},"
        "error=>{globalThis.pocSummary=calls.join('|')+':'"
        "+checks.join(',')+':'+(error instanceof TypeError);});"
        "}finally{__tilefinchFetchAsync=asyncNative;"
        "__tilefinchFetchSync=syncNative;}})()";
    CHECK(request_policy_runtime != NULL && request_policy_result.success
          && script_runtime_evaluate_diagnostic(
                 request_policy_runtime, request_policy_source,
                 "<request-policy-probe>", &request_policy_result)
          && strcmp(request_policy_result.summary,
                    "fetch:same-origin:omit|xhr:cors:include:"
                    "true,true,true,true,true,true,true,true,true,true,true:"
                    "true") == 0);
    script_runtime_destroy(request_policy_runtime);
    document_destroy(&request_policy_document);
    CHECK(budget.current == request_policy_baseline);

    puts("test: streamed EventSource and fail-closed WebSocket lifecycle");
    size_t live_network_baseline = budget.current;
    char live_network_trace_error[256] = {0};
    CHECK(fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-event-source",
        live_network_trace_error, sizeof(live_network_trace_error)));
    static const char live_network_page[] =
        "<!doctype html><title>Live network</title><body></body>";
    PocDocument live_network_document = {0};
    ScriptResult live_network_result = {0};
    CHECK(document_parse(
        &live_network_document, &budget, live_network_page,
        strlen(live_network_page), 17));
    ScriptRuntime *live_network_runtime = script_runtime_create_deferred(
        &live_network_document, &budget, 5 * MIB, 1000,
        "https://events.test/page", NULL, &live_network_result);
    static const char live_network_source[] =
        "(()=>{const seen=[];let socketClosed=false,sourceDone=false;"
        "const mark=value=>{seen.push(value);globalThis.pocSummary="
        "'live-network:seen:'+seen.join('|');};"
        "const finish=()=>{if(socketClosed&&sourceDone)"
        "globalThis.pocSummary=seen.join('|');};"
        "const source=new EventSource('/feed');"
        "source.onopen=()=>mark('open');"
        "source.addEventListener('status',event=>mark("
        "'status:'+event.data+':'+event.lastEventId));"
        "source.onmessage=event=>{mark('message:'+event.data+':'"
        "+event.lastEventId);sourceDone=true;source.close();finish();};"
        "source.onerror=()=>{if(!sourceDone)mark('source-error');};"
        "const socket=new WebSocket('wss://events.test/socket',['chat']);"
        "socket.onclose=event=>{socketClosed=event.code===1006"
        "&&!event.wasClean;mark('socket-closed');finish();};"
        "let invalidClose=false;try{new WebSocket("
        "'wss://events.test/socket#bad');}"
        "catch(error){invalidClose=error instanceof DOMException"
        "&&error.name==='SyntaxError';}"
        "if(!invalidClose)mark('socket-validation-failed');"
        "globalThis.pocSummary='live-network:pending';})()";
    bool live_network_evaluated = live_network_runtime != NULL
        && live_network_result.success
        && script_runtime_evaluate_diagnostic(
            live_network_runtime, live_network_source,
            "<live-network-probe>", &live_network_result);
    if (!live_network_evaluated) {
        fprintf(stderr, "live network setup error=%s\n",
                live_network_result.error);
    }
    CHECK(live_network_evaluated);
    for (size_t tick = 0;
         tick < 32
         && strcmp(
                live_network_result.summary,
                "open|status:first\nsecond:7|message:final bird:7|"
                "socket-closed") != 0;
         tick++) {
        CHECK(script_runtime_advance(
            live_network_runtime, 1, 4, &live_network_result));
    }
    if (strcmp(
            live_network_result.summary,
            "open|status:first\nsecond:7|message:final bird:7|"
            "socket-closed") != 0) {
        fprintf(stderr, "live network summary=%s error=%s trace=%s\n",
                live_network_result.summary, live_network_result.error,
                live_network_trace_error);
    }
    CHECK(strcmp(
        live_network_result.summary,
        "open|status:first\nsecond:7|message:final bird:7|"
        "socket-closed") == 0);
    script_runtime_destroy(live_network_runtime);
    document_destroy(&live_network_document);
    fetch_trace_end();
    CHECK(budget.current == live_network_baseline);

    puts("test: cooperative script cancellation and recovery");
    size_t script_cancellation_baseline = budget.current;
    LayoutCooperateProbe script_probe = {.cancel_script_at = 1};
    TilefinchPlatformServices script_services = {
        .context = &script_probe,
        .cooperate = test_layout_cooperate
    };
    tilefinch_platform_set_services(&script_services);
    ScriptResult cancelled_scripts;
    CHECK(!scripts_run_document(&document, &budget, 4 * MIB, 1000,
                                &cancelled_scripts)
          && script_probe.script_calls == 1
          && cancelled_scripts.interrupted
          && budget.current == script_cancellation_baseline);
    script_probe = (LayoutCooperateProbe) {0};
    ScriptResult recovered_scripts;
    CHECK(scripts_run_document(&document, &budget, 4 * MIB, 1000,
                               &recovered_scripts)
          && recovered_scripts.success
          && script_probe.script_calls
             == recovered_scripts.script_cooperative_yields
          && recovered_scripts.script_cooperative_yields >= 6
          && recovered_scripts.script_max_slice_us > 0
          && budget.current == script_cancellation_baseline);
    tilefinch_platform_set_services(NULL);

    puts("test: window.location assignment requests navigation");
    size_t location_assignment_baseline = budget.current;
    static const char location_assignment_page[] =
        "<!doctype html><body><script>"
        "window.location='/assigned?from=window';"
        "location.href='/assigned?from=href';"
        "</script></body>";
    PocDocument location_assignment_document;
    CHECK(document_parse(&location_assignment_document, &budget,
                         location_assignment_page,
                         strlen(location_assignment_page), 17));
    ScriptResult location_assignment_result;
    ScriptRuntime *location_assignment_runtime = script_runtime_create(
        &location_assignment_document, &budget, 4 * MIB, 1000,
        "https://example.test/start", &location_assignment_result);
    char assigned_url[NAVIGATION_URL_LIMIT];
    bool assigned_replace = true;
    CHECK(location_assignment_runtime != NULL
          && script_runtime_consume_navigation(
              location_assignment_runtime, assigned_url,
              sizeof(assigned_url), &assigned_replace)
          && strcmp(assigned_url,
                    "https://example.test/assigned?from=href") == 0
          && !assigned_replace);
    script_runtime_destroy(location_assignment_runtime);
    document_destroy(&location_assignment_document);
    CHECK(budget.current == location_assignment_baseline);

    puts("test: deterministic clock advances during synchronous JavaScript");
    size_t deterministic_clock_baseline = budget.current;
    static const char deterministic_clock_page[] =
        "<!doctype html><html><body><script>"
        "const started=Date.now();let iterations=0;"
        "while(Date.now()-started<5)iterations++;"
        "const elapsed=Date.now()-started;"
        "const performanceStarted=performance.now();let performanceIterations=0;"
        "while(performance.now()-performanceStarted<5)performanceIterations++;"
        "const performanceElapsed=performance.now()-performanceStarted;"
        "const afterPerformanceDate=Date.now();"
        "globalThis.pocSummary=(elapsed===6&&performanceStarted===0"
        "&&performanceElapsed===6&&afterPerformanceDate-started===7"
        "&&iterations===4&&performanceIterations===4)"
        "?'clock-advanced':'clock-frozen';"
        "</script></body></html>";
    PocDocument deterministic_clock_document;
    CHECK(document_parse(&deterministic_clock_document, &budget,
                         deterministic_clock_page,
                         strlen(deterministic_clock_page), 17));
    ScriptResult deterministic_clock_result;
    script_runtime_configure_deterministic_replay(true, 42);
    bool deterministic_clock_ok = scripts_run_document_at(
        &deterministic_clock_document, &budget, 4 * MIB, 1000,
        "https://clock-loop.test/", &deterministic_clock_result);
    script_runtime_configure_deterministic_replay(false, 1);
    CHECK(deterministic_clock_ok
          && deterministic_clock_result.success
          && deterministic_clock_result.date_now_calls > 2
          && deterministic_clock_result.performance_now_calls >= 1
          && strcmp(deterministic_clock_result.summary,
                    "clock-advanced") == 0);
    document_destroy(&deterministic_clock_document);
    CHECK(budget.current == deterministic_clock_baseline);

    puts("test: deterministic Date constructor, function, and Intl defaults");
    size_t deterministic_date_baseline = budget.current;
    static const char deterministic_date_page[] =
        "<!doctype html><html><body><script>"
        "const origin=1700000000042,before=Date.now(),constructed=new Date(),"
        "called=Date(0),parsed=Date.parse(called),"
        "formatter=new Intl.DateTimeFormat('en-US',{timeZone:'UTC'}),"
        "implicit=formatter.format(),explicitDate=new Date(Date.now()),"
        "explicit=formatter.format(explicitDate),parts=formatter.formatToParts(constructed),"
        "numberText=new Intl.NumberFormat('en-US').format(1234.5),"
        "percentText=new Intl.NumberFormat('en-US',{style:'percent'}).format(.25),"
        "currencyText=new Intl.NumberFormat('en-US',{style:'currency',currency:'USD'}).format(1234.5),"
        "pluralText=new Intl.PluralRules('en-US').select(1)+'/'"
        "+new Intl.PluralRules('en-US').select(2)+'/'"
        "+new Intl.PluralRules('en-US',{type:'ordinal'}).select(2),"
        "prototypeDescriptor=Object.getOwnPropertyDescriptor(Date,'prototype'),"
        "nowDescriptor=Object.getOwnPropertyDescriptor(Date,'now'),"
        "nameDescriptor=Object.getOwnPropertyDescriptor(Date,'name'),"
        "lengthDescriptor=Object.getOwnPropertyDescriptor(Date,'length');"
        "class ReplayDateSubclass extends Date{}const subclass=new ReplayDateSubclass;"
        "function AlternateDate(){}AlternateDate.prototype={alternate:true};"
        "const reflected=Reflect.construct(Date,[],AlternateDate),"
        "times=[before,constructed.getTime(),subclass.getTime(),"
        "Date.prototype.getTime.call(reflected),explicitDate.getTime()];"
        "const deterministic=times[0]===origin&&times[1]===origin+1"
        "&&times[2]===origin+5&&times[3]===origin+6"
        "&&times[4]===origin+4&&Math.abs(parsed-before)<1000;"
        "const descriptors=Date.name==='Date'&&Date.length===7"
        "&&!prototypeDescriptor.writable&&!prototypeDescriptor.enumerable"
        "&&!prototypeDescriptor.configurable&&nowDescriptor.writable"
        "&&!nowDescriptor.enumerable&&nowDescriptor.configurable"
        "&&!nameDescriptor.writable&&!nameDescriptor.enumerable"
        "&&nameDescriptor.configurable&&!lengthDescriptor.writable"
        "&&!lengthDescriptor.enumerable&&lengthDescriptor.configurable;"
        "const semantics=typeof called==='string'&&new Date(123).getTime()===123"
        "&&constructed instanceof Date&&subclass instanceof Date"
        "&&subclass instanceof ReplayDateSubclass"
        "&&Object.getPrototypeOf(reflected)===AlternateDate.prototype"
        "&&reflected.alternate===true"
        "&&Date.prototype.constructor===Date"
        "&&Object.getPrototypeOf(Date)===Function.prototype"
        "&&Object.prototype.toString.call(constructed)==='[object Date]'"
        "&&Date.UTC(1970,0,1)===0&&Date.parse('1970-01-01T00:00:00.000Z')===0"
        "&&called==='Tue Nov 14 2023 22:13:20 GMT+0000 (Coordinated Universal Time)'"
        "&&constructed.toString()===called&&constructed.getTimezoneOffset()===0"
        "&&constructed.getHours()===22&&constructed.toLocaleDateString()==='11/14/2023'"
        "&&implicit===explicit&&implicit==='11/14/2023'"
        "&&parts.map(part=>part.type+':'+part.value).join(',')"
        "==='month:11,literal:/,day:14,literal:/,year:2023'"
        "&&formatter.resolvedOptions().locale==='en-US'"
        "&&formatter.resolvedOptions().timeZone==='UTC'"
        "&&numberText==='1,234.5'&&percentText==='25%'"
        "&&currencyText==='USD 1,234.5'&&pluralText==='one/other/two'"
        "&&new Intl.Locale('en_US').toString()==='en-US'"
        "&&Intl.getCanonicalLocales(['en_US'])[0]==='en-US'"
        "&&new Intl.Collator('en-US',{sensitivity:'base'}).compare('A','a')===0"
        "&&new Intl.RelativeTimeFormat('en-US',{numeric:'auto'}).format(-1,'day')==='yesterday'"
        "&&new Intl.ListFormat('en-US').format(['a','b'])==='a and b'"
        "&&new Intl.DisplayNames('en-US',{type:'region'}).of('US')==='United States';"
        "const sources=Function.prototype.toString.call(Date).includes('[native code]')"
        "&&Function.prototype.toString.call(Date.now).includes('[native code]')"
        "&&Date.now.name==='now'&&Date.now.length===0;"
        "globalThis.pocSummary=deterministic&&descriptors&&semantics&&sources"
        "?'DATE-REPLAY-OK':'DATE-REPLAY-FAILED:'+JSON.stringify({"
        "times,called,parsed,implicit,explicit,deterministic,descriptors,semantics,"
        "sources,dateSource:Function.prototype.toString.call(Date),"
        "nowSource:Function.prototype.toString.call(Date.now)});"
        "</script></body></html>";
    PocDocument deterministic_date_document;
    CHECK(document_parse(
        &deterministic_date_document, &budget, deterministic_date_page,
        strlen(deterministic_date_page), 17));
    ScriptResult deterministic_date_result;
    script_runtime_configure_deterministic_replay(true, 42);
    bool deterministic_date_ok = scripts_run_document_at(
        &deterministic_date_document, &budget, 4u * MIB, 1000,
        "https://date-replay.test/", &deterministic_date_result);
    script_runtime_configure_deterministic_replay(false, 0);
    if (!deterministic_date_ok
        || strcmp(deterministic_date_result.summary,
                  "DATE-REPLAY-OK") != 0) {
        fprintf(stderr, "deterministic Date probe ok=%d summary=%s error=%s\n",
                deterministic_date_ok, deterministic_date_result.summary,
                deterministic_date_result.error);
    }
    CHECK(deterministic_date_ok && deterministic_date_result.success
          && deterministic_date_result.date_now_calls >= 7
          && strcmp(deterministic_date_result.summary,
                    "DATE-REPLAY-OK") == 0);
    document_destroy(&deterministic_date_document);
    CHECK(budget.current == deterministic_date_baseline);

    puts("test: deterministic clock tick, timer, rAF, and timeline vector");
    size_t deterministic_tick_baseline = budget.current;
    static const char deterministic_tick_page[] =
        "<!doctype html><html><body></body></html>";
    static const char deterministic_tick_source[] =
        "(()=>{const values=[performance.timeOrigin,Date.now(),performance.now()];"
        "setTimeout(()=>values.push(Date.now(),performance.now()),10);"
        "requestAnimationFrame(timestamp=>{values.push(timestamp,Date.now(),"
        "performance.now(),document.timeline.currentTime);"
        "globalThis.pocSummary=values.join(',');});})()";
    PocDocument deterministic_tick_document;
    CHECK(document_parse(
        &deterministic_tick_document, &budget, deterministic_tick_page,
        strlen(deterministic_tick_page), 17));
    ViewportContext deterministic_tick_viewport;
    CHECK(viewport_context_init(
        &deterministic_tick_viewport, 480, 272, 480, 272));
    ScriptRuntimeOptions deterministic_tick_options = {
        .viewport = deterministic_tick_viewport,
        .defer_document_scripts = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
    };
    ScriptResult deterministic_tick_result;
    script_runtime_configure_deterministic_replay(true, 42);
    ScriptRuntime *deterministic_tick_runtime =
        script_runtime_create_configured(
            &deterministic_tick_document, &budget, 4u * MIB, 1000,
            "https://clock-vector.test/", &deterministic_tick_options,
            &deterministic_tick_result);
    CHECK(deterministic_tick_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              deterministic_tick_runtime, deterministic_tick_source,
              "<deterministic-clock-vector>", &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 16, 8,
              &deterministic_tick_result));
    static const char deterministic_tick_expected[] =
        "1700000000042,1700000000042,0,1700000000059,17,18,"
        "1700000000060,18,19";
    if (strcmp(deterministic_tick_result.summary,
               deterministic_tick_expected) != 0) {
        fprintf(stderr, "deterministic tick vector summary=%s error=%s\n",
                deterministic_tick_result.summary,
                deterministic_tick_result.error);
    }
    CHECK(deterministic_tick_result.success
          && deterministic_tick_result.timer_callbacks_run == 2
          && strcmp(deterministic_tick_result.summary,
                    deterministic_tick_expected) == 0);

    static const char deterministic_task_order_source[] =
        "(()=>{const order=[];setTimeout(()=>{order.push('t1');"
        "Promise.resolve().then(()=>order.push('m1'));},0);"
        "setTimeout(()=>{order.push('t2');Promise.resolve().then("
        "()=>{order.push('m2');globalThis.pocSummary=order.join(',');});},0);})()";
    CHECK(script_runtime_evaluate_diagnostic(
              deterministic_tick_runtime,
              deterministic_task_order_source,
              "<deterministic-task-order>", &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 0, 2,
              &deterministic_tick_result)
          && strcmp(deterministic_tick_result.summary,
                    "t1,m1,t2,m2") == 0);

    static const char deterministic_raf_surface_source[] =
        "(()=>{let immediate=false,cancelled=false,observed=false;"
        "try{requestAnimationFrame(null)}catch(error){immediate=error instanceof TypeError;}"
        "const cancelledId=requestAnimationFrame(()=>cancelled=true);"
        "cancelAnimationFrame(cancelledId);const timeline=document.timeline,"
        "documentDescriptor=Object.getOwnPropertyDescriptor(document,'timeline'),"
        "timeDescriptor=Object.getOwnPropertyDescriptor(timeline,'currentTime'),"
        "rafDescriptor=Object.getOwnPropertyDescriptor(globalThis,'requestAnimationFrame'),"
        "current=timeline.currentTime,observer=new PerformanceObserver(()=>observed=true);"
        "observer.observe({entryTypes:['mark','resource']});const mark=performance.mark("
        "'hidden',{startTime:4,detail:'detail'}),measure=performance.measure("
        "'span','hidden');performance.clearMarks('hidden');let missing='';"
        "try{performance.measure('missing','hidden')}catch(error){missing=error.name}"
        "performance.clearMeasures();"
        "requestAnimationFrame(timestamp=>{globalThis.pocSummary=immediate&&!cancelled"
        "&&!observed&&typeof timestamp==='number'&&timestamp>=current"
        "&&timeline===document.timeline&&typeof documentDescriptor.get==='function'"
        "&&documentDescriptor.set===undefined&&documentDescriptor.enumerable"
        "&&documentDescriptor.configurable&&typeof timeDescriptor.get==='function'"
        "&&timeDescriptor.set===undefined&&timeDescriptor.enumerable"
        "&&timeDescriptor.configurable&&rafDescriptor.writable"
        "&&rafDescriptor.enumerable&&rafDescriptor.configurable"
        "&&mark.name==='hidden'&&mark.entryType==='mark'&&mark.startTime===4"
        "&&mark.detail==='detail'&&measure.name==='span'"
        "&&measure.entryType==='measure'&&measure.startTime===4"
        "&&measure.duration>=0&&missing==='SyntaxError'"
        "&&performance.getEntries().length===0"
        "&&performance.getEntriesByName('hidden').length===0"
        "&&performance.getEntriesByType('mark').length===0"
        "&&PerformanceObserver.supportedEntryTypes.length===0"
        "&&observer.takeRecords().length===0?'RAF-REPLAY-OK':'RAF-REPLAY-FAILED';});})()";
    CHECK(script_runtime_evaluate_diagnostic(
              deterministic_tick_runtime,
              deterministic_raf_surface_source,
              "<deterministic-raf-surface>", &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 16, 4,
              &deterministic_tick_result)
          && deterministic_tick_result.timer_callbacks_run == 5
          && strcmp(deterministic_tick_result.summary,
                    "RAF-REPLAY-OK") == 0);
    ScriptDeterministicReplayDiagnostics deterministic_diagnostics;
    if (script_runtime_deterministic_replay_diagnostics(
            deterministic_tick_runtime, &deterministic_diagnostics)
        && (deterministic_diagnostics.wall_elapsed_ms != 35
            || deterministic_diagnostics.monotonic_elapsed_ms != 38
            || deterministic_diagnostics.wall_observations != 3
            || deterministic_diagnostics.monotonic_observations != 6
            || deterministic_diagnostics.monotonic_samples != 2)) {
        fprintf(stderr,
                "deterministic clock diagnostics host=%llu wall=%llu mono=%llu "
                "wall-obs=%llu mono-obs=%llu samples=%llu sources="
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long) deterministic_diagnostics.host_elapsed_ms,
                (unsigned long long) deterministic_diagnostics.wall_elapsed_ms,
                (unsigned long long) deterministic_diagnostics.monotonic_elapsed_ms,
                (unsigned long long) deterministic_diagnostics.wall_observations,
                (unsigned long long) deterministic_diagnostics.monotonic_observations,
                (unsigned long long) deterministic_diagnostics.monotonic_samples,
                (unsigned long long) deterministic_diagnostics.sources.date_now,
                (unsigned long long) deterministic_diagnostics.sources.date_function,
                (unsigned long long) deterministic_diagnostics.sources.date_constructor,
                (unsigned long long) deterministic_diagnostics.sources.performance_now,
                (unsigned long long) deterministic_diagnostics.sources.performance_mark,
                (unsigned long long) deterministic_diagnostics.sources.performance_measure,
                (unsigned long long) deterministic_diagnostics.sources.animation_timeline,
                (unsigned long long) deterministic_diagnostics.sources
                    .idle_deadline_time_remaining,
                (unsigned long long) deterministic_diagnostics.sources.animation_frame,
                (unsigned long long) deterministic_diagnostics.sources.event_timestamp,
                (unsigned long long) deterministic_diagnostics.sources
                    .intersection_observer,
                (unsigned long long) deterministic_diagnostics.sources.idle_callback_start);
    }
    CHECK(script_runtime_deterministic_replay_diagnostics(
              deterministic_tick_runtime, &deterministic_diagnostics)
          && deterministic_diagnostics.seed == 42
          && deterministic_diagnostics.clock_origin_ms
                 == UINT64_C(1700000000042)
          && deterministic_diagnostics.host_elapsed_ms == 32
          && deterministic_diagnostics.wall_elapsed_ms == 35
          && deterministic_diagnostics.monotonic_elapsed_ms == 38
          && deterministic_diagnostics.wall_observations == 3
          && deterministic_diagnostics.monotonic_observations == 6
          && deterministic_diagnostics.monotonic_samples == 2
          && deterministic_diagnostics.sources.date_now == 3
          && deterministic_diagnostics.sources.date_function == 0
          && deterministic_diagnostics.sources.date_constructor == 0
          && deterministic_diagnostics.sources.performance_now == 3
          && deterministic_diagnostics.sources.performance_mark == 0
          && deterministic_diagnostics.sources.performance_measure == 1
          && deterministic_diagnostics.sources.animation_timeline == 2
          && deterministic_diagnostics.sources
                 .idle_deadline_time_remaining == 0
          && deterministic_diagnostics.sources.animation_frame == 2
          && deterministic_diagnostics.sources.event_timestamp == 0
          && deterministic_diagnostics.sources.intersection_observer == 0
          && deterministic_diagnostics.sources.idle_callback_start == 0);

    static const char bounded_animation_source[] =
        "(()=>{const box=document.createElement('div');"
        "document.body.appendChild(box);"
        "box.style.width='10px';"
        "box.style.transitionProperty='width';"
        "box.style.transitionDuration='32ms';"
        "box.style.width='30px';"
        "const transitionTarget=box.style.width==='30px';"
        "let transitionIntermediate=false;"
        "requestAnimationFrame(()=>requestAnimationFrame(()=>{"
        "const width=(box.getAttribute('style').match("
        "/width\\s*:\\s*([^;]+)/)||[])[1]||'';"
        "transitionIntermediate=width!=='10px'&&width!=='30px'&&!!width;}));"
        "const animation=box.animate([{opacity:'0'},{opacity:'1'}],"
        "{duration:32,iterations:1});"
        "const batchedFrames=__tilefinchRootCensus.timers===2;"
        "const initial=animation.playState==='running'"
        "&&animation.currentTime===0;"
        "animation.finished.then(()=>{globalThis.pocSummary="
        "transitionTarget&&transitionIntermediate&&batchedFrames&&initial"
        "&&animation.playState==='finished'"
        "&&box.style.opacity==='1'?'BOUNDED-ANIMATION-OK':"
        "'BOUNDED-ANIMATION-FAILED:'+"
        "[transitionTarget,transitionIntermediate,batchedFrames,initial,"
        "animation.playState,box.style.opacity,"
        "getComputedStyle(box).width].join(',');});})()";
    CHECK(script_runtime_evaluate_diagnostic(
              deterministic_tick_runtime, bounded_animation_source,
              "<bounded-animation>", &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 16, 16,
              &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 16, 16,
              &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 16, 16,
              &deterministic_tick_result));
    if (strcmp(deterministic_tick_result.summary,
               "BOUNDED-ANIMATION-OK") != 0) {
        fprintf(stderr, "bounded animation summary=%s error=%s\n",
                deterministic_tick_result.summary,
                deterministic_tick_result.error);
    }
    CHECK(deterministic_tick_result.success
          && strcmp(deterministic_tick_result.summary,
                    "BOUNDED-ANIMATION-OK") == 0);
    script_runtime_destroy(deterministic_tick_runtime);

    puts("test: motion-free class changes reuse a negative inline hint");
    ScriptResult motion_free_result;
    ScriptRuntime *motion_free_runtime = script_runtime_create_configured(
        &deterministic_tick_document, &budget, 4u * MIB, 1000,
        "https://motion-free.test/", &deterministic_tick_options,
        &motion_free_result);
    static const char motion_free_source[] =
        "(()=>{const box=document.createElement('div');"
        "document.body.appendChild(box);"
        "const before=__tilefinchRootCensus.motionInlineHintScans;"
        "for(let i=0;i<32;i++)box.className='state-'+i;"
        "const scans=__tilefinchRootCensus.motionInlineHintScans-before;"
        "globalThis.pocSummary=scans===1?'MOTION-NEGATIVE-CACHED':"
        "'MOTION-NEGATIVE-SCANS:'+scans})()";
    CHECK(motion_free_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              motion_free_runtime, motion_free_source,
              "<motion-negative-cache>", &motion_free_result)
          && motion_free_result.success
          && strcmp(motion_free_result.summary,
                    "MOTION-NEGATIVE-CACHED") == 0);
    script_runtime_destroy(motion_free_runtime);

    puts("test: reduced-motion mode skips stylesheet animation scans");
    ScriptResult stylesheet_animation_result;
    ScriptRuntimeOptions stylesheet_animation_options =
        deterministic_tick_options;
    stylesheet_animation_options.defer_document_scripts = false;
    ScriptRuntime *stylesheet_animation_runtime =
        script_runtime_create_configured(
            &deterministic_tick_document, &budget, 4u * MIB, 1000,
            "https://motion.test/", &stylesheet_animation_options,
            &stylesheet_animation_result);
    CHECK(stylesheet_animation_runtime != NULL
          && stylesheet_animation_result.bootstrap_lazy_module_loads == 0);
    static const char stylesheet_animation_source[] =
        "(()=>{const style=document.createElement('style'),"
        "box=document.createElement('div');"
        "style.textContent='@keyframes pulse{from{opacity:0}'"
        "+'to{opacity:1}}.motion-probe{animation:pulse 32ms linear}';"
        "box.className='motion-probe';"
        "document.head.appendChild(style);document.body.appendChild(box);"
        "queueMicrotask(()=>{globalThis.pocSummary="
        "matchMedia('(prefers-reduced-motion:reduce)').matches"
        "&&__tilefinchRootCensus.motionScans===0"
        "?'STYLESHEET-MOTION-REDUCED':'STYLESHEET-MOTION-SCANNED:'+"
        "__tilefinchRootCensus.motionScans});"
        "globalThis.pocSummary='STYLESHEET-MOTION-PENDING';})()";
    CHECK(stylesheet_animation_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              stylesheet_animation_runtime, stylesheet_animation_source,
              "<stylesheet-animation>", &stylesheet_animation_result));
    for (size_t tick = 0; tick < 2
                          && strcmp(stylesheet_animation_result.summary,
                                    "STYLESHEET-MOTION-REDUCED") != 0;
         tick++) {
        CHECK(script_runtime_advance(
            stylesheet_animation_runtime, 16, 16,
            &stylesheet_animation_result));
    }
    if (strcmp(stylesheet_animation_result.summary,
               "STYLESHEET-MOTION-REDUCED") != 0) {
        fprintf(stderr, "stylesheet animation summary=%s error=%s\n",
                stylesheet_animation_result.summary,
                stylesheet_animation_result.error);
    }
    CHECK(stylesheet_animation_result.success
          && strcmp(stylesheet_animation_result.summary,
                    "STYLESHEET-MOTION-REDUCED") == 0
          && stylesheet_animation_result.bootstrap_lazy_module_loads == 1
          && stylesheet_animation_result.bootstrap_lazy_module_failures == 0);

    puts("test: motion saturation reveals final authored state");
    static const char saturated_animation_source[] =
        "(()=>{const animations=[];let last=null;"
        "for(let i=0;i<40;i++){const box=document.createElement('div');"
        "box.style.opacity='0';document.body.appendChild(box);last=box;"
        "animations.push(box.animate([{opacity:'0'},{opacity:'1'}],"
        "{duration:1000}));}"
        "globalThis.pocSummary=last.style.opacity==='1'"
        "&&animations[39].playState==='finished'"
        "?'MOTION-SATURATION-REVEALS-FINAL'"
        ":'MOTION-SATURATION-HID-CONTENT:'"
        "+[last.style.opacity,animations[39].playState].join(',');"
        "for(const animation of animations)animation.cancel();})()";
    CHECK(script_runtime_evaluate_diagnostic(
              stylesheet_animation_runtime, saturated_animation_source,
              "<saturated-animation>", &stylesheet_animation_result));
    if (strcmp(stylesheet_animation_result.summary,
               "MOTION-SATURATION-REVEALS-FINAL") != 0) {
        fprintf(stderr, "saturated animation summary=%s error=%s\n",
                stylesheet_animation_result.summary,
                stylesheet_animation_result.error);
    }
    CHECK(stylesheet_animation_result.success
          && strcmp(stylesheet_animation_result.summary,
                    "MOTION-SATURATION-REVEALS-FINAL") == 0);
    script_runtime_destroy(stylesheet_animation_runtime);

    puts("test: deterministic clock source domains and stable callback samples");
    static const char deterministic_clock_sources_source[] =
        "(()=>{const origin=1700000000042,dateNow=Date.now(),"
        "dateFunction=Date(),dateConstructor=new Date().getTime(),"
        "performanceNow=performance.now(),automaticMark=performance.mark('auto'),"
        "explicitMark=performance.mark('explicit',{startTime:1}),"
        "implicitMeasure=performance.measure('implicit','explicit'),"
        "timeline=document.timeline.currentTime,event=new Event('probe'),"
        "eventTime=event.timeStamp,state={completed:0,io:false,idle:false,raf:[]};"
        "const synchronous=dateNow===origin&&typeof dateFunction==='string'"
        "&&dateConstructor===origin+2&&performanceNow===0"
        "&&automaticMark.startTime===1&&explicitMark.startTime===1"
        "&&implicitMeasure.startTime===1&&implicitMeasure.duration===1"
        "&&timeline===3&&eventTime===4&&event.timeStamp===eventTime"
        "&&performance.timing.navigationStart===performance.timeOrigin"
        "&&performance.navigation.type===0"
        "&&performance.navigation.redirectCount===0;"
        "const finish=()=>{if(state.completed===3)globalThis.pocSummary="
        "synchronous&&state.io&&state.idle&&state.raf.length===2"
        "&&state.raf[0]===22&&state.raf[1]===state.raf[0]"
        "?'CLOCK-SOURCES-OK':'CLOCK-SOURCES-FAILED:'+JSON.stringify(state);};"
        "const observer=new IntersectionObserver(entries=>{state.io="
        "entries.length===2&&entries[0].time===4"
        "&&entries[1].time===entries[0].time;observer.disconnect();"
        "state.completed++;finish();});observer.observe(document.body);"
        "observer.observe(document.documentElement);"
        "requestIdleCallback(deadline=>{const first=deadline.timeRemaining(),"
        "second=deadline.timeRemaining();state.idle=!deadline.didTimeout"
        "&&first===8&&second===7;state.completed++;finish();});"
        "const frame=timestamp=>{state.raf.push(timestamp);if(state.raf.length===2){"
        "state.completed++;finish();}};requestAnimationFrame(frame);"
        "requestAnimationFrame(frame);globalThis.pocSummary='CLOCK-SOURCES-PENDING';})()";
    ScriptResult deterministic_clock_sources_result;
    ScriptRuntime *deterministic_clock_sources_runtime =
        script_runtime_create_configured(
            &deterministic_tick_document, &budget, 4u * MIB, 1000,
            "https://clock-sources.test/", &deterministic_tick_options,
            &deterministic_clock_sources_result);
    CHECK(deterministic_clock_sources_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              deterministic_clock_sources_runtime,
              deterministic_clock_sources_source,
              "<deterministic-clock-sources>",
              &deterministic_clock_sources_result)
          && script_runtime_advance(
              deterministic_clock_sources_runtime, 16, 8,
              &deterministic_clock_sources_result));
    if (strcmp(deterministic_clock_sources_result.summary,
               "CLOCK-SOURCES-OK") != 0) {
        fprintf(stderr, "deterministic clock sources summary=%s error=%s\n",
                deterministic_clock_sources_result.summary,
                deterministic_clock_sources_result.error);
    }
    ScriptDeterministicReplayDiagnostics source_diagnostics;
    CHECK(deterministic_clock_sources_result.success
          && strcmp(deterministic_clock_sources_result.summary,
                    "CLOCK-SOURCES-OK") == 0
          && script_runtime_deterministic_replay_diagnostics(
              deterministic_clock_sources_runtime, &source_diagnostics)
          && source_diagnostics.seed == 42
          && source_diagnostics.clock_origin_ms
                 == UINT64_C(1700000000042)
          && source_diagnostics.host_elapsed_ms == 16
          && source_diagnostics.wall_elapsed_ms == 19
          && source_diagnostics.monotonic_elapsed_ms == 22
          && source_diagnostics.wall_observations == 3
          && source_diagnostics.monotonic_observations == 6
          && source_diagnostics.monotonic_samples == 4
          && source_diagnostics.wall_elapsed_ms
                 == source_diagnostics.host_elapsed_ms
                    + source_diagnostics.wall_observations
          && source_diagnostics.monotonic_elapsed_ms
                 == source_diagnostics.host_elapsed_ms
                    + source_diagnostics.monotonic_observations
          && source_diagnostics.sources.date_now == 1
          && source_diagnostics.sources.date_function == 1
          && source_diagnostics.sources.date_constructor == 1
          && source_diagnostics.sources.performance_now == 1
          && source_diagnostics.sources.performance_mark == 1
          && source_diagnostics.sources.performance_measure == 1
          && source_diagnostics.sources.animation_timeline == 1
          && source_diagnostics.sources
                 .idle_deadline_time_remaining == 2
          && source_diagnostics.sources.animation_frame == 1
          && source_diagnostics.sources.event_timestamp == 1
          && source_diagnostics.sources.intersection_observer == 1
          && source_diagnostics.sources.idle_callback_start == 1);
    script_runtime_destroy(deterministic_clock_sources_runtime);
    script_runtime_configure_deterministic_replay(false, 0);

    puts("test: network tasks checkpoint microtasks between completions");
    char deterministic_network_error[256] = {0};
    CHECK(fetch_trace_replay_begin(
              TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-fetch-order",
              deterministic_network_error,
              sizeof(deterministic_network_error)));
    ScriptResult deterministic_network_result;
    script_runtime_configure_deterministic_replay(true, 42);
    ScriptRuntime *deterministic_network_runtime =
        script_runtime_create_configured(
            &deterministic_tick_document, &budget, 4u * MIB, 1000,
            "https://image-cap.test/", &deterministic_tick_options,
            &deterministic_network_result);
    static const char deterministic_network_order_source[] =
        "(()=>{const order=[],fail=error=>{globalThis.pocSummary='FETCH:'"
        "+String(error)};fetch('https://image-cap.test/a.svg',{mode:'no-cors'}).then("
        "()=>{order.push('f1');Promise.resolve().then(()=>order.push('m1'));},fail);"
        "fetch('https://image-cap.test/b.svg',{mode:'no-cors'}).then(()=>{order.push('f2');"
        "Promise.resolve().then(()=>{order.push('m2');globalThis.pocSummary="
        "order.join(',');});},fail);})()";
    bool deterministic_network_ok = deterministic_network_runtime != NULL
        && script_runtime_evaluate_diagnostic(
            deterministic_network_runtime,
            deterministic_network_order_source,
            "<deterministic-network-order>",
            &deterministic_network_result);
    for (size_t turn = 0;
         deterministic_network_ok && turn < 16
         && strcmp(deterministic_network_result.summary,
                   "f1,m1,f2,m2") != 0;
         turn++) {
        deterministic_network_ok = script_runtime_advance(
            deterministic_network_runtime, 0, 4,
            &deterministic_network_result);
    }
    if (!deterministic_network_ok
        || strcmp(deterministic_network_result.summary,
                  "f1,m1,f2,m2") != 0) {
        fprintf(stderr, "deterministic network order ok=%d summary=%s "
                "error=%s trace=%s completed=%zu failures=%zu pending=%zu\n",
                deterministic_network_ok,
                deterministic_network_result.summary,
                deterministic_network_result.error,
                deterministic_network_error,
                deterministic_network_result.async_network_completed,
                deterministic_network_result.network_failures,
                deterministic_network_result.pending_tasks);
    }
    CHECK(deterministic_network_ok
          && deterministic_network_result.async_network_completed == 2
          && strcmp(deterministic_network_result.summary,
                    "f1,m1,f2,m2") == 0);
    script_runtime_destroy(deterministic_network_runtime);
    script_runtime_configure_deterministic_replay(false, 0);
    fetch_trace_end();

    puts("test: saturated Date remains isolated from monotonic replay time");
    char saturation_directory[] = "/tmp/tilefinch-clock-v2-XXXXXX";
    CHECK(mkdtemp(saturation_directory) != NULL);
    char saturation_trace_path[192];
    int saturation_path_length = snprintf(
        saturation_trace_path, sizeof(saturation_trace_path),
        "%s/trace.meta", saturation_directory);
    CHECK(saturation_path_length > 0
          && (size_t) saturation_path_length
                 < sizeof(saturation_trace_path));
    FILE *saturation_trace = fopen(saturation_trace_path, "wb");
    CHECK(saturation_trace != NULL);
    bool saturation_trace_written = fprintf(
        saturation_trace,
        "psp-http-trace-clock=1\n"
        "origin-ms=8639999999999999\n"
        "capture-complete=yes\n"
        "record-count=0\n") > 0;
    bool saturation_trace_closed = fclose(saturation_trace) == 0;
    CHECK(saturation_trace_written && saturation_trace_closed);
    char saturation_trace_error[256] = {0};
    CHECK(fetch_trace_replay_begin(
              saturation_directory, saturation_trace_error,
              sizeof(saturation_trace_error)));
    script_runtime_configure_deterministic_replay(true, 42);
    ScriptResult saturation_result;
    ScriptRuntime *saturation_runtime = script_runtime_create_configured(
        &deterministic_tick_document, &budget, 4u * MIB, 1000,
        "https://clock-saturation.test/", &deterministic_tick_options,
        &saturation_result);
    static const char saturation_initial_source[] =
        "globalThis.clockSaturation={dates:[Date.now(),Date.now(),Date.now()],"
        "monotonic:[performance.now(),performance.now()]};";
    static const char saturation_final_source[] =
        "(()=>{const date=Date.now(),monotonic=performance.now(),"
        "probe=globalThis.clockSaturation;globalThis.pocSummary="
        "probe.dates[0]===8639999999999999"
        "&&probe.dates[1]===8640000000000000"
        "&&probe.dates[2]===8640000000000000"
        "&&probe.monotonic[0]===0&&probe.monotonic[1]===1"
        "&&date===8640000000000000&&monotonic===9"
        "?'DATE-SATURATION-OK':'DATE-SATURATION-FAILED';})()";
    CHECK(saturation_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              saturation_runtime, saturation_initial_source,
              "<date-saturation-initial>", &saturation_result)
          && script_runtime_advance(
              saturation_runtime, 7, 0, &saturation_result)
          && script_runtime_evaluate_diagnostic(
              saturation_runtime, saturation_final_source,
              "<date-saturation-final>", &saturation_result));
    ScriptDeterministicReplayDiagnostics saturation_diagnostics;
    CHECK(saturation_result.success
          && strcmp(saturation_result.summary,
                    "DATE-SATURATION-OK") == 0
          && script_runtime_deterministic_replay_diagnostics(
              saturation_runtime, &saturation_diagnostics)
          && saturation_diagnostics.seed == 42
          && saturation_diagnostics.clock_origin_ms
                 == UINT64_C(8639999999999999)
          && saturation_diagnostics.host_elapsed_ms == 7
          && saturation_diagnostics.wall_elapsed_ms == 11
          && saturation_diagnostics.monotonic_elapsed_ms == 10
          && saturation_diagnostics.wall_observations == 4
          && saturation_diagnostics.monotonic_observations == 3
          && saturation_diagnostics.monotonic_samples == 0
          && saturation_diagnostics.sources.date_now == 4
          && saturation_diagnostics.sources.date_function == 0
          && saturation_diagnostics.sources.date_constructor == 0
          && saturation_diagnostics.sources.performance_now == 3
          && saturation_diagnostics.sources.performance_mark == 0
          && saturation_diagnostics.sources.performance_measure == 0
          && saturation_diagnostics.sources.animation_timeline == 0
          && saturation_diagnostics.sources
                 .idle_deadline_time_remaining == 0
          && saturation_diagnostics.sources.animation_frame == 0
          && saturation_diagnostics.sources.event_timestamp == 0
          && saturation_diagnostics.sources.intersection_observer == 0
          && saturation_diagnostics.sources.idle_callback_start == 0);
    script_runtime_destroy(saturation_runtime);
    script_runtime_configure_deterministic_replay(false, 0);
    fetch_trace_end();
    CHECK(remove(saturation_trace_path) == 0
          && rmdir(saturation_directory) == 0);
    document_destroy(&deterministic_tick_document);
    CHECK(budget.current == deterministic_tick_baseline);

    puts("test: deterministic entropy is document-derived and byte-exact");
    size_t deterministic_entropy_baseline = budget.current;
    static const char deterministic_entropy_page[] =
        "<!doctype html><html><body></body></html>";
    static const char entropy_url[] =
        "https://example.test/path?q=%E2%9C%93#frag";
    static const char entropy_other_url[] =
        "https://example.test/other";
    const uint64_t entropy_seed = UINT64_C(0x0123456789abcdef);
    PocDocument deterministic_entropy_document;
    CHECK(document_parse(
        &deterministic_entropy_document, &budget,
        deterministic_entropy_page, strlen(deterministic_entropy_page), 17));
    ViewportContext deterministic_entropy_viewport;
    CHECK(viewport_context_init(
        &deterministic_entropy_viewport, 480, 272, 480, 272));

    /* These C vectors pin the v1 framing, endian order, scope tag, and final
       avalanche independently of any JavaScript string formatting. */
    char entropy_overlong_url[2049];
    memset(entropy_overlong_url, 'x', sizeof(entropy_overlong_url));
    memcpy(entropy_overlong_url, "https://example.test/", 21);
    entropy_overlong_url[sizeof(entropy_overlong_url) - 1] = '\0';
    CHECK(script_runtime_deterministic_entropy_state_v1(
              entropy_seed, entropy_url,
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL)
              == UINT64_C(0xe5f973356bb39186)
          && script_runtime_deterministic_entropy_state_v1(
              entropy_seed, entropy_url,
              SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME)
              == UINT64_C(0x0e0263c55aff884e)
          && script_runtime_deterministic_entropy_state_v1(
              entropy_seed, entropy_other_url,
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL)
              == UINT64_C(0xc13532515dd03e85)
          && script_runtime_deterministic_entropy_state_v1(
              0, NULL, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) == 0
          && script_runtime_deterministic_entropy_state_v1(
              0, "", SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) == 0
          && script_runtime_deterministic_entropy_state_v1(
              0, "https://example.test",
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) == 0
          && script_runtime_deterministic_entropy_state_v1(
              0, "file:///tmp/replay",
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) == 0
          && script_runtime_deterministic_entropy_state_v1(
              0, entropy_url, (ScriptDocumentScope) 99) == 0
          && script_runtime_deterministic_entropy_state_v1(
              0, entropy_overlong_url,
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL) == 0);

    ScriptRuntimeOptions invalid_replay_options = {
        .viewport = deterministic_entropy_viewport,
        .defer_document_scripts = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
    };
    ScriptResult invalid_replay_result;
    script_runtime_configure_deterministic_replay(true, entropy_seed);
    size_t invalid_replay_baseline = budget.current;
    ScriptRuntime *invalid_replay_runtime =
        script_runtime_create_configured(
            &deterministic_entropy_document, &budget, 4u * MIB, 1000,
            "https://example.test", &invalid_replay_options,
            &invalid_replay_result);
    CHECK(invalid_replay_runtime == NULL
          && strstr(invalid_replay_result.error,
                    "exact canonical HTTP(S)") != NULL
          && budget.current == invalid_replay_baseline);

    ScriptResult maximum_seed_result;
    script_runtime_configure_deterministic_replay(true, ULLONG_MAX);
    ScriptRuntime *maximum_seed_runtime = script_runtime_create_configured(
        &deterministic_entropy_document, &budget, 4u * MIB, 1000,
        "https://maximum-seed.test/", &invalid_replay_options,
        &maximum_seed_result);
    CHECK(maximum_seed_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              maximum_seed_runtime,
              "globalThis.pocSummary=Number.isFinite(Date.now())"
              "&&Date.now()<=8640000000000000?'SAFE-ORIGIN':'BAD-ORIGIN'",
              "<maximum-replay-seed>", &maximum_seed_result)
          && strcmp(maximum_seed_result.summary, "SAFE-ORIGIN") == 0);
    script_runtime_destroy(maximum_seed_runtime);

    char entropy_first[256] = {0};
    char entropy_repeat[256] = {0};
    char entropy_other[256] = {0};
    char entropy_child[256] = {0};
    static const char entropy_expected[] =
        "8a9695e646a54|eae9cebd39442a5da02e95ab39|"
        "0b2b3939-b393-4980-b97f-204ef0dd9220|92|d00a601d20868";
    CHECK(deterministic_entropy_probe(
              &deterministic_entropy_document, &budget,
              &deterministic_entropy_viewport, entropy_seed, entropy_url,
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
              entropy_first, sizeof(entropy_first))
          && strcmp(entropy_first, entropy_expected) == 0);
    /* Intervening URL and scope variants would perturb the old global
       runtime_sequence scheme. They must diverge locally without changing a
       later recreation of the original document realm. */
    CHECK(deterministic_entropy_probe(
              &deterministic_entropy_document, &budget,
              &deterministic_entropy_viewport, entropy_seed,
              entropy_other_url, SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
              entropy_other, sizeof(entropy_other))
          && strcmp(entropy_other, entropy_first) != 0
          && deterministic_entropy_probe(
              &deterministic_entropy_document, &budget,
              &deterministic_entropy_viewport, entropy_seed, entropy_url,
              SCRIPT_DOCUMENT_SCOPE_CHILD_FRAME,
              entropy_child, sizeof(entropy_child))
          && strcmp(entropy_child, entropy_first) != 0
          && deterministic_entropy_probe(
              &deterministic_entropy_document, &budget,
              &deterministic_entropy_viewport, entropy_seed, entropy_url,
              SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL,
              entropy_repeat, sizeof(entropy_repeat))
          && strcmp(entropy_repeat, entropy_expected) == 0);
    script_runtime_configure_deterministic_replay(false, 0);
    document_destroy(&deterministic_entropy_document);
    CHECK(budget.current == deterministic_entropy_baseline);

    puts("test: uncaught event listener isolation");
    size_t listener_baseline = budget.current;
    static const char listener_page[] =
        "<!doctype html><html><head><title>Listener isolation</title></head>"
        "<body><script>"
        "let sequence=[];"
        "document.addEventListener('DOMContentLoaded',()=>{"
        "sequence.push('first');throw new Error('expected listener failure')});"
        "document.addEventListener('DOMContentLoaded',()=>sequence.push('second'));"
        "addEventListener('load',()=>{console.error=()=>{"
        "throw new Error('expected reporter failure')};"
        "throw new Error('expected load failure')});"
        "addEventListener('load',()=>{sequence.push('load');"
        "globalThis.pocSummary=sequence.join(',')});"
        "</script></body></html>";
    PocDocument listener_document;
    CHECK(document_parse(&listener_document, &budget, listener_page,
                         strlen(listener_page), 17));
    ScriptResult listener_result;
    CHECK(scripts_run_document(&listener_document, &budget, 4 * MIB, 1000,
                               &listener_result));
    CHECK(listener_result.success
          && listener_result.dom_content_loaded_dispatched
          && strcmp(listener_result.summary, "first,second,load") == 0);
    document_destroy(&listener_document);
    CHECK(budget.current == listener_baseline);

    static const char script_failure_page[] =
        "<!doctype html><html><body>"
        "<script>throw new Error('expected author failure')</script>"
        "<script>globalThis.pocSummary='continued'</script>"
        "</body></html>";
    PocDocument script_failure_document;
    CHECK(document_parse(&script_failure_document, &budget,
                         script_failure_page, strlen(script_failure_page),
                         17));
    ScriptResult script_failure_result;
    CHECK(scripts_run_document(&script_failure_document, &budget, 4 * MIB,
                               1000, &script_failure_result));
    CHECK(script_failure_result.success
          && script_failure_result.scripts_failed == 1
          && script_failure_result.scripts_evaluated == 1
          && strcmp(script_failure_result.summary, "continued") == 0);
    document_destroy(&script_failure_document);
    CHECK(budget.current == listener_baseline);

    TileCache cache;
    puts("test: tiles");
    CHECK(tile_cache_init(&cache, &budget, &layout, 12));
    uint16_t *frame = budget_malloc(&budget, 480u * 272u * sizeof(*frame));
    CHECK(frame != NULL);
    CHECK(tile_cache_set_frame(&cache, frame, 480u * 272u));
    puts("test: frame zero");
    CHECK(tile_cache_render_frame(&cache, 0, 480, 272, NULL));
    puts("test: frame scrolled");
    CHECK(tile_cache_render_frame(&cache, 128, 480, 272, NULL));
    CHECK(tile_cache_render_frame(&cache, 0, 480, 272, NULL));
    puts("test: frame-aware eight-tile admission avoids cyclic thrash");
    TileCache tight_cache;
    uint16_t *tight_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*tight_frame));
    CHECK(tight_frame != NULL
          && tile_cache_init(&tight_cache, &budget, &layout, 8)
          && tight_cache.glyph_cache_capacity == TILEFINCH_GLYPH_CACHE_ENTRIES
          && tile_cache_set_frame(&tight_cache, tight_frame, 480u * 272u)
          && tile_cache_render_frame(&tight_cache, 0, 480, 272, NULL)
          && memcmp(tight_frame, frame,
                    480u * 272u * sizeof(*tight_frame)) == 0);
    size_t tight_hits = tight_cache.hits;
    size_t tight_misses = tight_cache.misses;
    size_t tight_rasterized = tight_cache.rasterized;
    CHECK(tile_cache_render_frame(&tight_cache, 0, 480, 272, NULL)
          && memcmp(tight_frame, frame,
                    480u * 272u * sizeof(*tight_frame)) == 0
          && tight_cache.hits - tight_hits == 8
          && tight_cache.misses - tight_misses == 4
          && tight_cache.rasterized - tight_rasterized == 4
          && tight_cache.frame_preserved_tiles >= 8
          && tight_cache.glyph_cache_bytes <= TILEFINCH_GLYPH_CACHE_BYTES);
    tile_cache_destroy(&tight_cache);
    budget_free(&budget, tight_frame);
    size_t valid_before_damage = 0;
    for (size_t i = 0; i < cache.tile_capacity; i++) {
        if (cache.tiles[i].valid) valid_before_damage++;
    }
    size_t rasterized_before_invalidation = cache.rasterized;
    CHECK(tile_cache_replace_layout_damage(&cache, &layout, 0, 0, 64, 64));
    size_t valid_after_damage = 0;
    for (size_t i = 0; i < cache.tile_capacity; i++) {
        if (cache.tiles[i].valid) valid_after_damage++;
    }
    CHECK(tile_cache_render_frame(&cache, 0, 480, 272, NULL)
          && cache.invalidations == 1
          && valid_before_damage == 12
          && valid_after_damage == valid_before_damage - 1
          && cache.rasterized == rasterized_before_invalidation + 1
          && cache.raster_us > 0 && cache.frame_us > 0
          && cache.frames_rendered == 4);
    render_paint_focus_outline(frame, 480u * 272u, 480, 272,
                               10, 10, 20, 12);
    CHECK(frame[8u * 480u + 8u] == test_rgb565(0xffffff)
          && frame[9u * 480u + 9u] == test_rgb565(0x0a84ff));
    render_paint_authored_focus_outline(
        frame, 480u * 272u, 480, 272, 50, 20, 30, 16,
        2, 3, STYLE_OUTLINE_SOLID, 0x123456, 255);
    CHECK(frame[15u * 480u + 45u] == test_rgb565(0x123456)
          && frame[16u * 480u + 46u] == test_rgb565(0x123456));
    puts("test: tile teardown");
    CHECK(cache.misses > 0 && cache.rasterized == cache.misses
          && layout.spatial_band_count > 0
          && cache.command_candidates
             < cache.rasterized * layout.count);
    puts("test: cancellable bounded render work");
    cache.idle_glyph_warming_disabled = false;
    tile_cache_schedule_prefetch_row(&cache, 400, 480);
    CHECK(tile_cache_idle_work_pending(&cache)
          && tile_cache_run_idle_work(&cache, 1, 1)
          && cache.idle_jobs_scheduled == 1
          && cache.idle_slices == 1
          && cache.idle_units == 1);
    tile_cache_cancel_idle_work(&cache);
    CHECK(!tile_cache_idle_work_pending(&cache)
          && cache.idle_jobs_cancelled == 1);
    tile_cache_schedule_prefetch_row(&cache, 528, 480);
    while (tile_cache_idle_work_pending(&cache)) {
        (void) tile_cache_run_idle_work(&cache, 10000, 16);
    }
    CHECK(cache.idle_jobs_scheduled == 2
          && cache.idle_jobs_completed == 1
          && cache.prefetch_rows == 1
          /* A prior frame can already have admitted every eligible glyph,
             and an off-document row legitimately has no glyph candidates.
             The scheduler's bounded completion does not require a miss. */
          && cache.max_idle_unit_us <= cache.max_idle_slice_us);
    puts("test: startup visual budget hands its exact cursor to idle work");
    tile_cache_schedule_prefetch_row(&cache, 0, 480);
    CHECK(tile_cache_idle_work_pending(&cache));
    /* Force more than the startup helper's 32-unit allowance while retaining
       the same target identity that a real scheduled row would use. */
    cache.idle_work.last_tile_x = cache.idle_work.next_tile_x + 63;
    size_t startup_cancelled_before = cache.idle_jobs_cancelled;
    size_t startup_scheduled_before = cache.idle_jobs_scheduled;
    tile_cache_prepare_startup_visuals(
        &cache, 0, 480, UINT64_MAX / 2u, 1);
    CHECK(tile_cache_idle_work_pending(&cache)
          && !cache.idle_work.startup
          && !cache.idle_work.allow_large_images
          && cache.idle_jobs_cancelled == startup_cancelled_before
          && cache.idle_jobs_scheduled == startup_scheduled_before);
    RenderIdleWork startup_cursor = cache.idle_work;
    (void) tile_cache_run_idle_work(&cache, UINT64_MAX / 2u, 1);
    CHECK(cache.idle_jobs_scheduled == startup_scheduled_before
          && cache.idle_jobs_cancelled == startup_cancelled_before
          && memcmp(
                 &startup_cursor, &cache.idle_work,
                 sizeof(startup_cursor)) != 0);
    tile_cache_cancel_idle_work(&cache);
    puts("test: bounded foreground frame preparation");
    uint16_t *stable_frame = budget_malloc(
        &budget, 480u * 272u * sizeof(*stable_frame));
    CHECK(stable_frame != NULL
          && tile_cache_render_frame(&cache, 0, 480, 200, NULL));
    memcpy(stable_frame, frame, 480u * 272u * sizeof(*stable_frame));
    tile_cache_invalidate_rect(&cache, 0, 0, 480, 272);
    CHECK(tile_cache_prepare_frame_bounded(
              &cache, 0, 480, 272, UINT64_MAX / 2u, 1)
              == RENDER_FRAME_WORK_PENDING
          && tile_cache_frame_work_pending(&cache)
          && cache.frame_job_units == 1
          && memcmp(stable_frame, frame,
                    480u * 272u * sizeof(*stable_frame)) == 0);
    TilefinchCancellation frame_cancellation;
    tilefinch_cancellation_init(&frame_cancellation);
    tilefinch_cancellation_request(&frame_cancellation);
    CHECK(tile_cache_prepare_frame_bounded_cancelable(
              &cache, 0, 480, 272, UINT64_MAX / 2u, 1,
              &frame_cancellation)
              == RENDER_FRAME_WORK_CANCELLED
          && !tile_cache_frame_work_pending(&cache)
          && memcmp(stable_frame, frame,
                    480u * 272u * sizeof(*stable_frame)) == 0);
    CHECK(tile_cache_prepare_frame_bounded(
              &cache, 0, 480, 272, UINT64_MAX / 2u, 1)
              == RENDER_FRAME_WORK_PENDING);
    size_t frame_job_cancellations = cache.frame_jobs_cancelled;
    RenderFrameWorkResult replaced_frame_job =
        tile_cache_prepare_frame_bounded(
            &cache, 0, 480, 200, UINT64_MAX / 2u, 1);
    CHECK(replaced_frame_job == RENDER_FRAME_WORK_PENDING);
    CHECK(cache.frame_jobs_cancelled > frame_job_cancellations);
    while (tile_cache_frame_work_pending(&cache)) {
        CHECK(tile_cache_prepare_frame_bounded(
                  &cache, 0, 480, 200, 10000, 2)
                  != RENDER_FRAME_WORK_FAILED);
    }
    CHECK(cache.frame_work.ready
          && cache.frame_jobs_completed == 1
          && tile_cache_render_frame(&cache, 0, 480, 200, NULL)
          && !cache.frame_work.ready
          && memcmp(stable_frame, frame,
                    480u * 200u * sizeof(*stable_frame)) == 0
          && cache.max_frame_job_unit_us
                 <= cache.max_frame_job_slice_us);
    budget_free(&budget, stable_frame);
    tile_cache_destroy(&cache);
    budget_free(&budget, frame);

    puts("test: detached control state is retired before node reclamation");
    size_t control_state_baseline = budget.current;
    static const char control_state_page[] =
        "<!doctype html><body><input id=retired></body>";
    PocDocument control_state_document = {0};
    CHECK(document_parse(
              &control_state_document, &budget, control_state_page,
              sizeof(control_state_page) - 1u, 11));
    lxb_dom_node_t *retired_control = find_id(
        lxb_dom_interface_node(control_state_document.html), "retired");
    CHECK(retired_control != NULL
          && document_control_value_set(
              &control_state_document, retired_control, "live", 4)
          && control_state_document.control_state_count == 1);
    lxb_dom_node_t *control_clone =
        lxb_dom_node_clone(retired_control, false);
    CHECK(control_clone != NULL
          && control_clone->user == retired_control->user);
    /* Lexbor copies node->user. Discarding that clone must only detach its
       borrowed pointer, never unlink or free the original node's state. */
    CHECK(document_control_state_discard_subtree(
              &control_state_document, control_clone)
          && control_clone->user == NULL
          && control_state_document.control_state_count == 1);
    size_t original_value_length = 0;
    CHECK(strcmp(document_control_value(
                     retired_control, &original_value_length), "live") == 0
          && original_value_length == 4);
    lxb_dom_node_destroy_deep(control_clone);
    lxb_dom_node_remove(retired_control);
    CHECK(document_control_state_discard_subtree(
        &control_state_document, retired_control));
    CHECK(control_state_document.control_state_count == 0
          && retired_control->user == NULL);
    lxb_dom_node_destroy_deep(retired_control);
    document_destroy(&control_state_document);
    CHECK(budget.current == control_state_baseline);

    puts("test: template-owned control state is retired with its host");
    static const char template_control_state_page[] =
        "<!doctype html><body><template id=owner><input></template></body>";
    PocDocument template_control_document = {0};
    CHECK(document_parse(
              &template_control_document, &budget,
              template_control_state_page,
              sizeof(template_control_state_page) - 1u, 13));
    lxb_dom_node_t *template_node = find_id(
        lxb_dom_interface_node(template_control_document.html), "owner");
    CHECK(template_node != NULL && template_node->ns == LXB_NS_HTML);
    lxb_html_template_element_t *template_element =
        lxb_html_interface_template(template_node);
    lxb_dom_node_t *template_input =
        template_element->content == NULL ? NULL
        : lxb_dom_interface_node(template_element->content)->first_child;
    CHECK(template_input != NULL
          && document_control_value_set(
              &template_control_document, template_input, "inside", 6)
          && template_control_document.control_state_count == 1);
    lxb_dom_node_remove(template_node);
    CHECK(document_control_state_discard_subtree(
              &template_control_document, template_node)
          && template_control_document.control_state_count == 0
          && template_input->user == NULL);
    lxb_dom_node_destroy_deep(template_node);
    document_destroy(&template_control_document);
    CHECK(budget.current == control_state_baseline);

    puts("test: malformed-table controls retain parser form ownership");
    static const char parser_form_page[] =
        "<!doctype html><body><table><form id=parser-owner>"
        "<tr><td><select id=parser-select></select></tr>"
        "<tr><td><input id=parser-input></tr></form></table></body>";
    PocDocument parser_form_document = {0};
    CHECK(document_parse(
              &parser_form_document, &budget, parser_form_page,
              sizeof(parser_form_page) - 1u, 17));
    lxb_dom_node_t *parser_form_root = lxb_dom_interface_node(
        parser_form_document.html);
    lxb_dom_node_t *parser_form =
        find_id(parser_form_root, "parser-owner");
    lxb_dom_node_t *parser_select =
        find_id(parser_form_root, "parser-select");
    lxb_dom_node_t *parser_input =
        find_id(parser_form_root, "parser-input");
    CHECK(parser_form != NULL && parser_select != NULL
          && parser_input != NULL
          && parser_select->parent != parser_form
          && parser_input->parent != parser_form
          && document_control_parser_form_owner(parser_select) == parser_form
          && document_control_parser_form_owner(parser_input) == parser_form
          && parser_form_document.parser_form_owner_count == 2
          && document_has_parser_form_owners(&parser_form_document));
    lxb_dom_node_remove(parser_form);
    CHECK(document_control_state_discard_subtree(
              &parser_form_document, parser_form)
          && document_control_parser_form_owner(parser_select) == NULL
          && document_control_parser_form_owner(parser_input) == NULL
          && parser_form_document.parser_form_owner_count == 0
          && !document_has_parser_form_owners(&parser_form_document));
    lxb_dom_node_destroy_deep(parser_form);
    document_destroy(&parser_form_document);
    CHECK(budget.current == control_state_baseline);

    puts("test: cache gone");
    layout_destroy(&layout);
    puts("test: layout gone");
    stylesheet_destroy(&sheet);
    puts("test: style gone");
    document_destroy(&document);
    puts("test: document gone");
    CHECK(budget.current == 0);

    return 0;
}
