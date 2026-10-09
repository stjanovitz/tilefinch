#include <stdio.h>
#include <string.h>

#include "script_test_support.h"

/* Generic DOM contracts, not captured page scripts. Each case has a fresh
   realm so public constructor/prototype replacements cannot leak between it
   and another case; even failures finish the ownership teardown check. */
static bool run_case(const char *name, const char *source)
{
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-intrinsics.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    for (unsigned turn = 0; ok && turn < 4; turn++)
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

int main(void)
{
    bool ok = true;
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
    return ok ? 0 : 1;
}
