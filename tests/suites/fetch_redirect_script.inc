static bool pump_script_network(ScriptRuntime *runtime, ScriptResult *result)
{
    for (size_t pump = 0; pump < 500; pump++) {
        if (!script_runtime_advance(runtime, 4, 4, result)) return false;
        if (result->pending_tasks == 0) return true;
    }
    return false;
}

static bool test_javascript_binary_responses(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    static const char html[] =
        "<!doctype html><title>Binary response</title><body></body>";
    PocDocument document = {0};
    ScriptResult result = {0};
    char document_url[256];
    snprintf(document_url, sizeof(document_url),
             "http://127.0.0.1:%u/page", port);
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1, 17));
    ScriptRuntime *runtime = script_runtime_create(
        &document, budget, 4u * MIB, 5000, document_url, &result);
    CHECK(runtime != NULL && result.success);

    static const char synchronous_probe[] =
        "(()=>{const own=(object,name)=>Object.prototype.hasOwnProperty.call("
        "object,name),raw=__tilefinchFetchSync('GET','/binary',undefined,'','',"
        "'cors','same-origin'),bytes=new Uint8Array(raw.bodyBytes),"
        "textRaw=__tilefinchFetchSync('GET','/final',undefined,'','','cors',"
        "'same-origin'),textXhr=new XMLHttpRequest;"
        "textXhr.open('GET','/binary',false);textXhr.send();"
        "const arrayXhr=new XMLHttpRequest;arrayXhr.open('GET','/binary',false);"
        "arrayXhr.responseType='arraybuffer';arrayXhr.send();"
        "const arrayBytes=new Uint8Array(arrayXhr.response),"
        "textArrayXhr=new XMLHttpRequest;"
        "textArrayXhr.open('GET','/final',false);"
        "textArrayXhr.responseType='arraybuffer';textArrayXhr.send();"
        "const textArray=new TextDecoder().decode(textArrayXhr.response);"
        "let guarded=false;"
        "try{void arrayXhr.responseText;}catch(error){guarded=error.name==="
        "'InvalidStateError';}const jsonXhr=new XMLHttpRequest;"
        "jsonXhr.open('GET','/binary-json',false);jsonXhr.responseType='json';"
        "jsonXhr.send();const blobXhr=new XMLHttpRequest;"
        "blobXhr.open('GET','/binary',false);blobXhr.responseType='blob';"
        "blobXhr.send();const checks=[!own(raw,'body'),"
        "raw.bodyBytes instanceof ArrayBuffer,raw.bodyLength===5,"
        "bytes.join(',')==='0,65,195,169,66',own(textRaw,'body'),"
        "!own(textRaw,'bodyBytes'),textRaw.bodyLength===textRaw.body.length,"
        "textXhr.responseText.length===4,textXhr.responseText.charCodeAt(0)===0,"
        "textXhr.responseText.charCodeAt(2)===233,textXhr.response==="
        "textXhr.responseText,arrayBytes.join(',')==='0,65,195,169,66',"
        "textArray.startsWith('method=GET;'),"
        "guarded,jsonXhr.response&&jsonXhr.response.ok===true&&"
        "jsonXhr.response.n===7,blobXhr.response instanceof Blob&&"
        "blobXhr.response.size===5&&blobXhr.response.type==="
        "'application/octet-stream'];globalThis.pocSummary=checks.every(Boolean)"
        "?'binary-sync-ok':'binary-sync-failed:'+checks.join(',');})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, synchronous_probe, "<binary-sync-probe>", &result)
          && strcmp(result.summary, "binary-sync-ok") == 0
          && result.last_xhr_response_bytes == 5
          && result.last_xhr_response_text_units == 0
          && strcmp(result.last_xhr_response_type, "blob") == 0);

    static const char asynchronous_probe[] =
        "(()=>{const checks=[];globalThis.pocSummary='pending';"
        "fetch('/binary').then(response=>{checks.push(response._body===null,"
        "response._bytes instanceof Uint8Array&&!response.bodyUsed);"
        "const clone=response.clone();return response.arrayBuffer().then("
        "buffer=>{checks.push(new Uint8Array(buffer).join(',')==="
        "'0,65,195,169,66',response.bodyUsed,response._bytes===null&&"
        "response._body===null);return clone.text();}).then(text=>checks.push("
        "text.length===4,text.charCodeAt(0)===0,text.charCodeAt(2)===233,"
        "clone.bodyUsed,clone._bytes===null,clone._body===null));}).then("
        "()=>fetch('/binary-json'))"
        ".then(response=>{checks.push(response._body===null);return "
        "response.json();}).then(value=>{checks.push(value.ok===true&&"
        "value.n===7);const response=new Response('A\\u00e9B');return "
        "response.arrayBuffer().then(buffer=>checks.push("
        "new Uint8Array(buffer).join(',')==='65,195,169,66',"
        "response.bodyUsed,response._bytes===null&&response._body===null));})"
        ".then(()=>{globalThis.pocSummary=checks.every(Boolean)?"
        "'binary-fetch-ok':'binary-fetch-failed:'+checks.join(',');})"
        ".catch(error=>globalThis.pocSummary='binary-fetch-error:'+error);})()";
    bool binary_evaluated = script_runtime_evaluate_diagnostic(
        runtime, asynchronous_probe, "<binary-fetch-probe>", &result);
    bool binary_pumped = binary_evaluated
        && pump_script_network(runtime, &result);
    bool binary_valid = binary_pumped
        && strcmp(result.summary, "binary-fetch-ok") == 0;
    if (!binary_valid) {
        fprintf(stderr,
                "binary-fetch evaluated=%d pumped=%d summary=\"%s\" "
                "error=\"%s\"\n",
                binary_evaluated, binary_pumped, result.summary,
                result.error);
    }
    CHECK(binary_valid);

    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return true;
}

static bool browser_engine_promoted_shell_is_live(BrowserEngine *engine)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    BrowserController *controller = browser_engine_controller(engine);
    TileCache *render = browser_engine_render_shell(engine);
    if (navigation == NULL || controller == NULL || render == NULL
        || !navigation->page.loaded
        || controller->navigation != navigation
        || render->source_layout != &navigation->page.layout) return false;
    if (render->owns_visual_layout) {
        if (render->layout != &render->visual_layout) return false;
    } else if (render->layout != render->source_layout) {
        return false;
    }
    return browser_engine_render_frame(engine, NULL);
}

static bool test_browser_engine_url_replay_promotion(
    Budget *suite_budget, unsigned port, const char *trace_directory)
{
    if (suite_budget == NULL || suite_budget->current != 0
        || !budget_uninstall_lexbor(suite_budget)) return false;
    static const char incumbent[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>Replay incumbent</title><body><a href='#old'>old</a>";
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 16u * MIB;
    config.session_cache_limit = 256u * 1024u;
    config.maximum_document_bytes = 64u * 1024u;
    config.tile_capacity = 2;
    char error[256] = {0}, url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/nav-final", port);

    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    bool captured = engine != NULL
        && browser_engine_commit_html(
               engine, "http://127.0.0.1/incumbent", incumbent,
               sizeof(incumbent) - 1, true)
        && fetch_trace_capture_begin(trace_directory, error, sizeof(error))
        && browser_engine_load_url(engine, url, true);
    fetch_trace_end();
    captured = captured
        && strcmp(browser_engine_navigation(engine)->page.document.title,
                  "Redirected") == 0
        && browser_engine_navigation(engine)->loads_committed == 2
        && browser_engine_promoted_shell_is_live(engine)
        && browser_engine_shutdown(engine);
    browser_engine_destroy(engine);
    if (!captured) {
        (void) budget_install_lexbor(suite_budget);
        return false;
    }

    engine = browser_engine_create(&config, error, sizeof(error));
    bool replayed = engine != NULL
        && browser_engine_commit_html(
               engine, "http://127.0.0.1/incumbent", incumbent,
               sizeof(incumbent) - 1, true)
        && fetch_trace_replay_begin(trace_directory, error, sizeof(error))
        && browser_engine_load_url(engine, url, true);
    fetch_trace_end();
    replayed = replayed
        && strcmp(browser_engine_navigation(engine)->page.document.title,
                  "Redirected") == 0
        && browser_engine_navigation(engine)->loads_committed == 2
        && browser_engine_navigation(engine)->performance
               .slices[NAVIGATION_SLICE_PARSER].slices != 0
        && browser_engine_navigation(engine)->performance
               .slices[NAVIGATION_SLICE_LAYOUT].slices != 0
        && browser_engine_promoted_shell_is_live(engine)
        && browser_engine_shutdown(engine);
    browser_engine_destroy(engine);
    return budget_install_lexbor(suite_budget) && replayed;
}

static bool test_javascript_credentials(Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    static const char html[] =
        "<!doctype html><title>Credentials</title><body></body>";
    PocDocument document = {0};
    BrowserSession session;
    ScriptResult result = {0};
    char document_url[256], cross_url[256], cookie_url[256];
    snprintf(document_url, sizeof(document_url),
             "http://localhost:%u/page", port);
    snprintf(cross_url, sizeof(cross_url),
             "http://127.0.0.1:%u/script-start", port);
    snprintf(cookie_url, sizeof(cookie_url),
             "http://127.0.0.1:%u/", port);
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1, 17));
    CHECK(browser_session_init(&session, budget, 64u * 1024u));
    CHECK(browser_session_cookie_set_http(
              &session, document_url, "local=allowed; Path=/")
          && browser_session_cookie_set_http(
              &session, cookie_url, "cross=secret; Path=/"));
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, budget, 4u * MIB, 5000, document_url, &session, &result);
    CHECK(runtime != NULL && result.success);

    static const char omit_source[] =
        "globalThis.pocSummary='pending';"
        "fetch('/same',{credentials:'omit'}).then(response=>response.text())"
        ".then(text=>globalThis.pocSummary=text)"
        ".catch(error=>globalThis.pocSummary='error:'+error)";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, omit_source, "<credentials-omit>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "cookie=<none>") != NULL
          && strstr(result.summary, "origin=<none>") != NULL
          && strstr(result.summary, "fetch-dest=empty") != NULL
          && strstr(result.summary, "fetch-mode=cors") != NULL
          && strstr(result.summary, "fetch-site=same-origin") != NULL);
    char cookies[512];
    CHECK(browser_session_cookie_header(
              &session, document_url, cookies, sizeof(cookies))
          && strcmp(cookies, "local=allowed") == 0);

    char cross_source[768];
    int written = snprintf(
        cross_source, sizeof(cross_source),
        "globalThis.pocSummary='pending';"
        "(()=>{const script=document.createElement('script');"
        "script.src='%s';script.addEventListener('error',()=>{"
        "globalThis.pocSummary='script-error'});"
        "document.head.appendChild(script)})()", cross_url);
    CHECK(written > 0 && (size_t) written < sizeof(cross_source));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, cross_source, "<credentials-cross-script>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "cookie=<none>") != NULL
          && strstr(result.summary, "origin=<none>") != NULL
          && strstr(result.summary, "fetch-dest=script") != NULL
          && strstr(result.summary, "fetch-mode=no-cors") != NULL
          && strstr(result.summary, "fetch-site=cross-site") != NULL
          && browser_session_cookie_header(
                 &session, cookie_url, cookies, sizeof(cookies))
          /* Classic no-CORS scripts use include credentials. SameSite=Lax
             withholds the seeded cookie, and the default third-party policy
             also ignores ordinary redirect/final response cookies. */
          && strstr(cookies, "cross=secret") != NULL
          && strstr(cookies, "script_hop=seen") == NULL
          && strstr(cookies, "script_final=seen") == NULL);

    static const char same_origin_source[] =
        "globalThis.pocSummary='pending';"
        "fetch('/same',{credentials:'same-origin'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary=text)"
        ".catch(error=>globalThis.pocSummary='error:'+error)";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, same_origin_source, "<credentials-same-origin>",
              &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "local=allowed") != NULL
          && strstr(result.summary, "hop=seen") != NULL
          && strstr(result.summary, "origin=<none>") != NULL
          && strstr(result.summary, "fetch-dest=empty") != NULL
          && strstr(result.summary, "fetch-mode=cors") != NULL
          && strstr(result.summary, "fetch-site=same-origin") != NULL
          && browser_session_cookie_header(
                 &session, document_url, cookies, sizeof(cookies))
          && strstr(cookies, "local=allowed") != NULL
          && strstr(cookies, "hop=seen") != NULL
          && strstr(cookies, "final_cookie=seen") != NULL);

    static const char post_source[] =
        "(()=>{const xhr=new XMLHttpRequest;"
        "xhr.open('POST','/method',false);xhr.send('payload');"
        "globalThis.pocSummary=xhr.responseText})()";
    char expected_origin[256];
    snprintf(expected_origin, sizeof(expected_origin),
             "origin=http://localhost:%u", port);
    bool post_evaluated = script_runtime_evaluate_diagnostic(
        runtime, post_source, "<xhr-origin>", &result);
    bool post_valid = post_evaluated
        && strstr(result.summary, "method=POST") != NULL
        && strstr(result.summary, "body=payload") != NULL
        && strstr(result.summary, expected_origin) != NULL
        && strstr(result.summary, "fetch-dest=empty") != NULL
        && strstr(result.summary, "fetch-mode=cors") != NULL
        && strstr(result.summary, "fetch-site=same-origin") != NULL;
    if (!post_valid) {
        fprintf(stderr,
                "xhr-origin evaluated=%d summary=\"%s\" error=\"%s\"\n",
                post_evaluated, result.summary, result.error);
    }
    CHECK(post_valid);

    script_runtime_destroy(runtime);
    char redirect_document_url[256];
    snprintf(redirect_document_url, sizeof(redirect_document_url),
             "http://127.0.0.1:%u/page", port);
    result = (ScriptResult) {0};
    runtime = script_runtime_create_with_session(
        &document, budget, 4u * MIB, 5000, redirect_document_url,
        &session, &result);
    static const char cross_redirect_source[] =
        "globalThis.pocSummary='pending';"
        "fetch('/cross',{credentials:'include'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)";
    CHECK(runtime != NULL && result.success
          && script_runtime_evaluate_diagnostic(
                 runtime, cross_redirect_source,
                 "<cross-origin-redirect>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:TypeError:") != NULL
          && strstr(result.summary, "redirect") != NULL);
    script_runtime_destroy(runtime);
    browser_session_destroy(&session);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return true;
}

static bool test_javascript_cors(Budget *budget, unsigned document_port,
                                 unsigned target_port)
{
    size_t baseline = budget->current;
    static const char html[] =
        "<!doctype html><title>CORS</title><body></body>";
    PocDocument document = {0};
    BrowserSession session;
    ScriptResult result = {0};
    char document_url[256], cookie_url[256], target[256], source[4096];
    snprintf(document_url, sizeof(document_url),
             "http://localhost:%u/page", document_port);
    snprintf(cookie_url, sizeof(cookie_url),
             "http://127.0.0.1:%u/", target_port);
    FetchRequestValidationError validation = FETCH_REQUEST_VALIDATION_OK;
    FetchRequest forged_preflight = {
        .method = "GET",
        .extra_headers = "Access-Control-Request-Method: PUT"
    };
    CHECK(!fetch_request_validate(&forged_preflight, &validation)
          && validation == FETCH_REQUEST_VALIDATION_EXTRA_HEADERS);
    forged_preflight = (FetchRequest) {
        .method = "GET", .origin = "http://localhost.invalid",
        .extra_headers = "Access-Control-Request-Method: PUT",
        .cors_preflight = true,
        .credentials = FETCH_CREDENTIALS_SAME_ORIGIN,
        .credential_origin = "http://localhost.invalid",
        .enforce_cors = true, .redirect_same_origin_only = true
    };
    CHECK(!fetch_request_validate(&forged_preflight, &validation)
          && validation == FETCH_REQUEST_VALIDATION_EXTRA_HEADERS);
    CHECK(document_parse(&document, budget, html, sizeof(html) - 1, 17));
    CHECK(browser_session_init(&session, budget, 64u * 1024u)
          && browser_session_cookie_set_http(
                 &session, cookie_url, "cross=secret; Path=/"));
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, budget, 4u * MIB, 5000, document_url, &session, &result);
    CHECK(runtime != NULL && result.success);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-simple", target_port);
    int written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s',{credentials:'same-origin'})"
        ".then(async response=>{const text=await response.text();"
        "return 'simple:'+text+'|visible='"
        "+response.headers.get('x-cors-visible')+'|secret='"
        "+response.headers.get('x-cors-secret')+'|acao='"
        "+response.headers.get('access-control-allow-origin')})"
        ".then(value=>globalThis.pocSummary=value)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-simple>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "simple:cors-method=GET") != NULL
          && strstr(result.summary, "cookie=<none>") != NULL
          && strstr(result.summary, "|visible=visible") != NULL
          && strstr(result.summary, "|secret=null") != NULL
          && strstr(result.summary, "|acao=null") != NULL);
    char cookies[512];
    CHECK(browser_session_cookie_header(
              &session, cookie_url, cookies, sizeof(cookies))
          && strcmp(cookies, "cross=secret") == 0);

    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';"
        "fetch('%s?wildcard=1',{credentials:'omit'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='wildcard:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-wildcard>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "wildcard:cors-method=GET") != NULL
          && strstr(result.summary, "cookie=<none>") != NULL);

    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';"
        "fetch('%s?credentials=1',{credentials:'include'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='credentialed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    bool credentialed = written > 0 && (size_t) written < sizeof(source)
        && script_runtime_evaluate_diagnostic(
               runtime, source, "<cors-credentials>", &result)
        && pump_script_network(runtime, &result);
    bool read_cross_cookies = browser_session_cookie_header(
        &session, cookie_url, cookies, sizeof(cookies));
    CHECK(credentialed
          && strstr(result.summary, "credentialed:cors-method=GET") != NULL
          /* SameSite still applies to credentials:include.  The fixture's
             default-Lax cookie is correctly withheld on this cross-site
             subresource request, while an accepted response cookie commits. */
          && strstr(result.summary, "cookie=<none>") != NULL
          && read_cross_cookies
          && strstr(cookies, "cors_response=seen") == NULL);

    /* The response becomes eligible only after an explicit bounded grant for
       the top-level site. The request cookie remains withheld by SameSite. */
    CHECK(browser_session_set_third_party_cookie_site_allowed(
              &session, document_url, true));
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';"
        "fetch('%s?credentials=1',{credentials:'include'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='credentialed-grant:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-credentials-granted>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary,
                    "credentialed-grant:cors-method=GET") != NULL
          && strstr(result.summary, "cookie=<none>") != NULL
          && browser_session_cookie_header(
                 &session, cookie_url, cookies, sizeof(cookies))
          && strstr(cookies, "cors_response=seen") != NULL);

    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';"
        "fetch('%s?wildcard=1&credentials=1',{credentials:'include'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-credential-wildcard>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:TypeError:") != NULL
          && strstr(result.summary, "exposed:") == NULL);

    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s?duplicate=1')"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-duplicate-origin>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:TypeError:") != NULL
          && strstr(result.summary, "exposed:") == NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-denied", target_port);
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s')"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-denied>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:TypeError:") != NULL
          && strstr(result.summary, "cors-body-must-not-be-exposed") == NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-simple", target_port);
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';"
        "fetch('%s',{mode:'same-origin'})"
        ".then(()=>globalThis.pocSummary='exposed')"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-same-origin-mode>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:") != NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-preflight", target_port);
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s',{method:'PUT',"
        "headers:{'Content-Type':'application/json','X-Test':'yes'},"
        "body:'{\\\"ok\\\":1}'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='preflight:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-preflight>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "preflight:cors-method=PUT") != NULL
          && strstr(result.summary, "x-test=yes") != NULL
          && strstr(result.summary, "body={\"ok\":1}") != NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-preflight-denied", target_port);
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s',{method:'PUT',"
        "headers:{'Content-Type':'application/json','X-Test':'yes'},"
        "body:'denied'})"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-preflight-denied>", &result)
          && pump_script_network(runtime, &result)
          && strstr(result.summary, "blocked:TypeError: CORS preflight failed")
                 != NULL
          && strstr(result.summary, "exposed:") == NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-cross-redirect", target_port);
    written = snprintf(
        source, sizeof(source),
        "globalThis.pocSummary='pending';fetch('%s')"
        ".then(response=>response.text())"
        ".then(text=>globalThis.pocSummary='exposed:'+text)"
        ".catch(error=>globalThis.pocSummary='blocked:'+error)", target);
    bool redirect_blocked = written > 0
        && (size_t) written < sizeof(source)
        && script_runtime_evaluate_diagnostic(
               runtime, source, "<cors-cross-redirect>", &result)
        && pump_script_network(runtime, &result);
    CHECK(redirect_blocked
          && strstr(result.summary, "blocked:TypeError:") != NULL
          && strstr(result.summary, "exposed:") == NULL);

    snprintf(target, sizeof(target),
             "http://127.0.0.1:%u/cors-simple", target_port);
    written = snprintf(
        source, sizeof(source),
        "(()=>{const xhr=new XMLHttpRequest;xhr.open('GET','%s',false);"
        "xhr.send();globalThis.pocSummary='xhr:'+xhr.status+':'"
        "+xhr.responseText+':visible='"
        "+xhr.getResponseHeader('x-cors-visible')+':secret='"
        "+xhr.getResponseHeader('x-cors-secret')})()", target);
    CHECK(written > 0 && (size_t) written < sizeof(source)
          && script_runtime_evaluate_diagnostic(
                 runtime, source, "<cors-sync-xhr>", &result)
          && strstr(result.summary, "xhr:200:cors-method=GET") != NULL
          && strstr(result.summary, ":visible=visible:secret=null") != NULL);

    script_runtime_destroy(runtime);
    browser_session_destroy(&session);
    document_destroy(&document);
    CHECK(budget->current == baseline);
    return true;
}

typedef struct {
    size_t header_calls;
    size_t body_bytes;
} CorsChainProbe;

static bool cors_chain_headers(void *opaque, const FetchResult *metadata)
{
    CorsChainProbe *probe = opaque;
    probe->header_calls++;
    return metadata->status_code == 200;
}

static bool cors_chain_body(void *opaque, const unsigned char *data,
                            size_t length)
{
    (void) data;
    CorsChainProbe *probe = opaque;
    probe->body_bytes += length;
    return true;
}

static FetchRequest cors_chain_request(char origin[128], char initiator[256],
                                       unsigned port)
{
    snprintf(origin, 128, "http://127.0.0.1:%u", port);
    snprintf(initiator, 256,
             "http://127.0.0.1:%u/page", port);
    return (FetchRequest) {
        .method = "GET", .cookie = "cors_secret=must-not-return",
        .origin = origin, .allow_http_errors = true,
        .credentials = FETCH_CREDENTIALS_SAME_ORIGIN,
        .credential_origin = origin, .initiator_url = initiator,
        .enforce_cors = true, .redirect_same_origin_only = false
    };
}

static bool run_cors_chain_case(Budget *budget, const char *url,
                                unsigned initiator_port, bool allowed)
{
    size_t baseline = budget->current;
    char origin[128], initiator[256];
    FetchRequest request = cors_chain_request(
        origin, initiator, initiator_port);
    FetchResult result = {.budget = budget};
    bool sync_success = fetch_redirect(budget, url, &request, &result);
    CHECK(sync_success == allowed && result.redirect_origin_tainted);
    if (allowed) {
        CHECK(result.data != NULL
              && strstr(result.data, "cors-chain-final") != NULL
              && strstr(result.data, "origin=null") != NULL
              && strstr(result.data, "cookie=<none>") != NULL);
    } else {
        CHECK(result.data == NULL
              && strstr(result.error, "CORS response denied") != NULL);
    }
    fetch_result_destroy(&result);

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64u * 1024u);
    CHECK(scheduler != NULL);
    CorsChainProbe probe = {0};
    FetchStreamOptions stream = {
        .on_headers = cors_chain_headers,
        .on_body = cors_chain_body,
        .opaque = &probe
    };
    uint64_t id = fetch_scheduler_enqueue_stream(
        scheduler, url, &request, 64u * 1024u, 5000, &stream);
    bool scheduled_success = false, taken = false;
    FetchStreamMetrics metrics = {0};
    result = (FetchResult) {.budget = budget};
    for (size_t pump = 0; id != 0 && pump < 2000 && !taken; pump++) {
        (void) fetch_scheduler_pump(scheduler, 1, 4);
        taken = fetch_scheduler_take_stream(
            scheduler, id, &scheduled_success, &metrics, &result);
    }
    CHECK(id != 0 && taken && scheduled_success == allowed
          && result.redirect_origin_tainted);
    if (allowed) {
        CHECK(probe.header_calls == 1 && probe.body_bytes != 0
              && metrics.headers_delivered);
    } else {
        CHECK(probe.header_calls == 0 && probe.body_bytes == 0
              && !metrics.headers_delivered
              && strstr(result.error, "CORS response denied") != NULL);
    }
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return budget->current == baseline;
}

static bool run_cors_cache_304_case(Budget *budget, const char *url,
                                    unsigned initiator_port,
                                    bool cached_validated, bool allowed)
{
    size_t baseline = budget->current;
    char origin[128], initiator[256];
    FetchRequest request = cors_chain_request(
        origin, initiator, initiator_port);
    request.credentials = FETCH_CREDENTIALS_INCLUDE;
    request.if_none_match = "\"cors-cache\"";
    request.cors_cached_response_validated = cached_validated;

    FetchResult result = {.budget = budget};
    bool success = fetch_redirect(budget, url, &request, &result);
    CHECK(success == allowed && !result.redirect_origin_tainted
          && (!allowed || result.status_code == 304));
    if (!allowed) {
        CHECK(result.data == NULL
              && strstr(result.error, "CORS response denied") != NULL);
    }
    fetch_result_destroy(&result);

    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64u * 1024u);
    CHECK(scheduler != NULL);
    result = (FetchResult) {.budget = budget};
    success = fetch_scheduler_request(
        scheduler, url, &request, 64u * 1024u, 5000, &result);
    CHECK(success == allowed && !result.redirect_origin_tainted
          && (!allowed || result.status_code == 304));
    if (!allowed) {
        CHECK(result.data == NULL
              && strstr(result.error, "CORS response denied") != NULL);
    }
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return budget->current == baseline;
}

static bool test_scheduled_redirect_taint_on_terminal_failure(
    Budget *budget, unsigned port)
{
    size_t baseline = budget->current;
    char url[512], origin[128], initiator[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-cross-unreachable", port);
    FetchRequest request = cors_chain_request(origin, initiator, port);
    FetchScheduler *scheduler = fetch_scheduler_create(
        budget, 1, 64u * 1024u);
    CHECK(scheduler != NULL);
    FetchResult result = {.budget = budget};
    bool success = fetch_scheduler_request(
        scheduler, url, &request, 64u * 1024u, 2000, &result);
    CHECK(!success && result.redirect_origin_tainted
          && strstr(result.error, "CORS response denied") == NULL);
    fetch_result_destroy(&result);
    fetch_scheduler_destroy(scheduler);
    return budget->current == baseline;
}

static bool test_redirect_cors_final_response_boundary(
    Budget *budget, unsigned port, unsigned cross_port)
{
    char url[512];
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross?allow=1", port);
    CHECK(run_cors_chain_case(budget, url, port, true));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross?allow=0", port);
    CHECK(run_cors_chain_case(budget, url, port, false));

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-cross-same"
             "?allow_redirect=1&allow_final=1", cross_port);
    CHECK(run_cors_chain_case(budget, url, port, true));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-cross-same"
             "?allow_redirect=1&allow_final=0", cross_port);
    CHECK(run_cors_chain_case(budget, url, port, false));

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross-same"
             "?allow_middle=1&allow_final=1", port);
    CHECK(run_cors_chain_case(budget, url, port, true));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross-same"
             "?allow_middle=1&allow_final=0", port);
    CHECK(run_cors_chain_case(budget, url, port, false));
    /* The tainted intermediate redirect is itself a CORS response and must
       fail before the final same-origin hop is attempted. */
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross-same"
             "?allow_middle=0&allow_final=1", port);
    CHECK(run_cors_chain_case(budget, url, port, false));

    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-cache-304", cross_port);
    CHECK(run_cors_cache_304_case(budget, url, port, false, false));
    CHECK(run_cors_cache_304_case(budget, url, port, true, true));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-cache-304?acao=bad", cross_port);
    CHECK(run_cors_cache_304_case(budget, url, port, true, false));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-cache-304?acao=origin", cross_port);
    CHECK(run_cors_cache_304_case(budget, url, port, true, true));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-cache-304"
             "?acao=origin&acac=false", cross_port);
    CHECK(run_cors_cache_304_case(budget, url, port, true, false));
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-cache-304"
             "?acao=origin&acac=true", cross_port);
    CHECK(run_cors_cache_304_case(budget, url, port, true, true));
    CHECK(test_scheduled_redirect_taint_on_terminal_failure(budget, port));
    return true;
}

static bool test_redirect_cors_trace_taint(
    Budget *budget, unsigned port, const char *trace_directory)
{
    size_t baseline = budget->current;
    char url[512], origin[128], initiator[256], error[256] = {0};
    snprintf(url, sizeof(url),
             "http://127.0.0.1:%u/cors-chain-same-cross-same"
             "?allow_middle=1&allow_final=1", port);
    FetchRequest request = cors_chain_request(origin, initiator, port);
    request.if_none_match = "\"trace-cors\"";
    FetchResult result = {.budget = budget};
    CHECK(fetch_trace_capture_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &request, &result)
          && result.redirect_origin_tainted
          && strstr(result.data, "origin=null") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && fetch_redirect(budget, url, &request, &result)
          && result.redirect_origin_tainted
          && strstr(result.data, "origin=null") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    FetchRequest mismatch = request;
    mismatch.enforce_cors = false;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && !fetch_redirect(budget, url, &mismatch, &result)
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    mismatch = request;
    mismatch.redirect_same_origin_only = true;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && !fetch_redirect(budget, url, &mismatch, &result)
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    mismatch = request;
    mismatch.cors_cached_response_validated = true;
    result = (FetchResult) {.budget = budget};
    CHECK(fetch_trace_replay_begin(trace_directory, error, sizeof(error))
          && !fetch_redirect(budget, url, &mismatch, &result)
          && strstr(result.error, "does not match") != NULL);
    fetch_result_destroy(&result);
    fetch_trace_end();

    char meta_path[4096];
    int meta_written = snprintf(
        meta_path, sizeof(meta_path), "%s/0000.meta", trace_directory);
    CHECK(meta_written > 0 && (size_t) meta_written < sizeof(meta_path));

    /* Retained records which predate explicit redirect taint cannot safely be
       replayed for CORS-mode requests: a same-origin final URL does not prove
       that the redirect chain stayed same-origin. */
    CHECK(trace_rewrite_record_version(
              trace_directory, 0, "11", "7")
          && replace_trace_byte(
              meta_path, "redirect-origin-tainted", 'x')
          && trace_replay_rejects(
              budget, trace_directory, url, &request)
          && replace_trace_byte(
              meta_path, "xedirect-origin-tainted", 'r')
          && trace_rewrite_record_version(
              trace_directory, 0, "7", "11"));

    /* Corpus startup now validates the complete finalized record inventory,
       including every body, before serving requests.  With that stronger
       preflight satisfied, a metadata-denied CORS response must still reject
       before exposing any body bytes to the caller. */
    static const char allow_null[] = "access-control-allow-origin: null";
    static const char deny_null[] = "access-control-allow-origin: nulx";
    static const char typed_allow_null[] =
        "response-security-allow-origin=null";
    static const char typed_deny_null[] =
        "response-security-allow-origin=nulx";
    const size_t allow_last = sizeof(allow_null) - 2;
    bool header_denied = replace_trace_byte_at_offset(
        meta_path, allow_null, allow_last, 'x')
        && replace_trace_byte_at_offset(
            meta_path, typed_allow_null,
            sizeof(typed_allow_null) - 2, 'x');
    result = (FetchResult) {.budget = budget};
    bool denied_before_body = header_denied
        && fetch_trace_replay_begin(trace_directory, error, sizeof(error))
        && !fetch_redirect(budget, url, &request, &result)
        && result.data == NULL
        && strstr(result.error, "CORS response denied") != NULL;
    fetch_trace_end();
    fetch_result_destroy(&result);
    bool header_restored = header_denied
        && replace_trace_byte_at_offset(
            meta_path, deny_null, allow_last, 'l')
        && replace_trace_byte_at_offset(
            meta_path, typed_deny_null,
            sizeof(typed_deny_null) - 2, 'l');
    CHECK(denied_before_body && header_restored);
    return budget->current == baseline;
}
