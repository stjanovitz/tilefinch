/* Bootstrap footprint guard.
 *
 * The PSP realm is 5 MiB. Everything the trusted bootstrap retains after
 * evaluation is headroom a page never gets, and a preserved-runtime document
 * rebind must fit in whatever is left. Functional tests that happened to run
 * small heaps used to catch bootstrap growth by accident, failing on layout
 * noise without saying what grew; this test measures the numbers directly on
 * the device-sized realm and prints them on every run so growth is attributed
 * before a budget trips. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"

#include <stdio.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define KIB 1024u

/* The device realm. */
#define FOOTPRINT_REALM_BYTES (5u * MIB)
/* Resident bootstrap state after a trivial document is bound may take at
   most 2 MiB, leaving pages at least 3 MiB of the realm. Measured at
   1,922 KiB when this guard was introduced (2026-09), so the budget trips on
   roughly 130 KiB of growth; raise it only with a device-side reason. */
#define FOOTPRINT_BOOTSTRAP_LIMIT_BYTES (2u * MIB)
/* Heap a preserved-runtime rebind retains on top of the resident state;
   measured at 96 KiB when introduced. */
#define FOOTPRINT_REBIND_LIMIT_BYTES (256u * KIB)
/* Each lazily installed module, measured as retained heap after its first
   activation; the largest (canvas) measured 222 KiB when introduced. */
#define FOOTPRINT_LAZY_MODULE_LIMIT_BYTES (512u * KIB)

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

static const char page_html[] =
    "<!doctype html><title>Footprint</title><body><p id=p>hi</p></body>";

typedef struct {
    const char *name;
    const char *trigger;
} LazyProbe;

static size_t used_bytes(const ScriptRuntime *runtime)
{
    size_t remaining = script_runtime_heap_remaining(runtime);
    return remaining > FOOTPRINT_REALM_BYTES
        ? 0u : FOOTPRINT_REALM_BYTES - remaining;
}

int main(void)
{
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    memset(&document, 0, sizeof(document));
    CHECK(document_parse(&document, &budget, page_html,
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
    };
    ScriptResult result;
    memset(&result, 0, sizeof(result));
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, FOOTPRINT_REALM_BYTES, 8000,
        "https://footprint.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    size_t resident = used_bytes(runtime);
    printf("footprint: realm=%u resident-after-bootstrap=%zu (%zu KiB)\n",
           FOOTPRINT_REALM_BYTES, resident, resident / KIB);

    static const LazyProbe probes[] = {
        {"streams", "typeof ReadableStream"},
        {"indexeddb", "typeof IDBFactory"},
        {"motion", "typeof AnimationEvent"},
        {"canvas", "typeof document.createElement('canvas').getContext"},
        {"game-audio", "typeof AudioContext"},
        {"capabilities", "typeof speechSynthesis"},
        {"worker", "typeof Worker"},
        {"intl", "typeof Intl.NumberFormat"},
    };
    size_t before = resident;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        char source[256];
        snprintf(source, sizeof(source),
                 "globalThis.pocSummary=String(%s)", probes[i].trigger);
        ScriptResult probe_result;
        memset(&probe_result, 0, sizeof(probe_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, source, "<footprint-lazy>", &probe_result));
        size_t after = used_bytes(runtime);
        size_t delta = after > before ? after - before : 0u;
        printf("footprint: lazy %-12s retained=%zu (%zu KiB)\n",
               probes[i].name, delta, delta / KIB);
        CHECK(delta <= FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
        before = after;
    }

    /* A dedicated worker realm: its own QuickJS context plus the copied
       scope surface. Measured retained heap after creation. */
    {
        size_t before_worker = used_bytes(runtime);
        ScriptResult worker_result;
        memset(&worker_result, 0, sizeof(worker_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "globalThis.__footprintWorker=new Worker(URL.createObjectURL("
            "new Blob(['postMessage(1)'])));globalThis.pocSummary='ok'",
            "<footprint-worker>", &worker_result));
        size_t after_worker = used_bytes(runtime);
        size_t delta = after_worker > before_worker ? after_worker - before_worker : 0u;
        printf("footprint: worker realm retained=%zu (%zu KiB)\n",
               delta, delta / KIB);
        CHECK(delta <= FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
    }
    /* A same-origin local frame: its own QuickJS context (frames.js hands
       the WindowProxy a persistent realm) plus the child document. */
    {
        size_t before_frame = used_bytes(runtime);
        ScriptResult frame_result;
        memset(&frame_result, 0, sizeof(frame_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "const f=document.createElement('iframe');"
            "document.body.appendChild(f);"
            "globalThis.__footprintFrame=f.contentWindow;"
            "globalThis.pocSummary=typeof f.contentWindow.document",
            "<footprint-frame>", &frame_result));
        size_t after_frame = used_bytes(runtime);
        size_t delta = after_frame > before_frame ? after_frame - before_frame : 0u;
        printf("footprint: frame realm retained=%zu (%zu KiB) summary=%s\n",
               delta, delta / KIB, frame_result.summary);
        CHECK(strcmp(frame_result.summary, "object") == 0);
        CHECK(delta <= 180u * KIB);
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "const frame=globalThis.__footprintFrame, doc=frame.document;"
            "const evaluate=frame.eval;"
            "evaluate('var frameValue=19; function add(){return ++frameValue;}');"
            "globalThis.pocSummary=frame===globalThis.__footprintFrame"
            "&&doc===frame.document&&evaluate===frame.eval"
            "&&evaluate('this===window&&globalThis===window')"
            "&&frame.Array!==Array&&frame.Promise!==Promise"
            "&&evaluate('add()')===20?'FRAME-LAZY-OK':'bad'",
            "<footprint-frame-activate>", &frame_result));
        CHECK(strcmp(frame_result.summary, "FRAME-LAZY-OK") == 0);
        size_t activated = used_bytes(runtime);
        CHECK(activated > after_frame + 16u * KIB);
        printf("footprint: frame activation retained=%zu (%zu KiB)\n",
            activated - after_frame, (activated - after_frame) / KIB);
    }
    /* Preserved-runtime rebind onto a second document. */
    PocDocument replacement;
    memset(&replacement, 0, sizeof(replacement));
    CHECK(document_parse(&replacement, &budget, page_html,
                         sizeof(page_html) - 1u, 512u));
    size_t before_rebind = used_bytes(runtime);
    ScriptResult rebind_result;
    memset(&rebind_result, 0, sizeof(rebind_result));
    CHECK(script_runtime_rebind_document(runtime, &replacement,
                                         &rebind_result));
    size_t after_rebind = used_bytes(runtime);
    size_t rebind_delta = after_rebind > before_rebind
        ? after_rebind - before_rebind : 0u;
    printf("footprint: rebind retained=%zu (%zu KiB)\n",
           rebind_delta, rebind_delta / KIB);
    CHECK(rebind_delta <= FOOTPRINT_REBIND_LIMIT_BYTES);
    CHECK(resident <= FOOTPRINT_BOOTSTRAP_LIMIT_BYTES);

    script_runtime_destroy(runtime);
    document_destroy(&replacement);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    puts("bootstrap footprint tests passed");
    return 0;
}
