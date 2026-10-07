/* HTML's script cloning steps copy "already started" and nothing else. A
   clone of a script that ran must never run, whether the script itself or
   an ancestor was cloned, and whether it was cloned while the document
   pipeline was still discovering scripts or after load; a clone of a script
   that never started runs once when inserted. Pages clone such subtrees
   routinely: carousel loop modes duplicate slides, sticky headers clone the
   masthead, jQuery's .clone() and page builders duplicate sections. */

typedef struct {
    const char *name;
    const char *url;
    const char *html;
    const char *expected;
    const char *classic_url;
    const char *classic_source;
    const char *module_url;
    const char *module_source;
    /* Exact lab-census counts, or SIZE_MAX to skip the check. */
    size_t clones_started;
    size_t clones_unstarted;
    size_t clones_suppressed;
    size_t clones_refused_minimum;
} ScriptCloneCase;

static bool run_script_clone_case(Budget *budget, const ScriptCloneCase *test)
{
    const char *test_name = test->name;
    BrowserSession browser = {0};
    NavigationSession navigation = {0};
    bool browser_ready = false, navigation_ready = false, ok = true;

    REQUIRE(browser_session_init(&browser, budget, 256u * 1024u));
    browser_ready = true;
    if (test->classic_url != NULL) {
        REQUIRE(put_script(&browser, test->classic_url,
                           test->classic_source));
    }
    if (test->module_url != NULL) {
        REQUIRE(put_module(&browser, test->module_url,
                           test->module_source));
    }
    REQUIRE(start_custom_runtime_page(
        &navigation, budget, &browser, test->url, test->html,
        32, 64u * 1024u, 32u * 1024u));
    navigation_ready = true;
    size_t advances = 0;
    REQUIRE(advance_until(&navigation, test->expected, 24, &advances));
    /* Nothing runs twice or late. */
    for (size_t step = 0; step < 3; step++) {
        REQUIRE(navigation_advance_runtime(&navigation, 1, 1));
    }
    const ScriptResult *result = &navigation.page.script_result;
    if (strcmp(result->summary, test->expected) != 0) {
        fprintf(stderr, "%s: late summary \"%s\"\n", test_name,
                result->summary);
    }
    REQUIRE(strcmp(result->summary, test->expected) == 0);
    if (test->clones_started != SIZE_MAX
        && (result->script_clones_started != test->clones_started
            || result->script_clones_unstarted != test->clones_unstarted
            || result->script_clones_suppressed
                   != test->clones_suppressed)) {
        fprintf(stderr,
                "%s: census started=%zu unstarted=%zu suppressed=%zu "
                "(expected %zu/%zu/%zu)\n", test_name,
                result->script_clones_started,
                result->script_clones_unstarted,
                result->script_clones_suppressed, test->clones_started,
                test->clones_unstarted, test->clones_suppressed);
        ok = false;
    }
    if (result->script_clones_refused < test->clones_refused_minimum) {
        fprintf(stderr, "%s: census refused=%zu (expected >= %zu)\n",
                test_name, result->script_clones_refused,
                test->clones_refused_minimum);
        ok = false;
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

/* Inline, external, external-module and inline-module parser scripts are
   each cloned by themselves and inside a deep clone of their parent, once
   while the pipeline still runs (from the last module, after every other
   script) and once after load. Before the fix the inline, external and
   inline-module clones ran again during load: the clone of a parser script
   had no native state, which the pipeline's discovery waves read as a
   newly inserted script. */
static bool test_cloned_started_scripts_never_rerun(Budget *budget)
{
    static const char page[] =
        "<!doctype html><head><script>globalThis.runs=[];</script></head>"
        "<body><div id=w><script id=i>runs.push('i');</script>"
        "<script id=e src=/e.js></script>"
        "<script id=m type=module src=/m.js></script>"
        "<script id=im type=module>runs.push('im');</script></div>"
        "<script type=module>"
        "const cloneAll=()=>{"
        "for(const id of ['i','e','m','im'])document.body.append("
        "document.getElementById(id).cloneNode(true));"
        "document.body.append(document.getElementById('w').cloneNode(true));"
        "};cloneAll();"
        "setTimeout(()=>{cloneAll();setTimeout(()=>{globalThis.pocSummary="
        "'started:'+runs.join(',');},0);},0);"
        "</script></body>";
    const ScriptCloneCase test = {
        .name = "cloned started scripts never run again",
        .url = "https://clone-started.test/", .html = page,
        .expected = "started:i,e,m,im",
        .classic_url = "https://clone-started.test/e.js",
        .classic_source = "globalThis.runs.push('e');",
        .module_url = "https://clone-started.test/m.js",
        .module_source = "globalThis.runs.push('m');",
        .clones_started = 16, .clones_unstarted = 0,
        .clones_suppressed = 16
    };
    return run_script_clone_case(budget, &test);
}

/* A clone of a script that never started is not started either: template
   contents (cloned and imported), a created script cloned before its
   insertion, a contextual-fragment script cloned before connection, and
   consent-manager text/plain scripts retyped before or after cloning. A
   clone is a new element otherwise, so it is force-async whatever the
   source's async IDL value was. Before the fix the after-load clones never
   ran at all, and the clone kept the source's force-async flag. */
static bool test_unstarted_script_clones_run_once(Budget *budget)
{
    static const char page[] =
        "<!doctype html><head><script>globalThis.runs=[];</script></head>"
        "<body><template id=t><script>runs.push('t');</script></template>"
        "<script id=p type=text/plain>runs.push('p');</script>"
        "<script id=q type=text/plain>runs.push('q');</script>"
        "<script>"
        "document.body.append(document.getElementById('t').content"
        ".cloneNode(true));"
        "const s=document.createElement('script');"
        "s.textContent=\"runs.push('s')\";const sc=s.cloneNode(true);"
        "document.body.append(s);document.body.append(sc);"
        "const f=document.createRange().createContextualFragment("
        "\"<script>runs.push('f')<\\/script>\");const fc=f.cloneNode(true);"
        "document.body.append(f);document.body.append(fc);"
        "setTimeout(()=>{"
        "document.body.append(document.importNode("
        "document.getElementById('t').content,true));"
        "const p=document.getElementById('p'),pc=p.cloneNode(true);"
        "pc.type='';p.replaceWith(pc);"
        "const q=document.getElementById('q');q.type='text/javascript';"
        "q.replaceWith(q.cloneNode(true));"
        "const a=document.createElement('script');a.async=false;"
        "const forced=a.async===false&&a.cloneNode(false).async===true;"
        "setTimeout(()=>{globalThis.pocSummary='unstarted:'+runs.join(',')"
        "+':'+forced;},0);},0);"
        "</script></body>";
    const ScriptCloneCase test = {
        .name = "clones of never-started scripts run once",
        .url = "https://clone-unstarted.test/", .html = page,
        .expected = "unstarted:t,s,s,f,f,t,p,q:true",
        .clones_started = 0, .clones_unstarted = 7,
        .clones_suppressed = 0
    };
    return run_script_clone_case(budget, &test);
}

/* Moving is not cloning: an inline, an external and a created script that
   ran stay started when moved within the document or adopted into another
   document and back, during load and after it. */
static bool test_moved_started_scripts_never_rerun(Budget *budget)
{
    static const char page[] =
        "<!doctype html><head><script>globalThis.runs=[];</script></head>"
        "<body><div id=b></div><script id=i>runs.push('i');</script>"
        "<script id=e src=/e.js></script>"
        "<script>"
        "const d=document.createElement('script');"
        "d.textContent=\"runs.push('d')\";document.body.append(d);"
        "const moveAll=()=>{const b=document.getElementById('b'),"
        "other=document.implementation.createHTMLDocument('');"
        "for(const n of [document.getElementById('i'),"
        "document.getElementById('e'),d]){b.append(n);"
        "other.body.append(other.adoptNode(n));"
        "document.body.append(document.adoptNode(n));}};moveAll();"
        "setTimeout(()=>{moveAll();setTimeout(()=>{globalThis.pocSummary="
        "'moved:'+runs.join(',');},0);},0);"
        "</script></body>";
    const ScriptCloneCase test = {
        .name = "moved started scripts never run again",
        .url = "https://clone-move.test/", .html = page,
        .expected = "moved:i,e,d",
        .classic_url = "https://clone-move.test/e.js",
        .classic_source = "globalThis.runs.push('e');",
        .clones_started = 0, .clones_unstarted = 0,
        .clones_suppressed = 0
    };
    return run_script_clone_case(budget, &test);
}

/* The script-state table is bounded. Clones that are dropped or removed
   release their state, reclaimed when the table fills even inside one long
   synchronous script, so a page that clones a started script hundreds of
   times never exhausts it. Once live clones do fill it, a clone that would
   need to record a script is refused (cloneNode returns null) instead of
   producing a script that runs a second time; a clone without scripts is
   unaffected. Before the fix the parser script's clones needed no state,
   so none was refused and the inserted one ran again. */
static bool test_script_clone_state_exhaustion_fails_safe(Budget *budget)
{
    static const char page[] =
        "<!doctype html><head><script>globalThis.runs=[];</script></head>"
        "<body><div id=w><script id=i>runs.push('i');</script></div>"
        "<div id=x><b>plain</b></div>"
        "<script>"
        "const w=document.getElementById('w');let churned=true;"
        "for(let k=0;k<400;k++){const c=w.cloneNode(true);"
        "if(!c){churned=false;break;}document.body.append(c);c.remove();}"
        "const held=[];"
        "for(let k=0;k<300;k++)held.push(w.cloneNode(true));"
        "const refused=held.includes(null);"
        "const c=w.cloneNode(true);if(c)document.body.append(c);"
        "const plain=document.getElementById('x').cloneNode(true)!==null;"
        "globalThis.capacity=[churned,refused,c===null,plain].join('/');"
        "setTimeout(()=>{globalThis.pocSummary='exhaust:'+runs.join(',')"
        "+':'+capacity;},0);"
        "</script></body>";
    const ScriptCloneCase test = {
        .name = "script clone state exhaustion fails safe",
        .url = "https://clone-exhaust.test/", .html = page,
        .expected = "exhaust:i:true/true/true/true",
        .clones_started = SIZE_MAX, .clones_refused_minimum = 1
    };
    return run_script_clone_case(budget, &test);
}
