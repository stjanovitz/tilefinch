#include "tilefinch/page_find.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

int main(void)
{
    Budget budget;
    budget_init(&budget, 128u * 1024u);
    DrawCommand commands[] = {
        {.type = DRAW_TEXT, .text = "The", .text_length = 3,
         .x = 10, .y = 40, .width = 30, .height = 18,
         .radius = LAYOUT_TEXT_FIND_BLOCK_START},
        {.type = DRAW_TEXT, .text = "quick", .text_length = 5,
         .x = 50, .y = 40, .width = 50, .height = 18,
         .radius = LAYOUT_TEXT_FIND_SPACE_BEFORE},
        {.type = DRAW_FILL_RECT, .x = 0, .y = 0, .width = 1, .height = 1},
        {.type = DRAW_TEXT, .text = "Brown", .text_length = 5,
         .x = 110, .y = 40, .width = 50, .height = 18,
         .radius = LAYOUT_TEXT_FIND_SPACE_BEFORE},
        {.type = DRAW_TEXT, .text = "fox", .text_length = 3,
         .x = 170, .y = 40, .width = 30, .height = 18,
         .radius = LAYOUT_TEXT_FIND_SPACE_BEFORE},
        {.type = DRAW_TEXT, .text = "banana", .text_length = 6,
         .x = 10, .y = 80, .width = 60, .height = 18,
         .radius = LAYOUT_TEXT_FIND_BLOCK_START}
    };
    uint8_t flags[6] = {0};
    LayoutDocument layout = {
        .commands = commands,
        .count = sizeof(commands) / sizeof(commands[0]),
        .command_flags = flags
    };
    PageFindIndex find = {0};
    CHECK(page_find_build(&find, &budget, &layout, "QUICK brown"));
    CHECK(find.match_count == 1 && find.selected == 0 && !find.truncated);
    CHECK(find.matches[0].start.command_index == 1
          && find.matches[0].start.text_offset == 0
          && find.matches[0].end.command_index == 3
          && find.matches[0].end.text_offset == 5
          && find.matches[0].top == 40
          && find.matches[0].bottom == 58);
    commands[1].y = 44;
    commands[3].y = 44;
    CHECK(page_find_refresh_geometry(&find, &layout)
          && find.matches[0].top == 44
          && find.matches[0].bottom == 62);
    layout.text_fingerprint++;
    CHECK(!page_find_refresh_geometry(&find, &layout));
    layout.text_fingerprint--;
    commands[1].y = 40;
    commands[3].y = 40;
    PageFindRect rects[PAGE_FIND_RECT_LIMIT] = {{0}};
    size_t rect_count = page_find_match_rects(
        &find, &layout, 0, rects, PAGE_FIND_RECT_LIMIT);
    CHECK(rect_count == 2 && rects[0].x == 50 && rects[0].width == 50
          && rects[1].x == 110 && rects[1].width == 50);

    CHECK(page_find_build(&find, &budget, &layout, "ana"));
    CHECK(find.match_count == 2);
    CHECK(page_find_select_nearest(&find, 70) && find.selected == 0);
    CHECK(page_find_move(&find, 1) && find.selected == 1 && !find.wrapped);
    CHECK(page_find_move(&find, 1) && find.selected == 0 && find.wrapped);
    CHECK(page_find_move(&find, -1) && find.selected == 1 && find.wrapped);

    /* A block boundary resets KMP: concatenated words must not match across
       paragraphs, while an authored collapsed space does match phrases. */
    CHECK(page_find_build(&find, &budget, &layout, "foxbanana"));
    CHECK(find.match_count == 0);
    CHECK(page_find_build(
        &find, &budget, &layout, "brown\xc2\xa0" "fox"));
    CHECK(find.match_count == 1);

    /* Search remains logical, while its highlight follows the retained
       visual-order glyph map rather than interpolating the command box. */
    DrawCommand bidi_text = {
        .type = DRAW_TEXT, .text = "abc", .text_length = 3,
        .x = 10, .y = 60, .width = 30, .height = 18
    };
    LayoutBidiGlyph bidi_glyphs[] = {
        {.codepoint = 'c', .x_fixed = 10 * 64,
         .logical_byte_offset = 2, .advance_fixed = 10 * 64,
         .logical_byte_length = 1, .level = 1},
        {.codepoint = 'b', .x_fixed = 20 * 64,
         .logical_byte_offset = 1, .advance_fixed = 10 * 64,
         .logical_byte_length = 1, .level = 1},
        {.codepoint = 'a', .x_fixed = 30 * 64,
         .logical_byte_offset = 0, .advance_fixed = 10 * 64,
         .logical_byte_length = 1, .level = 1}
    };
    LayoutBidiCommand bidi_map = {
        .command_index = 0, .glyph_start = 0, .glyph_count = 3, .level = 1
    };
    LayoutDocument bidi_layout = {
        .commands = &bidi_text, .count = 1,
        .bidi_glyphs = bidi_glyphs, .bidi_glyph_count = 3,
        .bidi_commands = &bidi_map, .bidi_command_count = 1
    };
    CHECK(page_find_build(&find, &budget, &bidi_layout, "a")
          && find.match_count == 1);
    rect_count = page_find_match_rects(
        &find, &bidi_layout, 0, rects, PAGE_FIND_RECT_LIMIT);
    CHECK(rect_count == 1 && rects[0].x == 30 && rects[0].width == 10);
    CHECK(page_find_build(&find, &budget, &bidi_layout, "c")
          && find.match_count == 1);
    rect_count = page_find_match_rects(
        &find, &bidi_layout, 0, rects, PAGE_FIND_RECT_LIMIT);
    CHECK(rect_count == 1 && rects[0].x == 10 && rects[0].width == 10);
    size_t logical = SIZE_MAX;
    int caret_x = -1;
    CHECK(layout_bidi_visual_hit_test(
              &bidi_layout, 0, 10 * 64, &logical, &caret_x)
          && logical == 3 && caret_x == 10 * 64);
    CHECK(layout_bidi_visual_step(
              &bidi_layout, 0, caret_x, 1, &logical, &caret_x)
          && logical == 2 && caret_x == 20 * 64);
    CHECK(layout_bidi_visual_step(
              &bidi_layout, 0, caret_x, 1, &logical, &caret_x)
          && logical == 1 && caret_x == 30 * 64);
    CHECK(layout_bidi_visual_step(
              &bidi_layout, 0, caret_x, 1, &logical, &caret_x)
          && logical == 0 && caret_x == 40 * 64);
    CHECK(!layout_bidi_visual_step(
        &bidi_layout, 0, caret_x, 1, &logical, &caret_x));

    char repeated[PAGE_FIND_MATCH_LIMIT + 32u];
    memset(repeated, 'x', sizeof(repeated));
    DrawCommand repeated_command = {
        .type = DRAW_TEXT, .text = repeated,
        .text_length = sizeof(repeated),
        .x = 0, .y = 10, .width = 400, .height = 18
    };
    uint8_t fixed_flag = LAYOUT_COMMAND_FIXED;
    LayoutDocument repeated_layout = {
        .commands = &repeated_command,
        .count = 1,
        .command_flags = &fixed_flag
    };
    CHECK(page_find_build(&find, &budget, &repeated_layout, "x"));
    CHECK(find.match_count == PAGE_FIND_MATCH_LIMIT && find.truncated);
    CHECK(find.matches[0].fixed);
    rect_count = page_find_match_rects(
        &find, &repeated_layout, 0, rects, PAGE_FIND_RECT_LIMIT);
    CHECK(rect_count == 1 && rects[0].fixed);

    page_find_clear(&find);
    CHECK(budget.current == 0 && budget_categories_reconcile(&budget));
    CHECK(!page_find_build(&find, &budget, &layout, ""));
    char oversized[PAGE_FIND_QUERY_LIMIT + 2u];
    memset(oversized, 'q', sizeof(oversized));
    oversized[sizeof(oversized) - 1u] = '\0';
    CHECK(!page_find_build(&find, &budget, &layout, oversized));
    puts("page-find-tests: ok");
    return 0;
}
