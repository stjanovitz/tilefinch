/* libFuzzer target: Tilefinch's HTML document glue on top of lexbor
   (streamed document_parse, document statistics/body text, attribute
   helpers), followed by stylesheet construction from inline <style> and
   style attributes and a full layout pass.

   Byte 0 selects the parser chunk size (byte % 5); bit 7 parses with
   scripting enabled (so <noscript> is raw text) and then applies the
   static-shell fallback that runs when the script pipeline cannot start
   (innerHTML replacement of authored content). The rest is the HTML
   document. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/layout.h"
#include "tilefinch/style.h"

#define MIB (1024u * 1024u)

static void walk(lxb_dom_node_t *node, unsigned depth)
{
    for (; node != NULL && depth < 256; node = node->next) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t length = 0;
            (void) document_element_name(node, &length);
            (void) document_attribute(node, "href", &length);
            (void) document_control_value(node, &length);
        } else if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            size_t length = 0;
            (void) document_text_data(node, &length);
        }
        walk(node->first_child, depth + 1);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 128 * 1024) return 0;
    static const size_t chunks[] = {1, 7, 64, 4096, 65536};
    size_t chunk = chunks[data[0] % (sizeof(chunks) / sizeof(chunks[0]))];
    size_t length = size - 1;
    char *html = malloc(length == 0 ? 1 : length);
    if (html == NULL) return 0;
    if (length != 0) memcpy(html, data + 1, length);

    Budget budget;
    budget_init(&budget, 24u * MIB);
    if (!budget_install_lexbor(&budget)) abort();
    PocDocument document = {0};
    bool parsed;
    if ((data[0] & 0x80u) != 0) {
        DocumentParser parser = {0};
        parsed = document_parser_begin(&parser, &budget)
            && document_parser_set_scripting(&parser, true);
        for (size_t offset = 0; parsed && offset < length; offset += chunk) {
            size_t part = length - offset < chunk ? length - offset : chunk;
            parsed = document_parser_feed(&parser, html + offset, part);
        }
        parsed = parsed && document_parser_finish(&parser, &document);
        if (!parsed) document_parser_abort(&parser);
        if (parsed) (void) document_install_static_shell_fallback(&document);
    } else {
        parsed = document_parse(&document, &budget, html, length, chunk);
    }
    if (parsed) {
        (void) document_body_text(&document);
        walk(lxb_dom_interface_node(document.html), 0);
        Stylesheet sheet = {0};
        if (stylesheet_build(&sheet, &budget, &document, 480)) {
            LayoutDocument layout = {0};
            (void) layout_build(&layout, &budget, &document, &sheet, NULL,
                                NULL, 480);
            layout_destroy(&layout);
        }
        stylesheet_destroy(&sheet);
        (void) document_refresh(&document);
    }
    document_destroy(&document);
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_html: budget leak of %zu bytes\n",
                budget.current);
        budget_dump_active(&budget, stderr, 16);
        abort();
    }
    (void) budget_uninstall_lexbor(&budget);
    free(html);
    return 0;
}
