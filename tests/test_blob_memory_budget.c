/* Blob and blob: URL bytes are page memory.
 *
 * A Blob's bytes (and a File's, which is a Blob) live in an ArrayBuffer of
 * the page realm, so QuickJS allocates them from the realm's allocator pool,
 * which charges the page Budget, and the realm's heap limit bounds them.
 * URL.createObjectURL() keeps a reference to the Blob, not a copy. This test
 * makes large blobs on the device-sized realm and checks that the page
 * Budget and the realm heap both grow by their size, that a page cannot make
 * more than its heap holds, that dropping them gives the memory back, and
 * that blobs and blob: URLs have no count or size cap of their own: the
 * realm heap refuses them, revoking a URL frees the Blob it alone held, and
 * URLs a page never revokes go with its realm on navigation. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define KIB 1024u

/* The device realm and a page Budget with room beyond it. */
#define REALM_BYTES (5u * MIB)
#define PAGE_BUDGET_BYTES (24u * MIB)
#define BLOB_BYTES (400u * KIB)
#define BLOB_COUNT 6

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static const char page_html[] =
    "<!doctype html><title>Blobs</title><body><p>blobs</p></body>";

typedef struct {
    Budget budget;
    PocDocument document;
    ScriptRuntime *runtime;
} BlobPage;

static bool page_open_realm(BlobPage *page, size_t realm_bytes)
{
    memset(page, 0, sizeof(*page));
    budget_init(&page->budget, PAGE_BUDGET_BYTES);
    CHECK(budget_install_lexbor(&page->budget));
    CHECK(document_parse(&page->document, &page->budget, page_html,
                         sizeof(page_html) - 1u, 512u));
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ScriptExecutionPolicy policy;
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &policy));
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .omit_test_probes = true,
    };
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    page->runtime = script_runtime_create_configured(
        &page->document, &page->budget, realm_bytes, 8000,
        "https://blobs.test/", &options, &result);
    CHECK(page->runtime != NULL && result.success);
    return true;
}

static bool page_open(BlobPage *page)
{
    return page_open_realm(page, REALM_BYTES);
}

static bool page_close(BlobPage *page)
{
    script_runtime_destroy(page->runtime);
    document_destroy(&page->document);
    CHECK(budget_uninstall_lexbor(&page->budget));
    CHECK(page->budget.current == 0);
    return true;
}

static bool evaluate(BlobPage *page, const char *source, const char *expected)
{
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    CHECK(script_runtime_evaluate_diagnostic(page->runtime, source,
                                             "<blob-budget>", &result));
    if (strcmp(result.summary, expected) != 0) {
        fprintf(stderr, "summary %s, expected %s\n", result.summary, expected);
        return false;
    }
    return true;
}

static bool test_blob_type_normalization(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "const blob=(type)=>new Blob(['x'],{type});"
        "const invalid=['\\u0000text/plain','text/\\tplain','text/plain\\n',"
        "'text/plain\\u007f','text/\\u212a','text/plain\\u00e9','\\ud800'];"
        "const request=(body)=>new Request('/upload',{method:'POST',body});"
        "const r=[blob(' TEXT/PLAIN; CHARSET=UTF-8~ ').type,"
        "blob(null).type,blob(false).type,blob(0).type,blob(undefined).type,"
        "new File(['x'],'f',{type:'TEXT/HTML'}).type,"
        "new File(['x'],'f',{type:'text/\\tplain'}).type,"
        "new File(['x'],'f',{type:false}).type,"
        "blob('text/plain').slice(0,1,'TEXT/HTML').type,"
        "blob('text/plain').slice(0,1,'text/\\u212a').type,"
        "request(blob('TEXT/PLAIN')).headers.get('content-type'),"
        "request(blob('text/\\tplain')).headers.has('content-type'),"
        "invalid.every(type=>blob(type).type==='')];"
        "try{blob(Symbol());r.push('no error')}catch(e){r.push(e.name)}"
        "globalThis.pocSummary=r.join('|');",
        " text/plain; charset=utf-8~ |null|false|0||text/html||false|"
        "text/html||text/plain|false|true|TypeError"));
    CHECK(evaluate(&page, "pocSummary=new Blob([],null).type===''?'empty':'bad';",
                   "empty"));
    return page_close(&page);
}

static bool test_blob_string_parts_and_live_views(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "{let conversions=0;const input=new Uint8Array([9,10,11]);"
        "const view=new DataView(input.buffer,1,1);"
        "const text={toString(){conversions++;input[1]=65;"
        "return 'a'.repeat(4095)+'\\ud83d\\ude00\\ud800\\u00e9'}};"
        "const blob=new Blob([view,text,new Blob(['z'])]);"
        "input[1]=66;"
        "blob.bytes().then(b=>pocSummary=conversions===1&&b.length===4106&&"
        "b[0]===65&&b[4095]===97&&"
        "b.slice(4096).join(',')==='240,159,152,128,239,191,189,195,169,122'"
        "?'mixed':'bad:'+conversions+':'+b.length);}",
        "mixed"));
    return page_close(&page);
}

/* Blobs and Files a page holds (with and without blob: URLs) are charged to
   the page Budget and the realm heap, byte for byte, and handed back when
   the page drops them. */
static bool test_held_blobs_are_charged(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t budget_before = page.budget.current;
    size_t heap_before = script_runtime_heap_used(page.runtime);
    char source[512];
    snprintf(source, sizeof(source),
             /* A block, so the source buffer is not a global binding. */
             "{const source=new Uint8Array(%u);"
             "globalThis.held=[];"
             "for(let i=0;i<%d;i++)held.push(i%%2?new File([source],'f'+i)"
             ":new Blob([source]));"
             "globalThis.heldURL=URL.createObjectURL(held[0]);"
             "globalThis.pocSummary=held.reduce((n,b)=>n+b.size,0)+':'"
             "+heldURL.startsWith('blob:https://blobs.test/')}",
             BLOB_BYTES, BLOB_COUNT);
    char expected[64];
    snprintf(expected, sizeof(expected), "%u:true",
             BLOB_BYTES * (unsigned) BLOB_COUNT);
    CHECK(evaluate(&page, source, expected));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t budget_held = page.budget.current;
    size_t heap_held = script_runtime_heap_used(page.runtime);
    size_t blobs = (size_t) BLOB_BYTES * BLOB_COUNT;
    printf("blob-budget: %d blobs of %u bytes: budget +%zu heap +%zu\n",
           BLOB_COUNT, BLOB_BYTES, budget_held - budget_before,
           heap_held - heap_before);
    CHECK(budget_held >= budget_before + blobs);
    CHECK(heap_held >= heap_before + blobs);
    /* Charged once: the blob: URL holds the Blob, not a copy of its bytes. */
    CHECK(heap_held <= heap_before + blobs + 64u * KIB);

    CHECK(evaluate(&page,
                   "URL.revokeObjectURL(heldURL);globalThis.held=null;"
                   "globalThis.pocSummary='dropped'", "dropped"));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t budget_after = page.budget.current;
    size_t heap_after = script_runtime_heap_used(page.runtime);
    printf("blob-budget: after dropping: budget %+lld heap %+lld vs before\n",
           (long long) budget_after - (long long) budget_before,
           (long long) heap_after - (long long) heap_before);
    CHECK(heap_after <= heap_before + 64u * KIB);
    CHECK(budget_after <= budget_before + 64u * KIB);
    return page_close(&page);
}

/* A page that keeps making blobs stops at its realm heap instead of growing
   past it: the Blob that does not fit is refused with a RangeError before
   anything is allocated, so the allocator refuses nothing. */
static bool test_blobs_stop_at_the_realm_heap(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    size_t heap_free = script_runtime_heap_available(page.runtime);
    size_t refusals = script_runtime_allocation_refusals(page.runtime);
    char source[512];
    snprintf(source, sizeof(source),
             "const source=new Uint8Array(%u);"
             "globalThis.held=[];let failure='none';"
             "try{for(let i=0;i<64;i++)held.push(new Blob([source]));}"
             "catch(error){failure=error instanceof RangeError?'range':"
             "String(error&&error.message).includes('memory')?'memory':"
             "'other';}"
             "const count=held.length;globalThis.held=null;"
             "globalThis.pocSummary=count+':'+failure",
             BLOB_BYTES);
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    CHECK(script_runtime_evaluate_diagnostic(page.runtime, source,
                                             "<blob-budget-exhaust>", &result));
    printf("blob-budget: free heap %zu, made %s (blobs:failure)\n",
           heap_free, result.summary);
    unsigned count = (unsigned) strtoul(result.summary, NULL, 10);
    const char *failure = strchr(result.summary, ':');
    CHECK(failure != NULL && strcmp(failure, ":range") == 0);
    CHECK(script_runtime_allocation_refusals(page.runtime) == refusals);
    /* 64 blobs would be 25 MiB; the page got no more than its heap holds. */
    CHECK(count > 0 && (size_t) count * BLOB_BYTES <= heap_free);
    CHECK(page.budget.current <= PAGE_BUDGET_BYTES);
    /* The realm still works after the refusal. */
    (void) script_runtime_collect_and_trim(page.runtime);
    CHECK(evaluate(&page, "globalThis.pocSummary=String(new Blob(['ok']).size)",
                   "2"));
    return page_close(&page);
}

/* Neither a Blob nor the blob: URL registry has a cap of its own: a Blob
   over 512 KiB (from bytes or text), two 400 KiB Blobs behind URLs, and a
   thousand URLs for one Blob are all made. */
static bool test_blobs_and_urls_have_no_cap_of_their_own(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "{const r=[];"
        "const big=new Blob([new Uint8Array(512*1024+1)]);"
        "r.push(big.size===512*1024+1);"
        "const text=new Blob(['x'.repeat(300*1024),'\\u20ac'.repeat(100*1024)]);"
        "r.push(text.size===600*1024);"
        "const a=new Blob([new Uint8Array(400*1024)]),"
        "b=new Blob([new Uint8Array(400*1024)]);"
        "const held=[URL.createObjectURL(a),URL.createObjectURL(b),"
        "URL.createObjectURL(big),URL.createObjectURL(text)];"
        "r.push(new Set(held).size===4);"
        "const small=new Blob(['x']),urls=new Set();"
        "for(let i=0;i<1000;i++)urls.add(URL.createObjectURL(small));"
        "r.push(urls.size===1000);"
        "for(const u of held)URL.revokeObjectURL(u);"
        "for(const u of urls)URL.revokeObjectURL(u);"
        "globalThis.pocSummary=r.join(',')}",
        "true,true,true,true"));
    return page_close(&page);
}

/* A blob: URL keeps its Blob alive: Blobs reachable only through URLs stay
   charged, and revoking the URLs returns their bytes to the realm heap and
   the page Budget. */
static bool test_revoking_returns_url_held_bytes(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page, "globalThis.urls=[];globalThis.pocSummary='ready'",
                   "ready"));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t budget_before = page.budget.current;
    size_t heap_before = script_runtime_heap_used(page.runtime);
    char source[256];
    snprintf(source, sizeof(source),
             "for(let i=0;i<%d;i++)"
             "urls.push(URL.createObjectURL(new Blob([new Uint8Array(%u)])));"
             "globalThis.pocSummary=String(urls.length)",
             BLOB_COUNT - 2, BLOB_BYTES);
    char expected[16];
    snprintf(expected, sizeof(expected), "%d", BLOB_COUNT - 2);
    CHECK(evaluate(&page, source, expected));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t blobs = (size_t) BLOB_BYTES * (BLOB_COUNT - 2);
    size_t heap_held = script_runtime_heap_used(page.runtime);
    printf("blob-budget: %d URL-held blobs: budget +%zu heap +%zu\n",
           BLOB_COUNT - 2, page.budget.current - budget_before,
           heap_held - heap_before);
    CHECK(heap_held >= heap_before + blobs);
    CHECK(page.budget.current >= budget_before + blobs);

    CHECK(evaluate(&page,
                   "for(const u of urls)URL.revokeObjectURL(u);"
                   "globalThis.pocSummary=String(urls.length)", expected));
    (void) script_runtime_collect_and_trim(page.runtime);
    printf("blob-budget: after revoking: budget %+lld heap %+lld vs before\n",
           (long long) page.budget.current - (long long) budget_before,
           (long long) script_runtime_heap_used(page.runtime)
               - (long long) heap_before);
    CHECK(script_runtime_heap_used(page.runtime) <= heap_before + 64u * KIB);
    CHECK(page.budget.current <= budget_before + 64u * KIB);
    return page_close(&page);
}

/* Runs `loop` (which pushes to globalThis.urls until it throws) and checks
   that the realm heap, not a blob cap, refused it with `failure` ("range":
   a Blob that would not fit; "memory": the allocator), cleanly: the realm still
   works, and revoking every URL made gives the heap back, apart from
   `kept_per_url` bytes for each URL made. Writes how many were made. */
static bool exhaust_blob_urls(BlobPage *page, const char *loop,
                              const char *failure_expected,
                              size_t kept_per_url, unsigned *made)
{
    CHECK(evaluate(page, "globalThis.urls=[];globalThis.pocSummary='ready'",
                   "ready"));
    (void) script_runtime_collect_and_trim(page->runtime);
    size_t heap_before = script_runtime_heap_used(page->runtime);
    size_t budget_before = page->budget.current;
    char source[640];
    /* `reserve` is dropped when the loop fails, so the check and the
       revoking script that follow have room to run however little the
       failed allocation left. */
    snprintf(source, sizeof(source),
             "{let failure='none',reserve=new ArrayBuffer(65536);"
             "try{%s}"
             /* QuickJS throws null when the heap cannot hold the
                out-of-memory error object itself. */
             "catch(error){reserve=null;"
             "failure=error instanceof RangeError?'range':"
             "error===null||String(error.message).includes('memory')?"
             "'memory':'other:'+String(error&&error.message);}"
             "globalThis.pocSummary=urls.length+':'+failure}", loop);
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    bool evaluated = script_runtime_evaluate_diagnostic(
        page->runtime, source, "<blob-url-exhaust>", &result);
    if (!evaluated)
        fprintf(stderr, "blob URL exhaustion: %s %s\n", result.summary,
                result.error);
    CHECK(evaluated);
    *made = (unsigned) strtoul(result.summary, NULL, 10);
    const char *failure = strchr(result.summary, ':');
    if (failure == NULL || strcmp(failure + 1, failure_expected) != 0)
        fprintf(stderr, "blob URL exhaustion: %s\n", result.summary);
    CHECK(failure != NULL && strcmp(failure + 1, failure_expected) == 0);
    CHECK(page->budget.current <= PAGE_BUDGET_BYTES);
    (void) script_runtime_collect_and_trim(page->runtime);
    CHECK(evaluate(page,
                   "for(const u of urls)URL.revokeObjectURL(u);"
                   "globalThis.urls=null;"
                   "globalThis.pocSummary=String(new Blob(['ok']).size)",
                   "2"));
    (void) script_runtime_collect_and_trim(page->runtime);
    printf("blob-budget: after revoking %u URLs: budget %+lld heap %+lld\n",
           *made,
           (long long) page->budget.current - (long long) budget_before,
           (long long) script_runtime_heap_used(page->runtime)
               - (long long) heap_before);
    size_t kept = 64u * KIB + kept_per_url * *made;
    CHECK(script_runtime_heap_used(page->runtime) <= heap_before + kept);
    CHECK(page->budget.current <= budget_before + kept);
    return true;
}

/* Large Blobs held only by blob: URLs stop at the realm heap (the next is
   refused up front); so do URL entries alone, for one small Blob, which
   the allocator refuses: the registry cannot grow past the heap. */
static bool test_blob_urls_stop_at_the_realm_heap(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    size_t heap_free = script_runtime_heap_available(page.runtime);
    char loop[160];
    snprintf(loop, sizeof(loop),
             "for(let i=0;i<64;i++)"
             "urls.push(URL.createObjectURL(new Blob([new Uint8Array(%u)])));",
             BLOB_BYTES);
    unsigned large = 0;
    CHECK(exhaust_blob_urls(&page, loop, "range", 0u, &large));
    printf("blob-budget: free heap %zu, %u URL-held blobs\n", heap_free, large);
    CHECK(large > 1u && (size_t) large * BLOB_BYTES <= heap_free);

    /* QuickJS never shrinks a Map's hash table (a pointer per bucket, at
       most a bucket per entry), so the registry keeps that after its first
       peak; the second peak reuses it, so nothing accumulates. */
    static const char many[] =
        "const small=new Blob(['x']);"
        "for(let i=0;i<1000000;i++)urls.push(URL.createObjectURL(small));";
    unsigned entries = 0, again = 0;
    CHECK(exhaust_blob_urls(&page, many, "memory", sizeof(void *), &entries));
    CHECK(exhaust_blob_urls(&page, many, "memory", 0u, &again));
    printf("blob-budget: %u, then %u blob: URLs for one Blob\n", entries,
           again);
    CHECK(entries > 1000u && entries < 1000000u);
    CHECK(again + entries / 64u >= entries && again <= entries + entries / 64u);
    return page_close(&page);
}

/* blob: URLs a page never revokes belong to its realm: navigating the tab
   retires them with it, so they cannot pile up across documents. */
static bool test_unrevoked_urls_go_with_the_document(void)
{
    static const char html[] =
        "<!doctype html><title>Blobs</title><body><p>blobs</p></body>";
    static const char script[] =
        "{const big=[];for(let i=0;i<3;i++)"
        "big.push(URL.createObjectURL(new Blob([new Uint8Array(400*1024)])));"
        "const small=new Blob(['x']);let n=big.length;"
        "for(let i=0;i<2000;i++,n++)URL.createObjectURL(small);"
        "globalThis.pocSummary=String(n)}";
    Budget budget;
    budget_init(&budget, PAGE_BUDGET_BYTES);
    CHECK(budget_install_lexbor(&budget));
    NavigationSession navigation;
    memset(&navigation, 0, sizeof(navigation));
    CHECK(navigation_init(&navigation, &budget, 4));
    navigation_enable_scripts(&navigation, REALM_BYTES, 8000);
    navigation_set_fixed_script_memory_limits(&navigation, true);
    size_t settled = 0;
    bool ok = true;
    for (unsigned visit = 0; ok && visit < 6u; visit++) {
        char url[64];
        snprintf(url, sizeof(url), "https://blobs.test/page%u", visit);
        uint64_t generation = navigation_begin(&navigation);
        ScriptResult result;
        memset(&result, 0, sizeof(result));
        ok = navigation_commit_html(&navigation, generation, url, html,
                                    sizeof(html) - 1u, 480, NULL, NULL, true)
            && navigation.page.runtime != NULL
            && script_runtime_evaluate_diagnostic(
                   navigation.page.runtime, script, "<blob-nav>", &result)
            && strcmp(result.summary, "2003") == 0
            && budget.current >= 3u * 400u * KIB;
        if (!ok) {
            fprintf(stderr, "visit %u: %s %s\n", visit, result.summary,
                    result.error);
            break;
        }
        (void) script_runtime_collect_and_trim(navigation.page.runtime);
        printf("blob-budget: visit %u holds %zu page bytes\n", visit,
               budget.current);
        /* The first visits settle session state (history, caches). */
        if (visit == 1u) settled = budget.current;
        if (visit > 1u) ok = budget.current <= settled + 128u * KIB;
    }
    navigation_destroy(&navigation);
    CHECK(ok);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    return true;
}

/* text() reads a Blob over TextDecoder's 1 MiB call bound, including a
   sequence split where it decodes in pieces, and returns a promise. */
static bool test_large_blob_reads_as_text(void)
{
    BlobPage page;
    CHECK(page_open_realm(&page, 10u * MIB));
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(page.runtime,
        "globalThis.pocSummary='pending';"
        "{const blob=new Blob(['a'.repeat(512*1024-1),'\\u00e9',"
        "'b'.repeat(600*1024)]),read=blob.text();"
        "if(!(read instanceof Promise))pocSummary='not a promise';"
        "else read.then(text=>pocSummary=text.length===1138688&&"
        "text[524287]==='\\u00e9'&&text[524286]==='a'&&text[524288]==='b'&&"
        "text.endsWith('bb')?'read':'wrong:'+text.length,"
        "e=>pocSummary='rejected:'+e.name)}",
        "<blob-text>", &result)
        || (fprintf(stderr, "large blob text: %s\n", result.error), false));
    for (unsigned turn = 0; turn < 8u
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    if (strcmp(result.summary, "read") != 0)
        fprintf(stderr, "large blob text: %s\n", result.summary);
    CHECK(strcmp(result.summary, "read") == 0);
    return page_close(&page);
}

/* The text readers must stream the same admitted Blob, preserve a code
   point split across decoder calls, and recognize only the initial BOM. */
static bool test_large_blob_file_readers(void)
{
    static const char make_blob[] =
        "function make(){const b=new Uint8Array(1100000);b.fill(65);"
        "b.set([239,187,191]);b.set([195,169],524287);"
        "b.set([239,187,191],524289);return new Blob([b])}"
        "function good(s){return s.length===1099994&&s[0]==='A'&&"
        "s[524284]==='\\u00e9'&&s[524285]==='\\ufeff'&&s.endsWith('AA')}"
        "function utf16(){const b=new Uint8Array(1100000);b.fill(65);"
        "b.set([255,254]);b.set([61,216,0,222,255,254],524286);"
        "return new Blob([b])}"
        "function good16(s){return s.length===549999&&s.charCodeAt(0)===16705&&"
        "s.codePointAt(262142)===0x1f600&&s.charCodeAt(262144)===0xfeff}";
    BlobPage page;
    CHECK(page_open_realm(&page, 14u * MIB));
    char source[2048];
    ScriptResult result = {0};
    snprintf(source, sizeof(source),
        "%s;pocSummary='pending';const r=new FileReader(),b=make(),events=[];"
        "for(const name of ['loadstart','progress','load','error','loadend'])"
        "r.addEventListener(name,e=>events.push(e.type));"
        "r.onloadend=()=>pocSummary=r.error?'read:'+r.error.name:"
        "good(r.result)&&events.join(',')==='loadstart,progress,load,loadend'"
        "?'async':'wrong';r.readAsText(b,'UTF-8');", make_blob);
    CHECK(script_runtime_evaluate_diagnostic(page.runtime, source,
                                             "<blob-async-reader>", &result));
    for (unsigned turn = 0; turn < 24u
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    bool async_ok = strcmp(result.summary, "async") == 0;
    if (!async_ok) fprintf(stderr, "large async reader: %s\n", result.summary);
    CHECK(page_close(&page));

    CHECK(page_open_realm(&page, 14u * MIB));
    memset(&result, 0, sizeof(result));
    snprintf(source, sizeof(source),
        "pocSummary='pending';const source=%c%s;"
        "try{const r=new FileReaderSync();"
        "postMessage(good(r.readAsText(make(),'UTF-8'))&&"
        "good16(r.readAsText(utf16(),'utf-16le'))"
        "?'sync':'wrong')}catch(e){postMessage('read:'+e.name)}%c;"
        "const w=new Worker(URL.createObjectURL(new Blob([source])));"
        "w.onmessage=e=>{pocSummary=e.data;w.terminate()};", '`', make_blob, '`');
    CHECK(script_runtime_evaluate_diagnostic(page.runtime, source,
                                             "<blob-sync-reader>", &result));
    for (unsigned turn = 0; turn < 24u
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    bool sync_ok = strcmp(result.summary, "sync") == 0;
    if (!sync_ok) fprintf(stderr, "large sync reader: %s\n", result.summary);
    CHECK(page_close(&page));
    return async_ok && sync_ok;
}

static bool test_blob_text_read_keeps_bytes_private(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(page.runtime,
        "pocSummary='pending';const b=new Blob([new Uint8Array([65,0])]),"
        "r=new FileReader(),subarray=Uint8Array.prototype.subarray;"
        "let captured;Uint8Array.prototype.subarray=function(...args){"
        "captured=this;return subarray.apply(this,args)};"
        "r.onloadend=()=>{Uint8Array.prototype.subarray=subarray;"
        "if(captured)captured[0]=90;"
        "b.bytes().then(bytes=>pocSummary="
        "r.result==='A'&&bytes[0]===65?'private':'wrong')};"
        "r.readAsText(b,'utf-16le');", "<blob-text-private>", &result));
    for (unsigned turn = 0; turn < 12u
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    CHECK(strcmp(result.summary, "private") == 0);
    return page_close(&page);
}

/* A page whose Blob the heap cannot hold: its scripts run as page tasks,
   where a refused allocation meets the realm memory stop rule. `ballast`
   bytes are taken first so the page starts `room` bytes from its limit
   (0: no ballast). Each of `tasks` timer tasks runs `task`, which sets
   globalThis.outcome; the realm must keep running and report `expected`. */
static bool page_blob_task(const char *label, size_t room, const char *task,
                           unsigned tasks, const char *expected)
{
    static const char html[] =
        "<!doctype html><title>Blobs</title><body><p>blobs</p>"
        "<script>globalThis.outcomes=[]</script></body>";
    Budget budget;
    budget_init(&budget, PAGE_BUDGET_BYTES);
    CHECK(budget_install_lexbor(&budget));
    NavigationSession navigation;
    memset(&navigation, 0, sizeof(navigation));
    CHECK(navigation_init(&navigation, &budget, 2));
    navigation_enable_scripts(&navigation, 8u * MIB, 8000);
    navigation_enable_document_scripts(&navigation, 4, 64u * KIB, 64u * KIB,
                                       8000);
    bool ok = navigation_commit_html(
        &navigation, navigation_begin(&navigation), "https://blobs.test/",
        html, sizeof(html) - 1u, 480, NULL, NULL, true)
        && navigation.page.runtime != NULL;
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    size_t obtainable = 0;
    if (ok) {
        (void) script_runtime_collect_and_trim(navigation.page.runtime);
        obtainable = script_runtime_heap_obtainable(navigation.page.runtime);
        char ballast[160];
        snprintf(ballast, sizeof(ballast),
                 "globalThis.ballast=new Uint8Array(%zu);pocSummary='ballast'",
                 room != 0 && obtainable > room ? obtainable - room : 0u);
        ok = script_runtime_evaluate_diagnostic(
            navigation.page.runtime, ballast, "<blob-ballast>", &result);
    }
    char source[1024];
    snprintf(source, sizeof(source),
             "for(let t=0;t<%u;t++)setTimeout(()=>{let outcome='none';"
             "try{%s}catch(e){outcome=e===null?'null':e.name}"
             "outcomes.push(outcome)},0);pocSummary='armed'", tasks, task);
    ok = ok && script_runtime_evaluate_diagnostic(
        navigation.page.runtime, source, "<blob-arm>", &result);
    size_t before_tasks = ok
        ? script_runtime_heap_obtainable(navigation.page.runtime) : 0;
    for (unsigned turn = 0; ok && turn < 4u * tasks; turn++)
        (void) navigation_advance_runtime(&navigation, 16, 4);
    /* A later ordinary task still runs. */
    if (ok && navigation.page.runtime != NULL)
        ok = script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "setTimeout(()=>{const a=[];for(let i=0;i<2000;i++)a.push({i});"
            "outcomes.push('later:'+a.length)},0);pocSummary='armed'",
            "<blob-later>", &result);
    for (unsigned turn = 0; ok && turn < 4u; turn++)
        (void) navigation_advance_runtime(&navigation, 16, 4);
    bool alive = ok && navigation.page.runtime != NULL
        && !navigation.page.script_memory_limited;
    size_t after_tasks = alive
        ? script_runtime_heap_obtainable(navigation.page.runtime) : 0;
    memset(&result, 0, sizeof(result));
    if (alive)
        alive = script_runtime_evaluate_diagnostic(
            navigation.page.runtime,
            "pocSummary=outcomes.join(',')", "<blob-outcome>", &result);
    printf("blob-page %s: obtainable %zu, %zu before the tasks, %zu after; "
           "realm %s; outcomes %s\n", label, obtainable, before_tasks,
           after_tasks, !ok ? "not set up" : alive ? "kept" : "stopped",
           alive ? result.summary : "-");
    bool matched = alive && strcmp(result.summary, expected) == 0;
    navigation_destroy(&navigation);
    CHECK(budget.current == 0);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(matched);
    return true;
}

/* A Blob larger than the heap can obtain is refused with a catchable
   RangeError, and a page that keeps trying keeps its realm. */
static bool test_page_survives_a_blob_too_large_for_its_heap(void)
{
    return page_blob_task("huge", 0u,
        "const part=new Uint8Array(1<<20);"
        "new Blob(Array(64).fill(part));outcome='made'",
        3u, "RangeError,RangeError,RangeError,later:2000");
}

static bool test_page_survives_a_blob_with_too_much_text(void)
{
    return page_blob_task("huge text", 0u,
        "const part='x'.repeat(256*1024);"
        "new Blob(Array(128).fill(part));outcome='made'",
        3u, "RangeError,RangeError,RangeError,later:2000");
}

/* A shared wide string rope fits near the heap ceiling, but making it
   contiguous does not. Refuse it before QuickJS attempts to flatten it. */
static bool test_page_survives_a_blob_with_a_large_rope(void)
{
    return page_blob_task("tight rope", 3u * MIB,
        "let part='\\u0100'.repeat(256*1024);"
        "for(let i=0;i<3;i++)part+=part;"
        "new Blob([{toString(){return part}}]);outcome='made'",
        1u, "RangeError,later:2000");
}

/* A page near its limit that keeps making moderate blob: URLs gets a
   catchable RangeError once the next Blob would take the heap into the
   stop rule's headroom, and keeps its realm. */
static bool test_page_near_its_limit_keeps_its_realm(void)
{
    return page_blob_task("tight", 3u * MIB,
        "globalThis.urls=[];"
        "const part=new Uint8Array(400*1024);"
        "for(let i=0;i<64;i++)urls.push(URL.createObjectURL(new Blob([part])))",
        1u, "RangeError,later:2000");
}

/* A one-byte slice and the first streamed chunks must fit even with less
   heap headroom than the source. Keeping the slice and cancelled readers
   alive must not keep the source bytes alive. */
static bool test_blob_slice_and_stream_copy_only_the_output(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "{const input=new Uint8Array(400*1024);input[0]=11;"
        "input[4096]=22;input[input.length-1]=33;"
        "globalThis.large=new Blob([input]);input.fill(0);}"
        "new ReadableStream().cancel();"
        "globalThis.pocSummary='ready'", "ready"));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t headroom = script_runtime_heap_available(page.runtime);
    CHECK(headroom > 128u * KIB);
    char ballast[128];
    snprintf(ballast, sizeof(ballast),
             "globalThis.ballast=new Uint8Array(%zu);"
             "globalThis.pocSummary='ballast'", headroom - 128u * KIB);
    CHECK(evaluate(&page, ballast, "ballast"));
    CHECK(script_runtime_heap_available(page.runtime) < BLOB_BYTES);
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(page.runtime,
        "globalThis.pocSummary='pending';"
        "(async()=>{"
        "globalThis.small=large.slice(-1,undefined,'TEXT/PLAIN');"
        "if(small.size!==1||small.type!=='text/plain'||"
        "(await small.bytes())[0]!==33||large.slice(-Infinity,0).size||"
        "large.slice(Infinity).size||large.slice(6,1).size)"
        "throw Error('slice');"
        "const file=new File(['012345'],'test'),clipped=file.slice(-2.5,5.5);"
        "if(!(clipped instanceof Blob)||clipped instanceof File||"
        "await clipped.text()!=='45'||file.slice(0,null,null).type!=='null'||"
        "file.slice(0,null).size||file.slice(-0.5,1).size!==1||"
        "file.slice(-0.1,1).size!==1)throw Error('file slice');"
        "globalThis.a=large.stream().getReader();"
        "globalThis.b=large.stream().getReader();"
        "const first=(await a.read()).value;"
        "if(first.length!==4096||first[0]!==11)throw Error('first');"
        "first[0]=0;const again=(await b.read()).value;"
        "const second=(await b.read()).value;"
        "if(again.length!==4096||again[0]!==11||"
        "second.length!==4096||second[0]!==22)throw Error('independent');"
        "await a.cancel();await b.cancel();"
        "if(!(await a.read()).done||!(await b.read()).done)"
        "throw Error('cancel');"
        "const r=new Blob([new Uint8Array(4097)]).stream().getReader();"
        "if((await r.read()).value.length!==4096||"
        "(await r.read()).value.length!==1||!(await r.read()).done)"
        "throw Error('end');"
        "return 'copied';"
        "})().then(s=>pocSummary=s,e=>pocSummary=String(e));",
        "<blob-budget-slice-stream>", &result));
    for (unsigned turn = 0; turn < 32u
         && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    if (strcmp(result.summary, "copied") != 0)
        fprintf(stderr, "blob slice/stream: %s\n", result.summary);
    CHECK(strcmp(result.summary, "copied") == 0);
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t before_drop = script_runtime_heap_used(page.runtime);
    CHECK(evaluate(&page,
        "globalThis.large=null;globalThis.pocSummary='dropped'", "dropped"));
    (void) script_runtime_collect_and_trim(page.runtime);
    CHECK(script_runtime_heap_used(page.runtime) + BLOB_BYTES <= before_drop);
    return page_close(&page);
}

/* Keeping a Request (and thus its body stream) must not retain the buffered
   snapshot once a caller has cancelled or read the stream directly. */
static bool test_consumed_request_snapshots_are_released(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "globalThis.held=[];new Request('/upload',{method:'POST',body:'warmup'});"
        "globalThis.pocSummary='ready'", "ready"));
    (void) script_runtime_collect_and_trim(page.runtime);
    size_t heap_before = script_runtime_heap_used(page.runtime);
    size_t budget_before = page.budget.current;
    CHECK(evaluate(&page,
        "{const input=new Uint8Array(192*1024);input.fill(17);"
        "for(let i=0;i<6;i++)held.push(new Request('/upload',"
        "{method:'POST',body:input}));input.fill(0);}"
        "globalThis.pocSummary=String(held.length)", "6"));
    (void) script_runtime_collect_and_trim(page.runtime);
    CHECK(script_runtime_heap_used(page.runtime) >= heap_before + 6u * 192u * KIB);

    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_diagnostic(page.runtime,
        "globalThis.pocSummary='pending';"
        "(async()=>{for(let i=0;i<held.length;i++){"
        "const request=held[i],stream=request.body;"
        "if(request.bodyUsed)throw Error('premature disturbance');"
        "if(i<3){await stream.cancel();}else{"
        "const reader=stream.getReader(),first=await reader.read();"
        "if(first.done||first.value.length!==192*1024||"
        "first.value[0]!==17||!(await reader.read()).done)"
        "throw Error('buffered snapshot');reader.releaseLock();}"
        "if(!request.bodyUsed||await request.bytes().then(()=>true,"
        "error=>!(error instanceof TypeError)))throw Error('body reusable');"
        "}return String(held.length)})().then(s=>pocSummary=s,e=>pocSummary=String(e));",
        "<request-budget-consumed>", &result));
    for (unsigned turn = 0; turn < 32u
        && strcmp(result.summary, "pending") == 0; turn++)
        CHECK(script_runtime_advance(page.runtime, 1u, 8u, &result));
    if (strcmp(result.summary, "6") != 0)
        fprintf(stderr, "consumed requests: %s\n", result.summary);
    CHECK(strcmp(result.summary, "6") == 0);
    (void) script_runtime_collect_and_trim(page.runtime);
    CHECK(script_runtime_heap_used(page.runtime) <= heap_before + 128u * KIB);
    CHECK(page.budget.current <= budget_before + 128u * KIB);
    return page_close(&page);
}

int main(void)
{
    if (!test_blob_type_normalization()) return 1;
    if (!test_blob_string_parts_and_live_views()) return 1;
    if (!test_held_blobs_are_charged()) return 1;
    if (!test_blobs_stop_at_the_realm_heap()) return 1;
    if (!test_blobs_and_urls_have_no_cap_of_their_own()) return 1;
    if (!test_revoking_returns_url_held_bytes()) return 1;
    if (!test_blob_urls_stop_at_the_realm_heap()) return 1;
    if (!test_unrevoked_urls_go_with_the_document()) return 1;
    bool reads_ok = test_large_blob_reads_as_text();
    reads_ok = test_large_blob_file_readers() && reads_ok;
    reads_ok = test_blob_text_read_keeps_bytes_private() && reads_ok;
    reads_ok = test_page_survives_a_blob_with_too_much_text() && reads_ok;
    reads_ok = test_page_survives_a_blob_with_a_large_rope() && reads_ok;
    if (!reads_ok) return 1;
    if (!test_page_survives_a_blob_too_large_for_its_heap()) return 1;
    if (!test_page_near_its_limit_keeps_its_realm()) return 1;
    if (!test_blob_slice_and_stream_copy_only_the_output()) return 1;
    if (!test_consumed_request_snapshots_are_released()) return 1;
    printf("PASS: blob bytes are page memory\n");
    return 0;
}
