static bool test_native_script_element_state(Budget *budget)
{
    const char *test_name = "native script-element state and currentScript";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char exercise[] =
        "globalThis.scriptStateEvents=[];"
        "const currentDescriptor=Object.getOwnPropertyDescriptor("
        "document,'currentScript');"
        "let redefineBlocked=false;try{Object.defineProperty(document,"
        "'currentScript',{value:'poison'});}catch(error){redefineBlocked=true;}"
        "document.currentScript='poison';"
        "const currentProtected=currentDescriptor&&"
        "currentDescriptor.configurable===false&&"
        "currentDescriptor.set===undefined&&redefineBlocked&&"
        "delete document.currentScript===false&&"
        "document.currentScript===null;"
        "const crossScript=document.createElement('script'),"
        "crossLink=document.createElement('link');"
        "const crossOriginMissing=crossScript.crossOrigin===null;"
        "crossScript.crossOrigin='USE-CREDENTIALS';"
        "const crossOriginKnown=crossScript.crossOrigin==='use-credentials'&&"
        "crossScript.getAttribute('crossorigin')==='USE-CREDENTIALS';"
        "crossScript.setAttribute('crossorigin','unexpected');"
        "const crossOriginInvalid=crossScript.crossOrigin==='anonymous';"
        "crossScript.crossOrigin=null;crossLink.crossOrigin='';"
        "const crossOriginReflected=crossOriginMissing&&crossOriginKnown&&"
        "crossOriginInvalid&&!crossScript.hasAttribute('crossorigin')&&"
        "crossScript.crossOrigin===null&&"
        "crossLink.crossOrigin==='anonymous';"
        "globalThis.__tilefinchMaybeScheduleDynamicScript=()=>{throw new Error("
        "'poison schedule gate');};"
        "globalThis.__tilefinchScheduleDynamicScript=()=>{throw new Error("
        "'poison native scheduler');};"
        "globalThis.__tilefinchWrap=()=>({id:'poison-wrap'});"
        "globalThis.__tilefinchCurrentScriptForStable=()=>({id:'poison-stable'});"
        "globalThis.lateRuns=0;globalThis.nestedSyncOk=false;"
        "globalThis.lateScript=document.createElement('script');"
        "document.head.appendChild(lateScript);"
        "lateScript.textContent=`globalThis.lateRuns++;"
        "scriptStateEvents.push(document.currentScript===lateScript?"
        "'late-current':'late-wrong');"
        "Promise.resolve().then(()=>scriptStateEvents.push('microtask'));"
        "lateScript.remove();scriptStateEvents.push("
        "document.currentScript===lateScript&&!lateScript.isConnected?"
        "'late-detached':'late-detached-wrong');"
        "globalThis.nestedScript=document.createElement('script');"
        "nestedScript.textContent=\"scriptStateEvents.push("
        "document.currentScript===nestedScript?'nested-current':"
        "'nested-wrong');nestedScript.remove();scriptStateEvents.push("
        "document.currentScript===nestedScript&&!nestedScript.isConnected?"
        "'nested-detached':'nested-detached-wrong');\";"
        "document.head.appendChild(nestedScript);"
        "globalThis.nestedSyncOk=document.currentScript===lateScript&&"
        "!scriptStateEvents.includes('microtask');"
        "scriptStateEvents.push(nestedSyncOk?'late-restored':"
        "'late-not-restored');`;"
        "lateScript.textContent='globalThis.lateRuns+=100;';"
        "document.head.appendChild(lateScript);"
        "globalThis.ihRuns=0;const innerHtmlScript="
        "document.createElement('script');"
        "document.head.appendChild(innerHtmlScript);"
        "innerHtmlScript.innerHTML='globalThis.ihRuns++;';"
        "globalThis.parsedRuns=0;const parsedContainer="
        "document.createElement('div');"
        "parsedContainer.innerHTML='<script>globalThis.parsedRuns++;<\\/script>';"
        "const parsedScript=parsedContainer.firstChild;"
        "document.head.appendChild(parsedScript);"
        "parsedScript.textContent='globalThis.parsedRuns+=100;';"
        "globalThis.cloneRuns=0;const cloneContainer="
        "document.createElement('div'),cloneSource="
        "document.createElement('script');"
        "cloneSource.textContent='globalThis.cloneRuns++;';"
        "cloneContainer.appendChild(cloneSource);"
        "document.head.appendChild(cloneContainer.cloneNode(true));"
        "globalThis.atomicEvents=[];globalThis.atomicFirst="
        "document.createElement('script');globalThis.atomicSecond="
        "document.createElement('script');"
        "atomicFirst.textContent=\"atomicEvents.push('first');"
        "document.head.innerHTML='';\";"
        "atomicSecond.textContent=\"atomicEvents.push('second');\";"
        "document.head.append(atomicFirst,atomicSecond);"
        "const syncState=scriptStateEvents.join(',')==="
        "'late-current,late-detached,nested-current,nested-detached,"
        "late-restored'&&lateRuns===1&&nestedSyncOk&&"
        "document.currentScript===null;"
        "globalThis.pocSummary='native-state:' + "
        "(currentProtected?'protected':'poisoned')+':' + "
        "(crossOriginReflected?'cors':'cors-wrong')+':' + "
        "(syncState?'sync':'wrong-order')+':' + "
        "(ihRuns===1?'innerhtml':'innerhtml-wrong')+':' + "
        "(parsedRuns===0?'parsed-inert':'parsed-ran')+':' + "
        "(cloneRuns===1?'clone':'clone-wrong')+':' + "
        "atomicEvents.join(',');";
    static const char expected[] =
        "native-state:protected:cors:sync:innerhtml:parsed-inert:clone:first";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://native-state.test/",
        empty_page, 16, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    bool evaluated = evaluate(
        &navigation, exercise, "<native-script-state>");
    if (!evaluated) {
        fprintf(stderr, "native state evaluation error: %s\n",
                navigation.page.script_result.error);
    }
    REQUIRE(evaluated);
    if (strcmp(navigation.page.script_result.summary, expected) != 0) {
        fprintf(stderr, "native state summary: %s; error: %s\n",
                navigation.page.script_result.summary,
                navigation.page.script_result.error);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary, expected) == 0);

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

static bool test_external_start_and_async_state(Budget *budget)
{
    const char *test_name = "external script start and force-async state";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char late_source[] =
        "globalThis.lateExternalRuns++;";
    static const char should_not_run_source[] =
        "globalThis.shouldNotRun++;";
    static const char exercise[] =
        "globalThis.lateExternalRuns=0;globalThis.shouldNotRun=0;"
        "globalThis.emptyErrors=0;"
        "const raw=document.createElement('script'),"
        "assignedTrue=document.createElement('script'),"
        "assignedFalse=document.createElement('script');"
        "const initial=raw.async===true;raw.setAttribute('async','');"
        "const reflected=raw.async===true;raw.removeAttribute('async');"
        "const rawCleared=raw.async===false;assignedTrue.async=true;"
        "assignedTrue.removeAttribute('async');"
        "const trueCleared=assignedTrue.async===false;"
        "assignedFalse.async=false;"
        "const falseCleared=assignedFalse.async===false;"
        "globalThis.asyncStateOk=initial&&reflected&&rawCleared&&"
        "trueCleared&&falseCleared;"
        "globalThis.emptyScript=document.createElement('script');"
        "emptyScript.src='';emptyScript.addEventListener('error',()=>{"
        "emptyErrors++;});document.head.appendChild(emptyScript);"
        "emptyScript.src='/should-not-run.js';"
        "globalThis.lateExternal=document.createElement('script');"
        "lateExternal.addEventListener('load',()=>{"
        "lateExternal.textContent='globalThis.shouldNotRun+=10;';"
        "lateExternal.src='/should-not-run.js';lateExternal.remove();"
        "document.head.appendChild(lateExternal);"
        "globalThis.pocSummary='external-state:' + "
        "(asyncStateOk?'async':'async-wrong')+':' + emptyErrors+':' + "
        "lateExternalRuns+':' + shouldNotRun;});"
        "document.head.appendChild(lateExternal);"
        "lateExternal.src='/late.js';"
        "globalThis.pocSummary='external-state:pending';";
    static const char expected[] = "external-state:async:1:1:0";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(&browser, "https://external-state.test/late.js",
                       late_source));
    REQUIRE(put_script(
        &browser, "https://external-state.test/should-not-run.js",
        should_not_run_source));
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://external-state.test/",
        empty_page, 8, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    size_t hits = browser.cache_hits;
    size_t requests = navigation.page.script_result.network_requests;
    REQUIRE(evaluate(&navigation, exercise, "<external-script-state>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "external-state:pending") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, expected, 8, &advances));
    REQUIRE(browser.cache_hits == hits + 1u);
    REQUIRE(navigation.page.script_result.network_requests == requests + 1u);
    for (size_t step = 0; step < 3; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(strcmp(navigation.page.script_result.summary, expected) == 0);

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

static bool test_inline_module_deferred_current_script(Budget *budget)
{
    const char *test_name = "inline module deferred currentScript semantics";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char append_module[] =
        "globalThis.inlineModuleRuns=0;globalThis.inlineModuleCurrent=false;"
        "globalThis.inlineModule=document.createElement('script');"
        "inlineModule.type='module';"
        "inlineModule.textContent='globalThis.inlineModuleRuns++;"
        "globalThis.inlineModuleCurrent=document.currentScript===null;';"
        "inlineModule.addEventListener('load',()=>{globalThis.pocSummary="
        "'inline-module:' + inlineModuleRuns+':' + "
        "(inlineModuleCurrent?'null':'wrong')+':' + "
        "(document.currentScript===null?'restored':'leaked');});"
        "document.head.appendChild(inlineModule);"
        "globalThis.pocSummary='inline-module:pending';";
    static const char append_pending[] =
        "const pending=document.createElement('script');"
        "pending.type='module';pending.textContent='globalThis.neverObserved=1;';"
        "document.head.appendChild(pending);"
        "globalThis.pocSummary='inline-module:teardown-pending';";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://inline-module.test/",
        empty_page, 8, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(evaluate(&navigation, append_module, "<append-inline-module>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "inline-module:pending") == 0);
    size_t advances = 0;
    REQUIRE(advance_until(
        &navigation, "inline-module:1:null:restored", 8, &advances));
    REQUIRE(advances > 0);
    REQUIRE(evaluate(&navigation, append_pending,
                     "<append-pending-inline-module>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "inline-module:teardown-pending") == 0);

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

static bool test_script_state_capacity_is_explicit(Budget *budget)
{
    const char *test_name = "script-element state capacity is explicit";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char exercise[] =
        "let allocated=0;for(let index=0;index<255;index++){const handle="
        "__tilefinchCreate('script','http://www.w3.org/1999/xhtml');"
        "if(!handle)break;allocated++;}"
        "const container=document.createElement('div'),last="
        "document.createElement('script');"
        "if(last)container.appendChild(last);"
        "const cloneFailed=last&&container.cloneNode(true)===null;"
        "const createFailed=document.createElement('script')===null;"
        "globalThis.pocSummary='state-cap:' + "
        "(allocated===255&&last?'filled':'early')+':' + "
        "(cloneFailed?'clone-explicit':'clone-silent')+':' + "
        "(createFailed?'create-explicit':'create-recovered');";
    static const char expected[] =
        "state-cap:filled:clone-explicit:create-recovered";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://state-cap.test/",
        empty_page, 8, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    bool evaluated = evaluate(
        &navigation, exercise, "<script-state-capacity>");
    if (!evaluated) {
        fprintf(stderr, "state capacity evaluation error: %s\n",
                navigation.page.script_result.error);
    }
    REQUIRE(evaluated);
    if (strcmp(navigation.page.script_result.summary, expected) != 0) {
        fprintf(stderr, "state capacity summary: expected=\"%s\" got=\"%s\"\n",
                expected, navigation.page.script_result.summary);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary, expected) == 0);

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

static bool test_pending_dynamic_element_destruction(Budget *budget)
{
    const char *test_name =
        "detached pending dynamic script retains prepared execution";
    static const char empty_page[] =
        "<!doctype html><head></head><body></body>";
    static const char doomed_source[] =
        "globalThis.destroyedPendingRan=true;";
    static const char append_and_destroy[] =
        "globalThis.destroyedPendingRan=false;globalThis.destroyedEvents=[];"
        "const holder=document.createElement('div');"
        "document.body.appendChild(holder);"
        "const doomed=document.createElement('script');doomed.src='/doomed.js';"
        "doomed.addEventListener('load',()=>destroyedEvents.push('load'));"
        "doomed.addEventListener('error',()=>destroyedEvents.push('error'));"
        "holder.appendChild(doomed);"
        "holder.innerHTML='<span>replacement</span>';"
        "globalThis.pocSummary='pending-destroy:removed';";
    static const char inspect[] =
        "globalThis.pocSummary='pending-destroy:' + "
        "(destroyedPendingRan?'ran':'inert')+':' + "
        "(destroyedEvents.length?destroyedEvents.join(','):'quiet');";
    ScriptReplayRecord records[] = {
        {.url = "https://pending-destroy.test/doomed.js",
         .source = doomed_source, .delay_pumps = 2}
    };
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
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://pending-destroy.test/",
        empty_page, 1, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(evaluate(
        &navigation, append_and_destroy, "<append-and-destroy-script>"));
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "pending-destroy:removed") == 0);
    for (size_t step = 0; step < 8u; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(evaluate(&navigation, inspect, "<inspect-destroyed-script>"));
    if (strcmp(navigation.page.script_result.summary,
               "pending-destroy:ran:load") != 0) {
        fprintf(stderr, "pending destroy summary: %s error: %s\n",
                navigation.page.script_result.summary,
                navigation.page.script_result.error);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "pending-destroy:ran:load") == 0);
    REQUIRE(navigation.page.script_result.dynamic_scripts_completed == 1u);

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

static bool test_initial_script_snapshot_mutation(Budget *budget)
{
    const char *test_name = "initial script snapshot survives destructive DOM";
    static const char page[] =
        "<!doctype html><head>"
        "<script>globalThis.initialEvents=['first'];"
        "const executing=document.currentScript;"
        "document.head.innerHTML='<title>replaced</title>';"
        "initialEvents.push(document.currentScript===executing&&"
        "!executing.isConnected?'detached':'wrong');"
        "globalThis.pocSummary='initial-mutation:'+initialEvents.join(',');"
        "</script>"
        "<script>initialEvents.push('second');"
        "globalThis.pocSummary='initial-mutation:second-ran';</script>"
        "</head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://initial-mutation.test/",
        page, 2, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    if (strcmp(navigation.page.script_result.summary,
               "initial-mutation:first,detached") != 0) {
        fprintf(stderr, "initial mutation summary: %s error: %s\n",
                navigation.page.script_result.summary,
                navigation.page.script_result.error);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "initial-mutation:first,detached") == 0);

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

static bool test_external_script_self_destruction(Budget *budget)
{
    const char *test_name =
        "external script self-destruction skips stale resource event";
    static const char source[] =
        "globalThis.externalSelfRan=true;"
        "document.head.innerHTML='<title>replaced</title>';"
        "globalThis.pocSummary='external-self:ran';";
    static const char page[] =
        "<!doctype html><head>"
        "<script src='/self-destroy.js'></script>"
        "<script>globalThis.pocSummary='external-self:stale-ran';</script>"
        "</head><body></body>";
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(put_script(
        &browser, "https://external-self.test/self-destroy.js", source));
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://external-self.test/",
        page, 2, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    if (strcmp(navigation.page.script_result.summary,
               "external-self:ran") != 0) {
        fprintf(stderr, "external self summary: %s error: %s\n",
                navigation.page.script_result.summary,
                navigation.page.script_result.error);
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "external-self:ran") == 0);
    REQUIRE(navigation.script_parser_blocking == 1u);
    REQUIRE(navigation.script_loaded == 1u);

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

static bool test_deep_initial_script_snapshot(Budget *budget)
{
    const char *test_name = "deep initial script traversal is iterative";
    static const char prefix[] =
        "<!doctype html><body><div style='display:none'>";
    static const char open[] = "<div>";
    static const char close[] = "</div>";
    static const char script[] =
        "<script>globalThis.pocSummary='deep-initial:ran';</script>";
    const size_t depth = 1024;
    size_t length = sizeof(prefix) - 1u
        + depth * (sizeof(open) - 1u + sizeof(close) - 1u)
        + sizeof(script) - 1u + sizeof(close) - 1u;
    char *page = malloc(length + 1u);
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;
    REQUIRE(page != NULL);
    size_t used = 0;
    memcpy(page + used, prefix, sizeof(prefix) - 1u);
    used += sizeof(prefix) - 1u;
    for (size_t index = 0; index < depth; index++) {
        memcpy(page + used, open, sizeof(open) - 1u);
        used += sizeof(open) - 1u;
    }
    memcpy(page + used, script, sizeof(script) - 1u);
    used += sizeof(script) - 1u;
    for (size_t index = 0; index < depth + 1u; index++) {
        memcpy(page + used, close, sizeof(close) - 1u);
        used += sizeof(close) - 1u;
    }
    page[used] = '\0';
    REQUIRE(used == length);
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, "https://deep-initial.test/",
        page, 1, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   "deep-initial:ran") == 0);

cleanup:
    free(page);
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}
