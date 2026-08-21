/*
 * Input latency micro-benchmark.
 *
 * Written to settle a specific question from a device report: d-pad focus
 * movement felt slow while shoulder-button scrolling felt fine, even though
 * both end in the same render path. The suspicion was that focus pays for a
 * DOM event dispatch through the JavaScript runtime and scrolling does not.
 *
 * It reports per-operation cost for focus, scroll, same-target pointer moves,
 * and pointer boundary crossings on a link-dense page, with the script
 * runtime present and absent. Host timings are not device timings; the ratio
 * between the two columns and the work shape are what transfer.
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
static char *build_page(unsigned script_mode, size_t *length)
{
    size_t capacity = 256u * 1024u;
    char *page = malloc(capacity);
    if (page == NULL) return NULL;
    int used = snprintf(
        page, capacity,
        "<!doctype html><title>Focus</title>"
        "<style>a{display:block;padding:2px}</style><body>%s"
        "<div><div><div><section><article>",
        script_mode == 2u
            ? "<script>var k=0;document.body.addEventListener('mousemove',"
              "function(){k++});document.body.addEventListener('mouseover',"
              "function(){k++})</script>"
            : (script_mode == 1u ? "<script>var k=1;k+=1</script>" : ""));
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

static bool measure(unsigned script_mode, double *focus_us, double *scroll_us,
                    double *pointer_same_us, double *pointer_cross_us)
{
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.javascript.enabled = script_mode != 0u;
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
    char *page = build_page(script_mode, &length);
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
                refusals, ITERATIONS, script_mode != 0u ? "on" : "off",
                browser_engine_last_error(engine));
    }
    *focus_us = (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    start = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        (void) browser_engine_scroll_by(engine, (i % 2) ? 16 : -16);
    }
    *scroll_us = (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    const NavigationSession *navigation = browser_engine_navigation(engine);
    const LayoutDocument *layout = navigation == NULL
        ? NULL : &navigation->page.layout;
    if (layout == NULL || layout->link_count < 2u) {
        fprintf(stderr, "pointer benchmark needs two retained links\n");
        browser_engine_destroy(engine);
        return false;
    }
    int points_x[2], points_y[2];
    for (size_t at = 0; at < 2u; at++) {
        const LinkRegion *link = &layout->links[at];
        points_x[at] = viewport_css_to_device(
            &layout->viewport, link->x + link->width / 2);
        points_y[at] = viewport_css_to_device(
            &layout->viewport, link->y + link->height / 2);
    }
    bool activate = false, changed = false;
    (void) browser_engine_pointer_event(
        engine, CONTROLLER_POINTER_MOVE, points_x[0], points_y[0],
        &activate, &changed);
    start = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        (void) browser_engine_pointer_event(
            engine, CONTROLLER_POINTER_MOVE,
            points_x[0] + (i & 1), points_y[0], &activate, &changed);
    }
    *pointer_same_us =
        (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    start = now_ms();
    for (int i = 0; i < ITERATIONS; i++) {
        unsigned point = (unsigned) i & 1u;
        (void) browser_engine_pointer_event(
            engine, CONTROLLER_POINTER_MOVE,
            points_x[point], points_y[point], &activate, &changed);
    }
    *pointer_cross_us =
        (now_ms() - start) * 1000.0 / (double) ITERATIONS;

    browser_engine_destroy(engine);
    return true;
}

int main(void)
{
    double focus_js = 0, scroll_js = 0, focus_plain = 0, scroll_plain = 0;
    double focus_idle_js = 0, scroll_idle_js = 0;
    double pointer_same_js = 0, pointer_cross_js = 0;
    double pointer_same_idle_js = 0, pointer_cross_idle_js = 0;
    double pointer_same_plain = 0, pointer_cross_plain = 0;
    if (!measure(2u, &focus_js, &scroll_js,
                 &pointer_same_js, &pointer_cross_js)) return 1;
    if (!measure(1u, &focus_idle_js, &scroll_idle_js,
                 &pointer_same_idle_js, &pointer_cross_idle_js)) return 1;
    if (!measure(0u, &focus_plain, &scroll_plain,
                 &pointer_same_plain, &pointer_cross_plain)) return 1;

    printf("input-latency links=%d iterations=%d\n", LINKS, ITERATIONS);
    printf("  javascript=on   focus=%.1fus scroll=%.1fus ratio=%.1fx\n",
           focus_js, scroll_js,
           scroll_js > 0 ? focus_js / scroll_js : 0.0);
    printf("  javascript=off  focus=%.1fus scroll=%.1fus ratio=%.1fx\n",
           focus_plain, scroll_plain,
           scroll_plain > 0 ? focus_plain / scroll_plain : 0.0);
    printf("  js/no-listener  focus=%.1fus scroll=%.1fus\n",
           focus_idle_js, scroll_idle_js);
    printf("  dispatch cost per focus move = %.1fus (%.1fx the rest)\n",
           focus_js - focus_plain,
           focus_plain > 0 ? (focus_js - focus_plain) / focus_plain : 0.0);
    printf("  javascript=on   pointer-same=%.1fus pointer-cross=%.1fus\n",
           pointer_same_js, pointer_cross_js);
    printf("  js/no-listener  pointer-same=%.1fus pointer-cross=%.1fus\n",
           pointer_same_idle_js, pointer_cross_idle_js);
    printf("  javascript=off  pointer-same=%.1fus pointer-cross=%.1fus\n",
           pointer_same_plain, pointer_cross_plain);
    return 0;
}
