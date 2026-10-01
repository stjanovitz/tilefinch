/* FinalizationRegistry cleanup under the hosts' own advance cadence.

   Wrapper cleanup runs as low-priority engine tasks inside
   script_runtime_advance (at most sixteen per advance, within the caller's
   task budget). No host gates its advance on pending author work: the PSP
   frame loop calls psp_advance_page_runtime every frame with a budget of
   two (pausing only for native menus and transient handoffs), and the lab
   and scripted boot loops tick with a budget of eight. An idle page with
   queued cleanups therefore drains without counting as pending author work,
   which would otherwise delay startup settle, boot-window return, and the
   degraded-page recovery clocks. A page whose due tasks fill every turn is
   different: two frame-rate intervals took both PSP slots forever and no
   cleanup ever ran, holding every collected wrapper's handle slot. These
   fixtures drive BrowserEngine as the PSP frame loop does and require the
   queue, and the handle slots it holds, to drain in both cases. */
#include "browser_engine_test_support.h"
#include "../src/js_runtime_internal.h"

static size_t cleanup_live_handle_slots(const ScriptRuntime *runtime)
{
    size_t live = 0;
    for (size_t slot = 0; slot < runtime->bridge.node_count; slot++)
        if (runtime->bridge.nodes[slot] != NULL) live++;
    return live;
}

static int cleanup_drain_journey(const char *page, size_t page_length,
                                 bool idle, unsigned garbage_nodes)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://cleanup.test/",
                                     page, page_length, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    ScriptRuntime *runtime = navigation->page.runtime;
    CHECK(runtime != NULL);
    for (unsigned tick = 0; tick < 8u; tick++)
        CHECK(browser_engine_advance_runtime(engine, 16u, 2u, NULL));
    size_t baseline = cleanup_live_handle_slots(runtime);
    /* Wrappers for detached nodes, half in self-cycles which only a
       collection clears. Nothing else is left to run. */
    char garbage_script[512];
    snprintf(garbage_script, sizeof(garbage_script),
        "(()=>{for(let i=0;i<%u;i++){const node=document.createElement('i');"
        "if(i&1)node.self=node;}})();"
        "globalThis.cleanupFinalizedBefore="
        "__tilefinchRetentionStats.wrappersFinalized;"
        "globalThis.cleanupTasksBefore=globalThis.tasks|0;"
        "globalThis.pocSummary='CLEANUP-GARBAGE'", garbage_nodes);
    CHECK(script_runtime_evaluate_diagnostic(runtime, garbage_script,
        "<cleanup-garbage>", &navigation->page.script_result));
    CHECK(strcmp(navigation->page.script_result.summary,
                 "CLEANUP-GARBAGE") == 0);
    size_t garbage = cleanup_live_handle_slots(runtime);
    CHECK(garbage >= baseline + garbage_nodes);
    (void) script_runtime_collect_and_trim(runtime);
    CHECK(JS_IsCleanupJobPending(runtime->runtime));
    /* Queued cleanup is not author work: it must not look like an
       unsettled startup to the recovery and boot-window heuristics. */
    CHECK(script_runtime_has_pending_author_work(runtime) == !idle);
    unsigned ticks = 0;
    for (; ticks < 4096u && JS_IsCleanupJobPending(runtime->runtime);
         ticks++)
        CHECK(browser_engine_advance_runtime(engine, 16u, 2u, NULL));
    size_t drained = cleanup_live_handle_slots(runtime);
    CHECK(script_runtime_evaluate_diagnostic(runtime,
        "globalThis.pocSummary=(__tilefinchRetentionStats.wrappersFinalized"
        "-cleanupFinalizedBefore)+' '+((globalThis.tasks|0)"
        "-cleanupTasksBefore)",
        "<cleanup-count>", &navigation->page.script_result));
    printf("cleanup-drain %s ticks=%u pending=%d slots=%zu->%zu->%zu "
           "finalized,tasks=%s\n", idle ? "idle" : "busy", ticks,
           JS_IsCleanupJobPending(runtime->runtime), baseline, garbage,
           drained, navigation->page.script_result.summary);
    CHECK(!JS_IsCleanupJobPending(runtime->runtime));
    unsigned finalized = 0, tasks = 0;
    CHECK(sscanf(navigation->page.script_result.summary, "%u %u",
                 &finalized, &tasks) == 2);
    CHECK(finalized >= garbage_nodes);
    /* Promotion takes at most one slot in every five busy turns. */
    CHECK(idle ? tasks == 0 : tasks >= ticks * 2u * 4u / 5u);
    CHECK(drained <= baseline + 8u);
    CHECK(script_runtime_has_pending_author_work(runtime) == !idle);
    browser_engine_destroy(engine);
    return 0;
}

int test_idle_page_drains_wrapper_cleanups(void)
{
    static const char idle[] =
        "<!doctype html><title>Idle</title><body><div id=root>Ready</div>";
    CHECK(cleanup_drain_journey(idle, sizeof(idle) - 1u, true, 1200u) == 0);
    /* Two frame-rate intervals are due on every PSP frame and would take
       both of its task slots forever. */
    static const char busy[] =
        "<!doctype html><title>Busy</title><body><div id=root>Ready</div>"
        "<script>var tasks=0;setInterval(()=>{tasks++},16);"
        "setInterval(()=>{tasks++},16);</script>";
    CHECK(cleanup_drain_journey(busy, sizeof(busy) - 1u, false, 300u) == 0);
    return 0;
}
