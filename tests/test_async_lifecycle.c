#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/multiplayer.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"

#include <stdio.h>
#include <string.h>

/* Synthetic admission only: real native bridge records and event delivery. */
static JSValue reserve_test_socket(JSContext *context, JSValueConst receiver,
                                  int argc, JSValueConst *argv)
{
    (void) receiver;
    DomBridge *bridge = JS_GetContextOpaque(context);
    int64_t id = 0;
    if (argc < 1 || JS_ToInt64(context, &id, argv[0]) < 0) return JS_EXCEPTION;
    if (bridge->websocket_count >= SCRIPT_WEBSOCKET_LIMIT) return JS_NewInt32(context, 0);
    if (!bridge->websocket_event_scratch)
        bridge->websocket_event_scratch = budget_malloc(
            bridge->budget, FETCH_WEBSOCKET_MESSAGE_LIMIT);
    if (!bridge->websocket_event_scratch) return JS_ThrowOutOfMemory(context);
    for (size_t slot = 0; slot < SCRIPT_WEBSOCKET_LIMIT; slot++) {
        if (bridge->websockets[slot].active) continue;
        bridge->websockets[slot] = (ScriptWebSocket) {.active = true, .id = (uint64_t) id};
        bridge->websocket_count++;
        return JS_NewInt64(context, id);
    }
    return JS_NewInt32(context, 0);
}

static unsigned native_event_calls;
static JSValue observe_native_event(JSContext *context, JSValueConst receiver,
                                   int argc, JSValueConst *argv)
{
    (void) context; (void) receiver; (void) argc; (void) argv;
    native_event_calls++;
    return JS_UNDEFINED;
}

typedef struct { uint64_t id; bool open; } NativeMultiplayerFixture;
static uint64_t native_multiplayer_open(void *opaque, Budget *budget,
                                       const TilefinchMultiplayerRequest *request)
{
    NativeMultiplayerFixture *fixture = opaque;
    if (budget == NULL || request == NULL || fixture->open) return 0;
    fixture->open = true;
    return ++fixture->id;
}
static bool native_multiplayer_cancel(void *opaque, uint64_t id)
{
    NativeMultiplayerFixture *fixture = opaque;
    if (!fixture->open || fixture->id != id) return false;
    fixture->open = false;
    return true;
}
static bool native_multiplayer_take(void *opaque, uint64_t id, unsigned char *bytes,
                                   size_t capacity, TilefinchMultiplayerEvent *event)
{
    (void) bytes; (void) capacity;
    if (!native_multiplayer_cancel(opaque, id)) return false;
    *event = (TilefinchMultiplayerEvent) {
        .kind = TILEFINCH_MULTIPLAYER_EVENT_CLOSE, .close_code = 1000, .clean = true
    };
    return true;
}

typedef enum {
    NETWORK_NONE,
    NETWORK_PRIMITIVES,
    NETWORK_NATIVE_WEBSOCKET,
    NETWORK_REFUSED_STRING,
    NETWORK_REFUSED_BUFFER,
    NETWORK_NATIVE_MULTIPLAYER
} NetworkCase;

/* Real scheduler/observer integration, not a second scheduling model. */
static int run_case_impl(const char *name, const char *source, NetworkCase network_seam)
{
    static const char page[] = "<!doctype html><title>Async lifecycle</title>"
        "<body><div id=target style='width:100px;height:20px'>Target</div>";
    Budget budget;
    budget_init(&budget, 16u * 1024u * 1024u);
    if (!budget_install_lexbor(&budget)) return 1;
    PocDocument document;
    if (!document_parse(&document, &budget, page, sizeof(page) - 1u, 17)) return 1;
    ScriptResult result = {0};
    ScriptRuntime *runtime;
    NativeMultiplayerFixture multiplayer = {0};
    const TilefinchMultiplayerBackend multiplayer_backend = {
        .open = native_multiplayer_open, .cancel = native_multiplayer_cancel,
        .take_event = native_multiplayer_take
    };
    if (network_seam == NETWORK_NATIVE_MULTIPLAYER
        && !tilefinch_multiplayer_install_backend(&multiplayer_backend, &multiplayer))
        return 1;
    if (network_seam) {
        ScriptRuntimeOptions options = {
            .defer_document_scripts = true,
            .allow_test_network_primitive_overrides = true
        };
        if (!viewport_context_init(&options.viewport, 480, 272, 480, 272)) return 1;
        runtime = script_runtime_create_configured(&document, &budget,
            8u * 1024u * 1024u, 3000,
            network_seam == NETWORK_NATIVE_MULTIPLAYER ? "https://tilefinch.local/offline/app?id=1"
                              : "https://lifecycle.test/",
            &options, &result);
    } else runtime = script_runtime_create_deferred(&document, &budget,
        8u * 1024u * 1024u, 3000, "https://lifecycle.test/", NULL, &result);
    bool ran = runtime != NULL;
    if (ran && network_seam) ran = script_runtime_evaluate_diagnostic(runtime,
        "for(const key of ['__tilefinchEventSourceStart','__tilefinchEventSourceClose'])"
        "if(!Object.getOwnPropertyDescriptor(globalThis,key).writable)"
        "throw Error('network test seam was not enabled')", "<async-network-seam>", &result);
    if (ran && network_seam == NETWORK_NATIVE_WEBSOCKET) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        ran = JS_SetPropertyStr(runtime->context, global, "__testSocketReserve",
            JS_NewCFunction(runtime->context, reserve_test_socket, "__testSocketReserve", 1)) >= 0;
        JS_FreeValue(runtime->context, global);
    }
    if (ran && network_seam == NETWORK_NATIVE_MULTIPLAYER)
        script_runtime_notify_user_activation(runtime);
    if (ran) ran = script_runtime_evaluate_diagnostic(
        runtime, source, "<async-lifecycle>", &result);
    if (ran && (network_seam == NETWORK_REFUSED_STRING || network_seam == NETWORK_REFUSED_BUFFER)) {
        JSValue global = JS_GetGlobalObject(runtime->context);
        ran = JS_SetPropertyStr(runtime->context, global, "__testNativeEvent",
            JS_NewCFunction(runtime->context, observe_native_event, "__testNativeEvent", 1)) >= 0;
        JS_FreeValue(runtime->context, global);
        native_event_calls = 0;
        char event_text[2048];
        memset(event_text, 'x', sizeof(event_text));
        JS_SetMemoryLimit(runtime->runtime, 0);
        JSValue refused = network_seam == NETWORK_REFUSED_STRING
            ? JS_NewStringLen(runtime->context, event_text, sizeof(event_text))
            : JS_NewArrayBufferCopy(runtime->context,
                                   (const uint8_t *) event_text, sizeof(event_text));
        JS_SetMemoryLimit(runtime->runtime, 8u * 1024u * 1024u);
        bool exception_created = JS_IsException(refused);
        bool delivered = js_rt_network_call_global(
            runtime, "__testNativeEvent", 1, &refused);
        JS_FreeValue(runtime->context, refused);
        JSValue pending = JS_GetException(runtime->context);
        if (!exception_created || delivered || native_event_calls != 0 || !JS_IsUninitialized(pending))
            fprintf(stderr, "native event refusal: exception=%d delivered=%d callbacks=%u pending-empty=%d\n",
                exception_created, delivered, native_event_calls, JS_IsUninitialized(pending));
        ran = ran && exception_created && !delivered && native_event_calls == 0
            && JS_IsUninitialized(pending);
        JS_FreeValue(runtime->context, pending);
        if (ran) {
            JSValue valid = JS_NewInt32(runtime->context, 7);
            ran = js_rt_network_call_global(runtime, "__testNativeEvent", 1, &valid)
                && native_event_calls == 1;
            JS_FreeValue(runtime->context, valid);
        }
    }
    if (ran && network_seam == NETWORK_NATIVE_MULTIPLAYER) {
        for (size_t round = 0; ran && round < 3u; round++)
            ran = js_rt_multiplayer_deliver(runtime, 1, NULL)
                && runtime->bridge.multiplayer.active
                && runtime->bridge.multiplayer.id == 2u + round
                && runtime->bridge.multiplayer_event_scratch != NULL
                && multiplayer.open && multiplayer.id == 2u + round;
    }
    if (ran && network_seam == NETWORK_NATIVE_WEBSOCKET) {
        const FetchWebSocketEvent closed = {
            .kind = FETCH_WEBSOCKET_EVENT_CLOSE, .close_code = 1000, .was_clean = true
        };
        for (size_t round = 0; ran && round < 3u; round++) {
            ran = js_rt_websocket_deliver_event(runtime, 0, &closed)
                && runtime->bridge.websocket_count == 2u
                && runtime->bridge.websockets[0].active
                && runtime->bridge.websockets[0].id == 3u + round
                && runtime->bridge.websockets[1].active
                && runtime->bridge.websockets[1].id == 2u
                && runtime->bridge.websocket_event_scratch != NULL;
        }
    }
    for (unsigned turn = 0; ran && turn < 64u; turn++)
        ran = script_runtime_advance(runtime, 20u, 32u, &result);
    bool ok = ran && result.success && !strcmp(result.summary, "OK");
    if (!ok) fprintf(stderr, "%s: ran=%d summary=%s error=%s\n",
                     name, ran, result.summary, result.error);
    script_runtime_destroy(runtime);
    if (network_seam == NETWORK_NATIVE_MULTIPLAYER) {
        ok = !multiplayer.open && ok;
        /* Keep a failing ownership assertion from poisoning later cases. */
        if (multiplayer.open) (void) tilefinch_multiplayer_cancel(multiplayer.id);
        ok = tilefinch_multiplayer_install_backend(NULL, NULL) && ok;
    }
    document_destroy(&document);
    bool clean = budget.current == 0;
    if (!clean) fprintf(stderr, "%s: owned=%zu\n", name, budget.current);
    bool uninstalled = clean && budget_uninstall_lexbor(&budget);
    printf("async lifecycle %s: %s\n", name, ok && uninstalled ? "pass" : "fail");
    return !(ok && uninstalled);
}

static int run_case(const char *name, const char *source)
{
    return run_case_impl(name, source, NETWORK_NONE);
}

static const char port_capacity[] =
    "pocSummary='PORT-PENDING';const ids=[];for(let i=0;i<256;i++){"
    "const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "const channel=new MessageChannel;channel.port1.onmessage=e=>{"
    "pocSummary=e.data==='retained'?'OK':'PORT-BAD';channel.port1.close();"
    "channel.port2.close()};channel.port2.postMessage('retained');"
    "for(const id of ids)clearTimeout(id);";

static const char performance_capacity[] =
    "pocSummary='PERFORMANCE-PENDING';const observer=new PerformanceObserver(list=>{"
    "const entries=list.getEntries();pocSummary=entries.length===1&&entries[0]."
    "name==='retained'?'OK':'PERFORMANCE-BAD';observer.disconnect()});"
    "observer.observe({entryTypes:['mark']});const ids=[];for(let i=0;i<256;i++){"
    "const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "performance.mark('retained');for(const id of ids)clearTimeout(id);";

static const char resize_capacity[] =
    "pocSummary='RESIZE-PENDING';const ids=[];for(let i=0;i<256;i++){"
    "const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "const target=document.getElementById('target'),observer=new ResizeObserver(entries=>{"
    "pocSummary=entries.length===1&&entries[0].target===target?'OK':'RESIZE-BAD';"
    "observer.disconnect()});observer.observe(target);for(const id of ids)clearTimeout(id);";

static const char worker_performance_capacity[] =
    "pocSummary='WORKER-PENDING';const source=\"const observer=new PerformanceObserver(list=>{"
    "const entries=list.getEntries();postMessage(entries.length===1&&entries[0].name==='retained');"
    "observer.disconnect()});observer.observe({entryTypes:['mark']});const ids=[];"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "performance.mark('retained');for(const id of ids)clearTimeout(id);\","
    "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "worker.onmessage=e=>{pocSummary=e.data===true?'OK':'WORKER-BAD';worker.terminate();"
    "URL.revokeObjectURL(url)};worker.onerror=e=>{pocSummary='WORKER-ERROR';e.preventDefault()};";

static const char intersection_capacity[] =
    "pocSummary='INTERSECTION-PENDING';const target=document.getElementById('target');"
    "let resized=false,intersected=false;const resize=new ResizeObserver(()=>{resized=true;"
    "resize.disconnect();if(intersected)pocSummary='OK'});resize.observe(target);"
    "const ids=[];for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;"
    "ids.push(id)}const observer=new IntersectionObserver(entries=>{intersected=entries.length"
    "===1&&entries[0].target===target;observer.disconnect();if(resized&&intersected)pocSummary='OK'});"
    "observer.observe(target);for(const id of ids)clearTimeout(id);";

static const char intersection_continuation[] =
    "pocSummary='INTERSECTION-CONTINUATION-PENDING';const targets=[],seen=new Set();"
    "for(let i=0;i<80;i++){const target=document.createElement('div');"
    "document.body.appendChild(target);targets.push(target)}"
    "const ids=[];for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;"
    "ids.push(id)}const observer=new IntersectionObserver(entries=>{for(const entry of entries){"
    "if(seen.has(entry.target)){pocSummary='INTERSECTION-DUPLICATE';return}seen.add(entry.target)}"
    "if(seen.size===targets.length){observer.disconnect();pocSummary='OK'}});"
    "for(const target of targets)observer.observe(target);"
    "queueMicrotask(()=>{for(const id of ids)clearTimeout(id)});";

static const char observer_disconnect[] =
    "pocSummary='DISCONNECT-PENDING';const observer=new PerformanceObserver(list=>{"
    "const entries=list.getEntries();pocSummary=entries.length===1&&entries[0].name==='fresh'"
    "?'OK':'DISCONNECT-BAD';observer.disconnect()});observer.observe({entryTypes:['mark']});"
    "const ids=[];for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "performance.mark('discarded');observer.disconnect();for(const id of ids)clearTimeout(id);"
    "observer.observe({entryTypes:['mark']});performance.mark('fresh');";

static const char worker_observer_termination[] =
    "pocSummary='TERMINATION-PENDING';const source=\"postMessage('armed');"
    "const observer=new PerformanceObserver(()=>postMessage('late'));"
    "observer.observe({entryTypes:['mark']});for(let i=0;i<256;i++)if(!setTimeout(()=>{},600000))break;"
    "performance.mark('discarded');\",url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "worker.onmessage=e=>{if(e.data!=='armed'){pocSummary='TERMINATION-LATE';return}"
    "worker.terminate();URL.revokeObjectURL(url);setTimeout(()=>{if(pocSummary==='TERMINATION-PENDING')"
    "pocSummary='OK'},0)};";

static const char worker_observer_intrinsics[] =
    "pocSummary='INTRINSICS-PENDING';const source=\"const observer=new PerformanceObserver(list=>{"
    "postMessage(list.getEntries()[0].name==='retained');observer.disconnect()});"
    "observer.observe({entryTypes:['mark']});self.setTimeout=()=>{throw Error('author timer')};"
    "performance.mark('retained');\",url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "worker.onmessage=e=>{pocSummary=e.data===true?'OK':'INTRINSICS-BAD';worker.terminate();"
    "URL.revokeObjectURL(url)};worker.onerror=e=>{pocSummary='INTRINSICS-ERROR';e.preventDefault()};";

static const char observer_intrinsics[] =
    "pocSummary='OBSERVER-INTRINSICS-PENDING';const original=globalThis.queueMicrotask;"
    "let calls=0;globalThis.queueMicrotask=callback=>{calls++;return original(callback)};"
    "const target=document.getElementById('target'),observer=new IntersectionObserver(entries=>{"
    "pocSummary=calls===0&&entries.length===1&&entries[0].target===target?'OK':'OBSERVER-INTRINSICS-BAD';"
    "observer.disconnect()});observer.observe(target);globalThis.queueMicrotask=original;";

static const char observer_task_cancellation[] =
    "pocSummary='OBSERVER-CANCELLED';const last=setTimeout(()=>{},600000);clearTimeout(last);"
    "const observer=new PerformanceObserver(()=>{pocSummary='OK';observer.disconnect()});"
    "observer.observe({entryTypes:['mark']});performance.mark('retained');clearTimeout(last+1);";

static const char port_task_cancellation[] =
    "pocSummary='PORT-CANCELLED';const last=setTimeout(()=>{},600000);clearTimeout(last);"
    "const channel=new MessageChannel;channel.port1.onmessage=()=>{pocSummary='OK';"
    "channel.port1.close();channel.port2.close()};channel.port2.postMessage('retained');"
    "clearInterval(last+1);cancelAnimationFrame(last+1);";

static const char cancellation_kinds[] =
    "pocSummary='WRONG-CANCEL';let timeout=false,frame=false;"
    "const timer=setTimeout(()=>{timeout=true;if(frame)pocSummary='OK'},0);"
    "cancelAnimationFrame(timer);const raf=requestAnimationFrame(()=>{frame=true;"
    "if(timeout)pocSummary='OK'});clearTimeout(raf);clearInterval(raf);";

static const char cancellation_controls[] =
    "pocSummary='OK';const timer=setTimeout(()=>pocSummary='CANCELLED-TIMEOUT-RAN',0);"
    "clearInterval(timer);const raf=requestAnimationFrame(()=>pocSummary='CANCELLED-FRAME-RAN');"
    "cancelAnimationFrame(raf);let calls=0;const interval=setInterval(()=>{"
    "if(++calls>1)pocSummary='CANCELLED-INTERVAL-RAN';clearTimeout(interval)},0);";

static const char worker_timer_isolation[] =
    "pocSummary='WORKER-TIMER-CANCELLED';const source=\"const id=setTimeout(()=>postMessage('fired'),40);"
    "postMessage({id});\",url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "worker.onmessage=e=>{if(e.data==='fired'){pocSummary='OK';worker.terminate();URL.revokeObjectURL(url)}"
    "else clearTimeout(e.data.id)};";

static const char worker_observer_cancellation[] =
    "pocSummary='WORKER-OBSERVER-CANCELLED';const source=\"const last=setTimeout(()=>{},600000);"
    "clearTimeout(last);const observer=new PerformanceObserver(()=>{observer.disconnect();postMessage(true)});"
    "observer.observe({entryTypes:['mark']});performance.mark('retained');clearTimeout(last+1);\","
    "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);worker.onmessage=e=>{"
    "pocSummary=e.data===true?'OK':'WORKER-OBSERVER-BAD';worker.terminate();URL.revokeObjectURL(url)};";

static const char file_reader_cancellation[] =
    "pocSummary='FILE-READER-CANCELLED';const last=setTimeout(()=>{},600000);clearTimeout(last);"
    "const reader=new FileReader;reader.onload=()=>{pocSummary=reader.result==='retained'?'OK':'FILE-BAD'};"
    "reader.readAsText(new Blob(['retained']));clearTimeout(last+1);";

static const char abort_timeout_cancellation[] =
    "pocSummary='ABORT-TIMEOUT-CANCELLED';const last=setTimeout(()=>{},600000);clearTimeout(last);"
    "const signal=AbortSignal.timeout(1);signal.onabort=()=>{pocSummary='OK'};clearTimeout(last+1);";

static const char long_abort_timeout[] =
    "pocSummary='OK';const signal=AbortSignal.timeout(0xffffffff);"
    "signal.onabort=()=>{pocSummary='LONG-TIMEOUT-FIRED'};";

static const char timer_reentrant_admission[] =
    "pocSummary='TIMER-OVER-ADMITTED';const ids=[];const outer=setTimeout(()=>{},"
    "{valueOf(){for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);"
    "if(!id)break;ids.push(id)}return 600000}});"
    "if(!outer&&ids.length===128)pocSummary='OK';"
    "clearTimeout(outer);for(const id of ids)clearTimeout(id);";

static const char worker_inbound_admission[] =
    "pocSummary='INBOUND-PENDING';const url=URL.createObjectURL(new Blob(["
    "'onmessage=e=>postMessage(e.data.byteLength)'])),worker=new Worker(url),"
    "buffer=new ArrayBuffer(16),ids=[];worker.onmessage=e=>{"
    "pocSummary=e.data===16&&buffer.byteLength===0?'OK':'INBOUND-BAD';"
    "worker.terminate();URL.revokeObjectURL(url)};"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "let refused=false;try{worker.postMessage(buffer,[buffer])}catch(error){"
    "refused=error.name==='QuotaExceededError'}const preserved=buffer.byteLength===16;"
    "for(const id of ids)clearTimeout(id);if(refused&&preserved)worker.postMessage(buffer,[buffer]);"
    "else{pocSummary='LOST-INBOUND-TRANSFER';worker.terminate();URL.revokeObjectURL(url)};";

static const char worker_outbound_admission[] =
    "pocSummary='OUTBOUND-PENDING';const source=\"const buffer=new ArrayBuffer(16),ids=[];"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "let refused=false;try{postMessage(buffer,[buffer])}catch(error){"
    "refused=error.name==='QuotaExceededError'}const preserved=buffer.byteLength===16;"
    "for(const id of ids)clearTimeout(id);if(refused&&preserved)postMessage(buffer,[buffer]);"
    "else postMessage('LOST-OUTBOUND-TRANSFER');close();\","
    "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "worker.onmessage=e=>{pocSummary=e.data.byteLength===16?'OK':'OUTBOUND-BAD';"
    "worker.terminate();URL.revokeObjectURL(url)};";

static const char worker_reentrant_admission[] =
    "pocSummary='REENTRANT-PENDING';const url=URL.createObjectURL(new Blob(["
    "'onmessage=e=>postMessage(e.data.buffer.byteLength)'])),worker=new Worker(url),"
    "buffer=new ArrayBuffer(16),ids=[];worker.onmessage=e=>{"
    "pocSummary=e.data===16&&buffer.byteLength===0?'OK':'REENTRANT-BAD';"
    "worker.terminate();URL.revokeObjectURL(url)};worker.postMessage({get buffer(){"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "return buffer}},[buffer]);for(const id of ids)clearTimeout(id);";

static const char worker_clone_rollback[] =
    "pocSummary='ROLLBACK-PENDING';const source=\"let good=true;"
    "for(let i=0;i<140;i++){const buffer=new ArrayBuffer(8);let failed=false;"
    "try{postMessage({buffer,bad:()=>{}},[buffer])}catch(e){failed=e.name==='DataCloneError'}"
    "good=good&&failed&&buffer.byteLength===8}postMessage(good);close();\","
    "url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);let good=true;"
    "for(let i=0;i<140;i++){const buffer=new ArrayBuffer(8);let failed=false;"
    "try{worker.postMessage({buffer,bad:()=>{}},[buffer])}catch(e){failed=e.name==='DataCloneError'}"
    "good=good&&failed&&buffer.byteLength===8}worker.onmessage=e=>{"
    "pocSummary=good&&e.data===true?'OK':'ROLLBACK-BAD';worker.terminate();URL.revokeObjectURL(url)};";

static const char worker_outbound_close[] =
    "pocSummary='CLOSE-PENDING';const source=\"postMessage({get value(){close();return 1}});"
    "postMessage({value:2});\",url=URL.createObjectURL(new Blob([source])),worker=new Worker(url);"
    "let received=0;worker.onmessage=e=>{if(e.data.value!==++received)pocSummary='CLOSE-ORDER';"
    "else if(received===2){pocSummary='OK';worker.terminate();URL.revokeObjectURL(url)}};";

static const char file_reader_admission[] =
    "pocSummary='READER-STRANDED';const ids=[],reader=new FileReader;"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "let refused=false;try{reader.readAsText(new Blob(['old']))}catch(e){"
    "refused=e.name==='QuotaExceededError'}const unchanged=reader.readyState===0&&"
    "reader.result===null&&reader.error===null;for(const id of ids)clearTimeout(id);"
    "if(refused&&unchanged){reader.onload=()=>{pocSummary=reader.result==='retry'&&"
    "reader.error===null?'OK':'READER-RETRY-BAD'};reader.readAsText(new Blob(['retry']))}";

static const char file_reader_async_start[] =
    "pocSummary='READER-START-PENDING';const reader=new FileReader;let returned=false,bad=false;"
    "reader.onloadstart=()=>{if(!returned||reader.readyState!==1||reader.result!==null)bad=true};"
    "reader.onloadend=()=>{pocSummary=!bad&&returned&&reader.readyState===2&&"
    "reader.result==='value'?'OK':'READER-SYNCHRONOUS-START'};"
    "reader.readAsText(new Blob(['value']));returned=true;";

static const char file_reader_load_chain[] =
    "pocSummary='READER-CHAIN-PENDING';const reader=new FileReader,loads=[];let ends=0,bad=false;"
    "reader.onload=()=>{loads.push(reader.result);if(reader.result==='first')"
    "reader.readAsText(new Blob(['second']))};reader.onloadend=()=>{ends++;"
    "if(reader.readyState===1||reader.result!=='second')bad=true;"
    "pocSummary=!bad&&ends===1&&loads.join(',')==='first,second'?'OK':'READER-OLD-LOADEND'};"
    "reader.readAsText(new Blob(['first']));";

static const char file_reader_abort_chain[] =
    "pocSummary='READER-ABORT-PENDING';const reader=new FileReader;let ends=0,bad=false;"
    "reader.onabort=()=>reader.readAsText(new Blob(['replacement']));reader.onloadend=()=>{"
    "ends++;if(reader.readyState===1||reader.result!=='replacement')bad=true;"
    "pocSummary=!bad&&ends===1?'OK':'READER-ABORT-LOADEND'};"
    "reader.readAsText(new Blob(['discarded']));reader.abort();";

static const char file_reader_progress_abort[] =
    "pocSummary='READER-PROGRESS-PENDING';const reader=new FileReader;let aborts=0,loads=0;"
    "reader.onprogress=()=>reader.abort();reader.onabort=()=>aborts++;reader.onload=()=>loads++;"
    "reader.onloadend=()=>{pocSummary=aborts===1&&loads===0&&reader.readyState===2&&"
    "reader.result===null&&reader.error===null?'OK':'READER-IGNORED-ABORT'};"
    "reader.readAsText(new Blob(['discarded']));";

static const char file_reader_handler_exception[] =
    "pocSummary='READER-EXCEPTION-PENDING';addEventListener('error',event=>event.preventDefault());"
    "const reader=new FileReader;let errors=0;reader.onerror=()=>errors++;"
    "reader.onload=()=>{throw Error('handler-only')};reader.onloadend=()=>{"
    "pocSummary=errors===0&&reader.result==='intact'&&reader.error===null&&reader.readyState===2"
    "?'OK':'READER-HANDLER-BECAME-READ-ERROR'};reader.readAsText(new Blob(['intact']));";

static const char file_reader_private_lifecycle[] =
    "pocSummary='READER-STATE-PENDING';const reader=new FileReader;let blocked=false;"
    "reader.readAsText(new Blob(['intact']));Object.defineProperty(reader,'readyState',"
    "{value:0,writable:true});reader._task=0;try{reader.readAsText(new Blob(['replacement']))}"
    "catch(e){blocked=e.name==='InvalidStateError'}reader.onload=()=>{"
    "pocSummary=blocked&&reader.result==='intact'?'OK':'READER-AUTHOR-STATE'};";

static const char file_reader_start_microtask[] =
    "pocSummary='READER-START-JOB-PENDING';const reader=new FileReader;let loads=0,aborts=0;"
    "reader.onloadstart=()=>Promise.resolve().then(()=>reader.abort());reader.onload=()=>loads++;"
    "reader.onabort=()=>aborts++;reader.onloadend=()=>{pocSummary=loads===0&&aborts===1&&"
    "reader.result===null?'OK':'READER-START-JOB-LATE'};reader.readAsText(new Blob(['discarded']));";

static const char file_reader_progress_microtask[] =
    "pocSummary='READER-PROGRESS-JOB-PENDING';const reader=new FileReader;let loads=0,aborts=0;"
    "reader.onprogress=()=>Promise.resolve().then(()=>reader.abort());reader.onload=()=>loads++;"
    "reader.onabort=()=>aborts++;reader.onloadend=()=>{pocSummary=loads===0&&aborts===1&&"
    "reader.result===null?'OK':'READER-PROGRESS-JOB-LATE'};reader.readAsText(new Blob(['discarded']));";

static const char file_reader_stage_pressure[] =
    "pocSummary='READER-STAGE-PENDING';const reader=new FileReader,ids=[];"
    "reader.onloadstart=()=>{for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);"
    "if(!id)break;ids.push(id)}queueMicrotask(()=>{for(const id of ids)clearTimeout(id)})};"
    "reader.onloadend=()=>{pocSummary=reader.result==='intact'&&reader.error===null?'OK':"
    "'READER-STAGE-LOST'};reader.readAsText(new Blob(['intact']));";

static const char file_reader_reserved_pressure[] =
    "pocSummary='READER-RESERVED-PENDING';const reader=new FileReader,ids=[];let errors=0,ends=0;"
    "reader.onerror=()=>{errors++;if(reader.error.name!=='QuotaExceededError'){"
    "pocSummary='READER-WRONG-REFUSAL';return}for(const id of ids)__tilefinchCancelTimer(id);"
    "reader.readAsText(new Blob(['replacement']))};reader.onloadend=()=>{ends++;"
    "pocSummary=errors===1&&ends===1&&reader.result==='replacement'&&reader.error===null"
    "?'OK':'READER-RESERVED-CHAIN'};reader.readAsText(new Blob(['discarded']));"
    "for(let i=0;i<256;i++){const id=__tilefinchScheduleReservedTask(()=>{},600000);"
    "if(!id)break;ids.push(id)}";

static const char file_reader_worker_termination[] =
    "pocSummary='READER-WORKER-PENDING';const source=\"const reader=new FileReader;"
    "reader.onloadstart=()=>postMessage('started');reader.onload=()=>postMessage('late');"
    "reader.readAsText(new Blob(['discarded']));\",url=URL.createObjectURL(new Blob([source])),"
    "worker=new Worker(url);worker.onmessage=e=>{if(e.data!=='started'){"
    "pocSummary='READER-WORKER-LATE';return}worker.terminate();URL.revokeObjectURL(url);"
    "setTimeout(()=>{if(pocSummary==='READER-WORKER-PENDING')pocSummary='OK'},0)};";

static const char event_source_reconnect_parser[] =
    "pocSummary='SOURCE-PARSER-PENDING';const deliver=__tilefinchDeliverEventSource,"
    "encoder=new TextEncoder,seen=[];let starts=0,header=false;"
    "__tilefinchEventSourceClose=()=>true;__tilefinchEventSourceStart=(url,credentials,last)=>{"
    "const id=++starts;if(id===2)header=last==='committed';queueMicrotask(()=>{"
    "deliver(id,'open','','https://lifecycle.test');if(id===1){deliver(id,'chunk',encoder.encode("
    "'retry:250\\nid:committed\\ndata:initial\\n\\nid:uncommitted\\nevent:stale\\n"
    "data:partial\\nx:').buffer);deliver(id,'chunk',new Uint8Array([0xe2]).buffer);"
    "deliver(id,'finish','')}else{const body=encoder.encode('id:kept\\ndata:fresh\\n\\n'),"
    "bytes=new Uint8Array(body.length+3);bytes.set([0xef,0xbb,0xbf]);bytes.set(body,3);"
    "deliver(id,'chunk',bytes.buffer)}});return id};const source=new EventSource('/feed');"
    "source.addEventListener('stale',()=>{source.close();pocSummary='SOURCE-STALE-PARSER'});"
    "source.onmessage=e=>{seen.push(e.data);if(e.data==='fresh'){source.close();pocSummary="
    "header&&seen.join(',')==='initial,fresh'&&e.lastEventId==='kept'?'OK':'SOURCE-PARSER-BAD'}};";

static const char event_source_handler_exception[] =
    "pocSummary='SOURCE-HANDLER-PENDING';addEventListener('error',event=>event.preventDefault());"
    "const deliver=__tilefinchDeliverEventSource;__tilefinchEventSourceClose=()=>true;"
    "__tilefinchEventSourceStart=()=>{queueMicrotask(()=>deliver(1,'open',''));"
    "queueMicrotask(()=>deliver(1,'chunk','data:one\\n\\n'));"
    "queueMicrotask(()=>deliver(1,'chunk','data:two\\n\\n'));return 1};"
    "const source=new EventSource('/feed');let seen=0;source.onmessage=e=>{if(++seen===1)"
    "throw Error('handler-only');source.close();pocSummary=e.data==='two'&&seen===2"
    "?'OK':'SOURCE-HANDLER-DUPLICATE'};";

static const char event_source_close_error[] =
    "pocSummary='SOURCE-CLOSE-PENDING';const deliver=__tilefinchDeliverEventSource;"
    "__tilefinchEventSourceClose=()=>true;__tilefinchEventSourceStart=()=>{queueMicrotask(()=>{"
    "deliver(1,'finish','');pocSummary=source.readyState===EventSource.CLOSED&&"
    "__tilefinchSchedulerSnapshot()[2]===0?'OK':'SOURCE-CLOSED-TIMER'});return 1};"
    "const source=new EventSource('/feed');source.onerror=()=>source.close();";

static const char event_source_reconnect_pressure[] =
    "pocSummary='SOURCE-PRESSURE-PENDING';const ids=[],deliver=__tilefinchDeliverEventSource;"
    "let starts=0;__tilefinchEventSourceClose=()=>true;__tilefinchEventSourceStart=()=>{"
    "const id=++starts;queueMicrotask(()=>{if(id===1){deliver(id,'chunk','retry:250\\n');"
    "deliver(id,'finish','')}else{source.close();pocSummary=starts===2?'OK':'SOURCE-EXTRA-RETRY'}});"
    "return id};const source=new EventSource('/feed');source.onerror=()=>queueMicrotask(()=>{"
    "for(const id of ids)clearTimeout(id)});for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);"
    "if(!id)break;ids.push(id)}";

static const char event_source_cr_delimiters[] =
    "pocSummary='SOURCE-CR-PENDING';const deliver=__tilefinchDeliverEventSource,seen=[];"
    "let immediate=false;__tilefinchEventSourceClose=()=>true;__tilefinchEventSourceStart=()=>{"
    "queueMicrotask(()=>{deliver(1,'open','');deliver(1,'chunk','data:first\\r');"
    "deliver(1,'chunk','\\r');immediate=seen.join(',')==='first';"
    "deliver(1,'chunk','\\ndata:second\\r\\n\\r\\n');source.close();"
    "pocSummary=immediate&&seen.join(',')==='first,second'?'OK':'SOURCE-CR-DELAYED'});return 1};"
    "const source=new EventSource('/feed');source.onmessage=e=>seen.push(e.data);";

static const char event_source_deferred_close[] =
    "pocSummary='SOURCE-DEFERRED-CLOSE-PENDING';const ids=[],deliver=__tilefinchDeliverEventSource;"
    "let starts=0;__tilefinchEventSourceClose=()=>true;__tilefinchEventSourceStart=()=>{"
    "starts++;queueMicrotask(()=>{deliver(1,'finish','');source.close();"
    "for(const id of ids)clearTimeout(id);pocSummary=source.readyState===EventSource.CLOSED&&"
    "__tilefinchSchedulerSnapshot()[2]===0&&starts===1?'OK':'SOURCE-DEFERRED-RESURRECTED'});"
    "return 1};const source=new EventSource('/feed');"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}";

static const char network_primitives_sealed[] =
    "pocSummary=['__tilefinchEventSourceStart','__tilefinchEventSourceClose'].every(key=>{"
    "const descriptor=Object.getOwnPropertyDescriptor(globalThis,key),original=globalThis[key];"
    "try{globalThis[key]=()=>0}catch(_){}return descriptor&&!descriptor.writable&&"
    "!descriptor.configurable&&globalThis[key]===original})?'OK':'SOURCE-PRIMITIVE-WRITABLE';";

static const char websocket_error_close[] =
    "pocSummary='SOCKET-CLOSE-PENDING';addEventListener('error',e=>e.preventDefault());"
    "__tilefinchWebSocketStart=()=>{queueMicrotask(()=>__tilefinchDeliverWebSocket("
    "1,'close',undefined,'failed',1006,false));return 1};"
    "const socket=new WebSocket('wss://lifecycle.test/feed');let errors=0;"
    "socket.onerror=()=>{errors++;throw Error('socket-handler-only')};"
    "socket.onclose=e=>{pocSummary=errors===1&&e.code===1006&&!e.wasClean&&"
    "socket.readyState===WebSocket.CLOSED?'OK':'SOCKET-CLOSE-BAD'};";

static const char websocket_closing_messages[] =
    "pocSummary='SOCKET-CLOSING-PENDING';let messages=0;"
    "__tilefinchWebSocketStart=()=>{queueMicrotask(()=>__tilefinchDeliverWebSocket("
    "1,'open',undefined,'',0,false));return 1};"
    "__tilefinchWebSocketClose=()=>{queueMicrotask(()=>{__tilefinchDeliverWebSocket("
    "1,'text',new TextEncoder().encode('late').buffer,'',0,false);"
    "__tilefinchDeliverWebSocket(1,'close',undefined,'',1000,true)});return true};"
    "const socket=new WebSocket('wss://lifecycle.test/feed');"
    "socket.onopen=()=>socket.close();socket.onmessage=()=>messages++;"
    "socket.onclose=()=>{pocSummary=messages===0?'OK':'SOCKET-LATE-MESSAGE'};";

static const char websocket_close_conversion[] =
    "pocSummary='SOCKET-CONVERSION-PENDING';let conversions=0,valid=false;"
    "__tilefinchWebSocketStart=()=>{queueMicrotask(()=>__tilefinchDeliverWebSocket("
    "1,'open',undefined,'',0,false));return 1};"
    "__tilefinchWebSocketClose=(id,code,reason)=>{valid=id===1&&code===1000&&reason==='once';"
    "queueMicrotask(()=>__tilefinchDeliverWebSocket(1,'close',undefined,'',1000,true));return true};"
    "const socket=new WebSocket('wss://lifecycle.test/feed');"
    "socket.onopen=()=>socket.close(1000,{toString(){conversions++;"
    "return conversions===1?'once':'x'.repeat(200)}});"
    "socket.onclose=()=>{pocSummary=valid&&conversions===1?'OK':'SOCKET-UNVALIDATED-REASON'};";

static const char websocket_failure_admission[] =
    "pocSummary='SOCKET-ADMISSION-PENDING';const ids=[];"
    "__tilefinchWebSocketStart=()=>0;for(let i=0;i<256;i++){"
    "const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "let refused=false;try{new WebSocket('wss://lifecycle.test/feed')}"
    "catch(e){refused=e.name==='QuotaExceededError'}"
    "for(const id of ids)clearTimeout(id);"
    "pocSummary=refused?'OK':'SOCKET-STRANDED-CONNECTING';";

static const char websocket_native_retirement[] =
    "pocSummary='SOCKET-NATIVE-PENDING';let next=0,closes=0;"
    "__tilefinchWebSocketStart=()=>__testSocketReserve(++next);"
    "let current=new WebSocket('wss://lifecycle.test/first');"
    "const other=new WebSocket('wss://lifecycle.test/other');"
    "const replace=()=>{closes++;current=new WebSocket('wss://lifecycle.test/next');"
    "current.onclose=replace;if(closes===3)pocSummary='OK'};current.onclose=replace;";

static const char websocket_failure_order[] =
    "pocSummary='SOCKET-FAILURE-ORDER-PENDING';__tilefinchWebSocketStart=()=>0;"
    "const socket=new WebSocket('wss://lifecycle.test/feed');let errors=0,"
    "async=socket.readyState===WebSocket.CONNECTING;"
    "socket.onerror=()=>{errors++;async&&=socket.readyState===WebSocket.CLOSED};"
    "socket.onclose=e=>{pocSummary=async&&errors===1&&e.code===1006&&!e.wasClean"
    "?'OK':'SOCKET-FAILURE-ORDER-BAD'};";

static const char websocket_full_timers_success[] =
    "pocSummary='SOCKET-FULL-SUCCESS-PENDING';const ids=[];"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "__tilefinchWebSocketStart=()=>{queueMicrotask(()=>__tilefinchDeliverWebSocket("
    "1,'open',undefined,'',0,false));return 1};"
    "__tilefinchWebSocketClose=()=>{queueMicrotask(()=>__tilefinchDeliverWebSocket("
    "1,'close',undefined,'',1000,true));return true};"
    "const socket=new WebSocket('wss://lifecycle.test/feed');socket.onopen=()=>{"
    "for(const id of ids)clearTimeout(id);socket.close()};"
    "socket.onclose=()=>{pocSummary='OK'};";

static const char multiplayer_native_retirement[] =
    "pocSummary='MULTIPLAYER-NATIVE-PENDING';let closes=0;"
    "let current=navigator.tilefinchMultiplayer.host({gameId:'lifecycle-test'});"
    "const replace=()=>{closes++;current=navigator.tilefinchMultiplayer.host("
    "{gameId:'lifecycle-test'});current.onclose=replace;"
    "if(closes===3)pocSummary='OK'};current.onclose=replace;";

static const char multiplayer_error_close[] =
    "pocSummary='MULTIPLAYER-CLOSE-PENDING';addEventListener('error',e=>e.preventDefault());"
    "__tilefinchMultiplayerStart=()=>0;let errors=0,closedAtError=false;"
    "const channel=navigator.tilefinchMultiplayer.host({gameId:'lifecycle-test'});"
    "channel.onerror=()=>{errors++;closedAtError=channel.readyState==='closed';"
    "throw Error('multiplayer-handler-only')};"
    "channel.onclose=e=>{pocSummary=errors===1&&closedAtError&&e.code===1006&&!e.wasClean&&"
    "channel.readyState==='closed'?'OK':'MULTIPLAYER-CLOSE-BAD'};";

static const char multiplayer_closing_messages[] =
    "pocSummary='MULTIPLAYER-CLOSING-PENDING';let messages=0;"
    "__tilefinchMultiplayerStart=()=>{queueMicrotask(()=>__tilefinchDeliverMultiplayer("
    "1,'open'));return 1};"
    "__tilefinchMultiplayerClose=()=>{queueMicrotask(()=>{__tilefinchDeliverMultiplayer("
    "1,'text',new TextEncoder().encode('late').buffer);"
    "__tilefinchDeliverMultiplayer(1,'close',undefined,'',1000,true)});return true};"
    "const channel=navigator.tilefinchMultiplayer.host({gameId:'lifecycle-test'});"
    "channel.onopen=()=>channel.close();channel.onmessage=()=>messages++;"
    "channel.onclose=()=>{pocSummary=messages===0?'OK':'MULTIPLAYER-LATE-MESSAGE'};";

static const char multiplayer_failure_admission[] =
    "pocSummary='MULTIPLAYER-ADMISSION-PENDING';const ids=[];"
    "__tilefinchMultiplayerStart=()=>0;for(let i=0;i<256;i++){"
    "const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "let refused=false;try{navigator.tilefinchMultiplayer.host({gameId:'lifecycle-test'})}"
    "catch(e){refused=e.name==='QuotaExceededError'}"
    "for(const id of ids)clearTimeout(id);"
    "pocSummary=refused?'OK':'MULTIPLAYER-STRANDED-CONNECTING';";

static const char multiplayer_full_timers_success[] =
    "pocSummary='MULTIPLAYER-FULL-SUCCESS-PENDING';const ids=[];"
    "for(let i=0;i<256;i++){const id=setTimeout(()=>{},600000);if(!id)break;ids.push(id)}"
    "__tilefinchMultiplayerStart=()=>{queueMicrotask(()=>__tilefinchDeliverMultiplayer("
    "1,'open'));return 1};"
    "__tilefinchMultiplayerClose=()=>{queueMicrotask(()=>__tilefinchDeliverMultiplayer("
    "1,'close',undefined,'',1000,true));return true};"
    "const channel=navigator.tilefinchMultiplayer.host({gameId:'lifecycle-test'});"
    "channel.onopen=()=>{for(const id of ids)clearTimeout(id);channel.close()};"
    "channel.onclose=()=>{pocSummary='OK'};";

int main(int argc, char **argv)
{
    typedef struct { const char *name, *source; } AsyncCase;
    static const AsyncCase cases[] = {
        {"ports", port_capacity}, {"performance", performance_capacity},
        {"resize", resize_capacity}, {"worker-performance", worker_performance_capacity},
        {"intersection", intersection_capacity},
        {"intersection-continuation", intersection_continuation},
        {"disconnect", observer_disconnect}, {"termination", worker_observer_termination},
        {"worker-intrinsics", worker_observer_intrinsics},
        {"observer-intrinsics", observer_intrinsics},
        {"observer-cancellation", observer_task_cancellation},
        {"port-cancellation", port_task_cancellation},
        {"cancellation-kinds", cancellation_kinds},
        {"cancellation-controls", cancellation_controls},
        {"worker-timer-isolation", worker_timer_isolation},
        {"worker-observer-cancellation", worker_observer_cancellation},
        {"file-reader-cancellation", file_reader_cancellation},
        {"abort-timeout-cancellation", abort_timeout_cancellation},
        {"long-abort-timeout", long_abort_timeout},
        {"timer-reentrant-admission", timer_reentrant_admission},
        {"worker-inbound-admission", worker_inbound_admission},
        {"worker-outbound-admission", worker_outbound_admission},
        {"worker-reentrant-admission", worker_reentrant_admission},
        {"worker-clone-rollback", worker_clone_rollback},
        {"worker-outbound-close", worker_outbound_close},
        {"file-reader-admission", file_reader_admission},
        {"file-reader-async-start", file_reader_async_start},
        {"file-reader-load-chain", file_reader_load_chain},
        {"file-reader-abort-chain", file_reader_abort_chain},
        {"file-reader-progress-abort", file_reader_progress_abort},
        {"file-reader-handler-exception", file_reader_handler_exception},
        {"file-reader-private-lifecycle", file_reader_private_lifecycle},
        {"file-reader-start-microtask", file_reader_start_microtask},
        {"file-reader-progress-microtask", file_reader_progress_microtask},
        {"file-reader-stage-pressure", file_reader_stage_pressure},
        {"file-reader-reserved-pressure", file_reader_reserved_pressure},
        {"file-reader-worker-termination", file_reader_worker_termination},
        {"network-primitives-sealed", network_primitives_sealed},
    };
    static const AsyncCase network_cases[] = {
        {"event-source-reconnect-parser", event_source_reconnect_parser},
        {"event-source-handler-exception", event_source_handler_exception},
        {"event-source-close-error", event_source_close_error},
        {"event-source-reconnect-pressure", event_source_reconnect_pressure},
        {"event-source-cr-delimiters", event_source_cr_delimiters},
        {"event-source-deferred-close", event_source_deferred_close},
        {"websocket-error-close", websocket_error_close},
        {"websocket-closing-messages", websocket_closing_messages},
        {"websocket-close-conversion", websocket_close_conversion},
        {"websocket-failure-admission", websocket_failure_admission},
        {"websocket-failure-order", websocket_failure_order},
        {"websocket-full-timers-success", websocket_full_timers_success},
        {"multiplayer-error-close", multiplayer_error_close},
        {"multiplayer-closing-messages", multiplayer_closing_messages},
        {"multiplayer-failure-admission", multiplayer_failure_admission},
        {"multiplayer-full-timers-success", multiplayer_full_timers_success},
    };
    int failures = 0;
    bool selected = false;
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "websocket-native-retirement"))) {
        selected = true;
        failures += run_case_impl("websocket-native-retirement", websocket_native_retirement, NETWORK_NATIVE_WEBSOCKET);
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "network-event-argument-refusal"))) {
        selected = true;
        failures += run_case_impl("network-event-argument-refusal", "pocSummary='OK'", NETWORK_REFUSED_STRING);
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "network-event-buffer-refusal"))) {
        selected = true;
        failures += run_case_impl("network-event-buffer-refusal", "pocSummary='OK'", NETWORK_REFUSED_BUFFER);
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "multiplayer-native-retirement"))) {
        selected = true;
        failures += run_case_impl("multiplayer-native-retirement", multiplayer_native_retirement, NETWORK_NATIVE_MULTIPLAYER);
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (argc == 1 || (argc == 2 && !strcmp(argv[1], cases[i].name)))
        {
            selected = true;
            failures += run_case(cases[i].name, cases[i].source);
        }
    for (size_t i = 0; i < sizeof(network_cases) / sizeof(network_cases[0]); i++)
        if (argc == 1 || (argc == 2 && !strcmp(argv[1], network_cases[i].name))) {
            selected = true;
            failures += run_case_impl(network_cases[i].name, network_cases[i].source, NETWORK_PRIMITIVES);
        }
    return !selected ? 2 : failures != 0;
}
