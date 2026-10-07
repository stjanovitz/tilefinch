/* Web-standards conformance probes for surface that page scripts (and
   fingerprinting/anti-bot scripts in particular) can observe directly:
   time-zone agreement between Date and Intl, WebIDL function names and
   lengths, where document members live, when location changes, and the
   Window's child navigables. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

/* Runs `script` as the only classic script of a fresh document whose body
   starts with `body` and returns whether globalThis.pocSummary equalled
   `expected`. */
static bool run_probe_after(const char *label, const char *body,
                            const char *script, const char *expected)
{
    static char page[64 * 1024];
    int written = snprintf(page, sizeof(page),
                           "<!doctype html><html><head><title>probe</title>"
                           "</head><body>%s<script>%s</script>"
                           "</body></html>",
                           body, script);
    if (written < 0 || (size_t) written >= sizeof(page)) {
        fprintf(stderr, "%s: probe page too large\n", label);
        return false;
    }
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    if (!document_parse(&document, &budget, page, strlen(page), 17)) {
        fprintf(stderr, "%s: parse failed\n", label);
        return false;
    }
    ScriptResult result;
    bool ran = scripts_run_document_at(&document, &budget, 8u * MIB, 3000,
                                       "https://conformance.test/start",
                                       &result);
    bool ok = ran && result.success
        && strcmp(result.summary, expected) == 0;
    if (!ok) {
        fprintf(stderr, "%s: ran=%d success=%d summary=%s error=%s\n", label,
                ran, result.success, result.summary, result.error);
    }
    document_destroy(&document);
    if (ok && budget.current != 0) {
        fprintf(stderr, "%s: %zu bytes left owned\n", label, budget.current);
        ok = false;
    }
    return ok;
}

static bool run_probe(const char *label, const char *script,
                      const char *expected)
{
    return run_probe_after(label, "<p id=p>text</p>", script, expected);
}

/* ---- 1. Intl's default time zone and formatting agree with Date ---- */

static const char time_zone_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const zone=new Intl.DateTimeFormat().resolvedOptions().timeZone;"
    "const instants=[Date.UTC(2024,0,15,17,5,9),Date.UTC(2024,6,15,3,4,5),"
    "Date.UTC(2023,10,5,8,30,0)];"
    "for(const t of instants){const d=new Date(t);"
    "const fmt=new Intl.DateTimeFormat();"
    "expect('date '+t,fmt.format(d)===d.toLocaleDateString());"
    "expect('date shape '+t,fmt.format(d)===(d.getMonth()+1)+'/'+d.getDate()"
    "+'/'+d.getFullYear());"
    "const time=new Intl.DateTimeFormat('en-US',{hour:'numeric',"
    "minute:'numeric',second:'numeric'}).format(d);"
    "expect('time '+t+' '+time+' '+d.toLocaleTimeString(),"
    "time===d.toLocaleTimeString());"
    "expect('both '+t,new Intl.DateTimeFormat('en-US',{year:'numeric',"
    "month:'numeric',day:'numeric',hour:'numeric',minute:'numeric',"
    "second:'numeric'}).format(d)===d.toLocaleString());"
    "const parts=new Intl.DateTimeFormat('en-US',{hour:'numeric',"
    "hour12:false}).formatToParts(d);"
    "expect('hour part '+t,Number(parts.find(p=>p.type==='hour').value)"
    "===d.getHours());"
    "const utc=new Intl.DateTimeFormat('en-US',{timeZone:'UTC',"
    "hour:'numeric',hour12:false}).formatToParts(d);"
    "expect('utc hour '+t,Number(utc.find(p=>p.type==='hour').value)"
    "===d.getUTCHours());}"
    "const fixed=new Intl.DateTimeFormat('en-US',{timeZone:'+05:30',"
    "hour:'numeric',minute:'numeric',hour12:false});"
    "expect('fixed zone',fixed.resolvedOptions().timeZone==='+05:30'"
    "&&fixed.format(Date.UTC(2024,0,1,0,0))==='05:30');"
    "expect('named format getter',typeof new Intl.DateTimeFormat().format"
    "==='function'&&[0].map(new Intl.DateTimeFormat('en-US',"
    "{timeZone:'UTC'}).format)[0]==='1/1/1970');"
    "expect('long date',new Intl.DateTimeFormat('en-US',{timeZone:'UTC',"
    "weekday:'long',year:'numeric',month:'long',day:'numeric'})"
    ".format(Date.UTC(2026,9,1))==='Thursday, October 1, 2026');"
    "let invalid=false;try{new Intl.DateTimeFormat('en',{timeZone:'not a zone'})}"
    "catch(e){invalid=e instanceof RangeError}expect('invalid zone',invalid);"
    "globalThis.pocSummary=failures.length?'FAIL:'+failures.join('|'):"
    "zone+'@'+new Date(instants[0]).getTimezoneOffset()+'/'"
    "+new Date(instants[1]).getTimezoneOffset();";

static int set_time_zone(const char *value)
{
    if (value == NULL) {
        if (unsetenv("TZ") != 0) return 1;
    } else if (setenv("TZ", value, 1) != 0) {
        return 1;
    }
    tzset();
    return 0;
}

static int test_time_zone_agreement(void)
{
    puts("test: Intl default time zone agrees with Date");
    const char *original = getenv("TZ");
    char saved[128] = {0};
    bool had_original = original != NULL;
    if (had_original) snprintf(saved, sizeof(saved), "%s", original);

    /* An installed IANA zone is reported by name, with DST-dependent
       offsets taken from Date for each instant. */
    CHECK(set_time_zone("America/New_York") == 0);
    CHECK(run_probe("new-york", time_zone_probe,
                    "America/New_York@300/240"));
    CHECK(set_time_zone(":Asia/Kolkata") == 0);
    CHECK(run_probe("kolkata", time_zone_probe, "Asia/Kolkata@-330/-330"));
    /* UTC spellings collapse to the canonical "UTC". */
    CHECK(set_time_zone("Etc/UTC") == 0);
    CHECK(run_probe("etc-utc", time_zone_probe, "UTC@0/0"));
    CHECK(set_time_zone("") == 0);
    CHECK(run_probe("empty", time_zone_probe, "UTC@0/0"));
    /* A POSIX rule names no IANA zone: report an offset identifier rather
       than inventing a city. */
    CHECK(set_time_zone("<+0530>-5:30") == 0);
    CHECK(run_probe("posix-rule", time_zone_probe, "+05:30@-330/-330"));
    CHECK(set_time_zone("<-03>3") == 0);
    CHECK(run_probe("posix-negative", time_zone_probe, "-03:00@180/180"));

    CHECK(set_time_zone(had_original ? saved : NULL) == 0);
    return 0;
}


/* ---- 2. WebIDL function names, lengths and native source text ---- */

/* Each row: holder expression | member | kind (m=method, g=getter,
   s=setter) | WebIDL length. Methods are read with an ordinary [[Get]]
   from the holder, accessors from the first descriptor on its chain. */
static const char function_shape_probe[] =
    "const el=document.createElement('div');"
    /* Element wrappers carry many Node/Element members on a shared
       per-tag prototype rather than on Node.prototype/Element.prototype;
       the names are checked here through an element, wherever they live. */
    "const rows=`"
    "window|setTimeout|m|1;window|setInterval|m|1;window|clearTimeout|m|0;"
    "window|clearInterval|m|0;window|requestAnimationFrame|m|1;"
    "window|cancelAnimationFrame|m|1;window|queueMicrotask|m|1;"
    "window|fetch|m|1;window|atob|m|1;window|btoa|m|1;"
    "window|structuredClone|m|1;window|getComputedStyle|m|1;"
    "window|matchMedia|m|1;window|postMessage|m|1;window|scrollTo|m|0;"
    "window|scrollBy|m|0;window|scroll|m|0;window|requestIdleCallback|m|1;"
    "window|cancelIdleCallback|m|1;window|createImageBitmap|m|1;"
    "window|getSelection|m|0;window|open|m|0;window|reportError|m|1;"
    "window|addEventListener|m|2;window|removeEventListener|m|2;"
    "window|dispatchEvent|m|1;"
    "EventTarget.prototype|addEventListener|m|2;"
    "EventTarget.prototype|removeEventListener|m|2;"
    "EventTarget.prototype|dispatchEvent|m|1;"
    "Node.prototype|appendChild|m|1;Node.prototype|insertBefore|m|2;"
    "el|removeChild|m|1;el|replaceChild|m|2;"
    "el|cloneNode|m|0;Node.prototype|contains|m|1;"
    "Node.prototype|hasChildNodes|m|0;Node.prototype|normalize|m|0;"
    "Node.prototype|getRootNode|m|0;Node.prototype|isSameNode|m|1;"
    "Node.prototype|isEqualNode|m|1;"
    "Node.prototype|compareDocumentPosition|m|1;"
    "Node.prototype|isConnected|g|0;"
    "el|getAttribute|m|1;el|setAttribute|m|2;"
    "el|removeAttribute|m|1;"
    "el|hasAttribute|m|1;"
    "el|toggleAttribute|m|1;"
    "el|getAttributeNames|m|0;"
    "Element.prototype|hasAttributes|m|0;"
    "Element.prototype|replaceChildren|m|0;"
    "Element.prototype|checkVisibility|m|0;"
    "HTMLElement.prototype|dir|g|0;HTMLElement.prototype|dir|s|1;"
    "HTMLElement.prototype|inputMode|g|0;"
    "HTMLElement.prototype|accessKey|g|0;HTMLElement.prototype|autofocus|g|0;"
    "HTMLImageElement.prototype|loading|g|0;"
    "HTMLScriptElement.prototype|text|g|0;HTMLScriptElement.prototype|text|s|1;"
    "HTMLScriptElement|supports|m|1;"
    "URL|canParse|m|1;URL|parse|m|1;URL.prototype|username|g|0;"
    "URL.prototype|password|s|1;URLSearchParams.prototype|size|g|0;"
    "Document.prototype|open|m|0;Document.prototype|prerendering|g|0;"
    "Navigator.prototype|globalPrivacyControl|g|0;"
    "window|print|m|0;Document.prototype|all|g|0;"
    "TextEvent.prototype|initTextEvent|m|1;"
    "Element.prototype|querySelector|m|1;"
    "Element.prototype|querySelectorAll|m|1;Element.prototype|matches|m|1;"
    "Element.prototype|closest|m|1;"
    "el|getBoundingClientRect|m|0;"
    "Element.prototype|getElementsByTagName|m|1;"
    "Element.prototype|getElementsByClassName|m|1;"
    "Element.prototype|insertAdjacentHTML|m|2;"
    "Element.prototype|insertAdjacentElement|m|2;"
    "Element.prototype|insertAdjacentText|m|2;"
    "Element.prototype|attachShadow|m|1;Element.prototype|animate|m|1;"
    "el|remove|m|0;el|append|m|0;"
    "el|prepend|m|0;el|before|m|0;"
    "el|after|m|0;el|replaceWith|m|0;"
    "el|scrollIntoView|m|0;"
    "Element.prototype|outerHTML|g|0;Element.prototype|outerHTML|s|1;"
    "el|innerHTML|g|0;el|innerHTML|s|1;"
    "el|innerText|g|0;el|innerText|s|1;"
    "Document.prototype|links|g|0;Document.prototype|anchors|g|0;"
    "Document.prototype|scrollingElement|g|0;"
    "HTMLElement.prototype|click|m|0;HTMLElement.prototype|focus|m|0;"
    "HTMLElement.prototype|blur|m|0;"
    "HTMLElement.prototype|onclick|g|0;HTMLElement.prototype|onclick|s|1;"
    "Document.prototype|onclick|g|0;Window.prototype|onload|g|0;"
    "Window.prototype|onload|s|1;"
    "document|createElement|m|1;document|createElementNS|m|2;"
    "document|createTextNode|m|1;document|createComment|m|1;"
    "document|createDocumentFragment|m|0;document|createEvent|m|1;"
    "document|createRange|m|0;document|getElementById|m|1;"
    "document|getElementsByTagName|m|1;"
    "document|getElementsByClassName|m|1;"
    "document|getElementsByName|m|1;document|querySelector|m|1;"
    "document|querySelectorAll|m|1;document|importNode|m|1;"
    "document|adoptNode|m|1;document|elementFromPoint|m|2;"
    "document|elementsFromPoint|m|2;document|hasFocus|m|0;"
    "document|getSelection|m|0;document|execCommand|m|1;"
    "document|write|m|0;document|writeln|m|0;"
    "document|createAttribute|m|1;"
    "document|title|g|0;document|title|s|1;document|cookie|g|0;"
    "document|cookie|s|1;document|body|g|0;document|head|g|0;"
    "document|readyState|g|0;document|visibilityState|g|0;"
    "document|hidden|g|0;document|documentElement|g|0;document|URL|g|0;"
    "document|currentScript|g|0;document|activeElement|g|0;"
    "Event.prototype|preventDefault|m|0;"
    "Event.prototype|stopPropagation|m|0;"
    "Event.prototype|stopImmediatePropagation|m|0;"
    "Event.prototype|composedPath|m|0;Event.prototype|type|g|0;"
    "Event.prototype|target|g|0;Event.prototype|currentTarget|g|0;"
    "Event.prototype|defaultPrevented|g|0;Event.prototype|timeStamp|g|0;"
    "console|log|m|0;console|warn|m|0;console|error|m|0;console|info|m|0;"
    "console|debug|m|0;"
    "location|assign|m|1;location|replace|m|1;location|reload|m|0;"
    "location|toString|m|0;location|href|g|0;location|href|s|1;"
    "location|origin|g|0;location|hash|g|0;location|hash|s|1;"
    "location|pathname|g|0;location|search|s|1;window|location|g|0;"
    "window|location|s|1;"
    "history|pushState|m|2;history|replaceState|m|2;history|back|m|0;"
    "history|forward|m|0;history|go|m|0;"
    "XMLHttpRequest.prototype|open|m|2;XMLHttpRequest.prototype|send|m|0;"
    "XMLHttpRequest.prototype|setRequestHeader|m|2;"
    "XMLHttpRequest.prototype|abort|m|0;"
    "Function.prototype|toString|m|0;"
    "HTMLCanvasElement.prototype|getContext|m|1;"
    "HTMLCanvasElement.prototype|toDataURL|m|0"
    "`.split(';');"
    "const toSource=Function.prototype.toString,failures=[];"
    "const lookup=(base,key,kind)=>{if(kind==='m')return base[key];"
    "for(let at=base;at;at=Object.getPrototypeOf(at)){"
    "const d=Object.getOwnPropertyDescriptor(at,key);"
    "if(d)return kind==='g'?d.get:d.set;}};"
    "for(const row of rows){const [holder,key,kind,length]=row.split('|');"
    "const base=holder==='window'?window:(0,eval)(holder),"
    "fn=lookup(base,key,kind),"
    "name=kind==='m'?key:(kind==='g'?'get ':'set ')+key;"
    "if(typeof fn!=='function'){failures.push(holder+'.'+name+':missing');continue;}"
    "const text=toSource.call(fn),problems=[];"
    "if(fn.name!==name)problems.push('name='+JSON.stringify(fn.name));"
    "if(fn.length!==Number(length))problems.push('length='+fn.length);"
    "if(text!=='function '+name+'() {\\n    [native code]\\n}')"
    "problems.push('source='+JSON.stringify(text.slice(0,40)));"
    "if(problems.length)failures.push(holder+'.'+name+':'+problems.join(','));}"
    "const timers=[setTimeout,setInterval,clearTimeout,clearInterval,"
    "requestAnimationFrame,cancelAnimationFrame,requestIdleCallback,"
    "cancelIdleCallback];"
    "if(new Set(timers).size!==timers.length)failures.push('shared timer functions');"
    "globalThis.pocSummary=failures.length?failures.length+' FAIL '"
    "+failures.join(' | '):'FUNCTION-SHAPES-OK';";

static int test_function_shapes(void)
{
    puts("test: WebIDL function names, lengths and native source");
    CHECK(run_probe("function-shapes", function_shape_probe,
                    "FUNCTION-SHAPES-OK"));
    return 0;
}

/* ---- 3. Document members live on Document.prototype ---- */

static const char document_surface_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const has=(o,k)=>Object.prototype.hasOwnProperty.call(o,k);"
    "expect('own names '+Object.getOwnPropertyNames(document).join(),"
    "JSON.stringify(Object.getOwnPropertyNames(document))==='[\"location\"]'"
    "&&Reflect.ownKeys(document).length===1);"
    "const loc=Object.getOwnPropertyDescriptor(document,'location');"
    "expect('location accessor',loc&&typeof loc.get==='function'"
    "&&typeof loc.set==='function'&&loc.enumerable&&!loc.configurable"
    "&&loc.get.name==='get location'&&document.location===location);"
    "for(const k of ['createElement','createElementNS','createTextNode',"
    "'createComment','createDocumentFragment','createEvent','createRange',"
    "'createAttribute','getElementById','getElementsByTagName',"
    "'getElementsByClassName','getElementsByName','querySelector',"
    "'querySelectorAll','adoptNode','importNode','hasFocus','getSelection',"
    "'execCommand','elementFromPoint','elementsFromPoint','exitFullscreen',"
    "'body','head','documentElement','title','cookie','readyState',"
    "'visibilityState','hidden','currentScript','activeElement','URL',"
    "'documentURI','compatMode','contentType','doctype','forms','images',"
    "'scripts','styleSheets','lang','dir','nodeType','nodeName',"
    "'links','anchors','scrollingElement',"
    "'defaultView','dispatchEvent'])"
    "expect('on Document.prototype: '+k,has(Document.prototype,k));"
    "expect('identity',document.createElement===Document.prototype.createElement"
    "&&document.getElementById===Document.prototype.getElementById"
    "&&document.addEventListener===EventTarget.prototype.addEventListener"
    "&&document.removeEventListener===EventTarget.prototype.removeEventListener"
    "&&window.addEventListener===EventTarget.prototype.addEventListener"
    "&&!has(window,'addEventListener')&&!has(Node.prototype,'addEventListener'));"
    "expect('chain',document instanceof HTMLDocument&&document instanceof Document"
    "&&document instanceof Node&&document instanceof EventTarget"
    "&&Object.getPrototypeOf(document)===HTMLDocument.prototype"
    "&&Object.getPrototypeOf(HTMLDocument.prototype)===Document.prototype"
    "&&Object.getPrototypeOf(Document.prototype)===Node.prototype"
    "&&Object.getPrototypeOf(Node.prototype)===EventTarget.prototype);"
    "expect('no internals',!['bodyText','nodeCount','onDOMContentLoaded',"
    "'__activeElement','__tilefinchBodyValue','__tilefinchSelectionChanged',"
    "'__tilefinchCustomElementRegistry'].some(k=>k in document)"
    "&&!Object.getOwnPropertyNames(Document.prototype).some(k=>k[0]==='_'));"
    "const d=Object.getOwnPropertyDescriptor(Document.prototype,'readyState');"
    "expect('accessor shapes',d.get.name==='get readyState'&&!d.set"
    "&&d.enumerable&&d.configurable&&document.nodeType===9"
    "&&document.nodeName==='#document'&&document.defaultView===window"
    "&&document.body.tagName==='BODY'&&document.title==='probe'"
    "&&typeof document.readyState==='string');"
    "document.title='renamed';expect('title setter',document.title==='renamed');"
    "let docEvents=0;const listener=()=>docEvents++;"
    "document.addEventListener('probe',listener);"
    "document.dispatchEvent(new Event('probe'));"
    "document.getElementById('p').dispatchEvent(new Event('probe',{bubbles:true}));"
    "document.removeEventListener('probe',listener);"
    "document.dispatchEvent(new Event('probe'));"
    "expect('document listeners '+docEvents,docEvents===2);"
    /* window: EventTarget members are inherited, origin is [Replaceable]. */
    "let windowEvents=0;addEventListener('probe',()=>windowEvents++);"
    "const windowDispatch=window.dispatchEvent(new Event('probe'));"
    "expect('window events',windowEvents===1&&windowDispatch===true"
    "&&window.dispatchEvent===EventTarget.prototype.dispatchEvent"
    "&&!has(window,'dispatchEvent')&&!has(window,'removeEventListener'));"
    "expect('window origin',window.origin===location.origin"
    "&&window.origin==='https://conformance.test');"
    /* Secondary documents keep their own members: a parsed document's
       listeners and queries do not reach the main document. */
    "const parsed=new DOMParser().parseFromString("
    "'<p class=a>one</p><p>two</p>','text/html');"
    "let parsedEvents=0;parsed.addEventListener('probe',()=>parsedEvents++);"
    "document.dispatchEvent(new Event('probe'));"
    "expect('parsed isolation',parsedEvents===0&&parsed.body!==document.body"
    "&&parsed.getElementsByTagName('p').length===2"
    "&&parsed instanceof Document&&parsed.nodeType===9);"
    "const made=document.implementation.createHTMLDocument('made');"
    "expect('created document',made.title==='made'&&made!==document"
    "&&made.body!==document.body&&made instanceof HTMLDocument"
    "&&made.getElementsByTagName('title').length===1);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'DOCUMENT-SURFACE-OK';";

static int test_document_surface(void)
{
    puts("test: document members live on Document.prototype");
    CHECK(run_probe("document-surface", document_surface_probe,
                    "DOCUMENT-SURFACE-OK"));
    return 0;
}

/* ---- 4. Location changes when navigation commits ---- */

static const char location_cross_document_probe[] =
    "{const start=location.href,failures=[],"
    "expect=(name,ok)=>{if(!ok)failures.push(name+'='+location.href)};"
    "location.replace('/replaced');"
    "expect('replace',location.href===start&&document.URL===start);"
    "location.assign('/assigned');expect('assign',location.href===start);"
    "location.href='/href-set';expect('href',location.href===start);"
    "location.pathname='/path-set';"
    "expect('pathname',location.pathname==='/start');"
    "location.search='?q=2';expect('search',location.search==='?a=1');"
    "window.location='/window-set';expect('window',location.href===start);"
    "document.location='/doc-set';expect('document',location.href===start);"
    "const names=Object.getOwnPropertyNames(location).sort().join();"
    "expect('own members '+names,names==='assign,hash,host,hostname,href,"
    "origin,pathname,port,protocol,reload,replace,search,toString');"
    "expect('unforgeable',Object.getOwnPropertyNames(location).every(k=>"
    "!Object.getOwnPropertyDescriptor(location,k).configurable));"
    "expect('brand',location instanceof Location&&!(location instanceof URL)"
    "&&Object.prototype.toString.call(location)==='[object Location]'"
    "&&!('searchParams' in location)&&!('assign' in URL.prototype)"
    "&&String(location)===start);"
    "let constructed=false;try{new Location()}catch(e){constructed=e instanceof TypeError}"
    "expect('illegal constructor',constructed);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | '):'CROSS-OK';}";

static const char location_same_document_probe[] =
    "{const start=location.href,failures=[],"
    "expect=(name,ok)=>{if(!ok)failures.push(name+'='+location.href)};"
    "location.hash='frag';"
    "expect('hash',location.href===start+'#frag'&&location.hash==='#frag'"
    "&&document.URL===location.href);"
    "location.href='#two';expect('href fragment',location.hash==='#two');"
    "location.assign(start+'#three');"
    "expect('assign fragment',location.hash==='#three');"
    "history.pushState(null,'','/pushed?z=1');"
    "expect('pushState',location.pathname==='/pushed'&&location.search==='?z=1');"
    "location.replace('#four');"
    "expect('replace fragment',location.href==="
    "'https://conformance.test/pushed?z=1#four');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | '):'SAME-OK';}";

static int run_location_stage(ScriptRuntime *runtime, const char *source,
                              const char *expected_summary,
                              const char *expected_url, bool expected_replace)
{
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    bool evaluated = script_runtime_evaluate_diagnostic(
        runtime, source, "<location-probe>", &result);
    if (!evaluated || strcmp(result.summary, expected_summary) != 0) {
        fprintf(stderr, "location probe: evaluated=%d summary=%s error=%s\n",
                evaluated, result.summary, result.error);
        return 1;
    }
    char url[2048];
    bool replace = false, activated = false;
    CHECK(script_runtime_consume_navigation(runtime, url, sizeof(url),
                                            &replace, &activated));
    if (strcmp(url, expected_url) != 0 || replace != expected_replace) {
        fprintf(stderr, "location probe: navigation url=%s replace=%d\n",
                url, replace);
        return 1;
    }
    return 0;
}

static int test_location_navigation_timing(void)
{
    puts("test: location keeps its URL until a cross-document navigation commits");
    static const char page[] =
        "<!doctype html><html><head><title>probe</title></head>"
        "<body><p id=p>text</p></body></html>";
    Budget budget;
    budget_init(&budget, 16 * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    CHECK(document_parse(&document, &budget, page, strlen(page), 17));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_deferred(
        &document, &budget, 8u * MIB, 3000,
        "https://conformance.test/start?a=1", NULL, &result);
    CHECK(runtime != NULL);
    /* The host acts on the last request; the old document's location must
       not have moved for any of them. */
    int failed = run_location_stage(runtime, location_cross_document_probe,
                                    "CROSS-OK",
                                    "https://conformance.test/doc-set", false);
    /* Fragment navigations and history entries stay same-document and
       update the URL synchronously. */
    if (failed == 0)
        failed = run_location_stage(
            runtime, location_same_document_probe, "SAME-OK",
            "https://conformance.test/pushed?z=1#four", true);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(failed == 0 && budget.current == 0);
    return 0;
}

/* ---- 5. Window child navigables: length, indexed and named access ---- */

/* Every <iframe>/<frame> in the document's tree has a child navigable as
   soon as it is connected, loaded or not and displayed or not; template
   contents and shadow trees do not count. A consent-management locator
   stub finds the frame it inserted by name and does not insert another. */
static const char child_navigables_body[] =
    "<iframe name=parsed></iframe>"
    "<div style=display:none><iframe name=hiddenparsed></iframe></div>"
    "<template><iframe name=intemplate></iframe></template>";

static const char child_navigables_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "expect('frames is window',window.frames===window&&frames===self);"
    "const ld=Object.getOwnPropertyDescriptor(window,'length');"
    "expect('length accessor',ld&&typeof ld.get==='function'"
    "&&ld.get.name==='get length'&&typeof ld.set==='function'"
    "&&ld.enumerable&&ld.configurable);"
    "const parsed=document.querySelectorAll('iframe');"
    "expect('parsed '+length,length===2&&frames.length===2"
    "&&window[0]===parsed[0].contentWindow&&window.parsed===window[0]"
    "&&window[1]===parsed[1].contentWindow&&window[1].name==='hiddenparsed'"
    "&&window.hiddenparsed===window[1]&&window.intemplate===undefined"
    "&&!('intemplate' in window)&&window[2]===undefined&&!(2 in window));"
    "const before=length;"
    "function tt(e){if(!window.frames[e]){if(document.body){"
    "const t=document.createElement('iframe');t.style.cssText='display:none';"
    "t.name=e;document.body.appendChild(t)}return!0}return!1}"
    "const first=tt('__tcfapiLocator'),second=tt('__tcfapiLocator');"
    "expect('stub inserts once',first===true&&second===false"
    "&&document.querySelectorAll('iframe[name=__tcfapiLocator]').length===1);"
    "const loc=document.querySelector('iframe[name=__tcfapiLocator]'),"
    "w=loc.contentWindow;"
    "expect('named',typeof window.frames.__tcfapiLocator==='object'"
    "&&typeof window.__tcfapiLocator==='object'&&window.__tcfapiLocator===w"
    "&&frames['__tcfapiLocator']===w&&'__tcfapiLocator' in window);"
    "expect('indexed',length===before+1&&window[before]===w"
    "&&before in window&&window[before+1]===undefined"
    "&&!((before+1) in window));"
    "expect('child window',w.parent===window&&w.top===window"
    "&&typeof w.postMessage==='function'&&w.closed===false"
    "&&w.name==='__tcfapiLocator'&&w.length===0&&w.frames===w"
    "&&w.window===w&&String(w.location.href)==='about:blank');"
    "let found=null;for(let f=window;f;){try{if(f.frames['__tcfapiLocator'])"
    "{found=f;break}}catch(e){}if(f===window.top)break;f=f.parent}"
    "expect('locator walk',found===window);"
    "const remote=document.createElement('iframe');"
    "remote.src='https://elsewhere.test/cmp';remote.name='remote';"
    "document.body.appendChild(remote);"
    "expect('unloaded frame',length===before+2"
    "&&window[before+1]===remote.contentWindow"
    "&&window.remote===remote.contentWindow);"
    "loc.name='renamed';"
    "expect('rename',window.renamed===w&&window.__tcfapiLocator===undefined"
    "&&!('__tcfapiLocator' in window)&&w.name==='renamed');"
    "w.name='inner';"
    "expect('child rename',window.inner===w&&loc.getAttribute('name')==="
    "'renamed');"
    "loc.setAttribute('name','renamed');"
    "expect('attribute wins',w.name==='renamed'&&window.renamed===w);"
    "const p=document.createElement('p');p.id='dup';"
    "document.body.appendChild(p);"
    "const dup=document.createElement('iframe');dup.name='dup';"
    "document.body.appendChild(dup);"
    "expect('frame before id',window.dup===dup.contentWindow);"
    "globalThis.ownName=7;const own=document.createElement('iframe');"
    "own.name='ownName';document.body.appendChild(own);"
    "expect('own property wins',window.ownName===7);"
    "const replacement=document.createElement('iframe');"
    "replacement.name='replacementName';document.body.appendChild(replacement);"
    "expect('replacement exposed',window.replacementName==="
    "replacement.contentWindow);"
    "const replacementGetter=()=>42;"
    "Object.defineProperty(window,'replacementName',{configurable:true,"
    "get:replacementGetter});replacement.remove();"
    "expect('author getter preserved',window.replacementName===42&&"
    "Object.getOwnPropertyDescriptor(window,'replacementName').get==="
    "replacementGetter);"
    "let nameReads=0;const firstFrame=parsed[0],"
    "originalGetAttribute=firstFrame.getAttribute;"
    "firstFrame.getAttribute=function(name){if(name==='name')nameReads++;"
    "return originalGetAttribute.call(this,name)};"
    "document.body.addEventListener('checklength',()=>{"
    "const stableCount=length;nameReads=0;"
    "for(let repeat=0;repeat<100;repeat++)expect('stable count',"
    "length===stableCount);"
    "expect('stable length avoids name rescans',nameReads===0);"
    "firstFrame.contentWindow.name='taskRenamed';"
    "expect('task name refresh',window.taskRenamed===firstFrame.contentWindow);"
    "firstFrame.setAttribute('name','parsed');"
    "expect('task attribute refresh',window.parsed===firstFrame.contentWindow"
    "&&window.taskRenamed===undefined);});"
    "document.body.dispatchEvent(new Event('checklength'));"
    "firstFrame.getAttribute=originalGetAttribute;"
    "const count=length;loc.remove();"
    "expect('removal',length===count-1&&window.renamed===undefined"
    "&&!('renamed' in window)&&!((count-1) in window)"
    "&&window[before]===remote.contentWindow);"
    "if(document.body.attachShadow){const host=document.createElement('div');"
    "document.body.appendChild(host);const root=host.attachShadow("
    "{mode:'open'});root.appendChild(document.createElement('iframe'));"
    "expect('shadow excluded '+length,length===count-1);}"
    "const box=document.createElement('div');"
    "box.innerHTML='<span><iframe name=deep></iframe></span>';"
    "document.body.appendChild(box);"
    "expect('subtree insert',length===count"
    "&&window.deep===box.querySelector('iframe').contentWindow);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'CHILD-NAVIGABLES-OK';";

/* A page parsed without any frame skips the frame walks
   (document_frames_impossible) until one is inserted anywhere, however it
   arrives: built detached, nested in parsed markup, cloned, or moved out of
   template contents. */
static const char frameless_navigables_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "expect('none',length===0&&window[0]===undefined);"
    "const box=document.createElement('div'),inner=document.createElement('p');"
    "const built=document.createElement('iframe');built.name='built';"
    "inner.appendChild(built);box.appendChild(inner);"
    "expect('detached',length===0);document.body.appendChild(box);"
    "expect('detached subtree',length===1&&window.built===built.contentWindow);"
    "const host=document.createElement('div');"
    "host.innerHTML='<span><iframe name=parsed></iframe></span>';"
    "document.body.appendChild(host);"
    "expect('parsed subtree',length===2&&window.parsed!==undefined);"
    "document.body.appendChild(host.cloneNode(true));"
    "expect('cloned subtree',length===3&&window[2]!==window[1]);"
    "box.remove();"
    "expect('removed subtree',length===2&&window.built===undefined);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'FRAMELESS-NAVIGABLES-OK';";

static const char frameless_template_body[] =
    "<p>no frames</p><template><div><iframe name=later></iframe></div>"
    "</template>";

static const char frameless_template_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "expect('template excluded',length===0);"
    "document.body.appendChild(document.querySelector('template').content);"
    "expect('moved from template',length===1&&window.later!==undefined);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'FRAMELESS-TEMPLATE-OK';";

static int test_child_navigables(void)
{
    puts("test: window.length, window[i] and window[name] see child frames");
    CHECK(run_probe_after("child-navigables", child_navigables_body,
                          child_navigables_probe, "CHILD-NAVIGABLES-OK"));
    CHECK(run_probe("frameless-navigables", frameless_navigables_probe,
                    "FRAMELESS-NAVIGABLES-OK"));
    CHECK(run_probe_after("frameless-template", frameless_template_body,
                          frameless_template_probe, "FRAMELESS-TEMPLATE-OK"));
    return 0;
}

/* ---- 6. Small API gaps the October 2026 site census found ---- */

/* Every element interface in the HTML standard's element table that
   Tilefinch parses: a writable, non-enumerable global constructor whose
   instanceof holds for parsed and created elements. Elements outside the
   table are HTMLUnknownElement; valid custom element names stay
   HTMLElement until defined. */
static const char element_interfaces_body[] =
    "<pre id=pre>x</pre><table><tr><td id=td>1</td><th id=th>h</th></tr>"
    "</table><data id=data value=1>d</data><span id=span>s</span>"
    "<foo id=foo></foo><x-y id=xy></x-y><section id=section></section>";

static const char element_interfaces_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const table={HTMLAreaElement:'area',HTMLBaseElement:'base',"
    "HTMLBRElement:'br',HTMLDataElement:'data',HTMLDataListElement:'datalist',"
    "HTMLDListElement:'dl',HTMLDirectoryElement:'dir',HTMLEmbedElement:'embed',"
    "HTMLFontElement:'font',HTMLFrameElement:'frame',HTMLHRElement:'hr',"
    "HTMLHeadElement:'head',HTMLHeadingElement:'h1 h2 h3 h4 h5 h6',"
    "HTMLHtmlElement:'html',HTMLLIElement:'li',HTMLMapElement:'map',"
    "HTMLMarqueeElement:'marquee',HTMLMenuElement:'menu',"
    "HTMLMeterElement:'meter',HTMLModElement:'del ins',"
    "HTMLOListElement:'ol',HTMLObjectElement:'object',"
    "HTMLOptGroupElement:'optgroup',HTMLParagraphElement:'p',"
    "HTMLParamElement:'param',HTMLPictureElement:'picture',"
    "HTMLPreElement:'pre listing xmp',HTMLProgressElement:'progress',"
    "HTMLQuoteElement:'blockquote q',HTMLSpanElement:'span',"
    "HTMLTableElement:'table',HTMLTableCaptionElement:'caption',"
    "HTMLTableCellElement:'td th',HTMLTableColElement:'col colgroup',"
    "HTMLTableRowElement:'tr',HTMLTableSectionElement:'thead tbody tfoot',"
    "HTMLTimeElement:'time',HTMLTitleElement:'title',"
    "HTMLTrackElement:'track',HTMLUListElement:'ul',"
    /* Interfaces Tilefinch already had keep working. */
    "HTMLDivElement:'div',HTMLAnchorElement:'a',HTMLImageElement:'img',"
    "HTMLInputElement:'input',HTMLScriptElement:'script',"
    "HTMLUnknownElement:'applet bgsound blink isindex keygen multicol "
    "nextid spacer foo menuitem'};"
    "for(const [name,tags] of Object.entries(table)){const C=globalThis[name];"
    "if(typeof C!=='function'){failures.push('missing '+name);continue;}"
    "const d=Object.getOwnPropertyDescriptor(globalThis,name);"
    "expect(name+' descriptor',d.writable&&d.configurable);"
    "expect(name+' shape',C.name===name&&C.prototype.constructor===C"
    "&&C.prototype instanceof HTMLElement"
    "&&Object.getPrototypeOf(C)===HTMLElement);"
    "for(const tag of tags.split(' ')){const el=document.createElement(tag);"
    "expect(name+' '+tag,el instanceof C&&el.constructor===C"
    "&&el.localName===tag);}"
    "let illegal=false;try{new C()}catch(e){illegal=e instanceof TypeError}"
    "expect(name+' illegal constructor',illegal);}"
    "for(const name of ['HTMLParagraphElement','HTMLPreElement',"
    "'HTMLTableCellElement'])"
    "expect(name+' not enumerable',"
    "!Object.getOwnPropertyDescriptor(globalThis,name)?.enumerable&&name in globalThis);"
    "const $=id=>document.getElementById(id);"
    "expect('parsed',$('pre') instanceof HTMLPreElement"
    "&&$('td') instanceof HTMLTableCellElement"
    "&&$('th') instanceof HTMLTableCellElement"
    "&&$('data') instanceof HTMLDataElement&&$('span') instanceof HTMLSpanElement"
    "&&document.querySelector('table') instanceof HTMLTableElement"
    "&&document.querySelector('tbody') instanceof HTMLTableSectionElement"
    "&&document.querySelector('tr') instanceof HTMLTableRowElement"
    "&&document.documentElement instanceof HTMLHtmlElement"
    "&&document.head instanceof HTMLHeadElement"
    "&&document.querySelector('title') instanceof HTMLTitleElement);"
    "expect('unknown',$('foo') instanceof HTMLUnknownElement"
    "&&!($('xy') instanceof HTMLUnknownElement)&&$('xy') instanceof HTMLElement"
    "&&!($('section') instanceof HTMLUnknownElement)"
    "&&$('section').constructor===HTMLElement"
    "&&!(document.createElement('font-face') instanceof HTMLElement"
    "&&!(document.createElement('font-face') instanceof HTMLUnknownElement)));"
    "expect('not cross-mapped',!($('pre') instanceof HTMLParagraphElement)"
    "&&!(document.createElement('div') instanceof HTMLParagraphElement));"
    /* Members shared by every element still reach the new interfaces. */
    "const p=document.createElement('p');p.id='made';p.textContent='x';"
    "p.classList.add('c');p.style.color='red';document.body.appendChild(p);"
    "expect('members',p.outerHTML==='<p id=\"made\" class=\"c\" style=\"color: red;\">x</p>'"
    "&&$('made')===p&&p.parentNode===document.body&&p.matches('p.c'));"
    "class Fancy extends HTMLParagraphElement{};"
    "customElements.define('fancy-p',Fancy,{extends:'p'});"
    "const fancy=document.createElement('p',{is:'fancy-p'});"
    "expect('customized built-in',fancy instanceof Fancy"
    "&&fancy instanceof HTMLParagraphElement&&fancy.localName==='p');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'ELEMENT-INTERFACES-OK';";

/* Element members: hasAttributes, replaceChildren and checkVisibility on
   Element.prototype, and the reflected attributes pages read. */
static const char element_members_body[] =
    "<p id=p>text</p><div id=hid style=\"display:none\"><span id=inhid>x"
    "</span></div><div id=op style=\"opacity:0\"><b id=opc>o</b></div>"
    "<div id=vh style=\"visibility:hidden\">v</div>"
    "<div id=dc style=\"display:contents\">c</div>"
    "<div id=cv style=\"content-visibility:hidden\"><i id=cvc>c</i></div>"
    "<div id=ok>ok</div>";

static const char element_members_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const t=(name,f)=>{try{expect(name,f())}catch(e){failures.push(name+':'+e)}};"
    "const $=id=>document.getElementById(id);"
    "t('hasAttributes',()=>{const a=document.createElement('div');"
    "const before=!a.hasAttributes();a.setAttribute('data-x','');"
    "const during=a.hasAttributes();a.removeAttribute('data-x');"
    "return typeof Element.prototype.hasAttributes==='function'&&before"
    "&&during&&!a.hasAttributes()&&$('p').hasAttributes()"
    "&&!document.body.hasAttributes()"
    "&&document.createElementNS('http://www.w3.org/2000/svg','g')"
    ".hasAttributes()===false"
    "&&!document.implementation.createHTMLDocument('d').createElement('i')"
    ".hasAttributes();});"
    "t('replaceChildren',()=>{const host=document.createElement('div');"
    "host.append('a',document.createElement('b'));"
    "host.replaceChildren('x',document.createElement('i'));"
    "const one=host.childNodes.length===2&&host.firstChild.data==='x'"
    "&&host.lastChild.localName==='i';host.replaceChildren();"
    "return typeof Element.prototype.replaceChildren==='function'"
    "&&one&&host.childNodes.length===0"
    "&&Element.prototype.replaceChildren===host.replaceChildren;});"
    "t('checkVisibility',()=>{const v=(id,o)=>$(id).checkVisibility(o);"
    "return typeof Element.prototype.checkVisibility==='function'"
    "&&v('ok')===true&&v('p')===true&&v('hid')===false&&v('inhid')===false"
    "&&v('dc')===false&&v('cv')===true&&v('cvc')===false"
    "&&v('op')===true&&v('op',{checkOpacity:true})===false"
    "&&v('opc',{opacityProperty:true})===false"
    "&&v('vh')===true&&v('vh',{checkVisibilityCSS:true})===false"
    "&&v('vh',{visibilityProperty:true})===false"
    "&&document.createElement('div').checkVisibility()===false"
    "&&document.head.checkVisibility()===false"
    "&&document.querySelector('script').checkVisibility()===false;});"
    "t('dir',()=>{const e=document.createElement('div');const a=e.dir==='';"
    "e.setAttribute('dir','RTL');const b=e.dir==='rtl';"
    "e.setAttribute('dir','sideways');const c=e.dir==='';e.dir='Auto';"
    "return a&&b&&c&&e.getAttribute('dir')==='Auto'&&e.dir==='auto'"
    "&&'dir' in HTMLElement.prototype;});"
    /* Not reflected: it is the drag-and-drop feature test, and Tilefinch
       has no drag and drop. */
    "t('no draggable',()=>!('draggable' in document.createElement('img')));"
    "t('loading',()=>{const r=[];for(const tag of ['img','iframe']){"
    "const e=document.createElement(tag);r.push(e.loading);"
    "e.setAttribute('loading','LAZY');r.push(e.loading);"
    "e.setAttribute('loading','soon');r.push(e.loading);"
    "e.loading='lazy';r.push(e.getAttribute('loading'));}"
    "return r.join()==='eager,lazy,eager,lazy,eager,lazy,eager,lazy'"
    "&&!('loading' in document.createElement('div'));});"
    "t('inputMode',()=>{const e=document.createElement('input');"
    "const a=e.inputMode==='';e.setAttribute('inputmode','NUMERIC');"
    "const b=e.inputMode==='numeric';e.setAttribute('inputmode','keypad');"
    "const c=e.inputMode==='';e.inputMode='email';"
    "return a&&b&&c&&e.getAttribute('inputmode')==='email';});"
    "t('accessKey',()=>{const e=document.createElement('input');"
    "const a=e.accessKey==='';e.accessKey='k';"
    "return a&&e.getAttribute('accesskey')==='k'&&e.accessKey==='k';});"
    "t('autofocus',()=>{const e=document.createElement('input');"
    "const a=e.autofocus===false;e.autofocus=true;"
    "const b=e.hasAttribute('autofocus')&&e.autofocus;e.autofocus=false;"
    "return a&&b&&!e.hasAttribute('autofocus')"
    "&&'autofocus' in SVGElement.prototype;});"
    "t('script.text',()=>{const s=document.createElement('script');"
    "s.text='window.scriptTextRan=1';s.appendChild(document.createComment('c'));"
    "const read=s.text==='window.scriptTextRan=1';"
    "document.head.appendChild(s);"
    "return read&&window.scriptTextRan===1"
    "&&!('text' in document.createElement('div'));});"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'ELEMENT-MEMBERS-OK';";

/* URL and URLSearchParams statics and attributes. Tilefinch's URL layer
   rejects userinfo by design (src/url.c), so username and password are
   always empty and setting them leaves the URL unchanged. */
static const char url_members_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const t=(name,f)=>{try{expect(name,f())}catch(e){failures.push(name+':'+e)}};"
    "t('size',()=>{const q=new URLSearchParams('a=1&b=2&a=3');const a=q.size===3;"
    "q.append('c','4');const b=q.size===4;q.delete('a');"
    "const u=new URL('https://example.test/?x=1&y=2');"
    "return a&&b&&q.size===2&&u.searchParams.size===2"
    "&&new URLSearchParams().size===0"
    "&&typeof Object.getOwnPropertyDescriptor(URLSearchParams.prototype,"
    "'size').get==='function';});"
    "t('userinfo',()=>{const u=new URL('https://example.test/p?q#h');"
    "u.username='who';u.password='secret';"
    "return 'username' in URL.prototype&&'password' in URL.prototype"
    "&&u.username===''&&u.password===''"
    "&&u.href==='https://example.test/p?q#h';});"
    "t('canParse',()=>URL.canParse('https://example.test/')===true"
    "&&URL.canParse('nope')===false&&URL.canParse('/x')===false"
    "&&URL.canParse('/x','https://example.test/a/')===true"
    "&&URL.canParse('x','nope')===false"
    "&&URL.canParse.length===1&&(()=>{try{URL.canParse();return false}"
    "catch(e){return e instanceof TypeError}})());"
    "t('parse',()=>{const u=URL.parse('../y','https://example.test/a/b/c');"
    "return u instanceof URL&&u.href==='https://example.test/a/y'"
    "&&URL.parse('nope')===null&&URL.parse('/x')===null"
    "&&URL.parse('https://example.test/z').pathname==='/z'"
    "&&URL.parse.length===1;});"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'URL-MEMBERS-OK';";

/* document.open: the 3-argument form is window.open; during parsing it is
   ignored (HTML's active-parser case); otherwise it empties the body and
   later write() calls fill it, as for a reopened document. */
static const char document_open_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const t=(name,f)=>{try{expect(name,f())}catch(e){failures.push(name+':'+e)}};"
    "t('shape',()=>typeof Document.prototype.open==='function'"
    "&&document.open===Document.prototype.open&&document.open.length===0);"
    "t('while parsing',()=>document.readyState==='loading'"
    "&&document.open()===document&&document.getElementById('p')!==null);"
    "t('reopen',()=>{const d=document.implementation.createHTMLDocument('d');"
    "d.body.appendChild(d.createElement('p'));"
    "d.body.appendChild(d.createTextNode('old'));"
    "const r=d.open();const empty=d.body.childNodes.length===0;"
    "d.write('<b>new</b>');d.writeln('<i>line</i>');d.close();"
    "return r===d&&empty&&d.body.childNodes.length>=2"
    "&&d.body.firstChild.localName==='b'"
    "&&d.body.childNodes[1].localName==='i';});"
    "t('xml',()=>{const x=document.implementation.createDocument(null,'r');"
    "try{x.open();return false}catch(e){return e instanceof DOMException"
    "&&e.name==='InvalidStateError'}});"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'DOCUMENT-OPEN-OK';";

/* TextEvent, privacy and lifecycle flags, explicit-resource-management
   symbols and HTMLScriptElement.supports. */
static const char platform_members_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const t=(name,f)=>{try{expect(name,f())}catch(e){failures.push(name+':'+e)}};"
    "t('TextEvent',()=>{let illegal=false;try{new TextEvent('textInput')}"
    "catch(e){illegal=e instanceof TypeError}"
    "const e=document.createEvent('TextEvent');"
    "const fresh=e instanceof TextEvent&&e instanceof UIEvent&&e.data==='';"
    "e.initTextEvent('textInput',true,true,window,'hi');let seen='';"
    "document.body.addEventListener('textInput',ev=>seen=ev.data);"
    "document.body.dispatchEvent(e);"
    "return illegal&&fresh&&e.type==='textInput'&&e.bubbles&&e.cancelable"
    "&&e.data==='hi'&&e.view===window&&seen==='hi'"
    "&&document.createEvent('textevent') instanceof TextEvent"
    "&&TextEvent.prototype.initTextEvent.length===1&&'TextEvent' in window;});"
    "t('globalPrivacyControl',()=>navigator.globalPrivacyControl===false"
    "&&typeof Object.getOwnPropertyDescriptor(Navigator.prototype,"
    "'globalPrivacyControl').get==='function');"
    "t('prerendering',()=>document.prerendering===false"
    "&&'prerendering' in Document.prototype"
    "&&!Object.prototype.hasOwnProperty.call(document,'prerendering'));"
    "t('symbols',()=>['dispose','asyncDispose','metadata'].every(k=>{"
    "const d=Object.getOwnPropertyDescriptor(Symbol,k);"
    "return d&&typeof d.value==='symbol'&&!d.writable&&!d.configurable"
    "&&!d.enumerable&&d.value.description==='Symbol.'+k"
    "&&Symbol.keyFor(d.value)===undefined;})"
    "&&Symbol.dispose!==Symbol.asyncDispose);"
    "t('SuppressedError',()=>{const e=new SuppressedError(1,2,'m');"
    "const bare=SuppressedError(3,4);"
    "return typeof SuppressedError==='function'&&SuppressedError.length===3"
    "&&e instanceof SuppressedError&&e instanceof Error&&e.error===1"
    "&&e.suppressed===2&&e.message==='m'&&e.name==='SuppressedError'"
    "&&bare instanceof SuppressedError&&bare.error===3"
    "&&!Object.prototype.hasOwnProperty.call(bare,'message')"
    "&&!Object.getOwnPropertyDescriptor(globalThis,'SuppressedError')"
    ".enumerable;});"
    "t('HTMLScriptElement.supports',()=>HTMLScriptElement.supports('classic')"
    "&&HTMLScriptElement.supports('module')"
    "&&!HTMLScriptElement.supports('importmap')"
    "&&!HTMLScriptElement.supports('speculationrules')"
    "&&!HTMLScriptElement.supports('Module')"
    "&&!HTMLScriptElement.supports('')&&HTMLScriptElement.supports.length===1);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'PLATFORM-MEMBERS-OK';";

/* Second-census gaps: document.all (HTMLAllCollection with
   [[IsHTMLDDA]]), print(), PerformanceEventTiming as
   a detectable interface with no entries, and an empty, settled
   document.fonts. */
static const char legacy_surface_body[] =
    "<p id=p>text</p><form name=f></form><img name=pic>"
    "<div id=twin></div><a id=twin></a>";

static const char legacy_surface_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const t=(name,f)=>{try{expect(name,f())}catch(e){failures.push(name+':'+e)}};"
    "t('all ddaa',()=>{const a=document.all;return typeof a==='undefined'"
    "&&!a&&a==null&&a==undefined&&a!==undefined&&a!==null"
    "&&(a?1:2)===2&&(a??0)===a&&a===document.all"
    "&&Object.prototype.toString.call(a)==='[object HTMLAllCollection]'"
    "&&'all' in document&&!Object.prototype.hasOwnProperty.call(document,"
    "'all');});"
    "t('all items',()=>{const a=document.all,"
    "tags=document.getElementsByTagName('*');"
    "return a.length===tags.length&&a[0]===document.documentElement"
    "&&a.item(1)===document.head&&a(0)===a[0]&&a('0')===a[0]"
    "&&a.item('p')===document.getElementById('p')&&a.p===a.item('p')"
    "&&a['f'].localName==='form'&&a.namedItem('pic').localName==='img'"
    "&&a.namedItem('nope')===null&&a(undefined)===null"
    "&&a.item(a.length)===null&&a[a.length]===undefined"
    "&&a.twin.length===2&&a.length===Array.from(a).length"
    "&&Object.keys(a).length===a.length&&!Object.keys(a).includes('length')"
    "&&a.length===[...a].length;});"
    "t('all live',()=>{const a=document.all,n=a.length;"
    "document.body.appendChild(document.createElement('i'));"
    "return a.length===n+1&&a[n].localName==='i';});"
    "t('all brand',()=>{const g=Object.getOwnPropertyDescriptor("
    "Document.prototype,'all').get;try{g.call({});return false}"
    "catch(e){return e instanceof TypeError&&g.call(document)===document.all"
    "}});"
    "t('all native',()=>{const p=Document.prototype,q=p.querySelectorAll,"
    "g=Element.prototype.getAttribute,D=Document;"
    "p.querySelectorAll=()=>[];Element.prototype.getAttribute=()=>null;"
    "globalThis.Document=function(){};try{const a=document.all;"
    "return a.length>4&&a[0]===document.documentElement"
    "&&a.p===document.getElementById('p')&&a.namedItem('f').localName==='form'}"
    "finally{p.querySelectorAll=q;Element.prototype.getAttribute=g;"
    "globalThis.Document=D}});"
    "t('all other document',()=>{const d=document.implementation"
    ".createHTMLDocument('d');return typeof d.all==='undefined'"
    "&&d.all!==document.all&&d.all===d.all"
    "&&d.all.length===d.getElementsByTagName('*').length;});"
    /* window.orientation stays absent (see js_runtime/legacy_surface.inc);
       screen.orientation carries the orientation. */
    "t('print, no orientation',()=>!('orientation' in window)"
    "&&!('onorientationchange' in window)&&screen.orientation.angle===0"
    "&&typeof print==='function'&&print()===undefined&&print.length===0);"
    "t('PerformanceEventTiming',()=>{let illegal=false;"
    "try{new PerformanceEventTiming()}catch(e){illegal=e instanceof TypeError}"
    "const p=PerformanceEventTiming.prototype;return illegal"
    "&&typeof PerformanceEventTiming==='function'"
    "&&Object.getPrototypeOf(p)===PerformanceEntry.prototype"
    "&&['processingStart','processingEnd','cancelable','target',"
    "'interactionId'].every(k=>k in p)"
    "&&!PerformanceObserver.supportedEntryTypes.includes('event')"
    "&&!PerformanceObserver.supportedEntryTypes.includes('first-input')"
    "&&performance.getEntriesByType('event').length===0;});"
    "t('fonts',()=>{const f=document.fonts;let parse=false,add=false;"
    "try{f.check('serif')}catch(e){parse=e instanceof DOMException"
    "&&e.name==='SyntaxError'}try{f.add({})}catch(e){add=e instanceof "
    "TypeError}return f===document.fonts&&f instanceof FontFaceSet"
    "&&f instanceof EventTarget&&f.status==='loaded'&&f.size===0"
    "&&f.check('12px serif')&&f.check('bold 1em/2 \"Some Face\", serif')"
    "&&f.check('12pt serif')&&f.check('italic small-caps 700 large/1.5 a')"
    "&&['12px','12zz serif','nonsense 12px serif'].every(v=>{"
    "try{f.check(v)}catch(e){return e.name==='SyntaxError'}return false})"
    "&&parse&&add&&f.has({})===false&&f.delete({})===false"
    "&&[...f].length===0&&f.onloadingdone===null"
    "&&(f.onloadingdone=()=>0,typeof f.onloadingdone==='function')"
    "&&Object.prototype.toString.call(f)==='[object FontFaceSet]'"
    "&&!Object.prototype.hasOwnProperty.call(document,'fonts');});"
    "globalThis.pocSummary='LEGACY-SURFACE-PENDING';"
    "Promise.all([document.fonts.ready,document.fonts.load('16px Foo'),"
    "document.fonts.load('nonsense').then(()=>false,e=>e.name==='SyntaxError')"
    "]).then(([ready,faces,rejected])=>{expect('ready',ready===document.fonts);"
    "expect('load',Array.isArray(faces)&&faces.length===0);"
    "expect('load rejects',rejected);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'LEGACY-SURFACE-OK';});";

/* Whole-document walks on a page larger than any fixed result cap: every
   native list (query, tag collection, children, document.all) must hold all
   5,000 elements, not stop at a cap below the handle table. */
static const char large_walk_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "const host=document.createElement('div');"
    "host.innerHTML='<b></b>'.repeat(5000);document.body.appendChild(host);"
    "t('querySelectorAll',()=>document.querySelectorAll('b').length===5000"
    "&&host.querySelectorAll('*').length===5000);"
    "t('getElementsByTagName',()=>"
    "document.getElementsByTagName('b').length===5000);"
    "t('children',()=>host.children.length===5000"
    "&&host.childNodes.length===5000&&host.lastChild===host.children[4999]);"
    "t('all',()=>document.all.length>5000&&document.all.length"
    "===document.getElementsByTagName('*').length"
    "&&document.all[document.all.length-1]===host.lastChild);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'LARGE-WALKS-OK';";

/* A form-associated custom element disabled through its own attribute or a
   disabled fieldset is not focusable, like a native control, even when its
   class defines a disabled accessor that only reflects the attribute. */
static const char face_focus_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "customElements.define('x-face',class extends HTMLElement{"
    "static formAssociated=true;constructor(){super();this.attachInternals()}"
    "get disabled(){return this.hasAttribute('disabled')}});"
    "const set=document.createElement('fieldset'),"
    "face=document.createElement('x-face'),own=document.createElement('x-face');"
    "face.tabIndex=0;own.tabIndex=0;own.setAttribute('disabled','');"
    "set.disabled=true;set.appendChild(face);"
    "document.body.append(set,own);"
    "face.focus();expect('fieldset disabled',document.activeElement!==face);"
    "own.focus();expect('own disabled',document.activeElement!==own);"
    "set.disabled=false;face.focus();"
    "expect('enabled',document.activeElement===face);"
    "expect('one focus',HTMLElement.prototype.focus===Element.prototype.focus);"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'FACE-FOCUS-OK';";

/* sendBeacon() returns true only for a beacon the network queue admitted:
   it measures the request the way the queue does (URL and headers too),
   not by its body alone. Native starts are held pending so the first
   request keeps its bytes queued. */
static const char beacon_admission_probe[] =
    "const real=__tilefinchFetchAsync;let next=1000;"
    "globalThis.__tilefinchFetchAsync=()=>++next;"
    "const s=__tilefinchNetworkQueueStats;"
    "fetch('/hold',{method:'POST',body:new Uint8Array(40000)}).catch(()=>{});"
    "const remaining=96*1024-s.currentBytes,before=s.admitted,"
    "sent=navigator.sendBeacon('/beacon-with-a-long-path',"
    "new Uint8Array(remaining-8)),admitted=s.admitted-before,"
    "fits=navigator.sendBeacon('/b',new Uint8Array(64)),"
    "admittedSmall=s.admitted-before-admitted;"
    "globalThis.__tilefinchFetchAsync=real;"
    "globalThis.pocSummary=!sent&&admitted===0&&fits&&admittedSmall===1"
    "?'BEACON-ADMISSION-OK':['BEACON',sent,admitted,fits,admittedSmall]"
    ".join(':');";

/* Fetch's "request-no-cors" guard: a no-cors Request drops headers that
   are not no-CORS-safelisted (the body then supplies text/plain), and only
   GET, HEAD and POST are allowed. */
static const char no_cors_request_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "t('dropped',()=>{const r=new Request('https://other.test/x',"
    "{mode:'no-cors',method:'POST',body:'{}',headers:{"
    "'Content-Type':'application/json','X-Probe':'1','Accept':'text/html',"
    "'Range':'bytes=0-1'}});"
    "r.headers.append('x-late','1');r.headers.set('content-language','en');"
    "return r.headers.get('content-type')==='text/plain;charset=UTF-8'"
    "&&!r.headers.has('x-probe')&&!r.headers.has('range')"
    "&&!r.headers.has('x-late')&&r.headers.get('accept')==='text/html'"
    "&&r.headers.get('content-language')==='en'"
    "&&new Request(r).headers.has('x-probe')===false});"
    "t('cors keeps',()=>new Request('/x',{headers:{'X-Probe':'1'}})"
    ".headers.get('x-probe')==='1');"
    "t('method',()=>{try{new Request('/same',{mode:'no-cors',"
    "method:'PUT'})}catch(e){return e instanceof TypeError}return false});"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'NO-CORS-REQUEST-OK';";

/* A constructed sheet splits its text into rules with the native scanner
   that bounds <link> sheets: a ';' inside parentheses does not end a
   statement, a stray top-level '}' joins the next rule's prelude, and an
   unclosed final block is closed. */
static const char constructed_split_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "const texts=css=>{const s=new CSSStyleSheet();s.replaceSync(css);"
    "return [...s.cssRules].map(r=>r.cssText).join('|')};"
    "t('parentheses',()=>texts('@media (min-width:1px;) {.a{color:red}}"
    " .b{color:blue}')==='@media (min-width:1px;) {.a{color:red}}|"
    ".b{color:blue}');"
    "t('stray brace',()=>texts('} .a{color:red} .b{color:blue}')==="
    "'} .a{color:red}|.b{color:blue}');"
    "t('unclosed',()=>texts('.a{color:red} .b{color:blue  ')==="
    "'.a{color:red}|.b{color:blue}');"
    "t('bridges deleted',()=>!['__tilefinchCssStatementEnds',"
    "'__tilefinchConstructedSheetText','__tilefinchSetAdoptedSheets']"
    ".some(name=>name in globalThis));"
    "t('wide text',()=>texts('.\\u00e9{content:\"\\ud83d\\ude00;\"}"
    ".c{color:red}')==='.\\u00e9{content:\"\\ud83d\\ude00;\"}|"
    ".c{color:red}');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'CONSTRUCTED-SPLIT-OK';";

/* Reflected attributes share one definition: every one is an enumerable
   WebIDL member whose accessors carry WebIDL names, whichever bootstrap
   module installs it. */
static const char reflected_shape_probe[] =
    "const failures=[];"
    "for(const [owner,name] of [[HTMLElement,'title'],[HTMLElement,'dir'],"
    "[HTMLElement,'autofocus'],[HTMLScriptElement,'noModule'],"
    "[HTMLInputElement,'readOnly'],[HTMLFormElement,'noValidate'],"
    "[HTMLFormElement,'method'],[HTMLMetaElement,'httpEquiv'],"
    "[HTMLMediaElement,'playsInline'],[HTMLImageElement,'srcset']]){"
    "const d=Object.getOwnPropertyDescriptor(owner.prototype,name);"
    "if(!d||!d.enumerable||!d.configurable||d.get.name!=='get '+name"
    "||d.set.name!=='set '+name)failures.push(owner.name+'.'+name)}"
    "const meta=document.createElement('meta'),form=document.createElement('form');"
    "meta.httpEquiv='refresh';form.setAttribute('method','POST');"
    "if(meta.getAttribute('http-equiv')!=='refresh'||form.method!=='post'"
    "||document.createElement('form').method!=='get')failures.push('values');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'REFLECTED-SHAPE-OK';";

/* TextDecoder decodes every single-byte encoding of the Encoding Standard
   with the tables the document parser uses, by any of its labels. */
static const char text_decoder_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "const bytes=a=>new Uint8Array(a);"
    "t('koi8-r',()=>{const d=new TextDecoder(' KOI8-R ');"
    "return d.encoding==='koi8-r'&&d.decode(bytes([0xc1,0xc2]))==='\u0430\u0431'});"
    "t('windows-1251',()=>new TextDecoder('cp1251').decode(bytes([0xc0,0x41]))"
    "==='\u0410A');"
    "t('ibm866',()=>new TextDecoder('866').encoding==='ibm866');"
    "t('x-user-defined',()=>new TextDecoder('x-user-defined')"
    ".decode(bytes([0x41,0x80]))==='A\uf780');"
    "t('fatal',()=>{try{new TextDecoder('windows-874',{fatal:true})"
    ".decode(bytes([0xdb]))}catch(e){return e instanceof TypeError}return false});"
    "t('replacing',()=>new TextDecoder('windows-874').decode(bytes([0xdb]))"
    "==='\ufffd');"
    "t('unknown',()=>{try{new TextDecoder('nonsense')}catch(e){"
    "return e instanceof RangeError}return false});"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'TEXT-DECODER-OK';";

static int test_census_api_gaps(void)
{
    puts("test: census API gaps (interfaces, members, URL, open, platform)");
    int failed = 0;
    if (!run_probe_after("element-interfaces", element_interfaces_body,
                         element_interfaces_probe, "ELEMENT-INTERFACES-OK"))
        failed++;
    if (!run_probe_after("element-members", element_members_body,
                         element_members_probe, "ELEMENT-MEMBERS-OK"))
        failed++;
    if (!run_probe("url-members", url_members_probe, "URL-MEMBERS-OK"))
        failed++;
    if (!run_probe("document-open", document_open_probe, "DOCUMENT-OPEN-OK"))
        failed++;
    if (!run_probe("platform-members", platform_members_probe,
                   "PLATFORM-MEMBERS-OK"))
        failed++;
    if (!run_probe_after("legacy-surface", legacy_surface_body,
                         legacy_surface_probe, "LEGACY-SURFACE-OK"))
        failed++;
    if (!run_probe("large-walks", large_walk_probe, "LARGE-WALKS-OK"))
        failed++;
    if (!run_probe("face-focus", face_focus_probe, "FACE-FOCUS-OK"))
        failed++;
    if (!run_probe("beacon-admission", beacon_admission_probe,
                   "BEACON-ADMISSION-OK"))
        failed++;
    if (!run_probe("no-cors-request", no_cors_request_probe,
                   "NO-CORS-REQUEST-OK"))
        failed++;
    if (!run_probe("constructed-split", constructed_split_probe,
                   "CONSTRUCTED-SPLIT-OK"))
        failed++;
    if (!run_probe("reflected-shape", reflected_shape_probe,
                   "REFLECTED-SHAPE-OK"))
        failed++;
    if (!run_probe("text-decoder", text_decoder_probe, "TEXT-DECODER-OK"))
        failed++;
    CHECK(failed == 0);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failures = 0;
    failures += test_time_zone_agreement();
    failures += test_function_shapes();
    failures += test_document_surface();
    failures += test_location_navigation_timing();
    failures += test_child_navigables();
    failures += test_census_api_gaps();
    if (failures != 0) return 1;
    puts("web conformance tests passed");
    return 0;
}
