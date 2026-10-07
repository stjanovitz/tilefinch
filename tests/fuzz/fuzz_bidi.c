/* libFuzzer target: Unicode bidi paragraph analysis and line reordering
   (src/text_bidi.c over SheenBidi). Byte 0 picks the base direction and
   line-breaking pattern; the rest is UTF-8 text (malformed allowed). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilefinch/budget.h"
#include "tilefinch/text_bidi.h"

#define MIB (1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 2 * TEXT_BIDI_PARAGRAPH_BYTE_LIMIT) return 0;
    unsigned mode = data[0];
    size_t length = size - 1;
    char *text = malloc(length == 0 ? 1 : length);
    if (text == NULL) return 0;
    if (length != 0) memcpy(text, data + 1, length);

    Budget budget;
    budget_init(&budget, 4u * MIB);
    (void) text_bidi_maybe_needed(text, length);
    TextBidiStatus status = TEXT_BIDI_OK;
    TextBidiParagraph *paragraph = text_bidi_paragraph_create(
        &budget, text, length, (TextBidiBase) (mode & 3u), &status);
    if (paragraph != NULL) {
        size_t count = text_bidi_paragraph_count(paragraph);
        (void) text_bidi_paragraph_level(paragraph);
        const TextBidiUnit *units = text_bidi_paragraph_units(paragraph);
        for (size_t i = 0; i < count; i++) {
            if (units[i].byte_offset > length
                || units[i].byte_length > length - units[i].byte_offset)
                abort();
        }
        uint16_t *order = malloc(sizeof(uint16_t) * (count + 1));
        size_t line = 1 + (mode >> 2);
        for (size_t start = 0; order != NULL && start < count;
             start += line) {
            size_t n = count - start < line ? count - start : line;
            size_t runs = 0;
            if (text_bidi_line_visual_order(paragraph, start, n, order, n,
                                            &runs)) {
                for (size_t i = 0; i < n; i++)
                    if (order[i] < start || order[i] >= start + n) abort();
            }
        }
        size_t runs = 0;
        if (order != NULL)
            (void) text_bidi_line_visual_order(paragraph, 0, count, order,
                                               count, &runs);
        free(order);
        text_bidi_paragraph_destroy(paragraph);
    }
    if (budget.current != 0) {
        fprintf(stderr, "fuzz_bidi: budget leak of %zu bytes\n",
                budget.current);
        abort();
    }
    free(text);
    return 0;
}
