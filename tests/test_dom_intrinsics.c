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

static bool run_case(const char *name, const char *source)
{
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
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
    bool zero_owned = script_page_fixture_close(&fixture);
    if (!zero_owned) fprintf(stderr, "%s: nonzero teardown ownership\n", name);
    printf("%s: %s teardown-owned-bytes=%zu\n", name,
           ok ? "PASS" : "FAIL", fixture.budget.current);
    return ok && zero_owned;
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
        "ownerRefs.push(new WeakRef(persisted),new WeakRef(persisted.sheet));})()",
        name, &result);
    if (ok) ok = script_runtime_advance(runtime, 16, 16, &result);
    if (ok) {
        (void) script_runtime_collect_and_trim(runtime);
        ok = script_runtime_evaluate_diagnostic(runtime,
            "(()=>{if(ownerRefs.some(ref=>ref.deref()))throw Error('owner retained');"
            "if(heldSheet.ownerNode.textContent!=='i{color:blue}')"
            "throw Error('held sheet lost owner');globalThis.heldSheet=null;"
            "const node=document.getElementById('persisted'),sheet=node.sheet;"
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

int main(int argc, char **argv)
{
    if (argc > 2) return 2;
    if (argc == 2) selected_case = argv[1];
    bool ok = true;
    ok &= test_weak_shadow_slot_reclamation();
    ok &= test_disconnected_observer_ownership();
    ok &= test_author_sheet_owner_lifetime();
    ok &= test_test_probe_surface();
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
        "if(__tilefinchRootCensus.mutationObservers!==1)throw Error('registration');"
        "const target=document.createElement('div');document.body.appendChild(target);"
        "queueMicrotask(()=>{if(publicCalls!==0||"
        "__tilefinchRootCensus.motionScans<1)throw Error('intrinsic delivery');"
        "globalThis.pocSummary='OK';});})()");
    return selected_case != NULL && !selected_case_ran ? 2 : ok ? 0 : 1;
}
