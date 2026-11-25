/* Bootstrap footprint guard.
 *
 * The PSP realm is 5 MiB. Everything the trusted bootstrap retains after
 * evaluation is headroom a page never gets. Functional tests that happened to run
 * small heaps used to catch bootstrap growth by accident, failing on layout
 * noise without saying what grew; this test measures the numbers directly on
 * the device-sized realm and prints them on every run so growth is attributed
 * before a budget trips. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"
/* Test-only: the realm's QuickJS runtime, for the live-block census. */
#include "../src/js_runtime_internal.h"

#include <stdio.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define KIB 1024u

/* The device realm. */
#define FOOTPRINT_REALM_BYTES (5u * MIB)
/* Resident bootstrap state after a trivial document is bound may take at
   most 2 MiB, leaving pages at least 3 MiB of the realm. Measured at
   1,922 KiB when this guard was introduced (2026-09), so the budget trips on
   roughly 130 KiB of growth; raise it only with a device-side reason.
   2026-11-14 it had reached 2,104,066 bytes, over the budget: the
   bootstrap's atoms were relocated on restore, so QuickJS copied every
   function's code into the heap. Pinning the eager modules' atoms
   (JS_PinAtomTable; src/bootstrap/README.md) lets that code run from ROM
   in place; a regression there alone puts this figure back over 2 MiB. */
#define FOOTPRINT_BOOTSTRAP_LIMIT_BYTES (2u * MIB)
/* Each lazily installed module, measured as retained heap after its first
   activation; the largest (canvas) measured 222 KiB when introduced. */
#define FOOTPRINT_LAZY_MODULE_LIMIT_BYTES (512u * KIB)
/* The bootstrap's function records after a collection (JSMemoryUsage
   js_func_size: headers, constant pools, capture descriptors and variable
   tables; the code runs from ROM). 694,155 bytes while every function kept
   its variable table; 609,315 (2026-11-23) once the generator wrote none
   for the functions whose locals nothing captures
   (JS_WRITE_OBJ_STRIP_UNUSED_LOCALS, src/bootstrap/README.md), so losing
   that puts the figure back over this limit. */
#define FOOTPRINT_FUNCTION_RECORD_LIMIT_BYTES (640u * KIB)

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

/* Bytes in live blocks after a full collection: the allocator total less
   the free blocks inside its arenas. The allocator total (used_bytes) moves
   in whole arenas: QuickJS carves small allocations from 4 KiB arenas, one
   set per size class (31 classes), and a class with no free block takes a
   new arena for its next block. A change of a few hundred bytes therefore
   reads as 4 KiB or more whenever one class it touches is full, so that
   total suits the 2 MiB budget, not a delta of a few KiB. */
static size_t live_bytes_after_gc(const ScriptRuntime *runtime)
{
    JS_RunGC(runtime->runtime);
    JSMemoryUsage usage;
    JS_ComputeMemoryUsage(runtime->runtime, &usage);
    int64_t free_in_arenas =
        usage.malloc_arena_capacity - usage.malloc_arena_used;
    return usage.malloc_size > free_in_arenas
        ? (size_t) (usage.malloc_size - free_in_arenas) : 0u;
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
        /* Measure the shipping realm: no test-only probes. */
        .omit_test_probes = true,
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
    {
        JSMemoryUsage usage;
        JS_RunGC(runtime->runtime);
        JS_ComputeMemoryUsage(runtime->runtime, &usage);
        printf("footprint: function records=%lld bytes=%lld\n",
               (long long) usage.js_func_count,
               (long long) usage.js_func_size);
        CHECK(usage.js_func_size <= FOOTPRINT_FUNCTION_RECORD_LIMIT_BYTES);
    }

    /* EventSource, FileReader, WebGPU's GPU and WGSLLanguageFeatures, and
       the channel behind navigator.tilefinchMultiplayer are lazy modules: a
       realm starts with the globals' placeholders and the frozen entry
       object, and builds none of them. Reading a descriptor, `in` and the
       entry object's fields loads nothing. */
    {
        ScriptResult cold_result = {0};
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "globalThis.pocSummary=['EventSource','FileReader','GPU',"
            "'WGSLLanguageFeatures'].map(n=>{const d=Object"
            ".getOwnPropertyDescriptor(globalThis,n);return (n in globalThis)"
            "+':'+typeof d.get+':'+d.enumerable+':'+d.configurable}).join()"
            "+'|'+Object.isFrozen(navigator.tilefinchMultiplayer)+':'"
            "+navigator.tilefinchMultiplayer.supported+':'"
            "+navigator.tilefinchMultiplayer.maxMessageSize",
            "<footprint-cold-apis>", &cold_result));
        printf("footprint: cold APIs %s lazy-loads=%zu\n",
               cold_result.summary, cold_result.bootstrap_lazy_module_loads);
        CHECK(strcmp(cold_result.summary,
                     "true:function:true:true,true:function:true:true,"
                     "true:function:true:true,true:function:true:true"
                     "|true:true:512") == 0);
        CHECK(cold_result.bootstrap_lazy_module_loads == 0);
    }

    /* SharedWorker is an eager refusing shell. Probing it must not activate
       the Worker module (130-530 KiB on real pages) and may retain only
       the probe's own small change, measured in live bytes. */
    {
        size_t before_shared = live_bytes_after_gc(runtime);
        ScriptResult shared_result;
        memset(&shared_result, 0, sizeof(shared_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "globalThis.pocSummary=typeof SharedWorker+':'+!window.SharedWorker",
            "<footprint-shared-worker>", &shared_result));
        size_t after_shared = live_bytes_after_gc(runtime);
        size_t delta = after_shared > before_shared
            ? after_shared - before_shared : 0u;
        printf("footprint: SharedWorker probe live=%zu (%zu KiB) "
               "lazy-loads=%zu summary=%s\n",
               delta, delta / KIB, shared_result.bootstrap_lazy_module_loads,
               shared_result.summary);
        CHECK(strcmp(shared_result.summary, "function:false") == 0);
        CHECK(shared_result.bootstrap_lazy_module_loads == 0);
        CHECK(delta <= 4u * KIB);
    }

    /* Window messaging's bounded structured-clone wire (frame-clone.js)
       is built by the first postMessage, not at bootstrap, and loads that
       one module; the message still arrives intact. */
    {
        size_t before_message = used_bytes(runtime);
        ScriptResult message_result;
        memset(&message_result, 0, sizeof(message_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "addEventListener('message',e=>{globalThis.__footprintMessage="
            "e.data.a.join()+':'+e.data.m.get(1)+':'+(e.data.u===undefined)}"
            ");postMessage({a:[1,2],m:new Map([[1,'x']]),u:undefined},'*');"
            "globalThis.pocSummary='posted'",
            "<footprint-message>", &message_result));
        CHECK(strcmp(message_result.summary, "posted") == 0);
        CHECK(message_result.bootstrap_lazy_module_loads == 1);
        for (unsigned turn = 0; turn < 4; turn++)
            CHECK(script_runtime_advance(runtime, 16, 16, &message_result));
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, "globalThis.pocSummary=String(globalThis.__footprintMessage)",
            "<footprint-message-check>", &message_result));
        size_t after_message = used_bytes(runtime);
        size_t delta = after_message > before_message
            ? after_message - before_message : 0u;
        printf("footprint: lazy frame-clone retained=%zu (%zu KiB) "
               "lazy-loads=%zu summary=%s\n",
               delta, delta / KIB, message_result.bootstrap_lazy_module_loads,
               message_result.summary);
        CHECK(strcmp(message_result.summary, "1,2:x:true") == 0);
        CHECK(message_result.bootstrap_lazy_module_loads == 1);
        CHECK(delta <= FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
    }

    /* Plain attribute writes and copies skip the namespace module; the
       first namespace write loads it through a private factory. */
    {
        ScriptResult attribute_result = {0};
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const e=document.createElement('div');"
            "e.setAttribute('data-a','plain');"
            "globalThis.pocSummary=e.cloneNode(true).getAttribute('data-a')})()",
            "<footprint-ordinary-attributes>", &attribute_result));
        CHECK(strcmp(attribute_result.summary, "plain") == 0);
        CHECK(attribute_result.bootstrap_lazy_module_loads == 1);
        size_t before_attributes = used_bytes(runtime);
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const e=document.createElement('div');"
            "const hidden=!('__tilefinchEnsureDOMAttributesBootstrap' in globalThis)"
            "&&!('__tilefinchSetDOMAttributesFactory' in globalThis);"
            "Object.defineProperty(globalThis,'__tilefinchDOMAttributesFactory',"
            "{value:()=>{throw Error('page factory')}});"
            "e.setAttributeNS('urn:footprint','p:a','namespaced');"
            "globalThis.pocSummary=hidden?e.cloneNode(true)"
            ".getAttributeNS('urn:footprint','a'):'exposed hook'})()",
            "<footprint-namespaced-attributes>", &attribute_result));
        CHECK(strcmp(attribute_result.summary, "namespaced") == 0);
        CHECK(attribute_result.bootstrap_lazy_module_loads == 2);
        CHECK(used_bytes(runtime) <= before_attributes + FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
    }

    /* Each API moved out of compat.js and platform.js is built on its first
       touch, loading its own module and no other, and still works; the
       module's heap, which every realm used to hold, is retained from
       then on. */
    {
        static const LazyProbe moved[] = {
            {"event-source",
             "(()=>{const s=new EventSource('https://footprint.test/feed');"
             "const ok=s instanceof EventSource&&s instanceof EventTarget"
             "&&EventSource.name==='EventSource'&&s.readyState===0"
             "&&EventSource.CLOSED===2&&EventSource.prototype.OPEN===1;"
             "s.close();return ok&&s.readyState===2})()"},
            {"file-reader",
             "(()=>{const r=new FileReader();r.readAsText(new Blob(['x']));"
             "const ok=r instanceof FileReader&&r instanceof EventTarget"
             "&&FileReader.name==='FileReader'&&r.readyState===1"
             "&&FileReader.DONE===2;r.abort();return ok&&r.readyState===2})()"},
            {"multiplayer",
             "(()=>{const c=navigator.tilefinchMultiplayer.host("
             "{gameId:'footprint'});return c instanceof EventTarget"
             "&&c.constructor.name==='TilefinchMultiplayerChannel'"
             "&&c.readyState==='connecting'})()"},
            {"gpu",
             "(()=>{const g=navigator.gpu;let refused=false;"
             "try{new GPU()}catch(e){refused=e instanceof TypeError}"
             "return g instanceof GPU&&g===navigator.gpu&&refused"
             "&&g.wgslLanguageFeatures instanceof WGSLLanguageFeatures"
             "&&g.wgslLanguageFeatures.size===0"
             "&&g.getPreferredCanvasFormat()==='bgra8unorm'"
             "&&Object.prototype.toString.call(g)==='[object GPU]'})()"},
        };
        for (size_t i = 0; i < sizeof(moved) / sizeof(moved[0]); i++) {
            char source[512];
            snprintf(source, sizeof(source),
                     "globalThis.pocSummary=String(%s)", moved[i].trigger);
            ScriptResult moved_before = {0}, moved_result = {0};
            CHECK(script_runtime_evaluate_diagnostic(
                runtime, "globalThis.pocSummary='before'",
                "<footprint-moved-before>", &moved_before));
            size_t before_moved = live_bytes_after_gc(runtime);
            CHECK(script_runtime_evaluate_diagnostic(
                runtime, source, "<footprint-moved>", &moved_result));
            size_t after_moved = live_bytes_after_gc(runtime);
            size_t delta = after_moved > before_moved
                ? after_moved - before_moved : 0u;
            printf("footprint: lazy %-12s live=%zu (%zu KiB) "
                   "lazy-loads=%zu summary=%s\n", moved[i].name, delta,
                   delta / KIB, moved_result.bootstrap_lazy_module_loads,
                   moved_result.summary);
            CHECK(strcmp(moved_result.summary, "true") == 0);
            CHECK(moved_result.bootstrap_lazy_module_loads
                  == moved_before.bootstrap_lazy_module_loads + 1);
            CHECK(delta >= 2u * KIB);
            CHECK(delta <= FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
        }
    }

    static const LazyProbe probes[] = {
        {"streams", "typeof ReadableStream"},
        {"indexeddb", "typeof IDBFactory"},
        {"motion", "typeof AnimationEvent"},
        {"canvas", "typeof document.createElement('canvas').getContext"},
        {"game-audio", "typeof AudioContext"},
        {"capabilities", "typeof speechSynthesis"},
        {"worker", "typeof Worker"},
        {"intl", "typeof Intl.NumberFormat"},
        {"traversal", "typeof document.createTreeWalker"},
        {"serialize", "typeof XMLSerializer"},
        {"webgl-ifaces", "typeof WebGLRenderingContext"},
        /* This realm has no layout, so no box is authoritative. */
        {"geometry",
         "document.getElementById('p').getBoundingClientRect().width"},
        {"validation",
         "document.createElement('input').validity.valid"},
        {"detached-doc",
         "document.implementation.createDocument(null,'r').documentElement"
         ".localName"},
        {"subtle",
         "typeof crypto.subtle.generateKey({name:'AES-GCM',length:128},"
         "false,['encrypt'])"},
    };
    size_t before = used_bytes(runtime), loads_before = 0;
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
        printf("footprint: lazy %-12s retained=%zu (%zu KiB) "
               "lazy-loads=%zu\n", probes[i].name, delta, delta / KIB,
               probe_result.bootstrap_lazy_module_loads);
        CHECK(delta <= FOOTPRINT_LAZY_MODULE_LIMIT_BYTES);
        /* Reading WebGL's interface objects loads their small module, not
           webgl.js; a layout-less measurement, a validity check, an
           XML document and a SubtleCrypto operation each load only their
           own. */
        if (strcmp(probes[i].name, "webgl-ifaces") == 0
            || strcmp(probes[i].name, "geometry") == 0
            || strcmp(probes[i].name, "validation") == 0
            || strcmp(probes[i].name, "detached-doc") == 0
            || strcmp(probes[i].name, "subtle") == 0)
            CHECK(probe_result.bootstrap_lazy_module_loads
                  == loads_before + 1);
        loads_before = probe_result.bootstrap_lazy_module_loads;
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
        printf("footprint: frame activation retained=%zu (%zu KiB)\n",
            activated - after_frame, (activated - after_frame) / KIB);
        /* Proves activation built the realm; the exact figure moves by
           several KiB with collection timing (17 KiB -> 13 KiB when the
           bootstrap grew by one Range helper). */
        CHECK(activated > after_frame + 8u * KIB);
    }
    CHECK(resident <= FOOTPRINT_BOOTSTRAP_LIMIT_BYTES);

    script_runtime_destroy(runtime);

    /* A realm whose FileReader and WebGPU modules failed to load (the
       host's failed state, 3 in lazy_bootstrap_state; indices are the
       runtime's LazyBootstrapFeature values): navigator.gpu reads
       undefined, and a Worker still starts, with neither. Only the
       EventSource and multiplayer delivery hooks may be missing when the
       host calls one; any other missing hook is a failed call. */
    {
        memset(&result, 0, sizeof(result));
        runtime = script_runtime_create_configured(
            &document, &budget, FOOTPRINT_REALM_BYTES, 8000,
            "https://footprint.test/", &options, &result);
        CHECK(runtime != NULL && result.success);
        CHECK(!js_rt_network_call_global(
            runtime, "__tilefinchNoSuchHook", 0, NULL));
        CHECK(js_rt_network_call_global(
            runtime, "__tilefinchDeliverEventSource", 0, NULL));
        runtime->lazy_bootstrap_state[26] = 3; /* FileReader */
        runtime->lazy_bootstrap_state[28] = 3; /* GPU */
        ScriptResult failed_result = {0};
        bool ok = script_runtime_evaluate_diagnostic(
            runtime,
            "globalThis.pocSummary='PENDING';let gpu,made;"
            "try{gpu=String(navigator.gpu)}catch(e){gpu='threw '+e.name}"
            "try{const w=new Worker(URL.createObjectURL(new Blob(["
            "\"postMessage(typeof FileReader+':'+typeof navigator.gpu)\"])));"
            "made='ok';w.onmessage=e=>{pocSummary=gpu+'|'+made+'|'+e.data};"
            "w.onerror=e=>{pocSummary='worker error '+e.message}}"
            "catch(e){pocSummary=gpu+'|threw '+e.name}",
            "<footprint-failed-modules>", &failed_result);
        for (unsigned turn = 0; ok && turn < 16
             && strcmp(failed_result.summary, "PENDING") == 0; turn++)
            ok = script_runtime_advance(runtime, 16, 16, &failed_result);
        printf("footprint: failed modules %s\n", failed_result.summary);
        CHECK(ok && strcmp(failed_result.summary,
                           "undefined|ok|undefined:undefined") == 0);
        script_runtime_destroy(runtime);
    }
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget));
    CHECK(budget.current == 0);
    puts("bootstrap footprint tests passed");
    return 0;
}
