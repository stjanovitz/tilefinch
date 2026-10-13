#include "tilefinch/browser_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pressure check line=%d: %s\n", \
    __LINE__, #x); return 1; } } while (0)

static const char *const cases[] = {
    "navigation", "mutation", "reader", "basic", "css", "render", "history",
    "container-query", "resource-images", "dom-query-growth"
};

static const char first[] =
    "<!doctype html><title>Incumbent</title><a href=/next>Next</a>"
    "<input name=q value=retained><p>Old borrowed text</p>";
static const char recovery[] =
    "<!doctype html><title>Recovered</title><a href=/next>Recovered link</a>";
static const char dom_growth_script[] =
    "(()=>{const root=document.querySelector('article');"
    "for(let i=0;i<256;i++){const n=document.createElement('span');"
    "n.setAttribute('class','pressure-item');n.textContent='item '+i;"
    "root.appendChild(n);}"
    "const nodes=document.querySelectorAll('.pressure-item');"
    "globalThis.pocSummary='DOM-GROWTH '+nodes.length;})()";

static size_t make_page(char *page, size_t capacity)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><title>Pressure surface</title><style>"
        "body{margin:0}p{margin:2px}table{border-collapse:collapse}"
        "td{border:1px solid #222}a:focus{border:2px solid blue}</style>"
        "<form action=/search><input name=q value=retained><button>Search</button></form>"
        "<a href=/next>Next page</a><article><h1>A readable article</h1>");
    for (unsigned i = 0; i < 96; i++) {
        int n = snprintf(page + used, capacity - used,
            "<p>Paragraph %u contains enough ordinary text to select a reading "
            "surface, preserve useful content, and exercise wrapped text layout.</p>", i);
        if (n < 0 || (size_t) n >= capacity - used) return 0;
        used += (size_t) n;
    }
    int n = snprintf(page + used, capacity - used,
        "</article><table><tr><td>One</td><td>Two</td></tr></table>"
        "<svg width=48 height=48><circle cx=24 cy=24 r=22 fill=green/></svg>"
        "<script>setTimeout(()=>{document.querySelector('article').innerHTML="
        "'<h1>Updated article</h1><p>Replacement text with a new link.</p>"
        "<a href=/updated>Updated link</a>';"
        "document.body.style.padding='3px';},50);</script>");
    if (n < 0 || (size_t) n >= capacity - used) return 0;
    return used + (size_t) n;
}

static size_t make_container_page(char *page, size_t capacity)
{
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><title>Container pressure</title><style>"
        "article{container-type:inline-size;container-name:card;width:320px}"
        ".item{--ink:#246;text-shadow:1px 1px #333}"
        "@container card (width > 10px){.item{color:var(--ink)}}"
        "</style><article>");
    if (used >= capacity) return 0;
    for (unsigned i = 0; i < 280; i++) {
        int n = snprintf(page + used, capacity - used,
                         "<p class=item>Item %u</p>", i);
        if (n < 0 || (size_t) n >= capacity - used) return 0;
        used += (size_t) n;
    }
    int n = snprintf(page + used, capacity - used, "</article>");
    if (n < 0 || (size_t) n >= capacity - used) return 0;
    return used + (size_t) n;
}

static size_t make_image_page(char *page, size_t capacity)
{
    static const char png[] =
        "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB"
        "CAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
    size_t used = (size_t) snprintf(page, capacity,
        "<!doctype html><title>Image pressure</title><style>"
        "img,.tile{width:24px;height:24px;margin:3px}"
        ".tile{background-image:url('%s')}</style><main>", png);
    if (used >= capacity) return 0;
    for (unsigned i = 0; i < 24; i++) {
        int n = snprintf(page + used, capacity - used,
                         "<img alt='Image %u' src='%s'>", i, png);
        if (n < 0 || (size_t) n >= capacity - used) return 0;
        used += (size_t) n;
    }
    int n = snprintf(page + used, capacity - used,
        "<div class=tile></div><svg width=32 height=32>"
        "<circle cx=16 cy=16 r=12 fill=green></circle></svg></main>");
    if (n < 0 || (size_t) n >= capacity - used) return 0;
    return used + (size_t) n;
}

/* Every operation may refuse or retire its page. It must never leave a
   dangling graph, poison the next navigation, or hide a teardown leak. */
static int run_case(unsigned which, size_t failure, size_t headroom,
                    size_t *attempts)
{
    char page[32768], error[256] = {0};
    size_t length = which == 7
        ? make_container_page(page, sizeof(page))
        : which == 8 ? make_image_page(page, sizeof(page))
        : make_page(page, sizeof(page));
    CHECK(length != 0);
    BrowserConfig config;
    browser_config_init(&config, NULL);
    config.memory_limit = 16u * MIB;
    config.javascript.enabled = config.javascript.document_scripts_enabled =
        which != 8;
    config.resources.enabled = which == 8;
    BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
    CHECK(engine != NULL);
    CHECK(browser_engine_commit_html(engine, "https://pressure.test/first",
        first, sizeof(first)-1, true));
    if (which != 0 && which != 7 && which != 8)
        CHECK(browser_engine_commit_html(engine, "https://pressure.test/page",
            page, length, true));
    CHECK(browser_engine_render_frame(engine, NULL));
    Budget *budget = browser_engine_budget(engine);
    size_t limit = budget->limit;
    if (headroom != SIZE_MAX) {
        CHECK(headroom <= limit - budget->current);
        budget->limit = budget->current + headroom;
    }
    budget_inject_failure_after(budget, failure);
    switch (which) {
    case 0: (void) browser_engine_commit_html(engine, "https://pressure.test/page",
                page, length, true); break;
    case 1: {
        bool changed = false;
        (void) browser_engine_advance_runtime(engine, 100, 16, &changed);
        break;
    }
    case 2: {
        ReaderDocumentAnalysis analysis;
        (void) browser_engine_prepare_reader(engine, &analysis);
        break;
    }
    case 3: {
        ReaderDocumentAnalysis analysis;
        (void) browser_engine_prepare_basic_view(engine, &analysis);
        break;
    }
    case 4: {
        static const char css[] = "body{font-size:18px;padding:7px}p{color:#246}";
        (void) browser_engine_apply_user_css(engine, css, sizeof(css)-1);
        break;
    }
    case 5:
        (void) browser_engine_scroll_by(engine, 700);
        (void) browser_engine_render_frame(engine, NULL);
        break;
    case 6: (void) browser_engine_history_move(engine, false); break;
    case 7: (void) browser_engine_commit_html(engine,
                "https://pressure.test/container", page, length, true); break;
    case 8: {
        (void) browser_engine_commit_html(engine,
                "https://pressure.test/images", page, length, true);
        for (unsigned tick = 0; tick < 16; tick++) {
            bool changed = false;
            (void) browser_engine_run_idle_work(engine, &changed);
        }
        (void) browser_engine_scroll_by(engine, 480);
        for (unsigned tick = 0; tick < 8; tick++) {
            bool changed = false;
            (void) browser_engine_run_idle_work(engine, &changed);
        }
        (void) browser_engine_render_frame(engine, NULL);
        if (failure == SIZE_MAX && headroom == SIZE_MAX) {
            NavigationSession *navigation = browser_engine_navigation(engine);
            bool decoded = false;
            for (size_t i = 0; i < navigation->page.images.count; i++) {
                if (navigation->page.images.items[i].pixels != NULL)
                    decoded = true;
            }
            CHECK(decoded);
        }
        break;
    }
    case 9: {
        NavigationSession *navigation = browser_engine_navigation(engine);
        if (navigation->page.runtime != NULL) {
            bool evaluated = script_runtime_evaluate_diagnostic(
                navigation->page.runtime, dom_growth_script,
                "<pressure-dom-growth>", &navigation->page.script_result);
            if (failure == SIZE_MAX && headroom == SIZE_MAX) {
                CHECK(evaluated);
                CHECK(strcmp(navigation->page.script_result.summary,
                             "DOM-GROWTH 256") == 0);
            }
        }
        (void) browser_engine_advance_runtime(engine, 100, 16, NULL);
        (void) browser_engine_render_frame(engine, NULL);
        break;
    }
    default: CHECK(false);
    }
    if (attempts != NULL) {
        CHECK(failure == SIZE_MAX && budget->failure_injection_enabled);
        *attempts = SIZE_MAX - budget->failure_injection_countdown;
    }
    budget_clear_failure_injection(budget);
    budget->limit = limit;
    NavigationSession *navigation = browser_engine_navigation(engine);
    if (navigation->page.loaded) {
        CHECK(navigation->page.document.html != NULL);
        (void) browser_engine_render_frame(engine, NULL);
        (void) browser_engine_focus_move(engine, true);
        bool changed = false;
        (void) browser_engine_run_idle_work(engine, &changed);
    }
    CHECK(browser_engine_commit_html(engine, "https://pressure.test/recovery",
        recovery, sizeof(recovery)-1, true));
    CHECK(browser_engine_render_frame(engine, NULL)
        && browser_engine_focus_move(engine, true));
    bool stopped = browser_engine_shutdown(engine);
    if (!stopped) {
        fprintf(stderr, "pressure shutdown: %s\n", browser_engine_last_error(engine));
        budget_dump_active(budget, stderr, 16);
    }
    CHECK(stopped);
    CHECK(budget->current == 0 && budget->accounting_repair_count == 0
        && budget_active_allocations(budget, NULL) == 0);
    browser_engine_destroy(engine);
    return 0;
}

static int run_initial_limits(void)
{
    static const size_t limits[] = {64*1024, 128*1024, 256*1024, 512*1024,
                                    MIB, 2*MIB, 4*MIB, 8*MIB, 16*MIB};
    for (unsigned heap = 0; heap < 2; heap++) {
        for (size_t i = 0; i < sizeof(limits)/sizeof(limits[0]); i++) {
            BrowserConfig config;
            browser_config_init(&config, NULL);
            config.memory_limit = heap ? 16u*MIB : limits[i];
            config.javascript.enabled = config.javascript.document_scripts_enabled = true;
            if (heap) config.javascript.heap_limit = limits[i];
            char error[256] = {0};
            fprintf(stderr, "pressure initialization heap=%u limit=%zu\n", heap, limits[i]);
            BrowserEngine *engine = browser_engine_create(&config, error, sizeof(error));
            if (engine == NULL) { CHECK(error[0] != '\0'); continue; }
            (void) browser_engine_commit_html(engine, "https://pressure.test/initial",
                first, sizeof(first)-1, true);
            if (browser_engine_navigation(engine)->page.loaded)
                (void) browser_engine_render_frame(engine, NULL);
            Budget *budget = browser_engine_budget(engine);
            CHECK(browser_engine_shutdown(engine));
            CHECK(budget->current == 0 && budget->accounting_repair_count == 0);
            browser_engine_destroy(engine);
        }
    }
    return 0;
}

static int run_fragment_refusals(void)
{
    static const char html[] = "<!doctype html><body><p>Incumbent</p></body>";
    static const char fragment[] = "<p>Replacement</p><a href=/next>Next</a>";
    for (size_t point = 0; point < 96; point++) {
        Budget budget;
        budget_init(&budget, 4u*MIB);
        CHECK(budget_install_lexbor(&budget));
        PocDocument document = {0};
        CHECK(document_parse(&document, &budget, html, sizeof(html)-1, 4096));
        lxb_dom_node_t *body = document_body_node(&document);
        fprintf(stderr, "pressure fragment allocation=%zu\n", point);
        budget_inject_failure_after(&budget, point);
        (void) document_set_element_inner_html(&document, body,
                                               fragment, sizeof(fragment)-1);
        budget_clear_failure_injection(&budget);
        CHECK(document_set_element_inner_html(&document, body,
                                              fragment, sizeof(fragment)-1));
        CHECK(document_refresh(&document));
        document_destroy(&document);
        CHECK(budget.current == 0 && budget.accounting_repair_count == 0);
        CHECK(budget_uninstall_lexbor(&budget));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3) {
        unsigned which = (unsigned) strtoul(argv[1], NULL, 10);
        size_t failure = (size_t) strtoull(argv[2], NULL, 10);
        if (which >= sizeof(cases)/sizeof(cases[0])) return 2;
        return run_case(which, failure, SIZE_MAX, NULL);
    }
    unsigned begin = 0, end = (unsigned)(sizeof(cases)/sizeof(cases[0]));
    if (argc == 2) {
        char *tail = NULL;
        unsigned long selected = strtoul(argv[1], &tail, 10);
        CHECK(tail != argv[1] && *tail == '\0' && selected < end);
        begin = (unsigned) selected;
        end = begin + 1;
    } else {
        CHECK(argc == 1);
        CHECK(run_initial_limits() == 0);
        CHECK(run_fragment_refusals() == 0);
    }
    size_t total = 0;
    for (unsigned which = begin; which < end; which++) {
        size_t attempts = 0;
        CHECK(run_case(which, SIZE_MAX, SIZE_MAX, &attempts) == 0);
        CHECK(attempts < 4096);
        for (size_t i = 0; i <= attempts; i++) {
            fprintf(stderr, "pressure boundary case=%s allocation=%zu/%zu\n",
                    cases[which], i, attempts);
            CHECK(run_case(which, i, SIZE_MAX, NULL) == 0);
            total++;
        }
        static const size_t rooms[] = {0, 4096, 65536, 256*1024, MIB};
        for (size_t i = 0; i < sizeof(rooms)/sizeof(rooms[0]); i++) {
            fprintf(stderr, "pressure capacity case=%s headroom=%zu\n",
                    cases[which], rooms[i]);
            CHECK(run_case(which, SIZE_MAX, rooms[i], NULL) == 0);
            total++;
        }
    }
    printf("memory-pressure cases=%zu teardown=zero recovery=usable\n", total);
    return 0;
}
