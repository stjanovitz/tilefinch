/* libFuzzer target: CSS stylesheet, inline style, selector, media/supports
   query and color parsing, followed by cascade resolution and layout over a
   fixed document.

   Input layout (NUL-separated, later parts optional):
     stylesheet \0 inline-style-for-#fz \0 selector-or-query
   Each part is passed with an exact length and no terminator. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/style.h"

#define MIB (1024u * 1024u)

static const char page[] =
    "<!doctype html><html lang=en><head><title>t</title></head>"
    "<body class='b k' dir=ltr>"
    "<header id=top><nav><ul><li class=a><a href='/x' rel=next>one</a>"
    "<li class='a b'><a href='https://e.test/' title=T>two</a></ul></nav>"
    "</header>"
    "<main><article id=art data-kind=note lang=fr>"
    "<h1>Head</h1><p class=k>Para <b>bold</b> <i>it</i> <span id=fz>x</span>"
    "<bdi>\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</bdi> <code>c</code></p>"
    "<div class=g><div class=c1>1</div><div class=c2>2</div>"
    "<div class=c3 hidden>3</div></div>"
    "<table><caption>c</caption><tr><th>h<td colspan=2>d</tr>"
    "<tr><td>a<td>b<td>c</tr></table>"
    "<ol start=3><li>one<li value=9>two</ol>"
    "<form><input id=i1 type=text value=v placeholder=p>"
    "<input type=checkbox checked><label for=i1>L</label>"
    "<select><option>o1<option selected>o2</select>"
    "<textarea>t</textarea><button disabled>b</button></form>"
    "<img alt=alt width=10 height=10><svg width=10 height=10>"
    "<rect width=5 height=5 /></svg>"
    "<details open><summary>s</summary>d</details>"
    "<pre>  pre\ttext\n</pre><blockquote>q</blockquote>"
    "<video controls width=20></video>"
    "</article></main><footer><p>f</p></footer></body></html>";

static size_t next_part(const uint8_t *data, size_t size, size_t *cursor,
                        const char **part)
{
    size_t start = *cursor;
    if (start > size) {
        *part = NULL;
        return 0;
    }
    const uint8_t *nul = memchr(data + start, 0, size - start);
    size_t end = nul == NULL ? size : (size_t) (nul - data);
    *part = (const char *) data + start;
    *cursor = end + 1;
    return end - start;
}

static char *heap_copy(const char *text, size_t length)
{
    char *copy = malloc(length == 0 ? 1 : length);
    if (copy != NULL && length != 0) memcpy(copy, text, length);
    return copy;
}

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *id)
{
    for (; node != NULL; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const char *value = document_attribute(node, "id", NULL);
            if (value != NULL && strcmp(value, id) == 0) return node;
        }
        lxb_dom_node_t *found = find_id(node->first_child, id);
        if (found != NULL) return found;
    }
    return NULL;
}

static void resolve_tree(const Stylesheet *sheet, lxb_dom_node_t *node,
                         const ComputedStyle *parent, const char *selector,
                         size_t selector_length,
                         const StyleQuerySelectorList *list, unsigned depth)
{
    for (; node != NULL && depth < 64; node = node->next) {
        ComputedStyle style;
        const ComputedStyle *own = parent;
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            style = style_for_node(sheet, node, parent);
            own = &style;
            if (selector != NULL) {
                (void) style_selector_matches(node, selector,
                                              selector_length);
                if (list != NULL)
                    (void) style_query_selector_list_matches(list, node, NULL);
            }
        }
        resolve_tree(sheet, node->first_child, own, selector,
                     selector_length, list, depth + 1);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 64 * 1024) return 0;
    size_t cursor = 0;
    const char *css_part, *inline_part, *query_part;
    size_t css_length = next_part(data, size, &cursor, &css_part);
    size_t inline_length = next_part(data, size, &cursor, &inline_part);
    size_t query_length = next_part(data, size, &cursor, &query_part);
    char *css = heap_copy(css_part, css_length);
    char *query = query_part != NULL ? heap_copy(query_part, query_length)
                                     : NULL;

    Budget budget;
    budget_init(&budget, 24u * MIB);
    if (!budget_install_lexbor(&budget)) abort();

    PocDocument document = {0};
    if (!document_parse(&document, &budget, page, sizeof(page) - 1u, 4096))
        abort();
    lxb_dom_node_t *root = lxb_dom_interface_node(document.html);
    if (inline_part != NULL) {
        lxb_dom_node_t *target = find_id(root, "fz");
        if (target == NULL
            || lxb_dom_element_set_attribute(
                   lxb_dom_interface_element(target),
                   (const lxb_char_t *) "style", 5,
                   (const lxb_char_t *) inline_part, inline_length) == NULL)
            abort();
        document_note_attribute_mutation(&document, "style", 5);
    }

    Stylesheet sheet = {0};
    if (stylesheet_build(&sheet, &budget, &document, 480)) {
        (void) stylesheet_add_css_from(&sheet, css, css_length,
                                       "https://base.test/css/main.css");
        if (query != NULL) {
            (void) stylesheet_media_matches(&sheet, query, query_length);
            (void) stylesheet_supports_matches(&sheet, query, query_length);
            uint32_t color = 0;
            uint8_t alpha = 0;
            (void) style_color_parse(query, query_length, &color, &alpha);
        }
        StyleQuerySelectorList list;
        bool prepared = query != NULL
            && style_query_selector_list_prepare(&list, query, query_length);
        resolve_tree(&sheet, root, NULL, query, query_length,
                     prepared ? &list : NULL, 0);
        LayoutDocument layout = {0};
        (void) layout_build(&layout, &budget, &document, &sheet, NULL, NULL,
                            480);
        layout_destroy(&layout);
    }
    stylesheet_destroy(&sheet);
    document_destroy(&document);
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_css: budget leak of %zu bytes\n",
                budget.current);
        budget_dump_active(&budget, stderr, 16);
        abort();
    }
    (void) budget_uninstall_lexbor(&budget);
    free(query);
    free(css);
    return 0;
}
