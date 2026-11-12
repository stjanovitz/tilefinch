#ifndef TILEFINCH_TEXT_BIDI_H
#define TILEFINCH_TEXT_BIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"

/* Paragraph analysis is deliberately smaller than the document limits. A
   larger paragraph degrades to logical-order text instead of growing scratch
   storage or monopolising the PSP browser thread. */
#define TEXT_BIDI_PARAGRAPH_BYTE_LIMIT 8192u
#define TEXT_BIDI_CODEPOINT_LIMIT 2048u
#define TEXT_BIDI_RUN_LIMIT 256u
/* One inline flow may cross explicit Unicode paragraph separators. */
#define TEXT_BIDI_SEGMENT_LIMIT 32u

typedef enum {
    TEXT_BIDI_BASE_AUTO_LTR = 0,
    TEXT_BIDI_BASE_LTR,
    TEXT_BIDI_BASE_RTL,
    TEXT_BIDI_BASE_AUTO_RTL
} TextBidiBase;

typedef enum {
    TEXT_BIDI_OK = 0,
    TEXT_BIDI_NOT_NEEDED,
    TEXT_BIDI_MALFORMED,
    TEXT_BIDI_LIMIT_EXCEEDED,
    TEXT_BIDI_OUT_OF_MEMORY
} TextBidiStatus;

typedef struct TextBidiParagraph TextBidiParagraph;

/* shaped_codepoint of a unit whose letter a ligature has absorbed: a word
   joiner, which every text path already draws as nothing at zero width. */
#define TEXT_BIDI_NO_GLYPH 0x2060u

typedef struct {
    uint32_t codepoint;
    uint32_t shaped_codepoint;
    uint32_t byte_offset;
    uint8_t byte_length;
    uint8_t level;
    uint8_t cluster_extender;
} TextBidiUnit;

/* ASCII and ordinary LTR Unicode return false without allocating. Directional
   controls return true even when no RTL letter is present. */
bool text_bidi_maybe_needed(const char *text, size_t length);

TextBidiParagraph *text_bidi_paragraph_create(
    Budget *budget, const char *text, size_t length, TextBidiBase base,
    TextBidiStatus *status);
void text_bidi_paragraph_destroy(TextBidiParagraph *paragraph);

size_t text_bidi_paragraph_count(const TextBidiParagraph *paragraph);
unsigned text_bidi_paragraph_level(const TextBidiParagraph *paragraph);
const TextBidiUnit *text_bidi_paragraph_units(
    const TextBidiParagraph *paragraph);

/* Emits logical unit indices in line visual order. The line range uses unit
   indices, not UTF-8 byte offsets. Every successful call emits line_count
   entries exactly once. */
bool text_bidi_line_visual_order(
    TextBidiParagraph *paragraph, size_t line_start, size_t line_count,
    uint16_t *visual_to_logical, size_t capacity, size_t *run_count);

/* Appends a line to output at *used, as UTF-8 in display order and shaped, for
   a painter that draws left to right and cannot shape. A line feed and a letter
   absorbed by a ligature write nothing. At most TEXT_BIDI_LINE_UNIT_LIMIT units;
   false if the line is longer or does not fit, with the terminator not written. */
#define TEXT_BIDI_LINE_UNIT_LIMIT 256u
bool text_bidi_line_append_utf8(
    TextBidiParagraph *paragraph, size_t line_start, size_t line_count,
    char *output, size_t capacity, size_t *used);

#endif
