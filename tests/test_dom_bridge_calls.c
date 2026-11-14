/* Script-to-DOM bridge calls that pages make in bulk (Internet Archive's
   lit components: template walks, @queryAll reads, per-frame cell
   measurement). Each cheaper path is checked twice: against the semantics a
   page can observe (descriptors, identity, liveness, overrides, filter
   ordering), and by a count that shows the bridge does less work. */
#include "tilefinch_test_common.h"

typedef struct {
    Budget budget;
    NavigationSession session;
} BridgePage;

static bool bridge_page_open(BridgePage *page, const char *html)
{
    budget_init(&page->budget, 24u * MIB);
    if (!budget_install_lexbor(&page->budget)) return false;
    if (!navigation_init(&page->session, &page->budget, 2)) return false;
    navigation_enable_scripts(&page->session, 6u * MIB, 4000);
    uint64_t generation = navigation_begin(&page->session);
    return navigation_commit_html(&page->session, generation,
                                  "https://bridge-calls.test/", html,
                                  strlen(html), 480, NULL, NULL, true)
        && page->session.page.runtime != NULL;
}

static bool bridge_page_eval(BridgePage *page, const char *script,
                             const char *expected)
{
    ScriptResult *result = &page->session.page.script_result;
    if (!script_runtime_evaluate_diagnostic(page->session.page.runtime,
                                            script, "<bridge-calls>",
                                            result)) {
        fprintf(stderr, "bridge-calls: evaluation failed: %s %s\n",
                result->summary, result->error);
        return false;
    }
    if (strcmp(result->summary, expected) != 0) {
        fprintf(stderr, "bridge-calls: expected '%s', got '%s'\n", expected,
                result->summary);
        return false;
    }
    return true;
}

static bool bridge_page_close(BridgePage *page)
{
    navigation_destroy(&page->session);
    return page->budget.current == 0 && budget_uninstall_lexbor(&page->budget);
}

static const char bridge_page_html[] =
    "<!doctype html><html><body>"
    "<div id=list><span class=c id=a>a</span><span class=c id=b>b</span>"
    "<i><span class=c id=c>c</span></i></div>"
    "<div id=cells style='width:200px'>"
    "<div class=cell style='height:20px' data-cell-index=0 data-rendered></div>"
    "<div class=cell style='height:31px' data-cell-index=1></div>"
    "<div class=cell style='height:0px'></div>"
    "<div class=cell style='display:none;height:9px'></div>"
    "</div><x-host id=host></x-host></body></html>";

/* A static NodeList: an ordinary object of NodeList.prototype whose indices
   are enumerable, configurable, read-only own data properties and whose
   length is a configurable, read-only, non-enumerable own data property.
   Every receiver kind, the empty list, and a fresh object per call. */
static int test_query_all_list_semantics(void)
{
    puts("test: querySelectorAll returns the same static NodeList shape");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    CHECK(bridge_page_eval(&page,
        "(()=>{const out=[];"
        "const shape=(l)=>{const names=Object.getOwnPropertyNames(l).join(',');"
        "const d=Object.getOwnPropertyDescriptors(l);let flags='';"
        "for(const k of Object.getOwnPropertyNames(l)){const x=d[k];"
        "flags+=k+':'+(x.writable?'w':'-')+(x.enumerable?'e':'-')"
        "+(x.configurable?'c':'-')+(typeof x.get)+';'}"
        "return names+'|'+flags+'|'+Object.keys(l).join(',')"
        "+'|'+Object.getOwnPropertySymbols(l).length"
        "+'|'+(Object.getPrototypeOf(l)===NodeList.prototype)"
        "+'|'+(l instanceof NodeList)+'|'+Array.isArray(l)"
        "+'|'+Object.prototype.toString.call(l)+'|'+Object.isExtensible(l)"
        "+'|'+Object.isFrozen(l)};"
        "const list=document.getElementById('list');"
        "const host=document.getElementById('host');"
        "const root=host.attachShadow({mode:'open'});"
        "root.innerHTML='<b class=c>x</b><b class=c>y</b>';"
        "const frag=document.createDocumentFragment();"
        "frag.appendChild(document.createElement('span')).className='c';"
        "out.push(shape(document.querySelectorAll('.c')));"
        "out.push(shape(list.querySelectorAll('.c')));"
        "out.push(shape(frag.querySelectorAll('.c')));"
        "out.push(shape(root.querySelectorAll('.c')));"
        "out.push(shape(list.querySelectorAll('.none')));"
        "const l=list.querySelectorAll('span');"
        "let forIn='';for(const k in l)forIn+=k+',';"
        "let iterated='';for(const n of l)iterated+=n.id;"
        "let each='';l.forEach((n,i,self)=>each+=i+n.id+(self===l));"
        "out.push(forIn,iterated,each,[...l.keys()].join(''),"
        "[...l.values()].map(n=>n.id).join(''),"
        "[...l.entries()].map(e=>e[0]+e[1].id).join(''));"
        "out.push(String(l.item(0)===l[0]&&l.item(2)===l[2]"
        "&&l.item(3)===null&&l.item(-1)===null&&l.item('1')===l[1]"
        "&&l.item(4294967297)===l[1]&&l[3]===undefined));"
        "out.push(String(l[Symbol.iterator]===NodeList.prototype[Symbol.iterator]"
        "&&l.item===NodeList.prototype.item"
        "&&!Object.prototype.hasOwnProperty.call(l,'item')));"
        "out.push(String(list.querySelectorAll('span')!==l"
        "&&list.querySelectorAll('span')[0]===l[0]));"
        "l[0]=null;l.length=9;l[7]=1;"
        "out.push(l[0].id+l.length+l[7]+delete l[1]+l.length);"
        "const late=document.createElement('span');list.appendChild(late);"
        "out.push(String(list.querySelectorAll('span').length)+"
        "document.querySelectorAll('#list span').length);"
        "const saved=NodeList.prototype.item;"
        "NodeList.prototype.item=function(){return 'page'};"
        "out.push(list.querySelectorAll('span').item(0));"
        "NodeList.prototype.item=saved;"
        "globalThis.pocSummary=out.join('\\n');})()",
        "0,1,2,length|0:-ecundefined;1:-ecundefined;2:-ecundefined;length:--cundefined;|0,1,2|0|true|true|false|[object NodeList]|true|false\n"
        "0,1,2,length|0:-ecundefined;1:-ecundefined;2:-ecundefined;length:--cundefined;|0,1,2|0|true|true|false|[object NodeList]|true|false\n"
        "0,length|0:-ecundefined;length:--cundefined;|0|0|true|true|false|[object NodeList]|true|false\n"
        "0,1,length|0:-ecundefined;1:-ecundefined;length:--cundefined;|0,1|0|true|true|false|[object NodeList]|true|false\n"
        "length|length:--cundefined;||0|true|true|false|[object NodeList]|true|false\n"
        "0,1,2,\n"
        "abc\n"
        "0atrue1btrue2ctrue\n"
        "012\n"
        "abc\n"
        "0a1b2c\n"
        "true\n"
        "true\n"
        "true\n"
        "a31true3\n"
        "44\n"
        "page"));
    CHECK(bridge_page_close(&page));
    return 0;
}

/* The list is built in the native call: no bootstrap copy that defines
   each entry through the page-replaceable Object.defineProperty (a page
   could count those calls, and lit's @queryAll pays them on every read). */
static int test_query_all_builds_list_natively(void)
{
    puts("test: querySelectorAll defines no entries through script");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    CHECK(bridge_page_eval(&page,
        "(()=>{const define=Object.defineProperty;let calls=0;"
        "Object.defineProperty=function(o,k,d){calls++;"
        "return define.call(this,o,k,d)};"
        "let total=0;try{"
        "total+=document.getElementById('cells').querySelectorAll('.cell')"
        ".length;total+=document.querySelectorAll('span').length;"
        "}finally{Object.defineProperty=define}"
        "globalThis.pocSummary=total+':'+calls;})()",
        "7:0"));
    CHECK(bridge_page_close(&page));
    return 0;
}

/* A page that removes its <head> while it is still being parsed: the
   parser goes on inserting "after head" elements into that removed head
   (as the HTML parsing algorithm says). Replacing the root's children
   after load detaches the parser's head with no wrapper, which used to
   destroy it at once while Lexbor still pointed at it; a later meta CSP
   insertion reads that pointer. Freed Lexbor memory is not poisoned, so
   this checks the path runs and answers as a browser would, not the
   read itself. */
static int test_removed_head_outlives_parse(void)
{
    puts("test: a head removed during parsing stays usable to the parser");
    BridgePage page;
    CHECK(bridge_page_open(&page,
        "<!doctype html><html><head><title>A</title></head>"
        "<script>document.head.remove();globalThis.seen=[];</script>"
        "<script>seen.push(String(document.head))</script>"
        "<meta id=m name=x><link id=l rel=x><title>B</title>"
        "<body><p id=p>x</p></body></html>"));
    CHECK(bridge_page_eval(&page,
        "globalThis.pocSummary=seen.join()+'|'+document.getElementById('m')"
        "+'|'+document.getElementById('l')+'|'+document.title+'|'"
        "+document.getElementById('p').id+'|'+document.head",
        "null|null|null||p|null"));
    CHECK(bridge_page_eval(&page,
        "(()=>{document.documentElement.innerHTML='<body><p id=q></p></body>';"
        "const m=document.createElement('meta');m.httpEquiv="
        "'Content-Security-Policy';m.content=\"img-src 'none'\";"
        "document.body.append(m);"
        "globalThis.pocSummary=String(document.querySelectorAll('meta')"
        ".length)})()", "1"));
    CHECK(bridge_page_close(&page));
    CHECK(bridge_page_open(&page,
        "<!doctype html><html><head><title>A</title></head>"
        "<body><p id=p>x</p></body></html>"));
    CHECK(bridge_page_eval(&page,
        "(()=>{document.documentElement.innerHTML='<body><p id=q></p></body>';"
        "const m=document.createElement('meta');m.httpEquiv="
        "'Content-Security-Policy';m.content=\"img-src 'none'\";"
        "document.body.append(m);document.title='C';"
        "globalThis.pocSummary=document.head+'|'+document.title})()",
        "[object HTMLHeadElement]|C"));
    CHECK(bridge_page_close(&page));
    return 0;
}

/* A node registered while the handle table has no free slot (each new
   node while a page builds) does not read the whole free-slot bitmap to
   find that out: lit's template walks register thousands. */
static int test_new_nodes_skip_full_slot_search(void)
{
    puts("test: new nodes do not search a full handle table");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    ScriptResult *result = &page.session.page.script_result;
    size_t words = result->dom_handle_slot_search_words;
    size_t reuses = result->dom_handle_slot_reuses;
    CHECK(bridge_page_eval(&page,
        "(()=>{globalThis.kept=[];for(let i=0;i<3000;i++)"
        "kept.push(document.createElement('p'));"
        "globalThis.pocSummary=String(kept.length);})()", "3000"));
    CHECK(result->dom_handle_slot_search_words - words < 3000);
    CHECK(result->dom_handle_slot_reuses == reuses);
    CHECK(bridge_page_close(&page));
    return 0;
}

/* Queries from the document or a shadow root never see into a shadow tree
   below them, whichever query, and a light-DOM node after a shadow tree too
   large for one walk (DOM_TRAVERSAL_VISIT_LIMIT nodes) is still found: the
   walk steps over the tree instead of through it. */
static int test_query_steps_over_shadow_trees(void)
{
    puts("test: queries step over shadow trees");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    CHECK(bridge_page_eval(&page,
        "(()=>{const out=[];const ids=l=>[...l].map(n=>n.id).join(',');"
        "const host=document.getElementById('host');"
        "const root=host.attachShadow({mode:'open'});"
        "root.innerHTML='<span class=c id=s1>s</span><div id=inner></div>';"
        "const inner=root.getElementById('inner').attachShadow({mode:'open'});"
        "inner.innerHTML='<span class=c id=s2></span>';"
        "const spans=document.getElementsByTagName('span');"
        "out.push(ids(document.querySelectorAll('.c'))+'|'"
        "+ids(root.querySelectorAll('.c'))+'|'+ids(inner.querySelectorAll("
        "'span'))+'|'+spans.length+'|'+document.getElementById('s1')+'|'"
        "+root.getElementById('s1').id+'|'+document.querySelector('#s2')+'|'"
        "+root.querySelector('span:last-of-type').id+'|'"
        "+document.getElementsByClassName('c').length);"
        "const late=document.createElement('span');late.className='c';"
        "late.id='d';host.after(late);"
        "out.push(spans.length+'|'+ids(document.querySelectorAll('.c')));"
        "const big=document.createElement('x-big');document.body.append(big);"
        "big.attachShadow({mode:'open'}).innerHTML="
        "'<i>'+'<b></b>'.repeat(33000)+'</i>';"
        "const after=document.createElement('b');after.className='late';"
        "after.id='e';big.after(after);"
        "out.push(document.querySelectorAll('.late').length+'|'"
        "+document.querySelector('b.late')?.id+'|'"
        "+document.getElementsByClassName('late').length+'|'"
        "+document.getElementsByTagName('b').length+'|'"
        "+document.getElementById('e')?.id+'|'+spans.length);"
        "globalThis.pocSummary=out.join('\\n');})()",
        "a,b,c|s1|s2|3|null|s1|null|s1|3\n"
        "4|a,b,c,d\n"
        "1|e|1|1|e|4"));
    CHECK(bridge_page_close(&page));
    return 0;
}

/* getBoundingClientRect (and offsetWidth/offsetHeight over it) reads the
   native geometry into a private array: no result object per call, which
   IA's per-frame cell measurement and ResizeObserver pay thousands of times.
   Other geometry readers still take the object. */
static int test_bounding_rect_builds_no_geometry_object(void)
{
    puts("test: getBoundingClientRect builds no geometry object");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    ScriptResult *result = &page.session.page.script_result;
    CHECK(bridge_page_eval(&page,
        "(()=>{const cells=[...document.querySelectorAll('.cell')];"
        "let sum=0;for(let i=0;i<10;i++)for(const c of cells)"
        "sum+=c.offsetHeight+c.getBoundingClientRect().width;"
        "globalThis.pocSummary=String(sum);})()",
        "6510"));
    size_t queries = result->geometry_queries;
    size_t objects = result->geometry_result_objects;
    CHECK(bridge_page_eval(&page,
        "(()=>{const cells=[...document.querySelectorAll('.cell')];"
        "let sum=0;for(let i=0;i<10;i++)for(const c of cells)"
        "sum+=c.offsetHeight+c.getBoundingClientRect().width;"
        "globalThis.pocSummary=String(sum);})()",
        "6510"));
    CHECK(result->geometry_queries - queries >= 60);
    CHECK(result->geometry_result_objects == objects);
    CHECK(bridge_page_eval(&page,
        "globalThis.pocSummary=String(document.getElementById('cells')"
        ".scrollHeight)", "51"));
    CHECK(result->geometry_result_objects == objects + 1);
    CHECK(bridge_page_close(&page));
    return 0;
}

/* TreeWalker over cloned template content the way lit walks it: one
   document-rooted walker (SHOW_ELEMENT | SHOW_COMMENT) moved onto each
   clone. The walk is live (a node removed or inserted between steps is
   observed), passes over text without wrapping it, and stops at the end of
   the clone. Filter callbacks see nodes in tree order with whatToShow
   applied first, REJECT skips a subtree and SKIP only the node, a throwing
   filter propagates and leaves the walker usable, and a reentrant step is
   an InvalidStateError. */
static int test_tree_walker_semantics(void)
{
    puts("test: TreeWalker steps keep their observable semantics");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    CHECK(bridge_page_eval(&page,
        "(()=>{const out=[];const t=document.createElement('template');"
        "t.innerHTML='<div id=d1><!--c1--><span id=s1>x</span><!--c2--></div>"
        "text<p id=p1><b id=b1></b></p><!--c3-->';"
        "const name=n=>n===null?'null':n.nodeType===8?'#'+n.data"
        ":n.nodeType===11?'frag':n===document?'doc':n.id;"
        "const w=document.createTreeWalker(document,129);"
        "const walk=(f,mutate)=>{w.currentNode=f;const seen=[];"
        "for(let n=w.nextNode(),i=0;n&&i<64;n=w.nextNode(),i++){"
        "seen.push(name(n));if(mutate)mutate(n)}return seen.join(' ')};"
        "const f1=document.importNode(t.content,true);"
        "out.push(walk(f1));"
        "out.push(String(w.currentNode===f1.lastChild)+name(w.root));"
        "out.push(walk(f1));"
        "const f2=document.importNode(t.content,true);"
        "out.push(walk(f2,n=>{if(n.id==='s1')n.nextSibling.remove();"
        "if(n.id==='p1')n.appendChild(document.createComment('late'))}));"
        "const f3=document.importNode(t.content,true);"
        "out.push(walk(f3,n=>{if(n.id==='d1')n.remove()}));"
        "const f4=document.importNode(t.content,true);"
        "w.currentNode=f4.querySelector('#s1');"
        "out.push(name(w.nextNode())+','+name(w.previousNode())+','"
        "+name(w.previousNode())+','+name(w.parentNode())+','"
        "+name(w.firstChild())+','+name(w.nextSibling())+','"
        "+name(w.lastChild()));"
        "w.currentNode=document;"
        "const calls=[];const f5=document.importNode(t.content,true);"
        "const fw=document.createTreeWalker(f5,NodeFilter.SHOW_ELEMENT,"
        "{acceptNode(n){calls.push(name(n)+':'+(this===undefined));"
        "return n.id==='d1'?NodeFilter.FILTER_REJECT:n.id==='p1'?"
        "NodeFilter.FILTER_SKIP:NodeFilter.FILTER_ACCEPT}});"
        "const accepted=[];for(let n;(n=fw.nextNode());)accepted.push(name(n));"
        "out.push(accepted.join(' ')+'|'+calls.join(' '));"
        "let thrown='';const tw=document.createTreeWalker(f5,1,"
        "n=>{if(n.id==='b1')throw new Error('boom');return 1});"
        "const tseen=[];try{for(let n;(n=tw.nextNode());)tseen.push(name(n))}"
        "catch(e){thrown=e.message}"
        "const after=name(tw.currentNode);tw.currentNode=f5.lastChild;"
        "out.push(tseen.join(' ')+'|'+thrown+'|'+after"
        "+'|'+name(tw.nextNode()));"
        "let inner='';const rw=document.createTreeWalker(f5,1,"
        "n=>{try{rw.nextNode()}catch(e){inner=e.name}return 1});"
        "rw.nextNode();out.push(inner);"
        "const fn=document.createTreeWalker(f5,1,function(n){"
        "return this===undefined?1:2});out.push(name(fn.nextNode()));"
        "out.push(String(w instanceof TreeWalker)+Object.prototype.toString"
        ".call(w)+Object.getOwnPropertyNames(w).length+w.whatToShow"
        "+(w.filter===null));"
        "globalThis.pocSummary=out.join('\\n');})()",
        "d1 #c1 s1 #c2 p1 b1 #c3\n"
        "truedoc\n"
        "d1 #c1 s1 #c2 p1 b1 #c3\n"
        "d1 #c1 s1 p1 b1 #late #c3\n"
        "d1 #c1 s1 #c2\n"
        "#c2,s1,#c1,d1,#c1,s1,null\n"
        "b1|d1:false p1:false b1:false\n"
        "d1 s1 p1|boom|p1|null\n"
        "InvalidStateError\n"
        "null\n"
        "true[object TreeWalker]0129true"));
    CHECK(bridge_page_close(&page));
    return 0;
}

/* offsetWidth/offsetHeight are the rounded getBoundingClientRect() box, and
   follow a page's replacement of that method (on the prototype or the
   element; the per-interface layer shadows Element.prototype's), of
   DOMRect, or of the geometry reader underneath; a style write or
   insertion between reads is seen by the next read; a detached or
   display:none element reads 0. */
static int test_offset_geometry_semantics(void)
{
    puts("test: offset geometry keeps its observable semantics");
    BridgePage page;
    CHECK(bridge_page_open(&page, bridge_page_html));
    CHECK(bridge_page_eval(&page,
        "(()=>{const out=[];const cells=[...document.querySelectorAll('.cell')];"
        "out.push(cells.map(c=>c.offsetHeight+'/'+c.offsetWidth).join(' '));"
        "out.push(String(cells.every(c=>c.offsetHeight===Math.floor("
        "c.getBoundingClientRect().height+0.5))));"
        "cells[0].style.height='50px';out.push(String(cells[0].offsetHeight));"
        "const extra=document.createElement('div');extra.style.height='7px';"
        "cells[1].before(extra);out.push(extra.offsetHeight+','+"
        "cells[1].getBoundingClientRect().top+','+cells[0].offsetHeight);"
        "extra.remove();out.push(extra.offsetHeight+','+extra.offsetWidth);"
        "const proto=Element.prototype,saved=proto.getBoundingClientRect;"
        "let seen=0;proto.getBoundingClientRect=function(){seen++;"
        "return{height:41.6,width:3.4}};"
        "out.push(cells[1].offsetHeight+','+cells[1].offsetWidth+','+seen);"
        "proto.getBoundingClientRect=saved;"
        "cells[1].getBoundingClientRect=()=>({height:-2.5,width:NaN});"
        "out.push(cells[1].offsetHeight+','+cells[1].offsetWidth+','"
        "+cells[0].offsetHeight);delete cells[1].getBoundingClientRect;"
        "const Rect=DOMRect;let made=0;globalThis.DOMRect=class{"
        "constructor(x,y,w,h){made++;this.width=w}get height(){return 77}};"
        "out.push(cells[1].offsetHeight+','+cells[1].offsetWidth+','+made);"
        "globalThis.DOMRect=Rect;"
        "out.push(cells[1].offsetHeight+','+(cells[1].getBoundingClientRect()"
        " instanceof DOMRect));"
        "cells[1].__tilefinchGeometryValue=()=>({x:1,y:2,authoritative:1,"
        "width:3,height:4.4});{const r=cells[1].getBoundingClientRect();"
        "out.push(r.x+','+r.y+','+r.width+','+r.height+','"
        "+cells[1].offsetHeight+','+cells[0].offsetHeight)}"
        "delete cells[1].__tilefinchGeometryValue;"
        "out.push(cells[1].offsetHeight+','+cells.map(c=>{"
        "const r=c.getBoundingClientRect();return[r.x,r.y,r.width,r.height]"
        ".join(':')}).join(' '));"
        "globalThis.pocSummary=out.join('\\n');})()",
        "20/200 31/200 0/200 0/0\n"
        "true\n"
        "50\n"
        "7,79,50\n"
        "0,0\n"
        "31,200,0\n"
        "-2,NaN,50\n"
        "77,200,2\n"
        "31,true\n"
        "1,2,3,4.4,4,50\n"
        "31,8:22:200:50 8:72:200:31 8:103:200:0 0:0:0:0"));
    CHECK(bridge_page_close(&page));
    return 0;
}

int main(void)
{
    CHECK(test_offset_geometry_semantics() == 0);
    CHECK(test_tree_walker_semantics() == 0);
    CHECK(test_query_all_builds_list_natively() == 0);
    CHECK(test_bounding_rect_builds_no_geometry_object() == 0);
    CHECK(test_query_steps_over_shadow_trees() == 0);
    CHECK(test_new_nodes_skip_full_slot_search() == 0);
    CHECK(test_removed_head_outlives_parse() == 0);
    CHECK(test_query_all_list_semantics() == 0);
    puts("tilefinch-dom-bridge-call-tests: all checks passed");
    return 0;
}
