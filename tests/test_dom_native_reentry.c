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

/* Runs setup, has the host dispatch each of `types` (NULL-terminated) at
   `selector` as user input, then runs check, which sets pocSummary. */
static bool run_host_dispatch_case(const char *name, const char *setup,
                                   const char *selector,
                                   const char *const *types,
                                   const char *check)
{
    if (selected_case != NULL && strcmp(name, selected_case) != 0) return true;
    selected_case_ran = true;
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0))
        return false;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://dom-reentry.test/", &fixture.options, &result);
    bool ok = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, setup, name, &result);
    for (size_t i = 0; ok && types[i] != NULL; i++)
        ok = script_runtime_dispatch(runtime, selector, types[i], &result);
    ok = ok && script_runtime_evaluate_diagnostic(runtime, check, name,
                                                  &result);
    ok = ok && result.success && strcmp(result.summary, "OK") == 0;
    if (!ok)
        fprintf(stderr, "%s: summary=%s error=%s\n", name,
                result.summary, result.error);
    script_runtime_destroy(runtime);
    bool zero_owned = script_page_fixture_close(&fixture);
    if (!zero_owned) fprintf(stderr, "%s: nonzero teardown ownership\n", name);
    printf("%s: %s teardown-owned-bytes=%zu\n", name,
           ok && zero_owned ? "PASS" : "FAIL", fixture.budget.current);
    return ok && zero_owned;
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

/* Fires each event family compat.js builds and marks trusted: focus and
   blur (FocusEvent), dialog cancel and close (Event), popover toggles
   (ToggleEvent), FileReader and XMLHttpRequest progress (ProgressEvent, and
   readystatechange as a plain Event) and MessageChannel delivery
   (MessageEvent). The case captured the bootstrap's interfaces as NE, NF,
   NT, NP and NM before it replaced any global, and defines finish(bad),
   which runs once the three asynchronous families have reported. Every
   event must be trusted, an instance of the bootstrap's interface, and
   shaped as the browser fires it. */
#define BROWSER_EVENTS_FIRE \
    "document.body.insertAdjacentHTML('beforeend','<input id=a><input id=b>" \
    "<dialog id=dl></dialog><div id=pp popover></div>');" \
    "const bad=[],want=(c,m)=>{if(!c)bad.push(m)},$=id=>" \
    "document.getElementById(id),a=$('a'),b=$('b'),dl=$('dl'),pp=$('pp');" \
    "const is=(e,I)=>e.isTrusted===true&&e instanceof I&&e instanceof NE," \
    "rt=e=>e.relatedTarget===document.body?'body':" \
    "e.relatedTarget?e.relatedTarget.id:String(e.relatedTarget);" \
    "const focus=[],focusLog=e=>focus.push([e.type,e.target.id,rt(e)," \
    "e.bubbles,e.cancelable,is(e,NF)].join(':'));" \
    "for(const t of['focus','blur','focusin','focusout'])" \
    "{a.addEventListener(t,focusLog);b.addEventListener(t,focusLog)}" \
    "const plain=[],plainLog=e=>plain.push([e.type,e.bubbles,e.cancelable," \
    "is(e,NE)].join(':'));" \
    "dl.addEventListener('cancel',plainLog);" \
    "dl.addEventListener('close',plainLog);" \
    "const toggles=[],toggleLog=e=>toggles.push([e.type,e.oldState," \
    "e.newState,e.bubbles,e.cancelable,is(e,NT)].join(':'));" \
    "pp.addEventListener('beforetoggle',toggleLog);" \
    "pp.addEventListener('toggle',toggleLog);" \
    "try{a.focus();b.focus();b.blur();dl.show();dl.requestClose();" \
    "pp.showPopover();pp.hidePopover()}" \
    "catch(error){bad.push('sync threw '+error)}" \
    "want(focus.join()==='focus:a:body:false:false:true," \
    "focusin:a:body:true:false:true,blur:a:b:false:false:true," \
    "focusout:a:b:true:false:true,focus:b:a:false:false:true," \
    "focusin:b:a:true:false:true,blur:b:body:false:false:true," \
    "focusout:b:body:true:false:true','focus '+focus);" \
    "want(plain.join()==='cancel:false:true:true,close:false:false:true'," \
    "'dialog '+plain);" \
    "want(toggles.join()==='beforetoggle:closed:open:false:true:true," \
    "toggle:closed:open:false:false:true,beforetoggle:open:closed:false:" \
    "false:true,toggle:open:closed:false:false:true','popover '+toggles);" \
    "let pending=3;const settle=()=>{if(--pending===0)finish(bad)};" \
    "const progress=(e)=>[e.type,e.lengthComputable,e.loaded,e.total," \
    "e.bubbles,e.cancelable,is(e,NP)].join(':');" \
    "const reader=new FileReader,read=[];" \
    "for(const t of['loadstart','progress','load'])" \
    "reader.addEventListener(t,e=>read.push(progress(e)));" \
    "reader.onloadend=e=>{read.push(progress(e));want(read.join()===" \
    "'loadstart:true:0:3:false:false:true,progress:true:3:3:false:false:" \
    "true,load:true:3:3:false:false:true,loadend:true:3:3:false:false:true'," \
    "'reader '+read);settle()};" \
    "reader.readAsText(new Blob(['abc']));" \
    "const url=URL.createObjectURL(new Blob(['xyz'])),xhr=new XMLHttpRequest," \
    "states=[];xhr.onreadystatechange=e=>states.push([e.type,e.bubbles," \
    "is(e,NE),e instanceof NP].join(':'));" \
    "xhr.onloadend=e=>{want(states.length>0&&states.every(s=>s===" \
    "'readystatechange:false:true:false'),'readystate '+states);" \
    "want(progress(e)==='loadend:true:3:3:false:false:true'," \
    "'xhr '+progress(e));settle()};" \
    "xhr.onerror=()=>{bad.push('xhr error');settle()};" \
    "xhr.open('GET',url);xhr.send();" \
    "const channel=new MessageChannel;channel.port2.onmessage=e=>{" \
    "want(e.type==='message'&&e.data==='hi'&&e.ports.length===0" \
    "&&!e.bubbles&&!e.cancelable&&is(e,NM),'message '+[e.type,e.data," \
    "e.isTrusted,e instanceof NM]);settle()};" \
    "channel.port1.postMessage('hi');"

/* Replaces each global interface in `names` with an author constructor
   that counts its calls. With keep, it builds the bootstrap's event and
   shares its prototype, as wrapping polyfills do; without, it returns a
   plain object, as a page's own unrelated Event class would. */
#define BROWSER_EVENTS_REPLACE(keep, names) \
    "globalThis.pocSummary='PENDING';const NE=Event,NF=FocusEvent," \
    "NT=ToggleEvent,NP=ProgressEvent,NM=MessageEvent;let made=0;" \
    "for(const name of " names "){const Native=globalThis[name]," \
    "Author=function(type,init){made++;" \
    "return " keep "?new Native(type,init):{type:String(type)}};" \
    "if(" keep ")Author.prototype=Native.prototype;globalThis[name]=Author}" \
    "let finish=bad=>{if(made)bad.push('author constructor ran '+made);" \
    "globalThis.pocSummary=bad.length?'BAD '+bad.join(' | '):'OK'};"

/* The lazy modules define their Event and EventTarget subclasses on first
   use, after author code has run. Before that first use, the page:
   wrap     replaces both globals with counting constructors that build the
            bootstrap's own object and share its prototype (polyfills);
   replace  replaces both with unrelated constructors returning plain
            objects;
   proto    keeps the globals but rewrites the interfaces' prototypes:
            `constructor` becomes a counting function and a page mark is
            added (instances must inherit the mark, as in browsers);
   none     leaves everything alone.
   The family body then triggers the module and checks its classes against
   NE and NET, the interfaces captured here before any change (and that
   they still print as native code). The page's
   constructors must never run, and every instance must be genuine: the
   bootstrap's brand check accepts its events, its targets dispatch.
   `pending` counts the outstanding asynchronous checks, one of them the
   synchronous body; settle() ends one. */
static const char lazy_events_prelude[] =
    "(()=>{globalThis.pocSummary='PENDING';"
    "const NE=Event,NET=EventTarget,isEv=__tilefinchIsEvent,bad=[],"
    "want=(c,m)=>{if(!c)bad.push(m)},mode=%s;let made=0,pending=1;"
    "const finish=()=>{if(made)bad.push('author constructor ran '+made);"
    "pocSummary=bad.length?'BAD '+bad.join(' | '):'OK'},"
    "settle=()=>{if(--pending===0)finish()},"
    "inherits=(C,B)=>{for(let at=C,n=0;at&&n<8;"
    "at=Object.getPrototypeOf(at),n++)if(at===B)return true;return false},"
    "parent=(C,B)=>{want(typeof C==='function'&&Object.getPrototypeOf(C)===B"
    "&&Object.getPrototypeOf(C.prototype)===B.prototype,"
    "(C&&C.name)+' is not directly under '+B.name);"
    "want(Function.prototype.toString.call(C)==='function '+C.name+"
    "'() {\\n    [native code]\\n}',C.name+' source shows')},"
    "marked=o=>mode!=='proto'||o.tfPageMark===1,"
    "genuineTarget=(t,label)=>{want(t instanceof NET&&marked(t),"
    "label+' is not an EventTarget');let got=0;const f=()=>got++;"
    "try{t.addEventListener('tf-ping',f);t.dispatchEvent(new NE('tf-ping'));"
    "t.removeEventListener('tf-ping',f)}"
    "catch(error){bad.push(label+' dispatch threw '+error)}"
    "want(got===1,label+' dispatch reached '+got)},"
    "genuineEvent=(e,I,label)=>want(isEv(e)&&e instanceof I&&e instanceof NE"
    "&&marked(e),label+' is not a genuine event');"
    "if(mode==='wrap'||mode==='replace')"
    "for(const[name,Native]of[['Event',NE],['EventTarget',NET]]){"
    "const Author=function(...args){made++;return mode==='wrap'?"
    "Reflect.construct(Native,args,new.target||Native):"
    "{type:String(args[0])}};"
    "if(mode==='wrap')Author.prototype=Native.prototype;"
    "globalThis[name]=Author}"
    "else if(mode==='proto')for(const Native of[NE,NET]){"
    "Native.prototype.constructor=function Author(){made++;return{}};"
    "Object.defineProperty(Native.prototype,'tfPageMark',"
    "{value:1,configurable:true})}"
    "try{%s}catch(error){bad.push('threw '+error)}settle()})()";

static const struct {
    const char *name;
    const char *body;
} lazy_event_families[] = {
    {"motion",
     "const AE=AnimationEvent;parent(AE,NE);"
     "const e=new AE('animationstart',{animationName:'spin',elapsedTime:2});"
     "genuineEvent(e,AE,'AnimationEvent');"
     "want(e.animationName==='spin'&&e.elapsedTime===2,'AnimationEvent init');"
     "const el=document.createElement('i');document.body.appendChild(el);"
     "let got=0;el.addEventListener('animationstart',x=>{got+=x===e});"
     "el.dispatchEvent(e);want(got===1,'AnimationEvent dispatch '+got);"},
    {"speech",
     "const U=SpeechSynthesisUtterance,SE=SpeechSynthesisEvent,"
     "SEE=SpeechSynthesisErrorEvent;"
     "parent(U,NET);parent(SpeechSynthesis,NET);parent(SE,NE);parent(SEE,SE);"
     "const u=new U('hi');genuineTarget(u,'utterance');"
     "genuineTarget(speechSynthesis,'speechSynthesis');"
     "genuineEvent(new SE('boundary',{utterance:u,charIndex:1}),SE,"
     "'SpeechSynthesisEvent');"
     "pending++;u.addEventListener('error',e=>{"
     "genuineEvent(e,SEE,'speech error');want(e.utterance===u&&"
     "e.error==='synthesis-unavailable','speech error init');settle()});"
     "speechSynthesis.speak(u);"},
    {"media-source",
     "for(const C of[MediaSource,SourceBuffer,SourceBufferList])"
     "parent(C,NET);"
     "const source=new MediaSource;genuineTarget(source,'MediaSource');"
     "want(source.sourceBuffers instanceof SourceBufferList,"
     "'sourceBuffers');genuineTarget(source.sourceBuffers,'sourceBuffers');"},
    {"font-face-set",
     "const fonts=document.fonts;parent(FontFaceSet,NET);"
     "want(fonts instanceof FontFaceSet,'document.fonts');"
     "genuineTarget(fonts,'document.fonts');"
     "pending++;fonts.ready.then(f=>{want(f===fonts,'fonts.ready');settle()},"
     "error=>{bad.push('fonts.ready '+error);settle()});"},
    {"media-capture",
     "for(const C of[MediaStream,MediaStreamTrack,MediaDevices])"
     "parent(C,NET);parent(MediaStreamTrackEvent,NE);"
     "const stream=new MediaStream;genuineTarget(stream,'MediaStream');"
     "want(stream.getTracks().length===0,'empty stream');"
     "genuineTarget(navigator.mediaDevices,'mediaDevices');"
     "let threw=false;try{new MediaStreamTrackEvent('addtrack',{track:{}})}"
     "catch(error){threw=error instanceof TypeError}"
     "want(threw,'MediaStreamTrackEvent took a posing track');"},
    {"indexeddb",
     "for(const C of[IDBRequest,IDBOpenDBRequest,IDBTransaction,"
     "IDBDatabase])want(inherits(C,NET),C.name+' is not an EventTarget');"
     "pending++;const request=indexedDB.open('tf-lazy-events',1);"
     "genuineTarget(request,'IDBOpenDBRequest');let upgraded=false;"
     "request.onupgradeneeded=e=>{upgraded=true;"
     "genuineEvent(e,NE,'upgradeneeded');"
     "request.result.createObjectStore('s')};"
     "request.onsuccess=e=>{genuineEvent(e,NE,'success');"
     "want(upgraded&&e.target===request,'open events');"
     "const db=request.result;genuineTarget(db,'IDBDatabase');db.close();"
     "settle()};"
     "request.onerror=()=>{bad.push('open failed');settle()};"},
    {"game-audio",
     "const context=new AudioContext;"
     "for(const C of[BaseAudioContext,AudioContext,GainNode,"
     "AudioBufferSourceNode])want(inherits(C,NET),"
     "C.name+' is not an EventTarget');"
     "parent(BaseAudioContext,NET);"
     "genuineTarget(context,'AudioContext');"
     "genuineTarget(context.createGain(),'GainNode');"},
    /* The worker realm defines PromiseRejectionEvent, Worker and its
       global scope classes from the owner's interfaces when a Worker is
       constructed: it must take the captured ones too. */
    {"worker",
     "parent(Worker,NET);"
     "const source=\"const P=PromiseRejectionEvent,out=[typeof P,"
     "Object.getPrototypeOf(P).name,typeof self.addEventListener,"
     "typeof performance.addEventListener];let hit=0;"
     "self.addEventListener('tf-direct',()=>hit++);"
     "const direct=new P('tf-direct',{promise:Promise.resolve(),reason:1});"
     "self.dispatchEvent(direct);out.push(hit,direct.reason,"
     "direct instanceof P);"
     "self.addEventListener('unhandledrejection',e=>postMessage(["
     "e.type,e.reason,e instanceof P,e.cancelable,"
     "e.promise instanceof Promise].join(':')));"
     "postMessage(out.join(':'));Promise.reject(5);\";"
     "pending++;const worker=new Worker(URL.createObjectURL(new Blob("
     "[source],{type:'text/javascript'})));genuineTarget(worker,'Worker');"
     "const seen=[];worker.onmessage=e=>{"
     "genuineEvent(e,MessageEvent,'worker message');seen.push(e.data);"
     "if(seen.length<2)return;want(seen.join()==='function:Event:function:"
     "function:1:1:true,unhandledrejection:5:true:true:true',"
     "'worker saw '+seen);worker.terminate();settle()};"
     "worker.onerror=e=>{bad.push('worker error '+(e&&e.message));"
     "worker.terminate();settle()};"},
};

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
    /* The browser builds the events it marks trusted with the bootstrap's
       interfaces: a page that replaces the globals never has its
       constructor called with browser-internal arguments, and never gets
       its own object dispatched as a trusted event. */
    ok &= run_case("browser-events-ignore-wrapped-constructors",
        "(()=>{" BROWSER_EVENTS_REPLACE("true",
            "['Event','FocusEvent','ToggleEvent','ProgressEvent',"
            "'MessageEvent']")
        BROWSER_EVENTS_FIRE "})()");
    ok &= run_case("browser-events-ignore-replaced-constructors",
        "(()=>{" BROWSER_EVENTS_REPLACE("false",
            "['FocusEvent','ToggleEvent','ProgressEvent','MessageEvent']")
        BROWSER_EVENTS_FIRE "})()");
    /* The init dictionaries those events are built from have no
       prototype: an accessor the page put on Object.prototype never runs
       while the browser builds one, and cannot choose bubbles, cancelable
       or a family field. Unarmed, the accessors behave as plain
       properties so the page's own code is unaffected. */
    ok &= run_case("browser-event-init-ignores-object-prototype",
        "(()=>{" BROWSER_EVENTS_REPLACE("true", "[]")
        "let fired='',armed=false;"
        "for(const k of['bubbles','cancelable','composed','lengthComputable',"
        "'loaded','total','data','origin','lastEventId','source','ports'])"
        "Object.defineProperty(Object.prototype,k,{__proto__:null,"
        "configurable:true,get(){if(armed)fired+=k+' ';return undefined},"
        "set(v){if(armed)fired+=k+'= ';Object.defineProperty(this,k,"
        "{__proto__:null,value:v,writable:true,enumerable:true,"
        "configurable:true})}});"
        "const done=finish;finish=bad=>{armed=false;"
        "if(fired)bad.push('page accessors ran: '+fired.trim());done(bad)};"
        "armed=true;" BROWSER_EVENTS_FIRE "})()");
    /* Dispatch recognises an event by the bootstrap's own brand, not by
       instanceof against the global: after a page replaces Event with an
       unrelated constructor, its own dispatches of real events and the
       browser's events (a worker's message included) still reach their
       listeners, at the target phase. */
    ok &= run_case("events-dispatch-after-unrelated-event-global",
        "(()=>{" BROWSER_EVENTS_REPLACE("false",
            "['Event','FocusEvent','ToggleEvent','ProgressEvent',"
            "'MessageEvent']")
        "document.body.insertAdjacentHTML('beforeend','<i id=pg></i>');"
        "const pageLog=[],pageTargets=[['pg',document.getElementById('pg')],"
        "['doc',document],['win',window]];"
        "for(const[name,t]of pageTargets)t.addEventListener('ping',e=>"
        "pageLog.push(name+':'+e.eventPhase+':'+(e.target===t)+':'"
        "+e.isTrusted));"
        "let pageError='';try{for(const[,t]of pageTargets)"
        "t.dispatchEvent(new NE('ping'))}catch(error){pageError=String(error)}"
        BROWSER_EVENTS_FIRE
        "want(!pageError&&pageLog.join()==='pg:2:true:false,doc:2:true:false,"
        "win:2:true:false','page dispatch '+pageError+' '+pageLog);"
        "pending++;const worker=new Worker(URL.createObjectURL(new Blob(["
        "'postMessage(7)'],{type:'text/javascript'})));"
        "worker.onmessage=e=>{want(e.data===7&&is(e,NM)&&e.eventPhase===2,"
        "'worker '+[e.data,e.isTrusted,e instanceof NM,e.eventPhase]);"
        "worker.terminate();settle()};"
        "worker.onerror=()=>{bad.push('worker error');settle()};"
        "})()");
    /* Objects that only pose as events stay refused, as before: neither a
       page's Symbol.hasInstance, a borrowed prototype chain nor a replaced
       global Event gets one dispatched. */
    ok &= run_case("posing-events-stay-refused",
        "(()=>{" REENTRY_BODY
        "const NE=Event,log=[],el=document.getElementById('d');"
        "for(const t of[el,document,window])"
        "t.addEventListener('ping',()=>log.push('ran'));"
        "const refuse=(label,make)=>{for(const t of[el,document,window]){"
        "let threw=false;try{t.dispatchEvent(make())}catch(error){threw="
        "error instanceof TypeError||error instanceof DOMException}"
        "check(threw,label+' accepted')}};"
        "refuse('plain object',()=>({type:'ping'}));"
        "refuse('borrowed prototype',()=>Object.create(NE.prototype));"
        "const Pose=function(){this.type='ping'};Pose.prototype=NE.prototype;"
        "refuse('constructed on the prototype',()=>new Pose);"
        "Object.defineProperty(NE,Symbol.hasInstance,{configurable:true,"
        "value:()=>true});"
        "refuse('hasInstance on Event',()=>({type:'ping'}));"
        "delete NE[Symbol.hasInstance];"
        "const Page=function(type){this.type=type};"
        "Object.defineProperty(Page,Symbol.hasInstance,{value:()=>true});"
        "globalThis.Event=Page;"
        "refuse('replaced Event',()=>new Page('ping'));"
        "refuse('replaced Event prototype',()=>Object.create(Page.prototype));"
        "check(log.length===0,'posing event reached a listener');"
        "el.dispatchEvent(new NE('ping'));"
        "check(log.length===1,'real event refused');"
        "globalThis.pocSummary='OK'})()");
    /* Event and EventTarget subclasses defined by lazily loaded modules
       stay under the bootstrap's interfaces whatever the page did to the
       globals before the module loaded (lazy_events_prelude). */
    {
        static const char *const modes[] = {"wrap", "replace", "proto",
                                            "none"};
        for (size_t family = 0;
             family < sizeof(lazy_event_families) /
                          sizeof(lazy_event_families[0]);
             family++)
            for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]);
                 mode++) {
                char name[64], quoted[16], source[8192];
                snprintf(name, sizeof(name), "lazy-events-%s-%s",
                         lazy_event_families[family].name, modes[mode]);
                snprintf(quoted, sizeof(quoted), "'%s'", modes[mode]);
                int length = snprintf(source, sizeof(source),
                                      lazy_events_prelude, quoted,
                                      lazy_event_families[family].body);
                if (length < 0 || (size_t) length >= sizeof(source)) {
                    fprintf(stderr, "%s: source too long\n", name);
                    ok = false;
                    continue;
                }
                ok &= run_case(name, source);
            }
    }
    /* What hardening.js publishes for the lazy modules to extend: a frozen,
       null-prototype table whose every entry is the same interface object
       the page already sees at the global of that name (so it grants
       nothing the page lacks), in a non-writable, non-configurable,
       non-enumerable global hidden from reflection like the others. */
    ok &= run_case("lazy-event-interfaces-publication",
        "(()=>{const check=(c,m)=>{if(!c)throw new Error(m)},"
        "table=globalThis.__tilefinchEventInterfaces,"
        "d=Object.getOwnPropertyDescriptor(globalThis,"
        "'__tilefinchEventInterfaces');"
        "check(d&&!d.writable&&!d.enumerable&&!d.configurable,'descriptor');"
        "check(Object.isFrozen(table)&&Object.getPrototypeOf(table)===null,"
        "'table is not frozen and prototype-free');"
        "const keys=Reflect.ownKeys(table);"
        "check(keys.includes('Event')&&keys.includes('EventTarget'),"
        "'keys '+keys);"
        "for(const key of keys)check(typeof table[key]==='function'&&"
        "table[key]===globalThis[key],key+' is not the public interface');"
        "check(!Object.getOwnPropertyNames(globalThis).includes("
        "'__tilefinchEventInterfaces'),'visible to reflection');"
        "const NE=Event;globalThis.Event=function(){};"
        "globalThis.__tilefinchEventInterfaces=null;"
        "check(globalThis.__tilefinchEventInterfaces===table&&"
        "table.Event===NE,'table changed');"
        "globalThis.pocSummary='OK'})()");
    /* User input the host dispatches is built the same way: a page that
       wraps or replaces the input event interfaces never builds the
       trusted click, key, mouse, input, composition, submit or change. */
    {
        static const char *const input_types[] = {
            "click", "keydown", "mousedown", "input", "compositionstart",
            "submit", "change", NULL,
        };
        static const char host_input_check[] =
            "(()=>{const want=['click:PointerEvent:true:true:true',"
            "'keydown:KeyboardEvent:true:true:true',"
            "'mousedown:MouseEvent:true:true:true',"
            "'input:InputEvent:true:false:true',"
            "'compositionstart:CompositionEvent:true:false:false',"
            "'submit:SubmitEvent:true:true:false',"
            "'change:Event:true:false:false'].join();"
            "pocSummary=made===0&&seen.join()===want?'OK':"
            "'BAD made='+made+' '+seen})()";
#define HOST_INPUT_SETUP(keep, names) \
        "(()=>{document.body.insertAdjacentHTML('beforeend'," \
        "'<form id=t><input></form>');globalThis.made=0;" \
        "const natives={};for(const name of['Event','PointerEvent'," \
        "'KeyboardEvent','MouseEvent','InputEvent','CompositionEvent'," \
        "'SubmitEvent'])natives[name]=globalThis[name];" \
        "for(const name of " names "){const Native=globalThis[name]," \
        "Author=function(type,init){made++;" \
        "return " keep "?new Native(type,init):{type:String(type)}};" \
        "if(" keep ")Author.prototype=Native.prototype;" \
        "globalThis[name]=Author}" \
        "globalThis.seen=[];const t=document.getElementById('t');" \
        "for(const[type,name]of[['click','PointerEvent'],['keydown'," \
        "'KeyboardEvent'],['mousedown','MouseEvent'],['input','InputEvent']," \
        "['compositionstart','CompositionEvent'],['submit','SubmitEvent']," \
        "['change','Event']])t.addEventListener(type,e=>{seen.push([e.type," \
        "e instanceof natives[name]&&e instanceof natives.Event?name:'?'," \
        "e.isTrusted,e.cancelable,e.composed].join(':'));" \
        "if(type==='submit')e.preventDefault()});pocSummary='OK'})()"
        ok &= run_host_dispatch_case("host-input-ignores-wrapped-constructors",
            HOST_INPUT_SETUP("true",
                "['Event','PointerEvent','KeyboardEvent','MouseEvent',"
                "'InputEvent','CompositionEvent','SubmitEvent']"),
            "#t", input_types, host_input_check);
        ok &= run_host_dispatch_case("host-input-ignores-replaced-constructors",
            HOST_INPUT_SETUP("false",
                "['PointerEvent','KeyboardEvent','MouseEvent','InputEvent',"
                "'CompositionEvent','SubmitEvent']"),
            "#t", input_types, host_input_check);
#undef HOST_INPUT_SETUP
    }
    /* isTrusted is defined with the bootstrap's own defineProperty, so a
       page that replaces Object.defineProperty cannot define it as a
       plain true value on the events it constructs. */
    ok &= run_case("event-is-trusted-ignores-replaced-define-property",
        "(()=>{" REENTRY_BODY
        "const define=Object.defineProperty;let asked=0;"
        "Object.defineProperty=function(o,k,d){if(k==='isTrusted'){asked++;"
        "d={value:true,enumerable:true}}return define(o,k,d)};"
        "let e;try{e=new Event('x')}finally{Object.defineProperty=define}"
        "check(e.isTrusted===false&&asked===0,"
        "'isTrusted forged: '+[e.isTrusted,asked]);"
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
