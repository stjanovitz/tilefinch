/* A pending load's phase (browser_navigation_job_phase), which the PSP
   loading status names ("RECEIVING PAGE  O CANCEL"): from its metrics
   alone, and over replayed loads from tests/fixtures/http-navigation-phase,
   one page with inline scripts and one without. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/fetch.h"
#include "tilefinch/navigation.h"

#include <stdio.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)
#define FIXTURE TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-navigation-phase"
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "LOADING-PHASE CHECK failed at %s:%d: %s\n",        \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static int test_from_metrics(void)
{
    BrowserNavigationJobMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));
    CHECK(browser_navigation_job_phase(NULL) == BROWSER_NAVIGATION_PHASE_CONNECTING);
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_CONNECTING);
    metrics.load.stream.headers_delivered = true;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_RECEIVING);
    metrics.load.body_bytes = 400u;
    metrics.completion_per_mille = 280u;
    /* An inline script while the document still arrives: receiving. */
    metrics.load.scripts_evaluated = 1u;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_RECEIVING);
    /* The last byte is in and the parser works through its scripts. */
    metrics.load.response_received = true;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_RUNNING_SCRIPTS);
    /* Past the network share: scripts running, or the rest of the load. */
    metrics.completion_per_mille = 760u;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_RUNNING_SCRIPTS);
    metrics.load.scripts_evaluated = 0u;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_FINISHING);
    /* All bytes in without scripts and before finalizing: still receiving
       (the parser has the rest of the document to read). */
    metrics.completion_per_mille = 690u;
    CHECK(browser_navigation_job_phase(&metrics) == BROWSER_NAVIGATION_PHASE_RECEIVING);
    return 0;
}

static BrowserEngine *create_engine(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 32u * MIB;
    config.javascript.enabled = true;
    config.javascript.document_scripts_enabled = true;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine creation failed: %s\n", error);
    return engine;
}

/* The phases one load passes through, in order, each once. */
static int replay_phases(BrowserEngine *engine, const char *url,
                         unsigned *seen, unsigned capacity, unsigned *count)
{
    *count = 0;
    CHECK(browser_engine_begin_navigation_url(engine, url, MIB, 5000, true));
    BrowserNavigationJobStatus status = browser_engine_navigation_status(engine);
    for (unsigned pump = 0;
         pump < 8192 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++) {
        BrowserNavigationJobMetrics metrics;
        CHECK(browser_engine_navigation_job_metrics(engine, &metrics));
        unsigned phase = (unsigned) browser_navigation_job_phase(&metrics);
        if (*count == 0 || seen[*count - 1u] != phase) {
            CHECK(*count < capacity);
            seen[(*count)++] = phase;
        }
        status = browser_engine_pump_navigation(engine, NULL);
    }
    if (status != BROWSER_NAVIGATION_JOB_SUCCEEDED)
        fprintf(stderr, "%s: status=%d error=\"%s\"\n", url, (int) status,
                browser_engine_last_error(engine));
    CHECK(status == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    fprintf(stderr, "%s phases:", url);
    for (unsigned at = 0; at < *count; at++) fprintf(stderr, " %u", seen[at]);
    fputc('\n', stderr);
    return 0;
}

static bool phase_seen(const unsigned *seen, unsigned count,
                       BrowserNavigationPhase phase)
{
    for (unsigned at = 0; at < count; at++)
        if (seen[at] == (unsigned) phase) return true;
    return false;
}

static int test_replayed_loads(void)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin_response_keyed(FIXTURE, error, sizeof(error)))
        fprintf(stderr, "replay: %s\n", error);
    CHECK(error[0] == '\0');
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    unsigned seen[16];
    unsigned count = 0;
    CHECK(replay_phases(engine, "https://phase.test/scripted", seen, 16u,
                        &count) == 0);
    /* No status says scripts run before the first byte, and the network
       phases never come back. */
    CHECK(count >= 3u);
    CHECK(seen[0] == BROWSER_NAVIGATION_PHASE_CONNECTING);
    CHECK(seen[1] == BROWSER_NAVIGATION_PHASE_RECEIVING);
    CHECK(phase_seen(seen, count, BROWSER_NAVIGATION_PHASE_RUNNING_SCRIPTS));
    for (unsigned at = 2u; at < count; at++)
        CHECK(seen[at] != BROWSER_NAVIGATION_PHASE_CONNECTING
              && seen[at] != BROWSER_NAVIGATION_PHASE_RECEIVING);
    CHECK(replay_phases(engine, "https://phase.test/plain", seen, 16u,
                        &count) == 0);
    /* The replay hands the whole body over at once, which the parser
       reads in the same pump: receiving may not be seen. */
    CHECK(count >= 2u);
    CHECK(seen[0] == BROWSER_NAVIGATION_PHASE_CONNECTING);
    CHECK(seen[count - 1u] == BROWSER_NAVIGATION_PHASE_FINISHING);
    CHECK(!phase_seen(seen, count, BROWSER_NAVIGATION_PHASE_RUNNING_SCRIPTS));
    browser_engine_destroy(engine);
    return 0;
}

int main(void)
{
    if (test_from_metrics() != 0) return 1;
    if (test_replayed_loads() != 0) return 1;
    puts("navigation loading phase: ok");
    return 0;
}
