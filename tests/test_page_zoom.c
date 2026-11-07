/* Page zoom: the viewport rule (viewport_apply_page_zoom), how it composes
 * with a page's own meta viewport, and a zoomed page's geometry through the
 * engine -- relayout in place, rollback on refusal, hit testing, focus,
 * scrolling, page-down, fixed layers and the next navigation. */
#include "tilefinch/browser_engine.h"
#include "tilefinch/budget.h"
#include "tilefinch/controller.h"
#include "tilefinch/navigation.h"
#include "tilefinch/render.h"
#include "../src/tilefinch_test_faults.h"
#include "tilefinch/viewport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                              \
    if (!(condition)) {                                                    \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                               \
        return 1;                                                          \
    }                                                                      \
} while (0)

static int test_steps_cycle_and_validate(void)
{
    static const unsigned steps[] = {50, 67, 75, 80, 90, 100, 110, 125, 150};
    CHECK(sizeof(steps) / sizeof(steps[0]) == VIEWPORT_PAGE_ZOOM_STEP_COUNT);
    for (size_t i = 0; i < VIEWPORT_PAGE_ZOOM_STEP_COUNT; i++) {
        CHECK(viewport_page_zoom_steps[i] == steps[i]
              && viewport_page_zoom_valid(steps[i]));
        size_t next = (i + 1u) % VIEWPORT_PAGE_ZOOM_STEP_COUNT;
        size_t previous = (i + VIEWPORT_PAGE_ZOOM_STEP_COUNT - 1u)
                          % VIEWPORT_PAGE_ZOOM_STEP_COUNT;
        CHECK(viewport_page_zoom_step(steps[i], 1) == steps[next]
              && viewport_page_zoom_step(steps[i], -1) == steps[previous]
              && viewport_page_zoom_step(steps[i], 0) == steps[i]);
    }
    CHECK(!viewport_page_zoom_valid(0) && !viewport_page_zoom_valid(66)
          && !viewport_page_zoom_valid(200));
    /* An unknown stored value steps from the default. */
    CHECK(viewport_page_zoom_step(66, 1) == 110
          && viewport_page_zoom_step(66, -1) == 90);
    return 0;
}

static int check_zoomed(int css_width, unsigned percent, int expected_width)
{
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, css_width,
                                (272 * css_width + 479) / 480, 480, 272));
    if (css_width == 480) {
        viewport.scale_numerator = 1;
        viewport.scale_denominator = 1;
    }
    ViewportContext before = viewport;
    CHECK(viewport_apply_page_zoom(&viewport, percent));
    if (expected_width == css_width) {
        CHECK(memcmp(&viewport, &before, sizeof(viewport)) == 0);
        return 0;
    }
    CHECK(viewport.css_width == expected_width
          && viewport.css_height == (272 * expected_width + 479) / 480
          && viewport.device_width == 480 && viewport.device_height == 272
          && viewport.scale_numerator == 480
          && viewport.scale_denominator == expected_width
          && viewport_context_is_scaled(&viewport));
    /* The zoomed screen maps back onto the whole device. */
    CHECK(viewport_css_to_device(&viewport, viewport.css_width) == 480);
    return 0;
}

static int test_viewport_math(void)
{
    /* A page that follows the device width takes the zoomed screen. */
    static const struct { unsigned percent; int width; } device[] = {
        {50, 960}, {67, 716}, {75, 640}, {80, 600}, {90, 533},
        {100, 480}, {110, 436}, {125, 384}, {150, 320}
    };
    for (size_t i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        CHECK(check_zoomed(480, device[i].percent, device[i].width) == 0);
    /* The legacy desktop width is already narrower than any zoom-out
       asks for: never shrunk a second time, and zooming in keeps it
       fitting the screen. */
    for (size_t i = 0; i < VIEWPORT_PAGE_ZOOM_STEP_COUNT; i++)
        CHECK(check_zoomed(980, viewport_page_zoom_steps[i], 980) == 0);
    /* A declared width widens only when the zoom asks for more. */
    CHECK(check_zoomed(800, 50, 960) == 0 && check_zoomed(800, 67, 800) == 0
          && check_zoomed(800, 75, 800) == 0
          && check_zoomed(800, 125, 800) == 0);
    /* A declared width narrower than the screen is enlarged already;
       zoom-out widens it, zoom-in leaves it. */
    CHECK(check_zoomed(320, 75, 640) == 0 && check_zoomed(320, 150, 320) == 0);
    ViewportContext viewport;
    CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
    ViewportContext before = viewport;
    CHECK(!viewport_apply_page_zoom(&viewport, 66)
          && !viewport_apply_page_zoom(NULL, 50)
          && memcmp(&viewport, &before, sizeof(viewport)) == 0);
    /* Device and CSS conversions stay inverse within a pixel at every
       step, which hit testing and scrolling depend on. */
    for (size_t i = 0; i < VIEWPORT_PAGE_ZOOM_STEP_COUNT; i++) {
        CHECK(viewport_context_init(&viewport, 480, 272, 480, 272));
        viewport.scale_numerator = viewport.scale_denominator = 1;
        CHECK(viewport_apply_page_zoom(&viewport, viewport_page_zoom_steps[i]));
        for (int device_y = 0; device_y < 272; device_y += 17) {
            int css = viewport_device_to_css(&viewport, device_y);
            int back = viewport_css_to_device(&viewport, css);
            CHECK(back <= device_y && device_y - back <= 2);
        }
    }
    return 0;
}

static BrowserEngine *zoom_engine_with_scripts(bool scripts)
{
    BrowserDeviceProfile profile;
    browser_device_profile_psp3000(&profile);
    BrowserConfig config;
    browser_config_init(&config, &profile);
    config.memory_limit = 16u * MIB;
    config.tile_capacity = 8u;
    config.resources.enabled = false;
    config.javascript.enabled = scripts;
    char error[256] = {0};
    BrowserEngine *engine = browser_engine_create(&config, error,
                                                  sizeof(error));
    if (engine == NULL) fprintf(stderr, "engine: %s\n", error);
    return engine;
}

static BrowserEngine *zoom_engine(void)
{
    return zoom_engine_with_scripts(false);
}

static int test_meta_viewport_composition(void)
{
    BrowserEngine *engine = zoom_engine();
    CHECK(engine != NULL);
    static const struct {
        const char *head;
        unsigned percent;
        int width;
    } cases[] = {
        {"<meta name=viewport content='width=device-width'>", 50, 960},
        {"<meta name=viewport content='width=device-width'>", 125, 384},
        {"<meta name=viewport content='initial-scale=1'>", 75, 640},
        {"<meta name=viewport content='initial-scale=1'>", 150, 320},
        {"<meta name=viewport content='width=800'>", 50, 960},
        {"<meta name=viewport content='width=800'>", 75, 800},
        {"<meta name=viewport content='width=800'>", 150, 800},
        {"<meta name=viewport content='width=480'>", 150, 480},
        {"<meta name=viewport content='width=480'>", 50, 960},
        {"<meta name=viewport content='width=1024'>", 50, 1024},
        {"", 50, 980},
        {"", 150, 980},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char html[256];
        int length = snprintf(html, sizeof(html),
                              "<!doctype html>%s<body>text</body>",
                              cases[i].head);
        CHECK(length > 0 && (size_t) length < sizeof(html));
        CHECK(browser_engine_set_page_zoom(engine, cases[i].percent));
        CHECK(browser_engine_commit_html(engine, "https://meta.test/", html,
                                         (size_t) length, true));
        int width = browser_engine_navigation(engine)->viewport.css_width;
        if (width != cases[i].width)
            fprintf(stderr, "case %zu: width %d, want %d\n", i, width,
                    cases[i].width);
        CHECK(width == cases[i].width);
    }
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static const char zoom_page[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<style>body{margin:0;background:#ffffff}"
    "#target{display:block;position:absolute;left:100px;top:40px;"
    "width:120px;height:30px;background:#0000aa;color:#0000aa}"
    "#fixed{position:fixed;left:0;bottom:0;width:100%;height:16px;"
    "background:#00aa00}#tail{height:3000px}</style><body>"
    "<a id=target href=/next>target</a><div id=tail></div>"
    "<div id=fixed></div></body>";

static uint16_t rgb565(unsigned rgb)
{
    return (uint16_t) ((((rgb >> 16) & 0xf8u) << 8)
                       | (((rgb >> 8) & 0xfcu) << 3) | ((rgb & 0xffu) >> 3));
}

/* The link's device rectangle is where the zoom puts its CSS box; a focus
   probe at its centre finds it and the rendered frame shows it there, with
   the fixed bar on the screen's last row. */
static int check_zoomed_geometry(BrowserEngine *engine, int css_width)
{
    NavigationSession *navigation = browser_engine_navigation(engine);
    const ViewportContext *viewport = &navigation->page.layout.viewport;
    CHECK(navigation->viewport.css_width == css_width
          && viewport->css_width == css_width
          && navigation->page.layout.link_count == 1u);
    CHECK(browser_engine_scroll_to_edge(engine, false));
    int left = viewport_css_to_device(viewport, 100);
    int top = viewport_css_to_device(viewport, 40);
    int right = viewport_css_to_device(viewport, 220);
    int bottom = viewport_css_to_device(viewport, 70);
    CHECK(browser_engine_focus_at(engine, (left + right) / 2,
                                  (top + bottom) / 2));
    BrowserController *controller = browser_engine_controller(engine);
    CHECK(controller->focus_kind == CONTROLLER_FOCUS_LINK);
    int fx = 0, fy = 0, fw = 0, fh = 0;
    CHECK(browser_engine_focus_indicator_rect(engine, &fx, &fy, &fw, &fh)
          && fx == 100 && fy == 40 && fw == 120 && fh == 30);
    CHECK(browser_engine_render_frame(engine, NULL));
    size_t pixels = 0;
    uint16_t *frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(frame != NULL && pixels == 480u * 272u);
    uint16_t blue = rgb565(0x0000aa);
    CHECK(frame[(size_t) ((top + bottom) / 2) * 480u
                + (size_t) ((left + right) / 2)] == blue
          && frame[(size_t) ((top + bottom) / 2) * 480u
                   + (size_t) (right + 2)] != blue
          && frame[271u * 480u + 240u] == rgb565(0x00aa00));
    /* Scrolling and page-down move by device pixels mapped to CSS. */
    CHECK(browser_engine_scroll_by(engine, 100));
    int scrolled = navigation_current(navigation)->scroll_y;
    CHECK(scrolled == viewport_device_to_css(viewport, 100));
    CHECK(browser_engine_scroll_page(engine, 1));
    int paged = navigation_current(navigation)->scroll_y;
    CHECK(paged > scrolled
          && paged - scrolled <= viewport->css_height
          && paged - scrolled >= viewport->css_height / 2);
    CHECK(browser_engine_render_frame(engine, NULL));
    frame = browser_engine_framebuffer(engine, &pixels);
    CHECK(frame[271u * 480u + 240u] == rgb565(0x00aa00));
    CHECK(browser_engine_scroll_to_edge(engine, true));
    CHECK(navigation_current(navigation)->scroll_y
          == viewport_max_scroll_css(viewport,
                                     navigation->page.layout.height));
    return 0;
}

static int test_engine_applies_zoom_in_place(void)
{
    BrowserEngine *engine = zoom_engine();
    CHECK(engine != NULL);
    CHECK(browser_engine_page_zoom(engine) == 100u);
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/", zoom_page,
                                     sizeof(zoom_page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(!viewport_context_is_scaled(&navigation->viewport)
          && check_zoomed_geometry(engine, 480) == 0);
    static const char css[] = "p{color:#aa0000}";
    static const struct { unsigned percent; int width; } steps[] = {
        {50, 960}, {75, 640}, {125, 384}, {150, 320}, {90, 533}, {100, 480}
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        CHECK(browser_engine_apply_presentation(
            engine, css, sizeof(css) - 1u, steps[i].percent));
        CHECK(browser_engine_page_zoom(engine) == steps[i].percent
              && navigation->user_css_length == sizeof(css) - 1u
              && check_zoomed_geometry(engine, steps[i].width) == 0);
    }
    /* A refused render shell rolls the whole presentation back: viewport,
       zoom, sheet and owned memory. */
    CHECK(browser_engine_apply_presentation(engine, css, sizeof(css) - 1u,
                                            75u));
    Budget *budget = browser_engine_budget(engine);
    size_t owned = budget->current;
    ViewportContext before = navigation->viewport;
    browser_engine_test_refuse_next_render_shell_init();
    CHECK(!browser_engine_apply_presentation(engine, "", 0u, 50u));
    CHECK(budget->current == owned
          && memcmp(&navigation->viewport, &before, sizeof(before)) == 0
          && browser_engine_page_zoom(engine) == 75u
          && navigation->user_css_length == sizeof(css) - 1u
          && check_zoomed_geometry(engine, 640) == 0);
    CHECK(!browser_engine_apply_presentation(engine, css, sizeof(css) - 1u,
                                             66u)
          && browser_engine_page_zoom(engine) == 75u);
    /* The next page is resolved at the stored zoom. */
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/next",
                                     zoom_page, sizeof(zoom_page) - 1u,
                                     true));
    navigation = browser_engine_navigation(engine);
    CHECK(check_zoomed_geometry(engine, 640) == 0);
    /* set_page_zoom alone leaves the open page as it is. */
    CHECK(browser_engine_set_page_zoom(engine, 50u)
          && !browser_engine_set_page_zoom(engine, 55u)
          && navigation->viewport.css_width == 640
          && browser_engine_page_zoom(engine) == 50u);
    CHECK(browser_engine_shutdown(engine));
    BrowserEngineMetrics final = {0};
    CHECK(browser_engine_metrics(engine, &final)
          && final.budget_current == 0u);
    browser_engine_destroy(engine);
    return 0;
}

static int test_engine_wide_viewport_is_not_shrunk_twice(void)
{
    BrowserEngine *engine = zoom_engine();
    CHECK(engine != NULL);
    static const char wide[] =
        "<!doctype html><meta name=viewport content='width=800'>"
        "<body style='margin:0'><p>A page laid out for a wider screen.</p>"
        "</body>";
    CHECK(browser_engine_commit_html(engine, "https://wide.test/", wide,
                                     sizeof(wide) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    CHECK(navigation->viewport.css_width == 800);
    size_t relayouts = navigation->performance.full_relayouts;
    /* Neither step changes the viewport: the zoom is only remembered. */
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 75u)
          && navigation->viewport.css_width == 800
          && navigation->viewport.scale_denominator == 800
          && browser_engine_page_zoom(engine) == 75u);
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 150u)
          && navigation->viewport.css_width == 800
          && browser_engine_page_zoom(engine) == 150u
          && navigation->performance.full_relayouts == relayouts);
    /* A different sheet still relays out. */
    CHECK(browser_engine_apply_presentation(engine, "p{color:red}", 12u, 150u)
          && navigation->performance.full_relayouts > relayouts
          && navigation->user_css_length == 12u);
    /* Asking again for the zoom it already has relays out, which callers
       use to pick up other changes. */
    relayouts = navigation->performance.full_relayouts;
    CHECK(browser_engine_apply_presentation(engine, "p{color:red}", 12u, 150u)
          && navigation->performance.full_relayouts > relayouts);
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 150u));
    relayouts = navigation->performance.full_relayouts;
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 50u)
          && navigation->viewport.css_width == 960
          && navigation->page.layout.viewport.css_width == 960
          && navigation->performance.full_relayouts > relayouts);
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 100u)
          && navigation->viewport.css_width == 800);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int script_expect(ScriptRuntime *runtime, const char *expression,
                         const char *expected)
{
    char source[2048];
    int length = snprintf(source, sizeof(source),
        "__tilefinchClipboardWrite(String((%s)))", expression);
    CHECK(length > 0 && (size_t) length < sizeof(source));
    ScriptResult result = {0};
    CHECK(script_runtime_evaluate_probe(runtime, source, "<zoom-test>", &result));
    if (strcmp(result.last_clipboard_text, expected))
        fprintf(stderr, "zoom script: got %s, expected %s\n",
                result.last_clipboard_text, expected);
    CHECK(!strcmp(result.last_clipboard_text, expected));
    return 0;
}

static int test_script_viewport_commit_and_rollback(void)
{
    BrowserEngine *engine = zoom_engine_with_scripts(true);
    CHECK(engine != NULL);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<p>Viewport</p><script>globalThis.edges=[];"
        "addEventListener('resize',()=>edges.push('window:'+innerWidth));"
        "visualViewport.addEventListener('resize',()=>"
        "edges.push('visual:'+visualViewport.width));</script>";
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/", html,
                                     sizeof(html) - 1u, true));
    ScriptRuntime *runtime = browser_engine_navigation(engine)->page.runtime;
    CHECK(runtime != NULL);
    CHECK(script_expect(runtime, "[innerWidth,visualViewport.width,edges.length]",
                         "480,480,0") == 0);
    CHECK(script_expect(runtime,
        "(globalThis.dispatchEvent=()=>{throw Error('replaced dispatch')},true)",
        "true") == 0);
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 50u));
    /* Geometry is synchronous, but author events must wait for a normal
       checkpoint after the render-shell transaction committed. */
    CHECK(script_expect(runtime, "[innerWidth,visualViewport.width,visualViewport.height,edges.length]",
                         "960,960,544,0") == 0);
    CHECK(script_expect(runtime, "(Promise.resolve().then(()=>edges.push('promise')),true)",
                         "true") == 0);
    bool changed = false;
    CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime, "edges.join(',')",
                         "promise,window:960,visual:960") == 0);
    browser_engine_test_refuse_next_render_shell_init();
    CHECK(!browser_engine_apply_presentation(engine, "", 0u, 75u));
    CHECK(script_expect(runtime, "[innerWidth,visualViewport.width,edges.length]",
                         "960,960,3") == 0);
    CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime, "edges.length", "3") == 0);
    /* Several committed choices before a turn coalesce to the latest
       viewport, without an unbounded resize queue. */
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 75u));
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 125u));
    CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime, "edges.join(',')",
                         "promise,window:960,visual:960,window:384,visual:384") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_zoom_preserves_external_styles(void)
{
    BrowserEngine *engine = zoom_engine();
    CHECK(engine != NULL);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<link rel=stylesheet href='main.css'><p id=target>Styled</p>";
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/", html,
                                     sizeof(html) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    navigation->external_resources_enabled = true;
    navigation->maximum_stylesheets = 4u;
    navigation->maximum_stylesheet_bytes = 8192u;
    navigation->maximum_stylesheet_file_bytes = 4096u;
    navigation->resource_timeout_ms = 1000;
    Budget *budget = browser_engine_budget(engine);
    StylesheetDocumentResources *resources = &navigation->page.stylesheet_resources;
    resources->budget = budget;
    char main_css[1600], imported_css[3100];
    snprintf(main_css, sizeof(main_css),
        "@import 'imp.css';#target{color:#112233}/*%01400d*/", 0);
    snprintf(imported_css, sizeof(imported_css),
        "#target{background-color:#445566}/*%03000d*/", 0);
    const char *urls[] = {"https://zoom.test/main.css", "https://zoom.test/imp.css"};
    const char *sheets[] = {main_css, imported_css};
    for (size_t i = 0; i < 2u; i++) {
        size_t length = strlen(sheets[i]);
        unsigned char *bytes = budget_malloc(budget, length);
        CHECK(bytes != NULL);
        memcpy(bytes, sheets[i], length);
        BrowserSharedBody *body = browser_shared_body_take(budget, bytes, length);
        CHECK(body != NULL);
        bool retained = stylesheet_document_resources_retain(
            resources, urls[i], urls[i], "no-referrer", body, length, false,
            TILEFINCH_CREDENTIALS_INCLUDE, NULL);
        browser_shared_body_release(body);
        CHECK(retained);
    }
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 100u));
    size_t rules = navigation->page.stylesheet.count;
    if (rules < 2u) fprintf(stderr,
        "style setup rules=%zu resources=%zu attempted=%zu loaded=%zu failed=%zu skipped=%zu base=%s url=%s\n",
        rules, navigation->page.stylesheet_resources.count,
        navigation->page.external_stylesheets.attempted,
        navigation->page.external_stylesheets.loaded,
        navigation->page.external_stylesheets.failed,
        navigation->page.external_stylesheets.skipped_limit,
        navigation->page.resource_base_url,navigation->page.document_url);
    CHECK(rules >= 2u);
    /* Byte admission and memory pressure use the same skipped-sheet path.
       A zoom must refuse that replacement, not publish fewer author rules. */
    navigation->maximum_stylesheet_bytes = 4096u;
    CHECK(!browser_engine_apply_presentation(engine, "", 0u, 50u));
    CHECK(navigation->page.stylesheet.count == rules
          && navigation->viewport.css_width == 480
          && browser_engine_page_zoom(engine) == 100u);
    navigation->maximum_stylesheet_bytes = 8192u;
    CHECK(browser_engine_apply_presentation(engine, "", 0u, 50u));
    CHECK(navigation->page.stylesheet.count == rules
          && navigation->viewport.css_width == 960);
    /* Changing the viewport may legitimately deactivate a media-qualified
       sheet. That is not an admission refusal and must not block zoom. */
    lxb_dom_node_t *link = lxb_dom_interface_node(
        navigation->page.document.html->head)->first_child;
    for (unsigned scanned = 0; link != NULL && link->local_name != LXB_TAG_LINK
         && scanned < 16u; scanned++) link = link->next;
    CHECK(link != NULL && link->local_name == LXB_TAG_LINK);
    static const char media[] = "(max-width:600px)";
    CHECK(lxb_dom_element_set_attribute(lxb_dom_interface_element(link),
        (const lxb_char_t *) "media", 5u,
        (const lxb_char_t *) media, sizeof(media) - 1u) != NULL);
    CHECK(browser_engine_apply_page_zoom(engine, 100u));
    CHECK(browser_engine_apply_page_zoom(engine, 50u)
          && navigation->viewport.css_width == 960
          && navigation->page.external_stylesheets.skipped_media == 1u);
    CHECK(browser_engine_apply_page_zoom(engine, 100u)
          && navigation->page.stylesheet.count == rules);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_zoom_noop_and_cancellation(void)
{
    BrowserEngine *engine = zoom_engine();
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/", zoom_page,
                                     sizeof(zoom_page) - 1u, true));
    NavigationSession *navigation = browser_engine_navigation(engine);
    size_t relayouts = navigation->performance.full_relayouts;
    uint64_t shell = browser_engine_render_shell_serial(engine);
    /* A debounced choice which returns to its incumbent value does no
       work; resource refresh remains a separate presentation operation. */
    CHECK(browser_engine_apply_page_zoom(engine, 100u));
    CHECK(navigation->performance.full_relayouts == relayouts
          && browser_engine_render_shell_serial(engine) == shell);
    tilefinch_test_faults()->cancel_next_layout_cooperate = true;
    CHECK(!browser_engine_apply_page_zoom(engine, 50u));
    CHECK(!tilefinch_test_faults()->cancel_next_layout_cooperate
          && navigation->viewport.css_width == 480
          && browser_engine_page_zoom(engine) == 100u
          && browser_engine_render_shell_serial(engine) == shell);
    CHECK(browser_engine_render_frame(engine, NULL));
    CHECK(browser_engine_apply_page_zoom(engine, 50u));
    relayouts = navigation->performance.full_relayouts;
    shell = browser_engine_render_shell_serial(engine);
    CHECK(browser_engine_apply_page_zoom(engine, 50u));
    CHECK(navigation->performance.full_relayouts == relayouts
          && browser_engine_render_shell_serial(engine) == shell);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_native_resize_delivery(void)
{
    BrowserEngine *engine = zoom_engine_with_scripts(true);
    CHECK(engine != NULL);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<p>Resize delivery</p><script>globalThis.edges=[];"
        "addEventListener('resize',e=>{edges.push('first:'+e.isTrusted);"
        "let p=Promise.resolve();for(let i=0;i<150;i++)p=p.then(()=>{});"
        "p.then(()=>edges.push('promise'));});"
        "onresize=()=>edges.push('property');"
        "addEventListener('resize',()=>edges.push('second'));"
        "visualViewport.addEventListener('resize',()=>{edges.push('visual-first');"
        "Promise.resolve().then(()=>edges.push('visual-promise'));});"
        "visualViewport.addEventListener('resize',()=>edges.push('visual-second'));"
        "visualViewport.onresize=()=>edges.push('visual-handler');"
        "globalThis.onceCount=0;visualViewport.addEventListener('resize',"
        "()=>onceCount++,{once:true});"
        "__tilefinchTrustedEvent=()=>{throw Error('replaced marker');};"
        "</script>";
    CHECK(browser_engine_commit_html(engine, "https://zoom.test/", html,
                                     sizeof(html) - 1u, true));
    ScriptRuntime *runtime = browser_engine_navigation(engine)->page.runtime;
    CHECK(browser_engine_apply_page_zoom(engine, 50u));
    bool changed = false;
    CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime,
        "!edges.includes('second')&&!edges.includes('visual-first')", "true") == 0);
    for (unsigned i = 0; i < 16u; i++)
        CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime, "edges.join(',')",
        "first:true,promise,property,second,visual-first,visual-promise,visual-second,visual-handler") == 0);
    CHECK(script_expect(runtime, "onceCount", "1") == 0);
    CHECK(browser_engine_apply_page_zoom(engine, 100u));
    for (unsigned i = 0; i < 16u; i++)
        CHECK(browser_engine_advance_runtime(engine, 16u, 8u, &changed));
    CHECK(script_expect(runtime,
        "[onceCount,edges.filter(x=>x==='property').length]", "1,2") == 0);
    CHECK(script_expect(runtime,
        "(()=>{let out=[];visualViewport.onscroll=()=>out.push('handler');"
        "visualViewport.addEventListener('scroll',()=>out.push('listener'),{once:true});"
        "visualViewport.dispatchEvent(new Event('scroll'));"
        "visualViewport.dispatchEvent(new Event('scroll'));return out.join(',')})()",
        "listener,handler,handler") == 0);
    CHECK(script_expect(runtime,
        "(visualViewport.onscroll=e=>e.preventDefault(),"
        "visualViewport.dispatchEvent(new Event('scroll',{cancelable:true})))",
        "false") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

static int test_diagnostic_viewport_scale(void)
{
    const char *old = getenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI");
    char *saved = old == NULL ? NULL : strdup(old);
    CHECK(old == NULL || saved != NULL);
    CHECK(setenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI", "1", 1) == 0);
    BrowserEngine *engine = zoom_engine_with_scripts(true);
    CHECK(engine != NULL);
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<script>globalThis.loadScale=visualViewport.scale;</script>";
    /* Realm creation follows document load rather than engine creation. */
    bool loaded = browser_engine_commit_html(engine, "https://zoom.test/", html,
                                             sizeof(html) - 1u, true);
    if (saved != NULL) {
        CHECK(setenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI", saved, 1) == 0);
        free(saved);
    } else CHECK(unsetenv("TILEFINCH_DIAGNOSTIC_MOBILE_SAFARI") == 0);
    CHECK(loaded);
    ScriptRuntime *runtime = browser_engine_navigation(engine)->page.runtime;
    CHECK(script_expect(runtime, "loadScale", "1") == 0);
    CHECK(browser_engine_apply_page_zoom(engine, 50u));
    CHECK(script_expect(runtime, "visualViewport.scale", "1") == 0);
    CHECK(browser_engine_apply_page_zoom(engine, 150u));
    CHECK(script_expect(runtime, "visualViewport.scale", "1") == 0);
    CHECK(browser_engine_shutdown(engine));
    browser_engine_destroy(engine);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--script-viewport"))
        return test_script_viewport_commit_and_rollback();
    if (argc == 2 && !strcmp(argv[1], "--style-preservation"))
        return test_zoom_preserves_external_styles();
    if (argc == 2 && !strcmp(argv[1], "--resize-delivery"))
        return test_native_resize_delivery();
    if (argc == 2 && !strcmp(argv[1], "--diagnostic-scale"))
        return test_diagnostic_viewport_scale();
    if (argc == 2 && !strcmp(argv[1], "--meta-viewport"))
        return test_meta_viewport_composition();
    if (test_steps_cycle_and_validate() != 0) return 1;
    if (test_viewport_math() != 0) return 1;
    if (test_meta_viewport_composition() != 0) return 1;
    if (test_engine_applies_zoom_in_place() != 0) return 1;
    if (test_engine_wide_viewport_is_not_shrunk_twice() != 0) return 1;
    if (test_script_viewport_commit_and_rollback() != 0) return 1;
    if (test_zoom_preserves_external_styles() != 0) return 1;
    if (test_zoom_noop_and_cancellation() != 0) return 1;
    if (test_native_resize_delivery() != 0) return 1;
    if (test_diagnostic_viewport_scale() != 0) return 1;
    puts("page zoom tests passed");
    return 0;
}
