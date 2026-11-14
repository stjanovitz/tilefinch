/* A failed load says what it ran into (browser_engine_navigation_failure_code)
   from typed facts, not from the engine's sentence: a transport timeout, no
   response, a document over the load's byte limit, an allocation the page
   budget refused. The PSP failure page and its toast show that code's
   catalog headline and next step (psp_ui_failure_headline). Replayed
   responses from tests/fixtures/http-navigation-failure. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/budget.h"
#include "tilefinch/diagnostics.h"
#include "tilefinch/fetch.h"
#include "tilefinch/navigation.h"
#include "tilefinch/psp_ui.h"

#include <stdio.h>
#include <string.h>

#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif

#define MIB (1024u * 1024u)
#define FIXTURE TILEFINCH_TEST_SOURCE_DIR "/tests/fixtures/http-navigation-failure"
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "FAILURE-CODE CHECK failed at %s:%d: %s\n",         \
                __FILE__, __LINE__, #condition);                           \
        return 1;                                                          \
    }                                                                      \
} while (0)

static BrowserEngine *create_engine(void)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 24u * MIB;
    config.javascript.enabled = false;
    config.javascript.document_scripts_enabled = false;
    config.resources.enabled = false;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine creation failed: %s\n", error);
    return engine;
}

static BrowserNavigationJobStatus run(BrowserEngine *engine, const char *url,
                                      size_t maximum_bytes)
{
    if (!browser_engine_begin_navigation_url(engine, url, maximum_bytes, 5000,
                                             true))
        return BROWSER_NAVIGATION_JOB_FAILED;
    BrowserNavigationJobStatus status = browser_engine_navigation_status(engine);
    for (unsigned pump = 0;
         pump < 4096 && status == BROWSER_NAVIGATION_JOB_PENDING; pump++)
        status = browser_engine_pump_navigation(engine, NULL);
    return status;
}

static int expect(BrowserEngine *engine, const char *url, size_t maximum_bytes,
                  TilefinchDiagnosticCode code)
{
    BrowserNavigationJobStatus status = run(engine, url, maximum_bytes);
    TilefinchDiagnosticCode found = browser_engine_navigation_failure_code(engine);
    if (status != BROWSER_NAVIGATION_JOB_FAILED || found != code)
        fprintf(stderr, "%s: status=%d code=%s (want %s) error=\"%s\"\n", url,
                (int) status, tilefinch_diagnostic_code_name(found),
                tilefinch_diagnostic_code_name(code),
                browser_engine_last_error(engine));
    CHECK(status == BROWSER_NAVIGATION_JOB_FAILED);
    CHECK(found == code);
    /* The sentence stays for the Diagnostic QR. */
    CHECK(browser_engine_last_error(engine)[0] != '\0');
    return 0;
}

static int test_engine_codes(void)
{
    char error[256] = {0};
    if (!fetch_trace_replay_begin_response_keyed(FIXTURE, error, sizeof(error)))
        fprintf(stderr, "replay: %s\n", error);
    CHECK(error[0] == '\0');
    BrowserEngine *engine = create_engine();
    CHECK(engine != NULL);
    CHECK(expect(engine, "https://failure.test/timeout", MIB,
                 TILEFINCH_DIAGNOSTIC_TIMEOUT) == 0);
    CHECK(expect(engine, "https://failure.test/offline", MIB,
                 TILEFINCH_DIAGNOSTIC_NETWORK_FAILED) == 0);
    CHECK(expect(engine, "https://failure.test/big", 4096,
                 TILEFINCH_DIAGNOSTIC_LIMIT_REACHED) == 0);
    /* The same page within its limit commits, and a commit clears it. */
    CHECK(run(engine, "https://failure.test/big", MIB)
          == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    CHECK(browser_engine_navigation_failure_code(engine) == TILEFINCH_DIAGNOSTIC_OK);
    /* The parser refusing the body after its headers arrived is no
       network failure, though the transfer never finished (no status). */
    navigation_test_refuse_next_parser_checkpoint(
        NAVIGATION_TEST_PARSER_FEED_HARD_FAILURE);
    CHECK(expect(engine, "https://failure.test/big", MIB,
                 TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED) == 0);
    CHECK(browser_engine_navigation(engine)->last_response_started);
    /* The incumbent page keeps running on the same Budget while the next
       load waits, and an optional allocation it makes there (a cache put, a
       decode) may be refused. That is not the load's: a site that cannot be
       reached still says so, not "not enough memory". */
    CHECK(run(engine, "https://failure.test/big", MIB)
          == BROWSER_NAVIGATION_JOB_SUCCEEDED);
    Budget *shared = browser_engine_budget(engine);
    CHECK(browser_engine_begin_navigation_url(
        engine, "https://failure.test/offline", MIB, 5000, true));
    size_t refusals = shared->failure_count;
    CHECK(budget_malloc(shared, shared->limit) == NULL);
    CHECK(shared->failure_count != refusals);
    BrowserNavigationJobStatus incumbent_status =
        browser_engine_navigation_status(engine);
    for (unsigned pump = 0; pump < 4096
         && incumbent_status == BROWSER_NAVIGATION_JOB_PENDING; pump++) {
        incumbent_status = browser_engine_pump_navigation(engine, NULL);
        /* Between the load's pumps, as the page's own turns run. */
        (void) budget_malloc(shared, shared->limit);
    }
    TilefinchDiagnosticCode incumbent_code =
        browser_engine_navigation_failure_code(engine);
    if (incumbent_code != TILEFINCH_DIAGNOSTIC_NETWORK_FAILED)
        fprintf(stderr, "incumbent refusal: code=%s (want network-failed)\n",
                tilefinch_diagnostic_code_name(incumbent_code));
    CHECK(incumbent_status == BROWSER_NAVIGATION_JOB_FAILED);
    CHECK(incumbent_code == TILEFINCH_DIAGNOSTIC_NETWORK_FAILED);
    /* Too little memory to start the transfer (its response buffers), and
       then a page whose DOM does not fit: both are memory, not network,
       although the first never got a response. */
    Budget *budget = browser_engine_budget(engine);
    size_t limit = budget->limit;
    budget->limit = budget->current + 512u * 1024u;
    int result = expect(engine, "https://failure.test/heavy", MIB,
                        TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED);
    budget->limit = limit;
    CHECK(result == 0);
    budget->limit = budget->current + 2u * MIB + 256u * 1024u;
    result = expect(engine, "https://failure.test/heavy", MIB,
                    TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED);
    bool responded = browser_engine_navigation(engine)->last_response_started;
    budget->limit = limit;
    CHECK(result == 0);
    CHECK(responded);
    browser_engine_destroy(engine);
    return 0;
}

/* Every code a failure can carry has a headline and a next step, and both
   fit the failure page's status as two lines. */
static int test_headlines(void)
{
    const char *seen[16] = {0};
    size_t seen_count = 0;
    for (unsigned code = TILEFINCH_DIAGNOSTIC_LIFECYCLE;
         code <= TILEFINCH_DIAGNOSTIC_INTERNAL_FAILED; code++) {
        const char *headline = psp_ui_failure_headline(code);
        const char *step = psp_ui_failure_next_step(code);
        CHECK(headline != NULL && headline[0] != '\0' && strchr(headline, '\n') == NULL);
        CHECK(step != NULL && step[0] != '\0' && strchr(step, '\n') == NULL);
        CHECK(strlen(headline) + 1u + strlen(step) < PSP_UI_STATUS_CAPACITY);
        bool known = false;
        for (size_t at = 0; at < seen_count; at++)
            known = known || strcmp(seen[at], headline) == 0;
        if (!known) seen[seen_count++] = headline;
    }
    /* Memory, size, timeout, network, unreadable, scripts, drawing and
       the rest. */
    CHECK(seen_count == 8u);
    CHECK(strcmp(psp_ui_failure_headline(TILEFINCH_DIAGNOSTIC_ALLOCATION_FAILED),
                 psp_ui_failure_headline(TILEFINCH_DIAGNOSTIC_NETWORK_FAILED)) != 0);
    return 0;
}

/* The failure page draws the two lines apart: the next step on its own
   line under the headline. Pages that differ only in the step differ
   below the rows where pages that differ only in the headline do. */
static int diff_rows(const uint16_t *a, const uint16_t *b, int width,
                     int height, int *top, int *bottom)
{
    *top = height;
    *bottom = -1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (a[y * width + x] == b[y * width + x]) continue;
            if (y < *top) *top = y;
            *bottom = y;
            break;
        }
    }
    return *bottom >= 0;
}

static int test_failure_page_lines(void)
{
    enum { WIDTH = 480, HEIGHT = 272 };
    static uint16_t frames[3][WIDTH * HEIGHT];
    static const char *const details[3] = {
        "Could not reach the site\nCheck Wi-Fi, then Retry",
        "Could not reach the site\nRetry, or try again later",
        "The page could not be read\nCheck Wi-Fi, then Retry"};
    for (size_t at = 0; at < 3u; at++) {
        PspUiState ui;
        psp_ui_init(&ui);
        psp_ui_set_page(&ui, "Example", "https://failure.test/", true);
        psp_ui_show_failure_recovery(&ui, details[at], 0u);
        CHECK(ui.screen == PSP_UI_SCREEN_FAILURE_RECOVERY);
        /* Past the sheet's entry motion. */
        for (unsigned frame = 0; frame < 60u; frame++)
            psp_ui_composite(&ui, frames[at], WIDTH, HEIGHT, WIDTH);
    }
    int step_top, step_bottom, headline_top, headline_bottom;
    CHECK(diff_rows(frames[0], frames[1], WIDTH, HEIGHT, &step_top,
                    &step_bottom));
    CHECK(diff_rows(frames[0], frames[2], WIDTH, HEIGHT, &headline_top,
                    &headline_bottom));
    if (step_top <= headline_bottom)
        fprintf(stderr, "failure page: headline rows %d-%d, step rows %d-%d\n",
                headline_top, headline_bottom, step_top, step_bottom);
    CHECK(step_top > headline_bottom);
    return 0;
}

int main(void)
{
    if (test_engine_codes() != 0) return 1;
    if (test_headlines() != 0) return 1;
    if (test_failure_page_lines() != 0) return 1;
    puts("navigation failure codes: ok");
    return 0;
}
