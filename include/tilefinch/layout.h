#ifndef TILEFINCH_LAYOUT_H
#define TILEFINCH_LAYOUT_H

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/document.h"
#include "tilefinch/resources.h"
#include "tilefinch/style.h"
#include "tilefinch/viewport.h"

#define LAYOUT_SPATIAL_BAND_HEIGHT 128
#define LAYOUT_GRADIENT_LIMIT 32
#define LAYOUT_VISUAL_PRIORITY_LIMIT 128
#define LAYOUT_GENERATED_TEXT_LIMIT 4096
#define LAYOUT_COMMAND_FIXED UINT8_C(1)
#define LAYOUT_COMMAND_OVERFLOW UINT8_C(2)
#define LAYOUT_COMMAND_DYNAMIC_OVERFLOW UINT8_C(4)
#define LAYOUT_COMMAND_CLIPPED_X UINT8_C(8)
/* A positioned command whose authored paint order follows intersecting
   overflow content. It joins that bounded per-frame stream rather than being
   baked underneath it in a cached tile. */
#define LAYOUT_COMMAND_LATE_POSITIONED UINT8_C(16)
#define LAYOUT_IMAGE_FIT_STRETCH UINT8_C(0)
#define LAYOUT_IMAGE_FIT_COVER UINT8_C(1)
#define LAYOUT_IMAGE_FIT_CONTAIN UINT8_C(2)
/* Natural-size background modes: the image paints 1:1 at a source
   offset and the command box clips it; the tiled variants repeat along
   the named axes (CSS background-repeat initial is repeat). */
#define LAYOUT_IMAGE_FIT_SPRITE UINT8_C(3)
#define LAYOUT_IMAGE_FIT_SPRITE_TILE_X UINT8_C(4)
#define LAYOUT_IMAGE_FIT_SPRITE_TILE_Y UINT8_C(5)
#define LAYOUT_IMAGE_FIT_SPRITE_TILE_XY UINT8_C(6)
#define LAYOUT_IMAGE_FIT_NONE UINT8_C(7)
#define LAYOUT_IMAGE_FIT_SCALE_DOWN UINT8_C(8)
#define LAYOUT_STROKE_SOLID UINT8_C(0)
#define LAYOUT_STROKE_DASHED UINT8_C(1)
#define LAYOUT_STROKE_DOTTED UINT8_C(2)
/* image_fit is otherwise unused by text commands, so the retained underline
   bit costs no display-list bytes on the PSP. */
#define LAYOUT_TEXT_DECORATION_UNDERLINE UINT8_C(4)
/* The remaining text-only bits encode auto (zero) or an integer offset from
   -15px through +15px.  Image fit values never use these bits. */
#define LAYOUT_TEXT_UNDERLINE_OFFSET_SHIFT 3
#define LAYOUT_TEXT_UNDERLINE_OFFSET_MASK UINT8_C(0xf8)
#define LAYOUT_TEXT_X_FRACTION_MASK 63
#define LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT 6
#define LAYOUT_TEXT_FONT_SIZE_FRACTION_MASK (63 << 6)
#define LAYOUT_TEXT_TRANSFORM_SHIFT 12
#define LAYOUT_TEXT_TRANSFORM_MASK (3 << LAYOUT_TEXT_TRANSFORM_SHIFT)
/* Nested atomic inline boxes flush their own line before the containing line.
   Mark those text commands so the outer baseline pass remains linear and
   does not translate child-box contents a second time. */
#define LAYOUT_TEXT_BASELINE_ALIGNED (1 << 14)
/* A bounded bidi run is rasterized in visual RTL order. The bit occupies the
   last text-only radius bit and does not grow the display list. */
#define LAYOUT_TEXT_RTL (1 << 15)
/* Text-shadow commands reuse the otherwise vacant high radius bits. They
   remain ordinary DRAW_TEXT entries so glyph caching, transforms, clipping,
   bidi and fallback fonts stay on one path without growing DrawCommand. */
#define LAYOUT_TEXT_SHADOW (1 << 16)
#define LAYOUT_TEXT_SHADOW_BLUR_SHIFT 17
#define LAYOUT_TEXT_SHADOW_BLUR_MASK (63 << LAYOUT_TEXT_SHADOW_BLUR_SHIFT)
/* Find-in-page reconstructs collapsed whitespace from retained text runs.
   These text-only bits preserve the two boundaries that geometry cannot:
   authored whitespace before a run and the start of a block flow. */
#define LAYOUT_TEXT_FIND_SPACE_BEFORE (1 << 23)
#define LAYOUT_TEXT_FIND_BLOCK_START (1 << 24)
/* Authored font-weight is retained divided by ten and is bounded at 100.
   Its spare high bit carries the positioned paint phase without growing the
   PSP display list. */
#define LAYOUT_COMMAND_FONT_WEIGHT_MASK UINT8_C(0x7f)
#define LAYOUT_COMMAND_POSITIONED_PHASE UINT8_C(0x80)
#define LAYOUT_COMMAND_FONT_FAMILY_MASK UINT8_C(0x1f)
#define LAYOUT_COMMAND_FILTER_SHIFT 5
#define LAYOUT_COMMAND_FILTER_MASK UINT8_C(0xe0)
#define LAYOUT_FILL_GRADIENT_AS_MASK UINT8_C(1)

static inline int layout_scale_radius_code(
    int code, int numerator, int denominator)
{
    if (denominator <= 0 || numerator <= 0) return 0;
    if (!style_border_radius_is_packed(code)) {
        if (code <= 0) return 0;
        int64_t scaled = (int64_t) code * numerator;
        scaled = (scaled + denominator - 1) / denominator;
        return scaled > INT_MAX ? INT_MAX : (int) scaled;
    }
    int values[4];
    bool uniform = true;
    for (unsigned corner = 0; corner < 4; corner++) {
        int64_t scaled =
            (int64_t) style_border_radius_corner(code, corner) * numerator;
        scaled = (scaled + denominator - 1) / denominator;
        if (scaled > 127) scaled = 127;
        values[corner] = (int) scaled;
        if (corner != 0 && values[corner] != values[0]) uniform = false;
    }
    return uniform ? values[0]
        : style_border_radius_pack(
              values[0], values[1], values[2], values[3]);
}

typedef enum {
    DRAW_FILL_RECT,
    DRAW_TEXT,
    DRAW_IMAGE,
    DRAW_STROKE_RECT,
    /* One `box-shadow` layer.  x/y/width/height are the OUTER bounds -- the
       spread-adjusted shadow rect already inflated by the blur radius -- so
       the existing intersects()/tile cull sees the full painted extent
       without a shadow special case.  scale carries the blur radius, and
       radius is the shadow rect's corner radius; the rasteriser recovers the
       shadow rect by deflating the command box by scale on every side. */
    DRAW_SHADOW_RECT
} DrawCommandType;

typedef struct {
    const char *text;
    const ImageResource *image;
    uint32_t text_length;
    uint32_t color;
    int x;
    int y;
    int width;
    int height;
    int radius;
    int z_index;
    int scale;
    int font_size;
    uint16_t opacity_scale;
    uint8_t type;
    uint8_t font_family;
    uint8_t font_weight;
    /* Bit 0 is authored italic. Bits 1..2 carry an optional quarter-turn
       paint transform, reusing padding in the PSP display command. */
    uint8_t font_italic;
    int8_t letter_spacing;
    uint8_t image_fit;
} DrawCommand;

#define LAYOUT_COMMAND_FONT_ITALIC UINT8_C(1)
#define LAYOUT_COMMAND_ROTATION_SHIFT 1
#define LAYOUT_COMMAND_ROTATION_MASK UINT8_C(6)

static inline bool draw_command_font_italic(const DrawCommand *command)
{
    return command != NULL
        && (command->font_italic & LAYOUT_COMMAND_FONT_ITALIC) != 0;
}

static inline unsigned draw_command_rotation_quadrants(
    const DrawCommand *command)
{
    return command == NULL ? 0u
        : (command->font_italic & LAYOUT_COMMAND_ROTATION_MASK)
          >> LAYOUT_COMMAND_ROTATION_SHIFT;
}

static inline void draw_command_set_rotation_quadrants(
    DrawCommand *command, unsigned quadrants)
{
    if (command == NULL) return;
    command->font_italic = (uint8_t) (
        (command->font_italic & ~LAYOUT_COMMAND_ROTATION_MASK)
        | ((quadrants << LAYOUT_COMMAND_ROTATION_SHIFT)
           & LAYOUT_COMMAND_ROTATION_MASK));
}

static inline FontFamily draw_command_font_family(
    const DrawCommand *command)
{
    return command == NULL ? FONT_SANS
        : (FontFamily) (command->font_family
                        & LAYOUT_COMMAND_FONT_FAMILY_MASK);
}

static inline unsigned draw_command_filter_code(
    const DrawCommand *command)
{
    return command == NULL ? 0u
        : (command->font_family & LAYOUT_COMMAND_FILTER_MASK)
          >> LAYOUT_COMMAND_FILTER_SHIFT;
}

static inline void draw_command_set_filter_code(
    DrawCommand *command, unsigned code)
{
    if (command == NULL) return;
    command->font_family = (uint8_t) (
        (command->font_family & LAYOUT_COMMAND_FONT_FAMILY_MASK)
        | ((code << LAYOUT_COMMAND_FILTER_SHIFT)
           & LAYOUT_COMMAND_FILTER_MASK));
}

static inline uint8_t draw_command_font_weight_code(
    const DrawCommand *command)
{
    return command == NULL ? 0
           : command->font_weight & LAYOUT_COMMAND_FONT_WEIGHT_MASK;
}

static inline bool draw_command_has_positioned_phase(
    const DrawCommand *command)
{
    return command != NULL
           && (command->font_weight & LAYOUT_COMMAND_POSITIONED_PHASE) != 0;
}

/* font_size is unused by DRAW_FILL_RECT (only DRAW_TEXT ever writes it), so
   a fill rect retains its axial-gradient slot there rather than growing the
   PSP display list.  Zero means a plain solid fill; otherwise the value is
   one past the index into LayoutDocument.gradients. */
static inline bool draw_command_fill_gradient(const DrawCommand *command,
                                              size_t *slot)
{
    if (command == NULL || command->type != DRAW_FILL_RECT
        || command->font_size <= 0) return false;
    if (slot != NULL) *slot = (size_t) command->font_size - 1u;
    return true;
}

static inline void draw_command_set_fill_gradient(DrawCommand *command,
                                                  size_t slot)
{
    if (command == NULL) return;
    command->font_size = (int) slot + 1;
}

/* radius and font_size are unused by DRAW_IMAGE; sprite commands retain
   the source-pixel offset in them.  text_length is likewise unused by image
   commands, so scaled sprite sheets pack their authored CSS width/height in
   its two 16-bit halves.  Zero keeps the image's natural CSS size. */
static inline int draw_command_image_offset_x(const DrawCommand *command)
{
    return command == NULL ? 0 : command->radius;
}

static inline int draw_command_image_offset_y(const DrawCommand *command)
{
    return command == NULL ? 0 : command->font_size;
}

static inline void draw_command_set_image_offset(DrawCommand *command,
                                                 int offset_x, int offset_y)
{
    if (command == NULL) return;
    command->radius = offset_x;
    command->font_size = offset_y;
}

static inline int draw_command_image_sprite_width(
    const DrawCommand *command)
{
    return command == NULL ? 0
        : (int) (command->text_length & UINT32_C(0xffff));
}

static inline int draw_command_image_sprite_height(
    const DrawCommand *command)
{
    return command == NULL ? 0 : (int) (command->text_length >> 16);
}

static inline void draw_command_set_image_sprite_size(
    DrawCommand *command, int width, int height)
{
    if (command == NULL) return;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (width > UINT16_MAX) width = UINT16_MAX;
    if (height > UINT16_MAX) height = UINT16_MAX;
    command->text_length = (uint32_t) (uint16_t) width
                           | ((uint32_t) (uint16_t) height << 16);
}

/* scale is otherwise unused by DRAW_IMAGE. Background and mask commands
   retain their paint-box corner radius there because sprite positioning
   already occupies radius/font_size. */
static inline int draw_command_image_clip_radius(const DrawCommand *command)
{
    return command == NULL || command->type != DRAW_IMAGE ? 0
        : command->scale;
}

/* radius/font_size are also free on ordinary replaced-image commands.
   Percent object positions use them only for fit modes below the sprite
   range or the explicit none/scale-down modes. */
static inline void draw_command_set_object_position(
    DrawCommand *command, uint16_t x_position, uint16_t y_position)
{
    if (command == NULL) return;
    command->radius = x_position;
    command->font_size = y_position;
}

/* image_fit doubles as the DRAW_TEXT decoration bit set (see
   LAYOUT_TEXT_DECORATION_* above); use these accessors rather than testing
   the overloaded field directly. */
static inline bool draw_command_text_has_underline(const DrawCommand *command)
{
    return command != NULL
           && (command->image_fit & LAYOUT_TEXT_DECORATION_UNDERLINE) != 0;
}

static inline void draw_command_set_text_underline_offset(
    DrawCommand *command, int pixels)
{
    if (command == NULL) return;
    if (pixels < -15) pixels = -15;
    if (pixels > 15) pixels = 15;
    command->image_fit = (uint8_t) (
        (command->image_fit & ~LAYOUT_TEXT_UNDERLINE_OFFSET_MASK)
        | ((pixels + 16) << LAYOUT_TEXT_UNDERLINE_OFFSET_SHIFT));
}

static inline bool draw_command_text_underline_offset(
    const DrawCommand *command, int *pixels)
{
    if (command == NULL) return false;
    unsigned code = (command->image_fit
                     & LAYOUT_TEXT_UNDERLINE_OFFSET_MASK)
                    >> LAYOUT_TEXT_UNDERLINE_OFFSET_SHIFT;
    if (code == 0) return false;
    if (pixels != NULL) *pixels = (int) code - 16;
    return true;
}

/* radius is unused by DRAW_TEXT. Its low six bits retain the command's 26.6
   x fraction without changing the PSP display-list footprint. */
static inline int draw_command_text_x_fixed(const DrawCommand *command)
{
    if (command == NULL) return 0;
    int64_t fixed = (int64_t) command->x * 64
        + (command->type == DRAW_TEXT
           ? command->radius & LAYOUT_TEXT_X_FRACTION_MASK : 0);
    return fixed > INT32_MAX ? INT32_MAX
           : (fixed < INT32_MIN ? INT32_MIN : (int) fixed);
}

static inline TextTransformMode draw_command_text_transform(
    const DrawCommand *command)
{
    return command == NULL || command->type != DRAW_TEXT
        ? TEXT_TRANSFORM_NONE
        : (TextTransformMode) ((command->radius
                               & LAYOUT_TEXT_TRANSFORM_MASK)
                              >> LAYOUT_TEXT_TRANSFORM_SHIFT);
}

static inline bool draw_command_text_baseline_aligned(
    const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && (command->radius & LAYOUT_TEXT_BASELINE_ALIGNED) != 0;
}

static inline bool draw_command_text_rtl(const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && (command->radius & LAYOUT_TEXT_RTL) != 0;
}

static inline bool draw_command_is_text_shadow(
    const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && (command->radius & LAYOUT_TEXT_SHADOW) != 0;
}

static inline bool draw_command_text_find_space_before(
    const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && (command->radius & LAYOUT_TEXT_FIND_SPACE_BEFORE) != 0;
}

static inline bool draw_command_text_find_block_start(
    const DrawCommand *command)
{
    return command != NULL && command->type == DRAW_TEXT
        && (command->radius & LAYOUT_TEXT_FIND_BLOCK_START) != 0;
}

static inline int draw_command_text_shadow_blur(
    const DrawCommand *command)
{
    return !draw_command_is_text_shadow(command) ? 0
        : (command->radius & LAYOUT_TEXT_SHADOW_BLUR_MASK)
          >> LAYOUT_TEXT_SHADOW_BLUR_SHIFT;
}

static inline void draw_command_set_text_shadow_blur(
    DrawCommand *command, int blur)
{
    if (command == NULL || command->type != DRAW_TEXT) return;
    if (blur < 0) blur = 0;
    if (blur > 63) blur = 63;
    command->radius = (command->radius
                       & ~LAYOUT_TEXT_SHADOW_BLUR_MASK)
        | LAYOUT_TEXT_SHADOW
        | (blur << LAYOUT_TEXT_SHADOW_BLUR_SHIFT);
}

static inline unsigned draw_command_transform_codepoint(
    const DrawCommand *command, unsigned codepoint, bool word_start)
{
    TextTransformMode mode = draw_command_text_transform(command);
    if ((mode == TEXT_TRANSFORM_UPPERCASE
         || (mode == TEXT_TRANSFORM_CAPITALIZE && word_start))
        && codepoint >= 'a' && codepoint <= 'z') {
        return codepoint - 'a' + 'A';
    }
    if (mode == TEXT_TRANSFORM_LOWERCASE
        && codepoint >= 'A' && codepoint <= 'Z') {
        return codepoint - 'A' + 'a';
    }
    return codepoint;
}

static inline int draw_command_text_font_size_fixed(
    const DrawCommand *command)
{
    if (command == NULL) return 0;
    int64_t fixed = (int64_t) command->font_size * 64
        + (command->type == DRAW_TEXT
           ? (command->radius & LAYOUT_TEXT_FONT_SIZE_FRACTION_MASK)
             >> LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT : 0);
    return fixed > INT32_MAX ? INT32_MAX
           : (fixed < INT32_MIN ? INT32_MIN : (int) fixed);
}

static inline void draw_command_set_text_x_fixed(DrawCommand *command,
                                                  int fixed)
{
    if (command == NULL) return;
    int64_t wide = fixed;
    command->x = wide >= 0 ? (int) (wide / 64)
                 : (int) (-((-wide + 63) / 64));
    unsigned fraction = (unsigned) (wide - (int64_t) command->x * 64) & 63u;
    command->radius = (command->radius & ~LAYOUT_TEXT_X_FRACTION_MASK)
                      | (int) fraction;
}

static inline void draw_command_set_text_font_size_fixed(
    DrawCommand *command, int fixed)
{
    if (command == NULL) return;
    if (fixed < 0) fixed = 0;
    command->font_size = fixed / 64;
    command->radius =
        (command->radius & ~LAYOUT_TEXT_FONT_SIZE_FRACTION_MASK)
        | ((fixed & 63) << LAYOUT_TEXT_FONT_SIZE_FRACTION_SHIFT);
}

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(DrawCommand) == 56,
               "32-bit DrawCommand layout changed for PSP display lists");
#elif UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(DrawCommand) == 64,
               "64-bit DrawCommand host layout changed");
#else
#error "Unsupported pointer width for DrawCommand layout"
#endif

typedef struct {
    size_t command_start;
    size_t command_end;
    int origin_y;
    int top;
} StickyRange;

typedef struct {
    size_t command_start;
    size_t command_end;
    size_t link_start;
    size_t link_end;
    size_t control_start;
    size_t control_end;
    int origin_y;
    int height;
    int inset;
    bool from_bottom;
} FixedRange;

typedef struct {
    const char *url;
    lxb_dom_node_t *node;
    int x;
    int y;
    int width;
    int height;
    int z_index;
    uint32_t url_length;
} LinkRegion;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(LinkRegion) == 32,
               "32-bit LinkRegion layout changed for PSP hit testing");
#else
_Static_assert(sizeof(LinkRegion) == 40,
               "64-bit LinkRegion host layout changed");
#endif

typedef enum {
    CONTROL_INPUT,
    CONTROL_TEXTAREA,
    CONTROL_EDITABLE,
    CONTROL_BUTTON,
    CONTROL_SELECT,
    CONTROL_TOGGLE,
    CONTROL_RANGE,
    CONTROL_RESIZE
} ControlType;

typedef struct {
    int x;
    int y;
    int width;
    int height;
    ControlType type;
    lxb_dom_node_t *node;
    int z_index;
} ControlRegion;

typedef struct {
    lxb_dom_node_t *node;
    int x;
    int y;
    int width;
    int height;
    int client_width;
    int client_height;
    int content_width;
    int content_height;
    int scroll_x;
    int scroll_y;
    uint32_t command_start;
    uint32_t command_end;
    uint32_t scroll_command_start;
    uint32_t scroll_command_end;
    uint16_t clip_radius;
    uint8_t clips_x : 1;
    uint8_t clips_y : 1;
    uint8_t clip_only_x : 1;
    uint8_t clip_only_y : 1;
    uint8_t cssom_geometry_authoritative : 1;
    uint8_t clip_flags_reserved : 3;
    /* Zero for normal-flow boxes. For positioned boxes this is the bounded
       DOM-ancestor distance to their containing block; 255 denotes the
       initial containing block. It reuses trailing structure padding on
       both PSP and host builds. */
    uint8_t positioned_ancestor_distance;
    /* The overflow clip edge is the padding box, not the border box.  These
       bounded offsets occupy bytes recovered by packing the boolean flags,
       so LayoutNodeBox remains 68 bytes on PSP. */
    uint8_t clip_inset_left;
    uint8_t clip_inset_top;
} LayoutNodeBox;

#define LAYOUT_SCROLL_CONTAINER_LIMIT 128u
#define LAYOUT_SCROLL_SNAP_CANDIDATE_LIMIT 256u

typedef enum {
    LAYOUT_SCROLLBAR_AUTO = 0,
    LAYOUT_SCROLLBAR_THIN,
    LAYOUT_SCROLLBAR_NONE
} LayoutScrollbarWidth;

typedef enum {
    LAYOUT_CURSOR_AUTO = 0,
    LAYOUT_CURSOR_POINTER,
    LAYOUT_CURSOR_TEXT,
    LAYOUT_CURSOR_CROSSHAIR,
    LAYOUT_CURSOR_MOVE,
    LAYOUT_CURSOR_WAIT,
    LAYOUT_CURSOR_NOT_ALLOWED,
    LAYOUT_CURSOR_RESIZE_HORIZONTAL,
    LAYOUT_CURSOR_RESIZE_VERTICAL,
    LAYOUT_CURSOR_HIDDEN
} LayoutCursor;

typedef struct {
    uint32_t node_box_index;
    int16_t padding_top;
    int16_t padding_right;
    int16_t padding_bottom;
    int16_t padding_left;
    uint16_t thumb_rgb565;
    uint16_t track_rgb565;
    /* bits 0..1 axis (x/y), bit 2 mandatory, bit 3 smooth, bits 4..5
       overscroll-x, bits 6..7 overscroll-y, bits 8..9 scrollbar width. */
    uint16_t flags;
} LayoutScrollContainer;

typedef struct {
    uint32_t node_box_index;
    uint16_t container_index;
    int16_t margin_top;
    int16_t margin_right;
    int16_t margin_bottom;
    int16_t margin_left;
    uint8_t align_x;
    uint8_t align_y;
    uint8_t stop_always;
    uint8_t reserved;
} LayoutScrollSnapCandidate;

#define LAYOUT_CLIP_RADIUS_MASK UINT16_C(0x03ff)
#define LAYOUT_CLIP_MARGIN_SHIFT 10
#define LAYOUT_CLIP_MARGIN_MASK UINT16_C(0xfc00)

static inline unsigned layout_node_box_clip_radius(
    const LayoutNodeBox *box)
{
    return box == NULL ? 0 : box->clip_radius & LAYOUT_CLIP_RADIUS_MASK;
}

static inline unsigned layout_node_box_clip_margin(
    const LayoutNodeBox *box)
{
    return box == NULL ? 0
        : (box->clip_radius & LAYOUT_CLIP_MARGIN_MASK)
          >> LAYOUT_CLIP_MARGIN_SHIFT;
}

static inline StyleOverflowClipBox layout_node_box_clip_box(
    const LayoutNodeBox *box)
{
    return box == NULL ? STYLE_OVERFLOW_CLIP_PADDING_BOX
        : (StyleOverflowClipBox) (box->clip_flags_reserved & 3u);
}

static inline void layout_node_box_set_clip_geometry(
    LayoutNodeBox *box, unsigned radius, unsigned margin,
    StyleOverflowClipBox clip_box)
{
    if (box == NULL) return;
    if (radius > LAYOUT_CLIP_RADIUS_MASK) radius = LAYOUT_CLIP_RADIUS_MASK;
    if (margin > 63u) margin = 63u;
    box->clip_radius = (uint16_t) (
        radius | (margin << LAYOUT_CLIP_MARGIN_SHIFT));
    box->clip_flags_reserved = (uint8_t) clip_box & 3u;
}

typedef struct LayoutNodeIndexEntry LayoutNodeIndexEntry;
typedef struct LayoutFocusIndexEntry LayoutFocusIndexEntry;
typedef struct LayoutReuseCache LayoutReuseCache;

typedef struct {
    size_t retained_bytes;
    size_t style_hits;
    size_t style_misses;
    size_t intrinsic_hits;
    size_t intrinsic_misses;
    size_t table_row_hits;
    size_t table_row_misses;
    size_t scoped_invalidations;
    size_t full_resets;
} LayoutReuseStats;

typedef struct {
    uint64_t total_us;
    uint64_t root_style_us;
    uint64_t flow_us;
    uint64_t compact_us;
    uint64_t focus_index_us;
    uint64_t paint_order_us;
    uint64_t spatial_index_us;
    uint64_t finalize_us;
    uint64_t style_resolutions;
    uint64_t style_cache_hits;
    uint64_t style_cache_misses;
    uint64_t style_resolve_us;
    uint64_t style_rule_queries;
    uint64_t style_rule_candidates;
    uint64_t style_variable_lookups;
    uint64_t style_variable_rule_candidates;
    uint64_t style_variable_cache_hits;
    uint64_t style_variable_cache_misses;
    uint64_t style_variable_cache_negative_hits;
    uint64_t style_variable_cache_evictions;
    size_t style_variable_cache_bytes;
    uint64_t style_deferred_rule_applications;
    uint64_t style_deferred_rule_us;
    uint64_t intrinsic_width_visits;
    uint64_t intrinsic_min_visits;
    uint64_t intrinsic_cache_hits;
    uint64_t intrinsic_cache_misses;
    uint64_t intrinsic_paired_text_measurements;
    uint64_t margin_collapse_visits;
    uint64_t margin_cache_hits;
    uint64_t margin_cache_misses;
    uint64_t flat_iterator_passes;
    uint64_t flat_iterator_yields;
    uint64_t flex_iterator_passes;
    uint64_t flex_iterator_yields;
    uint64_t flex_basis_requests;
    uint64_t flex_minimum_requests;
    uint64_t float_band_queries;
    uint64_t float_exclusion_probes;
    size_t resumable_phases;
    size_t resumable_passes;
    uint64_t maximum_resumable_phase_us;
} LayoutPerformance;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(LayoutNodeBox) == 68,
               "32-bit LayoutNodeBox layout changed for PSP indices");
#else
_Static_assert(sizeof(LayoutNodeBox) == 72,
               "64-bit LayoutNodeBox host layout changed");
#endif

typedef struct {
    Budget *budget;
    DrawCommand *commands;
    size_t count;
    size_t capacity;
    LinkRegion *links;
    size_t link_count;
    size_t link_capacity;
    ControlRegion *controls;
    size_t control_count;
    size_t control_capacity;
    /*
     * Build-only [link start/end, control start/end] ranges parallel to
     * node_boxes. Flex/grid translations use them to touch only one item's
     * interactions; compact_layout_storage releases them before retention.
     */
    uint32_t *node_interaction_ranges;
    size_t node_interaction_capacity;
    /* Lazily allocated, fixed-capacity arena for runtime-expanded generated
       content and list markers referenced by retained draw commands. */
    char *generated_text_storage;
    size_t generated_text_used;
    int width;
    int scroll_width;
    int height;
    ViewportContext viewport;
    uint32_t page_background;
    /* True when this layout encountered an external image, mask, or
       background image whose decoded pixels were not available.  A bounded
       provisional layout can use this aggregate bit to avoid presenting an
       incomplete above-the-fold shell without retaining another flag on
       every node (which would be disproportionately expensive on PSP). */
    bool unresolved_external_visuals;
    bool visual_priority_overflow;
    ImagePriorityTarget *visual_priority_targets;
    size_t visual_priority_count;
    size_t visual_priority_capacity;
    /* Bounded, deduplicated gradient ramps referenced by fill commands.
       Inline rather than heap-grown: a page of buttons sharing one ramp
       costs a single slot, and a hostile page that exceeds the cap simply
       stops getting new ramps (its fills paint their solid colour) instead
       of demanding unbounded storage. */
    StyleGradient gradients[LAYOUT_GRADIENT_LIMIT];
    size_t gradient_count;
    const FontSet *fonts;
    const WebFontSet *web_fonts;
    StickyRange *sticky_ranges;
    size_t sticky_count;
    size_t sticky_capacity;
    FixedRange *fixed_ranges;
    size_t fixed_count;
    size_t fixed_capacity;
    LayoutNodeBox *node_boxes;
    size_t node_box_count;
    size_t node_box_capacity;
    /* Sparse retained scroll metadata. Ordinary pages allocate neither
       table; hostile pages degrade after deterministic hard caps. */
    LayoutScrollContainer *scroll_containers;
    size_t scroll_container_count;
    LayoutScrollSnapCandidate *scroll_snap_candidates;
    size_t scroll_snap_candidate_count;
    bool scroll_metadata_truncated;
    /* Probe-only packed horizontal/vertical padding sums. Allocated only
       when container geometry is being collected, rather than growing every
       PSP LayoutNodeBox. */
    uint32_t *node_container_padding_sums;
    size_t node_container_padding_capacity;
    bool retain_container_padding;
    /* Lazily allocated only when an overflow clip has asymmetric circular
       corners. Zero entries use LayoutNodeBox's compact uniform radius. */
    int32_t *node_clip_radius_codes;
    /* Cached TILEFINCH_TRACE_* environment state, resolved once per build in
       layout_build_context so hot paths never call getenv().  Zero/NULL when
       tracing is compiled out (TILEFINCH_NO_TRACE) or the variables are unset. */
    uint32_t trace_flags;
    const char *trace_flex_class;
    const char *trace_layout_class;
    const char *trace_pseudo_class;
    const char *trace_range_class;
    LayoutNodeIndexEntry *node_index;
    size_t node_index_capacity;
    size_t node_index_count;
    bool node_index_growth_disabled;
    LayoutFocusIndexEntry *focus_index;
    size_t focus_index_capacity;
    uint32_t *paint_order;
    size_t paint_order_count;
    uint8_t *command_flags;
    size_t *spatial_band_offsets;
    uint32_t *spatial_band_orders;
    size_t spatial_band_count;
    size_t spatial_band_order_count;
    uint32_t *spatial_global_orders;
    size_t spatial_global_count;
    uint32_t *overflow_orders;
    size_t overflow_order_count;
    /* Authored after an intersecting overflow command. These remain outside
       cached tiles so the final overlay pass can preserve exact paint order. */
    uint32_t *late_positioned_orders;
    size_t late_positioned_order_count;
    uint64_t max_work_slice_us;
    /* Final retained-text fingerprint. Find-in-page uses it to refresh only
       match geometry after animation relayouts whose text is unchanged. */
    uint64_t text_fingerprint;
    size_t max_work_slice_units;
    size_t layout_work_units;
    size_t cooperative_yields;
    lxb_dom_node_t *max_work_slice_node;
    LayoutPerformance performance;
} LayoutDocument;

bool layout_build(LayoutDocument *layout, Budget *budget,
                  const PocDocument *document, const Stylesheet *stylesheet,
                  const FontSet *fonts, const ImageResources *images,
                  int viewport_width);
bool layout_build_context(LayoutDocument *layout, Budget *budget,
                          const PocDocument *document,
                          const Stylesheet *stylesheet,
                          const FontSet *fonts,
                          const ImageResources *images,
                          const ViewportContext *viewport);
bool layout_build_context_reuse(LayoutDocument *layout, Budget *budget,
                                const PocDocument *document,
                                const Stylesheet *stylesheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                const ViewportContext *viewport,
                                LayoutReuseCache *reuse);
/* Builds an ephemeral top-of-document display list through y_limit CSS
   pixels. A true truncated result is suitable for first paint only; callers
   must subsequently build the authoritative full layout. The same bounded
   reuse cache may be passed to both builds so preview style/intrinsic work is
   useful during completion rather than duplicated. */
bool layout_build_context_preview(LayoutDocument *layout, Budget *budget,
                                  const PocDocument *document,
                                  const Stylesheet *stylesheet,
                                  const FontSet *fonts,
                                  const ImageResources *images,
                                  const ViewportContext *viewport,
                                  LayoutReuseCache *reuse, int y_limit,
                                  bool *truncated);

/* A transactional, resumable authoritative layout build.  The job owns its
   in-progress display list and transient selector/layout caches; callers see
   no partial LayoutDocument.  Each pump advances at most one externally
   resumable phase.  Recursive block/flex/grid/table formatting contexts are
   currently atomic within the flow phase, but retain the ordinary bounded
   cooperate checkpoints while they run.  The document, stylesheet, fonts,
   images, viewport, and optional reuse cache are borrowed and must remain
   immutable and alive until the job is destroyed. */
typedef struct LayoutBuildJob LayoutBuildJob;

typedef enum {
    LAYOUT_BUILD_PENDING = 0,
    LAYOUT_BUILD_COMPLETE,
    LAYOUT_BUILD_FAILED,
    LAYOUT_BUILD_CANCELLED
} LayoutBuildStatus;

LayoutBuildJob *layout_build_job_begin(
    Budget *budget, const PocDocument *document,
    const Stylesheet *stylesheet, const FontSet *fonts,
    const ImageResources *images, const ViewportContext *viewport,
    LayoutReuseCache *reuse);
LayoutBuildStatus layout_build_job_pump(LayoutBuildJob *job);
/* Moves a completed result into `layout`.  `layout` must be empty; the job
   remains destroyable after the move. */
bool layout_build_job_take(LayoutBuildJob *job, LayoutDocument *layout);
void layout_build_job_cancel(LayoutBuildJob *job);
void layout_build_job_destroy(LayoutBuildJob *job);
LayoutReuseCache *layout_reuse_cache_create(Budget *budget);
void layout_reuse_cache_destroy(LayoutReuseCache *cache);
void layout_reuse_cache_reset(LayoutReuseCache *cache);
/* Bind retained styles to one immutable stylesheet/font/viewport state.
   Image discovery uses the same binding before resolving nodes, allowing its
   complete document walk to seed the subsequent authoritative layout. */
void layout_reuse_cache_prepare(LayoutReuseCache *cache,
                                const Stylesheet *sheet,
                                const FontSet *fonts,
                                const ImageResources *images,
                                int viewport_width);
/* Returns true on a retained hit. On a miss, resolves the canonical
   layout-safe style (including ch metrics and focus-neutral outlines), stores
   it when a cache is present, and always writes result. */
bool layout_reuse_cache_resolve_style(LayoutReuseCache *cache,
                                      const Stylesheet *sheet,
                                      const FontSet *fonts,
                                      lxb_dom_node_t *node,
                                      const ComputedStyle *parent,
                                      ComputedStyle *result);
/* Shared inheritance seed for the html/body root. Resource discovery must
   use the same seed when it hands computed styles to layout. */
ComputedStyle layout_initial_root_style(void);
/* Safe only while node and cached nodes belong to a live, non-destructively
   mutated DOM. Tree replacement/removal callers must reset the cache. */
void layout_reuse_cache_invalidate_node(LayoutReuseCache *cache,
                                        lxb_dom_node_t *node,
                                        bool text_or_structure_sensitive);
/* Mutation journal proved that this change cannot affect a :has() selector;
   invalidate the ordinary subtree/sizing scope without discarding the cache
   merely because unrelated relational rules exist elsewhere in the sheet. */
void layout_reuse_cache_invalidate_node_scoped(
    LayoutReuseCache *cache, lxb_dom_node_t *node,
    bool text_or_structure_sensitive);
/* Focus/focus-within can change styles on the focused node and any ancestor.
   Retain the cache allocation and dependency metadata, but discard entries
   whose focus-state assumptions are no longer valid. */
void layout_reuse_cache_invalidate_focus(
    LayoutReuseCache *cache, lxb_dom_node_t *node);
/* Computed CSS is independent of decoded image resources. Preserve it when
   provisional layout hands off to an image-complete authoritative pass,
   while invalidating only intrinsic/table measurements whose subtrees gained
   an image. */
void layout_reuse_cache_update_images(LayoutReuseCache *cache,
                                      const ImageResources *images);
/* Rebinds an unchanged stylesheet after transactional struct ownership
   moves. Content rebuilds must use the ordinary invalidation path. */
void layout_reuse_cache_rebind_stylesheet(
    LayoutReuseCache *cache, const Stylesheet *previous,
    const Stylesheet *replacement);
void layout_reuse_cache_stats(const LayoutReuseCache *cache,
                              LayoutReuseStats *stats);
bool layout_reuse_cache_can_reuse_mutations(const LayoutReuseCache *cache);
bool layout_build_viewport(LayoutDocument *layout, Budget *budget,
                           const PocDocument *document,
                           const Stylesheet *stylesheet,
                           const FontSet *fonts,
                           const ImageResources *images,
                           int css_viewport_width,
                           int css_viewport_height,
                           int device_width, int device_height);
bool layout_clone_visual(LayoutDocument *visual,
                         const LayoutDocument *source);
int layout_css_to_visual(const LayoutDocument *layout, int value);
int layout_visual_to_css(const LayoutDocument *layout, int value);
void layout_destroy(LayoutDocument *layout);
const LinkRegion *layout_link_at(const LayoutDocument *layout, int x, int y);
const ControlRegion *layout_control_at(const LayoutDocument *layout,
                                       int x, int y);
const LayoutNodeBox *layout_box_for_node(const LayoutDocument *layout,
                                         const lxb_dom_node_t *node);
bool layout_focus_for_node(const LayoutDocument *layout,
                           const lxb_dom_node_t *node,
                           bool *control, size_t *index);
/*
 * Recolour the one rounded-border display-list command retained for node.
 * This is deliberately narrower than arbitrary paint mutation: callers must
 * first pass style_focus_change_classify(), and dry_run lets a multi-node
 * focus transition prove every patch before changing any command.
 */
bool layout_apply_focus_border_paint(
    LayoutDocument *layout, const Stylesheet *stylesheet,
    lxb_dom_node_t *node, const ComputedStyle *style, bool dry_run,
    int *left, int *top, int *right, int *bottom);
bool layout_scroll_node(LayoutDocument *layout, lxb_dom_node_t *node,
                        int scroll_x, int scroll_y);
bool layout_build_scroll_metadata(LayoutDocument *layout,
                                  const Stylesheet *stylesheet);
/* Applies a delta to the deepest scroll container containing/focusing node,
   then chains through ancestors according to overscroll-behavior. */
bool layout_scroll_node_chain(LayoutDocument *layout, lxb_dom_node_t *node,
                              int delta_x, int delta_y,
                              int *remaining_x, int *remaining_y);
/* Snaps every affected axis of node's nearest scroll container after input
   settles. The candidate scan is capped by
   LAYOUT_SCROLL_SNAP_CANDIDATE_LIMIT. */
bool layout_scroll_node_settle(LayoutDocument *layout, lxb_dom_node_t *node);
bool layout_scroll_node_reveal(LayoutDocument *layout,
                               lxb_dom_node_t *node);
LayoutCursor layout_cursor_for_node(const Stylesheet *stylesheet,
                                    lxb_dom_node_t *node);
LayoutScrollbarWidth layout_root_scrollbar_width(
    const LayoutDocument *layout, const Stylesheet *stylesheet);
void layout_transfer_scroll_state(const LayoutDocument *previous,
                                  LayoutDocument *replacement);
bool layout_positioned_command_escapes_clip(
    const LayoutDocument *layout, size_t command_index,
    const LayoutNodeBox *clip_box);
int layout_node_box_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box);
int layout_node_box_effective_clip_radius_code(
    const LayoutDocument *layout, const LayoutNodeBox *box);

#endif
