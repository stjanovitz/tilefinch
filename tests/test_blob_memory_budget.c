/* Blob and blob: URL bytes are page memory.
 *
 * A Blob's bytes (and a File's, which is a Blob) live in an ArrayBuffer of
 * the page realm, so QuickJS allocates them from the realm's allocator pool,
 * which charges the page Budget, and the realm's heap limit bounds them.
 * URL.createObjectURL() keeps a reference to the Blob, not a copy. This test
 * makes large blobs on the device-sized realm and checks that the page
 * Budget and the realm heap both grow by their size, that a page cannot make
 * more than its heap holds, that dropping them gives the memory back, and
 * that the bootstrap's own blob URL bounds (16 URLs, 512 KiB registered,
 * 512 KiB per Blob) still hold. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
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

static bool page_open(BlobPage *page)
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
        &page->document, &page->budget, REALM_BYTES, 8000,
        "https://blobs.test/", &options, &result);
    CHECK(page->runtime != NULL && result.success);
    return true;
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

/* A page that keeps making blobs runs out of its realm heap, the same way as
   for any other allocation, instead of growing past it. */
static bool test_blobs_stop_at_the_realm_heap(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    size_t heap_free = script_runtime_heap_available(page.runtime);
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
    CHECK(failure != NULL && strcmp(failure, ":memory") == 0);
    /* 64 blobs would be 25 MiB; the page got no more than its heap holds. */
    CHECK(count > 0 && (size_t) count * BLOB_BYTES <= heap_free);
    CHECK(page.budget.current <= PAGE_BUDGET_BYTES);
    /* The realm still works after the refusal. */
    (void) script_runtime_collect_and_trim(page.runtime);
    CHECK(evaluate(&page, "globalThis.pocSummary=String(new Blob(['ok']).size)",
                   "2"));
    return page_close(&page);
}

/* The bootstrap's separate blob URL bounds: 512 KiB per Blob, 512 KiB of
   registered blob: URL bytes, 16 URLs; revoking returns the allowance. */
static bool test_blob_url_bounds(void)
{
    BlobPage page;
    CHECK(page_open(&page));
    CHECK(evaluate(&page,
        "const r=[];"
        "try{new Blob([new Uint8Array(512*1024+1)]);r.push('big-ok')}"
        "catch(e){r.push(e instanceof RangeError?'big-range':'big-other')}"
        "const a=new Blob([new Uint8Array(400*1024)]),"
        "b=new Blob([new Uint8Array(400*1024)]);"
        "const ua=URL.createObjectURL(a);"
        "try{URL.createObjectURL(b);r.push('second-ok')}"
        "catch(e){r.push(e instanceof RangeError?'second-range':'second-other')}"
        "URL.revokeObjectURL(ua);"
        "const ub=URL.createObjectURL(b);r.push('after-revoke');"
        "URL.revokeObjectURL(ub);"
        "const small=new Blob(['x']),urls=[];"
        "try{for(let i=0;i<17;i++)urls.push(URL.createObjectURL(small))}"
        "catch(e){r.push('urls-'+urls.length)}"
        "for(const u of urls)URL.revokeObjectURL(u);"
        "globalThis.pocSummary=r.join(',')",
        "big-range,second-range,after-revoke,urls-16"));
    return page_close(&page);
}

int main(void)
{
    if (!test_held_blobs_are_charged()) return 1;
    if (!test_blobs_stop_at_the_realm_heap()) return 1;
    if (!test_blob_url_bounds()) return 1;
    printf("PASS: blob bytes are page memory\n");
    return 0;
}
