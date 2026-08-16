#ifndef TILEFINCH_STYLE_INTERNAL_H
#define TILEFINCH_STYLE_INTERNAL_H

/* Shared internals for the style subsystem translation units
   (style.c, style_math.c, style_values.c, style_properties.c,
   style_sheet.c, style_match.c, style_resolve.c).  Nothing here is part
   of the public tilefinch API; consumers must keep including
   tilefinch/style.h only. */

#include "tilefinch/style.h"

enum {
    STYLE_MODERN_USER_SELECT = 1u << 0,
    STYLE_MODERN_TOUCH_ACTION = 1u << 1,
    STYLE_MODERN_TEXT_SIZE_ADJUST = 1u << 2,
    STYLE_MODERN_RESIZE = 1u << 3,
    STYLE_MODERN_TEXT_WRAP = 1u << 4,
    STYLE_MODERN_TRANSLATE = 1u << 5,
    STYLE_MODERN_ROTATE = 1u << 6,
    STYLE_MODERN_SCALE = 1u << 7,
    STYLE_MODERN_ISOLATION = 1u << 8,
    STYLE_MODERN_LOGICAL_RADIUS = 1u << 9
};
#include "style_paint_internal.h"

#include <ctype.h>
#include <string.h>

#define budget_malloc(b, s) budget_malloc_category((b), BUDGET_CATEGORY_STYLE, (s))
#define budget_calloc(b, n, s) budget_calloc_category((b), BUDGET_CATEGORY_STYLE, (n), (s))
#define budget_realloc(b, p, s) budget_realloc_category((b), BUDGET_CATEGORY_STYLE, (p), (s))

#define STYLE_SELECTOR_CHUNK_MIN_BYTES 1024u
#define STYLE_SELECTOR_CHUNK_MAX_BYTES (16u * 1024u)

bool stylesheet_extend_selector_program(
    Stylesheet *sheet, const uint16_t *prefix_offsets,
    size_t prefix_offset_count, size_t *reused_rules,
    size_t *compiled_rules);

struct StyleTextChunk {
    StyleTextChunk *next;
    size_t used;
    size_t capacity;
    char data[];
};

typedef struct {
    char *references[STYLE_WEB_FONT_SOURCE_LIMIT];
    char *source_bases[STYLE_WEB_FONT_SOURCE_LIMIT];
    char source_referrer_policies[STYLE_WEB_FONT_SOURCE_LIMIT]
                                 [STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY];
    size_t source_count;
} StyleWebFontFaceSource;

typedef struct {
    char name[STYLE_WEB_FONT_NAME_CAPACITY];
    StyleWebFontFaceSource regular;
    StyleWebFontFaceSource bold;
} StyleWebFontFamily;

struct StyleWebFonts {
    WebFontSet loaded;
    StyleWebFontFamily families[TILEFINCH_WEB_FONT_FAMILY_LIMIT];
    size_t family_count;
    size_t retained_source_bytes;
    StylesheetWebFontStats stats;
};

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

enum {
    STYLE_MATH_PUSH_AFFINE,
    STYLE_MATH_ADD,
    STYLE_MATH_SUBTRACT,
    STYLE_MATH_SCALE,
    STYLE_MATH_DIVIDE,
    STYLE_MATH_MINIMUM,
    STYLE_MATH_MAXIMUM,
    STYLE_MATH_CLAMP
};

struct StyleMathInstruction {
    int32_t a;
    int32_t b;
    uint8_t op;
    uint8_t reserved[3];
};

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
#define STYLE_MATH_NODE_LIMIT 32u
#define STYLE_MATH_STACK_LIMIT 8u
#define STYLE_MATH_NESTING_LIMIT 8u
#define STYLE_MATH_ARGUMENT_LIMIT 8u
#define STYLE_MATH_PROGRAM_LIMIT 256u
#define STYLE_MATH_INSTRUCTION_LIMIT 2048u

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
#define STYLE_TEXT_OFFSET_SHIFT 2
#define STYLE_TEXT_OFFSET_MASK UINT16_C(0x007c)
#define STYLE_ANCESTOR_TEXT_OFFSET_SHIFT 7
#define STYLE_ANCESTOR_TEXT_OFFSET_MASK UINT16_C(0x0f80)

#define S_MARGIN_ALL (S_MARGIN_TOP | S_MARGIN_RIGHT | S_MARGIN_BOTTOM | S_MARGIN_LEFT)
#define S_PADDING_ALL (S_PADDING_TOP | S_PADDING_RIGHT | S_PADDING_BOTTOM | S_PADDING_LEFT)
#define S_BORDER_ALL (S_BORDER_TOP | S_BORDER_RIGHT | S_BORDER_BOTTOM | S_BORDER_LEFT)

/* Small helpers shared across the style translation units. */

static inline bool span_equal(const char *text, size_t length,
                                    const char *wanted)
{
    return strlen(wanted) == length && memcmp(text, wanted, length) == 0;
}

static inline bool span_case_equal(const char *text, size_t length,
                                         const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    if (wanted_length != length) return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char) text[i])
            != tolower((unsigned char) wanted[i])) return false;
    }
    return true;
}

static inline bool span_starts(const char *text, size_t length,
                                     const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    return length >= wanted_length && memcmp(text, wanted, wanted_length) == 0;
}

static inline void trim(const char **text, size_t *length)
{
    while (*length != 0 && isspace((unsigned char) **text)) {
        (*text)++;
        (*length)--;
    }
    while (*length != 0 && isspace((unsigned char) (*text)[*length - 1])) {
        (*length)--;
    }
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

typedef enum {
    STYLE_MATH_TYPE_NUMBER,
    STYLE_MATH_TYPE_LENGTH
} StyleMathType;

typedef enum {
    STYLE_MATH_NODE_VALUE,
    STYLE_MATH_NODE_ADD,
    STYLE_MATH_NODE_SUBTRACT,
    STYLE_MATH_NODE_SCALE,
    STYLE_MATH_NODE_DIVIDE,
    STYLE_MATH_NODE_MINIMUM,
    STYLE_MATH_NODE_MAXIMUM,
    STYLE_MATH_NODE_CLAMP
} StyleMathNodeOp;

typedef struct {
    int64_t a;
    int64_t b;
    uint8_t op;
    uint8_t type;
    uint8_t left;
    uint8_t right;
    uint8_t third;
    bool folded;
} StyleMathNode;

typedef struct {
    const Stylesheet *sheet;
    const char *text;
    size_t length;
    size_t at;
    unsigned nesting;
    StyleMathNode nodes[STYLE_MATH_NODE_LIMIT];
    size_t node_count;
    int em_basis;
    int rem_basis;
    int ch_basis;
    bool failed;
} StyleMathParser;

typedef struct {
    StyleMathInstruction instructions[STYLE_MATH_NODE_LIMIT];
    size_t count;
    uint8_t stack_depth;
    bool has_percent;
} StyleMathCandidate;

enum {
    CASCADE_AUTHOR_NORMAL,
    CASCADE_USER_NORMAL,
    CASCADE_AUTHOR_IMPORTANT,
    CASCADE_USER_IMPORTANT
};

#define STYLE_RULE_INDEX_EMPTY UINT32_MAX
#define STYLE_RULE_INDEX_MAX_SOURCES 34

struct StyleRuleIndexBucket {
    uint64_t hash;
    uint32_t representative;
    uint32_t first;
    uint32_t count;
    uint32_t fill;
};

#define STYLE_TOKEN_BLOOM_WORDS 2u

struct StyleRuleFilter {
    struct StyleTokenBloom {
        uint32_t words[STYLE_TOKEN_BLOOM_WORDS];
    } compound, ancestors;
};

typedef struct {
    uint32_t at;
    uint32_t end;
} StyleRuleIndexSource;

typedef enum {
    STYLE_SELECTOR_END,
    STYLE_SELECTOR_TAG,
    STYLE_SELECTOR_TAG_ID,
    STYLE_SELECTOR_CLASS,
    STYLE_SELECTOR_ID,
    STYLE_SELECTOR_COMPOUND,
    STYLE_SELECTOR_PARENT,
    STYLE_SELECTOR_ANCESTOR,
    STYLE_SELECTOR_ADJACENT,
    STYLE_SELECTOR_GENERAL_SIBLING
} StyleSelectorOpcode;

#define STYLE_SELECTOR_PROGRAM_DEPTH_LIMIT 32u
#define STYLE_SELECTOR_IDENTIFIER_CAPACITY 192u

struct StyleSelectorInstruction {
    uint16_t text_offset;
    uint8_t text_length;
    uint8_t opcode;
};


_Static_assert(sizeof(StyleSelectorInstruction) == 4,
               "selector instructions must remain PSP-compact");

typedef struct StyleTokenBloom StyleTokenBloom;

static inline StyleTokenBloom style_token_bloom_empty(void)
{
    return (StyleTokenBloom) {{0, 0}};
}

static inline void style_token_bloom_merge(StyleTokenBloom *destination,
                                           StyleTokenBloom source)
{
    for (size_t i = 0; i < STYLE_TOKEN_BLOOM_WORDS; i++) {
        destination->words[i] |= source.words[i];
    }
}

static inline bool style_token_bloom_empty_value(StyleTokenBloom bloom)
{
    return (bloom.words[0] | bloom.words[1]) == 0;
}

static inline bool style_token_bloom_missing(StyleTokenBloom required,
                                             StyleTokenBloom available)
{
    for (size_t i = 0; i < STYLE_TOKEN_BLOOM_WORDS; i++) {
        if ((required.words[i] & ~available.words[i]) != 0) return true;
    }
    return false;
}

static inline StyleTokenBloom style_compound_token_bloom(
    StyleSelectorOpcode opcode, const char *text, size_t length)
{
    uint32_t hash = UINT32_C(2166136261) ^ (uint32_t) opcode;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (unsigned char) text[i]) * UINT32_C(16777619);
    }
    StyleTokenBloom bloom = style_token_bloom_empty();
    for (size_t word = 0; word < STYLE_TOKEN_BLOOM_WORDS; word++) {
        uint32_t mixed = hash + UINT32_C(0x9e3779b9) * (uint32_t) word;
        mixed ^= mixed >> 16;
        mixed *= UINT32_C(0x7feb352d);
        mixed ^= mixed >> 15;
        bloom.words[word] = UINT32_C(1) << (mixed & 31u);
    }
    return bloom;
}

static inline StyleTokenBloom style_compound_tag_id_bloom(uintptr_t tag_id)
{
    uint32_t value = (uint32_t) tag_id ^ UINT32_C(0x9e3779b9);
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    StyleTokenBloom bloom = style_token_bloom_empty();
    for (size_t word = 0; word < STYLE_TOKEN_BLOOM_WORDS; word++) {
        uint32_t mixed = value + UINT32_C(0x85ebca6b) * (uint32_t) word;
        mixed ^= mixed >> 13;
        bloom.words[word] = UINT32_C(1) << (mixed & 31u);
    }
    return bloom;
}

bool style_selector_matches_profiled(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *selector,
                                     size_t selector_length);
bool style_selector_matches_prepared(const Stylesheet *sheet,
                                     lxb_dom_node_t *node,
                                     const char *selector,
                                     size_t selector_length,
                                     size_t rightmost_compound_offset);
bool style_rule_selector_matches(const Stylesheet *sheet,
                                 size_t rule_index,
                                 lxb_dom_node_t *node);
/* Returns false only for an allocation failure. Invalid or capacity-bounded
   queries are ignored with a zero output id. */
bool style_register_container_query(Stylesheet *sheet,
                                    const char *prelude, size_t length,
                                    uint8_t parent_query,
                                    uint8_t *query_id);
bool style_container_query_matches(const Stylesheet *sheet, uint8_t query,
                                   lxb_dom_node_t *node);
void style_container_units_for_node(Stylesheet *sheet,
                                    lxb_dom_node_t *node);
void style_relative_selector_cache_begin(Stylesheet *sheet);
void style_relative_selector_cache_end(Stylesheet *sheet);
bool stylesheet_prepare_focus_rule_index(Stylesheet *sheet);

typedef struct {
    lxb_dom_node_t *node;
    const char *tag;
    size_t tag_length;
    const char *id;
    size_t id_length;
    const char *classes;
    size_t classes_length;
    uintptr_t tag_id;
} StyleMatchSubject;

void style_match_subject_prepare(lxb_dom_node_t *node,
                                 StyleMatchSubject *subject);
bool style_rule_selector_matches_subject(
    const Stylesheet *sheet, size_t rule_index, lxb_dom_node_t *node,
    const StyleMatchSubject *subject);

struct StyleResolveScratch {
    /* Image-source provenance for the declarations currently being
       parsed (external stylesheet base URL / referrer policy / slot). */
    const char *current_image_source_base;
    const char *current_image_source_referrer_policy;
    uint8_t current_image_source_slot;
    /* The node currently being resolved and its font-relative bases,
       consulted by var()/em/rem reparsing. */
    lxb_dom_node_t *resolution_node;
    PseudoElement resolution_pseudo;
    int font_resolution_parent;
    int font_resolution_inherited;
    int font_resolution_root;
    bool font_resolution_active;
    bool font_resolution_locked;
    int font_ch_basis;
    bool font_ch_basis_active;
    bool font_ch_pending;
    bool font_relative_applied;
    bool non_font_relative_applied;
    bool logical_applied;
    bool logical_axes_locked;
    bool logical_axes_mismatch;
    uint8_t logical_axes;
    /* Transient and owned by one layout build, never by the retained sheet. */
    struct StyleVariableCache *variable_cache;
    struct StyleContainerState *container_states;
    struct StyleContainerMatchCacheEntry *container_match_cache;
    size_t container_state_capacity;
    size_t container_state_count;
    size_t container_match_cache_capacity;
    int container_inline_basis;
    int container_block_basis;
    bool container_basis_active;
};

#define STYLE_CONTAINER_QUERY_LIMIT 63u
#define STYLE_CONTAINER_NAME_LIMIT 32u
#define STYLE_CONTAINER_QUERY_TEXT_CAPACITY 128u
#define STYLE_CONTAINER_NAME_CAPACITY 48u
#define STYLE_CONTAINER_WALK_LIMIT 64u

typedef struct {
    char condition[STYLE_CONTAINER_QUERY_TEXT_CAPACITY];
    uint32_t name_bit;
    uint8_t parent;
    uint8_t condition_length;
    uint8_t needs_inline_size;
    uint8_t needs_block_size;
} StyleContainerQuery;

struct StyleConditionalQueries {
    StyleContainerQuery queries[STYLE_CONTAINER_QUERY_LIMIT];
    char names[STYLE_CONTAINER_NAME_LIMIT][STYLE_CONTAINER_NAME_CAPACITY];
    uint8_t query_count;
    uint8_t name_count;
};

typedef struct StyleContainerState {
    lxb_dom_node_t *node;
    uint32_t name_bits;
    int content_width;
    int content_height;
    uint8_t type;
    bool occupied;
} StyleContainerState;

typedef struct StyleContainerMatchCacheEntry {
    lxb_dom_node_t *node;
    uint8_t query;
    bool matched;
    bool occupied;
} StyleContainerMatchCacheEntry;

enum {
    STYLE_CONTAINER_TYPE_NONE,
    STYLE_CONTAINER_TYPE_INLINE_SIZE,
    STYLE_CONTAINER_TYPE_SIZE
};

/* Selectors are bounded to 191 bytes, so the upper byte of the prepared
   rightmost-compound offset can carry a one-based @container query id
   without growing StyleRule on the PSP. */
#define STYLE_RULE_COMPOUND_OFFSET_MASK UINT16_C(0x00ff)
#define STYLE_RULE_CONTAINER_QUERY_SHIFT 8u

static inline uint8_t style_rule_container_query(const StyleRule *rule)
{
    return rule == NULL ? 0 : (uint8_t) (
        rule->rightmost_compound_offset >> STYLE_RULE_CONTAINER_QUERY_SHIFT);
}

static inline size_t style_rule_rightmost_compound(const StyleRule *rule)
{
    return rule == NULL ? 0
        : rule->rightmost_compound_offset & STYLE_RULE_COMPOUND_OFFSET_MASK;
}

static inline void style_rule_set_container_query(StyleRule *rule,
                                                  uint8_t query)
{
    if (rule == NULL) return;
    rule->rightmost_compound_offset = (uint16_t) (
        (rule->rightmost_compound_offset & STYLE_RULE_COMPOUND_OFFSET_MASK)
        | ((uint16_t) query << STYLE_RULE_CONTAINER_QUERY_SHIFT));
}

#define STYLE_DEFERRED_FONT_RELATIVE UINT16_C(1)
#define STYLE_DEFERRED_NON_FONT_RELATIVE UINT16_C(2)
#define STYLE_DEFERRED_LOGICAL UINT16_C(4)
#define STYLE_DEFERRED_DYNAMIC UINT16_C(8)
#define STYLE_DEFERRED_CH UINT16_C(16)

struct StyleDeferredInstruction {
    uint32_t value_offset;
    uint16_t value_length;
    uint8_t property_index;
    uint8_t important;
};

_Static_assert(sizeof(StyleDeferredInstruction) == 8,
               "deferred declaration instructions must remain PSP-compact");

struct StyleRevertRuleMask {
    uint64_t mask;
    uint64_t mask_high;
    uint32_t order;
};

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
_Static_assert(sizeof(StyleGridTrackTemplate) == 88,
               "Grid track templates must remain compact");
_Static_assert(sizeof(StyleGridAreas) == 3555,
               "optional Grid metadata must remain within its PSP budget");

/* CSS initial font size and the engine's used-font-size clamp (px). */
#define STYLE_DEFAULT_FONT_PX 16
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

/* Tracing.  Mirrors the layout subsystem: environment is read once per
   stylesheet build; TILEFINCH_NO_TRACE compiles the trace bodies out. */
#ifndef TILEFINCH_NO_TRACE
#define TILEFINCH_TRACE_COMPILED_IN 1
#else
#define TILEFINCH_TRACE_COMPILED_IN 0
#endif

#define STYLE_TRACE_PAINT  (UINT8_C(1) << 0)
#define STYLE_TRACE_LAYOUT (UINT8_C(1) << 1)

#define STYLE_TRACE(sheet, FLAG) \
    (TILEFINCH_TRACE_COMPILED_IN != 0 \
     && ((sheet)->trace_flags & STYLE_TRACE_##FLAG) != 0)

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
bool style_math_identifier_equal(const char *text, size_t length,
                                 const char *wanted);
bool style_math_candidate(const Stylesheet *sheet, const char *text,
                          size_t length, StyleMathParser *parser,
                          StyleMathCandidate *candidate, int *root_output,
                          int em_basis, int rem_basis, int ch_basis);
int style_parse_length(const Stylesheet *sheet, const char *text,
                       size_t length, int fallback, bool *percent);


/* style_values.c */
bool style_length_is_auto(const Stylesheet *sheet, const char *text, size_t length);
uint64_t style_parse_border(
    const Stylesheet *sheet, const char *text, size_t length,
    int *width, uint32_t *color, uint8_t *alpha, unsigned *line_style,
    uint64_t edge_mask);
bool style_set_border_color(
    Stylesheet *sheet, ComputedStyle *style, StyleBorderSide side,
    uint32_t color, uint8_t alpha);
bool style_copy_border_color(
    Stylesheet *sheet, ComputedStyle *target, const ComputedStyle *source,
    StyleBorderSide side);
bool style_parse_pair_with_auto(const Stylesheet *sheet, const char *text, size_t length, int values[2], bool automatic[2], bool percentages[2]);
void style_parse_pair(const Stylesheet *sheet, const char *text, size_t length, int *first, int *second);
bool style_parse_grid_line(const Stylesheet *sheet, const char *text, size_t length, int *line, int *span);
bool style_parse_grid_track(const Stylesheet *sheet, const char *text,
                            size_t length, uint8_t *type, unsigned *value,
                            unsigned *minimum);
bool style_parse_color_with_alpha(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color, uint8_t *alpha);
bool style_font_span_equal(const char *text, size_t length, const char *wanted);
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
void style_parse_transform_translation(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_text_underline_offset(const Stylesheet *sheet, const char *text, size_t length, unsigned *offset_code);
bool style_parse_text_decoration_underline(const Stylesheet *sheet, const char *text, size_t length, bool *underline);
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
bool style_parse_font_shorthand(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *font);
bool style_parse_color(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color);
uint64_t style_parse_box(const Stylesheet *sheet, const char *text, size_t length, StyleEdges *edges, bool padding);
bool style_parse_background_size(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_background_shorthand_color(const Stylesheet *sheet, const char *text, size_t length, uint32_t *color, uint8_t *alpha, bool *transparent);
bool style_parse_background_position(const Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
bool style_parse_background_shorthand_image(Stylesheet *sheet, const char *text, size_t length, ComputedStyle *style);
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


/* style_properties.c */
bool declaration_is_important(const char *value, size_t length);
size_t declaration_value_length(const char *value, size_t length);
size_t skip_css_space_and_comments(const char *text, size_t length,
                                   size_t at);
size_t find_declaration_end(const char *text, size_t length, size_t at);
bool style_parse_declarations(Stylesheet *sheet, const char *text,
                              size_t length, ComputedStyle *style,
                              uint64_t *mask, uint64_t *mask_high,
                              uint64_t *inherit_mask,
                              bool collect_variables, int important_filter,
                              uint64_t *revert_rule_mask,
                              uint64_t *revert_rule_mask_high);
bool style_parse_declarations_split(Stylesheet *sheet, const char *text,
                                    size_t length,
                                    ComputedStyle *normal_style,
                                    uint64_t *normal_mask,
                                    uint64_t *normal_mask_high,
                                    uint64_t *normal_inherit_mask,
                                    ComputedStyle *important_style,
                                    uint64_t *important_mask,
                                    uint64_t *important_mask_high,
                                    uint64_t *important_inherit_mask,
                                    uint64_t *normal_revert_rule_mask,
                                    uint64_t *normal_revert_rule_mask_high,
                                    uint64_t *important_revert_rule_mask,
                                    uint64_t *important_revert_rule_mask_high);
bool style_compile_deferred_program(Stylesheet *sheet, const char *text,
                                    size_t length, uint32_t *program_offset,
                                    uint16_t *program_count);
bool style_property_name_is_logical(const char *name, size_t length);
bool style_apply_deferred_program(
    Stylesheet *sheet, const char *text, size_t length,
    uint32_t program_offset, uint16_t program_count, bool important,
    ComputedStyle *style, uint64_t *mask, uint64_t *mask_high,
    uint64_t *inherit_mask);

/* style_sheet.c */
bool style_set_variable(Stylesheet *sheet, const char *name,
                        size_t name_length, const char *value,
                        size_t value_length);
void stylesheet_rule_revert_mask(const Stylesheet *sheet,
                                 const StyleRule *rule,
                                 uint64_t *mask, uint64_t *mask_high);


/* style_match.c */
bool style_tag_is(lxb_dom_node_t *node, const char *wanted);
bool option_is_displayed_by_default(lxb_dom_node_t *option);
bool class_contains_length(const char *classes, size_t length,
                           const char *wanted, size_t wanted_length);

/* selector scanning shared with the sheet builder */
size_t skip_selector_identifier(const char *text, size_t length, size_t at);


/* style_sheet.c: rule-index access shared with the resolver */
const char *style_rule_fast_key(const StyleRule *rule);
StyleRuleIndexBucket *style_rule_find_bucket(const Stylesheet *sheet,
                                             SelectorType type,
                                             const char *text, size_t length,
                                             bool create);
void stylesheet_prepare_rule_index(Stylesheet *sheet);

/* style_selector_program.c */
void stylesheet_drop_selector_program(Stylesheet *sheet);
void stylesheet_prepare_selector_program(Stylesheet *sheet);

#endif /* TILEFINCH_STYLE_INTERNAL_H */
