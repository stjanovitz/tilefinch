/* Exercise trusted audio selection through BrowserEngine and its shared
   navigation runtime factory. No audio worker or fetched child is started. */
#include "tilefinch/browser_engine.h"
#include "../src/js_runtime_internal.h"
#include "../src/tilefinch_test_faults.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "audio selection failed at %s:%d: %s\n",             \
                __FILE__, __LINE__, #condition);                             \
        exit(1);                                                             \
    }                                                                        \
} while (0)

/* -1 preserves the browser default; 0/1 exercise both explicit selectors. */
static BrowserEngine *create_engine(BrowserConfig *config, int selection)
{
    CHECK(selection >= -1 && selection <= 1);
    bool native = selection != 0;
    browser_config_init(config, NULL);
    CHECK(config->javascript.game_audio_internal_slots);
    config->memory_limit = 24u * MIB;
    config->session_cache_limit = 256u * 1024u;
    config->tile_capacity = 0;
    config->javascript.enabled = true;
    config->javascript.document_scripts_enabled = true;
    config->javascript.heap_limit = 5u * MIB;
    config->javascript.runtime_timeout_ms = 30000;
    config->javascript.execution_policy.slow_compile_threshold_us = UINT64_MAX;
    config->javascript.execution_policy.slow_callback_threshold_us = UINT64_MAX;
    if (selection >= 0)
        config->javascript.game_audio_internal_slots = native;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine creation: %s\n", error);
    CHECK(engine != NULL);
    CHECK(browser_engine_navigation(engine)->game_audio_internal_slots == native);
    return engine;
}

static ScriptRuntime *commit_page(BrowserEngine *engine, const char *url)
{
    static const char html[] =
        "<!doctype html><title>Audio selection</title><body>Ready"
        "<script>globalThis.pocSummary='READY'</script>";
    CHECK(browser_engine_commit_html(engine, url, html, sizeof(html) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->page.runtime != NULL);
    CHECK(strcmp(navigation->page.script_result.summary, "READY") == 0);
    return navigation->page.runtime;
}

static void check_status(BrowserEngine *engine, ScriptRuntime *runtime,
                          bool selected, bool installed, bool present, bool failed)
{
    Budget *budget = browser_engine_budget(engine);
    size_t allocations = budget->allocation_count;
    ScriptGameAudioStatus status = {0};
    CHECK(script_runtime_game_audio_status(runtime, &status));
    CHECK(status.selected_native == selected);
    CHECK(status.lazy_installed == installed);
    CHECK(status.native_present == present);
    CHECK(status.failed == failed);
    CHECK(budget->allocation_count == allocations);
    CHECK(!script_runtime_game_audio_status(runtime, NULL));
    /* Independently distinguish actual native records from a config echo. */
    unsigned counts[5] = {0};
    CHECK(js_rt_audio_slot_test_snapshot(runtime, counts) == present);
    CHECK(runtime->game_audio == NULL);
}

static void evaluate(BrowserEngine *engine, ScriptRuntime *runtime,
                     const char *source, const char *expected)
{
    ScriptResult result = {0};
    bool okay = script_runtime_evaluate_diagnostic(
        runtime, source, "<audio-selection-test>", &result);
    if (!okay) fprintf(stderr, "audio selection script: %s\n", result.error);
    CHECK(okay);
    CHECK(strcmp(result.summary, expected) == 0);
    CHECK(browser_engine_navigation(engine)->page.runtime == runtime);
}

static void install_audio(BrowserEngine *engine, ScriptRuntime *runtime)
{
    evaluate(engine, runtime,
        "if(typeof selectedContext!=='undefined')throw new Error('old realm');"
        "globalThis.selectedContext=new AudioContext();"
        "globalThis.pocSummary=selectedContext.state", "suspended");
}

static void shutdown_engine(BrowserEngine *engine)
{
    Budget *budget = browser_engine_budget(engine);
    CHECK(budget != NULL);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics metrics = {0};
    CHECK(browser_engine_metrics(engine, &metrics));
    CHECK(metrics.budget_current == 0u);
    CHECK(metrics.budget_active_allocations == 0u);
    CHECK(budget->external_reserved == 0u);
    browser_engine_destroy(engine);
}

static void selection_lane(int selection)
{
    bool native = selection != 0;
    BrowserConfig config;
    BrowserEngine *engine = create_engine(&config, selection);
    /* The engine copies trusted configuration. Mutating the caller's copy
       cannot change either its first realm or a subsequent navigation. */
    config.javascript.game_audio_internal_slots = !native;
    static const char *const urls[] = {
        "https://audio-selection.test/first",
        "https://audio-selection.test/second"
    };
    for (unsigned page = 0; page < 2u; page++) {
        ScriptRuntime *runtime = commit_page(engine, urls[page]);
        CHECK(browser_engine_navigation(engine)->game_audio_internal_slots == native);
        check_status(engine, runtime, native, false, false, false);
        check_status(engine, runtime, native, false, false, false);
        install_audio(engine, runtime);
        check_status(engine, runtime, native, true, native, false);
        if (native) {
            unsigned counts[5] = {0};
            CHECK(js_rt_audio_slot_test_snapshot(runtime, counts));
            CHECK(counts[0] == 1u && counts[1] == 1u);
            CHECK(counts[2] == 0u && counts[3] == 0u && counts[4] == 0u);
        }
        /* Leave the suspended context owned by this page. Navigation and
           shutdown must retire it through the ordinary lifecycle. */
    }
    shutdown_engine(engine);
}

static void failed_installation_status(void)
{
    BrowserConfig config;
    BrowserEngine *engine = create_engine(&config, true);
    ScriptRuntime *runtime = commit_page(engine, "https://audio-selection.test/refused");
    check_status(engine, runtime, true, false, false, false);
    tilefinch_test_faults()->audio_slot_install_fail_at = 1;
    evaluate(engine, runtime,
        "try{new AudioContext();globalThis.pocSummary='UNEXPECTED'}"
        "catch(error){globalThis.pocSummary='REFUSED'}", "REFUSED");
    CHECK(tilefinch_test_faults()->audio_slot_install_fail_at == 0u);
    /* The arena exists but initialization failed. Do not certify installation
       merely because native storage was admitted before the refusal. */
    check_status(engine, runtime, true, false, true, true);
    unsigned counts[5] = {0};
    CHECK(js_rt_audio_slot_test_snapshot(runtime, counts));
    for (unsigned i = 0; i < 5u; i++) CHECK(counts[i] == 0u);
    runtime = commit_page(engine, "https://audio-selection.test/retry");
    check_status(engine, runtime, true, false, false, false);
    install_audio(engine, runtime);
    check_status(engine, runtime, true, true, true, false);
    shutdown_engine(engine);
}

int main(void)
{
    ScriptGameAudioStatus absent = {true, true, true, true};
    CHECK(!script_runtime_game_audio_status(NULL, &absent));
    CHECK(!absent.selected_native && !absent.lazy_installed
          && !absent.native_present && !absent.failed);
    CHECK(!script_runtime_game_audio_status(NULL, NULL));
    script_runtime_configure_deterministic_replay(true, 17);
    selection_lane(-1);
    selection_lane(0);
    selection_lane(1);
    failed_installation_status();
    script_runtime_configure_deterministic_replay(false, 0);
    puts("game audio selection: default/off/on, fresh navigation, lazy/failure "
         "status and zero-owned shutdown passed");
    return 0;
}
