#include <stdio.h>
#include <string.h>

#include "script_test_support.h"

/* Generic DOM contracts, not captured page scripts. Each case has a fresh
   realm so public constructor/prototype replacements cannot leak between it
   and another case; even failures finish the ownership teardown check. */
static const char *selected_case;
static bool selected_case_ran;
static bool test_weak_shadow_slot_reclamation(void)
{
    const char *name = "already-weakened-shadow-slot-reclamation";
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0)) return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(runtime,
        "(()=>{class Card extends HTMLElement{constructor(){super();"
        "this.attachShadow({mode:'open'}).innerHTML='<b>Transient</b>'}}"
        "customElements.define('weak-card',Card);globalThis.heldCards=[];})()",
        name, &result);
    /* Retain every owner until all native slots are occupied, but construct
       in ordinary task-sized batches. Sanitizer overhead must not turn a
       slot-lifetime test into one oversized synchronous author task. */
    for (unsigned batch=0;ok && batch<16;batch++)
        ok=script_runtime_evaluate_diagnostic(runtime,
            "(()=>{for(let i=0;i<16;i++){const card=document.createElement('weak-card');"
            "document.body.append(card);card.remove();heldCards.push(card);}})()",
            name,&result);
    if(ok) ok=script_runtime_evaluate_diagnostic(runtime,
        "globalThis.heldCards=null",name,&result);
    if (ok) ok = script_runtime_advance(runtime, 16, 16, &result);
    if (ok) {
        (void) script_runtime_collect_and_trim(runtime);
        /* Native handle releases are queued finalization tasks. Registration
           must reclaim dead slots even when disconnect already weakened the
           bootstrap cache, rather than require a second weakening change. */
        ok = script_runtime_evaluate_diagnostic(runtime,
            "(()=>{const card=document.createElement('weak-card');"
            "document.body.append(card);if(card.shadowRoot?.textContent!=="
            "'Transient')throw Error('dead root slots not reclaimed');"
            "globalThis.pocSummary='OK'})()", name, &result);
    }
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok) fprintf(stderr, "%s: summary=%s error=%s\n", name,
                     result.summary, result.error);
    script_runtime_destroy(runtime);
    bool zero_owned = script_page_fixture_close(&fixture);
    printf("%s: %s teardown-owned-bytes=%zu\n", name,
           ok && zero_owned ? "PASS" : "FAIL", fixture.budget.current);
    return ok && zero_owned;
}

static bool run_page_case(const char *name, const char *source, bool styled)
{
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return false;
    Stylesheet sheet = {0};
    if (styled && !stylesheet_build(&sheet, &fixture.budget, &fixture.document,
                                    480))
        return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
    if (styled && runtime != NULL) script_runtime_set_stylesheet(runtime, &sheet);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    /* Batched component creation also schedules bounded finalizer tasks;
       allow those tasks to drain without relaxing the realm's watchdog. */
    for (unsigned turn = 0; ok && turn < 256; turn++)
        ok = script_runtime_advance(runtime, 16, 16, &result);
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok)
        fprintf(stderr, "%s: summary=%s error=%s\n", name,
                result.summary, result.error);
    script_runtime_destroy(runtime);
    if (styled) stylesheet_destroy(&sheet);
    bool zero_owned = script_page_fixture_close(&fixture);
    if (!zero_owned) fprintf(stderr, "%s: nonzero teardown ownership\n", name);
    printf("%s: %s teardown-owned-bytes=%zu\n", name,
           ok ? "PASS" : "FAIL", fixture.budget.current);
    return ok && zero_owned;
}

static bool run_case(const char *name, const char *source)
{
    return run_page_case(name, source, false);
}

/* With a stylesheet, as a page has: innerText reads the rendered text only
   when styles can be computed, and is textContent otherwise. */
static bool run_styled_case(const char *name, const char *source)
{
    return run_page_case(name, source, true);
}

static bool test_disconnected_observer_ownership(void)
{
    const char *name = "disconnected-observer-ownership";
    if (selected_case != NULL && strcmp(name,selected_case)!=0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture,"<script></script>",0)) return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime=script_runtime_create_configured(&fixture.document,
        &fixture.budget,12u*1024u*1024u,1000,"https://dom-intrinsics.test/",
        &fixture.options,&result);
    bool ok=runtime!=NULL && script_runtime_evaluate_diagnostic(runtime,
        "globalThis.observerRefs=[];(()=>{for(const Type of [MutationObserver,"
        "ResizeObserver])for(let i=0;i<64;i++){const owner={payload:'x'.repeat(4096)};"
        "owner.observer=new Type(()=>owner.payload);owner.observer.disconnect();"
        "observerRefs.push(new WeakRef(owner));}})()",name,&result);
    if (ok) ok=script_runtime_advance(runtime,16,16,&result);
    if (ok) {
        (void)script_runtime_collect_and_trim(runtime);
        ok=script_runtime_evaluate_diagnostic(runtime,
            "(()=>{if(observerRefs.some(ref=>ref.deref()))"
            "throw Error('disconnected observer retains owner');"
            "globalThis.pocSummary='OK'})()",name,&result);
    }
    ok=ok && result.success && strcmp(result.summary,"OK")==0;
    if (!ok) fprintf(stderr,"%s: %s\n",name,result.error);
    script_runtime_destroy(runtime);
    bool zero=script_page_fixture_close(&fixture);
    printf("%s: %s\n",name,ok&&zero?"PASS":"FAIL");
    return ok&&zero;
}

static bool test_author_sheet_owner_lifetime(void)
{
    const char *name = "author-sheet-owner-lifetime";
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0)) return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(&fixture.document,
        &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(runtime,
        "globalThis.ownerRefs=[];(()=>{"
        "if('__tilefinchTakeOwnerAttributeState' in globalThis)throw Error('private hook');"
        "for(let i=0;i<32;i++){"
        "const node=document.createElement('style');"
        "node.textContent='b{color:red}';document.head.append(node);"
        "const sheet=node.sheet;if(sheet!==node.sheet||sheet.ownerNode!==node)"
        "throw Error('sheet identity');node.remove();node.payload='x'.repeat(4096);"
        "ownerRefs.push(new WeakRef(node),new WeakRef(sheet));}"
        "const held=document.createElement('style');held.textContent='i{color:blue}';"
        "document.head.append(held);Object.freeze(held);"
        "globalThis.heldSheet=held.sheet;if(held.sheet!==heldSheet)"
        "throw Error('frozen identity');held.remove();"
        "const persisted=document.createElement('style');persisted.id='persisted';"
        "persisted.textContent='b{color:red}';document.head.append(persisted);"
        "persisted.sheet.insertRule('i{color:green}',1);"
        "globalThis.persistedLease=persisted.__tilefinchHandleLease;"
        "ownerRefs.push(new WeakRef(persisted.sheet));})()",
        name, &result);
    if (ok) ok = script_runtime_advance(runtime, 16, 16, &result);
    if (ok) {
        (void) script_runtime_collect_and_trim(runtime);
        ok = script_runtime_evaluate_diagnostic(runtime,
            "(()=>{if(ownerRefs.some(ref=>ref.deref()))throw Error('owner retained');"
            "if(heldSheet.ownerNode.textContent!=='i{color:blue}')"
            "throw Error('held sheet lost owner');globalThis.heldSheet=null;"
            "const node=document.getElementById('persisted'),sheet=node.sheet;"
            "if(node.__tilefinchHandleLease===persistedLease)"
            "throw Error('owner retained');"
            "if(sheet.cssRules.length!==2||!sheet.cssRules[1].cssText.includes('green'))"
            "throw Error('CSSOM override lost');"
            "node.textContent='em{color:orange}';"
            "if(node.sheet.cssRules.length!==1||!node.sheet.cssRules[0].cssText.includes('orange'))"
            "throw Error('DOM edit not reflected');globalThis.pocSummary='OK'})()",
            name, &result);
    }
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok) fprintf(stderr, "%s: %s\n", name, result.error);
    script_runtime_destroy(runtime);
    bool zero = script_page_fixture_close(&fixture);
    printf("%s: %s\n", name, ok && zero ? "PASS" : "FAIL");
    return ok && zero;
}

/* Evaluates `source` (which must set pocSummary='OK') in a realm with or
   without the bootstrap's test-only probes. */
static bool run_probe_surface_case(const char *name, bool omit_test_probes,
                                   const char *source)
{
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0)) return false;
    ScriptRuntimeOptions options = fixture.options;
    options.omit_test_probes = omit_test_probes;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok) fprintf(stderr, "%s: summary=%s error=%s\n", name,
                     result.summary, result.error);
    script_runtime_destroy(runtime);
    bool zero = script_page_fixture_close(&fixture);
    printf("%s: %s\n", name, ok && zero ? "PASS" : "FAIL");
    return ok && zero;
}

/* A page-defined __tilefinchTraceObject used to be called on every new node
   wrapper, and its return value replaced the wrapper the bootstrap cached. */
static const char page_trace_hook_ignored[] =
    "(()=>{let calls=0;const fake={fake:true};"
    "globalThis.__tilefinchTraceObject=()=>{calls++;return fake};"
    "const div=document.createElement('div');document.body.append(div);"
    "const found=document.body.lastElementChild;"
    "if(calls!==0||div===fake||found!==div||!(div instanceof HTMLDivElement))"
    "throw Error('page trace hook substituted wrappers: calls='+calls);"
    "globalThis.pocSummary='OK'})()";

static bool test_test_probe_surface(void)
{
    bool ok = run_probe_surface_case(
        "page-trace-hook-ignored-shipping", true, page_trace_hook_ignored);
    ok &= run_probe_surface_case(
        "page-trace-hook-ignored-probes", false, page_trace_hook_ignored);
    /* The lab's trace reaches wrappers only through the probe-realm setter,
       which the page cannot replace. */
    ok &= run_probe_surface_case("lab-wrapper-trace-setter", false,
        "(()=>{const seen=[];const set=__tilefinchSetWrapperTrace;"
        "try{globalThis.__tilefinchSetWrapperTrace=null}catch(_){}"
        "if(__tilefinchSetWrapperTrace!==set)throw Error('setter replaced');"
        "set((node,label)=>{seen.push(label);return node});"
        "const span=document.createElement('span');set(null);"
        "document.createElement('em');"
        "if(seen.join()!=='element.span'||!(span instanceof HTMLSpanElement))"
        "throw Error('trace '+seen.join());globalThis.pocSummary='OK'})()");
    /* The shipping realm surface: no test-only probes, eager or lazy, even
       when the page plants the flag before a lazy module loads. */
    ok &= run_probe_surface_case("shipping-realm-has-no-test-probes", true,
        "(()=>{globalThis.__tilefinchTestProbes=true;"
        "document.createElement('canvas').getContext('2d');void IDBFactory;"
        "void AnimationEvent;"
        "const present=['__tilefinchSetWrapperTrace','__tilefinchRootCensus',"
        "'__tilefinchNetworkQueueStats','__tilefinchIndexedDBStats',"
        "'__tilefinchCanvasDiagnostics','__tilefinchTransitionStats',"
        "'__tilefinchDiagnosticLookup','__tilefinchLastUncaughtTask',"
        "'__tilefinchXHRSendCalls','__tilefinchXHRLastStates',"
        "'__tilefinchXHRDiagnostics','__tilefinchFrameEvalTelemetry',"
        "'__tilefinchDiagnosticMobileSafari','__tilefinchClearNodeCache']"
        ".filter(name=>name in globalThis);"
        "if(present.length)throw Error('test probes: '+present.join());"
        "globalThis.pocSummary='OK'})()");
    /* A top-level `var` (or a non-configurable defineProperty) makes the
       flag non-configurable, so the loader cannot replace it. The lazy load
       must then fail closed rather than run with the page's value, and a
       page-owned accessor must not run inside trusted initialization. */
    ok &= run_probe_surface_case("shipping-realm-page-var-flag", true,
        "var __tilefinchTestProbes=1;"
        "(()=>{try{document.createElement('canvas').getContext('2d')}catch(_){}"
        "try{void IDBFactory}catch(_){}try{void CSSStyleSheet}catch(_){}"
        "const present=['__tilefinchCanvasDiagnostics',"
        "'__tilefinchIndexedDBStats','__tilefinchRootCensus',"
        "'__tilefinchDiagnosticLookup','__tilefinchTransitionStats']"
        ".filter(name=>name in globalThis);"
        "if(present.length)throw Error('test probes: '+present.join());"
        "globalThis.pocSummary='OK'})()");
    ok &= run_probe_surface_case("shipping-realm-page-accessor-flag", true,
        "(()=>{let calls=0;Object.defineProperty(globalThis,"
        "'__tilefinchTestProbes',{get(){calls++;return true}});"
        "try{document.createElement('canvas').getContext('2d')}catch(_){}"
        "try{void IDBFactory}catch(_){}"
        "const present=['__tilefinchCanvasDiagnostics',"
        "'__tilefinchIndexedDBStats'].filter(name=>name in globalThis);"
        "if(present.length||calls)"
        "throw Error('test probes: '+present.join()+' calls='+calls);"
        "globalThis.pocSummary='OK'})()");
    /* The lazy Worker loader hands its installer the realm and worker
       natives (create_worker_realm evaluates a string in a new realm, an
       eval a CSP without 'unsafe-eval' forbids). A page-owned
       __tilefinchInstallWorker must never become that installer: a
       non-configurable accessor fails the load closed, and a configurable
       one is replaced without running and is gone after the load. */
    ok &= run_probe_surface_case("worker-installer-page-accessor", true,
        "(()=>{let calls=0,natives=0,sets=0;const fake=function(...args){"
        "calls++;natives+=args.filter(a=>typeof a==='function').length};"
        "Object.defineProperty(globalThis,'__tilefinchInstallWorker',"
        "{get(){return fake},set(_){sets++}});"
        "try{void Worker}catch(_){}"
        "try{new Worker('data:text/javascript,')}catch(_){}"
        "if(calls||natives||sets)throw Error('page installer reached: calls='"
        "+calls+' natives='+natives+' sets='+sets);"
        "globalThis.pocSummary='OK'})()");
    ok &= run_probe_surface_case("worker-installer-page-configurable", true,
        "(()=>{let reads=0,sets=0;Object.defineProperty(globalThis,"
        "'__tilefinchInstallWorker',{configurable:true,"
        "get(){reads++;return ()=>{}},set(_){sets++}});"
        "const loaded=typeof Worker==='function';"
        "const left='__tilefinchInstallWorker' in globalThis;"
        "if(!loaded||left||reads||sets)throw Error('loaded='+loaded"
        "+' left='+left+' reads='+reads+' sets='+sets);"
        "globalThis.pocSummary='OK'})()");
    /* The IndexedDB and motion modules publish native-read state (a stats
       record, the style-flush callback) on a temporary host channel. A
       page-owned __tilefinchHostChannel must not receive that state or
       hand the native a page object in its place: the loads fail closed. */
    ok &= run_probe_surface_case("host-channel-page-accessor", true,
        "(()=>{const planted={};let reads=0;Object.defineProperty(globalThis,"
        "'__tilefinchHostChannel',{get(){reads++;return planted},set(_){}});"
        "try{void IDBFactory}catch(_){}try{void AnimationEvent}catch(_){}"
        "const keys=Object.keys(planted);"
        "if(reads||keys.length)throw Error('host channel reached: reads='"
        "+reads+' keys='+keys.join());"
        "globalThis.pocSummary='OK'})()");
    ok &= run_probe_surface_case("probe-realm-publishes-lazy-probes", false,
        "(()=>{document.createElement('canvas').getContext('2d');"
        "void IDBFactory;void AnimationEvent;"
        "const missing=['__tilefinchSetWrapperTrace','__tilefinchRootCensus',"
        "'__tilefinchNetworkQueueStats','__tilefinchIndexedDBStats',"
        "'__tilefinchCanvasDiagnostics','__tilefinchTransitionStats',"
        "'__tilefinchDiagnosticLookup']"
        ".filter(name=>!(name in globalThis));"
        "if(missing.length||!('motionScans' in __tilefinchRootCensus)"
        "||'__tilefinchTestProbes' in globalThis)"
        "throw Error('missing probes: '+missing.join());"
        "globalThis.pocSummary='OK'})()");
    return ok;
}

/* getElementById answers from a memo between DOM changes: every change
   that can move an id is seen (script writes, a native insertion standing
   in for the parser), while class and style writes keep the answers. */
static bool test_element_id_memo(void)
{
    const char *name = "element-id-memo";
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture,
            "<body><div id=a></div><div id=t></div><div id=host></div>"
            "<div id=sh></div><script></script></body>", 0)) return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const ok=(c,m)=>{if(!c)throw Error(m)},"
        "by=(id)=>document.getElementById(id);"
        "for(let i=0;i<300;i++)document.body.append(document.createElement('p'));"
        "const a=by('a');ok(a&&a.id==='a','a');"
        "for(let i=0;i<100;i++){ok(by('a')===a,'hit '+i);"
        "ok(by('missing')===null,'miss '+i);document.body.className='c'+i;"
        "a.style.width=i+'px';a.classList.toggle('on')}"
        "a.id='b';ok(by('a')===null&&by('b')===a,'id property');"
        "a.setAttribute('id','a');ok(by('a')===a&&by('b')===null,'setAttribute');"
        "const early=document.createElement('p');early.id='a';"
        "document.body.insertBefore(early,document.body.firstChild);"
        "ok(by('a')===early,'earlier duplicate');"
        "early.remove();ok(by('a')===a,'removed duplicate');"
        "a.removeAttribute('id');ok(by('a')===null,'removeAttribute');"
        "a.toggleAttribute('id');a.setAttributeNS(null,'id','a');"
        "ok(by('a')===a,'setAttributeNS');"
        "const added=document.createElement('i');added.id='missing';"
        "document.body.appendChild(added);ok(by('missing')===added,'inserted');"
        "const host=by('host');host.innerHTML='<b id=inner></b>';"
        "ok(by('inner')===host.firstChild,'innerHTML');"
        "host.textContent='';ok(by('inner')===null,'textContent');"
        "const shadow=by('sh').attachShadow({mode:'open'});"
        "ok(by('hidden')===null,'before shadow');"
        "shadow.innerHTML='<u id=hidden></u>';ok(by('hidden')===null,'shadow');"
        "const t=by('t');t.getAttributeNode('id').value='t2';"
        "ok(by('t2')===t&&by('t')===null,'Attr value');"
        "ok(by('late')===null,'late absent');"
        "globalThis.pocSummary='OK'})()", name, &result);
    if (ok) ok = result.success && strcmp(result.summary, "OK") == 0;
    /* 200 lookups between class and style writes walk only twice (the
       first 'a' and 'missing'), so the whole case stays far below its
       ~240 lookups. */
    size_t walks = result.element_id_walks;
    if (ok && walks > 40u) {
        fprintf(stderr, "%s: %zu id walks\n", name, walks);
        ok = false;
    }
    /* A native insertion, as the parser makes, is seen too. */
    lxb_dom_element_t *late = ok ? lxb_dom_document_create_element(
        &fixture.document.html->dom_document,
        (const lxb_char_t *) "div", 3, NULL) : NULL;
    lxb_dom_element_t *body = lxb_dom_interface_element(
        lxb_html_document_body_element(fixture.document.html));
    ok = ok && late != NULL && body != NULL
        && lxb_dom_element_set_attribute(late, (const lxb_char_t *) "id", 2,
                                         (const lxb_char_t *) "late", 4)
               != NULL;
    if (ok) lxb_dom_node_insert_child(lxb_dom_interface_node(body),
                                      lxb_dom_interface_node(late));
    ok = ok && script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const late=document.getElementById('late');"
        "if(!late||late.localName!=='div')throw Error('native insertion');"
        "globalThis.pocSummary='OK'})()", name, &result);
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok) fprintf(stderr, "%s: %s (walks %zu)\n", name, result.error,
                     walks);
    script_runtime_destroy(runtime);
    bool zero = script_page_fixture_close(&fixture);
    printf("%s: %s\n", name, ok && zero ? "PASS" : "FAIL");
    return ok && zero;
}

int main(int argc, char **argv)
{
    if (argc > 2) return 2;
    if (argc == 2) selected_case = argv[1];
    bool ok = true;
    ok &= test_weak_shadow_slot_reclamation();
    ok &= test_disconnected_observer_ownership();
    ok &= test_author_sheet_owner_lifetime();
    ok &= test_test_probe_surface();
    ok &= test_element_id_memo();
    ok &= run_case("insertion-of-a-document-or-attr-changes-nothing",
        "(()=>{const host=document.createElement('div');document.body.append(host);"
        "const ref=host.appendChild(document.createElement('i')),"
        "frag=document.createDocumentFragment(),"
        "attr=document.createAttribute('data-x');"
        "const routes={appendChild:n=>host.appendChild(n),"
        "insertBefore:n=>host.insertBefore(n,ref),"
        "replaceChild:n=>host.replaceChild(n,ref),"
        "append:n=>host.append(n),prepend:n=>host.prepend(n),"
        "before:n=>ref.before(n),after:n=>ref.after(n),"
        "replaceWith:n=>ref.replaceWith(n),"
        "replaceChildren:n=>host.replaceChildren(n),"
        "insertAdjacentElement:n=>host.insertAdjacentElement('beforeend',n),"
        "fragmentAppend:n=>frag.appendChild(n)};"
        "for(const [label,node] of [['document',document],['attr',attr]])"
        "for(const [name,insert] of Object.entries(routes)){"
        "let thrown=null;try{insert(node)}catch(e){thrown=e}"
        "const where=label+' '+name+': ';"
        "if(!thrown||thrown.name!=='HierarchyRequestError'&&thrown.name!=='TypeError')"
        "throw Error(where+'threw '+(thrown&&thrown.name));"
        "if(typeof document.createElement!=='function'||document.nodeType!==9||"
        "document.nodeName!=='#document'||document.documentElement?.nodeName!=='HTML')"
        "throw Error(where+'document damaged');"
        "if(attr.nodeType!==2||attr.name!=='data-x')throw Error(where+'attr damaged');"
        "if(host.childNodes.length!==1||host.firstChild!==ref||frag.firstChild)"
        "throw Error(where+'tree changed');}"
        "if(document.createElement('b').nodeName!=='B')throw Error('later script');"
        "const other=document.implementation.createHTMLDocument(''),"
        "foreign=other.body.appendChild(other.createElement('p'));"
        "let refused=null;try{host.insertBefore(foreign,document.createElement('q'))}"
        "catch(e){refused=e}"
        "if(refused?.name!=='NotFoundError'||foreign.parentNode!==other.body)"
        "throw Error('refused insertion moved a foreign node');"
        "host.appendChild(foreign);"
        "if(foreign.parentNode!==host||host.lastChild?.nodeName!=='P')"
        "throw Error('foreign node not adopted');"
        "const xml=document.implementation.createDocument(null,'r',null);"
        "for(const [name,make] of [['text',d=>d.createTextNode('t')],"
        "['comment',d=>d.createComment('c')],['element',d=>d.createElement('u')],"
        "['xml',()=>xml.createElementNS(null,'x')]]){"
        "const before=host.childNodes.length;host.appendChild(make(other));"
        "host.insertBefore(make(other),host.firstChild);"
        "if(host.childNodes.length!==before+2)throw Error('shim '+name+' not inserted')}"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("connected-reactions-after-whole-fragment",
        "(async()=>{const log=[];let react=null;"
        "class Probe extends HTMLElement{connectedCallback(){"
        "log.push(this.id+'='+[...this.parentNode.children].map(e=>e.id).join());"
        "react?.(this)}}customElements.define('x-probe',Probe);"
        "const mk=id=>{const e=document.createElement('x-probe');e.id=id;return e},"
        "frag=()=>{const f=document.createDocumentFragment();"
        "f.append(mk('a'),mk('b'));return f};"
        "const routes={"
        "appendChild:(h,o)=>h.appendChild(frag()),"
        "insertBefore:(h,o)=>h.insertBefore(frag(),o),"
        "append:(h,o)=>h.append(mk('a'),mk('b')),"
        "prepend:(h,o)=>h.prepend(mk('a'),mk('b')),"
        "before:(h,o)=>o.before(mk('a'),mk('b')),"
        "after:(h,o)=>o.after(mk('a'),mk('b')),"
        "replaceWith:(h,o)=>o.replaceWith(mk('a'),mk('b')),"
        "replaceChild:(h,o)=>h.replaceChild(frag(),o),"
        "replaceChildren:(h,o)=>h.replaceChildren(mk('a'),mk('b'))};"
        "const want={appendChild:'o,a,b',insertBefore:'a,b,o',append:'o,a,b',"
        "prepend:'a,b,o',before:'a,b,o',after:'o,a,b',replaceWith:'a,b',"
        "replaceChild:'a,b',replaceChildren:'a,b'};"
        "for(const [name,insert] of Object.entries(routes)){"
        "const host=document.body.appendChild(document.createElement('div')),"
        "old=host.appendChild(document.createElement('i'));old.id='o';"
        "log.length=0;insert(host,old);"
        "const expected='a='+want[name]+';b='+want[name];"
        "if(log.join(';')!==expected)"
        "throw Error(name+': '+log.join(';')+' want '+expected);"
        "host.remove();}"
        /* A reaction may remove the reference node or add to the tree: that
           can neither fail the insertion nor lose its mutation record. */
        "const host=document.body.appendChild(document.createElement('div')),"
        "old=host.appendChild(document.createElement('i'));old.id='o';"
        "react=()=>{react=null;old.remove()};host.insertBefore(frag(),old);"
        "if([...host.children].map(e=>e.id).join()!=='a,b')"
        "throw Error('reaction removing the reference');"
        "const records=[];const seen=new MutationObserver(list=>{"
        "for(const r of list)records.push([...r.addedNodes].map(n=>n.id).join('+'))});"
        "seen.observe(host,{childList:true});"
        "react=e=>{react=null;const c=document.createElement('u');c.id='c';"
        "host.append(c)};host.appendChild(frag());"
        "await Promise.resolve();await Promise.resolve();"
        "if(records.join()!=='a+b,c')throw Error('records '+records.join());"
        "globalThis.pocSummary='OK'})().catch(e=>{globalThis.pocSummary=String(e)})");
    ok &= run_case("non-element-wrapper-properties",
        "(()=>{for(const node of [document,document.createTextNode('held'),"
        "document.createComment('held'),document.createDocumentFragment()]){"
        "if('style' in node||'classList' in node)throw Error('element-only property');"
        "node.marker=17;const child=document.createElement('span');"
        "if(node.nodeType===11){node.append(child);if(node.firstChild!==child)"
        "throw Error('fragment identity');}}"
        "const element=document.createElement('div');"
        "element.style.color='red';element.classList.add('present');"
        "if(!element.classList.contains('present')||element.style.color!=='red')"
        "throw Error('element properties lost');globalThis.pocSummary='OK'})()");
    /* Each case's expected value comes from DOM "clone a node", checked against
       Chromium. A clone has no root unless the source's was made clonable. */
#define SHADOW_CLONE_HELPERS \
        "const check=(c,m)=>{if(!c)throw Error(m)};" \
        "const tags=n=>[...n.childNodes].map(c=>c.localName).join();" \
        "const make=(init)=>{const host=document.createElement('x-h');" \
        "host.append(document.createElement('u'));" \
        "const root=host.attachShadow(init||{mode:'open'});" \
        "root.append(document.createElement('b'),document.createElement('i'));" \
        "return{host,root}};" \
        "const plain=(c,m)=>{check(c.shadowRoot===null,m+' root');" \
        "check(tags(c)==='u',m+' children '+tags(c));" \
        "check(c.innerHTML==='<u></u>',m+' html '+c.innerHTML);" \
        "check(c.childElementCount===1&&c.lastChild.localName==='u',m+' ends');" \
        "check(!c.querySelector('b'),m+' query')};"
    ok &= run_case("clone-omits-shadow-carrier",
        "(()=>{" SHADOW_CLONE_HELPERS
        "for(const mode of['open','closed']){const{host,root}=make({mode});"
        "document.body.append(host);plain(host.cloneNode(true),'deep '+mode);"
        "check(tags(root)==='b,i'&&root.host===host,'source kept '+mode)}"
        "const{host,root}=make();document.body.append(host);"
        "const shallow=host.cloneNode(false);"
        "check(shallow.shadowRoot===null&&!shallow.firstChild,'shallow');"
        "plain(document.importNode(host,true),'import');"
        "check(host.outerHTML==='<x-h><u></u></x-h>',host.outerHTML);"
        "const wrap=document.createElement('main');wrap.append(host);"
        "document.body.append(wrap);"
        "plain(wrap.cloneNode(true).firstChild,'ancestor');"
        "const range=document.createRange();range.selectNodeContents(wrap);"
        "plain(range.cloneContents().firstChild,'range');"
        "const connected=host.cloneNode(true);document.body.append(connected);"
        "plain(connected,'connected');connected.remove();"
        "const nested=document.createElement('x-n');"
        "nested.attachShadow({mode:'open'}).append(document.createElement('em'));"
        "root.append(nested);"
        "check(!nested.cloneNode(true).firstChild,'nested host');"
        "const outer=make();outer.root.append(host);wrap.remove();"
        "plain(outer.root.firstChild.nextSibling.nextSibling.cloneNode(true),"
        "'host inside a shadow tree');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("clone-omits-carrier-in-template-and-fragment",
        "(()=>{" SHADOW_CLONE_HELPERS
        "let{host,root}=make();const template=document.createElement('template');"
        "template.content.append(host);"
        "plain(template.cloneNode(true).content.firstChild,'template');"
        "plain(template.content.cloneNode(true).firstChild,'template content');"
        "plain(document.importNode(template.content,true).firstChild,'imported');"
        "check(host.shadowRoot===root,'template source');"
        "({host,root}=make());const fragment=document.createDocumentFragment();"
        "fragment.append(host);"
        "plain(fragment.cloneNode(true).firstChild,'fragment');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("clone-drops-carrier-holding-a-script",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const{host,root}=make();const script=document.createElement('script');"
        "script.textContent='globalThis.ranInClone=true';root.append(script);"
        "document.body.append(host);"
        "for(let i=0;i<64;i++){const copy=host.cloneNode(true);plain(copy,'copy');"
        "document.body.append(copy);copy.remove()}"
        "check(globalThis.ranInClone===true,'source ran');"
        "globalThis.ranInClone=false;document.body.append(host.cloneNode(true));"
        "check(globalThis.ranInClone===false,'clone ran');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("shadow-root-itself-cannot-be-cloned",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const{host,root}=make();document.body.append(host);"
        "const refused=(f,m)=>{try{f()}catch(e){check(e.name==='NotSupportedError',"
        "m+' '+e.name);check(tags(root)==='b,i'&&host.shadowRoot===root,m+' kept');"
        "return}throw Error(m+' did not throw')};"
        "refused(()=>root.cloneNode(true),'deep');"
        "refused(()=>root.cloneNode(false),'shallow');"
        "refused(()=>document.importNode(root,true),'import');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("clonable-shadow-root",
        "(()=>{" SHADOW_CLONE_HELPERS
        "for(const value of[false,undefined]){const init={mode:'open'};"
        "if(value!==undefined)init.clonable=value;const{host,root}=make(init);"
        "check(root.clonable===false,'not clonable');"
        "check(host.cloneNode(true).shadowRoot===null,'no copy')}"
        "const whole=(c,m)=>{const r=c.shadowRoot;check(r&&r.host===c,m+' root');"
        "check(r.clonable===true&&r.mode==='open',m+' members');"
        "check(tags(r)==='b,i',m+' contents '+tags(r));"
        "check(!c.querySelector('b'),m+' query')};"
        "const{host,root}=make({mode:'open',clonable:true,delegatesFocus:true});"
        "document.body.append(host);check(root.clonable===true,'getter');"
        "const deep=host.cloneNode(true);whole(deep,'deep');"
        "check(deep.shadowRoot!==root&&tags(deep)==='u','light kept');"
        "check(deep.shadowRoot.delegatesFocus===true,'delegates focus');"
        "const shallow=host.cloneNode(false);whole(shallow,'shallow');"
        "check(!shallow.firstChild,'shallow light');"
        "whole(document.importNode(host,false),'import');"
        "const wrap=document.createElement('main');wrap.append(host);"
        "whole(wrap.cloneNode(true).firstChild,'ancestor');"
        "const template=document.createElement('template');"
        "template.content.append(host);"
        "whole(template.cloneNode(true).content.firstChild,'template');"
        "const inner=document.createElement('x-n'),other=document.createElement('x-m');"
        "inner.attachShadow({mode:'open',clonable:true})"
        ".append(document.createElement('em'));"
        "other.attachShadow({mode:'open'}).append(document.createElement('em'));"
        "root.append(inner,other);const nested=host.cloneNode(true).shadowRoot;"
        "check(tags(nested.children[2].shadowRoot)==='em','nested clonable');"
        "check(nested.children[3].shadowRoot===null,'nested plain');"
        "const closed=make({mode:'closed',clonable:true}),"
        "closedCopy=closed.host.cloneNode(true);"
        "check(closedCopy.shadowRoot===null&&tags(closedCopy)==='u','closed copy');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("inserting-shadow-root-moves-its-children",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const verbs={"
        "appendChild:(t,r)=>{check(t.appendChild(r)===r,'returned root');return 'p,q,b,i'},"
        "insertBefore:(t,r)=>{check(t.insertBefore(r,t.lastChild)===r,'returned root');return 'p,b,i,q'},"
        "insertBeforeNull:(t,r)=>{t.insertBefore(r,null);return 'p,q,b,i'},"
        "replaceChild:(t,r)=>{check(t.replaceChild(r,t.firstChild).localName==='p','returned old');return 'b,i,q'},"
        "append:(t,r)=>{t.append(r);return 'p,q,b,i'},"
        "appendMany:(t,r)=>{t.append(r,document.createElement('s'));return 'p,q,b,i,s'},"
        "prepend:(t,r)=>{t.prepend(r);return 'b,i,p,q'},"
        "before:(t,r)=>{t.lastChild.before(r);return 'p,b,i,q'},"
        "after:(t,r)=>{t.firstChild.after(r);return 'p,b,i,q'},"
        "replaceWith:(t,r)=>{t.firstChild.replaceWith(r);return 'b,i,q'},"
        "replaceChildren:(t,r)=>{t.replaceChildren(r);return 'b,i'},"
        "range:(t,r)=>{const g=document.createRange();g.selectNodeContents(t);"
        "g.insertNode(r);return 'b,i,p,q'},"
        "fragment:(t,r)=>{const f=document.createDocumentFragment();f.append(r);"
        "check(tags(f)==='b,i','fragment took '+tags(f));t.append(f);return 'p,q,b,i'}};"
        "for(const mode of['open','closed'])for(const name of Object.keys(verbs)){"
        "const{host,root}=make({mode});document.body.append(host);"
        "const t=document.createElement('div');"
        "t.append(document.createElement('p'),document.createElement('q'));"
        "document.body.append(t);const want=verbs[name](t,root),at=name+' '+mode;"
        "check(tags(t)===want,at+' target '+tags(t));"
        "check(tags(root)==='',at+' root kept '+tags(root));"
        "check(root.host===host,at+' host link');"
        "check(tags(host)==='u',at+' host view '+tags(host));"
        "if(mode==='open')check(host.shadowRoot===root,at+' shadowRoot');"
        "root.append(document.createElement('em'));"
        "check(tags(root)==='em',at+' root still usable');"
        "check(root.firstChild.getRootNode()===root,at+' root node');"
        "check(host.isConnected&&root.isConnected,at+' connected');host.remove();t.remove()}"
        "{const{host,root}=make();root.replaceChildren();const t=document.createElement('div');"
        "check(t.appendChild(root)===root,'empty');check(tags(t)===''&&root.host===host,'empty kept')}"
        "{const{host,root}=make();host.appendChild(root);"
        "check(tags(host)==='u,b,i','own host '+tags(host));"
        "check(tags(root)===''&&host.shadowRoot===root,'own host kept')}"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("refused-insert-keeps-shadow-tree",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const refused=(name,how,want)=>{const{host,root}=make();"
        "const wrap=document.createElement('main');wrap.append(host);"
        "document.body.append(wrap);const bystander=document.createElement('div');"
        "bystander.append(document.createElement('s'));document.body.append(bystander);"
        "let got='no throw';try{how({host,root,wrap,bystander})}catch(e){got=e.name}"
        "check(got===want,name+' threw '+got);"
        "check(tags(root)==='b,i',name+' root '+tags(root));"
        "check(tags(host)==='u',name+' host '+tags(host));"
        "check(host.shadowRoot===root&&root.host===host,name+' links');"
        "check(host.parentNode===wrap&&wrap.parentNode===document.body,name+' place');"
        "check(root.firstChild.getRootNode()===root,name+' root node');"
        "check(tags(bystander)==='s',name+' bystander');"
        "wrap.remove();bystander.remove()};"
        "const hier='HierarchyRequestError';"
        "refused('into itself',({root})=>root.appendChild(root),hier);"
        "refused('into descendant',({root})=>root.firstChild.appendChild(root),hier);"
        "refused('bad reference',({root,bystander})=>bystander.insertBefore(root,root.firstChild),'NotFoundError');"
        "refused('document',({root})=>document.appendChild(root),hier);"
        "refused('adopt',({root})=>document.adoptNode(root),hier);"
        "refused('adopt other document',({root})=>document.implementation"
        ".createHTMLDocument('').adoptNode(root),hier);"
        "for(const[who,pick]of[['host',s=>s.host],['ancestor',s=>s.wrap]]){"
        "const verb=(name,how)=>{refused(who+' into root '+name,s=>how(s.root,pick(s)),hier);"
        "refused(who+' into tree '+name,s=>how(s.root.firstChild,pick(s)),hier)};"
        "verb('appendChild',(p,n)=>p.appendChild(n));"
        "verb('insertBefore',(p,n)=>p.insertBefore(n,null));"
        "verb('append',(p,n)=>p.append(n));"
        "verb('prepend',(p,n)=>p.prepend(n));"
        "verb('replaceChildren',(p,n)=>p.replaceChildren(n));"
        "refused(who+' replaceChild',s=>s.root.replaceChild(pick(s),s.root.firstChild),hier);"
        "refused(who+' before',s=>s.root.firstChild.before(pick(s)),hier);"
        "refused(who+' after',s=>s.root.firstChild.after(pick(s)),hier);"
        "refused(who+' replaceWith',s=>s.root.firstChild.replaceWith(pick(s)),hier);"
        "refused(who+' range',s=>{const g=document.createRange();"
        "g.selectNodeContents(s.root);g.insertNode(pick(s))},hier)}"
        "const{host:outer,root:outerRoot}=make(),inner=make();"
        "outerRoot.append(inner.host);let name='no throw';"
        "try{inner.root.appendChild(outer)}catch(e){name=e.name}"
        "check(name===hier,'outer host into inner root '+name);"
        "check(tags(outerRoot)==='b,i,x-h'&&inner.host.shadowRoot===inner.root,'nested kept');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("template-cannot-enter-its-own-content",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const hier='HierarchyRequestError',threw=f=>{try{f()}catch(e){return e.name}return 'no throw'};"
        "const wrap=document.createElement('main'),t=document.createElement('template');"
        "wrap.append(t);document.body.append(wrap);"
        "const inside=document.createElement('p');t.content.append(inside);"
        "check(threw(()=>t.content.appendChild(t))===hier,'template into content');"
        "check(threw(()=>t.content.appendChild(wrap))===hier,'ancestor into content');"
        "check(threw(()=>inside.appendChild(t))===hier,'template under content');"
        "check(threw(()=>inside.append(wrap))===hier,'ancestor under content');"
        "check(t.parentNode===wrap&&wrap.parentNode===document.body,'tree moved');"
        "check(tags(t.content)==='p'&&!inside.firstChild,'content moved');"
        "t.content.append(document.createElement('s'));"
        "check(tags(t.content)==='p,s','unrelated insert');"
        "const copy=t.cloneNode(true);check(tags(copy.content)==='p,s','clone');"
        "const other=document.createElement('template');"
        "check(threw(()=>t.content.append(other))==='no throw','other template');"
        "const f=document.createDocumentFragment(),e=document.createElement('p');f.append(e);"
        "check(threw(()=>wrap.append(f))==='no throw','plain fragment');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("serialization-omits-shadow-carrier",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const same=(got,want,m)=>check(got===want,m+': '+got);"
        "const named=(n,l,d,init)=>{const h=document.createElement(n);if(l)h.innerHTML=l;"
        "h.attachShadow(init||{mode:'open'}).innerHTML=d;return h};"
        "for(const mode of['open','closed']){"
        "const h=named('x-h','<u>l</u>','<b>d</b>',{mode}),w=document.createElement('main');"
        "w.append(h);document.body.append(w);"
        "same(w.innerHTML,'<x-h><u>l</u></x-h>',mode+' ancestor inner');"
        "same(w.outerHTML,'<main><x-h><u>l</u></x-h></main>',mode+' ancestor outer');"
        "same(h.innerHTML,'<u>l</u>',mode+' host inner');"
        "same(h.outerHTML,'<x-h><u>l</u></x-h>',mode+' host outer');"
        "const mark='sen'+'tinel';h.shadowRoot?.append(mark);"
        "check(mode==='closed'||!document.body.innerHTML.includes(mark),mode+' body');w.remove()}"
        "{const h=named('x-h','','<b>d</b>'),w=document.createElement('main');w.append(h);"
        "same(h.innerHTML,'','empty host');same(w.innerHTML,'<x-h></x-h>','empty ancestor')}"
        "{const h=named('x-h','<u>l</u>','<b>d</b>'),w=document.createElement('main');w.append(h);"
        "same(w.innerHTML,'<x-h><u>l</u></x-h>','detached')}"
        "{const inner=named('x-n','<i>il</i>','<em>id</em>'),outer=named('x-h','<u>ol</u>','<b>od</b>'),"
        "w=document.createElement('main');outer.shadowRoot.append(inner);w.append(outer);"
        "same(outer.shadowRoot.innerHTML,'<b>od</b><x-n><i>il</i></x-n>','root keeps its own, not nested');"
        "same(w.innerHTML,'<x-h><u>ol</u></x-h>','nested ancestor');"
        "outer.append(named('x-m','<i>l2</i>','<em>d2</em>'));"
        "same(outer.innerHTML,'<u>ol</u><x-m><i>l2</i></x-m>','nested light host');"
        "same(w.innerHTML,'<x-h><u>ol</u><x-m><i>l2</i></x-m></x-h>','nested light ancestor')}"
        "{const t=document.createElement('template'),s=document.createElement('section'),"
        "n=document.createElement('template');"
        "t.content.append(named('x-h','<u>l</u>','<b>d</b>'));s.append(t);"
        "same(t.innerHTML,'<x-h><u>l</u></x-h>','template inner');"
        "same(t.outerHTML,'<template><x-h><u>l</u></x-h></template>','template outer');"
        "same(s.innerHTML,'<template><x-h><u>l</u></x-h></template>','template under ancestor');"
        "n.content.append(t);s.append(n);"
        "same(n.innerHTML,'<template><x-h><u>l</u></x-h></template>','nested template');"
        "same(s.innerHTML,'<template><template><x-h><u>l</u></x-h></template></template>','nested template under ancestor')}"
        "{const h=named('x-h','<u>l</u>','<b>d</b>',{mode:'open',clonable:true});"
        "same(h.outerHTML,'<x-h><u>l</u></x-h>','clonable outer');"
        "const c=h.cloneNode(true);same(c.innerHTML,'<u>l</u>','clonable copy');"
        "same(c.shadowRoot.innerHTML,'<b>d</b>','clonable copy root')}"
        "{const h=named('x-h','<u>l</u>','<b>d</b>'),w=document.createElement('main');"
        "w.append(h);document.body.append(w);const want='<x-h><u>l</u></x-h>';"
        "const a=document.createElement('div');a.innerHTML=w.innerHTML;"
        "same(a.innerHTML,want,'through innerHTML');check(a.firstChild.shadowRoot===null,'no root');"
        "const b=document.createElement('div');b.insertAdjacentHTML('beforeend',w.innerHTML);"
        "same(b.innerHTML,want,'through insertAdjacentHTML');"
        "const c=document.createElement('div');c.setHTMLUnsafe(w.innerHTML);"
        "same(c.innerHTML,want,'through setHTMLUnsafe');"
        "const g=document.createRange();g.selectNodeContents(w);"
        "const e=document.createElement('div');e.append(g.cloneContents());"
        "same(e.innerHTML,want,'through a range clone');w.remove()}"
        "{const d=document.createElement('div');"
        "d.innerHTML='<div data-tilefinch-shadow-root=\"\">forged</div>';"
        "same(d.innerHTML,'<div data-tilefinch-shadow-root=\"\">forged</div>','forged marker')}"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("serialization-beside-shadow-trees-keeps-markup",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const same=(got,want,m)=>check(got===want,m+': '+got);"
        "const source='<svg viewBox=\"0 0 1 1\"><circle r=\"1\"></circle>"
        "<foreignObject><p>x</p></foreignObject></svg><math><mi>y</mi></math>"
        "<pre>\\n\\nz</pre><textarea>\\n\\nq</textarea><noscript><b>n</b></noscript>"
        "<xmp><i></xmp><iframe>a<b</iframe><table><tbody><tr><td>c</td></tr></tbody></table>"
        "<img src=\"a.png\" alt=\"&quot;\"><input disabled><wbr><hr><p>&nbsp;&lt;</p>"
        "<style>a>b{}</style><!--c--><template><i>t</i></template>',"
        "want=source.replace('<pre>\\n\\n','<pre>\\n')"
        ".replace('<textarea>\\n\\n','<textarea>\\n').replace('disabled','disabled=\"\"');"
        "make({mode:'open'});"
        "const d=document.createElement('div');d.innerHTML=source;same(d.innerHTML,want,'plain');"
        "const h=document.createElement('x-h');h.innerHTML=source;"
        "h.attachShadow({mode:'open'}).innerHTML='<b>d</b>';same(h.innerHTML,want,'host');"
        "const w=document.createElement('main');w.append(h);"
        "same(w.innerHTML,'<x-h>'+want+'</x-h>','ancestor');"
        "const v=document.createElement('br');v.append(document.createElement('i'));"
        "const p=document.createElement('div');p.append(v);same(p.innerHTML,'<br>','void child');"
        "const t=document.createTextNode('a&<>\"b'),q=document.createElement('div');q.append(t);"
        "same(q.innerHTML,'a&amp;&lt;&gt;\"b','text escapes');"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("text-content-omits-shadow-text",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const same=(got,want,m)=>check(got===want,m+': '+got);"
        "const named=(n,l,d,init)=>{const h=document.createElement(n);if(l)h.innerHTML=l;"
        "h.attachShadow(init||{mode:'open'}).innerHTML=d;return h};"
        "for(const mode of['open','closed']){"
        "const h=named('x-h','<u>l</u>','<b>d</b>',{mode}),w=document.createElement('main');"
        "w.append(h);document.body.append(w);"
        "same(w.textContent,'l',mode+' ancestor');same(h.textContent,'l',mode+' host');"
        "const g=document.createRange();g.selectNodeContents(w);"
        "same(g.toString(),'l',mode+' range');w.remove()}"
        "{const h=named('x-h','','<b>d</b>'),w=document.createElement('main');w.append(h);"
        "same(h.textContent,'','empty host');same(w.textContent,'','empty ancestor');"
        "same(h.shadowRoot.textContent,'d','root')}"
        "{const h=named('x-h','one','<b>d</b>'),w=document.createElement('main');w.append(h);"
        "same(h.textContent,'one','single text child');same(w.textContent,'one','single text ancestor')}"
        "{const inner=named('x-n','<i>il</i>','<em>id</em>'),outer=named('x-h','<u>ol</u>','<b>od</b>'),"
        "w=document.createElement('main');outer.shadowRoot.append(inner);w.append(outer);"
        "same(outer.shadowRoot.textContent,'odil','root keeps its own, not nested');"
        "same(w.textContent,'ol','nested ancestor');"
        "outer.append(named('x-m','<i>l2</i>','<em>d2</em>'));w.append('tail');"
        "same(outer.textContent,'oll2','nested light host');"
        "same(w.textContent,'oll2tail','text after the host')}"
        "{const h=named('x-h','<u>l</u>','<b>d</b>'),f=document.createDocumentFragment(),"
        "t=document.createElement('template');f.append(h);same(f.textContent,'l','fragment');"
        "t.content.append(f);same(t.content.textContent,'l','template content');"
        "same(t.textContent,'','template')}"
        "{const h=named('x-h','<u>l</u>','<b>d</b>',{mode:'open',clonable:true}),c=h.cloneNode(true);"
        "same(c.textContent,'l','clonable copy');same(c.shadowRoot.textContent,'d','clonable copy root')}"
        "{const d=document.createElement('div');"
        "d.innerHTML='<div data-tilefinch-shadow-root=\"\">forged</div>';"
        "same(d.textContent,'forged','forged marker')}"
        "globalThis.pocSummary='OK'})()");
    ok &= run_styled_case("inner-text-omits-shadow-text",
        "(()=>{" SHADOW_CLONE_HELPERS
        "const same=(got,want,m)=>check(got===want,m+': '+JSON.stringify(got));"
        "const named=(l,d,init)=>{const h=document.createElement('x-h');h.style.display='block';"
        "if(l)h.innerHTML=l;h.attachShadow(init||{mode:'open'}).innerHTML=d;return h};"
        "const place=e=>{const w=document.createElement('main');w.append(e);document.body.append(w);return w};"
        "for(const mode of['open','closed']){const h=named('<u>l</u>','<b>d</b>',{mode}),w=place(h);"
        "same(w.innerText,'',mode+' unslotted ancestor');same(h.innerText,'',mode+' unslotted host');w.remove()}"
        "{const h=named('','<b>d</b>'),w=place(h);same(w.innerText,'','no light');w.remove()}"
        "{const h=named('slotted<u>l</u>','A<slot></slot>B'),w=place(h);"
        "same(w.innerText,'slottedl','slotted ancestor');same(h.innerText,'slottedl','slotted host');w.remove()}"
        "{const h=named('<p>l1</p><p>l2</p>','<div>d1<slot></slot></div><div>d2</div>'),w=place(h);"
        "const a=document.createElement('p'),z=document.createElement('p');"
        "a.textContent='before';z.textContent='after';w.prepend(a);w.append(z);"
        "same(w.innerText,'before\\n\\nl1\\n\\nl2\\n\\nafter','blocks');w.remove()}"
        "{const inner=named('<i>il</i>','<em>id</em>'),outer=named('<u>ol</u>','<div id=box>od</div>'),"
        "w=place(outer);outer.shadowRoot.firstChild.append(inner);"
        "same(w.innerText,'','nested');"
        "same(outer.shadowRoot.firstChild.innerText,'od','inside a shadow tree, nested host left out');w.remove()}"
        "{const h=named('<u>l</u>','<b>d</b>'),w=place(h);w.style.display='none';"
        "same(w.innerText,'l','not rendered ancestor');same(h.innerText,'l','not rendered host');w.remove()}"
        "{const h=named('<u>l</u>','<b>d</b>'),d=document.createElement('div');d.append(h);"
        "same(d.innerText,'l','detached ancestor');same(h.innerText,'l','detached host')}"
        "{const r=document.createElement('tr'),t=document.createElement('table'),"
        "c=document.createElement('td'),x=document.createElement('x-c');c.textContent='t1';"
        "x.style.display='table-cell';x.textContent='t2';"
        "x.attachShadow({mode:'open'}).innerHTML='<i>hidden</i><slot></slot>';"
        "r.append(c,x);t.append(r);document.body.append(t);"
        "same(t.innerText,'t1\\tt2','host as table cell');t.remove()}"
        "{const t=document.createElement('table'),b=document.createElement('tbody'),"
        "h=document.createElement('div');h.style.display='table-row';"
        "for(const v of['a','b']){const c=document.createElement('span');"
        "c.style.display='table-cell';c.textContent=v;h.append(c)}"
        "h.attachShadow({mode:'open'}).innerHTML='<slot></slot>';b.append(h);t.append(b);"
        "document.body.append(t);same(t.innerText,'a\tb','cells beside a carrier');t.remove()}"
        "{const w=document.createElement('div'),h=document.createElement('div');"
        "document.body.append(w);w.append(h);h.append('x');"
        "h.attachShadow({mode:'open'}).innerHTML='y<slot></slot>z';"
        "same(h.innerText,'x','div host');w.remove()}"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("shadow-tree-during-custom-construction",
        "(()=>{class Card extends HTMLElement{constructor(){super();"
        "this.attachShadow({mode:'open'}).innerHTML='<b>Ready</b>'}}"
        "customElements.define('constructor-card',Card);"
        "const card=document.createElement('constructor-card');"
        "if(!(card instanceof Card)||card.childNodes.length!==0||"
        "card.shadowRoot?.textContent!=='Ready')throw Error('shadow construction');"
        "document.body.append(card);card.remove();document.body.append(card);"
        "if(!card.shadowRoot.isConnected||card.shadowRoot.host!==card)"
        "throw Error('shadow construction continuity');globalThis.pocSummary='OK'})()");
    ok &= run_case("element-scroll-setter-events",
        "(async()=>{const e=document.createElement('div');"
        "e.style.cssText='width:20px;height:20px;overflow:auto';"
        "e.innerHTML='<div style=\"height:200px;width:200px\"></div>';document.body.append(e);"
        "let count=0,last=0;e.addEventListener('scroll',()=>{count++;last=e.scrollTop});"
        "e.scrollTop=10;e.scrollTop=20;"
        "if(count)throw Error('synchronous scroll event');"
        "await new Promise(r=>setTimeout(r,0));"
        "if(count!==1||last!==20)throw Error('coalesced scroll event '+count+'/'+last);"
        "e.scrollTop=20;await new Promise(r=>setTimeout(r,0));"
        "if(count!==1)throw Error('unchanged scroll event');"
        "e.scrollTop=0;await new Promise(r=>setTimeout(r,0));"
        "if(count!==2||last!==0)throw Error('return scroll event');"
        "globalThis.pocSummary='OK';})().catch(e=>{globalThis.pocSummary=String(e)})");
    ok &= run_case("private-wrapper-reference-deref",
        "(()=>{const p=document.createElement('div'),n=document.createElement('span');"
        "document.body.append(p);p.append(n);"
        "WeakRef.prototype.deref=function(){throw Error('private reference exposed')};"
        "if(n.parentNode!==p||p.firstChild!==n)throw Error('cached wrapper identity');"
        "n.remove();p.append(n);if(n.parentNode!==p)throw Error('rebound reference');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("ordinary-native-wrapper-identity",
        "(()=>{const e=document.createElement('div');document.body.append(e);"
        "const child=document.createElementNS('http://www.w3.org/2000/svg','svg');"
        "e.append(child);if(Object.hasOwn(e,'__tilefinchStableKey')||"
        "Object.hasOwn(e,'__tilefinchRemoteTagName')||e.tagName!=='DIV'||"
        "e.nodeType!==1||e.parentNode!==document.body||child.parentNode!==e||"
        "document.documentElement.parentNode!==document||"
        "child.namespaceURI!=='http://www.w3.org/2000/svg')throw Error('native identity');"
        "e.remove();document.body.append(e);"
        "if(child.parentElement!==e||!child.isConnected)throw Error('reattach identity');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("shared-custom-element-native-members",
        "(()=>{class Base extends HTMLElement{get custom(){return 'base'}}"
        "class Card extends Base{getAttribute(name){return super.getAttribute(name)}}"
        "customElements.define('shared-card',Card);const nodes=[];"
        "for(let i=0;i<80;i++){const n=document.createElement('shared-card');"
        "n.setAttribute('id','shared-'+i);n.textContent=String(i);document.body.append(n);"
        "if(Object.getPrototypeOf(n)!==Card.prototype||!(n instanceof Base)||"
        "Object.hasOwn(n,'appendChild')||Object.hasOwn(n,'nodeType')||"
        "n.custom!=='base'||n.getAttribute('id')!=='shared-'+i||n.textContent!==String(i))"
        "throw Error('shared custom members '+i);nodes.push(n);}"
        "nodes[0].remove();document.body.append(nodes[0]);"
        "const d=document.implementation.createHTMLDocument('Other'),e=d.createElement('div');"
        "e.setAttribute('id','secondary');e.textContent='Detached';"
        "if(e.ownerDocument!==d||e.textContent!=='Detached'||e.isConnected||"
        "e.getAttribute('id')!=='secondary')throw Error('secondary members');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("frozen-custom-element-prototype-fallback",
        "(()=>{Object.freeze(HTMLElement.prototype);class Card extends HTMLElement{}"
        "customElements.define('frozen-card',Card);const n=document.createElement('frozen-card');"
        "document.body.append(n);n.innerHTML='<b>Safe</b>';"
        "if(n.querySelector('b').textContent!=='Safe'||!n.isConnected)"
        "throw Error('frozen custom prototype');globalThis.pocSummary='OK';})()");
    ok &= run_case("materialized-node-attributes",
        "(()=>{const other=document.implementation.createHTMLDocument('Other'),"
        "node=other.createElement('span');node.setAttribute('id','materialized');"
        "node.setAttributeNS('urn:test','p:value','kept');node.textContent='Keep';"
        "document.body.append(node);if(node.ownerDocument!==document||"
        "node.parentElement!==document.body||node.textContent!=='Keep'||"
        "document.getElementById('materialized')?.getAttribute('id')!=='materialized'||"
        "node.getAttributeNS('urn:test','value')!=='kept')throw Error('materialization');"
        "node.remove();document.body.append(node);"
        "if(node.getAttribute('id')!=='materialized')throw Error('reattach attributes');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("bounded-shared-root-adoption-and-cache-reuse",
        "(async()=>{const sheet=new CSSStyleSheet();sheet.replaceSync('.label{color:red}');"
        "for(let i=0;i<200;i++){const host=document.createElement('div');host.id='card-'+i;"
        "document.body.append(host);const root=host.attachShadow({mode:'open'});"
        "root.adoptedStyleSheets=[sheet];root.innerHTML='<b class=label>'+i+'</b>';"
        "if(i%16===15)await new Promise(resolve=>setTimeout(resolve,0));}"
        "for(let i=0;i<500;i++){const host=document.createElement('div');"
        "document.body.append(host);host.attachShadow({mode:'open'}).innerHTML='<i>Transient</i>';"
        "if(i===280)host.id='later-card';else host.remove();"
        "if(i%16===15)await new Promise(resolve=>setTimeout(resolve,0));}"
        "for(let i=0;i<200;i++){const host=document.getElementById('card-'+i),root=host.shadowRoot;"
        "if(!root||root.host!==host||root.adoptedStyleSheets[0]!==sheet||"
        "root.querySelector('b').textContent!==String(i))throw Error('connected root lost '+i);}"
        "if(document.getElementById('later-card').shadowRoot?.textContent!=='Transient')"
        "throw Error('reclaimed cache entry not reused');"
        "globalThis.pocSummary='OK';})().catch(error=>{"
        "globalThis.pocSummary=error.name+':'+error.message})");
    ok &= run_case("disconnected-shadow-component-reclamation",
        "(async()=>{const held=document.createElement('div');document.body.append(held);"
        "const retained=held.attachShadow({mode:'closed'});retained.innerHTML='<b>Keep</b>';"
        "held.remove();for(let i=0;i<220;i++){const host=document.createElement('div');"
        "document.body.append(host);const root=host.attachShadow({mode:'open'});"
        "root.innerHTML='<span>Transient</span>';host.remove();"
        "if(i%16===15)await new Promise(resolve=>setTimeout(resolve,0));}"
        "document.body.append(held);if(retained.host!==held||!retained.isConnected||"
        "retained.querySelector('b').textContent!=='Keep'||held.shadowRoot!==null)"
        "throw Error('author-held detached root lost');"
        "const host=document.createElement('div');document.body.append(host);"
        "host.attachShadow({mode:'open'}).innerHTML='<a href=\"/last\">Last</a>';"
        "if(host.shadowRoot.querySelector('a').textContent!=='Last')throw Error('new root');"
        "globalThis.pocSummary='OK';})().catch(error=>{"
        "globalThis.pocSummary=error.name+':'+error.message})");
    ok &= run_case("shared-shadow-native-members",
        "(()=>{const roots=[],hosts=[];for(let i=0;i<48;i++){"
        "const host=document.createElement('div');document.body.append(host);"
        "const root=host.attachShadow({mode:i%2?'closed':'open'});"
        "root.innerHTML='<a href=\"/item\"><span>Card '+i+'</span></a>';"
        "if(Object.getOwnPropertyDescriptor(root,'appendChild'))"
        "throw Error('native members copied per component');"
        "if(root.host!==host||root.ownerDocument!==document||!root.isConnected||"
        "root.parentNode!==null||root.parentElement!==null||root.namespaceURI!==null||"
        "root.nodeType!==11||root.nodeName!=='#document-fragment'||"
        "root.querySelector('span').textContent!=='Card '+i||"
        "root.firstChild.parentNode!==root||host.childNodes.length!==0||"
        "host.shadowRoot!==(i%2?null:root))throw Error('root identity/view');"
        "roots.push(root);hosts.push(host);}"
        "const getter=Object.getOwnPropertyDescriptor(hosts[0],'children').get;"
        "if(getter!==Object.getOwnPropertyDescriptor(hosts[1],'children').get)"
        "throw Error('host getter copied per component');"
        "hosts[0].remove();if(roots[0].isConnected)throw Error('detached state');"
        "document.body.append(hosts[0]);if(!roots[0].isConnected)throw Error('reattach');"
        "const other=document.implementation.createHTMLDocument('Other'),"
        "host=other.createElement('div'),root=host.attachShadow({mode:'open'});"
        "root.appendChild(other.createTextNode('detached'));"
        "if(root.textContent!=='detached'||root.ownerDocument!==other||root.isConnected||"
        "root.firstChild.parentNode!==root)throw Error('secondary root');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("frozen-shadow-prototype-fallback",
        "(()=>{Object.freeze(ShadowRoot.prototype);"
        "const host=document.createElement('div');document.body.append(host);"
        "const root=host.attachShadow({mode:'open'});root.innerHTML='<b>Safe</b>';"
        "if(root.querySelector('b').textContent!=='Safe'||!root.isConnected)"
        "throw Error('frozen prototype');globalThis.pocSummary='OK';})()");
    ok &= run_case("large-listener-identity-lifecycle",
        "(()=>{const target=new EventTarget(),callbacks=[];let calls=0;"
        "for(let i=0;i<80;i++){const f=()=>calls++;callbacks.push(f);"
        "target.addEventListener('probe',f);target.addEventListener('probe',f);}"
        "target.removeEventListener('probe',callbacks[40]);"
        "target.addEventListener('probe',callbacks[40],{once:true});"
        "target.addEventListener('probe',callbacks[40],true);"
        "target.dispatchEvent(new Event('probe'));"
        "if(calls!==81)throw Error('first dispatch:'+calls);"
        "target.addEventListener('probe',callbacks[40]);"
        "target.dispatchEvent(new Event('probe'));"
        "if(calls!==162)throw Error('reregister:'+calls);"
        "for(const f of callbacks){target.removeEventListener('probe',f);"
        "target.removeEventListener('probe',f,true);}"
        "target.dispatchEvent(new Event('probe'));"
        "if(calls!==162)throw Error('removed listener retained');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("listener-dedup-without-author-array-callback",
        "(()=>{const target=new EventTarget();let calls=0;"
        "const callback=()=>calls++,some=Array.prototype.some;"
        "try{Array.prototype.some=()=>{throw Error('author some called')};"
        "target.addEventListener('probe',callback);"
        "target.addEventListener('probe',callback,{once:true,passive:true});"
        "target.addEventListener('probe',callback,true);"
        "}finally{Array.prototype.some=some;}"
        "target.dispatchEvent(new Event('probe'));"
        "target.dispatchEvent(new Event('probe'));"
        "if(calls!==4)throw Error('listener identity/options changed:'+calls);"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("offset-parent-terminals",
        "(()=>{const b=document.body,h=document.documentElement;"
        "for(const position of ['static','relative','fixed']){"
        "b.style.position=h.style.position=position;"
        "if(b.offsetParent!==null||h.offsetParent!==null)"
        "throw Error('root/body terminal:'+position);}"
        "b.style.position=h.style.position='static';"
        "const p=document.createElement('div'),c=document.createElement('i');"
        "p.style.position='relative';b.appendChild(p);p.appendChild(c);"
        "if(c.offsetParent!==p||p.offsetParent!==b)throw Error('ancestor');"
        "let at=c,steps=0;while(at&&steps<8){at=at.offsetParent;steps++;}"
        "if(at!==null||steps!==3)throw Error('chain did not terminate');"
        "globalThis.pocSummary='OK';})()");
    ok &= run_case("mutation-observer-enumeration-wrapper",
        "(()=>{const Original=MutationObserver,probe=new Original(()=>{});"
        "for(const key of ['observe','disconnect','takeRecords']){"
        "const d=Object.getOwnPropertyDescriptor(Original.prototype,key);"
        "if(!d||!d.enumerable||!d.writable||!d.configurable)"
        "throw Error('operation descriptor:'+key);}"
        "function Wrapper(cb){this.inner=new Original(cb);}"
        "for(const key in probe)if(typeof probe[key]==='function')"
        "Wrapper.prototype[key]=function(...args){"
        "return this.inner[key](...args);};"
        "globalThis.MutationObserver=Wrapper;"
        "const target=document.createElement('div');document.body.append(target);"
        "const drained=new MutationObserver(()=>{throw Error('drained callback')});"
        "drained.observe(target,{attributes:true});target.setAttribute('x','1');"
        "if(drained.takeRecords().length!==1||drained.takeRecords().length!==0)"
        "throw Error('records');drained.disconnect();target.setAttribute('x','2');"
        "if(drained.takeRecords().length!==0)throw Error('disconnect');"
        "const live=new MutationObserver(records=>{"
        "if(records.length!==1||records[0].attributeName!=='x')"
        "throw Error('delivery');live.disconnect();globalThis.pocSummary='OK';});"
        "live.observe(target,{attributes:true});target.setAttribute('x','3');})()");
    ok &= run_case("lazy-motion-observer-intrinsic",
        "(()=>{if(__tilefinchRootCensus.motionScans!==undefined)"
        "throw Error('motion was not lazy');"
        "const Original=MutationObserver;let publicCalls=0;"
        "globalThis.MutationObserver=function(){publicCalls++;"
        "throw Error('public constructor used internally');};"
        "for(const key of ['observe','disconnect','takeRecords'])"
        "Original.prototype[key]=()=>{publicCalls++;throw Error('public method');};"
        "const style=document.createElement('style');"
        "style.textContent='@keyframes pulse{from{opacity:0}to{opacity:1}}';"
        "document.head.appendChild(style);__tilefinchMaybeStartMotion();"
        "if(__tilefinchRootCensus.mutationObservers!==0)throw Error('registration');"
        "const target=document.createElement('div');document.body.appendChild(target);"
        "queueMicrotask(()=>{if(publicCalls!==0||"
        "__tilefinchRootCensus.motionScans<1)throw Error('intrinsic delivery');"
        /* The document's tree changing asks for another scan; a tree outside
           it does not. */
        "const scans=__tilefinchRootCensus.motionScans;"
        "const detached=document.createElement('div');"
        "detached.appendChild(document.createElement('p'));"
        "queueMicrotask(()=>{if(__tilefinchRootCensus.motionScans!==scans)"
        "throw Error('detached tree scanned');"
        "target.appendChild(document.createElement('p'));"
        "queueMicrotask(()=>{if(__tilefinchRootCensus.motionScans<=scans)"
        "throw Error('document tree not scanned');"
        "globalThis.pocSummary='OK';});});});})()");
    /* DOMStringMap exposes the element's data-* attributes as own
       enumerable properties: Bootstrap 5 reads every component option
       through Object.keys(element.dataset). */
    ok &= run_case("dataset-enumerates-data-attributes",
        "(()=>{const e=document.createElement('div');document.body.append(e);"
        "e.setAttribute('data-bs-toggle','modal');e.setAttribute('title','t');"
        "e.setAttribute('data-bs-target','#m');e.setAttribute('data-x','1');"
        "const keys=Object.keys(e.dataset).join();"
        "if(keys!=='bsToggle,bsTarget,x')throw Error('keys '+keys);"
        "const seen=[];for(const k in e.dataset)seen.push(k);"
        "if(seen.join()!==keys)throw Error('for-in '+seen.join());"
        "const copy={...e.dataset};"
        "if(Object.keys(copy).join()!==keys||copy.bsTarget!=='#m')"
        "throw Error('spread '+JSON.stringify(copy));"
        "const json=JSON.stringify(e.dataset);"
        "if(json!=='{\"bsToggle\":\"modal\",\"bsTarget\":\"#m\",\"x\":\"1\"}')"
        "throw Error('json '+json);"
        "if(Object.entries(e.dataset).map(p=>p.join('=')).join()!=="
        "'bsToggle=modal,bsTarget=#m,x=1')throw Error('entries');"
        "const d=Object.getOwnPropertyDescriptor(e.dataset,'bsToggle');"
        "if(!d||d.value!=='modal'||!d.enumerable||!d.writable||!d.configurable)"
        "throw Error('descriptor');"
        "if(Object.getOwnPropertyDescriptor(e.dataset,'missing')!==undefined"
        "||Object.hasOwn(e.dataset,'missing')||!Object.hasOwn(e.dataset,'x'))"
        "throw Error('own property');"
        "if(e.dataset!==e.dataset)throw Error('not the same object');"
        "if(Object.prototype.toString.call(e.dataset)!=='[object DOMStringMap]')"
        "throw Error('brand');"
        "e.dataset.newKey='v';if(e.getAttribute('data-new-key')!=='v'"
        "||Object.keys(e.dataset).at(-1)!=='newKey')throw Error('set');"
        "Object.assign(e.dataset,{other:'2'});"
        "if(e.getAttribute('data-other')!=='2')throw Error('assign');"
        "delete e.dataset.x;"
        "if(e.hasAttribute('data-x')||'x' in e.dataset)throw Error('delete');"
        "let thrown=null;try{e.dataset['a-b']='1'}catch(x){thrown=x}"
        "if(thrown?.name!=='SyntaxError')throw Error('a-b accepted');"
        "if(e.dataset['bs-toggle']!==undefined)throw Error('dashed read');"
        "if(typeof e.dataset.toString!=='function')throw Error('builtin shadowed');"
        "const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');"
        "svg.setAttribute('data-kind','icon');"
        "if(Object.keys(svg.dataset).join()!=='kind'||svg.dataset.kind!=='icon')"
        "throw Error('svg dataset');"
        "globalThis.pocSummary='OK'})()");
    /* A dataset write is an ordinary attribute write: removing or replacing
       the attribute afterwards leaves no stale value behind. */
    ok &= run_case("dataset-follows-attribute-changes",
        "(()=>{const e=document.createElement('div');document.body.append(e);"
        "e.dataset.open='1';e.removeAttribute('data-open');"
        "if(e.dataset.open!==undefined||'open' in e.dataset"
        "||Object.keys(e.dataset).length)"
        "throw Error('stale after removeAttribute: '+e.dataset.open);"
        "e.dataset.state='a';e.setAttribute('data-state','b');"
        "if(e.dataset.state!=='b'||e.getAttributeNS(null,'data-state')!=='b')"
        "throw Error('stale after setAttribute: '+e.dataset.state);"
        "e.setAttributeNS(null,'data-ns','1');e.setAttribute('data-ns','2');"
        "if(e.getAttributeNS(null,'data-ns')!=='2'||e.dataset.ns!=='2')"
        "throw Error('namespace read stale after setAttribute');"
        "e.removeAttribute('data-ns');"
        "if(e.getAttributeNS(null,'data-ns')!==null||e.hasAttributeNS(null,'data-ns')"
        "||e.dataset.ns!==undefined||[...e.attributes].some(a=>a.name==='data-ns'))"
        "throw Error('namespace record kept after removeAttribute');"
        "if([...e.attributes].map(a=>a.name).join()!=='data-state')"
        "throw Error('attributes '+[...e.attributes].map(a=>a.name).join());"
        "globalThis.pocSummary='OK'})()");
    /* A CSSOM write changes the style attribute, so an observer watching
       attributes gets one record per write, with the previous value. */
    ok &= run_case("cssom-style-writes-queue-attribute-records",
        "(async()=>{const e=document.createElement('div');document.body.append(e);"
        "const records=[],others=[];"
        "const observer=new MutationObserver(list=>{for(const r of list)"
        "records.push(r.type+':'+r.attributeName+':'+r.oldValue)});"
        "observer.observe(e,{attributes:true,attributeOldValue:true,"
        "attributeFilter:['style']});"
        "new MutationObserver(list=>others.push(...list))"
        ".observe(e,{attributes:true,attributeFilter:['class']});"
        "const want=[];const step=write=>{"
        "want.push('attributes:style:'+e.getAttribute('style'));write()};"
        "step(()=>{e.style.color='red'});"
        "step(()=>{e.style.setProperty('display','none')});"
        "step(()=>{e.style.setProperty('display','')});"
        "step(()=>{e.style.removeProperty('color')});"
        "step(()=>{e.style.cssText='width: 4px'});"
        "step(()=>{e.style.flex='1 1 0px'});"
        "e.style.removeProperty('height');"
        "await Promise.resolve();"
        "if(records.join('|')!==want.join('|'))"
        "throw Error('records '+records.join('|')+' want '+want.join('|'));"
        "if(others.length)throw Error('filtered observer saw style');"
        "globalThis.pocSummary='OK'})().catch(e=>{globalThis.pocSummary=String(e)})");
    /* getElementsByClassName/TagName return live collections: the common
       drain loops terminate, and later insertions are seen. */
    ok &= run_case("element-queries-return-live-collections",
        "(()=>{const root=document.createElement('div');document.body.append(root);"
        "for(let i=0;i<5;i++){const s=document.createElement('span');"
        "s.className='active x';root.append(s)}"
        "const byClass=document.getElementsByClassName('active'),"
        "inRoot=root.getElementsByClassName('x  active'),"
        "tags=root.getElementsByTagName('span'),"
        "all=document.getElementsByTagName('SPAN');"
        "if(!(byClass instanceof HTMLCollection)||!(tags instanceof HTMLCollection)"
        "||!(all instanceof HTMLCollection))throw Error('not HTMLCollection');"
        "if(byClass.length!==5||inRoot.length!==5||all.length!==5)"
        "throw Error('initial '+byClass.length+','+inRoot.length+','+all.length);"
        "let n=0;while(byClass.length&&n++<10)byClass[0].classList.remove('active');"
        "if(n!==5||byClass.length||inRoot.length)throw Error('class drain '+n);"
        "n=0;while(tags.length&&n++<10)tags[0].parentNode.removeChild(tags[0]);"
        "if(n!==5||tags.length||all.length)throw Error('tag drain '+n);"
        "const added=root.appendChild(document.createElement('span'));"
        "added.className='active';added.id='named';"
        "if(tags.length!==1||tags[0]!==added||tags.item(0)!==added"
        "||byClass[0]!==added||tags.namedItem('named')!==added"
        "||[...tags][0]!==added||Array.from(byClass).length!==1)"
        "throw Error('insertion not seen');"
        "const odd=root.getElementsByTagName('x:odd');"
        "if(odd.length!==0)throw Error('odd tag');"
        "if(document.getElementsByClassName('  ').length!==0)"
        "throw Error('empty class list');"
        "const detached=document.createElement('div');"
        "detached.innerHTML='<p class=k></p><p></p>';"
        "const inDetached=detached.getElementsByClassName('k');"
        "if(inDetached.length!==1)throw Error('detached');"
        "detached.firstChild.className='';"
        "if(inDetached.length!==0)throw Error('detached change not seen');"
        "globalThis.pocSummary='OK'})()");
    /* A live collection reads its list once between DOM changes, and every
       kind of change made in the same task is seen at once. */
    ok &= run_case("live-collections-reuse-reads-between-mutations",
        "(()=>{const stats=__tilefinchRetentionStats,list=document.createElement('ul');"
        "document.body.append(list);"
        "for(let i=0;i<50;i++)list.append(document.createElement('li'));"
        "const kids=list.children,items=list.getElementsByTagName('li');"
        "let before=stats.liveCollectionReads,seen=0;"
        "for(let i=0;i<kids.length;i++)if(kids[i])seen++;"
        "for(let i=0;i<items.length;i++)if(items.item(i))seen++;"
        "for(let i=0;i<list.childNodes.length;i++)if(list.childNodes[i])seen++;"
        "if(seen!==150)throw Error('seen '+seen);"
        "const reads=stats.liveCollectionReads-before;"
        "if(reads!==3)throw Error('index loops read '+reads+' times');"
        "if(list.childNodes!==list.childNodes)throw Error('childNodes identity');"
        "const check=(label,want)=>{"
        "if(kids.length!==want||items.length!==want)"
        "throw Error(label+': '+kids.length+'/'+items.length+' want '+want)};"
        "list.append(document.createElement('li'));check('append',51);"
        "list.firstElementChild.remove();check('remove',50);"
        "list.insertAdjacentHTML('beforeend','<li></li><li></li>');check('adjacent',52);"
        "list.lastElementChild.outerHTML='<p></p>';"
        "if(items.length!==51||kids.length!==52)throw Error('outerHTML');"
        "list.innerHTML='<li class=a></li>';check('innerHTML',1);"
        "const byClass=list.getElementsByClassName('a');"
        "if(byClass.length!==1)throw Error('class');"
        "list.firstChild.setAttribute('class','b');"
        "if(byClass.length!==0)throw Error('setAttribute class');"
        "list.firstChild.className='a';if(byClass.length!==1)throw Error('className');"
        "list.firstChild.classList.toggle('a');if(byClass.length)throw Error('toggle');"
        "list.firstChild.id='x';if(kids.namedItem('x')!==list.firstChild)"
        "throw Error('id');"
        "list.textContent='';check('textContent',0);"
        "const text=list.appendChild(document.createTextNode('ab'));"
        "if(list.childNodes.length!==1)throw Error('text append');"
        "text.splitText(1);if(list.childNodes.length!==2)throw Error('splitText');"
        "list.normalize();if(list.childNodes.length!==1)throw Error('normalize');"
        "text.remove();if(list.childNodes.length!==0)throw Error('text remove');"
        "list.replaceChildren(document.createElement('li'));check('replaceChildren',1);"
        "const images=document.images,count=images.length;"
        "document.body.append(document.createElement('img'));"
        "if(images.length!==count+1)throw Error('images');"
        "globalThis.pocSummary='OK'})()");
    return selected_case != NULL && !selected_case_ran ? 2 : ok ? 0 : 1;
}
