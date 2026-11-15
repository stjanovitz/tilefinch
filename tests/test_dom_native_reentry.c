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
/* Nonzero: after its input, the host scrolls the page here and runs a few
   turns before the check. */
static int case_page_scroll;
/* Set: after the case's source, the host commits a same-document
   navigation to #tf-hash and then finishes loading the document. */
static bool case_host_lifecycle;

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
    if (ok && case_host_lifecycle)
        ok = script_runtime_commit_same_document(
                 runtime, "https://dom-reentry.test/#tf-hash",
                 "https://dom-reentry.test/", &result)
             && script_runtime_finish_loading(runtime, &result);
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
    if (ok && case_page_scroll != 0) {
        ok = script_runtime_set_page_scroll(runtime, case_page_scroll);
        for (unsigned turn = 0; ok && turn < 4; turn++)
            ok = script_runtime_advance(runtime, 16, 16, &result);
    }
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

/* The same four page modes (lazy_events_prelude) applied to one other
   global, BASE, that browser code extends, reparents under or compares
   against after author code may have replaced it: a lazy module's base
   interface (WritableStream, PerformanceEntry, DOMException), or
   EventTarget where eager code read the global at call time. NB is that
   base as captured here first; under(C) wants C directly below it.
   Bodies that need it also patch NB.prototype.addEventListener and
   removeEventListener (LISTENER_PATCH), which an event handler attribute
   must not call: setting `onx` never goes through a page's patched
   addEventListener in browsers. */
static const char lazy_bases_prelude[] =
    "(()=>{globalThis.pocSummary='PENDING';"
    "const base=%s,mode=%s,NB=globalThis[base],bad=[],"
    "want=(c,m)=>{if(!c)bad.push(m)};let made=0,pending=1,patched=0;"
    "const finish=()=>{if(made)bad.push('author constructor ran '+made);"
    "pocSummary=bad.length?'BAD '+bad.join(' | '):'OK'},"
    "settle=()=>{if(--pending===0)finish()},"
    "under=(C,label)=>want(typeof C==='function'&&"
    "Object.getPrototypeOf(C)===NB&&"
    "Object.getPrototypeOf(C.prototype)===NB.prototype,"
    "label+' is not directly under '+base);"
    "if(mode==='wrap'||mode==='replace'){"
    "const Author=function(...args){made++;return mode==='wrap'?"
    "Reflect.construct(NB,args,new.target||NB):{}};"
    "if(mode==='wrap')Author.prototype=NB.prototype;globalThis[base]=Author}"
    "else if(mode==='proto'){"
    "NB.prototype.constructor=function Author(){made++;return{}};"
    "Object.defineProperty(NB.prototype,'tfPageMark',"
    "{value:1,configurable:true})}"
    "try{%s}catch(error){bad.push('threw '+error)}settle()})()";

#define LISTENER_PATCH \
    "const add=NB.prototype.addEventListener," \
    "remove=NB.prototype.removeEventListener;" \
    "NB.prototype.addEventListener=function(...a){patched++;" \
    "return add.apply(this,a)};" \
    "NB.prototype.removeEventListener=function(...a){patched++;" \
    "return remove.apply(this,a)};" \
    "const unpatch=()=>{NB.prototype.addEventListener=add;" \
    "NB.prototype.removeEventListener=remove;" \
    "want(patched===0,'handler attribute called the page\\'s " \
    "addEventListener '+patched)};"

/* Runs SRC in a Worker and wants its one message to be EXPECT. The URL
   whose createObjectURL makes the worker's URL is the prelude's, so a case
   whose base is URL still makes one. */
#define WORKER_RUN(src, expect) \
    "pending++;const w=new Worker((base==='URL'?NB:URL).createObjectURL(" \
    "new Blob([\"" src "\"],{type:'text/javascript'})));" \
    "w.onmessage=e=>{want(e.data===" expect ",'worker saw '+e.data);" \
    "w.terminate();settle()};" \
    "w.onerror=e=>{bad.push('worker error '+(e&&e.message));" \
    "w.terminate();settle()};"

/* Counts, and forwards, every call of a dispatchEvent on the prototype
   chain of each object passed to watch(); unwatch() restores them and
   wants none: the browser's own delivery dispatches with the methods
   its targets inherit, so a page's replacement sees only its own calls. */
#define DISPATCH_WATCH \
    "let sent=0;const undo=[],watch=o=>{for(let p=o;p;" \
    "p=Object.getPrototypeOf(p)){const d=Object.getOwnPropertyDescriptor(" \
    "p,'dispatchEvent');let seen=false;for(let i=0;i<undo.length;i++)" \
    "if(undo[i][0]===p)seen=true;if(!d||typeof d.value!=='function'||seen)" \
    "continue;undo[undo.length]=[p,d];const f=d.value;" \
    "Object.defineProperty(p,'dispatchEvent',{...d,value:function(e){" \
    "sent++;return Reflect.apply(f,this,[e])}})}},unwatch=()=>{" \
    "for(let i=0;i<undo.length;i++)Object.defineProperty(undo[i][0]," \
    "'dispatchEvent',undo[i][1]);undo.length=0;" \
    "want(sent===0,'browser called the page\\'s dispatchEvent '+sent)};"

/* Inserts an iframe, uses its realm and document (and writes to it),
   posts it a message and has it message its parent, which must arrive.
   (The harness has no embedder, so a message to the frame goes no
   further than the native queue.) */
#define FRAME_RUN \
    "pending++;const f=document.createElement('iframe');" \
    "try{document.body.appendChild(f);const w=f.contentWindow," \
    "d=f.contentDocument;want(w&&w.eval('1+1')===2&&d&&d.body&&" \
    "d.body.nodeName==='BODY','frame realm');" \
    "d.body.appendChild(d.createElement('p'));" \
    "want(d.body.childNodes.length===1,'frame document');" \
    "d.write('<i id=tfw></i>');w.postMessage({a:[1]},'*');" \
    "const heard=e=>{if(e.data!=='up')return;" \
    "removeEventListener('message',heard);" \
    "want(d.getElementById('tfw'),'frame write');settle()};" \
    "addEventListener('message',heard);" \
    "w.eval(\"parent.postMessage('up','*')\")}" \
    "catch(x){bad.push('frame threw '+x);settle()}"

static const struct {
    const char *name;
    const char *base;
    const char *body;
} lazy_base_families[] = {
    {"opfs-writable", "WritableStream",
     "under(FileSystemWritableFileStream,'FileSystemWritableFileStream');"},
    {"event-timing", "PerformanceEntry",
     "under(PerformanceEventTiming,'PerformanceEventTiming');"},
    {"overconstrained", "DOMException",
     "under(OverconstrainedError,'OverconstrainedError');"
     "const e=new OverconstrainedError('width','too wide');"
     "want(e instanceof NB&&e.name==='OverconstrainedError'&&"
     "e.constraint==='width'&&e.message==='too wide',"
     "'OverconstrainedError instance');"
     "pending++;navigator.mediaDevices.getUserMedia({video:true}).then("
     "()=>{bad.push('camera granted');settle()},"
     "error=>{want(error instanceof NB&&error.name==='NotFoundError',"
     "'camera refusal is not a DOMException: '+error);settle()});"},
    /* MediaQueryList is defined eagerly; it used to reparent its prototype
       under the global EventTarget's at every matchMedia(). */
    {"media-query-list", "EventTarget",
     "const m=matchMedia('(min-width: 1px)');"
     "want(Object.getPrototypeOf(MediaQueryList.prototype)===NB.prototype"
     "&&m instanceof NB,'MediaQueryList is not under EventTarget');"
     "let got=0;m.addEventListener('change',()=>got++);"
     "want(typeof m.dispatchEvent==='function','MediaQueryList dispatch');"},
    /* structuredClone and postMessage refuse platform objects. An
       EventTarget is one whatever the page did to its global or to its
       prototype's constructor. */
    {"clone-event-target", "EventTarget",
     "for(const[label,send]of[['structuredClone',v=>structuredClone(v)],"
     "['postMessage',v=>postMessage(v,'*')]]){let error=null;"
     "try{send(new NB)}catch(e){error=e}"
     "want(error&&error.name==='DataCloneError',"
     "label+' cloned an EventTarget: '+error)}"},
    {"xhr-handlers", "EventTarget",
     LISTENER_PATCH
     "const x=new XMLHttpRequest;let got=0;x.onload=()=>got++;"
     "x.dispatchEvent(new Event('load'));x.onload=null;"
     "x.dispatchEvent(new Event('load'));"
     "want(got===1,'XMLHttpRequest onload ran '+got);"
     "const u=x.upload;let up=0;u.onprogress=()=>up++;"
     "u.dispatchEvent(new Event('progress'));u.onprogress=null;"
     "want(up===1,'upload onprogress ran '+up);unpatch();"},
    {"abort-signal-handler", "EventTarget",
     LISTENER_PATCH
     "const c=new AbortController;let got=0;c.signal.onabort=()=>got++;"
     "c.abort();want(got===1,'onabort ran '+got);"
     "const d=new AbortController;d.signal.onabort=()=>got++;"
     "d.signal.onabort=null;d.abort();want(got===1,'cleared onabort ran');"
     "unpatch();"},
    {"window-handlers", "EventTarget",
     LISTENER_PATCH
     "let got=0;onresize=()=>got++;"
     "dispatchEvent(new Event('resize'));onresize=null;"
     "dispatchEvent(new Event('resize'));"
     "want(got===1,'window onresize ran '+got);unpatch();"},
    /* The lazily loaded modules' handler attributes, and the listeners
       they add for their own bookkeeping (IndexedDB's upgrade
       transaction), take the captured pair too. */
    {"indexeddb-handlers", "EventTarget",
     LISTENER_PATCH
     "pending++;const r=indexedDB.open('tf-handlers',1);let up=0;"
     "r.onupgradeneeded=()=>{up++;r.result.createObjectStore('s')};"
     "r.onsuccess=()=>{const db=r.result;db.onversionchange=()=>{};"
     "db.onversionchange=null;db.close();want(up===1,'upgrade ran '+up);"
     "unpatch();settle()};"
     "r.onerror=()=>{bad.push('open failed');unpatch();settle()};"},
    {"message-port-handlers", "EventTarget",
     LISTENER_PATCH
     "const c=new MessageChannel,d=new MessageChannel;pending++;"
     "d.port1.onmessageerror=()=>{};d.port1.onmessageerror=null;"
     "d.port1.onmessage=()=>{};"
     "c.port2.onmessage=e=>{want(e.data===1&&e.ports.length===1,"
     "'port message '+e.data);c.port2.onmessage=null;c.port1.close();"
     "unpatch();settle()};c.port1.postMessage(1,[d.port1]);"},
    /* The worker global's own addEventListener too: the worker realm
       shares the owner's EventTarget.prototype, whose owner patches must
       not see the worker's listeners. Called bare, it listens on the
       worker global, as self.addEventListener does. */
    {"worker-handlers", "EventTarget",
     LISTENER_PATCH
     "pending++;const w=new Worker(URL.createObjectURL(new Blob(["
     "\"self.addEventListener('message',e=>postMessage('self'+e.data));"
     "addEventListener('message',e=>postMessage('bare'+e.data));"
     "self.addEventListener('tf',()=>postMessage('tf'));"
     "dispatchEvent(new Event('tf'))\"],"
     "{type:'text/javascript'})));"
     "w.onmessageerror=()=>{};w.onmessageerror=null;const got=[];"
     "w.onmessage=e=>{got.push(e.data);if(got.length<3)return;"
     "want(got.sort().join()==='bare1,self1,tf','worker replied '+got);"
     "w.terminate();unpatch();settle()};"
     "w.onerror=e=>{bad.push('worker error '+(e&&e.message));"
     "w.terminate();unpatch();settle()};w.postMessage(1);"},
    {"permission-status-handler", "EventTarget",
     LISTENER_PATCH
     "pending++;navigator.permissions.query({name:'microphone'}).then(s=>{"
     "s.onchange=()=>{};s.onchange=null;unpatch();settle()},"
     "e=>{bad.push('query '+e);unpatch();settle()});"},
    {"speech-media-handlers", "EventTarget",
     LISTENER_PATCH
     "const u=new SpeechSynthesisUtterance('hi');"
     "u.onstart=()=>{};u.onstart=null;"
     "const s=new MediaStream;s.onaddtrack=()=>{};s.onaddtrack=null;"
     "unpatch();"},
    /* The interfaces lazy modules add members to: mediaDevices goes on
       Navigator.prototype and fonts on Document.prototype, whatever the
       page left at the globals. */
    {"media-devices-navigator", "Navigator",
     "const md=navigator.mediaDevices,"
     "d=Object.getOwnPropertyDescriptor(NB.prototype,'mediaDevices');"
     "want(d&&typeof d.get==='function','mediaDevices is not on "
     "Navigator.prototype');"
     "want(md&&md===navigator.mediaDevices&&md instanceof MediaDevices,"
     "'navigator.mediaDevices '+md);"},
    {"fonts-document", "Document",
     "const fonts=document.fonts,"
     "d=Object.getOwnPropertyDescriptor(NB.prototype,'fonts');"
     "want(d&&typeof d.get==='function','fonts is not on Document.prototype');"
     "want(fonts&&fonts===document.fonts&&fonts instanceof FontFaceSet,"
     "'document.fonts '+fonts);"},
    {"canvas-element", "HTMLCanvasElement",
     "const c=document.createElement('canvas'),g=c.getContext('2d');"
     "want(g&&typeof g.fillRect==='function','getContext '+g);"},
    {"get-html-element", "Element",
     "document.body.innerHTML='<b>x</b>';"
     "want(document.body.getHTML()==='<b>x</b>','getHTML');"
     "want(typeof Object.getOwnPropertyDescriptor(NB.prototype,'getHTML')"
     "?.value==='function','getHTML is not a method of Element.prototype');"},
    /* A Worker reads the shared names its owner defines eagerly as they
       were before the owner's script ran: the owner's replacement
       constructor never runs for the worker. */
    {"worker-shared-xhr", "XMLHttpRequest",
     "pending++;const w=new Worker(URL.createObjectURL(new Blob(["
     "\"const x=new XMLHttpRequest;postMessage([typeof x.open,"
     "x instanceof XMLHttpRequest,typeof XMLHttpRequest.prototype.send]"
     ".join(':'))\"],{type:'text/javascript'})));"
     "w.onmessage=e=>{want(e.data==='function:true:function',"
     "'worker saw '+e.data);w.terminate();settle()};"
     "w.onerror=e=>{bad.push('worker error '+(e&&e.message));"
     "w.terminate();settle()};"},
    {"worker-shared-event", "Event",
     "pending++;const w=new Worker(URL.createObjectURL(new Blob(["
     "\"const e=new Event('tf',{cancelable:true});e.preventDefault();"
     "postMessage([e.type,e.defaultPrevented,e instanceof Event].join(':'))"
     "\"],{type:'text/javascript'})));"
     "w.onmessage=e=>{want(e.data==='tf:true:true','worker saw '+e.data);"
     "w.terminate();settle()};"
     "w.onerror=e=>{bad.push('worker error '+(e&&e.message));"
     "w.terminate();settle()};"},
    /* Lazy modules build errors, promises and platform objects with the
       constructors hardening.js captured, not with whatever the page left
       at the global when the module loaded or the call ran. The Worker's
       owner code builds its URL, AbortController, Request and Response
       with them and hands the worker those, not the page's. */
    {"worker-type-error", "TypeError",
     "let e;try{new Worker('data:,',{type:'bogus'})}catch(x){e=x}"
     "want(e instanceof NB&&e.message==='unsupported Worker type',"
     "'Worker type error '+e);"},
    {"worker-dom-exception", "DOMException",
     "let e;try{new Worker('data:,x')}catch(x){e=x}"
     "want(e instanceof NB&&e.name==='SyntaxError','Worker URL error '+e);"},
    {"worker-url", "URL",
     WORKER_RUN("postMessage(new URL('https://a.test/b?c').pathname)",
                "'/b'")},
    {"worker-headers", "Headers",
     WORKER_RUN("postMessage(new Headers({a:'1'}).get('a'))", "'1'")},
    {"worker-request", "Request",
     WORKER_RUN("postMessage(new Request('https://a.test/x').url)",
                "'https://a.test/x'")},
    {"worker-response", "Response",
     WORKER_RUN("postMessage(new Response('hi').status)", "200")},
    {"worker-abort-controller", "AbortController",
     WORKER_RUN("postMessage(typeof fetch)", "'function'")},
    {"opfs-promise", "Promise",
     "pending++;const p=navigator.storage.getDirectory();"
     "want(p instanceof NB,'getDirectory did not return a Promise');"
     "p.then(d=>{want(d instanceof FileSystemDirectoryHandle,'directory '+d);"
     "settle()},e=>{want(e instanceof DOMException,'getDirectory '+e);"
     "settle()});"},
    {"streams-promise", "Promise",
     "pending++;const p=new ReadableStream({start(c){c.enqueue(1)}})"
     ".getReader().read();want(p instanceof NB,'read is not a Promise');"
     "p.then(r=>{want(r.value===1,'read '+r.value);settle()});"},
    {"streams-type-error", "TypeError",
     "const r=new ReadableStream;r.getReader();let e;"
     "try{r.getReader()}catch(x){e=x}"
     "want(e instanceof NB&&e.message==='stream is locked','locked '+e);"},
    {"canvas-dom-exception", "DOMException",
     "const g=document.createElement('canvas').getContext('2d');let e;"
     "try{g.getImageData(0,0,0,0)}catch(x){e=x}"
     "want(e instanceof NB&&e.name==='IndexSizeError','getImageData '+e);"},
    /* A child frame's realm (its document, its scope) is built when the
       iframe is inserted, after the page replaced Node, Document or
       Element: insertion still loads the frame and messages still cross.
       (test_document_write_scripts.c covers an async script's write.) */
    {"frame-node", "Node", FRAME_RUN},
    {"frame-document", "Document", FRAME_RUN},
    {"frame-element", "Element", FRAME_RUN},
    /* The node checks of custom elements and traversal use the realm's
       Node, whatever the page left at the global. */
    {"node-checks", "Node",
     "customElements.upgrade(document.body);"
     "const w=document.createTreeWalker(document.body);"
     "document.body.appendChild(document.createElement('b'));"
     "want(w.nextNode()&&document.createNodeIterator(document.body)"
     ".nextNode()===document.body,'traversal');"},
    /* The URL code a Worker's construction runs (parsing, serializing,
       the special-scheme setter) converts with the realm's String. */
    {"worker-url-string", "String",
     "const u=new URL(' https://a.test/b c?d=1#e ');"
     "want(u.href==='https://a.test/b%20c?d=1#e','URL '+u.href);"
     WORKER_RUN("postMessage(self.location.protocol)", "'blob:'")},
    /* Reporting an uncaught callback exception fires the Window error
       event with the Window's own dispatch, and suppresses the onerror
       attribute for that dispatch without setting it, so neither the
       page's dispatchEvent nor its listener methods run. */
    {"report-uncaught", "EventTarget",
     "let legacy=0,heard=0;onerror=()=>{legacy++;return true};"
     LISTENER_PATCH DISPATCH_WATCH
     "pending++;watch(window);"
     "add.call(window,'error',e=>{heard++;e.preventDefault()});"
     "setTimeout(()=>{throw new Error('tf-boom')},0);"
     "setTimeout(()=>{unwatch();unpatch();onerror=null;"
     "want(legacy===1&&heard===1,'reported '+legacy+' '+heard);settle()},"
     "40);"},
    /* terminate() aborts the worker's lifetime signal: the abort event is
       dispatched with EventTarget's own method. */
    {"worker-terminate", "EventTarget",
     DISPATCH_WATCH
     "watch(new AbortController().signal);"
     "const w=new Worker(URL.createObjectURL(new Blob(['1'],"
     "{type:'text/javascript'})));w.terminate();unwatch();"},
    /* DOMException is compat.js's, defined after dom.js, platform.js and
       custom-elements.js load; they bind it then, so what they throw is
       the realm's whatever the page left at the global. */
    {"late-dom-exception", "DOMException",
     "const throws=(label,name,f)=>{let e;try{f()}catch(x){e=x}"
     "want(e instanceof NB&&e.name===name,label+' threw '+e)};"
     "throws('dom.js','InvalidCharacterError',"
     "()=>document.createElement('1x'));"
     "throws('platform.js','NotSupportedError',"
     "()=>document.adoptNode(document));"
     "throws('custom-elements.js','SyntaxError',"
     "()=>customElements.define('nohyphen',class extends HTMLElement{}));"},
    /* The browser's own events (media, hashchange, readystatechange and
       load, element scroll, slotchange) are dispatched with the methods
       their targets inherit, never a page's replacement. */
    {"internal-dispatch-media", "EventTarget",
     DISPATCH_WATCH
     "const v=document.createElement('video');document.body.appendChild(v);"
     "let got=0;v.addEventListener('volumechange',()=>got++);watch(v);"
     "v.volume=0.5;v.muted=true;unwatch();"
     "want(got===2,'volumechange '+got);"},
    /* The host commits a same-document navigation (hashchange) and
       then finishes loading (readystatechange, load). */
    {"host-lifecycle-dispatch", "EventTarget",
     DISPATCH_WATCH
     "pending++;const seen=[];watch(window);watch(document);"
     "addEventListener('hashchange',()=>seen.push('hash'));"
     "document.addEventListener('readystatechange',()=>"
     "seen.push(document.readyState));"
     "addEventListener('load',()=>{unwatch();want(seen.join()==="
     "'hash,interactive,complete','events '+seen);settle()});"},
    {"internal-dispatch-scroll-slot", "EventTarget",
     DISPATCH_WATCH
     "pending++;document.body.insertAdjacentHTML('beforeend','<div id=sc "
     "style=\"overflow:scroll;height:20px\"><div style=\"height:200px\">"
     "</div></div><div id=sh></div>');"
     "const sc=document.getElementById('sc'),sh=document.getElementById('sh'),"
     "slot=sh.attachShadow({mode:'open'}).appendChild("
     "document.createElement('slot')),seen=[];"
     "sc.addEventListener('scroll',()=>seen.push('scroll'));"
     "slot.addEventListener('slotchange',()=>seen.push('slotchange'));"
     "watch(sc);watch(slot);sc.scrollTop=50;"
     "sh.appendChild(document.createElement('b'));"
     "setTimeout(()=>{unwatch();want(seen.sort().join()==="
     "'scroll,slotchange','events '+seen);settle()},40);"},
    /* Dispatch snapshots listener lists by index: the page's array
       iterator never runs. */
    {"dispatch-listener-copy", "EventTarget",
     "const I=Object.getPrototypeOf([][Symbol.iterator]()),next=I.next;"
     "let steps=0,got=0;const f=()=>got++,el=document.createElement('i');"
     "document.body.appendChild(el);el.addEventListener('tf',f);"
     "addEventListener('tf',f);document.addEventListener('tf',f);"
     "const t=new NB;t.addEventListener('tf',f);"
     "I.next=function(){steps++;return Reflect.apply(next,this,[])};"
     "try{el.dispatchEvent(new Event('tf',{bubbles:true}));"
     "dispatchEvent(new Event('tf'));t.dispatchEvent(new Event('tf'))}"
     "finally{I.next=next}"
     "want(steps===0&&got===5,'iterator ran '+steps+', listeners '+got);"},
    /* The private hooks lazy modules publish are reserved before author
       code: a page can neither define one first (so its function never
       runs in the module's place) nor replace one after the module set
       it, and none is in the enumerable global inventory. */
    {"lazy-hooks", "HTMLCanvasElement",
     "const f=()=>{bad.push('page hook ran')},names=["
     "'CanvasAttributeChanged','CanvasConnected','CanvasDimension',"
     "'CanvasReadbackState','FlushCanvasSurfaces','SetCanvasDimension',"
     "'BeginMotionObservation','MotionRecheck','MotionTreeChanged',"
     "'TransitionsDirty','WatchTransitions','CreateWebGLContext',"
     "'FlushWebGLSurfaces','WebGLConnected','WebGLReadback','WebGLResize',"
     "'AbortIndexedDBForWorker','CloneWasmModule'];"
     "for(let i=0;i<names.length;i++){const n='__tilefinch'+names[i];"
     "try{Object.defineProperty(globalThis,n,{value:f,configurable:true})}"
     "catch(_){}try{globalThis[n]=f}catch(_){}"
     "const d=Object.getOwnPropertyDescriptor(globalThis,n);"
     "want(d&&typeof d.get==='function'&&!d.enumerable&&!d.configurable,"
     "n+' is not reserved')}"
     "document.body.appendChild(document.createElement('canvas'));"
     "document.createElement('canvas').getContext('2d');"
     "document.body.appendChild(document.createElement('div'))"
     ".addEventListener('transitionend',()=>{});"
     "for(let i=0;i<11;i++){const n='__tilefinch'+names[i],"
     "v=globalThis[n];want(typeof v==='function'&&v!==f,n+' not set');"
     "try{globalThis[n]=f}catch(_){}want(globalThis[n]===v,n+' replaced')}"
     "document.body.appendChild(document.createElement('canvas'));"},
    /* Page code that runs while a lazy module loads (here a patched array
       iterator a module's for-of calls) can neither set one of its hooks
       before the module does nor claim a hook publisher of its own. */
    {"lazy-hooks-mid-load", "HTMLCanvasElement",
     "const f=()=>{bad.push('page hook ran')},names=["
     "'CanvasAttributeChanged','CanvasConnected','CanvasDimension',"
     "'CanvasReadbackState','FlushCanvasSurfaces','SetCanvasDimension',"
     "'BeginMotionObservation','MotionRecheck','MotionTreeChanged',"
     "'TransitionsDirty','WatchTransitions','CreateWebGLContext',"
     "'FlushWebGLSurfaces','WebGLConnected','WebGLReadback','WebGLResize',"
     "'AbortIndexedDBForWorker','CloneWasmModule'],"
     "I=Object.getPrototypeOf([][Symbol.iterator]()),next=I.next;"
     "let ran=0,claims=0,inside=false;"
     "I.next=function(){if(!inside){inside=true;ran++;try{"
     "const c=globalThis.__tilefinchEventInterfaces.claimHooks?.();"
     "if(typeof c==='function')claims++;"
     "for(let i=0;i<names.length;i++){const n='__tilefinch'+names[i];"
     "try{globalThis[n]=f}catch(_){}if(typeof c==='function')c(n,f)}}"
     "finally{inside=false}}return Reflect.apply(next,this,[])};"
     "try{document.createElement('canvas').getContext('2d');"
     "document.createElement('canvas').getContext('webgl');"
     "document.body.appendChild(document.createElement('div'))"
     ".addEventListener('transitionend',()=>{});void indexedDB;"
     "void WebAssembly.Module}finally{I.next=next}"
     "want(ran>0,'no page code ran while modules loaded');"
     "want(claims===0,'page claimed '+claims+' publishers');"
     "for(let i=0;i<names.length;i++){const n='__tilefinch'+names[i];"
     "want(globalThis[n]!==f,n+' is the page\\'s')}"
     "document.body.appendChild(document.createElement('canvas'));"},
    /* Event handler attributes register with the target's own method:
       never a page's patch of EventTarget's (the window, the document) nor
       a node's own addEventListener. That holds for an HTML or SVG element
       of a document made with new Document() or createHTMLDocument(), too.
       (An element new Document().createElement() makes has no namespace,
       so, as in browsers, it has no handler properties.) */
    {"handler-attributes", "EventTarget",
     "let got='';const h=e=>got+=e.currentTarget===document?'d':'e',"
     "el=document.createElement('b');document.body.appendChild(el);"
     LISTENER_PATCH
     "el.onclick=h;el.click();el.onclick=null;el.click();"
     "onmessage=h;onmessage=null;document.onclick=h;document.body.click();"
     "document.onclick=null;el.addEventListener=el.removeEventListener="
     "()=>{patched++};el.onkeyup=h;el.onkeyup=null;"
     "delete el.addEventListener;delete el.removeEventListener;"
     "el.setAttribute('onfocus','globalThis.tfInline=1');"
     "el.dispatchEvent(new Event('focus'));"
     "const nd=new Document(),hd=document.implementation"
     ".createHTMLDocument('t'),made=[nd.createElementNS("
     "'http://www.w3.org/1999/xhtml','i'),nd.createElementNS("
     "'http://www.w3.org/2000/svg','g'),hd.createElement('i')];"
     "for(let i=0;i<made.length;i++){const z=made[i];z.onclick=h;"
     "z.dispatchEvent(new Event('click'));z.onclick=null;"
     "z.dispatchEvent(new Event('click'))}unpatch();"
     "want(got==='edeee'&&globalThis.tfInline===1,'handlers ran '+got+' '+"
     "globalThis.tfInline);"},
    /* A worker's lifetime signal aborts with AbortController's own
       abort() and is read with its own signal getter. */
    {"worker-lifetime-abort", "AbortController",
     "pending++;const P=NB.prototype,ab=P.abort,"
     "sg=Object.getOwnPropertyDescriptor(P,'signal');let calls=0;"
     "P.abort=function(...a){calls++;return Reflect.apply(ab,this,a)};"
     "Object.defineProperty(P,'signal',{...sg,get(){calls++;"
     "return Reflect.apply(sg.get,this,[])}});"
     "const restore=()=>{P.abort=ab;Object.defineProperty(P,'signal',sg)};"
     "try{const w=new Worker(URL.createObjectURL(new Blob(['1'],"
     "{type:'text/javascript'})));setTimeout(()=>{w.terminate();"
     "setTimeout(()=>{restore();want(calls===0,'page abort ran '+calls);"
     "settle()},10)},10)}catch(x){restore();bad.push('threw '+x);settle()}"},
    /* A document's children change by index: no page patch of the Array
       methods, Array.from or the array iterator runs, for the page's
       document, one from createHTMLDocument() or one from new Document(). */
    {"document-insert-arrays", "Array",
     "const AP=NB.prototype,names=['filter','map','push','includes',"
     "'indexOf','slice','splice','some','lastIndexOf','find','forEach',"
     "'concat'],saved=[],I=Object.getPrototypeOf([][Symbol.iterator]()),"
     "next=I.next,from=NB.from,h=document.implementation"
     ".createHTMLDocument('t'),n=new Document(),c1=h.createComment('a'),"
     "c2=n.createComment('b'),kids=[];let calls=0;"
     "for(let i=0;i<document.childNodes.length;i++)"
     "kids[i]=document.childNodes[i];"
     "for(let i=0;i<names.length;i++){const f=AP[names[i]];saved[i]=f;"
     "AP[names[i]]=function(...a){calls++;return Reflect.apply(f,this,a)}}"
     "I.next=function(){calls++;return Reflect.apply(next,this,[])};"
     "NB.from=function(...a){calls++;return Reflect.apply(from,this,a)};"
     "let threw=null;try{document.replaceChildren.apply(document,kids);"
     "h.append(c1);"
     "h.removeChild(c1);h.insertBefore(c1,h.documentElement);n.append(c2);"
     "n.removeChild(c2);n.appendChild(c2)}catch(x){threw=x}finally{"
     "for(let i=0;i<names.length;i++)AP[names[i]]=saved[i];I.next=next;"
     "NB.from=from}"
     "want(!threw,'threw '+threw);want(calls===0,'page array code ran '+calls);"
     "want(h.childNodes[1]===c1&&n.lastChild===c2,'children');"},
    /* The browser's own builders and tree walks read no static a page
       can replace after they loaded: Object's (create, defineProperty and
       the rest) and Array.from and Array.isArray, for a document made with
       new Document() or createDocument() and its children, a detached
       host's shadow root, a node's first wrapper of an interface, and
       focusing a text field. Wrapping a node, the first of its interface
       or not, calls no WeakMap, Array or array-iterator method either. */
    {"page-statics", "Object",
     "const OS=['create','defineProperty','defineProperties',"
     "'setPrototypeOf','getPrototypeOf','getOwnPropertyDescriptor',"
     "'getOwnPropertyDescriptors','freeze','keys','assign','entries',"
     "'values','hasOwn','is'],AR=globalThis.Array,AP=AR.prototype,"
     "I=NB.getPrototypeOf([][Symbol.iterator]()),WP=WeakMap.prototype,"
     "X='http://www.w3.org/1999/xhtml',undo=[];let calls=0,where='';"
     "const wrapF=(o,k)=>{const f=o[k];undo[undo.length]=()=>{o[k]=f};"
     "o[k]=function(...a){calls++;if(where.indexOf(' '+k)<0)where+=' '+k;"
     "return Reflect.apply(f,this,a)}},restore=()=>{"
     "for(let i=undo.length-1;i>=0;i--)undo[i]();undo.length=0};"
     "let threw=null,ok=false,second=false;try{"
     "for(let i=0;i<OS.length;i++)wrapF(NB,OS[i]);"
     "wrapF(AR,'from');wrapF(AR,'isArray');"
     "const nd=new Document(),root=nd.createElementNS(X,'html'),"
     "a=nd.createElementNS(X,'i');nd.appendChild(root);root.appendChild(a);"
     "const found=nd.getElementsByTagName('i'),host=nd.createElementNS(X,"
     "'div'),sr=host.attachShadow({mode:'open'}),"
     "kids=sr.childNodes.length+sr.children.length,"
     "xd=document.implementation.createDocument(null,'r');"
     "document.createElement('marquee');document.createElement('meter');"
     "const inp=document.createElement('input');"
     "document.body.appendChild(inp);inp.focus();"
     "ok=found.length===1&&found[0]===a&&kids===0&&"
     "xd.documentElement.localName==='r'&&document.activeElement===inp;"
     "inp.remove();restore();const first=calls;"
     "const AN=['includes','indexOf','push','filter','map','slice','some',"
     "'find','forEach'];for(let i=0;i<AN.length;i++)wrapF(AP,AN[i]);"
     "wrapF(I,'next');wrapF(WP,'get');wrapF(WP,'set');"
     "const t=document.createElement('progress');"
     "document.body.appendChild(t);document.createElement('track');"
     "second=t.parentNode===document.body;restore();t.remove();"
     "want(first===0,'statics ran '+first)}"
     "catch(x){threw=x}finally{restore()}"
     "want(!threw,'threw '+threw);want(ok&&second,'results');"
     "want(calls===0,'page code ran '+calls+':'+where);"},
    /* Nodes of a document made with new Document() or createDocumentType
       get the realm's Comment and DocumentType prototypes. */
    {"detached-comment", "Comment",
     "const c=new Document().createComment('x');"
     "want(Object.getPrototypeOf(c)===NB.prototype,'comment prototype');"},
    {"detached-doctype", "DocumentType",
     "const t=document.implementation.createDocumentType('x','','');"
     "want(Object.getPrototypeOf(t)===NB.prototype,'doctype prototype');"},
};
#undef LISTENER_PATCH
#undef WORKER_RUN
#undef DISPATCH_WATCH
#undef FRAME_RUN

/* Lazy modules publish their factories and private hooks with the
   defineProperty hardening.js captured and null-prototype descriptors. A
   page that wraps Object.defineProperty never sees one of those names
   (wrap), and one that drops them cannot keep a module from loading
   (replace); the page's function still defines everything else. */
static const char define_property_prelude[] =
    "(()=>{globalThis.pocSummary='PENDING';const mode=%s,"
    "ND=Object.defineProperty,seen=[],bad=[],want=(c,m)=>{if(!c)bad.push(m)};"
    "let pending=1;const finish=()=>{Object.defineProperty=ND;"
    "if(seen.length)bad.push('page defineProperty saw '+seen);"
    "pocSummary=bad.length?'BAD '+bad.join(' | '):'OK'},"
    "settle=()=>{if(--pending===0)finish()};"
    "if(mode!=='none')Object.defineProperty=function(o,k,d){"
    "const hook=typeof k==='string'&&k.startsWith('__tilefinch');"
    "if(hook)seen.push(k);return mode==='replace'&&hook?o:ND(o,k,d)};"
    "try{%s}catch(error){bad.push('threw '+error)}settle()})()";

static const struct {
    const char *name;
    const char *body;
} define_property_families[] = {
    {"frame-clone",
     "pending++;addEventListener('message',e=>{"
     "want(e.data&&e.data.a===1,'message '+e.data);settle()},{once:true});"
     "postMessage({a:1},'*');"},
    {"documents",
     "const d=document.implementation.createHTMLDocument('t');"
     "want(d.title==='t','createHTMLDocument '+d.title);"},
    {"forms",
     "document.body.innerHTML='<select id=s><option>a</option></select>';"
     "want(document.getElementById('s').options.length===1,'options');"},
    {"shadow",
     "const h=document.createElement('div'),r=h.attachShadow({mode:'open'});"
     "want(h.shadowRoot===r,'attachShadow');"},
    {"cssom-rules",
     "const st=document.createElement('style');st.textContent='a{color:red}';"
     "document.head.appendChild(st);"
     "want(st.sheet.cssRules.length===1,'cssRules');"},
    {"serialize",
     "document.body.innerHTML='<b>x</b>';"
     "want(document.body.getHTML()==='<b>x</b>','getHTML');"},
    {"indexeddb",
     "pending++;const r=indexedDB.open('tf-define');"
     "r.onsuccess=()=>{r.result.close();settle()};"
     "r.onerror=()=>{bad.push('open failed');settle()};"},
    {"media-capture",
     "want(typeof navigator.mediaDevices.getUserMedia==='function',"
     "'mediaDevices');"},
    {"webgl",
     "const g=document.createElement('canvas').getContext('webgl');"
     "want(g===null||typeof g.clear==='function','webgl '+g);"},
    {"wasm",
     "want(typeof WebAssembly.Module==='function','WebAssembly');"},
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
        /* The other bases browser code extends or compares against after
           author code may have replaced them (lazy_bases_prelude). */
        for (size_t family = 0;
             family < sizeof(lazy_base_families) /
                          sizeof(lazy_base_families[0]);
             family++)
            for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]);
                 mode++) {
                char name[64], quoted_base[40], quoted[16], source[8192];
                snprintf(name, sizeof(name), "lazy-bases-%s-%s",
                         lazy_base_families[family].name, modes[mode]);
                snprintf(quoted_base, sizeof(quoted_base), "'%s'",
                         lazy_base_families[family].base);
                snprintf(quoted, sizeof(quoted), "'%s'", modes[mode]);
                int length = snprintf(source, sizeof(source),
                                      lazy_bases_prelude, quoted_base, quoted,
                                      lazy_base_families[family].body);
                if (length < 0 || (size_t) length >= sizeof(source)) {
                    fprintf(stderr, "%s: source too long\n", name);
                    ok = false;
                    continue;
                }
                case_host_lifecycle =
                    strncmp(lazy_base_families[family].name,
                            "host-lifecycle", 14) == 0;
                ok &= run_case(name, source);
                case_host_lifecycle = false;
            }
    }
    {
        static const char *const modes[] = {"wrap", "replace", "none"};
        for (size_t family = 0;
             family < sizeof(define_property_families) /
                          sizeof(define_property_families[0]);
             family++)
            for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]);
                 mode++) {
                char name[64], quoted[16], source[4096];
                snprintf(name, sizeof(name), "lazy-define-%s-%s",
                         define_property_families[family].name, modes[mode]);
                snprintf(quoted, sizeof(quoted), "'%s'", modes[mode]);
                int length = snprintf(source, sizeof(source),
                                      define_property_prelude, quoted,
                                      define_property_families[family].body);
                if (length < 0 || (size_t) length >= sizeof(source)) {
                    fprintf(stderr, "%s: source too long\n", name);
                    ok = false;
                    continue;
                }
                ok &= run_case(name, source);
            }
    }
    /* Window messaging (frame-clone.js, loaded on the first window
       message) and structuredClone (platform.js's worker clone) after the
       page replaced the constructors they build and check values with
       (constructors), patched the prototype methods and getters a clone
       could reach (methods: Map and Set methods, the Map, Set and Array
       iterators' next(), the view getters, Array push and some), or both.
       No page function runs anywhere on the path, from the call to the
       message listener; every value arrives built by the realm's own
       constructors, and an error whose name is an Object.prototype member
       stays an Error. */
    {
        static const char clone_prelude[] =
            "(()=>{globalThis.pocSummary='PENDING';const mode=%s,via=%s,"
            "N={},ran=[],saved=[],note=name=>{ran[ran.length]=name};"
            "const named=new Error('m');named.name='constructor';"
            "const value={m:new Map([[1,'x']]),s:new Set([3]),d:new Date(5),"
            "r:/x/g,e:new TypeError('t'),c:named,b:5n,n:-0,a:[1,[2]],"
            "v:new Uint8Array([7,8]).subarray(1),"
            "w:new DataView(new ArrayBuffer(4),1,2)};"
            "const check=(d,error)=>{const bad=[],want=(c,m)=>{if(!c)bad.push(m)};"
            "want(!ran.length,'page code ran: '+ran.join());"
            "want(d.m instanceof Map&&d.m.get(1)==='x','map');"
            "want(d.s instanceof Set&&d.s.has(3),'set');"
            "want(d.d instanceof Date&&d.d.getTime()===5,'date');"
            "want(d.r instanceof RegExp&&d.r.source==='x'&&d.r.flags==='g',"
            "'regexp');"
            "want(d.e instanceof TypeError&&d.e.message==='t','error');"
            "want(d.c instanceof Error&&d.c.message==='m','named error '+"
            "Object.prototype.toString.call(d.c));"
            "want(d.b===5n&&Object.is(d.n,-0),'bigint or -0');"
            "want(Array.isArray(d.a)&&d.a[1][0]===2,'array');"
            "want(d.v instanceof Uint8Array&&d.v.length===1&&d.v[0]===8,'view');"
            "want(d.w instanceof DataView&&d.w.byteOffset===1&&"
            "d.w.byteLength===2,'data view');"
            "want(error&&error.name==='DataCloneError'&&"
            "error instanceof DOMException,'clone error '+error);"
            "pocSummary=bad.length?'BAD '+bad.join(' | '):'OK'};"
            /* The patches go in last, and nothing after them but the
               message or clone itself calls a patched function. */
            "const author=(f,name)=>function(...a){note(name);"
            "return new.target?Reflect.construct(f,a,new.target):"
            "Reflect.apply(f,this,a)};"
            "const TA=Object.getPrototypeOf(Uint8Array.prototype),"
            "it=o=>Object.getPrototypeOf(o[Symbol.iterator]()),"
            "targets=[[Map.prototype,['has','get','set','forEach','entries']],"
            "[Set.prototype,['has','add','forEach','values']],"
            "[it(new Map),['next']],[it(new Set),['next']],"
            "[TA,['buffer','byteOffset','byteLength','length']],"
            "[DataView.prototype,['buffer','byteOffset','byteLength']],"
            "[it([]),['next']]];"
            "const names=['Array','BigInt','Date','Map','Number','RegExp','Set',"
            "'String','Error','TypeError','DOMException'];"
            "let error=null;"
            "if(via==='window')"
            "addEventListener('message',ev=>{restore();check(ev.data,error)});"
            "if(mode!=='constructors')for(let i=0;i<targets.length;i++)"
            "for(let j=0;j<targets[i][1].length;j++){const o=targets[i][0],"
            "k=targets[i][1][j],d=Object.getOwnPropertyDescriptor(o,k),"
            "slot=d.get?'get':'value';saved[saved.length]=[o,k,d];"
            "Object.defineProperty(o,k,{...d,[slot]:author(d[slot],k)})}"
            "if(mode!=='methods')for(let i=0;i<names.length;i++){"
            "const name=names[i],Native=N[name]=globalThis[name],"
            "Author=globalThis[name]=author(Native,name);"
            "Object.setPrototypeOf(Author,Native);"
            "Author.prototype=Native.prototype}"
            "const restore=()=>{for(let i=0;i<saved.length;i++)"
            "Object.defineProperty(saved[i][0],saved[i][1],saved[i][2]);"
            "for(const k in N)globalThis[k]=N[k]};"
            "if(via==='window'){try{postMessage(()=>1,'*')}catch(x){error=x}"
            "postMessage(value,'*')}"
            "else{let copy;try{try{structuredClone(()=>1)}catch(x){error=x}"
            "copy=structuredClone(value)}finally{restore()}check(copy,error)}"
            "})()";
        static const char *const modes[] = {"constructors", "methods",
                                            "both"};
        static const char *const vias[] = {"window", "structured"};
        for (size_t via = 0; via < 2; via++)
            for (size_t mode = 0; mode < 3; mode++) {
                char name[80], quoted_mode[24], quoted_via[24], source[8192];
                snprintf(name, sizeof(name), "clone-ignores-page-%s-%s",
                         vias[via], modes[mode]);
                snprintf(quoted_mode, sizeof(quoted_mode), "'%s'",
                         modes[mode]);
                snprintf(quoted_via, sizeof(quoted_via), "'%s'", vias[via]);
                int length = snprintf(source, sizeof(source), clone_prelude,
                                      quoted_mode, quoted_via);
                if (length < 0 || (size_t) length >= sizeof(source)) {
                    fprintf(stderr, "%s: source too long\n", name);
                    ok = false;
                    continue;
                }
                ok &= run_case(name, source);
            }
    }
    /* The browser's own delivery dispatches with the methods its targets
       inherit, never a page's replacement of dispatchEvent (wrapped:
       counted and forwarded; replaced: counted and dropped). Host input,
       pointer capture, the scroll flush on document and visualViewport,
       BroadcastChannel and matchMedia's legacy addListener are each run
       with every dispatchEvent on the target's prototype chain patched. */
    {
#define DISPATCH_PATCH(mode) \
        "globalThis.calls=0;const patched=[],undo=[],unpatch=()=>{" \
        "for(let i=0;i<patched.length;i++)Object.defineProperty(patched[i]," \
        "'dispatchEvent',undo[i])},patch=o=>{for(let p=o;p;" \
        "p=Object.getPrototypeOf(p)){const d=Object.getOwnPropertyDescriptor(" \
        "p,'dispatchEvent');if(!d||typeof d.value!=='function'||" \
        "patched.includes(p))continue;patched[patched.length]=p;" \
        "undo[undo.length]=d;const f=d.value;" \
        "Object.defineProperty(p,'dispatchEvent',{...d,value:" mode \
        "==='wrap'?function(e){calls++;return Reflect.apply(f,this,[e])}:" \
        "function(){calls++;return true}})}};"
        static const char *const modes[] = {"'wrap'", "'replace'"};
        static const char *const mode_names[] = {"wrap", "replace"};
        static const char *const host_types[] = {
            "click", "input", "submit", NULL,
        };
        static const char *const no_types[] = {NULL};
        static const char host_check[] =
            "pocSummary=calls===0&&seen.join()==='click,input,submit'?'OK':"
            "'BAD calls='+calls+' '+seen";
        static const char host_setup[] =
            "(()=>{" DISPATCH_PATCH("%s")
            "document.body.insertAdjacentHTML('beforeend',"
            "'<form id=t><input></form>');globalThis.seen=[];"
            "const t=document.getElementById('t');"
            "for(const type of['click','input','submit'])"
            "t.addEventListener(type,e=>{seen.push(e.type);"
            "if(type==='submit')e.preventDefault()});"
            "patch(t);patch(t.firstChild);patch(document);pocSummary='OK'})()";
        static const char scroll_setup[] =
            "(()=>{" DISPATCH_PATCH("%s")
            "const vv=visualViewport;globalThis.seen=[];globalThis.vv=vv;"
            "document.addEventListener('scroll',()=>seen.push('doc'));"
            "vv.addEventListener('scroll',()=>seen.push('vv'));"
            "vv.onscroll=()=>seen.push('on');patch(vv);patch(document);"
            "globalThis.visualViewport={pageTop:0};pocSummary='OK'})()";
        static const char scroll_check[] =
            "pocSummary=calls===0&&seen.join()==='doc,vv,on'&&"
            "vv.pageTop===40?'OK':'BAD calls='+calls+' '+seen+' '+vv.pageTop";
        static const char capture_source[] =
            "(()=>{" DISPATCH_PATCH("%s")
            "document.body.insertAdjacentHTML('beforeend','<div id=c></div>');"
            "const c=document.getElementById('c'),seen=[];"
            "for(const type of['gotpointercapture','lostpointercapture'])"
            "c.addEventListener(type,e=>seen.push(e.type));patch(c);"
            "c.setPointerCapture(1);c.releasePointerCapture(1);"
            "pocSummary=calls===0&&seen.join()==="
            "'gotpointercapture,lostpointercapture'?'OK':"
            "'BAD calls='+calls+' '+seen})()";
        /* Delivery also reads no page-patchable collection method and
           clones with the realm's structuredClone. */
        static const char broadcast_source[] =
            "(()=>{" DISPATCH_PATCH("%s")
            "pocSummary='PENDING';const a=new BroadcastChannel('c'),"
            "b=new BroadcastChannel('c'),seen=[],saved=[],NC=structuredClone,"
            "it=o=>Object.getPrototypeOf(o[Symbol.iterator]());"
            "const restore=()=>{for(let i=0;i<saved.length;i++)"
            "Object.defineProperty(saved[i][0],saved[i][1],saved[i][2]);"
            "globalThis.structuredClone=NC;unpatch()};"
            "b.onmessage=e=>seen.push('h:'+e.data);"
            "b.addEventListener('message',e=>{restore();seen.push(e.data);"
            "pocSummary=calls===0&&seen.join()==='h:hi,hi'?'OK':"
            "'BAD calls='+calls+' '+seen});"
            "const targets=[[Map.prototype,['get','set','delete']],"
            "[Set.prototype,['add','delete','forEach','size']],"
            "[it(new Set),['next']]];"
            "for(let i=0;i<targets.length;i++)"
            "for(let j=0;j<targets[i][1].length;j++){const o=targets[i][0],"
            "k=targets[i][1][j],d=Object.getOwnPropertyDescriptor(o,k),"
            "slot=d.get?'get':'value',f=d[slot];saved[saved.length]=[o,k,d];"
            "Object.defineProperty(o,k,{...d,[slot]:function(...x){calls++;"
            "return Reflect.apply(f,this,x)}})}"
            "globalThis.structuredClone=%s==='wrap'?v=>{calls++;return NC(v)}:"
            "()=>{calls++;return 'forged'};patch(b);a.postMessage('hi')})()";
        static const char media_source[] =
            "(()=>{" DISPATCH_PATCH("%s")
            "const q=matchMedia('(min-width: 1px)'),P=MediaQueryList.prototype,"
            "saved={},seen=[],f=e=>seen.push(e.type);"
            "for(const k of['addEventListener','removeEventListener']){"
            "const g=saved[k]=P[k];P[k]=%s==='wrap'?function(...a){calls++;"
            "return Reflect.apply(g,this,a)}:function(){calls++}}"
            "q.addListener(f);Object.assign(P,saved);"
            "q.dispatchEvent(new Event('change'));"
            "const added=seen.length;P.removeEventListener=function(){calls++};"
            "q.removeListener(f);P.removeEventListener=saved.removeEventListener;"
            "q.dispatchEvent(new Event('change'));"
            "pocSummary=calls===0&&added===1&&seen.length===1?'OK':"
            "'BAD calls='+calls+' added='+added+' seen='+seen.length})()";
        for (size_t mode = 0; mode < 2; mode++) {
            char name[80], source[4096];
            int length;
#define FORMAT_DISPATCH_CASE(label, ...) \
            snprintf(name, sizeof(name), label "-%s", mode_names[mode]); \
            length = snprintf(source, sizeof(source), __VA_ARGS__); \
            if (length < 0 || (size_t) length >= sizeof(source)) { \
                fprintf(stderr, "%s: source too long\n", name); \
                ok = false; \
                continue; \
            }
            FORMAT_DISPATCH_CASE("host-input-ignores-page-dispatch",
                                 host_setup, modes[mode]);
            ok &= run_host_dispatch_case(name, source, "#t", host_types,
                                         host_check);
            FORMAT_DISPATCH_CASE("page-scroll-ignores-page-dispatch",
                                 scroll_setup, modes[mode]);
            case_page_scroll = 40;
            ok &= run_host_dispatch_case(name, source, "body", no_types,
                                         scroll_check);
            case_page_scroll = 0;
            FORMAT_DISPATCH_CASE("pointer-capture-ignores-page-dispatch",
                                 capture_source, modes[mode]);
            ok &= run_case(name, source);
            FORMAT_DISPATCH_CASE("broadcast-channel-ignores-page-dispatch",
                                 broadcast_source, modes[mode], modes[mode]);
            ok &= run_case(name, source);
            FORMAT_DISPATCH_CASE("media-query-listener-ignores-page-methods",
                                 media_source, modes[mode], modes[mode]);
            ok &= run_case(name, source);
#undef FORMAT_DISPATCH_CASE
        }
#undef DISPATCH_PATCH
    }
    /* A script-made XML document's nodes delegate addEventListener and
       removeEventListener to EventTarget's methods as browsers expose them:
       a page's patch of those methods (zone.js) still sees each call, but
       replacing the EventTarget global changes nothing. */
    ok &= run_case("detached-listeners-delegate-to-event-target",
        "(()=>{const d=document.implementation.createDocument(null,'r'),"
        "el=d.createElement('div'),P=EventTarget.prototype,"
        "add=P.addEventListener,remove=P.removeEventListener,NET=EventTarget,"
        "seen=[],f=e=>seen.push(e.type);"
        "let zone=0;P.addEventListener=function(...a){zone++;"
        "return Reflect.apply(add,this,a)};"
        "P.removeEventListener=function(...a){zone++;"
        "return Reflect.apply(remove,this,a)};"
        "globalThis.EventTarget=function(){};"
        "try{d.addEventListener('x',f);el.addEventListener('x',f);"
        "d.dispatchEvent(new Event('x'));el.dispatchEvent(new Event('x'));"
        "d.removeEventListener('x',f);el.removeEventListener('x',f);"
        "d.dispatchEvent(new Event('x'));el.dispatchEvent(new Event('x'))}"
        "finally{globalThis.EventTarget=NET;"
        "P.addEventListener=add;P.removeEventListener=remove}"
        "pocSummary=zone===4&&seen.join()==='x,x'?'OK':"
        "'BAD zone='+zone+' seen='+seen})()");
    /* Host focus and blur, and a label's activation of its control, run
       the focusing, unfocusing and click steps themselves: a page's own
       focus, blur or click on the element (wrapped: counted and
       forwarded; replaced: counted and dropped) never runs. */
    {
        static const char *const modes[] = {"wrap", "replace"};
        static const char *const focus_types[] = {"focus", "blur", NULL};
        static const char focus_setup[] =
            "(()=>{document.body.insertAdjacentHTML('beforeend','<input id=a>"
            "<label id=l for=b>L</label><input id=b type=checkbox>');"
            "globalThis.calls=0;globalThis.seen=[];const mode='%s',"
            "$=id=>document.getElementById(id),a=$('a'),b=$('b');"
            "for(const t of['focus','blur'])a.addEventListener(t,e=>"
            "seen.push(e.type));"
            "for(const[o,k]of[[a,'focus'],[a,'blur'],[b,'focus'],[b,'click']]){"
            "const f=o[k];o[k]=mode==='wrap'?function(...x){calls++;"
            "return Reflect.apply(f,this,x)}:function(){calls++}}"
            "pocSummary='OK'})()";
        static const char focus_check[] =
            "(()=>{const b=document.getElementById('b');"
            "document.getElementById('l').click();"
            "pocSummary=calls===0&&seen.join()==='focus,blur'&&b.checked&&"
            "document.activeElement===b?'OK':'BAD calls='+calls+' seen='+seen+"
            "' checked='+b.checked+' active='+document.activeElement?.id})()";
        for (size_t mode = 0; mode < 2; mode++) {
            char name[80], source[2048];
            snprintf(name, sizeof(name), "host-focus-ignores-page-methods-%s",
                     modes[mode]);
            int length = snprintf(source, sizeof(source), focus_setup,
                                  modes[mode]);
            if (length < 0 || (size_t) length >= sizeof(source)) {
                fprintf(stderr, "%s: source too long\n", name);
                ok = false;
                continue;
            }
            ok &= run_host_dispatch_case(name, source, "#a", focus_types,
                                         focus_check);
        }
    }
    /* DOMException, which the browser throws too, converts with its own
       String and finds its code in a prototype-free table. */
    ok &= run_case("dom-exception-ignores-replaced-string",
        "(()=>{const NS=String;let made=0;"
        "globalThis.String=function(v){made++;return NS(v)};"
        "let e;try{e=new DOMException('m','constructor')}"
        "finally{globalThis.String=NS}"
        "globalThis.pocSummary=made===0&&e.message==='m'&&"
        "e.name==='constructor'&&e.code===0?'OK':"
        "'BAD made='+made+' code='+typeof e.code})()");
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
        "const keys=Reflect.ownKeys(table),"
        "listen=['addListener','removeListener','dispatch','defineProperty',"
        "'apply','promiseThen','promiseReject','abortController',"
        "'controllerSignal','signalReason','claimHooks'];"
        "for(const key of['Event','EventTarget','Navigator','Document',"
        "'XMLHttpRequest','console',...listen])"
        "check(keys.includes(key),'keys '+keys);"
        "for(const key of keys)if(!listen.includes(key))"
        "check((typeof table[key]==='function'||key==='console')&&"
        "table[key]===globalThis[key],key+' is not the public interface');"
        /* The listener pair is EventTarget's own methods, call-bound. */
        "const t=new EventTarget;let got=0;const f=()=>got++;"
        "table.addListener(t,'x',f);t.dispatchEvent(new Event('x'));"
        "table.removeListener(t,'x',f);t.dispatchEvent(new Event('x'));"
        "table.addListener(t,'y',f);table.dispatch(t,new Event('y'));"
        "check(got===2,'listener pair ran '+got);"
        /* AbortController's own abort() and signal and reason getters. */
        "const ac=new AbortController;table.abortController(ac,7);"
        "check(table.controllerSignal(ac)===ac.signal&&"
        "table.signalReason(ac.signal)===7,'abort trio');"
        /* The intrinsics beside it are the realm's own. */
        "check(table.defineProperty===Object.defineProperty&&"
        "table.apply===Reflect.apply&&(r=>(r.catch(()=>{}),r instanceof Promise))"
        "(table.promiseReject(1))"
        "&&table.promiseThen(Promise.resolve(),()=>1) instanceof Promise,"
        "'intrinsics');"
        "for(const key of['TypeError','Promise','Map','URL','Headers',"
        "'Request','Response'])check(keys.includes(key),'no '+key);"
        "check(!Object.getOwnPropertyNames(globalThis).includes("
        "'__tilefinchEventInterfaces'),'visible to reflection');"
        "const NE=Event;globalThis.Event=function(){};"
        "globalThis.__tilefinchEventInterfaces=null;"
        "check(globalThis.__tilefinchEventInterfaces===table&&"
        "table.Event===NE,'table changed');"
        /* No lazy module loads now, so there is no hook publisher. */
        "check(table.claimHooks()===null,'claimed a hook publisher');"
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
