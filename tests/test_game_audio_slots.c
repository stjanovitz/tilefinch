/* Exercise both implementations through public WebAudio APIs in real realms.
   The host mixer seam is synchronous: this does not qualify a PSP worker. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        exit(1);                                                             \
    }                                                                        \
} while (0)

enum { BLOCKS = 9, BLOCK_FRAMES = 8, OSCILLATOR_BLOCKS = 8 };

typedef struct {
    Budget budget;
    PocDocument document;
    ScriptRuntime *runtime;
    ScriptResult result;
    bool native;
} AudioCase;

typedef struct {
    int16_t pcm[BLOCKS][BLOCK_FRAMES * 2u];
    unsigned count;
} AudioResult;

typedef struct {
    int16_t pcm[OSCILLATOR_BLOCKS][TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    unsigned count;
} OscillatorResult;

static lxb_dom_node_t *find_button(lxb_dom_node_t *node)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && length == 5u && memcmp(id, "start", 5u) == 0)
            return node;
        lxb_dom_node_t *nested = find_button(node->first_child);
        if (nested != NULL) return nested;
    }
    return NULL;
}

static void settle(AudioCase *test)
{
    for (unsigned at = 0;
         at < 32u && JS_IsJobPending(test->runtime->runtime); at++) {
        CHECK(script_runtime_advance(test->runtime, 0, 4, &test->result));
    }
    CHECK(!JS_IsJobPending(test->runtime->runtime));
}

static void expect_summary(AudioCase *test, const char *expected)
{
    settle(test);
    if (strcmp(test->result.summary, expected) != 0) {
        fprintf(stderr, "audio slots lane=%s expected=%s summary=%s error=%s callback=%s\n",
                test->native ? "native" : "reference", expected,
                test->result.summary, test->result.error,
                test->result.last_uncaught_callback_error);
        CHECK(false);
    }
}

static void evaluate(AudioCase *test, const char *source, const char *expected)
{
    bool okay = script_runtime_evaluate_diagnostic(
        test->runtime, source, "<game-audio-slots-test>", &test->result);
    if (!okay) {
        fprintf(stderr, "audio slots lane=%s: %s / %s\n",
                test->native ? "native" : "reference", test->result.error,
                test->result.last_uncaught_callback_error);
    }
    CHECK(okay);
    expect_summary(test, expected);
}

static void activate(AudioCase *test, const char *expected)
{
    lxb_dom_node_t *button = find_button(lxb_dom_interface_node(test->document.html));
    CHECK(button != NULL);
    CHECK(script_runtime_dispatch_activation_node(test->runtime, button, &test->result));
    expect_summary(test, expected);
}

static void check_backend(AudioCase *test, unsigned active_sources)
{
    unsigned counts[5] = {0};
    CHECK(test->runtime->game_audio_internal_slots == test->native);
    CHECK((test->runtime->game_audio_slots != NULL) == test->native);
    CHECK(js_rt_audio_slot_test_snapshot(test->runtime, counts) == test->native);
    if (test->native) {
        CHECK(counts[0] >= 1u && counts[1] >= 1u);
        CHECK(counts[4] == active_sources);
    }
}

static void begin_case(AudioCase *test, bool native)
{
    memset(test, 0, sizeof(*test));
    test->native = native;
    budget_init(&test->budget, 24u * MIB);
    CHECK(budget_install_lexbor(&test->budget));
    static const char html[] =
        "<!doctype html><html><body><button id='start'>Start</button></body></html>";
    CHECK(document_parse(&test->document, &test->budget, html, sizeof(html) - 1u, 17));
    ScriptRuntimeOptions options = {
        .defer_document_scripts = true,
        .game_audio_internal_slots = native
    };
    CHECK(viewport_context_init(&options.viewport, 480, 272, 480, 272));
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &options.execution_policy));
    options.execution_policy.slow_compile_threshold_us = UINT64_MAX;
    options.execution_policy.slow_callback_threshold_us = UINT64_MAX;
    test->runtime = script_runtime_create_configured(
        &test->document, &test->budget, 5u * MIB, 30000,
        "https://audio-slots.test/", &options, &test->result);
    CHECK(test->runtime != NULL && test->result.success);
    CHECK(test->runtime->game_audio_internal_slots == native);
}

static void end_case(AudioCase *test)
{
    script_runtime_destroy(test->runtime);
    document_destroy(&test->document);
    CHECK(test->budget.current == 0u);
    CHECK(test->budget.external_reserved == 0u);
    CHECK(budget_active_allocations(&test->budget, NULL) == 0u);
    CHECK(budget_uninstall_lexbor(&test->budget));
}

static void mix_exact(AudioCase *test, AudioResult *result, size_t frames,
                      unsigned numerator, unsigned denominator,
                      bool left, bool right)
{
    static const int16_t wave[] = {1000, -1000, 2000, -2000};
    CHECK(test->runtime->game_audio != NULL && frames <= BLOCK_FRAMES);
    CHECK(result->count < BLOCKS && denominator != 0u);
    int16_t *pcm = result->pcm[result->count++];
    memset(pcm, 0, BLOCK_FRAMES * 2u * sizeof(*pcm));
    size_t allocations = test->budget.allocation_count;
    CHECK(tilefinch_game_audio_mix(test->runtime->game_audio, pcm, frames));
    CHECK(test->budget.allocation_count == allocations);
    for (size_t frame = 0; frame < frames; frame++) {
        int16_t sample = (int16_t) ((int) wave[frame % 4u]
            * (int) numerator / (int) denominator);
        CHECK(pcm[frame * 2u] == (left ? sample : 0));
        CHECK(pcm[frame * 2u + 1u] == (right ? sample : 0));
    }
}

static void dispatch_completions(AudioCase *test)
{
    /* Only the ordinary runtime advance drains the mixer's completion mailbox. */
    CHECK(script_runtime_advance(test->runtime, 0, 4, &test->result));
    CHECK(script_runtime_advance(test->runtime, 0, 4, &test->result));
}

static void run_lane(bool native, AudioResult *result)
{
    AudioCase test;
    begin_case(&test, native);
    memset(result, 0, sizeof(*result));

    evaluate(&test,
        "globalThis.audioTest={context:new AudioContext(),started:false,"
        "oneEnded:0,oneHandler:0,loopEnded:0,loopHandler:0};"
        "(()=>{const t=audioTest;document.getElementById('start').addEventListener('click',"
        "()=>{t.context.resume().then(()=>{"
        "if(t.started){globalThis.pocSummary='RESUMED';return}"
        "const source=t.context.createBufferSource();source.buffer=t.buffer;"
        "source.connect(t.context.destination);"
        "source.addEventListener('ended',()=>t.oneEnded++);"
        "source.onended=()=>t.oneHandler++;source.start();"
        "t.source=source;t.started=true;globalThis.pocSummary='STARTED'"
        "},error=>globalThis.pocSummary=error.name)});"
        "t.context.resume().then(()=>globalThis.pocSummary='UNTRUSTED-RESUMED',"
        "error=>globalThis.pocSummary=error.name==='NotAllowedError'&&"
        "t.context.state==='suspended'?'BLOCKED':'WRONG-REFUSAL')})()",
        "BLOCKED");
    CHECK(test.runtime->game_audio == NULL);
    check_backend(&test, 0);

    evaluate(&test,
        "audioTest.wav=()=>{const b=new Uint8Array(52),v=new DataView(b.buffer),"
        "text=(at,s)=>{for(let i=0;i<s.length;i++)b[at+i]=s.charCodeAt(i)},"
        "u16=(at,n)=>v.setUint16(at,n,true),u32=(at,n)=>v.setUint32(at,n,true);"
        "text(0,'RIFF');u32(4,44);text(8,'WAVE');text(12,'fmt ');u32(16,16);"
        "u16(20,1);u16(22,1);u32(24,44100);u32(28,88200);u16(32,2);u16(34,16);"
        "text(36,'data');u32(40,8);[1000,-1000,2000,-2000].forEach("
        "(n,i)=>v.setInt16(44+i*2,n,true));return b.buffer};"
        "audioTest.decode=()=>audioTest.context.decodeAudioData(audioTest.wav()).then("
        "buffer=>{audioTest.buffer=buffer;globalThis.pocSummary=buffer.length===4&&"
        "buffer.sampleRate===44100&&buffer.numberOfChannels===1?'DECODED':'BAD-BUFFER'},"
        "error=>globalThis.pocSummary=error.name);audioTest.decode()",
        "DECODED");
    activate(&test, "STARTED");
    check_backend(&test, 1);
    mix_exact(&test, result, 4, 1, 1, true, true);
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=audioTest.oneEnded===1&&audioTest.oneHandler===1"
        "?'ONCE':'WRONG-ENDED'", "ONCE");
    check_backend(&test, 0);
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=audioTest.oneEnded===1&&audioTest.oneHandler===1"
        "?'ONCE':'DUPLICATE-ENDED'", "ONCE");

    evaluate(&test,
        "(()=>{const t=audioTest,c=t.context,g=c.createGain(),p=c.createStereoPanner(),"
        "s=c.createBufferSource();g.gain.value=.25;p.pan.value=-1;"
        "s.buffer=t.buffer;s.loop=true;s.connect(g).connect(p).connect(c.destination);"
        "s.addEventListener('ended',()=>t.loopEnded++);s.onended=()=>t.loopHandler++;"
        "s.start();t.loop=s;t.gain=g;t.pan=p;const curve=new Float32Array([.5,.5]);"
        "g.gain.setValueCurveAtTime(curve,0,.01);curve.fill(0);"
        "globalThis.pocSummary='CURVE-COPIED'})()", "CURVE-COPIED");
    check_backend(&test, 1);
    mix_exact(&test, result, 8, 1, 2, true, false);

    evaluate(&test,
        "(()=>{const p=audioTest.gain.gain;p.cancelScheduledValues(0);"
        "const curve=new Float32Array([.75,.75]);p.setValueCurveAtTime(curve,0,.01);"
        "curve.fill(0);globalThis.pocSummary='CURVE-REPLACED'})()", "CURVE-REPLACED");
    mix_exact(&test, result, 8, 3, 4, true, false);
    evaluate(&test,
        "audioTest.gain.gain.cancelScheduledValues(0);"
        "globalThis.pocSummary='CURVE-CANCELLED'", "CURVE-CANCELLED");
    mix_exact(&test, result, 8, 1, 4, true, false);

    evaluate(&test,
        "audioTest.context.suspend().then(()=>{audioTest.frozen=audioTest.context.currentTime;"
        "globalThis.pocSummary=audioTest.context.state==='suspended'?'SUSPENDED':'BAD-STATE'})",
        "SUSPENDED");
    mix_exact(&test, result, 8, 0, 1, false, false);
    CHECK(script_runtime_advance(test.runtime, 25, 4, &test.result));
    evaluate(&test,
        "globalThis.pocSummary=audioTest.context.currentTime===audioTest.frozen&&"
        "audioTest.context.state==='suspended'?'CLOCK-FROZEN':'CLOCK-MOVED'", "CLOCK-FROZEN");
    activate(&test, "RESUMED");
    evaluate(&test,
        "globalThis.pocSummary=audioTest.context.state==='running'?'RUNNING':'BAD-STATE'",
        "RUNNING");
    mix_exact(&test, result, 8, 1, 4, true, false);
    evaluate(&test,
        "audioTest.gain.gain.value=.5;audioTest.pan.pan.value=1;"
        "globalThis.pocSummary='MIX-CHANGED'", "MIX-CHANGED");
    mix_exact(&test, result, 8, 1, 2, false, true);

    evaluate(&test, "audioTest.loop.stop();globalThis.pocSummary='STOPPED'", "STOPPED");
    mix_exact(&test, result, 8, 0, 1, false, false);
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=audioTest.loopEnded===1&&audioTest.loopHandler===1&&"
        "audioTest.oneEnded===1&&audioTest.oneHandler===1?'ALL-ONCE':'WRONG-ENDED'", "ALL-ONCE");
    check_backend(&test, 0);
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=audioTest.loopEnded===1&&audioTest.loopHandler===1"
        "?'STOP-ONCE':'DUPLICATE-ENDED'", "STOP-ONCE");

    evaluate(&test,
        "audioTest.context.close().then(()=>globalThis.pocSummary="
        "audioTest.context.state==='closed'?'CLOSED':'BAD-STATE')", "CLOSED");
    CHECK(test.runtime->game_audio == NULL);
    evaluate(&test,
        "audioTest.oldContext=audioTest.context;audioTest.context=new AudioContext();"
        "audioTest.started=false;audioTest.decode()", "DECODED");
    activate(&test, "STARTED");
    check_backend(&test, 1);
    mix_exact(&test, result, 4, 1, 1, true, true);
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=audioTest.oldContext.state==='closed'&&"
        "audioTest.context.state==='running'&&audioTest.oneEnded===2&&"
        "audioTest.oneHandler===2&&audioTest.loopEnded===1&&audioTest.loopHandler===1"
        "?'RESTARTED':'BAD-RESTART'", "RESTARTED");
    check_backend(&test, 0);
    CHECK(result->count == BLOCKS);
    /* Teardown owns an open context/engine, decoded PCM and retained JS graph. */
    CHECK(test.runtime->game_audio != NULL);
    end_case(&test);
}

static void mix_oscillators(AudioCase *test, OscillatorResult *result, bool audible)
{
    CHECK(test->runtime->game_audio != NULL && result->count < OSCILLATOR_BLOCKS);
    int16_t *pcm = result->pcm[result->count++];
    size_t allocations = test->budget.allocation_count;
    CHECK(tilefinch_game_audio_mix(test->runtime->game_audio, pcm,
                                  TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES));
    CHECK(test->budget.allocation_count == allocations);
    bool nonzero = false;
    for (size_t frame = 0; frame < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES; frame++) {
        /* Shared gain with no panner has identical left and right channels. */
        CHECK(pcm[frame * 2u] == pcm[frame * 2u + 1u]);
        if (pcm[frame * 2u] != 0) nonzero = true;
    }
    CHECK(nonzero == audible);
}

static void run_oscillator_lane(bool native, OscillatorResult *result)
{
    AudioCase test;
    begin_case(&test, native);
    memset(result, 0, sizeof(*result));
    evaluate(&test,
        "globalThis.synthTest={context:new AudioContext(),voices:[],"
        "ended:[0,0,0,0],handlers:[0,0,0,0]};"
        "document.getElementById('start').addEventListener('click',()=>{"
        "const t=synthTest,c=t.context;c.resume().then(()=>{"
        "const gain=c.createGain();gain.gain.value=.125;gain.connect(c.destination);"
        "t.gain=gain;const types=['sine','square','sawtooth','triangle'];"
        "for(let i=0;i<4;i++){const o=c.createOscillator();o.type=types[i];"
        "o.frequency.value=220+i*110;o.connect(gain);"
        "o.addEventListener('ended',()=>t.ended[i]++);"
        "o.onended=()=>t.handlers[i]++;o.start(0);t.voices.push(o)}"
        "const curve=new Float32Array([0,.25,.125]);"
        "gain.gain.setValueCurveAtTime(curve,0,.02);curve.fill(0);"
        "for(let i=0;i<4;i++){const frequency=220+i*110,"
        "pitch=new Float32Array([frequency,frequency*2]);"
        "t.voices[i].frequency.setValueCurveAtTime(pitch,0,.02);pitch.fill(1)}"
        "globalThis.pocSummary='FOUR-OSCILLATORS'"
        "},error=>globalThis.pocSummary=error.name)});"
        "globalThis.pocSummary='OSCILLATORS-READY'", "OSCILLATORS-READY");
    activate(&test, "FOUR-OSCILLATORS");
    check_backend(&test, 4);

    /* Both nonconstant curves cross a 512-frame boundary, without JS ticks. */
    mix_oscillators(&test, result, true);
    mix_oscillators(&test, result, true);
    evaluate(&test,
        "(()=>{const t=synthTest;t.gain.gain.cancelScheduledValues(0);"
        "for(let i=0;i<4;i++){t.voices[i].frequency.cancelScheduledValues(0);"
        "t.voices[i].frequency.value=225+i*110}"
        "t.gain.gain.setTargetAtTime(.25,0,.003);"
        "globalThis.pocSummary='FOUR-TARGETS'})()", "FOUR-TARGETS");
    mix_oscillators(&test, result, true);

    evaluate(&test,
        "synthTest.gain.gain.cancelScheduledValues(0);synthTest.gain.gain.value=.1875;"
        "synthTest.voices[1].stop(0);globalThis.pocSummary='MIDDLE-STOPPED'",
        "MIDDLE-STOPPED");
    mix_oscillators(&test, result, true);
    dispatch_completions(&test);
    check_backend(&test, 3);
    evaluate(&test,
        "globalThis.pocSummary=synthTest.ended.join(',')==='0,1,0,0'&&"
        "synthTest.handlers.join(',')==='0,1,0,0'?'THREE-REMAIN':'WRONG-COMPLETION'",
        "THREE-REMAIN");

    /* Completion compacts the native active list. Reusing this shared gain
       must rebuild its source mask rather than address the old positions. */
    evaluate(&test,
        "(()=>{const p=synthTest.gain.gain;p.value=.25;"
        "const curve=new Float32Array([.25,.125]);p.setValueCurveAtTime(curve,0,.01);"
        "curve.fill(0);globalThis.pocSummary='THREE-CURVES'})()", "THREE-CURVES");
    mix_oscillators(&test, result, true);
    evaluate(&test,
        "synthTest.gain.gain.cancelScheduledValues(0);synthTest.voices[2].stop(0);"
        "globalThis.pocSummary='SECOND-MIDDLE-STOPPED'", "SECOND-MIDDLE-STOPPED");
    mix_oscillators(&test, result, true);
    dispatch_completions(&test);
    check_backend(&test, 2);
    evaluate(&test,
        "globalThis.pocSummary=synthTest.ended.join(',')==='0,1,1,0'&&"
        "synthTest.handlers.join(',')==='0,1,1,0'?'TWO-REMAIN':'WRONG-COMPLETION'",
        "TWO-REMAIN");
    evaluate(&test,
        "synthTest.gain.gain.value=.375;"
        "synthTest.gain.gain.setTargetAtTime(.125,0,.002);"
        "globalThis.pocSummary='TWO-TARGETS'", "TWO-TARGETS");
    mix_oscillators(&test, result, true);

    evaluate(&test,
        "synthTest.gain.gain.cancelScheduledValues(0);"
        "synthTest.voices[0].stop(0);synthTest.voices[3].stop(0);"
        "globalThis.pocSummary='ALL-STOPPED'", "ALL-STOPPED");
    mix_oscillators(&test, result, false);
    dispatch_completions(&test);
    check_backend(&test, 0);
    evaluate(&test,
        "globalThis.pocSummary=synthTest.ended.every(n=>n===1)&&"
        "synthTest.handlers.every(n=>n===1)?'FOUR-ENDED-ONCE':'WRONG-COMPLETION'",
        "FOUR-ENDED-ONCE");
    dispatch_completions(&test);
    evaluate(&test,
        "globalThis.pocSummary=synthTest.ended.every(n=>n===1)&&"
        "synthTest.handlers.every(n=>n===1)?'STILL-ONCE':'DUPLICATE-COMPLETION'",
        "STILL-ONCE");
    evaluate(&test,
        "synthTest.context.close().then(()=>globalThis.pocSummary="
        "synthTest.context.state==='closed'?'OSCILLATORS-CLOSED':'BAD-STATE')",
        "OSCILLATORS-CLOSED");
    CHECK(test.runtime->game_audio == NULL && result->count == OSCILLATOR_BLOCKS);
    end_case(&test);
}

static double mix_rms(AudioCase *test, int16_t *pcm, size_t frames, size_t from, size_t to)
{
    CHECK(frames <= TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES && from < to && to <= frames);
    size_t allocations = test->budget.allocation_count;
    CHECK(tilefinch_game_audio_mix(test->runtime->game_audio, pcm, frames));
    CHECK(test->budget.allocation_count == allocations);
    double sum = 0;
    for (size_t frame = from; frame < to; frame++)
        sum += (double) pcm[frame * 2u] * pcm[frame * 2u];
    return sqrt(sum / (double) (to - from));
}

/* Native-only Web Audio behaviour the legacy reference model never had:
   timelines, value reads, AudioBuffer PCM access and BiquadFilterNode. */
static void run_spec_lane(void)
{
    AudioCase test;
    begin_case(&test, true);
    static int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    evaluate(&test,
        "globalThis.specTest={context:new AudioContext()};"
        "document.getElementById('start').onclick=()=>specTest.context.resume().then("
        "()=>pocSummary='RUNNING');pocSummary='READY'", "READY");
    activate(&test, "RUNNING");

    /* Argument errors, the event quota and the pan restriction. */
    evaluate(&test,
        "(()=>{const c=specTest.context,g=c.createGain(),p=g.gain,names=[],"
        "attempt=f=>{try{f();names.push('ok')}catch(e){names.push(e.name)}};"
        "attempt(()=>p.exponentialRampToValueAtTime(0,1));"
        "attempt(()=>p.setValueAtTime(NaN,1));"
        "attempt(()=>p.setValueAtTime(1,-1));"
        "attempt(()=>p.setTargetAtTime(1,1,-1));"
        "attempt(()=>p.setValueCurveAtTime(new Float32Array([1]),1,1));"
        "attempt(()=>p.setValueCurveAtTime(new Float32Array([0,1]),1,1));"
        "attempt(()=>p.setValueAtTime(1,1.5));"
        "attempt(()=>p.cancelAndHoldAtTime(0));"
        "attempt(()=>p.cancelScheduledValues(0));"
        "const chained=p.setValueAtTime(0,2)===p;p.cancelScheduledValues(0);"
        "attempt(()=>{for(let i=0;i<33;i++)p.setValueAtTime(i/33,1+i/100)});"
        "attempt(()=>c.createStereoPanner().pan.linearRampToValueAtTime(1,c.currentTime+1));"
        "const expected='RangeError,TypeError,RangeError,RangeError,InvalidStateError,ok,"
        "NotSupportedError,ok,ok,QuotaExceededError,NotSupportedError';"
        "pocSummary=chained&&names.join()===expected?'ERRORS':names.join()})()", "ERRORS");

    /* A beep, scheduled ahead: the 1024-frame ramp starts with the voice
       and plays without JS ticks; the gain then holds 0.5. */
    evaluate(&test,
        "(()=>{const c=specTest.context,o=c.createOscillator(),g=c.createGain(),"
        "t=c.currentTime+.02;o.connect(g).connect(c.destination);g.gain.setValueAtTime(0,t);"
        "g.gain.linearRampToValueAtTime(.5,t+1024/44100);o.start(t);"
        "specTest.beep={o,g,t};pocSummary='BEEP'})()", "BEEP");
    static int16_t beep[6u * 512u * 2u];
    for (size_t block = 0; block < 6u; block++)
        (void) mix_rms(&test, beep + block * 1024u, 512, 0, 512);
    size_t onset = 0;
    while (onset < 6u * 512u && beep[onset * 2u] == 0) onset++;
    CHECK(onset > 0u && onset + 1536u <= 6u * 512u);
    double ramp_start = 0, ramp_end = 0, hold = 0;
    for (size_t frame = 0; frame < 128u; frame++) {
        double a = beep[(onset + frame) * 2u], b = beep[(onset + 896u + frame) * 2u];
        ramp_start += a * a;
        ramp_end += b * b;
    }
    for (size_t frame = onset + 1024u; frame < onset + 1536u; frame++)
        hold += (double) beep[frame * 2u] * beep[frame * 2u];
    hold = sqrt(hold / 512.0);
    CHECK(ramp_end > 49.0 * ramp_start);
    CHECK(hold > 0.5 * 0.7071 * 32768.0 * 0.97 && hold < 0.5 * 0.7071 * 32768.0 * 1.03);
    evaluate(&test,
        "(()=>{const b=specTest.beep;b.o.stop();pocSummary='BEEP-DONE'})()", "BEEP-DONE");
    CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
    dispatch_completions(&test);

    /* .value reads the timeline at currentTime. */
    evaluate(&test,
        "(()=>{const c=specTest.context,g=c.createGain(),t=c.currentTime;"
        "g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(1,t+1);"
        "g.gain.setValueAtTime(.25,t+2);specTest.read={g,t};pocSummary='RAMP-SET'})()",
        "RAMP-SET");
    CHECK(script_runtime_advance(test.runtime, 500, 4, &test.result));
    evaluate(&test,
        "(()=>{const r=specTest.read,c=specTest.context,v=r.g.gain.value,"
        "x=(c.currentTime-r.t);pocSummary=Math.abs(v-x)<.01&&x>.4&&x<.6?'VALUE-READ':"
        "JSON.stringify([v,x])})()", "VALUE-READ");
    evaluate(&test,
        "(()=>{const r=specTest.read,c=specTest.context,p=r.g.gain,now=c.currentTime-r.t;"
        "p.cancelAndHoldAtTime(c.currentTime);const held=p.value;"
        "pocSummary=Math.abs(held-now)<.01?'HELD':JSON.stringify([held,now])})()", "HELD");

    /* createBuffer + getChannelData: one shared view, heard as written. */
    evaluate(&test,
        "(()=>{const c=specTest.context,b=c.createBuffer(1,4,44100),d=b.getChannelData(0);"
        "d.set([.5,-.5,.25,-.25]);const s=c.createBufferSource();s.buffer=b;s.loop=true;"
        "s.connect(c.destination);s.start();specTest.pcm={b,d,s};"
        "pocSummary=d===b.getChannelData(0)&&d instanceof Float32Array&&d.length===4&&"
        "b.duration===4/44100?'BUFFER':'BAD-BUFFER'})()", "BUFFER");
    CHECK(tilefinch_game_audio_mix(test.runtime->game_audio, pcm, 4));
    static const int16_t written[4] = {16384, -16384, 8192, -8192};
    for (size_t frame = 0; frame < 4u; frame++) {
        CHECK(pcm[frame * 2u] == written[frame]);
        CHECK(pcm[frame * 2u + 1u] == written[frame]);
    }
    evaluate(&test,
        "(()=>{const p=specTest.pcm;p.d.fill(.125);const out=new Float32Array(6),"
        "names=[];p.b.copyFromChannel(out,0,1);"
        "p.b.copyToChannel(new Float32Array([-.125]),0,3);"
        "for(const f of [()=>p.b.getChannelData(1),()=>p.b.copyToChannel([1],0),"
        "()=>specTest.context.createBuffer(3,4,44100),"
        "()=>specTest.context.createBuffer(1,4,4000),"
        "()=>specTest.context.createBuffer(1,0,44100),"
        "()=>new AudioBuffer({}),()=>specTest.context.createBuffer(2,200000,44100)])"
        "try{f();names.push('ok')}catch(e){names.push(e.name)}"
        "const made=new AudioBuffer({length:8,sampleRate:22050});"
        "pocSummary=out.join()==='0.125,0.125,0.125,0,0,0'&&made.numberOfChannels===1&&"
        "made.sampleRate===22050&&made.getChannelData(0).every(v=>v===0)&&names.join()==="
        "'IndexSizeError,TypeError,NotSupportedError,NotSupportedError,NotSupportedError,"
        "TypeError,NotSupportedError'?'COPIES':names.join()+' '+out.join()})()", "COPIES");
    CHECK(tilefinch_game_audio_mix(test.runtime->game_audio, pcm, 4));
    static const int16_t rewritten[4] = {4096, 4096, 4096, -4096};
    for (size_t frame = 0; frame < 4u; frame++) CHECK(pcm[frame * 2u] == rewritten[frame]);
    evaluate(&test, "specTest.pcm.s.stop();pocSummary='PCM-STOPPED'", "PCM-STOPPED");
    CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
    dispatch_completions(&test);

    /* Decoded WAV PCM reads back as normalized floats. */
    evaluate(&test,
        "(()=>{const b=new Uint8Array(52),v=new DataView(b.buffer),"
        "text=(at,s)=>{for(let i=0;i<s.length;i++)b[at+i]=s.charCodeAt(i)};"
        "text(0,'RIFF');v.setUint32(4,44,true);text(8,'WAVE');text(12,'fmt ');"
        "v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);"
        "v.setUint32(24,44100,true);v.setUint32(28,88200,true);v.setUint16(32,2,true);"
        "v.setUint16(34,16,true);text(36,'data');v.setUint32(40,8,true);"
        "[16384,-16384,8192,0].forEach((n,i)=>v.setInt16(44+i*2,n,true));"
        "return specTest.context.decodeAudioData(b.buffer).then(buffer=>{"
        "const out=new Float32Array(4);buffer.copyFromChannel(out,0);"
        "const view=buffer.getChannelData(0);"
        "pocSummary=out.join()==='0.5,-0.5,0.25,0'&&view.join()===out.join()"
        "?'DECODED-PCM':out.join()+' '+view.join()})})()", "DECODED-PCM");

    /* Compressed formats are refused by name. */
    evaluate(&test,
        "(()=>{const c=specTest.context,ogg=new Uint8Array(64),mp3=new Uint8Array(64);"
        "ogg.set([79,103,103,83]);mp3.set([0xff,0xfb,0x90,0x64]);"
        "const refused=b=>c.decodeAudioData(b.buffer).then(()=>'decoded',"
        "e=>e.name+':'+/Ogg|MP3/.exec(e.message));"
        "return Promise.all([refused(ogg),refused(mp3)]).then(r=>pocSummary="
        "r.join()==='EncodingError:Ogg,EncodingError:MP3'?'COMPRESSED-REFUSED':r.join())})()",
        "COMPRESSED-REFUSED");

    /* BiquadFilterNode: attributes, then lowpass against highpass. */
    evaluate(&test,
        "(()=>{const c=specTest.context,f=c.createBiquadFilter(),g=new BiquadFilterNode(c,"
        "{type:'peaking',Q:3,gain:6,frequency:1000});f.type='bogus';"
        "pocSummary=f instanceof AudioNode&&f.type==='lowpass'&&f.frequency.value===350&&"
        "f.Q.value===1&&f.gain.value===0&&f.detune.value===0&&g.type==='peaking'&&"
        "g.Q.value===3&&g.gain.value===6&&g.frequency.value===1000&&"
        "f.frequency.maxValue===22050?'FILTER-ATTRIBUTES':'BAD-FILTER'})()",
        "FILTER-ATTRIBUTES");
    double filtered[2];
    for (unsigned pass = 0; pass < 2u; pass++) {
        evaluate(&test, pass == 0u
            ? "(()=>{const c=specTest.context,o=c.createOscillator(),f=c.createBiquadFilter();"
              "f.frequency.value=100;o.connect(f).connect(c.destination);o.start();"
              "specTest.filter={o,f};pocSummary='LOWPASS'})()"
            : "(()=>{const c=specTest.context,o=c.createOscillator(),f=c.createBiquadFilter();"
              "f.type='highpass';f.frequency.value=100;o.connect(f).connect(c.destination);"
              "o.start();specTest.filter={o,f};pocSummary='HIGHPASS'})()",
            pass == 0u ? "LOWPASS" : "HIGHPASS");
        (void) mix_rms(&test, pcm, 512, 0, 512);
        filtered[pass] = mix_rms(&test, pcm, 512, 0, 512);
        evaluate(&test, "specTest.filter.o.stop();pocSummary='FILTER-STOPPED'",
                 "FILTER-STOPPED");
        CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
        dispatch_completions(&test);
    }
    /* 440 Hz is 2.2 octaves above a 100 Hz cutoff: about -26 dB lowpass. */
    CHECK(filtered[0] > 0.0 && filtered[0] * 10.0 < filtered[1]);
    CHECK(filtered[1] > 0.70 * 32768.0 * 0.9);

    /* An automated cutoff sweep runs in the mixer. */
    evaluate(&test,
        "(()=>{const c=specTest.context,o=c.createOscillator(),f=c.createBiquadFilter(),"
        "t=c.currentTime+.02;o.frequency.value=2000;f.frequency.setValueAtTime(100,t);"
        "f.frequency.exponentialRampToValueAtTime(8000,t+1024/44100);"
        "o.connect(f).connect(c.destination);o.start(t);specTest.filter={o,f};"
        "pocSummary='SWEEP'})()", "SWEEP");
    for (size_t block = 0; block < 6u; block++)
        (void) mix_rms(&test, beep + block * 1024u, 512, 0, 512);
    onset = 0;
    while (onset < 6u * 512u && beep[onset * 2u] == 0) onset++;
    CHECK(onset > 0u && onset + 1152u <= 6u * 512u);
    double quiet = 0, open = 0;
    for (size_t frame = 128; frame < 256u; frame++) {
        double a = beep[(onset + frame) * 2u], b = beep[(onset + 896u + frame) * 2u];
        quiet += a * a;
        open += b * b;
    }
    /* A 2 kHz tone under a 100 Hz cutoff, then under an 8 kHz one. */
    CHECK(quiet * 100.0 < open);
    evaluate(&test, "specTest.filter.o.stop();pocSummary='SWEEP-STOPPED'", "SWEEP-STOPPED");
    CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
    dispatch_completions(&test);
    end_case(&test);
}

/* Graph edits that a playing source cannot take are refused for that call
   only; lanes and node slots come back without the cycle collector where
   they can; loop points follow the specification. Native only. */
static void run_graph_lane(void)
{
    AudioCase test;
    begin_case(&test, true);
    static int16_t pcm[TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u];
    evaluate(&test,
        "globalThis.G={context:new AudioContext(),kept:[]};"
        "document.getElementById('start').onclick=()=>G.context.resume().then("
        "()=>pocSummary='RUNNING');pocSummary='READY'", "READY");
    activate(&test, "RUNNING");

    /* A playing source through five gains, four automated: automating the
       fifth, or connecting it to a fifth automated gain, throws, and the
       graph is as it was, so later edits on the same path still work. */
    evaluate(&test,
        "(()=>{const c=G.context,o=c.createOscillator(),g=[],names=[],"
        "attempt=f=>{try{f();names.push('ok')}catch(e){names.push(e.name)}};"
        "for(let i=0;i<5;i++)g.push(c.createGain());o.connect(g[0]);"
        "for(let i=0;i<4;i++)g[i].connect(g[i+1]);g[4].connect(c.destination);"
        "const t=c.currentTime+1;for(let i=0;i<4;i++)g[i].gain.setValueAtTime(1,t);"
        "o.start();attempt(()=>g[4].gain.setValueAtTime(.5,t));"
        "attempt(()=>{g[4].gain.value=.25});"
        "attempt(()=>{g[3].disconnect();g[3].connect(g[4])});"
        "const h=c.createGain();h.gain.setValueAtTime(1,t);h.connect(c.destination);"
        "attempt(()=>g[4].connect(h));attempt(()=>c.createGain().gain.setValueAtTime(1,t));"
        "G.chain={o,g,h};pocSummary=names.join()+(g[4].gain.value===.25?'':' value')})()",
        "NotSupportedError,ok,ok,NotSupportedError,ok");
    (void) mix_rms(&test, pcm, 512, 0, 512);
    double heard = mix_rms(&test, pcm, 512, 0, 512);
    /* g[4] still reaches the destination, at its static 0.25. */
    CHECK(heard > 0.25 * 0.7071 * 32768.0 * 0.97 && heard < 0.25 * 0.7071 * 32768.0 * 1.03);
    evaluate(&test, "G.chain.o.stop();G.chain=null;pocSummary='CHAIN-STOPPED'",
             "CHAIN-STOPPED");
    CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
    dispatch_completions(&test);
    end_case(&test);

    /* Six kept sounds of three automated parameters each need 18 lanes of
       16: the settled ones of earlier sounds go back without a collection
       (none could free them: every node is reachable). */
    begin_case(&test, true);
    evaluate(&test,
        "globalThis.G={context:new AudioContext(),kept:[]};"
        "document.getElementById('start').onclick=()=>G.context.resume().then("
        "()=>pocSummary='RUNNING');pocSummary='READY'", "READY");
    activate(&test, "RUNNING");
    static const char *const sounds[] = {"S1", "S2", "S3", "S4", "S5", "S6"};
    for (size_t at = 0; at < 6u; at++) {
        evaluate(&test,
            "(()=>{const c=G.context,t=c.currentTime,o=c.createOscillator(),g=c.createGain();"
            "g.gain.setValueAtTime(0,t);g.gain.linearRampToValueAtTime(.5,t+.01);"
            "o.frequency.setValueAtTime(880,t);o.detune.setValueAtTime(100,t);"
            "o.connect(g).connect(c.destination);G.kept.push({o,g,t});"
            "pocSummary='S'+G.kept.length})()", sounds[at]);
        CHECK(script_runtime_advance(test.runtime, 50, 4, &test.result));
    }
    /* A returned lane reads its settled value, and automation resuming on
       a new lane ramps from the settled event (t + 0.01), not from now
       (which would read 0.5); currentTime is quantized, hence 5e-3. */
    evaluate(&test,
        "(()=>{const k=G.kept[0],c=G.context,now=c.currentTime,end=k.t+.01;"
        "const settled=k.g.gain.value===.5&&k.o.frequency.value===880&&"
        "k.o.detune.value===100;k.g.gain.linearRampToValueAtTime(1,now+1);"
        "const v=k.g.gain.value,want=.5+.5*(now-end)/(now+1-end);"
        "pocSummary=settled&&now-end>.2&&Math.abs(v-want)<5e-3?'ANCHORED':"
        "JSON.stringify([settled,v,want])})()", "ANCHORED");

    /* Unreachable nodes whose params were read wait for the cycle
       collector; a full table runs it before refusing. */
    evaluate(&test,
        "(()=>{const c=G.context;for(let i=0;i<24;i++){const g=c.createGain();"
        "g.gain.value=.5}pocSummary='NODES'})()", "NODES");

    /* loopStart/loopEnd: non-finite values throw; any finite pair is kept,
       and one that is not a valid pair loops the whole buffer. */
    evaluate(&test,
        "(()=>{const c=G.context,s=c.createBufferSource(),names=[];"
        "for(const v of [NaN,Infinity])try{s.loopStart=v;names.push('ok')}"
        "catch(e){names.push(e.name)}"
        "try{s.loopEnd=-Infinity;names.push('ok')}catch(e){names.push(e.name)}"
        "s.loopStart=-1;s.loopEnd=-2;const kept=s.loopStart===-1&&s.loopEnd===-2;"
        "const b=c.createBuffer(1,4,44100);b.getChannelData(0).set([.5,-.5,.25,-.25]);"
        "s.buffer=b;s.loop=true;s.connect(c.destination);s.start();G.loop=s;"
        "pocSummary=kept?names.join():'NOT-KEPT'})()", "TypeError,TypeError,TypeError");
    CHECK(tilefinch_game_audio_mix(test.runtime->game_audio, pcm, 8));
    static const int16_t looped[4] = {16384, -16384, 8192, -8192};
    for (size_t frame = 0; frame < 8u; frame++)
        CHECK(pcm[frame * 2u] == looped[frame % 4u]);
    evaluate(&test, "G.loop.stop();G.loop=null;pocSummary='LOOP-STOPPED'", "LOOP-STOPPED");
    CHECK(mix_rms(&test, pcm, 512, 0, 512) == 0.0);
    dispatch_completions(&test);
    end_case(&test);
}

static void run_context_construction_lane(bool native)
{
    AudioCase test;
    begin_case(&test, native);
    evaluate(&test,
        "(()=>{let refused=0;for(const C of [BaseAudioContext,class extends BaseAudioContext{}])"
        "try{new C()}catch(e){if(e instanceof TypeError)refused++}"
        "pocSummary=refused===2?'ABSTRACT':'EXTRA-CONTEXT'})()", "ABSTRACT");
    evaluate(&test,
        "globalThis.contextChild=class extends AudioContext{};"
        "globalThis.contextInstance=new contextChild;let refused=false;"
        "try{new AudioContext}catch(e){refused=e.name==='QuotaExceededError'}"
        "pocSummary=refused&&contextInstance instanceof contextChild&&"
        "contextInstance.state==='suspended'?'ONE-CONTEXT':'BAD-CONSTRUCTION'", "ONE-CONTEXT");
    evaluate(&test, "contextInstance.close().then(()=>pocSummary='CLOSED')", "CLOSED");
    end_case(&test);
}

static void run_context_clock_lane(bool native)
{
    AudioCase test;
    begin_case(&test, native);
    evaluate(&test,
        "globalThis.clockTest={context:new AudioContext,changes:0};"
        "clockTest.context.onstatechange=()=>clockTest.changes++;"
        "document.getElementById('start').onclick=()=>clockTest.context.resume().then("
        "()=>pocSummary='RUNNING');pocSummary='READY'", "READY");
    activate(&test, "RUNNING");
    CHECK(script_runtime_advance(test.runtime, 70, 4, &test.result));
    evaluate(&test,
        "clockTest.before=clockTest.context.currentTime;clockTest.context.close().then(()=>{"
        "clockTest.frozen=clockTest.context.currentTime;pocSummary=clockTest.before>0&&"
        "clockTest.frozen>=clockTest.before&&clockTest.context.state==='closed'"
        "?'CLOCK-CLOSED':'CLOCK-REWOUND'})", "CLOCK-CLOSED");
    CHECK(script_runtime_advance(test.runtime, 70, 4, &test.result));
    evaluate(&test,
        "Promise.all([clockTest.context.close(),clockTest.context.resume().then(()=>false,"
        "e=>e.name==='InvalidStateError'),clockTest.context.suspend().then(()=>false,"
        "e=>e.name==='InvalidStateError')]).then(results=>{pocSummary=results[1]&&results[2]&&"
        "clockTest.changes===2&&clockTest.context.currentTime===clockTest.frozen"
        "?'TERMINAL':'BAD-TERMINAL'})", "TERMINAL");
    CHECK(test.runtime->game_audio == NULL);
    end_case(&test);
}

static void run_closed_source_lane(bool native)
{
    AudioCase test;
    begin_case(&test, native);
    evaluate(&test,
        "globalThis.closedTest={old:new AudioContext};"
        "closedTest.source=closedTest.old.createOscillator();"
        "closedTest.old.close().then(()=>{closedTest.context=new AudioContext;"
        "document.getElementById('start').onclick=()=>closedTest.context.resume().then("
        "()=>pocSummary='RUNNING');pocSummary='READY'})", "READY");
    activate(&test, "RUNNING");
    evaluate(&test,
        "let refused=false;try{closedTest.source.start()}catch(e){"
        "refused=e.name==='InvalidStateError'}closedTest.old.close().then(()=>{"
        "pocSummary=refused&&closedTest.context.state==='running'"
        "?'SOURCE-RETIRED':'STALE-SOURCE-STARTED'})", "SOURCE-RETIRED");
    check_backend(&test, 0);
    int16_t pcm[16];
    memset(pcm, 0x55, sizeof(pcm));
    CHECK(tilefinch_game_audio_mix(test.runtime->game_audio, pcm, 8));
    for (size_t at = 0; at < 16u; at++) CHECK(pcm[at] == 0);
    end_case(&test);
}

int main(int argc, char **argv)
{
    AudioResult reference, native;
    OscillatorResult reference_oscillators, native_oscillators;
    script_runtime_configure_deterministic_replay(true, 17);
    if (argc == 2) {
        if (!strcmp(argv[1], "--constructors-reference"))
            run_context_construction_lane(false);
        else if (!strcmp(argv[1], "--clock-reference")) run_context_clock_lane(false);
        else if (!strcmp(argv[1], "--clock-native")) run_context_clock_lane(true);
        else if (!strcmp(argv[1], "--closed-source-reference")) run_closed_source_lane(false);
        else return 2;
        return 0;
    }
    run_context_construction_lane(false);
    run_context_construction_lane(true);
    run_context_clock_lane(false);
    run_context_clock_lane(true);
    run_closed_source_lane(false);
    run_closed_source_lane(true);
    run_lane(false, &reference);
    run_lane(true, &native);
    run_oscillator_lane(false, &reference_oscillators);
    run_oscillator_lane(true, &native_oscillators);
    run_spec_lane();
    run_graph_lane();
    script_runtime_configure_deterministic_replay(false, 0);
    CHECK(reference.count == native.count);
    CHECK(memcmp(reference.pcm, native.pcm, sizeof(reference.pcm)) == 0);
    CHECK(reference_oscillators.count == native_oscillators.count);
    /* Native AudioParam curves follow the spec's interpolation over the
       wall-clock timeline; the legacy reference model does not, so the two
       agree only until the pitch curves have drifted their phases. */
    for (unsigned i = 0; i < TILEFINCH_GAME_AUDIO_OUTPUT_FRAMES * 2u; i++)
        CHECK(abs(reference_oscillators.pcm[0][i] - native_oscillators.pcm[0][i]) <= 64);
    CHECK(memcmp(reference_oscillators.pcm[OSCILLATOR_BLOCKS - 1u],
                 native_oscillators.pcm[OSCILLATOR_BLOCKS - 1u],
                 sizeof(reference_oscillators.pcm[0])) == 0);
    puts("game audio slots: exact PCM, four voices, automation, compaction and lifecycle passed");
    return 0;
}
