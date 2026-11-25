/* Platform-API conformance probes: selector parsing, URL, CSSOM rule
   objects, Streams queueing, number formatting, inline style, legacy
   events, Blob URLs, encoders and performance entries. Each probe runs in a
   fresh page realm and reports the names of failed expectations. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/selector_syntax.h"
#include "tilefinch/viewport.h"

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
   returns whether globalThis.pocSummary equalled `expected`. With
   `no_snapshot` the realm has no interface snapshot (boot.cfg
   interface_snapshot=0), so a page's patch of a platform member is what
   the browser's own code would call too. */
static bool run_probe_realm(const char *label, const char *body,
                            const char *script, const char *expected,
                            bool no_snapshot)
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
    ScriptRuntimeOptions options = {
        .defer_document_scripts = true,
        .interface_snapshot_disabled = no_snapshot,
    };
    ScriptRuntime *runtime = NULL;
    if (viewport_context_init(&options.viewport, 480, 272, 480, 272))
        runtime = script_runtime_create_configured(
            &document, &budget, 12u * MIB, 3000,
            "https://conformance.test/platform", &options, &result);
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

static bool run_probe_body(const char *label, const char *body,
                           const char *script, const char *expected)
{
    return run_probe_realm(label, body, script, expected, false);
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
    /* The parser's steps are not on the prototype at all: a page's
       same-named method is never called. */
    "t('parser steps locked',()=>{const own=Object.getOwnPropertyNames("
    "URL.prototype).filter((k)=>k[0]==='_').length;"
    "URL.prototype._setSpecial=URL.prototype._set=function(){"
    "this._origin='https://bank.example'};"
    "const ok=own===0&&new URL('https://b.test/').origin==='https://b.test';"
    "delete URL.prototype._setSpecial;delete URL.prototype._set;return ok});"
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

static const char fetch_value_probe[] =
    PROBE_PRELUDE
    "t('null Request init',()=>new Request('/').method==='GET'"
    "&&new Request('/',null).method==='GET');"
    "t('null Response init',()=>new Response(null,null).status===200"
    "&&Response.json(4,null).status===200);"
    "t('valid status text',()=>new Response(null,{statusText:'All\\tOK'}).statusText"
    "==='All\\tOK');"
    "t('invalid status text',()=>['a\\nb','a\\rb','a\\0b','a\\x7fb','a\\u0100']"
    ".every(statusText=>throwsName(()=>new Response(null,{statusText}),'TypeError')));"
    "t('non-JSON values',()=>[undefined,Symbol('s'),()=>{}]"
    ".every(x=>throwsName(()=>Response.json(x),'TypeError')));"
    "const h=new Headers({a:'1',c:'3'}), iter=h.entries();"
    "const first=iter.next().value;h.set('b','2');h.set('c','new');"
    "t('live Headers iterator',()=>first.join()==='a,1'"
    "&&iter.next().value.join()==='b,2'&&iter.next().value.join()==='c,new'"
    "&&iter.next().done);"
    "h.set('d','4');t('exhausted remains done',()=>iter.next().done);"
    "const visits=[];h.forEach((v,k)=>{visits.push(k+v);"
    "if(k==='a')h.set('ab','inserted');});"
    "t('live Headers forEach',()=>visits.join()==='a1,abinserted,b2,cnew,d4');"
    "const c=new Headers(), firstCookie='a=1; Expires=Wed, 09 Jun 2027 10:18:14 GMT';"
    "c.append('Set-Cookie',firstCookie);c.append('set-cookie','b=2');"
    "t('separate cookie entries',()=>typeof c.getSetCookie==='function'"
    "&&c.getSetCookie().join('|')===firstCookie+'|b=2'"
    "&&JSON.stringify(Array.from(c))===JSON.stringify([['set-cookie',firstCookie],"
    "['set-cookie','b=2']])&&c.get('set-cookie')===firstCookie+', b=2');"
    "t('defensive cookie copies',()=>{const copy=new Headers(c),values=c.getSetCookie();"
    "values[0]='not stored';c.set('set-cookie','c=3');"
    "return c.getSetCookie().join()==='c=3'&&copy.getSetCookie().join('|')===firstCookie+'|b=2'"
    "&&(copy.delete('Set-Cookie'),copy.getSetCookie().length===0);});"
    "t('no-cors guard still excludes cookie entries',()=>{"
    "const guarded=new Request('/',{mode:'no-cors',headers:c}).headers;"
    "guarded.append('Set-Cookie','untouched=true');"
    "return guarded.getSetCookie().length===0&&!guarded.has('set-cookie');});"
    "t('separate cookies count toward quota',()=>{const quota=new Headers();"
    "for(let i=0;i<256;i++)quota.append('set-cookie','a=1');"
    "return throwsName(()=>quota.append('set-cookie','a=2'),'RangeError')"
    "&&throwsName(()=>quota.set('x','z'),'RangeError')"
    "&&(quota.set('set-cookie','b=1'),quota.set('x','z'),quota.getSetCookie().length===1);});"
    "t('Headers fill ignores patched methods',()=>{const proto=Headers.prototype,"
    "append=proto.append,set=proto.set;let calls=0;proto.append=proto.set=()=>{calls++};"
    "try{const pairs=new Headers([['x','1'],['x','2']]),record=new Headers({Y:'1',y:'2'});"
    "return calls===0&&pairs.get('x')==='1, 2'&&record.get('y')==='1, 2'}"
    "finally{proto.append=append;proto.set=set}});"
    "const xhr=new XMLHttpRequest();let opened=0;"
    "xhr.onreadystatechange=()=>opened++;xhr.open('GET','/first');"
    "xhr.open('GET','/second');"
    "t('reopen does not emit unchanged readyState',()=>opened===1&&xhr.readyState===1);"
    "xhr.responseType='json';"
    "t('invalid xhr enum ignored',()=>{xhr.responseType='invalid';return xhr.responseType==='json'});"
    "t('sync Window open cannot retain typed response',()=>throwsName("
    "()=>xhr.open('GET','/sync',false),'InvalidAccessError')&&xhr.responseType==='json'"
    "&&opened===1);"
    "xhr.responseType='';xhr.open('GET','/sync',null);"
    "t('sync Window xhr rejects responseType',()=>throwsName("
    "()=>xhr.responseType='json','InvalidAccessError'));"
    PROBE_SUMMARY("FETCH-VALUES-OK");

static const char fetch_form_body_probe[] =
    PROBE_PRELUDE
    "(async()=>{try{"
    "const failureName=async(p)=>p.then(()=>'',e=>e.name);"
    "const filename='folder/\\ud800\\r\\n\"file.bin',"
    "file=new File(['x'],filename,{lastModified:NaN});"
    "t('File metadata is readonly and USVString',()=>"
    "file.name==='folder/\\ufffd\\r\\n\"file.bin'&&file.lastModified===0"
    "&&!Object.hasOwn(file,'name')&&!Object.hasOwn(file,'lastModified')"
    "&&!Reflect.set(file,'name','wrong')&&!Reflect.set(file,'lastModified',123)"
    "&&throwsName(()=>Object.getOwnPropertyDescriptor(File.prototype,'name')"
    ".get.call(new Blob()),'TypeError'));"
    "const stamp=n=>new File([],'f',{lastModified:n}).lastModified;"
    "t('File lastModified uses signed Web IDL long long',()=>"
    "stamp(-2.7)===-2&&Object.is(stamp(-0.7),0)&&stamp(Infinity)===0"
    "&&stamp(2**63)===-(2**63)&&stamp(2**64)===0"
    "&&throwsName(()=>stamp(1n),'TypeError')"
    "&&throwsName(()=>new File([],Symbol()),'TypeError')"
    "&&new File([],'f',null).lastModified>0);"
    "const namedForm=new FormData();namedForm.append('f',file);"
    "Object.defineProperty(file,'name',{value:'shadow'});"
    "const namedWire=await new Request('/form',{method:'POST',body:namedForm}).text();"
    "t('multipart writes original File name and escapes quotes and newlines',()=>"
    "namedWire.includes('filename=\"folder/\\ufffd%0D%0A%22file.bin\"')"
    "&&!namedWire.includes('filename=\"shadow\"'));"
    "const navigationFields=new FormData();"
    "navigationFields.append('meta\\rfirst\\nnext\\r\\nlast','value\\nnext\\rlast');"
    "navigationFields.append('sample',new File(['sample'],'a\\rb\\nc\".bin'));"
    "const uploadRequest=new Request('/upload',{method:'POST',body:navigationFields});"
    "const uploadBoundary=uploadRequest.headers.get('content-type').split('boundary=')[1];"
    "const uploadWire=await uploadRequest.text();"
    "t('script multipart matches navigation field and filename encoding',()=>"
    "uploadWire==='--'+uploadBoundary+'\\r\\nContent-Disposition: form-data; '"
    "+'name=\"meta%0D%0Afirst%0D%0Anext%0D%0Alast\"\\r\\n\\r\\nvalue\\r\\nnext\\r\\nlast\\r\\n'"
    "+'--'+uploadBoundary+'\\r\\nContent-Disposition: form-data; '"
    "+'name=\"sample\"; filename=\"a%0Db%0Ac%22.bin\"\\r\\n'"
    "+'Content-Type: application/octet-stream\\r\\n\\r\\nsample\\r\\n'"
    "+'--'+uploadBoundary+'--\\r\\n');"
    "const form=new FormData();form.append('same','first');form.append('same','second');"
    "form.append('meta;tag','value\\nsecond');"
    "form.append('upload',new File([new Uint8Array([0,255,13,10])],"
    "'packed.bin',{type:'application/octet-stream'}));"
    "for(const [label,body] of [['request',new Request('/form',{method:'POST',body:form})],"
    "['response',new Response(form)]]){"
    "try{const parsed=await body.formData(),upload=parsed.get('upload');"
    "const bytes=Array.from(new Uint8Array(await upload.arrayBuffer()));"
    "t(label+' multipart round trip',()=>parsed.getAll('same').join()==='first,second'"
    "&&parsed.get('meta;tag')==='value\\r\\nsecond'&&upload instanceof File"
    "&&upload.name==='packed.bin'&&upload.type==='application/octet-stream'"
    "&&bytes.join()==='0,255,13,10'&&body.bodyUsed);"
    "const twice=await failureName(body.formData());"
    "t(label+' consumed body rejects',()=>twice==='TypeError');"
    "}catch(e){failures.push(label+' multipart round trip:'+e)}}"
    "const urlEncoded=new Response('?q=y',{headers:{"
    "'content-type':'Application/X-WWW-Form-Urlencoded; charset=UTF-8'}});"
    "try{const decoded=await urlEncoded.formData();"
    "t('exact case-insensitive MIME type',()=>decoded.get('?q')==='y');}"
    "catch(e){failures.push('exact case-insensitive MIME type:'+e)}"
    "const prefix=new Response('a=x',{headers:{"
    "'content-type':'application/x-www-form-urlencoded-oops'}});"
    "const prefixError=await failureName(prefix.formData());"
    "t('reject MIME prefixes and consume',()=>prefixError==='TypeError'&&prefix.bodyUsed);"
    "const typed=(body,boundary='short')=>new Response(body,{headers:{"
    "'content-type':'Multipart/Form-Data; unused=\"x;y\"; boundary=\"'+boundary+'\"'}});"
    "const wire='--short\\r\\nContent-Disposition: form-data; name=\"item\"\\r\\n\\r\\n'"
    "+'a\\r\\n--shorter\\r\\nb\\r\\n--short--';"
    "try{const parsed=await typed(wire).formData();"
    "t('quoted boundary, closing EOF and exact marker',()=>parsed.get('item')"
    "==='a\\r\\n--shorter\\r\\nb');}catch(e){failures.push('quoted boundary:'+e)}"
    "try{const empty=await new Response(new FormData()).formData();"
    "t('empty multipart',()=>Array.from(empty).length===0);}"
    "catch(e){failures.push('empty multipart:'+e)}"
    "const truncated=await failureName(typed(wire.slice(0,-9)).formData());"
    "t('multipart missing close',()=>truncated==='TypeError');"
    "const largeHeader=await failureName(typed('--short\\r\\nUnneeded: '+"
    "'x'.repeat(8192)+'\\r\\nContent-Disposition: form-data; name=x\\r\\n\\r\\nx'"
    "+'\\r\\n--short--').formData());"
    "t('multipart header scan stays bounded',()=>largeHeader==='TypeError');"
    "const tooMany=await failureName(typed(('"
    "--short\\r\\nContent-Disposition: form-data; name=x\\r\\n\\r\\nx\\r\\n').repeat(257)"
    "+'--short--').formData());"
    "t('multipart entries stay bounded',()=>tooMany==='RangeError');"
    "}catch(e){failures.push('aborted:'+e)}"
    PROBE_SUMMARY("FETCH-FORM-BODY-OK") "})();";

static const char fetch_worker_api_probe[] =
    "const src=\"const x=new XMLHttpRequest();"
    "x.open('GET','https://conformance.test/unused',false);x.responseType='json';"
    "const h=new Headers();h.append('set-cookie','a=1');h.append('set-cookie','b=2');"
    "let rejected=false;try{Response.json(undefined)}catch(e){rejected=e.name==='TypeError'}"
    "postMessage(x.responseType==='json'&&h.getSetCookie().join()==='a=1,b=2'"
    "&&h.getSetCookie() instanceof Array&&rejected&&Response.json(4,null).status===200)\","
    "url=URL.createObjectURL(new Blob([src])),w=new Worker(url);"
    "w.onerror=()=>globalThis.pocSummary='FAIL worker API exception';"
    "w.onmessage=e=>{globalThis.pocSummary=e.data?'FETCH-WORKER-OK':'FAIL worker mismatch';"
    "w.terminate();URL.revokeObjectURL(url);};";

/* The browser's own Fetch and XHR code (body Content-Type inference, the
   no-cors guard, Response.json, multipart bodies, body MIME types, XHR
   request headers) uses Fetch's internal header operations, never
   Headers.prototype methods a page replaced. Run with and without the
   interface snapshot, which otherwise hides the page's patch from the
   browser. */
static const char fetch_patched_headers_probe[] =
    PROBE_PRELUDE
    "(async()=>{const proto=Headers.prototype,saved={},names=['append','set',"
    "'has','get','delete'];let calls=0;"
    "for(const name of names){saved[name]=proto[name];proto[name]=function(){"
    "calls++;return name==='has'?true:name==='get'?'x/patched':undefined}}"
    "const type=(headers)=>saved.get.call(headers,'content-type');"
    "try{const form=new FormData();form.append('a','1');"
    "const text=new Request('/p',{method:'POST',body:'t'}),"
    "params=new Request('/p',{method:'POST',body:new URLSearchParams('a=1')}),"
    "multipart=new Request('/p',{method:'POST',body:form}),"
    "guarded=new Request('/p',{mode:'no-cors',headers:{accept:'a','x-y':'z'}}),"
    "json=Response.json({a:1}),"
    "typed=new Response('x',{headers:{'content-type':'a/b'}}),"
    "encoded=new Response('a=1',{headers:{"
    "'content-type':'application/x-www-form-urlencoded'}});"
    "const xhr=new XMLHttpRequest();xhr.open('POST','/x');"
    "xhr.setRequestHeader('X-A','1');"
    "t('inferred text type',()=>type(text.headers)==='text/plain;charset=UTF-8');"
    "t('inferred params type',()=>type(params.headers)==="
    "'application/x-www-form-urlencoded;charset=UTF-8');"
    "t('multipart type',()=>type(multipart.headers).startsWith("
    "'multipart/form-data; boundary='));"
    "t('no-cors guard',()=>JSON.stringify([...guarded.headers])==="
    "'[[\"accept\",\"a\"]]');"
    "t('Response.json type',()=>type(json.headers)==='application/json');"
    "const blob=await typed.blob(),parsed=await encoded.formData();"
    "t('body MIME type',()=>blob.type==='a/b');"
    "t('form body parse',()=>parsed.get('a')==='1');"
    "t('no patched call',()=>calls===0);"
    "}catch(e){failures.push('aborted:'+e)}"
    "finally{for(const name of names)proto[name]=saved[name]}"
    PROBE_SUMMARY("FETCH-PATCHED-HEADERS-OK") "})();";

static int test_fetch_api_edges(void)
{
    puts("test: Fetch/Headers/XHR value and transition edge cases");
    int failed = !run_probe("fetch API values", fetch_value_probe, "FETCH-VALUES-OK");
    failed += !run_probe("fetch form body", fetch_form_body_probe, "FETCH-FORM-BODY-OK");
    failed += !run_probe("fetch Worker API", fetch_worker_api_probe, "FETCH-WORKER-OK");
    for (int no_snapshot = 0; no_snapshot < 2; no_snapshot++)
        failed += !run_probe_realm(
            no_snapshot ? "fetch patched Headers, no snapshot"
                        : "fetch patched Headers",
            "<p id=p>text</p>", fetch_patched_headers_probe,
            "FETCH-PATCHED-HEADERS-OK", no_snapshot != 0);
    return failed;
}

/* ---- Live Range limits and replace-all mutations ---- */

/* A page that keeps many Ranges got QuotaExceededError from new Range()
   past 256; the oldest now stop following mutations instead. textContent
   and innerHTML move boundaries inside the replaced children to (parent,
   0) even without a MutationObserver, which no longer snapshots child
   lists for Ranges alone (its 256-child cap left a boundary in a removed
   child beyond it). */
static const char live_ranges_probe[] =
    PROBE_PRELUDE
    "const p=document.getElementById('p'),text=p.firstChild;"
    "const keep=[];let thrown=null;"
    "try{for(let i=0;i<5000;i++)keep.push(i&1?new Range():"
    "document.createRange())}catch(e){thrown=e}"
    "t('no quota error',()=>thrown===null&&keep.length===5000);"
    "const newest=keep[4999];newest.setStart(text,2);newest.setEnd(text,4);"
    "text.insertData(0,'ab');"
    "t('newest live',()=>newest.startOffset===4&&newest.endOffset===6);"
    "t('oldest readable',()=>keep[0].startContainer===document"
    "&&keep[0].collapsed);"
    "keep.length=0;"
    "const box=document.createElement('div');document.body.appendChild(box);"
    "const fill=()=>{box.textContent='';for(let i=0;i<300;i++){"
    "const c=document.createElement('b');c.textContent='c'+i;"
    "box.appendChild(c)}};"
    "const check=(label,replace,observe)=>{fill();"
    "const inner=new Range(),outer=new Range(),far=new Range();"
    "inner.setStart(box.children[280].firstChild,1);inner.setEnd(box,290);"
    "outer.setStart(box.children[2].firstChild,1);outer.setEnd(box,5);"
    "far.setStart(text,1);far.setEnd(text,2);"
    "const o=observe?new MutationObserver(()=>{}):null;"
    "if(o)o.observe(box,{childList:true});replace();if(o)o.disconnect();"
    "t(label+' inner',()=>inner.startContainer===box&&inner.startOffset===0"
    "&&inner.endContainer===box&&inner.endOffset===0);"
    "t(label+' outer',()=>outer.startContainer===box&&outer.startOffset===0"
    "&&outer.collapsed);"
    "t(label+' far',()=>far.startContainer===text&&far.startOffset===1"
    "&&far.endOffset===2)};"
    "check('textContent',()=>{box.textContent='x'},false);"
    "check('innerHTML',()=>{box.innerHTML='<i>y</i>'},false);"
    "check('observed textContent',()=>{box.textContent='x'},true);"
    "check('observed innerHTML',()=>{box.innerHTML='<i>y</i>'},true);"
    PROBE_SUMMARY("LIVE-RANGES-OK");

static int test_live_ranges(void)
{
    puts("test: live Ranges past the tracking limit and replace-all edits");
    CHECK(run_probe("live-ranges", live_ranges_probe, "LIVE-RANGES-OK"));
    return 0;
}

/* ---- Markup of secondary documents' elements ---- */

/* Chromium's results: an XML document's elements (DOMParser's SVG
   documents, createDocument) serialize as XML for innerHTML and outerHTML,
   each top-level child of innerHTML declaring its own namespace (WPT
   domparsing/innerhtml-03.xhtml), while getHTML() stays HTML; template
   contents stay HTML; and the root element of a createHTMLDocument or
   DOMParser document, whose parent is the document, refuses an outerHTML
   assignment. */
static const char document_markup_probe[] =
    PROBE_PRELUDE
    "const S='http://www.w3.org/2000/svg',X='http://www.w3.org/1999/xhtml';"
    "const svg=new DOMParser().parseFromString('<svg xmlns=\"'+S+'\"><g a=\"1\">"
    "<text>a&amp;b</text></g><rect/></svg>','image/svg+xml').documentElement;"
    "t('svg document innerHTML',()=>svg.innerHTML==='<g xmlns=\"'+S+'\" a=\"1\">"
    "<text>a&amp;b</text></g><rect xmlns=\"'+S+'\"/>');"
    "t('svg document outerHTML',()=>svg.outerHTML==='<svg xmlns=\"'+S+'\">"
    "<g a=\"1\"><text>a&amp;b</text></g><rect/></svg>');"
    "t('svg document getHTML',()=>svg.getHTML()==="
    "'<g a=\"1\"><text>a&amp;b</text></g><rect></rect>');"
    "t('svg document new element',()=>"
    "svg.ownerDocument.createElementNS(S,'circle').outerHTML==='<circle xmlns=\"'+S+'\"/>');"
    "const xd=document.implementation.createDocument(X,'html'),"
    "div=()=>xd.createElement('div');"
    "t('xml children declare namespace',()=>{const d=div();"
    "d.appendChild(xd.createElement('xmp')).appendChild(xd.createElement('span'))"
    ".appendChild(xd.createTextNode('<'));"
    "return d.innerHTML==='<xmp xmlns=\"'+X+'\"><span>&lt;</span></xmp>'});"
    "t('xml void child',()=>{const d=div();d.appendChild(xd.createElement('br'));"
    "return d.innerHTML==='<br xmlns=\"'+X+'\" />'});"
    "t('xml prefixed child',()=>{const d=div();"
    "d.appendChild(xd.createElementNS(X,'html:br'));"
    "return d.innerHTML==='<html:br xmlns:html=\"'+X+'\" />'});"
    "t('template contents stay HTML',()=>{const p=document.createElement('template');"
    "p.innerHTML='<p><br></p>';return p.content.firstChild.innerHTML==='<br>'});"
    "const hd=document.implementation.createHTMLDocument('t');"
    "t('createHTMLDocument markup',()=>{const e=hd.createElement('div');"
    "e.innerHTML='<b>x</b>';hd.body.appendChild(e);"
    "return e.innerHTML==='<b>x</b>'&&e.outerHTML==='<div><b>x</b></div>'"
    "&&hd.body.innerHTML==='<div><b>x</b></div>'});"
    "for(const [name,doc] of [['createHTMLDocument',hd],['DOMParser',"
    "new DOMParser().parseFromString('<p>x</p>','text/html')]])"
    "t(name+' root outerHTML',()=>{const root=doc.documentElement,"
    "before=root.outerHTML;return throwsName(()=>{root.outerHTML='<html></html>'},"
    "'NoModificationAllowedError')&&doc.documentElement===root"
    "&&root.outerHTML===before});"
    PROBE_SUMMARY("DOCUMENT-MARKUP-OK");

static int test_document_markup(void)
{
    puts("test: markup of secondary documents' elements");
    CHECK(run_probe("document-markup", document_markup_probe,
                    "DOCUMENT-MARKUP-OK"));
    return 0;
}

/* ---- Namespaced attributes through copies and Attr nodes ---- */

/* Chromium's results: a copy (cloneNode, importNode, deep or shallow, from
   any document) keeps each attribute's namespace, prefix and case, and
   setAttributeNodeNS / setAttributeNode with a namespaced Attr replace the
   old Attr, which keeps its value and is returned, with one mutation
   record carrying the old value. */
static const char namespace_mirror_overflow_probe[] =
    PROBE_PRELUDE
    "const made=()=>{const e=document.createElement('div');"
    "e.setAttributeNS('urn:x','p:a','1');return e};"
    "t('namespace mirror index overflow preserves copies and resets when empty',()=>{"
    "const root=document.createElement('section'),owners=[];"
    "for(let i=0;i<132;i++)owners.push(made());"
    "root.appendChild(owners[131]);const copy=root.cloneNode(true).firstChild;"
    "const preserved=copy.getAttributeNS('urn:x','a')==='1'&&copy.attributes[0].prefix==='p'"
    "&&copy.attributes[0]!==owners[131].attributes[0];"
    "copy.removeAttributeNS('urn:x','a');"
    "for(const owner of owners)owner.removeAttributeNS('urn:x','a');"
    "let walked=false;Object.defineProperty(root,'firstElementChild',{get(){"
    "walked=true;return root.firstChild}});"
    "const cleared=root.cloneNode(true).firstChild.attributes.length===0&&!walked;"
    "const fresh=document.createElement('section');fresh.appendChild(made());"
    "return preserved&&cleared&&fresh.cloneNode(true).firstChild"
    ".getAttributeNS('urn:x','a')==='1'&&!walked});"
    PROBE_SUMMARY("NAMESPACE-MIRROR-OVERFLOW-OK");

static const char namespaced_attribute_probe[] =
    PROBE_PRELUDE
    "const XL='http://www.w3.org/1999/xlink',S='http://www.w3.org/2000/svg',"
    "shape=a=>[a.name,a.namespaceURI,a.prefix,a.localName,a.value].join();"
    "const made=(doc=document,tag='div')=>{const e=doc.createElement(tag);"
    "e.setAttributeNS('urn:x','p:a','1');return e};"
    "t('shallow clone',()=>shape(made().cloneNode().attributes[0])==='p:a,urn:x,p,a,1');"
    "t('deep clone',()=>{const box=document.createElement('section');"
    "box.appendChild(document.createElement('i')).appendChild(made());"
    "const copy=box.cloneNode(true).firstChild.firstChild;"
    "return shape(copy.attributes[0])==='p:a,urn:x,p,a,1'"
    "&&copy.getAttributeNS('urn:x','a')==='1'});"
    "t('deep clone after moving a namespaced descendant',()=>{"
    "const left=document.createElement('section'),right=document.createElement('section'),"
    "branch=left.appendChild(document.createElement('b')),leaf=branch.appendChild(made());"
    "right.appendChild(branch);Object.defineProperty(leaf,'parentNode',{value:null});"
    "const copy=right.cloneNode(true).firstChild.firstChild;"
    "return !left.cloneNode(true).firstChild&&copy.getAttributeNS('urn:x','a')==='1'"
    "&&copy.attributes[0]!==leaf.attributes[0]});"
    "t('importNode',()=>{const hd=document.implementation.createHTMLDocument('');"
    "const box=hd.createElement('p');box.appendChild(made(hd));"
    "return shape(document.importNode(box,true).firstChild.attributes[0])"
    "==='p:a,urn:x,p,a,1'});"
    "t('template clone',()=>shape(made(document,'template').cloneNode(true)"
    ".attributes[0])==='p:a,urn:x,p,a,1');"
    "t('svg xlink clone',()=>{const g=document.createElementNS(S,'g'),"
    "use=g.appendChild(document.createElementNS(S,'use'));"
    "use.setAttributeNS(XL,'xlink:href','#i');"
    "return shape(g.cloneNode(true).firstChild.attributes[0])"
    "==='xlink:href,'+XL+',xlink,href,#i'});"
    "t('case kept in a clone',()=>{const e=document.createElement('div');"
    "e.setAttributeNS(null,'Foo','v');const c=e.cloneNode();"
    "return c.attributes[0].name==='Foo'&&c.getAttributeNS(null,'Foo')==='v'});"
    "for(const method of ['setAttributeNodeNS','setAttributeNode']){"
    "t(method+' appends a reattached ordinary Attr once',()=>{"
    "const e=document.createElement('div'),a=document.createAttribute('a'),"
    "b=document.createAttribute('b');a.value='1';e[method](a);e[method](b);"
    "e.removeAttribute('a');const detached=a.ownerElement===null&&a.value==='1'"
    "&&e.attributes.length===1&&e.attributes[0]===b;"
    "const result=e[method](a);a.value='2';"
    "return detached&&result===null&&a.ownerElement===e&&e.attributes.length===2"
    "&&e.attributes[0]===b&&e.attributes[1]===a&&e.getAttribute('a')==='2'});"
    "t(method+' keeps first insertion identity',()=>{const e=document.createElement('div'),"
    "a=document.createAttributeNS('urn:x','p:a');a.value='v';"
    "const result=e[method](a);a.value='new';"
    "return result===null&&a.ownerElement===e&&e.attributes[0]===a"
    "&&e.getAttributeNS('urn:x','a')==='new'&&e.removeAttributeNode(a)===a"
    "&&a.ownerElement===null&&!e.hasAttributeNS('urn:x','a')});"
    "t(method+' matches null namespace, not the qualified name',()=>{"
    "const e=document.createElement('div');e.setAttributeNS('urn:x','a','one');"
    "const ns=e.attributes[0],a=document.createAttribute('a');a.value='two';"
    "const result=e[method](a);a.value='new';"
    "const clone=e.cloneNode();"
    "const both=result===null&&ns.ownerElement===e&&a.ownerElement===e"
    "&&e.attributes.length===2&&e.attributes[0]===ns&&e.attributes[1]===a"
    "&&e.getAttribute('a')==='one'&&e.getAttributeNS('urn:x','a')==='one'"
    "&&e.getAttributeNS(null,'a')==='new'&&clone.getAttributeNS(null,'a')==='new'"
    "&&clone.getAttributeNS('urn:x','a')==='one';"
    "e.setAttribute('a','three');const byName=ns.value==='three'&&a.value==='new';"
    "e.removeAttribute('a');"
    "return both&&byName&&ns.ownerElement===null&&e.attributes[0]===a"
    "&&e.getAttribute('a')==='new'});"
    "t(method+' replaces',()=>{const e=made(),old=e.attributes[0],"
    "next=document.createAttributeNS('urn:x','z:a');next.value='9';"
    "const o=new MutationObserver(()=>{});o.observe(e,{attributes:true,"
    "attributeOldValue:true});const returned=e[method](next),"
    "records=o.takeRecords();o.disconnect();"
    "return returned===old&&old.value==='1'&&old.ownerElement===null"
    "&&e.attributes.length===1&&e.attributes[0]===next&&next.ownerElement===e"
    "&&e.getAttributeNS('urn:x','a')==='9'&&e.getAttribute('p:a')===null"
    "&&e.outerHTML==='<div z:a=\"9\"></div>'&&records.length===1"
    "&&records[0].oldValue==='1'&&records[0].attributeNamespace==='urn:x'});}"
    PROBE_SUMMARY("NAMESPACED-ATTRIBUTES-OK");

static int test_namespaced_attributes(void)
{
    puts("test: namespaced attributes through copies and Attr nodes");
    const bool overflow = run_probe_realm("namespace-mirror-overflow", "",
                            namespace_mirror_overflow_probe,
                            "NAMESPACE-MIRROR-OVERFLOW-OK", true);
    CHECK(run_probe("namespaced-attributes", namespaced_attribute_probe,
                    "NAMESPACED-ATTRIBUTES-OK"));
    CHECK(overflow);
    return 0;
}

/* ---- getHTML() members ---- */

/* Chromium's results: Element.prototype.getHTML and
   ShadowRoot.prototype.getHTML are separate operations, each refusing the
   other interface's receivers; the options argument is a GetHTMLOptions
   dictionary whose shadowRoots holds only ShadowRoots. Shadow roots are
   never serializable here, so only the listed roots are included. */
static const char get_html_probe[] =
    PROBE_PRELUDE
    "const host=document.createElement('div');host.innerHTML='<i>l</i>';"
    "const root=host.attachShadow({mode:'closed'});root.innerHTML='<b>s</b>';"
    "const E=Element.prototype.getHTML,R=ShadowRoot.prototype.getHTML;"
    "t('separate members',()=>E!==R&&E.name==='getHTML'&&R.name==='getHTML'"
    "&&E.length===0&&R.length===0);"
    "t('ShadowRoot member refuses an element',()=>throwsName(()=>R.call(host),"
    "'TypeError'));"
    "t('Element member refuses a shadow root',()=>throwsName(()=>E.call(root),"
    "'TypeError'));"
    "t('not a constructor',()=>throwsName(()=>new E(),'TypeError'));"
    "t('options must be a dictionary',()=>throwsName(()=>host.getHTML(5),"
    "'TypeError')&&host.getHTML(null)==='<i>l</i>');"
    "t('shadowRoots holds ShadowRoots',()=>throwsName(()=>"
    "host.getHTML({shadowRoots:[host]}),'TypeError'));"
    "t('listed root',()=>host.getHTML({shadowRoots:[root]})==="
    "'<template shadowrootmode=\"closed\"><b>s</b></template><i>l</i>');"
    "t('ShadowRoot getHTML',()=>root.getHTML()==='<b>s</b>');"
    PROBE_SUMMARY("GET-HTML-OK");

static int test_get_html(void)
{
    puts("test: getHTML() members and options");
    CHECK(run_probe("get-html", get_html_probe, "GET-HTML-OK"));
    return 0;
}

/* ---- Console arguments ---- */

/* With no console trace, console methods hand their arguments on without
   reading them: no stack getter, toString or toPrimitive fires (as in a
   browser with its developer tools closed). */
static const char console_untouched_probe[] =
    PROBE_PRELUDE
    "let reads=0;const o={get stack(){reads++;return 's'},"
    "toString(){reads++;return 'o'},[Symbol.toPrimitive](){reads++;return 'p'}};"
    "const e=new Error('x');Object.defineProperty(e,'stack',"
    "{get(){reads++;return 'trace'}});"
    "for(const m of ['log','info','warn','error','debug','trace'])"
    "console[m](o,e,'text',1);"
    "t('no argument read',()=>reads===0);"
    "t('method names',()=>console.log.name==='log'&&console.trace.name==='trace');"
    PROBE_SUMMARY("CONSOLE-UNTOUCHED-OK");

static int test_console_untouched(void)
{
    puts("test: console methods leave their arguments unread");
    CHECK(run_probe("console-untouched", console_untouched_probe,
                    "CONSOLE-UNTOUCHED-OK"));
    return 0;
}

/* ---- Built-in function names ---- */

/* Built-in functions carry their property's name, as in other browsers. */
static const char builtin_names_probe[] =
    PROBE_PRELUDE
    "t('Date.now',()=>Date.now.name==='now'"
    "&&String(Date.now).startsWith('function now('));"
    PROBE_SUMMARY("BUILTIN-NAMES-OK");

static int test_builtin_names(void)
{
    puts("test: built-in function names");
    CHECK(run_probe("builtin-names", builtin_names_probe, "BUILTIN-NAMES-OK"));
    return 0;
}

/* ---- No touch event handlers ---- */

/* The device has no touch screen (no TouchEvent, maxTouchPoints 0), so
   the touch handler attributes are absent, as in a desktop browser; the
   other handler attributes stay. */
static const char no_touch_handlers_probe[] =
    PROBE_PRELUDE
    "const e=document.createElement('div');"
    "for(const k of ['ontouchstart','ontouchend','ontouchmove','ontouchcancel'])"
    "t(k+' absent',()=>!(k in window)&&!(k in document)&&!(k in e));"
    "for(const k of ['onclick','onpointerdown','onwheel','ontransitionend'])"
    "t(k+' present',()=>(k in window)&&(k in document)&&(k in e));"
    "t('maxTouchPoints',()=>navigator.maxTouchPoints===0);"
    PROBE_SUMMARY("NO-TOUCH-HANDLERS-OK");

static int test_no_touch_handlers(void)
{
    puts("test: no touch event handler attributes");
    CHECK(run_probe("no-touch-handlers", no_touch_handlers_probe,
                    "NO-TOUCH-HANDLERS-OK"));
    return 0;
}

/* ---- Media features ---- */

/* matchMedia answers the device's values: a browser tab with scripts, an
   ordinary screen (sRGB, standard range, fast updates, colors not
   inverted) whose size is screen's, at one device pixel per CSS pixel. */
static const char media_features_probe[] =
    PROBE_PRELUDE
    "const m=q=>matchMedia(q).matches,w=screen.width,h=screen.height;"
    "for(const q of ['(scripting: enabled)','(scripting)',"
    "'(display-mode: browser)','(update: fast)','(update)',"
    "'(dynamic-range: standard)','(video-dynamic-range: standard)',"
    "'(inverted-colors: none)','(device-width: '+w+'px)',"
    "'(device-height: '+h+'px)','(min-device-width: '+w+'px)',"
    "'(max-device-width: '+w+'px)','(device-aspect-ratio: '+w+'/'+h+')',"
    "'(-webkit-min-device-pixel-ratio: 1)','(-webkit-device-pixel-ratio: 1)',"
    "'(-webkit-max-device-pixel-ratio: 1.5)','(pointer: coarse)',"
    "'(hover: none)'])t(q,()=>m(q));"
    "for(const q of ['(scripting: none)','(display-mode: standalone)',"
    "'(update: slow)','(dynamic-range: high)','(inverted-colors)',"
    "'(inverted-colors: inverted)','(device-width: '+(w+1)+'px)',"
    "'(max-device-width: '+(w-1)+'px)',"
    "'(-webkit-min-device-pixel-ratio: 2)','(-webkit-device-pixel-ratio: 1x)',"
    "'(scripting: sometimes)'])t('not '+q,()=>!m(q));"
    PROBE_SUMMARY("MEDIA-FEATURES-OK");

static int test_media_features(void)
{
    puts("test: media features describe the device");
    CHECK(run_probe("media-features", media_features_probe,
                    "MEDIA-FEATURES-OK"));
    return 0;
}

/* ---- Label activation ---- */

/* click() on an element inside a label activates the labelled control, as
   a click on the label does; interactive content inside keeps the click. */
static const char label_descendant_probe[] =
    PROBE_PRELUDE
    "document.body.innerHTML='<label><input type=checkbox id=a>"
    "<span id=sa>text</span></label><label for=b><b><i id=ib>x</i></b>"
    "</label><input type=checkbox id=b><label><input type=checkbox id=c>"
    "<a href=#n id=ac>link</a></label>';"
    "const $=id=>document.getElementById(id);"
    "$('sa').click();t('wrapped control',()=>$('a').checked);"
    "$('ib').click();t('for control',()=>$('b').checked);"
    "$('ac').addEventListener('click',e=>e.preventDefault());$('ac').click();"
    "t('interactive descendant',()=>!$('c').checked);"
    "$('a').click();t('control itself toggles once',()=>!$('a').checked);"
    PROBE_SUMMARY("LABEL-DESCENDANT-OK");

static int test_label_descendant(void)
{
    puts("test: label activation from a descendant click");
    CHECK(run_probe("label-descendant", label_descendant_probe,
                    "LABEL-DESCENDANT-OK"));
    return 0;
}

/* ---- Promise rejection events ---- */

/* An unhandled rejection fires PromiseRejectionEvent unhandledrejection
   (with its promise and reason) on the window, and rejectionhandled once
   a handler is attached later; a handled rejection fires nothing. */
static const char rejection_events_probe[] =
    PROBE_PRELUDE
    "t('constructor',()=>typeof PromiseRejectionEvent==='function'"
    "&&throwsName(()=>new PromiseRejectionEvent('x',{}),'TypeError'));"
    "const seen=[];let unhandled=null,handled=null;"
    "addEventListener('unhandledrejection',e=>{unhandled=e;seen.push(e.type)});"
    "addEventListener('rejectionhandled',e=>{handled=e;seen.push(e.type)});"
    "const reason=new Error('r'),p=Promise.reject(reason);"
    "Promise.reject(1).catch(()=>{});"
    "setTimeout(()=>{"
    "t('unhandledrejection',()=>unhandled instanceof PromiseRejectionEvent"
    "&&unhandled.promise===p&&unhandled.reason===reason&&unhandled.isTrusted"
    "&&unhandled.cancelable&&seen.join()==='unhandledrejection');"
    "p.catch(()=>{});"
    "setTimeout(()=>{"
    "t('rejectionhandled',()=>handled instanceof PromiseRejectionEvent"
    "&&handled.promise===p&&handled.reason===reason&&!handled.cancelable"
    "&&seen.join()==='unhandledrejection,rejectionhandled');"
    PROBE_SUMMARY("REJECTION-EVENTS-OK") "},20)},20);";

static int test_rejection_events(void)
{
    puts("test: window promise rejection events");
    CHECK(run_probe("rejection-events", rejection_events_probe,
                    "REJECTION-EVENTS-OK"));
    return 0;
}

/* Rejections made while the task queue is full cannot be scheduled: the
   tracker keeps no record of them. Once the queue drains, a later
   unhandled rejection still fires unhandledrejection, and the refused
   ones (promise and reason) can be collected. */
static int test_rejection_events_after_full_queue(void)
{
    puts("test: window rejection events after a full task queue");
    Budget budget;
    budget_init(&budget, 24 * MIB);
    budget_install_lexbor(&budget);
    PocDocument document;
    static const char page[] =
        "<!doctype html><html><body><p>text</p></body></html>";
    CHECK(document_parse(&document, &budget, page, sizeof(page) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntimeOptions options = { .defer_document_scripts = true };
    CHECK(viewport_context_init(&options.viewport, 480, 272, 480, 272));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 12u * MIB, 3000,
        "https://conformance.test/platform", &options, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.seen=[];globalThis.refs=[];globalThis.ids=[];"
        "addEventListener('unhandledrejection',e=>seen.push(e.reason));"
        "for(let i=0;i<256;i++){const id=setTimeout(()=>{},1e6);"
        "if(!id)break;ids.push(id)}"
        "for(let i=0;i<24;i++){const reason={i};refs.push(new WeakRef(reason));"
        "Promise.reject(reason)}"
        "pocSummary=ids.length<256?'FULL':'NOT-FULL'",
        "rejections-full-queue", &result));
    CHECK(strcmp(result.summary, "FULL") == 0);
    CHECK(script_runtime_advance(runtime, 1u, 8u, &result));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "for(const id of ids)clearTimeout(id);ids.length=0;pocSummary='DRAINED'",
        "rejections-drain", &result));
    (void) script_runtime_collect_and_trim(runtime);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.kept=refs.filter(r=>r.deref()).length;"
        "Promise.reject('late');pocSummary='pending'",
        "rejections-late", &result));
    for (unsigned turn = 0; turn < 16u; turn++)
        CHECK(script_runtime_advance(runtime, 1u, 8u, &result));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "pocSummary=kept+':'+seen.join()", "rejections-check", &result));
    bool ok = strcmp(result.summary, "0:late") == 0;
    if (!ok) fprintf(stderr, "rejections after full queue: %s\n",
                     result.summary);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    ok = ok && budget.current == 0;
    budget_uninstall_lexbor(&budget);
    CHECK(ok);
    return 0;
}

/* ---- Window event handler attributes ---- */

/* The WindowEventHandlers attributes exist before assignment and run for
   dispatchEvent and for the browser's own events (the same-document commit
   and restore the host calls). */
static const char window_handlers_probe[] =
    PROBE_PRELUDE
    "const names=['onhashchange','onpopstate','onpagehide','onpageshow',"
    "'onbeforeunload','onstorage','onunhandledrejection','onrejectionhandled',"
    "'onlanguagechange','onunload','onafterprint','onbeforeprint'];"
    "for(const n of names)t(n+' in window',()=>n in window&&window[n]===null);"
    "const seen=[];for(const n of names)window[n]=e=>seen.push(e.type);"
    "for(const n of names)window.dispatchEvent(new Event(n.slice(2)));"
    "t('handlers run for dispatchEvent',()=>seen.length===names.length);"
    "for(const [o,n] of [[Window.prototype,'onhashchange'],"
    "[HTMLElement.prototype,'onclick']]){const d=Object.getOwnPropertyDescriptor(o,n);"
    "t(n+' accessor names',()=>d.get.name==='get '+n&&d.set.name==='set '+n"
    "&&d.enumerable&&d.configurable&&!('prototype' in d.get))}"
    "seen.length=0;const at=location.href;"
    "__tilefinchCommitSameDocument(at+'#next',at);"
    "__tilefinchRestoreSameDocument(at,at+'#next');"
    "t('browser hashchange and popstate',()=>"
    "seen.join()==='hashchange,popstate,hashchange');"
    "t('not on elements',()=>!('onhashchange' in document.body)"
    "&&!('onpopstate' in document));"
    PROBE_SUMMARY("WINDOW-HANDLERS-OK");

static int test_window_handlers(void)
{
    puts("test: window event handler attributes");
    CHECK(run_probe("window-handlers", window_handlers_probe,
                    "WINDOW-HANDLERS-OK"));
    return 0;
}

/* ---- Worker global references ---- */

/* In a worker an undeclared name is a ReferenceError, as in a page, while
   a missing property of self stays undefined; shared names still resolve. */
static const char worker_references_probe[] =
    "const src=\"const r=[];"
    "try{missingName;r.push('no throw')}catch(e){r.push(e.name)}"
    "try{new MissingCtor();r.push('no throw')}catch(e){r.push(e.name)}"
    "r.push(typeof missingName,String(self.missingName),"
    "typeof EventTarget,typeof console);postMessage(r.join())\","
    "url=URL.createObjectURL(new Blob([src])),w=new Worker(url);"
    "w.onerror=e=>globalThis.pocSummary='FAIL worker error '+e.message;"
    "w.onmessage=e=>{globalThis.pocSummary=e.data==='ReferenceError,"
    "ReferenceError,undefined,undefined,function,object'"
    "?'WORKER-REFERENCES-OK':'FAIL '+e.data;"
    "w.terminate();URL.revokeObjectURL(url);};";

static int test_worker_references(void)
{
    puts("test: worker global references");
    CHECK(run_probe("worker-references", worker_references_probe,
                    "WORKER-REFERENCES-OK"));
    return 0;
}

/* ---- postMessage needs its message argument ---- */

/* WebIDL: postMessage() with no arguments throws TypeError on the window,
   a MessagePort, a BroadcastChannel, a Worker and a worker's global;
   postMessage(undefined) still posts. */
static const char post_message_arity_probe[] =
    "const r=[],f=g=>{try{g();r.push('no throw')}catch(e){r.push(e.name)}},"
    "b=new BroadcastChannel('arity');"
    "f(()=>postMessage());f(()=>new MessageChannel().port1.postMessage());"
    "f(()=>b.postMessage());b.close();f(()=>postMessage(undefined));"
    "const src=\"let a;try{postMessage();a='no throw'}catch(e){a=e.name}"
    "postMessage(a)\",url=URL.createObjectURL(new Blob([src])),"
    "w=new Worker(url);f(()=>w.postMessage());"
    "w.onerror=e=>globalThis.pocSummary='FAIL worker error '+e.message;"
    "w.onmessage=e=>{const got=r.join()+','+e.data;"
    "globalThis.pocSummary=got==='TypeError,TypeError,TypeError,no throw,"
    "TypeError,TypeError'?'POST-MESSAGE-ARITY-OK':'FAIL '+got;"
    "w.terminate();URL.revokeObjectURL(url);};";

static int test_post_message_arity(void)
{
    puts("test: postMessage requires its message");
    CHECK(run_probe("post-message-arity", post_message_arity_probe,
                    "POST-MESSAGE-ARITY-OK"));
    return 0;
}

/* ---- Web IDL required arguments ---- */

/* WebIDL: an operation or constructor called with fewer arguments than it
   requires throws TypeError before it runs, and its `length` is that
   count. Each row is "receiver: member count ...": every listed operation
   called with count - 1 arguments throws Chrome's "N argument(s)
   required, but only M present." TypeError. The element and text-node
   rows reach the wrappers' shared native-node members; the canvas rows
   load the lazy canvas module. */
static const char required_arguments_probe[] =
    "const svg=document.createElementNS('http://www.w3.org/2000/svg','svg'),"
    "canvas=document.createElement('canvas'),ctx=canvas.getContext('2d'),"
    "R={window:'atob 1 btoa 1 cancelAnimationFrame 1 getComputedStyle 1 "
    "matchMedia 1 queueMicrotask 1 requestAnimationFrame 1 "
    "requestIdleCallback 1 setInterval 1 setTimeout 1 structuredClone 1',"
    "document:'adoptNode 1 createComment 1 createElement 1 "
    "createElementNS 2 createEvent 1 createTextNode 1 elementFromPoint 2 "
    "getElementById 1 getElementsByClassName 1 getElementsByName 1 "
    "getElementsByTagName 1 importNode 1 querySelector 1 "
    "querySelectorAll 1 appendChild 1 contains 1 removeChild 1 "
    "addEventListener 2 removeEventListener 2 dispatchEvent 1',"
    "el:'closest 1 getAttribute 1 getAttributeNS 2 getAttributeNode 1 "
    "getElementsByClassName 1 getElementsByTagName 1 hasAttribute 1 "
    "insertAdjacentElement 2 insertAdjacentHTML 2 insertAdjacentText 2 "
    "matches 1 querySelector 1 querySelectorAll 1 removeAttribute 1 "
    "setAttribute 2 setAttributeNS 3 toggleAttribute 1 appendChild 1 "
    "compareDocumentPosition 1 contains 1 insertBefore 2 isEqualNode 1 "
    "isSameNode 1 removeChild 1 replaceChild 2 addEventListener 2 "
    "dispatchEvent 1 removeEventListener 2',"
    "svg:'getAttribute 1 setAttribute 2 matches 1 appendChild 1 "
    "removeChild 1 addEventListener 2',"
    "text:'appendChild 1 contains 1 isEqualNode 1 removeChild 1 "
    "addEventListener 2',"
    "mql:'addEventListener 2 dispatchEvent 1 removeEventListener 2',"
    "visualViewport:'addEventListener 2 dispatchEvent 1',"
    "classList:'contains 1 replace 2 toggle 1',"
    "range:'selectNode 1 selectNodeContents 1 setEnd 2 setEndAfter 1 "
    "setEndBefore 1 setStart 2 setStartAfter 1 setStartBefore 1',"
    "xhr:'getResponseHeader 1 open 2 setRequestHeader 2',"
    "crypto:'getRandomValues 1',"
    "localStorage:'getItem 1 key 1 removeItem 1 setItem 2',"
    "history:'pushState 2 replaceState 2',navigator:'sendBeacon 1',"
    "performance:'getEntriesByName 1 getEntriesByType 1 mark 1 measure 1',"
    "CSS:'escape 1 supports 1',URL:'createObjectURL 1 revokeObjectURL 1',"
    "customElements:'define 2 get 1',"
    "formData:'append 2 delete 1 get 1 getAll 1 has 1 set 2',"
    "headers:'append 2 delete 1 get 1 has 1 set 2',"
    "params:'append 2 delete 1 get 1 getAll 1 has 1 set 2',"
    "canvas:'getContext 1',"
    "ctx:'arc 5 arcTo 5 bezierCurveTo 6 clearRect 4 createLinearGradient 4 "
    "createPattern 2 createRadialGradient 6 drawImage 3 ellipse 7 "
    "fillRect 4 fillText 3 getImageData 4 lineTo 2 measureText 1 moveTo 2 "
    "putImageData 3 quadraticCurveTo 4 rect 4 rotate 1 scale 2 "
    "strokeRect 4 strokeText 3 transform 6 translate 2'},"
    "O={window,document,el:document.getElementById('p'),svg,"
    "text:document.createTextNode('x'),mql:matchMedia('(min-width:1px)'),"
    "visualViewport,"
    "classList:document.getElementById('p').classList,"
    "range:document.createRange(),xhr:new XMLHttpRequest(),crypto,"
    "localStorage,history,navigator,performance,CSS,URL,customElements,"
    "formData:new FormData(),headers:new Headers(),"
    "params:new URLSearchParams(),canvas,ctx},"
    "C='AnimationEvent BroadcastChannel CloseEvent CustomEvent ErrorEvent "
    "Event EventSource FocusEvent HashChangeEvent InputEvent "
    "IntersectionObserver KeyboardEvent MessageEvent MouseEvent "
    "MutationObserver PerformanceObserver PointerEvent PopStateEvent "
    "ProgressEvent Request ResizeObserver StorageEvent TransitionEvent "
    "UIEvent URL WebSocket Worker',"
    "bad=[],want=(n)=>n+' argument'+(n>1?'s':'')+' required, but only '+"
    "(n-1)+' present.',"
    "check=(label,f,n,run)=>{if(typeof f!=='function'){bad.push(label+"
    "' missing');return}if(f.length!==n)bad.push(label+' length '+f.length);"
    "try{run(Array(n-1).fill(0));bad.push(label+' no throw')}catch(e){"
    "if(!(e instanceof TypeError)||!e.message.endsWith(want(n)))"
    "bad.push(label+' '+e)}};"
    "for(const key in R){const w=R[key].split(' ');"
    "for(let i=0;i<w.length;i+=2)check(key+'.'+w[i],O[key][w[i]],+w[i+1],"
    "(a)=>O[key][w[i]](...a))}"
    "for(const name of C.split(' '))check('new '+name,globalThis[name],1,"
    "(a)=>Reflect.construct(globalThis[name],a));"
    /* Chrome's text in full; a bare call names the Window; bound, applied
       and subclass constructor calls are checked too; an explicit
       undefined counts as an argument. */
    "const message=(f)=>{try{f()}catch(e){return e.message}return 'no throw'};"
    "if(message(()=>document.createElement())!==\"Failed to execute "
    "'createElement' on 'Document': 1 argument required, but only 0 "
    "present.\")bad.push('createElement message');"
    "if(message(()=>atob())!==\"Failed to execute 'atob' on 'Window': 1 "
    "argument required, but only 0 present.\")bad.push('atob message');"
    "if(message(()=>new URL())!==\"Failed to construct 'URL': 1 argument "
    "required, but only 0 present.\")bad.push('URL message');"
    "if(!message(()=>document.body.setAttribute.bind(document.body,'a')())"
    ".endsWith(want(2)))bad.push('bound');"
    "if(!message(()=>Reflect.apply(document.getElementById,document,[]))"
    ".endsWith(want(1)))bad.push('apply');"
    "if(!message(()=>new (class extends Event{})()).endsWith(want(1)))"
    "bad.push('subclass');"
    "if(document.getElementById(undefined)!==null||"
    "typeof setTimeout(()=>{})!=='number')bad.push('undefined argument');"
    "globalThis.pocSummary=bad.length?'FAIL '+bad.join(' | '):"
    "'REQUIRED-ARGUMENTS-OK';";

static int test_required_arguments(void)
{
    puts("test: Web IDL required arguments");
    CHECK(run_probe("required-arguments", required_arguments_probe,
                    "REQUIRED-ARGUMENTS-OK"));
    return 0;
}

/* insertAdjacentHTML() and atob() with no arguments threw a SyntaxError
   and an InvalidCharacterError DOMException; browsers throw TypeError. A
   bad position and a bad encoding keep their DOMExceptions. */
static const char missing_argument_types_probe[] =
    PROBE_PRELUDE
    "const b=document.body;"
    "t('insertAdjacentHTML()',()=>throwsName(()=>b.insertAdjacentHTML(),"
    "'TypeError'));"
    "t('insertAdjacentHTML(a)',()=>throwsName(()=>"
    "b.insertAdjacentHTML('beforeend'),'TypeError'));"
    "t('insertAdjacentHTML position',()=>{try{b.insertAdjacentHTML('x','')}"
    "catch(e){return e instanceof DOMException&&e.name==='SyntaxError'}});"
    "t('atob()',()=>throwsName(()=>atob(),'TypeError'));"
    "t('atob encoding',()=>{try{atob('%')}catch(e){return e instanceof "
    "DOMException&&e.name==='InvalidCharacterError'}});"
    PROBE_SUMMARY("MISSING-ARGUMENT-TYPES-OK");

static int test_missing_argument_types(void)
{
    puts("test: missing arguments throw TypeError");
    CHECK(run_probe("missing-argument-types", missing_argument_types_probe,
                    "MISSING-ARGUMENT-TYPES-OK"));
    return 0;
}

/* ---- Web IDL promise operations ----

   A promise-returning operation reports a wrong receiver or a missing
   argument as a rejected promise, never a throw, with the TypeError a
   synchronous operation would throw; structuredClone() and sendBeacon()
   return no promise and keep throwing. Each row is [label, call, outcome,
   error name, message]: outcome 'reject', 'throw' or 'resolve' (the
   message then is the value expected, when given). U is a call with an
   undefined receiver, P one on the interface prototype itself, and a
   bare name a call with too few arguments. */
static const char promise_operations_probe[] =
    "const P=(C,m)=>()=>C.prototype[m].call(C.prototype),"
    "U=(C,m)=>()=>C.prototype[m].call(undefined),"
    "II=['TypeError','Illegal invocation'],"
    "need=(f,i,n)=>['TypeError',\"Failed to execute '\"+f+\"' on '\"+i+"
    "\"': \"+n+' argument'+(n>1?'s':'')+' required, but only 0 present.'],"
    /* before any other lazy feature loads: the SubtleCrypto operations
       get their checks when crypto.subtle is first read */
    "subtle=crypto.subtle,early=subtle.importKey(),"
    "rs=new ReadableStream(),"
    "S={decrypt:3,deriveBits:2,deriveKey:5,digest:2,encrypt:3,exportKey:2,"
    "generateKey:3,importKey:5,sign:3,unwrapKey:7,verify:4,wrapKey:4},"
    "rows=["
    "['subtle.importKey() first',()=>early,'reject',"
    "...need('importKey','SubtleCrypto',5)],"
    "['ReadableStream.cancel U',U(ReadableStream,'cancel'),'reject',...II],"
    "['ReadableStream.cancel P',P(ReadableStream,'cancel'),'reject',...II],"
    "['WritableStream.abort U',U(WritableStream,'abort'),'reject',...II],"
    "['WritableStream.abort P',P(WritableStream,'abort'),'reject',...II],"
    "['WritableStream.close U',U(WritableStream,'close'),'reject',...II],"
    "['WritableStream.close P',P(WritableStream,'close'),'reject',...II],"
    "['Blob.text U',U(Blob,'text'),'reject',...II],"
    "['Blob.text P',P(Blob,'text'),'reject',...II],"
    "['Blob.arrayBuffer U',U(Blob,'arrayBuffer'),'reject',...II],"
    "['Blob.arrayBuffer P',P(Blob,'arrayBuffer'),'reject',...II],"
    "['Response.json U',U(Response,'json'),'reject',...II],"
    "['Response.json P',P(Response,'json'),'reject',...II],"
    "['Response.json {}',()=>Response.prototype.json.call({}),'reject',...II],"
    "['AudioContext.resume U',U(AudioContext,'resume'),'reject',...II],"
    "['AudioContext.resume P',P(AudioContext,'resume'),'reject',...II],"
    "['play U',U(HTMLMediaElement,'play'),'reject',...II],"
    "['play P',P(HTMLMediaElement,'play'),'reject',...II],"
    "['requestFullscreen U',U(Element,'requestFullscreen'),'reject',...II],"
    "['decode U',U(HTMLImageElement,'decode'),'reject',...II],"
    "['exitFullscreen U',U(Document,'exitFullscreen'),'reject',...II],"
    "['exitFullscreen',()=>document.exitFullscreen(),'reject','TypeError',"
    "'Document not active'],"
    "['databases U',()=>indexedDB.databases.call(undefined),'reject',...II],"
    "['readText U',()=>Clipboard.prototype.readText.call(undefined),"
    "'reject',...II],"
    "['fonts.load()',()=>document.fonts.load(),'reject',"
    "...need('load','FontFaceSet',1)],"
    "['pipeTo U',U(ReadableStream,'pipeTo'),'reject',...II],"
    "['pipeTo()',()=>rs.pipeTo(),'reject',...need('pipeTo','ReadableStream',1)],"
    "['pipeTo({})',()=>rs.pipeTo({}),'reject','TypeError',\"Failed to execute "
    "'pipeTo' on 'ReadableStream': parameter 1 is not of type "
    "'WritableStream'.\"],"
    "['fetch()',()=>fetch(),'reject',...need('fetch','Window',1)],"
    "['fetch U',()=>fetch.call(undefined),'reject',...need('fetch','Window',1)],"
    "['fetch(bad URL)',()=>fetch('http://['),'reject','TypeError',"
    "\"Failed to execute 'fetch' on 'Window': Failed to parse URL from "
    "http://[\"],"
    "['new Request(bad URL)',()=>new Request('http://['),'throw','TypeError',"
    "\"Failed to construct 'Request': Failed to parse URL from http://[\"],"
    "['createImageBitmap()',()=>createImageBitmap(),'reject',"
    "...need('createImageBitmap','Window',1)],"
    "['whenDefined()',()=>customElements.whenDefined(),'reject',"
    "...need('whenDefined','CustomElementRegistry',1)],"
    "['writeText()',()=>navigator.clipboard.writeText(),'reject',"
    "...need('writeText','Clipboard',1)],"
    "['getHighEntropyValues()',()=>navigator.userAgentData"
    ".getHighEntropyValues(),'reject',"
    "...need('getHighEntropyValues','NavigatorUAData',1)],"
    "['permissions.query()',()=>navigator.permissions.query(),'reject',"
    "...need('query','Permissions',1)],"
    "['structuredClone()',()=>structuredClone(),'throw',"
    "...need('structuredClone','Window',1)],"
    "['sendBeacon()',()=>navigator.sendBeacon(),'throw',"
    "...need('sendBeacon','Navigator',1)],"
    "...Object.keys(S).map(k=>['subtle.'+k+'()',()=>subtle[k](),'reject',"
    "...need(k,'SubtleCrypto',S[k])]),"
    /* the operations still work when called as they should be */
    "['cancel',()=>new ReadableStream().cancel(),'resolve'],"
    "['abort',()=>new WritableStream().abort(),'resolve'],"
    "['Blob.text',()=>new Blob(['a']).text(),'resolve','','a'],"
    "['Response.json',()=>new Response('[1]').json().then(v=>v[0]),"
    "'resolve','',1],"
    "['fonts.load',()=>document.fonts.load('10px serif').then(v=>v.length),"
    "'resolve','',0],"
    "['databases',()=>indexedDB.databases().then(v=>v.length),'resolve','',0],"
    "['writeText',()=>navigator.clipboard.writeText('a'),'resolve'],"
    "['digest',()=>subtle.digest('SHA-256',new Uint8Array(1))"
    ".then(v=>v.byteLength),'resolve','',32],"
    "['webkitExitFullscreen',()=>Promise.resolve("
    "document.webkitExitFullscreen()),'resolve','',undefined]];"
    "const bad=[],pending=new Set();"
    "for(const [label,f,kind,name,message] of rows){let r;"
    "try{r=f()}catch(e){if(kind!=='throw')bad.push(label+' threw '+e);"
    "else if(e.name!==name||e.message!==message)bad.push(label+' '+e);"
    "continue}"
    "if(kind==='throw'){bad.push(label+' no throw');continue}"
    "if(!r||typeof r.then!=='function'){bad.push(label+' no promise');"
    "continue}"
    "pending.add(label);"
    "r.then(v=>{pending.delete(label);if(kind==='reject')"
    "bad.push(label+' resolved');else if(message!==undefined&&v!==message)"
    "bad.push(label+' value '+v)},"
    "e=>{pending.delete(label);if(kind!=='reject')bad.push(label+' rejected '+e);"
    "else if(e.name!==name||e.message!==message)bad.push(label+' '+e.name+"
    "': '+e.message)})}"
    "if(document.fonts.load.length!==1||fetch.length!==1)bad.push('length');"
    "if(!(customElements.whenDefined('x-y') instanceof Promise))"
    "bad.push('whenDefined');"
    /* every row settles in a task or two; one still open is reported */
    "const finish=(n)=>setTimeout(()=>{if(n&&pending.size)return finish(n-1);"
    "for(const label of pending)bad.push(label+' pending');"
    "globalThis.pocSummary=bad.length?'FAIL '+bad.join(' | '):"
    "'PROMISE-OPERATIONS-OK'},0);finish(8);";

static int test_promise_operations(void)
{
    puts("test: Web IDL promise operations reject");
    CHECK(run_probe("promise-operations", promise_operations_probe,
                    "PROMISE-OPERATIONS-OK"));
    return 0;
}

/* ---- Web IDL receiver interfaces ----

   An interface member accepts the objects of its own interface only: a
   wrong browser object fails as a plain object does ("Illegal invocation";
   a promise operation rejects with it), while instances of subclasses
   (custom elements, a page's EventTarget, Blob or Response class) and a
   frame's objects, which share the page's interfaces, pass, as does a
   member the receiver's own chain holds (a text node's native layer holds
   Element.prototype.remove). Each row is
   [label, call, outcome, value]: outcome 'throw' or 'reject' (with Illegal
   invocation), 'type' (any TypeError thrown: an undefined receiver is
   left to the member), 'ok' or 'resolve' (value, when given, is the
   result expected). */
static const char receiver_interfaces_probe[] =
    "const g=(C,k)=>Object.getOwnPropertyDescriptor(C.prototype,k).get,"
    "div=()=>document.createElement('div'),"
    "frame=document.getElementById('f'),fd=frame.contentDocument,"
    "fw=frame.contentWindow;"
    "class SubBlob extends Blob{};class SubResponse extends Response{};"
    "class SubTarget extends EventTarget{};"
    "class XEl extends HTMLElement{};customElements.define('x-el',XEl);"
    "const rows=["
    /* promise operations */
    "['databases on document',()=>indexedDB.databases.call(document),"
    "'reject'],"
    "['databases on {}',()=>IDBFactory.prototype.databases.call({}),'reject'],"
    "['databases on undefined',()=>indexedDB.databases.call(undefined),"
    "'reject'],"
    "['Blob.text on Response',()=>Blob.prototype.text.call(new Response('x')),"
    "'reject'],"
    "['Blob.arrayBuffer on Request',()=>Blob.prototype.arrayBuffer.call("
    "new Request('https://conformance.test/')),'reject'],"
    "['Response.json on Blob',()=>Response.prototype.json.call("
    "new Blob(['1'])),'reject'],"
    "['Response.text on Request',()=>Response.prototype.text.call("
    "new Request('https://conformance.test/')),'reject'],"
    "['play on div',()=>HTMLMediaElement.prototype.play.call(div()),"
    "'reject'],"
    "['decode on div',()=>HTMLImageElement.prototype.decode.call(div()),"
    "'reject'],"
    "['cancel on WritableStream',()=>ReadableStream.prototype.cancel.call("
    "new WritableStream()),'reject'],"
    /* synchronous operations and attributes */
    "['appendChild on window',()=>Node.prototype.appendChild.call(window,"
    "div()),'throw'],"
    "['parentNode on window',()=>g(Node,'parentNode').call(window),'throw'],"
    "['parentNode on {}',()=>g(Node,'parentNode').call({}),'throw'],"
    "['getAttribute on document',()=>Element.prototype.getAttribute.call("
    "document,'id'),'throw'],"
    "['getAttribute on text',()=>Element.prototype.getAttribute.call("
    "document.createTextNode('t'),'id'),'throw'],"
    "['getAttribute on undefined',()=>Element.prototype.getAttribute.call("
    "undefined,'id'),'type'],"
    "['createElement on div',()=>Document.prototype.createElement.call(div(),"
    "'b'),'throw'],"
    "['Blob size on Response',()=>g(Blob,'size').call(new Response('x')),"
    "'throw'],"
    "['addEventListener on Blob',()=>EventTarget.prototype.addEventListener"
    ".call(new Blob([]),'x',()=>{}),'throw'],"
    /* the right interface, a subclass, or a frame's object */
    "['contains on body',()=>Node.prototype.contains.call(document.body,"
    "document.getElementById('p')),'ok',true],"
    "['parentNode on body',()=>g(Node,'parentNode').call(document.body)"
    "===document.documentElement,'ok',true],"
    "['addEventListener on window',()=>EventTarget.prototype"
    ".addEventListener.call(window,'x',()=>{}),'ok'],"
    "['getAttribute on svg',()=>{const s=document.createElementNS("
    "'http://www.w3.org/2000/svg','svg');s.setAttribute('id','s');"
    "return Element.prototype.getAttribute.call(s,'id')},'ok','s'],"
    "['custom element',()=>{const x=new XEl();x.setAttribute('id','x');"
    "document.body.appendChild(x);return Element.prototype.getAttribute"
    ".call(x,'id')+g(Node,'parentNode').call(x).tagName},'ok','xBODY'],"
    "['EventTarget subclass',()=>{const e=new SubTarget();let n=0;"
    "EventTarget.prototype.addEventListener.call(e,'x',()=>n++);"
    "e.dispatchEvent(new Event('x'));return n},'ok',1],"
    "['File as Blob',()=>Blob.prototype.text.call(new File(['cd'],'f')),"
    "'resolve','cd'],"
    "['Blob subclass',()=>new SubBlob(['ab']).text(),'resolve','ab'],"
    "['Blob subclass size',()=>g(Blob,'size').call(new SubBlob(['abc'])),"
    "'ok',3],"
    "['Response subclass',()=>new SubResponse('[2]').json().then(v=>v[0]),"
    "'resolve',2],"
    "['text remove',()=>{const t=document.createTextNode('t');"
    "document.body.appendChild(t);t.remove();return t.parentNode},'ok',null],"
    "['upload handler',()=>{const u=new XMLHttpRequest().upload,f=()=>{};"
    "u.onprogress=f;return u.onprogress===f},'ok',true],"
    "['remove on Blob',()=>Element.prototype.remove.call(new Blob([])),"
    "'throw'],"
    "['frame interfaces',()=>fw.Node===Node&&fw.Blob===Blob,'ok',true],"
    "['frame createElement',()=>Document.prototype.createElement.call(fd,"
    "'b').tagName,'ok','B'],"
    "['frame body',()=>Element.prototype.getAttribute.call(fd.body,'id')+"
    "g(Node,'firstChild').call(fd.body).tagName,'ok','nullP'],"
    "['frame listener',()=>EventTarget.prototype.addEventListener.call(fd,"
    "'x',()=>{}),'ok'],"
    "['frame Blob',()=>Blob.prototype.text.call(new fw.Blob(['fb'])),"
    "'resolve','fb']];"
    "const bad=[],pending=new Set();"
    "for(const [label,f,kind,value] of rows){let r;"
    "try{r=f()}catch(e){if(kind==='type'?e.name!=='TypeError':"
    "kind!=='throw'||e.name!=='TypeError'||e.message!=='Illegal invocation')"
    "bad.push(label+' threw '+e);continue}"
    "if(kind==='throw'||kind==='type'){bad.push(label+' no throw');continue}"
    "if(kind==='ok'){if(value!==undefined&&r!==value)"
    "bad.push(label+' value '+r);continue}"
    "if(!r||typeof r.then!=='function'){bad.push(label+' no promise');"
    "continue}"
    "pending.add(label);"
    "r.then(v=>{pending.delete(label);if(kind==='reject')"
    "bad.push(label+' resolved');else if(value!==undefined&&v!==value)"
    "bad.push(label+' value '+v)},"
    "e=>{pending.delete(label);if(kind!=='reject')bad.push(label+' rejected '"
    "+e);else if(e.name!=='TypeError'||e.message!=='Illegal invocation')"
    "bad.push(label+' '+e.name+': '+e.message)})}"
    "const finish=(n)=>setTimeout(()=>{if(n&&pending.size)return finish(n-1);"
    "for(const label of pending)bad.push(label+' pending');"
    "globalThis.pocSummary=bad.length?'FAIL '+bad.join(' | '):"
    "'RECEIVER-INTERFACES-OK'},0);finish(8);";

static int test_receiver_interfaces(void)
{
    puts("test: Web IDL members reject another interface's objects");
    CHECK(run_probe_body("receiver-interfaces",
                         "<p id=p>text</p><iframe id=f srcdoc='<p>y</p>'>"
                         "</iframe>", receiver_interfaces_probe,
                         "RECEIVER-INTERFACES-OK"));
    return 0;
}

/* ---- Web IDL receivers, call by call ----

   A member a native layer holds (a text node's layer holds Element's
   remove, tagName and the rest, a meta element's HTMLTemplateElement's
   content) accepts that layer's nodes call by call. One such call widened
   the member for the rest of the page, so Element.prototype.remove then
   accepted any object inheriting from Node.prototype, and copying a member
   onto a plain object's prototype passed and widened it as well
   (XMLHttpRequest upload handlers then accepted any EventTarget). Each
   case calls the member on objects of the wrong interface, on a valid
   unusual receiver, on the wrong objects again, through a page's copy of
   it on a plain prototype, and on the wrong objects a last time: every
   call but the valid one throws "Illegal invocation". */
static const char receiver_per_call_probe[] =
    "const out=f=>{try{f();return 'ok'}catch(e){return e instanceof "
    "TypeError&&e.message==='Illegal invocation'?'i':'E'}},"
    "d=(o,k)=>Object.getOwnPropertyDescriptor(o,k),"
    "node=()=>Object.create(Node.prototype),"
    "text=()=>document.body.appendChild(document.createTextNode('t')),"
    "frag=()=>{const f=document.createDocumentFragment();"
    "f.appendChild(document.createElement('i'));return f},"
    "meta=()=>document.querySelector('meta'),"
    "call=(desc,kind,r)=>kind==='get'?desc.get.call(r):kind==='set'?"
    "desc.set.call(r,null):desc.value.call(r),"
    "use=(o,key,kind)=>kind==='get'?o[key]:kind==='set'?(o[key]=null):"
    "o[key](),cases=[],bad=[];"
    "for(const [key,kind,valid] of [['remove','value',text],"
    "['before','value',text],['tagName','get',text],"
    "['attributes','get',text],['id','get',text],['localName','get',text],"
    "['lastElementChild','get',text],['children','get',frag],"
    "['firstElementChild','get',frag]])"
    "cases.push([key+' '+kind,d(Element.prototype,key),kind,"
    "[node,()=>document],valid,Node.prototype,key]);"
    "for(const kind of ['get','set'])cases.push(['content '+kind,"
    "d(Object.getPrototypeOf(meta()),'content'),kind,"
    "[()=>Object.create(HTMLElement.prototype),"
    "()=>document.createElement('div')],meta,HTMLElement.prototype,"
    "'content']);"
    "for(const type of ['loadstart','progress','abort','error','load',"
    "'timeout','loadend'])for(const kind of ['get','set'])"
    "cases.push(['on'+type+' '+kind,"
    "d(XMLHttpRequestEventTarget.prototype,'on'+type),kind,"
    "[()=>Object.create(EventTarget.prototype),"
    "()=>new AbortController().signal],"
    "()=>new XMLHttpRequest().upload,EventTarget.prototype,'on'+type]);"
    "for(const [label,desc,kind,invalid,valid,base,key] of cases){"
    "if(!desc){bad.push(label+' missing');continue}"
    "const wrong=()=>invalid.map(f=>out(()=>call(desc,kind,f()))).join(''),"
    "first=wrong(),v=out(()=>call(desc,kind,valid())),again=wrong(),"
    "copy=Object.create(base);Object.defineProperty(copy,key,desc);"
    "const forged=out(()=>use(Object.create(copy),key,kind)),last=wrong();"
    "if(v!=='ok'||forged!=='i'||(first+again+last).replace(/i/g,''))"
    "bad.push(label+': '+[first,v,again,forged,last].join('/'))}"
    "globalThis.pocSummary=bad.length?'FAIL '+bad.join(' | '):"
    "'RECEIVER-PER-CALL-OK';";

static int test_receiver_per_call(void)
{
    puts("test: an unusual valid receiver passes for its own call only");
    CHECK(run_probe_body("receiver-per-call",
                         "<p id=p>text</p><meta name=m content=c>",
                         receiver_per_call_probe, "RECEIVER-PER-CALL-OK"));
    return 0;
}

/* removeChild and replaceChild threw a plain Error for a non-node and
   for a node that is not a child; browsers throw TypeError and a
   NotFoundError DOMException. */
static const char remove_child_errors_probe[] =
    PROBE_PRELUDE
    "const b=document.body,p=document.getElementById('p'),"
    "other=document.createElement('i'),"
    "notFound=(f)=>{try{f()}catch(e){return e instanceof DOMException&&"
    "e.name==='NotFoundError'}return false};"
    "t('removeChild()',()=>throwsName(()=>b.removeChild(),'TypeError'));"
    "t('removeChild(null)',()=>throwsName(()=>b.removeChild(null),"
    "'TypeError'));"
    "t('removeChild({})',()=>throwsName(()=>b.removeChild({}),'TypeError'));"
    "t('removeChild(other)',()=>notFound(()=>b.removeChild(other)));"
    "t('replaceChild(null,p)',()=>throwsName(()=>b.replaceChild(null,p),"
    "'TypeError'));"
    "t('replaceChild(other,{})',()=>throwsName(()=>b.replaceChild(other,{}),"
    "'TypeError'));"
    "t('replaceChild(other,other)',()=>notFound(()=>"
    "b.replaceChild(other,document.createElement('b'))));"
    "t('removeChild(p)',()=>b.removeChild(p)===p&&!p.parentNode);"
    PROBE_SUMMARY("REMOVE-CHILD-ERRORS-OK");

static int test_remove_child_errors(void)
{
    puts("test: removeChild and replaceChild error types");
    CHECK(run_probe("remove-child-errors", remove_child_errors_probe,
                    "REMOVE-CHILD-ERRORS-OK"));
    return 0;
}

/* crypto.getRandomValues threw a TypeMismatchError DOMException for
   anything that is not an integer typed array; Web IDL makes a value
   that is not an ArrayBufferView a TypeError. A float array or a
   DataView keeps the TypeMismatchError. */
static const char random_values_errors_probe[] =
    PROBE_PRELUDE
    "const mismatch=(f)=>{try{f()}catch(e){return e instanceof DOMException"
    "&&e.name==='TypeMismatchError'}return false};"
    "t('getRandomValues()',()=>throwsName(()=>crypto.getRandomValues(),"
    "'TypeError'));"
    "t('getRandomValues(1)',()=>throwsName(()=>crypto.getRandomValues(1),"
    "'TypeError'));"
    "t('getRandomValues({})',()=>throwsName(()=>crypto.getRandomValues({}),"
    "'TypeError'));"
    "t('getRandomValues(buffer)',()=>throwsName(()=>"
    "crypto.getRandomValues(new ArrayBuffer(4)),'TypeError'));"
    "t('getRandomValues(float)',()=>mismatch(()=>"
    "crypto.getRandomValues(new Float32Array(1))));"
    "t('getRandomValues(view)',()=>mismatch(()=>"
    "crypto.getRandomValues(new DataView(new ArrayBuffer(4)))));"
    "t('getRandomValues(bytes)',()=>"
    "crypto.getRandomValues(new Uint8Array(4)).length===4);"
    PROBE_SUMMARY("RANDOM-VALUES-ERRORS-OK");

static int test_random_values_errors(void)
{
    puts("test: getRandomValues error types");
    CHECK(run_probe("random-values-errors", random_values_errors_probe,
                    "RANDOM-VALUES-ERRORS-OK"));
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--fetch-api-only"))
        return test_fetch_api_edges() != 0;
    if (argc == 2 && !strcmp(argv[1], "--receivers-only"))
        return (test_receiver_interfaces() | test_receiver_per_call()) != 0;
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
    failures += test_fetch_api_edges();
    failures += test_live_ranges();
    failures += test_document_markup();
    failures += test_namespaced_attributes();
    failures += test_get_html();
    failures += test_console_untouched();
    failures += test_no_touch_handlers();
    failures += test_media_features();
    failures += test_label_descendant();
    failures += test_rejection_events();
    failures += test_rejection_events_after_full_queue();
    failures += test_window_handlers();
    failures += test_worker_references();
    failures += test_post_message_arity();
    failures += test_required_arguments();
    failures += test_missing_argument_types();
    failures += test_promise_operations();
    failures += test_receiver_interfaces();
    failures += test_receiver_per_call();
    failures += test_remove_child_errors();
    failures += test_random_values_errors();
    failures += test_builtin_names();
    if (failures != 0) return 1;
    puts("web platform conformance tests passed");
    return 0;
}
