/* Range.createContextualFragment (DOM Parsing). Reddit's bundles call it
   and failed with "not a function". It parses with the range's start node
   (or that node's parent element) as the fragment parser's context, and,
   unlike innerHTML, leaves the parsed scripts unstarted and script-inserted:
   they run once, through the ordinary insertion path and its CSP checks,
   when the fragment is connected. */

typedef struct {
    const char *name;
    const char *url;
    const char *html;
    const char *exercise;
    const char *expected;
    /* "content-security-policy: ..." response header line, or NULL. */
    const char *policy;
    const ScriptReplayRecord *records;
    size_t record_count;
    const char *cached_url;
    const char *cached_source;
} ContextualFragmentCase;

static bool run_contextual_fragment_case(
    Budget *budget, const ContextualFragmentCase *test)
{
    const char *test_name = test->name;
    char directory[128] = {0}, error[256] = {0};
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool fixture_ready = false, replaying = false;
    bool browser_ready = false, navigation_ready = false, ok = true;

    if (test->record_count != 0) {
        REQUIRE(write_replay_fixture(
            directory, test->records, test->record_count));
        fixture_ready = true;
        REQUIRE(fetch_trace_replay_begin(directory, error, sizeof(error)));
        replaying = true;
    }
    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    if (test->cached_url != NULL) {
        REQUIRE(put_script(&browser, test->cached_url, test->cached_source));
    }
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, test->url, test->html,
        16, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    if (test->policy != NULL) {
        REQUIRE(tilefinch_csp_parse_response_headers(
            &navigation.page.document.content_security_policy, test->url,
            test->policy, strlen(test->policy), false));
    }
    bool evaluated = evaluate(&navigation, test->exercise, test->name);
    if (!evaluated) {
        fprintf(stderr, "%s evaluation error: %s\n", test_name,
                navigation.page.script_result.error);
    }
    REQUIRE(evaluated);
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, test->expected, 16, &advances));
    /* Nothing runs twice or late. */
    for (size_t step = 0; step < 3; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    REQUIRE(strcmp(navigation.page.script_result.summary,
                   test->expected) == 0);

cleanup:
    if (navigation_ready) navigation_destroy(&navigation);
    if (browser_ready) browser_session_destroy(&browser);
    if (replaying) fetch_trace_end();
    if (fixture_ready) remove_replay_fixture(directory, test->record_count);
    if (budget->current != 0) {
        fprintf(stderr, "%s leaked %zu owned bytes\n", test_name,
                budget->current);
        ok = false;
    }
    return ok;
}

/* The context element decides how the markup parses: a tbody accepts a
   bare <tr>, <body> (for a document or <html> start) drops it, and the
   RCDATA parent of a text node keeps markup as text. Fragments left
   unconnected are reclaimed at teardown. */
static bool test_contextual_fragment_contexts(Budget *budget)
{
    static const char page[] =
        "<!doctype html><html><head></head><body>"
        "<div id=d><span id=s>x</span></div>"
        "<table><tbody id=tb></tbody></table>"
        "<textarea id=ta>q</textarea></body></html>";
    static const char exercise[] =
        "const o=[],r=document.createRange(),"
        "d=document.getElementById('d'),tb=document.getElementById('tb'),"
        "ta=document.getElementById('ta');"
        "o.push(typeof r.createContextualFragment);"
        /* An element start node is its own context. */
        "r.selectNodeContents(d);"
        "let f=r.createContextualFragment('<b>a</b>c');"
        "o.push(f instanceof DocumentFragment,f.ownerDocument===document,"
        "f.childNodes.length,f.firstChild.tagName,f.isConnected,"
        "d.childNodes.length);"
        /* Table context: a <tr> string parses as a row. */
        "const row='<tr><td>1</td><td>2</td></tr>';"
        "r.selectNodeContents(tb);f=r.createContextualFragment(row);"
        "o.push(f.firstChild.tagName,f.firstChild.children.length);"
        "tb.appendChild(f);o.push(tb.children.length,"
        "tb.firstChild.textContent);"
        /* A document start node, and <html>, both parse as <body>. */
        "const doc=document.createRange().createContextualFragment(row);"
        "r.setStart(document.documentElement,0);"
        "const html=r.createContextualFragment(row);"
        "o.push(doc.childNodes.length,doc.firstChild.nodeType,"
        "doc.textContent,html.firstChild.nodeType,html.textContent);"
        /* A text node's parent element is the context. */
        "r.setStart(ta.firstChild,0);"
        "f=r.createContextualFragment('<b>x</b>');"
        "o.push(f.childNodes.length,f.firstChild.nodeType,f.textContent);"
        /* Script-side documents keep their own ownership. */
        "const other=document.implementation.createHTMLDocument('');"
        "const or=other.createRange?other.createRange():new Range();"
        "or.selectNodeContents(other.body);"
        "f=or.createContextualFragment('<i>z</i>');"
        "o.push(f.ownerDocument===other,f.firstChild.tagName);"
        /* The innerHTML byte ceiling bounds it too. */
        "let bounded='none';try{r.selectNodeContents(d);"
        "r.createContextualFragment('x'.repeat(256*1024+1));}"
        "catch(e){bounded=e.name;}o.push(bounded);"
        /* Cheap Range members beside it. */
        "const s=document.getElementById('s'),q=document.createRange();"
        "q.selectNode(s);o.push(q.startContainer===d,q.startOffset,"
        "q.endOffset,Range.START_TO_END,q.END_TO_START);"
        "const w=document.createRange();w.selectNodeContents(document.body);"
        "o.push(q.compareBoundaryPoints(Range.START_TO_START,w),"
        "w.compareBoundaryPoints(Range.END_TO_END,q),"
        "q.comparePoint(s,0),q.comparePoint(document.body,0),"
        "q.isPointInRange(tb,0));"
        "globalThis.pocSummary=o.join('|');";
    static const char expected[] =
        "function|true|true|2|B|false|1|TR|2|1|12|1|3|12|3|12|"
        "1|3|<b>x</b>|true|I|NotSupportedError|"
        "true|0|1|1|3|1|1|0|-1|false";
    const ContextualFragmentCase test = {
        .name = "contextual fragment context element",
        .url = "https://fragment-context.test/", .html = page,
        .exercise = exercise, .expected = expected
    };
    return run_contextual_fragment_case(budget, &test);
}

/* innerHTML scripts never run; contextual-fragment scripts wait for
   connection, then run once, are not force-async, and see currentScript. */
static bool test_contextual_fragment_scripts(Budget *budget)
{
    static const char page[] = "<!doctype html><head></head><body></body>";
    static const char exercise[] =
        "globalThis.runs=[];globalThis.currentOk=false;"
        "const holder=document.createElement('div');"
        "holder.innerHTML='<script>runs.push(\"inner\")<\\/script>';"
        "document.body.appendChild(holder);"
        "const r=document.createRange();r.selectNodeContents(document.body);"
        "const f=r.createContextualFragment("
        "'<script id=a>runs.push(\"a\");currentOk="
        "document.currentScript===document.getElementById(\"a\")<\\/script>"
        "<script src=\"/frag.js\"><\\/script><p>t</p>');"
        "const scripts=[...f.querySelectorAll('script')];"
        "const notForced=scripts.every((s)=>s.async===false);"
        "scripts[1].addEventListener('load',()=>{"
        "const again=document.createElement('div');"
        "again.appendChild(scripts[0]);document.body.appendChild(again);"
        "globalThis.pocSummary=['scripts',runs.join(','),currentOk,"
        "notForced].join(':');});"
        "const park=document.createElement('div');park.appendChild(f);"
        "const parked=runs.length;"
        "document.body.appendChild(park);"
        "globalThis.pocSummary='scripts:pending:'+parked+':'+runs.join(',');";
    static const char frag_source[] = "globalThis.runs.push('ext');";
    const ContextualFragmentCase test = {
        .name = "contextual fragment scripts run once on insertion",
        .url = "https://fragment-scripts.test/", .html = page,
        .exercise = exercise, .expected = "scripts:a,ext:true:true",
        .cached_url = "https://fragment-scripts.test/frag.js",
        .cached_source = frag_source
    };
    return run_contextual_fragment_case(budget, &test);
}

/* Under script-src 'self' 'nonce-n0', fragment scripts are judged exactly
   like createElement scripts: an unnonced inline script is refused, a
   nonced one runs, a same-origin URL loads and a cross-origin URL errors.
   The replay serves both URLs so an escaped check would execute, not hang
   on the network. */
static bool test_contextual_fragment_csp(Budget *budget)
{
    static const char page[] = "<!doctype html><head></head><body></body>";
    static const char exercise[] =
        "globalThis.ran=[];const events=[];"
        "const note=(s,n)=>{s.addEventListener('load',()=>{"
        "events.push(n+':load');done();});"
        "s.addEventListener('error',()=>{events.push(n+':error');done();});};"
        "const done=()=>{if(events.length===4)globalThis.pocSummary="
        "'csp:'+ran.sort().join(',')+':'+events.sort().join(',');};"
        "const r=document.createRange();r.selectNodeContents(document.body);"
        "const f=r.createContextualFragment("
        "'<script>ran.push(\"inline\")<\\/script>"
        "<script nonce=n0>ran.push(\"nonced\")<\\/script>"
        "<script src=\"/ok.js\"><\\/script>"
        "<script src=\"https://evil.test/x.js\"><\\/script>');"
        "const fs=f.querySelectorAll('script');"
        "note(fs[2],'frag-self');note(fs[3],'frag-cross');"
        "document.body.appendChild(f);"
        "const plain=document.createElement('script');"
        "plain.textContent='ran.push(\"plain\")';"
        "document.body.appendChild(plain);"
        "const self=document.createElement('script');self.src='/ok2.js';"
        "note(self,'element-self');document.body.appendChild(self);"
        "const cross=document.createElement('script');"
        "cross.src='https://evil.test/y.js';note(cross,'element-cross');"
        "document.body.appendChild(cross);"
        "globalThis.pocSummary='csp:pending';";
    static const ScriptReplayRecord records[] = {
        {.url = "https://fragment-csp.test/ok.js",
         .source = "globalThis.ran.push('self');"},
        {.url = "https://fragment-csp.test/ok2.js",
         .source = "globalThis.ran.push('self2');"},
        {.url = "https://evil.test/x.js",
         .source = "globalThis.ran.push('cross');"},
        {.url = "https://evil.test/y.js",
         .source = "globalThis.ran.push('cross2');"}
    };
    const ContextualFragmentCase test = {
        .name = "contextual fragment scripts obey CSP",
        .url = "https://fragment-csp.test/", .html = page,
        .exercise = exercise,
        .expected = "csp:nonced,self,self2:element-cross:error,"
                    "element-self:load,frag-cross:error,frag-self:load",
        .policy = "content-security-policy: script-src 'self' 'nonce-n0'\n",
        .records = records, .record_count = 4
    };
    return run_contextual_fragment_case(budget, &test);
}
