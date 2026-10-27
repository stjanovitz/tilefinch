/* Models that the block, inline and flex layout paths share: the form-control
   painter pieces (value text, resize grip) and, below, the replaced-element
   intrinsic model and tag-id predicates.  Each case runs the same element
   through more than one formatting path so the paths cannot drift apart. */

#include "tilefinch_test_common.h"
#include <strings.h>
#include "../src/layout_internal.h"

typedef struct {
    Budget budget;
    PocDocument document;
    Stylesheet sheet;
    LayoutDocument layout;
    bool parsed, styled, built;
} ModelPage;

static bool model_page_open(ModelPage *page, const char *html)
{
    memset(page, 0, sizeof(*page));
    budget_init(&page->budget, 16u * MIB);
    page->parsed = budget_install_lexbor(&page->budget)
        && document_parse(&page->document, &page->budget, html,
                          strlen(html), 17);
    page->styled = page->parsed
        && stylesheet_build(&page->sheet, &page->budget, &page->document,
                            480);
    page->built = page->styled
        && layout_build(&page->layout, &page->budget, &page->document,
                        &page->sheet, NULL, NULL, 480);
    return page->built;
}

static int model_page_close(ModelPage *page)
{
    if (page->built) layout_destroy(&page->layout);
    if (page->styled) stylesheet_destroy(&page->sheet);
    if (page->parsed) document_destroy(&page->document);
    CHECK(page->budget.current == 0u && budget_uninstall_lexbor(&page->budget));
    return 0;
}

static lxb_dom_node_t *model_node(ModelPage *page, const char *id)
{
    return find_id(lxb_dom_interface_node(page->document.html), id);
}

/* --- Form controls ------------------------------------------------------ */

/* Every resizable textarea (authored `resize`; the UA sheet leaves it at
   none) gets a hit region and a visible three-line grip
   in its bottom-right corner, whichever arm (block or inline-block) laid it
   out. Before the painters were shared only the block arm drew the grip. */
static int test_textarea_resize_grip_every_display(void)
{
    static const char page_html[] =
        "<!doctype html><style>body{margin:0;color:#102030}"
        "textarea{width:120px;height:60px;margin:4px;resize:both}</style>"
        "<textarea id=inline>a</textarea>"
        "<textarea id=block style=display:block>b</textarea>"
        "<textarea id=fixed style=resize:none>c</textarea>";
    ModelPage page;
    CHECK(model_page_open(&page, page_html));
    const char *ids[] = {"inline", "block", "fixed"};
    for (size_t k = 0; k < 3; k++) {
        lxb_dom_node_t *node = model_node(&page, ids[k]);
        const LayoutNodeBox *box = layout_box_for_node(&page.layout, node);
        CHECK(node != NULL && box != NULL);
        size_t handles = 0;
        int handle_right = 0, handle_bottom = 0;
        for (size_t i = 0; i < page.layout.control_count; i++) {
            const ControlRegion *region = &page.layout.controls[i];
            if (region->node != node || region->type != CONTROL_RESIZE)
                continue;
            handles++;
            handle_right = region->x + region->width;
            handle_bottom = region->y + region->height;
        }
        size_t grip_lines = 0;
        for (size_t i = box->command_start; i < box->command_end; i++) {
            const DrawCommand *command = &page.layout.commands[i];
            if (command->type != DRAW_FILL_RECT || command->height != 1
                || command->color != 0x102030) continue;
            int size = command->width;
            if ((size == 2 || size == 5 || size == 8)
                && command->x == handle_right - size - 2) grip_lines++;
        }
        bool resizable = k != 2;
        if (resizable) {
            CHECK(handles == 1 && handle_right == box->x + box->width
                  && handle_bottom == box->y + box->height
                  && grip_lines == 3);
        } else {
            CHECK(handles == 0 && grip_lines == 0);
        }
    }
    return model_page_close(&page);
}

static const DrawCommand *model_text_in(const ModelPage *page,
                                        const LayoutNodeBox *box)
{
    for (size_t i = box->command_start; i < box->command_end; i++) {
        if (page->layout.commands[i].type == DRAW_TEXT
            && page->layout.commands[i].text_length != 0)
            return &page->layout.commands[i];
    }
    return NULL;
}

static bool model_text_is(const DrawCommand *text, const char *wanted)
{
    if (wanted[0] == '\0') return text == NULL;
    return text != NULL && text->text_length == strlen(wanted)
        && memcmp(text->text, wanted, text->text_length) == 0;
}

/* Value selection is one function for both arms: a password value is
   masked and a value-less push button shows its UA label even when the
   control is blockified (display:block, or a flex item), while an empty
   value attribute is an empty label (DuckDuckGo's icon search button
   relies on that). A file input's value attribute is never its label, so
   it keeps "CHOOSE FILE" with an empty or any authored value. The placeholder
   is author text and is never masked. */
static int test_control_value_text_every_display(void)
{
    static const char page_html[] =
        "<!doctype html><style>body{margin:0}input{width:160px}"
        ".b{display:block}.f{display:flex}</style>"
        "<input id=pw-inline type=password value=secret>"
        "<input id=pw-block class=b type=password value=secret>"
        "<div class=f><input id=pw-flex type=password value=secret></div>"
        "<input id=pw-upper type=PASSWORD value=secret>"
        "<input id=ph-inline type=password placeholder=Passphrase>"
        "<input id=ph-block class=b type=password placeholder=Passphrase>"
        "<input id=submit-inline type=submit>"
        "<input id=submit-block class=b type=submit>"
        "<input id=reset-block class=b type=reset>"
        "<input id=empty-inline type=submit value=''>"
        "<input id=empty-block class=b type=submit value=''>"
        "<input id=text-block class=b value=plain>"
        "<input id=file-empty type=file value=''>"
        "<input id=file-value class=b type=file value=x.txt>"
        "<input id=file-bare type=file placeholder=Pick>";
    ModelPage page;
    CHECK(model_page_open(&page, page_html));
    static const struct { const char *id, *text; } expected[] = {
        {"pw-inline", "******"}, {"pw-block", "******"},
        {"pw-flex", "******"}, {"pw-upper", "******"},
        {"ph-inline", "Passphrase"}, {"ph-block", "Passphrase"},
        {"submit-inline", "SUBMIT"}, {"submit-block", "SUBMIT"},
        {"reset-block", "RESET"}, {"text-block", "plain"},
        {"empty-inline", ""}, {"empty-block", ""},
        {"file-empty", "CHOOSE FILE"}, {"file-value", "CHOOSE FILE"},
        {"file-bare", "CHOOSE FILE"},
    };
    int failures = 0;
    for (size_t k = 0; k < sizeof(expected) / sizeof(expected[0]); k++) {
        const LayoutNodeBox *box = layout_box_for_node(
            &page.layout, model_node(&page, expected[k].id));
        const DrawCommand *text = box == NULL ? NULL
            : model_text_in(&page, box);
        if (!model_text_is(text, expected[k].text)) {
            fprintf(stderr, "control %s: got '%.*s', want '%s'\n",
                    expected[k].id, text == NULL ? 0 : (int) text->text_length,
                    text == NULL ? "" : text->text, expected[k].text);
            failures++;
        }
    }
    CHECK(model_page_close(&page) == 0);
    CHECK(failures == 0);
    return 0;
}

/* --- Replaced elements --------------------------------------------------- */

/* One intrinsic model answers for every formatting path: the same element
   as an inline atomic box, as a display:block box and as a flex item gets
   the same used size from the shared natural size and default boxes. */
static int test_replaced_intrinsic_model_every_path(void)
{
    static const struct {
        const char *markup;
        int width, height;
    } cases[] = {
        {"<video id=%s></video>", 300, 150},
        {"<iframe id=%s></iframe>", 300, 150},
        {"<canvas id=%s></canvas>", 300, 150},
        {"<audio id=%s controls></audio>", 300, 54},
        {"<img id=%s width=40 height=20 style=width:80px;height:auto>",
         80, 40},
        {"<video id=%s style=width:120px></video>", 120, 150},
        {"<canvas id=%s style=width:120px></canvas>", 120, 60},
    };
    static const char *const contexts[] = {
        "<p>%s</p>",
        "<div class=b>%s</div>",
        "<div class=f>%s</div>",
    };
    int failures = 0;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char element[128], body[1536], page_html[2048];
        size_t used = 0;
        body[0] = '\0';
        for (size_t k = 0; k < 3; k++) {
            char id[8], wrapped[256];
            snprintf(id, sizeof(id), "r%zu", k);
            snprintf(element, sizeof(element), cases[c].markup, id);
            snprintf(wrapped, sizeof(wrapped), contexts[k], element);
            used += (size_t) snprintf(body + used, sizeof(body) - used,
                                      "%s", wrapped);
        }
        snprintf(page_html, sizeof(page_html),
                 "<!doctype html><style>body{margin:0}p,div{margin:0}"
                 "video,iframe,canvas,audio,img{border:0;padding:0;"
                 "margin:0;vertical-align:top}"
                 ".b>*{display:block}.f{display:flex;align-items:flex-start}"
                 "</style>%s", body);
        ModelPage page;
        CHECK(model_page_open(&page, page_html));
        for (size_t k = 0; k < 3; k++) {
            char id[8];
            snprintf(id, sizeof(id), "r%zu", k);
            const LayoutNodeBox *box = layout_box_for_node(
                &page.layout, model_node(&page, id));
            if (box == NULL || box->width != cases[c].width
                || box->height != cases[c].height) {
                fprintf(stderr, "replaced %s in %s: got %dx%d, want %dx%d\n",
                        cases[c].markup, contexts[k],
                        box ? box->width : -1, box ? box->height : -1,
                        cases[c].width, cases[c].height);
                failures++;
            }
        }
        CHECK(model_page_close(&page) == 0);
    }
    CHECK(failures == 0);
    return 0;
}

/* --- Tag-id predicates ---------------------------------------------------- */

/* The retired string tests: layout_node_name_is/image_name_is compared the
   element's local name exactly, reader_name_is case-insensitively. */
static bool reference_name_is(lxb_dom_node_t *node, const char *wanted,
                              bool fold_case)
{
    size_t length = 0;
    const char *name = document_element_name(node, &length);
    if (name == NULL || strlen(wanted) != length) return false;
    return fold_case ? strncasecmp(name, wanted, length) == 0
                     : memcmp(name, wanted, length) == 0;
}

static size_t tag_equivalence_walk(lxb_dom_node_t *node, size_t *failures)
{
    size_t visited = 0;
    for (; node != NULL; node = node->next) {
        visited++;
        for (lxb_tag_id_t tag = 0; tag < LXB_TAG__LAST_ENTRY; tag++) {
            const char *name = (const char *) lxb_tag_name_by_id(tag, NULL);
            if (name == NULL) continue;
            bool by_id = document_node_is_tag(node, tag);
            if (reference_name_is(node, name, false) != by_id
                || reference_name_is(node, name, true) != by_id) {
                size_t length = 0;
                const char *actual = document_element_name(node, &length);
                fprintf(stderr, "tag %s: node %.*s (type %d) by-id=%d\n",
                        name, (int) length, actual == NULL ? "" : actual,
                        (int) node->type, by_id);
                (*failures)++;
            }
        }
        if (lxb_dom_interface_node(node)->first_child != NULL)
            visited += tag_equivalence_walk(node->first_child, failures);
    }
    return visited;
}

/* document_node_is_tag() answers exactly what the string tests did, for
   every static lexbor tag and every node: HTML in any source case, SVG and
   MathML (SVG <a> and <image> versus HTML <a> and <img>, camel-case
   foreignObject), custom elements whose names contain static ones, text
   and comment nodes, and elements created through the DOM API with
   upper-case names or a foreign namespace. */
static int test_tag_id_predicate_equivalence(void)
{
    static const char page_html[] =
        "<!doctype html><title>t</title><style>p{}</style>"
        "<IMG src=x><Img alt=y><A href=#>a</A><DIV><P>p<BR></P></DIV>"
        "<svg><a href=#><text>t</text></a><image href=x></image>"
        "<foreignObject><p>in</p></foreignObject><g><use/></g>"
        "<title>s</title><style></style><script></script></svg>"
        "<math><mi>x</mi><mtext>y</mtext><annotation-xml></annotation-xml>"
        "</math><x-img></x-img><img-x></img-x><my-a></my-a><video-player>"
        "</video-player><table><tr><td>c</td></tr></table>"
        "<template><img></template><!-- c --><select><option>o</select>"
        "<textarea>t</textarea><input><button>b</button><hr><ol><li>l</ol>";
    ModelPage page;
    CHECK(model_page_open(&page, page_html));
    lxb_dom_document_t *dom = lxb_dom_interface_node(page.document.html)
        ->owner_document;
    lxb_dom_node_t *body = lxb_dom_interface_node(
        lxb_html_document_body_element(page.document.html));
    CHECK(dom != NULL && body != NULL);
    static const char svg_ns[] = "http://www.w3.org/2000/svg";
    static const struct { const char *name; bool svg; } created[] = {
        {"IMG", false}, {"Video", false}, {"x-Img", false},
        {"A", true}, {"foreignObject", true}, {"IMAGE", true},
        {"Svg", true}, {"g", true},
    };
    for (size_t i = 0; i < sizeof(created) / sizeof(created[0]); i++) {
        const char *name = created[i].name;
        lxb_dom_element_t *element = created[i].svg
            ? lxb_dom_element_create(dom, (const lxb_char_t *) name,
                  strlen(name), (const lxb_char_t *) svg_ns,
                  sizeof(svg_ns) - 1, NULL, 0, NULL, 0, true)
            : lxb_dom_document_create_element(
                  dom, (const lxb_char_t *) name, strlen(name), NULL);
        CHECK(element != NULL);
        lxb_dom_node_insert_child(body, lxb_dom_interface_node(element));
    }
    size_t failures = 0;
    size_t visited = tag_equivalence_walk(
        lxb_dom_interface_node(dom), &failures);
    CHECK(model_page_close(&page) == 0);
    CHECK(visited > 60 && failures == 0);
    return 0;
}

int main(void)
{
    CHECK(test_textarea_resize_grip_every_display() == 0);
    CHECK(test_control_value_text_every_display() == 0);
    CHECK(test_replaced_intrinsic_model_every_path() == 0);
    CHECK(test_tag_id_predicate_equivalence() == 0);
    puts("layout shared models: ok");
    return 0;
}
