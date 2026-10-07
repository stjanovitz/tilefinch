/* Normal AudioContext construction with bounded host-only refusal points.
   This tests lazy installation/publication and native record ownership, not
   mixer allocation, worker startup, or device failure handling. */
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/viewport.h"
#include "../src/js_runtime_internal.h"
#include "../src/tilefinch_test_faults.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
static const char *case_name;
static unsigned case_ordinal;
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s[%u] failed at %s:%d: %s\n",                       \
                case_name, case_ordinal, __FILE__, __LINE__, #condition);    \
        exit(1);                                                             \
    }                                                                        \
} while (0)

enum { INSTALL_ADMISSIONS = 42, PUBLISH_ADMISSIONS = 11 };
static const char *const constructor_names[] = {
    "AudioParam", "AudioNode", "GainNode", "StereoPannerNode",
    "OscillatorNode", "AudioBufferSourceNode", "AudioDestinationNode",
    "AudioBuffer", "BaseAudioContext", "AudioContext", "webkitAudioContext"
};

typedef struct {
    Budget budget;
    PocDocument document;
    ScriptRuntime *runtime;
    ScriptResult result;
} AudioCase;

typedef struct {
    int present;
    int flags;
    bool value_function;
    bool getter_function;
    bool setter_function;
} PropertyShape;

static void clear_faults(void)
{
    TilefinchTestFaults *faults = tilefinch_test_faults();
    faults->audio_slot_allocation_fail_at = 0;
    faults->audio_slot_install_fail_at = 0;
    faults->audio_slot_publish_fail_at = 0;
}

static void begin_case(AudioCase *test)
{
    clear_faults();
    memset(test, 0, sizeof(*test));
    budget_init(&test->budget, 24u * MIB);
    CHECK(budget_install_lexbor(&test->budget));
    static const char html[] =
        "<!doctype html><html><body><button id='start'>Start</button></body></html>";
    CHECK(document_parse(&test->document, &test->budget, html, sizeof(html) - 1u, 17));
    ScriptRuntimeOptions options = {
        .defer_document_scripts = true,
        .game_audio_internal_slots = true
    };
    CHECK(viewport_context_init(&options.viewport, 480, 272, 480, 272));
    CHECK(script_execution_policy_for_profile(
        SCRIPT_EXECUTION_PROFILE_PSP_REALISTIC, &options.execution_policy));
    options.execution_policy.slow_compile_threshold_us = UINT64_MAX;
    options.execution_policy.slow_callback_threshold_us = UINT64_MAX;
    test->runtime = script_runtime_create_configured(
        &test->document, &test->budget, 5u * MIB, 30000,
        "https://audio-slots-faults.test/", &options, &test->result);
    CHECK(test->runtime != NULL && test->result.success);
    CHECK(test->runtime->game_audio_internal_slots);
    CHECK(test->runtime->game_audio_slots == NULL);
    CHECK(test->runtime->game_audio == NULL);
}

static void end_case(AudioCase *test)
{
    clear_faults();
    script_runtime_destroy(test->runtime);
    document_destroy(&test->document);
    CHECK(test->budget.current == 0u);
    CHECK(test->budget.external_reserved == 0u);
    CHECK(budget_active_allocations(&test->budget, NULL) == 0u);
    CHECK(budget_uninstall_lexbor(&test->budget));
}

static JSValue construct(AudioCase *test)
{
    static const char source[] = "new AudioContext()";
    return JS_Eval(test->runtime->context, source, sizeof(source) - 1u,
                   "<audio-slots-faults>", JS_EVAL_TYPE_GLOBAL);
}

static void consume_exception(AudioCase *test, JSValue value)
{
    CHECK(JS_IsException(value));
    CHECK(JS_HasException(test->runtime->context));
    JSValue exception = JS_GetException(test->runtime->context);
    CHECK(!JS_IsUndefined(exception));
    JS_FreeValue(test->runtime->context, exception);
    CHECK(!JS_HasException(test->runtime->context));
}

static void empty_records(AudioCase *test)
{
    unsigned counts[5] = {0};
    JS_RunGC(test->runtime->runtime);
    CHECK(js_rt_audio_slot_test_snapshot(test->runtime, counts));
    for (unsigned i = 0; i < 5; i++) CHECK(counts[i] == 0u);
    CHECK(test->runtime->game_audio == NULL);
}

static void capture_surface(AudioCase *test, PropertyShape surface[PUBLISH_ADMISSIONS])
{
    JSContext *ctx = test->runtime->context;
    JSValue global = JS_GetGlobalObject(ctx);
    CHECK(!JS_IsException(global));
    for (unsigned i = 0; i < PUBLISH_ADMISSIONS; i++) {
        JSAtom atom = JS_NewAtom(ctx, constructor_names[i]);
        CHECK(atom != JS_ATOM_NULL);
        JSPropertyDescriptor descriptor = {
            .value = JS_UNDEFINED, .getter = JS_UNDEFINED, .setter = JS_UNDEFINED
        };
        int present = JS_GetOwnProperty(ctx, &descriptor, global, atom);
        CHECK(present >= 0);
        surface[i] = (PropertyShape) {
            .present = present,
            .flags = descriptor.flags & (JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE |
                                          JS_PROP_WRITABLE | JS_PROP_GETSET),
            .value_function = JS_IsFunction(ctx, descriptor.value),
            .getter_function = JS_IsFunction(ctx, descriptor.getter),
            .setter_function = JS_IsFunction(ctx, descriptor.setter)
        };
        JS_FreeValue(ctx, descriptor.value);
        JS_FreeValue(ctx, descriptor.getter);
        JS_FreeValue(ctx, descriptor.setter);
        JS_FreeAtom(ctx, atom);
    }
    JS_FreeValue(ctx, global);
    CHECK(!JS_HasException(ctx));
}

static void check_lazy_surface(const PropertyShape surface[PUBLISH_ADMISSIONS])
{
    unsigned accessors = 0;
    for (unsigned i = 0; i < PUBLISH_ADMISSIONS; i++) {
        CHECK(!surface[i].value_function);
        if (!surface[i].present) continue;
        CHECK(surface[i].flags & JS_PROP_GETSET);
        CHECK(surface[i].getter_function && surface[i].setter_function);
        accessors++;
    }
    CHECK(accessors == 6u);
}

static void check_rollback(AudioCase *test,
                           const PropertyShape before[PUBLISH_ADMISSIONS])
{
    PropertyShape after[PUBLISH_ADMISSIONS];
    /* Descriptor reads do not invoke lazy getters. Reinstalled accessor
       identities may differ, but no data constructor may become visible. */
    capture_surface(test, after);
    check_lazy_surface(after);
    for (unsigned i = 0; i < PUBLISH_ADMISSIONS; i++) {
        CHECK(before[i].present == after[i].present);
        CHECK(before[i].flags == after[i].flags);
        CHECK(before[i].value_function == after[i].value_function);
        CHECK(before[i].getter_function == after[i].getter_function);
        CHECK(before[i].setter_function == after[i].setter_function);
    }
    CHECK(JS_IsUndefined(test->runtime->game_audio_host_suspend));
    CHECK(JS_IsUndefined(test->runtime->game_audio_host_complete));
    empty_records(test);
}

static void close_success(AudioCase *test, JSValue context)
{
    JSContext *ctx = test->runtime->context;
    CHECK(!JS_IsException(context) && JS_IsObject(context));
    CHECK(!JS_HasException(ctx));
    CHECK(JS_IsFunction(ctx, test->runtime->game_audio_host_suspend));
    CHECK(JS_IsFunction(ctx, test->runtime->game_audio_host_complete));
    unsigned counts[5] = {0};
    CHECK(js_rt_audio_slot_test_snapshot(test->runtime, counts));
    CHECK(counts[0] == 1u && counts[1] == 1u);
    CHECK(counts[2] == 0u && counts[3] == 0u && counts[4] == 0u);
    CHECK(test->runtime->game_audio == NULL);
    JSValue close = JS_GetPropertyStr(ctx, context, "close");
    CHECK(JS_IsFunction(ctx, close));
    JSValue promise = JS_Call(ctx, close, context, 0, NULL);
    CHECK(!JS_IsException(promise));
    CHECK(JS_PromiseState(ctx, promise) == JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, promise);
    JS_FreeValue(ctx, close);
    JS_FreeValue(ctx, context);
    for (unsigned at = 0; at < 32u && JS_IsJobPending(test->runtime->runtime); at++)
        CHECK(script_runtime_advance(test->runtime, 0, 4, &test->result));
    CHECK(!JS_IsJobPending(test->runtime->runtime));
    empty_records(test);
}

static void fresh_retry(void)
{
    AudioCase test;
    begin_case(&test);
    close_success(&test, construct(&test));
    end_case(&test);
}

static void installation_trial(bool publication, unsigned ordinal)
{
    case_name = publication ? "publication" : "installation";
    case_ordinal = ordinal;
    AudioCase test;
    begin_case(&test);
    PropertyShape before[PUBLISH_ADMISSIONS];
    capture_surface(&test, before);
    check_lazy_surface(before);
    unsigned *remaining = publication
        ? &tilefinch_test_faults()->audio_slot_publish_fail_at
        : &tilefinch_test_faults()->audio_slot_install_fail_at;
    unsigned bound = publication ? PUBLISH_ADMISSIONS : INSTALL_ADMISSIONS;
    *remaining = ordinal;
    JSValue context = construct(&test);
    if (ordinal <= bound) {
        CHECK(*remaining == 0u);
        consume_exception(&test, context);
        clear_faults();
        check_rollback(&test, before);
        /* A failed lazy module stays failed in this realm; it must not expose
           a partial installation on a second ordinary constructor access. */
        consume_exception(&test, construct(&test));
        check_rollback(&test, before);
    } else {
        /* The one-past-end control pins the complete bounded admission set. */
        CHECK(*remaining == 1u);
        clear_faults();
        close_success(&test, context);
    }
    end_case(&test);
    if (ordinal <= bound) fresh_retry();
}

static void allocation_trial(unsigned ordinal)
{
    case_name = "context-allocation";
    case_ordinal = ordinal;
    AudioCase test;
    begin_case(&test);
    tilefinch_test_faults()->audio_slot_allocation_fail_at = ordinal;
    consume_exception(&test, construct(&test));
    CHECK(tilefinch_test_faults()->audio_slot_allocation_fail_at == 0u);
    clear_faults();
    empty_records(&test);
    /* Installation succeeded; only the context or its destination was
       refused. Retry must not see a phantom exclusive active context. */
    CHECK(JS_IsFunction(test.runtime->context, test.runtime->game_audio_host_suspend));
    CHECK(JS_IsFunction(test.runtime->context, test.runtime->game_audio_host_complete));
    close_success(&test, construct(&test));
    end_case(&test);
    fresh_retry();
}

int main(void)
{
    script_runtime_configure_deterministic_replay(true, 17);
    for (unsigned at = 1; at <= INSTALL_ADMISSIONS + 1u; at++)
        installation_trial(false, at);
    for (unsigned at = 1; at <= PUBLISH_ADMISSIONS + 1u; at++)
        installation_trial(true, at);
    allocation_trial(1); /* AudioSlotContext ownership record. */
    allocation_trial(2); /* Destination AudioSlotNodeRef, after context ownership. */
    clear_faults();
    script_runtime_configure_deterministic_replay(false, 0);
    puts("game audio slots faults: 42 install + 11 publication + 2 allocation refusals; "
         "rollback, fresh retries and zero Budget ownership passed");
    return 0;
}
