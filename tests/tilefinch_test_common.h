#ifndef TILEFINCH_TEST_COMMON_H
#define TILEFINCH_TEST_COMMON_H

/* Shared scaffolding for the tilefinch unit-test binaries
   (test_tilefinch.c and test_layout.c): includes, budget/font test
   configuration, the CHECK macro, and small DOM/display-list lookup
   helpers used by more than one suite. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lexbor/html/serialize.h>
#include <lexbor/html/interfaces/template_element.h>

#include "tilefinch/budget.h"
#include "tilefinch/budget_quickjs.h"
#include "tilefinch/cancellation.h"
#include "tilefinch/controller.h"
#include "tilefinch/document.h"
#include "tilefinch/document_backing.h"
#include "tilefinch/fetch.h"
#include "tilefinch/font.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/integer_math.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"
#include "tilefinch/platform.h"
#include "tilefinch/pixel_math.h"
#include "tilefinch/render.h"
#include "tilefinch/resources.h"
#include "tilefinch/section_pager.h"
#include "tilefinch/section_router.h"
#include "tilefinch/section_store.h"
#include "tilefinch/session.h"
#include "tilefinch/style.h"
#include "tilefinch/youtube_lite.h"
#include "tilefinch/youtube_resolver.h"

#define MIB (1024u * 1024u)
#ifndef TILEFINCH_TEST_SANS_FONT
#define TILEFINCH_TEST_SANS_FONT ""
#endif
#ifndef TILEFINCH_TEST_SOURCE_DIR
#define TILEFINCH_TEST_SOURCE_DIR "."
#endif
#ifndef TILEFINCH_TEST_SERIF_FONT
#define TILEFINCH_TEST_SERIF_FONT ""
#endif
#ifndef TILEFINCH_TEST_SANS_BOLD_FONT
#define TILEFINCH_TEST_SANS_BOLD_FONT ""
#endif
#ifndef TILEFINCH_TEST_SERIF_BOLD_FONT
#define TILEFINCH_TEST_SERIF_BOLD_FONT ""
#endif
#ifndef TILEFINCH_TEST_SANS_ITALIC_FONT
#define TILEFINCH_TEST_SANS_ITALIC_FONT ""
#endif
#ifndef TILEFINCH_TEST_METRIC_SANS_FONT
#define TILEFINCH_TEST_METRIC_SANS_FONT ""
#endif
#ifndef TILEFINCH_TEST_METRIC_SANS_BOLD_FONT
#define TILEFINCH_TEST_METRIC_SANS_BOLD_FONT ""
#endif
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                 \
        return 1;                                                            \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *wanted)
{
    for (; node != NULL; node = node->next) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
        lxb_dom_node_t *found = find_id(node->first_child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}
static int command_x(const LayoutDocument *layout, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) return command->x;
    }
    return -1;
}
static const DrawCommand *text_command(const LayoutDocument *layout,
                                       const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    for (size_t i = 0; i < layout->count; i++) {
        const DrawCommand *command = &layout->commands[i];
        if (command->type == DRAW_TEXT
            && command->text_length == wanted_length
            && memcmp(command->text, wanted, wanted_length) == 0) {
            return command;
        }
    }
    return NULL;
}
static uint16_t test_rgb565(uint32_t color)
{
    return (uint16_t) (((color >> 19) << 11)
                       | (((color >> 10) & 0x3fu) << 5)
                       | ((color >> 3) & 0x1fu));
}

#endif /* TILEFINCH_TEST_COMMON_H */
