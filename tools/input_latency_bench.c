/*
 * Input latency micro-benchmark.
 *
 * Written to settle a specific question from a device report: d-pad focus
 * movement felt slow while shoulder-button scrolling felt fine, even though
 * both end in the same render path. The suspicion was that focus pays for a
 * DOM event dispatch through the JavaScript runtime and scrolling does not.
 *
 * It reports per-operation cost for focus and scroll on a link-dense page,
 * with the script runtime present and absent, so the dispatch cost is
 * isolated rather than inferred. Host timings are not device timings; the
 * ratio between the two columns is the number that transfers.
 */

#include "tilefinch/browser_engine.h"
#include "tilefinch/navigation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LINKS 200
#define ITERATIONS 400

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1.0e6;
}

/* A page shaped like the one that was slow: many links, deep enough nesting
   that a bubble walk has somewhere to go. */
static char *build_page(bool with_script, size_t *length)
{
    size_t capacity = 256u * 1024u;
    char *page = malloc(capacity);
    if (page == NULL) return NULL;
    int used = snprintf(
        page, capacity,
        "<!doctype html><title>Focus</title>"
        "<style>a{display:block;padding:2px}</style><body>%s"
        "<div><div><div><section><article>",
        with_script ? "<script>var k=1;k+=1;</script>" : "");
    for (int i = 0; i < LINKS && used > 0 && (size_t) used < capacity; i++) {
        used += snprintf(page + used, capacity - (size_t) used,
                         "<p><a href='https://example.test/%d'>link %d</a></p>",
                         i, i);
    }
    used += snprintf(page + used, capacity - (size_t) used,
                     "</article></section></div></div></div>");
    *length = (size_t) used;
    return page;
}

static bool measure(bool with_script, double *focus_us, double *scroll_us)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.javascript.enabled = with_script;
    char error[256] = {0};
    if (!browser_config_validate(&config, error, sizeof(error))) {
        fprintf(stderr, "config: %s\n", error);
        return false;
    }
    BrowserEngine *engine = browser_engine_create(
        &config, error, sizeof(error));
    if (engine == NULL) {
        fprintf(stderr, "engine: %s\n", error);
        return false;
    }
    size_t length = 0;
    char *page = build_page(with_script, &length);
    bool ok = page != NULL
        && browser_engine_commit_html(
               engine, "https://focus.test/", page, length, true);
    free(page);
    if (!ok) {
        fprintf(stderr, "commit failed: %s\n",
                browser_engine_last_error(engine));
        const NavigationSession *nav = browser_engine_navigation(engine);
        if (nav != NULL) {
            fprintf(stderr, "      script-result error=\"%s\" success=%d\n",
                    nav->page.script_result.error,
                    nav->page.script_result.success ? 1 : 0);
        }
        browser_engine_destroy(engine);
        return false;
    }

    /* Warm the first focus so allocation of the initial wrapper is not
       charged to the measured loop. */
    (void) browser_engine_focus_move(engine, true);

    /* A refusal still does the work that precedes it, and counting them
       separately keeps a silent early-out from flattering the average. */
    int refusals = 0;
    double start = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        if (!browser_engine_focus_move(engine, true)) refusals++;
    }
    if (refusals != 0) {
        fprintf(stderr, "note: %d/%d focus moves refused (javascript=%s)"
                " last-error=\"%s\"\n",
                refusals, ITERATIONS, with_script ? "on" : "off",
                browser_engine_last_error(engine));
    }
    *focus_us = (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    start = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        (void) browser_engine_scroll_by(engine, (i % 2) ? 16 : -16);
    }
    *scroll_us = (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    browser_engine_destroy(engine);
    return true;
}

int main(void)
{
    double focus_js = 0, scroll_js = 0, focus_plain = 0, scroll_plain = 0;
    if (!measure(true, &focus_js, &scroll_js)) return 1;
    if (!measure(false, &focus_plain, &scroll_plain)) return 1;

    printf("input-latency links=%d iterations=%d\n", LINKS, ITERATIONS);
    printf("  javascript=on   focus=%.1fus scroll=%.1fus ratio=%.1fx\n",
           focus_js, scroll_js,
           scroll_js > 0 ? focus_js / scroll_js : 0.0);
    printf("  javascript=off  focus=%.1fus scroll=%.1fus ratio=%.1fx\n",
           focus_plain, scroll_plain,
           scroll_plain > 0 ? focus_plain / scroll_plain : 0.0);
    printf("  dispatch cost per focus move = %.1fus (%.1fx the rest)\n",
           focus_js - focus_plain,
           focus_plain > 0 ? (focus_js - focus_plain) / focus_plain : 0.0);
    return 0;
}
