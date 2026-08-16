#include "tilefinch/page_find.h"

#include <limits.h>
#include <string.h>

_Static_assert(sizeof(PageFindMatch) * PAGE_FIND_MATCH_LIMIT <= 8u * 1024u,
               "page-find retained table must stay below 8 KiB");

static unsigned char find_fold(unsigned char value)
{
    return value >= 'A' && value <= 'Z'
        ? (unsigned char) (value + ('a' - 'A')) : value;
}

static bool find_space_at(const unsigned char *text, size_t length,
                          size_t at, size_t *consumed)
{
    if (consumed != NULL) *consumed = 1;
    if (text == NULL || at >= length) return false;
    unsigned char value = text[at];
    if (value == ' ' || value == '\t' || value == '\r'
        || value == '\n' || value == '\f') return true;
    if (value == 0xc2u && at + 1u < length && text[at + 1u] == 0xa0u) {
        if (consumed != NULL) *consumed = 2;
        return true;
    }
    return false;
}

static bool find_normalize_query(const char *query,
                                 unsigned char output[PAGE_FIND_QUERY_LIMIT],
                                 size_t *output_length)
{
    if (query == NULL || output == NULL || output_length == NULL)
        return false;
    size_t raw_length = strnlen(query, PAGE_FIND_QUERY_LIMIT + 1u);
    if (raw_length == 0 || raw_length > PAGE_FIND_QUERY_LIMIT) return false;
    size_t written = 0;
    bool pending_space = false;
    for (size_t at = 0; at < raw_length;) {
        size_t consumed = 1;
        if (find_space_at(
                (const unsigned char *) query, raw_length, at, &consumed)) {
            pending_space = written != 0;
            at += consumed;
            continue;
        }
        if (pending_space) {
            if (written == PAGE_FIND_QUERY_LIMIT) return false;
            output[written++] = ' ';
            pending_space = false;
        }
        if (written == PAGE_FIND_QUERY_LIMIT) return false;
        output[written++] = (unsigned char) query[at++];
    }
    *output_length = written;
    return written != 0;
}

static bool find_text_command(const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && command->text != NULL && command->text_length != 0
        && !draw_command_is_text_shadow(command);
}

static void find_match_vertical_bounds(const LayoutDocument *layout,
                                       PageFindMatch *match)
{
    match->top = INT_MAX;
    match->bottom = INT_MIN;
    match->fixed = false;
    if (layout == NULL || match->start.command_index >= layout->count
        || match->end.command_index >= layout->count
        || match->end.command_index < match->start.command_index
        || match->end.command_index - match->start.command_index
               > PAGE_FIND_COMMAND_SPAN_LIMIT) {
        match->top = match->bottom = 0;
        return;
    }
    for (size_t at = match->start.command_index;
         at <= match->end.command_index; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (!find_text_command(command)) continue;
        if (layout->command_flags != NULL
            && (layout->command_flags[at] & LAYOUT_COMMAND_FIXED) != 0)
            match->fixed = true;
        if (command->y < match->top) match->top = command->y;
        int64_t bottom_wide = (int64_t) command->y
                              + (int64_t) command->height;
        int bottom = bottom_wide > INT_MAX ? INT_MAX
            : bottom_wide < INT_MIN ? INT_MIN : (int) bottom_wide;
        if (bottom > match->bottom) match->bottom = bottom;
    }
    if (match->top == INT_MAX || match->bottom == INT_MIN)
        match->top = match->bottom = 0;
}

bool page_find_build(PageFindIndex *index, Budget *budget,
                     const LayoutDocument *layout, const char *query)
{
    if (index == NULL || budget == NULL || layout == NULL || query == NULL)
        return false;
    page_find_clear(index);
    unsigned char normalized[PAGE_FIND_QUERY_LIMIT];
    unsigned char folded[PAGE_FIND_QUERY_LIMIT];
    size_t query_length = 0;
    if (!find_normalize_query(query, normalized, &query_length)) return false;
    index->matches = budget_calloc_category(
        budget, BUDGET_CATEGORY_NAVIGATION,
        PAGE_FIND_MATCH_LIMIT, sizeof(*index->matches));
    if (index->matches == NULL) return false;
    index->budget = budget;
    index->layout_text_fingerprint = layout->text_fingerprint;
    memcpy(index->query, normalized, query_length);
    index->query[query_length] = '\0';

    uint8_t prefix[PAGE_FIND_QUERY_LIMIT] = {0};
    for (size_t i = 0; i < query_length; i++)
        folded[i] = find_fold(normalized[i]);
    for (size_t i = 1, matched = 0; i < query_length; i++) {
        while (matched != 0 && folded[i] != folded[matched])
            matched = prefix[matched - 1u];
        if (folded[i] == folded[matched]) matched++;
        prefix[i] = (uint8_t) matched;
    }

    PageFindPosition recent[PAGE_FIND_QUERY_LIMIT] = {{0}};
    size_t stream_offset = 0, matched = 0;
    bool previous_space = false;
    for (size_t command_index = 0; command_index < layout->count;
         command_index++) {
        const DrawCommand *command = &layout->commands[command_index];
        if (!find_text_command(command)) continue;
        bool block_start = draw_command_text_find_block_start(command);
        if (block_start) {
            matched = 0;
            previous_space = false;
        } else if (draw_command_text_find_space_before(command)
                   && !previous_space) {
            unsigned char value = ' ';
            recent[stream_offset % query_length] = (PageFindPosition) {
                .command_index = (uint32_t) command_index,
                .text_offset = 0
            };
            while (matched != 0 && value != folded[matched])
                matched = prefix[matched - 1u];
            if (value == folded[matched]) matched++;
            stream_offset++;
            previous_space = true;
        }
        for (size_t offset = 0; offset < command->text_length;) {
            size_t consumed = 1;
            bool is_space = find_space_at(
                (const unsigned char *) command->text,
                command->text_length, offset, &consumed);
            if (is_space && previous_space) {
                offset += consumed;
                continue;
            }
            unsigned char value = is_space ? ' ' : find_fold(
                (unsigned char) command->text[offset]);
            PageFindPosition position = {
                .command_index = (uint32_t) command_index,
                .text_offset = (uint32_t) offset
            };
            recent[stream_offset % query_length] = position;
            while (matched != 0 && value != folded[matched])
                matched = prefix[matched - 1u];
            if (value == folded[matched]) matched++;
            stream_offset++;
            previous_space = is_space;
            offset += consumed;
            if (matched != query_length) continue;

            PageFindPosition start = recent[
                (stream_offset - query_length) % query_length];
            PageFindPosition end = {
                .command_index = (uint32_t) command_index,
                .text_offset = (uint32_t) offset
            };
            if (end.command_index >= start.command_index
                && end.command_index - start.command_index
                       <= PAGE_FIND_COMMAND_SPAN_LIMIT) {
                if (index->match_count == PAGE_FIND_MATCH_LIMIT) {
                    index->truncated = true;
                    return true;
                }
                PageFindMatch *match =
                    &index->matches[index->match_count++];
                *match = (PageFindMatch) {.start = start, .end = end};
                find_match_vertical_bounds(layout, match);
            }
            matched = prefix[matched - 1u];
        }
    }
    return true;
}

bool page_find_refresh_geometry(PageFindIndex *index,
                                const LayoutDocument *layout)
{
    if (index == NULL || layout == NULL || index->query[0] == '\0'
        || index->layout_text_fingerprint != layout->text_fingerprint)
        return false;
    for (size_t at = 0; at < index->match_count; at++)
        find_match_vertical_bounds(layout, &index->matches[at]);
    return true;
}

void page_find_clear(PageFindIndex *index)
{
    if (index == NULL) return;
    if (index->budget != NULL)
        budget_free(index->budget, index->matches);
    memset(index, 0, sizeof(*index));
}

bool page_find_select_nearest(PageFindIndex *index, int document_y)
{
    if (index == NULL || index->match_count == 0) return false;
    size_t selected = 0;
    for (size_t i = 0; i < index->match_count; i++) {
        if (index->matches[i].top >= document_y) {
            selected = i;
            break;
        }
    }
    index->selected = selected;
    return true;
}

bool page_find_move(PageFindIndex *index, int direction)
{
    if (index == NULL || direction == 0 || index->match_count == 0)
        return false;
    if (direction < 0) {
        index->wrapped = index->selected == 0;
        index->selected = index->selected == 0
            ? index->match_count - 1u : index->selected - 1u;
    } else {
        index->wrapped = index->selected + 1u == index->match_count;
        index->selected = (index->selected + 1u) % index->match_count;
    }
    return true;
}

const PageFindMatch *page_find_selected(const PageFindIndex *index)
{
    return index == NULL || index->selected >= index->match_count
        ? NULL : &index->matches[index->selected];
}

static void find_command_range(const PageFindMatch *match,
                               const DrawCommand *command,
                               size_t command_index, size_t *start,
                               size_t *end)
{
    *start = command_index == match->start.command_index
        ? match->start.text_offset : 0u;
    *end = command_index == match->end.command_index
        ? match->end.text_offset : command->text_length;
    if (*start > command->text_length) *start = command->text_length;
    if (*end > command->text_length) *end = command->text_length;
    if (*end < *start) *end = *start;
}

size_t page_find_match_rects(const PageFindIndex *index,
                             const LayoutDocument *layout,
                             size_t match_index, PageFindRect *rects,
                             size_t capacity)
{
    if (index == NULL || layout == NULL || rects == NULL || capacity == 0
        || match_index >= index->match_count) return 0;
    if (capacity > PAGE_FIND_RECT_LIMIT) capacity = PAGE_FIND_RECT_LIMIT;
    const PageFindMatch *match = &index->matches[match_index];
    if (match->start.command_index >= layout->count
        || match->end.command_index >= layout->count
        || match->end.command_index < match->start.command_index
        || match->end.command_index - match->start.command_index
               > PAGE_FIND_COMMAND_SPAN_LIMIT) return 0;
    size_t count = 0;
    for (size_t at = match->start.command_index;
         at <= match->end.command_index; at++) {
        const DrawCommand *command = &layout->commands[at];
        if (!find_text_command(command)) continue;
        size_t start = 0, end = 0;
        find_command_range(match, command, at, &start, &end);
        if (end <= start) continue;
        int width = command->width > 0 ? command->width : 1;
        int64_t first = (int64_t) width * (int64_t) start
                        / (int64_t) command->text_length;
        int64_t last = (int64_t) width * (int64_t) end
                       / (int64_t) command->text_length;
        int left = draw_command_text_rtl(command)
            ? command->x + width - (int) last
            : command->x + (int) first;
        int right = draw_command_text_rtl(command)
            ? command->x + width - (int) first
            : command->x + (int) last;
        if (right <= left) right = left + 1;
        PageFindRect rect = {
            .x = left,
            .y = command->y,
            .width = right - left,
            .height = command->height > 0 ? command->height : 1,
            .fixed = layout->command_flags != NULL
                && (layout->command_flags[at] & LAYOUT_COMMAND_FIXED) != 0
        };
        if (count != 0) {
            PageFindRect *previous = &rects[count - 1u];
            int previous_right = previous->x + previous->width;
            if (previous->y == rect.y && previous->height == rect.height
                && previous->fixed == rect.fixed
                && rect.x >= previous_right && rect.x - previous_right <= 2) {
                previous->width = rect.x + rect.width - previous->x;
                continue;
            }
        }
        if (count < capacity) rects[count++] = rect;
        else {
            PageFindRect *last_rect = &rects[capacity - 1u];
            int left_edge = rect.x < last_rect->x ? rect.x : last_rect->x;
            int top = rect.y < last_rect->y ? rect.y : last_rect->y;
            int right_edge = rect.x + rect.width;
            if (last_rect->x + last_rect->width > right_edge)
                right_edge = last_rect->x + last_rect->width;
            int bottom = rect.y + rect.height;
            if (last_rect->y + last_rect->height > bottom)
                bottom = last_rect->y + last_rect->height;
            last_rect->x = left_edge;
            last_rect->y = top;
            last_rect->width = right_edge - left_edge;
            last_rect->height = bottom - top;
            last_rect->fixed = last_rect->fixed && rect.fixed;
        }
    }
    return count;
}
