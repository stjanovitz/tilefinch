/* Platform-API conformance probes: selector parsing, URL, CSSOM rule
   objects, Streams queueing, number formatting, inline style, legacy
   events, Blob URLs, encoders and performance entries. Each probe runs in a
   fresh page realm and reports the names of failed expectations. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/selector_syntax.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

/* The probe preamble: t(name, f) records `name` when f() is false or
   throws; throwsName(f, name) is whether f() throws an error so named. */
#define PROBE_PRELUDE                                                        \
    "const failures=[],t=(name,f)=>{try{if(!f())failures.push(name)}"        \
    "catch(e){failures.push(name+':'+e)}};"                                  \
    "const throwsName=(f,n)=>{try{f()}catch(e){return e.name===n}"           \
    "return false};"
#define PROBE_SUMMARY(ok)                                                    \
    "globalThis.pocSummary=failures.length?'FAIL '+failures.join(' | ')"     \
    ":'" ok "';"

/* Runs `script` as the only classic script of a fresh document whose body
   starts with `body`, pumps the event loop for asynchronous probes, and
   returns whether globalThis.pocSummary equalled `expected`. */
static bool run_probe_body(const char *label, const char *body,
                           const char *script, const char *expected)
{
    static char page[64 * 1024];
    int written = snprintf(page, sizeof(page),
                           "<!doctype html><html><head><title>probe</title>"
                           "</head><body>%s</body></html>", body);
    if (written < 0 || (size_t) written >= sizeof(page)) {
        fprintf(stderr, "%s: probe page too large\n", label);
        return false;
    }
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    if (!document_parse(&document, &budget, page, strlen(page), 17)) {
        fprintf(stderr, "%s: parse failed\n", label);
        return false;
    }
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_deferred(
        &document, &budget, 12u * MIB, 3000,
        "https://conformance.test/platform", NULL, &result);
    bool ran = runtime != NULL && script_runtime_evaluate_diagnostic(
        runtime, script, label, &result);
    for (unsigned turn = 0; ran && turn < 64u
         && strcmp(result.summary, expected) != 0
         && strncmp(result.summary, "FAIL", 4) != 0; turn++)
        ran = script_runtime_advance(runtime, 1u, 8u, &result);
    bool ok = ran && result.success && strcmp(result.summary, expected) == 0;
    if (!ok)
        fprintf(stderr, "%s: ran=%d success=%d summary=%s error=%s\n", label,
                ran, result.success, result.summary, result.error);
    if (runtime != NULL) script_runtime_destroy(runtime);
    document_destroy(&document);
    if (ok && budget.current != 0) {
        fprintf(stderr, "%s: %zu bytes left owned\n", label, budget.current);
        ok = false;
    }
    budget_uninstall_lexbor(&budget);
    return ok;
}

static bool run_probe(const char *label, const char *script,
                      const char *expected)
{
    return run_probe_body(label, "<p id=p>text</p>", script, expected);
}

/* ---- Selector syntax (JR-07) ---- */

static const char *const valid_selectors[] = {
    "*", "div", "DIV", "#a", ".b", "[c]", "[c=d]", "[c='d e']", "[c=\"d\"]",
    "[c~=d]", "[c|=d]", "[c^=d]", "[c$=d]", "[c*=d]", "[ c = d i ]",
    "[c=\"d\"s]", "a b", "a>b", "a > b", "a+b", "a ~ b", "a,b", " a , b ",
    "a.b#c[d]:hover", "-x", "--x", "_x", "\\31 23", "#\\31 a", ".a\\:b",
    "li:nth-child(2)", "li:nth-child(2n+1)", "li:nth-child( -n + 3 )",
    "li:nth-child(2n- 1)", "li:nth-child(odd)", "li:nth-child(EVEN)",
    "li:nth-child(+5)", "li:nth-child(n)", "li:nth-child(2n+1 of .x)",
    "li:nth-of-type(3n)", ":not(.a, .b)", ":not(a b)", ":is()", ":is(a, b c)",
    ":where(.x > .y)", "a:has(> b)", "a:has(+ b, ~ c)", "a:has(b c)",
    ":root", ":scope > li", ":host", ":host(.a)", ":host-context(.a)",
    "::slotted(span)", "::part(a b)", "p::before", "p:before", "p::after",
    "p:first-line", "p::first-letter", "p::before:hover", "::placeholder",
    ":lang(en, \"de-*\")", ":dir(rtl)", ":state(open)", ":heading(1, 2)",
    ":current", ":current(p, li)", "input:-webkit-autofill",
    ":focus-visible", ":popover-open", "\xc3\xa9t\xc3\xa9", ".\xe2\x9c\x93",
    "a:not(:has(b))",
};

static const char *const invalid_selectors[] = {
    "", " ", "li:first", "li:last", "p:left", "p:right", "p:blank",
    "li:eq(0)", "li:visible", "[name!=q]", "li >", "> li", "a > > b", "a,",
    ",a", "a,,b", "li!", "[a=b c]", "[a=1]", "#1", "#", ".1a", ".", "[",
    "[a", "[a=]", "[a='b]", "a[b]c", "li:nth-child(foo)", "li:nth-child()",
    "li:nth-child(2 n)", "li:nth-child(+ 2)", "li:nth-child(2n+)",
    "li:nth-of-type(2 of .x)", ":not()", ":not", ":has", ":has()",
    ":has(1)", "a:has(b:has(c))", ":is(:bogus, li)", ":is(::before)",
    ":not(p::before)", ":hover(x)", ":not(.a", "a)", "a]", "::bogus",
    ":bogus", "p::before span", "p::before.a", "p::before > a", "p:before(x)",
    "::part", "::slotted", "::slotted(a b)", ":host(a b)",
    ":host(:not(a b))", "*|a", "|a", "ns|a", "[*|href]", "[xlink|href]",
    "a || b", "a/**/b", ":lang()", ":dir()", ":dir(a b)", "a\\\n", "a\"",
    "@", "a{}", "a;b",
};

static int test_selector_syntax_table(void)
{
    puts("test: selector syntax table");
    int failed = 0;
    for (size_t i = 0; i < sizeof(valid_selectors) / sizeof(*valid_selectors);
         i++) {
        const char *text = valid_selectors[i];
        if (!selector_syntax_valid(text, strlen(text))) {
            fprintf(stderr, "valid selector refused: \"%s\"\n", text);
            failed++;
        }
    }
    for (size_t i = 0;
         i < sizeof(invalid_selectors) / sizeof(*invalid_selectors); i++) {
        const char *text = invalid_selectors[i];
        if (selector_syntax_valid(text, strlen(text))) {
            fprintf(stderr, "invalid selector accepted: \"%s\"\n", text);
            failed++;
        }
    }
    /* Nesting is capped; a deeper selector is refused, not recursed into. */
    char deep[16 * 6 + 8 + 16];
    size_t used = 0;
    for (unsigned i = 0; i < SELECTOR_SYNTAX_NESTING_LIMIT; i++) {
        memcpy(deep + used, ":not(", 5);
        used += 5;
    }
    deep[used++] = 'a';
    for (unsigned i = 0; i < SELECTOR_SYNTAX_NESTING_LIMIT; i++)
        deep[used++] = ')';
    if (!selector_syntax_valid(deep, used)) {
        fprintf(stderr, "selector at the nesting cap refused\n");
        failed++;
    }
    char deeper[sizeof(deep) + 8];
    memcpy(deeper, ":not(", 5);
    memcpy(deeper + 5, deep, used);
    deeper[used + 5] = ')';
    if (selector_syntax_valid(deeper, used + 6)) {
        fprintf(stderr, "selector past the nesting cap accepted\n");
        failed++;
    }
    /* A NUL byte is U+FFFD, a name character, as in CSS. */
    CHECK(selector_syntax_valid("a\0b", 3));
    CHECK(failed == 0);
    return 0;
}

/* querySelectorAll throws for the selectors jQuery answers itself, so its
   Sizzle fallback runs; valid ones still match. */
static const char selector_probe[] =
    PROBE_PRELUDE
    "const d=document.createElement('div');"
    "d.innerHTML='<ul><li class=x>a</li><li>b</li></ul>"
    "<input type=hidden name=h><input name=q>';document.body.appendChild(d);"
    "for(const s of ['li:first','tr:first td','input[type!=hidden]','li >',"
    "'li!','[a=b c]','li:nth-child(foo)','#1'])"
    "{t('qsa '+s,()=>throwsName(()=>document.querySelectorAll(s),'SyntaxError'));"
    "t('qs '+s,()=>throwsName(()=>d.querySelector(s),'SyntaxError'));"
    "t('matches '+s,()=>throwsName(()=>d.matches(s),'SyntaxError'));"
    "t('closest '+s,()=>throwsName(()=>d.closest(s),'SyntaxError'))}"
    /* The jQuery pattern: qSA first, own engine when it throws. */
    "const select=s=>{try{return['native',document.querySelectorAll(s).length]}"
    "catch(e){return['fallback',e.name]}};"
    "t('jquery first',()=>select('li:first').join()==='fallback,SyntaxError');"
    "t('valid list',()=>document.querySelectorAll('li.x, input[name=q]')"
    ".length===2);"
    "t('nth of',()=>document.querySelectorAll('li:nth-child(1 of .x)')"
    ".length===1);"
    "t('pseudo-element',()=>document.querySelectorAll('li:before').length===0"
    "&&document.querySelectorAll('li::after').length===0);"
    "t('is empty',()=>document.querySelectorAll(':is()').length===0);"
    "t('closest valid',()=>d.querySelector('li').closest('ul > li')!==null);"
    "t('supports',()=>CSS.supports('selector(li:first-child)')"
    "&&!CSS.supports('selector(li:first)')"
    "&&!CSS.supports('selector([a!=b])'));"
    PROBE_SUMMARY("SELECTORS-OK");

static int test_selector_errors(void)
{
    puts("test: invalid selectors throw SyntaxError in the page");
    CHECK(run_probe("selectors", selector_probe, "SELECTORS-OK"));
    return 0;
}

/* ---- URL: non-special schemes, ws(s), input trimming (JR-05) ---- */

static const char url_probe[] =
    PROBE_PRELUDE
    "const u=s=>new URL(s),parts=x=>[x.protocol,x.host,x.pathname,x.search,"
    "x.hash,x.origin].join('|');"
    "t('mailto',()=>parts(u('mailto:a@b.c?subject=a b#f'))"
    "==='mailto:||a@b.c|?subject=a%20b|#f|null');"
    "t('data',()=>u('data:text/plain,hi').pathname==='text/plain,hi');"
    "t('about',()=>u('about:blank').href==='about:blank');"
    "t('javascript',()=>u('javascript:void(0)').protocol==='javascript:');"
    "t('tel spaces kept',()=>u('tel:+1 555').href==='tel:+1 555');"
    "t('hierarchical',()=>parts(u('foo://h:8/a/./b/../c?q#f'))"
    "==='foo:|h:8|/a/c|?q|#f|null');"
    "t('ws',()=>u('ws://a.test:80/x').href==='ws://a.test/x'"
    "&&u('wss://a.test/x').origin==='wss://a.test');"
    "t('ws relative',()=>new URL('/y','wss://a.test/x').href==='wss://a.test/y');"
    "t('protocol to wss',()=>{const x=u('https://a.test/s');x.protocol='wss:';"
    "return x.href==='wss://a.test/s'});"
    "t('special to non-special refused',()=>{const x=u('https://a.test/s');"
    "x.protocol='mailto';return x.href==='https://a.test/s'});"
    "t('trim',()=>u('  https://a.test/a  ').href==='https://a.test/a'"
    "&&u('\\thttps://a.\\ntest/').href==='https://a.test/'"
    "&&new URL(' /x','https://a.test/b').href==='https://a.test/x');"
    "t('x: base',()=>new URL('a/../b?q','x:/').href==='x:/b?q');"
    "t('fragment on opaque base',()=>new URL('#x','mailto:a@b').href"
    "==='mailto:a@b#x');"
    "t('relative on opaque base',()=>throwsName(()=>new URL('x','mailto:a'),"
    "'TypeError'));"
    "t('relative on hierarchical base',()=>new URL('b','foo://h/a/c').href"
    "==='foo://h/a/b');"
    "t('bad port',()=>throwsName(()=>u('foo://h:99999/'),'TypeError'));"
    "t('opaque setters',()=>{const x=u('mailto:a');x.host='h';x.pathname='p';"
    "x.search='s=1';x.hash='h';return x.href==='mailto:a?s=1#h'});"
    "t('searchParams',()=>{const x=u('foo:a?x=1');x.searchParams.append('y','2');"
    "return x.href==='foo:a?x=1&y=2'});"
    "t('ipv6',()=>u('http://[::1]:8080/x').host==='[::1]:8080');"
    "t('backslash',()=>u('https:\\\\\\\\a.test\\\\x').href==='https://a.test/x');"
    "t('canParse',()=>URL.canParse('mailto:x@y')&&URL.canParse('tel:1')"
    "&&!URL.canParse('/x'));"
    "t('anchor',()=>{const a=document.createElement('a');a.href='mailto:x@y.z';"
    "return a.protocol==='mailto:'&&a.pathname==='x@y.z'});"
    "t('all links',()=>{document.body.insertAdjacentHTML('beforeend',"
    "'<a href=\"mailto:m@x.test\">m</a><a href=\"tel:1\">t</a><a href=\"/p\">p</a>');"
    "return [...document.links].map(a=>new URL(a.href).protocol).join()"
    "==='mailto:,tel:,https:'});"
    PROBE_SUMMARY("URL-OK");

static int test_url_schemes(void)
{
    puts("test: URL parses non-special schemes, ws(s) and trims input");
    CHECK(run_probe("url", url_probe, "URL-OK"));
    return 0;
}

/* ---- CSSOM rule objects (JR-08) ---- */

static const char cssom_probe[] =
    PROBE_PRELUDE
    "const st=document.createElement('style');st.textContent="
    "':root{--brand:#123456}.a{color:red}@media (min-width:1px){.b{margin:1px}}"
    "@keyframes k{from{opacity:0}to{opacity:1}}"
    "@font-face{font-family:F;src:url(f.woff)}@supports (display:grid){.c{color:red}}';"
    "document.head.appendChild(st);const sh=st.sheet,R=sh.cssRules;"
    "t('rule count',()=>R.length===6&&R.item(1)===R[1]);"
    "t('root var',()=>R[0].style.getPropertyValue('--brand').trim()==='#123456');"
    "t('constants',()=>CSSRule.STYLE_RULE===1&&CSSRule.MEDIA_RULE===4"
    "&&R[0].KEYFRAMES_RULE===7);"
    "t('hierarchy',()=>R[1] instanceof CSSStyleRule&&R[1] instanceof CSSRule"
    "&&R[2] instanceof CSSMediaRule&&R[2] instanceof CSSGroupingRule"
    "&&R[3] instanceof CSSKeyframesRule&&R[4] instanceof CSSFontFaceRule"
    "&&R[5] instanceof CSSSupportsRule);"
    "t('types',()=>[...R].map(r=>r.type).join()==='1,1,4,7,5,12');"
    "t('selectorText',()=>R[1].selectorText==='.a');"
    "t('media',()=>R[2].conditionText==='(min-width:1px)'"
    "&&R[2].media.mediaText==='(min-width:1px)'&&R[2].cssRules.length===1"
    "&&R[2].cssRules[0].selectorText==='.b'&&R[2].cssRules[0].parentRule===R[2]);"
    "t('keyframes',()=>R[3].name==='k'&&R[3].cssRules.length===2"
    "&&R[3].findRule('to').style.opacity==='1');"
    "t('font-face',()=>R[4].style.getPropertyValue('font-family')==='F');"
    "t('style write',()=>{R[1].style.color='blue';"
    "return /color: blue/.test(R[1].cssText)&&sh.cssRules[1]===R[1]});"
    "t('nested insert',()=>{R[2].insertRule('.d{top:0}',1);"
    "return R[2].cssRules.length===2&&/\\.d/.test(R[2].cssText)"
    "&&/\\.d/.test(sh.cssRules[2].cssText)});"
    "t('illegal constructor',()=>throwsName(()=>new CSSStyleRule(),'TypeError'));"
    "t('addRule',()=>sh.addRule('.e','top:1px')===-1&&sh.cssRules[sh.cssRules"
    ".length-1].selectorText==='.e');"
    PROBE_SUMMARY("CSSOM-OK");

static int test_cssom_rules(void)
{
    puts("test: CSSOM rules expose style, nested cssRules and CSSRule types");
    CHECK(run_probe("cssom", cssom_probe, "CSSOM-OK"));
    return 0;
}

/* ---- Writes through rule objects keep the sheet's text bound and roll
   back when refused ---- */

/* The 256 KiB bound of the sheet's own insertRule applies to writes through
   nested rules too, adopted or not, and a refused write leaves the rule
   objects matching the applied text, so later writes still work. */
static const char cssom_rule_quota_probe[] =
    PROBE_PRELUDE
    "const big=(n,k)=>'.'+n+'{background:url('+'a'.repeat(k*1024)+')}',"
    "count=s=>(s.match(/\\.x\\d+\\{/g)||[]).length,"
    "quota=f=>throwsName(f,'QuotaExceededError');"
    "const u=new CSSStyleSheet();u.replaceSync('@media screen{.a{}}');"
    "const gu=u.cssRules[0];"
    "t('unadopted group insert',()=>{gu.insertRule(big('x1',100),1);"
    "gu.insertRule(big('x2',100),2);return quota(()=>gu.insertRule(big('x3',100),3))"
    "&&gu.cssRules.length===3&&count(gu.cssText)===2"
    "&&u.cssRules[0].cssText.length<=262144&&u.__rules[0]===gu.cssText});"
    "const b=new CSSStyleSheet();b.replaceSync('@media screen{.a{color:red}}');"
    "document.adoptedStyleSheets=[b];const g=b.cssRules[0];let refused=false;"
    "for(let i=1;i<9&&!refused;i++)refused=quota(()=>g.insertRule(big('x'+i,200),"
    "g.cssRules.length));"
    "t('adopted group insert',()=>refused&&g.cssRules.length===2"
    "&&count(g.cssText)===1&&count(b.__rules[0])===1&&b.__rules[0]===g.cssText);"
    "t('writes after a refusal',()=>{g.cssRules[0].selectorText='.b';"
    "g.deleteRule(1);return g.cssRules.length===1&&/^\\.b \\{/.test(g.cssRules[0].cssText)"
    "&&b.__rules[0]===g.cssText&&count(b.__rules[0])===0&&/\\.b \\{/.test(b.__rules[0])});"
    "const c=new CSSStyleSheet();c.replaceSync('@media screen{.a{color:red}}');"
    "document.adoptedStyleSheets=[c];const h=c.cssRules[0],r=h.cssRules[0];"
    "t('near-full group',()=>h.insertRule(big('x1',100),1)===1"
    "&&h.insertRule(big('x2',100),2)===2);"
    "t('refused style write',()=>quota(()=>r.style.setProperty('background',"
    "'url('+'q'.repeat(64*1024)+')'))&&r.style.getPropertyValue('background')===''"
    "&&r.style.getPropertyValue('color')==='red'&&!c.__rules[0].includes('qqq')"
    "&&h.cssText===c.__rules[0]);"
    "t('style write after a refusal',()=>{r.style.setProperty('top','1px');"
    "return /top: 1px/.test(c.__rules[0])&&r.style.getPropertyValue('top')==='1px'});"
    "t('refused selectorText',()=>quota(()=>{r.selectorText='.'+'s'.repeat(64*1024)})"
    "&&r.selectorText==='.a'&&/^@media screen \\{\\n  \\.a \\{/.test(c.__rules[0]));"
    "t('shrinking write',()=>{h.deleteRule(2);h.deleteRule(1);"
    "return count(c.__rules[0])===0&&h.cssRules.length===1});"
    PROBE_SUMMARY("CSSOM-QUOTA-OK");

static int test_cssom_rule_quota(void)
{
    puts("test: writes through CSSOM rules keep the sheet bound and roll back");
    CHECK(run_probe("cssom-quota", cssom_rule_quota_probe, "CSSOM-QUOTA-OK"));
    return 0;
}

/* ---- Streams queue by size and memory, not chunk count (JR-09) ---- */

static const char streams_probe[] =
    PROBE_PRELUDE
    "(async()=>{try{const read=async r=>{const out=[];for(;;){const x=await r.read();"
    "if(x.done)return out;out.push(x.value)}};"
    "const text=Array.from({length:100},(_,i)=>'{\"i\":'+i+'}').join('\\n');"
    "const src=new ReadableStream({start(c){c.enqueue(text);c.close()}}),"
    "lines=new TransformStream({transform(chunk,c){"
    "for(const l of chunk.split('\\n'))c.enqueue(l)}});"
    "const got=await read(src.pipeThrough(lines).getReader());"
    "t('line splitter',()=>got.length===100&&got[99]==='{\"i\":99}');"
    "let size=null;const many=new ReadableStream({start(c){"
    "for(let i=0;i<1000;i++)c.enqueue(i);size=c.desiredSize;c.close()}});"
    "const all=await read(many.getReader());"
    "t('1000 queued',()=>all.length===1000&&all[999]===999);"
    "t('desiredSize negative',()=>size<0);"
    "let refused=null,queued=0;const big=new ReadableStream({start(c){"
    "try{for(;queued<4096;queued++)c.enqueue(new Uint8Array(65536))}"
    "catch(e){refused=e}}});"
    "t('memory bound',()=>refused instanceof RangeError"
    "&&/memory/.test(refused.message)&&queued>64);"
    "let bigError=null;await big.getReader().read().catch(e=>{bigError=e});"
    "t('refused stream errored',()=>bigError===refused);"
    "const sink=[],ws=new WritableStream({write(v){sink.push(v)}}),"
    "w=ws.getWriter(),writes=[];for(let i=0;i<100;i++)writes.push(w.write(i));"
    "await Promise.all(writes);t('100 writes',()=>sink.length===100);"
    "}catch(e){failures.push('aborted:'+e)}"
    PROBE_SUMMARY("STREAMS-OK") "})();";

static int test_streams_queue(void)
{
    puts("test: Streams queue past 64 chunks, bounded by memory");
    CHECK(run_probe("streams", streams_probe, "STREAMS-OK"));
    return 0;
}

/* ---- en-US number and locale formatting (JR-10) ---- */

static const char intl_probe[] =
    PROBE_PRELUDE
    "const nf=(o,v)=>new Intl.NumberFormat('en-US',o).format(v),"
    "usd={style:'currency',currency:'USD'};"
    "t('usd',()=>nf(usd,1234.5)==='$1,234.50'&&nf(usd,-5)==='-$5.00');"
    "t('eur jpy',()=>nf({style:'currency',currency:'EUR'},3)==='€3.00'"
    "&&nf({style:'currency',currency:'JPY'},500.6)==='¥501');"
    "t('code',()=>nf({style:'currency',currency:'CHF'},12)==='CHF\\u00a012.00'"
    "&&nf({style:'currency',currency:'USD',currencyDisplay:'code'},1)"
    "==='USD\\u00a01.00');"
    "t('accounting',()=>nf({...usd,currencySign:'accounting'},-12)==='($12.00)');"
    "t('decimal',()=>nf({},1234567.891)==='1,234,567.891'"
    "&&nf({},1.0005)==='1.001'&&nf({minimumFractionDigits:2},5)==='5.00');"
    "t('percent',()=>nf({style:'percent'},0.256)==='26%');"
    "t('compact',()=>nf({notation:'compact'},1234)==='1.2K'"
    "&&nf({notation:'compact'},999999)==='1M'"
    "&&nf({notation:'compact',compactDisplay:'long'},2e6)==='2 million');"
    "t('significant',()=>nf({maximumSignificantDigits:3},123456)==='123,000');"
    "t('parts',()=>new Intl.NumberFormat('en-US',usd).formatToParts(1.5)"
    ".map(p=>p.type+':'+p.value).join()"
    "==='currency:$,integer:1,decimal:.,fraction:50');"
    "t('currency required',()=>throwsName(()=>new Intl.NumberFormat('en-US',"
    "{style:'currency'}),'TypeError'));"
    "t('number toLocaleString',()=>(1234567).toLocaleString()==='1,234,567'"
    "&&(12.5).toLocaleString('en-US',usd)==='$12.50'"
    "&&(5n).toLocaleString()==='5');"
    "t('date options',()=>new Date(0).toLocaleString('en-US',{timeZone:'UTC'})"
    "==='1/1/1970, 12:00:00 AM'"
    "&&new Date(0).toLocaleTimeString('en-US',{timeZone:'UTC'})==='12:00:00 AM'"
    "&&new Date(0).toLocaleDateString('en-US',{timeZone:'UTC',month:'long',"
    "day:'numeric',year:'numeric'})==='January 1, 1970');"
    "t('date brand',()=>throwsName(()=>Date.prototype.toLocaleString.call({},"
    "'en-US'),'TypeError'));"
    "t('page Intl ignored',()=>{Intl.NumberFormat=function(){throw 1};"
    "return (1000).toLocaleString()==='1,000'});"
    PROBE_SUMMARY("INTL-OK");

static int test_intl_formatting(void)
{
    puts("test: en-US currency, grouping and toLocaleString formatting");
    CHECK(run_probe("intl", intl_probe, "INTL-OK"));
    return 0;
}

/* ---- style.x = null removes the declaration (JR-13) ---- */

static const char style_null_probe[] =
    PROBE_PRELUDE
    "const e=document.getElementById('p');"
    "e.style.display='none';e.style.width='5px';"
    "e.style.display=null;e.style.width=null;"
    "t('null property',()=>e.style.display===''&&e.style.width===''"
    "&&!/display|width|null/.test(e.getAttribute('style')||''));"
    "e.style.setProperty('color','red');e.style.setProperty('color',null);"
    "t('setProperty null',()=>e.style.color==='');"
    "e.style.color='red';e.style.cssText=null;"
    "t('cssText null',()=>e.style.cssText===''&&e.style.color==='');"
    "const d=document.createElement('div');d.style.width='1px';"
    "d.style.width=null;t('detached null',()=>d.style.width===''"
    "&&!/null/.test(d.getAttribute('style')||''));"
    "const st=document.createElement('style');st.textContent='.q{color:red}';"
    "document.head.appendChild(st);const rs=st.sheet.cssRules[0].style;"
    "rs.color=null;t('rule null',()=>rs.color===''"
    "&&!/null/.test(st.sheet.cssRules[0].cssText));"
    "t('string null kept',()=>{e.style.setProperty('--x','null');"
    "return e.style.getPropertyValue('--x').trim()==='null'});"
    PROBE_SUMMARY("STYLE-NULL-OK");

static int test_style_null(void)
{
    puts("test: style.x = null removes the declaration");
    CHECK(run_probe("style-null", style_null_probe, "STYLE-NULL-OK"));
    return 0;
}

/* ---- location.origin and internal URL reads cannot be forged (JR-25) ---- */

static const char origin_probe[] =
    PROBE_PRELUDE
    "const real=location.origin,d=Object.getOwnPropertyDescriptor(URL.prototype,"
    "'origin');Object.defineProperty(URL.prototype,'origin',{configurable:true,"
    "get(){return 'https://bank.example'}});"
    "t('location.origin',()=>location.origin===real&&window.origin===real);"
    "t('blob prefix',()=>URL.createObjectURL(new Blob(['x']))"
    ".startsWith('blob:'+real+'/'));"
    "t('referrer',()=>new Request('/x',{referrer:'https://evil.test/a'})"
    ".referrer!=='https://evil.test/a');"
    "Object.defineProperty(URL.prototype,'origin',d);"
    "Object.defineProperty(Object.prototype,'_origin',{configurable:true,"
    "get(){return 'https://bank.example'},set(v){}});"
    "t('prototype setter',()=>new URL('https://a.test/x').origin"
    "==='https://a.test');"
    "t('history',()=>{history.replaceState(null,'','/moved');"
    "return location.origin===real&&location.pathname==='/moved'});"
    "delete Object.prototype._origin;"
    "t('parser steps locked',()=>{try{URL.prototype._setSpecial=function(){"
    "this._origin='https://bank.example'}}catch(_){}"
    "return new URL('https://b.test/').origin==='https://b.test'"
    "&&!delete URL.prototype._set});"
    PROBE_SUMMARY("ORIGIN-OK");

static int test_origin_unforgeable(void)
{
    puts("test: location.origin and internal URL reads resist author getters");
    CHECK(run_probe("origin", origin_probe, "ORIGIN-OK"));
    return 0;
}

/* ---- URL searchParams edits rewrite only the query (JR-26) ---- */

static const char search_params_probe[] =
    PROBE_PRELUDE
    "const u=new URL('https://a.test/p?a=1#h'),q=u.searchParams;"
    "q.append('b','x y');q.append('c','é&=!');"
    "t('append',()=>u.href==='https://a.test/p?a=1&b=x+y&c=%C3%A9%26%3D%21#h'"
    "&&u.search==='?a=1&b=x+y&c=%C3%A9%26%3D%21');"
    "q.delete('a');t('delete',()=>u.search==='?b=x+y&c=%C3%A9%26%3D%21');"
    "q.set('b','2');q.append('a','0');q.sort();"
    "t('set sort',()=>u.search==='?a=0&b=2&c=%C3%A9%26%3D%21'&&u.hash==='#h');"
    "u.search='?z=9';q.append('y','8');"
    "t('search then append',()=>u.href==='https://a.test/p?z=9&y=8#h'"
    "&&q.get('z')==='9'&&q.size===2);"
    "u.hash='#k';q.append('x','7');"
    "t('hash kept',()=>u.href==='https://a.test/p?z=9&y=8&x=7#k');"
    "for(const k of [...q.keys()])q.delete(k);"
    "t('emptied',()=>u.href==='https://a.test/p#k'&&u.search==='');"
    "q.append('k','v');t('after empty',()=>u.search==='?k=v'&&q.toString()==='k=v');"
    "const big=new URL('https://a.test/');for(let i=0;i<150;i++)"
    "big.searchParams.append('k'+i,String(i));"
    "t('150 appends',()=>big.search.length>1000&&big.searchParams.get('k149')"
    "==='149'&&new URL(big.href).searchParams.size===150);"
    "const o=new URL('foo:a?x=1');o.searchParams.append('y','2');"
    "t('non-special',()=>o.href==='foo:a?x=1&y=2');"
    PROBE_SUMMARY("SEARCH-PARAMS-OK");

static int test_search_params_edits(void)
{
    puts("test: URL searchParams edits keep href and search in step");
    CHECK(run_probe("search-params", search_params_probe, "SEARCH-PARAMS-OK"));
    return 0;
}

/* ---- TextEncoder.encodeInto against a per-code-point reference (JR-18) ---- */

static const char encode_into_probe[] =
    PROBE_PRELUDE
    "const ref=(text,room)=>{const out=[];let read=0;"
    "while(read<text.length){let cp=text.codePointAt(read),units=cp>65535?2:1;"
    "if(cp>=55296&&cp<=57343)cp=65533;"
    "const b=cp<128?[cp]:cp<2048?[192|cp>>6,128|cp&63]:cp<65536"
    "?[224|cp>>12,128|cp>>6&63,128|cp&63]"
    ":[240|cp>>18,128|cp>>12&63,128|cp>>6&63,128|cp&63];"
    "if(out.length+b.length>room)break;out.push(...b);read+=units;}"
    "return {read,written:out.length,bytes:out.join()};};"
    "const e=new TextEncoder(),texts=['','a','abc','é','€x','😀','a😀b',"
    "'\\ud83d','x\\ud83d','\\ude00y','\\ud83d\\ud83d\\ude00','é€😀a\\udc00',"
    "'héllo wörld € 😀 '.repeat(5)];let bad='';"
    "for(const text of texts)for(let room=0;room<=text.length*3+2;room++){"
    "const d=new Uint8Array(room+2).fill(7),view=d.subarray(1,room+1),"
    "r=e.encodeInto(text,view),w=ref(text,room);"
    "if(r.read!==w.read||r.written!==w.written||"
    "[...view.subarray(0,r.written)].join()!==w.bytes||d[0]!==7||d[room+1]!==7)"
    "{bad=JSON.stringify(text)+'@'+room+':'+r.read+','+r.written;break;}}"
    "t('matches reference',()=>bad==='');"
    "t('fits at once',()=>{const d=new Uint8Array(64),r=e.encodeInto('a😀',d);"
    "return r.read===3&&r.written===5&&d[1]===240&&d[5]===0;});"
    "t('result shape',()=>{const r=e.encodeInto('ab',new Uint8Array(1));"
    "return Object.keys(r).join()==='read,written'&&r.read===1;});"
    "t('needs Uint8Array',()=>{try{e.encodeInto('a',new Uint16Array(4));"
    "return false;}catch(x){return x instanceof TypeError;}});"
    "t('over a mebibyte cut',()=>{const text='€'.repeat(400000),"
    "d=new Uint8Array(1100000),r=e.encodeInto(text,d);"
    "return r.written===1099998&&r.read===366666&&d[1099997]===172;});"
    PROBE_SUMMARY("ENCODE-INTO-OK");

static int test_encode_into(void)
{
    puts("test: TextEncoder.encodeInto writes whole sequences that fit");
    CHECK(run_probe("encode-into", encode_into_probe, "ENCODE-INTO-OK"));
    return 0;
}

/* ---- FileReader.readAsDataURL encodes natively (JR-19) ---- */

static const char data_url_probe[] =
    PROBE_PRELUDE
    "(async()=>{try{const abc='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789+/',ref=b=>{let s='';for(let i=0;i<b.length;i+=3){"
    "const n=b[i]<<16|(b[i+1]||0)<<8|(b[i+2]||0);s+=abc[n>>18]+abc[n>>12&63]"
    "+(i+1<b.length?abc[n>>6&63]:'=')+(i+2<b.length?abc[n&63]:'=');}return s;},"
    "read=blob=>new Promise((ok,no)=>{const r=new FileReader();"
    "r.onload=()=>ok(r.result);r.onerror=()=>no(r.error);r.readAsDataURL(blob);});"
    "const big=new Uint8Array(262147);for(let i=0;i<big.length;i++)"
    "big[i]=(i*7919+(i>>8))&255;"
    "for(const [name,bytes,type] of [['empty',new Uint8Array(0),''],"
    "['one',new Uint8Array([255]),'image/png'],['two',new Uint8Array([0,128]),''],"
    "['three',new Uint8Array([1,2,3]),'text/plain'],['big',big,'']]){"
    "const got=await read(new Blob([bytes],{type}));"
    "t(name,()=>got==='data:'+(type||'application/octet-stream')+';base64,'+ref(bytes));}"
    "globalThis.btoa=()=>'page';"
    "const kept=await read(new Blob(['hi'],{type:'text/plain'}));"
    "t('page btoa unused',()=>kept==='data:text/plain;base64,aGk=');"
    "}catch(e){failures.push('aborted:'+e)}"
    PROBE_SUMMARY("DATA-URL-OK") "})();";

static int test_data_url_reader(void)
{
    puts("test: FileReader.readAsDataURL matches a reference base64");
    CHECK(run_probe("data-url", data_url_probe, "DATA-URL-OK"));
    return 0;
}

/* ---- Legacy event init methods, null init dictionaries (JR-16) ---- */

static const char legacy_events_probe[] =
    PROBE_PRELUDE
    "(async()=>{try{"
    "t('null init',()=>[Event,CustomEvent,UIEvent,MouseEvent,PointerEvent,"
    "KeyboardEvent,InputEvent,FocusEvent,CompositionEvent,SubmitEvent,"
    "TransitionEvent,ToggleEvent,ProgressEvent,ErrorEvent,MessageEvent,"
    "CloseEvent].every(E=>{const e=new E('x',null);return e.type==='x'"
    "&&e.bubbles===false;}));"
    "t('init lengths',()=>Event.length===1&&MouseEvent.length===1"
    "&&MouseEvent.prototype.initMouseEvent.length===1);"
    "const m=document.createEvent('MouseEvents');"
    "m.initMouseEvent('click',true,true,window,2,10,20,30,40,true,false,true,false,"
    "1,document.body);"
    "t('initMouseEvent',()=>m.type==='click'&&m.bubbles&&m.cancelable&&m.view===window"
    "&&m.detail===2&&m.screenX===10&&m.screenY===20&&m.clientX===30&&m.clientY===40"
    "&&m.x===30&&m.ctrlKey&&!m.altKey&&m.shiftKey&&!m.metaKey&&m.button===1"
    "&&m.relatedTarget===document.body&&m.getModifierState('Shift'));"
    "let got=null;document.body.addEventListener('click',e=>{got=e},{once:true});"
    "document.body.dispatchEvent(m);t('dispatches',()=>got===m&&m.target===document.body);"
    "const u=document.createEvent('UIEvents');u.initUIEvent('resize',false,true,null,5);"
    "t('initUIEvent',()=>u.type==='resize'&&!u.bubbles&&u.cancelable&&u.view===null"
    "&&u.detail===5);"
    "const k=document.createEvent('KeyboardEvent');"
    "k.initKeyboardEvent('keydown',true,false,window,'Enter',3,false,true);"
    "t('initKeyboardEvent',()=>k.type==='keydown'&&k.bubbles&&k.key==='Enter'"
    "&&k.location===3&&k.altKey&&!k.ctrlKey&&k.getModifierState('Alt'));"
    "t('init needs type',()=>throwsName(()=>m.initMouseEvent(),'TypeError'));"
    "const r=new FileReader();"
    "const bin=await new Promise((ok,no)=>{r.onload=()=>ok(r.result);"
    "r.onerror=()=>no(r.error);r.readAsBinaryString(new Blob(["
    "new Uint8Array([0,65,128,255]),new Uint8Array(20000).fill(200)]));});"
    "t('readAsBinaryString',()=>bin.length===20004&&bin.charCodeAt(0)===0"
    "&&bin.slice(1,2)==='A'&&bin.charCodeAt(2)===128&&bin.charCodeAt(3)===255"
    "&&bin.charCodeAt(20003)===200);"
    "}catch(e){failures.push('aborted:'+e)}"
    PROBE_SUMMARY("LEGACY-EVENTS-OK") "})();";

static int test_legacy_events(void)
{
    puts("test: legacy init*Event methods and null event init dictionaries");
    CHECK(run_probe("legacy-events", legacy_events_probe, "LEGACY-EVENTS-OK"));
    return 0;
}

/* ---- Performance entries buffered per type (JR-23) ---- */

static const char performance_buffers_probe[] =
    PROBE_PRELUDE
    "(async()=>{try{const p=performance;"
    "p.mark('start-a');for(let i=0;i<130;i++)p.mark('m'+i);"
    "t('measure after 130 marks',()=>p.measure('x','start-a').duration>=0);"
    "p.mark('start-b');"
    "for(let i=0;i<200;i++)__tilefinchRecordResourceTiming('https://a.test/r'+i,'img');"
    "t('resources keep marks',()=>p.measure('y','start-b').duration>=0"
    "&&p.getEntriesByName('start-a','mark').length===1);"
    "const res=p.getEntriesByType('resource');"
    "t('resource bound',()=>res.length===128&&res[127].name==='https://a.test/r199'"
    "&&res[0].name==='https://a.test/r72');"
    "for(let i=0;i<300;i++)p.mark('n'+i);"
    "const marks=p.getEntriesByType('mark');"
    "t('mark bound',()=>marks.length===256&&marks[255].name==='n299'"
    "&&!throwsName(()=>p.measure('z','n44'),'SyntaxError')"
    "&&throwsName(()=>p.measure('z','n43'),'SyntaxError'));"
    "for(let i=0;i<200;i++)p.measure('q'+i);"
    "t('measure bound',()=>p.getEntriesByType('measure').length===64);"
    "const all=p.getEntries();"
    "t('navigation kept',()=>all[0].entryType==='navigation'"
    "&&p.getEntriesByType('navigation').length===1"
    "&&p.getEntriesByType('visibility-state').length===1);"
    "t('sorted',()=>all.every((e,i)=>!i||all[i-1].startTime<=e.startTime)"
    "&&all.length===1+1+256+64+128);"
    "t('by name',()=>p.getEntriesByName('n299').length===1"
    "&&p.getEntriesByName('n299','measure').length===0);"
    "p.clearMarks('n299');t('clear one mark',()=>p.getEntriesByType('mark').length===255"
    "&&p.getEntriesByName('n298').length===1);"
    "p.clearMarks();p.clearMeasures('q199');"
    "t('clear',()=>p.getEntriesByType('mark').length===0"
    "&&p.getEntriesByType('measure').length===63);"
    "p.clearResourceTimings();t('clear resources',()=>p.getEntriesByType('resource')"
    ".length===0&&p.getEntriesByType('measure').length===63);"
    "p.mark('late');const seen=await new Promise(ok=>{new PerformanceObserver("
    "(list,o)=>{o.disconnect();ok(list.getEntries().map(e=>e.name))})"
    ".observe({type:'mark',buffered:true});});"
    "t('buffered observer',()=>seen.join()==='late');"
    "}catch(e){failures.push('aborted:'+e)}"
    PROBE_SUMMARY("PERF-BUFFERS-OK") "})();";

static int test_performance_buffers(void)
{
    puts("test: performance entries are bounded per entry type");
    CHECK(run_probe("perf-buffers", performance_buffers_probe, "PERF-BUFFERS-OK"));
    return 0;
}

/* ---- adoptedStyleSheets is one live list; insertRule wants one rule (JR-28) ---- */

static const char adopted_sheets_probe[] =
    PROBE_PRELUDE
    "const a=new CSSStyleSheet(),b=new CSSStyleSheet(),c=new CSSStyleSheet();"
    "a.replaceSync('.a{color:red}');b.replaceSync('.b{color:blue}');"
    "const list=document.adoptedStyleSheets;"
    "t('same array',()=>list===document.adoptedStyleSheets&&Array.isArray(list));"
    "list.push(a);t('push',()=>document.adoptedStyleSheets.length===1"
    "&&document.adoptedStyleSheets[0]===a);"
    "list.push(b,c);list.splice(1,1);"
    "t('splice',()=>list.length===2&&list[0]===a&&list[1]===c);"
    "list.unshift(b);list.shift();t('shift',()=>list.length===2"
    "&&list[0]===a&&list[1]===c);"
    "list[1]=b;t('index write',()=>list[1]===b);"
    "list.push(a);t('duplicate',()=>list.length===3&&list[2]===a);"
    "list.pop();list.length=1;t('length',()=>list.length===1&&list[0]===a);"
    "t('index past end',()=>Reflect.set(list,'5',b)===false&&list.length===1"
    "&&Reflect.set(list,'length',4)===false);"
    "t('not constructed',()=>throwsName(()=>list.push(document.createElement('style')"
    ".sheet),'NotAllowedError')&&list.length===1);"
    "t('limit',()=>throwsName(()=>{for(let i=0;i<20;i++)list.push(new CSSStyleSheet())},"
    "'QuotaExceededError')&&list.length===16);"
    "document.adoptedStyleSheets=[b];"
    "t('setter keeps array',()=>list.length===1&&list[0]===b"
    "&&document.adoptedStyleSheets===list);"
    "list.custom=1;t('other properties',()=>list.custom===1);"
    "const host=document.createElement('div');document.body.appendChild(host);"
    "const root=host.attachShadow({mode:'open'});root.adoptedStyleSheets.push(a);"
    "t('shadow root list',()=>root.adoptedStyleSheets[0]===a&&list[0]===b);"
    "t('insertRule }{',()=>throwsName(()=>a.insertRule('}{'),'SyntaxError')"
    "&&a.cssRules.length===1);"
    "t('insertRule ok',()=>a.insertRule('.z{top:0}',1)===1&&a.cssRules.length===2);"
    PROBE_SUMMARY("ADOPTED-OK");

static int test_adopted_sheets(void)
{
    puts("test: adoptedStyleSheets is one live list; insertRule needs one rule");
    CHECK(run_probe("adopted", adopted_sheets_probe, "ADOPTED-OK"));
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int failures = 0;
    failures += test_selector_syntax_table();
    failures += test_selector_errors();
    failures += test_url_schemes();
    failures += test_cssom_rules();
    failures += test_cssom_rule_quota();
    failures += test_streams_queue();
    failures += test_intl_formatting();
    failures += test_style_null();
    failures += test_origin_unforgeable();
    failures += test_search_params_edits();
    failures += test_encode_into();
    failures += test_data_url_reader();
    failures += test_legacy_events();
    failures += test_performance_buffers();
    failures += test_adopted_sheets();
    if (failures != 0) return 1;
    puts("web platform conformance tests passed");
    return 0;
}
