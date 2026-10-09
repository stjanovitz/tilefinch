#include "tilefinch/browser_engine.h"
#include "tilefinch/controller.h"
#include "tilefinch/document.h"
#include "tilefinch/fetch.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/navigation.h"
#include "tilefinch/sha256.h"
#include "tilefinch/user_agent.h"
#include "tilefinch/viewport.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lexbor/dom/interfaces/node.h>

#include "tilefinch/platform.h"
#include "tilefinch/runtime_clock.h"
#include "tilefinch/script_split.h"
#include "../src/js_runtime_internal.h"
#include "../src/tilefinch_test_faults.h"
#include "tilefinch/budget_quickjs.h"

#define MIB (1024u * 1024u)

typedef struct {
    size_t calls;
} CallbackAbortCooperate;

static bool callback_abort_cooperate(void *context, const char *phase,
                                     size_t completed_work_units)
{
    (void) phase;
    (void) completed_work_units;
    CallbackAbortCooperate *state = context;
    state->calls++;
    return false;
}

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
/* Deterministic compile-abort driver: the cooperate service refuses to
   continue, so the parser's bounded interrupt poll aborts an admitted
   compile without depending on wall-clock timing. */
typedef struct {
    size_t calls;
} CompileAbortCooperate;

static bool compile_abort_cooperate(void *context, const char *phase,
                                    size_t completed_work_units)
{
    (void) phase;
    (void) completed_work_units;
    CompileAbortCooperate *state = context;
    state->calls++;
    return false;
}
#endif

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return 1;                                                            \
    }                                                                        \
} while (0)

typedef struct {
    uint64_t now_ns;
    size_t calls;
} TimedCooperate;

static uint64_t timed_cooperate_clock(void *opaque)
{
    return ((TimedCooperate *) opaque)->now_ns;
}

static bool timed_cooperate_poll(void *opaque, const char *phase, size_t work)
{
    (void) phase;
    (void) work;
    ((TimedCooperate *) opaque)->calls++;
    return true;
}

static JSValue timed_promise_job(JSContext *context, int argc,
                                 JSValueConst *argv)
{
    (void) argc;
    (void) argv;
    TimedCooperate *probe = JS_GetContextOpaque(context);
    probe->now_ns += UINT64_C(10000000);
    return JS_UNDEFINED;
}

/* A clock held at the instant it was installed, for one evaluation whose
   checks compare two reads of it. Game-audio scheduling reads currentTime
   (performance.now()) once in the page and again when it converts a start
   time to a delay; with the real clock a loaded host could stall the page
   between the two reads for longer than the 5 ms lead the check gives the
   oscillator, which then started at once. The held values are real
   instants, so the runtime's clock never moves backwards. */
typedef struct {
    uint64_t monotonic_ns;
    uint64_t wall_ns;
} HeldClock;

static uint64_t held_clock_monotonic_ns(void *context)
{
    return ((const HeldClock *) context)->monotonic_ns;
}

static uint64_t held_clock_wall_ns(void *context)
{
    return ((const HeldClock *) context)->wall_ns;
}

static bool evaluate_with_held_clock(ScriptRuntime *runtime,
                                     const char *source, const char *name,
                                     ScriptResult *result)
{
    HeldClock held = {
        .monotonic_ns = tilefinch_platform_monotonic_time_ns(),
        .wall_ns = tilefinch_platform_wall_time_ns()
    };
    TilefinchPlatformServices services = {
        .context = &held,
        .wall_time_ns = held_clock_wall_ns,
        .monotonic_time_ns = held_clock_monotonic_ns
    };
    tilefinch_platform_set_services(&services);
    bool evaluated = script_runtime_evaluate_diagnostic(
        runtime, source, name, result);
    tilefinch_platform_set_services(NULL);
    return evaluated;
}

static int test_user_activation_expiry(void)
{
    TimedCooperate probe = { .now_ns = UINT64_C(1000000000) };
    TilefinchPlatformServices services = {
        .context = &probe, .monotonic_time_ns = timed_cooperate_clock
    };
    DomBridge bridge = { .performance_origin_ns = probe.now_ns };
    tilefinch_platform_set_services(&services);
    js_rt_bridge_notify_user_activation(&bridge);
    CHECK(js_rt_bridge_user_activation_is_active(&bridge)
          && bridge.user_activation_has_been_active);
    probe.now_ns += UINT64_C(4999000000);
    CHECK(js_rt_bridge_user_activation_is_active(&bridge));
    probe.now_ns += UINT64_C(1000000);
    CHECK(!js_rt_bridge_user_activation_is_active(&bridge)
          && bridge.user_activation_has_been_active);
    tilefinch_platform_set_services(NULL);
    return 0;
}

static int test_watchdog_elapsed_cooperation(void)
{
    TimedCooperate probe = { .now_ns = UINT64_C(1000000000) };
    TilefinchPlatformServices services = { .context = &probe,
        .monotonic_time_ns = timed_cooperate_clock,
        .cooperate = timed_cooperate_poll };
    ScriptRuntime runtime = {0};
    runtime.runtime = JS_NewRuntime();
    CHECK(runtime.runtime != NULL);
    runtime.watchdog.deadline_ms = 100000;
    runtime.watchdog.slice_started_ns = probe.now_ns;
    tilefinch_platform_set_services(&services);
    CHECK(js_rt_runtime_native_checkpoint(&runtime) && probe.calls == 0);
    probe.now_ns += UINT64_C(8000000);
    /* Two expensive units must yield without waiting for four polls. */
    CHECK(js_rt_runtime_native_checkpoint(&runtime) && probe.calls == 1);
    CHECK(runtime.watchdog.deadline_ms == 100000);
    for (unsigned at = 0; at < 4; at++)
        CHECK(js_rt_runtime_native_checkpoint(&runtime));
    CHECK(probe.calls == 2); /* Cheap units retain the four-poll bound. */
    Budget budget = {0};
    runtime.budget = &budget;
    runtime.context = JS_NewContext(runtime.runtime);
    CHECK(runtime.context != NULL);
    JS_SetContextOpaque(runtime.context, &probe);
    for (unsigned at = 0; at < 3; at++)
        CHECK(JS_EnqueueJob(runtime.context, timed_promise_job, 0, NULL) == 0);
    /* Jobs without any VM instruction polls must also service native input. */
    CHECK(js_rt_runtime_run_jobs(&runtime));
    CHECK(probe.calls == 4 && !JS_IsJobPending(runtime.runtime));
    CHECK(runtime.watchdog.deadline_ms == 100000);
    tilefinch_platform_set_services(NULL);
    JS_FreeContext(runtime.context);
    JS_FreeRuntime(runtime.runtime);
    return 0;
}

static int test_reduced_dom_event_counter(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    CHECK(runtime != NULL);
    JSContext *context = JS_NewContext(runtime);
    CHECK(context != NULL);

    JS_SetContextOpaque(context, NULL);
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);

    DomBridge bridge = {0};
    JS_SetContextOpaque(context, &bridge);
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);

    ScriptResult result = {0};
    bridge.result = &result;
    (void) js_dom_record_event(context, JS_UNDEFINED, 0, NULL);
    CHECK(result.events_dispatched == 1);

    JS_SetContextOpaque(context, NULL);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return 0;
}

static int test_job_heap_rejection_is_fatal(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://job-heap.test/", NULL, &result);
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__tilefinchHeapFailure=()=>"
              "new ArrayBuffer(65536)",
              "<job-heap-failure-setup>", &result));

    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue callback = JS_GetPropertyStr(
        runtime->context, global, "__tilefinchHeapFailure");
    CHECK(JS_IsFunction(runtime->context, callback));
    size_t slot = ((size_t) runtime->checkpoint_continuation_head
                   + runtime->checkpoint_continuation_count)
        % SCRIPT_CHECKPOINT_CONTINUATION_LIMIT;
    runtime->checkpoint_continuations[slot] =
        JS_DupValue(runtime->context, callback);
    runtime->checkpoint_continuation_count++;

    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    size_t rejections_before = script_runtime_heap_rejections(runtime);
    JS_SetMemoryLimit(runtime->runtime, live + 1024u);
    CHECK(!js_rt_runtime_run_jobs(runtime)
          && runtime->checkpoint_continuation_count == 0u
          && script_runtime_heap_rejections(runtime) > rejections_before);

    /* A fatal task result retires the owning page in production. The VM still
       has to be destructible and testable after the exception was consumed. */
    JS_SetMemoryLimit(runtime->runtime, 5u * MIB);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "delete globalThis.__tilefinchHeapFailure;"
              "globalThis.pocSummary='JOB-HEAP-RECOVERED'",
              "<job-heap-failure-recovery>", &result)
          && strcmp(result.summary, "JOB-HEAP-RECOVERED") == 0);
    JS_FreeValue(runtime->context, callback);
    JS_FreeValue(runtime->context, global);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* After parsing, an attribute or inline-style write changes no document
   statistic but the attribute totals, so the next advance must not pay a
   whole-document refresh; a structural mutation still does. */
static int test_attribute_mutation_skips_document_refresh(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><p id=a class=x>Ready</p></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://attribute-refresh.test/", NULL, &result);
    CHECK(runtime != NULL);
    size_t attributes = document.attribute_count;
    CHECK(!document.bidi_markup_present && !document.attribute_totals_stale);
    /* A refresh attempted now would be refused and fail the page. */
    script_runtime_test_refuse_next_document_refresh();
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const p=document.getElementById('a');"
              "p.setAttribute('dir','rtl');p.style.color='red';"
              "p.removeAttribute('class');globalThis.pocSummary='ATTR'",
              "<attribute-mutation>", &result)
          && strcmp(result.summary, "ATTR") == 0
          && script_runtime_advance(runtime, 0, 1024, &result)
          && !script_runtime_document_refresh_failed(runtime));
    CHECK(document.bidi_markup_present && document.attribute_totals_stale);
    document_refresh_attribute_totals(&document);
    CHECK(!document.attribute_totals_stale
          && document.attribute_count == attributes + 1u);
    /* A structural mutation refreshes, which consumes the refusal. */
    (void) script_runtime_evaluate_diagnostic(
        runtime, "document.body.appendChild(document.createElement('p'))",
        "<structural-mutation>", &result);
    (void) script_runtime_advance(runtime, 0, 1024, &result);
    CHECK(script_runtime_document_refresh_failed(runtime));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* getElementById reads each element's id attribute; renames and removals
   must be seen at once, and the empty string never matches. */
static int test_get_element_by_id_tracks_id_changes(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><p id=first>a</p><p id=''>b</p></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000, "https://by-id.test/", NULL,
        &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const a=document.getElementById('first'),r=[a!==null,"
              "document.getElementById('')===null];a.id='second';"
              "r.push(document.getElementById('first')===null,"
              "document.getElementById('second')===a);"
              "a.removeAttribute('id');"
              "r.push(document.getElementById('second')===null);"
              "globalThis.pocSummary=r.every(Boolean)?'BY-ID-OK':"
              "'BY-ID-FAILED:'+r",
              "<by-id>", &result)
          && strcmp(result.summary, "BY-ID-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Custom-element hooks skip all work until something is defined; after a
   definition, wrappers made earlier upgrade, removal still reaches
   disconnectedCallback, and a re-created wrapper keeps its prototype. */
/* HTML document.domain setter: a parent domain that is not a public suffix
   is accepted (a no-op in an origin-keyed agent cluster, so the getter keeps
   the host); public suffixes, unrelated hosts and malformed values still
   throw SecurityError. */
static int test_document_domain_setter(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://m.example.co.uk/path", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const bad=[];const throws=(v)=>{try{document.domain=v;"
              "return false}catch(e){return e instanceof DOMException&&"
              "e.name==='SecurityError'}};"
              "for(const v of ['example.co.uk','EXAMPLE.co.uk','m.example.co.uk'])"
              "if(throws(v))bad.push('accept:'+v);"
              "for(const v of ['co.uk','uk','other.co.uk','ample.co.uk',"
              "'x.m.example.co.uk','','example.co.uk:443','example.co.uk/x',"
              "'a@example.co.uk','127.0.0.1','[::1]'])"
              "if(!throws(v))bad.push('refuse:'+v);"
              "if(document.domain!=='m.example.co.uk')bad.push('getter:'"
              "+document.domain);"
              "if(typeof globalThis.__tilefinchDocumentDomainValid==='function'&&"
              "Object.keys(globalThis).includes('__tilefinchDocumentDomainValid'))"
              "bad.push('enumerable');"
              "globalThis.pocSummary=bad.length?'DOMAIN:'+bad.join(','):"
              "'DOMAIN-OK';})()",
              "<document-domain-setter>", &result));
    if (strcmp(result.summary, "DOMAIN-OK") != 0) {
        fprintf(stderr, "document.domain probe: %s\n", result.summary);
    }
    CHECK(strcmp(result.summary, "DOMAIN-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_custom_element_hooks_after_first_definition(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><x-early id=early></x-early></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000, "https://custom.test/", NULL,
        &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const early=document.getElementById('early');"
              "const d=document.createElement('div');document.body.appendChild(d);"
              "d.remove();const log=[];class XEarly extends HTMLElement{"
              "connectedCallback(){log.push('c')}"
              "disconnectedCallback(){log.push('d')}}"
              "customElements.define('x-early',XEarly);"
              "const upgraded=early instanceof XEarly;early.remove();"
              "document.body.appendChild(early);__tilefinchClearNodeCache();"
              "const again=document.getElementById('early');"
              "globalThis.pocSummary=upgraded&&again instanceof XEarly"
              "&&log.join('')==='cdc'?'CE-OK':'CE:'+upgraded+','"
              "+(again instanceof XEarly)+','+log.join('');})()",
              "<custom-element-hooks>", &result)
          && strcmp(result.summary, "CE-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Script-facing helpers made cheaper must keep their answers: focus leaves
   an element that became unfocusable (judged on the task's final state),
   a custom element still observes its style attribute, and cached selector
   validation still throws for every invalid use. */
static int test_focus_style_and_selector_helpers(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><div id=list></div>"
        "<input id=gone><input id=kept><input id=flip></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 6u * MIB, 4000, "https://helpers.test/", NULL,
        &result);
    CHECK(runtime != NULL);
    static const char *const focus_cases[][2] = {
        { "gone", "document.getElementById('gone').disabled=true;" },
        { "kept", "" },
        { "flip", "const f=document.getElementById('flip');"
                  "f.disabled=true;f.disabled=false;" },
    };
    for (size_t i = 0; i < 3; i++) {
        char script[512];
        snprintf(script, sizeof(script),
                 "(()=>{document.getElementById('%s').focus();"
                 "const list=document.getElementById('list');"
                 "for(let i=0;i<40;i++){list.textContent=String(i);"
                 "list.style.width=i+'px';}%s"
                 "globalThis.pocSummary='queued';})()",
                 focus_cases[i][0], focus_cases[i][1]);
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime, script, "<focus-fixup>", &result));
        for (int step = 0; step < 4; step++)
            CHECK(script_runtime_advance(runtime, 20, 64, &result));
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime,
                  "globalThis.pocSummary=document.activeElement===document.body"
                  "?'body':String(document.activeElement.id)",
                  "<focus-result>", &result));
        const char *expected = i == 0 ? "body" : focus_cases[i][0];
        if (strcmp(result.summary, expected) != 0)
            fprintf(stderr, "focus case %zu: %s\n", i, result.summary);
        CHECK(strcmp(result.summary, expected) == 0);
    }
    /* Only mutations that can change the focused element (itself, an
       ancestor, its removal, a style sheet) ask whether it is still
       focusable; each ask reads its computed style. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const kept=document.getElementById('kept');kept.focus();"
              "const list=document.getElementById('list'),original="
              "getComputedStyle;globalThis.focusReads=0;globalThis."
              "getComputedStyle=function(target){if(target===kept)"
              "globalThis.focusReads++;return original.apply(this,arguments)};"
              "for(let i=0;i<12;i++)setTimeout(()=>{list.textContent=String(i);"
              "list.setAttribute('data-i',String(i))},0);"
              "setTimeout(()=>{globalThis.unrelatedReads=focusReads;"
              "document.body.setAttribute('data-x','1')},0);"
              "setTimeout(()=>{kept.setAttribute('data-y','1')},0);"
              "globalThis.pocSummary='queued';})()",
              "<focus-fixup-relevant>", &result));
    for (int step = 0; step < 4; step++)
        CHECK(script_runtime_advance(runtime, 20, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=(document.activeElement.id==='kept'"
              "&&document.getElementById('list').textContent==='11'"
              "&&unrelatedReads===0&&focusReads===2)?'RELEVANT-ONLY'"
              ":'READS:'+unrelatedReads+','+focusReads",
              "<focus-fixup-relevant-result>", &result));
    if (strcmp(result.summary, "RELEVANT-ONLY") != 0)
        fprintf(stderr, "focus fixup relevance: %s\n", result.summary);
    CHECK(strcmp(result.summary, "RELEVANT-ONLY") == 0);
    /* Removing the focused element's ancestor still moves focus. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const wrap=document.createElement('div'),input="
              "document.createElement('input');input.id='nested';"
              "wrap.appendChild(input);document.body.appendChild(wrap);"
              "input.focus();setTimeout(()=>wrap.remove(),0);"
              "globalThis.pocSummary='queued';})()",
              "<focus-fixup-removal>", &result));
    for (int step = 0; step < 4; step++)
        CHECK(script_runtime_advance(runtime, 20, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=document.activeElement===document.body"
              "?'body':String(document.activeElement.id)",
              "<focus-fixup-removal-result>", &result)
          && strcmp(result.summary, "body") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const seen=[];class XStyled extends HTMLElement{"
              "static get observedAttributes(){return['style']}"
              "attributeChangedCallback(n,o,v){seen.push(n+':'+(o===null)+':'"
              "+/red/.test(v))}}customElements.define('x-styled',XStyled);"
              "const x=document.createElement('x-styled');document.body.appendChild(x);"
              "x.style.color='red';const plain=document.createElement('p');"
              "plain.style.setProperty('color','blue');const throws=()=>{"
              "try{document.body.matches('p[');return false}catch(e){"
              "return e.name==='SyntaxError'}};const t1=throws(),t2=throws();"
              "const inner=document.createElement('span');x.appendChild(inner);"
              "const ok=seen.join()==='style:true:true'"
              "&&plain.style.color==='blue'&&t1&&t2"
              "&&inner.closest('x-styled')===x&&inner.closest('x-styled')===x"
              "&&x.matches(':defined')&&inner.closest(':scope')===inner;"
              "globalThis.pocSummary=ok?'HELPERS-OK':'HELPERS:'+seen.join()"
              "+','+plain.style.color+','+t1+t2;})()",
              "<style-and-selectors>", &result)
          && strcmp(result.summary, "HELPERS-OK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const a=new Event('x'),b=new Event('y'),"
              "d=Object.getOwnPropertyDescriptor(a,'isTrusted'),"
              "el=document.createElement('p');el.className='a b a';"
              "const r=[d.get===Object.getOwnPropertyDescriptor(b,'isTrusted')"
              ".get,d.enumerable&&!d.configurable,a.isTrusted===false,"
              "el.classList.toggle('c')===true,el.className==='a b c',"
              "el.classList.toggle('a')===false,el.className==='b c',"
              "el.classList.contains('b'),!el.classList.contains(''),"
              "el.classList.toggle('b',true)===true,el.className==='b c',"
              "el.classList.toggle('z',false)===false,el.className==='b c'];"
              "globalThis.pocSummary=r.every(Boolean)?'TOKENS-OK':"
              "'TOKENS:'+r})()",
              "<tokens-and-events>", &result)
          && strcmp(result.summary, "TOKENS-OK") == 0);
    /* Canvas size is re-read once per scheduler tick; every attribute path
       must still reset the bitmap at once through the attribute hook. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const c=document.createElement('canvas');"
              "document.body.appendChild(c);const x=c.getContext('2d');"
              "const cleared=()=>x.getImageData(0,0,1,1).data[3]===0;"
              "const r=[];x.fillRect(0,0,300,150);c.setAttribute('width','40');"
              "r.push(cleared());x.fillRect(0,0,300,150);c.width=30;"
              "r.push(cleared());x.fillRect(0,0,300,150);"
              "c.getAttributeNode('width').value='20';r.push(cleared());"
              "x.fillRect(0,0,300,150);c.setAttributeNS(null,'height','10');"
              "r.push(cleared());x.fillRect(0,0,300,150);"
              "c.removeAttributeNS(null,'height');r.push(cleared());"
              "x.fillRect(0,0,300,150);c.toggleAttribute('width');"
              "r.push(cleared(),c.width===300,c.height===150);"
              "globalThis.pocSummary=r.every(Boolean)?'CANVAS-SIZE-OK':"
              "'CANVAS-SIZE:'+r})()",
              "<canvas-size-paths>", &result)
          && strcmp(result.summary, "CANVAS-SIZE-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Inserting a subtree classifies its resources from the attributes of the
   elements that can load one; other elements need no attribute lookups.
   The classification must still see images, posters and stylesheets, and
   still ignore a non-stylesheet link. */
static int test_inserted_subtree_resource_classification(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><head></head><body><div id=host></div></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 6u * MIB, 4000, "https://classify.test/", NULL,
        &result);
    CHECK(runtime != NULL);
    static const struct {
        const char *markup;
        const char *parent;
        bool image_scan;
        bool rebuild;
    } cases[] = {
        { "<div><p class=a data-x=1>text <b title=t>b</b></p></div>",
          "host", false, false },
        { "<p><span><img src=a.png alt=x></span></p>", "host", true, false },
        { "<section><video poster=p.png></video></section>", "host",
          true, false },
        { "<link rel=stylesheet href=s.css>", "head", false, true },
        { "<link rel=preload href=s.js as=script>", "head", false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&runtime->bridge.mutations, 0,
               sizeof(runtime->bridge.mutations));
        char script[512];
        snprintf(script, sizeof(script),
                 "(()=>{const t=document.createElement('template');"
                 "t.innerHTML='%s';const n=document.importNode("
                 "t.content.firstChild,true);(%s).appendChild(n);"
                 "globalThis.pocSummary='inserted';})()",
                 cases[i].markup,
                 strcmp(cases[i].parent, "head") == 0
                     ? "document.head" : "document.getElementById('host')");
        CHECK(script_runtime_evaluate_diagnostic(
                  runtime, script, "<classify>", &result)
              && strcmp(result.summary, "inserted") == 0);
        const ScriptMutationJournal *journal = &runtime->bridge.mutations;
        if (journal->image_resource_scan_required != cases[i].image_scan
            || journal->resource_rebuild_required != cases[i].rebuild) {
            fprintf(stderr, "classification case %zu: scan=%d rebuild=%d\n",
                    i, journal->image_resource_scan_required,
                    journal->resource_rebuild_required);
        }
        CHECK(journal->image_resource_scan_required == cases[i].image_scan
              && journal->resource_rebuild_required == cases[i].rebuild);
    }
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int check_external_compile_cycles(size_t source_bytes,
                                         size_t target_remaining,
                                         bool expect_collection)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://compile-pressure.test/", NULL, &result);
    CHECK(runtime != NULL);
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    /* Reserve only a fixed 768 KiB above bootstrap, then fill it with
       unreachable cycles. No site payload, timing assumptions, or heap raise
       in the production path: reference counting alone cannot free these. */
    runtime->base_memory_limit = live + 768u * 1024u;
    runtime->boot_window_active = false;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    JS_SetGCThreshold(runtime->runtime, SIZE_MAX);
    static const uint8_t bytes[4096] = {0};
    for (unsigned i = 0; i < 256
         && script_runtime_heap_remaining(runtime) > target_remaining; i++) {
        JSValue cycle = JS_NewObject(runtime->context);
        CHECK(!JS_IsException(cycle));
        CHECK(JS_SetPropertyStr(runtime->context, cycle, "self",
                  JS_DupValue(runtime->context, cycle)) >= 0);
        CHECK(JS_SetPropertyStr(runtime->context, cycle, "payload",
                  JS_NewArrayBufferCopy(runtime->context, bytes, sizeof(bytes))) >= 0);
        JS_FreeValue(runtime->context, cycle);
    }
    CHECK(script_runtime_heap_remaining(runtime) <= target_remaining);
    char source[32769];
    for (size_t i = 0; i < sizeof(source) - 1u; i += 8u)
        memcpy(source + i, "void 0; ", 8u);
    CHECK(source_bytes <= sizeof(source) - 1u && source_bytes % 8u == 0u);
    source[source_bytes] = '\0';
    bool admitted = false;
    size_t rejections = script_runtime_heap_rejections(runtime);
    size_t before = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    JSValue compiled = js_rt_compile_source_type(runtime->context, source,
        source_bytes, "https://compile-pressure.test/loader.js",
        JS_EVAL_TYPE_GLOBAL, SCRIPT_COMPILE_SOURCE_EXTERNAL, &result, &admitted);
    CHECK(admitted && !JS_IsException(compiled));
    CHECK(script_runtime_heap_rejections(runtime) == rejections);
    size_t after = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    printf("external-compile-pressure source=%zu headroom=%zu collect=%d "
           "owned-before=%zu owned-after=%zu\n", source_bytes,
           target_remaining, expect_collection, before, after);
    /* Dead cycles occupy at least 256 KiB. The small compilation must not
       collect them when it has ample room; the tight-heap case must reclaim
       them before parsing, without a failed allocation/retry. */
    CHECK(expect_collection ? after < before - 128u * 1024u : after >= before);
    JSValue evaluated = JS_EvalFunction(runtime->context, compiled);
    CHECK(!JS_IsException(evaluated));
    JS_FreeValue(runtime->context, evaluated);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_external_compile_reclaims_cycles(void)
{
    CHECK(check_external_compile_cycles(32768, 16384, true) == 0);
    CHECK(check_external_compile_cycles(256, 16384, true) == 0);
    CHECK(check_external_compile_cycles(256, 512u * 1024u, false) == 0);
    CHECK(check_external_compile_cycles(1424, 512u * 1024u, false) == 0);
    return 0;
}

static int test_gc_pacing_requires_heap_growth(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://compile-pressure.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = live + 768u * 1024u;
    runtime->boot_window_active = false;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    JS_SetGCThreshold(runtime->runtime, SIZE_MAX);
    static const uint8_t payload[600u * 1024u] = {0};
    JSValue retained = JS_NewArrayBufferCopy(runtime->context, payload, sizeof(payload));
    CHECK(!JS_IsException(retained));
    JSValue cycle = JS_NewObject(runtime->context);
    CHECK(!JS_IsException(cycle));
    CHECK(JS_SetPropertyStr(runtime->context, cycle, "self",
              JS_DupValue(runtime->context, cycle)) >= 0);
    CHECK(JS_SetPropertyStr(runtime->context, cycle, "payload",
              JS_NewArrayBufferCopy(runtime->context, payload, 64u * 1024u)) >= 0);
    JS_FreeValue(runtime->context, cycle);
    size_t before = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    CHECK(script_runtime_heap_remaining(runtime) < 128u * 1024u);
    /* The reserve is already occupied. Three almost idle advances must not
       lower the automatic threshold below the live graph and collect the
       same graph again on their first small object allocation. */
    for (unsigned i = 0; i < 3; i++) {
        CHECK(script_runtime_advance(runtime, 16, 4, &result));
        JSValue small = JS_NewObject(runtime->context);
        CHECK(!JS_IsException(small));
        JS_FreeValue(runtime->context, small);
    }
    size_t after = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    CHECK(after >= before - 16u * 1024u);
    /* Growth still crosses the paced threshold and reclaims the dead cycle;
       the fix must not disable automatic collection under real pressure. */
    JSValue growth = JS_NewArrayBufferCopy(runtime->context, payload, 64u * 1024u);
    CHECK(!JS_IsException(growth));
    JSValue trigger = JS_NewObject(runtime->context);
    CHECK(!JS_IsException(trigger));
    JS_FreeValue(runtime->context, trigger);
    CHECK(budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool)
          < before + 16u * 1024u);
    JS_FreeValue(runtime->context, growth);
    JS_FreeValue(runtime->context, retained);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static size_t growth_reclaim_calls;
static size_t growth_reclaim_count(void *opaque, size_t needed)
{
    (void) opaque;
    (void) needed;
    growth_reclaim_calls++;
    return 0;
}

/* Growth raises the limit by the shortfall the current headroom leaves,
   not by the whole request: 9 MiB live under a 10 MiB limit fits a 2 MiB
   request once the limit reaches 11 MiB. A request the ceiling cannot hold
   is refused without evicting optional caches. */
static int test_heap_growth_uses_existing_headroom(void)
{
    Budget budget;
    budget_init(&budget, 64u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://growth-headroom.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    runtime->boot_window_active = false;
    runtime->base_memory_limit = 10u * MIB;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    script_runtime_enable_heap_growth(runtime, 11u * MIB, 4u * MIB);
    CHECK(runtime->heap_growth_enabled);
    /* Allocation shape: live below the limit. */
    CHECK(js_rt_heap_growth_hook(runtime, 9u * MIB, 2u * MIB, 10u * MIB)
          == 11u * MIB);
    /* Reallocation shape: the pool passes only the size increase. */
    runtime->base_memory_limit = 10u * MIB;
    CHECK(js_rt_heap_growth_hook(runtime, 9u * MIB + 512u * 1024u, 1u * MIB,
                                 10u * MIB) == 10u * MIB + 512u * 1024u);
    /* Already within the limit: nothing to raise. */
    runtime->base_memory_limit = 10u * MIB;
    CHECK(js_rt_heap_growth_hook(runtime, 8u * MIB, 1u * MIB, 10u * MIB)
          == 10u * MIB);
    /* Past the ceiling: refused, and the caches are left alone. */
    runtime->base_memory_limit = 10u * MIB;
    growth_reclaim_calls = 0;
    budget_set_reclaim_hook(&budget, growth_reclaim_count, NULL);
    size_t refusals = runtime->heap_growth_refusals;
    CHECK(js_rt_heap_growth_hook(runtime, 9u * MIB, 3u * MIB, 10u * MIB)
          == 0u);
    CHECK(runtime->heap_growth_refusals == refusals + 1u
          && runtime->heap_growth_refused_reason == 2u
          && growth_reclaim_calls == 0u);
    budget_set_reclaim_hook(&budget, NULL, NULL);
    /* End to end: a buffer larger than the ceiling's margin but within
       the headroom plus that margin is admitted. */
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = live + 1536u * 1024u;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    script_runtime_enable_heap_growth(
        runtime, runtime->base_memory_limit + 1u * MIB, 4u * MIB);
    static const char big[] = "globalThis.__big=new ArrayBuffer(2097152);1";
    JSValue kept = JS_Eval(runtime->context, big, sizeof(big) - 1u,
                           "<growth-headroom>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(kept));
    JS_FreeValue(runtime->context, kept);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Attribute writes are counted always but timed only for the profiler: a
   realm without one reads no clock on this path. */
static int test_attribute_write_timing_needs_profiler(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body><p id=p>x</p></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://attribute-timing.test/", NULL, &result);
    CHECK(runtime != NULL && runtime->profile == NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "const p=document.getElementById('p');"
        "for(let i=0;i<400;i++)p.setAttribute('data-n',String(i))",
        "<attribute-timing>", &result));
    CHECK(runtime->bridge.attribute_writes >= 400u);
    CHECK(runtime->bridge.attribute_write_ns == 0u);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A settled realm whose growth is still in use collects for a return check
   at a backed-off cadence, not every 256 advances forever. */
static int test_heap_return_checks_back_off(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 2u * MIB, 4000,
        "https://return-backoff.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    runtime->boot_window_active = false;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    script_runtime_enable_heap_growth(runtime, 24u * MIB, 4u * MIB);
    /* Grow past the floor and keep it: nothing can be returned. */
    static const char keep[] = "globalThis.__keep=new ArrayBuffer(6291456);1";
    JSValue kept = JS_Eval(runtime->context, keep, sizeof(keep) - 1u,
                           "<return-backoff>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(kept));
    JS_FreeValue(runtime->context, kept);
    CHECK(runtime->base_memory_limit > runtime->heap_growth_floor);
    size_t collections = runtime->heap_collections;
    size_t returns = runtime->heap_growth_returns;
    for (unsigned turn = 0; turn < 4096u; turn++)
        CHECK(script_runtime_advance(runtime, 1, 4, &result));
    size_t checks = runtime->heap_collections - collections;
    if (checks > 5u)
        printf("return checks: %zu collections in 4096 advances\n", checks);
    CHECK(runtime->heap_growth_returns == returns);
    CHECK(checks >= 2u && checks <= 5u);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The boot-window return, collect-and-trim re-arm and the page's
   remaining-heap probe read the allocator-maintained count: a census on
   top of the check's collection lengthened a large settled heap's pause by
   half again. Both outcomes of the headroom rule hold with no census. */
static int test_heap_decisions_skip_census(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    CHECK(setenv("TILEFINCH_JS_BOOT_WINDOW_KB", "2048", 1) == 0);
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 6u * MIB, 4000,
        "https://boot-census.test/", NULL, &result);
    CHECK(unsetenv("TILEFINCH_JS_BOOT_WINDOW_KB") == 0);
    CHECK(runtime != NULL && runtime->boot_window_active);
    size_t censuses = runtime->heap_censuses;
    size_t base = runtime->base_memory_limit;
    size_t reserve = base / 8u;
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    CHECK(live + reserve + 256u * 1024u < base);
    /* Retain just past what fits the base with its reserve. */
    char keep[96];
    snprintf(keep, sizeof(keep), "globalThis.__keep=new ArrayBuffer(%zu);1",
             base - reserve - live + 256u * 1024u);
    JSValue kept = JS_Eval(runtime->context, keep, strlen(keep),
                           "<boot-census>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(kept));
    JS_FreeValue(runtime->context, kept);
    for (unsigned turn = 0; turn < 1024u && runtime->boot_window_checks == 0;
         turn++)
        CHECK(script_runtime_advance(runtime, 1, 4, &result));
    CHECK(runtime->boot_window_checks == 1u && runtime->boot_window_active);
    CHECK(budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool)
          + reserve > base);
    static const char drop[] = "globalThis.__keep=null;1";
    JSValue dropped = JS_Eval(runtime->context, drop, sizeof(drop) - 1u,
                              "<boot-census>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(dropped));
    JS_FreeValue(runtime->context, dropped);
    for (unsigned turn = 0; turn < 2048u && runtime->boot_window_active;
         turn++)
        CHECK(script_runtime_advance(runtime, 1, 4, &result));
    CHECK(!runtime->boot_window_active
          && runtime->boot_window_checks == 2u
          && runtime->boot_window_returned_advance != 0);
    CHECK(budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool)
          + reserve <= base);
    (void) script_runtime_collect_and_trim(runtime);
    static const char probe[] = "__tilefinchHeapRemaining()";
    JSValue remaining = JS_Eval(runtime->context, probe, sizeof(probe) - 1u,
                                "<boot-census>", JS_EVAL_TYPE_GLOBAL);
    int64_t remaining_bytes = 0;
    CHECK(JS_ToInt64(runtime->context, &remaining_bytes, remaining) == 0);
    JS_FreeValue(runtime->context, remaining);
    CHECK(remaining_bytes > 0 && (size_t) remaining_bytes < base);
    CHECK(runtime->heap_censuses == censuses);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A growable realm whose limit sits a sliver above its live graph (where
   give-back used to leave it) must regain collection headroom from spare
   page Budget, so garbage-heavy work stops collecting the whole heap every
   few hundred KiB; with no spare Budget above the reserve it must not. */
static int test_gc_pacing_pregrows_growable_heap(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://gc-headroom.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    runtime->boot_window_active = false;
    script_runtime_enable_heap_growth(runtime, 24u * MIB, 4u * MIB);
    CHECK(runtime->heap_growth_enabled);
    /* A multi-MiB live graph, like a hydrated application's. */
    static const char keep[] = "globalThis.__keep = new ArrayBuffer(4194304);";
    JSValue kept = JS_Eval(runtime->context, keep, sizeof(keep) - 1u,
                           "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(kept));
    JS_FreeValue(runtime->context, kept);
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = live + 768u * 1024u;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    CHECK(runtime->heap_growth_pregrows >= 1u);
    CHECK(runtime->base_memory_limit >= live + live / 2u);
    /* Garbage below the 25% pacing share: no collection. */
    size_t collections = runtime->heap_collections;
    static const uint8_t payload[64u * 1024u] = {0};
    for (size_t made = 0; made + sizeof(payload) < live / 5u;
         made += sizeof(payload)) {
        JSValue garbage = JS_NewArrayBufferCopy(
            runtime->context, payload, sizeof(payload));
        CHECK(!JS_IsException(garbage));
        JS_FreeValue(runtime->context, garbage);
        JSValue cycle = JS_NewObject(runtime->context);
        CHECK(!JS_IsException(cycle));
        CHECK(JS_SetPropertyStr(runtime->context, cycle, "self",
                  JS_DupValue(runtime->context, cycle)) >= 0);
        JS_FreeValue(runtime->context, cycle);
    }
    CHECK(runtime->heap_collections == collections);
    /* Under pressure the same state stays tight. */
    JS_RunGC(runtime->runtime);
    live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = live + 768u * 1024u;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    size_t spare = budget_remaining(&budget) - 4u * MIB + 256u * 1024u;
    void *occupied = budget_malloc(&budget, spare);
    CHECK(occupied != NULL);
    size_t pregrows = runtime->heap_growth_pregrows;
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    CHECK(runtime->heap_growth_pregrows == pregrows);
    CHECK(runtime->base_memory_limit == live + 768u * 1024u);
    budget_free(&budget, occupied);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Collection thrash near the heap limit (xe.com on the PSP: 165 full
   collections, 25.6 s of one 27.7 s task, each marking ~12 MB that was
   almost all live). One realm per case: `live` MiB retained, its limit a
   sliver above that, automatic collection paced as the PSP app paces it.
   TILEFINCH_JS_GC_PACING=0 restores the old pacing; these fail there. */
typedef struct {
    Budget budget;
    PocDocument document;
    ScriptResult result;
    ScriptRuntime *runtime;
    size_t live;
} GcThrashRealm;

static int gc_thrash_open(GcThrashRealm *realm, size_t keep_bytes,
                          size_t headroom, size_t ceiling_above_live)
{
    memset(realm, 0, sizeof(*realm));
    budget_init(&realm->budget, 32u * MIB);
    CHECK(budget_install_lexbor(&realm->budget));
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&realm->document, &realm->budget, html,
                         sizeof(html) - 1u, 17));
    realm->runtime = script_runtime_create_with_session(
        &realm->document, &realm->budget, 5u * MIB, 60000,
        "https://gc-thrash.test/", NULL, &realm->result);
    ScriptRuntime *runtime = realm->runtime;
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &realm->result));
    runtime->boot_window_active = false;
    runtime->base_memory_limit = 16u * MIB;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    char keep[96];
    snprintf(keep, sizeof(keep), "globalThis.__keep=new ArrayBuffer(%zu);1",
             keep_bytes);
    JSValue kept = JS_Eval(runtime->context, keep, strlen(keep),
                           "<gc-thrash>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(kept));
    JS_FreeValue(runtime->context, kept);
    JS_RunGC(runtime->runtime);
    realm->live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = realm->live + headroom;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    if (ceiling_above_live != 0)
        script_runtime_enable_heap_growth(
            runtime, realm->live + ceiling_above_live, 4u * MIB);
    /* Mid-task, as on the device: an advance's pre-growth is not coming,
       and QuickJS has re-armed at half the headroom. */
    CHECK(script_runtime_advance(runtime, 16, 4, &realm->result));
    runtime->base_memory_limit = realm->live + headroom;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    JS_SetGCThreshold(runtime->runtime, realm->live + headroom / 2u);
    return 0;
}

static int gc_thrash_close(GcThrashRealm *realm)
{
    script_runtime_destroy(realm->runtime);
    document_destroy(&realm->document);
    CHECK(realm->budget.current == 0
          && budget_uninstall_lexbor(&realm->budget));
    return 0;
}

/* `source` runs as one task; returns its collections, *threw = exception. */
static size_t gc_thrash_run(GcThrashRealm *realm, const char *source,
                            bool *threw, char *message, size_t message_size)
{
    ScriptRuntime *runtime = realm->runtime;
    size_t before = runtime->heap_collections;
    JSValue value = JS_Eval(runtime->context, source, strlen(source),
                            "<gc-thrash>", JS_EVAL_TYPE_GLOBAL);
    *threw = JS_IsException(value);
    if (message_size != 0) message[0] = '\0';
    if (*threw) {
        JSValue error = JS_GetException(runtime->context);
        const char *text = JS_ToCString(runtime->context, error);
        if (text != NULL && message_size != 0)
            snprintf(message, message_size, "%s", text);
        if (text != NULL) JS_FreeCString(runtime->context, text);
        JS_FreeValue(runtime->context, error);
    }
    JS_FreeValue(runtime->context, value);
    return runtime->heap_collections - before;
}

/* Live data grows toward the ceiling with the page Budget to spare: the
   limit used to rise 512 KiB at a time on demand, with QuickJS halving its
   headroom to each step (about a dozen full collections per step). Past
   the ceiling the realm must still run out of memory promptly. */
static int test_gc_thrash_live_growth_reaches_refusal(void)
{
    GcThrashRealm realm;
    CHECK(gc_thrash_open(&realm, 4u * MIB, 768u * 1024u, 6u * MIB) == 0);
    bool threw = false;
    char message[128];
    size_t collections = gc_thrash_run(&realm,
        "const k=[];for(let i=0;;i++)k.push({i:i,s:'x'+i});",
        &threw, message, sizeof(message));
    ScriptRuntime *runtime = realm.runtime;
    printf("gc-thrash live-growth: live=%zu collections=%zu limit=%zu "
           "refusals=%zu reason=%u error=\"%s\"\n", realm.live,
           collections, runtime->base_memory_limit,
           runtime->heap_growth_refusals,
           (unsigned) runtime->heap_growth_refused_reason, message);
    /* QuickJS throws null when even the error object does not fit. */
    CHECK(threw && (message[0] == '\0' || strcmp(message, "null") == 0
                    || strstr(message, "out of memory") != NULL));
    CHECK(runtime->heap_growth_refusals >= 1u
          && runtime->heap_growth_refused_reason == 2u);
    CHECK(runtime->base_memory_limit <= realm.live + 6u * MIB);
    CHECK(budget_remaining(&realm.budget) >= 4u * MIB);
    CHECK(collections <= 24u);
    return gc_thrash_close(&realm);
}

/* About 50 MB of cyclic garbage over ~6 MiB live, in one task, 768 KiB
   below a limit that could grow. Collections that free garbage are kept
   (that is what holds churn under the limit): the garbage must not buy
   itself more limit, and the page reserve holds. */
static int test_gc_thrash_garbage_churn_stays_under_limit(void)
{
    GcThrashRealm realm;
    CHECK(gc_thrash_open(&realm, 4u * MIB, 768u * 1024u, 20u * MIB) == 0);
    bool threw = false;
    char message[128];
    size_t collections = gc_thrash_run(&realm,
        "for(let i=0;i<56000;i++){const o={a:new Array(32).fill(i)};"
        "o.self=o;}1", &threw, message, sizeof(message));
    ScriptRuntime *runtime = realm.runtime;
    printf("gc-thrash churn: live=%zu collections=%zu limit=%zu "
           "error=\"%s\"\n", realm.live, collections,
           runtime->base_memory_limit, message);
    CHECK(!threw && collections >= 4u);
    CHECK(runtime->base_memory_limit <= realm.live + 768u * 1024u);
    CHECK(budget_remaining(&realm.budget) >= 4u * MIB);
    return gc_thrash_close(&realm);
}

/* The same churn in a realm that cannot grow, its headroom below an
   amortized step: each collection frees what the churn left, so this is
   progress, not exhaustion. It must finish, never backing off into a
   refusal. */
static int test_gc_thrash_starved_churn_completes(void)
{
    GcThrashRealm realm;
    CHECK(gc_thrash_open(&realm, 4u * MIB, 384u * 1024u, 0) == 0);
    bool threw = false;
    char message[128];
    size_t collections = gc_thrash_run(&realm,
        "for(let i=0;i<14000;i++){const o={a:new Array(32).fill(i)};"
        "o.self=o;}1", &threw, message, sizeof(message));
    ScriptRuntime *runtime = realm.runtime;
    printf("gc-thrash starved churn: collections=%zu error=\"%s\"\n",
           collections, message);
    CHECK(!threw);
    CHECK(collections >= 1u);
    return gc_thrash_close(&realm);
}

/* External scripts compiled near the current limit, with growth to spare:
   the compile-pressure collection ran before every one (forty full
   collections that free nothing); now it waits until it is due. */
static int test_gc_thrash_compile_pressure_amortized(void)
{
    GcThrashRealm realm;
    CHECK(gc_thrash_open(&realm, 4u * MIB, 256u * 1024u, 20u * MIB) == 0);
    ScriptRuntime *runtime = realm.runtime;
    size_t length = 64u * 1024u;
    char *source = malloc(length + 64u);
    CHECK(source != NULL);
    memcpy(source, "/*", 2u);
    memset(source + 2u, 'x', length - 2u);
    int tail = snprintf(source + length, 64u,
                        "*/globalThis.__n=(globalThis.__n|0)+1;");
    size_t total = length + (size_t) tail;
    size_t before = runtime->heap_collections;
    for (unsigned i = 0; i < 40u; i++) {
        ScriptResult result = {0};
        bool admitted = false;
        JSValue compiled = js_rt_compile_source_type(
            runtime->context, source, total,
            "https://gc-thrash.test/chunk.js", JS_EVAL_TYPE_GLOBAL,
            SCRIPT_COMPILE_SOURCE_EXTERNAL, &result, &admitted);
        CHECK(admitted && !JS_IsException(compiled));
        JSValue ran = JS_EvalFunction(runtime->context, compiled);
        CHECK(!JS_IsException(ran));
        JS_FreeValue(runtime->context, ran);
    }
    free(source);
    size_t collections = runtime->heap_collections - before;
    printf("gc-thrash compile pressure: compiles=40 collections=%zu\n",
           collections);
    CHECK(collections <= 8u);
    return gc_thrash_close(&realm);
}

/* Realloc-only appends do not enter the object-allocation GC trigger. A
   live array a few bytes below its limit must grow on demand, not repeatedly
   collect the unchanged graph before each small capacity increase. */
static JSValue gc_array_pin_heap(JSContext *context, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    ScriptRuntime *runtime = bridge->host;
    JSValue global = JS_GetGlobalObject(context);
    JSValue array = JS_GetPropertyStr(context, global, "__array");
    JSValue item = JS_GetPropertyStr(context, global, "__item");
    JSValue length = JS_GetPropertyStr(context, array, "length");
    uint32_t count = 0, capacity = JS_GetFastArrayCapacityForTest(array);
    int okay = JS_ToUint32(context, &count, length);
    JS_FreeValue(context, length);
    for (uint32_t i = count; okay >= 0 && i < capacity; i++)
        okay = JS_SetPropertyUint32(context, array, i, JS_DupValue(context, item));
    JS_FreeValue(context, item);
    JS_FreeValue(context, array);
    JS_FreeValue(context, global);
    if (okay < 0) return JS_EXCEPTION;
    JS_RunGC(runtime->runtime);
    size_t live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = live + 64u;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    script_runtime_enable_heap_growth(runtime, live + 2u * MIB, 4u * MIB);
    if (argc != 0 && !JS_ToBool(context, argv[0]))
        runtime->heap_growth_enabled = false;
    JS_SetGCThreshold(runtime->runtime, live + 32u);
    return JS_UNDEFINED;
}

static int test_gc_thrash_array_growth_amortized(void)
{
    GcThrashRealm realm;
    CHECK(gc_thrash_open(&realm, 4u * MIB, 768u * 1024u, 6u * MIB) == 0);
    ScriptRuntime *runtime = realm.runtime;
    runtime->base_memory_limit = 16u * MIB;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    JSValue global = JS_GetGlobalObject(runtime->context);
    CHECK(JS_SetPropertyStr(runtime->context, global, "__pinArrayHeap",
        JS_NewCFunction(runtime->context, gc_array_pin_heap, "pinArrayHeap", 0)) >= 0);
    JS_FreeValue(runtime->context, global);
    JSValue value = JS_Eval(runtime->context,
        "globalThis.__item={n:1};globalThis.__array=new Array(10000).fill(__item);",
        strlen("globalThis.__item={n:1};globalThis.__array=new Array(10000).fill(__item);"),
        "<gc-array-setup>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(runtime->context, value);
    JS_RunGC(runtime->runtime);
    realm.live = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    runtime->base_memory_limit = realm.live + 64u;
    JS_SetMemoryLimit(runtime->runtime, runtime->base_memory_limit);
    script_runtime_enable_heap_growth(runtime, realm.live + 2u * MIB, 4u * MIB);
    JS_SetGCThreshold(runtime->runtime, realm.live + 32u);
    bool threw = false;
    char message[128];
    size_t collections = gc_thrash_run(&realm,
        "__pinArrayHeap();for(let i=0;i<40000;i++)__array.push(__item);__array.length",
        &threw, message, sizeof(message));
    printf("gc-thrash array-growth: collections=%zu limit=%zu error=\"%s\"\n",
        collections, runtime->base_memory_limit, message);
    CHECK(!threw && collections <= 4u);
    CHECK(runtime->base_memory_limit <= realm.live + 3u * MIB);
    CHECK(budget_remaining(&realm.budget) >= 4u * MIB);
    collections = gc_thrash_run(&realm,
        "__array=new Array(10000).fill(__item);__pinArrayHeap(false);"
        "for(let i=0;i<100;i++){try{__array.push(__item)}catch(e){}}",
        &threw, message, sizeof(message));
    printf("gc-thrash array-refusal: collections=%zu error=\"%s\"\n",
        collections, message);
    CHECK(!threw && collections <= 24u);
    return gc_thrash_close(&realm);
}

static int test_gc_thrash(void)
{
    /* Every case runs (and prints its counts) even after a failure. */
    int failed = test_gc_thrash_live_growth_reaches_refusal();
    failed |= test_gc_thrash_garbage_churn_stays_under_limit();
    failed |= test_gc_thrash_starved_churn_completes();
    failed |= test_gc_thrash_compile_pressure_amortized();
    failed |= test_gc_thrash_array_growth_amortized();
    return failed;
}

static int test_dom_wrapper_receiver_sharing(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    char html[4096] = "<!doctype html><body>";
    size_t length = strlen(html);
    for (unsigned i = 0; i < 48; i++)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
                                   "<p id=p%u>Item</p>", i);
    CHECK(document_parse(&document, &budget, html, length, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://wrapper-pressure.test/", NULL, &result);
    CHECK(runtime != NULL);
    /* A forged receiver can retire its detached node from the intermediate
       parent property lookup. Native getters must not retain that raw node
       across author code, nor resolve a different handle on a second read. */
    static const char retired_parent_receiver[] =
        "(()=>{function getterFor(node,name){for(let p=node,steps=0;"
        "p&&steps<16;p=Object.getPrototypeOf(p),steps++){"
        "const d=Object.getOwnPropertyDescriptor(p,name);if(d)return d.get}"
        "throw Error('missing getter')}"
        "for(const name of ['parentNode','parentElement']){"
        "const n=document.createElement('i'),h=n.__handle,"
        "lease=n.__tilefinchHandleLease;let released=false,reads=0;"
        "const receiver={get __handle(){reads++;return h}};"
        "Object.defineProperty(receiver,'__tilefinchDetachedParent',{get(){"
        "if(!released)released=__tilefinchReleaseNodeWrapper(h,lease);"
        "return null}});const getter=getterFor(n,name);"
        "if(getter.call(receiver)!==null||!released||reads<1)"
        "throw Error('retired '+name);"
        "if(__tilefinchReleaseNodeWrapper(h,lease))throw Error('stale lease');}"
        "const child=document.createElement('i'),parent=document.createElement('b');"
        "parent.append(child);if(child.parentNode!==parent||"
        "child.parentElement!==parent)throw Error('ordinary parent');"
        "const sentinel=Error('parent accessor'),receiver={__handle:child.__handle};"
        "Object.defineProperty(receiver,'__tilefinchDetachedParent',"
        "{get(){throw sentinel}});let caught=false;try{"
        "getterFor(child,'parentNode').call(receiver)"
        "}catch(e){caught=e===sentinel}if(!caught)throw Error('exception lost');"
        "globalThis.pocSummary='REENTRANT-PARENT-SAFE';})()";
    CHECK(script_runtime_evaluate_diagnostic(runtime, retired_parent_receiver,
        "<reentrant-parent-receiver>", &result)
        && strcmp(result.summary, "REENTRANT-PARENT-SAFE") == 0);
    size_t before = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    JSMemoryUsage wrapper_before, wrapper_after;
    JS_ComputeMemoryUsage(runtime->runtime, &wrapper_before);
    static const char script[] =
        "(()=>{const register=FinalizationRegistry.prototype.register,"
        "unregister=FinalizationRegistry.prototype.unregister;"
        "FinalizationRegistry.prototype.register=function(){"
        "throw Error('private wrapper record exposed')};"
        "FinalizationRegistry.prototype.unregister=function(){"
        "throw Error('private wrapper token exposed')};try{"
        "globalThis.finalizerPrivacyProbe=document.createElement('i');"
        "if(!finalizerPrivacyProbe)throw Error('missing fresh wrapper');"
        "globalThis.wrapperProbe=document.querySelectorAll('p');"
        "for(const n of wrapperProbe)void n.classList;"
        "(()=>{const a=wrapperProbe[0],b=wrapperProbe[1];"
        /* <p> wrappers share HTMLParagraphElement.prototype, which sits
           above the native layer holding the receiver descriptors. */
        "const inherited=(o,k)=>{for(let at=Object.getPrototypeOf(o);at;"
        "at=Object.getPrototypeOf(at)){const d=Object.getOwnPropertyDescriptor"
        "(at,k);if(d)return d}},x=inherited(a,'id'),y=inherited(b,'id');"
        "if(Object.getPrototypeOf(a)!==Object.getPrototypeOf(b)||"
        "Object.prototype.hasOwnProperty.call(a,'id'))throw Error('unshared prototype');"
        "if(wrapperProbe.length!==48||x.get!==y.get||x.set!==y.set||"
        "!x.enumerable||!x.configurable||a.hasAttribute!==b.hasAttribute)"
        "throw Error('unshared receiver-only descriptors');"
        "const ca=Object.getOwnPropertyDescriptor(a,'classList'),"
        "cb=Object.getOwnPropertyDescriptor(b,'classList');"
        "if(ca.get!==cb.get||ca.set!==cb.set)throw Error('unshared classList');"
        "if(a.classList.add!==b.classList.add||a.classList.item!==b.classList.item)"
        "throw Error('unshared token-list methods');"
        "a.classList.add.call(b.classList,'borrowed');"
        "if(!b.classList.contains('borrowed')||a.classList.contains('borrowed'))"
        "throw Error('token-list receiver');"
        "b.classList.remove('borrowed');"
        "var badReceiver=false;try{a.classList.add.call({},'bad')}"
        "catch(e){badReceiver=e instanceof TypeError}"
        "if(!badReceiver)throw Error('token-list brand');"
        "for(let i=0;i<wrapperProbe.length;i++){const t=wrapperProbe[i].classList;"
        "t.add('stable');if(t[0]!=='stable'||!(0 in t)||t.item(1)!==null"
        "||Array.from(t).join(' ')!=='stable')throw Error('token-list indexing');"
        "try{t.add('must-not-write','bad token')}catch(e){}"
        "if(t.contains('must-not-write'))throw Error('token-list atomic validation');"
        "t.remove('stable')}"
        "const list=a.classList;list.add('one');b.classList.add('two');"
        "if(a.classList!==list||!list.contains('one')||list.contains('two')"
        "||ca.get.call(b)!==b.classList)throw Error('classList receiver identity');"
        "a.id='first';b.id='second';a.hidden=true;"
        "if(a.id!=='first'||b.id!=='second'||!a.hidden||b.hidden)"
        "throw Error('cross-wrapper attribute state');"
        "let aa=0,bb=0;a.addEventListener('x',()=>aa++);"
        "if(a.addEventListener!==b.addEventListener||"
        "a.removeEventListener!==b.removeEventListener)throw Error('event methods');"
        "b.addEventListener('x',()=>bb++);a.dispatchEvent(new Event('x'));"
        "if(aa!==1||bb!==0)throw Error('cross-wrapper listeners');"
        "b.remove();if(!a.isConnected||b.isConnected||b.id!=='second')"
        "throw Error('detached wrapper state');"
        "Object.defineProperty(a,'id',{value:'own',configurable:true});"
        "if(a.id!=='own'||b.id!=='second')throw Error('shared override');"
        "globalThis.pocSummary='DOM-WRAPPERS-SHARED';})()"
        "}finally{FinalizationRegistry.prototype.register=register;"
        "FinalizationRegistry.prototype.unregister=unregister}})()";
    bool evaluated = script_runtime_evaluate_diagnostic(runtime, script,
        "<wrapper-pressure>", &result);
    size_t after = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    JS_ComputeMemoryUsage(runtime->runtime, &wrapper_after);
    printf("dom-wrapper-pressure wrappers=48 owned-before=%zu owned-after=%zu objects=%lld\n",
        before, after, (long long)(wrapper_after.obj_count - wrapper_before.obj_count));
    if (!evaluated) fprintf(stderr, "wrapper-pressure error=%s\n", result.error);
    CHECK(evaluated && strcmp(result.summary, "DOM-WRAPPERS-SHARED") == 0);
    CHECK(after >= before && after - before < 512u * 1024u);
    /* Parsed nodes can already have wrappers from bootstrap discovery. Use
       fresh strongly retained nodes to isolate cache bookkeeping growth. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.wrapperCachePressure=[document.createElement('i')];",
        "<wrapper-cache-warmup>", &result));
    JS_ComputeMemoryUsage(runtime->runtime, &wrapper_before);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "for(let i=0;i<48;i++)wrapperCachePressure.push(document.createElement('i'));",
        "<wrapper-cache-growth>", &result));
    JS_ComputeMemoryUsage(runtime->runtime, &wrapper_after);
    printf("dom-wrapper-cache fresh=48 objects=%lld bytes=%lld\n",
        (long long)(wrapper_after.obj_count - wrapper_before.obj_count),
        (long long)(wrapper_after.malloc_size - wrapper_before.malloc_size));
    /* Five objects per fresh wrapper with the shared record, versus seven
       before; leave one object's margin for unrelated receiver metadata. */
    CHECK(wrapper_after.obj_count - wrapper_before.obj_count <= 6 * 48);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_dom_order_without_sibling_wrappers(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    char html[16384] = "<!doctype html><body><div id=order>";
    size_t length = strlen(html);
    for (unsigned i = 0; i < 400; ++i)
        length += (size_t) snprintf(html + length, sizeof(html) - length,
                                   "<p id=n%u>Text</p>", i);
    CHECK(document_parse(&document, &budget, html, length, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://node-order.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.orderParent=document.getElementById('order');"
        "globalThis.orderFirst=document.getElementById('n0');"
        "globalThis.orderLast=document.getElementById('n399');",
        "<order-setup>", &result));
    size_t slots = runtime->bridge.node_count;
    size_t before = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "for(let i=0;i<32;i++){"
        "if(orderFirst.compareDocumentPosition(orderLast)!==4||"
        "orderLast.compareDocumentPosition(orderFirst)!==2||"
        "orderParent.compareDocumentPosition(orderLast)!==20||"
        "orderLast.compareDocumentPosition(orderParent)!==10)"
        "throw Error('native tree order');}"
        "pocSummary='NATIVE-ORDER-OK'",
        "<native-order>", &result));
    size_t after = budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    printf("dom-order slots=%zu->%zu heap=%zu->%zu\n", slots,
        runtime->bridge.node_count, before, after);
    CHECK(strcmp(result.summary, "NATIVE-ORDER-OK") == 0
          && runtime->bridge.node_count == slots
          && after < before + 32u * 1024u);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "orderParent.insertBefore(orderLast,orderFirst);"
        "if(orderFirst.compareDocumentPosition(orderLast)!==2||"
        "orderLast.compareDocumentPosition(orderFirst)!==4)throw Error('stale order');"
        "const detached=document.createElement('div');detached.append(orderFirst);"
        "const a=orderFirst.compareDocumentPosition(orderLast),"
        "b=orderLast.compareDocumentPosition(orderFirst);"
        "if((a&33)!==33||(b&33)!==33||(a&6)===(b&6))throw Error('disconnected order');",
        "<mutated-order>", &result));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static TimedCooperate *task_slice_clock;

static JSValue consume_task_slice(JSContext *context, JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void) context; (void) this_value; (void) argc; (void) argv;
    task_slice_clock->now_ns += UINT64_C(10000000);
    return JS_UNDEFINED;
}

static int test_runtime_task_time_slice(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://task-slice.test/", NULL, &result);
    CHECK(runtime != NULL);
    runtime->bridge.execution_policy.maximum_advance_time_us = 8000;
    JSValue global = JS_GetGlobalObject(runtime->context);
    CHECK(JS_SetPropertyStr(runtime->context, global, "consumeTaskSlice",
        JS_NewCFunction(runtime->context, consume_task_slice, "consumeTaskSlice", 0)) >= 0);
    JS_FreeValue(runtime->context, global);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.taskOrder='';for(let i=0;i<3;i++)setTimeout(()=>{"
        "consumeTaskSlice();taskOrder+=i;Promise.resolve().then(()=>taskOrder+='m');},0)",
        "<task-slice>", &result));
    TimedCooperate clock = { .now_ns = UINT64_C(1000000000) };
    task_slice_clock = &clock;
    TilefinchPlatformServices services = {
        .context = &clock, .monotonic_time_ns = timed_cooperate_clock,
        .cooperate = timed_cooperate_poll
    };
    tilefinch_platform_set_services(&services);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(script_runtime_advance(runtime, 16, 16, &result));
        JSValue value = JS_Eval(runtime->context, "taskOrder", 9, "<check>", 0);
        const char *order = JS_ToCString(runtime->context, value);
        static const char *expected[] = { "0m", "0m1m", "0m1m2m" };
        CHECK(order != NULL && strcmp(order, expected[i]) == 0);
        JS_FreeCString(runtime->context, order);
        JS_FreeValue(runtime->context, value);
    }
    tilefinch_platform_set_services(NULL);
    task_slice_clock = NULL;
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static bool runtime_string_is(ScriptRuntime *runtime, const char *expression,
                              const char *expected)
{
    JSValue value = JS_Eval(runtime->context, expression, strlen(expression),
                            "<check>", 0);
    const char *text = JS_ToCString(runtime->context, value);
    bool equal = text != NULL && strcmp(text, expected) == 0;
    if (!equal) fprintf(stderr, "%s = %s\n", expression, text ? text : "?");
    JS_FreeCString(runtime->context, text);
    JS_FreeValue(runtime->context, value);
    return equal;
}

/* theguardian.com creates one IntersectionObserver per lazy island (75);
   the old 64-observer cap threw. The bound is page-wide registrations,
   which is what each update re-evaluates, and an update step evaluates a
   bounded slice so many targets never make one long step. */
static int test_resize_observer_registration_budget(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    const char html[] = "<!doctype html><body></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://observer-budget.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{for(let i=0;i<300;i++)new ResizeObserver(()=>{});"
        "let delivered=0;const active=[];for(let i=0;i<100;i++){"
        "const target=document.createElement('div');document.body.append(target);"
        "const observer=new ResizeObserver(entries=>delivered+=entries.length);"
        "observer.observe(target);active.push([observer,target])}"
        "globalThis.roBudget={active,get delivered(){return delivered}}})()",
        "<resize-budget>", &result));
    CHECK(script_runtime_advance(runtime, 16, 32, &result));
    CHECK(runtime_string_is(runtime, "String(roBudget.delivered)", "100"));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{for(const [observer,target] of roBudget.active)observer.unobserve(target);"
        "const target=roBudget.active[0][1],all=[];"
        "let refused=false;try{for(let i=0;i<1100;i++)"
        "{const observer=new ResizeObserver(()=>{});all.push(observer);"
        "observer.observe(target)}}catch(e){"
        "refused=e instanceof RangeError&&/target limit/.test(e.message)}"
        "if(!refused)throw Error('unbounded registrations');"
        "for(const observer of all)observer.disconnect();"
        "for(let i=0;i<1023;i++)all[i].observe(target);"
        "const inner=new ResizeObserver(()=>{}),outer=new ResizeObserver(()=>{});"
        "let reentrantRefused=false;try{outer.observe(target,{get box(){"
        "inner.observe(target);return 'content-box'}})}catch(e){"
        "reentrantRefused=e instanceof RangeError}"
        "inner.disconnect();outer.observe(target);outer.disconnect();"
        "globalThis.pocSummary=reentrantRefused?'RESIZE-BOUNDED':'RESIZE-UNBOUNDED'})()",
        "<resize-budget-refusal>", &result));
    CHECK(strcmp(result.summary, "RESIZE-BOUNDED") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget_uninstall_lexbor(&budget) && budget.current == 0);
    return 0;
}

static int test_intersection_observer_registration_budget(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://io-budget.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.io={initial:0,slice:-1,refused:false,again:false,"
        "calls:0,afterScroll:-1,error:''};"
        "const els=[];for(let i=0;i<1100;i++){const d=document."
        "createElement('div');document.body.appendChild(d);els.push(d)}"
        "const proto=Object.getPrototypeOf(els[0]),"
        "rect=proto.getBoundingClientRect;"
        "proto.getBoundingClientRect=function(){io.calls++;"
        "return rect.call(this)};"
        "try{for(let i=0;i<200;i++)new IntersectionObserver(entries=>{"
        "io.initial+=entries.length}).observe(els[i]);"
        "const big=new IntersectionObserver(()=>{});"
        "try{for(let i=200;i<1100;i++)big.observe(els[i])}"
        "catch(error){io.refused=error instanceof RangeError&&"
        "/target limit/.test(error.message)}"
        "big.disconnect();const late=new IntersectionObserver(()=>{});"
        "late.observe(els[1099]);io.again=true;late.disconnect()}"
        "catch(error){io.error=String(error)}"
        "Promise.resolve().then(()=>{io.slice=io.calls})",
        "<io-budget>", &result));
    /* The first step ran in the microtask checkpoint: one slice only. */
    CHECK(runtime_string_is(runtime, "JSON.stringify([io.error,io.refused,"
                            "io.again,io.slice])",
                            "[\"\",true,true,32]"));
    for (unsigned i = 0; i < 12; i++)
        CHECK(script_runtime_advance(runtime, 16, 16, &result));
    /* Every first observation arrived, one per observer. */
    CHECK(runtime_string_is(runtime, "String(io.initial)", "200"));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "io.calls=0;dispatchEvent(new Event('scroll'));"
        "Promise.resolve().then(()=>{io.afterScroll=io.calls})",
        "<io-scroll>", &result));
    CHECK(runtime_string_is(runtime, "String(io.afterScroll)", "32"));
    for (unsigned i = 0; i < 12; i++)
        CHECK(script_runtime_advance(runtime, 16, 16, &result));
    /* The pass finished over later tasks: every registration once. */
    CHECK(runtime_string_is(runtime, "String(io.calls)", "200"));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* What a frame loop may start a turn for without waiting for vblank: a due
   task, a microtask checkpoint left pending, never a frame callback. */
static int test_task_runnable_follows_the_event_loop(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://task-runnable.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_advance(runtime, 16, 4, &result));
    ScriptRunnableState state;
    unsigned frame_delay_ms = 99;
    CHECK(!script_runtime_task_runnable(runtime));
    CHECK(!script_runtime_frame_clock_delay_ms(runtime, &frame_delay_ms));
    CHECK(script_runtime_runnable_state(runtime, &state) && state.timers == 0);
    /* An animation frame is not runnable before or after its due time: it
       waits for a rendering opportunity. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.frames=0;requestAnimationFrame(()=>frames++)",
        "<raf>", &result));
    CHECK(script_runtime_frame_clock_delay_ms(runtime, &frame_delay_ms)
          && frame_delay_ms == 16);
    CHECK(!script_runtime_task_runnable(runtime));
    CHECK(script_runtime_advance(runtime, 20, 0, &result));
    CHECK(script_runtime_runnable_state(runtime, &state));
    CHECK(state.timers == 1 && state.timer_known
          && state.timer_due_in_us <= 0 && state.timer_frame_callback);
    CHECK(script_runtime_frame_clock_delay_ms(runtime, &frame_delay_ms)
          && frame_delay_ms == 0);
    CHECK(!script_runtime_task_runnable(runtime));
    /* A due timeout behind the due frame at the head is runnable. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.behind=0;setTimeout(()=>behind++,0)", "<behind>",
        &result));
    CHECK(script_runtime_runnable_state(runtime, &state));
    CHECK(state.timers == 2 && state.timer_frame_callback
          && state.task_known && state.task_due_in_us <= 0);
    CHECK(script_runtime_task_runnable(runtime));
    CHECK(script_runtime_advance(runtime, 0, 4, &result));
    CHECK(!script_runtime_task_runnable(runtime));
    /* A due timeout is. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "setTimeout(()=>{let p=Promise.resolve();"
        "for(let i=0;i<200;i++)p=p.then(()=>{})},0)",
        "<timeout>", &result));
    CHECK(script_runtime_task_runnable(runtime));
    CHECK(script_runtime_runnable_state(runtime, &state));
    CHECK(state.timer_known && state.timer_due_in_us <= 0
          && !state.timer_frame_callback && !state.jobs_pending);
    CHECK(!script_runtime_frame_clock_delay_ms(runtime, &frame_delay_ms));
    /* Its 200 chained jobs outlast one bounded checkpoint: the next turn
       has work before any timer is due. */
    CHECK(script_runtime_advance(runtime, 0, 4, &result));
    CHECK(script_runtime_runnable_state(runtime, &state));
    CHECK(state.timers == 0 && state.jobs_pending);
    CHECK(script_runtime_task_runnable(runtime));
    for (unsigned i = 0; i < 8 && script_runtime_task_runnable(runtime); i++)
        CHECK(script_runtime_advance(runtime, 0, 4, &result));
    CHECK(!script_runtime_task_runnable(runtime));
    CHECK(script_runtime_runnable_state(runtime, &state)
          && !state.jobs_pending);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int32_t page_int(ScriptRuntime *runtime, const char *expression)
{
    JSValue value = JS_Eval(runtime->context, expression, strlen(expression),
                            "<check>", 0);
    int32_t number = -1;
    if (JS_ToInt32(runtime->context, &number, value) < 0) number = -1;
    JS_FreeValue(runtime->context, value);
    return number;
}

/* The PSP frame loop starts a turn after a 2 ms yield when page work is
   already waiting, and after a vblank otherwise. The page's timer clock is
   virtual there (no sampled wall clock), so early turns must move it by
   the wall time that passed, never by a whole tick: a timeout or an
   animation frame may not run ahead of real time while a long promise
   chain keeps the turns short. */
static int test_fast_turns_follow_wall_time(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://fast-turns.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(!runtime->bridge.wall_clock_timers);
    TilefinchRuntimeClock clock = {0};
    uint64_t wall_us = UINT64_C(1000000);
    CHECK(script_runtime_advance(
        runtime, tilefinch_runtime_clock_step(&clock, wall_us, 16), 2,
        &result));
    /* A canvas publication's fast followup can arrive 1ms before the next
       virtual rAF deadline. Wait that remainder rather than consuming the
       followup on an empty turn and then waiting an entire vblank. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.earlyFrames=0;requestAnimationFrame(()=>earlyFrames++)",
        "<early-frame>", &result));
    unsigned required_ms = 0;
    CHECK(script_runtime_frame_clock_delay_ms(runtime, &required_ms)
          && required_ms == 16);
    uint32_t wait_us = UINT32_MAX;
    TilefinchRuntimeClock before_wait = clock;
    CHECK(tilefinch_runtime_clock_frame_wait(
        &clock, wall_us + 15000u, 16, required_ms, &wait_us));
    CHECK(wait_us == 1000u && clock.last_us == before_wait.last_us
          && clock.carry_us == before_wait.carry_us);
    wall_us += 15000u + wait_us;
    CHECK(script_runtime_advance(runtime,
        tilefinch_runtime_clock_step(&clock, wall_us, 16), 2, &result));
    CHECK(page_int(runtime, "earlyFrames") == 1);
    CHECK(!script_runtime_frame_clock_delay_ms(runtime, &required_ms));
    CHECK(!tilefinch_runtime_clock_frame_wait(
        &clock, wall_us, 16, 17, &wait_us));
    CHECK(tilefinch_runtime_clock_frame_wait(
        &clock, wall_us + 16000u, 16, 16, &wait_us) && wait_us == 0);
    clock.carry_us = 400u;
    CHECK(tilefinch_runtime_clock_frame_wait(
        &clock, wall_us + 15000u, 16, 16, &wait_us) && wait_us == 600u);
    clock.carry_us = 0;
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.chainDone=0;globalThis.frames=0;"
        "globalThis.timeoutAt=-1;globalThis.turn=0;"
        "setTimeout(()=>{timeoutAt=turn},100);"
        "setTimeout(()=>{let p=Promise.resolve();"
        "for(let i=0;i<1000;i++)p=p.then(()=>{});p.then(()=>{chainDone=1})},0);"
        "const f=()=>{frames++;requestAnimationFrame(f)};"
        "requestAnimationFrame(f)",
        "<fast-turns>", &result));
    uint64_t started_us = wall_us;
    int32_t fired_turn = -1;
    uint64_t fired_us = 0;
    for (int32_t turn = 1; turn <= 40; turn++) {
        wall_us += script_runtime_task_runnable(runtime)
            ? UINT64_C(2000) : UINT64_C(16667);
        char set_turn[48];
        snprintf(set_turn, sizeof(set_turn), "turn=%d", (int) turn);
        CHECK(page_int(runtime, set_turn) == turn);
        CHECK(script_runtime_advance(
            runtime, tilefinch_runtime_clock_step(&clock, wall_us, 16), 2,
            &result));
        /* Animation frames never outrun a 16 ms frame rate. */
        CHECK((uint64_t) page_int(runtime, "frames")
              <= (wall_us - started_us) / UINT64_C(16000) + 1u);
        if (fired_turn < 0 && page_int(runtime, "timeoutAt") >= 0) {
            fired_turn = page_int(runtime, "timeoutAt");
            fired_us = wall_us;
        }
    }
    /* The chain took several short turns, and the 100 ms timeout ran only
       once 100 ms of wall time had passed. */
    CHECK(page_int(runtime, "chainDone") == 1);
    CHECK(fired_turn > 0);
    CHECK(fired_us - started_us >= UINT64_C(99000));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_blank_recovery_author_work_census(void)
{
    JSRuntime *quickjs = JS_NewRuntime();
    CHECK(quickjs != NULL);
    ScriptRuntime runtime = {0};
    runtime.runtime = quickjs;

    /* These remain visible in ScriptResult.pending_tasks for diagnostics,
       but long-lived transports must not keep a blank page in recovery's
       deferred state forever. */
    runtime.result.pending_tasks = 4u;
    runtime.bridge.event_source_count = 1u;
    runtime.bridge.websocket_count = 1u;
    runtime.bridge.multiplayer.active = true;
    CHECK(!script_runtime_has_pending_author_work(&runtime));

    runtime.pending_timer_tasks = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.pending_timer_tasks = 0u;
    runtime.pending_network_tasks = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.pending_network_tasks = 0u;
    runtime.bridge.async_fetch_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.bridge.async_fetch_count = 0u;
    runtime.bridge.dynamic_script_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));
    runtime.bridge.dynamic_script_count = 0u;
    runtime.page_visibility_queue_count = 1u;
    CHECK(script_runtime_has_pending_author_work(&runtime));

    JS_FreeRuntime(quickjs);
    return 0;
}

static lxb_dom_node_t *find_script(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *name = document_element_name(node, &length);
        if (name != NULL && length == 6 && memcmp(name, "script", 6) == 0)
            return node;
        lxb_dom_node_t *nested = find_script(node->first_child);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static lxb_dom_node_t *find_element_id(lxb_dom_node_t *node,
                                       const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *nested = find_element_id(node->first_child, wanted);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static bool digest_matches_hex(
    const uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES], const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    if (hex == NULL
        || strlen(hex) != TILEFINCH_SHA256_DIGEST_BYTES * 2u) return false;
    for (size_t index = 0; index < TILEFINCH_SHA256_DIGEST_BYTES; index++) {
        if (digits[digest[index] >> 4] != hex[index * 2u]
            || digits[digest[index] & 15u] != hex[index * 2u + 1u]) {
            return false;
        }
    }
    return true;
}

static bool collect_and_drain_finalizers(ScriptRuntime *runtime,
                                         ScriptResult *result)
{
    (void) script_runtime_collect_and_trim(runtime);
    /* Cleanup tasks have their own bounded drain after promise checkpoints.
       Allow enough turns for the large wrapper-churn fixture without
       weakening the production task quota. Repeat collection after tasks:
       newly unreachable WeakRef
       targets are not guaranteed to retire in the first GC cycle. */
    for (size_t checkpoint = 0; checkpoint < 64; checkpoint++) {
        if (checkpoint != 0 && checkpoint % 4 == 0)
            (void) script_runtime_collect_and_trim(runtime);
        if (!script_runtime_advance(runtime, 0, 1024, result)) return false;
    }
    return true;
}

static int test_stream_and_xhr_private_state_reclamation(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 8u * MIB, 4000,
        "https://private-state.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "new ReadableStream({start(c){c.close()}});new XMLHttpRequest();"
        "globalThis.pocSummary='PRIVATE-STATE-WARM';",
        "<private-state-warm>", &result));
    CHECK(collect_and_drain_finalizers(runtime, &result));
    size_t baseline =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);

    /* Browser-private state must be owned by its public object, not by a
       realm-global WeakMap value. QuickJS cannot collect a WeakMap value/key
       ephemeron when the value closes over its otherwise-dead key; that used
       to retain every dropped stream queue and completed XHR response. */
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "(()=>{for(let at=0;at<2;at++)new ReadableStream({start(c){"
        "c.enqueue(new Uint8Array(256*1024));c.close()}});"
        "const xhr=new XMLHttpRequest();xhr.payload=new Uint8Array(512*1024);"
        "xhr.onload=function(){return xhr}})();"
        "globalThis.__retainedController=null;"
        "(()=>{new ReadableStream({start(c){__retainedController=c}})})();"
        "globalThis.__retainedXHR=new XMLHttpRequest();"
        "globalThis.__retainedThis=false;__retainedXHR.onload=function(){"
        "__retainedThis=this===__retainedXHR};"
        "globalThis.pocSummary='PRIVATE-STATE-PENDING';",
        "<private-state-pressure>", &result));
    CHECK(collect_and_drain_finalizers(runtime, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "__retainedController.enqueue(new Uint8Array([7]));"
        "__retainedController.close();"
        "__retainedXHR.dispatchEvent(new Event('load'));"
        "let branded=false;try{Object.getOwnPropertyDescriptor("
        "XMLHttpRequest.prototype,'responseText').get.call({})}catch(error){"
        "branded=error instanceof TypeError}"
        "globalThis.pocSummary=__retainedThis&&branded?"
        "'PRIVATE-STATE-OK':'PRIVATE-STATE-FAILED';"
        "globalThis.__retainedController=null;globalThis.__retainedXHR=null;",
        "<private-state-live-owner>", &result));
    CHECK(strcmp(result.summary, "PRIVATE-STATE-OK") == 0);
    CHECK(collect_and_drain_finalizers(runtime, &result));
    size_t retained =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    printf("private-state-reclamation baseline=%zu retained=%zu delta=%zu\n",
           baseline, retained, retained > baseline ? retained - baseline : 0u);
    CHECK(retained <= baseline + 128u * 1024u);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_response_body_release_with_retained_wrappers(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000,
        "https://response-retention.test/", NULL, &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.retainedResponses=[];new Response('warm').body.cancel();"
        "globalThis.pocSummary='RESPONSE-RETENTION-WARM';",
        "<response-retention-warm>", &result));
    CHECK(collect_and_drain_finalizers(runtime, &result));
    size_t baseline =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary='RESPONSE-RETENTION-PENDING';(async()=>{"
        "for(let at=0;at<12;at++){const response=new Response(new Uint8Array("
        "128*1024));retainedResponses.push(response);if((at&1)===0)await "
        "response.body.cancel();else if((at%3)===0){await Promise.resolve();await "
        "response.arrayBuffer()}else{const reader=response.body.getReader();"
        "for(;;){const item=await reader.read();if(item.done)break}}}"
        "globalThis.pocSummary='RESPONSE-RETENTION-OK'})().catch(error=>{"
        "globalThis.pocSummary='RESPONSE-RETENTION-ERROR:'+String(error&&"
        "error.stack||error)});",
        "<response-retention>", &result));
    for (size_t tick = 0; tick < 32
         && strcmp(result.summary, "RESPONSE-RETENTION-PENDING") == 0;
         tick++) {
        CHECK(script_runtime_advance(runtime, 0, 1024, &result));
    }
    CHECK(strcmp(result.summary, "RESPONSE-RETENTION-OK") == 0);
    CHECK(collect_and_drain_finalizers(runtime, &result));
    size_t retained =
        budget_quickjs_pool_js_malloc_current(runtime->quickjs_pool);
    printf("response-retention baseline=%zu retained=%zu delta=%zu\n",
           baseline, retained, retained > baseline ? retained - baseline : 0u);
    /* Twelve live Response wrappers and their exhausted stream state are
       small. Their twelve 128 KiB snapshots and any prefetched 4 KiB copies
       must no longer be retained after cancel, public drain, or fast drain. */
    CHECK(retained <= baseline + 128u * 1024u);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

typedef struct {
    TilefinchCredentialsMode first_credentials;
    TilefinchCredentialsMode second_credentials;
    char first_referrer_url[256];
    char second_referrer_url[256];
    char first_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    char second_referrer_policy[BROWSER_MODULE_REFERRER_POLICY_LIMIT];
    size_t calls;
    bool unexpected_url;
} ModuleCredentialProbe;

static char *probe_copy(const char *text)
{
    size_t length = strlen(text);
    char *copy = malloc(length + 1u);
    if (copy != NULL) memcpy(copy, text, length + 1u);
    return copy;
}

static bool module_credential_probe_load(
    void *opaque, const ScriptModuleLoadRequest *request,
    ScriptModuleLoadResult *result)
{
    static const char first_request[] =
        "https://response-a.test/modules/child.js";
    static const char second_request[] =
        "https://response-b.test/modules/child.js";
    static const char first_response[] =
        "https://cdn-a.test/final/child.js";
    static const char second_response[] =
        "https://cdn-b.test/final/child.js";
    static const char source[] = "export const value=import.meta.url;";
    ModuleCredentialProbe *probe = opaque;
    if (probe == NULL || request == NULL || request->request_url == NULL
        || request->referrer_url == NULL
        || request->referrer_policy == NULL || result == NULL) return false;
    const char *response_url = NULL;
    if (strcmp(request->request_url, first_request) == 0) {
        probe->first_credentials = request->credentials;
        snprintf(probe->first_referrer_url,
                 sizeof(probe->first_referrer_url), "%s",
                 request->referrer_url);
        snprintf(probe->first_referrer_policy,
                 sizeof(probe->first_referrer_policy), "%s",
                 request->referrer_policy);
        response_url = first_response;
    } else if (strcmp(request->request_url, second_request) == 0) {
        probe->second_credentials = request->credentials;
        snprintf(probe->second_referrer_url,
                 sizeof(probe->second_referrer_url), "%s",
                 request->referrer_url);
        snprintf(probe->second_referrer_policy,
                 sizeof(probe->second_referrer_policy), "%s",
                 request->referrer_policy);
        response_url = second_response;
    } else {
        probe->unexpected_url = true;
        return false;
    }
    probe->calls++;
    result->source = probe_copy(source);
    result->source_length = sizeof(source) - 1u;
    result->response_url = probe_copy(response_url);
    if (result->source == NULL || result->response_url == NULL) {
        free(result->source);
        free(result->response_url);
        memset(result, 0, sizeof(*result));
        return false;
    }
    return true;
}

static void module_credential_probe_release(
    void *opaque, ScriptModuleLoadResult *result)
{
    (void) opaque;
    if (result == NULL) return;
    free(result->source);
    free(result->response_url);
    memset(result, 0, sizeof(*result));
}

static bool test_delayed_module_request_contexts(
    ScriptRuntime *runtime, lxb_dom_node_t *script, ScriptResult *result)
{
    static const char first_root[] =
        "globalThis.__loadCredentialA=()=>import('./child.js');";
    static const char second_root[] =
        "globalThis.__loadCredentialB=()=>import('./child.js');";
    static const char delayed_imports[] =
        "globalThis.pocSummary='MODULE-CREDENTIALS-PENDING';"
        "Promise.all([__loadCredentialA(),__loadCredentialB()])"
        ".then(values=>{globalThis.pocSummary='MODULE-CREDENTIALS:' + "
        "values[0].value+'|'+values[1].value})"
        ".catch(error=>{globalThis.pocSummary='MODULE-CREDENTIALS-ERROR:'"
        "+error});";
    static const char expected[] =
        "MODULE-CREDENTIALS:https://cdn-a.test/final/child.js|"
        "https://cdn-b.test/final/child.js";
    ModuleCredentialProbe probe = {0};
    script_runtime_set_module_loader(
        runtime, module_credential_probe_load,
        module_credential_probe_release, &probe);
    bool ok = script_runtime_evaluate_external_module_context(
            runtime, script, first_root, sizeof(first_root) - 1u,
            "https://request-a.test/root.js",
            "https://response-a.test/modules/root.js",
            "origin", TILEFINCH_CREDENTIALS_SAME_ORIGIN, result)
        && script_runtime_evaluate_external_module_context(
            runtime, script, second_root, sizeof(second_root) - 1u,
            "https://request-b.test/root.js",
            "https://response-b.test/modules/root.js",
            "no-referrer", TILEFINCH_CREDENTIALS_INCLUDE, result);
    /* Loader availability is mutable; the graph metadata is not. */
    script_runtime_set_module_loader(runtime, NULL, NULL, NULL);
    script_runtime_set_module_loader(
        runtime, module_credential_probe_load,
        module_credential_probe_release, &probe);
    ok = ok && script_runtime_evaluate_diagnostic(
        runtime, delayed_imports, "<delayed-module-imports>", result);
    for (size_t checkpoint = 0;
         ok && checkpoint < 8 && strcmp(result->summary, expected) != 0;
         checkpoint++) {
        ok = script_runtime_advance(runtime, 0, 32, result);
    }
    ok = ok && strcmp(result->summary, expected) == 0
        && probe.calls == 2 && !probe.unexpected_url
        && probe.first_credentials == TILEFINCH_CREDENTIALS_SAME_ORIGIN
        && probe.second_credentials == TILEFINCH_CREDENTIALS_INCLUDE
        && strcmp(probe.first_referrer_url,
                  "https://response-a.test/modules/root.js") == 0
        && strcmp(probe.second_referrer_url,
                  "https://response-b.test/modules/root.js") == 0
        && strcmp(probe.first_referrer_policy, "origin") == 0
        && strcmp(probe.second_referrer_policy, "no-referrer") == 0;
    script_runtime_set_module_loader(runtime, NULL, NULL, NULL);
    return ok;
}

static int test_computed_style_native_cooperation(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><style>div{color:red;padding-left:10%}</style><body>"
        "<div><div><div><div><div><div><div><div id=probe>Text";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u*MIB, 4000, "https://style.test/", &options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    lxb_dom_node_t *node = find_element_id(lxb_dom_interface_node(document.html), "probe");
    CHECK(node != NULL);
    JSValue args[2] = {
        JS_NewInt64(runtime->context, script_runtime_node_handle(runtime, node)),
        JS_NewString(runtime->context, "color")
    };
    /* Percentage padding must not trigger a second ancestor cascade. A
       minimal retained parent box makes the old fallback run; count native
       checkpoints rather than measuring a noisy wall-clock microbenchmark. */
    LayoutNodeBox parent_box = {
        .node = node->parent, .width = 100, .client_width = 100
    };
    LayoutDocument layout = {
        .width = 480, .node_boxes = &parent_box, .node_box_count = 1
    };
    runtime->bridge.layout = &layout;
    js_rt_runtime_arm_watchdog(runtime);
    JSValue measured = js_computed_style_get(
        runtime->context, JS_UNDEFINED, 2, args);
    CHECK(!JS_IsException(measured));
    JS_FreeValue(runtime->context, measured);
    size_t color_polls = runtime->watchdog.polls;
    JS_FreeValue(runtime->context, args[1]);
    args[1] = JS_NewString(runtime->context, "padding-left");
    /* Retained styles survive entries into JavaScript; a host style change
       makes each read below walk the whole chain again. */
    document_style_changed();
    js_rt_runtime_arm_watchdog(runtime);
    measured = js_computed_style_get(runtime->context, JS_UNDEFINED, 2, args);
    CHECK(!JS_IsException(measured) && runtime->watchdog.polls == color_polls);
    JS_FreeValue(runtime->context, measured);
    static const char guarded_source[] =
        "(function(read,handle,property){try{return read(handle,property)}"
        "catch(error){return 'caught'}})";
    JSValue guarded = JS_Eval(runtime->context, guarded_source,
        sizeof(guarded_source) - 1u, "<guarded-style-read>", JS_EVAL_TYPE_GLOBAL);
    JSValue reader = JS_NewCFunction(runtime->context, js_computed_style_get,
        "readStyle", 2);
    CHECK(JS_IsFunction(runtime->context, guarded)
        && JS_IsFunction(runtime->context, reader));
    JSValue call_args[3] = { reader, args[0], args[1] };
    CallbackAbortCooperate probe = {0};
    TilefinchPlatformServices services = { .context = &probe,
        .cooperate = callback_abort_cooperate };
    document_style_changed();
    js_rt_runtime_arm_watchdog(runtime);
    tilefinch_platform_set_services(&services);
    JSValue value = JS_Call(runtime->context, guarded, JS_UNDEFINED, 3, call_args);
    tilefinch_platform_set_services(NULL);
    CHECK(probe.calls == 1 && JS_IsException(value));
    JSValue exception = JS_GetException(runtime->context);
    JS_FreeValue(runtime->context, exception);
    /* Ordinary author conversion failures remain catchable. Cancellation is
       task control flow, not a blanket change to computed-style errors. */
    js_rt_runtime_arm_watchdog(runtime);
    static const char conversion_source[] =
        "({toString(){throw new Error('conversion')}})";
    JSValue conversion = JS_Eval(runtime->context, conversion_source,
        sizeof(conversion_source) - 1u, "<style-conversion>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(conversion));
    call_args[2] = conversion;
    value = JS_Call(runtime->context, guarded, JS_UNDEFINED, 3, call_args);
    const char *caught = JS_ToCString(runtime->context, value);
    CHECK(caught != NULL && strcmp(caught, "caught") == 0);
    JS_FreeCString(runtime->context, caught);
    JS_FreeValue(runtime->context, value);
    JS_FreeValue(runtime->context, conversion);
    document_style_changed();
    js_rt_runtime_arm_watchdog(runtime);
    runtime->watchdog.deadline_ms = 0;
    value = js_computed_style_get(runtime->context, JS_UNDEFINED, 2, args);
    CHECK(JS_IsException(value) && runtime->watchdog.interrupted);
    exception = JS_GetException(runtime->context);
    JS_FreeValue(runtime->context, exception);
    JS_FreeValue(runtime->context, guarded);
    JS_FreeValue(runtime->context, reader);
    JS_FreeValue(runtime->context, args[0]);
    JS_FreeValue(runtime->context, args[1]);
    runtime->bridge.layout = NULL;
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.pocSummary=getComputedStyle(document.getElementById('probe')).color",
        "<style-after-cancel>", &result)
        && strcmp(result.summary, "rgb(255, 0, 0)") == 0);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const style=getComputedStyle(document.getElementById('probe'));"
        "let readonly=false;try{style.color='blue'}catch(error){readonly="
        "error instanceof DOMException&&error.name==='NoModificationAllowedError'}"
        "const names=[...style],first=style.item(0);globalThis.pocSummary="
        "style instanceof CSSStyleDeclaration"
        "&&Object.prototype.toString.call(style)==='[object CSSStyleDeclaration]'"
        "&&style.length===names.length&&first===style[0]"
        "&&style.getPropertyPriority('color')===''&&readonly"
        "&&style.color==='rgb(255, 0, 0)'?'COMPUTED-STYLE-OK':"
        "'COMPUTED-STYLE-FAILED'})()",
        "<computed-style-contract>", &result)
        && strcmp(result.summary, "COMPUTED-STYLE-OK") == 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Consecutive computed-style reads of one element reuse its cascade; the
   memo must still see a class change, a sibling that flips :last-child,
   and a native DOM change made between two entries into JavaScript (as the
   parser makes), none of which may leave a stale value. */
static int test_computed_style_memo_invalidation(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><style>p{display:block;color:rgb(0,0,255)}"
        "p.gone{display:none}p:last-child{color:rgb(255,0,0)}</style>"
        "<body><div id=host><p id=probe>Text</p></div>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://memo.test/", &options,
        &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const p=document.getElementById('probe'),"
              "cs=getComputedStyle(p),r=[cs.display==='block',"
              "cs.color==='rgb(255, 0, 0)'];p.className='gone';"
              "r.push(cs.display==='none');p.className='';"
              "r.push(cs.display==='block');const q=document.createElement('p');"
              "document.getElementById('host').appendChild(q);"
              "r.push(cs.color==='rgb(0, 0, 255)');q.remove();"
              "r.push(cs.color==='rgb(255, 0, 0)');"
              "globalThis.pocSummary=r.every(Boolean)?'MEMO-OK':'MEMO:'+r})()",
              "<computed-style-memo>", &result)
          && strcmp(result.summary, "MEMO-OK") == 0);
    /* Leave a memo for the probe, then append a sibling natively, which
       bumps no DOM generation, as the parser would between scripts. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('probe')).color",
              "<computed-style-before>", &result)
          && strcmp(result.summary, "rgb(255, 0, 0)") == 0);
    lxb_dom_node_t *host = find_element_id(
        lxb_dom_interface_node(document.html), "host");
    CHECK(host != NULL);
    lxb_dom_element_t *sibling = lxb_dom_document_create_element(
        lxb_dom_interface_node(document.html)->owner_document,
        (const lxb_char_t *) "p", 1, NULL);
    CHECK(sibling != NULL);
    lxb_dom_node_insert_child(host, lxb_dom_interface_node(sibling));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('probe')).color",
              "<computed-style-after>", &result)
          && strcmp(result.summary, "rgb(0, 0, 255)") == 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static JSValue test_set_container_width(JSContext *context,
    JSValueConst this_value, int argc, JSValueConst *argv)
{
    (void) this_value;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int32_t width = 0;
    if (argc != 1 || JS_ToInt32(context, &width, argv[0]) < 0)
        return JS_EXCEPTION;
    Stylesheet *sheet = (Stylesheet *) bridge->stylesheet;
    if (width < 0) {
        style_container_layout_state_clear(sheet);
        return JS_UNDEFINED;
    }
    lxb_dom_node_t *root = find_element_id(
        lxb_dom_interface_node(bridge->document->html), "container");
    lxb_dom_node_t *other = find_element_id(
        lxb_dom_interface_node(bridge->document->html), "other");
    if (!style_container_layout_state_begin(sheet, bridge->budget, 2)
        || !style_container_layout_state_add(
            sheet, root, width, 100, 0, 0)
        || !style_container_layout_state_add(sheet, other, 50, 100, 0, 0))
        return JS_ThrowInternalError(context, "container state refused");
    return JS_UNDEFINED;
}

static bool host_style_read(ScriptRuntime *runtime, const char *id,
                            const char *property, const char *expected,
                            int warm);

/* Container geometry can change within one script entry without a DOM
   mutation. Both the node memo and inherited-style cache must invalidate,
   including the variable-resolution lease, while repeated reads stay cheap. */
static int test_computed_style_container_cache(bool inline_units)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    char html[1024];
    int html_length = snprintf(html, sizeof(html),
        "<!doctype html><style>section{container-type:inline-size}"
        ".shade{--tone:rgb(1,2,3)}p{color:var(--tone);%s}"
        "@container (min-width:200px){.shade{--tone:rgb(4,5,6)}}"
        "</style><body><section id=container><div class=shade>"
        "<p id=x %s>X</p><p id=y %s>Y</p></div></section>"
        "<section id=other><div class=shade><p id=z %s>Z</p></div></section>",
        inline_units ? "" : "margin-left:10cqw;padding-left:calc(10cqw + 1px)",
        inline_units ? "style='margin-left:10cqw;padding-left:calc(10cqw + 1px)'" : "",
        inline_units ? "style='margin-left:10cqw;padding-left:calc(10cqw + 1px)'" : "",
        inline_units ? "style='margin-left:10cqw;padding-left:calc(10cqw + 1px)'" : "");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(html_length > 0 && (size_t) html_length < sizeof(html)
        && document_parse(&document, &budget, html, (size_t) html_length, 31)
        && stylesheet_build(&sheet, &budget, &document, 480));
    CHECK(sheet.has_container_relative_units == !inline_units);
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
        && script_execution_policy_for_profile(
            SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://container-cache.test/",
        &options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    JSValue global = JS_GetGlobalObject(runtime->context);
    CHECK(JS_SetPropertyStr(runtime->context, global, "setContainerWidth",
        JS_NewCFunction(runtime->context, test_set_container_width,
            "setContainerWidth", 1)) >= 0);
    JS_FreeValue(runtime->context, global);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const x=getComputedStyle(document.getElementById('x')),"
        "y=getComputedStyle(document.getElementById('y')),"
        "z=getComputedStyle(document.getElementById('z'));const r=[];"
        "setContainerWidth(100);"
        "r.push(x.color==='rgb(1, 2, 3)',x.marginLeft==='10px');"
        "for(let i=0;i<12;i++)r.push(y.color===x.color,"
        "z.marginLeft==='5px',x.marginLeft==='10px',"
        "z.paddingLeft==='6px',x.paddingLeft==='11px');"
        "setContainerWidth(300);"
        "r.push(x.color==='rgb(4, 5, 6)',x.marginLeft==='30px',"
        "y.color===x.color,y.marginLeft==='30px');"
        "setContainerWidth(100);"
        "r.push(x.color==='rgb(1, 2, 3)',y.marginLeft==='10px');"
        "setContainerWidth(-1);r.push(x.color==='rgb(1, 2, 3)');"
        "setContainerWidth(300);r.push(x.color==='rgb(4, 5, 6)',"
        "y.marginLeft==='30px');"
        "pocSummary=r.every(Boolean)?'CONTAINER-CACHE-OK':"
        "r.join(',')+' sizes='+x.marginLeft+'/'+y.marginLeft})()",
        "<computed-style-container-cache>", &result));
    if (strcmp(result.summary, "CONTAINER-CACHE-OK") != 0)
        fprintf(stderr, "container cache: %s\n", result.summary);
    CHECK(strcmp(result.summary, "CONTAINER-CACHE-OK") == 0);
    CHECK(runtime->bridge.computed_style_cache.hits >= 12);
    CHECK(runtime->bridge.computed_style_cache.full_clears >= 5);
    /* Every layout pass rebuilds the container states. Identical states
       keep retained styles across entries; changed ones do not. */
    CHECK(host_style_read(runtime, "x", "color", "rgb(4, 5, 6)", true));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "setContainerWidth(300)", "<container-same>", &result));
    CHECK(host_style_read(runtime, "x", "color", "rgb(4, 5, 6)", true));
    CHECK(host_style_read(runtime, "y", "marginLeft", "30px", true));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "setContainerWidth(100)", "<container-changed>", &result));
    CHECK(host_style_read(runtime, "x", "color", "rgb(1, 2, 3)", false));
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* The focused element stays connected when a subtree holding it moves, but
   it inherits its new ancestors' visibility: moving its ancestor under a
   visibility:hidden container has to run the focus fixup (focus moves to
   the body). Insertions under its ancestors that do not contain it still
   ask nothing. */
static int test_focus_fixup_follows_moved_subtree(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char html[] =
        "<!doctype html><style>.shut{visibility:hidden}</style><body>"
        "<div id=shut class=shut></div><div id=wrap><input id=moved></div>"
        "</body>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://focus-move.test/",
        &options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const $=(i)=>document.getElementById(i),input=$('moved'),"
              "wrap=$('wrap');input.focus();"
              "const original=globalThis.getComputedStyle;"
              "globalThis.movedReads=0;globalThis.getComputedStyle="
              "function(target){if(target===input)globalThis.movedReads++;"
              "return original.apply(this,arguments)};"
              "setTimeout(()=>{document.body.appendChild("
              "document.createElement('p'));wrap.appendChild("
              "document.createElement('span'));wrap.append('text')},0);"
              "setTimeout(()=>{globalThis.besideReads=movedReads;"
              "globalThis.beforeMove=document.activeElement.id;"
              "$('shut').appendChild(wrap)},0);"
              "globalThis.pocSummary='queued';})()",
              "<focus-fixup-move>", &result));
    for (int step = 0; step < 4; step++)
        CHECK(script_runtime_advance(runtime, 20, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=[besideReads,beforeMove,"
              "document.getElementById('moved').isConnected,"
              "getComputedStyle(document.getElementById('moved')).visibility,"
              "document.activeElement===document.body?'body':"
              "String(document.activeElement.id)].join()",
              "<focus-fixup-move-result>", &result));
    if (strcmp(result.summary, "0,moved,true,hidden,body") != 0)
        fprintf(stderr, "focus fixup move: %s\n", result.summary);
    CHECK(strcmp(result.summary, "0,moved,true,hidden,body") == 0);
    /* The same move with the focused input inside a shadow tree, whose
       ancestry the native chain cannot see past the shadow root. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const host=document.createElement('div');"
              "document.body.appendChild(host);const input="
              "document.createElement('input');input.id='inner';"
              "host.attachShadow({mode:'open'}).appendChild(input);"
              "input.focus();globalThis.shadowBefore=document.activeElement"
              "===host&&host.shadowRoot.activeElement===input;"
              "setTimeout(()=>document.getElementById('shut')"
              ".appendChild(host),0);globalThis.pocSummary='queued';})()",
              "<focus-fixup-shadow-move>", &result));
    for (int step = 0; step < 4; step++)
        CHECK(script_runtime_advance(runtime, 20, 64, &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=[shadowBefore,"
              "document.activeElement===document.body?'body':"
              "String(document.activeElement.id||document.activeElement."
              "tagName)].join()",
              "<focus-fixup-shadow-move-result>", &result));
    if (strcmp(result.summary, "true,body") != 0)
        fprintf(stderr, "focus fixup shadow move: %s\n", result.summary);
    CHECK(strcmp(result.summary, "true,body") == 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Reads of different elements reuse resolved ancestor styles; a class
   change on an ancestor, an inline style on a parent, and a native change
   between entries must each reach every descendant's inherited value. */
static int test_computed_style_ancestor_cache(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><style>.a{color:rgb(1,2,3)}.b{color:rgb(4,5,6)}"
        "section p{font-weight:700}.c p{font-weight:400}</style>"
        "<body><section id=root class=a><div><div id=mid>"
        "<p id=x>1</p><p id=y>2</p></div></div></section>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://cache.test/", &options,
        &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const $=(i)=>document.getElementById(i),"
              "x=getComputedStyle($('x')),y=getComputedStyle($('y')),"
              "r=[x.color==='rgb(1, 2, 3)',y.color==='rgb(1, 2, 3)',"
              "x.fontWeight==='700'];"
              "$('root').className='b';"
              "r.push(y.color==='rgb(4, 5, 6)',x.color==='rgb(4, 5, 6)');"
              "$('root').classList.add('c');"
              "r.push(x.fontWeight==='400',y.fontWeight==='400');"
              "$('mid').style.color='rgb(7, 8, 9)';"
              "r.push(x.color==='rgb(7, 8, 9)',y.color==='rgb(7, 8, 9)');"
              "globalThis.pocSummary=r.every(Boolean)?'CACHE-OK':"
              "'CACHE:'+r})()",
              "<computed-style-cache>", &result)
          && strcmp(result.summary, "CACHE-OK") == 0);
    /* The second element's chain came from the cache. */
    CHECK(runtime->bridge.computed_style_cache.hits != 0);
    size_t probes_before = runtime->bridge.computed_style_cache.lookup_probes;
    size_t hits_before = runtime->bridge.computed_style_cache.hits;
    uint64_t read_started = tilefinch_platform_monotonic_time_ns();
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const host=document.getElementById('mid'),a=[];"
        "for(let i=0;i<120;i++){const p=document.createElement('p');"
        "host.appendChild(p);a.push(p)}"
        "for(let pass=0;pass<4;pass++){"
        "for(let i=0;i<120;i++)if(getComputedStyle(a[i]).color!=="
        "'rgb(7, 8, 9)')throw Error('evicted style');"
        "for(let i=0;i<1000;i++)if(getComputedStyle(a[80+i%40]).color!=="
        "'rgb(7, 8, 9)')throw Error('indexed style');"
        "host.style.color=pass%2?'rgb(7,8,9)':'rgb(10,11,12)';"
        "for(let i=0;i<120;i++)if(getComputedStyle(a[i]).color!=="
        "(pass%2?'rgb(7, 8, 9)':'rgb(10, 11, 12)'))throw Error('stale style');"
        "host.style.color='rgb(7,8,9)'}pocSummary='INDEXED-STYLE-OK'})()",
        "<computed-style-index>", &result));
    CHECK(strcmp(result.summary, "INDEXED-STYLE-OK") == 0);
    size_t probes = runtime->bridge.computed_style_cache.lookup_probes
        - probes_before;
    size_t hits = runtime->bridge.computed_style_cache.hits - hits_before;
    printf("computed-style index: probes=%zu hits=%zu us=%llu\n", probes, hits,
           (unsigned long long) ((tilefinch_platform_monotonic_time_ns()
                                   - read_started) / 1000u));
    CHECK(hits > 4000 && probes < hits * 8u);
    /* A native class change between entries, as the parser might make. */
    lxb_dom_node_t *root = find_element_id(
        lxb_dom_interface_node(document.html), "root");
    CHECK(root != NULL);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('y')).color",
              "<computed-style-cache-before>", &result)
          && strcmp(result.summary, "rgb(7, 8, 9)") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "document.getElementById('mid').style.color='';"
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('x')).color",
              "<computed-style-cache-inline-cleared>", &result)
          && strcmp(result.summary, "rgb(4, 5, 6)") == 0);
    lxb_dom_element_set_attribute(
        lxb_dom_interface_element(root), (const lxb_char_t *) "class", 5,
        (const lxb_char_t *) "a", 1);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('y')).color",
              "<computed-style-cache-after>", &result)
          && strcmp(result.summary, "rgb(1, 2, 3)") == 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* The script split (script_split.h) charges the bridge natives page script
   calls to their kinds, through the ordinary native call path: computed
   styles, DOM queries (global natives and the querySelector methods),
   mutations and first-call compiles of lazy bodies; the rest is JS. */
static int test_script_split_attributes_bridge_work(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><style>.a{color:rgb(1,2,3)}</style>"
        "<body><section id=root class=a><p id=x>1</p><p id=y>2</p>"
        "</section>";
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://split.test/", &options,
        &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(script_split_enabled());
    ScriptSplitTotals before, after, delta;
    script_split_snapshot(&before);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const x=document.getElementById('x'),r=[];"
              "for(let i=0;i<3;i++)"
              "r.push(getComputedStyle(x).color==='rgb(1, 2, 3)');"
              "r.push(document.querySelectorAll('p').length===2,"
              "x.querySelector('b')===null,"
              "x.closest('section')===document.getElementById('root'),"
              "x.matches('p'));"
              "x.setAttribute('data-k','1');"
              /* A body long enough to be compiled lazily on first call. */
              "const f=new Function('a','let s=0;'+"
              "'for(let i=0;i<a.length;i++){s+=a.charCodeAt(i)*31;}'.repeat(40)"
              "+'return s');r.push(f('ab')>0);"
              "globalThis.pocSummary=r.every(Boolean)?'SPLIT-OK':"
              "'SPLIT:'+r})()",
              "<script-split>", &result)
          && strcmp(result.summary, "SPLIT-OK") == 0);
    script_split_snapshot(&after);
    script_split_difference(&after, &before, &delta);
    printf("script split: js=%llu style=%llu/%llu query=%llu/%llu "
           "mutate=%llu/%llu compile=%llu/%llu\n",
           (unsigned long long) delta.us[SCRIPT_SPLIT_JS],
           (unsigned long long) delta.us[SCRIPT_SPLIT_STYLE],
           (unsigned long long) delta.calls[SCRIPT_SPLIT_STYLE],
           (unsigned long long) delta.us[SCRIPT_SPLIT_QUERY],
           (unsigned long long) delta.calls[SCRIPT_SPLIT_QUERY],
           (unsigned long long) delta.us[SCRIPT_SPLIT_MUTATE],
           (unsigned long long) delta.calls[SCRIPT_SPLIT_MUTATE],
           (unsigned long long) delta.us[SCRIPT_SPLIT_COMPILE],
           (unsigned long long) delta.calls[SCRIPT_SPLIT_COMPILE]);
    CHECK(delta.calls[SCRIPT_SPLIT_STYLE] >= 3);
    CHECK(delta.calls[SCRIPT_SPLIT_QUERY] >= 4);
    CHECK(delta.calls[SCRIPT_SPLIT_MUTATE] >= 1);
    /* The engine's lazy-compile hook: new Function's body is compiled on
       its first call, inside the script. */
    CHECK(delta.calls[SCRIPT_SPLIT_COMPILE] >= 1);
    CHECK(delta.calls[SCRIPT_SPLIT_FETCH] == 0);
    CHECK(delta.us[SCRIPT_SPLIT_JS] > 0);
    /* Leaving script leaves nothing open. */
    CHECK(script_split_current() == SCRIPT_SPLIT_KIND_COUNT);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* Mutations between reads in one script drop only the cached styles they
   can change: siblings (sibling combinators, structural pseudo-classes),
   :has() subjects, and custom properties must still see every change; long
   class lists match through the token index, and past its capacity through
   the plain scan. */
static int test_computed_style_scoped_invalidation(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    char html[8192];
    size_t used = (size_t) snprintf(html, sizeof(html), "%s",
        "<!doctype html><style>"
        ".on + .next{color:rgb(1,1,1)}"
        "li:last-child{color:rgb(2,2,2)}"
        ".box:has(.hot){color:rgb(3,3,3)}"
        ":root{--c:rgb(4,4,4)}.theme{--c:rgb(5,5,5)}.use{color:var(--c)}"
        ".k97{color:rgb(6,6,6)}.long-token-name-z{color:rgb(7,7,7)}"
        "[data-open] .dt{color:rgb(9,9,9)}"
        "</style><body><div id=dwrap><p id=dt class=dt>d</p></div>"
        "<div id=pair><p id=first>a</p>"
        "<p id=second class=next>b</p></div>"
        "<ul id=list><li id=l1>1</li><li id=l2>2</li></ul>"
        "<div id=box class=box><span id=kid>k</span></div>"
        "<section id=wrap><div id=mid><p id=user class=use>u</p></div>"
        "</section><p id=long class='");
    for (unsigned i = 0; i < 12u; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "filler-class-%u ", i);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
                              "long-token-name-z'>l</p><p id=many class='");
    for (unsigned i = 0; i < 100u; i++)
        used += (size_t) snprintf(html + used, sizeof(html) - used,
                                  "k%u ", i);
    used += (size_t) snprintf(html + used, sizeof(html) - used,
                              "'>m</p></body>");
    PocDocument document = {0};
    Stylesheet sheet = {0};
    CHECK(document_parse(&document, &budget, html, used, 31)
          && stylesheet_build(&sheet, &budget, &document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://scoped.test/", &options,
        &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const $=(i)=>document.getElementById(i),"
              "c=(i)=>getComputedStyle($(i)).color,r=[];"
              "r.push(c('second')!=='rgb(1, 1, 1)');"
              "$('first').className='on';"
              "r.push(c('second')==='rgb(1, 1, 1)');"
              "r.push(c('l2')==='rgb(2, 2, 2)',c('l1')!=='rgb(2, 2, 2)');"
              "const l3=document.createElement('li');$('list').append(l3);"
              "r.push(c('l2')!=='rgb(2, 2, 2)');"
              "r.push(c('box')!=='rgb(3, 3, 3)');"
              "$('kid').className='hot';"
              "r.push(c('box')==='rgb(3, 3, 3)');"
              "r.push(c('user')==='rgb(4, 4, 4)');"
              "$('wrap').className='theme';"
              "r.push(c('user')==='rgb(5, 5, 5)');"
              "$('mid').style.setProperty('--c','rgb(8, 8, 8)');"
              "r.push(c('user')==='rgb(8, 8, 8)');"
              "$('mid').style.removeProperty('--c');"
              "r.push(c('user')==='rgb(5, 5, 5)');"
              "r.push(c('long')==='rgb(7, 7, 7)',c('many')==='rgb(6, 6, 6)');"
              "$('many').classList.remove('k97');"
              "r.push(c('many')!=='rgb(6, 6, 6)');"
              "globalThis.pocSummary=r.every(Boolean)?'SCOPED-OK':"
              "'SCOPED:'+r.map((v,i)=>v?'':i).filter(String).join(',')})()",
              "<computed-style-scoped>", &result));
    if (strcmp(result.summary, "SCOPED-OK") != 0)
        printf("scoped: %s %s\n", result.summary, result.error);
    CHECK(strcmp(result.summary, "SCOPED-OK") == 0);
    /* A data-* attribute no selector tests keeps the cached chain; one a
       selector tests restyles. */
    size_t misses_before = runtime->bridge.computed_style_cache.misses;
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=getComputedStyle("
              "document.getElementById('dt')).color",
              "<computed-style-data-cold>", &result)
          && strcmp(result.summary, "rgb(9, 9, 9)") != 0);
    size_t cold_misses =
        runtime->bridge.computed_style_cache.misses - misses_before;
    misses_before = runtime->bridge.computed_style_cache.misses;
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const wrap=document.getElementById('dwrap'),"
              "dt=document.getElementById('dt'),first=getComputedStyle(dt)"
              ".color;for(let i=0;i<5;i++){wrap.setAttribute('data-note',"
              "String(i));document.documentElement.setAttribute('data-state',"
              "String(i));if(getComputedStyle(dt).color!==first)"
              "throw Error('unreferenced data attribute restyled')}"
              "globalThis.pocSummary=first})()",
              "<computed-style-data-unreferenced>", &result)
          && strcmp(result.summary, "rgb(9, 9, 9)") != 0);
    /* The chain read cold above survives the entry and every write. */
    CHECK(cold_misses != 0
          && runtime->bridge.computed_style_cache.misses == misses_before);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const wrap=document.getElementById('dwrap'),"
              "dt=document.getElementById('dt'),before=getComputedStyle(dt)"
              ".color;wrap.setAttribute('data-open','');const open="
              "getComputedStyle(dt).color;wrap.removeAttribute('data-open');"
              "globalThis.pocSummary=before!=='rgb(9, 9, 9)'&&open==="
              "'rgb(9, 9, 9)'&&getComputedStyle(dt).color===before?"
              "'DATA-OK':'DATA:'+[before,open]})()",
              "<computed-style-data-referenced>", &result)
          && strcmp(result.summary, "DATA-OK") == 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

/* A runtime over `html` with its stylesheet built and attached, kept by
   the caller for cache checks between evaluations. */
static ScriptRuntime *var_cache_runtime(Budget *budget, PocDocument *document,
                                        Stylesheet *sheet, const char *html)
{
    if (!document_parse(document, budget, html, strlen(html), 31)
        || !stylesheet_build(sheet, budget, document, 480)) return NULL;
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)
        || !script_execution_policy_for_profile(
            SCRIPT_EXECUTION_PROFILE_LAB, &policy)) return NULL;
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        document, budget, 6u * MIB, 4000, "https://varcache.test/", &options,
        &result);
    if (runtime != NULL) script_runtime_set_stylesheet(runtime, sheet);
    return runtime;
}

static bool var_cache_eval(ScriptRuntime *runtime, const char *script,
                           const char *expected)
{
    ScriptResult result = {0};
    if (!script_runtime_evaluate_diagnostic(runtime, script,
                                            "<computed-style-var-cache>",
                                            &result)
        || strcmp(result.summary, expected) != 0) {
        printf("var cache: expected %s got %s %s\n", expected,
               result.summary, result.error);
        return false;
    }
    return true;
}

static bool count_style_layout_flush(void *opaque)
{
    (*(size_t *) opaque)++;
    return false;
}

/* Names the computed-style getter answers without the element's cascade:
   one nothing registers (the page reads scroll-margin-block-start; the
   script falls back to the inline declaration) and custom properties,
   which come from custom-property rules. Values are unchanged; the
   cascade miss counter proves no element was resolved. */
static int test_computed_style_reads_without_cascade(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>:root{--tone: rgb(1, 2, 3)}"
        ".a{--gap: calc(var(--unit) * 2);--unit:4px}"
        ".a::before{--mark:'x';content:var(--mark)}"
        "p{color:var(--tone)}</style><body><div class=a>"
        "<p id=x style='scroll-margin-block-start: 5px'>1</p>"
        "<svg><rect id=r fill=red width=1 height=1></rect></svg></div>");
    CHECK(runtime != NULL);
    size_t misses = runtime->bridge.computed_style_cache.misses;
    CHECK(var_cache_eval(runtime,
        "(()=>{const x=document.getElementById('x'),s=getComputedStyle(x),"
        "b=getComputedStyle(x.parentElement,'::before');"
        "pocSummary=[s.getPropertyValue('scroll-margin-block-start'),"
        "s.getPropertyValue('--tone'),s.getPropertyValue('--gap'),"
        "s.getPropertyValue('--missing'),b.getPropertyValue('--mark'),"
        "s.getPropertyValue('no-such-property')].join('|')})()",
        "5px|rgb(1, 2, 3)|calc(4px * 2)||'x'|"));
    CHECK(runtime->bridge.computed_style_cache.misses == misses);
    /* Registered properties and SVG presentation attributes still resolve
       through the cascade. */
    CHECK(var_cache_eval(runtime,
        "pocSummary=getComputedStyle(document.getElementById('x')).color+'|'"
        "+getComputedStyle(document.getElementById('r')).fill",
        "rgb(1, 2, 3)|red"));
    CHECK(runtime->bridge.computed_style_cache.misses > misses);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* Misses a script's reads of `id`'s color cost, and whether it reads
   `expected`. */
static bool sibling_scope_read(ScriptRuntime *runtime, const char *id,
                               const char *expected, size_t *misses)
{
    char source[256];
    snprintf(source, sizeof(source),
             "pocSummary=getComputedStyle(document.getElementById('%s'))"
             ".color", id);
    size_t before = runtime->bridge.computed_style_cache.misses;
    bool ok = var_cache_eval(runtime, source, expected);
    *misses = runtime->bridge.computed_style_cache.misses - before;
    return ok;
}

/* Changes that sibling and positional tests (and :has()) let reach other
   elements drop those elements' own cached styles; their descendants are
   checked against the parents' fresh styles, so an unchanged sibling keeps
   its subtree's chain (layout's reuse cache does the same). Everything a
   change can restyle still reads fresh: a sibling test reaching
   descendants, a custom property a sibling or positional rule sets, a
   :has() subject, and siblings an :empty test reaches. */
static int test_computed_style_sibling_scopes(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>"
        ".on + .other{color:rgb(1,1,1)}"
        ".on + .deep-host .deep{color:rgb(2,2,2)}"
        ".on + .var-host{--c:rgb(3,3,3)}.var-use{color:var(--c,rgb(9,9,9))}"
        "li:first-child{color:rgb(4,4,4)}"
        ".first-var:first-child{--f:rgb(5,5,5)}.f-use{color:var(--f,rgb(8,8,8))}"
        ".box:has(.hot){color:rgb(6,6,6)}"
        ".e:empty + .after{color:rgb(7,7,7)}"
        "</style><body>"
        "<main><div id=a></div>"
        "<div id=b><div><div><div><p id=far>x</p></div></div></div></div>"
        "</main><section><div id=a2></div>"
        "<div id=h class=deep-host><p id=dp class=deep>d</p></div>"
        "<div id=v><p id=vp class=var-use>v</p></div></section>"
        "<ul id=list><li id=l1>1</li><li id=l2><span id=l2s>s</span></li>"
        "</ul>"
        "<div id=fl><div id=fv class=first-var><p id=fu class=f-use>f</p>"
        "</div></div>"
        "<div class=box id=box><i id=kid>k</i></div>"
        "<section><div id=e class=e></div><p id=after class=after>t</p>"
        "</section></body>");
    CHECK(runtime != NULL);
    size_t misses = 0;
    /* Warm the chains. */
    CHECK(sibling_scope_read(runtime, "far", "rgb(0, 0, 0)", &misses));
    CHECK(sibling_scope_read(runtime, "dp", "rgb(0, 0, 0)", &misses));
    CHECK(sibling_scope_read(runtime, "vp", "rgb(9, 9, 9)", &misses));
    CHECK(sibling_scope_read(runtime, "l2s", "rgb(0, 0, 0)", &misses));
    CHECK(sibling_scope_read(runtime, "fu", "rgb(5, 5, 5)", &misses));
    CHECK(sibling_scope_read(runtime, "after", "rgb(7, 7, 7)", &misses));
    /* A sibling test that matches nothing new: #b and <main> restyle
       unchanged, #far's chain below them holds. The parent's whole
       subtree used to go (four more cascades). A test reaching
       descendants concerns only <section>'s children. */
    CHECK(var_cache_eval(runtime,
        "document.getElementById('a').className='on';pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "far", "rgb(0, 0, 0)", &misses));
    CHECK(misses <= 2);
    /* What the change does restyle reads fresh. */
    CHECK(var_cache_eval(runtime,
        "document.getElementById('a2').className='on';pocSummary='ok'",
        "ok"));
    CHECK(sibling_scope_read(runtime, "dp", "rgb(2, 2, 2)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('v').className='var-host';pocSummary='ok'",
        "ok"));
    CHECK(sibling_scope_read(runtime, "vp", "rgb(9, 9, 9)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('v').before(document.getElementById('a2'));"
        "pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "vp", "rgb(3, 3, 3)", &misses));
    /* Positions: an insertion before the first item restyles l2's
       subtree only through l2's own style; a positional custom-property
       rule restyles its element's subtree. */
    CHECK(var_cache_eval(runtime,
        "document.getElementById('list').prepend("
        "document.createElement('li'));pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "l2s", "rgb(0, 0, 0)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('l1').remove();"
        "document.getElementById('list').firstElementChild.remove();"
        "pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "l2s", "rgb(4, 4, 4)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('fl').prepend(document.createElement('b'));"
        "pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "fu", "rgb(8, 8, 8)", &misses));
    /* A :has() answer far from #far drops the subject, not every style. */
    CHECK(sibling_scope_read(runtime, "far", "rgb(0, 0, 0)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('kid').className='hot';pocSummary='ok'",
        "ok"));
    CHECK(sibling_scope_read(runtime, "box", "rgb(6, 6, 6)", &misses));
    CHECK(sibling_scope_read(runtime, "far", "rgb(0, 0, 0)", &misses));
    CHECK(misses <= 1);
    /* An element's :empty read by a sibling test restyles its sibling
       (the parent's subtree used to be all that went). */
    CHECK(sibling_scope_read(runtime, "after", "rgb(7, 7, 7)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('e').append('x');pocSummary='ok'", "ok"));
    CHECK(sibling_scope_read(runtime, "after", "rgb(0, 0, 0)", &misses));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('e').textContent='';pocSummary='ok'",
        "ok"));
    CHECK(sibling_scope_read(runtime, "after", "rgb(7, 7, 7)", &misses));
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A section replacement detaches the page's document, destroys its tree
   wholesale (no per-node removal reaches the removal listener) and binds
   the next one. getComputedStyle's retained styles held the old tree's
   node addresses; the next mutation note followed them into freed memory
   (a nondeterministic crash in experimental-section-link-activation).
   Detaching must forget every retained node. */
static int test_computed_style_forgets_detached_document(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument first = {0}, second = {0};
    Stylesheet first_sheet = {0}, second_sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &first, &first_sheet,
        "<!doctype html><style>.on{color:rgb(1,2,3)}</style><body>"
        "<main><div id=a class=on><p id=p>x</p></div><div id=b></div>"
        "</main></body>");
    CHECK(runtime != NULL);
    CHECK(var_cache_eval(runtime,
        "pocSummary=getComputedStyle(document.getElementById('p')).color+"
        "getComputedStyle(document.getElementById('a')).color",
        "rgb(1, 2, 3)rgb(1, 2, 3)"));
    lxb_dom_node_t *root = lxb_dom_interface_node(first.html);
    lxb_dom_node_t *held[] = {find_element_id(root, "a"),
                              find_element_id(root, "p")};
    for (size_t i = 0; i < 2; i++)
        CHECK(held[i] != NULL
              && js_rt_bridge_computed_style_cache_holds(&runtime->bridge,
                                                         held[i]));
    script_runtime_detach_document(runtime, &first);
    for (size_t i = 0; i < 2; i++)
        CHECK(!js_rt_bridge_computed_style_cache_holds(&runtime->bridge,
                                                       held[i]));
    stylesheet_destroy(&first_sheet);
    document_destroy(&first);
    /* The next document: a mutation note and fresh reads touch only it. */
    static const char next_html[] =
        "<!doctype html><style>.on{color:rgb(4,5,6)}</style><body>"
        "<section><div id=a><p id=p>y</p></div></section></body>";
    CHECK(document_parse(&second, &budget, next_html, strlen(next_html), 31));
    CHECK(stylesheet_build(&second_sheet, &budget, &second, 480));
    ScriptResult result = {0};
    CHECK(script_runtime_rebind_document(runtime, &second, &result));
    script_runtime_set_stylesheet(runtime, &second_sheet);
    CHECK(var_cache_eval(runtime,
        "document.getElementById('a').setAttribute('class','on');"
        "pocSummary=getComputedStyle(document.getElementById('p')).color",
        "rgb(4, 5, 6)"));
    script_runtime_destroy(runtime);
    stylesheet_destroy(&second_sheet);
    document_destroy(&second);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_inline_style_scoped_invalidation(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>.watch:has(.hot){color:red}"
        "#other{color:rgb(1,2,3)}#child{color:var(--tone,blue)}</style>"
        "<body><div class=watch><span id=edit><i id=child>c</i></span></div>"
        "<section><span id=other>o</span></section></body>");
    CHECK(runtime != NULL);
    size_t full_before = runtime->bridge.computed_style_cache.full_clears;
    CHECK(var_cache_eval(runtime,
        "(()=>{const edit=document.getElementById('edit'),"
        "child=document.getElementById('child'),other=document.getElementById('other');"
        "for(let i=0;i<8;i++){if(getComputedStyle(other).color!=='rgb(1, 2, 3)')"
        "throw Error('other');edit.style.setProperty('--tone',i%2?'red':'blue');"
        "if(getComputedStyle(child).color!==(i%2?'rgb(255, 0, 0)':'rgb(0, 0, 255)'))"
        "throw Error('inherit');}pocSummary='INLINE-SCOPED-OK'})()",
        "INLINE-SCOPED-OK"));
    /* Only the cold cache starts empty. Unrelated :has() selectors must not
       turn each CSSOM property write into whole-document invalidation. */
    printf("inline-style cache: full=%zu scoped=%zu misses=%zu\n",
           runtime->bridge.computed_style_cache.full_clears - full_before,
           runtime->bridge.computed_style_cache.scoped_clears,
           runtime->bridge.computed_style_cache.misses);
    CHECK(runtime->bridge.computed_style_cache.full_clears - full_before == 1);
    CHECK(!runtime->bridge.mutations.relational_selector_sensitive);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);

    /* Separate sheets: a plain [style] dependency must not accidentally
       make the escaped-name case pass by invalidating on its behalf. */
    for (unsigned escaped = 0; escaped < 2; escaped++) {
        char html[320];
        snprintf(html, sizeof(html),
            "<!doctype html><style>.watch{color:blue}"
            ".watch:has([%s*='red']){color:red}</style>"
            "<body><div class=watch id=watch><span id=edit>x</span></div></body>",
            escaped ? "s\\74 yle" : "style");
        runtime = var_cache_runtime(&budget, &document, &sheet, html);
        CHECK(runtime != NULL);
        CHECK(var_cache_eval(runtime,
        "(()=>{const p=document.getElementById('watch'),e=document.getElementById('edit');"
        "if(getComputedStyle(p).color!=='rgb(0, 0, 255)')throw Error('initial');"
        "e.style.color='red';if(getComputedStyle(p).color!=='rgb(255, 0, 0)')"
        "throw Error('style selector');e.style.removeProperty('color');"
        "if(getComputedStyle(p).color!=='rgb(0, 0, 255)')throw Error('remove');"
        "pocSummary='INLINE-RELATIONAL-OK'})()", "INLINE-RELATIONAL-OK"));
        CHECK(runtime->bridge.mutations.relational_selector_sensitive);
        script_runtime_destroy(runtime);
        stylesheet_destroy(&sheet);
        document_destroy(&document);
        CHECK(budget.current == 0);
    }
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

void style_test_inject_focus_marker_remove_failures(size_t count);

/* One read of #id's `property` in its own entry into JavaScript. It must
   print `expected`; `warm` says it must be served from retained styles (no
   cascade miss), 0 that it must cascade again, -1 either. */
static bool host_style_read(ScriptRuntime *runtime, const char *id,
                            const char *property, const char *expected,
                            int warm)
{
    char source[192];
    snprintf(source, sizeof(source),
             "globalThis.pocSummary=getComputedStyle("
             "document.getElementById('%s')).%s", id, property);
    size_t misses = runtime->bridge.computed_style_cache.misses;
    ScriptResult result = {0};
    bool evaluated = script_runtime_evaluate_diagnostic(
        runtime, source, "<host-style-read>", &result);
    size_t missed = runtime->bridge.computed_style_cache.misses - misses;
    bool ok = evaluated && strcmp(result.summary, expected) == 0
        && (warm < 0 || (missed == 0) == (warm != 0));
    if (!ok)
        printf("host style read #%s.%s: got '%s' want '%s' misses=%zu "
               "warm=%d %s\n", id, property, result.summary, expected,
               missed, (int) warm, result.error);
    return ok;
}

static bool host_style_set_attribute(lxb_dom_node_t *node, const char *name,
                                     const char *value)
{
    return node != NULL && lxb_dom_element_set_attribute(
        lxb_dom_interface_element(node), (const lxb_char_t *) name,
        strlen(name), (const lxb_char_t *) value, strlen(value)) != NULL;
}

/* Computed styles are retained across entries into JavaScript. A host turn
   that changes no style input keeps them; each host-side input drops them:
   native attribute writes (controller checked state, a focus marker a
   probe fails to restore), stylesheet loads, colour-scheme and viewport
   changes, native removal (which also evicts the node), and animation
   steps. Host writes to detached trees, the focus probe and script's own
   journaled writes leave the host generation alone. */
static int test_computed_style_host_generation(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>#a{color:rgb(1,2,3)}.hot #a{color:rgb(4,5,6)}"
        "#a:focus{color:rgb(7,7,7)}#check:checked+#label{color:rgb(9,9,9)}"
        "#list p:last-child{color:rgb(8,8,8)}"
        "@media (max-width:300px){#wrap #a{color:rgb(3,3,3)}}</style>"
        "<body><div id=wrap><span id=a>a</span></div>"
        "<input type=checkbox id=check><span id=label>l</span>"
        "<div id=list><p id=first>1</p><p id=second>2</p></div>"
        "<div id=stage><div id=fade>f</div></div></body>");
    CHECK(runtime != NULL);
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    lxb_dom_node_t *a = find_element_id(root, "a");
    lxb_dom_node_t *wrap = find_element_id(root, "wrap");
    lxb_dom_node_t *check = find_element_id(root, "check");
    lxb_dom_node_t *first = find_element_id(root, "first");
    CHECK(a != NULL && wrap != NULL && check != NULL && first != NULL);
    ScriptResult result = {0};

    /* Survives entries and an empty host turn. */
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 2, 3)", false));
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 2, 3)", true));
    CHECK(script_runtime_advance(runtime, 16, 8, &result));
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 2, 3)", true));

    /* A detached construction tree (as for an SVG raster clone) and the
       focus-state probe, which restores its marker, change nothing. */
    lxb_dom_element_t *loose = lxb_dom_document_create_element(
        &document.html->dom_document, (const lxb_char_t *) "div", 3, NULL);
    lxb_dom_element_t *loose_child = lxb_dom_document_create_element(
        &document.html->dom_document, (const lxb_char_t *) "i", 1, NULL);
    CHECK(loose != NULL && loose_child != NULL
          && host_style_set_attribute(lxb_dom_interface_node(loose),
                                      "class", "hot")
          && lxb_dom_node_append_child(lxb_dom_interface_node(loose),
                 lxb_dom_interface_node(loose_child))
             == LXB_DOM_EXCEPTION_OK);
    lxb_dom_node_destroy_deep(lxb_dom_interface_node(loose));
    ComputedStyle normal, focused;
    (void) style_focus_change_classify(&sheet, a, NULL, &normal, &focused);
    CHECK(document_attribute(a, "data-tilefinch-focus", &(size_t){0})
          == NULL);
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 2, 3)", true));

    /* A probe that cannot restore the marker leaves the element focused. */
    style_test_inject_focus_marker_remove_failures(3);
    CHECK(style_focus_change_classify(&sheet, a, NULL, &normal, &focused)
          == STYLE_FOCUS_CHANGE_UNSAFE);
    style_test_inject_focus_marker_remove_failures(0);
    CHECK(host_style_read(runtime, "a", "color", "rgb(7, 7, 7)", false));
    CHECK(lxb_dom_element_remove_attribute(lxb_dom_interface_element(a),
              (const lxb_char_t *) "data-tilefinch-focus",
              sizeof("data-tilefinch-focus") - 1u) == LXB_STATUS_OK);
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 2, 3)", false));

    /* Native attribute writes: an ancestor class, as the parser or reader
       mode make, and the checked attribute the controller toggles. */
    CHECK(host_style_set_attribute(wrap, "class", "hot"));
    CHECK(host_style_read(runtime, "a", "color", "rgb(4, 5, 6)", false));
    CHECK(host_style_read(runtime, "label", "color", "rgb(0, 0, 0)", false));
    CHECK(host_style_read(runtime, "label", "color", "rgb(0, 0, 0)", true));
    CHECK(host_style_set_attribute(check, "checked", ""));
    CHECK(host_style_read(runtime, "label", "color", "rgb(9, 9, 9)", false));

    /* A stylesheet load. */
    static const char loaded[] = "#wrap #a{color:rgb(5,5,5)}";
    CHECK(stylesheet_add_css(&sheet, loaded, sizeof(loaded) - 1u));
    CHECK(host_style_read(runtime, "a", "color", "rgb(5, 5, 5)", false));
    CHECK(host_style_read(runtime, "a", "color", "rgb(5, 5, 5)", true));

    /* A colour-scheme change reaches the generation before any rebuild;
       a viewport change rebuilds the sheet in place. */
    uint64_t generation = document_style_generation();
    bool dark = stylesheet_prefers_dark_color_scheme();
    stylesheet_set_prefers_dark_color_scheme(!dark);
    CHECK(document_style_generation() != generation);
    stylesheet_set_prefers_dark_color_scheme(dark);
    CHECK(host_style_read(runtime, "a", "color", "rgb(5, 5, 5)", false));
    stylesheet_destroy(&sheet);
    CHECK(stylesheet_build(&sheet, &budget, &document, 240));
    CHECK(host_style_read(runtime, "a", "color", "rgb(3, 3, 3)", false));
    CHECK(host_style_read(runtime, "a", "color", "rgb(3, 3, 3)", true));

    /* Native removal evicts the node before its address can be reused. */
    CHECK(host_style_read(runtime, "first", "color", "rgb(0, 0, 0)", false));
    CHECK(host_style_read(runtime, "second", "color", "rgb(8, 8, 8)",
                          false));
    CHECK(js_rt_bridge_computed_style_cache_holds(&runtime->bridge, first));
    lxb_dom_node_destroy_deep(first);
    CHECK(!js_rt_bridge_computed_style_cache_holds(&runtime->bridge, first));
    CHECK(host_style_read(runtime, "second", "color", "rgb(8, 8, 8)",
                          false));

    /* Animation steps are script writes the host drives: each step
       restyles the animated element and nothing else. */
    CHECK(host_style_read(runtime, "a", "color", "rgb(3, 3, 3)", false));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "document.getElementById('fade').animate([{opacity:'0'},"
        "{opacity:'1'}],{duration:64,iterations:1});"
        "globalThis.pocSummary=getComputedStyle("
        "document.getElementById('fade')).opacity",
        "<host-style-animate>", &result));
    char before[32];
    snprintf(before, sizeof(before), "%s", result.summary);
    CHECK(script_runtime_advance(runtime, 32, 8, &result));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.pocSummary=getComputedStyle("
        "document.getElementById('fade')).opacity",
        "<host-style-animate-step>", &result));
    if (strcmp(result.summary, before) == 0)
        printf("animation step: opacity stayed %s\n", before);
    CHECK(strcmp(result.summary, before) != 0);
    CHECK(host_style_read(runtime, "a", "color", "rgb(3, 3, 3)", true));

    /* Script's own writes are journaled, not host changes. */
    generation = document_style_generation();
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const list=document.getElementById('list'),"
        "i=document.createElement('i');list.appendChild(i);"
        "i.setAttribute('class','x');i.style.color='red';i.textContent='t';"
        "list.insertBefore(document.createElement('b'),i);"
        "list.firstChild.remove();list.innerHTML+='<u>u</u>';"
        "document.getElementById('label').removeAttribute('id');"
        "globalThis.pocSummary='SCRIPT-WRITES'})()",
        "<host-style-script-writes>", &result)
          && strcmp(result.summary, "SCRIPT-WRITES") == 0);
    CHECK(document_style_generation() == generation);

    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The streaming parser is a host style input: a sibling it appends ends an
   earlier element's :last-child match between two entries. */
static int test_computed_style_parser_insertion(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char head[] =
        "<!doctype html><style>p{color:rgb(1,1,1)}"
        "p:last-child{color:rgb(2,2,2)}</style><body><div id=list>"
        "<p id=a>a</p>";
    static const char tail[] = "<p id=b>b</p></div></body>";
    DocumentParser parser = {0};
    CHECK(document_parser_begin(&parser, &budget)
          && document_parser_feed(&parser, head, sizeof(head) - 1u));
    Stylesheet sheet = {0};
    CHECK(stylesheet_build(&sheet, &budget, &parser.document, 480));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &parser.document, &budget, 6u * MIB, 4000, "https://parse.test/",
        &options, &result);
    CHECK(runtime != NULL);
    script_runtime_set_stylesheet(runtime, &sheet);
    CHECK(host_style_read(runtime, "a", "color", "rgb(2, 2, 2)", false));
    CHECK(host_style_read(runtime, "a", "color", "rgb(2, 2, 2)", true));
    CHECK(document_parser_feed(&parser, tail, sizeof(tail) - 1u));
    CHECK(host_style_read(runtime, "a", "color", "rgb(1, 1, 1)", false));
    CHECK(host_style_read(runtime, "b", "color", "rgb(2, 2, 2)", false));
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_parser_abort(&parser);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The engine's own host paths, end to end: a committed page's retained
   styles survive runtime advances, and follow controller focus moves, a
   checkbox activation, contenteditable typing and a form value edit. */
static int test_computed_style_controller_host_changes(void)
{
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    static const char page[] =
        "<!doctype html><style>#check:checked+#label{color:rgb(9,9,9)}"
        "#field:focus{color:rgb(4,5,6)}#edit:empty{color:rgb(1,1,1)}"
        "#other{color:rgb(7,7,7)}</style><body>"
        "<p id=other>other</p><form><input type=checkbox id=check>"
        "<span id=label>label</span><input id=field value=a></form>"
        "<div id=edit contenteditable></div></body>";
    static NavigationSession navigation;
    CHECK(navigation_init(&navigation, &budget, 2));
    navigation_enable_scripts(&navigation, 4 * MIB, 1000);
    uint64_t generation = navigation_begin(&navigation);
    CHECK(navigation_commit_html(&navigation, generation,
              "https://host-style.test/", page, strlen(page), 480,
              NULL, NULL, true));
    ScriptRuntime *runtime = navigation.page.runtime;
    CHECK(runtime != NULL);
    lxb_dom_node_t *root =
        lxb_dom_interface_node(navigation.page.document.html);
    lxb_dom_node_t *check = find_element_id(root, "check");
    lxb_dom_node_t *field = find_element_id(root, "field");
    lxb_dom_node_t *edit = find_element_id(root, "edit");
    CHECK(check != NULL && field != NULL && edit != NULL);
    CHECK(host_style_read(runtime, "other", "color", "rgb(7, 7, 7)", false));
    CHECK(host_style_read(runtime, "label", "color", "rgb(0, 0, 0)", false));
    CHECK(host_style_read(runtime, "edit", "color", "rgb(1, 1, 1)", false));
    for (int frame = 0; frame < 3; frame++)
        CHECK(navigation_advance_runtime(&navigation, 16, 8));
    CHECK(host_style_read(runtime, "other", "color", "rgb(7, 7, 7)", true));
    CHECK(host_style_read(runtime, "label", "color", "rgb(0, 0, 0)", true));

    BrowserController controller;
    ControllerAction action;
    CHECK(controller_init(&controller, &navigation));
    /* Activation: the controller writes the checked attribute natively. */
    CHECK(controller_focus_node(&controller, check)
          && controller_activate(&controller, &action));
    CHECK(host_style_read(runtime, "label", "color", "rgb(9, 9, 9)", false));
    /* A focus move reaches :focus through the page's focus handling. */
    CHECK(host_style_read(runtime, "field", "color", "rgb(0, 0, 0)", false));
    CHECK(controller_focus_node(&controller, field));
    CHECK(host_style_read(runtime, "field", "color", "rgb(4, 5, 6)", false));
    /* A form value is no style input. */
    CHECK(host_style_read(runtime, "other", "color", "rgb(7, 7, 7)", -1));
    CHECK(host_style_read(runtime, "other", "color", "rgb(7, 7, 7)", true));
    CHECK(controller_insert_text(&controller, "b", 1));
    CHECK(host_style_read(runtime, "other", "color", "rgb(7, 7, 7)", true));
    /* Typing into contenteditable replaces its children natively. */
    CHECK(controller_focus_node(&controller, edit)
          && controller_insert_text(&controller, "x", 1));
    CHECK(host_style_read(runtime, "edit", "color", "rgb(0, 0, 0)", false));
    navigation_destroy(&navigation);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_device_answer_checkpoint(void)
{
    char probe[1025];
    FILE *file = fopen(TILEFINCH_TEST_SOURCE_DIR
        "/tests/input-scripts/chatgpt-ask.until.js", "rb");
    CHECK(file != NULL);
    size_t length = fread(probe, 1, sizeof(probe) - 1, file);
    CHECK(!ferror(file) && fgetc(file) == EOF);
    CHECK(fclose(file) == 0 && length <= 1024);
    probe[length] = '\0';
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><body><div data-message-role=assistant>"
        "<h4>Assistant said:</h4><div role=status>Waiting</div>"
        "<div id=answer data-assistant-markdown></div></div></body>");
    CHECK(runtime != NULL);
    char source[1400];
    int size = snprintf(source, sizeof(source),
        "globalThis.__tfMark='sent';globalThis.answerReady=function(){%s};"
        "pocSummary=String(answerReady())", probe);
    CHECK(size > 0 && (size_t) size < sizeof(source));
    CHECK(var_cache_eval(runtime, source, "false"));
    /* A supervisor can advance past a live MARK before its page diagnostic
       runs. The second wait must not accept the still-ready home composer
       merely because __tfMark has not reached 'sent' yet. */
    CHECK(var_cache_eval(runtime,
        "globalThis.__tfMark='hook';"
        "document.documentElement.dataset.octaneHomeBehaviorReady='1';"
        "pocSummary=String(answerReady())", "true"));
    CHECK(var_cache_eval(runtime,
        "pocSummary=String(answerReady())", "false"));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('answer').textContent='   ';"
        "pocSummary=String(answerReady())", "false"));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('answer').textContent='Hello';"
        "pocSummary=String(answerReady())", "true"));
    /* Received content can still be behind an author presentation gate.
       Never certify the reply checkpoint while its turn remains hidden. */
    CHECK(var_cache_eval(runtime,
        "document.getElementById('answer').parentElement.hidden=true;"
        "pocSummary=String(answerReady())", "false"));
    CHECK(var_cache_eval(runtime,
        "document.getElementById('answer').parentElement.hidden=false;"
        "pocSummary=String(answerReady())", "true"));
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_computed_style_keywords_without_layout(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>.on{position:relative;overflow:clip;"
        "box-sizing:border-box;z-index:7;display:flex;flex-direction:column;"
        "flex-wrap:wrap;align-items:center;align-self:center;"
        "align-content:center;justify-content:center;order:3}</style>"
        "<body><div id=p>x</div></body>");
    CHECK(runtime != NULL);
    size_t flushes = 0;
    bool dirty = false;
    runtime->bridge.relayout_dirty = &dirty;
    script_runtime_set_synchronous_layout_callback(runtime,
        count_style_layout_flush, &flushes);
    CHECK(var_cache_eval(runtime,
        "globalThis.p=document.getElementById('p');"
        "globalThis.cs=getComputedStyle(p);p.className='on';"
        "pocSummary=[cs.position,cs.overflowX,cs.overflowY,cs.overflow,"
        "cs.boxSizing,cs.zIndex,cs.flexDirection,cs.flexWrap,cs.alignItems,"
        "cs.alignSelf,cs.alignContent,cs.justifyContent,cs.order].join('|')",
        "relative|clip|clip|clip|border-box|7|column|wrap|center|center|center|center|3"));
    CHECK(dirty && flushes == 0);
    /* Separate live declarations share their traps, never their state. */
    CHECK(var_cache_eval(runtime,
        "globalThis.other=document.createElement('div');"
        "document.body.append(other);globalThis.otherStyle=getComputedStyle(other);"
        "globalThis.styles=[];pocSummary='WARM'", "WARM"));
    JSMemoryUsage before, after;
    JS_ComputeMemoryUsage(runtime->runtime, &before);
    CHECK(var_cache_eval(runtime,
        "for(let i=0;i<100;i++)styles.push(getComputedStyle(i%2?p:other));"
        "pocSummary='WRAPPERS'", "WRAPPERS"));
    JS_ComputeMemoryUsage(runtime->runtime, &after);
    printf("computed-style wrappers: bytes=%lld\n",
        (long long) (after.memory_used_size - before.memory_used_size));
    /* One target, proxy and state per result, not six fresh trap closures. */
    CHECK(after.memory_used_size - before.memory_used_size < 64 * 1024);
    CHECK(var_cache_eval(runtime,
        "if(cs===styles[1]||styles[0].position!=='static'||"
        "styles[1].position!=='relative')throw Error('shared style state');"
        "other.className='on';if(otherStyle.position!=='relative')"
        "throw Error('not live');other.remove();"
        "if(Object.keys(otherStyle).length||!Object.keys(cs).length||"
        "!('0' in cs)||('0' in otherStyle))throw Error('shared enumeration');"
        "pocSummary='STATE-OK'", "STATE-OK"));
    CHECK(var_cache_eval(runtime, "pocSummary=cs.width", "auto"));
    CHECK(flushes != 0);
    flushes = 0;
    runtime->bridge.mutations.conservative_resource_scan = true;
    CHECK(var_cache_eval(runtime, "pocSummary=cs.position", "relative"));
    CHECK(flushes != 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* getComputedStyle's var() table is created by the first read that
   resolves a var() against a sheet declaring custom properties: plain
   pages (even with var() fallbacks) never allocate it, and mutation-time
   clears stay no-ops. Once created it serves repeated reads and still
   sees custom-property changes through inline style and class changes. */
static int test_computed_style_var_cache_on_demand(void)
{
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    PocDocument document = {0};
    Stylesheet sheet = {0};
    ScriptRuntime *runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>p{color:rgb(1,2,3)}"
        ".f{color:var(--missing, rgb(9, 9, 9))}</style><body>"
        "<div id=wrap><p id=a>a</p><p id=b class=f>b</p></div></body>");
    CHECK(runtime != NULL);
    __typeof__(runtime->bridge.computed_style_cache) *cache =
        &runtime->bridge.computed_style_cache;
    CHECK(var_cache_eval(runtime,
        "(()=>{const $=(i)=>document.getElementById(i),"
        "c=(i)=>getComputedStyle($(i)).color,r=[];"
        "r.push(c('a')==='rgb(1, 2, 3)',c('b')==='rgb(9, 9, 9)');"
        "$('a').className='x';$('wrap').append(document.createElement('p'));"
        "r.push(c('a')==='rgb(1, 2, 3)',c('b')==='rgb(9, 9, 9)');"
        "globalThis.pocSummary=r.every(Boolean)?'PLAIN-OK':"
        "'PLAIN:'+r.map((v,i)=>v?'':i).filter(String).join(',')})()",
        "PLAIN-OK"));
    CHECK(cache->entries != NULL && cache->hits != 0);
    CHECK(cache->variables.cache == NULL && cache->variables.creations == 0
          && cache->variables.clears == 0);
    CHECK(style_variable_cache_lease_bytes(&cache->variables) == 0);
    CHECK(js_rt_bridge_computed_style_cache_bytes(&runtime->bridge) != 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);

    document = (PocDocument) {0};
    sheet = (Stylesheet) {0};
    runtime = var_cache_runtime(&budget, &document, &sheet,
        "<!doctype html><style>:root{--c:rgb(4,4,4)}.alt{--c:rgb(5,5,5)}"
        ".use{color:var(--c)}p{color:rgb(1,2,3)}</style><body>"
        "<section id=wrap><p id=plain>p</p><div id=mid>"
        "<p id=u1 class=use>1</p><p id=u2 class=use>2</p></div></section>"
        "</body>");
    CHECK(runtime != NULL);
    cache = &runtime->bridge.computed_style_cache;
    /* Custom properties alone allocate nothing until a var() resolves. */
    CHECK(var_cache_eval(runtime,
        "globalThis.pocSummary=getComputedStyle("
        "document.getElementById('plain')).color",
        "rgb(1, 2, 3)"));
    CHECK(cache->variables.cache == NULL && cache->variables.creations == 0);
    /* One task: the caches live under one validity (a new evaluation
       may start a new epoch), so the sibling's lookup reuses the chain. */
    CHECK(var_cache_eval(runtime,
        "globalThis.pocSummary=[getComputedStyle("
        "document.getElementById('u1')).color,getComputedStyle("
        "document.getElementById('u2')).color].join()",
        "rgb(4, 4, 4),rgb(4, 4, 4)"));
    CHECK(cache->variables.cache != NULL && cache->variables.creations == 1);
    CHECK(style_variable_cache_lease_bytes(&cache->variables) != 0);
    CHECK(cache->variables.hits != 0);
    CHECK(var_cache_eval(runtime,
        "(()=>{const $=(i)=>document.getElementById(i),"
        "c=(i)=>getComputedStyle($(i)).color,r=[];"
        "document.documentElement.style.setProperty('--c','rgb(8, 8, 8)');"
        "r.push(c('u1')==='rgb(8, 8, 8)',c('u2')==='rgb(8, 8, 8)');"
        "$('wrap').className='alt';"
        "r.push(c('u1')==='rgb(5, 5, 5)',c('u2')==='rgb(5, 5, 5)');"
        "$('mid').style.setProperty('--c','rgb(6, 6, 6)');"
        "r.push(c('u2')==='rgb(6, 6, 6)');"
        "$('mid').style.removeProperty('--c');$('wrap').className='';"
        "r.push(c('u1')==='rgb(8, 8, 8)');"
        "globalThis.pocSummary=r.every(Boolean)?'VAR-OK':"
        "'VAR:'+r.map((v,i)=>v?'':i).filter(String).join(',')})()",
        "VAR-OK"));
    CHECK(cache->variables.creations == 1 && cache->variables.clears != 0);
    script_runtime_destroy(runtime);
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    CHECK(budget.current == 0);
    return 0;
}

static int test_native_dynamic_code_policy(void)
{
#if defined(TILEFINCH_QUICKJS_DYNAMIC_CODE_POLICY)
    Budget budget;
    budget_init(&budget, 12u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body></body></html>";
    PocDocument document;
    CHECK(document_parse(
        &document, &budget, html, sizeof(html) - 1u, sizeof(html)));
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
        .defer_document_scripts = true,
        .dynamic_code_disabled = true,
        .document_scope = SCRIPT_DOCUMENT_SCOPE_TOP_LEVEL
    };
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char blocked[] =
        "(()=>{let blocked=typeof __tilefinchRunWorker==='undefined'?1:0;"
        "const indirect=eval,AsyncFunction="
        "(async function(){}).constructor,GeneratorFunction="
        "(function*(){}).constructor;for(const compile of["
        "()=>eval('1'),()=>indirect('1'),()=>Function('return 1'),"
        "()=>AsyncFunction('return 1'),()=>GeneratorFunction('yield 1')])"
        "{try{compile()}catch(error){if(error instanceof TypeError)blocked++}}"
        "globalThis.pocSummary='DYNAMIC-CODE-BLOCKED:'+blocked})()";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, blocked, "<dynamic-code-blocked>", &result)
          && strcmp(result.summary, "DYNAMIC-CODE-BLOCKED:6") == 0);
    static const char inline_allowed[] =
        "(()=>{const node=document.createElement('button');"
        "node.setAttribute('onclick',\"this.setAttribute('data-fired',"
        "event.type);return false\");document.body.appendChild(node);"
        "const accepted=node.dispatchEvent(new MouseEvent('click',"
        "{cancelable:true}));globalThis.pocSummary=!accepted&&"
        "node.getAttribute('data-fired')==='click'?"
        "'INLINE-HANDLER-ALLOWED':'INLINE-HANDLER-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, inline_allowed, "<inline-handler-allowed>", &result)
          && strcmp(result.summary, "INLINE-HANDLER-ALLOWED") == 0);
    script_runtime_destroy(runtime);

    options.dynamic_code_disabled = false;
    runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char allowed[] =
        "globalThis.pocSummary=eval('20+1')+Function('return 21')()"
        "+(0,eval)('21')===63?'DYNAMIC-CODE-ALLOWED':'FAILED'";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, allowed, "<dynamic-code-allowed>", &result)
          && strcmp(result.summary, "DYNAMIC-CODE-ALLOWED") == 0);
    script_runtime_destroy(runtime);

    static const char csp_blocked[] =
        "content-security-policy: script-src 'none'\n";
    CHECK(tilefinch_csp_parse_response_headers(
        &document.content_security_policy, "https://policy.test/",
        csp_blocked, sizeof(csp_blocked) - 1u, false));
    options.dynamic_code_disabled = true;
    runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000,
        "https://policy.test/", &options, &result);
    static const char inline_blocked[] =
        "(()=>{const node=document.createElement('button');"
        "node.setAttribute('onclick',\"this.setAttribute('data-fired',"
        "'yes')\");document.body.appendChild(node);"
        "const accepted=node.dispatchEvent(new MouseEvent('click',"
        "{cancelable:true}));globalThis.pocSummary=accepted&&"
        "node.getAttribute('data-fired')===null?"
        "'INLINE-HANDLER-BLOCKED':'INLINE-HANDLER-ESCAPED'})()";
    CHECK(runtime != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, inline_blocked, "<inline-handler-blocked>", &result)
          && strcmp(result.summary, "INLINE-HANDLER-BLOCKED") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0);
#endif
    return 0;
}

/* A connected element moved under a detached parent (a framework parking
   a subtree in a fragment) left the page: the journal records the removal
   it is, classified before the move, not an unclassified change of the old
   parent that forces the whole-document resource scan. */
static int test_detached_move_is_a_removal(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><div id=a><span id=s>Text</span></div>"
        "<div id=b><span id=t>More</span></div>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000, "https://mutation.test/", NULL, &result);
    CHECK(runtime != NULL);
    ScriptMutationJournal journal;
    (void) script_runtime_consume_mutations(runtime, &journal);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "var parked=document.createElement('div');"
        "parked.appendChild(document.getElementById('s'));"
        "parked.insertBefore(document.getElementById('t'),null);",
        "<detached-move>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    size_t removals = 0;
    for (size_t i = 0; i < journal.count; i++) {
        const ScriptMutationRecord *record = &journal.records[i];
        CHECK(record->kind != SCRIPT_MUTATION_UNKNOWN);
        if (record->kind == SCRIPT_MUTATION_CHILD_LIST && record->node != NULL
            && record->scope != NULL) removals++;
    }
    CHECK(removals == 2 && !journal.conservative_resource_scan
          && !journal.overflowed);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_coalesced_attribute_tokens(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body><p id=p>Text</p>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 5u * MIB, 4000, "https://mutation.test/", NULL, &result);
    CHECK(runtime != NULL);
    ScriptMutationJournal journal;
    (void) script_runtime_consume_mutations(runtime, &journal);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "var p=document.getElementById('p');p.className='first';p.className='second';",
        "<coalesced-attributes>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    const ScriptMutationRecord *record = NULL;
    for (size_t i = 0; i < journal.count; i++)
        if (strcmp(journal.records[i].attribute, "class") == 0) record = &journal.records[i];
    CHECK(record != NULL && record->changed_tokens_exact);
    bool first = false, second = false;
    for (size_t i = 0; i < record->changed_token_count; i++) {
        const uint32_t *tokens = script_mutation_record_tokens(&journal, record);
        first |= tokens[i] == stylesheet_identity_token_hash(false, "first", 5);
        second |= tokens[i] == stylesheet_identity_token_hash(false, "second", 6);
    }
    CHECK(first && second);
    /* An atomic-CSS swap of dozens of classes stays exact... */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "p.className='';", "<reset-token-probe>", &result));
    (void) script_runtime_consume_mutations(runtime, &journal);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "p.className=Array.from({length:40},(_, i)=>'x'+i).join(' ');"
        "p.className=Array.from({length:40},(_, i)=>'x'+(i+10)).join(' ');",
        "<coalesced-atomic-swap>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    record = NULL;
    for (size_t i = 0; i < journal.count; i++)
        if (strcmp(journal.records[i].attribute, "class") == 0) record = &journal.records[i];
    CHECK(record != NULL && record->changed_tokens_exact
          && record->changed_token_count == 50);
    /* ...but a record never holds more than the per-record limit. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "p.className='small';p.className=Array.from({length:70},(_, i)=>'y'+i).join(' ');",
        "<coalesced-token-overflow>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    for (size_t i = 0; i < journal.count; i++)
        if (strcmp(journal.records[i].attribute, "class") == 0)
            CHECK(!journal.records[i].changed_tokens_exact);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "p.className='';", "<reset-token-probe>", &result));
    (void) script_runtime_consume_mutations(runtime, &journal);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "for(let i=0;i<70;i++)p.className='token'+i;",
        "<coalesced-union-overflow>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    record = NULL;
    for (size_t i = 0; i < journal.count; i++)
        if (strcmp(journal.records[i].attribute, "class") == 0) record = &journal.records[i];
    CHECK(record != NULL && !record->changed_tokens_exact
          && record->changed_token_count == 0);
    /* The shared pool is bounded: once a turn has spent it, later class
       changes are recorded inexact (conservative), never truncated. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "p.className='';for(let e=0;e<20;e++){const d=document.createElement('div');"
        "document.body.appendChild(d);}",
        "<pool-setup>", &result));
    (void) script_runtime_consume_mutations(runtime, &journal);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "document.querySelectorAll('div').forEach((d,e)=>{"
        "d.className=Array.from({length:60},(_, i)=>'z'+e+'_'+i).join(' ');});",
        "<pool-exhaustion>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    size_t exact = 0, inexact = 0;
    for (size_t i = 0; i < journal.count; i++) {
        if (strcmp(journal.records[i].attribute, "class") != 0) continue;
        if (journal.records[i].changed_tokens_exact) {
            exact++;
            CHECK(journal.records[i].changed_token_count == 60
                  && journal.records[i].changed_token_offset + 60u
                         <= SCRIPT_MUTATION_TOKEN_POOL);
        } else {
            inexact++;
        }
    }
    CHECK(exact == SCRIPT_MUTATION_TOKEN_POOL / 60u && exact + inexact == 20
          && journal.token_count <= SCRIPT_MUTATION_TOKEN_POOL);
    /* This is a renderer invalidation journal, not MutationObserver's
       author-visible record queue. Long names share a conservative prefix
       dependency and must not consume a slot on every repeated write. */
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "for(let i=0;i<300;i++){"
        "p.setAttribute('data-long-render-dependency-attribute-one',String(i));"
        "p.setAttribute('data-long-render-dependency-attribute-two',String(i));}",
        "<long-attribute-coalescing>", &result));
    CHECK(script_runtime_consume_mutations(runtime, &journal));
    CHECK(!journal.overflowed && journal.count == 1);
    CHECK(strlen(journal.records[0].attribute)
          == SCRIPT_MUTATION_ATTRIBUTE_LIMIT - 1u);
    CHECK(!journal.records[0].changed_tokens_exact);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static ScriptRuntime *host_state_runtime(Budget *budget, PocDocument *document,
                                         ScriptResult *result)
{
    static const char html[] = "<!doctype html><body>Ready</body>";
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    if (!document_parse(document, budget, html, sizeof(html) - 1u, 17)
        || !viewport_context_init(&viewport, 480, 272, 480, 272)
        || !script_execution_policy_for_profile(
            SCRIPT_EXECUTION_PROFILE_LAB, &policy)) return NULL;
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true,
        .allow_test_network_primitive_overrides = true };
    return script_runtime_create_configured(
        document, budget, 6u * MIB, 4000, "https://host-state.test/",
        &options, result);
}

static bool host_state_eval(ScriptRuntime *runtime, const char *source,
                            const char *expected, ScriptResult *result)
{
    if (script_runtime_evaluate_diagnostic(runtime, source, "<host-state>",
                                           result)
        && strcmp(result->summary, expected) == 0) return true;
    fprintf(stderr, "host state: expected %s got %s %s\n", expected,
            result->summary, result->error);
    return false;
}

/* Advances the scheduler clock by `ms` in either mode: the elapsed argument
   drives the caller-driven wheel, the platform clock the sampled one. */
static bool host_state_step(ScriptRuntime *runtime, TimedCooperate *clock,
                            unsigned ms, ScriptResult *result)
{
    clock->now_ns += (uint64_t) ms * UINT64_C(1000000);
    return script_runtime_advance(runtime, ms, 16, result);
}

/* The event loop answers "is a timer due" from the wheel size, head and
   clock scheduler.js publishes in host_state, and flushes page scroll only
   when the published flag is set. A timer must still run on the exact turn
   it falls due, a zero-delay chain within one turn, and a hidden page must
   still pass over its due frame timer to a due author timer. */
static int test_host_state_timers_and_scroll(bool sampled_clock)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    TimedCooperate clock = { .now_ns = UINT64_C(1000000000) };
    TilefinchPlatformServices services = {
        .context = &clock, .monotonic_time_ns = timed_cooperate_clock,
        .cooperate = timed_cooperate_poll
    };
    tilefinch_platform_set_services(&services);
    script_runtime_configure_wall_clock_timers(sampled_clock);
    PocDocument document;
    ScriptResult result = {0};
    ScriptRuntime *runtime = host_state_runtime(&budget, &document, &result);
    CHECK(runtime != NULL);
    const double *state = runtime->host_state;
    CHECK(state[SCRIPT_HOST_STATE_SAMPLED_CLOCK] == (sampled_clock ? 1 : 0));
    CHECK(host_state_eval(runtime,
        "globalThis.log=[];setTimeout(()=>log.push('t30'),30);"
        "globalThis.pocSummary=[typeof __tilefinchHostChannel,"
        "typeof __tilefinchPendingWork].join()",
        "undefined,undefined", &result));
    CHECK(runtime->pending_timer_tasks == 1 && result.pending_tasks == 1
          && state[SCRIPT_HOST_STATE_TIMERS] == 1);
    CHECK(host_state_step(runtime, &clock, 29, &result)
          && host_state_eval(runtime, "pocSummary=log.join()", "", &result));
    CHECK(host_state_step(runtime, &clock, 1, &result)
          && host_state_eval(runtime, "pocSummary=log.join()", "t30", &result)
          && runtime->pending_timer_tasks == 0
          && state[SCRIPT_HOST_STATE_TIMERS] == 0);

    CHECK(host_state_eval(runtime,
        "setTimeout(()=>{log.push('a');setTimeout(()=>log.push('b'),0)},0);"
        "pocSummary=''", "", &result));
    CHECK(host_state_step(runtime, &clock, 0, &result)
          && host_state_eval(runtime, "pocSummary=log.join()", "t30,a,b",
                             &result));

    CHECK(host_state_eval(runtime,
        "requestAnimationFrame(()=>log.push('raf'));"
        "setTimeout(()=>log.push('t20'),20);pocSummary=''", "", &result));
    CHECK(script_runtime_set_page_visibility(runtime, false)
          && host_state_step(runtime, &clock, 20, &result)
          && host_state_eval(runtime, "pocSummary=log.join()",
                             "t30,a,b,t20", &result)
          && runtime->pending_timer_tasks == 1);
    CHECK(script_runtime_set_page_visibility(runtime, true)
          && host_state_step(runtime, &clock, 0, &result)
          && host_state_eval(runtime, "pocSummary=log.join()",
                             "t30,a,b,t20,raf", &result));

    CHECK(host_state_eval(runtime,
        "log.length=0;globalThis.iv=setInterval(()=>log.push('i'),10);"
        "setTimeout(()=>log.push('late'),1000);pocSummary=''", "",
        &result));
    for (int i = 0; i < 3; i++)
        CHECK(host_state_step(runtime, &clock, 10, &result));
    CHECK(host_state_eval(runtime, "pocSummary=log.join()", "i,i,i",
                          &result)
          && runtime->pending_timer_tasks == 2);
    CHECK(host_state_eval(runtime, "clearInterval(iv);pocSummary=''", "",
                          &result)
          && runtime->pending_timer_tasks == 1
          && state[SCRIPT_HOST_STATE_TIMERS] == 1);
    CHECK(host_state_eval(runtime,
        "for(let id=1;id<64;id++)clearTimeout(id);pocSummary=''", "",
        &result)
          && runtime->pending_timer_tasks == 0
          && state[SCRIPT_HOST_STATE_EARLIEST_DUE] == INFINITY);

    CHECK(host_state_eval(runtime,
        "globalThis.scrolls=0;addEventListener('scroll',()=>scrolls++);"
        "visualViewport.addEventListener('scroll',()=>scrolls++);"
        "pocSummary=''", "", &result)
          && state[SCRIPT_HOST_STATE_SCROLL_PENDING] == 0);
    CHECK(script_runtime_set_page_scroll(runtime, 40)
          && state[SCRIPT_HOST_STATE_SCROLL_PENDING] == 1
          && host_state_eval(runtime, "pocSummary=scrolls+':'+scrollY",
                             "0:40", &result));
    CHECK(host_state_step(runtime, &clock, 0, &result)
          && state[SCRIPT_HOST_STATE_SCROLL_PENDING] == 0
          && host_state_step(runtime, &clock, 0, &result)
          && host_state_eval(runtime, "pocSummary=String(scrolls)", "2",
                             &result));
    CHECK(script_runtime_set_page_scroll(runtime, 40)
          && state[SCRIPT_HOST_STATE_SCROLL_PENDING] == 0);

    script_runtime_destroy(runtime);
    document_destroy(&document);
    script_runtime_configure_wall_clock_timers(false);
    tilefinch_platform_set_services(NULL);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static bool host_state_network_matches(ScriptRuntime *runtime,
                                       ScriptResult *result)
{
    char expected[256];
    snprintf(expected, sizeof(expected), "%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu",
             result->async_network_logical_admitted,
             result->async_network_logical_completed,
             result->async_network_logical_rejected,
             result->async_network_logical_cancelled,
             result->async_network_logical_timed_out,
             result->async_network_logical_peak,
             result->async_network_logical_peak_bytes,
             result->async_network_active_native,
             result->async_network_pending_logical);
    return host_state_eval(runtime,
        "{const s=__tilefinchNetworkQueueStats;pocSummary=[s.admitted,"
        "s.completed,s.rejected,s.cancelled,s.timedOut,s.peakCount,"
        "s.peakBytes,s.active,s.waiting].join()}", expected, result);
}

/* The result snapshot and the pending-work count read the plain records
   behind the network-queue and IndexedDB stat views. Every reported field
   must equal the view the page-side diagnostics read. */
static int test_host_state_network_and_indexeddb_stats(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    ScriptResult result = {0};
    ScriptRuntime *runtime = host_state_runtime(&budget, &document, &result);
    CHECK(runtime != NULL && JS_IsObject(runtime->host_network_queue_stats)
          && JS_IsUndefined(runtime->host_indexed_db_stats));
    CHECK(host_state_eval(runtime,
        "let nextNative=5000;globalThis.__tilefinchFetchAsync=()=>nextNative++;"
        "globalThis.__tilefinchCancelNetwork=()=>true;"
        "globalThis.aborts=[];for(let i=0;i<6;i++){const c=new AbortController;"
        "aborts.push(c);fetch('https://host-state.test/w/'+i,{signal:"
        "c.signal}).catch(()=>{});}pocSummary=''", "", &result));
    CHECK(script_runtime_advance(runtime, 0, 16, &result));
    CHECK(result.async_network_active_native == 4
          && result.async_network_pending_logical == 2
          && runtime->pending_network_tasks == 2
          && result.pending_tasks >= 2
          && host_state_network_matches(runtime, &result));
    CHECK(host_state_eval(runtime,
        "for(const c of aborts)c.abort();pocSummary=''", "", &result)
          && script_runtime_advance(runtime, 0, 16, &result)
          && result.async_network_pending_logical == 0
          && result.async_network_logical_cancelled == 6
          && runtime->pending_network_tasks == 0
          && host_state_network_matches(runtime, &result));

    static const char indexeddb_probe[] =
        "(async()=>{const request=r=>new Promise((ok,fail)=>{r.onsuccess="
        "()=>ok(r.result);r.onerror=()=>fail(r.error);});"
        "const opening=indexedDB.open('host-state',1);opening.onupgradeneeded="
        "()=>opening.result.createObjectStore('items');"
        "const db=await request(opening),tx=db.transaction('items',"
        "'readwrite');await request(tx.objectStore('items').put("
        "'x'.repeat(100),1));await new Promise(ok=>tx.oncomplete=ok);"
        "db.close();const s=__tilefinchIndexedDBStats;"
        "pocSummary='IDB:'+[s.opens,s.deletes,s.transactions,s.requests,"
        "s.records,s.bytes,s.peakBytes,s.quotaErrors].join()+':'+"
        "typeof __tilefinchHostChannel;})().catch(error=>{pocSummary="
        "'IDB-ERROR:'+error})";
    CHECK(script_runtime_evaluate_diagnostic(
        runtime, indexeddb_probe, "<host-state-idb>", &result));
    for (size_t tick = 0; tick < 16
         && strncmp(result.summary, "IDB", 3) != 0; tick++)
        CHECK(script_runtime_advance(runtime, 0, 1024, &result));
    char expected[256];
    snprintf(expected, sizeof(expected),
             "IDB:%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu:undefined",
             result.indexed_db_opens, result.indexed_db_deletes,
             result.indexed_db_transactions, result.indexed_db_requests,
             result.indexed_db_records, result.indexed_db_bytes,
             result.indexed_db_peak_bytes, result.indexed_db_quota_errors);
    if (strcmp(result.summary, expected) != 0)
        fprintf(stderr, "host state idb: %s != %s\n", result.summary,
                expected);
    CHECK(strcmp(result.summary, expected) == 0
          && JS_IsObject(runtime->host_indexed_db_stats)
          && result.indexed_db_opens == 1 && result.indexed_db_records == 1
          && result.indexed_db_bytes > 0);

    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The real scheduler behind the JavaScript queue, replayed from a fixture:
   more requests than the shared scheduler admits at once (two 1 MiB
   response reservations here) all start in FIFO order and all complete
   (completion order is the transport's, as in a browser); a
   request aborted while queued never starts; a cross-origin no-cors request
   is sent and resolves to an opaque response; one with a non-safelisted
   header is still sent, without it (the request-no-cors guard drops it);
   one with a non-safelisted method, or whose opaque body exceeds 64 KiB,
   is a TypeError, as is a non-safelisted header reaching the native
   backstop directly; and
   detaching the document with requests
   both on the wire and queued rejects all of them, starts none of the
   queued ones, and leaves no scheduler reservation or Budget bytes. */
static int test_network_queue_replay(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    char trace_error[256] = {0};
    CHECK(fetch_trace_replay_begin(
        TILEFINCH_TEST_SOURCE_DIR "/fixtures/http-fetch-queue",
        trace_error, sizeof(trace_error)));
    static const char html[] = "<!doctype html><body>Queue</body>";
    PocDocument document;
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17)
          && viewport_context_init(&viewport, 480, 272, 480, 272)
          && script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &policy));
    ScriptRuntimeOptions options = { .viewport = viewport,
        .execution_policy = policy, .defer_document_scripts = true,
        .allow_test_network_primitive_overrides = true };
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 6u * MIB, 4000, "https://queue.test/",
        &options, &result);
    CHECK(runtime != NULL);
    static const char probe[] =
        "(async()=>{const real=globalThis.__tilefinchFetchAsync;"
        "globalThis.starts=[];globalThis.__tilefinchFetchAsync="
        "function(...args){const id=real.apply(this,args);"
        "if(Number(id)!==0)starts.push(String(args[1]));return id;};"
        "const order=[],requests=[];for(let i=0;i<8;i++)requests.push("
        "fetch('/q/'+i).then(r=>r.text()).then(text=>order.push(text),"
        "error=>order.push('error:'+error.message)));"
        "const queued=__tilefinchNetworkQueueStats.waiting,"
        "aborter=new AbortController,aborted=fetch('/q/never',{signal:"
        "aborter.signal}).then(()=>'resolved',error=>error.name);"
        "aborter.abort();const pixel=fetch('https://pixel.example/p.gif',"
        "{mode:'no-cors',credentials:'include'}).then(r=>[r.type,r.status,"
        "r.url===''&&[...r.headers].length===0].join(),error=>'error:'+error),"
        "header=fetch('https://pixel.example/p.gif',{mode:'no-cors',"
        "headers:{'x-probe':'1'}}).then(()=>'resolved',error=>error.name),"
        "put=fetch('https://pixel.example/p.gif',{mode:'no-cors',"
        "method:'PUT'}).then(()=>'resolved',error=>error.name),"
        "big=fetch('https://pixel.example/big.bin',{mode:'no-cors'})"
        ".then(r=>r.type,error=>error.name),"
        "backstop=(()=>{try{real('GET','https://pixel.example/p.gif',undefined,"
        "'','x-probe: 1','no-cors','same-origin');return 'started'}"
        "catch(error){return error.name}})();"
        "await Promise.all(requests);"
        "globalThis.pocSummary=['QUEUE',starts.filter(url=>url.includes('/q/'))"
        ".map(url=>url.slice(-3)).join(),order.slice().sort().join(),queued,"
        "await aborted,"
        "starts.some(url=>url.endsWith('/never')),await pixel,"
        "(await header,starts.filter(url=>url.endsWith('/p.gif')).length),"
        "await put,await big,backstop].join('|');})().catch(error=>{globalThis.pocSummary="
        "'QUEUE-ERROR:'+String(error)+String(error&&error.stack||'')});";
    bool ok = script_runtime_evaluate_diagnostic(
        runtime, probe, "<network-queue-replay>", &result);
    for (size_t tick = 0; ok && tick < 256
         && strncmp(result.summary, "QUEUE", 5) != 0; tick++) {
        ok = script_runtime_advance(runtime, 4, 64, &result);
    }
    static const char expected[] =
        "QUEUE|q/0,q/1,q/2,q/3,q/4,q/5,q/6,q/7|q0,q1,q2,q3,q4,q5,q6,q7|6|"
        "AbortError|false|opaque,0,true|"
        "2|TypeError|TypeError|TypeError";
    if (!ok || strcmp(result.summary, expected) != 0) {
        fprintf(stderr, "network queue replay: ok=%d summary=%s error=%s "
                "trace=%s\n", ok, result.summary, result.error, trace_error);
    }
    CHECK(ok && strcmp(result.summary, expected) == 0);

    static const char teardown_probe[] =
        "globalThis.teardown=[];starts.length=0;for(let i=0;i<8;i++)"
        "teardown.push(fetch('/t/'+i).then(()=>'resolved',"
        "error=>error.name));globalThis.pocSummary='TEARDOWN:'+starts.length"
        "+':'+__tilefinchNetworkQueueStats.waiting;";
    /* The shared scheduler's response reservation admits two at once; the
       other six wait in the JavaScript FIFO. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, teardown_probe, "<network-queue-teardown>", &result)
          && strcmp(result.summary, "TEARDOWN:2:6") == 0);
    CHECK(script_runtime_advance(runtime, 4, 64, &result));
    size_t reserved = 0, domain_active = 0;
    fetch_scheduler_reservation_state(
        runtime->bridge.fetch_scheduler, &reserved, NULL, &domain_active,
        NULL);
    CHECK(fetch_scheduler_pending(runtime->bridge.fetch_scheduler) == 2
          && reserved != 0 && domain_active == 2
          && runtime->bridge.async_fetch_count == 2
          && result.async_network_pending_logical == 6);
    script_runtime_detach_document(runtime, &document);
    static const char settled_probe[] =
        "Promise.all(teardown).then(values=>{globalThis.pocSummary="
        "'SETTLED:'+values.join()+':'+starts.length;});";
    ok = script_runtime_evaluate_diagnostic(
        runtime, settled_probe, "<network-queue-settled>", &result);
    for (size_t tick = 0; ok && tick < 16; tick++) {
        ok = script_runtime_advance(runtime, 50, 64, &result);
    }
    static const char settled[] =
        "SETTLED:TypeError,TypeError,TypeError,TypeError,TypeError,"
        "TypeError,TypeError,TypeError:2";
    if (!ok || strcmp(result.summary, settled) != 0) {
        fprintf(stderr, "network queue teardown: ok=%d summary=%s error=%s\n",
                ok, result.summary, result.error);
    }
    fetch_scheduler_reservation_state(
        runtime->bridge.fetch_scheduler, &reserved, NULL, &domain_active,
        NULL);
    if (fetch_scheduler_pending(runtime->bridge.fetch_scheduler) != 0
        || reserved != 0 || domain_active != 0
        || runtime->bridge.async_fetch_count != 0
        || result.async_network_active_native != 0
        || result.async_network_pending_logical != 0) {
        fprintf(stderr, "network queue teardown residue: pending=%zu "
                "reserved=%zu domain=%zu native=%zu active=%zu logical=%zu\n",
                fetch_scheduler_pending(runtime->bridge.fetch_scheduler),
                reserved, domain_active, runtime->bridge.async_fetch_count,
                result.async_network_active_native,
                result.async_network_pending_logical);
    }
    CHECK(ok && strcmp(result.summary, settled) == 0
          && fetch_scheduler_pending(runtime->bridge.fetch_scheduler) == 0
          && reserved == 0 && domain_active == 0
          && runtime->bridge.async_fetch_count == 0
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    fetch_trace_end();
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* URLSearchParams and URL setters convert through Web IDL USVString: a lone
   surrogate becomes U+FFFD and a valid pair is kept. The conversion is
   checked against the specification's per-code-unit algorithm on fixed
   cases and on every string of up to four units over a surrogate-heavy
   alphabet, and through the public APIs that use it. */
static int test_usv_string_conversion(void)
{
    Budget budget;
    budget_init(&budget, 16u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    ScriptResult result = {0};
    ScriptRuntime *runtime = host_state_runtime(&budget, &document, &result);
    CHECK(runtime != NULL);
    static const char probe[] =
        "{const reference=text=>{let out='';for(let i=0;i<text.length;i++){"
        "const u=text.charCodeAt(i);if(u>=0xd800&&u<=0xdbff){const n=i+1<"
        "text.length?text.charCodeAt(i+1):0;if(n>=0xdc00&&n<=0xdfff){"
        "out+=text[i]+text[i+1];i++;continue;}out+='\\ufffd';}else if(u>="
        "0xdc00&&u<=0xdfff)out+='\\ufffd';else out+=text[i];}return out;},"
        "convert=value=>{const p=new URLSearchParams;p.append(value,'');"
        "return p.keys().next().value;},failures=[];"
        "const fixed=[['a\\ud800b','a\\ufffdb'],['a\\udc00b','a\\ufffdb'],"
        "['\\ud83d\\ude00','\\ud83d\\ude00'],['\\udc00\\ud800',"
        "'\\ufffd\\ufffd'],['\\ud800\\ud83d\\ude00\\udc00x',"
        "'\\ufffd\\ud83d\\ude00\\ufffdx'],['\\ud800','\\ufffd'],"
        "['\\udbff','\\ufffd'],['',''],['plain','plain'],"
        "['caf\\u00e9\\uffff','caf\\u00e9\\uffff']];"
        "for(const [input,want] of fixed)if(convert(input)!==want)"
        "failures.push('fixed:'+escape(input));"
        "const units=['a','\\u00e9','\\ud800','\\udbff','\\udc00','\\udfff',"
        "'\\ud83d','\\ude00','\\uffff'];let checked=0;"
        "const walk=(prefix,depth)=>{if(convert(prefix)!==reference(prefix))"
        "failures.push('walk:'+escape(prefix));checked++;if(depth===4)return;"
        "for(const unit of units)walk(prefix+unit,depth+1);};walk('',0);"
        "const params=new URLSearchParams([['k\\ud800','v\\udc00']]);"
        "params.set('s\\ud83d\\ude00','\\ud83d');"
        "const surface=[params.get('k\\ud800'),params.has('k\\ufffd'),"
        "params.toString(),convert(12),convert(null),convert(undefined),"
        "convert({toString(){return 'o\\udfff'}})];"
        "const url=new URL('https://host-state.test/');"
        "url.pathname='/p\\ud800';url.search='?q=\\udc00';"
        "url.hash='h\\ud83d\\ude00\\ud800';surface.push(url.href);"
        "const want=['v\\ufffd',true,'k%EF%BF%BD=v%EF%BF%BD&s%F0%9F%98%80="
        "%EF%BF%BD','12','null','undefined','o\\ufffd','https://host-state."
        "test/p%EF%BF%BD?q=%EF%BF%BD#h%F0%9F%98%80%EF%BF%BD'];"
        "surface.forEach((value,index)=>{if(value!==want[index])"
        "failures.push('surface'+index+':'+escape(String(value)));});"
        "pocSummary=failures.length?failures.slice(0,4).join('|'):"
        "'USV-OK:'+checked;}";
    CHECK(host_state_eval(runtime, probe, "USV-OK:7381", &result));
    /* ASCII query components already are their UTF-8 decoding. Pin the
       work bound: the decoder must not materialize one array item per byte.
       This counter is scoped to construction; the ordinary entry-list push
       remains visible and is allowed. */
    static const char ascii_probe[] =
        "{const text='value'.repeat(2048),push=Array.prototype.push;"
        "let calls=0,params;Array.prototype.push=function(...values){"
        "calls++;return Reflect.apply(push,this,values)};try{params="
        "new URLSearchParams('plain='+text+'&second=a+b')}finally{"
        "Array.prototype.push=push}pocSummary=params.get('plain')===text&&"
        "params.get('second')==='a b'&&calls<=4?'URL-ASCII-OK':"
        "'URL-ASCII-FAILED:'+calls;}";
    CHECK(host_state_eval(runtime, ascii_probe, "URL-ASCII-OK", &result));
    static const char url_decode_probe[] =
        "{const cases=[['q=plain+words','plain words'],['q=%41%2b+A','A+ A'],"
        "['q=%E2%82A','\\ufffdA'],['q=%GG%','%GG%'],['q=caf\\u00e9',"
        "'caf\\u00e9'],['q=\\ud83d\\ude00','\\ud83d\\ude00'],"
        "['q=\\ud800','\\ufffd'],['q=\\u0000\\u007f','\\u0000\\u007f']],"
        "bad=cases.filter(pair=>new URLSearchParams(pair[0]).get('q')!==pair[1]);"
        "const cap=256*1024,plain='a'.repeat(cap);let refused='';"
        "const boundary=new URLSearchParams('q='+plain).get('q')===plain;"
        "try{new URLSearchParams('q='+plain+'a')}catch(error){refused=error.name}"
        "pocSummary=!bad.length&&boundary&&refused==='RangeError'?"
        "'URL-DECODE-BOUNDS-OK':'URL-DECODE-BOUNDS-FAILED:'+"
        "[bad.length,boundary,refused].join(',');}";
    CHECK(host_state_eval(runtime, url_decode_probe,
                          "URL-DECODE-BOUNDS-OK", &result));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* The node-handle table has SCRIPT_DOM_HANDLE_SLOT_CAPACITY slots, and a
   slot is released only when its wrapper is finalized, which normally waits
   for a job checkpoint. A single synchronous script that creates more
   unreferenced wrappers than the table holds must reclaim the dead ones at
   exhaustion; one that genuinely retains them all must get a clean
   RangeError from the DOM API, and the realm must recover once they drop. */
static bool handle_table_eval(ScriptRuntime *runtime, const char *source,
                              const char *expected, ScriptResult *result)
{
    if (script_runtime_evaluate_diagnostic(runtime, source,
                                           "<handle-table>", result)
        && strcmp(result->summary, expected) == 0) return true;
    fprintf(stderr, "handle table: expected %s got %s error=%s live=%zu "
            "peak=%zu high-water=%zu reuses=%zu exhaustions=%zu "
            "releases=%zu\n", expected, result->summary, result->error,
            result->dom_handle_slots_live, result->dom_handle_slots_peak,
            result->dom_handle_slots_high_water,
            result->dom_handle_slot_reuses, result->dom_handle_exhaustions,
            result->dom_handle_wrapper_releases);
    return false;
}

static int test_dom_handle_table_exhaustion(void)
{
    Budget budget;
    budget_init(&budget, 48u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] =
        "<!doctype html><body><div id=list></div></body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_with_session(
        &document, &budget, 24u * MIB, 60000,
        "https://handle-table.test/", NULL, &result);
    CHECK(runtime != NULL);

    /* (a) Well over a table's worth of dead wrappers inside one script:
       plain garbage, self-cycles that only a collection clears, and whole
       detached trees walked and then dropped. */
    size_t exhaustions_before = result.dom_handle_exhaustions;
    size_t reuses_before = result.dom_handle_slot_reuses;
    CHECK(handle_table_eval(
        runtime,
        "(()=>{let made=0,walked=0,error='';try{"
        "for(let i=0;i<12000;i++){const node=document.createElement('i');"
        "if(i&1)node.self=node;made++;}"
        "for(let round=0;round<16;round++){"
        "const tree=document.createElement('div');"
        "tree.innerHTML='<p><b>x</b>y</p>'.repeat(200);"
        "for(const at of tree.querySelectorAll('*'))"
        "walked+=at.childNodes.length;}"
        "}catch(caught){error=String(caught);}"
        "globalThis.pocSummary=made===12000&&walked===16*200*3&&!error?"
        "'HANDLE-RECLAIM-OK':'HANDLE-RECLAIM-FAILED:'+made+':'+walked+':'"
        "+error;})()",
        "HANDLE-RECLAIM-OK", &result));
    CHECK(result.dom_handle_exhaustions == exhaustions_before
          && result.dom_handle_slot_reuses
               >= reuses_before + SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_high_water
               <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    /* (b) Every slot strongly held: creation and walks fail at the API with
       the exhaustion error, counted, instead of a null deep in bootstrap. */
    CHECK(handle_table_eval(
        runtime,
        "(()=>{const list=document.getElementById('list');"
        "list.innerHTML='<p>a</p>'.repeat(40);"
        "globalThis.pocSummary='HANDLE-LIST-READY';})()",
        "HANDLE-LIST-READY", &result));
    CHECK(collect_and_drain_finalizers(runtime, &result));
    exhaustions_before = result.dom_handle_exhaustions;
    CHECK(handle_table_eval(
        runtime,
        "(()=>{const keep=globalThis.__handleKeep=[];"
        "const list=document.getElementById('list');"
        "const clean=(error)=>error instanceof RangeError"
        "&&/DOM node handle table exhausted/.test(error.message);"
        "const attempt=(run)=>{try{run();return 'none';}catch(error){"
        "return clean(error)?'clean':String(error);}};"
        "const create=attempt(()=>{for(let i=0;i<9000;i++)"
        "keep.push(document.createElement('b'));});"
        "const walks=[attempt(()=>list.children.length),"
        "attempt(()=>list.querySelectorAll('p')),"
        "attempt(()=>list.firstChild),"
        "attempt(()=>document.createTextNode('t'))];"
        "globalThis.pocSummary=create==='clean'&&keep.length>8000"
        "&&walks.every((w)=>w==='clean')?'HANDLE-EXHAUSTED-CLEAN':"
        "'HANDLE-EXHAUSTED-FAILED:'+keep.length+':'+create+':'"
        "+walks.join(',');})()",
        "HANDLE-EXHAUSTED-CLEAN", &result));
    CHECK(result.dom_handle_exhaustions >= exhaustions_before + 5);

    /* Releasing them makes the same realm usable again, even before any
       cleanup job has run. */
    CHECK(handle_table_eval(
        runtime,
        "(()=>{globalThis.__handleKeep=null;let made=0,error='';try{"
        "for(let i=0;i<9000;i++){document.createElement('u');made++;}"
        "const list=document.getElementById('list');"
        "made+=list.children.length+list.querySelectorAll('p').length;"
        "}catch(caught){error=String(caught);}"
        "globalThis.pocSummary=made===9080&&!error?'HANDLE-RECOVERED':"
        "'HANDLE-RECOVER-FAILED:'+made+':'+error;})()",
        "HANDLE-RECOVERED", &result));
    CHECK(collect_and_drain_finalizers(runtime, &result));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

/* A news front page holds 9-10 thousand live elements and walks them all
   (apnews.com: 9,322). The realistic PSP profile grows the handle table on
   demand to SCRIPT_DOM_HANDLE_SLOT_CAPACITY_REALISTIC; strict keeps the
   base table. Growth is Budget-admitted, a refusal is the ordinary clean
   exhaustion, and teardown returns every byte. */
static ScriptRuntime *handle_scaling_runtime(
    PocDocument *document, Budget *budget, ScriptExecutionProfile profile,
    ScriptResult *result)
{
    ViewportContext viewport;
    ScriptExecutionPolicy policy;
    if (!viewport_context_init(&viewport, 480, 272, 480, 272)
        || !script_execution_policy_for_profile(profile, &policy))
        return NULL;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = policy,
    };
    return script_runtime_create_configured(
        document, budget, 24u * MIB, 60000, "https://handle-scale.test/",
        &options, result);
}

static int test_dom_handle_table_scales_with_profile(void)
{
    static const char walk[] =
        "(()=>{const keep=globalThis.__walkKeep=[];let error='';try{"
        "const visit=(node)=>{for(let at=node.firstElementChild;at;"
        "at=at.nextElementSibling){keep.push(at);visit(at);}};"
        "keep.push(document.documentElement);visit(document.documentElement);"
        "}catch(caught){error=(caught instanceof RangeError"
        "&&/DOM node handle table exhausted/.test(caught.message))"
        "?'clean':String(caught);}"
        "globalThis.pocSummary=error?'WALK-'+error+':'+keep.length"
        ":'WALK-OK:'+keep.length;})()";
    /* 12,000 connected elements plus html, head and body. */
    size_t html_capacity = 64u + 12000u * 11u;
    char *html = malloc(html_capacity);
    CHECK(html != NULL);
    size_t length = (size_t) snprintf(html, html_capacity,
                                      "<!doctype html><body>");
    for (size_t i = 0; i < 12000u; i++) {
        memcpy(html + length, "<i></i>", 7u);
        length += 7u;
    }
    html[length] = '\0';
    static const struct {
        ScriptExecutionProfile profile;
        const char *expected;
        size_t capacity;
    } cases[] = {
        { SCRIPT_EXECUTION_PROFILE_PSP_STRICT, "WALK-clean:",
          SCRIPT_DOM_HANDLE_SLOT_CAPACITY },
        { SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, "WALK-OK:12003",
          SCRIPT_DOM_HANDLE_SLOT_CAPACITY_REALISTIC },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        Budget budget;
        budget_init(&budget, 64u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document;
        CHECK(document_parse(&document, &budget, html, length, 17));
        ScriptResult result = {0};
        ScriptRuntime *runtime = handle_scaling_runtime(
            &document, &budget, cases[c].profile, &result);
        CHECK(runtime != NULL);
        CHECK(result.dom_handle_slot_capacity
              == SCRIPT_DOM_HANDLE_SLOT_CAPACITY);
        CHECK(script_runtime_evaluate_diagnostic(
            runtime, walk, "<handle-walk>", &result));
        if (strncmp(result.summary, cases[c].expected,
                    strlen(cases[c].expected)) != 0) {
            fprintf(stderr, "handle scaling profile=%d: %s capacity=%zu "
                    "growths=%zu refusals=%zu exhaustions=%zu\n",
                    (int) cases[c].profile, result.summary,
                    result.dom_handle_slot_capacity,
                    result.dom_handle_growths,
                    result.dom_handle_growth_refusals,
                    result.dom_handle_exhaustions);
            CHECK(false);
        }
        CHECK(result.dom_handle_slot_capacity == cases[c].capacity);
        if (cases[c].capacity == SCRIPT_DOM_HANDLE_SLOT_CAPACITY) {
            CHECK(result.dom_handle_growths == 0
                  && result.dom_handle_exhaustions != 0);
        } else {
            CHECK(result.dom_handle_growths == 1
                  && result.dom_handle_exhaustions == 0
                  && result.dom_handle_slots_high_water >= 12003u);
            /* Exhaustion at the grown size is the same clean RangeError,
               and the realm recovers once the nodes are released. */
            CHECK(script_runtime_evaluate_diagnostic(
                runtime,
                "(()=>{const keep=globalThis.__extraKeep=[];let error='';"
                "try{for(let i=0;i<8000;i++)"
                "keep.push(document.createElement('b'));}catch(caught){"
                "error=caught instanceof RangeError&&/DOM node handle table "
                "exhausted/.test(caught.message)?'clean':String(caught);}"
                "globalThis.pocSummary='EXTRA-'+error+':'+keep.length;})()",
                "<handle-exhaust>", &result));
            CHECK(strncmp(result.summary, "EXTRA-clean:", 12) == 0
                  && result.dom_handle_exhaustions != 0
                  && result.dom_handle_slot_capacity
                         == SCRIPT_DOM_HANDLE_SLOT_CAPACITY_REALISTIC);
            CHECK(script_runtime_evaluate_diagnostic(
                runtime,
                "(()=>{globalThis.__extraKeep=null;let made=0;"
                "for(let i=0;i<2000;i++){document.createElement('u');made++;}"
                "globalThis.pocSummary='RECOVERED:'+made;})()",
                "<handle-recover>", &result)
                  && strcmp(result.summary, "RECOVERED:2000") == 0);
        }
        CHECK(collect_and_drain_finalizers(runtime, &result));
        script_runtime_destroy(runtime);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }

    /* A growth the Budget refuses leaves the full base table intact and
       reports a clean exhaustion; nothing leaks. */
    {
        Budget budget;
        budget_init(&budget, 64u * MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document;
        static const char small[] = "<!doctype html><body></body>";
        CHECK(document_parse(&document, &budget, small,
                             sizeof(small) - 1u, 17));
        ScriptResult result = {0};
        ScriptRuntime *runtime = handle_scaling_runtime(
            &document, &budget, SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC,
            &result);
        CHECK(runtime != NULL);
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const keep=globalThis.__fill=[];"
            "for(let i=0;i<8150;i++)keep.push(document.createElement('b'));"
            "globalThis.pocSummary='FILLED:'+keep.length;})()",
            "<handle-fill>", &result)
              && strcmp(result.summary, "FILLED:8150") == 0);
        size_t limit = budget.limit;
        budget.limit = budget.current + 160u * 1024u;
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const keep=globalThis.__fill;let error='';try{"
            "for(let i=0;i<200;i++)keep.push(document.createElement('s'));"
            "}catch(caught){error=caught instanceof RangeError?'clean'"
            ":String(caught);}"
            "globalThis.pocSummary='REFUSED-'+error;})()",
            "<handle-refused>", &result));
        budget.limit = limit;
        if (strcmp(result.summary, "REFUSED-clean") != 0
            || result.dom_handle_growth_refusals == 0
            || result.dom_handle_slot_capacity
                   != SCRIPT_DOM_HANDLE_SLOT_CAPACITY) {
            fprintf(stderr, "handle growth refusal: %s capacity=%zu "
                    "refusals=%zu\n", result.summary,
                    result.dom_handle_slot_capacity,
                    result.dom_handle_growth_refusals);
            CHECK(false);
        }
        /* With room again, the next registration grows the table. */
        CHECK(script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const keep=globalThis.__fill;"
            "for(let i=0;i<200;i++)keep.push(document.createElement('q'));"
            "globalThis.pocSummary='GREW:'+keep.length;})()",
            "<handle-grew>", &result)
              && result.dom_handle_slot_capacity
                     == SCRIPT_DOM_HANDLE_SLOT_CAPACITY_REALISTIC);
        CHECK(collect_and_drain_finalizers(runtime, &result));
        script_runtime_destroy(runtime);
        document_destroy(&document);
        CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    }
    free(html);
    return 0;
}

static int test_finalizers_do_not_starve_promises(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    static const char html[] = "<!doctype html><body>Ready</body>";
    CHECK(document_parse(&document, &budget, html, sizeof(html)-1u, 17));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 8u*MIB, 4000, "https://jobs.test/", &result);
    CHECK(runtime != NULL);
    static const char source[] =
        "globalThis.steps=0;globalThis.cleaned=0;globalThis.cleanupPromises=0;"
        "globalThis.registry=new FinalizationRegistry(()=>{"
        "if(cleaned!==cleanupPromises)throw Error('missing cleanup checkpoint');"
        "cleaned++;Promise.resolve().then(()=>cleanupPromises++)});"
        "for(let i=0;i<330;i++){let x={};x.self=x;registry.register(x,i)}"
        "Promise.resolve().then(function step(){steps++;"
        "if(steps<8)Promise.resolve().then(step)});";
    JSValue value = JS_Eval(runtime->context, source, sizeof(source)-1u,
                             "<finalizer-pressure>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(runtime->context, value);
    JS_RunGC(runtime->runtime);
    JS_RunGC(runtime->runtime);
    uint64_t started = tilefinch_platform_monotonic_time_ns();
    CHECK(js_rt_runtime_run_jobs(runtime));
    JSValue global = JS_GetGlobalObject(runtime->context);
    JSValue steps = JS_GetPropertyStr(runtime->context, global, "steps");
    JSValue cleaned = JS_GetPropertyStr(runtime->context, global, "cleaned");
    int32_t step_count=0, cleanup_count=0;
    CHECK(JS_ToInt32(runtime->context, &step_count, steps)==0);
    CHECK(JS_ToInt32(runtime->context, &cleanup_count, cleaned)==0);
    printf("finalizer checkpoint: promises=%d cleanups=%d us=%llu\n",
           step_count, cleanup_count, (unsigned long long)
           ((tilefinch_platform_monotonic_time_ns()-started)/1000u));
    JS_FreeValue(runtime->context, steps);
    JS_FreeValue(runtime->context, cleaned);
    JS_FreeValue(runtime->context, global);
    CHECK(step_count == 8 && cleanup_count == 0);
    for (unsigned i=0;i<64;i++)
        CHECK(script_runtime_advance(runtime, 0, 16, &result));
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "if(cleaned!==330||cleanupPromises!==330)throw Error('cleanup lost');"
        "pocSummary='CLEANUP-OK'",
        "<cleanup-complete>", &result));
    /* Destruction must also release a queue which never got an idle turn. */
    static const char pending[] =
        "for(let i=0;i<100;i++){let x={};x.self=x;registry.register(x,i)}";
    value = JS_Eval(runtime->context, pending, sizeof(pending)-1u,
                    "<undrained-cleanups>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(runtime->context, value);
    JS_RunGC(runtime->runtime);
    JS_RunGC(runtime->runtime);
    CHECK(JS_IsCleanupJobPending(runtime->runtime));
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current==0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_mutation_observer_shadow_boundaries(void)
{
    Budget budget;
    budget_init(&budget, 32u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    const char html[] = "<!doctype html><html><body></body></html>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 8u * MIB, 4000, "https://observer.test/", &result);
    CHECK(runtime != NULL);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const m=new MutationObserver(()=>{});"
        "m.observe(document.body,{attributes:true});"
        "document.body.setAttribute('data-a','1');"
        "document.body.setAttribute('data-b','2');"
        "const records=m.takeRecords();"
        "if(records.length!==2||records.some(r=>r.addedNodes.length||"
        "r.removedNodes.length)||records[0].addedNodes===records[1].addedNodes||"
        "records[0].removedNodes===records[1].removedNodes)"
        "throw Error('mutation lists shared');m.disconnect();"
        "for(const mode of ['open','closed']){"
        "const host=document.createElement('div');document.body.append(host);"
        "const root=host.attachShadow({mode}),leaf=document.createElement('i');"
        "root.append(leaf);const outer=new MutationObserver(()=>{}),"
        "inner=new MutationObserver(()=>{}),doc=new MutationObserver(()=>{});"
        "outer.observe(host,{attributes:true,childList:true,subtree:true});"
        "doc.observe(document,{attributes:true,childList:true,subtree:true});"
        "inner.observe(root,{attributes:true,subtree:true});"
        "leaf.setAttribute('data-x','1');"
        "if(outer.takeRecords().length||doc.takeRecords().length||"
        "inner.takeRecords().length!==1)throw Error('shadow boundary '+mode);"
        "root.removeChild(leaf);leaf.setAttribute('data-x','2');"
        "if(inner.takeRecords().length!==1||outer.takeRecords().length)"
        "throw Error('shadow transient '+mode);"
        "root.append(leaf);host.remove();doc.takeRecords();"
        "leaf.setAttribute('data-x','3');"
        "if(inner.takeRecords().length!==1||outer.takeRecords().length||"
        "doc.takeRecords().length)throw Error('host transient '+mode);"
        "outer.disconnect();inner.disconnect();doc.disconnect();host.remove();"
        "}pocSummary='SHADOW-OBSERVERS-OK'})()",
        "<shadow-observers>", &result));
    CHECK(strcmp(result.summary, "SHADOW-OBSERVERS-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_css_property_name_canonical_fast_path(void)
{
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    const char html[] = "<!doctype html><html><body></body></html>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 8u * MIB, 4000, "https://css-name.test/", &result);
    CHECK(runtime != NULL);
    bool normalized = script_runtime_evaluate_diagnostic(runtime,
        "(()=>{const normalize=__tilefinchCssName,"
        "original=String.prototype.charCodeAt,exec=RegExp.prototype.exec;"
        "let scans=0,conversions=0;"
        "String.prototype.charCodeAt=function(i){scans++;return original.call(this,i)};"
        "try{for(const value of ['', 'display', 'background-color',"
        "'scroll-margin-block-start','--MixedCase','width-\\u00c4',"
        "'width-\\ud83d\\ude00','a'.repeat(2048)]){"
        "if(normalize(value)!==value)throw Error('canonical value')}"
        "if(normalize('cssFloat')!=='float')throw Error('cssFloat');"
        "if(normalize({toString(){conversions++;return 'border-color'}})"
        "!=='border-color'||conversions!==1)throw Error('conversion');"
        "if(scans!==0)throw Error('canonical name rescanned:'+scans);"
        "for(const [value,wanted] of [['marginTop','margin-top'],"
        "['WebkitTransform','-webkit-transform'],['XML','-x-m-l'],"
        "['xY-Z','x-y--z']])if(normalize(value)!==wanted)"
        "throw Error('camel case:'+value);"
        "if(scans===0)throw Error('fallback untested');"
        "scans=0;const prefix='a'.repeat(2048);"
        "if(normalize(prefix+'B')!==prefix+'-b'||scans!==1)"
        "throw Error('camel prefix rescanned:'+scans);"
        "RegExp.prototype.exec=function(){throw Error('author exec')};"
        "if(normalize('border-left')!=='border-left'||"
        "normalize('borderLeft')!=='border-left')throw Error('intrinsic');"
        "}finally{String.prototype.charCodeAt=original;RegExp.prototype.exec=exec}"
        "pocSummary='CSS-NAME-FAST-PATH-OK'})()",
        "<css-name-fast-path>", &result);
    if (!normalized) fprintf(stderr, "CSS name normalization: %s\n", result.error);
    CHECK(normalized);
    CHECK(strcmp(result.summary, "CSS-NAME-FAST-PATH-OK") == 0);
    script_runtime_destroy(runtime);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

static int test_diagnostic_probe_leaves_jobs_pending(void)
{
    Budget budget;
    budget_init(&budget, 24u * MIB);
    CHECK(budget_install_lexbor(&budget));
    PocDocument document;
    const char html[] = "<!doctype html><html><body></body></html>";
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1u, 17));
    size_t document_bytes = budget.current;
    ScriptResult result;
    ScriptRuntime *runtime = script_runtime_create(
        &document, &budget, 8u * MIB, 4000, "https://probe.test/", &result);
    CHECK(runtime != NULL);
    Watchdog original_watchdog = runtime->watchdog;
    CHECK(script_runtime_evaluate_probe(runtime,
        "globalThis.probeRan=0;Promise.resolve().then(()=>{probeRan++});"
        "globalThis.directProbe=function(){return this===globalThis&&probeRan===1};"
        "globalThis.throwProbe=function(){throw new Error('direct-probe-error')};"
        "globalThis.loopProbe=function(){while(true){}};"
        "globalThis.notCallable=7;"
        "__tilefinchClipboardWrite(String(probeRan))",
        "<probe-queue>", &result));
    CHECK(strcmp(result.last_clipboard_text, "0") == 0);
    CHECK(runtime->watchdog.deadline_ms == original_watchdog.deadline_ms);
    CHECK(runtime->watchdog.polls == original_watchdog.polls);
    CHECK(runtime->watchdog.interrupted == original_watchdog.interrupted);
    CHECK(script_runtime_evaluate_probe(runtime,
        "__tilefinchClipboardWrite(String(probeRan))",
        "<probe-pending>", &result));
    CHECK(strcmp(result.last_clipboard_text, "0") == 0);
    size_t compile_attempts = runtime->result.host_compile_attempts;
    bool matched = true;
    for (unsigned i = 0; i < 32; i++) {
        CHECK(script_runtime_call_boolean_probe(runtime, "directProbe", &matched));
        CHECK(!matched);
        CHECK(runtime->result.host_compile_attempts == compile_attempts);
    }
    CHECK(runtime->watchdog.deadline_ms == original_watchdog.deadline_ms);
    CHECK(runtime->watchdog.polls == original_watchdog.polls);
    CHECK(script_runtime_advance(runtime, 0, 16, &result));
    CHECK(script_runtime_call_boolean_probe(runtime, "directProbe", &matched));
    CHECK(matched && runtime->result.host_compile_attempts == compile_attempts);
    CHECK(!script_runtime_call_boolean_probe(runtime, "missingProbe", &matched));
    CHECK(!matched);
    CHECK(!script_runtime_call_boolean_probe(runtime, "notCallable", &matched));
    CHECK(!matched);
    CHECK(!script_runtime_call_boolean_probe(runtime, "throwProbe", &matched));
    CHECK(!matched && strstr(runtime->result.error, "direct-probe-error") != NULL);
    char long_name[98];
    memset(long_name, 'x', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';
    CHECK(!script_runtime_call_boolean_probe(runtime, long_name, &matched));
    CHECK(!matched);
    CHECK(script_runtime_evaluate_probe(runtime,
        "__tilefinchClipboardWrite(String(probeRan))",
        "<probe-complete>", &result));
    CHECK(strcmp(result.last_clipboard_text, "1") == 0);
    CHECK(!script_runtime_evaluate_probe(runtime,
        "throw new Error('probe-exception')", "<probe-error>", &result));
    CHECK(strstr(result.error, "probe-exception") != NULL);
    runtime->result.interrupted = true;
    runtime->result.watchdog_polls = 23;
    runtime->result.watchdog_elapsed_ms = 17;
    CHECK(script_runtime_call_boolean_probe(runtime, "directProbe", &matched));
    CHECK(matched && runtime->result.interrupted
          && runtime->result.watchdog_polls == 23
          && runtime->result.watchdog_elapsed_ms == 17);
    runtime->result.success = false;
    CHECK(script_runtime_evaluate_probe(runtime,
        "__tilefinchClipboardWrite('recovered')", "<probe-recover>", &result));
    CHECK(strcmp(result.last_clipboard_text, "recovered") == 0);
    CHECK(result.interrupted && result.watchdog_polls == 23
          && result.watchdog_elapsed_ms == 17);
    script_runtime_limit_execution_for_us(runtime, 1000);
    CHECK(!script_runtime_call_boolean_probe(runtime, "loopProbe", &matched));
    CHECK(!matched && runtime->result.interrupted);
    CHECK(!script_runtime_evaluate_probe(runtime,
        "while(true){}", "<probe-timeout>", &result));
    CHECK(result.interrupted);
    script_runtime_clear_execution_limit(runtime);
    script_runtime_destroy(runtime);
    CHECK(budget.current == document_bytes);
    document_destroy(&document);
    CHECK(budget.current == 0 && budget_uninstall_lexbor(&budget));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--task-runnable-only") == 0)
        return test_task_runnable_follows_the_event_loop();
    CHECK(test_task_runnable_follows_the_event_loop() == 0);
    if (argc == 2 && strcmp(argv[1], "--fast-turns-only") == 0)
        return test_fast_turns_follow_wall_time();
    CHECK(test_fast_turns_follow_wall_time() == 0);
    if (argc == 2 && strcmp(argv[1], "--diagnostic-probe-only") == 0)
        return test_diagnostic_probe_leaves_jobs_pending();
    CHECK(test_diagnostic_probe_leaves_jobs_pending() == 0);
    if (argc == 2 && strcmp(argv[1], "--css-name-only") == 0)
        return test_css_property_name_canonical_fast_path();
    CHECK(test_css_property_name_canonical_fast_path() == 0);
    if (argc == 2 && strcmp(argv[1], "--answer-checkpoint-only") == 0)
        return test_device_answer_checkpoint();
    CHECK(test_device_answer_checkpoint() == 0);
    if (argc == 2 && strcmp(argv[1], "--sibling-scopes-only") == 0)
        return test_computed_style_sibling_scopes();
    if (argc == 2 && strcmp(argv[1], "--detached-document-only") == 0)
        return test_computed_style_forgets_detached_document();
    if (argc == 2 && strcmp(argv[1], "--style-no-cascade-only") == 0)
        return test_computed_style_reads_without_cascade();
    if (argc == 2 && strcmp(argv[1], "--script-split-only") == 0)
        return test_script_split_attributes_bridge_work();
    if (argc == 2 && strcmp(argv[1], "--style-keywords-only") == 0)
        return test_computed_style_keywords_without_layout();
    CHECK(test_computed_style_keywords_without_layout() == 0);
    if (argc == 2 && strcmp(argv[1], "--finalizers-only") == 0)
        return test_finalizers_do_not_starve_promises();
    CHECK(test_finalizers_do_not_starve_promises() == 0);
    if (argc == 2 && strcmp(argv[1], "--shadow-observers-only") == 0)
        return test_mutation_observer_shadow_boundaries();
    CHECK(test_mutation_observer_shadow_boundaries() == 0);
    if (argc == 2 && strcmp(argv[1], "--coalesced-tokens-only") == 0)
        return test_coalesced_attribute_tokens();
    if (argc == 2 && strcmp(argv[1], "--handle-table-only") == 0)
        return test_dom_handle_table_exhaustion()
            || test_dom_handle_table_scales_with_profile();
    CHECK(test_coalesced_attribute_tokens() == 0);
    if (argc == 2 && strcmp(argv[1], "--detached-move-only") == 0)
        return test_detached_move_is_a_removal();
    CHECK(test_detached_move_is_a_removal() == 0);
    if (argc == 2 && strcmp(argv[1], "--external-compile-pressure-only") == 0)
        return test_external_compile_reclaims_cycles()
            || test_gc_pacing_requires_heap_growth()
            || test_dom_wrapper_receiver_sharing()
            || test_dom_order_without_sibling_wrappers();
    if (argc == 2 && strcmp(argv[1], "--gc-array-growth-only") == 0)
        return test_gc_thrash_array_growth_amortized();
    if (argc == 2 && strcmp(argv[1], "--gc-thrash-only") == 0)
        return test_gc_thrash();
    if (argc == 2 && strcmp(argv[1], "--task-time-slice-only") == 0)
        return test_runtime_task_time_slice();
    if (argc == 2
        && strcmp(argv[1], "--intersection-observer-budget-only") == 0)
        return test_intersection_observer_registration_budget();
    if (argc == 2 && strcmp(argv[1], "--resize-observer-budget-only") == 0)
        return test_resize_observer_registration_budget();
    if (argc == 2 && strcmp(argv[1], "--job-heap-rejection-only") == 0)
        return test_job_heap_rejection_is_fatal();
    if (argc == 2 && strcmp(argv[1], "--host-state-only") == 0)
        return test_host_state_timers_and_scroll(false)
            || test_host_state_timers_and_scroll(true)
            || test_host_state_network_and_indexeddb_stats()
            || test_usv_string_conversion();
    if (argc == 2 && strcmp(argv[1], "--network-queue-only") == 0)
        return test_network_queue_replay();
    CHECK(test_external_compile_reclaims_cycles() == 0);
    CHECK(test_gc_pacing_requires_heap_growth() == 0
          && test_gc_pacing_pregrows_growable_heap() == 0
          && test_heap_growth_uses_existing_headroom() == 0
          && test_heap_return_checks_back_off() == 0
          && test_heap_decisions_skip_census() == 0
          && test_attribute_write_timing_needs_profiler() == 0);
    CHECK(test_gc_thrash() == 0);
    CHECK(test_dom_wrapper_receiver_sharing() == 0);
    CHECK(test_dom_order_without_sibling_wrappers() == 0);
    CHECK(test_dom_handle_table_exhaustion() == 0);
    CHECK(test_dom_handle_table_scales_with_profile() == 0);
    CHECK(test_stream_and_xhr_private_state_reclamation() == 0);
    CHECK(test_response_body_release_with_retained_wrappers() == 0);
    CHECK(test_runtime_task_time_slice() == 0);
    CHECK(test_intersection_observer_registration_budget() == 0);
    CHECK(test_resize_observer_registration_budget() == 0);
    CHECK(test_computed_style_native_cooperation() == 0);
    CHECK(test_computed_style_memo_invalidation() == 0);
    CHECK(test_computed_style_ancestor_cache() == 0);
    CHECK(test_script_split_attributes_bridge_work() == 0);
    CHECK(test_computed_style_reads_without_cascade() == 0);
    CHECK(test_computed_style_sibling_scopes() == 0);
    CHECK(test_computed_style_forgets_detached_document() == 0);
    CHECK(test_computed_style_container_cache(false) == 0);
    CHECK(test_computed_style_container_cache(true) == 0);
    CHECK(test_computed_style_scoped_invalidation() == 0);
    CHECK(test_inline_style_scoped_invalidation() == 0);
    CHECK(test_computed_style_host_generation() == 0);
    CHECK(test_computed_style_parser_insertion() == 0);
    CHECK(test_computed_style_controller_host_changes() == 0);
    CHECK(test_computed_style_var_cache_on_demand() == 0);
    CHECK(test_watchdog_elapsed_cooperation() == 0);
    CHECK(test_reduced_dom_event_counter() == 0);
    CHECK(test_job_heap_rejection_is_fatal() == 0);
    CHECK(test_attribute_mutation_skips_document_refresh() == 0);
    CHECK(test_get_element_by_id_tracks_id_changes() == 0);
    CHECK(test_document_domain_setter() == 0);
    CHECK(test_custom_element_hooks_after_first_definition() == 0);
    CHECK(test_focus_style_and_selector_helpers() == 0);
    CHECK(test_focus_fixup_follows_moved_subtree() == 0);
    CHECK(test_inserted_subtree_resource_classification() == 0);
    CHECK(test_blank_recovery_author_work_census() == 0);
    CHECK(test_host_state_timers_and_scroll(false) == 0);
    CHECK(test_host_state_timers_and_scroll(true) == 0);
    CHECK(test_host_state_network_and_indexeddb_stats() == 0);
    CHECK(test_network_queue_replay() == 0);
    CHECK(test_usv_string_conversion() == 0);
    CHECK(test_native_dynamic_code_policy() == 0);
    CHECK(test_user_activation_expiry() == 0);
    uint8_t digest[TILEFINCH_SHA256_DIGEST_BYTES];
    CHECK(tilefinch_sha256_digest(NULL, 0, digest)
          && digest_matches_hex(
                 digest,
                 "e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855"));
    static const uint8_t abc[] = "abc";
    CHECK(tilefinch_sha256_digest(abc, sizeof(abc) - 1u, digest)
          && digest_matches_hex(
                 digest,
                 "ba7816bf8f01cfea414140de5dae2223"
                 "b00361a396177a9cb410ff61f20015ad"));
    static const uint8_t two_block_vector[] =
        "abcdbcdecdefdefgefghfghighijhijk"
        "ijkljklmklmnlmnomnopnopq";
    CHECK(tilefinch_sha256_digest(
              two_block_vector, sizeof(two_block_vector) - 1u, digest)
          && digest_matches_hex(
                 digest,
                 "248d6a61d20638b8e5c026930c3e6039"
                 "a33ce45964ff2167f6ecedd419db06c1")
          && !tilefinch_sha256_digest(NULL, 1, digest)
          && !tilefinch_sha256_digest(abc, sizeof(abc) - 1u, NULL));

    CHECK(script_module_mime_type_allowed("text/javascript")
          && script_module_mime_type_allowed(
                 "  Text/JavaScript1.5 ; charset=UTF-8")
          && script_module_mime_type_allowed("application/x-ecmascript")
          && script_module_mime_type_allowed("text/livescript;version=1")
          && !script_module_mime_type_allowed(NULL)
          && !script_module_mime_type_allowed("")
          && !script_module_mime_type_allowed("text/plain")
          && !script_module_mime_type_allowed("application/json")
          && !script_module_mime_type_allowed("text/javascript-module")
          && script_module_revalidated_mime_allowed(
                 "text/javascript", "")
          && script_module_revalidated_mime_allowed(
                 "text/javascript", "application/javascript")
          && !script_module_revalidated_mime_allowed(
                 "text/javascript", "text/plain")
          && !script_module_revalidated_mime_allowed(
                 "text/plain", "text/javascript"));
    bool attribute_module = false;
    static const char spaced_classic[] = " \tText/JavaScript1.5 \r";
    static const char spaced_module[] = "\n MoDuLe \t";
    CHECK(script_type_attribute_classify(
              spaced_classic, sizeof(spaced_classic) - 1u,
              &attribute_module)
          && !attribute_module
          && script_type_attribute_classify(
              spaced_module, sizeof(spaced_module) - 1u,
              &attribute_module)
          && attribute_module
          && !script_type_attribute_classify(
              " application/json ", 18u, &attribute_module));

    ScriptExecutionPolicy lab, strict, realistic, invalid;
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_LAB, &lab)
          && lab.maximum_host_compile_source_bytes == 0
          && lab.slow_compile_threshold_us == 16000
          && lab.slow_callback_threshold_us == 16000
          && lab.maximum_host_compile_projected_us == 0
          && lab.modeled_compile_bytes_per_ms == 0);
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_PSP_STRICT, &strict)
          && strict.maximum_host_compile_source_bytes == 256u * 1024u
          && strict.maximum_advance_time_us == 16000
          && strict.maximum_host_compile_projected_us == 0
          && strict.modeled_compile_bytes_per_ms == 0);
    CHECK(script_execution_policy_for_profile(
              SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &realistic)
          && realistic.maximum_host_compile_source_bytes
               == 4u * 1024u * 1024u
          && realistic.maximum_advance_time_us == 16000
          && realistic.maximum_host_compile_source_bytes
               > strict.maximum_host_compile_source_bytes);
    invalid.maximum_host_compile_source_bytes = 7;
    CHECK(!script_execution_policy_for_profile(
               (ScriptExecutionProfile) 99, &invalid)
          && invalid.maximum_host_compile_source_bytes == 0);

    BrowserDeviceProfile device;
    BrowserConfig config;
    browser_device_profile_psp3000(&device);
    browser_config_init(&config, &device);
    CHECK(config.javascript.execution_policy.maximum_host_compile_source_bytes
              == realistic.maximum_host_compile_source_bytes
          && config.javascript.maximum_file_bytes
               <= config.javascript.execution_policy
                      .maximum_host_compile_source_bytes);

    Budget budget;
    budget_init(&budget, 16u * MIB);
    budget_install_lexbor(&budget);
    static const char html[] =
        "<!doctype html><html><body><svg id=parsed-svg>"
        "<g id=parsed-group></g></svg><script id=target>"
        "globalThis.__streamProbe = new ReadableStream();"
        "</script></body></html>";
    PocDocument document;
    CHECK(document_parse(&document, &budget, html, sizeof(html) - 1, 17));
    size_t document_bytes = budget.current;

    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));

    /* An unavailable bytecode object and an explicitly disabled bytecode path
       both compile the same bootstrap sources exactly once. The restore fault
       occurs after deserialization, where admission used to be double-counted
       before source fallback. */
    ScriptResult unavailable_bytecode_result;
    ScriptResult disabled_bytecode_result;
    bool unavailable_fault_set = setenv(
        "TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE", "1", 1) == 0;
    bool unavailable_bytecode_ran = unavailable_fault_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 8u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &unavailable_bytecode_result);
    bool unavailable_fault_unset =
        unsetenv("TILEFINCH_TEST_BOOTSTRAP_BYTECODE_UNAVAILABLE") == 0;
    bool disabled_bytecode_set =
        setenv("TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE", "1", 1) == 0;
    bool disabled_bytecode_ran = disabled_bytecode_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 8u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &disabled_bytecode_result);
    bool disabled_bytecode_unset =
        unsetenv("TILEFINCH_DISABLE_BOOTSTRAP_BYTECODE") == 0;
    if (!unavailable_bytecode_ran || !disabled_bytecode_ran) {
        fprintf(stderr,
                "bootstrap fallback probe: unavailable=%d error=\"%s\" "
                "disabled=%d error=\"%s\"\n",
                unavailable_bytecode_ran,
                unavailable_bytecode_result.error,
                disabled_bytecode_ran, disabled_bytecode_result.error);
    }
    CHECK(unavailable_fault_set && unavailable_fault_unset
          && disabled_bytecode_set && disabled_bytecode_unset
          && unavailable_bytecode_ran && disabled_bytecode_ran
          && unavailable_bytecode_result.bootstrap_bytecode_restore_failures
               != 0
          && unavailable_bytecode_result.host_compile_attempts
               == disabled_bytecode_result.host_compile_attempts
          && unavailable_bytecode_result.host_compile_source_bytes
               == disabled_bytecode_result.host_compile_source_bytes
          && unavailable_bytecode_result.bootstrap_lazy_module_loads == 1
          && disabled_bytecode_result.bootstrap_lazy_module_loads == 1
          && unavailable_bytecode_result.bootstrap_lazy_module_failures == 0
          && disabled_bytecode_result.bootstrap_lazy_module_failures == 0
          && budget.current == document_bytes);

    /* Formatting made the core DOM source larger than its serialized
       bytecode. Tight heaps must follow the current representation sizes,
       not the historical fixed-size heuristic. */
    ScriptResult tight_heap_result;
    bool tight_window_set =
        setenv("TILEFINCH_JS_BOOT_WINDOW_KB", "1024", 1) == 0;
    bool tight_heap_ran = tight_window_set
        && scripts_run_document_at_context_with_policy(
               &document, &budget, 6u * MIB, 4000,
               "https://example.test/", &viewport, &strict,
               &tight_heap_result);
    bool tight_window_unset =
        unsetenv("TILEFINCH_JS_BOOT_WINDOW_KB") == 0;
    bool tight_heap_ok = tight_window_set && tight_heap_ran
        && tight_window_unset
        && tight_heap_result.bootstrap_bytecode_source_preferences == 0
        && tight_heap_result.bootstrap_bytecode_preferred_source_bytes == 0
        && tight_heap_result.bootstrap_bytecode_restores != 0
        && tight_heap_result.bootstrap_bytecode_stored_bytes != 0
        && tight_heap_result.bootstrap_bytecode_stored_bytes
             == tight_heap_result.bootstrap_bytecode_bytes
        && budget.current == document_bytes;
    if (!tight_heap_ok) {
        fprintf(stderr,
                "tight bootstrap run failed: set=%d ran=%d unset=%d "
                "error=\"%s\" preferences=%zu "
                "source-bytes=%zu restores=%zu failures=%zu "
                "stored=%zu bytecode=%zu current=%zu\n",
                tight_window_set, tight_heap_ran, tight_window_unset,
                tight_heap_result.error,
                tight_heap_result.bootstrap_bytecode_source_preferences,
                tight_heap_result.bootstrap_bytecode_preferred_source_bytes,
                tight_heap_result.bootstrap_bytecode_restores,
                tight_heap_result.bootstrap_bytecode_restore_failures,
                tight_heap_result.bootstrap_bytecode_stored_bytes,
                tight_heap_result.bootstrap_bytecode_bytes,
                budget.current);
    }
    CHECK(tight_heap_ok);

    /* A limit below the first built-in source must reject it before QuickJS
       parsing begins and fully roll runtime ownership back. */
    ScriptExecutionPolicy tiny = strict;
    tiny.maximum_host_compile_source_bytes = 1;
    ScriptResult one_shot_result;
    CHECK(!scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &tiny,
              &one_shot_result)
          && one_shot_result.host_compile_attempts == 1
          && one_shot_result.host_compile_rejections == 1
          && one_shot_result.host_compile_source_bytes == 0
          && one_shot_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && budget.current == document_bytes);
    ScriptRuntimeOptions tiny_options = {
        .viewport = viewport,
        .execution_policy = tiny,
        .defer_document_scripts = true
    };
    ScriptResult tiny_result;
    ScriptRuntime *runtime = script_runtime_create_configured(
        &document, &budget, 4u * MIB, 1000,
        "https://example.test/", &tiny_options, &tiny_result);
    CHECK(runtime == NULL
          && tiny_result.host_compile_attempts == 1
          && tiny_result.host_compile_rejections == 1
          && tiny_result.host_compile_source_bytes == 0
          && tiny_result.max_nonpreemptible_compile_bytes == 0
          && tiny_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && tiny_result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_INTERNAL
          && strstr(tiny_result.error, "rejected before compile") != NULL
          && budget.current == document_bytes);

    /* Projected-time admission mirrors the byte ceiling: the throughput
       model is enforced before QuickJS parsing begins, fails truthfully,
       and rolls runtime ownership back to zero. */
    ScriptExecutionPolicy modeled = strict;
    modeled.maximum_host_compile_source_bytes = 0;
    modeled.maximum_host_compile_projected_us = 1000;
    modeled.modeled_compile_bytes_per_ms = 1;
    ScriptResult projected_result;
    CHECK(!scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &modeled,
              &projected_result)
          && projected_result.host_compile_attempts == 1
          && projected_result.host_compile_rejections == 1
          && projected_result.host_compile_projected_rejections == 1
          && projected_result.host_compile_projected_rejected_bytes != 0
          && projected_result.host_compile_rejected_source_bytes == 0
          && projected_result.host_compile_source_bytes == 0
          && projected_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_PROJECTED_TIME
          && strstr(projected_result.error, "modeled compile time") != NULL
          && budget.current == document_bytes);

    /* A generous model admits everything and leaves the projected counters
       untouched. */
    ScriptExecutionPolicy generous = strict;
    generous.maximum_host_compile_projected_us = UINT64_MAX;
    generous.modeled_compile_bytes_per_ms = SIZE_MAX;
    ScriptResult generous_result;
    CHECK(scripts_run_document_at_context_with_policy(
              &document, &budget, 4u * MIB, 1000,
              "https://example.test/", &viewport, &generous,
              &generous_result)
          && generous_result.host_compile_rejections == 0
          && generous_result.host_compile_projected_rejections == 0
          && generous_result.host_compile_projected_rejected_bytes == 0
          && generous_result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED);

    strict.slow_compile_threshold_us = UINT64_MAX;
    strict.slow_callback_threshold_us = UINT64_MAX;
    ScriptRuntimeOptions options = {
        .viewport = viewport,
        .execution_policy = strict,
        .defer_document_scripts = true,
        .allow_test_network_primitive_overrides = true
    };
    ScriptResult result;
    /* DOM functionality probes below churn ~1100 nodes; the pristine
       (unpatched) QuickJS interpreter at -O0 needs more than a second. */
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success
          && result.host_compile_attempts >= 5
          && result.host_compile_rejections == 0
          && result.host_compile_source_limit_bytes
               == strict.maximum_host_compile_source_bytes
          && result.nonpreemptible_compile_count == 0
          && result.nonpreemptible_callback_count == 0);

    puts("test: large network delivery enters from the collected heap floor");
    size_t response_heap_before = script_runtime_heap_remaining(runtime);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{for(let i=0;i<256;i++){const cycle={"
              "pad:'x'.repeat(1024)};cycle.self=cycle;}"
              "globalThis.pocSummary='NETWORK-PRESSURE-READY'})()",
              "<network-pressure-setup>", &result)
          && strcmp(result.summary, "NETWORK-PRESSURE-READY") == 0);
    size_t response_heap_with_cycles = script_runtime_heap_remaining(runtime);
    CHECK(response_heap_with_cycles < response_heap_before
          && js_rt_prepare_network_response_delivery(
                 runtime, 512u * 1024u - 1u) == 0
          && script_runtime_heap_remaining(runtime)
                 == response_heap_with_cycles);
    (void) js_rt_prepare_network_response_delivery(runtime, 512u * 1024u);
    CHECK(script_runtime_heap_remaining(runtime) > response_heap_with_cycles);

    puts("test: bounded Gamepad API publishes one stable PSP controller");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__gamepadEvents=[];const bridge="
              "Object.getOwnPropertyDescriptor(globalThis,"
              "'__tilefinchUpdateGamepad');"
              "const first=navigator.getGamepads(),second="
              "navigator.getGamepads();first.push(null);"
              "addEventListener('gamepadconnected',e=>"
              "__gamepadEvents.push(e.type+':'+e.gamepad.index));"
              "addEventListener('gamepaddisconnected',e=>"
              "__gamepadEvents.push(e.type+':'+e.gamepad.index));"
              "globalThis.pocSummary=bridge&&!bridge.writable&&"
              "!bridge.configurable&&first!==second&&first.length===5&&"
              "second.length===4&&second.every(value=>value===null)?"
              "'GAMEPAD-HIDDEN':'GAMEPAD-LEAKED'",
              "<gamepad-install>", &result)
          && strcmp(result.summary, "GAMEPAD-HIDDEN") == 0);
    TilefinchGamepadState gamepad = {
        .buttons = (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_PRIMARY)
            | (UINT32_C(1) << TILEFINCH_GAMEPAD_BUTTON_DPAD_UP),
        .axes = {INT16_MAX, -INT16_MAX, 0, 0},
        .timestamp_ms = 1234,
        .connected = true
    };
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const first=navigator.getGamepads(),p=first[0],"
              "secondSnapshot=navigator.getGamepads(),second="
              "secondSnapshot[0];"
              "globalThis.__gamepadRef=p;"
              "globalThis.__gamepadButtonsRef=p.buttons;"
              "globalThis.pocSummary="
              "first!==secondSnapshot&&first.length===4&&p===second&&"
              "first.slice(1).every(value=>value===null)&&"
              "p.id==='PSP Built-in Controller'"
              "&&p.mapping==='standard'&&p.buttons.length===17"
              "&&p.buttons[0].pressed&&p.buttons[12].value===1"
              "&&!p.buttons[1].pressed&&p.axes.length===4"
              "&&p.axes[0]===1&&p.axes[1]===-1&&p.timestamp===1234?"
              "'GAMEPAD-MAPPED':'GAMEPAD-BAD'})()",
              "<gamepad-connected>", &result)
          && strcmp(result.summary, "GAMEPAD-MAPPED") == 0
          && script_runtime_advance(runtime, 0, 8, &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=__gamepadEvents.join(',')",
              "<gamepad-connect-event>", &result)
          && strcmp(result.summary, "gamepadconnected:0") == 0);
    gamepad.axes[0] = 8192;
    gamepad.axes[1] = -4096;
    gamepad.timestamp_ms = 1240;
    CHECK(script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const p=navigator.getGamepads()[0];"
              "globalThis.pocSummary=p===__gamepadRef&&"
              "p.buttons===__gamepadButtonsRef&&p.buttons[0].pressed&&"
              "p.buttons[12].pressed&&!p.buttons[1].pressed&&"
              "p.axes[0]>0.24&&p.axes[0]<0.26&&"
              "p.axes[1]>-0.13&&p.axes[1]<-0.12&&p.timestamp===1240?"
              "'GAMEPAD-AXIS-UPDATED':'GAMEPAD-BAD'})()",
              "<gamepad-axis-only>", &result)
          && strcmp(result.summary, "GAMEPAD-AXIS-UPDATED") == 0);
    CHECK(tilefinch_gamepad_state_update(
              &gamepad, false, 0, 0, 0, 1250)
          && script_runtime_set_gamepad_state(runtime, &gamepad)
          && script_runtime_advance(runtime, 0, 8, &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const pads=navigator.getGamepads();globalThis.pocSummary="
              "pads.length===4&&pads.every(value=>value===null)&&"
              "__gamepadEvents.join(',')==='gamepadconnected:0,"
              "gamepaddisconnected:0'?'GAMEPAD-DISCONNECTED':'GAMEPAD-BAD'})()",
              "<gamepad-disconnected>", &result)
          && strcmp(result.summary, "GAMEPAD-DISCONNECTED") == 0);

    puts("test: Tilefinch page-controls requests require user activation");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "navigator.tilefinch.requestPageControls().then(()=>"
              "globalThis.pocSummary='PAGE-CONTROLS-DIRECT-BAD',error=>"
              "globalThis.pocSummary=error.name==='NotAllowedError'&&"
              "navigator.tilefinch.pageControlsExitChord==='Start+Select'"
              "?'PAGE-CONTROLS-DIRECT-BLOCKED':'PAGE-CONTROLS-WRONG');"
              "const control=document.createElement('button');"
              "control.id='page-controls-target';document.body.append(control);"
              "control.addEventListener('click',()=>navigator.tilefinch."
              "requestPageControls().then(()=>globalThis.pocSummary="
              "'PAGE-CONTROLS-REQUESTED'));",
              "<page-controls-setup>", &result)
          && strcmp(result.summary, "PAGE-CONTROLS-DIRECT-BLOCKED") == 0
          && !script_runtime_page_fullscreen_active(runtime));
    lxb_dom_node_t *page_controls_target = find_element_id(
        lxb_dom_interface_node(document.html), "page-controls-target");
    CHECK(page_controls_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, page_controls_target, &result)
          && strcmp(result.summary, "PAGE-CONTROLS-REQUESTED") == 0
          && script_runtime_page_fullscreen_active(runtime)
          && script_runtime_take_page_controls_notice(runtime)
          && script_runtime_exit_page_fullscreen(runtime));

    puts("test: page-controls notice may quiet only repeats in a document");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "const quiet=document.createElement('button');"
              "quiet.id='page-controls-quiet';document.body.append(quiet);"
              "quiet.addEventListener('click',()=>navigator.tilefinch."
              "requestPageControls(undefined,{notice:'once'}).then(()=>"
              "globalThis.pocSummary='PAGE-CONTROLS-QUIET'));"
              "globalThis.pocSummary=navigator.tilefinch.pageControlsNotices"
              ".join(',')==='always,once'&&Object.isFrozen(navigator."
              "tilefinch.pageControlsNotices)?'NOTICE-DETECTABLE':'NOTICE-BAD';",
              "<page-controls-notice-setup>", &result)
          && strcmp(result.summary, "NOTICE-DETECTABLE") == 0);
    lxb_dom_node_t *page_controls_quiet = find_element_id(
        lxb_dom_interface_node(document.html), "page-controls-quiet");
    /* A repeat that asks for {notice: "once"} is quiet. */
    CHECK(page_controls_quiet != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, page_controls_quiet, &result)
          && strcmp(result.summary, "PAGE-CONTROLS-QUIET") == 0
          && script_runtime_page_fullscreen_active(runtime)
          && !script_runtime_take_page_controls_notice(runtime)
          && script_runtime_exit_page_fullscreen(runtime));
    /* The request is consumed: a later plain claim announces again. */
    CHECK(script_runtime_dispatch_activation_node(
              runtime, page_controls_target, &result)
          && strcmp(result.summary, "PAGE-CONTROLS-REQUESTED") == 0
          && script_runtime_take_page_controls_notice(runtime)
          && script_runtime_exit_page_fullscreen(runtime));
    /* A document that has not shown the notice yet (a fresh bridge) shows
       it even when its first claim asks for "once". */
    runtime->bridge.page_controls_notice_shown = false;
    CHECK(script_runtime_dispatch_activation_node(
              runtime, page_controls_quiet, &result)
          && strcmp(result.summary, "PAGE-CONTROLS-QUIET") == 0
          && script_runtime_take_page_controls_notice(runtime)
          && script_runtime_exit_page_fullscreen(runtime)
          && script_runtime_take_page_controls_notice(NULL));

    puts("test: transient user activation reaches microtasks and navigation");
    /* Earlier cases deliberately exercise trusted page controls on this
       long-lived fixture. Reset only the test-owned activation state so this
       case can pin the initial Navigator contract independently. */
    runtime->bridge.user_activation_active = false;
    runtime->bridge.user_activation_has_been_active = false;
    runtime->bridge.user_activation_expires_ms = 0;
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__activationOriginalURL=location.href;"
              "globalThis.__activationTarget=document.createElement('button');"
              "__activationTarget.id='activation-target';"
              "document.body.append(__activationTarget);"
              "__activationTarget.addEventListener('click',()=>"
              "Promise.resolve().then(()=>{globalThis.pocSummary="
              "navigator.userActivation.isActive&&"
              "navigator.userActivation.hasBeenActive?"
              "'ACTIVATION-MICROTASK-OK':'ACTIVATION-MICROTASK-BAD';"
              "location.href='https://example.test/activated'}));"
              "globalThis.pocSummary=!navigator.userActivation.isActive&&"
              "!navigator.userActivation.hasBeenActive&&"
              "typeof __tilefinchUserActivationState==='undefined'?"
              "'ACTIVATION-INITIAL-OK':'ACTIVATION-INITIAL-BAD'",
              "<user-activation-setup>", &result)
          && strcmp(result.summary, "ACTIVATION-INITIAL-OK") == 0);
    char activation_url[NAVIGATION_URL_LIMIT];
    bool activation_replace = false;
    bool activation_snapshot = true;
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "__activationTarget.click()",
              "<synthetic-activation>", &result)
          && strcmp(result.summary, "ACTIVATION-MICROTASK-BAD") == 0
          && script_runtime_consume_navigation(
                 runtime, activation_url, sizeof(activation_url),
                 &activation_replace, &activation_snapshot)
          && strcmp(activation_url,
                    "https://example.test/activated") == 0
          && !activation_replace && !activation_snapshot);
    lxb_dom_node_t *activation_target = find_element_id(
        lxb_dom_interface_node(document.html), "activation-target");
    activation_snapshot = false;
    CHECK(activation_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, activation_target, &result)
          && strcmp(result.summary, "ACTIVATION-MICROTASK-OK") == 0
          && script_runtime_consume_navigation(
                 runtime, activation_url, sizeof(activation_url),
                 &activation_replace, &activation_snapshot)
          && activation_snapshot
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "history.replaceState(null,'',__activationOriginalURL);"
                 "__activationTarget.remove();"
                 "delete globalThis.__activationTarget;"
                 "delete globalThis.__activationOriginalURL;"
                 "delete globalThis.__tilefinchLastClickDefault",
                 "<user-activation-cleanup>", &result));
    runtime->bridge.user_activation_active = false;
    runtime->bridge.user_activation_expires_ms = 0;

    puts("test: asynchronous callback entry observes cancellation");
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__callbackBoundaryRan=0;"
              "setTimeout(()=>{globalThis.__callbackBoundaryRan=1},0);"
              "globalThis.pocSummary='CALLBACK-BOUNDARY-ARMED'",
              "<callback-boundary-arm>", &result)
          && strcmp(result.summary, "CALLBACK-BOUNDARY-ARMED") == 0);
    CallbackAbortCooperate callback_abort = {0};
    TilefinchPlatformServices callback_abort_services = {
        .context = &callback_abort,
        .cooperate = callback_abort_cooperate
    };
    size_t callback_poll_calls_before =
        result.host_callback_calls_with_interrupt_polls;
    tilefinch_platform_set_services(&callback_abort_services);
    bool callback_advanced = script_runtime_advance(runtime, 1, 1, &result);
    tilefinch_platform_set_services(NULL);
    CHECK(!callback_advanced && result.interrupted
          && callback_abort.calls == 1
          && result.host_callback_calls_with_interrupt_polls
                 > callback_poll_calls_before);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=String(globalThis.__callbackBoundaryRan)",
              "<callback-boundary-observe>", &result)
          && strcmp(result.summary, "0") == 0);

    static const char hardening_probe[] =
        "(()=>{const hidden=!Object.keys(globalThis).some(key=>"
        "key.startsWith('__tilefinch')),lookup=Object.getOwnPropertyDescriptor("
        "globalThis,'__tilefinchDiagnosticLookup'),wrap="
        "Object.getOwnPropertyDescriptor(globalThis,'__tilefinchWrap'),native="
        "Object.getOwnPropertyDescriptor(globalThis,'__tilefinchClone'),retire="
        "Object.getOwnPropertyDescriptor(globalThis,"
        "'__tilefinchRetireNativeNodeState');"
        "let privateBlocked=false,evalBlocked=false;"
        "try{__tilefinchDiagnosticLookup('timers')}catch(error){"
        "privateBlocked=error instanceof ReferenceError}"
        "try{__tilefinchDiagnosticLookup('timers.length=0')}catch(error){"
        "evalBlocked=error instanceof ReferenceError}"
        "const node=document.createElement('div'),handle=node.__handle;"
        "node.__handle=2147483647;let redefineBlocked=false;"
        "try{Object.defineProperty(node,'__handle',{value:1})}"
        "catch(error){redefineBlocked=error instanceof TypeError}"
        "globalThis.pocSummary=hidden&&lookup&&!lookup.enumerable"
        "&&!lookup.writable&&!lookup.configurable&&wrap&&!wrap.enumerable"
        "&&!wrap.writable&&!wrap.configurable&&privateBlocked&&evalBlocked"
        "&&native&&!native.enumerable&&!native.writable&&!native.configurable"
        "&&retire&&!retire.enumerable&&!retire.writable"
        "&&!retire.configurable"
        "&&node.__handle===handle&&redefineBlocked"
        "?'REALM-HARDENING-OK':'REALM-HARDENING-FAILED:'+"
        "JSON.stringify({hidden,visible:Object.keys(globalThis).filter("
        "key=>key.startsWith('__tilefinch')).slice(0,16),"
        "lookup:[!!lookup,lookup?.enumerable,"
        "lookup?.writable,lookup?.configurable],wrap:[!!wrap,"
        "wrap?.enumerable,wrap?.writable,wrap?.configurable],"
        "native:[!!native,native?.enumerable,native?.writable,"
        "native?.configurable],retire:[!!retire,retire?.enumerable,"
        "retire?.writable,retire?.configurable],"
        "privateBlocked,evalBlocked,handle,nodeHandle:node.__handle,"
        "redefineBlocked});})()";
    bool hardening_ok = script_runtime_evaluate_diagnostic(
        runtime, hardening_probe, "<realm-hardening-probe>", &result);
    if (!hardening_ok
        || strcmp(result.summary, "REALM-HARDENING-OK") != 0) {
        fprintf(stderr, "hardening probe: ok=%d summary=%s error=%s\n",
                hardening_ok, result.summary, result.error);
    }
    CHECK(hardening_ok
          && strcmp(result.summary, "REALM-HARDENING-OK") == 0);

    puts("test: native event listeners checkpoint microtasks between callbacks");
    static const char native_event_order_setup[] =
        "globalThis.__nativeEventOrder=[];"
        "addEventListener('message',()=>{"
        "__nativeEventOrder.push('first');"
        "Promise.resolve().then(()=>__nativeEventOrder.push('microtask'))});"
        "addEventListener('message',()=>__nativeEventOrder.push('second'));";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_event_order_setup,
              "<native-event-order-setup>", &result)
          && script_runtime_dispatch_message(
              runtime, "{\"probe\":true}", "https://source.test", -2,
              &result)
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=__nativeEventOrder.join(',')",
              "<native-event-order-result>", &result)
          && strcmp(result.summary, "first,microtask,second") == 0);

    puts("test: exception formatting cannot poison the runtime");
    static const char hostile_exception_probe[] =
        "throw {[Symbol.toPrimitive](){throw new Error('secondary')}}";
    CHECK(!script_runtime_evaluate_diagnostic(
              runtime, hostile_exception_probe,
              "<hostile-exception-formatting>", &result));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='EXCEPTION-RECOVERED'",
              "<hostile-exception-recovery>", &result)
          && strcmp(result.summary, "EXCEPTION-RECOVERED") == 0);
    static const char hostile_rejection_probe[] =
        "globalThis.__hostileRejection=Promise.reject({"
        "[Symbol.toPrimitive](){throw new Error('secondary')},"
        "get stack(){throw new Error('secondary-stack')}});"
        "globalThis.pocSummary='REJECTION-FORMATTED'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, hostile_rejection_probe,
              "<hostile-rejection-formatting>", &result)
          && strcmp(result.summary, "REJECTION-FORMATTED") == 0
          && script_runtime_evaluate_diagnostic(
              runtime, "__hostileRejection.catch(()=>{});"
                       "globalThis.pocSummary='REJECTION-RECOVERED'",
              "<hostile-rejection-recovery>", &result)
          && strcmp(result.summary, "REJECTION-RECOVERED") == 0);

    puts("test: selector-list commas ignore quoted syntax");
    static const char quoted_selector_probe[] =
        "(()=>{const a=document.createElement('div'),"
        "b=document.createElement('div');a.title='(';a.id='quoted-open';"
        "b.id='quoted-other';document.body.append(a,b);"
        "const first=document.querySelectorAll('[title=\"(\"], #quoted-other'),"
        "second=document.querySelectorAll('[title=\"a,b\"], #quoted-other');"
        "a.title='a,b';const third=document.querySelectorAll("
        "'[title=\"a,b\"], #quoted-other');a.remove();b.remove();"
        "globalThis.pocSummary=first.length===2&&second.length===1"
        "&&third.length===2?'QUOTED-SELECTOR-OK':'QUOTED-SELECTOR-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, quoted_selector_probe, "<quoted-selector-probe>",
              &result)
          && strcmp(result.summary, "QUOTED-SELECTOR-OK") == 0);

    puts("test: lower-level web compatibility regressions stay fixed");
    static const char compatibility_probe[] =
        "(async()=>{const source=new ArrayBuffer(8),"
        "view=new DataView(source,2,3);view.setUint8(0,41);"
        "const cloned=structuredClone(view),dataViewOk="
        "cloned instanceof DataView&&cloned.byteOffset===2"
        "&&cloned.byteLength===3&&cloned.getUint8(0)===41;"
        "const shared={v:1},graph={left:shared,right:shared,list:[shared]};"
        "graph.self=graph;const clonedGraph=structuredClone(graph),"
        "graphOk=clonedGraph.self===clonedGraph&&clonedGraph.left"
        "===clonedGraph.right&&clonedGraph.list[0]===clonedGraph.left"
        "&&clonedGraph.left!==shared&&clonedGraph.left.v===1;"
        "const clonedMap=structuredClone(new Map([['when',new Date(5)],"
        "['set',new Set([1,2])],['re',/a+/gi]])),mapOk="
        "clonedMap.get('when').getTime()===5&&clonedMap.get('set').has(2)"
        "&&clonedMap.get('re').flags==='gi'&&clonedMap.get('re').source"
        "==='a+';let cloneErrorName='';try{structuredClone({f(){}});}"
        "catch(error){cloneErrorName=error.name;}"
        "const sharedBuffer=new ArrayBuffer(4),viewA=new Uint8Array("
        "sharedBuffer),viewB=new Uint8Array(sharedBuffer,2),clonedViews="
        "structuredClone({a:viewA,b:viewB});clonedViews.a[3]=9;"
        "const clonedErr=structuredClone(new RangeError('bounded')),"
        "arrayPrototype=Object.create(Array.prototype);"
        "arrayPrototype[1]={inherited:true};const arrayValue=[1,,3];"
        "Object.setPrototypeOf(arrayValue,arrayPrototype);"
        "arrayValue.metadata={value:41};"
        "const clonedArray=structuredClone(arrayValue),arrayPropertiesOk="
        "clonedArray.length===3&&clonedArray[0]===1&&clonedArray[2]===3"
        "&&!Object.prototype.hasOwnProperty.call(clonedArray,'1')"
        "&&clonedArray[1]===undefined&&clonedArray.metadata.value===41;"
        "cloneOk=graphOk&&mapOk&&arrayPropertiesOk"
        "&&cloneErrorName==='DataCloneError'"
        "&&clonedViews.b[1]===9&&clonedViews.a.buffer!==sharedBuffer"
        "&&clonedErr instanceof RangeError&&clonedErr.message==='bounded';"
        "const closingURL=URL.createObjectURL(new Blob([\"postMessage('before')"
        ";close();postMessage('after');setTimeout(()=>postMessage('late'),0);"
        "\"])),closing=new Worker(closingURL),closeMessages=[];"
        "closing.onmessage=event=>closeMessages.push(event.data);"
        "const libURL=URL.createObjectURL(new Blob([\"function libValue(){return 41}\"])),"
        "isoURL=URL.createObjectURL(new Blob([\"importScripts(\"+JSON.stringify(libURL)+\");"
        "const uaData=navigator.userAgentData,uaPromise=uaData&&"
        "uaData.getHighEntropyValues([]),connection=navigator.connection,"
        "permissionPromise=navigator.permissions.query({name:'geolocation'}),"
        "storagePromise=navigator.storage.persisted(),"
        "gpuPromise=navigator.gpu&&navigator.gpu.requestAdapter();"
        "storagePromise.catch(()=>{});"
        "const bare=(function(){return this})();"
        "postMessage([this===self,self===globalThis,typeof window,bare===self,"
        "typeof document,libValue()+1,typeof console,"
        "Object.getPrototypeOf(self)===DedicatedWorkerGlobalScope.prototype,"
        "typeof onmessage,typeof NavigatorUAData==='function',"
        "typeof NetworkInformation==='function',"
        "typeof NavigatorUAData==='function'&&uaData instanceof NavigatorUAData,"
        "typeof NetworkInformation==='function'&&connection instanceof NetworkInformation,"
        "uaPromise instanceof Promise,permissionPromise instanceof Promise,"
        "storagePromise instanceof Promise,gpuPromise instanceof Promise])\"])),"
        "iso=new Worker(isoURL),isoMessages=[];"
        "iso.onmessage=event=>isoMessages.push(event.data);"
        "const form=document.createElement('form'),"
        "select=document.createElement('select'),"
        "first=document.createElement('option'),"
        "second=document.createElement('option');"
        "first.value='first';second.value='second';"
        "select.append(first,second);form.append(select);"
        "document.body.append(form);select.selectedIndex=1;form.reset();"
        "const selectOk=select.selectedIndex===0&&first.selected"
        "&&!second.selected;form.remove();"
        "const flex=document.createElement('div');flex.style.flex='2 3 25%';"
        "document.body.append(flex);const flexValue=flex.style.flex,"
        "flexOk=flexValue==='2 3 25%';flex.remove();"
        "const stream=new ReadableStream({start(){}}),reader=stream.getReader(),"
        "pending=reader.read().then(()=>'',error=>error.name);"
        "let releaseThrew=false;try{reader.releaseLock()}catch(_){"
        "releaseThrew=true}const pendingName=await pending,streamOk="
        "!releaseThrew&&pendingName==='TypeError'&&!stream.locked;"
        "for(let hop=0;hop<8&&(closeMessages.length<2||isoMessages.length<1);hop++)"
        "await new Promise(resolve=>setTimeout(resolve,0));"
        "await new Promise(resolve=>setTimeout(resolve,0));"
        "const closeOk=closeMessages.join(',')==='before,after';"
        "const isoOk=JSON.stringify(isoMessages[0])===JSON.stringify(["
        "true,true,'undefined',true,'undefined',42,'object',true,'object',"
        "true,true,true,true,true,true,true,true]);"
        "iso.terminate();URL.revokeObjectURL(closingURL);URL.revokeObjectURL(isoURL);URL.revokeObjectURL(libURL);"
        "globalThis.pocSummary=dataViewOk&&cloneOk&&closeOk&&isoOk&&selectOk&&flexOk"
        "&&streamOk?'COMPATIBILITY-REGRESSIONS-OK'"
        ":'COMPATIBILITY-REGRESSIONS-FAILED:'"
        "+JSON.stringify({dataViewOk,cloneOk,graphOk,mapOk,cloneErrorName,"
        "closeOk,closeMessages,isoOk,isoMessages,selectOk,flexOk,streamOk,flexValue});"
        "})().catch(error=>{"
        "globalThis.pocSummary='COMPATIBILITY-REGRESSIONS-ERROR:'+error});";
    bool compatibility_ok = script_runtime_evaluate_diagnostic(
        runtime, compatibility_probe, "<compatibility-regressions>", &result);
    for (size_t tick = 0; compatibility_ok && tick < 24
         && strncmp(result.summary, "COMPATIBILITY-REGRESSIONS-", 26) != 0;
         tick++) {
        compatibility_ok = script_runtime_advance(runtime, 0, 64, &result);
    }
    if (!compatibility_ok
        || strcmp(result.summary, "COMPATIBILITY-REGRESSIONS-OK") != 0) {
        fprintf(stderr, "compatibility probe: ok=%d summary=%s error=%s\n",
                compatibility_ok, result.summary, result.error);
    }
    CHECK(compatibility_ok
          && strcmp(result.summary, "COMPATIBILITY-REGRESSIONS-OK") == 0);

    puts("test: checkpoint continuation exceptions stay contained");
    CHECK(runtime->checkpoint_continuation_count == 0u
          && script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.__tilefinchThrowingContinuation=()=>{"
              "throw new Error('continuation failure')};",
              "<checkpoint-continuation-setup>", &result));
    JSValue continuation_global = JS_GetGlobalObject(runtime->context);
    JSValue throwing_continuation = JS_GetPropertyStr(
        runtime->context, continuation_global,
        "__tilefinchThrowingContinuation");
    CHECK(JS_IsFunction(runtime->context, throwing_continuation));
    size_t continuation_slot =
        ((size_t) runtime->checkpoint_continuation_head
         + runtime->checkpoint_continuation_count)
        % SCRIPT_CHECKPOINT_CONTINUATION_LIMIT;
    runtime->checkpoint_continuations[continuation_slot] =
        JS_DupValue(runtime->context, throwing_continuation);
    runtime->checkpoint_continuation_count++;
    size_t continuation_errors_before =
        runtime->result.uncaught_callback_errors;
    CHECK(js_rt_runtime_run_jobs(runtime));
    js_rt_runtime_update_result(runtime, &result);
    CHECK(runtime->checkpoint_continuation_count == 0u
          && result.uncaught_callback_errors
                 == continuation_errors_before + 1u);
    JS_FreeValue(runtime->context, throwing_continuation);
    JS_FreeValue(runtime->context, continuation_global);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "delete globalThis.__tilefinchThrowingContinuation;"
              "globalThis.pocSummary='CONTINUATION-RECOVERED'",
              "<checkpoint-continuation-recovery>", &result)
          && strcmp(result.summary, "CONTINUATION-RECOVERED") == 0);

    puts("test: oversized data script fails visibly and remains counted");
    size_t oversized_data_failed_before = result.dynamic_scripts_failed;
    /* The 16 KiB src exceeds the serialized URL limit, so the refusal does
       not depend on heap headroom. The element counts its own events: a
       prepared script fires exactly one of load and error. */
    static const char oversized_data_script_probe[] =
        "(()=>{const script=document.createElement('script'),"
        "events=globalThis.__oversizedDataEvents={script,load:0,error:0};"
        "script.src='data:text/javascript,'+'x'.repeat(16384);"
        "script.addEventListener('load',()=>{events.load++;"
        "globalThis.pocSummary='OVERSIZED-DATA-LOADED'});"
        "script.addEventListener('error',()=>{events.error++;"
        "globalThis.pocSummary='OVERSIZED-DATA-ERROR'});"
        "document.head.appendChild(script);"
        "globalThis.pocSummary='OVERSIZED-DATA-PENDING';})()";
    bool oversized_data_ok = script_runtime_evaluate_diagnostic(
        runtime, oversized_data_script_probe, "<oversized-data-script>",
        &result);
    for (size_t tick = 0; oversized_data_ok && tick < 16
         && strcmp(result.summary, "OVERSIZED-DATA-ERROR") != 0; tick++) {
        oversized_data_ok = script_runtime_advance(runtime, 0, 128, &result);
    }
    CHECK(oversized_data_ok
          && strcmp(result.summary, "OVERSIZED-DATA-ERROR") == 0
          && result.dynamic_scripts_failed
                 == oversized_data_failed_before + 1u);
    for (size_t tick = 0; oversized_data_ok && tick < 4; tick++) {
        oversized_data_ok = script_runtime_advance(runtime, 0, 128, &result);
    }
    /* Detach the element once settled: it precedes the page's own script in
       document order, and later host-level completions below would otherwise
       dispatch their load and error events to its listeners. */
    static const char oversized_data_settled[] =
        "(()=>{const events=globalThis.__oversizedDataEvents;"
        "delete globalThis.__oversizedDataEvents;events.script.remove();"
        "globalThis.pocSummary='OVERSIZED-DATA-EVENTS:'+events.load+':'+"
        "events.error})()";
    CHECK(oversized_data_ok
          && script_runtime_evaluate_diagnostic(
                 runtime, oversized_data_settled,
                 "<oversized-data-settled>", &result)
          && strcmp(result.summary, "OVERSIZED-DATA-EVENTS:0:1") == 0
          && result.dynamic_scripts_failed
                 == oversized_data_failed_before + 1u);

    static const char trusted_dispatch_setup[] =
        "globalThis.__tilefinchTrustedDispatchHits=0;"
        "document.querySelector('script').addEventListener("
        "'trusted-dispatch',()=>{"
        "globalThis.__tilefinchTrustedDispatchHits++});"
        "globalThis.__tilefinchWrap=()=>({dispatchEvent(){return false}});"
        "globalThis.__tilefinchDispatchHandle=()=>false;";
    lxb_dom_node_t *trusted_dispatch_target = find_script(
        lxb_dom_interface_node(document.html));
    CHECK(trusted_dispatch_target != NULL
          && script_runtime_evaluate_diagnostic(
              runtime, trusted_dispatch_setup,
              "<trusted-dispatch-setup>", &result)
          && script_runtime_dispatch_node(
              runtime, trusted_dispatch_target,
              "trusted-dispatch", &result)
          && !result.last_event_cancelled);
    static const char trusted_dispatch_check[] =
        "globalThis.pocSummary="
        "__tilefinchTrustedDispatchHits===1?'TRUSTED-DISPATCH-OK':"
        "'TRUSTED-DISPATCH-FAILED:'+__tilefinchTrustedDispatchHits";
    bool trusted_dispatch_ok = script_runtime_evaluate_diagnostic(
        runtime, trusted_dispatch_check,
        "<trusted-dispatch-check>", &result);
    if (!trusted_dispatch_ok
        || strcmp(result.summary, "TRUSTED-DISPATCH-OK") != 0) {
        fprintf(stderr, "trusted dispatch probe: ok=%d summary=%s error=%s\n",
                trusted_dispatch_ok, result.summary, result.error);
    }
    CHECK(trusted_dispatch_ok
          && strcmp(result.summary, "TRUSTED-DISPATCH-OK") == 0);

    static const char bounded_ancestor_probe[] =
        "(()=>{const first=document.createElement('div'),"
        "second=document.createElement('div');"
        "first.__tilefinchDetachedParent=second;"
        "second.__tilefinchDetachedParent=first;let blocked=false;"
        "try{first.dispatchEvent(new Event('cycle'))}catch(error){"
        "blocked=error instanceof DOMException&&"
        "error.name==='HierarchyRequestError'}"
        "first.__tilefinchDetachedParent=null;"
        "second.__tilefinchDetachedParent=null;"
        "globalThis.pocSummary=blocked?'ANCESTOR-BOUND-OK':"
        "'ANCESTOR-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, bounded_ancestor_probe, "<ancestor-bound-probe>",
              &result)
          && strcmp(result.summary, "ANCESTOR-BOUND-OK") == 0);

    static const char bounded_clone_probe[] =
        "(()=>{const host=document.createElement('div');"
        "host.innerHTML='<i>'.repeat(70)+'leaf'+'</i>'.repeat(70);"
        "let blocked=false;try{host.cloneNode(true)}catch(error){"
        "blocked=error instanceof Error}"
        "globalThis.pocSummary=blocked?'CLONE-BOUND-OK':"
        "'CLONE-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, bounded_clone_probe, "<clone-bound-probe>", &result)
          && strcmp(result.summary, "CLONE-BOUND-OK") == 0);

    static const char clone_control_state_probe[] =
        "(()=>{const original=document.createElement('input');"
        "original.value='original';const clone=original.cloneNode(false);"
        "clone.value='clone';globalThis.pocSummary="
        "original.value==='original'&&clone.value==='clone'"
        "?'CLONE-CONTROL-STATE-OK':'CLONE-CONTROL-STATE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, clone_control_state_probe,
              "<clone-control-state-probe>", &result)
          && strcmp(result.summary, "CLONE-CONTROL-STATE-OK") == 0);

    static const char foreign_template_probe[] =
        "(()=>{const node=document.createElementNS("
        "'http://www.w3.org/2000/svg','template');"
        "const noContent=node.content==null;let setterBounded=false;"
        "try{node.innerHTML='<g></g>';setterBounded=true}"
        "catch(error){setterBounded=error instanceof Error}"
        "globalThis.pocSummary=noContent&&setterBounded"
        "?'FOREIGN-TEMPLATE-OK':'FOREIGN-TEMPLATE-FAILED';})()";
    bool foreign_template_ok = script_runtime_evaluate_diagnostic(
        runtime, foreign_template_probe,
        "<foreign-template-probe>", &result);
    if (!foreign_template_ok
        || strcmp(result.summary, "FOREIGN-TEMPLATE-OK") != 0) {
        fprintf(stderr, "foreign template probe: ok=%d summary=%s error=%s\n",
                foreign_template_ok, result.summary, result.error);
    }
    CHECK(foreign_template_ok
          && strcmp(result.summary, "FOREIGN-TEMPLATE-OK") == 0);

    static const char observer_isolation_probe[] =
        "(()=>{const target=document.createElement('div');"
        "document.body.appendChild(target);let survivor=false;"
        "const throwing=new MutationObserver(()=>{throw new Error('probe')}),"
        "healthy=new MutationObserver(()=>{survivor=true});"
        "throwing.observe(target,{attributes:true});"
        "healthy.observe(target,{attributes:true});"
        "target.setAttribute('data-probe','one');"
        "return Promise.resolve().then(()=>{target.setAttribute("
        "'data-probe','two');return Promise.resolve()}).then(()=>{"
        "throwing.disconnect();healthy.disconnect();target.remove();"
        "globalThis.pocSummary=survivor?'OBSERVER-ISOLATION-OK':"
        "'OBSERVER-ISOLATION-FAILED'})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, observer_isolation_probe,
              "<observer-isolation-probe>", &result)
          && strcmp(result.summary, "OBSERVER-ISOLATION-OK") == 0);

    static const char observer_lifecycle_probe[] =
        "(()=>{const parent=document.createElement('div'),"
        "child=document.createElement('span');parent.appendChild(child);"
        "document.body.appendChild(parent);let records=[];"
        "const mo=new MutationObserver(items=>records.push(...items));"
        "mo.observe(parent,{childList:true,subtree:true,attributes:true});"
        "parent.removeChild(child);child.setAttribute('data-after','yes');"
        "let invalidMargin=false,invalidThreshold=false;"
        "try{new IntersectionObserver(()=>{},{rootMargin:'1em'})}"
        "catch(error){invalidMargin=error instanceof SyntaxError}"
        "try{new IntersectionObserver(()=>{},{threshold:2})}"
        "catch(error){invalidThreshold=error instanceof RangeError}"
        "const io=new IntersectionObserver(()=>{},"
        "{root:document,rootMargin:'10px 5%',threshold:[1,.5,.5,0]});"
        "io.observe(document.body);return Promise.resolve().then(()=>{"
        "const queued=io.takeRecords();return Promise.resolve().then(()=>{"
        "const transient=records.some(record=>record.type==='childList')"
        "&&records.some(record=>record.type==='attributes'"
        "&&record.target===child);const normalized="
        "io.rootMargin==='10px 5% 10px 5%'"
        "&&io.thresholds.join(',')==='0,0.5,1'&&queued.length===1;"
        "mo.disconnect();io.disconnect();parent.remove();"
        "globalThis.pocSummary=transient&&normalized&&invalidMargin"
        "&&invalidThreshold?'OBSERVER-LIFECYCLE-OK':"
        "'OBSERVER-LIFECYCLE-FAILED:'+JSON.stringify({transient,"
        "normalized,invalidMargin,invalidThreshold,records:records.length,"
        "queued:queued.length})})})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, observer_lifecycle_probe,
              "<observer-lifecycle-probe>", &result)
          && strcmp(result.summary, "OBSERVER-LIFECYCLE-OK") == 0);

    static const char observer_old_value_probe[] =
        "(()=>{const parent=document.createElement('div'),child=document."
        "createElement('span');parent.appendChild(child);document.body.appendChild("
        "parent);child.setAttribute('data-value','before');let observed=null,"
        "detachedObserved=false;const "
        "observer=new MutationObserver(records=>{observed=records[0]?.oldValue}),"
        "detached=new MutationObserver(records=>{detachedObserved=records.some("
        "record=>record.type==='attributes'&&record.target===child)});"
        "observer.observe(parent,{subtree:true,attributes:true});observer.observe("
        "child,{attributes:true,attributeOldValue:true});detached.observe(parent,"
        "{subtree:true,attributes:true});parent.removeChild(child);child.setAttribute("
        "'data-value','after');return Promise.resolve().then(()=>{observer."
        "disconnect();detached.disconnect();parent.remove();globalThis.pocSummary="
        "observed==='before'&&detachedObserved?'OBSERVER-OLD-VALUE-OK':"
        "'OBSERVER-OLD-VALUE-FAILED:'+String(observed)+','+detachedObserved})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, observer_old_value_probe,
              "<observer-old-value-probe>", &result)
          && strcmp(result.summary, "OBSERVER-OLD-VALUE-OK") == 0);

    static const char observer_transient_lifecycle_probe[] =
        "(async()=>{const parent=document.createElement('div'),child=document."
        "createElement('div'),grand=document.createElement('span');child.appendChild("
        "grand);parent.appendChild(child);document.body.appendChild(parent);let first="
        "[];const propagated=new MutationObserver(records=>first.push(...records));"
        "propagated.observe(parent,{subtree:true,childList:true,attributes:true});"
        "parent.removeChild(child);child.removeChild(grand);grand.setAttribute("
        "'data-probe','yes');await Promise.resolve();await Promise.resolve();const "
        "propagates=first.filter(record=>record.type==='childList').length===2&&first."
        "some(record=>record.type==='attributes'&&record.target===grand);propagated."
        "disconnect();const secondParent=document.createElement('div'),secondChild="
        "document.createElement('span');secondParent.appendChild(secondChild);document."
        "body.appendChild(secondParent);let callbacks=0,lateRecords=0;const cleared="
        "new MutationObserver(records=>{callbacks++;lateRecords+=records.length;if("
        "callbacks===1)secondChild.setAttribute('data-late','yes')});cleared.observe("
        "secondParent,{subtree:true,childList:true,attributes:true});secondParent."
        "removeChild(secondChild);await Promise.resolve();await Promise.resolve();await "
        "Promise.resolve();const clearsBeforeCallback=callbacks===1&&lateRecords===1;"
        "cleared.disconnect();const thirdParent=document.createElement('div'),"
        "thirdChild=document.createElement('span');thirdParent.appendChild(thirdChild);"
        "document.body.appendChild(thirdParent);let third=[];const replaced=new "
        "MutationObserver(records=>third.push(...records));replaced.observe(thirdParent,"
        "{subtree:true,childList:true,attributes:true});thirdParent.removeChild("
        "thirdChild);replaced.observe(thirdParent,{childList:true});thirdChild."
        "setAttribute('data-after','yes');await Promise.resolve();await Promise.resolve();"
        "const reobserveClears=!third.some(record=>record.type==='attributes');replaced."
        "disconnect();"
        /* Transient registrations end per observer, just before its own
           callback: an earlier observer's change to the removed subtree
           still reaches a later one. */
        "const fourthParent=document.createElement('div'),fourthChild=document."
        "createElement('span');fourthParent.appendChild(fourthChild);document.body."
        "appendChild(fourthParent);const early=[],late=[];let earlyCalls=0;const "
        "earlier=new MutationObserver(records=>{early.push(...records);if(++earlyCalls"
        "===1)fourthChild.setAttribute('data-from-earlier','yes')}),later=new "
        "MutationObserver(records=>late.push(...records));earlier.observe(fourthParent,"
        "{subtree:true,childList:true,attributes:true});later.observe(fourthParent,"
        "{subtree:true,childList:true,attributes:true});fourthParent.removeChild("
        "fourthChild);await Promise.resolve();await Promise.resolve();await Promise."
        "resolve();const perObserver=late.some(record=>record.type==='attributes'&&"
        "record.target===fourthChild)&&!early.some(record=>record.type==='attributes');"
        "earlier.disconnect();later.disconnect();"
        "parent.remove();secondParent.remove();thirdParent.remove();fourthParent.remove();"
        "globalThis.pocSummary=propagates&&clearsBeforeCallback&&reobserveClears&&"
        "perObserver?"
        "'OBSERVER-TRANSIENT-LIFECYCLE-OK':'OBSERVER-TRANSIENT-LIFECYCLE-FAILED:'+"
        "[propagates,callbacks,lateRecords,reobserveClears,perObserver].join(',')})()"
        ".catch(error=>"
        "{globalThis.pocSummary='OBSERVER-TRANSIENT-LIFECYCLE-ERROR:'+String(error&&"
        "error.stack||error)});";
    bool observer_transient_lifecycle_ok = script_runtime_evaluate_diagnostic(
        runtime, observer_transient_lifecycle_probe,
        "<observer-transient-lifecycle-probe>", &result);
    for (size_t tick = 0; observer_transient_lifecycle_ok && tick < 16
         && strcmp(result.summary, "OBSERVER-TRANSIENT-LIFECYCLE-OK") != 0;
         tick++) {
        observer_transient_lifecycle_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    CHECK(observer_transient_lifecycle_ok
          && strcmp(result.summary, "OBSERVER-TRANSIENT-LIFECYCLE-OK") == 0);

    /* Delivery follows the order observers were queued, not registered,
       and one a callback queues waits for the next round. */
    static const char observer_delivery_order_probe[] =
        "(async()=>{const log=[],a=document.createElement('div'),b=document."
        "createElement('div'),c=document.createElement('div');document.body.append("
        "a,b,c);const mo=(name,after)=>new MutationObserver(()=>{log.push(name);"
        "after&&after()});const C=mo('C'),B=mo('B'),A=mo('A',()=>{if(!log.includes("
        "'C'))c.setAttribute('data-from-a','1')});C.observe(c,{attributes:true});"
        "B.observe(b,{attributes:true});A.observe(a,{attributes:true});a."
        "setAttribute('data-x','1');b.setAttribute('data-x','1');for(let i=0;i<4;"
        "i++)await Promise.resolve();A.disconnect();B.disconnect();C.disconnect();"
        "a.remove();b.remove();c.remove();const order=log.join(',');globalThis."
        "pocSummary=order==='A,B,C'?'OBSERVER-ORDER-OK':'OBSERVER-ORDER-FAILED:'+"
        "order})().catch(error=>{globalThis.pocSummary='OBSERVER-ORDER-ERROR:'+"
        "String(error)});";
    bool observer_order_ok = script_runtime_evaluate_diagnostic(
        runtime, observer_delivery_order_probe, "<observer-order-probe>",
        &result);
    for (size_t tick = 0; observer_order_ok && tick < 16
         && strcmp(result.summary, "OBSERVER-ORDER-OK") != 0; tick++)
        observer_order_ok = script_runtime_advance(runtime, 0, 1024, &result);
    if (strcmp(result.summary, "OBSERVER-ORDER-OK") != 0)
        fprintf(stderr, "observer order: %s\n", result.summary);
    CHECK(observer_order_ok
          && strcmp(result.summary, "OBSERVER-ORDER-OK") == 0);

    /* Mutation-observer interest: fast paths (native ancestor and nearest-id
       walks, per-registration interest before any ancestry check) keep the
       records, oldValue merging, transient registrations and the 256-step
       ancestor bound of the wrapper walks; see queueMutationRecords. */
    static const char observer_interest_probe[] =
        "(async()=>{const fails=[],check=(name,ok)=>{if(!ok)fails.push(name)},"
        "flush=async()=>{for(let i=0;i<4;i++)await Promise.resolve()},"
        "el=(tag,id)=>{const n=document.createElement(tag);if(id!==undefined)n."
        "setAttribute('id',id);return n},"
        "line=r=>r.map(x=>[x.type,x.target&&(x.target.id||x.target.nodeName),x."
        "attributeName,x.oldValue,x.addedNodes.length,x.removedNodes.length].jo"
        "in(':')).join('|');"
        "const root=el('div','mi-root'),mid=el('section'),leaf=el('span','mi-le"
        "af'),other=el('div','mi-other');"
        "mid.appendChild(leaf);root.appendChild(mid);document.body.append(root,"
        "other);"
        "const dirty=globalThis.__tilefinchDirtyNodeIds;dirty.clear();"
        "mid.setAttribute('data-a','1');leaf.setAttribute('data-a','1');"
        "check('dirty-nearest',dirty.has('i:mi-root')&&dirty.has('i:mi-leaf')&&"
        "dirty.size===2);"
        "const longId='x'.repeat(129),overlong=el('div',longId),inner=el('i'),e"
        "mpty=el('b','');"
        "overlong.appendChild(inner);mid.append(overlong,empty);const text=docu"
        "ment.createTextNode('a');leaf.appendChild(text);"
        "dirty.clear();inner.setAttribute('data-a','1');empty.setAttribute('dat"
        "a-a','1');"
        "check('dirty-skips',dirty.has('i:mi-root')&&dirty.size===1);"
        "dirty.clear();text.data='b';check('dirty-text',dirty.has('i:mi-leaf')&"
        "&dirty.size===1);"
        "dirty.clear();el('div').setAttribute('data-a','1');check('dirty-none',"
        "dirty.size===0);"
        /* Without a section save nothing drains the set: it keeps the first
           64 keys in insertion order and counts each later add as dropped. */
        "const stats=globalThis.__tilefinchRetentionStats,drops=stats.dirtyDro"
        "ps,bound=[];"
        "for(let i=0;i<70;i++){const n=el('i','mi-bound-'+i);bound.push(n);n.s"
        "etAttribute('data-a','1')}"
        "check('dirty-bound',dirty.size===64&&dirty.has('i:mi-bound-0')&&dirty"
        ".has('i:mi-bound-63')&&!dirty.has('i:mi-bound-64')&&[...dirty][0]==='"
        "i:mi-bound-0'&&stats.dirtyDrops===drops+12);"
        "const boundDrops=stats.dirtyDrops;bound[5].setAttribute('data-a','2');"
        "check('dirty-bound-known',dirty.size===64&&stats.dirtyDrops===boundDr"
        "ops);dirty.clear();bound[69].setAttribute('data-a','2');"
        "check('dirty-bound-cleared',dirty.size===1&&dirty.has('i:mi-bound-69'"
        "));dirty.clear();"
        "const unrelatedLog=[],unrelated=new MutationObserver(r=>unrelatedLog.p"
        "ush(...r));"
        "unrelated.observe(other,{attributes:true,childList:true,characterData:"
        "true,subtree:true});"
        "leaf.setAttribute('data-b','1');mid.appendChild(el('u'));text.data='c'"
        ";await flush();"
        "check('unrelated',unrelatedLog.length===0);unrelated.disconnect();"
        "const got=[],filtered=new MutationObserver(r=>got.push(...r));"
        "filtered.observe(root,{attributes:true,subtree:true,attributeFilter:['"
        "data-c']});"
        "filtered.observe(leaf,{attributes:true,attributeOldValue:true});"
        "leaf.setAttribute('data-c','old');leaf.setAttribute('data-c','new');le"
        "af.setAttribute('data-d','x');"
        "mid.setAttribute('data-c','m');mid.setAttribute('data-d','m');root.set"
        "Attribute('data-c','r');"
        "other.setAttribute('data-c','o');await flush();filtered.disconnect();"
        "check('filter-old-value',line(got)==='attributes:mi-leaf:data-c::0:0|a"
        "ttributes:mi-leaf:data-c:old:0:0|'+"
        "'attributes:mi-leaf:data-d::0:0|attributes:SECTION:data-c::0:0|attribu"
        "tes:mi-root:data-c::0:0');"
        "const listLog=[],list=new MutationObserver(r=>listLog.push(...r));list"
        ".observe(root,{childList:true,subtree:true});"
        "mid.setAttribute('data-f','1');text.data='d';mid.appendChild(el('em'))"
        ";await flush();list.disconnect();"
        "check('child-list-only',line(listLog)==='childList:SECTION:::1:0');"
        /* Re-observing a target replaces its options, and with them the
           record types the observer is asked about. */
        "const swapLog=[],swap=new MutationObserver(r=>swapLog.push(...r));"
        "swap.observe(mid,{attributes:true});swap.observe(mid,{childList:true}"
        ");mid.setAttribute('data-s','1');mid.appendChild(el('b'));"
        "await flush();swap.disconnect();swap.observe(mid,{characterData:true,"
        "subtree:true});mid.setAttribute('data-s','2');text.data='s';"
        "await flush();swap.disconnect();"
        "check('reobserve-types',line(swapLog)==='childList:SECTION:::1:0|"
        "characterData:#text:::0:0');"
        "const attrLog=[],attrs=new MutationObserver(r=>attrLog.push(...r));att"
        "rs.observe(root,{attributes:true,subtree:true});"
        "const moved=el('div','mi-moved'),movedChild=el('i');moved.appendChild("
        "movedChild);mid.appendChild(moved);"
        "mid.removeChild(leaf);leaf.setAttribute('data-e','1');mid.removeChild("
        "moved);other.appendChild(moved);"
        "movedChild.setAttribute('data-e','2');await flush();"
        "check('transient',line(attrLog)==='attributes:mi-leaf:data-e::0:0|attr"
        "ibutes:I:data-e::0:0');"
        "leaf.setAttribute('data-e','3');movedChild.setAttribute('data-e','4');"
        "await flush();attrs.disconnect();"
        "check('transient-ends',attrLog.length===2);"
        "const batch=[],late=new MutationObserver(r=>batch.push(...r));"
        "mid.setAttribute('data-g','1');late.observe(root,{attributes:true,subt"
        "ree:true});mid.setAttribute('data-g','2');"
        "late.disconnect();mid.setAttribute('data-g','3');late.observe(root,{at"
        "tributes:true,subtree:true,attributeOldValue:true});"
        "mid.setAttribute('data-g','4');await flush();late.disconnect();"
        "check('mid-batch',line(batch)==='attributes:SECTION:data-g:3:0:0');"
        "const docLog=[],doc=new MutationObserver(r=>docLog.push(...r));"
        "doc.observe(document,{attributes:true,subtree:true,attributeFilter:['d"
        "ata-h']});"
        "mid.setAttribute('data-h','1');const loose=el('div'),looseChild=el('sp"
        "an');loose.appendChild(looseChild);"
        "looseChild.setAttribute('data-h','1');await flush();doc.disconnect();"
        "check('document',line(docLog)==='attributes:SECTION:data-h::0:0');"
        "const chain=[el('div')];for(let i=0;i<260;i++){const c=el('div');c.app"
        "endChild(chain[0]);chain.unshift(c)}"
        "const deep=[],bounded=new MutationObserver(r=>deep.push(...r));bounded"
        ".observe(chain[0],{attributes:true,subtree:true});"
        "chain[255].setAttribute('data-i','1');chain[256].setAttribute('data-i'"
        ",'1');chain[257].setAttribute('data-i','1');"
        "await flush();bounded.disconnect();"
        "check('ancestor-bound',deep.map(r=>chain.indexOf(r.target)).join(',')="
        "=='255,256');"
        "const dataLog=[],data=new MutationObserver(r=>dataLog.push(...r));"
        "data.observe(root,{characterData:true,characterDataOldValue:true,subtr"
        "ee:true});"
        "const midText=document.createTextNode('a');mid.appendChild(midText);mi"
        "dText.data='b';await flush();data.disconnect();"
        "check('character-data',dataLog.length===1&&dataLog[0].oldValue==='a'&&"
        "dataLog[0].target===midText);"
        /* Replacing one registration must recompute aggregate interest,
           retain other target kinds, and drop that registration's detached
           transient roots. Disconnect/reobserve starts with fresh interest. */
        "const kinds=[],changing=new MutationObserver(r=>kinds.push(...r));"
        "changing.observe(root,{attributes:true,subtree:true});"
        "changing.observe(other,{childList:true});"
        "const removed=el('div');mid.appendChild(removed);removed.remove();"
        "changing.observe(root,{characterData:true,subtree:true});"
        "removed.setAttribute('data-k','1');mid.setAttribute('data-k','1');"
        "midText.data='c';other.appendChild(el('b'));await flush();"
        "check('replace-interest',kinds.length===2&&kinds[0].type==='character"
        "Data'&&kinds[1].type==='childList');changing.disconnect();kinds.length=0;"
        "changing.observe(root,{attributes:true,subtree:true});midText.data='d';"
        "mid.setAttribute('data-k','2');await flush();changing.disconnect();"
        "check('reobserve-interest',kinds.length===1&&kinds[0].type==='attributes');"
        "const fragment=document.createDocumentFragment(),fragmentChild=el('div"
        "');fragment.appendChild(fragmentChild);"
        "const fragmentLog=[],fragmentObserver=new MutationObserver(r=>fragment"
        "Log.push(...r));"
        "fragmentObserver.observe(fragment,{attributes:true,subtree:true});frag"
        "mentChild.setAttribute('data-j','1');"
        "await flush();fragmentObserver.disconnect();"
        "check('fragment',fragmentLog.length===1&&fragmentLog[0].target===fragm"
        "entChild);"
        "root.remove();other.remove();"
        "globalThis.pocSummary=fails.length?'OBSERVER-INTEREST-FAILED:'+fails.j"
        "oin(','):'OBSERVER-INTEREST-OK'})()"
        ".catch(error=>{globalThis.pocSummary='OBSERVER-INTEREST-ERROR:'+String"
        "(error)+String(error&&error.stack)});";
    bool observer_interest_ok = script_runtime_evaluate_diagnostic(
        runtime, observer_interest_probe, "<observer-interest-probe>",
        &result);
    for (size_t tick = 0; observer_interest_ok && tick < 16
         && strcmp(result.summary, "OBSERVER-INTEREST-OK") != 0; tick++)
        observer_interest_ok = script_runtime_advance(runtime, 0, 1024,
                                                      &result);
    if (strcmp(result.summary, "OBSERVER-INTEREST-OK") != 0)
        fprintf(stderr, "observer interest: %s\n", result.summary);
    CHECK(observer_interest_ok
          && strcmp(result.summary, "OBSERVER-INTEREST-OK") == 0);

    /* The page's own script, not whichever element a probe last inserted
       ahead of it: the completions below dispatch load and error to it. */
    lxb_dom_node_t *script = find_element_id(
        lxb_dom_interface_node(document.html), "target");
    CHECK(script != NULL && script == find_script(
              lxb_dom_interface_node(document.html)));
    size_t oversized_length =
        strict.maximum_host_compile_source_bytes + 1;
    char *oversized = malloc(oversized_length + 1);
    CHECK(oversized != NULL);
    memset(oversized, ';', oversized_length);
    oversized[oversized_length] = '\0';
    size_t attempts_before = result.host_compile_attempts;
    size_t compiled_before = result.host_compile_source_bytes;
    CHECK(!script_runtime_evaluate_external(
              runtime, script, oversized, oversized_length,
              "https://example.test/oversized.js", &result)
          && result.success
          && result.host_compile_attempts == attempts_before + 1
          && result.host_compile_rejections == 1
          && result.host_compile_rejected_source_bytes == oversized_length
          && result.host_compile_source_bytes == compiled_before
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_REJECTED_SOURCE_LIMIT
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_EXTERNAL
          && result.last_compile_source_bytes == oversized_length
          && strcmp(result.last_compile_source_name,
                    "https://example.test/oversized.js") == 0
          && result.max_nonpreemptible_compile_bytes < oversized_length
          && result.external_scripts_failed == 1
          && result.external_scripts_loaded == 0);
    free(oversized);

    static const char diagnostic[] =
        "globalThis.pocSummary='RESPONSIVE-AFTER-REJECTION'";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, diagnostic, "<post-rejection>", &result)
          && result.success
          && strcmp(result.summary, "RESPONSIVE-AFTER-REJECTION") == 0
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_DIAGNOSTIC
          && result.host_compile_rejections == 1
          && result.host_callback_calls != 0
          && result.nonpreemptible_compile_count == 0
          && result.nonpreemptible_callback_count == 0);

#if defined(PSP_BROWSER_BELLARD_QUICKJS)
    puts("test: watchdog aborts an admitted compile through parser polls");
    size_t abort_source_length = 48u * 1024u;
    char *abort_source = malloc(abort_source_length + 1);
    CHECK(abort_source != NULL);
    for (size_t at = 0; at < abort_source_length; at += 4) {
        memcpy(abort_source + at, "a=1;", 4);
    }
    abort_source[abort_source_length] = '\0';
    CompileAbortCooperate abort_cooperate = {0};
    TilefinchPlatformServices abort_services = {
        .context = &abort_cooperate,
        .cooperate = compile_abort_cooperate
    };
    tilefinch_platform_set_services(&abort_services);
    size_t aborts_before = result.host_compile_watchdog_aborts;
    CHECK(!script_runtime_evaluate_diagnostic(
              runtime, abort_source, "<compile-abort>", &result)
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ABORTED_WATCHDOG
          && result.host_compile_watchdog_aborts == aborts_before + 1
          && result.host_compile_watchdog_aborted_bytes
               >= abort_source_length
          && abort_cooperate.calls != 0
          && strstr(result.error, "compilation interrupted") != NULL);
    tilefinch_platform_set_services(NULL);
    free(abort_source);
    /* The realm stays healthy after a truthful compile abort. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='RESPONSIVE-AFTER-ABORT'",
              "<post-abort>", &result)
          && result.success
          && strcmp(result.summary, "RESPONSIVE-AFTER-ABORT") == 0
          && result.last_compile_admission
               == SCRIPT_COMPILE_ADMISSION_ACCEPTED);
#endif

    puts("test: null external script completion is successful");
    size_t loaded_before_null = result.external_scripts_loaded;
    size_t failed_before_null = result.external_scripts_failed;
    static const char null_completion[] = "null";
    CHECK(script_runtime_evaluate_external(
              runtime, script, null_completion,
              sizeof(null_completion) - 1,
              "https://example.test/null-completion.js", &result)
          && result.success
          && result.external_scripts_loaded == loaded_before_null + 1
          && result.external_scripts_failed == failed_before_null);

    /* Collection decides when dead wrappers' FinalizationRegistry cleanups
       join the job queue, and one checkpoint runs at most 64 jobs, so a
       backlog left by earlier probes could otherwise outlast this probe's
       own checkpoint. Collect now and settle the queue first. */
    (void) script_runtime_collect_and_trim(runtime);
    for (size_t tick = 0;
         tick < 64 && js_rt_runtime_checkpoint_pending(runtime); tick++) {
        CHECK(js_rt_runtime_run_jobs(runtime));
    }
    CHECK(!js_rt_runtime_checkpoint_pending(runtime));
    js_rt_runtime_update_result(runtime, &result);
    size_t callbacks_before_promise = result.host_callback_calls;
    static const char promise_job[] =
        "Promise.resolve().then(()=>{"
        "globalThis.pocSummary='PROMISE-JOB-OBSERVED'})";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, promise_job, "<promise-job-probe>", &result)
          && strcmp(result.summary, "PROMISE-JOB-OBSERVED") == 0
          && result.host_callback_calls > callbacks_before_promise);

    puts("test: promise rejection lifecycle telemetry distinguishes undefined");
    size_t rejections_created_before = result.promise_rejections_created;
    size_t rejections_handled_before = result.promise_rejections_handled;
    size_t rejections_undefined_before = result.promise_rejections_undefined;
    static const char rejection_probe[] =
        "globalThis.__tilefinchUnhandledProbe=Promise.reject(undefined);"
        "globalThis.__tilefinchHandledProbe=Promise.reject(new Error('handled'));"
        "globalThis.__tilefinchHandledProbe.catch(()=>{});";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, rejection_probe, "<promise-rejection-probe>", &result)
          && result.promise_rejections == 1
          && result.promise_rejections_created
               == rejections_created_before + 2
          && result.promise_rejections_handled
               == rejections_handled_before + 1
          && result.promise_rejections_undefined
               == rejections_undefined_before + 1);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "__tilefinchUnhandledProbe.catch(()=>{})",
              "<promise-rejection-handle>", &result)
          && result.promise_rejections == 0
          && result.promise_rejections_handled
               == rejections_handled_before + 2);

    static const char webcrypto_digest_probe[] =
        "(()=>{let synchronous=false,arrayPromise,typedPromise,viewPromise,"
        "unsupportedPromise,typePromise,brandPromise,symbolPromise,quotaPromise;try{"
        "const expected='ba7816bf8f01cfea414140de5dae2223'"
        "+'b00361a396177a9cb410ff61f20015ad';"
        "const whole=new Uint8Array([97,98,99]);"
        "const framed=new Uint8Array([0,97,98,99,255]);"
        "arrayPromise=crypto.subtle.digest('SHA-256',whole.buffer);"
        "typedPromise=crypto.subtle.digest('sha-256',framed.subarray(1,4));"
        "viewPromise=crypto.subtle.digest({name:'SHA-256'},"
        "new DataView(framed.buffer,1,3));"
        "unsupportedPromise=crypto.subtle.digest('SHA-1',whole);"
        "typePromise=crypto.subtle.digest('SHA-256','abc');"
        "brandPromise=SubtleCrypto.prototype.digest.call({},'SHA-256',whole);"
        "symbolPromise=crypto.subtle.digest({name:Symbol()},whole);"
        "quotaPromise=crypto.subtle.digest('SHA-256',"
        "new Uint8Array(1024*1024+1));"
        "const hex=value=>[...new Uint8Array(value)]"
        ".map(byte=>byte.toString(16).padStart(2,'0')).join('');"
        "Promise.all([arrayPromise.then(value=>value instanceof ArrayBuffer"
        "&&hex(value)===expected),typedPromise.then(value=>hex(value)===expected),"
        "viewPromise.then(value=>hex(value)===expected),"
        "unsupportedPromise.then(()=>false,error=>error.name==='NotSupportedError'),"
        "typePromise.then(()=>false,error=>error.name==='TypeError'),"
        "brandPromise.then(()=>false,error=>error.name==='TypeError'),"
        "symbolPromise.then(()=>false,error=>error.name==='TypeError'),"
        "quotaPromise.then(()=>false,error=>error.name==='RangeError')])"
        ".then(checks=>{globalThis.pocSummary=!synchronous"
        "&&arrayPromise instanceof Promise&&checks.every(Boolean)"
        "?'WEBCRYPTO-DIGEST-OK':'WEBCRYPTO-DIGEST-FAILED'});"
        "}catch(error){synchronous=true;globalThis.pocSummary="
        "'WEBCRYPTO-DIGEST-SYNC-THROW:'+error;}})();";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webcrypto_digest_probe, "<webcrypto-digest-probe>",
              &result)
          && strcmp(result.summary, "WEBCRYPTO-DIGEST-OK") == 0);

    static const char image_factory_probe[] =
        "(()=>{const image=new Image(37,19),called=Image(11,7);"
        "globalThis.pocSummary=typeof Image==='function'"
        "&&Image.prototype===HTMLImageElement.prototype"
        "&&image instanceof Image&&image instanceof HTMLImageElement"
        "&&image.tagName==='IMG'&&image.getAttribute('width')==='37'"
        "&&image.getAttribute('height')==='19'&&called instanceof Image"
        "&&called.getAttribute('width')==='11'"
        "&&called.getAttribute('height')==='7'"
        "?'IMAGE-FACTORY-OK':'IMAGE-FACTORY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, image_factory_probe, "<image-factory-probe>", &result)
          && strcmp(result.summary, "IMAGE-FACTORY-OK") == 0);

    puts("test: page visibility pauses visual tasks and preserves edges");
    static const char visibility_setup[] =
        "globalThis.__visibilityEdges=[];globalThis.__hiddenRaf=0;"
        "globalThis.__visibilityHandlerEdges=[];globalThis.__hiddenTimer=0;"
        "let illegal=false;"
        "try{new VisibilityStateEntry()}catch(error){illegal="
        "error instanceof TypeError}const initialVisibilityEntries="
        "performance.getEntriesByType('visibility-state');"
        "globalThis.__visibilityPerformanceInitial=illegal&&"
        "initialVisibilityEntries.length===1&&"
        "initialVisibilityEntries[0] instanceof VisibilityStateEntry&&"
        "initialVisibilityEntries[0] instanceof PerformanceEntry&&"
        "Object.prototype.toString.call(initialVisibilityEntries[0])==="
        "'[object VisibilityStateEntry]'&&"
        "Object.getOwnPropertyNames(initialVisibilityEntries[0]).length===0&&"
        "initialVisibilityEntries[0].name==='visible'&&"
        "initialVisibilityEntries[0].entryType==='visibility-state'&&"
        "initialVisibilityEntries[0].startTime===0&&"
        "initialVisibilityEntries[0].duration===0&&"
        "PerformanceObserver.supportedEntryTypes.includes('visibility-state');"
        "const visibilityObserver=new PerformanceObserver(()=>{});"
        "visibilityObserver.observe({type:'visibility-state',buffered:true});"
        "globalThis.__visibilityPerformanceInitial="
        "__visibilityPerformanceInitial&&"
        "visibilityObserver.takeRecords().map(entry=>entry.name)"
        ".join(',')==='visible';visibilityObserver.disconnect();"
        "if(typeof __tilefinchPageVisible!=='undefined'||"
        "typeof __tilefinchApplyPageVisibility!=='undefined')"
        "throw new Error('visibility host bridge leaked');"
        "document.onvisibilitychange=()=>__visibilityHandlerEdges.push("
        "document.visibilityState);document.addEventListener("
        "'visibilitychange',()=>__visibilityEdges.push("
        "document.visibilityState));requestAnimationFrame(()=>__hiddenRaf++);"
        "setTimeout(()=>__hiddenTimer++,0);";
    /* This runtime uses the PSP strict policy, whose 16 ms advance target
       lets a turn start a second task only while wall time remains. Each
       advance below must run the visibility edge and then the due timer or
       frame callback in the same turn; on a loaded host the edge alone could
       use the 16 ms and push the callback to a later turn. The yield target
       is not what these checks are about, so lift it for this block. */
    uint64_t visibility_advance_target_us =
        runtime->bridge.execution_policy.maximum_advance_time_us;
    runtime->bridge.execution_policy.maximum_advance_time_us = 0;
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, visibility_setup, "<visibility-setup>", &result));
    CHECK(script_runtime_set_page_visibility(runtime, false));
    CHECK(script_runtime_advance(runtime, 16, 16, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=[document.hidden,"
        "document.visibilityState==='hidden',"
        "__visibilityEdges.join(',')==='hidden',"
        "__visibilityHandlerEdges.join(',')==='hidden',"
        "__visibilityPerformanceInitial,"
        "performance.getEntriesByType('visibility-state').map("
        "entry=>entry.name).join(',')==='visible,hidden',"
        "__hiddenTimer===1,__hiddenRaf===0].map(Number).join('')",
        "<visibility-hidden>", &result));
    if (strcmp(result.summary, "11111111") != 0)
        fprintf(stderr, "visibility hidden state: %s\n", result.summary);
    CHECK(strcmp(result.summary, "11111111") == 0);
    CHECK(script_runtime_set_page_visibility(runtime, true)
          && script_runtime_advance(runtime, 16, 16, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=!document.hidden&&"
                 "document.visibilityState==='visible'&&"
                 "__visibilityEdges.join(',')==='hidden,visible'&&"
                 "__visibilityHandlerEdges.join(',')==='hidden,visible'&&"
                 "__hiddenRaf===1"
                 "?'VISIBILITY-RESTORED-OK':'VISIBILITY-RESTORE-FAILED'",
                 "<visibility-restored>", &result)
          && strcmp(result.summary, "VISIBILITY-RESTORED-OK") == 0);
    CHECK(script_runtime_set_page_visibility(runtime, false)
          && script_runtime_set_page_visibility(runtime, true)
          && script_runtime_advance(runtime, 0, 1, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=document.hidden&&"
                 "__visibilityEdges.slice(-1)[0]==='hidden'"
                 "?'VISIBILITY-QUEUED-HIDDEN-OK':"
                 "'VISIBILITY-QUEUED-HIDDEN-FAILED'",
                 "<visibility-queued-hidden>", &result)
          && strcmp(result.summary, "VISIBILITY-QUEUED-HIDDEN-OK") == 0
          && script_runtime_advance(runtime, 0, 1, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=!document.hidden&&"
                 "__visibilityEdges.slice(-2).join(',')==='hidden,visible'"
                 "?'VISIBILITY-QUEUED-VISIBLE-OK':"
                 "'VISIBILITY-QUEUED-VISIBLE-FAILED'",
                 "<visibility-queued-visible>", &result)
          && strcmp(result.summary, "VISIBILITY-QUEUED-VISIBLE-OK") == 0);
    runtime->bridge.execution_policy.maximum_advance_time_us =
        visibility_advance_target_us;

    puts("test: page fullscreen requires native user activation");
    static const char fullscreen_setup[] =
        "(()=>{const target=document.createElement('div');"
        "target.id='fullscreen-target';document.body.append(target);"
        "globalThis.__fullscreenChanges=0;"
        "document.addEventListener('fullscreenchange',()=>"
        "globalThis.__fullscreenChanges++);"
        "target.requestFullscreen().then(()=>"
        "globalThis.pocSummary='FULLSCREEN-DIRECT-BAD',error=>"
        "globalThis.pocSummary=error.name==='NotAllowedError'"
        "?'FULLSCREEN-DIRECT-BLOCKED':'FULLSCREEN-DIRECT-WRONG');"
        "target.addEventListener('click',()=>target.requestFullscreen().then("
        "()=>globalThis.pocSummary=document.fullscreenElement===target"
        "?'FULLSCREEN-ENTERED':'FULLSCREEN-ENTER-FAILED'));})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, fullscreen_setup, "<fullscreen-setup>", &result)
          && strcmp(result.summary, "FULLSCREEN-DIRECT-BLOCKED") == 0);
    lxb_dom_node_t *fullscreen_target = find_element_id(
        lxb_dom_interface_node(document.html), "fullscreen-target");
    CHECK(fullscreen_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, fullscreen_target, &result)
          && strcmp(result.summary, "FULLSCREEN-ENTERED") == 0
          && script_runtime_page_fullscreen_active(runtime));
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "document.exitFullscreen().then(()=>globalThis.pocSummary="
              "document.fullscreenElement===null&&__fullscreenChanges===2"
              "?'FULLSCREEN-EXITED':'FULLSCREEN-EXIT-FAILED')",
              "<fullscreen-exit>", &result)
          && strcmp(result.summary, "FULLSCREEN-EXITED") == 0
          && !script_runtime_page_fullscreen_active(runtime));
    /* Entry and exit queue the same bounded render-fixup a real frame would
       service. Drain it before the later raw timer-cap probe. */
    CHECK(script_runtime_advance(runtime, 16, 16, &result));

    puts("test: bounded game audio decodes and starts under user activation");
    static const char game_audio_setup[] =
        "(()=>{const bytes=new Uint8Array(52),view=new DataView(bytes.buffer),"
        "text=(at,value)=>{for(let i=0;i<value.length;i++)bytes[at+i]="
        "value.charCodeAt(i)},u16=(at,value)=>view.setUint16(at,value,true),"
        "u32=(at,value)=>view.setUint32(at,value,true);text(0,'RIFF');"
        "u32(4,44);text(8,'WAVE');text(12,'fmt ');u32(16,16);u16(20,1);"
        "u16(22,1);u32(24,44100);u32(28,88200);u16(32,2);u16(34,16);"
        "text(36,'data');u32(40,8);view.setInt16(44,1000,true);"
        "view.setInt16(46,-1000,true);view.setInt16(48,2000,true);"
        "view.setInt16(50,-2000,true);const context=new AudioContext(),"
        "target=document.createElement('button');target.id='game-audio-target';"
        "document.body.append(target);context.resume().then(()=>"
        "globalThis.pocSummary='GAME-AUDIO-DIRECT-BAD',error=>"
        "globalThis.pocSummary=error.name==='NotAllowedError'"
        "?'GAME-AUDIO-DIRECT-BLOCKED':'GAME-AUDIO-DIRECT-WRONG');"
        "context.decodeAudioData(bytes.buffer).then(buffer=>{"
        "globalThis.__gameAudioBuffer=buffer;target.addEventListener('click',"
        "()=>{context.resume().then(()=>{const source=context.createBufferSource();"
        "source.buffer=buffer;source.connect(context.destination);source.start();"
        "globalThis.__gameAudioSource=source;globalThis.__gameAudioEnded=[];"
        "source.addEventListener('ended',()=>__gameAudioEnded.push('listener'));"
        "source.onended=()=>__gameAudioEnded.push('handler');"
        "globalThis.pocSummary=context.state==='running'&&buffer.length===4"
        "?'GAME-AUDIO-STARTED':'GAME-AUDIO-START-FAILED'})})})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_setup, "<game-audio-setup>", &result)
          && strcmp(result.summary, "GAME-AUDIO-DIRECT-BLOCKED") == 0);
    lxb_dom_node_t *game_audio_target = find_element_id(
        lxb_dom_interface_node(document.html), "game-audio-target");
    CHECK(game_audio_target != NULL
          && script_runtime_dispatch_activation_node(
                 runtime, game_audio_target, &result)
          && strcmp(result.summary, "GAME-AUDIO-STARTED") == 0);
    int16_t game_audio_samples[8] = {0};
    CHECK(runtime->game_audio != NULL
          && tilefinch_game_audio_mix(runtime->game_audio,
                                      game_audio_samples, 4)
          && game_audio_samples[0] == 1000
          && game_audio_samples[2] == -1000);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=__gameAudioSource._voice===0&&"
                 "__gameAudioEnded.includes('listener')&&"
                 "__gameAudioEnded.includes('handler')&&"
                 "__gameAudioEnded.length===2"
                 "?'GAME-AUDIO-ENDED-OK':'GAME-AUDIO-ENDED-FAILED'",
                 "<game-audio-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-ENDED-OK") == 0);
    puts("test: game audio supports bounded synthesis, panning, and schedules");
    static const char game_audio_synthesis_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,gain="
        "context.createGain(),pan=context.createStereoPanner(),"
        "osc=context.createOscillator();gain.gain.value=.5;pan.pan.value=1;"
        "osc.frequency.value=11025;osc.connect(gain).connect(pan).connect("
        "context.destination);globalThis.__gameOscEnded=0;"
        "globalThis.__gameSynthGain=gain;globalThis.__gameSynthPan=pan;"
        "osc.onended=()=>__gameOscEnded++;const now=context.currentTime;"
        "osc.start(now+.005);osc.stop(now+.008);"
        "globalThis.pocSummary=osc.type==='sine'&&pan.pan.value===1"
        "?'GAME-AUDIO-SYNTHESIS-STARTED':"
        "'GAME-AUDIO-SYNTHESIS-FAILED'})()";
    CHECK(evaluate_with_held_clock(
              runtime, game_audio_synthesis_probe,
              "<game-audio-synthesis>", &result)
          && strcmp(result.summary, "GAME-AUDIO-SYNTHESIS-STARTED") == 0);
    int16_t game_audio_synthesis_samples[800];
    memset(game_audio_synthesis_samples, 0x55,
           sizeof(game_audio_synthesis_samples));
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
                                  game_audio_synthesis_samples, 400));
    bool oscillator_silent_before_start = true;
    bool oscillator_right_channel_played = false;
    for (size_t frame = 0; frame < 400; frame++) {
        if (game_audio_synthesis_samples[frame * 2u] != 0)
            oscillator_silent_before_start = false;
        if (frame < 180u
            && game_audio_synthesis_samples[frame * 2u + 1u] != 0)
            oscillator_silent_before_start = false;
        if (frame >= 180u
            && game_audio_synthesis_samples[frame * 2u + 1u] != 0)
            oscillator_right_channel_played = true;
    }
    CHECK(oscillator_silent_before_start && oscillator_right_channel_played);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "globalThis.pocSummary=__gameOscEnded===1"
                 "?'GAME-AUDIO-SYNTHESIS-ENDED':"
                 "'GAME-AUDIO-SYNTHESIS-END-FAILED'",
                 "<game-audio-synthesis-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-SYNTHESIS-ENDED") == 0);
    puts("test: game-audio gain envelopes advance without JavaScript ticks");
    static const char game_audio_envelope_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,gain=__gameSynthGain,"
        "osc=context.createOscillator();gain.gain.value=0;"
        "osc.frequency.value=440;osc.connect(gain).connect(context.destination);"
        "osc.start();const now=context.currentTime;gain.gain.setValueAtTime(0,now);"
        "const dispatch=context._forSourcesThrough;"
        "context._forSourcesThrough=()=>{throw Error('automation callback dispatch')};"
        "try{gain.gain.cancelScheduledValues(now);"
        "gain.gain.setTargetAtTime(.5,now,.002);"
        "gain.gain.setTargetAtTime(0,now+.02,.004)}"
        "finally{context._forSourcesThrough=dispatch}"
        "globalThis.__gameEnvelopeOsc=osc;globalThis.pocSummary="
        "typeof gain.gain.setTargetAtTime==='function'"
        "?'GAME-AUDIO-ENVELOPE-SCHEDULED':'GAME-AUDIO-ENVELOPE-MISSING'})()";
    CHECK(evaluate_with_held_clock(
              runtime, game_audio_envelope_probe,
              "<game-audio-envelope>", &result)
          && strcmp(result.summary, "GAME-AUDIO-ENVELOPE-SCHEDULED") == 0);
    int envelope_peak = 0, envelope_tail = 0;
    int16_t game_audio_envelope_samples[
        TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    for (size_t block = 0; block < 8; block++) {
        CHECK(tilefinch_game_audio_mix(
            runtime->game_audio, game_audio_envelope_samples,
            TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
        for (size_t sample = 0;
             sample < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; sample++) {
            int magnitude = game_audio_envelope_samples[sample] < 0
                ? -(int) game_audio_envelope_samples[sample]
                : (int) game_audio_envelope_samples[sample];
            if (magnitude > envelope_peak) envelope_peak = magnitude;
        }
        if (block == 7u) {
            for (size_t sample =
                     (TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES - 32u) * 2u;
                 sample < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; sample++) {
                int magnitude = game_audio_envelope_samples[sample] < 0
                    ? -(int) game_audio_envelope_samples[sample]
                    : (int) game_audio_envelope_samples[sample];
                if (magnitude > envelope_tail) envelope_tail = magnitude;
            }
        }
    }
    CHECK(envelope_peak > 4000 && envelope_tail < 128);
    puts("test: copied gain and pitch curves progress without page advances");
    static const char game_audio_curve_probe[] =
        "(()=>{const c=__gameAudioBuffer._context,g=__gameSynthGain,"
        "o=__gameEnvelopeOsc;o.frequency.value=220;"
        "g.gain.cancelScheduledValues(0);g.gain.value=0;"
        "const now=c.currentTime,"
        "gain=new Float32Array([0,.5,0]),pitch=new Float32Array([220,880]);"
        "g.gain.setValueCurveAtTime(gain,now,.02);"
        "o.frequency.setValueCurveAtTime(pitch,now,.02);"
        "gain.fill(0);pitch.fill(1);globalThis.__gameCurveOsc=o;"
        "let checks=0;const refuses=(fn,name)=>{try{fn()}catch(e){"
        "if(e.name===name)checks++}};"
        "refuses(()=>g.gain.setValueCurveAtTime([0,1],now,.01),'NotSupportedError');"
        "refuses(()=>g.gain.setValueCurveAtTime([0],now,.01),'InvalidStateError');"
        "refuses(()=>g.gain.setValueCurveAtTime([0,NaN],now,.01),'TypeError');"
        "refuses(()=>g.gain.setValueCurveAtTime([0,1],now,0),'RangeError');"
        "refuses(()=>g.gain.setValueCurveAtTime(new Float32Array(65),now,.01),"
        "'QuotaExceededError');"
        "refuses(()=>o.frequency.setValueCurveAtTime([220,880],now,11),"
        "'NotSupportedError');"
        "const cmd=__tilefinchGameAudioCommand,marker={};"
        "if(cmd(11,o._voice,1,new Float64Array([220,880]),1,1,0,.01)===false)checks++;"
        "refuses(()=>cmd(11,o._voice,1,new Float32Array([NaN,880]),1,1,0,.01),'TypeError');"
        "try{cmd(11,o._voice,1,pitch,1,1,{valueOf(){throw marker}},.01)}"
        "catch(e){if(e===marker)checks++}"
        "const detached=new Float32Array([220,880]);"
        "refuses(()=>cmd(11,o._voice,1,detached,1,1,{valueOf(){"
        "detached.buffer.transfer();return 0}},.01),'TypeError');"
        "globalThis.pocSummary=checks===10?'GAME-AUDIO-CURVES-SCHEDULED':'CURVE-BOUNDS-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_curve_probe, "<game-audio-curves>", &result)
          && strcmp(result.summary, "GAME-AUDIO-CURVES-SCHEDULED") == 0);
    int curve_peak = 0, curve_tail = 0;
    size_t curve_early_edges = 0, curve_late_edges = 0;
    int16_t curve_previous = 0;
    for (size_t block = 0; block < 4u; block++) {
        CHECK(tilefinch_game_audio_mix(runtime->game_audio,
            game_audio_envelope_samples, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
        for (size_t frame = 0; frame < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES; frame++) {
            int sample = game_audio_envelope_samples[frame * 2u];
            int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > curve_peak) curve_peak = magnitude;
            if (block == 3u && magnitude > curve_tail) curve_tail = magnitude;
            if (sample != 0 && curve_previous != 0
                && (sample < 0) != (curve_previous < 0)) {
                if (block == 0u && frame < 256u) curve_early_edges++;
                if (block == 1u && frame < 256u) curve_late_edges++;
            }
            curve_previous = (int16_t) sample;
        }
    }
    CHECK(curve_peak > 7000 && curve_tail == 0
          && curve_late_edges > curve_early_edges);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
              "(()=>{const o=__gameCurveOsc,g=__gameSynthGain;"
              "o.frequency.cancelScheduledValues(0);g.gain.cancelScheduledValues(0);"
              "g.gain.value=.25;o.frequency.value=220;"
              "g.gain.setValueCurveAtTime([.25,.25],0,.005);"
              "o.frequency.setValueCurveAtTime([220,880],0,.005);"
              "globalThis.pocSummary='GAME-AUDIO-CURVES-CANCELLED'})()",
              "<game-audio-curve-cancel>", &result)
          && strcmp(result.summary, "GAME-AUDIO-CURVES-CANCELLED") == 0);
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
        game_audio_envelope_samples, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
    CHECK(game_audio_envelope_samples[100] != 0);
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
        game_audio_envelope_samples, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
    size_t held_pitch_edges = 0;
    int held_previous = 0;
    for (size_t frame = 1; frame < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES; frame++) {
        int after = game_audio_envelope_samples[frame * 2u];
        if (held_previous != 0 && after != 0
            && (held_previous < 0) != (after < 0))
            held_pitch_edges++;
        if (after != 0) held_previous = after;
    }
    CHECK(held_pitch_edges >= 18u && held_pitch_edges <= 22u);
    puts("test: direct gain automation skips constant mix resolution; chains stay live");
    puts("test: game audio schedules at a returned currentTime without rereading the clock");
    CHECK(script_runtime_evaluate_diagnostic(runtime,
              "(()=>{const g=__gameSynthGain,c=g.context,curve=new Float32Array([.25,.25]);"
              "const original=performance.now;let reads=0;"
              "performance.now=function(){reads++;return original.call(performance)};"
              "let due=false,future=false,later=-1;"
              "try{g.gain.cancelScheduledValues(0);const now=c.currentTime;reads=0;"
              "g.gain.cancelScheduledValues(now);g.gain.setValueCurveAtTime(curve,now,.005);"
              "due=reads===0;reads=0;g.gain.cancelScheduledValues(0);"
              "g.gain.setTargetAtTime(.2,c.currentTime+.25,.01);"
              "future=reads===2;later=c.currentTime-now}"
              "finally{performance.now=original;g.gain.cancelScheduledValues(0)}"
              "globalThis.pocSummary=due&&future&&later>=0?'GAME-AUDIO-DUE-NOW':"
              "'GAME-AUDIO-CLOCK-'+due+'-'+future+'-'+later})()",
              "<game-audio-due-now>", &result)
          && strcmp(result.summary, "GAME-AUDIO-DUE-NOW") == 0);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
              "(()=>{const o=__gameCurveOsc,g=__gameSynthGain,c=g.context;"
              "g.gain.cancelScheduledValues(0);"
              "const original=Math.min;let clamps=0;"
              "Math.min=function(a,b){clamps++;return original(a,b)};"
              "try{g.gain.setValueCurveAtTime(new Float32Array([.25,.25]),0,.005)}"
              "finally{Math.min=original}"
              "const direct=clamps===0;g.gain.cancelScheduledValues(0);"
              "const downstream=c.createGain(),pan=__gameSynthPan;"
              "downstream.gain.value=.5;pan.pan.value=-1;"
              "g.connect(downstream).connect(pan).connect(c.destination);"
              "g.gain.setValueCurveAtTime(new Float32Array([.25,.25]),0,.005);"
              "globalThis.__gameCurveDownstream=downstream;"
              "globalThis.pocSummary=direct?'GAME-AUDIO-DIRECT-MIX':"
              "'GAME-AUDIO-REDUNDANT-MIX'})()",
              "<game-audio-direct-mix>", &result)
          && strcmp(result.summary, "GAME-AUDIO-DIRECT-MIX") == 0);
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
        game_audio_envelope_samples, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
    int chained_gain_peak = 0;
    for (size_t frame = 0; frame < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES; frame++) {
        int sample = game_audio_envelope_samples[frame * 2u];
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > chained_gain_peak) chained_gain_peak = magnitude;
        CHECK(game_audio_envelope_samples[frame * 2u + 1u] == 0);
    }
    CHECK(chained_gain_peak > 3900 && chained_gain_peak <= 4096);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
              "__gameSynthGain.gain.cancelScheduledValues(0);"
              "__gameCurveDownstream.gain.value=.25;"
              "__gameSynthGain.gain.setValueCurveAtTime(new Float32Array([.25,.25]),0,.005);"
              "globalThis.pocSummary='GAME-AUDIO-CHAIN-CHANGED'",
              "<game-audio-chain-changed>", &result));
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
        game_audio_envelope_samples, TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
    int changed_gain_peak = 0;
    for (size_t frame = 0; frame < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES; frame++) {
        int sample = game_audio_envelope_samples[frame * 2u];
        int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > changed_gain_peak) changed_gain_peak = magnitude;
        CHECK(game_audio_envelope_samples[frame * 2u + 1u] == 0);
    }
    CHECK(changed_gain_peak > 1950 && changed_gain_peak <= 2048);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
              "__gameCurveOsc.stop();__gameSynthGain.gain.cancelScheduledValues(0);"
              "globalThis.pocSummary='GAME-AUDIO-CURVES-STOPPED'",
              "<game-audio-curve-stop>", &result)
          && script_runtime_advance(runtime, 0, 4, &result));
    static const char game_audio_loop_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,source="
        "context.createBufferSource(),pan=__gameSynthPan;"
        "source.buffer=__gameAudioBuffer;source.loop=true;"
        "source.loopStart=1/44100;source.loopEnd=3/44100;"
        "source.connect(pan).connect(context.destination);source.start();"
        "pan.pan.value=-1;globalThis.__gameLoopSource=source;"
        "globalThis.pocSummary='GAME-AUDIO-LOOP-STARTED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_loop_probe,
              "<game-audio-loop>", &result)
          && strcmp(result.summary, "GAME-AUDIO-LOOP-STARTED") == 0);
    int16_t game_audio_loop_samples[12] = {0};
    CHECK(tilefinch_game_audio_mix(runtime->game_audio,
                                  game_audio_loop_samples, 6)
          && game_audio_loop_samples[0] == 1000
          && game_audio_loop_samples[1] == 0
          && game_audio_loop_samples[2] == -1000
          && game_audio_loop_samples[4] == 2000
          && game_audio_loop_samples[6] == -1000
          && game_audio_loop_samples[8] == 2000);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "__gameLoopSource.stop();globalThis.pocSummary="
              "'GAME-AUDIO-LOOP-STOPPED'",
              "<game-audio-loop-stop>", &result)
          && strcmp(result.summary, "GAME-AUDIO-LOOP-STOPPED") == 0
          && script_runtime_advance(runtime, 0, 4, &result));
    static const char game_audio_bounds_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,sources=[];"
        "for(let i=0;i<4;i++){const source=context.createBufferSource();"
        "source.buffer=__gameAudioBuffer;source.loop=true;"
        "source.connect(context.destination);source.start();sources.push(source)}"
        "sources[0].connect(__gameSynthGain).connect(context.destination);"
        "const firstMix=sources[0]._updateMix,otherMix=sources[1]._updateMix;"
        "let firstUpdates=0,otherUpdates=0;"
        "sources[0]._updateMix=()=>firstUpdates++;"
        "sources[1]._updateMix=()=>otherUpdates++;"
        "__gameSynthGain.gain.value=.3;"
        "const mixFiltered=firstUpdates===1&&otherUpdates===0;"
        "const unrelatedTarget=sources[2]._target;let unrelatedVisits=0;"
        "Object.defineProperty(sources[2],'_target',{configurable:true,"
        "get(){unrelatedVisits++;return unrelatedTarget}});"
        "__gameSynthGain.gain.value=.31;"
        "const graphRetained=unrelatedVisits===0;"
        "Object.defineProperty(sources[2],'_target',{configurable:true,"
        "writable:true,value:unrelatedTarget});"
        "sources[0].connect(context.destination);"
        "sources[1].connect(__gameSynthGain);firstUpdates=otherUpdates=0;"
        "__gameSynthGain.gain.value=.32;"
        "const graphReconnected=firstUpdates===0&&otherUpdates===1;"
        "sources[1].disconnect();firstUpdates=otherUpdates=0;"
        "__gameSynthGain.gain.value=.33;"
        "const graphDisconnected=firstUpdates===0&&otherUpdates===0;"
        "sources[0]._updateMix=firstMix;sources[1]._updateMix=otherMix;"
        "let bounded=false;try{const extra=context.createBufferSource();"
        "extra.buffer=__gameAudioBuffer;extra.connect(context.destination);"
        "extra.start()}catch(error){bounded=error.name==='QuotaExceededError'}"
        "globalThis.__stoppedAudioEnded=0;for(const source of sources){"
        "source.onended=()=>__stoppedAudioEnded++;source.stop()}"
        "let pinned=false;try{const pending=context.createBufferSource();"
        "pending.buffer=__gameAudioBuffer;pending.connect(context.destination);"
        "pending.start()}catch(error){pinned=error.name==='QuotaExceededError'}"
        "globalThis.pocSummary=bounded&&pinned&&mixFiltered&&graphRetained"
        "&&graphReconnected&&graphDisconnected"
        "?'GAME-AUDIO-BOUNDS-OK':'GAME-AUDIO-BOUNDS-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_bounds_probe,
              "<game-audio-bounds>", &result)
          && strcmp(result.summary, "GAME-AUDIO-BOUNDS-OK") == 0);
    CHECK(script_runtime_advance(runtime, 0, 4, &result)
          && script_runtime_evaluate_diagnostic(
                 runtime,
                 "(()=>{let reused=false;try{const source="
                 "__gameAudioBuffer._context.createBufferSource();"
                 "source.buffer=__gameAudioBuffer;"
                 "source.connect(__gameAudioBuffer._context.destination);"
                 "source.start();source.stop();reused=true}catch(error){}"
                 "globalThis.pocSummary=__stoppedAudioEnded===4&&reused"
                 "?'GAME-AUDIO-STOP-ENDED-OK':"
                 "'GAME-AUDIO-STOP-ENDED-FAILED'})()",
                 "<game-audio-stop-ended>", &result)
          && strcmp(result.summary, "GAME-AUDIO-STOP-ENDED-OK") == 0);
    script_runtime_suspend_game_audio(runtime);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "globalThis.pocSummary=__gameAudioBuffer._context.state==="
              "'suspended'?'GAME-AUDIO-HOST-SUSPEND-OK':"
              "'GAME-AUDIO-HOST-SUSPEND-FAILED'",
              "<game-audio-host-suspend>", &result)
          && strcmp(result.summary, "GAME-AUDIO-HOST-SUSPEND-OK") == 0);
    static const char game_audio_reentrant_close_probe[] =
        "(()=>{const context=__gameAudioBuffer._context,source="
        "context.createBufferSource();source.buffer=__gameAudioBuffer;"
        "source.connect(context.destination);let safe=false;try{source.start(0,"
        "{valueOf(){context.close();return 0}})}catch(error){safe="
        "context.state==='closed'}globalThis.pocSummary=safe"
        "?'GAME-AUDIO-REENTRANT-CLOSE-OK':"
        "'GAME-AUDIO-REENTRANT-CLOSE-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, game_audio_reentrant_close_probe,
              "<game-audio-reentrant-close>", &result)
          && strcmp(result.summary,
                    "GAME-AUDIO-REENTRANT-CLOSE-OK") == 0
          && runtime->game_audio == NULL);

    static const char responsive_embedding_probe[] =
        "(()=>{const picture=document.createElement('picture'),"
        "source=document.createElement('source'),img=document.createElement('img');"
        "source.srcset='data:,selected';picture.appendChild(source);"
        "img.src='fallback.png';img.srcset='small.png 100w, large.png 400w';"
        "img.sizes='calc(25vw + 10px)';picture.appendChild(img);"
        "document.body.appendChild(picture);const selected=img.currentSrc;"
        "source.media='not all';const fallback=img.currentSrc;"
        "const frame=document.createElement('iframe'),view=frame.contentWindow;"
        "view.initialMarker='preserved';"
        "frame.srcdoc='<p>first</p>';document.body.appendChild(frame);"
        "const first=frame.contentDocument.body.textContent,"
        "initialPreserved=view.initialMarker==='preserved';"
        "view.sameValueStale='stale';frame.srcdoc='<p>first</p>';"
        "const sameValueFresh=view.sameValueStale===undefined"
        "&&frame.contentDocument.body.textContent==='first';"
        "view.oldDocumentValue='stale';view.eval('var staleFrameVar=1');"
        "frame.srcdoc='<p>second</p>';"
        "const second=frame.contentDocument.body.textContent,"
        "stable=view===frame.contentWindow"
        "&&frame.contentDocument.defaultView===view"
        "&&view.oldDocumentValue===undefined"
        "&&view.eval('typeof staleFrameVar')==='undefined';"
        "const srcdocDocument=frame.contentDocument;view.fallbackMarker='kept';"
        "frame.src='fallback-a.html';const fallbackSetIgnored="
        "frame.contentDocument===srcdocDocument&&view.fallbackMarker==='kept';"
        "frame.removeAttribute('src');const fallbackRemoveIgnored="
        "frame.contentDocument===srcdocDocument&&view.fallbackMarker==='kept';"
        "frame.removeAttribute('srcdoc');"
        "const cleared=frame.contentDocument.body.textContent,"
        "blank=document.createElement('iframe'),blankView=blank.contentWindow;"
        "blankView.initialBlankMarker='preserved';document.body.appendChild(blank);"
        "const initialBlankDocument=blank.contentDocument;blank.src='about:blank';"
        "const explicitBlankFresh=blank.contentDocument!==initialBlankDocument"
        "&&blankView.initialBlankMarker==='preserved';blankView.removalStale='stale';"
        "blank.removeAttribute('src');const removalFresh="
        "blankView.removalStale===undefined,empty=document.createElement('iframe');"
        "empty.src='';const emptyView=empty.contentWindow;emptyView.firstMarker='kept';"
        "document.body.appendChild(empty);empty.srcdoc='<p>empty-first</p>';const "
        "emptyPreserved=emptyView.firstMarker==='kept';emptyView.nextMarker='stale';"
        "empty.srcdoc='<p>empty-second</p>';const emptyCleared="
        "emptyView.nextMarker===undefined,about=document.createElement('iframe');"
        "about.src='about:blank';const aboutView=about.contentWindow;aboutView."
        "firstMarker='kept';document.body.appendChild(about);about.srcdoc="
        "'<p>about-first</p>';const aboutPreserved=aboutView.firstMarker==='kept';"
        "aboutView.nextMarker='stale';about.srcdoc='<p>about-second</p>';const "
        "aboutCleared=aboutView.nextMarker===undefined;"
        "globalThis.pocSummary=selected==='data:,selected'"
        "&&fallback==='https://example.test/large.png'"
        "&&first==='first'&&initialPreserved&&second==='second'&&cleared===''"
        "&&sameValueFresh&&fallbackSetIgnored&&fallbackRemoveIgnored"
        "&&explicitBlankFresh&&removalFresh&&emptyPreserved&&emptyCleared"
        "&&aboutPreserved&&aboutCleared&&stable?"
        "'RESPONSIVE-EMBEDDING-OK':'RESPONSIVE-EMBEDDING-FAILED:'+"
        "[first,initialPreserved,sameValueFresh,second,stable,"
        "fallbackSetIgnored,fallbackRemoveIgnored,cleared,explicitBlankFresh,"
        "removalFresh,emptyPreserved,emptyCleared,aboutPreserved,aboutCleared]."
        "join(',');})()";
    bool responsive_embedding_ok = script_runtime_evaluate_diagnostic(
        runtime, responsive_embedding_probe,
        "<responsive-embedding-probe>", &result);
    if (!responsive_embedding_ok
        || strcmp(result.summary, "RESPONSIVE-EMBEDDING-OK") != 0) {
        fprintf(stderr, "responsive embedding: ok=%d summary=%s error=%s\n",
                responsive_embedding_ok, result.summary, result.error);
    }
    CHECK(responsive_embedding_ok
          && strcmp(result.summary, "RESPONSIVE-EMBEDDING-OK") == 0);

    static const char media_factory_probe[] =
        "(()=>{const audio=new Audio(),sourced=new Audio('sounds/tone.mp3'),"
        "called=Audio(),video=document.createElement('video'),"
        "source=document.createElement('source');"
        "source.src='sounds/fallback.ogg';called.appendChild(source);"
        "const skipped=document.createElement('source'),"
        "selected=document.createElement('source');"
        "skipped.src='movie.webm';skipped.type='video/webm';"
        "const hidden=document.createElement('source');"
        "hidden.src='hidden.mp4';hidden.type='video/mp4';"
        "hidden.media='not all';video.appendChild(hidden);"
        "selected.src='movie.mp4';selected.type='video/mp4';"
        "video.appendChild(skipped);video.appendChild(selected);"
        "const events=[];for(const name of ['seeking','seeked'])"
        "audio.addEventListener(name,()=>events.push(name));"
        "const promise=audio.play();promise.catch(()=>{});"
        "audio.currentTime=1.5;audio.volume=.25;"
        "globalThis.pocSummary=typeof Audio==='function'"
        "&&Audio.prototype===HTMLAudioElement.prototype"
        "&&audio instanceof Audio&&video instanceof HTMLVideoElement"
        "&&called instanceof HTMLAudioElement&&audio.preload==='auto'"
        "&&sourced.currentSrc==='https://example.test/sounds/tone.mp3'"
        "&&called.currentSrc==='https://example.test/sounds/fallback.ogg'"
        "&&video.currentSrc==='https://example.test/movie.mp4'"
        "&&audio.canPlayType('audio/mpeg')===''&&promise instanceof Promise"
        "&&audio.paused&&audio.currentTime===1.5&&audio.volume===.25"
        "&&events.join(',')==='seeking,seeked'"
        "&&audio.readyState===HTMLMediaElement.HAVE_NOTHING"
        "&&audio.networkState===HTMLMediaElement.NETWORK_EMPTY"
        "?'MEDIA-FACTORY-OK':'MEDIA-FACTORY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, media_factory_probe, "<media-factory-probe>", &result)
          && strcmp(result.summary, "MEDIA-FACTORY-OK") == 0);

    static const char collator_probe[] =
        "(()=>{const collator=new Intl.Collator(undefined,{sensitivity:'base',"
        "numeric:true}),compare=collator.compare,called=Intl.Collator('en-US',"
        "{ignorePunctuation:true}),options=collator.resolvedOptions();"
        "globalThis.pocSummary=typeof Intl.Collator==='function'"
        "&&collator instanceof Intl.Collator&&called instanceof Intl.Collator"
        "&&compare('Éclair','eclair')===0&&compare('item2','item10')<0"
        "&&called.compare('a-b','ab')===0&&options.sensitivity==='base'"
        "&&options.numeric===true&&Intl.Collator.supportedLocalesOf(['en-US'])[0]"
        "==='en-US'?'COLLATOR-OK':'COLLATOR-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, collator_probe, "<collator-probe>", &result)
          && strcmp(result.summary, "COLLATOR-OK") == 0);

    static const char additional_intl_probe[] =
        "(()=>{const relative=new Intl.RelativeTimeFormat('en-US',"
        "{numeric:'auto'}),list=new Intl.ListFormat('en-US',"
        "{type:'disjunction'}),names=new Intl.DisplayNames('en-US',"
        "{type:'language',fallback:'none'});"
        "const relativeParts=relative.formatToParts(-2,'days'),"
        "listParts=list.formatToParts(['red','green','blue']);"
        "globalThis.pocSummary=relative.format(-1,'day')==='yesterday'"
        "&&relative.format(2,'hours')==='in 2 hours'"
        "&&relativeParts.some(part=>part.type==='integer'&&part.value==='2')"
        "&&list.format(['red','green'])==='red or green'"
        "&&list.format(['red','green','blue'])==='red, green, or blue'"
        "&&listParts.filter(part=>part.type==='element').length===3"
        "&&names.of('fr')==='French'&&names.of('zz')===undefined"
        "&&Intl.RelativeTimeFormat.supportedLocalesOf(['en-US'])[0]==='en-US'"
        "?'ADDITIONAL-INTL-OK':'ADDITIONAL-INTL-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, additional_intl_probe, "<additional-intl-probe>",
              &result)
          && strcmp(result.summary, "ADDITIONAL-INTL-OK") == 0);

    static const char clipboard_probe[] =
        "(async()=>{const area=document.createElement('textarea');"
        "area.value='copy this text';document.body.appendChild(area);"
        "area.setSelectionRange(5,9);area.select();area.setSelectionRange(5,9);"
        "const copied=document.execCommand('copy'),legacy=await navigator.clipboard.readText();"
        "await navigator.clipboard.writeText('modern text');"
        "const modern=await navigator.clipboard.readText();area.remove();"
        "globalThis.pocSummary=copied&&legacy==='this'&&modern==='modern text'"
        "&&document.queryCommandSupported('copy')"
        "?'CLIPBOARD-OK':'CLIPBOARD-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, clipboard_probe, "<clipboard-probe>", &result)
          && strcmp(result.summary, "CLIPBOARD-OK") == 0
          && result.clipboard_writes == 2
          && strcmp(result.last_clipboard_text, "modern text") == 0);

    static const char document_location_probe[] =
        "(()=>{const before=location.href,host=location.hostname;"
        "const initial=document.URL===before&&document.documentURI===before"
        "&&document.baseURI===before&&document.domain===host;"
        "document.domain=host;history.replaceState(null,'','#standards');"
        "const live=document.URL===location.href"
        "&&document.documentURI===location.href"
        "&&document.baseURI===location.href;let rejected=false;"
        "try{document.domain=host.includes('.')?host.slice(host.indexOf('.')+1)"
        ":'invalid.example';}catch(error){rejected=error instanceof DOMException"
        "&&error.name==='SecurityError';}history.replaceState(null,'',before);"
        "globalThis.pocSummary=initial&&live&&rejected&&document.domain===host"
        "?'DOCUMENT-LOCATION-OK':'DOCUMENT-LOCATION-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, document_location_probe, "<document-location-probe>",
              &result)
          && strcmp(result.summary, "DOCUMENT-LOCATION-OK") == 0);

    static const char frozen_base_probe[] =
        "(()=>{const before=location.href,head=document.querySelector('head'),"
        "base=document.createElement('base');base.setAttribute('href','assets/');"
        "head.appendChild(base);const frozen=new URL('assets/',before).href,"
        "moved=new URL('/moved/page.html',before).href;"
        "history.replaceState(null,'',moved);const stayed=document.baseURI===frozen;"
        "base.setAttribute('href','changed/');const changed="
        "new URL('changed/',moved).href,changedNow=document.baseURI===changed;"
        "document.body.setAttribute('data-unrelated','yes');"
        "const again=new URL('/again/index.html',before).href;"
        "history.replaceState(null,'',again);const stayedAgain="
        "document.baseURI===changed;base.setAttribute('href','data:text/plain,no');"
        "const invalidNow=document.baseURI===again,third="
        "new URL('/third/index.html',before).href;history.replaceState(null,'',third);"
        "const invalidStayed=document.baseURI===again;base.remove();"
        "const fallback=document.baseURI===location.href;"
        "history.replaceState(null,'',before);"
        "globalThis.pocSummary=stayed&&changedNow&&stayedAgain&&invalidNow"
        "&&invalidStayed&&fallback"
        "&&document.baseURI===before?'FROZEN-BASE-OK':'FROZEN-BASE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frozen_base_probe, "<frozen-base-probe>", &result)
          && strcmp(result.summary, "FROZEN-BASE-OK") == 0);
    (void) script_runtime_collect_and_trim(runtime);

    static const char ordinary_namespace_read_probe[] =
        "(()=>{const node=document.createElement('div');"
        "node.setAttribute('data-probe','first');"
        "Object.defineProperty(node,'attributes',{get(){throw Error('enumerated')}});"
        "const first=node.getAttributeNS(null,'data-probe')==='first';"
        "node.setAttribute('data-probe','second');"
        "const live=node.getAttributeNS('','data-probe')==='second';"
        "node.removeAttribute('data-probe');"
        "globalThis.pocSummary=first&&live&&node.getAttributeNS(null,'data-probe')===null"
        "?'ORDINARY-NS-READ-OK':'ORDINARY-NS-READ-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(runtime, ordinary_namespace_read_probe,
              "<ordinary-namespace-read-probe>", &result)
          && strcmp(result.summary, "ORDINARY-NS-READ-OK") == 0);

    static const char namespaced_attribute_probe[] =
        "(()=>{const svg=document.createElementNS('http://www.w3.org/2000/svg',"
        "'svg'),use=document.createElementNS('http://www.w3.org/2000/svg','use'),"
        "xlink='http://www.w3.org/1999/xlink';svg.appendChild(use);"
        "use.setAttributeNS(xlink,'xlink:href','#mark');const set="
        "use.getAttributeNS(xlink,'href')==='#mark'"
        "&&use.hasAttributeNS(xlink,'href');use.removeAttributeNS(xlink,'href');"
        "const html=document.createElement('div');html.setAttribute('data-a','plain');"
        "const sensitive=html.getAttributeNS(null,'DATA-A')===null;"
        "html.setAttributeNS('urn:test','x:data-a','namespaced');"
        "const mirrored=html.getAttributeNS('urn:test','data-a')==='namespaced'"
        "&&html.getAttributeNS(null,'data-a')==='plain';"
        "use.setAttribute('viewBox','0 0 1 1');"
        "globalThis.pocSummary=set&&sensitive&&mirrored"
        "&&use.getAttributeNS(null,'viewBox')==='0 0 1 1'"
        "&&!use.hasAttributeNS(xlink,'href')"
        "?'NAMESPACE-ATTRIBUTE-OK':'NAMESPACE-ATTRIBUTE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, namespaced_attribute_probe,
              "<namespaced-attribute-probe>", &result)
          && strcmp(result.summary, "NAMESPACE-ATTRIBUTE-OK") == 0);

    static const char svg_element_probe[] =
        "(()=>{const svgNS='http://www.w3.org/2000/svg',"
        "htmlNS='http://www.w3.org/1999/xhtml',"
        "parsed=document.getElementById('parsed-svg'),"
        "group=document.getElementById('parsed-group'),"
        "made=document.createElementNS(svgNS,'svg'),"
        "path=document.createElementNS(svgNS,'path'),"
        "rect=document.createElementNS(svgNS,'rect'),"
        "html=document.createElementNS(htmlNS,'div'),"
        "plain=document.createElementNS(null,'widget');made.appendChild(path);"
        "rect.setAttribute('x','3');rect.setAttribute('y','4');"
        "rect.setAttribute('width','12');rect.setAttribute('height','7');"
        "rect.style.cssText='color: red; width: 12px';"
        "rect.style.setProperty('height','7px');"
        "const detachedComputed=getComputedStyle(rect);"
        "made.appendChild(rect);const box=rect.getBBox();"
        "const text=document.createElementNS(svgNS,'text');"
        "text.textContent='metric';made.appendChild(text);"
        "const textLength=text.getComputedTextLength();"
        "let nonTextRejected=false;try{rect.getComputedTextLength()}"
        "catch(error){nonTextRejected=error instanceof TypeError}"
        "let namespaceError=false,characterError=false;"
        "try{document.createElementNS(null,'x:item')}catch(error){"
        "namespaceError=error instanceof DOMException"
        "&&error.name==='NamespaceError'}"
        "try{document.createElementNS(svgNS,'bad name')}catch(error){"
        "characterError=error instanceof DOMException"
        "&&error.name==='InvalidCharacterError'}"
        "const detached=document.implementation.createDocument(svgNS,'svg'),"
        "detachedRoot=detached.documentElement;"
        "const ok=typeof SVGElement==='function'"
        "&&typeof SVGGraphicsElement==='function'"
        "&&typeof SVGGeometryElement==='function'"
        "&&typeof SVGTextContentElement==='function'"
        "&&typeof SVGSVGElement==='function'"
        "&&Object.getPrototypeOf(SVGElement.prototype)===Element.prototype"
        "&&SVGGraphicsElement.prototype===SVGElement.prototype"
        "&&SVGTextContentElement.prototype===SVGElement.prototype"
        "&&parsed instanceof SVGSVGElement"
        "&&parsed instanceof Element&&!(parsed instanceof HTMLElement)"
        "&&parsed.namespaceURI===svgNS&&parsed.tagName==='svg'"
        "&&group instanceof SVGGraphicsElement&&group.ownerSVGElement===parsed"
        "&&made instanceof SVGSVGElement&&path instanceof SVGGeometryElement"
        "&&path.ownerSVGElement===made&&path.viewportElement===made"
        "&&rect instanceof SVGGeometryElement&&box instanceof DOMRect"
        "&&text instanceof SVGTextContentElement"
        "&&Number.isFinite(textLength)&&textLength>=0&&nonTextRejected"
        "&&box.x===3&&box.y===4&&box.width===12&&box.height===7"
        "&&rect.style.getPropertyValue('color')==='red'"
        "&&rect.style.width==='12px'&&rect.style.height==='7px'"
        /* `made` is never attached, so rect stays disconnected. CSSOM gives
           a disconnected element an empty declaration block: no inline text
           and no default stands in for a computed value. */
        "&&detachedComputed.getPropertyValue('width')===''"
        "&&detachedComputed.display===''&&detachedComputed.length===0"
        "&&html instanceof HTMLDivElement&&!(html instanceof SVGElement)"
        "&&plain instanceof Element&&!(plain instanceof HTMLElement)"
        "&&!(plain instanceof SVGElement)&&plain.namespaceURI===null"
        "&&detachedRoot instanceof SVGSVGElement"
        "&&detachedRoot.namespaceURI===svgNS"
        "&&namespaceError&&characterError;"
        "globalThis.pocSummary=ok?'SVG-ELEMENT-OK':'SVG-ELEMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, svg_element_probe, "<svg-element-probe>", &result)
          && strcmp(result.summary, "SVG-ELEMENT-OK") == 0);

    static const char window_event_target_probe[] =
        "(()=>{let calls=0;const listener=()=>calls++,"
        "type='tilefinch-window-event-target-probe';"
        "EventTarget.prototype.addEventListener.call(window,type,listener);"
        "window.dispatchEvent(new Event(type));"
        "EventTarget.prototype.removeEventListener.call(window,type,listener);"
        "window.dispatchEvent(new Event(type));"
        "globalThis.pocSummary=window instanceof Window&&"
        "Object.getPrototypeOf(Window.prototype)===EventTarget.prototype&&"
        "Object.prototype.toString.call(window)==='[object Window]'&&"
        "screen instanceof Screen&&"
        "screen instanceof EventTarget&&"
        "screen.orientation instanceof ScreenOrientation&&"
        "!('onclick' in EventTarget.prototype)&&"
        "!('onabort' in screen)&&"
        "'onclick' in HTMLElement.prototype&&"
        "'onclick' in window&&"
        "Object.prototype.toString.call(screen)==='[object Screen]'&&"
        "Object.prototype.toString.call(screen.orientation)==="
        "'[object ScreenOrientation]'&&"
        "calls===1?'WINDOW-EVENT-TARGET-OK':"
        "'WINDOW-EVENT-TARGET-FAILED:'+calls;})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, window_event_target_probe,
              "<window-event-target-probe>", &result)
          && strcmp(result.summary, "WINDOW-EVENT-TARGET-OK") == 0);

    static const char canvas_2d_probe[] =
        "(()=>{const canvas=document.createElement('canvas'),"
        "defaults=canvas.width===300&&canvas.height===150;"
        "canvas.width=2;canvas.height=1;const context=canvas.getContext('2d'),"
        "same=context===canvas.getContext('2D'),"
        "unsupported=canvas.getContext('webgl')===null;"
        "context.fillStyle='rebeccapurple';const named=context.fillStyle;"
        "context.fillStyle='not-a-color';const invalidRetained="
        "context.fillStyle===named;context.fillStyle='blue';"
        "context.fillRect(0,0,2,1);context.save();context.fillStyle='red';"
        "context.globalAlpha=.5;context.fillRect(0,0,1,1);"
        "context.globalAlpha=2;const alphaRetained=context.globalAlpha===.5;"
        "context.restore();const restored=context.fillStyle==='#0000ff'"
        "&&context.globalAlpha===1,pixel=context.getImageData(0,0,1,1).data;"
        "context.clearRect(1,0,1,1);const clear="
        "context.getImageData(1,0,1,1).data;"
        "const supplied=new ImageData(new Uint8ClampedArray([1,2,3,4]),1);"
        "context.putImageData(supplied,1,0);const put="
        "context.getImageData(1,0,1,1).data;"
        "const cloned=context.createImageData(supplied),cloneSized="
        "cloned.width===1&&cloned.height===1&&cloned!==supplied"
        "&&cloned.data.every(value=>value===0);"
        "const dirtyCanvas=document.createElement('canvas');"
        "dirtyCanvas.width=3;dirtyCanvas.height=1;"
        "const dirtyContext=dirtyCanvas.getContext('2d'),dirtySource="
        "new ImageData(new Uint8ClampedArray([255,0,0,255,0,255,0,255,"
        "0,0,255,255]),3,1);"
        "dirtyContext.putImageData(dirtySource,0,0,1,0,1,1);"
        "const dirtyPixels=dirtyContext.getImageData(0,0,3,1).data,"
        "dirtyPut=dirtyPixels[3]===0&&dirtyPixels[4]===0"
        "&&dirtyPixels[5]===255&&dirtyPixels[6]===0"
        "&&dirtyPixels[7]===255&&dirtyPixels[11]===0;"
        "dirtyContext.clearRect(0,0,3,1);"
        "dirtyContext.putImageData(dirtySource,0,0,2,0,-2,1);"
        "const negativeDirty=dirtyContext.getImageData(0,0,3,1).data,"
        "negativeDirtyPut=negativeDirty[0]===255&&negativeDirty[3]===255"
        "&&negativeDirty[4]===0&&negativeDirty[5]===255"
        "&&negativeDirty[7]===255&&negativeDirty[11]===0;"
        "let dirtyRange=false;try{dirtyContext.putImageData("
        "dirtySource,Infinity,0)}catch(error){dirtyRange="
        "error instanceof DOMException&&error.name==='NotSupportedError'}"
        "dirtyContext.fillStyle='red';dirtyContext.fillRect(0,0,3,1);"
        "dirtyContext.save();dirtyContext.translate(2,0);"
        "dirtyContext.lineWidth=7;dirtyContext.reset();"
        "dirtyContext.restore();const apiReset="
        "dirtyContext.lineWidth===1&&dirtyContext.fillStyle==='#000000'"
        "&&dirtyContext.getTransform().e===0"
        "&&dirtyContext.getImageData(0,0,3,1).data.every(value=>value===0);"
        "const compositeCanvas=document.createElement('canvas');"
        "compositeCanvas.width=1;compositeCanvas.height=1;"
        "const compositeContext=compositeCanvas.getContext('2d');"
        "compositeContext.fillStyle='red';compositeContext.fillRect(0,0,1,1);"
        "compositeContext.globalCompositeOperation='destination-out';"
        "compositeContext.globalAlpha=.5;compositeContext.fillRect(0,0,1,1);"
        "compositeContext.globalCompositeOperation='invalid';"
        "const composite=compositeContext.getImageData(0,0,1,1).data,"
        "compositeMode=compositeContext.globalCompositeOperation;"
        "context.fillStyle='hsl(120 100% 25%)';const hsl=context.fillStyle;"
        "canvas.setAttribute('width','2');const reset="
        "context.fillStyle==='#000000'&&context.globalAlpha===1"
        "&&context.getImageData(0,0,1,1).data.every(value=>value===0);"
        "canvas.width=513;canvas.height=257;context.fillStyle='#123456';"
        "context.fillRect(0,0,1,1);const boundedPixel="
        "context.getImageData(0,0,1,1).data,bounded="
        "canvas.width===480&&canvas.height===240"
        "&&boundedPixel[0]===18&&boundedPixel[1]===52"
        "&&boundedPixel[2]===86&&boundedPixel[3]===255;"
        "canvas.width=2147483647;canvas.height=0;const zeroBound="
        "canvas.width===480&&canvas.height===0;"
        "let quota=false,index=false;try{context.createImageData(513,257)}"
        "catch(error){quota=error instanceof DOMException"
        "&&error.name==='QuotaExceededError'}"
        "try{context.getImageData(0,0,0,1)}catch(error){"
        "index=error instanceof DOMException&&error.name==='IndexSizeError'}"
        "const ok=defaults&&context instanceof CanvasRenderingContext2D"
        "&&same&&unsupported&&invalidRetained&&named==='#663399'"
        "&&alphaRetained&&restored&&pixel[0]===128&&pixel[1]===0"
        "&&pixel[2]===128&&pixel[3]===255"
        "&&clear.every(value=>value===0)&&put[0]===1&&put[1]===2"
        "&&put[2]===3&&put[3]===4&&composite[0]===255"
        "&&composite[1]===0&&composite[2]===0&&composite[3]===128"
        "&&compositeMode==='destination-out'"
        "&&hsl==='#008000'&&reset&&bounded&&zeroBound"
        "&&quota&&index&&supplied.width===1&&supplied.height===1"
        "&&supplied.colorSpace==='srgb'&&cloneSized&&dirtyPut"
        "&&negativeDirtyPut&&dirtyRange&&apiReset;"
        "globalThis.pocSummary=ok?"
        "'CANVAS-2D-OK':'CANVAS-2D-FAILED:'+JSON.stringify({defaults,same,"
        "unsupported,invalidRetained,named,alphaRetained,restored,"
        "pixel:[...pixel],clear:[...clear],put:[...put],"
        "composite:[...composite],compositeMode,hsl,reset,bounded,zeroBound,"
        "quota,index,cloneSized,dirtyPut,negativeDirtyPut,dirtyRange,"
        "apiReset});})()";
    bool canvas_2d_ok = script_runtime_evaluate_diagnostic(
        runtime, canvas_2d_probe, "<canvas-2d-probe>", &result);
    if (!canvas_2d_ok || strcmp(result.summary, "CANVAS-2D-OK") != 0) {
        fprintf(stderr, "canvas 2d probe: ok=%d summary=%s error=%s\n",
                canvas_2d_ok, result.summary, result.error);
    }
    CHECK(canvas_2d_ok && strcmp(result.summary, "CANVAS-2D-OK") == 0);

    static const char image_bitmap_probe[] =
        "(async()=>{const data=new ImageData(new Uint8ClampedArray(["
        "255,0,0,255,0,0,255,255]),2,1),"
        "bitmap=await createImageBitmap(data,1,0,1,1,{resizeWidth:2,"
        "resizeHeight:2,resizeQuality:'pixelated'}),"
        "ratio=await createImageBitmap(data,{resizeWidth:4}),"
        "canvas=document.createElement('canvas');canvas.width=2;canvas.height=2;"
        "const context=canvas.getContext('2d');context.drawImage(bitmap,0,0);"
        "const pixel=context.getImageData(1,1,1,1).data,good="
        "bitmap instanceof ImageBitmap&&bitmap.width===2&&bitmap.height===2"
        "&&ratio.width===4&&ratio.height===2"
        "&&pixel[0]===0&&pixel[1]===0&&pixel[2]===255&&pixel[3]===255;"
        "bitmap.close();ratio.close();let closed=false,illegal=false;"
        "try{context.drawImage(bitmap,0,0)}"
        "catch(error){closed=error.name==='InvalidStateError'}"
        "try{new ImageBitmap()}catch(error){illegal=error instanceof TypeError}"
        "globalThis.pocSummary=good&&closed&&illegal?'IMAGE-BITMAP-OK':"
        "'IMAGE-BITMAP-FAILED';})().catch(error=>globalThis.pocSummary="
        "'IMAGE-BITMAP-ERROR:'+error)";
    bool image_bitmap_ok = script_runtime_evaluate_diagnostic(
        runtime, image_bitmap_probe, "<image-bitmap-probe>", &result);
    if (!image_bitmap_ok || strcmp(result.summary, "IMAGE-BITMAP-OK") != 0)
        fprintf(stderr, "image bitmap probe: ok=%d summary=%s error=%s\n",
                image_bitmap_ok, result.summary, result.error);
    CHECK(image_bitmap_ok && strcmp(result.summary, "IMAGE-BITMAP-OK") == 0);

    static const char canvas_save_overflow_probe[] =
        "(()=>{const canvas=document.createElement('canvas'),"
        "context=canvas.getContext('2d');context.fillStyle='red';"
        "for(let i=0;i<17;i++)context.save();context.fillStyle='blue';"
        "context.restore();const paired=context.fillStyle==='#0000ff';"
        "for(let i=0;i<16;i++)context.restore();const restored="
        "context.fillStyle==='#ff0000';globalThis.pocSummary=paired&&restored"
        "?'CANVAS-SAVE-OVERFLOW-OK':'CANVAS-SAVE-OVERFLOW-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_save_overflow_probe,
              "<canvas-save-overflow-probe>", &result)
          && strcmp(result.summary, "CANVAS-SAVE-OVERFLOW-OK") == 0);

    /* Coercion may run arbitrary page code.  A stale canvas handle must not
       survive wrapper release plus a document subtree replacement between
       argument conversion and the native surface commit. */
    ImageResources canvas_commit_images = {.budget = &budget};
    script_runtime_set_images(runtime, &canvas_commit_images);
    static const char canvas_commit_lifetime_probe[] =
        "(()=>{const old=document.createElement('canvas');old.width=1;"
        "old.height=1;document.body.appendChild(old);const handle=old.__handle,"
        "lease=old.__tilefinchHandleLease,pixels=new Uint8ClampedArray(4);"
        "let released=false;"
        "const hostile={valueOf(){old.remove();released="
        "__tilefinchReleaseNodeWrapper(handle,lease);document.body.innerHTML="
        "'<canvas id=replacement width=1 height=1></canvas>';return 1;}};"
        "const committed=__tilefinchCommitCanvasSurface(handle,hostile,1,"
        "pixels,0,0,1,1),replacement=document.getElementById('replacement');"
        "globalThis.pocSummary=!committed&&replacement&&released"
        "?'CANVAS-COMMIT-LIFETIME-OK':'CANVAS-COMMIT-LIFETIME-FAILED:'+"
        "JSON.stringify({committed,released,replacement:!!replacement,"
        "connected:old.isConnected});})()";
    bool canvas_commit_lifetime_ok = script_runtime_evaluate_diagnostic(
        runtime, canvas_commit_lifetime_probe,
        "<canvas-commit-lifetime-probe>", &result);
    if (!canvas_commit_lifetime_ok
        || strcmp(result.summary, "CANVAS-COMMIT-LIFETIME-OK") != 0) {
        fprintf(stderr, "canvas commit lifetime probe: ok=%d summary=%s error=%s\n",
                canvas_commit_lifetime_ok, result.summary, result.error);
    }
    CHECK(canvas_commit_lifetime_ok
          && strcmp(result.summary, "CANVAS-COMMIT-LIFETIME-OK") == 0);

    static const char canvas_taint_source_probe[] =
        "(()=>{const image=document.createElement('img');"
        "image.id='cross-origin-canvas-source';"
        "image.src='https://images.example.test/private.png';"
        "document.body.appendChild(image);globalThis.pocSummary="
        "'CANVAS-TAINT-SOURCE-READY';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_taint_source_probe,
              "<canvas-taint-source-probe>", &result)
          && strcmp(result.summary, "CANVAS-TAINT-SOURCE-READY") == 0);
    lxb_dom_node_t *taint_source = find_element_id(
        lxb_dom_interface_node(runtime->document->html),
        "cross-origin-canvas-source");
    static unsigned char private_pixel[4] = {17u, 34u, 51u, 255u};
    canvas_commit_images.items = budget_calloc_category(
        &budget, BUDGET_CATEGORY_RESOURCE, 1,
        sizeof(*canvas_commit_images.items));
    CHECK(taint_source != NULL && canvas_commit_images.items != NULL);
    canvas_commit_images.capacity = 1u;
    canvas_commit_images.count = 1u;
    canvas_commit_images.items[0] = (ImageResource) {
        .node = taint_source,
        .pixels = private_pixel,
        .source_width = 1,
        .source_height = 1,
        .width = 1,
        .height = 1,
        .cross_origin = true
    };
    static const char image_decode_probe[] =
        "(async()=>{const image=document.getElementById("
        "'cross-origin-canvas-source');await image.decode();const bitmap="
        "await createImageBitmap(image),canvas=document.createElement('canvas'),"
        "context=canvas.getContext('2d');canvas.width=1;canvas.height=1;"
        "context.drawImage(bitmap,0,0);let tainted=false;try{context.getImageData("
        "0,0,1,1)}catch(error){tainted=error.name==='SecurityError'}bitmap.close();"
        "const empty="
        "document.createElement('img');let rejected=false;try{await empty.decode()}"
        "catch(error){rejected=error.name==='EncodingError'}"
        "globalThis.pocSummary=image.complete&&image.naturalWidth===1&&tainted"
        "&&rejected"
        "?'IMAGE-DECODE-OK':'IMAGE-DECODE-FAILED'})().catch(error=>"
        "globalThis.pocSummary='IMAGE-DECODE-ERROR:'+error)";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, image_decode_probe, "<image-decode-probe>", &result)
          && strcmp(result.summary, "IMAGE-DECODE-OK") == 0);
    static const char canvas_taint_probe[] =
        "(()=>{const image=document.getElementById('cross-origin-canvas-source'),"
        "canvas=document.createElement('canvas'),context=canvas.getContext('2d');"
        "canvas.width=1;canvas.height=1;context.drawImage(image,0,0);"
        "const denied=call=>{try{call();return false}catch(error){return "
        "error instanceof DOMException&&error.name==='SecurityError'}};"
        "const pixels=denied(()=>context.getImageData(0,0,1,1)),"
        "url=denied(()=>canvas.toDataURL()),blob=denied(()=>canvas.toBlob(()=>{}));"
        "const patterned=document.createElement('canvas'),"
        "patternContext=patterned.getContext('2d');patterned.width=1;"
        "patterned.height=1;patternContext.createPattern(image,'repeat');"
        "const pattern=denied(()=>patternContext.getImageData(0,0,1,1));"
        "const copied=document.createElement('canvas'),copy=copied.getContext('2d');"
        "copied.width=1;copied.height=1;copy.drawImage(canvas,0,0);"
        "const propagated=denied(()=>copy.getImageData(0,0,1,1));"
        "const webglSource=document.createElement('canvas'),webgl2d="
        "webglSource.getContext('2d');webglSource.width=1;webglSource.height=1;"
        "webgl2d.drawImage(image,0,0);globalThis.__taintedWebglSource=webglSource;"
        "canvas.width=1;const reset=context.getImageData(0,0,1,1).data[3]===0;"
        "globalThis.pocSummary=pixels&&url&&blob&&pattern&&propagated&&reset"
        "?'CANVAS-TAINT-OK':'CANVAS-TAINT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_taint_probe, "<canvas-taint-probe>", &result)
          && strcmp(result.summary, "CANVAS-TAINT-OK") == 0);

    /* The retained queue is capped at eight canvases.  Disconnect its first
       eight entries before their microtasks run, leaving a connected ninth
       entry outside the queue.  That ninth microtask must publish its own
       surface rather than assuming an earlier batch included it. */
    static const char canvas_ninth_commit_probe[] =
        "(()=>{const before=__tilefinchCanvasDiagnostics.surfaceCommits,"
        "canvases=[];for(let i=0;i<9;i++){const canvas="
        "document.createElement('canvas');canvas.width=1;canvas.height=1;"
        "document.body.appendChild(canvas);const context=canvas.getContext('2d');"
        "context.fillStyle='red';context.fillRect(0,0,1,1);canvases.push(canvas)}"
        "for(let i=0;i<8;i++)canvases[i].remove();globalThis.pocSummary="
        "'CANVAS-NINTH-COMMIT-PENDING';queueMicrotask(()=>{const committed="
        "__tilefinchCanvasDiagnostics.surfaceCommits-before;"
        "globalThis.pocSummary=committed===1?'CANVAS-NINTH-COMMIT-OK':"
        "'CANVAS-NINTH-COMMIT-FAILED:'+committed;canvases[8].remove()})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_ninth_commit_probe,
              "<canvas-ninth-commit-probe>", &result)
          && script_runtime_advance(runtime, 0, 32, &result)
          && result.success
          && strcmp(result.summary, "CANVAS-NINTH-COMMIT-OK") == 0);

    static const char webgl_basic_probe[] =
        "(()=>{const canvas=document.createElement('canvas');canvas.width=8;"
        "canvas.height=8;document.body.appendChild(canvas);const gl="
        "canvas.getContext('webgl'),same=gl===canvas.getContext('experimental-webgl'),"
        "exclusive=canvas.getContext('2d')===null,vs=gl.createShader(gl.VERTEX_SHADER),"
        "fs=gl.createShader(gl.FRAGMENT_SHADER);globalThis.__testWebgl=gl;"
        "const taintTexture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,taintTexture);"
        "let taintRejected=false;try{gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,"
        "gl.UNSIGNED_BYTE,__taintedWebglSource)}catch(error){taintRejected="
        "error.name==='SecurityError'}"
        "gl.shaderSource(vs,'attribute vec2 position;void main(){gl_Position='"
        "+'vec4(position,0.0,1.0);}');gl.compileShader(vs);"
        "gl.shaderSource(fs,'precision mediump float;uniform vec4 tint;void main()'"
        "+'{gl_FragColor=tint;}');gl.compileShader(fs);const program=gl.createProgram();"
        "gl.attachShader(program,vs);gl.attachShader(program,fs);gl.linkProgram(program);"
        "gl.useProgram(program);const buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buffer);"
        "gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,0,1]),gl.STATIC_DRAW);"
        "const location=gl.getAttribLocation(program,'position');gl.enableVertexAttribArray(location);"
        "gl.vertexAttribPointer(location,2,gl.FLOAT,false,0,0);"
        "gl.uniform4f(gl.getUniformLocation(program,'tint'),1,0,0,1);"
        "gl.clearColor(0,0,1,1);gl.clear(gl.COLOR_BUFFER_BIT);"
        "gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();const pixel=new Uint8Array(4);"
        "gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,pixel);"
        "const dvs=gl.createShader(gl.VERTEX_SHADER),dfs=gl.createShader(gl.FRAGMENT_SHADER),"
        "depthProgram=gl.createProgram();gl.shaderSource(dvs,'attribute vec3 position;'"
        "+'void main(){gl_Position=vec4(position,1.);}');gl.shaderSource(dfs,"
        "'precision mediump float;uniform vec4 tint;void main(){gl_FragColor=tint;}');"
        "gl.compileShader(dvs);gl.compileShader(dfs);gl.attachShader(depthProgram,dvs);"
        "gl.attachShader(depthProgram,dfs);gl.linkProgram(depthProgram);gl.useProgram(depthProgram);"
        "const depthBuffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,depthBuffer);"
        "gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,.5,1,-1,.5,0,1,.5,"
        "-1,-1,-.5,1,-1,-.5,0,1,-.5,-1,-1,.5,1,-1,.5,0,1,.5]),gl.STATIC_DRAW);"
        "const dp=gl.getAttribLocation(depthProgram,'position');gl.enableVertexAttribArray(dp);"
        "gl.vertexAttribPointer(dp,3,gl.FLOAT,false,0,0);const dt="
        "gl.getUniformLocation(depthProgram,'tint');gl.enable(gl.DEPTH_TEST);"
        "gl.clearColor(0,0,0,1);gl.clearDepth(1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);"
        "gl.uniform4f(dt,0,0,1,1);gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();"
        "gl.uniform4f(dt,1,0,0,1);gl.drawArrays(gl.TRIANGLES,3,3);gl.finish();"
        "gl.uniform4f(dt,0,1,0,1);gl.drawArrays(gl.TRIANGLES,6,3);gl.finish();"
        "const depthPixel=new Uint8Array(4);gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,depthPixel);"
        "gl.disable(gl.DEPTH_TEST);"
        "const texture=gl.createTexture();gl.bindTexture(gl.TEXTURE_2D,texture);"
        "gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.NEAREST);"
        "gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.NEAREST);"
        "gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,1,1,0,gl.RGBA,gl.UNSIGNED_BYTE,"
        "new Uint8Array([0,255,0,255]));const tvs=gl.createShader(gl.VERTEX_SHADER),"
        "tfs=gl.createShader(gl.FRAGMENT_SHADER),textured=gl.createProgram();"
        "gl.shaderSource(tvs,'attribute vec2 position;attribute vec2 texcoord;'"
        "+'varying vec2 uv;void main(){uv=texcoord;gl_Position=vec4(position,0.,1.);}');"
        "gl.shaderSource(tfs,'precision mediump float;varying vec2 uv;uniform sampler2D image;'"
        "+'void main(){gl_FragColor=texture2D(image,uv);}');gl.compileShader(tvs);"
        "gl.compileShader(tfs);gl.attachShader(textured,tvs);gl.attachShader(textured,tfs);"
        "gl.linkProgram(textured);gl.useProgram(textured);gl.bindBuffer(gl.ARRAY_BUFFER,buffer);"
        "const p2=gl.getAttribLocation(textured,'position');gl.enableVertexAttribArray(p2);"
        "gl.vertexAttribPointer(p2,2,gl.FLOAT,false,0,0);const uvBuffer=gl.createBuffer();"
        "gl.bindBuffer(gl.ARRAY_BUFFER,uvBuffer);gl.bufferData(gl.ARRAY_BUFFER,"
        "new Float32Array([0,0,1,0,.5,1]),gl.STATIC_DRAW);const uv="
        "gl.getAttribLocation(textured,'texcoord');gl.enableVertexAttribArray(uv);"
        "gl.vertexAttribPointer(uv,2,gl.FLOAT,false,0,0);gl.clear(gl.COLOR_BUFFER_BIT);"
        "gl.drawArrays(gl.TRIANGLES,0,3);gl.finish();const texturedPixel=new Uint8Array(4);"
        "gl.readPixels(4,4,1,1,gl.RGBA,gl.UNSIGNED_BYTE,texturedPixel);"
        "const rejectedVs=gl.createShader(gl.VERTEX_SHADER),rejectedFs="
        "gl.createShader(gl.FRAGMENT_SHADER),rejected=gl.createProgram();"
        "gl.shaderSource(rejectedVs,'attribute vec2 position;void main(){for(int i=0;i<2;i++){}'"
        "+'gl_Position=vec4(position,0.,1.);}');gl.compileShader(rejectedVs);"
        "gl.shaderSource(rejectedFs,'void main(){gl_FragColor=vec4(1.);}');"
        "gl.compileShader(rejectedFs);gl.attachShader(rejected,rejectedVs);"
        "gl.attachShader(rejected,rejectedFs);gl.linkProgram(rejected);"
        "const second=document.createElement('canvas').getContext('webgl'),"
        "third=document.createElement('canvas').getContext('webgl');"
        "const ok=gl&&same&&exclusive&&second&&third===null&&taintRejected"
        "&&gl.getContextAttributes().alpha===false"
        "&&gl.getProgramParameter(program,gl.LINK_STATUS)"
        "&&pixel[0]>240&&pixel[1]<8&&pixel[2]<8&&pixel[3]>240"
        "&&depthPixel[0]>240&&depthPixel[1]<8&&depthPixel[2]<8"
        "&&texturedPixel[0]<8&&texturedPixel[1]>240&&texturedPixel[2]<8"
        "&&canvas.toDataURL().startsWith('data:image/png;base64,')"
        "&&!gl.getProgramParameter(rejected,gl.LINK_STATUS)"
        "&&gl.getParameter(gl.MAX_TEXTURE_SIZE)===512;"
        "globalThis.pocSummary=ok?'WEBGL-BASIC-OK':'WEBGL-BASIC-FAILED:'"
        "+Array.from(pixel).join(',')+':'+gl.getProgramInfoLog(rejected);})()";
    bool webgl_basic_ok = script_runtime_evaluate_diagnostic(
        runtime, webgl_basic_probe, "<webgl-basic-probe>", &result);
    if (!webgl_basic_ok || strcmp(result.summary, "WEBGL-BASIC-OK") != 0) {
        fprintf(stderr, "WebGL basic probe: ok=%d summary=%s error=%s\n",
                webgl_basic_ok, result.summary, result.error);
    }
    CHECK(webgl_basic_ok && strcmp(result.summary, "WEBGL-BASIC-OK") == 0);

    static const char webgl_state_probe[] =
        "(()=>{const g=globalThis.__testWebgl,c=g.canvas,v="
        "g.createShader(g.VERTEX_SHADER),f=g.createShader(g.FRAGMENT_SHADER);"
        "g.shaderSource(v,'attribute vec2 p;void main(){gl_Position='"
        "+'vec4(p,0.,1.);}');g.shaderSource(f,'precision mediump float;'"
        "+'uniform vec4 color;void main(){gl_FragColor=color;}');"
        "g.compileShader(v);g.compileShader(f);const p=g.createProgram();"
        "g.attachShader(p,v);g.attachShader(p,f);g.linkProgram(p);g.useProgram(p);"
        "const b=g.createBuffer();g.bindBuffer(g.ARRAY_BUFFER,b);"
        "g.bufferData(g.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,0,1]),"
        "g.STATIC_DRAW);const a=g.getAttribLocation(p,'p');"
        "g.enableVertexAttribArray(a);g.vertexAttribPointer(a,2,g.FLOAT,false,0,0);"
        "const color=g.getUniformLocation(p,'color'),pixel=new Uint8Array(4);"
        "g.clearColor(0,0,1,1);g.clear(g.COLOR_BUFFER_BIT);g.enable(g.BLEND);"
        "g.uniform4f(color,1,0,0,.5);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const replace=pixel[0]>240&&pixel[1]<8&&pixel[2]<8&&pixel[3]>120;"
        "g.blendFunc(g.SRC_ALPHA,g.ONE_MINUS_SRC_ALPHA);"
        "g.uniform4f(color,0,1,0,.5);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const alpha=pixel[0]>110&&pixel[0]<145&&pixel[1]>110&&pixel[1]<145;"
        "g.disable(g.BLEND);g.clearColor(0,0,0,1);g.clear(g.COLOR_BUFFER_BIT);"
        "g.bufferData(g.ARRAY_BUFFER,new Float32Array([-.8,-.8,.8,-.8,.8,.8]),"
        "g.STATIC_DRAW);g.uniform4f(color,1,1,1,1);g.drawArrays(g.LINE_LOOP,0,3);"
        "g.finish();g.readPixels(4,4,1,1,g.RGBA,g.UNSIGNED_BYTE,pixel);"
        "const loop=pixel[0]>200;const frames=[];for(let i=0;i<5;i++)"
        "frames.push(g.createFramebuffer());const bounded=frames[4]===null;"
        "g.deleteFramebuffer(frames[0]);const reclaimed=!!g.createFramebuffer();"
        "while(g.getError()!==g.NO_ERROR){}g.blendFunc(g.DST_COLOR,g.ONE);"
        "const stateError=g.getError()===g.INVALID_ENUM&&!g.isContextLost();"
        "const emptyRead=new Uint8Array(0);g.readPixels(0,0,0,0,g.RGBA,"
        "g.UNSIGNED_BYTE,emptyRead);const zeroRead=g.getError()===g.NO_ERROR;"
        "g.blendFuncSeparate(g.ONE,g.ZERO,g.SRC_ALPHA,g.ONE_MINUS_SRC_ALPHA);"
        "const splitError=g.getError()===g.INVALID_OPERATION;"
        "c.width=640;c.height=480;const boundedSize=g.drawingBufferWidth===362"
        "&&g.drawingBufferHeight===272;g.clearColor(.25,.5,.75,1);"
        "g.clear(g.COLOR_BUFFER_BIT|g.DEPTH_BUFFER_BIT);g.finish();"
        "const largePixel=new Uint8Array(4);g.readPixels(181,136,1,1,g.RGBA,"
        "g.UNSIGNED_BYTE,largePixel);const largeSurface=!g.isContextLost()"
        "&&largePixel[0]>55&&largePixel[1]>115&&largePixel[2]>175;"
        "c.width=2147483647;c.height=0;const zeroBound="
        "g.drawingBufferWidth===480&&g.drawingBufferHeight===0"
        "&&!g.isContextLost();"
        "globalThis.pocSummary=replace&&alpha&&loop&&bounded&&reclaimed&&stateError"
        "&&zeroRead"
        "&&splitError"
        "&&boundedSize&&largeSurface&&zeroBound"
        "?'WEBGL-STATE-OK':'WEBGL-STATE-FAILED:'+Array.from(pixel).join(',');})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_state_probe, "<webgl-state-probe>", &result)
          && strcmp(result.summary, "WEBGL-STATE-OK") == 0);

    /* MDN's 3D tutorial composes projection and model-view mat4 uniforms in
       its vertex shader. The bounded backend combines that chain before
       sending one fixed-function transform to the native renderer. */
    static const char webgl_two_matrix_probe[] =
        "(()=>{const g=globalThis.__testWebgl,c=g.canvas;c.width=16;c.height=16;"
        "const v=g.createShader(g.VERTEX_SHADER),f=g.createShader(g.FRAGMENT_SHADER),"
        "p=g.createProgram();g.shaderSource(v,'attribute vec4 aVertexPosition;'"
        "+'uniform mat4 uModelViewMatrix;uniform mat4 uProjectionMatrix;'"
        "+'void main(void){gl_Position=uProjectionMatrix*uModelViewMatrix*'"
        "+'aVertexPosition;}');g.shaderSource(f,'precision mediump float;'"
        "+'uniform vec4 color;void main(void){gl_FragColor=color;}');"
        "g.compileShader(v);g.compileShader(f);g.attachShader(p,v);g.attachShader(p,f);"
        "g.linkProgram(p);g.useProgram(p);const b=g.createBuffer();"
        "g.bindBuffer(g.ARRAY_BUFFER,b);g.bufferData(g.ARRAY_BUFFER,new Float32Array("
        "[-.25,-.25,.25,-.25,0,.25]),g.STATIC_DRAW);const a="
        "g.getAttribLocation(p,'aVertexPosition');g.enableVertexAttribArray(a);"
        "g.vertexAttribPointer(a,2,g.FLOAT,false,0,0);const projection=new Float32Array("
        "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]),model=new Float32Array("
        "[1,0,0,0,0,1,0,0,0,0,1,0,.5,0,0,1]);"
        "g.uniformMatrix4fv(g.getUniformLocation(p,'uProjectionMatrix'),false,projection);"
        "g.uniformMatrix4fv(g.getUniformLocation(p,'uModelViewMatrix'),false,model);"
        "g.uniform4f(g.getUniformLocation(p,'color'),1,0,0,1);g.clearColor(0,0,0,1);"
        "g.clear(g.COLOR_BUFFER_BIT);g.drawArrays(g.TRIANGLES,0,3);g.finish();"
        "const moved=new Uint8Array(4),origin=new Uint8Array(4);"
        "g.readPixels(12,8,1,1,g.RGBA,g.UNSIGNED_BYTE,moved);"
        "g.readPixels(8,8,1,1,g.RGBA,g.UNSIGNED_BYTE,origin);"
        "globalThis.pocSummary=g.getProgramParameter(p,g.LINK_STATUS)&&moved[0]>240"
        "&&origin[0]<8?'WEBGL-TWO-MATRIX-OK':'WEBGL-TWO-MATRIX-FAILED:'"
        "+Array.from(moved).join(',')+':'+Array.from(origin).join(',');})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_two_matrix_probe, "<webgl-two-matrix-probe>",
              &result)
          && strcmp(result.summary, "WEBGL-TWO-MATRIX-OK") == 0);

    static const char webgl_lifetime_probe[] =
        "(()=>{const old=document.createElement('canvas');old.width=1;"
        "old.height=1;document.body.appendChild(old);const handle=old.__handle,"
        "lease=old.__tilefinchHandleLease,commands=new Float64Array(64),"
        "textures=new Float64Array(),hostile={valueOf(){old.remove();"
        "__tilefinchReleaseNodeWrapper(handle,lease);document.body.innerHTML="
        "'<canvas id=webgl-replacement width=1 height=1></canvas>';return 1;}};"
        "commands[0]=0;commands[1]=0x4000;commands[5]=1;"
        "const rendered=__tilefinchWebGLRender(handle,hostile,1,commands,[],textures);"
        "globalThis.pocSummary=!rendered&&document.getElementById('webgl-replacement')"
        "?'WEBGL-LIFETIME-OK':'WEBGL-LIFETIME-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, webgl_lifetime_probe, "<webgl-lifetime-probe>",
              &result)
          && strcmp(result.summary, "WEBGL-LIFETIME-OK") == 0);
    script_runtime_set_images(runtime, NULL);
    images_destroy(&canvas_commit_images);

    /* Direct packed-command natives are an untrusted boundary even though
       the authored bootstrap normally sends small finite coordinates. */
    static const char canvas_numeric_boundary_probe[] =
        "(()=>{const pixels=new Uint8ClampedArray(16),base="
        "[0,0,1,1,255,0,0,255,1,1],bad=[NaN,Infinity,Number.MAX_VALUE];"
        "let rejected=true;for(const value of bad){const command="
        "new Float64Array(base);command[0]=value;rejected="
        "rejected&&!__tilefinchCanvasRasterRectBatch(pixels,2,2,command);}"
        "const points=new Float64Array([0,0,1,0]),empty=new Float64Array();"
        "rejected=rejected&&!__tilefinchCanvasRasterPath(pixels,2,2,points,"
        "true,false,0,0,0,255,1,1,1,0,0,10,empty,0,"
        "new Float64Array([NaN,0,2,2]),empty,empty);"
        "const imageCommand=new Float64Array([0,1,1,0,0,1,1,0,0,1,1,0,1,"
        "Number.MAX_VALUE,1,0,0,1,0,0,0,0,2,2]);"
        "rejected=rejected&&!__tilefinchCanvasRasterImageBatch(pixels,2,2,"
        "[new Uint8ClampedArray(4)],imageCommand);"
        "rejected=rejected&&!__tilefinchCanvasRasterImage(pixels,2,2,"
        "new Uint8ClampedArray(4),1,1,0,0,1,1,0,0,1,1,false,1,1,"
        "new Float64Array([1,0,0,1,Number.MAX_VALUE,0]),"
        "new Float64Array([0,0,2,2]),empty);"
        "globalThis.pocSummary=rejected?'CANVAS-NUMERIC-BOUNDARY-OK':"
        "'CANVAS-NUMERIC-BOUNDARY-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_numeric_boundary_probe,
              "<canvas-numeric-boundary-probe>", &result)
          && strcmp(result.summary, "CANVAS-NUMERIC-BOUNDARY-OK") == 0);

    /* A hostile stroke can multiply glyph pixels by a 33x33 neighborhood.
       The native call must degrade within its fixed work allowance and return
       control to JavaScript instead of monopolizing the browser thread. */
    static const char canvas_native_work_bound_probe[] =
        "(()=>{const canvas=document.createElement('canvas');canvas.width=512;"
        "canvas.height=256;const context=canvas.getContext('2d');"
        "context.font='64px sans-serif';context.lineWidth=16;"
        "context.strokeText('W'.repeat(256),0,96);"
        "context.fillStyle='red';context.fillRect(0,0,1,1);"
        "globalThis.pocSummary=context.getImageData(0,0,1,1).data[3]>0"
        "?'CANVAS-NATIVE-WORK-BOUND-OK':'CANVAS-NATIVE-WORK-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_native_work_bound_probe,
              "<canvas-native-work-bound-probe>", &result)
          && strcmp(result.summary, "CANVAS-NATIVE-WORK-BOUND-OK") == 0);

    /* A batch is one native call and therefore gets one allowance.  Resetting
       the allowance for every command lets a bounded 64-command batch multiply
       the browser-thread stall even though every individual rectangle fits. */
    static const char canvas_native_batch_work_bound_probe[] =
        "(()=>{const pixels=new Uint8ClampedArray(512*256*4),"
        "commands=new Float64Array(64*10);"
        "for(let i=0;i<31;i++)commands.set([0,0,512,256,0,255,0,255,1,1],i*10);"
        "commands.set([0,0,512,255,0,255,0,255,1,1],31*10);"
        "commands.set([0,0,512,2,255,0,0,255,1,1],32*10);"
        "for(let i=33;i<64;i++)commands.set([0,0,1,1,255,0,0,255,1,1],i*10);"
        "const completed=__tilefinchCanvasRasterRectBatch(pixels,512,256,commands),"
        "bounded=pixels[0]===0&&pixels[1]===255"
        "&&pixels[512*4]===0&&pixels[512*4+1]===255,"
        "continued=__tilefinchCanvasRasterRect(pixels,512,256,0,0,1,1,"
        "255,0,0,255,1,1);globalThis.pocSummary=completed===2&&bounded&&continued"
        "&&pixels[0]===255&&pixels[3]===255"
        "?'CANVAS-NATIVE-BATCH-WORK-BOUND-OK':"
        "'CANVAS-NATIVE-BATCH-WORK-BOUND-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, canvas_native_batch_work_bound_probe,
              "<canvas-native-batch-work-bound-probe>", &result)
          && strcmp(result.summary,
                    "CANVAS-NATIVE-BATCH-WORK-BOUND-OK") == 0);

    static const char class_list_probe[] =
        "(()=>{const node=document.createElement('div');"
        "node.className='one two';const removed=!node.classList.toggle('two'),"
        "added=node.classList.toggle('three'),forced=node.classList.toggle('four',false);"
        "node.classList.add('five','six');node.classList.remove('one','five');"
        "const replaced=node.classList.replace('six','seven');"
        "globalThis.pocSummary=removed&&added&&!forced&&replaced"
        "&&node.classList.value==='three seven'&&node.classList.length===2"
        "&&node.classList.item(1)==='seven'"
        "?'CLASS-LIST-OK':'CLASS-LIST-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, class_list_probe, "<class-list-probe>", &result)
          && strcmp(result.summary, "CLASS-LIST-OK") == 0);

    static const char element_collection_probe[] =
        "(()=>{const host=document.createElement('section'),"
        "first=document.createElement('span'),"
        "second=document.createElement('span'),"
        "nested=document.createElement('em');"
        "host.innerHTML='<i></i>'.repeat(140);"
        "first.className='shared alpha';second.className='shared beta';"
        "nested.className='shared alpha';second.appendChild(nested);"
        "host.append(first,second);document.body.appendChild(host);"
        "const documentClass=document.getElementsByClassName('shared alpha'),"
        "elementClass=host.getElementsByClassName('shared'),"
        "elementTag=host.getElementsByTagName('span'),"
        "documentTag=document.getElementsByTagName('em');"
        "const ok=documentClass.length===2&&documentClass.item(0)===first"
        "&&documentClass.item(1)===nested&&elementClass.length===3"
        "&&elementTag.length===2&&documentTag.length===1"
        "&&document.getElementsByClassName('   ').length===0;host.remove();"
        "globalThis.pocSummary=ok?'ELEMENT-COLLECTION-OK':"
        "'ELEMENT-COLLECTION-FAILED';})()";
    bool element_collection_ok = script_runtime_evaluate_diagnostic(
        runtime, element_collection_probe,
        "<element-collection-probe>", &result);
    if (!element_collection_ok
        || strcmp(result.summary, "ELEMENT-COLLECTION-OK") != 0) {
        fprintf(stderr, "element collection probe: ok=%d summary=%s "
                "error=%s\n", element_collection_ok,
                result.summary, result.error);
    }
    CHECK(element_collection_ok
          && strcmp(result.summary, "ELEMENT-COLLECTION-OK") == 0);

    /* Exercise the logical request queue independently of libcurl timing.
       The fake native surface records launch order and returns inert IDs;
       completions are delivered through the same host callback used by the
       real scheduler.  This keeps the quota, FIFO, cancellation, and timeout
       assertions deterministic and makes teardown leaks visible to the
       budget check below. */
    static const char network_queue_probe[] =
        "(async()=>{const nativeFetch=globalThis.__tilefinchFetchAsync,"
        "nativeCancel=globalThis.__tilefinchCancelNetwork,starts=[],cancels=[];"
        "let nextNative=1000,deferNativeOnce=true;"
        "globalThis.__tilefinchFetchAsync=(method,url)=>{url=String(url);"
        "if(url.endsWith('/native-defer')&&deferNativeOnce){"
        "deferNativeOnce=false;return 0;}starts.push(url);return nextNative++;};"
        "globalThis.__tilefinchCancelNetwork=id=>{cancels.push(Number(id));"
        "return true;};const raw={status:200,url:'https://example.test/ok',"
        "contentType:'text/plain',headers:'content-type: text/plain\\n',"
        "body:'ok'};const requests=[];for(let i=0;i<12;i++)requests.push("
        "fetch('https://example.test/fifo/'+i).then(value=>value.text()));"
        "const waitingAbort=new AbortController,waitingCancelled=fetch("
        "'https://example.test/cancel-waiting',{signal:waitingAbort.signal})"
        ".then(()=>'',error=>error.name);waitingAbort.abort();"
        "let delivered=1000;while(delivered<1012){const end=nextNative;"
        "while(delivered<end)__tilefinchDeliverNetwork(delivered++,true,raw);"
        "await Promise.resolve();}const bodies=await Promise.all(requests),"
        "cancelName=await waitingCancelled;const fifo=starts.length===12"
        "&&starts.every((url,index)=>url==='https://example.test/fifo/'+index);"
        "const abortControllers=[],abortPromises=[],abortStart=starts.length;"
        "for(let i=0;i<5;i++){const controller=new AbortController;"
        "abortControllers.push(controller);abortPromises.push(fetch("
        "'https://example.test/abort-fifo/'+i,{signal:controller.signal})"
        ".catch(()=>{}));}abortControllers[0].abort();await Promise.resolve();"
        "const abortReleased=starts[abortStart+4]"
        "==='https://example.test/abort-fifo/4';"
        "for(const controller of abortControllers)controller.abort();"
        "await Promise.all(abortPromises);"
        "const controllers=[],quotaPromises=[];for(let i=0;i<129;i++){"
        "const controller=new AbortController;controllers.push(controller);"
        "quotaPromises.push(fetch('https://example.test/quota/'+i,{"
        "signal:controller.signal}).then(()=>'',error=>error.name));}"
        "await Promise.resolve();const countQuota=await quotaPromises[128];"
        "let insideXhrSend=true,xhrQuotaError=false,xhrQuotaReentered=false;"
        "const quotaXhr=new XMLHttpRequest;quotaXhr.open('GET',"
        "'https://example.test/quota-xhr');quotaXhr.onerror=()=>{"
        "xhrQuotaError=true;if(insideXhrSend)xhrQuotaReentered=true;};"
        "quotaXhr.send();insideXhrSend=false;const xhrDeferred="
        "!xhrQuotaError&&!xhrQuotaReentered;__tilefinchPumpTimers(0,4);"
        "for(const controller of controllers)controller.abort();"
        "await Promise.all(quotaPromises);const byteQuota=await fetch("
        "'https://example.test/byte-quota',{method:'POST',"
        "body:'x'.repeat(132000)}).then(()=>'',error=>error.name);"
        "const blockers=[];for(let i=0;i<4;i++){const controller="
        "new AbortController;blockers.push(controller);fetch("
        "'https://example.test/timeout-blocker/'+i,{signal:controller.signal})"
        ".catch(()=>{});}let timeoutEvent=false;const xhr=new XMLHttpRequest;"
        "xhr.open('GET','https://example.test/queued-timeout');xhr.timeout=5;"
        "xhr.ontimeout=()=>{timeoutEvent=true;};xhr.send();"
        "const queuedBeforeTimeout=__tilefinchNetworkQueueStats.waiting===1;"
        "__tilefinchPumpTimers(5,16);for(const controller of blockers)"
        "controller.abort();await Promise.resolve();const uploadEvents=[],"
        "uploadXhr=new XMLHttpRequest,recordUpload=label=>event=>uploadEvents."
        "push([label,event.isTrusted,event instanceof ProgressEvent,"
        "event.lengthComputable,event.loaded,event.total].join(':'));"
        "for(const type of ['loadstart','progress','load','loadend'])"
        "uploadXhr.upload.addEventListener(type,recordUpload('u-'+type));"
        "uploadXhr.addEventListener('loadend',recordUpload('x-loadend'));"
        "uploadXhr.open('POST','https://example.test/upload');"
        "uploadXhr.send('abcde');const uploadNative=nextNative-1;"
        "__tilefinchDeliverNetwork(uploadNative,true,raw,true,100,200,300,"
        "400,500,2,2,true,false,false,true,3,200,'text/plain');"
        "await Promise.resolve();"
        "const deferredPromise=fetch('https://example.test/native-defer')"
        ".then(value=>value.text());const nativeDeferred="
        "__tilefinchNetworkQueueStats.waiting===1;"
        "__tilefinchPumpTimers(20,16);const deferredNative=nextNative-1;"
        "__tilefinchDeliverNetwork(deferredNative,true,raw,true,110,210,310,"
        "410,510,2,2,true,false,false,true,2,200,'text/plain');"
        "const deferredBody=await deferredPromise;"
        "const xhrTiming=performance.getEntriesByName("
        "'https://example.test/upload','resource')[0],fetchTiming="
        "performance.getEntriesByName("
        "'https://example.test/native-defer','resource')[0],timingOk="
        "xhrTiming&&xhrTiming.initiatorType==='xmlhttprequest'&&"
        "xhrTiming.nextHopProtocol==='h2'&&xhrTiming.responseStatus===200&&"
        "xhrTiming.contentType==='text/plain'&&xhrTiming.encodedBodySize===2&&"
        "fetchTiming&&fetchTiming.initiatorType==='fetch'&&"
        "fetchTiming.nextHopProtocol==='http/1.1'&&"
        "fetchTiming.responseStatus===200&&fetchTiming.contentType==="
        "'text/plain'&&fetchTiming.encodedBodySize===2;"
        "const progressShape=new ProgressEvent('shape',{lengthComputable:true,"
        "loaded:3,total:5}),progressShapeOk=Object.getOwnPropertyNames("
        "progressShape).join(',')==='isTrusted'&&Object.getOwnPropertyNames("
        "ProgressEvent.prototype).join(',')==='lengthComputable,loaded,total,constructor'"
        "&&progressShape.lengthComputable&&progressShape.loaded===3"
        "&&progressShape.total===5;"
        "const uploadOk=uploadEvents.join('|')==="
        "'u-loadstart:true:true:true:0:5|u-progress:true:true:true:5:5|'"
        "+'u-load:true:true:true:5:5|u-loadend:true:true:true:5:5|'"
        "+'x-loadend:true:true:true:2:2';const stats="
        "__tilefinchNetworkQueueStats;globalThis.__tilefinchFetchAsync=nativeFetch;"
        "globalThis.__tilefinchCancelNetwork=nativeCancel;"
        "globalThis.pocSummary=fifo&&bodies.every(value=>value==='ok')"
        "&&cancelName==='AbortError'&&countQuota==='TypeError'"
        "&&xhrDeferred&&xhrQuotaError&&!xhrQuotaReentered"
        "&&abortReleased&&byteQuota==='TypeError'&&queuedBeforeTimeout&&timeoutEvent"
        "&&uploadOk&&progressShapeOk&&timingOk&&nativeDeferred&&deferredBody==='ok'"
        "&&xhr.readyState===4&&xhr.status===0&&stats.peakCount===128"
        "&&stats.rejected===3&&stats.rejectedBytes>=1"
        "&&stats.cancelled===139&&stats.timedOut===1"
        "&&stats.completed===14&&stats.launchFailed===0"
        "&&stats.active===0&&stats.waiting===0"
        "&&stats.currentCount===0&&cancels.length===13"
        "?'NETWORK-QUEUE-OK':'NETWORK-QUEUE-FAILED:'+JSON.stringify(stats);"
        "})().catch(error=>{globalThis.pocSummary='NETWORK-QUEUE-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool network_queue_ok = script_runtime_evaluate_diagnostic(
        runtime, network_queue_probe, "<network-queue-probe>", &result);
    for (size_t tick = 0; network_queue_ok && tick < 32
         && strncmp(result.summary, "NETWORK-QUEUE-", 14) != 0; tick++) {
        network_queue_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!network_queue_ok
        || strcmp(result.summary, "NETWORK-QUEUE-OK") != 0) {
        fprintf(stderr, "network queue probe: ok=%d summary=%s error=%s "
                "admitted=%zu completed=%zu rejected=%zu cancelled=%zu "
                "timedout=%zu peak=%zu peakbytes=%zu active=%zu pending=%zu\n",
                network_queue_ok, result.summary, result.error,
                result.async_network_logical_admitted,
                result.async_network_logical_completed,
                result.async_network_logical_rejected,
                result.async_network_logical_cancelled,
                result.async_network_logical_timed_out,
                result.async_network_logical_peak,
                result.async_network_logical_peak_bytes,
                result.async_network_active_native,
                result.async_network_pending_logical);
    }
    CHECK(network_queue_ok
          && strcmp(result.summary, "NETWORK-QUEUE-OK") == 0
          && result.async_network_logical_admitted == 153
          && result.async_network_logical_completed == 14
          && result.async_network_logical_rejected == 3
          && result.async_network_logical_cancelled == 139
          && result.async_network_logical_timed_out == 1
          && result.async_network_logical_peak == 128
          && result.async_network_logical_peak_bytes <= 256u * 1024u
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    static const char fetch_network_error_surface_probe[] =
        "(async()=>{const nativeFetch=globalThis.__tilefinchFetchAsync;"
        "globalThis.__tilefinchFetchAsync=()=>777;const pending=fetch("
        "'https://example.test/native-failure').then(()=>'',error=>"
        "error.name+':'+error.message);__tilefinchDeliverNetwork(777,false,"
        "'Failed to connect to private.example:443',true,0,0,0,0,700,0,0,"
        "true,false,false,false,0,0,'');const text=await pending,entry="
        "performance.getEntriesByName("
        "'https://example.test/native-failure','resource')[0];"
        "globalThis.__tilefinchFetchAsync=nativeFetch;globalThis.pocSummary="
        "text==='TypeError:Failed to fetch'&&entry&&entry.initiatorType==="
        "'fetch'&&entry.responseStatus===0&&entry.transferSize===0"
        "?'FETCH-NETWORK-ERROR-OK':"
        "'FETCH-NETWORK-ERROR-FAILED:'+text})().catch(error=>{"
        "globalThis.pocSummary='FETCH-NETWORK-ERROR-FAILED:'+String(error)});";
    bool fetch_network_error_surface_ok = script_runtime_evaluate_diagnostic(
        runtime, fetch_network_error_surface_probe,
        "<fetch-network-error-surface-probe>", &result);
    for (size_t tick = 0; fetch_network_error_surface_ok && tick < 8
         && strcmp(result.summary, "FETCH-NETWORK-ERROR-OK") != 0; tick++) {
        fetch_network_error_surface_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    CHECK(fetch_network_error_surface_ok
          && strcmp(result.summary, "FETCH-NETWORK-ERROR-OK") == 0);

    static const char abort_algorithm_isolation_probe[] =
        "(()=>{const controller=new AbortController,signal=controller.signal,"
        "order=[];"
        "const combined=AbortSignal.any([signal]),request=new Request('/abort',"
        "{signal}),target=new EventTarget;let calls=0;target.addEventListener("
        "'probe',()=>calls++,{signal});signal.addEventListener('abort',()=>{"
        "order.push('source');if(!combined.aborted||!request.signal.aborted)"
        "order.push('unsettled');target.dispatchEvent(new Event('probe'))});"
        "combined.addEventListener('abort',()=>order.push('combined'));"
        "request.signal.addEventListener('abort',()=>order.push('request'));"
        "const reason={why:'stop'};"
        "controller.abort(reason);target.dispatchEvent(new Event('probe'));"
        "globalThis.pocSummary=combined.aborted&&combined.reason===reason&&"
        "request.signal.aborted&&request.signal.reason===reason&&calls===0&&"
        "order.join(',')==='source,combined,request'?"
        "'ABORT-ALGORITHM-ISOLATION-OK':'ABORT-ALGORITHM-ISOLATION-FAILED:'+"
        "[combined.aborted,request.signal.aborted,calls,order].join(',')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, abort_algorithm_isolation_probe,
              "<abort-algorithm-isolation-probe>", &result)
          && strcmp(result.summary,
                    "ABORT-ALGORITHM-ISOLATION-OK") == 0);

    static const char abort_dependency_order_probe[] =
        "(()=>{const sourceController=new AbortController,source="
        "sourceController.signal,first=AbortSignal.any([source]),second="
        "AbortSignal.any([source]),nested=AbortSignal.any([first]),order=[];"
        "source.addEventListener('abort',()=>order.push('source'));first."
        "addEventListener('abort',()=>order.push('first'));second.addEventListener("
        "'abort',()=>order.push('second'));nested.addEventListener('abort',()=>"
        "order.push('nested'));sourceController.abort('graph');const graph="
        "order.join(',')==='source,first,second,nested',sourceController2=new "
        "AbortController,source2=sourceController2.signal,order2=[],first2="
        "AbortSignal.any([source2]),nested2=AbortSignal.any([first2]),second2="
        "AbortSignal.any([source2]);source2.addEventListener('abort',()=>order2.push("
        "'source'));first2.addEventListener('abort',()=>order2.push('first'));"
        "nested2.addEventListener('abort',()=>order2.push('nested'));second2."
        "addEventListener('abort',()=>order2.push('second'));sourceController2.abort();"
        "const flattened=order2.join(',')==='source,first,nested,second',quotaSource="
        "new AbortController,duplicates=[];let deduplicated=true;try{duplicates.push("
        "AbortSignal.any(Array(64).fill(quotaSource.signal)));duplicates.push("
        "AbortSignal.any(Array(64).fill(quotaSource.signal)));duplicates.push("
        "AbortSignal.any([quotaSource.signal]))}catch(error){deduplicated=false}const "
        "iterableController=new AbortController,iterable=AbortSignal.any((function*()"
        "{yield iterableController.signal;iterableController.abort('during-iteration')"
        "})()),iterableAbort=iterable.aborted&&iterable.reason==='during-iteration',"
        "reasonFirst=new AbortController,reasonSecond=new AbortController,competing="
        "AbortSignal.any((function*(){yield reasonFirst.signal;yield reasonSecond.signal;"
        "reasonSecond.abort('second');reasonFirst.abort('first')})()),firstReason="
        "competing.aborted&&competing.reason==='first';let trailingInvalid=false;const "
        "preAborted=new AbortController;preAborted.abort();try{AbortSignal.any(["
        "preAborted.signal,{}])}catch(error){trailingInvalid=error instanceof TypeError}"
        "const emptyRoot=new AbortController,empties=[];for(let i=0;i<64;i++)empties."
        "push(AbortSignal.any([]));const emptyCombined=AbortSignal.any(empties),"
        "emptyAndRoot=AbortSignal.any([emptyCombined,emptyRoot.signal]);emptyRoot.abort("
        "'real-root');const emptyRoots=emptyAndRoot.aborted&&emptyAndRoot.reason==="
        "'real-root',a=new AbortController,"
        "b=new AbortController,reentrant=[];a.signal.addEventListener('abort',()=>{"
        "reentrant.push('a-before');b.abort();reentrant.push('a-after')});b.signal."
        "addEventListener('abort',()=>reentrant.push('b'));a.abort();const sync="
        "reentrant.join(',')==='a-before,b,a-after';const cleanupSource=new "
        "AbortController,dependent=AbortSignal.any([cleanupSource.signal]),target="
        "new EventTarget;let dependentCalls=0;target.addEventListener('probe',()=>"
        "dependentCalls++,{signal:dependent});cleanupSource.signal.addEventListener("
        "'abort',()=>target.dispatchEvent(new Event('probe')));cleanupSource.abort();"
        "target.dispatchEvent(new Event('probe'));globalThis.pocSummary=graph&&flattened"
        "&&deduplicated&&iterableAbort&&firstReason&&trailingInvalid&&emptyRoots&&sync"
        "&&dependentCalls===1?'ABORT-DEPENDENCY-ORDER-OK':"
        "'ABORT-DEPENDENCY-ORDER-FAILED:'+order+'|'+order2+'|'+deduplicated+'|'"
        "+[iterableAbort,firstReason,trailingInvalid,emptyRoots]+'|'"
        "+reentrant+'|'+dependentCalls})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, abort_dependency_order_probe,
              "<abort-dependency-order-probe>", &result)
          && strcmp(result.summary, "ABORT-DEPENDENCY-ORDER-OK") == 0);

    static const char event_handler_order_probe[] =
        "(()=>{const node=document.createElement('button'),order=[],first=()=>"
        "order.push('first'),last=()=>order.push('last');node.addEventListener("
        "'click',first);node.onclick=()=>order.push('handler');node.addEventListener("
        "'click',last);node.dispatchEvent(new Event('click',{cancelable:true}));"
        "const initial=order.join(',')==='first,handler,last';order.length=0;"
        "node.onclick=()=>order.push('replacement');node.dispatchEvent(new Event("
        "'click'));const replacement=order.join(',')==='first,replacement,last';"
        "order.length=0;node.onclick=null;node.onclick=()=>order.push('readded');"
        "node.dispatchEvent(new Event('click'));const readded=order.join(',')==="
        "'first,last,readded';const stopped=document.createElement('button');"
        "let afterStop=0;stopped.onclick=event=>event.stopImmediatePropagation();"
        "stopped.addEventListener('click',()=>afterStop++);stopped.dispatchEvent("
        "new Event('click'));const documentEvent=new Event('click',"
        "{cancelable:true}),windowEvent=new Event('click',"
        "{cancelable:true});document.onclick=()=>false;"
        "window.onclick=()=>false;const documentCancelled="
        "!document.dispatchEvent(documentEvent)&&documentEvent.defaultPrevented,"
        "windowCancelled=!window.dispatchEvent(windowEvent)&&"
        "windowEvent.defaultPrevented;document.onclick=null;"
        "window.onclick=null;const markup=document.createElement('button'),"
        "markupOrder=[];globalThis.__markupOrder=markupOrder;markup.setAttribute("
        "'onclick',\"__markupOrder.push('markup')\");markup.addEventListener("
        "'click',()=>markupOrder.push('listener'));markup.dispatchEvent(new Event("
        "'click'));const markupFirst=markupOrder.join(',')==='markup,listener';"
        "markupOrder.length=0;markup.setAttribute('onclick',"
        "\"__markupOrder.push('changed')\");markup.dispatchEvent(new Event('click'));"
        "const markupChanged=markupOrder.join(',')==='changed,listener';"
        "const parsedHost=document.createElement('div');parsedHost.innerHTML="
        "'<button onclick=\"__markupOrder.push(\\'parsed\\')\"></button>';const "
        "parsed=parsedHost.firstChild;markupOrder.length=0;parsed.addEventListener("
        "'click',()=>markupOrder.push('listener'));parsed.dispatchEvent(new Event("
        "'click'));const parsedFirst=markupOrder.join(',')==='parsed,listener';"
        "const switched=document.createElement('button'),switchedOrder=[];"
        "globalThis.__switchedOrder=switchedOrder;switched.onclick=()=>"
        "switchedOrder.push('property');switched.addEventListener('click',()=>"
        "switchedOrder.push('listener'));switched.setAttribute('onclick',"
        "\"__switchedOrder.push('attribute')\");switched.dispatchEvent(new Event("
        "'click'));const propertyToMarkup=switchedOrder.join(',')==="
        "'attribute,listener';delete globalThis.__markupOrder;delete globalThis."
        "__switchedOrder;globalThis.pocSummary=initial&&replacement"
        "&&readded&&afterStop===0&&documentCancelled&&windowCancelled&&markupFirst"
        "&&markupChanged&&parsedFirst&&propertyToMarkup?"
        "'EVENT-HANDLER-ORDER-OK':'EVENT-HANDLER-ORDER-FAILED:'+"
        "[initial,replacement,readded,afterStop,documentCancelled,windowCancelled,"
        "markupFirst,markupChanged,parsedFirst,propertyToMarkup]"
        ".join(',')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, event_handler_order_probe,
              "<event-handler-order-probe>", &result)
          && strcmp(result.summary, "EVENT-HANDLER-ORDER-OK") == 0);

    char raw_text_response_bytes[] = {(char) 0xe9, '\0', 'A'};
    FetchResult raw_text_response = {0};
    raw_text_response.data = raw_text_response_bytes;
    raw_text_response.length = sizeof(raw_text_response_bytes);
    snprintf(raw_text_response.content_type,
             sizeof(raw_text_response.content_type), "%s",
             "text/plain;charset=iso-8859-1");
    JSValue raw_text_payload = JS_NewObject(runtime->context);
    CHECK(!JS_IsException(raw_text_payload));
    /* A textual XHR may bypass the intermediate ArrayBuffer, but malformed
       UTF-8 must stay byte-backed so the Encoding Standard replacement path
       remains authoritative. */
    CHECK(js_rt_script_set_response_body(
        runtime->context, raw_text_payload, &raw_text_response, true));
    JSValue raw_text_global = JS_GetGlobalObject(runtime->context);
    CHECK(JS_SetPropertyStr(runtime->context, raw_text_global,
                            "__tilefinchTestRawTextPayload",
                            raw_text_payload) >= 0);

    char direct_text_response_bytes[] = {
        (char) 0xef, (char) 0xbb, (char) 0xbf, 'o', 'k'
    };
    FetchResult direct_text_response = {0};
    direct_text_response.data = direct_text_response_bytes;
    direct_text_response.length = sizeof(direct_text_response_bytes);
    JSValue direct_text_payload = JS_NewObject(runtime->context);
    CHECK(!JS_IsException(direct_text_payload));
    CHECK(js_rt_script_set_response_body(
        runtime->context, direct_text_payload, &direct_text_response, true));
    CHECK(JS_SetPropertyStr(runtime->context, raw_text_global,
                            "__tilefinchTestDirectTextPayload",
                            direct_text_payload) >= 0);
    JS_FreeValue(runtime->context, raw_text_global);

    static const char request_body_content_type_probe[] =
        "(async()=>{const nativeFetch=globalThis.__tilefinchFetchAsync,seen=[];"
        "let nextId=1777;globalThis.__tilefinchFetchAsync=(method,url,body,type,headers,"
        "...rest)=>{seen.push([String(type),String(headers),String(rest[8])]);"
        "return nextId++;};"
        "const stringRequest=new Request('https://example.test/string',{"
        "method:'POST',body:'probe'}),paramsRequest=new Request("
        "'https://example.test/params',{method:'POST',body:new URLSearchParams("
        "[['a','b']])}),blobRequest=new Request('https://example.test/blob',{"
        "method:'POST',body:new Blob(['x'],{type:'application/x-probe'})}),"
        "originalBytes=new Uint8Array([1]),"
        "bytesRequest=new Request('https://example.test/bytes',{method:'POST',"
        "body:originalBytes}),explicitRequest=new Request("
        "'https://example.test/explicit',{method:'POST',body:'probe',headers:{"
        "'content-type':'application/custom'}}),form=new FormData(),"
        "oldBoundary='----tilefinch-form-boundary';form.append('field',new Blob("
        "['abc'],{type:'text/plain'}),'report.txt');form.append('line\\r\\nname',"
        "'hello\\r\\n--'+oldBoundary+'\\r\\nContent-Disposition: form-data; name=\\\"role\\\"\\r\\n\\r\\nadmin');"
        "const formRequest=new Request('https://example.test/form',{method:'POST',"
        "body:form}),formType=formRequest.headers.get('content-type'),"
        "formBoundary=formType.split('boundary=')[1],formText=await formRequest.text(),"
        "file=form.get('field');originalBytes[0]=9;"
        "bytesRequest._bodyBytesSnapshot=new Uint8Array([8]);"
        "const pending=fetch(stringRequest)"
        ".then(response=>response.text());"
        "__tilefinchDeliverNetwork(1777,true,{status:200,url:"
        "'https://example.test/string',contentType:'text/plain',headers:"
        "'content-type: text/plain\\n',body:'ok'});await pending;"
        "const headerPending=fetch('https://example.test/headers',{headers:{"
        "Accept:'application/json','If-None-Match':'\\\"tag\\\"',"
        "'If-Modified-Since':'Sun, 06 Nov 1994 08:49:37 GMT','X-Custom':'kept'}});"
        "__tilefinchDeliverNetwork(1778,true,{status:204,url:"
        "'https://example.test/headers',headers:'content-type: application/json\\n',"
        "body:''});const headerResponse=await headerPending;"
        "const rawPending=fetch('https://example.test/raw');"
        "__tilefinchDeliverNetwork(1779,true,Object.assign("
        "__tilefinchTestRawTextPayload,{status:200,url:'https://example.test/raw',"
        "headers:'content-type: text/plain;charset=iso-8859-1\\n'}));"
        "const rawBytes=new Uint8Array(await (await rawPending).arrayBuffer());"
        "const snapshotted=await bytesRequest.bytes();"
        "globalThis.__tilefinchFetchAsync=nativeFetch;const ok="
        "stringRequest.headers.get('content-type')==="
        "'text/plain;charset=UTF-8'&&paramsRequest.headers.get('content-type')"
        "==='application/x-www-form-urlencoded;charset=UTF-8'&&"
        "blobRequest.headers.get('content-type')==='application/x-probe'&&"
        "bytesRequest.headers.get('content-type')===null&&"
        "explicitRequest.headers.get('content-type')==='application/custom'&&"
        "snapshotted.length===1&&snapshotted[0]===1&&"
        "file instanceof File&&file.name==='report.txt'&&file.type==='text/plain'&&"
        "formBoundary&&formBoundary!==oldBoundary&&formText.includes(oldBoundary)&&"
        "formText.includes('name=\\\"line%0D%0Aname\\\"')&&"
        "headerResponse.status===204&&headerResponse.body===null&&"
        "rawBytes.join(',')==='233,0,65'&&"
        "__tilefinchTestRawTextPayload.body===undefined&&"
        "__tilefinchTestRawTextPayload.bodyLength===3&&"
        "__tilefinchTestDirectTextPayload.body==='ok'&&"
        "__tilefinchTestDirectTextPayload.bodyBytes===undefined&&"
        "__tilefinchTestDirectTextPayload.bodyLength===5&&seen.length===3&&"
        "seen[0][0]==='text/plain;charset=UTF-8'&&seen[0][1]===''&&"
        "seen[0][2]==='*/*'&&seen[1][2]==='application/json'&&"
        "seen[1][1].includes('if-none-match: \\\"tag\\\"')&&"
        "seen[1][1].includes('if-modified-since: Sun, 06 Nov 1994 08:49:37 GMT')&&"
        "seen[1][1].includes('x-custom: kept');"
        "globalThis.pocSummary=ok?'REQUEST-BODY-CONTENT-TYPE-OK':"
        "'REQUEST-BODY-CONTENT-TYPE-FAILED:'+JSON.stringify({seen,string:"
        "stringRequest.headers.get('content-type'),params:paramsRequest.headers"
        ".get('content-type'),blob:blobRequest.headers.get('content-type'),bytes:"
        "bytesRequest.headers.get('content-type'),explicit:explicitRequest.headers"
        ".get('content-type')});})().catch(error=>{globalThis.pocSummary="
        "'REQUEST-BODY-CONTENT-TYPE-ERROR:'+String(error&&error.stack||error)});";
    bool request_body_content_type_ok = script_runtime_evaluate_diagnostic(
        runtime, request_body_content_type_probe,
        "<request-body-content-type-probe>", &result);
    for (size_t tick = 0; request_body_content_type_ok && tick < 8
         && strcmp(result.summary, "REQUEST-BODY-CONTENT-TYPE-OK") != 0;
         tick++) {
        request_body_content_type_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!request_body_content_type_ok
        || strcmp(result.summary, "REQUEST-BODY-CONTENT-TYPE-OK") != 0) {
        fprintf(stderr, "request body content type probe: ok=%d summary=%s "
                "error=%s\n", request_body_content_type_ok,
                result.summary, result.error);
    }
    CHECK(request_body_content_type_ok
          && strcmp(result.summary, "REQUEST-BODY-CONTENT-TYPE-OK") == 0);

    static const char response_body_content_type_probe[] =
        "(async()=>{const params=new Response(new URLSearchParams([['a','b']])),"
        "string=new Response('plain'),blob=new Response(new Blob(['blob'],{type:"
        "'application/x-probe'})),explicit=new Response('override',{headers:{"
        "'content-type':'application/custom'}}),form=new FormData();form.append("
        "'field','value');const multipart=new Response(form),multipartType="
        "multipart.headers.get('content-type'),texts=await Promise.all([params.text(),"
        "string.text(),blob.text(),explicit.text(),multipart.text()]);"
        "globalThis.pocSummary=params.headers.get('content-type')==="
        "'application/x-www-form-urlencoded;charset=UTF-8'&&string.headers.get("
        "'content-type')==='text/plain;charset=UTF-8'&&blob.headers.get("
        "'content-type')==='application/x-probe'&&explicit.headers.get("
        "'content-type')==='application/custom'&&multipartType.startsWith("
        "'multipart/form-data; boundary=----tilefinch-')&&texts[0]==='a=b'&&"
        "texts[1]==='plain'&&texts[2]==='blob'&&texts[3]==='override'&&texts[4]."
        "includes('name=\"field\"')?'RESPONSE-BODY-CONTENT-TYPE-OK':"
        "'RESPONSE-BODY-CONTENT-TYPE-FAILED'})().catch(error=>{globalThis."
        "pocSummary='RESPONSE-BODY-CONTENT-TYPE-ERROR:'+String(error&&error.stack"
        "||error)});";
    bool response_body_content_type_ok = script_runtime_evaluate_diagnostic(
        runtime, response_body_content_type_probe,
        "<response-body-content-type-probe>", &result);
    for (size_t tick = 0; response_body_content_type_ok && tick < 16
         && strcmp(result.summary, "RESPONSE-BODY-CONTENT-TYPE-OK") != 0;
         tick++) {
        response_body_content_type_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    CHECK(response_body_content_type_ok
          && strcmp(result.summary, "RESPONSE-BODY-CONTENT-TYPE-OK") == 0);

    static const char request_validation_ownership_probe[] =
        "(async()=>{const failures=[],check=(init)=>{const source=new Request("
        "'https://example.test/source',{method:'POST',body:'retained'});let threw="
        "false;try{new Request(source,init)}catch(error){threw=error instanceof "
        "TypeError}failures.push(threw&&!source.bodyUsed&&!source.body.locked);return "
        "source};check({method:'GET'});check({mode:'invalid'});check({credentials:"
        "'invalid'});check({signal:{}});const inherited=new Request("
        "'https://example.test/inherit',{method:'POST',body:'retained'}),moved=new "
        "Request(inherited,{body:null}),text=await moved.text();globalThis.pocSummary="
        "failures.every(Boolean)&&inherited.bodyUsed&&text==='retained'?"
        "'REQUEST-VALIDATION-OWNERSHIP-OK':'REQUEST-VALIDATION-OWNERSHIP-FAILED:'+"
        "failures.join(',')+','+inherited.bodyUsed+','+text})().catch(error=>{"
        "globalThis.pocSummary='REQUEST-VALIDATION-OWNERSHIP-ERROR:'+String(error&&"
        "error.stack||error)});";
    bool request_validation_ownership_ok = script_runtime_evaluate_diagnostic(
        runtime, request_validation_ownership_probe,
        "<request-validation-ownership-probe>", &result);
    for (size_t tick = 0; request_validation_ownership_ok && tick < 16
         && strcmp(result.summary, "REQUEST-VALIDATION-OWNERSHIP-OK") != 0;
         tick++) {
        request_validation_ownership_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    CHECK(request_validation_ownership_ok
          && strcmp(result.summary, "REQUEST-VALIDATION-OWNERSHIP-OK") == 0);

    static const char request_signal_rollback_probe[] =
        "(()=>{const controller=new AbortController,bad={toString(){throw new "
        "Error('body')}};let failures=0;for(let at=0;at<128;at++)try{new Request("
        "'https://example.test/fail',{method:'POST',body:bad,signal:controller."
        "signal})}catch(error){if(error.message==='body')failures++}let valid=false;"
        "try{valid=new Request('https://example.test/valid',{signal:controller."
        "signal}).signal.aborted===false}catch(error){}globalThis.pocSummary="
        "failures===128&&valid?'REQUEST-SIGNAL-ROLLBACK-OK':"
        "'REQUEST-SIGNAL-ROLLBACK-FAILED:'+failures+','+valid})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, request_signal_rollback_probe,
              "<request-signal-rollback-probe>", &result)
          && strcmp(result.summary, "REQUEST-SIGNAL-ROLLBACK-OK") == 0);

    static const char body_consumption_probe[] =
        "(async()=>{const request=new Request('https://example.test/body',{"
        "method:'POST',body:'request-data'}),requestText=await request.text(),"
        "requestLocked=request.body.locked,response=new Response("
        "'response-data'),responseText=await response.text(),responseLocked="
        "response.body.locked,source=new Request('https://example.test/"
        "transfer',{method:'POST',body:'transfer'}),moved=new Request(source),"
        "movedText=await moved.text(),cloneSource=new Request('https://example.test/"
        "clone',{method:'POST',body:'clone'}),clone=cloneSource.clone(),cloneLeft="
        "await cloneSource.text(),cloneRight=await clone.text(),streamRequest=new "
        "Request('https://example.test/stream',{method:'POST',duplex:'half',body:new "
        "ReadableStream({start(c){c.enqueue(new Uint8Array([111,107]));c.close()}})}),"
        "streamText=await streamRequest.text(),bom=await new Response('\\uFEFFx').text(),"
        "surrogate=await new Response('\\ud800').text();let nonByte='';try{await new "
        "Response(new ReadableStream({start(c){c.enqueue('bad');c.close()}})).text()}"
        "catch(error){nonByte=error.name}globalThis.pocSummary=requestText==="
        "'request-data'&&request.bodyUsed&&requestLocked&&responseText==="
        "'response-data'&&response.bodyUsed&&responseLocked&&source.bodyUsed&&"
        "movedText==='transfer'&&cloneLeft==='clone'&&cloneRight==='clone'&&"
        "streamText==='ok'&&bom==='x'&&surrogate.charCodeAt(0)===65533&&"
        "nonByte==='TypeError'?'BODY-CONSUMPTION-OK':'BODY-CONSUMPTION-FAILED'})()"
        ".catch(error=>{globalThis.pocSummary='BODY-CONSUMPTION-ERROR:'+String("
        "error&&error.stack||error)});";
    bool body_consumption_ok = script_runtime_evaluate_diagnostic(
        runtime, body_consumption_probe, "<body-consumption-probe>", &result);
    for (size_t tick = 0; body_consumption_ok && tick < 16
         && strcmp(result.summary, "BODY-CONSUMPTION-OK") != 0; tick++) {
        body_consumption_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!body_consumption_ok
        || strcmp(result.summary, "BODY-CONSUMPTION-OK") != 0) {
        fprintf(stderr, "body consumption probe: ok=%d summary=%s error=%s\n",
                body_consumption_ok, result.summary, result.error);
    }
    CHECK(body_consumption_ok
          && strcmp(result.summary, "BODY-CONSUMPTION-OK") == 0);

    static const char buffered_response_stream_probe[] =
        "(async()=>{const bytes=new Uint8Array(5000),response=new Response(bytes),"
        "reader=response.body.getReader();let closed=false;reader.closed.then("
        "()=>closed=true);await Promise.resolve();const openBefore=!closed,first="
        "await reader.read();await Promise.resolve();const openBetween=!closed,"
        "second=await reader.read();await reader.closed;const end=await reader."
        "read(),empty=await new Response(new Uint8Array()).body.getReader().read(),"
        "cancelReader=new Response('cancel').body.getReader();await cancelReader."
        "cancel();await cancelReader.closed;globalThis.pocSummary=openBefore&&"
        "openBetween&&!first.done&&first.value.length===4096&&!second.done&&"
        "second.value.length===904&&end.done&&empty.done?"
        "'BUFFERED-RESPONSE-STREAM-OK':'BUFFERED-RESPONSE-STREAM-FAILED'})().catch("
        "error=>{globalThis.pocSummary='BUFFERED-RESPONSE-STREAM-ERROR:'+String("
        "error&&error.stack||error)});";
    bool buffered_response_stream_ok = script_runtime_evaluate_diagnostic(
        runtime, buffered_response_stream_probe,
        "<buffered-response-stream-probe>", &result);
    for (size_t tick = 0; buffered_response_stream_ok && tick < 16
         && strcmp(result.summary, "BUFFERED-RESPONSE-STREAM-OK") != 0; tick++) {
        buffered_response_stream_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!buffered_response_stream_ok
        || strcmp(result.summary, "BUFFERED-RESPONSE-STREAM-OK") != 0) {
        fprintf(stderr, "buffered response stream probe: ok=%d summary=%s error=%s\n",
                buffered_response_stream_ok, result.summary, result.error);
    }
    CHECK(buffered_response_stream_ok
          && strcmp(result.summary, "BUFFERED-RESPONSE-STREAM-OK") == 0);

    static const char response_clone_probe[] =
        "(async()=>{const run=async size=>{const bytes=new Uint8Array(size);for(let "
        "i=0;i<size;i++)bytes[i]=i&255;const response=new Response(bytes),clone="
        "response.clone(),left=new Uint8Array(await response.arrayBuffer()),right="
        "new Uint8Array(await clone.arrayBuffer());return left.length===size&&"
        "right.length===size&&left.every((value,index)=>value===(index&255))&&"
        "right.every((value,index)=>value===(index&255))};globalThis.pocSummary="
        "await run(1)&&await run(4097)?'RESPONSE-CLONE-OK':"
        "'RESPONSE-CLONE-FAILED'})().catch(error=>{globalThis.pocSummary="
        "'RESPONSE-CLONE-ERROR:'+String(error&&error.stack||error)});";
    bool response_clone_ok = script_runtime_evaluate_diagnostic(
        runtime, response_clone_probe, "<response-clone-probe>", &result);
    for (size_t tick = 0; response_clone_ok && tick < 32
         && strcmp(result.summary, "RESPONSE-CLONE-OK") != 0; tick++) {
        response_clone_ok = script_runtime_advance(runtime, 0, 2048, &result);
    }
    CHECK(response_clone_ok
          && strcmp(result.summary, "RESPONSE-CLONE-OK") == 0);

    static const char streaming_upload_abort_probe[] =
        "(async()=>{const nativeFetch=globalThis.__tilefinchFetchAsync,controller="
        "new AbortController,reason={stop:true};let nativeCalls=0,cancelled=false;"
        "globalThis.__tilefinchFetchAsync=()=>{nativeCalls++;return 991};const body="
        "new ReadableStream({pull(){},cancel(value){cancelled=value===reason}}),"
        "request=new Request('https://example.test/upload',{method:'POST',body,"
        "duplex:'half',signal:controller.signal}),pending=fetch(request).then(()=>"
        "null,error=>error);await Promise.resolve();controller.abort(reason);const "
        "outcome=await pending;globalThis.__tilefinchFetchAsync=nativeFetch;"
        "globalThis.pocSummary=outcome===reason&&cancelled&&!body.locked&&nativeCalls"
        "===0?'STREAM-UPLOAD-ABORT-OK':'STREAM-UPLOAD-ABORT-FAILED:'+"
        "[outcome===reason,cancelled,body.locked,nativeCalls].join(',')})().catch("
        "error=>{globalThis.pocSummary='STREAM-UPLOAD-ABORT-ERROR:'+String(error&&"
        "error.stack||error)});";
    bool streaming_upload_abort_ok = script_runtime_evaluate_diagnostic(
        runtime, streaming_upload_abort_probe,
        "<stream-upload-abort-probe>", &result);
    for (size_t tick = 0; streaming_upload_abort_ok && tick < 16
         && strcmp(result.summary, "STREAM-UPLOAD-ABORT-OK") != 0; tick++) {
        streaming_upload_abort_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!streaming_upload_abort_ok
        || strcmp(result.summary, "STREAM-UPLOAD-ABORT-OK") != 0) {
        fprintf(stderr, "stream upload abort probe: ok=%d summary=%s error=%s\n",
                streaming_upload_abort_ok, result.summary, result.error);
    }
    CHECK(streaming_upload_abort_ok
          && strcmp(result.summary, "STREAM-UPLOAD-ABORT-OK") == 0);

    static const char utf8_decoder_probe[] =
        "(()=>{const codes=value=>Array.from(value,char=>char.charCodeAt(0)),"
        "first=codes(new TextDecoder().decode(new Uint8Array([0xe2,0x41]))),"
        "partial=codes(new TextDecoder().decode(new Uint8Array([0xe2,0x82,0x41]))),"
        "scalar=codes(new TextDecoder().decode(new Uint8Array([0xed,0xa0,0x80]))),"
        "stream=new TextDecoder(),streamFirst=codes(stream.decode(new Uint8Array("
        "[0xe2,0x41]),{stream:true})),streamEnd=codes(stream.decode()),"
        "bomStream=new TextDecoder(),"
        "bomPrefix=bomStream.decode(new Uint8Array([65]),{stream:true}),bomFinal="
        "bomStream.decode(new Uint8Array([239,187,191,66])),url=codes("
        "new URLSearchParams('a=%E2%82A').get('a')),validBytes=new Uint8Array("
        "[0,65,226,130,172,240,159,152,128,0]),valid=new TextDecoder().decode("
        "validBytes.subarray(1,9)),params=new URLSearchParams("
        "'a=one'),entry=params.entries().next().value;entry[1]='changed';"
        "globalThis.pocSummary=first.join(',')==='65533,65'&&partial.join(',')==="
        "'65533,65'&&scalar.join(',')==='65533,65533,65533'&&streamFirst.join(',')"
        "==='65533,65'&&streamEnd.length===0&&bomPrefix==='A'&&"
        "bomFinal.length===2&&bomFinal.charCodeAt(0)===65279&&bomFinal[1]==='B'&&"
        "url.join(',')==='65533,65'&&"
        "valid==='A€😀'&&typeof __tilefinchDecodeUtf8Valid==='undefined'&&"
        "params.get('a')==='one'?'UTF8-DECODER-OK':'UTF8-DECODER-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, utf8_decoder_probe, "<utf8-decoder-probe>", &result)
          && strcmp(result.summary, "UTF8-DECODER-OK") == 0);

    /* Streamed chunks decode their complete prefix natively and carry an
       unfinished sequence; every split of valid text reassembles, and an
       invalid tail still yields its replacement characters (or throws, when
       fatal) in the call that received it. */
    static const char utf8_stream_probe[] =
        "(()=>{const text='\\ufeffA\\u20ac\\u00e9\\ud83d\\ude00B',"
        "bytes=new TextEncoder().encode(text),codes=value=>Array.from(value,"
        "char=>char.charCodeAt(0)).join(',');let splits=true;"
        "for(let a=0;a<=bytes.length;a++)for(let b=a;b<=bytes.length;b++){"
        "const d=new TextDecoder();const out=d.decode(bytes.subarray(0,a),"
        "{stream:true})+d.decode(bytes.subarray(a,b),{stream:true})+"
        "d.decode(bytes.subarray(b));if(out!==text.slice(1))splits=false}"
        "const kept=new TextDecoder('utf-8',{ignoreBOM:true});const withBom="
        "kept.decode(bytes.subarray(0,2),{stream:true})+kept.decode("
        "bytes.subarray(2));const bad=new TextDecoder(),badFirst=codes("
        "bad.decode(new Uint8Array([65,0xe0,0x80]),{stream:true})),"
        "badEnd=bad.decode();const cut=new TextDecoder(),cutFirst=cut.decode("
        "new Uint8Array([65,0xf0,0x9f]),{stream:true}),cutEnd=codes("
        "cut.decode());const fatal=new TextDecoder('utf-8',{fatal:true});let "
        "fatalSplit=fatal.decode(new Uint8Array([0xe2,0x82]),{stream:true})+"
        "fatal.decode(new Uint8Array([0xac]),{stream:true}),fatalError='';"
        "try{fatal.decode(new Uint8Array([0x41,0xed,0xa0]),{stream:true})}"
        "catch(error){fatalError=error.name}"
        "globalThis.pocSummary=splits&&withBom===text&&badFirst==="
        "'65,65533,65533'&&badEnd===''&&cutFirst==='A'&&cutEnd==='65533'&&"
        "fatalSplit==='\\u20ac'&&fatalError==='TypeError'?'UTF8-STREAM-OK':"
        "'UTF8-STREAM-FAILED:'+[splits,withBom===text,badFirst,badEnd,cutFirst,"
        "cutEnd,fatalSplit,fatalError].join('|')})()";
    bool utf8_stream_ok = script_runtime_evaluate_diagnostic(
        runtime, utf8_stream_probe, "<utf8-stream-probe>", &result);
    if (!utf8_stream_ok || strcmp(result.summary, "UTF8-STREAM-OK") != 0)
        fprintf(stderr, "utf8 stream probe: ok=%d summary=%s error=%s\n",
                utf8_stream_ok, result.summary, result.error);
    CHECK(utf8_stream_ok && strcmp(result.summary, "UTF8-STREAM-OK") == 0);

    static const char css_and_message_probe[] =
        "(()=>{const element=document.createElement('div');document.body.append("
        "element);element.style.display='none';element.style.display="
        "'definitely-invalid';const preserved=element.style.display;element.style."
        "setProperty('display','block','invalid-priority');const invalidPriority="
        "element.style.display;element.style.setProperty('display','none',"
        "'important');const priority=element.style.getPropertyPriority('display'),"
        "value=element.style.getPropertyValue('display');element.style.setProperty("
        "'flex','1 1 auto','important');const flexPriority=element.style."
        "getPropertyPriority('flex');element.style.setProperty('flex-grow','2','');"
        "const mixedFlexPriority=element.style.getPropertyPriority('flex');let "
        "multiple='',relative='',"
        "empty='',clone='';try{new CSSStyleSheet().insertRule('a{color:red} b{color:"
        "blue}')}catch(error){multiple=error.name}try{postMessage('x','/path')}catch("
        "error){relative=error.name}try{postMessage('x','')}catch(error){empty=error."
        "name}try{postMessage(()=>{},'https://other.test')}catch(error){clone=error."
        "name}element.remove();globalThis.pocSummary=preserved==='none'&&"
        "invalidPriority==='none'&&priority==='important'&&value==='none'&&"
        "flexPriority==='important'&&mixedFlexPriority===''&&"
        "multiple==='SyntaxError'&&relative==='SyntaxError'&&empty==='SyntaxError'&&"
        "clone==='DataCloneError'?'CSS-MESSAGE-OK':'CSS-MESSAGE-FAILED:'+"
        "[preserved,invalidPriority,priority,value,flexPriority,mixedFlexPriority,"
        "multiple,relative,empty,clone].join("
        "',')})()";
    bool css_and_message_ok = script_runtime_evaluate_diagnostic(
        runtime, css_and_message_probe, "<css-message-probe>", &result);
    if (!css_and_message_ok || strcmp(result.summary, "CSS-MESSAGE-OK") != 0)
        fprintf(stderr, "css/message probe: ok=%d summary=%s error=%s\n",
                css_and_message_ok, result.summary, result.error);
    CHECK(css_and_message_ok
          && strcmp(result.summary, "CSS-MESSAGE-OK") == 0);

    static const char local_blob_network_probe[] =
        "(()=>{globalThis.pocSummary='LOCAL-BLOB-PENDING';const stats="
        "__tilefinchNetworkQueueStats,before=[stats.admitted,stats.launched,"
        "stats.currentCount,stats.localBlobReads,stats.localBlobFailures],"
        "checks=[],type='application/x-tilefinch-local';"
        "const fetchBlob=new Blob([new Uint8Array([0,65,255])],{type}),"
        "fetchURL=URL.createObjectURL(fetchBlob),fetchJob=fetch(fetchURL)"
        ".then(response=>{checks.push(response.status===200,response.url==="
        "fetchURL,response.headers.get('content-type')===type,response.body!=="
        "null);return response.arrayBuffer()}).then(buffer=>checks.push("
        "new Uint8Array(buffer).join(',')==='0,65,255'));"
        "URL.revokeObjectURL(fetchURL);const order=[],xhrBlob="
        "new Blob(['local-xhr'],{type:'text/plain'}),xhrURL="
        "URL.createObjectURL(xhrBlob),xhrJob=new Promise(resolve=>{const xhr="
        "new XMLHttpRequest;xhr.onreadystatechange=event=>{order.push(xhr.readyState);"
        "checks.push(event.isTrusted,event.constructor===Event,"
        "!(event instanceof ProgressEvent))};xhr.onload=event=>{order.push(5);"
        "checks.push(event.isTrusted,event instanceof ProgressEvent)};"
        "xhr.onloadend=event=>{order.push(6);checks.push(event.isTrusted,"
        "event instanceof ProgressEvent,event.lengthComputable,"
        "event.loaded===9,event.total===9);"
        "checks.push(xhr.status===200,xhr.responseURL===xhrURL,"
        "xhr.getResponseHeader('content-type')==='text/plain',"
        "new TextDecoder().decode(xhr.response)==='local-xhr',"
        "order.join(',')==='1,9,2,3,4,5,6');resolve()};xhr.open('GET',xhrURL);"
        "xhr.responseType='arraybuffer';xhr.send();order.push(9);"
        "URL.revokeObjectURL(xhrURL)});const stressBlob=new Blob(['local']),"
        "stressURL=URL.createObjectURL(stressBlob),stress=[];"
        "for(let i=0;i<8;i++)stress.push(new Promise(resolve=>{const xhr="
        "new XMLHttpRequest;xhr.onload=()=>resolve(xhr.responseText==='local');"
        "xhr.onerror=()=>resolve(false);xhr.open('GET',stressURL);xhr.send()}));"
        "URL.revokeObjectURL(stressURL);const revokedJob=fetch(stressURL)"
        ".then(()=>false,error=>error instanceof TypeError);Promise.all("
        "[fetchJob,xhrJob,revokedJob,...stress]).then(values=>{checks.push("
        "values[2]===true,values.slice(3).every(Boolean),"
        "stats.admitted===before[0],stats.launched===before[1],"
        "stats.currentCount===before[2],stats.localBlobReads-before[3]===10,"
        "stats.localBlobFailures-before[4]===1);globalThis.pocSummary="
        "checks.every(Boolean)?'LOCAL-BLOB-OK':'LOCAL-BLOB-FAILED:'+"
        "checks.join(',')+':order='+order.join(',')}).catch(error=>"
        "globalThis.pocSummary='LOCAL-BLOB-ERROR:'+String(error&&error.stack||"
        "error));})()";
    bool local_blob_network_ok = script_runtime_evaluate_diagnostic(
        runtime, local_blob_network_probe, "<local-blob-network-probe>",
        &result);
    for (size_t tick = 0; local_blob_network_ok && tick < 32
         && strncmp(result.summary, "LOCAL-BLOB-", 11) != 0; tick++) {
        local_blob_network_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!local_blob_network_ok
        || strcmp(result.summary, "LOCAL-BLOB-OK") != 0) {
        fprintf(stderr, "local blob network probe: ok=%d summary=%s error=%s\n",
                local_blob_network_ok, result.summary, result.error);
    }
    CHECK(local_blob_network_ok
          && strcmp(result.summary, "LOCAL-BLOB-OK") == 0
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    static const char xhr_reopen_probe[] =
        "(()=>{globalThis.pocSummary='XHR-REOPEN-PENDING';const oldURL="
        "URL.createObjectURL(new Blob(['old-response'])),newURL="
        "URL.createObjectURL(new Blob(['new-response'])),run=targetState=>"
        "new Promise(resolve=>{const xhr=new XMLHttpRequest;let reopened=false;"
        "xhr.onreadystatechange=()=>{if(!reopened&&xhr.readyState==="
        "targetState){reopened=true;xhr.open('GET',newURL);xhr.send()}};"
        "xhr.onloadend=()=>{if(reopened)resolve(xhr.readyState===4&&"
        "xhr.status===200&&xhr.responseText==='new-response')};"
        "xhr.open('GET',oldURL);xhr.send()});Promise.all([run(2),run(4)])"
        ".then(values=>{URL.revokeObjectURL(oldURL);URL.revokeObjectURL(newURL);"
        "globalThis.pocSummary=values.every(Boolean)?'XHR-REOPEN-OK':"
        "'XHR-REOPEN-FAILED:'+values.join(',')}).catch(error=>{"
        "globalThis.pocSummary='XHR-REOPEN-ERROR:'+String(error&&error.stack||"
        "error)})})()";
    bool xhr_reopen_ok = script_runtime_evaluate_diagnostic(
        runtime, xhr_reopen_probe, "<xhr-reopen-probe>", &result);
    for (size_t tick = 0; xhr_reopen_ok && tick < 16
         && strcmp(result.summary, "XHR-REOPEN-PENDING") == 0; tick++) {
        xhr_reopen_ok = script_runtime_advance(runtime, 0, 128, &result);
    }
    if (!xhr_reopen_ok || strcmp(result.summary, "XHR-REOPEN-OK") != 0) {
        fprintf(stderr, "xhr reopen probe: ok=%d summary=%s error=%s\n",
                xhr_reopen_ok, result.summary, result.error);
    }
    CHECK(xhr_reopen_ok && strcmp(result.summary, "XHR-REOPEN-OK") == 0);

    static const char xhr_reopen_timeout_probe[] =
        "(()=>{const nativeFetch=globalThis.__tilefinchFetchAsync,nativeCancel="
        "globalThis.__tilefinchCancelNetwork;let nextNative=9100;const cancels=[];"
        "globalThis.__tilefinchFetchAsync=()=>nextNative++;"
        "globalThis.__tilefinchCancelNetwork=id=>{cancels.push(Number(id));"
        "return true};const xhr=new XMLHttpRequest;xhr.open('GET',"
        "'https://example.test/old-timeout');xhr.timeout=5;xhr.send();"
        "xhr.open('GET','https://example.test/replacement');xhr.timeout=0;"
        "xhr.send();__tilefinchPumpTimers(10,16);const raw={status:200,url:"
        "'https://example.test/replacement',contentType:'text/plain',headers:"
        "'content-type: text/plain\\n',body:'replacement'};"
        "__tilefinchDeliverNetwork(9101,true,raw);__tilefinchPumpTimers(10,16);"
        "globalThis.__tilefinchFetchAsync=nativeFetch;"
        "globalThis.__tilefinchCancelNetwork=nativeCancel;globalThis.pocSummary="
        "xhr.readyState===4&&xhr.status===200&&xhr.responseText==='replacement'"
        "&&cancels.join(',')==='9100'?'XHR-REOPEN-TIMEOUT-OK':"
        "'XHR-REOPEN-TIMEOUT-FAILED:'+xhr.readyState+':'+xhr.status+':' +"
        "cancels.join(',')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, xhr_reopen_timeout_probe,
              "<xhr-reopen-timeout-probe>", &result)
          && strcmp(result.summary, "XHR-REOPEN-TIMEOUT-OK") == 0);

    static const char worker_network_location_probe[] =
        "(()=>{globalThis.pocSummary='WORKER-LOCATION-PENDING';const nativeFetch="
        "globalThis.__tilefinchFetchAsync,workerURL='https://example.test/path/"
        "script.js?mode=1#part';globalThis.__tilefinchFetchAsync=()=>9700;const "
        "worker=new Worker(workerURL);worker.onmessage=event=>{globalThis."
        "pocSummary=event.data;worker.terminate();globalThis.__tilefinchFetchAsync="
        "nativeFetch};__tilefinchPumpTimers(0,8);__tilefinchDeliverNetwork(9700,"
        "true,{status:200,url:workerURL,contentType:'text/javascript',headers:"
        "'content-type: text/javascript\\n',body:\"postMessage([location.href,"
        "location.origin,location.protocol,location.host,location.hostname,"
        "location.port,location.pathname,location.search,location.hash,String("
        "location)].join('|'))\"})})()";
    bool worker_network_location_ok = script_runtime_evaluate_diagnostic(
        runtime, worker_network_location_probe,
        "<worker-network-location-probe>", &result);
    for (size_t tick = 0; worker_network_location_ok && tick < 16
         && strcmp(result.summary, "WORKER-LOCATION-PENDING") == 0; tick++) {
        worker_network_location_ok = script_runtime_advance(
            runtime, 0, 128, &result);
    }
    static const char expected_worker_network_location[] =
        "https://example.test/path/script.js?mode=1#part|"
        "https://example.test|https:|example.test|example.test||"
        "/path/script.js|?mode=1|#part|"
        "https://example.test/path/script.js?mode=1#part";
    if (!worker_network_location_ok
        || strcmp(result.summary, expected_worker_network_location) != 0) {
        fprintf(stderr, "worker network location probe: ok=%d summary=%s "
                "error=%s\n", worker_network_location_ok,
                result.summary, result.error);
    }
    CHECK(worker_network_location_ok
          && strcmp(result.summary, expected_worker_network_location) == 0);

    static const char xhr_event_target_probe[] =
        "(()=>{const xhr=new XMLHttpRequest,calls=[],controller="
        "new AbortController,first=()=>calls.push('first');"
        "xhr.addEventListener('load',first);xhr.onload=()=>calls.push('old');"
        "xhr.addEventListener('load',{handleEvent(){calls.push('object')}},"
        "{once:true});xhr.onload=()=>calls.push('handler');"
        "xhr.addEventListener('load',()=>calls.push('aborted'),{signal:"
        "controller.signal});xhr.addEventListener('load',()=>calls.push("
        "'capture'),{capture:true});controller.abort();"
        "xhr.dispatchEvent(new Event('load'));xhr.dispatchEvent(new Event("
        "'load'));globalThis.pocSummary=calls.join(',')==="
        "'capture,first,handler,object,capture,first,handler'"
        "?'XHR-EVENT-TARGET-OK':'XHR-EVENT-TARGET-FAILED:'+calls.join(',')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, xhr_event_target_probe,
              "<xhr-event-target-probe>", &result)
          && strcmp(result.summary, "XHR-EVENT-TARGET-OK") == 0);

    static const char large_base64_probe[] =
        "(()=>{const encoded='A'.repeat(823120),decoded=atob(encoded);"
        "globalThis.pocSummary=decoded.length===617340"
        "&&decoded.charCodeAt(0)===0"
        "&&decoded.charCodeAt(decoded.length-1)===0"
        "?'LARGE-BASE64-OK':'LARGE-BASE64-FAILED:'+decoded.length})()";
    bool large_base64_ok = script_runtime_evaluate_diagnostic(
        runtime, large_base64_probe, "<large-base64-probe>", &result);
    if (!large_base64_ok
        || strcmp(result.summary, "LARGE-BASE64-OK") != 0) {
        fprintf(stderr, "large base64 probe: ok=%d summary=%s error=%s\n",
                large_base64_ok, result.summary, result.error);
    }
    CHECK(large_base64_ok
          && strcmp(result.summary, "LARGE-BASE64-OK") == 0);

    static const char indexeddb_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);"
        "tx.addEventListener('abort',()=>reject(tx.error));}),"
        "binarySource=new Uint8Array([7,8]),arraySource=[1],binaryRange="
        "IDBKeyRange.only(binarySource),arrayRange=IDBKeyRange.only(arraySource);"
        "binarySource[0]=9;arraySource[0]=9;let invalidCmp='',invalidRange='',"
        "emptyOpenRange='';try{indexedDB.cmp(NaN,0)}catch(error){invalidCmp="
        "error.name}try{IDBKeyRange.only(undefined)}catch(error){invalidRange="
        "error.name}try{IDBKeyRange.bound(1,1,true,false)}catch(error){"
        "emptyOpenRange=error.name}const keyContract=indexedDB.cmp(-Infinity,"
        "Infinity)<0&&indexedDB.cmp(0,new Date(0))<0&&indexedDB.cmp(new Date(0),"
        "'0')<0&&indexedDB.cmp('0',new Uint8Array([0]))<0&&indexedDB.cmp("
        "new Uint8Array([0]),[])<0&&indexedDB.cmp(new Uint8Array([1,2]),"
        "new Uint8Array([1,3]))<0&&binaryRange.includes(new Uint8Array([7,8]))"
        "&&!binaryRange.includes(binarySource)&&arrayRange.includes([1])"
        "&&!arrayRange.includes(arraySource)&&invalidCmp==='DataError'"
        "&&invalidRange==='DataError'&&emptyOpenRange==='DataError';"
        "const opening=indexedDB.open('tilefinch-probe',2);let upgraded=false,"
        "pendingResult='',pendingError='',closeEvents=0;try{void opening.result}"
        "catch(error){pendingResult=error.name}try{void opening.error}catch(error)"
        "{pendingError=error.name}"
        "opening.addEventListener('upgradeneeded',event=>{upgraded="
        "event.oldVersion===0&&event.newVersion===2&&opening.readyState==='done'"
        "&&opening.result instanceof IDBDatabase&&opening.transaction instanceof "
        "IDBTransaction;const store="
        "opening.result.createObjectStore('items',{keyPath:'id'});"
        "store.createIndex('tags','tags',{multiEntry:true});});"
        "const db=await request(opening);db.addEventListener('close',()=>"
        "closeEvents++);const write=db.transaction('items',"
        "'readwrite'),store=write.objectStore('items'),writeDone="
        "finished(write);const payload={map:new Map([['k',3]]),set:new Set("
        "[4]),pattern:/a+/gi,blob:new Blob(['b']),file:new File(['f'],'f.txt'),"
        "error:new TypeError('x'),boxed:new Number(7),big:BigInt(9)};"
        "await Promise.all([request(store.put({id:2,"
        "name:'two',tags:['even','all'],payload})),request(store.put({id:1,"
        "name:'one',tags:['odd','all']})),request(store.put({id:[new Date(0)],"
        "name:'date-key'})),request(store.put({id:['1970-01-01T00:00:00.000Z'],"
        "name:'string-key'})),request(store.put({id:new Uint8Array([7,8]),"
        "name:'binary-key'})),writeDone]);"
        "const read=db.transaction('items','readonly'),readDone="
        "finished(read),one=await request(read.objectStore('items').get(1)),"
        "all=await request(read.objectStore('items').getAll()),"
        "even=await request(read.objectStore('items').index('tags').getAll("
        "IDBKeyRange.only('even'))),dateKey=await request(read.objectStore("
        "'items').get([new Date(0)])),stringKey=await request(read.objectStore("
        "'items').get(['1970-01-01T00:00:00.000Z'])),binaryKey=await request("
        "read.objectStore('items').get(new Uint8Array([7,8])));await readDone;"
        "const cursorTx="
        "db.transaction('items'),cursorDone=finished(cursorTx),cursorRequest="
        "cursorTx.objectStore('items').index('tags').openCursor(),seen=[];"
        "let cursor=await request(cursorRequest);while(cursor){seen.push("
        "cursor.key+':'+cursor.primaryKey);cursor.continue();cursor=await "
        "request(cursor.request);}await cursorDone;db.close();"
        "const reopened=await request(indexedDB.open('tilefinch-probe')),"
        "againTx=reopened.transaction('items'),againDone=finished(againTx),"
        "again=await request(againTx.objectStore('items').get(2));"
        "await againDone;reopened.close();await request(indexedDB.deleteDatabase("
        "'tilefinch-probe'));const telemetry=__tilefinchIndexedDBStats;"
        "globalThis.pocSummary=keyContract&&upgraded"
        "&&pendingResult==='InvalidStateError'&&pendingError==='InvalidStateError'"
        "&&closeEvents===0&&db instanceof IDBDatabase"
        "&&opening instanceof IDBOpenDBRequest&&store instanceof IDBObjectStore"
        "&&one.name==='one'&&all.length===5&&dateKey.name==='date-key'"
        "&&stringKey.name==='string-key'&&binaryKey.name==='binary-key'"
        "&&even.length===1&&even[0].id===2&&again.name==='two'"
        "&&again.payload.map.get('k')===3&&again.payload.set.has(4)"
        "&&again.payload.pattern.source==='a+'&&again.payload.pattern.flags==="
        "'gi'&&again.payload.blob.size===1&&again.payload.file.name==='f.txt'"
        "&&again.payload.error.name==='TypeError'&&again.payload.boxed.valueOf()"
        "===7&&again.payload.big===BigInt(9)"
        "&&seen.join(',')==='all:1,all:2,even:2,odd:1'"
        "&&telemetry.records===0&&telemetry.bytes===0&&telemetry.opens===2"
        "&&telemetry.deletes===1?'INDEXEDDB-OK':'INDEXEDDB-FAILED:'+"
        "JSON.stringify(telemetry);})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_probe, "<indexeddb-probe>", &result);
    for (size_t tick = 0; indexeddb_ok && tick < 16
         && strncmp(result.summary, "INDEXEDDB-", 10) != 0; tick++) {
        indexeddb_ok = script_runtime_advance(runtime, 0, 1024, &result);
    }
    if (!indexeddb_ok || strcmp(result.summary, "INDEXEDDB-OK") != 0) {
        fprintf(stderr, "indexeddb probe: ok=%d summary=%s error=%s\n",
                indexeddb_ok, result.summary, result.error);
    }
    CHECK(indexeddb_ok && strcmp(result.summary, "INDEXEDDB-OK") == 0
          && result.indexed_db_opens == 2
          && result.indexed_db_deletes == 1
          && result.indexed_db_transactions == 5
          && result.indexed_db_requests == 13
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0
          && result.indexed_db_peak_bytes > 0
          && result.indexed_db_quota_errors == 0
          /* The same realm loaded Game Audio, Canvas, Streams, and the OPFS
             constructors earlier; IndexedDB is the next deferred standards
             module admitted. */
          /* Cumulative for this runtime: the compatibility probe above also
             loads the Worker module for its close() check, and the Intl
             probes load the lazy Intl module. */
          && result.bootstrap_lazy_module_loads == 8
          && result.bootstrap_lazy_module_failures == 0);

    static const char indexeddb_generator_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{tx.addEventListener("
        "'complete',resolve);tx.addEventListener('abort',()=>reject(tx.error));}),"
        "name='tilefinch-idb-generator',opening=indexedDB.open(name,1);"
        "opening.addEventListener('upgradeneeded',()=>{opening.result."
        "createObjectStore('inline',{keyPath:'id',autoIncrement:true});"
        "opening.result.createObjectStore('external',{autoIncrement:true})});"
        "const db=await request(opening),write=db.transaction(['inline',"
        "'external'],'readwrite'),writeDone=finished(write),inline=write."
        "objectStore('inline'),external=write.objectStore('external'),"
        "inlineKey=await request(inline.add({name:'first'})),explicitKey="
        "await request(external.put('explicit',1)),generatedKey=await request("
        "external.put('automatic'));await writeDone;const read=db.transaction("
        "['inline','external']),readDone=finished(read),inlineValue=await request("
        "read.objectStore('inline').get(1)),externalValues=await request(read."
        "objectStore('external').getAll());await readDone;db.close();await request("
        "indexedDB.deleteDatabase(name));globalThis.pocSummary=inlineKey===1"
        "&&inlineValue.id===1&&inlineValue.name==='first'&&explicitKey===1"
        "&&generatedKey===2&&externalValues.join(',')==='explicit,automatic'"
        "?'INDEXEDDB-GENERATOR-OK':'INDEXEDDB-GENERATOR-FAILED:'+JSON.stringify("
        "{inlineKey,inlineValue,explicitKey,generatedKey,externalValues})})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-GENERATOR-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool indexeddb_generator_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_generator_probe, "<indexeddb-generator-probe>",
        &result);
    for (size_t tick = 0; indexeddb_generator_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-GENERATOR-", 20) != 0; tick++) {
        indexeddb_generator_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_generator_ok
        || strcmp(result.summary, "INDEXEDDB-GENERATOR-OK") != 0) {
        fprintf(stderr, "indexeddb generator probe: ok=%d summary=%s error=%s\n",
                indexeddb_generator_ok, result.summary, result.error);
    }
    CHECK(indexeddb_generator_ok
          && strcmp(result.summary, "INDEXEDDB-GENERATOR-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_success_exception_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),name="
        "'tilefinch-idb-success-exception',opening=indexedDB.open(name,1);"
        "opening.addEventListener('upgradeneeded',()=>opening.result."
        "createObjectStore('records'));const db=await request(opening),"
        "write=db.transaction('records','readwrite'),outcome=new Promise(resolve"
        "=>{write.addEventListener('abort',()=>resolve('abort'));write."
        "addEventListener('complete',()=>resolve('complete'))}),put=write."
        "objectStore('records').put('must-rollback',1);put.addEventListener("
        "'success',()=>{throw new Error('success handler failed')});const state="
        "await outcome,read=db.transaction('records'),readDone=new Promise("
        "(resolve,reject)=>{read.addEventListener('complete',resolve);read."
        "addEventListener('abort',()=>reject(read.error))}),saved=await request("
        "read.objectStore('records').get(1));await readDone;db.close();await request("
        "indexedDB.deleteDatabase(name));globalThis.pocSummary=state==='abort'"
        "&&saved===undefined?'INDEXEDDB-SUCCESS-EXCEPTION-OK':"
        "'INDEXEDDB-SUCCESS-EXCEPTION-FAILED:'+state+':'+String(saved)})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-SUCCESS-EXCEPTION-ERROR:'"
        "+String(error&&error.stack||error)});";
    bool indexeddb_success_exception_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_success_exception_probe,
        "<indexeddb-success-exception-probe>", &result);
    for (size_t tick = 0; indexeddb_success_exception_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-SUCCESS-EXCEPTION-", 28) != 0;
         tick++) {
        indexeddb_success_exception_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_success_exception_ok
        || strcmp(result.summary, "INDEXEDDB-SUCCESS-EXCEPTION-OK") != 0) {
        fprintf(stderr,
                "indexeddb success exception probe: ok=%d summary=%s error=%s\n",
                indexeddb_success_exception_ok, result.summary, result.error);
    }
    CHECK(indexeddb_success_exception_ok
          && strcmp(result.summary, "INDEXEDDB-SUCCESS-EXCEPTION-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_query_upgrade_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));req."
        "addEventListener('error',()=>reject(req.error));}),finished=tx=>new "
        "Promise((resolve,reject)=>{tx.addEventListener('complete',resolve);"
        "tx.addEventListener('abort',()=>reject(tx.error));}),queryName="
        "'tilefinch-idb-query-snapshot',queryOpen=indexedDB.open(queryName,1);"
        "queryOpen.addEventListener('upgradeneeded',()=>queryOpen.result."
        "createObjectStore('records'));const queryDb=await request(queryOpen),"
        "seed=queryDb.transaction('records','readwrite'),seedDone=finished(seed),"
        "seedStore=seed.objectStore('records');await Promise.all([request("
        "seedStore.put('one',new Uint8Array([1]))),request(seedStore.put('two',"
        "new Uint8Array([2]))),seedDone]);const mutation=queryDb.transaction("
        "'records','readwrite'),mutationDone=finished(mutation),mutationStore="
        "mutation.objectStore('records'),getKey=new Uint8Array([1]),getRequest="
        "request(mutationStore.get(getKey));getKey[0]=2;const deleteKey=new "
        "Uint8Array([1]),deleteRequest=request(mutationStore.delete(deleteKey));"
        "deleteKey[0]=2;const original=await getRequest;await deleteRequest;await "
        "mutationDone;const verify=queryDb.transaction('records'),verifyDone="
        "finished(verify),remaining=await request(verify.objectStore('records')."
        "getAll());await verifyDone;queryDb.close();await request(indexedDB."
        "deleteDatabase(queryName));const upgradeName='tilefinch-idb-blocked-',"
        "firstOpen=indexedDB.open(upgradeName,1);firstOpen.addEventListener("
        "'upgradeneeded',()=>firstOpen.result.createObjectStore('records'));"
        "const firstDb=await request(firstOpen);let versionchange=false,blocked="
        "false,upgraded=false;firstDb.addEventListener('versionchange',event=>{"
        "versionchange=event.oldVersion===1&&event.newVersion===2;setTimeout("
        "()=>firstDb.close(),0)});const secondOpen=indexedDB.open(upgradeName,2);"
        "secondOpen.addEventListener('blocked',()=>blocked=true);secondOpen."
        "addEventListener('upgradeneeded',()=>upgraded=true);const secondDb=await "
        "request(secondOpen);secondDb.close();await request(indexedDB.deleteDatabase("
        "upgradeName));globalThis.pocSummary=original==='one'&&remaining.length===1"
        "&&remaining[0]==='two'&&versionchange&&blocked&&upgraded&&secondDb."
        "version===2?'INDEXEDDB-QUERY-UPGRADE-OK':"
        "'INDEXEDDB-QUERY-UPGRADE-FAILED:'+JSON.stringify({original,remaining,"
        "versionchange,blocked,upgraded,version:secondDb.version})})().catch(error"
        "=>{globalThis.pocSummary='INDEXEDDB-QUERY-UPGRADE-ERROR:'+String(error"
        "&&error.stack||error)});";
    bool indexeddb_query_upgrade_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_query_upgrade_probe,
        "<indexeddb-query-upgrade-probe>", &result);
    for (size_t tick = 0; indexeddb_query_upgrade_ok && tick < 48
         && strncmp(result.summary, "INDEXEDDB-QUERY-UPGRADE-", 24) != 0;
         tick++) {
        indexeddb_query_upgrade_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_query_upgrade_ok
        || strcmp(result.summary, "INDEXEDDB-QUERY-UPGRADE-OK") != 0) {
        fprintf(stderr,
                "indexeddb query/upgrade probe: ok=%d summary=%s error=%s\n",
                indexeddb_query_upgrade_ok, result.summary, result.error);
    }
    CHECK(indexeddb_query_upgrade_ok
          && strcmp(result.summary, "INDEXEDDB-QUERY-UPGRADE-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_failure_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-failure',beforeQuota="
        "__tilefinchIndexedDBStats.quotaErrors,opening=indexedDB.open(name,1);"
        "opening.addEventListener('upgradeneeded',()=>opening.result."
        "createObjectStore('records',{autoIncrement:true}));const parallel="
        "indexedDB.open(name,1),parallelPromise=request(parallel),db=await "
        "request(opening),parallelDb=await parallelPromise;parallelDb.close();"
        "const aborting=db.transaction('records','readwrite'),"
        "abortDone=finished(aborting).then(()=>'',error=>error.name),store="
        "aborting.objectStore('records'),first=request(store.add('temp')),"
        "duplicate=request(store.add('duplicate',1)).then(()=>'',error=>"
        "error.name),trailing=request(store.get(1)).then(()=>'',error=>"
        "error.name),abortResults=await Promise.all([first,duplicate,trailing,"
        "abortDone]);const write=db.transaction('records','readwrite'),"
        "writeDone=finished(write),restoredKey=await request(write.objectStore("
        "'records').add('kept'));await writeDone;const quota=db.transaction("
        "'records','readwrite'),quotaDone=finished(quota).then(()=>'',error=>"
        "error.name),quotaStore=quota.objectStore('records'),quotaRequest="
        "request(quotaStore.put(new ArrayBuffer(5*1024*1024),2)).then(()=>'',"
        "error=>error.name),quotaTailRequest=request(quotaStore.get(1)).then("
        "()=>'',error=>error.name),quotaResults=await Promise.all([quotaRequest,"
        "quotaTailRequest,quotaDone]),[quotaName,quotaTrailing,quotaDoneName]="
        "quotaResults;"
        "const verify=db.transaction('records'),verifyDone=finished(verify),"
        "kept=await request(verify.objectStore('records').get(1)),count=await "
        "request(verify.objectStore('records').count());await verifyDone;"
        "const originalIncludes=Array.prototype.includes;"
        "Array.prototype.includes=()=>false;const locked=db.transaction("
        "'records','readwrite'),lockedDone="
        "finished(locked),lockedValue=request(locked.objectStore('records')."
        "get(1)),queued=db.transaction('records','readwrite'),queuedDone="
        "finished(queued),queuedValue=request(queued.objectStore('records')."
        "get(1));let queuedSettled=false;queuedDone.then(()=>queuedSettled=true);"
        "const lockedResult=await lockedValue,queuedBeforeRelease="
        "!queuedSettled;await lockedDone;const queuedResult=await queuedValue;"
        "await queuedDone;Array.prototype.includes=originalIncludes;"
        "const timersBefore=__tilefinchSchedulerSnapshot()[2],timers=[];for(let i=0;"
        "i<160;i++){const id=setTimeout(()=>{},1000);if(!id)break;timers.push(id)}"
        "let saturatedName='',replacementFinished=false;const saturated="
        "db.transaction('records'),replacementReady=new Promise((resolve,reject)"
        "=>saturated.addEventListener('abort',()=>{saturatedName=saturated.error."
        "name;for(const id of timers)clearTimeout(id);try{const replacement="
        "db.transaction('records');finished(replacement).then(()=>{"
        "replacementFinished=true;resolve()},reject)}catch(error){reject(error)}}));"
        "await replacementReady;"
        "const recovered=db.transaction('records'),recoveredDone=finished("
        "recovered),recoveredValue=await request(recovered.objectStore('records')."
        "get(1));await recoveredDone;"
        "db.close();const bad=indexedDB.open(name,2);bad.addEventListener("
        "'upgradeneeded',()=>{bad.result.createObjectStore('partial');bad."
        "transaction.objectStore('records').createIndex('temporary','value');"
        "throw new Error('upgrade failure')});const badName=await request(bad)"
        ".then(()=>'',error=>error.name);let retryOld=-1,rolledBack=false;"
        "const retry=indexedDB.open(name,2);retry.addEventListener("
        "'upgradeneeded',event=>{retryOld=event.oldVersion;rolledBack="
        "!retry.result.objectStoreNames.contains('partial')&&retry.result."
        "objectStoreNames.contains('records')&&!retry.transaction.objectStore("
        "'records').indexNames.contains('temporary')});const reopened=await "
        "request(retry),check=reopened.transaction('records'),checkDone="
        "finished(check),persisted=await request(check.objectStore('records')."
        "get(1));await checkDone;reopened.close();await request(indexedDB."
        "deleteDatabase(name));const versioned=indexedDB.open(name,2);"
        "versioned.addEventListener('upgradeneeded',()=>versioned.result."
        "createObjectStore('parallel'));const current=indexedDB.open(name),"
        "versionedDb=await request(versioned),currentDb=await request(current),"
        "currentVersion=currentDb.version;versionedDb.close();currentDb.close();"
        "await request(indexedDB.deleteDatabase(name));const doomedOpen="
        "indexedDB.open('tilefinch-delete-live',1);doomedOpen.addEventListener("
        "'upgradeneeded',()=>doomedOpen.result.createObjectStore('items'));"
        "const doomedDb=await request(doomedOpen),doomedTx=doomedDb.transaction("
        "'items','readwrite'),doomedDone=finished(doomedTx).then(()=>'',error=>"
        "error.name);doomedTx.objectStore('items').put('temporary',1);"
        "const deleteLive=request(indexedDB.deleteDatabase("
        "'tilefinch-delete-live'));const doomedName=await doomedDone;"
        "await deleteLive;globalThis.pocSummary="
        "abortResults[0]===1"
        "&&abortResults[1]==='ConstraintError'&&abortResults[2]==='AbortError'"
        "&&abortResults[3]==='ConstraintError'&&restoredKey===1"
        "&&quotaName==='QuotaExceededError'&&quotaTrailing==='AbortError'"
        "&&quotaDoneName==='QuotaExceededError'&&kept==='kept'&&count===1"
        "&&lockedResult==='kept'&&queuedBeforeRelease&&queuedResult==='kept'"
        "&&timers.length+timersBefore===128"
        "&&saturatedName==='QuotaExceededError'&&replacementFinished"
        "&&recoveredValue==='kept'"
        "&&badName==='AbortError'&&retryOld===1&&rolledBack"
        "&&persisted==='kept'&&__tilefinchIndexedDBStats.quotaErrors"
        "===beforeQuota+1&&currentVersion===2&&doomedName==='AbortError'"
        "&&__tilefinchIndexedDBStats.records===0"
        "&&__tilefinchIndexedDBStats.bytes===0"
        "?'INDEXEDDB-FAILURES-OK':'INDEXEDDB-FAILURES-FAILED:'"
        "+JSON.stringify({abortResults,restoredKey,quotaName,quotaTrailing,"
        "quotaDoneName,kept,count,lockedResult,queuedBeforeRelease,queuedResult,"
        "timers:timers.length,timersBefore,saturatedName,replacementFinished,"
        "recoveredValue,badName,"
        "retryOld,rolledBack,persisted,currentVersion,doomedName,"
        "stats:__tilefinchIndexedDBStats});})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-FAILURES-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool indexeddb_failures_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_failure_probe, "<indexeddb-failure-probe>",
        &result);
    for (size_t tick = 0; indexeddb_failures_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-FAILURES-", 19) != 0; tick++) {
        indexeddb_failures_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_failures_ok
        || strcmp(result.summary, "INDEXEDDB-FAILURES-OK") != 0) {
        fprintf(stderr, "indexeddb failure probe: ok=%d summary=%s error=%s\n",
                indexeddb_failures_ok, result.summary, result.error);
    }
    CHECK(indexeddb_failures_ok
          && strcmp(result.summary, "INDEXEDDB-FAILURES-OK") == 0
          && result.indexed_db_quota_errors == 1
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_task_admission_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{req."
        "addEventListener('success',()=>resolve(req.result));req.addEventListener("
        "'error',()=>reject(req.error))}),finished=tx=>new Promise((resolve,reject)"
        "=>{tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error))}),name='tilefinch-idb-task-admission',opening="
        "indexedDB.open(name,1);opening.addEventListener('upgradeneeded',()=>"
        "opening.result.createObjectStore('records'));const db=await request("
        "opening),seed=db.transaction('records','readwrite'),seedDone=finished("
        "seed);await Promise.all([request(seed.objectStore('records').put('kept',"
        "1)),seedDone]);const blocker=db.transaction('records','readwrite'),"
        "blockerDone=finished(blocker),blockerRead=request(blocker.objectStore("
        "'records').get(1)),queued=db.transaction('records','readwrite');let "
        "aborts=0,pendingAtAbort=-1,lastErrorCheckpoint=false,checkpointAtAbort=false,"
        "order=[],requests=[];queued.addEventListener("
        "'abort',()=>{aborts++;pendingAtAbort=0;for(const item of requests)if(item."
        "readyState==='pending')pendingAtAbort++;checkpointAtAbort=lastErrorCheckpoint;"
        "order.push('abort')});const queuedDone="
        "finished(queued).then(()=>'',error=>error.name),settled=[],store=queued."
        "objectStore('records');for(let i=0;i<140;i++){const item=store.put(i,i+2);"
        "requests.push(item);"
        "settled.push(new Promise(resolve=>{item.addEventListener('success',()=>"
        "resolve('success'));item.addEventListener('error',()=>{order.push(i);if(i===139)"
        "queueMicrotask(()=>lastErrorCheckpoint=true);resolve(item.error."
        "name)})}))}await blockerRead;await blockerDone;const outcomes=await Promise."
        "all(settled),abortName=await queuedDone,verify=db.transaction('records'),"
        "verifyDone=finished(verify),count=await request(verify.objectStore('records')"
        ".count());await verifyDone;const reuse=db.transaction('records','readwrite'),"
        "reuseDone=finished(reuse),reused=await request(reuse.objectStore('records')"
        ".put('after',2));await reuseDone;db.close();await request(indexedDB."
        "deleteDatabase(name));globalThis.pocSummary=outcomes.length===140&&"
        "outcomes.every(value=>value==='AbortError')&&aborts===1&&pendingAtAbort===0&&"
        "checkpointAtAbort&&"
        "order.length===141&&order.every((value,index)=>index<140?value===index:value==="
        "'abort')&&abortName==='QuotaExceededError'&&count===1&&reused===2?"
        "'INDEXEDDB-ADMISSION-OK':"
        "'INDEXEDDB-ADMISSION-FAILED:'+JSON.stringify({outcomes:outcomes.length,"
        "errors:outcomes.filter(value=>value==='AbortError').length,aborts,abortName,"
        "pendingAtAbort,checkpointAtAbort,orderStart:order.slice(0,4),"
        "orderEnd:order.slice(-4),count,"
        "reused})})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-ADMISSION-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_task_admission_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_task_admission_probe,
        "<indexeddb-task-admission-probe>", &result);
    for (size_t tick = 0; indexeddb_task_admission_ok && tick < 64
         && strncmp(result.summary, "INDEXEDDB-ADMISSION-", 20) != 0; tick++) {
        indexeddb_task_admission_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_task_admission_ok
        || strcmp(result.summary, "INDEXEDDB-ADMISSION-OK") != 0) {
        fprintf(stderr, "indexeddb task-admission probe: ok=%d summary=%s "
                "error=%s\n", indexeddb_task_admission_ok,
                result.summary, result.error);
    }
    CHECK(indexeddb_task_admission_ok
          && strcmp(result.summary, "INDEXEDDB-ADMISSION-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    /* Record identity for array keys is derived by serializing the key. If
       that serializer is the live JSON.stringify, a page can replace it and
       make the map lookup in delete() miss while the range scan still finds
       the record and still subtracts its bytes. Two range deletes then drive
       stats.records and stats.bytes negative, after which the quota check
       (delta > BYTE_LIMIT - stats.bytes) stops bounding anything. */
    static const char indexeddb_key_token_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-keytoken',"
        "opening=indexedDB.open(name,1);opening.addEventListener("
        "'upgradeneeded',()=>opening.result.createObjectStore('arr'));"
        "const db=await request(opening),write=db.transaction('arr',"
        "'readwrite'),writeDone=finished(write),store=write.objectStore('arr');"
        "await Promise.all([request(store.put({v:1},[1])),"
        "request(store.put({v:2},[2])),writeDone]);"
        "const seeded=__tilefinchIndexedDBStats.records,"
        "seededBytes=__tilefinchIndexedDBStats.bytes,"
        "nativeStringify=JSON.stringify;let remaining=-1,records=-1,bytes=-1;"
        "JSON.stringify=()=>'poisoned';"
        "try{for(let i=0;i<2;i++){const tx=db.transaction('arr','readwrite'),"
        "txDone=finished(tx);await Promise.all([request(tx.objectStore('arr')."
        "delete(IDBKeyRange.bound([0],[9]))),txDone]);}"
        "const read=db.transaction('arr'),readDone=finished(read);"
        "remaining=(await request(read.objectStore('arr').getAll())).length;"
        "await readDone;records=__tilefinchIndexedDBStats.records;"
        "bytes=__tilefinchIndexedDBStats.bytes;}"
        "finally{JSON.stringify=nativeStringify;}db.close();"
        "await request(indexedDB.deleteDatabase(name));"
        "globalThis.pocSummary=seeded===2&&seededBytes>0&&remaining===0"
        "&&records===0&&bytes===0?'INDEXEDDB-KEYTOKEN-OK':"
        "'INDEXEDDB-KEYTOKEN-FAILED:'+JSON.stringify({seeded,seededBytes,"
        "remaining,records,bytes});})()"
        ".catch(error=>{globalThis.pocSummary='INDEXEDDB-KEYTOKEN-ERROR:'+"
        "String(error&&error.stack||error)});";
    bool indexeddb_key_token_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_key_token_probe, "<indexeddb-key-token-probe>",
        &result);
    for (size_t tick = 0; indexeddb_key_token_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-KEYTOKEN-", 19) != 0; tick++) {
        indexeddb_key_token_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_key_token_ok
        || strcmp(result.summary, "INDEXEDDB-KEYTOKEN-OK") != 0) {
        fprintf(stderr,
                "indexeddb key-token probe: ok=%d summary=%s error=%s\n",
                indexeddb_key_token_ok, result.summary, result.error);
    }
    CHECK(indexeddb_key_token_ok
          && strcmp(result.summary, "INDEXEDDB-KEYTOKEN-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_unique_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),"
        "finished=tx=>new Promise((resolve,reject)=>{"
        "tx.addEventListener('complete',resolve);tx.addEventListener('abort',"
        "()=>reject(tx.error));}),name='tilefinch-idb-unique',"
        "opening=indexedDB.open(name,1);opening.addEventListener("
        "'upgradeneeded',()=>{const store=opening.result.createObjectStore("
        "'items',{keyPath:'id'});store.createIndex('email','email',{unique:true})});"
        "const db=await request(opening),first=db.transaction('items','readwrite'),"
        "firstDone=finished(first);await Promise.all([request(first.objectStore("
        "'items').add({id:1,email:'same@example.test'})),firstDone]);"
        "const duplicate=db.transaction('items','readwrite');let duplicateBubbled="
        "false;duplicate.addEventListener('error',{handleEvent(event){if("
        "event.target instanceof IDBRequest)duplicateBubbled=event.currentTarget"
        "===duplicate}});const "
        "duplicateDone=finished(duplicate).then(()=>'',error=>error.name),"
        "duplicateRequest=request(duplicate.objectStore('items').add("
        "{id:2,email:'same@example.test'})).then(()=>'',error=>error.name),"
        "duplicateNames=await Promise.all([duplicateRequest,duplicateDone]);"
        "db.close();await request(indexedDB.deleteDatabase(name));"
        "const existing=indexedDB.open(name,1);existing.addEventListener("
        "'upgradeneeded',()=>existing.result.createObjectStore('items',"
        "{keyPath:'id'}));const existingDb=await request(existing),"
        "seed=existingDb.transaction('items','readwrite'),seedDone=finished(seed),"
        "seedStore=seed.objectStore('items');await Promise.all(["
        "request(seedStore.add({id:1,email:'same@example.test'})),"
        "request(seedStore.add({id:2,email:'same@example.test'})),seedDone]);"
        "existingDb.close();let createName='';const upgrade=indexedDB.open(name,2);"
        "upgrade.addEventListener('upgradeneeded',()=>{try{upgrade.transaction."
        "objectStore('items').createIndex('email','email',{unique:true})}"
        "catch(error){createName=error.name;upgrade.transaction.abort()}});"
        "const upgradeName=await request(upgrade).then(()=>'',error=>error.name);"
        "await request(indexedDB.deleteDatabase(name));"
        "globalThis.pocSummary=duplicateNames[0]==='ConstraintError'"
        "&&duplicateNames[1]==='ConstraintError'&&createName==='ConstraintError'"
        "&&upgradeName==='AbortError'&&duplicateBubbled?"
        "'INDEXEDDB-UNIQUE-OK':"
        "'INDEXEDDB-UNIQUE-FAILED:'+JSON.stringify({duplicateNames,createName,"
        "upgradeName});})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-UNIQUE-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_unique_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_unique_probe, "<indexeddb-unique-probe>", &result);
    for (size_t tick = 0; indexeddb_unique_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-UNIQUE-", 17) != 0; tick++) {
        indexeddb_unique_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_unique_ok
        || strcmp(result.summary, "INDEXEDDB-UNIQUE-OK") != 0) {
        fprintf(stderr, "indexeddb unique probe: ok=%d summary=%s error=%s\n",
                indexeddb_unique_ok, result.summary, result.error);
    }
    CHECK(indexeddb_unique_ok
          && strcmp(result.summary, "INDEXEDDB-UNIQUE-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    static const char indexeddb_contract_probe[] =
        "(async()=>{const request=req=>new Promise((resolve,reject)=>{"
        "req.addEventListener('success',()=>resolve(req.result));"
        "req.addEventListener('error',()=>reject(req.error));}),finished=tx=>"
        "new Promise((resolve,reject)=>{tx.addEventListener('complete',resolve);"
        "tx.addEventListener('abort',()=>reject(tx.error));}),name="
        "'tilefinch-idb-contract',originalClone=globalThis.structuredClone;"
        "let poisonCalls=0;globalThis.structuredClone=()=>{poisonCalls++;throw "
        "new Error('poison')};try{const opening=indexedDB.open(name,1);opening."
        "addEventListener('upgradeneeded',()=>opening.result.createObjectStore("
        "'items',{keyPath:'id'}));const db=await request(opening),burst=[];for(let "
        "i=0;i<9;i++)burst.push(db.transaction('items'));await Promise.all(burst."
        "map(finished));const write=db."
        "transaction('items','readwrite'),writeDone=finished(write),store=write."
        "objectStore('items'),source={id:1,value:'before'},cycle={id:2},shared="
        "{value:3};cycle.self=cycle;const first=request(store.put(source)),"
        "second=request(store.put(cycle)),third=request(store.put({id:3,a:shared,"
        "b:shared})),eventRequest=store.put({id:4,value:'event'}),eventCalls=[],"
        "abortListeners=new AbortController;eventRequest.addEventListener('success',"
        "()=>eventCalls.push('first'));eventRequest."
        "addEventListener('success',{handleEvent(event){eventCalls.push(event.target"
        "===eventRequest&&event.currentTarget===eventRequest?'object':'bad')}},{once:"
        "true});eventRequest.addEventListener('success',()=>eventCalls.push('abort'),"
        "{signal:abortListeners.signal});abortListeners.abort();const fourth=request("
        "eventRequest);source.value='after';let cloneName='',postCommitName='';"
        "try{store.put(()=>{},4)}catch(error){cloneName=error.name}write.commit();"
        "try{store.get(1)}catch(error){postCommitName=error.name}await Promise.all("
        "[first,second,third,fourth,writeDone]);const read=db.transaction('items'),"
        "readDone=finished(read),readStore=read.objectStore('items');await Promise."
        "resolve();await Promise.resolve();await Promise.resolve();const values="
        "await Promise.all([request(readStore.get(1)),"
        "request(readStore.get(2)),request(readStore.get(3))]);await readDone;"
        "const order=[],ordered=db.transaction('items'),orderedDone=finished("
        "ordered),orderedRequest=ordered.objectStore('items').get(1);"
        "orderedRequest.addEventListener('success',()=>order.push('success'));"
        "queueMicrotask(()=>order.push('microtask'));await request(orderedRequest);"
        "await orderedDone;const successLate=db.transaction('items'),successLateStore="
        "successLate.objectStore('items'),successLateDone=finished(successLate),"
        "successLateName=await new Promise(resolve=>{const first="
        "successLateStore.get(1);first.addEventListener('success',()=>setTimeout("
        "()=>{try{successLateStore.get(1);resolve('none')}catch(error){resolve("
        "error.name)}},0))});await successLateDone;const prequeued="
        "db.transaction('items'),prequeuedStore=prequeued.objectStore('items'),"
        "prequeuedDone=finished(prequeued);prequeuedStore.get(1);const "
        "prequeuedName=await new Promise(resolve=>setTimeout(()=>{try{"
        "prequeuedStore.get(1);resolve('none')}catch(error){resolve(error.name)}},"
        "0));await prequeuedDone;"
        "const late=db.transaction('items'),lateStore=late.objectStore('items'),"
        "lateDone=finished(late),lateName=await new Promise(resolve=>setTimeout("
        "()=>{try{lateStore.get(1);resolve('none')}catch(error){resolve(error.name)}}"
        ",0));await lateDone;db.close();await request(indexedDB.deleteDatabase("
        "name));globalThis.pocSummary=burst.length===9&&poisonCalls===0&&"
        "cloneName==='DataCloneError'"
        "&&postCommitName==='TransactionInactiveError'&&values[0].value==="
        "'before'&&values[1].self===values[1]&&values[2].a===values[2].b&&"
        "lateName==='TransactionInactiveError'&&successLateName==="
        "'TransactionInactiveError'&&eventRequest instanceof EventTarget"
        "&&prequeuedName==='TransactionInactiveError'&&typeof globalThis."
        "__tilefinchQueueCheckpointContinuation==='undefined'"
        "&&eventCalls.join(',')==='first,object'&&order.join(',')==="
        "'microtask,success'?'INDEXEDDB-CONTRACT-OK':"
        "'INDEXEDDB-CONTRACT-FAILED:'+JSON.stringify({poisonCalls,cloneName,"
        "postCommitName,first:values[0].value,cycle:values[1].self===values[1],"
        "alias:values[2].a===values[2].b,lateName,successLateName,prequeuedName,"
        "order})}finally{"
        "globalThis."
        "structuredClone=originalClone}})().catch(error=>{globalThis.pocSummary="
        "'INDEXEDDB-CONTRACT-ERROR:'+String(error&&error.stack||error)});";
    bool indexeddb_contract_ok = script_runtime_evaluate_diagnostic(
        runtime, indexeddb_contract_probe, "<indexeddb-contract-probe>",
        &result);
    for (size_t tick = 0; indexeddb_contract_ok && tick < 32
         && strncmp(result.summary, "INDEXEDDB-CONTRACT-", 19) != 0; tick++) {
        indexeddb_contract_ok = script_runtime_advance(
            runtime, 0, 1024, &result);
    }
    if (!indexeddb_contract_ok
        || strcmp(result.summary, "INDEXEDDB-CONTRACT-OK") != 0) {
        fprintf(stderr, "indexeddb contract probe: ok=%d summary=%s error=%s\n",
                indexeddb_contract_ok, result.summary, result.error);
    }
    CHECK(indexeddb_contract_ok
          && strcmp(result.summary, "INDEXEDDB-CONTRACT-OK") == 0
          && result.indexed_db_records == 0
          && result.indexed_db_bytes == 0);

    /* hardening.js runs once, over a snapshot of the globals that exist at
       that moment, so a __tilefinch global created lazily on a later write used
       to stay enumerable for the rest of the page. And the host entry points
       the event loop invokes by name every tick were left writable, which
       makes each of them a way for page script to take over the tick. */
    static const char entry_point_hardening_probe[] =
        "(()=>{const lazy=['__tilefinchSubmittedFormHandle',"
        "'__tilefinchSubmittedSubmitterHandle','__tilefinchFragmentInsertCount',"
        "'__tilefinchFragmentInsertText',"
        "'__tilefinchBase64Error','__tilefinchSelectedControl',"
        "'__tilefinchParentAppendBypass','__tilefinchMutationSuppressed',"
        "'__tilefinchNow'],"
        "entries=['__tilefinchCommitSameDocument','__tilefinchDeliverNetwork',"
        "'__tilefinchRecordEventHandler','__tilefinchReportUncaught',"
        "'__tilefinchRunTask',"
        "'__tilefinchDispatchDOMContentLoaded',"
        "'__tilefinchIntersectionRecheck','__tilefinchParserMutationCheckpoint',"
        "'__tilefinchMediaRecheck',"
        "'__tilefinchPumpTimers','__tilefinchRebindDocument',"
        "'__tilefinchRecordResourceTiming','__tilefinchRefreshNamedProperties',"
        "'__tilefinchRestoreSameDocument','__tilefinchRestoreSectionState',"
        "'__tilefinchSaveSectionState','__tilefinchSetFrameWindowState',"
        "'__tilefinchWrapRemote','__tilefinchWrapRemoteRelation',"
        "'__tilefinchWrapRemoteSelector','__tilefinchWrapRemoteStable'];"
        "for(const name of lazy)globalThis[name]=globalThis[name];"
        "const enumerable=lazy.filter(name=>Object.getOwnPropertyDescriptor("
        "globalThis,name)?.enumerable!==false),"
        "writable=entries.filter(name=>{const d="
        "Object.getOwnPropertyDescriptor(globalThis,name);"
        "return !d||typeof d.value!=='function'||d.writable!==false"
        "||d.configurable!==false;});"
        "const pumpBefore=__tilefinchPumpTimers,taskBefore=__tilefinchRunTask;"
        "try{globalThis.__tilefinchPumpTimers=()=>0;}catch(error){}"
        "try{globalThis.__tilefinchRunTask=(_,callback)=>callback();}catch(error){}"
        "const held=__tilefinchPumpTimers===pumpBefore"
        "&&__tilefinchRunTask===taskBefore;"
        "globalThis.pocSummary=enumerable.length===0&&writable.length===0"
        "&&held?'HARDENING-OK':'HARDENING-FAILED:'"
        "+enumerable.join(',')+'|'+writable.join(',')+'|'+held;})()";
    bool entry_hardening_ok = script_runtime_evaluate_diagnostic(
        runtime, entry_point_hardening_probe,
        "<entry-point-hardening-probe>", &result);
    if (!entry_hardening_ok
        || strcmp(result.summary, "HARDENING-OK") != 0) {
        fprintf(stderr, "entry hardening: ok=%d summary=%s error=%s\n",
                entry_hardening_ok, result.summary, result.error);
    }
    CHECK(entry_hardening_ok
          && strcmp(result.summary, "HARDENING-OK") == 0);

    static const char frame_window_probe[] =
        "(()=>{const frame=document.createElement('iframe'),"
        "child=frame.contentWindow;globalThis.pocSummary=child"
        "&&child.window===child&&child.self===child&&child.globalThis===child"
        "&&child.parent===globalThis&&child.String!==String"
        "&&child.String.prototype!==String.prototype"
        "&&typeof child.eval==='function'&&typeof child.postMessage==='function'"
        "&&child.eval('String===globalThis.String')"
        "?'FRAME-WINDOW-GLOBALS-OK':'FRAME-WINDOW-GLOBALS-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frame_window_probe, "<frame-window-probe>", &result)
          && strcmp(result.summary, "FRAME-WINDOW-GLOBALS-OK") == 0);
    size_t frame_windows_before = result.root_frame_windows;

    CHECK(script_runtime_evaluate_diagnostic(
              runtime,
              "(()=>{const frame=document.createElement('iframe');"
              "document.body.appendChild(frame);globalThis.crossFrame=frame;"
              "globalThis.crossWindow=frame.contentWindow;"
              "crossWindow.initialMarker='preserved';"
              "globalThis.pocSummary=String(frame.__handle)})()",
              "<cross-frame-setup>", &result));
    long cross_frame_handle = strtol(result.summary, NULL, 10);
    CHECK(cross_frame_handle > 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, true, false,
        "https://parent.test/child?entry=enabled", false, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.initialMarker==='preserved'"
        "&&crossWindow.location.href==='about:blank'?"
        "'FRAME-START-PRESERVES-INITIAL-OK':"
        "'FRAME-START-PRESERVES-INITIAL-FAILED:'+crossWindow.location.href",
        "<frame-start-preserves-initial>", &result)
        && strcmp(result.summary, "FRAME-START-PRESERVES-INITIAL-OK") == 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, true, false,
        "https://parent.test/child?entry=enabled", true, &result));
    bool initial_frame_commit_ok = script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.location.search"
        "==='?entry=enabled'&&crossWindow.initialMarker==='preserved'?"
        "'FRAME-LOCATION-OK':'FRAME-LOCATION-FAILED:'+crossWindow.location.search"
        "+','+String(crossWindow.initialMarker)",
        "<same-origin-frame-location>", &result);
    if (!initial_frame_commit_ok
        || strcmp(result.summary, "FRAME-LOCATION-OK") != 0) {
        fprintf(stderr, "initial frame commit: ok=%d summary=%s error=%s\n",
                initial_frame_commit_ok, result.summary, result.error);
    }
    CHECK(initial_frame_commit_ok
          && strcmp(result.summary, "FRAME-LOCATION-OK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "crossWindow.beforeCrossCommit='visible';"
        "globalThis.pocSummary='FRAME-CROSS-START-ARMED'",
        "<cross-frame-start-setup>", &result));
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, false, false,
        "https://other.test/child", false, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.beforeCrossCommit==='visible'"
        "&&crossWindow.document!==undefined"
        "&&crossWindow.location.search==='?entry=enabled'?"
        "'FRAME-CROSS-START-KEEPS-COMMIT-OK':"
        "'FRAME-CROSS-START-KEEPS-COMMIT-FAILED'",
        "<cross-frame-start-keeps-commit>", &result)
        && strcmp(result.summary, "FRAME-CROSS-START-KEEPS-COMMIT-OK") == 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, false, false,
        "https://other.test/child", true,
        &result));
    CHECK(result.root_frame_windows == frame_windows_before + 1);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossFrame.contentWindow===crossWindow"
        "&&crossWindow.closed===false"
        "&&crossWindow.document===undefined"
        "&&crossWindow.location===undefined"
        "&&crossWindow.eval===undefined"
        "&&crossWindow.pocSummary===undefined"
        "&&('document' in crossWindow)===false"
        "&&('pocSummary' in crossWindow)===false"
        "&&crossWindow.window===crossWindow"
        "&&crossWindow.opener===null"
        "&&Reflect.set(crossWindow,'postMessage',()=>{})===false?"
        "'CROSS-WINDOW-OK':'CROSS-WINDOW-FAILED'",
        "<cross-frame-check>", &result)
        && strcmp(result.summary, "CROSS-WINDOW-OK") == 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, true, false,
        "https://parent.test/child?return=enabled", false, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.document===undefined"
        "&&crossWindow.location===undefined?"
        "'FRAME-RETURN-START-KEEPS-COMMIT-OK':"
        "'FRAME-RETURN-START-KEEPS-COMMIT-FAILED'",
        "<frame-return-start-keeps-commit>", &result)
        && strcmp(result.summary,
                  "FRAME-RETURN-START-KEEPS-COMMIT-OK") == 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, true, false,
        "https://parent.test/child?return=enabled", true, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossFrame.contentWindow===crossWindow"
        "&&crossWindow.location.search==='?return=enabled'"
        "&&crossWindow.beforeCrossCommit===undefined?"
        "'FRAME-RETURN-COMMIT-FRESH-OK':"
        "'FRAME-RETURN-COMMIT-FRESH-FAILED'",
        "<frame-return-commit-fresh>", &result)
        && strcmp(result.summary, "FRAME-RETURN-COMMIT-FRESH-OK") == 0);
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "crossWindow.sameUrlStale=1;crossWindow.eval("
        "'var sameUrlVar=1');globalThis.pocSummary='FRAME-STALE-ARMED'",
        "<same-url-frame-stale-setup>", &result));
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, true, true, false,
        "https://parent.test/child?return=enabled", true, &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.sameUrlStale===undefined"
        "&&crossWindow.eval('typeof sameUrlVar')==='undefined'?"
        "'FRAME-SAME-URL-FRESH-OK':'FRAME-SAME-URL-FRESH-FAILED'",
        "<same-url-frame-fresh-check>", &result)
        && strcmp(result.summary, "FRAME-SAME-URL-FRESH-OK") == 0);
    CHECK(script_runtime_set_frame_window_state(
        runtime, cross_frame_handle, false, false, false, NULL, false,
        &result));
    CHECK(script_runtime_evaluate_diagnostic(
        runtime,
        "globalThis.pocSummary=crossWindow.closed?"
        "'CLOSED-WINDOW-OK':'CLOSED-WINDOW-FAILED';crossFrame.remove()",
        "<closed-frame-check>", &result)
        && strcmp(result.summary, "CLOSED-WINDOW-OK") == 0);

    static const char frame_retention_probe[] =
        "(()=>{const frames=[];for(let i=0;i<24;i++){const frame="
        "document.createElement('iframe');frames.push(frame);void "
        "frame.contentWindow}const receive=event=>{globalThis.pocSummary="
        "__tilefinchRootCensus.frameWindows<=16&&event.source===window?"
        "'FRAME-RETENTION-OK':'FRAME-RETENTION-FAILED'};"
        "addEventListener('message',receive,{once:true});"
        "globalThis.pocSummary='FRAME-RETENTION-PENDING';"
        "postMessage({probe:true},'*')})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, frame_retention_probe,
              "<frame-retention-probe>", &result)
          && script_runtime_advance(runtime, 0, 8, &result)
          && script_runtime_advance(runtime, 0, 8, &result)
          && strcmp(result.summary, "FRAME-RETENTION-OK") == 0);

    static const char callback_task_probe[] =
        "(()=>{const xhr=new XMLHttpRequest();"
        "xhr.addEventListener('readystatechange',()=>{"
        "throw new Error('task-probe')});xhr.open('GET','data:text/plain,ok');"
        "globalThis.pocSummary='CALLBACK-TASK-PROBE-OK';})()";
    size_t callback_errors_before = result.uncaught_callback_errors;
    bool callback_task_ok = script_runtime_evaluate_diagnostic(
        runtime, callback_task_probe, "<callback-task-probe>", &result);
    bool callback_task_valid = callback_task_ok
        && strcmp(result.summary, "CALLBACK-TASK-PROBE-OK") == 0
        && result.uncaught_callback_errors == callback_errors_before + 1
        && strstr(result.last_uncaught_callback_error, "task-probe") != NULL
        && strstr(result.last_uncaught_callback_error, "<browser-") == NULL
        && strstr(result.last_uncaught_callback_error, "call (native)") == NULL
        && strstr(result.last_uncaught_callback_task, "realm=top") != NULL
        && strstr(result.last_uncaught_callback_task,
                  "context=event readystatechange") != NULL;
    if (!callback_task_valid) {
        fprintf(stderr,
                "callback task probe: ok=%d summary=%s count=%zu error=%s "
                "task=%s roots=%zu\n", callback_task_ok, result.summary,
                result.uncaught_callback_errors,
                result.last_uncaught_callback_error,
                result.last_uncaught_callback_task,
                result.root_node_wrappers);
    }
    CHECK(callback_task_valid);

    static const char node_move_probe[] =
        "(()=>{const left=document.createElement('div'),"
        "right=document.createElement('div'),first=document.createElement('i'),"
        "moved=document.createElement('b'),last=document.createElement('u');"
        "left.append(first,moved,last);document.body.append(left,right);"
        "const appendReturn=right.appendChild(moved);"
        "right.appendChild(moved);"
        "const selfReturn=left.insertBefore(first,first);"
        "left.insertBefore(moved,last);"
        "let appendCycle=false,insertCycle=false;"
        "try{left.appendChild(left)}catch(_){appendCycle=true}"
        "const anchor=document.createElement('em');moved.appendChild(anchor);"
        "try{moved.insertBefore(left,anchor)}catch(_){insertCycle=true}"
        "globalThis.pocSummary=appendReturn===moved&&selfReturn===first"
        "&&left.childNodes.length===3&&left.firstChild===first"
        "&&first.nextSibling===moved&&moved.nextSibling===last"
        "&&right.childNodes.length===0&&appendCycle&&insertCycle"
        "?'NODE-MOVE-OK':'NODE-MOVE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, node_move_probe, "<node-move-probe>", &result)
          && strcmp(result.summary, "NODE-MOVE-OK") == 0);

    static const char rebound_matches_probe[] =
        "(()=>{const host=document.createElement('section'),"
        "child=document.createElement('span'),cached="
        "document.documentElement.matches;host.className='host';"
        "child.className='target';host.appendChild(child);"
        "globalThis.pocSummary=cached.call(child,'.target')"
        "&&!cached.call(child,'.host')&&cached.call(host,'.host')"
        "?'REBOUND-MATCHES-OK':'REBOUND-MATCHES-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, rebound_matches_probe, "<rebound-matches-probe>",
              &result)
          && strcmp(result.summary, "REBOUND-MATCHES-OK") == 0);

    static const char replace_child_probe[] =
        "(()=>{const parent=document.createElement('div'),"
        "old=document.createElement('i'),replacement=document.createElement('b');"
        "old.textContent='old';replacement.textContent='new';parent.appendChild(old);"
        "const returned=parent.replaceChild(replacement,old);"
        "const fragment=document.createDocumentFragment(),first="
        "document.createElement('u'),second=document.createElement('em');"
        "fragment.append(first,second);const fragmentReturn="
        "parent.replaceChild(fragment,replacement);let rejected=false;"
        "try{parent.replaceChild(document.createElement('q'),old)}catch(_){rejected=true}"
        "globalThis.pocSummary=returned===old&&fragmentReturn===replacement"
        "&&parent.childNodes.length===2&&parent.firstChild===first"
        "&&first.nextSibling===second&&fragment.childNodes.length===0&&rejected"
        "?'REPLACE-CHILD-OK':'REPLACE-CHILD-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, replace_child_probe, "<replace-child-probe>", &result)
          && strcmp(result.summary, "REPLACE-CHILD-OK") == 0);

    /* jQuery's buildFragment retains parsed child wrappers, clears their
       temporary container, then appends those detached nodes to a fragment.
       textContent replacement must preserve those referenced Node objects. */
    static const char fragment_builder_probe[] =
        "(()=>{const temporary=document.createElement('div'),"
        "destination=document.createElement('main');temporary.innerHTML="
        "'<a id=fragment-a>one</a><span>two</span>';const retained="
        "[...temporary.childNodes],firstHandle=retained[0].__handle;"
        "temporary.textContent='';const fragment=document.createDocumentFragment();"
        "for(const child of retained)fragment.appendChild(child);"
        "destination.appendChild(fragment);globalThis.pocSummary="
        "temporary.childNodes.length===0&&fragment.childNodes.length===0"
        "&&destination.childNodes.length===2&&retained[0].__handle==="
        "firstHandle&&retained[0].parentNode===destination"
        "&&destination.textContent==='onetwo'?'FRAGMENT-BUILDER-OK':"
        "'FRAGMENT-BUILDER-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, fragment_builder_probe, "<fragment-builder-probe>",
              &result)
          && strcmp(result.summary, "FRAGMENT-BUILDER-OK") == 0);

    /* Lexbor owns template.content through its host template even though the
       DocumentFragment is parentless.  Releasing the content wrapper must
       not independently destroy that fragment while the template survives. */
    static const char template_content_lifetime_probe[] =
        "(()=>{const template=document.createElement('template');"
        "template.innerHTML='<b>owned</b>';const content=template.content,"
        "handle=content.__handle,lease=content.__tilefinchHandleLease;"
        "const released=__tilefinchReleaseNodeWrapper(handle,lease);"
        "globalThis.__tilefinchOwnedTemplate=template;globalThis.pocSummary="
        "released===false&&template.content.querySelector('b').textContent"
        "==='owned'?'TEMPLATE-CONTENT-LIFETIME-OK':"
        "'TEMPLATE-CONTENT-LIFETIME-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, template_content_lifetime_probe,
              "<template-content-lifetime-probe>", &result)
          && strcmp(result.summary, "TEMPLATE-CONTENT-LIFETIME-OK") == 0);

    /* innerHTML has the same replace-all lifetime contract as textContent:
       removed children become detached, not invalid, while wrappers exist. */
    static const char retained_inner_html_probe[] =
        "(()=>{const host=document.createElement('section');host.innerHTML="
        "'<div><b id=retained-inner>old</b></div>';const retained="
        "host.querySelector('#retained-inner'),parent=retained.parentNode,"
        "handle=retained.__handle;host.innerHTML='<p>new</p>';"
        "const destination=document.createElement('aside');destination."
        "appendChild(parent);globalThis.pocSummary=retained.__handle===handle"
        "&&retained.parentNode===parent&&parent.parentNode===destination"
        "&&retained.textContent==='old'&&host.textContent==='new'"
        "?'RETAINED-INNER-HTML-OK':'RETAINED-INNER-HTML-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, retained_inner_html_probe,
              "<retained-inner-html-probe>", &result)
          && strcmp(result.summary, "RETAINED-INNER-HTML-OK") == 0);

    static const char shadow_content_and_event_probe[] =
        "(()=>{const host=document.createElement('div'),"
        "light=document.createElement('span');light.id='light-before';"
        "host.appendChild(light);document.body.appendChild(host);"
        "const root=host.attachShadow({mode:'open'}),"
        "slot=document.createElement('slot');root.appendChild(slot);"
        "let slotSeen=false,hostSeen=false,documentSeen=false;"
        "slot.addEventListener('tilefinch-noncomposed',()=>slotSeen=true);"
        "host.addEventListener('tilefinch-noncomposed',()=>hostSeen=true);"
        "document.addEventListener('tilefinch-noncomposed',"
        "()=>documentSeen=true,{once:true});"
        "light.dispatchEvent(new Event('tilefinch-noncomposed',"
        "{bubbles:true,composed:false}));"
        "host.innerHTML='<b id=light-after>new</b>';"
        "const htmlOk=host.shadowRoot===root&&root.firstChild===slot"
        "&&host.querySelector('#light-after')!==null"
        "&&host.innerHTML.includes('light-after')"
        "&&!host.innerHTML.includes('<slot');"
        "host.textContent='plain';const textOk=host.shadowRoot===root"
        "&&root.firstChild===slot&&host.textContent==='plain';"
        "const removed=host.firstChild;"
        "const returned=host.removeChild(removed);"
        "globalThis.pocSummary=slotSeen&&hostSeen&&documentSeen&&htmlOk"
        "&&textOk&&returned===removed?'SHADOW-CONTENT-EVENT-OK':"
        "'SHADOW-CONTENT-EVENT-FAILED:'+JSON.stringify({slotSeen,"
        "hostSeen,documentSeen,htmlOk,textOk,returned:returned===removed});"
        "host.remove()})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, shadow_content_and_event_probe,
              "<shadow-content-event-probe>", &result)
          && strcmp(result.summary, "SHADOW-CONTENT-EVENT-OK") == 0);

    static const char delegated_focus_probe[] =
        "(()=>{const host=document.createElement('div');"
        "document.body.appendChild(host);const root=host.attachShadow("
        "{mode:'open',delegatesFocus:true}),button=document.createElement("
        "'button');button.textContent='focus';root.appendChild(button);"
        "host.focus();const delegated=document.activeElement===host"
        "&&root.activeElement===button;"
        "let arrowRejected=false;try{customElements.define("
        "'x-tilefinch-arrow',()=>{})}catch(error){"
        "arrowRejected=error instanceof TypeError}"
        "globalThis.pocSummary=delegated&&arrowRejected"
        "?'DELEGATED-FOCUS-CONSTRUCTOR-OK':"
        "'DELEGATED-FOCUS-CONSTRUCTOR-FAILED:'+JSON.stringify({"
        "delegated,arrowRejected,documentActive:"
        "document.activeElement?.localName,shadowActive:"
        "root.activeElement?.localName});host.remove()})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, delegated_focus_probe,
              "<delegated-focus-constructor-probe>", &result)
          && strcmp(result.summary,
                    "DELEGATED-FOCUS-CONSTRUCTOR-OK") == 0);

    static const char custom_elements_probe[] =
        "(()=>{const events=[],existing=document.createElement('x-tilefinch-probe');"
        "existing.setAttribute('observed','before');document.body.appendChild(existing);"
        "const undefinedBefore=!existing.matches(':defined')&&document.body.matches(':defined');"
        "let resolved=false;customElements.whenDefined('x-tilefinch-probe').then("
        "constructor=>{resolved=constructor===Probe;globalThis.pocSummary="
        "undefinedBefore&&resolved&&existing instanceof Probe&&existing.matches(':defined')"
        "&&existing.constructed===1&&events.join(',')==='attribute:null:before,connected,attribute:before:after,connected,disconnected'"
        "&&created instanceof Probe&&created.matches(':defined')&&sheet.cssRules.length===1"
        "?'CUSTOM-ELEMENTS-OK':'CUSTOM-ELEMENTS-FAILED:'+JSON.stringify({undefinedBefore,resolved,events,existing:existing.constructed,defined:existing.matches(':defined'),existingState:__tilefinchGetCustomState(existing.__handle),createdState:__tilefinchGetCustomState(created.__handle),createdDefined:created.matches(':defined')});});"
        "class Probe extends HTMLElement{static observedAttributes=['observed'];"
        "constructor(){super();this.constructed=(this.constructed||0)+1;}"
        "connectedCallback(){events.push('connected')}disconnectedCallback(){events.push('disconnected')}"
        "attributeChangedCallback(name,oldValue,newValue){events.push('attribute:'+oldValue+':'+newValue)}}"
        "customElements.define('x-tilefinch-probe',Probe);existing.setAttribute('observed','after');"
        "const created=document.createElement('x-tilefinch-probe');document.body.appendChild(created);"
        "existing.remove();const sheet=new CSSStyleSheet();sheet.replaceSync('x{color:red}');})()";
    bool custom_elements_ok = script_runtime_evaluate_diagnostic(
        runtime, custom_elements_probe, "<custom-elements-probe>", &result);
    if (!custom_elements_ok
        || strcmp(result.summary, "CUSTOM-ELEMENTS-OK") != 0) {
        fprintf(stderr, "custom elements probe: ok=%d summary=%s error=%s\n",
                custom_elements_ok, result.summary, result.error);
    }
    CHECK(custom_elements_ok
          && strcmp(result.summary, "CUSTOM-ELEMENTS-OK") == 0);

    static const char customized_builtin_identity_probe[] =
        "(()=>{const wrong=document.createElement('div');"
        "wrong.setAttribute('is','x-tilefinch-button');"
        "const right=document.createElement('button');"
        "right.setAttribute('is','x-tilefinch-button');"
        "document.body.append(wrong,right);"
        "class TilefinchButton extends HTMLButtonElement{constructor(){"
        "super();this.upgraded=true}}"
        "customElements.define('x-tilefinch-button',TilefinchButton,"
        "{extends:'button'});customElements.upgrade(document.body);"
        "globalThis.pocSummary=!(wrong instanceof TilefinchButton)"
        "&&!wrong.upgraded&&right instanceof TilefinchButton&&right.upgraded"
        "?'CUSTOMIZED-BUILTIN-IDENTITY-OK':"
        "'CUSTOMIZED-BUILTIN-IDENTITY-FAILED:'+JSON.stringify({"
        "wrongInstance:wrong instanceof TilefinchButton,"
        "wrongUpgraded:wrong.upgraded,rightInstance:right instanceof "
        "TilefinchButton,rightUpgraded:right.upgraded,"
        "wrongState:wrong.__tilefinchCustomElementState,"
        "rightState:right.__tilefinchCustomElementState});"
        "wrong.remove();right.remove()})()";
    bool customized_builtin_ok = script_runtime_evaluate_diagnostic(
        runtime, customized_builtin_identity_probe,
        "<customized-builtin-identity-probe>", &result);
    if (!customized_builtin_ok
        || strcmp(result.summary, "CUSTOMIZED-BUILTIN-IDENTITY-OK") != 0) {
        fprintf(stderr, "customized builtin probe: ok=%d summary=%s error=%s\n",
                customized_builtin_ok, result.summary, result.error);
    }
    CHECK(customized_builtin_ok
          && strcmp(result.summary,
                    "CUSTOMIZED-BUILTIN-IDENTITY-OK") == 0);

    static const char constructed_stylesheet_probe[] =
        "(()=>{const first=new CSSStyleSheet(),second=new CSSStyleSheet();"
        "first.replaceSync('/* leading */ @import url(ignored.css);"
        ".constructed-a{color:red}.constructed-b{display:block}');"
        "const importRemoved=first.cssRules.length===2"
        "&&first.cssRules.every(rule=>rule.parentStyleSheet===first);"
        "const inserted=first.insertRule('.constructed-mid{width:3px}',1)===1"
        "&&first.cssRules[1].cssText.includes('constructed-mid');"
        "first.deleteRule(1);second.replaceSync('.constructed-last{height:4px}');"
        /* Adopted sheets live natively, outside the document tree. */
        "const styleCount=document.querySelectorAll('style').length,"
        "lastChild=document.documentElement.lastChild;"
        "document.adoptedStyleSheets=[first,second];"
        "const nodes=[],ordered=document.querySelectorAll('style').length"
        "===styleCount&&document.documentElement.lastChild===lastChild"
        "&&document.adoptedStyleSheets[0]===first"
        "&&document.adoptedStyleSheets[1]===second;"
        "first.replaceSync('.constructed-live{color:blue}');"
        "const live=first.cssRules.length===1"
        "&&first.cssRules[0].selectorText==='.constructed-live';"
        "let duplicate=false,ordinary=false,syntax=false;"
        "try{document.adoptedStyleSheets=[first,first]}catch(error){"
        "duplicate=error.name==='NotAllowedError'}"
        "try{document.adoptedStyleSheets=[document.createElement('style').sheet]}"
        "catch(error){ordinary=error.name==='NotAllowedError'}"
        /* CSS has no fatal syntax errors: the parser closes a block left
           open at the end, so replaceSync never throws SyntaxError. */
        "const broken=new CSSStyleSheet();broken.replaceSync('.broken{');"
        "syntax=broken.cssRules.length===1"
        "&&broken.cssRules[0].cssText==='.broken{}';"
        "const author=document.createElement('style');"
        "author.textContent='.author-a{color:red}@media (min-width:1px){"
        ".author-b{display:block}}';document.head.appendChild(author);"
        "const listedSheets=document.styleSheets,authorSheet=author.sheet,"
        "authorRules=authorSheet.cssRules,authorListed="
        "listedSheets.includes(authorSheet)"
        "&&document.styleSheets===listedSheets"
        "&&document.styleSheets.item(999)===null"
        "&&authorSheet instanceof CSSStyleSheet"
        "&&authorSheet.ownerNode===author&&authorSheet.href===null"
        "&&authorSheet.cssRules===authorRules"
        "&&authorSheet.cssRules.length===2"
        "&&authorSheet.cssRules[0].selectorText==='.author-a'"
        "&&authorSheet.cssRules[0].parentStyleSheet===authorSheet;"
        "const lazyAuthorTracking=authorSheet.__authorRevision===undefined"
        "&&!Object.prototype.hasOwnProperty.call(authorSheet,'__refreshAuthor');"
        "const authorText=author.textContent,authorChild=author.firstChild,"
        "observer=new MutationObserver(()=>{});"
        "observer.observe(author,{childList:true,characterData:true,subtree:true});"
        "authorSheet.insertRule('.author-inserted{height:7px}',2);"
        "const authorInserted=author.textContent===authorText"
        "&&author.firstChild===authorChild&&observer.takeRecords().length===0"
        "&&authorRules.length===3;authorSheet.deleteRule(2);"
        "const authorDeleted=author.textContent===authorText"
        "&&author.firstChild===authorChild&&observer.takeRecords().length===0"
        "&&authorRules.length===2;"
        "observer.disconnect();"
        "author.textContent='.author-live{width:4px}';"
        "const authorRefreshed=author.sheet===authorSheet"
        "&&authorSheet.cssRules.length===1"
        "&&authorSheet.cssRules[0].selectorText==='.author-live';"
        "document.adoptedStyleSheets=[second];"
        "const removed=document.adoptedStyleSheets.length===1"
        "&&document.adoptedStyleSheets[0]===second;"
        "const detachedDoc=document.implementation.createHTMLDocument(''),"
        "detached=detachedDoc.createElement('div'),oldStyle=detached.style;"
        "oldStyle.width='1px';detached.setAttribute('style','height:2px');"
        "const detachedStyleInvalidated=detached.style!==oldStyle"
        "&&detached.style.cssText.includes('height');"
        "author.remove();globalThis.pocSummary=importRemoved&&inserted"
        "&&ordered&&live&&authorListed&&authorRefreshed"
        "&&lazyAuthorTracking&&authorInserted&&authorDeleted&&detachedStyleInvalidated"
        "&&duplicate&&ordinary&&syntax&&removed"
        "?'CONSTRUCTED-STYLESHEET-OK':'CONSTRUCTED-STYLESHEET-FAILED:'"
        "+JSON.stringify({importRemoved,inserted,ordered,live,duplicate,"
        "ordinary,syntax,removed,authorListed,authorRefreshed,lazyAuthorTracking,authorInserted,"
        "authorDeleted,detachedStyleInvalidated,"
        "count:nodes.length});})()";
    bool constructed_stylesheet_ok = script_runtime_evaluate_diagnostic(
        runtime, constructed_stylesheet_probe,
        "<constructed-stylesheet-probe>", &result);
    if (!constructed_stylesheet_ok
        || strcmp(result.summary, "CONSTRUCTED-STYLESHEET-OK") != 0) {
        fprintf(stderr, "constructed stylesheet probe: ok=%d summary=%s error=%s\n",
                constructed_stylesheet_ok, result.summary, result.error);
    }
    CHECK(constructed_stylesheet_ok
          && strcmp(result.summary, "CONSTRUCTED-STYLESHEET-OK") == 0);

    static const char observer_reobserve_and_select_probe[] =
        "(()=>{const target=document.createElement('div'),"
        "select=document.createElement('select'),"
        "option=document.createElement('option'),"
        "selected=document.createElement('selectedcontent');"
        "option.value='one';option.textContent='ONE';"
        "select.append(option,selected);document.body.append(target,select);"
        "select.selectedIndex=0;let resizeDeliveries=0,mutations=0;"
        "const resize=new ResizeObserver(()=>{resizeDeliveries++;"
        "globalThis.__tilefinchReobserveDeliveries=resizeDeliveries});"
        "resize.observe(target);resize.disconnect();resize.observe(target);"
        "const mutation=new MutationObserver(items=>mutations+=items.length);"
        "mutation.observe(selected,{childList:true,subtree:true});"
        "void select.value;void select.value;__tilefinchResizeRecheck();"
        "globalThis.__tilefinchReobserveCleanup=()=>{"
        "resize.disconnect();mutation.disconnect();target.remove();select.remove();"
        "return mutations};globalThis.pocSummary='OBSERVER-REOBSERVE-PENDING'})()";
    bool observer_reobserve_ok = script_runtime_evaluate_diagnostic(
        runtime, observer_reobserve_and_select_probe,
        "<observer-reobserve-select-probe>", &result)
        && script_runtime_advance(runtime, 16, 32, &result)
        && script_runtime_evaluate_diagnostic(
            runtime,
            "(()=>{const resizeDeliveries="
            "globalThis.__tilefinchReobserveDeliveries||0,"
            "mutations=globalThis.__tilefinchReobserveCleanup();"
            "globalThis.pocSummary=resizeDeliveries===1&&mutations===0"
            "?'OBSERVER-REOBSERVE-SELECT-OK':"
            "'OBSERVER-REOBSERVE-SELECT-FAILED:'+JSON.stringify({"
            "resizeDeliveries,mutations})})()",
            "<observer-reobserve-select-result>", &result);
    if (!observer_reobserve_ok
        || strcmp(result.summary, "OBSERVER-REOBSERVE-SELECT-OK") != 0) {
        fprintf(stderr, "observer reobserve probe: ok=%d summary=%s error=%s\n",
                observer_reobserve_ok, result.summary, result.error);
    }
    CHECK(observer_reobserve_ok
          && strcmp(result.summary,
                    "OBSERVER-REOBSERVE-SELECT-OK") == 0);

    static const char event_source_pending_event_cap_probe[] =
        "(()=>{const source=new EventSource('https://events.test/feed');"
        "let errors=0;source.onerror=()=>errors++;"
        "for(let i=0;i<80&&!source._closed;i++)"
        "source._chunk('data:'+('x'.repeat(1024))+'\\n');"
        "const bounded=source._closed&&source.readyState===EventSource.CLOSED"
        "&&source._data.length===0&&source._dataBytes===0&&errors===1;"
        "source.close();globalThis.pocSummary=bounded"
        "?'EVENT-SOURCE-PENDING-CAP-OK':'EVENT-SOURCE-PENDING-CAP-FAILED'})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, event_source_pending_event_cap_probe,
              "<event-source-pending-cap-probe>", &result)
          && strcmp(result.summary,
                    "EVENT-SOURCE-PENDING-CAP-OK") == 0);

    static const char match_media_probe[] =
        "(()=>{const initial=innerWidth,"
        "range=matchMedia('(400px <= width <= 500px)'),"
        "minimum=matchMedia('(width >= 30em)'),"
        "orientation=matchMedia('screen and (orientation: landscape)'),"
        "notPrint=matchMedia('not print and (min-width: 100px)'),"
        "print=matchMedia('print and (min-width: 100px)'),"
        "monochrome=matchMedia('(monochrome)'),"
        "zeroMonochrome=matchMedia('(monochrome: 0)'),"
        "reducedMotion=matchMedia('(prefers-reduced-motion: reduce)'),"
        "ordinaryMotion=matchMedia('(prefers-reduced-motion: no-preference)');"
        "let events=0,eventShape=false;"
        "const listener={handleEvent(event){events++;eventShape="
        "event instanceof MediaQueryListEvent&&event instanceof Event"
        "&&event.media===range.media&&!event.matches}};"
        "range.addEventListener('change',listener);innerWidth=300;"
        "__tilefinchMediaRecheck();__tilefinchMediaRecheck();"
        "range.removeEventListener('change',listener);innerWidth=initial;"
        "__tilefinchMediaRecheck();"
        "globalThis.pocSummary=range instanceof EventTarget"
        "&&minimum.matches&&orientation.matches"
        "&&notPrint.matches&&!print.matches&&!monochrome.matches"
        "&&zeroMonochrome.matches&&reducedMotion.matches"
        "&&!ordinaryMotion.matches&&events===1&&eventShape"
        "?'MATCH-MEDIA-OK':'MATCH-MEDIA-FAILED:'+JSON.stringify({"
        "range:range.matches,minimum:minimum.matches,"
        "orientation:orientation.matches,notPrint:notPrint.matches,"
        "print:print.matches,monochrome:monochrome.matches,"
        "zeroMonochrome:zeroMonochrome.matches,"
        "reducedMotion:reducedMotion.matches,"
        "ordinaryMotion:ordinaryMotion.matches,events,eventShape});})()";
    bool match_media_ok = script_runtime_evaluate_diagnostic(
        runtime, match_media_probe, "<match-media-probe>", &result);
    if (!match_media_ok || strcmp(result.summary, "MATCH-MEDIA-OK") != 0) {
        fprintf(stderr, "match media probe: ok=%d summary=%s error=%s\n",
                match_media_ok, result.summary, result.error);
    }
    CHECK(match_media_ok && strcmp(result.summary, "MATCH-MEDIA-OK") == 0);

    /* The probes above intentionally retain globals, constructors, and
       detached fixtures. Start the independent wrapper-lifetime pressure
       suite in a fresh realm so its fixed heap and native budget measure
       handle recycling rather than unrelated conformance-test order. */
    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Detached nodes remain live while referenced, while unreferenced churn
       is reclaimed by wrapper finalization without aliasing old handles. */
    size_t handle_exhaustions_before = result.dom_handle_exhaustions;
    static const char dom_handle_reuse_probe[] =
        "(()=>{const host=document.createElement('div'),detached="
        "document.createElement('span'),detachedHandle=detached.__handle;"
        "document.body.appendChild(host);detached.textContent='detached';"
        "document.body.appendChild(detached);detached.remove();const stale="
        "document.createTextNode('stale');host.appendChild(stale);const "
        "staleHandle=stale.__handle;host.textContent='';let created=0;"
        "for(let i=0;i<1100;i++){const node=document.createTextNode('node-'+i);"
        "if(!node)break;host.appendChild(node);host.textContent='';created++;"
        "if((i&31)===31)__tilefinchClearNodeCache();}const replacement="
        "document.createTextNode('replacement'),replacementHandle="
        "replacement&&replacement.__handle;host.appendChild(replacement);"
        "stale.data='still-live';host.appendChild(stale);const staleSafe="
        "stale.__handle===staleHandle&&stale.data==='still-live'"
        "&&replacement.data==='replacement';"
        "detached.textContent="
        "'retained-detached';host.appendChild(detached);const detachedSafe="
        "detached.__handle===detachedHandle&&detached.textContent==="
        "'retained-detached'&&detached.parentNode?.__handle===host.__handle;"
        "globalThis.pocSummary=created===1100&&staleSafe&&detachedSafe"
        "?'DOM-HANDLE-REUSE-OK':'DOM-HANDLE-REUSE-FAILED:'"
        "+JSON.stringify({created,staleHandle,replacementHandle,staleSafe,"
        "detachedSafe,staleData:stale.data,replacementData:replacement.data,"
        "staleParent:stale.parentNode?.__handle===host.__handle});})()";
    bool dom_handle_reuse_ok = script_runtime_evaluate_diagnostic(
        runtime, dom_handle_reuse_probe, "<dom-handle-reuse-probe>",
        &result);
    if (!dom_handle_reuse_ok
        || strcmp(result.summary, "DOM-HANDLE-REUSE-OK") != 0) {
        fprintf(stderr, "DOM handle reuse probe: ok=%d summary=%s error=%s "
                "live=%zu peak=%zu high-water=%zu reuses=%zu "
                "exhaustions=%zu budget=%zu failures=%zu heap=%zu\n",
                dom_handle_reuse_ok, result.summary,
                result.error, result.dom_handle_slots_live,
                result.dom_handle_slots_peak,
                result.dom_handle_slots_high_water,
                result.dom_handle_slot_reuses,
                result.dom_handle_exhaustions, budget.current,
                budget.failure_count,
                script_runtime_heap_remaining(runtime));
    }
    CHECK(dom_handle_reuse_ok
          && strcmp(result.summary, "DOM-HANDLE-REUSE-OK") == 0
          && result.dom_handle_exhaustions == handle_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_peak
               <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_high_water
               <= SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    static const char connected_wrapper_setup[] =
        "(()=>{const node=document.createElement('section');"
        "node.setAttribute('data-tilefinch-wrapper-probe','connected');"
        "document.body.appendChild(node);globalThis.__tilefinchProbeHandle="
        "node.__handle;globalThis.__tilefinchProbeOldLease="
        "node.__tilefinchHandleLease;globalThis.pocSummary="
        "typeof WeakRef==='function'&&typeof FinalizationRegistry==='function'"
        "&&node.__tilefinchHandleLease>0"
        "?'WEAK-NODE-CACHE-SETUP-OK':'WEAK-NODE-CACHE-SETUP-FAILED';"
        "__tilefinchClearNodeCache();})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, connected_wrapper_setup,
              "<connected-wrapper-setup>", &result)
          && strcmp(result.summary, "WEAK-NODE-CACHE-SETUP-OK") == 0);
    size_t connected_preserves_before =
        result.dom_handle_connected_preserves;
    bool connected_collected = collect_and_drain_finalizers(
        runtime, &result)
        && collect_and_drain_finalizers(runtime, &result);
    if (!connected_collected || result.dom_handle_connected_preserves
        <= connected_preserves_before) {
        fprintf(stderr, "connected wrapper collection: ok=%d live=%zu "
                "peak=%zu releases=%zu preserves=%zu stale=%zu\n",
                connected_collected, result.dom_handle_slots_live,
                result.dom_handle_slots_peak,
                result.dom_handle_wrapper_releases,
                result.dom_handle_connected_preserves,
                result.dom_handle_stale_releases);
    }
    CHECK(connected_collected
          && result.dom_handle_connected_preserves
               > connected_preserves_before);

    /* Reacquiring a connected native node after its wrapper is collected
       keeps the handle stable.  The old lease models a cleanup job that was
       already queued before reacquisition; it must not release the new
       wrapper's handle. */
    static const char connected_wrapper_reacquire[] =
        "(()=>{const node=document.querySelector("
        "'[data-tilefinch-wrapper-probe=connected]'),same=node&&node.__handle"
        "===globalThis.__tilefinchProbeHandle,newLease=node&&"
        "node.__tilefinchHandleLease,staleRejected=!__tilefinchReleaseNodeWrapper("
        "globalThis.__tilefinchProbeHandle,globalThis.__tilefinchProbeOldLease);"
        "if(node){node.textContent='reacquired';node.remove();}"
        "globalThis.__tilefinchRetainedDetached=node;globalThis.pocSummary="
        "same&&newLease>globalThis.__tilefinchProbeOldLease&&staleRejected"
        "&&node.textContent==='reacquired'?'CONNECTED-REACQUIRE-OK':"
        "'CONNECTED-REACQUIRE-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, connected_wrapper_reacquire,
              "<connected-wrapper-reacquire>", &result)
          && strcmp(result.summary, "CONNECTED-REACQUIRE-OK") == 0);
    size_t wrapper_releases_before_retained =
        result.dom_handle_wrapper_releases;
    CHECK(collect_and_drain_finalizers(runtime, &result));
    /* Cache-cleared wrappers from the preceding churn may now retire here.
       Take the retained-node baseline after that intentional drain. */
    wrapper_releases_before_retained =
        result.dom_handle_wrapper_releases;
    static const char retained_detached_probe[] =
        "(()=>{const node=globalThis.__tilefinchRetainedDetached,handle="
        "node.__handle;node.textContent='retained-after-gc';"
        "document.body.appendChild(node);const usable=node.isConnected"
        "&&node.__handle===handle&&node.textContent==='retained-after-gc';"
        "node.remove();node.id='tilefinch-release-probe';node.removeAttribute('id');"
        "globalThis.__tilefinchDroppedHandle=node.__handle;"
        "globalThis.__tilefinchDroppedLease=node.__tilefinchHandleLease;"
        "globalThis.__tilefinchRetainedDetached=null;globalThis.pocSummary="
        "usable?'RETAINED-DETACHED-OK':'RETAINED-DETACHED-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, retained_detached_probe,
              "<retained-detached-probe>", &result)
          && strcmp(result.summary, "RETAINED-DETACHED-OK") == 0
          && result.dom_handle_wrapper_releases
               == wrapper_releases_before_retained);
    /* Run a separate checkpoint after dropping the final strong reference.
       FinalizationRegistry timing is deliberately unspecified, so require a
       healthy drain here and prove reclamation with the bounded multi-wave
       churn below instead of requiring this one wrapper to retire promptly. */
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, "globalThis.pocSummary='DROP-DRAIN'",
              "<retained-detached-drain>", &result)
          && strcmp(result.summary, "DROP-DRAIN") == 0);
    CHECK(collect_and_drain_finalizers(runtime, &result));

    size_t churn_releases_before = result.dom_handle_wrapper_releases;
    size_t churn_reuses_before = result.dom_handle_slot_reuses;
    size_t churn_exhaustions_before = result.dom_handle_exhaustions;
    static const char removed_wrapper_churn[] =
        "(()=>{let created=0;for(let i=0;i<600;i++){const node="
        "document.createElement('i');if(!node)break;"
        "document.body.appendChild(node);node.remove();created++;}"
        "globalThis.pocSummary=created===600?'REMOVED-WRAPPER-CHURN-OK':"
        "'REMOVED-WRAPPER-CHURN-FAILED:'+created;})()";
    for (size_t round = 0; round < 4; round++) {
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, removed_wrapper_churn,
            "<removed-wrapper-churn>", &result);
        bool summarized = evaluated
            && strcmp(result.summary, "REMOVED-WRAPPER-CHURN-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!evaluated || !summarized || !collected) {
            fprintf(stderr, "removed wrapper churn round=%zu evaluated=%d "
                    "summary=%s error=%s collected=%d live=%zu peak=%zu "
                    "releases=%zu reuses=%zu exhaustions=%zu\n",
                    round, evaluated, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_slots_peak,
                    result.dom_handle_wrapper_releases,
                    result.dom_handle_slot_reuses,
                    result.dom_handle_exhaustions);
        }
        CHECK(evaluated && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases
               >= churn_releases_before + 2400
          && result.dom_handle_slot_reuses >= churn_reuses_before + 1800
          && result.dom_handle_exhaustions == churn_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY
          && result.dom_handle_slots_peak
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    /* Handle lookup goes through a pointer index. Register live nodes
       interleaved with detached throwaways, let collection retire the
       throwaways (deleting index entries between the live ones), then drop
       every wrapper: re-registering each live node must find its handle. */
    static const char dom_handle_index_setup[] =
        "(()=>{const host=document.createElement('div');host.id='hx';"
        "document.body.appendChild(host);const kept=[];"
        "for(let i=0;i<2000;i++){const s=document.createElement('span');"
        "host.appendChild(s);kept.push(s.__handle);"
        "document.createTextNode('t'+i).__handle;}"
        "globalThis.__tilefinchIndexKept=kept;__tilefinchClearNodeCache();"
        "globalThis.pocSummary='DOM-HANDLE-INDEX-SETUP';})()";
    static const char dom_handle_index_verify[] =
        "(()=>{__tilefinchClearNodeCache();const kept="
        "globalThis.__tilefinchIndexKept,seen=new Set();let same=0,i=0;"
        "for(let c=document.getElementById('hx').firstChild;c;"
        "c=c.nextSibling,i++){const h=c.__handle;if(h===kept[i])same++;"
        "seen.add(h);}document.getElementById('hx').remove();"
        "globalThis.pocSummary=same===2000&&seen.size===2000"
        "?'DOM-HANDLE-INDEX-OK':'DOM-HANDLE-INDEX-FAILED:'+same+'/'"
        "+seen.size;})()";
    size_t index_releases_before = result.dom_handle_wrapper_releases;
    bool index_setup = script_runtime_evaluate_diagnostic(
        runtime, dom_handle_index_setup, "<dom-handle-index-setup>",
        &result);
    bool index_collected = index_setup
        && collect_and_drain_finalizers(runtime, &result);
    if (!index_collected
        || result.dom_handle_wrapper_releases
               < index_releases_before + 256) {
        fprintf(stderr, "DOM handle index setup=%d summary=%s error=%s "
                "collected=%d releases=%zu->%zu live=%zu\n",
                index_setup, result.summary, result.error, index_collected,
                index_releases_before, result.dom_handle_wrapper_releases,
                result.dom_handle_slots_live);
    }
    CHECK(index_collected
          && result.dom_handle_wrapper_releases
                 >= index_releases_before + 256);
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, dom_handle_index_verify, "<dom-handle-index-verify>",
              &result));
    if (strcmp(result.summary, "DOM-HANDLE-INDEX-OK") != 0)
        fprintf(stderr, "DOM handle index probe: %s error=%s\n",
                result.summary, result.error);
    CHECK(strcmp(result.summary, "DOM-HANDLE-INDEX-OK") == 0);

    /* Start detached-subtree ownership in a fresh bounded realm so it
       measures its own transient rather than unrelated test-order debt. */
    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 12u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Descendants of a detached root still have native parent pointers, but
       none is connected to the document.  All three wrappers in every
       removed subtree must therefore become recyclable after collection. */
    size_t subtree_releases_before = result.dom_handle_wrapper_releases;
    size_t subtree_exhaustions_before = result.dom_handle_exhaustions;
    static const char detached_subtree_churn[] =
        "(()=>{let created=0;for(let i=0;i<100;i++){const root="
        "document.createElement('div'),child=document.createElement('span'),"
        "leaf=document.createTextNode('leaf');child.appendChild(leaf);"
        "root.appendChild(child);document.body.appendChild(root);root.remove();"
        "created+=3;}__tilefinchClearNodeCache();globalThis.pocSummary=created===300?"
        "'DETACHED-SUBTREE-CHURN-OK':'DETACHED-SUBTREE-CHURN-FAILED:'"
        "+created;})()";
    /* The preceding four 600-wrapper waves already prove cross-checkpoint
       recycling beyond the realm cap. This independent three-level wave
       validates detached-subtree ownership and bounded handle shape. */
    for (size_t round = 0; round < 1; round++) {
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, detached_subtree_churn,
            "<detached-subtree-churn>", &result);
        bool summarized = evaluated
            && strcmp(result.summary, "DETACHED-SUBTREE-CHURN-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!evaluated || !summarized || !collected) {
            fprintf(stderr, "detached subtree churn round=%zu evaluated=%d "
                    "summary=%s error=%s collected=%d live=%zu peak=%zu "
                    "budget=%zu failures=%zu heap-remaining=%zu\n",
                    round, evaluated, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_slots_peak, budget.current,
                    budget.failure_count,
                    script_runtime_heap_remaining(runtime));
        }
        CHECK(evaluated && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases >= subtree_releases_before
          && result.dom_handle_exhaustions == subtree_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 12u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Explicit wrapper retirement follows the same native invalidation path
       as the finalizers exercised by the preceding 2400-wrapper churn.
       Listener, handler, and form-state maps have fixed 128-entry bounds and
       must be purged as each detached identity retires. */
    static const char native_state_churn_first[] =
        "(()=>{const callback=globalThis.__tilefinchStateCallback||="
        "function(){};for(let i=0;i<96;i++){const node="
        "document.createElement('i');node.addEventListener('click',callback);"
        "node.onclick=callback;node.value='v'+i;document.body.appendChild(node);"
        "node.remove();__tilefinchReleaseNodeWrapper(node.__handle,"
        "node.__tilefinchHandleLease);}"
        "__tilefinchClearNodeCache();globalThis.pocSummary="
        "'NATIVE-STATE-FIRST-OK';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_state_churn_first,
              "<native-state-churn-first>", &result)
          && strcmp(result.summary, "NATIVE-STATE-FIRST-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));
    static const char native_state_churn_second[] =
        "(()=>{const before={l:__tilefinchRetentionStats.listenerDrops,"
        "h:__tilefinchRetentionStats.handlerDrops},callback="
        "globalThis.__tilefinchStateCallback;"
        "for(let i=0;i<96;i++){const node=document.createElement('i');"
        "node.addEventListener('click',callback);node.onclick=callback;"
        "node.value='v'+i;document.body.appendChild(node);node.remove();}"
        "globalThis.pocSummary=__tilefinchRetentionStats.listenerDrops===before.l"
        "&&__tilefinchRetentionStats.handlerDrops===before.h?"
        "'NATIVE-STATE-RETIREMENT-OK':'NATIVE-STATE-RETIREMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_state_churn_second,
              "<native-state-churn-second>", &result)
          && strcmp(result.summary, "NATIVE-STATE-RETIREMENT-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Explicitly retiring a detached child and then its root invalidates every
       registered handle in the subtree. Native invalidation must retire every
       child-keyed listener, handler, and form-state entry. A later batch keeps
       its detached wrappers alive through the evaluation and verifies those
       still-live states are counted without overflowing their bounded maps. */
    static const char native_subtree_churn_first[] =
        "(()=>{const callback=()=>{};for(let i=0;i<80;i++){const root="
        "document.createElement('div'),child=document.createElement('input');"
        "child.addEventListener('click',callback);child.onclick=callback;"
        "child.value='v'+i;root.appendChild(child);document.body.appendChild(root);"
        "root.remove();__tilefinchReleaseNodeWrapper(child.__handle,"
        "child.__tilefinchHandleLease);__tilefinchReleaseNodeWrapper(root.__handle,"
        "root.__tilefinchHandleLease);}__tilefinchClearNodeCache();globalThis.pocSummary="
        "'NATIVE-SUBTREE-FIRST-OK';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_subtree_churn_first,
              "<native-subtree-churn-first>", &result)
          && strcmp(result.summary, "NATIVE-SUBTREE-FIRST-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));
    static const char native_subtree_churn_second[] =
        "(()=>{const before={l:__tilefinchRetentionStats.listenerDrops,"
        "h:__tilefinchRetentionStats.handlerDrops,"
        "nl:__tilefinchRootCensus.nativeListenerTargets,"
        "nh:__tilefinchRootCensus.nativeHandlerTargets},callback=()=>{};"
        "globalThis.__tilefinchNativeSubtreeBefore=before;"
        "for(let i=0;i<64;i++){const root=document.createElement('div'),"
        "child=document.createElement('input');child.addEventListener("
        "'click',callback);child.onclick=callback;child.value='v'+i;"
        "root.appendChild(child);document.body.appendChild(root);root.remove();}"
        "__tilefinchClearNodeCache();"
        "globalThis.pocSummary=__tilefinchRetentionStats.listenerDrops===before.l"
        "&&__tilefinchRetentionStats.handlerDrops===before.h"
        "&&__tilefinchRootCensus.nativeListenerTargets===before.nl+64"
        "&&__tilefinchRootCensus.nativeHandlerTargets===before.nh+64?"
        "'NATIVE-SUBTREE-RETIREMENT-OK':"
        "'NATIVE-SUBTREE-RETIREMENT-FAILED';})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, native_subtree_churn_second,
              "<native-subtree-churn-second>", &result)
          && strcmp(result.summary,
                    "NATIVE-SUBTREE-RETIREMENT-OK") == 0
          && collect_and_drain_finalizers(runtime, &result));

    script_runtime_destroy(runtime);
    runtime = NULL;
    document_destroy(&document);
    CHECK(budget.current == 0
          && document_parse(
              &document, &budget, html, sizeof(html) - 1, 17));
    document_bytes = budget.current;
    runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 1000,
        "https://example.test/", &options, &result);
    CHECK(runtime != NULL && result.success);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(script != NULL);

    /* Replacing an element with another element carrying the same id must
       create a distinct DOM identity. The old detached wrapper stays usable
       until the probe explicitly retires it after the identity assertion. */
    size_t replacement_releases_before = result.dom_handle_wrapper_releases;
    size_t replacement_exhaustions_before = result.dom_handle_exhaustions;
    static const char same_id_replacement_churn[] =
        "(()=>{const host=document.createElement('section');"
        "document.body.appendChild(host);let retained="
        "document.createElement('div');retained.setAttribute("
        "'id','tilefinch-stable-rebind');"
        "host.appendChild(retained);let replaced=0,ok=true;"
        "for(let i=0;i<150;i++){const old=retained,"
        "oldHandle=old.__handle;old.remove();"
        "host.innerHTML='<div id=\"tilefinch-stable-rebind\"></div>';"
        "const found=document.getElementById('tilefinch-stable-rebind');"
        "old.textContent='detached-'+i;"
        "if(!found||found===old||found.__handle===oldHandle||"
        "old.textContent!=='detached-'+i){ok=false;break;}"
        "__tilefinchReleaseNodeWrapper(old.__handle,old.__tilefinchHandleLease);"
        "retained=found;replaced++;}retained?.removeAttribute('id');"
        "retained?.remove();if(retained)__tilefinchReleaseNodeWrapper("
        "retained.__handle,retained.__tilefinchHandleLease);host.remove();"
        "__tilefinchReleaseNodeWrapper(host.__handle,host.__tilefinchHandleLease);"
        "globalThis.pocSummary="
        "ok&&replaced===150?'SAME-ID-REPLACEMENT-OK':"
        "'SAME-ID-REPLACEMENT-FAILED:'+JSON.stringify({ok,replaced});})()";
    for (size_t round = 0; round < 1; round++) {
        bool replacement_ok = script_runtime_evaluate_diagnostic(
            runtime, same_id_replacement_churn,
            "<same-id-replacement-churn>", &result);
        bool summarized = replacement_ok
            && strcmp(result.summary, "SAME-ID-REPLACEMENT-OK") == 0;
        bool collected = summarized
            && collect_and_drain_finalizers(runtime, &result);
        if (!replacement_ok || !summarized || !collected) {
            fprintf(stderr, "same-id replacement round=%zu ok=%d "
                    "summary=%s error=%s collected=%d live=%zu "
                    "releases=%zu exhaustions=%zu\n",
                    round, replacement_ok, result.summary, result.error,
                    collected, result.dom_handle_slots_live,
                    result.dom_handle_wrapper_releases,
                    result.dom_handle_exhaustions);
        }
        CHECK(replacement_ok && summarized && collected);
    }
    CHECK(result.dom_handle_wrapper_releases
               >= replacement_releases_before + 150
          && result.dom_handle_exhaustions == replacement_exhaustions_before
          && result.dom_handle_slots_live
               < SCRIPT_DOM_HANDLE_SLOT_CAPACITY);

    /* Page capability diagnostics may wrap classic scripts in a `with`
       environment, but modules are intrinsically strict and must be compiled
       unchanged.  This caught every diagnostic-enabled module failing before
       its first statement on modern application shells. */
    static const char page_trace_setup[] =
        "globalThis.__tilefinchPageTrace={seen:new Map(),missing:new Set(),"
        "events:[]};globalThis.__tilefinchGlobalProxy=new Proxy(globalThis,{"
        "has(target,key){return Reflect.has(target,key)},"
        "get(target,key,receiver){return Reflect.get(target,key,receiver)}});";
    static const char traced_module[] =
        "globalThis.pocSummary='TRACED-MODULE-EXECUTED';"
        "export const ready=true;";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, page_trace_setup, "<page-trace-setup>", &result)
          && script_runtime_evaluate_external_typed(
              runtime, script, traced_module, sizeof(traced_module) - 1,
              "https://example.test/traced-module.js", true, &result)
          && result.success
          && strcmp(result.summary, "TRACED-MODULE-EXECUTED") == 0
          && result.last_compile_source_kind
               == SCRIPT_COMPILE_SOURCE_MODULE);

    puts("test: JavaScript browser identity matches native client hints");
    static const char browser_identity_probe[] =
        "const d=navigator.userAgentData,b=d.brands[0];let navigatorIllegal=false;"
        "try{Object.getOwnPropertyDescriptor(Navigator.prototype,'userAgent')."
        "get.call(Object.create(Navigator.prototype))}catch(error){"
        "navigatorIllegal=error instanceof TypeError}const authenticNavigator="
        "navigator,navigatorDescriptor=Object.getOwnPropertyDescriptor("
        "globalThis,'navigator'),languagesGetter=Object.getOwnPropertyDescriptor("
        "Navigator.prototype,'languages').get,brandsGetter=Object."
        "getOwnPropertyDescriptor(NavigatorUAData.prototype,'brands').get,"
        "forgedNavigator=Object.create(Navigator.prototype),forgedUAData=Object."
        "create(NavigatorUAData.prototype);Object.defineProperty(forgedNavigator,"
        "'userAgentData',{configurable:true,value:forgedUAData});let authenticOk="
        "false,forgedNavigatorRejected=false,forgedUARejected=false;try{Object."
        "defineProperty(globalThis,'navigator',{configurable:true,enumerable:"
        "navigatorDescriptor.enumerable,writable:false,value:forgedNavigator});"
        "authenticOk=languagesGetter.call(authenticNavigator).join(',')==="
        "'en-US,en'&&brandsGetter.call(d)[0].brand===b.brand;try{languagesGetter."
        "call(forgedNavigator)}catch(error){forgedNavigatorRejected=error "
        "instanceof TypeError}try{brandsGetter.call(forgedUAData)}catch(error){"
        "forgedUARejected=error instanceof TypeError}}finally{Object."
        "defineProperty(globalThis,'navigator',navigatorDescriptor)}const "
        "navigatorBrandStable=authenticOk&&forgedNavigatorRejected&&"
        "forgedUARejected;"
        "d.getHighEntropyValues(['uaFullVersion','fullVersionList'])"
        ".then(v=>globalThis.pocSummary=navigatorIllegal&&navigatorBrandStable?"
        "b.brand+'|'+b.version+'|'+v.uaFullVersion+'|'"
        "+v.fullVersionList[0].brand+'|'"
        "+v.fullVersionList[0].version+'|'+navigator.language+'|'"
        "+navigator.languages.join(','):'NAVIGATOR-BRAND-FAILED');";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, browser_identity_probe, "<browser-identity-probe>",
              &result)
          && script_runtime_advance(runtime, 0, 16, &result)
          && strcmp(result.summary,
                    TILEFINCH_BROWSER_BRAND "|"
                    TILEFINCH_BROWSER_BRAND_VERSION "|"
                    TILEFINCH_BROWSER_FULL_VERSION "|"
                    TILEFINCH_BROWSER_BRAND "|"
                    TILEFINCH_BROWSER_FULL_VERSION "|en-US|en-US,en") == 0);

    puts("test: UA client hints preserve Web IDL and bounded values");
    static const char ua_client_hints_probe[] =
        "(()=>{const data=navigator.userAgentData,proto="
        "NavigatorUAData.prototype,accesses=[],hints={"
        "[Symbol.iterator](){accesses.push('iterator');let at=0;const values="
        "['architecture','formFactors','wow64'];return{next(){accesses.push("
        "'next'+at);return at<values.length?{value:{toString(){accesses.push("
        "'string'+at);return values[at++]}},done:false}:{done:true}}}}};"
        "const forged=Object.create(proto);let getterIllegal=0;for(const name of "
        "['brands','mobile','platform'])try{Object.getOwnPropertyDescriptor("
        "proto,name).get.call(forged)}catch(error){if(error instanceof TypeError)"
        "getterIllegal++}"
        "const rejected=invoke=>{let promise;try{promise=invoke()}catch(_){return "
        "Promise.resolve(false)}return promise instanceof Promise?promise.then("
        "()=>false,error=>error instanceof TypeError):Promise.resolve(false)},"
        "order=[],timed=data.getHighEntropyValues([]).then(()=>order.push('ua'));"
        "queueMicrotask(()=>order.push('micro'));Promise.all([data."
        "getHighEntropyValues(hints),data.getHighEntropyValues(["
        "'fullVersionList']),rejected(()=>proto.getHighEntropyValues.call(forged,"
        "[])),rejected(()=>data.getHighEntropyValues([Symbol('hint')])),"
        "rejected(()=>data.getHighEntropyValues({[Symbol.iterator](){throw new "
        "TypeError('iterator')}})),rejected(()=>data.getHighEntropyValues()),"
        "timed]).then(values=>{"
        "const first=values[0],second=values[1];globalThis.pocSummary="
        "getterIllegal===3&&values[2]&&values[3]&&values[4]&&values[5]&&"
        "order.join(',')==='micro,ua'&&accesses.join(',')==="
        "'iterator,next0,string0,next1,string1,next2,string2,next3'&&"
        "first.architecture==='MIPS'&&Array.isArray(first.formFactors)&&"
        "first.formFactors.length===1&&first.formFactors[0]==='Mobile'&&"
        "first.wow64===false&&first.brands===data.brands&&"
        "second.fullVersionList.length===data.brands.length?"
        "'UA-CLIENT-HINTS-OK':'UA-CLIENT-HINTS-FAILED:'+JSON.stringify({"
        "getterIllegal,rejections:values.slice(2,6),order,accesses,first,"
        "second})})})()";
    CHECK(script_runtime_evaluate_diagnostic(
              runtime, ua_client_hints_probe, "<ua-client-hints-probe>",
              &result));
    for (size_t tick = 0; tick < 8
         && strcmp(result.summary, "UA-CLIENT-HINTS-OK") != 0; tick++)
        CHECK(script_runtime_advance(runtime, 0, 32, &result));
    if (strcmp(result.summary, "UA-CLIENT-HINTS-OK") != 0)
        fprintf(stderr, "UA client hints summary: %s error=%s\n",
                result.summary, result.error);
    CHECK(strcmp(result.summary, "UA-CLIENT-HINTS-OK") == 0);

    puts("test: speech synthesis degrades through a bounded empty engine");
    static const char speech_synthesis_probe[] =
        "(()=>{const synth=speechSynthesis,u=new SpeechSynthesisUtterance("
        "'hello');let error='',illegal=false;u.onerror=event=>{error="
        "event.error};try{new SpeechSynthesis}catch(_){illegal=true}"
        "synth.speak(u);setTimeout(()=>{globalThis.pocSummary=["
        "synth instanceof SpeechSynthesis,Object.prototype.toString.call("
        "synth)==='[object SpeechSynthesis]',synth.getVoices().length===0,"
        "!synth.pending,!synth.speaking,!synth.paused,u instanceof "
        "EventTarget,u.text==='hello',error==='synthesis-unavailable',"
        "illegal].every(Boolean)?'SPEECH-EMPTY-OK':'SPEECH-EMPTY-FAILED'"
        "},0)})()";
    bool speech_synthesis_ok = script_runtime_evaluate_diagnostic(
        runtime, speech_synthesis_probe, "<speech-synthesis-probe>", &result);
    for (size_t tick = 0; speech_synthesis_ok && tick < 8
         && strcmp(result.summary, "SPEECH-EMPTY-OK") != 0; tick++) {
        speech_synthesis_ok = script_runtime_advance(
            runtime, 0, 64, &result);
    }
    CHECK(speech_synthesis_ok
          && strcmp(result.summary, "SPEECH-EMPTY-OK") == 0);

    puts("test: MediaSource probes do not advertise an absent byte stream");
    static const char media_source_probe[] =
        "(()=>{const source=new MediaSource;let state='';try{"
        "source.addSourceBuffer('video/mp4; codecs=\"avc1.42E01E\"')}"
        "catch(error){state=error.name}globalThis.pocSummary=["
        "MediaSource.isTypeSupported('video/mp4')===false,"
        "MediaSource.canConstructInDedicatedWorker===false,"
        "source.readyState==='closed',Number.isNaN(source.duration),"
        "source.sourceBuffers===source.activeSourceBuffers,"
        "source.sourceBuffers.length===0,state==='InvalidStateError',"
        "Object.prototype.toString.call(source)==='[object MediaSource]'"
        "].every(Boolean)?'MEDIA-SOURCE-EMPTY-OK':'MEDIA-SOURCE-EMPTY-FAILED'"
        "})()";
    bool media_source_ok = script_runtime_evaluate_diagnostic(
        runtime, media_source_probe, "<media-source-probe>", &result);
    if (!media_source_ok
        || strcmp(result.summary, "MEDIA-SOURCE-EMPTY-OK") != 0) {
        fprintf(stderr, "MediaSource probe: ok=%d summary=%s error=%s\n",
                media_source_ok, result.summary, result.error);
    }
    CHECK(media_source_ok
          && strcmp(result.summary, "MEDIA-SOURCE-EMPTY-OK") == 0);

    puts("test: detached runtimes reject new network work");
    script_runtime_detach_document(runtime, &document);
    static const char detached_fetch_probe[] =
        "fetch('/after-detach').then(()=>{"
        "globalThis.pocSummary='DETACHED-FETCH-RESOLVED'"
        "}).catch(error=>{globalThis.pocSummary=error instanceof TypeError"
        "?'DETACHED-FETCH-REJECTED':'DETACHED-FETCH-WRONG:'+error});";
    bool detached_fetch_ok = script_runtime_evaluate_diagnostic(
        runtime, detached_fetch_probe, "<detached-fetch-probe>", &result);
    for (size_t tick = 0; detached_fetch_ok && tick < 8
         && strcmp(result.summary, "DETACHED-FETCH-REJECTED") != 0; tick++) {
        detached_fetch_ok = script_runtime_advance(runtime, 0, 64, &result);
    }
    CHECK(detached_fetch_ok
          && strcmp(result.summary, "DETACHED-FETCH-REJECTED") == 0
          && result.async_network_active_native == 0
          && result.async_network_pending_logical == 0);

    script_runtime_destroy(runtime);
    runtime = NULL;

    puts("test: classic external bytecode restores in a new realm");
    BrowserSession bytecode_session = {0};
    ScriptRuntimeOptions bytecode_options = options;
    bytecode_options.session = &bytecode_session;
    static const char cached_script_source[] =
        "globalThis.pocSummary='CLASSIC-BYTECODE-CACHE-OK'";
    static const char cached_script_url[] =
        "https://example.test/shared-classic.js";
    size_t bytecode_baseline = budget.current;
    CHECK(browser_session_init(
              &bytecode_session, &budget, 512u * 1024u)
          && browser_session_cache_put_http(
              &bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, "cache-v1", NULL,
              "text/javascript", "public,max-age=3600", NULL, 1));
    ScriptResult first_bytecode_result = {0};
    ScriptRuntime *first_bytecode_runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &bytecode_options,
        &first_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(first_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              first_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, false, &first_bytecode_result)
          && strcmp(first_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && first_bytecode_result.external_script_bytecode_cache_misses == 1
          && first_bytecode_result.external_script_bytecode_deferred == 1
          && first_bytecode_result.external_script_bytecode_cache_hits == 0);
    /* The page's idle turn stores what the load queued. */
    CHECK(script_runtime_store_pending_bytecode(first_bytecode_runtime,
                                                &first_bytecode_result)
          && first_bytecode_result.external_script_bytecode_cache_stores
                 == 1);
    size_t first_compile_attempts =
        first_bytecode_result.host_compile_attempts;
    script_runtime_destroy(first_bytecode_runtime);

    ScriptResult second_bytecode_result = {0};
    ScriptRuntime *second_bytecode_runtime = script_runtime_create_configured(
        &document, &budget, 16u * MIB, 8000,
        "https://example.test/", &bytecode_options,
        &second_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(second_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              second_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, false, &second_bytecode_result)
          && strcmp(second_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && second_bytecode_result.external_script_bytecode_cache_hits == 1
          && second_bytecode_result.external_script_bytecode_cache_misses == 0
          && second_bytecode_result.external_script_bytecode_cache_bytes != 0
          && second_bytecode_result.host_compile_attempts + 1
                 == first_compile_attempts);
    script_runtime_destroy(second_bytecode_runtime);
    browser_session_destroy(&bytecode_session);
    CHECK(budget.current == bytecode_baseline);

    puts("test: install-time classic bytecode restores without compilation");
    BrowserSession installed_bytecode_session = {0};
    CHECK(browser_session_init(
              &installed_bytecode_session, &budget, 512u * 1024u)
          && browser_session_cache_put_http(
              &installed_bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, "cache-v1", NULL,
              "text/javascript", "public,max-age=3600", NULL, 1));
    unsigned char *installed_bytecode = NULL;
    size_t installed_bytecode_length = 0;
    /* Session/cache bodies are byte spans, not C strings. Keep this allocation
       exact-sized so ASan detects a compiler read beyond the caller's span. */
    char *installed_source_span = budget_malloc(
        &budget, sizeof(cached_script_source) - 1u);
    CHECK(installed_source_span != NULL);
    memcpy(installed_source_span, cached_script_source,
           sizeof(cached_script_source) - 1u);
    size_t installed_compile_baseline = budget.current;
    for (size_t failure = 0; failure < 8; failure++) {
        budget_inject_failure_after(&budget, failure);
        CHECK(!script_compile_classic_bytecode(
                  &budget, installed_source_span,
                  sizeof(cached_script_source) - 1u, cached_script_url,
                  128u * 1024u, &installed_bytecode,
                  &installed_bytecode_length)
              && installed_bytecode == NULL && installed_bytecode_length == 0
              && budget.current == installed_compile_baseline);
        budget_clear_failure_injection(&budget);
    }
    CHECK(!script_compile_classic_bytecode(
              &budget, installed_source_span, SIZE_MAX, cached_script_url,
              128u * 1024u, &installed_bytecode, &installed_bytecode_length)
          && installed_bytecode == NULL && installed_bytecode_length == 0
          && budget.current == installed_compile_baseline);
    static const char invalid_compile_span[] = {'{'};
    CHECK(!script_compile_classic_bytecode(
              &budget, invalid_compile_span, sizeof(invalid_compile_span),
              cached_script_url, 128u * 1024u, &installed_bytecode,
              &installed_bytecode_length)
          && installed_bytecode == NULL && installed_bytecode_length == 0
          && budget.current == installed_compile_baseline);
    CHECK(script_compile_classic_bytecode(
              &budget, installed_source_span,
              sizeof(cached_script_source) - 1, cached_script_url,
              128u * 1024u, &installed_bytecode,
              &installed_bytecode_length)
          && installed_bytecode != NULL && installed_bytecode_length != 0
          && browser_session_classic_script_bytecode_put(
              &installed_bytecode_session, cached_script_url,
              (const unsigned char *) cached_script_source,
              sizeof(cached_script_source) - 1, installed_bytecode,
              installed_bytecode_length));
    budget_free(&budget, installed_source_span);
    budget_free(&budget, installed_bytecode);
    ScriptRuntimeOptions installed_bytecode_options = options;
    installed_bytecode_options.session = &installed_bytecode_session;
    ScriptResult installed_bytecode_result = {0};
    ScriptRuntime *installed_bytecode_runtime =
        script_runtime_create_configured(
            &document, &budget, 16u * MIB, 8000,
            "https://example.test/", &installed_bytecode_options,
            &installed_bytecode_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(installed_bytecode_runtime != NULL && script != NULL
          && script_runtime_evaluate_external_classic_cached(
              installed_bytecode_runtime, script, cached_script_source,
              sizeof(cached_script_source) - 1, cached_script_url,
              cached_script_url, false, &installed_bytecode_result)
          && strcmp(installed_bytecode_result.summary,
                    "CLASSIC-BYTECODE-CACHE-OK") == 0
          && installed_bytecode_result.external_script_bytecode_cache_hits == 1
          && installed_bytecode_result.external_script_bytecode_cache_misses
                 == 0);
    script_runtime_destroy(installed_bytecode_runtime);
    browser_session_destroy(&installed_bytecode_session);
    CHECK(budget.current == bytecode_baseline);

    puts("test: a storage change the Memory Stick refuses reaches the page");
    {
        static const char storage_site[] = "https://example.test/";
        char storage_directory[64];
        snprintf(storage_directory, sizeof(storage_directory),
                 "/tmp/tilefinch-js-storage-XXXXXX");
        BrowserSession storage_session = {0};
        ScriptRuntimeOptions storage_options = options;
        storage_options.session = &storage_session;
        CHECK(mkdtemp(storage_directory) != NULL
              && browser_session_init(&storage_session, &budget,
                                      512u * 1024u)
              && browser_session_site_storage_configure(
                     &storage_session, storage_directory)
              && browser_session_site_storage_set_policy(
                     &storage_session, storage_site,
                     BROWSER_SITE_STORAGE_STICK));
        ScriptResult storage_result = {0};
        ScriptRuntime *storage_runtime = script_runtime_create_configured(
            &document, &budget, 16u * MIB, 8000, storage_site,
            &storage_options, &storage_result);
        CHECK(storage_runtime != NULL
              && script_runtime_evaluate_diagnostic(
                     storage_runtime,
                     "localStorage.setItem('keep','1');"
                     "localStorage.setItem('other','2')",
                     "<storage-seed>", &storage_result));
        /* Refused: the call throws and the items stay. */
        tilefinch_test_faults()->fail_site_storage_appends = 1;
        CHECK(script_runtime_evaluate_diagnostic(
                  storage_runtime,
                  "(()=>{let name='none';try{localStorage.removeItem('keep')}"
                  "catch(error){name=error.name}globalThis.pocSummary=name+"
                  "','+localStorage.getItem('keep')})()",
                  "<storage-remove-refused>", &storage_result)
              && strcmp(storage_result.summary, "UnknownError,1") == 0);
        tilefinch_test_faults()->fail_site_storage_appends = 2;
        CHECK(script_runtime_evaluate_diagnostic(
                  storage_runtime,
                  "(()=>{let name='none';try{localStorage.clear()}"
                  "catch(error){name=error.name}globalThis.pocSummary=name+"
                  "','+localStorage.length})()",
                  "<storage-clear-refused>", &storage_result)
              && strcmp(storage_result.summary, "UnknownError,2") == 0);
        /* Accepted: no exception, and the items are gone. */
        CHECK(script_runtime_evaluate_diagnostic(
                  storage_runtime,
                  "localStorage.removeItem('keep');localStorage.clear();"
                  "globalThis.pocSummary='cleared,'+localStorage.length",
                  "<storage-clear>", &storage_result)
              && strcmp(storage_result.summary, "cleared,0") == 0);
        script_runtime_destroy(storage_runtime);
        CHECK(browser_session_site_storage_forget(&storage_session,
                                                  storage_site));
        browser_session_destroy(&storage_session);
        rmdir(storage_directory);
        CHECK(budget.current == bytecode_baseline);
    }

    size_t module_budget_baseline = budget.current;

    /* Module-map entries intentionally live for their realm's lifetime.
       Exercise the two-root delayed-import scenario in its own bounded realm
       and prove that destroying it returns every native allocation. */
    ScriptResult module_result;
    ScriptRuntime *module_runtime = script_runtime_create_configured(
        &document, &budget, 4u * MIB, 1000,
        "https://example.test/", &options, &module_result);
    script = find_script(lxb_dom_interface_node(document.html));
    CHECK(module_runtime != NULL && module_result.success
          && script != NULL
          && test_delayed_module_request_contexts(
              module_runtime, script, &module_result));
    script_runtime_destroy(module_runtime);
    CHECK(budget.current == module_budget_baseline);

    /* FinalizationRegistry cleanup is a native QuickJS job: an exception from
       its callback is returned directly by JS_ExecutePendingJob. This tests
       the host's fatal/nonfatal boundary without a private hook or changing
       the browser's queueMicrotask implementation. */
    /* Parser interrupt polls make a blown creation deadline observable
       during bootstrap compiles, so the watchdog budget must genuinely
       cover runtime creation on unoptimized builds. */
    ScriptResult job_result;
    ScriptRuntime *job_runtime = script_runtime_create_configured(
        &document, &budget, 8u * MIB, 1000,
        "https://job-exception.test/", &options, &job_result);
    CHECK(job_runtime != NULL && job_result.success);
    static const char install_authored_finalizer[] =
        "globalThis.pocSummary='AUTHORED-FINALIZER-PENDING';"
        "globalThis.__authoredFinalizer=new FinalizationRegistry(()=>{"
        "Promise.resolve().then(()=>{globalThis.pocSummary="
        "'AUTHORED-FINALIZER-CONTINUED'});"
        "throw new Error('author says out of memory')});"
        "globalThis.__authoredFinalizerTarget={};"
        "__authoredFinalizerTarget.self=__authoredFinalizerTarget;"
        "__authoredFinalizer.register(__authoredFinalizerTarget,'held');";
    CHECK(script_runtime_evaluate_diagnostic(
              job_runtime, install_authored_finalizer,
              "<install-authored-finalizer>", &job_result)
          && script_runtime_evaluate_diagnostic(
              job_runtime, "globalThis.__authoredFinalizerTarget=null",
              "<release-authored-finalizer>", &job_result));
    size_t authored_failures_before = budget.failure_count;
    size_t authored_errors_before = job_result.uncaught_callback_errors;
    (void) script_runtime_collect_and_trim(job_runtime);
    CHECK(script_runtime_advance(job_runtime, 0, 8, &job_result)
          && job_result.success
          && strcmp(job_result.summary,
                    "AUTHORED-FINALIZER-CONTINUED") == 0
          && job_result.uncaught_callback_errors
                 == authored_errors_before + 1
          && strstr(job_result.last_uncaught_callback_error,
                    "author says out of memory") != NULL
          && budget.failure_count == authored_failures_before);

    /* A completed timeout is retained as diagnostic history until the next
       public result update. It must not make an unrelated job in a newly
       armed watchdog slice fatal. Keep this target cyclic so collection, and
       therefore its native cleanup job, happens only after the timeout. */
    static const char install_stale_finalizer[] =
        "globalThis.pocSummary='STALE-FINALIZER-PENDING';"
        "globalThis.__staleFinalizer=new FinalizationRegistry(()=>{"
        "Promise.resolve().then(()=>{globalThis.pocSummary="
        "'STALE-FINALIZER-CONTINUED'});"
        "throw new Error('ordinary author finalizer')});"
        "globalThis.__staleFinalizerTarget={};"
        "__staleFinalizerTarget.self=__staleFinalizerTarget;"
        "__staleFinalizer.register(__staleFinalizerTarget,'held');";
    CHECK(script_runtime_evaluate_diagnostic(
        job_runtime, install_stale_finalizer,
        "<install-stale-finalizer>", &job_result));
    CHECK(!script_runtime_evaluate_diagnostic(
              job_runtime, "for(;;){}", "<intentional-timeout>",
              &job_result)
          && job_result.interrupted);
    CHECK(script_runtime_evaluate_diagnostic(
              job_runtime, "globalThis.__staleFinalizerTarget=null",
              "<release-stale-finalizer>", &job_result)
          && job_result.interrupted);
    size_t stale_failures_before = budget.failure_count;
    size_t stale_errors_before = job_result.uncaught_callback_errors;
    (void) script_runtime_collect_and_trim(job_runtime);
    CHECK(script_runtime_advance(job_runtime, 0, 8, &job_result)
          && job_result.success && !job_result.interrupted
          && strcmp(job_result.summary,
                    "STALE-FINALIZER-CONTINUED") == 0
          && job_result.uncaught_callback_errors == stale_errors_before + 1
          && strstr(job_result.last_uncaught_callback_error,
                    "ordinary author finalizer") != NULL
          && budget.failure_count == stale_failures_before);
    script_runtime_destroy(job_runtime);

    document_destroy(&document);
    CHECK(budget.current == 0
          && budget_active_allocations(&budget, NULL) == 0);

    /* Watchdog cadence, including the parser interrupt polls, must remain
       page-invisible: identical seeds and URLs produce identical replay
       entropy and clock observations regardless of the watchdog budget. */
    puts("test: parser interrupt polls stay replay-invisible");
    script_runtime_configure_deterministic_replay(true, 77);
    static const char replay_html[] =
        "<!doctype html><html><body></body></html>";
    static const char replay_probe[] =
        "globalThis.pocSummary=[Math.random(),Math.random(),Date.now(),"
        "performance.now()].join(',')";
    char replay_first[512] = {0};
    char replay_second[512] = {0};
    for (int round = 0; round < 2; round++) {
        PocDocument replay_document;
        CHECK(document_parse(&replay_document, &budget, replay_html,
                             sizeof(replay_html) - 1, 17));
        ScriptRuntimeOptions replay_options = {
            .viewport = viewport,
            .execution_policy = lab,
            .defer_document_scripts = true
        };
        ScriptResult replay_result;
        ScriptRuntime *replay_runtime = script_runtime_create_configured(
            &replay_document, &budget, 4u * MIB,
            round == 0 ? 1000 : 4000,
            "https://replay.test/", &replay_options, &replay_result);
        CHECK(replay_runtime != NULL
              && script_runtime_evaluate_diagnostic(
                     replay_runtime, replay_probe, "<replay-probe>",
                     &replay_result)
              && replay_result.success);
        snprintf(round == 0 ? replay_first : replay_second,
                 sizeof(replay_first), "%s", replay_result.summary);
        script_runtime_destroy(replay_runtime);
        document_destroy(&replay_document);
        CHECK(budget.current == 0);
    }
    script_runtime_configure_deterministic_replay(false, 0);
    CHECK(replay_first[0] != '\0'
          && strcmp(replay_first, replay_second) == 0);

    Budget navigation_budget;
    budget_init(&navigation_budget, 2u * MIB);
    NavigationSession navigation;
    CHECK(navigation_init(&navigation, &navigation_budget, 2)
          && navigation_set_script_execution_policy(&navigation, &realistic)
          && navigation.script_execution_policy
                 .maximum_host_compile_source_bytes
               == realistic.maximum_host_compile_source_bytes);
    navigation_destroy(&navigation);
    CHECK(navigation_budget.current == 0
          && budget_active_allocations(&navigation_budget, NULL) == 0);

    puts("tilefinch-js-responsiveness-tests: all checks passed");
    return 0;
}
