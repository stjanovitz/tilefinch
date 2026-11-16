/* Web-standards conformance probes for surface that page scripts (and
   fingerprinting/anti-bot scripts in particular) can observe directly:
   time-zone agreement between Date and Intl, WebIDL function names and
   lengths, where document members live, when location changes, and the
   Window's child navigables. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/session.h"

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
static bool run_probe_at(const char *label, const char *url,
                         const char *body, const char *script,
                         const char *expected)
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
                                       url, &result);
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

static bool run_probe_after(const char *label, const char *body,
                            const char *script, const char *expected)
{
    return run_probe_at(label, "https://conformance.test/start", body,
                        script, expected);
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
    "t('loading',()=>{const r=[];for(const tag of ['img','iframe']){"
    "const e=document.createElement(tag);r.push(e.loading);"
    "e.setAttribute('loading','LAZY');r.push(e.loading);"
    "e.setAttribute('loading','soon');r.push(e.loading);"
    "e.loading='lazy';r.push(e.getAttribute('loading'));"
    "e.setAttribute('loading','Auto');r.push(e.loading);}"
    "return r.join()==='auto,lazy,auto,lazy,auto,auto,lazy,auto,lazy,auto'"
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
    "&&HTMLScriptElement.supports('importmap')"
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

/* An absolute URL ignores its base, so an opaque base (blob:, data:,
   about:blank) cannot reject it; relative input against one fails. */
static const char url_opaque_base_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "const throws=(i,b)=>{try{new URL(i,b)}catch(e){return e instanceof TypeError}"
    "return false};"
    "t('blob',()=>new URL('https://example.com/a.js',"
    "'blob:https://example.com/uuid').href==='https://example.com/a.js');"
    "t('data',()=>new URL('https://example.com/b?x#y','data:text/plain,hi')"
    ".href==='https://example.com/b?x#y');"
    "t('about',()=>new URL('http://example.com/c','about:blank').href"
    "==='http://example.com/c');"
    "t('blob relative',()=>throws('a.js','blob:https://example.com/uuid'));"
    "t('blob rooted',()=>throws('/a.js','blob:https://example.com/uuid'));"
    "t('about relative',()=>throws('a.js','about:blank'));"
    "t('data relative',()=>throws('?q','data:text/plain,hi'));"
    "t('invalid base',()=>throws('https://example.com/','nope'));"
    "t('canParse',()=>URL.canParse('https://example.com/',"
    "'blob:https://example.com/u')&&!URL.canParse('x','blob:https://example.com/u'));"
    "t('relative',()=>new URL('../d','https://example.com/a/b/c').href"
    "==='https://example.com/a/d');"
    "t('hierarchical base',()=>new URL('a/../b?q','x:/').pathname==='/b');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'URL-OPAQUE-BASE-OK';";

/* A document whose own URL is not a network URL (a fixture file path, as
   psp-browser-lab --fixture runs) still resolves relative input against its
   fallback base, through location.href and document.baseURI too, while an
   opaque base passed by script rejects it. */
static const char url_document_base_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "t('implicit',()=>new URL('a/../b.js').pathname.endsWith('/b.js'));"
    "t('location',()=>/^https?:/.test(location.href)"
    "&&new URL('c.js',location.href).pathname.endsWith('/c.js'));"
    "t('baseURI',()=>new URL('d.js',document.baseURI).pathname.endsWith('/d.js'));"
    "t('explicit opaque',()=>{try{new URL('a.js','about:blank')}catch(e){"
    "return e instanceof TypeError}return false});"
    "t('absolute',()=>new URL('https://example.com/x','about:blank').href"
    "==='https://example.com/x');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'URL-DOCUMENT-BASE-OK';";

/* TextDecoder decodes UTF-16LE and UTF-16BE by every Encoding Standard
   label, with BOM handling, fatal mode and streamed partial units. */
static const char text_decoder_utf16_probe[] =
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"
    "catch(e){failures.push(name+':'+e)}};"
    "const bytes=a=>new Uint8Array(a),"
    "d=(l,a,o)=>new TextDecoder(l,o).decode(bytes(a)),"
    "fatal=(l,a)=>{try{d(l,a,{fatal:true})}catch(e){return e instanceof TypeError}"
    "return false},emoji='\\ud83d\\ude00';"
    "t('le',()=>d('utf-16le',[0x41,0,0x3d,0xd8,0,0xde,0xe9,0])==='A'+emoji+'\\u00e9');"
    "t('be',()=>d('utf-16be',[0,0x41,0xd8,0x3d,0xde,0,0x4e,0x2d])==='A'+emoji+'\\u4e2d');"
    "t('le bom',()=>d('utf-16le',[0xff,0xfe,0x41,0])==='A');"
    "t('be bom',()=>d('utf-16be',[0xfe,0xff,0,0x41])==='A');"
    "t('ignoreBOM',()=>{const x=new TextDecoder('utf-16le',{ignoreBOM:true});"
    "return x.ignoreBOM&&x.decode(bytes([0xff,0xfe,0x41,0]))==='\\ufeffA'});"
    "t('other bom kept',()=>d('utf-16le',[0xfe,0xff,0x41,0])==='\\ufffeA');"
    "t('bom per decode',()=>{const x=new TextDecoder('utf-16be');"
    "return x.decode(bytes([0xfe,0xff,0,0x41]))==='A'"
    "&&x.decode(bytes([0xfe,0xff,0,0x42]))==='B'});"
    "t('lone',()=>d('utf-16le',[0,0xd8,0x41,0])==='\\ufffdA'"
    "&&d('utf-16le',[0,0xdc,0x41,0])==='\\ufffdA'"
    "&&d('utf-16be',[0xd8,0])==='\\ufffd'&&d('utf-16le',[0x41,0,0x42])==='A\\ufffd');"
    "t('fatal lone',()=>fatal('utf-16le',[0,0xd8,0x41,0])"
    "&&fatal('utf-16be',[0xdc,0,0,0x41])&&fatal('utf-16le',[0x3d,0xd8]));"
    "t('fatal odd',()=>fatal('utf-16le',[0x41,0,0x42]));"
    "t('fatal ok',()=>d('utf-16be',[0xd8,0x3d,0xde,0,0xff,0xfd],{fatal:true})"
    "===emoji+'\\ufffd');"
    "t('stream odd',()=>{const x=new TextDecoder('utf-16le');"
    "return x.decode(bytes([0x41]),{stream:true})===''"
    "&&x.decode(bytes([0,0x42]),{stream:true})==='A'&&x.decode(bytes([0]))==='B'});"
    "t('stream pair',()=>{const x=new TextDecoder('utf-16be',{fatal:true});"
    "return x.decode(bytes([0xd8,0x3d]),{stream:true})===''"
    "&&x.decode(bytes([0xde,0]))===emoji});"
    "t('stream pair odd',()=>{const x=new TextDecoder('utf-16le');"
    "return x.decode(bytes([0x3d]),{stream:true})===''"
    "&&x.decode(bytes([0xd8,0]),{stream:true})===''"
    "&&x.decode(bytes([0xde]),{stream:true})===emoji&&x.decode()===''});"
    "t('stream bom',()=>{const x=new TextDecoder('utf-16le');"
    "return x.decode(bytes([0xff]),{stream:true})===''"
    "&&x.decode(bytes([0xfe,0x41,0]),{stream:true})==='A'"
    "&&x.decode(bytes([0xff,0xfe]))==='\\ufeff'"
    "&&x.decode(bytes([0xff,0xfe,0x42,0]))==='B'});"
    "t('stream flush',()=>{const x=new TextDecoder('utf-16le');"
    "x.decode(bytes([0x3d,0xd8]),{stream:true});return x.decode()==='\\ufffd'});"
    "t('stream fatal flush',()=>{const x=new TextDecoder('utf-16le',{fatal:true});"
    "x.decode(bytes([0x41]),{stream:true});try{x.decode()}catch(e){"
    "return e instanceof TypeError&&x.decode(bytes([0x42,0]))==='B'}return false});"
    "t('labels',()=>[['utf-16','utf-16le'],[' UTF-16LE ','utf-16le'],"
    "['unicode','utf-16le'],['ucs-2','utf-16le'],['csunicode','utf-16le'],"
    "['iso-10646-ucs-2','utf-16le'],['unicodefeff','utf-16le'],"
    "['utf-16be','utf-16be'],['unicodefffe','utf-16be']]"
    ".every(([l,n])=>new TextDecoder(l).encoding===n));"
    "t('views',()=>{const b=new Uint8Array([9,0x41,0,0x42,0]);"
    "return new TextDecoder('utf-16le').decode(b.subarray(1))==='AB'"
    "&&new TextDecoder('utf-16le').decode(new DataView(b.buffer,1,2))==='A'"
    "&&new TextDecoder('utf-16le').decode(b.buffer.slice(1))==='AB'});"
    "t('large',()=>{const n=300000,a=new Uint8Array(n*2);"
    "for(let i=0;i<n;i++)a[2*i]=0x61+i%26;"
    "const s=new TextDecoder('utf-16le').decode(a);"
    "return s.length===n&&s.charCodeAt(n-1)===0x61+(n-1)%26});"
    "t('encoder',()=>new TextEncoder().encoding==='utf-8');"
    "t('bridge deleted',()=>!('__tilefinchDecodeBytes' in globalThis));"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'TEXT-DECODER-UTF16-OK';";

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
    if (!run_probe("text-decoder-utf16", text_decoder_utf16_probe,
                   "TEXT-DECODER-UTF16-OK"))
        failed++;
    if (!run_probe("url-opaque-base", url_opaque_base_probe,
                   "URL-OPAQUE-BASE-OK"))
        failed++;
    if (!run_probe_at("url-fixture-document-base", "fixtures/demo.html",
                      "<p id=p>text</p>", url_document_base_probe,
                      "URL-DOCUMENT-BASE-OK"))
        failed++;
    CHECK(failed == 0);
    return 0;
}

/* Inline event handler attributes run with the element, its form owner
   (form-associated elements only) and the document on the scope chain,
   ahead of the global object, and `this` is the element. */
static const char handler_scope_body[] =
    "<link id=lazy rel=stylesheet media=none href='data:text/css,'"
    " onload=\"if(media!='all')media='all'\">"
    "<form id=f name=signup action='/go'><input name=user value=ada>"
    "<input id=pw name=pass value=x onclick=\"globalThis.formSeen="
    "[user.value,pass===this,elements.length,elements[0]===user,name].join()\">"
    "</form>"
    "<p id=doc onclick=\"globalThis.docSeen=[URL===document.URL,"
    "typeof cookie,typeof getElementById,id,title].join()\">doc</p>"
    "<p id=self onclick=\"globalThis.selfSeen=this===event.currentTarget"
    "&&this.id==='self'&&id==='self'\">self</p>"
    "<p id=glob onclick=\"globalThis.globSeen=[typeof JSON,"
    "typeof handlerScopeMarker,typeof window,Math.max(1,2)].join()\">g</p>"
    "<p id=shadow onclick=\"var title='local';globalThis.shadowSeen="
    "title+','+document.title\">s</p>"
    "<p id=strict onclick=\"'use strict';globalThis.strictSeen="
    "(function(){return this})()===undefined&&id==='strict'\">st</p>"
    "<p id=cancel onclick=\"return false\">c</p>";
static const char handler_scope_probe[] =
    "const failures=[],click=id=>document.getElementById(id)"
    ".dispatchEvent(new MouseEvent('click',{cancelable:true}));"
    "globalThis.handlerScopeMarker=1;"
    "const lazy=document.getElementById('lazy');"
    "lazy.dispatchEvent(new Event('load'));"
    "if(lazy.media!=='all'||lazy.getAttribute('media')!=='all')"
    "failures.push('lazy-css:'+lazy.media);"
    "click('pw');"
    "if(globalThis.formSeen!=='ada,true,2,true,pass')"
    "failures.push('form:'+globalThis.formSeen);"
    "click('doc');"
    "if(globalThis.docSeen!=='true,string,function,doc,')"
    "failures.push('document:'+globalThis.docSeen);"
    "click('self');if(globalThis.selfSeen!==true)failures.push('this');"
    "click('glob');"
    "if(globalThis.globSeen!=='object,number,object,2')"
    "failures.push('global:'+globalThis.globSeen);"
    "click('shadow');if(globalThis.shadowSeen!=='local,probe')"
    "failures.push('shadow:'+globalThis.shadowSeen);"
    "click('strict');if(globalThis.strictSeen!==true)"
    "failures.push('strict:'+globalThis.strictSeen);"
    "if(click('cancel')!==false)failures.push('return-false');"
    "if('user' in globalThis||'media' in globalThis)failures.push('leak');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'HANDLER-SCOPE-OK';";

static int test_inline_handler_scope(void)
{
    puts("test: inline event handlers see element, form and document scopes");
    CHECK(run_probe_after("handler-scope", handler_scope_body,
                          handler_scope_probe, "HANDLER-SCOPE-OK"));
    return 0;
}

/* Form-only members live on HTMLFormElement alone, so a control's inline
   handler reads its form's `action`; DOM's [Unscopable] lists keep a bare
   `remove()` in a handler from reaching element.remove(). */
static const char handler_members_body[] =
    "<form id=f action='/submit' target=out><input id=act onclick=\""
    "globalThis.actionSeen=action;globalThis.targetSeen=target\"></form>"
    "<a id=link href='/x' target=frame>x</a>"
    "<p id=rm onclick=\"globalThis.removeTried=1;remove()\">rm</p>"
    "<p id=app onclick=\"globalThis.appendSeen=typeof append\">app</p>";
static const char handler_members_probe[] =
    "const failures=[],click=id=>document.getElementById(id)"
    ".dispatchEvent(new MouseEvent('click',{cancelable:true}));"
    "const form=document.getElementById('f'),"
    "div=document.createElement('div');"
    "for(const name of['action','target','method','enctype','noValidate',"
    "'elements','submit','requestSubmit','reset'])"
    "{if(name in div)failures.push('div-has-'+name);"
    "if(!(name in form))failures.push('form-lacks-'+name);}"
    "if('action' in document.getElementById('act'))failures.push('input-action');"
    "if(!/\\/submit$/.test(form.action))failures.push('form.action:'+form.action);"
    "if(form.target!=='out')failures.push('form.target:'+form.target);"
    "if(document.getElementById('link').target!=='frame')failures.push('a.target');"
    "for(const tag of['area','base'])"
    "if(!('target' in document.createElement(tag)))failures.push(tag+'.target');"
    "click('act');"
    "if(globalThis.actionSeen!==form.action)"
    "failures.push('handler-action:'+globalThis.actionSeen);"
    "if(globalThis.targetSeen!=='out')"
    "failures.push('handler-target:'+globalThis.targetSeen);"
    "click('rm');"
    "if(globalThis.removeTried!==1||!document.getElementById('rm'))"
    "failures.push('bare-remove-removed');"
    "globalThis.remove=()=>{globalThis.globalRemove=1};"
    "click('rm');delete globalThis.remove;"
    "if(globalThis.globalRemove!==1||!document.getElementById('rm'))"
    "failures.push('bare-remove-global');"
    "click('app');if(globalThis.appendSeen!=='undefined')"
    "failures.push('bare-append:'+globalThis.appendSeen);"
    "const lists=[[Element,'after append before prepend remove replaceChildren"
    " replaceWith slot'],[CharacterData,'after before remove replaceWith'],"
    "[DocumentType,'after before remove replaceWith'],"
    "[Document,'append prepend replaceChildren'],"
    "[DocumentFragment,'append prepend replaceChildren']];"
    "for(const[type,names]of lists){"
    "const d=Object.getOwnPropertyDescriptor(type.prototype,Symbol.unscopables),"
    "u=d&&d.value;"
    "if(!u||d.writable||d.enumerable||!d.configurable"
    "||Object.getPrototypeOf(u)!==null"
    "||Object.keys(u).sort().join(' ')!==names)"
    "failures.push('unscopables:'+type.name);}"
    "if(Element.prototype[Symbol.unscopables].remove!==true)"
    "failures.push('unscopables-remove');"
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"
    ":'HANDLER-MEMBERS-OK';";

static int test_handler_form_members_and_unscopables(void)
{
    puts("test: form-only members and DOM unscopables in handler scopes");
    CHECK(run_probe_after("handler-members", handler_members_body,
                          handler_members_probe, "HANDLER-MEMBERS-OK"));
    return 0;
}

/* ---- Platform methods present as natives ----
   Bootstrap functions print as natives (their bytecode carries no source),
   and the bootstrap's lowerCamelCase `function`s are read as methods (no
   own `prototype`, not constructors; JS_READ_OBJ_PLATFORM_METHODS). Members
   that live on the per-tag native layer are also published on their
   interface prototypes, where pages capture them. Author functions keep
   their source text and constructors. */

static const char platform_method_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const toSource=Function.prototype.toString,has=(o,k)=>Object.prototype.has"
    "OwnProperty.call(o,k);"
    "const native=(fn,name)=>toSource.call(fn)==='function '+name+'() {\\n    ["
    "native code]\\n}';"
    "const constructs=(fn)=>{try{Reflect.construct(String,[],fn);return true}ca"
    "tch(e){return false}};"
    "const rows=[[EventTarget.prototype,'addEventListener',2],[EventTarget.prot"
    "otype,'dispatchEvent',1],"
    "[Node.prototype,'appendChild',1],[Node.prototype,'insertBefore',2],[Node.p"
    "rototype,'contains',1],"
    "[Node.prototype,'removeChild',1],[Node.prototype,'replaceChild',2],[Node.p"
    "rototype,'cloneNode',0],"
    "[Element.prototype,'querySelector',1],[Element.prototype,'closest',1],[Ele"
    "ment.prototype,'matches',1],"
    "[Element.prototype,'setAttribute',2],[Element.prototype,'getBoundingClient"
    "Rect',0],"
    "[Element.prototype,'replaceWith',0],[Element.prototype,'remove',0],[HTMLEl"
    "ement.prototype,'click',0],"
    "[HTMLElement.prototype,'focus',0],[Document.prototype,'querySelectorAll',1"
    "],"
    "[Document.prototype,'importNode',1],[Document.prototype,'elementFromPoint'"
    ",2],"
    "[DOMTokenList.prototype,'add',0],[DOMTokenList.prototype,'contains',1],[DO"
    "MTokenList.prototype,'remove',0],"
    "[DOMTokenList.prototype,'toggle',1],[HTMLInputElement.prototype,'checkVali"
    "dity',0],"
    "[HTMLInputElement.prototype,'setCustomValidity',1],[HTMLMediaElement.proto"
    "type,'play',0],"
    "[Response.prototype,'json',0],[XMLHttpRequest.prototype,'open',2],[Navigat"
    "or.prototype,'sendBeacon',1],"
    "[window,'reportError',1],[Object,'getOwnPropertyNames',1],[Reflect,'ownKey"
    "s',1],"
    "[Function.prototype,'toString',0]];"
    "for(const [holder,key,length] of rows){const d=Object.getOwnPropertyDescri"
    "ptor(holder,key),fn=d&&d.value,label=(holder.constructor&&holder.construct"
    "or.name||'')+'.'+key;"
    "if(typeof fn!=='function'){failures.push(label+':missing');continue}"
    "const problems=[];"
    "if(fn.name!==key)problems.push('name='+fn.name);"
    "if(fn.length!==length)problems.push('length='+fn.length);"
    "if(!native(fn,key))problems.push('source');"
    "if(has(fn,'prototype'))problems.push('prototype');"
    "if(constructs(fn))problems.push('constructor');"
    "if(!d.writable||!d.configurable)problems.push('flags');"
    "if(problems.length)failures.push(label+':'+problems.join(','));}"
    "for(const [holder,key] of [[Node.prototype,'textContent'],[Node.prototype,"
    "'parentNode'],[Element.prototype,'innerHTML'],"
    "[Element.prototype,'childElementCount'],[HTMLIFrameElement.prototype,'cont"
    "entWindow'],[HTMLIFrameElement.prototype,'src'],"
    "[HTMLScriptElement.prototype,'src'],[HTMLAnchorElement.prototype,'href'],["
    "HTMLImageElement.prototype,'src'],"
    "[Event.prototype,'type'],[StorageEvent.prototype,'key'],[DOMTokenList.prot"
    "otype,'length']]){"
    "const d=Object.getOwnPropertyDescriptor(holder,key);"
    "if(!d||typeof d.get!=='function'){failures.push(key+':no getter');continue"
    "}"
    "if(!native(d.get,'get '+key)||d.get.name!=='get '+key)failures.push(key+':"
    "getter '+JSON.stringify(toSource.call(d.get).slice(0,24)));"
    "if(has(d.get,'prototype')||constructs(d.get))failures.push(key+':getter pr"
    "ototype');"
    "if(!d.configurable||!d.enumerable)failures.push(key+':getter flags');}"
    "const div=document.createElement('div');div.innerHTML='<b>x</b>';"
    "const text=Object.getOwnPropertyDescriptor(Node.prototype,'textContent').g"
    "et.call(div);"
    "expect('captured textContent',text==='x');"
    "expect('captured innerHTML',Object.getOwnPropertyDescriptor(Element.protot"
    "ype,'innerHTML').get.call(div)==='<b>x</b>');"
    "expect('childElementCount',div.childElementCount===1&&document.childElemen"
    "tCount===1);"
    "div.removeChild(div.firstChild);expect('childElementCount after remove',di"
    "v.childElementCount===0);"
    "const toStringSelf=Function.prototype.toString;"
    "expect('toString self',native(toStringSelf,'toString')&&toStringSelf.call("
    "toStringSelf)===toSource.call(toStringSelf)&&!has(toStringSelf,'prototype'"
    ")&&!constructs(toStringSelf));"
    "function authorHelper(a,b){return a+b}"
    "const authorArrow=(x)=>x*2;"
    "expect('author source',toSource.call(authorHelper)==='function authorHelpe"
    "r(a,b){return a+b}'&&String(authorArrow)==='(x)=>x*2');"
    "expect('author prototype',has(authorHelper,'prototype')&&constructs(author"
    "Helper));"
    "const lowerCtor=new Function('this.v=1');expect('author Function',new lowe"
    "rCtor().v===1);"
    "globalThis.pocSummary=failures.length?failures.length+' FAIL '+failures.jo"
    "in(' | '):'PLATFORM-METHODS-OK';";

static int test_platform_methods_present_as_native(void)
{
    puts("test: platform methods are native-shaped; author functions keep source");
    CHECK(run_probe("platform-methods", platform_method_probe,
                    "PLATFORM-METHODS-OK"));
    return 0;
}

/* ---- Init-dictionary events and DOMTokenList ----
   Storage polling constructs StorageEvents; capability probes also walk
   window.DOMTokenList.prototype.add. */

static const char init_event_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const throwsType=(f)=>{try{f();return false}catch(e){return e instanceof T"
    "ypeError}};"
    "const e=new StorageEvent('storage',{key:'k',oldValue:null,newValue:'v',url"
    ":'https://a.test/',storageArea:localStorage,bubbles:true});"
    "expect('storage fields',e.type==='storage'&&e.key==='k'&&e.oldValue===null"
    "&&e.newValue==='v'&&e.url==='https://a.test/'&&e.storageArea===localStorag"
    "e&&e.bubbles===true);"
    "expect('storage defaults',(()=>{const d=new StorageEvent('storage');return"
    " d.key===null&&d.oldValue===null&&d.newValue===null&&d.url===''&&d.storage"
    "Area===null&&d.bubbles===false})());"
    "expect('storage strings',new StorageEvent('x',{key:5,newValue:undefined})."
    "key==='5'&&new StorageEvent('x',{newValue:undefined}).newValue===null);"
    "expect('storage area type',throwsType(()=>new StorageEvent('x',{storageAre"
    "a:{}})));"
    "expect('storage proto',e instanceof Event&&Object.prototype.toString.call("
    "e)==='[object StorageEvent]'&&StorageEvent.name==='StorageEvent'&&StorageE"
    "vent.length===1&&!Object.keys(window).includes('StorageEvent'));"
    "const i=document.createEvent('StorageEvent');i.initStorageEvent('storage',"
    "true,false,'a','b','c','https://b.test/',sessionStorage);"
    "expect('initStorageEvent',i instanceof StorageEvent&&i.type==='storage'&&i"
    ".bubbles&&!i.cancelable&&i.key==='a'&&i.oldValue==='b'&&i.newValue==='c'&&"
    "i.url==='https://b.test/'&&i.storageArea===sessionStorage&&StorageEvent.pr"
    "ototype.initStorageEvent.length===1);"
    "expect('key getter',throwsType(()=>Object.getOwnPropertyDescriptor(Storage"
    "Event.prototype,'key').get.call({})));"
    "const h=new HashChangeEvent('hashchange',{oldURL:'a',newURL:'b'});expect('"
    "hashchange',h.oldURL==='a'&&h.newURL==='b'&&new HashChangeEvent('x').oldUR"
    "L==='');"
    "const s={};const p=new PopStateEvent('popstate',{state:s});expect('popstat"
    "e',p.state===s&&new PopStateEvent('x').state===null&&p.hasUAVisualTransiti"
    "on===false);"
    "const list=document.getElementById('p').classList;"
    "expect('DOMTokenList',list instanceof DOMTokenList&&Object.prototype.toStr"
    "ing.call(list)==='[object DOMTokenList]'&&Object.keys(list).length===0&&DO"
    "MTokenList.prototype[Symbol.iterator]===DOMTokenList.prototype.values&&thr"
    "owsType(()=>new DOMTokenList()));"
    "list.add('a','b');list.toggle('a');expect('DOMTokenList ops',list.value==="
    "'b'&&list.length===1&&list[0]==='b'&&list.contains('b')&&[...list].join()="
    "=='b');"
    "expect('DOMTokenList walk',['add','contains','remove'].every((k)=>typeof w"
    "indow.DOMTokenList.prototype[k]==='function'));"
    "const frame=document.createElement('iframe');frame.sandbox.add('allow-scri"
    "pts','allow-forms');frame.sandbox.toggle('allow-forms');"
    "expect('sandbox list',frame.getAttribute('sandbox')==='allow-scripts'&&fra"
    "me.sandbox instanceof DOMTokenList&&frame.sandbox.length===1);"
    /* supports(): sandbox and rel define supported tokens (ASCII
       case-insensitive); every other list throws a TypeError. */
    "expect('sandbox supports',frame.sandbox.supports('allow-scripts')&&frame"
    ".sandbox.supports('ALLOW-SAME-ORIGIN')&&frame.sandbox.supports('allow-po"
    "pups-to-escape-sandbox')&&!frame.sandbox.supports('allow-everything')&&!"
    "frame.sandbox.supports('')&&!frame.sandbox.supports('allow-forms allow-m"
    "odals'));"
    "const link=document.createElement('link');link.rel='Stylesheet';"
    "expect('link relList',link.relList instanceof DOMTokenList&&link.relList"
    ".contains('Stylesheet')&&link.relList.supports('preload')&&link.relList."
    "supports('STYLESHEET')&&!link.relList.supports('noopener')&&!link.relLis"
    "t.supports('bogus'));"
    "link.relList.add('preload');link.relList='icon';"
    "expect('relList ops',link.rel==='icon'&&link.getAttribute('rel')==='icon"
    "'&&link.relList.length===1);"
    "expect('anchor relList',['a','area','form'].every((tag)=>{const e=docume"
    "nt.createElement(tag);return e.relList instanceof DOMTokenList&&e.relLis"
    "t.supports('noopener')&&e.relList.supports('noreferrer')&&e.relList.supp"
    "orts('opener')&&!e.relList.supports('stylesheet')}));"
    "expect('no supported tokens',throwsType(()=>list.supports('a'))&&throwsT"
    "ype(()=>document.createElement('div').sandbox.supports('allow-scripts'))"
    "&&!('relList' in document.createElement('div')));"
    "globalThis.pocSummary=failures.length?failures.length+' FAIL '+failures.jo"
    "in(' | '):'INIT-EVENTS-OK';";

static int test_init_events_and_token_list(void)
{
    puts("test: StorageEvent, HashChangeEvent, PopStateEvent, DOMTokenList");
    CHECK(run_probe("init-events", init_event_probe, "INIT-EVENTS-OK"));
    return 0;
}

/* ---- Large character data ----
   createTextNode and createComment take any string: a page that loads its
   CSS late puts one style sheet of several hundred KiB into a <style> as a
   single Text node, then appends the <style> to <head>. Both used to
   return null above 256 KiB, so the append threw "Node required". (This
   runner has no style engine; web_runtime_navigation.inc checks that such
   a sheet applies.) */

static const char large_character_data_probe[] =
    "const failures=[],expect=(name,ok)=>{if(!ok)failures.push(name)};"
    "const css='/*'+'x'.repeat(600000)+'*/#p{color:rgb(1, 2, 3)}';"
    "const text=document.createTextNode(css);"
    "expect('text node',text instanceof Text&&text.nodeType===3&&"
    "text.data.length===css.length&&text.length===css.length);"
    "const before=document.styleSheets.length,style=document.createElement("
    "'STYLE');style.type='text/css';style.appendChild(text);"
    "document.getElementsByTagName('HEAD')[0].appendChild(style);"
    "expect('style inserted',style.parentNode===document.head&&"
    "style.firstChild===text&&style.textContent.length===css.length);"
    "expect('sheet count',document.styleSheets.length===before+1&&"
    "document.styleSheets[before].ownerNode===style);"
    "const comment=document.createComment('y'.repeat(300000));"
    "expect('comment node',comment instanceof Comment&&"
    "comment.data.length===300000);"
    "document.body.appendChild(comment);"
    "expect('comment inserted',document.body.lastChild===comment);"
    "const other=document.implementation.createHTMLDocument('')"
    ".createTextNode('z'.repeat(400000));"
    "expect('other document',other instanceof Text&&other.data.length===400000);"
    "globalThis.pocSummary=failures.length?failures.length+' FAIL '+failures.jo"
    "in(' | '):'LARGE-CHARACTER-DATA-OK';";

static int test_large_character_data(void)
{
    puts("test: createTextNode and createComment with data over 256 KiB");
    CHECK(run_probe("large-character-data", large_character_data_probe,
                    "LARGE-CHARACTER-DATA-OK"));
    return 0;
}

static int test_writable_terminal_handoffs(void)
{
    puts("test: writable stream abort, queued work, and in-flight close handoffs");
    static const char probe[] =
        "(async()=>{const failures=[],expect=(name,ok)=>{if(!ok){failures.push(name);"
        "globalThis.pocSummary='FAIL '+failures.join(' | ')}};const "
        "out=p=>p.then(()=>true,e=>e),defer=()=>{let resolve,reject;const promise="
        "new Promise((a,b)=>{resolve=a;reject=b});return {promise,resolve,reject}};"
        "{const start=defer(),reason={},calls=[],s=new WritableStream({start(){"
        "return start.promise},write(){calls.push('write')},close(){calls.push('close')},"
        "abort(r){calls.push(r===reason?'abort':'wrong')}}),w=s.getWriter(),closed="
        "out(w.closed),write=out(w.write('queued')),close=out(w.close()),a=w.abort(reason),"
        "b=w.abort({});expect('same abort promise',a===b);expect('wait for start',"
        "calls.length===0);start.resolve();expect('abort queued write',await write===reason);"
        "expect('abort queued close',await close===reason);expect('abort completion',"
        "await out(a)===true&&await closed===reason);await w.abort({});expect('single abort',"
        "calls.join()==='abort');expect('stored abort error',await out(w.write('late'))===reason)}"
        "{const started=defer(),finish=defer(),calls=[],reason={},s=new WritableStream({"
        "write(v){calls.push(v);started.resolve();return finish.promise.then(()=>calls.push('end'))},"
        "abort(){calls.push('abort')}}),w=s.getWriter(),closed=out(w.closed),first="
        "out(w.write('first')),second=out(w.write('second')),close=out(w.close());await "
        "started.promise;const a=w.abort(reason);expect('wait for in-flight write',"
        "calls.join()==='first');finish.resolve();expect('in-flight write completes',"
        "await first===true);expect('queued work rejected',await second===reason&&"
        "await close===reason);await a;expect('serialized abort',calls.join()==='first,end,abort'"
        "&&await closed===reason)}"
        "for(const fail of [false,true]){const started=defer(),finish=defer(),reason={},"
        "closeError={},calls=[],s=new WritableStream({close(){started.resolve();return "
        "finish.promise},abort(){calls.push('abort')}}),w=s.getWriter(),closed=out(w.closed),"
        "close=out(w.close());await started.promise;const abort=out(w.abort(reason));"
        "if(fail)finish.reject(closeError);else finish.resolve();expect('in-flight close result',"
        "await close===(fail?closeError:true));expect('in-flight close abort result',"
        "await abort===(fail?closeError:true));expect('in-flight close terminal result',"
        "await closed===(fail?reason:true));expect('no abort during close',calls.length===0)}"
        "{const start=defer(),reason={},s=new WritableStream({start(){return start.promise}}),"
        "w=s.getWriter(),closed=out(w.closed),write=out(w.write('x')),close=out(w.close());"
        "start.reject(reason);expect('start rejection',await write===reason&&await close===reason"
        "&&await closed===reason);expect('terminal abort is no-op',await out(w.abort({}))===true);"
        "expect('stored start error',await out(w.write('late'))===reason)}"
        "{const reason={},sinkError={},s=new WritableStream({abort(){throw sinkError}}),"
        "w=s.getWriter(),closed=out(w.closed);expect('abort rejection',await out(w.abort(reason))"
        "===sinkError&&await closed===reason)}"
        "{const root=await navigator.storage.getDirectory(),file=await root.getFileHandle("
        "'task-refusal.txt',{create:true});for(const close of [false,true]){const stream="
        "await file.createWritable(),owner=stream.getWriter(),closed=out(owner.closed),ids=[];"
        "for(let i=0;i<256;i++){const id=setTimeout(()=>{},60000);if(!id)break;ids.push(id)}"
        "expect('bounded timer fixture',ids.length>0&&ids.length<256);const error=await out("
        "close?owner.close():owner.write('discard'));for(const id of ids)clearTimeout(id);"
        "expect('writer task refuses safely',error.name==='QuotaExceededError'&&await closed"
        "===error);const retry=await file.createWritable();await retry.abort();expect('refused "
        "writer preserves file',(await file.getFile()).size===0)}}"
        "{const root=await navigator.storage.getDirectory(),file=await root.getFileHandle("
        "'handoff.txt',{create:true}),initial=await file.createWritable();await initial.write("
        "'keep');await initial.close();const failed=await file.createWritable({keepExistingData:"
        "true}),owner=failed.getWriter(),closed=out(owner.closed);"
        "await owner.write('discard');const error=await out(owner.write("
        "{type:'write'}));expect('file write rejects',error instanceof TypeError);await closed;"
        "const next=await file.createWritable({keepExistingData:true});await owner.abort();"
        "const locked=await out(file.createWritable());expect('old abort preserves new lock',"
        "locked.name==='NoModificationAllowedError');await next.abort();expect('failed file "
        "write rolls back',await(await file.getFile()).text()==='keep')}"
        "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | '):'WRITABLE-OK'"
        "})().catch(e=>globalThis.pocSummary='UNEXPECTED '+String(e));";
    static const char page[] = "<!doctype html><title>Stream lifecycle</title>";
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17));
    BrowserSession session;
    CHECK(browser_session_init(&session, &budget, 64u * 1024u));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_deferred(
        &document, &budget, 8u * MIB, 3000,
        "https://conformance.test/streams", &session, &result);
    CHECK(runtime != NULL);
    bool ran = script_runtime_evaluate_diagnostic(
        runtime, probe, "<writable-handoffs>", &result);
    /* The cases intentionally exceed one bounded microtask checkpoint.
       Continue the real owner loop rather than weakening its job limit. */
    for (unsigned turn = 0; ran && turn < 64u
         && strcmp(result.summary, "WRITABLE-OK") != 0; turn++)
        ran = script_runtime_advance(runtime, 1u, 8u, &result);
    bool ok = ran && result.success && !strcmp(result.summary, "WRITABLE-OK");
    if (!ok) fprintf(stderr, "writable handoffs: ran=%d summary=%s error=%s\n",
                     ran, result.summary, result.error);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    browser_session_destroy(&session);
    CHECK(ok && budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* ---- Window and document members pages read as constants ---- */

#define COMPAT_PRELUDE                                                       \
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"        \
    "catch(e){failures.push(name+':'+e)}};"
#define COMPAT_SUMMARY(ok)                                                   \
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"     \
    ":'" ok "';"

/* The PSP window fills its screen at the origin; its browsing context is
   open; window.name and status are settable strings; the move, resize,
   focus and capture operations exist and do nothing. */
static const char window_members_probe[] =
    COMPAT_PRELUDE
    "t('screen position',()=>screenX===0&&screenY===0&&screenLeft===0"
    "&&screenTop===0);"
    "t('outer size',()=>outerWidth===screen.width&&outerHeight===screen.height);"
    "t('closed',()=>window.closed===false&&self.closed===false"
    "&&typeof Object.getOwnPropertyDescriptor(window,'closed').get==='function');"
    "t('name default',()=>name===''&&window.name==='');"
    "name=42;t('name set',()=>name==='42'&&typeof name==='string');"
    "name={toString(){return 'obj'}};t('name string',()=>window.name==='obj');"
    "const d=Object.getOwnPropertyDescriptor(window,'name');"
    "t('name accessor',()=>typeof d.get==='function'&&typeof d.set==='function'"
    "&&d.enumerable&&d.configurable);"
    "t('status',()=>status===''&&(status='busy',status==='busy'));"
    "t('operations',()=>['stop','focus','blur','moveTo','moveBy','resizeTo',"
    "'resizeBy','captureEvents','releaseEvents'].every(n=>"
    "typeof window[n]==='function'&&window[n]()===undefined));"
    "t('lengths',()=>moveTo.length===2&&resizeBy.length===2&&stop.length===0);"
    "const f=document.createElement('iframe');f.name='child';"
    "document.body.appendChild(f);const w=f.contentWindow;"
    "t('frame members',()=>w.eval('[screenX,closed,typeof moveTo].join()')"
    "==='0,false,function'&&w.name==='child'&&w.eval('name')==='child');"
    "t('replaceable',()=>(window.screenX=5,screenX===5));"
    COMPAT_SUMMARY("WINDOW-MEMBERS-OK");

/* document.characterSet reports the encoding the parser chose (here from
   the <meta> prescan), with charset and inputEncoding as aliases; documents
   built from strings are UTF-8. */
static const char character_set_probe[] =
    COMPAT_PRELUDE
    "t('main',()=>document.characterSet==='windows-1251'"
    "&&document.charset==='windows-1251'"
    "&&document.inputEncoding==='windows-1251');"
    "t('on prototype',()=>!Object.hasOwn(document,'characterSet')"
    "&&Object.getOwnPropertyDescriptor(Document.prototype,'characterSet')"
    ".get.name==='get characterSet');"
    "t('created',()=>document.implementation.createHTMLDocument('x')"
    ".characterSet==='UTF-8'&&new DOMParser().parseFromString('<p>',"
    "'text/html').charset==='UTF-8'&&new Document().inputEncoding==='UTF-8');"
    "t('brand',()=>{try{Object.getOwnPropertyDescriptor(Document.prototype,"
    "'characterSet').get.call({});return false}catch(e){"
    "return e instanceof TypeError}});"
    "const f=document.createElement('iframe');document.body.appendChild(f);"
    "t('frame',()=>f.contentDocument.characterSet==='UTF-8');"
    COMPAT_SUMMARY("CHARACTER-SET-OK");

/* A frame window owns the names a top-level window does: Google's sign-in
   check samples Object.getOwnPropertyNames(frame) and reads each name's
   .prototype. The interface objects are the page's (shared, not copied);
   the page's indexed child windows are its own, not the frame's;
   the frame's own operations carry their WebIDL names. */
static const char frame_global_names_probe[] =
    COMPAT_PRELUDE
    "const f=document.createElement('iframe');document.body.appendChild(f);"
    "const w=f.contentWindow,names=Object.getOwnPropertyNames(w),"
    "own=new Set(names);"
    "t('superset',()=>Object.getOwnPropertyNames(window).every(n=>"
    "own.has(n)||String(n>>>0)===n));"
    "t('unique',()=>own.size===names.length);"
    "t('no bridges',()=>!names.some(n=>n.startsWith('__tilefinch')));"
    "t('inside',()=>w.eval('Object.getOwnPropertyNames(window).length')"
    "===names.length&&w.eval('Object.hasOwn(window,\"HTMLElement\")'));"
    "t('descriptors',()=>names.every(n=>!!Object.getOwnPropertyDescriptor(w,n)));"
    "t('interfaces',()=>['HTMLElement','Event','Node','MutationObserver',"
    "'CustomElementRegistry','Document'].every(n=>own.has(n)"
    "&&typeof w[n].prototype==='object'&&w[n]===window[n]));"
    "t('realm intrinsics',()=>w.Object!==Object&&w.Array.isArray(w.eval('[]')));"
    "t('page globals stay',()=>(window.pageOnly=1,!Object.hasOwn(w,'pageOnly')));"
    "t('operation names',()=>w.postMessage.name==='postMessage'"
    "&&w.postMessage.length===1&&w.getComputedStyle.name==='getComputedStyle'"
    "&&!('prototype' in w.postMessage));"
    "t('focus close',()=>['close','focus','blur'].every(n=>"
    "typeof w[n]==='function'&&w[n]!==window[n]&&w[n].name===n"
    "&&w[n]()===undefined)&&w.closed===false&&typeof window.close==='function');"
    COMPAT_SUMMARY("FRAME-GLOBAL-NAMES-OK");

/* HTML's reflected attributes: strings, booleans, nullable and plain
   enumerated keywords, (clamped, positive) integers and URLs resolved
   against the document, through getAttribute/setAttribute. */
static const char reflected_attributes_probe[] =
    COMPAT_PRELUDE
    "const img=document.createElement('img');"
    "t('alt',()=>img.alt===''&&(img.alt='x',img.getAttribute('alt')==='x'&&img.alt==='x'));"
    "t('co null',()=>img.crossOrigin===null);"
    "t('co empty',()=>(img.setAttribute('crossorigin',''),img.crossOrigin==='anonymous'));"
    "t('co uc',()=>(img.setAttribute('crossorigin','USE-credentials'),img.crossOrigin==='use-credentials'));"
    "t('co bad',()=>(img.setAttribute('crossorigin','zz'),img.crossOrigin==='anonymous'));"
    "t('co rm',()=>(img.crossOrigin=null,!img.hasAttribute('crossorigin')));"
    /* crossOrigin on every element with a CORS settings attribute; the
       parser keeps a bare attribute without a value. */
    "for(const tag of['img','script','link','audio','video']){"
    "const h=document.createElement('div');"
    "h.innerHTML='<'+tag+' crossorigin>'+(tag==='img'||tag==='link'?'':'</'+tag+'>');"
    "const e=h.firstChild;"
    "t(tag+' co bare',()=>e.getAttribute('crossorigin')===''&&e.crossOrigin==='anonymous');"
    "t(tag+' co values',()=>[['','anonymous'],['anonymous','anonymous'],"
    "['use-credentials','use-credentials'],['USE-CREDENTIALS','use-credentials'],"
    "['zz','anonymous']].every(([v,w])=>(e.setAttribute('crossorigin',v),e.crossOrigin===w)));"
    "t(tag+' co set',()=>(e.crossOrigin='Use-Credentials',"
    "e.getAttribute('crossorigin')==='Use-Credentials'&&e.crossOrigin==='use-credentials'));"
    "t(tag+' co null',()=>(e.crossOrigin=null,!e.hasAttribute('crossorigin')&&e.crossOrigin===null));}"
    "t('ismap',()=>!img.isMap&&(img.isMap=1,img.getAttribute('ismap')==='')&&(img.isMap=false,!img.hasAttribute('ismap')));"
    "t('decoding',()=>img.decoding==='auto'&&(img.decoding='SYNC',img.decoding==='sync'));"
    "t('hspace',()=>img.hspace===0&&(img.setAttribute('hspace',' 12px'),img.hspace===12)&&(img.setAttribute('hspace','-3'),img.hspace===0));"
    "t('hspace set',()=>(img.hspace=-1,img.getAttribute('hspace')==='0'));"
    "t('border',()=>(img.border=null,img.getAttribute('border')===''));"
    "t('longDesc',()=>(img.setAttribute('longdesc','d.html'),img.longDesc==='https://conformance.test/d.html')&&(img.removeAttribute('longdesc'),img.longDesc===''));"
    "t('names',()=>Object.getOwnPropertyDescriptor(HTMLImageElement.prototype,'alt').get.name==='get alt'&&Object.getOwnPropertyDescriptor(HTMLImageElement.prototype,'alt').enumerable);"
    "const td=document.createElement('td');"
    "t('colspan',()=>td.colSpan===1&&(td.setAttribute('colspan','5000'),td.colSpan===1000)&&(td.setAttribute('colspan','0'),td.colSpan===1));"
    "t('rowspan',()=>td.rowSpan===1&&(td.setAttribute('rowspan','0'),td.rowSpan===0));"
    "const ta=document.createElement('textarea');"
    "t('cols',()=>ta.cols===20&&ta.rows===2);"
    "t('cols0',()=>{try{ta.cols=0;return false}catch(e){return e.name==='IndexSizeError'}});"
    "const b=document.createElement('button');"
    "t('formAction',()=>b.formAction===location.href&&(b.formAction='/go',b.formAction==='https://conformance.test/go'));"
    "t('formMethod',()=>b.formMethod===''&&(b.formMethod='POST',b.formMethod==='post')&&(b.formMethod='x',b.formMethod==='get'));"
    "const a=document.createElement('a');a.textContent='hi';"
    "t('a.text',()=>a.text==='hi'&&(a.text='yo',a.textContent==='yo'));"
    "const ol=document.createElement('ol');"
    "t('start',()=>ol.start===1&&(ol.start=-4,ol.getAttribute('start')==='-4'&&ol.start===-4));"
    "t('form.autocomplete',()=>document.createElement('form').autocomplete==='on');"
    "t('enterKeyHint',()=>document.createElement('div').enterKeyHint==='');"
    "t('track',()=>document.createElement('track').kind==='subtitles');"
    "t('frame img',()=>{const f=document.createElement('iframe');document.body.appendChild(f);return f.contentWindow.eval(\"const d=new DOMParser().parseFromString('<img alt=q>','text/html');d.querySelector('img').alt==='q'&&document.createElement('img').alt===''&&document!==parent.document\")});"
    "t('brand',()=>{try{Object.getOwnPropertyDescriptor(HTMLImageElement.prototype,'alt').get.call({});return false}catch(e){return e instanceof TypeError}});"
    COMPAT_SUMMARY("REFLECTED-ATTRIBUTES-OK");

/* Web IDL constants and the members every node, element or UI event has:
   Node's type constants on every node, a detached element without an
   offsetParent, an uninitialised MouseEvent without a view and with which
   as button + 1, img.loading's "auto" default (Chromium's; HTML's missing
   and invalid value default is "eager", which Firefox and WebKit report),
   MouseEvent's fromElement and toElement as Chromium and WebKit answer
   them, draggable and the drag-and-drop feature tests that read it, and
   the DOM, HTML, CSSOM View and Pointer Lock members that were missing. */
static const char element_core_probe[] =
    COMPAT_PRELUDE
    "const img=document.createElement('img'),H='http://www.w3.org/1999/xhtml';"
    "t('node constants',()=>img.ELEMENT_NODE===1&&img.ATTRIBUTE_NODE===2"
    "&&img.TEXT_NODE===3&&img.CDATA_SECTION_NODE===4"
    "&&img.ENTITY_REFERENCE_NODE===5&&img.ENTITY_NODE===6"
    "&&img.PROCESSING_INSTRUCTION_NODE===7&&img.COMMENT_NODE===8"
    "&&img.DOCUMENT_NODE===9&&img.DOCUMENT_TYPE_NODE===10"
    "&&img.DOCUMENT_FRAGMENT_NODE===11&&img.NOTATION_NODE===12"
    "&&Node.NOTATION_NODE===12&&document.COMMENT_NODE===8"
    "&&document.createTextNode('').ELEMENT_NODE===1"
    "&&img.DOCUMENT_POSITION_FOLLOWING===4);"
    "t('constant shape',()=>{const d=Object.getOwnPropertyDescriptor("
    "Node.prototype,'COMMENT_NODE'),s=Object.getOwnPropertyDescriptor(Node,"
    "'COMMENT_NODE');return d.enumerable&&!d.writable&&!d.configurable"
    "&&s.enumerable&&!s.writable&&!s.configurable});"
    "t('constants enumerate',()=>{const seen=[];for(const k in img)"
    "if(/_NODE$/.test(k))seen.push(k);return seen.length===12});"
    "t('offsetParent detached',()=>img.offsetParent===null);"
    "const shown=document.body.appendChild(document.createElement('p'));"
    "t('offsetParent',()=>shown.offsetParent===document.body);"
    "const hidden=document.body.appendChild(document.createElement('p'));"
    "hidden.style.display='none';"
    "t('offsetParent no box',()=>hidden.offsetParent===null);"
    "const mouse=document.createEvent('MouseEvents');"
    "t('view',()=>mouse.view===null&&new MouseEvent('x').view===null"
    "&&new UIEvent('x',{view:window}).view===window);"
    "t('init view',()=>(mouse.initMouseEvent('click',true,true,window,0,0,"
    "0,0,0,false,false,false,false,1,null),mouse.view===window"
    "&&mouse.which===2));"
    "t('which',()=>new MouseEvent('x').which===1"
    "&&new MouseEvent('x',{button:2}).which===3"
    "&&new KeyboardEvent('k',{keyCode:65}).which===65"
    "&&new UIEvent('x').which===0);"
    "t('trusted view',()=>{let view;shown.addEventListener('click',"
    "e=>{view=e.view});shown.click();return view===window});"
    "t('loading',()=>img.loading==='auto'"
    "&&(img.setAttribute('loading','LAZY'),img.loading==='lazy')"
    "&&(img.setAttribute('loading','EAGER'),img.loading==='eager')"
    "&&(img.setAttribute('loading','zz'),img.loading==='auto'));"
    "t('from/to created',()=>mouse.fromElement===null"
    "&&mouse.toElement===null"
    "&&document.createEvent('MouseEvents').toElement===null"
    "&&new MouseEvent('mouseover',{relatedTarget:shown}).fromElement===shown"
    "&&new MouseEvent('mouseout',{relatedTarget:shown}).toElement===shown"
    "&&new MouseEvent('mouseover',{relatedTarget:window}).fromElement"
    "===null);"
    "t('from/to fired',()=>{const seen=[],to=hidden;for(const type of"
    "['mouseover','mouseenter','mouseout','mouseleave','click'])"
    "{to.addEventListener(type,e=>seen.push(type+':'+(e.fromElement===shown"
    "?'s':e.fromElement===to?'h':e.fromElement)+'>'+(e.toElement===shown"
    "?'s':e.toElement===to?'h':e.toElement)),{once:true});"
    "to.dispatchEvent(new MouseEvent(type,{relatedTarget:type==='click'"
    "?null:shown}))}return seen.join()==="
    "'mouseover:s>h,mouseenter:s>h,mouseout:h>s,mouseleave:h>s,click:null>h'"
    "});"
    "t('from/to shape',()=>['fromElement','toElement'].every(n=>{const d="
    "Object.getOwnPropertyDescriptor(MouseEvent.prototype,n);"
    "return d&&d.enumerable&&d.configurable&&!d.set&&d.get.name==='get '+n"
    "&&!Object.hasOwn(mouse,n)&&n in new PointerEvent('x')"
    "&&!(n in new Event('x'))}));"
    "t('baseURI',()=>img.baseURI===document.baseURI"
    "&&document.createTextNode('').baseURI===document.baseURI"
    "&&document.implementation.createHTMLDocument('').createElement('p')"
    ".baseURI==='about:blank'&&(()=>{const f=document.body.appendChild("
    "document.createElement('iframe'));return f.contentDocument"
    ".createElement('p').baseURI===document.baseURI})());"
    "const div=document.createElement('div'),a=document.createElement('a');"
    "t('draggable',()=>img.draggable===true&&div.draggable===false"
    "&&a.draggable===false&&(a.href='/x',a.draggable===true)"
    "&&(img.draggable=false,img.getAttribute('draggable')==='false'"
    "&&img.draggable===false)&&(div.setAttribute('draggable','TRUE'),"
    "div.draggable===true));"
    "t('drag feature tests',()=>'draggable' in document.createElement('span')"
    "&&(('draggable' in div)||('ondragstart' in div&&'ondrop' in div))"
    "&&Object.hasOwn(HTMLElement.prototype,'draggable'));"
    "const inner=div.appendChild(document.createElement('i'));"
    "t('spellcheck',()=>inner.spellcheck===true"
    "&&(div.spellcheck=false,div.getAttribute('spellcheck')==='false'"
    "&&inner.spellcheck===false)&&(inner.setAttribute('spellcheck',''),"
    "inner.spellcheck===true));"
    "t('translate',()=>inner.translate===true"
    "&&(div.translate=false,div.getAttribute('translate')==='no'"
    "&&inner.translate===false)&&(inner.setAttribute('translate','yes'),"
    "inner.translate===true));"
    "const form=document.createElement('form'),"
    "field=form.appendChild(document.createElement('input'));"
    "t('autocapitalize',()=>div.autocapitalize===''"
    "&&(div.autocapitalize='OFF',div.autocapitalize==='none')"
    "&&(div.setAttribute('autocapitalize','on'),"
    "div.autocapitalize==='sentences')"
    "&&(div.setAttribute('autocapitalize','zz'),"
    "div.autocapitalize==='sentences')"
    "&&(form.setAttribute('autocapitalize','words'),"
    "field.autocapitalize==='words'));"
    "t('part',()=>img.part instanceof DOMTokenList&&img.part.length===0"
    "&&(img.part.add('a'),img.getAttribute('part')==='a')"
    "&&(img.part='b c',img.part.length===2&&img.part.contains('c')));"
    "t('img x y',()=>img.x===0&&img.y===0&&(()=>{const placed="
    "document.body.appendChild(document.createElement('img'));"
    "placed.style.cssText='display:block;width:10px;height:10px';"
    "const r=placed.getBoundingClientRect();"
    "return placed.x===Math.round(r.left+scrollX)"
    "&&placed.y===Math.round(r.top+scrollY)})());"
    "t('lookups',()=>img.lookupNamespaceURI(null)===H"
    "&&img.lookupNamespaceURI('xml')==='http://www.w3.org/XML/1998/namespace'"
    "&&img.lookupNamespaceURI('nope')===null&&img.lookupPrefix(H)===null"
    "&&img.isDefaultNamespace(H)&&!img.isDefaultNamespace('urn:x')"
    "&&document.lookupNamespaceURI(null)===H"
    "&&document.createDocumentFragment().lookupNamespaceURI(null)===null);"
    "t('prefixed',()=>{const e=document.createElementNS('urn:x','p:e');"
    "return e.lookupPrefix('urn:x')==='p'&&e.lookupNamespaceURI('p')==='urn:x'"
    "&&!e.isDefaultNamespace('urn:x')});"
    "t('byTagNameNS',()=>{const root=document.createElement('div');"
    "root.innerHTML='<g></g><svg><g></g></svg>';"
    "const html=root.getElementsByTagNameNS(H,'g'),"
    "any=root.getElementsByTagNameNS('*','g');"
    "const before=[html.length,any.length].join();"
    "root.appendChild(document.createElement('g'));"
    "return before==='1,2'&&html.length===2&&any.length===3"
    "&&html instanceof HTMLCollection"
    "&&document.getElementsByTagNameNS(H,'body').length===1});"
    "t('webkitMatchesSelector',()=>img.webkitMatchesSelector('img')"
    "&&!img.webkitMatchesSelector('p')"
    "&&Object.hasOwn(Element.prototype,'webkitMatchesSelector'));"
    "t('no typed om',()=>!('computedStyleMap' in img));"
    "t('pointer lock',()=>typeof img.requestPointerLock==='function'"
    "&&document.pointerLockElement===null);"
    /* The request settles in a later microtask: the summary waits for it. */
    "let lockError=0;document.onpointerlockerror=()=>{lockError++};"
    "globalThis.pocSummary='PENDING';"
    "img.requestPointerLock().then(()=>'locked',e=>e.name).then(result=>{"
    "t('pointer lock refused',()=>result==='NotSupportedError'"
    "&&lockError===1);"
    COMPAT_SUMMARY("ELEMENT-CORE-OK")
    "});";

/* childNodes is a live NodeList, the same object on every read, for the
   page's nodes, the document, fragments, shadow hosts and script-side
   documents. */
static const char child_nodes_probe[] =
    COMPAT_PRELUDE
    "const d=document.createElement('div');"
    "d.innerHTML='<span>a</span>text<b>c</b>';"
    "const c=d.childNodes;"
    "t('type',()=>c instanceof NodeList&&!Array.isArray(c)"
    "&&Object.prototype.toString.call(c)==='[object NodeList]'"
    "&&c===d.childNodes);"
    "t('read',()=>c.length===3&&c[0].localName==='span'&&c[1].nodeType===3"
    "&&c.item(2).localName==='b'&&c[3]===undefined&&c.item(3)===null"
    "&&2 in c&&!(3 in c)&&Object.keys(c).join()==='0,1,2');"
    "t('live',()=>(d.appendChild(document.createElement('u')),c.length===4"
    "&&c[3].localName==='u')&&(d.removeChild(d.firstChild),c.length===3"
    "&&c[0].nodeType===3)&&(d.textContent='',c.length===0"
    "&&c[0]===undefined));"
    "d.innerHTML='<i></i><i></i><i></i>';"
    "t('iterate',()=>[...c].length===3&&Array.from(c.keys()).join()==='0,1,2'"
    "&&(()=>{let n=0;c.forEach((node,index,list)=>{n+=list===c&&"
    "node===c[index]?1:0});return n===3})()"
    "&&Array.prototype.indexOf.call(c,d.lastChild)===2);"
    "t('read only',()=>(c[0]=null,c.length=0,c.length===3"
    "&&c[0]===d.firstChild)&&(c.note=1,c.note===1));"
    "t('empty',()=>{const e=document.createElement('img').childNodes;"
    "return e instanceof NodeList&&e.length===0});"
    "t('text',()=>document.createTextNode('x').childNodes.length===0);"
    "t('document',()=>document.childNodes instanceof NodeList"
    "&&document.childNodes===document.childNodes"
    "&&document.childNodes[document.childNodes.length-1]"
    "===document.documentElement);"
    "t('fragment',()=>{const f=document.createDocumentFragment(),"
    "l=f.childNodes;f.appendChild(document.createElement('p'));"
    "return l instanceof NodeList&&l.length===1&&l===f.childNodes});"
    "t('shadow host',()=>{const h=document.createElement('div');"
    "h.attachShadow({mode:'open'}).innerHTML='<slot></slot>';"
    "const l=h.childNodes;h.appendChild(document.createElement('p'));"
    "return l instanceof NodeList&&l.length===1&&l===h.childNodes"
    "&&h.shadowRoot.childNodes instanceof NodeList"
    "&&h.shadowRoot.childNodes.length===1});"
    "t('other document',()=>{const o=document.implementation"
    ".createHTMLDocument('t'),l=o.body.childNodes,top=o.childNodes;"
    "o.body.appendChild(o.createElement('p'));"
    "return l instanceof NodeList&&l.length===1&&l===o.body.childNodes"
    "&&top instanceof NodeList&&top.length===2&&top===o.childNodes});"
    "t('parsed document',()=>{const o=new DOMParser().parseFromString("
    "'<p>a</p><p>b</p>','text/html');"
    "return o.body.childNodes instanceof NodeList"
    "&&o.body.childNodes.length===2});"
    COMPAT_SUMMARY("CHILD-NODES-OK");

/* ARIAMixin, which only Google's sites get for now
   (site_identity_aria_reflection_site): every string attribute reflects its
   content attribute or is null, and the element-reference attributes
   resolve IDs in the element's tree or hold the elements set through
   them. */
#define ARIA_STRING_NAMES                                                    \
    "('role ariaAtomic ariaAutoComplete ariaBrailleLabel "                   \
    "ariaBrailleRoleDescription ariaBusy ariaChecked ariaColCount "          \
    "ariaColIndex ariaColIndexText ariaColSpan ariaCurrent ariaDescription " \
    "ariaDisabled ariaExpanded ariaHasPopup ariaHidden ariaInvalid "         \
    "ariaKeyShortcuts ariaLabel ariaLevel ariaLive ariaModal ariaMultiLine " \
    "ariaMultiSelectable ariaOrientation ariaPlaceholder ariaPosInSet "      \
    "ariaPressed ariaReadOnly ariaRelevant ariaRequired ariaRoleDescription "\
    "ariaRowCount ariaRowIndex ariaRowIndexText ariaRowSpan ariaSelected "   \
    "ariaSetSize ariaSort ariaValueMax ariaValueMin ariaValueNow "           \
    "ariaValueText').split(' ')"
#define ARIA_ELEMENT_NAMES                                                   \
    "('ariaActiveDescendantElement ariaControlsElements "                    \
    "ariaDescribedByElements ariaDetailsElements ariaErrorMessageElements "  \
    "ariaFlowToElements ariaLabelledByElements ariaOwnsElements')"           \
    ".split(' ')"
/* Runs the probe in the <script type=text/x-probe> with this id inside a
   same-origin child frame's realm, and reports the frame's summary. */
#define aria_frame_probe(id)                                                  \
    "const f=document.body.appendChild(document.createElement('iframe')),"   \
    "w=f.contentWindow;w.eval(document.getElementById('" id "').textContent);" \
    "globalThis.pocSummary=w.document!==document&&w!==window"                 \
    "?String(w.pocSummary):'not a frame realm';"

static const char aria_reflection_probe[] =
    COMPAT_PRELUDE
    "const names=" ARIA_STRING_NAMES ",elementNames=" ARIA_ELEMENT_NAMES ";"
    /* A child frame's realm shares the page's DOM interfaces, whose
       members throw the page's TypeError. */
    "const isTypeError=e=>e instanceof TypeError||e instanceof parent.TypeError;"
    "t('element names',()=>elementNames.every(n=>{const d=Object"
    ".getOwnPropertyDescriptor(Element.prototype,n);"
    "return d&&d.enumerable&&d.configurable&&d.get&&d.set}));"
    "const img=document.createElement('img');"
    "t('null',()=>names.every(n=>n in img&&img[n]===null));"
    "t('on Element',()=>names.every(n=>{const d=Object"
    ".getOwnPropertyDescriptor(Element.prototype,n);"
    "return d&&d.enumerable&&d.configurable&&d.get.name==='get '+n}));"
    "t('reflect',()=>names.every(n=>{const attr=n==='role'?n:'aria-'+"
    "n.slice(4).toLowerCase();img[n]='v';const set=img.getAttribute(attr);"
    "img.setAttribute(attr,'w');const got=img[n];img[n]=null;"
    "return set==='v'&&got==='w'&&!img.hasAttribute(attr)}));"
    "t('undefined removes',()=>(img.ariaLabel='x',img.ariaLabel=undefined,"
    "!img.hasAttribute('aria-label')));"
    "t('svg',()=>document.createElementNS('http://www.w3.org/2000/svg','g')"
    ".ariaLabel===null);"
    "t('brand',()=>{try{Object.getOwnPropertyDescriptor(Element.prototype,"
    "'ariaLabel').get.call({});return false}"
    "catch(e){return isTypeError(e)}});"
    "const host=document.body.appendChild(document.createElement('div'));"
    "host.innerHTML='<p id=one></p><p id=two></p><span id=owner></span>';"
    "const owner=host.lastChild,one=host.firstChild,two=one.nextSibling;"
    "t('element null',()=>owner.ariaActiveDescendantElement===null"
    "&&owner.ariaLabelledByElements===null&&owner.ariaControlsElements===null"
    "&&owner.ariaDescribedByElements===null&&owner.ariaDetailsElements===null"
    "&&owner.ariaErrorMessageElements===null&&owner.ariaFlowToElements===null"
    "&&owner.ariaOwnsElements===null);"
    "t('ids',()=>(owner.setAttribute('aria-labelledby','two missing one'),"
    "owner.ariaLabelledByElements.length===2"
    "&&owner.ariaLabelledByElements[0]===two"
    "&&owner.ariaLabelledByElements[1]===one"
    "&&Object.isFrozen(owner.ariaLabelledByElements))"
    "&&(owner.setAttribute('aria-activedescendant','two'),"
    "owner.ariaActiveDescendantElement===two));"
    "t('explicit',()=>(owner.ariaControlsElements=[one],"
    "owner.getAttribute('aria-controls')===''"
    "&&owner.ariaControlsElements.length===1"
    "&&owner.ariaControlsElements[0]===one)"
    "&&(owner.ariaActiveDescendantElement=one,"
    "owner.getAttribute('aria-activedescendant')===''"
    "&&owner.ariaActiveDescendantElement===one));"
    "t('out of scope',()=>{const away=document.createElement('p');"
    "owner.ariaControlsElements=[away];"
    "return owner.ariaControlsElements.length===0});"
    "t('clear',()=>(owner.ariaControlsElements=null,"
    "!owner.hasAttribute('aria-controls')"
    "&&owner.ariaControlsElements===null));"
    "t('type',()=>{try{owner.ariaActiveDescendantElement={};return false}"
    "catch(e){return isTypeError(e)}});"
    COMPAT_SUMMARY("ARIA-REFLECTION-OK");

/* Everywhere else only the ARIA members every site had before the set was
   added (v0.1.50: role, ariaAtomic, ariaLive) exist, reflecting their
   content attributes; no other ARIA member exists, on elements or their
   prototypes, and the content attributes still work. */
static const char aria_absent_probe[] =
    COMPAT_PRELUDE
    "const kept=['role','ariaAtomic','ariaLive'],names=" ARIA_STRING_NAMES
    ".concat(" ARIA_ELEMENT_NAMES ").filter(n=>!kept.includes(n));"
    "const img=document.createElement('img');"
    "t('kept',()=>kept.every(n=>{const d=Object.getOwnPropertyDescriptor("
    "Element.prototype,n),attr=n==='role'?n:'aria-'+n.slice(4).toLowerCase();"
    "if(!d||!d.enumerable||!d.configurable||img[n]!==null)return false;"
    "img[n]='dialog';const set=img.getAttribute(attr);"
    "img.setAttribute(attr,'w');const got=img[n];img[n]=null;"
    "return set==='dialog'&&got==='w'&&!img.hasAttribute(attr)"
    "&&!Object.hasOwn(img,n)}));"
    "t('absent',()=>names.every(n=>!(n in img)&&!(n in Element.prototype)"
    "&&!(n in document.body)));"
    "t('attributes',()=>(img.setAttribute('aria-label','x'),"
    "img.getAttribute('aria-label')==='x'&&img.ariaLabel===undefined));"
    "t('no host channel',()=>!('__tilefinchHostChannel' in globalThis));"
    COMPAT_SUMMARY("ARIA-ABSENT-OK");

/* Event handler IDL attributes: the standard GlobalEventHandlers on
   elements, documents and windows, Fullscreen's on Element and Document,
   and Pointer Lock's and Page Visibility's on Document alone. Each is null
   until set and then runs for a dispatched event of its type. The handler
   attributes HTML does not define are absent. */
static const char event_handler_attributes_probe[] =
    COMPAT_PRELUDE
    "const added=('beforematch canplaythrough command "
    "contentvisibilityautostatechange contextlost contextrestored cuechange "
    "formdata gotpointercapture lostpointercapture pointercancel "
    "pointerrawupdate progress resize scrollend securitypolicyviolation "
    "selectstart slotchange webkitanimationend webkitanimationiteration "
    "webkitanimationstart webkittransitionend').split(' ');"
    "const img=document.createElement('img'),"
    "svg=document.createElementNS('http://www.w3.org/2000/svg','g');"
    "t('present',()=>added.every(type=>img['on'+type]===null"
    "&&svg['on'+type]===null&&document['on'+type]===null"
    "&&('on'+type) in window));"
    "t('own interface',()=>added.every(type=>['HTMLElement','SVGElement',"
    "'Document','Window'].every(name=>{const d=Object"
    ".getOwnPropertyDescriptor(globalThis[name].prototype,'on'+type);"
    "return name==='Window'&&type==='resize'&&!d"
    "?Object.hasOwn(window,'onresize'):d&&d.enumerable"
    "&&d.get.name==='get on'+type})));"
    "t('dispatch',()=>added.every(type=>{let hits=0,self=null;"
    "img['on'+type]=function(event){hits+=event.type===type?1:0;self=this};"
    "img.dispatchEvent(new Event(type));const set=img['on'+type];"
    "img['on'+type]=null;img.dispatchEvent(new Event(type));"
    "return hits===1&&self===img&&typeof set==='function'"
    "&&img['on'+type]===null}));"
    "t('content attribute',()=>{const e=document.createElement('div');"
    "e.setAttribute('onprogress','window.__progressHit=event.type');"
    "e.dispatchEvent(new Event('progress'));"
    "return window.__progressHit==='progress'"
    "&&typeof e.onprogress==='function'});"
    "t('document dispatch',()=>{let hit=0;document.onscrollend=()=>{hit++};"
    "document.dispatchEvent(new Event('scrollend'));"
    "document.onscrollend=null;return hit===1});"
    "t('fullscreen',()=>img.onfullscreenchange===null"
    "&&img.onfullscreenerror===null&&document.onfullscreenchange===null"
    "&&svg.onfullscreenchange===null&&!('onfullscreenchange' in window)"
    "&&Object.hasOwn(Element.prototype,'onfullscreenerror'));"
    "t('document only',()=>document.onpointerlockchange===null"
    "&&document.onpointerlockerror===null"
    "&&document.onvisibilitychange===null&&!('onvisibilitychange' in img)"
    "&&!('onpointerlockchange' in img)&&!('onvisibilitychange' in window));"
    "t('not in HTML',()=>['oncompositionstart','oncompositionupdate',"
    "'oncompositionend','onfocusin','onfocusout','onbeforecopy','onsearch',"
    "'onmousewheel'].every(name=>!(name in img)&&!(name in document)));"
    COMPAT_SUMMARY("EVENT-HANDLER-ATTRIBUTES-OK");

/* What enumerating an element or event shows: no internal plumbing, no
   member of another element's interface, and the Web IDL constants and
   legacy Event members. Each such member is on its own interface's
   prototype and its elements. */
static const char member_ownership_probe[] =
    COMPAT_PRELUDE
    "const keys=object=>{const seen=[];for(const key in object)seen.push(key);"
    "return seen};"
    "const img=document.createElement('img'),imgKeys=keys(img);"
    "t('no plumbing',()=>imgKeys.every(key=>!key.startsWith('__'))"
    "&&keys(document.createEvent('MouseEvents')).every("
    "key=>!key.startsWith('__'))"
    "&&keys(document.createElement('div')).every(key=>!key.startsWith('__'))"
    "&&keys(document.createTextNode('')).every(key=>!key.startsWith('__')));"
    "const foreign=('async checked content contentDocument contentWindow "
    "defer disabled getElementById href hreflang placeholder rel sandbox "
    "select selected selectionDirection selectionEnd selectionStart "
    "setSelectionRange type value').split(' ');"
    "t('img',()=>foreign.every(name=>!(name in img)&&img[name]===undefined)"
    "&&['src','width','height','name','alt','id','tagName'].every("
    "name=>name in img));"
    "t('div',()=>foreign.concat(['src','width','height','name']).every("
    "name=>!(name in document.createElement('div'))));"
    "t('text',()=>foreign.every(name=>!(name in document.createTextNode(''))));"
    "t('expando',()=>{const div=document.createElement('div');div.value='x';"
    "return div.value==='x'&&!div.hasAttribute('value')"
    "&&Object.hasOwn(div,'value')});"
    "const owners={value:'Input TextArea Select Option Button Output LI Data "
    "Meter Progress Param',checked:'Input',selected:'Option',"
    "placeholder:'Input TextArea',selectionStart:'Input TextArea',"
    "select:'Input TextArea',disabled:'Input Button Select TextArea FieldSet "
    "OptGroup Option Link Style',href:'Anchor Area Base Link',"
    "hreflang:'Anchor Link',rel:'Anchor Area Link Form',"
    "src:'Image IFrame Script Input Source Track Embed Audio Video',"
    "sandbox:'IFrame',async:'Script',defer:'Script',content:'Meta Template',"
    "contentWindow:'IFrame Frame Object',contentDocument:'IFrame Frame Object',"
    "width:'Image Canvas IFrame Video Table',height:'Image Canvas IFrame Video',"
    "name:'Input Form Button IFrame Image Anchor Meta Map',"
    "type:'Input Button Script Style Link Anchor OList LI'};"
    "const tags={Anchor:'a',Image:'img',TextArea:'textarea',OList:'ol',"
    "OptGroup:'optgroup',FieldSet:'fieldset',IFrame:'iframe'};"
    "t('owners',()=>Object.keys(owners).every(name=>owners[name].split(' ')"
    ".every(stem=>{const element=document.createElement("
    "tags[stem]||stem.toLowerCase());"
    "return element instanceof globalThis['HTML'+stem+'Element']"
    "&&name in element&&name in globalThis['HTML'+stem+'Element'].prototype"
    "&&!Object.hasOwn(HTMLElement.prototype,name)})));"
    "t('prototype setter',()=>{const input=document.createElement('input');"
    "Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set"
    ".call(input,'typed');return input.value==='typed'});"
    "t('values',()=>{const li=document.createElement('li');li.value=3;"
    "const a=document.createElement('a');a.href='/x';"
    "const input=document.createElement('input');input.checked=true;"
    "return li.getAttribute('value')==='3'&&a.href==='https://conformance.test/x'"
    "&&input.checked&&input.type==='text'"
    "&&document.createElement('template').content.nodeType===11"
    "&&document.createDocumentFragment().getElementById('x')===null});"
    "customElements.define('own-er',class extends HTMLElement{});"
    "const custom=document.body.appendChild(document.createElement('own-er'));"
    "t('custom element',()=>foreign.every(name=>!(name in custom))"
    "&&custom.tagName==='OWN-ER'&&custom.childNodes.length===0"
    "&&keys(custom).every(key=>!key.startsWith('__')));"
    "const mouse=document.createEvent('MouseEvents'),mouseKeys=keys(mouse);"
    "t('event members',()=>['NONE','CAPTURING_PHASE','AT_TARGET',"
    "'BUBBLING_PHASE','cancelBubble','returnValue','srcElement'].every("
    "name=>mouseKeys.includes(name))&&mouse.NONE===0&&mouse.AT_TARGET===2"
    "&&Event.BUBBLING_PHASE===3&&Object.keys(Event).length===4"
    "&&Object.keys(KeyboardEvent).length===4);"
    COMPAT_SUMMARY("MEMBER-OWNERSHIP-OK");

/* A frame's window.eval is its realm's own eval, for its scripts and its
   parent alike: the same function everywhere, direct inside the frame and
   indirect (frame realm, frame global scope) from the parent. */
static const char frame_eval_identity_probe[] =
    COMPAT_PRELUDE
    "const f=document.createElement('iframe');document.body.appendChild(f);"
    "const w=f.contentWindow;"
    "t('stable',()=>w.eval===w.eval&&w.eval===f.contentWindow.eval);"
    "t('inside',()=>w.eval('eval===window.eval&&eval===self.eval"
    "&&eval===globalThis.eval'));"
    "t('same function',()=>w.eval('eval')===w.eval&&w.eval!==eval);"
    "t('realm',()=>w.eval('[]') instanceof w.Array"
    "&&!(w.eval('[]') instanceof Array));"
    "t('this',()=>w.eval('this')===w);"
    "t('direct inside',()=>w.eval('(function(){var local=6;"
    "return eval(\"local*7\")})()')===42);"
    "t('indirect from parent',()=>{var local=1;"
    "try{w.eval('local');return false}catch(e){return e.name==='ReferenceError'}});"
    "t('lexical per call',()=>(w.eval('let once=1'),"
    "w.eval('typeof once')==='undefined'));"
    COMPAT_SUMMARY("FRAME-EVAL-OK");

/* A frame's document is a typed document like the page's: its elements are
   HTMLImageElement and the rest, with their members, and the document has
   the frame's URL, Location, WindowProxy and the initial about:blank
   document's quirks mode. */
static const char frame_document_probe[] =
    COMPAT_PRELUDE
    "const f=document.createElement('iframe');document.body.appendChild(f);"
    "const w=f.contentWindow,d=f.contentDocument;"
    "t('same',()=>d===w.document&&d.defaultView===w);"
    "t('url',()=>d.URL==='about:blank'&&d.documentURI==='about:blank'"
    "&&d.location===w.location&&d.readyState==='complete');"
    "t('quirks',()=>d.compatMode==='BackCompat'&&d.doctype===null);"
    "t('tree',()=>d.documentElement instanceof HTMLHtmlElement"
    "&&d.head instanceof HTMLHeadElement&&d.body instanceof HTMLBodyElement);"
    "const img=d.createElement('img');"
    "t('typed',()=>img instanceof HTMLImageElement"
    "&&img.constructor===HTMLImageElement"
    "&&d.createElement('div').constructor===HTMLDivElement"
    "&&d.createElement('a') instanceof HTMLAnchorElement);"
    "t('members',()=>img.name===''&&img.alt===''&&img.width===0"
    "&&(img.width=12,img.width===12&&img.getAttribute('width')==='12')"
    "&&new Image().height===0&&document.createElement('img').width===0"
    "&&img.complete===true&&img.ownerDocument===d);"
    "d.body.innerHTML='<img name=n alt=a><input type=checkbox checked>';"
    "t('parsed',()=>d.querySelector('img') instanceof HTMLImageElement"
    "&&d.querySelector('img').name==='n'&&d.querySelector('img').alt==='a'"
    "&&d.querySelector('input').checked===true);"
    "t('not page',()=>d!==document&&!document.contains(d.body)"
    "&&d.body.ownerDocument===d);"
    COMPAT_SUMMARY("FRAME-DOCUMENT-OK");

/* WebGL's interface objects exist before any context does: feature tests
   and constant reads find WebGLRenderingContext and its constants (on the
   interface object and its prototype), the object interfaces throw
   "Illegal constructor", and a context and its objects are instances of
   those interfaces. WebGL 2 is not exposed, so neither is its interface. */
static const char webgl_interfaces_probe[] =
    COMPAT_PRELUDE
    "const names=['WebGLObject','WebGLBuffer','WebGLFramebuffer',"
    "'WebGLProgram','WebGLRenderbuffer','WebGLShader','WebGLTexture',"
    "'WebGLUniformLocation','WebGLActiveInfo','WebGLShaderPrecisionFormat',"
    "'WebGLRenderingContext','WebGLContextEvent'];"
    "t('present',()=>names.every(n=>typeof window[n]==='function'"
    "&&Object.getOwnPropertyNames(window).includes(n)));"
    "const G=WebGLRenderingContext;"
    "t('constants',()=>G.DEPTH_BUFFER_BIT===0x100&&G.COLOR_BUFFER_BIT===0x4000"
    "&&G.TRIANGLES===4&&G.TEXTURE31===0x84df&&G.RGB565===0x8d62"
    "&&G.prototype.STATIC_DRAW===0x88e4&&G.BROWSER_DEFAULT_WEBGL===0x9244"
    "&&G.VERTEX_ARRAY_BINDING_OES===undefined);"
    "t('constant descriptor',()=>{const d=Object.getOwnPropertyDescriptor(G,"
    "'FLOAT');return d.value===0x1406&&d.enumerable&&!d.writable"
    "&&!d.configurable});"
    "t('illegal',()=>names.slice(0,11).every(n=>{try{new window[n]();"
    "return false}catch(e){return e instanceof TypeError}}));"
    "t('inheritance',()=>Object.getPrototypeOf(WebGLBuffer)===WebGLObject"
    "&&Object.getPrototypeOf(WebGLTexture.prototype)===WebGLObject.prototype"
    "&&Object.getPrototypeOf(WebGLContextEvent.prototype)===Event.prototype);"
    "t('class string',()=>Object.prototype.toString.call(G.prototype)"
    "==='[object WebGLRenderingContext]');"
    "t('context event',()=>{const e=new WebGLContextEvent('webglcontextlost',"
    "{statusMessage:'gone'});return e.type==='webglcontextlost'"
    "&&e.statusMessage==='gone'&&e instanceof Event"
    "&&new WebGLContextEvent('x').statusMessage===''});"
    "t('no webgl2',()=>typeof WebGL2RenderingContext==='undefined'"
    "&&document.createElement('canvas').getContext('webgl2')===null);"
    "const gl=document.createElement('canvas').getContext('webgl');"
    "t('context',()=>gl instanceof WebGLRenderingContext"
    "&&Object.getPrototypeOf(gl)===G.prototype&&gl.TRIANGLES===4"
    "&&typeof gl.drawArrays==='function'&&G.prototype.drawArrays===gl.drawArrays);"
    "t('objects',()=>gl.createBuffer() instanceof WebGLBuffer"
    "&&gl.createBuffer() instanceof WebGLObject"
    "&&gl.createTexture() instanceof WebGLTexture"
    "&&gl.createShader(gl.VERTEX_SHADER) instanceof WebGLShader"
    "&&gl.createProgram() instanceof WebGLProgram"
    "&&gl.createFramebuffer() instanceof WebGLFramebuffer"
    "&&gl.createRenderbuffer() instanceof WebGLRenderbuffer"
    "&&gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER,gl.HIGH_FLOAT)"
    " instanceof WebGLShaderPrecisionFormat);"
    "t('same names',()=>window.WebGLRenderingContext===G);"
    COMPAT_SUMMARY("WEBGL-INTERFACES-OK");

/* A page cannot reach into WebGL before its first context: the interface
   table webgl.js builds from is frozen, a replaced Reflect.construct never
   sees the context classes (so the context limit holds), and the members
   are on WebGLRenderingContext.prototype already, forwarding to the
   implementation once it loads. A wrapper the page put there first stays.
   The context-lost event is a WebGLContextEvent. */
static const char webgl_hardening_probe[] =
    COMPAT_PRELUDE
    "const G=WebGLRenderingContext,P=G.prototype,RC=Reflect.construct,"
    "seen=[];Reflect.construct=function(T,a,N){seen.push(T.name);"
    "return RC(T,a,N)};"
    "const table=window.__tilefinchWebGLInterfaces,Fake=function(){};"
    "t('table frozen',()=>Object.isFrozen(table));"
    "try{table.WebGLProgram=Fake}catch(_){}"
    "t('members first',()=>typeof P.drawArrays==='function'"
    "&&P.drawArrays.length===3&&P.drawArrays.name==='drawArrays'"
    "&&typeof Object.getOwnPropertyDescriptor(P,'drawingBufferWidth').get"
    "==='function'&&!Object.getOwnPropertyDescriptor(P,'clear').enumerable);"
    "t('member receiver',()=>{try{P.getError.call({});return false}"
    "catch(e){return e.message==='Illegal invocation'}});"
    "const real=P.getParameter;let wrapped=0;"
    "P.getParameter=function(p){wrapped++;return real.call(this,p)};"
    "const canvases=[];let made=0;"
    "for(let i=0;i<4;i++){const c=document.createElement('canvas');"
    "document.body.appendChild(c);canvases.push(c);"
    "if(c.getContext('webgl'))made++}"
    "const gl=canvases[0].getContext('webgl');"
    "Reflect.construct=RC;"
    "t('context limit',()=>made===2);"
    "t('construct unseen',()=>seen.length===0);"
    "t('real program',()=>{const p=gl.createProgram();"
    "return p instanceof WebGLProgram&&!(p instanceof Fake)});"
    "t('wrapper kept',()=>gl.getParameter(gl.MAX_TEXTURE_SIZE)>0"
    "&&wrapped===1&&P.getParameter!==real);"
    "t('forwarding',()=>{gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "return gl.getError()===0&&gl.drawingBufferWidth===300});"
    "let lost=null;gl.canvas.addEventListener('webglcontextlost',"
    "e=>{lost=e});gl._lose('lost for the test');"
    "t('lost event',()=>lost instanceof WebGLContextEvent"
    "&&lost.statusMessage==='lost for the test'&&lost.cancelable);"
    COMPAT_SUMMARY("WEBGL-HARDENING-OK");

/* A page that froze WebGLRenderingContext.prototype before its first
   context still gets working contexts. */
static const char webgl_frozen_probe[] =
    COMPAT_PRELUDE
    "const G=WebGLRenderingContext;Object.freeze(G.prototype);"
    "const gl=document.createElement('canvas').getContext('webgl');"
    "t('context',()=>gl instanceof G&&gl.drawingBufferWidth===300);"
    "t('draws',()=>{gl.clearColor(1,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);"
    "return gl.getError()===0&&gl.createBuffer() instanceof WebGLBuffer});"
    "t('still frozen',()=>Object.isFrozen(G.prototype));"
    COMPAT_SUMMARY("WEBGL-FROZEN-OK");

/* Every promise-returning operation the receiver marking leaves unmarked
   (runtime_promise_operations) names a real interface member once the
   features that define them have loaded: a stale entry would be skipped
   silently, and the operation would throw instead of rejecting. */
static const char promise_operations_probe[] =
    "void indexedDB;new ReadableStream();new WritableStream();"
    "void navigator.mediaDevices;void document.fonts;"
    "try{new AudioContext()}catch(_){}"
    "navigator.storage.getDirectory().catch(()=>{});"
    "void FileSystemHandle;"
    "const left=__tilefinchUnresolvedPromiseOperations();"
    "globalThis.pocSummary=left.length?'UNRESOLVED '+left.join(' ')"
    ":'PROMISE-OPERATIONS-OK';";

/* Navigation Timing's legacy performance.timing: its members in IDL order
   (secureConnectionStart included), moments that have not come yet read 0
   while the document parses, domLoading is the document's creation, and
   after load every lifecycle moment is set, in order, and agrees with the
   navigation entry. */
static const char navigation_timing_probe[] =
    COMPAT_PRELUDE
    "const T=performance.timing,o=performance.timeOrigin,"
    "n=performance.getEntriesByType('navigation')[0],"
    "life=['domInteractive','domContentLoadedEventStart',"
    "'domContentLoadedEventEnd','domComplete','loadEventStart',"
    "'loadEventEnd'];"
    "t('order',()=>Object.keys(T).join()==='navigationStart,unloadEventStart,"
    "unloadEventEnd,redirectStart,redirectEnd,fetchStart,domainLookupStart,"
    "domainLookupEnd,connectStart,connectEnd,secureConnectionStart,"
    "requestStart,responseStart,responseEnd,domLoading,domInteractive,"
    "domContentLoadedEventStart,domContentLoadedEventEnd,domComplete,"
    "loadEventStart,loadEventEnd');"
    "t('parsing',()=>document.readyState==='loading'&&life.every(k=>T[k]===0"
    "&&n[k]===0)&&T.secureConnectionStart===0);"
    "t('dom loading',()=>T.domLoading>=T.fetchStart&&T.domLoading<=o"
    "+performance.now());"
    "t('fetch phases',()=>T.navigationStart===o&&T.fetchStart===o"
    "&&T.domainLookupStart===o&&T.requestStart===o);"
    "globalThis.pocSummary='PENDING';"
    "addEventListener('load',()=>queueMicrotask(()=>{"
    "t('after load',()=>life.every(k=>T[k]===o+n[k]&&n[k]>0));"
    "t('in order',()=>{const c=['fetchStart','domLoading',...life];"
    "for(let i=1;i<c.length;i++)if(T[c[i]]<T[c[i-1]])return false;"
    "return n.duration===n.loadEventEnd&&n.domInteractive"
    ">=n.responseStart});"
    COMPAT_SUMMARY("NAVIGATION-TIMING-OK") "}));";

static int test_window_and_document_constants(void)
{
    puts("test: window position, name, status, no-op operations, characterSet, frame globals, reflected attributes, frame eval, frame documents, WebGL interfaces, navigation timing");
    int failed = 0;
    if (!run_probe("window-members", window_members_probe,
                   "WINDOW-MEMBERS-OK"))
        failed++;
    if (!run_probe_after("character-set",
                         "<meta charset=windows-1251><p>text</p>",
                         character_set_probe, "CHARACTER-SET-OK"))
        failed++;
    if (!run_probe("reflected-attributes", reflected_attributes_probe,
                   "REFLECTED-ATTRIBUTES-OK"))
        failed++;
    if (!run_probe("element-core", element_core_probe, "ELEMENT-CORE-OK"))
        failed++;
    if (!run_probe("child-nodes", child_nodes_probe, "CHILD-NODES-OK"))
        failed++;
    /* The ARIA reflections: present on google.com and its subdomains (the
       sign-in page and its /_/bscframe child frame among them), absent
       elsewhere, look-alike hosts included. */
    static const char *const aria_sites[] = {
        "https://accounts.google.com/v3/signin/identifier",
        "https://accounts.google.com/_/bscframe",
        "https://www.google.com/", "https://google.com/",
    };
    static const char *const aria_absent_sites[] = {
        "https://conformance.test/start", "https://notgoogle.com/",
        "https://google.com.example.test/", "https://www.youtube.com/",
    };
    for (size_t i = 0; i < sizeof(aria_sites) / sizeof(aria_sites[0]); i++)
        if (!run_probe_at(aria_sites[i], aria_sites[i], "<p id=p>text</p>",
                          aria_reflection_probe, "ARIA-REFLECTION-OK"))
            failed++;
    for (size_t i = 0; i < sizeof(aria_absent_sites)
                               / sizeof(aria_absent_sites[0]); i++)
        if (!run_probe_at(aria_absent_sites[i], aria_absent_sites[i],
                          "<p id=p>text</p>", aria_absent_probe,
                          "ARIA-ABSENT-OK"))
            failed++;
    /* The same probes in a child frame's realm: the sign-in page's frames
       have the reflections, other pages' do not. */
    {
        static char body[24 * 1024];
        int written = snprintf(body, sizeof(body),
            "<p id=p>text</p><script type=text/x-probe id=present>%s"
            "</script><script type=text/x-probe id=absent>%s</script>",
            aria_reflection_probe, aria_absent_probe);
        if (written < 0 || (size_t) written >= sizeof(body)) {
            fprintf(stderr, "ARIA frame probe too large\n");
            failed++;
        } else {
            if (!run_probe_at("aria-child-frame",
                              "https://accounts.google.com/v3/signin/identifier",
                              body, aria_frame_probe("present"),
                              "ARIA-REFLECTION-OK"))
                failed++;
            if (!run_probe_at("aria-absent-child-frame",
                              "https://conformance.test/start", body,
                              aria_frame_probe("absent"), "ARIA-ABSENT-OK"))
                failed++;
        }
    }
    if (!run_probe("event-handler-attributes",
                   event_handler_attributes_probe,
                   "EVENT-HANDLER-ATTRIBUTES-OK"))
        failed++;
    if (!run_probe("member-ownership", member_ownership_probe,
                   "MEMBER-OWNERSHIP-OK"))
        failed++;
    if (!run_probe("frame-document", frame_document_probe,
                   "FRAME-DOCUMENT-OK"))
        failed++;
    if (!run_probe("frame-eval", frame_eval_identity_probe, "FRAME-EVAL-OK"))
        failed++;
    if (!run_probe("frame-global-names", frame_global_names_probe,
                   "FRAME-GLOBAL-NAMES-OK"))
        failed++;
    if (!run_probe("webgl-interfaces", webgl_interfaces_probe,
                   "WEBGL-INTERFACES-OK"))
        failed++;
    if (!run_probe("webgl-hardening", webgl_hardening_probe,
                   "WEBGL-HARDENING-OK"))
        failed++;
    if (!run_probe("webgl-frozen", webgl_frozen_probe, "WEBGL-FROZEN-OK"))
        failed++;
    if (!run_probe("promise-operations", promise_operations_probe,
                   "PROMISE-OPERATIONS-OK"))
        failed++;
    if (!run_probe("navigation-timing", navigation_timing_probe,
                   "NAVIGATION-TIMING-OK"))
        failed++;
    return failed;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--writable-only"))
        return test_writable_terminal_handoffs();
    int failures = 0;
    failures += test_time_zone_agreement();
    failures += test_function_shapes();
    failures += test_document_surface();
    failures += test_location_navigation_timing();
    failures += test_child_navigables();
    failures += test_census_api_gaps();
    failures += test_inline_handler_scope();
    failures += test_handler_form_members_and_unscopables();
    failures += test_platform_methods_present_as_native();
    failures += test_init_events_and_token_list();
    failures += test_large_character_data();
    failures += test_writable_terminal_handoffs();
    failures += test_window_and_document_constants();
    if (failures != 0) return 1;
    puts("web conformance tests passed");
    return 0;
}
