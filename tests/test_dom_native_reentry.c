#include <stdio.h>
#include <string.h>

#include "tilefinch/style.h"
#include "script_test_support.h"

/* Page code must never run in the middle of a DOM native while the native
   holds node pointers, and when author code does run inside one (argument
   coercion), the nodes the native resolved stay valid until it returns.
   Each case is a fresh realm whose body is built by innerHTML, so the
   parsed nodes have handle slots without wrappers: the case in which a
   removal used to destroy a node at once. Every case ends with the
   ownership teardown check. */
static const char *selected_case;
static bool selected_case_ran;
/* Nonzero: the case's handle-table ceiling, so the table can grow. */
static size_t case_handle_slots;

static bool run_page_case(const char *name, const char *source, bool styled)
{
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return false;
    if (case_handle_slots != 0)
        fixture.options.execution_policy.dom_handle_slot_capacity =
            case_handle_slots;
    Stylesheet sheet = {0};
    if (styled && !stylesheet_build(&sheet, &fixture.budget, &fixture.document,
                                    480))
        return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-reentry.test/", &fixture.options, &result);
    if (styled && runtime != NULL) script_runtime_set_stylesheet(runtime, &sheet);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, source, name, &result);
    for (unsigned turn = 0; ok && turn < 16; turn++)
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
           ok && zero_owned ? "PASS" : "FAIL", fixture.budget.current);
    return ok && zero_owned;
}

static bool run_case(const char *name, const char *source)
{
    return run_page_case(name, source, false);
}

/* #g > #p > three <i>, #other, #d; parsed by innerHTML, so no wrappers. */
#define REENTRY_BODY \
    "document.body.innerHTML='<div id=g><div id=p><i title=a></i>" \
    "<i title=b></i><i title=c></i></div></div><div id=other>" \
    "<x-a id=xa></x-a></div><span id=d title=orig data-k=v>t</span>';" \
    "const check=(c,m)=>{if(!c)throw Error(m)};"

/* An accessor on Object.prototype (named keys) and Array.prototype
   (indices) that records a [[Set]] while `armed` and then drops the value,
   so a native that sets instead of defining both runs page code and loses
   the field. Unarmed, it defines the own property, as bootstrap code that
   assigns to fresh objects expects. Descriptors are null-prototype: an
   inherited `value` would turn an accessor descriptor into an invalid one. */
#define REENTRY_TRAPS \
    "let fired='';let armed=false;" \
    "const bare=o=>Object.setPrototypeOf(o,null);" \
    "const trap=(o,k)=>Object.defineProperty(o,k,bare({configurable:true," \
    "get(){return undefined},set(v){if(armed){fired+=String(k)+' ';return}" \
    "Object.defineProperty(this,k,bare({value:v,writable:true," \
    "enumerable:true,configurable:true}))}}));" \
    "const untrap=()=>{for(let i=0;i<8;i++)delete Array.prototype[i];" \
    "for(const k of keys)delete Object.prototype[k]};" \
    "const keys=['name','value','text','nodes','bytes','x','y','width'," \
    "'height','retained','authoritative','clientWidth','scrollTop'];" \
    "for(let i=0;i<8;i++)trap(Array.prototype,i);" \
    "for(const k of keys)trap(Object.prototype,k);" \
    "const bad=[],soft=(c,m)=>{if(!c)bad.push(m)};" \
    "const call=(label,f)=>{armed=true;let r;try{r=f()}finally{armed=false}" \
    "if(fired){bad.push(label+' ran page accessors '+fired.trim());fired=''}" \
    "return r};" \
    "const own=(o,k)=>Object.prototype.hasOwnProperty.call(o,k);"

int main(int argc, char **argv)
{
    if (argc > 2) return 2;
    if (argc == 2) selected_case = argv[1];
    bool ok = true;
    /* NB-03: result lists and records are defined, never [[Set]]. */
    ok &= run_page_case("fresh-results-are-defined",
        "(()=>{" REENTRY_BODY REENTRY_TRAPS
        "try{const p=__tilefinchQuery('#p'),d=__tilefinchQuery('#d'),"
        "other=__tilefinchQuery('#other');"
        "const titles=l=>{const t=[];for(let i=0;i<l.length;i++)"
        "t.push(own(l,i)?__tilefinchGetAttribute(l[i],'title'):'hole');"
        "return t.join()};"
        "soft(titles(call('children',()=>__tilefinchChildren(p)))==='a,b,c',"
        "'children');"
        "soft(titles(call('childNodes',()=>__tilefinchChildNodes(p)))"
        "==='a,b,c','childNodes');"
        "const top=call('documentChildNodes',()=>__tilefinchDocumentChildNodes());"
        "soft(top.length>0&&own(top,0),'documentChildNodes');"
        "soft(titles(call('queryAll',()=>__tilefinchQueryAll('i')))==='a,b,c',"
        "'queryAll');"
        /* Inclusive of the root, which has no title. */
        "soft(titles(call('descendants',()=>__tilefinchDescendants(p,1)))"
        "===',a,b,c','descendants');"
        "const attrs=call('attributes',()=>__tilefinchAttributes(d));"
        "soft(attrs.length===3&&attrs.every(a=>own(a,'name')&&own(a,'value'))"
        "&&attrs.map(a=>a.name+'='+a.value).join()==='id=d,title=orig,data-k=v',"
        "'attributes');"
        "const id=call('identity',()=>__tilefinchNodeIdentity(d,[],true));"
        "soft(own(id,0)&&id[0]==='SPAN'&&id[1]===1&&own(id,4),'identity');"
        "const prefix=call('textPrefix',()=>__tilefinchGetTextPrefix(d,64,8));"
        "soft(own(prefix,'text')&&prefix.text==='t'&&own(prefix,'nodes'),"
        "'text prefix');"
        "const box=call('geometry',()=>__tilefinchGeometry(d));"
        "soft(own(box,'x')&&own(box,'width')&&own(box,'scrollTop'),'geometry');"
        "const names=call('styleSupport',()=>__tilefinchComputedStyleSupport());"
        "soft(names.length>8&&own(names,0),'style support');"
        "const ids=call('namedIds',()=>__tilefinchNamedElementIds());"
        "soft(ids.includes('g')&&ids.includes('p')&&own(ids,0),'named ids');"
        "const found=call('transitions',()=>__tilefinchTransitionDiscover(p,8));"
        "soft(own(found,0)&&typeof found[0]==='number','transition discover');"
        "const custom=call('candidates',"
        "()=>__tilefinchCustomElementCandidates(other));"
        "soft(custom.length===1&&own(custom,0),'custom candidates');"
        "}finally{untrap()}"
        "check(!bad.length,bad.join('; '));"
        "globalThis.pocSummary='OK'})()", true);
    /* NB-03, public API only (the reviewer's repro): a page setter on
       Array.prototype[0] that empties the parent and reuses the freed
       records in another parent must not make childNodes follow a freed
       sibling link into that other tree. */
    ok &= run_case("child-nodes-index-setter",
        "(()=>{" REENTRY_BODY
        "const parent=document.getElementById('p'),"
        "other=document.getElementById('other');let armed=true;"
        "Object.defineProperty(Array.prototype,0,{configurable:true,"
        "get(){return undefined},set(value){Object.defineProperty(this,0,"
        "{value,writable:true,configurable:true,enumerable:true});"
        "if(!armed)return;armed=false;parent.textContent='';"
        "for(let i=0;i<8;i++){const fresh=document.createElement('i');"
        "fresh.setAttribute('title','other'+i);other.appendChild(fresh)}}});"
        "let kids;try{kids=Array.from(parent.childNodes)}"
        "finally{delete Array.prototype[0]}"
        "for(const node of kids)"
        "check(node&&node.parentNode!==other&&!/^other/.test("
        "node.getAttribute('title')),'foreign node in childNodes: '"
        "+(node&&node.getAttribute('title')));"
        "globalThis.pocSummary='OK'})()");
    ok &= run_case("children-index-setter-native",
        "(()=>{" REENTRY_BODY
        "const parent=document.getElementById('p'),"
        "other=document.getElementById('other');let fired=0;"
        "Object.defineProperty(Array.prototype,0,{configurable:true,"
        "get(){return undefined},set(value){Object.defineProperty(this,0,"
        "{value,writable:true,configurable:true,enumerable:true});"
        "if(fired++)return;parent.textContent='';"
        "for(let i=0;i<8;i++){const fresh=document.createElement('i');"
        "fresh.setAttribute('title','other'+i);other.appendChild(fresh)}}});"
        "let kids;try{kids=__tilefinchChildren(parent.__handle)}"
        "finally{delete Array.prototype[0]}"
        "check(fired===0,'Array.prototype setter ran inside the native');"
        "check(kids.length===3,'children '+kids.length);"
        "globalThis.pocSummary='OK'})()");
    /* NB-05, JR-24: the node-retirement hook and the cleanups it runs are
       bootstrap code inside a native discard. With Number, Map.prototype
       .delete, the array iterator and the typed-array length getter all
       replaced, retiring a handle must call none of them. The reviewer's
       repro: a page Number() that destroyed the receiver and reused its
       record, so the native appended its text through a stale pointer. */
    ok &= run_case("retire-hook-uses-captured-intrinsics",
        "(()=>{" REENTRY_BODY
        "const grand=__tilefinchQuery('#g'),parent=__tilefinchQuery('#p');"
        "__tilefinchQuery('i');"
        "document.createElement('x-host').attachShadow({mode:'open'});"
        "const RealNumber=Number,realDelete=Map.prototype.delete,"
        "realIterator=Array.prototype[Symbol.iterator],"
        "typed=Object.getPrototypeOf(Float64Array.prototype),"
        "realLength=Object.getOwnPropertyDescriptor(typed,'length');"
        "let calls='',fresh=0;"
        "globalThis.Number=function(v){if(!calls){"
        "__tilefinchSetText(grand,'');fresh=__tilefinchCreate('div')}"
        "calls+='Number ';return RealNumber(v)};"
        "Map.prototype.delete=function(k){calls+='delete ';"
        "return realDelete.call(this,k)};"
        "Array.prototype[Symbol.iterator]=function(){calls+='iterator ';"
        "return realIterator.call(this)};"
        "Object.defineProperty(typed,'length',{configurable:true,"
        "get(){calls+='length ';return realLength.get.call(this)}});"
        "let result;try{result=__tilefinchSetText(parent,'WRITTEN')}finally{"
        "globalThis.Number=RealNumber;Map.prototype.delete=realDelete;"
        "Array.prototype[Symbol.iterator]=realIterator;"
        "Object.defineProperty(typed,'length',realLength)}"
        "check(calls==='','page code ran in node retirement: '+calls);"
        "check(result===true&&__tilefinchGetText(parent)==='WRITTEN',"
        "'text not written to the receiver');"
        "check(!fresh||__tilefinchGetText(fresh)==='',"
        "'text written through a stale receiver');"
        "globalThis.pocSummary='OK'})()");
    /* NB-02: a native keeps the node it resolved alive while page code it
       calls (here a toString) empties the node's parent and allocates a
       fresh element; the handle retires at once, the node only when the
       native returns. The reviewer's repro read the fresh node's
       attribute through the stale pointer and wrote to it. */
    ok &= run_case("coercion-keeps-receiver-alive",
        "(()=>{document.body.innerHTML="
        "'<div id=p><span id=d title=orig></span></div>';"
        "const check=(c,m)=>{if(!c)throw Error(m)};"
        "const p=__tilefinchQuery('#p'),d=__tilefinchQuery('#d');"
        "check(__tilefinchGetAttribute(d,'title')==='orig','setup');"
        "let fresh=0;"
        "const read=__tilefinchGetAttribute(d,{toString(){"
        "__tilefinchSetText(p,'');fresh=__tilefinchCreate('span');"
        "__tilefinchSetAttribute(fresh,'title','FRESH-NODE');return 'title'}});"
        "check(read==='orig','read through a destroyed receiver: '+read);"
        "check(__tilefinchGetAttribute(d,'title')===null,"
        "'the removed receiver kept its handle');"
        "__tilefinchSetInnerHTML(p,'<span id=e title=orig2></span>');"
        "const e=__tilefinchQuery('#e');let fresh2=0;"
        "const wrote=__tilefinchSetAttribute(e,{toString(){"
        "__tilefinchSetText(p,'');fresh2=__tilefinchCreate('span');"
        "return 'data-stale'}},'WRITTEN');"
        "check(wrote===true,'write refused');"
        "check(__tilefinchGetAttribute(fresh2,'data-stale')===null,"
        "'write landed on a fresh node');"
        "check(__tilefinchGetAttribute(e,'data-stale')===null,"
        "'the removed receiver kept its handle after a write');"
        "globalThis.pocSummary='OK'})()");
    /* NB-02 with NB-05's receiver: SetText's text coercion removes the
       receiver itself; the text goes into the receiver, never elsewhere. */
    ok &= run_case("set-text-coercion-removes-receiver",
        "(()=>{" REENTRY_BODY
        "const g=__tilefinchQuery('#g'),p=__tilefinchQuery('#p');let fresh=0;"
        "const result=__tilefinchSetText(p,{toString(){"
        "__tilefinchSetText(g,'');fresh=__tilefinchCreate('div');"
        "return 'WRITTEN'}});"
        "check(result===true,'set text refused');"
        "check(__tilefinchGetText(fresh)==='','text written through a stale "
        "receiver: '+__tilefinchGetText(fresh));"
        "check(__tilefinchGetText(g)==='','grandparent kept children');"
        "globalThis.pocSummary='OK'})()");
    /* A node removed while the native held it, and inserted again by that
       native, stays in the document; the drain leaves it alone. */
    ok &= run_case("reinserted-deferred-root-survives",
        "(()=>{" REENTRY_BODY
        "const body=__tilefinchQuery('body'),p=__tilefinchQuery('#p'),"
        "d=__tilefinchQuery('#d');"
        "const moved=__tilefinchQuery('i');"
        "__tilefinchInsertBefore(body,moved,{valueOf(){"
        "__tilefinchSetText(p,'');return d}});"
        "const again=__tilefinchQuery('i[title=a]');"
        "check(again!==0,'reinserted node destroyed');"
        "check(__tilefinchGetAttribute(again,'title')==='a','reinserted title');"
        "check(__tilefinchQueryAll('i').length===1,'other children kept');"
        "globalThis.pocSummary='OK'})()");
    /* The native's own exception survives the deferred destroy, and the
       subtree is gone afterwards. */
    ok &= run_case("deferred-destroy-keeps-exception",
        "(()=>{" REENTRY_BODY
        "const g=__tilefinchQuery('#g'),p=__tilefinchQuery('#p');let caught='';"
        "try{__tilefinchGetAttribute(p,{toString(){"
        "__tilefinchSetText(g,'');throw Error('mine')}})}"
        "catch(error){caught=error.message}"
        "check(caught==='mine','exception lost: '+caught);"
        "check(__tilefinchGetAttribute(p,'id')===null,'subtree kept');"
        "check(__tilefinchQueryAll('i').length===0,'children kept');"
        "globalThis.pocSummary='OK'})()");
    /* JR-14: the trusted-event marker is not reachable from the page, so
       isTrusted cannot be forged; the browser's own events stay trusted. */
    ok &= run_case("trusted-event-marker-unreachable",
        "(()=>{" REENTRY_BODY
        "check(typeof __tilefinchTrustedEvent==='undefined',"
        "'marker callable: '+typeof __tilefinchTrustedEvent);"
        "check(!('__tilefinchTrustedEventShare' in globalThis),'share left');"
        "check(new Event('x').isTrusted===false,'constructed event trusted');"
        "document.body.insertAdjacentHTML('beforeend',"
        "'<form id=f><input id=i></form>');"
        "const form=document.getElementById('f'),"
        "input=document.getElementById('i');let submit,focus;"
        "form.addEventListener('submit',e=>{submit=e.isTrusted;"
        "e.preventDefault()});"
        "input.addEventListener('focus',e=>{focus=e.isTrusted});"
        "form.requestSubmit();input.focus();"
        "check(submit===true,'requestSubmit event untrusted: '+submit);"
        "check(focus===true,'focus event untrusted: '+focus);"
        "globalThis.pocSummary='OK'})()");
    /* NB-06: NodeOwner coerces its tag before it reads the tag table, which
       valueOf can reallocate by growing the handle table. */
    case_handle_slots = SCRIPT_DOM_HANDLE_SLOT_CAPACITY_REALISTIC;
    ok &= run_case("node-owner-tag-after-table-growth",
        "(()=>{" REENTRY_BODY
        "const d=__tilefinchQuery('#d');"
        "check(__tilefinchNodeOwner(d,3)===true,'first tag');"
        "let made=0;"
        "const set=__tilefinchNodeOwner(d,{valueOf(){"
        "for(let i=0;i<9000;i++)if(__tilefinchCreate('i'))made++;"
        "return 200}});"
        "check(made===9000,'created '+made);"
        "check(set===true,'tag refused');"
        "check(__tilefinchNodeOwner(d)===200,"
        "'tag lost: '+__tilefinchNodeOwner(d));"
        "globalThis.pocSummary='OK'})()");
    case_handle_slots = 0;
    if (selected_case != NULL && !selected_case_ran) {
        fprintf(stderr, "unknown case: %s\n", selected_case);
        return 2;
    }
    return ok ? 0 : 1;
}
