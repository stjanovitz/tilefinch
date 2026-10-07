/* Exercise both implementations through public WebAudio APIs in real realms.
   The host mixer seam is synchronous: this does not qualify a PSP worker. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/game_audio.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"

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

int main(void)
{
    AudioResult reference, native;
    OscillatorResult reference_oscillators, native_oscillators;
    script_runtime_configure_deterministic_replay(true, 17);
    run_lane(false, &reference);
    run_lane(true, &native);
    run_oscillator_lane(false, &reference_oscillators);
    run_oscillator_lane(true, &native_oscillators);
    script_runtime_configure_deterministic_replay(false, 0);
    CHECK(reference.count == native.count);
    CHECK(memcmp(reference.pcm, native.pcm, sizeof(reference.pcm)) == 0);
    CHECK(reference_oscillators.count == native_oscillators.count);
    CHECK(memcmp(reference_oscillators.pcm, native_oscillators.pcm,
                 sizeof(reference_oscillators.pcm)) == 0);
    puts("game audio slots: exact PCM, four voices, automation, compaction and lifecycle passed");
    return 0;
}
