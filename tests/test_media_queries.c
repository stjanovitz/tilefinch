#include <stdio.h>
#include <string.h>
#include "script_test_support.h"
#include "tilefinch/style.h"

static bool stylesheet_application(void)
{
    static const char html[] =
        "<!doctype html><html><head><style>html{font-size:32px}"
        "#control{color:#112233}"
        "@media (width: min(30em, 500px)){#control{color:#445566}}"
        "@media (min-width:calc(480px / 0)){#control{color:#ff0000}}"
        "@media (width:calc(30em)){.icon span+span:last-child{"
        "position:absolute!important;clip:rect(1px,1px,1px,1px);"
        "width:1px;height:1px;overflow:hidden}}"
        "</style></head><body><a id=control class=icon>"
        "<span></span><span>Label</span></a></body></html>";
    Budget budget; budget_init(&budget, 16u * 1024u * 1024u);
    PocDocument document = {0}; Stylesheet sheet = {0};
    bool parsed = budget_install_lexbor(&budget)
        && document_parse(&document, &budget, html, sizeof(html) - 1, 17);
    bool built = parsed && stylesheet_build(&sheet, &budget, &document, 480);
    lxb_dom_node_t *body = parsed ? lxb_dom_interface_node(document.html->body) : NULL;
    lxb_dom_node_t *control = body ? body->first_child : NULL;
    ComputedStyle style = {0}, label = {0};
    if (built && control && control->last_child) {
        style = style_for_node(&sheet, control, NULL);
        label = style_for_node(&sheet, control->last_child, &style);
    }
    bool ok = built && control && style.color == 0x445566
        && label.clip_rect_empty && label.hidden && label.width == 1 && label.height == 1;
    printf("stylesheet application/compact label %s color=%06x hidden=%d width=%d height=%d\n",
           ok ? "PASS" : "FAIL", style.color, label.hidden, label.width, label.height);
    if (built) stylesheet_destroy(&sheet);
    if (parsed) document_destroy(&document);
    return ok && budget.current == 0;
}

/* Repository-owned query reductions: stylesheet and JS use identical
   viewport inputs, independently of authored root font declarations. */
int main(void)
{
    static const struct { const char *query; bool expected; } cases[] = {
        {"(max-width: calc(1120px - 1px))", true},
        {"(width: calc(30em))", true},
        {"(width: min(500px, 30rem))", true},
        {"(width: max(470px, 30em))", true},
        {"(width: clamp(500px, 480px, 400px))", false},
        {"(width: clamp(400px, 480px, 500px))", true},
        {"(width < calc(480px + 0.5px))", true},
        {"(width >= calc(480px + 0.5px))", false},
        {"(calc(30em - 1px) < width <= max(480px, 400px))", true},
        {"((width >= 30em) and (height < 300px))", true},
        {"(width < 400px) or (width: calc(30em))", true},
        {"not ((width > 480px) or (height > 272px))", true},
        {"(max-width: calc(1px + 2))", false},
        {"(min-width: calc(480px / 0))", false},
        {"(max-width: calc(480px * 1px))", false},
        {"(max-width: 480garbage)", false},
        {"(max-width: 100%)", false},
        {"(max-width: calc(480px+1px))", false},
        {"(max-width: calc(480px - 1px)) trailing", false},
        {"(width: min(400px, 480px)), (width: 480px)", true},
        {"(resolution: 96dpi)", true},
        {"(resolution: 1dppx)", true},
        {"(min-resolution: 2dppx)", false},
        {"(resolution)", true},
        {"(min-resolution: 0dpi)", true},
        {"(max-resolution: 0dpi)", false},
        {"not (resolution: 0dpi)", true},
        {"not (min-resolution: -1dpi)", false},
        {"(color)", true},
        {"(color: 8)", true},
        {"(color: 4)", false},
        {"(min-color: 4)", true},
        {"(max-color: 4)", false},
        {"(monochrome)", false},
        {"(monochrome: 0)", true},
        {"(pointer: coarse) and (hover: none)", true},
        {"not (unrecognized-feature: value)", false},
        {"not (max-width: calc(1px + 2))", false},
        {"(orientation: landscape)", true},
        {"not (width: 400px) and (height: 272px)", false},
        {"(width:480px) and (height:272px) or (pointer:coarse)", false},
        {"not ((width:480px) and)", false},
        {"not (orientation:bogus)", false},
        {"not (width:480)", false},
        {"not (width:calc(480))", false},
        {"not (aspect-ratio:/1)", false},
        {"(width:60ch)", true},
        {"(width:calc(100vw))", true},
        {"not (width:min(480px)*calc(1))", false},
        {"not (width:(480px))", false},
        {"screen and (width:480px) or (height:272px)", false},
        {"screen and ((width:480px) or (height:100px))", true},
    };
    ScriptPageFixture fixture;
    if (!script_page_fixture_open(&fixture, "<script></script>", 0)) return 1;
    ScriptResult result = {0};
    ScriptRuntime *runtime = script_runtime_create_configured(
        &fixture.document, &fixture.budget, 12u * 1024u * 1024u, 1000,
        "https://queries.test/", &fixture.options, &result);
    Stylesheet sheet = {.viewport_width = 480, .viewport_height = 272};
    bool ok = runtime != NULL;
    for (size_t i = 0; runtime != NULL && i < sizeof(cases)/sizeof(cases[0]); i++) {
        bool css = stylesheet_media_matches(&sheet, cases[i].query,
                                             strlen(cases[i].query));
        char source[1024];
        snprintf(source, sizeof(source),
            "document.documentElement.style.fontSize='32px';"
            "globalThis.pocSummary=String(matchMedia('%s').matches)",
            cases[i].query);
        bool evaluated = script_runtime_evaluate_diagnostic(
            runtime, source, "media-query-parity", &result);
        bool js = evaluated && strcmp(result.summary, "true") == 0;
        bool passed = evaluated && css == cases[i].expected && js == css;
        printf("%s css=%d js=%d expected=%d %s\n", cases[i].query, css, js,
               cases[i].expected, passed ? "PASS" : "FAIL");
        ok &= passed;
    }
    if (runtime != NULL) {
        bool events = script_runtime_evaluate_diagnostic(runtime,
            "(()=>{const initial=innerWidth,q=matchMedia('(width >= calc(30em))');"
            "let count=0;q.onchange=e=>{if(e.matches)throw Error('event');count++};"
            "innerWidth=479;__tilefinchMediaRecheck();__tilefinchMediaRecheck();"
            "q.onchange=null;innerWidth=initial;__tilefinchMediaRecheck();"
            "try{__tilefinchMediaMatches=()=>false}catch{}"
            "globalThis.pocSummary=String(count===1&&q.matches&&"
            "matchMedia('(width:480px)').matches)})()",
            "query-events-intrinsics", &result)
            && strcmp(result.summary, "true") == 0;
        printf("events/native intrinsic %s\n", events ? "PASS" : "FAIL");
        ok &= events;
        script_runtime_set_viewport(runtime, 272, 480, 272, 480);
        sheet.viewport_width = 272; sheet.viewport_height = 480;
        bool portrait = stylesheet_media_matches(&sheet,
            "(orientation:portrait)", sizeof("(orientation:portrait)") - 1)
            && script_runtime_evaluate_diagnostic(runtime,
                "globalThis.pocSummary="
                "String(matchMedia('(orientation:portrait)').matches)",
                "portrait", &result) && strcmp(result.summary, "true") == 0;
        printf("portrait %s\n", portrait ? "PASS" : "FAIL");
        ok &= portrait;
        sheet.viewport_width = sheet.viewport_height = 480;
        bool square = stylesheet_media_matches(&sheet,
            "(orientation:portrait)", sizeof("(orientation:portrait)") - 1);
        script_runtime_set_viewport(runtime, 480, 480, 480, 480);
        square &= script_runtime_evaluate_diagnostic(runtime,
            "globalThis.pocSummary=String(matchMedia('(orientation:portrait)').matches)",
            "square-orientation", &result) && strcmp(result.summary, "true") == 0;
        ok &= square;
        printf("square orientation %s\n", square ? "PASS" : "FAIL");
        bool bounded = script_runtime_evaluate_diagnostic(runtime,
            "globalThis.pocSummary=String(!matchMedia('('.repeat(40)+'width:480px'+')'.repeat(40)).matches"
            "&&!matchMedia(' '.repeat(4097)).matches)",
            "query-bounds", &result) && strcmp(result.summary, "true") == 0;
        ok &= bounded;
        printf("query quotas %s\n", bounded ? "PASS" : "FAIL");
    }
    script_runtime_destroy(runtime);
    ok &= script_page_fixture_close(&fixture);
    printf("teardown-owned-bytes=%zu\n", fixture.budget.current);
    ok &= stylesheet_application();
    return ok ? 0 : 1;
}
