#ifndef TILEFINCH_PAGE_FIND_H
#define TILEFINCH_PAGE_FIND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/layout.h"

#define PAGE_FIND_QUERY_LIMIT 96u
#define PAGE_FIND_MATCH_LIMIT 256u
#define PAGE_FIND_RECT_LIMIT 8u
#define PAGE_FIND_COMMAND_SPAN_LIMIT 64u

typedef struct {
    uint32_t command_index;
    uint32_t text_offset;
} PageFindPosition;

typedef struct {
    PageFindPosition start;
    PageFindPosition end;
    int top;
    int bottom;
    bool fixed;
} PageFindMatch;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    bool fixed;
} PageFindRect;

typedef struct {
    Budget *budget;
    PageFindMatch *matches;
    size_t match_count;
    size_t selected;
    uint64_t layout_text_fingerprint;
    char query[PAGE_FIND_QUERY_LIMIT + 1u];
    bool truncated;
    bool wrapped;
} PageFindIndex;

/* Builds once from retained visible text commands. No DOM walk or relayout is
   performed, and the result is capped at PAGE_FIND_MATCH_LIMIT. */
bool page_find_build(PageFindIndex *index, Budget *budget,
                     const LayoutDocument *layout, const char *query);
void page_find_clear(PageFindIndex *index);
bool page_find_select_nearest(PageFindIndex *index, int document_y);
/* Reuses KMP positions when a relayout changed geometry but retained the
   exact same text stream and block/space boundaries. */
bool page_find_refresh_geometry(PageFindIndex *index,
                                const LayoutDocument *layout);
bool page_find_move(PageFindIndex *index, int direction);
const PageFindMatch *page_find_selected(const PageFindIndex *index);
/* Returns bounded command-fragment rectangles for one match. Adjacent
   fragments on the same line are merged. */
size_t page_find_match_rects(const PageFindIndex *index,
                             const LayoutDocument *layout,
                             size_t match_index, PageFindRect *rects,
                             size_t capacity);

#endif
