#ifndef TILEFINCH_STYLE_H
#define TILEFINCH_STYLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/viewport.h"

#define STYLE_BACKGROUND_SIZE_EXPLICIT  UINT8_C(1)
#define STYLE_BACKGROUND_WIDTH_PERCENT  UINT8_C(2)
#define STYLE_BACKGROUND_HEIGHT_PERCENT UINT8_C(4)
#define STYLE_BACKGROUND_WIDTH_AUTO     UINT8_C(8)
#define STYLE_BACKGROUND_HEIGHT_AUTO    UINT8_C(16)
#define STYLE_BACKGROUND_POSITION_PIXELS UINT8_C(32)
#define STYLE_BACKGROUND_NO_REPEAT_X    UINT8_C(64)
#define STYLE_BACKGROUND_NO_REPEAT_Y    UINT8_C(128)
#include "tilefinch/font.h"

/* Bounded gradient storage. Gradients are retained in the stylesheet's
   shared paint table rather than copied into every ComputedStyle. A value
   with more stops than this is simply unparseable, and the P1 fallback
   semantics then leave the previous background in force. */
#define STYLE_GRADIENT_STOP_LIMIT 8
#define STYLE_GRADIENT_ANGLE_MASK UINT16_C(0x01ff)
#define STYLE_GRADIENT_REPEATING UINT16_C(0x2000)
#define STYLE_GRADIENT_RADIAL_CIRCLE UINT16_C(0x4000)
#define STYLE_GRADIENT_RADIAL UINT16_C(0x8000)

/* Bounded shared `box-shadow` storage, on the same no-per-element-allocation
   rule as the gradient ramp above.  The limits are not cosmetic: the shadow
   rasteriser touches (width + 2*(blur+spread)) x (height + 2*(blur+spread))
   pixels per layer, so an unclamped blur or spread from a hostile page would
   demand an unbounded raster job.  These caps hold the worst case to a
   bounded skirt around the box, mirroring TILEFINCH_FONT_RASTER_PIXEL_LIMIT.
   Offsets are clamped rather than rejected (a huge offset simply pushes the
   shadow off-screen, where the tile cull discards it), while a layer list
   longer than the cap is unparseable so the P1 fallback keeps the previous
   declaration. */
#define STYLE_BOX_SHADOW_LIMIT 4
#define STYLE_BOX_SHADOW_OFFSET_LIMIT 64
#define STYLE_BOX_SHADOW_BLUR_LIMIT 32
#define STYLE_BOX_SHADOW_SPREAD_LIMIT 32

/* blur packs two flag bits above its 0..STYLE_BOX_SHADOW_BLUR_LIMIT value so
   a shadow layer stays at eight bytes. */
#define STYLE_BOX_SHADOW_BLUR_MASK UINT8_C(0x3f)
#define STYLE_BOX_SHADOW_CURRENT_COLOR UINT8_C(0x40)
#define STYLE_BOX_SHADOW_INSET UINT8_C(0x80)

/* Discriminants for the background-image union. */
#define STYLE_BACKGROUND_IMAGE_NONE UINT8_C(0)
#define STYLE_BACKGROUND_IMAGE_URL UINT8_C(1)
#define STYLE_BACKGROUND_IMAGE_GRADIENT UINT8_C(2)

#define STYLE_IMAGE_URL_CAPACITY 256
#define STYLE_IMAGE_REFERENCE_LIMIT 128
#define STYLE_IMAGE_SOURCE_LIMIT 32
#define STYLE_SELECTOR_CAPACITY 192
#define STYLE_FAST_KEY_CAPACITY 64
#define STYLE_LAYER_CAPACITY 16
#define STYLE_LAYER_NAME_CAPACITY 48
#define STYLE_GENERATED_TEXT_CAPACITY 64
#define STYLE_DIAGNOSTIC_PROPERTY_CAPACITY 16
#define STYLE_DIAGNOSTIC_PROPERTY_NAME_CAPACITY 40
#define STYLE_GENERATED_TEXT_LIMIT 64
#define STYLE_WEB_FONT_NAME_CAPACITY 64
#define STYLE_WEB_FONT_SOURCE_LIMIT 2
#define STYLE_WEB_FONT_REFERRER_POLICY_CAPACITY 40

typedef struct {
    const char *reference;
    const char *source_base_url;
    /* NULL identifies an inline stylesheet, whose subresource policy remains
       the current document policy. External sources always expose a retained
       normalized value; an empty string is a known-empty policy. */
    const char *source_referrer_policy;
    unsigned family_slot;
    bool bold;
} StylesheetWebFontSource;

typedef struct {
    size_t declarations_discovered;
    size_t sources_selected;
    size_t unsupported_sources;
    size_t duplicate_sources;
    size_t skipped_family_limit;
    size_t skipped_source_limit;
} StylesheetWebFontStats;

/* Fixed pixel lengths remain their signed integer value.  Reference-dependent
   values use positive tags well outside the bounded layout coordinate range,
   so existing fixed-value storage stays four bytes per property. */
typedef int32_t StyleLength;
#define STYLE_LENGTH_NONE INT32_MAX
#define STYLE_LENGTH_MIN_CONTENT (INT32_MAX - 1)
#define STYLE_LENGTH_MAX_CONTENT (INT32_MAX - 2)
#define STYLE_LENGTH_FIT_CONTENT (INT32_MAX - 3)
#define STYLE_LENGTH_DIRECT_LIMIT INT32_C(1048575)

#define STYLE_INSET_TOP_PERCENT UINT8_C(1)
#define STYLE_INSET_RIGHT_PERCENT UINT8_C(2)
#define STYLE_INSET_BOTTOM_PERCENT UINT8_C(4)
#define STYLE_INSET_LEFT_PERCENT UINT8_C(8)

#define GRID_TRACK_AUTO UINT8_C(0)
#define GRID_TRACK_FIXED UINT8_C(1)
#define GRID_TRACK_FLEX UINT8_C(2)
#define GRID_TRACK_PERCENT UINT8_C(3)
/*
 * GRID_TRACK_AUTO values retain the content-sizing function without adding
 * bytes to ComputedStyle. Zero remains the ordinary stretchable `auto`
 * track; the other forms are intrinsic and therefore do not participate in
 * justify-content:stretch. fit-content() uses the high bit plus a bounded
 * pixel cap.
 */
#define GRID_TRACK_AUTO_VALUE UINT16_C(0)
#define GRID_TRACK_MIN_CONTENT_VALUE UINT16_C(1)
#define GRID_TRACK_MAX_CONTENT_VALUE UINT16_C(2)
#define GRID_TRACK_FIT_CONTENT_FLAG UINT16_C(0x8000)
#define GRID_TRACK_FIT_CONTENT_MASK UINT16_C(0x7fff)
#define GRID_TRACK_REPEAT_LIMIT 12
#define STYLE_GRID_AREA_TEMPLATE_LIMIT 32
#define STYLE_GRID_AREA_NAME_LIMIT 32
#define STYLE_GRID_AREA_NAME_CAPACITY 32
#define STYLE_GRID_AREA_RECT_LIMIT 12
#define STYLE_GRID_AREA_ROW_LIMIT 8
#define STYLE_GRID_TRACK_TEMPLATE_LIMIT 31
#define STYLE_GRID_LINE_NAME_LIMIT 15
#define STYLE_GRID_LINE_NAMES_PER_LINE 2

typedef uint8_t DisplayMode;
enum {
    DISPLAY_INLINE,
    DISPLAY_INLINE_BLOCK,
    DISPLAY_INLINE_FLEX,
    DISPLAY_INLINE_GRID,
    DISPLAY_BLOCK,
    DISPLAY_FLOW_ROOT,
    DISPLAY_FLEX,
    DISPLAY_GRID,
    DISPLAY_TABLE,
    DISPLAY_TABLE_ROW,
    DISPLAY_TABLE_CELL,
    DISPLAY_TABLE_ROW_GROUP,
    DISPLAY_TABLE_HEADER_GROUP,
    DISPLAY_TABLE_FOOTER_GROUP,
    DISPLAY_TABLE_COLUMN,
    DISPLAY_CONTENTS,
    DISPLAY_NONE
};

typedef uint8_t FlexDirection;
enum {
    FLEX_ROW,
    FLEX_ROW_REVERSE,
    FLEX_COLUMN,
    FLEX_COLUMN_REVERSE
};

typedef enum {
    ALIGN_START,
    ALIGN_CENTER,
    ALIGN_END,
    ALIGN_STRETCH,
    ALIGN_BASELINE
} AlignItems;

typedef uint8_t AlignSelf;
enum {
    ALIGN_SELF_AUTO,
    ALIGN_SELF_START,
    ALIGN_SELF_CENTER,
    ALIGN_SELF_END,
    ALIGN_SELF_STRETCH,
    ALIGN_SELF_BASELINE
};

typedef uint8_t JustifyContent;
enum {
    JUSTIFY_START,
    JUSTIFY_CENTER,
    JUSTIFY_END,
    JUSTIFY_SPACE_BETWEEN,
    JUSTIFY_SPACE_AROUND,
    JUSTIFY_SPACE_EVENLY,
    JUSTIFY_STRETCH
};

typedef uint8_t TextAlign;
enum {
    TEXT_ALIGN_START,
    TEXT_ALIGN_CENTER,
    TEXT_ALIGN_END,
    TEXT_ALIGN_LEFT,
    TEXT_ALIGN_RIGHT
};

/* Retained in one byte inside ComputedStyle.  The legacy `break-word`
   keyword is deliberately distinct from overflow-wrap: it creates
   emergency opportunities without changing min-content sizing. */
typedef uint8_t WordBreakMode;
enum {
    WORD_BREAK_NORMAL,
    WORD_BREAK_ALL,
    WORD_BREAK_KEEP_ALL,
    WORD_BREAK_LEGACY
};

typedef uint8_t FloatMode;
enum {
    FLOAT_NONE,
    FLOAT_LEFT,
    FLOAT_RIGHT
};

typedef uint8_t ClearMode;
enum {
    CLEAR_NONE,
    CLEAR_LEFT,
    CLEAR_RIGHT,
    CLEAR_BOTH
};

/* Keep inherited wrapping state byte-sized: a C enum would normally occupy
   an int in every retained ComputedStyle. */
typedef uint8_t OverflowWrap;
enum {
    OVERFLOW_WRAP_NORMAL,
    OVERFLOW_WRAP_BREAK_WORD,
    OVERFLOW_WRAP_ANYWHERE
};

#define STYLE_OVERFLOW_WRAP_MASK UINT8_C(0x03)
#define STYLE_TEXT_WRAP_SHIFT 2
#define STYLE_TEXT_WRAP_MASK UINT8_C(0x0c)
#define STYLE_USER_SELECT_SHIFT 4
#define STYLE_USER_SELECT_MASK UINT8_C(0x30)
/* Declaration-only marker. It is consumed by apply_values() and is never
   retained in a resolved ComputedStyle. This lets line-clamp share the
   text-overflow cascade slot without an unrelated text-overflow rule
   resetting an authored clamp. */
#define STYLE_LINE_CLAMP_SPECIFIED UINT8_C(0x40)
#define STYLE_ISOLATION_ISOLATE UINT8_C(0x40)
#define STYLE_TEXT_OVERFLOW_ELLIPSIS UINT8_C(0x80)

typedef uint8_t StyleTextWrap;
enum {
    STYLE_TEXT_WRAP_NORMAL,
    STYLE_TEXT_WRAP_BALANCE,
    STYLE_TEXT_WRAP_PRETTY
};

typedef uint8_t StyleUserSelect;
enum {
    STYLE_USER_SELECT_AUTO,
    STYLE_USER_SELECT_TEXT,
    STYLE_USER_SELECT_NONE,
    STYLE_USER_SELECT_ALL
};

typedef uint8_t StyleTouchAction;
enum {
    STYLE_TOUCH_ACTION_AUTO,
    STYLE_TOUCH_ACTION_NONE,
    STYLE_TOUCH_ACTION_PAN_X,
    STYLE_TOUCH_ACTION_PAN_Y
};

typedef uint8_t StyleResize;
enum {
    STYLE_RESIZE_NONE,
    STYLE_RESIZE_BOTH,
    STYLE_RESIZE_HORIZONTAL,
    STYLE_RESIZE_VERTICAL
};

typedef enum {
    PSEUDO_NONE,
    PSEUDO_BEFORE,
    PSEUDO_AFTER
} PseudoElement;

typedef uint8_t VerticalAlign;
enum {
    VERTICAL_BASELINE,
    VERTICAL_SUPER,
    VERTICAL_SUB,
    VERTICAL_MIDDLE,
    VERTICAL_TOP,
    VERTICAL_BOTTOM
};

typedef uint8_t AppearanceMode;
enum {
    APPEARANCE_NONE,
    APPEARANCE_AUTO,
    APPEARANCE_BASE,
    APPEARANCE_BASE_SELECT,
    APPEARANCE_BUTTON,
    APPEARANCE_CHECKBOX,
    APPEARANCE_LISTBOX,
    APPEARANCE_MENULIST_BUTTON,
    APPEARANCE_METER,
    APPEARANCE_PROGRESS_BAR,
    APPEARANCE_RADIO,
    APPEARANCE_SEARCHFIELD,
    APPEARANCE_TEXTAREA,
    APPEARANCE_TEXTFIELD
};

/* Grid auto-flow consumes the two formerly reserved containing-block bits.
   justify-self shares the unused high nibble of the byte-sized appearance
   field.  Both keep the compact ComputedStyle envelope unchanged. */
#define STYLE_GRID_AUTO_FLOW_COLUMN UINT8_C(1)
#define STYLE_GRID_AUTO_FLOW_DENSE  UINT8_C(2)
#define STYLE_APPEARANCE_MASK       UINT8_C(0x0f)
#define STYLE_JUSTIFY_SELF_SHIFT    4
#define STYLE_JUSTIFY_SELF_MASK     UINT8_C(0x70)

typedef uint8_t TextTransformMode;
enum {
    TEXT_TRANSFORM_NONE,
    TEXT_TRANSFORM_UPPERCASE,
    TEXT_TRANSFORM_LOWERCASE,
    TEXT_TRANSFORM_CAPITALIZE
};

typedef struct {
    int top;
    int right;
    int bottom;
    int left;
} StyleEdges;

/* A bounded static linear/radial colour ramp.  Colour and alpha share
   one word per stop (0xAARRGGBB) and the stop offset is a 0..255 fraction of
   the gradient line, which keeps the whole ramp at 43 bytes inline.
   `angle` stores CSS degrees in its low nine bits; the high bits distinguish
   radial/circular/repeating ramps without growing every computed style. */
typedef struct {
    uint32_t stop_argb[STYLE_GRADIENT_STOP_LIMIT];
    uint8_t stop_position[STYLE_GRADIENT_STOP_LIMIT];
    uint16_t angle;
    uint8_t stop_count;
} StyleGradient;

/* Compact storage shared by box-shadow and text-shadow. Text shadows keep
   spread at zero and never set the inset flag. */
typedef struct {
    uint32_t argb;
    int8_t offset_x;
    int8_t offset_y;
    int8_t spread;
    uint8_t blur;
} StyleBoxShadow;

#define STYLE_BORDER_RADIUS_CORNER_MASK UINT32_C(0x7f)
#define STYLE_BORDER_RADIUS_PRESENT_SHIFT 28u
#define STYLE_BORDER_RADIUS_PRESENT_MASK UINT32_C(0xf0000000)

static inline bool style_border_radius_is_packed(int code)
{
    return ((uint32_t) code & STYLE_BORDER_RADIUS_PRESENT_MASK) != 0;
}

static inline uint8_t style_border_radius_present(int code)
{
    return style_border_radius_is_packed(code)
        ? (uint8_t) ((uint32_t) code >> STYLE_BORDER_RADIUS_PRESENT_SHIFT)
        : UINT8_C(0x0f);
}

static inline int style_border_radius_corner(int code, unsigned corner)
{
    if (!style_border_radius_is_packed(code)) return code > 0 ? code : 0;
    if (corner > 3) corner = 3;
    return (int) (((uint32_t) code >> (corner * 7u))
                  & STYLE_BORDER_RADIUS_CORNER_MASK);
}

static inline int style_border_radius_pack(
    int top_left, int top_right, int bottom_right, int bottom_left)
{
    int values[4] = {top_left, top_right, bottom_right, bottom_left};
    uint32_t packed = STYLE_BORDER_RADIUS_PRESENT_MASK;
    for (unsigned corner = 0; corner < 4; corner++) {
        int value = values[corner];
        if (value < 0) value = 0;
        if (value > 127) value = 127;
        packed |= (uint32_t) value << (corner * 7u);
    }
    return (int) packed;
}

static inline int style_border_radius_set_corner_present(
    int code, uint8_t present, unsigned corner, int value)
{
    if (corner > 3) corner = 3;
    int values[4];
    for (unsigned at = 0; at < 4; at++) {
        values[at] = style_border_radius_corner(code, at);
    }
    if (value < 0) value = 0;
    if (value > 127) value = 127;
    values[corner] = value;
    present |= (uint8_t) (UINT8_C(1) << corner);
    uint32_t packed = (uint32_t) present
                      << STYLE_BORDER_RADIUS_PRESENT_SHIFT;
    for (unsigned at = 0; at < 4; at++) {
        packed |= (uint32_t) values[at] << (at * 7u);
    }
    return (int) packed;
}

static inline int style_border_radius_merge(int current, int incoming)
{
    if (!style_border_radius_is_packed(incoming)) return incoming;
    uint8_t present = style_border_radius_present(incoming);
    int values[4];
    bool uniform = true;
    for (unsigned corner = 0; corner < 4; corner++) {
        values[corner] = (present & (UINT8_C(1) << corner)) != 0
            ? style_border_radius_corner(incoming, corner)
            : style_border_radius_corner(current, corner);
        if (corner != 0 && values[corner] != values[0]) uniform = false;
    }
    return uniform ? values[0]
        : style_border_radius_pack(
              values[0], values[1], values[2], values[3]);
}

static inline int style_border_radius_maximum(int code)
{
    int maximum = 0;
    for (unsigned corner = 0; corner < 4; corner++) {
        int value = style_border_radius_corner(code, corner);
        if (value > maximum) maximum = value;
    }
    return maximum;
}

static inline void style_border_radius_resolve_corners(
    int code, int width, int height, int radii[4])
{
    if (radii == NULL) return;
    if (width <= 0 || height <= 0) {
        radii[0] = radii[1] = radii[2] = radii[3] = 0;
        return;
    }
    for (unsigned corner = 0; corner < 4; corner++) {
        radii[corner] = style_border_radius_corner(code, corner);
    }
    if (!style_border_radius_is_packed(code)) {
        int maximum = width < height ? width / 2 : height / 2;
        if (radii[0] > maximum) radii[0] = maximum;
        radii[1] = radii[2] = radii[3] = radii[0];
        return;
    }
    int64_t factor = INT64_C(32768);
    const int sums[4] = {
        radii[0] + radii[1], radii[3] + radii[2],
        radii[0] + radii[3], radii[1] + radii[2]
    };
    const int limits[4] = {width, width, height, height};
    for (unsigned pair = 0; pair < 4; pair++) {
        if (sums[pair] <= 0 || sums[pair] <= limits[pair]) continue;
        int64_t candidate =
            (int64_t) limits[pair] * INT64_C(32768) / sums[pair];
        if (candidate < factor) factor = candidate;
    }
    if (factor >= INT64_C(32768)) return;
    for (unsigned corner = 0; corner < 4; corner++) {
        radii[corner] = (int) ((int64_t) radii[corner] * factor
                               / INT64_C(32768));
    }
}

static inline int style_border_radius_adjust(int code, int delta)
{
    if (!style_border_radius_is_packed(code)) {
        int value = code + delta;
        return value > 0 ? value : 0;
    }
    int values[4];
    bool uniform = true;
    for (unsigned corner = 0; corner < 4; corner++) {
        values[corner] = style_border_radius_corner(code, corner) + delta;
        if (values[corner] < 0) values[corner] = 0;
        if (corner != 0 && values[corner] != values[0]) uniform = false;
    }
    if (uniform) return values[0];
    for (unsigned corner = 0; corner < 4; corner++) {
        if (values[corner] > 127) values[corner] = 127;
    }
    if (values[0] == values[1] && values[0] == values[2]
        && values[0] == values[3]) return values[0];
    return style_border_radius_pack(
        values[0], values[1], values[2], values[3]);
}

/* Apply the CSS outset corner correction without floating point. The input
   radii describe the selected edge before expansion; zero-radius corners
   stay sharp, while sufficiently round corners grow by the full outset. */
static inline int style_border_radius_outset(
    int code, int outset, int edge_width, int edge_height)
{
    if (outset <= 0) return style_border_radius_adjust(code, outset);
    int edge_maximum = edge_width > edge_height ? edge_width : edge_height;
    if (edge_maximum <= 0) return code;
    int values[4];
    bool uniform = true;
    for (unsigned corner = 0; corner < 4; corner++) {
        int radius = style_border_radius_corner(code, corner);
        if (radius <= 0) {
            values[corner] = 0;
        } else if (radius > outset) {
            values[corner] = radius + outset;
        } else {
            int64_t coverage =
                (int64_t) 2 * radius * INT64_C(32768) / edge_maximum;
            if (coverage > INT64_C(32768)) {
                values[corner] = radius + outset;
            } else {
                int64_t ratio =
                    (int64_t) radius * INT64_C(32768) / outset;
                int64_t inverse_ratio = INT64_C(32768) - ratio;
                int64_t inverse_ratio_cube =
                    ((inverse_ratio * inverse_ratio) >> 15)
                    * inverse_ratio >> 15;
                int64_t coverage_cube =
                    ((coverage * coverage) >> 15) * coverage >> 15;
                int64_t factor = INT64_C(32768)
                    - ((inverse_ratio_cube
                        * (INT64_C(32768) - coverage_cube)) >> 15);
                values[corner] = radius + (int) (
                    ((int64_t) outset * factor + INT64_C(16384)) >> 15);
            }
        }
        if (corner != 0 && values[corner] != values[0]) uniform = false;
    }
    if (uniform) return values[0];
    for (unsigned corner = 0; corner < 4; corner++) {
        if (values[corner] > 127) values[corner] = 127;
    }
    if (values[0] == values[1] && values[0] == values[2]
        && values[0] == values[3]) return values[0];
    return style_border_radius_pack(
        values[0], values[1], values[2], values[3]);
}

/* One box- or text-shadow layer, held to eight bytes: colour and alpha share
   a word (0xAARRGGBB), geometry is byte-sized because it is hard-clamped, and
   blur carries the inset/currentColor flags in its top two bits. */
static inline int style_box_shadow_blur(const StyleBoxShadow *shadow)
{
    return shadow == NULL ? 0 : (shadow->blur & STYLE_BOX_SHADOW_BLUR_MASK);
}

static inline bool style_box_shadow_is_inset(const StyleBoxShadow *shadow)
{
    return shadow != NULL && (shadow->blur & STYLE_BOX_SHADOW_INSET) != 0;
}

static inline bool style_box_shadow_uses_current_color(
    const StyleBoxShadow *shadow)
{
    return shadow != NULL
           && (shadow->blur & STYLE_BOX_SHADOW_CURRENT_COLOR) != 0;
}

typedef struct StyleMathInstruction StyleMathInstruction;
typedef struct StyleMathProgram StyleMathProgram;

typedef enum {
    FONT_SIZE_UNIT_ABSOLUTE = 0,
    FONT_SIZE_UNIT_PERCENT,
    FONT_SIZE_UNIT_EM,
    FONT_SIZE_UNIT_REM
} FontSizeUnit;

typedef enum {
    LIST_STYLE_AUTO = 0,
    LIST_STYLE_NONE,
    LIST_STYLE_DISC,
    LIST_STYLE_CIRCLE,
    LIST_STYLE_SQUARE,
    LIST_STYLE_DECIMAL,
    LIST_STYLE_DECIMAL_LEADING_ZERO,
    LIST_STYLE_LOWER_ALPHA,
    LIST_STYLE_UPPER_ALPHA,
    LIST_STYLE_LOWER_ROMAN,
    LIST_STYLE_UPPER_ROMAN
} ListStyleType;

#define STYLE_COUNTER_OPERATION_LIMIT 2
#define STYLE_COUNTER_OPERATION_SET_LIMIT 255

typedef struct {
    const char *name;
    int value;
} StyleCounterOperation;

typedef struct {
    uint8_t count;
    StyleCounterOperation operations[STYLE_COUNTER_OPERATION_LIMIT];
} StyleCounterOperations;

#define WHITE_SPACE_NORMAL UINT8_C(0)
#define WHITE_SPACE_NOWRAP UINT8_C(1)
#define WHITE_SPACE_PRE UINT8_C(2)
#define WHITE_SPACE_PRE_WRAP UINT8_C(3)
#define WHITE_SPACE_PRE_LINE UINT8_C(4)
#define WHITE_SPACE_BREAK_SPACES UINT8_C(5)

#define STYLE_OBJECT_FIT_FILL UINT8_C(0)
#define STYLE_OBJECT_FIT_COVER UINT8_C(1)
#define STYLE_OBJECT_FIT_CONTAIN UINT8_C(2)
#define STYLE_OBJECT_FIT_NONE UINT8_C(7)
#define STYLE_OBJECT_FIT_SCALE_DOWN UINT8_C(8)
#define STYLE_OBJECT_POSITION_PERCENT_MASK UINT16_C(0x007f)
#define STYLE_OBJECT_POSITION_OFFSET_SHIFT 7u

static inline uint16_t style_object_position_encode(int percent, int offset)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (offset < -256) offset = -256;
    if (offset > 255) offset = 255;
    return (uint16_t) percent
        | (uint16_t) ((unsigned) offset & 0x1ffu)
          << STYLE_OBJECT_POSITION_OFFSET_SHIFT;
}

static inline int style_object_position_percent(uint16_t encoded)
{
    return encoded & STYLE_OBJECT_POSITION_PERCENT_MASK;
}

static inline int style_object_position_offset(uint16_t encoded)
{
    int value = (encoded >> STYLE_OBJECT_POSITION_OFFSET_SHIFT) & 0x1ff;
    return value >= 0x100 ? value - 0x200 : value;
}

typedef struct {
    DisplayMode display;
    uint32_t color;
    uint8_t color_alpha;
    uint32_t background;
    uint8_t background_alpha;
    bool has_background;
    /* Packed into the existing two-byte alignment hole.  This retains both
       the element's inherited underline offset and the offset belonging to
       an ancestor-originated decoration without growing ComputedStyle. */
    uint16_t text_decoration_state;
    /* background_image_kind distinguishes the common scalar URL from a
       gradient retained behind paint_stack_state. */
    const char *background_image;
    /* Grouped with the byte-sized background fields so the discriminant
       costs no padding of its own. */
    uint8_t background_image_kind;
    uint8_t background_fit;
    uint16_t background_width;
    uint16_t background_height;
    /* Percent of the paint area by default; device pixels (sprite-sheet
       crops such as background-position:0 -1323px) when
       STYLE_BACKGROUND_POSITION_PIXELS is set in background_size_flags. */
    int16_t background_position_x;
    int16_t background_position_y;
    uint8_t background_size_flags;
    uint8_t object_fit;
    int font_scale;
    int font_size;
    /* The used size is retained in 26.6 pixels without growing the PSP
       style record.  font_size_unit is nonzero only while a relative
       declaration is waiting for computed-value resolution. */
    uint8_t font_size_fraction;
    uint8_t font_size_unit;
    uint8_t root_font_size;
    uint8_t root_font_size_fraction;
    FontFamily font_family;
    uint16_t font_weight;
    bool font_bold;
    bool font_italic;
    VerticalAlign vertical_align;
    int word_spacing;
    int line_height;
    StyleLength text_indent;
    TextAlign text_align;
    FloatMode float_mode;
    ClearMode clear_mode;
    /* Retaining the computed keyword costs the same byte as the former
       nowrap flag and lets CSSOM distinguish pre-wrap/pre-line correctly. */
    uint8_t white_space_mode;
    OverflowWrap overflow_wrap;
    WordBreakMode word_break_mode;
    int8_t letter_spacing;
    StyleEdges margin;
    uint8_t margin_top_auto : 1;
    uint8_t margin_left_auto : 1;
    uint8_t margin_right_auto : 1;
    uint8_t margin_bottom_auto : 1;
    uint8_t margin_top_percent : 1;
    uint8_t margin_left_percent : 1;
    uint8_t margin_right_percent : 1;
    uint8_t margin_bottom_percent : 1;
    StyleEdges padding;
    StyleEdges border;
    uint32_t border_color;
    uint8_t border_alpha;
    /* -webkit-line-clamp is bounded to 31 lines and shares the otherwise
       unused byte beside border_alpha. */
    uint8_t line_clamp;
    /* Width/style/offset are packed into a two-byte visual word.  This lands
       in the existing alignment hole before border_radius. */
    uint16_t outline_state;
    int border_radius;
    int gap;
    int row_gap;
    /*
     * Low eight bits retain the explicit column count; bits 8..15 retain a
     * one-based stylesheet-local named-area template. Packing the uncommon
     * template reference here avoids growing every PSP ComputedStyle.
     */
    int grid_columns;
    int grid_min_column_width;
    /* Six four-bit fields retain bounded column/row placement without
       growing ComputedStyle. Values 1..9 are line numbers, 15 represents
       the supported -1/end line, and spans are clamped to eight tracks. */
    uint8_t grid_placement[3];
    /* Uses the byte that aligned FlexDirection: a five-bit integral length,
       a temporary em-unit bit, and a two-bit overflow visual-box selector. */
    uint8_t overflow_clip_margin;
    /* Bounded basic-shape clip-path state, packed into the alignment hole
       before flex_direction. */
    uint16_t clip_path_state;
    FlexDirection flex_direction;
    bool flex_wrap;
    bool flex_wrap_reverse;
    int flex_grow;
    int order;
    uint16_t flex_shrink;
    bool has_flex_basis;
    bool flex_basis_percent;
    int flex_basis;
    int flex_basis_offset;
    bool has_width;
    bool width_max_content;
    bool width_percent;
    bool min_width_auto;
    bool min_width_percent;
    bool max_width_percent;
    int width;
    int width_offset;
    bool has_height;
    bool height_percent;
    bool min_height_percent;
    bool max_height_percent;
    int height;
    int aspect_width;
    int aspect_height;
    int min_width;
    int min_width_offset;
    int min_height;
    int max_width;
    int max_width_offset;
    int max_height;
    bool box_sizing_border_box;
    /* These two byte-sized container defaults replace the former four-byte
       AlignItems field, so Box Alignment's justify-items costs no retained
       per-node memory on PSP. */
    uint8_t align_items;
    uint8_t justify_items;
    AlignSelf align_self;
    JustifyContent align_content;
    JustifyContent justify_content;
    const char *mask_image;
    bool list_style_none;
    bool generated_content;
    uint8_t generated_text_length;
    /* Uses alignment padding before generated_text. */
    bool table_layout_fixed;
    /* Authored outline colour uses the alignment slot before generated_text. */
    uint32_t outline_color;
    const char *generated_text;
    uint8_t generated_attr_length;
    uint8_t outline_alpha;
    /* One-based ID for uncommon per-side border colours. This consumes
       pointer-alignment padding before generated_attr. */
    uint8_t border_color_set;
    const char *generated_attr;
    bool has_top;
    bool has_right;
    bool has_bottom;
    bool has_left;
    int top;
    int right;
    int bottom;
    int left;
    uint8_t inset_percent_mask;
    uint16_t hidden : 1;
    uint16_t clip_rect_empty : 1;
    uint16_t out_of_flow : 1;
    uint16_t relative_position : 1;
    uint16_t sticky_position : 1;
    uint16_t fixed_position : 1;
    uint16_t overflow_x_scroll : 1;
    uint16_t overflow_y_scroll : 1;
    uint16_t overflow_x_clip_only : 1;
    uint16_t overflow_y_clip_only : 1;
    uint16_t has_z_index : 1;
    uint16_t grid_auto_column_type : 2;
    uint16_t grid_auto_row_type : 2;
    uint16_t visibility_hidden : 1;
    /* A single bounded implicit-track size covers the common Level 1 form.
       Lists retain their first value deterministically until cyclic implicit
       track lists can justify additional per-style storage. */
    uint16_t grid_auto_column_value;
    uint16_t grid_auto_row_value;
    /* A second bounded implicit sizing function is independent of the
       explicit template and therefore stays in the computed style. */
    uint16_t grid_auto_column_second;
    uint16_t grid_auto_row_second;
    int z_index;
    uint8_t opacity;
    uint8_t has_transform : 1;
    uint8_t has_perspective : 1;
    uint8_t has_filter : 1;
    uint8_t has_layout_containment : 1;
    uint8_t will_change_transform : 1;
    uint8_t pointer_events_none : 1;
    uint8_t table_border_collapse : 1;
    uint8_t containing_block_reserved : 2;
    /* This second bitfield byte already existed for grid-auto-flow.  The
       remaining six bits retain the bounded mobile interaction subset
       without growing every computed style on the PSP. */
    uint8_t individual_rotate_quadrants : 2;
    uint8_t resize_mode : 2;
    uint8_t touch_action : 2;
    uint8_t content_visibility : 2;
    /* Uniform CSS scale in Q2.6. */
    uint8_t transform_scale_q6;
    bool transform_x_percent;
    bool transform_y_percent;
    int transform_x;
    int transform_y;
    /* Uses existing tail padding on 64-bit hosts and one byte on PSP. */
    AppearanceMode appearance;
    TextTransformMode text_transform;
    /* The low byte is a one-based ID into the optional shared paint-stack
       table. The high byte retains four two-bit border line styles without
       growing the PSP computed-style record. */
    uint16_t paint_stack_state;
    /* Low three bits are StyleFilter; bit 3 retains direction, bits 4..5
       retain writing-mode, bit 6 marks a layout-time `ch` re-resolution,
       and bit 7 retains overflow-x:hidden. */
    uint8_t filter_code;
    /* One-based indices into the stylesheet's bounded, optional counter
       operation pool.  IDs keep the hot per-node style record smaller than
       three host pointers while preserving independent cascade semantics. */
    uint8_t counter_reset_id;
    uint8_t counter_increment_id;
    uint8_t counter_set_id;
    uint8_t list_style_type : 4;
    uint8_t list_style_inside : 1;
    uint8_t generated_expression : 1;
    uint8_t unicode_bidi_override : 1;
    /* Element scroll indicators overlay content on Tilefinch, so a stable
       gutter occupies no geometry while the computed keyword is retained. */
    uint8_t scrollbar_gutter_stable : 1;
    uint16_t object_position_x;
    uint16_t object_position_y;
} ComputedStyle;

typedef uint8_t StyleContentVisibility;
enum {
    STYLE_CONTENT_VISIBILITY_VISIBLE = 0,
    STYLE_CONTENT_VISIBILITY_AUTO,
    STYLE_CONTENT_VISIBILITY_HIDDEN,
    /* Declaration-only marker, normalized to VISIBLE by apply_values(). */
    STYLE_CONTENT_VISIBILITY_EXPLICIT_VISIBLE
};

static inline bool computed_style_width_min_content(
    const ComputedStyle *style)
{
    return style != NULL && style->has_width
        && style->width == STYLE_LENGTH_MIN_CONTENT;
}

static inline bool computed_style_width_max_content(
    const ComputedStyle *style)
{
    return style != NULL && style->has_width
        && style->width == STYLE_LENGTH_MAX_CONTENT;
}

static inline bool computed_style_width_fit_content(
    const ComputedStyle *style)
{
    return style != NULL && style->has_width
        && style->width == STYLE_LENGTH_FIT_CONTENT;
}

#define COMPUTED_GRID_COLUMN_COUNT_MASK UINT32_C(0x000000ff)
#define COMPUTED_GRID_TEMPLATE_AREA_SHIFT 8u
#define COMPUTED_GRID_TEMPLATE_AREA_MASK UINT32_C(0x00003f00)
#define COMPUTED_GRID_SUBGRID_COLUMNS UINT32_C(0x00004000)
#define COMPUTED_GRID_SUBGRID_ROWS UINT32_C(0x00008000)
#define COMPUTED_GRID_COLUMN_TEMPLATE_SHIFT 16u
#define COMPUTED_GRID_COLUMN_TEMPLATE_MASK UINT32_C(0x001f0000)
#define COMPUTED_GRID_ROW_TEMPLATE_SHIFT 21u
#define COMPUTED_GRID_ROW_TEMPLATE_MASK UINT32_C(0x03e00000)
#define COMPUTED_GRID_ROW_START_NAMED UINT32_C(0x04000000)
#define COMPUTED_GRID_ROW_END_NAMED UINT32_C(0x08000000)
#define COMPUTED_GRID_COLUMN_START_NAMED UINT32_C(0x10000000)
#define COMPUTED_GRID_COLUMN_END_NAMED UINT32_C(0x20000000)
#define COMPUTED_GRID_NAMED_LINE_MASK UINT32_C(0x3c000000)
#define COMPUTED_GRID_COLUMN_GAP_SPECIFIED UINT32_C(0x40000000)
#define COMPUTED_GRID_ROW_GAP_SPECIFIED UINT32_C(0x80000000)
#define COMPUTED_GRID_NAMED_AREA_MARKER UINT8_C(0xf0)

enum {
    COMPUTED_GRID_COLUMN_START_SHIFT = 0,
    COMPUTED_GRID_COLUMN_END_SHIFT = 4,
    COMPUTED_GRID_COLUMN_SPAN_SHIFT = 8,
    COMPUTED_GRID_ROW_START_SHIFT = 12,
    COMPUTED_GRID_ROW_END_SHIFT = 16,
    COMPUTED_GRID_ROW_SPAN_SHIFT = 20,
    COMPUTED_GRID_NEGATIVE_LINE_MIN = 10,
    COMPUTED_GRID_LINE_LAST = 15
};

static inline unsigned computed_style_grid_column_count(
    const ComputedStyle *style)
{
    return style == NULL ? 0
        : (unsigned) style->grid_columns & COMPUTED_GRID_COLUMN_COUNT_MASK;
}

static inline void computed_style_set_grid_column_count(
    ComputedStyle *style, unsigned count)
{
    if (style == NULL) return;
    if (count > GRID_TRACK_REPEAT_LIMIT) count = GRID_TRACK_REPEAT_LIMIT;
    style->grid_columns = (int) (
        ((uint32_t) style->grid_columns
         & ~COMPUTED_GRID_COLUMN_COUNT_MASK)
        | count);
}

static inline uint8_t computed_style_grid_template_area_id(
    const ComputedStyle *style)
{
    return style == NULL ? 0 : (uint8_t) (
        ((uint32_t) style->grid_columns
         & COMPUTED_GRID_TEMPLATE_AREA_MASK)
        >> COMPUTED_GRID_TEMPLATE_AREA_SHIFT);
}

static inline void computed_style_set_grid_template_area_id(
    ComputedStyle *style, uint8_t id)
{
    if (style == NULL) return;
    style->grid_columns = (int) (
        ((uint32_t) style->grid_columns
         & ~COMPUTED_GRID_TEMPLATE_AREA_MASK)
        | ((uint32_t) id << COMPUTED_GRID_TEMPLATE_AREA_SHIFT));
}

static inline bool computed_style_grid_subgrid_columns(
    const ComputedStyle *style)
{
    return style != NULL
        && ((uint32_t) style->grid_columns
            & COMPUTED_GRID_SUBGRID_COLUMNS) != 0;
}

static inline bool computed_style_grid_subgrid_rows(
    const ComputedStyle *style)
{
    return style != NULL
        && ((uint32_t) style->grid_columns
            & COMPUTED_GRID_SUBGRID_ROWS) != 0;
}

static inline void computed_style_set_grid_subgrid_columns(
    ComputedStyle *style, bool enabled)
{
    if (style == NULL) return;
    uint32_t packed = (uint32_t) style->grid_columns;
    if (enabled) packed |= COMPUTED_GRID_SUBGRID_COLUMNS;
    else packed &= ~COMPUTED_GRID_SUBGRID_COLUMNS;
    style->grid_columns = (int) packed;
}

static inline void computed_style_set_grid_subgrid_rows(
    ComputedStyle *style, bool enabled)
{
    if (style == NULL) return;
    uint32_t packed = (uint32_t) style->grid_columns;
    if (enabled) packed |= COMPUTED_GRID_SUBGRID_ROWS;
    else packed &= ~COMPUTED_GRID_SUBGRID_ROWS;
    style->grid_columns = (int) packed;
}

static inline bool computed_style_grid_column_gap_specified(
    const ComputedStyle *style)
{
    return style != NULL
        && ((uint32_t) style->grid_columns
            & COMPUTED_GRID_COLUMN_GAP_SPECIFIED) != 0;
}

static inline bool computed_style_grid_row_gap_specified(
    const ComputedStyle *style)
{
    return style != NULL
        && ((uint32_t) style->grid_columns
            & COMPUTED_GRID_ROW_GAP_SPECIFIED) != 0;
}

static inline void computed_style_set_grid_column_gap_specified(
    ComputedStyle *style, bool specified)
{
    if (style == NULL) return;
    uint32_t packed = (uint32_t) style->grid_columns;
    if (specified) packed |= COMPUTED_GRID_COLUMN_GAP_SPECIFIED;
    else packed &= ~COMPUTED_GRID_COLUMN_GAP_SPECIFIED;
    style->grid_columns = (int) packed;
}

static inline void computed_style_set_grid_row_gap_specified(
    ComputedStyle *style, bool specified)
{
    if (style == NULL) return;
    uint32_t packed = (uint32_t) style->grid_columns;
    if (specified) packed |= COMPUTED_GRID_ROW_GAP_SPECIFIED;
    else packed &= ~COMPUTED_GRID_ROW_GAP_SPECIFIED;
    style->grid_columns = (int) packed;
}

#define COMPUTED_GRID_TEMPLATE_ID_ACCESSORS(axis, SHIFT, MASK)              \
    static inline uint8_t computed_style_grid_##axis##_template_id(          \
        const ComputedStyle *style)                                          \
    {                                                                        \
        return style == NULL ? 0 : (uint8_t) (                               \
            ((uint32_t) style->grid_columns & (MASK)) >> (SHIFT));           \
    }                                                                        \
    static inline void computed_style_set_grid_##axis##_template_id(         \
        ComputedStyle *style, uint8_t id)                                     \
    {                                                                        \
        if (style == NULL) return;                                            \
        style->grid_columns = (int) (                                         \
            ((uint32_t) style->grid_columns & ~(MASK))                        \
            | ((uint32_t) id << (SHIFT)));                                   \
    }

COMPUTED_GRID_TEMPLATE_ID_ACCESSORS(
    column, COMPUTED_GRID_COLUMN_TEMPLATE_SHIFT,
    COMPUTED_GRID_COLUMN_TEMPLATE_MASK)
COMPUTED_GRID_TEMPLATE_ID_ACCESSORS(
    row, COMPUTED_GRID_ROW_TEMPLATE_SHIFT,
    COMPUTED_GRID_ROW_TEMPLATE_MASK)

#undef COMPUTED_GRID_TEMPLATE_ID_ACCESSORS

static inline uint8_t computed_style_grid_line_name(
    const ComputedStyle *style, unsigned placement_shift, uint32_t named_flag)
{
    if (style == NULL || placement_shift >= 24
        || (((uint32_t) style->grid_columns & named_flag) == 0)) return 0;
    unsigned byte = placement_shift / 8u;
    unsigned within = placement_shift % 8u;
    return (uint8_t) (
        (style->grid_placement[byte] >> within) & UINT8_C(0x0f));
}

static inline void computed_style_set_grid_line_name(
    ComputedStyle *style, unsigned placement_shift,
    uint32_t named_flag, uint8_t id)
{
    if (style == NULL || placement_shift >= 24) return;
    if (id == 0) {
        style->grid_columns = (int) (
            (uint32_t) style->grid_columns & ~named_flag);
        return;
    }
    if ((style->grid_placement[2] & UINT8_C(0xf0))
        == COMPUTED_GRID_NAMED_AREA_MARKER) {
        memset(style->grid_placement, 0, sizeof(style->grid_placement));
    }
    unsigned byte = placement_shift / 8u;
    unsigned within = placement_shift % 8u;
    uint8_t mask = (uint8_t) (UINT8_C(0x0f) << within);
    style->grid_placement[byte] = (uint8_t) (
        (style->grid_placement[byte] & ~mask)
        | ((id & UINT8_C(0x0f)) << within));
    uint32_t packed = (uint32_t) style->grid_columns;
    packed |= named_flag;
    style->grid_columns = (int) packed;
}

#define COMPUTED_GRID_LINE_NAME_ACCESSORS(                                  \
    axis, field, PLACEMENT_SHIFT, NAMED_FLAG)                                \
    static inline uint8_t computed_style_grid_##axis##_##field##_name(       \
        const ComputedStyle *style)                                          \
    {                                                                        \
        return computed_style_grid_line_name(                                \
            style, PLACEMENT_SHIFT, NAMED_FLAG);                             \
    }                                                                        \
    static inline void computed_style_set_grid_##axis##_##field##_name(      \
        ComputedStyle *style, uint8_t id)                                     \
    {                                                                        \
        computed_style_set_grid_line_name(                                   \
            style, PLACEMENT_SHIFT, NAMED_FLAG, id);                         \
    }

COMPUTED_GRID_LINE_NAME_ACCESSORS(
    row, start, COMPUTED_GRID_ROW_START_SHIFT,
    COMPUTED_GRID_ROW_START_NAMED)
COMPUTED_GRID_LINE_NAME_ACCESSORS(
    row, end, COMPUTED_GRID_ROW_END_SHIFT,
    COMPUTED_GRID_ROW_END_NAMED)
COMPUTED_GRID_LINE_NAME_ACCESSORS(
    column, start, COMPUTED_GRID_COLUMN_START_SHIFT,
    COMPUTED_GRID_COLUMN_START_NAMED)
COMPUTED_GRID_LINE_NAME_ACCESSORS(
    column, end, COMPUTED_GRID_COLUMN_END_SHIFT,
    COMPUTED_GRID_COLUMN_END_NAMED)

#undef COMPUTED_GRID_LINE_NAME_ACCESSORS

#define STYLE_FILTER_CODE_MASK UINT8_C(0x07)
#define STYLE_DIRECTION_RTL UINT8_C(0x08)
#define STYLE_WRITING_MODE_SHIFT 4
#define STYLE_WRITING_MODE_MASK UINT8_C(0x30)
#define STYLE_FONT_CH_PENDING UINT8_C(0x40)
#define STYLE_OVERFLOW_X_HIDDEN UINT8_C(0x80)
#define STYLE_BACKGROUND_OVERLAY_MASK UINT16_C(0x00ff)
#define STYLE_BORDER_LINES_SHIFT 8

enum {
    STYLE_FILTER_NONE,
    STYLE_FILTER_GRAYSCALE,
    STYLE_FILTER_INVERT,
    STYLE_FILTER_SEPIA,
    STYLE_FILTER_BRIGHTEN,
    STYLE_FILTER_DARKEN
};

enum {
    STYLE_WRITING_HORIZONTAL_TB,
    STYLE_WRITING_VERTICAL_RL,
    STYLE_WRITING_VERTICAL_LR
};

static inline unsigned computed_style_writing_mode(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_WRITING_HORIZONTAL_TB
        : (style->filter_code & STYLE_WRITING_MODE_MASK)
          >> STYLE_WRITING_MODE_SHIFT;
}

static inline bool computed_style_direction_rtl(const ComputedStyle *style)
{
    return style != NULL && (style->filter_code & STYLE_DIRECTION_RTL) != 0;
}

static inline TextAlign computed_style_used_text_align(
    const ComputedStyle *style)
{
    if (style == NULL) return TEXT_ALIGN_LEFT;
    if (style->text_align == TEXT_ALIGN_START) {
        return computed_style_direction_rtl(style)
            ? TEXT_ALIGN_RIGHT : TEXT_ALIGN_LEFT;
    }
    if (style->text_align == TEXT_ALIGN_END) {
        return computed_style_direction_rtl(style)
            ? TEXT_ALIGN_LEFT : TEXT_ALIGN_RIGHT;
    }
    return style->text_align;
}

typedef enum {
    STYLE_BORDER_TOP,
    STYLE_BORDER_RIGHT,
    STYLE_BORDER_BOTTOM,
    STYLE_BORDER_LEFT,
    STYLE_BORDER_SIDE_COUNT
} StyleBorderSide;

typedef enum {
    STYLE_BORDER_NONE,
    STYLE_BORDER_SOLID,
    STYLE_BORDER_DASHED,
    STYLE_BORDER_DOTTED
} StyleBorderLine;

static inline unsigned computed_style_border_line(
    const ComputedStyle *style, StyleBorderSide side)
{
    if (style == NULL || side >= STYLE_BORDER_SIDE_COUNT) {
        return STYLE_BORDER_NONE;
    }
    return (unsigned) (
        (style->paint_stack_state
         >> (STYLE_BORDER_LINES_SHIFT + (unsigned) side * 2u)) & 3u);
}

static inline void computed_style_set_border_line(
    ComputedStyle *style, StyleBorderSide side, unsigned line)
{
    if (style == NULL || side >= STYLE_BORDER_SIDE_COUNT) return;
    unsigned shift = STYLE_BORDER_LINES_SHIFT + (unsigned) side * 2u;
    style->paint_stack_state = (uint16_t) (
        (style->paint_stack_state & ~(UINT16_C(3) << shift))
        | (((uint16_t) line & UINT16_C(3)) << shift));
}

static inline uint8_t computed_style_border_color_set(
    const ComputedStyle *style)
{
    return style == NULL ? 0u : style->border_color_set;
}

static inline uint8_t computed_style_paint_stack_id(
    const ComputedStyle *style)
{
    return style == NULL ? 0u
        : (uint8_t) (style->paint_stack_state
                     & STYLE_BACKGROUND_OVERLAY_MASK);
}

static inline void computed_style_set_paint_stack_id(
    ComputedStyle *style, uint8_t one_based)
{
    if (style == NULL) return;
    style->paint_stack_state = (uint16_t) (
        (style->paint_stack_state
         & ~STYLE_BACKGROUND_OVERLAY_MASK)
        | (one_based & STYLE_BACKGROUND_OVERLAY_MASK));
}

/* Compatibility names keep the current layout callers small while the
   stylesheet accessor derives the overlay from the shared representation. */
static inline uint16_t computed_style_background_overlay_gradient(
    const ComputedStyle *style)
{
    return computed_style_paint_stack_id(style);
}

static inline void computed_style_set_background_overlay_gradient(
    ComputedStyle *style, uint16_t one_based)
{
    computed_style_set_paint_stack_id(style, (uint8_t) one_based);
}

static inline bool computed_style_overflow_x_hidden(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->filter_code & STYLE_OVERFLOW_X_HIDDEN) != 0;
}

static inline unsigned computed_style_filter_code(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_FILTER_NONE
        : style->filter_code & STYLE_FILTER_CODE_MASK;
}

#define STYLE_OUTLINE_WIDTH_MASK UINT16_C(0x000f)
#define STYLE_OUTLINE_STYLE_SHIFT 4
#define STYLE_OUTLINE_STYLE_MASK UINT16_C(0x0030)
#define STYLE_OUTLINE_OFFSET_SHIFT 6
#define STYLE_OUTLINE_OFFSET_MASK UINT16_C(0x0fc0)
#define STYLE_OUTLINE_OFFSET_LIMIT 32
#define STYLE_OUTLINE_CURRENT_COLOR UINT16_C(0x1000)
/* Declaration-only markers let the packed width/style fields cascade as
   independent longhands without spending another property-mask word. They
   are never copied into the resolved ComputedStyle. */
#define STYLE_OUTLINE_DECL_WIDTH UINT16_C(0x2000)
#define STYLE_OUTLINE_DECL_STYLE UINT16_C(0x4000)

enum {
    STYLE_OUTLINE_NONE,
    STYLE_OUTLINE_SOLID,
    STYLE_OUTLINE_DASHED,
    STYLE_OUTLINE_DOTTED
};

static inline unsigned computed_style_outline_width(
    const ComputedStyle *style)
{
    return style == NULL ? 0u
        : style->outline_state & STYLE_OUTLINE_WIDTH_MASK;
}

static inline unsigned computed_style_outline_style(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_OUTLINE_NONE
        : (style->outline_state & STYLE_OUTLINE_STYLE_MASK)
          >> STYLE_OUTLINE_STYLE_SHIFT;
}

static inline int computed_style_outline_offset(const ComputedStyle *style)
{
    if (style == NULL) return 0;
    unsigned raw = (style->outline_state & STYLE_OUTLINE_OFFSET_MASK)
                   >> STYLE_OUTLINE_OFFSET_SHIFT;
    return raw >= 32u ? (int) raw - 64 : (int) raw;
}

#define STYLE_CLIP_PATH_TYPE_MASK UINT16_C(0x0003)
#define STYLE_CLIP_PATH_INSET_SHIFT 2
#define STYLE_CLIP_PATH_INSET_MASK UINT16_C(0x01fc)
#define STYLE_CLIP_PATH_INSET_PERCENT UINT16_C(0x0200)
#define STYLE_CLIP_PATH_RADIUS_SHIFT 10
#define STYLE_CLIP_PATH_RADIUS_MASK UINT16_C(0xfc00)

enum {
    STYLE_CLIP_PATH_NONE,
    STYLE_CLIP_PATH_INSET,
    STYLE_CLIP_PATH_CIRCLE
};

static inline unsigned computed_style_clip_path_type(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_CLIP_PATH_NONE
        : style->clip_path_state & STYLE_CLIP_PATH_TYPE_MASK;
}

static inline unsigned computed_style_clip_path_inset(
    const ComputedStyle *style)
{
    return style == NULL ? 0u
        : (style->clip_path_state & STYLE_CLIP_PATH_INSET_MASK)
          >> STYLE_CLIP_PATH_INSET_SHIFT;
}

static inline unsigned computed_style_clip_path_radius(
    const ComputedStyle *style)
{
    return style == NULL ? 0u
        : (style->clip_path_state & STYLE_CLIP_PATH_RADIUS_MASK)
          >> STYLE_CLIP_PATH_RADIUS_SHIFT;
}

static inline OverflowWrap computed_style_overflow_wrap(
    const ComputedStyle *style)
{
    return style == NULL ? OVERFLOW_WRAP_NORMAL
        : (OverflowWrap) (style->overflow_wrap & STYLE_OVERFLOW_WRAP_MASK);
}

static inline StyleTextWrap computed_style_text_wrap(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_TEXT_WRAP_NORMAL
        : (StyleTextWrap) ((style->overflow_wrap & STYLE_TEXT_WRAP_MASK)
                           >> STYLE_TEXT_WRAP_SHIFT);
}

static inline StyleUserSelect computed_style_user_select(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_USER_SELECT_AUTO
        : (StyleUserSelect) ((style->overflow_wrap & STYLE_USER_SELECT_MASK)
                             >> STYLE_USER_SELECT_SHIFT);
}

static inline bool computed_style_isolation_isolate(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->overflow_wrap & STYLE_ISOLATION_ISOLATE) != 0;
}

static inline bool computed_style_text_overflow_ellipsis(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->overflow_wrap & STYLE_TEXT_OVERFLOW_ELLIPSIS) != 0;
}

#define STYLE_OVERFLOW_CLIP_MARGIN_MASK UINT8_C(0x1f)
#define STYLE_OVERFLOW_CLIP_MARGIN_EM UINT8_C(0x20)
#define STYLE_OVERFLOW_CLIP_BOX_SHIFT 6
#define STYLE_OVERFLOW_CLIP_BOX_MASK UINT8_C(0xc0)

typedef enum {
    STYLE_OVERFLOW_CLIP_PADDING_BOX = 0,
    STYLE_OVERFLOW_CLIP_CONTENT_BOX = 1,
    STYLE_OVERFLOW_CLIP_BORDER_BOX = 2
} StyleOverflowClipBox;

static inline unsigned computed_style_overflow_clip_margin(
    const ComputedStyle *style)
{
    return style == NULL ? 0
        : style->overflow_clip_margin & STYLE_OVERFLOW_CLIP_MARGIN_MASK;
}

static inline StyleOverflowClipBox computed_style_overflow_clip_box(
    const ComputedStyle *style)
{
    return style == NULL ? STYLE_OVERFLOW_CLIP_PADDING_BOX
        : (StyleOverflowClipBox) (
            (style->overflow_clip_margin & STYLE_OVERFLOW_CLIP_BOX_MASK)
            >> STYLE_OVERFLOW_CLIP_BOX_SHIFT);
}

static inline bool computed_style_grid_auto_flow_column(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->containing_block_reserved
            & STYLE_GRID_AUTO_FLOW_COLUMN) != 0;
}

static inline bool computed_style_grid_auto_flow_dense(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->containing_block_reserved
            & STYLE_GRID_AUTO_FLOW_DENSE) != 0;
}

static inline AlignSelf computed_style_justify_self(
    const ComputedStyle *style)
{
    return style == NULL ? ALIGN_SELF_AUTO
        : (AlignSelf) ((style->appearance & STYLE_JUSTIFY_SELF_MASK)
                       >> STYLE_JUSTIFY_SELF_SHIFT);
}

static inline void computed_style_set_justify_self(
    ComputedStyle *style, AlignSelf value)
{
    if (style == NULL) return;
    style->appearance = (AppearanceMode) (
        (style->appearance & ~STYLE_JUSTIFY_SELF_MASK)
        | (((uint8_t) value << STYLE_JUSTIFY_SELF_SHIFT)
           & STYLE_JUSTIFY_SELF_MASK));
}

static inline bool computed_style_grid_area_is_named(
    const ComputedStyle *style)
{
    return style != NULL
        && (style->grid_placement[2] & UINT8_C(0xf0))
           == COMPUTED_GRID_NAMED_AREA_MARKER;
}

static inline uint8_t computed_style_grid_named_area_id(
    const ComputedStyle *style)
{
    return computed_style_grid_area_is_named(style)
        ? style->grid_placement[0] : 0;
}

static inline void computed_style_set_grid_named_area_id(
    ComputedStyle *style, uint8_t id)
{
    if (style == NULL) return;
    memset(style->grid_placement, 0, sizeof(style->grid_placement));
    style->grid_columns = (int) (
        (uint32_t) style->grid_columns & ~COMPUTED_GRID_NAMED_LINE_MASK);
    if (id != 0) {
        style->grid_placement[0] = id;
        style->grid_placement[2] = COMPUTED_GRID_NAMED_AREA_MARKER;
    }
}

static inline unsigned computed_style_encode_grid_line(int line)
{
    if (line < -6) line = -6;
    if (line < 0) return (unsigned) (16 + line);
    return line > 9 ? 9u : (unsigned) line;
}

static inline bool computed_style_grid_line_is_negative(unsigned line)
{
    return line >= COMPUTED_GRID_NEGATIVE_LINE_MIN;
}

static inline int computed_style_decode_grid_line(unsigned line)
{
    return computed_style_grid_line_is_negative(line)
        ? (int) line - 16 : (int) line;
}

static inline uint8_t computed_style_grid_placement_value(
    const ComputedStyle *style, unsigned shift)
{
    if (style == NULL || shift >= 24) return 0;
    if (computed_style_grid_area_is_named(style)) return 0;
    uint32_t named_flag = 0;
    if (shift == COMPUTED_GRID_ROW_START_SHIFT) {
        named_flag = COMPUTED_GRID_ROW_START_NAMED;
    } else if (shift == COMPUTED_GRID_ROW_END_SHIFT) {
        named_flag = COMPUTED_GRID_ROW_END_NAMED;
    } else if (shift == COMPUTED_GRID_COLUMN_START_SHIFT) {
        named_flag = COMPUTED_GRID_COLUMN_START_NAMED;
    } else if (shift == COMPUTED_GRID_COLUMN_END_SHIFT) {
        named_flag = COMPUTED_GRID_COLUMN_END_NAMED;
    }
    if (((uint32_t) style->grid_columns & named_flag) != 0) return 0;
    unsigned byte = shift / 8u;
    unsigned within = shift % 8u;
    return (uint8_t) ((style->grid_placement[byte] >> within) & 0x0fu);
}

static inline void computed_style_set_grid_placement_value(
    ComputedStyle *style, unsigned shift, unsigned value)
{
    if (style == NULL || shift >= 24) return;
    if (computed_style_grid_area_is_named(style)) {
        memset(style->grid_placement, 0, sizeof(style->grid_placement));
    }
    unsigned byte = shift / 8u;
    unsigned within = shift % 8u;
    uint8_t mask = (uint8_t) (0x0fu << within);
    style->grid_placement[byte] = (uint8_t) (
        (style->grid_placement[byte] & ~mask)
        | ((value & 0x0fu) << within));
    uint32_t named_flag = 0;
    if (shift == COMPUTED_GRID_ROW_START_SHIFT) {
        named_flag = COMPUTED_GRID_ROW_START_NAMED;
    } else if (shift == COMPUTED_GRID_ROW_END_SHIFT) {
        named_flag = COMPUTED_GRID_ROW_END_NAMED;
    } else if (shift == COMPUTED_GRID_COLUMN_START_SHIFT) {
        named_flag = COMPUTED_GRID_COLUMN_START_NAMED;
    } else if (shift == COMPUTED_GRID_COLUMN_END_SHIFT) {
        named_flag = COMPUTED_GRID_COLUMN_END_NAMED;
    }
    style->grid_columns = (int) (
        (uint32_t) style->grid_columns & ~named_flag);
}

#define COMPUTED_GRID_PLACEMENT_ACCESSORS(axis, field, shift)                 \
    static inline uint8_t computed_style_grid_##axis##_##field(               \
        const ComputedStyle *style)                                            \
    {                                                                          \
        return computed_style_grid_placement_value(style, shift);              \
    }                                                                          \
    static inline void computed_style_set_grid_##axis##_##field(               \
        ComputedStyle *style, unsigned value)                                  \
    {                                                                          \
        computed_style_set_grid_placement_value(style, shift, value);          \
    }

COMPUTED_GRID_PLACEMENT_ACCESSORS(
    column, start, COMPUTED_GRID_COLUMN_START_SHIFT)
COMPUTED_GRID_PLACEMENT_ACCESSORS(
    column, end, COMPUTED_GRID_COLUMN_END_SHIFT)
COMPUTED_GRID_PLACEMENT_ACCESSORS(
    column, span, COMPUTED_GRID_COLUMN_SPAN_SHIFT)
COMPUTED_GRID_PLACEMENT_ACCESSORS(
    row, start, COMPUTED_GRID_ROW_START_SHIFT)
COMPUTED_GRID_PLACEMENT_ACCESSORS(
    row, end, COMPUTED_GRID_ROW_END_SHIFT)
COMPUTED_GRID_PLACEMENT_ACCESSORS(
    row, span, COMPUTED_GRID_ROW_SPAN_SHIFT)

#undef COMPUTED_GRID_PLACEMENT_ACCESSORS

/* Gaps reuse one signed integer. Non-negative values are fixed CSS pixels;
   a negative value losslessly retains an integral percentage as -(p + 1).
   This avoids growing every retained ComputedStyle for a relatively uncommon
   value while still resolving percentage column gaps against used width. */
static inline bool computed_style_gap_is_percent(int value)
{
    return value < 0;
}

static inline int computed_style_gap_percent(int value)
{
    return value < 0 ? -value - 1 : 0;
}

static inline int computed_style_resolve_gap(int value, int basis)
{
    if (value >= 0) return value;
    int percent = computed_style_gap_percent(value);
    return basis > 0 ? (int) ((int64_t) basis * percent / 100) : 0;
}

static inline int computed_style_font_size_fixed(const ComputedStyle *style)
{
    if (style == NULL) return 16 * 64;
    int64_t fixed = (int64_t) style->font_size * 64
                    + (style->font_size_fraction & 63u);
    return fixed > INT32_MAX ? INT32_MAX
           : (fixed < INT32_MIN ? INT32_MIN : (int) fixed);
}

static inline int computed_style_root_font_size_fixed(
    const ComputedStyle *style)
{
    return style == NULL || style->root_font_size == 0 ? 16 * 64
           : (int) style->root_font_size * 64
             + (style->root_font_size_fraction & 63u);
}

typedef enum {
    SELECTOR_TAG,
    SELECTOR_CLASS,
    SELECTOR_ID
} SelectorType;

typedef struct {
    ComputedStyle values;
    uint64_t mask;
    uint64_t mask_high;
    /* Properties explicitly set to the CSS-wide `inherit` keyword (S_*
       bits); resolution copies these from the parent's computed style
       after the declaration's concrete values apply. */
    uint64_t inherit_mask;
    char *deferred_declarations;
    size_t deferred_length;
    /* UINT32_MAX retains the exact declaration-string parser fallback.
       Otherwise this indexes the stylesheet's compact, order-preserving
       property/value program. */
    uint32_t deferred_program_offset;
    uint16_t deferred_program_count;
    uint16_t deferred_program_reserved;
} StyleDeclaration;

typedef struct {
    const char *selector;
    uint32_t declaration_index;
    unsigned origin;
    unsigned layer;
    unsigned specificity;
    unsigned order;
    uint16_t selector_length;
    uint16_t fast_key_offset;
    uint16_t rightmost_compound_offset;
    uint8_t fast_key_length;
    uint8_t type;
    uint8_t pseudo;
    /* Slot zero is the document/inline context. External stylesheets retain
       a compact source slot in the existing tail padding so declarations
       reparsed after var() substitution keep the consuming sheet's URL and
       Referrer-Policy without growing StyleRule. */
    uint8_t image_source_slot;
    bool has_fast_key;
    bool important;
} StyleRule;

typedef struct {
    char name[48];
    char value[96];
} StyleVariable;

typedef struct {
    char selector[192];
    char name[48];
    char value[96];
    bool important;
    uint8_t pseudo;
    /* One-based @container definition. This occupies existing alignment
       padding before origin on both host and PSP builds. */
    uint8_t container_query;
    unsigned origin;
    unsigned layer;
    unsigned specificity;
    unsigned order;
} StyleCustomRule;

typedef struct {
    char name[STYLE_DIAGNOSTIC_PROPERTY_NAME_CAPACITY];
    uint32_t count;
} StyleDiagnosticProperty;

typedef struct StyleRuleIndexBucket StyleRuleIndexBucket;
typedef struct StyleRuleFilter StyleRuleFilter;
typedef struct StyleCustomRuleIndexEntry StyleCustomRuleIndexEntry;
typedef struct StyleSelectorInstruction StyleSelectorInstruction;
typedef struct StyleDeferredInstruction StyleDeferredInstruction;
typedef struct StyleRevertRuleMask StyleRevertRuleMask;
typedef struct StyleTextChunk StyleTextChunk;
typedef struct StyleWebFonts StyleWebFonts;
typedef struct StyleResolveScratch StyleResolveScratch;
typedef struct StyleImageSources StyleImageSources;
typedef struct StyleGridAreas StyleGridAreas;
typedef struct StyleGridTrackStorage StyleGridTrackStorage;
typedef struct StyleConditionalQueries StyleConditionalQueries;
typedef struct StylePaintStorage StylePaintStorage;

typedef struct {
    uint32_t colors[STYLE_BORDER_SIDE_COUNT];
    uint8_t alphas[STYLE_BORDER_SIDE_COUNT];
} StyleBorderColors;

#define STYLE_RELATIVE_SELECTOR_CACHE_CAPACITY 8u

typedef struct {
    const char *text;
    lxb_dom_node_t *anchor;
    const lxb_dom_node_t *scope;
    uint32_t epoch;
    uint32_t hash;
    uint16_t length;
    uint8_t functional_depth;
    bool matched;
} StyleRelativeSelectorCacheEntry;

typedef struct {
    Budget *budget;
    /* Monotonic identity for the sheet contents.  The NavigationPage keeps
       this structure at a stable address across transactional rebuilds, so
       pointer identity alone cannot safely key retained computed styles. */
    uint64_t build_generation;
    /* CSP style-src applies independently to style attributes and <style>
       blocks. This bit survives transactional sheet moves without retaining
       a pointer into a movable PocDocument. */
    bool block_inline_style_attributes;
    StyleRule *rules;
    size_t count;
    size_t capacity;
    /* Zero normally. Resource ingestion gives very large individual sheets
       a split head/tail rule ceiling so one generated utility bundle cannot
       consume the layout reserve without discarding all late responsive and
       component rules. Parsing continues between the retained windows; the
       next source gets fresh bounds. */
    size_t source_rule_limit_end;
    size_t source_rule_head_limit_end;
    const char *source_rule_tail_begin;
    /* When an author realm cannot finish within its bounded resources,
       unresolved custom-element light DOM remains useful static content.
       This suppresses content-hiding :not(:defined) fallbacks while keeping
       positive :defined rules standards-exact. */
    bool static_custom_element_fallback;
    /* Monotonic parse summary used to activate the ROM-backed motion module
       without rescanning the DOM or retaining authored CSS in JavaScript. */
    bool has_motion_keyframes;
    /* Lazily materialized exact indices for the uncommon focus-state
       classifier. A representative article retains 25 entries instead of
       rescanning roughly 1,300 rules on every d-pad move. */
    uint32_t *focus_rule_indices;
    size_t focus_rule_count;
    bool focus_rule_index_ready;
    StyleDeclaration *declarations;
    size_t declaration_count;
    size_t declaration_capacity;
    /* Sparse, optional rollback masks for authored rules that use the
       CSS-wide `revert-rule` keyword. Ordinary pages pay only these three
       words in the sheet rather than two masks in every declaration. */
    StyleRevertRuleMask *revert_rule_masks;
    size_t revert_rule_mask_count;
    size_t revert_rule_mask_capacity;
    uint32_t *declaration_index_slots;
    size_t declaration_index_slot_count;
    StyleTextChunk *selector_chunks;
    StyleTextChunk *selector_chunks_tail;
    size_t selector_bytes;
    size_t selector_storage_bytes;
    StyleVariable *variables;
    size_t variable_count;
    size_t variable_capacity;
    StyleCustomRule *custom_rules;
    size_t custom_rule_count;
    size_t custom_rule_capacity;
    size_t deferred_bytes;
    size_t important_rule_count;
    char layer_names[STYLE_LAYER_CAPACITY][STYLE_LAYER_NAME_CAPACITY];
    uint8_t layer_parents[STYLE_LAYER_CAPACITY];
    uint8_t layer_ranks[STYLE_LAYER_CAPACITY];
    size_t layer_count;
    unsigned current_layer;
    uint8_t current_container_query;
    char **generated_texts;
    size_t generated_text_count;
    size_t generated_text_capacity;
    size_t generated_text_bytes;
    StyleCounterOperations *counter_operation_sets;
    size_t counter_operation_set_count;
    size_t counter_operation_set_capacity;
    char **image_urls;
    size_t image_url_count;
    size_t image_url_capacity;
    size_t image_url_bytes;
    /* Allocated only when an external stylesheet actually contributes an
       image-bearing or deferred declaration. */
    StyleImageSources *image_sources;
    StylePaintStorage *paint_storage;
    /* Explicit Grid track lists are uncommon and interned lazily in stable
       small blocks instead of inflating every retained ComputedStyle. */
    StyleGridTrackStorage *grid_tracks;
    /* Allocated only when authored CSS uses named Grid areas. */
    StyleGridAreas *grid_areas;
    /* Allocated only when authored CSS contains @container rules. */
    StyleConditionalQueries *conditional_queries;
    /* Allocated only for declarations whose four border colours differ. */
    StyleBorderColors *border_color_sets;
    size_t border_color_set_count;
    size_t border_color_set_capacity;
    /* Allocated only when an active @font-face is discovered.  Keeping the
       page font state behind a stable pointer makes transactional page moves
       safe without growing every Stylesheet or retained ComputedStyle. */
    StyleWebFonts *web_fonts;
    /* Transient per-resolution scratch (the node being resolved, its
       font-relative bases, and image-source provenance for the declarations
       being parsed).  Allocated once per sheet in stylesheet_build_context;
       internal to the style subsystem and saved/restored around nested
       resolutions there. */
    StyleResolveScratch *resolve_scratch;
    /* Transient layout-owned cooperation state. Unlike resolve_scratch this
       is not saved/restored around nested property reparsing. */
    bool (*selector_cooperate)(
        void *opaque, lxb_dom_node_t *node, size_t completed_visits);
    void *selector_cooperate_opaque;
    size_t selector_cooperate_visits;
    size_t selector_cooperate_next;
    size_t selector_cooperate_quota;
    bool selector_cooperate_cancelled;
    int viewport_width;
    int viewport_height;
    unsigned current_origin;
    unsigned next_order;
    size_t cascade_starts[4];
    size_t cascade_ends[4];
    StyleRuleIndexBucket *rule_index_buckets;
    uint32_t *rule_index_entries;
    /* Optional per-rule Bloom requirements for the directly matched
       compound and its ancestor chain. False positives fall through to the
       exact selector program; false negatives are forbidden. */
    StyleRuleFilter *rule_filters;
    size_t rule_index_bucket_count;
    size_t rule_index_universal_count;
    size_t rule_index_bytes;
    StyleSelectorInstruction *selector_program;
    uint16_t *selector_program_offsets;
    size_t selector_program_instruction_count;
    size_t selector_program_rule_count;
    size_t selector_program_bytes;
    /* Transient, page-owned selector programs imported from immutable
       per-response cache fragments. Offsets are keyed by the stable
       (source-order, importance) identity, so the final cascade sort cannot
       detach a seed from its rule. These buffers are consumed and released
       when the authoritative selector program is prepared. */
    StyleSelectorInstruction *selector_fragment_program;
    uint16_t *selector_fragment_offsets;
    uint16_t *selector_fragment_counts;
    size_t selector_fragment_instruction_count;
    size_t selector_fragment_offset_count;
    /* Transient suffix-append map. Existing program instructions stay owned
       by the sheet while cascade sorting moves their rules; the map restores
       offsets by the stable (source-order, importance) identity. */
    uint16_t *selector_append_offsets;
    size_t selector_append_offset_count;
    bool selector_append_active;
    bool selector_append_restore_rule_index;
    uint64_t selector_append_reused_rules;
    uint64_t selector_append_compiled_rules;
    StyleCustomRuleIndexEntry *custom_rule_index;
    size_t custom_rule_index_count;
    size_t custom_rule_index_bytes;
    StyleMathInstruction *math_instructions;
    StyleMathProgram *math_programs;
    size_t math_instruction_count;
    size_t math_instruction_capacity;
    size_t math_program_count;
    size_t math_program_capacity;
    size_t math_pool_bytes;
    StyleDeferredInstruction *deferred_instructions;
    size_t deferred_instruction_count;
    size_t deferred_instruction_capacity;
    size_t deferred_program_bytes;
    uint64_t rule_index_queries;
    uint64_t rule_index_candidates;
    uint64_t rule_compound_filter_rejections;
    uint64_t rule_ancestor_filter_rejections;
    uint64_t rule_index_fallbacks;
    uint64_t variable_lookup_calls;
    uint64_t variable_rule_candidates;
    uint64_t variable_cache_hits;
    uint64_t variable_cache_misses;
    uint64_t variable_cache_negative_hits;
    uint64_t variable_cache_evictions;
    size_t variable_cache_peak_bytes;
    uint64_t deferred_rule_applications;
    uint64_t deferred_rule_us;
    uint64_t deferred_program_executions;
    uint64_t deferred_program_fallbacks;
    uint64_t deferred_program_instructions;
    uint64_t selector_match_calls;
    uint64_t selector_match_successes;
    uint64_t selector_match_characters;
    uint64_t selector_compound_calls;
    uint64_t selector_attribute_checks;
    uint64_t selector_pseudo_checks;
    uint64_t selector_ancestor_visits;
    uint64_t selector_sibling_visits;
    uint64_t selector_descendant_visits;
    uint64_t selector_relative_queries;
    uint64_t selector_relative_cache_hits;
    uint64_t selector_relative_walks;
    uint64_t selector_relative_visits;
    uint64_t selector_relative_exhaustions;
    uint64_t selector_relative_max_visits;
    uint64_t selector_compiled_rule_calls;
    uint64_t selector_fallback_rule_calls;
    uint64_t selector_compiled_rule_matches;
    uint64_t selector_subject_cache_hits;
    uint64_t selector_subject_cache_misses;
    uint64_t selector_tag_id_checks;
    uint64_t diagnostic_declarations;
    uint64_t diagnostic_supported_declarations;
    uint64_t diagnostic_rejected_declarations;
    uint64_t diagnostic_deferred_declarations;
    uint64_t diagnostic_custom_property_drops;
    uint64_t diagnostic_selector_drops;
    uint64_t diagnostic_unknown_media_features;
    uint64_t diagnostic_supports_false_queries;
    StyleDiagnosticProperty diagnostic_rejected_properties[
        STYLE_DIAGNOSTIC_PROPERTY_CAPACITY];
    size_t diagnostic_rejected_property_count;
    /* Cached TILEFINCH_TRACE_* environment state, resolved once per build so
       rule ingestion and resolution never call getenv().  Zero when tracing
       is compiled out (TILEFINCH_NO_TRACE) or the variables are unset. */
    uint8_t trace_flags;
    bool rule_index_ready;
    bool rule_index_attempted;
    bool rule_ancestor_filter_active;
    bool selector_program_ready;
    bool selector_program_attempted;
    bool custom_rule_index_ready;
    bool custom_rule_index_attempted;
    bool document_rules_deferred;
    bool rule_batch_active;
    bool rule_batch_dirty;
    bool has_multicolumn_rules;
    bool has_container_relative_units;
    bool has_scroll_interaction_rules;
    bool has_cursor_rules;
    /* Bitset of sparse modern mobile declarations present in this sheet.
       Nodes on ordinary pages skip the retained-property pass entirely. */
    uint16_t modern_property_mask;
    uint8_t relative_selector_cache_depth;
    uint32_t relative_selector_cache_epoch;
    StyleRelativeSelectorCacheEntry relative_selector_cache[
        STYLE_RELATIVE_SELECTOR_CACHE_CAPACITY];
} Stylesheet;

bool stylesheet_build(Stylesheet *sheet, Budget *budget,
                      const PocDocument *document, int viewport_width);
/* Returns active, non-pseudo selectors whose declaration makes the matched
   element a flex/grid/table layout container. Pointers remain owned by sheet. */
size_t stylesheet_layout_island_selectors(const Stylesheet *sheet,
                                          const char **selectors,
                                          size_t capacity);
bool stylesheet_build_context(Stylesheet *sheet, Budget *budget,
                              const PocDocument *document,
                              const ViewportContext *viewport);
/* Initializes a sheet at the document viewport without parsing inline author
   rules. Use when the resource pipeline will immediately consume all <style>
   and <link> inputs in DOM order, avoiding a throwaway inline parse. */
bool stylesheet_build_context_deferred(
    Stylesheet *sheet, Budget *budget, const PocDocument *document,
    const ViewportContext *viewport);
/* Reinitializes a built sheet at the same viewport with no author rules.
   The external-resource pipeline uses this before replaying <style> and
   <link> inputs in actual DOM order. Existing sheet contents are discarded. */
bool stylesheet_reset_document_rules(Stylesheet *sheet);
/* Resource discovery may append several document-ordered <style>/<link>
   sources before any resolver can observe the sheet. Batch mode preserves
   parsing and source order while sorting cascade storage only once. */
bool stylesheet_begin_rule_batch(Stylesheet *sheet);
bool stylesheet_end_rule_batch(Stylesheet *sheet);
/* Adds one inline <style> source at the current author-cascade position. */
bool stylesheet_add_style_element(
    Stylesheet *sheet, lxb_dom_node_t *element,
    const TilefinchContentSecurityPolicy *content_security_policy);
bool stylesheet_build_viewport(Stylesheet *sheet, Budget *budget,
                               const PocDocument *document,
                               int viewport_width, int viewport_height);
bool stylesheet_add_css(Stylesheet *sheet, const char *css, size_t length);
void stylesheet_enable_static_custom_element_fallback(Stylesheet *sheet);
bool stylesheet_add_css_from(Stylesheet *sheet, const char *css,
                             size_t length, const char *source_base_url);
bool stylesheet_add_css_from_context(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy);
/* Parses CSS normally while optionally retaining a bounded, pointer-free
   structural IR for the final selector/declaration operations. The IR is
   viewport-specific and is deliberately unavailable for constructs whose
   identities depend on the surrounding sheet (layers, container queries,
   and font faces). The caller owns successful output through the stylesheet
   budget. */
bool stylesheet_add_css_from_context_capture_ir(
    Stylesheet *sheet, const char *css, size_t length,
    const char *source_base_url, const char *source_referrer_policy,
    unsigned char **ir_data, size_t *ir_length);
typedef enum {
    STYLE_PARSED_IR_REJECTED = 0,
    STYLE_PARSED_IR_APPLIED,
    STYLE_PARSED_IR_FAILED
} StyleParsedIrApplyResult;
/* Replays a validated structural IR into a fresh stylesheet context. This
   still parses declarations against the destination sheet's own intern
   tables, preserving cascade, URL provenance, variables, and bounded
   resource admission while skipping the recursive CSS structure walk. */
StyleParsedIrApplyResult stylesheet_add_parsed_ir_from_context(
    Stylesheet *sheet, const unsigned char *ir_data, size_t ir_length,
    const char *source_base_url, const char *source_referrer_policy,
    size_t *operation_count);
bool stylesheet_parsed_ir_matches(
    const Stylesheet *sheet, const unsigned char *ir_data, size_t ir_length);
/* Appends parser-discovered inline <style> elements to an existing sheet in
   the supplied document order.  The caller must prove that the prior style
   and link inputs are an unchanged prefix; on failure the sheet is
   conservatively discard-only rather than transactionally restored. */
bool stylesheet_append_style_elements(
    Stylesheet *sheet, lxb_dom_node_t *const *elements, size_t count,
    const TilefinchContentSecurityPolicy *content_security_policy);
/* State which can change how a subsequently parsed stylesheet compiles.
   Incremental appenders compare this before/after their suffix and discard
   the fast path if variables, layer ordering, or web-font namespaces moved. */
uint64_t stylesheet_parse_context_signature(const Stylesheet *sheet);

/* Builds and consumes an immutable selector-program fragment for a single
   already-parsed CSS response. The binary representation owns no stylesheet
   pointers and is safe to retain in the RAM-only HTTP cache. Applying a
   fragment first verifies the exact rule/selector sequence; a context- or
   viewport-dependent mismatch is a normal miss and never changes the sheet. */
bool stylesheet_compiled_fragment_build(
    Stylesheet *sheet, size_t rule_begin, size_t rule_end,
    unsigned char **data, size_t *length);
size_t stylesheet_compiled_fragment_apply(
    Stylesheet *sheet, size_t rule_begin, size_t rule_end,
    const unsigned char *data, size_t length);

/* Bounded generated-content helpers shared with layout.  Returned counter
   operation storage remains owned by the stylesheet. */
const StyleCounterOperations *style_counter_operations(
    const Stylesheet *sheet, uint8_t id);
size_t style_decode_generated_text(const char *value, char *output,
                                   size_t capacity);
bool stylesheet_add_user_css(Stylesheet *sheet, const char *css, size_t length);
void stylesheet_destroy(Stylesheet *sheet);
/* Clears document-addressed transient matcher/layout state before a compiled
   sheet is moved into the RAM-only navigation cache. Authored rules and
   indices are retained; no source or compiled data is serialized. */
void stylesheet_prepare_for_document_reuse(Stylesheet *sheet);
size_t stylesheet_web_font_source_count(const Stylesheet *sheet);
bool stylesheet_web_font_source(const Stylesheet *sheet, size_t index,
                                StylesheetWebFontSource *source);
FontFace *stylesheet_web_font_face(Stylesheet *sheet, unsigned family_slot,
                                   bool bold);
const WebFontSet *stylesheet_web_font_set(const Stylesheet *sheet);
bool stylesheet_web_font_stats(const Stylesheet *sheet,
                               StylesheetWebFontStats *stats);
/* Returns the request provenance attached to an interned CSS image
   reference. Inline/document references succeed with NULL source outputs;
   external references expose the retained final stylesheet response URL and
   normalized response Referrer-Policy. */
bool stylesheet_image_url_source(const Stylesheet *sheet,
                                 const char *reference,
                                 const char **source_base_url,
                                 const char **source_referrer_policy);
const StyleGradient *stylesheet_background_overlay_gradient(
    const Stylesheet *sheet, uint16_t one_based_id);
const StyleGradient *stylesheet_background_gradient(
    const Stylesheet *sheet, const ComputedStyle *style);
size_t stylesheet_box_shadow_count(
    const Stylesheet *sheet, const ComputedStyle *style);
const StyleBoxShadow *stylesheet_box_shadow(
    const Stylesheet *sheet, const ComputedStyle *style, size_t index);
/* Returns the uniform radius directly or a packed four-corner code
   understood by the display-list helpers in layout.h. */
int stylesheet_border_radius_code(
    const Stylesheet *sheet, const ComputedStyle *style);
/* Named Grid areas use a stylesheet-local bounded table and add no bytes to
   the per-node style record. The resolver mutates only the caller's style
   copy, translating a matching name into ordinary numeric placement. */
unsigned stylesheet_grid_template_area_rows(
    const Stylesheet *sheet, const ComputedStyle *container);
unsigned stylesheet_grid_template_area_columns(
    const Stylesheet *sheet, const ComputedStyle *container);
bool stylesheet_resolve_named_grid_area(
    const Stylesheet *sheet, const ComputedStyle *container,
    ComputedStyle *item);
bool stylesheet_serialize_grid_template_areas(
    const Stylesheet *sheet, const ComputedStyle *container,
    char *output, size_t capacity);
unsigned stylesheet_grid_track_count(
    const Stylesheet *sheet, const ComputedStyle *container, bool rows);
uint8_t stylesheet_grid_track_type(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index);
unsigned stylesheet_grid_track_value(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index);
unsigned stylesheet_grid_track_minimum(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned index);
uint8_t stylesheet_grid_track_line_name(
    const Stylesheet *sheet, const ComputedStyle *container,
    bool rows, unsigned line, unsigned slot);
bool stylesheet_resolve_named_grid_lines(
    const Stylesheet *sheet, const ComputedStyle *container,
    ComputedStyle *item);
bool stylesheet_serialize_grid_template_tracks(
    const Stylesheet *sheet, const ComputedStyle *container, bool rows,
    char *output, size_t capacity);
uint32_t stylesheet_border_color(
    const Stylesheet *sheet, const ComputedStyle *style,
    StyleBorderSide side, uint8_t *alpha);
ComputedStyle style_for_node(const Stylesheet *sheet, lxb_dom_node_t *node,
                             const ComputedStyle *parent);
/*
 * Resolves a node with and without Tilefinch's internal focus marker. True
 * means the two computed styles differ only in CSS outline fields and no
 * relational/pseudo-element focus selector can affect another box. The DOM
 * marker is restored before return.
 */
bool style_focus_change_is_outline_only(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused);
/* True when changing one attribute value can alter any selector containing
   :has(), including a changed class/id token outside the relative selector.
   The caller supplies both values before mutating the DOM so removals are
   classified as precisely as additions. */
bool stylesheet_attribute_change_may_affect_has(
    const Stylesheet *sheet, const char *name, size_t name_length,
    const char *old_value, size_t old_length,
    const char *new_value, size_t new_length);
typedef enum {
    STYLE_FOCUS_CHANGE_UNSAFE = 0,
    STYLE_FOCUS_CHANGE_OUTLINE_ONLY,
    /*
     * Geometry and every inherited/descendant-visible computed field are
     * unchanged. Only outline and a uniformly painted rounded border differ;
     * inset shadows may also differ because Tilefinch deliberately does not
     * emit them into the display list.
     */
    STYLE_FOCUS_CHANGE_BORDER_PAINT_ONLY
} StyleFocusChange;
StyleFocusChange style_focus_change_classify(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, ComputedStyle *normal,
    ComputedStyle *focused);
/* Layout-only second pass for font-metric-dependent `ch` units. */
ComputedStyle style_for_node_with_ch_basis(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const ComputedStyle *parent, int ch_basis);
ComputedStyle style_for_pseudo(const Stylesheet *sheet, lxb_dom_node_t *node,
                               PseudoElement pseudo,
                               const ComputedStyle *parent);
/* Resolves the winning, inherited custom property for CSSOM inspection
   without retaining it in every ComputedStyle. */
bool style_custom_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node, PseudoElement pseudo,
    const char *name, size_t name_length, char *output, size_t output_size);
/* Resolves a retained presentation declaration on exactly this element.
   Tilefinch keeps the small SVG paint subset outside ComputedStyle so pages
   without inline SVG pay no per-node memory cost. Presentation attributes
   and SVG inheritance are applied by the inline-SVG consumer. */
bool style_retained_presentation_value(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size);
/* Resolves any declaration deliberately retained outside ComputedStyle.
   This includes the sparse scroll-container and pointer properties as well
   as SVG presentation values. Callers must use a property collected by the
   stylesheet parser; unknown properties deterministically return false. */
bool style_retained_property_value(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *name, size_t name_length, char *output, size_t output_size);
#define STYLE_RETAINED_BOX_VALUE_CAPACITY 96u
typedef struct {
    uint8_t present_mask;
    char values[4][STYLE_RETAINED_BOX_VALUE_CAPACITY];
} StyleRetainedBoxValues;
/* Resolves a retained physical box shorthand and its four longhands as one
   cascade. This preserves source order between shorthand and longhand rules
   without adding those uncommon values to every ComputedStyle. Values are
   ordered top, right, bottom, left. */
bool style_retained_box_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *shorthand, StyleRetainedBoxValues *output);
typedef struct {
    uint8_t present_mask;
    char values[2][STYLE_RETAINED_BOX_VALUE_CAPACITY];
} StyleRetainedPairValues;
/* Resolves a retained one/two-value shorthand and two named longhands as one
   cascade. Values remain in the shorthand's authored first/second order. */
bool style_retained_pair_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    const char *shorthand, const char *first_longhand,
    const char *second_longhand, StyleRetainedPairValues *output);
#define STYLE_RETAINED_PRESENTATION_COUNT 12u
#define STYLE_RETAINED_PRESENTATION_VALUE_CAPACITY 96u
typedef struct {
    uint16_t present_mask;
    char values[STYLE_RETAINED_PRESENTATION_COUNT]
               [STYLE_RETAINED_PRESENTATION_VALUE_CAPACITY];
} StyleRetainedPresentationValues;
const char *style_retained_presentation_name(size_t index);
/* Resolves the complete retained SVG paint subset in one bounded cascade
   pass. The inline-SVG consumer uses this to avoid twelve selector and inline
   declaration scans per element. */
bool style_retained_presentation_values(
    const Stylesheet *sheet, lxb_dom_node_t *node,
    StyleRetainedPresentationValues *output);
/* Bounded size-container geometry is layout-owned transient scratch rather
   than retained per-node style state. */
bool stylesheet_has_container_queries(const Stylesheet *sheet);
bool style_container_layout_state_begin(Stylesheet *sheet, Budget *budget,
                                        size_t expected_containers);
bool style_container_layout_state_add(Stylesheet *sheet,
                                      lxb_dom_node_t *node,
                                      int content_width,
                                      int content_height,
                                      int padding_horizontal,
                                      int padding_vertical);
void style_container_layout_state_finish(Stylesheet *sheet);
void style_container_layout_state_clear(Stylesheet *sheet);
uint64_t style_container_layout_state_signature(const Stylesheet *sheet);
bool computed_style_has_text_underline(const ComputedStyle *style);
bool computed_style_has_ancestor_text_underline(const ComputedStyle *style);
/* Returns false for the computed `auto` value, otherwise writes the bounded
   signed pixel offset. */
bool computed_style_text_underline_offset(const ComputedStyle *style,
                                          int *pixels);
/* Selects the offset belonging to the decoration this compact renderer will
   paint.  An element-originated line wins over a propagated ancestor line. */
bool computed_style_effective_text_underline_offset(
    const ComputedStyle *style, int *pixels);
bool style_selector_matches(lxb_dom_node_t *node, const char *selector,
                            size_t selector_length);
bool style_selector_matches_scoped(lxb_dom_node_t *node,
                                   const char *selector,
                                   size_t selector_length,
                                   const lxb_dom_node_t *scope);
bool stylesheet_media_matches(const Stylesheet *sheet, const char *query,
                              size_t query_length);
bool stylesheet_supports_matches(Stylesheet *sheet, const char *query,
                                 size_t query_length);
/* Parses a standalone CSS color without stylesheet variable resolution.
   Canvas and other non-cascade consumers use this entry point so they share
   the renderer's color grammar without constructing a throwaway sheet. */
bool style_color_parse(const char *text, size_t length,
                       uint32_t *color, uint8_t *alpha);
const StyleDeclaration *stylesheet_rule_declaration(
    const Stylesheet *sheet, const StyleRule *rule);
size_t stylesheet_retained_bytes(const Stylesheet *sheet);
/* Resolve a packed fixed/percentage/math length against its CSS percentage
   basis.  This is allocation-free and leaves keyword handling to the
   property-specific caller. */
bool style_length_resolve(const Stylesheet *sheet, StyleLength value,
                          int reference, int *pixels);
/* Parses a CSS length into used pixels for sparse, non-cascade consumers.
   Percentages are returned as their numeric percentage and marked through
   percent; callers apply the property-specific reference basis. */
int style_parse_length(const Stylesheet *sheet, const char *text,
                       size_t length, int fallback, bool *percent);

#endif
