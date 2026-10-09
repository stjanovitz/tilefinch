#include <stdio.h>
#include <string.h>
#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/style.h"
#include "tilefinch/layout.h"

static bool run_case(const char *kind, bool column, bool explicit_height,
                      bool textarea, bool stretch)
{
    char html[1536];
    snprintf(html, sizeof(html),
        "<!doctype html><style>body{margin:8px}.row{display:flex;"
        "flex-direction:%s;align-items:center;font:24px/29px sans-serif;"
        "height:29px;width:360px;overflow:hidden;border:1px solid #888}"
        "input,textarea{font:inherit;padding:0;border:0;width:220px;align-self:%s;%s}"
        "</style><div class=row><span>$</span>%s</div>",
        column ? "column" : "row", stretch ? "stretch" : "center",
        explicit_height ? "height:29px" : "", kind);
    Budget budget; budget_init(&budget, 16u * 1024u * 1024u);
    PocDocument document = {0}; Stylesheet sheet = {0}; LayoutDocument layout = {0};
    bool parsed = budget_install_lexbor(&budget)
        && document_parse(&document, &budget, html, strlen(html), 17);
    bool styled = parsed && stylesheet_build(&sheet, &budget, &document, 480);
    bool built = styled && layout_build(&layout, &budget, &document, &sheet, NULL, NULL, 480);
    bool ok = built && layout.control_count == 1;
    const ControlRegion *control = ok ? &layout.controls[0] : NULL;
    const DrawCommand *text = NULL;
    for (size_t i = 0; built && i < layout.count; i++) {
        const DrawCommand *command = &layout.commands[i];
        if (command->type == DRAW_TEXT && command->text_length > 1) text = command;
    }
    ok &= text != NULL;
    if (text && control) {
        /* Explicit height uses the authored border box (29px). The native
           hit region may include the inherited line's descender pixel. */
        int expected_y = textarea ? control->y
            : control->y + ((explicit_height ? 29 : control->height) - text->height) / 2;
        ok &= text->y == expected_y;
        printf("input %s column=%d explicit=%d text-y=%d box-y=%d box-h=%d expected-y=%d %s\n",
            kind, column, explicit_height, text->y, control->y, control->height,
            expected_y, ok ? "PASS" : "FAIL");
    }
    if (built) layout_destroy(&layout);
    if (styled) stylesheet_destroy(&sheet);
    if (parsed) document_destroy(&document);
    printf("teardown-owned-bytes=%zu\n", budget.current);
    return ok && budget.current == 0;
}

int main(void)
{
    bool ok = true;
    const char *values[] = {"<input value=123456>", "<input placeholder=123456>", "<input type=password value=123456>"};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        ok &= run_case(values[i], false, false, false, true);
        ok &= run_case(values[i], false, true, false, true);
        ok &= run_case(values[i], true, true, false, true);
        ok &= run_case(values[i], false, true, false, false);
    }
    ok &= run_case("<textarea>123456</textarea>", false, true, true, true);
    ok &= run_case("<textarea>123456</textarea>", true, true, true, true);
    return ok ? 0 : 1;
}
