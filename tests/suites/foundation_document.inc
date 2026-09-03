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

    puts("test: streamed EventSource and bounded WebSocket lifecycle");
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
    ScriptRuntimeOptions live_network_options = {
        .allow_test_network_primitive_overrides = true
    };
    CHECK(viewport_context_init(
        &live_network_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *live_network_runtime = script_runtime_create_configured(
        &live_network_document, &budget, 5 * MIB, 1000,
        "https://events.test/page", &live_network_options,
        &live_network_result);
    static const char live_network_source[] =
        "(()=>{const seen=[];let socketClosed=false,sourceDone=false,"
        "socketChecks=true,socketSends=0;"
        "const mark=value=>{seen.push(value);globalThis.pocSummary="
        "'live-network:seen:'+seen.join('|');};"
        "const finish=()=>{if(socketClosed&&sourceDone)"
        "globalThis.pocSummary=socketChecks?'live-network-ok':'socket-bad';};"
        "const source=new EventSource('/feed');"
        "source.onopen=()=>mark('open');"
        "source.addEventListener('status',event=>mark("
        "'status:'+event.data+':'+event.lastEventId));"
        "source.onmessage=event=>{mark('message:'+event.data+':'"
        "+event.lastEventId);sourceDone=true;source.close();finish();};"
        "source.onerror=()=>{if(!sourceDone)mark('source-error');};"
        "__tilefinchWebSocketStart=(url,protocols)=>{socketChecks&&="
        "url==='wss://events.test/socket'&&protocols==='chat';return 77;};"
        "__tilefinchWebSocketSend=(id,buffer,binary)=>{const bytes="
        "new Uint8Array(buffer),first=socketSends++===0;socketChecks&&="
        "id===77&&(first?(!binary&&new TextDecoder().decode(buffer)==="
        "'bird'):(binary&&bytes.join(',')==='1,2,3'));queueMicrotask(()=>{"
        "__tilefinchDeliverWebSocket(77,'drain',bytes.length,'',0,false);"
        "if(first)__tilefinchDeliverWebSocket(77,'text',new TextEncoder()"
        ".encode('echo:bird').buffer,'',0,false);else "
        "__tilefinchDeliverWebSocket(77,'binary',new Uint8Array([3,2,1])"
        ".buffer,'',0,false);});return true;};"
        "__tilefinchWebSocketClose=(id,code,reason)=>{socketChecks&&="
        "id===77&&code===1000&&reason==='done';queueMicrotask(()=>"
        "__tilefinchDeliverWebSocket(77,'close',undefined,'done',1000,true));"
        "return true;};"
        "const socket=new WebSocket('wss://events.test/socket',['chat']);"
        "socket.onopen=()=>{socketChecks&&=socket.protocol==='chat';"
        "socket.send('bird');socketChecks&&=socket.bufferedAmount===4;};"
        "socket.onmessage=event=>{if(typeof event.data==='string'){"
        "socketChecks&&=event.data==='echo:bird'&&socket.bufferedAmount===0;"
        "socket.binaryType='arraybuffer';socket.send(new Uint8Array([1,2,3]));"
        "socketChecks&&=socket.bufferedAmount===3;}else{socketChecks&&="
        "event.data instanceof ArrayBuffer&&new Uint8Array(event.data)"
        ".join(',')==='3,2,1'&&socket.bufferedAmount===0;"
        "socket.close(1000,'done');}};"
        "socket.onclose=event=>{socketClosed=event.code===1000"
        "&&event.wasClean&&event.reason==='done';finish();};"
        "queueMicrotask(()=>__tilefinchDeliverWebSocket("
        "77,'open',undefined,'chat',0,false));"
        "let invalidClose=false;try{new WebSocket("
        "'wss://events.test/socket#bad');}"
        "catch(error){invalidClose=error instanceof DOMException"
        "&&error.name==='SyntaxError';}"
        "let invalidCode=false;try{socket.close(NaN);}catch(error){"
        "invalidCode=error instanceof DOMException&&"
        "error.name==='InvalidAccessError';}"
        "let iteratorClosed=false,nexts=0,bounded=false;try{new WebSocket("
        "'wss://events.test/overflow',{[Symbol.iterator](){return{next(){"
        "nexts++;return{value:'p'+nexts,done:false};},return(){iteratorClosed="
        "true;return{done:true};}};}});}catch(error){bounded=error instanceof "
        "DOMException&&error.name==='SyntaxError';}socketChecks&&="
        "invalidClose&&invalidCode&&bounded&&iteratorClosed&&nexts===17;"
        "if(!socketChecks)mark('socket-validation-failed');"
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
                "live-network-ok") != 0;
         tick++) {
        CHECK(script_runtime_advance(
            live_network_runtime, 1, 4, &live_network_result));
    }
    if (strcmp(
            live_network_result.summary,
            "live-network-ok") != 0) {
        fprintf(stderr, "live network summary=%s error=%s trace=%s\n",
                live_network_result.summary, live_network_result.error,
                live_network_trace_error);
    }
    CHECK(strcmp(
        live_network_result.summary,
        "live-network-ok") == 0);
    script_runtime_destroy(live_network_runtime);
    document_destroy(&live_network_document);
    fetch_trace_end();
    CHECK(budget.current == live_network_baseline);

    puts("test: installed-game multiplayer channel lifecycle");
    size_t multiplayer_baseline = budget.current;
    static const char multiplayer_page[] =
        "<!doctype html><title>Installed game</title><body></body>";
    PocDocument multiplayer_document = {0};
    ScriptResult multiplayer_result = {0};
    CHECK(document_parse(
        &multiplayer_document, &budget, multiplayer_page,
        strlen(multiplayer_page), 17));
    ScriptRuntimeOptions multiplayer_options = {
        .allow_test_network_primitive_overrides = true
    };
    CHECK(viewport_context_init(
        &multiplayer_options.viewport, 480, 272, 480, 272));
    ScriptRuntime *multiplayer_runtime = script_runtime_create_configured(
        &multiplayer_document, &budget, 5 * MIB, 1000,
        "https://tilefinch.local/offline/app?id=7", &multiplayer_options,
        &multiplayer_result);
    static const char multiplayer_source[] =
        "(()=>{let good=true,sends=0;"
        "__tilefinchMultiplayerStart=(mode,game,name,code)=>{good&&="
        "mode==='host'&&game==='treadline-arena'&&name==='Bird'&&!code;"
        "return 91;};"
        "__tilefinchMultiplayerSend=(id,buffer,binary)=>{const bytes="
        "new Uint8Array(buffer);good&&=id===91&&!binary&&"
        "new TextDecoder().decode(buffer)==='hello';sends++;"
        "queueMicrotask(()=>{__tilefinchDeliverMultiplayer(91,'drain',"
        "bytes.length,'',0,false,0,'','');"
        "__tilefinchDeliverMultiplayer(91,'binary',new Uint8Array([4,5])"
        ".buffer,'',0,false,0,'','');});return true;};"
        "__tilefinchMultiplayerAccept=(id,peer,yes)=>"
        "(good&&=id===91&&peer===7&&yes,true);"
        "__tilefinchMultiplayerAddRemoteCode=(id,code)=>"
        "(good&&=id===91&&code==='123456789012',true);"
        "__tilefinchMultiplayerClose=(id,code,reason)=>{good&&="
        "id===91&&code===1000&&reason==='done';queueMicrotask(()=>"
        "__tilefinchDeliverMultiplayer(91,'close',undefined,'done',1000,"
        "true,0,'',''));return true;};"
        "const channel=navigator.tilefinchMultiplayer.host({"
        "gameId:'treadline-arena',name:'Bird'});"
        "good&&=channel.readyState==='connecting'&&!channel.ordered&&"
        "channel.maxRetransmits===0&&navigator.tilefinchMultiplayer."
        "maxMessageSize===512;"
        "channel.oninvitecode=e=>{good&&=e.code==='123456789012';"
        "good&&=channel.addRemoteCode(e.code);};"
        "channel.onpeerrequest=e=>{good&&=e.peerIdentity===7&&"
        "e.name==='Finch';good&&=channel.accept(e.peerIdentity);};"
        "channel.onopen=()=>{channel.send('hello');good&&="
        "channel.bufferedAmount===5;};"
        "channel.onmessage=e=>{good&&=e.data instanceof ArrayBuffer&&"
        "new Uint8Array(e.data).join(',')==='4,5'&&"
        "channel.bufferedAmount===0&&sends===1;channel.close(1000,'done');};"
        "channel.onclose=e=>{globalThis.pocSummary=good&&e.wasClean&&"
        "e.code===1000?'multiplayer-ok':'multiplayer-bad';};"
        "queueMicrotask(()=>{__tilefinchDeliverMultiplayer(91,'invitecode',"
        "undefined,'direct',0,false,0,'123456789012','');"
        "__tilefinchDeliverMultiplayer(91,'peerrequest',undefined,'lan',0,"
        "false,7,'','Finch');__tilefinchDeliverMultiplayer(91,'open',"
        "undefined,'',0,false,7,'','Finch');});"
        "globalThis.pocSummary='multiplayer-pending';})()";
    CHECK(multiplayer_runtime != NULL && multiplayer_result.success
          && script_runtime_evaluate_diagnostic(
                 multiplayer_runtime, multiplayer_source,
                 "<multiplayer-probe>", &multiplayer_result));
    for (size_t tick = 0;
         tick < 16u
         && strcmp(multiplayer_result.summary, "multiplayer-ok") != 0;
         tick++) {
        CHECK(script_runtime_advance(
            multiplayer_runtime, 1, 4, &multiplayer_result));
    }
    CHECK(strcmp(multiplayer_result.summary, "multiplayer-ok") == 0);
    script_runtime_destroy(multiplayer_runtime);
    document_destroy(&multiplayer_document);
    CHECK(budget.current == multiplayer_baseline);

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

    puts("test: ordinary high-resolution time is reduced for non-isolated pages");
    size_t reduced_clock_baseline = budget.current;
    static const char reduced_clock_page[] =
        "<!doctype html><html><body><script>"
        "let aligned=true,monotonic=true,last=performance.now();"
        "for(let i=0;i<512;i++){const value=performance.now(),units=value*10;"
        "if(Math.abs(units-Math.round(units))>1e-7)aligned=false;"
        "if(value<last)monotonic=false;last=value;}"
        "globalThis.pocSummary=aligned&&monotonic"
        "&&globalThis.crossOriginIsolated===false"
        "?'REDUCED-CLOCK-OK':'REDUCED-CLOCK-FAILED';"
        "</script></body></html>";
    PocDocument reduced_clock_document;
    CHECK(document_parse(&reduced_clock_document, &budget,
                         reduced_clock_page,
                         strlen(reduced_clock_page), 17));
    ScriptResult reduced_clock_result;
    CHECK(scripts_run_document_at(
              &reduced_clock_document, &budget, 4 * MIB, 1000,
              "https://clock-precision.test/", &reduced_clock_result)
          && reduced_clock_result.success
          && strcmp(reduced_clock_result.summary,
                    "REDUCED-CLOCK-OK") == 0);
    document_destroy(&reduced_clock_document);
    CHECK(budget.current == reduced_clock_baseline);

    puts("test: WebAssembly exported memory stays coherent across calls");
    size_t wasm_memory_baseline = budget.current;
    static const char wasm_memory_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,11,2,96,1,127,1,127,96,2,127,127,0,"
        "3,3,2,0,1,5,4,1,1,1,2,7,25,3,6,109,101,109,111,114,121,2,0,"
        "4,108,111,97,100,0,0,5,115,116,111,114,101,0,1,10,19,2,7,0,"
        "32,0,45,0,0,11,9,0,32,0,32,1,58,0,0,11]);"
        "const invalid=Uint8Array.from([0,97,115,109,0,0,0,0]),"
        "syncModule=new WebAssembly.Module(bytes),"
        "shape=WebAssembly.validate(bytes)&&!WebAssembly.validate(invalid)"
        "&&syncModule instanceof WebAssembly.Module"
        "&&Object.prototype.toString.call(syncModule)==='[object WebAssembly.Module]'"
        "&&Object.prototype.toString.call(WebAssembly)==='[object WebAssembly]';"
        "Promise.all([WebAssembly.compile(bytes),"
        "WebAssembly.instantiate(syncModule),WebAssembly.instantiate(bytes),"
        "WebAssembly.compile(invalid).then(()=>false,"
        "error=>error instanceof WebAssembly.CompileError)]).then(values=>{"
        "const compiled=values[0],instance=values[1],pair=values[2],"
        "memory=new Uint8Array(instance.exports.memory.buffer);"
        "memory[9]=42;const fromScript=instance.exports.load(9);"
        "instance.exports.store(10,77);"
        "const storedBeforeGrow=memory[10];"
        "const oldBuffer=instance.exports.memory.buffer,oldPages=instance.exports.memory.grow(1),"
        "grown=instance.exports.memory.buffer;"
        "const api=shape&&compiled instanceof WebAssembly.Module"
        "&&instance instanceof WebAssembly.Instance"
        "&&instance.exports.memory instanceof WebAssembly.Memory"
        "&&oldPages===1&&oldBuffer.byteLength===0&&grown.byteLength===131072"
        "&&pair.module instanceof WebAssembly.Module"
        "&&pair.instance instanceof WebAssembly.Instance&&values[3];"
        "globalThis.pocSummary=api&&fromScript===42&&storedBeforeGrow===77"
        "?'WASM-MEMORY-COHERENT':'WASM-MEMORY-STALE';"
        "},error=>{globalThis.pocSummary='WASM-MEMORY-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_memory_document;
    CHECK(document_parse(&wasm_memory_document, &budget,
                         wasm_memory_page, strlen(wasm_memory_page), 17));
    ScriptResult wasm_memory_result;
    CHECK(scripts_run_document_at(
              &wasm_memory_document, &budget, 8 * MIB, 2000,
              "https://wasm-memory.test/", &wasm_memory_result)
          && wasm_memory_result.success
          && strcmp(wasm_memory_result.summary,
                    "WASM-MEMORY-COHERENT") == 0);
    document_destroy(&wasm_memory_document);
    CHECK(budget.current == wasm_memory_baseline);

    puts("test: exported WebAssembly memory is not charged to the realm heap");
    size_t wasm_large_memory_baseline = budget.current;
    static const char wasm_large_memory_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "5,3,1,0,16,7,5,1,1,109,2,0]);"
        "WebAssembly.instantiate(bytes).then(value=>{"
        "const memory=value.instance.exports.m,view=new Uint8Array(memory.buffer);"
        "view[0]=37;view[view.length-1]=91;"
        "globalThis.pocSummary=memory instanceof WebAssembly.Memory"
        "&&view.length===1048576&&view[0]===37&&view[view.length-1]===91"
        "?'WASM-LARGE-MEMORY-OK':'WASM-LARGE-MEMORY-FAILED';"
        "},error=>{globalThis.pocSummary='WASM-LARGE-MEMORY-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_large_memory_document;
    CHECK(document_parse(&wasm_large_memory_document, &budget,
                         wasm_large_memory_page,
                         strlen(wasm_large_memory_page), 17));
    ScriptResult wasm_large_memory_result;
    CHECK(scripts_run_document_at(
              &wasm_large_memory_document, &budget, 2 * MIB, 2000,
              "https://wasm-large-memory.test/", &wasm_large_memory_result)
          && wasm_large_memory_result.success
          && strcmp(wasm_large_memory_result.summary,
                    "WASM-LARGE-MEMORY-OK") == 0);
    document_destroy(&wasm_large_memory_document);
    CHECK(budget.current == wasm_large_memory_baseline);

    puts("test: fixed-width WebAssembly SIMD follows the standard binary format");
    size_t wasm_simd_baseline = budget.current;
    static const char wasm_simd_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,2,1,0,"
        "7,8,1,4,115,105,109,100,0,0,10,25,1,23,0,253,12,"
        "1,0,0,0,2,0,0,0,3,0,0,0,4,0,0,0,253,27,2,11]);"
        "WebAssembly.instantiate(bytes).then(value=>{"
        "globalThis.pocSummary=WebAssembly.validate(bytes)"
        "&&value.instance.exports.simd()===3"
        "?'WASM-SIMD-OK':'WASM-SIMD-FAILED';"
        "},error=>{globalThis.pocSummary='WASM-SIMD-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_simd_document;
    CHECK(document_parse(&wasm_simd_document, &budget,
                         wasm_simd_page, strlen(wasm_simd_page), 17));
    ScriptResult wasm_simd_result;
    CHECK(scripts_run_document_at(
              &wasm_simd_document, &budget, 6 * MIB, 2000,
              "https://wasm-simd.test/", &wasm_simd_result)
          && wasm_simd_result.success
          && strcmp(wasm_simd_result.summary, "WASM-SIMD-OK") == 0);
    document_destroy(&wasm_simd_document);
    CHECK(budget.current == wasm_simd_baseline);

    puts("test: bounded WebAssembly surface follows standard module APIs");
    size_t wasm_surface_baseline = budget.current;
    static const char wasm_surface_page[] =
        "<!doctype html><html><body><script>"
        "const base=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,11,2,96,1,127,1,127,96,2,127,127,0,"
        "3,3,2,0,1,5,4,1,1,1,2,7,25,3,6,109,101,109,111,114,121,2,0,"
        "4,108,111,97,100,0,0,5,115,116,111,114,101,0,1,10,19,2,7,0,"
        "32,0,45,0,0,11,9,0,32,0,32,1,58,0,0,11]),"
        "custom=new Uint8Array(base.length+7);custom.set(base);"
        "custom.set([0,5,1,120,1,2,3],base.length);"
        "const module=new WebAssembly.Module(custom),"
        "imports=WebAssembly.Module.imports(module),"
        "exports=WebAssembly.Module.exports(module),"
        "sections=WebAssembly.Module.customSections(module,'x'),"
        "memory=new WebAssembly.Memory({initial:1,maximum:2}),"
        "emptyMemory=new WebAssembly.Memory({initial:0,maximum:1}),"
        "emptyOld=emptyMemory.buffer,emptyPages=emptyMemory.grow(1),"
        "zeroOld=memory.buffer,zeroPages=memory.grow(0),zeroNew=memory.buffer,"
        "old=zeroNew,oldPages=memory.grow(1),"
        "table=new WebAssembly.Table({element:'externref',initial:1,maximum:2}),"
        "global=new WebAssembly.Global({value:'i64',mutable:true},7n);"
        "table.set(0,global);table.grow(1,null);global.value=9n;"
        "const response=new Response(base,{headers:{'Content-Type':'application/wasm'}});"
        "Promise.all([WebAssembly.compileStreaming(response),"
        "WebAssembly.instantiateStreaming(new Response(base,{headers:{'Content-Type':'application/wasm; charset=binary'}})),"
        "WebAssembly.compileStreaming(new Response(base,{headers:{'Content-Type':'text/plain'}})).then(()=>false,e=>e instanceof TypeError),"
        "WebAssembly.compile(new Uint8Array()).then(()=>false,e=>e instanceof WebAssembly.CompileError),"
        "WebAssembly.compileStreaming({ok:true,headers:{get(){return 'application/wasm';}},arrayBuffer(){return Promise.resolve(base);}}).then(()=>false,e=>e instanceof TypeError)]).then(values=>{"
        "const section=new Uint8Array(sections[0]),"
        "okay=imports.length===0&&exports.length===3"
        "&&exports[0].name==='memory'&&exports[0].kind==='memory'"
        "&&sections.length===1&&section.join(',')==='1,2,3'"
        "&&emptyPages===0&&emptyOld.byteLength===0"
        "&&emptyMemory.buffer.byteLength===65536"
        "&&zeroPages===1&&zeroOld.byteLength===0&&zeroNew!==zeroOld"
        "&&memory.buffer.byteLength===131072&&old.byteLength===0&&oldPages===1"
        "&&table.length===2&&table.get(0)===global&&global.value===9n"
        "&&values[0] instanceof WebAssembly.Module"
        "&&values[1].instance instanceof WebAssembly.Instance"
        "&&values[2]&&values[3]&&values[4];"
        "globalThis.pocSummary=okay?'WASM-STANDARD-SURFACE-OK':'WASM-STANDARD-SURFACE-FAILED';"
        "},e=>{globalThis.pocSummary='WASM-STANDARD-SURFACE-ERROR:'+e;});"
        "</script></body></html>";
    PocDocument wasm_surface_document;
    CHECK(document_parse(&wasm_surface_document, &budget,
                         wasm_surface_page, strlen(wasm_surface_page), 17));
    ScriptResult wasm_surface_result;
    CHECK(scripts_run_document_at(
              &wasm_surface_document, &budget, 10 * MIB, 3000,
              "https://wasm-surface.test/", &wasm_surface_result)
          && wasm_surface_result.success
          && strcmp(wasm_surface_result.summary,
                    "WASM-STANDARD-SURFACE-OK") == 0);
    document_destroy(&wasm_surface_document);
    CHECK(budget.current == wasm_surface_baseline);

    puts("test: exported WebAssembly globals preserve type and mutability");
    size_t wasm_global_baseline = budget.current;
    static const char wasm_global_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,5,1,96,0,1,126,3,2,1,0,"
        "6,11,2,127,0,65,7,11,126,1,66,9,11,"
        "7,15,3,1,103,3,0,1,109,3,1,3,103,101,116,0,0,"
        "10,6,1,4,0,35,1,11]);"
        "WebAssembly.instantiate(bytes).then(value=>{"
        "const g=value.instance.exports.g,m=value.instance.exports.m;"
        "let immutable=false;try{g.value=8;}catch(error){immutable=error instanceof TypeError;}"
        "m.value=11n;"
        "globalThis.pocSummary=g instanceof WebAssembly.Global"
        "&&m instanceof WebAssembly.Global&&g.value===7&&immutable"
        "&&m.value===11n&&value.instance.exports.get()===11n"
        "&&!Object.hasOwn(value.instance,'exports')"
        "&&Object.getOwnPropertyDescriptor(WebAssembly.Instance.prototype,'exports').enumerable"
        "&&Object.keys(WebAssembly).sort().join(',')==='compile,compileStreaming,instantiate,instantiateStreaming,validate'"
        "?'WASM-GLOBAL-OK':'WASM-GLOBAL-FAILED';"
        "},error=>{globalThis.pocSummary='WASM-GLOBAL-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_global_document;
    CHECK(document_parse(&wasm_global_document, &budget,
                         wasm_global_page, strlen(wasm_global_page), 17));
    ScriptResult wasm_global_result;
    CHECK(scripts_run_document_at(
              &wasm_global_document, &budget, 8 * MIB, 2000,
              "https://wasm-global.test/", &wasm_global_result)
          && wasm_global_result.success
          && strcmp(wasm_global_result.summary, "WASM-GLOBAL-OK") == 0);
    document_destroy(&wasm_global_document);
    CHECK(budget.current == wasm_global_baseline);

    puts("test: reentrant WebAssembly import getters retain the native runtime");
    size_t wasm_reentrant_baseline = budget.current;
    static const char wasm_reentrant_page[] =
        "<!doctype html><html><body><script>"
        "const empty=Uint8Array.from([0,97,115,109,1,0,0,0]),"
        "imported=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,2,13,2,1,109,1,97,0,0,1,109,1,98,0,0]),"
        "emptyModule=new WebAssembly.Module(empty);let reads=0;"
        "function reenter(){reads++;WebAssembly.validate(empty);"
        "new WebAssembly.Module(empty);new WebAssembly.Instance(emptyModule);}"
        "const namespace={};for(const name of ['a','b'])"
        "Object.defineProperty(namespace,name,{get(){reenter();return function(){};}});"
        "const imports={get m(){reenter();return namespace;}};"
        "const outer=new WebAssembly.Instance(new WebAssembly.Module(imported),imports);"
        "globalThis.pocSummary=outer instanceof WebAssembly.Instance&&reads===4"
        "?'WASM-REENTRANT-IMPORTS-OK':'WASM-REENTRANT-IMPORTS-FAILED:'+reads;"
        "</script></body></html>";
    PocDocument wasm_reentrant_document;
    CHECK(document_parse(&wasm_reentrant_document, &budget,
                         wasm_reentrant_page,
                         strlen(wasm_reentrant_page), 17));
    ScriptResult wasm_reentrant_result;
    CHECK(scripts_run_document_at(
              &wasm_reentrant_document, &budget, 8 * MIB, 2000,
              "https://wasm-reentrant.test/", &wasm_reentrant_result)
          && wasm_reentrant_result.success
          && strcmp(wasm_reentrant_result.summary,
                    "WASM-REENTRANT-IMPORTS-OK") == 0);
    document_destroy(&wasm_reentrant_document);
    CHECK(budget.current == wasm_reentrant_baseline);

    puts("test: WebAssembly JS API coercions and function metadata match the standard");
    size_t wasm_jsapi_baseline = budget.current;
    static const char wasm_jsapi_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,6,1,96,2,127,127,0,3,2,1,0,"
        "7,9,2,1,97,0,0,1,98,0,0,10,4,1,2,0,11]),"
        "instance=new WebAssembly.Instance(new WebAssembly.Module(bytes)),"
        "a=instance.exports.a,b=instance.exports.b,"
        "externTable=new WebAssembly.Table({element:'externref',initial:1}),"
        "funcTable=new WebAssembly.Table({element:'anyfunc',initial:1}),"
        "externGlobal=new WebAssembly.Global({value:'externref'}),"
        "funcGlobal=new WebAssembly.Global({value:'anyfunc'}),"
        "funcValueGlobal=new WebAssembly.Global({value:'anyfunc'},a),"
        "called=WebAssembly.CompileError('x',{cause:7}),"
        "built=new WebAssembly.CompileError('y'),"
        "Derived=class extends WebAssembly.CompileError{},derived=new Derived('z'),"
        "wrapped=new WebAssembly.Global({value:'i64'},'18446744073709551617');"
        "const order=[],orderedTableDescriptor={"
        "get element(){order.push('element');return 'externref';},"
        "get initial(){order.push('initial');return 0;},"
        "get maximum(){order.push('maximum');return 1;}},"
        "orderedGlobalDescriptor={"
        "get mutable(){order.push('mutable');return false;},"
        "get value(){order.push('value');return 'i32';}};"
        "new WebAssembly.Table(orderedTableDescriptor);"
        "new WebAssembly.Global(orderedGlobalDescriptor);"
        "funcTable.set(0,a);externTable.grow(1);"
        "let bigintRefused=false,nanRefused=false,funcrefRefused=false,"
        "i64NumberRefused=false,numericBigIntRefused=0,"
        "emptyCompileError=false,reads=0;"
        "new WebAssembly.Memory({get initial(){reads++;return 0;},"
        "get maximum(){reads++;return 1;}});"
        "try{new WebAssembly.Memory({initial:1n});}"
        "catch(error){bigintRefused=error instanceof TypeError;}"
        "try{new WebAssembly.Memory({initial:NaN});}"
        "catch(error){nanRefused=error instanceof TypeError;}"
        "try{new WebAssembly.Table({element:'funcref',initial:0});}"
        "catch(error){funcrefRefused=error instanceof TypeError;}"
        "try{new WebAssembly.Global({value:'i64'},1);}"
        "catch(error){i64NumberRefused=error instanceof TypeError;}"
        "for(const type of ['i32','f32','f64'])try{"
        "new WebAssembly.Global({value:type},1n);}catch(error){"
        "if(error instanceof TypeError)numericBigIntRefused++;}"
        "try{new WebAssembly.Module(new Uint8Array());}"
        "catch(error){emptyCompileError=error instanceof WebAssembly.CompileError;}"
        "const nameDescriptor=Object.getOwnPropertyDescriptor("
        "WebAssembly.CompileError.prototype,'name'),"
        "prototypeDescriptor=Object.getOwnPropertyDescriptor("
        "WebAssembly.CompileError,'prototype');"
        "globalThis.pocSummary=a===b&&a.length===2&&a.name==='0'"
        "&&externTable.get(0)===undefined&&externTable.get(1)===undefined"
        "&&funcTable.get(0)===a&&externGlobal.value===undefined"
        "&&funcGlobal.value===null&&funcValueGlobal.value===a"
        "&&called instanceof WebAssembly.CompileError"
        "&&built instanceof WebAssembly.CompileError"
        "&&derived instanceof Derived&&derived instanceof WebAssembly.CompileError"
        "&&Object.getPrototypeOf(derived)===Derived.prototype"
        "&&called.message==='x'&&called.cause===7"
        "&&!Object.hasOwn(called,'name')&&nameDescriptor"
        "&&!nameDescriptor.enumerable&&WebAssembly.CompileError.length===1"
        "&&prototypeDescriptor&&!prototypeDescriptor.writable"
        "&&wrapped.value===1n&&bigintRefused&&nanRefused&&funcrefRefused"
        "&&i64NumberRefused&&numericBigIntRefused===3"
        "&&emptyCompileError&&reads===2"
        "&&order.join(',')==='element,initial,maximum,mutable,value'"
        "?'WASM-JSAPI-OK':'WASM-JSAPI-FAILED';"
        "</script></body></html>";
    PocDocument wasm_jsapi_document;
    CHECK(document_parse(&wasm_jsapi_document, &budget,
                         wasm_jsapi_page, strlen(wasm_jsapi_page), 17));
    ScriptResult wasm_jsapi_result;
    CHECK(scripts_run_document_at(
              &wasm_jsapi_document, &budget, 8 * MIB, 2000,
              "https://wasm-jsapi.test/", &wasm_jsapi_result)
          && wasm_jsapi_result.success
          && strcmp(wasm_jsapi_result.summary, "WASM-JSAPI-OK") == 0);
    document_destroy(&wasm_jsapi_document);
    CHECK(budget.current == wasm_jsapi_baseline);

    puts("test: WebAssembly descriptor and Module-overload timing follow WPT");
    size_t wasm_wpt_baseline = budget.current;
    static const char wasm_wpt_page[] =
        "<!doctype html><html><body><script>"
        "const descriptorOrder=[],memoryDescriptor=function(){};"
        "Object.defineProperties(memoryDescriptor,{"
        "address:{get(){descriptorOrder.push('ma');return {toString(){descriptorOrder.push('mas');return 'i32';}};}},"
        "initial:{get(){descriptorOrder.push('mi');return {valueOf(){descriptorOrder.push('miv');return 0;}};}},"
        "maximum:{get(){descriptorOrder.push('mm');return {valueOf(){descriptorOrder.push('mmv');return 1;}};}},"
        "shared:{get(){descriptorOrder.push('ms');return false;}}});"
        "new WebAssembly.Memory(memoryDescriptor);"
        "const tableDescriptor=function(){};Object.defineProperties(tableDescriptor,{"
        "element:{get(){descriptorOrder.push('te');return {toString(){descriptorOrder.push('tes');return 'externref';}};}},"
        "address:{get(){descriptorOrder.push('ta');return {toString(){descriptorOrder.push('tas');return 'i32';}};}},"
        "initial:{get(){descriptorOrder.push('ti');return {valueOf(){descriptorOrder.push('tiv');return 0;}};}},"
        "maximum:{get(){descriptorOrder.push('tm');return {valueOf(){descriptorOrder.push('tmv');return 1;}};}}});"
        "new WebAssembly.Table(tableDescriptor);"
        "const globalDescriptor=function(){};globalDescriptor.value='i32';"
        "const global=new WebAssembly.Global(globalDescriptor);"
        "let i64ObjectRefused=false,numberObjectRefused=false,symbolsRefused=0,addressRefused=0,tableUndefinedRefused=0;"
        "try{new WebAssembly.Global({value:'i64'},{valueOf(){return 1;}});}catch(error){i64ObjectRefused=error instanceof TypeError;}"
        "try{new WebAssembly.Global({value:'i32'},{valueOf(){return 1n;}});}catch(error){numberObjectRefused=error instanceof TypeError;}"
        "try{new WebAssembly.CompileError(Symbol());}catch(error){if(error instanceof TypeError)symbolsRefused++;}"
        "try{new WebAssembly.Table({element:Symbol(),initial:0});}catch(error){if(error instanceof TypeError)symbolsRefused++;}"
        "try{new WebAssembly.Global({value:Symbol()});}catch(error){if(error instanceof TypeError)symbolsRefused++;}"
        "const bytes=Uint8Array.from([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,2,11,1,3,101,110,118,3,97,100,100,0,0,3,2,1,0,7,8,1,4,99,97,108,108,0,1,10,10,1,8,0,32,0,32,1,16,0,11]),"
        "module=new WebAssembly.Module(bytes),moduleOrder=[],byteOrder=[],"
        "moduleImports={get env(){moduleOrder.push('env');return {get add(){moduleOrder.push('add');return (a,b)=>a+b;}};}},"
        "byteImports={get env(){byteOrder.push('env');return {get add(){byteOrder.push('add');return (a,b)=>a+b;}};}};"
        "const modulePromise=WebAssembly.instantiate(module,moduleImports),"
        "moduleWasSynchronous=moduleOrder.join(',')==='env,add',"
        "bytePromise=WebAssembly.instantiate(bytes,byteImports),"
        "bytesStayedDeferred=byteOrder.length===0;"
        "try{WebAssembly.Module.customSections(module,Symbol());}catch(error){if(error instanceof TypeError)symbolsRefused++;}"
        "try{new WebAssembly.Memory({address:'i64',initial:0n});}catch(error){if(error instanceof TypeError)addressRefused++;}"
        "try{new WebAssembly.Table({element:'anyfunc',address:'i64',initial:0n});}catch(error){if(error instanceof TypeError)addressRefused++;}"
        "try{new WebAssembly.Table({element:'anyfunc',initial:0},undefined);}catch(error){if(error instanceof TypeError)tableUndefinedRefused++;}"
        "const optionalTable=new WebAssembly.Table({element:'anyfunc',initial:1});"
        "try{optionalTable.set(0,undefined);}catch(error){if(error instanceof TypeError)tableUndefinedRefused++;}"
        "try{optionalTable.grow(0,undefined);}catch(error){if(error instanceof TypeError)tableUndefinedRefused++;}"
        "optionalTable.set(0);const omittedTableValue=optionalTable.get(0);"
        "Promise.all([modulePromise,bytePromise]).then(values=>{"
        "const okay=Object.getPrototypeOf(WebAssembly)===Object.prototype"
        "&&typeof WebAssembly.hasOwnProperty==='function'"
        "&&descriptorOrder.join(',')==='ma,mas,mi,miv,mm,mmv,ms,te,tes,ta,tas,ti,tiv,tm,tmv'"
        "&&global.value===0&&i64ObjectRefused&&numberObjectRefused&&symbolsRefused===4"
        "&&addressRefused===2&&tableUndefinedRefused===3&&omittedTableValue===null"
        "&&moduleWasSynchronous&&bytesStayedDeferred"
        "&&byteOrder.join(',')==='env,add'"
        "&&values[0].exports.call(19,23)===42"
        "&&values[1].instance.exports.call(20,22)===42;"
        "globalThis.pocSummary=okay?'WASM-WPT-ORDER-OK':'WASM-WPT-ORDER-FAILED';"
        "},error=>{globalThis.pocSummary='WASM-WPT-ORDER-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_wpt_document;
    CHECK(document_parse(&wasm_wpt_document, &budget,
                         wasm_wpt_page, strlen(wasm_wpt_page), 17));
    ScriptResult wasm_wpt_result;
    CHECK(scripts_run_document_at(
              &wasm_wpt_document, &budget, 10 * MIB, 3000,
              "https://wasm-wpt.test/", &wasm_wpt_result)
          && wasm_wpt_result.success
          && strcmp(wasm_wpt_result.summary, "WASM-WPT-ORDER-OK") == 0);
    document_destroy(&wasm_wpt_document);
    CHECK(budget.current == wasm_wpt_baseline);

    puts("test: unsupported native WebAssembly tables fail explicitly");
    size_t wasm_table_export_baseline = budget.current;
    static const char wasm_table_export_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "4,5,1,112,1,1,1,7,5,1,1,116,1,0]),"
        "module=new WebAssembly.Module(bytes),"
        "shape=WebAssembly.Module.exports(module);"
        "WebAssembly.instantiate(module).then(()=>{"
        "globalThis.pocSummary='WASM-TABLE-UNEXPECTED';"
        "},error=>{globalThis.pocSummary=shape.length===1"
        "&&shape[0].name==='t'&&shape[0].kind==='table'"
        "&&error instanceof WebAssembly.LinkError"
        "?'WASM-TABLE-BOUNDED':'WASM-TABLE-WRONG-ERROR';});"
        "</script></body></html>";
    PocDocument wasm_table_export_document;
    CHECK(document_parse(&wasm_table_export_document, &budget,
                         wasm_table_export_page,
                         strlen(wasm_table_export_page), 17));
    ScriptResult wasm_table_export_result;
    CHECK(scripts_run_document_at(
              &wasm_table_export_document, &budget, 8 * MIB, 2000,
              "https://wasm-table-export.test/",
              &wasm_table_export_result)
          && wasm_table_export_result.success
          && strcmp(wasm_table_export_result.summary,
                    "WASM-TABLE-BOUNDED") == 0);
    document_destroy(&wasm_table_export_document);
    CHECK(budget.current == wasm_table_export_baseline);

    puts("test: asynchronous WebAssembly APIs snapshot BufferSource bytes");
    size_t wasm_snapshot_baseline = budget.current;
    static const char wasm_snapshot_page[] =
        "<!doctype html><html><body><script>"
        "const original=Uint8Array.from([0,97,115,109,1,0,0,0]),"
        "compileBytes=original.slice(),instantiateBytes=original.slice();"
        "const compiled=WebAssembly.compile(compileBytes),"
        "instantiated=WebAssembly.instantiate(instantiateBytes);"
        "compileBytes.fill(0);instantiateBytes.fill(0);"
        "Promise.all([compiled,instantiated]).then(values=>{"
        "globalThis.pocSummary=values[0] instanceof WebAssembly.Module"
        "&&values[1].module instanceof WebAssembly.Module"
        "&&values[1].instance instanceof WebAssembly.Instance"
        "?'WASM-SNAPSHOT-OK':'WASM-SNAPSHOT-BAD';"
        "},e=>{globalThis.pocSummary='WASM-SNAPSHOT-ERROR:'+e;});"
        "</script></body></html>";
    PocDocument wasm_snapshot_document;
    CHECK(document_parse(&wasm_snapshot_document, &budget,
                         wasm_snapshot_page, strlen(wasm_snapshot_page), 17));
    ScriptResult wasm_snapshot_result;
    CHECK(scripts_run_document_at(
              &wasm_snapshot_document, &budget, 10 * MIB, 3000,
              "https://wasm-snapshot.test/", &wasm_snapshot_result)
          && wasm_snapshot_result.success
          && strcmp(wasm_snapshot_result.summary,
                    "WASM-SNAPSHOT-OK") == 0);
    document_destroy(&wasm_snapshot_document);
    CHECK(budget.current == wasm_snapshot_baseline);

    puts("test: WebAssembly i64 and bounded multi-value results are preserved");
    size_t wasm_values_baseline = budget.current;
    static const char wasm_values_page[] =
        "<!doctype html><html><body><script>"
        "const i64=Uint8Array.from([0,97,115,109,1,0,0,0,1,6,1,96,1,126,1,126,3,2,1,0,7,6,1,2,105,100,0,0,10,6,1,4,0,32,0,11]),"
        "multi=Uint8Array.from([0,97,115,109,1,0,0,0,1,6,1,96,0,2,127,126,3,2,1,0,7,6,1,2,109,118,0,0,10,8,1,6,0,65,7,66,9,11]),"
        "trap=Uint8Array.from([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,7,8,1,4,98,111,111,109,0,0,10,5,1,3,0,0,11]);"
        "Promise.all([WebAssembly.instantiate(i64),WebAssembly.instantiate(multi),WebAssembly.instantiate(trap)]).then(values=>{"
        "const pair=values[1].instance.exports.mv();"
        "let trapped=false;try{values[2].instance.exports.boom();}catch(e){trapped=e instanceof WebAssembly.RuntimeError;}"
        "globalThis.pocSummary=values[0].instance.exports.id(922337203685477580n)===922337203685477580n"
        "&&Array.isArray(pair)&&pair[0]===7&&pair[1]===9n&&trapped?'WASM-VALUES-OK':'WASM-VALUES-FAILED';"
        "},e=>{globalThis.pocSummary='WASM-VALUES-ERROR:'+e;});"
        "</script></body></html>";
    PocDocument wasm_values_document;
    CHECK(document_parse(&wasm_values_document, &budget,
                         wasm_values_page, strlen(wasm_values_page), 17));
    ScriptResult wasm_values_result;
    CHECK(scripts_run_document_at(
              &wasm_values_document, &budget, 8 * MIB, 2000,
              "https://wasm-values.test/", &wasm_values_result)
          && wasm_values_result.success
          && strcmp(wasm_values_result.summary, "WASM-VALUES-OK") == 0);
    document_destroy(&wasm_values_document);
    CHECK(budget.current == wasm_values_baseline);

    puts("test: bounded WebAssembly function imports preserve JavaScript behavior");
    size_t wasm_import_baseline = budget.current;
    static const char wasm_import_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,1,127,2,11,1,3,101,110,118,3,97,100,100,0,0,3,2,1,0,7,8,1,4,99,97,108,108,0,1,10,10,1,8,0,32,0,32,1,16,0,11]),"
        "module=new WebAssembly.Module(bytes),descriptors=WebAssembly.Module.imports(module),"
        "sentinel=new Error('sentinel');let shouldThrow=false;"
        "const good=new WebAssembly.Instance(module,{env:{add:(a,b)=>{if(shouldThrow)throw sentinel;return a+b;}}}),"
        "other=new WebAssembly.Instance(module,{env:{add:(a,b)=>a*b}}),"
        "sum=good.exports.call(19,23),product=other.exports.call(6,7);shouldThrow=true;"
        "let propagated=false;"
        "try{good.exports.call(1,2);}"
        "catch(e){propagated=e===sentinel;}"
        "const getterError=new Error('getter'),hostile={};"
        "Object.defineProperty(hostile,'env',{get(){throw getterError;}});"
        "let getterPropagated=false,missingLinked=false;"
        "try{new WebAssembly.Instance(module,hostile);}catch(e){getterPropagated=e===getterError;}"
        "try{new WebAssembly.Instance(module,{});}catch(e){missingLinked=e instanceof WebAssembly.LinkError;}"
        "globalThis.pocSummary=descriptors.length===1"
        "&&descriptors[0].module==='env'&&descriptors[0].name==='add'"
        "&&descriptors[0].kind==='function'&&sum===42&&product===42"
        "&&propagated&&getterPropagated&&missingLinked?'WASM-IMPORT-OK':'WASM-IMPORT-FAILED';"
        "</script></body></html>";
    PocDocument wasm_import_document;
    CHECK(document_parse(&wasm_import_document, &budget,
                         wasm_import_page, strlen(wasm_import_page), 17));
    ScriptResult wasm_import_result;
    CHECK(scripts_run_document_at(
              &wasm_import_document, &budget, 10 * MIB, 2000,
              "https://wasm-import.test/", &wasm_import_result)
          && wasm_import_result.success
          && strcmp(wasm_import_result.summary, "WASM-IMPORT-OK") == 0);
    document_destroy(&wasm_import_document);
    CHECK(budget.current == wasm_import_baseline);

    puts("test: WebAssembly instruction metering bounds hostile loops");
    size_t wasm_meter_baseline = budget.current;
    static const char wasm_meter_page[] =
        "<!doctype html><html><body><script>"
        "const loop=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,3,2,1,0,7,8,1,4,108,111,111,112,0,0,"
        "10,9,1,7,0,3,64,12,0,11,11]);"
        "const startLoop=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,3,2,1,0,8,1,0,"
        "10,9,1,7,0,3,64,12,0,11,11]);"
        "let startBounded=false;try{new WebAssembly.Instance("
        "new WebAssembly.Module(startLoop));}catch(error){"
        "startBounded=error instanceof WebAssembly.RuntimeError;}"
        "WebAssembly.instantiate(loop).then(value=>{"
        "let bounded=false;try{value.instance.exports.loop();}"
        "catch(error){bounded=error instanceof WebAssembly.RuntimeError;}"
        "globalThis.pocSummary=bounded&&startBounded&&1+1===2"
        "?'WASM-METER-OK':'WASM-METER-FAILED';"
        "},error=>{globalThis.pocSummary='WASM-METER-ERROR:'+error;});"
        "</script></body></html>";
    PocDocument wasm_meter_document;
    CHECK(document_parse(&wasm_meter_document, &budget,
                         wasm_meter_page, strlen(wasm_meter_page), 17));
    ScriptResult wasm_meter_result;
    CHECK(scripts_run_document_at(
              &wasm_meter_document, &budget, 8 * MIB, 3000,
              "https://wasm-meter.test/", &wasm_meter_result)
          && wasm_meter_result.success
          && strcmp(wasm_meter_result.summary, "WASM-METER-OK") == 0);
    document_destroy(&wasm_meter_document);
    CHECK(budget.current == wasm_meter_baseline);

    puts("test: WebAssembly start-function traps use RuntimeError");
    size_t wasm_start_trap_baseline = budget.current;
    static const char wasm_start_trap_page[] =
        "<!doctype html><html><body><script>"
        "const bytes=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,3,2,1,0,8,1,0,10,5,1,3,0,0,11]);"
        "const imported=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,2,7,1,1,109,1,102,0,0,8,1,0]),sentinel={};"
        "let trapped=false,importCalled=0,importPropagated=false;try{new WebAssembly.Instance("
        "new WebAssembly.Module(bytes));}catch(error){"
        "trapped=error instanceof WebAssembly.RuntimeError;}"
        "new WebAssembly.Instance(new WebAssembly.Module(imported),"
        "{m:{f(){importCalled++;}}});try{new WebAssembly.Instance("
        "new WebAssembly.Module(imported),{m:{f(){throw sentinel;}}});}"
        "catch(error){importPropagated=error===sentinel;}"
        "globalThis.pocSummary=trapped&&importCalled===1&&importPropagated"
        "?'WASM-START-TRAP-OK':'WASM-START-TRAP-BAD';"
        "</script></body></html>";
    PocDocument wasm_start_trap_document;
    CHECK(document_parse(&wasm_start_trap_document, &budget,
                         wasm_start_trap_page,
                         strlen(wasm_start_trap_page), 17));
    ScriptResult wasm_start_trap_result;
    CHECK(scripts_run_document_at(
              &wasm_start_trap_document, &budget, 8 * MIB, 2000,
              "https://wasm-start-trap.test/", &wasm_start_trap_result)
          && wasm_start_trap_result.success
          && strcmp(wasm_start_trap_result.summary,
                    "WASM-START-TRAP-OK") == 0);
    document_destroy(&wasm_start_trap_document);
    CHECK(budget.current == wasm_start_trap_baseline);

    puts("test: WebAssembly traps clear and toolchain helpers stay explicit");
    size_t wasm_trap_recovery_baseline = budget.current;
    static const char wasm_trap_recovery_page[] =
        "<!doctype html><html><body><script>"
        "const recovery=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,5,1,96,0,1,127,3,3,2,0,0,7,13,2,4,116,114,97,112,"
        "0,0,2,111,107,0,1,10,10,2,3,0,0,11,4,0,65,42,11]),"
        "ctors=Uint8Array.from([0,97,115,109,1,0,0,0,"
        "1,4,1,96,0,0,3,2,1,0,7,21,1,17,95,95,119,97,115,109,"
        "95,99,97,108,108,95,99,116,111,114,115,0,0,"
        "10,5,1,3,0,0,11]);"
        "const instance=new WebAssembly.Instance(new WebAssembly.Module(recovery));"
        "let trapped=false;try{instance.exports.trap();}catch(error){"
        "trapped=error instanceof WebAssembly.RuntimeError;}"
        "const helperInstance=new WebAssembly.Instance(new WebAssembly.Module(ctors));"
        "let helperExplicit=false;try{helperInstance.exports.__wasm_call_ctors();}"
        "catch(error){helperExplicit=error instanceof WebAssembly.RuntimeError;}"
        "globalThis.pocSummary=trapped&&instance.exports.ok()===42&&helperExplicit"
        "?'WASM-TRAP-RECOVERY-OK':'WASM-TRAP-RECOVERY-BAD';"
        "</script></body></html>";
    PocDocument wasm_trap_recovery_document;
    CHECK(document_parse(&wasm_trap_recovery_document, &budget,
                         wasm_trap_recovery_page,
                         strlen(wasm_trap_recovery_page), 17));
    ScriptResult wasm_trap_recovery_result;
    CHECK(scripts_run_document_at(
              &wasm_trap_recovery_document, &budget, 8 * MIB, 3000,
              "https://wasm-trap-recovery.test/",
              &wasm_trap_recovery_result)
          && wasm_trap_recovery_result.success
          && strcmp(wasm_trap_recovery_result.summary,
                    "WASM-TRAP-RECOVERY-OK") == 0);
    document_destroy(&wasm_trap_recovery_document);
    CHECK(budget.current == wasm_trap_recovery_baseline);

    puts("test: bounded Worker scope can compile and execute WebAssembly");
    size_t wasm_worker_baseline = budget.current;
    static const char wasm_worker_page[] =
        "<!doctype html><html><body></body></html>";
    static const char wasm_worker_source[] =
        "(()=>{const source=`const bytes=Uint8Array.from(["
        "0,97,115,109,1,0,0,0,1,5,1,96,0,1,127,3,2,1,0,"
        "7,10,1,6,97,110,115,119,101,114,0,0,10,6,1,4,0,65,42,11]);"
        "WebAssembly.instantiate(bytes).then(value=>postMessage("
        "value.instance.exports.answer()),error=>postMessage(String(error)));"
        "onmessage=event=>postMessage(event.data instanceof WebAssembly.Module)`,"
        "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
        "module=new WebAssembly.Module(Uint8Array.from(["
        "0,97,115,109,1,0,0,0])),cloned=structuredClone(module);let phase=0;"
        "worker.onmessage=event=>{if(phase++===0&&event.data===42){"
        "worker.postMessage(module);return;}globalThis.pocSummary="
        "event.data===true&&cloned instanceof WebAssembly.Module&&cloned!==module"
        "?'WASM-WORKER-OK':'WASM-WORKER-BAD:'+event.data;"
        "worker.terminate();URL.revokeObjectURL(url);};})()";
    PocDocument wasm_worker_document;
    CHECK(document_parse(&wasm_worker_document, &budget,
                         wasm_worker_page, strlen(wasm_worker_page), 17));
    ScriptResult wasm_worker_result;
    ScriptRuntime *wasm_worker_runtime = script_runtime_create_deferred(
        &wasm_worker_document, &budget, 10 * MIB, 3000,
        "https://wasm-worker.test/", NULL, &wasm_worker_result);
    CHECK(wasm_worker_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              wasm_worker_runtime, wasm_worker_source,
              "<wasm-worker>", &wasm_worker_result));
    for (size_t turn = 0; turn < 12; turn++) {
        CHECK(script_runtime_advance(
            wasm_worker_runtime, turn, 8, &wasm_worker_result));
    }
    CHECK(wasm_worker_result.success
          && strcmp(wasm_worker_result.summary, "WASM-WORKER-OK") == 0);
    script_runtime_destroy(wasm_worker_runtime);
    document_destroy(&wasm_worker_document);
    CHECK(budget.current == wasm_worker_baseline);

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
    static const char late_animation_frame_source[] =
        "(()=>{globalThis.pocSummary='RAF-LATE-PENDING';let first=0;"
        "requestAnimationFrame(timestamp=>{first=timestamp;"
        "requestAnimationFrame(next=>{globalThis.pocSummary="
        "next-first===50?'RAF-LATE-OK':'RAF-LATE-FAILED:'+(next-first);});});})()";
    CHECK(script_runtime_evaluate_diagnostic(
              deterministic_tick_runtime, late_animation_frame_source,
              "<late-animation-frame>", &deterministic_tick_result)
          && script_runtime_advance(
              deterministic_tick_runtime, 50, 16,
              &deterministic_tick_result)
          && strcmp(deterministic_tick_result.summary,
                    "RAF-LATE-PENDING") == 0
          && script_runtime_advance(
              deterministic_tick_runtime, 50, 16,
              &deterministic_tick_result)
          && strcmp(deterministic_tick_result.summary,
                    "RAF-LATE-OK") == 0);
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

    puts("test: reduced-motion mode resolves one static stylesheet state");
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
        "style.textContent='@keyframes pulse{from{opacity:0;transform:translateY(800px)}'"
        "+'to{opacity:1;transform:translateY(20px)}}.motion-probe{'"
        "+'animation:pulse 32ms linear;animation-timeline:scroll()}';"
        "box.className='motion-probe';"
        "document.head.appendChild(style);document.body.appendChild(box);"
        "queueMicrotask(()=>{globalThis.pocSummary="
        "matchMedia('(prefers-reduced-motion:reduce)').matches"
        "&&__tilefinchRootCensus.motionScans===1&&box.style.opacity==='1'"
        "&&box.style.transform==='none'"
        "?'STYLESHEET-MOTION-STATIC':'STYLESHEET-MOTION-BAD:'+"
        "[__tilefinchRootCensus.motionScans,box.style.opacity,"
        "box.style.transform].join(',')});"
        "globalThis.pocSummary='STYLESHEET-MOTION-PENDING';})()";
    CHECK(stylesheet_animation_runtime != NULL
          && script_runtime_evaluate_diagnostic(
              stylesheet_animation_runtime, stylesheet_animation_source,
              "<stylesheet-animation>", &stylesheet_animation_result));
    for (size_t tick = 0; tick < 2
                          && strcmp(stylesheet_animation_result.summary,
                                    "STYLESHEET-MOTION-STATIC") != 0;
         tick++) {
        CHECK(script_runtime_advance(
            stylesheet_animation_runtime, 16, 16,
            &stylesheet_animation_result));
    }
    if (strcmp(stylesheet_animation_result.summary,
               "STYLESHEET-MOTION-STATIC") != 0) {
        fprintf(stderr, "stylesheet animation summary=%s error=%s\n",
                stylesheet_animation_result.summary,
                stylesheet_animation_result.error);
    }
    CHECK(stylesheet_animation_result.success
          && strcmp(stylesheet_animation_result.summary,
                    "STYLESHEET-MOTION-STATIC") == 0
          && stylesheet_animation_result.bootstrap_lazy_module_loads == 1
          && stylesheet_animation_result.bootstrap_lazy_module_failures == 0);

    puts("test: bounded native text prefixes preserve textContent semantics");
    static const char bounded_text_prefix_source[] =
        "(()=>{const style=document.createElement('style');"
        "style.append(document.createTextNode('\\u00e9a'),"
        "document.createTextNode('bc'));document.head.appendChild(style);"
        "const complete=style.textContent,prefix="
        "__tilefinchGetTextPrefix(style.__handle,3,4096),"
        "oneNode=__tilefinchGetTextPrefix(style.__handle,16,2),"
        "allNodes=__tilefinchGetTextPrefix(style.__handle,16,3),"
        "box=document.createElement('div');"
        "box.setAttribute('style','\\u00e9abc');"
        "document.body.appendChild(box);"
        "const attr=__tilefinchGetStyleAttributePrefix(box.__handle,3);"
        "globalThis.pocSummary=complete==='\\u00e9abc'"
        "&&prefix.text==='\\u00e9a'&&prefix.nodes===2"
        "&&oneNode.text==='\\u00e9a'&&oneNode.nodes===2"
        "&&allNodes.text==='\\u00e9abc'&&allNodes.nodes===3"
        "&&attr==='\\u00e9a'&&box.getAttribute('style')==='\\u00e9abc'"
        "?'BOUNDED-TEXT-PREFIX-OK':'BOUNDED-TEXT-PREFIX-BAD:'"
        "+[complete,prefix.text,prefix.nodes,oneNode.text,oneNode.nodes,"
        "allNodes.text,allNodes.nodes,attr].join('|')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              stylesheet_animation_runtime, bounded_text_prefix_source,
              "<bounded-text-prefix>", &stylesheet_animation_result)
          && stylesheet_animation_result.success
          && strcmp(stylesheet_animation_result.summary,
                    "BOUNDED-TEXT-PREFIX-OK") == 0);

    puts("test: bounded text prefix charges textless descendants");
    static const char bounded_textless_start_source[] =
        "globalThis.__boundedTextless=document.createElement('style');"
        "__boundedTextless.appendChild(document.createTextNode('x'));"
        "document.head.appendChild(__boundedTextless)";
    static const char bounded_textless_finish_source[] =
        "(()=>{const prefix=__tilefinchGetTextPrefix("
        "__boundedTextless.__handle,16,128);"
        "globalThis.pocSummary=prefix.text==='x'"
        "&&prefix.nodes===128?'BOUNDED-TEXTLESS-NODES-OK':"
        "'BOUNDED-TEXTLESS-NODES-BAD:'+"
        "[prefix.text,prefix.nodes].join('|')})()";
    CHECK(script_runtime_evaluate_diagnostic(
        stylesheet_animation_runtime, bounded_textless_start_source,
        "<bounded-textless-start>", &stylesheet_animation_result));
    lxb_dom_node_t *textless_style = NULL;
    lxb_dom_node_t *textless_root = lxb_dom_interface_node(
        deterministic_tick_document.html);
    lxb_dom_node_t *textless_at = textless_root;
    for (size_t visited = 0; textless_at != NULL && visited < 512; visited++) {
        size_t name_length = 0;
        const char *name = document_element_name(textless_at, &name_length);
        if (name != NULL && name_length == 5
            && strncasecmp(name, "style", 5) == 0) {
            textless_style = textless_at;
        }
        if (textless_at->first_child != NULL) {
            textless_at = textless_at->first_child;
            continue;
        }
        while (textless_at != NULL && textless_at != textless_root
               && textless_at->next == NULL) {
            textless_at = textless_at->parent;
        }
        textless_at = textless_at == NULL || textless_at == textless_root
            ? NULL : textless_at->next;
    }
    CHECK(textless_style != NULL);
    BudgetAllocationOwner textless_owner = document_allocation_owner_enter(
        &deterministic_tick_document);
    for (size_t child = 0; child < 128 && textless_style != NULL; child++) {
        static const lxb_char_t tag[] = "i";
        lxb_dom_element_t *empty = lxb_dom_document_create_element(
            &deterministic_tick_document.html->dom_document,
            tag, sizeof(tag) - 1u, NULL);
        CHECK(empty != NULL
              && lxb_dom_node_append_child(
                     textless_style, lxb_dom_interface_node(empty))
                     == LXB_DOM_EXCEPTION_OK);
    }
    document_allocation_owner_leave(
        &deterministic_tick_document, textless_owner);
    bool bounded_textless_ok = script_runtime_evaluate_diagnostic(
        stylesheet_animation_runtime, bounded_textless_finish_source,
        "<bounded-textless-prefix>", &stylesheet_animation_result);
    if (!bounded_textless_ok
        || strcmp(stylesheet_animation_result.summary,
                  "BOUNDED-TEXTLESS-NODES-OK") != 0) {
        fprintf(stderr, "bounded textless summary=%s error=%s\n",
                stylesheet_animation_result.summary,
                stylesheet_animation_result.error);
    }
    CHECK(bounded_textless_ok
          && stylesheet_animation_result.success
          && strcmp(stylesheet_animation_result.summary,
                    "BOUNDED-TEXTLESS-NODES-OK") == 0);

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

    puts("test: oversized style motion scans retain only a bounded prefix");
    size_t oversized_motion_baseline = budget.current;
    size_t oversized_motion_budget_limit = budget.limit;
    CHECK(budget.limit <= SIZE_MAX - 8u * MIB);
    budget.limit += 8u * MIB;
    static const char oversized_motion_page[] =
        "<!doctype html><html><head><style></style></head><body>"
        "<div id=oversized-motion class=oversized-motion>VISIBLE</div>"
        "</body></html>";
    static const char oversized_motion_css_prefix[] =
        "@keyframes huge{from{opacity:0}to{opacity:1}}"
        ".oversized-motion{animation:huge 1s linear}/*";
    const size_t oversized_motion_padding = 5u * MIB;
    PocDocument oversized_motion_document = {0};
    CHECK(document_parse(
        &oversized_motion_document, &budget, oversized_motion_page,
        sizeof(oversized_motion_page) - 1u, 17));
    ScriptResult oversized_motion_result = {0};
    ScriptRuntimeOptions oversized_motion_options =
        deterministic_tick_options;
    oversized_motion_options.defer_document_scripts = false;
    ScriptRuntime *oversized_motion_runtime =
        script_runtime_create_configured(
            &oversized_motion_document, &budget, 6u * MIB, 5000,
            "https://oversized-motion.test/", &oversized_motion_options,
            &oversized_motion_result);
    CHECK(oversized_motion_runtime != NULL);
    if (oversized_motion_runtime != NULL) {
        lxb_dom_node_t *root = lxb_dom_interface_node(
            oversized_motion_document.html);
        lxb_dom_node_t *style_node = NULL;
        lxb_dom_node_t *at = root;
        for (size_t visited = 0; at != NULL && visited < 64; visited++) {
            size_t name_length = 0;
            const char *name = document_element_name(at, &name_length);
            if (name != NULL && name_length == 5
                && strncasecmp(name, "style", 5) == 0) {
                style_node = at;
                break;
            }
            if (at->first_child != NULL) {
                at = at->first_child;
                continue;
            }
            while (at != NULL && at != root && at->next == NULL) {
                at = at->parent;
            }
            at = at == NULL || at == root ? NULL : at->next;
        }
        const size_t oversized_motion_text_length =
            sizeof(oversized_motion_css_prefix) - 1u
            + oversized_motion_padding + 2u;
        char *oversized_motion_text = malloc(
            oversized_motion_text_length);
        CHECK(style_node != NULL && oversized_motion_text != NULL);
        if (style_node != NULL && oversized_motion_text != NULL) {
            memcpy(oversized_motion_text, oversized_motion_css_prefix,
                   sizeof(oversized_motion_css_prefix) - 1u);
            memset(oversized_motion_text
                       + sizeof(oversized_motion_css_prefix) - 1u,
                   ' ', oversized_motion_padding);
            oversized_motion_text[oversized_motion_text_length - 2u] = '*';
            oversized_motion_text[oversized_motion_text_length - 1u] = '/';
            BudgetAllocationOwner previous = document_allocation_owner_enter(
                &oversized_motion_document);
            lxb_dom_text_t *padding = lxb_dom_document_create_text_node(
                &oversized_motion_document.html->dom_document,
                (const lxb_char_t *) oversized_motion_text,
                oversized_motion_text_length);
            bool appended = padding != NULL
                && lxb_dom_node_append_child(
                       style_node, lxb_dom_interface_node(padding))
                       == LXB_DOM_EXCEPTION_OK;
            document_allocation_owner_leave(
                &oversized_motion_document, previous);
            CHECK(appended);
            if (padding != NULL && !appended) {
                lxb_dom_node_destroy_deep(lxb_dom_interface_node(padding));
            }
            free(oversized_motion_text);
        }
        size_t remaining = script_runtime_heap_remaining(
            oversized_motion_runtime);
        size_t rejections = script_runtime_heap_rejections(
            oversized_motion_runtime);
        /* The old style.textContent path needed one allocation larger than
           the entire remaining JS heap. */
        if (!(remaining < oversized_motion_padding
              && remaining > 384u * 1024u)) {
            fprintf(stderr,
                    "oversized motion pre-scan remaining=%zu padding=%zu\n",
                    remaining, oversized_motion_padding);
        }
        CHECK(remaining < oversized_motion_padding
              && remaining > 384u * 1024u);
        static const char oversized_motion_start[] =
            "(()=>{const style=document.querySelector('style'),"
            "prefix=__tilefinchGetTextPrefix(style.__handle,262144,4096);"
            "globalThis.pocSummary=prefix.text.startsWith('@keyframes huge')"
            "&&prefix.text.length===262144&&prefix.nodes===2"
            "?'OVERSIZED-MOTION-PENDING':"
            "'OVERSIZED-MOTION-PREFIX-BAD:'+"
            "[prefix.text.length,prefix.nodes].join(',');"
            "document.body.setAttribute('style','color:black');"
            "__tilefinchMaybeStartMotion();})()";
        bool oversized_motion_started = script_runtime_evaluate_diagnostic(
            oversized_motion_runtime, oversized_motion_start,
            "<oversized-motion-start>", &oversized_motion_result);
        if (!oversized_motion_started) {
            fprintf(stderr,
                    "oversized motion start failed summary=%s error=%s "
                    "remaining=%zu rejects=%zu budget=%zu/%zu\n",
                    oversized_motion_result.summary,
                    oversized_motion_result.error, remaining,
                    script_runtime_heap_rejections(
                        oversized_motion_runtime),
                    budget.current, budget.limit);
        }
        CHECK(oversized_motion_started);
        for (size_t tick = 0; tick < 4; tick++) {
            CHECK(script_runtime_advance(
                oversized_motion_runtime, 16, 16,
                &oversized_motion_result));
        }
        static const char oversized_motion_initial_check[] =
            "(()=>{const box=document.getElementById('oversized-motion');"
            "globalThis.pocSummary=__tilefinchRootCensus.motionScans>=1"
            "&&box.style.opacity==='1'?'OVERSIZED-MOTION-INITIAL-OK':"
            "'OVERSIZED-MOTION-INITIAL-BAD:'+"
            "[__tilefinchRootCensus.motionScans,box.style.opacity].join(',');"
            "box.className='oversized-motion changed'})()";
        CHECK(script_runtime_evaluate_diagnostic(
            oversized_motion_runtime, oversized_motion_initial_check,
            "<oversized-motion-initial-check>", &oversized_motion_result));
        for (size_t tick = 0; tick < 4; tick++) {
            CHECK(script_runtime_advance(
                oversized_motion_runtime, 16, 16,
                &oversized_motion_result));
        }
        static const char oversized_motion_mutation_check[] =
            "globalThis.pocSummary="
            "__tilefinchRootCensus.motionScans>=2"
            "?'OVERSIZED-MOTION-MUTATION-OK':"
            "'OVERSIZED-MOTION-MUTATION-BAD:'+"
            "__tilefinchRootCensus.motionScans";
        CHECK(script_runtime_evaluate_diagnostic(
            oversized_motion_runtime, oversized_motion_mutation_check,
            "<oversized-motion-mutation-check>",
            &oversized_motion_result));
        if (strcmp(oversized_motion_result.summary,
                   "OVERSIZED-MOTION-MUTATION-OK") != 0) {
            fprintf(stderr,
                    "oversized motion summary=%s error=%s remaining=%zu "
                    "rejects=%zu\n",
                    oversized_motion_result.summary,
                    oversized_motion_result.error, remaining,
                    script_runtime_heap_rejections(
                        oversized_motion_runtime));
        }
        CHECK(oversized_motion_result.success
              && strcmp(oversized_motion_result.summary,
                        "OVERSIZED-MOTION-MUTATION-OK") == 0
              && script_runtime_heap_rejections(
                     oversized_motion_runtime) == rejections);
        script_runtime_destroy(oversized_motion_runtime);
    }
    document_destroy(&oversized_motion_document);
    budget.limit = oversized_motion_budget_limit;
    CHECK(budget.current == oversized_motion_baseline);

    puts("test: oversized inline motion styles use the native byte prefix");
    size_t oversized_inline_baseline = budget.current;
    size_t oversized_inline_budget_limit = budget.limit;
    CHECK(budget.limit <= SIZE_MAX - 8u * MIB);
    budget.limit += 8u * MIB;
    static const char oversized_inline_page[] =
        "<!doctype html><html><head></head><body>"
        "<div id=oversized-inline>VISIBLE</div></body></html>";
    static const char oversized_inline_prefix[] =
        "animation:huge 1s linear;--nonascii:\xc3\xa9;";
    PocDocument oversized_inline_document = {0};
    CHECK(document_parse(
        &oversized_inline_document, &budget, oversized_inline_page,
        sizeof(oversized_inline_page) - 1u, 17));
    ScriptResult oversized_inline_result = {0};
    ScriptRuntimeOptions oversized_inline_options = deterministic_tick_options;
    oversized_inline_options.defer_document_scripts = false;
    ScriptRuntime *oversized_inline_runtime = script_runtime_create_configured(
        &oversized_inline_document, &budget, 6u * MIB, 5000,
        "https://oversized-inline.test/", &oversized_inline_options,
        &oversized_inline_result);
    CHECK(oversized_inline_runtime != NULL);
    if (oversized_inline_runtime != NULL) {
        const size_t inline_padding = 5u * MIB;
        const size_t inline_length = sizeof(oversized_inline_prefix) - 1u
            + inline_padding;
        char *inline_value = malloc(inline_length);
        lxb_dom_node_t *body = document_body_node(&oversized_inline_document);
        lxb_dom_node_t *inline_node = body != NULL ? body->first_child : NULL;
        CHECK(inline_value != NULL && inline_node != NULL);
        if (inline_value != NULL && inline_node != NULL) {
            memcpy(inline_value, oversized_inline_prefix,
                   sizeof(oversized_inline_prefix) - 1u);
            memset(inline_value + sizeof(oversized_inline_prefix) - 1u,
                   ' ', inline_padding);
            BudgetAllocationOwner previous = document_allocation_owner_enter(
                &oversized_inline_document);
            lxb_dom_attr_t *attribute = lxb_dom_element_set_attribute(
                lxb_dom_interface_element(inline_node),
                (const lxb_char_t *) "style", 5,
                (const lxb_char_t *) inline_value, inline_length);
            document_allocation_owner_leave(
                &oversized_inline_document, previous);
            CHECK(attribute != NULL);
            free(inline_value);
        }
        size_t remaining = script_runtime_heap_remaining(
            oversized_inline_runtime);
        size_t rejections = script_runtime_heap_rejections(
            oversized_inline_runtime);
        CHECK(remaining < inline_padding && remaining > 384u * 1024u);
        static const char oversized_inline_start[] =
            "(()=>{const box=document.getElementById('oversized-inline'),"
            "prefix=__tilefinchGetStyleAttributePrefix(box.__handle,38),"
            "utf8=new TextEncoder().encode(prefix);"
            "globalThis.pocSummary=prefix.startsWith('animation:huge')"
            "&&utf8.length===38&&prefix.endsWith('\\u00e9')"
            "?'OVERSIZED-INLINE-PENDING':'OVERSIZED-INLINE-PREFIX-BAD:'"
            "+[prefix,utf8.length].join('|');"
            "document.body.setAttribute('style','color:black');"
            "__tilefinchMaybeStartMotion();})()";
        CHECK(script_runtime_evaluate_diagnostic(
            oversized_inline_runtime, oversized_inline_start,
            "<oversized-inline-start>", &oversized_inline_result)
              && oversized_inline_result.success
              && strcmp(oversized_inline_result.summary,
                        "OVERSIZED-INLINE-PENDING") == 0);
        for (size_t tick = 0; tick < 4; tick++) {
            CHECK(script_runtime_advance(
                oversized_inline_runtime, 16, 16,
                &oversized_inline_result));
        }
        static const char oversized_inline_check[] =
            "globalThis.pocSummary=__tilefinchRootCensus.motionScans>=1"
            "?'OVERSIZED-INLINE-OK':'OVERSIZED-INLINE-BAD:'"
            "+__tilefinchRootCensus.motionScans";
        CHECK(script_runtime_evaluate_diagnostic(
            oversized_inline_runtime, oversized_inline_check,
            "<oversized-inline-check>", &oversized_inline_result)
              && oversized_inline_result.success
              && strcmp(oversized_inline_result.summary,
                        "OVERSIZED-INLINE-OK") == 0
              && script_runtime_heap_rejections(
                     oversized_inline_runtime) == rejections);
        script_runtime_destroy(oversized_inline_runtime);
    }
    document_destroy(&oversized_inline_document);
    budget.limit = oversized_inline_budget_limit;
    CHECK(budget.current == oversized_inline_baseline);

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
        "let sequence=[],errorEvents=0;"
        "addEventListener('error',()=>{errorEvents++;"
        "throw new Error('expected nested error-listener failure')});"
        "document.addEventListener('DOMContentLoaded',()=>{"
        "sequence.push('first');throw new Error('expected listener failure')});"
        "document.addEventListener('DOMContentLoaded',()=>sequence.push('second'));"
        "addEventListener('load',()=>{console.error=()=>{"
        "throw new Error('expected reporter failure')};"
        "throw new Error('expected load failure')});"
        "addEventListener('load',()=>{sequence.push('load');"
        "globalThis.pocSummary=sequence.join(',')+':'+errorEvents});"
        "</script></body></html>";
    PocDocument listener_document;
    CHECK(document_parse(&listener_document, &budget, listener_page,
                         strlen(listener_page), 17));
    ScriptResult listener_result;
    CHECK(scripts_run_document(&listener_document, &budget, 4 * MIB, 1000,
                               &listener_result));
    CHECK(listener_result.success
          && listener_result.dom_content_loaded_dispatched
          && strcmp(listener_result.summary, "first,second,load:2") == 0);
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

    puts("test: failed hydration restores a complete server-rendered body");
    static const char hydration_page[] =
        "<!doctype html><body><main><h1>Server title</h1>"
        "<p>The server-rendered article remains useful when an author script "
        "clears its mount point and then exhausts its execution budget. This "
        "text deliberately exceeds the useful-content threshold.</p>"
        "<input id=hydration-control value=server>"
        "<a href='/next'>Continue reading</a></main></body>";
    PocDocument hydration_document = {0};
    CHECK(document_parse(&hydration_document, &budget, hydration_page,
                         strlen(hydration_page), 97u));
    DocumentBodySnapshot hydration_snapshot = {0};
    CHECK(document_body_snapshot_capture(
              &hydration_document, &hydration_snapshot));
    lxb_dom_node_t *hydration_body = document_body_node(&hydration_document);
    CHECK(hydration_body != NULL
          && document_set_element_inner_html(
                 &hydration_document, hydration_body,
                 "<input id=loading-control value=loading>",
                 sizeof("<input id=loading-control value=loading>") - 1u));
    lxb_dom_node_t *hydration_control = find_id(
        lxb_dom_interface_node(hydration_document.html), "loading-control");
    CHECK(hydration_control != NULL
          && document_control_value_set(
                 &hydration_document, hydration_control, "changed", 7u)
          && hydration_document.control_state_count == 1u
          && document_body_snapshot_restore_if_degraded(
                 &hydration_document, &hydration_snapshot, NULL, NULL));
    const char *hydration_text = document_body_text(&hydration_document);
    CHECK(hydration_text != NULL
          && strstr(hydration_text, "Server title") != NULL
          && strstr(hydration_text, "Continue reading") != NULL
          && hydration_document.control_state_count == 0u
          && find_id(lxb_dom_interface_node(hydration_document.html),
                     "hydration-control") != NULL);
    document_body_snapshot_destroy(&hydration_snapshot);
    document_destroy(&hydration_document);
    CHECK(budget.current == listener_baseline);

    puts("test: failed hydration restores vanished server action with prose intact");
    static const char actionable_hydration_page[] =
        "<!doctype html><body><main><h1>Account help</h1>"
        "<p>This server-rendered guidance remains fully visible after the "
        "application mount fails, so text-only degradation checks must not "
        "hide the fact that its only useful navigation action disappeared.</p>"
        "<a id=server-action href='/account/help'>Open account help</a>"
        "</main></body>";
    PocDocument actionable_hydration_document = {0};
    DocumentBodySnapshot actionable_hydration_snapshot = {0};
    CHECK(document_parse(
              &actionable_hydration_document, &budget,
              actionable_hydration_page, strlen(actionable_hydration_page),
              97u)
          && document_body_snapshot_capture(
                 &actionable_hydration_document,
                 &actionable_hydration_snapshot)
          && actionable_hydration_snapshot.source_action_count == 1u);
    lxb_dom_node_t *actionable_hydration_body =
        document_body_node(&actionable_hydration_document);
    static const char prose_only_hydration[] =
        "<main><h1>Account help</h1>"
        "<p>This server-rendered guidance remains fully visible after the "
        "application mount fails, so text-only degradation checks must not "
        "hide the fact that its only useful navigation action disappeared.</p>"
        "</main>";
    CHECK(actionable_hydration_body != NULL
          && document_set_element_inner_html(
                 &actionable_hydration_document, actionable_hydration_body,
                 prose_only_hydration, sizeof(prose_only_hydration) - 1u)
          && document_body_snapshot_restore_if_degraded(
                 &actionable_hydration_document,
                 &actionable_hydration_snapshot, NULL, NULL)
          && find_id(
                 lxb_dom_interface_node(actionable_hydration_document.html),
                 "server-action") != NULL);
    document_body_snapshot_destroy(&actionable_hydration_snapshot);
    document_destroy(&actionable_hydration_document);
    CHECK(budget.current == listener_baseline);

    puts("test: compact action-only hydration shell remains recoverable");
    static const char compact_action_page[] =
        "<!doctype html><body><form id=compact-action action=/find method=get>"
        "<input name=q><button>Go</button></form></body>";
    PocDocument compact_action_document = {0};
    DocumentBodySnapshot compact_action_snapshot = {0};
    CHECK(document_parse(
              &compact_action_document, &budget, compact_action_page,
              strlen(compact_action_page), 97u)
          && document_body_snapshot_capture(
                 &compact_action_document, &compact_action_snapshot)
          && compact_action_snapshot.source_text_bytes < 128u
          && compact_action_snapshot.source_action_count != 0u);
    lxb_dom_node_t *compact_action_body =
        document_body_node(&compact_action_document);
    CHECK(compact_action_body != NULL
          && document_set_element_inner_html(
                 &compact_action_document, compact_action_body,
                 "<main id=loading>Loading</main>",
                 sizeof("<main id=loading>Loading</main>") - 1u)
          && document_body_snapshot_restore_if_degraded(
                 &compact_action_document, &compact_action_snapshot,
                 NULL, NULL)
          && find_id(
                 lxb_dom_interface_node(compact_action_document.html),
                 "compact-action") != NULL
          && find_id(
                 lxb_dom_interface_node(compact_action_document.html),
                 "loading") == NULL);
    document_body_snapshot_destroy(&compact_action_snapshot);
    document_destroy(&compact_action_document);
    CHECK(budget.current == listener_baseline);

    puts("test: body snapshot reserves its exact-bound terminator");
    enum { exact_snapshot_bytes = 256u * 1024u };
    static const char exact_prefix[] = "<!doctype html><body><main>";
    static const char exact_suffix[] = "</main></body>";
    size_t exact_text_bytes = exact_snapshot_bytes
        - (sizeof("<main>") - 1u) - (sizeof("</main>") - 1u);
    size_t exact_source_bytes = sizeof(exact_prefix) - 1u
        + exact_text_bytes + sizeof(exact_suffix) - 1u;
    char *exact_source = malloc(exact_source_bytes + 1u);
    CHECK(exact_source != NULL);
    size_t exact_used = 0;
    memcpy(exact_source + exact_used, exact_prefix,
           sizeof(exact_prefix) - 1u);
    exact_used += sizeof(exact_prefix) - 1u;
    memset(exact_source + exact_used, 'x', exact_text_bytes);
    exact_used += exact_text_bytes;
    memcpy(exact_source + exact_used, exact_suffix,
           sizeof(exact_suffix) - 1u);
    exact_used += sizeof(exact_suffix) - 1u;
    exact_source[exact_used] = '\0';
    PocDocument exact_snapshot_document = {0};
    DocumentBodySnapshot exact_snapshot = {0};
    CHECK(exact_used == exact_source_bytes
          && document_parse(
                 &exact_snapshot_document, &budget,
                 exact_source, exact_source_bytes, 4096u)
          && document_body_snapshot_capture(
                 &exact_snapshot_document, &exact_snapshot)
          && exact_snapshot.length == exact_snapshot_bytes
          && exact_snapshot.capacity >= exact_snapshot_bytes + 1u
          && exact_snapshot.markup[exact_snapshot_bytes] == '\0');
    document_body_snapshot_destroy(&exact_snapshot);
    document_destroy(&exact_snapshot_document);
    free(exact_source);
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
