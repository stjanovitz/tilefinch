#ifndef TILEFINCH_STYLE_VALUES_INTERNAL_H
#define TILEFINCH_STYLE_VALUES_INTERNAL_H

/* Value-parser internals of the style subsystem: property-presence masks,
   the packed length/math tags and math programs, image-source and
   text-decoration encodings, the span helpers, Grid template storage, and
   the prototypes of the value parsers (style_math.c, style_values.c).
   Part of style_internal.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tilefinch/style.h"
#include "style_cssom.h"

#include <ctype.h>
#include <string.h>

typedef enum {
    STYLE_IMAGE_REFERRER_DEFAULT = 0,
    STYLE_IMAGE_REFERRER_NO_REFERRER,
    STYLE_IMAGE_REFERRER_NO_REFERRER_WHEN_DOWNGRADE,
    STYLE_IMAGE_REFERRER_ORIGIN,
    STYLE_IMAGE_REFERRER_ORIGIN_WHEN_CROSS_ORIGIN,
    STYLE_IMAGE_REFERRER_SAME_ORIGIN,
    STYLE_IMAGE_REFERRER_STRICT_ORIGIN,
    STYLE_IMAGE_REFERRER_STRICT_ORIGIN_WHEN_CROSS_ORIGIN,
    STYLE_IMAGE_REFERRER_UNSAFE_URL
} StyleImageReferrerPolicy;

typedef struct {
    char *base_url;
    uint8_t referrer_policy;
} StyleImageSourceContext;

struct StyleImageSources {
    StyleImageSourceContext *items;
    size_t count;
    size_t capacity;
    size_t retained_base_bytes;
};

/* ComputedStyle continues to expose a plain NUL-terminated pointer. The two
   bytes immediately before it retain the source slot and bounded length. */
typedef struct {
    uint8_t source_slot;
    uint8_t length;
    char reference[];
} StyleImageReference;

struct StyleMathProgram {
    uint16_t first;
    uint8_t count;
    uint8_t stack_depth;
    uint32_t hash;
};

#define STYLE_LENGTH_PERCENT_TAG INT32_C(0x20000000)
#define STYLE_LENGTH_PROGRAM_TAG INT32_C(0x40000000)
#define STYLE_LENGTH_PERCENT_BIAS INT32_C(0x10000000)
#define STYLE_LENGTH_PERCENT_END INT32_C(0x3fffffff)
#define STYLE_LENGTH_PROGRAM_END INT32_C(0x4000ffff)
#define STYLE_MATH_Q16 INT64_C(65536)
#define STYLE_MATH_SOURCE_CAPACITY 256u
/* style_resolve_value() buffers. A declaration whose var()-substituted
   value does not fit its buffer is dropped, so each parser resolves into
   the class of the longest value its property can take:
   - STYLE_CUSTOM_RESOLVED_CAPACITY (style.h): open-ended values, i.e.
     shorthands, lists, several components, functions (colours, images,
     shapes, transforms), strings and names;
   - STYLE_MATH_SOURCE_CAPACITY: one number, length or angle, which a math
     function may spell (the math compiler refuses longer source), or one
     of the property's keywords;
   - STYLE_KEYWORD_RESOLVED_CAPACITY: values made of keywords only (the
     longest, such as text-transform's `full-width full-size-kana
     capitalize`, are under 40 bytes), and comparisons against a keyword.
   Narrower named classes tied to storage (STYLE_GRID_AREA_NAME_CAPACITY,
   STYLE_RETAINED_VALUE_CAPACITY) document their own bound. These are
   stack buffers in leaf parsers reached through the property table, so a
   buffer is live only for its own declaration. */
#define STYLE_KEYWORD_RESOLVED_CAPACITY 96u
/* ComputedStyle.transform_scale_q14 is Q14 (16384 = 1.0) and clamps at
   255/64 (3.98x), the largest factor the retained display list draws. */
#define STYLE_TRANSFORM_SCALE_Q14_ONE 16384u
#define STYLE_TRANSFORM_SCALE_Q14_MAX 65280u

/* Property-presence masks for ComputedStyle/StyleDeclaration.  The low
   64 bits live in `mask`, the S2_* bits in `mask_high`. */
#define S_DISPLAY        (UINT64_C(1) << 0)
#define S_COLOR          (UINT64_C(1) << 1)
#define S_BACKGROUND     (UINT64_C(1) << 2)
#define S_FONT_SCALE     (UINT64_C(1) << 3)
#define S_LINE_HEIGHT    (UINT64_C(1) << 4)
#define S_MARGIN_TOP     (UINT64_C(1) << 5)
#define S_MARGIN_RIGHT   (UINT64_C(1) << 6)
#define S_MARGIN_BOTTOM  (UINT64_C(1) << 7)
#define S_MARGIN_LEFT    (UINT64_C(1) << 8)
#define S_PADDING_TOP    (UINT64_C(1) << 9)
#define S_PADDING_RIGHT  (UINT64_C(1) << 10)
#define S_PADDING_BOTTOM (UINT64_C(1) << 11)
#define S_PADDING_LEFT   (UINT64_C(1) << 12)
#define S_GAP            (UINT64_C(1) << 13)
#define S_FLEX_DIRECTION (UINT64_C(1) << 14)
#define S_FLEX_WRAP      (UINT64_C(1) << 15)
#define S_WIDTH          (UINT64_C(1) << 16)
#define S_VISIBILITY     (UINT64_C(1) << 17)
#define S_POSITION       (UINT64_C(1) << 18)
#define S_FONT_FAMILY    (UINT64_C(1) << 19)
#define S_FONT_WEIGHT    (UINT64_C(1) << 20)
#define S_FLEX_GROW      (UINT64_C(1) << 21)
#define S_FLEX_SHRINK    (UINT64_C(1) << 22)
#define S_FLEX_BASIS     (UINT64_C(1) << 23)
#define S_BORDER_TOP     (UINT64_C(1) << 24)
#define S_BORDER_RIGHT   (UINT64_C(1) << 25)
#define S_BORDER_BOTTOM  (UINT64_C(1) << 26)
#define S_BORDER_LEFT    (UINT64_C(1) << 27)
#define S_BORDER_COLOR   (UINT64_C(1) << 28)
#define S_HEIGHT         (UINT64_C(1) << 29)
#define S_MIN_WIDTH      (UINT64_C(1) << 30)
#define S_MIN_HEIGHT     (UINT64_C(1) << 31)
#define S_ALIGN_ITEMS    (UINT64_C(1) << 32)
#define S_MASK_IMAGE     (UINT64_C(1) << 33)
#define S_LIST_STYLE     (UINT64_C(1) << 34)
#define S_JUSTIFY_CONTENT (UINT64_C(1) << 35)
#define S_CONTENT         (UINT64_C(1) << 36)
#define S_TOP             (UINT64_C(1) << 37)
#define S_RIGHT           (UINT64_C(1) << 38)
#define S_BOTTOM          (UINT64_C(1) << 39)
#define S_LEFT            (UINT64_C(1) << 40)
#define S_FONT_STYLE      (UINT64_C(1) << 41)
#define S_WORD_SPACING    (UINT64_C(1) << 42)
#define S_VERTICAL_ALIGN  (UINT64_C(1) << 43)
#define S_BORDER_RADIUS   (UINT64_C(1) << 44)
#define S_OVERFLOW_X      (UINT64_C(1) << 45)
#define S_OVERFLOW_Y      (UINT64_C(1) << 46)
#define S_MAX_WIDTH       (UINT64_C(1) << 47)
#define S_MAX_HEIGHT      (UINT64_C(1) << 48)
#define S_BOX_SIZING      (UINT64_C(1) << 49)
#define S_Z_INDEX         (UINT64_C(1) << 50)
#define S_OPACITY         (UINT64_C(1) << 51)
#define S_TRANSFORM       (UINT64_C(1) << 52)
#define S_TEXT_ALIGN      (UINT64_C(1) << 53)
#define S_WHITE_SPACE     (UINT64_C(1) << 54)
#define S_ROW_GAP         (UINT64_C(1) << 55)
#define S_GRID_COLUMNS    (UINT64_C(1) << 56)
#define S_ASPECT_RATIO    (UINT64_C(1) << 57)
#define S_BACKGROUND_IMAGE (UINT64_C(1) << 58)
#define S_BACKGROUND_SIZE  (UINT64_C(1) << 59)
#define S_OBJECT_FIT       (UINT64_C(1) << 60)
#define S_OVERFLOW_WRAP    (UINT64_C(1) << 61)
#define S_WORD_BREAK       (UINT64_C(1) << 62)
#define S_LETTER_SPACING   (UINT64_C(1) << 63)

#define S2_ALIGN_SELF      (UINT64_C(1) << 0)
#define S2_ALIGN_CONTENT   (UINT64_C(1) << 1)
#define S2_ORDER           (UINT64_C(1) << 2)
#define S2_GRID_COLUMN_START (UINT64_C(1) << 3)
#define S2_GRID_COLUMN_END   (UINT64_C(1) << 4)
#define S2_GRID_COLUMN_SPAN  (UINT64_C(1) << 5)
#define S2_FLOAT             (UINT64_C(1) << 6)
#define S2_CLEAR             (UINT64_C(1) << 7)
#define S2_CLIP_RECT         (UINT64_C(1) << 8)
#define S2_BACKGROUND_POSITION (UINT64_C(1) << 9)
#define S2_TEXT_INDENT         (UINT64_C(1) << 10)
#define S2_TEXT_DECORATION     (UINT64_C(1) << 11)
#define S2_TEXT_UNDERLINE_OFFSET (UINT64_C(1) << 12)
#define S2_BACKGROUND_REPEAT     (UINT64_C(1) << 13)
#define S2_BOX_SHADOW            (UINT64_C(1) << 14)
#define S2_APPEARANCE            (UINT64_C(1) << 15)
#define S2_TEXT_TRANSFORM         (UINT64_C(1) << 16)
#define S2_GRID_UNIFORM_ROWS      (UINT64_C(1) << 17)
#define S2_GRID_ROW_START         (UINT64_C(1) << 18)
#define S2_GRID_ROW_END           (UINT64_C(1) << 19)
#define S2_GRID_ROW_SPAN          (UINT64_C(1) << 20)
#define S2_TABLE_LAYOUT           (UINT64_C(1) << 21)
#define S2_PERSPECTIVE            (UINT64_C(1) << 22)
#define S2_FILTER                 (UINT64_C(1) << 23)
#define S2_CONTAIN                (UINT64_C(1) << 24)
#define S2_WILL_CHANGE            (UINT64_C(1) << 25)
#define S2_POINTER_EVENTS         (UINT64_C(1) << 26)
#define S2_GRID_AUTO_FLOW         (UINT64_C(1) << 27)
#define S2_JUSTIFY_SELF           (UINT64_C(1) << 28)
#define S2_GRID_AUTO_COLUMNS      (UINT64_C(1) << 29)
#define S2_GRID_AUTO_ROWS         (UINT64_C(1) << 30)
#define S2_OVERFLOW_CLIP_MARGIN   (UINT64_C(1) << 31)
#define S2_TEXT_OVERFLOW          (UINT64_C(1) << 32)
#define S2_OUTLINE_COLOR          (UINT64_C(1) << 33)
#define S2_OUTLINE_STYLE          (UINT64_C(1) << 34)
#define S2_OUTLINE_OFFSET         (UINT64_C(1) << 35)
#define S2_CLIP_PATH              (UINT64_C(1) << 36)
#define S2_BACKGROUND_LAYERS      (UINT64_C(1) << 37)
#define S2_BORDER_COLLAPSE        (UINT64_C(1) << 38)
#define S2_BORDER_COLOR_TOP       (UINT64_C(1) << 39)
#define S2_BORDER_COLOR_RIGHT     (UINT64_C(1) << 40)
#define S2_BORDER_COLOR_BOTTOM    (UINT64_C(1) << 41)
#define S2_BORDER_COLOR_LEFT      (UINT64_C(1) << 42)
#define S2_BORDER_LINE_TOP        (UINT64_C(1) << 43)
#define S2_BORDER_LINE_RIGHT      (UINT64_C(1) << 44)
#define S2_BORDER_LINE_BOTTOM     (UINT64_C(1) << 45)
#define S2_BORDER_LINE_LEFT       (UINT64_C(1) << 46)
#define S2_WRITING_MODE           (UINT64_C(1) << 47)
#define S2_COUNTER_RESET          (UINT64_C(1) << 48)
#define S2_COUNTER_INCREMENT      (UINT64_C(1) << 49)
#define S2_COUNTER_SET            (UINT64_C(1) << 50)
#define S2_LIST_STYLE_TYPE        (UINT64_C(1) << 51)
#define S2_LIST_STYLE_POSITION    (UINT64_C(1) << 52)
#define S2_OBJECT_POSITION        (UINT64_C(1) << 53)
#define S2_DIRECTION              (UINT64_C(1) << 54)
#define S2_UNICODE_BIDI           (UINT64_C(1) << 55)
#define S2_SCROLLBAR_GUTTER       (UINT64_C(1) << 56)
#define S2_JUSTIFY_ITEMS          (UINT64_C(1) << 57)
#define S2_GRID_TEMPLATE_AREAS    (UINT64_C(1) << 58)
#define S2_GRID_AREA_NAME         (UINT64_C(1) << 59)
#define S2_BACKGROUND_BOX         (UINT64_C(1) << 60)
#define S2_MASK_POSITION          (UINT64_C(1) << 61)
#define S2_MASK_REPEAT            (UINT64_C(1) << 62)
#define S2_MASK_SIZE              (UINT64_C(1) << 63)
#define S2_BORDER_COLOR_ALL \
    (S2_BORDER_COLOR_TOP | S2_BORDER_COLOR_RIGHT \
     | S2_BORDER_COLOR_BOTTOM | S2_BORDER_COLOR_LEFT)
#define S2_BORDER_LINE_ALL \
    (S2_BORDER_LINE_TOP | S2_BORDER_LINE_RIGHT \
     | S2_BORDER_LINE_BOTTOM | S2_BORDER_LINE_LEFT)

#define STYLE_TEXT_DECORATION_OWN UINT16_C(0x0001)
#define STYLE_TEXT_DECORATION_ANCESTOR UINT16_C(0x0002)
/* Declaration-only CSS-wide value; resolved before publication. */
#define STYLE_TEXT_DECORATION_INHERIT UINT16_C(0x1000)
#define STYLE_TEXT_OFFSET_SHIFT 2
#define STYLE_TEXT_OFFSET_MASK UINT16_C(0x007c)
#define STYLE_ANCESTOR_TEXT_OFFSET_SHIFT 7
#define STYLE_ANCESTOR_TEXT_OFFSET_MASK UINT16_C(0x0f80)

#define S_MARGIN_ALL (S_MARGIN_TOP | S_MARGIN_RIGHT | S_MARGIN_BOTTOM | S_MARGIN_LEFT)
#define S_PADDING_ALL (S_PADDING_TOP | S_PADDING_RIGHT | S_PADDING_BOTTOM | S_PADDING_LEFT)
#define S_BORDER_ALL (S_BORDER_TOP | S_BORDER_RIGHT | S_BORDER_BOTTOM | S_BORDER_LEFT)

/* Small helpers shared across the style translation units. */

/* The looping span helpers are defined once, out of line, in
   style_spans.c: as static inline functions every style object laid out
   its own copy (seven of trim), spreading one hot routine across the
   I-cache. The short names below forward to them. span_equal and
   span_starts stay inline: their callers pass literals whose strlen()
   folds away. */
void style_trim(const char **text, size_t *length);
bool style_span_case_equal(const char *text, size_t length,
                           const char *wanted);
bool style_span_contains_text(const char *text, size_t length,
                              const char *wanted);

static inline bool span_equal(const char *text, size_t length,
                                    const char *wanted)
{
    return strlen(wanted) == length && memcmp(text, wanted, length) == 0;
}

/* snprintf(output, size, "%s", text) without the formatter: copies at most
   size - 1 bytes and always terminates (nothing when size is 0). */
static inline void style_copy_text(char *output, size_t size,
                                   const char *text)
{
    if (output == NULL || size == 0) return;
    size_t length = text == NULL ? 0 : strlen(text);
    if (length >= size) length = size - 1u;
    if (length != 0) memcpy(output, text, length);
    output[length] = '\0';
}

/* Whether the exact bytes of `wanted` occur anywhere in the span. */
static inline bool span_contains_text(const char *text, size_t length,
                                      const char *wanted)
{
    return style_span_contains_text(text, length, wanted);
}

/* ASCII case-insensitive equality of the span and `wanted`. */
static inline bool span_case_equal(const char *text, size_t length,
                                   const char *wanted)
{
    return style_span_case_equal(text, length, wanted);
}

static inline bool span_starts(const char *text, size_t length,
                                     const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return length >= wanted_length && memcmp(text, wanted, wanted_length) == 0;
}

/* Drops leading and trailing white space from the span. */
static inline void trim(const char **text, size_t *length)
{
    style_trim(text, length);
}

static inline bool name_character(char value)
{
    return isalnum((unsigned char) value) || value == '-' || value == '_'
           || (unsigned char) value >= 0x80u;
}

static inline unsigned style_text_offset_code(const ComputedStyle *style,
                                              unsigned shift, uint16_t mask)
{
    return style == NULL ? 0u
        : (unsigned) ((style->text_decoration_state & mask) >> shift);
}

static inline void style_set_text_offset_code(ComputedStyle *style,
                                              unsigned code, unsigned shift,
                                              uint16_t mask)
{
    if (style == NULL) return;
    style->text_decoration_state = (uint16_t) (
        (style->text_decoration_state & ~mask)
        | (((uint16_t) code << shift) & mask));
}

static inline void style_set_text_underline(ComputedStyle *style,
                                            bool underline)
{
    if (style == NULL) return;
    if (underline) {
        style->text_decoration_state |= STYLE_TEXT_DECORATION_OWN;
    } else {
        style->text_decoration_state &= ~STYLE_TEXT_DECORATION_OWN;
    }
}

static inline void style_set_ancestor_text_decoration(ComputedStyle *style,
                                                      bool underline,
                                                      unsigned offset_code)
{
    if (style == NULL) return;
    if (underline) {
        style->text_decoration_state |= STYLE_TEXT_DECORATION_ANCESTOR;
    } else {
        style->text_decoration_state &= ~STYLE_TEXT_DECORATION_ANCESTOR;
        offset_code = 0;
    }
    style_set_text_offset_code(style, offset_code,
                               STYLE_ANCESTOR_TEXT_OFFSET_SHIFT,
                               STYLE_ANCESTOR_TEXT_OFFSET_MASK);
}

static inline bool style_decode_text_offset(unsigned code, int *pixels)
{
    if (code == 0) return false;
    if (pixels != NULL) *pixels = (int) code - 16;
    return true;
}

typedef struct {
    uint8_t name_id;
    uint8_t row_start;
    uint8_t row_end;
    uint8_t column_start;
    uint8_t column_end;
} StyleGridAreaRect;

typedef struct {
    uint8_t rows;
    uint8_t columns;
    uint8_t area_count;
    uint8_t reserved;
    StyleGridAreaRect areas[STYLE_GRID_AREA_RECT_LIMIT];
} StyleGridAreaTemplate;

typedef struct {
    uint8_t track_count;
    uint8_t subgrid;
    uint8_t line_names[GRID_TRACK_REPEAT_LIMIT + 1]
                      [STYLE_GRID_LINE_NAMES_PER_LINE];
    uint8_t track_types[GRID_TRACK_REPEAT_LIMIT];
    uint16_t track_values[GRID_TRACK_REPEAT_LIMIT];
    uint16_t track_minimums[GRID_TRACK_REPEAT_LIMIT];
    uint8_t automatic_minimums[(GRID_TRACK_REPEAT_LIMIT + 7u) / 8u];
} StyleGridTrackTemplate;

#define STYLE_GRID_TRACK_BLOCK_SIZE 4u
#define STYLE_GRID_TRACK_BLOCK_COUNT \
    ((STYLE_GRID_TRACK_TEMPLATE_LIMIT + STYLE_GRID_TRACK_BLOCK_SIZE - 1u) \
     / STYLE_GRID_TRACK_BLOCK_SIZE)

struct StyleGridTrackStorage {
    /* Blocks preserve template addresses while keeping ordinary non-Grid
       stylesheets at zero track-storage cost. */
    StyleGridTrackTemplate **blocks;
    size_t count;
    size_t capacity;
};

static inline StyleGridTrackTemplate *style_grid_track_storage_slot(
    StyleGridTrackStorage *storage, size_t index)
{
    if (storage == NULL || index >= STYLE_GRID_TRACK_TEMPLATE_LIMIT) {
        return NULL;
    }
    size_t block_index = index / STYLE_GRID_TRACK_BLOCK_SIZE;
    if (storage->blocks == NULL || block_index >= storage->capacity) {
        return NULL;
    }
    StyleGridTrackTemplate *block = storage->blocks[block_index];
    return block == NULL
        ? NULL : &block[index % STYLE_GRID_TRACK_BLOCK_SIZE];
}

static inline const StyleGridTrackTemplate *
style_grid_track_storage_const_slot(
    const StyleGridTrackStorage *storage, size_t index)
{
    return style_grid_track_storage_slot(
        (StyleGridTrackStorage *) storage, index);
}

struct StyleGridAreas {
    uint8_t name_count;
    uint8_t template_count;
    uint8_t line_name_count;
    char names[STYLE_GRID_AREA_NAME_LIMIT][STYLE_GRID_AREA_NAME_CAPACITY];
    char line_names[STYLE_GRID_LINE_NAME_LIMIT][STYLE_GRID_AREA_NAME_CAPACITY];
    StyleGridAreaTemplate templates[STYLE_GRID_AREA_TEMPLATE_LIMIT];
};

_Static_assert(sizeof(StyleGridAreaRect) == 5,
               "named Grid area rectangles must remain compact");
_Static_assert(sizeof(StyleGridAreaTemplate) == 64,
               "named Grid templates must remain compact");
/* 24 tracks with up to four names per line: 226 bytes per template,
   allocated four at a time only by stylesheets that declare track lists (at
   most 31 templates, 6.9 KiB). */
_Static_assert(sizeof(StyleGridTrackTemplate) == 226,
               "Grid track templates must remain compact");
_Static_assert(sizeof(StyleGridAreas) == 4355,
               "optional Grid metadata must remain within its PSP budget");
/* CSS initial font size and the engine's used-font-size clamp (px). */
#define STYLE_DEFAULT_FONT_PX 16
#define STYLE_GENERATED_SCALAR_LENGTH UINT8_MAX
#define STYLE_FONT_MIN_PX 6
#define STYLE_FONT_MAX_PX 128

/* ComputedStyle is retained per cached node; its packing is a PSP memory
   commitment (see the field comments in tilefinch/style.h). */
#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(ComputedStyle) == 376,
               "ComputedStyle packing changed; update the PSP budget notes");
_Static_assert(sizeof(StyleRule) == 40,
               "prepared selector metadata must remain in StyleRule padding");
#else
_Static_assert(sizeof(ComputedStyle) == 344,
               "ComputedStyle PSP packing changed; remeasure the style budget");
_Static_assert(sizeof(StyleRule) == 36,
               "prepared selector metadata must remain in StyleRule padding");
#endif

/* Cross-translation-unit prototypes.  These functions were file-local
   statics before the style.c decomposition; they remain internal to the
   style subsystem. */

bool style_resolve_value(const Stylesheet *sheet, const char *text,
                         size_t length, char *output, size_t output_size,
                         unsigned depth);

/* style_math.c */
bool parse_style_length(Stylesheet *sheet, const char *text, size_t length,
                        StyleLength *value, bool *has_percent);
bool parse_dimension_length(Stylesheet *sheet, const char *text,
                            size_t length, StyleLength *value,
                            bool *has_percent);
void style_math_restore(Stylesheet *sheet, size_t program_count,
                        size_t instruction_count);
bool style_keyword(const Stylesheet *sheet, const char *text, size_t length,
                   const char *keyword);
bool style_math_resolve_instructions(const StyleMathInstruction *instructions,
                                     size_t count, uint8_t stack_depth,
                                     int reference, int *pixels);
bool style_math_resolve_number_thousandths(
    const Stylesheet *sheet, const char *text, size_t length,
    int *thousandths);
bool style_math_resolve_number_thousandths_in(
    const Stylesheet *sheet, const char *text, size_t length,
    int minimum, int maximum, int *thousandths);
bool style_math_resolve_number_thousandths_wide(
    const Stylesheet *sheet, const char *text, size_t length,
    int64_t *thousandths);

/* A size container condition (the @container prelude after its name) for a
   content box of width x height, with the media condition grammar; vw/vh
   lengths resolve against the viewport. A top-level comma separates
   alternatives. Unknown features and syntax are false. */
bool style_container_condition_matches(const char *text, size_t length,
                                       int width, int height,
                                       int viewport_width, int viewport_height);
bool style_math_resolve_media_length(const char *text, size_t length,
                                      int width, int height, int32_t *pixels_q8);
bool style_parse_letter_spacing(const Stylesheet *sheet, const char *text,
                               size_t length, int16_t *fixed);
int style_parse_length(const Stylesheet *sheet, const char *text,
                       size_t length, int fallback, bool *percent);

/* style_values.c */
bool style_length_is_auto(const Stylesheet *sheet, const char *text, size_t length);
uint64_t style_parse_border(
    const Stylesheet *sheet, const char *text, size_t length,
    int *width, uint32_t *color, uint8_t *alpha, unsigned *line_style,
    uint64_t edge_mask);
/* The one <line-width> keyword map (thin 1, medium 3, thick 5 px, as
   Chrome draws them), shared by border-width, outline-width and the
   border and outline shorthands; ASCII case-insensitive. */
bool style_line_width_keyword(const char *text, size_t length, int *width);
/* A border <line-style> keyword as STYLE_BORDER_*, or UINT_MAX. */
unsigned style_border_line_style(const char *text, size_t length);
bool style_set_border_color(
    Stylesheet *sheet, ComputedStyle *style, StyleBorderSide side,
    uint32_t color, uint8_t alpha);
bool style_copy_border_color(
    Stylesheet *sheet, ComputedStyle *target, const ComputedStyle *source,
    StyleBorderSide side);
bool style_parse_pair_with_auto(const Stylesheet *sheet, const char *text, size_t length, int values[2], bool automatic[2], bool percentages[2]);
bool style_parse_grid_line(const Stylesheet *sheet, const char *text, size_t length, int *line, int *span);
bool style_parse_grid_track(const Stylesheet *sheet, const char *text,
                            size_t length, uint8_t *type, unsigned *value,
                            unsigned *minimum);
bool style_parse_color_with_alpha(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color, uint8_t *alpha);
/* The degrees in one `unit` (deg, grad, rad or turn, ASCII
   case-insensitive); false for any other unit. */
bool style_angle_unit_degrees(const char *unit, size_t length, double *scale);
/* A CSS <angle> in degrees (finite, not normalized): a number and one of
   those units, surrounding whitespace allowed. A unitless zero is an angle
   only where `unitless_zero` (transform functions, gradients, rotate). */
bool style_parse_angle_degrees(const char *text, size_t length,
                               bool unitless_zero, double *degrees);
/* The rotate and scale properties as the retained display list applies
   them (NUL-terminated values): quarter turns about z, and one uniform
   factor in Q14. @supports asks the same functions. */
bool style_rotate_value_quadrants(const char *value, uint8_t *quadrants);
bool style_scale_value_q14(const char *value, uint16_t *scale_q14);
int style_font_size_from_fixed(int fixed, uint8_t *fraction);
const char *style_store_generated_text(Stylesheet *sheet, const char *text, size_t length);
uint8_t style_store_counter_operations(
    Stylesheet *sheet, const StyleCounterOperations *operations);
const StyleCounterOperations *style_counter_operations(
    const Stylesheet *sheet, uint8_t id);
bool style_parse_padding_pair(Stylesheet *sheet, const char *text, size_t length, int *first, int *second);
bool style_parse_image_url(Stylesheet *sheet, const char *text, size_t length, const char **output);
/* Shared paren/quote-aware top-level list iterator; *cursor starts at zero. */
bool style_next_top_level(const char *text, size_t length, char separator, size_t *cursor, size_t *start, size_t *end);
/* The same scan for whitespace-separated shorthand components: the next
   non-empty token outside parentheses and quotes, so `rgb(0 0 0 / 50%)` or
   `calc(1px + 1px)` stays one token. *cursor starts at zero; false once no
   token remains. */
bool style_next_top_level_token(const char *text, size_t length, size_t *cursor, size_t *start, size_t *end);
/* Bounded `linear-gradient()`; false for anything outside that grammar,
   which makes the declaration invalid and triggers the fallback path. */
bool style_parse_gradient(const Stylesheet *sheet, const char *text, size_t length, StyleGradient *gradient);
bool style_parse_box_shadow(const Stylesheet *sheet, const char *text, size_t length, StyleBoxShadow *shadows, uint8_t *count);
bool style_parse_text_shadow(const Stylesheet *sheet, const char *text,
                             size_t length, StyleBoxShadow *shadows,
                             uint8_t *count);
bool style_parse_font_weight_component(const char *text, size_t length, uint16_t *weight);
bool stylesheet_select_image_source_slot(Stylesheet *sheet, uint8_t slot);
bool stylesheet_current_image_source_slot(Stylesheet *sheet, uint8_t *slot);
void style_parse_transform_translation(Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_text_underline_offset(const Stylesheet *sheet, const char *text, size_t length, unsigned *offset_code);
bool style_parse_text_decoration_underline(const Stylesheet *sheet, const char *text, size_t length, bool *underline, bool *inherit);
uint64_t style_parse_padding_box(Stylesheet *sheet, const char *text, size_t length, StyleEdges *edges);
uint64_t style_parse_margin_box(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
int style_parse_line_height(const Stylesheet *sheet, const char *text, size_t length);
/* Returns false when the value could not be resolved; the caller must
   then drop the declaration and leave the cascaded track list in force. */
bool style_parse_grid_columns(Stylesheet *sheet, const char *text,
                              size_t length, ComputedStyle *style);
bool style_parse_grid_rows(Stylesheet *sheet, const char *text,
                           size_t length, ComputedStyle *style);
bool style_parse_grid_line_or_name(
    Stylesheet *sheet, const char *text, size_t length,
    int *line, int *span, uint8_t *name_id);
bool style_parse_grid_template_areas(
    Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_grid_area_name(
    Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
int style_parse_font_size(const Stylesheet *sheet, const char *text, size_t length, uint8_t *unit, uint8_t *fraction);
bool style_parse_color(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color);
bool style_parse_background_size(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_background_shorthand_color(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color, uint8_t *alpha, bool *transparent, bool *current_color);
bool style_parse_background_position(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_background_shorthand_image(Stylesheet *sheet,
                                            const char *text, size_t length,
                                            ComputedStyle *style,
                                            bool reset_geometry);
bool style_parse_background_shorthand_position(
    const Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style);
bool style_parse_mask_image_layers(Stylesheet *sheet, const char *text,
                                   size_t length, ComputedStyle *style);
bool style_parse_mask_shorthand(Stylesheet *sheet, const char *text,
                                size_t length, ComputedStyle *style);
bool style_parse_mask_layer_size(Stylesheet *sheet, const char *text,
                                 size_t length, ComputedStyle *style);
bool style_parse_mask_layer_position(Stylesheet *sheet, const char *text,
                                     size_t length, ComputedStyle *style);
bool style_parse_background_layer_position(
    Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style);
bool style_parse_background_layer_position_axis(
    Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style, bool horizontal);
bool style_parse_background_layer_size(
    Stylesheet *sheet, const char *text, size_t length,
    ComputedStyle *style);
bool style_parse_mask_layer_repeat(Stylesheet *sheet, const char *text,
                                   size_t length, ComputedStyle *style);
bool style_parse_background_box(Stylesheet *sheet, const char *text,
                                size_t length, ComputedStyle *style,
                                bool set_origin, bool set_clip);
typedef enum {
    STYLE_PAINT_INTERN_INVALID,
    STYLE_PAINT_INTERN_RETAINED,
    STYLE_PAINT_INTERN_LIMIT,
    STYLE_PAINT_INTERN_OOM
} StylePaintInternResult;
StylePaintInternResult style_intern_paint_stack(
    Stylesheet *sheet, const StylePaintStack *stack, uint8_t *id);
StylePaintInternResult style_intern_text_shadow(
    Stylesheet *sheet, const StylePaintStack *source, uint8_t *id);
StylePaintStack style_paint_stack_copy(
    const Stylesheet *sheet, const ComputedStyle *style);
bool style_apply_paint_stack(
    Stylesheet *sheet, ComputedStyle *style, const StylePaintStack *stack,
    bool *retained);
bool style_noninherited_length_is_auto(const Stylesheet *sheet, const char *text, size_t length);
bool style_font_family_component(const Stylesheet *sheet, const char *text, size_t length, FontFamily *generic);
bool style_font_family_append_codepoint(char *output, size_t capacity, size_t *written, unsigned codepoint);
size_t style_decode_generated_text(const char *value, char *output, size_t capacity);
bool style_css_hex_digit(unsigned char character, unsigned *value);
bool style_canonical_font_family_name(const char *text, size_t length, char output[ STYLE_WEB_FONT_NAME_CAPACITY]);
size_t style_append_css_codepoint(char *output, size_t length, size_t capacity, unsigned codepoint);

#endif /* TILEFINCH_STYLE_VALUES_INTERNAL_H */
